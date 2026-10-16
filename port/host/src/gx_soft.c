/*
 * Software implementation of the subset of GX (GameCube GPU API) the game uses.
 *
 * Rendering goes into a CPU-side 32-bit RGBA framebuffer + float depth buffer (the "EFB"). GXCopyDisp() snapshots
 * it, and port_gx_present() hands the snapshot to port_video_present() (RSX display on the PS3, nothing/test hook
 * on the host). Meant as a first, correctness-first path to get a picture; a hardware (RSX) path can replace it.
 *
 * Supported: matrices/viewport/scissor, direct and indexed vertex data (POS/NRM/CLR0/CLR1/TEX0-7), basic lighting,
 * texgen (2x4/3x4 matrices from POS/NRM/TEX), full TEV colour/alpha combiner incl. konstants, textures (I4 I8 IA4 IA8
 * RGB565 RGB5A3 RGBA8 C4 C8 C14X2 CMPR + TLUTs), blending, alpha compare, Z buffer, culling, display lists.
 * Not supported yet: fog, indirect texturing, texture swap tables, EFB->texture copies, lines/points, mipmapping.
 *
 * All data is read in host byte order (the PS3 is big-endian like the GameCube; no byte swapping is done).
 */
#include <dolphin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>
#include <sys/time.h>

/* Internal render resolution divisor: 1 = full 640x480, 2 = half (320x240, upscaled by the video layer).
 * Default is full resolution; -DGX_DIV=2 trades quality for speed. */
#ifndef GX_DIV
#define GX_DIV 1
#endif
#define EFB_W (640 / GX_DIV)
#define EFB_H (528 / GX_DIV)
#define MAXTEV 16

_Static_assert(GX_CC_RASC == 10 && GX_CC_ZERO == 15 && GX_CC_TEXC == 8, "TEV colour arg enum");
_Static_assert(GX_CA_TEXA == 4 && GX_CA_ZERO == 7 && GX_CA_KONST == 6, "TEV alpha arg enum");

/* ------------------------------------------------------------------------------------------------ state */
typedef struct { float v[4]; } F4;

typedef struct {
    u8 enable, amb_src, mat_src, diff_fn, attn_fn;
    u32 light_mask;
} ChanCtrl;

typedef struct {
    float pos[3], dir[3];
    GXColor color;
    float a[3], k[3];
} LightD; /* overlay on GXLightObj (64 bytes) */

typedef struct {
    u8 func, src, mtx, post;
} TexGen;

typedef struct {
    u8 ci[4], ai[4];
    u8 c_op, c_bias, c_scale, c_clamp, c_out;
    u8 a_op, a_bias, a_scale, a_clamp, a_out;
    u8 coord, map, color;
    u8 kcsel, kasel;
} Stage;

typedef struct {
    u32 *rgba;
    int w, h;
    u8 ws, wt;
    int wmask, hmask; /* w-1 / h-1 when a power of two, else -1 */
} Tex;

typedef struct {
    u32 ptr_lo, ptr_hi;
    u16 w, h;
    u8 fmt, ws, wt, mip, minf, magf;
    u32 tlut;
} TexObjD; /* overlay on GXTexObj (88 bytes) */

typedef struct {
    u32 ptr_lo, ptr_hi;
    u16 n;
    u8 fmt;
} TlutObjD;

static struct {
    int inited;
    /* transform */
    float pos_mtx[10][12], nrm_mtx[10][12], tex_mtx[10][12];
    float proj[4][4];
    int cur_mtx;
    float vp[6];
    int sc[4];
    /* vertex format */
    u8 vdesc[GX_VA_MAX_ATTR + 1];
    struct { u8 cnt, type, frac; } vfmt[8][GX_VA_MAX_ATTR + 1];
    const u8 *arr[GX_VA_MAX_ATTR + 1];
    u32 arr_stride[GX_VA_MAX_ATTR + 1];
    /* colour channels: 0 COLOR0, 1 COLOR1, 2 ALPHA0, 3 ALPHA1 */
    ChanCtrl chan[4];
    float mat[2][4], amb[2][4];
    int nchans;
    LightD light[8];
    /* texgen / TEV */
    TexGen texgen[8];
    int ntexgens;
    Stage st[MAXTEV];
    int ntev;
    float reg[4][4];
    float kcol[4][4];
    /* raster */
    int cull, z_en, z_func, z_upd;
    int bl_type, bl_src, bl_dst;
    int col_upd, a_upd;
    int ac0, ar0, aop, ac1, ar1;
    /* textures */
    Tex *bound[8];
    /* EFB */
    u32 *color;
    float *depth;
    u32 clear_color;
    float clear_z, depth_fill_z;
    int depth_dirty;
    /* copy / present */
    u32 *front;
    int copy_w, copy_h, copy_h_real, front_w, front_h;
    int frame_ready;
    u32 frames;
} G;


/* ---- statistics, printed once per ~2 seconds of presented frames ---- */
static struct {
    u32 tex_decodes, tris, tris_drawn, px_fast, px_general, frames, vc_hit, vc_miss, fast_fail[8];
    u8 pat[6][16];
    u32 pat_n[6];
    u64 t_draw, t_present, t_decode, t_last;
} G_stats;

static u64 now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * 1000000ull + tv.tv_usec;
}

static u32 vepoch = 1; /* bumped by every call that changes how a vertex would be transformed/lit */

static GXFifoObj fifo_dummy;

/* ------------------------------------------------------------------------------------------------ helpers */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static float sat(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

static void efb_clear(void)
{
    /* colour: 64-bit stores; depth: only if something wrote to it since the last fill (2D-only frames never do) */
    u64 v = ((u64)G.clear_color << 32) | G.clear_color;
    u64 *c = (u64 *)G.color;
    int i, n = EFB_W * EFB_H / 2;
    for (i = 0; i < n; i++) c[i] = v;
    if (G.depth_dirty || G.depth_fill_z != G.clear_z) {
        for (i = 0; i < EFB_W * EFB_H; i++) G.depth[i] = G.clear_z;
        G.depth_dirty = 0;
        G.depth_fill_z = G.clear_z;
    }
}

static void mtx_identity34(float m[12])
{
    memset(m, 0, 12 * sizeof(float));
    m[0] = m[5] = m[10] = 1.f;
}

static void gx_ensure_init(void)
{
    int i;
    if (G.inited) return;
    memset(&G, 0, sizeof(G));
    /* cache-line aligned: the pixel loops stream through these and the PPU pays dearly for split lines */
    G.color = (u32 *)memalign(128, EFB_W * EFB_H * 4);
    G.depth = (float *)memalign(128, EFB_W * EFB_H * 4);
    G.front = (u32 *)memalign(128, EFB_W * EFB_H * 4);
    if (G.color) memset(G.color, 0, EFB_W * EFB_H * 4);
    if (G.depth) memset(G.depth, 0, EFB_W * EFB_H * 4);
    if (G.front) memset(G.front, 0, EFB_W * EFB_H * 4);
    G.depth_fill_z = -1.f; /* force the first depth fill */
    if (!G.color || !G.depth || !G.front) {
        OSReport("gx_soft: out of memory for the EFB\n");
        OSPanic("gx_soft.c", __LINE__, "EFB alloc failed\n");
    }
    for (i = 0; i < 10; i++) {
        mtx_identity34(G.pos_mtx[i]);
        mtx_identity34(G.nrm_mtx[i]);
        mtx_identity34(G.tex_mtx[i]);
    }
    G.proj[0][0] = G.proj[1][1] = 1.f;
    G.proj[2][2] = -1.f; /* arbitrary sane default */
    G.proj[3][3] = 1.f;
    G.vp[0] = G.vp[1] = 0.f;
    G.vp[2] = 640.f / GX_DIV;
    G.vp[3] = 480.f / GX_DIV;
    G.vp[4] = 0.f;
    G.vp[5] = 1.f;
    G.sc[0] = 0; G.sc[1] = 0; G.sc[2] = 640 / GX_DIV; G.sc[3] = 480 / GX_DIV;
    for (i = 0; i < 4; i++) {
        G.chan[i].mat_src = GX_SRC_REG;
        G.chan[i].amb_src = GX_SRC_REG;
    }
    for (i = 0; i < 2; i++) {
        G.mat[i][0] = G.mat[i][1] = G.mat[i][2] = G.mat[i][3] = 1.f;
    }
    G.nchans = 1;
    G.ntexgens = 1;
    G.ntev = 1;
    G.st[0].ci[0] = GX_CC_ZERO; G.st[0].ci[1] = GX_CC_ZERO; G.st[0].ci[2] = GX_CC_ZERO; G.st[0].ci[3] = GX_CC_RASC;
    G.st[0].ai[0] = GX_CA_ZERO; G.st[0].ai[1] = GX_CA_ZERO; G.st[0].ai[2] = GX_CA_ZERO; G.st[0].ai[3] = GX_CA_RASA;
    G.st[0].c_scale = G.st[0].a_scale = GX_CS_SCALE_1;
    G.st[0].c_clamp = G.st[0].a_clamp = 1;
    G.st[0].coord = 0xFF; G.st[0].map = 0xFF; G.st[0].color = GX_COLOR0A0;
    G.cull = GX_CULL_BACK;
    G.z_en = 1; G.z_func = GX_LEQUAL; G.z_upd = 1;
    G.bl_type = GX_BM_NONE;
    G.col_upd = G.a_upd = 1;
    G.ac0 = G.ac1 = GX_ALWAYS;
    G.aop = GX_AOP_AND;
    G.clear_z = 1.f;
    G.copy_w = 640 / GX_DIV;
    G.copy_h = 480 / GX_DIV;
    G.copy_h_real = 480;
    G.inited = 1;
    efb_clear();
}

/* ------------------------------------------------------------------------------------------------ textures */
typedef struct {
    u64 key_hash;
    const u8 *ptr;
    u16 w, h;
    u8 fmt;
    u32 tlut, tlut_gen;
    Tex tex;
    int used;
} TexCache;

#define TEXCACHE_N 512
#define TEXCACHE_MAX_BYTES (48u * 1024 * 1024)
static TexCache tcache[TEXCACHE_N];
static int tcache_next;
static u32 tcache_bytes;

static u64 sample_hash(const u8 *p, u32 size);
typedef struct { u32 *rgba; int n; u32 gen; u64 hash; } TlutSlot;
static TlutSlot tluts[32];

static u8 e5to8(u32 v) { return (u8)((v << 3) | (v >> 2)); }
static u8 e6to8(u32 v) { return (u8)((v << 2) | (v >> 4)); }
static u32 pack(u8 r, u8 g, u8 b, u8 a) { return ((u32)a << 24) | ((u32)r << 16) | ((u32)g << 8) | b; }
static u16 rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

static u32 rgb565(u16 v) { return pack(e5to8(v >> 11), e6to8((v >> 5) & 63), e5to8(v & 31), 255); }

static u32 rgb5a3(u16 v)
{
    if (v & 0x8000) return pack(e5to8((v >> 10) & 31), e5to8((v >> 5) & 31), e5to8(v & 31), 255);
    return pack(((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 7) * 255 / 7);
}

static void tlut_entry_convert(u32 *dst, const u8 *src, int n, int fmt)
{
    int i;
    for (i = 0; i < n; i++) {
        u16 v = rd16(src + i * 2);
        if (fmt == GX_TL_IA8) dst[i] = pack(v & 255, v & 255, v & 255, v >> 8);
        else if (fmt == GX_TL_RGB565) dst[i] = rgb565(v);
        else dst[i] = rgb5a3(v);
    }
}

static void blk_dims(int fmt, int *bw, int *bh)
{
    switch (fmt) {
        case GX_TF_I4: case GX_TF_C4: *bw = 8; *bh = 8; break;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: *bw = 8; *bh = 4; break;
        case GX_TF_CMPR: *bw = 8; *bh = 8; break;
        default: *bw = 4; *bh = 4; break; /* IA8 RGB565 RGB5A3 RGBA8 C14X2 */
    }
}

static u32 tex_bytes(int w, int h, int fmt)
{
    int bw, bh;
    u32 nbx, nby;
    blk_dims(fmt, &bw, &bh);
    nbx = (w + bw - 1) / bw;
    nby = (h + bh - 1) / bh;
    return nbx * nby * (fmt == GX_TF_RGBA8 ? 64 : 32);
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, u8 mipmap, u8 max_lod)
{
    u32 total = 0, w = width, h = height;
    int lod;
    for (lod = 0; lod <= (mipmap ? max_lod : 0); lod++) {
        total += tex_bytes(w ? w : 1, h ? h : 1, format);
        if (w > 1) w >>= 1;
        if (h > 1) h >>= 1;
    }
    return (total + 31) & ~31u;
}

static void decode_tex(Tex *t, const u8 *src, int w, int h, int fmt, int tlut)
{
    int bw, bh, bx, by, x, y;
    int nbx, nby;
    const TlutSlot *pal = (tlut >= 0 && tlut < 32) ? &tluts[tlut] : NULL;
    const u8 *p = src;
    blk_dims(fmt, &bw, &bh);
    nbx = (w + bw - 1) / bw;
    nby = (h + bh - 1) / bh;
    t->w = w;
    t->h = h;
    t->rgba = (u32 *)calloc((size_t)w * h, 4);
    if (!t->rgba) { OSReport("gx_soft: texture alloc failed\n"); return; }
#define PUT(px, py, val) do { int _x = (px), _y = (py); if (_x < w && _y < h) t->rgba[_y * w + _x] = (val); } while (0)
#define PAL(i) ((pal && pal->rgba && (int)(i) < pal->n) ? pal->rgba[(i)] : 0u)
    for (by = 0; by < nby; by++) {
        for (bx = 0; bx < nbx; bx++) {
            int ox = bx * bw, oy = by * bh;
            switch (fmt) {
                case GX_TF_I4:
                    for (y = 0; y < 8; y++) for (x = 0; x < 8; x += 2) {
                        u8 b = *p++;
                        u8 i0 = (b >> 4) * 17, i1 = (b & 15) * 17;
                        PUT(ox + x, oy + y, pack(i0, i0, i0, i0));
                        PUT(ox + x + 1, oy + y, pack(i1, i1, i1, i1));
                    }
                    break;
                case GX_TF_I8:
                    for (y = 0; y < 4; y++) for (x = 0; x < 8; x++) { u8 i = *p++; PUT(ox + x, oy + y, pack(i, i, i, i)); }
                    break;
                case GX_TF_IA4:
                    for (y = 0; y < 4; y++) for (x = 0; x < 8; x++) {
                        u8 b = *p++;
                        u8 a = (b >> 4) * 17, i = (b & 15) * 17;
                        PUT(ox + x, oy + y, pack(i, i, i, a));
                    }
                    break;
                case GX_TF_IA8:
                    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
                        u8 a = p[0], i = p[1];
                        p += 2;
                        PUT(ox + x, oy + y, pack(i, i, i, a));
                    }
                    break;
                case GX_TF_RGB565:
                    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) { PUT(ox + x, oy + y, rgb565(rd16(p))); p += 2; }
                    break;
                case GX_TF_RGB5A3:
                    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) { PUT(ox + x, oy + y, rgb5a3(rd16(p))); p += 2; }
                    break;
                case GX_TF_RGBA8:
                    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) {
                        int k = y * 4 + x;
                        PUT(ox + x, oy + y, pack(p[k * 2 + 1], p[32 + k * 2], p[32 + k * 2 + 1], p[k * 2]));
                    }
                    p += 64;
                    break;
                case GX_TF_C4:
                    for (y = 0; y < 8; y++) for (x = 0; x < 8; x += 2) {
                        u8 b = *p++;
                        PUT(ox + x, oy + y, PAL(b >> 4));
                        PUT(ox + x + 1, oy + y, PAL(b & 15));
                    }
                    break;
                case GX_TF_C8:
                    for (y = 0; y < 4; y++) for (x = 0; x < 8; x++) { PUT(ox + x, oy + y, PAL(*p)); p++; }
                    break;
                case 10: /* GX_TF_C14X2 */
                    for (y = 0; y < 4; y++) for (x = 0; x < 4; x++) { PUT(ox + x, oy + y, PAL(rd16(p) & 0x3FFF)); p += 2; }
                    break;
                case GX_TF_CMPR: {
                    int sb;
                    for (sb = 0; sb < 4; sb++) {
                        int sx = ox + (sb & 1) * 4, sy = oy + (sb >> 1) * 4;
                        u16 c0 = rd16(p), c1 = rd16(p + 2);
                        u32 col[4];
                        int r;
                        u32 a0 = rgb565(c0), a1 = rgb565(c1);
                        col[0] = a0;
                        col[1] = a1;
                        if (c0 > c1) {
                            col[2] = pack((2 * ((a0 >> 16) & 255) + ((a1 >> 16) & 255)) / 3, (2 * ((a0 >> 8) & 255) + ((a1 >> 8) & 255)) / 3,
                                          (2 * (a0 & 255) + (a1 & 255)) / 3, 255);
                            col[3] = pack((((a0 >> 16) & 255) + 2 * ((a1 >> 16) & 255)) / 3, (((a0 >> 8) & 255) + 2 * ((a1 >> 8) & 255)) / 3,
                                          ((a0 & 255) + 2 * (a1 & 255)) / 3, 255);
                        } else {
                            col[2] = pack((((a0 >> 16) & 255) + ((a1 >> 16) & 255)) / 2, (((a0 >> 8) & 255) + ((a1 >> 8) & 255)) / 2,
                                          ((a0 & 255) + (a1 & 255)) / 2, 255);
                            col[3] = 0;
                        }
                        for (r = 0; r < 4; r++) {
                            u8 bits = p[4 + r];
                            for (x = 0; x < 4; x++) PUT(sx + x, sy + r, col[(bits >> (6 - 2 * x)) & 3]);
                        }
                        p += 8;
                    }
                    break;
                }
                default:
                    p += 32;
                    break;
            }
        }
    }
#undef PUT
#undef PAL
}

static u64 sample_hash(const u8 *p, u32 size)
{
    u64 h = 1469598103934665603ull;
    u32 i, step = size / 256;
    if (step == 0) step = 1;
    for (i = 0; i < size; i += step) { h ^= p[i]; h *= 1099511628211ull; }
    for (i = 0; i < 64 && i < size; i++) { h ^= p[i]; h *= 1099511628211ull; h ^= p[size - 1 - i]; h *= 1099511628211ull; }
    return h ^ size;
}

static Tex *tex_lookup(const TexObjD *o)
{
    const u8 *ptr = (const u8 *)(uintptr_t)(((u64)o->ptr_hi << 32) | o->ptr_lo);
    u32 size, gen = 0;
    u64 hash;
    int i, tl = -1;
    TexCache *c;
    if (!ptr || !o->w || !o->h) return NULL;
    size = tex_bytes(o->w, o->h, o->fmt);
    hash = sample_hash(ptr, size);
    if (o->fmt == GX_TF_C4 || o->fmt == GX_TF_C8 || o->fmt == 10) {
        tl = (int)(o->tlut & 31);
        gen = tluts[tl].gen;
    }
    for (i = 0; i < TEXCACHE_N; i++) {
        c = &tcache[i];
        if (c->used && c->ptr == ptr && c->w == o->w && c->h == o->h && c->fmt == o->fmt && c->tlut == (u32)(tl < 0 ? 0 : tl) &&
            c->tlut_gen == gen && c->key_hash == hash)
            return &c->tex;
    }
    /* keep the decoded-texture cache under its memory budget (round-robin eviction) */
    for (i = 0; i < TEXCACHE_N && tcache_bytes + (u32)o->w * o->h * 4 > TEXCACHE_MAX_BYTES; i++) {
        TexCache *e = &tcache[tcache_next];
        tcache_next = (tcache_next + 1) % TEXCACHE_N;
        if (e->used) { tcache_bytes -= (u32)e->tex.w * e->tex.h * 4; free(e->tex.rgba); memset(e, 0, sizeof(*e)); }
    }
    c = &tcache[tcache_next];
    tcache_next = (tcache_next + 1) % TEXCACHE_N;
    if (c->used) { tcache_bytes -= (u32)c->tex.w * c->tex.h * 4; free(c->tex.rgba); }
    memset(c, 0, sizeof(*c));
    c->used = 1;
    c->ptr = ptr;
    c->w = o->w;
    c->h = o->h;
    c->fmt = o->fmt;
    c->tlut = tl < 0 ? 0 : tl;
    c->tlut_gen = gen;
    c->key_hash = hash;
    { u64 d0 = now_us(); decode_tex(&c->tex, ptr, o->w, o->h, o->fmt, tl); G_stats.t_decode += now_us() - d0; }
    if (!c->tex.rgba) { c->used = 0; return NULL; }
    c->tex.wmask = ((o->w & (o->w - 1)) == 0) ? o->w - 1 : -1;
    c->tex.hmask = ((o->h & (o->h - 1)) == 0) ? o->h - 1 : -1;
    G_stats.tex_decodes++;
    tcache_bytes += (u32)o->w * o->h * 4;
    return &c->tex;
}

void GXInitTexObj(GXTexObj *obj, void *image_ptr, u16 width, u16 height, GXTexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap)
{
    TexObjD *o = (TexObjD *)obj;
    memset(obj, 0, sizeof(*obj));
    o->ptr_lo = (u32)(uintptr_t)image_ptr;
    o->ptr_hi = (u32)((u64)(uintptr_t)image_ptr >> 32);
    o->w = width; o->h = height; o->fmt = format; o->ws = wrap_s; o->wt = wrap_t; o->mip = mipmap;
    o->minf = GX_LINEAR; o->magf = GX_LINEAR;
}

void GXInitTexObjCI(GXTexObj *obj, void *image_ptr, u16 width, u16 height, GXCITexFmt format, GXTexWrapMode wrap_s, GXTexWrapMode wrap_t, u8 mipmap, u32 tlut_name)
{
    GXInitTexObj(obj, image_ptr, width, height, (GXTexFmt)format, wrap_s, wrap_t, mipmap);
    ((TexObjD *)obj)->tlut = tlut_name;
}

void GXInitTexObjLOD(GXTexObj *obj, GXTexFilter min_filt, GXTexFilter mag_filt, f32 min_lod, f32 max_lod, f32 lod_bias, GXBool bias_clamp, GXBool do_edge_lod, GXAnisotropy max_aniso)
{
    TexObjD *o = (TexObjD *)obj;
    o->minf = min_filt;
    o->magf = mag_filt;
}

void GXInitTlutObj(GXTlutObj *tlut_obj, void *lut, GXTlutFmt fmt, u16 n_entries)
{
    TlutObjD *t = (TlutObjD *)tlut_obj;
    memset(tlut_obj, 0, sizeof(*tlut_obj));
    t->ptr_lo = (u32)(uintptr_t)lut;
    t->ptr_hi = (u32)((u64)(uintptr_t)lut >> 32);
    t->n = n_entries;
    t->fmt = fmt;
}

void GXLoadTlut(GXTlutObj *tlut_obj, u32 tlut_name)
{
    TlutObjD *t = (TlutObjD *)tlut_obj;
    const u8 *src = (const u8 *)(uintptr_t)(((u64)t->ptr_hi << 32) | t->ptr_lo);
    TlutSlot *s = &tluts[tlut_name & 31];
    u32 n = t->n ? t->n : 1;
    u64 h;
    if (!src) return;
    /* The game loads a palette before every paletted draw. Only touch the decoded textures when it really changed,
       otherwise every CI texture would be re-decoded on every draw. */
    h = sample_hash(src, n * 2) ^ ((u64)t->fmt << 56) ^ n;
    if (s->rgba && s->n == (int)t->n && s->hash == h) return;
    free(s->rgba);
    s->rgba = (u32 *)calloc(n, 4);
    s->n = t->n;
    s->hash = h;
    s->gen++;
    if (s->rgba) tlut_entry_convert(s->rgba, src, t->n, t->fmt);
}

void GXLoadTexObj(GXTexObj *obj, GXTexMapID id)
{
    gx_ensure_init();
    if ((unsigned)id < 8) {
        TexObjD *o = (TexObjD *)obj;
        Tex *t = tex_lookup(o);
        if (t) { t->ws = o->ws; t->wt = o->wt; }
        G.bound[id] = t;
        /* filter selection is read from the object at load time */
        if (t) { t->w = o->w; t->h = o->h; }
        /* store linear flag in the high bit of ws (cheap, avoids a second struct) */
        if (t) { t->ws = (o->ws & 3) | ((o->magf == GX_LINEAR || o->magf == 3 || o->magf == 5) ? 0x80 : 0); }
    }
}

void GXInvalidateTexAll(void) {}

static int wrap_i(int i, int n, int mode)
{
    if (mode == GX_REPEAT) { i %= n; return i < 0 ? i + n : i; }
    if (mode == GX_MIRROR) {
        int p = n * 2;
        i %= p;
        if (i < 0) i += p;
        return i < n ? i : p - 1 - i;
    }
    return i < 0 ? 0 : (i >= n ? n - 1 : i);
}

static void sample_tex(const Tex *t, float s, float tt, float out[4])
{
    float u = s * t->w, v = tt * t->h;
    int ws = t->ws & 3, wt = t->wt & 3;
    u32 c;
    if (t->ws & 0x80) {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
        float ax = fu - x0, ay = fv - y0;
        int x1 = wrap_i(x0 + 1, t->w, ws), y1 = wrap_i(y0 + 1, t->h, wt);
        u32 c00, c10, c01, c11;
        int k;
        x0 = wrap_i(x0, t->w, ws);
        y0 = wrap_i(y0, t->h, wt);
        c00 = t->rgba[y0 * t->w + x0];
        c10 = t->rgba[y0 * t->w + x1];
        c01 = t->rgba[y1 * t->w + x0];
        c11 = t->rgba[y1 * t->w + x1];
        for (k = 0; k < 4; k++) {
            int sh = k == 3 ? 24 : (16 - 8 * k);
            float a = (float)((c00 >> sh) & 255), b = (float)((c10 >> sh) & 255);
            float cc = (float)((c01 >> sh) & 255), d = (float)((c11 >> sh) & 255);
            out[k] = ((a + (b - a) * ax) * (1 - ay) + (cc + (d - cc) * ax) * ay) / 255.f;
        }
        return;
    }
    c = t->rgba[wrap_i((int)floorf(v), t->h, wt) * t->w + wrap_i((int)floorf(u), t->w, ws)];
    out[0] = ((c >> 16) & 255) / 255.f;
    out[1] = ((c >> 8) & 255) / 255.f;
    out[2] = (c & 255) / 255.f;
    out[3] = (c >> 24) / 255.f;
}

/* ------------------------------------------------------------------------------------------------ simple state setters */
GXFifoObj *GXInit(void *base, u32 size)
{
    gx_ensure_init();
    return &fifo_dummy;
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz)
{
    gx_ensure_init();
    G.vp[0] = left / GX_DIV; G.vp[1] = top / GX_DIV; G.vp[2] = wd / GX_DIV; G.vp[3] = ht / GX_DIV; G.vp[4] = nearz; G.vp[5] = farz;
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz, u32 field) { GXSetViewport(left, top, wd, ht, nearz, farz); }

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht)
{
    gx_ensure_init();
    G.sc[0] = left / GX_DIV; G.sc[1] = top / GX_DIV; G.sc[2] = (wd + GX_DIV - 1) / GX_DIV; G.sc[3] = (ht + GX_DIV - 1) / GX_DIV;
}

void GXSetProjection(const void *mtx, GXProjectionType type)
{
    const float *m = (const float *)mtx; /* Mtx44, row-major */
    gx_ensure_init(); vepoch++;
    memcpy(G.proj, m, sizeof(G.proj));
}

void GXLoadPosMtxImm(const void *mtx, u32 id)
{
    gx_ensure_init(); vepoch++;
    if (id / 3 < 10) memcpy(G.pos_mtx[id / 3], mtx, 12 * sizeof(float));
}

void GXLoadNrmMtxImm(const void *mtx, u32 id)
{
    gx_ensure_init(); vepoch++;
    if (id / 3 < 10) memcpy(G.nrm_mtx[id / 3], mtx, 12 * sizeof(float));
}

void GXLoadTexMtxImm(const void *mtx, u32 id, GXTexMtxType type)
{
    gx_ensure_init(); vepoch++;
    if (id >= GX_TEXMTX0 && (id - GX_TEXMTX0) / 3 < 10) {
        float m[12];
        memset(m, 0, sizeof(m));
        if (type == GX_MTX2x4) memcpy(m, mtx, 8 * sizeof(float));
        else memcpy(m, mtx, 12 * sizeof(float));
        memcpy(G.tex_mtx[(id - GX_TEXMTX0) / 3], m, sizeof(m));
    }
}

void GXSetCurrentMtx(u32 id) { gx_ensure_init(); vepoch++; G.cur_mtx = (id / 3 < 10) ? id / 3 : 0; }

void GXClearVtxDesc(void) { gx_ensure_init(); vepoch++; memset(G.vdesc, 0, sizeof(G.vdesc)); }
void GXSetVtxDesc(GXAttr attr, GXAttrType type) { gx_ensure_init(); vepoch++; if (attr <= GX_VA_MAX_ATTR) G.vdesc[attr] = type; }

void GXSetVtxAttrFmt(GXVtxFmt vtxfmt, GXAttr attr, GXCompCnt cnt, GXCompType type, u8 frac)
{
    gx_ensure_init(); vepoch++;
    if (vtxfmt < 8 && attr <= GX_VA_MAX_ATTR) { G.vfmt[vtxfmt][attr].cnt = cnt; G.vfmt[vtxfmt][attr].type = type; G.vfmt[vtxfmt][attr].frac = frac; }
}

void GXSetArray(GXAttr attr, const void *data, u32 size, u8 stride)
{
    gx_ensure_init(); vepoch++;
    if (attr <= GX_VA_MAX_ATTR) { G.arr[attr] = (const u8 *)data; G.arr_stride[attr] = stride; }
}

void GXSetNumChans(u8 n) { gx_ensure_init(); vepoch++; G.nchans = n; }
void GXSetNumTexGens(u8 n) { gx_ensure_init(); vepoch++; G.ntexgens = n; }
void GXSetNumTevStages(u8 n) { gx_ensure_init(); G.ntev = n > MAXTEV ? MAXTEV : n; }

static void color_to_f4(float o[4], GXColor c) { o[0] = c.r / 255.f; o[1] = c.g / 255.f; o[2] = c.b / 255.f; o[3] = c.a / 255.f; }

void GXSetChanMatColor(GXChannelID chan, GXColor c)
{
    float f[4];
    int ch = (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) ? 1 : 0;
    gx_ensure_init(); vepoch++;
    color_to_f4(f, c);
    if (chan == GX_COLOR0 || chan == GX_COLOR1 || chan == GX_COLOR0A0 || chan == GX_COLOR1A1) memcpy(G.mat[ch], f, 3 * sizeof(float));
    if (chan == GX_ALPHA0 || chan == GX_ALPHA1 || chan == GX_COLOR0A0 || chan == GX_COLOR1A1) G.mat[ch][3] = f[3];
}

void GXSetChanAmbColor(GXChannelID chan, GXColor c)
{
    float f[4];
    int ch = (chan == GX_COLOR1 || chan == GX_ALPHA1 || chan == GX_COLOR1A1) ? 1 : 0;
    gx_ensure_init(); vepoch++;
    color_to_f4(f, c);
    if (chan == GX_COLOR0 || chan == GX_COLOR1 || chan == GX_COLOR0A0 || chan == GX_COLOR1A1) memcpy(G.amb[ch], f, 3 * sizeof(float));
    if (chan == GX_ALPHA0 || chan == GX_ALPHA1 || chan == GX_COLOR0A0 || chan == GX_COLOR1A1) G.amb[ch][3] = f[3];
}

void GXSetChanCtrl(GXChannelID chan, GXBool enable, GXColorSrc amb_src, GXColorSrc mat_src, u32 light_mask, GXDiffuseFn diff_fn, GXAttnFn attn_fn)
{
    int idx[2], n = 0, i;
    gx_ensure_init(); vepoch++;
    switch (chan) {
        case GX_COLOR0: idx[n++] = 0; break;
        case GX_COLOR1: idx[n++] = 1; break;
        case GX_ALPHA0: idx[n++] = 2; break;
        case GX_ALPHA1: idx[n++] = 3; break;
        case GX_COLOR0A0: idx[n++] = 0; idx[n++] = 2; break;
        case GX_COLOR1A1: idx[n++] = 1; idx[n++] = 3; break;
        default: return;
    }
    for (i = 0; i < n; i++) {
        ChanCtrl *c = &G.chan[idx[i]];
        c->enable = enable; c->amb_src = amb_src; c->mat_src = mat_src; c->light_mask = light_mask; c->diff_fn = diff_fn; c->attn_fn = attn_fn;
    }
}

/* lights */
void GXInitLightPos(GXLightObj *o, f32 x, f32 y, f32 z) { LightD *l = (LightD *)o; l->pos[0] = x; l->pos[1] = y; l->pos[2] = z; }
void GXInitLightDir(GXLightObj *o, f32 nx, f32 ny, f32 nz) { LightD *l = (LightD *)o; l->dir[0] = nx; l->dir[1] = ny; l->dir[2] = nz; }
void GXInitLightColor(GXLightObj *o, GXColor c) { ((LightD *)o)->color = c; }
void GXInitLightAttn(GXLightObj *o, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
    LightD *l = (LightD *)o;
    l->a[0] = a0; l->a[1] = a1; l->a[2] = a2; l->k[0] = k0; l->k[1] = k1; l->k[2] = k2;
}
void GXInitLightAttnK(GXLightObj *o, f32 k0, f32 k1, f32 k2) { LightD *l = (LightD *)o; l->k[0] = k0; l->k[1] = k1; l->k[2] = k2; }

void GXInitLightSpot(GXLightObj *o, f32 cutoff, GXSpotFn fn)
{
    LightD *l = (LightD *)o;
    float c = cosf(cutoff * 3.14159265f / 180.f), d;
    switch (fn) {
        case GX_SP_FLAT: l->a[0] = -1000.f * c; l->a[1] = 1000.f; l->a[2] = 0; break;
        case GX_SP_COS: d = 1.f - c; l->a[0] = -c / d; l->a[1] = 1.f / d; l->a[2] = 0; break;
        case GX_SP_COS2: d = 1.f - c; l->a[0] = 0; l->a[1] = -c / d; l->a[2] = 1.f / d; break;
        case GX_SP_SHARP: d = (1.f - c) * (1.f - c); l->a[0] = c * (c - 2.f) / d; l->a[1] = 2.f / d; l->a[2] = -1.f / d; break;
        case GX_SP_RING1: d = (1.f - c) * (1.f - c); l->a[0] = -4.f * c / d; l->a[1] = 4.f * (1.f + c) / d; l->a[2] = -4.f / d; break;
        case GX_SP_RING2: d = (1.f - c) * (1.f - c); l->a[0] = 1.f - 2.f * c * c / d; l->a[1] = 4.f * c / d; l->a[2] = -2.f / d; break;
        default: l->a[0] = 1; l->a[1] = 0; l->a[2] = 0; break;
    }
}

void GXInitLightDistAttn(GXLightObj *o, f32 ref_distance, f32 ref_brightness, GXDistAttnFn fn)
{
    LightD *l = (LightD *)o;
    float b = ref_brightness, r = ref_distance;
    if (r < 0.f || b < 0.f || b >= 1.f) fn = GX_DA_OFF;
    switch (fn) {
        case GX_DA_GENTLE: l->k[0] = 1.f; l->k[1] = (1.f - b) / (b * r); l->k[2] = 0; break;
        case GX_DA_MEDIUM: l->k[0] = 1.f; l->k[1] = 0.5f * (1.f - b) / (b * r); l->k[2] = 0.5f * (1.f - b) / (b * r * r); break;
        case GX_DA_STEEP: l->k[0] = 1.f; l->k[1] = 0; l->k[2] = (1.f - b) / (b * r * r); break;
        default: l->k[0] = 1.f; l->k[1] = 0; l->k[2] = 0; break;
    }
}

void GXInitSpecularDir(GXLightObj *o, float nx, float ny, float nz)
{
    LightD *l = (LightD *)o;
    l->dir[0] = -nx; l->dir[1] = -ny; l->dir[2] = -nz;
    l->pos[0] = nx * -1048576.f; l->pos[1] = ny * -1048576.f; l->pos[2] = nz * -1048576.f;
}

void GXLoadLightObjImm(GXLightObj *o, GXLightID light)
{
    int i;
    gx_ensure_init(); vepoch++;
    for (i = 0; i < 8; i++) if (light & (1u << i)) { memcpy(&G.light[i], o, sizeof(LightD)); break; }
}

/* texgen */
void GXSetTexCoordGen2(GXTexCoordID dst, GXTexGenType func, GXTexGenSrc src, u32 mtx, GXBool normalize, u32 postmtx)
{
    gx_ensure_init(); vepoch++;
    if (dst < 8) { G.texgen[dst].func = func; G.texgen[dst].src = src; G.texgen[dst].mtx = mtx; G.texgen[dst].post = postmtx; }
}

/* TEV */
void GXSetTevOrder(GXTevStageID s, GXTexCoordID coord, GXTexMapID map, GXChannelID color)
{
    gx_ensure_init();
    if (s < MAXTEV) { G.st[s].coord = coord; G.st[s].map = map; G.st[s].color = color; }
}

void GXSetTevColorIn(GXTevStageID s, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d)
{
    gx_ensure_init();
    if (s < MAXTEV) { G.st[s].ci[0] = a; G.st[s].ci[1] = b; G.st[s].ci[2] = c; G.st[s].ci[3] = d; }
}

void GXSetTevAlphaIn(GXTevStageID s, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{
    gx_ensure_init();
    if (s < MAXTEV) { G.st[s].ai[0] = a; G.st[s].ai[1] = b; G.st[s].ai[2] = c; G.st[s].ai[3] = d; }
}

void GXSetTevColorOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out)
{
    gx_ensure_init();
    if (s < MAXTEV) { G.st[s].c_op = op; G.st[s].c_bias = bias; G.st[s].c_scale = scale; G.st[s].c_clamp = clamp; G.st[s].c_out = out; }
}

void GXSetTevAlphaOp(GXTevStageID s, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool clamp, GXTevRegID out)
{
    gx_ensure_init();
    if (s < MAXTEV) { G.st[s].a_op = op; G.st[s].a_bias = bias; G.st[s].a_scale = scale; G.st[s].a_clamp = clamp; G.st[s].a_out = out; }
}

void GXSetTevOp(GXTevStageID id, GXTevMode mode)
{
    u8 cc = (id == GX_TEVSTAGE0) ? GX_CC_RASC : GX_CC_CPREV;
    u8 ca = (id == GX_TEVSTAGE0) ? GX_CA_RASA : GX_CA_APREV;
    switch (mode) {
        case GX_MODULATE:
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_TEXC, cc, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, ca, GX_CA_ZERO);
            break;
        case GX_DECAL:
            GXSetTevColorIn(id, cc, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ca);
            break;
        case GX_BLEND:
            GXSetTevColorIn(id, cc, GX_CC_ONE, GX_CC_TEXC, GX_CC_ZERO);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_TEXA, ca, GX_CA_ZERO);
            break;
        case GX_REPLACE:
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
            break;
        default: /* PASSCLR */
            GXSetTevColorIn(id, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, cc);
            GXSetTevAlphaIn(id, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, ca);
            break;
    }
    GXSetTevColorOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(id, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GXSetTevColor(GXTevRegID id, GXColor c) { gx_ensure_init(); if (id < 4) color_to_f4(G.reg[id], c); }
void GXSetTevColorS10(GXTevRegID id, GXColorS10 c)
{
    gx_ensure_init();
    if (id < 4) { G.reg[id][0] = c.r / 255.f; G.reg[id][1] = c.g / 255.f; G.reg[id][2] = c.b / 255.f; G.reg[id][3] = c.a / 255.f; }
}
void GXSetTevKColor(GXTevKColorID id, GXColor c) { gx_ensure_init(); if (id < 4) color_to_f4(G.kcol[id], c); }
void GXSetTevKColorSel(GXTevStageID s, GXTevKColorSel sel) { gx_ensure_init(); if (s < MAXTEV) G.st[s].kcsel = sel; }
void GXSetTevKAlphaSel(GXTevStageID s, GXTevKAlphaSel sel) { gx_ensure_init(); if (s < MAXTEV) G.st[s].kasel = sel; }

/* raster state */
void GXSetCullMode(GXCullMode m) { gx_ensure_init(); G.cull = m; }
void GXSetZMode(GXBool en, GXCompare func, GXBool upd) { gx_ensure_init(); G.z_en = en; G.z_func = func; G.z_upd = upd; }
void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst, GXLogicOp op) { gx_ensure_init(); G.bl_type = type; G.bl_src = src; G.bl_dst = dst; }
void GXSetColorUpdate(GXBool e) { gx_ensure_init(); G.col_upd = e; }
void GXSetAlphaUpdate(GXBool e) { gx_ensure_init(); G.a_upd = e; }
void GXSetAlphaCompare(GXCompare c0, u8 r0, GXAlphaOp op, GXCompare c1, u8 r1) { gx_ensure_init(); G.ac0 = c0; G.ar0 = r0; G.aop = op; G.ac1 = c1; G.ar1 = r1; }

/* ------------------------------------------------------------------------------------------------ vertex assembly */
typedef struct {
    float pos[3], nrm[3], clr[2][4], tex[8][2];
} Raw;

typedef struct {
    float cx, cy, cz, cw;
    float col[2][4];
    float tc[8][2];
    float sx, sy, sz, iw;
} PV;

static const u8 vorder[12] = { GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_CLR1, GX_VA_TEX0, GX_VA_TEX1, GX_VA_TEX2, GX_VA_TEX3, GX_VA_TEX4, GX_VA_TEX5, GX_VA_TEX6, GX_VA_TEX7 };

static struct {
    int in_prim, prim, fmt, slot, count, parity;
    Raw raw;
    PV buf[4];
    int nbuf;
    u64 vkey;
    int all_idx, cur_indexed;
    int recording;
    u8 *dl;
    u32 dl_cap, dl_len;
    int dl_overflow;
} A;

static int next_slot(int from)
{
    int i;
    for (i = from; i < 12; i++) if (G.vdesc[vorder[i]] != GX_NONE) return i;
    return 12;
}

static void raster_tri(PV *v0, PV *v1, PV *v2);

static void tri_clip(const PV *a, const PV *b, const PV *c);

static void light_term(int ch, int comp_alpha, const Raw *r, const float vp[3], const float n[3], float out[4])
{
    /* out: lighting factor (rgb) / alpha for one channel */
    const ChanCtrl *cc = &G.chan[ch + (comp_alpha ? 2 : 0)];
    int i;
    float acc[4];
    if (cc->amb_src == GX_SRC_VTX) memcpy(acc, r->clr[ch], sizeof(acc));
    else memcpy(acc, G.amb[ch], sizeof(acc));
    for (i = 0; i < 8; i++) {
        const LightD *l;
        float L[3], d2, len, att = 1.f, dif;
        if (!(cc->light_mask & (1u << i))) continue;
        l = &G.light[i];
        L[0] = l->pos[0] - vp[0]; L[1] = l->pos[1] - vp[1]; L[2] = l->pos[2] - vp[2];
        d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2];
        len = sqrtf(d2);
        if (len > 1e-6f) { L[0] /= len; L[1] /= len; L[2] /= len; }
        if (cc->attn_fn == GX_AF_SPOT) {
            float cosa = -(L[0] * l->dir[0] + L[1] * l->dir[1] + L[2] * l->dir[2]);
            float num, den;
            if (cosa < 0.f) cosa = 0.f;
            num = l->a[0] + l->a[1] * cosa + l->a[2] * cosa * cosa;
            den = l->k[0] + l->k[1] * len + l->k[2] * d2;
            att = den != 0.f ? clampf(num / den, 0.f, 1e4f) : 0.f;
        }
        dif = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        if (cc->diff_fn == GX_DF_NONE) dif = 1.f;
        else if (cc->diff_fn == GX_DF_CLAMP && dif < 0.f) dif = 0.f;
        {
            float lc[4] = { l->color.r / 255.f, l->color.g / 255.f, l->color.b / 255.f, l->color.a / 255.f };
            int k;
            for (k = 0; k < 4; k++) acc[k] += lc[k] * dif * att;
        }
    }
    {
        int k;
        for (k = 0; k < 4; k++) out[k] = sat(acc[k]);
    }
}

static void transform_vertex(const Raw *r, PV *o)
{
    const float *pm = G.pos_mtx[G.cur_mtx], *nm = G.nrm_mtx[G.cur_mtx];
    float vp[3], n[3], nl;
    int i, c;
    vp[0] = pm[0] * r->pos[0] + pm[1] * r->pos[1] + pm[2] * r->pos[2] + pm[3];
    vp[1] = pm[4] * r->pos[0] + pm[5] * r->pos[1] + pm[6] * r->pos[2] + pm[7];
    vp[2] = pm[8] * r->pos[0] + pm[9] * r->pos[1] + pm[10] * r->pos[2] + pm[11];
    o->cx = G.proj[0][0] * vp[0] + G.proj[0][1] * vp[1] + G.proj[0][2] * vp[2] + G.proj[0][3];
    o->cy = G.proj[1][0] * vp[0] + G.proj[1][1] * vp[1] + G.proj[1][2] * vp[2] + G.proj[1][3];
    o->cz = G.proj[2][0] * vp[0] + G.proj[2][1] * vp[1] + G.proj[2][2] * vp[2] + G.proj[2][3];
    o->cw = G.proj[3][0] * vp[0] + G.proj[3][1] * vp[1] + G.proj[3][2] * vp[2] + G.proj[3][3];
    n[0] = nm[0] * r->nrm[0] + nm[1] * r->nrm[1] + nm[2] * r->nrm[2];
    n[1] = nm[4] * r->nrm[0] + nm[5] * r->nrm[1] + nm[6] * r->nrm[2];
    n[2] = nm[8] * r->nrm[0] + nm[9] * r->nrm[1] + nm[10] * r->nrm[2];
    if (G.chan[0].enable || G.chan[1].enable || G.chan[2].enable || G.chan[3].enable) {
        nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl > 1e-8f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
    }
    /* colour channels */
    for (c = 0; c < 2; c++) {
        float rgb[4], lit[4];
        float mat[4];
        const ChanCtrl *cc = &G.chan[c], *ca = &G.chan[c + 2];
        if (c >= G.nchans) { memset(o->col[c], 0, sizeof(o->col[c])); continue; }
        for (i = 0; i < 3; i++) mat[i] = cc->mat_src == GX_SRC_VTX ? r->clr[c][i] : G.mat[c][i];
        mat[3] = ca->mat_src == GX_SRC_VTX ? r->clr[c][3] : G.mat[c][3];
        memcpy(rgb, mat, sizeof(rgb));
        if (cc->enable) {
            light_term(c, 0, r, vp, n, lit);
            for (i = 0; i < 3; i++) rgb[i] = mat[i] * lit[i];
        }
        if (ca->enable) {
            light_term(c, 1, r, vp, n, lit);
            rgb[3] = mat[3] * lit[3];
        }
        memcpy(o->col[c], rgb, sizeof(rgb));
    }
    /* texgen */
    for (i = 0; i < 8; i++) {
        const TexGen *tg = &G.texgen[i];
        float in[4], s, t;
        o->tc[i][0] = o->tc[i][1] = 0.f;
        if (i >= G.ntexgens) continue;
        if (tg->src == GX_TG_POS) { in[0] = r->pos[0]; in[1] = r->pos[1]; in[2] = r->pos[2]; }
        else if (tg->src == GX_TG_NRM) { in[0] = r->nrm[0]; in[1] = r->nrm[1]; in[2] = r->nrm[2]; }
        else { int tn = tg->src - GX_TG_TEX0; if (tn < 0 || tn > 7) tn = 0; in[0] = r->tex[tn][0]; in[1] = r->tex[tn][1]; in[2] = 1.f; }
        in[3] = 1.f;
        if (tg->mtx == GX_IDENTITY || tg->mtx < GX_TEXMTX0 || (tg->mtx - GX_TEXMTX0) / 3 >= 10) { s = in[0]; t = in[1]; }
        else {
            const float *m = G.tex_mtx[(tg->mtx - GX_TEXMTX0) / 3];
            s = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3] * in[3];
            t = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7] * in[3];
            if (tg->func == GX_TG_MTX3x4) {
                float q = m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11] * in[3];
                if (q != 0.f) { s /= q; t /= q; }
            }
        }
        o->tc[i][0] = s;
        o->tc[i][1] = t;
    }
}

static void push_vertex(const PV *v)
{
    switch (A.prim) {
        case GX_TRIANGLES:
            A.buf[A.nbuf++] = *v;
            if (A.nbuf == 3) { tri_clip(&A.buf[0], &A.buf[1], &A.buf[2]); A.nbuf = 0; }
            break;
        case GX_QUADS:
            A.buf[A.nbuf++] = *v;
            if (A.nbuf == 4) { tri_clip(&A.buf[0], &A.buf[1], &A.buf[2]); tri_clip(&A.buf[0], &A.buf[2], &A.buf[3]); A.nbuf = 0; }
            break;
        case GX_TRIANGLESTRIP:
            if (A.nbuf < 2) { A.buf[A.nbuf++] = *v; A.parity = 0; }
            else {
                if (A.parity == 0) tri_clip(&A.buf[0], &A.buf[1], v);
                else tri_clip(&A.buf[1], &A.buf[0], v);
                A.buf[0] = A.buf[1];
                A.buf[1] = *v;
                A.parity ^= 1;
            }
            break;
        case GX_TRIANGLEFAN:
            if (A.nbuf < 2) A.buf[A.nbuf++] = *v;
            else { tri_clip(&A.buf[0], &A.buf[1], v); A.buf[1] = *v; }
            break;
        default:
            break; /* lines/points not supported yet */
    }
}

extern int port_gx_no_vcache;
typedef struct { u64 key; u32 epoch; PV pv; } VCache;
static VCache vcache[256];

static void finish_vertex(void)
{
    if (A.all_idx && !port_gx_no_vcache) {
        /* all attributes came from indexed arrays: the transformed vertex is a pure function of the indices */
        VCache *e = &vcache[(u32)(A.vkey ^ (A.vkey >> 29) ^ (A.vkey >> 47)) & 255];
        if (e->epoch == vepoch && e->key == A.vkey) {
            G_stats.vc_hit++;
        } else {
            transform_vertex(&A.raw, &e->pv);
            e->key = A.vkey;
            e->epoch = vepoch;
            G_stats.vc_miss++;
        }
        push_vertex(&e->pv);
    } else {
        PV pv;
        transform_vertex(&A.raw, &pv);
        push_vertex(&pv);
    }
    A.vkey = (u64)A.fmt + 1469598103934665603ull;
    A.all_idx = 1;
    /* reset colours to white for the next vertex when no colour is supplied */
    A.raw.clr[0][0] = A.raw.clr[0][1] = A.raw.clr[0][2] = A.raw.clr[0][3] = 1.f;
    A.raw.clr[1][0] = A.raw.clr[1][1] = A.raw.clr[1][2] = A.raw.clr[1][3] = 1.f;
}

static void slot_done(void)
{
    A.slot = next_slot(A.slot + 1);
    if (A.slot >= 12) {
        finish_vertex();
        A.slot = next_slot(0);
    }
}

static u64 prim_t0;
static void begin_prim(int prim, int fmt)
{
    int i, j;
    prim_t0 = now_us();
    A.in_prim = 1; A.prim = prim; A.fmt = fmt & 7; A.nbuf = 0; A.parity = 0;
    A.vkey = (u64)A.fmt + 1469598103934665603ull;
    A.all_idx = 1;
    A.slot = next_slot(0);
    memset(&A.raw, 0, sizeof(A.raw));
    for (i = 0; i < 2; i++) for (j = 0; j < 4; j++) A.raw.clr[i][j] = 1.f;
}

static float comp_frac_scale(int attr, int type, int fixed_frac)
{
    int frac = fixed_frac >= 0 ? fixed_frac : G.vfmt[A.fmt][attr].frac;
    if (type == GX_F32) return 1.f;
    return 1.f / (float)(1 << (frac & 31));
}

static void put_floats(int n, const float *f)
{
    int attr, i;
    if (!A.in_prim || A.slot >= 12) return;
    if (!A.cur_indexed) A.all_idx = 0;
    attr = vorder[A.slot];
    if (attr == GX_VA_POS) { for (i = 0; i < 3; i++) A.raw.pos[i] = i < n ? f[i] : 0.f; }
    else if (attr == GX_VA_NRM) { for (i = 0; i < 3; i++) A.raw.nrm[i] = i < n ? f[i] : 0.f; }
    else if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) { A.raw.tex[attr - GX_VA_TEX0][0] = f[0]; A.raw.tex[attr - GX_VA_TEX0][1] = n > 1 ? f[1] : 0.f; }
    slot_done();
}

static void put_ints(int n, const int *v)
{
    float f[3];
    int attr, i;
    if (!A.in_prim || A.slot >= 12) return;
    attr = vorder[A.slot];
    {
        int type = G.vfmt[A.fmt][attr].type;
        float sc = attr == GX_VA_NRM ? 1.f / (float)(1 << (type == GX_S8 ? 6 : 14)) : comp_frac_scale(attr, type, -1);
        if (type == GX_F32) sc = 1.f; /* caller used an integer entry point on a float format: take as is */
        for (i = 0; i < 3; i++) f[i] = i < n ? v[i] * sc : 0.f;
    }
    put_floats(n, f);
}

static void put_color(float r, float g, float b, float a)
{
    int attr;
    if (!A.in_prim || A.slot >= 12) return;
    if (!A.cur_indexed) A.all_idx = 0;
    attr = vorder[A.slot];
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        float *c = A.raw.clr[attr - GX_VA_CLR0];
        c[0] = r; c[1] = g; c[2] = b; c[3] = a;
    }
    slot_done();
}

static float rd_comp(const u8 *p, int type, float scale)
{
    switch (type) {
        case GX_U8: return p[0] * scale;
        case GX_S8: return (s8)p[0] * scale;
        case GX_U16: { u16 v; memcpy(&v, p, 2); return v * scale; }
        case GX_S16: { s16 v; memcpy(&v, p, 2); return v * scale; }
        default: { float v; memcpy(&v, p, 4); return v; }
    }
}

static int comp_size(int type)
{
    return (type == GX_U8 || type == GX_S8) ? 1 : ((type == GX_U16 || type == GX_S16) ? 2 : 4);
}

static void put_index_impl(unsigned idx);

static void put_index(unsigned idx)
{
    if (!A.in_prim || A.slot >= 12) return;
    A.vkey = (A.vkey ^ (((u64)idx << 8) | (u64)A.slot)) * 1099511628211ull;
    A.cur_indexed = 1;
    put_index_impl(idx);
    A.cur_indexed = 0;
}

static void put_index_impl(unsigned idx)
{
    int attr;
    const u8 *base;
    if (!A.in_prim || A.slot >= 12) return;
    attr = vorder[A.slot];
    base = G.arr[attr];
    if (!base) { slot_done(); return; }
    base += (size_t)idx * G.arr_stride[attr];
    if (attr == GX_VA_POS || attr == GX_VA_NRM || (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7)) {
        int type = G.vfmt[A.fmt][attr].type, cnt = G.vfmt[A.fmt][attr].cnt, n, i;
        float f[3] = { 0, 0, 0 };
        float sc = attr == GX_VA_NRM ? 1.f / (float)(1 << (type == GX_S8 ? 6 : 14)) : comp_frac_scale(attr, type, -1);
        if (attr == GX_VA_POS) n = cnt == GX_POS_XYZ ? 3 : 2;
        else if (attr == GX_VA_NRM) n = 3;
        else n = cnt == GX_TEX_ST ? 2 : 1;
        for (i = 0; i < n; i++) f[i] = rd_comp(base + i * comp_size(type), type, sc);
        put_floats(n, f);
        return;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        int type = G.vfmt[A.fmt][attr].type;
        float r = 1, g = 1, b = 1, a = 1;
        u16 v;
        switch (type) {
            case GX_RGB565: memcpy(&v, base, 2); r = (v >> 11) / 31.f; g = ((v >> 5) & 63) / 63.f; b = (v & 31) / 31.f; break;
            case GX_RGB8: case GX_RGBX8: r = base[0] / 255.f; g = base[1] / 255.f; b = base[2] / 255.f; break;
            case GX_RGBA4: memcpy(&v, base, 2); r = (v >> 12) / 15.f; g = ((v >> 8) & 15) / 15.f; b = ((v >> 4) & 15) / 15.f; a = (v & 15) / 15.f; break;
            default: r = base[0] / 255.f; g = base[1] / 255.f; b = base[2] / 255.f; a = base[3] / 255.f; break;
        }
        put_color(r, g, b, a);
        return;
    }
    slot_done();
}

/* ---- display lists: recorded in the real GX FIFO byte layout so sizes match what the game budgets for ----
 * GXBegin -> command byte (prim | vtxfmt) + u16 vertex count; every vertex call then appends its operands at
 * exactly the width the call has (index calls 1/2 bytes, direct calls their native width). 0x00 bytes are NOPs
 * (list padding). Replay parses the vertex data using the vertex descriptor/format current at call time. */
static int rec_room(u32 n)
{
    if (A.dl_len + n + 32 > A.dl_cap) { A.dl_overflow = 1; return 0; }
    return 1;
}

static void rec(const void *p, u32 n)
{
    if (!rec_room(n)) return;
    memcpy(A.dl + A.dl_len, p, n);
    A.dl_len += n;
}

#define VERTEX_FLOATS(n_, ...) do { float _f[3] = { __VA_ARGS__ }; if (A.recording) rec(_f, sizeof(float) * (n_)); else put_floats((n_), _f); } while (0)
#define VERTEX_INTS(T_, n_, ...) do { T_ _v[3] = { __VA_ARGS__ }; if (A.recording) rec(_v, sizeof(T_) * (n_)); else { int _i[3] = { _v[0], _v[1], _v[2] }; put_ints((n_), _i); } } while (0)

static void vtx_index8(u8 idx)
{
    if (A.recording) rec(&idx, 1);
    else put_index(idx);
}

static void vtx_index16(u16 idx)
{
    if (A.recording) rec(&idx, 2);
    else put_index(idx);
}

static void vtx_color(int nbytes, u8 r, u8 g, u8 b, u8 a)
{
    if (A.recording) { u8 c[4] = { r, g, b, a }; rec(c, nbytes); }
    else put_color(r / 255.f, g / 255.f, b / 255.f, a / 255.f);
}

void GXBegin(GXPrimitive type, GXVtxFmt vtxfmt, u16 nverts)
{
    gx_ensure_init();
    if (A.recording) { u8 cmd = (u8)(type | (vtxfmt & 7)); rec(&cmd, 1); rec(&nverts, 2); return; }
    begin_prim(type, vtxfmt);
}

void GXEnd(void)
{
    if (!A.recording) { if (A.in_prim) G_stats.t_draw += now_us() - prim_t0; A.in_prim = 0; }
}

void GXPosition3f32(f32 x, f32 y, f32 z) { VERTEX_FLOATS(3, x, y, z); }
void GXPosition3u16(u16 x, u16 y, u16 z) { VERTEX_INTS(u16, 3, x, y, z); }
void GXPosition3s16(s16 x, s16 y, s16 z) { VERTEX_INTS(s16, 3, x, y, z); }
void GXPosition3u8(u8 x, u8 y, u8 z) { VERTEX_INTS(u8, 3, x, y, z); }
void GXPosition3s8(s8 x, s8 y, s8 z) { VERTEX_INTS(s8, 3, x, y, z); }
void GXPosition2f32(f32 x, f32 y) { VERTEX_FLOATS(2, x, y); }
void GXPosition2u16(u16 x, u16 y) { VERTEX_INTS(u16, 2, x, y); }
void GXPosition2s16(s16 x, s16 y) { VERTEX_INTS(s16, 2, x, y); }
void GXPosition2u8(u8 x, u8 y) { VERTEX_INTS(u8, 2, x, y); }
void GXPosition2s8(s8 x, s8 y) { VERTEX_INTS(s8, 2, x, y); }
void GXPosition1x16(u16 index) { vtx_index16(index); }
void GXPosition1x8(u8 index) { vtx_index8(index); }
void GXNormal3f32(f32 x, f32 y, f32 z) { VERTEX_FLOATS(3, x, y, z); }
void GXNormal3s16(s16 x, s16 y, s16 z) { VERTEX_INTS(s16, 3, x, y, z); }
void GXNormal3s8(s8 x, s8 y, s8 z) { VERTEX_INTS(s8, 3, x, y, z); }
void GXNormal1x16(u16 index) { vtx_index16(index); }
void GXNormal1x8(u8 index) { vtx_index8(index); }
void GXColor4u8(u8 r, u8 g, u8 b, u8 a) { vtx_color(4, r, g, b, a); }
void GXColor3u8(u8 r, u8 g, u8 b) { vtx_color(3, r, g, b, 255); }
void GXColor1u32(u32 clr) { vtx_color(4, clr >> 24, (clr >> 16) & 255, (clr >> 8) & 255, clr & 255); }
void GXColor1u16(u16 clr)
{
    if (A.recording) rec(&clr, 2);
    else put_color(e5to8(clr >> 11) / 255.f, e6to8((clr >> 5) & 63) / 255.f, e5to8(clr & 31) / 255.f, 1.f);
}
void GXColor1x16(u16 index) { vtx_index16(index); }
void GXColor1x8(u8 index) { vtx_index8(index); }
void GXTexCoord2f32(f32 s, f32 t) { VERTEX_FLOATS(2, s, t); }
void GXTexCoord2u16(u16 s, u16 t) { VERTEX_INTS(u16, 2, s, t); }
void GXTexCoord2s16(s16 s, s16 t) { VERTEX_INTS(s16, 2, s, t); }
void GXTexCoord2u8(u8 s, u8 t) { VERTEX_INTS(u8, 2, s, t); }
void GXTexCoord2s8(s8 s, s8 t) { VERTEX_INTS(s8, 2, s, t); }
void GXTexCoord1f32(f32 s, f32 t) { VERTEX_FLOATS(1, s); }
void GXTexCoord1u16(u16 s, u16 t) { VERTEX_INTS(u16, 1, s); }
void GXTexCoord1s16(s16 s, s16 t) { VERTEX_INTS(s16, 1, s); }
void GXTexCoord1u8(u8 s, u8 t) { VERTEX_INTS(u8, 1, s); }
void GXTexCoord1s8(s8 s, s8 t) { VERTEX_INTS(s8, 1, s); }
void GXTexCoord1x16(u16 index) { vtx_index16(index); }
void GXTexCoord1x8(u8 index) { vtx_index8(index); }

void GXBeginDisplayList(void *list, u32 size)
{
    gx_ensure_init();
    A.recording = 1;
    A.dl = (u8 *)list;
    A.dl_cap = size;
    A.dl_len = 0;
    A.dl_overflow = 0;
}

u32 GXEndDisplayList(void)
{
    u32 len;
    if (!A.recording) return 0;
    A.recording = 0;
    if (A.dl_overflow) {
        OSReport("gx_soft: display list overflow (cap %u)\n", A.dl_cap);
        A.dl_len = 0;
    }
    len = (A.dl_len + 31) & ~31u; /* hardware pads lists to 32 bytes with NOPs */
    if (len > A.dl_cap) len = A.dl_cap;
    memset(A.dl + A.dl_len, 0, len - A.dl_len);
    return len;
}

/* read one vertex attribute (direct or indexed, as per the descriptor) from the list and feed it in */
static const u8 *dl_read_attr(const u8 *p, int attr)
{
    int d = G.vdesc[attr];
    int type, cnt;
    if (d == GX_INDEX8) { put_index(*p); return p + 1; }
    if (d == GX_INDEX16) { u16 v; memcpy(&v, p, 2); put_index(v); return p + 2; }
    type = G.vfmt[A.fmt][attr].type;
    cnt = G.vfmt[A.fmt][attr].cnt;
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
        float r = 1, g = 1, b = 1, a = 1;
        u16 v;
        switch (type) {
            case GX_RGB565: memcpy(&v, p, 2); r = (v >> 11) / 31.f; g = ((v >> 5) & 63) / 63.f; b = (v & 31) / 31.f; put_color(r, g, b, a); return p + 2;
            case GX_RGB8: r = p[0] / 255.f; g = p[1] / 255.f; b = p[2] / 255.f; put_color(r, g, b, a); return p + 3;
            case GX_RGBX8: r = p[0] / 255.f; g = p[1] / 255.f; b = p[2] / 255.f; put_color(r, g, b, a); return p + 4;
            case GX_RGBA4: memcpy(&v, p, 2); r = (v >> 12) / 15.f; g = ((v >> 8) & 15) / 15.f; b = ((v >> 4) & 15) / 15.f; a = (v & 15) / 15.f; put_color(r, g, b, a); return p + 2;
            case GX_RGBA6: put_color(r, g, b, a); return p + 3;
            default: r = p[0] / 255.f; g = p[1] / 255.f; b = p[2] / 255.f; a = p[3] / 255.f; put_color(r, g, b, a); return p + 4;
        }
    } else {
        float f[3] = { 0, 0, 0 };
        float sc = attr == GX_VA_NRM ? 1.f / (float)(1 << (type == GX_S8 ? 6 : 14)) : comp_frac_scale(attr, type, -1);
        int n, i;
        if (attr == GX_VA_POS) n = cnt == GX_POS_XYZ ? 3 : 2;
        else if (attr == GX_VA_NRM) n = 3;
        else n = cnt == GX_TEX_ST ? 2 : 1;
        for (i = 0; i < n; i++) f[i] = rd_comp(p + i * comp_size(type), type, sc);
        put_floats(n, f);
        return p + n * comp_size(type);
    }
}

void GXCallDisplayList(const void *list, u32 nbytes)
{
    const u8 *p = (const u8 *)list, *end = p + nbytes;
    u64 t0 = now_us();
    gx_ensure_init();
    while (p < end) {
        u8 cmd = *p++;
        if (cmd == 0) continue; /* NOP / padding */
        if (cmd >= 0x80) {
            u16 n;
            int v, i;
            memcpy(&n, p, 2);
            p += 2;
            begin_prim(cmd & 0xF8, cmd & 7);
            for (v = 0; v < n && p < end; v++) {
                for (i = 0; i < 12; i++) {
                    if (G.vdesc[vorder[i]] != GX_NONE) p = dl_read_attr(p, vorder[i]);
                }
            }
            A.in_prim = 0;
            continue;
        }
        break; /* unknown command: stop */
    }
    G_stats.t_draw += now_us() - t0;
}

/* ------------------------------------------------------------------------------------------------ clipping + raster */
static void pv_lerp(const PV *a, const PV *b, float t, PV *o)
{
    const float *pa = (const float *)a, *pb = (const float *)b;
    float *po = (float *)o;
    int i, n = sizeof(PV) / sizeof(float);
    for (i = 0; i < n; i++) po[i] = pa[i] + (pb[i] - pa[i]) * t;
}

static int clip_plane(const PV *in, int n, PV *out, int plane)
{
    int i, m = 0;
    for (i = 0; i < n; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % n];
        float da = plane == 0 ? a->cz + a->cw : -a->cz;
        float db = plane == 0 ? b->cz + b->cw : -b->cz;
        if (da >= 0.f) out[m++] = *a;
        if ((da >= 0.f) != (db >= 0.f)) {
            float t = da / (da - db);
            pv_lerp(a, b, t, &out[m++]);
        }
    }
    return m;
}

static void tri_clip(const PV *a, const PV *b, const PV *c)
{
    PV p0[8], p1[8];
    int n, i;
    int in_a = (a->cz + a->cw >= 0.f && a->cz <= 0.f), in_b = (b->cz + b->cw >= 0.f && b->cz <= 0.f), in_c = (c->cz + c->cw >= 0.f && c->cz <= 0.f);
    if (in_a && in_b && in_c) {
        if (G.cull != GX_CULL_NONE && a->cw > 0.f && b->cw > 0.f && c->cw > 0.f) {
            /* Facing from the 3x3 determinant of the clip-space (x, y, w) rows: no divides needed. With w > 0 the
               sign equals the orientation in normalised device space (y up); positive = counter-clockwise = front. */
            float det = a->cx * (b->cy * c->cw - c->cy * b->cw) - a->cy * (b->cx * c->cw - c->cx * b->cw) + a->cw * (b->cx * c->cy - c->cx * b->cy);
            if (det == 0.f) { G_stats.tris++; return; }
            if ((G.cull == GX_CULL_BACK && det < 0.f) || (G.cull == GX_CULL_FRONT && det > 0.f) || G.cull == GX_CULL_ALL) { G_stats.tris++; return; }
        }
        raster_tri((PV *)a, (PV *)b, (PV *)c); /* raster_tri only reads the vertices */
        return;
    }
    p0[0] = *a; p0[1] = *b; p0[2] = *c;
    n = clip_plane(p0, 3, p1, 0);
    if (n < 3) return;
    n = clip_plane(p1, n, p0, 1);
    if (n < 3) return;
    for (i = 1; i + 1 < n; i++) raster_tri(&p0[0], &p0[i], &p0[i + 1]);
}

static int cmp_ok(int func, float a, float b)
{
    switch (func) {
        case GX_NEVER: return 0;
        case GX_LESS: return a < b;
        case GX_EQUAL: return a == b;
        case GX_LEQUAL: return a <= b;
        case GX_GREATER: return a > b;
        case GX_NEQUAL: return a != b;
        case GX_GEQUAL: return a >= b;
        default: return 1;
    }
}

static inline int ffloor(float f) { int i = (int)f; return (f < (float)i) ? i - 1 : i; }

static inline int wrap_fast(int i, int n, int mask, int mode)
{
    if (mode == GX_CLAMP) return i < 0 ? 0 : (i >= n ? n - 1 : i);
    if (mode == GX_REPEAT && mask >= 0) return i & mask;
    return wrap_i(i, n, mode);
}

/* returns texel as 4 floats 0..1 */
static inline void fetch_fast(const Tex *t, int wmask, int hmask, float s, float tt, float out[4])
{
    int ws = t->ws & 3, wt = t->wt & 3;
    float u = s * t->w, v = tt * t->h;
    const float k = 1.f / 255.f;
    u32 c;
    if (t->ws & 0x80) {
        float fu = u - 0.5f, fv = v - 0.5f;
        int x0 = ffloor(fu), y0 = ffloor(fv);
        int ax = (int)((fu - x0) * 256.f), ay = (int)((fv - y0) * 256.f);
        int x1, y1;
        u32 c00, c10, c01, c11;
        int sh;
        /* snap to the nearest texel when within ~1% of a texel centre (1:1 sprites): one fetch instead of four */
        if (ax <= 2) ax = 0; else if (ax >= 254) { ax = 0; x0++; }
        if (ay <= 2) ay = 0; else if (ay >= 254) { ay = 0; y0++; }
        if ((ax | ay) == 0) {
            c = t->rgba[wrap_fast(y0, t->h, hmask, wt) * t->w + wrap_fast(x0, t->w, wmask, ws)];
            out[0] = ((c >> 16) & 255) * k; out[1] = ((c >> 8) & 255) * k; out[2] = (c & 255) * k; out[3] = (c >> 24) * k;
            return;
        }
        x1 = wrap_fast(x0 + 1, t->w, wmask, ws);
        y1 = wrap_fast(y0 + 1, t->h, hmask, wt);
        x0 = wrap_fast(x0, t->w, wmask, ws);
        y0 = wrap_fast(y0, t->h, hmask, wt);
        c00 = t->rgba[y0 * t->w + x0]; c10 = t->rgba[y0 * t->w + x1];
        c01 = t->rgba[y1 * t->w + x0]; c11 = t->rgba[y1 * t->w + x1];
        for (sh = 0; sh < 4; sh++) {
            int shf = sh == 3 ? 24 : (16 - 8 * sh);
            int a = (c00 >> shf) & 255, b = (c10 >> shf) & 255, cc = (c01 >> shf) & 255, d = (c11 >> shf) & 255;
            int top = a * 256 + (b - a) * ax, bot = cc * 256 + (d - cc) * ax;
            out[sh] = ((top * 256 + (bot - top) * ay) >> 16) * k;
        }
        return;
    }
    c = t->rgba[wrap_fast(ffloor(v), t->h, hmask, wt) * t->w + wrap_fast(ffloor(u), t->w, wmask, ws)];
    out[0] = ((c >> 16) & 255) * k; out[1] = ((c >> 8) & 255) * k; out[2] = (c & 255) * k; out[3] = (c >> 24) * k;
}

static void write_pixel(int idx, float z, const float res[4])
{
    int k;
    /* alpha test */
    {
        int r0 = cmp_ok(G.ac0, floorf(res[3] * 255.f + 0.5f), (float)G.ar0), r1 = cmp_ok(G.ac1, floorf(res[3] * 255.f + 0.5f), (float)G.ar1);
        int pass;
        switch (G.aop) {
            case GX_AOP_AND: pass = r0 && r1; break;
            case GX_AOP_OR: pass = r0 || r1; break;
            case GX_AOP_XOR: pass = r0 ^ r1; break;
            default: pass = !(r0 ^ r1); break;
        }
        if (!pass) return;
    }
    /* depth */
    if (G.z_en && !cmp_ok(G.z_func, z, G.depth[idx])) return;
    if (G.z_upd) G.depth[idx] = z;
    /* blend + write */
    {
        u32 d = G.color[idx];
        float dst[4] = { ((d >> 16) & 255) / 255.f, ((d >> 8) & 255) / 255.f, (d & 255) / 255.f, (d >> 24) / 255.f };
        float o[4];
        if (G.bl_type == GX_BM_BLEND || G.bl_type == GX_BM_SUBTRACT) {
            float sf[4], df[4];
            for (k = 0; k < 4; k++) {
                float sa = res[3], da = dst[3];
                switch (G.bl_src) {
                    case GX_BL_ZERO: sf[k] = 0; break;
                    case GX_BL_ONE: sf[k] = 1; break;
                    case GX_BL_SRCCLR: sf[k] = dst[k]; break;       /* for the source factor this slot means DSTCLR */
                    case GX_BL_INVSRCCLR: sf[k] = 1 - dst[k]; break;
                    case GX_BL_SRCALPHA: sf[k] = sa; break;
                    case GX_BL_INVSRCALPHA: sf[k] = 1 - sa; break;
                    case GX_BL_DSTALPHA: sf[k] = da; break;
                    default: sf[k] = 1 - da; break;
                }
                switch (G.bl_dst) {
                    case GX_BL_ZERO: df[k] = 0; break;
                    case GX_BL_ONE: df[k] = 1; break;
                    case GX_BL_SRCCLR: df[k] = res[k]; break;
                    case GX_BL_INVSRCCLR: df[k] = 1 - res[k]; break;
                    case GX_BL_SRCALPHA: df[k] = sa; break;
                    case GX_BL_INVSRCALPHA: df[k] = 1 - sa; break;
                    case GX_BL_DSTALPHA: df[k] = da; break;
                    default: df[k] = 1 - da; break;
                }
                o[k] = G.bl_type == GX_BM_SUBTRACT ? dst[k] * df[k] - res[k] * sf[k] : res[k] * sf[k] + dst[k] * df[k];
            }
        } else {
            memcpy(o, res, sizeof(o));
        }
        {
            u32 nc = pack((u8)(sat(o[0]) * 255.f + 0.5f), (u8)(sat(o[1]) * 255.f + 0.5f), (u8)(sat(o[2]) * 255.f + 0.5f), (u8)(sat(o[3]) * 255.f + 0.5f));
            if (!G.col_upd) nc = (nc & 0xFF000000u) | (d & 0x00FFFFFFu);
            if (!G.a_upd) nc = (nc & 0x00FFFFFFu) | (d & 0xFF000000u);
            if (G.col_upd || G.a_upd) G.color[idx] = nc;
        }
    }
}


static void shade_pixel(int px, int py, float z, const float col[2][4], const float tc[8][2])
{
    /* Sources are indexed by the GX_CC_* / GX_CA_* enum values themselves, so inputs are plain table lookups:
       colour args: 0 CPREV 1 APREV 2 C0 3 A0 4 C1 5 A1 6 C2 7 A2 8 TEXC 9 TEXA 10 RASC 11 RASA 12 ONE 13 HALF 14 KONST 15 ZERO
       alpha args:  0 APREV 1 A0 2 A1 3 A2 4 TEXA 5 RASA 6 KONST 7 ZERO
       The four TEV registers (prev, r0, r1, r2) live directly in S[0,2,4,6] (rgb) / SA[0..3] (alpha). */
    float S[16][3];
    float SA[8];
    float res[4];
    int s, k, r;
    int idx = py * EFB_W + px;
    for (r = 0; r < 4; r++) {
        S[2 * r][0] = G.reg[r][0]; S[2 * r][1] = G.reg[r][1]; S[2 * r][2] = G.reg[r][2];
        S[2 * r + 1][0] = S[2 * r + 1][1] = S[2 * r + 1][2] = G.reg[r][3];
        SA[r] = G.reg[r][3];
    }
    S[GX_CC_ONE][0] = S[GX_CC_ONE][1] = S[GX_CC_ONE][2] = 1.f;
    S[GX_CC_HALF][0] = S[GX_CC_HALF][1] = S[GX_CC_HALF][2] = 0.5f;
    S[GX_CC_ZERO][0] = S[GX_CC_ZERO][1] = S[GX_CC_ZERO][2] = 0.f;
    SA[GX_CA_ZERO] = 0.f;
    for (s = 0; s < G.ntev; s++) {
        const Stage *st = &G.st[s];
        float texc[4] = { 0, 0, 0, 0 };
        static const float scl[4] = { 1.f, 2.f, 4.f, 0.5f };
        float cbias, abias, csc, asc, a_out;
        const float *ras = NULL;
        int ks = st->kcsel, as = st->kasel;
        if (st->map < 8 && st->coord < 8 && G.bound[st->map] && G.bound[st->map]->rgba)
            fetch_fast(G.bound[st->map], G.bound[st->map]->wmask, G.bound[st->map]->hmask, tc[st->coord][0], tc[st->coord][1], texc);
        S[GX_CC_TEXC][0] = texc[0]; S[GX_CC_TEXC][1] = texc[1]; S[GX_CC_TEXC][2] = texc[2];
        S[GX_CC_TEXA][0] = S[GX_CC_TEXA][1] = S[GX_CC_TEXA][2] = texc[3];
        SA[GX_CA_TEXA] = texc[3];
        if (st->color == GX_COLOR0A0 || st->color == GX_COLOR0) ras = col[0];
        else if (st->color == GX_COLOR1A1 || st->color == GX_COLOR1) ras = col[1];
        if (ras) {
            S[GX_CC_RASC][0] = ras[0]; S[GX_CC_RASC][1] = ras[1]; S[GX_CC_RASC][2] = ras[2];
            S[GX_CC_RASA][0] = S[GX_CC_RASA][1] = S[GX_CC_RASA][2] = ras[3];
            SA[GX_CA_RASA] = ras[3];
        } else {
            S[GX_CC_RASC][0] = S[GX_CC_RASC][1] = S[GX_CC_RASC][2] = 0.f;
            S[GX_CC_RASA][0] = S[GX_CC_RASA][1] = S[GX_CC_RASA][2] = 0.f;
            SA[GX_CA_RASA] = 0.f;
        }
        if (ks < 8) S[GX_CC_KONST][0] = S[GX_CC_KONST][1] = S[GX_CC_KONST][2] = (8 - ks) / 8.f;
        else if (ks >= 12 && ks <= 15) { S[GX_CC_KONST][0] = G.kcol[ks - 12][0]; S[GX_CC_KONST][1] = G.kcol[ks - 12][1]; S[GX_CC_KONST][2] = G.kcol[ks - 12][2]; }
        else if (ks >= 16 && ks <= 31) S[GX_CC_KONST][0] = S[GX_CC_KONST][1] = S[GX_CC_KONST][2] = G.kcol[(ks - 16) & 3][(ks - 16) >> 2];
        else S[GX_CC_KONST][0] = S[GX_CC_KONST][1] = S[GX_CC_KONST][2] = 1.f;
        if (as < 8) SA[GX_CA_KONST] = (8 - as) / 8.f;
        else if (as >= 16 && as <= 31) SA[GX_CA_KONST] = G.kcol[(as - 16) & 3][(as - 16) >> 2];
        else SA[GX_CA_KONST] = 1.f;

        cbias = st->c_bias == GX_TB_ADDHALF ? 0.5f : (st->c_bias == GX_TB_SUBHALF ? -0.5f : 0.f);
        abias = st->a_bias == GX_TB_ADDHALF ? 0.5f : (st->a_bias == GX_TB_SUBHALF ? -0.5f : 0.f);
        csc = scl[st->c_scale & 3];
        asc = scl[st->a_scale & 3];
        {
            const float *ca = S[st->ci[0]], *cb = S[st->ci[1]], *cc = S[st->ci[2]], *cd = S[st->ci[3]];
            float out[3];
            for (k = 0; k < 3; k++) {
                float v = ca[k] * (1.f - cc[k]) + cb[k] * cc[k];
                v = cd[k] + (st->c_op == GX_TEV_SUB ? -v : v) + cbias;
                v *= csc;
                out[k] = st->c_clamp ? sat(v) : clampf(v, -4.f, 4.f);
            }
            {
                float v = SA[st->ai[0]] * (1.f - SA[st->ai[2]]) + SA[st->ai[1]] * SA[st->ai[2]];
                v = SA[st->ai[3]] + (st->a_op == GX_TEV_SUB ? -v : v) + abias;
                v *= asc;
                a_out = st->a_clamp ? sat(v) : clampf(v, -4.f, 4.f);
            }
            /* outputs: colour and alpha may target different registers */
            {
                int rc_ = st->c_out & 3, ra_ = st->a_out & 3;
                S[2 * rc_][0] = out[0]; S[2 * rc_][1] = out[1]; S[2 * rc_][2] = out[2];
                SA[ra_] = a_out;
                S[2 * ra_ + 1][0] = S[2 * ra_ + 1][1] = S[2 * ra_ + 1][2] = a_out;
            }
        }
    }
    res[0] = S[0][0]; res[1] = S[0][1]; res[2] = S[0][2]; res[3] = SA[0];
    write_pixel(idx, z, res);
}

static float edge_fn(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* top-left fill rule for a positively oriented (y-down) triangle edge a->b */
static int edge_top_left(float ax, float ay, float bx, float by)
{
    float dx = bx - ax, dy = by - ay;
    return (dy == 0.f && dx > 0.f) || dy < 0.f;
}

/* ---- fast paths: a single TEV stage in one of the standard configurations ---- */
enum { FAST_NONE = 0, FAST_MODULATE, FAST_REPLACE, FAST_PASSCLR, FAST_MODULATE_KA, FAST_PASSCLR_KA };

int port_gx_force_generic; /* test hook: forces the generic TEV path */
int port_gx_no_vcache;     /* test hook: disables the post-transform vertex cache */

static inline int cmp_i(int func, int a, int b)
{
    switch (func) {
        case GX_NEVER: return 0;
        case GX_LESS: return a < b;
        case GX_EQUAL: return a == b;
        case GX_LEQUAL: return a <= b;
        case GX_GREATER: return a > b;
        case GX_NEQUAL: return a != b;
        case GX_GEQUAL: return a >= b;
        default: return 1;
    }
}

static inline int atest8(int a8)
{
    int r0 = cmp_i(G.ac0, a8, G.ar0), r1 = cmp_i(G.ac1, a8, G.ar1);
    switch (G.aop) {
        case GX_AOP_AND: return r0 && r1;
        case GX_AOP_OR: return r0 || r1;
        case GX_AOP_XOR: return r0 ^ r1;
        default: return !(r0 ^ r1);
    }
}

static int fast_select(void)
{
    const Stage *st = &G.st[0];
    int tex_ok;
    if (port_gx_force_generic) return FAST_NONE;
    if (G.ntev != 1) { G_stats.fast_fail[1]++; return FAST_NONE; }
    if (st->c_op != GX_TEV_ADD || st->a_op != GX_TEV_ADD || st->c_bias != GX_TB_ZERO || st->a_bias != GX_TB_ZERO) { G_stats.fast_fail[2]++; return FAST_NONE; }
    if (st->c_scale != GX_CS_SCALE_1 || st->a_scale != GX_CS_SCALE_1 || !st->c_clamp || !st->a_clamp || st->c_out != 0 || st->a_out != 0) { G_stats.fast_fail[3]++; return FAST_NONE; }
    if (st->color != GX_COLOR0A0 && st->color != GX_COLOR0) { G_stats.fast_fail[4]++; return FAST_NONE; }
    tex_ok = st->map < 8 && st->coord < 8 && G.bound[st->map] && G.bound[st->map]->rgba && G.ntexgens > st->coord;
    if (st->ci[0] == GX_CC_ZERO && st->ci[1] == GX_CC_TEXC && st->ci[2] == GX_CC_RASC && st->ci[3] == GX_CC_ZERO &&
        st->ai[0] == GX_CA_ZERO && st->ai[1] == GX_CA_TEXA && st->ai[2] == GX_CA_RASA && st->ai[3] == GX_CA_ZERO) {
        if (!tex_ok) { G_stats.fast_fail[6]++; return FAST_NONE; }
        return FAST_MODULATE;
    }
    if (st->ci[0] == GX_CC_ZERO && st->ci[1] == GX_CC_ZERO && st->ci[2] == GX_CC_ZERO && st->ci[3] == GX_CC_TEXC &&
        st->ai[0] == GX_CA_ZERO && st->ai[1] == GX_CA_ZERO && st->ai[2] == GX_CA_ZERO && st->ai[3] == GX_CA_TEXA) {
        if (!tex_ok) { G_stats.fast_fail[6]++; return FAST_NONE; }
        return FAST_REPLACE;
    }
    if (st->ci[0] == GX_CC_ZERO && st->ci[1] == GX_CC_ZERO && st->ci[2] == GX_CC_ZERO && st->ci[3] == GX_CC_RASC &&
        st->ai[0] == GX_CA_ZERO && st->ai[1] == GX_CA_ZERO && st->ai[2] == GX_CA_ZERO && st->ai[3] == GX_CA_RASA)
        return FAST_PASSCLR;
    /* texture * vertex colour, alpha = texture alpha * constant (a fade level) */
    if (st->ci[0] == GX_CC_ZERO && st->ci[1] == GX_CC_TEXC && st->ci[2] == GX_CC_RASC && st->ci[3] == GX_CC_ZERO &&
        st->ai[0] == GX_CA_ZERO && st->ai[1] == GX_CA_TEXA && st->ai[2] == GX_CA_KONST && st->ai[3] == GX_CA_ZERO) {
        if (!tex_ok) { G_stats.fast_fail[6]++; return FAST_NONE; }
        return FAST_MODULATE_KA;
    }
    /* flat vertex colour with a constant alpha */
    if (st->ci[0] == GX_CC_ZERO && st->ci[1] == GX_CC_ZERO && st->ci[2] == GX_CC_ZERO && st->ci[3] == GX_CC_RASC &&
        st->ai[0] == GX_CA_ZERO && st->ai[1] == GX_CA_ZERO && st->ai[2] == GX_CA_ZERO && st->ai[3] == GX_CA_KONST)
        return FAST_PASSCLR_KA;
    G_stats.fast_fail[5]++;
    {
        /* remember a few distinct unrecognised single-stage setups so they can be given a fast path later */
        u8 key[16];
        int i, j;
        key[0] = st->ci[0]; key[1] = st->ci[1]; key[2] = st->ci[2]; key[3] = st->ci[3];
        key[4] = st->ai[0]; key[5] = st->ai[1]; key[6] = st->ai[2]; key[7] = st->ai[3];
        key[8] = st->c_scale; key[9] = st->a_scale; key[10] = st->c_bias; key[11] = st->a_bias; key[12] = st->c_op | (st->a_op << 1) | (st->c_clamp << 2) | (st->a_clamp << 3); key[13] = st->c_out; key[14] = st->a_out; key[15] = st->color;
        for (i = 0; i < 6; i++) {
            if (G_stats.pat_n[i] && !memcmp(G_stats.pat[i], key, 16)) { G_stats.pat_n[i]++; return FAST_NONE; }
        }
        for (j = 0; j < 6; j++) if (!G_stats.pat_n[j]) { memcpy(G_stats.pat[j], key, 16); G_stats.pat_n[j] = 1; break; }
    }
    return FAST_NONE;
}

/* ---- integer helpers for the affine fast path ---- */
static inline u32 lerp32(u32 c0, u32 c1, int w) /* w 0..256, both colours packed 0xAARRGGBB */
{
    u32 rb = (((c0 & 0x00FF00FFu) * (u32)(256 - w) + (c1 & 0x00FF00FFu) * (u32)w) >> 8) & 0x00FF00FFu;
    u32 ag = ((((c0 >> 8) & 0x00FF00FFu) * (u32)(256 - w) + ((c1 >> 8) & 0x00FF00FFu) * (u32)w)) & 0xFF00FF00u;
    return rb | ag;
}

static inline int mul8(int a, int b)
{
    int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

/* u, v: texel coordinates in 16.16 fixed point */
static inline u32 fetch_i(const Tex *t, int linear, int u, int v)
{
    int ws = t->ws & 3, wt = t->wt & 3;
    if (linear) {
        int fu = u - 32768, fv = v - 32768;
        int x0 = fu >> 16, y0 = fv >> 16;
        int ax = (fu >> 8) & 255, ay = (fv >> 8) & 255;
        int x1, y1;
        u32 c00, c10, c01, c11;
        if (ax <= 2) ax = 0; else if (ax >= 254) { ax = 0; x0++; }
        if (ay <= 2) ay = 0; else if (ay >= 254) { ay = 0; y0++; }
        if ((ax | ay) == 0) return t->rgba[wrap_fast(y0, t->h, t->hmask, wt) * t->w + wrap_fast(x0, t->w, t->wmask, ws)];
        x1 = wrap_fast(x0 + 1, t->w, t->wmask, ws);
        y1 = wrap_fast(y0 + 1, t->h, t->hmask, wt);
        x0 = wrap_fast(x0, t->w, t->wmask, ws);
        y0 = wrap_fast(y0, t->h, t->hmask, wt);
        c00 = t->rgba[y0 * t->w + x0]; c10 = t->rgba[y0 * t->w + x1];
        c01 = t->rgba[y1 * t->w + x0]; c11 = t->rgba[y1 * t->w + x1];
        return lerp32(lerp32(c00, c10, ax), lerp32(c01, c11, ax), ay);
    }
    return t->rgba[wrap_fast(v >> 16, t->h, t->hmask, wt) * t->w + wrap_fast(u >> 16, t->w, t->wmask, ws)];
}

/* ---- per-row coverage: the pixels k in [0,n) whose edge function is inside, with the top-left rule ---- */
static inline int edge_in(float e_row, float dEx, int k, int tl)
{
    float e = e_row + dEx * (float)k;
    return e > 0.f || (e == 0.f && tl);
}

static void edge_span(float e_row, float dEx, int tl, int n, int *lo, int *hi)
{
    if (dEx == 0.f) {
        if (!edge_in(e_row, 0.f, 0, tl)) *lo = *hi; /* empty */
    } else if (dEx > 0.f) {
        float t = -e_row / dEx;
        int kmin = t < -1.f ? -1 : (t > (float)(n + 1) ? n + 1 : (int)ceilf(t));
        if (kmin < 0) kmin = 0;
        while (kmin > 0 && edge_in(e_row, dEx, kmin - 1, tl)) kmin--;
        while (kmin < n && !edge_in(e_row, dEx, kmin, tl)) kmin++;
        if (kmin > *lo) *lo = kmin;
    } else {
        float t = e_row / -dEx;
        int kmax = t < -1.f ? 0 : (t > (float)(n + 1) ? n : (int)ceilf(t)); /* exclusive upper bound guess */
        if (kmax < 0) kmax = 0;
        if (kmax > n) kmax = n;
        while (kmax < n && edge_in(e_row, dEx, kmax, tl)) kmax++;
        while (kmax > 0 && !edge_in(e_row, dEx, kmax - 1, tl)) kmax--;
        if (kmax < *hi) *hi = kmax;
    }
}

#define NA_MAX 28

static void raster_tri(PV *v0, PV *v1, PV *v2)
{
    PV *v[3] = { v0, v1, v2 };
    float sx[3], sy[3], sz[3], iw[3];
    float area, minx, maxx, miny, maxy;
    int x0, x1, y0, y1, x, y, i, k, tl0, tl1, tl2;
    int fast, na, affine, ntc, int_path, fast_tex = 0, ka8 = 255;
    float kaf = 1.f;
    const Tex *ftex = NULL;
    int fcoord = 0, flinear = 0;
    float inv_area, iw_const = 1.f, inv_iw_const = 1.f;
    float dEx[3], dEy[3], e_row[3];
    float pl0[NA_MAX], pldx[NA_MAX], pldy[NA_MAX];
    int nfast_blend, alpha_trivial;
    int act[NA_MAX], nact = 0;
    int use_col1 = 0;
    unsigned tcmask = 0;
    float col[2][4], tc[8][2];
    G_stats.tris++;
    for (i = 0; i < 3; i++) {
        float w = v[i]->cw;
        if (w == 0.f) return;
        iw[i] = 1.f / w;
        sx[i] = G.vp[0] + (v[i]->cx * iw[i] + 1.f) * 0.5f * G.vp[2];
        sy[i] = G.vp[1] + (1.f - v[i]->cy * iw[i]) * 0.5f * G.vp[3];
        sz[i] = G.vp[5] + (v[i]->cz * iw[i]) * (G.vp[5] - G.vp[4]);
    }
    area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (area == 0.f) return;
    /* y-down screen: area < 0 means counter-clockwise as seen on screen = GX front face */
    {
        int front = area < 0.f;
        if (G.cull == GX_CULL_ALL) return;
        if (G.cull == GX_CULL_BACK && !front) return;
        if (G.cull == GX_CULL_FRONT && front) return;
    }
    if (area < 0.f) { /* make the winding positive so edge functions are > 0 inside */
        PV *tv = v[1]; float t;
        v[1] = v[2]; v[2] = tv;
        t = sx[1]; sx[1] = sx[2]; sx[2] = t;
        t = sy[1]; sy[1] = sy[2]; sy[2] = t;
        t = sz[1]; sz[1] = sz[2]; sz[2] = t;
        t = iw[1]; iw[1] = iw[2]; iw[2] = t;
        area = -area;
    }
    minx = fminf(sx[0], fminf(sx[1], sx[2]));
    maxx = fmaxf(sx[0], fmaxf(sx[1], sx[2]));
    miny = fminf(sy[0], fminf(sy[1], sy[2]));
    maxy = fmaxf(sy[0], fmaxf(sy[1], sy[2]));
    x0 = (int)floorf(minx); x1 = (int)ceilf(maxx);
    y0 = (int)floorf(miny); y1 = (int)ceilf(maxy);
    if (x0 < G.sc[0]) x0 = G.sc[0];
    if (y0 < G.sc[1]) y0 = G.sc[1];
    if (x1 > G.sc[0] + G.sc[2]) x1 = G.sc[0] + G.sc[2];
    if (y1 > G.sc[1] + G.sc[3]) y1 = G.sc[1] + G.sc[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EFB_W) x1 = EFB_W;
    if (y1 > EFB_H) y1 = EFB_H;
    if (x0 >= x1 || y0 >= y1) return;
    G_stats.tris_drawn++;
    if (G.z_upd) G.depth_dirty = 1;

    tl0 = edge_top_left(sx[1], sy[1], sx[2], sy[2]); /* edge opposite v0 */
    tl1 = edge_top_left(sx[2], sy[2], sx[0], sy[0]);
    tl2 = edge_top_left(sx[0], sy[0], sx[1], sy[1]);
    /* edge functions: E0 opposite v0 (edge v1->v2), E1 (v2->v0), E2 (v0->v1); weight_i = E_i / area */
    dEx[0] = -(sy[2] - sy[1]); dEy[0] = sx[2] - sx[1];
    dEx[1] = -(sy[0] - sy[2]); dEy[1] = sx[0] - sx[2];
    dEx[2] = -(sy[1] - sy[0]); dEy[2] = sx[1] - sx[0];
    e_row[0] = edge_fn(sx[1], sy[1], sx[2], sy[2], x0 + 0.5f, y0 + 0.5f);
    e_row[1] = edge_fn(sx[2], sy[2], sx[0], sy[0], x0 + 0.5f, y0 + 0.5f);
    e_row[2] = edge_fn(sx[0], sy[0], sx[1], sy[1], x0 + 0.5f, y0 + 0.5f);
    inv_area = 1.f / area;

    affine = (iw[0] == iw[1] && iw[1] == iw[2]);
    if (affine) { iw_const = iw[0]; inv_iw_const = 1.f / iw_const; }
    ntc = G.ntexgens < 8 ? G.ntexgens : 8;
    fast = fast_select();
    alpha_trivial = (G.ac0 == GX_ALWAYS && G.ac1 == GX_ALWAYS && (G.aop == GX_AOP_AND || G.aop == GX_AOP_OR));
    if (fast == FAST_MODULATE) {
        /* vertex colour is exactly opaque white on all three vertices: modulating by it is a no-op */
        int w = 1, vi;
        for (vi = 0; vi < 3; vi++) if (v[vi]->col[0][0] != 1.f || v[vi]->col[0][1] != 1.f || v[vi]->col[0][2] != 1.f || v[vi]->col[0][3] != 1.f) w = 0;
        if (w) fast = FAST_REPLACE;
    }
    fast_tex = (fast == FAST_MODULATE || fast == FAST_REPLACE || fast == FAST_MODULATE_KA);
    if (fast == FAST_MODULATE_KA || fast == FAST_PASSCLR_KA) {
        int as = G.st[0].kasel;
        kaf = as < 8 ? (8 - as) / 8.f : ((as >= 16 && as <= 31) ? G.kcol[(as - 16) & 3][(as - 16) >> 2] : 1.f);
        ka8 = (int)(kaf * 255.f + 0.5f);
    }
    if (fast_tex) {
        ftex = G.bound[G.st[0].map];
        fcoord = G.st[0].coord;
        flinear = (ftex->ws & 0x80) != 0;
    }
    nfast_blend = (G.bl_type == GX_BM_BLEND && G.bl_src == GX_BL_SRCALPHA && G.bl_dst == GX_BL_INVSRCALPHA) ? 1 : (G.bl_type == GX_BM_NONE ? 2 : 0);
    int_path = fast && affine && nfast_blend && G.col_upd && G.a_upd;

    /* attribute planes: 0 z, 1 iw, 2..5 col0*iw, 6..9 col1*iw, 10+2j,11+2j texcoord j * iw. Only the ones in use. */
    na = 10 + 2 * ntc;
    act[nact++] = 0;
    if (!affine) act[nact++] = 1;
    for (k = 2; k < 6; k++) act[nact++] = k;
    if (fast) {
        if (fast_tex) { act[nact++] = 10 + 2 * fcoord; act[nact++] = 11 + 2 * fcoord; }
    } else {
        for (i = 0; i < G.ntev; i++) {
            const Stage *st = &G.st[i];
            if (st->color == GX_COLOR1A1 || st->color == GX_COLOR1) use_col1 = 1;
            if (st->map < 8 && st->coord < 8 && G.bound[st->map]) tcmask |= 1u << st->coord;
        }
        if (use_col1) for (k = 6; k < 10; k++) act[nact++] = k;
        for (i = 0; i < ntc; i++) if (tcmask & (1u << i)) { act[nact++] = 10 + 2 * i; act[nact++] = 11 + 2 * i; }
    }
    for (i = 0; i < nact; i++) {
        float q[3];
        int j;
        k = act[i];
        for (j = 0; j < 3; j++) {
            const PV *pv = v[j];
            if (k == 0) q[j] = sz[j];
            else if (k == 1) q[j] = iw[j];
            else if (k < 6) q[j] = pv->col[0][k - 2] * iw[j];
            else if (k < 10) q[j] = pv->col[1][k - 6] * iw[j];
            else q[j] = pv->tc[(k - 10) >> 1][(k - 10) & 1] * iw[j];
        }
        pl0[k] = (q[0] * e_row[0] + q[1] * e_row[1] + q[2] * e_row[2]) * inv_area;
        pldx[k] = (q[0] * dEx[0] + q[1] * dEx[1] + q[2] * dEx[2]) * inv_area;
        pldy[k] = (q[0] * dEy[0] + q[1] * dEy[1] + q[2] * dEy[2]) * inv_area;
    }
    memset(col, 0, sizeof(col));
    memset(tc, 0, sizeof(tc));

    for (y = y0; y < y1; y++) {
        int lo = 0, hi = x1 - x0, xa, xb;
        edge_span(e_row[0], dEx[0], tl0, x1 - x0, &lo, &hi);
        edge_span(e_row[1], dEx[1], tl1, x1 - x0, &lo, &hi);
        edge_span(e_row[2], dEx[2], tl2, x1 - x0, &lo, &hi);
        xa = x0 + lo;
        xb = x0 + hi;
        if (xa < xb) {
            if (int_path) {
                /* all-integer inner loop (flat/orthographic sprites): attributes in 16.16 fixed point */
                float cs = inv_iw_const * 255.f * 65536.f;
                int c[4], dc[4], u = 0, vv = 0, du = 0, dv = 0;
                float zc = pl0[0] + pldx[0] * (float)lo, dz = pldx[0];
                for (k = 0; k < 4; k++) { c[k] = (int)((pl0[2 + k] + pldx[2 + k] * (float)lo) * cs); dc[k] = (int)(pldx[2 + k] * cs); }
                if (fast_tex) {
                    float us = inv_iw_const * (float)ftex->w * 65536.f, vs = inv_iw_const * (float)ftex->h * 65536.f;
                    u = (int)((pl0[10 + 2 * fcoord] + pldx[10 + 2 * fcoord] * (float)lo) * us);
                    vv = (int)((pl0[11 + 2 * fcoord] + pldx[11 + 2 * fcoord] * (float)lo) * vs);
                    du = (int)(pldx[10 + 2 * fcoord] * us);
                    dv = (int)(pldx[11 + 2 * fcoord] * vs);
                }
                G_stats.px_fast += xb - xa;
                for (x = xa; x < xb; x++, zc += dz, u += du, vv += dv, c[0] += dc[0], c[1] += dc[1], c[2] += dc[2], c[3] += dc[3]) {
                    int idx = y * EFB_W + x;
                    int r, g, b, a;
                    u32 d, o;
                    if (((x - xa) & 15) == 0) { /* stay a cache line or two ahead of the stores / texture reads */
                        __builtin_prefetch(&G.color[idx + 32], 1, 0);
                        if (G.z_en) __builtin_prefetch(&G.depth[idx + 32], 1, 0);
                        if (fast_tex) __builtin_prefetch(&ftex->rgba[wrap_fast((vv + dv * 16) >> 16, ftex->h, ftex->hmask, ftex->wt & 3) * ftex->w + wrap_fast((u + du * 16) >> 16, ftex->w, ftex->wmask, ftex->ws & 3)], 0, 0);
                    }
                    if (G.z_en && !cmp_ok(G.z_func, zc, G.depth[idx])) continue;
                    r = c[0] >> 16; g = c[1] >> 16; b = c[2] >> 16; a = c[3] >> 16;
                    r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g);
                    b = b < 0 ? 0 : (b > 255 ? 255 : b); a = a < 0 ? 0 : (a > 255 ? 255 : a);
                    if (fast_tex) {
                        u32 t = fetch_i(ftex, flinear, u, vv);
                        if (fast == FAST_MODULATE) {
                            r = mul8(r, (t >> 16) & 255); g = mul8(g, (t >> 8) & 255); b = mul8(b, t & 255); a = mul8(a, t >> 24);
                        } else if (fast == FAST_MODULATE_KA) {
                            r = mul8(r, (t >> 16) & 255); g = mul8(g, (t >> 8) & 255); b = mul8(b, t & 255); a = mul8(t >> 24, ka8);
                        } else { r = (t >> 16) & 255; g = (t >> 8) & 255; b = t & 255; a = t >> 24; }
                    } else if (fast == FAST_PASSCLR_KA) {
                        a = ka8;
                    }
                    if (!alpha_trivial && !atest8(a)) continue;
                    if (G.z_upd) G.depth[idx] = zc;
                    if (nfast_blend == 1) {
                        if (a == 0) continue;        /* fully transparent: colour and alpha stay as they are */
                        if (a == 255) { o = pack(r, g, b, 255); }
                        else {
                            /* two channels per multiply: blue/red in one word, green/alpha in the other */
                            u32 sp = pack(r, g, b, a), rb, ag;
                            int ia = 255 - a;
                            d = G.color[idx];
                            rb = (sp & 0x00FF00FFu) * (u32)a + (d & 0x00FF00FFu) * (u32)ia + 0x00800080u;
                            rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
                            ag = ((sp >> 8) & 0x00FF00FFu) * (u32)a + ((d >> 8) & 0x00FF00FFu) * (u32)ia + 0x00800080u;
                            ag = (ag + ((ag >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
                            o = rb | ag;
                        }
                    } else {
                        o = pack(r, g, b, a);
                    }
                    G.color[idx] = o;
                }
            } else {
                float cur[NA_MAX];
                int ntcu = ntc;
                for (i = 0; i < nact; i++) cur[act[i]] = pl0[act[i]] + pldx[act[i]] * (float)lo;
                for (x = xa; x < xb; x++) {
                    int idx = y * EFB_W + x;
                    float z = cur[0];
                    float invw = affine ? inv_iw_const : (cur[1] != 0.f ? 1.f / cur[1] : 0.f);
                    if (fast) {
                        float res[4];
                        G_stats.px_fast++;
                        if (G.z_en && !cmp_ok(G.z_func, z, G.depth[idx])) goto next_pixel;
                        res[0] = cur[2] * invw; res[1] = cur[3] * invw; res[2] = cur[4] * invw; res[3] = cur[5] * invw;
                        if (fast_tex) {
                            float tx[4];
                            fetch_fast(ftex, ftex->wmask, ftex->hmask, cur[10 + 2 * fcoord] * invw, cur[11 + 2 * fcoord] * invw, tx);
                            if (fast == FAST_MODULATE) { res[0] *= tx[0]; res[1] *= tx[1]; res[2] *= tx[2]; res[3] *= tx[3]; }
                            else if (fast == FAST_MODULATE_KA) { res[0] *= tx[0]; res[1] *= tx[1]; res[2] *= tx[2]; res[3] = tx[3] * kaf; }
                            else { res[0] = tx[0]; res[1] = tx[1]; res[2] = tx[2]; res[3] = tx[3]; }
                        } else if (fast == FAST_PASSCLR_KA) {
                            res[3] = kaf;
                        }
                        if (!alpha_trivial && !atest8((int)(sat(res[3]) * 255.f + 0.5f))) goto next_pixel;
                        if (G.z_upd) G.depth[idx] = z;
                        if (nfast_blend == 1 && G.col_upd && G.a_upd) {
                            u32 d = G.color[idx];
                            float a = sat(res[3]), ia = 1.f - a;
                            float r = sat(res[0]) * a + ((d >> 16) & 255) * (1.f / 255.f) * ia;
                            float g = sat(res[1]) * a + ((d >> 8) & 255) * (1.f / 255.f) * ia;
                            float b = sat(res[2]) * a + (d & 255) * (1.f / 255.f) * ia;
                            float oa = a * a + (d >> 24) * (1.f / 255.f) * ia;
                            G.color[idx] = pack((u8)(r * 255.f + 0.5f), (u8)(g * 255.f + 0.5f), (u8)(b * 255.f + 0.5f), (u8)(sat(oa) * 255.f + 0.5f));
                        } else if (nfast_blend == 2 && G.col_upd && G.a_upd) {
                            G.color[idx] = pack((u8)(sat(res[0]) * 255.f + 0.5f), (u8)(sat(res[1]) * 255.f + 0.5f), (u8)(sat(res[2]) * 255.f + 0.5f), (u8)(sat(res[3]) * 255.f + 0.5f));
                        } else {
                            int z_en = G.z_en, ac0 = G.ac0, ac1 = G.ac1;
                            G.z_en = 0; G.ac0 = G.ac1 = GX_ALWAYS; /* depth and alpha test were already applied above */
                            write_pixel(idx, z, res);
                            G.z_en = z_en; G.ac0 = ac0; G.ac1 = ac1;
                        }
                    } else {
                        int j;
                        if (alpha_trivial && G.z_en && !cmp_ok(G.z_func, z, G.depth[idx])) goto next_pixel; /* early Z */
                        G_stats.px_general++;
                        for (j = 0; j < 4; j++) col[0][j] = cur[2 + j] * invw;
                        if (use_col1) for (j = 0; j < 4; j++) col[1][j] = cur[6 + j] * invw;
                        for (j = 0; j < ntcu; j++) if (tcmask & (1u << j)) { tc[j][0] = cur[10 + 2 * j] * invw; tc[j][1] = cur[11 + 2 * j] * invw; }
                        shade_pixel(x, y, z, col, tc);
                    }
                next_pixel:
                    for (i = 0; i < nact; i++) cur[act[i]] += pldx[act[i]];
                }
            }
        }
        e_row[0] += dEy[0]; e_row[1] += dEy[1]; e_row[2] += dEy[2];
        for (i = 0; i < nact; i++) pl0[act[i]] += pldy[act[i]];
    }
}

/* ------------------------------------------------------------------------------------------------ frame copy / present */
void GXSetCopyClear(GXColor c, u32 z)
{
    gx_ensure_init();
    G.clear_color = pack(c.r, c.g, c.b, c.a);
    G.clear_z = (float)z / 16777215.f;
}

void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
    gx_ensure_init();
    G.copy_w = (wd / GX_DIV) > EFB_W ? EFB_W : (wd / GX_DIV);
    G.copy_h = (ht / GX_DIV) > EFB_H ? EFB_H : (ht / GX_DIV);
    G.copy_h_real = ht;
}

u32 GXSetDispCopyYScale(f32 vscale)
{
    gx_ensure_init();
    return (u32)(G.copy_h_real * vscale);
}

void GXSetDispCopyDst(u16 wd, u16 ht) {}

void GXCopyDisp(void *dest, GXBool clear)
{
    int y;
    gx_ensure_init();
    for (y = 0; y < G.copy_h; y++) memcpy(&G.front[y * G.copy_w], &G.color[y * EFB_W], G.copy_w * 4);
    G.front_w = G.copy_w;
    G.front_h = G.copy_h;
    G.frame_ready = 1;
    G.frames++;
    vepoch++;
    if (clear) efb_clear();
}

void GXCopyTex(void *dest, GXBool clear)
{
    gx_ensure_init();
    if (clear) efb_clear();
}

void GXAdjustForOverscan(GXRenderModeObj *rmin, GXRenderModeObj *rmout, u16 hor, u16 ver)
{
    if (rmin != rmout) memcpy(rmout, rmin, sizeof(*rmout));
}

void GXDrawDone(void) {}
void GXWaitDrawDone(void) {}
void GXSetDrawDone(void) {}

/* Hook for the platform video layer. The PS3 build provides a strong definition (port/ps3/src/video_ps3.c). */
__attribute__((weak)) void port_video_present(const u32 *argb, int w, int h) {}

/* Called once per displayed frame (from VIWaitForRetrace). */
int port_gx_present(void)
{
    if (G.inited && G.frame_ready) {
        u64 t0 = now_us();
        G.frame_ready = 0;
        port_video_present(G.front, G.front_w, G.front_h);
        G_stats.t_present += now_us() - t0;
        G_stats.frames++;
        if (G_stats.frames >= 30) {
            u64 now = now_us();
            u64 wall = G_stats.t_last ? now - G_stats.t_last : 0;
            OSReport("[gx] %u frames in %u ms: draw %u ms (%u tris, %u drawn; px fast %u / generic %u), tex decode %u ms (%u), present %u ms, vcache %u/%u, fast-reject ntev %u ops %u clamp %u color %u mode %u tex %u\n",
                     G_stats.frames, (u32)(wall / 1000), (u32)(G_stats.t_draw / 1000), G_stats.tris, G_stats.tris_drawn, G_stats.px_fast, G_stats.px_general,
                     (u32)(G_stats.t_decode / 1000), G_stats.tex_decodes, (u32)(G_stats.t_present / 1000), G_stats.vc_hit, G_stats.vc_miss,
                     G_stats.fast_fail[1], G_stats.fast_fail[2], G_stats.fast_fail[3], G_stats.fast_fail[4], G_stats.fast_fail[5], G_stats.fast_fail[6]);
            {
                int i;
                for (i = 0; i < 6; i++) if (G_stats.pat_n[i]) {
                    const u8 *k = G_stats.pat[i];
                    OSReport("[gx]   unmatched 1-stage TEV x%u: color in %d,%d,%d,%d alpha in %d,%d,%d,%d scale %d/%d bias %d/%d flags %02x out %d/%d ras %d\n", G_stats.pat_n[i],
                             k[0], k[1], k[2], k[3], k[4], k[5], k[6], k[7], k[8], k[9], k[10], k[11], k[12], k[13], k[14], k[15]);
                }
            }
            memset(&G_stats, 0, sizeof(G_stats));
            G_stats.t_last = now;
        } else if (!G_stats.t_last) {
            G_stats.t_last = now_us();
        }
        return 1;
    }
    return 0;
}

/* test/diagnostic access */
const u32 *port_gx_front(int *w, int *h)
{
    if (w) *w = G.front_w;
    if (h) *h = G.front_h;
    return G.front;
}
const u32 *port_gx_efb(void) { return G.color; }
