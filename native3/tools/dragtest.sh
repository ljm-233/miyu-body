#!/usr/bin/env bash
# 拖动验收：一轮做完 截图→注入→截图→比对
#
#   用法: dragtest.sh DX DY STEPS STEP_MS
#   产出: /tmp/drag-before.png /tmp/drag-after.png + 数字
set -u
cd "$(dirname "$0")/.." || exit 1
DX=${1:--100}; DY=${2:-0}; STEPS=${3:-10}; STEP_MS=${4:-60}
LOG=/tmp/miyu-wayland.log

# 等它安静下来（真人可能正在玩鼠标），最多等 20 秒
quiet=0
for _ in $(seq 1 20); do
    n1=$(grep -c '\[dbg\]' "$LOG")
    sleep 1
    n2=$(grep -c '\[dbg\]' "$LOG")
    if [ "$n1" = "$n2" ]; then quiet=$((quiet+1)); else quiet=0; fi
    [ "$quiet" -ge 3 ] && break
done
echo "[test] 静默 $quiet 秒后开测"

grim -o eDP-1 /tmp/drag-before.png || exit 1
P=$(python3 tools/bbox.py /tmp/drag-before.png /tmp/drag-ref.png --head | sed -n 's/.*= (\(.*\), \(.*\))/\1 \2/p')
PX=$(echo "$P" | cut -d' ' -f1); PY=$(echo "$P" | cut -d' ' -f2)
echo "[test] 按点 ($PX,$PY)  注入位移 ($DX,$DY) 分 $STEPS 步"
before_margin=$(grep -o '结束拖动：margin=([0-9-]*,[0-9-]*)' "$LOG" | tail -1)
echo "[test] 注入前最后一次 margin: ${before_margin:-（还没拖过）}"

tools/draginject "$PX" "$PY" "$DX" "$DY" "$STEPS" "$STEP_MS" --rel 2>/dev/null
sleep 0.6
grim -o eDP-1 /tmp/drag-after.png || exit 1

echo '--- 拖动前 ---'; python3 tools/bbox.py /tmp/drag-before.png /tmp/drag-ref.png
echo '--- 拖动后 ---'; python3 tools/bbox.py /tmp/drag-after.png /tmp/drag-ref.png
echo '--- 拖动日志 ---'
grep -E '开始拖动|结束拖动' "$LOG" | tail -2
