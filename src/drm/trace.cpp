#include "mw/drm/trace.hpp"

#include <cerrno>

#include "mw/trace/log.hpp"

namespace mw::drm {
namespace {

IoctlStats g_stats{};

/// seal_init_phase() 时的快照。check_sealed() 拿它比对。
IoctlStats g_sealed{};
bool g_is_sealed = false;

/// 已经报过的越界项不再重复刷屏
bool g_seal_reported = false;

void append_if_nonzero(std::string& out, const char* name, uint64_t value) {
    if (value == 0u) {
        return;
    }
    if (! out.empty()) {
        out += '\n';
    }
    out += "  ";
    out += name;
    out += " = ";
    out += std::to_string(value);
}

void append_pair(std::string& out, const char* name, uint64_t value) {
    if (value == 0u) {
        return;
    }
    if (! out.empty()) {
        out += ' ';
    }
    out += name;
    out += '=';
    out += std::to_string(value);
}

/**
 * @brief 必须配平的"分配 / 释放"计数对
 *
 * 只放**真正的内核对象分配**。判据是：有且只有一种获取方式，
 * 有且只有一种释放方式，两者一一对应。
 *
 * **DRM master 不在这张表里**，虽然它看起来像一对。原因：
 * master 不是分配出来的对象，是一个"模式"，而且获取方式有两种 ——
 * 打开一个没人占用的节点时会**隐式成为 master**（不产生任何 ioctl），
 * 显式抢占才走 drmSetMaster。释放却只有 drmDropMaster 一种。
 * 把它当成对配平项，会在"隐式获取 + 显式释放"这条正常路径上误报泄漏。
 * master 的状态单独报告，见下面的 report_leaks_on_exit()。
 */
struct PairedCounter {
    const char* name;
    uint64_t IoctlStats::*acquire;
    uint64_t IoctlStats::*release;
};

constexpr PairedCounter kPairs[] = {
    // 用 *_acquired/*_released 而不是 ioctl 计数器，理由见 trace.hpp 的注释：
    // ioctl 计数是尝试次数，配平要的是成功次数。
    {"property blob", &IoctlStats::blob_acquired, &IoctlStats::blob_released},
    {"dumb buffer", &IoctlStats::dumb_acquired, &IoctlStats::dumb_released},
    {"framebuffer", &IoctlStats::fb_acquired, &IoctlStats::fb_released},
};

/// 密封之后禁止再增长的项
struct SealedCounter {
    const char* name;
    uint64_t IoctlStats::*field;
};

constexpr SealedCounter kSealed[] = {
    {"get_resources", &IoctlStats::get_resources},
    {"get_plane_resources", &IoctlStats::get_plane_resources},
    {"get_connector", &IoctlStats::get_connector},
    {"get_encoder", &IoctlStats::get_encoder},
    {"get_plane", &IoctlStats::get_plane},
    {"get_properties", &IoctlStats::get_properties},
    {"get_property", &IoctlStats::get_property},
    {"get_cap", &IoctlStats::get_cap},
    {"set_client_cap", &IoctlStats::set_client_cap},
};

} // namespace

IoctlStats& stats() noexcept {
    return g_stats;
}

std::string IoctlStats::to_string() const {
    std::string out;
    append_if_nonzero(out, "get_cap", get_cap);
    append_if_nonzero(out, "set_client_cap", set_client_cap);
    append_if_nonzero(out, "get_resources", get_resources);
    append_if_nonzero(out, "get_plane_resources", get_plane_resources);
    append_if_nonzero(out, "get_connector", get_connector);
    append_if_nonzero(out, "get_encoder", get_encoder);
    append_if_nonzero(out, "get_plane", get_plane);
    append_if_nonzero(out, "get_properties", get_properties);
    append_if_nonzero(out, "get_property", get_property);
    append_if_nonzero(out, "get_blob", get_blob);
    append_if_nonzero(out, "create_blob", create_blob);
    append_if_nonzero(out, "destroy_blob", destroy_blob);
    append_if_nonzero(out, "set_master", set_master);
    append_if_nonzero(out, "drop_master", drop_master);
    append_if_nonzero(out, "create_dumb", create_dumb);
    append_if_nonzero(out, "map_dumb", map_dumb);
    append_if_nonzero(out, "destroy_dumb", destroy_dumb);
    append_if_nonzero(out, "add_fb", add_fb);
    append_if_nonzero(out, "rm_fb", rm_fb);
    append_if_nonzero(out, "prime_handle_to_fd", prime_handle_to_fd);
    append_if_nonzero(out, "prime_fd_to_handle", prime_fd_to_handle);
    append_if_nonzero(out, "gem_close", gem_close);
    append_if_nonzero(out, "atomic_test", atomic_test);
    append_if_nonzero(out, "atomic_commit", atomic_commit);
    append_if_nonzero(out, "read_events", read_events);
    append_if_nonzero(out, "page_flip_events", page_flip_events);
    append_if_nonzero(out, "atomic_test_einval", atomic_test_einval);
    append_if_nonzero(out, "atomic_test_ebusy", atomic_test_ebusy);
    append_if_nonzero(out, "atomic_test_enospc", atomic_test_enospc);
    append_if_nonzero(out, "atomic_test_other", atomic_test_other);
    append_if_nonzero(out, "atomic_commit_einval", atomic_commit_einval);
    append_if_nonzero(out, "atomic_commit_ebusy", atomic_commit_ebusy);
    append_if_nonzero(out, "atomic_commit_other", atomic_commit_other);
    if (out.empty()) {
        out = "  (no ioctls recorded)";
    }
    return out;
}

std::string IoctlStats::to_line() const {
    std::string out;
    append_pair(out, "commit", atomic_commit);
    append_pair(out, "test", atomic_test);
    append_pair(out, "flip", page_flip_events);
    append_pair(out, "addfb", add_fb);
    append_pair(out, "rmfb", rm_fb);
    // 下面这几项在稳态下应该恒为 0，一旦非零就说明热路径越界了
    append_pair(out, "!getprops", get_properties);
    append_pair(out, "!getres", get_resources);
    if (out.empty()) {
        out = "(idle)";
    }
    return out;
}

std::string IoctlStats::to_line_full() const {
    std::string out;
    append_pair(out, "get_cap", get_cap);
    append_pair(out, "set_client_cap", set_client_cap);
    append_pair(out, "get_resources", get_resources);
    append_pair(out, "get_plane_resources", get_plane_resources);
    append_pair(out, "get_connector", get_connector);
    append_pair(out, "get_encoder", get_encoder);
    append_pair(out, "get_plane", get_plane);
    append_pair(out, "get_properties", get_properties);
    append_pair(out, "get_property", get_property);
    append_pair(out, "get_blob", get_blob);
    append_pair(out, "create_blob", create_blob);
    append_pair(out, "destroy_blob", destroy_blob);
    append_pair(out, "set_master", set_master);
    append_pair(out, "drop_master", drop_master);
    append_pair(out, "create_dumb", create_dumb);
    append_pair(out, "map_dumb", map_dumb);
    append_pair(out, "destroy_dumb", destroy_dumb);
    append_pair(out, "add_fb", add_fb);
    append_pair(out, "rm_fb", rm_fb);
    append_pair(out, "atomic_test", atomic_test);
    append_pair(out, "atomic_commit", atomic_commit);
    append_pair(out, "read_events", read_events);
    append_pair(out, "page_flip_events", page_flip_events);

    uint64_t total = get_cap + set_client_cap + get_resources + get_plane_resources +
                     get_connector + get_encoder + get_plane + get_properties + get_property +
                     get_blob + create_blob + destroy_blob + set_master + drop_master +
                     create_dumb + map_dumb + destroy_dumb + add_fb + rm_fb + atomic_test +
                     atomic_commit + read_events;
    append_pair(out, "TOTAL", total);

    return out.empty() ? std::string("(idle)") : out;
}

IoctlStats IoctlStats::delta(const IoctlStats& newer, const IoctlStats& older) noexcept {
    IoctlStats d{};
    // 逐字段相减。字段多但都是同构的，写成宏反而更难读。
    d.get_cap = newer.get_cap - older.get_cap;
    d.set_client_cap = newer.set_client_cap - older.set_client_cap;
    d.get_resources = newer.get_resources - older.get_resources;
    d.get_plane_resources = newer.get_plane_resources - older.get_plane_resources;
    d.get_connector = newer.get_connector - older.get_connector;
    d.get_encoder = newer.get_encoder - older.get_encoder;
    d.get_plane = newer.get_plane - older.get_plane;
    d.get_properties = newer.get_properties - older.get_properties;
    d.get_property = newer.get_property - older.get_property;
    d.get_blob = newer.get_blob - older.get_blob;
    d.create_blob = newer.create_blob - older.create_blob;
    d.destroy_blob = newer.destroy_blob - older.destroy_blob;
    d.set_master = newer.set_master - older.set_master;
    d.drop_master = newer.drop_master - older.drop_master;
    d.create_dumb = newer.create_dumb - older.create_dumb;
    d.map_dumb = newer.map_dumb - older.map_dumb;
    d.destroy_dumb = newer.destroy_dumb - older.destroy_dumb;
    d.add_fb = newer.add_fb - older.add_fb;
    d.rm_fb = newer.rm_fb - older.rm_fb;
    d.dumb_acquired = newer.dumb_acquired - older.dumb_acquired;
    d.dumb_released = newer.dumb_released - older.dumb_released;
    d.fb_acquired = newer.fb_acquired - older.fb_acquired;
    d.fb_released = newer.fb_released - older.fb_released;
    d.blob_acquired = newer.blob_acquired - older.blob_acquired;
    d.blob_released = newer.blob_released - older.blob_released;
    d.prime_handle_to_fd = newer.prime_handle_to_fd - older.prime_handle_to_fd;
    d.prime_fd_to_handle = newer.prime_fd_to_handle - older.prime_fd_to_handle;
    d.gem_close = newer.gem_close - older.gem_close;
    d.atomic_test = newer.atomic_test - older.atomic_test;
    d.atomic_commit = newer.atomic_commit - older.atomic_commit;
    d.read_events = newer.read_events - older.read_events;
    d.page_flip_events = newer.page_flip_events - older.page_flip_events;
    d.atomic_test_einval = newer.atomic_test_einval - older.atomic_test_einval;
    d.atomic_test_ebusy = newer.atomic_test_ebusy - older.atomic_test_ebusy;
    d.atomic_test_enospc = newer.atomic_test_enospc - older.atomic_test_enospc;
    d.atomic_test_other = newer.atomic_test_other - older.atomic_test_other;
    d.atomic_commit_einval = newer.atomic_commit_einval - older.atomic_commit_einval;
    d.atomic_commit_ebusy = newer.atomic_commit_ebusy - older.atomic_commit_ebusy;
    d.atomic_commit_other = newer.atomic_commit_other - older.atomic_commit_other;
    return d;
}

void seal_init_phase() noexcept {
    g_sealed = g_stats;
    g_is_sealed = true;
    g_seal_reported = false;
    LOG_INFO("ioctl accounting sealed; init phase used:\n{}", g_stats.to_string());
    LOG_INFO("from now on the counters below must stay flat; growth means a hot-path violation");
}

void check_sealed(const char* where) noexcept {
    if (! g_is_sealed || g_seal_reported) {
        return;
    }
    for (const auto& entry : kSealed) {
        const uint64_t before = g_sealed.*(entry.field);
        const uint64_t now = g_stats.*(entry.field);
        if (now > before) {
            LOG_ERROR(
                "hot-path violation at '{}': {} grew from {} to {} after the init phase was sealed",
                where, entry.name, before, now);
            LOG_ERROR("  this is an ioctl per frame that should have been cached at modeset time");
            // 故意不 abort：学习阶段更想看到它继续跑，好观察后果。
            // 报一次就够，不刷屏。
            g_seal_reported = true;
            return;
        }
    }
}

void report_leaks_on_exit() noexcept {
    LOG_INFO("final ioctl counts:\n{}", g_stats.to_string());

    bool leaked = false;
    for (const auto& pair : kPairs) {
        const uint64_t acquired = g_stats.*(pair.acquire);
        const uint64_t released = g_stats.*(pair.release);
        if (acquired == released) {
            continue;
        }
        leaked = true;
        // 两个方向都要能正确显示。无符号相减会下溢成天文数字，
        // 反而掩盖了"释放多于获取"这种同样值得警惕的情况。
        if (acquired > released) {
            LOG_WARN("resource leak: {} acquired={} released={} ({} still held)", pair.name,
                     acquired, released, acquired - released);
        } else {
            LOG_WARN("resource accounting bug: {} released={} exceeds acquired={} by {}",
                     pair.name, released, acquired, released - acquired);
        }
    }
    if (! leaked) {
        LOG_INFO("all paired kernel resources released cleanly");
    }

    // master 单独报告，不参与配平检查（见 kPairs 上方的说明）。
    // 正常路径有两种：
    //   隐式获取 + 显式释放  ->  set=0 drop=1
    //   显式获取 + 显式释放  ->  set=1 drop=1
    if (g_stats.set_master != 0u || g_stats.drop_master != 0u) {
        LOG_INFO("drm master: {} explicit acquisition(s), {} release(s){}", g_stats.set_master,
                 g_stats.drop_master,
                 g_stats.set_master == 0u && g_stats.drop_master != 0u
                     ? " (master was inherited on open, not taken via ioctl)"
                     : "");
    }
}

void record_atomic_test_failure(int err) noexcept {
    switch (err) {
        case EINVAL: ++g_stats.atomic_test_einval; break;
        case EBUSY:  ++g_stats.atomic_test_ebusy; break;
        case ENOSPC: ++g_stats.atomic_test_enospc; break;
        default:     ++g_stats.atomic_test_other; break;
    }
}

void record_atomic_commit_failure(int err) noexcept {
    switch (err) {
        case EINVAL: ++g_stats.atomic_commit_einval; break;
        case EBUSY:  ++g_stats.atomic_commit_ebusy; break;
        default:     ++g_stats.atomic_commit_other; break;
    }
}

const char* errno_name(int err) noexcept {
    switch (err) {
        case 0:       return "OK";
        case EPERM:   return "EPERM";
        case ENOENT:  return "ENOENT";
        case EINTR:   return "EINTR";
        case EIO:     return "EIO";
        case ENXIO:   return "ENXIO";
        case EAGAIN:  return "EAGAIN";
        case ENOMEM:  return "ENOMEM";
        case EACCES:  return "EACCES";
        case EFAULT:  return "EFAULT";
        case EBUSY:   return "EBUSY";
        case ENODEV:  return "ENODEV";
        case EINVAL:  return "EINVAL";
        case ENOSPC:  return "ENOSPC";
        case ERANGE:  return "ERANGE";
        case ENOSYS:  return "ENOSYS";
        // Linux 上 ENOTSUP == EOPNOTSUPP，只能列一个
        case ENOTSUP: return "ENOTSUP";
        case EBADF:   return "EBADF";
        case EDEADLK: return "EDEADLK";
        default:      return "E?";
    }
}

} // namespace mw::drm
