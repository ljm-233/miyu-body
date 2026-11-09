#!/bin/bash
cd /home/link/.dsh/miyu/body/platform-wayland || exit 1
rm -f /tmp/selftest.log /tmp/pw.png /tmp/pw-full.png
./build/selftest > /tmp/selftest.log 2>&1 &
PID=$!
echo "selftest pid=$PID"
for i in $(seq 1 120); do
  grep -q 'READY' /tmp/selftest.log 2>/dev/null && break
  kill -0 $PID 2>/dev/null || { echo "提前退出"; break; }
  sleep 0.1
done
sleep 0.8
grim -o eDP-1 /tmp/pw.png 2>/tmp/grim.err; echo "grim eDP-1 rc=$? $(cat /tmp/grim.err)"
grim /tmp/pw-full.png 2>>/tmp/grim.err; echo "grim full rc=$?"
sleep 0.2
kill $PID 2>/dev/null; wait $PID 2>/dev/null
pkill -f 'build/selftest' 2>/dev/null
echo "--- log ---"; cat /tmp/selftest.log
