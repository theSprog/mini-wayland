/**
 * @file render/buffer_source.hpp
 * @brief scanout buffer 的两条分配路径
 *
 * ## 问题
 *
 * 当显示设备与渲染设备是两个独立的 DRM 节点时，一块既要 GPU 画、
 * 又要显示控制器扫描的 buffer 必须在两边都建立起引用。
 * 谁做最初的分配，有两个互不等价的答案。
 *
 * ## ScanoutDevice：显示侧分配
 *
 *   dumb_create(KMS 节点) -> GEM handle 已在 KMS 节点上 -> addfb2 直接可用
 *                         -> 导出 dmabuf -> 渲染节点导入为渲染目标
 *
 *  - **不经过 KMS 节点的 PRIME 导入路径**。导入发生在渲染侧。
 *    显示驱动对导入内存的要求（连续性、对齐）因此完全不参与。
 *  - 对齐由 KMS 驱动自己的 dumb 分配器保证 —— 它分配、它校验，
 *    两边用的是同一套规则。
 *  - **只有线性排布**。dumb buffer 没有 modifier 可谈。
 *
 * ## RenderDevice：渲染侧分配
 *
 *   gbm_bo_create_with_modifiers(渲染节点) -> gbm_bo_get_fd()
 *     -> PRIME_FD_TO_HANDLE(KMS 节点) -> addfb2
 *
 *  - modifier 协商在这里才有内容：候选来自目标 plane 的 IN_FORMATS，
 *    由分配器挑一个。这是 Step 4 dmabuf-feedback tranche 的雏形。
 *  - 依赖 KMS 节点能导入外部 dmabuf。能否成功取决于两个设备的内存
 *    管理关系：共用管理器时通常直接通过，否则可能有连续性要求。
 *  - stride 由渲染侧分配器决定，**未必满足显示侧的对齐要求**。
 *    实现应在 addfb2 之前用 `drm::probe_pitch_alignment()` 的结果自查，
 *    因为内核对此往往只回一个看不出原因的 EINVAL。
 *
 * ## 为什么两个都做
 *
 * 不是冗余。二者对应两种真实策略：**"让约束最严的设备分配"** vs
 * **"把约束告诉分配者让它挑"**。后者是现代合成器的做法，前者在
 * 嵌入式栈里很常见（Mesa 的 kmsro 走的就是前者）。
 *
 * 同一个接口下两条路都能跑通，才说明接口没有漏进对某种拓扑的假设。
 * Step 2 起 VKMS 因为没有 render node 退出端到端验收，这两条路径
 * 接替它做通用性试金石。
 *
 * **两条路都保留，即使其中一条在当前环境下不通。** 能不能走通取决于两个
 * 设备的内存管理关系，而那是随驱动与硬件演进变化的东西 —— 把任何一条
 * 删掉或标成"备选"，都是把某个时间点的环境状态固化成了架构。
 * 可用性一律由 `probe_buffer_sources()` 运行时回答。
 *
 * 这条约束已经被验证过一次：渲染侧分配 -> 显示侧导入这个方向一度不通，
 * 后来驱动修好了，**用户态一行没改**就从不可用变成可用。
 * 具体是哪一版、当时的失败形状如何，记在 `docs/env.md`，不写在这里 ——
 * 接口契约里不该出现某个时间点的环境状态。
 *
 * 开发顺序建议 **先 ScanoutDevice 后 RenderDevice**：前者跑通意味着
 * 渲染目标绑定、绘制、提交全部验证过；此时后者若失败，可以确定是
 * 跨设备导入的问题，而不是自己的代码。
 *
 * ## 本文件不做的事
 *
 * 不解码 modifier、不判断设备型号、不为任何具体驱动写分支。
 * 能力一律运行时探测（见 `probe_buffer_sources()`）。
 * 关于具体硬件的观察记录在 `docs/` 下，不写进接口契约。
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mw/core/error.hpp"
#include "mw/drm/framebuffer.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/types.hpp"

namespace mw::render {

using drm::Format;
using drm::Modifier;
using drm::Size;

// ---------------------------------------------------------------------------
// 分配结果
// ---------------------------------------------------------------------------

/**
 * @brief 一块两个设备都认识的 buffer
 *
 * 无论走哪条路径，产出都归一成这个类型。上层（swapchain、渲染器、
 * plane 分配器）只看见它，看不见是谁分配的。
 *
 * move-only。持有 fb_id 与 dmabuf fd 的所有权。
 */
class ScanoutBuffer {
  public:
    ScanoutBuffer() noexcept;
    ~ScanoutBuffer();

    ScanoutBuffer(ScanoutBuffer&&) noexcept;
    ScanoutBuffer& operator=(ScanoutBuffer&&) noexcept;
    ScanoutBuffer(const ScanoutBuffer&) = delete;
    ScanoutBuffer& operator=(const ScanoutBuffer&) = delete;

    Size size() const noexcept;
    Format format() const noexcept;

    /// **实际拿到的** modifier，未必等于请求列表里的第一项。
    /// kModifierInvalid 表示走了不带 modifier 的 addfb2 路径。
    Modifier modifier() const noexcept;

    /// KMS 节点上的 fb_id，可以直接绑给 plane
    drm::FbId fb_id() const noexcept;

    /// 跨设备/跨进程传递用的描述。fd 借用，所有权仍在本对象。
    const drm::DmabufDesc& dmabuf() const noexcept;

    /// 行跨距，字节。**不等于** width * bpp / 8。
    uint32_t stride() const noexcept;

    // ---- CPU 访问（可选能力）----

    /**
     * @brief 是否支持 CPU 直接写像素
     *
     * dumb buffer 恒 true；GBM 分配取决于 modifier 是否线性以及是否可映射。
     *
     * Step 2 的第一个 demo 用 CPU 填色验证整条链路，确认无误后再接 GL ——
     * 这样 GL 出问题时能立刻排除是链路问题。
     */
    bool cpu_writable() const noexcept;

    /**
     * @brief 只写映射
     *
     * @warning 显存映射常常是写合并甚至非缓存的。顺序写尚可，读回极慢。
     *          按行顺序写，不要做 read-modify-write。需要读改写请在普通
     *          内存里画完再整块拷进来。
     */
    Result<span<uint8_t>> map_write();

    /**
     * @brief 结束一次 CPU 写，释放映射
     *
     * 默认不需要调用：映射一直保持到析构，热路径上一次 map/unmap 都没有。
     *
     * **但"保持映射"隐含了一个假设：映射指向的就是 dmabuf 背后那块内存。**
     * 这个假设在 dumb 路径上必然成立，在 GBM 路径上不一定 —— 底层资源不是
     * host-visible 时，`gbm_bo_map()` 可能返回一块 staging buffer，真正的
     * 拷回发生在 `gbm_bo_unmap()` 里。那种情况下不 unmap 就等于没写。
     *
     * 提供这个入口是为了让上层能把假设变成可测的东西：每帧配对调用一次，
     * 画面从黑变对，就说明分配器走的是 staging 路径。
     *
     * @warning 每帧调用会在热路径上引入 map/unmap，违反 Step 2 的稳态
     *          ioctl 约束。**它是诊断手段，不是常规用法。**
     */
    void end_cpu_write() noexcept;

    std::string to_string() const;

    bool valid() const noexcept;

  private:
    friend class ScanoutDeviceSource;
    friend class RenderDeviceSource;

    // pimpl：两条路径的持有物不同（dumb 的 GEM 对象 vs GBM 的 bo），
    // 但这个差异不该出现在头文件里，否则上层就被迫认识 gbm 和 dumb。
    explicit ScanoutBuffer(std::unique_ptr<struct ScanoutBufferImpl> impl) noexcept;
    std::unique_ptr<struct ScanoutBufferImpl> impl_;
};

// ---------------------------------------------------------------------------
// 分配请求
// ---------------------------------------------------------------------------

struct AllocRequest {
    Size size{};
    Format format{};

    /**
     * @brief 可接受的 modifier 候选，按调用方的偏好排序
     *
     * 来源应当是目标 plane 的 IN_FORMATS 里该 format 对应的全部 modifier，
     * **原样转发，不做 vendor 解码**（见 README 硬约束）。
     *
     * 空列表 = 不指定，走不带 modifier 的分配路径。
     *
     * @note IN_FORMATS **不保证是可用组合的完备清单**。驱动可能宣告出
     *       addfb2 能过但 atomic_check 拒绝的组合。所以分配成功不代表
     *       能上屏，调用方必须做一次 TEST_ONLY，并准备好换一个重试。
     */
    span<const Modifier> modifiers{};

    /// 要求 CPU 可写。为 true 时实现会排除不可映射的 modifier。
    bool need_cpu_write = false;
};

// ---------------------------------------------------------------------------
// 分配器
// ---------------------------------------------------------------------------

enum class SourceKind {
    ScanoutDevice,  ///< 显示节点分配（dumb），线性
    RenderDevice,   ///< 渲染节点分配（GBM），可协商 modifier
};

const char* to_string(SourceKind kind) noexcept;

class BufferSource {
  public:
    BufferSource(const BufferSource&) = delete;
    BufferSource& operator=(const BufferSource&) = delete;
    virtual ~BufferSource() = default;

    virtual SourceKind kind() const noexcept = 0;

    /**
     * @brief 分配一块 buffer 并注册为 KMS 节点上的 fb
     *
     * 两条路径的差异全部收敛在这个函数里：
     *   ScanoutDevice —— dumb_create + addfb2（无 PRIME 导入）+ 导出 dmabuf
     *   RenderDevice  —— GBM 分配 + 导出 + PRIME 导入 + addfb2
     *
     * 失败时 message 里要带上走到了哪一步。stride 不满足目标设备的对齐
     * 是一个高频失败点，实现应当用 `drm::probe_pitch_alignment()` 的结果
     * 在 addfb2 之前自查并明确报错，而不是让内核回一个看不出原因的 EINVAL。
     */
    [[nodiscard]] virtual Result<ScanoutBuffer> allocate(const AllocRequest& req) = 0;

    /**
     * @brief 本分配器在给定格式下能产出的 modifier
     *
     * ScanoutDevice 恒返回空（只有线性，且走无 modifier 路径）。
     * RenderDevice 返回分配器声称可用的集合。
     *
     * 调用方拿它与 plane 的 IN_FORMATS 求交，得到真正的候选列表。
     */
    [[nodiscard]] virtual std::vector<Modifier> available_modifiers(Format format) const = 0;

    /// 人类可读的一行，启动时打一次
    virtual std::string describe() const = 0;

  protected:
    BufferSource() = default;
};

// ---------------------------------------------------------------------------
// 构造与探测
// ---------------------------------------------------------------------------

/// 显示侧分配。只需要 KMS fd。
Result<std::unique_ptr<BufferSource>> make_scanout_device_source(BorrowedFd kms_fd,
                                                                 drm::HandleCache& cache);

/**
 * @brief 渲染侧分配。需要 render node 路径。
 *
 * 内部打开 render node 并创建 GBM 设备。render node 的 fd 由本对象持有，
 * 与 KMS fd 严格分离。
 */
Result<std::unique_ptr<BufferSource>> make_render_device_source(BorrowedFd kms_fd,
                                                                const std::string& render_node_path,
                                                                drm::HandleCache& cache);

/// 一次探测的结论
struct SourceProbe {
    SourceKind kind = SourceKind::ScanoutDevice;
    bool usable = false;
    std::string detail{};  ///< 不可用时的原因，可直接打给用户
};

/**
 * @brief 实际试分配一小块 buffer，判断每条路径是否可用
 *
 * 不靠 caps 推断。跨设备导入能否成功取决于两个设备的内存管理关系、
 * 分配出的内存是否满足目标设备的要求、stride 是否对齐 —— 这些都不是
 * `drmGetCap` 能回答的。**唯一可靠的判断方式是真的分配一次。**
 *
 * 不需要 DRM master（addfb2 不需要）。
 *
 * @param size 试分配尺寸。小尺寸快且省显存，但小 buffer 的 stride 可能
 *             小于对齐值从而绕过对齐检查 —— 要验证对齐问题请传真实分辨率。
 */
std::vector<SourceProbe> probe_buffer_sources(BorrowedFd kms_fd,
                                              const std::string& render_node_path, Size size);

} // namespace mw::render