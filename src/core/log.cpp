#include "mw/core/log.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace mw {
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

LogLevel log_level() noexcept {
    ensure_init();
    return g_level;
}

void set_log_level(LogLevel level) noexcept {
    ensure_init();
    g_level = level;
}

void log_write(LogLevel level, const char* file, int line, const std::string& msg) noexcept {
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

void log_error_object(const Error& e, const char* what) noexcept {
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

LogIndent::LogIndent() noexcept {
    ++g_indent;
}

LogIndent::~LogIndent() noexcept {
    if (g_indent > 0) {
        --g_indent;
    }
}

} // namespace mw
