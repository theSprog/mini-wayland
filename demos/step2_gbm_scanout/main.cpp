/**
 * demos/step2_gbm_scanout -- Step 2 的验收程序
 *
 * 一条完整的现代上屏链路：
 *
 *   分配（显示侧 dumb / 渲染侧 GBM）
 *     -> 导出 dmabuf
 *     -> [GL] 导入成 EGLImage -> FBO -> GLES 绘制
 *     -> [CPU] mmap 直接写像素
 *     -> PRIME 导入 + addfb2（渲染侧分配时）
 *     -> atomic commit
 *
 * ## 为什么 CPU 与 GL 两条画法在同一个程序里
 *
 * 设计文档原本计划两个 demo（`step2_gbm_scanout` 与 `step2_gles_cube`）。
 * 合成一个，因为二者的差别**只有"谁往 buffer 里写像素"这一段**，
 * 而 modeset、帧循环、事件处理、退出清理、ioctl 记账是逐字相同的几百行。
 * 抄两份的直接后果是：其中一份出了 bug，另一份照样绿，而你会相信绿的那份。
 *
 * 于是：`--draw cpu` 先跑通链路，`--draw gl` 再把 GL 接上去。
 * GL 出问题时能立刻排除是链路问题 —— 这正是原计划想要的性质。
 *
 * ## 需要 DRM master
 *
 *   sudo systemctl stop lightdm     # 或者 Ctrl+Alt+F3 切到裸 tty
 *   sudo ./build/debug/bin/step2_gbm_scanout --draw cpu
 *
 * ## 用法
 *
 *   -d <name>       按 DRM driver name 打开 KMS 节点
 *   -D <path>       指定 KMS 节点
 *   -g <path>       GBM / EGL 用的节点（默认：实测每个节点后挑一个）
 *   -s <kind>       分配来源：scanout（dumb）| render（GBM）。默认 scanout
 *   --draw <kind>   cpu | gl。默认 cpu
 *   -f <n>          跑 n 帧后退出
 *   -b <n>          缓冲数，2 或 3（默认 2）
 *   --no-modifiers  忽略 IN_FORMATS，模拟一个不支持 modifier 的驱动
 *   --dry-run       只做到 modeset 的 TEST_ONLY
 *   -h
 *
 * ## 验收看什么
 *
 *  1. 画面正常、无撕裂
 *  2. 启动时打印的 swapchain 摘要里，modifier 是不是你以为的那个
 *  3. **稳态每帧的 ioctl 只有 1 次 atomic_commit**。每秒报告里
 *     add_fb / prime_* 的增量必须是 0，不为 0 会直接报 ERROR ——
 *     "每帧重新 import + addfb2" 能跑、看不出问题、但每帧多三次 ioctl，
 *     这是这一步最容易失守的地方。
 *  4. 退出时 create/destroy 配平
 */
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include <GLES2/gl2.h>

// dma-buf 的 CPU 访问同步 ioctl。4.6 起就在，但把它做成可选依赖 ——
// 缺了只是少一次 cache 维护，不该让整个 demo 编不过。
#if defined(__has_include)
#  if __has_include(<linux/dma-buf.h>)
#    include <linux/dma-buf.h>
#    define MW_HAVE_DMA_BUF_SYNC 1
#  endif
#endif

#include "mw/core/log.hpp"
#include "mw/drm/atomic.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/event.hpp"
#include "mw/drm/trace.hpp"
#include "mw/egl/display.hpp"
#include "mw/gbm/device.hpp"
#include "mw/render/buffer_source.hpp"
#include "mw/render/gl_node.hpp"
#include "mw/render/swapchain.hpp"
#include "mw/render/target.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

volatile std::sig_atomic_t g_should_stop = 0;

void on_signal(int /*signum*/) {
    g_should_stop = 1;
}

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // 不设 SA_RESTART：poll 要能被打断
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

// ---------------------------------------------------------------------------
// 选项
// ---------------------------------------------------------------------------

enum class DrawKind { Cpu, Gl };

struct Options {
    const char* driver_name = nullptr;
    const char* device_path = nullptr;
    const char* gbm_node = nullptr;
    render::SourceKind source = render::SourceKind::ScanoutDevice;
    DrawKind draw = DrawKind::Cpu;
    uint64_t frame_limit = 0;
    uint32_t buffer_count = 2;
    std::vector<std::string> skip_nodes{};
    bool no_modifiers = false;
    bool dry_run = false;
    bool verify = false;
    bool unmap_each_frame = false;
};

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  -d <name>       open the KMS node by DRM driver name\n");
    std::printf("  -D <path>       open a specific KMS node\n");
    std::printf("  -g <path>       node for GBM/EGL (default: probe every node and pick;\n");
    std::printf("                  passing it explicitly skips the whole probe)\n");
    std::printf("  -x <path>       do not probe this node (repeatable)\n");
    std::printf("  -s <kind>       allocation source: scanout | render (default scanout)\n");
    std::printf("  --draw <kind>   cpu | gl (default cpu)\n");
    std::printf("  -f <n>          stop after n frames\n");
    std::printf("  -b <n>          number of buffers, 2 or 3 (default 2)\n");
    std::printf("  --no-modifiers  ignore IN_FORMATS, as if the driver had none\n");
    std::printf("  --verify        read the first frame back through every available\n");
    std::printf("                  channel and compare them before going on screen\n");
    std::printf("  --unmap-each-frame  diagnostic: unmap the CPU mapping after every\n");
    std::printf("                  frame. Costs map/unmap per frame, so it violates the\n");
    std::printf("                  steady-state ioctl budget on purpose\n");
    std::printf("  --dry-run       stop after the modeset TEST_ONLY\n");
    std::printf("  -h              this help\n");
    std::printf("\nthis program needs DRM master:\n");
    std::printf("  sudo systemctl stop lightdm    # or switch to a bare tty\n");
    std::printf("\nenvironment: MW_LOG=error|warn|info|debug|trace, MW_LOG_TIME=1\n");
}

// ---------------------------------------------------------------------------
// CPU 绘制
// ---------------------------------------------------------------------------

/**
 * @brief CPU 往 buffer 里画一帧
 *
 * 图案要能用肉眼判断三件事：颜色顺序（格式搞错会串色）、
 * 竖条平移（撕裂会让它在某一行断开）、横带下移（垂直更新是否连续）。
 *
 * 性能：这类内存通常是 write-combining，顺序宽写快、读回极慢。
 * 所以先在普通内存里拼好一整行再整行 memcpy —— 只有一次顺序写穿过它。
 */
/**
 * @brief 竖彩条的调色板
 *
 * 提到文件作用域是因为 `--verify` 要在不重画一遍的前提下算出
 * 某个坐标**应该**是什么颜色。两处用同一份常量，改了颜色不会
 * 让自检悄悄失去意义。
 */
const uint32_t kBars[] = {
    0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
    0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u,
};
constexpr uint32_t kBarCount = sizeof(kBars) / sizeof(kBars[0]);

void draw_cpu(span<uint8_t> pixels, Size size, uint32_t stride, uint64_t frame) {
    static std::vector<uint32_t> normal_row;
    static std::vector<uint32_t> band_row;
    normal_row.resize(size.width);
    band_row.resize(size.width);

    const uint32_t bar_width = size.width / kBarCount;
    const auto marker_x = static_cast<uint32_t>((frame * 4u) % size.width);
    constexpr uint32_t kMarkerWidth = 8u;

    for (uint32_t x = 0; x < size.width; ++x) {
        const uint32_t bar = bar_width > 0u ? (x / bar_width) % kBarCount : 0u;
        uint32_t color = kBars[bar];
        const bool in_marker =
            (x >= marker_x && x < marker_x + kMarkerWidth) ||
            (marker_x + kMarkerWidth > size.width && x < (marker_x + kMarkerWidth) - size.width);
        if (in_marker) {
            color = 0x00ff8000u;
        }
        normal_row[x] = color;
        band_row[x] = ~color & 0x00ffffffu;
    }

    const auto band_y = static_cast<uint32_t>((frame * 3u) % size.height);
    constexpr uint32_t kBandHeight = 6u;
    const size_t row_bytes = static_cast<size_t>(size.width) * 4u;

    for (uint32_t y = 0; y < size.height; ++y) {
        const size_t offset = static_cast<size_t>(y) * stride;
        if (offset + row_bytes > pixels.size()) {
            break;  // 映射比 stride*height 短：分配器给的 size 有问题，别越界
        }
        const bool in_band = (y >= band_y && y < band_y + kBandHeight) ||
                             (band_y + kBandHeight > size.height &&
                              y < (band_y + kBandHeight) - size.height);
        const uint32_t* source = in_band ? band_row.data() : normal_row.data();
        std::memcpy(pixels.data() + offset, source, row_bytes);
    }
}

// ---------------------------------------------------------------------------
// 读回自检
// ---------------------------------------------------------------------------

/*
 * ## 为什么需要这个
 *
 * 这条链路上的每一层都只校验元数据：
 *
 *   PRIME_FD_TO_HANDLE  只说明内核建出了 GEM object
 *   addfb2              只校验 format/stride/size 自洽
 *   TEST_ONLY           只校验 plane 约束（缩放、带宽、格式组合）
 *   page flip event     只说明 CRTC 翻页了
 *
 * **没有任何一层校验"像素真的在那块内存里、而且显示引擎读得到"。**
 * 所以整条链路可以全绿而屏幕是黑的 —— 这个失败模式已经在
 * RenderDevice 路径上出现过一次。
 *
 * 这里做的事：把第一帧从**所有能用的通道**读回来，横向比对。
 * 通道之间不一致，比任何单个通道的绝对值都更有信息量：
 *
 *   GL 有内容 + dmabuf mmap 全零   -> GPU 没写进这块 dmabuf 的内存
 *   CPU 映射有内容 + mmap 全零     -> gbm_bo_map 给的是 staging buffer，
 *                                     真正的拷回发生在 gbm_bo_unmap
 *   两个通道都有内容 + 屏幕是黑的  -> 内存是对的，显示引擎读不到，
 *                                     问题在 KMD 的地址/映射
 *   mmap 直接失败                  -> 导出方没实现 dma-buf mmap，
 *                                     这本身是一条要记进 docs 的能力结论
 *
 * 这不是一次性的排障脚本。Step 3 起 buffer 全部来自别的进程和别的设备，
 * 同一类"层层成功、画面全黑"会反复出现，得有个机器可判的手段。
 */

/// 一个采样点在各通道上的读数。has_* 为 false 表示该通道本次不可用。
struct SamplePoint {
    uint32_t x = 0;
    uint32_t y = 0;

    uint32_t expected = 0;
    bool has_expected = false;

    uint32_t cpu = 0;
    bool has_cpu = false;

    uint32_t dmabuf = 0;
    bool has_dmabuf = false;

    uint32_t gl = 0;
    bool has_gl = false;
};

/// 采样点：每根彩条的中心，取一行不会落在滚动横带里的 y。
std::vector<SamplePoint> make_sample_points(Size size) {
    std::vector<SamplePoint> points;
    const uint32_t bar_width = size.width / kBarCount;
    if (bar_width == 0) {
        return points;
    }
    // frame 0 的横带在 y=[0,6)，游标在 x=[0,8)，取中间的行与条心都避开。
    const uint32_t y = size.height / 2;
    for (uint32_t bar = 0; bar < kBarCount; ++bar) {
        SamplePoint point;
        point.x = bar * bar_width + bar_width / 2;
        point.y = y;
        point.expected = kBars[bar];
        points.push_back(point);
    }
    return points;
}

/// XR24 在内存里是小端的 0x00RRGGBB，高 8 位是 X，比较时一律抹掉。
uint32_t load_xrgb(const uint8_t* base, size_t stride, uint32_t x, uint32_t y) {
    uint32_t value = 0;
    std::memcpy(&value, base + (static_cast<size_t>(y) * stride) + (static_cast<size_t>(x) * 4u),
                sizeof(value));
    return value & 0x00ffffffu;
}

#if defined(MW_HAVE_DMA_BUF_SYNC)
void sync_dma_buf(int fd, uint64_t flags) {
    struct dma_buf_sync sync{};
    sync.flags = flags;
    if (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) != 0) {
        // 不是所有导出方都实现了它。失败只影响 cache 一致性，
        // 而且失败本身也是一条能力观察，所以只降级成 DEBUG。
        LOG_DEBUG("DMA_BUF_IOCTL_SYNC(flags={}) failed: {}", flags, errno_name(errno));
    }
}
#endif

/**
 * @brief 直接 mmap dmabuf fd 读回
 *
 * 这是**绕过分配器的那条通道**：不经过 gbm_bo_map，也不经过 GL，
 * 看到的是这个 fd 背后真正的那块内存。
 */
void sample_via_dmabuf(const render::ScanoutBuffer& buffer, std::vector<SamplePoint>& points) {
    const drm::DmabufDesc& desc = buffer.dmabuf();
    if (desc.num_planes != 1) {
        LOG_WARN("  dmabuf readback skipped: {} planes, this check only handles single-plane",
                 desc.num_planes);
        return;
    }
    const int fd = desc.fds[0].get();
    if (fd < 0) {
        return;
    }

    const size_t stride = buffer.stride();
    const size_t length =
        static_cast<size_t>(desc.offsets[0]) + (stride * static_cast<size_t>(buffer.size().height));

    void* mapped = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        LOG_WARN("  dmabuf readback unavailable: mmap on the dmabuf fd failed with {}",
                 errno_name(errno));
        LOG_WARN("  that means the exporter does not implement dma-buf mmap; record it in docs "
                 "and fall back to comparing the CPU and GL channels only");
        return;
    }

#if defined(MW_HAVE_DMA_BUF_SYNC)
    sync_dma_buf(fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
#endif

    const auto* base = static_cast<const uint8_t*>(mapped) + desc.offsets[0];
    for (SamplePoint& point : points) {
        point.dmabuf = load_xrgb(base, stride, point.x, point.y);
        point.has_dmabuf = true;
    }

#if defined(MW_HAVE_DMA_BUF_SYNC)
    sync_dma_buf(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
#endif

    munmap(mapped, length);
}

/// 通过分配器给的 CPU 映射读回（dumb 是 mmap，GBM 是 gbm_bo_map）。
void sample_via_cpu_map(render::ScanoutBuffer& buffer, std::vector<SamplePoint>& points) {
    if (! buffer.cpu_writable()) {
        return;
    }
    auto pixels = buffer.map_write();
    if (! pixels) {
        LOG_WARN("  cpu readback unavailable: {}", pixels.error().message);
        return;
    }
    // 写合并内存读回极慢，所以只读这几个点，不要在这里遍历整帧。
    const size_t stride = buffer.stride();
    for (SamplePoint& point : points) {
        const size_t offset = (static_cast<size_t>(point.y) * stride) +
                              (static_cast<size_t>(point.x) * 4u);
        if (offset + 4u > pixels.value().size()) {
            continue;
        }
        point.cpu = load_xrgb(pixels.value().data(), stride, point.x, point.y);
        point.has_cpu = true;
    }
}

/**
 * @brief 从 GL 的 FBO 读回
 *
 * @note GL 的原点在左下，buffer 的第 0 行在顶部，所以 y 要翻转。
 *       GLES 只保证 GL_RGBA/GL_UNSIGNED_BYTE 这一组合可读。
 */
void sample_via_gl(const render::Swapchain::Slot& slot, std::vector<SamplePoint>& points) {
    if (! slot.has_target()) {
        return;
    }
    if (auto status = slot.target.bind(); ! status) {
        LOG_WARN("  gl readback unavailable: {}", status.error().message);
        return;
    }
    const uint32_t height = slot.buffer.size().height;
    for (SamplePoint& point : points) {
        uint8_t rgba[4] = {0, 0, 0, 0};
        const auto gl_y = static_cast<GLint>(height - 1u - point.y);
        glReadPixels(static_cast<GLint>(point.x), gl_y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        if (glGetError() != GL_NO_ERROR) {
            LOG_WARN("  gl readback failed at ({}, {})", point.x, point.y);
            break;
        }
        point.gl = (static_cast<uint32_t>(rgba[0]) << 16) |
                   (static_cast<uint32_t>(rgba[1]) << 8) | static_cast<uint32_t>(rgba[2]);
        point.has_gl = true;
    }
    render::GlRenderTarget::unbind();
}

/// 固定 8 字符宽，缺席的通道用横线占位，这样表格列是对齐的。
/// 内部 fmt 的语法是 [[fill]align][width][type]，**没有 '#' 前缀形式**，
/// 所以 "0x" 得自己拼。
std::string format_channel(bool has, uint32_t value) {
    return has ? ("0x" + fmt("{:06x}", value)) : std::string("--------");
}

/**
 * @brief 第一帧画完之后、上屏之前跑一次
 *
 * @return 全部可用通道一致时为 true。**返回 false 不终止 demo** ——
 *         把画面留在屏幕上，用眼睛和这份表格对照，比直接退出有用。
 */
bool verify_first_frame(render::Swapchain::Slot& slot, bool cpu_drawn) {
    LOG_INFO("--verify: reading the first frame back through every available channel");
    LOG_SCOPE();

    std::vector<SamplePoint> points = make_sample_points(slot.buffer.size());
    if (points.empty()) {
        LOG_WARN("the buffer is too narrow to sample; skipping");
        return true;
    }
    if (! cpu_drawn) {
        // GL 画的内容不是彩条，没有可预测的期望值。这时候自检的判据
        // 只有"各通道是否互相一致"，而那恰恰是分辨 A/B 需要的东西。
        for (SamplePoint& point : points) {
            point.has_expected = false;
        }
    } else {
        for (SamplePoint& point : points) {
            point.has_expected = true;
        }
    }

    sample_via_gl(slot, points);
    sample_via_cpu_map(slot.buffer, points);
    sample_via_dmabuf(slot.buffer, points);

    LOG_INFO("      x     y expected       gl      cpu   dmabuf");
    bool all_zero_dmabuf = true;
    bool any_dmabuf = false;
    bool mismatch = false;

    for (const SamplePoint& point : points) {
        LOG_INFO("  {:>5} {:>5} {} {} {} {}", point.x, point.y,
                 format_channel(point.has_expected, point.expected),
                 format_channel(point.has_gl, point.gl),
                 format_channel(point.has_cpu, point.cpu),
                 format_channel(point.has_dmabuf, point.dmabuf));

        if (point.has_dmabuf) {
            any_dmabuf = true;
            if (point.dmabuf != 0) {
                all_zero_dmabuf = false;
            }
        }
        if (point.has_expected && point.has_cpu && point.cpu != point.expected) {
            mismatch = true;
        }
        if (point.has_expected && point.has_dmabuf && point.dmabuf != point.expected) {
            mismatch = true;
        }
        if (point.has_cpu && point.has_dmabuf && point.cpu != point.dmabuf) {
            mismatch = true;
        }
    }

    if (any_dmabuf && all_zero_dmabuf) {
        LOG_ERROR("every sample read back as zero through the dmabuf fd: the pixels never "
                  "reached the memory this fd refers to");
        LOG_ERROR("if the cpu column has content, the allocator handed out a staging buffer "
                  "and the copy back only happens on unmap");
        LOG_ERROR("if the gl column has content, the GPU wrote somewhere else than this dmabuf");
        return false;
    }
    if (mismatch) {
        LOG_ERROR("the channels disagree; the columns above say which pair");
        return false;
    }

    LOG_INFO("every available channel agrees");
    if (! any_dmabuf) {
        LOG_WARN("the dmabuf channel was unavailable, so this only proves the allocator's own "
                 "mapping is consistent -- it says nothing about what the display engine reads");
    } else {
        LOG_INFO("the pixels are in the memory this dmabuf fd refers to; if the screen is still "
                 "blank, the display engine cannot reach that memory and the problem is below "
                 "this program");
    }
    return true;
}

// ---------------------------------------------------------------------------
// GL 绘制
// ---------------------------------------------------------------------------

/**
 * @brief 最小 GLES2 场景：一个旋转的彩色四边形 + 动画背景
 *
 * 刻意**不画立方体**：立方体要深度缓冲，那意味着还要给 FBO 配一个 depth
 * renderbuffer，多一个独立的失败点。这一步要验证的是"导入的 dmabuf 能不能
 * 当渲染目标"，不是 3D 管线。深度附件留到真正需要它的时候再加。
 */
class GlScene {
  public:
    GlScene() = default;
    ~GlScene() {
        if (program_ != 0u) {
            glDeleteProgram(program_);
        }
    }

    GlScene(const GlScene&) = delete;
    GlScene& operator=(const GlScene&) = delete;
    GlScene(GlScene&&) = delete;
    GlScene& operator=(GlScene&&) = delete;

    Status init() {
        static const char* kVertex =
            "attribute vec2 a_pos;\n"
            "attribute vec3 a_color;\n"
            "uniform mat2 u_rot;\n"
            "uniform float u_aspect;\n"
            "varying vec3 v_color;\n"
            "void main() {\n"
            "    vec2 p = u_rot * a_pos;\n"
            "    p.x /= u_aspect;\n"
            "    gl_Position = vec4(p, 0.0, 1.0);\n"
            "    v_color = a_color;\n"
            "}\n";
        static const char* kFragment =
            "precision mediump float;\n"
            "varying vec3 v_color;\n"
            "void main() {\n"
            "    gl_FragColor = vec4(v_color, 1.0);\n"
            "}\n";

        const GLuint vertex = TRY(compile(GL_VERTEX_SHADER, kVertex));
        const GLuint fragment = TRY(compile(GL_FRAGMENT_SHADER, kFragment));

        program_ = glCreateProgram();
        glAttachShader(program_, vertex);
        glAttachShader(program_, fragment);
        glLinkProgram(program_);
        // 链接之后 shader 对象就可以删了，program 自己持有引用。
        glDeleteShader(vertex);
        glDeleteShader(fragment);

        GLint linked = GL_FALSE;
        glGetProgramiv(program_, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            char log[512] = {};
            GLsizei length = 0;
            glGetProgramInfoLog(program_, sizeof(log) - 1, &length, log);
            return Err(Errc::Internal, fmt("shader link failed: {}", log));
        }

        attr_pos_ = glGetAttribLocation(program_, "a_pos");
        attr_color_ = glGetAttribLocation(program_, "a_color");
        uniform_rot_ = glGetUniformLocation(program_, "u_rot");
        uniform_aspect_ = glGetUniformLocation(program_, "u_aspect");
        if (attr_pos_ < 0 || attr_color_ < 0) {
            return Err(Errc::Internal, "the shader lost its attributes during linking");
        }
        return Ok();
    }

    void draw(Size size, uint64_t frame) const {
        const float t = static_cast<float>(frame) * 0.02f;

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glClearColor(0.10f + 0.10f * std::sin(t), 0.12f, 0.18f + 0.10f * std::cos(t), 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        static const GLfloat kPositions[] = {
            -0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f,
        };
        static const GLfloat kColors[] = {
            1.0f, 0.2f, 0.2f, 0.2f, 1.0f, 0.2f, 0.2f, 0.4f, 1.0f, 1.0f, 1.0f, 0.2f,
        };

        const float c = std::cos(t);
        const float s = std::sin(t);
        // GLSL 的 mat2 是列主序：[c s; -s c] 表示绕原点旋转
        const GLfloat rot[4] = {c, s, -s, c};
        const float aspect = size.height != 0u
                                 ? static_cast<float>(size.width) / static_cast<float>(size.height)
                                 : 1.0f;

        glUseProgram(program_);
        glUniformMatrix2fv(uniform_rot_, 1, GL_FALSE, rot);
        glUniform1f(uniform_aspect_, aspect);

        const auto pos = static_cast<GLuint>(attr_pos_);
        const auto color = static_cast<GLuint>(attr_color_);
        glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, kPositions);
        glVertexAttribPointer(color, 3, GL_FLOAT, GL_FALSE, 0, kColors);
        glEnableVertexAttribArray(pos);
        glEnableVertexAttribArray(color);
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
        glDisableVertexAttribArray(pos);
        glDisableVertexAttribArray(color);
    }

  private:
    static Result<GLuint> compile(GLenum type, const char* source) {
        const GLuint shader = glCreateShader(type);
        if (shader == 0u) {
            return Err(Errc::Internal, "glCreateShader returned 0");
        }
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);

        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (compiled != GL_TRUE) {
            char log[512] = {};
            GLsizei length = 0;
            glGetShaderInfoLog(shader, sizeof(log) - 1, &length, log);
            glDeleteShader(shader);
            return Err(Errc::Internal, fmt("shader compilation failed: {}", log));
        }
        return Ok(shader);
    }

    GLuint program_ = 0;
    GLint attr_pos_ = -1;
    GLint attr_color_ = -1;
    GLint uniform_rot_ = -1;
    GLint uniform_aspect_ = -1;
};

// ---------------------------------------------------------------------------
// modifier 候选
// ---------------------------------------------------------------------------

/**
 * @brief 从目标 plane 的 IN_FORMATS 里取出该 format 的全部 modifier
 *
 * **原样转发，不排序、不解码 vendor 位。** 排序属于协商策略（Step 4 的
 * dmabuf-feedback tranche），这一步只负责把驱动说的话交给分配器。
 *
 * kModifierInvalid 要剔掉：它表示"这个 plane 没有 modifier 信息"，
 * 不是一个可以拿去分配的值。
 */
std::vector<Modifier> plane_modifiers(const Plane& plane, Format format) {
    std::vector<Modifier> out;
    for (const FormatModifier& entry : plane.formats) {
        if (entry.format != format || entry.modifier == kModifierInvalid) {
            continue;
        }
        out.push_back(entry.modifier);
    }
    return out;
}

// ---------------------------------------------------------------------------
// modeset / teardown（与 step1 同一套，行为上刻意保持一致）
// ---------------------------------------------------------------------------

Status do_modeset(const Device& device, const OutputPath& path, AtomicRequest& request,
                  const PropertyBlob& mode_blob, FbId first_fb) {
    const Connector* connector = device.connector(path.connector);
    const Crtc* crtc = device.crtc(path.crtc);
    const Plane* plane = device.plane(path.primary_plane);
    if (connector == nullptr || crtc == nullptr || plane == nullptr) {
        return Err(Errc::StaleSnapshot, "output path refers to objects not in the snapshot");
    }

    request.reset();
    TRY(request.bind_connector(*connector, path.crtc));
    TRY(request.set_crtc_mode(*crtc, mode_blob.id(), true));
    TRY(request.set_plane(*plane, first_fb, path.crtc, SrcRect::whole(path.size()),
                          CrtcRect::at_origin(path.size())));
    request.dump("modeset");

    const int test_result = request.test(CommitFlags::AllowModeset);
    if (test_result != 0) {
        LOG_ERROR("modeset TEST_ONLY rejected with {}", errno_name(test_result));
        // TEST_ONLY 在这一步失败的高频原因和 Step 1 不同：那时只有 dumb，
        // 现在 fb 可能带着一个 plane 其实不接受的 modifier。
        LOG_ERROR("if the buffer carries a modifier, the plane may not accept that exact "
                  "(format, modifier) pair even though IN_FORMATS advertises it");
        request.bisect_rejection(CommitFlags::AllowModeset);
        return Err(Errc::AtomicTestFailed,
                   fmt("modeset TEST_ONLY failed with {}", errno_name(test_result)));
    }
    LOG_INFO("modeset TEST_ONLY passed");
    return Ok();
}

void teardown(const Device& device, const OutputPath& path, AtomicRequest& request) {
    const Connector* connector = device.connector(path.connector);
    const Crtc* crtc = device.crtc(path.crtc);
    if (connector == nullptr || crtc == nullptr) {
        LOG_WARN("cannot tear down: objects vanished from the snapshot");
        return;
    }

    LOG_INFO("tearing down: disabling every plane on {} then the CRTC itself",
             to_string(path.crtc));
    request.reset();
    for (const PlaneType type : {PlaneType::Primary, PlaneType::Overlay, PlaneType::Cursor}) {
        for (const PlaneId id : device.planes_for_crtc(path.crtc, type)) {
            const Plane* plane = device.plane(id);
            if (plane == nullptr) {
                continue;
            }
            if (auto status = request.disable_plane(*plane); ! status) {
                log_error_object(status.error(), "disable_plane");
            }
        }
    }
    if (auto status = request.disable_crtc(*crtc); ! status) {
        log_error_object(status.error(), "disable_crtc");
    }
    if (auto status = request.bind_connector(*connector, kNoCrtc); ! status) {
        log_error_object(status.error(), "unbind connector");
    }
    if (auto status = request.commit(CommitFlags::AllowModeset); ! status) {
        log_error_object(status.error(), "teardown commit");
        LOG_WARN("the display may be left in an inconsistent state");
    } else {
        LOG_INFO("teardown committed");
    }
}

// ---------------------------------------------------------------------------
// 帧循环
// ---------------------------------------------------------------------------

struct FrameContext {
    const Device& device;
    const OutputPath& path;
    AtomicRequest& request;
    render::Swapchain& chain;
    const egl::Display* display;  ///< GL 路径下非空
    const GlScene* scene;         ///< 同上
    const Options& options;
};

Status draw_into(const FrameContext& ctx, render::Swapchain::Slot& slot, uint64_t frame) {
    if (ctx.options.draw == DrawKind::Cpu) {
        auto pixels = slot.buffer.map_write();
        if (! pixels) {
            return unexpected<Error>(pixels.error());
        }
        draw_cpu(pixels.value(), slot.buffer.size(), slot.buffer.stride(), frame);
        if (ctx.options.unmap_each_frame) {
            // 见 ScanoutBuffer::end_cpu_write()。这里放的是一个假设的开关，
            // 不是一个优化：如果 unmap 之后画面才对，那说明上面那次写落在了
            // staging buffer 里，而不是 dmabuf 背后的内存。
            slot.buffer.end_cpu_write();
        }
        return Ok();
    }

    TRY(slot.target.bind());
    ctx.scene->draw(slot.buffer.size(), frame);

    // CPU 在这里阻塞等 GPU。**这是 Step 6 要删掉的那一行**，
    // 理由写在 render/target.hpp 的 finish_rendering() 上。
    // TODO(step6): 换成导出 fence fd 作为 plane 的 IN_FENCE_FD。
    TRY(render::finish_rendering(*ctx.display));
    render::GlRenderTarget::unbind();
    return Ok();
}

Status run_frame_loop(const FrameContext& ctx) {
    const Plane* plane = ctx.device.plane(ctx.path.primary_plane);
    if (plane == nullptr) {
        return Err(Errc::StaleSnapshot, "primary plane vanished");
    }

    const SrcRect src = SrcRect::whole(ctx.path.size());
    const CrtcRect dst = CrtcRect::at_origin(ctx.path.size());
    const uint64_t nominal_frame_ns = ctx.path.mode.frame_duration_ns();

    FrameStats frame_stats;
    IoctlStats last_report = stats();
    uint64_t frame = 0;
    uint64_t last_report_frame = 0;
    timespec last_report_time{};
    clock_gettime(CLOCK_MONOTONIC, &last_report_time);

    LOG_INFO("entering the frame loop; press Ctrl+C to stop");

    while (g_should_stop == 0) {
        if (ctx.options.frame_limit != 0 && frame >= ctx.options.frame_limit) {
            break;
        }

        render::Swapchain::Slot& slot = ctx.chain.acquire();
        TRY(draw_into(ctx, slot, frame));

        ctx.request.reset();
        TRY(ctx.request.set_plane(*plane, slot.buffer.fb_id(), ctx.path.crtc, src, dst));

        const auto commit_status =
            ctx.request.commit(CommitFlags::Nonblock | CommitFlags::PageFlipEvent, frame);
        if (! commit_status) {
            log_error_object(commit_status.error(), "frame commit");
            ctx.request.bisect_rejection(CommitFlags::None);
            return unexpected<Error>(commit_status.error());
        }
        ctx.chain.mark_submitted();

        const int timeout_ms = static_cast<int>((nominal_frame_ns * 5u) / 1000000u) + 100;
        const bool readable = TRY(wait_readable(ctx.device.fd(), timeout_ms));
        if (! readable) {
            if (g_should_stop != 0) {
                break;
            }
            LOG_WARN("no page flip event within {} ms; {} submission(s) in flight", timeout_ms,
                     ctx.chain.in_flight());
            continue;
        }

        const size_t handled = TRY(read_events(
            ctx.device.fd(), ctx.device.caps().timestamp_monotonic, [&](const FlipEvent& event) {
                ctx.chain.on_flip_complete();
                frame_stats.record(event);
            }));
        if (handled == 0) {
            continue;
        }
        ++frame;

        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const auto elapsed_ns =
            static_cast<uint64_t>(now.tv_sec - last_report_time.tv_sec) * 1000000000ULL +
            static_cast<uint64_t>(now.tv_nsec) - static_cast<uint64_t>(last_report_time.tv_nsec);
        if (elapsed_ns >= 1000000000ULL) {
            const IoctlStats current = stats();
            const IoctlStats delta = IoctlStats::delta(current, last_report);

            LOG_INFO("{}", frame_stats.to_line());
            LOG_INFO("  last second: {} frames, ioctls: {}", frame - last_report_frame,
                     delta.to_line());

            // 验收标准第 5 条，靠计数器而不是靠自觉。
            // 每帧重新 export + import + addfb2 是能跑的 —— 画面正常，
            // 只是白白多三次 ioctl 和一次内核侧的 fb 对象创建。
            // 不主动检查的话没人会发现。
            const uint64_t rebinds =
                delta.add_fb + delta.prime_fd_to_handle + delta.prime_handle_to_fd;
            if (rebinds != 0u) {
                LOG_ERROR("  {} buffer re-binding ioctl(s) in the steady state "
                          "(add_fb={} fd_to_handle={} handle_to_fd={}); every buffer should "
                          "have been imported and registered exactly once at startup",
                          rebinds, delta.add_fb, delta.prime_fd_to_handle,
                          delta.prime_handle_to_fd);
            }

            last_report = current;
            last_report_frame = frame;
            last_report_time = now;
        }

        check_sealed("frame loop");
    }

    LOG_INFO("frame loop finished: {}", frame_stats.to_line());
    if (ctx.chain.in_flight() > 0) {
        LOG_INFO("{} submission(s) still in flight at exit; their page flip events were never "
                 "read, so commit count will exceed flip count by that much",
                 ctx.chain.in_flight());
    }
    return Ok();
}

// ---------------------------------------------------------------------------
// 组装
// ---------------------------------------------------------------------------

Result<Device> open_device(const Options& options) {
    if (options.device_path != nullptr) {
        return Device::open(std::string(options.device_path));
    }
    if (options.driver_name != nullptr) {
        return Device::open_by_driver(std::string(options.driver_name));
    }
    return Device::open_first_kms();
}

/**
 * @brief 挑一个能承载 GBM / EGL 的节点
 *
 * `-g` 给了就用它，不做二次判断 —— 显式指定的意思就是"我知道我在干什么"。
 *
 * 没给就实测：对每个候选节点真的建一次 GBM + EGL + 渲染目标，取排名最高的。
 * **不用 `drm::find_render_node()` 的配对结果**，理由见 render/gl_node.hpp：
 * 那条元数据关系回答的是"同一个物理设备的 render node 是哪个"，不是
 * "哪个节点跑得起 GL"，而在多节点拓扑下二者会分叉，且分叉时不会报错 ——
 * GL 栈会静默退到软件光栅化，看起来一切正常。
 */
Result<std::string> pick_gl_node(const Device& kms, const Options& options) {
    if (options.gbm_node != nullptr) {
        return Ok(std::string(options.gbm_node));
    }

    // 探测不是免费的：每个候选都要把一套用户态驱动加载进来跑一遍，
    // 而且遇到会 oops 内核的驱动时每跑一次就多脏一点。
    LOG_INFO("probing every node to find a GL host; pass -g <node> to skip this");
    render::GlNodeProbe probe;
    probe.kms_path = kms.path();
    probe.skip = options.skip_nodes;
    const std::vector<render::GlNode> nodes = render::probe_gl_nodes(probe);

    LOG_INFO("GL host candidates:");
    for (const auto& node : nodes) {
        LOG_INFO("  {}", node.to_line());
    }

    const render::GlNode* best =
        render::best_gl_node(span<const render::GlNode>(nodes.data(), nodes.size()));
    if (best == nullptr) {
        return Err(Errc::Unsupported, "no node could host a GBM device and an EGL context");
    }
    if (best->looks_like_software()) {
        // 静默用软件光栅化是最糟的结果：画面对、帧率低得离谱，而你会去
        // 怀疑 KMS 提交路径。
        LOG_WARN("the best available GL host on this system looks like a software "
                 "rasteriser ('{}'); expect correct pixels at a tiny fraction of the frame "
                 "rate, and no hardware-usable allocations",
                 best->gl_renderer);
    }
    if (! best->renders_into_imported) {
        LOG_WARN("{} cannot use an externally allocated buffer as a render target; "
                 "--draw gl will fail there",
                 best->path);
    }
    LOG_INFO("using {} as the GL host (override with -g)", best->path);
    return Ok(best->path);
}

int run(const Options& options) {
    auto device_result = open_device(options);
    if (! device_result) {
        log_error_object(device_result.error(), "cannot open a KMS device");
        return 1;
    }
    Device device = std::move(device_result).value();

    auto path_result = device.pick_output();
    if (! path_result) {
        log_error_object(path_result.error(), "cannot pick an output");
        return 1;
    }
    const OutputPath path = std::move(path_result).value();
    LOG_INFO("{}", path.to_string());

    const Plane* primary = device.plane(path.primary_plane);
    if (primary == nullptr) {
        LOG_ERROR("the chosen primary plane is not in the snapshot");
        return 1;
    }

    const Format format{DRM_FORMAT_XRGB8888};
    if (! primary->supports_format(format)) {
        LOG_ERROR("the primary plane does not advertise {}", to_string(format));
        return 1;
    }

    std::vector<Modifier> modifiers;
    if (options.no_modifiers) {
        LOG_INFO("--no-modifiers: pretending the driver exposes no IN_FORMATS");
    } else {
        modifiers = plane_modifiers(*primary, format);
        LOG_INFO("the primary plane advertises {} modifier(s) for {}", modifiers.size(),
                 to_string(format));
        for (const Modifier modifier : modifiers) {
            LOG_DEBUG("  candidate {}", to_string(modifier));
        }
    }

    // 渲染宿主节点只探一次，而且必须在本进程碰 EGL 之前 ——
    // probe_gl_nodes() 会 fork，而 fork 一个已经带着 GL 上下文的进程，
    // 子进程里的驱动状态是未定义的。
    std::string gl_node_path;
    const bool need_gl_node =
        options.draw == DrawKind::Gl || options.source == render::SourceKind::RenderDevice;
    if (need_gl_node) {
        auto node_result = pick_gl_node(device, options);
        if (! node_result) {
            log_error_object(node_result.error(), "cannot find a node to render on");
            return 1;
        }
        gl_node_path = std::move(node_result).value();
    }

    // ---- 声明顺序即析构逆序，这里必须小心 ----
    // swapchain 里的 GL 对象归 EGL 上下文所有，fb 归 KMS device 所有，
    // imported handle 归 cache 所有。所以顺序是：
    //   device -> cache -> gbm -> egl -> source -> swapchain
    // 反过来会在退出时对着已销毁的上下文删 GL 对象。
    HandleCache cache(device.fd());

    gbm::Device gbm_device;
    egl::Display display;
    GlScene scene;
    const bool need_gl = options.draw == DrawKind::Gl;

    if (need_gl) {
        auto gbm_result = gbm::Device::open(gl_node_path);
        if (! gbm_result) {
            log_error_object(gbm_result.error(), "cannot create a GBM device for EGL");
            return 1;
        }
        gbm_device = std::move(gbm_result).value();

        auto display_result = egl::Display::create(gbm_device);
        if (! display_result) {
            log_error_object(display_result.error(), "cannot bring up EGL");
            return 1;
        }
        display = std::move(display_result).value();

        if (! display.caps().can_render_into_imported_image()) {
            LOG_ERROR("this GL implementation cannot render into an imported dmabuf; "
                      "use --draw cpu, or find out why the GL stack fell back");
            LOG_ERROR("{}", display.caps().to_string());
            return 1;
        }
        if (auto status = scene.init(); ! status) {
            log_error_object(status.error(), "cannot build the GLES scene");
            return 1;
        }
    }

    // 分配来源。渲染侧分配需要一个 GBM 设备的节点路径 —— 这里传路径而不是
    // 复用上面那个 gbm_device，是因为"渲染的设备"和"分配的设备"在设计上
    // 就是解耦的，让它们各自打开自己的节点更能暴露耦合。
    const std::string alloc_node =
        options.source == render::SourceKind::RenderDevice ? gl_node_path : std::string();

    auto source_result =
        options.source == render::SourceKind::ScanoutDevice
            ? render::make_scanout_device_source(device.fd(), cache)
            : render::make_render_device_source(device.fd(), alloc_node, cache);
    if (! source_result) {
        log_error_object(source_result.error(), "cannot build the buffer source");
        return 1;
    }
    std::unique_ptr<render::BufferSource> source = std::move(source_result).value();
    LOG_INFO("allocating with the {}", source->describe());

    render::SwapchainDesc desc;
    desc.size = path.size();
    desc.format = format;
    desc.count = options.buffer_count;
    desc.modifiers = span<const Modifier>(modifiers.data(), modifiers.size());
    desc.need_cpu_write = ! need_gl;

    auto chain_result = need_gl ? render::Swapchain::create_with_targets(*source, display, desc)
                                : render::Swapchain::create(*source, desc);
    if (! chain_result) {
        log_error_object(chain_result.error(), "cannot build the swapchain");
        return 1;
    }
    render::Swapchain chain = std::move(chain_result).value();

    // master 必须在 modeset 之前拿到。上面所有步骤（枚举、分配、addfb2、
    // EGL）都不需要它 —— 这一点本身值得知道：buffer 的准备与显示的控制权
    // 是两件独立的事。
    auto master_result = device.acquire_master();
    if (! master_result) {
        log_error_object(master_result.error(), "cannot become DRM master");
        return 1;
    }
    const MasterGuard master = std::move(master_result).value();

    auto blob_result = PropertyBlob::create(device.fd(), &path.mode.raw, sizeof(path.mode.raw));
    if (! blob_result) {
        log_error_object(blob_result.error(), "cannot create the MODE_ID blob");
        return 1;
    }
    const PropertyBlob mode_blob = std::move(blob_result).value();

    AtomicRequest request(device.fd());
    const FrameContext ctx{device,  path, request, chain, need_gl ? &display : nullptr,
                           need_gl ? &scene : nullptr, options};

    // 第一帧先画好再 modeset，否则屏幕会闪一下未初始化的显存内容。
    if (auto status = draw_into(ctx, chain.acquire(), 0); ! status) {
        log_error_object(status.error(), "first frame");
        return 1;
    }

    // 自检放在 modeset 之前：这一帧的像素已经画完，但还没有任何东西
    // 依赖它上屏。此时通道之间的分歧是纯粹的 buffer 问题，和 KMS 无关。
    // 失败不退出 —— 让画面照常上屏，屏幕上看到的和表格对照才有意义。
    if (options.verify && ! verify_first_frame(chain.acquire(), options.draw == DrawKind::Cpu)) {
        LOG_WARN("--verify failed; going on screen anyway so the picture can be compared "
                 "against the table above");
    }

    if (auto status = do_modeset(device, path, request, mode_blob, chain.acquire().buffer.fb_id());
        ! status) {
        log_error_object(status.error(), "modeset");
        return 1;
    }

    if (options.dry_run) {
        LOG_INFO("--dry-run: the modeset passed TEST_ONLY, committing nothing");
        return 0;
    }

    if (auto status = request.commit(CommitFlags::AllowModeset); ! status) {
        log_error_object(status.error(), "modeset commit");
        return 1;
    }
    LOG_INFO("modeset committed; the display should be on");
    // modeset 这次提交没带 PAGE_FLIP_EVENT，不会有完成事件回来。
    chain.mark_submitted(/*expects_event=*/false);

    // 初始化到此为止。之后每帧再出现 get_properties / addfb / prime
    // 就是热路径越界。
    seal_init_phase();

    int exit_code = 0;
    if (auto status = run_frame_loop(ctx); ! status) {
        log_error_object(status.error(), "frame loop");
        exit_code = 1;
    }

    teardown(device, path, request);
    LOG_INFO("releasing resources");
    return exit_code;
}

bool parse_source(const char* text, render::SourceKind& out) {
    if (std::strcmp(text, "scanout") == 0) {
        out = render::SourceKind::ScanoutDevice;
        return true;
    }
    if (std::strcmp(text, "render") == 0) {
        out = render::SourceKind::RenderDevice;
        return true;
    }
    return false;
}

bool parse_draw(const char* text, DrawKind& out) {
    if (std::strcmp(text, "cpu") == 0) {
        out = DrawKind::Cpu;
        return true;
    }
    if (std::strcmp(text, "gl") == 0) {
        out = DrawKind::Gl;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "--dry-run") == 0) {
            options.dry_run = true;
            continue;
        }
        if (std::strcmp(arg, "--no-modifiers") == 0) {
            options.no_modifiers = true;
            continue;
        }
        if (std::strcmp(arg, "--verify") == 0) {
            options.verify = true;
            continue;
        }
        if (std::strcmp(arg, "--unmap-each-frame") == 0) {
            options.unmap_each_frame = true;
            continue;
        }
        if (std::strcmp(arg, "-d") == 0 && i + 1 < argc) {
            options.driver_name = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-D") == 0 && i + 1 < argc) {
            options.device_path = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-x") == 0 && i + 1 < argc) {
            options.skip_nodes.emplace_back(argv[++i]);
            continue;
        }
        if (std::strcmp(arg, "-g") == 0 && i + 1 < argc) {
            options.gbm_node = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-s") == 0 && i + 1 < argc) {
            if (! parse_source(argv[++i], options.source)) {
                LOG_ERROR("unknown allocation source '{}'; expected scanout or render", argv[i]);
                return 1;
            }
            continue;
        }
        if (std::strcmp(arg, "--draw") == 0 && i + 1 < argc) {
            if (! parse_draw(argv[++i], options.draw)) {
                LOG_ERROR("unknown draw mode '{}'; expected cpu or gl", argv[i]);
                return 1;
            }
            continue;
        }
        if (std::strcmp(arg, "-f") == 0 && i + 1 < argc) {
            options.frame_limit = std::strtoull(argv[++i], nullptr, 10);
            continue;
        }
        if (std::strcmp(arg, "-b") == 0 && i + 1 < argc) {
            options.buffer_count = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
            continue;
        }
        LOG_ERROR("unknown or incomplete option '{}'", arg);
        print_usage(argv[0]);
        return 1;
    }

    install_signal_handlers();

    const int exit_code = run(options);

    // run() 已经返回，它的局部对象全部析构完了。这时候 create 与 destroy
    // 的计数才应该配平 —— 不平就是真泄漏。
    report_leaks_on_exit();
    return exit_code;
}