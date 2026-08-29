/**
 * @file core/unique_fd.hpp
 * @brief 文件描述符所有权的唯一表达
 *
 * 后续所有 DMA-BUF fd、fence fd、syncobj fd、DRM 节点 fd 都走这里。
 * 显式同步阶段（Step 6）FD 的 double close / 泄漏是最容易翻车的地方，
 * 从第一行代码起就不允许出现裸 int 所有权。
 *
 * 设计取舍：
 *  - **不提供 `operator int()`**。隐式转换会让 `close(fd)`、
 *    `fds.push_back(fd)` 这类"悄悄丢所有权"的写法编译通过。
 *    要传给 libdrm 就显式写 `.get()`，读代码时一眼能看出这是借用。
 *  - `release()` 标了 `[[nodiscard]]`：忘记接返回值 = 直接泄漏。
 *  - 析构里 close 失败**不报错也不重试**。Linux 上 close() 返回 EINTR 时
 *    fd 已经被回收，重试会关掉别人刚拿到的同号 fd。需要感知 close 错误的
 *    场景（写文件落盘）本项目没有。
 */
#pragma once

#include "mw/core/error.hpp"

namespace mw {

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

    ~UniqueFd();

    UniqueFd(UniqueFd&& other) noexcept;
    UniqueFd& operator=(UniqueFd&& other) noexcept;

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
    [[nodiscard]] int release() noexcept;

    /// 关闭当前 fd 并接管新的
    void reset(int fd = -1) noexcept;

    void swap(UniqueFd& other) noexcept;

    // ---- 工厂 ----

    /// open(2)。失败返回 errno 域的 Error（EACCES / ENOENT 上层要分辨）
    static Result<UniqueFd> open(const char* path, int flags);

    /// dup(2)，带 CLOEXEC（内部用 F_DUPFD_CLOEXEC，不是 dup 后再 fcntl）
    Result<UniqueFd> duplicate() const;

  private:
    int fd_ = -1;
};

inline void swap(UniqueFd& a, UniqueFd& b) noexcept {
    a.swap(b);
}

} // namespace mw
