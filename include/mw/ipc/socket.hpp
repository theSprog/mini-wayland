/**
 * @file ipc/socket.hpp
 * @brief UNIX domain socket：监听 / 连接 / 地址与生命周期
 *
 * ## 为什么是 SOCK_SEQPACKET 而不是 SOCK_STREAM
 *
 * `SCM_RIGHTS` 的辅助数据挂在**内核认为的那条消息**上。用 STREAM 时
 * 一次 `recvmsg` 可能读到半条消息，而 fd 已经全部到手 ——
 * "fd 属于哪条消息"要靠自己重建，这是这类代码最经典的错法。
 *
 * SEQPACKET 保边界、保可靠、保顺序、有连接语义，Linux 上原生支持。
 * wayland 用 STREAM 是历史与可移植性的结果，代价是自己处理粘包。
 * 既然明确不做 wayland wire，就没有理由继承这份麻烦。
 *
 * Step 4 换 `libwayland-server` 时会退回 STREAM，**这是预期内的**：
 * 那时粘包由 libwayland 处理，不是我们的代码。Step 3 用 SEQPACKET
 * 是为了让"传输"这一层不产生噪音，把出错时的指向性留给序列化和生命周期。
 *
 * ## socket 路径与 Step 4 同址
 *
 * `$XDG_RUNTIME_DIR/mini-wayland-<n>`（无该变量时退到 `/tmp`），
 * 与真实 wayland socket 同一个位置、同样的生命周期问题：
 * 残留的 socket 文件、属主与权限。提前踩掉。
 *
 * **权限这件事一定会撞上**：server 需要 DRM master 通常要 root，
 * client 若以普通用户跑就连不上 root 属主的 socket。
 * 这里不替调用方决定策略（chmod / 换用户 / 都用 root），
 * 只保证失败时的错误信息说得清是权限问题。
 */
#pragma once

#include <string>

#include "mw/internal/error.hpp"
#include "mw/internal/unique_fd.hpp"

using internal::Result;
using internal::Status;
using internal::UniqueFd;
using internal::BorrowedFd;

namespace mw::ipc {

/**
 * @brief 默认 socket 路径
 *
 * `$XDG_RUNTIME_DIR/mini-wayland-<n>`，无 `XDG_RUNTIME_DIR` 时用 `/tmp`。
 * 返回值可能超过 `sockaddr_un::sun_path` 的长度限制（108），
 * 由 `listen_seqpacket` / `connect_seqpacket` 负责报错，不在这里截断 ——
 * 截断出来的路径能连上别的东西。
 */
std::string default_socket_path(int display_number = 0);

/**
 * @brief 监听中的 socket，析构时 unlink 路径
 *
 * unlink 必须在这里做：进程崩溃留下的 socket 文件会让下一次 bind 拿到
 * EADDRINUSE，而那个报错指向"地址被占用"，读起来像是有另一个 server 在跑。
 *
 * bind 之前也会 unlink 一次（残留清理）。**这意味着后启动的 server 会
 * 抢掉先启动的那个的路径** —— 与 wayland 的行为一致，
 * 真正的互斥要靠文件锁，Step 4 再做（那时才有多 server 共存的场景）。
 */
class ListeningSocket {
  public:
    ListeningSocket() = default;
    ~ListeningSocket();

    ListeningSocket(ListeningSocket&& other) noexcept;
    ListeningSocket& operator=(ListeningSocket&& other) noexcept;
    ListeningSocket(const ListeningSocket&) = delete;
    ListeningSocket& operator=(const ListeningSocket&) = delete;

    /**
     * @brief 创建、bind、listen
     *
     * `SOCK_SEQPACKET | SOCK_CLOEXEC`。bind 前 unlink 残留。
     */
    static Result<ListeningSocket> create(const std::string& path, int backlog = 4);

    /**
     * @brief 接受一个连接
     *
     * 阻塞。需要非阻塞时先 `set_nonblocking()`，此时无连接返回
     * `EAGAIN` 域的 Error，调用方据此区分"没人连"和"出错了"。
     */
    Result<UniqueFd> accept() const;

    Status set_nonblocking(bool on) const;

    BorrowedFd fd() const noexcept {
        return listener_.borrow();
    }

    const std::string& path() const noexcept {
        return path_;
    }

    bool valid() const noexcept {
        return listener_.valid();
    }

  private:
    ListeningSocket(UniqueFd listener, std::string path) noexcept;

    UniqueFd listener_{};
    std::string path_{};
};

/**
 * @brief 连接到一个已在监听的 socket
 *
 * `SOCK_SEQPACKET | SOCK_CLOEXEC`。失败时的两个 errno 值得分辨：
 * `ENOENT` = server 没起来（或路径不对），`EACCES` = 起来了但权限不够。
 * 实现要把这两条写成不同的提示，它们的处理动作完全不同。
 */
Result<UniqueFd> connect_seqpacket(const std::string& path);

/**
 * @brief 一对已连接的 socket，用于 `--spawn` 模式
 *
 * `socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC)`。
 * server 自己 fork+exec client 时直接把一端继承下去，
 * 不经过文件系统 —— 顺带绕开了上面那些权限问题，
 * 所以 `--spawn` 是 CI 里应该走的路径。
 *
 * @note 传给子进程的那一端要显式清掉 `FD_CLOEXEC`，
 *       否则 exec 之后子进程手里什么都没有。这是唯一一处
 *       故意不带 CLOEXEC 的地方，实现里要写明。
 */
Result<std::pair<UniqueFd, UniqueFd>> make_socket_pair();

} // namespace mw::ipc
