// parse_args.hpp
// 本文件是独立的、可复用的命令行参数解析器。
// 和其他 internal:: 头文件不同，这个文件可以直接拷贝到其他项目里用，依赖 C++17 标准库即可
#pragma once

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace internal {

namespace parse_args {


template<typename T>
inline constexpr bool always_false_v = false;

// Type traits for container and optional detection
template <typename T>
struct is_optional : std::false_type {};
template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};
template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template <typename T>
struct is_vector : std::false_type {};
template <typename T, typename Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type {};
template <typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

// String conversion helper
template <typename T>
bool from_string_impl(std::string_view sv, T& out) {
    if constexpr (std::is_same_v<T, std::string>) {
        out = std::string(sv);
        return true;
    } else if constexpr (std::is_same_v<T, std::string_view>) {
        out = sv;
        return true;
    } else if constexpr (std::is_same_v<T, bool>) {
        if (sv == "true" || sv == "1" || sv == "on" || sv == "yes") {
            out = true;
            return true;
        }
        if (sv == "false" || sv == "0" || sv == "off" || sv == "no") {
            out = false;
            return true;
        }
        return false;
    } else if constexpr (std::is_integral_v<T>) {
        auto [p, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
        return ec == std::errc{} && p == sv.data() + sv.size();
    } else if constexpr (std::is_floating_point_v<T>) {
        std::string str(sv);
        char* end = nullptr;
        errno = 0;
        if constexpr (std::is_same_v<T, float>) {
            out = std::strtof(str.c_str(), &end);
        } else if constexpr (std::is_same_v<T, double>) {
            out = std::strtod(str.c_str(), &end);
        } else if constexpr (std::is_same_v<T, long double>) {
            out = std::strtold(str.c_str(), &end);
        }
        return end == str.c_str() + str.size() && errno == 0;
    } else {
        static_assert(always_false_v<T>, 
            "Unsupported type for parse_args. Please bind to std::string and convert manually in your application logic.");
        return false;
    }
}

static inline std::string extract_basename(std::string_view path) {
    auto pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(pos + 1));
}

template <typename Config>
struct ParseResult {
    Config config;
    std::optional<std::string> error;

    explicit operator bool() const noexcept { return !error.has_value(); }
};

template <typename Config>
class parser {
    struct Action {
        std::string short_opt;
        std::string long_opt;
        std::string pos_name;
        std::string description;

        // 显式初始化列表，修复 -Weffc++ 警告
        Action() : short_opt(), long_opt(), pos_name(), description() {}
        virtual ~Action() = default;

        virtual bool execute(Config& cfg, std::string_view val) = 0;
        virtual bool is_flag() const = 0;
        virtual bool is_optional_pos() const = 0;
        virtual bool is_variadic_pos() const = 0;
        virtual std::string type_hint() const = 0;
    };

    template <typename MemberType>
    struct MemberAction : Action {
        MemberType Config::*ptr;

        // 显式调用基类构造，修复 -Weffc++ 警告
        explicit MemberAction(MemberType Config::*p) : Action(), ptr(p) {}

        bool execute(Config& cfg, std::string_view val) override {
            if constexpr (std::is_same_v<MemberType, bool>) {
                if (val.empty()) {
                    cfg.*ptr = true;
                    return true;
                }
                return from_string_impl(val, cfg.*ptr);
            } else if constexpr (is_optional_v<MemberType>) {
                typename MemberType::value_type tmp{};
                if (!from_string_impl(val, tmp)) return false;
                cfg.*ptr = std::move(tmp);
                return true;
            } else if constexpr (is_vector_v<MemberType>) { // LCOV_EXCL_LINE
                typename MemberType::value_type tmp{};
                if (!from_string_impl(val, tmp)) return false;
                (cfg.*ptr).push_back(std::move(tmp));
                return true;
            } else {
                return from_string_impl(val, cfg.*ptr);
            }
        }

        bool is_flag() const override {
            return std::is_same_v<MemberType, bool>;
        }

        bool is_optional_pos() const override {
            return is_optional_v<MemberType>;
        }

        bool is_variadic_pos() const override {
            return is_vector_v<MemberType>;
        }

        std::string type_hint() const override {
            if constexpr (std::is_same_v<MemberType, bool>) {
                return "";
            } else if constexpr (std::is_integral_v<MemberType>) {
                return "<INT>";
            } else if constexpr (std::is_floating_point_v<MemberType>) {
                return "<FLOAT>";
            } else if constexpr (is_optional_v<MemberType>) {
                if constexpr (std::is_integral_v<typename MemberType::value_type>) {
                    return "<INT>";
                } else if constexpr (std::is_floating_point_v<typename MemberType::value_type>) {
                    return "<FLOAT>";
                } else {
                    return "<STR>";
                }
            } else {
                return "<STR>";
            }
        }
    };

public:
    // 显式初始化所有成员，修复 -Weffc++ 警告
    explicit parser(std::string description = "")
        : description_(std::move(description)),
          program_name_(),
          options_(),
          positionals_(),
          examples_(),
          notes_() {}

    template <typename MemberType, typename... Args>
    parser& bind(MemberType Config::*member, Args&&... args) {
        auto act = std::make_shared<MemberAction<MemberType>>(member);
        std::vector<std::string> params = { std::string(std::forward<Args>(args))... };

        for (auto& param : params) {
            if (param.rfind("--", 0) == 0) {
                act->long_opt = param;
            } else if (param.rfind("-", 0) == 0) {
                act->short_opt = param;
            } else {
                if (act->short_opt.empty() && act->long_opt.empty() && act->pos_name.empty()) {
                    act->pos_name = param;
                } else {
                    act->description = param;
                }
            }
        }

        if (!act->short_opt.empty() || !act->long_opt.empty()) {
            options_.push_back(act);
        } else {
            positionals_.push_back(act);
        }

        return *this;
    }

    parser& example(std::string cmd, std::string desc) {
        examples_.emplace_back(std::move(cmd), std::move(desc));
        return *this;
    }

    parser& note(std::string text) {
        notes_.push_back(std::move(text));
        return *this;
    }

    ParseResult<Config> parse(int argc, char* const argv[]) const {
        Config cfg{};
        
        if (argc > 0 && argv[0]) {
            program_name_ = extract_basename(argv[0]);
        }
        if (program_name_.empty()) {
            program_name_ = "app";
        }

        bool stop_options = false;
        size_t pos_idx = 0;
        std::vector<bool> pos_satisfied(positionals_.size(), false);

        for (int i = 1; i < argc; ++i) {
            std::string_view token = argv[i];

            if (!stop_options && token == "--") {
                stop_options = true;
                continue;
            }

            if (!stop_options && token.rfind("-", 0) == 0 && token != "-") {
                std::string_view key = token;
                std::optional<std::string_view> inline_val;

                auto eq_pos = token.find('=');
                if (eq_pos != std::string_view::npos) {
                    key = token.substr(0, eq_pos);
                    inline_val = token.substr(eq_pos + 1);
                }

                auto opt_act = find_option(key);
                if (!opt_act) {
                    return {cfg, "Unknown option: " + std::string(key)};
                }

                if (opt_act->is_flag()) {
                    if (inline_val.has_value()) {
                        return {cfg, "Flag '" + std::string(key) + "' does not take a value"};
                    }
                    opt_act->execute(cfg, "");
                } else {
                    std::string_view val;
                    if (inline_val.has_value()) {
                        val = *inline_val;
                    } else {
                        if (i + 1 >= argc) {
                            return {cfg, "Option '" + std::string(key) + "' requires a value"};
                        }
                        val = argv[++i];
                    }

                    if (!opt_act->execute(cfg, val)) {
                        return {cfg, "Invalid value '" + std::string(val) + "' for option '" + std::string(key) + "'"};
                    }
                }
            } else {
                if (pos_idx < positionals_.size()) {
                    auto& pos_act = positionals_[pos_idx];
                    if (!pos_act->execute(cfg, token)) {
                        return {cfg, "Invalid value '" + std::string(token) + "' for positional argument '" + pos_act->pos_name + "'"};
                    }
                    pos_satisfied[pos_idx] = true;
                    if (!pos_act->is_variadic_pos()) {
                        ++pos_idx;
                    }
                } else {
                    return {cfg, "Unexpected positional argument: " + std::string(token)};
                }
            }
        }

        for (size_t j = 0; j < positionals_.size(); ++j) {
            const auto& act = positionals_[j];
            if (!pos_satisfied[j] && !act->is_optional_pos() && !act->is_variadic_pos()) {
                return {cfg, "Missing required positional argument: " + act->pos_name};
            }
        }

        return {cfg, std::nullopt};
    }

    void print_help(std::ostream& os = std::cout) const {
        os << "Usage: " << (program_name_.empty() ? "app" : program_name_);
        if (!options_.empty()) {
            os << " [OPTIONS]";
        }
        for (const auto& pos : positionals_) {
            if (pos->is_variadic_pos()) {
                os << " [" << pos->pos_name << "...]";
            } else if (pos->is_optional_pos()) {
                os << " [" << pos->pos_name << "]";
            } else {
                os << " <" << pos->pos_name << ">";
            }
        }
        os << "\n";

        if (!description_.empty()) {
            os << "\nDescription:\n  " << description_ << "\n";
        }

        if (!positionals_.empty()) {
            os << "\nPositional Arguments:\n";
            size_t max_len = 0;
            for (const auto& pos : positionals_) {
                max_len = std::max(max_len, pos->pos_name.length());
            }
            for (const auto& pos : positionals_) {
                os << "  " << std::left << std::setw(static_cast<int>(max_len + 4)) << pos->pos_name
                   << pos->description << "\n";
            }
        }

        if (!options_.empty()) {
            os << "\nOptions:\n";
            std::vector<std::string> opt_headers;
            size_t max_len = 0;
            for (const auto& opt : options_) {
                std::string header;
                if (!opt->short_opt.empty() && !opt->long_opt.empty()) {
                    header = opt->short_opt + ", " + opt->long_opt;
                } else if (!opt->short_opt.empty()) {
                    header = opt->short_opt;
                } else {
                    header = "    " + opt->long_opt;
                }

                std::string hint = opt->type_hint();
                if (!hint.empty()) {
                    header += " " + hint;
                }
                max_len = std::max(max_len, header.length());
                opt_headers.push_back(std::move(header));
            }

            for (size_t i = 0; i < options_.size(); ++i) {
                os << "  " << std::left << std::setw(static_cast<int>(max_len + 4)) << opt_headers[i]
                   << options_[i]->description << "\n";
            }
        }

        if (!examples_.empty()) {
            os << "\nExamples:\n";
            for (const auto& [cmd, desc] : examples_) {
                os << "  # " << desc << "\n";
                os << "  $ " << cmd << "\n\n";
            }
        }

        if (!notes_.empty()) {
            os << "Notes:\n";
            for (const auto& note : notes_) {
                os << "  * " << note << "\n";
            }
        }
    }

private:
    std::shared_ptr<Action> find_option(std::string_view key) const {
        for (const auto& opt : options_) {
            if (opt->short_opt == key || opt->long_opt == key) {
                return opt;
            }
        }
        return nullptr;
    }

    std::string description_;
    mutable std::string program_name_;
    std::vector<std::shared_ptr<Action>> options_;
    std::vector<std::shared_ptr<Action>> positionals_;
    std::vector<std::pair<std::string, std::string>> examples_;
    std::vector<std::string> notes_;
};
}   // namespace parse_args

}   // namespace internal