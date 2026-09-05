/**
 * @file demos/hello_screen/main.cpp
 * @brief `mw::display::Screen` 的最小完整用法 —— 下游项目的起点模板
 *
 * 这个 demo 和其它 step demo 的定位不同。step demo 的职责是**把中间状态
 * 摊开给人看**，所以它们直接调 mw/drm 与 mw/render，每个都四百行起。
 * 这一个的职责相反：证明消费这个库不需要知道那四百行。
 *
 * 除去参数解析和画图本身，真正和显示相关的代码是下面 `run()` 里的
 * 十几行。把 `draw()` 换成一个软件光栅化器，就是 mini-render 的 main。
 *
 * 默认走 Offscreen 后端，所以**不需要 root、不需要停显示管理器、
 * 不需要 /dev/dri**，开发机上直接跑。加 `-b kms` 才真的点屏。
 *
 * @code
 *   ./build/debug/bin/hello_screen -f 120                    # 什么都不需要
 *   ./build/debug/bin/hello_screen -f 3 -o /tmp/out          # 出三张 PPM
 *   sudo ./build/debug/bin/hello_screen -b kms -f 600        # 真上屏
 * @endcode
 */
#include <cstdio>
#include <string>

#include "mw/trace/log.hpp"
#include "mw/display/screen.hpp"
#include "mw/internal/span.hpp"
#include "mw/internal/parse_args.hpp"
#include "mw/internal/signal.hpp"
#include "mw/version.hpp"

using namespace mw;
using internal::span;
using mw::display::Backend;
using mw::display::Frame;
using mw::display::Screen;
using mw::display::ScreenConfig;

namespace {

volatile sig_atomic_t g_should_stop = 0;

void on_signal(int /*signum*/) {
    g_should_stop = 1;
}

/// 命令行配置。字段即选项，`internal::parse_args` 负责绑定与 --help。
///
/// **不要在这里手写 argv 循环。** 见 docs/internal-lib.md：
/// 手写的版本每个 demo 一份、各自漏掉不同的边界情况（缺参数、`--opt=val`、
/// 未知选项静默忽略），而且 --help 和实际支持的选项会慢慢对不上。
struct Options {
    std::string backend = "offscreen";
    uint64_t frames = 0;
    std::string size = "1280x720";
    std::string device{};
    std::string dump_dir{};
    bool no_pace = false;
};

// ---------------------------------------------------------------------------
// 画点东西 —— 这里就是下游项目该替换掉的地方
// ---------------------------------------------------------------------------
//
// 刻意写得笨：一个渐变加一条移动的竖条。竖条的作用是让"帧真的在推进"
// 和"帧号在涨但画面没变"这两件事在肉眼下可分辨 —— 后者是一个非常常见的
// 故障形态（画进了一块没被扫描的 buffer），静态画面看不出来。

void draw(const Frame& frame, uint64_t tick) {
    const uint32_t bar_x = static_cast<uint32_t>(tick * 7u % frame.size.width);

    for (uint32_t y = 0; y < frame.size.height; ++y) {
        span<uint8_t> row = frame.row(y);
        if (row.empty()) {
            break;
        }
        const auto green = static_cast<uint8_t>(y * 255u / frame.size.height);
        for (uint32_t x = 0; x < frame.size.width; ++x) {
            const auto red = static_cast<uint8_t>(x * 255u / frame.size.width);
            const bool in_bar = (x >= bar_x && x < bar_x + 24u);

            // XRGB8888 在小端机器上的字节序是 B G R X。
            // 用 frame.stride 定位行、用 4 定位列 —— 不要用 width*4 算行首，
            // 真实 buffer 的 stride 常常比那大。
            uint8_t* px = row.data() + static_cast<size_t>(x) * 4u;
            px[0] = in_bar ? 255u : 0u;  // B
            px[1] = in_bar ? 255u : green;
            px[2] = in_bar ? 255u : red;
            px[3] = 0u;
        }
    }
}

// ---------------------------------------------------------------------------

bool parse_size(const std::string& text, drm::Size& out);

int run(const Options& options) {
    ScreenConfig config;
    config.device_path = options.device;
    config.offscreen_pace = ! options.no_pace;
    config.dump_dir = options.dump_dir;
    config.dump_every = options.dump_dir.empty() ? 0u : 1u;

    if (options.backend == "kms") {
        config.backend = Backend::Kms;
    } else if (options.backend == "offscreen") {
        config.backend = Backend::Offscreen;
    } else {
        std::fprintf(stderr, "error: unknown backend '%s'\n", options.backend.c_str());
        return 2;
    }
    if (! parse_size(options.size, config.offscreen_size)) {
        std::fprintf(stderr, "error: bad size '%s', expected WxH\n", options.size.c_str());
        return 2;
    }

    auto screen_result = Screen::open(config);
    if (! screen_result) {
        log_error_object(screen_result.error(), "Screen::open");
        return 1;
    }
    Screen screen = std::move(screen_result).value();
    LOG_INFO("{}", screen.to_string());

    for (uint64_t i = 0; ((! g_should_stop) && (options.frames == 0 || i < options.frames)); ++i) {
        auto frame = screen.begin_frame();
        if (! frame) {
            log_error_object(frame.error(), "begin_frame");
            return 1;
        }

        draw(frame.value(), i);

        if (auto status = screen.present(); ! status) {
            log_error_object(status.error(), "present");
            return 1;
        }
    }

    LOG_INFO("{}", screen.stats().to_line());
    return 0;
}

bool parse_size(const std::string& text, drm::Size& out) {
    unsigned w = 0;
    unsigned h = 0;
    if (std::sscanf(text.c_str(), "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
        return false;
    }
    out = drm::Size{w, h};
    return true;
}

} // namespace

int main(int argc, char** argv) {
    // 信号处理走 internal::sig::guard —— RAII，析构时把 handler 恢复回去。
    // 手写 sigaction 的版本从来不恢复，在这个 demo 里无所谓，
    // 但在一个会被别人 fork/exec 的合成器里就不是无所谓的事。
    const auto signals = internal::sig::bind({
        {SIGINT, on_signal},
        {SIGTERM, on_signal},
    });

    if (! check_abi()) {
        std::fprintf(stderr, "error: header/library version mismatch\n");
        return 2;
    }

    auto parser = internal::parse_args::parser<Options>("draw a moving bar onto a Screen")
        .bind(&Options::backend,  "-b", "--backend",  "kms | offscreen")
        .bind(&Options::frames,   "-f", "--frames",   "frame count, 0 = until Ctrl+C")
        .bind(&Options::size,     "-s", "--size",     "offscreen size, WxH")
        .bind(&Options::device,   "-D", "--device",   "KMS device node; empty = auto")
        .bind(&Options::dump_dir, "-o", "--dump-dir", "offscreen: write each frame as PPM here")
        .bind(&Options::no_pace,  "--no-pace",        "offscreen: do not sleep between frames")
        .example("hello_screen -f 120", "offscreen, no privileges needed")
        .example("sudo hello_screen -b kms -f 600", "real scanout")
        .note("The offscreen backend touches no DRM object and validates nothing "
              "about the display path -- see mw/display/screen.hpp.");

    auto config = parser.parse(argc, argv);
    return run(config);
}
