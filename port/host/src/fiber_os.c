#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "fiber_os.h"

#ifndef PORT_PS3
/* ---- host: POSIX threads + semaphores ---- */
#include <pthread.h>
#include <semaphore.h>

unsigned long long port_os_sem_create(void)
{
    sem_t *s = (sem_t *)malloc(sizeof(sem_t));
    sem_init(s, 0, 0);
    return (unsigned long long)(size_t)s;
}
void port_os_sem_wait(unsigned long long h) { while (sem_wait((sem_t *)(size_t)h) != 0) {} }
void port_os_sem_post(unsigned long long h) { sem_post((sem_t *)(size_t)h); }

typedef struct { void (*entry)(void *); void *arg; } StartInfo;
static void *thread_main(void *p)
{
    StartInfo si = *(StartInfo *)p;
    free(p);
    si.entry(si.arg);
    return 0;
}
void port_os_thread_start(void (*entry)(void *), void *arg, size_t stack_size)
{
    pthread_t t;
    pthread_attr_t a;
    StartInfo *si = (StartInfo *)malloc(sizeof(*si));
    si->entry = entry; si->arg = arg;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, stack_size);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &a, thread_main, si) != 0) { fprintf(stderr, "port: pthread_create failed\n"); abort(); }
    pthread_attr_destroy(&a);
}
void port_os_thread_exit(void) { pthread_exit(0); }

#else
/* ---- PS3 (PSL1GHT): LV2 PPU threads + semaphores.
 * UNTESTED: written from memory of <sys/thread.h> / <sys/sem.h>; field and macro names may need adjusting. */
#include <sys/thread.h>
#include <sys/sem.h>

unsigned long long port_os_sem_create(void)
{
    sys_sem_t sem;
    sys_sem_attr_t attr;
    memset(&attr, 0, sizeof(attr));
    attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
    attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
    strncpy(attr.name, "fib", sizeof(attr.name) - 1);
    if (sysSemCreate(&sem, &attr, 0, 0x7fffffff) != 0) { fprintf(stderr, "port: sysSemCreate failed\n"); abort(); }
    return (unsigned long long)sem;
}
void port_os_sem_wait(unsigned long long h) { sysSemWait((sys_sem_t)h, 0); }
void port_os_sem_post(unsigned long long h) { sysSemPost((sys_sem_t)h, 1); }

void port_os_thread_start(void (*entry)(void *), void *arg, size_t stack_size)
{
    sys_ppu_thread_t t;
    if (sysThreadCreate(&t, entry, arg, 1000, stack_size, 0, "mp4proc") != 0) {
        fprintf(stderr, "port: sysThreadCreate failed\n");
        abort();
    }
}
void port_os_thread_exit(void) { sysThreadExit(0); }
#endif
