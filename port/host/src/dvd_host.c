/* File-backed replacement for the DVD layer (host and PS3).
 *
 * The game addresses files by disc path ("data/board.bin", "/meminfo.bin", "dll/m401Dll.rel").
 * We map those onto a directory that holds the contents of the disc's `files/` tree
 * (what dtk / Dolphin's "Extract Files" produce: data/, dll/, thp/ ... and meminfo.bin).
 * All reads complete synchronously; "async" reads call their callback before returning. */
#define _GNU_SOURCE
#include <dolphin.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "port_log.h"

#define PORT_MAX_ENTRIES 1024
#define PORT_PATH_MAX 512

static char disc_root[PORT_PATH_MAX];
static int disc_root_ready;
static char *entry_path[PORT_MAX_ENTRIES]; /* absolute host path per entry number */
static u32 entry_size[PORT_MAX_ENTRIES];
static int entry_count;

/* Candidate locations, tried in order. The first that contains the marker file wins. */
static const char *const candidate_roots[] = {
    "/dev_usb000/MP4/",
    "/dev_usb001/MP4/",
    "/dev_hdd0/MP4/",
    "/dev_hdd0/game/MP4PORT01/USRDIR/disc/",
    "./gamedata/",
    "./disc/",
    0,
};
#define PORT_MARKER "data/board.bin"

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void find_disc_root(void)
{
    const char *env = getenv("PORT_DISC_ROOT");
    int i;
    char probe[PORT_PATH_MAX];
    if (disc_root_ready) return;
    disc_root_ready = 1;
    if (env && *env) {
        snprintf(disc_root, sizeof(disc_root), "%s%s", env, env[strlen(env) - 1] == '/' ? "" : "/");
        fprintf(stderr, "port: disc root from PORT_DISC_ROOT: %s\n", disc_root);
        return;
    }
    for (i = 0; candidate_roots[i]; i++) {
        snprintf(probe, sizeof(probe), "%s%s", candidate_roots[i], PORT_MARKER);
        if (file_exists(probe)) {
            snprintf(disc_root, sizeof(disc_root), "%s", candidate_roots[i]);
            fprintf(stderr, "port: disc root: %s\n", disc_root);
            return;
        }
    }
    fprintf(stderr, "port: no disc root found (looked for %s under:", PORT_MARKER);
    for (i = 0; candidate_roots[i]; i++) fprintf(stderr, " %s", candidate_roots[i]);
    fprintf(stderr, ")\n");
}

/* Resolve `rel` under disc_root, matching each path component case-insensitively if needed. */
static int resolve_path(const char *rel, char *out, size_t outsz)
{
    char cur[PORT_PATH_MAX];
    char comp[256];
    const char *p = rel;
    size_t n;
    while (*p == '/') p++;
    snprintf(cur, sizeof(cur), "%s", disc_root);
    n = strlen(cur);
    if (n > 1 && cur[n - 1] == '/') cur[n - 1] = 0;
    while (*p) {
        size_t len = strcspn(p, "/");
        char next[PORT_PATH_MAX];
        struct stat st;
        if (len == 0) { p++; continue; }
        if (len >= sizeof(comp)) return 0;
        memcpy(comp, p, len); comp[len] = 0;
        snprintf(next, sizeof(next), "%s/%s", cur, comp);
        if (stat(next, &st) != 0) {
            DIR *d = opendir(cur);
            struct dirent *e;
            int found = 0;
            if (!d) return 0;
            while ((e = readdir(d))) {
                if (strcasecmp(e->d_name, comp) == 0) {
                    snprintf(next, sizeof(next), "%s/%s", cur, e->d_name);
                    found = 1;
                    break;
                }
            }
            closedir(d);
            if (!found) return 0;
        }
        snprintf(cur, sizeof(cur), "%s", next);
        p += len;
        while (*p == '/') p++;
    }
    snprintf(out, outsz, "%s", cur);
    return 1;
}

s32 DVDConvertPathToEntrynum(char *path)
{
    char abs[PORT_PATH_MAX];
    struct stat st;
    int i;
    find_disc_root();
    if (!disc_root[0] || !path || !resolve_path(path, abs, sizeof(abs))) return -1;
    if (stat(abs, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    for (i = 0; i < entry_count; i++)
        if (strcmp(entry_path[i], abs) == 0) return i;
    if (entry_count >= PORT_MAX_ENTRIES) return -1;
    entry_path[entry_count] = strdup(abs);
    entry_size[entry_count] = (u32)st.st_size;
    return entry_count++;
}

BOOL DVDFastOpen(s32 entry, DVDFileInfo *fi)
{
    if (entry < 0 || entry >= entry_count) return FALSE;
    memset(fi, 0, sizeof(*fi));
    fi->startAddr = (u32)entry;
    fi->length = entry_size[entry];
    fi->cb.state = DVD_STATE_END;
    return TRUE;
}

BOOL DVDOpen(char *path, DVDFileInfo *fi)
{
    s32 e = DVDConvertPathToEntrynum(path);
    if (e < 0) {
        fprintf(stderr, "port: DVDOpen: not found: %s\n", path ? path : "(null)");
        return FALSE;
    }
    return DVDFastOpen(e, fi);
}

BOOL DVDClose(DVDFileInfo *fi) { return TRUE; }

/* Reads up to `length` bytes at `offset`; anything past EOF is zero-filled (the game rounds sizes up to 32 B). */
static s32 port_read(DVDFileInfo *fi, void *addr, s32 length, s32 offset)
{
    FILE *f;
    size_t got = 0;
    if (fi->startAddr >= (u32)entry_count) return -1;
    f = fopen(entry_path[fi->startAddr], "rb");
    if (!f) return -1;
    if (fseek(f, offset, SEEK_SET) == 0) got = fread(addr, 1, (size_t)length, f);
    fclose(f);
    if (got < (size_t)length) memset((char *)addr + got, 0, (size_t)length - got);
    return length;
}

BOOL DVDReadPrio(DVDFileInfo *fi, void *addr, s32 length, s32 offset, s32 prio)
{
    return port_read(fi, addr, length, offset) >= 0 ? length : -1;
}

BOOL DVDReadAsyncPrio(DVDFileInfo *fi, void *addr, s32 length, s32 offset, DVDCallback cb, s32 prio)
{
    s32 r = port_read(fi, addr, length, offset);
    fi->cb.state = DVD_STATE_END;
    if (cb) cb(r, fi);
    return r >= 0;
}

s32 DVDGetCommandBlockStatus(const DVDCommandBlock *b) { return b->state; }
s32 DVDGetDriveStatus(void) { return DVD_STATE_END; }
void DVDInit(void) { find_disc_root(); }
s32 DVDCancel(DVDCommandBlock *b) { return 0; }
