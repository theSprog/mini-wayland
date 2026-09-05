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

#include "mw/internal/error.hpp"

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

inline const char* DrmError::error_message() const noexcept {
    switch (code_) {
        case Errc::OpenFailed:
            return "failed to open DRM node";
        case Errc::NotKmsCapable:
            return "node has no KMS resources (render node, or a card with no display engine)";
        case Errc::NoAtomicSupport:
            return "DRM_CLIENT_CAP_ATOMIC not available; this project is atomic-only";
        case Errc::NoUniversalPlanes:
            return "DRM_CLIENT_CAP_UNIVERSAL_PLANES not available";
        case Errc::NoDumbBuffer:
            return "DRM_CAP_DUMB_BUFFER is 0; CPU-drawn buffers unavailable";
        case Errc::NotMaster:
            return "not DRM master";
        case Errc::ResourceQueryFailed:
            return "failed to query DRM resources";
        case Errc::NoConnectedConnector:
            return "no connected connector found";
        case Errc::NoModeAvailable:
            return "connector is connected but reports no modes";
        case Errc::NoCompatibleCrtc:
            return "no CRTC in the encoder's possible_crtcs bitmask is usable";
        case Errc::NoCompatiblePlane:
            return "no plane of the requested type is usable on this CRTC";
        case Errc::StaleSnapshot:
            return "resource snapshot is stale; rescan() happened after this path was computed";
        case Errc::PropertyNotFound:
            return "required KMS property not found on object";
        case Errc::PropertyTypeMismatch:
            return "KMS property has an unexpected type";
        case Errc::BlobCreateFailed:
            return "failed to create property blob";
        case Errc::BlobReadFailed:
            return "failed to read property blob";
        case Errc::DumbCreateFailed:
            return "DRM_IOCTL_MODE_CREATE_DUMB failed";
        case Errc::DumbMapFailed:
            return "DRM_IOCTL_MODE_MAP_DUMB or mmap failed";
        case Errc::AddFbFailed:
            return "drmModeAddFB2 failed";
        case Errc::AtomicTestFailed:
            return "atomic TEST_ONLY rejected by the kernel";
        case Errc::AtomicCommitFailed:
            return "atomic commit rejected by the kernel";
        case Errc::Unsupported:
            return "capability not present on this kernel or driver, and no fallback exists";
        case Errc::Internal:
            return "internal error";
    }
    return "unknown drm error";
}

} // namespace mw::drm
