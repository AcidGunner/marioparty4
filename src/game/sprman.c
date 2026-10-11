#include "game/sprite.h"
#include "game/disp.h"
#include "game/memory.h"
#include "game/init.h"

#include "dolphin/mtx.h"

#ifndef __MWERKS__
#include "game/hu3d.h"
#endif

#define SPRITE_DIRTY_ATTR 0x1
#define SPRITE_DIRTY_XFORM 0x2
#define SPRITE_DIRTY_COLOR 0x4

typedef struct sprite_order {
    u16 group;
    u16 sprite;
    u16 prio;
    u16 next;
} SpriteOrder;

HUSPRITE HuSprData[HUSPR_MAX];
HUSPRGRP HuSprGrpData[HUSPR_GRP_MAX];
static SpriteOrder HuSprOrder[HUSPR_MAX*2];

static s16 HuSprOrderNum;
static s16 HuSprOrderNo;
static BOOL HuSprPauseF;

static void HuSprOrderEntry(s16 group, s16 sprite);


void HuSprInit(void)
{
    s16 i;
    HUSPRITE *sprite;
    HUSPRGRP *group;
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        sprite->data = NULL;
    }
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        group->capacity = 0;
    }
    sprite = &HuSprData[0];
    sprite->prio = 0;
    sprite->data = (void *)1;
    HuSprPauseF = FALSE;
}

void HuSprClose(void)
{
    s16 i;
    HUSPRGRP *group;
    HUSPRITE *sprite;
    
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity != 0) {
            HuSprGrpKill(i);
        }
    }
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(sprite->data) {
            HuSprKill(i);
        }
    }
    HuSprPauseF = FALSE;
}

void HuSprExec(s16 draw_no)
{
    HUSPRITE *sprite;
    while(sprite = HuSprCall()) {
        if(!(sprite->attr & HUSPR_ATTR_DISPOFF) && sprite->drawNo == draw_no) {
            HuSprDisp(sprite);
        }
    }
}

void HuSprBegin(void)
{
    Mtx temp, rot;
    s16 i, j;
    Vec axis = {0, 0, 1};
    HUSPRGRP *group;
    group = HuSprGrpData;
    HuSprOrderNum = 1;
    HuSprOrder[0].next = 0;
    HuSprOrder[0].prio = -1;
    for(i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity != 0) {
            MTXTrans(temp, group->center.x*group->scale.x, group->center.y*group->scale.y, 0.0f);
            MTXRotAxisDeg(rot, &axis, group->zRot);
            MTXConcat(rot, temp, group->mtx);
            MTXScale(temp, group->scale.x, group->scale.y, 1.0f);
            MTXConcat(group->mtx, temp, group->mtx);
            mtxTransCat(group->mtx, group->pos.x, group->pos.y, 0);
            for(j=0; j<group->capacity; j++) {
                if(group->members[j] != -1) {
                    HuSprOrderEntry(i, group->members[j]);
                }
            }
        }
    }
    HuSprOrderNo = 0;
}

static void HuSprOrderEntry(s16 group, s16 sprite)
{
    SpriteOrder *order = &HuSprOrder[HuSprOrderNum];
    s16 prio = HuSprData[sprite].prio;
    s16 prev, next;
    if(HuSprOrderNum >= HUSPR_MAX*2) {
        OSReport("Order Max Over!\n");
        return;
    }
    next = HuSprOrder[0].next;
    for(prev = 0; next != 0; prev = next, next = HuSprOrder[next].next) {
        if(HuSprOrder[next].prio < prio) {
            break;
        }
    }
    order->next = HuSprOrder[prev].next;
    HuSprOrder[prev].next = HuSprOrderNum;
    order->prio = prio;
    order->group = group;
    order->sprite = sprite;
    HuSprOrderNum++;
}

HUSPRITE *HuSprCall(void)
{
    HuSprOrderNo = HuSprOrder[HuSprOrderNo].next;
    if(HuSprOrderNo != 0) {
        SpriteOrder *order = &HuSprOrder[HuSprOrderNo];
        HUSPRITE *sprite = &HuSprData[order->sprite];
        sprite->groupMtx = &HuSprGrpData[order->group].mtx;
        if(sprite->attr & HUSPR_ATTR_FUNC) {
            return sprite;
        }
        sprite->frameP = &sprite->data->bank[sprite->bank].frame[sprite->animNo];
        sprite->patP = &sprite->data->pat[sprite->frameP->pat];
        return sprite;
    } else {
        return NULL;
    }
}

static inline void SpriteCalcFrame(HUSPRITE *sprite, ANIMBANK *bank, ANIMFRAME **frame, s16 loop)
{
    if(sprite->time >= (*frame)->time) {
        sprite->animNo++;
        sprite->time -= (*frame)->time;
        if(sprite->animNo >= bank->timeNum || (*frame)[1].time == -1) {
            if(loop) {
                sprite->animNo = 0;
            } else {
                sprite->animNo = bank->timeNum-1;
            }
        }
        *frame = &bank->frame[sprite->animNo];
    } else if(sprite->time < 0) {
        sprite->animNo--;
        if(sprite->animNo < 0) {
            if(loop) {
                sprite->animNo = bank->timeNum-1;
            } else {
                sprite->animNo = 0;
            }
        }
        *frame = &bank->frame[sprite->animNo];
        sprite->time += (*frame)->time;
    }
}

void HuSprFinish(void)
{
    ANIMDATA *anim;
    ANIMBANK *bank;
    ANIMFRAME *frame;
    HUSPRITE *sprite;
    s16 i;
    s16 j;
    s16 loop;
    s16 dir;
    
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(sprite->data && !(sprite->attr & HUSPR_ATTR_FUNC)) {
            if(!HuSprPauseF || (sprite->attr & HUSPR_ATTR_NOPAUSE)) {
                anim = sprite->data;
                bank = &anim->bank[sprite->bank];
                frame = &bank->frame[sprite->animNo];
                loop = (sprite->attr & HUSPR_ATTR_LOOP) ? 0 : 1;
                if(!(sprite->attr & HUSPR_ATTR_NOANIM)) {
                    dir = (sprite->attr & HUSPR_ATTR_REVERSE) ? -1 : 1;
                    for(j=0; j<(s32)sprite->speed*minimumVcount; j++) {
                        sprite->time += dir;
                        SpriteCalcFrame(sprite, bank, &frame, loop);
                    }
                    sprite->time += (sprite->speed*(float)minimumVcount)-j;
                    SpriteCalcFrame(sprite, bank, &frame, loop);
                }
                sprite->dirty = 0;
            }
        }
    }
}

void HuSprPauseSet(BOOL value)
{
    HuSprPauseF = value;
}

#ifdef TARGET_PC
/* Disc/embedded animation blobs use the GameCube layout: big-endian, 4-byte pointers (stored as offsets).
 * ANIMDATA/ANIMBANK/ANIMPAT/ANIMBMP hold real pointers, so on a 64-bit build they are a different size and
 * cannot be fixed up in place. Instead the blob is parsed field by field into native structs.
 * ANIMFRAME/ANIMLAYER contain only s16/u8, so they stay in the blob (NOTE: on a little-endian host their
 * s16 fields would still need byte-swapping; the PS3 is big-endian so it does not).
 * An in-place marker records the conversion: the blob's bank-offset slot (+8) is overwritten with the
 * 32-bit address of the native ANIMDATA (the arena and .data are below 4 GB). */
#include <stdint.h>
#include <string.h>

static u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 rd16be(const u8 *p) { return (u16)(((u32)p[0] << 8) | p[1]); }
static void wr32be(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

ANIMDATA *HuSprAnimRead(void *data)
{
    u8 *raw = (u8 *)data;
    ANIMDATA *anim;
    ANIMBANK *bank;
    ANIMPAT *pat;
    ANIMBMP *bmp;
    s16 i, bankNum, patNum, bmpNum;
    u32 w, nativeHi;
    const u8 *b, *p, *m;

    /* Already a native ANIMDATA (HuSprAnimMake or an earlier read)? Its 64-bit `bank` pointer's high word is 0;
       a raw blob never has 0 there (the header occupies the first 0x14 bytes). */
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    nativeHi = *(u32 *)(raw + 8);
#else
    nativeHi = *(u32 *)(raw + 12);
#endif
    if (nativeHi == 0) {
        ((ANIMDATA *)data)->useNum++;
        return (ANIMDATA *)data;
    }
    w = rd32be(raw + 8);
    if (w & 0xFFFF0000) { /* blob already converted: marker is the native struct's address */
        anim = (ANIMDATA *)(uintptr_t)w;
        anim->useNum++;
        return anim;
    }

    bankNum = rd16be(raw + 0);
    patNum = rd16be(raw + 2);
    bmpNum = rd16be(raw + 4);
    anim = (ANIMDATA *)HuMemDirectMalloc(HEAP_MODEL, sizeof(ANIMDATA) + bankNum * sizeof(ANIMBANK)
                                         + patNum * sizeof(ANIMPAT) + bmpNum * sizeof(ANIMBMP));
    if ((uintptr_t)anim >> 32) {
        OSReport("sprman: ANIMDATA at %p is above 4 GB, cannot mark blob\n", (void *)anim);
        OSPanic("sprman.c", __LINE__, "anim above 4GB\n");
    }
    memset(anim, 0, sizeof(ANIMDATA) + bankNum * sizeof(ANIMBANK) + patNum * sizeof(ANIMPAT) + bmpNum * sizeof(ANIMBMP));
    bank = (ANIMBANK *)(anim + 1);
    pat = (ANIMPAT *)(bank + bankNum);
    bmp = (ANIMBMP *)(pat + patNum);
    anim->bankNum = bankNum;
    anim->patNum = patNum;
    anim->bmpNum = bmpNum;
    anim->useNum = 0;
    anim->bank = bank;
    anim->pat = pat;
    anim->bmp = bmp;
    anim->port_src = raw;

    b = raw + rd32be(raw + 8);   /* GC ANIMBANK: s16 timeNum, s16 unk, u32 frame  (8 bytes)  */
    p = raw + rd32be(raw + 0xC); /* GC ANIMPAT: 5 x s16, pad, u32 layer           (0x10 bytes) */
    m = raw + rd32be(raw + 0x10);/* GC ANIMBMP: u8,u8,s16,s16,s16,u32,u32,u32     (0x14 bytes) */
    for (i = 0; i < bankNum; i++, b += 8) {
        bank[i].timeNum = rd16be(b);
        bank[i].unk = rd16be(b + 2);
        bank[i].frame = (ANIMFRAME *)(raw + rd32be(b + 4));
    }
    for (i = 0; i < patNum; i++, p += 0x10) {
        pat[i].layerNum = rd16be(p);
        pat[i].centerX = rd16be(p + 2);
        pat[i].centerY = rd16be(p + 4);
        pat[i].sizeX = rd16be(p + 6);
        pat[i].sizeY = rd16be(p + 8);
        pat[i].layer = (ANIMLAYER *)(raw + rd32be(p + 0xC));
    }
    for (i = 0; i < bmpNum; i++, m += 0x14) {
        bmp[i].pixSize = m[0];
        bmp[i].dataFmt = m[1];
        bmp[i].palNum = rd16be(m + 2);
        bmp[i].sizeX = rd16be(m + 4);
        bmp[i].sizeY = rd16be(m + 6);
        bmp[i].dataSize = rd32be(m + 8);
        bmp[i].palData = raw + rd32be(m + 0xC);
        bmp[i].data = raw + rd32be(m + 0x10);
    }
    wr32be(raw + 8, (u32)(uintptr_t)anim); /* in-place "already converted" marker */
    return anim;
}

void HuSprAnimFree(ANIMDATA *anim)
{
    if (anim->port_src) {
        HuMemDirectFree(anim->port_src);
    }
    HuMemDirectFree(anim);
}
#else
ANIMDATA *HuSprAnimRead(void *data)
{
    s16 i;
    ANIMBMP *bmp;
    ANIMBANK *bank;
    ANIMPAT *pat;
    
    ANIMDATA *anim = (ANIMDATA *)data;
    if((u32)anim->bank & 0xFFFF0000) {
        anim->useNum++;
        return anim;
    }
    bank = (ANIMBANK *)((u32)anim->bank+(u32)data);
    anim->bank = bank;
    pat = (ANIMPAT *)((u32)anim->pat+(u32)data);
    anim->pat = pat;
    bmp = (ANIMBMP *)((u32)anim->bmp+(u32)data);
    anim->bmp = bmp;
    for(i=0; i<anim->bankNum; i++, bank++) {
        bank->frame = (ANIMFRAME *)((u32)bank->frame+(u32)data);
    }
    for(i=0; i<anim->patNum; i++, pat++) {
        pat->layer = (ANIMLAYER *)((u32)pat->layer+(u32)data);
    }
    for(i=0; i<anim->bmpNum; i++, bmp++) {
        bmp->palData = (void *)((u32)bmp->palData+(u32)data);
        bmp->data = (void *)((u32)bmp->data+(u32)data);
    }
    anim->useNum = 0;
    return anim;
}
#endif

void HuSprAnimLock(ANIMDATA *anim)
{
    anim->useNum++;
}

s16 HuSprCreate(ANIMDATA *anim, s16 prio, s16 bank)
{
    HUSPRITE *sprite;
    s16 i;
    for(sprite = &HuSprData[1], i=1; i<HUSPR_MAX; i++, sprite++) {
        if(!sprite->data) {
            break;
        }
    }
    if(i == HUSPR_MAX) {
        return HUSPR_NONE;
    }
    sprite->data = anim;
    sprite->speed = 1.0f;
    sprite->animNo = 0;
    sprite->bank = bank;
    sprite->time = 0.0f;
    sprite->attr = 0;
    sprite->drawNo = 0;
    sprite->r = sprite->g = sprite->b = sprite->a = 255;
    sprite->pos.x = sprite->pos.y = sprite->zRot = 0.0f;
    sprite->prio = prio;
    sprite->scale.x = sprite->scale.y = 1.0f;
    sprite->wrapS = sprite->wrapT = GX_CLAMP;
    sprite->uvScaleX = sprite->uvScaleY = 1;
    sprite->bg = NULL;
    sprite->scissorX = sprite->scissorY = 0;
    sprite->scissorW = 640;
    sprite->scissorH = 480;
    if(anim) {
        HuSprAnimLock(anim);
    }
    return i;
}

s16 HuSprFuncCreate(HUSPRFUNC func, s16 prio)
{
    HUSPRITE *sprite;
    s16 index = HuSprCreate(NULL, prio, 0);
    if(index == HUSPR_NONE) {
        return HUSPR_NONE;
    }
    sprite = &HuSprData[index];
    sprite->func = func;
    sprite->attr |= HUSPR_ATTR_FUNC;
    return index;
}

s16 HuSprGrpCreate(s16 capacity)
{
    HUSPRGRP *group;
    s16 i, j;
    for(group = HuSprGrpData, i=0; i<HUSPR_GRP_MAX; i++, group++) {
        if(group->capacity == 0) {
            break;
        }
    }
    if(i == HUSPR_GRP_MAX) {
        return HUSPR_GRP_NONE;
    }
    group->members = HuMemDirectMalloc(HEAP_HEAP, sizeof(s16)*capacity);
    for(j=0; j<capacity; j++) {
        group->members[j] = HUSPR_NONE;
    }
    group->capacity = capacity;
    group->pos.x = group->pos.y = group->zRot = group->center.x = group->center.y = 0.0f;
    group->scale.x = group->scale.y = 1.0f;
    return i;
}

s16 HuSprGrpCopy(s16 group)
{
    HUSPRGRP *new_group_ptr;
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 new_group = HuSprGrpCreate(group_ptr->capacity);
    s16 i;
    if(new_group == HUSPR_GRP_NONE) {
        return HUSPR_GRP_NONE;
    }
    new_group_ptr = &HuSprGrpData[new_group];
    new_group_ptr->pos.x = group_ptr->pos.x;
    new_group_ptr->pos.y = group_ptr->pos.y;
    new_group_ptr->zRot = group_ptr->zRot;
    new_group_ptr->scale.x = group_ptr->scale.x;
    new_group_ptr->scale.y = group_ptr->scale.y;
    new_group_ptr->center.x = group_ptr->center.x;
    new_group_ptr->center.y = group_ptr->center.y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HUSPRITE *old_sprite = &HuSprData[group_ptr->members[i]];
            s16 new_sprite = HuSprCreate(old_sprite->data, old_sprite->prio, old_sprite->bank);
            HuSprData[new_sprite] = *old_sprite;
            HuSprGrpMemberSet(new_group, i, new_sprite);
        }
    }
    return new_group;
}

void HuSprGrpMemberSet(s16 group, s16 member, s16 sprite)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] != HUSPR_NONE) {
        return;
    }
    group_ptr->members[member] = sprite;
}

void HuSprGrpMemberKill(s16 group, s16 member)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    HuSprKill(group_ptr->members[member]);
    group_ptr->members[member] = HUSPR_NONE;
}

void HuSprGrpKill(s16 group)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprKill(group_ptr->members[i]);
        }
    }
    group_ptr->capacity = 0;
    HuMemDirectFree(group_ptr->members);
}

void HuSprKill(s16 sprite)
{
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    if(!sprite_ptr->data) {
        return;
    }
    if(!(sprite_ptr->attr & HUSPR_ATTR_FUNC)) {
        HuSprAnimKill(sprite_ptr->data);
        if(sprite_ptr->bg) {
            HuSprAnimKill(sprite_ptr->bg);
            sprite_ptr->bg = NULL;
        }
    }
    sprite_ptr->data = NULL;
}

void HuSprAnimKill(ANIMDATA *anim)
{
    if(--anim->useNum <= 0) {
        if(anim->bmpNum & ANIM_BMP_ALLOC) {
            if(anim->bmp->data) {
                HuMemDirectFree(anim->bmp->data);
            }
            if(anim->bmp->palData) {
                HuMemDirectFree(anim->bmp->palData);
            }
        }
        HuSprAnimFree(anim);
    }
}

void HuSprAttrSet(s16 group, s16 member, s32 attr)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr;
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    sprite_ptr = &HuSprData[group_ptr->members[member]];
    sprite_ptr->attr |= attr;
    sprite_ptr->dirty |= SPRITE_DIRTY_ATTR;
}

void HuSprAttrReset(s16 group, s16 member, s32 attr)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HUSPRITE *sprite_ptr;
    if(group_ptr->capacity == 0 || group_ptr->capacity <= member || group_ptr->members[member] == HUSPR_NONE) {
        return;
    }
    sprite_ptr = &HuSprData[group_ptr->members[member]];
    sprite_ptr->attr &= ~attr;
    sprite_ptr->dirty |= SPRITE_DIRTY_ATTR;
}

void HuSprPosSet(s16 group, s16 member, float x, float y)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->pos.x = x;
    sprite_ptr->pos.y = y;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprZRotSet(s16 group, s16 member, float z_rot)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->zRot = z_rot;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprScaleSet(s16 group, s16 member, float x, float y)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->scale.x = x;
    sprite_ptr->scale.y = y;
    sprite_ptr->dirty |= SPRITE_DIRTY_XFORM;
}

void HuSprTPLvlSet(s16 group, s16 member, float tp_lvl)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->a = tp_lvl*255;
    sprite_ptr->dirty |= SPRITE_DIRTY_COLOR;
}

void HuSprColorSet(s16 group, s16 member, u8 r, u8 g, u8 b)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->r = r;
    sprite_ptr->g = g;
    sprite_ptr->b = b;
    sprite_ptr->dirty |= SPRITE_DIRTY_COLOR;
}

void HuSprSpeedSet(s16 group, s16 member, float speed)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    HuSprData[group_ptr->members[member]].speed = speed;
}

void HuSprBankSet(s16 group, s16 member, s16 bank)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    ANIMDATA *anim = sprite_ptr->data;
    ANIMBANK *bank_ptr = &anim->bank[sprite_ptr->bank];
    ANIMFRAME *frame_ptr = &bank_ptr->frame[sprite_ptr->animNo];
    sprite_ptr->bank = bank;
    if(sprite_ptr->attr & HUSPR_ATTR_REVERSE) {
        sprite_ptr->animNo = bank_ptr->timeNum-1;
        frame_ptr = &bank_ptr->frame[sprite_ptr->animNo];
        sprite_ptr->time = frame_ptr->time;
    } else {
        sprite_ptr->time = 0;
        sprite_ptr->animNo = 0;
    }
}

void HuSprGrpPosSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->pos.x = x;
    group_ptr->pos.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != -1) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpCenterSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->center.x = x;
    group_ptr->center.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpZRotSet(s16 group, float z_rot)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->zRot = z_rot;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpScaleSet(s16 group, float x, float y)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    group_ptr->scale.x = x;
    group_ptr->scale.y = y;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_XFORM;
        }
    }
}

void HuSprGrpTPLvlSet(s16 group, float tp_lvl)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].a = tp_lvl*255;
            HuSprData[group_ptr->members[i]].dirty |= SPRITE_DIRTY_COLOR;
        }
    }
}

void HuSprGrpDrawNoSet(s16 group, s32 draw_no)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprData[group_ptr->members[i]].drawNo = draw_no;
        }
    }
}

void HuSprDrawNoSet(s16 group, s16 member, s32 draw_no)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->drawNo = draw_no;
}

void HuSprPriSet(s16 group, s16 member, s16 prio)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->prio = prio;
}

void HuSprGrpScissorSet(s16 group, s16 x, s16 y, s16 w, s16 h)
{
    HUSPRGRP *group_ptr = &HuSprGrpData[group];
    s16 i;
    for(i=0; i<group_ptr->capacity; i++) {
        if(group_ptr->members[i] != HUSPR_NONE) {
            HuSprScissorSet(group, i, x, y, w, h);
        }
    }
}

void HuSprScissorSet(s16 group, s16 member, s16 x, s16 y, s16 w, s16 h)
{
    HUSPRITE *sprite_ptr = &HuSprData[HuSprGrpData[group].members[member]];
    sprite_ptr->scissorX = x;
    sprite_ptr->scissorY = y;
    sprite_ptr->scissorW = w;
    sprite_ptr->scissorH = h;
}

static s16 bitSizeTbl[11] = { 32, 24, 16, 8, 4, 16, 8, 8, 4, 8, 4 };

ANIMDATA *HuSprAnimMake(s16 sizeX, s16 sizeY, s16 dataFmt)
{
    ANIMLAYER *layer;
    ANIMBMP *bmp;
    ANIMDATA *anim;
    ANIMPAT *pat;
    ANIMFRAME *frame;
    void *temp;
    ANIMBANK *bank;
    ANIMDATA *new_anim;

    anim = new_anim = HuMemDirectMalloc(HEAP_MODEL, sizeof(ANIMDATA)+sizeof(ANIMBANK)+sizeof(ANIMFRAME)
                                            +sizeof(ANIMPAT)+sizeof(ANIMLAYER)+sizeof(ANIMBMP));

    bank = temp = &new_anim[1];
    anim->bank = bank;
    frame = temp = ((char *)temp+sizeof(ANIMBANK));
    bank->frame = frame;
    pat = temp = ((char *)temp+sizeof(ANIMFRAME));
    anim->pat = pat;
    layer = temp = ((char *)temp+sizeof(ANIMPAT));
    pat->layer = layer;
    bmp = temp = ((char *)temp+sizeof(ANIMLAYER));
    anim->bmp = bmp;
    anim->useNum = 0;
    anim->bankNum = 1;
    anim->patNum = 1;
    anim->bmpNum = (1|ANIM_BMP_ALLOC);
    bank->timeNum = 1;
    bank->unk = 10;
    frame->pat = 0;
    frame->time = 10;
    frame->shiftX = frame->shiftY = frame->flip = 0;
    pat->layerNum = 1;
    pat->centerX = sizeX/2;
    pat->centerY = sizeY/2;
    pat->sizeX = sizeX;
    pat->sizeY = sizeY;
    layer->alpha = 255;
    layer->flip = 0;
    layer->bmpNo = 0;
    layer->startX = layer->startY = 0;
    layer->sizeX = sizeX;
    layer->sizeY = sizeY;
    layer->shiftX = layer->shiftY = 0;
    layer->vtx[0] = layer->vtx[1] = 0;
    layer->vtx[2] = sizeX;
    layer->vtx[3] = 0;
    layer->vtx[4] = sizeX;
    layer->vtx[5] = sizeY;
    layer->vtx[6] = 0;
    layer->vtx[7] = sizeY;
    bmp->pixSize = bitSizeTbl[dataFmt];
    bmp->dataFmt = dataFmt;
    bmp->palNum = 0;
    bmp->sizeX = sizeX;
    bmp->sizeY = sizeY;
    bmp->dataSize = sizeX*sizeY*bitSizeTbl[dataFmt]/8;
    bmp->palData = NULL;
    bmp->data = NULL;
#ifdef TARGET_PC
    anim->port_src = NULL;
#endif
    return anim;
}

void HuSprBGSet(s16 group, s16 member,  ANIMDATA *bg, s16 bg_bank)
{
    s16 sprite = HuSprGrpData[group].members[member];
    HuSprSprBGSet(sprite, bg, bg_bank);
}

void HuSprSprBGSet(s16 sprite, ANIMDATA *bg, s16 bg_bank)
{
    HUSPRITE *sprite_ptr = &HuSprData[sprite];
    sprite_ptr->bg = bg;
    sprite_ptr->bgBank = bg_bank;
    sprite_ptr->wrapT = sprite_ptr->wrapS = GX_REPEAT;
    sprite_ptr->attr &= ~HUSPR_ATTR_LINEAR;
}

void AnimDebug(ANIMDATA *anim)
{
    ANIMPAT *pat;
    ANIMLAYER *layer;
    s16 i;
    s16 j;
    ANIMFRAME *frame;
    ANIMBANK *bank;
    ANIMBMP *bmp;
    
    OSReport("patNum %d,bankNum %d,bmpNum %d\n", anim->patNum, anim->bankNum, anim->bmpNum & ANIM_BMP_NUM_MASK);
    pat = anim->pat;
    for(i=0; i<anim->patNum; i++) {
        OSReport("PATTERN%d:\n", i);
        OSReport("\tlayerNum %d,center (%d,%d),size (%d,%d)\n", pat->layerNum, pat->centerX, pat->centerX, pat->sizeX, pat->sizeY);
        layer = pat->layer;
        for(j=0; j<pat->layerNum; j++) {
            OSReport("\t\tfileNo %d,flip %x\n", layer->bmpNo, layer->flip);
            OSReport("\t\tstart (%d,%d),size (%d,%d),shift (%d,%d)\n", layer->startX, layer->startY, layer->sizeX, layer->sizeY, layer->shiftX, layer->shiftY);
            if(j != pat->layerNum-1) {
                OSReport("\n");
            }
            layer++;
        }
        pat++;
    }
    bank = anim->bank;
    for(i=0; i<anim->bankNum; i++) {
        OSReport("BANK%d:\n", i);
        OSReport("\ttimeNum %d\n", bank->timeNum);
        frame = bank->frame;
        for(j=0; j<bank->timeNum; j++) {
            OSReport("\t\tpat %d,time %d,shift(%d,%d),flip %x\n", frame->pat, frame->time, frame->shiftX, frame->shiftY, frame->flip);
            frame++;
        }
        bank++;
    }
    bmp = anim->bmp;
    for(i=0; i<anim->bmpNum & ANIM_BMP_NUM_MASK; i++) {
        OSReport("BMP%d:\n", i);
        OSReport("\tpixSize %d,palNum %d,size (%d,%d)\n", bmp->pixSize, bmp->palNum, bmp->sizeX, bmp->sizeY);
        bmp++;
    }
}
