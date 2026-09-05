#include "mw/version.hpp"

#include <string>

namespace mw {

// 这个 TU 编译进库里，所以它看到的是**构建库时**的头文件。
// 消费者看到的是**安装后**的头文件。两者不一致就是安装漏了一半。
int runtime_version() noexcept {
    return MW_VERSION_NUMBER;
}

std::string version_string() {
    return std::to_string(MW_VERSION_MAJOR) + "." + std::to_string(MW_VERSION_MINOR) + "." +
           std::to_string(MW_VERSION_PATCH);
}

} // namespace mw
