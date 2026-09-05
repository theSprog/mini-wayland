#include "mw/drm/caps.hpp"

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstring>

#include "mw/trace/log.hpp"
#include "mw/drm/property.hpp"
#include "mw/drm/trace.hpp"

using internal::Ok;
using internal::Err;
using internal::fmt;

namespace mw::drm {
namespace {

/// drmGetCap 的薄包装
///
/// 语义要点：**返回非 0 不代表"不支持"，代表"内核不认识这个 cap"**。
/// 老内核对新加的 cap 返回 EINVAL。两种情况我们都当作 false，
/// 但日志要分开 —— "驱动说不支持"和"内核太老不认识"排查方向完全不同。
bool probe_cap(BorrowedFd fd, uint64_t capability, const char* name) {
    uint64_t value = 0;
    const int ret = MW_DRM_CALL(get_cap, drmGetCap(fd.get(), capability, &value), "cap={} ({})",
                                capability, name);
    if (ret != 0) {
        LOG_DEBUG("cap {:<28} unknown to this kernel (drmGetCap -> {})", name, errno_name(errno));
        return false;
    }
    LOG_DEBUG("cap {:<28} = {}", name, value);
    return value != 0u;
}

uint64_t probe_cap_value(BorrowedFd fd, uint64_t capability, const char* name) {
    uint64_t value = 0;
    const int ret = MW_DRM_CALL(get_cap, drmGetCap(fd.get(), capability, &value), "cap={} ({})",
                                capability, name);
    if (ret != 0) {
        LOG_DEBUG("cap {:<28} unknown to this kernel", name);
        return 0u;
    }
    LOG_DEBUG("cap {:<28} = {}", name, value);
    return value;
}

/// drmSetClientCap 的薄包装。设不上不报错 —— 是否致命由 check_minimum() 判定。
bool request_client_cap(BorrowedFd fd, uint64_t capability, const char* name) {
    const int ret = MW_DRM_CALL(set_client_cap, drmSetClientCap(fd.get(), capability, 1),
                                "client_cap={} ({})", capability, name);
    if (ret != 0) {
        LOG_DEBUG("client cap {:<24} DENIED ({})", name, errno_name(errno));
        return false;
    }
    LOG_DEBUG("client cap {:<24} granted", name);
    return true;
}

void append_flag(std::string& out, const char* name, bool value) {
    out += "  ";
    out += name;
    const size_t pad = 30u;
    for (size_t i = std::strlen(name); i < pad; ++i) {
        out += ' ';
    }
    out += value ? "yes" : "no";
    out += '\n';
}

void append_number(std::string& out, const char* name, uint64_t value) {
    out += "  ";
    out += name;
    const size_t pad = 30u;
    for (size_t i = std::strlen(name); i < pad; ++i) {
        out += ' ';
    }
    out += std::to_string(value);
    out += '\n';
}

} // namespace

Result<DeviceCaps> probe_kernel_caps(BorrowedFd fd) {
    DeviceCaps caps;

    // ---- 驱动身份。仅用于日志，主逻辑不得据此分支。----
    if (auto* version = drmGetVersion(fd.get()); version != nullptr) {
        if (version->name != nullptr) {
            caps.driver_name.assign(version->name,
                                    static_cast<size_t>(version->name_len > 0 ? version->name_len
                                                                              : 0));
        }
        if (version->desc != nullptr) {
            caps.driver_desc.assign(version->desc,
                                    static_cast<size_t>(version->desc_len > 0 ? version->desc_len
                                                                              : 0));
        }
        caps.version_major = static_cast<uint32_t>(version->version_major);
        caps.version_minor = static_cast<uint32_t>(version->version_minor);
        drmFreeVersion(version);
    }
    LOG_INFO("DRM driver: '{}' v{}.{} ({})", caps.driver_name, caps.version_major,
             caps.version_minor, caps.driver_desc);

    // ---- client caps ----
    //
    // 顺序有讲究：先 UNIVERSAL_PLANES 再 ATOMIC。内核在设 ATOMIC 时会隐式
    // 打开 universal planes，但反过来不成立；而且在老内核上分开设的行为
    // 更可预期。
    //
    // 副作用提醒：这里**改变了 fd 的状态**。同一个 fd 在别处再设一次
    // 是无害的，但如果有人依赖"设置前"的枚举结果（legacy 模式下
    // drmModeGetPlaneResources 只返回 overlay plane），就会看到不一致。
    // 全工程只有这一个设置点。
    LOG_DEBUG("requesting client capabilities");
    {
        LOG_SCOPE();
        caps.universal_planes =
            request_client_cap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, "UNIVERSAL_PLANES");
        caps.atomic = request_client_cap(fd, DRM_CLIENT_CAP_ATOMIC, "ATOMIC");
        caps.writeback_connectors =
            request_client_cap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, "WRITEBACK_CONNECTORS");
        caps.aspect_ratio = request_client_cap(fd, DRM_CLIENT_CAP_ASPECT_RATIO, "ASPECT_RATIO");
    }

    if (caps.atomic && ! caps.universal_planes) {
        // ATOMIC 隐式带上 universal planes，即使显式设置那一步失败了。
        LOG_DEBUG("ATOMIC granted, so universal planes are implicitly enabled");
        caps.universal_planes = true;
    }

    // ---- drmGetCap ----
    LOG_DEBUG("probing kernel capabilities");
    {
        LOG_SCOPE();
        caps.dumb_buffer = probe_cap(fd, DRM_CAP_DUMB_BUFFER, "DUMB_BUFFER");
        caps.prime_import = (probe_cap_value(fd, DRM_CAP_PRIME, "PRIME") & DRM_PRIME_CAP_IMPORT) != 0u;
        caps.prime_export = (probe_cap_value(fd, DRM_CAP_PRIME, "PRIME") & DRM_PRIME_CAP_EXPORT) != 0u;
        caps.addfb2_modifiers = probe_cap(fd, DRM_CAP_ADDFB2_MODIFIERS, "ADDFB2_MODIFIERS");
        caps.timestamp_monotonic = probe_cap(fd, DRM_CAP_TIMESTAMP_MONOTONIC, "TIMESTAMP_MONOTONIC");
        caps.crtc_in_vblank_event = probe_cap(fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, "CRTC_IN_VBLANK_EVENT");
        caps.syncobj = probe_cap(fd, DRM_CAP_SYNCOBJ, "SYNCOBJ");
        caps.syncobj_timeline = probe_cap(fd, DRM_CAP_SYNCOBJ_TIMELINE, "SYNCOBJ_TIMELINE");
        caps.async_page_flip = probe_cap(fd, DRM_CAP_ASYNC_PAGE_FLIP, "ASYNC_PAGE_FLIP");
        // TODO(kernel-6.6): DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 是 6.8+ 才有的，
        // 当前 libdrm 头里可能没这个宏。等升级内核后再加，不用 #ifdef 绕。

        caps.cursor_width = probe_cap_value(fd, DRM_CAP_CURSOR_WIDTH, "CURSOR_WIDTH");
        caps.cursor_height = probe_cap_value(fd, DRM_CAP_CURSOR_HEIGHT, "CURSOR_HEIGHT");
    }

    if (! caps.timestamp_monotonic) {
        // 老驱动的 vblank 时间戳是 CLOCK_REALTIME，NTP 一跳帧节拍就乱了。
        LOG_WARN("vblank timestamps are CLOCK_REALTIME, not MONOTONIC; "
                 "frame pacing will be sensitive to clock adjustments");
    }

    return Ok(std::move(caps));
}

void refine_caps_from_objects(DeviceCaps& caps, const PropertyMap& any_crtc_props,
                              const PropertyMap& any_plane_props) {
    // drmGetCap 报告的是"内核有没有这个 UAPI"，属性存在性报告的是
    // "这个驱动实现了没有"。两者都要看。
    //
    // 只看"任意一个"对象是有意的：KMS 里同类对象的属性集合由驱动统一注册，
    // 不同 plane 之间只可能差 IN_FORMATS 的**内容**，不会差属性有没有。
    // 真出现不一致，Step 5 的 TEST_ONLY 会暴露出来。
    caps.prop_crtc_out_fence_ptr = any_crtc_props.has("OUT_FENCE_PTR");
    caps.prop_crtc_vrr_enabled = any_crtc_props.has("VRR_ENABLED");

    caps.prop_plane_in_fence_fd = any_plane_props.has("IN_FENCE_FD");
    caps.prop_plane_in_formats = any_plane_props.has("IN_FORMATS");
    caps.prop_plane_zpos = any_plane_props.has("zpos");
    caps.prop_plane_alpha = any_plane_props.has("alpha");
    caps.prop_plane_pixel_blend_mode = any_plane_props.has("pixel blend mode");

    LOG_DEBUG("refined caps from object properties:");
    LOG_SCOPE();
    LOG_DEBUG("CRTC  OUT_FENCE_PTR    = {}", caps.prop_crtc_out_fence_ptr);
    LOG_DEBUG("CRTC  VRR_ENABLED      = {}", caps.prop_crtc_vrr_enabled);
    LOG_DEBUG("plane IN_FENCE_FD      = {}", caps.prop_plane_in_fence_fd);
    LOG_DEBUG("plane IN_FORMATS       = {}", caps.prop_plane_in_formats);
    LOG_DEBUG("plane zpos             = {}", caps.prop_plane_zpos);
    LOG_DEBUG("plane alpha            = {}", caps.prop_plane_alpha);
    LOG_DEBUG("plane pixel blend mode = {}", caps.prop_plane_pixel_blend_mode);

    // 显式同步是成对的：只有 in-fence 没有 out-fence（或反之）用不起来。
    if (caps.prop_plane_in_fence_fd != caps.prop_crtc_out_fence_ptr) {
        LOG_WARN("explicit sync is only half-implemented by this driver "
                 "(IN_FENCE_FD={}, OUT_FENCE_PTR={}); Step 6 will have to fall back",
                 caps.prop_plane_in_fence_fd, caps.prop_crtc_out_fence_ptr);
    }
}

Status DeviceCaps::check_minimum() const {
    // 只检查真正没有替代路径的能力。
    // dumb_buffer 之类是"某个 Step 需要"，由各 Step 自己检查 ——
    // 否则 Step 3 之后的纯 DMA-BUF 路径会被一个用不到的 dumb_buffer 挡住。
    if (! atomic) {
        return Err(Errc::NoAtomicSupport,
                   fmt("driver '{}' does not accept DRM_CLIENT_CAP_ATOMIC; "
                       "this project has no legacy KMS path",
                       driver_name));
    }
    if (! universal_planes) {
        return Err(Errc::NoUniversalPlanes, fmt("driver '{}' does not expose universal planes",
                                                driver_name));
    }
    return Ok();
}

std::string DeviceCaps::to_string() const {
    std::string out;
    out.reserve(1024);

    out += "driver: ";
    out += driver_name;
    out += " v";
    out += std::to_string(version_major);
    out += ".";
    out += std::to_string(version_minor);
    out += " (";
    out += driver_desc;
    out += ")\n";

    out += "client caps (what we asked the kernel for):\n";
    append_flag(out, "ATOMIC", atomic);
    append_flag(out, "UNIVERSAL_PLANES", universal_planes);
    append_flag(out, "WRITEBACK_CONNECTORS", writeback_connectors);
    append_flag(out, "ASPECT_RATIO", aspect_ratio);

    out += "kernel caps (drmGetCap):\n";
    append_flag(out, "DUMB_BUFFER", dumb_buffer);
    append_flag(out, "PRIME_IMPORT", prime_import);
    append_flag(out, "PRIME_EXPORT", prime_export);
    append_flag(out, "ADDFB2_MODIFIERS", addfb2_modifiers);
    append_flag(out, "TIMESTAMP_MONOTONIC", timestamp_monotonic);
    append_flag(out, "CRTC_IN_VBLANK_EVENT", crtc_in_vblank_event);
    append_flag(out, "SYNCOBJ", syncobj);
    append_flag(out, "SYNCOBJ_TIMELINE", syncobj_timeline);
    append_flag(out, "ASYNC_PAGE_FLIP", async_page_flip);
    append_number(out, "CURSOR_WIDTH", cursor_width);
    append_number(out, "CURSOR_HEIGHT", cursor_height);

    out += "driver-implemented properties:\n";
    append_flag(out, "CRTC OUT_FENCE_PTR", prop_crtc_out_fence_ptr);
    append_flag(out, "CRTC VRR_ENABLED", prop_crtc_vrr_enabled);
    append_flag(out, "plane IN_FENCE_FD", prop_plane_in_fence_fd);
    append_flag(out, "plane IN_FORMATS", prop_plane_in_formats);
    append_flag(out, "plane zpos", prop_plane_zpos);
    append_flag(out, "plane alpha", prop_plane_alpha);
    append_flag(out, "plane pixel blend mode", prop_plane_pixel_blend_mode);
    append_flag(out, "writeback connector", has_writeback_connector);

    out += "topology:\n";
    append_number(out, "connectors", num_connectors);
    append_number(out, "crtcs", num_crtcs);
    append_number(out, "planes", num_planes);

    return out;
}

} // namespace mw::drm
