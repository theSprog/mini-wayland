#include "mw/egl/display.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <gbm.h>

#include <cstring>
#include <utility>

#include "mw/core/log.hpp"
#include "mw/drm/error.hpp"

namespace mw::egl {

namespace {

bool has_extension(const char* list, const char* name) {
    if (list == nullptr || name == nullptr) {
        return false;
    }
    // 不能用 strstr：EGL_KHR_image 是 EGL_KHR_image_base 的前缀，
    // 子串匹配会把后者误判成前者。必须按空格分隔的整词比较。
    const size_t len = std::strlen(name);
    const char* pos = list;
    while ((pos = std::strstr(pos, name)) != nullptr) {
        const bool left_ok = (pos == list) || (pos[-1] == ' ');
        const char after = pos[len];
        const bool right_ok = (after == ' ' || after == '\0');
        if (left_ok && right_ok) {
            return true;
        }
        pos += len;
    }
    return false;
}

const char* egl_error_name(EGLint error) {
    switch (error) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "EGL_<unknown>";
    }
}

std::string last_egl_error() {
    const EGLint error = eglGetError();
    return fmt("{} (0x{:x})", egl_error_name(error), static_cast<unsigned>(error));
}

} // namespace

// ---------------------------------------------------------------------------
// Caps
// ---------------------------------------------------------------------------

std::vector<std::string> Caps::missing() const {
    std::vector<std::string> out;
    if (! image_base) {
        out.emplace_back("EGL_KHR_image_base");
    }
    if (! dmabuf_import) {
        out.emplace_back("EGL_EXT_image_dma_buf_import");
    }
    if (! surfaceless_context) {
        out.emplace_back("EGL_KHR_surfaceless_context");
    }
    return out;
}

std::string Caps::to_string() const {
    std::string out = fmt("EGL {} by '{}', apis '{}'", version, vendor, client_apis);
    auto flag = [&out](const char* name, bool value) {
        out += fmt("\n  {:<38} {}", name, value ? "yes" : "no");
    };
    flag("EGL_KHR_image_base", image_base);
    flag("EGL_EXT_image_dma_buf_import", dmabuf_import);
    flag("EGL_EXT_image_dma_buf_import_modifiers", dmabuf_import_modifiers);
    flag("EGL_KHR_surfaceless_context", surfaceless_context);
    flag("EGL_KHR_no_config_context", no_config_context);
    flag("EGL_MESA_image_dma_buf_export", dmabuf_export);
    flag("EGL_KHR_fence_sync", fence_sync);
    flag("EGL_ANDROID_native_fence_sync", native_fence_sync);
    flag("EGL_KHR_wait_sync", wait_sync);
    return out;
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

Display::~Display() {
    if (display_ == nullptr) {
        return;
    }
    auto dpy = static_cast<EGLDisplay>(display_);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_ != nullptr) {
        eglDestroyContext(dpy, static_cast<EGLContext>(context_));
        context_ = nullptr;
    }
    eglTerminate(dpy);
    display_ = nullptr;
}

Display::Display(Display&& other) noexcept
    : display_(std::exchange(other.display_, nullptr)),
      context_(std::exchange(other.context_, nullptr)),
      config_(std::exchange(other.config_, nullptr)),
      caps_(std::move(other.caps_)) {}

Display& Display::operator=(Display&& other) noexcept {
    if (this != &other) {
        this->~Display();
        display_ = std::exchange(other.display_, nullptr);
        context_ = std::exchange(other.context_, nullptr);
        config_ = std::exchange(other.config_, nullptr);
        caps_ = std::move(other.caps_);
    }
    return *this;
}

Result<Display> Display::create(const gbm::Device& device) {
    if (! device.valid()) {
        return Err(drm::Errc::Internal, "egl::Display::create() with an empty gbm device");
    }

    // 优先走 eglGetPlatformDisplay（EGL 1.5）。取不到就退回
    // eglGetPlatformDisplayEXT，再不行才用 eglGetDisplay ——
    // 后者对 GBM native display 的解释依赖实现，不可靠。
    // gbm_device* 就是 GBM 平台的 native display
    EGLDisplay dpy = EGL_NO_DISPLAY;
    void* gbm_native = device.raw();
    if (gbm_native == nullptr) {
        return Err(drm::Errc::Internal, "gbm device has no native handle");
    }

    const char* client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    const bool have_platform_gbm = has_extension(client_exts, "EGL_KHR_platform_gbm") ||
                                   has_extension(client_exts, "EGL_MESA_platform_gbm");

    if (have_platform_gbm) {
        auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (get_platform_display != nullptr) {
            dpy = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_native, nullptr);
        }
    }
    if (dpy == EGL_NO_DISPLAY) {
        LOG_WARN("no GBM platform display available, falling back to eglGetDisplay(); "
                 "the result depends on the EGL implementation");
        dpy = eglGetDisplay(static_cast<EGLNativeDisplayType>(gbm_native));
    }
    if (dpy == EGL_NO_DISPLAY) {
        return Err(drm::Errc::Unsupported,
                   fmt("could not obtain an EGL display for the GBM device: {}",
                       last_egl_error()));
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(dpy, &major, &minor) != EGL_TRUE) {
        return Err(drm::Errc::Unsupported,
                   fmt("eglInitialize failed: {}", last_egl_error()));
    }

    Display out;
    out.display_ = dpy;

    // ---- 能力探测。全部运行时判断，不用 #ifdef。 ----
    const char* exts = eglQueryString(dpy, EGL_EXTENSIONS);
    Caps& caps = out.caps_;
    caps.version = fmt("{}.{}", major, minor);
    const char* vendor = eglQueryString(dpy, EGL_VENDOR);
    caps.vendor = vendor != nullptr ? vendor : "<unknown>";
    const char* apis = eglQueryString(dpy, EGL_CLIENT_APIS);
    caps.client_apis = apis != nullptr ? apis : "<unknown>";

    caps.image_base = has_extension(exts, "EGL_KHR_image_base");
    caps.dmabuf_import = has_extension(exts, "EGL_EXT_image_dma_buf_import");
    caps.dmabuf_import_modifiers = has_extension(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    caps.surfaceless_context = has_extension(exts, "EGL_KHR_surfaceless_context");
    caps.no_config_context = has_extension(exts, "EGL_KHR_no_config_context");
    caps.dmabuf_export = has_extension(exts, "EGL_MESA_image_dma_buf_export");
    caps.fence_sync = has_extension(exts, "EGL_KHR_fence_sync");
    caps.native_fence_sync = has_extension(exts, "EGL_ANDROID_native_fence_sync");
    caps.wait_sync = has_extension(exts, "EGL_KHR_wait_sync");

    if (! caps.sufficient_for_rendering()) {
        std::string names;
        for (const auto& name : caps.missing()) {
            if (! names.empty()) {
                names += ", ";
            }
            names += name;
        }
        return Err(drm::Errc::Unsupported,
                   fmt("the EGL implementation is missing required extension(s): {}", names));
    }

    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        return Err(drm::Errc::Unsupported,
                   fmt("eglBindAPI(OpenGL ES) failed: {}", last_egl_error()));
    }

    // 不创建 EGLSurface，所以不需要挑一个匹配窗口的 config。
    // 有 no_config_context 就完全不挑；没有就挑一个最小可用的。
    EGLConfig config = nullptr;
    if (! caps.no_config_context) {
        const EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                       EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                                       EGL_RED_SIZE, 8,
                                       EGL_GREEN_SIZE, 8,
                                       EGL_BLUE_SIZE, 8,
                                       EGL_NONE};
        EGLint count = 0;
        if (eglChooseConfig(dpy, config_attrs, &config, 1, &count) != EGL_TRUE || count < 1) {
            return Err(drm::Errc::Unsupported,
                       fmt("eglChooseConfig found no usable config: {}", last_egl_error()));
        }
    }
    out.config_ = config;

    const EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext context = eglCreateContext(dpy, caps.no_config_context ? EGL_NO_CONFIG_KHR : config,
                                          EGL_NO_CONTEXT, context_attrs);
    if (context == EGL_NO_CONTEXT) {
        return Err(drm::Errc::Unsupported,
                   fmt("eglCreateContext failed: {}", last_egl_error()));
    }
    out.context_ = context;

    TRY(out.make_current());

    LOG_INFO("EGL {} ready on '{}' -- renderer '{}'", caps.version, caps.vendor,
             out.gl_renderer());
    if (! caps.dmabuf_import_modifiers) {
        LOG_WARN("EGL_EXT_image_dma_buf_import_modifiers is absent; only linear buffers can be "
                 "imported reliably");
    }
    return Ok(std::move(out));
}

Status Display::make_current() const {
    if (display_ == nullptr || context_ == nullptr) {
        return Err(drm::Errc::Internal, "make_current() on an uninitialised EGL display");
    }
    // 无表面：这正是 EGL_KHR_surfaceless_context 的用途。
    if (eglMakeCurrent(static_cast<EGLDisplay>(display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                       static_cast<EGLContext>(context_)) != EGL_TRUE) {
        return Err(drm::Errc::Unsupported,
                   fmt("eglMakeCurrent (surfaceless) failed: {}", last_egl_error()));
    }
    return Ok();
}

Result<Image> Display::import_dmabuf(const drm::DmabufDesc& desc) const {
    if (display_ == nullptr) {
        return Err(drm::Errc::Internal, "import_dmabuf() on an uninitialised EGL display");
    }
    TRY(desc.validate());

    const bool use_modifiers =
        desc.modifier != drm::kModifierInvalid && caps_.dmabuf_import_modifiers;

    if (desc.modifier != drm::kModifierInvalid && ! caps_.dmabuf_import_modifiers) {
        // 忽略 modifier 去导入一块非线性 buffer，结果是花屏而不是干净的失败，
        // 排查成本极高。宁可吵一点。
        LOG_WARN("importing a buffer with modifier {} but the implementation cannot accept "
                 "modifiers; the result will be wrong unless the layout happens to be linear",
                 drm::to_string(desc.modifier));
    }

    // 属性表按平面展开。EGL 的属性名是每平面一组常量，没有下标形式，
    // 所以只能手工列开。
    static const EGLint kFdAttr[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                                      EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint kOffsetAttr[4] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint kPitchAttr[4] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint kModLoAttr[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint kModHiAttr[4] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};

    std::vector<EGLint> attrs;
    attrs.reserve(8 + desc.num_planes * 10);
    attrs.push_back(EGL_WIDTH);
    attrs.push_back(static_cast<EGLint>(desc.size.width));
    attrs.push_back(EGL_HEIGHT);
    attrs.push_back(static_cast<EGLint>(desc.size.height));
    attrs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
    attrs.push_back(static_cast<EGLint>(desc.format));

    for (uint32_t i = 0; i < desc.num_planes && i < 4; ++i) {
        attrs.push_back(kFdAttr[i]);
        attrs.push_back(desc.fds[i].get());
        attrs.push_back(kOffsetAttr[i]);
        attrs.push_back(static_cast<EGLint>(desc.offsets[i]));
        attrs.push_back(kPitchAttr[i]);
        attrs.push_back(static_cast<EGLint>(desc.strides[i]));
        if (use_modifiers) {
            const uint64_t raw = static_cast<uint64_t>(desc.modifier);
            attrs.push_back(kModLoAttr[i]);
            attrs.push_back(static_cast<EGLint>(raw & 0xffffffffu));
            attrs.push_back(kModHiAttr[i]);
            attrs.push_back(static_cast<EGLint>(raw >> 32));
        }
    }
    attrs.push_back(EGL_NONE);

    auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    if (create_image == nullptr) {
        return Err(drm::Errc::Unsupported, "eglCreateImageKHR is not available");
    }

    EGLImageKHR image = create_image(static_cast<EGLDisplay>(display_), EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        return Err(drm::Errc::Unsupported,
                   fmt("eglCreateImageKHR(dmabuf) failed: {} -- {}", last_egl_error(),
                       desc.to_string()));
    }

    LOG_DEBUG("imported dmabuf as EGLImage: {}", desc.to_string());
    return Ok(Image(display_, image));
}

std::string Display::gl_renderer() const {
    const auto* renderer = glGetString(GL_RENDERER);
    if (renderer == nullptr) {
        return "<no current context>";
    }
    return std::string(reinterpret_cast<const char*>(renderer));
}

std::string Display::to_string() const {
    if (display_ == nullptr) {
        return "<empty EGL display>";
    }
    return fmt("EGL {} vendor='{}'", caps_.version, caps_.vendor);
}

// ---------------------------------------------------------------------------
// Image
// ---------------------------------------------------------------------------

Image::~Image() {
    if (image_ == nullptr || display_ == nullptr) {
        return;
    }
    auto destroy_image =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    if (destroy_image != nullptr) {
        destroy_image(static_cast<EGLDisplay>(display_), static_cast<EGLImageKHR>(image_));
    }
    image_ = nullptr;
    display_ = nullptr;
}

Image::Image(Image&& other) noexcept
    : display_(std::exchange(other.display_, nullptr)),
      image_(std::exchange(other.image_, nullptr)) {}

Image& Image::operator=(Image&& other) noexcept {
    if (this != &other) {
        this->~Image();
        display_ = std::exchange(other.display_, nullptr);
        image_ = std::exchange(other.image_, nullptr);
    }
    return *this;
}

} // namespace mw::egl
