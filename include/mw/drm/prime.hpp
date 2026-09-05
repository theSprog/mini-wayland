/**
 * @file drm/prime.hpp
 * @brief PRIME：GEM handle <-> dmabuf fd 的双向转换
 *
 * PRIME 是 DRM 里"把一块显存变成一个可以跨 fd、跨进程传递的 fd"的机制。
 * 两个方向：
 *
 *   DRM_IOCTL_PRIME_HANDLE_TO_FD   GEM handle -> dmabuf fd（导出）
 *   DRM_IOCTL_PRIME_FD_TO_HANDLE   dmabuf fd -> GEM handle（导入）
 *
 * ## 为什么它在 Step 2 就必须出现
 *
 * 教科书（kmscube 那一类）的 GBM 上屏路径是：
 *
 *   gbm_create_device(kms_fd) -> gbm_bo_create -> gbm_bo_get_handle()
 *     -> drmModeAddFB2(kms_fd, handle, ...)
 *
 * 它成立的前提是 **KMS fd 和渲染 fd 是同一个 fd**。当显示设备与渲染设备
 * 是两个独立的 DRM 节点时（显示节点不能渲染、render node 没有 KMS），
 * 这个前提不成立：`gbm_bo_get_handle()` 返回的 handle 属于渲染节点，
 * 拿去 KMS 节点上 addfb2 是错的 —— 好的情况是 EINVAL，坏的情况是命中
 * 一个碰巧存在的无关 handle，症状不稳定且极难查。
 *
 * **GEM handle 的作用域是单个 drm_file**，这是 UAPI 层面的规则，
 * 与具体驱动无关。跨 fd 使用 buffer 的唯一正规途径就是 PRIME。
 *
 * 所以这类拓扑下 buffer 必须走：
 *
 *   分配器所在设备 -> 导出 dmabuf fd -> PRIME_FD_TO_HANDLE(目标设备)
 *     -> addfb2(目标设备)
 *
 * Step 3 的跨进程只是在中间插了一段 SCM_RIGHTS，机制本身完全一样。
 *
 * ## 核心陷阱：内核会去重，但不会为你计数
 *
 * `drm_gem_prime_fd_to_handle()` 内部对每个 drm_file 维护一张
 * dma_buf -> handle 的表。**同一个 dma_buf 在同一个 fd 上导入两次，
 * 返回同一个 handle，且不增加任何引用计数**（内核在查表命中时直接
 * 返回，不走 drm_gem_handle_reference）。这是 DRM 核心行为，所有驱动一致。
 *
 * 后果：如果你按"一次导入一个 RAII 对象"的直觉写，两个对象各自
 * GEM_CLOSE，第二次就把还在用的 handle 销毁了。这不是理论风险：
 *
 *   - 一个多平面 buffer（如 NV12）的各平面通常在**同一个 dma_buf** 里，
 *     导入两个平面就是导入同一个 dma_buf 两次 —— 单帧内立刻触发。
 *   - Step 3 之后多个 surface 引用同一个 client buffer，同理。
 *
 * 所以引用计数必须由用户态做，这就是 HandleCache 存在的唯一理由。
 *
 * ## handle 的生命周期比你以为的短
 *
 * 按 DRM 惯例，驱动的 `fb_create` 会对 GEM 对象取引用并由 fb 持有，
 * 因此 `drmModeAddFB2` 成功后关掉 handle，fb_id 依然有效、依然能扫描。
 * 正常流程里 imported handle 是**临时量**：导入 -> addfb2 -> 立刻释放，
 * 长期持有的是 fb_id 而不是 handle。
 *
 * 这条是惯例而非 UAPI 强制，所以 `demos/step2_prime_roundtrip` 会在目标
 * 设备上实测一次（关掉全部 handle 后 drmModeGetFB 仍能拿到 fb），
 * 而不是直接假设。
 *
 * 真正需要长期缓存的是 dmabuf -> fb_id 的映射（避免每帧重复 addfb2），
 * 那属于更上一层的策略，不放在这里。
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

#include "mw/internal/error.hpp"
#include "mw/internal/unique_fd.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/framebuffer.hpp"
#include "mw/drm/types.hpp"

using internal::Result;
using internal::Status;
using internal::UniqueFd;

namespace mw::drm {

class HandleCache;

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

/// PRIME 导出的访问权限
enum class PrimeAccess {
    /// 只读。够用于 scanout 与 GPU 采样。
    ReadOnly,

    /// 读写（DRM_RDWR）。**导入方想 mmap 这个 dmabuf 写像素时必需**。
    /// Step 3 的 client 用 dumb buffer + CPU 画图时走这条。
    ReadWrite,
};

/**
 * @brief GEM handle -> dmabuf fd
 *
 * 总是带 O_CLOEXEC —— 合成器会 fork/exec，泄漏一个 dmabuf fd 到子进程
 * 意味着那块显存永远不释放，而且现场看不出来。
 *
 * 需要 DRM_PRIME_CAP_EXPORT（`DeviceCaps::prime_export`）。
 * 调用方自行检查；这里不重复探测，探测结果属于 Device。
 *
 * @param device  持有该 handle 的 DRM fd
 * @param handle  该 fd 上的 GEM handle
 */
Result<UniqueFd> export_dmabuf(BorrowedFd device, GemHandle handle,
                               PrimeAccess access = PrimeAccess::ReadOnly);

// ---------------------------------------------------------------------------
// 导入
// ---------------------------------------------------------------------------

/**
 * @brief 一次导入的结果，析构时归还引用
 *
 * move-only。**必须由 HandleCache 产生**，没有公开构造函数 ——
 * 直接调 drmPrimeFDToHandle 拿裸 handle 再自己 close 就是上面说的
 * 双重释放场景，这里用类型系统堵死。
 *
 * 典型用法是临时量：
 * @code
 *   auto h = TRY(cache.import(kms_fd, dmabuf_fd));
 *   auto fb = TRY(Framebuffer::add(kms_fd, desc_using(h.handle())));
 *   // h 在这里析构；fb 仍然有效，因为 fb 自己持有 GEM 引用
 * @endcode
 */
class ImportedHandle {
  public:
    ImportedHandle() noexcept = default;
    ~ImportedHandle();

    ImportedHandle(ImportedHandle&& other) noexcept;
    ImportedHandle& operator=(ImportedHandle&& other) noexcept;
    ImportedHandle(const ImportedHandle&) = delete;
    ImportedHandle& operator=(const ImportedHandle&) = delete;

    GemHandle handle() const noexcept {
        return handle_;
    }

    bool valid() const noexcept {
        return cache_ != nullptr && handle_ != GemHandle{0};
    }

    /// 提前归还（析构会再做一次，幂等）
    void reset() noexcept;

  private:
    friend class HandleCache;
    ImportedHandle(HandleCache* cache, GemHandle handle) noexcept
        : cache_(cache), handle_(handle) {}

    HandleCache* cache_ = nullptr;  ///< 不拥有；cache 必须比所有 handle 活得久
    GemHandle handle_{0};
};

/**
 * @brief 一个 DRM fd 上的 imported handle 引用计数表
 *
 * **每个 DRM fd 一个实例**，不能跨 fd 共享 —— GEM handle 的作用域就是
 * 单个 drm_file，同一个数值在两个 fd 上是两个不相干的对象。
 * 构造时绑定 fd，import() 再传一次是为了断言一致（debug 下）。
 *
 * ## 为什么用 handle 而不是 dmabuf 的 inode 做 key
 *
 * wlroots / weston 用 `fstat(dmabuf_fd).st_ino` 做 key，好处是缓存命中时
 * **连 ioctl 都不用发**（同一个 dma_buf 无论怎么 dup / SCM_RIGHTS 传递，
 * inode 恒定）。这里先不这么做，理由：
 *
 *  1. 那依赖\"dmabuf 的 inode 唯一标识底层对象\"这条不成文假设。
 *     用 handle 做 key 不依赖任何假设 —— 内核已经替我们去重了，
 *     我们只需要不重复释放。
 *  2. 省下的那次 ioctl 在 Step 2 不在热路径（一共两三个 buffer）。
 *
 * TODO(step4): client 每帧提交时若 trace 计数显示 prime_fd_to_handle
 * 明显增长，再换成 inode 做 key。先让计数器说话，不提前优化。
 */
class HandleCache {
  public:
    explicit HandleCache(BorrowedFd device) noexcept : device_(device) {}

    /// 断言所有 handle 都已归还。有残留时 LOG_ERROR 并强制清理，不 abort ——
    /// 泄漏应该被看见，但不该让退出路径更难调试。
    ~HandleCache();

    HandleCache(HandleCache&&) = delete;
    HandleCache& operator=(HandleCache&&) = delete;
    HandleCache(const HandleCache&) = delete;
    HandleCache& operator=(const HandleCache&) = delete;

    /**
     * @brief dmabuf fd -> 该设备上的 GEM handle
     *
     * 每次都真的发一次 PRIME_FD_TO_HANDLE（见上面 key 选择的说明），
     * 拿到 handle 后在表里累加引用。所以\"同一个 dma_buf 导入两次\"
     * 会得到相同的 handle 和引用计数 2，两个 ImportedHandle 各自析构，
     * 归零时才 GEM_CLOSE。
     *
     * @param device 必须与构造时的 fd 相同（debug 下断言）
     * @param dmabuf 借用，本函数不改变其所有权
     */
    Result<ImportedHandle> import(BorrowedFd device, BorrowedFd dmabuf);

    /// 当前被引用的 handle 数。退出时应为 0，配平检查用。
    size_t live_count() const noexcept;

    /// 某个 handle 当前的引用数，0 表示不在表里。诊断用。
    uint32_t ref_count(GemHandle handle) const noexcept;

    /// 一行摘要："2 handles live, 3 refs total"
    std::string to_string() const;

  private:
    friend class ImportedHandle;

    /// ImportedHandle 析构时调用；归零则 GEM_CLOSE
    void release(GemHandle handle) noexcept;

    BorrowedFd device_{};
    std::unordered_map<uint32_t, uint32_t> refs_{};  ///< handle -> refcount
};

// ---------------------------------------------------------------------------
// dmabuf 的完整描述
// ---------------------------------------------------------------------------

/// 与 framebuffer.hpp 的 kMaxFbPlanes 同源，直接复用避免两处漂移
inline constexpr size_t kMaxDmabufPlanes = kMaxFbPlanes;

/**
 * @brief 一块可跨设备/跨进程传递的 buffer 的完整描述
 *
 * 这是 Step 2 之后各层之间流通的\"通用货币\"：GBM 分配出来的、
 * Step 3 从 socket 收到的、Step 4 从 linux-dmabuf 协议拿到的，
 * 最终都归一成这个结构，再交给 import + addfb2。
 *
 * **拥有 fd**，move-only。
 *
 * @note 多平面时各平面**可能共享同一个 dma_buf**（NV12 的 Y/UV 常见于
 *       同一块分配里，靠 offset 区分）。本结构不做共享检测，每个平面
 *       各持有一个独立的 fd（必要时 dup）——所有权规则简单一条，
 *       去重交给 HandleCache 在导入时处理。
 */
struct DmabufDesc {
    Size size{};
    Format format{};

    /**
     * @brief 全体平面共用的 modifier
     *
     * UAPI 要求一个 fb 的所有平面 modifier 相同，所以这里只存一个。
     * kModifierInvalid 表示\"没有 modifier 信息\"，语义上**不等于** LINEAR，
     * 对应 addfb2 的两条不同路径（见 framebuffer.hpp）。
     */
    Modifier modifier = kModifierInvalid;

    uint32_t num_planes = 1;

    UniqueFd fds[kMaxDmabufPlanes]{};
    uint32_t offsets[kMaxDmabufPlanes]{};
    uint32_t strides[kMaxDmabufPlanes]{};

    DmabufDesc() = default;
    ~DmabufDesc() = default;
    DmabufDesc(DmabufDesc&&) noexcept = default;
    DmabufDesc& operator=(DmabufDesc&&) noexcept = default;
    DmabufDesc(const DmabufDesc&) = delete;
    DmabufDesc& operator=(const DmabufDesc&) = delete;

    /// 基本自洽性：num_planes 在范围内、fd 有效、stride 非 0
    Status validate() const;

    /// 一行摘要，日志用
    std::string to_string() const;
};

/**
 * @brief 导入 + 注册 fb 一步到位
 *
 * 内部：对每个平面 cache.import()，组装 FramebufferDesc，
 * 调 `Framebuffer::add_with_fallback()`，然后**释放全部 imported handle**
 * （fb 自己持有 GEM 引用，见文件头说明）。
 *
 * 这是 Step 2/3/4 的公共路径，单独提出来避免三处各写一遍。
 *
 * @param downgraded 出参，是否因 modifier 被拒而降级到不带 modifier 的 addfb2。
 *                   驱动不认某个私有 modifier 时会走这条。
 */
Result<Framebuffer> import_as_framebuffer(BorrowedFd kms_device, HandleCache& cache,
                                          const DmabufDesc& desc, bool* downgraded = nullptr);

// ---------------------------------------------------------------------------
// 行跨距对齐探测
// ---------------------------------------------------------------------------

/**
 * @brief 探测设备要求的行跨距对齐（字节）
 *
 * ## 为什么需要它
 *
 * 有些 KMS 驱动在 addfb2 时校验 `pitch % alignment`，不满足就返回一个
 * 分辨不出原因的 EINVAL。而 pitch 是**分配器**决定的 —— 当分配发生在
 * 另一个设备上（GBM 在 render node 上分配、显示在 KMS 节点上）时，
 * 两边对齐要求不一致就会在 addfb2 处炸掉，且错误信息毫无指向性。
 *
 * ## 怎么探
 *
 * 分配一个 **1x1 的 32bpp dumb buffer**。理论最小 pitch 是 4 字节；
 * 按对齐向上取整的驱动会返回对齐值本身。
 *
 * 这是**纯行为探测，不含任何 vendor 知识**，在任何"按固定值取整"的
 * 驱动上都成立。驱动不做对齐就返回 4，表示无约束。
 *
 * ## 使用限制（重要）
 *
 * **只用于诊断，不用于强制。** dumb 分配器用的对齐值与 addfb2 校验用的
 * 对齐值是不是同一个，属于驱动实现细节，不保证。所以正确用法是：
 * addfb2 失败之后，用探到的值给出一条有指向性的提示，
 * 而不是提前拒绝分配。
 *
 * @return 对齐字节数；探测失败（无 dumb 能力等）返回 nullopt
 *
 * @note 会产生一次 create_dumb + destroy_dumb，只应在初始化阶段调用。
 */
std::optional<uint32_t> probe_pitch_alignment(BorrowedFd device);

/**
 * @brief 给定 pitch 是否满足探测到的对齐
 *
 * @param alignment probe_pitch_alignment() 的结果。nullopt 或 <= 1 时恒真。
 */
bool pitch_is_aligned(uint32_t pitch, std::optional<uint32_t> alignment) noexcept;

} // namespace mw::drm
