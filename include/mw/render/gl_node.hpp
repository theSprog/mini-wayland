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
    bool scanout_accepted_by_kms = false;

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
     * @brief KMS 设备的 fd
     *
     * 用来做两件事：分配一块外来 buffer 去测导入方向、以及把候选节点分配的
     * buffer 拿来注册 fb 去测导出方向。
     *
     * 无效时这两项跳过（记为 false），只测到 EGL 为止 —— 没有显示设备的
     * 机器上依然可以回答"哪个节点能跑 GL"。
     */
    BorrowedFd kms_fd{};

    /// KMS 节点路径。会作为候选之一 —— 在某些拓扑下能跑 GL 的恰恰是它。
    std::string kms_path{};

    /// 试分配尺寸。小一点快，但太小的 stride 可能绕过对齐检查。
    drm::Size test_size{256, 256};
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
