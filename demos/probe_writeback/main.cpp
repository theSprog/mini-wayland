/**
 * demos/probe_writeback -- KMS writeback connector 可用性探针
 *
 * ## 它回答什么
 *
 * `docs/open-questions.md` 的 Q-1 ~ Q-4。这四条决定了 Step 5 的验收方案：
 *
 *   Q-1  writeback 的 CRTC 是不是 no_vblank（这版 atomic_commit_tail 没有
 *        drm_atomic_helper_fake_vblank，若是则 flip 事件不来、内核等满 10 秒）
 *   Q-2  writeback connector 能否与显示 connector 挂同一个 CRTC
 *        （共存才能"屏幕照常亮 + 同时抓一份"，否则只能牺牲显示做离屏自检）
 *   Q-3  WRITEBACK_FB_ID / WRITEBACK_PIXEL_FORMATS 是否都在
 *        （已知 OUT_FENCE_PTR 在，另两个是**推断**，推断要落到实测）
 *   Q-4  WB_POINT 的默认抓取点（抓错点，CRC 比对的对象就不是屏幕那一帧）
 *
 * ## 为什么它是一个独立探针，而不是某个 step 的一部分
 *
 * 这四条里任何一条不成立，writeback 这条路就要重新估价。把它做成独立的
 * 一次性实验，一天之内就能知道答案；建在某个 step 的关键路径上，
 * 则要等那个 step 做到一半才发现地基不对。
 *
 * 它对本项目的价值不在 Step 3 而在 **Step 5**：plane 分配器的典型故障是
 * "某个窗口是黑的"，那一层没有任何进程内判据可以覆盖。而"层层成功、
 * 画面全黑"在这个项目里已经真的发生过两次（`docs/lessons.md` L-1、L-15）。
 *
 * ## 危险动作是显式开关
 *
 * 默认只做 TEST_ONLY，不真提交。`--commit` 才会真的下发 ——
 * 因为 Q-1 的坏结果就是**内核在 flip_done 上等满 10 秒**，
 * 那会让整机的显示卡住十秒。探针不该在默认路径上做这种事。
 *
 * ## 用法
 *
 *   sudo ./build/debug/bin/probe_writeback                  # 只枚举 + TEST_ONLY
 *   sudo ./build/debug/bin/probe_writeback --commit         # 真提交并回读
 *   sudo ./build/debug/bin/probe_writeback -D /dev/dri/card2 --commit --mode coexist
 *
 *   -D <path>        指定 KMS 节点
 *   -d <name>        按 driver name 打开
 *   --mode <kind>    coexist | headless | both（默认 both）
 *   --commit         真的提交（默认只 TEST_ONLY）
 *   --timeout <ms>   等 flip / fence 的超时，默认 2000（内核自己的超时是 10s）
 *   -h
 */
#include <poll.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mw/core/log.hpp"
#include "mw/drm/atomic.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/dumb_buffer.hpp"
#include "mw/drm/event.hpp"
#include "mw/drm/property.hpp"
#include "mw/drm/trace.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

volatile std::sig_atomic_t g_should_stop = 0;

void on_signal(int /*signum*/) {
    g_should_stop = 1;
}

enum class Mode { Coexist, Headless, Both };

struct Options {
    const char* device_path = nullptr;
    const char* driver_name = nullptr;
    Mode mode = Mode::Both;
    bool commit = false;
    int timeout_ms = 2000;
};

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  -D <path>       open a specific KMS node\n");
    std::printf("  -d <name>       open the KMS node by DRM driver name\n");
    std::printf("  --mode <kind>   coexist | headless | both (default both)\n");
    std::printf("  --commit        really commit (default: TEST_ONLY only)\n");
    std::printf("  --timeout <ms>  flip / fence timeout, default 2000\n");
    std::printf("  -h              this help\n");
    std::printf("\nwithout --commit nothing is ever put on the wire beyond TEST_ONLY.\n");
    std::printf("with --commit a bad outcome can block the display for ~10s (the kernel's\n");
    std::printf("own flip_done timeout) -- do not run it on a machine you care about.\n");
}

bool parse_options(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char** out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs an argument\n", arg.c_str());
                return false;
            }
            *out = argv[++i];
            return true;
        };
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else if (arg == "-D") {
            if (! next(&options.device_path)) return false;
        } else if (arg == "-d") {
            if (! next(&options.driver_name)) return false;
        } else if (arg == "--commit") {
            options.commit = true;
        } else if (arg == "--mode") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            if (std::strcmp(value, "coexist") == 0) {
                options.mode = Mode::Coexist;
            } else if (std::strcmp(value, "headless") == 0) {
                options.mode = Mode::Headless;
            } else if (std::strcmp(value, "both") == 0) {
                options.mode = Mode::Both;
            } else {
                std::fprintf(stderr, "unknown mode '%s'\n", value);
                return false;
            }
        } else if (arg == "--timeout") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            options.timeout_ms = static_cast<int>(std::strtol(value, nullptr, 10));
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", arg.c_str());
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

int g_findings = 0;

void finding(const char* tag, const std::string& text) {
    std::printf("  [%s] %s\n", tag, text.c_str());
    ++g_findings;
}

// ---------------------------------------------------------------------------
// Q-3 / Q-4：属性存在性与默认值
// ---------------------------------------------------------------------------

/// writeback 的三个核心属性。DRM 核心的 drm_writeback_connector_init()
/// 一次性创建这三个，所以理论上要么都在要么都不在 —— 但那是推断，这里实测。
constexpr const char* kWritebackProps[] = {
    "WRITEBACK_FB_ID",
    "WRITEBACK_PIXEL_FORMATS",
    "WRITEBACK_OUT_FENCE_PTR",
};

/// 解析 WRITEBACK_PIXEL_FORMATS blob（一串 uint32 fourcc）
std::vector<Format> read_pixel_formats(BorrowedFd fd, const Connector& conn) {
    std::vector<Format> formats;
    const auto info = conn.props.find("WRITEBACK_PIXEL_FORMATS");
    if (! info || info->value == 0) {
        return formats;
    }
    auto blob = BlobView::get(fd, BlobId{static_cast<uint32_t>(info->value)});
    if (! blob) {
        LOG_WARN("cannot read WRITEBACK_PIXEL_FORMATS blob: {}", blob.error().message);
        return formats;
    }
    // **按 blob 的实际长度读，不要按某个结构体 memcpy** —— 长度是驱动说了算的。
    const size_t count = blob.value().size() / sizeof(uint32_t);
    const auto* raw = static_cast<const uint32_t*>(blob.value().data());
    for (size_t i = 0; i < count; ++i) {
        formats.push_back(Format{raw[i]});
    }
    return formats;
}

void describe_writeback_connector(const Device& device, const Connector& conn) {
    std::printf("\n%s (%s), %s\n", to_string(conn.id).c_str(), conn.name.c_str(),
                conn.connected ? "connected" : "disconnected");

    // Q-3
    bool all_present = true;
    for (const char* name : kWritebackProps) {
        const bool present = conn.props.has(name);
        std::printf("  %-24s %s\n", name, present ? "present" : "MISSING");
        all_present = all_present && present;
    }
    if (all_present) {
        finding("Q-3", "all three writeback properties are present");
    } else {
        finding("Q-3", "at least one writeback property is missing -- "
                       "this connector cannot be driven through the standard path");
    }

    // Q-3 附带：能写进去的格式
    const std::vector<Format> formats = read_pixel_formats(device.fd(), conn);
    std::string list;
    for (const Format format : formats) {
        if (! list.empty()) {
            list += " ";
        }
        list += to_string(format);
    }
    std::printf("  pixel formats (%zu): %s\n", formats.size(),
                list.empty() ? "<none reported>" : list.c_str());

    // Q-4：厂商属性只**读**，绝不设置。它们不在任何标准里，
    // 设了就等于把一个厂商假设焊进了探针。
    for (const auto& [name, info] : conn.props.entries()) {
        if (name.rfind("WB_", 0) == 0 || name == "DATA_TRUNC" || name == "DOWN_SAMPLE" ||
            name == "R2Y" || name == "DITHER") {
            std::printf("  vendor property %-14s = %llu%s\n", name.c_str(),
                        static_cast<unsigned long long>(info.value),
                        info.immutable ? " (immutable)" : "");
        }
    }

    // 它能挂在哪些 CRTC 上
    std::string crtc_list;
    for (const EncoderId enc_id : conn.encoders) {
        const Encoder* encoder = device.encoder(enc_id);
        if (encoder == nullptr) {
            continue;
        }
        for (const Crtc& crtc : device.crtcs()) {
            if (encoder->possible_crtcs.contains(crtc.index)) {
                if (! crtc_list.empty()) {
                    crtc_list += " ";
                }
                crtc_list += to_string(crtc.id);
            }
        }
    }
    std::printf("  possible crtcs: %s\n", crtc_list.empty() ? "<none>" : crtc_list.c_str());
}

// ---------------------------------------------------------------------------
// 构造一次带 writeback 的 atomic 请求
// ---------------------------------------------------------------------------

/**
 * @brief 从 WRITEBACK_PIXEL_FORMATS 里挑一个目标格式
 *
 * **必须挑，不能假定。** 第一版这里写死了 XR24 —— 而这块硬件的
 * writeback 报的 19 个格式里根本没有 XR24（有 AR24）。结果整条链路
 * 被 ENOTSUP 拒掉，而拒绝理由看起来像"writeback 不支持"，
 * 差点把 Q-2 记成"不支持共存"。**把约束打印出来又不去用它，
 * 比没打印更糟**：屏幕上那 19 个格式就在同一次运行的上一屏。
 *
 * 偏好顺序：能被 CPU 直接读回来比对的 32 位 packed RGB 优先。
 * 回读是 writeback 唯一的用途，选一个还要自己转换的格式没有意义。
 */
const Format* pick_writeback_format(span<const Format> formats) {
    static const uint32_t kPreferred[] = {
        DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
        DRM_FORMAT_RGBA8888, DRM_FORMAT_BGRA8888,
    };
    for (const uint32_t wanted : kPreferred) {
        for (const Format& format : formats) {
            if (static_cast<uint32_t>(format) == wanted) {
                return &format;
            }
        }
    }
    return nullptr;
}

struct Attempt {
    const Connector* writeback = nullptr;
    const Connector* display = nullptr; ///< 共存模式下同挂一个 CRTC 的显示 connector
    const Crtc* crtc = nullptr;
    const Plane* plane = nullptr;
    const ModeInfo* mode = nullptr;
    Format writeback_format{DRM_FORMAT_ARGB8888}; ///< 取自 WRITEBACK_PIXEL_FORMATS
};

/**
 * @brief 把 writeback 的目标 buffer 与 out fence 挂上去
 *
 * 两件事值得记住：
 *
 * 1. `WRITEBACK_FB_ID` 是**一次性**属性。内核消费完这次 commit 就把它清了，
 *    想抓下一帧必须再设一次。这与 plane 的 `FB_ID`（粘住不变）完全不同。
 * 2. `WRITEBACK_OUT_FENCE_PTR` 的"值"是一个**用户态指针**，
 *    内核往那个地址写回一个 sync_file fd。属性值是指针这件事在 KMS 里
 *    只有这一处和 CRTC 的 OUT_FENCE_PTR，第一次见会以为写错了。
 */
Status attach_writeback(AtomicRequest& request, const Connector& writeback, CrtcId crtc,
                        FbId dst_fb, int* out_fence_fd) {
    const PropertyId crtc_prop = TRY(writeback.props.require("CRTC_ID"));
    const PropertyId fb_prop = TRY(writeback.props.require("WRITEBACK_FB_ID"));
    TRY(request.add(writeback.id, crtc_prop, static_cast<uint64_t>(raw(crtc)), "CRTC_ID"));
    TRY(request.add(writeback.id, fb_prop, static_cast<uint64_t>(raw(dst_fb)),
                    "WRITEBACK_FB_ID"));

    if (out_fence_fd != nullptr) {
        const PropertyId fence_prop = TRY(writeback.props.require("WRITEBACK_OUT_FENCE_PTR"));
        *out_fence_fd = -1;
        TRY(request.add(writeback.id, fence_prop,
                        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(out_fence_fd)),
                        "WRITEBACK_OUT_FENCE_PTR"));
    }
    return Ok();
}

Status build_request(const Device& device, AtomicRequest& request, const Attempt& attempt,
                     const PropertyBlob& mode_blob, FbId source_fb, FbId writeback_fb, Size size,
                     int* out_fence_fd) {
    request.reset();
    if (attempt.display != nullptr) {
        TRY(request.bind_connector(*attempt.display, attempt.crtc->id));
    } else {
        // **headless 必须显式解绑这个 CRTC 上的显示 connector。**
        //
        // 不解绑的话，内核状态里 HDMI 还挂在上面，而我们正把 CRTC 的 mode
        // 改成 writeback 自报的 3200x1920 —— 显示器做不到那个时序，EINVAL。
        // 而这个残留状态很可能是**我们自己上一次尝试留下的**：
        // 同一次运行里先跑了 coexist。
        //
        // 这就是"防御性完整 modeset、不假设 CRTC 状态干净"那条原则的
        // 又一个实例：atomic 提交只描述你说到的属性，没说到的**保持原样**，
        // 而"原样"未必是你以为的那样。
        for (const Connector& conn : device.connectors()) {
            if (conn.is_writeback) {
                continue;
            }
            for (const EncoderId enc_id : conn.encoders) {
                const Encoder* encoder = device.encoder(enc_id);
                if (encoder != nullptr && encoder->possible_crtcs.contains(attempt.crtc->index)) {
                    TRY(request.bind_connector(conn, kNoCrtc));
                    break;
                }
            }
        }
    }
    TRY(request.set_crtc_mode(*attempt.crtc, mode_blob.id(), true));
    TRY(request.set_plane(*attempt.plane, source_fb, attempt.crtc->id, SrcRect::whole(size),
                          CrtcRect::at_origin(size)));
    TRY(attach_writeback(request, *attempt.writeback, attempt.crtc->id, writeback_fb,
                         out_fence_fd));
    return Ok();
}

/// 等 fence signal。返回 false 表示超时。
bool wait_fence(int fence_fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fence_fd;
    pfd.events = POLLIN;
    int ready = -1;
    do {
        ready = ::poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

/**
 * @brief 比对回写下来的像素与我们画进去的
 *
 * 判据不是"像素完全相等" —— 显示通路上可能有色彩转换、抖动、
 * 或者 WB_POINT 抓的根本不是显示输出那一级（Q-4）。所以先看两件更基本的事：
 * 有没有非零像素，以及采样点的颜色**顺序**对不对。
 * 不相等但顺序对，说明抓到的是这一帧、只是经过了处理；
 * 全零则说明什么都没抓到。这两种结论要能分开。
 */
void compare_readback(DumbBuffer& writeback_buffer, Size size, span<const uint32_t> expected_bars) {
    // 一律抹掉最高 8 位再比：源是 XR24（X 位无定义），目标可能是 AR24
    // （alpha 由硬件填）。比 alpha 只会得到一个与内容无关的差异。
    const span<uint8_t> pixels = writeback_buffer.bytes();
    const size_t stride = writeback_buffer.pitch();

    size_t nonzero = 0;
    const size_t sample_rows = size.height < 64 ? size.height : 64;
    for (uint32_t y = 0; y < sample_rows; ++y) {
        const uint8_t* row = pixels.data() + (static_cast<size_t>(y) * stride);
        for (uint32_t x = 0; x < size.width; x += 16) {
            uint32_t value = 0;
            std::memcpy(&value, row + (static_cast<size_t>(x) * 4u), sizeof(value));
            if ((value & 0x00ffffffu) != 0) {
                ++nonzero;
            }
        }
    }

    if (nonzero == 0) {
        finding("L3", "the writeback buffer is entirely zero -- the job reported success but "
                      "nothing was captured (check WB_POINT and dmesg)");
        return;
    }

    const uint32_t bar_width = size.width / static_cast<uint32_t>(expected_bars.size());
    const uint32_t y = size.height / 2;
    size_t matched = 0;
    for (size_t bar = 0; bar < expected_bars.size(); ++bar) {
        const uint32_t x = static_cast<uint32_t>(bar) * bar_width + bar_width / 2;
        uint32_t value = 0;
        std::memcpy(&value,
                    pixels.data() + (static_cast<size_t>(y) * stride) +
                        (static_cast<size_t>(x) * 4u),
                    sizeof(value));
        value &= 0x00ffffffu;
        std::printf("    bar %zu: wrote 0x%06x, read back 0x%06x%s\n", bar,
                    expected_bars[bar], value, value == expected_bars[bar] ? "" : "  (differs)");
        if (value == expected_bars[bar]) {
            ++matched;
        }
    }

    if (matched == expected_bars.size()) {
        finding("L3", "writeback captured the frame exactly -- CRC-style verification is usable, "
                      "Step 5 has a machine-checkable acceptance criterion");
    } else {
        finding("L3", fmt("writeback captured something ({}/{} sample points match exactly); "
                          "the pipeline applies some conversion, so compare structure rather than "
                          "exact values, and check WB_POINT",
                          matched, expected_bars.size()));
    }
}

const uint32_t kBars[] = {
    0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
    0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00303030u,
};

void draw_bars(DumbBuffer& buffer, Size size) {
    constexpr uint32_t kBarCount = sizeof(kBars) / sizeof(kBars[0]);
    const span<uint8_t> pixels = buffer.bytes();
    const size_t stride = buffer.pitch();
    const uint32_t bar_width = size.width / kBarCount;
    std::vector<uint32_t> row(size.width);
    for (uint32_t x = 0; x < size.width; ++x) {
        row[x] = kBars[bar_width > 0 ? (x / bar_width) % kBarCount : 0];
    }
    for (uint32_t y = 0; y < size.height; ++y) {
        std::memcpy(pixels.data() + (static_cast<size_t>(y) * stride), row.data(),
                    static_cast<size_t>(size.width) * 4u);
    }
}

// ---------------------------------------------------------------------------
// 一次尝试
// ---------------------------------------------------------------------------

int run_attempt(const Device& device, const Attempt& attempt, const Options& options,
                const char* label) {
    const Size size = attempt.mode->size();
    std::printf("\n--- %s: %s + %s on %s, %ux%u, writeback format %s\n", label,
                to_string(attempt.writeback->id).c_str(),
                attempt.display != nullptr ? attempt.display->name.c_str() : "<no display>",
                to_string(attempt.crtc->id).c_str(), size.width, size.height,
                to_string(attempt.writeback_format).c_str());

    auto source = DumbBuffer::create(device.fd(), size, Format{DRM_FORMAT_XRGB8888});
    if (! source) {
        log_error_object(source.error(), "cannot allocate the source buffer");
        return 1;
    }
    DumbBuffer source_buffer = std::move(source).value();
    if (auto status = source_buffer.register_framebuffer(); ! status) {
        log_error_object(status.error(), "cannot register the source framebuffer");
        return 1;
    }
    draw_bars(source_buffer, size);

    // 目标格式取自 WRITEBACK_PIXEL_FORMATS，不是随手写一个
    auto destination = DumbBuffer::create(device.fd(), size, attempt.writeback_format);
    if (! destination) {
        log_error_object(destination.error(), "cannot allocate the writeback buffer");
        return 1;
    }
    DumbBuffer writeback_buffer = std::move(destination).value();
    if (auto status = writeback_buffer.register_framebuffer(); ! status) {
        log_error_object(status.error(), "cannot register the writeback framebuffer");
        return 1;
    }
    writeback_buffer.fill(0x00000000u); // 全零起步，这样"什么都没抓到"是可分辨的

    auto blob = PropertyBlob::create(device.fd(), &attempt.mode->raw, sizeof(attempt.mode->raw));
    if (! blob) {
        log_error_object(blob.error(), "cannot create the mode blob");
        return 1;
    }
    const PropertyBlob mode_blob = std::move(blob).value();

    AtomicRequest request(device.fd());
    int fence_fd = -1;

    // ---- TEST_ONLY（Q-2 的答案就在这里）----
    if (auto status = build_request(device, request, attempt, mode_blob, source_buffer.fb_id(),
                                    writeback_buffer.fb_id(), size, nullptr);
        ! status) {
        log_error_object(status.error(), "cannot build the atomic request");
        return 1;
    }
    request.dump("writeback test");

    const int test_result = request.test(CommitFlags::AllowModeset);
    if (test_result != 0) {
        finding("Q-2", fmt("{}: TEST_ONLY rejected with {}", label, errno_name(test_result)));
        request.bisect_rejection(CommitFlags::AllowModeset);
        return 1;
    }
    finding("Q-2", fmt("{}: TEST_ONLY accepted", label));

    if (! options.commit) {
        std::printf("  (not committing; pass --commit to answer Q-1)\n");
        return 0;
    }

    // ---- 真提交（Q-1 的答案在这里）----
    if (auto status = build_request(device, request, attempt, mode_blob, source_buffer.fb_id(),
                                    writeback_buffer.fb_id(), size, &fence_fd);
        ! status) {
        log_error_object(status.error(), "cannot build the commit request");
        return 1;
    }

    // 非阻塞 + PAGE_FLIP_EVENT：**必须非阻塞**。阻塞提交会让我们卡在内核的
    // wait_for_flip_done 里，那正是 Q-1 的坏结果，而卡住的进程什么都报不出来。
    if (auto status = request.commit(CommitFlags::AllowModeset | CommitFlags::Nonblock |
                                         CommitFlags::PageFlipEvent,
                                     0);
        ! status) {
        log_error_object(status.error(), "writeback commit");
        return 1;
    }

    const bool readable = [&] {
        auto result = wait_readable(device.fd(), options.timeout_ms);
        return result && result.value();
    }();

    if (! readable) {
        finding("Q-1", fmt("{}: no page flip event within {} ms -- this is exactly the shape of a "
                           "no_vblank CRTC without drm_atomic_helper_fake_vblank; the kernel will "
                           "keep waiting until its own 10s flip_done timeout",
                           label, options.timeout_ms));
    } else {
        size_t events = 0;
        (void) read_events(device.fd(), device.caps().timestamp_monotonic,
                           [&](const FlipEvent&) { ++events; });
        finding("Q-1", fmt("{}: page flip event arrived ({} event(s)); the writeback CRTC does "
                           "deliver completion normally",
                           label, events));
    }

    // ---- out fence 与回读 ----
    if (fence_fd < 0) {
        finding("L3", "the kernel did not hand back a writeback out fence; "
                      "there is no safe moment to read the buffer");
    } else {
        UniqueFd fence(fence_fd);
        if (! wait_fence(fence.get(), options.timeout_ms)) {
            finding("L3", fmt("the writeback out fence did not signal within {} ms",
                              options.timeout_ms));
        } else {
            std::printf("  writeback fence signalled, reading the buffer back\n");
            compare_readback(writeback_buffer, size, span<const uint32_t>(kBars, 8));
        }
    }

    return 0;
}

// ---------------------------------------------------------------------------
// 组装
// ---------------------------------------------------------------------------

int run(const Options& options) {
    auto device_result = options.device_path != nullptr
                             ? Device::open(std::string(options.device_path))
                             : (options.driver_name != nullptr
                                    ? Device::open_by_driver(std::string(options.driver_name))
                                    : Device::open_first_kms());
    if (! device_result) {
        log_error_object(device_result.error(), "cannot open a KMS device");
        return 1;
    }
    Device device = std::move(device_result).value();

    // writeback connector 只在设了 DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 之后才可见。
    // Device 在 open 时就设了；这里再确认一次，因为"一个都没枚举到"和
    // "cap 没设上"是两种完全不同的结论。
    std::vector<const Connector*> writebacks;
    for (const Connector& conn : device.connectors()) {
        if (conn.is_writeback) {
            writebacks.push_back(&conn);
        }
    }
    if (writebacks.empty()) {
        finding("Q-3", "no writeback connector was enumerated; either this driver has none or "
                       "DRM_CLIENT_CAP_WRITEBACK_CONNECTORS was refused");
        return 1;
    }
    std::printf("found %zu writeback connector(s)\n", writebacks.size());

    for (const Connector* conn : writebacks) {
        describe_writeback_connector(device, *conn);
    }

    auto master = device.acquire_master();
    if (! master) {
        LOG_ERROR("{}", device.master_diagnosis());
        return 1;
    }

    int failures = 0;
    const Crtc* last_crtc = nullptr;
    const Plane* last_plane = nullptr;

    for (const Connector* writeback : writebacks) {
        // 这个 writeback 能挂哪些 CRTC
        std::vector<const Crtc*> candidates;
        for (const EncoderId enc_id : writeback->encoders) {
            const Encoder* encoder = device.encoder(enc_id);
            if (encoder == nullptr) {
                continue;
            }
            for (const Crtc& crtc : device.crtcs()) {
                if (encoder->possible_crtcs.contains(crtc.index)) {
                    candidates.push_back(&crtc);
                }
            }
        }

        for (const Crtc* crtc : candidates) {
            const std::vector<PlaneId> planes = device.planes_for_crtc(crtc->id, PlaneType::Primary);
            if (planes.empty()) {
                continue;
            }
            const Plane* plane = device.plane(planes.front());
            if (plane == nullptr) {
                continue;
            }

            // 同一个 CRTC 上有没有一个真实显示 connector
            const Connector* display = nullptr;
            for (const Connector& conn : device.connectors()) {
                if (conn.is_writeback || ! conn.connected || conn.modes.empty()) {
                    continue;
                }
                for (const EncoderId enc_id : conn.encoders) {
                    const Encoder* encoder = device.encoder(enc_id);
                    if (encoder != nullptr && encoder->possible_crtcs.contains(crtc->index)) {
                        display = &conn;
                        break;
                    }
                }
                if (display != nullptr) {
                    break;
                }
            }

            const std::vector<Format> wb_formats = read_pixel_formats(device.fd(), *writeback);
            const Format* wb_format = pick_writeback_format(wb_formats);
            if (wb_format == nullptr) {
                finding("Q-3", fmt("{} reports no packed 32-bit RGB format we can read back; "
                                   "content verification through writeback would need a format "
                                   "conversion, which defeats the point",
                                   to_string(writeback->id)));
                continue;
            }

            Attempt attempt;
            attempt.writeback = writeback;
            attempt.crtc = crtc;
            attempt.plane = plane;
            attempt.writeback_format = *wb_format;
            last_crtc = crtc;
            last_plane = plane;

            const bool want_coexist = options.mode != Mode::Headless;
            const bool want_headless = options.mode != Mode::Coexist;

            if (want_coexist && display != nullptr) {
                attempt.display = display;
                attempt.mode = display->preferred_mode();
                if (attempt.mode != nullptr) {
                    failures += run_attempt(device, attempt, options, "coexist");
                }
            } else if (want_coexist) {
                std::printf("\n--- coexist: %s has no connected display connector, skipped\n",
                            to_string(crtc->id).c_str());
            }

            if (want_headless) {
                // 无显示 connector，只有 writeback 挂在 CRTC 上。
                // mode 取自 writeback connector 自己报的列表 —— writeback
                // connector 的 mode 列表描述的是它能写多大，不是某个显示器。
                attempt.display = nullptr;
                attempt.mode = writeback->preferred_mode();
                if (attempt.mode == nullptr) {
                    std::printf("\n--- headless: %s reports no mode, skipped\n",
                                to_string(writeback->id).c_str());
                } else {
                    failures += run_attempt(device, attempt, options, "headless");
                }
            }

            if (g_should_stop != 0) {
                break;
            }
        }
    }

    // 退出前把 CRTC 关掉。不是为了好看：一次带 writeback 的提交之后，
    // 这块硬件的 DPU 会继续往那块 buffer 写（见 repro/wb-oneshot-fault/），
    // 而我们马上就要释放它。关掉 CRTC 至少让故障停在一个明确的时刻。
    if (options.commit && last_crtc != nullptr && last_plane != nullptr) {
        AtomicRequest teardown(device.fd());
        (void) teardown.disable_plane(*last_plane);
        (void) teardown.disable_crtc(*last_crtc);
        for (const Connector& conn : device.connectors()) {
            (void) teardown.bind_connector(conn, kNoCrtc);
        }
        if (auto status = teardown.commit(CommitFlags::AllowModeset); ! status) {
            log_error_object(status.error(), "teardown");
        } else {
            std::printf("teardown: crtc disabled\n");
        }
    }

    std::printf("\n%d finding(s) recorded above. Copy them into docs/env.md and close\n"
                "Q-1 ~ Q-4 in docs/open-questions.md.\n",
                g_findings);
    if (options.commit) {
        std::printf("note: --commit leaves the display showing this probe's test pattern, and the\n"
                    "framebuffers are removed on exit, so the screen goes blank until something\n"
                    "else drives it (switch VT, or restart your display manager).\n");
    }
    if (! options.commit) {
        std::printf("this run only did TEST_ONLY; Q-1 needs --commit.\n");
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (! parse_options(argc, argv, options)) {
        return 2;
    }

    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    const int rc = run(options);
    report_leaks_on_exit();
    return rc;
}
