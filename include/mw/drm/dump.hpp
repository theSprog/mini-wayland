/**
 * @file drm/dump.hpp
 * @brief 资源拓扑的人类可读打印 —— 相当于我们自己实现的 modetest
 *
 * 用途有两个，都是学习性的：
 *
 * 1. **和 modetest 对照**。启动时把我们自己枚举出来的东西完整打一遍，
 *    逐行和 `modetest -M vsdrm -c -p` 比对。对不上就说明枚举写错了 ——
 *    这比等到 atomic commit 返回 EINVAL 再回头猜要高效得多。
 *
 * 2. **理解 possible_crtcs**。dump_topology() 把位图展开成可读的
 *    "encoder 141 -> [CRTC 84]"，一眼看出为什么不能随便配对。
 *
 * 分层例外：这是**唯一**允许做 modifier vendor 解码的地方。
 * 主逻辑（allocator / render / wayland）不得包含本头文件。
 * 见 README 的"modifier 不透明"约束。
 *
 * 性能：这些函数全部只在启动 / modeset / 出错诊断时调用，
 * 内部随便分配字符串。**不要在帧循环里调**。
 */
#pragma once

#include <string>

#include "mw/core/error.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {

class Device;
class PropertyMap;
struct Connector;
struct Crtc;
struct Encoder;
struct ModeInfo;
struct Plane;

/**
 * @brief render node 的同步相关能力
 *
 * 单独探测是因为：**syncobj 是渲染侧特性，KMS 节点上没有很正常。**
 * 真机上 KMS 节点和 render node 常常是同一个 PCI 设备的两个 DRM 节点
 * （勘察结果里 card0 / card2 就是这样），能力集不同。
 * Step 6 的 linux-drm-syncobj-v1 依赖的是这一侧的 timeline syncobj。
 */
struct RenderSyncCaps {
    std::string driver_name{};
    bool syncobj = false;
    bool syncobj_timeline = false;
    bool prime_import = false;
    bool prime_export = false;
};

Result<RenderSyncCaps> probe_render_node_sync_caps(const std::string& render_path);

/**
 * @brief 紧凑摘要 —— 默认输出
 *
 * 目标是**一屏能看完、能直接贴进聊天窗口**：每个 connector / crtc / plane
 * 各一行，不展开属性表。跨环境对比、贴给别人看问题，用这个。
 *
 * 全量属性表用 dump_device()，几百行，适合重定向到文件慢慢翻。
 */
void dump_summary(const Device& dev);

/**
 * @brief 自检：断言我们真正依赖的那些不变量
 *
 * 比"肉眼比对 modetest 输出"高效得多 —— 我们关心的其实只有十来条性质
 * （每个 CRTC 有没有 primary plane、connected 的 connector 有没有可用
 * encoder、pick_output 能不能跑通、选中的 plane 支不支持 XR24……），
 * 与其把几百行 dump 读一遍，不如让程序直接判定。
 *
 * 每条打一行 `PASS` / `FAIL` / `WARN`。
 *
 * @return 失败的条数（0 表示全过）
 */
size_t run_self_checks(const Device& dev);

/// 全量打印：caps + 所有 connector / crtc / plane / 属性。几百行，建议重定向到文件。
void dump_device(const Device& dev);

/**
 * @brief 原样打印 IN_FORMATS blob 的内部结构，并做自洽性校验
 *
 * 存在的理由：老版本 modetest 根本不打印 IN_FORMATS，没法拿它做交叉验证。
 * 与其猜，不如把 blob 的头部字段和每条 `drm_format_modifier` 记录原样打出来，
 * 再验证几条应该成立的等式：
 *
 *   - `version == FORMAT_BLOB_CURRENT`
 *   - `formats_offset + count_formats*4 <= blob 长度`
 *   - `modifiers_offset + count_modifiers*24 <= blob 长度`
 *   - `sum over records of popcount(formats bitmask) == 我们解析出的 pair 数`
 *   - 每条记录的 `offset + 63` 不超过 `count_formats`（否则位图指向不存在的格式）
 *
 * 数据自洽就说明解析是对的，不需要外部工具背书。
 */
void dump_in_formats_raw(const Device& dev, const Plane& plane);

/// connector -> encoder -> CRTC 连通图，possible_crtcs 位图展开成 CRTC id 列表
void dump_topology(const Device& dev);

void dump_connector(const Device& dev, const Connector& conn);
void dump_crtc(const Device& dev, const Crtc& crtc);
void dump_plane(const Device& dev, const Plane& plane);
void dump_encoder(const Device& dev, const Encoder& enc);

/// 打印属性表：名字、id、类型、immutable、当前值。enum 类型展开取值列表。
void dump_properties(const Device& dev, const PropertyMap& props, const char* label);

/**
 * @brief 完整 modeline
 *
 * clock / hdisplay / hsync_start / hsync_end / htotal /
 * vdisplay / vsync_start / vsync_end / vtotal / vrefresh / flags / type。
 * 学会读这一行，VRR 和 frame pacing 才有基础 —— 帧时长是
 * htotal * vtotal / clock，而不是 vrefresh 那个取整过的数。
 */
std::string describe_mode(const ModeInfo& mode);

/**
 * @brief modifier 的人类可读名，如 "LINEAR" 或 "VENDOR(0x0b):0x0000000000002000"
 *
 * **只用于日志**。任何主逻辑分支依赖这个函数的返回值都是 bug。
 * 未知 vendor 不猜，原样打十六进制。
 */
std::string describe_modifier(Modifier mod);

/// fourcc + modifier 的组合描述，如 "XR24/LINEAR"
std::string describe_format(const FormatModifier& fm);

} // namespace mw::drm
