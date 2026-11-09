#!/bin/sh
# 出声：优先播缓存，没缓存就后台补生成（下次就有）。
# CosyVoice 合成一句要几十秒，所以「第一次慢、之后快」是这套唯一可行的节奏。
#   body-voice.sh "在"          播；没缓存则后台生成并退出（不阻塞这次说话）
#   body-voice.sh --list        看有哪些
#   body-voice.sh --gen         后台补生成常用句子集
set -u
VOICE="${MIYU_VOICE_DIR:-/home/link/.dsh/miyu/body/voice}"
ROOT=/home/link/.dsh/plugins/feiyu-voice
PY=$ROOT/runtime/cosy-venv/bin/python
INFER=$ROOT/models/FeiyuVoice-CosyVoice3/infer.py
key_of() { printf '%s' "$1" | md5sum | cut -c1-10; }

gen_one() {
  t="$1"; k=$(key_of "$t")
  [ -f "$VOICE/$k.wav" ] && return 0
  B=$(mktemp -d); mkdir -p "$B/o"
  printf '{"text":"%s","style":"cute","output_dir":"%s/o"}' "$t" "$B" > "$B/req.json"
  if "$PY" "$INFER" --request "$B/req.json" --style cute >/dev/null 2>&1 && [ -f "$B/o/speech.wav" ]; then
    cp "$B/o/speech.wav" "$VOICE/$k.wav"
    printf '%s\t%s\n' "$k" "$t" >> "$VOICE/index.tsv"
    echo "[voice] 补好了：$t" >> "$VOICE/gen.log"
  fi
  rm -rf "$B"
}

case "${1:---list}" in
  --list) echo "预生成 $(ls "$VOICE"/*.wav 2>/dev/null | wc -l) 句"; cat "$VOICE/index.tsv" 2>/dev/null | cut -f2 | paste -sd' ' - ;;
  --gen)  nohup "$VOICE/gen.sh" > "$VOICE/gen.log" 2>&1 & echo "后台补生成中" ;;
  *)
    t="$1"; k=$(key_of "$t")
    if [ -f "$VOICE/$k.wav" ]; then
      ( command -v pw-play >/dev/null && pw-play "$VOICE/$k.wav" >/dev/null 2>&1 || mpv --no-video --really-quiet "$VOICE/$k.wav" >/dev/null 2>&1 ) &
      echo "[voice] $t"
    else
      ( gen_one "$t" ) >/dev/null 2>&1 &
      echo "[voice] 没有「$t」，后台生成中（下次就能播）" >&2
      exit 1
    fi ;;
esac
