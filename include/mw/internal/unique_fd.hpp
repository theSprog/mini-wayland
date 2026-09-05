/**
 * @file unique_fd.hpp
 * @brief 文件描述符所有权的唯一表达 (Header-only)
 *
 * 设计取舍：
 *  - **不提供 `operator int()`**。隐式转换会让 `close(fd)`、
 *    `fds.push_back(fd)` 这类"悄悄丢所有权"的写法编译通过。
 *    要传给 libdrm 就显式写 `.get()`，读代码时一眼能看出这是借用。
 *  - `release()` 标了 `[[nodiscard]]`：忘记接返回值 = 直接泄漏。
 *  - 析构里 close 失败**不报错也不重试**。
 *  - Linux 上 close() 返回 EINTR 时 fd 已经被回收，重试会关掉别人刚拿到的同号 fd。
 */
#pragma once

#include "./error.hpp"

namespace internal {

/**
 * @brief 借用的 fd —— 明确表示"我只是用一下，不负责关"
 *
 * 用在函数签名上，让 `int` 参数到底是所有权转移还是借用不再靠猜。
 * 零开销，可平凡拷贝。
 */
class BorrowedFd {
  public:
    constexpr BorrowedFd() noexcept = default;
    constexpr explicit BorrowedFd(int fd) noexcept : fd_(fd) {}

    constexpr int get() const noexcept {
        return fd_;
    }

    constexpr bool valid() const noexcept {
        return fd_ >= 0;
    }

    constexpr explicit operator bool() const noexcept {
        return valid();
    }

  private:
    int fd_ = -1;
};

class UniqueFd {
  public:
    constexpr UniqueFd() noexcept = default;

    /// 接管 fd 的所有权。fd < 0 视为"空"，不是错误。
    constexpr explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    /// 借用。生命周期由本对象保证，调用方不得 close。
    constexpr int get() const noexcept {
        return fd_;
    }

    constexpr BorrowedFd borrow() const noexcept {
        return BorrowedFd(fd_);
    }

    constexpr bool valid() const noexcept {
        return fd_ >= 0;
    }

    constexpr explicit operator bool() const noexcept {
        return valid();
    }

    /// 交出所有权，调用方负责 close
    [[nodiscard]] int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    /// 关闭当前 fd 并接管新的
    void reset(int fd = -1) noexcept {
        if (fd_ >= 0 && fd_ != fd) {
            // Linux 上 close() 返回 EINTR 时 fd 已经被回收，重试会关掉
            // 别人刚拿到的同号 fd。所以既不重试也不报错。
            const int rc = ::close(fd_);
            (void)rc;
        }
        fd_ = fd;
    }

    void swap(UniqueFd& other) noexcept {
        std::swap(fd_, other.fd_);
    }

    // ---- 工厂 ----

    /// open(2)。失败返回 errno 域的 Error（EACCES / ENOENT 上层要分辨）
    static Result<UniqueFd> open(const char* path, int flags) {
        // O_CLOEXEC 默认加上：这个项目后面会 fork/exec 测试客户端，
        // 泄漏一个 DRM fd 给子进程会让 master 语义变得非常难查。
        const int fd = ::open(path, flags | O_CLOEXEC);
        if (fd < 0) {
            return sys_err_ctx("open", std::string(path));
        }
        return Ok(UniqueFd(fd));
    }

    /// dup(2)，带 CLOEXEC（内部用 F_DUPFD_CLOEXEC，不是 dup 后再 fcntl）
    Result<UniqueFd> duplicate() const {
        if (fd_ < 0) {
            return sys_err("dup", EBADF);
        }
        // 用 F_DUPFD_CLOEXEC 而不是 dup()+fcntl()：后者在两步之间有窗口，
        // 多线程下 fork 会把 fd 漏出去。
        const int fd = ::fcntl(fd_, F_DUPFD_CLOEXEC, 0);
        if (fd < 0) {
            return sys_err("fcntl(F_DUPFD_CLOEXEC)");
        }
        return Ok(UniqueFd(fd));
    }

  private:
    int fd_ = -1;
};

inline void swap(UniqueFd& a, UniqueFd& b) noexcept {
    a.swap(b);
}

} // namespace internal
