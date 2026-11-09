#!/usr/bin/env bash
# 让米鱼说一句话 / 换个表情 / 推个心情 —— 往身体的本地控制口塞一行 JSON。
#
#   tools/body-say.sh "今晚先把透明修好"          # 只说
#   tools/body-say.sh --mood 62 "有点累"          # 带心情
#   tools/body-say.sh --expr 前倾 "你回来了"      # 带表情
#   tools/body-say.sh --expr 默认 "..."           # 淡出回默认表情
#   tools/body-say.sh --dur 3.5 "慢慢说"          # 指定时长（默认 max(1.2s, 字数*0.16s)）
#
# socket：$XDG_RUNTIME_DIR/miyu-body.sock，没有就 /tmp/miyu-body-$UID.sock。
# 用 python3 组 JSON + 发（socat 不一定有；python 一定在，而且省得手动转义引号）。
set -u

usage() {
    cat >&2 <<'USAGE'
用法: body-say.sh [--mood 0-100] [--expr 名字] [--dur 秒] "要说的话"
      body-say.sh --expr 前倾            # 只切表情，不说话
      body-say.sh --mood 62              # 只推心情
USAGE
    exit 2
}

mood=""
expr=""
dur=""
have_expr=0
text=""

while [ $# -gt 0 ]; do
    case "$1" in
        --mood)  [ $# -ge 2 ] || usage; mood="$2"; shift 2 ;;
        --expr)  [ $# -ge 2 ] || usage; expr="$2"; have_expr=1; shift 2 ;;
        --dur)   [ $# -ge 2 ] || usage; dur="$2"; shift 2 ;;
        -h|--help) usage ;;
        --) shift; text="$*"; break ;;
        -*) echo "不认识的选项：$1" >&2; usage ;;
        *)  text="$1"; shift ;;
    esac
done

if [ -z "$text" ] && [ -z "$mood" ] && [ "$have_expr" = 0 ]; then
    usage
fi

SOCK="${XDG_RUNTIME_DIR:-/tmp}/miyu-body.sock"
if [ ! -S "$SOCK" ]; then
    SOCK="/tmp/miyu-body-$(id -u).sock"
    if [ ! -S "$SOCK" ]; then
        echo "[body-say] 找不到 socket（$XDG_RUNTIME_DIR/miyu-body.sock 或 $SOCK）—— 身体起了吗？" >&2
        exit 1
    fi
fi

MOOD="$mood" EXPR="$expr" HAVE_EXPR="$have_expr" DUR="$dur" SAY="$text" SOCK="$SOCK" \
python3 - <<'PY'
import json, os, socket, sys

sock_path = os.environ["SOCK"]
payload = {}

say = os.environ.get("SAY", "")
if say:
    payload["say"] = say
if os.environ.get("HAVE_EXPR") == "1":
    payload["expr"] = os.environ.get("EXPR", "")
if os.environ.get("MOOD", "") != "":
    try:
        payload["mood"] = int(os.environ["MOOD"])
    except ValueError:
        sys.exit("[body-say] --mood 要一个整数，给的是：%r" % os.environ["MOOD"])
if os.environ.get("DUR", "") != "":
    try:
        payload["dur"] = float(os.environ["DUR"])
    except ValueError:
        sys.exit("[body-say] --dur 要一个数字，给的是：%r" % os.environ["DUR"])

line = json.dumps(payload, ensure_ascii=False) + "\n"

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(2.0)
try:
    s.connect(sock_path)
    s.sendall(line.encode("utf-8"))
    s.shutdown(socket.SHUT_WR)   # 身体那边读到 EOF 就当这一行收完
finally:
    s.close()

print("[body-say] -> %s  %s" % (sock_path, line.strip()))
PY
