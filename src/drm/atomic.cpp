#include "mw/drm/atomic.hpp"

#include <xf86drm.h>

#include <cerrno>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/trace.hpp"

namespace mw::drm {
namespace {

/// 这些常量必须和 UAPI 一致。头文件里为了不暴露 libdrm 宏而写成了字面量，
/// 在这里静态断言一次，改错了编译期就炸。
static_assert(static_cast<uint32_t>(CommitFlags::TestOnly) == DRM_MODE_ATOMIC_TEST_ONLY,
              "CommitFlags::TestOnly must match DRM_MODE_ATOMIC_TEST_ONLY");
static_assert(static_cast<uint32_t>(CommitFlags::Nonblock) == DRM_MODE_ATOMIC_NONBLOCK,
              "CommitFlags::Nonblock must match DRM_MODE_ATOMIC_NONBLOCK");
static_assert(static_cast<uint32_t>(CommitFlags::AllowModeset) == DRM_MODE_ATOMIC_ALLOW_MODESET,
              "CommitFlags::AllowModeset must match DRM_MODE_ATOMIC_ALLOW_MODESET");
static_assert(static_cast<uint32_t>(CommitFlags::PageFlipEvent) == DRM_MODE_PAGE_FLIP_EVENT,
              "CommitFlags::PageFlipEvent must match DRM_MODE_PAGE_FLIP_EVENT");

} // namespace

std::string to_string(CommitFlags flags) {
    std::string out;
    const auto add = [&out](const char* name) {
        if (! out.empty()) {
            out += '|';
        }
        out += name;
    };
    if (has_flag(flags, CommitFlags::TestOnly)) add("TEST_ONLY");
    if (has_flag(flags, CommitFlags::Nonblock)) add("NONBLOCK");
    if (has_flag(flags, CommitFlags::AllowModeset)) add("ALLOW_MODESET");
    if (has_flag(flags, CommitFlags::PageFlipEvent)) add("PAGE_FLIP_EVENT");
    return out.empty() ? "none" : out;
}

// ---------------------------------------------------------------------------

AtomicRequest::AtomicRequest(BorrowedFd fd, size_t reserve)
    : fd_(fd), req_(drmModeAtomicAlloc()), entries_() {
    // 一次性预留，之后 reset() 只把 size 归零、不释放容量。
    // 这是"热路径零堆分配"在这一层的落点。
    entries_.reserve(reserve);
    if (req_ == nullptr) {
        LOG_ERROR("drmModeAtomicAlloc returned null (out of memory)");
    }
}

AtomicRequest::~AtomicRequest() {
    if (req_ != nullptr) {
        drmModeAtomicFree(req_);
        req_ = nullptr;
    }
}

AtomicRequest::AtomicRequest(AtomicRequest&& other) noexcept
    : fd_(other.fd_), req_(std::exchange(other.req_, nullptr)), entries_(std::move(other.entries_)) {}

AtomicRequest& AtomicRequest::operator=(AtomicRequest&& other) noexcept {
    if (this != &other) {
        if (req_ != nullptr) {
            drmModeAtomicFree(req_);
        }
        fd_ = other.fd_;
        req_ = std::exchange(other.req_, nullptr);
        entries_ = std::move(other.entries_);
    }
    return *this;
}

void AtomicRequest::reset() noexcept {
    if (req_ != nullptr) {
        // 把游标退回开头。libdrm 内部的数组容量保留，不重新分配。
        drmModeAtomicSetCursor(req_, 0);
    }
    entries_.clear();  // 只归零 size，容量还在
}

span<const AtomicEntry> AtomicRequest::entries() const noexcept {
    return span<const AtomicEntry>(entries_.data(), entries_.size());
}

// ---------------------------------------------------------------------------
// 底层
// ---------------------------------------------------------------------------

Status AtomicRequest::add(uint32_t object_id, PropertyId prop, uint64_t value,
                          const char* debug_name) {
    if (req_ == nullptr) {
        return Err(Errc::Internal, "atomic request was not allocated");
    }
    if (prop == kNoProperty) {
        // 属性 id 为 0 说明上游解析时该属性不存在却仍被使用了。
        // 直接下发内核只会回一个没头没尾的 EINVAL，这里提前拦住。
        return Err(Errc::PropertyNotFound,
                   fmt("attempt to set unresolved property '{}' on object {}",
                       debug_name != nullptr ? debug_name : "?", object_id));
    }

    const int ret = drmModeAtomicAddProperty(req_, object_id, static_cast<uint32_t>(prop), value);
    if (ret < 0) {
        return Err(Errc::Internal,
                   fmt("drmModeAtomicAddProperty(obj={} prop={} value={}) returned {}", object_id,
                       static_cast<uint32_t>(prop), value, ret));
    }

    entries_.push_back(AtomicEntry{object_id, prop, value, debug_name});
    return Ok();
}

Status AtomicRequest::add(ConnectorId obj, PropertyId prop, uint64_t value,
                          const char* debug_name) {
    return add(raw(obj), prop, value, debug_name);
}

Status AtomicRequest::add(CrtcId obj, PropertyId prop, uint64_t value, const char* debug_name) {
    return add(raw(obj), prop, value, debug_name);
}

Status AtomicRequest::add(PlaneId obj, PropertyId prop, uint64_t value, const char* debug_name) {
    return add(raw(obj), prop, value, debug_name);
}

// ---------------------------------------------------------------------------
// 语义层
// ---------------------------------------------------------------------------

Status AtomicRequest::bind_connector(const Connector& conn, CrtcId crtc) {
    // connector 上只有 CRTC_ID 一个属性要设。mode 是 **CRTC** 的属性，
    // 不是 connector 的 —— 这一点和 legacy 的 drmModeSetCrtc(fd, crtc, fb,
    // x, y, connectors, count, mode) 的直觉正好相反。
    return add(conn.id, conn.prop_ids.crtc_id, raw(crtc), "CRTC_ID");
}

Status AtomicRequest::set_crtc_mode(const Crtc& crtc, BlobId mode_blob, bool active) {
    TRY(add(crtc.id, crtc.prop_ids.mode_id, raw(mode_blob), "MODE_ID"));
    TRY(add(crtc.id, crtc.prop_ids.active, active ? 1u : 0u, "ACTIVE"));
    return Ok();
}

Status AtomicRequest::disable_crtc(const Crtc& crtc) {
    TRY(add(crtc.id, crtc.prop_ids.active, 0u, "ACTIVE"));
    TRY(add(crtc.id, crtc.prop_ids.mode_id, 0u, "MODE_ID"));
    return Ok();
}

Status AtomicRequest::set_plane_geometry(const Plane& plane, const SrcRect& src,
                                         const CrtcRect& crtc_rect) {
    // **16.16 的唯一落点。** src 的字段已经是 Fixed16，这里只取 raw()，
    // 全工程没有任何一处手写的 << 16。
    TRY(add(plane.id, plane.prop_ids.src_x, src.x.raw(), "SRC_X"));
    TRY(add(plane.id, plane.prop_ids.src_y, src.y.raw(), "SRC_Y"));
    TRY(add(plane.id, plane.prop_ids.src_w, src.width.raw(), "SRC_W"));
    TRY(add(plane.id, plane.prop_ids.src_h, src.height.raw(), "SRC_H"));

    // CRTC_X/Y 是有符号的（plane 可以部分移出屏幕），要走 int32 -> uint64
    // 的位模式转换，不能直接 static_cast<uint64_t>(-1) 那样符号扩展成
    // 0xffff'ffff'ffff'ffff —— 内核按 32 位有符号读。
    TRY(add(plane.id, plane.prop_ids.crtc_x, static_cast<uint32_t>(crtc_rect.x), "CRTC_X"));
    TRY(add(plane.id, plane.prop_ids.crtc_y, static_cast<uint32_t>(crtc_rect.y), "CRTC_Y"));
    TRY(add(plane.id, plane.prop_ids.crtc_w, crtc_rect.width, "CRTC_W"));
    TRY(add(plane.id, plane.prop_ids.crtc_h, crtc_rect.height, "CRTC_H"));
    return Ok();
}

Status AtomicRequest::set_plane_fb(const Plane& plane, FbId fb, CrtcId crtc) {
    TRY(add(plane.id, plane.prop_ids.fb_id, raw(fb), "FB_ID"));
    TRY(add(plane.id, plane.prop_ids.crtc_id, raw(crtc), "CRTC_ID"));
    return Ok();
}

Status AtomicRequest::set_plane(const Plane& plane, FbId fb, CrtcId crtc, const SrcRect& src,
                                const CrtcRect& crtc_rect) {
    TRY(set_plane_fb(plane, fb, crtc));
    TRY(set_plane_geometry(plane, src, crtc_rect));
    return Ok();
}

Status AtomicRequest::disable_plane(const Plane& plane) {
    // FB_ID 和 CRTC_ID 都置 0 才算真正解绑。只置一个内核会拒绝
    // （"plane 有 fb 但没 crtc"或反之都是非法状态）。
    TRY(add(plane.id, plane.prop_ids.fb_id, 0u, "FB_ID"));
    TRY(add(plane.id, plane.prop_ids.crtc_id, 0u, "CRTC_ID"));
    return Ok();
}

// ---------------------------------------------------------------------------
// 提交
// ---------------------------------------------------------------------------

int AtomicRequest::test(CommitFlags extra) noexcept {
    if (req_ == nullptr) {
        return EINVAL;
    }
    const uint32_t flags = static_cast<uint32_t>(extra | CommitFlags::TestOnly);

    // 注意这里返回 int errno 而不是 Status：TEST_ONLY 失败是**预期内的
    // 控制流**（Step 5 的 plane 分配器每帧都会撞几次），构造 Error 要拼
    // 两个 std::string，不该出现在那条路径上。
    const int ret = MW_DRM_CALL(atomic_test,
                                drmModeAtomicCommit(fd_.get(), req_, flags, nullptr),
                                "flags={} props={}", to_string(extra | CommitFlags::TestOnly),
                                entries_.size());
    if (ret == 0) {
        return 0;
    }
    const int err = errno;
    record_atomic_test_failure(err);
    return err;
}

Status AtomicRequest::commit(CommitFlags flags, uint64_t user_data) {
    if (req_ == nullptr) {
        return Err(Errc::Internal, "atomic request was not allocated");
    }
    if (has_flag(flags, CommitFlags::TestOnly)) {
        return Err(Errc::Internal, "use test() for TEST_ONLY, not commit()");
    }

    // user_data 是原样回传的：内核不解释它，page flip 事件里会带回来。
    // 我们用它标识"这是第几帧"，收到事件时就知道哪个 buffer 可以复用了。
    void* const opaque = reinterpret_cast<void*>(static_cast<uintptr_t>(user_data));

    const int ret = MW_DRM_CALL(
        atomic_commit,
        drmModeAtomicCommit(fd_.get(), req_, static_cast<uint32_t>(flags), opaque),
        "flags={} props={} user_data={}", to_string(flags), entries_.size(), user_data);
    if (ret != 0) {
        const int err = errno;
        record_atomic_commit_failure(err);
        return Err(Errc::AtomicCommitFailed,
                   fmt("atomic commit failed with {} (flags={}, {} properties)", errno_name(err),
                       to_string(flags), entries_.size()));
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// 诊断
// ---------------------------------------------------------------------------

void AtomicRequest::dump(const char* label) const {
    LOG_INFO("atomic request{}{}: {} properties", label != nullptr ? " " : "",
             label != nullptr ? label : "", entries_.size());
    LOG_SCOPE();

    // 按对象分组打印。KMS 的请求本来就是"一张 (object, property, value)
    // 三元组的表"，分组只是为了人读起来方便；下发给内核的就是这张表。
    uint32_t current_object = 0;
    bool first = true;
    for (const auto& entry : entries_) {
        if (first || entry.object_id != current_object) {
            current_object = entry.object_id;
            first = false;
            LOG_INFO("object {}:", current_object);
        }
        LOG_SCOPE();
        if (entry.name != nullptr) {
            LOG_INFO("{:<10} (prop {}) = {} (0x{:x})", entry.name,
                     static_cast<uint32_t>(entry.property), entry.value, entry.value);
        } else {
            LOG_INFO("prop {} = {} (0x{:x})", static_cast<uint32_t>(entry.property), entry.value,
                     entry.value);
        }
    }
}

std::optional<size_t> AtomicRequest::bisect_rejection(CommitFlags extra) {
    // 内核只回一个 errno，不告诉你是哪个对象、哪个属性出的问题。
    // 这里用属性子集反复 TEST_ONLY，缩小到"去掉它就能过"的那一条。
    //
    // 局限性要说清楚：KMS 的约束是**整体性**的（显存带宽、平面层叠、
    // 缩放比例上限），单条属性未必是真凶。这个结果是启发性的线索，
    // 不是判决书。但对"某个属性值超出硬件范围"这类问题非常有效。
    //
    // 代价：O(n) 次额外 ioctl 和临时请求分配。**只在出错路径调用。**

    if (entries_.empty()) {
        return std::nullopt;
    }

    const int baseline = test(extra);
    if (baseline == 0) {
        LOG_INFO("bisect: the full request passes TEST_ONLY, nothing to bisect");
        return std::nullopt;
    }
    LOG_INFO("bisect: full request fails with {}, narrowing down ({} properties)",
             errno_name(baseline), entries_.size());
    LOG_SCOPE();

    // 保存一份，因为下面要反复重建 req_
    const std::vector<AtomicEntry> all = entries_;

    // 用一个临时请求重放子集
    const auto replay = [&](size_t skip_index) -> int {
        AtomicRequest probe(fd_, all.size());
        for (size_t i = 0; i < all.size(); ++i) {
            if (i == skip_index) {
                continue;
            }
            const auto status =
                probe.add(all[i].object_id, all[i].property, all[i].value, all[i].name);
            if (! status) {
                return -1;
            }
        }
        return probe.test(extra);
    };

    // 逐条剔除。n 通常是几十，一次 TEST_ONLY 是微秒级，可以接受。
    // 真正的二分在这里没有意义 —— 我们要找的是"去掉它就能过"的单点，
    // 而不是一个有序序列里的分界。
    for (size_t i = 0; i < all.size(); ++i) {
        const int result = replay(i);
        if (result == 0) {
            const AtomicEntry& suspect = all[i];
            LOG_WARN("bisect: removing object {} property {} ({}) = {} makes the request pass",
                     suspect.object_id, static_cast<uint32_t>(suspect.property),
                     suspect.name != nullptr ? suspect.name : "?", suspect.value);
            LOG_WARN("  note: KMS constraints are global (bandwidth, plane stacking, scaling);");
            LOG_WARN("  this is a strong hint, not proof that the property itself is invalid");
            return i;
        }
    }

    LOG_WARN("bisect: no single property removal makes the request pass");
    LOG_WARN("  the rejection is probably a global constraint rather than one bad value");
    LOG_WARN("  (bandwidth limits, unsupported plane combination, or a driver-side state issue)");
    return std::nullopt;
}

} // namespace mw::drm
