#!/usr/bin/env bash
# regression.sh — 一键回归：启动探针 → 点圆心 → 点左上角 → SIGTERM → 检查清理
set -u
cd /home/link/.dsh/miyu/body/input-probe || exit 1

rm -f run.log
WAYLAND_DEBUG=1 ./input-probe >/dev/null 2>run.log &
PID=$!
sleep 1.2
echo "probe pid=$PID alive=$(kill -0 $PID 2>/dev/null && echo yes || echo no)"

echo "--- click 圆心 (2280,360) ---"
./clicker 2280 360 2>&1 | grep -v 绑定
sleep 0.25
echo "--- click 左上角 (2100,180) ---"
./clicker 2100 180 2>&1 | grep -v 绑定
sleep 0.25
echo "--- click 圆内靠左 (2130,360) ---"
./clicker 2130 360 2>&1 | grep -v 绑定
sleep 0.3

echo "=== 指针事件 ==="
grep -E "^\[ptr\]|^\[region\]" run.log

echo "=== SIGTERM ==="
T0=$(date +%s%N)
kill -TERM "$PID"
for _ in $(seq 1 40); do kill -0 "$PID" 2>/dev/null || break; sleep 0.05; done
T1=$(date +%s%N)
if kill -0 "$PID" 2>/dev/null; then
	echo "!! 进程仍活着，强杀"
	kill -9 "$PID"
fi
echo "退出耗时 $(( (T1-T0)/1000000 )) ms"
sleep 0.3
echo "残留进程: $(pgrep -x input-probe | wc -l)"
echo "残留 layer: $(niri msg layers 2>&1 | grep -cE 'probe|recorder')"
echo "protocol error: $(grep -cE '\.error|protocol error' run.log)"
