#include "mw/core/unique_fd.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include "mw/core/log.hpp"

namespace mw {

UniqueFd::~UniqueFd() {
    reset();
}

UniqueFd::UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset(std::exchange(other.fd_, -1));
    }
    return *this;
}

int UniqueFd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    LOG_TRACE("UniqueFd: released fd={} (caller now owns it)", fd);
    return fd;
}

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0 && fd_ != fd) {
        // Linux 上 close() 返回 EINTR 时 fd 已经被回收，重试会关掉
        // 别人刚拿到的同号 fd。所以既不重试也不报错。
        const int rc = ::close(fd_);
        if (rc != 0) {
            LOG_DEBUG("UniqueFd: close(fd={}) returned {} errno={}", fd_, rc, errno);
        } else {
            LOG_TRACE("UniqueFd: closed fd={}", fd_);
        }
    }
    fd_ = fd;
}

void UniqueFd::swap(UniqueFd& other) noexcept {
    std::swap(fd_, other.fd_);
}

Result<UniqueFd> UniqueFd::open(const char* path, int flags) {
    // O_CLOEXEC 默认加上：这个项目后面会 fork/exec 测试客户端，
    // 泄漏一个 DRM fd 给子进程会让 master 语义变得非常难查。
    const int fd = ::open(path, flags | O_CLOEXEC);
    if (fd < 0) {
        return sys_err_ctx("open", std::string(path));
    }
    LOG_DEBUG("UniqueFd: opened '{}' -> fd={} flags={:x}", path, fd, flags | O_CLOEXEC);
    return Ok(UniqueFd(fd));
}

Result<UniqueFd> UniqueFd::duplicate() const {
    if (fd_ < 0) {
        return sys_err("dup", EBADF);
    }
    // 用 F_DUPFD_CLOEXEC 而不是 dup()+fcntl()：后者在两步之间有窗口，
    // 多线程下 fork 会把 fd 漏出去。
    const int fd = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
    if (fd < 0) {
        return sys_err("fcntl(F_DUPFD_CLOEXEC)");
    }
    LOG_TRACE("UniqueFd: duplicated fd={} -> fd={}", fd_, fd);
    return Ok(UniqueFd(fd));
}

} // namespace mw
