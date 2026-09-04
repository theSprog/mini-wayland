/**
 * @file version.hpp
 * @brief 版本号 —— 只有在这个库被别的项目消费之后才有意义
 *
 * ## 为什么需要它
 *
 * 库被安装到 `$(PREFIX)` 之后，头文件和 `.a` 就可能各自更新。
 * 消费者最先撞到的一类问题是"头文件里有这个字段，链接进来的实现里没有"——
 * 表现为莫名其妙的链接错误或者更糟的内存布局错位。
 *
 * 编译期常量来自头文件，`runtime_version()` 编译在库里。两者对不上，
 * 就说明头和库不是同一次构建的产物。`check_abi()` 把这个检查做成一行。
 *
 * ## 版本号的含义
 *
 * 这不是一个对外承诺兼容性的库，语义版本那一套在这里不适用。
 * 约定简化成两条：
 *
 *   MINOR 跟着 step 走 —— Step N 收尾时 MINOR 置为 N。
 *   PATCH 是同一个 step 内的接口改动。
 *
 * 所以 `0.3.x` 就是"Step 3 收尾时的接口"。消费者（比如 mini-render）
 * 应当记下自己是对着哪个版本写的，见 `docs/api.md` 的稳定性分级。
 */
#pragma once

#include <cstdint>
#include <string>

#define MW_VERSION_MAJOR 0
#define MW_VERSION_MINOR 3
#define MW_VERSION_PATCH 0

/// 可用于 `#if MW_VERSION_NUMBER >= MW_VERSION_CHECK(0, 3, 0)`
#define MW_VERSION_CHECK(major, minor, patch) ((major) * 10000 + (minor) * 100 + (patch))

#define MW_VERSION_NUMBER \
    MW_VERSION_CHECK(MW_VERSION_MAJOR, MW_VERSION_MINOR, MW_VERSION_PATCH)

namespace mw {

/// 编译这个 TU 时头文件里写的版本
inline constexpr int kHeaderVersion = MW_VERSION_NUMBER;

/// 编译**库**时头文件里写的版本。实现在 src/core/version.cpp。
int runtime_version() noexcept;

/// "0.3.0"
std::string version_string();

/**
 * @brief 头与库是否来自同一次构建
 *
 * @return true 表示一致。不一致时**本函数不打日志也不 abort** ——
 *         它可能在 main() 之前的静态初始化里被调用，那时日志系统
 *         还没准备好。判断结果交给调用方处理。
 */
inline bool check_abi() noexcept {
    return runtime_version() == kHeaderVersion;
}

} // namespace mw
