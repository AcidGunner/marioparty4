#ifndef _GAME_HSFLOAD_H
#define _GAME_HSFLOAD_H

#include "game/hsfformat.h"

HSFDATA *LoadHSF(void *data);
void ClusterAdjustObject(HSFDATA *model, HSFDATA *src_model);
char *SetName(u32 *str_ofs);
#ifdef TARGET_PC
/* Models are native structs separate from the file buffer; this frees both. */
void HuHsfFree(HSFDATA *hsf);
#else
#define HuHsfFree(hsf) HuMemDirectFree(hsf)
#endif
char *MakeObjectName(char *name);
s32 CmpObjectName(char *name1, char *name2);

#endif
