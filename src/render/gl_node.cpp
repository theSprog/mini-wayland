#include "mw/render/gl_node.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <xf86drm.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/prime.hpp"
#include "mw/egl/display.hpp"
#include "mw/gbm/device.hpp"
#include "mw/render/buffer_source.hpp"

namespace mw::render {

namespace {

/// `/dev/dri` 下所有存在的 render node，按编号升序
std::vector<std::string> list_render_nodes() {
    std::vector<std::string> paths;
    // 按编号试探而不是读目录：编号空间很小，且这样输出顺序稳定，
    // 两次运行的结果可以直接 diff。
    for (int minor = 128; minor < 144; ++minor) {
        std::string path = fmt("/dev/dri/renderD{}", minor);
        auto fd = UniqueFd::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd) {
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

std::string drm_driver_name(const std::string& path) {
    auto fd = UniqueFd::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (! fd) {
        return {};
    }
    drmVersionPtr version = drmGetVersion(fd.value().get());
    if (version == nullptr) {
        return {};
    }
    std::string name(version->name != nullptr ? version->name : "");
    drmFreeVersion(version);
    return name;
}

/**
 * @brief 方向一：外来 dmabuf -> 本节点的渲染目标
 *
 * 在 KMS 设备上分配一块 buffer，导入到本节点的 GL 上下文里当渲染目标。
 * 这是"显示设备分配、GPU 来画"那条路的可行性判据。
 */
void probe_import_direction(GlNode& node, const egl::Display& display, BorrowedFd kms_fd,
                            drm::Size size) {
    drm::HandleCache cache(kms_fd);
    auto source = make_scanout_device_source(kms_fd, cache);
    if (! source) {
        node.detail = fmt("no buffer to import: {}", source.error().message);
        return;
    }

    AllocRequest request;
    request.size = size;
    request.format = drm::Format{DRM_FORMAT_XRGB8888};
    auto buffer = source.value()->allocate(request);
    if (! buffer) {
        node.detail = fmt("no buffer to import: {}", buffer.error().message);
        return;
    }

    auto target = GlRenderTarget::create(display, buffer.value());
    if (! target) {
        node.detail = fmt("an externally allocated buffer cannot be used as a render target "
                          "here: {}",
                          target.error().message);
        return;
    }
    node.renders_into_imported = true;
    node.attach_kind = target.value().attach_kind();
}

/**
 * @brief 方向二：本节点分配 -> KMS 设备扫描输出
 *
 * 拆成两步记录：分配本身成不成，以及分配出来的东西 KMS 设备认不认。
 * 二者的失败原因完全不同 —— 前者是这个节点没有可扫描输出的内存池，
 * 后者是两个设备的内存管理关系不通（对齐、连续性、IOMMU 映射）。
 * 合成一条会把一个很具体的问题变成一句"不支持"。
 */
void probe_export_direction(GlNode& node, BorrowedFd kms_fd, drm::Size size) {
    if (! kms_fd.valid()) {
        return;
    }
    drm::HandleCache cache(kms_fd);
    auto source = make_render_device_source(kms_fd, node.path, cache);
    if (! source) {
        return;
    }

    AllocRequest request;
    request.size = size;
    request.format = drm::Format{DRM_FORMAT_XRGB8888};
    auto buffer = source.value()->allocate(request);
    if (buffer) {
        node.allocates_scanout = true;
        node.scanout_accepted_by_kms = true;
        return;
    }

    // 分配与注册在同一个调用里，靠错误消息分不开。再单独试一次纯分配，
    // 就能把"分不出来"和"分出来了但对方不收"区分开。
    auto device = gbm::Device::open(node.path);
    if (! device) {
        return;
    }
    auto bo = device.value().allocate(size, drm::Format{DRM_FORMAT_XRGB8888},
                                      span<const drm::Modifier>{},
                                      gbm::Usage::Scanout | gbm::Usage::Rendering);
    node.allocates_scanout = bo.has_value();
}

} // namespace

// ---------------------------------------------------------------------------

bool GlNode::looks_like_software() const noexcept {
    // 通用图形栈里几个众所周知的软件后备实现。**不是厂商判断** ——
    // 这些名字在任何机器上都可能出现，与具体板卡无关。
    // 只用于打破平局和提示，主逻辑不读它。
    static const char* const kSoftwareNames[] = {"llvmpipe", "softpipe", "swrast", "SWR"};
    for (const char* name : kSoftwareNames) {
        if (gl_renderer.find(name) != std::string::npos) {
            return true;
        }
    }
    return false;
}

int GlNode::rank() const noexcept {
    if (! egl) {
        return 0;
    }
    int score = 1;
    // 能把外来 buffer 当渲染目标是最重要的一条：合成器一定要做这件事
    // （客户端送来的 buffer 要能采样、自己的输出 buffer 要能画）。
    if (renders_into_imported) {
        score += 8;
    }
    if (allocates_scanout) {
        score += 2;
    }
    if (scanout_accepted_by_kms) {
        score += 2;
    }
    // 软件光栅化能通过上面全部判据，只是慢。所以它必须在最后被压下去，
    // 否则一个"什么都能过"的软件后端会盖住真正的硬件节点。
    if (! looks_like_software()) {
        score += 32;
    }
    return score;
}

std::string GlNode::to_line() const {
    auto yn = [](bool value) { return value ? "yes" : "no "; };
    return fmt("{:<22} {:<10} gbm={} egl={} import={} alloc={} scanout={} {}", path,
               drm_driver.empty() ? "?" : drm_driver, yn(gbm), yn(egl),
               yn(renders_into_imported), yn(allocates_scanout), yn(scanout_accepted_by_kms),
               gl_renderer.empty() ? std::string("-") : gl_renderer);
}

// ---------------------------------------------------------------------------

std::vector<GlNode> probe_gl_nodes(const GlNodeProbe& probe) {
    std::vector<std::string> candidates = list_render_nodes();
    // KMS 节点也是候选。在显示驱动自带渲染、或者用户态用粘合层把显示节点
    // 接进 GL 栈的拓扑下，能跑 GL 的恰恰是它。
    if (! probe.kms_path.empty() &&
        std::find(candidates.begin(), candidates.end(), probe.kms_path) == candidates.end()) {
        candidates.push_back(probe.kms_path);
    }

    std::vector<GlNode> results;
    results.reserve(candidates.size());

    for (const auto& path : candidates) {
        GlNode node;
        node.path = path;
        node.drm_driver = drm_driver_name(path);

        LOG_DEBUG("probing {} as a GL host", path);
        LOG_SCOPE();

        auto device = gbm::Device::open(path);
        if (! device) {
            node.detail = device.error().message;
            results.push_back(std::move(node));
            continue;
        }
        node.gbm = true;

        // Display 必须比 target 活得久，所以先声明。
        auto display_result = egl::Display::create(device.value());
        if (! display_result) {
            node.detail = display_result.error().message;
            results.push_back(std::move(node));
            continue;
        }
        const egl::Display display = std::move(display_result).value();
        node.egl = true;
        node.egl_version = display.caps().version;
        node.gl_renderer = display.caps().gl_renderer;
        node.gl_version = display.caps().gl_version;
        node.egl_import_modifiers = display.caps().dmabuf_import_modifiers;
        node.egl_native_fence_sync = display.caps().native_fence_sync;

        // syncobj 是**渲染侧**特性。在别的节点上问会得到一个与本节点无关的
        // 答案，而这个答案会决定 Step 6 走 syncobj timeline 还是 sync_file。
        if (auto node_fd = UniqueFd::open(path.c_str(), O_RDWR | O_CLOEXEC)) {
            uint64_t value = 0;
            if (drmGetCap(node_fd.value().get(), DRM_CAP_SYNCOBJ, &value) == 0) {
                node.syncobj = value != 0;
            }
            value = 0;
            if (drmGetCap(node_fd.value().get(), DRM_CAP_SYNCOBJ_TIMELINE, &value) == 0) {
                node.syncobj_timeline = value != 0;
            }
        }

        if (probe.kms_fd.valid()) {
            probe_import_direction(node, display, probe.kms_fd, probe.test_size);
            probe_export_direction(node, probe.kms_fd, probe.test_size);
        } else {
            node.detail = "no KMS device given; only GBM and EGL were checked";
        }

        results.push_back(std::move(node));
    }

    return results;
}

const GlNode* best_gl_node(span<const GlNode> nodes) noexcept {
    const GlNode* best = nullptr;
    for (const GlNode& node : nodes) {
        if (node.rank() <= 0) {
            continue;
        }
        if (best == nullptr || node.rank() > best->rank()) {
            best = &node;
        }
    }
    return best;
}

} // namespace mw::render
