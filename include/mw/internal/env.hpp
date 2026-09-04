#pragma once

#include <charconv>
#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace internal {
namespace env {
static inline bool iequals(std::string_view lhs, std::string_view rhs) noexcept {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
            std::tolower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

static inline bool parse_bool(std::string_view sv, bool& out) noexcept {
    if (sv == "1" || iequals(sv, "true") || iequals(sv, "yes") || iequals(sv, "on")) {
        out = true;
        return true;
    }
    if (sv == "0" || iequals(sv, "false") || iequals(sv, "no") || iequals(sv, "off")) {
        out = false;
        return true;
    }
    return false;
}

template <typename T>
static inline bool parse_value(std::string_view sv, T& out) {
    using Decayed = std::decay_t<T>;

    if constexpr (std::is_same_v<Decayed, std::string>) {
        out = std::string(sv);
        return true;
    } else if constexpr (std::is_same_v<Decayed, int>) {
        if (sv.empty()) {
            return false;
        }
        int parsed = 0;
        auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), parsed);
        if (ec == std::errc{} && ptr == sv.data() + sv.size()) {
            out = parsed;
            return true;
        }
        return false;
    } else if constexpr (std::is_same_v<Decayed, bool>) {
        return parse_bool(sv, out);
    } else {
        static_assert(
            std::is_same_v<Decayed, std::string> ||
            std::is_same_v<Decayed, int> ||
            std::is_same_v<Decayed, bool>,
            "[env::read] Only std::string, int, and bool are supported."
        );
        return false;
    }
}

template <typename T>
std::optional<T> read() {
    T config{};

    auto visitor = [](auto& target, const char* key) -> bool {
        const char* val = std::getenv(key);
        if (val == nullptr) {
            return true; // 未设置时保留结构体定义的默认值
        }
        return parse_value(std::string_view(val), target);
    };

    if (config.__env_accept(visitor)) {
        return config;
    }
    return std::nullopt;
}

} // namespace env
} // namespace internal

#define ENV_FIELD(member, env_key) \
    [&](auto& v) { return v(this->member, env_key); }

#define ENV_SCHEMA(...) \
    template <typename Visitor> \
    bool __env_accept(Visitor&& visitor) { \
        auto dispatch = [&](auto&&... fields) { \
            return (fields(visitor) && ...); \
        }; \
        return dispatch(__VA_ARGS__); \
    }
