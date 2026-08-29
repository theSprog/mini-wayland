#pragma once

#include "panic.hpp" // IWYU pragma: keep

#include <utility>

// panic: 库内部错误, 注意不应该用 panic 表示用户异常逻辑
#define PANIC(msg)              \
    do {                        \
        ::internal::panic(msg); \
    } while (0)

#define UNREACHABLE() PANIC("unreachable code reached")

#define TODO() PANIC("not implemented")

#define ASSERT(expr)                           \
    do {                                       \
        if (! (expr)) {                        \
            PANIC("assertion failed: " #expr); \
        }                                      \
    } while (0)

// 带自定义信息的 assert
#define EXPECT(cond, msg) \
    do {                  \
        if (! (cond)) {   \
            PANIC(msg);   \
        }                 \
    } while (0)

// 简易 try 宏（expected 错误传播）
//
// 注意两点：
//  1. 这是 GNU statement-expression，所以工程不能开 -Wpedantic。
//  2. 成功值是 **移动** 出来的（std::move(_result).value()），
//     否则 move-only 类型（UniqueFd / Framebuffer / DumbBuffer）
//     会因为拷贝构造被删除而编译失败。
//     推论：TRY(expr) 里的 expr 应该是临时量或你不再需要的变量，
//     不要写 TRY(some_variable_you_still_use)。
#define TRY(expr)                                                 \
    ({                                                            \
        auto&& _result = (expr);                                  \
        if (! _result) return unexpected<Error>(_result.error()); \
        std::move(_result).value();                               \
    })

#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   (__builtin_expect(! ! (x), 1))
#define UNLIKELY(x) (__builtin_expect(! ! (x), 0))
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ASSUME(cond)                           \
    do {                                       \
        if (! (cond)) __builtin_unreachable(); \
    } while (0)
#else
#define ASSUME(cond)  \
    do {              \
        (void)(cond); \
    } while (0)
#endif
