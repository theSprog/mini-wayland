#include "mw/render/gl_node.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/trace.hpp"
#include "mw/egl/display.hpp"
#include "mw/gbm/device.hpp"
#include "mw/render/buffer_source.hpp"

namespace mw::render {

namespace {

/**
 * @brief 子进程回传给父进程的结果
 *
 * 定长 POD。父子是同一个可执行文件、同一份布局，所以直接按字节读写，
 * 不做序列化。字符串必须是定长数组 —— 管道里传 std::string 的指针没有意义。
 */
struct Wire {
    bool gbm = false;
    bool egl = false;
    bool renders_into_imported = false;
    bool allocates_scanout = false;
    bool scanout_accepted_by_kms = false;
    bool egl_import_modifiers = false;
    bool egl_native_fence_sync = false;
    bool syncobj = false;
    bool syncobj_timeline = false;
    bool renderbuffer_path = false;  ///< true = AttachKind::Renderbuffer
    char gl_renderer[128] = {};
    char gl_version[128] = {};
    char egl_version[32] = {};
    char detail[512] = {};
};

void put(char* dst, size_t capacity, const std::string& value) {
    const size_t length = std::min(value.size(), capacity - 1u);
    std::memcpy(dst, value.data(), length);
    dst[length] = '\0';
}

/// `/dev/dri` 下所有能打开的节点。render node 排前面，只是为了输出好读。
std::vector<std::string> list_candidate_nodes(const std::string& kms_path) {
    std::vector<std::string> paths;

    // 按编号试探而不是读目录：编号空间很小，且这样输出顺序稳定，
    // 两次运行的结果可以直接 diff。
    auto scan = [&paths](const char* prefix, int first, int last) {
        for (int minor = first; minor < last; ++minor) {
            std::string path = fmt("/dev/dri/{}{}", prefix, minor);
            if (auto fd = UniqueFd::open(path.c_str(), O_RDWR | O_CLOEXEC)) {
                paths.push_back(std::move(path));
            }
        }
    };
    scan("renderD", 128, 144);
    // primary node 也测：有的驱动只在 primary 上暴露完整的 GL 栈，
    // 而且显示节点本身就是候选之一。
    scan("card", 0, 16);

    if (! kms_path.empty() && std::find(paths.begin(), paths.end(), kms_path) == paths.end()) {
        paths.push_back(kms_path);
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
void probe_import_direction(Wire& wire, const egl::Display& display, BorrowedFd kms_fd,
                            drm::Size size) {
    drm::HandleCache cache(kms_fd);
    auto source = make_scanout_device_source(kms_fd, cache);
    if (! source) {
        put(wire.detail, sizeof(wire.detail),
            fmt("no buffer to import: {}", source.error().message));
        return;
    }

    AllocRequest request;
    request.size = size;
    request.format = drm::Format{DRM_FORMAT_XRGB8888};
    auto buffer = source.value()->allocate(request);
    if (! buffer) {
        put(wire.detail, sizeof(wire.detail),
            fmt("no buffer to import: {}", buffer.error().message));
        return;
    }

    auto target = GlRenderTarget::create(display, buffer.value());
    if (! target) {
        put(wire.detail, sizeof(wire.detail),
            fmt("an externally allocated buffer cannot be used as a render target here: {}",
                target.error().message));
        return;
    }
    wire.renders_into_imported = true;
    wire.renderbuffer_path = target.value().attach_kind() == AttachKind::Renderbuffer;
}

/**
 * @brief 方向二：本节点分配 -> KMS 设备扫描输出
 *
 * 拆成两步记录：分配本身成不成，以及分出来的东西 KMS 设备认不认。
 * 二者的失败原因完全不同 —— 前者是这个节点没有可扫描输出的内存池，
 * 后者是两个设备的内存管理关系不通（对齐、连续性、IOMMU 映射）。
 * 合成一条会把一个很具体的问题变成一句"不支持"。
 */
void probe_export_direction(Wire& wire, const std::string& path, BorrowedFd kms_fd,
                            drm::Size size) {
    drm::HandleCache cache(kms_fd);
    auto source = make_render_device_source(kms_fd, path, cache);
    if (! source) {
        return;
    }

    AllocRequest request;
    request.size = size;
    request.format = drm::Format{DRM_FORMAT_XRGB8888};
    if (source.value()->allocate(request)) {
        wire.allocates_scanout = true;
        wire.scanout_accepted_by_kms = true;
        return;
    }

    // 分配与注册在同一个调用里，靠错误消息分不开。再单独试一次纯分配，
    // 就能把"分不出来"和"分出来了但对方不收"区分开。
    auto device = gbm::Device::open(path);
    if (! device) {
        return;
    }
    wire.allocates_scanout =
        device.value()
            .allocate(size, drm::Format{DRM_FORMAT_XRGB8888}, span<const drm::Modifier>{},
                      gbm::Usage::Scanout | gbm::Usage::Rendering)
            .has_value();
}

/// 单个节点的全部探测工作。**通常在子进程里执行**，不要在这里改全局状态。
void probe_one(const std::string& path, const GlNodeProbe& probe, Wire& wire) {
    // KMS fd 在这里自己打开，不用父进程传进来的：这样即使本函数崩了，
    // 它建过的 GEM 对象与 fb 都随这个 fd 一起消失，不会留在父进程的 fd 上
    // 变成没人认领的内核对象。
    UniqueFd kms_fd;
    if (! probe.kms_path.empty()) {
        if (auto opened = UniqueFd::open(probe.kms_path.c_str(), O_RDWR | O_CLOEXEC)) {
            kms_fd = std::move(opened).value();
        }
    }

    auto device = gbm::Device::open(path);
    if (! device) {
        put(wire.detail, sizeof(wire.detail), device.error().message);
        return;
    }
    wire.gbm = true;

    auto display_result = egl::Display::create(device.value());
    if (! display_result) {
        put(wire.detail, sizeof(wire.detail), display_result.error().message);
        return;
    }
    const egl::Display display = std::move(display_result).value();
    wire.egl = true;

    const egl::Caps& caps = display.caps();
    put(wire.egl_version, sizeof(wire.egl_version), caps.version);
    put(wire.gl_renderer, sizeof(wire.gl_renderer), caps.gl_renderer);
    put(wire.gl_version, sizeof(wire.gl_version), caps.gl_version);
    wire.egl_import_modifiers = caps.dmabuf_import_modifiers;
    wire.egl_native_fence_sync = caps.native_fence_sync;

    // syncobj 是**渲染侧**特性。在别的节点上问会得到一个与本节点无关的
    // 答案，而这个答案会决定 Step 6 走 syncobj timeline 还是 sync_file。
    if (auto node_fd = UniqueFd::open(path.c_str(), O_RDWR | O_CLOEXEC)) {
        uint64_t value = 0;
        if (drmGetCap(node_fd.value().get(), DRM_CAP_SYNCOBJ, &value) == 0) {
            wire.syncobj = value != 0;
        }
        value = 0;
        if (drmGetCap(node_fd.value().get(), DRM_CAP_SYNCOBJ_TIMELINE, &value) == 0) {
            wire.syncobj_timeline = value != 0;
        }
    }

    if (! kms_fd.valid()) {
        put(wire.detail, sizeof(wire.detail),
            "no KMS device available; only GBM and EGL were checked");
        return;
    }
    probe_import_direction(wire, display, kms_fd.borrow(), probe.test_size);
    probe_export_direction(wire, path, kms_fd.borrow(), probe.test_size);
}

void apply(GlNode& node, const Wire& wire) {
    node.gbm = wire.gbm;
    node.egl = wire.egl;
    node.renders_into_imported = wire.renders_into_imported;
    node.allocates_scanout = wire.allocates_scanout;
    node.scanout_accepted_by_kms = wire.scanout_accepted_by_kms;
    node.egl_import_modifiers = wire.egl_import_modifiers;
    node.egl_native_fence_sync = wire.egl_native_fence_sync;
    node.syncobj = wire.syncobj;
    node.syncobj_timeline = wire.syncobj_timeline;
    node.attach_kind = wire.renderbuffer_path ? AttachKind::Renderbuffer : AttachKind::Texture;
    node.gl_renderer = wire.gl_renderer;
    node.gl_version = wire.gl_version;
    node.egl_version = wire.egl_version;
    node.detail = wire.detail;
}

/// 读满或失败。管道一次 read 不保证给全。
bool read_exactly(int fd, void* buffer, size_t size) {
    auto* out = static_cast<uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::read(fd, out + done, size - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n == 0) {
            return false;  // 子进程没写完就没了
        } else if (errno != EINTR) {
            return false;
        }
    }
    return true;
}

bool write_exactly(int fd, const void* buffer, size_t size) {
    const auto* in = static_cast<const uint8_t*>(buffer);
    size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, in + done, size - done);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && errno != EINTR) {
            return false;
        }
    }
    return true;
}

GlNode probe_node(const std::string& path, const GlNodeProbe& probe) {
    GlNode node;
    node.path = path;
    node.drm_driver = drm_driver_name(path);
    node.same_device_as_kms = ! probe.kms_path.empty() && path == probe.kms_path;

    if (std::find(probe.skip.begin(), probe.skip.end(), path) != probe.skip.end()) {
        node.skipped = true;
        node.detail = "skipped on request";
        return node;
    }

    if (! probe.isolate) {
        Wire wire;
        probe_one(path, probe, wire);
        apply(node, wire);
        return node;
    }

    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
        node.detail = fmt("cannot create a pipe to isolate the probe: {}", drm::errno_name(errno));
        return node;
    }
    UniqueFd read_end(pipe_fds[0]);
    UniqueFd write_end(pipe_fds[1]);

    const pid_t pid = ::fork();
    if (pid < 0) {
        node.detail = fmt("cannot fork to isolate the probe: {}", drm::errno_name(errno));
        return node;
    }

    if (pid == 0) {
        // ---- 子进程 ----
        read_end.reset();
        Wire wire;
        probe_one(path, probe, wire);
        // probe_one 的局部对象已经析构完，内核资源都还回去了。
        // 用 _exit 而不是 exit：不跑 atexit，也不重复 flush 从父进程继承来的
        // stdio 缓冲（那会让同一行日志出现两次）。
        const bool sent = write_exactly(write_end.get(), &wire, sizeof(wire));
        ::_exit(sent ? 0 : 1);
    }

    // ---- 父进程 ----
    write_end.reset();  // 不关掉的话下面永远读不到 EOF
    Wire wire;
    const bool got_result = read_exactly(read_end.get(), &wire, sizeof(wire));

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        // 被信号打断，重试
    }

    if (WIFSIGNALED(status)) {
        node.crashed = true;
        // 这不是工具故障，是关于这个节点的结论：它的用户态驱动在被这样使用时
        // 会崩。父进程照常继续测下一个节点 —— 隔离存在的全部意义就在这里。
        node.detail = fmt("the probe died on signal {}; this node is unusable. Check dmesg: "
                          "a kernel BUG_ON kills the task the same way a userspace crash "
                          "does, and only the kernel log tells them apart. Pass -x {} to "
                          "stop poking it on every run",
                          WTERMSIG(status), path);
        LOG_WARN("probing {} killed the child process (signal {}); continuing with the "
                 "remaining candidates -- check dmesg, this may be a kernel oops rather "
                 "than a userspace crash",
                 path, WTERMSIG(status));
        return node;
    }
    if (! got_result) {
        node.detail = "the probe subprocess exited without reporting a result";
        return node;
    }
    apply(node, wire);
    return node;
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
    if (crashed || skipped || ! egl) {
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
    // 只有跨设备成立才算数。候选就是显示设备自己时，"显示设备收得下它自己
    // 分配的东西"是恒真的，给它加分会让显示节点凭一条空结论压过真正的
    // 渲染节点 —— 实测里就发生过。
    if (scanout_accepted_by_kms && ! same_device_as_kms) {
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
    std::string renderer = "-";
    if (skipped) {
        renderer = "<skipped>";
    } else if (crashed) {
        renderer = "<crashed, see dmesg>";
    } else if (! gl_renderer.empty()) {
        renderer = gl_renderer;
    }
    // 退化的那一项标成 "-"，不让它看起来像一条跨设备结论
    const char* scanout = same_device_as_kms
                              ? "-  "
                              : (scanout_accepted_by_kms ? "yes" : "no ");
    return fmt("{:<22} {:<10} gbm={} egl={} import={} alloc={} scanout={} {}{}", path,
               drm_driver.empty() ? std::string("?") : drm_driver, yn(gbm), yn(egl),
               yn(renders_into_imported), yn(allocates_scanout), scanout, renderer,
               same_device_as_kms ? "  (is the display device)" : "");
}

// ---------------------------------------------------------------------------

std::vector<GlNode> probe_gl_nodes(const GlNodeProbe& probe) {
    const std::vector<std::string> candidates = list_candidate_nodes(probe.kms_path);

    std::vector<GlNode> results;
    results.reserve(candidates.size());
    for (const auto& path : candidates) {
        LOG_DEBUG("probing {} as a GL host", path);
        results.push_back(probe_node(path, probe));
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
