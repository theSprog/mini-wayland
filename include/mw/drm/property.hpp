/**
 * @file drm/property.hpp
 * @brief KMS 属性表缓存与 property blob 的 RAII
 *
 * 两条硬约束在这里落地：
 *
 * 1. **属性 ID 只在 init / modeset 阶段查一次。**
 *    `drmModeObjectGetProperties` 是 ioctl，每帧调用是纯粹的浪费。
 *    PropertyMap 在枚举阶段建好 name → PropertyId 的表；热路径**连
 *    PropertyMap 都不碰**，而是用下面的 `XxxPropIds` 这种"解析好的
 *    结构体"，成员就是 PropertyId，取用是一次结构体字段读。
 *
 * 2. **blob 必须 RAII。**
 *    `drmModeCreatePropertyBlob` 出来的 blob 不销毁会一直挂在内核
 *    DRM 文件私有数据上。modeset 一次泄漏一个，热插拔场景下很快就
 *    能观察到。PropertyBlob 负责这件事。
 */
#pragma once

// 分层约定：`mw/drm/*.hpp` 允许直接引 libdrm 的 C 头，这一层的职责就是包装它。
// 再往上（render / allocator / wayland）不得包含 xf86drm*.h，只能用本层的类型。
#include <xf86drmMode.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {

/// 属性的种类（来自 flags 的 DRM_MODE_PROP_*）
enum class PropKind {
    Range,        ///< 无符号区间
    SignedRange,  ///< 有符号区间
    Enum,
    Bitmask,
    Blob,
    Object,
    Unknown,
};

struct PropertyInfo {
    PropertyId id = kNoProperty;
    PropKind kind = PropKind::Unknown;
    bool immutable = false;
    bool atomic = false;   ///< DRM_MODE_PROP_ATOMIC：只对 atomic client 可见
    uint64_t value = 0;    ///< 枚举那一刻的当前值，仅供诊断，不要当状态用
};

/**
 * @brief 属性**定义**的设备级缓存
 *
 * 这是 KMS 属性模型里很容易忽略的一点：**属性定义是设备全局的，
 * 属性值才是每个对象各有一份。**
 *
 * 驱动在初始化时创建一批 property 对象（`CRTC_ID`、`SRC_W`、`type` ……），
 * 每个有自己的 mode object id；然后把它们**附加**到多个 connector /
 * crtc / plane 上。所以 `drmModeGetProperty(fd, prop_id)` 返回的元数据
 * （名字、类型、flags）和你从哪个对象查到这个 id 完全无关。
 *
 * 不缓存的后果：真机上枚举 16 个对象要 230 次 GetProperty，
 * 而实际不同的属性定义只有几十个。缓存后一个定义只查一次。
 *
 * 只在初始化/枚举期使用；定义在设备生命周期内不变，跨 rescan 也有效。
 */
class PropertyDefCache {
  public:
    struct Definition {
        std::string name{};
        PropKind kind = PropKind::Unknown;
        bool immutable = false;
        bool atomic = false;
    };

    PropertyDefCache() noexcept : defs_() {}

    /// 命中返回缓存，未命中才发 ioctl。失败返回 nullptr（调用方跳过该属性）。
    const Definition* get(BorrowedFd fd, uint32_t property_id);

    size_t size() const noexcept {
        return defs_.size();
    }

    size_t hits() const noexcept {
        return hits_;
    }

    size_t misses() const noexcept {
        return misses_;
    }

    void clear() noexcept;

  private:
    std::vector<std::pair<uint32_t, Definition>> defs_;  ///< 按 property_id 升序
    size_t hits_ = 0;
    size_t misses_ = 0;
};

/**
 * @brief 一个 KMS 对象（connector / crtc / plane）的属性表
 *
 * 内部是按名字排序的扁平数组 + 二分查找，没有哈希表。
 * 单个对象的属性个数是几十量级，扁平数组的 cache 局部性更好，
 * 而且构建期就能排好序，没有 rehash。
 */
class PropertyMap {
  public:
    PropertyMap() noexcept : entries_() {}

    /**
     * @param object_type DRM_MODE_OBJECT_CONNECTOR / _CRTC / _PLANE
     * @param cache       属性定义缓存，由 Device 持有并跨对象复用。
     *                    传缓存而不是内部搞个静态的，是因为 prop_id 只在
     *                    单个 DRM fd 内唯一 —— 多设备时静态缓存会串味。
     */
    static Result<PropertyMap> query(BorrowedFd fd, uint32_t object_id, uint32_t object_type,
                                     PropertyDefCache& cache);

    /// 找不到返回 nullopt，不构造 Error（枚举阶段大量"可选属性存在性检查"走这条）
    std::optional<PropertyInfo> find(std::string_view name) const noexcept;

    bool has(std::string_view name) const noexcept;

    /// 找不到返回 Errc::PropertyNotFound，message 里带对象 id 与属性名
    Result<PropertyId> require(std::string_view name) const;

    uint64_t value_or(std::string_view name, uint64_t fallback) const noexcept;

    size_t size() const noexcept;

    /// 供日志用：遍历全部属性（会拷贝名字，别放热路径）
    std::vector<std::pair<std::string, PropertyInfo>> entries() const;

  private:
    struct Entry {
        std::string name{};
        PropertyInfo info{};
    };
    std::vector<Entry> entries_{};  ///< 按 name 升序
};

// ---------------------------------------------------------------------------
// 热路径用的"解析好的属性 ID 集合"
// ---------------------------------------------------------------------------
// modeset 阶段从 PropertyMap 解析一次，之后每帧直接读字段。
// 必选属性缺失 → 直接报错退出（atomic 驱动必须有）。
// 可选属性缺失 → 存 kNoProperty，运行时分支跳过。

struct ConnectorPropIds {
    PropertyId crtc_id = kNoProperty;      ///< 必选
    PropertyId link_status = kNoProperty;  ///< 可选
    PropertyId non_desktop = kNoProperty;  ///< 可选
    PropertyId max_bpc = kNoProperty;      ///< 可选，勘察到 eDP-1 有
    PropertyId vrr_capable = kNoProperty;  ///< 可选
    PropertyId edid = kNoProperty;         ///< 可选
    PropertyId dpms = kNoProperty;         ///< 可选（legacy 遗留，atomic 下不用）

    static Result<ConnectorPropIds> resolve(const PropertyMap& props);
};

struct CrtcPropIds {
    PropertyId mode_id = kNoProperty;         ///< 必选
    PropertyId active = kNoProperty;          ///< 必选
    PropertyId out_fence_ptr = kNoProperty;   ///< 可选，Step 6 用
    PropertyId vrr_enabled = kNoProperty;     ///< 可选，Step 7 用
    PropertyId gamma_lut = kNoProperty;       ///< 可选
    PropertyId gamma_lut_size = kNoProperty;  ///< 可选

    static Result<CrtcPropIds> resolve(const PropertyMap& props);
};

struct PlanePropIds {
    PropertyId type = kNoProperty;      ///< 必选（immutable enum）
    PropertyId fb_id = kNoProperty;     ///< 必选
    PropertyId crtc_id = kNoProperty;   ///< 必选
    PropertyId src_x = kNoProperty;     ///< 必选，16.16
    PropertyId src_y = kNoProperty;
    PropertyId src_w = kNoProperty;
    PropertyId src_h = kNoProperty;
    PropertyId crtc_x = kNoProperty;    ///< 必选，整数
    PropertyId crtc_y = kNoProperty;
    PropertyId crtc_w = kNoProperty;
    PropertyId crtc_h = kNoProperty;

    PropertyId in_formats = kNoProperty;       ///< 可选（勘察：vsdrm 8 个 plane 全有）
    PropertyId in_fence_fd = kNoProperty;      ///< 可选，Step 6 用
    PropertyId rotation = kNoProperty;         ///< 可选
    PropertyId pixel_blend_mode = kNoProperty; ///< 可选，Step 5 用
    PropertyId alpha = kNoProperty;            ///< 可选
    PropertyId zpos = kNoProperty;             ///< 可选，Step 5 用
    PropertyId color_encoding = kNoProperty;   ///< 可选，YUV overlay 用
    PropertyId color_range = kNoProperty;      ///< 可选

    static Result<PlanePropIds> resolve(const PropertyMap& props);
};

// ---------------------------------------------------------------------------
// Blob
// ---------------------------------------------------------------------------

/**
 * @brief 我们创建、并负责销毁的 property blob（如 MODE_ID）
 *
 * 析构调用 drmModeDestroyPropertyBlob。
 * 注意：blob 被 atomic commit 引用后仍可销毁 —— 内核对已生效的 mode blob
 * 自己持引用。但**在 commit 生效前不能销毁**，所以 modeset 期间的 blob
 * 生命周期要覆盖整个 commit 调用。
 */
class PropertyBlob {
  public:
    PropertyBlob() = default;
    ~PropertyBlob();

    PropertyBlob(PropertyBlob&& other) noexcept;
    PropertyBlob& operator=(PropertyBlob&& other) noexcept;
    PropertyBlob(const PropertyBlob&) = delete;
    PropertyBlob& operator=(const PropertyBlob&) = delete;

    static Result<PropertyBlob> create(BorrowedFd fd, const void* data, size_t length);

    BlobId id() const noexcept {
        return id_;
    }

    bool valid() const noexcept {
        return id_ != kNoBlob;
    }

    /// 提前销毁（析构会再做一次，幂等）
    void reset() noexcept;

  private:
    PropertyBlob(BorrowedFd fd, BlobId id) noexcept : fd_(fd), id_(id) {}

    BorrowedFd fd_{};
    BlobId id_ = kNoBlob;
};

/**
 * @brief 只读 blob 视图（drmModeGetPropertyBlob 的 RAII 包装）
 *
 * 用来读 IN_FORMATS / EDID / 厂商私有的 DC_INFO 之类。
 * data() 指向 libdrm 分配的内存，析构时 drmModeFreePropertyBlob。
 */
class BlobView {
  public:
    BlobView() = default;
    ~BlobView();

    BlobView(BlobView&& other) noexcept;
    BlobView& operator=(BlobView&& other) noexcept;
    BlobView(const BlobView&) = delete;
    BlobView& operator=(const BlobView&) = delete;

    static Result<BlobView> get(BorrowedFd fd, BlobId id);

    const void* data() const noexcept;
    size_t size() const noexcept;

    bool valid() const noexcept {
        return blob_ != nullptr;
    }

  private:
    explicit BlobView(drmModePropertyBlobRes* blob) noexcept : blob_(blob) {}

    drmModePropertyBlobRes* blob_ = nullptr;
};

} // namespace mw::drm
