#!/bin/bash
# 跑 probe → 等 READY → 等 0.6s（让合成器 present）→ grim 截图 → 立刻杀掉
cd /home/link/.dsh/miyu/body/wayland-probe || exit 1
rm -f probe.log /tmp/wl-probe.png /tmp/wl-probe-full.png

./probe > probe.log 2>&1 &
PID=$!
echo "probe pid=$PID"

for i in $(seq 1 100); do
  grep -q '^\[probe\] READY' probe.log 2>/dev/null && break
  if ! kill -0 $PID 2>/dev/null; then echo "probe 提前退出了"; break; fi
  sleep 0.1
done

sleep 0.8

grim -o eDP-1 /tmp/wl-probe.png 2>/tmp/grim.err
echo "grim eDP-1 rc=$? $(cat /tmp/grim.err)"
grim /tmp/wl-probe-full.png 2>>/tmp/grim.err
echo "grim full rc=$?"

sleep 0.2
kill $PID 2>/dev/null
wait $PID 2>/dev/null
pkill -f wayland-probe/probe 2>/dev/null

echo "--- probe.log ---"
cat probe.log
echo "--- files ---"
ls -la /tmp/wl-probe*.png
python3 -c "from PIL import Image; im=Image.open('/tmp/wl-probe.png'); print('eDP-1 shot size', im.size); im2=Image.open('/tmp/wl-probe-full.png'); print('full shot size', im2.size)"
