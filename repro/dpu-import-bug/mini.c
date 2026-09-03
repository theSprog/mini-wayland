// gcc -O1 -Wall -o mini mini.c $(pkg-config --cflags --libs libdrm gbm)

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>

#define EXP_DEV "/dev/dri/renderD130"   /* pvr   */
#define IMP_DEV "/dev/dri/card2"        /* vsdrm */

static void fill_bars(uint8_t *base, int w, int h, int stride)
{
    static const uint32_t bars[8] = {
        0x00ffffff, 0x00ffff00, 0x0000ffff, 0x0000ff00,
        0x00ff00ff, 0x00ff0000, 0x000000ff, 0x00000000,
    };
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(base + (long)y * stride);
        for (int x = 0; x < w; x++)
            row[x] = bars[x / (w / 8)];
    }
}

int main(void)
{
    int ret = 0;
    int exp_fd = open(EXP_DEV, O_RDWR | O_CLOEXEC);
    int imp_fd = open(IMP_DEV, O_RDWR | O_CLOEXEC);
    printf("exp = %d, imp = %d\n", exp_fd, imp_fd);
    /* pick the first connected connector and a crtc that can drive it */
    drmModeRes *res = drmModeGetResources(imp_fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < res->count_connectors && !conn; i++) {
        conn = drmModeGetConnector(imp_fd, res->connectors[i]);
        if (conn->connection != DRM_MODE_CONNECTED || !conn->count_modes) {
            drmModeFreeConnector(conn);
            conn = NULL;
        }
    }
    drmModeEncoder *enc = drmModeGetEncoder(imp_fd, conn->encoders[0]);
    uint32_t crtc_id = 0;
    for (int i = 0; i < res->count_crtcs; i++)
        if (enc->possible_crtcs & (1u << i)) { crtc_id = res->crtcs[i]; break; }
    drmModeModeInfo mode = conn->modes[0];
    printf("connector %u, crtc %u, mode %ux%u\n",
           conn->connector_id, crtc_id, mode.hdisplay, mode.vdisplay);
    /* pvr has no dumb_create (ENOSYS), so allocation has to go via gbm */
    struct gbm_device *gbm = gbm_create_device(exp_fd);
    struct gbm_bo *bo = gbm_bo_create(gbm, mode.hdisplay, mode.vdisplay,
                                      GBM_FORMAT_XRGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR);
    uint32_t stride = gbm_bo_get_stride(bo);
    printf("bo = %p, stride = %u\n", (void *)bo, stride);
    /* paint the bars through the allocator's own mapping */
    void *cookie = NULL;
    uint32_t map_stride = 0;
    void *base = gbm_bo_map(bo, 0, 0, mode.hdisplay, mode.vdisplay,
                            GBM_BO_TRANSFER_WRITE, &map_stride, &cookie);
    printf("gbm_bo_map = %p, map stride = %u\n", base, map_stride);
    fill_bars(base, mode.hdisplay, mode.vdisplay, (int)map_stride);
    gbm_bo_unmap(bo, cookie);
    struct drm_prime_handle prime = {
        .fd = gbm_bo_get_fd(bo),
    };
    printf("dmabuf fd = %d\n", prime.fd);
    ret = ioctl(imp_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
    printf("PRIME_FD_TO_HANDLE ret = %d, handle = %u\n", ret, prime.handle);
    uint32_t fb = 0;
    uint32_t handles[4] = { prime.handle }, pitches[4] = { stride },
             offsets[4] = { 0 };
    ret = drmModeAddFB2(imp_fd, mode.hdisplay, mode.vdisplay,
                        DRM_FORMAT_XRGB8888, handles, pitches, offsets, &fb, 0);
    printf("addfb2 ret = %d, fb = %u\n", ret, fb);
    ret = drmModeSetCrtc(imp_fd, crtc_id, fb, 0, 0, &conn->connector_id, 1, &mode);
    printf("setcrtc ret = %d\n", ret);
    printf("\n--- every call above returned 0 ---\n");
    printf("expect: eight colour bars (white yellow cyan green magenta red blue black)\n");
    printf("actual: black screen\n");
    sleep(15);
    return 0;
}