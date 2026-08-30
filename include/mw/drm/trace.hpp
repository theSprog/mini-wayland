/**
 * @file drm/trace.hpp
 * @brief ioctl 日志与计数 —— 用来验证"热路径零 ioctl"是不是真的做到了
 *
 * 项目有一条硬约束：property id 只在 init/modeset 阶段查询，热路径禁止
 * drmModeObjectGetProperties。问题是这条约束光靠代码审查很容易漏 ——
 * 某个看起来无害的辅助函数里调了一次 require()，每帧就多一次 ioctl，
 * 而且完全不影响画面，你根本不会发现。
 *
 * 所以：**每一类 ioctl 都记数**，demo 每秒打印增量。
 * 稳态下 get_properties / get_resources 的增量必须是 0；
 * atomic_commit 的增量必须等于帧数。数字对不上就是有代码越界了。
 *
 * 这也是理解"一帧到底发生了多少次内核往返"的最直接手段。
 *
 * 性能：计数器是 uint64_t 自增，单线程无锁（合成器主循环本来就是单线程）。
 * Trace 日志在等级不够时不求值参数，热路径开销为一次整数比较。
 */
#pragma once

#include <cstdint>
#include <string>

#include "mw/core/log.hpp"

namespace mw::drm {

/**
 * @brief 分类 ioctl 计数
 *
 * 成对的计数（create/destroy）故意分开记，因为"泄漏"的表现就是两者
 * 长期不收敛。程序退出时打一次，差值不为零就说明 RAII 有漏网之鱼。
 */
struct IoctlStats {
    // ---- 只该在 init / modeset 阶段增长 ----
    uint64_t get_cap = 0;
    uint64_t set_client_cap = 0;
    uint64_t get_resources = 0;
    uint64_t get_plane_resources = 0;
    uint64_t get_connector = 0;   ///< 注意：带 force probe 的那个会阻塞几十 ms
    uint64_t get_encoder = 0;
    uint64_t get_plane = 0;
    uint64_t get_properties = 0;  ///< 热路径增量必须为 0
    uint64_t get_property = 0;
    uint64_t get_blob = 0;
    uint64_t create_blob = 0;
    uint64_t destroy_blob = 0;    ///< 应与 create_blob 收敛，否则漏了 RAII
    uint64_t set_master = 0;
    uint64_t drop_master = 0;

    // ---- buffer / framebuffer ----
    uint64_t create_dumb = 0;
    uint64_t map_dumb = 0;
    uint64_t destroy_dumb = 0;    ///< 应与 create_dumb 收敛
    uint64_t add_fb = 0;
    uint64_t rm_fb = 0;           ///< 应与 add_fb 收敛

    // ---- PRIME（Step 2 起）----
    // gem_close 故意**不**与 prime_fd_to_handle 配对统计：内核对同一个
    // dma_buf 的重复导入返回同一个 handle 且不加引用，所以 N 次导入只对应
    // 1 次 close。配平表的准入判据（一种获取、一种释放、一一对应）不满足。
    // 引用计数由 HandleCache 自己维护，用 live_count() 检查。
    // ---- 资源配平（与上面的 ioctl 计数器分开）----
    // ioctl 计数器记的是**尝试次数**，配平表要的是**成功获取次数**，二者不等：
    //   - 失败的 create_dumb 会让 ioctl 计数 +1，但没有资源需要释放
    //   - add_fb 的 modifier 降级路径可能发两次 ioctl 才拿到一个 fb
    // 用 ioctl 计数器做配平会产生假阳性泄漏报告，掩盖真的泄漏。
    // 这些字段由 RAII 类型在**成功**路径上显式递增。
    uint64_t dumb_acquired = 0;
    uint64_t dumb_released = 0;
    uint64_t fb_acquired = 0;
    uint64_t fb_released = 0;
    uint64_t blob_acquired = 0;
    uint64_t blob_released = 0;

    uint64_t prime_handle_to_fd = 0;
    uint64_t prime_fd_to_handle = 0;
    uint64_t gem_close = 0;

    // ---- 每帧 ----
    uint64_t atomic_test = 0;
    uint64_t atomic_commit = 0;
    uint64_t read_events = 0;
    uint64_t page_flip_events = 0;

    // ---- 失败计数，按 errno 分类（定位 vsdrm 那个 EBUSY 用）----
    uint64_t atomic_test_einval = 0;
    uint64_t atomic_test_ebusy = 0;
    uint64_t atomic_test_enospc = 0;
    uint64_t atomic_test_other = 0;
    uint64_t atomic_commit_einval = 0;
    uint64_t atomic_commit_ebusy = 0;
    uint64_t atomic_commit_other = 0;

    /// 多行，**只打印非零项**，避免刷屏
    std::string to_string() const;

    /**
     * @brief 单行紧凑形式，如 "commit=60 flip=60 !getprops=1"
     *
     * **通常应该打在 delta() 的结果上**，不是绝对值 —— 每秒的增量才有
     * 意义。带 `!` 前缀的几项是"稳态下必须为 0"的，在 delta 里一旦非零
     * 就说明热路径越界了。
     */
    std::string to_line() const;

    /// 单行的**全部非零项**，如 "get_resources=1 get_properties=14 ..."。
    /// 和 to_line() 的区别：那个只挑帧循环关心的几项。启动收尾各打一次用这个。
    std::string to_line_full() const;

    /// newer - older，用于打印每秒增量
    static IoctlStats delta(const IoctlStats& newer, const IoctlStats& older) noexcept;
};

/// 进程级全局计数器。单线程，不加锁。
IoctlStats& stats() noexcept;

/**
 * @brief 声明"初始化阶段结束了"
 *
 * modeset 完成后调一次。之后每帧调 check_sealed()，
 * 若 get_resources / get_properties / get_connector 等发生了增长，
 * LOG_ERROR 报出来并指明是哪一项。
 *
 * **故意不 abort**：学习阶段更想看到它继续跑，好观察后果
 * （多一次 ioctl 通常只是掉帧，不会黑屏，正好用来体会"性能 bug 的隐蔽性"）。
 */
void seal_init_phase() noexcept;

/// @param where 调用点标签，出现在报错里，如 "frame loop"
void check_sealed(const char* where) noexcept;

/// 退出时调用：打印全量计数，并对 create/destroy 不配对的项报 warning
void report_leaks_on_exit() noexcept;

/// errno -> 分类计数器的归类，供 atomic 提交路径调用
void record_atomic_test_failure(int err) noexcept;
void record_atomic_commit_failure(int err) noexcept;

} // namespace mw::drm

/**
 * @brief 包住一次返回 int 的 libdrm 调用：Trace 打参数，失败打 errno，顺带记数
 *
 * @param counter_ IoctlStats 的成员名
 * @param call_    实际调用表达式
 * @param ...      给 fmt() 的参数说明，第一个必须是格式串
 *
 * @code
 *   const int ret = MW_DRM_CALL(atomic_commit,
 *                               drmModeAtomicCommit(fd, req, flags, nullptr),
 *                               "fd={} flags={:x} props={}", fd, flags, nprops);
 * @endcode
 *
 * 保证：宏内部保存并恢复 errno，日志本身不会污染调用方看到的 errno。
 */
#define MW_DRM_CALL(counter_, call_, ...)                                     \
    ([&]() -> int {                                                           \
        LOG_TRACE("ioctl " #counter_ " <- " __VA_ARGS__);                      \
        const int mw_ret_ = (call_);                                          \
        const int mw_errno_ = errno;                                          \
        ++::mw::drm::stats().counter_;                                        \
        if (mw_ret_ != 0) {                                                   \
            LOG_DEBUG("ioctl " #counter_ " -> ret={} errno={} ({})", mw_ret_, \
                      mw_errno_, ::mw::drm::errno_name(mw_errno_));           \
        } else {                                                              \
            LOG_TRACE("ioctl " #counter_ " -> ok");                            \
        }                                                                     \
        errno = mw_errno_;                                                    \
        return mw_ret_;                                                       \
    }())

/// 同上，但用于返回指针的 libdrm 调用（drmModeGetResources 等）
#define MW_DRM_CALL_PTR(counter_, call_, ...)                                 \
    ([&]() {                                                                  \
        LOG_TRACE("ioctl " #counter_ " <- " __VA_ARGS__);                      \
        auto* mw_ptr_ = (call_);                                              \
        const int mw_errno_ = errno;                                          \
        ++::mw::drm::stats().counter_;                                        \
        if (mw_ptr_ == nullptr) {                                             \
            LOG_DEBUG("ioctl " #counter_ " -> null, errno={} ({})",           \
                      mw_errno_, ::mw::drm::errno_name(mw_errno_));           \
        }                                                                     \
        errno = mw_errno_;                                                    \
        return mw_ptr_;                                                       \
    }())

namespace mw::drm {

/// "EBUSY" / "EINVAL" / ... 未知的返回 "E?"。仅日志用。
const char* errno_name(int err) noexcept;

} // namespace mw::drm
