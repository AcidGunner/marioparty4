/* ARAM emulation. The GameCube has 16 MB of auxiliary RAM reached only through DMA; the game keeps data
 * directories there (src/game/armem.c) and waits for ARQ completion callbacks. Here ARAM is a plain malloc'd
 * buffer and every transfer is a synchronous memcpy that fires its callback before returning, so code that
 * spins on the callback (HuAR_DVDtoARAM: while (HuARDMACheck());) makes progress.
 * Main-memory addresses reach us as u32; that is valid because the arena and .data sit below 4 GB. */
#include <dolphin.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PORT_ARAM_SIZE 0x1000000u /* 16 MB */
#define PORT_ARAM_BASE 0x4000u    /* first usable ARAM address on hardware */

static u8 *aram;
static int aram_inited;
static ARCallback dma_callback;

static void aram_copy(u32 type, u32 mmem, u32 aaddr, u32 len)
{
    if ((u64)aaddr + len > PORT_ARAM_SIZE) {
        OSReport("aram: transfer out of range (aram %08x len %08x)\n", aaddr, len);
        return;
    }
    if (type == 0) { /* MRAM -> ARAM */
        memcpy(aram + aaddr, (void *)(uintptr_t)mmem, len);
    } else {         /* ARAM -> MRAM */
        memcpy((void *)(uintptr_t)mmem, aram + aaddr, len);
    }
}

u32 ARInit(u32 *stack_index_addr, u32 num_entries)
{
    if (!aram_inited) {
        aram = (u8 *)calloc(1, PORT_ARAM_SIZE);
        if (!aram) {
            OSReport("aram: cannot allocate %u bytes for ARAM\n", PORT_ARAM_SIZE);
            OSPanic("aram_host.c", __LINE__, "ARAM alloc failed\n");
        }
        aram_inited = 1;
    }
    return PORT_ARAM_BASE;
}

BOOL ARCheckInit(void) { return aram_inited ? TRUE : FALSE; }
u32 ARGetSize(void) { return PORT_ARAM_SIZE; }
u32 ARGetBaseAddress(void) { return PORT_ARAM_BASE; }
void ARSetSize(void) {}
void ARQInit(void) {}

void ARQPostRequest(ARQRequest *task, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length, ARQCallback callback)
{
    task->next = NULL;
    task->owner = owner;
    task->type = type;
    task->priority = priority;
    task->source = source;
    task->dest = dest;
    task->length = length;
    task->callback = callback;
    if (type == 0) aram_copy(0, source, dest, length);   /* MRAM -> ARAM */
    else           aram_copy(1, dest, source, length);   /* ARAM -> MRAM */
    if (callback) callback((u32)(uintptr_t)task);
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    aram_copy(type, mainmem_addr, aram_addr, length);
    if (dma_callback) dma_callback();
}

u32 ARGetDMAStatus(void) { return 0; }

ARCallback ARRegisterDMACallback(ARCallback callback)
{
    ARCallback old = dma_callback;
    dma_callback = callback;
    return old;
}
