#include "mw/render/target.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <type_traits>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"

namespace mw::render {

// 头文件用 unsigned int 表示 GL 名，是为了不把 <GLES2/gl2.h> 拖进接口。
// 这条静态断言保证那个替身没有偷偷失效。
static_assert(std::is_same<GLuint, unsigned int>::value,
              "GLuint is not unsigned int; render/target.hpp needs a different stand-in type");

namespace {

const char* gl_error_name(GLenum error) noexcept {
    switch (error) {
    case GL_NO_ERROR: return "GL_NO_ERROR";
    case GL_INVALID_ENUM: return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE: return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "GL_OUT_OF_MEMORY";
    default: return "GL_<unknown>";
    }
}

const char* fbo_status_name(GLenum status) noexcept {
    switch (status) {
    case GL_FRAMEBUFFER_COMPLETE: return "GL_FRAMEBUFFER_COMPLETE";
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
        return "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
    case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: return "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
    case GL_FRAMEBUFFER_UNSUPPORTED: return "GL_FRAMEBUFFER_UNSUPPORTED";
    default: return "GL_FRAMEBUFFER_<unknown>";
    }
}

/// 清空之前积下来的 GL 错误。GL 的错误是粘着的，不清就会把别人的账记到自己头上。
void drain_gl_errors() noexcept {
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {
        // 只是把队列排干
    }
}

/// 返回最近一次 GL 错误的名字，没有错误返回 nullptr
const char* take_gl_error() noexcept {
    const GLenum error = glGetError();
    return error == GL_NO_ERROR ? nullptr : gl_error_name(error);
}

/**
 * @brief 建立一条绑定路径，返回 (fbo, attachment)
 *
 * 失败时把已经建出来的 GL 对象全部删掉再返回，不留半成品 ——
 * 这个函数会被 create() 调用两次（试完 renderbuffer 再试 texture），
 * 泄漏在这里会被放大。
 */
Status attach(AttachKind kind, void* egl_image, Size size, GLuint& out_fbo,
              GLuint& out_attachment) {
    out_fbo = 0;
    out_attachment = 0;
    drain_gl_errors();

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    if (fbo == 0) {
        return Err(drm::Errc::Unsupported,
                   fmt("glGenFramebuffers produced nothing: {}",
                       take_gl_error() != nullptr ? "GL error" : "no error reported"));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    auto cleanup = [&fbo](GLuint attachment, bool is_texture) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (attachment != 0) {
            if (is_texture) {
                glDeleteTextures(1, &attachment);
            } else {
                glDeleteRenderbuffers(1, &attachment);
            }
        }
        glDeleteFramebuffers(1, &fbo);
    };

    GLuint attachment = 0;
    const auto image = static_cast<GLeglImageOES>(egl_image);

    if (kind == AttachKind::Renderbuffer) {
        auto target_renderbuffer =
            reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
                eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
        if (target_renderbuffer == nullptr) {
            cleanup(0, false);
            return Err(drm::Errc::Unsupported,
                       "glEGLImageTargetRenderbufferStorageOES is not available");
        }
        glGenRenderbuffers(1, &attachment);
        glBindRenderbuffer(GL_RENDERBUFFER, attachment);
        target_renderbuffer(GL_RENDERBUFFER, image);
        if (const char* error = take_gl_error(); error != nullptr) {
            cleanup(attachment, false);
            return Err(drm::Errc::Unsupported,
                       fmt("glEGLImageTargetRenderbufferStorageOES failed with {}", error));
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER,
                                  attachment);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    } else {
        auto target_texture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (target_texture == nullptr) {
            cleanup(0, false);
            return Err(drm::Errc::Unsupported, "glEGLImageTargetTexture2DOES is not available");
        }
        glGenTextures(1, &attachment);
        glBindTexture(GL_TEXTURE_2D, attachment);
        // GLES2 里非 mipmap 完整的纹理必须用 NEAREST/LINEAR + CLAMP，
        // 否则采样时是 incomplete。当渲染目标时用不到采样，但设了不亏，
        // 而且 Step 4 复用同一段代码去采样 client buffer 时就需要。
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        target_texture(GL_TEXTURE_2D, image);
        if (const char* error = take_gl_error(); error != nullptr) {
            cleanup(attachment, true);
            return Err(drm::Errc::Unsupported,
                       fmt("glEGLImageTargetTexture2DOES failed with {}", error));
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, attachment,
                               0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        cleanup(attachment, kind == AttachKind::Texture);
        return Err(drm::Errc::Unsupported,
                   fmt("the framebuffer is {} after attaching via {}", fbo_status_name(status),
                       to_string(kind)));
    }

    LOG_DEBUG("render target ready: {} via {}", drm::to_string(size), to_string(kind));
    out_fbo = fbo;
    out_attachment = attachment;
    return Ok();
}

} // namespace

// ---------------------------------------------------------------------------

const char* to_string(AttachKind kind) noexcept {
    switch (kind) {
    case AttachKind::Renderbuffer: return "renderbuffer";
    case AttachKind::Texture: return "texture";
    }
    return "<unknown>";
}

// ---------------------------------------------------------------------------

GlRenderTarget::GlRenderTarget(egl::Image image, unsigned int fbo, unsigned int attachment,
                               AttachKind kind, Size size) noexcept
    : image_(std::move(image)), fbo_(fbo), attachment_(attachment), kind_(kind), size_(size) {}

GlRenderTarget::~GlRenderTarget() {
    destroy();
}

GlRenderTarget::GlRenderTarget(GlRenderTarget&& other) noexcept
    : image_(std::move(other.image_)),
      fbo_(std::exchange(other.fbo_, 0u)),
      attachment_(std::exchange(other.attachment_, 0u)),
      kind_(other.kind_),
      size_(other.size_) {}

GlRenderTarget& GlRenderTarget::operator=(GlRenderTarget&& other) noexcept {
    if (this != &other) {
        destroy();
        image_ = std::move(other.image_);
        fbo_ = std::exchange(other.fbo_, 0u);
        attachment_ = std::exchange(other.attachment_, 0u);
        kind_ = other.kind_;
        size_ = other.size_;
    }
    return *this;
}

void GlRenderTarget::destroy() noexcept {
    if (attachment_ != 0u) {
        if (kind_ == AttachKind::Texture) {
            glDeleteTextures(1, &attachment_);
        } else {
            glDeleteRenderbuffers(1, &attachment_);
        }
        attachment_ = 0u;
    }
    if (fbo_ != 0u) {
        glDeleteFramebuffers(1, &fbo_);
        fbo_ = 0u;
    }
    // image_ 自己析构，顺序上排在附件之后 —— 反过来会让驱动看到一个
    // 引用着已销毁 EGLImage 的 renderbuffer。
}

Result<GlRenderTarget> GlRenderTarget::create_with(const egl::Display& display,
                                                   const ScanoutBuffer& buffer,
                                                   AttachKind kind) {
    if (! buffer.valid()) {
        return Err(drm::Errc::Internal, "GlRenderTarget::create_with() on an empty buffer");
    }

    auto image_result = display.import_dmabuf(buffer.dmabuf());
    if (! image_result) {
        return Err(drm::Errc::Unsupported,
                   fmt("cannot import the buffer as an EGLImage: {}",
                       image_result.error().message));
    }
    egl::Image image = std::move(image_result).value();

    GLuint fbo = 0;
    GLuint attachment = 0;
    TRY(attach(kind, image.raw(), buffer.size(), fbo, attachment));

    return Ok(GlRenderTarget(std::move(image), fbo, attachment, kind, buffer.size()));
}

Result<GlRenderTarget> GlRenderTarget::create(const egl::Display& display,
                                              const ScanoutBuffer& buffer) {
    if (! display.caps().gl_egl_image) {
        return Err(drm::Errc::Unsupported,
                   "GL_OES_EGL_image is absent: this GL implementation cannot use an imported "
                   "dmabuf as a render target. Draw with the CPU instead, or check whether the "
                   "GL stack fell back to a software rasteriser");
    }

    if (display.caps().gl_renderbuffer_from_image) {
        auto first = create_with(display, buffer, AttachKind::Renderbuffer);
        if (first) {
            return first;
        }
        // 降级要吵。静默走 texture 路径会让\"modifier 被隐式改写\"这类问题
        // 完全没有线索。
        LOG_WARN("the renderbuffer path is unusable ({}); falling back to the texture path",
                 first.error().message);
    } else {
        LOG_WARN("glEGLImageTargetRenderbufferStorageOES did not resolve; using the texture "
                 "path directly");
    }

    return create_with(display, buffer, AttachKind::Texture);
}

Status GlRenderTarget::bind() const {
    if (! valid()) {
        return Err(drm::Errc::Internal, "bind() on an empty render target");
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, static_cast<GLsizei>(size_.width), static_cast<GLsizei>(size_.height));
    if (const char* error = take_gl_error(); error != nullptr) {
        return Err(drm::Errc::Internal, fmt("binding the render target failed with {}", error));
    }
    return Ok();
}

void GlRenderTarget::unbind() noexcept {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

std::string GlRenderTarget::to_string() const {
    if (! valid()) {
        return "<empty render target>";
    }
    return fmt("{} fbo={} via {}", drm::to_string(size_), fbo_, render::to_string(kind_));
}

// ---------------------------------------------------------------------------

Status finish_rendering(const egl::Display& display) {
    (void)display;  // 参数留着是为了 Step 6 换成 fence 时签名不变
    drain_gl_errors();
    glFinish();
    if (const char* error = take_gl_error(); error != nullptr) {
        return Err(drm::Errc::Internal, fmt("glFinish reported {}", error));
    }
    return Ok();
}

} // namespace mw::render
