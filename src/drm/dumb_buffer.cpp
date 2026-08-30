#include "mw/drm/dumb_buffer.hpp"

#include <sys/mman.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/trace.hpp"

namespace mw::drm {

DumbBuffer::~DumbBuffer() {
    // 顺序有讲究：先撤 fb（它引用着 GEM 对象），再 munmap，最后销毁 dumb。
    // 反过来做内核会拒绝销毁一个还被 fb 引用的 GEM 对象。
    fb_.reset();

    if (pixels_ != nullptr) {
        if (::munmap(pixels_, static_cast<size_t>(byte_size_)) != 0) {
            LOG_WARN("munmap of dumb buffer failed: {}", errno_name(errno));
        }
        pixels_ = nullptr;
    }

    if (handle_ != GemHandle{0}) {
        drm_mode_destroy_dumb destroy{};
        destroy.handle = static_cast<uint32_t>(handle_);
        const int ret = MW_DRM_CALL(destroy_dumb,
                                    drmIoctl(fd_.get(), DRM_IOCTL_MODE_DESTROY_DUMB, &destroy),
                                    "handle={}", destroy.handle);
        if (ret != 0) {
            LOG_WARN("DESTROY_DUMB({}) failed; GEM object leaked", destroy.handle);
        } else {
            ++stats().dumb_released;
        }
        handle_ = GemHandle{0};
    }
}

DumbBuffer::DumbBuffer(DumbBuffer&& other) noexcept
    : fd_(other.fd_),
      size_(other.size_),
      format_(other.format_),
      pitch_(other.pitch_),
      byte_size_(other.byte_size_),
      handle_(std::exchange(other.handle_, GemHandle{0})),
      pixels_(std::exchange(other.pixels_, nullptr)),
      fb_(std::move(other.fb_)) {}

DumbBuffer& DumbBuffer::operator=(DumbBuffer&& other) noexcept {
    if (this != &other) {
        // 先把自己现有的资源析构掉。用交换的写法可以复用析构逻辑，
        // 但那样 other 会带着我们的旧资源多活一会儿；显式点更好读。
        this->~DumbBuffer();
        fd_ = other.fd_;
        size_ = other.size_;
        format_ = other.format_;
        pitch_ = other.pitch_;
        byte_size_ = other.byte_size_;
        handle_ = std::exchange(other.handle_, GemHandle{0});
        pixels_ = std::exchange(other.pixels_, nullptr);
        fb_ = std::move(other.fb_);
    }
    return *this;
}

Result<DumbBuffer> DumbBuffer::create(BorrowedFd fd, Size size, Format format, uint32_t bpp) {
    if (size.empty()) {
        return Err(Errc::DumbCreateFailed, "requested dumb buffer size is zero");
    }
    if (bpp != 32u) {
        // dumb buffer 的 bpp 与 format 必须自洽。这里不维护转换表 ——
        // 需要别的格式时用 GBM（Step 2），那才是正经的分配器。
        return Err(Errc::Unsupported,
                   fmt("dumb buffers here only support 32 bpp, got {}", bpp));
    }

    DumbBuffer buffer;
    buffer.fd_ = fd;
    buffer.size_ = size;
    buffer.format_ = format;

    // ---- 1. CREATE_DUMB ----
    // 驱动会自己决定 pitch（按硬件对齐要求），返回的 size 也可能大于
    // pitch * height。这两个值必须用驱动给的，不能自己算。
    drm_mode_create_dumb create{};
    create.width = size.width;
    create.height = size.height;
    create.bpp = bpp;

    if (MW_DRM_CALL(create_dumb, drmIoctl(fd.get(), DRM_IOCTL_MODE_CREATE_DUMB, &create),
                    "{}x{} bpp={}", size.width, size.height, bpp) != 0) {
        return Err(Errc::DumbCreateFailed,
                   fmt("CREATE_DUMB {}x{} bpp={} failed with {}", size.width, size.height, bpp,
                       errno_name(errno)));
    }

    // 只有 ioctl 成功之后才计入配平表 —— 失败的 create 没有资源需要释放。
    ++stats().dumb_acquired;

    buffer.handle_ = GemHandle{create.handle};
    buffer.pitch_ = create.pitch;
    buffer.byte_size_ = create.size;

    if (create.pitch < size.width * 4u) {
        return Err(Errc::DumbCreateFailed,
                   fmt("driver returned pitch={} which is smaller than width*4={}", create.pitch,
                       size.width * 4u));
    }
    if (create.pitch != size.width * 4u) {
        // 值得打出来：用 width*4 做行偏移是最常见的花屏原因。
        LOG_INFO("driver aligned the pitch to {} bytes (width*4 would be {}); "
                 "always use pitch() for row addressing",
                 create.pitch, size.width * 4u);
    }

    // ---- 2. MAP_DUMB ----
    // 换来的不是地址，是一个"在 DRM fd 上 mmap 时该用的偏移量"。
    drm_mode_map_dumb map{};
    map.handle = create.handle;
    if (MW_DRM_CALL(map_dumb, drmIoctl(fd.get(), DRM_IOCTL_MODE_MAP_DUMB, &map), "handle={}",
                    create.handle) != 0) {
        return Err(Errc::DumbMapFailed,
                   fmt("MAP_DUMB(handle={}) failed with {}", create.handle, errno_name(errno)));
    }

    // ---- 3. mmap ----
    void* addr = ::mmap(nullptr, static_cast<size_t>(create.size), PROT_READ | PROT_WRITE,
                        MAP_SHARED, fd.get(), static_cast<off_t>(map.offset));
    if (addr == MAP_FAILED) {
        return sys_err("mmap of dumb buffer");
    }
    buffer.pixels_ = static_cast<uint8_t*>(addr);

    // 到此为止。addfb2 是独立的一步 —— 见头文件里为什么。
    LOG_INFO("dumb buffer ready: {}", buffer.to_string());
    return Ok(std::move(buffer));
}

Status DumbBuffer::register_framebuffer() {
    if (! valid()) {
        return Err(Errc::Internal, "register_framebuffer() on an empty dumb buffer");
    }
    if (fb_.valid()) {
        return Ok();
    }
    const FramebufferDesc desc =
        FramebufferDesc::single_plane(size_, format_, handle_, pitch_, 0, kModifierInvalid);
    fb_ = TRY(Framebuffer::add(fd_, desc));
    LOG_DEBUG("dumb buffer registered as {}", drm::to_string(fb_.id()));
    return Ok();
}

span<uint8_t> DumbBuffer::bytes() noexcept {
    return span<uint8_t>(pixels_, static_cast<size_t>(byte_size_));
}

void DumbBuffer::fill(uint32_t argb) noexcept {
    if (pixels_ == nullptr) {
        return;
    }
    // 四个字节相同时 memset 快得多（dumb buffer 通常是 write-combining 内存，
    // 顺序宽写是它唯一擅长的事）。
    const auto byte = static_cast<uint8_t>(argb & 0xffu);
    const bool uniform = ((argb >> 8) & 0xffu) == byte && ((argb >> 16) & 0xffu) == byte &&
                         ((argb >> 24) & 0xffu) == byte;
    if (uniform) {
        std::memset(pixels_, byte, static_cast<size_t>(byte_size_));
        return;
    }

    for (uint32_t y = 0; y < size_.height; ++y) {
        uint32_t* line = row(y);
        for (uint32_t x = 0; x < size_.width; ++x) {
            line[x] = argb;
        }
    }
}

std::string DumbBuffer::to_string() const {
    return fmt("{} {} pitch={} size={} handle={} fb={}", drm::to_string(size_),
               drm::to_string(format_), pitch_, byte_size_, drm::to_string(handle_),
               fb_.valid() ? drm::to_string(fb_.id()) : std::string("none"));
}

// ---------------------------------------------------------------------------
// DumbBufferChain
// ---------------------------------------------------------------------------

Result<DumbBufferChain> DumbBufferChain::create(BorrowedFd fd, Size size, Format format,
                                                uint32_t count) {
    if (count < 2u || count > kMaxBuffers) {
        return Err(Errc::Unsupported,
                   fmt("buffer count {} out of range [2, {}]; single buffering always tears",
                       count, kMaxBuffers));
    }

    DumbBufferChain chain;
    chain.count_ = count;
    for (uint32_t i = 0; i < count; ++i) {
        chain.buffers_[i] = TRY(DumbBuffer::create(fd, size, format));
        // 缓冲链是给合成器用的，每一块都要能上屏，所以这里立刻注册 fb。
        TRY(chain.buffers_[i].register_framebuffer());
    }
    LOG_INFO("allocated {} dumb buffers of {} ({} MiB total)", count, drm::to_string(size),
             (chain.buffers_[0].byte_size() * count) / (1024u * 1024u));
    return Ok(std::move(chain));
}

DumbBuffer& DumbBufferChain::acquire() noexcept {
    return buffers_[next_];
}

void DumbBufferChain::mark_submitted() noexcept {
    next_ = (next_ + 1u) % count_;
    ++in_flight_;
}

void DumbBufferChain::on_flip_complete() noexcept {
    if (in_flight_ > 0u) {
        --in_flight_;
    } else {
        // 收到了没有对应提交的完成事件。通常意味着事件循环里少记了一次提交，
        // 或者内核补发了旧事件。不致命，但值得知道。
        LOG_WARN("flip completion with no outstanding submission");
    }
}

} // namespace mw::drm
