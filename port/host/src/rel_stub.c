/* Host build has no statically linked overlays yet; the PS3 build overrides this with port/ps3/src/rel_table.c. */
#include <dolphin.h>
__attribute__((weak)) int port_rel_lookup(const char *path, s32 (**prolog)(void), void (**epilog)(void)) { return 0; }
