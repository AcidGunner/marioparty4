/* Shadows include/game/jmp.h for host / PS3 builds (found first via -Iport/host/include).
 *
 * The original gcsetjmp/gclongjmp are PPC32 asm that save registers and *replace the stack pointer*,
 * which is how the game runs each "process" on its own stack. Here every process runs on its own
 * thread instead, and only one thread runs at a time (baton passing), so the game's cooperative
 * scheduler works unchanged. See port/host/src/fiber.c.
 *
 * Game-visible contract kept:
 *   - gcsetjmp(&j)        returns 0, and again with `status` when someone gclongjmp()s to j
 *   - gclongjmp(&j, st)   jumps to a context set by gcsetjmp, possibly owned by another process
 *   - j.lr = func; j.sp = stack   then  gclongjmp(&j, 1)   starts func in a new context
 *   - j.lr = func (only)          then  gclongjmp(&j, 1)   makes a parked context call func instead
 */
#ifndef _GAME_JMP_H
#define _GAME_JMP_H

#include "dolphin.h"

struct PortFiber;

typedef struct jump_buf {
    u32 lr;          /* start/redirect request: function address (0 = none) */
    u32 cr;
    u32 sp;          /* nonzero together with lr = "start a fresh context" */
    u32 r2;
    u32 pad;
    u32 regs[19];
    double flt_regs[19];
    /* host/PS3 additions */
    void *native[8]; /* __builtin_setjmp buffer */
    struct PortFiber *owner;
    s32 status;
} jmp_buf;

struct PortFiber *port_fiber_self(void);
void port_fiber_prepare(jmp_buf *j);
void port_gclongjmp(jmp_buf *j, s32 status);

/* Must stay a macro: setjmp has to be called from the frame that survives. */
#define gcsetjmp(j) (port_fiber_prepare(j), __builtin_setjmp((j)->native) ? (j)->status : 0)
#define gclongjmp(j, s) port_gclongjmp((j), (s))

#endif
