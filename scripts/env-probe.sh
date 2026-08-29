#!/bin/sh
# 把环境勘察那几条命令固化下来，换板子 / 升内核后重跑一次，
# 输出直接贴进 docs/env.md 做对照。
#
#   ./scripts/env-probe.sh            只读，不需要 root（部分项会缺）
#   sudo ./scripts/env-probe.sh       完整
set -u

sec() { printf '\n===== %s =====\n' "$1"; }

sec "kernel / distro"
uname -a
[ -r /etc/os-release ] && . /etc/os-release && echo "distro: $PRETTY_NAME"

sec "DRM nodes -> driver / PCI"
for d in /sys/class/drm/card[0-9]*; do
    [ -e "$d/device/driver" ] || continue
    case "$d" in *-*) continue;; esac   # 跳过 cardX-HDMI-A-1 这种 connector 目录
    printf '%-24s driver=%-12s dev=%s\n' \
        "$d" \
        "$(basename "$(readlink -f "$d/device/driver")")" \
        "$(basename "$(readlink -f "$d/device")")"
done

sec "DRM driver name (this is what modetest -M wants, not the PCI driver name)"
for n in /dev/dri/card*; do
    printf '%-20s ' "$n"
    # version 节点在 sysfs 里没有统一位置，直接问 modetest
    modetest -M "$(basename "$n")" >/dev/null 2>&1 \
        && echo "(driver name may differ from node name)" \
        || echo ""
done
echo "hint: try 'modetest -M <name> -c' on each; the one with a non-empty resource table is the KMS node"

sec "connector status"
for c in /sys/class/drm/card*-*/status; do
    [ -r "$c" ] || continue
    echo "$(dirname "$c" | xargs basename) => $(cat "$c")"
done

sec "who holds DRM master"
for n in /dev/dri/card*; do
    echo "--- $n"
    lsof "$n" 2>/dev/null || echo "(needs root, or nobody holds it)"
done

sec "userspace graphics stack"
command -v glxinfo   >/dev/null && glxinfo -B 2>/dev/null | head -12
command -v eglinfo   >/dev/null && eglinfo 2>/dev/null | grep -iE 'dma_buf|native_fence|platform_gbm|wait_sync' | sort -u
ls /etc/vulkan/icd.d/ 2>/dev/null || echo "no vulkan icd dir"

sec "debugfs"
ls /sys/kernel/debug/dri/ 2>/dev/null || echo "no debugfs (CONFIG_DEBUG_FS off) -- CRC self-check pipeline unavailable"

sec "build dependencies"
pkg-config --modversion libdrm 2>/dev/null || echo "libdrm-dev missing"
