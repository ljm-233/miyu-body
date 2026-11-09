#!/bin/sh
# 出声：只播**预生成**的常用短句（CosyVoice 合成一句要几十秒，实时不划算）。
#   body-voice.sh "在"        播「在」；没有这句就静默退出（exit 1）
#   body-voice.sh --list      看有哪些
#   body-voice.sh --gen       在后台补生成缺的（跑 voice/gen.sh）
set -u
VOICE="${MIYU_VOICE_DIR:-/home/link/.dsh/miyu/body/voice}"
case "${1:---list}" in
  --list) ls "$VOICE"/*.wav 2>/dev/null | wc -l | xargs -I{} echo "预生成 {} 句"; cat "$VOICE/index.tsv" 2>/dev/null | cut -f2 | paste -sd' ' - ;;
  --gen)  nohup "$VOICE/gen.sh" > "$VOICE/gen.log" 2>&1 & echo "后台补生成中，日志 $VOICE/gen.log" ;;
  *)
    key=$(printf '%s' "$1" | md5sum | cut -c1-10)
    if [ -f "$VOICE/$key.wav" ]; then
      ( command -v pw-play >/dev/null && pw-play "$VOICE/$key.wav" >/dev/null 2>&1 || mpv --no-video --really-quiet "$VOICE/$key.wav" >/dev/null 2>&1 ) &
      echo "[voice] $1"
    else
      echo "[voice] 没有预生成：$1（跑 --gen 补）" >&2
      exit 1
    fi ;;
esac
