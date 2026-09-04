#include "mw/drm/types.hpp"

#include <cstdio>

namespace mw::drm {
namespace {

/// "conn#142" 这种前缀形式。日志里一眼能分辨是哪类对象，
/// 这正是引入强类型的目的之一 —— 让类型信息一路带到输出。
std::string tagged(const char* tag, uint32_t value) {
    std::string out;
    out.reserve(12);
    out += tag;
    out += '#';
    out += std::to_string(value);
    return out;
}

} // namespace

std::string to_string(ConnectorId id) {
    return tagged("conn", static_cast<uint32_t>(id));
}

std::string to_string(EncoderId id) {
    return tagged("enc", static_cast<uint32_t>(id));
}

std::string to_string(CrtcId id) {
    return tagged("crtc", static_cast<uint32_t>(id));
}

std::string to_string(PlaneId id) {
    return tagged("plane", static_cast<uint32_t>(id));
}

std::string to_string(PropertyId id) {
    return tagged("prop", static_cast<uint32_t>(id));
}

std::string to_string(BlobId id) {
    return tagged("blob", static_cast<uint32_t>(id));
}

std::string to_string(FbId id) {
    return tagged("fb", static_cast<uint32_t>(id));
}

std::string to_string(GemHandle h) {
    return tagged("gem", static_cast<uint32_t>(h));
}

std::string to_string(CrtcIndex idx) {
    // 故意用 "[n]" 而不是 "#n"：视觉上就和 crtc id 区分开，
    // 看日志时不会把下标误读成 id。
    std::string out;
    out += '[';
    out += std::to_string(static_cast<uint32_t>(idx));
    out += ']';
    return out;
}

std::string to_string(PossibleCrtcs mask) {
    // 同时打位图原值和展开的下标列表，方便和 modetest 的
    // "possible_crtcs=0x2" 对照。
    std::string out;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", mask.mask());
    out += buf;
    out += "->{";
    bool first = true;
    for (uint32_t bit = 0; bit < 32u; ++bit) {
        if (mask.contains(CrtcIndex{bit})) {
            if (! first) {
                out += ',';
            }
            first = false;
            out += std::to_string(bit);
        }
    }
    out += '}';
    return out;
}

std::string to_string(Fixed16 v) {
    // 打成 "1920.000(0x7800000)"：既能读整数部分，又能核对下发的原值。
    // 16.16 漏移位的症状就是原值小了 65536 倍，打出来一眼能看见。
    const uint64_t raw_value = v.raw();
    const uint32_t whole = static_cast<uint32_t>(raw_value >> 16);
    const uint32_t frac = static_cast<uint32_t>(raw_value & 0xffffu);

    char buf[48];
    // frac 是 1/65536 单位，乘 1000000/65536 转成微小数位
    const uint32_t micro = static_cast<uint32_t>((static_cast<uint64_t>(frac) * 1000000u) >> 16);
    std::snprintf(buf, sizeof(buf), "%u.%06u(0x%llx)", whole, micro,
                  static_cast<unsigned long long>(raw_value));
    return std::string(buf);
}

std::string to_string(Size s) {
    return std::to_string(s.width) + "x" + std::to_string(s.height);
}

std::string to_string(const SrcRect& r) {
    return "src{" + to_string(r.x) + "," + to_string(r.y) + " " + to_string(r.width) + "x" +
           to_string(r.height) + "}";
}

std::string to_string(const CrtcRect& r) {
    return "crtc{" + std::to_string(r.x) + "," + std::to_string(r.y) + " " +
           std::to_string(r.width) + "x" + std::to_string(r.height) + "}";
}

std::string to_string(PlaneType t) {
    switch (t) {
        case PlaneType::Primary: return "Primary";
        case PlaneType::Overlay: return "Overlay";
        case PlaneType::Cursor:  return "Cursor";
    }
    return "Unknown";
}

std::string to_string(Format f) {
    // fourcc 是 4 个 ASCII 字符打包成 u32，低字节在前。
    // 不可打印的字节退化成 '?'，并附上十六进制原值。
    const uint32_t code = static_cast<uint32_t>(f);
    char chars[5] = {0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < 4u; ++i) {
        const auto byte = static_cast<unsigned char>((code >> (i * 8u)) & 0xffu);
        chars[i] = (byte >= 0x20u && byte < 0x7fu) ? static_cast<char>(byte) : '?';
    }

    if (code == 0u) {
        return "FMT(0)";
    }
    return std::string(chars);
}

std::string to_string(Modifier m) {
    // **只打十六进制**，不做任何 vendor 解码。
    // 需要人类可读的 vendor 名请用 dump.hpp::describe_modifier()，
    // 那是唯一允许解码的地方，且主逻辑不得依赖。
    const uint64_t raw_value = static_cast<uint64_t>(m);
    if (m == kModifierLinear) {
        return "LINEAR";
    }
    if (m == kModifierInvalid) {
        return "INVALID";
    }
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(raw_value));
    return std::string(buf);
}

std::string to_string(const FormatModifier& fm) {
    return to_string(fm.format) + "/" + to_string(fm.modifier);
}

uint32_t bytes_per_pixel(Format format) noexcept {
    // 刻意只列已知在用的。加新格式时**顺手确认它确实是单平面 packed**，
    // 否则这个函数会给出一个看起来合理、实际会算错地址的答案。
    if (format == kFormatXrgb8888 || format == kFormatArgb8888 ||
        format == kFormatXbgr8888 || format == kFormatAbgr8888) {
        return 4u;
    }
    if (format == kFormatRgb565) {
        return 2u;
    }
    return 0u;
}

} // namespace mw::drm
