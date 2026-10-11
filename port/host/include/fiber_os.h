#ifndef FIBER_OS_H
#define FIBER_OS_H
#include <stddef.h>
/* Tiny OS abstraction for fiber.c: counting semaphores and detached threads. */
unsigned long long port_os_sem_create(void);
void port_os_sem_wait(unsigned long long sem);
void port_os_sem_post(unsigned long long sem);
void port_os_thread_start(void (*entry)(void *), void *arg, size_t stack_size);
void port_os_thread_exit(void);
#endif
