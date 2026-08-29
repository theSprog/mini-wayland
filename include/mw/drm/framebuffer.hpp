/**
 * @file drm/framebuffer.hpp
 * @brief KMS framebuffer 对象（fb_id）的 RAII
 *
 * fb_id 是 KMS 里"可以被 plane 扫描的一块内存"的句柄。它和内存本身是
 * **两个东西**：内存可能来自 dumb buffer（Step 1）、GBM BO（Step 2）、
 * 或者从别的进程导入的 DMA-BUF（Step 3）。fb_id 只是把
 * "GEM handle + 宽高 + 格式 + pitch + offset (+ modifier)" 打包成
 * 一个内核认识的 id。
 *
 * 所以 Framebuffer 单独成类，不绑死在 DumbBuffer 上 —— 后面三个 Step
 * 都会用它，只是喂进来的 handle 来源不同。
 *
 * 两条 addfb 路径的区别（这是 Step 2 会踩的坑，先在这里写清楚）：
 *
 *   drmModeAddFB2()               不传 modifier，内核按驱动默认推断排布
 *   drmModeAddFB2WithModifiers()  显式传 modifier，需要
 *                                 DRM_CAP_ADDFB2_MODIFIERS，且 flags 要带
 *                                 DRM_MODE_FB_MODIFIERS
 *
 * 二者语义不同：`AddFB2` + 线性内存 **不等于** `AddFB2WithModifiers` +
 * `DRM_FORMAT_MOD_LINEAR`。所以 Plane::formats 里"没有 modifier 信息"
 * 记的是 kModifierInvalid 而不是 kModifierLinear，防止误走后一条路径。
 * 勘察结果里 vsdrm 的私有 modifier 走 WithModifiers 返回 EINVAL，
 * 正是需要在这两条路径间降级的场景。
 */
#pragma once

#include <array>
#include <cstdint>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {

/// 一个 framebuffer 最多 4 个平面（YUV 的 Y/U/V/A）
inline constexpr size_t kMaxFbPlanes = 4;

/**
 * @brief addfb2 的入参打包
 *
 * 固定大小数组，没有堆分配 —— Step 3 之后每收到一个 client buffer 就要
 * 构造一次，这是准热路径。
 */
struct FramebufferDesc {
    Size size{};
    Format format{};

    /// 实际使用的平面数（XR24 是 1，NV12 是 2）
    uint32_t num_planes = 1;

    std::array<GemHandle, kMaxFbPlanes> handles{};
    std::array<uint32_t, kMaxFbPlanes> pitches{};
    std::array<uint32_t, kMaxFbPlanes> offsets{};

    /**
     * @brief 每个平面的 modifier
     *
     * kModifierInvalid 表示"没有 modifier 信息" -> 走 drmModeAddFB2。
     * 其他值（含 kModifierLinear）-> 走 drmModeAddFB2WithModifiers。
     * UAPI 要求所有平面的 modifier 相同，构造时会校验。
     */
    std::array<Modifier, kMaxFbPlanes> modifiers{};

    /// 是否要求显式 modifier 路径。由 modifiers[0] 推导，这里只是缓存结论。
    bool uses_modifiers() const noexcept;

    /// 单平面 XR24 之类的便捷构造
    static FramebufferDesc single_plane(Size size, Format format, GemHandle handle,
                                        uint32_t pitch, uint32_t offset = 0,
                                        Modifier modifier = kModifierInvalid) noexcept;

    /// 日志用，一行
    std::string to_string() const;
};

class Framebuffer {
  public:
    Framebuffer() = default;
    ~Framebuffer();

    Framebuffer(Framebuffer&& other) noexcept;
    Framebuffer& operator=(Framebuffer&& other) noexcept;
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    /**
     * @brief 注册 framebuffer
     *
     * 内部按 desc.uses_modifiers() 选路径。若选了 WithModifiers 而设备
     * 没有 DRM_CAP_ADDFB2_MODIFIERS，直接返回 Errc::Unsupported，
     * **不做隐式降级** —— 降级是调用方的策略决定，不该藏在这一层。
     */
    static Result<Framebuffer> add(BorrowedFd fd, const FramebufferDesc& desc);

    /**
     * @brief 带自动降级的注册
     *
     * 先试 WithModifiers；失败且 errno 是 EINVAL / ENOTSUP 时，
     * 丢掉 modifier 重试 AddFB2，并把降级这件事 LOG_WARN 出来。
     * 勘察结果里 vsdrm 的私有 modifier 就走这条。
     *
     * @param downgraded 出参，是否发生了降级
     */
    static Result<Framebuffer> add_with_fallback(BorrowedFd fd, const FramebufferDesc& desc,
                                                 bool* downgraded = nullptr);

    FbId id() const noexcept {
        return id_;
    }

    bool valid() const noexcept {
        return id_ != kNoFb;
    }

    const FramebufferDesc& desc() const noexcept {
        return desc_;
    }

    Size size() const noexcept {
        return desc_.size;
    }

    /// 提前注销（析构会再做一次，幂等）
    void reset() noexcept;

  private:
    Framebuffer(BorrowedFd fd, FbId id, const FramebufferDesc& desc) noexcept;

    BorrowedFd fd_{};
    FbId id_ = kNoFb;
    FramebufferDesc desc_{};
};

} // namespace mw::drm
