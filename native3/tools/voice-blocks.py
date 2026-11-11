#!/usr/bin/env python3
"""词块：把缓存里的短句修剪成能拼的块，再按顺序拼成句子。

2026-11-11 16:2x —— 他退游戏后让我去生成，验出来的东西写在这儿。

**为什么需要修剪**：CosyVoice 念单个词会拖很长——「可以」全长 1.88 秒，
有声的部分只有 0.33 秒（1.39→1.71），前面一大段静音、后面一串尾音。
直接拼这些文件，句子会一顿一顿、中间大片空白。

**修剪之后**（按 -40dB 找有声区间，首尾各留 20/50ms、加 6ms 淡入淡出）：

    可以   1.88s → 0.42s
    不用   2.47s → 0.94s
    自己   1.02s → 0.54s
    怎么了 1.60s → 0.50s
    呀     1.60s → 0.45s

拼「可以自己」得 0.958 秒 —— 正常语速，接得上。

用法：

    voice-blocks.py --trim 可以 不用 自己      # 生成/刷新词块（存到 voice/blocks/）
    voice-blocks.py --say 可以自己             # 按词表贪心切分，拼一句，输出到 /tmp
    voice-blocks.py --list                     # 现有词块

**局限（别忘）**：
- 只认词表里有的词。句子切不干净就报缺哪个
- 拼出来没有整句的韵律（轻重、拖长音全丢），短句还行，长句像电报
- 「知道」「嗯」这种能量太低的词，阈值 -40dB 判不出有声区间，会被跳过
"""
import argparse
import os
import re
import subprocess
import sys

VOICE = os.path.expanduser("~/.dsh/miyu/body/voice")
BLOCKS = os.path.join(VOICE, "blocks")
BLOCKS_TSV = os.path.join(BLOCKS, "index.tsv")
NOISE = "-35dB"   # 2026-11-11 17:5x：-40dB 太宽松，把气声当静音，切出来的块头尾带一截空的
MIND = "0.03"


def key_of(text):
    r = subprocess.run(["bash", "-c", f"printf '%s' {shq(text)} | md5sum | cut -c1-10"],
                       capture_output=True, text=True)
    return r.stdout.strip()


def shq(s):
    return "'" + s.replace("'", "'\\''") + "'"


def duration(path):
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration",
                        "-of", "csv=p=0", path], capture_output=True, text=True)
    try:
        return float(r.stdout.strip())
    except ValueError:
        return 0.0


def voiced_range(path):
    """返回 (起, 止)。找不到有声区间就返回 None。"""
    r = subprocess.run(["ffmpeg", "-hide_banner", "-i", path,
                        "-af", f"silencedetect=noise={NOISE}:d={MIND}", "-f", "null", "-"],
                       capture_output=True, text=True)
    starts = [float(x) for x in re.findall(r"silence_start: ([\d.]+)", r.stderr)]
    ends = [float(x) for x in re.findall(r"silence_end: ([\d.]+)", r.stderr)]
    d = duration(path)
    if d <= 0:
        return None
    segs, prev = [], 0.0
    for i, s in enumerate(starts):
        if s - prev > 0.03:
            segs.append((prev, s))
        prev = ends[i] if i < len(ends) and ends[i] > s else s
    if d - prev > 0.03:
        segs.append((prev, d))
    if not segs:
        return None
    # 2026-11-11 18:0x：根因找到了。CosyVoice 念单字会出**好几段有声**，中间夹着
    # 大半秒静音（「我」= 0-0.09 有声、0.09-0.36 静音、0.36-0.79 有声）。
    # 之前取「第一段到最后一整段」，等于把中间那段静音原样端出来，拼起来就一顿一顿。
    # 现在只保留**最长的那段**（一般就是主音），前后各留 25/45ms。
    main = max(segs, key=lambda ab: ab[1] - ab[0])
    return max(0.0, main[0] - 0.025), min(d, main[1] + 0.045)


def load_index():
    idx = {}
    if os.path.exists(BLOCKS_TSV):
        with open(BLOCKS_TSV, encoding="utf-8") as f:
            for line in f:
                parts = line.rstrip("\n").split("\t")
                if len(parts) >= 2:
                    idx[parts[1]] = parts[0]
    return idx


def trim(words):
    os.makedirs(BLOCKS, exist_ok=True)
    idx = load_index()
    ok = 0
    for w in words:
        k = key_of(w)
        src = os.path.join(VOICE, k + ".wav")
        if not os.path.exists(src):
            print(f"跳过 {w}：缓存里没有这句（先 body-voice.sh --gen-one {shq(w)}）")
            continue
        vr = voiced_range(src)
        if not vr:
            print(f"跳过 {w}：找不到有声区间（能量太低，{NOISE} 判不出来）")
            continue
        a, b = vr
        span = b - a
        dst = os.path.join(BLOCKS, key_of(w) + ".wav")
        # 2026-11-11 17:3x：这里踩了个大坑 —— 之前用 `-ss a -t span` 再叠 afade，
        # 切出来的文件**全是静音**（-91dB）。他说「听不到」，我量了才发现。
        # 原因：`-ss` 之后音频 PTS 是从 a 开始的绝对时间，而 afade 的 st= 也按绝对时间算，
        # `afade=t=out:st=0.494` 落在 1.33 之前 → 整个片段一进来就已经淡完了。
        # 改成全在滤镜里做：atrim 切 → asetpts 归零 → 再淡入淡出。这样 st 才真是相对起点。
        fade_out = max(0.0, span - 0.006)
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src,
                        "-af", f"atrim=start={a:.3f}:end={b:.3f},asetpts=PTS-STARTPTS,"
                               f"afade=t=in:st=0:d=0.006,afade=t=out:st={fade_out:.3f}:d=0.006",
                        "-ar", "24000", "-ac", "1", dst], check=True)
        idx[w] = os.path.basename(dst)
        ok += 1
        print(f"{w}: {duration(src):.2f}s → {span:.2f}s")
    with open(BLOCKS_TSV, "w", encoding="utf-8") as f:
        for w, fn in sorted(idx.items()):
            f.write(f"{fn}\t{w}\n")
    print(f"词块表：{len(idx)} 条 → {BLOCKS_TSV}")
    return 0 if ok or idx else 1


def say(sentence, out="/tmp/voice-blocks-say.wav"):
    idx = load_index()
    if not idx:
        print("还没有词块表，先 --trim", file=sys.stderr)
        return 1
    # 贪心：最长匹配优先
    words = sorted(idx, key=len, reverse=True)
    seq, i = [], 0
    while i < len(sentence):
        hit = next((w for w in words if sentence.startswith(w, i)), None)
        if not hit:
            print(f"切不下去：从第 {i} 个字开始（{sentence[i:i+6]}…）", file=sys.stderr)
            return 1
        seq.append(hit)
        i += len(hit)
    files = [os.path.join(BLOCKS, idx[w]) for w in seq]
    # 2026-11-11 17:4x：他说「能听出来，但怪怪的」——量了一下拼出来的句子，
    # 词与词之间有 0.18~0.21 秒的空档（正常连读不到 0.1 秒），听着像报菜名。
    # 原因是修剪时每块首尾各留了 60/120ms，两块接起来就是 180ms。
    # 拼接前把每块的静音再收一次：只在词间留 35ms。
    for w, f in zip(seq, files):
        if not os.path.exists(f):
            print(f"词块文件不在：{f}", file=sys.stderr)
            return 1
    # 每块先收紧首尾（只留 18ms 头、35ms 尾），再拼
    args = []
    for f in files:
        args += ["-i", f]
    n = len(files)
    parts = []
    for k in range(n):
        parts.append(f"[{k}:a]atrim=start=0.018,asetpts=PTS-STARTPTS[b{k}]")
    graph = ";".join(parts) + ";" + "".join(f"[b{k}]" for k in range(n)) + f"concat=n={n}:v=0:a=1"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error"] + args +
                   ["-filter_complex", graph, "-ar", "24000", "-ac", "1", out], check=True)
    print("拼成：" + " / ".join(seq))
    print(f"{out}  时长 {duration(out):.2f}s")
    return 0


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--trim", nargs="+", metavar="词")
    ap.add_argument("--say", metavar="句子")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.trim:
        return trim(a.trim)
    if a.say:
        return say(a.say)
    if a.list:
        idx = load_index()
        for w, fn in sorted(idx.items()):
            print(f"{w}\t{duration(os.path.join(BLOCKS, fn)):.2f}s")
        print(f"共 {len(idx)} 个")
        return 0
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
