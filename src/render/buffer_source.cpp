#include "mw/render/buffer_source.hpp"

#include <drm_fourcc.h>
#include <sys/mman.h>
#include <xf86drm.h>

#include <algorithm>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/dumb_buffer.hpp"
#include "mw/drm/error.hpp"
#include "mw/gbm/device.hpp"

namespace mw::render {

// ---------------------------------------------------------------------------
// ScanoutBuffer
// ---------------------------------------------------------------------------

namespace {

/// ScanoutBuffer 的实现细节。两条路径产出同一种东西，差别只在怎么造出来。
struct BufferState {
    drm::DmabufDesc dmabuf{};
    drm::Framebuffer fb{};
    Modifier modifier = drm::kModifierInvalid;

    // CPU 映射。dumb 路径走 mmap，GBM 路径走 gbm_bo_map，
    // 二者的释放方式不同，所以各留一份状态。
    uint8_t* mapped = nullptr;
    size_t mapped_size = 0;
    gbm::Buffer gbm_buffer{};  ///< GBM 路径下持有 bo，dumb 路径为空
    drm::DumbBuffer dumb{};    ///< dumb 路径下持有 GEM 对象，GBM 路径为空
    void* map_cookie = nullptr;  ///< gbm_bo_map 的 out 参数，unmap 时要原样传回
    bool cpu_capable = false;
};

} // namespace

// 为了让头文件保持干净（不暴露 gbm/dumb 类型），实现放在一个 pimpl 里。
// ScanoutBuffer 的成员在这里定义。

struct ScanoutBufferImpl : BufferState {};

// ---------------------------------------------------------------------------

const char* to_string(SourceKind kind) noexcept {
    switch (kind) {
    case SourceKind::ScanoutDevice: return "scanout-device";
    case SourceKind::RenderDevice: return "render-device";
    }
    return "<unknown>";
}

// ---------------------------------------------------------------------------
// 显示侧分配
// ---------------------------------------------------------------------------

class ScanoutDeviceSource final : public BufferSource {
  public:
    ScanoutDeviceSource(BorrowedFd kms_fd, drm::HandleCache& cache)
        : kms_fd_(kms_fd), cache_(cache), alignment_(drm::probe_pitch_alignment(kms_fd)) {}

    SourceKind kind() const noexcept override {
        return SourceKind::ScanoutDevice;
    }

    Result<ScanoutBuffer> allocate(const AllocRequest& req) override {
        if (! req.modifiers.empty()) {
            // dumb buffer 的排布由驱动决定，没有协商余地。调用方给了候选
            // 说明它以为这里能挑，把这个误解说破，别让它以为拿到了指定布局。
            LOG_DEBUG("{} modifier candidate(s) ignored: dumb allocation has no modifier "
                      "negotiation",
                      req.modifiers.size());
        }

        auto dumb_result = drm::DumbBuffer::create(kms_fd_, req.size, req.format);
        if (! dumb_result) {
            return Err(drm::Errc::DumbCreateFailed,
                       fmt("dumb allocation on the scanout device failed: {}",
                           dumb_result.error().message));
        }
        drm::DumbBuffer dumb = std::move(dumb_result).value();

        // GEM handle 已经在 KMS 节点上，直接 addfb2 ——
        // 这条路径完全不经过 PRIME 导入。
        if (auto status = dumb.register_framebuffer(); ! status) {
            return Err(drm::Errc::AddFbFailed,
                       fmt("the scanout device would not register this dumb buffer as a "
                           "framebuffer: {}",
                           status.error().message));
        }
        auto exported = drm::export_dmabuf(kms_fd_, dumb.handle(), drm::PrimeAccess::ReadWrite);
        if (! exported) {
            return Err(drm::Errc::Unsupported,
                       fmt("the scanout device cannot export this buffer: {}",
                           exported.error().message));
        }

        auto state = std::make_unique<ScanoutBufferImpl>();
        state->dmabuf.size = req.size;
        state->dmabuf.format = req.format;
        state->dmabuf.modifier = drm::kModifierInvalid;
        state->dmabuf.num_planes = 1;
        state->dmabuf.fds[0] = std::move(exported).value();
        state->dmabuf.strides[0] = dumb.pitch();
        state->dmabuf.offsets[0] = 0;
        state->modifier = drm::kModifierInvalid;
        state->cpu_capable = true;
        state->fb = dumb.take_framebuffer();
        state->dumb = std::move(dumb);

        return Ok(ScanoutBuffer(std::move(state)));
    }

    std::vector<Modifier> available_modifiers(Format) const override {
        return {};  // dumb 没有 modifier 可谈
    }

    std::string describe() const override {
        return fmt("scanout-device allocator (dumb buffers, linear only, pitch alignment {})",
                   alignment_.has_value() ? fmt("{}B", *alignment_) : std::string("unknown"));
    }

  private:
    BorrowedFd kms_fd_;
    drm::HandleCache& cache_;
    std::optional<uint32_t> alignment_;
};

// ---------------------------------------------------------------------------
// 渲染侧分配
// ---------------------------------------------------------------------------

class RenderDeviceSource final : public BufferSource {
  public:
    RenderDeviceSource(BorrowedFd kms_fd, gbm::Device device, drm::HandleCache& cache)
        : kms_fd_(kms_fd),
          device_(std::move(device)),
          cache_(cache),
          alignment_(drm::probe_pitch_alignment(kms_fd)) {}

    SourceKind kind() const noexcept override {
        return SourceKind::RenderDevice;
    }

    Result<ScanoutBuffer> allocate(const AllocRequest& req) override {
        gbm::Usage usage = gbm::Usage::Scanout | gbm::Usage::Rendering;
        if (req.need_cpu_write) {
            usage = usage | gbm::Usage::CpuWrite;
        }

        auto buffer_result = device_.allocate(req.size, req.format, req.modifiers, usage);
        if (! buffer_result) {
            return Err(drm::Errc::Unsupported,
                       fmt("allocation on the render device failed: {}",
                           buffer_result.error().message));
        }
        gbm::Buffer buffer = std::move(buffer_result).value();

        // stride 是分配器定的，未必满足显示设备的要求。内核对此往往只回一个
        // 看不出原因的 EINVAL，所以在 addfb2 之前先自己查一遍。
        // 这是**诊断**，不是拒绝：探到的对齐值来自 dumb 分配器，
        // 和 addfb2 校验用的值是不是同一个属于驱动实现细节，不保证。
        const uint32_t stride = buffer.stride(0);
        if (! drm::pitch_is_aligned(stride, alignment_)) {
            LOG_WARN("the render device produced stride {} which is not a multiple of the "
                     "scanout device's probed alignment {}; addfb2 may reject it",
                     stride, *alignment_);
        }

        auto desc_result = buffer.export_dmabuf();
        if (! desc_result) {
            return Err(drm::Errc::Unsupported,
                       fmt("the render device cannot export this buffer: {}",
                           desc_result.error().message));
        }
        drm::DmabufDesc desc = std::move(desc_result).value();

        bool downgraded = false;
        auto fb_result = drm::import_as_framebuffer(kms_fd_, cache_, desc, &downgraded);
        if (! fb_result) {
            return Err(drm::Errc::AddFbFailed,
                       fmt("the scanout device would not accept the buffer: {}. "
                           "Check the kernel log; drivers often log the real reason there "
                           "while returning a bare EINVAL",
                           fb_result.error().message));
        }

        auto state = std::make_unique<ScanoutBufferImpl>();
        state->modifier = downgraded ? drm::kModifierInvalid : buffer.modifier();
        // 降级过就说明 fb 是按"无 modifier"建立的，记录实际状态而不是请求状态。
        state->dmabuf = std::move(desc);
        state->fb = std::move(fb_result).value();
        state->cpu_capable = req.need_cpu_write;
        state->gbm_buffer = std::move(buffer);

        return Ok(ScanoutBuffer(std::move(state)));
    }

    std::vector<Modifier> available_modifiers(Format format) const override {
        // GBM 没有"列出全部可用 modifier"的接口，只能逐个问。
        // 所以这里的输入必须由调用方给（通常是 plane 的 IN_FORMATS），
        // 我们只做过滤。签名上看不出这一点，是个遗憾。
        (void)format;
        return {};
    }

    /// 从候选里筛出本设备认的那些。调用方拿 IN_FORMATS 传进来。
    std::vector<Modifier> filter_supported(Format format, span<const Modifier> candidates) const {
        std::vector<Modifier> out;
        out.reserve(candidates.size());
        for (const Modifier modifier : candidates) {
            if (device_.format_modifier_supported(format, modifier)) {
                out.push_back(modifier);
            }
        }
        return out;
    }

    std::string describe() const override {
        return fmt("render-device allocator ({})", device_.to_string());
    }

  private:
    BorrowedFd kms_fd_;
    gbm::Device device_;
    drm::HandleCache& cache_;
    std::optional<uint32_t> alignment_;
};

// ---------------------------------------------------------------------------
// 工厂
// ---------------------------------------------------------------------------

Result<std::unique_ptr<BufferSource>> make_scanout_device_source(BorrowedFd kms_fd,
                                                                 drm::HandleCache& cache) {
    return Ok(std::unique_ptr<BufferSource>(new ScanoutDeviceSource(kms_fd, cache)));
}

Result<std::unique_ptr<BufferSource>> make_render_device_source(
    BorrowedFd kms_fd, const std::string& render_node_path, drm::HandleCache& cache) {
    auto device = gbm::Device::open(render_node_path);
    if (! device) {
        return Err(drm::Errc::Unsupported,
                   fmt("cannot use {} as a render device: {}", render_node_path,
                       device.error().message));
    }
    return Ok(std::unique_ptr<BufferSource>(
        new RenderDeviceSource(kms_fd, std::move(device).value(), cache)));
}

std::vector<SourceProbe> probe_buffer_sources(BorrowedFd kms_fd,
                                              const std::string& render_node_path, Size size) {
    std::vector<SourceProbe> results;
    const Format format{DRM_FORMAT_XRGB8888};

    // 探测就是真分配一次。跨设备导入能不能成、stride 对不对齐、
    // 两个设备的内存管理关系如何，都不是 drmGetCap 能回答的。
    auto try_source = [&](SourceKind kind, Result<std::unique_ptr<BufferSource>> made) {
        SourceProbe probe;
        probe.kind = kind;
        if (! made) {
            probe.detail = made.error().message;
            results.push_back(std::move(probe));
            return;
        }
        auto source = std::move(made).value();

        AllocRequest req;
        req.size = size;
        req.format = format;
        auto buffer = source->allocate(req);
        if (! buffer) {
            probe.detail = buffer.error().message;
        } else {
            probe.usable = true;
            probe.detail = buffer.value().to_string();
        }
        results.push_back(std::move(probe));
    };

    drm::HandleCache cache(kms_fd);
    try_source(SourceKind::ScanoutDevice, make_scanout_device_source(kms_fd, cache));
    if (! render_node_path.empty()) {
        try_source(SourceKind::RenderDevice,
                   make_render_device_source(kms_fd, render_node_path, cache));
    } else {
        SourceProbe probe;
        probe.kind = SourceKind::RenderDevice;
        probe.detail = "no render node given";
        results.push_back(std::move(probe));
    }
    return results;
}


// ---------------------------------------------------------------------------
// ScanoutBuffer 的成员
// ---------------------------------------------------------------------------

ScanoutBuffer::ScanoutBuffer() noexcept : impl_(nullptr) {}
ScanoutBuffer::ScanoutBuffer(ScanoutBuffer&&) noexcept = default;
ScanoutBuffer& ScanoutBuffer::operator=(ScanoutBuffer&&) noexcept = default;

ScanoutBuffer::ScanoutBuffer(std::unique_ptr<ScanoutBufferImpl> impl) noexcept
    : impl_(std::move(impl)) {}

ScanoutBuffer::~ScanoutBuffer() {
    if (impl_ && impl_->mapped != nullptr) {
        // dumb 路径的映射归 DumbBuffer 管，这里只处理 GBM 路径自己建立的。
        if (impl_->gbm_buffer.valid()) {
            gbm::unmap(impl_->gbm_buffer, impl_->map_cookie);
        }
        impl_->mapped = nullptr;
    }
}

bool ScanoutBuffer::valid() const noexcept {
    return impl_ != nullptr;
}

Size ScanoutBuffer::size() const noexcept {
    return impl_ ? impl_->dmabuf.size : Size{};
}

Format ScanoutBuffer::format() const noexcept {
    return impl_ ? impl_->dmabuf.format : Format{};
}

Modifier ScanoutBuffer::modifier() const noexcept {
    return impl_ ? impl_->modifier : drm::kModifierInvalid;
}

drm::FbId ScanoutBuffer::fb_id() const noexcept {
    return impl_ ? impl_->fb.id() : drm::kNoFb;
}

const drm::DmabufDesc& ScanoutBuffer::dmabuf() const noexcept {
    static const drm::DmabufDesc kEmpty;
    return impl_ ? impl_->dmabuf : kEmpty;
}

uint32_t ScanoutBuffer::stride() const noexcept {
    return impl_ ? impl_->dmabuf.strides[0] : 0u;
}

bool ScanoutBuffer::cpu_writable() const noexcept {
    return impl_ != nullptr && impl_->cpu_capable;
}

Result<span<uint8_t>> ScanoutBuffer::map_write() {
    if (! impl_) {
        return Err(drm::Errc::Internal, "map_write() on an empty buffer");
    }
    if (! impl_->cpu_capable) {
        return Err(drm::Errc::Unsupported,
                   "this buffer was not allocated for CPU access; pass need_cpu_write "
                   "in the allocation request");
    }

    // dumb 路径：DumbBuffer 已经映射好了，直接借它的。
    if (impl_->dumb.valid()) {
        return Ok(impl_->dumb.bytes());
    }

    // GBM 路径：按需映射一次，之后复用。
    if (impl_->mapped == nullptr) {
        auto mapped = gbm::map_write(impl_->gbm_buffer, impl_->map_cookie);
        if (! mapped) {
            return Err(drm::Errc::Unsupported,
                       fmt("gbm_bo_map failed: {}", mapped.error().message));
        }
        impl_->mapped = mapped.value().data();
        impl_->mapped_size = mapped.value().size();
    }
    return Ok(span<uint8_t>(impl_->mapped, impl_->mapped_size));
}

std::string ScanoutBuffer::to_string() const {
    if (! impl_) {
        return "<empty scanout buffer>";
    }
    return fmt("{} {} stride={} modifier={} {} cpu={}", drm::to_string(impl_->dmabuf.size),
               drm::to_string(impl_->dmabuf.format), stride(), drm::to_string(impl_->modifier),
               drm::to_string(impl_->fb.id()), impl_->cpu_capable ? "yes" : "no");
}

} // namespace mw::render
