/* Mimics the game's process scheduler (process.c) against the fiber layer. */
#include <dolphin.h>
#include <stdio.h>
#include <string.h>
#include "game/jmp.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

typedef struct { jmp_buf jump; int ticks, ended, killed; } Proc;
static Proc procs[3];
static int cur;
static jmp_buf sched;
static int trace[64], ntrace;
static int dead_count;

static void yield(Proc *p) { if (!gcsetjmp(&p->jump)) gclongjmp(&sched, 1); }
void HuPrcEnd(void) { procs[cur].ended = 1; dead_count++; gclongjmp(&sched, 2); }

static void body(void)
{
    Proc *p = &procs[cur];
    int id = cur;
    for (;;) { trace[ntrace++] = id * 10 + p->ticks; p->ticks++; yield(p); }
}

static int round_, local_;

int main(void)
{
    int i, ret;
    for (i = 0; i < 3; i++) {
        gcsetjmp(&procs[i].jump);           /* like HuPrcCreate */
        procs[i].jump.lr = (u32)(uintptr_t)body;
        procs[i].jump.sp = 0x1000 + i;      /* any nonzero: "fresh" */
    }
    cur = 0;
    ret = gcsetjmp(&sched);                 /* like HuPrcCall: loop lives after the setjmp */
    for (;;) {
        if (ret == 2) { local_ += 100; }
        if (ret == 1 || ret == 2) { cur++; if (cur == 3) { cur = 0; } }
        if (round_ >= 10) break;
        if (round_ == 7 && !procs[1].killed) { procs[1].killed = 1; procs[1].jump.lr = (u32)(uintptr_t)HuPrcEnd; }
        round_++;
        if (procs[cur].ended) { ret = 1; continue; }
        gclongjmp(&procs[cur].jump, 1);     /* resume or start */
    }
    printf("trace:"); for (i = 0; i < ntrace; i++) printf(" %d", trace[i]); printf("\n");
    CHECK(ntrace >= 6);
    CHECK(trace[0] == 0 && trace[1] == 10 && trace[2] == 20 && trace[3] == 1 && trace[4] == 11 && trace[5] == 21);
    CHECK(procs[1].ended == 1 && dead_count == 1);
    CHECK(local_ == 100);
    printf(fails ? "fiber_test: %d FAILED\n" : "fiber_test: all passed\n", fails);
    return fails != 0;
}
