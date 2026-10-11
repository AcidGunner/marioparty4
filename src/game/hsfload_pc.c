/*
 * 64-bit replacement for hsfload.c (compiled only when TARGET_PC is defined).
 *
 * The original LoadHSF() parses an HSF model file IN PLACE: it lays arrays of HSFxxx structs over the file
 * buffer and overwrites 4-byte offset fields with real pointers. That only works when pointers are 4 bytes.
 * Here every section is read from the file using the GameCube (32-bit) layout, field by field, and written
 * into freshly allocated native structs. Bulk data without pointers (vertices, normals, texture coords,
 * face strips, keyframes, bitmaps, cenv weights, ...) stays in the file buffer and is pointed to directly.
 *
 * Assumptions:
 *  - The file is in the same byte order as the host (true for the big-endian PS3, which reads the
 *    GameCube's big-endian data natively). No byte swapping is done.
 *  - Native blocks are malloc()ed, not taken from the game heap. They are tracked per model and released by
 *    HuHsfFree(), which also frees the file buffer (the original freed the model by freeing the file).
 *
 * File layouts (ILP32, offsets in bytes):
 *   HSFBUFFER   0x0C : name, count, data
 *   HSFBITMAP   0x20 : name 0, maxLod 4, dataFmt 8, pixSize 9, sizeX A, sizeY C, palSize E, tint 10, palData 14,
 *                      unk 18, data 1C
 *   HSFPALETTE  0x10 : name 0, unk 4, palSize 8, data C
 *   HSFATTRIBUTE 0x84: name 0, animWorkP 4, [8..0x80) plain data, bitmap 80
 *   HSFMATERIAL 0x3C : name 0, [4..0x38) plain data, attr 38
 *   HSFFACE     0x30 : type 0, mat 2, indices 4 (strip: count 1C, data 20), nbt 24
 *   HSFOBJECT   0x144: name 0, type 4, constData 8, flags C, mesh/camera/light at 10
 *   HSFCENV     0x24, HSFCENVDUAL 0x10, HSFCENVMULTI 0x10, HSFSKELETON 0x28, HSFPART 0x0C,
 *   HSFCLUSTER  0xA0, HSFSHAPE 0x0C, HSFMAPATTR 0x18, HSFMATRIX 0x0C, HSFMOTION 0x10, HSFTRACK 0x10
 */
#ifdef TARGET_PC

#include "game/hsfload.h"
#include "game/EnvelopeExec.h"
#include "game/memory.h"
#include "ctype.h"
#include "game/hsfformat.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define F_OBJ_SIZE 0x144
#define F_MESH_OFS 0x10

_Static_assert(offsetof(HSFATTRIBUTE, bitmap) - offsetof(HSFATTRIBUTE, unk8) == 0x78, "attribute plain block");
_Static_assert(offsetof(HSFMATERIAL, attrNum) + sizeof(u32) - offsetof(HSFMATERIAL, unk4) == 0x34, "material plain block");

/* ---- file readers (host byte order, unaligned-safe) ---- */
static u32 R32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static s32 RS32(const void *p) { s32 v; memcpy(&v, p, 4); return v; }
static u16 R16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static s16 RS16(const void *p) { s16 v; memcpy(&v, p, 2); return v; }
static float RF(const void *p) { float v; memcpy(&v, p, 4); return v; }

/* ---- globals kept for compatibility with the original file ---- */
char *StringTable;
char *DicStringTable;
void *fileptr;

/* ---- per-load state ---- */
static HSFHEADER head;
static HSFDATA Model;
static const u8 *symtab;      /* file symbol table: u32 entries */
static HSFOBJECT *objtop;     /* native objects */
static const u8 *fobjtop;     /* file objects */
static HSFBUFFER *vtxtop;
static HSFCLUSTER *ClusterTop;
static HSFATTRIBUTE *AttributeTop;
static GXColor rgba[100];

/* ---- allocation tracking ---- */
typedef struct HsfEntry {
    struct HsfEntry *next;
    HSFDATA *hsf;
    void *file;
    void *chain;
} HsfEntry;

static HsfEntry *entries;
static void *cur_chain;

static void *halloc(size_t n)
{
    u8 *b;
    if (n == 0) n = 1;
    b = (u8 *)calloc(1, n + 16);
    if (!b) {
        OSReport("hsfload: out of memory (%u bytes)\n", (u32)n);
        OSPanic("hsfload_pc.c", __LINE__, "hsf alloc failed\n");
    }
    *(void **)b = cur_chain;
    cur_chain = b;
    return b + 16;
}

void HuHsfFree(HSFDATA *hsf)
{
    HsfEntry **pp, *e;
    for (pp = &entries; (e = *pp) != NULL; pp = &e->next) {
        if (e->hsf == hsf) {
            void *c = e->chain;
            *pp = e->next;
            while (c) {
                void *next = *(void **)c;
                free(c);
                c = next;
            }
            if (e->file) HuMemDirectFree(e->file);
            free(e);
            return;
        }
    }
    OSReport("hsfload: HuHsfFree(%p): unknown model, freeing as raw block\n", (void *)hsf);
    HuMemDirectFree(hsf);
}

/* ---- helpers ---- */
#define SEC(x) ((const u8 *)fileptr + head.x.ofs)

static char *Str(u32 ofs) { return &StringTable[ofs]; }
static u32 Sym(u32 idx) { return R32(symtab + idx * 4); }

char *SetName(u32 *str_ofs) { return &StringTable[*str_ofs]; }

static HSFBUFFER *SearchVertexPtr(s32 id) { return id == -1 ? NULL : &Model.vertex[id]; }
static HSFBUFFER *SearchNormalPtr(s32 id) { return id == -1 ? NULL : &Model.normal[id]; }
static HSFBUFFER *SearchStPtr(s32 id) { return id == -1 ? NULL : &Model.st[id]; }
static HSFBUFFER *SearchColorPtr(s32 id) { return id == -1 ? NULL : &Model.color[id]; }
static HSFBUFFER *SearchFacePtr(s32 id) { return id == -1 ? NULL : &Model.face[id]; }
static HSFCENV *SearchCenvPtr(s32 id) { return id == -1 ? NULL : &Model.cenv[id]; }
static HSFPART *SearchPartPtr(s32 id) { return id == -1 ? NULL : &Model.part[id]; }
static HSFPALETTE *SearchPalettePtr(s32 id) { return id == -1 ? NULL : &Model.palette[id]; }
static HSFBITMAP *SearchBitmapPtr(s32 id) { return id == -1 ? NULL : &Model.bitmap[id]; }

/* ---- generic name/data buffers (color, vertex, normal, st, face) ---- */
static HSFBUFFER *LoadBuffers(const HSFSECTION *sec, const u8 **dataBase)
{
    const u8 *f = (const u8 *)fileptr + sec->ofs;
    const u8 *data = f + sec->count * 0x0C;
    HSFBUFFER *nb = (HSFBUFFER *)halloc(sec->count * sizeof(HSFBUFFER));
    s32 i;
    for (i = 0; i < sec->count; i++) {
        const u8 *e = f + i * 0x0C;
        nb[i].name = Str(R32(e));
        nb[i].count = RS32(e + 4);
        nb[i].data = (void *)(data + R32(e + 8));
    }
    if (dataBase) *dataBase = data;
    return nb;
}

static void SceneLoad(void)
{
    if (head.scene.count) {
        Model.scene = (HSFSCENE *)SEC(scene); /* floats + colour only: usable in place */
        Model.sceneNum = head.scene.count;
    }
}

static void ColorLoad(void)
{
    if (head.color.count) {
        Model.color = LoadBuffers(&head.color, NULL);
        Model.colorNum = head.color.count;
    }
}

static void VertexLoad(void)
{
    if (head.vertex.count) {
        vtxtop = Model.vertex = LoadBuffers(&head.vertex, NULL);
        Model.vertexNum = head.vertex.count;
    }
}

static void NormalLoad(void)
{
    if (head.normal.count) {
        Model.normal = LoadBuffers(&head.normal, NULL);
        Model.normalNum = head.normal.count;
    }
}

static void STLoad(void)
{
    if (head.st.count) {
        Model.st = LoadBuffers(&head.st, NULL);
        Model.stNum = head.st.count;
    }
}

static void FaceLoad(void)
{
    s32 i, j, k;
    const u8 *strip = NULL;
    if (!head.face.count) return;
    Model.face = LoadBuffers(&head.face, NULL);
    Model.faceNum = head.face.count;
    /* strip data follows the faces of the LAST buffer; every buffer's strips are addressed from there */
    {
        HSFBUFFER *last = &Model.face[head.face.count - 1];
        strip = (const u8 *)last->data + last->count * 0x30;
    }
    for (i = 0; i < head.face.count; i++) {
        HSFBUFFER *b = &Model.face[i];
        const u8 *ff = (const u8 *)b->data;
        HSFFACE *nf = (HSFFACE *)halloc((size_t)b->count * sizeof(HSFFACE));
        for (j = 0; j < b->count; j++, ff += 0x30) {
            HSFFACE *o = &nf[j];
            o->type = RS16(ff);
            o->mat = RS16(ff + 2);
            if (R16(ff) == 4) {
                for (k = 0; k < 12; k++) o->strip.indices[k / 4][k % 4] = RS16(ff + 4 + k * 2);
                o->strip.count = R32(ff + 0x1C);
                o->strip.data = (s16 *)(strip + R32(ff + 0x20) * (sizeof(s16) * 4));
            } else {
                for (k = 0; k < 16; k++) o->indices[k / 4][k % 4] = RS16(ff + 4 + k * 2);
            }
            o->nbt.x = RF(ff + 0x24);
            o->nbt.y = RF(ff + 0x28);
            o->nbt.z = RF(ff + 0x2C);
        }
        b->data = nf;
    }
}

static void PaletteLoad(void)
{
    s32 i;
    const u8 *f, *data_base;
    HSFPALETTE *np;
    if (!head.palette.count) return;
    f = SEC(palette);
    data_base = f + head.palette.count * 0x10;
    np = (HSFPALETTE *)halloc(head.palette.count * sizeof(HSFPALETTE));
    for (i = 0; i < head.palette.count; i++) {
        const u8 *e = f + i * 0x10;
        np[i].name = Str(R32(e));
        np[i].unk = RS32(e + 4);
        np[i].palSize = R32(e + 8);
        np[i].data = (u16 *)(data_base + R32(e + 0xC));
    }
    Model.palette = np;
    Model.paletteNum = head.palette.count;
}

static void BitmapLoad(void)
{
    s32 i;
    const u8 *f, *data_base;
    HSFBITMAP *nb;
    if (!head.bitmap.count) return;
    f = SEC(bitmap);
    data_base = f + head.bitmap.count * 0x20;
    nb = (HSFBITMAP *)halloc(head.bitmap.count * sizeof(HSFBITMAP));
    Model.bitmap = nb;
    Model.bitmapNum = head.bitmap.count;
    for (i = 0; i < head.bitmap.count; i++) {
        const u8 *e = f + i * 0x20;
        HSFPALETTE *pal;
        nb[i].name = Str(R32(e));
        nb[i].maxLod = R32(e + 4);
        nb[i].dataFmt = e[8];
        nb[i].pixSize = e[9];
        nb[i].sizeX = RS16(e + 0xA);
        nb[i].sizeY = RS16(e + 0xC);
        nb[i].palSize = RS16(e + 0xE);
        memcpy(&nb[i].tint, e + 0x10, sizeof(GXColor));
        pal = SearchPalettePtr(RS32(e + 0x14));
        nb[i].palData = pal ? pal->data : NULL;
        nb[i].unk = R32(e + 0x18);
        nb[i].data = (void *)(data_base + R32(e + 0x1C));
    }
}

static void MaterialLoad(void)
{
    s32 i, j;
    const u8 *f;
    HSFMATERIAL *nm;
    if (!head.material.count) return;
    f = SEC(material);
    nm = (HSFMATERIAL *)halloc(head.material.count * sizeof(HSFMATERIAL));
    Model.material = nm;
    Model.materialNum = head.material.count;
    for (i = 0; i < head.material.count; i++) {
        const u8 *e = f + i * 0x3C;
        nm[i].name = Str(R32(e));
        memcpy(&nm[i].unk4, e + 4, 0x34); /* pass .. attrNum: no pointers, same layout */
        nm[i].attr = (s32 *)halloc(nm[i].attrNum * sizeof(s32));
        for (j = 0; j < (s32)nm[i].attrNum; j++) nm[i].attr[j] = RS32(symtab + (R32(e + 0x38) + j) * 4);
        if (i < 100) {
            rgba[i].r = nm[i].litColor[0];
            rgba[i].g = nm[i].litColor[1];
            rgba[i].b = nm[i].litColor[2];
            rgba[i].a = 255;
        }
    }
}

static void AttributeLoad(void)
{
    s32 i;
    const u8 *f;
    HSFATTRIBUTE *na;
    if (!head.attribute.count) return;
    f = SEC(attribute);
    na = (HSFATTRIBUTE *)halloc(head.attribute.count * sizeof(HSFATTRIBUTE));
    AttributeTop = Model.attribute = na;
    Model.attributeNum = head.attribute.count;
    for (i = 0; i < head.attribute.count; i++) {
        const u8 *e = f + i * 0x84;
        u32 nameOfs = R32(e);
        na[i].name = (nameOfs != 0xFFFFFFFFu) ? Str(nameOfs) : NULL;
        na[i].animWorkP = NULL;
        memcpy(&na[i].unk8, e + 8, 0x78);
        na[i].bitmap = SearchBitmapPtr(RS32(e + 0x80));
    }
}

static void CenvLoad(void)
{
    s32 i, j;
    const u8 *f, *data_base, *weight_base;
    HSFCENV *nc;
    if (!head.cenv.count) return;
    f = SEC(cenv);
    data_base = f + head.cenv.count * 0x24;
    weight_base = data_base;
    nc = (HSFCENV *)halloc(head.cenv.count * sizeof(HSFCENV));
    Model.cenv = nc;
    Model.cenvNum = head.cenv.count;
    for (i = 0; i < head.cenv.count; i++) {
        const u8 *e = f + i * 0x24;
        nc[i].name = NULL;
        nc[i].singleCount = R32(e + 0x10);
        nc[i].dualCount = R32(e + 0x14);
        nc[i].multiCount = R32(e + 0x18);
        nc[i].vtxCount = R32(e + 0x1C);
        nc[i].copyCount = R32(e + 0x20);
        weight_base += nc[i].singleCount * 0x0C + nc[i].dualCount * 0x10 + nc[i].multiCount * 0x10;
    }
    for (i = 0; i < head.cenv.count; i++) {
        const u8 *e = f + i * 0x24;
        const u8 *sd = data_base + R32(e + 4);
        const u8 *dd = data_base + R32(e + 8);
        const u8 *md = data_base + R32(e + 0xC);
        HSFCENVDUAL *ndual;
        HSFCENVMULTI *nmulti;
        nc[i].singleData = (HSFCENVSINGLE *)sd; /* 12 bytes, no pointers: usable in place */
        ndual = (HSFCENVDUAL *)halloc(nc[i].dualCount * sizeof(HSFCENVDUAL));
        for (j = 0; j < (s32)nc[i].dualCount; j++) {
            const u8 *d = dd + j * 0x10;
            ndual[j].target1 = R32(d);
            ndual[j].target2 = R32(d + 4);
            ndual[j].weightNum = R32(d + 8);
            ndual[j].weight = (HSFCENVDUALWEIGHT *)(weight_base + R32(d + 0xC));
        }
        nc[i].dualData = ndual;
        nmulti = (HSFCENVMULTI *)halloc(nc[i].multiCount * sizeof(HSFCENVMULTI));
        for (j = 0; j < (s32)nc[i].multiCount; j++) {
            const u8 *m = md + j * 0x10;
            nmulti[j].weightNum = R32(m);
            nmulti[j].pos = R16(m + 4);
            nmulti[j].posNum = R16(m + 6);
            nmulti[j].normal = R16(m + 8);
            nmulti[j].normalNum = R16(m + 0xA);
            nmulti[j].weight = (HSFCENVMULTIWEIGHT *)(weight_base + R32(m + 0xC));
        }
        nc[i].multiData = nmulti;
    }
}

static void SkeletonLoad(void)
{
    s32 i, k;
    const u8 *f;
    HSFSKELETON *ns;
    if (!head.skeleton.count) return;
    f = SEC(skeleton);
    ns = (HSFSKELETON *)halloc(head.skeleton.count * sizeof(HSFSKELETON));
    Model.skeleton = ns;
    Model.skeletonNum = head.skeleton.count;
    for (i = 0; i < head.skeleton.count; i++) {
        const u8 *e = f + i * 0x28;
        float *dst = (float *)&ns[i].transform;
        ns[i].name = Str(R32(e));
        for (k = 0; k < 9; k++) dst[k] = RF(e + 4 + k * 4);
    }
}

static void PartLoad(void)
{
    s32 i;
    const u8 *f, *data;
    HSFPART *np;
    if (!head.part.count) return;
    f = SEC(part);
    data = f + head.part.count * 0x0C;
    np = (HSFPART *)halloc(head.part.count * sizeof(HSFPART));
    Model.part = np;
    Model.partNum = head.part.count;
    for (i = 0; i < head.part.count; i++) {
        const u8 *e = f + i * 0x0C;
        np[i].name = Str(R32(e));
        np[i].num = R32(e + 4);
        np[i].vertex = (u16 *)data + R32(e + 8);
    }
}

static void ClusterLoad(void)
{
    s32 i, j, k;
    const u8 *f;
    HSFCLUSTER *nc;
    if (!head.cluster.count) return;
    f = SEC(cluster);
    nc = (HSFCLUSTER *)halloc(head.cluster.count * sizeof(HSFCLUSTER));
    ClusterTop = Model.cluster = nc;
    Model.clusterNum = head.cluster.count;
    for (i = 0; i < head.cluster.count; i++) {
        const u8 *e = f + i * 0xA0;
        nc[i].name[0] = Str(R32(e));
        nc[i].name[1] = Str(R32(e + 4));
        nc[i].targetName = Str(R32(e + 8));
        nc[i].part = SearchPartPtr(RS32(e + 0xC));
        nc[i].index = RF(e + 0x10);
        for (k = 0; k < 32; k++) nc[i].weight[k] = RF(e + 0x14 + k * 4);
        nc[i].adjusted = e[0x94];
        nc[i].unk95 = e[0x95];
        nc[i].type = R16(e + 0x96);
        nc[i].vertexNum = R32(e + 0x98);
        nc[i].vertex = (HSFBUFFER **)halloc(nc[i].vertexNum * sizeof(HSFBUFFER *));
        for (j = 0; j < (s32)nc[i].vertexNum; j++) nc[i].vertex[j] = SearchVertexPtr(RS32(symtab + (R32(e + 0x9C) + j) * 4));
    }
}

static void ShapeLoad(void)
{
    s32 i, j;
    const u8 *f;
    HSFSHAPE *ns;
    if (!head.shape.count) return;
    f = SEC(shape);
    ns = (HSFSHAPE *)halloc(head.shape.count * sizeof(HSFSHAPE));
    Model.shape = ns;
    Model.shapeNum = head.shape.count;
    for (i = 0; i < head.shape.count; i++) {
        const u8 *e = f + i * 0x0C;
        ns[i].name = Str(R32(e));
        ns[i].num16[0] = R16(e + 4);
        ns[i].num16[1] = R16(e + 6);
        ns[i].vertex = (HSFBUFFER **)halloc(ns[i].num16[1] * sizeof(HSFBUFFER *));
        for (j = 0; j < ns[i].num16[1]; j++) ns[i].vertex[j] = &vtxtop[Sym(R32(e + 8) + j)];
    }
}

static void MapAttrLoad(void)
{
    s32 i;
    const u8 *f, *data;
    HSFMAPATTR *nm;
    if (!head.mapAttr.count) return;
    f = SEC(mapAttr);
    data = f + head.mapAttr.count * 0x18;
    nm = (HSFMAPATTR *)halloc(head.mapAttr.count * sizeof(HSFMAPATTR));
    Model.mapAttr = nm;
    Model.mapAttrNum = head.mapAttr.count;
    for (i = 0; i < head.mapAttr.count; i++) {
        const u8 *e = f + i * 0x18;
        nm[i].minX = RF(e);
        nm[i].minZ = RF(e + 4);
        nm[i].maxX = RF(e + 8);
        nm[i].maxZ = RF(e + 0xC);
        nm[i].data = (u16 *)data + R32(e + 0x10);
        nm[i].dataLen = R32(e + 0x14);
    }
}

/* ---- objects ---- */
static void DispObject(HSFOBJECT *parent, s32 idx)
{
    HSFOBJECT *obj = &objtop[idx];
    const u8 *fo = fobjtop + idx * F_OBJ_SIZE;
    const u8 *fm = fo + F_MESH_OFS;
    u32 i, n;
    switch (obj->type) {
        case HSF_OBJ_MESH:
        case HSF_OBJ_NULL1:
        case HSF_OBJ_REPLICA:
        case HSF_OBJ_ROOT:
        case HSF_OBJ_JOINT:
        case HSF_OBJ_NULL2:
        case HSF_OBJ_MAP:
            break;
        default:
            return;
    }
    obj->mesh.parent = parent;
    n = obj->mesh.childrenCount;
    obj->mesh.children = (HSFOBJECT **)halloc(n * sizeof(HSFOBJECT *));
    for (i = 0; i < n; i++) obj->mesh.children[i] = &objtop[Sym(R32(fm + 8) + i)];
    if (Model.root == NULL) Model.root = obj;
    if (obj->type == HSF_OBJ_MESH) {
        obj->mesh.vertex = SearchVertexPtr(RS32(fm + 0xF8));
        obj->mesh.normal = SearchNormalPtr(RS32(fm + 0xFC));
        obj->mesh.st = SearchStPtr(RS32(fm + 0x104));
        obj->mesh.color = SearchColorPtr(RS32(fm + 0x100));
        obj->mesh.face = SearchFacePtr(RS32(fm + 0xF4));
        obj->mesh.shape = (HSFBUFFER **)halloc(obj->mesh.shapeNum * sizeof(HSFBUFFER *));
        for (i = 0; i < obj->mesh.shapeNum; i++) obj->mesh.shape[i] = &vtxtop[Sym(R32(fm + 0x118) + i)];
        obj->mesh.cluster = (HSFCLUSTER **)halloc(obj->mesh.clusterNum * sizeof(HSFCLUSTER *));
        for (i = 0; i < obj->mesh.clusterNum; i++) obj->mesh.cluster[i] = &ClusterTop[Sym(R32(fm + 0x120) + i)];
        obj->mesh.cenv = SearchCenvPtr(RS32(fm + 0x128));
        obj->mesh.material = Model.material;
        obj->mesh.attribute = (RS32(fm + 0x10C) >= 0) ? Model.attribute : NULL;
        obj->mesh.file[0] = (u8 *)fileptr + R32(fm + 0x12C);
        obj->mesh.file[1] = (u8 *)fileptr + R32(fm + 0x130);
    } else if (obj->type == HSF_OBJ_REPLICA) {
        obj->mesh.replica = &objtop[R32(fm + 0x54)];
    }
    for (i = 0; i < n; i++) DispObject(obj, (s32)(obj->mesh.children[i] - objtop));
}

static void ObjectLoad(void)
{
    s32 i, k, root = 0;
    const u8 *f;
    HSFOBJECT *no;
    if (!head.object.count) return;
    f = SEC(object);
    fobjtop = f;
    no = (HSFOBJECT *)halloc(head.object.count * sizeof(HSFOBJECT));
    objtop = no;
    for (i = 0; i < head.object.count; i++) {
        const u8 *fo = f + i * F_OBJ_SIZE;
        const u8 *fm = fo + F_MESH_OFS;
        no[i].name = Str(R32(fo));
        no[i].type = R32(fo + 4);
        no[i].constData = NULL;
        no[i].flags = R32(fo + 0xC);
        if (no[i].type == HSF_OBJ_CAMERA) {
            float *c = (float *)&no[i].camera;
            for (k = 0; k < 10; k++) c[k] = RF(fm + k * 4);
        } else if (no[i].type == HSF_OBJ_LIGHT) {
            HSFLIGHT *l = &no[i].light;
            float *a = (float *)&l->pos;
            for (k = 0; k < 6; k++) a[k] = RF(fm + k * 4);
            l->type = fm[0x18];
            l->r = fm[0x19];
            l->g = fm[0x1A];
            l->b = fm[0x1B];
            l->unk2C = RF(fm + 0x1C);
            l->ref_distance = RF(fm + 0x20);
            l->ref_brightness = RF(fm + 0x24);
            l->cutoff = RF(fm + 0x28);
        } else {
            float *t = (float *)&no[i].mesh.base;
            for (k = 0; k < 18; k++) t[k] = RF(fm + 0xC + k * 4); /* base then curr: 18 floats */
            {
                float *m = (float *)&no[i].mesh.mesh;
                for (k = 0; k < 40; k++) m[k] = RF(fm + 0x54 + k * 4); /* min/max/baseMorph/morphWeight */
            }
            no[i].mesh.childrenCount = R32(fm + 4);
            no[i].mesh.writeNum = fm[0x110];
            no[i].mesh.unk121 = fm[0x111];
            no[i].mesh.shapeType = fm[0x112];
            no[i].mesh.matPass = fm[0x113];
            no[i].mesh.shapeNum = R32(fm + 0x114);
            no[i].mesh.clusterNum = R32(fm + 0x11C);
            no[i].mesh.cenvNum = R32(fm + 0x124);
        }
    }
    for (i = 0; i < head.object.count; i++) {
        if (RS32(f + i * F_OBJ_SIZE + F_MESH_OFS) == -1) { root = i; break; }
    }
    DispObject(NULL, root);
    Model.objectNum = head.object.count;
}

/* ---- motion ---- */
static s32 FindObjectName(const char *name)
{
    s32 i;
    for (i = 0; i < head.object.count; i++) {
        if (!strcmp(objtop[i].name, name)) return i;
    }
    return -1;
}

static s32 FindClusterName(const char *name)
{
    s32 i;
    for (i = 0; i < head.cluster.count; i++) {
        if (!strcmp(ClusterTop[i].name[0], name)) return i;
    }
    return -1;
}

static s32 FindAttributeName(const char *name)
{
    s32 i;
    for (i = 0; i < head.attribute.count; i++) {
        if (!AttributeTop[i].name) continue;
        if (!strcmp(AttributeTop[i].name, name)) return i;
    }
    return -1;
}

static void MotionLoad(void)
{
    s32 i, k;
    const u8 *f, *tracks, *track_data;
    HSFMOTION *nm;
    HSFTRACK *nt;
    s32 numTracks;
    if (!head.motion.count) return;
    f = SEC(motion);
    nm = (HSFMOTION *)halloc(head.motion.count * sizeof(HSFMOTION));
    for (i = 0; i < head.motion.count; i++) {
        const u8 *e = f + i * 0x10;
        nm[i].name = Str(R32(e));
        nm[i].numTracks = RS32(e + 4);
        nm[i].maxTime = RF(e + 0xC);
    }
    Model.motion = nm;
    numTracks = RS32(f + 4);
    Model.motionNum = numTracks;
    tracks = f + head.motion.count * 0x10;
    track_data = tracks + numTracks * 0x10;
    nt = (HSFTRACK *)halloc(numTracks * sizeof(HSFTRACK));
    nm[0].track = nt;
    for (i = 0; i < numTracks; i++) {
        const u8 *e = tracks + i * 0x10;
        HSFTRACK *t = &nt[i];
        u16 nameOfs = R16(e + 2);
        u32 dataOfs = R32(e + 0xC);
        t->type = e[0];
        t->start = e[1];
        t->target = nameOfs;
        memcpy((u8 *)t + offsetof(HSFTRACK, curveType) - 4, e + 4, 4); /* union at +4: clusterWeight / attrIdx+channel */
        t->curveType = R16(e + 8);
        t->numKeyframes = R16(e + 0xA);
        switch (t->type) {
            case HSF_TRACK_TRANSFORM:
            case HSF_TRACK_MORPH:
                if (objtop) t->target = (u16)FindObjectName(Str(nameOfs));
                break;
            case HSF_TRACK_CLUSTER:
            case HSF_TRACK_CLUSTER_WEIGHT:
                t->target = (u16)FindClusterName(Str(nameOfs));
                break;
            case HSF_TRACK_ATTRIBUTE:
                if ((s16)nameOfs != -1) t->attrIdx = (s16)FindAttributeName(Str(nameOfs));
                break;
            default:
                break;
        }
        switch (t->curveType) {
            case HSF_CURVE_STEP:
            case HSF_CURVE_LINEAR:
            case HSF_CURVE_BEZIER:
                t->data = (void *)(track_data + dataOfs);
                break;
            case HSF_CURVE_BITMAP:
                if (t->type == HSF_TRACK_ATTRIBUTE) {
                    const u8 *kf = track_data + dataOfs;
                    HSFBITMAPKEY *nk = (HSFBITMAPKEY *)halloc((size_t)t->numKeyframes * sizeof(HSFBITMAPKEY));
                    for (k = 0; k < t->numKeyframes; k++) {
                        nk[k].time = RF(kf + k * 8);
                        nk[k].data = SearchBitmapPtr(RS32(kf + k * 8 + 4));
                    }
                    t->data = nk;
                }
                break;
            case HSF_CURVE_CONST:
                t->value = RF(e + 0xC);
                break;
            default:
                break;
        }
    }
}

static void MatrixLoad(void)
{
    const u8 *f;
    HSFMATRIX *nm;
    if (!head.matrix.count) return;
    f = SEC(matrix);
    nm = (HSFMATRIX *)halloc(head.matrix.count * sizeof(HSFMATRIX));
    nm[0].base_idx = R32(f);
    nm[0].count = R32(f + 4);
    nm[0].data = (Mtx *)(f + 0x0C); /* matrices follow the first 12-byte header, as in the original */
    Model.matrix = nm;
    Model.matrixNum = head.matrix.count;
}


/* ---- diagnostics (TTY): summary of a loaded model plus sanity checks of the converted data ---- */
static void HsfDiag(HSFDATA *h)
{
    s32 i, j, k, bad = 0, shown = 0;
    OSReport("[hsf] loaded: obj %d vtx %d nrm %d st %d col %d face %d mat %d attr %d bmp %d pal %d cenv %d skel %d clus %d mot %d(trk) mtx %d\n",
             h->objectNum, h->vertexNum, h->normalNum, h->stNum, h->colorNum, h->faceNum, h->materialNum, h->attributeNum,
             h->bitmapNum, h->paletteNum, h->cenvNum, h->skeletonNum, h->clusterNum, h->motionNum, h->matrixNum);
    for (i = 0; i < h->objectNum; i++) {
        HSFOBJECT *o = &h->object[i];
        if (o->type != HSF_OBJ_MESH) continue;
        OSReport("[hsf]  mesh %d '%s': vtx %d nrm %d st %d col %d face %d (faces %d) cenvNum %u\n", i, o->name,
                 o->mesh.vertex ? (s32)(o->mesh.vertex - h->vertex) : -1, o->mesh.normal ? (s32)(o->mesh.normal - h->normal) : -1,
                 o->mesh.st ? (s32)(o->mesh.st - h->st) : -1, o->mesh.color ? (s32)(o->mesh.color - h->color) : -1,
                 o->mesh.face ? (s32)(o->mesh.face - h->face) : -1, o->mesh.face ? o->mesh.face->count : 0, o->mesh.cenvNum);
        if (o->mesh.face) {
            HSFFACE *f = (HSFFACE *)o->mesh.face->data;
            for (j = 0; j < o->mesh.face->count; j++, f++) {
                HSFMATERIAL *m = &h->material[f->mat & 0xFFF];
                s32 nv = (f->type & 7) == 3 ? 3 : 4;
                if ((f->type & 7) == 4) nv = 3;
                if ((f->mat & 0xFFF) >= h->materialNum) { bad++; if (shown++ < 6) OSReport("[hsf]   face %d: material %d out of range\n", j, f->mat); continue; }
                if (m->vtxMode == 5 && !o->mesh.color) { bad++; if (shown++ < 6) OSReport("[hsf]   face %d: vtxMode 5 but mesh has no color buffer\n", j); }
                for (k = 0; k < nv; k++) {
                    if (o->mesh.vertex && (f->indices[k][0] < 0 || f->indices[k][0] >= o->mesh.vertex->count)) { bad++; if (shown++ < 6) OSReport("[hsf]   face %d: vertex idx %d >= %d\n", j, f->indices[k][0], o->mesh.vertex->count); }
                    if (o->mesh.color && m->vtxMode == 5 && (f->indices[k][2] < 0 || f->indices[k][2] >= o->mesh.color->count)) { bad++; if (shown++ < 6) OSReport("[hsf]   face %d: color idx %d >= %d\n", j, f->indices[k][2], o->mesh.color->count); }
                }
            }
        }
    }
    OSReport("[hsf] sanity: %d problem(s)\n", bad);
}

/* ---- entry point ---- */
HSFDATA *LoadHSF(void *data)
{
    HSFDATA *hsf;
    HsfEntry *ent;
    const u8 *h = (const u8 *)data;
    s32 *sec;
    s32 i;

    cur_chain = NULL;
    memset(&Model, 0, sizeof(Model));
    objtop = NULL;
    vtxtop = NULL;
    ClusterTop = NULL;
    AttributeTop = NULL;
    fileptr = data;

    memcpy(head.magic, h, 8);
    sec = (s32 *)&head.scene;
    for (i = 0; i < 42; i++) sec[i] = RS32(h + 8 + i * 4); /* 21 sections of {ofs, count} */
    StringTable = (char *)data + head.string.ofs;
    symtab = (const u8 *)data + head.symbol.ofs;

    SceneLoad();
    ColorLoad();
    PaletteLoad();
    BitmapLoad();
    MaterialLoad();
    AttributeLoad();
    VertexLoad();
    NormalLoad();
    STLoad();
    FaceLoad();
    CenvLoad();
    SkeletonLoad();
    PartLoad();
    ClusterLoad();
    ShapeLoad();
    MapAttrLoad();
    ObjectLoad();
    MotionLoad();
    MatrixLoad();

    hsf = (HSFDATA *)halloc(sizeof(HSFDATA));
    memcpy(hsf, &Model, sizeof(HSFDATA));
    memcpy(hsf->magic, head.magic, 8);
    hsf->object = objtop;

#ifndef HSF_NO_4GB_CHECK /* sanitizer builds put the heap above 4 GB */
    if ((uintptr_t)hsf >> 32) {
        OSReport("hsfload: HSFDATA at %p is above 4 GB (the game stores model ids as u32)\n", (void *)hsf);
        OSPanic("hsfload_pc.c", __LINE__, "hsf above 4GB\n");
    }
#endif
    ent = (HsfEntry *)calloc(1, sizeof(HsfEntry));
    ent->hsf = hsf;
    ent->file = data;
    ent->chain = cur_chain;
    ent->next = entries;
    entries = ent;
    cur_chain = NULL;
    HsfDiag(hsf);
    InitEnvelope(hsf);
    return hsf;
}

/* ---- unchanged helpers from hsfload.c (they only touch native structs) ---- */
static s32 SearchObjectSetName(HSFDATA *data, char *name);

void ClusterAdjustObject(HSFDATA *model, HSFDATA *src_model)
{
    HSFCLUSTER *cluster;
    s32 i;
    if (!src_model) return;
    if (src_model->clusterNum == 0) return;
    cluster = src_model->cluster;
    if (cluster->adjusted) return;
    cluster->adjusted = 1;
    for (i = 0; i < src_model->clusterNum; i++, cluster++) {
        char *name = cluster->targetName;
        cluster->target = SearchObjectSetName(model, name);
    }
}

static s32 SearchObjectSetName(HSFDATA *data, char *name)
{
    HSFOBJECT *object = data->object;
    s32 i;
    for (i = 0; i < data->objectNum; i++, object++) {
        if (!CmpObjectName(object->name, name)) return i;
    }
    OSReport("Search Object Error %s\n", name);
    return -1;
}

char *MakeObjectName(char *name)
{
    static char buf[768];
    s32 index = 0, num_minus = 0;
    char *temp_name = name;
    while (*temp_name) {
        if (*temp_name == '-') {
            name = temp_name + 1;
            break;
        }
        temp_name++;
    }
    while (*name) {
        if (num_minus != 0) break;
        if (*name == '_' && !isalpha(name[1])) {
            num_minus++;
            break;
        }
        buf[index] = *name;
        name++;
        index++;
    }
    buf[index] = '\0';
    return buf;
}

s32 CmpObjectName(char *name1, char *name2) { return strcmp(name1, name2); }

#endif /* TARGET_PC */
