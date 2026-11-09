#!/usr/bin/env bash
# 推一份动作计划给身体 —— 她照着演，不用重启、不用碰渲染。
#
#   tools/body-plan.sh plans/招呼.json
#   tools/body-plan.sh --stop            # {"plan":null}：立刻停下，交回待机
#
# 干的事：读文件 → JSON 压成一行 → 包成 {"plan":{...}} → 写进 socket。
# 为什么不直接 cat 文件：ctl 那边是「一行一条 JSON」，多行文件会被当成半截行。
# 用 python3 压（json.loads/dumps），顺手做语法检查 —— 文件写错当场报，不会推个半截过去。
set -u

usage() {
    cat >&2 <<'USAGE'
用法: body-plan.sh <plan.json>
      body-plan.sh --stop
USAGE
    exit 2
}

[ $# -ge 1 ] || usage

SOCK="${XDG_RUNTIME_DIR:-/tmp}/miyu-body.sock"
if [ ! -S "$SOCK" ]; then
    SOCK="/tmp/miyu-body-$(id -u).sock"
    if [ ! -S "$SOCK" ]; then
        echo "[body-plan] 找不到 socket（$XDG_RUNTIME_DIR/miyu-body.sock 或 $SOCK）—— 身体起了吗？" >&2
        exit 1
    fi
fi

if [ "$1" = "--stop" ]; then
    PLAN="" SOCK="$SOCK" python3 - <<'PY'
import json, os, socket
line = json.dumps({"plan": None}) + "\n"
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
try:
    s.connect(os.environ["SOCK"]); s.sendall(line.encode()); s.shutdown(socket.SHUT_WR)
finally:
    s.close()
print("[body-plan] 停止计划 -> %s" % os.environ["SOCK"])
PY
    exit 0
fi

FILE="$1"
[ -f "$FILE" ] || { echo "[body-plan] 文件不存在：$FILE" >&2; exit 1; }

FILE="$FILE" SOCK="$SOCK" python3 - <<'PY'
import json, os, socket, sys

path = os.environ["FILE"]
try:
    with open(path, encoding="utf-8") as f:
        plan = json.load(f)
except Exception as e:
    sys.exit("[body-plan] %s 读不成 JSON：%s" % (path, e))

line = json.dumps({"plan": plan}, ensure_ascii=False, separators=(",", ":")) + "\n"

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
try:
    s.connect(os.environ["SOCK"])
    s.sendall(line.encode("utf-8"))
    s.shutdown(socket.SHUT_WR)      # 身体读到 EOF 就当这一行收完（和 body-say 一样）
finally:
    s.close()

acts = plan.get("actions", [])
total = sum(float(a.get("duration") or 0) for a in acts) if acts else float(plan.get("duration") or 0)
print("[body-plan] -> %s  「%s」%d 个 action，%.2fs，%d 字节"
      % (os.environ["SOCK"], plan.get("title", "(无题)"), len(acts), total, len(line)))
PY
