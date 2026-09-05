#include "mw/drm/framebuffer.hpp"

#include <xf86drmMode.h>

#include <cerrno>
#include <utility>

#include "mw/internal/error.hpp"
#include "mw/trace/log.hpp"
#include "mw/drm/trace.hpp"

using internal::Ok;
using internal::Err;
using internal::fmt;
using internal::Status;

namespace mw::drm {

bool FramebufferDesc::uses_modifiers() const noexcept {
    // 只看第 0 个平面：UAPI 要求一个 fb 的所有平面 modifier 相同。
    // kModifierInvalid 表示"没有 modifier 信息" -> 走 drmModeAddFB2。
    return modifiers[0] != kModifierInvalid;
}

FramebufferDesc FramebufferDesc::single_plane(Size size, Format format, GemHandle handle,
                                              uint32_t pitch, uint32_t offset,
                                              Modifier modifier) noexcept {
    FramebufferDesc desc;
    desc.size = size;
    desc.format = format;
    desc.num_planes = 1;
    desc.handles[0] = handle;
    desc.pitches[0] = pitch;
    desc.offsets[0] = offset;
    desc.modifiers[0] = modifier;
    // 其余三个平面的 modifier 也填成同一个值。内核在 DRM_MODE_FB_MODIFIERS
    // 路径下会检查所有 **使用中** 的平面，未使用的平面 handle 为 0 会被忽略，
    // 但填一致的值不会有坏处，而且 to_string 打出来更好读。
    for (size_t i = 1; i < kMaxFbPlanes; ++i) {
        desc.modifiers[i] = modifier;
    }
    return desc;
}

std::string FramebufferDesc::to_string() const {
    std::string out;
    out += drm::to_string(size);
    out += " ";
    out += drm::to_string(format);
    out += " planes=";
    out += std::to_string(num_planes);
    for (uint32_t i = 0; i < num_planes && i < kMaxFbPlanes; ++i) {
        out += fmt(" [{}: handle={} pitch={} offset={}]", i, drm::to_string(handles[i]),
                   pitches[i], offsets[i]);
    }
    out += " modifier=";
    out += drm::to_string(modifiers[0]);
    out += uses_modifiers() ? " (AddFB2WithModifiers)" : " (AddFB2)";
    return out;
}

// ---------------------------------------------------------------------------

Framebuffer::Framebuffer(BorrowedFd fd, FbId id, const FramebufferDesc& desc) noexcept
    : fd_(fd), id_(id), desc_(desc) {}

Framebuffer::~Framebuffer() {
    reset();
}

Framebuffer::Framebuffer(Framebuffer&& other) noexcept
    : fd_(other.fd_), id_(std::exchange(other.id_, kNoFb)), desc_(other.desc_) {}

Framebuffer& Framebuffer::operator=(Framebuffer&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        id_ = std::exchange(other.id_, kNoFb);
        desc_ = other.desc_;
    }
    return *this;
}

void Framebuffer::reset() noexcept {
    if (id_ == kNoFb) {
        return;
    }
    const uint32_t id = static_cast<uint32_t>(id_);
    const int ret = MW_DRM_CALL(rm_fb, drmModeRmFB(fd_.get(), id), "fb id={}", id);
    if (ret != 0) {
        LOG_WARN("drmModeRmFB({}) failed; kernel framebuffer leaked", id);
    } else {
        ++stats().fb_released;
        LOG_TRACE("removed framebuffer {}", id);
    }
    id_ = kNoFb;
}

namespace {

/**
 * @brief 真正下发 addfb2 的那一层，失败返回 errno 而不是 Error
 *
 * 拆出来是因为 add_with_fallback() 需要**按 errno 分支**决定要不要降级。
 * 如果只拿得到一个 drm 域的语义错误码，就没法区分"这个 modifier 不支持"
 * （该降级）和"handle 无效"（降级也没用）。
 *
 * 顺带避免了在降级路径上白构造一个 Error（那会拼两个 std::string）。
 *
 * @return 0 成功，否则是 errno
 */
int add_fb_ioctl(BorrowedFd fd, const FramebufferDesc& desc, uint32_t& fb_id) {
    // 转成 libdrm 要的裸数组。GemHandle 是强类型，这里是唯一需要脱下来的地方。
    uint32_t handles[kMaxFbPlanes] = {0, 0, 0, 0};
    uint64_t modifiers[kMaxFbPlanes] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < desc.num_planes; ++i) {
        handles[i] = static_cast<uint32_t>(desc.handles[i]);
        modifiers[i] = static_cast<uint64_t>(desc.modifiers[i]);
    }

    int ret = 0;
    if (desc.uses_modifiers()) {
        // 显式 modifier 路径需要 DRM_CAP_ADDFB2_MODIFIERS，且 flags 要带
        // DRM_MODE_FB_MODIFIERS。
        ret = MW_DRM_CALL(add_fb,
                          drmModeAddFB2WithModifiers(
                              fd.get(), desc.size.width, desc.size.height,
                              static_cast<uint32_t>(desc.format), handles, desc.pitches.data(),
                              desc.offsets.data(), modifiers, &fb_id, DRM_MODE_FB_MODIFIERS),
                          "AddFB2WithModifiers {}", desc.to_string());
    } else {
        // 不传 modifier，内核按驱动默认推断排布。
        // 注意这**不等于** modifier=LINEAR：两条路径在内核里走不同分支。
        ret = MW_DRM_CALL(add_fb,
                          drmModeAddFB2(fd.get(), desc.size.width, desc.size.height,
                                        static_cast<uint32_t>(desc.format), handles,
                                        desc.pitches.data(), desc.offsets.data(), &fb_id, 0),
                          "AddFB2 {}", desc.to_string());
    }

    return ret == 0 ? 0 : errno;
}

/// desc 的静态合法性检查。放在下发之前，因为内核对这些情况只回一个
/// 分辨不出原因的 EINVAL。
Status validate_desc(const FramebufferDesc& desc) {
    if (desc.size.empty()) {
        return Err(Errc::AddFbFailed, "framebuffer size is zero");
    }
    if (desc.num_planes == 0 || desc.num_planes > kMaxFbPlanes) {
        return Err(Errc::AddFbFailed, fmt("num_planes={} out of range", desc.num_planes));
    }
    // UAPI 要求所有平面用同一个 modifier。
    for (uint32_t i = 1; i < desc.num_planes; ++i) {
        if (desc.modifiers[i] != desc.modifiers[0]) {
            return Err(Errc::AddFbFailed,
                       fmt("plane {} modifier {} differs from plane 0 modifier {}; "
                           "the UAPI requires them to be identical",
                           i, to_string(desc.modifiers[i]), to_string(desc.modifiers[0])));
        }
    }
    return Ok();
}

} // namespace

Result<Framebuffer> Framebuffer::add(BorrowedFd fd, const FramebufferDesc& desc) {
    TRY(validate_desc(desc));

    uint32_t fb_id = 0;
    const int err = add_fb_ioctl(fd, desc, fb_id);
    if (err != 0) {
        return Err(Errc::AddFbFailed,
                   fmt("{} failed with {}: {}",
                       desc.uses_modifiers() ? "drmModeAddFB2WithModifiers" : "drmModeAddFB2",
                       errno_name(err), desc.to_string()));
    }

    // 只在拿到 fb 之后计入配平表。add_fb_ioctl 可能因为 modifier 降级发两次
    // ioctl 才成功，所以 ioctl 计数不能用来配平。
    ++stats().fb_acquired;
    LOG_DEBUG("added framebuffer {} <- {}", to_string(FbId{fb_id}), desc.to_string());
    return Ok(Framebuffer(fd, FbId{fb_id}, desc));
}

Result<Framebuffer> Framebuffer::add_with_fallback(BorrowedFd fd, const FramebufferDesc& desc,
                                                   bool* downgraded) {
    if (downgraded != nullptr) {
        *downgraded = false;
    }
    TRY(validate_desc(desc));

    uint32_t fb_id = 0;
    const int err = add_fb_ioctl(fd, desc, fb_id);
    if (err == 0) {
        ++stats().fb_acquired;
        LOG_DEBUG("added framebuffer {} <- {}", to_string(FbId{fb_id}), desc.to_string());
        return Ok(Framebuffer(fd, FbId{fb_id}, desc));
    }

    // 只有"内核/驱动不认这个 modifier"才值得降级。handle 无效、尺寸超限
    // 之类的错误换条路径也一样失败，重试只是浪费一次 ioctl。
    // 注意 Linux 上 ENOTSUP == EOPNOTSUPP，写一个就够。
    const bool worth_retrying = desc.uses_modifiers() && (err == EINVAL || err == ENOTSUP);
    if (! worth_retrying) {
        return Err(Errc::AddFbFailed,
                   fmt("{} failed with {}: {}",
                       desc.uses_modifiers() ? "drmModeAddFB2WithModifiers" : "drmModeAddFB2",
                       errno_name(err), desc.to_string()));
    }

    // **只有线性排布才允许丢掉 modifier。**
    //
    // 丢掉 modifier 意味着让驱动自己推断排布。这块内存如果本来就是线性的，
    // 驱动推断出线性，结果正确；如果它是 tiling 或压缩的，驱动会按线性去读，
    // **屏幕上出来的是垃圾，而且 addfb2 返回成功** —— 一个静默的错误结果，
    // 比一个响亮的失败糟糕得多。
    //
    // 所以非线性 modifier 被拒时不降级，直接失败，让调用方去换一个 modifier
    // 重新分配（见 render/buffer_source.cpp 的重试循环）。
    if (desc.modifiers[0] != kModifierLinear) {
        return Err(Errc::AddFbFailed,
                   fmt("drmModeAddFB2WithModifiers rejected modifier {} with {}; refusing to "
                       "retry without modifier info because this layout is not linear -- the "
                       "driver would misread the memory and put garbage on screen. Allocate "
                       "with a different modifier instead: {}",
                       to_string(desc.modifiers[0]), errno_name(err), desc.to_string()));
    }

    // 到这里 modifier 是 LINEAR。丢掉它让驱动自己推断，绝大多数驱动会推断出
    // 线性，所以结果正确。仍然要 WARN：这是一条假设，不是保证。
    LOG_WARN("AddFB2WithModifiers rejected LINEAR ({}), retrying without modifier info; "
             "the driver will infer the layout and is expected to infer linear",
             errno_name(err));

    FramebufferDesc plain = desc;
    for (auto& modifier : plain.modifiers) {
        modifier = kModifierInvalid;
    }

    const int plain_err = add_fb_ioctl(fd, plain, fb_id);
    if (plain_err != 0) {
        return Err(Errc::AddFbFailed,
                   fmt("drmModeAddFB2 also failed with {} after the modifier downgrade: {}",
                       errno_name(plain_err), plain.to_string()));
    }

    ++stats().fb_acquired;
    LOG_WARN("framebuffer created without modifier info; the driver will infer the layout");
    if (downgraded != nullptr) {
        *downgraded = true;
    }
    return Ok(Framebuffer(fd, FbId{fb_id}, plain));
}

} // namespace mw::drm
