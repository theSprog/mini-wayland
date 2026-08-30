#!/bin/bash
# mini-wayland 环境能力检查
#
#   ./scripts/check-env.sh              # 全量，结果落到文件
#   ./scripts/check-env.sh -q           # 只打结论
#   sudo ./scripts/check-env.sh -k      # 加上内核侧细节（需要 root）
#
# ## 用途
#
# 平台能力会随时间解锁 —— 内核升级、驱动补功能、Mesa 更新、编译配置改变。
# 这个脚本可以反复跑，回答"现在能支撑到第几步、被什么挡住"。
#
# 跑完把输出存档，下次升级后再跑一遍 diff，就能看出解锁了什么。
#
# 不取 DRM master、不做 modeset，桌面跑着也能用。

set -u

QUIET=0
KERNEL=0
OUT="check-env-$(date +%Y%m%d-%H%M%S).txt"

while getopts "qko:h" opt; do
    case "$opt" in
        q) QUIET=1 ;;
        k) KERNEL=1 ;;
        o) OUT="$OPTARG" ;;
        h) sed -n '2,18p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

cd "$(dirname "$0")/.." || exit 1
BIN=./build/debug/bin
exec > >(tee "$OUT") 2>&1

echo "mini-wayland environment check"
echo "date:    $(date -Is)"
echo "kernel:  $(uname -r)"
echo "user:    $(id -un)"
echo

# ---------------------------------------------------------------------------
echo "=== build ==="
if ! make WERROR=1 -j"$(nproc)" 2>&1 | tail -5; then
    echo "!! build failed; nothing below can be trusted"
    exit 1
fi
echo

# ---------------------------------------------------------------------------
echo "=== capability gates ==="
# 这是主要结论。真的分配 buffer 去测查询回答不了的东西。
if [ "$QUIET" = "1" ]; then
    "$BIN/probe_caps" -q
else
    "$BIN/probe_caps"
fi
GATES_RC=$?
echo
echo "(probe_caps exit: $GATES_RC -- 0 all clear, 2 something blocked)"
echo

if [ "$QUIET" = "1" ]; then
    echo "results in $OUT"
    exit $GATES_RC
fi

# ---------------------------------------------------------------------------
echo "=== device inventory ==="
"$BIN/probe_render" -m
echo

# ---------------------------------------------------------------------------
echo "=== PRIME correctness ==="
# 与能力无关，是回归测试：引用计数、fb 引用语义。
"$BIN/step2_prime_roundtrip" -s 1920x1080
echo "(exit: $?)"
echo

# ---------------------------------------------------------------------------
echo "=== GL stack ==="
for dir in /usr/local/lib/dri /usr/lib/x86_64-linux-gnu/dri /usr/lib/dri /usr/lib64/dri; do
    [ -d "$dir" ] || continue
    echo "--- $dir ---"
    # megadriver 构建下多个 *_dri.so 共享同一个 inode。
    ls -li "$dir"/*_dri.so 2>/dev/null | awk '{print $1, $NF}' | sort -n | head -20
    echo
done

for tool in eglinfo es2_info glxinfo; do
    if command -v "$tool" >/dev/null 2>&1; then
        echo "--- $tool ---"
        "$tool" 2>&1 | grep -iE "renderer|vendor|version" | head -8
        break
    fi
done
echo

# ---------------------------------------------------------------------------
if [ "$KERNEL" = "1" ]; then
    echo "=== kernel side ==="
    echo "--- graphics modules ---"
    lsmod 2>/dev/null | awk 'NR==1 || /drm|gpu|gbm/' | head -15
    echo
    echo "--- DRM config ---"
    for f in /proc/config.gz "/boot/config-$(uname -r)"; do
        [ -e "$f" ] || continue
        { [ "${f##*.}" = "gz" ] && zcat "$f" || cat "$f"; } \
            | grep -E "^CONFIG_DRM|^CONFIG_CMA=|IOMMU_SUPPORT" | head -20
        break
    done
    echo
    echo "--- recent DRM messages ---"
    dmesg 2>/dev/null | grep -iE "drm|gpu" | tail -20
    echo
fi

# ---------------------------------------------------------------------------
echo "==================================================================="
echo "results in $OUT"
echo
echo "keep this file. after a kernel / driver / Mesa update, run again and"
echo "diff the two -- gates that moved from BLOCK to PASS are newly unlocked"
echo "capabilities, and the project can start relying on them."
echo "==================================================================="
exit $GATES_RC
