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

# 没指定动作时按心情自动配一个：低落低头、平常点头、高兴招呼
if [ -z "$MOVE" ]; then
  M="${MIYU_MOOD:-50}"
  if [ "$M" -lt 45 ]; then MOVE=低头
  elif [ "$M" -lt 60 ]; then MOVE=点头
  else MOVE=招呼; fi
fi

# 动作名三种写法都认：plan 文件名、.json 路径、或者片段名（走 body-move.sh）
if [ -n "$MOVE" ]; then
  case "$MOVE" in
    *.json)
      [ -f "$MOVE" ] && "$HERE/body-plan.sh" "$MOVE" >/dev/null 2>&1 || true ;;
    *)
      if [ -f "$HERE/../plans/$MOVE.json" ]; then
        "$HERE/body-plan.sh" "$HERE/../plans/$MOVE.json" >/dev/null 2>&1 || true
      else
        "$HERE/body-move.sh" $MOVE >/dev/null 2>&1 || \
          echo "[body-speak] 既没有 plans/$MOVE.json，也不是片段：$MOVE" >&2
      fi ;;
  esac
fi
