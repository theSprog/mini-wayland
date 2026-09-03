#include <fcntl.h>
#include <sys/ioctl.h>
#include <drm/drm.h>
#include <stdio.h>

#define EXP_DEV "/dev/dri/card0"
#define IMP_DEV "/dev/dri/renderD129"

int main(void)
{
    int ret = 0;
    int exp_fd = open(EXP_DEV, O_RDWR | O_CLOEXEC);
    int imp_fd = open(IMP_DEV, O_RDWR | O_CLOEXEC);
    printf("exp = %d, imp = %d\n", exp_fd, imp_fd);

    struct drm_mode_create_dumb create = {
        .width = 64,
        .height = 64,
        .bpp = 32,
    };
    ret = ioctl(exp_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
    printf("ret = %d\n", ret);

    struct drm_prime_handle prime = {
        .handle = create.handle,
        .flags = DRM_CLOEXEC | DRM_RDWR,
    };
    ret = ioctl(exp_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
    printf("ret = %d\n", ret);

    ret = ioctl(imp_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
    printf("ret = %d\n", ret);

    return 0;
}

