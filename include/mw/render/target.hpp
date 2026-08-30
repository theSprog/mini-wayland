/**
 * @file render/target.hpp
 * @brief 把一块 ScanoutBuffer 变成 GL 能画进去的渲染目标
 *
 * ## 这一步在整条链路里的位置
 *
 *   分配（GBM / dumb）
 *     -> 导出 dmabuf                       drm/prime.hpp
 *     -> EGLImage                          egl/display.hpp
 *     -> **renderbuffer / texture + FBO**  本文件
 *     -> GLES 绘制
 *     -> 导入成 fb_id + atomic commit      drm/framebuffer.hpp
 *
 * 换句话说：EGL 负责"让 GL 认识这块内存"，本文件负责"让 GL 往里写"。
 * 这是两件事，实现上也确实可能只有前者 —— 见下。
 *
 * ## 两条绑定路径，必须都实现
 *
 * `GL_OES_EGL_image` 给出两个入口：
 *
 *   glEGLImageTargetRenderbufferStorageOES   EGLImage -> renderbuffer
 *   glEGLImageTargetTexture2DOES             EGLImage -> texture 2D
 *
 * 二者都能当 FBO 的 color attachment。**不能假设 renderbuffer 那条一定在**：
 * 扩展字符串写着支持、入口点也拿得到，FBO 仍可能是 INCOMPLETE。
 * 软件光栅化后端、以及只实现了纹理路径的嵌入式驱动都会这样。
 *
 * 所以 `create()` 的策略是：**先试 renderbuffer，不完整就退到 texture**，
 * 并把实际走了哪条记在 `attach_kind()` 里。降级会 WARN 一次，不静默。
 *
 * 为什么优先 renderbuffer：它语义上就是"只写的渲染目标"，驱动不需要为它
 * 准备采样路径（mipmap、swizzle、纹理布局转换）。texture 路径在某些实现上
 * 会触发一次隐式的布局转换，而那正好会毁掉我们费劲协商来的 modifier。
 *
 * ## 本文件不做的事
 *
 * **不管同步。** 画完到上屏之间的等待由 `finish_rendering()` 承担，
 * 那是一个临时方案，见它自己的说明。
 *
 * **不持有 ScanoutBuffer。** 目标只借用它的 dmabuf 描述来建立 EGLImage。
 * 谁分配的、fb_id 是多少，本文件一概不关心 —— 那是 swapchain 的事。
 * 但由此产生一条调用方必须遵守的规则：**ScanoutBuffer 必须活得比
 * GlRenderTarget 久**。
 */
#pragma once

#include <string>

#include "mw/core/error.hpp"
#include "mw/egl/display.hpp"
#include "mw/render/buffer_source.hpp"

namespace mw::render {

/// EGLImage 挂到 FBO 的方式
enum class AttachKind {
    /// glEGLImageTargetRenderbufferStorageOES。首选。
    Renderbuffer,
    /// glEGLImageTargetTexture2DOES。renderbuffer 不可用时的降级路径。
    Texture,
};

const char* to_string(AttachKind kind) noexcept;

/**
 * @brief 一块 buffer 对应的 FBO
 *
 * move-only。析构时按 GL 要求的顺序拆除（先解绑 FBO，再删附件）。
 *
 * @note GL 对象属于创建它的**上下文**。本类不记录上下文，也不做检查 ——
 *       Step 2 是单线程单上下文。多上下文引入时这里要加显式绑定，
 *       别指望"反正都是 current 的那个"。
 */
class GlRenderTarget {
  public:
    GlRenderTarget() noexcept = default;
    ~GlRenderTarget();

    GlRenderTarget(GlRenderTarget&&) noexcept;
    GlRenderTarget& operator=(GlRenderTarget&&) noexcept;
    GlRenderTarget(const GlRenderTarget&) = delete;
    GlRenderTarget& operator=(const GlRenderTarget&) = delete;

    /**
     * @brief 建立渲染目标，绑定方式自动选择
     *
     * 先试 renderbuffer；FBO 不完整或入口点缺失时退到 texture，
     * 两条都不成才返回错误。降级会 WARN。
     *
     * @param display 必须已经 make_current，且必须比本对象活得久
     * @param buffer  必须比本对象活得久
     */
    static Result<GlRenderTarget> create(const egl::Display& display,
                                         const ScanoutBuffer& buffer);

    /**
     * @brief 强制某一条绑定路径，不降级
     *
     * 用来验证"另一条路也能走"，以及在怀疑 modifier 被纹理路径改写时做对照。
     * 常规使用请用 `create()`。
     */
    static Result<GlRenderTarget> create_with(const egl::Display& display,
                                              const ScanoutBuffer& buffer, AttachKind kind);

    bool valid() const noexcept {
        return fbo_ != 0;
    }

    /// 实际用上的绑定方式。降级发生过的话这里看得出来。
    AttachKind attach_kind() const noexcept {
        return kind_;
    }

    Size size() const noexcept {
        return size_;
    }

    /// 底层 FBO 名。只给同层的绘制代码用，不要外泄。
    unsigned int fbo() const noexcept {
        return fbo_;
    }

    /**
     * @brief 绑定为当前渲染目标并设好 viewport
     *
     * viewport 一起设是有意的：忘记设 viewport 会画出一个尺寸对不上的画面，
     * 而这种错误看起来像"缩放没配好"，很容易怪到 KMS 头上。
     */
    Status bind() const;

    /// 解绑回默认 framebuffer（无表面上下文下它是 0，不可绘制）
    static void unbind() noexcept;

    std::string to_string() const;

  private:
    GlRenderTarget(egl::Image image, unsigned int fbo, unsigned int attachment, AttachKind kind,
                   Size size) noexcept;

    void destroy() noexcept;

    egl::Image image_{};
    unsigned int fbo_ = 0;
    unsigned int attachment_ = 0;  ///< renderbuffer 名或 texture 名，看 kind_
    AttachKind kind_ = AttachKind::Renderbuffer;
    Size size_{};
};

/**
 * @brief 等 GL 把像素真的写完
 *
 * ## 这是一个临时方案，Step 6 会删掉它
 *
 * 现在的做法是 `glFinish()` —— **CPU 阻塞等 GPU**。这正是现代显示管线要
 * 消灭的东西：合成器本该提交完就去干别的，由硬件自己等 GPU。
 *
 * 之所以现在必须这么做：atomic commit 之后 CRTC 随时可能开始扫描这块内存，
 * 而"GPU 写完了没有"这件事，用户态目前拿不到任何凭据。
 *
 * 有人会指望隐式同步（dmabuf 的 reservation object 上挂着 GPU 的 fence，
 * KMS 提交时自动等它）。本项目不依赖它，两个理由：
 *
 *   1. **不保证存在**。隐式 fence 由驱动决定挂不挂，跨设备路径尤其没谱。
 *   2. **不可观测**。它成立时和 glFinish 的效果一样，不成立时是偶发花屏。
 *      分不出这两种情况的机制，不能作为正确性依据。
 *
 * 蒙对了比错了更糟：错了会被立刻发现，蒙对了会在换一块板子时突然坏掉。
 *
 * TODO(step6): 换成 EGL_ANDROID_native_fence_sync 导出 fence fd，
 *              作为 plane 的 IN_FENCE_FD 提交给内核。届时删掉本函数的
 *              全部调用，帧率的变化就是显式同步带来的收益。
 */
Status finish_rendering(const egl::Display& display);

} // namespace mw::render
