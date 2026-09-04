#!/usr/bin/env bash
#
# Step 3 acceptance run.
#
# Usage:
#   ./scripts/step3-acceptance.sh                 # everything, auto-picked KMS node
#   sudo ./scripts/step3-acceptance.sh -D /dev/dri/card2
#   sudo ./scripts/step3-acceptance.sh -D /dev/dri/card4    # VKMS
#   ./scripts/step3-acceptance.sh --phase 0       # only the no-root self-check
#
# Options:
#   -D <path>     KMS node to use (default: let the demo pick the first one)
#   -g <path>     GBM node, only needed for the render allocation path
#   -f <n>        frames for the main run (default 600)
#   --phase <n>   run a single phase: 0 smoke, 1 dry-run, 2 main, 3 faults
#   -o <dir>      log directory (default: build/acceptance-step3)
#
# Phases 1-3 need DRM master, so they need root and no display server:
#   sudo systemctl stop lightdm      # or switch to a bare tty with Ctrl+Alt+F3
#
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT}/build/debug/bin"

DEV=""
GBM=""
FRAMES=600
PHASE="all"
LOGDIR="${ROOT}/build/acceptance-step3"

while [ $# -gt 0 ]; do
    case "$1" in
        -D) DEV="$2"; shift 2 ;;
        -g) GBM="$2"; shift 2 ;;
        -f) FRAMES="$2"; shift 2 ;;
        -o) LOGDIR="$2"; shift 2 ;;
        --phase) PHASE="$2"; shift 2 ;;
        -h|--help) sed -n '3,25p' "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done

DEV_ARGS=()
[ -n "${DEV}" ] && DEV_ARGS+=(-D "${DEV}")
[ -n "${GBM}" ] && DEV_ARGS+=(-g "${GBM}")

mkdir -p "${LOGDIR}"

PASS=0
FAIL=0

banner() {
    echo
    echo "=============================================================="
    echo "$*"
    echo "=============================================================="
}

# Runs a command, tees to a log, and records the exit status.
# The log is the artifact -- read it when something fails.
run_step() {
    local name="$1"; shift
    local log="${LOGDIR}/${name}.log"
    echo "--- ${name}"
    echo "    $*"
    if "$@" >"${log}" 2>&1; then
        echo "    PASS  (log: ${log})"
        PASS=$((PASS + 1))
        return 0
    fi
    echo "    FAIL  (log: ${log})"
    FAIL=$((FAIL + 1))
    return 1
}

# A fault case passes when the server REJECTS it: the client must exit 0 after
# receiving an ERROR, nothing may go on screen, and there must be no crash.
run_fault() {
    local name="$1"
    local log="${LOGDIR}/fault-${name}.log"
    echo "--- fault ${name}"
    if "${BIN}/step3_dmabuf_ipc" --spawn --fault "${name}" -f 30 "${DEV_ARGS[@]}" \
            >"${log}" 2>&1; then
        if grep -q "rejecting\|server rejected it" "${log}"; then
            echo "    PASS  rejected with a diagnosable error"
            grep -m1 "rejecting\|server rejected it" "${log}" | sed 's/^/          /'
            PASS=$((PASS + 1))
        else
            echo "    FAIL  exited cleanly but nothing rejected it (log: ${log})"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "    FAIL  non-zero exit (log: ${log})"
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x "${BIN}/step3_dmabuf_ipc" ]; then
    banner "building"
    make -C "${ROOT}" -j"$(nproc)" || exit 1
fi

# ---------------------------------------------------------------------------
# phase 0 -- no hardware, no root
# ---------------------------------------------------------------------------
if [ "${PHASE}" = "all" ] || [ "${PHASE}" = "0" ]; then
    banner "phase 0: mw/ipc self-check (no root, no hardware)"
    run_step "smoke_ipc" "${BIN}/smoke_ipc"
    tail -n 3 "${LOGDIR}/smoke_ipc.log"
fi

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo
        echo "phases 1-3 need DRM master; re-run with sudo:"
        echo "  sudo $0 $*"
        exit 1
    fi
}

# ---------------------------------------------------------------------------
# phase 1 -- modeset TEST_ONLY only, nothing goes on screen
# ---------------------------------------------------------------------------
if [ "${PHASE}" = "all" ] || [ "${PHASE}" = "1" ]; then
    need_root "$@"
    banner "phase 1: modeset dry run"
    run_step "dry-run" "${BIN}/step3_dmabuf_ipc" --dry-run "${DEV_ARGS[@]}"
fi

# ---------------------------------------------------------------------------
# phase 2 -- the real thing
# ---------------------------------------------------------------------------
if [ "${PHASE}" = "all" ] || [ "${PHASE}" = "2" ]; then
    need_root "$@"
    banner "phase 2: ${FRAMES} frames across the process boundary"
    run_step "main" "${BIN}/step3_dmabuf_ipc" --spawn -f "${FRAMES}" --verify=8 "${DEV_ARGS[@]}"

    log="${LOGDIR}/main.log"
    echo
    echo "  content check (L1/L2):"
    grep -E "content check|verification unavailable|L2 gap" "${log}" | sed 's/^/    /' || \
        echo "    (nothing -- did --verify run?)"
    echo "  steady-state ioctl budget:"
    grep -E "re-binding ioctl" "${log}" | sed 's/^/    /' || \
        echo "    clean: no add_fb / prime_* in the steady state"
    echo "  frame pacing:"
    grep -E "frame loop finished|fps=" "${log}" | tail -n 2 | sed 's/^/    /'
    echo "  release timing pressure:"
    grep -E "slot wait|never waited" "${log}" | sed 's/^/    /'
    echo "  balance at exit:"
    grep -E "handle cache|leak|acquired|released" "${log}" | sed 's/^/    /'
fi

# ---------------------------------------------------------------------------
# phase 3 -- fault injection
# ---------------------------------------------------------------------------
if [ "${PHASE}" = "all" ] || [ "${PHASE}" = "3" ]; then
    need_root "$@"
    banner "phase 3: fault injection (every case must be rejected)"
    for fault in bad-stride bad-offset missing-fd extra-fd not-dmabuf \
                 tiny-buffer bad-modifier stale-header half-message; do
        run_fault "${fault}"
    done
fi

banner "summary: ${PASS} passed, ${FAIL} failed"
echo "logs in ${LOGDIR}"
echo
echo "still to check by hand (no automation covers these):"
echo "  - the picture is correct and does not tear"
echo "  - the 32-pixel signature strip is visible in the top-left corner"
echo "  - dmesg is clean (a kernel BUG_ON looks exactly like a userspace crash"
echo "    from waitpid's point of view)"
exit $([ "${FAIL}" -eq 0 ] && echo 0 || echo 1)
