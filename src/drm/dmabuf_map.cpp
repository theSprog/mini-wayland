#include "mw/drm/dmabuf_map.hpp"

#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"

namespace mw::drm {
namespace {

uint64_t sync_flags(MapAccess access, bool start) noexcept {
    uint64_t flags = start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END;
    flags |= (access == MapAccess::ReadWrite) ? DMA_BUF_SYNC_RW : DMA_BUF_SYNC_READ;
    return flags;
}

int do_sync(BorrowedFd dmabuf, MapAccess access, bool start) noexcept {
    dma_buf_sync sync{};
    sync.flags = sync_flags(access, start);
    int ret = 0;
    do {
        ret = ::ioctl(dmabuf.get(), DMA_BUF_IOCTL_SYNC, &sync);
    } while (ret != 0 && errno == EINTR);
    return ret;
}

} // namespace

// ---------------------------------------------------------------------------
// DmabufAccess
// ---------------------------------------------------------------------------

DmabufAccess::DmabufAccess(BorrowedFd dmabuf, MapAccess access) noexcept
    : dmabuf_(dmabuf), access_(access), active_(true) {}

DmabufAccess::DmabufAccess(DmabufAccess&& other) noexcept
    : dmabuf_(other.dmabuf_), access_(other.access_), active_(other.active_) {
    other.active_ = false;
}

DmabufAccess& DmabufAccess::operator=(DmabufAccess&& other) noexcept {
    if (this != &other) {
        end();
        dmabuf_ = other.dmabuf_;
        access_ = other.access_;
        active_ = other.active_;
        other.active_ = false;
    }
    return *this;
}

DmabufAccess::~DmabufAccess() {
    end();
}

Result<DmabufAccess> DmabufAccess::begin(BorrowedFd dmabuf, MapAccess access) {
    if (! dmabuf.valid()) {
        return Err(Errc::Internal, "DmabufAccess::begin() with an invalid fd");
    }
    if (do_sync(dmabuf, access, true) != 0) {
        // 不发 SYNC_START 就读，在有缓存一致性问题的平台上会读到陈旧数据，
        // 而且是间歇性的。所以这里失败必须报上去，不能"试试看直接读"。
        return sys_err("ioctl(DMA_BUF_IOCTL_SYNC, START)");
    }
    return Ok(DmabufAccess(dmabuf, access));
}

void DmabufAccess::end() noexcept {
    if (! active_) {
        return;
    }
    active_ = false;
    if (do_sync(dmabuf_, access_, false) != 0) {
        // 到这里 CPU 访问已经结束，调用方也做不了什么 —— 但它必须被看见。
        LOG_WARN("ioctl(DMA_BUF_IOCTL_SYNC, END) failed: {}", std::strerror(errno));
    }
}

// ---------------------------------------------------------------------------
// DmabufMapping
// ---------------------------------------------------------------------------

DmabufMapping::DmabufMapping(void* addr, size_t length, BorrowedFd dmabuf,
                             MapAccess access) noexcept
    : addr_(addr), length_(length), dmabuf_(dmabuf), access_(access) {}

DmabufMapping::DmabufMapping(DmabufMapping&& other) noexcept
    : addr_(other.addr_), length_(other.length_), dmabuf_(other.dmabuf_), access_(other.access_) {
    other.addr_ = nullptr;
    other.length_ = 0;
}

DmabufMapping& DmabufMapping::operator=(DmabufMapping&& other) noexcept {
    if (this != &other) {
        if (addr_ != nullptr) {
            ::munmap(addr_, length_);
        }
        addr_ = other.addr_;
        length_ = other.length_;
        dmabuf_ = other.dmabuf_;
        access_ = other.access_;
        other.addr_ = nullptr;
        other.length_ = 0;
    }
    return *this;
}

DmabufMapping::~DmabufMapping() {
    if (addr_ != nullptr) {
        ::munmap(addr_, length_);
        addr_ = nullptr;
    }
}

Result<DmabufMapping> DmabufMapping::create(BorrowedFd dmabuf, MapAccess access, size_t length) {
    if (! dmabuf.valid()) {
        return Err(Errc::Internal, "DmabufMapping::create() with an invalid fd");
    }

    if (length == 0) {
        // 问内核要真实长度。比调用方按 stride * height 自己算可靠：
        // 分配器可能有额外对齐或尾部填充，算小了会在最后几行读到越界的东西，
        // 而那种越界读**不会报错**，只会给出看起来合理的垃圾。
        const off_t end = ::lseek(dmabuf.get(), 0, SEEK_END);
        if (end < 0) {
            return sys_err("lseek(dmabuf, SEEK_END)");
        }
        length = static_cast<size_t>(end);
    }
    if (length == 0) {
        return Err(Errc::Internal, "dmabuf reports a size of zero");
    }

    const int prot = (access == MapAccess::ReadWrite) ? (PROT_READ | PROT_WRITE) : PROT_READ;
    void* addr = ::mmap(nullptr, length, prot, MAP_SHARED, dmabuf.get(), 0);
    if (addr == MAP_FAILED) {
        const int err = errno;
        // 两类失败要分开说，处理动作完全不同：
        //   ENOSYS / EINVAL -> 导出方没实现 dma-buf 的 mmap。**能力问题**，
        //                      调用方应当降级成"这一层校验拿不到"并打印。
        //   EACCES          -> 导出时没带 DRM_RDWR 却要求了写权限。用法问题。
        // 三类失败要分开说，处理动作完全不同：
        //   ENOSYS / EINVAL -> 导出方没实现 dma-buf mmap
        //   EACCES          -> 导出方实现了，但**策略上拒绝** CPU 映射
        //                      （实测：本项目目标硬件的 GPU 侧就是这样）
        // 前两类都是能力判断，不是故障：调用方应当降级成"这一层校验拿不到"
        // 并打印，不要让验收静默地少一层。
        if (err == ENOSYS || err == EINVAL) {
            return Err(Errc::Unsupported,
                       fmt("mmap(dmabuf, {} byte(s)) failed with {} -- the exporter does not "
                           "implement dma-buf mmap",
                           length, std::strerror(err)));
        }
        if (err == EACCES || err == EPERM) {
            return Err(Errc::Unsupported,
                       fmt("mmap(dmabuf, {} byte(s)) failed with {} -- the exporter refuses CPU "
                           "mapping of this buffer (this is a policy, not a missing feature; "
                           "re-exporting with DRM_RDWR will not help a read-only mapping)",
                           length, std::strerror(err)));
        }
        return sys_err_ctx("mmap(dmabuf)", fmt("{} byte(s)", length), err);
    }

    LOG_DEBUG("mapped dmabuf fd {} ({} bytes, {})", dmabuf.get(), length,
              access == MapAccess::ReadWrite ? "rw" : "ro");
    return Ok(DmabufMapping(addr, length, dmabuf, access));
}

span<const uint8_t> DmabufMapping::bytes() const noexcept {
    if (addr_ == nullptr) {
        return {};
    }
    return span<const uint8_t>(static_cast<const uint8_t*>(addr_), length_);
}

span<uint8_t> DmabufMapping::mutable_bytes() noexcept {
    if (addr_ == nullptr || access_ != MapAccess::ReadWrite) {
        return {};
    }
    return span<uint8_t>(static_cast<uint8_t*>(addr_), length_);
}

Result<DmabufAccess> DmabufMapping::begin_access() const {
    return DmabufAccess::begin(dmabuf_, access_);
}

std::string DmabufMapping::to_string() const {
    if (! valid()) {
        return "dmabuf mapping <invalid>";
    }
    return fmt("dmabuf mapping {} byte(s) {}", length_,
               access_ == MapAccess::ReadWrite ? "rw" : "ro");
}

bool dmabuf_cpu_mappable(BorrowedFd dmabuf) {
    auto mapping = DmabufMapping::create(dmabuf, MapAccess::Read);
    if (! mapping) {
        LOG_DEBUG("dmabuf is not cpu-mappable: {}", mapping.error().message);
        return false;
    }
    return true;
}

} // namespace mw::drm
