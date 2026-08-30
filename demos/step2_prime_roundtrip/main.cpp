/**
 * demos/step2_prime_roundtrip -- PRIME 导出/导入的正确性验证
 *
 * **只依赖 mw/drm/prime.hpp。不链 GBM、不链 EGL、不需要 master、不需要 root。**
 *
 * 这个约束是刻意的。跨设备导入在很多平台上是一条少有人走的路径，出问题时
 * 需要的是一个几十行的复现程序去调 KMD，而不是在几千行的合成器里找。
 * 所以本 demo 必须能单独跑，也必须能单独编。
 *
 *   step2_prime_roundtrip                        # 自动挑设备
 *   step2_prime_roundtrip -d /dev/dri/cardN      # 指定显示设备
 *   step2_prime_roundtrip -p /dev/dri/cardN      # 指定对端设备，跑跨设备用例
 *   step2_prime_roundtrip -s 1920x1080           # 用真实分辨率（默认 256x256）
 *
 * 用例：
 *
 *   1. 导出：dumb buffer -> dmabuf fd
 *   2. 同设备回环：同一个 dmabuf 导入两次
 *      -> 两次拿到**同一个 handle**，引用计数为 2，只 GEM_CLOSE 一次
 *      这条验证的是 prime.hpp 存在的根本理由。内核对重复导入返回同一个
 *      handle 且**不加引用**，按"一次导入一个 RAII 对象"的直觉写就会双重
 *      释放。NV12 的两个平面常在同一个 dma_buf 里，单帧内就会命中。
 *   3. 导入后注册 fb，验证 fb 建立之后关掉 handle，fb_id 依然有效
 *   4. 跨设备（给了 -r 才跑）：渲染设备分配 -> 显示设备导入
 *      **这条是渲染侧分配路径能否成立的判据。**
 *      失败不算 demo 失败，算一条关于当前平台的结论。
 *
 * 尺寸默认 256x256 是为了快，但 -s 传真实分辨率才能压到 stride 对齐问题：
 * 小 buffer 的 stride 可能小于驱动的对齐值，从而绕过检查。
 *
 * 退出码：0 全过；1 打不开设备；2 有用例失败。
 * 跨设备用例失败只 WARN，不计入退出码 —— 它是一条待测结论，不是回归。
 */
#include <fcntl.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "mw/core/log.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/dumb_buffer.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/trace.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

struct Options {
    std::string display_path{};
    std::string render_path{};
    Size size{256, 256};
};

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  -d <path>   display device (default: first node with KMS resources)\n");
    std::printf("  -p <path>   peer device for the cross-device case; must be a PRIMARY node\n");
    std::printf("              (a cardN without KMS), not a renderDN -- see the note below\n");
    std::printf("  -s WxH      buffer size (default 256x256; use a real mode to\n");
    std::printf("              exercise stride alignment)\n");
    std::printf("  -h          this help\n");
    std::printf("\nneeds neither root nor DRM master.\n");
}

bool parse_size(const char* text, Size& out) {
    unsigned width = 0;
    unsigned height = 0;
    if (std::sscanf(text, "%ux%u", &width, &height) != 2 || width == 0 || height == 0) {
        return false;
    }
    out.width = width;
    out.height = height;
    return true;
}

/// 一条用例的结果。fatal=false 表示"失败也只是结论，不算回归"。
struct CaseResult {
    bool passed = false;
    bool fatal = true;
};

void report_case(const char* name, const CaseResult& result) {
    if (result.passed) {
        LOG_INFO("PASS  {}", name);
    } else if (result.fatal) {
        LOG_ERROR("FAIL  {}", name);
    } else {
        LOG_WARN("N/A   {} (see above; this is a finding, not a regression)", name);
    }
}

// ---------------------------------------------------------------------------

/// 用例 1+2+3：同设备导出 -> 双重导入 -> 注册 fb -> 关 handle -> fb 仍有效
CaseResult same_device_roundtrip(BorrowedFd device, Size size) {
    CaseResult result;

    auto buffer_result = DumbBuffer::create(device, size, Format{DRM_FORMAT_XRGB8888});
    if (! buffer_result) {
        LOG_ERROR("dumb buffer allocation failed: {}", buffer_result.error().message);
        return result;
    }
    const DumbBuffer buffer = std::move(buffer_result).value();
    LOG_INFO("allocated {}", buffer.to_string());

    // ---- 用例 1：导出 ----
    auto fd_result = export_dmabuf(device, buffer.handle(), PrimeAccess::ReadWrite);
    if (! fd_result) {
        LOG_ERROR("PRIME export failed: {}", fd_result.error().message);
        return result;
    }
    const UniqueFd dmabuf = std::move(fd_result).value();

    // ---- 用例 2：同一个 dmabuf 导入两次 ----
    HandleCache cache(device);

    auto first = cache.import(device, dmabuf.borrow());
    if (! first) {
        LOG_ERROR("first import failed: {}", first.error().message);
        return result;
    }
    ImportedHandle handle_a = std::move(first).value();

    auto second = cache.import(device, dmabuf.borrow());
    if (! second) {
        LOG_ERROR("second import failed: {}", second.error().message);
        return result;
    }
    ImportedHandle handle_b = std::move(second).value();

    if (handle_a.handle() != handle_b.handle()) {
        // 如果这里不相等，说明内核**没有**对重复导入做去重。那么 HandleCache
        // 的引用计数就是多余的（虽然无害），但更重要的是：其它假设可能也不成立。
        LOG_ERROR("importing the same dmabuf twice produced different handles ({} vs {}); "
                  "the kernel is expected to deduplicate per drm_file",
                  to_string(handle_a.handle()), to_string(handle_b.handle()));
        return result;
    }

    const uint32_t refs = cache.ref_count(handle_a.handle());
    if (refs != 2) {
        LOG_ERROR("expected refcount 2 after two imports, got {}", refs);
        return result;
    }
    LOG_INFO("two imports of one dmabuf share {} (refcount {})", to_string(handle_a.handle()),
             refs);

    // ---- 用例 3：注册 fb，然后关掉全部 handle，fb 应该仍然有效 ----
    const auto before_close = stats().gem_close;

    FramebufferDesc desc = FramebufferDesc::single_plane(
        size, Format{DRM_FORMAT_XRGB8888}, handle_a.handle(), buffer.pitch());

    auto fb_result = Framebuffer::add_with_fallback(device, desc);
    if (! fb_result) {
        LOG_ERROR("addfb2 on the imported handle failed: {}", fb_result.error().message);

        // stride 对齐是这里的高频失败点，而内核只回一个看不出原因的 EINVAL。
        // 探一次对齐值，把提示说到能行动的粒度。
        const auto alignment = probe_pitch_alignment(device);
        if (! pitch_is_aligned(buffer.pitch(), alignment)) {
            LOG_ERROR("stride {} is not a multiple of the probed alignment {}", buffer.pitch(),
                      *alignment);
        }
        LOG_ERROR("check 'dmesg | tail' for driver-side detail; many KMS drivers log the "
                  "real reason there while returning a bare EINVAL to userspace");
        return result;
    }
    const Framebuffer fb = std::move(fb_result).value();
    LOG_INFO("registered {} from the imported handle", to_string(fb.id()));

    // 显式放掉两个 handle。此时 GEM_CLOSE 应该恰好发生一次（引用 2 -> 0）。
    handle_a.reset();
    if (stats().gem_close != before_close) {
        LOG_ERROR("GEM_CLOSE fired while one reference was still held");
        return result;
    }
    handle_b.reset();
    if (stats().gem_close != before_close + 1) {
        LOG_ERROR("expected exactly one GEM_CLOSE after releasing both references, saw {}",
                  stats().gem_close - before_close);
        return result;
    }
    if (cache.live_count() != 0) {
        LOG_ERROR("cache still holds {} handle(s) after both were released",
                  cache.live_count());
        return result;
    }
    LOG_INFO("both references released, exactly one GEM_CLOSE, cache empty");

    // fb 仍然有效：驱动侧 addfb2 时对 GEM 对象取的引用转移给了 fb 自己，
    // 关掉 handle 不影响它。如果这个假设不成立，下面这行会失败。
    drmModeFBPtr probe = drmModeGetFB(device.get(), static_cast<uint32_t>(fb.id()));
    if (probe == nullptr) {
        LOG_ERROR("the framebuffer became invalid after its GEM handles were closed; "
                  "the driver does not take a reference in addfb2 -- handles must be "
                  "kept alive for the lifetime of the fb on this driver");
        return result;
    }
    drmModeFreeFB(probe);
    LOG_INFO("framebuffer still valid after closing every GEM handle (the fb holds its own "
             "reference)");

    result.passed = true;
    return result;
}

/// 用例 4：渲染设备分配 -> 显示设备导入。渲染侧分配路径的判据。
CaseResult cross_device_import(BorrowedFd display, const std::string& render_path, Size size) {
    CaseResult result;
    result.fatal = false;  // 失败是结论，不是回归

    auto render_fd_result = UniqueFd::open(render_path.c_str(), O_RDWR | O_CLOEXEC);
    if (! render_fd_result) {
        LOG_WARN("cannot open render device {}: {}", render_path,
                 render_fd_result.error().message);
        return result;
    }
    const UniqueFd render = std::move(render_fd_result).value();

    // 用 dumb buffer 而不是 GBM：本 demo 刻意不链 GBM。
    // 如果 render node 没有 dumb 能力，这条用例就跑不了 —— 那不是失败，
    // 只是需要换用 GBM 才能测，留给 Step 2 的后续 demo。
    //
    // ---- 为什么这里不能用 render node ----
    //
    // DRM 核心在 ioctl 分发时有这么一条：
    //
    //   if (!(flags & DRM_RENDER_ALLOW) && file_priv->minor->type == DRM_MINOR_RENDER)
    //           return -EACCES;
    //
    // 而 DRM_IOCTL_MODE_CREATE_DUMB **没有** DRM_RENDER_ALLOW 标志。
    // 所以 dumb 分配在任何 render node 上都会 EACCES —— 这是核心策略，
    // 与驱动无关，也与 DRM_CAP_DUMB_BUFFER 无关（那个 cap 描述的是
    // **设备**支不支持 dumb，不是这个**节点**允不允许调用）。
    //
    // render node 上分配 buffer 只能走驱动私有的 GEM 创建 ioctl，
    // 也就是 GBM 干的事 —— 而本 demo 刻意不链 GBM。
    //
    // 所以这里要找的是"另一个设备的 **primary node**"：它没有 KMS
    // （不是显示设备），但因为是 primary node，dumb ioctl 是放行的。
    // 这样就能在不引入 GBM 的前提下测跨设备导入。

    auto buffer_result = DumbBuffer::create(render.borrow(), size, Format{DRM_FORMAT_XRGB8888});
    if (! buffer_result) {
        LOG_WARN("allocation on {} failed: {}", render_path, buffer_result.error().message);
        return result;
    }
    const DumbBuffer buffer = std::move(buffer_result).value();
    LOG_INFO("allocated on the render device: {}", buffer.to_string());

    auto fd_result = export_dmabuf(render.borrow(), buffer.handle(), PrimeAccess::ReadOnly);
    if (! fd_result) {
        LOG_WARN("PRIME export from {} failed: {}", render_path, fd_result.error().message);
        return result;
    }
    const UniqueFd dmabuf = std::move(fd_result).value();

    // 这一步是真正的判据。显示设备没有自己的 IOMMU/MMU 时，驱动可能要求
    // 导入的内存物理连续；不满足就在这里 EINVAL。
    //
    // TODO(soc-build): 显示设备能否导入取决于它的 GEM 后端。若它与渲染设备
    //                   共用同一个内存管理器，这里通常直接通过；若它自己
    //                   dma_alloc 且没有 IOMMU，则要求导入内存物理连续。
    //                   失败时 dmesg 里的具体报错能区分这两种情况。
    HandleCache cache(display);
    auto imported = cache.import(display, dmabuf.borrow());
    if (! imported) {
        LOG_WARN("cross-device import failed: {}", imported.error().message);
        LOG_WARN("this is the decisive result for the render-allocated path.");
        LOG_WARN("check 'dmesg | tail' -- drivers usually log the real reason there.");
        return result;
    }
    const ImportedHandle handle = std::move(imported).value();
    LOG_INFO("cross-device import succeeded: {} on the display device",
             to_string(handle.handle()));

    // 导入成功不代表能上屏。stride 是渲染设备定的，显示设备未必接受。
    const auto display_alignment = probe_pitch_alignment(display);
    if (! pitch_is_aligned(buffer.pitch(), display_alignment)) {
        LOG_WARN("stride {} from the render device is not a multiple of the display "
                 "device's probed alignment {}; addfb2 is likely to fail",
                 buffer.pitch(), *display_alignment);
    }

    FramebufferDesc desc = FramebufferDesc::single_plane(
        size, Format{DRM_FORMAT_XRGB8888}, handle.handle(), buffer.pitch());

    auto fb_result = Framebuffer::add_with_fallback(display, desc);
    if (! fb_result) {
        LOG_WARN("addfb2 on the cross-device buffer failed: {}", fb_result.error().message);
        LOG_WARN("import worked but the display device will not scan this buffer out");
        return result;
    }
    const Framebuffer fb = std::move(fb_result).value();
    LOG_INFO("registered {} from a render-device buffer -- render-side allocation works",
             to_string(fb.id()));

    result.passed = true;
    return result;
}

/**
 * @brief 挑一个可以当"另一个设备"的 primary node
 *
 * 条件：不是显示设备本身、没有 KMS 资源、支持 dumb。
 * 刻意不选 renderDN —— dumb ioctl 在 render node 上会被 DRM 核心拒掉
 * （见 cross_device_import() 里的说明）。
 */
std::string pick_peer_primary_node(const std::string& display_path) {
    for (const auto& candidate : enumerate_devices()) {
        if (candidate.path == display_path || candidate.has_kms) {
            continue;
        }
        auto fd_result = UniqueFd::open(candidate.path.c_str(), O_RDWR | O_CLOEXEC);
        if (! fd_result) {
            continue;
        }
        const UniqueFd fd = std::move(fd_result).value();
        uint64_t has_dumb = 0;
        if (drmGetCap(fd.get(), DRM_CAP_DUMB_BUFFER, &has_dumb) == 0 && has_dumb != 0) {
            return candidate.path;
        }
    }
    return {};
}

/// 没指定显示设备时，挑第一个有 KMS 资源的
std::string pick_display_node() {
    for (const auto& candidate : enumerate_devices()) {
        if (candidate.has_kms) {
            return candidate.path;
        }
    }
    return {};
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "-d") == 0 && i + 1 < argc) {
            options.display_path = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-p") == 0 && i + 1 < argc) {
            options.render_path = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-s") == 0 && i + 1 < argc) {
            if (! parse_size(argv[++i], options.size)) {
                LOG_ERROR("bad size '{}', expected WxH", argv[i]);
                return 1;
            }
            continue;
        }
        LOG_ERROR("unknown option: {}", arg);
        print_usage(argv[0]);
        return 1;
    }

    if (options.display_path.empty()) {
        options.display_path = pick_display_node();
        if (options.display_path.empty()) {
            LOG_ERROR("no node with KMS resources found; pass one with -d");
            return 1;
        }
    }

    // 自动挑对端：另一个设备的 **primary node**（cardN，无 KMS）。
    // 不能用 renderDN —— dumb ioctl 在 render node 上被核心拒掉，见下面的说明。
    if (options.render_path.empty()) {
        options.render_path = pick_peer_primary_node(options.display_path);
        if (! options.render_path.empty()) {
            LOG_INFO("using peer primary node {}", options.render_path);
        }
    }

    auto display_result = UniqueFd::open(options.display_path.c_str(), O_RDWR | O_CLOEXEC);
    if (! display_result) {
        LOG_ERROR("cannot open {}: {}", options.display_path,
                  display_result.error().message);
        return 1;
    }
    const UniqueFd display = std::move(display_result).value();

    LOG_INFO("display device: {}", options.display_path);
    LOG_INFO("buffer size:    {}", to_string(options.size));
    if (const auto alignment = probe_pitch_alignment(display.borrow())) {
        LOG_INFO("pitch alignment on the display device: {} bytes", *alignment);
    }
    LOG_INFO("");

    bool failed = false;

    LOG_INFO("--- same-device roundtrip ---");
    {
        LOG_SCOPE();
        const CaseResult result = same_device_roundtrip(display.borrow(), options.size);
        report_case("same-device export/import/addfb", result);
        failed = failed || (! result.passed && result.fatal);
    }

    LOG_INFO("");
    LOG_INFO("--- cross-device import (render-side allocation) ---");
    {
        LOG_SCOPE();
        if (options.render_path.empty()) {
            LOG_WARN("no peer primary node found; skipping. Pass one with -p.");
        } else {
            const CaseResult result =
                cross_device_import(display.borrow(), options.render_path, options.size);
                report_case("render-device allocation imported by the display device", result);
        }
    }

    LOG_INFO("");
    report_leaks_on_exit();
    return failed ? 2 : 0;
}
