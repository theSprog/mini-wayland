#include <dirent.h>
#include <fcntl.h>
#include <xf86drm.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "mw/trace/log.hpp"
#include "mw/drm/trace.hpp"
#include "mw/drm/device.hpp"

using internal::Ok;
using internal::Err;
using internal::fmt;
using internal::Error;
using internal::unexpected;

// IN_FORMATS blob 的布局定义在 UAPI 头里
#include <drm_fourcc.h>
#include <drm_mode.h>

namespace mw::drm {
namespace {

// ---------------------------------------------------------------------------
// libdrm 资源的 RAII 包装
// ---------------------------------------------------------------------------
// libdrm 的每个 GetXxx 都配一个 FreeXxx，忘一个就漏一块。
// 全部包成 unique_ptr，之后代码里不出现任何裸的 drmModeFreeXxx。

struct ResDeleter {
    void operator()(drmModeRes* p) const noexcept {
        drmModeFreeResources(p);
    }
};
using UniqueRes = std::unique_ptr<drmModeRes, ResDeleter>;

struct PlaneResDeleter {
    void operator()(drmModePlaneRes* p) const noexcept {
        drmModeFreePlaneResources(p);
    }
};
using UniquePlaneRes = std::unique_ptr<drmModePlaneRes, PlaneResDeleter>;

struct ConnectorDeleter {
    void operator()(drmModeConnector* p) const noexcept {
        drmModeFreeConnector(p);
    }
};
using UniqueConnector = std::unique_ptr<drmModeConnector, ConnectorDeleter>;

struct EncoderDeleter {
    void operator()(drmModeEncoder* p) const noexcept {
        drmModeFreeEncoder(p);
    }
};
using UniqueEncoder = std::unique_ptr<drmModeEncoder, EncoderDeleter>;

struct PlaneDeleter {
    void operator()(drmModePlane* p) const noexcept {
        drmModeFreePlane(p);
    }
};
using UniquePlane = std::unique_ptr<drmModePlane, PlaneDeleter>;

// ---------------------------------------------------------------------------

PlaneType plane_type_from_value(uint64_t value) noexcept {
    switch (value) {
        case DRM_PLANE_TYPE_PRIMARY: return PlaneType::Primary;
        case DRM_PLANE_TYPE_CURSOR:  return PlaneType::Cursor;
        case DRM_PLANE_TYPE_OVERLAY: return PlaneType::Overlay;
        default:                     return PlaneType::Overlay;
    }
}

/**
 * @brief 解析 IN_FORMATS blob
 *
 * blob 的布局（drm_mode.h）：
 *
 *   struct drm_format_modifier_blob {
 *       u32 version, flags;
 *       u32 count_formats,   formats_offset;     -> u32 fourcc[count_formats]
 *       u32 count_modifiers, modifiers_offset;   -> struct drm_format_modifier[]
 *   };
 *   struct drm_format_modifier {
 *       u64 formats;   // 位图：相对 offset 的 64 个 format 里哪些支持本 modifier
 *       u32 offset;    // 位图第 0 位对应 fourcc[] 里的下标
 *       u64 modifier;
 *   };
 *
 * 位图 + offset 这个设计是为了压缩：格式多于 64 个时用多条
 * drm_format_modifier 记录，每条覆盖一个 64 格式的窗口。
 * 勘察结果里 vsdrm 的 plane 有 30+ 个格式，会用到这个机制。
 *
 * 这里**只搬运不解释** —— modifier 的值原样存进 FormatModifier，
 * 不做任何 vendor 判断。
 */
void parse_in_formats(const void* data, size_t length, std::vector<FormatModifier>& out) {
    if (data == nullptr || length < sizeof(drm_format_modifier_blob)) {
        LOG_WARN("IN_FORMATS blob too small ({} bytes), ignoring", length);
        return;
    }

    const auto* header = static_cast<const drm_format_modifier_blob*>(data);
    if (header->version != FORMAT_BLOB_CURRENT) {
        LOG_WARN("IN_FORMATS blob version {} is not {}, ignoring", header->version,
                 FORMAT_BLOB_CURRENT);
        return;
    }

    // 边界检查：blob 是内核给的，但偏移量算错会直接读越界。
    const auto* base = static_cast<const uint8_t*>(data);
    const size_t formats_end =
        static_cast<size_t>(header->formats_offset) + static_cast<size_t>(header->count_formats) * 4u;
    const size_t modifiers_end = static_cast<size_t>(header->modifiers_offset) +
                                 static_cast<size_t>(header->count_modifiers) *
                                     sizeof(drm_format_modifier);
    if (formats_end > length || modifiers_end > length) {
        LOG_WARN("IN_FORMATS blob offsets out of range (len={} formats_end={} modifiers_end={})",
                 length, formats_end, modifiers_end);
        return;
    }

    const auto* formats = reinterpret_cast<const uint32_t*>(base + header->formats_offset);
    const auto* modifiers =
        reinterpret_cast<const drm_format_modifier*>(base + header->modifiers_offset);

    out.reserve(out.size() + static_cast<size_t>(header->count_formats));

    for (uint32_t m = 0; m < header->count_modifiers; ++m) {
        const drm_format_modifier& entry = modifiers[m];
        for (uint32_t bit = 0; bit < 64u; ++bit) {
            if (((entry.formats >> bit) & 1ULL) == 0ULL) {
                continue;
            }
            const uint64_t index = static_cast<uint64_t>(entry.offset) + bit;
            if (index >= header->count_formats) {
                continue;
            }
            out.push_back(FormatModifier{Format{formats[index]}, Modifier{entry.modifier}});
        }
    }

    LOG_TRACE("IN_FORMATS: {} formats x {} modifier records -> {} pairs", header->count_formats,
              header->count_modifiers, out.size());
}

/// 从 sysfs 名字或 drmGetVersion 拿 DRM driver name（不是 PCI driver name）
std::string driver_name_of(BorrowedFd fd) {
    std::string name;
    if (auto* version = drmGetVersion(fd.get()); version != nullptr) {
        if (version->name != nullptr && version->name_len > 0) {
            name.assign(version->name, static_cast<size_t>(version->name_len));
        }
        drmFreeVersion(version);
    }
    return name;
}

} // namespace

// ---------------------------------------------------------------------------
// 名字表
// ---------------------------------------------------------------------------

const char* connector_type_name(uint32_t type) noexcept {
    switch (type) {
        case DRM_MODE_CONNECTOR_Unknown:     return "Unknown";
        case DRM_MODE_CONNECTOR_VGA:         return "VGA";
        case DRM_MODE_CONNECTOR_DVII:        return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID:        return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA:        return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite:   return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO:      return "SVIDEO";
        case DRM_MODE_CONNECTOR_LVDS:        return "LVDS";
        case DRM_MODE_CONNECTOR_Component:   return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN:     return "DIN";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_HDMIA:       return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB:       return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV:          return "TV";
        case DRM_MODE_CONNECTOR_eDP:         return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL:     return "Virtual";
        case DRM_MODE_CONNECTOR_DSI:         return "DSI";
        case DRM_MODE_CONNECTOR_DPI:         return "DPI";
        case DRM_MODE_CONNECTOR_WRITEBACK:   return "Writeback";
        default:                             return "Unknown";
    }
}

const char* encoder_type_name(uint32_t type) noexcept {
    switch (type) {
        case DRM_MODE_ENCODER_NONE:    return "None";
        case DRM_MODE_ENCODER_DAC:     return "DAC";
        case DRM_MODE_ENCODER_TMDS:    return "TMDS";
        case DRM_MODE_ENCODER_LVDS:    return "LVDS";
        case DRM_MODE_ENCODER_TVDAC:   return "TVDAC";
        case DRM_MODE_ENCODER_VIRTUAL: return "Virtual";
        case DRM_MODE_ENCODER_DSI:     return "DSI";
        case DRM_MODE_ENCODER_DPMST:   return "DP-MST";
        case DRM_MODE_ENCODER_DPI:     return "DPI";
        default:                       return "Unknown";
    }
}

std::string to_string(Generation g) {
    return "gen" + std::to_string(static_cast<uint64_t>(g));
}

// ---------------------------------------------------------------------------
// ModeInfo
// ---------------------------------------------------------------------------

uint32_t ModeInfo::refresh_mhz() const noexcept {
    // clock 单位是 kHz，htotal/vtotal 是像素/行数。
    // 刷新率 = clock * 1000 / (htotal * vtotal)  [Hz]
    //        = clock * 1000000 / (htotal * vtotal)  [mHz]
    //
    // 不用 raw.vrefresh：那个字段是四舍五入到整数 Hz 的（60 而不是 59.94），
    // 拿它算帧间隔会稳定偏差 0.1%，一分钟就差一帧。
    const uint64_t denom = static_cast<uint64_t>(raw.htotal) * static_cast<uint64_t>(raw.vtotal);
    if (denom == 0u) {
        return 0u;
    }
    uint64_t numerator = static_cast<uint64_t>(raw.clock) * 1000000ULL;

    // 隔行扫描：一个刷新周期只扫一半的行，实际场率翻倍
    if ((raw.flags & DRM_MODE_FLAG_INTERLACE) != 0u) {
        numerator *= 2u;
    }
    // DBLSCAN：每行扫两遍，实际帧率减半
    if ((raw.flags & DRM_MODE_FLAG_DBLSCAN) != 0u) {
        numerator /= 2u;
    }
    // vscan > 1：每行重复 vscan 次
    if (raw.vscan > 1u) {
        numerator /= raw.vscan;
    }

    return static_cast<uint32_t>(numerator / denom);
}

uint64_t ModeInfo::frame_duration_ns() const noexcept {
    const uint32_t mhz = refresh_mhz();
    if (mhz == 0u) {
        return 0u;
    }
    // 1e9 ns/s / (mhz / 1000 Hz) = 1e12 / mhz
    return 1000000000000ULL / static_cast<uint64_t>(mhz);
}

bool ModeInfo::is_preferred() const noexcept {
    return (raw.type & DRM_MODE_TYPE_PREFERRED) != 0u;
}

bool ModeInfo::is_interlaced() const noexcept {
    return (raw.flags & DRM_MODE_FLAG_INTERLACE) != 0u;
}

std::string ModeInfo::name() const {
    char buf[64];
    const uint32_t mhz = refresh_mhz();
    std::snprintf(buf, sizeof(buf), "%ux%u%s@%u.%03u", raw.hdisplay, raw.vdisplay,
                  is_interlaced() ? "i" : "", mhz / 1000u, mhz % 1000u);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Connector / Plane 查询
// ---------------------------------------------------------------------------

const ModeInfo* Connector::preferred_mode() const noexcept {
    for (const auto& mode : modes) {
        if (mode.is_preferred()) {
            return &mode;
        }
    }
    // 没有 PREFERRED 标记时用第一个：内核按偏好降序排列 mode 列表。
    return modes.empty() ? nullptr : &modes.front();
}

const ModeInfo* Connector::find_mode(Size wanted) const noexcept {
    const ModeInfo* best = nullptr;
    for (const auto& mode : modes) {
        if (mode.size() != wanted) {
            continue;
        }
        if (best == nullptr || mode.refresh_mhz() > best->refresh_mhz()) {
            best = &mode;
        }
    }
    return best;
}

bool Plane::supports(Format fmt, Modifier mod) const noexcept {
    for (const auto& pair : formats) {
        if (pair.format == fmt && pair.modifier == mod) {
            return true;
        }
    }
    return false;
}

bool Plane::supports_format(Format fmt) const noexcept {
    for (const auto& pair : formats) {
        if (pair.format == fmt) {
            return true;
        }
    }
    return false;
}

std::string OutputPath::to_string() const {
    std::string out;
    out += "output path (";
    out += drm::to_string(generation);
    out += "):\n";
    out += "  connector    " + drm::to_string(connector) + "\n";
    out += "  crtc         " + drm::to_string(crtc) + " " + drm::to_string(crtc_index) + "\n";
    out += "  primary      " + drm::to_string(primary_plane) + "\n";
    out += "  mode         " + mode.name() + "\n";
    out += "  frame time   " + std::to_string(mode.frame_duration_ns() / 1000u) + " us\n";
    return out;
}

std::string DeviceCandidate::to_string() const {
    std::string out = path;
    out += " driver=";
    out += driver_name.empty() ? "?" : driver_name;
    out += has_kms ? " kms=yes" : " kms=no";
    out += has_connected_connector ? " connected=yes" : " connected=no";
    return out;
}

// ---------------------------------------------------------------------------
// 节点发现
// ---------------------------------------------------------------------------

std::vector<DeviceCandidate> enumerate_devices() {
    std::vector<DeviceCandidate> out;

    // 不用 drmGetDevices2：它按 PCI/platform 设备聚合，而我们要的正是
    // "同一个物理设备的多个 card 节点"这层信息（勘察结果里 card0 和 card2
    // 是同一个 PCI 设备，card0 没有 connector）。直接扫 /dev/dri 更直白。
    DIR* dir = ::opendir("/dev/dri");
    if (dir == nullptr) {
        // errno 紧贴失败点抓，不隔任何调用 —— 日志宏虽然保证 errno 透明，
        // 但显式写出来读代码时不用去查宏的实现。
        const int err = errno;
        LOG_ERROR("cannot open /dev/dri: {}", errno_name(err));
        return out;
    }

    std::vector<std::string> paths;
    while (const dirent* entry = ::readdir(dir)) {
        if (std::strncmp(entry->d_name, "card", 4) != 0) {
            continue;
        }
        paths.emplace_back(std::string("/dev/dri/") + entry->d_name);
    }
    ::closedir(dir);

    std::sort(paths.begin(), paths.end());

    LOG_DEBUG("scanning {} card nodes under /dev/dri", paths.size());
    LOG_SCOPE();

    for (const auto& path : paths) {
        DeviceCandidate candidate;
        candidate.path = path;

        auto fd_result = UniqueFd::open(path.c_str(), O_RDWR);
        if (! fd_result) {
            // 权限不足是常态（非 root 跑的时候），不当错误。
            LOG_DEBUG("{}: cannot open ({})", path, errno_name(errno_of(fd_result.error())));
            out.push_back(std::move(candidate));
            continue;
        }
        const UniqueFd fd = std::move(fd_result).value();

        candidate.driver_name = driver_name_of(fd.borrow());

        auto* res = MW_DRM_CALL_PTR(get_resources, drmModeGetResources(fd.get()), "probe {}", path);
        if (res == nullptr) {
            // render node，或者没有显示引擎的 card（勘察结果里的 card0）
            LOG_DEBUG("{}: driver='{}' no KMS resources", path, candidate.driver_name);
            out.push_back(std::move(candidate));
            continue;
        }
        const UniqueRes resources(res);

        candidate.has_kms = resources->count_crtcs > 0 && resources->count_connectors > 0;

        for (int i = 0; i < resources->count_connectors; ++i) {
            // 用 GetConnectorCurrent 而不是 GetConnector：后者会强制
            // probe（DDC/AUX 通信），一个断开的 connector 能阻塞几十毫秒，
            // 扫全系统时会明显卡顿。这里只想知道内核缓存的状态。
            auto* conn = MW_DRM_CALL_PTR(
                get_connector, drmModeGetConnectorCurrent(fd.get(), resources->connectors[i]),
                "connector id={} (cached, no probe)", resources->connectors[i]);
            if (conn == nullptr) {
                continue;
            }
            const UniqueConnector connector(conn);
            if (connector->connection == DRM_MODE_CONNECTED) {
                candidate.has_connected_connector = true;
                break;
            }
        }

        LOG_DEBUG("{}", candidate.to_string());
        out.push_back(std::move(candidate));
    }

    return out;
}

std::optional<std::string> find_render_node(const std::string& kms_path) {
    auto fd_result = UniqueFd::open(kms_path.c_str(), O_RDWR);
    if (! fd_result) {
        LOG_DEBUG("find_render_node: cannot open {}", kms_path);
        return std::nullopt;
    }
    const UniqueFd fd = std::move(fd_result).value();

    drmDevicePtr device = nullptr;
    if (drmGetDevice2(fd.get(), 0, &device) != 0 || device == nullptr) {
        LOG_DEBUG("find_render_node: drmGetDevice2 failed for {}", kms_path);
        return std::nullopt;
    }

    std::optional<std::string> result;
    if ((device->available_nodes & (1 << DRM_NODE_RENDER)) != 0) {
        result = std::string(device->nodes[DRM_NODE_RENDER]);
        LOG_DEBUG("render node for {} is {}", kms_path, *result);
    } else {
        // VKMS 就没有 render node —— 它不是渲染设备。
        LOG_DEBUG("{} has no render node", kms_path);
    }
    drmFreeDevice(&device);
    return result;
}

// ---------------------------------------------------------------------------
// MasterGuard
// ---------------------------------------------------------------------------

MasterGuard::~MasterGuard() {
    drop();
}

MasterGuard::MasterGuard(MasterGuard&& other) noexcept
    : fd_(other.fd_), held_(std::exchange(other.held_, false)) {}

MasterGuard& MasterGuard::operator=(MasterGuard&& other) noexcept {
    if (this != &other) {
        drop();
        fd_ = other.fd_;
        held_ = std::exchange(other.held_, false);
    }
    return *this;
}

void MasterGuard::drop() noexcept {
    if (! held_) {
        return;
    }
    const int ret = MW_DRM_CALL(drop_master, drmDropMaster(fd_.get()), "fd={}", fd_.get());
    if (ret != 0) {
        LOG_WARN("drmDropMaster failed: {}", errno_name(errno));
    } else {
        LOG_INFO("dropped DRM master");
    }
    held_ = false;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

Result<Device> Device::open(const std::string& path) {
    Device device;
    device.path_ = path;
    device.fd_ = TRY(UniqueFd::open(path.c_str(), O_RDWR));

    LOG_INFO("opened KMS node {}", path);

    // 先探能力（会设 client caps），再枚举 —— 顺序不能反：
    // 没设 UNIVERSAL_PLANES 之前 drmModeGetPlaneResources 只返回 overlay plane，
    // primary 和 cursor 都是隐藏的。
    device.caps_ = TRY(probe_kernel_caps(device.fd_.borrow()));

    if (auto status = device.caps_.check_minimum(); ! status) {
        return unexpected<Error>(std::move(status).error());
    }

    if (auto status = device.rescan(); ! status) {
        return unexpected<Error>(std::move(status).error());
    }

    return Ok(std::move(device));
}

Result<Device> Device::open_by_driver(const std::string& drm_driver_name) {
    LOG_INFO("looking for a KMS node with DRM driver name '{}'", drm_driver_name);
    LOG_INFO("  (note: this is the DRM driver name from drmGetVersion, not the PCI driver name)");

    for (const auto& candidate : enumerate_devices()) {
        if (candidate.driver_name != drm_driver_name) {
            continue;
        }
        if (! candidate.has_kms) {
            // 同一个物理设备可能有多个节点，只有一个带 KMS。
            LOG_DEBUG("{} matches the driver name but has no KMS resources, skipping",
                      candidate.path);
            continue;
        }
        return Device::open(candidate.path);
    }

    return Err(Errc::NotKmsCapable,
               fmt("no KMS-capable node with DRM driver name '{}'", drm_driver_name));
}

Result<Device> Device::open_first_kms() {
    const auto candidates = enumerate_devices();

    // 两轮：先要"有 KMS 且有已连接显示器"的，退而求其次只要有 KMS。
    // 后者对 writeback-only 或者拔了线的开发场景有用。
    for (const auto& candidate : candidates) {
        if (candidate.has_kms && candidate.has_connected_connector) {
            LOG_INFO("picking {} (has a connected display)", candidate.path);
            return Device::open(candidate.path);
        }
    }
    for (const auto& candidate : candidates) {
        if (candidate.has_kms) {
            LOG_WARN("picking {} but it has no connected connector", candidate.path);
            return Device::open(candidate.path);
        }
    }
    return Err(Errc::NotKmsCapable, "no KMS-capable DRM node found under /dev/dri");
}

span<const Connector> Device::connectors() const noexcept {
    return span<const Connector>(connectors_.data(), connectors_.size());
}

span<const Crtc> Device::crtcs() const noexcept {
    return span<const Crtc>(crtcs_.data(), crtcs_.size());
}

span<const Encoder> Device::encoders() const noexcept {
    return span<const Encoder>(encoders_.data(), encoders_.size());
}

span<const Plane> Device::planes() const noexcept {
    return span<const Plane>(planes_.data(), planes_.size());
}

const Connector* Device::connector(ConnectorId id) const noexcept {
    for (const auto& item : connectors_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const Crtc* Device::crtc(CrtcId id) const noexcept {
    for (const auto& item : crtcs_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const Encoder* Device::encoder(EncoderId id) const noexcept {
    for (const auto& item : encoders_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

const Plane* Device::plane(PlaneId id) const noexcept {
    for (const auto& item : planes_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

std::optional<CrtcIndex> Device::crtc_index_of(CrtcId id) const noexcept {
    for (const auto& item : crtcs_) {
        if (item.id == id) {
            return item.index;
        }
    }
    return std::nullopt;
}

std::optional<CrtcId> Device::crtc_at(CrtcIndex index) const noexcept {
    for (const auto& item : crtcs_) {
        if (item.index == index) {
            return item.id;
        }
    }
    return std::nullopt;
}

std::vector<PlaneId> Device::planes_for_crtc(CrtcId crtc_id, PlaneType type) const {
    std::vector<PlaneId> out;
    const auto index = crtc_index_of(crtc_id);
    if (! index) {
        LOG_WARN("planes_for_crtc: {} is not in the current snapshot", to_string(crtc_id));
        return out;
    }
    for (const auto& item : planes_) {
        if (item.type == type && item.possible_crtcs.contains(*index)) {
            out.push_back(item.id);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// ---------------------------------------------------------------------------
// 枚举
// ---------------------------------------------------------------------------

Status Device::rescan() {
    connectors_.clear();
    encoders_.clear();
    crtcs_.clear();
    planes_.clear();

    generation_ = Generation{static_cast<uint64_t>(generation_) + 1u};
    LOG_INFO("enumerating KMS resources ({})", to_string(generation_));
    LOG_SCOPE();

    auto* res_raw = MW_DRM_CALL_PTR(get_resources, drmModeGetResources(fd_.get()), "fd={}",
                                    fd_.get());
    if (res_raw == nullptr) {
        return Err(Errc::NotKmsCapable, fmt("drmModeGetResources on '{}'", path_));
    }
    const UniqueRes res(res_raw);

    // ---- CRTC ----
    //
    // 关键：CrtcIndex 就是 res->crtcs[] 里的下标。possible_crtcs 位图的
    // 第 n 位对应 res->crtcs[n]，**不是** crtc_id。这个映射只在这里建立一次。
    crtcs_.reserve(static_cast<size_t>(res->count_crtcs));
    for (int i = 0; i < res->count_crtcs; ++i) {
        Crtc crtc_obj;
        crtc_obj.id = CrtcId{res->crtcs[i]};
        crtc_obj.index = CrtcIndex{static_cast<uint32_t>(i)};
        crtc_obj.props =
            TRY(PropertyMap::query(fd_.borrow(), res->crtcs[i], DRM_MODE_OBJECT_CRTC, prop_defs_));
        crtc_obj.prop_ids = TRY(CrtcPropIds::resolve(crtc_obj.props));
        crtc_obj.active = crtc_obj.props.value_or("ACTIVE", 0u) != 0u;

        LOG_DEBUG("crtc {} index={} active={}", to_string(crtc_obj.id), i, crtc_obj.active);
        crtcs_.push_back(std::move(crtc_obj));
    }

    // ---- encoder ----
    encoders_.reserve(static_cast<size_t>(res->count_encoders));
    for (int i = 0; i < res->count_encoders; ++i) {
        auto* enc_raw = MW_DRM_CALL_PTR(get_encoder, drmModeGetEncoder(fd_.get(), res->encoders[i]),
                                        "encoder id={}", res->encoders[i]);
        if (enc_raw == nullptr) {
            LOG_WARN("drmModeGetEncoder({}) failed, skipping", res->encoders[i]);
            continue;
        }
        const UniqueEncoder enc(enc_raw);

        Encoder encoder_obj;
        encoder_obj.id = EncoderId{enc->encoder_id};
        encoder_obj.type = enc->encoder_type;
        encoder_obj.possible_crtcs = PossibleCrtcs{enc->possible_crtcs};
        encoder_obj.current_crtc = CrtcId{enc->crtc_id};

        LOG_DEBUG("encoder {} type={} possible_crtcs={}", to_string(encoder_obj.id),
                  encoder_type_name(encoder_obj.type), to_string(encoder_obj.possible_crtcs));
        encoders_.push_back(encoder_obj);
    }

    // ---- connector ----
    connectors_.reserve(static_cast<size_t>(res->count_connectors));
    for (int i = 0; i < res->count_connectors; ++i) {
        // 这里用 drmModeGetConnector（会 probe），不是 Current 版本：
        // 我们要的是**此刻**的连接状态和 mode 列表，enumerate_devices()
        // 那种快速扫描才用 Current。代价是断开的 connector 可能阻塞几十毫秒。
        auto* conn_raw = MW_DRM_CALL_PTR(get_connector,
                                         drmModeGetConnector(fd_.get(), res->connectors[i]),
                                         "connector id={}", res->connectors[i]);
        if (conn_raw == nullptr) {
            LOG_WARN("drmModeGetConnector({}) failed, skipping", res->connectors[i]);
            continue;
        }
        const UniqueConnector conn(conn_raw);

        Connector connector_obj;
        connector_obj.id = ConnectorId{conn->connector_id};
        connector_obj.type = conn->connector_type;
        connector_obj.type_id = conn->connector_type_id;
        connector_obj.connected = conn->connection == DRM_MODE_CONNECTED;
        connector_obj.is_writeback = conn->connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
        connector_obj.mm_size = Size{conn->mmWidth, conn->mmHeight};
        connector_obj.current_crtc = kNoCrtc;

        {
            char name_buf[64];
            std::snprintf(name_buf, sizeof(name_buf), "%s-%u",
                          connector_type_name(connector_obj.type), connector_obj.type_id);
            connector_obj.name = name_buf;
        }

        connector_obj.modes.reserve(static_cast<size_t>(conn->count_modes));
        for (int m = 0; m < conn->count_modes; ++m) {
            ModeInfo mode;
            mode.raw = conn->modes[m];
            connector_obj.modes.push_back(mode);
        }

        connector_obj.encoders.reserve(static_cast<size_t>(conn->count_encoders));
        for (int e = 0; e < conn->count_encoders; ++e) {
            connector_obj.encoders.push_back(EncoderId{conn->encoders[e]});
        }

        // encoder_id 是内核记录的当前绑定，顺着它能找到当前 CRTC。
        // 仅供诊断 —— 我们的启动流程是无条件完整 modeset，不读当前状态做增量。
        if (conn->encoder_id != 0u) {
            if (const Encoder* enc = encoder(EncoderId{conn->encoder_id}); enc != nullptr) {
                connector_obj.current_crtc = enc->current_crtc;
            }
        }

        connector_obj.props =
            TRY(PropertyMap::query(fd_.borrow(), conn->connector_id, DRM_MODE_OBJECT_CONNECTOR,
                                   prop_defs_));
        connector_obj.prop_ids = TRY(ConnectorPropIds::resolve(connector_obj.props));

        LOG_DEBUG("connector {} {} {} modes={} encoders={}", to_string(connector_obj.id),
                  connector_obj.name, connector_obj.connected ? "connected" : "disconnected",
                  connector_obj.modes.size(), connector_obj.encoders.size());

        if (connector_obj.is_writeback) {
            caps_.has_writeback_connector = true;
        }
        connectors_.push_back(std::move(connector_obj));
    }

    // ---- plane ----
    auto* plane_res_raw = MW_DRM_CALL_PTR(get_plane_resources,
                                          drmModeGetPlaneResources(fd_.get()), "fd={}", fd_.get());
    if (plane_res_raw == nullptr) {
        return Err(Errc::ResourceQueryFailed, "drmModeGetPlaneResources");
    }
    const UniquePlaneRes plane_res(plane_res_raw);

    planes_.reserve(static_cast<size_t>(plane_res->count_planes));
    for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
        auto* plane_raw = MW_DRM_CALL_PTR(get_plane,
                                          drmModeGetPlane(fd_.get(), plane_res->planes[i]),
                                          "plane id={}", plane_res->planes[i]);
        if (plane_raw == nullptr) {
            LOG_WARN("drmModeGetPlane({}) failed, skipping", plane_res->planes[i]);
            continue;
        }
        const UniquePlane plane_ptr(plane_raw);

        Plane plane_obj;
        plane_obj.id = PlaneId{plane_ptr->plane_id};
        plane_obj.possible_crtcs = PossibleCrtcs{plane_ptr->possible_crtcs};
        plane_obj.current_crtc = CrtcId{plane_ptr->crtc_id};
        plane_obj.props =
            TRY(PropertyMap::query(fd_.borrow(), plane_ptr->plane_id, DRM_MODE_OBJECT_PLANE,
                                   prop_defs_));
        plane_obj.prop_ids = TRY(PlanePropIds::resolve(plane_obj.props));
        plane_obj.type = plane_type_from_value(plane_obj.props.value_or("type", DRM_PLANE_TYPE_OVERLAY));

        // 格式列表：优先 IN_FORMATS（带 modifier），否则退回裸 format 列表。
        bool got_in_formats = false;
        if (const auto info = plane_obj.props.find("IN_FORMATS");
            info && info->value != 0u) {
            auto blob_result = BlobView::get(fd_.borrow(), BlobId{static_cast<uint32_t>(info->value)});
            if (blob_result) {
                const BlobView blob = std::move(blob_result).value();
                parse_in_formats(blob.data(), blob.size(), plane_obj.formats);
                got_in_formats = ! plane_obj.formats.empty();
            } else {
                LOG_WARN("plane {} has IN_FORMATS but the blob could not be read",
                         to_string(plane_obj.id));
            }
        }

        if (! got_in_formats) {
            // 没有 modifier 信息，记 kModifierInvalid 而**不是** LINEAR。
            // 两者对应 addfb 的两条不同路径，混用会在某些驱动上被拒。
            plane_obj.formats.reserve(static_cast<size_t>(plane_ptr->count_formats));
            for (uint32_t f = 0; f < plane_ptr->count_formats; ++f) {
                plane_obj.formats.push_back(
                    FormatModifier{Format{plane_ptr->formats[f]}, kModifierInvalid});
            }
            LOG_DEBUG("plane {} has no IN_FORMATS; {} formats without modifier info",
                      to_string(plane_obj.id), plane_obj.formats.size());
        }

        LOG_DEBUG("plane {} type={} possible_crtcs={} formats={}", to_string(plane_obj.id),
                  to_string(plane_obj.type), to_string(plane_obj.possible_crtcs),
                  plane_obj.formats.size());

        planes_.push_back(std::move(plane_obj));
    }

    caps_.num_connectors = static_cast<uint32_t>(connectors_.size());
    caps_.num_crtcs = static_cast<uint32_t>(crtcs_.size());
    caps_.num_planes = static_cast<uint32_t>(planes_.size());

    if (! crtcs_.empty() && ! planes_.empty()) {
        refine_caps_from_objects(caps_, crtcs_.front().props, planes_.front().props);
    }

    LOG_INFO("enumerated {} connectors, {} encoders, {} crtcs, {} planes", connectors_.size(),
             encoders_.size(), crtcs_.size(), planes_.size());
    // 属性**定义**是设备全局的，属性**值**才是每个对象一份。
    // 这行数字就是证据：几百次查询里真正不同的定义只有几十个。
    LOG_INFO("property definitions: {} distinct, {} redundant queries avoided",
             prop_defs_.size(), prop_defs_.hits());
    return Ok();
}

// ---------------------------------------------------------------------------
// 拓扑匹配
// ---------------------------------------------------------------------------

Result<OutputPath> Device::pick_output(const OutputRequest& request) const {
    LOG_INFO("picking an output path");
    LOG_SCOPE();

    // ---- 1. connector ----
    const Connector* chosen = nullptr;
    if (request.connector) {
        chosen = connector(*request.connector);
        if (chosen == nullptr) {
            return Err(Errc::NoConnectedConnector,
                       fmt("requested {} is not present", to_string(*request.connector)));
        }
        if (! chosen->connected) {
            return Err(Errc::NoConnectedConnector,
                       fmt("requested {} ({}) is not connected", to_string(chosen->id),
                           chosen->name));
        }
    } else {
        for (const auto& item : connectors_) {
            if (! item.connected) {
                LOG_DEBUG("skip {} ({}): disconnected", to_string(item.id), item.name);
                continue;
            }
            if (item.is_writeback) {
                // writeback connector 永远报 connected，但它不是显示输出。
                LOG_DEBUG("skip {} ({}): writeback connector", to_string(item.id), item.name);
                continue;
            }
            if (item.modes.empty()) {
                LOG_DEBUG("skip {} ({}): connected but reports no modes", to_string(item.id),
                          item.name);
                continue;
            }
            chosen = &item;
            break;
        }
        if (chosen == nullptr) {
            return Err(Errc::NoConnectedConnector,
                       fmt("none of the {} connectors is a usable display output",
                           connectors_.size()));
        }
    }
    LOG_INFO("connector: {} ({})", to_string(chosen->id), chosen->name);

    // ---- 2. mode ----
    const ModeInfo* mode = nullptr;
    if (request.mode_size) {
        mode = chosen->find_mode(*request.mode_size);
        if (mode == nullptr) {
            return Err(Errc::NoModeAvailable,
                       fmt("{} does not advertise a {} mode", chosen->name,
                           to_string(*request.mode_size)));
        }
    } else {
        mode = chosen->preferred_mode();
        if (mode == nullptr) {
            return Err(Errc::NoModeAvailable, fmt("{} reports no modes", chosen->name));
        }
    }
    LOG_INFO("mode: {} ({} ns per frame)", mode->name(), mode->frame_duration_ns());

    // ---- 3. CRTC ----
    //
    // 这是整个函数的重点。必须真的遍历 encoder 的 possible_crtcs 位图，
    // 不能"取第一个 CRTC"。勘察结果里 HDMI-A-1 的 encoder possible_crtcs
    // 是 0x2，只允许 crtcs[1]；取 crtcs[0] 会在 commit 时得到 EINVAL。
    CrtcId chosen_crtc = kNoCrtc;
    CrtcIndex chosen_index{};
    for (const EncoderId enc_id : chosen->encoders) {
        const Encoder* enc = encoder(enc_id);
        if (enc == nullptr) {
            LOG_DEBUG("encoder {} listed by connector but not in snapshot", to_string(enc_id));
            continue;
        }
        LOG_DEBUG("encoder {} type={} possible_crtcs={}", to_string(enc_id),
                  encoder_type_name(enc->type), to_string(enc->possible_crtcs));
        LOG_SCOPE();

        for (const auto& candidate : crtcs_) {
            if (! enc->possible_crtcs.contains(candidate.index)) {
                LOG_TRACE("{} {} not in this encoder's bitmask", to_string(candidate.id),
                          to_string(candidate.index));
                continue;
            }

            // 别的 connector 正在用这个 CRTC 就跳过。
            // 单输出场景下这一条几乎不会触发，但多显示器时不检查会互相抢。
            bool taken = false;
            for (const auto& other : connectors_) {
                if (other.id != chosen->id && other.current_crtc == candidate.id &&
                    other.connected) {
                    LOG_DEBUG("{} is already driving {} ({}), skipping", to_string(candidate.id),
                              to_string(other.id), other.name);
                    taken = true;
                    break;
                }
            }
            if (taken) {
                continue;
            }

            chosen_crtc = candidate.id;
            chosen_index = candidate.index;
            break;
        }
        if (chosen_crtc != kNoCrtc) {
            break;
        }
    }

    if (chosen_crtc == kNoCrtc) {
        return Err(Errc::NoCompatibleCrtc,
                   fmt("no CRTC in the possible_crtcs bitmask of {}'s {} encoder(s) is available",
                       chosen->name, chosen->encoders.size()));
    }
    LOG_INFO("crtc: {} {}", to_string(chosen_crtc), to_string(chosen_index));

    // ---- 4. primary plane ----
    PlaneId primary = kNoPlane;
    for (const auto& candidate : planes_) {
        if (candidate.type != PlaneType::Primary) {
            continue;
        }
        if (! candidate.possible_crtcs.contains(chosen_index)) {
            LOG_TRACE("plane {} is primary but not usable on {}", to_string(candidate.id),
                      to_string(chosen_index));
            continue;
        }
        primary = candidate.id;
        break;
    }
    if (primary == kNoPlane) {
        return Err(Errc::NoCompatiblePlane,
                   fmt("no primary plane is usable on {} {}", to_string(chosen_crtc),
                       to_string(chosen_index)));
    }
    LOG_INFO("primary plane: {}", to_string(primary));

    OutputPath path;
    path.generation = generation_;
    path.connector = chosen->id;
    path.crtc = chosen_crtc;
    path.crtc_index = chosen_index;
    path.primary_plane = primary;
    path.mode = *mode;
    return Ok(path);
}

Status Device::validate(const OutputPath& path) const {
    if (path.generation != generation_) {
        return Err(Errc::StaleSnapshot,
                   fmt("output path was computed at {} but the device is at {}; "
                       "a rescan happened in between and all cached pointers are dangling",
                       to_string(path.generation), to_string(generation_)));
    }
    if (connector(path.connector) == nullptr) {
        return Err(Errc::StaleSnapshot, fmt("{} vanished", to_string(path.connector)));
    }
    if (crtc(path.crtc) == nullptr) {
        return Err(Errc::StaleSnapshot, fmt("{} vanished", to_string(path.crtc)));
    }
    if (plane(path.primary_plane) == nullptr) {
        return Err(Errc::StaleSnapshot, fmt("{} vanished", to_string(path.primary_plane)));
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// master
// ---------------------------------------------------------------------------

bool Device::is_master() const noexcept {
    // drmIsMaster 内部试一个只有 master 才能做的 ioctl（drmSetMaster 的
    // 幂等性检查），无副作用。
    return drmIsMaster(fd_.get()) != 0;
}

std::string Device::master_diagnosis() const {
    std::string out;
    out += "cannot become DRM master on ";
    out += path_;
    out += ". Common causes:\n";
    out += "  - a display server (X11 / another Wayland compositor) already holds master;\n";
    out += "    stop it with 'systemctl stop lightdm' or switch to a bare tty (Ctrl+Alt+F3)\n";
    out += "  - the process lacks permission; run as root, or join the 'video' group\n";
    out += "  - on kernels >= 5.8 a non-root process can only become master if it was\n";
    out += "    the first to open the node\n";
    out += "  - check who holds it:  sudo lsof ";
    out += path_;
    out += "\n";
    return out;
}

Result<MasterGuard> Device::acquire_master() {
    if (is_master()) {
        // 从 tty 直接启动且没人占的时候会走这条 —— 打开节点就自动是 master 了。
        LOG_INFO("already DRM master on {}", path_);
        return Ok(MasterGuard(fd_.borrow()));
    }

    const int ret = MW_DRM_CALL(set_master, drmSetMaster(fd_.get()), "fd={}", fd_.get());
    if (ret != 0) {
        const int err = errno;
        LOG_ERROR("drmSetMaster failed with {}", errno_name(err));
        LOG_ERROR("{}", master_diagnosis());
        return Err(Errc::NotMaster, fmt("drmSetMaster on '{}' returned {}", path_, errno_name(err)));
    }

    LOG_INFO("acquired DRM master on {}", path_);
    return Ok(MasterGuard(fd_.borrow()));
}

} // namespace mw::drm
