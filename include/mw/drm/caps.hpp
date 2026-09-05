/**
 * @file drm/caps.hpp
 * @brief 运行时能力探测 —— 把内核版本 / 驱动 / 平台的差异全部收敛到这一个结构体
 *
 * 原则（来自环境勘察结论第 4.3 节）：**不用 `#ifdef`**。
 * 同一份二进制要能在 5.4 与 6.6、VKMS 与 vsdrm 上都跑起来，只是走不同分支。
 * 编译期开关会让"在这台板子上能编过"变成"只能在这台板子上跑"。
 *
 * 探测分两阶段，因为有鸡生蛋问题：
 *   阶段 A `probe_kernel_caps()`：只做 drmGetCap / drmSetClientCap，
 *                                不需要枚举资源。
 *   阶段 B `DeviceCaps::refine_from_objects()`：需要真实的 connector /
 *                                crtc / plane 属性表才能知道
 *                                OUT_FENCE_PTR、IN_FORMATS 这些属性在不在。
 *                                由 Device 在枚举完成后调用。
 *
 * 注意 drmGetCap 报告的是**内核有没有这个 UAPI**，属性存在性报告的是
 * **这个驱动实现了没有**。两者都要看。勘察结果里 vsdrm 的
 * atomic commit 返回 EBUSY 就属于"UAPI 有、驱动行为不对"，caps 探不出来，
 * 只能靠启动时那次 TEST_ONLY 试探。
 */
#pragma once

#include <cstdint>
#include <string>

#include "mw/internal/error.hpp"
#include "mw/internal/unique_fd.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/types.hpp"

using internal::Status;
using internal::Result;
using internal::BorrowedFd;

namespace mw::drm {

class PropertyMap;

struct DeviceCaps {
    // ---- 驱动身份（仅诊断用，主逻辑不得据此分支）------------------------
    std::string driver_name{};  ///< DRM driver name，如 "vsdrm" / "vkms"
    std::string driver_desc{};
    uint32_t version_major = 0;
    uint32_t version_minor = 0;

    // ---- 阶段 A：drmSetClientCap 实际拿到的 ------------------------------
    bool atomic = false;             ///< 本工程硬要求，false 直接退出
    bool universal_planes = false;   ///< 硬要求（设 ATOMIC 会隐式带上）
    bool writeback_connectors = false;   ///< 可选，勘察到 vsdrm 有 2 个 writeback
    bool aspect_ratio = false;

    // ---- 阶段 A：drmGetCap ------------------------------------------------
    bool dumb_buffer = false;        ///< Step 1 必需
    bool prime_import = false;       ///< Step 3 必需
    bool prime_export = false;       ///< Step 3 必需
    bool addfb2_modifiers = false;   ///< Step 2 必需；无则只能走 LINEAR
    bool timestamp_monotonic = false;///< Step 7：vblank 时间戳是不是 CLOCK_MONOTONIC
    bool crtc_in_vblank_event = false;
    bool syncobj = false;            ///< Step 6
    bool syncobj_timeline = false;   ///< Step 6，timeline 是 5.11+
    bool async_page_flip = false;    ///< legacy 的异步翻页；atomic 版是 6.8+
                                     ///< TODO(kernel-6.6): DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP

    uint64_t cursor_width = 0;       ///< 硬件光标推荐尺寸，0 = 未知
    uint64_t cursor_height = 0;

    // ---- 阶段 B：属性存在性（枚举后填充）---------------------------------
    bool prop_crtc_out_fence_ptr = false;  ///< Step 6 的 out-fence
    bool prop_plane_in_fence_fd = false;   ///< Step 6 的 in-fence
    bool prop_plane_in_formats = false;    ///< Step 2 的 modifier 协商
    bool prop_plane_zpos = false;          ///< Step 5
    bool prop_plane_alpha = false;
    bool prop_plane_pixel_blend_mode = false;
    bool prop_crtc_vrr_enabled = false;    ///< Step 7
    bool has_writeback_connector = false;  ///< TODO(writeback): 无显示器自检管线

    // ---- 拓扑规模（仅用于日志与断言）--------------------------------------
    uint32_t num_crtcs = 0;
    uint32_t num_connectors = 0;
    uint32_t num_planes = 0;

    /**
     * @brief 本工程跑起来的最低要求是否满足
     *
     * 只检查真正没有替代路径的能力：atomic + universal planes。
     * dumb_buffer 之类是"某个 Step 需要"，由各 Step 自己检查，
     * 不在这里一票否决 —— 否则 Step 3 之后的纯 DMA-BUF 路径会被
     * 一个用不到的 dumb_buffer 挡住。
     */
    Status check_minimum() const;

    /// 多行、人类可读，启动时打一次
    std::string to_string() const;
};

/**
 * @brief 阶段 A 探测
 *
 * 会**修改 fd 的 client cap 状态**（这是唯一的设置点）：
 * 依次尝试 UNIVERSAL_PLANES、ATOMIC、WRITEBACK_CONNECTORS、ASPECT_RATIO，
 * 设不上的记 false，不报错 —— 是否致命由 check_minimum() 判定。
 *
 * 顺序有讲究：先 UNIVERSAL_PLANES 再 ATOMIC。虽然内核在
 * 设 ATOMIC 时会隐式打开 universal planes，但反过来不成立，
 * 而且老内核上分开设的行为更可预期。
 */
Result<DeviceCaps> probe_kernel_caps(BorrowedFd fd);

/**
 * @brief 阶段 B：用已枚举出的属性表补齐 caps 的后半段
 *
 * @param caps           待补齐（就地修改）
 * @param any_crtc_props 任一 CRTC 的属性表
 * @param any_plane_props 任一 plane 的属性表
 *
 * 只看"任意一个"是有意的：KMS 里同类对象的属性集合由驱动统一注册，
 * 不同 plane 之间只可能差 IN_FORMATS 的内容，不会差属性有没有。
 * 真出现不一致，Step 5 的 plane 分配器会在 TEST_ONLY 阶段暴露出来。
 */
void refine_caps_from_objects(DeviceCaps& caps,
                              const PropertyMap& any_crtc_props,
                              const PropertyMap& any_plane_props);

} // namespace mw::drm
