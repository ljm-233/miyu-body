#!/usr/bin/env python3
"""她自己开口：按此刻的心情和时段，从已有的台词里挑一句说出来。

2026-11-11 22:0x 他问「接下来做哪一个最好」，我选了这个——因为在这之前，
她动、她看人、她知道几点、知道你在用哪个窗口，**可说话的内容从来不是她的**：
每句都是外面推的指令，或者点她一下触发的固定台词。状态明明在那儿，一个字用不上。

**这一步不花钱**：台词都是缓存里现成的（81 句），只做「挑」不做「生成」。
真要说新的话，那得叫醒模型，是下一步。

规则很土，但能听出差别：

    半夜 / 很累      → night, tired
    心情低 / 很难过   → low
    很开心           → happy
    很在意           → miss
    其他             → greet, ack

冷却 20 分钟，同组内随机、避开最近说过的那几句。

用法：
    her-voice.py            # 判断一次，该说就说，不该说就退出
    her-voice.py --dry      # 只打印会挑哪句，不真的说
    her-voice.py --force    # 忽略冷却

定时器：`~/.config/systemd/user/miyu-voice.timer` 每 10 分钟跑一次。
"""
import argparse
import json
import os
import random
import socket
import subprocess
import sys
import time

HOME = os.path.expanduser("~")
VOICE = os.path.join(HOME, ".dsh/miyu/body/voice")
STATE = os.path.join(HOME, ".dsh/miyu/body/native3/state")
LINES = os.path.join(VOICE, "lines.tsv")
MOOD = os.path.join(STATE, "mood.json")
WORLD = os.path.join(STATE, "world.json")
SOCK = "/run/user/1000/miyu-body.sock"
# 上次说话的时间 + 最近说过的三句（就在 voice 目录里，跟台词放一起）
MEMO = os.path.join(VOICE, ".her-voice-state.json")

COOLDOWN = 20 * 60          # 两次主动开口至少隔 20 分钟
RECENT_KEEP = 4


def load_lines():
    groups = {}
    with open(LINES, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.rstrip("\n").split("\t")
            if len(parts) < 2:
                continue
            text, tag = parts[0], parts[1]
            groups.setdefault(tag, []).append(text)
    return groups


def load_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def pick_tags(mood, world):
    hour = int(world.get("clock", {}).get("hour", 12))
    tired = float(mood.get("tired", 0))
    sad = float(mood.get("sad", 0))
    happy = float(mood.get("happy", 0))
    care = float(mood.get("care", 0))
    mv = float(mood.get("mood", 50))

    if hour < 5 or hour >= 23 or tired >= 70:
        return ["night", "tired"]
    if sad >= 60 or mv < 35:
        return ["low"]
    if happy >= 70:
        return ["happy"]
    if care >= 70:
        return ["miss"]
    # 2026-11-11 22:1x：第一版默认走 greet/ack，试跑出来是「你问吧」「在的」——
    # 那是**应答**，没人问她的时候说这个很怪。主动开口只用「自言自语」那一类：
    # night / tired / low / happy / miss。greet 要等你真的刚回来才用（还没做检测），
    # ack 永远不主动开口。
    return ["miss", "happy"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--force", action="store_true")
    a = ap.parse_args()

    memo = load_json(MEMO)
    now = time.time()
    last = float(memo.get("last", 0))
    if not a.force and now - last < COOLDOWN:
        print(f"冷却中（还差 {int((COOLDOWN - (now - last)) / 60)} 分钟）")
        return 0

    mood = load_json(MOOD)
    world = load_json(WORLD)
    if not mood:
        print("读不到 mood.json", file=sys.stderr)
        return 1

    groups = load_lines()
    tags = pick_tags(mood, world)
    pool = []
    for t in tags:
        pool += groups.get(t, [])
    if not pool:
        print(f"这几组没台词：{tags}", file=sys.stderr)
        return 1

    recent = memo.get("recent", [])
    fresh = [x for x in pool if x not in recent] or pool
    text = random.choice(fresh)

    hour = int(world.get("clock", {}).get("hour", 12))
    print(f"心情 {mood.get('mood')}｜累 {mood.get('tired')}｜{hour} 点｜组 {tags} → 「{text}」")
    if a.dry:
        return 0

    # 推给身体：走 my（它自己会带 mood 翻表情）
    r = subprocess.run(["my", "say", text], capture_output=True, text=True,
                       cwd=os.path.join(HOME, ".dsh/miyu/body/native3"))
    if r.returncode != 0:
        print(f"推送失败：{r.stderr.strip()}", file=sys.stderr)
        return 1

    memo["last"] = now
    memo["recent"] = ([text] + [x for x in recent if x != text])[:RECENT_KEEP]
    memo["last_text"] = text
    os.makedirs(os.path.dirname(MEMO), exist_ok=True)
    with open(MEMO, "w", encoding="utf-8") as f:
        json.dump(memo, f, ensure_ascii=False)
    print("说了")
    return 0


if __name__ == "__main__":
    sys.exit(main())
