#!/usr/bin/env bash
# 起一个（且永远只有一个）米鱼的身体。
#
# 用法：
#   ./run.sh            X11(XWayland) 版 —— 备胎，build/miyu
#   ./run.sh --wayland  原生 Wayland 版 —— 正路，build/miyu-wayland
#
# 为什么要先杀干净：窗口背景是全透明的（glClearColor(0,0,0,0)），只要还剩一个旧进程，
# 它的窗口就和新窗口叠在同一点，两个模型透在一起 —— 看起来像"散架"。
# 所以规矩永远是：先杀干净 → 确认 0 个 → 再起。
set -u
here="$(cd "$(dirname "$0")" && pwd)"

mode="x11"
case "${1:-}" in
    --wayland|-w) mode="wayland" ;;
    "")           mode="x11" ;;
    *) echo "用法: $0 [--wayland]" >&2; exit 2 ;;
esac

kill_all() {
    local sig="$1" n pids
    for n in miyu miyu-wayland; do
        pids=$(ps -eo pid,comm | awk -v n="$n" '$2==n{print $1}')
        [ -n "$pids" ] && kill $sig $pids 2>/dev/null
    done
    sleep 1
}

kill_all ""
kill_all -9

left=$(ps -eo comm | grep -cxE 'miyu|miyu-wayland' || true)
if [ "$left" != "0" ]; then
    echo "[run] 还有 $left 个没死，先处理掉再起" >&2
    exit 1
fi

cd "$here" || exit 1

if [ "$mode" = "wayland" ]; then
    # Wayland 版**不能** unset WAYLAND_DISPLAY —— 它就是要连 wayland socket。
    # （X11 版反过来：不 unset 就会开出 GLFW 的 Wayland 窗口，见 wayland-migration.md 坑 1）
    ./build/miyu-wayland > /tmp/miyu-wayland.log 2>&1 &
    echo "[run] Wayland 版起好了 pid=$!，日志 /tmp/miyu-wayland.log"
else
    env -u WAYLAND_DISPLAY DISPLAY=:0 ./build/miyu > /tmp/miyu-run.log 2>&1 &
    echo "[run] X11 版起好了 pid=$!，日志 /tmp/miyu-run.log"
fi
