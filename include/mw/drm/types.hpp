/**
 * @file drm/types.hpp
 * @brief KMS 对象 ID、几何量、格式/修饰符的强类型表达
 *
 * 这个头文件的全部目的是：**让 KMS 里几类最容易混淆的裸整数在编译期分开**。
 *
 *  1. 各种 object id 都是 uint32_t —— plane_id 传成 crtc_id 编译期无感知，
 *     运行期表现为内核返回 EINVAL，非常难查。
 *  2. `SRC_*` 是 16.16 定点，`CRTC_*` 是普通整数。这是 atomic 新手第一大坑。
 *     这里用两个**不同的结构体**（SrcRect / CrtcRect）承载，移位由 Fixed16
 *     内部完成，调用方拿不到"裸的没移位的数"。
 *  3. `possible_crtcs` 是**按 CRTC 在资源数组里的下标**做的位图，
 *     不是按 crtc_id。CrtcIndex 与 CrtcId 是两个类型，不能互相赋值。
 *
 * 关于 Modifier：见 docs 里的"modifier 不透明原则"。本工程**任何主逻辑
 * 都不解析 modifier 的语义**，它只是一个从 IN_FORMATS 读出来、原样转发给
 * client、再原样交回内核的 token。唯一允许做 vendor 判断的地方是日志。
 */
#pragma once

#include <cstdint>
#include <string>

namespace mw::drm {

// ---------------------------------------------------------------------------
// 对象 ID
// ---------------------------------------------------------------------------
// DRM 里 id == 0 表示"无"（例如 plane 的 CRTC_ID 置 0 即 disable）。

enum class ConnectorId : uint32_t {};
enum class EncoderId : uint32_t {};
enum class CrtcId : uint32_t {};
enum class PlaneId : uint32_t {};
enum class PropertyId : uint32_t {};
enum class BlobId : uint32_t {};
enum class FbId : uint32_t {};
enum class GemHandle : uint32_t {};

inline constexpr ConnectorId kNoConnector{0};
inline constexpr CrtcId kNoCrtc{0};
inline constexpr PlaneId kNoPlane{0};
inline constexpr PropertyId kNoProperty{0};
inline constexpr BlobId kNoBlob{0};
inline constexpr FbId kNoFb{0};

template <typename Id>
constexpr uint32_t raw(Id id) noexcept {
    return static_cast<uint32_t>(id);
}

// ---------------------------------------------------------------------------
// CRTC 下标与 possible_crtcs 位图
// ---------------------------------------------------------------------------

/**
 * @brief CRTC 在 drmModeRes::crtcs[] 里的下标（不是 crtc_id！）
 *
 * encoder->possible_crtcs / plane->possible_crtcs 的第 n 位对应
 * drmModeRes::crtcs[n]。勘察结果里 encoder 146(DSI) 的 possible_crtcs
 * 是 0x3（两个 CRTC 都能挂），encoder 141(HDMI) 是 0x2 —— 说明"第一个
 * connector 配第一个 CRTC"这种写法在本硬件上直接就是错的。
 */
enum class CrtcIndex : uint32_t {};

class PossibleCrtcs {
  public:
    constexpr PossibleCrtcs() noexcept = default;
    constexpr explicit PossibleCrtcs(uint32_t mask) noexcept : mask_(mask) {}

    constexpr bool contains(CrtcIndex idx) const noexcept {
        const uint32_t bit = static_cast<uint32_t>(idx);
        return bit < 32u && ((mask_ >> bit) & 1u) != 0u;
    }

    constexpr bool empty() const noexcept {
        return mask_ == 0u;
    }

    constexpr uint32_t mask() const noexcept {
        return mask_;
    }

  private:
    uint32_t mask_ = 0;
};

// ---------------------------------------------------------------------------
// 16.16 定点
// ---------------------------------------------------------------------------

/**
 * @brief 16.16 定点数
 *
 * 只能通过 from_int / from_raw 构造，`raw()` 出来的就是可以直接塞进
 * SRC_X/Y/W/H 的值。工程里不允许出现手写的 `<< 16`。
 */
class Fixed16 {
  public:
    constexpr Fixed16() noexcept = default;

    static constexpr Fixed16 from_int(uint32_t v) noexcept {
        return Fixed16(static_cast<uint64_t>(v) << 16);
    }

    /// 已经是 16.16 形式的原始值（例如从内核读回来的属性值）
    static constexpr Fixed16 from_raw(uint64_t raw_value) noexcept {
        return Fixed16(raw_value);
    }

    constexpr uint64_t raw() const noexcept {
        return raw_;
    }

    /// 截断到整数部分，仅用于日志
    constexpr uint32_t truncated() const noexcept {
        return static_cast<uint32_t>(raw_ >> 16);
    }

  private:
    constexpr explicit Fixed16(uint64_t r) noexcept : raw_(r) {}
    uint64_t raw_ = 0;
};

// ---------------------------------------------------------------------------
// 几何
// ---------------------------------------------------------------------------

struct Size {
    uint32_t width = 0;
    uint32_t height = 0;

    constexpr bool operator==(const Size& o) const noexcept {
        return width == o.width && height == o.height;
    }
    constexpr bool operator!=(const Size& o) const noexcept {
        return ! (*this == o);
    }
    constexpr bool empty() const noexcept {
        return width == 0 || height == 0;
    }
};

/// plane 的 SRC_X/Y/W/H —— **16.16 定点**，单位是 buffer 内像素
struct SrcRect {
    Fixed16 x;
    Fixed16 y;
    Fixed16 width;
    Fixed16 height;

    /// 最常见的用法：整帧取样
    static constexpr SrcRect whole(Size size) noexcept {
        return SrcRect{Fixed16{}, Fixed16{}, Fixed16::from_int(size.width),
                       Fixed16::from_int(size.height)};
    }
};

/// plane 的 CRTC_X/Y/W/H —— **普通整数**，单位是屏幕像素。x/y 可为负。
struct CrtcRect {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    static constexpr CrtcRect at_origin(Size size) noexcept {
        return CrtcRect{0, 0, size.width, size.height};
    }
};

// ---------------------------------------------------------------------------
// plane 类型
// ---------------------------------------------------------------------------

enum class PlaneType {
    Primary,
    Overlay,
    Cursor,
};

// ---------------------------------------------------------------------------
// 像素格式 / 修饰符（均为不透明 token）
// ---------------------------------------------------------------------------

/// DRM fourcc。不解析，仅比较与转发；日志里转成 4 个字符。
enum class Format : uint32_t {};

/**
 * @brief 四个字符拼成一个 fourcc
 *
 * 等价于 libdrm 的 `fourcc_code()`，自己写一遍有两个理由：
 * 一是这个头文件因此不必 include `<drm_fourcc.h>`（那个宏体是 C 风格强转，
 * 只能靠 `-isystem` 压住告警，不该出现在最基础的类型头里）；
 * 二是把"fourcc 就是四个 ASCII 字符按小端拼出来的 32 位数"这件事写在代码里，
 * 比记住一串十六进制常量有用。
 */
constexpr Format fourcc(char a, char b, char c, char d) noexcept {
    return Format{static_cast<uint32_t>(static_cast<unsigned char>(a)) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16) |
                  (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24)};
}

// 只列工程当前真正用到的几个。**不要**在这里堆一张完整格式表 ——
// 那张表属于将来某个真的需要遍历格式的模块，不属于类型定义。
inline constexpr Format kFormatXrgb8888 = fourcc('X', 'R', '2', '4');
inline constexpr Format kFormatArgb8888 = fourcc('A', 'R', '2', '4');
inline constexpr Format kFormatXbgr8888 = fourcc('X', 'B', '2', '4');
inline constexpr Format kFormatAbgr8888 = fourcc('A', 'B', '2', '4');
inline constexpr Format kFormatRgb565 = fourcc('R', 'G', '1', '6');

/**
 * @brief 单像素字节数，**仅对单平面 packed 格式有意义**
 *
 * 认识的格式返回 4 / 2 之类，不认识的返回 0。返回 0 不代表格式非法，
 * 只代表"这个 API 表达不了它"——多平面格式（NV12 等）的每个平面
 * 各有各的 bpp 与 stride，一个标量回答不了。
 *
 * 存在的理由：CPU 往 buffer 里写像素时必须知道一个像素几个字节。
 * 没有这个函数，每个调用方都会自己写一句 `stride / width` 或者写死 4，
 * 前者在有 padding 的 stride 上是错的，后者在换格式时是错的。
 *
 * @warning 定位像素**只能用 buffer 自己报的 stride**，
 *          绝不能用 `width * bytes_per_pixel()` —— 两者常常不相等。
 */
uint32_t bytes_per_pixel(Format format) noexcept;

/**
 * @brief DRM format modifier。**不透明**。
 *
 * 禁止在主逻辑里出现任何形如 `(mod >> 56) == 0x0b` 的 vendor 判断。
 * 合成器只做三件事：从 IN_FORMATS 读出来 → 按 tranche 转发给 client →
 * client 回传后原样交给 addfb2，失败就降级。
 */
enum class Modifier : uint64_t {};

inline constexpr Modifier kModifierLinear{0};

/// DRM_FORMAT_MOD_INVALID：表示"没有 modifier 信息"，语义上不等于 LINEAR
inline constexpr Modifier kModifierInvalid{0x00ffffffffffffffULL};

struct FormatModifier {
    Format format{};
    Modifier modifier = kModifierLinear;

    constexpr bool operator==(const FormatModifier& o) const noexcept {
        return format == o.format && modifier == o.modifier;
    }
};

// ---------------------------------------------------------------------------
// to_string —— 供 fmt() 通过 ADL 找到
// ---------------------------------------------------------------------------
// 必须是**非模板**重载：写成 `template<class E, enable_if is_enum>` 会和
// format.hpp 里的 fallback 模板产生二义（实测 GCC 13 ambiguous）。

std::string to_string(ConnectorId id);
std::string to_string(EncoderId id);
std::string to_string(CrtcId id);
std::string to_string(PlaneId id);
std::string to_string(PropertyId id);
std::string to_string(BlobId id);
std::string to_string(FbId id);
std::string to_string(GemHandle h);
std::string to_string(CrtcIndex idx);
std::string to_string(PossibleCrtcs mask);
std::string to_string(Fixed16 v);
std::string to_string(Size s);
std::string to_string(const SrcRect& r);
std::string to_string(const CrtcRect& r);
std::string to_string(PlaneType t);

/// fourcc 的 4 个可打印字符，如 "XR24"
std::string to_string(Format f);

/// **只输出十六进制原值**，不做 vendor 解码。
/// 需要人类可读的 vendor 名请用 debug/ 下的独立工具，主逻辑不得依赖。
std::string to_string(Modifier m);

std::string to_string(const FormatModifier& fm);

} // namespace mw::drm
