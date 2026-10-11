/* Static replacement for src/REL/executor.c. The original walks _ctors/_dtors, which are linker-script
 * symbols of the Metrowerks REL format and do not exist here; the game is plain C so there are no ctors. */
#include <dolphin/types.h>

extern void ObjectSetup(void);

s32 _prolog(void)
{
    ObjectSetup();
    return 0;
}

void _epilog(void) {}
