/**
 * @file ipc/wire.hpp
 * @brief 跨进程 buffer 传递的线格式：消息定义、校验、与 DmabufDesc 的互转
 *
 * ## 这一层的职责边界
 *
 * 只做**字节与 fd 组** ↔ **`drm::DmabufDesc`** 的双向转换，以及消息的
 * 自洽性校验。不碰 socket（那是 `channel.hpp`），不碰 KMS（那是调用方）。
 *
 * 判据：本文件应该能在一台没有 GPU 的机器上编译并单测。
 *
 * ## 结构对齐 wayland，编码不对齐
 *
 * 字段集合按 `zwp_linux_buffer_params_v1::add` 设计：
 * 每平面 fd / offset / stride，modifier 拆成 hi / lo 两个 32 位 ——
 * 这些都不是本项目的选择，是那套协议的既定形状。Step 4 换成真协议时，
 * **替换的是传输层，不是数据模型**。
 *
 * 但不引入 wayland wire format（对象 id、opcode、变长数组）。
 * 那属于 Step 4 明确要剔除的脏活，在这里做等于提前付两遍代价。
 *
 * ## 三个"冗余"字段是故意的
 *
 * `MessageHeader` 里的 `abi_version` / `body_size` / `fd_count` 看着都是
 * 冗余信息：SEQPACKET 已经给了消息边界，cmsg 已经给了 fd 数量。
 *
 * **冗余就是判据。** 收发两侧一旦对不上，说明它们对这条消息的理解不一致 ——
 * 最常见的原因是两个二进制不是同一次构建出来的。同机、同 ABI，
 * 不代表同版本。这类错误不检查就会表现为"字段读出来是垃圾值"，
 * 而垃圾值恰好落在合法区间时，症状是画面错而不是报错。
 *
 * 改动任何 body 结构都必须 +1 `kWireAbiVersion`：下面的 static_assert
 * 把版本号绑在结构体尺寸上，改了忘记加会**编译失败**。
 *
 * ## 为什么 body 是定长 POD
 *
 * 每帧一条 `COMMIT`，属于热路径。定长 POD + 固定大小数组意味着收发两侧
 * 都不需要堆分配，也不需要变长解析（那是缓冲区溢出最爱藏身的地方）。
 * 代价是 `HELLO_ACK` 里能带的 modifier 个数有上限，超出就截断并 WARN ——
 * 对 Step 3 足够，Step 4 换成真协议后这个限制自然消失。
 */
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>

#include "mw/internal/span.hpp"
#include "mw/internal/error.hpp"
#include "mw/internal/unique_fd.hpp"
#include "mw/internal/unique_fd.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/types.hpp"

using internal::span;

namespace mw::ipc {

using drm::Format;
using drm::Modifier;
using drm::Size;

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

/// "MWIP"。放在每条消息最前面，第一道判据。
inline constexpr uint32_t kWireMagic = 0x4d574950u;

/// **改任何 body 结构都要 +1。** 下面的 static_assert 会强制这件事。
inline constexpr uint16_t kWireAbiVersion = 1;

/// body 的上限。定长缓冲区，收方按这个尺寸静态分配。
inline constexpr uint32_t kMaxBodySize = 256;

/// 一条消息最多携带几个 fd。与 dmabuf 的平面数上限同源。
inline constexpr uint32_t kMaxMessageFds = static_cast<uint32_t>(drm::kMaxDmabufPlanes);

/// `HELLO_ACK` 里能带的 modifier 个数上限
inline constexpr uint32_t kMaxAdvertisedModifiers = 16;

/// `ERROR` 消息里的描述长度。**英文**，直接打给用户。
inline constexpr uint32_t kMaxErrorDetail = 128;

/// buffer 的跨进程标识。由 client 分配，在一条连接内唯一。
enum class BufferId : uint32_t {};

constexpr uint32_t to_u32(BufferId id) noexcept {
    return static_cast<uint32_t>(id);
}

// ---------------------------------------------------------------------------
// 消息类型
// ---------------------------------------------------------------------------

/**
 * @brief 每条消息在 wayland 里都有对应物，这不是巧合
 *
 *   HELLO / HELLO_ACK    wl_display.get_registry / zwp_linux_dmabuf_feedback_v1
 *   CREATE_BUFFER        zwp_linux_buffer_params_v1.add + .create
 *   DESTROY_BUFFER       wl_buffer.destroy
 *   COMMIT               wl_surface.attach + .commit
 *   BUFFER_RELEASE       wl_buffer.release
 *   FRAME_DONE           wl_surface.frame
 *   ERROR                wl_display.error
 *
 * 保持这个对应关系是 Step 3 的主要设计目标之一。
 */
enum class MsgType : uint16_t {
    Invalid = 0,
    Hello = 1,
    HelloAck = 2,
    CreateBuffer = 3,
    DestroyBuffer = 4,
    Commit = 5,
    BufferRelease = 6,
    FrameDone = 7,
    Error = 8,
};

const char* to_string(MsgType type) noexcept;

/// `ERROR` 消息的码。**协议层面的错误**，不是 errno。
enum class WireError : uint32_t {
    None = 0,
    BadMagic = 1,
    BadVersion = 2,
    BadSize = 3,
    BadFdCount = 4,
    UnknownType = 5,
    UnknownBuffer = 6,   ///< COMMIT / DESTROY 引用了不存在的 buffer_id
    DuplicateBuffer = 7, ///< CREATE_BUFFER 用了已占用的 buffer_id
    ImportFailed = 8,    ///< PRIME_FD_TO_HANDLE 或 addfb2 失败
    NotSupported = 9,    ///< client 请求了 server 提供不了的东西
};

const char* to_string(WireError err) noexcept;

// ---------------------------------------------------------------------------
// 消息头
// ---------------------------------------------------------------------------

struct MessageHeader {
    uint32_t magic = kWireMagic;
    uint16_t abi_version = kWireAbiVersion;
    uint16_t type = static_cast<uint16_t>(MsgType::Invalid);
    uint32_t body_size = 0;
    uint32_t fd_count = 0;

    MsgType msg_type() const noexcept {
        return static_cast<MsgType>(type);
    }
};

static_assert(sizeof(MessageHeader) == 16, "wire header layout changed -- bump kWireAbiVersion");
static_assert(std::is_trivially_copyable<MessageHeader>::value, "header must be memcpy-able");

// ---------------------------------------------------------------------------
// body
// ---------------------------------------------------------------------------

/// client 侧希望从哪个设备分配。**决定权在 server**，见 HelloAckBody。
enum class SourceKindWire : uint32_t {
    Any = 0,
    ScanoutDevice = 1, ///< 显示节点 dumb，线性，不依赖跨设备导入
    RenderDevice = 2,  ///< 渲染节点 GBM，可协商 modifier
};

const char* to_string(SourceKindWire kind) noexcept;

struct HelloBody {
    static constexpr MsgType kType = MsgType::Hello;

    /// 与 header.abi_version 重复，故意的：见文件头"三个冗余字段"
    uint32_t client_abi = kWireAbiVersion;

    /// client 自己能做到的分配方式（位掩码，bit = SourceKindWire 的值）。
    /// server 只会在这个集合里挑，挑不出来就回 ERROR/NotSupported。
    uint32_t supported_sources = 0;
};

/**
 * @brief server 告诉 client 该怎么分配
 *
 * **这是 `linux-dmabuf-feedback` 的雏形。** 决策发生在 server 侧，
 * 因为真实 client（Mesa）不认识显示设备，也不该认识 ——
 * 它照着合成器发来的 tranche 分配。Step 3 把数据流向定成这样，
 * Step 4 换协议时就不用推翻它。
 *
 * @note modifier 列表**原样转发，不排序**。排序是 Step 4 的 tranche 策略。
 */
struct HelloAckBody {
    static constexpr MsgType kType = MsgType::HelloAck;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0; ///< fourcc

    uint32_t recommended_source = static_cast<uint32_t>(SourceKindWire::Any);

    /// 实际填入 modifiers[] 的个数。为 0 表示"不指定，走不带 modifier 的分配"，
    /// 语义上**不等于** LINEAR（见 framebuffer.hpp 的两条 addfb 路径）。
    uint32_t modifier_count = 0;

    /// 被截断时置位（候选超过 kMaxAdvertisedModifiers）。client 只需知道
    /// "这不是完整清单"，不需要知道少了哪些。
    uint32_t truncated = 0;

    uint64_t modifiers[kMaxAdvertisedModifiers] = {};
};

/**
 * @brief 一块 buffer 的完整描述。**随消息携带 num_planes 个 fd。**
 *
 * 一生只发一次。之后每帧只发 `COMMIT`(buffer_id) —— 这样稳态里
 * `prime_fd_to_handle` / `add_fb` 的增量为 0，Step 2 建立的记账约束
 * 跨过进程边界之后仍然成立。
 */
struct CreateBufferBody {
    static constexpr MsgType kType = MsgType::CreateBuffer;

    uint32_t buffer_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t num_planes = 1;

    /// TODO(step5): y_invert / interlaced / bottom_first，对应
    /// zwp_linux_buffer_params_v1 的 flags。现在恒为 0。
    uint32_t flags = 0;

    /// 拆成 hi/lo 是为了和 `zwp_linux_buffer_params_v1::add` 的形状一致
    uint32_t modifier_hi = 0;
    uint32_t modifier_lo = 0;

    uint32_t offsets[kMaxMessageFds] = {};
    uint32_t strides[kMaxMessageFds] = {};

    Modifier modifier() const noexcept;
    void set_modifier(Modifier mod) noexcept;
};

struct DestroyBufferBody {
    static constexpr MsgType kType = MsgType::DestroyBuffer;

    uint32_t buffer_id = 0;
    uint32_t reserved = 0;
};

struct CommitBody {
    static constexpr MsgType kType = MsgType::Commit;

    uint32_t buffer_id = 0;
    uint32_t frame_seq = 0;

    /// L1 内容判据：client 画进 buffer 左上角的签名块的 CRC。
    /// 见 `ipc/signature.hpp`。为 0 表示这一帧没带签名。
    uint32_t signature_crc = 0;

    /// TODO(step5): damage 区域。现在恒为全屏（w/h 为 0 表示全屏）。
    uint32_t damage_x = 0;
    uint32_t damage_y = 0;
    uint32_t damage_w = 0;
    uint32_t damage_h = 0;

    /// TODO(step6): bit0 = 随消息带 acquire fence fd。现在恒为 0，
    /// 同步靠 client 在提交前 glFinish。**字段先占位是有意的**：
    /// 带 fence 的提交和不带 fence 的提交是两种语义，
    /// 先把位置留出来，Step 6 改的是填充逻辑而不是消息集合。
    uint32_t flags = 0;
};

/**
 * @brief 这块 buffer 不再被扫描，client 可以覆写了
 *
 * **时机很容易写错**：一块 buffer 在被另一块换下来之前一直在被扫描。
 * 所以第 N 帧 flip 完成时该释放的是**第 N-1 帧**用的那块，不是第 N 帧那块。
 * 写成后者，双缓冲 client 会在正在扫描的 buffer 上作画，
 * 表现是间歇性撕裂，而且帧率越稳越难复现。
 *
 * TODO(step6): 换成带 release fence 的版本后，fence 一 signal 就能复用，
 *              不必等下一次 flip。当前这条保守规则是正确的，只是不是最优的。
 */
struct BufferReleaseBody {
    static constexpr MsgType kType = MsgType::BufferRelease;

    uint32_t buffer_id = 0;
    uint32_t flags = 0; ///< TODO(step6): bit0 = 随消息带 release fence fd
};

struct FrameDoneBody {
    static constexpr MsgType kType = MsgType::FrameDone;

    uint32_t frame_seq = 0; ///< 回显 COMMIT 里的序号
    uint32_t flip_seq = 0;  ///< server 侧的翻页计数，用于对丢帧

    /// TODO(step7): vblank 时间戳。**现在恒为 0**，不是忘了填 ——
    /// 填了就等于提前做了 Step 7 的时钟语义，而那部分还有
    /// 提交队列深度的问题要单独处理（见 vendor-kmd-notes）。
    uint64_t timestamp_ns = 0;
};

struct ErrorBody {
    static constexpr MsgType kType = MsgType::Error;

    uint32_t code = static_cast<uint32_t>(WireError::None);
    uint32_t reserved = 0;

    /// **英文**，以 '\0' 结尾。诊断粒度要求见 step3-design.md 8.4：
    /// "import failed" 不合格，要说到哪个 ioctl、什么 errno。
    char detail[kMaxErrorDetail] = {};
};

// 版本号与结构尺寸绑死：改了结构忘记 +1 版本号会编译失败。
static_assert(kWireAbiVersion == 1, "bump the sizes below together with the version");
static_assert(sizeof(HelloBody) == 8, "HelloBody changed -- bump kWireAbiVersion");
static_assert(sizeof(HelloAckBody) == 152, "HelloAckBody changed -- bump kWireAbiVersion");
static_assert(sizeof(CreateBufferBody) == 64, "CreateBufferBody changed -- bump kWireAbiVersion");
static_assert(sizeof(DestroyBufferBody) == 8, "DestroyBufferBody changed -- bump kWireAbiVersion");
static_assert(sizeof(CommitBody) == 32, "CommitBody changed -- bump kWireAbiVersion");
static_assert(sizeof(BufferReleaseBody) == 8, "BufferReleaseBody changed -- bump kWireAbiVersion");
static_assert(sizeof(FrameDoneBody) == 16, "FrameDoneBody changed -- bump kWireAbiVersion");
static_assert(sizeof(ErrorBody) == 136, "ErrorBody changed -- bump kWireAbiVersion");

static_assert(sizeof(HelloAckBody) <= kMaxBodySize, "body exceeds kMaxBodySize");
static_assert(sizeof(ErrorBody) <= kMaxBodySize, "body exceeds kMaxBodySize");

// ---------------------------------------------------------------------------
// 收到的一条消息
// ---------------------------------------------------------------------------

/**
 * @brief 头 + 定长 body + 随消息到达的 fd
 *
 * **move-only，持有 fd 的所有权。** 这是接收路径的核心契约：
 * 要么返回一个持有全部 fd 的 Message，要么一个 fd 都不留下。
 * 没有中间状态 —— 解析失败时半途返回、把已收到的 fd 漏在原地，
 * 是这类代码最常见的泄漏点，而且泄漏的是显存不是内存，现场看不出来。
 */
class Message {
  public:
    Message() = default;
    ~Message() = default;

    Message(Message&&) noexcept = default;
    Message& operator=(Message&&) noexcept = default;
    Message(const Message&) = delete;
    Message& operator=(const Message&) = delete;

    const MessageHeader& header() const noexcept {
        return header_;
    }

    MsgType type() const noexcept {
        return header_.msg_type();
    }

    uint32_t fd_count() const noexcept {
        return fd_count_;
    }

    /// 借用第 i 个 fd。所有权仍在本对象。
    BorrowedFd fd(uint32_t index) const noexcept;

    /**
     * @brief 按类型取 body
     *
     * 校验 `header_.type` 与 `T::kType` 一致、`body_size` 与 `sizeof(T)`
     * 一致，任一不符返回 nullptr。**不要绕过它直接 reinterpret_cast body_**。
     */
    template <typename T>
    const T* body_as() const noexcept {
        if (header_.msg_type() != T::kType || header_.body_size != sizeof(T)) {
            return nullptr;
        }
        return reinterpret_cast<const T*>(body_);
    }

    /// 交出全部 fd 的所有权（用于构造 DmabufDesc）。调用后 fd_count() 归零。
    void take_fds(UniqueFd (&out)[kMaxMessageFds], uint32_t& out_count) noexcept;

    /// 提前关闭全部 fd。析构会再做一次，幂等。
    void close_fds() noexcept;

    std::string to_string() const;

  private:
    friend class Channel;

    MessageHeader header_{};
    alignas(8) uint8_t body_[kMaxBodySize] = {};
    UniqueFd fds_[kMaxMessageFds] = {};
    uint32_t fd_count_ = 0;
};

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

/**
 * @brief 头部自洽性
 *
 * 检查 magic、版本、`body_size` 与实际收到的字节数、`fd_count` 与实际
 * 收到的 fd 数。**这四条都失败过才叫检查有用** —— 故障注入用例
 * `stale-header` / `half-message` / `missing-fd` / `extra-fd` 分别打这四条。
 */
Status validate_header(const MessageHeader& header, uint32_t received_body_bytes,
                       uint32_t received_fds);

/**
 * @brief `CREATE_BUFFER` 的自洽性
 *
 * `num_planes` 在 [1, kMaxMessageFds] 内、等于 fd 数、宽高非 0、
 * 各平面 stride 非 0。**不做尺寸与实际 buffer 大小的比对** ——
 * 那要靠内核在 addfb2 时校验 GEM 对象大小，用户态算一遍只会得到
 * 两套可能不一致的规则。
 */
Status validate(const CreateBufferBody& body, uint32_t fd_count);

// ---------------------------------------------------------------------------
// 与 DmabufDesc 互转
// ---------------------------------------------------------------------------

/// `DmabufDesc` -> 线格式。fd 不在这里传，由 channel 从 desc 里借用发送。
CreateBufferBody make_create_buffer(BufferId id, const drm::DmabufDesc& desc);

/**
 * @brief 线格式 + 收到的 fd -> `DmabufDesc`
 *
 * **消费 msg 里的 fd**（转移所有权）。失败时 msg 仍持有 fd，
 * 由 msg 的析构负责关闭 —— 所有权规则在任何一条路径上都不断。
 */
Result<drm::DmabufDesc> to_dmabuf_desc(const CreateBufferBody& body, Message& msg);

// ---------------------------------------------------------------------------
// CRC
// ---------------------------------------------------------------------------

/**
 * @brief CRC-32（IEEE 多项式，反射式，与 zlib 一致）
 *
 * 放在这里而不是各自实现：**收发两侧必须是同一份代码**，
 * 否则比对出的不一致到底是数据错了还是算法不同，无从判断。
 */
uint32_t crc32(span<const uint8_t> data) noexcept;

} // namespace mw::ipc
