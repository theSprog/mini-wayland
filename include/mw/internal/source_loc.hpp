#pragma once

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

namespace internal {
/**
 * @brief Represents the source location (file, function, line) where an error occurred.
 */
struct SourceLocation {
    const char* file = nullptr;
    const char* function = nullptr;
    int line = 0;

    constexpr SourceLocation() noexcept = default;

    constexpr SourceLocation(const char* f, const char* func, int l) noexcept : file(f), function(func), line(l) {}

    // Capture the current source location
    static constexpr SourceLocation current(const char* f = __builtin_FILE(),
                                            const char* func = __builtin_FUNCTION(),
                                            int l = __builtin_LINE()) noexcept {
        return SourceLocation(f, func, l);
    }

    std::string to_string() const {
        const char* trimmed_file = file;
        if (const char* p = std::strstr(file, "kdwarf/")) {
            trimmed_file = p;
        }
        std::ostringstream oss;
        oss << "[" << trimmed_file << ":" << line << " (" << function << ")]";
        return oss.str();
    }
};

inline std::ostream& operator<<(std::ostream& os, const SourceLocation& loc) {
    return os << loc.to_string();
}
} // namespace internal
