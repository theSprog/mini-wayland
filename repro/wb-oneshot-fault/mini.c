/* cc -O1 -Wall -o mini mini.c $(pkg-config --cflags --libs libdrm)
 *
 * One writeback commit, then free the buffer. The DPU keeps writing to it:
 *
 *   hygpu: [mmhub0] no-retry page fault ... Faulty UTCL2 client ID: DPU(AXI-1)
 *   hygpu:   RW: 0x1
 *
 * The return codes are all 0. The verdict is in dmesg.
 *
 * IDs below are from probe_kms / probe_writeback on this board.
 */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>

#define DEV       "/dev/dri/card2"
#define CONN      142   /* HDMI-A-1     */
#define WB_CONN   127   /* Writeback-2  */
#define CRTC      84
#define PLANE     87

#define P_MODE_ID   23
#define P_ACTIVE    22
#define P_CRTC_ID   20  /* on connector, writeback connector and plane */
#define P_FB_ID     17
#define P_SRC_X      9
#define P_SRC_Y     10
#define P_SRC_W     11
#define P_SRC_H     12
#define P_CRTC_X    13
#define P_CRTC_Y    14
#define P_CRTC_W    15
#define P_CRTC_H    16
#define P_WB_FB_ID  72


static void fill_color_bars(int fd, uint32_t handle, int w, int h, uint32_t pitch)
{
    static const uint32_t bars[8] = {
        0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
        0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
    };

    struct drm_mode_map_dumb map = { .handle = handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        return;
    }

    size_t size = (size_t)pitch * h;
    uint32_t *pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
    if (pixels == MAP_FAILED) {
        perror("mmap");
        return;
    }

    uint32_t stride = pitch / 4; /* 32 bpp 对应每个像素 4 字节 */

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int bar_idx = (x * 8) / w; /* 等分 8 个彩条，索引范围 0 ~ 7 */
            pixels[y * stride + x] = bars[bar_idx];
        }
    }

    munmap(pixels, size);
}

static uint32_t make_fb(int fd, int w, int h, uint32_t format, uint32_t *handle, uint32_t *pitch)
{
    struct drm_mode_create_dumb create = { .width = w, .height = h, .bpp = 32 };
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    *handle = create.handle;
    if (pitch)
        *pitch = create.pitch;

    uint32_t handles[4] = { create.handle }, pitches[4] = { create.pitch }, offsets[4] = { 0 };
    uint32_t fb = 0;
    int ret = drmModeAddFB2(fd, w, h, format, handles, pitches, offsets, &fb, 0);
    printf("addfb2 ret = %d, fb = %u\n", ret, fb);
    return fb;
}

int main(void)
{
    int fd = open(DEV, O_RDWR | O_CLOEXEC);
    printf("fd = %d\n", fd);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
    drmSetMaster(fd);

    drmModeConnector *conn = drmModeGetConnector(fd, CONN);
    drmModeModeInfo mode = conn->modes[0];
    int w = mode.hdisplay, h = mode.vdisplay;
    printf("mode = %dx%d\n", w, h);

    uint32_t blob = 0;
    drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob);

    uint32_t src_handle = 0, wb_handle = 0;
    uint32_t src_pitch = 0, wb_pitch = 0;

    uint32_t src_fb = make_fb(fd, w, h, DRM_FORMAT_XRGB8888, &src_handle, &src_pitch);
    uint32_t wb_fb  = make_fb(fd, w, h, DRM_FORMAT_ARGB8888, &wb_handle, &wb_pitch);

    fill_color_bars(fd, src_handle, w, h, src_pitch);

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, CONN, P_CRTC_ID, CRTC);
    drmModeAtomicAddProperty(req, CRTC, P_MODE_ID, blob);
    drmModeAtomicAddProperty(req, CRTC, P_ACTIVE, 1);
    drmModeAtomicAddProperty(req, PLANE, P_FB_ID, src_fb);
    drmModeAtomicAddProperty(req, PLANE, P_CRTC_ID, CRTC);
    drmModeAtomicAddProperty(req, PLANE, P_SRC_X, 0);
    drmModeAtomicAddProperty(req, PLANE, P_SRC_Y, 0);
    drmModeAtomicAddProperty(req, PLANE, P_SRC_W, (uint64_t)w << 16);
    drmModeAtomicAddProperty(req, PLANE, P_SRC_H, (uint64_t)h << 16);
    drmModeAtomicAddProperty(req, PLANE, P_CRTC_X, 0);
    drmModeAtomicAddProperty(req, PLANE, P_CRTC_Y, 0);
    drmModeAtomicAddProperty(req, PLANE, P_CRTC_W, w);
    drmModeAtomicAddProperty(req, PLANE, P_CRTC_H, h);
    drmModeAtomicAddProperty(req, WB_CONN, P_CRTC_ID, CRTC);
    drmModeAtomicAddProperty(req, WB_CONN, P_WB_FB_ID, wb_fb);

    int ret = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("commit ret = %d\n", ret);

    /* one writeback job is done; drop the buffer and keep scanning out */
    drmModeRmFB(fd, wb_fb);
    struct drm_mode_destroy_dumb destroy = { .handle = wb_handle };
    ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    printf("writeback buffer freed, watch dmesg for 3s\n");

    sleep(3);
    return 0;
}
