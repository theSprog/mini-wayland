/**
 * @file render/swapchain.hpp
 * @brief N 组 (ScanoutBuffer + 可选 GlRenderTarget) 的轮转
 *
 * ## 为什么自己写，而不是用 gbm_surface
 *
 * `gbm_surface_create` + `eglSwapBuffers` + `gbm_surface_lock_front_buffer`
 * 是最短路径，kmscube 走的就是它。本项目不用，因为它把三样东西藏进了
 * Mesa 内部，而这三样恰好是后面几步要控制的：
 *
 *   - **buffer 有几个**。Step 7 的 frame pacing 要按队列深度调度。
 *   - **什么时候可以复用**。Step 6 的 release fence 就是这个问题的答案。
 *   - **每个 buffer 的 fence**。同上。
 *
 * 代价是这个文件本身。收益是它可以被改写 —— 到 Step 6 只需要给 Slot
 * 加一个 fence 字段，而不是把 eglSwapBuffers 那条路整条拆掉重写。
 *
 * ## 一次性建好，运行期只切下标
 *
 * 分配、addfb2、导入 EGLImage、建 FBO 全部在 `create()` 里做完。
 * 这是 Step 2 验收标准里\"稳态每帧 0 次 prime / addfb\"的实现方式：
 * 不是靠自觉不调，是靠热路径上根本没有可调的东西。
 *
 * 验证不靠自觉：`drm/trace.hpp` 的计数器会在帧循环里核对。
 *
 * ## 轮转语义
 *
 * 与 `drm::DumbBufferChain` 一致，刻意做成同一套：
 *
 * @code
 *   auto& slot = chain.acquire();     // 取一个当前没在扫描的
 *   draw_into(slot);
 *   commit(slot.buffer.fb_id());
 *   chain.mark_submitted();
 *   // ... 收到 flip 完成事件后 ...
 *   chain.on_flip_complete();
 * @endcode
 *
 * **不做成阻塞式的"自动等待"接口。** "已提交但还没上屏"这个中间状态必须
 * 显式暴露给调用方 —— Step 6 的显式同步整个就是在这个状态上做文章。
 * 一个 `acquire()` 里偷偷 poll 的实现会让那一步无处下手。
 */
#pragma once

#include <array>
#include <string>

#include "mw/internal/error.hpp"
#include "mw/egl/display.hpp"
#include "mw/render/buffer_source.hpp"
#include "mw/render/target.hpp"

namespace mw::render {

struct SwapchainDesc {
    Size size{};
    Format format{};

    /// 2 = 双缓冲，3 = 三缓冲。单缓冲一定撕裂，不接受。
    uint32_t count = 2;

    /**
     * @brief 可接受的 modifier 候选，原样转发给分配器
     *
     * 来源应当是目标 plane 的 IN_FORMATS 里该 format 的全部 modifier。
     * 空 = 走不带 modifier 的分配路径。
     */
    span<const Modifier> modifiers{};

    /// 要求 CPU 可写（CPU 绘制路径）
    bool need_cpu_write = false;
};

/**
 * @brief 一组可轮转的渲染目标
 *
 * move-only。析构顺序：先 GL 对象，再 fb，再 buffer —— Slot 的成员声明
 * 顺序保证了这一点，不要调整。
 */
class Swapchain {
  public:
    static constexpr uint32_t kMaxBuffers = 4;

    /// 一个槽位持有的全部东西
    struct Slot {
        // 声明顺序即析构逆序。target 引用着 buffer 的 dmabuf，
        // 必须先于 buffer 销毁。
        ScanoutBuffer buffer{};
        GlRenderTarget target{};

        bool has_target() const noexcept {
            return target.valid();
        }
    };

    Swapchain() noexcept = default;
    ~Swapchain() = default;

    Swapchain(Swapchain&&) noexcept = default;
    Swapchain& operator=(Swapchain&&) noexcept = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    /**
     * @brief 只分配 buffer，不建 GL 渲染目标
     *
     * CPU 绘制路径用。`desc.need_cpu_write` 该为 true，否则拿不到映射。
     */
    static Result<Swapchain> create(BufferSource& source, const SwapchainDesc& desc);

    /**
     * @brief 分配 buffer 并为每一块建立 GL 渲染目标
     *
     * @param display 必须已 make_current，且必须比本对象活得久 ——
     *                GL 对象归上下文所有，上下文没了它们也没了。
     */
    static Result<Swapchain> create_with_targets(BufferSource& source,
                                                 const egl::Display& display,
                                                 const SwapchainDesc& desc);

    /// 当前可以安全写入的槽位
    Slot& acquire() noexcept;
    const Slot& acquire() const noexcept;

    /// 按下标访问，诊断与启动阶段用。越界返回 nullptr。
    Slot* at(uint32_t index) noexcept;

    /**
     * @brief 标记 acquire() 返回的那个已经提交，轮转到下一个
     *
     * @param expects_event 这次提交带了 PAGE_FLIP_EVENT 吗
     *
     *        modeset 那一次提交**不带**完成事件：它让第一块 buffer 开始被扫描
     *        （所以确实要轮转），但内核永远不会为它投递 flip 事件。
     *        传 true 的话 in_flight 会永久性地多 1，退出时那句
     *        "N 次提交还在飞"就一直多报一次 —— 一个恒定的偏差比没有计数
     *        更糟，因为它看起来像真的。
     */
    void mark_submitted(bool expects_event = true) noexcept;

    /// 收到 page flip 完成事件时调用
    void on_flip_complete() noexcept;

    uint32_t count() const noexcept {
        return count_;
    }

    /// 尚未收到完成事件的提交数。>= count() 说明该等了。
    uint32_t in_flight() const noexcept {
        return in_flight_;
    }

    /// 每块 buffer 实际拿到的 modifier 是否一致。不一致说明分配器每次都在
    /// 重新挑，值得知道 —— plane 分配器会假设同一条 swapchain 布局相同。
    bool modifiers_uniform() const noexcept;

    /// 多行摘要，启动时打一次
    std::string to_string() const;

  private:
    /// 两个工厂的共同实现。display 为空表示不建 GL 渲染目标。
    ///
    /// count_ 在全部分配成功之后才置：中途失败的半成品 count_ 保持 0，
    /// 这样 to_string() 与析构看到的都是一个诚实的状态。
    static Result<Swapchain> allocate(BufferSource& source, const SwapchainDesc& desc,
                                      const egl::Display* display);

    std::array<Slot, kMaxBuffers> slots_{};
    uint32_t count_ = 0;
    uint32_t next_ = 0;
    uint32_t in_flight_ = 0;
};

} // namespace mw::render
