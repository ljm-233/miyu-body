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
  else echo "失败: $line"; fi
  rm -rf "$B"
done <<'SENT'
早上好呀
我先去忙了
等一下哦
你说吧
我听着呢
好呀
真的假的
太好啦
有点累
我想你了
你回来啦
我在这儿呢
陪我一会儿
你吃了吗
早点睡
别熬夜
我等你
慢慢来
不着急
好厉害
我看看
让我想想
嗯嗯嗯
哎
呀
SENT
echo "=== 全好了"
