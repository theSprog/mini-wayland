/**
 * @file drm/dmabuf_map.hpp
 * @brief 直接 CPU 映射一个 dmabuf fd（带 `DMA_BUF_IOCTL_SYNC`）
 *
 * ## 与"分配器给的映射"是两回事
 *
 * `ScanoutBuffer::map_write()` 映射的是**自己分配的那块 bo**，
 * 走的是分配器（dumb / GBM）自己的映射入口。本文件映射的是
 * **一个外来的 dmabuf fd**，走的是 dma-buf 通用 UAPI
 * （`mmap(dmabuf_fd)` + `<linux/dma-buf.h>` 的 sync ioctl）。
 *
 * 两者最大的差别是 **`DMA_BUF_IOCTL_SYNC` 不能省**：
 * 跨设备共享的内存可能被缓存，不发 sync 就直接读，在有缓存一致性
 * 问题的平台上会读到陈旧数据 —— 而且是**间歇性**的。
 * 这正是本项目最怕的那类错误：偶尔对、偶尔错，且没有任何返回码提示。
 *
 * 把它们写成同一个接口是一个分类错误（`lessons.md` L-7 的同款）：
 * **带不带 sync 是两个不同的契约。**
 *
 * ## 这是可选能力，不是保证
 *
 * `mmap(dmabuf_fd)` 需要导出方实现了 dma-buf 的 mmap 操作。
 * 有的驱动不实现（本项目已经实测到过一次，见 `env.md` 第二节）。
 * 所以：
 *
 *  - `create()` 失败**不是错误路径的末端**，是一条能力判断。
 *    调用方应当把它降级成"这一层校验拿不到"，并**明确打印**，
 *    不要让验收静默地少一层。
 *  - 不要用它做功能，只用它做校验。`--verify` 之外的路径不该出现它。
 *
 * ## 它是诊断路径，不是常规路径
 *
 * 每帧 map + sync 会在热路径上引入 ioctl，违反稳态零 ioctl 约束。
 * 用法是"只验前 N 帧"或按需开关。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"

namespace mw::drm {

/// 映射的访问方式。决定 `mmap` 的 prot 与 sync 的方向标志。
enum class MapAccess {
    Read,      ///< 只读（校验用，Step 3 只用这个）
    ReadWrite, ///< 读写。**要求 dmabuf 是以 DRM_RDWR 导出的**，否则 mmap 失败
};

/**
 * @brief 一段 CPU 访问期间：构造时 `SYNC_START`，析构时 `SYNC_END`
 *
 * dma-buf 的规矩是 CPU 访问必须夹在这一对之间，两侧都不能少 ——
 * 少了 START 可能读到陈旧数据，少了 END 设备侧可能看不到写入。
 * 做成 RAII 是因为中间那段有 early return（校验失败就返回），
 * 手写配对迟早会漏一条路径。
 *
 * @note `SYNC_END` 失败只 WARN 不报错：那时 CPU 访问已经结束了，
 *       调用方也做不了什么。但它必须被看见。
 */
class DmabufAccess {
  public:
    DmabufAccess() = default;
    ~DmabufAccess();

    DmabufAccess(DmabufAccess&& other) noexcept;
    DmabufAccess& operator=(DmabufAccess&& other) noexcept;
    DmabufAccess(const DmabufAccess&) = delete;
    DmabufAccess& operator=(const DmabufAccess&) = delete;

    /// 发 `DMA_BUF_IOCTL_SYNC` 的 START
    static Result<DmabufAccess> begin(BorrowedFd dmabuf, MapAccess access);

    /// 提前结束（析构会再做一次，幂等）
    void end() noexcept;

  private:
    DmabufAccess(BorrowedFd dmabuf, MapAccess access) noexcept;

    BorrowedFd dmabuf_{};
    MapAccess access_ = MapAccess::Read;
    bool active_ = false;
};

/**
 * @brief 一个 dmabuf 的 CPU 映射
 *
 * move-only，析构 munmap。**不持有 fd**（借用）—— fd 的所有权在
 * `DmabufDesc` 或 `Message` 手里，这里再持一份只会让所有权变模糊。
 */
class DmabufMapping {
  public:
    DmabufMapping() = default;
    ~DmabufMapping();

    DmabufMapping(DmabufMapping&& other) noexcept;
    DmabufMapping& operator=(DmabufMapping&& other) noexcept;
    DmabufMapping(const DmabufMapping&) = delete;
    DmabufMapping& operator=(const DmabufMapping&) = delete;

    /**
     * @brief `mmap` 整个 dmabuf
     *
     * @param length 映射长度。传 0 表示用 `lseek(fd, 0, SEEK_END)` 问内核 ——
     *               dma-buf 支持这个用法，且比调用方自己按
     *               `stride * height` 算更可靠（分配器可能有额外对齐或
     *               尾部填充，算小了会在最后几行读到越界的东西）。
     *
     * 失败的常见原因写进错误信息里区分：`ENOSYS` / `EINVAL` 通常是
     * 导出方不支持映射（能力问题），`EACCES` 通常是导出时没带 `DRM_RDWR`
     * 却要求了写权限（用法问题）。两者的处理动作完全不同。
     */
    static Result<DmabufMapping> create(BorrowedFd dmabuf, MapAccess access, size_t length = 0);

    span<const uint8_t> bytes() const noexcept;

    /// 只在 `MapAccess::ReadWrite` 下有效，否则返回空 span
    span<uint8_t> mutable_bytes() noexcept;

    size_t size() const noexcept {
        return length_;
    }

    bool valid() const noexcept {
        return addr_ != nullptr;
    }

    /// 开一段 CPU 访问期（`SYNC_START` / `SYNC_END`）
    Result<DmabufAccess> begin_access() const;

    std::string to_string() const;

  private:
    DmabufMapping(void* addr, size_t length, BorrowedFd dmabuf, MapAccess access) noexcept;

    void* addr_ = nullptr;
    size_t length_ = 0;
    BorrowedFd dmabuf_{};
    MapAccess access_ = MapAccess::Read;
};

/**
 * @brief 本设备/本 fd 是否支持 CPU 映射 dmabuf
 *
 * 真的映射一次再解开，**不查任何 cap** —— 没有 cap 能回答这个问题
 * （见 `buffer_source.hpp` 里同样的论证）。
 *
 * @param dmabuf 一个已有的 dmabuf fd，借用
 */
bool dmabuf_cpu_mappable(BorrowedFd dmabuf);

} // namespace mw::drm
