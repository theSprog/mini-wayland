#pragma once

#include <string>
#include <sstream>
#include <vector>
#include <chrono>
#include <type_traits>
#include <tuple>
#include <algorithm>
#include <iostream>

namespace internal {
namespace {

// 整数进制枚举
enum IntBase {
    Dec,
    HexLower,
    HexUpper,
    Bin
};

// 对齐方式
enum Align {
    AlignNone,
    AlignLeft,   // '<'
    AlignRight,  // '>'
    AlignCenter  // '^'
};

// 格式说明：对齐/宽度/填充 + 进制
struct FormatSpec {
    bool has_base;
    IntBase base;
    Align align;
    size_t width;
    char fill;

    FormatSpec() : has_base(false), base(Dec), align(AlignNone), width(0), fill(' ') {}
};

// 解析 "{:>26}" / "{:<8}" / "{:x}" / "{:*^10}" 里冒号后面的部分
//
// 语法（std::format 的一个子集）：
//     [[fill]align][width][type]
//   align := '<' | '>' | '^'
//   type  := 'x' | 'X' | 'b' | 'd'
//
// 支持宽度是因为这个项目的日志大量是表格状的（属性表、format 列表、
// caps 列表），要和 modetest 的输出逐列对照。没有对齐就没法读。
inline FormatSpec parse_spec(const std::string& spec) {
    FormatSpec fs;
    if (spec.empty() || spec[0] != ':') {
        return fs;
    }

    size_t i = 1;

    // [[fill]align]：第二个字符是对齐符时，第一个就是填充字符
    if (i + 1 < spec.size() &&
        (spec[i + 1] == '<' || spec[i + 1] == '>' || spec[i + 1] == '^')) {
        fs.fill = spec[i];
        ++i;
    }
    if (i < spec.size() && (spec[i] == '<' || spec[i] == '>' || spec[i] == '^')) {
        fs.align = spec[i] == '<' ? AlignLeft : (spec[i] == '>' ? AlignRight : AlignCenter);
        ++i;
    }

    // '0' 前缀：零填充 + 右对齐（std::format 的 {:08x} 惯用法）
    if (fs.align == AlignNone && i < spec.size() && spec[i] == '0') {
        fs.fill = '0';
        fs.align = AlignRight;
        ++i;
    }

    // [width]
    while (i < spec.size() && spec[i] >= '0' && spec[i] <= '9') {
        fs.width = fs.width * 10 + static_cast<size_t>(spec[i] - '0');
        ++i;
    }

    // [type]
    if (i < spec.size()) {
        switch (spec[i]) {
            case 'x': fs.has_base = true; fs.base = HexLower; break;
            case 'X': fs.has_base = true; fs.base = HexUpper; break;
            case 'b': fs.has_base = true; fs.base = Bin;      break;
            case 'd': fs.has_base = true; fs.base = Dec;      break;
            default: break;
        }
    }

    // 只给了宽度没给对齐时，默认左对齐（和 std::format 对非数值类型一致，
    // 这里不区分类型，统一左对齐更可预期）
    if (fs.align == AlignNone && fs.width > 0) {
        fs.align = AlignLeft;
    }
    return fs;
}

// 按 spec 补齐宽度
inline std::string pad_to_width(const std::string& text, const FormatSpec& fs) {
    if (fs.width == 0 || text.size() >= fs.width) {
        return text;
    }
    const size_t total = fs.width - text.size();
    switch (fs.align) {
        case AlignRight:
            return std::string(total, fs.fill) + text;
        case AlignCenter: {
            const size_t left = total / 2;
            return std::string(left, fs.fill) + text + std::string(total - left, fs.fill);
        }
        case AlignLeft:
        case AlignNone:
        default:
            return text + std::string(total, fs.fill);
    }
}

// 上报错误并返回错误字符串
inline std::string format_error(const std::string& msg) {
    return "<?>(" + msg + ")";
}

// 拆分 format 字符串为若干文本或 "{...}" 片段
inline std::vector<std::string> parse_format(const std::string& fmt) {
    std::vector<std::string> parts;
    std::string buf;
    const char* p = fmt.c_str();
    while (*p) {
        if (*p == '{') {
            if (p[1] == '{') {
                buf += '{';
                p += 2;
            } else {
                parts.push_back(buf);
                buf.clear();
                ++p;
                const char* start = p;
                while (*p && *p != '}') ++p;
                if (! *p) return {format_error("unmatched '{'")};
                parts.push_back("{" + std::string(start, p) + "}");
                ++p;
            }
        } else if (*p == '}') {
            if (p[1] == '}') {
                buf += '}';
                p += 2;
            } else {
                return {format_error("unmatched '}'")};
            }
        } else {
            buf += *p++;
        }
    }
    if (! buf.empty()) parts.push_back(buf);
    return parts;
}

// to_string 重载：vector
template <typename T>
std::string to_string(const std::vector<T>& vec);

inline std::string to_string(unsigned char v) {
    return std::to_string(static_cast<unsigned int>(v));
}

inline std::string to_string(signed char v) {
    return std::to_string(static_cast<int>(v));
}

// to_string 重载：chrono durations
inline std::string to_string(const std::chrono::nanoseconds& d) {
    return std::to_string(d.count()) + "ns";
}

inline std::string to_string(const std::chrono::milliseconds& d) {
    return std::to_string(d.count()) + "ms";
}

inline std::string to_string(const std::chrono::microseconds& d) {
    return std::to_string(d.count()) + "us";
}

inline std::string to_string(const std::chrono::seconds& d) {
    return std::to_string(d.count()) + "s";
}

inline std::string to_string(const std::chrono::minutes& d) {
    return std::to_string(d.count()) + "min";
}

inline std::string to_string(const std::chrono::hours& d) {
    return std::to_string(d.count()) + "h";
}

inline std::string to_string(const std::chrono::system_clock::time_point& tp) {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return std::string(buf);
}

// fallback to_string
template <typename T>
std::string to_string(const T& val) {
    std::ostringstream oss;
    oss << val;
    return oss.str();
}

inline std::string to_string(bool b) {
    return b ? "true" : "false";
}

template <typename T>
std::string to_string(const std::vector<T>& vec) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i) oss << ", ";
        oss << to_string(vec[i]);
    }
    oss << "]";
    return oss.str();
}

// 整数格式化，仅对 integral<T> 生效
template <typename T>
typename std::enable_if<std::is_integral<T>::value && ! std::is_same<T, bool>::value, std::string>::type
format_integer(T val, IntBase base) {
    typedef typename std::make_unsigned<T>::type unsigned_t;

    // 先按原类型宽度转无符号再提升到 64 位：
    // 一是避开 ostream 的 char 重载，二是保持位宽语义
    // （int8_t(-1) 的十六进制仍是 ff，而不是 ffffffffffffffff）。
    const uint64_t bits = static_cast<uint64_t>(static_cast<unsigned_t>(val));

    if (base == Bin) {
        std::string s;
        uint64_t u = bits;
        do {
            s += (u & 1) ? '1' : '0';
            u >>= 1;
        } while (u);
        std::reverse(s.begin(), s.end());
        return s;
    }

    std::ostringstream oss;
    if (base == Dec) {
        if (std::is_signed<T>::value) {
            oss << static_cast<int64_t>(val);
        } else {
            oss << static_cast<uint64_t>(val);
        }
    } else {
        oss << std::hex;
        if (base == HexUpper) oss << std::uppercase;
        oss << bits;
    }
    return oss.str();
}

// 根据是否整型有选择地调用 format_integer 或 to_string
template <typename T>
std::string maybe_format_integer(const T& val, IntBase base, std::true_type) {
    return format_integer(val, base);
}

template <typename T>
std::string maybe_format_integer(const T& val, IntBase /*unused*/, std::false_type) {
    return to_string(val);
}

inline std::string maybe_format_integer(bool val, IntBase, std::true_type) {
    return to_string(val);
}

// 递归展开 tuple，根据 arg_index 调用对应格式化
template <size_t I, typename Tuple>
typename std::enable_if<I == std::tuple_size<Tuple>::value, void>::type
apply_arg(std::ostringstream& oss, const std::string& /*part*/, size_t /*arg_index*/, const Tuple& /*tup*/) {
    // 参数越界
    oss << format_error("not enough arguments");
}

template <size_t I, typename Tuple>
    typename std::enable_if <
    I<std::tuple_size<Tuple>::value, void>::type
    apply_arg(std::ostringstream& oss, const std::string& part, size_t arg_index, const Tuple& tup) {
    if (arg_index == I) {
        // 取出 {spec}
        std::string spec = part.substr(1, part.size() - 2);
        FormatSpec fs = parse_spec(spec);
        typedef typename std::remove_reference<typename std::tuple_element<I, Tuple>::type>::type ArgType;
        const ArgType& val = std::get<I>(tup);
        std::string rendered;
        if (fs.has_base) {
            // 整数才做进制，否则当作普通 to_string
            rendered = maybe_format_integer(val, fs.base,
                                            std::integral_constant<bool, std::is_integral<ArgType>::value>());
        } else {
            rendered = to_string(val);
        }
        oss << pad_to_width(rendered, fs);
    } else {
        // 继续下一个
        apply_arg<I + 1>(oss, part, arg_index, tup);
    }
}

} // namespace

// 公共接口
template <typename... Args>
std::string fmt(const std::string& fmt_str, const Args&... args) {
    std::vector<std::string> parts = parse_format(fmt_str);
    // 如果 parse_format 返回单个错误
    if (parts.size() == 1 && parts[0].rfind("<?>(", 0) == 0) {
        return parts[0];
    }

    std::ostringstream oss;
    std::tuple<const Args&...> tup(args...);
    size_t arg_index = 0;

    for (size_t i = 0; i < parts.size(); ++i) {
        const std::string& part = parts[i];
        if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
            apply_arg<0>(oss, part, arg_index, tup);
            ++arg_index;
        } else {
            oss << part;
        }
    }
    
    if (arg_index < sizeof...(Args)) {
        oss << format_error("too many arguments");
    }

    return oss.str();
}

} // namespace internal

