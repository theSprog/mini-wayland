
#include <drm_fourcc.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

#include "mw/trace/log.hpp"
#include "mw/drm/caps.hpp"
#include "mw/drm/dump.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/property.hpp"
#include "mw/internal/error.hpp"
#include "mw/internal/unique_fd.hpp"

using internal::Ok;
using internal::fmt;

namespace mw::drm {
namespace {

/// 定义在文件后半段（摘要那一节），这里先声明给 dump_plane 用
std::vector<uint64_t> distinct_modifier_list(const Plane& plane);

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

/**
 * @brief modifier 的 vendor 号 -> 名字
 *
 * 这张表是**唯一**允许解读 modifier 内部结构的地方，且只用于日志。
 * 定义在 drm_fourcc.h：高 8 位是 vendor，低 56 位是 vendor 自定义的 payload。
 * 我们只翻译 vendor 号，payload 一律原样打十六进制 —— 因为 payload 的
 * 含义完全由厂商定义，猜它等于给自己埋雷。
 */
const char* modifier_vendor_name(uint8_t vendor) noexcept {
    switch (vendor) {
        case DRM_FORMAT_MOD_VENDOR_NONE:      return "NONE";
        case DRM_FORMAT_MOD_VENDOR_INTEL:     return "INTEL";
        case DRM_FORMAT_MOD_VENDOR_AMD:       return "AMD";
        case DRM_FORMAT_MOD_VENDOR_NVIDIA:    return "NVIDIA";
        case DRM_FORMAT_MOD_VENDOR_SAMSUNG:   return "SAMSUNG";
        case DRM_FORMAT_MOD_VENDOR_QCOM:      return "QCOM";
        case DRM_FORMAT_MOD_VENDOR_VIVANTE:   return "VIVANTE";
        case DRM_FORMAT_MOD_VENDOR_BROADCOM:  return "BROADCOM";
        case DRM_FORMAT_MOD_VENDOR_ARM:       return "ARM";
        case DRM_FORMAT_MOD_VENDOR_ALLWINNER: return "ALLWINNER";
        case DRM_FORMAT_MOD_VENDOR_AMLOGIC:   return "AMLOGIC";
        default:                              return nullptr;
    }
}

std::string mode_flags_string(uint32_t flags) {
    std::string out;
    const auto add = [&out](const char* name) {
        if (! out.empty()) {
            out += '|';
        }
        out += name;
    };

    if ((flags & DRM_MODE_FLAG_PHSYNC) != 0u) add("+hsync");
    if ((flags & DRM_MODE_FLAG_NHSYNC) != 0u) add("-hsync");
    if ((flags & DRM_MODE_FLAG_PVSYNC) != 0u) add("+vsync");
    if ((flags & DRM_MODE_FLAG_NVSYNC) != 0u) add("-vsync");
    if ((flags & DRM_MODE_FLAG_INTERLACE) != 0u) add("interlace");
    if ((flags & DRM_MODE_FLAG_DBLSCAN) != 0u) add("dblscan");
    if ((flags & DRM_MODE_FLAG_CSYNC) != 0u) add("csync");
    if ((flags & DRM_MODE_FLAG_DBLCLK) != 0u) add("dblclk");
    if ((flags & DRM_MODE_FLAG_CLKDIV2) != 0u) add("clkdiv2");

    return out.empty() ? "-" : out;
}

std::string mode_type_string(uint32_t type) {
    std::string out;
    const auto add = [&out](const char* name) {
        if (! out.empty()) {
            out += '|';
        }
        out += name;
    };

    if ((type & DRM_MODE_TYPE_PREFERRED) != 0u) add("preferred");
    if ((type & DRM_MODE_TYPE_DRIVER) != 0u) add("driver");
    if ((type & DRM_MODE_TYPE_USERDEF) != 0u) add("userdef");

    return out.empty() ? "-" : out;
}

} // namespace

// ---------------------------------------------------------------------------

std::string describe_modifier(Modifier mod) {
    if (mod == kModifierLinear) {
        return "LINEAR";
    }
    if (mod == kModifierInvalid) {
        return "INVALID(no modifier info)";
    }

    const uint64_t value = static_cast<uint64_t>(mod);
    const auto vendor = static_cast<uint8_t>(value >> 56);
    const uint64_t payload = value & 0x00ffffffffffffffULL;

    char buf[80];
    if (const char* name = modifier_vendor_name(vendor); name != nullptr) {
        std::snprintf(buf, sizeof(buf), "%s:0x%014llx", name,
                      static_cast<unsigned long long>(payload));
    } else {
        // 勘察结果里 vsdrm 的 0x0b 就走这条：不是 upstream 分配过的 vendor 号。
        // 不猜，原样打出来。
        std::snprintf(buf, sizeof(buf), "VENDOR(0x%02x):0x%014llx", vendor,
                      static_cast<unsigned long long>(payload));
    }
    return std::string(buf);
}

std::string describe_format(const FormatModifier& fm) {
    return to_string(fm.format) + "/" + describe_modifier(fm.modifier);
}

std::string describe_mode(const ModeInfo& mode) {
    // 字段顺序刻意和 modetest 的 mode 行对齐，方便逐列比对：
    //   name refresh hdisp hss hse htot vdisp vss vse vtot clock flags type
    const drmModeModeInfo& m = mode.raw;
    char buf[256];
    const uint32_t mhz = mode.refresh_mhz();
    std::snprintf(buf, sizeof(buf),
                  "%ux%u %u.%03u Hz  h: %u %u %u %u  v: %u %u %u %u  clock %u kHz  "
                  "vscan %u  flags %s  type %s",
                  m.hdisplay, m.vdisplay, mhz / 1000u, mhz % 1000u, m.hdisplay, m.hsync_start,
                  m.hsync_end, m.htotal, m.vdisplay, m.vsync_start, m.vsync_end, m.vtotal, m.clock,
                  m.vscan, mode_flags_string(m.flags).c_str(), mode_type_string(m.type).c_str());
    return std::string(buf);
}

// ---------------------------------------------------------------------------

void dump_properties(const Device& dev, const PropertyMap& props, const char* label) {
    LOG_INFO("{} properties ({}):", label, props.size());
    LOG_SCOPE();

    for (const auto& [name, info] : props.entries()) {
        std::string suffix;
        if (info.immutable) {
            suffix += " immutable";
        }
        if (info.atomic) {
            suffix += " atomic";
        }

        if (info.kind == PropKind::Blob) {
            // blob 属性的 value 是 blob id。打出长度，内容不展开
            // （EDID / IN_FORMATS 都很长，需要时有专门的 dump）。
            std::string extra = "no blob";
            if (info.value != 0u) {
                auto blob = BlobView::get(dev.fd(), BlobId{static_cast<uint32_t>(info.value)});
                if (blob) {
                    extra = fmt("blob id={} length={}", info.value, blob.value().size());
                } else {
                    extra = fmt("blob id={} (unreadable)", info.value);
                }
            }
            LOG_INFO("{:>26}  id={:<4} {:<8} {}{}", name, static_cast<uint32_t>(info.id),
                     kind_name(info.kind), extra, suffix);
        } else {
            LOG_INFO("{:>26}  id={:<4} {:<8} value={}{}", name, static_cast<uint32_t>(info.id),
                     kind_name(info.kind), info.value, suffix);
        }
    }
}

void dump_connector(const Device& dev, const Connector& conn) {
    LOG_INFO("connector {} '{}' {} {}mm x {}mm", to_string(conn.id), conn.name,
             conn.connected ? "connected" : "disconnected", conn.mm_size.width,
             conn.mm_size.height);
    LOG_SCOPE();

    if (conn.is_writeback) {
        // writeback connector 永远报 connected，但它不是显示输出 —— 它是
        // "把 CRTC 的合成结果写回一块 buffer"的伪 connector。
        // TODO(writeback): 无显示器自检管线可以用它。
        LOG_INFO("this is a WRITEBACK connector, not a display output");
    }

    std::string encoder_list;
    for (const EncoderId enc_id : conn.encoders) {
        if (! encoder_list.empty()) {
            encoder_list += ", ";
        }
        encoder_list += to_string(enc_id);
        if (const Encoder* enc = dev.encoder(enc_id); enc != nullptr) {
            encoder_list += "(";
            encoder_list += encoder_type_name(enc->type);
            encoder_list += " ";
            encoder_list += to_string(enc->possible_crtcs);
            encoder_list += ")";
        }
    }
    LOG_INFO("encoders: {}", encoder_list.empty() ? "none" : encoder_list);

    if (conn.current_crtc != kNoCrtc) {
        LOG_INFO("currently driven by {}", to_string(conn.current_crtc));
    }

    LOG_INFO("modes ({}):", conn.modes.size());
    {
        LOG_SCOPE();
        for (const auto& mode : conn.modes) {
            LOG_INFO("{}{}", mode.is_preferred() ? "* " : "  ", describe_mode(mode));
        }
    }

    dump_properties(dev, conn.props, "connector");
}

void dump_encoder(const Device& dev, const Encoder& enc) {
    std::string crtc_list;
    for (const auto& crtc : dev.crtcs()) {
        if (enc.possible_crtcs.contains(crtc.index)) {
            if (! crtc_list.empty()) {
                crtc_list += ", ";
            }
            crtc_list += to_string(crtc.id);
        }
    }
    LOG_INFO("encoder {} type={} possible_crtcs={} -> [{}]", to_string(enc.id),
             encoder_type_name(enc.type), to_string(enc.possible_crtcs),
             crtc_list.empty() ? "none" : crtc_list);
}

void dump_crtc(const Device& dev, const Crtc& crtc) {
    LOG_INFO("crtc {} {} active={}", to_string(crtc.id), to_string(crtc.index), crtc.active);
    LOG_SCOPE();
    dump_properties(dev, crtc.props, "crtc");
}

void dump_plane(const Device& dev, const Plane& plane) {
    std::string crtc_list;
    for (const auto& crtc : dev.crtcs()) {
        if (plane.possible_crtcs.contains(crtc.index)) {
            if (! crtc_list.empty()) {
                crtc_list += ", ";
            }
            crtc_list += to_string(crtc.id);
        }
    }

    LOG_INFO("plane {} type={} possible_crtcs={} -> [{}]", to_string(plane.id),
             to_string(plane.type), to_string(plane.possible_crtcs),
             crtc_list.empty() ? "none" : crtc_list);
    LOG_SCOPE();

    if (plane.current_crtc != kNoCrtc) {
        LOG_INFO("currently bound to {}", to_string(plane.current_crtc));
    }

    // 按 format 分组打印它支持的 modifier 集合。
    // 平铺打 (format, modifier) 对在有几十个格式时会刷屏几百行，
    // 分组之后一眼能看出"哪些格式有私有 modifier、哪些只有 LINEAR"
    // —— 勘察结果里 vsdrm 只有 XR24/XR30/AR24/AR30 有私有 modifier，
    // YUV 格式全是 LINEAR，这个结论就是这么读出来的。
    std::map<uint32_t, std::vector<Modifier>> by_format;
    for (const auto& pair : plane.formats) {
        by_format[static_cast<uint32_t>(pair.format)].push_back(pair.modifier);
    }

    {
        const auto mods = distinct_modifier_list(plane);
        LOG_INFO("distinct modifiers ({}):", mods.size());
        LOG_SCOPE();
        for (const uint64_t raw_mod : mods) {
            LOG_INFO("{}", describe_modifier(Modifier{raw_mod}));
        }
    }

    LOG_INFO("formats ({} pairs across {} formats):", plane.formats.size(), by_format.size());
    {
        LOG_SCOPE();
        for (auto& [fourcc, modifiers] : by_format) {
            std::sort(modifiers.begin(), modifiers.end());
            std::string list;
            for (const Modifier mod : modifiers) {
                if (! list.empty()) {
                    list += ", ";
                }
                list += describe_modifier(mod);
            }
            LOG_INFO("{:>6}  {}", to_string(Format{fourcc}), list);
        }
    }

    dump_properties(dev, plane.props, "plane");
}

void dump_topology(const Device& dev) {
    LOG_INFO("=== topology (connector -> encoder -> crtc) ===");
    LOG_SCOPE();
    LOG_INFO("possible_crtcs is a bitmask over the CRTC *array index*, not the CRTC id;");
    LOG_INFO("the arrow below is that bitmask already expanded into real CRTC ids.");

    for (const auto& crtc : dev.crtcs()) {
        LOG_INFO("crtc index {} = {}", to_string(crtc.index), to_string(crtc.id));
    }

    for (const auto& conn : dev.connectors()) {
        LOG_INFO("{} '{}' {}", to_string(conn.id), conn.name,
                 conn.connected ? "connected" : "disconnected");
        LOG_SCOPE();
        if (conn.encoders.empty()) {
            LOG_INFO("no encoders");
            continue;
        }
        for (const EncoderId enc_id : conn.encoders) {
            const Encoder* enc = dev.encoder(enc_id);
            if (enc == nullptr) {
                LOG_INFO("{} (not in snapshot)", to_string(enc_id));
                continue;
            }
            dump_encoder(dev, *enc);
        }
    }

    LOG_INFO("planes by CRTC:");
    {
        LOG_SCOPE();
        for (const auto& crtc : dev.crtcs()) {
            for (const PlaneType type :
                 {PlaneType::Primary, PlaneType::Overlay, PlaneType::Cursor}) {
                const auto ids = dev.planes_for_crtc(crtc.id, type);
                std::string list;
                for (const PlaneId id : ids) {
                    if (! list.empty()) {
                        list += ", ";
                    }
                    list += to_string(id);
                }
                LOG_INFO("{} {:<8} [{}]", to_string(crtc.id), to_string(type),
                         list.empty() ? "none" : list);
            }
        }
    }
}


// ---------------------------------------------------------------------------
// IN_FORMATS 原始结构 dump
// ---------------------------------------------------------------------------

void dump_in_formats_raw(const Device& dev, const Plane& plane) {
    LOG_INFO("plane {} IN_FORMATS raw blob", to_string(plane.id));
    LOG_SCOPE();

    const auto info = plane.props.find("IN_FORMATS");
    if (! info) {
        LOG_INFO("this plane has no IN_FORMATS property");
        return;
    }
    if (info->value == 0u) {
        LOG_INFO("IN_FORMATS property exists but the blob id is 0");
        return;
    }

    auto blob_result = BlobView::get(dev.fd(), BlobId{static_cast<uint32_t>(info->value)});
    if (! blob_result) {
        LOG_ERROR("cannot read the blob");
        return;
    }
    const BlobView blob = std::move(blob_result).value();
    const size_t length = blob.size();

    LOG_INFO("blob id={} length={} bytes", info->value, length);

    if (length < sizeof(drm_format_modifier_blob)) {
        LOG_ERROR("blob is shorter than the header ({} bytes)", sizeof(drm_format_modifier_blob));
        return;
    }

    const auto* base = static_cast<const uint8_t*>(blob.data());
    const auto* header = reinterpret_cast<const drm_format_modifier_blob*>(base);

    LOG_INFO("header:");
    {
        LOG_SCOPE();
        LOG_INFO("version           = {} (expected {})", header->version, FORMAT_BLOB_CURRENT);
        LOG_INFO("flags             = 0x{:x}", header->flags);
        LOG_INFO("count_formats     = {}", header->count_formats);
        LOG_INFO("formats_offset    = {}", header->formats_offset);
        LOG_INFO("count_modifiers   = {}", header->count_modifiers);
        LOG_INFO("modifiers_offset  = {}", header->modifiers_offset);
        LOG_INFO("sizeof(drm_format_modifier) = {} (must be 24)", sizeof(drm_format_modifier));
    }

    size_t problems = 0;
    const auto require = [&problems](bool ok, const std::string& what) {
        if (ok) {
            LOG_INFO("OK    {}", what);
        } else {
            ++problems;
            LOG_ERROR("BAD   {}", what);
        }
    };

    require(header->version == FORMAT_BLOB_CURRENT, "blob version matches FORMAT_BLOB_CURRENT");

    const size_t formats_end = static_cast<size_t>(header->formats_offset) +
                               static_cast<size_t>(header->count_formats) * 4u;
    const size_t modifiers_end = static_cast<size_t>(header->modifiers_offset) +
                                 static_cast<size_t>(header->count_modifiers) *
                                     sizeof(drm_format_modifier);
    require(formats_end <= length, fmt("format array fits in the blob ({} <= {})", formats_end,
                                       length));
    require(modifiers_end <= length,
            fmt("modifier array fits in the blob ({} <= {})", modifiers_end, length));

    if (formats_end > length || modifiers_end > length) {
        LOG_ERROR("offsets are out of range, stopping here");
        return;
    }

    const auto* formats = reinterpret_cast<const uint32_t*>(base + header->formats_offset);
    const auto* modifiers =
        reinterpret_cast<const drm_format_modifier*>(base + header->modifiers_offset);

    // 格式数组
    LOG_INFO("formats ({}):", header->count_formats);
    {
        LOG_SCOPE();
        std::string line;
        for (uint32_t i = 0; i < header->count_formats; ++i) {
            if (! line.empty()) {
                line += " ";
            }
            line += fmt("[{}]{}", i, to_string(Format{formats[i]}));
            if ((i % 8u) == 7u) {
                LOG_INFO("{}", line);
                line.clear();
            }
        }
        if (! line.empty()) {
            LOG_INFO("{}", line);
        }
    }

    // 每条 modifier 记录
    LOG_INFO("modifier records ({}):", header->count_modifiers);
    uint64_t total_bits = 0;
    bool offsets_sane = true;
    {
        LOG_SCOPE();
        for (uint32_t m = 0; m < header->count_modifiers; ++m) {
            const drm_format_modifier& entry = modifiers[m];

            uint32_t bits = 0;
            for (uint32_t bit = 0; bit < 64u; ++bit) {
                if (((entry.formats >> bit) & 1ULL) != 0ULL) {
                    ++bits;
                }
            }
            total_bits += bits;

            // 位图第 0 位对应 formats[offset]，所以 offset 之后必须还有格式。
            // offset 超过 count_formats 说明我们对这个字段的理解是错的。
            const bool offset_ok = entry.offset < header->count_formats;
            if (! offset_ok) {
                offsets_sane = false;
            }

            LOG_INFO("[{:<2}] modifier=0x{:016x} offset={:<3} bitmask=0x{:016x} bits={} {} {}", m,
                     entry.modifier, entry.offset, entry.formats, bits,
                     describe_modifier(Modifier{entry.modifier}),
                     offset_ok ? "" : "  <-- offset >= count_formats");
        }
    }

    require(offsets_sane, "every record's offset is within the format array");

    // 最关键的一条：位图里置位的总数应该正好等于我们解析出的 (format, modifier)
    // 对数。对不上说明位图 / offset 的语义理解错了。
    const size_t parsed_pairs = plane.formats.size();
    require(total_bits == parsed_pairs,
            fmt("popcount over all bitmasks ({}) equals the parsed pair count ({})", total_bits,
                parsed_pairs));

    if (problems == 0) {
        LOG_INFO("blob is internally consistent; the parse can be trusted without modetest");
    } else {
        LOG_ERROR("{} consistency problem(s); the IN_FORMATS parse is suspect", problems);
    }
}

// ---------------------------------------------------------------------------
// render node 的同步能力探测
// ---------------------------------------------------------------------------

Result<RenderSyncCaps> probe_render_node_sync_caps(const std::string& render_path) {
    auto fd = TRY(UniqueFd::open(render_path.c_str(), O_RDWR));

    RenderSyncCaps out;
    if (auto* version = drmGetVersion(fd.get()); version != nullptr) {
        if (version->name != nullptr && version->name_len > 0) {
            out.driver_name.assign(version->name, static_cast<size_t>(version->name_len));
        }
        drmFreeVersion(version);
    }

    uint64_t value = 0;
    if (drmGetCap(fd.get(), DRM_CAP_SYNCOBJ, &value) == 0) {
        out.syncobj = value != 0u;
    }
    value = 0;
    if (drmGetCap(fd.get(), DRM_CAP_SYNCOBJ_TIMELINE, &value) == 0) {
        out.syncobj_timeline = value != 0u;
    }
    value = 0;
    if (drmGetCap(fd.get(), DRM_CAP_PRIME, &value) == 0) {
        out.prime_import = (value & DRM_PRIME_CAP_IMPORT) != 0u;
        out.prime_export = (value & DRM_PRIME_CAP_EXPORT) != 0u;
    }
    return Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// 紧凑摘要
// ---------------------------------------------------------------------------

namespace {

std::vector<uint64_t> distinct_modifier_list(const Plane& plane) {
    std::vector<uint64_t> seen;
    for (const auto& pair : plane.formats) {
        const auto raw_mod = static_cast<uint64_t>(pair.modifier);
        if (std::find(seen.begin(), seen.end(), raw_mod) == seen.end()) {
            seen.push_back(raw_mod);
        }
    }
    std::sort(seen.begin(), seen.end());
    return seen;
}

/// 一个 plane 支持的去重 modifier 集合，摘要里**截断**。
/// 真机上一个 plane 可能有十几个私有 modifier，全打出来一行上千字符，
/// 摘要就不摘要了。全量列表用 -v。
std::string distinct_modifiers(const Plane& plane, size_t limit) {
    const auto seen = distinct_modifier_list(plane);

    std::string out;
    size_t shown = 0;
    for (const uint64_t raw_mod : seen) {
        if (shown == limit) {
            out += ",+" + std::to_string(seen.size() - shown) + " more";
            break;
        }
        if (! out.empty()) {
            out += ",";
        }
        out += describe_modifier(Modifier{raw_mod});
        ++shown;
    }
    return out.empty() ? "-" : out;
}

/// 去重后的 format 个数（IN_FORMATS 展开后同一个 format 会出现多次）
size_t distinct_format_count(const Plane& plane) {
    std::vector<uint32_t> seen;
    for (const auto& pair : plane.formats) {
        const auto code = static_cast<uint32_t>(pair.format);
        if (std::find(seen.begin(), seen.end(), code) == seen.end()) {
            seen.push_back(code);
        }
    }
    return seen.size();
}

const char* yn(bool value) noexcept {
    return value ? "yes" : "no";
}

} // namespace

void dump_summary(const Device& dev) {
    const DeviceCaps& caps = dev.caps();

    LOG_INFO("device {} driver={} v{}.{}", dev.path(), caps.driver_name, caps.version_major,
             caps.version_minor);
    LOG_SCOPE();

    LOG_INFO("caps   atomic={} universal_planes={} dumb={} prime_import={} prime_export={}",
             yn(caps.atomic), yn(caps.universal_planes), yn(caps.dumb_buffer),
             yn(caps.prime_import), yn(caps.prime_export));
    LOG_INFO("       addfb2_modifiers={} monotonic_ts={} syncobj={} syncobj_timeline={}",
             yn(caps.addfb2_modifiers), yn(caps.timestamp_monotonic), yn(caps.syncobj),
             yn(caps.syncobj_timeline));
    LOG_INFO("       cursor={}x{} writeback_connector={}", caps.cursor_width, caps.cursor_height,
             yn(caps.has_writeback_connector));
    LOG_INFO("props  crtc: OUT_FENCE_PTR={} VRR_ENABLED={}", yn(caps.prop_crtc_out_fence_ptr),
             yn(caps.prop_crtc_vrr_enabled));
    LOG_INFO("       plane: IN_FENCE_FD={} IN_FORMATS={} zpos={} alpha={} blend={}",
             yn(caps.prop_plane_in_fence_fd), yn(caps.prop_plane_in_formats),
             yn(caps.prop_plane_zpos), yn(caps.prop_plane_alpha),
             yn(caps.prop_plane_pixel_blend_mode));

    LOG_INFO("crtcs ({}):", dev.crtcs().size());
    {
        LOG_SCOPE();
        for (const auto& crtc : dev.crtcs()) {
            LOG_INFO("{} {} active={}", to_string(crtc.index), to_string(crtc.id), crtc.active);
        }
    }

    LOG_INFO("connectors ({}):", dev.connectors().size());
    {
        LOG_SCOPE();
        for (const auto& conn : dev.connectors()) {
            // encoder 那一列直接把 possible_crtcs 展开成真实 CRTC id，
            // 因为"位图下标不是 crtc id"正是最容易搞错的地方。
            std::string encoders;
            for (const EncoderId enc_id : conn.encoders) {
                if (! encoders.empty()) {
                    encoders += " ";
                }
                encoders += to_string(enc_id);
                if (const Encoder* enc = dev.encoder(enc_id); enc != nullptr) {
                    encoders += "(";
                    encoders += encoder_type_name(enc->type);
                    encoders += " ";
                    std::string crtc_ids;
                    for (const auto& crtc : dev.crtcs()) {
                        if (enc->possible_crtcs.contains(crtc.index)) {
                            if (! crtc_ids.empty()) {
                                crtc_ids += ",";
                            }
                            crtc_ids += std::to_string(raw(crtc.id));
                        }
                    }
                    encoders += "->[" + (crtc_ids.empty() ? std::string("-") : crtc_ids) + "]";
                    encoders += ")";
                }
            }

            const ModeInfo* preferred = conn.preferred_mode();
            LOG_INFO("{:<10} {:<12} {:<13} modes={:<4} {:<20} enc=[{}]", to_string(conn.id),
                     conn.name, conn.connected ? "connected" : "disconnected", conn.modes.size(),
                     preferred != nullptr ? preferred->name() : std::string("-"),
                     encoders.empty() ? "-" : encoders);
        }
    }

    LOG_INFO("planes ({}):", dev.planes().size());
    {
        LOG_SCOPE();
        for (const auto& plane : dev.planes()) {
            std::string crtc_ids;
            for (const auto& crtc : dev.crtcs()) {
                if (plane.possible_crtcs.contains(crtc.index)) {
                    if (! crtc_ids.empty()) {
                        crtc_ids += ",";
                    }
                    crtc_ids += std::to_string(raw(crtc.id));
                }
            }
            const size_t mod_count = distinct_modifier_list(plane).size();
            LOG_INFO("{:<11} {:<8} crtcs->[{}] formats={:<3} pairs={:<4} mods={:<3} {}",
                     to_string(plane.id), to_string(plane.type),
                     crtc_ids.empty() ? std::string("-") : crtc_ids, distinct_format_count(plane),
                     plane.formats.size(), mod_count, distinct_modifiers(plane, 3));
        }
    }
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------

namespace {

size_t g_failures = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        LOG_INFO("PASS  {}", what);
    } else {
        ++g_failures;
        LOG_ERROR("FAIL  {}", what);
    }
}

void note(bool ok, const std::string& what) {
    // 不是错误，但值得知道 —— 比如 VKMS 没有 cursor plane。
    LOG_INFO("{}  {}", ok ? "PASS " : "WARN ", what);
}

} // namespace

size_t run_self_checks(const Device& dev) {
    g_failures = 0;

    LOG_INFO("=== self checks ===");
    LOG_SCOPE();

    const DeviceCaps& caps = dev.caps();

    // ---- 本工程的硬性前提 ----
    check(caps.atomic, "atomic modesetting is available");
    check(caps.universal_planes, "universal planes are available");
    check(! dev.crtcs().empty(), "device has at least one CRTC");
    check(! dev.planes().empty(), "device has at least one plane");

    // ---- CrtcIndex 与 CrtcId 的映射是双射 ----
    // 这条错了，所有 possible_crtcs 匹配都会静默地选错 CRTC。
    {
        bool bijective = true;
        for (const auto& crtc : dev.crtcs()) {
            const auto index = dev.crtc_index_of(crtc.id);
            const auto back = index ? dev.crtc_at(*index) : std::nullopt;
            if (! index || ! back || *back != crtc.id) {
                bijective = false;
                break;
            }
        }
        check(bijective, "CrtcId <-> CrtcIndex round-trips for every CRTC");
    }

    // ---- 每个 CRTC 至少有一个 primary plane ----
    // 没有 primary 就没法点屏。这条在 VKMS 和真硬件上都必须成立。
    {
        bool all_have_primary = true;
        for (const auto& crtc : dev.crtcs()) {
            const auto primaries = dev.planes_for_crtc(crtc.id, PlaneType::Primary);
            if (primaries.empty()) {
                LOG_WARN("  {} has no primary plane", to_string(crtc.id));
                all_have_primary = false;
            }
        }
        check(all_have_primary, "every CRTC has at least one primary plane");
    }

    // ---- plane 类型分布 ----
    {
        size_t primary = 0;
        size_t overlay = 0;
        size_t cursor = 0;
        for (const auto& plane : dev.planes()) {
            switch (plane.type) {
                case PlaneType::Primary: ++primary; break;
                case PlaneType::Overlay: ++overlay; break;
                case PlaneType::Cursor:  ++cursor; break;
            }
        }
        LOG_INFO("      plane types: {} primary, {} overlay, {} cursor", primary, overlay, cursor);
        note(cursor > 0, "hardware cursor plane present (absent on VKMS, Step 5 will degrade)");
        note(overlay > 0, "overlay plane present (needed for Step 5 multi-plane offload)");
    }

    // ---- connected 的 connector 必须有可用 encoder ----
    {
        bool ok = true;
        size_t usable = 0;
        for (const auto& conn : dev.connectors()) {
            if (! conn.connected || conn.is_writeback) {
                continue;
            }
            ++usable;
            if (conn.modes.empty()) {
                LOG_WARN("  {} ({}) is connected but reports no modes", to_string(conn.id),
                         conn.name);
                ok = false;
                continue;
            }
            bool has_crtc = false;
            for (const EncoderId enc_id : conn.encoders) {
                const Encoder* enc = dev.encoder(enc_id);
                if (enc != nullptr && ! enc->possible_crtcs.empty()) {
                    has_crtc = true;
                    break;
                }
            }
            if (! has_crtc) {
                LOG_WARN("  {} ({}) has no encoder with a non-empty possible_crtcs",
                         to_string(conn.id), conn.name);
                ok = false;
            }
        }
        check(usable > 0, "at least one connected non-writeback connector");
        check(ok, "every connected connector has modes and a usable encoder");
    }

    // ---- pick_output 能跑通 ----
    // 这是 Step 1 真正依赖的东西：拿到它就能构造 atomic request 了。
    {
        auto path_result = dev.pick_output();
        if (! path_result) {
            ++g_failures;
            LOG_ERROR("FAIL  pick_output() succeeds");
            log_error_object(path_result.error(), "      pick_output");
        } else {
            const OutputPath path = std::move(path_result).value();
            LOG_INFO("PASS  pick_output() succeeds");
            LOG_INFO("      -> {} {} {} {} @ {}", to_string(path.connector), to_string(path.crtc),
                     to_string(path.crtc_index), to_string(path.primary_plane), path.mode.name());
            LOG_INFO("      -> frame duration {} us", path.mode.frame_duration_ns() / 1000u);

            // 选中的 primary plane 必须支持 XR24 —— Step 1 的 dumb buffer
            // 只画 32bpp XRGB，不支持就点不亮。
            const Plane* plane = dev.plane(path.primary_plane);
            check(plane != nullptr && plane->supports_format(Format{0x34325258u}),
                  "the chosen primary plane supports XR24 (needed by the Step 1 dumb buffer)");

            // generation 校验：刚算出来的路径当然应该有效
            const auto valid = dev.validate(path);
            check(static_cast<bool>(valid), "the freshly computed OutputPath validates");
        }
    }

    // ---- 显式同步：KMS 侧和渲染侧要分开看 ----
    //
    // KMS 侧的显式同步只需要 sync_file fd（IN_FENCE_FD / OUT_FENCE_PTR 属性），
    // 跟 DRM syncobj 无关。syncobj 是**渲染侧**特性（驱动的 DRIVER_SYNCOBJ），
    // KMS 节点不 advertise 它是正常的 —— 真机上 KMS 节点和 render node
    // 往往是同一个 PCI 设备的两个 DRM 节点，能力集不同。
    //
    // 所以 Step 6 会拆成两半：
    //   - KMS 提交侧：IN_FENCE_FD / OUT_FENCE_PTR，只要属性在就能做
    //   - linux-drm-syncobj-v1 协议侧：要 render node 的 timeline syncobj
    note(caps.prop_plane_in_fence_fd == caps.prop_crtc_out_fence_ptr,
         "explicit sync properties are consistent (IN_FENCE_FD matches OUT_FENCE_PTR)");
    note(caps.prop_plane_in_fence_fd && caps.prop_crtc_out_fence_ptr,
         "KMS-side explicit sync usable (sync_file based, independent of syncobj)");

    if (! caps.syncobj || ! caps.syncobj_timeline) {
        LOG_INFO("      the KMS node reports syncobj={} timeline={}; syncobj is a render-side",
                 caps.syncobj, caps.syncobj_timeline);
        LOG_INFO("      feature, so check the render node before concluding Step 6 is blocked");
        const auto render_path = find_render_node(dev.path());
        if (! render_path) {
            note(false, "no render node paired with this KMS node (fine on VKMS)");
        } else {
            auto probe = probe_render_node_sync_caps(*render_path);
            if (! probe) {
                note(false, fmt("render node {} could not be probed", *render_path));
            } else {
                const RenderSyncCaps rc = std::move(probe).value();
                LOG_INFO("      render node {} driver='{}' syncobj={} timeline={}", *render_path,
                         rc.driver_name, rc.syncobj, rc.syncobj_timeline);
                note(rc.syncobj_timeline,
                     "timeline syncobj available on the render node (needed by "
                     "linux-drm-syncobj-v1)");
            }
        }
    }

    if (g_failures == 0) {
        LOG_INFO("all checks passed");
    } else {
        LOG_ERROR("{} check(s) failed", g_failures);
    }
    return g_failures;
}

void dump_device(const Device& dev) {
    LOG_INFO("=== device {} ===", dev.path());
    {
        LOG_SCOPE();
        LOG_INFO("{}", dev.caps().to_string());
    }

    LOG_INFO("=== connectors ({}) ===", dev.connectors().size());
    {
        LOG_SCOPE();
        for (const auto& conn : dev.connectors()) {
            dump_connector(dev, conn);
        }
    }

    LOG_INFO("=== crtcs ({}) ===", dev.crtcs().size());
    {
        LOG_SCOPE();
        for (const auto& crtc : dev.crtcs()) {
            dump_crtc(dev, crtc);
        }
    }

    LOG_INFO("=== planes ({}) ===", dev.planes().size());
    {
        LOG_SCOPE();
        for (const auto& plane : dev.planes()) {
            dump_plane(dev, plane);
        }
    }

    dump_topology(dev);
}

} // namespace mw::drm
