/**
 * @file drm/dumb_buffer.hpp
 * @brief CPU 可写的 dumb buffer —— Step 1 唯一的像素来源
 *
 * dumb buffer 是 DRM 提供的"最笨但通用"的显存分配方式：
 *
 *   DRM_IOCTL_MODE_CREATE_DUMB   驱动分配一块线性、可 scanout 的内存，
 *                                返回 GEM handle + pitch + size
 *   DRM_IOCTL_MODE_MAP_DUMB      换来一个 mmap 偏移
 *   mmap(drm_fd, ..., offset)    映射到用户态，直接写像素
 *   DRM_IOCTL_MODE_DESTROY_DUMB  释放
 *
 * 它的"笨"体现在三点，正好是后面几个 Step 要解决的：
 *  - 只能线性排布，GPU 读它很慢 -> Step 2 的 modifier 就是为了解决这个
 *  - 不能跨进程共享（没有 PRIME 语义保证）-> Step 3 换成 GBM + DMA-BUF
 *  - CPU 写 WC 内存，带宽很低，1080p 全屏刷一遍在弱板子上就要几 ms
 *
 * 所以 Step 1 之后它只会留在自检路径里。但它是理解
 * "GEM handle -> fb_id -> plane -> CRTC" 这条链的最短路径，不能跳过。
 *
 * @warning dumb buffer 的 mmap 内存通常是 **write-combining**：
 *          顺序写很快，随机读极慢。画图时按行顺序写，不要读回。
 */
#pragma once

#include <cstdint>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/framebuffer.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {

class DumbBuffer {
  public:
    DumbBuffer() = default;
    ~DumbBuffer();

    DumbBuffer(DumbBuffer&& other) noexcept;
    DumbBuffer& operator=(DumbBuffer&& other) noexcept;
    DumbBuffer(const DumbBuffer&) = delete;
    DumbBuffer& operator=(const DumbBuffer&) = delete;

    /**
     * @brief 分配 + 映射。**不注册 fb。**
     *
     * @note 早先的版本把 addfb2 一起做了，理由是"分配了却没有 fb 的 dumb
     *       buffer 没有用途"。这个判断是错的，两个场景直接推翻它：
     *
     *       1. **没有 KMS 的节点也能分配 dumb**。`CREATE_DUMB` 是 primary
     *          node 上就放行的 ioctl，而 addfb2 只有显示设备接受。想在一个
     *          非显示节点上分配 buffer 再导给显示设备（跨设备导入的验证），
     *          捆绑 addfb2 会让它在分配阶段就失败，且报错指向 addfb2，
     *          完全掩盖了真正要测的东西。
     *       2. **client 进程不是 master，也不该建 fb**。Step 3 起 client
     *          只负责分配和画，fb 是合成器的事。
     *
     *       所以注册 fb 是独立的一步，见 `register_framebuffer()`。
     *
     * @param bpp 每像素位数。只支持 32（XR24/AR24）；其他值返回 Unsupported。
     *            dumb buffer 的 bpp 与 format 必须自洽，这里不做转换表，
     *            需要别的格式时用 GBM（Step 2）。
     */
    static Result<DumbBuffer> create(BorrowedFd fd, Size size, Format format,
                                     uint32_t bpp = 32);

    /**
     * @brief 在**同一个 fd** 上把这块内存注册成 KMS framebuffer
     *
     * 只在有 KMS 的节点上有意义。已经注册过时直接返回成功（幂等）。
     *
     * 走朴素的 `drmModeAddFB2`（modifier 记为 kModifierInvalid）而不是
     * 显式 LINEAR：dumb buffer 的排布本来就该由驱动说了算，
     * 两条路径的语义差别见 framebuffer.hpp。
     */
    Status register_framebuffer();

    // ---- 几何 ----

    Size size() const noexcept {
        return size_;
    }

    /// 行跨距，字节。**不等于** width * 4，驱动会按硬件要求对齐。
    /// 画图必须用它做行偏移，用 width*4 在对齐不为 1 的设备上会撕裂。
    uint32_t pitch() const noexcept {
        return pitch_;
    }

    uint64_t byte_size() const noexcept {
        return byte_size_;
    }

    Format format() const noexcept {
        return format_;
    }

    GemHandle handle() const noexcept {
        return handle_;
    }

    const Framebuffer& framebuffer() const noexcept {
        return fb_;
    }

    /**
     * @brief 把 fb 的所有权移出去
     *
     * 用于把 dumb buffer 的 fb 交给别的容器持有，而 GEM 对象仍留在本对象里。
     * 移出之后 `fb_id()` 返回 kNoFb。
     *
     * 调用方必须保证 fb 不会活得比本对象久 —— fb 引用的 GEM 对象归本对象所有，
     * 本对象析构时会 DESTROY_DUMB。
     */
    Framebuffer take_framebuffer() noexcept {
        return std::move(fb_);
    }

    FbId fb_id() const noexcept {
        return fb_.id();
    }

    // ---- 像素访问 ----

    /// 整块映射。写完不需要 flush —— dumb buffer 是 coherent 的。
    span<uint8_t> bytes() noexcept;

    /**
     * @brief 第 y 行的起始地址，按 32bpp 解释
     *
     * 内联且不做边界检查（debug 构建下 ASan 会兜底）。
     * 这是 Step 1 的热路径：1080p 每帧 1080 次调用。
     */
    uint32_t* row(uint32_t y) noexcept {
        return reinterpret_cast<uint32_t*>(pixels_ + static_cast<size_t>(y) * pitch_);
    }

    /// 整块填成同一个值。内部用 memset 的快路径（当四个字节相同时）。
    void fill(uint32_t argb) noexcept;

    bool valid() const noexcept {
        return handle_ != GemHandle{0};
    }

    /// 一行摘要，日志用："1920x1080 XR24 pitch=7680 size=8294400 fb=42"
    std::string to_string() const;

  private:
    BorrowedFd fd_{};
    Size size_{};
    Format format_{};
    uint32_t pitch_ = 0;
    uint64_t byte_size_ = 0;
    GemHandle handle_{0};
    uint8_t* pixels_ = nullptr;  ///< mmap 结果，析构 munmap
    Framebuffer fb_{};
};

/**
 * @brief 双缓冲（或多缓冲）的 dumb buffer 组
 *
 * Step 1 必须有它：正在被扫描的 buffer 不能同时被 CPU 改写，
 * 否则会看到撕裂。KMS 的 page flip 完成事件到达之前，
 * 上一帧的 buffer 仍在被硬件读。
 *
 * 用法：
 * @code
 *   auto& buf = chain.acquire();      // 取一个当前没在扫描的
 *   draw(buf);
 *   commit(buf.fb_id());
 *   // ... 收到 flip 完成事件后 ...
 *   chain.on_flip_complete();
 * @endcode
 *
 * 故意不做成"自动等待"的阻塞接口：让调用方显式处理
 * "提交了但还没上屏"这个中间状态，Step 6 的显式同步就是在这个状态上做文章。
 */
class DumbBufferChain {
  public:
    static constexpr uint32_t kMaxBuffers = 3;

    DumbBufferChain() = default;

    /// @param count 2 = 双缓冲，3 = 三缓冲。Step 1 用 2 就够。
    static Result<DumbBufferChain> create(BorrowedFd fd, Size size, Format format,
                                          uint32_t count = 2);

    /// 当前可以安全写入的 buffer
    DumbBuffer& acquire() noexcept;

    /// 标记 acquire() 返回的那个已经提交，轮转到下一个
    /// @param expects_event 这次提交带了 PAGE_FLIP_EVENT 吗。modeset 那一次
    ///        不带：它让第一块 buffer 开始被扫描（要轮转），但内核不会为它
    ///        投递 flip 事件，计入 in_flight 会造成一个恒定的偏差。
    void mark_submitted(bool expects_event = true) noexcept;

    /// 收到 page flip 完成事件时调用，释放上上一帧
    void on_flip_complete() noexcept;

    uint32_t count() const noexcept {
        return count_;
    }

    /// 尚未收到完成事件的提交数。>= count 说明该等了。
    uint32_t in_flight() const noexcept {
        return in_flight_;
    }

  private:
    std::array<DumbBuffer, kMaxBuffers> buffers_{};
    uint32_t count_ = 0;
    uint32_t next_ = 0;
    uint32_t in_flight_ = 0;
};

} // namespace mw::drm
