/**
 * @file ipc/error.hpp
 * @brief `mw/ipc` 的错误域
 *
 * 与 `drm::Errc` 分开是有理由的：Step 3 的设计意图是
 * **"这一步里出错，一定是 IPC 的问题"**。错误域就是这句话在类型系统里的
 * 落实 —— 看见 domain 是 `ipc` 就不用去查 buffer 那一侧。
 *
 * 系统调用失败仍然走 `sys_err()` 的 errno 域，保留原始 errno：
 * `connect` 的 ENOENT（server 没起来）与 EACCES（起来了但权限不够）
 * 处理动作完全不同，靠 errno 分辨，不靠这里的枚举。
 */
#pragma once

#include "mw/core/error.hpp"

namespace mw::ipc {

enum class Errc : int {
    // ---- socket ----
    PathTooLong = 1,  ///< 超过 sockaddr_un::sun_path，**不截断**（截断后可能连上别的东西）
    ListenFailed,
    ConnectFailed,
    AcceptFailed,

    // ---- 收发 ----
    SendFailed,
    RecvFailed,
    ShortWrite,        ///< SEQPACKET 上不该发生，但仍然检查
    ControlTruncated,  ///< MSG_CTRUNC：内核对装不下的 fd 的处置不可依赖

    // ---- 协议 ----
    BadMagic,
    BadVersion,        ///< 两个二进制不是同一次构建出来的
    BadSize,
    BadFdCount,
    UnknownType,
    MalformedBody,

    // ---- 内容判据 ----
    SignatureMissing,   ///< 签名块的 magic 不对（全零 vs 垃圾值要在 message 里分开）
    SignatureMismatch,  ///< 读到了签名但与期望不符
    SignatureUnsupported, ///< 该格式承载不了签名，这一帧没有 L1/L2 覆盖

    Internal,
};

struct IpcError : internal::IError {
    DEFINE_ERROR_DOMAIN("ipc")

    explicit IpcError(Errc code) noexcept : code_(code) {}

    int error_code() const noexcept override {
        return static_cast<int>(code_);
    }

    const char* error_message() const noexcept override;

  private:
    Errc code_;
};

REGISTER_MAKE_ERROR(Errc, IpcError)

} // namespace mw::ipc
