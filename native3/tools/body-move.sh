#!/bin/sh
# 随手拼一个动作播出来：body-move.sh 抬头 点头 害羞
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp /tmp/miyu-plan-XXXX.json)
if "$HERE/plan-build.py" "$@" > "$TMP"; then
  "$HERE/body-plan.sh" "$TMP" && echo "[body-move] 播了: $*"
else
  rm -f "$TMP"; exit 1
fi
rm -f "$TMP"
