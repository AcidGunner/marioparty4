/*
 * PS3 video output for the software GX renderer (port/host/src/gx_soft.c).
 *
 * port_video_present() receives the finished 640x480-ish frame (0xAARRGGBB words), scales it to the console's
 * current video mode keeping the aspect ratio (letterboxed), writes it into the back display buffer and flips
 * with vsync. This follows PSL1GHT's rsxtest sample flow. NOTE: written without access to a PS3 toolchain, so it is
 * untested; if the build or the first frame misbehaves, this file is the place to look.
 *
 * The CPU writes straight into RSX local memory, which is simple but not fast; a later optimisation is to render
 * into main memory and let the RSX DMA it (rsxSetTransferData) or to draw with the RSX itself.
 */
#ifdef PORT_PS3

#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ppu-types.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#define CB_SIZE 0x100000
#define HOST_SIZE (32 * 1024 * 1024)
#define NUM_BUFFERS 2
#define MAX_DIM 1920

static gcmContextData *context;
static u32 *fb[NUM_BUFFERS];
static u32 fb_offset[NUM_BUFFERS];
static int disp_w, disp_h;
static int cur_buf;
static int first_flip = 1;
static int init_state; /* 0 = not tried, 1 = ok, -1 = failed */
static int xmap[MAX_DIM];

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

static int do_flip(int buffer)
{
    if (!first_flip) wait_flip();
    else gcmResetFlipStatus();
    if (gcmSetFlip(context, buffer) == 0) {
        rsxFlushBuffer(context);
        gcmSetWaitFlip(context);
        first_flip = 0;
        return 1;
    }
    return 0;
}

static int video_init(void)
{
    void *host_addr;
    videoState state;
    videoResolution res;
    videoConfiguration vconfig;
    int i;

    host_addr = memalign(1024 * 1024, HOST_SIZE);
    if (!host_addr) { fprintf(stderr, "video: host buffer alloc failed\n"); return 0; }
    rsxInit(&context, CB_SIZE, HOST_SIZE, host_addr);
    if (!context) { fprintf(stderr, "video: rsxInit failed\n"); return 0; }

    if (videoGetState(0, 0, &state) != 0 || state.state != 0) { fprintf(stderr, "video: no display connected\n"); return 0; }
    if (videoGetResolution(state.displayMode.resolution, &res) != 0) { fprintf(stderr, "video: videoGetResolution failed\n"); return 0; }

    memset(&vconfig, 0, sizeof(vconfig));
    vconfig.resolution = state.displayMode.resolution;
    vconfig.format = VIDEO_BUFFER_FORMAT_XRGB;
    vconfig.pitch = res.width * sizeof(u32);
    vconfig.aspect = state.displayMode.aspect;
    if (videoConfigure(0, &vconfig, NULL, 0) != 0) { fprintf(stderr, "video: videoConfigure failed\n"); return 0; }
    if (videoGetState(0, 0, &state) != 0) return 0;

    gcmSetFlipMode(GCM_FLIP_VSYNC);
    disp_w = res.width;
    disp_h = res.height;
    if (disp_w > MAX_DIM || disp_h > MAX_DIM) { fprintf(stderr, "video: unsupported mode %dx%d\n", disp_w, disp_h); return 0; }

    for (i = 0; i < NUM_BUFFERS; i++) {
        size_t size = (size_t)disp_w * disp_h * sizeof(u32);
        fb[i] = (u32 *)rsxMemalign(64, size);
        if (!fb[i] || rsxAddressToOffset(fb[i], &fb_offset[i]) != 0) { fprintf(stderr, "video: display buffer %d failed\n", i); return 0; }
        if (gcmSetDisplayBuffer(i, fb_offset[i], disp_w * sizeof(u32), disp_w, disp_h) != 0) { fprintf(stderr, "video: gcmSetDisplayBuffer failed\n"); return 0; }
        memset(fb[i], 0, size); /* black letterbox bars */
    }
    gcmResetFlipStatus();
    do_flip(NUM_BUFFERS - 1);
    cur_buf = 0;
    fprintf(stderr, "video: %dx%d display initialised\n", disp_w, disp_h);
    return 1;
}

void port_video_present(const u32 *src, int w, int h)
{
    int dw, dh, ox, oy, x, y;
    u32 *dst;
    if (init_state == 0) init_state = video_init() ? 1 : -1;
    if (init_state < 0 || w <= 0 || h <= 0) return;

    /* fit w:h into the display keeping the aspect ratio */
    dh = disp_h;
    dw = (int)(((long long)dh * w) / h);
    if (dw > disp_w) { dw = disp_w; dh = (int)(((long long)dw * h) / w); }
    ox = (disp_w - dw) / 2;
    oy = (disp_h - dh) / 2;
    for (x = 0; x < dw; x++) xmap[x] = (int)(((long long)x * w) / dw);

    dst = fb[cur_buf];
    for (y = 0; y < dh; y++) {
        const u32 *srow = src + (size_t)(((long long)y * h) / dh) * w;
        u32 *drow = dst + (size_t)(oy + y) * disp_w + ox;
        for (x = 0; x < dw; x++) drow[x] = srow[xmap[x]];
    }
    if (do_flip(cur_buf)) cur_buf ^= 1;
}

#endif /* PORT_PS3 */
