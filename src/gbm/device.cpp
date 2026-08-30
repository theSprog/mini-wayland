#include "mw/gbm/device.hpp"

#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>

#include <cerrno>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/trace.hpp"

namespace mw::gbm {

namespace {

uint32_t to_gbm_flags(Usage usage) {
    uint32_t flags = 0;
    if (has(usage, Usage::Scanout)) {
        flags |= GBM_BO_USE_SCANOUT;
    }
    if (has(usage, Usage::Rendering)) {
        flags |= GBM_BO_USE_RENDERING;
    }
    if (has(usage, Usage::CpuWrite)) {
        // GBM_BO_USE_LINEAR 是"CPU 能顺序访问"的实际含义。
        // GBM_BO_USE_WRITE 只保证 gbm_bo_write() 可用，那是个一次性写接口，
        // 拿不到可持续写的映射，不是我们要的。
        flags |= GBM_BO_USE_LINEAR;
    }
    return flags;
}

} // namespace

std::string to_string(Usage usage) {
    std::string out;
    auto append = [&out](const char* name) {
        if (! out.empty()) {
            out += "|";
        }
        out += name;
    };
    if (has(usage, Usage::Scanout)) {
        append("scanout");
    }
    if (has(usage, Usage::Rendering)) {
        append("rendering");
    }
    if (has(usage, Usage::CpuWrite)) {
        append("cpu-write");
    }
    return out.empty() ? std::string("none") : out;
}

// ---------------------------------------------------------------------------
// CPU 映射
// ---------------------------------------------------------------------------

Result<span<uint8_t>> map_write(const Buffer& buffer, void*& cookie) {
    if (! buffer.valid()) {
        return Err(drm::Errc::Internal, "map_write() on an empty gbm buffer");
    }
    uint32_t stride = 0;
    void* addr = gbm_bo_map(buffer.raw(), 0, 0, buffer.size().width, buffer.size().height,
                            GBM_BO_TRANSFER_WRITE, &stride, &cookie);
    if (addr == nullptr || addr == MAP_FAILED) {
        cookie = nullptr;
        return Err(drm::Errc::Unsupported,
                   fmt("gbm_bo_map failed with {}; the buffer may not be linear",
                       drm::errno_name(errno)));
    }
    if (stride != buffer.stride(0)) {
        // 映射时报的 stride 与 bo 的 stride 不一致意味着 GBM 在背后做了
        // 中转缓冲（staging）。写进去的内容会在 unmap 时才回写，
        // 而且行偏移要用这个 stride 而不是 bo 的。值得说出来。
        LOG_WARN("gbm_bo_map reported stride {} but the bo stride is {}; "
                 "the mapping is going through a staging buffer",
                 stride, buffer.stride(0));
    }
    const size_t length = static_cast<size_t>(stride) * buffer.size().height;
    return Ok(span<uint8_t>(static_cast<uint8_t*>(addr), length));
}

void unmap(const Buffer& buffer, void* cookie) noexcept {
    if (! buffer.valid() || cookie == nullptr) {
        return;
    }
    gbm_bo_unmap(buffer.raw(), cookie);
}

// ---------------------------------------------------------------------------
// Buffer
// ---------------------------------------------------------------------------

Buffer::~Buffer() {
    if (bo_ != nullptr) {
        gbm_bo_destroy(bo_);
        bo_ = nullptr;
    }
}

Buffer::Buffer(Buffer&& other) noexcept
    : bo_(std::exchange(other.bo_, nullptr)),
      size_(other.size_),
      format_(other.format_),
      modifier_(other.modifier_),
      plane_count_(other.plane_count_) {}

Buffer& Buffer::operator=(Buffer&& other) noexcept {
    if (this != &other) {
        if (bo_ != nullptr) {
            gbm_bo_destroy(bo_);
        }
        bo_ = std::exchange(other.bo_, nullptr);
        size_ = other.size_;
        format_ = other.format_;
        modifier_ = other.modifier_;
        plane_count_ = other.plane_count_;
    }
    return *this;
}

uint32_t Buffer::stride(uint32_t plane) const noexcept {
    if (bo_ == nullptr || plane >= plane_count_) {
        return 0;
    }
    return gbm_bo_get_stride_for_plane(bo_, static_cast<int>(plane));
}

uint32_t Buffer::offset(uint32_t plane) const noexcept {
    if (bo_ == nullptr || plane >= plane_count_) {
        return 0;
    }
    return gbm_bo_get_offset(bo_, static_cast<int>(plane));
}

Result<drm::DmabufDesc> Buffer::export_dmabuf() const {
    if (bo_ == nullptr) {
        return Err(drm::Errc::Internal, "export_dmabuf() on an empty gbm buffer");
    }
    if (plane_count_ > drm::kMaxDmabufPlanes) {
        return Err(drm::Errc::Unsupported,
                   fmt("gbm buffer has {} planes, more than the {} we can describe",
                       plane_count_, drm::kMaxDmabufPlanes));
    }

    drm::DmabufDesc desc;
    desc.size = size_;
    desc.format = format_;
    desc.modifier = modifier_;
    desc.num_planes = plane_count_;

    for (uint32_t i = 0; i < plane_count_; ++i) {
        // gbm_bo_get_fd_for_plane 在多平面分配落在同一块底层内存时，
        // 会为每个平面各返回一个独立的 fd（指向同一个 dma_buf）。
        // 所有权因此保持"一个平面一个 fd"，去重是导入侧的事。
        const int raw_fd = gbm_bo_get_fd_for_plane(bo_, static_cast<int>(i));
        if (raw_fd < 0) {
            return Err(drm::Errc::Unsupported,
                       fmt("gbm_bo_get_fd_for_plane({}) failed with {}", i, drm::errno_name(errno)));
        }
        desc.fds[i] = UniqueFd(raw_fd);
        desc.strides[i] = stride(i);
        desc.offsets[i] = offset(i);
    }

    TRY(desc.validate());
    return Ok(std::move(desc));
}

std::string Buffer::to_string() const {
    if (bo_ == nullptr) {
        return "<empty gbm buffer>";
    }
    std::string out = drm::to_string(size_);
    out += " ";
    out += drm::to_string(format_);
    out += fmt(" planes={} stride={}", plane_count_, stride(0));
    out += " modifier=";
    out += drm::to_string(modifier_);
    return out;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

Device::~Device() {
    if (device_ != nullptr) {
        gbm_device_destroy(device_);
        device_ = nullptr;
    }
}

Device::Device(Device&& other) noexcept
    : fd_(std::move(other.fd_)), device_(std::exchange(other.device_, nullptr)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (device_ != nullptr) {
            gbm_device_destroy(device_);
        }
        device_ = std::exchange(other.device_, nullptr);
        fd_ = std::move(other.fd_);
    }
    return *this;
}

Result<Device> Device::open(const std::string& node_path) {
    auto fd_result = UniqueFd::open(node_path.c_str(), O_RDWR | O_CLOEXEC);
    if (! fd_result) {
        return Err(drm::Errc::OpenFailed,
                   fmt("cannot open {}: {}", node_path, fd_result.error().message));
    }
    UniqueFd fd = std::move(fd_result).value();

    gbm_device* device = gbm_create_device(fd.get());
    if (device == nullptr) {
        return Err(drm::Errc::Unsupported,
                   fmt("gbm_create_device on {} failed; the node may not have a GBM backend "
                       "(display-only DRM nodes typically do not)",
                       node_path));
    }

    Device out(std::move(fd), device);
    LOG_INFO("opened GBM device on {} (backend '{}')", node_path, out.backend_name());
    return Ok(std::move(out));
}

std::string Device::backend_name() const {
    if (device_ == nullptr) {
        return "<none>";
    }
    const char* name = gbm_device_get_backend_name(device_);
    return name != nullptr ? std::string(name) : std::string("<unknown>");
}

bool Device::format_modifier_supported(Format format, Modifier modifier) const {
    if (device_ == nullptr) {
        return false;
    }
    if (modifier == drm::kModifierInvalid) {
        return true;  // 不带 modifier 的路径，交给分配本身去判断
    }
    const int planes = gbm_device_get_format_modifier_plane_count(
        device_, static_cast<uint32_t>(format), static_cast<uint64_t>(modifier));
    return planes > 0;
}

Result<Buffer> Device::allocate(Size size, Format format, span<const Modifier> modifiers,
                                Usage usage) const {
    if (device_ == nullptr) {
        return Err(drm::Errc::Internal, "allocate() on an empty gbm device");
    }
    if (size.empty()) {
        return Err(drm::Errc::Internal, "allocate() with zero size");
    }

    uint32_t flags = to_gbm_flags(usage);
    span<const Modifier> candidates = modifiers;

    // CPU 可写要求线性排布，与平铺/压缩 modifier 互斥。同时给了两者的话，
    // 静默地二选一会让调用方以为自己拿到了想要的东西，所以要说出来。
    if (has(usage, Usage::CpuWrite) && ! candidates.empty()) {
        LOG_WARN("cpu-write was requested together with {} modifier candidate(s); "
                 "ignoring the candidates and allocating linear",
                 candidates.size());
        candidates = span<const Modifier>();
    }

    gbm_bo* bo = nullptr;
    Modifier chosen = drm::kModifierInvalid;

    if (! candidates.empty()) {
        // 整个列表交给 GBM 自己挑。这里不排序、不做偏好判断 ——
        // 排序是协商策略，属于更上层。
        std::vector<uint64_t> raw;
        raw.reserve(candidates.size());
        for (const Modifier modifier : candidates) {
            if (modifier == drm::kModifierInvalid) {
                continue;  // INVALID 是"无信息"的哨兵，不是可分配的 modifier
            }
            raw.push_back(static_cast<uint64_t>(modifier));
        }

        if (! raw.empty()) {
            bo = gbm_bo_create_with_modifiers2(device_, size.width, size.height,
                                               static_cast<uint32_t>(format), raw.data(),
                                               static_cast<unsigned>(raw.size()), flags);
            if (bo != nullptr) {
                chosen = Modifier{gbm_bo_get_modifier(bo)};

                // GBM 应该只从我们给的列表里挑，但驱动实现有偏差的先例，
                // 而挑了列表外的 modifier 意味着后面 addfb2 大概率失败，
                // 且错误会出现在很远的地方。就地查出来。
                bool in_list = false;
                for (const Modifier modifier : candidates) {
                    in_list = in_list || (modifier == chosen);
                }
                if (! in_list) {
                    LOG_WARN("the allocator chose modifier {} which was not among the {} "
                             "candidates offered; scanout may be rejected later",
                             drm::to_string(chosen), candidates.size());
                }
            } else {
                LOG_DEBUG("gbm_bo_create_with_modifiers2 failed for {} candidate(s), "
                          "falling back to an unmodified allocation",
                          raw.size());
            }
        }
    }

    if (bo == nullptr) {
        bo = gbm_bo_create(device_, size.width, size.height, static_cast<uint32_t>(format), flags);
        chosen = drm::kModifierInvalid;
    }

    if (bo == nullptr) {
        return Err(drm::Errc::Unsupported,
                   fmt("gbm allocation of {} {} usage={} failed", drm::to_string(size),
                       drm::to_string(format), gbm::to_string(usage)));
    }

    Buffer buffer;
    buffer.bo_ = bo;
    buffer.size_ = size;
    buffer.format_ = format;
    buffer.modifier_ = chosen;
    buffer.plane_count_ = static_cast<uint32_t>(gbm_bo_get_plane_count(bo));

    LOG_DEBUG("allocated {}", buffer.to_string());
    return Ok(std::move(buffer));
}

std::string Device::to_string() const {
    if (device_ == nullptr) {
        return "<empty gbm device>";
    }
    return fmt("gbm device fd={} backend='{}'", fd_.get(), backend_name());
}

} // namespace mw::gbm
