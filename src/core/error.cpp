#include "mw/core/error.hpp"

#include <cstring>
#include <utility>

namespace mw {
namespace {

/// errno 域用的 IError 实现。message 在构造时就拼好，因为
/// Error 本来就要拷贝一份 std::string，多一次也无妨。
struct SysError final : IError {
    explicit SysError(int err) noexcept : err_(err), msg_() {
        // strerror_r 的 GNU 版本返回 char*，可能不写进 buf_。
        char buf[128];
        const char* text = ::strerror_r(err, buf, sizeof(buf));
        msg_ = text != nullptr ? text : "unknown error";
    }

    int error_code() const noexcept override {
        return err_;
    }

    const char* error_message() const noexcept override {
        return msg_.c_str();
    }

    const char* error_domain() const noexcept override {
        return kSysDomain;
    }

  private:
    int err_;
    std::string msg_;
};

} // namespace

// SourceLocation 是 3 个指针 + 一个 int 的 POD，按值传比 const& 更便宜，
// 而且默认实参 SourceLocation::current() 必须在调用点求值。
// cppcheck-suppress passedByValue
unexpected<Error> sys_err(const char* what, int err, SourceLocation loc) {
    std::string msg;
    msg.reserve(64);
    msg += what;
    msg += ": ";
    {
        const SysError se(err);
        msg += se.error_message();
    }
    msg += " (errno=";
    msg += std::to_string(err);
    msg += ")";
    return unexpected<Error>(Error(kSysDomain, err, std::move(msg), loc));
}

unexpected<Error> sys_err_ctx(const char* what, std::string_view context, int err,
                              // cppcheck-suppress passedByValue
                              SourceLocation loc) {
    std::string msg;
    msg.reserve(96);
    msg += what;
    msg += "('";
    msg.append(context.data(), context.size());
    msg += "'): ";
    {
        const SysError se(err);
        msg += se.error_message();
    }
    msg += " (errno=";
    msg += std::to_string(err);
    msg += ")";
    return unexpected<Error>(Error(kSysDomain, err, std::move(msg), loc));
}

bool is_errno(const Error& e, int errno_value) noexcept {
    return e.in_domain(kSysDomain) && e.code == errno_value;
}

int errno_of(const Error& e) noexcept {
    return e.in_domain(kSysDomain) ? e.code : 0;
}

} // namespace mw
