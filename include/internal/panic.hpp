#pragma once

#include "stacktrace.hpp"
#include <iostream>
#include <sstream>

namespace internal {

// LCOV_EXCL_START
// 不带上下文信息的 panic
[[noreturn]] inline void panic_no_loc(const std::string& msg) {
    std::cerr << "[PANIC]" << msg << std::endl;
    std::cerr << "Please submit a bug report to kdwarf maintainer, THANK YOU !!" << std::endl;
    auto st = Stacktrace::capture();
    std::cerr << "\nStackTrace: \n";
    st.print(std::cerr);
    std::abort();
    __builtin_unreachable();
}

// 默认 panic 自带上下文信息
[[noreturn]] inline void panic(const std::string& msg,
                               const char* file = __builtin_FILE(),
                               int line = __builtin_LINE(),
                               const char* func = __builtin_FUNCTION()) {
    std::ostringstream oss;
    oss << "[" << file << ":" << line << " (" << func << ")]: " << msg;
    panic_no_loc(oss.str());
}

// LCOV_EXCL_STOP

} // namespace internal
