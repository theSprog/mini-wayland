#include "mw/ipc/channel.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "mw/internal/expected.hpp"
#include "mw/trace/log.hpp"
#include "mw/ipc/error.hpp"

using internal::Ok;
using internal::Err;
using internal::fmt;
using internal::unexpected;
using internal::sys_err;
using internal::sys_err_ctx;

namespace mw::ipc {
namespace {

/// 控制缓冲区按 fd 数上限静态开满。见 channel.hpp 规则 3：
/// 内核对装不下的 fd 的处置在不同版本上不一致，不给它发生的机会。
constexpr size_t kControlSize = CMSG_SPACE(sizeof(int) * kMaxMessageFds);

} // namespace

const char* to_string(RecvStatus status) noexcept {
    switch (status) {
        case RecvStatus::Message:    return "message";
        case RecvStatus::Closed:     return "closed";
        case RecvStatus::WouldBlock: return "would-block";
    }
    return "unknown";
}

Channel::Channel(UniqueFd sock) noexcept : sock_(std::move(sock)), counters_() {}

// ---------------------------------------------------------------------------
// 发送
// ---------------------------------------------------------------------------

Status Channel::send(MsgType type, const void* body, uint32_t body_size,
                     span<const BorrowedFd> fds) {
    if (! sock_.valid()) {
        return Err(Errc::SendFailed, "send() on a closed channel");
    }
    if (body_size > kMaxBodySize) {
        return Err(Errc::BadSize,
                   fmt("body of {} is {} bytes, the limit is {}", to_string(type), body_size,
                       kMaxBodySize));
    }
    if (fds.size() > kMaxMessageFds) {
        return Err(Errc::BadFdCount,
                   fmt("cannot send {} fd(s), the limit is {}", fds.size(), kMaxMessageFds));
    }

    MessageHeader header{};
    header.type = static_cast<uint16_t>(type);
    header.body_size = body_size;
    header.fd_count = static_cast<uint32_t>(fds.size());

    iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = const_cast<void*>(body);
    iov[1].iov_len = body_size;

    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = (body_size > 0) ? 2 : 1;

    alignas(cmsghdr) char control[kControlSize] = {};
    if (! fds.empty()) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());

        cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());

        // fd 只是**借用**：SCM_RIGHTS 在内核里给它加引用，
        // 关掉自己那份是调用方的事（见 channel.hpp）。
        int* dst = reinterpret_cast<int*>(CMSG_DATA(cmsg));
        for (size_t i = 0; i < fds.size(); ++i) {
            if (! fds[i].valid()) {
                return Err(Errc::Internal, fmt("fd slot {} is invalid", i));
            }
            dst[i] = fds[i].get();
        }
    }

    ssize_t written = -1;
    do {
        written = ::sendmsg(sock_.get(), &msg, MSG_NOSIGNAL);
    } while (written < 0 && errno == EINTR);

    if (written < 0) {
        return sys_err_ctx("sendmsg", to_string(type));
    }

    const size_t expected = sizeof(header) + body_size;
    if (static_cast<size_t>(written) != expected) {
        // SEQPACKET 上要么整条发出去要么失败，短写不该发生。
        // 仍然检查：依赖"不会发生"和检查它的代价差得远。
        return Err(Errc::ShortWrite, fmt("sendmsg wrote {} of {} byte(s) for {}", written, expected,
                                         to_string(type)));
    }

    counters_.messages_sent += 1;
    counters_.bytes_sent += static_cast<uint64_t>(written);
    counters_.fds_sent += fds.size();
    LOG_TRACE("sent {} body={}B fds={}", to_string(type), body_size, fds.size());
    return {};
}

Status Channel::send_error(WireError code, std::string_view detail) {
    ErrorBody body{};
    body.code = static_cast<uint32_t>(code);
    const size_t n = detail.size() < (kMaxErrorDetail - 1) ? detail.size() : (kMaxErrorDetail - 1);
    std::memcpy(body.detail, detail.data(), n);
    body.detail[n] = '\0';
    return send(body);
}

// ---------------------------------------------------------------------------
// 接收
// ---------------------------------------------------------------------------

Result<RecvStatus> Channel::recv(Message& out) {
    if (! sock_.valid()) {
        return Err(Errc::RecvFailed, "recv() on a closed channel");
    }

    out.close_fds();

    iovec iov[2];
    iov[0].iov_base = &out.header_;
    iov[0].iov_len = sizeof(out.header_);
    iov[1].iov_base = out.body_;
    iov[1].iov_len = sizeof(out.body_);

    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    alignas(cmsghdr) char control[kControlSize] = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t received = -1;
    do {
        // MSG_CMSG_CLOEXEC 不能省：合成器会 fork/exec，漏一个 dmabuf fd
        // 到子进程意味着那块显存永远不释放，而且现场看不出来。
        received = ::recvmsg(sock_.get(), &msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Ok(RecvStatus::WouldBlock);
        }
        return sys_err("recvmsg");
    }

    // 先把 fd 收进 UniqueFd，**在任何校验之前** —— 后面每一条 early return
    // 都要保证不漏 fd，最简单的做法就是让它们从一开始就有主。
    uint32_t received_fds = 0;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const size_t payload = cmsg->cmsg_len - CMSG_LEN(0);
        const size_t count = payload / sizeof(int);
        const int* src = reinterpret_cast<const int*>(CMSG_DATA(cmsg));
        for (size_t i = 0; i < count; ++i) {
            if (received_fds < kMaxMessageFds) {
                out.fds_[received_fds] = UniqueFd(src[i]);
                received_fds += 1;
            } else {
                // 多出来的也要关，否则对端多发几个就能把我们的 fd 表撑爆。
                ::close(src[i]);
                received_fds += 1;
            }
        }
    }
    out.fd_count_ = (received_fds <= kMaxMessageFds) ? received_fds : kMaxMessageFds;

    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
        out.close_fds();
        return Err(Errc::ControlTruncated,
                   "ancillary data was truncated -- the peer sent more fds than the protocol allows");
    }

    if (received == 0) {
        out.close_fds();
        return Ok(RecvStatus::Closed);
    }

    if (static_cast<size_t>(received) < sizeof(MessageHeader)) {
        out.close_fds();
        return Err(Errc::BadSize,
                   fmt("received {} byte(s), too short for a {}-byte header", received,
                       sizeof(MessageHeader)));
    }

    const uint32_t body_bytes = static_cast<uint32_t>(static_cast<size_t>(received) -
                                                      sizeof(MessageHeader));
    if (auto ok = validate_header(out.header_, body_bytes, received_fds); ! ok) {
        out.close_fds();
        return unexpected<Error>(ok.error());
    }

    counters_.messages_received += 1;
    counters_.bytes_received += static_cast<uint64_t>(received);
    counters_.fds_received += out.fd_count_;
    LOG_TRACE("received {}", out.to_string());
    return Ok(RecvStatus::Message);
}

Status Channel::set_nonblocking(bool on) {
    const int flags = ::fcntl(sock_.get(), F_GETFL, 0);
    if (flags < 0) {
        return sys_err("fcntl(F_GETFL)");
    }
    const int updated = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(sock_.get(), F_SETFL, updated) != 0) {
        return sys_err("fcntl(F_SETFL)");
    }
    return {};
}

void Channel::close() noexcept {
    sock_.reset();
}

std::string Channel::counters_to_string() const {
    return fmt("messages {}/{} (tx/rx), bytes {}/{}, fds {}/{}", counters_.messages_sent,
               counters_.messages_received, counters_.bytes_sent, counters_.bytes_received,
               counters_.fds_sent, counters_.fds_received);
}

} // namespace mw::ipc
