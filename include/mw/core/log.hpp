/**
 * @file core/log.hpp
 * @brief 日志 —— 这个项目里日志不是辅助设施，是主要的观察窗口
 *
 * 我们做的每一件事最终都变成一次 ioctl，而 ioctl 的返回值只有一个 errno。
 * 想搞清楚"内核到底收到了什么、为什么拒绝"，唯一的办法是把下发的每一个
 * 属性、每一个 fd、每一次提交都打出来，然后和 modetest / drm.debug 的输出对照。
 *
 * 三条要求：
 *  1. **等级过滤发生在 fmt() 之前**。所以必须是宏。关掉的日志不能付出
 *     格式化代价 —— Trace 级别下每帧会打几十条，函数接口做不到零开销。
 *  2. **输出到 stderr**。合成器会独占 tty，stdout 可能被重定向；
 *     stderr 更容易在另一个终端 tail 到。
 *  3. **ioctl 有专门的记法**，见 drm/trace.hpp。
 *
 * 环境变量：
 *   MW_LOG=error|warn|info|debug|trace   默认 info
 *   MW_LOG_TIME=1                        每行加 CLOCK_MONOTONIC 时间戳
 *                                        （Step 7 对帧节拍时特别有用）
 *   MW_LOG_COLOR=0|1                     默认按 isatty 自动判断
 */
#pragma once

#include <cerrno>
#include <string>

#include "mw/core/error.hpp"

namespace mw {

enum class LogLevel : int {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Trace = 4,
};

/// 当前等级。第一次调用时从 MW_LOG 初始化，之后是纯读，可以放热路径。
LogLevel log_level() noexcept;

void set_log_level(LogLevel level) noexcept;

/// 实际写出。不要直接调，用下面的宏。
void log_write(LogLevel level, const char* file, int line, const std::string& msg) noexcept;

/// 打印 Error：domain / code / 源码位置；errno 域的额外附 strerror。
void log_error_object(const Error& e, const char* what = nullptr) noexcept;

/**
 * @brief 缩进作用域
 *
 * 资源枚举是树形的（device -> connector -> mode / encoder），
 * 平铺的日志读起来很痛苦。构造时 +1 层，析构时 -1 层。
 *
 * @code
 *   LOG_INFO("connector {}", conn.name);
 *   {
 *       LOG_SCOPE();
 *       for (const auto& m : conn.modes) LOG_DEBUG("mode {}", m.name());
 *   }
 * @endcode
 */
class LogIndent {
  public:
    LogIndent() noexcept;
    ~LogIndent() noexcept;

    LogIndent(const LogIndent&) = delete;
    LogIndent& operator=(const LogIndent&) = delete;
    LogIndent(LogIndent&&) = delete;
    LogIndent& operator=(LogIndent&&) = delete;
};

} // namespace mw

#define MW_CAT_IMPL(a, b) a##b
#define MW_CAT(a, b)      MW_CAT_IMPL(a, b)

/**
 * 日志宏保证 **errno 透明**：进宏之前的 errno 值，在参数求值时和出宏之后都还在。
 *
 * 为什么需要这个：宏的 if 条件会先调 log_level()，它第一次调用要走 getenv /
 * clock_gettime，这些都会改 errno。而日志参数（比如 errno_name(errno)）是在
 * if 之后才求值的 —— 不还原的话，你打出来的永远是 log 自己产生的 errno，
 * 不是失败的那个系统调用留下的。
 *
 * 这个坑很隐蔽（表现是错误码全变成一个奇怪的固定值），所以修在宏里，
 * 而不是要求每个调用点都先 `const int err = errno;`。
 */
#define MW_LOG_AT(level_, ...)                                                        \
    do {                                                                              \
        const int mw_saved_errno_ = errno;                                            \
        if (static_cast<int>(::mw::log_level()) >= static_cast<int>(level_)) {         \
            errno = mw_saved_errno_;                                                   \
            ::mw::log_write((level_), __FILE__, __LINE__, ::mw::fmt(__VA_ARGS__));     \
        }                                                                             \
        errno = mw_saved_errno_;                                                      \
    } while (0)

#define LOG_ERROR(...) MW_LOG_AT(::mw::LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...)  MW_LOG_AT(::mw::LogLevel::Warn, __VA_ARGS__)
#define LOG_INFO(...)  MW_LOG_AT(::mw::LogLevel::Info, __VA_ARGS__)
#define LOG_DEBUG(...) MW_LOG_AT(::mw::LogLevel::Debug, __VA_ARGS__)
#define LOG_TRACE(...) MW_LOG_AT(::mw::LogLevel::Trace, __VA_ARGS__)

/// 当前作用域内的日志缩进一层
#define LOG_SCOPE() ::mw::LogIndent MW_CAT(mw_log_indent_, __LINE__)
