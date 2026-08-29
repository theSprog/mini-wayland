/**
 * @file drm/device.hpp
 * @brief KMS 设备：节点选择、资源枚举、拓扑匹配、master 管理
 *
 * 三条设计决定，都是"哪种写法更能逼你看见真实机制"胜出：
 *
 * 1. **KMS fd 与 render fd 分离。**
 *    勘察结果：card0 与 card2 是同一个 PCI 设备的两个 DRM 节点，
 *    card0 没有 connector。所以"打开一个 card 就能又显示又渲染"的假设
 *    在本硬件上直接不成立。`Device` 只管 KMS；render node 由
 *    `find_render_node()` 单独给出路径，Step 2 起由 GBM/EGL 那层自己打开。
 *
 * 2. **拓扑匹配必须真的走 possible_crtcs 位图。**
 *    connector -> 它的 encoder 列表 -> 每个 encoder 的 possible_crtcs
 *    -> 位图里的下标 -> drmModeRes::crtcs[下标]。
 *    勘察结果里 HDMI-A-1 的 encoder 141 的 possible_crtcs 是 0x2，
 *    只允许 crtcs[1]（CRTC 84）。"取第一个 CRTC"会拿到 31，直接 EINVAL。
 *
 * 3. **枚举结果是带代号的快照。**
 *    热插拔后必须 rescan()，之前拿到的所有 `Connector*` / `Plane*` 全部失效。
 *    这里不把指针换成 ID 来"变安全"，而是引入 Generation 让失效
 *    变成一条明确的报错 —— 悬垂快照是合成器的经典 bug，
 *    亲眼看它发生一次比读十遍注释管用。
 */
#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/caps.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/property.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {

/**
 * @brief 资源快照的代号，每次 rescan() 递增
 *
 * 所有对外暴露的 Connector* / Crtc* / Plane* 指针只在同一个 generation
 * 内有效。OutputPath 记录自己是哪一代算出来的，modeset 前用
 * Device::validate() 校验。
 */
enum class Generation : uint64_t {};

std::string to_string(Generation g);

/// DRM_MODE_CONNECTOR_* -> "HDMI-A" / "eDP" / "Writeback" ...
/// 放在这里而不是 dump.hpp，是因为 Connector::name 要用它拼出
/// 和 modetest / sysfs 一致的名字，那属于主逻辑而不是诊断。
const char* connector_type_name(uint32_t type) noexcept;

/// DRM_MODE_ENCODER_* -> "TMDS" / "DSI" / "Virtual" ...
const char* encoder_type_name(uint32_t type) noexcept;

// ---------------------------------------------------------------------------
// 资源快照
// ---------------------------------------------------------------------------

/**
 * @brief 显示模式
 *
 * 直接持有 `drmModeModeInfo` 原始结构。原因：MODE_ID blob 要求把这个
 * 结构**逐字节**交给内核，自己定义一份再转换只会引入转换 bug。
 * 代价是 mw/drm 这一层必须 include <xf86drmMode.h>，见 README 分层约定。
 */
struct ModeInfo {
    drmModeModeInfo raw{};

    Size size() const noexcept {
        return Size{raw.hdisplay, raw.vdisplay};
    }

    /**
     * @brief 刷新率，毫赫兹
     *
     * 用 clock * 1000000 / (htotal * vtotal) 算，**不用** vrefresh 字段。
     * vrefresh 是四舍五入到整数的（60 而不是 59.94），Step 7 拿它算
     * 帧间隔会稳定偏差 0.1%，累积起来就是每分钟差一帧。
     */
    uint32_t refresh_mhz() const noexcept;

    /// 一帧的标称时长，纳秒。Step 7 判断丢帧的基准。
    uint64_t frame_duration_ns() const noexcept;

    bool is_preferred() const noexcept;
    bool is_interlaced() const noexcept;

    /// 简短形式 "1920x1080@60.000"
    std::string name() const;

    /// 完整 modeline，实现在 dump.cpp（见 dump.hpp::describe_mode）
    std::string describe() const;
};

struct Connector {
    ConnectorId id = kNoConnector;
    uint32_t type = 0;     ///< DRM_MODE_CONNECTOR_*
    uint32_t type_id = 0;  ///< 同类型里的序号
    std::string name{};    ///< "HDMI-A-1"，与 modetest / sysfs 一致
    bool connected = false;
    bool is_writeback = false;
    Size mm_size{};  ///< 物理尺寸，mm。0 表示未知
    std::vector<ModeInfo> modes{};
    std::vector<EncoderId> encoders{};
    CrtcId current_crtc = kNoCrtc;  ///< 枚举时刻内核记录的绑定，仅供诊断

    PropertyMap props{};
    ConnectorPropIds prop_ids{};

    /// preferred mode；没有标记则退回第一个；modes 为空返回 nullptr
    const ModeInfo* preferred_mode() const noexcept;

    /// 尺寸精确匹配的 mode 里刷新率最高的
    const ModeInfo* find_mode(Size size) const noexcept;
};

struct Encoder {
    EncoderId id{};
    uint32_t type = 0;
    PossibleCrtcs possible_crtcs{};
    CrtcId current_crtc = kNoCrtc;
};

struct Crtc {
    CrtcId id = kNoCrtc;
    CrtcIndex index{};    ///< 在 drmModeRes::crtcs[] 里的下标，位图匹配靠它
    bool active = false;  ///< 枚举时刻的 ACTIVE 属性值，仅供诊断

    PropertyMap props{};
    CrtcPropIds prop_ids{};
};

struct Plane {
    PlaneId id = kNoPlane;
    PlaneType type = PlaneType::Overlay;
    PossibleCrtcs possible_crtcs{};
    CrtcId current_crtc = kNoCrtc;

    /**
     * @brief 该 plane 支持的 (format, modifier) 全集
     *
     * 优先从 IN_FORMATS blob 读（带 modifier）。驱动没有 IN_FORMATS 时，
     * 回退到 drmModePlane::formats（只有 format，modifier 记为
     * kModifierInvalid —— 注意**不是 LINEAR**）。
     * 两者对应 addfb2 的两条不同路径，见 framebuffer.hpp 的说明。
     */
    std::vector<FormatModifier> formats{};

    bool supports(Format fmt, Modifier mod) const noexcept;
    bool supports_format(Format fmt) const noexcept;

    PropertyMap props{};
    PlanePropIds prop_ids{};
};

// ---------------------------------------------------------------------------
// 输出通路
// ---------------------------------------------------------------------------

/**
 * @brief 一条完整可用的 connector -> CRTC -> primary plane 通路
 *
 * Step 1 的最终产物：拿到它就可以构造 atomic request 点屏了。
 *
 * 这里**不含 cursor plane**。cursor 是 Step 5 的事，到那时会需要的是
 * "按 CRTC 查某类 plane"这个更一般的能力（见 planes_for_crtc），
 * 而不是在这个结构体里多塞一个字段。
 */
struct OutputPath {
    Generation generation{};  ///< 计算这条通路时的快照代号
    ConnectorId connector = kNoConnector;
    CrtcId crtc = kNoCrtc;
    CrtcIndex crtc_index{};
    PlaneId primary_plane = kNoPlane;
    ModeInfo mode{};

    Size size() const noexcept {
        return mode.size();
    }

    /// 多行摘要，modeset 前打一次
    std::string to_string() const;
};

struct OutputRequest {
    /// 不指定 -> 挑第一个 connected 且非 writeback 的 connector
    std::optional<ConnectorId> connector = std::nullopt;

    /// 不指定 -> 用 connector 的 preferred mode
    std::optional<Size> mode_size = std::nullopt;
};

// ---------------------------------------------------------------------------
// 节点发现
// ---------------------------------------------------------------------------

/// /dev/dri 下一个候选节点的描述
struct DeviceCandidate {
    std::string path{};         ///< "/dev/dri/card2"
    std::string driver_name{};  ///< DRM driver name（"vsdrm"），**不是** PCI driver name（"hygpu"）
    bool has_kms = false;
    bool has_connected_connector = false;

    std::string to_string() const;
};

/// 扫 /dev/dri/card*，不改变任何状态。打不开的节点跳过并 LOG_DEBUG。
std::vector<DeviceCandidate> enumerate_devices();

/// 找与该 KMS 设备同属一个物理设备的 render node 路径。Step 2 起用。
/// 找不到返回 nullopt（VKMS 就没有 render node）。
std::optional<std::string> find_render_node(const std::string& kms_path);

// ---------------------------------------------------------------------------
// master
// ---------------------------------------------------------------------------

/**
 * @brief DRM master 的 RAII 持有
 *
 * 单独成类而不是塞进 Device，是因为 master 边界本身就是要学的东西：
 * **枚举资源、读属性不需要 master，任何 modeset 需要。**
 * X11 在跑的时候你完全可以打开节点、枚举、打印拓扑，只是不能提交。
 *
 * Step 4 接 VT 切换后，切走时 drop()、切回时重新 acquire 并**重新完整
 * modeset**（不能假设切回来时 CRTC 状态还是你留下的那个）。
 */
class MasterGuard {
  public:
    MasterGuard() = default;
    ~MasterGuard();

    MasterGuard(MasterGuard&& other) noexcept;
    MasterGuard& operator=(MasterGuard&& other) noexcept;
    MasterGuard(const MasterGuard&) = delete;
    MasterGuard& operator=(const MasterGuard&) = delete;

    bool held() const noexcept {
        return held_;
    }

    /// 主动放弃（析构会再做一次，幂等）
    void drop() noexcept;

  private:
    friend class Device;
    explicit MasterGuard(BorrowedFd fd) noexcept : fd_(fd), held_(true) {}

    BorrowedFd fd_{};
    bool held_ = false;
};

// ---------------------------------------------------------------------------
// 设备
// ---------------------------------------------------------------------------

class Device {
  public:
    Device() = default;
    ~Device() = default;

    Device(Device&&) noexcept = default;
    Device& operator=(Device&&) noexcept = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // ---- 打开 ----
    // 三个入口都会：open -> probe_kernel_caps -> 全量枚举 -> refine caps。
    // 都**不碰 master**。

    static Result<Device> open(const std::string& path);

    /// 按 **DRM driver name** 打开（"vsdrm" / "vkms"），不是 PCI driver name
    static Result<Device> open_by_driver(const std::string& drm_driver_name);

    /// 挑第一个有 KMS 且有 connected connector 的节点
    static Result<Device> open_first_kms();

    // ---- 访问 ----

    BorrowedFd fd() const noexcept {
        return fd_.borrow();
    }

    const std::string& path() const noexcept {
        return path_;
    }

    const DeviceCaps& caps() const noexcept {
        return caps_;
    }

    Generation generation() const noexcept {
        return generation_;
    }

    span<const Connector> connectors() const noexcept;
    span<const Crtc> crtcs() const noexcept;
    span<const Encoder> encoders() const noexcept;
    span<const Plane> planes() const noexcept;

    const Connector* connector(ConnectorId id) const noexcept;
    const Crtc* crtc(CrtcId id) const noexcept;
    const Encoder* encoder(EncoderId id) const noexcept;
    const Plane* plane(PlaneId id) const noexcept;

    /// CrtcId <-> CrtcIndex 互转。possible_crtcs 位图匹配的唯一入口。
    std::optional<CrtcIndex> crtc_index_of(CrtcId id) const noexcept;
    std::optional<CrtcId> crtc_at(CrtcIndex index) const noexcept;

    /**
     * @brief 该 CRTC 上指定类型的全部 plane，按 plane id 升序
     *
     * 实现就是遍历 planes_ 检查 possible_crtcs.contains(index)。
     * Step 5 的分配器会重度使用；Step 1 只用它找 primary。
     *
     * 返回 vector（会分配）—— 这是初始化/重配置期的接口，不进帧循环。
     */
    std::vector<PlaneId> planes_for_crtc(CrtcId crtc, PlaneType type) const;

    // ---- 拓扑 ----

    /**
     * @brief 选一条可用的输出通路
     *
     * 步骤（每一步都 LOG_DEBUG 说明为什么选/为什么跳过）：
     *   1. 选 connector：指定的，或第一个 connected 且非 writeback 的
     *   2. 选 mode：指定尺寸精确匹配，或 preferred
     *   3. 遍历该 connector 的 encoder；对每个 encoder 遍历其
     *      possible_crtcs 位图；取第一个存在且未被别的 connector 占用的 CRTC
     *   4. 在该 CRTC 的 possible_crtcs 里找 type == Primary 的 plane
     *
     * 任一步失败返回对应的 Errc，message 里带上走到了哪一步、
     * 以及被跳过的候选各自的原因。
     */
    Result<OutputPath> pick_output(const OutputRequest& request = {}) const;

    /**
     * @brief 校验一条 OutputPath 是否还对应当前快照
     *
     * generation 不匹配 -> Errc::StaleSnapshot，message 里带上
     * "computed at gen N, device is at gen M"。每次 modeset 前调一次。
     */
    Status validate(const OutputPath& path) const;

    // ---- master ----

    /// drmSetMaster。已经是 master 时也返回成功。失败时 message 里带 master_diagnosis()。
    Result<MasterGuard> acquire_master();

    bool is_master() const noexcept;

    /**
     * @brief 拿不到 master 时的人类可读诊断
     *
     * 把 EACCES / EBUSY / EPERM 翻译成："another DRM master holds this node
     * (X11 or another compositor)" / "needs root or CAP_SYS_ADMIN" 等，
     * 并给出处理建议（stop the display manager / switch to a bare tty）。
     *
     * 只读，无副作用，任何时候都能调。
     */
    std::string master_diagnosis() const;

    // ---- 热插拔 ----

    /**
     * @brief 重新枚举全部资源，generation 递增
     *
     * 之前拿到的 Connector* / Crtc* / Plane* 指针全部失效。
     * TODO(hotplug): Step 4 接 udev monitor 后由事件驱动调用；Step 1 只手动调。
     */
    Status rescan();

  private:
    UniqueFd fd_{};
    std::string path_{};
    DeviceCaps caps_{};
    Generation generation_{0};

    std::vector<Connector> connectors_{};
    std::vector<Encoder> encoders_{};
    std::vector<Crtc> crtcs_{};
    std::vector<Plane> planes_{};

    /// 属性定义缓存。跨对象、跨 rescan 复用 —— 属性定义在设备生命周期内不变，
    /// 变的只是每个对象上的属性值。
    PropertyDefCache prop_defs_{};
};

} // namespace mw::drm
