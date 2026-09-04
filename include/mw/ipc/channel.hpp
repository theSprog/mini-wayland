/**
 * @file ipc/channel.hpp
 * @brief 一条连接上的消息收发：`SCM_RIGHTS` 传 fd
 *
 * ## fd 的五条规则（这里是泄漏和双关最集中的地方）
 *
 * 1. `recvmsg` 一律带 `MSG_CMSG_CLOEXEC`。不带就是往子进程漏 dmabuf，
 *    现场看不出来，那块显存永远不释放。
 * 2. 收到的 fd 数必须等于 `header.fd_count`，`fd_count` 必须与消息类型
 *    要求的数量一致。不等即协议错误。
 * 3. `MSG_CTRUNC` 一律当致命协议错误。控制缓冲区不够时内核对装不下的 fd
 *    如何处置，**不同版本行为不完全一致，不要依赖任何一种**。
 *    这里把缓冲区按 `kMaxMessageFds` 静态开满，触发了就说明对端在乱发。
 * 4. **任何一步解析失败，已收到的 fd 必须全部关闭再返回错误。**
 *    契约写死：要么返回一个持有全部 fd 的 `Message`，要么一个都不留下。
 * 5. 收到的 fd 立刻装进 `Message` 的 `UniqueFd`，裸 `int` 不越过一个函数边界。
 *
 * ## 发送侧不转移所有权
 *
 * `send()` 只**借用** fd。`SCM_RIGHTS` 是在内核里给 fd 加引用，
 * 发送方随后关闭自己那份是正确且必要的（否则引用永远不掉到 0），
 * 但那是调用方的事 —— 由持有 buffer 的对象在它该死的时候关。
 * 传输层替调用方关 fd 会让所有权变成"看情况"，这正是 `UniqueFd`
 * 存在的意义要消灭的东西。
 *
 * ## 阻塞语义
 *
 * 默认阻塞。合成器的帧循环要同时等 DRM fd 和 socket，
 * 所以提供 `set_nonblocking()` 与 `fd()`，poll 的组织留给调用方 ——
 * 这一层不引入自己的事件循环，Step 4 会有真正的那个。
 */
#pragma once

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/ipc/wire.hpp"

namespace mw::ipc {

/// `recv()` 的三种结果。**"对端关闭"不是错误**，是正常的生命周期事件。
enum class RecvStatus {
    Message,    ///< 收到一条完整消息
    Closed,     ///< 对端关闭连接（EOF）
    WouldBlock, ///< 非阻塞模式下暂时无数据
};

const char* to_string(RecvStatus status) noexcept;

/**
 * @brief 一条已连接的 socket 上的消息收发
 *
 * move-only，持有 socket fd。
 */
class Channel {
  public:
    Channel() = default;
    ~Channel() = default;

    explicit Channel(UniqueFd sock) noexcept;

    Channel(Channel&&) noexcept = default;
    Channel& operator=(Channel&&) noexcept = default;
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    // ---- 发送 ----

    /**
     * @brief 发一条消息
     *
     * @param fds 借用，本函数不改变其所有权（见文件头）。
     *            数量会写进 `header.fd_count`。
     *
     * 短写在 SEQPACKET 上不会发生（要么整条发出去，要么失败），
     * 但实现仍然检查返回的字节数 —— 依赖"不会发生"和检查它的代价差得远。
     */
    Status send(MsgType type, const void* body, uint32_t body_size,
                span<const BorrowedFd> fds = {});

    /// 类型安全的重载：body 类型自带 `kType`，写不错消息类型
    template <typename Body>
    Status send(const Body& body, span<const BorrowedFd> fds = {}) {
        return send(Body::kType, &body, static_cast<uint32_t>(sizeof(Body)), fds);
    }

    /**
     * @brief 发一条 `ERROR` 并**不**关闭连接
     *
     * 关不关由调用方决定：协议错误通常要断，但"这个 buffer 导入失败"
     * 未必要断整条连接。
     *
     * @param detail **英文**，会被截断到 `kMaxErrorDetail - 1`。
     *               诊断粒度要求见 `docs/step3-design.md` 8.4。
     */
    Status send_error(WireError code, std::string_view detail);

    // ---- 接收 ----

    /**
     * @brief 收一条消息
     *
     * 成功时 `out` 持有全部 fd。返回错误时 `out` 不含任何 fd（规则 4）。
     *
     * 已经校验过的：magic、abi 版本、`body_size` 与实收字节、
     * `fd_count` 与实收 fd 数、`MSG_CTRUNC`。**没有**校验 body 内容 ——
     * 那要按消息类型分别做，是调用方的事（`validate(CreateBufferBody, ...)`）。
     */
    Result<RecvStatus> recv(Message& out);

    Status set_nonblocking(bool on);

    /// 给 poll/epoll 用
    BorrowedFd fd() const noexcept {
        return sock_.borrow();
    }

    bool valid() const noexcept {
        return sock_.valid();
    }

    /// 主动关闭。析构会做同样的事。
    void close() noexcept;

    // ---- 计数（观测手段是一等公民）----

    /**
     * @brief 收发计数与 fd 计数
     *
     * `fds_sent` / `fds_received` 用于验收时和两端的 `/proc/self/fd`
     * 数量对账。**一个恒定的偏差比没有计数更糟**（`lessons.md` L-8），
     * 所以这两个数是分开的两个计数器，不是一个带正负的差值。
     */
    struct Counters {
        uint64_t messages_sent = 0;
        uint64_t messages_received = 0;
        uint64_t bytes_sent = 0;
        uint64_t bytes_received = 0;
        uint64_t fds_sent = 0;
        uint64_t fds_received = 0;
    };

    const Counters& counters() const noexcept {
        return counters_;
    }

    std::string counters_to_string() const;

  private:
    UniqueFd sock_{};
    Counters counters_{};
};

} // namespace mw::ipc
