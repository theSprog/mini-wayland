/**
 * demos/probe_kms -- 枚举 KMS 资源、自检、可选的全量 dump
 *
 * Step 1 的第一个检查点。**不碰 master、不提交任何东西**，
 * 所以 X11 在跑的时候也能跑，不需要停 lightdm。
 *
 *   probe_kms                      # 摘要 + 自检（默认，几十行）
 *   probe_kms -d vsdrm             # 按 DRM driver name 打开
 *   probe_kms /dev/dri/card2       # 指定节点
 *   probe_kms -l                   # 只列候选节点
 *   probe_kms -c                   # 只跑自检，最少输出
 *   probe_kms -t                   # 只打拓扑
 *   probe_kms -v > /tmp/full.txt   # 全量属性表，几百行，建议重定向
 *
 *   MW_LOG=debug probe_kms         # 看每一次 ioctl
 *
 * 默认输出刻意做得紧凑：能一屏看完、能直接贴出来。
 * 与其把几百行 dump 和 modetest 逐行比对，不如看自检那十几条 PASS/FAIL ——
 * 我们真正依赖的不变量就那么多。要做人工比对时再用 -v。
 *
 * 退出码：0 全过；1 打不开设备；2 有自检失败。
 */
#include <cstring>
#include <string>

#include "mw/trace/log.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/dump.hpp"
#include "mw/drm/trace.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

void print_usage(const char* argv0) {
    std::printf("usage: %s [options] [/dev/dri/cardN]\n", argv0);
    std::printf("  -d <name>   open by DRM driver name (e.g. vkms, vsdrm)\n");
    std::printf("  -l          list candidate nodes and exit\n");
    std::printf("  -c          self checks only (least output)\n");
    std::printf("  -t          topology only\n");
    std::printf("  -v          full dump including every property table (hundreds of lines)\n");
    std::printf("  -F          dump raw IN_FORMATS blobs and verify them for self-consistency\n");
    std::printf("  -h          this help\n");
    std::printf("\ndefault is a compact summary plus self checks.\n");
    std::printf("\nenvironment:\n");
    std::printf("  MW_LOG=error|warn|info|debug|trace\n");
    std::printf("  MW_LOG_TIME=1     prefix each line with a monotonic timestamp\n");
    std::printf("\nexit code: 0 all good, 1 cannot open device, 2 self check failed\n");
}

int list_nodes() {
    const auto candidates = enumerate_devices();
    if (candidates.empty()) {
        LOG_ERROR("no card nodes found under /dev/dri");
        return 1;
    }
    LOG_INFO("candidate nodes:");
    LOG_SCOPE();
    for (const auto& candidate : candidates) {
        LOG_INFO("{}", candidate.to_string());
    }
    LOG_INFO("");
    LOG_INFO("note: the driver name above is the DRM driver name from drmGetVersion,");
    LOG_INFO("      which is what 'modetest -M <name>' expects. It is NOT the PCI");
    LOG_INFO("      driver name you see under /sys/class/drm/cardN/device/driver.");

    bool saw_vkms = false;
    for (const auto& candidate : candidates) {
        if (candidate.driver_name == "vkms") {
            saw_vkms = true;
        }
    }
    if (! saw_vkms) {
        // 双环境验收要求 VKMS 那一半，缺了就提醒一下怎么补。
        LOG_INFO("");
        LOG_INFO("vkms is not loaded; 'sudo modprobe vkms' adds a virtual KMS node.");
        LOG_INFO("it is the generality litmus test: one primary plane, XR24 only,");
        LOG_INFO("no modifiers -- code that works there has no vendor assumptions.");
    }
    return 0;
}

Result<Device> open_target(const char* explicit_path, const char* driver_name) {
    if (explicit_path != nullptr) {
        return Device::open(std::string(explicit_path));
    }
    if (driver_name != nullptr) {
        return Device::open_by_driver(std::string(driver_name));
    }
    return Device::open_first_kms();
}

} // namespace

int main(int argc, char** argv) {
    const char* explicit_path = nullptr;
    const char* driver_name = nullptr;
    enum class Mode { Summary, ChecksOnly, Topology, Full, InFormats };
    Mode mode = Mode::Summary;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "-l") == 0) {
            return list_nodes();
        }
        if (std::strcmp(arg, "-t") == 0) {
            mode = Mode::Topology;
            continue;
        }
        if (std::strcmp(arg, "-c") == 0) {
            mode = Mode::ChecksOnly;
            continue;
        }
        if (std::strcmp(arg, "-v") == 0) {
            mode = Mode::Full;
            continue;
        }
        if (std::strcmp(arg, "-F") == 0) {
            mode = Mode::InFormats;
            continue;
        }
        if (std::strcmp(arg, "-d") == 0) {
            if (i + 1 >= argc) {
                LOG_ERROR("-d needs a driver name");
                return 1;
            }
            driver_name = argv[++i];
            continue;
        }
        if (arg[0] == '-') {
            LOG_ERROR("unknown option '{}'", arg);
            print_usage(argv[0]);
            return 1;
        }
        explicit_path = arg;
    }

    auto device_result = open_target(explicit_path, driver_name);
    if (! device_result) {
        log_error_object(device_result.error(), "cannot open a KMS device");
        LOG_INFO("");
        LOG_INFO("try '{} -l' to see what nodes are available", argv[0]);
        return 1;
    }
    const Device device = std::move(device_result).value();

    switch (mode) {
        case Mode::Topology:
            dump_topology(device);
            break;
        case Mode::Full:
            dump_device(device);
            break;
        case Mode::ChecksOnly:
            break;
        case Mode::InFormats:
            // 老版 modetest 不打印 IN_FORMATS，没法拿它交叉验证。
            // 这里把 blob 的原始结构打出来并自校验，让数据自己说话。
            for (const auto& plane : device.planes()) {
                dump_in_formats_raw(device, plane);
            }
            break;
        case Mode::Summary:
            dump_summary(device);
            break;
    }

    size_t failures = 0;
    if (mode != Mode::Topology && mode != Mode::InFormats) {
        failures = run_self_checks(device);
    }

    if (mode != Mode::ChecksOnly) {
        // master 只探测不获取：这个 demo 不提交任何东西，X11 在跑时也应该能跑完。
        LOG_INFO("=== master ===");
        LOG_SCOPE();
        if (device.is_master()) {
            LOG_INFO("this process IS DRM master (nothing else holds the node)");
        } else {
            LOG_INFO("this process is NOT DRM master -- fine for enumeration,");
            LOG_INFO("but any modeset would fail. Run with MW_LOG=debug for the diagnosis.");
            LOG_DEBUG("{}", device.master_diagnosis());
        }

        // 枚举一共花了多少次内核往返。数字本身就是学习材料：
        // 一个对象的属性表要一次 GetObjectProperties + N 次 GetProperty，
        // 全设备枚举下来往往几百次 —— 这就是为什么它只能做一次。
        LOG_INFO("ioctl cost of one full enumeration: {}", stats().to_line_full());
    }

    // 演示密封机制：枚举结束后再碰一次属性查询就会被抓出来。
    seal_init_phase();
    check_sealed("after enumeration");

    return failures == 0 ? 0 : 2;
}
