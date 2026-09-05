/**
 * @file display/screen.hpp
 * @brief 门面：一块能画进去、能上屏、能按帧节拍推进的屏幕
 *
 * ## 这一层为什么存在
 *
 * 到 Step 3 为止，把一帧像素送上屏需要的调用序列大致是：
 * 打开设备 → 抢 master → 枚举拓扑挑通路 → 建 property blob →
 * 建分配器 → 建 swapchain → 组 atomic 请求做 TEST_ONLY → 提交 modeset →
 * 每帧 acquire / 画 / 组请求 / 提交 / poll / read 事件 / 记账 →
 * 退出时逐个 plane 关掉再关 CRTC 再解绑 connector。
 *
 * `demos/step1_*` 与 `demos/step2_*` 各自写了一遍，四百行上下，
 * 两份几乎一样。**这是本工程内部的合理重复**（它们要展示这些步骤，
 * 那正是它们存在的意义），但对一个只想要"给我一块 buffer，画完帮我送上屏"
 * 的下游项目来说，这四百行是纯粹的负担，而且是一份会随本工程演进而
 * 悄悄过期的复制品。
 *
 * 所以这一层的读者是**库外的消费者**，不是本工程自己的 step demo。
 * step demo 继续直接用 `mw/drm` 与 `mw/render`，因为它们的职责就是
 * 把中间状态摊开给人看。
 *
 * ## 门面必须遵守的三条，否则它会毁掉后面几步
 *
 * 本工程刻意不用 `gbm_surface` + `eglSwapBuffers`，理由写在
 * `render/swapchain.hpp` 里：那套接口把队列深度、复用时机、每块 buffer
 * 的 fence 藏进了 Mesa。一个设计随便的门面会犯同样的错误，只不过藏的是
 * 我们自己的代码。所以：
 *
 *  1. **`submit()` 不阻塞。** 提交与"等出光"是两个函数
 *     （`submit()` / `wait_vblank()`）。`present()` 只是把两者接起来的
 *     便利函数，不是唯一入口。Step 6 的显式同步整个是在
 *     "已提交但尚未出光"这个中间状态上做文章，把它藏起来就等于把
 *     那一步的落点铲掉了。
 *  2. **不隐藏中间状态。** 帧序号、in-flight 数、vblank 时间戳、丢帧计数
 *     全部可读（`stats()`）。
 *  3. **留逃生舱。** `device()` / `output()` / `swapchain()` 直接返回下层
 *     对象。消费者需要自己组一次 atomic 请求（比如加一个 overlay plane）时，
 *     不必放弃这一层重新写一遍初始化。
 *
 * 判据很简单：Step 5 / 6 / 7 落地时，如果发现必须绕开 Screen 才能做事，
 * 那说明这个门面设计错了，应该改它而不是绕它。
 *
 * ## 两个后端
 *
 * `Backend::Kms` —— 真的上屏。需要 DRM master（root + 停显示管理器或切 tty）。
 *
 * `Backend::Offscreen` —— **完全不碰 DRM**，buffer 就是一块堆内存，
 * 可选按标称帧长 sleep 来模拟帧节拍，可选把每帧写成 PPM 文件。
 *
 * 加这个后端不是为了"多一种模式"，而是因为没有它，下游项目的开发循环是
 * 「改一行 → 切 tty → sudo → 看一眼 → 切回来」。软件光栅化这类需要
 * 反复微调的工作在那种循环下没法做。
 *
 * **但它有一条必须说清楚的局限：Offscreen 后端不验证显示链路的任何东西。**
 * 没有 stride 对齐约束、没有 modifier、没有跨设备导入、没有真实 vblank。
 * 它是算法开发工具，不是显示栈的测试替身。"在 Offscreen 下是对的"
 * 不构成"在 KMS 下也是对的"的任何证据 —— 两个后端唯一共享的契约是
 * `Frame` 里那几个字段的含义。
 *
 * ## 不做的事
 *
 * **不碰 GL / EGL / GBM 的绘制路径。** 本层只提供 CPU 可写的 buffer。
 * 想用 GPU 画就直接用 `mw/render`：那里的 `Swapchain::create_with_targets()`
 * 已经是够用的接口，再包一层只会把 `egl::Display` 与 `gbm::Device` 的生命周期
 * 打成一个更难解的结。这条界线以后可能会变，但要等到真有消费者需要它。
 *
 * **不管输入。** 键盘鼠标不在本工程当前的范围内，见 `docs/api.md`。
 *
 * @see docs/api.md          导出边界与稳定性分级
 * @see demos/hello_screen/  本文件的最小完整用法
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mw/internal/error.hpp"
#include "mw/drm/event.hpp"
#include "mw/drm/types.hpp"
#include "mw/render/buffer_source.hpp"

namespace mw {
namespace drm {
class Device;
struct OutputPath;
} // namespace drm
namespace render {
class Swapchain;
} // namespace render
} // namespace mw

namespace mw::display {

using drm::Format;
using drm::Size;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

enum class Backend {
    /// 真的点屏。需要 DRM master。
    Kms,
    /// 堆内存 + 可选 PPM 落盘。不需要任何权限，也不需要 /dev/dri。
    Offscreen,
};

const char* to_string(Backend backend) noexcept;

struct ScreenConfig {
    Backend backend = Backend::Kms;

    // ---- Kms 后端 ----

    /// 空 = 自动挑第一个带 KMS 且有已连接 connector 的节点
    std::string device_path{};

    /// 空 = 用 connector 的 preferred mode
    std::optional<Size> mode_size{};

    /**
     * @brief 谁来分配 buffer
     *
     * 默认 `ScanoutDevice`（显示节点 dumb 分配）。对 CPU 绘制来说这几乎
     * 总是想要的：线性排布、必然可映射、对齐由显示驱动自己保证。
     *
     * 换成 `RenderDevice` 会走 GBM 分配 + PRIME 导入，能协商 modifier ——
     * 但拿到的 buffer 未必 CPU 可写，`begin_frame()` 会因此失败。
     * 两条路径的差异见 `render/buffer_source.hpp`。
     */
    render::SourceKind source = render::SourceKind::ScanoutDevice;

    /// `source == RenderDevice` 时用哪个 render node。空 = 自动推断。
    std::string render_node{};

    // ---- 两个后端都认 ----

    /// 只有 `bytes_per_pixel()` 认识的单平面 packed 格式能用于 CPU 绘制
    Format format = drm::kFormatXrgb8888;

    /// 2 = 双缓冲，3 = 三缓冲。单缓冲必然撕裂，不接受。
    uint32_t buffer_count = 2;

    // ---- Offscreen 后端 ----

    Size offscreen_size{1280, 720};

    /// 标称刷新率，毫赫兹。60000 = 60Hz。用于 `frame_duration_ns()` 与节拍模拟。
    uint32_t offscreen_refresh_mhz = 60000;

    /**
     * @brief 是否按标称帧长 sleep，模拟 vblank 节拍
     *
     * true 时 `wait_vblank()` 睡到下一个整帧边界，帧率与真机接近。
     * false 时立即返回 —— 跑离线渲染、批量出图、跑测试时要的是这个。
     */
    bool offscreen_pace = true;

    /**
     * @brief 非空则把帧写成 PPM 到这个目录
     *
     * 选 PPM 不是因为它好，是因为它不需要任何依赖，二十行就能写完，
     * 而多引入一个图像库会让"这个库到底依赖什么"这个问题变复杂。
     * 需要 PNG 的话在消费者那边转，`convert`/`ffmpeg` 都行。
     */
    std::string dump_dir{};

    /// 每 N 帧落盘一次。0 = 不落盘（即使 dump_dir 非空）。1 = 每帧。
    uint32_t dump_every = 0;
};

// ---------------------------------------------------------------------------
// 一帧
// ---------------------------------------------------------------------------

/**
 * @brief 一次 `begin_frame()` 的产物
 *
 * **是一个借用视图，不持有任何东西。** 有效期截止到同一个 `Screen` 上
 * 的下一次 `begin_frame()` 或 `Screen` 析构，以先到者为准。
 * 不要存下来跨帧使用。
 */
struct Frame {
    /// 从 0 开始，每次 `begin_frame()` 递增
    uint64_t index = 0;

    /**
     * @brief 可写的像素内存
     *
     * @warning 这块内存**很可能是写合并（write-combining）甚至非缓存的**。
     *          顺序写没问题，读回会慢到离谱（量级是几十倍）。
     *          需要 read-modify-write（混合、抗锯齿、读 z-buffer）请在
     *          普通堆内存里画完再整块拷进来 —— 这也是绝大多数软件光栅化
     *          实现本来就在做的事。
     *          Offscreen 后端下它是普通堆内存，**所以这条约束在那里
     *          不会暴露**，见本文件开头对两个后端的说明。
     */
    span<uint8_t> pixels{};

    Size size{};

    /// 行跨距，字节。**不等于** `size.width * bytes_per_pixel(format)`。
    /// 定位像素只能用它。
    uint32_t stride = 0;

    Format format{};

    /// 第 `row` 行的起始位置。越界返回空 span。
    span<uint8_t> row(uint32_t y) const noexcept;
};

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

/**
 * @brief 一块屏幕的完整生命周期
 *
 * move-only。析构时按正确顺序拆除：KMS 后端会逐个关掉该 CRTC 上的 plane、
 * 关 CRTC、解绑 connector，再释放 buffer —— 顺序反了会留下一块指着已释放
 * 内存的 CRTC。
 *
 * @code
 *   ScreenConfig cfg;
 *   cfg.backend = Backend::Offscreen;          // 开发时；上板改成 Kms
 *   auto screen = TRY(Screen::open(cfg));
 *
 *   while (! done) {
 *       Frame frame = TRY(screen.begin_frame());
 *       my_rasterizer.draw(frame.pixels, frame.size, frame.stride);
 *       TRY(screen.present());                 // = submit() + wait_vblank()
 *   }
 * @endcode
 */
class Screen {
  public:
    Screen() noexcept;
    ~Screen();

    Screen(Screen&&) noexcept;
    Screen& operator=(Screen&&) noexcept;
    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    /**
     * @brief 打开、配置、完成首次 modeset
     *
     * 返回时屏幕已经亮着（显示第一块尚未绘制的 buffer，内容是清过的黑）。
     * 失败时错误消息里带着走到了哪一步。
     *
     * KMS 后端的常见失败：拿不到 DRM master（另一个进程占着，通常是
     * 显示管理器）、没有已连接的 connector、TEST_ONLY 被拒。
     * 前者的诊断可以看 `drm::Device::master_diagnosis()`。
     */
    static Result<Screen> open(const ScreenConfig& config);

    bool valid() const noexcept;

    Backend backend() const noexcept;
    Size size() const noexcept;
    Format format() const noexcept;

    /// 刷新率，毫赫兹。59940 = 59.94Hz。**不是**取整的 60。
    uint32_t refresh_mhz() const noexcept;

    /// 一帧的标称时长，纳秒。判断丢帧、做 frame pacing 的基准。
    uint64_t frame_duration_ns() const noexcept;

    // ---- 帧循环 ----

    /**
     * @brief 取一块当前没在被扫描的 buffer，映射出来
     *
     * 不阻塞。缓冲区都在飞的时候仍然会返回一块（可能正被扫描的那块的
     * 下一块），是否该先等一等由调用方看 `in_flight()` 决定 ——
     * 一个偷偷阻塞的 acquire 会让 Step 6 无处落脚，
     * 理由同 `render/swapchain.hpp`。
     *
     * 每帧调用不产生 ioctl，也不产生堆分配：映射在 `open()` 时就建好了。
     */
    Result<Frame> begin_frame();

    /**
     * @brief 提交上一次 `begin_frame()` 拿到的那一帧，**不等它出光**
     *
     * KMS 后端：一次带 `PAGE_FLIP_EVENT` 的非阻塞 atomic commit。
     * Offscreen 后端：按需落盘，然后立即返回。
     */
    Status submit();

    /**
     * @brief 等到硬件真的把上一帧扫出去
     *
     * KMS 后端：poll DRM fd，读并解析完成事件，喂给 `stats()`。
     * Offscreen 后端：`offscreen_pace` 为真时睡到下一个整帧边界。
     *
     * @param timeout_ms 负数 = 无限等。默认按标称帧长的若干倍推算，
     *                   够宽松到不会误报，又不至于卡死。
     * @return false 表示超时（KMS 下通常意味着提交被硬件吞了，值得查），
     *         此时**不算错误**，由调用方决定重试还是退出。
     */
    Result<bool> wait_vblank(int timeout_ms = -1);

    /// `submit()` 之后 `wait_vblank()`。最常见的用法，没有别的含义。
    Status present();

    /// 已提交但还没收到完成事件的帧数。>= `buffer_count()` 说明该等了。
    uint32_t in_flight() const noexcept;

    uint32_t buffer_count() const noexcept;

    /// 帧节拍统计。KMS 后端下 `dropped` 来自 vblank 序号跳变，是真的丢帧。
    const drm::FrameStats& stats() const noexcept;

    // ---- 逃生舱 ----
    //
    // 门面挡不住的事情就从这里出去，不要因为门面不够用就整个绕开它。
    // Offscreen 后端下这三个都返回 nullptr —— 那里根本没有 DRM 对象。

    drm::Device* device() noexcept;
    const drm::OutputPath* output() const noexcept;
    render::Swapchain* swapchain() noexcept;

    /// 多行摘要：后端、设备、mode、分配路径、buffer 数与各自的 modifier。
    /// 启动时打一次，出问题时它通常已经说明了一半。
    std::string to_string() const;

  private:
    explicit Screen(std::unique_ptr<struct ScreenImpl> impl) noexcept;
    std::unique_ptr<struct ScreenImpl> impl_;
};

} // namespace mw::display
