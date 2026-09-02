/**
 * demos/step1_kms_atomic_dumb -- Step 1 的验收程序
 *
 * CPU 在 dumb buffer 上画动画，纯 atomic commit 上屏。
 * 全程不出现任何 legacy KMS 调用。
 *
 * ## 需要 DRM master
 *
 * 和 probe_kms 不同，这个程序会接管显示输出，必须是 DRM master：
 *
 *   sudo systemctl stop lightdm     # 或者 Ctrl+Alt+F3 切到裸 tty
 *   sudo ./build/debug/bin/step1_kms_atomic_dumb -d vkms
 *
 * ## 用法
 *
 *   -d <name>    按 DRM driver name 打开（vkms / vsdrm）
 *   -D <path>    指定节点
 *   -f <n>       跑 n 帧后退出（默认无限，Ctrl+C 停）
 *   -b <n>       缓冲数，2 或 3（默认 2）
 *   --dry-run    只做到 modeset 的 TEST_ONLY，不真的提交
 *   -h
 *
 * ## 一帧发生了什么
 *
 *   1. acquire()  从缓冲链里取一个当前没在扫描的 buffer
 *   2. draw()     CPU 写像素（write-combining 内存，只顺序写不读）
 *   3. reset()    复用同一个 atomic request，不重新分配
 *   4. set_plane_fb + PAGE_FLIP_EVENT | NONBLOCK 提交
 *   5. poll + read_events 等完成事件，拿到 vblank 时间戳和 sequence
 *
 * 稳态下每帧的 ioctl 应该恰好是：1 次 atomic_commit + 1 次 read。
 * 程序退出时会打印计数，多出来的都是 bug。
 *
 * ## 退出时做什么
 *
 * 显式关掉 CRTC 上的所有 plane，再关 CRTC，最后才释放 fb 和 buffer。
 * 顺序反了内核会拒绝销毁还被引用的对象。
 */
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <drm_fourcc.h>

#include "mw/core/log.hpp"
#include "mw/drm/atomic.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/dumb_buffer.hpp"
#include "mw/drm/event.hpp"
#include "mw/drm/trace.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

// 信号处理只允许碰 volatile sig_atomic_t。
volatile std::sig_atomic_t g_should_stop = 0;

void on_signal(int /*signum*/) {
    g_should_stop = 1;
}

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    // 不设 SA_RESTART：我们希望 poll() 被信号打断后返回，
    // 好让主循环看到停止标志。
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

/**
 * @brief 画一帧
 *
 * 图案设计成能用肉眼判断三件事：
 *   - 彩条：颜色顺序对不对（格式 / 字节序搞错了会串色）
 *   - 竖直亮条水平移动：撕裂会让它在某一行断开错位
 *   - 水平亮带垂直移动：能看出垂直方向的更新是否连续
 *
 * 性能：dumb buffer 通常是 write-combining 内存，顺序宽写快、随机读极慢。
 * 所以先在普通内存里拼好一整行，再 memcpy 到每一行 —— 只有一次
 * 顺序写穿过 WC 内存。1080p 每帧 8 MiB，弱板子上几毫秒。
 */
void draw_frame(DumbBuffer& buffer, uint64_t frame) {
    const Size size = buffer.size();
    if (size.empty()) {
        return;
    }

    // XR24 在小端机器上的内存布局是 B,G,R,X，写成 uint32_t 就是 0x00RRGGBB。
    static const uint32_t kBars[] = {
        0x00ffffffu,  // white
        0x00ffff00u,  // yellow
        0x0000ffffu,  // cyan
        0x0000ff00u,  // green
        0x00ff00ffu,  // magenta
        0x00ff0000u,  // red
        0x000000ffu,  // blue
        0x00000000u,  // black
    };
    constexpr uint32_t kBarCount = sizeof(kBars) / sizeof(kBars[0]);

    // 每帧两个行模板：普通行和高亮带所在的行
    static std::vector<uint32_t> normal_row;
    static std::vector<uint32_t> band_row;
    normal_row.resize(size.width);
    band_row.resize(size.width);

    const uint32_t bar_width = size.width / kBarCount;
    // 竖直亮条：每帧右移 4 像素
    const uint32_t marker_x = static_cast<uint32_t>((frame * 4u) % size.width);
    constexpr uint32_t kMarkerWidth = 8u;

    for (uint32_t x = 0; x < size.width; ++x) {
        const uint32_t bar = bar_width > 0u ? (x / bar_width) % kBarCount : 0u;
        uint32_t color = kBars[bar];

        const bool in_marker =
            (x >= marker_x && x < marker_x + kMarkerWidth) ||
            // 环绕
            (marker_x + kMarkerWidth > size.width && x < (marker_x + kMarkerWidth) - size.width);
        if (in_marker) {
            color = 0x00ff8000u;  // orange
        }

        normal_row[x] = color;
        // 高亮带里把颜色反相，这样它经过任何彩条都看得见
        band_row[x] = ~color & 0x00ffffffu;
    }

    // 水平亮带：每帧下移 3 行
    const uint32_t band_y = static_cast<uint32_t>((frame * 3u) % size.height);
    constexpr uint32_t kBandHeight = 6u;

    const size_t row_bytes = static_cast<size_t>(size.width) * 4u;
    for (uint32_t y = 0; y < size.height; ++y) {
        const bool in_band = (y >= band_y && y < band_y + kBandHeight) ||
                             (band_y + kBandHeight > size.height &&
                              y < (band_y + kBandHeight) - size.height);
        const uint32_t* source = in_band ? band_row.data() : normal_row.data();
        std::memcpy(buffer.row(y), source, row_bytes);
    }
}

// ---------------------------------------------------------------------------
// modeset
// ---------------------------------------------------------------------------

struct Options {
    const char* driver_name = nullptr;
    const char* device_path = nullptr;
    uint64_t frame_limit = 0;  ///< 0 = 无限
    uint32_t buffer_count = 2;
    bool dry_run = false;
};

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  -d <name>   open by DRM driver name (vkms, vsdrm, ...)\n");
    std::printf("  -D <path>   open a specific node, e.g. /dev/dri/card2\n");
    std::printf("  -f <n>      stop after n frames (default: run until Ctrl+C)\n");
    std::printf("  -b <n>      number of buffers, 2 or 3 (default 2)\n");
    std::printf("  --dry-run   stop after the modeset TEST_ONLY, commit nothing\n");
    std::printf("  -h          this help\n");
    std::printf("\nthis program needs DRM master:\n");
    std::printf("  sudo systemctl stop lightdm    # or switch to a bare tty\n");
    std::printf("\nenvironment: MW_LOG=error|warn|info|debug|trace, MW_LOG_TIME=1\n");
}

/**
 * @brief 无条件完整 modeset
 *
 * 不读当前状态、不做增量、不假设 CRTC 是干净的。理由见设计文档的
 * "防御性启动流程"：接管一个别人（X11、上一个合成器、bootloader）
 * 留下的 KMS 状态时，增量修改的行为是不可预测的。
 *
 * 先 TEST_ONLY 试探。失败时自动做属性级定位，然后带诊断信息退出 ——
 * 这正是排查 atomic commit 被拒的主要手段。
 */
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

    // 改 mode 必须带 ALLOW_MODESET，否则内核认为你想在不允许 modeset 的
    // 情况下改 mode，直接 EINVAL。
    const int test_result = request.test(CommitFlags::AllowModeset);
    if (test_result != 0) {
        LOG_ERROR("modeset TEST_ONLY rejected with {}", errno_name(test_result));
        request.bisect_rejection(CommitFlags::AllowModeset);
        return Err(Errc::AtomicTestFailed,
                   fmt("modeset TEST_ONLY failed with {}", errno_name(test_result)));
    }
    LOG_INFO("modeset TEST_ONLY passed");
    return Ok();
}

/// 退出清理：先关 plane，再关 CRTC，再解绑 connector。一次原子提交完成。
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

    // 关掉这个 CRTC 上的所有 plane，不只是我们用的那个。别的 plane 可能
    // 是 X11 留下的（比如硬件光标），不关掉会挡着后面的进程。
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
// 主循环
// ---------------------------------------------------------------------------

Status run_frame_loop(const Device& device, const OutputPath& path, AtomicRequest& request,
                      DumbBufferChain& chain, const Options& options) {
    const Plane* plane = device.plane(path.primary_plane);
    if (plane == nullptr) {
        return Err(Errc::StaleSnapshot, "primary plane vanished");
    }

    const SrcRect src = SrcRect::whole(path.size());
    const CrtcRect dst = CrtcRect::at_origin(path.size());
    const uint64_t nominal_frame_ns = path.mode.frame_duration_ns();

    FrameStats frame_stats;
    IoctlStats last_report = stats();
    uint64_t frame = 0;
    uint64_t last_report_frame = 0;
    timespec last_report_time{};
    clock_gettime(CLOCK_MONOTONIC, &last_report_time);

    LOG_INFO("entering the frame loop; press Ctrl+C to stop");
    LOG_INFO("nominal frame duration is {} us ({})", nominal_frame_ns / 1000u, path.mode.name());

    while (g_should_stop == 0) {
        if (options.frame_limit != 0 && frame >= options.frame_limit) {
            break;
        }

        // ---- 1. 取一个当前没在扫描的 buffer ----
        DumbBuffer& buffer = chain.acquire();

        // ---- 2. CPU 画 ----
        draw_frame(buffer, frame);

        // ---- 3. 复用同一个 request，不重新分配 ----
        request.reset();
        TRY(request.set_plane(*plane, buffer.fb_id(), path.crtc, src, dst));

        // ---- 4. 提交 ----
        // NONBLOCK：不等 vblank 就返回，我们自己去 poll 完成事件。
        // PAGE_FLIP_EVENT：让内核在翻页完成时投递一个事件。
        // 不带 ALLOW_MODESET：稳态下不应该发生 modeset，带上反而会掩盖
        // "配置意外变了"这种问题。
        const auto commit_status =
            request.commit(CommitFlags::Nonblock | CommitFlags::PageFlipEvent, frame);
        if (! commit_status) {
            log_error_object(commit_status.error(), "frame commit");
            // 提交失败时做一次属性级定位，比只看 errno 有用得多
            request.bisect_rejection(CommitFlags::None);
            return unexpected<Error>(commit_status.error());
        }
        chain.mark_submitted();

        // ---- 5. 等完成事件 ----
        // 超时给 5 个标称帧时长：正常情况下远用不到，超了说明内核那边
        // 卡住了（或者我们提交的配置根本没让 CRTC 动起来）。
        const int timeout_ms = static_cast<int>((nominal_frame_ns * 5u) / 1000000u) + 100;
        const bool readable = TRY(wait_readable(device.fd(), timeout_ms));
        if (! readable) {
            if (g_should_stop != 0) {
                break;
            }
            LOG_WARN("no page flip event within {} ms; {} submissions in flight", timeout_ms,
                     chain.in_flight());
            continue;
        }

        const size_t handled = TRY(read_events(
            device.fd(), device.caps().timestamp_monotonic, [&](const FlipEvent& event) {
                chain.on_flip_complete();
                frame_stats.record(event);
            }));
        if (handled == 0) {
            continue;
        }

        ++frame;

        // ---- 每秒报告一次 ----
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const auto elapsed_ns =
            static_cast<uint64_t>(now.tv_sec - last_report_time.tv_sec) * 1000000000ULL +
            static_cast<uint64_t>(now.tv_nsec) - static_cast<uint64_t>(last_report_time.tv_nsec);
        if (elapsed_ns >= 1000000000ULL) {
            const IoctlStats current = stats();
            const IoctlStats delta = IoctlStats::delta(current, last_report);

            LOG_INFO("{}", frame_stats.to_line());
            // 这一行是"热路径零 ioctl"的运行时证据：
            // 稳态下 commit 数应该等于帧数，getprops / getres 必须是 0。
            LOG_INFO("  last second: {} frames, ioctls: {}", frame - last_report_frame,
                     delta.to_line());

            last_report = current;
            last_report_frame = frame;
            last_report_time = now;
        }

        check_sealed("frame loop");
    }

    LOG_INFO("frame loop finished: {}", frame_stats.to_line());

    // Ctrl+C 可能打断在"已提交但还没读到完成事件"的时刻。
    // 显式说出来，否则退出时 atomic_commit 比 page_flip_events 多几次
    // 会看着像 bug。
    if (chain.in_flight() > 0) {
        LOG_INFO("{} submission(s) still in flight at exit; their page flip events were "
                 "never read, so commit count will exceed flip count by that much",
                 chain.in_flight());
    }
    return Ok();
}

Result<Device> open_device(const Options& options) {
    if (options.device_path != nullptr) {
        return Device::open(std::string(options.device_path));
    }
    if (options.driver_name != nullptr) {
        return Device::open_by_driver(std::string(options.driver_name));
    }
    return Device::open_first_kms();
}

int run(const Options& options) {
    auto device_result = open_device(options);
    if (! device_result) {
        log_error_object(device_result.error(), "cannot open a KMS device");
        return 1;
    }
    Device device = std::move(device_result).value();

    if (! device.caps().dumb_buffer) {
        LOG_ERROR("this device has no dumb buffer support; Step 1 draws with the CPU and "
                  "cannot run here. Step 2 (GBM) will not need it.");
        return 1;
    }

    // master 必须在 modeset 之前拿到。枚举不需要它，所以上面那些都能在
    // X11 跑着的时候完成 —— 只有这一步会失败。
    auto master_result = device.acquire_master();
    if (! master_result) {
        log_error_object(master_result.error(), "cannot become DRM master");
        return 1;
    }
    const MasterGuard master = std::move(master_result).value();

    auto path_result = device.pick_output();
    if (! path_result) {
        log_error_object(path_result.error(), "cannot pick an output");
        return 1;
    }
    const OutputPath path = std::move(path_result).value();
    LOG_INFO("{}", path.to_string());

    // MODE_ID blob。**必须活到 commit 返回之后** —— 内核是在 commit 里
    // 才去取 blob 内容的。所以它在这里而不是 do_modeset 的局部变量里。
    auto blob_result =
        PropertyBlob::create(device.fd(), &path.mode.raw, sizeof(path.mode.raw));
    if (! blob_result) {
        log_error_object(blob_result.error(), "cannot create the MODE_ID blob");
        return 1;
    }
    const PropertyBlob mode_blob = std::move(blob_result).value();

    auto chain_result = DumbBufferChain::create(device.fd(), path.size(),
                                                Format{DRM_FORMAT_XRGB8888}, options.buffer_count);
    if (! chain_result) {
        log_error_object(chain_result.error(), "cannot allocate dumb buffers");
        return 1;
    }
    DumbBufferChain chain = std::move(chain_result).value();

    // 第一帧先画好再 modeset，否则屏幕会闪一下未初始化的显存内容。
    draw_frame(chain.acquire(), 0);

    AtomicRequest request(device.fd());

    if (auto status = do_modeset(device, path, request, mode_blob, chain.acquire().fb_id());
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
        LOG_ERROR("TEST_ONLY passed but the real commit failed. That combination usually means");
        LOG_ERROR("a driver-side state problem rather than an invalid configuration.");
        return 1;
    }
    LOG_INFO("modeset committed; the display should be on");
    LOG_INFO("from here on, atomic_commit should equal: 1 modeset + N frames + 1 teardown");
    // modeset 这次提交没带 PAGE_FLIP_EVENT，不会有完成事件回来。
    chain.mark_submitted(/*expects_event=*/false);

    // 初始化阶段到此为止。之后每帧再出现 get_properties / get_resources
    // 就是热路径越界，check_sealed() 会在循环里抓出来。
    seal_init_phase();

    int exit_code = 0;
    if (auto status = run_frame_loop(device, path, request, chain, options); ! status) {
        log_error_object(status.error(), "frame loop");
        exit_code = 1;
    }

    teardown(device, path, request);

    // 这里**不能**统计资源配平：request / chain / mode_blob / master / device
    // 都还是活着的局部变量，它们的析构函数（也就是 rmfb / destroy_dumb /
    // destroy_blob / dropmaster）要等 run() 返回才执行。
    // 在这里打会把"还没轮到析构"误报成"泄漏"。
    // 统计放在 main() 里，等这个函数完整返回之后。
    LOG_INFO("releasing resources");
    return exit_code;
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
        if (std::strcmp(arg, "-d") == 0 && i + 1 < argc) {
            options.driver_name = argv[++i];
            continue;
        }
        if (std::strcmp(arg, "-D") == 0 && i + 1 < argc) {
            options.device_path = argv[++i];
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

    // run() 已经返回，它的所有局部对象（framebuffer / dumb buffer /
    // property blob / master guard / device fd）都析构完了。
    // 这时候 create 与 destroy 的计数才应该配平 —— 不平就是真泄漏。
    report_leaks_on_exit();
    return exit_code;
}