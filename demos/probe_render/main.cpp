/**
 * demos/probe_render -- 枚举 render node，回答"哪个节点才是 GPU"
 *
 * **不碰 master、不分配任何 buffer**，X11 跑着也能用，不需要 root。
 *
 *   probe_render                 # 全部 render node + 每个 card 的配对关系
 *   probe_render -m              # 顺带探测各设备的 pitch 对齐要求
 *
 * 当显示与渲染分属不同 DRM 节点时，"哪个节点是渲染设备"不能从
 * 驱动名字推断 —— DRM driver name 与 PCI driver name、与实际的 IP
 * 供应商都可能不一致。打错节点的话，整条渲染路径的工作都会落空。
 *
 * 本 demo 把判断所需的原始信息全打出来，不下结论。
 *
 * 判断依据（本 demo 全部打出来，不做结论）：
 *   1. DRM driver name（drmGetVersion）—— modetest -M 要的就是这个
 *   2. 每个 card 通过 drmGetDevice2 报告的配对 render node
 *   3. 有没有 KMS 资源（有 = 显示设备，无 = 纯渲染设备）
 *   4. PRIME import / export 能力位
 *
 * 退出码：0 正常；1 /dev/dri 下什么都没有。
 */
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mw/trace/log.hpp"
#include "mw/internal/unique_fd.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/trace.hpp"

using namespace mw;
using namespace mw::drm;

namespace {

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  -m          also probe the pitch alignment of each node\n");
    std::printf("  -h          this help\n");
    std::printf("\nlists every /dev/dri node, its DRM driver name, whether it has KMS\n");
    std::printf("resources, and which render node it is paired with.\n");
    std::printf("\nneeds neither root nor DRM master.\n");
}

struct NodeInfo {
    std::string path{};
    std::string driver{};
    bool opened = false;
    bool has_kms = false;
    bool prime_import = false;
    bool prime_export = false;
    bool dumb = false;
    std::string paired_render{};  ///< 空表示没有配对的 render node
    std::string pitch_note{};
};

/// /dev/dri 下所有 card* 与 renderD* 的路径
std::vector<std::string> list_node_paths() {
    std::vector<std::string> paths;
    // 直接按编号试探而不是读目录：节点编号空间很小，且这样输出顺序稳定，
    // 便于把两次运行的结果 diff 起来看。
    for (int i = 0; i < 16; ++i) {
        paths.push_back(fmt("/dev/dri/card{}", i));
    }
    for (int i = 128; i < 144; ++i) {
        paths.push_back(fmt("/dev/dri/renderD{}", i));
    }
    return paths;
}

uint64_t get_cap(BorrowedFd fd, uint64_t cap) {
    uint64_t value = 0;
    if (drmGetCap(fd.get(), cap, &value) != 0) {
        return 0;
    }
    return value;
}

NodeInfo inspect(const std::string& path, bool probe_pitch) {
    NodeInfo info;
    info.path = path;

    auto fd_result = UniqueFd::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (! fd_result) {
        return info;  // 节点不存在或没权限，静默跳过
    }
    const UniqueFd fd = std::move(fd_result).value();
    info.opened = true;

    drmVersionPtr version = drmGetVersion(fd.get());
    if (version != nullptr) {
        info.driver.assign(version->name, static_cast<size_t>(version->name_len));
        drmFreeVersion(version);
    }

    // KMS 资源。render node 上这个必然失败，正好用来区分两类节点。
    drmModeResPtr resources = drmModeGetResources(fd.get());
    if (resources != nullptr) {
        info.has_kms = resources->count_crtcs > 0 && resources->count_connectors > 0;
        drmModeFreeResources(resources);
    }

    // DRM_CAP_PRIME 是一个位掩码，不是布尔量。
    const uint64_t prime = get_cap(fd.borrow(), DRM_CAP_PRIME);
    info.prime_import = (prime & DRM_PRIME_CAP_IMPORT) != 0u;
    info.prime_export = (prime & DRM_PRIME_CAP_EXPORT) != 0u;
    info.dumb = get_cap(fd.borrow(), DRM_CAP_DUMB_BUFFER) != 0u;

    drmDevicePtr device = nullptr;
    if (drmGetDevice2(fd.get(), 0, &device) == 0 && device != nullptr) {
        if ((device->available_nodes & (1 << DRM_NODE_RENDER)) != 0) {
            info.paired_render = device->nodes[DRM_NODE_RENDER];
        }
        drmFreeDevice(&device);
    }

    if (probe_pitch) {
        const auto alignment = probe_pitch_alignment(fd.borrow());
        if (! alignment.has_value()) {
            info.pitch_note = "n/a";
        } else if (*alignment == 4) {
            info.pitch_note = "none";
        } else {
            info.pitch_note = fmt("{}B", *alignment);
        }
    }

    return info;
}

void report(const std::vector<NodeInfo>& nodes, bool probe_pitch) {
    LOG_INFO("{:<22} {:<10} {:<5} {:<8} {:<8} {}", "node", "driver", "kms", "prime", "dumb",
             probe_pitch ? "pitch" : "paired render node");
    LOG_INFO("{:-<22} {:-<10} {:-<5} {:-<8} {:-<8} {:-<24}", "", "", "", "", "", "");

    for (const auto& node : nodes) {
        if (! node.opened) {
            continue;
        }
        std::string prime;
        if (node.prime_import) {
            prime += "i";
        }
        if (node.prime_export) {
            prime += "e";
        }
        if (prime.empty()) {
            prime = "-";
        }

        const std::string tail = probe_pitch
                                     ? node.pitch_note
                                     : (node.paired_render.empty() ? "-" : node.paired_render);

        LOG_INFO("{:<22} {:<10} {:<5} {:<8} {:<8} {}", node.path, node.driver,
                 node.has_kms ? "yes" : "no", prime, node.dumb ? "yes" : "no", tail);
    }
}

void interpret(const std::vector<NodeInfo>& nodes) {
    std::vector<const NodeInfo*> display_nodes;
    std::vector<const NodeInfo*> render_nodes;
    for (const auto& node : nodes) {
        if (! node.opened) {
            continue;
        }
        if (node.has_kms) {
            display_nodes.push_back(&node);
        }
        if (node.path.find("renderD") != std::string::npos) {
            render_nodes.push_back(&node);
        }
    }

    LOG_INFO("");
    LOG_INFO("interpretation:");
    LOG_SCOPE();

    LOG_INFO("{} display node(s), {} render node(s)", display_nodes.size(), render_nodes.size());

    for (const auto* node : display_nodes) {
        if (node->paired_render.empty()) {
            LOG_INFO("{} ({}) has KMS but no paired render node -- display only", node->path,
                     node->driver);
        } else {
            LOG_INFO("{} ({}) pairs with {}", node->path, node->driver, node->paired_render);
        }
    }

    LOG_INFO("");
    LOG_INFO("the pairing above is metadata: libdrm groups nodes by bus address. It does");
    LOG_INFO("NOT say which node runs GL. When one physical device exposes several DRM");
    LOG_INFO("nodes with different jobs, which one gets 'paired' depends on enumeration");
    LOG_INFO("order, not on capability -- and picking the wrong one fails silently, by");
    LOG_INFO("falling back to a software rasteriser that works and is a hundred times");
    LOG_INFO("slower.");
    LOG_INFO("");
    LOG_INFO("to find the real GL host, bring up GBM + EGL on every candidate:");
    LOG_INFO("");
    LOG_INFO("  ./build/debug/bin/probe_caps        # prints a GL host candidate table");
    LOG_INFO("");
    LOG_INFO("Mesa links its drivers as hardlinks to one megadriver, so identical inode");
    LOG_INFO("numbers across many *_dri.so tell you which drivers that build contains --");
    LOG_INFO("a node whose driver name has no matching *_dri.so cannot host GL at all:");
    LOG_INFO("");
    LOG_INFO("  ls -li <mesa dri dir>/*.so | sort -n | head -20");
}

} // namespace

int main(int argc, char** argv) {
    bool probe_pitch = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "-m") == 0) {
            probe_pitch = true;
            continue;
        }
        LOG_ERROR("unknown option: {}", argv[i]);
        print_usage(argv[0]);
        return 1;
    }

    std::vector<NodeInfo> nodes;
    for (const auto& path : list_node_paths()) {
        nodes.push_back(inspect(path, probe_pitch));
    }

    bool any = false;
    for (const auto& node : nodes) {
        any = any || node.opened;
    }
    if (! any) {
        LOG_ERROR("no usable nodes under /dev/dri (permission denied? try the video group)");
        return 1;
    }

    report(nodes, probe_pitch);
    interpret(nodes);
    return 0;
}
