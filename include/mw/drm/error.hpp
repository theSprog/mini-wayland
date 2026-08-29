/**
 * @file drm/error.hpp
 * @brief drm 模块的语义错误码（domain = "drm"）
 *
 * 与 core/error.hpp 的 "errno" 域分工：
 *   - 内核明确返回了 errno  → `sys_err("drmModeXxx")`，保留 EBUSY/EINVAL 原值
 *   - 我们自己判定的失败    → 这里的 Errc，例如"没有 connected 的 connector"
 *
 * 上层判定写法：
 * @code
 *   if (r.error().is(Errc::NoAtomicSupport)) { ... }      // 语义码
 *   if (is_errno(r.error(), EBUSY))          { ... }      // 内核码
 * @endcode
 */
#pragma once

#include "mw/core/error.hpp"

namespace mw::drm {

enum class Errc : int {
    // ---- 设备打开 / 能力 ----
    OpenFailed = 1,      ///< open(2) 失败（细节在 errno 域的 cause 里）
    NotKmsCapable,       ///< 打开成功但没有 KMS 资源（render node 或无显示的 card）
    NoAtomicSupport,     ///< DRM_CLIENT_CAP_ATOMIC 设不上 —— 本工程硬要求
    NoUniversalPlanes,   ///< DRM_CLIENT_CAP_UNIVERSAL_PLANES 设不上
    NoDumbBuffer,        ///< DRM_CAP_DUMB_BUFFER 为 0，Step 1 走不了
    NotMaster,           ///< 拿不到 DRM master（X11/别的合成器占着）

    // ---- 资源枚举 ----
    ResourceQueryFailed, ///< drmModeGetResources / GetPlaneResources 失败
    NoConnectedConnector,
    NoModeAvailable,     ///< connector 已连接但 mode 列表为空
    NoCompatibleCrtc,    ///< 遍历 possible_crtcs 后没有可用 CRTC
    NoCompatiblePlane,   ///< 该 CRTC 上找不到对应类型的 plane
    StaleSnapshot,       ///< 用了 rescan() 之前算出来的 OutputPath

    // ---- 属性 / blob ----
    PropertyNotFound,    ///< 按名字找属性没找到（附带对象与属性名）
    PropertyTypeMismatch,
    BlobCreateFailed,
    BlobReadFailed,

    // ---- 帧缓冲 ----
    DumbCreateFailed,
    DumbMapFailed,
    AddFbFailed,

    // ---- 提交 ----
    AtomicTestFailed,    ///< TEST_ONLY 被内核拒绝（可恢复，通常触发降级）
    AtomicCommitFailed,

    // ---- 兜底 ----
    Unsupported,         ///< 当前内核/驱动没这个能力，且没有降级路径
    Internal,
};

struct DrmError : internal::IError {
    DEFINE_ERROR_DOMAIN("drm")

    explicit DrmError(Errc code) noexcept : code_(code) {}

    int error_code() const noexcept override {
        return static_cast<int>(code_);
    }

    const char* error_message() const noexcept override;

  private:
    Errc code_;
};

REGISTER_MAKE_ERROR(Errc, DrmError)

} // namespace mw::drm
