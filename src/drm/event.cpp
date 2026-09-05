#include "mw/drm/event.hpp"

#include <drm.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "mw/trace/log.hpp"
#include "mw/drm/error.hpp"
#include "mw/drm/trace.hpp"

using internal::Ok;
using internal::Err;
using internal::fmt;
using internal::sys_err;

namespace mw::drm {
namespace {

/// 一次 read 最多取这么多字节。一个事件 32 字节，64 个足够容纳
/// 任何合理情况下积压的事件。
constexpr size_t kEventBufferSize = 32u * 64u;

/**
 * @brief CLOCK_REALTIME -> CLOCK_MONOTONIC 的偏移
 *
 * 只有老驱动（DRM_CAP_TIMESTAMP_MONOTONIC = 0）才需要。
 * 转换本身就是有损的：两个时钟之间的偏移会随 NTP 调整漂移，
 * 我们只能在读到事件的那一刻测一次。所以要 WARN。
 */
uint64_t realtime_to_monotonic_offset_ns() {
    timespec real{};
    timespec mono{};
    clock_gettime(CLOCK_REALTIME, &real);
    clock_gettime(CLOCK_MONOTONIC, &mono);

    const auto real_ns =
        static_cast<int64_t>(real.tv_sec) * 1000000000LL + static_cast<int64_t>(real.tv_nsec);
    const auto mono_ns =
        static_cast<int64_t>(mono.tv_sec) * 1000000000LL + static_cast<int64_t>(mono.tv_nsec);
    return static_cast<uint64_t>(real_ns - mono_ns);
}

bool g_warned_about_clock = false;

} // namespace

std::string FlipEvent::to_string() const {
    return fmt("{} seq={} ts={}.{:09}s{} user_data={}", drm::to_string(crtc), sequence,
               timestamp_ns / 1000000000ULL, timestamp_ns % 1000000000ULL,
               timestamp_monotonic ? "" : " (converted from REALTIME)", user_data);
}

Result<size_t> read_events(BorrowedFd fd, bool timestamp_is_monotonic,
                           const std::function<void(const FlipEvent&)>& on_flip) {
    // 这里不用 drmHandleEvent。自己 read + 解析 struct drm_event 的理由：
    //  1. 结构就这么点东西，自己解析一遍才会真正记住内核回传了什么
    //  2. 回调式接口把"一次 read 里可能有多个事件"藏起来了，而这正是
    //     理解 NONBLOCK 提交语义的关键
    //  3. Step 7 要拿 vblank 时间戳做 frame pacing，直接读结构体字段
    //     比经过 libdrm 的回调签名转换更清楚
    alignas(8) uint8_t buffer[kEventBufferSize];

    const ssize_t got = ::read(fd.get(), buffer, sizeof(buffer));
    ++stats().read_events;

    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // fd 是 O_NONBLOCK 且当前没有事件。不是错误。
            return Ok(size_t{0});
        }
        if (errno == EINTR) {
            return Ok(size_t{0});
        }
        return sys_err("read from DRM fd");
    }
    if (got == 0) {
        return Ok(size_t{0});
    }

    size_t handled = 0;
    size_t offset = 0;
    const auto available = static_cast<size_t>(got);

    while (offset + sizeof(drm_event) <= available) {
        drm_event header{};
        std::memcpy(&header, buffer + offset, sizeof(header));

        if (header.length < sizeof(drm_event) || offset + header.length > available) {
            LOG_WARN("malformed DRM event: length={} at offset={} of {} bytes read",
                     header.length, offset, available);
            break;
        }

        switch (header.type) {
            case DRM_EVENT_FLIP_COMPLETE:
            case DRM_EVENT_VBLANK: {
                if (header.length < sizeof(drm_event_vblank)) {
                    LOG_WARN("vblank event too short: {} bytes", header.length);
                    break;
                }
                drm_event_vblank vblank{};
                std::memcpy(&vblank, buffer + offset, sizeof(vblank));

                FlipEvent event;
                event.user_data = vblank.user_data;
                event.sequence = vblank.sequence;
                // crtc_id 只有在 DRM_CAP_CRTC_IN_VBLANK_EVENT 时才有效；
                // 老内核这个字段是 0。单输出场景无所谓，多输出时要注意。
                event.crtc = CrtcId{vblank.crtc_id};

                const uint64_t raw_ns = static_cast<uint64_t>(vblank.tv_sec) * 1000000000ULL +
                                        static_cast<uint64_t>(vblank.tv_usec) * 1000ULL;
                if (timestamp_is_monotonic) {
                    event.timestamp_ns = raw_ns;
                    event.timestamp_monotonic = true;
                } else {
                    const uint64_t offset_ns = realtime_to_monotonic_offset_ns();
                    event.timestamp_ns = raw_ns - offset_ns;
                    event.timestamp_monotonic = false;
                    if (! g_warned_about_clock) {
                        g_warned_about_clock = true;
                        LOG_WARN("vblank timestamps are CLOCK_REALTIME; converting to MONOTONIC "
                                 "with a sampled offset. Frame pacing will drift if the wall "
                                 "clock is adjusted.");
                    }
                }

                ++stats().page_flip_events;
                ++handled;
                LOG_TRACE("drm event: {}", event.to_string());
                on_flip(event);
                break;
            }
            default:
                // 内核以后可能加新事件类型（比如 6.x 的 writeback 完成事件）。
                // 跳过而不是报错，这是"不绑死内核版本"的一部分。
                LOG_DEBUG("ignoring unknown DRM event type {} ({} bytes)", header.type,
                          header.length);
                break;
        }

        offset += header.length;
    }

    return Ok(handled);
}

Result<bool> wait_readable(BorrowedFd fd, int timeout_ms) {
    pollfd pfd{};
    pfd.fd = fd.get();
    pfd.events = POLLIN;

    const int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) {
            // 被信号打断。上层的退出标志会在下一轮循环被看到。
            return Ok(false);
        }
        return sys_err("poll on DRM fd");
    }
    if (ret == 0) {
        return Ok(false);
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return Err(Errc::Internal, fmt("poll reported revents=0x{:x} on the DRM fd", pfd.revents));
    }
    return Ok((pfd.revents & POLLIN) != 0);
}

// ---------------------------------------------------------------------------
// FrameStats
// ---------------------------------------------------------------------------

void FrameStats::record(const FlipEvent& event) noexcept {
    if (frames > 0) {
        // sequence 是 vblank 计数，不是帧计数。差值 > 1 说明我们错过了
        // 至少一个刷新周期 —— 要么画得太慢，要么提交得太晚。
        // 这是丢帧最直接的证据，比测墙钟时间可靠。
        if (event.sequence > last_sequence) {
            const uint64_t gap = event.sequence - last_sequence;
            if (gap > 1u) {
                dropped += gap - 1u;
            }
        }

        if (event.timestamp_ns > last_timestamp_ns) {
            const uint64_t interval = event.timestamp_ns - last_timestamp_ns;
            sum_interval_ns += interval;
            if (interval < min_interval_ns) {
                min_interval_ns = interval;
            }
            if (interval > max_interval_ns) {
                max_interval_ns = interval;
            }
        }
    }

    last_sequence = event.sequence;
    last_timestamp_ns = event.timestamp_ns;
    ++frames;
}

double FrameStats::avg_interval_ms() const noexcept {
    if (frames < 2) {
        return 0.0;
    }
    return static_cast<double>(sum_interval_ns) / static_cast<double>(frames - 1) / 1e6;
}

double FrameStats::avg_fps() const noexcept {
    const double interval = avg_interval_ms();
    return interval > 0.0 ? 1000.0 / interval : 0.0;
}

std::string FrameStats::to_line() const {
    if (frames < 2) {
        return fmt("frames={} (not enough samples yet)", frames);
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "frames=%llu fps=%.2f interval=%.3fms [%.3f, %.3f] dropped=%llu",
                  static_cast<unsigned long long>(frames), avg_fps(), avg_interval_ms(),
                  static_cast<double>(min_interval_ns) / 1e6,
                  static_cast<double>(max_interval_ns) / 1e6,
                  static_cast<unsigned long long>(dropped));
    return std::string(buf);
}

void FrameStats::reset() noexcept {
    *this = FrameStats{};
}

} // namespace mw::drm
