/**
 * demos/smoke_core -- 不碰任何硬件的自检
 *
 * 目的：在没有 DRM 权限、甚至没有显卡的机器上也能验证
 * 日志 / 错误传播 / 强类型格式化 / ioctl 记账 这几个基础设施是通的。
 * Step 1 的真正点屏 demo 是 step1_kms_atomic_dumb，那个需要 master。
 *
 *   MW_LOG=trace MW_LOG_TIME=1 ./build/debug/bin/smoke_core
 */
#include <cerrno>

#include "mw/core/log.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/trace.hpp"
#include "mw/drm/types.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

Result<UniqueFd> open_something(const char* path) {
    auto fd = TRY(UniqueFd::open(path, 0));
    return Ok(std::move(fd));
}

void exercise_strong_types() {
    LOG_INFO("strong-typed ids print with their kind:");
    LOG_SCOPE();
    LOG_INFO("connector = {}", ConnectorId{142});
    LOG_INFO("crtc      = {}", CrtcId{84});
    LOG_INFO("crtc idx  = {}", CrtcIndex{1});
    LOG_INFO("plane     = {}", PlaneId{87});
    LOG_INFO("possible  = {}", PossibleCrtcs{0x3u});
    LOG_INFO("format    = {}", Format{0x34325258u});
    LOG_INFO("modifier  = {}", kModifierLinear);
    LOG_INFO("modifier  = {}", Modifier{0x0b00000000002000ULL});
}

void exercise_fixed_point() {
    // 16.16 的核心演示：SrcRect 里的值已经移过位，CrtcRect 没有。
    // 两者类型不同，写反了编译不过。
    const Size size{1920, 1080};
    const SrcRect src = SrcRect::whole(size);
    const CrtcRect dst = CrtcRect::at_origin(size);

    LOG_INFO("16.16 fixed point vs integer coordinates:");
    LOG_SCOPE();
    LOG_INFO("SRC_*  (16.16) = {}", src);
    LOG_INFO("CRTC_* (int)   = {}", dst);
    LOG_INFO("raw SRC_W = {} -- this is what goes into the atomic request",
             src.width.raw());
}

void exercise_errors() {
    LOG_INFO("error propagation:");
    LOG_SCOPE();

    auto ok = open_something("/dev/null");
    if (ok) {
        LOG_INFO("opened /dev/null as fd={}", ok.value().get());
    }

    auto bad = open_something("/definitely/not/here");
    if (! bad) {
        log_error_object(bad.error(), "expected failure");
        LOG_INFO("is_errno(ENOENT) = {}", is_errno(bad.error(), ENOENT));
        LOG_INFO("is_errno(EBUSY)  = {}", is_errno(bad.error(), EBUSY));
    }
}

void exercise_accounting() {
    LOG_INFO("ioctl accounting:");
    LOG_SCOPE();

    // 假装做了一轮初始化
    stats().get_resources += 1;
    stats().get_properties += 12;
    stats().create_blob += 1;
    stats().add_fb += 2;
    stats().create_dumb += 2;

    seal_init_phase();

    // 假装跑了 60 帧。注意打的是**增量**：绝对值里含初始化阶段的计数，
    // 每秒看增量才能判断稳态是否干净。
    const IoctlStats before_frames = stats();
    for (int i = 0; i < 60; ++i) {
        stats().atomic_commit += 1;
        stats().page_flip_events += 1;
        check_sealed("frame loop");
    }
    LOG_INFO("one second of steady state: {}",
             IoctlStats::delta(stats(), before_frames).to_line());

    // 故意越界一次，看它被抓出来
    LOG_INFO("now deliberately violating the rule:");
    stats().get_properties += 1;
    check_sealed("frame loop");

    // 收尾：故意不释放，看泄漏检测
    stats().destroy_blob += 1;
    stats().rm_fb += 2;
    stats().destroy_dumb += 1;  // 少一个，应该被报出来
    report_leaks_on_exit();
}

} // namespace

int main() {
    LOG_INFO("mini-wayland core smoke test");
    LOG_INFO("set MW_LOG=trace / MW_LOG_TIME=1 for more");

    exercise_strong_types();
    exercise_fixed_point();
    exercise_errors();
    exercise_accounting();

    LOG_INFO("done");
    return 0;
}
