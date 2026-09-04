#pragma once
#include <iostream>
#include <unistd.h>
#include <cstdint>

namespace internal {
namespace color {

template <typename T>
struct View {
    const T& val;
    uint8_t fg = 0;   // 30..37
    uint8_t bg = 0;   // 40..47
    uint8_t attr = 0; // 1: bold, 2: dim, 4: underline

    constexpr View bold()      const { auto v = *this; v.attr |= 1; return v; }
    constexpr View dim()       const { auto v = *this; v.attr |= 2; return v; }
    constexpr View underline() const { auto v = *this; v.attr |= 4; return v; }

    constexpr View on_black()   const { auto v = *this; v.bg = 40; return v; }
    constexpr View on_red()     const { auto v = *this; v.bg = 41; return v; }
    constexpr View on_green()   const { auto v = *this; v.bg = 42; return v; }
    constexpr View on_yellow()  const { auto v = *this; v.bg = 43; return v; }
    constexpr View on_blue()    const { auto v = *this; v.bg = 44; return v; }
    constexpr View on_magenta() const { auto v = *this; v.bg = 45; return v; }
    constexpr View on_cyan()    const { auto v = *this; v.bg = 46; return v; }
    constexpr View on_white()   const { auto v = *this; v.bg = 47; return v; }

    friend std::ostream& operator<<(std::ostream& os, const View& v) {
        static const bool tty_out = ::isatty(STDOUT_FILENO);
        static const bool tty_err = ::isatty(STDERR_FILENO);
        bool is_tty = (&os == &std::cout && tty_out) ||
                      ((&os == &std::cerr || &os == &std::clog) && tty_err);

        if (!is_tty) return os << v.val;

        os << "\033[";
        bool semi = false;
        auto put = [&](int code) {
            if (semi) os << ';';
            os << code;
            semi = true;
        };

        if (v.attr & 1) put(1);
        if (v.attr & 2) put(2);
        if (v.attr & 4) put(4);
        if (v.fg) put(v.fg);
        if (v.bg) put(v.bg);

        if (!semi) os << '0';
        return os << 'm' << v.val << "\033[0m";
    }
};

// 基础前景色入口
template <typename T> constexpr View<T> black(const T& v)   { return {v, 30, 0, 0}; }
template <typename T> constexpr View<T> red(const T& v)     { return {v, 31, 0, 0}; }
template <typename T> constexpr View<T> green(const T& v)   { return {v, 32, 0, 0}; }
template <typename T> constexpr View<T> yellow(const T& v)  { return {v, 33, 0, 0}; }
template <typename T> constexpr View<T> blue(const T& v)    { return {v, 34, 0, 0}; }
template <typename T> constexpr View<T> magenta(const T& v) { return {v, 35, 0, 0}; }
template <typename T> constexpr View<T> cyan(const T& v)    { return {v, 36, 0, 0}; }
template <typename T> constexpr View<T> white(const T& v)   { return {v, 37, 0, 0}; }

// 属性修饰入口
template <typename T> constexpr View<T> bold(const T& v)      { return View<T>{v}.bold(); }
template <typename T> constexpr View<T> dim(const T& v)       { return View<T>{v}.dim(); }
template <typename T> constexpr View<T> underline(const T& v) { return View<T>{v}.underline(); }

} // namespace color
} // namespace internal

