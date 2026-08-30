#include "mw/drm/prime.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/trace.hpp"

namespace mw::drm {

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

Result<UniqueFd> export_dmabuf(BorrowedFd device, GemHandle handle, PrimeAccess access) {
    if (! device.valid()) {
        return Err(Errc::Internal, "export_dmabuf called with an invalid device fd");
    }
    if (handle == GemHandle{0}) {
        return Err(Errc::Internal, "export_dmabuf called with a null GEM handle");
    }

    // O_CLOEXEC 恒开：合成器会 fork/exec，泄漏一个 dmabuf fd 到子进程意味着
    // 那块显存永远不释放，而且现场完全看不出来。
    uint32_t flags = DRM_CLOEXEC;
    if (access == PrimeAccess::ReadWrite) {
        flags |= DRM_RDWR;
    }

    int raw_fd = -1;
    const int ret = MW_DRM_CALL(prime_handle_to_fd,
                                drmPrimeHandleToFD(device.get(), static_cast<uint32_t>(handle),
                                                   flags, &raw_fd),
                                "handle={} flags={:x}", static_cast<uint32_t>(handle), flags);
    if (ret != 0) {
        const int err = errno;
        // ReadWrite 需要驱动的 dma_buf_ops 支持写映射。不支持时退一步只读，
        // 因为纯 scanout 用不到写权限 —— 但要 WARN，别让调用方以为拿到了
        // 可写的 fd 然后在 mmap 时才失败。
        if (access == PrimeAccess::ReadWrite && (err == EINVAL || err == ENOTSUP)) {
            LOG_WARN("PRIME export with DRM_RDWR rejected ({}), retrying read-only",
                     errno_name(err));
            return export_dmabuf(device, handle, PrimeAccess::ReadOnly);
        }
        return Err(Errc::Unsupported,
                   fmt("drmPrimeHandleToFD(handle={}) failed with {}",
                       static_cast<uint32_t>(handle), errno_name(err)));
    }

    LOG_DEBUG("exported {} as dmabuf fd {}{}", to_string(handle), raw_fd,
              access == PrimeAccess::ReadWrite ? " (rw)" : " (ro)");
    return Ok(UniqueFd(raw_fd));
}

// ---------------------------------------------------------------------------
// ImportedHandle
// ---------------------------------------------------------------------------

ImportedHandle::~ImportedHandle() {
    reset();
}

ImportedHandle::ImportedHandle(ImportedHandle&& other) noexcept
    : cache_(std::exchange(other.cache_, nullptr)),
      handle_(std::exchange(other.handle_, GemHandle{0})) {}

ImportedHandle& ImportedHandle::operator=(ImportedHandle&& other) noexcept {
    if (this != &other) {
        reset();
        cache_ = std::exchange(other.cache_, nullptr);
        handle_ = std::exchange(other.handle_, GemHandle{0});
    }
    return *this;
}

void ImportedHandle::reset() noexcept {
    if (cache_ == nullptr) {
        return;
    }
    cache_->release(handle_);
    cache_ = nullptr;
    handle_ = GemHandle{0};
}

// ---------------------------------------------------------------------------
// HandleCache
// ---------------------------------------------------------------------------

HandleCache::~HandleCache() {
    if (refs_.empty()) {
        return;
    }

    // 到这一步说明有 ImportedHandle 活得比 cache 还久，或者根本没被析构。
    // 前者是 use-after-free 的前兆（它们的析构会写已释放的 refs_），
    // 后者是泄漏。两种都值得大声报出来。
    uint64_t total = 0;
    for (const auto& entry : refs_) {
        total += entry.second;
    }
    LOG_ERROR("HandleCache destroyed with {} handle(s) still referenced ({} refs total); "
              "an ImportedHandle outlived its cache or was never released",
              refs_.size(), total);

    for (const auto& entry : refs_) {
        struct drm_gem_close args = {};
        args.handle = entry.first;
        MW_DRM_CALL(gem_close, drmIoctl(device_.get(), DRM_IOCTL_GEM_CLOSE, &args),
                    "forced close handle={}", entry.first);
    }
    refs_.clear();
}

Result<ImportedHandle> HandleCache::import(BorrowedFd device, BorrowedFd dmabuf) {
    if (device.get() != device_.get()) {
        // GEM handle 的作用域就是单个 drm_file。同一个数值在两个 fd 上是
        // 两个不相干的对象，混用会静默地操作错误的 buffer。
        return Err(Errc::Internal,
                   fmt("HandleCache is bound to fd {} but import() was called with fd {}; "
                       "GEM handles are per-drm-file and must not be mixed",
                       device_.get(), device.get()));
    }
    if (! dmabuf.valid()) {
        return Err(Errc::Internal, "import() called with an invalid dmabuf fd");
    }

    uint32_t raw_handle = 0;
    const int ret = MW_DRM_CALL(prime_fd_to_handle,
                                drmPrimeFDToHandle(device.get(), dmabuf.get(), &raw_handle),
                                "dmabuf fd={}", dmabuf.get());
    if (ret != 0) {
        const int err = errno;
        // 导入能否成功取决于目标设备的 GEM 后端：与源设备共用内存管理器时
        // 通常直接通过；目标设备自己管理内存且无 IOMMU 时，可能要求导入的
        // 内存物理连续。内核往往只回一个 EINVAL，真正的原因在 dmesg 里。
        return Err(Errc::Unsupported,
                   fmt("drmPrimeFDToHandle(fd={}) failed with {}; "
                       "if the importing device has no IOMMU it may require physically "
                       "contiguous memory -- check dmesg for driver-side detail",
                       dmabuf.get(), errno_name(err)));
    }

    // 内核对同一个 dma_buf 的重复导入返回**同一个 handle 且不加引用**，
    // 所以计数必须由我们维护。这不是优化，是正确性：
    // 一个 NV12 buffer 的两个平面常在同一个 dma_buf 里，单帧内就会命中。
    const uint32_t count = ++refs_[raw_handle];
    if (count > 1) {
        LOG_TRACE("dmabuf fd {} maps to existing handle {} (refcount now {})", dmabuf.get(),
                  raw_handle, count);
    } else {
        LOG_DEBUG("imported dmabuf fd {} as {}", dmabuf.get(), drm::to_string(GemHandle{raw_handle}));
    }

    return Ok(ImportedHandle(this, GemHandle{raw_handle}));
}

void HandleCache::release(GemHandle handle) noexcept {
    const uint32_t raw_handle = static_cast<uint32_t>(handle);
    const auto it = refs_.find(raw_handle);
    if (it == refs_.end()) {
        LOG_ERROR("release of handle {} which is not in the cache; double release?", raw_handle);
        return;
    }

    if (--it->second > 0) {
        LOG_TRACE("handle {} refcount now {}", raw_handle, it->second);
        return;
    }

    refs_.erase(it);

    struct drm_gem_close args = {};
    args.handle = raw_handle;
    const int ret = MW_DRM_CALL(gem_close, drmIoctl(device_.get(), DRM_IOCTL_GEM_CLOSE, &args),
                                "handle={}", raw_handle);
    if (ret != 0) {
        LOG_WARN("DRM_IOCTL_GEM_CLOSE({}) failed with {}; kernel GEM object leaked", raw_handle,
                 errno_name(errno));
    } else {
        LOG_TRACE("closed imported handle {}", raw_handle);
    }
}

size_t HandleCache::live_count() const noexcept {
    return refs_.size();
}

uint32_t HandleCache::ref_count(GemHandle handle) const noexcept {
    const auto it = refs_.find(static_cast<uint32_t>(handle));
    return it == refs_.end() ? 0u : it->second;
}

std::string HandleCache::to_string() const {
    uint64_t total = 0;
    for (const auto& entry : refs_) {
        total += entry.second;
    }
    return fmt("{} handle(s) live, {} ref(s) total", refs_.size(), total);
}

// ---------------------------------------------------------------------------
// DmabufDesc
// ---------------------------------------------------------------------------

Status DmabufDesc::validate() const {
    if (size.empty()) {
        return Err(Errc::AddFbFailed, "dmabuf description has zero size");
    }
    if (num_planes == 0 || num_planes > kMaxDmabufPlanes) {
        return Err(Errc::AddFbFailed, fmt("num_planes={} out of range", num_planes));
    }
    for (uint32_t i = 0; i < num_planes; ++i) {
        if (! fds[i].valid()) {
            return Err(Errc::AddFbFailed, fmt("plane {} has no dmabuf fd", i));
        }
        if (strides[i] == 0) {
            return Err(Errc::AddFbFailed, fmt("plane {} has zero stride", i));
        }
    }
    return Ok();
}

std::string DmabufDesc::to_string() const {
    std::string out = drm::to_string(size);
    out += " ";
    out += drm::to_string(format);
    out += fmt(" planes={}", num_planes);
    for (uint32_t i = 0; i < num_planes && i < kMaxDmabufPlanes; ++i) {
        out += fmt(" [{}: fd={} stride={} offset={}]", i, fds[i].get(), strides[i], offsets[i]);
    }
    out += " modifier=";
    out += drm::to_string(modifier);
    return out;
}

// ---------------------------------------------------------------------------
// 导入 + 注册 fb
// ---------------------------------------------------------------------------

Result<Framebuffer> import_as_framebuffer(BorrowedFd kms_device, HandleCache& cache,
                                          const DmabufDesc& desc, bool* downgraded) {
    TRY(desc.validate());

    // 每个平面各导入一次。共享同一个 dma_buf 的平面会拿到同一个 handle，
    // 由 cache 计数，各自的 ImportedHandle 析构时只 close 一次。
    ImportedHandle handles[kMaxDmabufPlanes]{};
    FramebufferDesc fb_desc;
    fb_desc.size = desc.size;
    fb_desc.format = desc.format;
    fb_desc.num_planes = desc.num_planes;

    for (uint32_t i = 0; i < desc.num_planes; ++i) {
        auto imported = cache.import(kms_device, desc.fds[i].borrow());
        if (! imported) {
            return Err(Errc::Unsupported,
                       fmt("failed to import plane {} of {}: {}", i, desc.to_string(),
                           imported.error().message));
        }
        handles[i] = std::move(imported).value();
        fb_desc.handles[i] = handles[i].handle();
        fb_desc.pitches[i] = desc.strides[i];
        fb_desc.offsets[i] = desc.offsets[i];
    }
    for (size_t i = 0; i < kMaxFbPlanes; ++i) {
        fb_desc.modifiers[i] = desc.modifier;
    }

    auto fb = Framebuffer::add_with_fallback(kms_device, fb_desc, downgraded);

    // 无论成败，handles 都在这里析构。addfb2 成功时 fb 对象自己持有
    // GEM 对象的引用（驱动侧 drm_gem_object_lookup 取的引用转移给了
    // fb->obj[i]，由 drm_gem_fb_destroy 释放），所以关掉 handle 之后
    // fb_id 依然有效、依然能扫描。长期持有的是 fb_id 而不是 handle。
    return fb;
}

// ---------------------------------------------------------------------------
// 行跨距对齐探测
// ---------------------------------------------------------------------------

std::optional<uint32_t> probe_pitch_alignment(BorrowedFd device) {
    // 1x1 的 32bpp：理论最小 pitch = 4 字节。按对齐向上取整的驱动会返回
    // 对齐值本身。不做对齐的驱动返回 4，表示无约束。
    struct drm_mode_create_dumb create = {};
    create.width = 1;
    create.height = 1;
    create.bpp = 32;

    const int ret = MW_DRM_CALL(create_dumb,
                                drmIoctl(device.get(), DRM_IOCTL_MODE_CREATE_DUMB, &create),
                                "probe 1x1 bpp=32");
    if (ret != 0) {
        LOG_DEBUG("pitch alignment probe failed ({}); the device may not support dumb buffers",
                  errno_name(errno));
        return std::nullopt;
    }

    const uint32_t pitch = create.pitch;

    struct drm_mode_destroy_dumb destroy = {};
    destroy.handle = create.handle;
    if (MW_DRM_CALL(destroy_dumb, drmIoctl(device.get(), DRM_IOCTL_MODE_DESTROY_DUMB, &destroy),
                    "probe handle={}", create.handle) != 0) {
        LOG_WARN("failed to destroy the probe dumb buffer; one GEM object leaked");
    }

    if (pitch < 4) {
        LOG_WARN("pitch alignment probe returned {} for a 1x1 32bpp buffer, which is below the "
                 "theoretical minimum of 4; ignoring the result",
                 pitch);
        return std::nullopt;
    }

    if (pitch == 4) {
        LOG_DEBUG("pitch alignment probe: no alignment requirement detected");
    } else {
        LOG_INFO("pitch alignment probe: the allocator rounds to {} bytes", pitch);
    }
    return pitch;
}

bool pitch_is_aligned(uint32_t pitch, std::optional<uint32_t> alignment) noexcept {
    if (! alignment.has_value() || *alignment <= 1u) {
        return true;
    }
    return (pitch % *alignment) == 0u;
}

} // namespace mw::drm
