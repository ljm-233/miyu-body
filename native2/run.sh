#!/usr/bin/env bash
# 起一个（且永远只有一个）native2 身体。
#
# 为什么要有这个脚本：窗口是 GLFW_TRANSPARENT_FRAMEBUFFER + glClearColor(0,0,0,0)，
# 背景是全透明的。只要还剩一个旧 miyu 进程，它的窗口就会和新窗口叠在同一点
# （两边都 glfwSetWindowPos(1900,150)），两个模型透在一起 —— 看起来就是
# "头、躯干、手臂、腿各在各的位置"，也就是"散架"。
# 所以规矩是：先杀干净 → 确认 0 个 → 再起。
set -u
here="$(cd "$(dirname "$0")" && pwd)"

kill_all() {
    ps -eo pid,comm | awk '$2=="miyu"{print $1}' | xargs -r kill 2>/dev/null
    sleep 1
    ps -eo pid,comm | awk '$2=="miyu"{print $1}' | xargs -r kill -9 2>/dev/null
    sleep 1
}

kill_all
left=$(ps -eo comm | grep -cx miyu || true)
if [ "$left" != "0" ]; then
    echo "[run] 还有 $left 个 miyu 没死，先处理掉再起" >&2
    exit 1
fi

cd "$here" || exit 1
env -u WAYLAND_DISPLAY DISPLAY=:0 ./build/miyu > /tmp/miyu-run.log 2>&1 &
echo "[run] 起好了 pid=$!，日志 /tmp/miyu-run.log"
