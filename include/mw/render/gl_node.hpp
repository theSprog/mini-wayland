/**
 * @file render/gl_node.hpp
 * @brief 哪个 DRM 节点能真正承载 GL 渲染 —— 靠实测，不靠元数据
 *
 * ## 为什么不能直接用 drmGetDevice2 的配对结果
 *
 * `drm::find_render_node()` 回答的是"**与这个 KMS 节点同属一个物理设备的
 * render node 是哪个**"。这是一条元数据关系，libdrm 按总线地址聚合得出。
 *
 * 它回答不了我们真正要问的问题：**哪个节点上跑得起 GL**。两者在下面这些
 * 情况下会分叉，而且都不罕见：
 *
 *  - 一个物理设备下挂着多个功能不同的 DRM 节点（显示、渲染、编解码），
 *    它们共享总线地址，于是"配对"结果取决于枚举顺序，不取决于能力。
 *  - 用户态驱动的覆盖面与内核节点不是一一对应：某个节点有 KMD 却没有
 *    对应的 UMD，GL 栈会静默退到软件光栅化，而**软件光栅化是能跑通的**——
 *    它不报错，只是慢一百倍，且分配不出可扫描输出的内存。
 *  - 反过来，承载 3D 的节点未必与显示节点同属一个物理设备。
 *
 * 最后一种情况最危险：**探测会"成功"**，然后你以为自己在 GPU 上跑。
 *
 * ## 判据只有一条：真的建一次
 *
 * 对每个候选节点依次做：
 *
 *   1. `gbm_create_device`
 *   2. EGL 初始化 + 建 GLES 上下文
 *   3. 把一块**外来的** dmabuf 导入并绑成渲染目标（"别人分配我来画"）
 *   4. 自己分配一块 scanout 用途的 bo，再交给 KMS 设备注册成 fb
 *      （"我分配别人来扫"）
 *
 * 第 3 和第 4 条是**两个方向，都要测，都不预设哪个是主路**。
 * 不同的硬件与不同的驱动成熟度下能走通的方向不一样，而且会随驱动演进变化。
 * 把任何一个方向写死进代码，都是把当下这块板子的状态当成了架构。
 *
 * @note 这四步之外的东西一概不判断。特别是**不做任何厂商名/驱动名匹配**：
 *       节点叫什么、驱动叫什么，与它能不能干活没有可靠关系。
 *
 * ## 每个候选跑在独立进程里
 *
 * 这一步要做的事情，本质上是**把一堆来路不明的用户态驱动依次加载进本进程**。
 * 其中一定会有和这块板子无关的（视频编解码节点、别的 IP 的节点），
 * 它们的 UMD 从来没被这样用过。实测就撞上了：某个节点的 EGL 实现在
 * `eglCreateImageKHR` 里直接段错误，整个探测工具随之死掉，
 * **后面的节点一个都没测到 —— 包括那个真正能用的**。
 *
 * 进程内是防不住的：崩在第三方 `.so` 里，没有任何返回值可以检查。
 * 所以每个候选 `fork()` 一个子进程去试，结果经管道回传。子进程崩了就记一条
 * "这个节点的驱动崩了"，继续测下一个 —— 而且这条记录本身就是有价值的发现。
 *
 * @warning **必须在本进程初始化 EGL / GL 之前调用。** fork 一个已经带着
 *          GL 上下文的进程，子进程里的驱动状态是未定义的。
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/types.hpp"
#include "mw/render/target.hpp"

namespace mw::render {

/// 一个节点作为 GL 渲染宿主的实测结果
struct GlNode {
    std::string path{};
    std::string drm_driver{};  ///< DRM driver name，仅用于显示

    // ---- 逐级递进，前一级失败后面就没测 ----
    bool gbm = false;  ///< gbm_create_device 成功
    bool egl = false;  ///< EGL 初始化 + GLES 上下文成功

    /// 外来 dmabuf 能被导入并绑成渲染目标（"别人分配，我来画"）
    bool renders_into_imported = false;

    /// 自己能分配带 scanout 用途的 bo（"我分配"）
    bool allocates_scanout = false;

    /// 自己分配的 bo 能被 KMS 设备导入并注册成 fb（"我分配，别人来扫"）
    ///
    /// @warning `same_device_as_kms` 为真时这一项是**退化结果**：分配方就是
    ///          显示设备自己，当然收得下。它不说明任何跨设备能力。
    bool scanout_accepted_by_kms = false;

    /// 这个候选就是 KMS 节点本身
    ///
    /// 不是问题 —— 显示节点上跑得起硬件 GL 是一种真实且好用的拓扑。
    /// 但它会让上面那一项退化，所以排序和报告都要把它单独拿出来说。
    bool same_device_as_kms = false;

    /// 走通的绑定路径。renders_into_imported 为 false 时无意义。
    AttachKind attach_kind = AttachKind::Renderbuffer;

    std::string gl_renderer{};
    std::string gl_version{};
    std::string egl_version{};

    // ---- 后续步骤要用的能力，顺手在这里一次问清 ----
    //
    // 这些必须问**实际承载渲染的那个节点**。问错节点得到的答案会一路带偏
    // 后面的设计决定，而且不会报错 —— 这正是本文件存在的理由。

    /// EGL_EXT_image_dma_buf_import_modifiers。缺了只能可靠导入线性 buffer。
    bool egl_import_modifiers = false;

    /// EGL_ANDROID_native_fence_sync。Step 6 从 GL 侧取 fence fd 靠它。
    bool egl_native_fence_sync = false;

    /// DRM_CAP_SYNCOBJ / DRM_CAP_SYNCOBJ_TIMELINE。
    /// 后者是 linux-drm-syncobj-v1 协议能否提供给客户端的前提。
    bool syncobj = false;
    bool syncobj_timeline = false;

    /**
     * @brief 探测这个节点时子进程被信号杀掉了
     *
     * 这本身是结论，不是工具故障。但**成因未必在用户态**：内核里的
     * `BUG_ON` 也会把当前任务打成 SIGSEGV，从 `waitpid` 看和用户态段错误
     * 一模一样。实测就撞上过后者（见 docs/step2-probe-results.md）。
     * 所以报告里要提醒去看 dmesg —— 那是唯一能分开这两种情况的地方。
     */
    bool crashed = false;

    /// 按要求跳过了，没有测
    bool skipped = false;

    /// 第一处失败的原因，可直接打给用户。全通过时为空。
    std::string detail{};

    /**
     * @brief GL_RENDERER 看起来像软件光栅化吗
     *
     * **仅用于排序提示与日志，主逻辑不得依赖。**
     *
     * 这不是厂商判断：匹配的是通用图形栈里那几个众所周知的软件后备实现的
     * 名字，它们在任何机器上都可能出现，与具体板卡无关。之所以需要它，
     * 是因为软件光栅化**能通过上面全部四条测试**（它只是慢），单靠能力
     * 判据分不出来。
     *
     * 只在多个节点能力相当时用来打破平局，并且一定要打印出来让人复核。
     */
    bool looks_like_software() const noexcept;

    /// 排序用的分数。**不是正确性判据**，只用于"没人指定时挑一个"。
    int rank() const noexcept;

    /// 一行摘要，表格用
    std::string to_line() const;
};

/// 探测输入
struct GlNodeProbe {
    /**
     * @brief KMS 节点路径
     *
     * 两个用途：分配一块外来 buffer 去测导入方向、把候选节点分配的 buffer
     * 拿来注册 fb 去测导出方向。它自己也是候选之一 —— 某些拓扑下能跑 GL 的
     * 恰恰是显示节点。
     *
     * 传路径而不是 fd：每个子进程**自己打开**，这样子进程崩掉时它建的
     * GEM 对象和 fb 随那个 fd 一起消失。共用父进程的 fd 的话，
     * 崩溃会在父进程的 fd 上留下一堆没人认领的内核对象。
     *
     * 空字符串表示没有显示设备：只测到 EGL 为止，导入/导出两项记为 false。
     */
    std::string kms_path{};

    /// 试分配尺寸。小一点快，但太小的 stride 可能绕过对齐检查。
    drm::Size test_size{256, 256};

    /**
     * @brief 不去碰的节点
     *
     * 探测本身有代价：每个候选都要把一套用户态驱动加载进来跑一遍。
     * 遇到会把内核打 oops 的驱动时，这个代价不只是时间 —— 每跑一次工具
     * 就多一次 oops，机器状态越来越脏。
     *
     * 所以留一个显式的排除口。**默认为空**：跳过谁是使用者的决定，
     * 不是探测器替他做的判断，而且被跳过的节点会在结果里标出来，
     * 不会变成一条看不见的假设。
     */
    std::vector<std::string> skip{};

    /**
     * @brief 每个候选是否 fork 到独立进程里去试
     *
     * 默认开。关掉只有一个用途：**用 gdb 直接看崩在哪**。
     * 关掉之后任何一个节点的驱动崩溃都会带走整个进程。
     */
    bool isolate = true;
};

/**
 * @brief 逐个节点实测，返回全部结果（包括失败的）
 *
 * 候选集合 = `/dev/dri` 下所有 render node + KMS 节点本身。
 * **不过滤、不提前放弃**：失败的节点也留在结果里并带上原因，
 * 因为"这个节点为什么不行"往往就是要查的东西。
 *
 * 代价是每个候选都会拉起一次 EGL。只在启动或诊断时调用。
 */
std::vector<GlNode> probe_gl_nodes(const GlNodeProbe& probe);

/**
 * @brief 从探测结果里挑一个
 *
 * 按 `rank()` 取最高的。全都不可用时返回 nullptr。
 *
 * 返回的是 nodes 里的元素，生命周期跟着它走。
 */
const GlNode* best_gl_node(span<const GlNode> nodes) noexcept;

} // namespace mw::render
