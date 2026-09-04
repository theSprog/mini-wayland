#include "mw/ipc/socket.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/ipc/error.hpp"

namespace mw::ipc {
namespace {

/// 把路径填进 sockaddr_un。**超长直接失败，不截断** ——
/// 截断出来的路径是一个合法但不同的地址，可能连上别的东西。
Status fill_address(sockaddr_un& addr, const std::string& path) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return Err(Errc::PathTooLong, fmt("socket path is {} bytes, the limit is {}", path.size(),
                                          sizeof(addr.sun_path) - 1));
    }
    std::memcpy(addr.sun_path, path.c_str(), path.size());
    return {};
}

} // namespace

std::string default_socket_path(int display_number) {
    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");
    // 与真实 wayland socket 同一个位置：残留文件、属主与权限这些问题
    // 提前踩掉，Step 4 换成真 socket 时不会再撞一遍。
    const std::string dir = (runtime_dir != nullptr && runtime_dir[0] != '\0') ? runtime_dir
                                                                              : "/tmp";
    return fmt("{}/mini-wayland-{}", dir, display_number);
}

// ---------------------------------------------------------------------------
// ListeningSocket
// ---------------------------------------------------------------------------

ListeningSocket::ListeningSocket(UniqueFd listener, std::string path) noexcept
    : listener_(std::move(listener)), path_(std::move(path)) {}

ListeningSocket::ListeningSocket(ListeningSocket&& other) noexcept
    : listener_(std::move(other.listener_)), path_(std::move(other.path_)) {
    other.path_.clear();
}

ListeningSocket& ListeningSocket::operator=(ListeningSocket&& other) noexcept {
    if (this != &other) {
        if (! path_.empty()) {
            ::unlink(path_.c_str());
        }
        listener_ = std::move(other.listener_);
        path_ = std::move(other.path_);
        other.path_.clear();
    }
    return *this;
}

ListeningSocket::~ListeningSocket() {
    if (! path_.empty()) {
        // 崩溃留下的 socket 文件会让下一次 bind 报 EADDRINUSE，
        // 而那条报错读起来像"已经有一个 server 在跑"，指向完全错误的方向。
        ::unlink(path_.c_str());
    }
}

Result<ListeningSocket> ListeningSocket::create(const std::string& path, int backlog) {
    sockaddr_un addr{};
    TRY(fill_address(addr, path));

    UniqueFd sock(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (! sock.valid()) {
        return sys_err("socket(AF_UNIX, SOCK_SEQPACKET)");
    }

    // 残留清理。这意味着后启动的 server 会抢掉先启动那个的路径 ——
    // 与 wayland 的行为一致；真正的互斥要靠文件锁，等 Step 4 有多 server
    // 共存的场景时再做。
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        LOG_WARN("could not remove stale socket at {}: {}", path, std::strerror(errno));
    }

    if (::bind(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        return sys_err_ctx("bind", path);
    }
    if (::listen(sock.get(), backlog) != 0) {
        return sys_err_ctx("listen", path);
    }

    LOG_INFO("listening on {}", path);
    return Ok(ListeningSocket(std::move(sock), path));
}

Result<UniqueFd> ListeningSocket::accept() const {
    if (! listener_.valid()) {
        return Err(Errc::AcceptFailed, "accept() called on a closed listening socket");
    }
    const int fd = ::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        return sys_err("accept4");
    }
    LOG_INFO("client connected");
    return Ok(UniqueFd(fd));
}

Status ListeningSocket::set_nonblocking(bool on) const {
    const int flags = ::fcntl(listener_.get(), F_GETFL, 0);
    if (flags < 0) {
        return sys_err("fcntl(F_GETFL)");
    }
    const int updated = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(listener_.get(), F_SETFL, updated) != 0) {
        return sys_err("fcntl(F_SETFL)");
    }
    return {};
}

// ---------------------------------------------------------------------------
// connect / socketpair
// ---------------------------------------------------------------------------

Result<UniqueFd> connect_seqpacket(const std::string& path) {
    sockaddr_un addr{};
    TRY(fill_address(addr, path));

    UniqueFd sock(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (! sock.valid()) {
        return sys_err("socket(AF_UNIX, SOCK_SEQPACKET)");
    }

    if (::connect(sock.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        // 这两个 errno 的处理动作完全不同，所以分开说：
        //   ENOENT -> server 还没起来，或者路径不对
        //   EACCES -> server 起来了，但当前用户没有权限连（典型：server 是
        //             root 起的，client 是普通用户）
        const char* hint = (err == ENOENT)   ? " (is the server running?)"
                           : (err == EACCES) ? " (socket exists but is not accessible to this user)"
                                             : "";
        // 保留 errno 域：调用方可以用 is_errno(e, ENOENT) 区分"没起来"和"没权限"，
        // 那两种情况的处理动作完全不同。
        return sys_err_ctx("connect", fmt("{}{}", path, hint), err);
    }

    LOG_INFO("connected to {}", path);
    return Ok(std::move(sock));
}

Result<std::pair<UniqueFd, UniqueFd>> make_socket_pair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) != 0) {
        return sys_err("socketpair(AF_UNIX, SOCK_SEQPACKET)");
    }
    return Ok(std::make_pair(UniqueFd(fds[0]), UniqueFd(fds[1])));
}

} // namespace mw::ipc
