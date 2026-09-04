/**
 * demos/step3_dmabuf_ipc -- Step 3 的验收程序
 *
 * 一块 buffer 由**另一个进程**分配和绘制，本进程只负责导入、注册、上屏：
 *
 *   client: 分配 -> CPU 画图案 + 签名 -> 导出 dmabuf
 *             -> CREATE_BUFFER(元数据 + SCM_RIGHTS 传 fd)   一生一次
 *             -> COMMIT(buffer_id, frame_seq, signature_crc) 每帧
 *   server: PRIME 导入 -> addfb2                             一生一次
 *             -> atomic commit                               每帧
 *             -> BUFFER_RELEASE / FRAME_DONE                 每帧
 *
 * ## 单个二进制，用 --role 分角色
 *
 * 不拆成两个 demo 目录，因为**线格式的编解码两侧必须是同一份代码**。
 * 拆开会诱导出两份定义，而"两份定义漂移"正是消息头里那三个冗余校验字段
 * 要抓的东西 —— 不该先把它制造出来再去抓（见 lessons.md L-10）。
 *
 * ## 为什么 client 只用 CPU 画
 *
 * Step 2 已经证明了 GL -> dmabuf -> 上屏这条链路。Step 3 加的是进程边界，
 * 把 EGL 塞进 client 只会在出错时多一个嫌疑人。
 * TODO(step4): 真实客户端（Mesa）用 GL 画，那时这条路径由标准客户端覆盖。
 *
 * ## 用法
 *
 *   sudo ./step3_dmabuf_ipc --spawn                 一条命令跑完整条链路
 *   sudo ./step3_dmabuf_ipc                         只起 server，等 client 连
 *   ./step3_dmabuf_ipc --role client                另一个终端
 *
 *   --role server|client   默认 server
 *   --spawn                server 自己拉起一个 client（CI 走这条）
 *   --socket <path>        覆盖默认路径（$XDG_RUNTIME_DIR/mini-wayland-0）
 *   --prefer <kind>        server 侧分配路径偏好：auto|scanout|render
 *   --verify[=N]           L2 回读校验前 N 帧（默认 8，0 关闭）
 *   -f <n>                 跑 n 帧后退出
 *   -b <n>                 client 的 buffer 槽位数（默认 3）
 *   -D <path> / -d <name>  KMS 节点
 *   -g <path>              client 走 render 路径时用的 GBM 节点
 *   --fault <case>         故障注入，见下
 *   --dry-run              server 只做到 modeset 的 TEST_ONLY
 *
 * ## 画面上应该看到什么（不要把这几样当成 bug）
 *
 *  - **一条 8 像素宽的橙色竖条**，每帧右移 4 像素。它是撕裂检测器：
 *    没有撕裂时它是一条笔直的竖线，一旦某次提交在扫描中途生效，
 *    这条线会在某个扫描行上左右错开。
 *  - **一条 6 像素高的横带**，每帧下移 3 像素，颜色是该处彩条的反色
 *    （黄->蓝、青->红、绿->品红……）。它检测垂直方向的更新是否连续。
 *    它扫到第 0 行时，画面顶边会出现一条与下方彩条颜色不同的细带 ——
 *    **那是预期的**，不是错位。
 *  - **左上角 32 个像素的灰阶签名块**，肉眼看是一小段深浅不一的点。
 *
 * ## 验收看什么
 *
 *  1. 画面正常、无撕裂，左上角有一条 32 像素的签名带
 *  2. `--verify` 的 L1/L2 全 PASS；不可用时必须明确打印降级，不能静默
 *  3. **稳态每帧 1 次 atomic_commit + 1 次 flip**，
 *     add_fb / prime_* 增量为 0（buffer 一生只注册一次）
 *  4. 退出时两端 fd 计数、HandleCache::live_count()、add_fb/rm_fb 配平
 *  5. 每一条 --fault 都被拒绝，且错误信息能定位到哪个 ioctl / 哪个字段
 */
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <drm_fourcc.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "mw/core/log.hpp"
#include "mw/drm/atomic.hpp"
#include "mw/drm/device.hpp"
#include "mw/drm/dmabuf_map.hpp"
#include "mw/drm/dumb_buffer.hpp"
#include "mw/drm/event.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/property.hpp"
#include "mw/drm/trace.hpp"
#include "mw/gbm/device.hpp"
#include "mw/ipc/channel.hpp"
#include "mw/ipc/error.hpp"
#include "mw/ipc/signature.hpp"
#include "mw/ipc/socket.hpp"
#include "mw/ipc/wire.hpp"

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
    action.sa_flags = 0; // 不设 SA_RESTART：poll 要能被打断
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    // client 先死时 server 会往一个没人读的 socket 上写。
    // sendmsg 带了 MSG_NOSIGNAL，但别的路径未必，统一忽略掉。
    signal(SIGPIPE, SIG_IGN);
}

/// 传给 exec 出来的 client，告诉它 socket 在哪个 fd 上
constexpr const char* kInheritedFdEnv = "MW_STEP3_IPC_FD";

// ---------------------------------------------------------------------------
// 选项
// ---------------------------------------------------------------------------

enum class Role { Server, Client };

/**
 * @brief 故障注入用例
 *
 * 每一条都必须：server 拒绝、给出能定位的错误、不崩溃、不上屏、不泄漏 fd。
 * 这一节是本 step 最重要的验收内容 —— "返回码只证明接口被接受了"
 * 那条原则的直接落实（lessons.md L-1）。
 */
enum class Fault {
    None,
    BadStride,   ///< stride 报小一半
    BadOffset,   ///< offset 超出 buffer
    MissingFd,   ///< num_planes=2 但只发 1 个 fd
    ExtraFd,     ///< 多发一个 fd
    NotDmabuf,   ///< 发一个不是 dma_buf 的 fd
    TinyBuffer,  ///< 声明全屏尺寸但实际只分配一小块
    BadModifier, ///< 报一个 plane 不支持的 modifier
    StaleHeader, ///< 改 abi_version
    HalfMessage, ///< body 截断
};

struct FaultName {
    const char* name;
    Fault fault;
};

constexpr FaultName kFaults[] = {
    {"bad-stride", Fault::BadStride},   {"bad-offset", Fault::BadOffset},
    {"missing-fd", Fault::MissingFd},   {"extra-fd", Fault::ExtraFd},
    {"not-dmabuf", Fault::NotDmabuf},   {"tiny-buffer", Fault::TinyBuffer},
    {"bad-modifier", Fault::BadModifier}, {"stale-header", Fault::StaleHeader},
    {"half-message", Fault::HalfMessage},
};

struct Options {
    Role role = Role::Server;
    bool spawn = false;
    std::string socket_path{};
    ipc::SourceKindWire prefer = ipc::SourceKindWire::Any;
    uint32_t verify_frames = 8;
    uint64_t frame_limit = 0;
    uint32_t buffer_count = 3;
    const char* driver_name = nullptr;
    const char* device_path = nullptr;
    const char* gbm_node = nullptr;
    Fault fault = Fault::None;
    bool dry_run = false;
};

void print_usage(const char* argv0) {
    std::printf("usage: %s [options]\n", argv0);
    std::printf("  --role server|client   role of this process (default server)\n");
    std::printf("  --spawn                server forks a client of its own\n");
    std::printf("  --socket <path>        socket path (default $XDG_RUNTIME_DIR/mini-wayland-0)\n");
    std::printf("  --prefer <kind>        allocation preference: auto|scanout|render\n");
    std::printf("  --verify[=n]           content check on the first n frames (default 8)\n");
    std::printf("  -f <n>                 stop after n frames\n");
    std::printf("  -b <n>                 client buffer slots (default 3)\n");
    std::printf("  -d <name>              open the KMS node by DRM driver name\n");
    std::printf("  -D <path>              open a specific KMS node\n");
    std::printf("  -g <path>              node for GBM when the client allocates there\n");
    std::printf("  --fault <case>         inject a protocol fault (client side):\n");
    std::printf("                         bad-stride bad-offset missing-fd extra-fd\n");
    std::printf("                         not-dmabuf tiny-buffer bad-modifier\n");
    std::printf("                         stale-header half-message\n");
    std::printf("  --dry-run              server stops after the modeset TEST_ONLY\n");
    std::printf("  -h                     this help\n");
    std::printf("\nthe server needs DRM master:\n");
    std::printf("  sudo systemctl stop lightdm    # or switch to a bare tty\n");
    std::printf("\nenvironment: MW_LOG=error|warn|info|debug|trace, MW_LOG_TIME=1\n");
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
        } else if (arg == "--role") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            if (std::strcmp(value, "server") == 0) {
                options.role = Role::Server;
            } else if (std::strcmp(value, "client") == 0) {
                options.role = Role::Client;
            } else {
                std::fprintf(stderr, "unknown role '%s'\n", value);
                return false;
            }
        } else if (arg == "--spawn") {
            options.spawn = true;
        } else if (arg == "--socket") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            options.socket_path = value;
        } else if (arg == "--prefer") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            if (std::strcmp(value, "auto") == 0) {
                options.prefer = ipc::SourceKindWire::Any;
            } else if (std::strcmp(value, "scanout") == 0) {
                options.prefer = ipc::SourceKindWire::ScanoutDevice;
            } else if (std::strcmp(value, "render") == 0) {
                options.prefer = ipc::SourceKindWire::RenderDevice;
            } else {
                std::fprintf(stderr, "unknown preference '%s'\n", value);
                return false;
            }
        } else if (arg == "--verify") {
            options.verify_frames = 8;
        } else if (arg.rfind("--verify=", 0) == 0) {
            options.verify_frames = static_cast<uint32_t>(std::strtoul(arg.c_str() + 9, nullptr, 10));
        } else if (arg == "--dry-run") {
            options.dry_run = true;
        } else if (arg == "--fault") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            bool found = false;
            for (const FaultName& entry : kFaults) {
                if (std::strcmp(entry.name, value) == 0) {
                    options.fault = entry.fault;
                    found = true;
                    break;
                }
            }
            if (! found) {
                std::fprintf(stderr, "unknown fault '%s'\n", value);
                return false;
            }
        } else if (arg == "-f") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            options.frame_limit = std::strtoull(value, nullptr, 10);
        } else if (arg == "-b") {
            const char* value = nullptr;
            if (! next(&value)) return false;
            options.buffer_count = static_cast<uint32_t>(std::strtoul(value, nullptr, 10));
        } else if (arg == "-d") {
            if (! next(&options.driver_name)) return false;
        } else if (arg == "-D") {
            if (! next(&options.device_path)) return false;
        } else if (arg == "-g") {
            if (! next(&options.gbm_node)) return false;
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", arg.c_str());
            print_usage(argv[0]);
            return false;
        }
    }

    if (options.socket_path.empty()) {
        options.socket_path = ipc::default_socket_path(0);
    }
    if (options.buffer_count < 2 || options.buffer_count > 4) {
        std::fprintf(stderr, "-b must be between 2 and 4\n");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 画一帧（两侧共用：client 画内容，server 画兜底帧）
// ---------------------------------------------------------------------------

/**
 * @brief 画面图案 + 左上角的签名带
 *
 * 图案要能用肉眼判断三件事：颜色顺序、竖条平移（撕裂会让它断开）、
 * 横带下移。签名带写在第 0 行的前 32 个像素，**必须在最前面** ——
 * 放在中间的话 stride 算错时它会跟着一起漂移，反而检测不出来。
 */
void draw_pattern(span<uint8_t> pixels, Size size, uint32_t stride, uint64_t frame,
                  uint32_t base_color) {
    static const uint32_t kBars[] = {
        0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
        0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u,
    };
    constexpr uint32_t kBarCount = sizeof(kBars) / sizeof(kBars[0]);

    std::vector<uint32_t> row(size.width);
    const uint32_t bar_width = size.width / kBarCount;
    const auto marker_x = static_cast<uint32_t>((frame * 4u) % size.width);
    const auto band_y = static_cast<uint32_t>((frame * 3u) % size.height);

    for (uint32_t x = 0; x < size.width; ++x) {
        const uint32_t bar = bar_width > 0u ? (x / bar_width) % kBarCount : 0u;
        uint32_t color = kBars[bar] ^ base_color;
        if (x >= marker_x && x < marker_x + 8u) {
            color = 0x00ff8000u;
        }
        row[x] = color;
    }

    const size_t row_bytes = static_cast<size_t>(size.width) * 4u;
    for (uint32_t y = 0; y < size.height; ++y) {
        const size_t offset = static_cast<size_t>(y) * stride;
        if (offset + row_bytes > pixels.size()) {
            break; // 映射比 stride*height 短，别越界
        }
        if (y >= band_y && y < band_y + 6u) {
            for (uint32_t x = 0; x < size.width; ++x) {
                const uint32_t inverted = ~row[x] & 0x00ffffffu;
                std::memcpy(pixels.data() + offset + (static_cast<size_t>(x) * 4u), &inverted, 4);
            }
        } else {
            std::memcpy(pixels.data() + offset, row.data(), row_bytes);
        }
    }
}

uint32_t random_nonce() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_nsec) ^ (static_cast<uint32_t>(::getpid()) << 16);
}

// ===========================================================================
// client
// ===========================================================================

/// client 侧的一块 buffer。两条分配路径的持有物不同，但对外只是"一块能画的内存"。
struct ClientBuffer {
    ipc::BufferId id{0};

    DumbBuffer dumb{};    ///< ScanoutDevice 路径
    gbm::Buffer bo{};     ///< RenderDevice 路径
    void* gbm_cookie = nullptr;

    DmabufDesc desc{};
    span<uint8_t> pixels{};
    bool busy = false; ///< 已提交、尚未收到 BUFFER_RELEASE
};

struct ClientState {
    ipc::Channel channel{};
    std::vector<ClientBuffer> buffers{};
    Size size{};
    Format format{DRM_FORMAT_XRGB8888};
    uint32_t nonce = 0;

    uint64_t frames_committed = 0;
    uint64_t frames_done = 0;
    /// **有多少帧曾因为没有空闲槽位而等过**（每帧至多计一次）。
    /// 早先按循环次数计，结果是"602 提交 / 597 等待"这种看着吓人、
    /// 其实只反映了每帧收两条消息的数字 —— 一个会误导人的计数器
    /// 比没有计数更糟（lessons.md L-8）。
    uint64_t frames_with_wait = 0;
};

/**
 * @brief 从显示节点分配（dumb）
 *
 * client 不是 DRM master。从 UAPI 看 `CREATE_DUMB` 既没有 `DRM_MASTER`
 * 也没有 `DRM_AUTH` 标志，主节点上任何打开者都能建 —— 但按项目惯例
 * 不采信推断，失败时把 errno 报回去让 server 换推荐。
 */
Result<ClientBuffer> allocate_scanout(BorrowedFd kms_fd, ipc::BufferId id, Size size,
                                      Format format) {
    ClientBuffer buffer{};
    buffer.id = id;
    buffer.dumb = TRY(DumbBuffer::create(kms_fd, size, format));
    buffer.pixels = buffer.dumb.bytes();

    // 导出成只读：server 只扫描，不写。多给的权限迟早会被用上。
    UniqueFd fd = TRY(export_dmabuf(kms_fd, buffer.dumb.handle(), PrimeAccess::ReadOnly));

    buffer.desc.size = size;
    buffer.desc.format = format;
    buffer.desc.modifier = kModifierInvalid; // dumb 没有 modifier 可谈
    buffer.desc.num_planes = 1;
    buffer.desc.offsets[0] = 0;
    buffer.desc.strides[0] = buffer.dumb.pitch();
    buffer.desc.fds[0] = std::move(fd);
    return Ok(std::move(buffer));
}

/// 从渲染节点分配（GBM）。CPU 要能写，所以只可能是线性排布。
Result<ClientBuffer> allocate_render(const gbm::Device& device, ipc::BufferId id, Size size,
                                     Format format, span<const Modifier> modifiers) {
    ClientBuffer buffer{};
    buffer.id = id;
    buffer.bo = TRY(device.allocate(size, format, modifiers,
                                    gbm::Usage::Scanout | gbm::Usage::CpuWrite));
    buffer.desc = TRY(buffer.bo.export_dmabuf());
    // **不在这里建立长期映射**，理由见 begin_draw()。
    return Ok(std::move(buffer));
}

/**
 * @brief 取得这一帧可写的像素
 *
 * 两条分配路径在这里的语义完全不同，**这一点花了一次黑屏才认清**：
 *
 *  - dumb：`mmap` 出来的就是 GEM 对象背后那块内存，映射一次可以一直用
 *  - GBM：`gbm_bo_map()` **不保证**给的是 bo 自己的内存。实现可以给一块
 *    staging buffer，真正的拷回发生在 `gbm_bo_unmap()`。长期持有映射、
 *    从不 unmap，就等于每一帧都写进了一块永远不会被送回去的影子内存 ——
 *    而分配、导出、导入、addfb2、atomic commit **全部返回成功**，
 *    屏幕上什么都没有。
 *
 * 所以 GBM 路径每帧 map / unmap。代价是每帧两次调用，
 * 对 CPU 绘制的 client 可以接受；真实客户端用 GL 画，不走这条路。
 *
 * step2 的 `--unmap-each-frame` 开关就是为这个假设准备的诊断手段，
 * 那时它只是一个"如果画面不对就试试"的开关，现在它是一条实测结论。
 */
Result<span<uint8_t>> begin_draw(ClientBuffer& buffer) {
    if (buffer.bo.valid()) {
        return gbm::map_write(buffer.bo, buffer.gbm_cookie);
    }
    return Ok(buffer.pixels);
}

/// 结束绘制。GBM 路径上这一步才是真正把像素送进 bo 的地方。
void end_draw(ClientBuffer& buffer) {
    if (buffer.bo.valid() && buffer.gbm_cookie != nullptr) {
        gbm::unmap(buffer.bo, buffer.gbm_cookie);
        buffer.gbm_cookie = nullptr;
    }
}

/// 故意破坏 CREATE_BUFFER 的字段。**只改线格式，不改真实 buffer** ——
/// 要测的是 server 能不能发现两者不符。
void apply_fault_to_body(Fault fault, ipc::CreateBufferBody& body) {
    switch (fault) {
        case Fault::BadStride:
            body.strides[0] = body.strides[0] / 2;
            break;
        case Fault::BadOffset:
            body.offsets[0] = body.strides[0] * body.height;
            break;
        case Fault::MissingFd:
            // 声明两个平面但只发一个 fd
            body.num_planes = 2;
            body.strides[1] = body.strides[0];
            break;
        case Fault::ExtraFd:
            // **声明不动**，多发一个 fd。要打的是"fd 数与 num_planes 不符"，
            // 早先顺手把 num_planes 也改成 2，结果 fd 数反而对上了，
            // 拒绝理由变成"plane 1 stride 为零" —— 拒是拒了，
            // 但测的不是想测的那条。故障注入自己也会注入错。
            break;
        case Fault::BadModifier:
            // 一个不可能被任何 plane 接受的私有 modifier
            body.set_modifier(static_cast<Modifier>(0x00ff'0000'dead'beefULL));
            break;
        default:
            break;
    }
}

/// 绕过 Channel 手工发一条坏消息（stale-header / half-message 用）
Status send_raw_broken(BorrowedFd sock, Fault fault, const ipc::CreateBufferBody& body,
                       BorrowedFd payload_fd) {
    ipc::MessageHeader header{};
    header.type = static_cast<uint16_t>(ipc::MsgType::CreateBuffer);
    header.body_size = sizeof(body);
    header.fd_count = 1;
    if (fault == Fault::StaleHeader) {
        header.abi_version = ipc::kWireAbiVersion + 1;
    }

    const size_t body_bytes = (fault == Fault::HalfMessage) ? sizeof(body) / 2 : sizeof(body);

    iovec iov[2];
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);
    iov[1].iov_base = const_cast<ipc::CreateBufferBody*>(&body);
    iov[1].iov_len = body_bytes;

    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    const int raw = payload_fd.get();
    std::memcpy(CMSG_DATA(cmsg), &raw, sizeof(raw));

    if (::sendmsg(sock.get(), &msg, MSG_NOSIGNAL) < 0) {
        return sys_err("sendmsg (fault injection)");
    }
    return Ok();
}

Status client_send_create(ClientState& state, ClientBuffer& buffer, Fault fault) {
    ipc::CreateBufferBody body = ipc::make_create_buffer(buffer.id, buffer.desc);
    apply_fault_to_body(fault, body);

    if (fault == Fault::TinyBuffer) {
        // buffer 真的只有 64x64，但对外声明成全屏。内核在 addfb2 时会拿
        // GEM 对象的真实大小去比，这一条测的就是**我们没有自己算一遍**
        // 那套可能与内核不一致的尺寸规则（见 wire.hpp 的 validate 注释）。
        body.width = state.size.width;
        body.height = state.size.height;
        body.strides[0] = state.size.width * 4u;
    }

    if (fault == Fault::StaleHeader || fault == Fault::HalfMessage) {
        return send_raw_broken(state.channel.fd(), fault, body, buffer.desc.fds[0].borrow());
    }

    BorrowedFd fds[ipc::kMaxMessageFds]{};
    uint32_t count = 0;
    fds[count++] = buffer.desc.fds[0].borrow();

    UniqueFd extra{};
    if (fault == Fault::ExtraFd) {
        // 多发一个真的 dmabuf fd：数量不符，且多出来的那个必须被 server 关掉
        extra = TRY(buffer.desc.fds[0].duplicate());
        fds[count++] = extra.borrow();
    } else if (fault == Fault::NotDmabuf) {
        // 一个合法但不是 dma_buf 的 fd。PRIME_FD_TO_HANDLE 应当拒绝它，
        // 而不是把它当成一块内存。
        extra = TRY(UniqueFd::open("/dev/null", O_RDONLY | O_CLOEXEC));
        fds[0] = extra.borrow();
    }
    // MissingFd：num_planes 已被改成 2，但这里仍然只发 1 个

    return state.channel.send(body, span<const BorrowedFd>(fds, count));
}

int run_client(const Options& options) {
    LOG_INFO("client: starting");

    // --spawn 时 socket 是从父进程继承来的，不走文件系统
    ipc::Channel channel;
    if (const char* inherited = std::getenv(kInheritedFdEnv); inherited != nullptr) {
        const int fd = static_cast<int>(std::strtol(inherited, nullptr, 10));
        LOG_INFO("client: using inherited socket fd {}", fd);
        channel = ipc::Channel(UniqueFd(fd));
    } else {
        auto sock = ipc::connect_seqpacket(options.socket_path);
        if (! sock) {
            log_error_object(sock.error(), "client: cannot connect");
            return 1;
        }
        channel = ipc::Channel(std::move(sock).value());
    }

    // **声明顺序即析构逆序。** 分配设备必须先于 ClientState 声明，
    // 否则设备 fd 会先关掉，随后 DumbBuffer 的析构对着一个已关闭的 fd
    // 下 DESTROY_DUMB，报"GEM object leaked" —— 内核其实在 close 时就把
    // 对象回收了，泄漏是假的，但记账是真错的。这个坑 step2 的 demo 里
    // 已经写过一次注释，我还是踩了。
    UniqueFd kms_fd{};
    gbm::Device gbm_device;

    ClientState state;
    state.channel = std::move(channel);
    state.nonce = random_nonce();

    // ---- 握手 ----
    ipc::HelloBody hello{};
    hello.supported_sources = (1u << static_cast<uint32_t>(ipc::SourceKindWire::ScanoutDevice)) |
                              (1u << static_cast<uint32_t>(ipc::SourceKindWire::RenderDevice));
    if (auto status = state.channel.send(hello); ! status) {
        log_error_object(status.error(), "client: HELLO");
        return 1;
    }

    ipc::Message message;
    auto recv_status = state.channel.recv(message);
    if (! recv_status || recv_status.value() != ipc::RecvStatus::Message) {
        LOG_ERROR("client: no HELLO_ACK from the server");
        return 1;
    }
    const auto* ack = message.body_as<ipc::HelloAckBody>();
    if (ack == nullptr) {
        LOG_ERROR("client: expected HELLO_ACK, got {}", message.to_string());
        return 1;
    }

    state.size = Size{ack->width, ack->height};
    state.format = static_cast<Format>(ack->format);
    const auto source = static_cast<ipc::SourceKindWire>(ack->recommended_source);
    std::vector<Modifier> modifiers;
    for (uint32_t i = 0; i < ack->modifier_count && i < ipc::kMaxAdvertisedModifiers; ++i) {
        modifiers.push_back(static_cast<Modifier>(ack->modifiers[i]));
    }

    LOG_INFO("client: server recommends {} for {}x{} {}, {} modifier(s){}",
             ipc::to_string(source), state.size.width, state.size.height,
             to_string(state.format), modifiers.size(), ack->truncated != 0 ? " (truncated)" : "");

    // ---- 按推荐分配 ----
    if (source == ipc::SourceKindWire::RenderDevice) {
        if (options.gbm_node == nullptr) {
            LOG_ERROR("client: the server recommends allocating on a render node but no -g given");
            return 1;
        }
        auto device = gbm::Device::open(options.gbm_node);
        if (! device) {
            log_error_object(device.error(), "client: cannot open the GBM node");
            return 1;
        }
        gbm_device = std::move(device).value();
    } else {
        if (options.device_path == nullptr) {
            // 不猜。猜错的后果是在另一个设备上分配，然后 server 那边
            // PRIME 导入失败或者更糟 —— 导入成功但扫出垃圾。
            LOG_ERROR("client: the server recommends allocating on the display node but no -D "
                      "was given; pass the same node the server opened");
            return 1;
        }
        auto opened = UniqueFd::open(options.device_path, O_RDWR | O_CLOEXEC);
        if (! opened) {
            log_error_object(opened.error(), "client: cannot open the KMS node");
            return 1;
        }
        kms_fd = std::move(opened).value();
    }

    for (uint32_t i = 0; i < options.buffer_count; ++i) {
        const auto id = static_cast<ipc::BufferId>(i + 1);
        // tiny-buffer 故意分配一块放不下声明尺寸的内存
        const Size alloc_size =
            (options.fault == Fault::TinyBuffer) ? Size{64, 64} : state.size;
        auto buffer = (source == ipc::SourceKindWire::RenderDevice)
                          ? allocate_render(gbm_device, id, alloc_size, state.format, modifiers)
                          : allocate_scanout(kms_fd.borrow(), id, alloc_size, state.format);
        if (! buffer) {
            log_error_object(buffer.error(), "client: allocation failed");
            // 分配失败不是协议错误，但 server 需要知道 —— 否则它会一直等
            (void) state.channel.send_error(ipc::WireError::NotSupported,
                                            "client could not allocate a buffer");
            return 1;
        }
        state.buffers.push_back(std::move(buffer).value());
        LOG_INFO("client: buffer {} ready, stride={} modifier={}", i + 1,
                 state.buffers.back().desc.strides[0],
                 to_string(state.buffers.back().desc.modifier));
    }

    // CREATE_BUFFER 每块只发一次。之后每帧只发 buffer_id ——
    // 稳态里 server 侧的 prime_fd_to_handle / add_fb 增量因此为 0。
    for (ClientBuffer& buffer : state.buffers) {
        if (auto status = client_send_create(state, buffer, options.fault); ! status) {
            log_error_object(status.error(), "client: CREATE_BUFFER");
            return 1;
        }
        if (options.fault != Fault::None) {
            LOG_INFO("client: injected fault, only one CREATE_BUFFER was sent");
            break;
        }
    }

    if (options.fault != Fault::None) {
        // 故障注入模式：等 server 的 ERROR，然后退出。
        // **不上屏是预期结果**，验收看的是错误信息说到什么粒度。
        //
        // 带超时，因为"server 什么都没说"是这里最重要的一种结果：
        // 它意味着注入的故障**没有被发现**。早先写成无限等待，
        // 于是这种情况表现为整个程序挂住 —— 一个失败的测试看起来像
        // 一个坏掉的测试框架，人的第一反应会是去查框架而不是查判据。
        constexpr int kFaultReplyTimeoutMs = 3000;
        pollfd pfd{};
        pfd.fd = state.channel.fd().get();
        pfd.events = POLLIN;
        const int ready = ::poll(&pfd, 1, kFaultReplyTimeoutMs);
        if (ready == 0) {
            LOG_ERROR("client: FAULT NOT DETECTED -- the server accepted the injected fault and "
                      "said nothing within {} ms",
                      kFaultReplyTimeoutMs);
            return 1;
        }
        if (ready < 0) {
            log_error_object(sys_err("poll").error(), "client: waiting for the fault reply");
            return 1;
        }

        ipc::Message reply;
        auto status = state.channel.recv(reply);
        if (! status) {
            log_error_object(status.error(), "client: recv after fault");
            return 1;
        }
        if (status.value() == ipc::RecvStatus::Closed) {
            LOG_INFO("client: server closed the connection after the injected fault");
            return 0;
        }
        if (const auto* error = reply.body_as<ipc::ErrorBody>(); error != nullptr) {
            LOG_INFO("client: server rejected it: [{}] {}",
                     ipc::to_string(static_cast<ipc::WireError>(error->code)), error->detail);
            return 0;
        }
        LOG_ERROR("client: expected an ERROR after the injected fault, got {}", reply.to_string());
        return 1;
    }

    // ---- 帧循环 ----
    LOG_INFO("client: entering the frame loop");
    uint64_t frame = 0;
    bool waited_this_frame = false;
    while (g_should_stop == 0) {
        if (options.frame_limit != 0 && state.frames_done >= options.frame_limit) {
            break;
        }

        ClientBuffer* free_buffer = nullptr;
        for (ClientBuffer& buffer : state.buffers) {
            if (! buffer.busy) {
                free_buffer = &buffer;
                break;
            }
        }

        if (free_buffer == nullptr) {
            // 没有空闲槽位就**阻塞等 BUFFER_RELEASE，不新分配** ——
            // 新分配能让程序跑下去，但会掩盖 release 时机写错的问题。
            if (! waited_this_frame) {
                state.frames_with_wait += 1;
                waited_this_frame = true;
            }
        } else {
            ipc::FrameSignature sig{};
            sig.run_nonce = state.nonce;
            sig.frame_seq = static_cast<uint32_t>(frame);
            sig.width = state.size.width;
            sig.height = state.size.height;
            sig.stride = free_buffer->desc.strides[0];
            sig.format = static_cast<uint32_t>(state.format);
            sig.modifier_lo = static_cast<uint32_t>(static_cast<uint64_t>(free_buffer->desc.modifier) &
                                                    0xffffffffULL);

            auto pixels = begin_draw(*free_buffer);
            if (! pixels) {
                log_error_object(pixels.error(), "client: cannot map the buffer for drawing");
                break;
            }
            draw_pattern(pixels.value(), state.size, free_buffer->desc.strides[0], frame, 0);
            if (ipc::signature_supported(state.format)) {
                if (auto status = ipc::write_signature(pixels.value(), sig); ! status) {
                    log_error_object(status.error(), "client: write_signature");
                }
            } else if (frame == 0) {
                LOG_WARN("client: {} cannot carry a frame signature -- this run has no L1/L2 "
                         "content check",
                         to_string(state.format));
            }

            // unmap 必须在 COMMIT 之前：GBM 路径上像素是在这一步才落地的。
            end_draw(*free_buffer);

            ipc::CommitBody commit{};
            commit.buffer_id = ipc::to_u32(free_buffer->id);
            commit.frame_seq = static_cast<uint32_t>(frame);
            commit.signature_crc = ipc::signature_supported(state.format) ? ipc::signature_crc(sig)
                                                                          : 0;
            if (auto status = state.channel.send(commit); ! status) {
                log_error_object(status.error(), "client: COMMIT");
                break;
            }
            free_buffer->busy = true;
            state.frames_committed += 1;
            waited_this_frame = false;
            frame += 1;
        }

        ipc::Message reply;
        auto status = state.channel.recv(reply);
        if (! status) {
            log_error_object(status.error(), "client: recv");
            break;
        }
        if (status.value() == ipc::RecvStatus::Closed) {
            LOG_INFO("client: the server closed the connection");
            break;
        }
        if (const auto* release = reply.body_as<ipc::BufferReleaseBody>(); release != nullptr) {
            for (ClientBuffer& buffer : state.buffers) {
                if (ipc::to_u32(buffer.id) == release->buffer_id) {
                    buffer.busy = false;
                }
            }
        } else if (const auto* done = reply.body_as<ipc::FrameDoneBody>(); done != nullptr) {
            state.frames_done += 1;
        } else if (const auto* error = reply.body_as<ipc::ErrorBody>(); error != nullptr) {
            LOG_ERROR("client: server error [{}]: {}",
                      ipc::to_string(static_cast<ipc::WireError>(error->code)), error->detail);
            break;
        }
    }

    LOG_INFO("client: {} committed, {} presented, {} frame(s) waited for a free slot",
             state.frames_committed, state.frames_done, state.frames_with_wait);
    LOG_INFO("client: {}", state.channel.counters_to_string());
    // 流水线深度：server 最多同时持有 3 块 —— 正在扫描的、已提交等 flip 的、
    // 已收到 COMMIT 还没提交的。所以 client 手里的空闲槽位是 (b - 3)。
    // b=3 时稳态恒为 0，每帧都要等一次 release，这是**预期行为**而不是故障：
    // 帧率不受影响（server 手里始终有下一帧），只是 client 无法把绘制
    // 和 server 的工作重叠起来。想要重叠就 -b 4。
    if (state.buffers.size() <= 3 && state.frames_with_wait > 0) {
        LOG_INFO("client: with {} slot(s) the server can hold all of them (on-screen + submitted "
                 "+ pending), so waiting once per frame is expected; use -b 4 to overlap drawing "
                 "with presentation",
                 state.buffers.size());
    }

    for (ClientBuffer& buffer : state.buffers) {
        end_draw(buffer); // 正常路径下已经解开了，这里只兜底
    }
    return 0;
}

// ===========================================================================
// server
// ===========================================================================

/// server 侧的一块 client buffer：导入一次，fb 一直留着
struct ImportedBuffer {
    DmabufDesc desc{};
    Framebuffer fb{};
    uint32_t last_frame_seq = 0;
};

struct VerifyState {
    uint32_t remaining = 0;
    bool mapping_unavailable = false;
    uint32_t passed = 0;
    uint32_t failed = 0;
};

/**
 * @brief L2：server 侧读回 client 写进 dmabuf 的签名
 *
 * 这是 Step 3 唯一能抓住"fd 传错 / offset 与 stride 序列化错位 /
 * 收到的是上一帧"这一类错误的判据。四层 ioctl 校验没有一层碰过像素。
 */
void verify_frame(VerifyState& verify, const ImportedBuffer& buffer, const ipc::CommitBody& commit) {
    if (verify.remaining == 0 || verify.mapping_unavailable) {
        return;
    }
    verify.remaining -= 1;

    auto mapping = DmabufMapping::create(buffer.desc.fds[0].borrow(), MapAccess::Read);
    if (! mapping) {
        // 导出方不支持 mmap 是**能力问题**，不是故障。但必须打印 ——
        // 静默降级等于让验收少一层而没人知道。
        verify.mapping_unavailable = true;
        LOG_WARN("server: content verification unavailable: {}", mapping.error().message);
        LOG_WARN("server: the exporter does not allow CPU mapping of this dmabuf; frames will go "
                 "on screen unverified (this is the L2 gap, record it in docs/env.md)");
        return;
    }

    auto access = mapping.value().begin_access();
    if (! access) {
        LOG_WARN("server: DMA_BUF_IOCTL_SYNC failed: {}", access.error().message);
    }

    auto sig = ipc::read_signature(mapping.value().bytes());
    if (! sig) {
        verify.failed += 1;
        LOG_ERROR("server: frame {} failed the content check: {}", commit.frame_seq,
                  sig.error().message);
        return;
    }

    const uint32_t crc = ipc::signature_crc(sig.value());
    if (commit.signature_crc != 0 && crc != commit.signature_crc) {
        verify.failed += 1;
        LOG_ERROR("server: frame {} signature crc mismatch: client says 0x{:x}, memory says 0x{:x}",
                  commit.frame_seq, commit.signature_crc, crc);
        return;
    }
    if (sig.value().frame_seq != commit.frame_seq) {
        verify.failed += 1;
        LOG_ERROR("server: frame {} carries the signature of frame {} -- the buffer was reused "
                  "before it was released",
                  commit.frame_seq, sig.value().frame_seq);
        return;
    }
    if (sig.value().stride != buffer.desc.strides[0]) {
        verify.failed += 1;
        LOG_ERROR("server: frame {} stride disagreement: signature says {}, wire said {}",
                  commit.frame_seq, sig.value().stride, buffer.desc.strides[0]);
        return;
    }

    verify.passed += 1;
    LOG_INFO("server: frame {} content check PASS {}", commit.frame_seq,
             ipc::to_string(sig.value()));
}

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
        return;
    }
    request.reset();
    for (const PlaneType type : {PlaneType::Primary, PlaneType::Overlay, PlaneType::Cursor}) {
        for (const PlaneId id : device.planes_for_crtc(path.crtc, type)) {
            const Plane* plane = device.plane(id);
            if (plane != nullptr) {
                (void) request.disable_plane(*plane);
            }
        }
    }
    (void) request.disable_crtc(*crtc);
    (void) request.bind_connector(*connector, kNoCrtc);
    if (auto status = request.commit(CommitFlags::AllowModeset); ! status) {
        log_error_object(status.error(), "teardown commit");
    } else {
        LOG_INFO("teardown committed");
    }
}

std::vector<Modifier> plane_modifiers(const Plane& plane, Format format) {
    std::vector<Modifier> out;
    for (const FormatModifier& entry : plane.formats) {
        if (entry.format == format && entry.modifier != kModifierInvalid) {
            out.push_back(entry.modifier);
        }
    }
    return out;
}

/// fork + exec 一个自己当 client。socket 那一端要显式清掉 CLOEXEC。
Result<pid_t> spawn_client(UniqueFd client_end, const Options& options,
                           const std::string& kms_path) {
    const pid_t pid = ::fork();
    if (pid < 0) {
        return sys_err("fork");
    }
    if (pid > 0) {
        return Ok(pid);
    }

    // 子进程。这是全工程唯一故意不带 CLOEXEC 的 fd —— 不清掉这个标志，
    // exec 之后子进程手里什么都没有。
    const int fd = client_end.release();
    const int flags = ::fcntl(fd, F_GETFD, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
        std::fprintf(stderr, "spawn: cannot clear FD_CLOEXEC\n");
        _exit(127);
    }
    char value[16];
    std::snprintf(value, sizeof(value), "%d", fd);
    setenv(kInheritedFdEnv, value, 1);

    std::vector<std::string> args{"step3_dmabuf_ipc", "--role", "client"};
    if (options.frame_limit != 0) {
        args.push_back("-f");
        args.push_back(std::to_string(options.frame_limit));
    }
    args.push_back("-b");
    args.push_back(std::to_string(options.buffer_count));
    // **传解析后的真实路径，不是命令行给的那个**：server 可能是按 driver name
    // 或"第一个 KMS 节点"打开的，client 猜不出来，猜错就是在别的设备上分配。
    args.push_back("-D");
    args.push_back(kms_path);
    if (options.gbm_node != nullptr) {
        args.push_back("-g");
        args.push_back(options.gbm_node);
    }
    if (options.fault != Fault::None) {
        for (const FaultName& entry : kFaults) {
            if (entry.fault == options.fault) {
                args.push_back("--fault");
                args.push_back(entry.name);
            }
        }
    }

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (std::string& arg : args) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    // _exit 而不是 exit：后者会 flush 从父进程继承来的 stdio 缓冲，
    // 同一行日志会打两遍（lessons.md L-3）。
    execv("/proc/self/exe", argv.data());
    std::fprintf(stderr, "spawn: execv failed: %s\n", std::strerror(errno));
    _exit(127);
}

int run_server(const Options& options) {
    auto device_result = options.device_path != nullptr
                             ? Device::open(std::string(options.device_path))
                             : (options.driver_name != nullptr
                                    ? Device::open_by_driver(std::string(options.driver_name))
                                    : Device::open_first_kms());
    if (! device_result) {
        log_error_object(device_result.error(), "server: cannot open a KMS device");
        return 1;
    }
    Device device = std::move(device_result).value();

    auto path_result = device.pick_output();
    if (! path_result) {
        log_error_object(path_result.error(), "server: cannot pick an output");
        return 1;
    }
    const OutputPath path = std::move(path_result).value();
    LOG_INFO("server: {}", path.to_string());

    const Plane* primary = device.plane(path.primary_plane);
    if (primary == nullptr) {
        LOG_ERROR("server: the chosen primary plane is not in the snapshot");
        return 1;
    }

    const Format format{DRM_FORMAT_XRGB8888};
    if (! primary->supports_format(format)) {
        LOG_ERROR("server: the primary plane does not advertise {}", to_string(format));
        return 1;
    }

    // ---- 兜底帧 ----
    // client 猝死时要先切回它，才能安全地销毁 client 的 fb。
    // 顺带解决了"启动瞬间没有内容可显示"。
    HandleCache cache(device.fd());
    auto fallback_result = DumbBuffer::create(device.fd(), path.size(), format);
    if (! fallback_result) {
        log_error_object(fallback_result.error(), "server: cannot allocate the fallback buffer");
        return 1;
    }
    DumbBuffer fallback = std::move(fallback_result).value();
    if (auto status = fallback.register_framebuffer(); ! status) {
        log_error_object(status.error(), "server: cannot register the fallback framebuffer");
        return 1;
    }
    draw_pattern(fallback.bytes(), path.size(), fallback.pitch(), 0, 0x00202020u);

    auto master_result = device.acquire_master();
    if (! master_result) {
        LOG_ERROR("server: {}", device.master_diagnosis());
        return 1;
    }
    MasterGuard master = std::move(master_result).value();

    auto blob_result = PropertyBlob::create(device.fd(), &path.mode.raw, sizeof(path.mode.raw));
    if (! blob_result) {
        log_error_object(blob_result.error(), "server: cannot create the mode blob");
        return 1;
    }
    const PropertyBlob mode_blob = std::move(blob_result).value();

    AtomicRequest request(device.fd());
    if (auto status = do_modeset(device, path, request, mode_blob, fallback.fb_id()); ! status) {
        log_error_object(status.error(), "server: modeset");
        return 1;
    }
    if (options.dry_run) {
        LOG_INFO("server: --dry-run, committing nothing");
        return 0;
    }
    if (auto status = request.commit(CommitFlags::AllowModeset); ! status) {
        log_error_object(status.error(), "server: modeset commit");
        return 1;
    }
    LOG_INFO("server: modeset committed, showing the fallback frame");

    // ---- 等 client ----
    ipc::ListeningSocket listener;
    UniqueFd client_end;
    pid_t child = -1;

    if (options.spawn) {
        auto pair = ipc::make_socket_pair();
        if (! pair) {
            log_error_object(pair.error(), "server: socketpair");
            return 1;
        }
        auto ends = std::move(pair).value();
        client_end = std::move(ends.first);
        auto spawned = spawn_client(std::move(ends.second), options, device.path());
        if (! spawned) {
            log_error_object(spawned.error(), "server: spawn");
            return 1;
        }
        child = spawned.value();
        LOG_INFO("server: spawned client pid {}", child);
    } else {
        auto created = ipc::ListeningSocket::create(options.socket_path);
        if (! created) {
            log_error_object(created.error(), "server: cannot listen");
            return 1;
        }
        listener = std::move(created).value();
        LOG_INFO("server: waiting for a client on {}", listener.path());
        auto accepted = listener.accept();
        if (! accepted) {
            log_error_object(accepted.error(), "server: accept");
            return 1;
        }
        client_end = std::move(accepted).value();
    }

    ipc::Channel channel(std::move(client_end));

    // ---- 握手 ----
    ipc::Message message;
    auto hello_status = channel.recv(message);
    if (! hello_status || hello_status.value() != ipc::RecvStatus::Message ||
        message.body_as<ipc::HelloBody>() == nullptr) {
        LOG_ERROR("server: no HELLO from the client");
        teardown(device, path, request);
        return 1;
    }
    const uint32_t client_sources = message.body_as<ipc::HelloBody>()->supported_sources;

    // **决策在 server 侧**：真实 client 不认识显示设备，它照着合成器发来的
    // tranche 分配。Step 4 换成 linux-dmabuf-feedback 时替换的是传输，
    // 不是数据流向。
    ipc::SourceKindWire recommended = options.prefer;
    if (recommended == ipc::SourceKindWire::Any) {
        // auto：render 优先（更接近真实 client 的形态，降级路径每次都被走到），
        // client 做不到时退回 scanout。
        const bool client_can_render =
            (client_sources & (1u << static_cast<uint32_t>(ipc::SourceKindWire::RenderDevice))) != 0;
        recommended = (client_can_render && options.gbm_node != nullptr)
                          ? ipc::SourceKindWire::RenderDevice
                          : ipc::SourceKindWire::ScanoutDevice;
    }

    ipc::HelloAckBody ack{};
    ack.width = path.size().width;
    ack.height = path.size().height;
    ack.format = static_cast<uint32_t>(format);
    ack.recommended_source = static_cast<uint32_t>(recommended);
    // 原样转发，不排序 —— 排序是 Step 4 的 tranche 策略
    const std::vector<Modifier> modifiers = plane_modifiers(*primary, format);
    for (const Modifier modifier : modifiers) {
        if (ack.modifier_count >= ipc::kMaxAdvertisedModifiers) {
            ack.truncated = 1;
            break;
        }
        ack.modifiers[ack.modifier_count++] = static_cast<uint64_t>(modifier);
    }
    LOG_INFO("server: recommending {} with {} modifier(s)", ipc::to_string(recommended),
             ack.modifier_count);
    if (auto status = channel.send(ack); ! status) {
        log_error_object(status.error(), "server: HELLO_ACK");
        teardown(device, path, request);
        return 1;
    }

    // ---- 帧循环 ----
    std::map<uint32_t, ImportedBuffer> buffers;
    VerifyState verify{options.verify_frames, false, 0, 0};

    uint32_t pending = 0;   ///< client 已 COMMIT、尚未提交给 KMS
    uint32_t submitted = 0; ///< 已提交给 KMS、等 flip
    uint32_t on_screen = 0; ///< 正在被扫描
    uint32_t pending_seq = 0;
    uint32_t submitted_seq = 0;
    bool in_flight = false;
    bool client_gone = false;

    FrameStats frame_stats;
    IoctlStats last_report = stats();
    // 第一个报告窗口里含着 CREATE_BUFFER 的 import + addfb2，那是启动不是稳态。
    // 不区分的话每次运行都会报一条假的 ERROR，而**一条恒定出现的告警等于
    // 没有告警** —— 下次真出问题时没人会当回事。
    bool startup_window = true;
    uint64_t frames = 0;
    uint64_t last_report_frame = 0;
    uint64_t flip_seq = 0;
    timespec last_report_time{};
    clock_gettime(CLOCK_MONOTONIC, &last_report_time);

    LOG_INFO("server: entering the frame loop");

    while (g_should_stop == 0 && ! client_gone) {
        if (options.frame_limit != 0 && frames >= options.frame_limit) {
            break;
        }

        // 有待上屏的 buffer 且上一帧已经翻完 -> 提交
        if (pending != 0 && ! in_flight) {
            auto it = buffers.find(pending);
            if (it != buffers.end()) {
                request.reset();
                if (auto status = request.set_plane(*primary, it->second.fb.id(), path.crtc,
                                                    SrcRect::whole(path.size()),
                                                    CrtcRect::at_origin(path.size()));
                    ! status) {
                    log_error_object(status.error(), "server: set_plane");
                    break;
                }
                if (auto status = request.commit(CommitFlags::Nonblock | CommitFlags::PageFlipEvent,
                                                 frames);
                    ! status) {
                    log_error_object(status.error(), "server: frame commit");
                    request.bisect_rejection(CommitFlags::None);
                    break;
                }
                submitted = pending;
                submitted_seq = pending_seq;
                pending = 0;
                in_flight = true;
            }
        }

        pollfd fds[2];
        fds[0].fd = device.fd().get();
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = channel.fd().get();
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        const int ready = ::poll(fds, 2, 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log_error_object(sys_err("poll").error(), "server: poll");
            break;
        }
        if (ready == 0) {
            LOG_WARN("server: idle for 1s (in_flight={} pending={})", in_flight, pending);
            continue;
        }

        // ---- DRM 事件 ----
        if ((fds[0].revents & POLLIN) != 0) {
            const auto handled = read_events(
                device.fd(), device.caps().timestamp_monotonic, [&](const FlipEvent& event) {
                    frame_stats.record(event);
                    in_flight = false;
                    flip_seq += 1;
                    frames += 1;

                    // **release 的时机**：这一帧翻完之后，被换下来的是上一帧
                    // 那块，不是刚提交的这块。写成后者，双缓冲 client 会在
                    // 正在扫描的 buffer 上作画。
                    const uint32_t released = on_screen;
                    on_screen = submitted;
                    if (released != 0 && released != on_screen) {
                        ipc::BufferReleaseBody body{};
                        body.buffer_id = released;
                        if (auto status = channel.send(body); ! status) {
                            client_gone = true;
                        }
                    }

                    ipc::FrameDoneBody done{};
                    done.frame_seq = submitted_seq;
                    done.flip_seq = static_cast<uint32_t>(flip_seq);
                    // TODO(step7): timestamp_ns 留空，见 wire.hpp
                    if (auto status = channel.send(done); ! status) {
                        client_gone = true;
                    }
                });
            if (! handled) {
                log_error_object(handled.error(), "server: read_events");
                break;
            }
        }

        // ---- client 消息 ----
        if ((fds[1].revents & (POLLIN | POLLHUP)) != 0) {
            ipc::Message incoming;
            auto status = channel.recv(incoming);
            if (! status) {
                // 协议错误：把原因告诉对端再断。诊断粒度是验收标准之一。
                LOG_ERROR("server: {}", status.error().message);
                (void) channel.send_error(ipc::WireError::BadSize, status.error().message);
                client_gone = true;
                continue;
            }
            if (status.value() == ipc::RecvStatus::Closed) {
                LOG_INFO("server: the client disconnected");
                client_gone = true;
                continue;
            }

            if (const auto* create = incoming.body_as<ipc::CreateBufferBody>(); create != nullptr) {
                if (buffers.count(create->buffer_id) != 0) {
                    (void) channel.send_error(ipc::WireError::DuplicateBuffer,
                                              fmt("buffer id {} is already in use",
                                                  create->buffer_id));
                    continue;
                }
                // **(format, modifier) 必须自己校验，不能指望内核。**
                // 实测这版 vsdrm 的 addfb2 收下了一个纯属编造的私有 modifier
                // （--fault bad-modifier 因此一度"未被发现"）。跨越信任边界
                // 进来的字段，只有我们自己知道该拿什么去比 —— 这里比的是
                // 这个 plane 在 IN_FORMATS 里公告过的集合，也就是我们
                // 在 HELLO_ACK 里发出去的那一份。
                //
                // TODO(step5): 这条只对直出成立。Step 5 有 GPU 合成回退之后，
                // 一个 plane 不接受的 modifier 未必要拒绝 —— 那时判据变成
                // "渲染器能不能采样它"，不再是"plane 能不能扫描它"。
                const auto client_modifier = create->modifier();
                if (client_modifier != kModifierInvalid &&
                    ! primary->supports(format, client_modifier)) {
                    const auto detail =
                        fmt("modifier {} is not in the set advertised for {} on {}; it was never "
                            "offered in HELLO_ACK",
                            to_string(client_modifier), to_string(format),
                            to_string(path.primary_plane));
                    LOG_ERROR("server: rejecting CREATE_BUFFER: {}", detail);
                    (void) channel.send_error(ipc::WireError::NotSupported, detail);
                    continue;
                }

                auto desc = ipc::to_dmabuf_desc(*create, incoming);
                if (! desc) {
                    // 错误码按原因分派：BadFdCount 和 BadSize 对 client 的
                    // 含义不同（前者是发送侧的 fd 组织错了，后者是字段本身错了）。
                    const ipc::WireError code = desc.error().is(ipc::Errc::BadFdCount)
                                                    ? ipc::WireError::BadFdCount
                                                    : ipc::WireError::BadSize;
                    LOG_ERROR("server: rejecting CREATE_BUFFER: {}", desc.error().message);
                    (void) channel.send_error(code, desc.error().message);
                    continue;
                }
                bool downgraded = false;
                auto fb = import_as_framebuffer(device.fd(), cache, desc.value(), &downgraded);
                if (! fb) {
                    LOG_ERROR("server: rejecting CREATE_BUFFER: {}", fb.error().message);
                    (void) channel.send_error(ipc::WireError::ImportFailed, fb.error().message);
                    continue;
                }
                if (downgraded) {
                    LOG_WARN("server: buffer {} was registered without its modifier",
                             create->buffer_id);
                }
                ImportedBuffer imported;
                imported.desc = std::move(desc).value();
                imported.fb = std::move(fb).value();
                LOG_INFO("server: imported buffer {} -> {}", create->buffer_id,
                         to_string(imported.fb.id()));
                buffers.emplace(create->buffer_id, std::move(imported));
            } else if (const auto* commit = incoming.body_as<ipc::CommitBody>();
                       commit != nullptr) {
                auto it = buffers.find(commit->buffer_id);
                if (it == buffers.end()) {
                    (void) channel.send_error(ipc::WireError::UnknownBuffer,
                                              fmt("buffer id {} was never created",
                                                  commit->buffer_id));
                    continue;
                }
                it->second.last_frame_seq = commit->frame_seq;
                verify_frame(verify, it->second, *commit);
                pending = commit->buffer_id;
                pending_seq = commit->frame_seq;
            } else if (const auto* destroy = incoming.body_as<ipc::DestroyBufferBody>();
                       destroy != nullptr) {
                if (destroy->buffer_id == on_screen || destroy->buffer_id == submitted) {
                    (void) channel.send_error(ipc::WireError::NotSupported,
                                              "cannot destroy a buffer that is still on screen");
                    continue;
                }
                buffers.erase(destroy->buffer_id);
            } else if (const auto* error = incoming.body_as<ipc::ErrorBody>(); error != nullptr) {
                LOG_ERROR("server: client reported an error: {}", error->detail);
                client_gone = true;
            }
        }

        // ---- 每秒报告 ----
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        const auto elapsed_ns =
            static_cast<uint64_t>(now.tv_sec - last_report_time.tv_sec) * 1000000000ULL +
            static_cast<uint64_t>(now.tv_nsec) - static_cast<uint64_t>(last_report_time.tv_nsec);
        if (elapsed_ns >= 1000000000ULL && frames > last_report_frame) {
            const IoctlStats current = stats();
            const IoctlStats delta = IoctlStats::delta(current, last_report);
            LOG_INFO("server: {}", frame_stats.to_line());
            LOG_INFO("  last second: {} frames, ioctls: {}", frames - last_report_frame,
                     delta.to_line());

            // 稳态零 import/addfb：跨过进程边界之后这条仍然成立，
            // 才说明边界是零成本的。
            const uint64_t rebinds =
                delta.add_fb + delta.prime_fd_to_handle + delta.prime_handle_to_fd;
            if (startup_window) {
                LOG_INFO("  (startup window: {} import/addfb ioctl(s) for {} client buffer(s); "
                         "the steady-state budget applies from the next report on)",
                         rebinds, buffers.size());
                startup_window = false;
            } else if (rebinds != 0u) {
                LOG_ERROR("  {} buffer re-binding ioctl(s) in the steady state; every client "
                          "buffer should be imported exactly once, at CREATE_BUFFER",
                          rebinds);
            }
            last_report = current;
            last_report_frame = frames;
            last_report_time = now;
        }
    }

    // ---- 收尾：先切回自有 buffer，再销毁 client 的资源 ----
    // **不允许在 on-screen 状态下 RmFB**：内核会为了自保去禁用 plane 或 CRTC，
    // 表现是闪一下黑屏，而且掩盖了真正的所有权错误。
    if (! buffers.empty()) {
        LOG_INFO("server: switching back to the fallback frame before releasing client buffers");
        request.reset();
        if (auto status = request.set_plane(*primary, fallback.fb_id(), path.crtc,
                                            SrcRect::whole(path.size()),
                                            CrtcRect::at_origin(path.size()));
            status) {
            if (auto committed = request.commit(CommitFlags::PageFlipEvent, 0); committed) {
                (void) wait_readable(device.fd(), 200);
                (void) read_events(device.fd(), device.caps().timestamp_monotonic,
                                   [](const FlipEvent&) {});
            }
        }
        buffers.clear();
    }

    LOG_INFO("server: frame loop finished: {}", frame_stats.to_line());
    LOG_INFO("server: {}", channel.counters_to_string());
    if (verify.passed + verify.failed > 0) {
        LOG_INFO("server: content check: {} passed, {} failed", verify.passed, verify.failed);
    } else if (verify.mapping_unavailable) {
        LOG_WARN("server: content check was not possible on this hardware (no dmabuf mmap)");
    }
    LOG_INFO("server: handle cache: {}", cache.to_string());

    teardown(device, path, request);

    if (child > 0) {
        int wait_status = 0;
        ::waitpid(child, &wait_status, 0);
        LOG_INFO("server: client exited with status {}", WEXITSTATUS(wait_status));
    }
    return (verify.failed == 0) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (! parse_options(argc, argv, options)) {
        return 2;
    }
    install_signal_handlers();

    const int rc = (options.role == Role::Server) ? run_server(options) : run_client(options);
    report_leaks_on_exit();
    return rc;
}
