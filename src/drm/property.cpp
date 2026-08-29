#include "mw/drm/property.hpp"

#include <xf86drm.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/trace.hpp"

namespace mw::drm {
namespace {

/// DRM_MODE_PROP_* 的 flags 位 -> 我们的 PropKind
///
/// 注意 legacy 的类型位（RANGE/ENUM/BLOB/BITMASK）和扩展类型位
/// （OBJECT/SIGNED_RANGE）用的是**不同的掩码**。这是 UAPI 的历史包袱：
/// 老的四种挤在 flags 的低位里，后加的两种用 DRM_MODE_PROP_EXTENDED_TYPE。
/// 用一个掩码去判所有类型会把 OBJECT 误判成 RANGE。
PropKind classify(uint32_t flags) noexcept {
    const uint32_t extended = flags & DRM_MODE_PROP_EXTENDED_TYPE;
    if (extended != 0u) {
        switch (extended) {
            case DRM_MODE_PROP_OBJECT:
                return PropKind::Object;
            case DRM_MODE_PROP_SIGNED_RANGE:
                return PropKind::SignedRange;
            default:
                return PropKind::Unknown;
        }
    }

    const uint32_t legacy = flags & DRM_MODE_PROP_LEGACY_TYPE;
    switch (legacy) {
        case DRM_MODE_PROP_RANGE:
            return PropKind::Range;
        case DRM_MODE_PROP_ENUM:
            return PropKind::Enum;
        case DRM_MODE_PROP_BLOB:
            return PropKind::Blob;
        case DRM_MODE_PROP_BITMASK:
            return PropKind::Bitmask;
        default:
            return PropKind::Unknown;
    }
}

const char* object_type_name(uint32_t object_type) noexcept {
    switch (object_type) {
        case DRM_MODE_OBJECT_CONNECTOR: return "connector";
        case DRM_MODE_OBJECT_CRTC:      return "crtc";
        case DRM_MODE_OBJECT_PLANE:     return "plane";
        case DRM_MODE_OBJECT_ENCODER:   return "encoder";
        case DRM_MODE_OBJECT_MODE:      return "mode";
        case DRM_MODE_OBJECT_FB:        return "fb";
        case DRM_MODE_OBJECT_BLOB:      return "blob";
        default:                        return "object";
    }
}

const char* kind_name(PropKind kind) noexcept {
    switch (kind) {
        case PropKind::Range:       return "range";
        case PropKind::SignedRange: return "srange";
        case PropKind::Enum:        return "enum";
        case PropKind::Bitmask:     return "bitmask";
        case PropKind::Blob:        return "blob";
        case PropKind::Object:      return "object";
        case PropKind::Unknown:     return "unknown";
    }
    return "unknown";
}

/// libdrm 的 drmModeObjectProperties 释放器
struct ObjectPropsDeleter {
    void operator()(drmModeObjectProperties* p) const noexcept {
        drmModeFreeObjectProperties(p);
    }
};
using UniqueObjectProps = std::unique_ptr<drmModeObjectProperties, ObjectPropsDeleter>;

struct PropertyDeleter {
    void operator()(drmModePropertyRes* p) const noexcept {
        drmModeFreeProperty(p);
    }
};
using UniqueProperty = std::unique_ptr<drmModePropertyRes, PropertyDeleter>;

} // namespace

// ---------------------------------------------------------------------------
// PropertyDefCache
// ---------------------------------------------------------------------------

const PropertyDefCache::Definition* PropertyDefCache::get(BorrowedFd fd, uint32_t property_id) {
    const auto it = std::lower_bound(
        defs_.begin(), defs_.end(), property_id,
        [](const std::pair<uint32_t, Definition>& entry, uint32_t target) {
            return entry.first < target;
        });
    if (it != defs_.end() && it->first == property_id) {
        ++hits_;
        return &it->second;
    }

    ++misses_;
    auto* prop_raw = MW_DRM_CALL_PTR(get_property, drmModeGetProperty(fd.get(), property_id),
                                     "prop id={}", property_id);
    if (prop_raw == nullptr) {
        return nullptr;
    }
    const UniqueProperty prop(prop_raw);

    Definition def;
    // prop->name 是定长 char[DRM_PROP_NAME_LEN]，不保证以 NUL 结尾。
    // 用 strnlen 限长，否则驱动给了个满长度的名字就会读越界。
    def.name.assign(prop->name, ::strnlen(prop->name, sizeof(prop->name)));
    def.kind = classify(prop->flags);
    def.immutable = (prop->flags & DRM_MODE_PROP_IMMUTABLE) != 0u;
    def.atomic = (prop->flags & DRM_MODE_PROP_ATOMIC) != 0u;

    const auto inserted = defs_.insert(it, {property_id, std::move(def)});
    return &inserted->second;
}

void PropertyDefCache::clear() noexcept {
    defs_.clear();
    hits_ = 0;
    misses_ = 0;
}

// ---------------------------------------------------------------------------
// PropertyMap
// ---------------------------------------------------------------------------

Result<PropertyMap> PropertyMap::query(BorrowedFd fd, uint32_t object_id, uint32_t object_type,
                                       PropertyDefCache& cache) {
    const char* type_name = object_type_name(object_type);

    auto* raw = MW_DRM_CALL_PTR(get_properties,
                                drmModeObjectGetProperties(fd.get(), object_id, object_type),
                                "{} id={}", type_name, object_id);
    if (raw == nullptr) {
        return Err(Errc::ResourceQueryFailed,
                   fmt("drmModeObjectGetProperties({} {})", type_name, object_id));
    }
    const UniqueObjectProps props(raw);

    PropertyMap map;
    map.entries_.reserve(props->count_props);

    LOG_DEBUG("querying {} properties on {} id={}", props->count_props, type_name, object_id);
    LOG_SCOPE();

    for (uint32_t i = 0; i < props->count_props; ++i) {
        const uint32_t prop_id = props->props[i];
        const uint64_t value = props->prop_values[i];

        const PropertyDefCache::Definition* def = cache.get(fd, prop_id);
        if (def == nullptr) {
            // 单个属性查不到不致命：驱动可能在我们枚举的间隙拔掉了对象。
            // 记一笔继续，后面 require() 会把真正需要的属性缺失暴露出来。
            LOG_WARN("drmModeGetProperty({}) failed on {} id={}, skipping", prop_id, type_name,
                     object_id);
            continue;
        }

        Entry entry;
        entry.name = def->name;
        entry.info.id = PropertyId{prop_id};
        entry.info.kind = def->kind;
        entry.info.immutable = def->immutable;
        entry.info.atomic = def->atomic;
        entry.info.value = value;

        LOG_TRACE("  {:>24} id={} kind={} value={} {}{}", entry.name, prop_id,
                  kind_name(entry.info.kind), value, entry.info.immutable ? "immutable " : "",
                  entry.info.atomic ? "atomic" : "");

        map.entries_.push_back(std::move(entry));
    }

    // 排序一次，之后所有查询都是二分。属性个数是几十量级，
    // 扁平数组的 cache 局部性比哈希表好，而且没有 rehash。
    std::sort(map.entries_.begin(), map.entries_.end(),
              [](const Entry& a, const Entry& b) { return a.name < b.name; });

    return Ok(std::move(map));
}

std::optional<PropertyInfo> PropertyMap::find(std::string_view name) const noexcept {
    const auto it = std::lower_bound(
        entries_.begin(), entries_.end(), name,
        [](const Entry& entry, std::string_view target) { return entry.name < target; });
    if (it == entries_.end() || it->name != name) {
        return std::nullopt;
    }
    return it->info;
}

bool PropertyMap::has(std::string_view name) const noexcept {
    return find(name).has_value();
}

Result<PropertyId> PropertyMap::require(std::string_view name) const {
    const auto info = find(name);
    if (! info) {
        return Err(Errc::PropertyNotFound, fmt("property '{}' not present on this object", name));
    }
    return Ok(info->id);
}

uint64_t PropertyMap::value_or(std::string_view name, uint64_t fallback) const noexcept {
    const auto info = find(name);
    return info ? info->value : fallback;
}

size_t PropertyMap::size() const noexcept {
    return entries_.size();
}

std::vector<std::pair<std::string, PropertyInfo>> PropertyMap::entries() const {
    std::vector<std::pair<std::string, PropertyInfo>> out;
    out.reserve(entries_.size());
    for (const auto& entry : entries_) {
        out.emplace_back(entry.name, entry.info);
    }
    return out;
}

// ---------------------------------------------------------------------------
// XxxPropIds
// ---------------------------------------------------------------------------
// 必选属性缺失 -> 直接失败。atomic 驱动必须注册这些，缺了说明驱动没实现
// atomic modeset，继续往下走只会在 commit 时得到一个没头没尾的 EINVAL。
//
// 可选属性缺失 -> 留 kNoProperty，运行时分支跳过（这就是"运行时 caps 探测"
// 落到属性粒度上的样子，不用 #ifdef）。

namespace {

/// 可选属性：找不到就留 kNoProperty，并 LOG_DEBUG 说明哪个功能会退化
void resolve_optional(const PropertyMap& props, std::string_view name, PropertyId& out,
                      const char* consequence) {
    const auto info = props.find(name);
    if (info) {
        out = info->id;
    } else {
        out = kNoProperty;
        LOG_DEBUG("optional property '{}' absent: {}", name, consequence);
    }
}

} // namespace

Result<ConnectorPropIds> ConnectorPropIds::resolve(const PropertyMap& props) {
    ConnectorPropIds ids;
    ids.crtc_id = TRY(props.require("CRTC_ID"));

    resolve_optional(props, "link-status", ids.link_status, "link retraining not observable");
    resolve_optional(props, "non-desktop", ids.non_desktop, "cannot detect VR headsets");
    resolve_optional(props, "max bpc", ids.max_bpc, "bit depth not selectable");
    resolve_optional(props, "vrr_capable", ids.vrr_capable, "VRR capability unknown");
    resolve_optional(props, "EDID", ids.edid, "no EDID parsing");
    resolve_optional(props, "DPMS", ids.dpms, "legacy DPMS unavailable (fine, atomic uses ACTIVE)");
    return Ok(ids);
}

Result<CrtcPropIds> CrtcPropIds::resolve(const PropertyMap& props) {
    CrtcPropIds ids;
    ids.mode_id = TRY(props.require("MODE_ID"));
    ids.active = TRY(props.require("ACTIVE"));

    resolve_optional(props, "OUT_FENCE_PTR", ids.out_fence_ptr,
                     "explicit sync out-fence unavailable (Step 6 will degrade)");
    resolve_optional(props, "VRR_ENABLED", ids.vrr_enabled, "variable refresh rate unavailable");
    resolve_optional(props, "GAMMA_LUT", ids.gamma_lut, "no gamma ramp");
    resolve_optional(props, "GAMMA_LUT_SIZE", ids.gamma_lut_size, "gamma LUT size unknown");
    return Ok(ids);
}

Result<PlanePropIds> PlanePropIds::resolve(const PropertyMap& props) {
    PlanePropIds ids;
    ids.type = TRY(props.require("type"));
    ids.fb_id = TRY(props.require("FB_ID"));
    ids.crtc_id = TRY(props.require("CRTC_ID"));
    ids.src_x = TRY(props.require("SRC_X"));
    ids.src_y = TRY(props.require("SRC_Y"));
    ids.src_w = TRY(props.require("SRC_W"));
    ids.src_h = TRY(props.require("SRC_H"));
    ids.crtc_x = TRY(props.require("CRTC_X"));
    ids.crtc_y = TRY(props.require("CRTC_Y"));
    ids.crtc_w = TRY(props.require("CRTC_W"));
    ids.crtc_h = TRY(props.require("CRTC_H"));

    resolve_optional(props, "IN_FORMATS", ids.in_formats,
                     "no modifier negotiation; falling back to plain format list");
    resolve_optional(props, "IN_FENCE_FD", ids.in_fence_fd,
                     "explicit sync in-fence unavailable (Step 6 will degrade)");
    resolve_optional(props, "rotation", ids.rotation, "no hardware rotation");
    resolve_optional(props, "pixel blend mode", ids.pixel_blend_mode,
                     "no per-plane blend mode control");
    resolve_optional(props, "alpha", ids.alpha, "no per-plane alpha");
    resolve_optional(props, "zpos", ids.zpos, "plane stacking order is fixed");
    resolve_optional(props, "COLOR_ENCODING", ids.color_encoding, "YUV encoding not selectable");
    resolve_optional(props, "COLOR_RANGE", ids.color_range, "YUV range not selectable");
    return Ok(ids);
}

// ---------------------------------------------------------------------------
// PropertyBlob
// ---------------------------------------------------------------------------

PropertyBlob::~PropertyBlob() {
    reset();
}

PropertyBlob::PropertyBlob(PropertyBlob&& other) noexcept
    : fd_(other.fd_), id_(std::exchange(other.id_, kNoBlob)) {}

PropertyBlob& PropertyBlob::operator=(PropertyBlob&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.fd_;
        id_ = std::exchange(other.id_, kNoBlob);
    }
    return *this;
}

Result<PropertyBlob> PropertyBlob::create(BorrowedFd fd, const void* data, size_t length) {
    uint32_t blob_id = 0;
    const int ret = MW_DRM_CALL(create_blob,
                                drmModeCreatePropertyBlob(fd.get(), data, length, &blob_id),
                                "length={}", length);
    if (ret != 0) {
        return Err(Errc::BlobCreateFailed, fmt("drmModeCreatePropertyBlob(length={})", length));
    }
    LOG_DEBUG("created property blob id={} length={}", blob_id, length);
    return Ok(PropertyBlob(fd, BlobId{blob_id}));
}

void PropertyBlob::reset() noexcept {
    if (id_ == kNoBlob) {
        return;
    }
    const uint32_t id = static_cast<uint32_t>(id_);
    // 内核对已生效的 mode blob 自己持引用，所以 commit 之后销毁是安全的。
    // 但**不能在 commit 返回之前**销毁 —— 内核是在 commit 里才取内容的。
    const int ret = MW_DRM_CALL(destroy_blob, drmModeDestroyPropertyBlob(fd_.get(), id),
                                "blob id={}", id);
    if (ret != 0) {
        LOG_WARN("drmModeDestroyPropertyBlob({}) failed; kernel blob leaked", id);
    } else {
        LOG_TRACE("destroyed property blob id={}", id);
    }
    id_ = kNoBlob;
}

// ---------------------------------------------------------------------------
// BlobView
// ---------------------------------------------------------------------------

BlobView::~BlobView() {
    if (blob_ != nullptr) {
        drmModeFreePropertyBlob(blob_);
        blob_ = nullptr;
    }
}

BlobView::BlobView(BlobView&& other) noexcept : blob_(std::exchange(other.blob_, nullptr)) {}

BlobView& BlobView::operator=(BlobView&& other) noexcept {
    if (this != &other) {
        if (blob_ != nullptr) {
            drmModeFreePropertyBlob(blob_);
        }
        blob_ = std::exchange(other.blob_, nullptr);
    }
    return *this;
}

Result<BlobView> BlobView::get(BorrowedFd fd, BlobId id) {
    if (id == kNoBlob) {
        return Err(Errc::BlobReadFailed, "blob id is 0");
    }
    auto* raw = MW_DRM_CALL_PTR(get_blob,
                                drmModeGetPropertyBlob(fd.get(), static_cast<uint32_t>(id)),
                                "blob id={}", static_cast<uint32_t>(id));
    if (raw == nullptr) {
        return Err(Errc::BlobReadFailed,
                   fmt("drmModeGetPropertyBlob({})", static_cast<uint32_t>(id)));
    }
    return Ok(BlobView(raw));
}

const void* BlobView::data() const noexcept {
    return blob_ != nullptr ? blob_->data : nullptr;
}

size_t BlobView::size() const noexcept {
    return blob_ != nullptr ? blob_->length : 0u;
}

} // namespace mw::drm
