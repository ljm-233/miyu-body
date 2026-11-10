#!/bin/sh
# 出声：优先播缓存，没缓存就后台补生成（下次就有）。
# CosyVoice 合成一句要几十秒，所以「第一次慢、之后快」是这套唯一可行的节奏。
#
#   body-voice.sh "在"             播；没缓存则后台补生成并立刻返回（不阻塞说话）
#   body-voice.sh --warm "文字"     只补生成、不播（body-say.sh 每说一句顺手调一次）
#   body-voice.sh --list           看有哪些
#   body-voice.sh --gen [文件]      批量补生成（默认读 $VOICE/corpus.txt），失败跳过不中断
#   body-voice.sh --gen-one "文字"  内部用：真跑一次合成（flock 串行；失败记 gen.log + failed.tsv）
#
# 合成失败的三档回退（2026-11-10 实测：「等一下」用 cute 合成出来的音频过不了
# infer.py 最后那条 len(audio)>2400 断言，被直接判死）：
#   1) cute   原文
#   2) gentle 原文
#   3) cute   原文+语气词「呀」（只对 ≤4 字）
# 三档都失败才记一次失败；同一句失败 2 次后不再重试（免得每次说话都白烧一次推理）。
set -u
VOICE="${MIYU_VOICE_DIR:-/home/link/.dsh/miyu/body/voice}"
ROOT=/home/link/.dsh/plugins/feiyu-voice
PY=$ROOT/runtime/cosy-venv/bin/python
INFER=$ROOT/models/FeiyuVoice-CosyVoice3/infer.py
GENLOG="$VOICE/gen.log"
FAILED="$VOICE/failed.tsv"
LOCK="$VOICE/.gen.lock"
MAXLEN="${MIYU_VOICE_MAXLEN:-30}"     # 超过这个字数不缓存（合成慢、复用率低）

key_of() { printf '%s' "$1" | md5sum | cut -c1-10; }
now() { date '+%Y-%m-%d %H:%M:%S'; }
chars() { printf '%s' "$1" | LC_ALL=C.UTF-8 wc -m; }

fail_count() {   # 这句已经失败过几次
    [ -f "$FAILED" ] || { echo 0; return 0; }
    awk -F'\t' -v t="$1" '$1==t{n=$2} END{print n+0}' "$FAILED"
}
fail_bump() {    # $1=文本 $2=原因
    n=$(( $(fail_count "$1") + 1 ))
    printf '%s\t%s\t%s\t%s\n' "$1" "$n" "$(now)" "$2" >> "$FAILED"
}

# try_synth 文本 key style → 0 成功；失败时原因写进 $REASON
try_synth() {
    t="$1"; k="$2"; style="$3"; REASON=""
    B=$(mktemp -d) || { REASON="mktemp 失败"; return 1; }
    mkdir -p "$B/o"
    # JSON 交给 python 写：文本里带引号/反斜杠时手写 printf 会把 JSON 弄坏
    if ! python3 - "$t" "$style" "$B/o" "$B/req.json" <<'PY'
import json, sys
t, style, out, req = sys.argv[1:5]
json.dump({"text": t, "style": style, "output_dir": out},
          open(req, "w", encoding="utf-8"), ensure_ascii=False)
PY
    then
        REASON="写 request JSON 失败"; rm -rf "$B"; return 1
    fi
    if "$PY" "$INFER" --request "$B/req.json" --style "$style" >"$B/out.log" 2>"$B/err.log"; then
        if [ -f "$B/o/speech.wav" ]; then
            if cp "$B/o/speech.wav" "$VOICE/$k.wav"; then
                rm -rf "$B"; return 0
            fi
            REASON="拷 wav 失败"; rm -rf "$B"; return 1
        fi
        REASON="infer.py 退出码 0 但没有 speech.wav"
    else
        # infer.py 的断言只写「AssertionError」，看不出是太短还是太轻，这里标清楚
        tail=$(tail -n 1 "$B/err.log" 2>/dev/null | tr -d '\r')
        case "$tail" in
            AssertionError*) REASON="infer.py:41 断言没过（音频 <0.1s 或近静音）" ;;
            *)               REASON="infer.py 失败：${tail:-无 stderr}" ;;
        esac
    fi
    rm -rf "$B"
    return 1
}

# 真跑一次合成，成功才落盘 + 进 index
gen_one() {
    t="$1"
    [ -z "$t" ] && return 1
    k=$(key_of "$t")
    [ -f "$VOICE/$k.wav" ] && { echo "跳过（已有）: $t"; return 0; }
    n=$(chars "$t")

    if try_synth "$t" "$k" cute; then
        printf '%s\t%s\n' "$k" "$t" >> "$VOICE/index.tsv"
        echo "好了: $t -> $k.wav"
        return 0
    fi
    last="$REASON"

    # 第二次机会：gentle 音色，语速/停顿不同，常能过那条 0.1 秒的坎
    if try_synth "$t" "$k" gentle; then
        printf '%s\t%s\n' "$k" "$t" >> "$VOICE/index.tsv"
        echo "好了: $t（gentle 重试） -> $k.wav"
        return 0
    fi
    last="$REASON"

    # 第三次：≤4 字加一个语气词再合成；wav 仍按原句的 key 存（多一个字，总比没声好）
    if [ "$n" -le 4 ]; then
        ext="${t}呀"
        if try_synth "$ext" "$k" cute; then
            printf '%s\t%s\n' "$k" "$t" >> "$VOICE/index.tsv"
            echo "好了: $t（合成用「$ext」） -> $k.wav"
            return 0
        fi
        last="$REASON"
    fi

    echo "失败: $t（$n 字）原因：$last"
    fail_bump "$t" "$last"
    return 1
}

# 后台补一条：spawn 出去就返回，绝不阻塞说话
warm() {
    t="$1"
    [ -z "$t" ] && return 0
    [ "$(chars "$t")" -gt "$MAXLEN" ] && return 0
    k=$(key_of "$t")
    [ -f "$VOICE/$k.wav" ] && return 0
    [ "$(fail_count "$t")" -ge 2 ] && return 0
    nohup sh "$0" --gen-one "$t" >>"$GENLOG" 2>&1 </dev/null &
    return 0
}

case "${1:---list}" in
  --list)
      echo "预生成 $(ls "$VOICE"/*.wav 2>/dev/null | wc -l) 句"
      cut -f2 "$VOICE/index.tsv" 2>/dev/null | paste -sd' ' - ;;
  --warm)
      shift; warm "${1:-}" ;;
  --gen-one)
      shift; t="${1:-}"
      [ -z "$t" ] && exit 2
      ( flock -w 300 9
        gen_one "$t" ) 9>"$LOCK" ;;
  --gen)
      shift
      F="${1:-$VOICE/corpus.txt}"
      [ -f "$F" ] || { echo "没有语料文件：$F" >&2; exit 1; }
      echo "=== $(now) 批量补生成（$F）"
      while IFS= read -r line; do
          [ -z "$line" ] && continue
          case "$line" in \#*) continue ;; esac
          sh "$0" --gen-one "$line"    # 失败已在 gen_one 里记过，继续下一条
      done < "$F"
      echo "=== $(now) 全部完成：$(ls "$VOICE"/*.wav 2>/dev/null | wc -l) 句" ;;
  *)
      t="$1"; k=$(key_of "$t")
      if [ -f "$VOICE/$k.wav" ]; then
          ( command -v pw-play >/dev/null && pw-play "$VOICE/$k.wav" >/dev/null 2>&1 || mpv --no-video --really-quiet "$VOICE/$k.wav" >/dev/null 2>&1 ) &
          echo "[voice] $t"
      else
          warm "$t"
          echo "[voice] 没有「$t」，后台生成中（下次就能播）" >&2
          exit 1
      fi ;;
esac
