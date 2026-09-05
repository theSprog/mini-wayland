/**
 * @file drm/atomic.hpp
 * @brief atomic modeset/翻页请求的构建与提交
 *
 * 整个工程只有这一个提交出口。legacy 的 drmModeSetCrtc / drmModePageFlip /
 * drmModeSetPlane 一次都不会出现。
 *
 * ## 两层接口，故意都留着
 *
 * **底层** `add(object_id, prop_id, value)` —— 和
 * `drmModeAtomicAddProperty` 一一对应。保留它是因为 KMS 的属性模型本身
 * 就是"对着一个对象设一个键值对"，把这层完全藏起来会让人以为 atomic 是
 * 什么复杂机制。它其实就是一张 (object, property, value) 三元组的表，
 * 一次性交给内核。
 *
 * **语义层** `set_plane_geometry(plane, SrcRect, CrtcRect)` 之类 ——
 * 负责把 16.16 移位、必选属性齐不齐这些容易错的部分包掉。
 * 日常写代码用这层。
 *
 * ## 请求影子日志
 *
 * AtomicRequest 内部维护一份 (object, prop, value) 的影子数组，
 * 容量在构造时一次性 reserve，之后热路径零分配。它带来三个能力：
 *
 *  1. 提交前 `dump()` 打印完整请求，和 `drm.debug=0x1ff` 的内核日志对照
 *  2. TEST_ONLY 被拒时做 `bisect_rejection()`，二分定位是哪一条属性有问题
 *  3. 统计条数，验证"每帧下发的属性数是稳定的"
 *
 * 第 2 点是排查勘察结果里 vsdrm 那个 EBUSY 的主要手段 ——
 * 内核只给一个 errno，不告诉你是哪个属性/哪个对象出的问题。
 *
 * ## 复用
 *
 * 每帧 new 一个 drmModeAtomicReq 是浪费。`reset()` 用
 * `drmModeAtomicSetCursor(req, 0)` 把游标退回开头，内部缓冲区复用，
 * 这样一整个帧循环只有启动时那一次分配。
 */
#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mw/internal/error.hpp"
#include "mw/internal/span.hpp"
#include "mw/internal/unique_fd.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {
using internal::BorrowedFd;
using internal::Status;
using internal::span;

struct Connector;
struct Crtc;
struct Plane;

/// 对应 DRM_MODE_ATOMIC_* / DRM_MODE_PAGE_FLIP_*
enum class CommitFlags : uint32_t {
    None = 0,
    TestOnly = 0x0100u,      ///< DRM_MODE_ATOMIC_TEST_ONLY
    Nonblock = 0x0200u,      ///< DRM_MODE_ATOMIC_NONBLOCK
    AllowModeset = 0x0400u,  ///< DRM_MODE_ATOMIC_ALLOW_MODESET
    PageFlipEvent = 0x01u,   ///< DRM_MODE_PAGE_FLIP_EVENT
};

constexpr CommitFlags operator|(CommitFlags a, CommitFlags b) noexcept {
    return static_cast<CommitFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

constexpr bool has_flag(CommitFlags set, CommitFlags f) noexcept {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(f)) != 0u;
}

std::string to_string(CommitFlags flags);

/// 影子日志的一条
struct AtomicEntry {
    uint32_t object_id = 0;
    PropertyId property = kNoProperty;
    uint64_t value = 0;

    /// 属性名。指向 PropertyMap 里的静态存储，**不拥有**。
    /// 只在 dump 时用；为 nullptr 时打 property id。
    const char* name = nullptr;
};

class AtomicRequest {
  public:
    /// @param reserve 影子数组预留条数。一条完整 modeset 大约 20~30 条，
    ///                多 plane 场景每个 plane 再加 10 条。默认 64 足够 Step 1~2。
    explicit AtomicRequest(BorrowedFd fd, size_t reserve = 64);
    ~AtomicRequest();

    AtomicRequest(AtomicRequest&& other) noexcept;
    AtomicRequest& operator=(AtomicRequest&& other) noexcept;
    AtomicRequest(const AtomicRequest&) = delete;
    AtomicRequest& operator=(const AtomicRequest&) = delete;

    /// 清空但保留底层缓冲。每帧开头调，不产生分配。
    void reset() noexcept;

    // ---- 底层：直接对应 drmModeAtomicAddProperty ----

    Status add(uint32_t object_id, PropertyId prop, uint64_t value,
               const char* debug_name = nullptr);

    Status add(ConnectorId obj, PropertyId prop, uint64_t value,
               const char* debug_name = nullptr);
    Status add(CrtcId obj, PropertyId prop, uint64_t value,
               const char* debug_name = nullptr);
    Status add(PlaneId obj, PropertyId prop, uint64_t value,
               const char* debug_name = nullptr);

    // ---- 语义层 ----

    /**
     * @brief connector 挂到 CRTC 上（或传 kNoCrtc 解绑）
     *
     * 只设 CRTC_ID 一个属性。mode 是 CRTC 的属性，不是 connector 的 ——
     * 这一点和 legacy 的 drmModeSetCrtc 直觉相反，值得单独记一笔。
     */
    Status bind_connector(const Connector& conn, CrtcId crtc);

    /**
     * @brief 启用 CRTC 并设置 mode
     *
     * @param mode_blob  MODE_ID blob。**调用方必须保证它在 commit 返回前存活**，
     *                   内核在 commit 里才会取内容。
     *
     * 带 mode 变更的提交必须配 CommitFlags::AllowModeset，否则内核返回 EINVAL。
     */
    Status set_crtc_mode(const Crtc& crtc, BlobId mode_blob, bool active);

    /// 关闭 CRTC：ACTIVE=0、MODE_ID=0
    Status disable_crtc(const Crtc& crtc);

    /**
     * @brief 设置 plane 的几何
     *
     * 这是 16.16 的唯一落点：`src` 的字段已经是 Fixed16，
     * 内部直接取 raw()，不再有任何移位运算。
     * `crtc_rect` 是普通整数。两者类型不同，传反了编译不过。
     */
    Status set_plane_geometry(const Plane& plane, const SrcRect& src, const CrtcRect& crtc_rect);

    /// 把 fb 绑到 plane 上并指定输出 CRTC
    Status set_plane_fb(const Plane& plane, FbId fb, CrtcId crtc);

    /**
     * @brief 一次设完 plane 的全部必选属性
     *
     * 等价于 set_plane_fb + set_plane_geometry。分开的版本留着是因为
     * Step 5 的分配器会先试几何、失败再换 plane，不需要每次重设 fb。
     */
    Status set_plane(const Plane& plane, FbId fb, CrtcId crtc, const SrcRect& src,
                     const CrtcRect& crtc_rect);

    /// 关闭 plane：FB_ID=0、CRTC_ID=0。退出清理时每个 plane 都要做。
    Status disable_plane(const Plane& plane);

    // ---- 提交 ----

    /**
     * @brief TEST_ONLY 试探
     *
     * 内核只做校验不真的生效。这是 Step 5 分配器的核心动作，
     * 也是启动时验证配置合法性的第一道关。
     *
     * 失败是**预期内的控制流**，所以：
     *  - 不构造 Error（会拼两个 std::string），直接返回 errno
     *  - 返回 0 表示通过
     *
     * @param extra 额外 flag。TEST_ONLY 会自动加上；
     *              需要 modeset 的配置这里也要传 AllowModeset，
     *              否则内核会以为你想在不允许 modeset 的情况下改 mode。
     */
    [[nodiscard]] int test(CommitFlags extra = CommitFlags::None) noexcept;

    /**
     * @brief 真正提交
     *
     * @param user_data 传给内核的 user_data，会原样出现在 page flip 事件里。
     *                  配 CommitFlags::PageFlipEvent 使用。
     *
     * 与 test() 不同，这里返回 Error —— 提交失败通常意味着要报错退出
     * 或者走一条重量级的恢复路径，值得付出构造 Error 的代价。
     */
    Status commit(CommitFlags flags, uint64_t user_data = 0);

    // ---- 诊断 ----

    size_t entry_count() const noexcept {
        return entries_.size();
    }

    span<const AtomicEntry> entries() const noexcept;

    /// 打印完整请求，按对象分组。和 drm.debug=0x1ff 的内核日志对照用。
    void dump(const char* label = nullptr) const;

    /**
     * @brief TEST_ONLY 失败后，二分定位是哪一条属性导致的
     *
     * 做法：反复用属性子集重新构造 TEST_ONLY 请求，缩小到最小的
     * "去掉它就能过"的集合。注意结果只是启发性的 —— KMS 的约束是
     * 整体性的（带宽、平面层叠），单条属性未必是真凶。但对
     * "某个属性值超出硬件范围"这类问题非常有效。
     *
     * 代价：O(log n) 次额外 ioctl，且会分配临时请求。
     * **只在出错路径调用**，不要放进正常流程。
     *
     * @return 可疑条目在 entries() 中的下标；无法定位时返回 nullopt
     */
    std::optional<size_t> bisect_rejection(CommitFlags extra = CommitFlags::None);

  private:
    BorrowedFd fd_{};
    drmModeAtomicReq* req_ = nullptr;
    std::vector<AtomicEntry> entries_{};
};

} // namespace mw::drm
