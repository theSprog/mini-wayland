#pragma once

#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include "source_loc.hpp"
#include "expected.hpp"

namespace internal {

using ErrorCode = int;

struct IError {
    virtual ~IError() = default;

    virtual ErrorCode error_code() const noexcept = 0;
    virtual const char* error_message() const noexcept = 0;

    /**
     * @brief 错误所属模块
     *
     * 必须返回静态字符串字面量（生命周期与程序相同），如 "elf" / "abbrev"。
     * 各模块的错误码枚举都从 0 起编号，数值本身不唯一，靠本字段区分来源。
     */
    virtual const char* error_domain() const noexcept = 0;
};

/**
 * @brief 在 IError 子类内部声明该类所属的错误域
 *
 * 用法（类体内，不带分号）：
 * @code
 *   struct ElfError : public IError {
 *       DEFINE_ERROR_DOMAIN("elf")
 *       ...
 *   };
 * @endcode
 */
#define DEFINE_ERROR_DOMAIN(domain_literal)              \
    const char* error_domain() const noexcept override { \
        return domain_literal;                           \
    }

/**
 * @brief 通用错误类型：模块 + 码值 + 描述 + 源码位置
 */
struct Error {
    const char* domain; ///< 静态字符串字面量，不拥有所有权
    ErrorCode code;
    std::string message;
    SourceLocation location;

    // domain + location + code + message => display_str
    std::string display_str;

    Error(const char* dom, ErrorCode c, std::string msg,
          SourceLocation loc = SourceLocation::current()) noexcept
        : domain(dom),
          code(c),
          message(std::move(msg)),
          location(loc),
          display_str() {
        display_str = location.to_string() + " " + domain + ": " + message;
    }

    // domain 是不拥有所有权的静态字面量，逐位拷贝即可。
    // 显式 default 一方面消除 -Weffc++ 对裸指针成员的告警，
    // 另一方面避免声明拷贝后隐式移动被抑制（Error 在 expected 中频繁移动）。
    Error(const Error&) = default;
    Error& operator=(const Error&) = default;
    Error(Error&&) noexcept = default;
    Error& operator=(Error&&) noexcept = default;

    /**
     * @brief 是否为指定模块的指定错误码
     *
     * 同时比对 domain 与 code，避免不同模块间码值重叠导致的误判。
     * make_error 通过 ADL 在 Enum 所在命名空间中查找，与 Err() 的机制一致。
     *
     * @code
     *   if (res.error().is(ElfErrorCode::SectionNotFound)) { ... }
     * @endcode
     */
    template <typename Enum>
    bool is(Enum expected_code) const noexcept {
        static_assert(std::is_enum<Enum>::value, "Error::is() expects an error code enum");

        const auto proto = make_error(expected_code);
        return code == proto.error_code() && in_domain(proto.error_domain());
    }

    /// 是否来自指定模块（先比指针，字面量通常被合并；否则回退到 strcmp）
    bool in_domain(const char* dom) const noexcept {
        if (dom == nullptr) {
            return false;
        }
        return domain == dom || std::strcmp(domain, dom) == 0;
    }

    const char* what() noexcept {
        return display_str.c_str();
    }

    virtual ~Error() = default;
};

template <typename U>
inline expected<std::decay_t<U>, Error> Ok(U&& v) noexcept(std::is_nothrow_constructible_v<std::decay_t<U>, U&&>) {
    return expected<std::decay_t<U>, Error>{std::forward<U>(v)};
}

// void 版本
inline expected<void, Error> Ok() noexcept {
    return expected<void, Error>{};
}

template <typename E>
inline unexpected<Error> ErrObj(E&& e,
                                std::optional<std::string> extra = std::nullopt,
                                SourceLocation loc = SourceLocation::current()) noexcept {
    static_assert(std::is_base_of_v<IError, std::decay_t<E>>, "ErrObj() only for IError-derived types");

    const auto& ref = e;
    std::string full_msg = ref.error_message();
    if (extra) {
        full_msg += ": ";
        full_msg += *extra;
    }

    return unexpected<Error>(Error(ref.error_domain(), ref.error_code(), std::move(full_msg), loc));
}

template <typename Enum>
inline unexpected<Error> Err(Enum code,
                             std::optional<std::string> extra = std::nullopt,
                             SourceLocation loc = SourceLocation::current()) noexcept {
    return ErrObj(make_error(code), std::move(extra), loc);
}

#define REGISTER_MAKE_ERROR(ErrorCodeType, ErrorType)                                                        \
    static_assert(std::is_enum<ErrorCodeType>::value, "REGISTER_MAKE_ERROR expects enum as first argument"); \
    inline ErrorType make_error(ErrorCodeType code) {                                                        \
        return ErrorType(code);                                                                              \
    }

/// 返回值统一别名：`Result<T>` / `Status`
template <typename T>
using Result = expected<T, Error>;

using Status = expected<void, Error>;

inline constexpr const char* kSysDomain = "errno";

struct SysError final : IError {
    explicit SysError(int err) noexcept : err_(err), msg_() {
        // strerror_r 的 GNU 版本返回 char*，可能不写进 buf_。
        char buf[128];
        const char* text = ::strerror_r(err, buf, sizeof(buf));
        msg_ = text != nullptr ? text : "unknown error";
    }

    int error_code() const noexcept override {
        return err_;
    }

    const char* error_message() const noexcept override {
        return msg_.c_str();
    }

    const char* error_domain() const noexcept override {
        return kSysDomain;
    }

  private:
    int err_;
    std::string msg_;
};

// SourceLocation 是 3 个指针 + 一个 int 的 POD，按值传比 const& 更便宜，
// 而且默认实参 SourceLocation::current() 必须在调用点求值。
// cppcheck-suppress passedByValue
inline unexpected<Error> sys_err(const char* what, int err = errno, SourceLocation loc = SourceLocation::current()) {
    std::string msg;
    msg.reserve(64);
    msg += what;
    msg += ": ";
    {
        const SysError se(err);
        msg += se.error_message();
    }
    msg += " (errno=";
    msg += std::to_string(err);
    msg += ")";
    return unexpected<Error>(Error(kSysDomain, err, std::move(msg), loc));
}

inline unexpected<Error> sys_err_ctx(const char* what, std::string_view context, int err = errno,
                              // cppcheck-suppress passedByValue
                              SourceLocation loc = SourceLocation::current()) {
    std::string msg;
    msg.reserve(96);
    msg += what;
    msg += "('";
    msg.append(context.data(), context.size());
    msg += "'): ";
    {
        const SysError se(err);
        msg += se.error_message();
    }
    msg += " (errno=";
    msg += std::to_string(err);
    msg += ")";
    return unexpected<Error>(Error(kSysDomain, err, std::move(msg), loc));
}

inline bool is_errno(const Error& e, int errno_value) noexcept {
    return e.in_domain(kSysDomain) && e.code == errno_value;
}

inline int errno_of(const Error& e) noexcept {
    return e.in_domain(kSysDomain) ? e.code : 0;
}

} // namespace internal