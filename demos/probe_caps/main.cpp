/**
 * demos/probe_caps -- 能力闸门：当前环境能支撑到哪一步
 *
 *   probe_caps              # 全部闸门
 *   probe_caps -s 6         # 只看某一步
 *   probe_caps -q           # 只打结论，不打细节（脚本里用）
 *
 * ## 存在的理由
 *
 * 本项目要在多种平台上跑，而**平台的能力会随时间解锁** —— 内核升级、
 * 驱动补齐功能、Mesa 更新、编译配置改变。今天走不通的路径明天可能就通了。
 *
 * 所以需要一个能反复跑的东西，回答一个具体问题：
 * **"现在这台机器能支撑到第几步？被什么挡住？"**
 *
 * ## 判据只有两种
 *
 *   1. 查询能回答的（扩展字符串、drmGetCap、property 是否存在）
 *   2. 查询回答不了、只能真做一次的（跨设备导入、modifier 分配、addfb2）
 *
 * 第二类占多数，因为图形栈里绝大多数"能不能"都不是一个标志位。
 * 所以本 demo **会真的分配 buffer、真的建 fb**，但不做 modeset、
 * 不取 master，跑着桌面也能用。
 *
 * ## 退出码
 *
 *   0  所有被检查的闸门都通过
 *   1  用法错误 / 没有可用设备
 *   2  有闸门未通过（正常情况，不是故障）
 */
#include <drm_fourcc.h>
#include <xf86drm.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mw/core/log.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/prime.hpp"
#include "mw/render/buffer_source.hpp"
#include "mw/render/gl_node.hpp"
#include "mw/render/target.hpp"

using namespace mw;

namespace {

enum class Verdict {
    Pass,     ///< 可用
    Blocked,  ///< 不可用，且这一步依赖它
    Degraded, ///< 不可用，但有降级路径
    Skipped,  ///< 前置条件不满足，没测
};

const char* symbol(Verdict verdict) {
    switch (verdict) {
    case Verdict::Pass: return "PASS";
    case Verdict::Blocked: return "BLOCK";
    case Verdict::Degraded: return "DEGRD";
    case Verdict::Skipped: return "SKIP";
    }
    return "?????";
}

struct Gate {
    int step = 0;
    std::string name{};
    Verdict verdict = Verdict::Skipped;
    std::string detail{};
    /// 未通过时该怎么办。空表示无从下手。
    std::string action{};
};

class Report {
  public:
    void add(int step, std::string name, Verdict verdict, std::string detail,
             std::string action = {}) {
        gates_.push_back(Gate{step, std::move(name), verdict, std::move(detail),
                              std::move(action)});
    }

    void print(bool quiet) const {
        int last_step = -1;
        for (const auto& gate : gates_) {
            if (gate.step != last_step) {
                LOG_INFO("");
                LOG_INFO("--- step {} ---", gate.step);
                last_step = gate.step;
            }
            LOG_INFO("  {}  {}", symbol(gate.verdict), gate.name);
            if (! quiet && ! gate.detail.empty()) {
                LOG_INFO("         {}", gate.detail);
            }
            if (! gate.action.empty() &&
                (gate.verdict == Verdict::Blocked || gate.verdict == Verdict::Degraded)) {
                LOG_INFO("         -> {}", gate.action);
            }
        }
    }

    /// 全部闸门都过的最高步骤
    int highest_clear_step() const {
        int highest = 0;
        for (int step = 1; step <= 7; ++step) {
            bool any = false;
            bool all_ok = true;
            for (const auto& gate : gates_) {
                if (gate.step != step) {
                    continue;
                }
                any = true;
                all_ok = all_ok && gate.verdict != Verdict::Blocked;
            }
            if (! any) {
                break;
            }
            if (! all_ok) {
                break;
            }
            highest = step;
        }
        return highest;
    }

    int degraded_count() const {
        int count = 0;
        for (const auto& gate : gates_) {
            if (gate.verdict == Verdict::Degraded) {
                ++count;
            }
        }
        return count;
    }

    bool any_blocked() const {
        for (const auto& gate : gates_) {
            if (gate.verdict == Verdict::Blocked) {
                return true;
            }
        }
        return false;
    }

  private:
    std::vector<Gate> gates_{};
};

// ---------------------------------------------------------------------------

void check_step1(Report& report, const drm::Device& device) {
    const auto& caps = device.caps();

    report.add(1, "atomic modesetting",
               caps.atomic ? Verdict::Pass : Verdict::Blocked,
               fmt("DRM_CLIENT_CAP_ATOMIC {}", caps.atomic ? "granted" : "refused"),
               "the whole project is atomic-only; a legacy-KMS driver cannot be supported");

    report.add(1, "universal planes",
               caps.universal_planes ? Verdict::Pass : Verdict::Blocked,
               fmt("DRM_CLIENT_CAP_UNIVERSAL_PLANES {}",
                   caps.universal_planes ? "granted" : "refused"));

    report.add(1, "dumb buffers", caps.dumb_buffer ? Verdict::Pass : Verdict::Blocked,
               "needed by the CPU-side allocation path");

    const auto alignment = drm::probe_pitch_alignment(device.fd());
    report.add(1, "pitch alignment probe",
               alignment.has_value() ? Verdict::Pass : Verdict::Degraded,
               alignment.has_value()
                   ? fmt("the allocator rounds row strides to {} bytes", *alignment)
                   : std::string("could not be determined"),
               "without it, stride-related addfb2 failures give no diagnostic hint");
}

void check_step2(Report& report, const drm::Device& device,
                 const render::GlNode* gl_node) {
    // ---- PRIME ----
    const auto& caps = device.caps();
    const bool prime_ok = caps.prime_import && caps.prime_export;
    report.add(2, "PRIME import + export", prime_ok ? Verdict::Pass : Verdict::Blocked,
               fmt("import={} export={}", caps.prime_import ? "yes" : "no",
                   caps.prime_export ? "yes" : "no"),
               "cross-device buffer sharing is impossible without both");

    report.add(2, "addfb2 with modifiers",
               caps.addfb2_modifiers ? Verdict::Pass : Verdict::Degraded,
               caps.addfb2_modifiers ? "the driver accepts explicit modifiers"
                                     : "only the modifier-less addfb2 path is available",
               "modifier negotiation degenerates to linear-only");

    // ---- 显示侧分配。只能真做一次。 ----
    drm::HandleCache cache(device.fd());
    auto scanout_source = render::make_scanout_device_source(device.fd(), cache);
    render::AllocRequest request;
    request.size = drm::Size{256, 256};
    request.format = drm::Format{DRM_FORMAT_XRGB8888};

    if (! scanout_source) {
        report.add(2, "scanout-device allocation path", Verdict::Blocked,
                   scanout_source.error().message,
                   "the display device cannot allocate its own scanout buffers");
    } else {
        auto buffer = scanout_source.value()->allocate(request);
        report.add(2, "scanout-device allocation path",
                   buffer ? Verdict::Pass : Verdict::Blocked,
                   buffer ? buffer.value().to_string() : buffer.error().message,
                   "the display device cannot allocate its own scanout buffers");
    }

    // ---- GL 宿主 ----
    if (gl_node == nullptr) {
        report.add(2, "GL host node", Verdict::Blocked,
                   "no node could bring up GBM + EGL",
                   "only CPU-drawn content can be shown");
        report.add(2, "buffers allocated by the display, drawn by GL", Verdict::Skipped,
                   "no GL host");
        report.add(2, "buffers allocated by the GL host, scanned by the display",
                   Verdict::Skipped, "no GL host");
        return;
    }

    const bool software = gl_node->looks_like_software();
    report.add(2, "GL host node", software ? Verdict::Degraded : Verdict::Pass,
               fmt("{} -- EGL {} renderer '{}'", gl_node->path, gl_node->egl_version,
                   gl_node->gl_renderer),
               "the GL stack fell back to a software rasteriser; it works but produces no "
               "hardware-usable allocations and is orders of magnitude slower. Check that the "
               "node actually driving the GPU has a matching user-mode driver installed");

    // ---- 两个方向，都测，都不预设哪个是主路 ----
    //
    // 走得通的方向随硬件与驱动成熟度变化。把任何一个写死进代码，就是把
    // 当下这块板子的状态当成了架构。见 render/gl_node.hpp。
    report.add(2, "buffers allocated by the display, drawn by GL",
               gl_node->renders_into_imported
                   ? (gl_node->attach_kind == render::AttachKind::Renderbuffer
                          ? Verdict::Pass
                          : Verdict::Degraded)
                   : Verdict::Blocked,
               gl_node->renders_into_imported
                   ? fmt("an imported dmabuf can be drawn into via {}",
                         render::to_string(gl_node->attach_kind))
                   : gl_node->detail,
               "the renderbuffer path is unavailable or GL cannot draw into foreign memory at "
               "all; without this direction GL output cannot reach the display");

    Verdict export_verdict = Verdict::Blocked;
    std::string export_detail;
    if (gl_node->same_device_as_kms) {
        // 分配方就是显示设备自己，收得下是恒真的。报成 PASS 会让人以为
        // 跨设备那条路通了。
        export_verdict = Verdict::Skipped;
        export_detail = "the GL host is the display device itself, so this says nothing "
                        "about cross-device import; force another node with -r to test it";
    } else if (gl_node->scanout_accepted_by_kms) {
        export_verdict = Verdict::Pass;
        export_detail = "the display device imports and scans out GL-host allocations";
    } else if (gl_node->allocates_scanout) {
        export_verdict = Verdict::Degraded;
        export_detail = "the GL host allocates scanout-capable memory but the display device "
                        "refuses to import it";
    } else {
        export_detail = "the GL host cannot allocate scanout-capable memory";
    }
    report.add(2, "buffers allocated by the GL host, scanned by the display", export_verdict,
               export_detail,
               "modifier negotiation has no effect while this direction is closed: only the "
               "display device's own linear allocations reach the screen");

    report.add(2, "EGL modifier-aware import",
               gl_node->egl_import_modifiers ? Verdict::Pass : Verdict::Degraded,
               "EGL_EXT_image_dma_buf_import_modifiers",
               "only linear buffers can be imported into GL reliably");
}

void check_step5(Report& report, const drm::Device& device) {
    size_t overlays = 0;
    size_t cursors = 0;
    for (const auto& plane : device.planes()) {
        if (plane.type == drm::PlaneType::Overlay) {
            ++overlays;
        }
        if (plane.type == drm::PlaneType::Cursor) {
            ++cursors;
        }
    }
    report.add(5, "overlay planes", overlays > 0 ? Verdict::Pass : Verdict::Degraded,
               fmt("{} overlay plane(s)", overlays),
               "multi-plane offload degenerates to GPU composition only");
    report.add(5, "cursor plane", cursors > 0 ? Verdict::Pass : Verdict::Degraded,
               fmt("{} cursor plane(s)", cursors),
               "the cursor has to be composited into the primary plane");

    // IN_FORMATS 存在与否，看有没有任何一项带真实 modifier：
    // 没有 IN_FORMATS 时回退路径会把 modifier 记成 kModifierInvalid。
    bool in_formats = false;
    for (const auto& plane : device.planes()) {
        for (const auto& pair : plane.formats) {
            in_formats = in_formats || pair.modifier != drm::kModifierInvalid;
        }
    }
    report.add(5, "IN_FORMATS blob", in_formats ? Verdict::Pass : Verdict::Degraded,
               in_formats ? "per-plane format/modifier pairs are advertised"
                          : "no IN_FORMATS; assume linear only",
               "modifier selection has nothing to choose from");
}

void check_step6(Report& report, const drm::Device& device, const render::GlNode* gl_node) {
    const auto& caps = device.caps();

    // KMS 侧的显式同步靠 sync_file，与 syncobj 无关 —— 这两件事经常被混为一谈。
    const bool kms_fences = caps.prop_plane_in_fence_fd && caps.prop_crtc_out_fence_ptr;
    report.add(6, "KMS in/out fences", kms_fences ? Verdict::Pass : Verdict::Blocked,
               fmt("IN_FENCE_FD={} OUT_FENCE_PTR={}", caps.prop_plane_in_fence_fd ? "yes" : "no",
                   caps.prop_crtc_out_fence_ptr ? "yes" : "no"),
               "the compositor would have to block on the CPU before every commit");

    if (gl_node == nullptr) {
        report.add(6, "fence export from GL", Verdict::Skipped, "no GL host");
        report.add(6, "timeline syncobj (protocol side)", Verdict::Skipped, "no GL host");
        return;
    }

    // 合成器自己的显式同步只需要这一条：把 GL 的完成点变成一个 fence fd，
    // 交给 plane 的 IN_FENCE_FD。不依赖 syncobj。
    report.add(6, "fence export from GL",
               gl_node->egl_native_fence_sync ? Verdict::Pass : Verdict::Blocked,
               fmt("EGL_ANDROID_native_fence_sync on {}", gl_node->path),
               "the compositor cannot turn GPU completion into a fence fd and would have to "
               "block on glFinish before every commit");

    // 这一条**必须问实际承载渲染的节点**。问错节点会得到一个与本项目无关的
    // 答案，而它决定的是能不能把 linux-drm-syncobj-v1 提供给客户端。
    report.add(6, "timeline syncobj (protocol side)",
               gl_node->syncobj_timeline ? Verdict::Pass : Verdict::Degraded,
               fmt("{} reports SYNCOBJ={} SYNCOBJ_TIMELINE={}", gl_node->path,
                   gl_node->syncobj ? 1 : 0, gl_node->syncobj_timeline ? 1 : 0),
               "linux-drm-syncobj-v1 cannot be offered to clients; the compositor's own "
               "explicit sync still works through sync_file, and clients fall back to "
               "implicit sync");
}

void check_step7(Report& report, const drm::Device& device) {
    const auto& caps = device.caps();
    report.add(7, "monotonic vblank timestamps",
               caps.timestamp_monotonic ? Verdict::Pass : Verdict::Blocked,
               "DRM_CAP_TIMESTAMP_MONOTONIC",
               "presentation timestamps could not be related to CLOCK_MONOTONIC");
    report.add(7, "vblank event in commit",
               caps.crtc_in_vblank_event ? Verdict::Pass : Verdict::Degraded,
               "DRM_CAP_CRTC_IN_VBLANK_EVENT",
               "the CRTC of a flip event has to be inferred rather than read");
    report.add(7, "variable refresh rate", caps.prop_crtc_vrr_enabled ? Verdict::Pass : Verdict::Degraded,
               caps.prop_crtc_vrr_enabled ? "VRR_ENABLED is present" : "no VRR_ENABLED property",
               "frame pacing is limited to fixed refresh intervals");
}

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  -d <path>   KMS device (default: first with a connected display)\n");
    std::printf("  -r <path>   force the GL host node (default: probe every node)\n");
    std::printf("  -s <n>      only check the gates for step n\n");
    std::printf("  -q          terse output\n");
    std::printf("  -x <path>   do not probe this node (repeatable; use it for a node\n");
    std::printf("              whose driver oopses the kernel)\n");
    std::printf("  --no-isolate  probe GL nodes in-process (for gdb; one bad driver\n");
    std::printf("                takes the whole tool down)\n");
    std::printf("  -h          this help\n");
    std::printf("\nallocates small buffers to test what queries cannot answer.\n");
    std::printf("does not modeset and does not take DRM master.\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string device_path;
    std::string render_node;
    int only_step = 0;
    bool quiet = false;
    bool isolate = true;
    std::vector<std::string> skip_nodes;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "-q") == 0) {
            quiet = true;
            continue;
        }
        if (std::strcmp(arg, "-x") == 0 && i + 1 < argc) {
            skip_nodes.emplace_back(argv[++i]);
            continue;
        }
        if (std::strcmp(arg, "--no-isolate") == 0) {
            isolate = false;
            continue;
        }
        if (std::strcmp(arg, "-d") == 0 && i + 1 < argc) {
            device_path = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-r") == 0 && i + 1 < argc) {
            render_node = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-s") == 0 && i + 1 < argc) {
            only_step = std::atoi(argv[++i]);
            continue;
        }
        LOG_ERROR("unknown option: {}", arg);
        print_usage(argv[0]);
        return 1;
    }

    auto device_result = device_path.empty() ? drm::Device::open_first_kms()
                                             : drm::Device::open(device_path);
    if (! device_result) {
        LOG_ERROR("no usable KMS device: {}", device_result.error().message);
        return 1;
    }
    const drm::Device device = std::move(device_result).value();

    LOG_INFO("KMS device:  {} ({})", device.path(), device.caps().driver_name);

    // 哪个节点能跑 GL 是**实测结论**，不是从设备元数据推出来的。
    // 后面 step 2 和 step 6 的好几条闸门都取决于它，问错节点会一路带偏。
    // 见 render/gl_node.hpp。
    render::GlNodeProbe gl_probe;
    gl_probe.kms_path = device.path();
    gl_probe.isolate = isolate;
    gl_probe.skip = skip_nodes;
    const std::vector<render::GlNode> gl_nodes = render::probe_gl_nodes(gl_probe);

    LOG_INFO("");
    LOG_INFO("--- GL host candidates ---");
    LOG_INFO("  each one is tried for real, in its own process");
    for (const auto& node : gl_nodes) {
        LOG_INFO("  {}", node.to_line());
        if (! node.detail.empty()) {
            LOG_INFO("        {}", node.detail);
        }
    }

    const render::GlNode* gl_node = nullptr;
    if (! render_node.empty()) {
        for (const auto& node : gl_nodes) {
            if (node.path == render_node) {
                gl_node = &node;
            }
        }
        if (gl_node == nullptr) {
            LOG_ERROR("{} is not among the probed nodes", render_node);
            return 1;
        }
        LOG_INFO("  using {} because -r said so", render_node);
    } else {
        gl_node = render::best_gl_node(
            span<const render::GlNode>(gl_nodes.data(), gl_nodes.size()));
        if (gl_node != nullptr) {
            LOG_INFO("  picked {} (highest ranked; override with -r)", gl_node->path);
        }
    }

    Report report;
    if (only_step == 0 || only_step == 1) {
        check_step1(report, device);
    }
    if (only_step == 0 || only_step == 2) {
        check_step2(report, device, gl_node);
    }
    if (only_step == 0 || only_step == 5) {
        check_step5(report, device);
    }
    if (only_step == 0 || only_step == 6) {
        check_step6(report, device, gl_node);
    }
    if (only_step == 0 || only_step == 7) {
        check_step7(report, device);
    }

    report.print(quiet);

    LOG_INFO("");
    if (only_step == 0) {
        const int highest = report.highest_clear_step();
        const int degraded = report.degraded_count();
        // "up to step N is clear" 只说明没有 BLOCK。降级项也会改变能做什么，
        // 不一起说出来这行就是误导。
        if (degraded == 0) {
            LOG_INFO("every gate up to step {} is clear", highest);
        } else {
            LOG_INFO("no gate up to step {} is blocked, but {} gate(s) are degraded -- "
                     "those steps work along a reduced path, see DEGRD above",
                     highest, degraded);
        }
        LOG_INFO("");
        LOG_INFO("BLOCK means the step cannot work at all; DEGRD means it works with a");
        LOG_INFO("reduced path. Re-run this after a kernel, driver or Mesa update -- gates");
        LOG_INFO("that are blocked today may open later.");
    }

    return report.any_blocked() ? 2 : 0;
}
