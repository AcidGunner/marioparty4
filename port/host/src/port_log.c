/* Tiny call-site registry so logging stubs can report which SDK functions the
 * game really hits (and how often) when the process exits. */
#include <stdio.h>
#include <stdlib.h>
#include "port_log.h"

static PortStubSite *sites;
static int atexit_set;

static void report(void)
{
    unsigned long total = 0;
    int n = 0;
    PortStubSite *s;
    for (s = sites; s; s = s->next) { total += s->count; n++; }
    fprintf(stderr, "\n[port] %d stubbed SDK functions were hit (%lu calls):\n", n, total);
    for (s = sites; s; s = s->next) fprintf(stderr, "[port]   %-32s %lu\n", s->name, s->count);
}

void port_stub_hit(PortStubSite *s)
{
    if (s->count++ == 0) {
        s->next = sites;
        sites = s;
        if (!atexit_set) { atexit_set = 1; atexit(report); }
#ifdef PORT_PS3
        /* no environment on a console: always trace, so a crash shows how far startup got */
        fprintf(stderr, "[port] first call: %s\n", s->name);
#else
        if (getenv("PORT_TRACE_FIRST")) fprintf(stderr, "[port] first call: %s\n", s->name);
#endif
    }
}
