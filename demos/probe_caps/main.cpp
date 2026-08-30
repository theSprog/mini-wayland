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
#include "mw/egl/display.hpp"
#include "mw/gbm/device.hpp"
#include "mw/render/buffer_source.hpp"

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

void check_step2(Report& report, const drm::Device& device, const std::string& render_node) {
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

    // ---- 分配路径。这两个只能真做一次。 ----
    drm::HandleCache cache(device.fd());
    const auto probes = render::probe_buffer_sources(device.fd(), render_node, drm::Size{256, 256});
    for (const auto& probe : probes) {
        const bool is_render = probe.kind == render::SourceKind::RenderDevice;
        report.add(2, fmt("{} allocation path", render::to_string(probe.kind)),
                   probe.usable ? Verdict::Pass
                                : (is_render ? Verdict::Degraded : Verdict::Blocked),
                   probe.detail,
                   is_render ? "GPU-allocated scanout is unavailable; fall back to allocating "
                               "on the display device"
                             : "the display device cannot allocate its own scanout buffers");
    }

    // ---- GBM / EGL ----
    if (render_node.empty()) {
        report.add(2, "GBM device", Verdict::Skipped, "no render node found");
        report.add(2, "EGL dmabuf import", Verdict::Skipped, "no render node found");
        return;
    }

    auto gbm_device = gbm::Device::open(render_node);
    if (! gbm_device) {
        report.add(2, "GBM device", Verdict::Blocked, gbm_device.error().message,
                   "GPU rendering is unavailable; only CPU-drawn content can be shown");
        report.add(2, "EGL dmabuf import", Verdict::Skipped, "no GBM device");
        return;
    }
    report.add(2, "GBM device", Verdict::Pass,
               fmt("backend '{}' on {}", gbm_device.value().backend_name(), render_node));

    auto display = egl::Display::create(gbm_device.value());
    if (! display) {
        report.add(2, "EGL dmabuf import", Verdict::Blocked, display.error().message,
                   "GPU rendering into shareable buffers is unavailable");
        return;
    }
    const auto& egl_caps = display.value().caps();
    report.add(2, "EGL dmabuf import", Verdict::Pass,
               fmt("{} -- renderer '{}'", egl_caps.vendor, display.value().gl_renderer()));
    report.add(2, "EGL modifier-aware import",
               egl_caps.dmabuf_import_modifiers ? Verdict::Pass : Verdict::Degraded,
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

void check_step6(Report& report, const drm::Device& device, const std::string& render_node) {
    const auto& caps = device.caps();

    // KMS 侧的显式同步靠 sync_file，与 syncobj 无关 —— 这两件事经常被混为一谈。
    const bool kms_fences = caps.prop_plane_in_fence_fd && caps.prop_crtc_out_fence_ptr;
    report.add(6, "KMS in/out fences", kms_fences ? Verdict::Pass : Verdict::Blocked,
               fmt("IN_FENCE_FD={} OUT_FENCE_PTR={}", caps.prop_plane_in_fence_fd ? "yes" : "no",
                   caps.prop_crtc_out_fence_ptr ? "yes" : "no"),
               "the compositor would have to block on the CPU before every commit");

    if (render_node.empty()) {
        report.add(6, "timeline syncobj (render side)", Verdict::Skipped, "no render node");
        return;
    }
    auto fd = UniqueFd::open(render_node.c_str(), O_RDWR | O_CLOEXEC);
    if (! fd) {
        report.add(6, "timeline syncobj (render side)", Verdict::Skipped,
                   fmt("cannot open {}", render_node));
        return;
    }
    uint64_t timeline = 0;
    drmGetCap(fd.value().get(), DRM_CAP_SYNCOBJ_TIMELINE, &timeline);
    report.add(6, "timeline syncobj (render side)",
               timeline != 0 ? Verdict::Pass : Verdict::Blocked,
               fmt("{} reports SYNCOBJ_TIMELINE={}", render_node, timeline),
               "linux-drm-syncobj-v1 cannot be offered to clients");
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
    std::printf("  -r <path>   render node (default: the paired one)\n");
    std::printf("  -s <n>      only check the gates for step n\n");
    std::printf("  -q          terse output\n");
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

    if (render_node.empty()) {
        if (const auto found = drm::find_render_node(device.path())) {
            render_node = *found;
        }
    }

    LOG_INFO("KMS device:  {} ({})", device.path(), device.caps().driver_name);
    LOG_INFO("render node: {}", render_node.empty() ? "<none>" : render_node);

    Report report;
    if (only_step == 0 || only_step == 1) {
        check_step1(report, device);
    }
    if (only_step == 0 || only_step == 2) {
        check_step2(report, device, render_node);
    }
    if (only_step == 0 || only_step == 5) {
        check_step5(report, device);
    }
    if (only_step == 0 || only_step == 6) {
        check_step6(report, device, render_node);
    }
    if (only_step == 0 || only_step == 7) {
        check_step7(report, device);
    }

    report.print(quiet);

    LOG_INFO("");
    if (only_step == 0) {
        const int highest = report.highest_clear_step();
        LOG_INFO("every gate up to step {} is clear", highest);
        LOG_INFO("");
        LOG_INFO("BLOCK means the step cannot work at all; DEGRD means it works with a");
        LOG_INFO("reduced path. Re-run this after a kernel, driver or Mesa update -- gates");
        LOG_INFO("that are blocked today may open later.");
    }

    return report.any_blocked() ? 2 : 0;
}
