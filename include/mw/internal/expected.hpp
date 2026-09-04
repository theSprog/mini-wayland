/**
 * @file expected.hpp
 * @brief 简易版 expected 类型（成功值与错误值的统一封装）
 *
 * 本模块实现了类似 C++23 标准的 expected 类型，封装了成功值（value）与错误值（error）的处理逻辑。
 * 设计风格参考 tl::expected，偏向易用性，支持隐式返回与高效移动操作，适合用于内部错误传递与条件分支控制。
 */

#pragma once

#include "source_loc.hpp"
#include "format.hpp"
#include "macro.hpp"
#include <optional>
#include <utility>
#include <type_traits>

namespace internal {

template <typename E>
class unexpected {
  private:
    E error_;

  public:
    using error_type = E;

    // Constructor
    template <typename G = E, typename = std::enable_if_t<std::is_constructible_v<E, G&&>>>
    explicit unexpected(G&& err) noexcept(std::is_nothrow_constructible<E, G&&>::value)
        : error_(std::forward<G>(err)) {}

    // Copy constructor
    unexpected(const unexpected& other) = default;

    // Move constructor
    unexpected(unexpected&& other) noexcept(std::is_nothrow_move_constructible<E>::value) = default;

    // Copy assignment
    unexpected& operator=(const unexpected& other) = default;

    // Move assignment
    unexpected& operator=(unexpected&& other) noexcept(std::is_nothrow_move_assignable<E>::value) = default;

    // Access error
    constexpr const E& error() const& noexcept {
        return error_;
    }

    constexpr E& error() & noexcept {
        return error_;
    }

    constexpr const E&& error() const&& noexcept {
        return std::move(error_);
    }

    constexpr E&& error() && noexcept {
        return std::move(error_);
    }

    // swap 支持
    friend void swap(unexpected& a, unexpected& b) noexcept(std::is_nothrow_swappable_v<E>) {
        using std::swap;
        swap(a.error_, b.error_);
    }

    // 比较运算符
    friend constexpr bool operator==(const unexpected& a, const unexpected& b) noexcept {
        return a.error_ == b.error_;
    }

    friend constexpr bool operator!=(const unexpected& a, const unexpected& b) noexcept {
        return ! (a == b);
    }
};

// C++17 CTAD for unexpected
template <typename E>
unexpected(E) -> unexpected<E>;

template <typename T, typename E>
class expected {
    static_assert(! std::is_void_v<T>, "Use expected<void,E> specialization for void");

  private:
    bool has_value_;

    union {
        T value_;
        E error_;
    };

  public:
    using value_type = T;
    using error_type = E;

    // Constructors
    // default constructor
    expected() noexcept(std::is_nothrow_default_constructible_v<T>) : has_value_(true), value_() {}

    // Success constructor
    template <typename U = T, typename = std::enable_if_t<! std::is_same<std::decay_t<U>, expected>::value>>
    explicit expected(U&& val) noexcept(std::is_nothrow_constructible<T, U&&>::value)
        : has_value_(true), value_(std::forward<U>(val)) {}

    // Construct expected from unexpected (copy)
    expected(const unexpected<E>& e) noexcept(std::is_nothrow_copy_constructible<E>::value)
        : has_value_(false), error_(e.error()) {}

    // Construct expected from unexpected (move)
    expected(unexpected<E>&& e) noexcept(std::is_nothrow_move_constructible<E>::value)
        : has_value_(false), error_(std::move(e.error())) {}

    // swap
    friend void swap(expected& a, expected& b) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                        std::is_nothrow_move_constructible_v<E> &&
                                                        std::is_nothrow_swappable_v<T> &&
                                                        std::is_nothrow_swappable_v<E>) {
        using std::swap;
        if (a.has_value_ && b.has_value_) {
            swap(a.value_, b.value_);
        } else if (! a.has_value_ && ! b.has_value_) {
            swap(a.error_, b.error_);
        } else {
            expected tmp = std::move(b);
            b = std::move(a);
            a = std::move(tmp);
        }
    }

    // Destructor
    ~expected() {
        destroy();
    }

    // Copy constructor
    expected(const expected& other) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                             std::is_nothrow_copy_constructible_v<E>)
        : has_value_(other.has_value_) {
        if (other.has_value_) {
            new (&value_) T(other.value_);
        } else {
            new (&error_) E(other.error_);
        }
    }

    // Move constructor
    expected(expected&& other) noexcept(std::is_nothrow_move_constructible<T>::value &&
                                        std::is_nothrow_move_constructible<E>::value)
        : has_value_(other.has_value_) {
        if (other.has_value_) {
            new (&value_) T(std::move(other.value_));
        } else {
            new (&error_) E(std::move(other.error_));
        }
    }

    // Copy assignment
    expected& operator=(const expected& other) noexcept(std::is_nothrow_copy_constructible_v<T> &&
                                                        std::is_nothrow_copy_constructible_v<E> &&
                                                        std::is_nothrow_destructible_v<T> &&
                                                        std::is_nothrow_destructible_v<E>) {
        if (this != &other) {
            destroy();
            if (other.has_value_) {
                new (&value_) T(other.value_);
                has_value_ = true;
            } else {
                new (&error_) E(other.error_);
                has_value_ = false;
            }
        }
        return *this;
    }

    // Move assignment
    expected& operator=(expected&& other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                   std::is_nothrow_move_constructible_v<E> &&
                                                   std::is_nothrow_destructible_v<T> &&
                                                   std::is_nothrow_destructible_v<E>) {
        if (this != &other) {
            destroy();
            if (other.has_value_) {
                new (&value_) T(std::move(other.value_));
                has_value_ = true;
            } else {
                new (&error_) E(std::move(other.error_));
                has_value_ = false;
            }
        }
        return *this;
    }

    // Accessors
    constexpr bool has_value() const noexcept {
        return has_value_;
    }

    constexpr explicit operator bool() const noexcept {
        return has_value_;
    }

    T& value(const SourceLocation& loc = SourceLocation::current()) & {
        check_has_value(loc);
        return value_;
    }

    const T& value(const SourceLocation& loc = SourceLocation::current()) const& {
        check_has_value(loc);
        return value_;
    }

    T&& value(const SourceLocation& loc = SourceLocation::current()) && {
        check_has_value(loc);
        return std::move(value_);
    }

    E& error(const SourceLocation& loc = SourceLocation::current()) & {
        check_has_error(loc);
        return error_;
    }

    const E& error(const SourceLocation& loc = SourceLocation::current()) const& {
        check_has_error(loc);
        return error_;
    }

    E&& error(const SourceLocation& loc = SourceLocation::current()) && {
        check_has_error(loc);
        return std::move(error_);
    }

    template <typename Enum>
    bool has_error(Enum code) const noexcept {
        static_assert(std::is_enum_v<Enum>, "has_error() expects an error code enum");
        return ! has_value_ && error_.is(code);
    }

    T value_or(T&& default_value) const& {
        if (has_value_) {
            return value_;
        } else {
            return default_value;
        }
    }

  private:
    void check_has_value(const SourceLocation& loc) const {
        if (! has_value_) {
            panic_no_loc(
                fmt("{}: Attempted to access value() but has_value_ == false", loc.to_string())); // LCOV_EXCL_LINE
        }
    }

    void check_has_error(const SourceLocation& loc) const {
        if (has_value_) {
            panic_no_loc(
                fmt("{}: Attempted to access error() but has_value_ == true", loc.to_string())); // LCOV_EXCL_LINE
        }
    }

    void destroy() noexcept {
        if (has_value_) {
            value_.~T();
        } else {
            error_.~E();
        }
    }
};

// void 特化
template <typename E>
class expected<void, E> {
  private:
    bool has_value_;
    std::optional<E> error_; // 只在失败时构造, 避免性能浪费

  public:
    using value_type = void;
    using error_type = E;

    // Constructors
    expected() noexcept : has_value_(true), error_(std::nullopt) {}

    expected(const unexpected<E>& e) noexcept(std::is_nothrow_copy_constructible_v<E>)
        : has_value_(false), error_(e.error()) {}

    expected(unexpected<E>&& e) noexcept(std::is_nothrow_move_constructible_v<E>)
        : has_value_(false), error_(std::move(e.error())) {}

    // Copy constructor
    expected(const expected& rhs) noexcept(std::is_nothrow_copy_constructible_v<E>) = default;
    expected(expected&& rhs) noexcept(std::is_nothrow_move_constructible_v<E>) = default;

    // Assignment
    expected& operator=(const expected& rhs) noexcept(std::is_nothrow_copy_assignable_v<E>) = default;
    expected& operator=(expected&& rhs) noexcept(std::is_nothrow_move_assignable_v<E>) = default;

    // Destructor
    ~expected() = default;

    // Access
    constexpr bool has_value() const noexcept {
        return has_value_;
    }

    constexpr explicit operator bool() const noexcept {
        return has_value_;
    }

    void value(const SourceLocation& loc = SourceLocation::current()) const {
        if (! has_value_) {
            panic_no_loc(fmt("{}: Accessing value() on expected<void, E> which holds an error", loc.to_string()));
        }
    }

    const E& error(const SourceLocation& loc = SourceLocation::current()) const& {
        if (has_value_) {
            panic_no_loc(fmt("{}: Accessing error() on expected<void, E> which holds a value(void)", loc.to_string()));
        }
        return *error_;
    }

    E&& error(const SourceLocation& loc = SourceLocation::current()) && {
        if (has_value_) {
            panic_no_loc(fmt("{}: Accessing error() on expected<void, E> which holds a value(void)", loc.to_string()));
        }
        return std::move(*error_);
    }

    /// 是否持有指定模块的指定错误码（持有值时返回 false，不会 panic）
    template <typename Enum>
    bool has_error(Enum code) const noexcept {
        static_assert(std::is_enum_v<Enum>, "has_error() expects an error code enum");
        return ! has_value_ && error_.has_value() && error_->is(code);
    }

    // 容器友好
    friend void swap(expected& a,
                     expected& b) noexcept(std::is_nothrow_move_constructible_v<E> && std::is_nothrow_swappable_v<E>) {
        using std::swap;
        swap(a.has_value_, b.has_value_);
        swap(a.error_, b.error_);
    }
};
}; // namespace internal