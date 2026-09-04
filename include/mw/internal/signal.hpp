#pragma once

#include <csignal>
#include <initializer_list>
#include <utility>
#include <vector>


namespace internal {
namespace sig {

struct special_action {
    enum type { ignore, default_action } value;
};

inline constexpr special_action ignore{special_action::ignore};
inline constexpr special_action default_action{special_action::default_action};

using func_sig_t = void (*)(int);
using func_void_t = void (*)();

struct handler {
    enum class kind { none, func_sig, func_void, ignore_signal, use_default } type{kind::none};
    union {
        func_sig_t  fn_sig;
        func_void_t fn_void;
    };

    constexpr handler() noexcept : type(kind::none), fn_sig(nullptr) {}
    constexpr handler(func_sig_t fn) noexcept : type(kind::func_sig), fn_sig(fn) {}
    constexpr handler(func_void_t fn) noexcept : type(kind::func_void), fn_void(fn) {}
    constexpr handler(special_action act) noexcept
        : type(act.value == special_action::ignore ? kind::ignore_signal : kind::use_default),
          fn_sig(nullptr) {}
};

struct rule {
    int signo;
    handler h;

    constexpr rule(int sig, func_sig_t fn) noexcept : signo(sig), h(fn) {}
    constexpr rule(int sig, func_void_t fn) noexcept : signo(sig), h(fn) {}
    constexpr rule(int sig, special_action act) noexcept : signo(sig), h(act) {}
};

inline constexpr int max_signals = 64;
inline handler g_handlers[max_signals]{};

inline void dispatcher(int signo) noexcept {
    if (signo >= 0 && signo < max_signals) {
        const auto& h = g_handlers[signo];
        if (h.type == handler::kind::func_sig && h.fn_sig) {
            h.fn_sig(signo);
        } else if (h.type == handler::kind::func_void && h.fn_void) {
            h.fn_void();
        }
    }
}

class guard {
public:
    guard() noexcept : entries_() {}
    ~guard() { restore(); }

    guard(const guard&) = delete;
    guard& operator=(const guard&) = delete;

    guard(guard&& other) noexcept : entries_(std::move(other.entries_)) {
        other.entries_.clear();
    }

    guard& operator=(guard&& other) noexcept {
        if (this != &other) {
            restore();
            entries_ = std::move(other.entries_);
            other.entries_.clear();
        }
        return *this;
    }

    void restore() noexcept {
        for (const auto& saved : entries_) {
            ::sigaction(saved.signo, &saved.old_sa, nullptr);
            if (saved.signo >= 0 && saved.signo < max_signals) {
                g_handlers[saved.signo] = saved.old_handler;
            }
        }
        entries_.clear();
    }

    void dismiss() noexcept {
        entries_.clear();
    }

private:
    friend guard bind(std::initializer_list<rule>);

    struct entry {
        int signo;
        struct sigaction old_sa;
        handler old_handler;
    };

    std::vector<entry> entries_;
};

[[nodiscard]] inline guard bind(std::initializer_list<rule> rules) {
    guard g;
    g.entries_.reserve(rules.size());

    for (const auto& r : rules) {
        if (r.signo < 0 || r.signo >= max_signals) {
            continue;
        }

        struct sigaction new_sa{};
        struct sigaction old_sa{};

        if (r.h.type == handler::kind::ignore_signal) {
            new_sa.sa_handler = SIG_IGN;
        } else if (r.h.type == handler::kind::use_default) {
            new_sa.sa_handler = SIG_DFL;
        } else {
            new_sa.sa_handler = &dispatcher;
            new_sa.sa_flags = SA_RESTART;
        }
        ::sigemptyset(&new_sa.sa_mask);

        if (::sigaction(r.signo, &new_sa, &old_sa) == 0) {
            g.entries_.push_back({r.signo, old_sa, g_handlers[r.signo]});
            g_handlers[r.signo] = r.h;
        }
    }

    return g;
}

} // namespace sig
} // namespace internal