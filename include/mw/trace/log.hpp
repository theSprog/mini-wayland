/**
 * @file trace/log.hpp
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

#include "mw/internal/error.hpp"

using internal::fmt;
using internal::Error;

namespace mw {

enum class LogLevel : int {
    Error = 0,
    Warn = 1,
    Info = 2,
    Debug = 3,
    Trace = 4,
};

namespace {

/// 缩进层数。单线程，不加锁。
int g_indent = 0;

bool g_initialised = false;
LogLevel g_level = LogLevel::Info;
bool g_timestamps = false;
bool g_color = false;

/// 进程启动时刻，用来把绝对时间变成相对秒数（读日志时好对齐）
timespec g_start{};

LogLevel parse_level(const char* text) noexcept {
    if (text == nullptr) {
        return LogLevel::Info;
    }
    if (std::strcmp(text, "error") == 0) return LogLevel::Error;
    if (std::strcmp(text, "warn") == 0) return LogLevel::Warn;
    if (std::strcmp(text, "info") == 0) return LogLevel::Info;
    if (std::strcmp(text, "debug") == 0) return LogLevel::Debug;
    if (std::strcmp(text, "trace") == 0) return LogLevel::Trace;
    return LogLevel::Info;
}

void ensure_init() noexcept {
    if (g_initialised) {
        return;
    }
    g_initialised = true;

    g_level = parse_level(std::getenv("MW_LOG"));

    const char* ts = std::getenv("MW_LOG_TIME");
    g_timestamps = ts != nullptr && ts[0] == '1';

    const char* color = std::getenv("MW_LOG_COLOR");
    g_color = color != nullptr ? (color[0] == '1') : (::isatty(STDERR_FILENO) == 1);

    clock_gettime(CLOCK_MONOTONIC, &g_start);
}

const char* level_tag(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Error: return "ERR ";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Debug: return "DBG ";
        case LogLevel::Trace: return "TRC ";
    }
    return "????";
}

const char* level_color(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Error: return "\033[31m";
        case LogLevel::Warn:  return "\033[33m";
        case LogLevel::Info:  return "\033[36m";
        case LogLevel::Debug: return "\033[90m";
        case LogLevel::Trace: return "\033[90m";
    }
    return "";
}

/// 只保留文件名，路径太长会把有用信息挤出屏幕
const char* short_file(const char* path) noexcept {
    const char* slash = std::strrchr(path, '/');
    return slash != nullptr ? slash + 1 : path;
}

} // namespace

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
    LogIndent() noexcept {
        ++g_indent;
    }
    ~LogIndent() noexcept {
        if (g_indent > 0) {
            --g_indent;
        }
    }

    LogIndent(const LogIndent&) = delete;
    LogIndent& operator=(const LogIndent&) = delete;
    LogIndent(LogIndent&&) = delete;
    LogIndent& operator=(LogIndent&&) = delete;
};


/// 当前等级。第一次调用时从 MW_LOG 初始化，之后是纯读，可以放热路径。
inline LogLevel log_level() noexcept {
    ensure_init();
    return g_level;
}

inline void set_log_level(LogLevel level) noexcept {
    ensure_init();
    g_level = level;
}

/// 实际写出。不要直接调，用下面的宏。
inline void log_write(LogLevel level, const char* file, int line, const std::string& msg) noexcept {
    ensure_init();

    char prefix[128];
    int n = 0;

    if (g_timestamps) {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const double elapsed = static_cast<double>(now.tv_sec - g_start.tv_sec) +
                               static_cast<double>(now.tv_nsec - g_start.tv_nsec) * 1e-9;
        n = std::snprintf(prefix, sizeof(prefix), "[%10.6f] ", elapsed);
        if (n < 0) {
            n = 0;
        }
    }

    const char* color = g_color ? level_color(level) : "";
    const char* reset = g_color ? "\033[0m" : "";

    std::fprintf(stderr, "%.*s%s%s%s %s:%d: %*s%s\n", n, prefix, color, level_tag(level), reset,
                 short_file(file), line, g_indent * 2, "", msg.c_str());
}

/// 打印 Error：domain / code / 源码位置；errno 域的额外附 strerror。
inline void log_error_object(const Error& e, const char* what) noexcept {
    // 报**错误产生的位置**，而不是这个函数所在的位置 —— 否则每条错误
    // 都指向 log.cpp，完全没有诊断价值。
    const char* file = e.location.file != nullptr ? e.location.file : "?";
    const int line = e.location.line;

    // errno 域的 Error 已经在 sys_err() 里带上 strerror 了，这里不重复。
    if (what != nullptr) {
        log_write(LogLevel::Error, file, line,
                  fmt("{}: [{}/{}] {}", what, e.domain, e.code, e.message));
    } else {
        log_write(LogLevel::Error, file, line, fmt("[{}/{}] {}", e.domain, e.code, e.message));
    }
    if (e.location.function != nullptr) {
        log_write(LogLevel::Debug, file, line, fmt("  in {}()", e.location.function));
    }
}



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
            ::mw::log_write((level_), __FILE__, __LINE__, ::internal::fmt(__VA_ARGS__));     \
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
