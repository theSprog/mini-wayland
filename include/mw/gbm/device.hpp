/**
 * @file gbm/device.hpp
 * @brief GBM 设备与 buffer object
 *
 * GBM（Generic Buffer Management）是 Mesa 提供的、与具体 GPU 无关的显存
 * 分配接口。本项目只用它做**分配**，不用它的 surface / swapchain：
 *
 *   用 gbm_bo_create_with_modifiers()，不用 gbm_surface_create()
 *
 * 理由见 render/buffer_source.hpp 的说明，一句话是：`gbm_surface` 把
 * swapchain 藏在 Mesa 内部，而 buffer 数量、复用时机、每个 buffer 的 fence
 * 恰恰是后续几步要控制的东西。
 *
 * ## 与 KMS 设备的关系
 *
 * GBM 设备建立在**渲染节点**的 fd 上，与 KMS fd 严格分离。
 * 两者之间靠 dmabuf 传递（见 drm/prime.hpp），不共享 GEM handle。
 *
 * 即使在 KMS 与渲染是同一个 fd 的平台上，本项目也保持这个分离 ——
 * 让一份代码在两种拓扑下走同一条路径，比省一次 PRIME 更有价值。
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/prime.hpp"
#include "mw/drm/types.hpp"

struct gbm_device;
struct gbm_bo;

namespace mw::gbm {

using drm::Format;
using drm::Modifier;
using drm::Size;

/// 分配用途。映射到 GBM_BO_USE_* 位。
enum class Usage : uint32_t {
    /// 要能被显示控制器扫描输出
    Scanout = 1u << 0,
    /// 要能被 GPU 当作渲染目标
    Rendering = 1u << 1,
    /// 要能被 CPU 映射写入。**会强制线性排布**，与压缩/平铺 modifier 互斥。
    CpuWrite = 1u << 2,
};

inline Usage operator|(Usage a, Usage b) noexcept {
    return static_cast<Usage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline bool has(Usage set, Usage bit) noexcept {
    return (static_cast<uint32_t>(set) & static_cast<uint32_t>(bit)) != 0u;
}

std::string to_string(Usage usage);

class Buffer;

/**
 * @brief 建立一个可写的 CPU 映射
 *
 * 只对线性排布的 buffer 有意义。返回的 cookie 必须原样传给 unmap() ——
 * GBM 用它记录内部状态，不是可以丢弃的 out 参数。
 *
 * @warning 显存映射通常是写合并的：顺序写尚可，读回极慢。
 */
Result<span<uint8_t>> map_write(const Buffer& buffer, void*& cookie);

/// 释放 map_write() 建立的映射。cookie 必须是同一次 map_write 返回的那个。
void unmap(const Buffer& buffer, void* cookie) noexcept;

// ---------------------------------------------------------------------------

class Device;

/**
 * @brief 一块 GBM 分配的 buffer
 *
 * move-only。析构时销毁 bo。
 *
 * @note 本类**不持有** fb_id，也不知道 KMS 的存在。要上屏请用
 *       `export_dmabuf()` 拿到 DmabufDesc，再交给
 *       `drm::import_as_framebuffer()`。这条边界是刻意的：
 *       分配层不该知道谁会消费它。
 */
class Buffer {
  public:
    Buffer() noexcept = default;
    ~Buffer();

    Buffer(Buffer&&) noexcept;
    Buffer& operator=(Buffer&&) noexcept;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    bool valid() const noexcept {
        return bo_ != nullptr;
    }

    Size size() const noexcept {
        return size_;
    }
    Format format() const noexcept {
        return format_;
    }

    /**
     * @brief 分配器**实际选中**的 modifier
     *
     * 未必是候选列表的第一项。不带 modifier 分配时返回 kModifierInvalid。
     * 与 KMS 一样按不透明 token 处理，不解码。
     */
    Modifier modifier() const noexcept {
        return modifier_;
    }

    uint32_t plane_count() const noexcept {
        return plane_count_;
    }
    uint32_t stride(uint32_t plane) const noexcept;
    uint32_t offset(uint32_t plane) const noexcept;

    /**
     * @brief 导出为跨设备/跨进程可传递的描述
     *
     * 每个平面各导出一个 fd。多平面共享同一块底层分配时，
     * 各平面会拿到指向同一 dma_buf 的不同 fd —— 所有权规则因此保持
     * "一个平面一个 fd"，去重交给导入侧的 HandleCache。
     *
     * @note 每次调用都产生新的 fd。结果应当缓存，不要每帧调。
     */
    Result<drm::DmabufDesc> export_dmabuf() const;

    /// 底层句柄。仅供同一层内的 EGL 互操作使用，不要外泄。
    gbm_bo* raw() const noexcept {
        return bo_;
    }

    std::string to_string() const;

  private:
    friend class Device;

    gbm_bo* bo_ = nullptr;
    Size size_{};
    Format format_{};
    Modifier modifier_ = drm::kModifierInvalid;
    uint32_t plane_count_ = 1;
};

// ---------------------------------------------------------------------------

/**
 * @brief 一个 GBM 设备，绑定到某个渲染节点
 *
 * 持有自己打开的 fd。与 KMS fd 无关。
 */
class Device {
  public:
    Device() noexcept = default;
    ~Device();

    Device(Device&&) noexcept;
    Device& operator=(Device&&) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    /**
     * @brief 打开一个节点并在其上创建 GBM 设备
     *
     * @param node_path 通常是一个 render node。用 primary node 也行，
     *                  但那会顺带取得 KMS 权限，不必要地扩大了权限面。
     */
    static Result<Device> open(const std::string& node_path);

    bool valid() const noexcept {
        return device_ != nullptr;
    }

    /// GBM 后端名（Mesa 的驱动名），诊断用
    std::string backend_name() const;

    /// 本设备的 fd。借用，所有权仍在本对象。
    BorrowedFd fd() const noexcept {
        return fd_.borrow();
    }

    /// 底层句柄。给 EGL 当 native display 用，不要外泄到 mw/render 之上。
    gbm_device* raw() const noexcept {
        return device_;
    }

    /**
     * @brief 检查某个 (format, modifier) 组合能否分配
     *
     * 走 `gbm_device_get_format_modifier_plane_count()`，不实际分配。
     *
     * @warning **返回 true 不代表 allocate() 一定成功**，也不代表分配出来的
     *          buffer 能被别的设备扫描输出。这里只是一次便宜的预筛，
     *          用来把明显不可能的组合挡在真分配之前。真正的判据是分配一次。
     */
    bool format_modifier_supported(Format format, Modifier modifier) const;

    /**
     * @brief 从候选 modifier 里挑一个并分配
     *
     * 把整个候选列表交给 GBM，由它选 —— **不在这里排序、不做偏好判断**。
     * 排序属于协商策略（Step 4 的 dmabuf-feedback tranche），
     * 分配层只负责执行。
     *
     * 候选为空时退回 `gbm_bo_create()`（不带 modifier），
     * 此时 `Buffer::modifier()` 返回 kModifierInvalid。
     *
     * @note `Usage::CpuWrite` 与非线性 modifier 互斥。同时给两者时，
     *       实现会忽略候选列表并走不带 modifier 的路径，并 WARN ——
     *       静默地二选一比报错更难查。
     */
    Result<Buffer> allocate(Size size, Format format, span<const Modifier> modifiers,
                            Usage usage) const;

    std::string to_string() const;

  private:
    Device(UniqueFd fd, gbm_device* device) noexcept
        : fd_(std::move(fd)), device_(device) {}

    UniqueFd fd_{};
    gbm_device* device_ = nullptr;
};

} // namespace mw::gbm
