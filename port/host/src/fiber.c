/* Cooperative contexts for the game's process scheduler, built on threads with baton passing.
 *
 * Exactly one fiber (thread) runs at a time. A context is identified by a jmp_buf whose `owner` is
 * the fiber that last called gcsetjmp on it. gclongjmp to a context owned by another fiber wakes that
 * fiber and parks the caller; the woken fiber then performs a real (same-thread) __builtin_longjmp to
 * the gcsetjmp site, so gcsetjmp "returns twice" exactly like setjmp. */
#include <dolphin.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "game/jmp.h"
#include "fiber_os.h"

extern void HuPrcEnd(void);

#define PORT_FIBER_STACK (256 * 1024)

typedef struct PortFiber {
    u64 sem;                 /* OS semaphore handle */
    int is_main;
    int resumable;           /* 1 between a gcsetjmp and the wake-up that follows */
    jmp_buf *wake_target;    /* where to longjmp when woken (NULL = run call_fn) */
    void (*call_fn)(void);   /* redirect request */
    void (*start_fn)(void);  /* first function for a new fiber */
} PortFiber;

static PortFiber main_fiber;
static PortFiber *cur;
static unsigned long switch_count;

PortFiber *port_fiber_self(void)
{
    if (!cur) {
        main_fiber.sem = port_os_sem_create();
        main_fiber.is_main = 1;
        cur = &main_fiber;
    }
    return cur;
}

void port_fiber_prepare(jmp_buf *j)
{
    PortFiber *me = port_fiber_self();
    j->lr = 0;
    j->sp = 0;
    j->owner = me;
    me->resumable = 1;
}

/* Hand the baton to `to` and give it up ourselves. Returns when someone wakes us again,
 * or never if we are a finished fiber (its thread exits). */
static void switch_to(PortFiber *to)
{
    PortFiber *me = cur;
    switch_count++;
    cur = to;
    port_os_sem_post(to->sem);
    if (!me->resumable && !me->is_main) {
        port_os_thread_exit(); /* nobody can resume this context any more */
    }
    port_os_sem_wait(me->sem);
    /* we were woken: cur == me, set by the waker */
    me->resumable = 0;
    if (me->call_fn) {
        void (*fn)(void) = me->call_fn;
        me->call_fn = 0;
        fn();
        HuPrcEnd(); /* a redirected context that returns is finished */
        abort();
    }
    {
        jmp_buf *t = me->wake_target;
        me->wake_target = 0;
        __builtin_longjmp(t->native, 1);
    }
}

static void fiber_entry(void *arg)
{
    PortFiber *me = (PortFiber *)arg;
    port_os_sem_wait(me->sem); /* wait for the first baton */
    me->resumable = 0;
    me->start_fn();
    HuPrcEnd(); /* process function returned: end the process */
    abort();
}

void port_gclongjmp(jmp_buf *t, s32 status)
{
    PortFiber *me = port_fiber_self();
    if (t->lr) {
        void (*fn)(void) = (void (*)(void))(uintptr_t)t->lr;
        int fresh = t->sp != 0;
        t->lr = 0;
        t->sp = 0;
        if (fresh) {
            PortFiber *f = (PortFiber *)calloc(1, sizeof(PortFiber));
            f->sem = port_os_sem_create();
            f->start_fn = fn;
            port_os_thread_start(fiber_entry, f, PORT_FIBER_STACK);
            switch_to(f);
        } else {
            PortFiber *o = t->owner;
            if (!o) { fprintf(stderr, "port: gclongjmp redirect to context with no owner\n"); abort(); }
            if (o == me) { fn(); HuPrcEnd(); abort(); }
            o->call_fn = fn;
            switch_to(o);
        }
        return; /* resumed through a gcsetjmp site, not here; kept for the compiler */
    }
    if (!t->owner) { fprintf(stderr, "port: gclongjmp to a context that was never set\n"); abort(); }
    t->status = status;
    if (t->owner == me) {
        __builtin_longjmp(t->native, 1);
    }
    t->owner->wake_target = t;
    switch_to(t->owner);
}

unsigned long port_fiber_switches(void) { return switch_count; }
