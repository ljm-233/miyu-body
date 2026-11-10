#!/usr/bin/env python3
"""把她「外面的世界」搬进 state/world.json。

2026-11-11 01:2x —— 他问「还缺什么」，我说的第二条和第三条：
    二、她看不见你在干什么（不知道屏幕上开着什么、你几点还没睡）
    三、记忆没接进身体（日记、今天做过什么，身体拿不到）
然后他说「那二三同时做」。

这个脚本只读、不改任何东西，把三样东西写成一个小 json：

    focus    你现在在用哪个窗口（应用名 + 标题）
    clock    本地时间、时段（深夜/早上/下午/晚上）、你醒着多久了
    today    今天的日记最后一行 / 账本最后一条（她「记得」的今天）

身体每 40 秒读一次（比心情慢一倍：窗口标题变来变去，不需要那么勤）。

**为什么不一直接进 LLM**：这具身体是纯 C++ 进程，没有会话也调不了模型。
它只能读现成的文件。要看懂屏幕上在干什么、要在日记里写东西，那是别处的活；
这里只负责「她已经知道这件事了」。

拿不到任何一项都让那一项空着，不编。
"""
import datetime
import json
import os
import subprocess
import sys

OUT = os.path.expanduser("~/.dsh/miyu/body/native3/state/world.json")
DIARY = os.path.expanduser("~/.dsh/miyu/diary/whale-diary.md")
LEDGER = os.path.expanduser("~/.dsh/miyu/db/LEDGER.md")


def sh(cmd):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=3)
        return r.stdout.strip() if r.returncode == 0 else ""
    except Exception:
        return ""


def focus():
    """当前焦点窗口。用 --json 那版：文本版是 "App ID: xxx"，json 直接给字段，省得猜。"""
    raw = sh("niri msg --json focused-window 2>/dev/null")
    if not raw:
        return {"app": "", "title": ""}
    try:
        d = json.loads(raw)
        return {"app": d.get("app_id", "") or "", "title": d.get("title", "") or ""}
    except Exception:
        return {"app": "", "title": ""}


def clock():
    now = datetime.datetime.now()
    h = now.hour
    if h < 5:
        period = "深夜"
    elif h < 9:
        period = "早上"
    elif h < 12:
        period = "上午"
    elif h < 14:
        period = "中午"
    elif h < 18:
        period = "下午"
    elif h < 23:
        period = "晚上"
    else:
        period = "深夜"
    return {"hour": h, "minute": now.minute, "period": period,
            "iso": now.strftime("%Y-%m-%dT%H:%M")}


def tail_line(path, maxlen=80):
    """取文件最后一行的前 maxlen 个字（日记/账本都是「一行一件」的格式）。"""
    try:
        with open(path, encoding="utf-8") as f:
            lines = [l.rstrip() for l in f if l.strip()]
    except Exception:
        return ""
    if not lines:
        return ""
    # 跳过纯标记行（比如 '---' 或 '## 标题'）
    for l in reversed(lines):
        if l.startswith("---") or l.startswith("###"):
            continue
        return l[:maxlen]
    return ""


def main():
    today = datetime.date.today()
    data = {
        "focus": focus(),
        "clock": clock(),
        "today": {
            "diary_date": "",       # 日记里最后一段是哪天
            "diary": "",
            "ledger": tail_line(LEDGER),
        },
        "at": int(datetime.datetime.now().timestamp()),
    }
    # 日记：找最后一个 "## YYYY-MM-DD" 标题，再取它下面的第一句
    try:
        with open(DIARY, encoding="utf-8") as f:
            text = f.read()
        lines = text.splitlines()
        for i in range(len(lines) - 1, -1, -1):
            l = lines[i].strip()
            if l.startswith("## ") and len(l) >= 13:
                data["today"]["diary_date"] = l[3:13]
                for j in range(i + 1, min(i + 12, len(lines))):
                    s = lines[j].strip()
                    if s and not s.startswith("**") and not s.startswith("#"):
                        data["today"]["diary"] = s[:80]
                        break
                break
    except Exception:
        pass

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    tmp = OUT + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    os.replace(tmp, OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
