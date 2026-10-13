/* Hand-written host implementations of the Dolphin SDK pieces that need real
 * behaviour (time, memory arena, vsync, pad). Everything else the game calls
 * lives in sdk_stubs_generated.c as a logging no-op. */
#define _GNU_SOURCE
#include <dolphin.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#ifdef PORT_PS3
#include <sys/time.h>
#include <unistd.h>
#else
#include <sys/mman.h>
#endif
#include "port_log.h"
#include "msm/msmsys.h"

volatile OSHeapHandle __OSCurrHeap = -1;

/* ---- time: GameCube timebase = bus clock / 4 = 40.5 MHz ---- */
#define PORT_TIMER_CLOCK 40500000ull

OSTime OSGetTime(void)
{
#ifdef PORT_PS3
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (OSTime)((u64)tv.tv_sec * PORT_TIMER_CLOCK + (u64)tv.tv_usec * PORT_TIMER_CLOCK / 1000000ull);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (OSTime)((u64)ts.tv_sec * PORT_TIMER_CLOCK + (u64)ts.tv_nsec * PORT_TIMER_CLOCK / 1000000000ull);
#endif
}

OSTick OSGetTick(void) { return (OSTick)OSGetTime(); }

/* ---- logging ---- */
void OSReport(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

__attribute__((weak)) void OSPanic(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "\n*** OSPanic %s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}

/* ---- interrupts / caches: nothing to do on a host ---- */
BOOL OSDisableInterrupts(void) { return FALSE; }
BOOL OSEnableInterrupts(void) { return FALSE; }
BOOL OSRestoreInterrupts(BOOL level) { return level; }
void DCFlushRange(void *a, u32 n) {}
void DCFlushRangeNoSync(void *a, u32 n) {}
void DCInvalidateRange(void *a, u32 n) {}
void DCStoreRange(void *a, u32 n) {}
void DCStoreRangeNoSync(void *a, u32 n) {}
void LCEnable(void) {}
void LCDisable(void) {}
void PPCSync(void) {}

/* ---- memory arena ----
 * The game carves its heaps out of [ArenaLo, ArenaHi). It also stores pointers
 * in u32 fields in places, so keep the arena below 4 GB (MAP_32BIT) while we
 * are still on a 64-bit host. */
#define PORT_ARENA_SIZE (64u * 1024 * 1024)
static void *arena_lo, *arena_hi;

static void arena_init(void)
{
    if (arena_lo) return;
    setvbuf(stderr, 0, _IONBF, 0); /* never lose the last line before a crash */
#ifdef PORT_PS3
    /* No mmap in PSL1GHT: take it from the heap, 32-byte aligned. */
    void *raw = malloc(PORT_ARENA_SIZE + 32);
    if (!raw) { fprintf(stderr, "port: arena malloc failed\n"); abort(); }
    void *p = (void *)(((uintptr_t)raw + 31) & ~(uintptr_t)31);
#else
    void *p = mmap(0, PORT_ARENA_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) { fprintf(stderr, "port: arena mmap failed\n"); abort(); }
#endif
    if ((uintptr_t)p >> 32)
        fprintf(stderr, "port: WARNING arena at %p is above 4 GB; u32-stored pointers will break\n", p);
    fprintf(stderr, "port: sizeof(void*)=%u arena=%p\n", (unsigned)sizeof(void *), p);
#ifdef PORT_PS3
    /* Touch every page now: if the console can't really back this much memory we want to die here, with a clear last line. */
    fprintf(stderr, "port: touching arena...\n");
    memset(p, 0, PORT_ARENA_SIZE);
    fprintf(stderr, "port: arena ok\n");
#endif
    arena_lo = p;
    arena_hi = (char *)p + PORT_ARENA_SIZE;
}

void *OSGetArenaLo(void) { arena_init(); return arena_lo; }
void *OSGetArenaHi(void) { arena_init(); return arena_hi; }
void OSSetArenaLo(void *p) { arena_init(); arena_lo = p; }
void OSSetArenaHi(void *p) { arena_init(); arena_hi = p; }
u32 OSGetPhysicalMemSize(void) { return 0x01800000; }
u32 OSGetConsoleSimulatedMemSize(void) { return 0x01800000; }
u32 OSGetConsoleType(void) { return 0x20000002; } /* retail HW, rev 2 */
void OSInit(void)
{
    /* On GameCube the OS fills these low-memory words at boot. Game code derives OS_TIMER_CLOCK from them
       (OSTicksToMilliseconds etc.); left at 0 those macros divide by zero and timed waits never finish. */
    __OSBusClock = 162000000u;  /* 162 MHz bus  -> 40.5 MHz timebase, matching PORT_TIMER_CLOCK */
    __OSCoreClock = 486000000u; /* 486 MHz core */
    fprintf(stderr, "port: __OSBusClock=%u OS_TIMER_CLOCK=%u\n", (unsigned)__OSBusClock, (unsigned)(__OSBusClock / 4));
    arena_init();
}

/* ---- video: pace the game at 60 Hz ---- */
static u32 retrace_count;

void VIInit(void) {}
extern int port_gx_present(void);

/* The game polls its pads from a vertical-retrace callback (HuPadInit -> VISetPostRetraceCallback(PadReadVSync)).
 * On hardware that fires from an interrupt; here it is called once per frame from VIWaitForRetrace.
 * The pre-retrace callback (HuDvdErrDispIntFunc) only wakes the DVD-error/soft-reset thread, which does not exist on
 * this port, so it is remembered but not fired. */
static VIRetraceCallback port_pre_retrace_cb, port_post_retrace_cb;

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback old = port_pre_retrace_cb;
    port_pre_retrace_cb = callback;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
    VIRetraceCallback old = port_post_retrace_cb;
    port_post_retrace_cb = callback;
    return old;
}

void VIWaitForRetrace(void)
{
    /* If a new frame was handed to the video layer, its (vsynced) flip already paced us; otherwise sleep one frame. */
    if (!port_gx_present()) {
#ifdef PORT_PS3
        usleep(16667);
#else
        struct timespec ts = { 0, 16666667 };
        nanosleep(&ts, 0);
#endif
    }
    retrace_count++;
    if (port_post_retrace_cb) port_post_retrace_cb(retrace_count);
}
u32 VIGetRetraceCount(void) { return retrace_count; }
u32 VIGetNextField(void) { return retrace_count & 1; }

/* ---- pad: the PS3 build has a real backend (port/ps3/src/pad_ps3.c); other builds report no controller ---- */
void SISetSamplingRate(u32 msecs) {}

#ifndef PORT_PS3
BOOL PADInit(void) { return TRUE; }
BOOL PADReset(u32 mask) { return TRUE; }
void PADSetSpec(u32 spec) {}
void PADControlMotor(s32 chan, u32 cmd) {}

u32 PADRead(PADStatus *status)
{
    int i;
    memset(status, 0, sizeof(PADStatus) * PAD_CHANMAX);
    for (i = 0; i < PAD_CHANMAX; i++) status[i].err = PAD_ERR_NO_CONTROLLER;
    return 0;
}

void PADClamp(PADStatus *status) {}
#endif

/* ---- standard GX render modes (from the Dolphin SDK tables) ---- */
#define GX_SP { {6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6},{6,6} }
GXRenderModeObj GXNtsc480IntDf = { VI_TVMODE_NTSC_INT, 640, 480, 480, 40, 0, 640, 480, VI_XFBMODE_DF, 0, 0, GX_SP, { 8, 8, 10, 12, 10, 8, 8 } };
GXRenderModeObj GXNtsc480Prog  = { VI_TVMODE_NTSC_PROG, 640, 480, 480, 40, 0, 640, 480, VI_XFBMODE_SF, 0, 0, GX_SP, { 0, 0, 21, 22, 21, 0, 0 } };
GXRenderModeObj GXMpal480IntDf = { VI_TVMODE_MPAL_INT, 640, 480, 480, 40, 0, 640, 480, VI_XFBMODE_DF, 0, 0, GX_SP, { 8, 8, 10, 12, 10, 8, 8 } };
GXRenderModeObj GXPal528IntDf  = { VI_TVMODE_PAL_INT, 640, 528, 528, 40, 23, 640, 528, VI_XFBMODE_DF, 0, 0, GX_SP, { 8, 8, 10, 12, 10, 8, 8 } };

/* ---- functions game code calls without a header prototype ---- */
void GXResetWriteGatherPipe(void) {}
s32 msmSysLoadGroup(s32 group, void *buf, s32 flag) { PORT_STUB("msmSysLoadGroup"); return 0; }

void __GXAbortWaitPECopyDone(void) { PORT_STUB("__GXAbortWaitPECopyDone"); }

/* ---- OS heaps: bump allocators over caller-supplied regions ----
 * The game calls OSCreateHeap once on the arena and then runs its own HuMem
 * heaps on top, so no free list is needed to boot. */
#define PORT_MAX_HEAPS 16
static struct { char *cur, *end; int used; } port_heaps[PORT_MAX_HEAPS];

void *OSInitAlloc(void *lo, void *hi, int maxHeaps)
{
    memset(port_heaps, 0, sizeof(port_heaps));
    __OSCurrHeap = -1;
    return lo;
}

OSHeapHandle OSCreateHeap(void *start, void *end)
{
    int i;
    for (i = 0; i < PORT_MAX_HEAPS; i++) {
        if (!port_heaps[i].used) {
            port_heaps[i].cur = (char *)(((uintptr_t)start + 31) & ~(uintptr_t)31);
            port_heaps[i].end = (char *)((uintptr_t)end & ~(uintptr_t)31);
            port_heaps[i].used = 1;
            return i;
        }
    }
    return -1;
}

OSHeapHandle OSSetCurrentHeap(OSHeapHandle h)
{
    OSHeapHandle old = __OSCurrHeap;
    __OSCurrHeap = h;
    return old;
}

void *OSAllocFromHeap(OSHeapHandle h, u32 size)
{
    if (h < 0 || h >= PORT_MAX_HEAPS || !port_heaps[h].used) return 0;
    size = (size + 31) & ~31u;
    if ((size_t)(port_heaps[h].end - port_heaps[h].cur) < size) return 0;
    void *p = port_heaps[h].cur;
    port_heaps[h].cur += size;
    return p;
}

void OSFreeToHeap(OSHeapHandle h, void *p) {}

/* Returns the bytes still free in the heap (the game uses this to size its last heap), -1 if invalid. */
long OSCheckHeap(OSHeapHandle h)
{
    if (h < 0 || h >= PORT_MAX_HEAPS || !port_heaps[h].used) return -1;
    return (long)(port_heaps[h].end - port_heaps[h].cur);
}

u32 OSGetSoundMode(void) { return 1; } /* OS_SOUND_MODE_STEREO */
