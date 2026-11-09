#!/bin/sh
# 预生成常用语音。一句话一次推理（CosyVoice 冷启动很慢，所以后台慢慢跑）。
ROOT=/home/link/.dsh/plugins/feiyu-voice
OUT=/home/link/.dsh/miyu/body/voice
PY=$ROOT/runtime/cosy-venv/bin/python
INFER=$ROOT/models/FeiyuVoice-CosyVoice3/infer.py
IDX=$OUT/index.tsv
[ -f "$IDX" ] || : > "$IDX"
while IFS= read -r line; do
  [ -z "$line" ] && continue
  key=$(printf '%s' "$line" | md5sum | cut -c1-10)
  if [ -f "$OUT/$key.wav" ]; then echo "跳过（已有）: $line"; continue; fi
  B=$(mktemp -d); mkdir -p "$B/o"
  printf '{"text":"%s","style":"cute","output_dir":"%s/o"}' "$line" "$B" > "$B/req.json"
  if "$PY" "$INFER" --request "$B/req.json" --style cute >/dev/null 2>&1 && [ -f "$B/o/speech.wav" ]; then
    cp "$B/o/speech.wav" "$OUT/$key.wav"
    printf '%s\t%s\n' "$key" "$line" >> "$IDX"
    echo "好了: $line -> $key.wav"
  else
    echo "失败: $line"
  fi
  rm -rf "$B"
done <<'SENT'
在
嗯
好
我在
怎么了
来了
收到
等一下
有点困
晚安
SENT
echo "=== 全部完成"
