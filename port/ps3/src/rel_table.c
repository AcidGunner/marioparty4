/* Overlays (REL) are linked statically on this port. omDLLLink() asks here for the entry points of the
 * overlay named by the disc path ("dll/bootdll.rel"). The list of overlays comes from rel_list.h, which the
 * Makefile generates from PORT_RELS; each REL(x) needs rel_x__prolog / rel_x__epilog from prefix_rel.sh. */
#include <dolphin.h>
#include <string.h>
#include <strings.h>

typedef struct { const char *name; s32 (*prolog)(void); void (*epilog)(void); } PortRel;

#define REL(n) extern s32 rel_##n##__prolog(void); extern void rel_##n##__epilog(void);
#include "rel_list.h"
#undef REL
#define REL(n) { #n, rel_##n##__prolog, rel_##n##__epilog },
static const PortRel port_rels[] = {
#include "rel_list.h"
    { 0, 0, 0 }
};
#undef REL

int port_rel_lookup(const char *path, s32 (**prolog)(void), void (**epilog)(void))
{
    char base[64];
    const char *p = strrchr(path, '/');
    const PortRel *r;
    size_t n;
    p = p ? p + 1 : path;
    n = strlen(p);
    if (n > 4 && strcasecmp(p + n - 4, ".rel") == 0) n -= 4;
    if (n >= sizeof(base)) return 0;
    memcpy(base, p, n);
    base[n] = 0;
    for (r = port_rels; r->name; r++) {
        if (strcasecmp(r->name, base) == 0) { *prolog = r->prolog; *epilog = r->epilog; return 1; }
    }
    return 0;
}
