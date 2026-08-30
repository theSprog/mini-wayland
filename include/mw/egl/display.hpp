/**
 * @file egl/display.hpp
 * @brief EGL 显示、上下文与 dmabuf ↔ EGLImage 互转
 *
 * ## 这一层刻意不做的事
 *
 * **不创建 EGLSurface，不用 gbm_surface。**
 * 用 `EGL_KHR_surfaceless_context` 建立无表面上下文，渲染目标由上层
 * 自己用 FBO 绑定（见 render/target.hpp）。
 *
 * 代价是多写一点代码；收益是 buffer 的数量、复用时机、每个 buffer 的
 * fence 全部握在自己手里 —— 而这三样正是后续显式同步和帧节拍要控制的。
 * 用 `eglSwapBuffers` 的话它们都在驱动内部。
 *
 * 另一个收益更实际：**导入自己分配的 buffer 和导入别的进程送来的 buffer
 * 走的是同一套代码**（都是 dmabuf → EGLImage）。后面接客户端 buffer 时
 * 不需要第二条路径。
 *
 * ## 扩展依赖
 *
 * 全部运行时探测，缺哪个就明确报出来，不用 #ifdef：
 *
 *   EGL_EXT_image_dma_buf_import              必需，dmabuf 导入
 *   EGL_EXT_image_dma_buf_import_modifiers    带 modifier 的导入
 *   EGL_KHR_surfaceless_context               必需，无表面上下文
 *   EGL_KHR_no_config_context                 可选，简化 config 选择
 *   EGL_KHR_fence_sync / EGL_ANDROID_native_fence_sync
 *                                             Step 6 显式同步用，此处仅探测
 */
#pragma once

#include <string>
#include <vector>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/prime.hpp"
#include "mw/gbm/device.hpp"

namespace mw::egl {

/// 运行时探测到的 EGL 能力
struct Caps {
    std::string vendor{};
    std::string version{};
    std::string client_apis{};

    // ---- 本步必需 ----
    bool image_base = false;             ///< EGL_KHR_image_base
    bool dmabuf_import = false;          ///< EGL_EXT_image_dma_buf_import
    bool surfaceless_context = false;    ///< EGL_KHR_surfaceless_context

    // ---- 可选，缺了会降级 ----
    bool dmabuf_import_modifiers = false;  ///< 不带则只能导入线性 buffer
    bool no_config_context = false;
    bool dmabuf_export = false;  ///< EGL_MESA_image_dma_buf_export

    // ---- Step 6 会用到，这里只记录 ----
    bool fence_sync = false;         ///< EGL_KHR_fence_sync
    bool native_fence_sync = false;  ///< EGL_ANDROID_native_fence_sync
    bool wait_sync = false;          ///< EGL_KHR_wait_sync

    // ---- GL 侧。必须在上下文 current 之后才能查，所以与上面分开。----
    //
    // EGL 能导入 dmabuf 成 EGLImage，**不等于** GL 能把它当渲染目标。
    // 前者是 EGL_EXT_image_dma_buf_import 的事，后者要 GL_OES_EGL_image。
    // 这两个扩展来自不同的规范，实现上也确实可能只有其一。

    std::string gl_vendor{};
    std::string gl_renderer{};
    std::string gl_version{};
    std::string glsl_version{};

    /// GL_OES_EGL_image：提供 glEGLImageTargetTexture2DOES 与
    /// glEGLImageTargetRenderbufferStorageOES。两条绑定路径都依赖它。
    bool gl_egl_image = false;

    /// GL_OES_EGL_image_external：采样外部 YUV 图像用。Step 5 才需要。
    bool gl_egl_image_external = false;

    /// glEGLImageTargetRenderbufferStorageOES 的入口拿得到吗
    ///
    /// 拿得到也不保证 FBO 会完整 —— 真正的判据是建一次然后
    /// glCheckFramebufferStatus。见 render/target.hpp。
    bool gl_renderbuffer_from_image = false;

    /// glEGLImageTargetTexture2DOES 的入口拿得到吗
    bool gl_texture_from_image = false;

    /// 本步能否工作。缺项由 missing() 列出。
    bool sufficient_for_rendering() const noexcept {
        return image_base && dmabuf_import && surfaceless_context;
    }

    /// GL 能不能把导入的 dmabuf 当渲染目标（两条绑定路径至少有一条）
    bool can_render_into_imported_image() const noexcept {
        return gl_egl_image && (gl_renderbuffer_from_image || gl_texture_from_image);
    }

    /// 缺失的必需扩展名，用于给出可行动的错误信息
    std::vector<std::string> missing() const;

    std::string to_string() const;
};

// ---------------------------------------------------------------------------

class Image;

/**
 * @brief 一个 EGL 显示 + 上下文，绑定到某个 GBM 设备
 *
 * move-only。析构时按 EGL 要求的顺序拆除。
 *
 * @note 上下文是**当前线程绑定**的。本类不做线程亲和检查 ——
 *       Step 2 是单线程。多线程渲染要引入时，这里需要加显式的
 *       make_current/release 语义，别默默让它在两个线程间漂。
 */
class Display {
  public:
    Display() noexcept = default;
    ~Display();

    Display(Display&&) noexcept;
    Display& operator=(Display&&) noexcept;
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    /**
     * @brief 在给定 GBM 设备上建立 EGL 显示与 GLES 上下文
     *
     * 走 `EGL_PLATFORM_GBM_KHR`。不创建任何 EGLSurface。
     *
     * @param device 必须比本对象活得久
     */
    static Result<Display> create(const gbm::Device& device);

    const Caps& caps() const noexcept {
        return caps_;
    }

    /// 把上下文绑到当前线程（无表面）
    Status make_current() const;

    /**
     * @brief 把一块 dmabuf 导入成 EGLImage
     *
     * 这条路径对"自己分配的 buffer"和"别的进程送来的 buffer"完全一样，
     * 是刻意的 —— 后面接客户端 buffer 时直接复用。
     *
     * modifier 非 kModifierInvalid 时需要
     * `EGL_EXT_image_dma_buf_import_modifiers`；没有该扩展时会忽略
     * modifier 并 WARN，因为忽略 modifier 导入非线性 buffer 会得到花屏，
     * 而不是干净的失败。
     */
    Result<Image> import_dmabuf(const drm::DmabufDesc& desc) const;

    /// GL_RENDERER / GL_VERSION，需要上下文已 current。诊断用。
    std::string gl_renderer() const;

    /**
     * @brief 查询 GL 侧能力，填进 caps()
     *
     * `create()` 里在 make_current 之后自动调一次。上下文被别的代码
     * 重新绑定过之后可以再调，代价是几次 glGetString。
     */
    void query_gl_caps();

    std::string to_string() const;

  private:
    friend class Image;

    void* display_ = nullptr;  ///< EGLDisplay
    void* context_ = nullptr;  ///< EGLContext
    void* config_ = nullptr;   ///< EGLConfig，no_config_context 时为空
    Caps caps_{};
};

/**
 * @brief 一个从 dmabuf 导入的 EGLImage
 *
 * move-only。析构时销毁。
 *
 * @note EGLImage 只是对底层 buffer 的一个引用。**它不延长 dmabuf fd 的
 *       生命周期**（EGL 在创建时会自己 dup 需要的东西，但这一点各实现
 *       行为不一）。安全的做法是让 DmabufDesc 至少活到 Image 创建返回。
 */
class Image {
  public:
    Image() noexcept = default;
    ~Image();

    Image(Image&&) noexcept;
    Image& operator=(Image&&) noexcept;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    bool valid() const noexcept {
        return image_ != nullptr;
    }

    /// 底层 EGLImageKHR。给 render 层绑定 renderbuffer / texture 用。
    void* raw() const noexcept {
        return image_;
    }

  private:
    friend class Display;
    Image(void* display, void* image) noexcept : display_(display), image_(image) {}

    void* display_ = nullptr;
    void* image_ = nullptr;
};

} // namespace mw::egl
