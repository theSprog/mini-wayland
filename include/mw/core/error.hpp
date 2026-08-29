/**
 * @file core/error.hpp
 * @brief 全工程统一的错误表达：expected/Error + 一个"裸 errno"域
 *
 * 两类错误分开表达，靠 Error::domain 区分：
 *
 *   domain = "errno"   —— 系统调用/ioctl 直接失败，code 就是 errno 原值。
 *                         这类错误**必须保留 errno 原值**，因为上层要靠
 *                         EBUSY / EINVAL / EACCES 做分支（例如 atomic
 *                         TEST_ONLY 失败后决定降级还是报错退出）。
 *
 *   domain = "drm" 等  —— 模块自己的语义错误码（见 drm/error.hpp）。
 *
 * 热路径约定（重要）：
 *   internal::Error 构造时会拼两个 std::string，**每帧都可能发生的"预期内失败"
 *   不要用它**。典型例子是 plane 分配器里的 DRM_MODE_ATOMIC_TEST_ONLY 试探
 *   —— 失败是正常控制流，返回 int errno 即可，不构造 Error。
 *   Error 用于：初始化、modeset、以及真正需要向用户报错退出的路径。
 */
#pragma once

#include <cerrno>
#include <string>
#include <string_view>

#include "internal/error.hpp"
#include "internal/expected.hpp"
#include "internal/format.hpp"
#include "internal/macro.hpp"
#include "internal/span.hpp"

namespace mw {

// ---- internal/ 里的基础类型提升到 mw:: ---------------------------------------
using internal::Error;
using internal::expected;
using internal::IError;
using internal::SourceLocation;
using internal::span;
using internal::unexpected;

using internal::Err;
using internal::ErrObj;
using internal::fmt;
using internal::Ok;

/// 返回值统一别名：`Result<Device>` / `Status`
template <typename T>
using Result = expected<T, Error>;

using Status = expected<void, Error>;

// ---- 裸 errno 域 -----------------------------------------------------------

/// 静态字面量，Error::in_domain 会先比指针再 strcmp
inline constexpr const char* kSysDomain = "errno";

/**
 * @brief 把刚刚失败的系统调用包成 Error，code 保留 errno 原值
 *
 * @param what  失败的调用名，静态字面量，如 "drmModeAtomicCommit"
 * @param err   errno 值。默认参数在**调用点**求值，所以必须在失败后
 *              **立刻**调用，中间不要插入任何可能改写 errno 的代码
 *              （包括 log、std::string 构造）。
 *
 * @code
 *   if (drmModeAtomicCommit(fd, req, flags, nullptr) != 0) {
 *       return sys_err("drmModeAtomicCommit");   // ← 紧贴失败点
 *   }
 * @endcode
 */
unexpected<Error> sys_err(const char* what,
                          int err = errno,
                          SourceLocation loc = SourceLocation::current());

/// 同上，但附加一段上下文（会额外分配，不要用在热路径）
unexpected<Error> sys_err_ctx(const char* what,
                              std::string_view context,
                              int err = errno,
                              SourceLocation loc = SourceLocation::current());

/// e 是否是 errno 域的指定错误。用法：`if (is_errno(r.error(), EBUSY)) ...`
bool is_errno(const Error& e, int errno_value) noexcept;

/// 若 e 属于 errno 域返回其 errno，否则返回 0
int errno_of(const Error& e) noexcept;

} // namespace mw
