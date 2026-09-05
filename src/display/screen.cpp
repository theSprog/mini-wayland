#include <cstdio>
#include <ctime>
#include <optional>
#include <vector>

#include "mw/internal/error.hpp"
#include "mw/trace/log.hpp"
#include "mw/drm/atomic.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/property.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/trace.hpp"
#include "mw/render/swapchain.hpp"
#include "mw/display/screen.hpp"

using internal::Ok;
using internal::Err;
using internal::sys_err_ctx;

namespace mw::display {

using drm::AtomicRequest;
using drm::CommitFlags;
using drm::Connector;
using drm::Crtc;
using drm::CrtcRect;
using drm::Errc;
using drm::FlipEvent;
using drm::FrameStats;
using drm::Plane;
using drm::PlaneType;
using drm::PropertyBlob;
using drm::SrcRect;

const char* to_string(Backend backend) noexcept {
    switch (backend) {
        case Backend::Kms:
            return "kms";
        case Backend::Offscreen:
            return "offscreen";
    }
    return "?";
}

span<uint8_t> Frame::row(uint32_t y) const noexcept {
    if (y >= size.height || stride == 0) {
        return {};
    }
    const size_t offset = static_cast<size_t>(y) * stride;
    if (offset + stride > pixels.size()) {
        return {};
    }
    return span<uint8_t>(pixels.data() + offset, stride);
}

namespace {

uint64_t now_ns() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

void sleep_until_ns(uint64_t deadline) noexcept {
    timespec ts{};
    ts.tv_sec = static_cast<time_t>(deadline / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(deadline % 1000000000ULL);
    // TIMER_ABSTIME：睡到一个绝对时刻，而不是"再睡 N 纳秒"。
    // 后者会把每次唤醒的调度抖动累加进去，几百帧之后就明显偏慢。
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
    }
}

/// 该 plane 在给定 format 下宣告支持的全部 modifier
std::vector<drm::Modifier> plane_modifiers(const Plane& plane, Format format) {
    std::vector<drm::Modifier> out;
    for (const drm::FormatModifier& fm : plane.formats) {
        if (fm.format == format && fm.modifier != drm::kModifierInvalid) {
            out.push_back(fm.modifier);
        }
    }
    return out;
}

Status write_ppm(const std::string& path, const Frame& frame) {
    const uint32_t bpp = drm::bytes_per_pixel(frame.format);
    if (bpp != 4u) {
        // 只支持 32 位 packed。别的格式要落盘请自己转 —— 在这里堆一张
        // 格式转换表，等于把这个诊断用的小功能变成一个像素格式库。
        return Err(Errc::Unsupported,
                   fmt("PPM dump only supports 32-bit packed formats, got {}",
                       drm::to_string(frame.format)));
    }

    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        return sys_err_ctx("fopen", path);
    }

    std::fprintf(file, "P6\n%u %u\n255\n", frame.size.width, frame.size.height);
    std::vector<uint8_t> line(static_cast<size_t>(frame.size.width) * 3u);
    for (uint32_t y = 0; y < frame.size.height; ++y) {
        const span<uint8_t> src = frame.row(y);
        if (src.empty()) {
            break;
        }
        for (uint32_t x = 0; x < frame.size.width; ++x) {
            const uint8_t* px = src.data() + static_cast<size_t>(x) * 4u;
            // XRGB8888 在小端机器上的字节序是 B G R X
            line[static_cast<size_t>(x) * 3u + 0u] = px[2];
            line[static_cast<size_t>(x) * 3u + 1u] = px[1];
            line[static_cast<size_t>(x) * 3u + 2u] = px[0];
        }
        std::fwrite(line.data(), 1, line.size(), file);
    }
    std::fclose(file);
    return Ok();
}

} // namespace

// ---------------------------------------------------------------------------

struct ScreenImpl {
    ScreenConfig config{};

    Size size{};
    Format format{};
    uint32_t refresh_mhz = 0;
    uint64_t frame_ns = 0;

    uint64_t frame_index = 0;
    uint32_t in_flight = 0;
    bool frame_open = false;  ///< begin_frame 之后、submit 之前
    FrameStats stats{};

    // ---- KMS 后端 ----
    // 声明顺序即析构逆序，不要调整：
    // chain(持 fb 与 dmabuf) -> source -> handles -> blob/request -> master -> device
    std::optional<drm::Device> device{};
    drm::MasterGuard master{};
    drm::OutputPath path{};
    std::optional<AtomicRequest> request{};
    PropertyBlob mode_blob{};
    std::unique_ptr<drm::HandleCache> handles{};
    std::unique_ptr<render::BufferSource> source{};
    render::Swapchain chain{};
    bool modeset_done = false;

    // ---- Offscreen 后端 ----
    std::vector<std::vector<uint8_t>> offscreen{};
    uint32_t offscreen_next = 0;
    uint32_t offscreen_current = 0;
    uint32_t offscreen_stride = 0;
    uint64_t next_deadline = 0;

    ~ScreenImpl() {
        if (config.backend == Backend::Kms && modeset_done) {
            teardown();
        }
    }

    ScreenImpl() = default;
    ScreenImpl(ScreenImpl&&) = delete;
    ScreenImpl& operator=(ScreenImpl&&) = delete;
    ScreenImpl(const ScreenImpl&) = delete;
    ScreenImpl& operator=(const ScreenImpl&) = delete;

    /// 关掉这条通路上的一切。**顺序不能反**：先让所有 plane 松开 fb，
    /// 再关 CRTC，最后解绑 connector。留一个还指着 fb 的 plane 就走，
    /// 下一个进程接管时会看到一个自己没设置过的状态。
    void teardown() noexcept {
        if (! device.has_value() || ! request.has_value()) {
            return;
        }
        const Connector* connector = device->connector(path.connector);
        const Crtc* crtc = device->crtc(path.crtc);
        if (connector == nullptr || crtc == nullptr) {
            LOG_WARN("cannot tear down: objects vanished from the snapshot");
            return;
        }

        request->reset();
        for (const PlaneType type : {PlaneType::Primary, PlaneType::Overlay, PlaneType::Cursor}) {
            for (const drm::PlaneId id : device->planes_for_crtc(path.crtc, type)) {
                const Plane* plane = device->plane(id);
                if (plane == nullptr) {
                    continue;
                }
                if (auto status = request->disable_plane(*plane); ! status) {
                    log_error_object(status.error(), "disable_plane");
                }
            }
        }
        if (auto status = request->disable_crtc(*crtc); ! status) {
            log_error_object(status.error(), "disable_crtc");
        }
        if (auto status = request->bind_connector(*connector, drm::kNoCrtc); ! status) {
            log_error_object(status.error(), "unbind connector");
        }
        if (auto status = request->commit(CommitFlags::AllowModeset); ! status) {
            log_error_object(status.error(), "teardown commit");
            LOG_WARN("the display may be left in an inconsistent state");
        }
        modeset_done = false;
    }
};

// ---------------------------------------------------------------------------

Screen::Screen() noexcept : impl_(nullptr) {}
Screen::~Screen() = default;
Screen::Screen(Screen&&) noexcept = default;
Screen& Screen::operator=(Screen&&) noexcept = default;
Screen::Screen(std::unique_ptr<ScreenImpl> impl) noexcept : impl_(std::move(impl)) {}

// ---------------------------------------------------------------------------
// 打开
// ---------------------------------------------------------------------------

namespace {

Status open_offscreen(ScreenImpl& impl) {
    const ScreenConfig& config = impl.config;
    const uint32_t bpp = drm::bytes_per_pixel(config.format);
    if (bpp == 0u) {
        return Err(Errc::Unsupported,
                   fmt("offscreen backend needs a single-plane packed format, got {}",
                       drm::to_string(config.format)));
    }
    if (config.offscreen_size.empty()) {
        return Err(Errc::Internal, "offscreen_size must not be empty");
    }

    impl.size = config.offscreen_size;
    impl.format = config.format;
    impl.refresh_mhz = config.offscreen_refresh_mhz;
    impl.frame_ns = config.offscreen_refresh_mhz == 0u
                        ? 0u
                        : 1000000000000ULL / config.offscreen_refresh_mhz;

    // 刻意**不**做任何对齐。真实驱动会把 stride 对齐到 64/256 字节，
    // 这里故意不模仿：一个"差不多像硬件"的假 stride 会让人以为自己的代码
    // 处理好了对齐，而实际上只是恰好对上了。要验证对齐必须上真硬件。
    impl.offscreen_stride = impl.size.width * bpp;

    impl.offscreen.resize(config.buffer_count);
    for (std::vector<uint8_t>& buf : impl.offscreen) {
        buf.assign(static_cast<size_t>(impl.offscreen_stride) * impl.size.height, 0u);
    }
    impl.next_deadline = now_ns();
    return Ok();
}

Result<drm::Device> open_kms_device(const ScreenConfig& config) {
    if (! config.device_path.empty()) {
        return drm::Device::open(config.device_path);
    }
    return drm::Device::open_first_kms();
}

Status open_kms(ScreenImpl& impl) {
    const ScreenConfig& config = impl.config;

    impl.device = TRY(open_kms_device(config));
    LOG_INFO("using {}", impl.device->path());

    if (! impl.device->caps().atomic) {
        return Err(Errc::NoAtomicSupport, "this device does not support atomic modesetting");
    }

    impl.master = TRY(impl.device->acquire_master());

    drm::OutputRequest request{};
    request.mode_size = config.mode_size;
    impl.path = TRY(impl.device->pick_output(request));
    TRY(impl.device->validate(impl.path));
    LOG_INFO("{}", impl.path.to_string());

    impl.size = impl.path.size();
    impl.format = config.format;
    impl.refresh_mhz = impl.path.mode.refresh_mhz();
    impl.frame_ns = impl.path.mode.frame_duration_ns();

    const Plane* plane = impl.device->plane(impl.path.primary_plane);
    if (plane == nullptr) {
        return Err(Errc::NoCompatiblePlane, "primary plane vanished from the snapshot");
    }
    if (! plane->supports_format(impl.format)) {
        return Err(Errc::Unsupported,
                   fmt("primary plane {} does not advertise format {}",
                       drm::to_string(impl.path.primary_plane), drm::to_string(impl.format)));
    }

    impl.handles = std::make_unique<drm::HandleCache>(impl.device->fd());

    if (config.source == render::SourceKind::RenderDevice) {
        std::string node = config.render_node;
        if (node.empty()) {
            std::optional<std::string> found = drm::find_render_node(impl.device->path());
            if (! found.has_value()) {
                return Err(Errc::Unsupported,
                           "source=RenderDevice was requested but no render node could be "
                           "inferred; pass ScreenConfig::render_node explicitly");
            }
            node = *found;
        }
        impl.source = TRY(render::make_render_device_source(impl.device->fd(), node, *impl.handles));
    } else {
        impl.source = TRY(render::make_scanout_device_source(impl.device->fd(), *impl.handles));
    }
    LOG_INFO("buffer source: {}", impl.source->describe());

    // modifier 候选原样来自 plane 的 IN_FORMATS，本层不解码也不排序。
    // 排序策略（tranche）是 Step 4 分配层的事，不属于门面。
    const std::vector<drm::Modifier> modifiers = plane_modifiers(*plane, impl.format);

    render::SwapchainDesc desc{};
    desc.size = impl.size;
    desc.format = impl.format;
    desc.count = config.buffer_count;
    desc.modifiers = span<const drm::Modifier>(modifiers.data(), modifiers.size());
    desc.need_cpu_write = true;

    impl.chain = TRY(render::Swapchain::create(*impl.source, desc));
    LOG_INFO("{}", impl.chain.to_string());

    impl.mode_blob =
        TRY(PropertyBlob::create(impl.device->fd(), &impl.path.mode.raw, sizeof(drmModeModeInfo)));
    impl.request.emplace(impl.device->fd());

    // ---- 首次 modeset ----
    //
    // 无条件走完整 modeset 并带 ALLOW_MODESET，**不假设 CRTC 是干净的**。
    // 上一个进程可能崩在半路，也可能刚从 VT 切回来。
    render::Swapchain::Slot& first = impl.chain.acquire();
    const Connector* connector = impl.device->connector(impl.path.connector);
    const Crtc* crtc = impl.device->crtc(impl.path.crtc);
    if (connector == nullptr || crtc == nullptr) {
        return Err(Errc::StaleSnapshot, "output path refers to objects not in the snapshot");
    }

    impl.request->reset();
    TRY(impl.request->bind_connector(*connector, impl.path.crtc));
    TRY(impl.request->set_crtc_mode(*crtc, impl.mode_blob.id(), true));
    TRY(impl.request->set_plane(*plane, first.buffer.fb_id(), impl.path.crtc,
                                SrcRect::whole(impl.size), CrtcRect::at_origin(impl.size)));

    const int test_result = impl.request->test(CommitFlags::AllowModeset);
    if (test_result != 0) {
        LOG_ERROR("modeset TEST_ONLY rejected with {}", drm::errno_name(test_result));
        impl.request->dump("rejected modeset");
        impl.request->bisect_rejection(CommitFlags::AllowModeset);
        return Err(Errc::AtomicTestFailed,
                   fmt("modeset TEST_ONLY failed with {}", drm::errno_name(test_result)));
    }
    TRY(impl.request->commit(CommitFlags::AllowModeset));
    impl.modeset_done = true;

    // modeset 那一次提交**不带** PAGE_FLIP_EVENT，所以永远不会有事件回来。
    // 传 false 让 in_flight 不因此永久多 1 —— 一个恒定偏差比没有计数更糟，
    // 因为它看起来像真的。
    impl.chain.mark_submitted(false);
    LOG_INFO("modeset committed; the screen is live");
    return Ok();
}

} // namespace

Result<Screen> Screen::open(const ScreenConfig& config) {
    if (config.buffer_count < 2 || config.buffer_count > render::Swapchain::kMaxBuffers) {
        return Err(Errc::Internal,
                   fmt("buffer_count must be in [2, {}], got {}",
                       render::Swapchain::kMaxBuffers, config.buffer_count));
    }

    auto impl = std::make_unique<ScreenImpl>();
    impl->config = config;

    if (config.backend == Backend::Offscreen) {
        TRY(open_offscreen(*impl));
    } else {
        TRY(open_kms(*impl));
    }
    return Ok(Screen(std::move(impl)));
}

// ---------------------------------------------------------------------------
// 查询
// ---------------------------------------------------------------------------

bool Screen::valid() const noexcept {
    return impl_ != nullptr;
}

Backend Screen::backend() const noexcept {
    return impl_ ? impl_->config.backend : Backend::Offscreen;
}

Size Screen::size() const noexcept {
    return impl_ ? impl_->size : Size{};
}

Format Screen::format() const noexcept {
    return impl_ ? impl_->format : Format{};
}

uint32_t Screen::refresh_mhz() const noexcept {
    return impl_ ? impl_->refresh_mhz : 0u;
}

uint64_t Screen::frame_duration_ns() const noexcept {
    return impl_ ? impl_->frame_ns : 0u;
}

uint32_t Screen::in_flight() const noexcept {
    return impl_ ? impl_->in_flight : 0u;
}

uint32_t Screen::buffer_count() const noexcept {
    return impl_ ? impl_->config.buffer_count : 0u;
}

const FrameStats& Screen::stats() const noexcept {
    static const FrameStats kEmpty{};
    return impl_ ? impl_->stats : kEmpty;
}

drm::Device* Screen::device() noexcept {
    return (impl_ && impl_->device.has_value()) ? &*impl_->device : nullptr;
}

const drm::OutputPath* Screen::output() const noexcept {
    return (impl_ && impl_->modeset_done) ? &impl_->path : nullptr;
}

render::Swapchain* Screen::swapchain() noexcept {
    return (impl_ && impl_->config.backend == Backend::Kms) ? &impl_->chain : nullptr;
}

// ---------------------------------------------------------------------------
// 帧循环
// ---------------------------------------------------------------------------

Result<Frame> Screen::begin_frame() {
    if (! impl_) {
        return Err(Errc::Internal, "begin_frame() on a moved-from Screen");
    }
    if (impl_->frame_open) {
        return Err(Errc::Internal, "begin_frame() called twice without submit()");
    }

    Frame frame{};
    frame.index = impl_->frame_index;
    frame.size = impl_->size;
    frame.format = impl_->format;

    if (impl_->config.backend == Backend::Offscreen) {
        impl_->offscreen_current = impl_->offscreen_next;
        std::vector<uint8_t>& buf = impl_->offscreen[impl_->offscreen_current];
        frame.pixels = span<uint8_t>(buf.data(), buf.size());
        frame.stride = impl_->offscreen_stride;
    } else {
        render::Swapchain::Slot& slot = impl_->chain.acquire();
        frame.pixels = TRY(slot.buffer.map_write());
        frame.stride = slot.buffer.stride();
    }

    impl_->frame_open = true;
    return Ok(frame);
}

Status Screen::submit() {
    if (! impl_) {
        return Err(Errc::Internal, "submit() on a moved-from Screen");
    }
    if (! impl_->frame_open) {
        return Err(Errc::Internal, "submit() without a matching begin_frame()");
    }
    impl_->frame_open = false;

    if (impl_->config.backend == Backend::Offscreen) {
        const ScreenConfig& config = impl_->config;
        if (! config.dump_dir.empty() && config.dump_every != 0u &&
            impl_->frame_index % config.dump_every == 0u) {
            char name[64];
            std::snprintf(name, sizeof(name), "/frame-%06llu.ppm",
                          static_cast<unsigned long long>(impl_->frame_index));
            Frame frame{};
            frame.index = impl_->frame_index;
            frame.size = impl_->size;
            frame.format = impl_->format;
            frame.stride = impl_->offscreen_stride;
            std::vector<uint8_t>& buf = impl_->offscreen[impl_->offscreen_current];
            frame.pixels = span<uint8_t>(buf.data(), buf.size());
            TRY(write_ppm(config.dump_dir + name, frame));
        }
        impl_->offscreen_next = (impl_->offscreen_next + 1u) % impl_->config.buffer_count;
        ++impl_->in_flight;
        return Ok();
    }

    const Plane* plane = impl_->device->plane(impl_->path.primary_plane);
    if (plane == nullptr) {
        return Err(Errc::StaleSnapshot, "primary plane vanished");
    }
    render::Swapchain::Slot& slot = impl_->chain.acquire();

    impl_->request->reset();
    TRY(impl_->request->set_plane(*plane, slot.buffer.fb_id(), impl_->path.crtc,
                                  SrcRect::whole(impl_->size), CrtcRect::at_origin(impl_->size)));

    auto status = impl_->request->commit(CommitFlags::Nonblock | CommitFlags::PageFlipEvent,
                                         impl_->frame_index);
    if (! status) {
        impl_->request->bisect_rejection(CommitFlags::None);
        return status;
    }
    impl_->chain.mark_submitted();
    impl_->in_flight = impl_->chain.in_flight();
    return Ok();
}

Result<bool> Screen::wait_vblank(int timeout_ms) {
    if (! impl_) {
        return Err(Errc::Internal, "wait_vblank() on a moved-from Screen");
    }

    if (impl_->config.backend == Backend::Offscreen) {
        if (impl_->config.offscreen_pace && impl_->frame_ns != 0u) {
            impl_->next_deadline += impl_->frame_ns;
            const uint64_t current = now_ns();
            if (impl_->next_deadline < current) {
                // 落后了：不去补睡，直接重置基准。否则一次卡顿之后
                // 会连着好几帧完全不睡，把节拍搅得更乱。
                impl_->next_deadline = current;
            } else {
                sleep_until_ns(impl_->next_deadline);
            }
        }
        FlipEvent event{};
        event.user_data = impl_->frame_index;
        event.timestamp_ns = now_ns();
        event.sequence = static_cast<uint32_t>(impl_->frame_index);
        impl_->stats.record(event);
        if (impl_->in_flight > 0u) {
            --impl_->in_flight;
        }
        ++impl_->frame_index;
        return Ok(true);
    }

    if (timeout_ms < 0) {
        // 标称帧长的 5 倍再加 100ms：宽松到不会因为一次调度抖动误报，
        // 又不至于在硬件真的吞了提交时卡死不动。
        timeout_ms = static_cast<int>((impl_->frame_ns * 5u) / 1000000u) + 100;
    }

    const bool readable = TRY(drm::wait_readable(impl_->device->fd(), timeout_ms));
    if (! readable) {
        return Ok(false);
    }

    const size_t handled =
        TRY(drm::read_events(impl_->device->fd(), impl_->device->caps().timestamp_monotonic,
                             [this](const FlipEvent& event) {
                                 impl_->chain.on_flip_complete();
                                 impl_->stats.record(event);
                             }));
    impl_->in_flight = impl_->chain.in_flight();
    if (handled == 0) {
        return Ok(false);
    }
    ++impl_->frame_index;
    return Ok(true);
}

Status Screen::present() {
    TRY(submit());
    const bool flipped = TRY(wait_vblank());
    if (! flipped) {
        LOG_WARN("no page flip event within the timeout; {} submission(s) in flight", in_flight());
    }
    return Ok();
}

// ---------------------------------------------------------------------------

std::string Screen::to_string() const {
    if (! impl_) {
        return "Screen(moved-from)";
    }
    std::string out = fmt("Screen backend={} {}x{} {} @{}.{:03} Hz buffers={}",
                          display::to_string(impl_->config.backend), impl_->size.width, impl_->size.height,
                          drm::to_string(impl_->format), impl_->refresh_mhz / 1000u,
                          impl_->refresh_mhz % 1000u, impl_->config.buffer_count);
    if (impl_->config.backend == Backend::Kms && impl_->device.has_value()) {
        out += "\n  device: " + impl_->device->path();
        out += "\n  " + impl_->path.to_string();
        if (impl_->source) {
            out += "\n  source: " + impl_->source->describe();
        }
        out += "\n  " + impl_->chain.to_string();
    } else {
        out += fmt("\n  heap buffers, stride={} B, pacing={}", impl_->offscreen_stride,
                   impl_->config.offscreen_pace ? "on" : "off");
        if (! impl_->config.dump_dir.empty() && impl_->config.dump_every != 0u) {
            out += fmt("\n  dumping every {} frame(s) to {}", impl_->config.dump_every,
                       impl_->config.dump_dir);
        }
        out += "\n  NOTE: this backend touches no DRM object; it validates nothing about "
               "the display path";
    }
    return out;
}

} // namespace mw::display
