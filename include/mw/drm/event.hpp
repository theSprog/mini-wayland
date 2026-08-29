/**
 * @file drm/event.hpp
 * @brief 读 DRM fd 上的完成事件（page flip / vblank）
 *
 * ## 为什么不用 drmHandleEvent
 *
 * libdrm 提供了 `drmHandleEvent` + `drmEventContext` 的回调式接口。
 * 这里不用它，自己 read(2) + 解析 `struct drm_event` 头，理由：
 *
 *  1. 事件结构本身就那么点东西（type + length，后面跟
 *     `struct drm_event_vblank`：user_data / tv_sec / tv_usec /
 *     sequence / crtc_id）。自己解析一遍才会真正记住内核回传了什么。
 *  2. 回调式接口会把"一次 read 里可能有多个事件"这件事藏起来，
 *     而这正是理解 nonblock 提交语义的关键。
 *  3. Step 7 要拿 vblank 时间戳做 frame pacing，直接拿结构体字段
 *     比经过 libdrm 的回调签名转换更清楚。
 *
 * ## 时间戳
 *
 * tv_sec/tv_usec 的时钟域由 `DRM_CAP_TIMESTAMP_MONOTONIC` 决定：
 * 为 1 时是 CLOCK_MONOTONIC，为 0 时是 CLOCK_REALTIME（老驱动）。
 * caps 里探到了这一位，这里据此换算，**不假设**是 monotonic。
 *
 * ## 注意
 *
 * DRM fd 默认是阻塞的。合成器主循环应该 poll/epoll 之后再 read，
 * 或者把 fd 设成 O_NONBLOCK。read_events() 不自己 poll，
 * 由调用方的事件循环决定何时调 —— Step 4 会把它并进 wl_event_loop。
 */
#pragma once

#include <cstdint>
#include <functional>

#include "mw/core/error.hpp"
#include "mw/core/unique_fd.hpp"
#include "mw/drm/types.hpp"

namespace mw::drm {

/// 一次翻页/vblank 完成
struct FlipEvent {
    CrtcId crtc = kNoCrtc;

    /// 提交时传给 commit() 的 user_data，原样回来。
    /// 通常用来标识"这是哪一帧"，进而知道哪个 buffer 可以复用了。
    uint64_t user_data = 0;

    /// 硬件出光时刻，纳秒。已按 caps.timestamp_monotonic 归一到
    /// CLOCK_MONOTONIC；若原始时钟是 REALTIME，转换过程会 LOG_WARN 一次。
    uint64_t timestamp_ns = 0;

    /// vblank 序号。丢帧检测靠它：相邻两帧序号差 > 1 就是漏了。
    uint32_t sequence = 0;

    /// 原始时钟是否就是 monotonic（false 表示做过换算，精度有损）
    bool timestamp_monotonic = true;

    std::string to_string() const;
};

/**
 * @brief 读并解析当前可读的全部事件
 *
 * 一次 read 可能返回多个事件，全部解析后逐个回调。
 * 遇到不认识的事件类型（比如内核新增的）跳过并 LOG_DEBUG，不报错。
 *
 * @param timestamp_is_monotonic 来自 DeviceCaps::timestamp_monotonic
 * @param on_flip 每个 page flip / vblank 事件调一次
 *
 * @return 处理的事件数；fd 无数据（EAGAIN）时返回 0，不算错误
 */
Result<size_t> read_events(BorrowedFd fd, bool timestamp_is_monotonic,
                           const std::function<void(const FlipEvent&)>& on_flip);

/**
 * @brief 等到至少一个事件可读，或超时
 *
 * 内部 poll(2)。Step 1 的帧循环用它就够；Step 4 之后会被
 * wayland 的事件循环取代。
 *
 * @param timeout_ms 负数表示无限等
 * @return true 表示 fd 可读；false 表示超时
 */
Result<bool> wait_readable(BorrowedFd fd, int timeout_ms);

/**
 * @brief 帧节拍统计
 *
 * 挂在 FlipEvent 上累积，Step 1 就开始收集 —— 早点建立"这块板子
 * 正常帧间隔长什么样"的基线，Step 7 做 frame pacing 时才有对照。
 */
struct FrameStats {
    uint64_t frames = 0;
    uint64_t dropped = 0;        ///< sequence 跳变检测出的丢帧数
    uint64_t last_sequence = 0;
    uint64_t last_timestamp_ns = 0;

    uint64_t min_interval_ns = UINT64_MAX;
    uint64_t max_interval_ns = 0;
    uint64_t sum_interval_ns = 0;

    /// 喂一个事件进来
    void record(const FlipEvent& ev) noexcept;

    double avg_interval_ms() const noexcept;
    double avg_fps() const noexcept;

    /// 一行摘要："frames=600 fps=59.94 interval=16.68ms [16.60,16.75] dropped=0"
    std::string to_line() const;

    void reset() noexcept;
};

} // namespace mw::drm
