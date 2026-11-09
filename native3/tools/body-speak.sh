#!/bin/sh
# 说话 + 动作，一条命令。
#   body-speak.sh "文字" [动作名]
# 动作名对应 native3/plans/<名字>.json，比如 招呼 / 害羞 / 点头；
# 也可以直接给 .json 路径。
# 心情默认取 $MIYU_MOOD，没设就 50。
set -u
TEXT="${1:-}"
[ -z "$TEXT" ] && { echo "用法: body-speak.sh \"文字\" [动作名|plan.json]" >&2; exit 1; }
shift || true
MOVE="${1:-}"
HERE=$(cd "$(dirname "$0")" && pwd)

"$HERE/body-say.sh" --mood "${MIYU_MOOD:-50}" "$TEXT" || exit 1

if [ -n "$MOVE" ]; then
  case "$MOVE" in
    *.json) PLAN="$MOVE" ;;
    *)      PLAN="$HERE/../plans/$MOVE.json" ;;
  esac
  if [ -f "$PLAN" ]; then
    "$HERE/body-plan.sh" "$PLAN" >/dev/null 2>&1 || true
  else
    echo "[body-speak] 没这个动作：$PLAN" >&2
  fi
fi
