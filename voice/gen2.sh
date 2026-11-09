#!/bin/sh
ROOT=/home/link/.dsh/plugins/feiyu-voice
OUT=/home/link/.dsh/miyu/body/voice
PY=$ROOT/runtime/cosy-venv/bin/python
INFER=$ROOT/models/FeiyuVoice-CosyVoice3/infer.py
while IFS= read -r line; do
  [ -z "$line" ] && continue
  key=$(printf '%s' "$line" | md5sum | cut -c1-10)
  [ -f "$OUT/$key.wav" ] && continue
  B=$(mktemp -d); mkdir -p "$B/o"
  printf '{"text":"%s","style":"cute","output_dir":"%s/o"}' "$line" "$B" > "$B/req.json"
  if "$PY" "$INFER" --request "$B/req.json" --style cute >/dev/null 2>&1 && [ -f "$B/o/speech.wav" ]; then
    cp "$B/o/speech.wav" "$OUT/$key.wav"; printf '%s\t%s\n' "$key" "$line" >> "$OUT/index.tsv"; echo "好了: $line"
  else echo "还是失败: $line"; fi
  rm -rf "$B"
done <<'SENT'
嗯嗯
来了呀
收到了
等我一下
你回来了
我在这儿
SENT
echo "=== 补完"
