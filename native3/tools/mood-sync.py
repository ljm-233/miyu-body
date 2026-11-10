#!/usr/bin/env python3
"""把她此刻的心情同步给身体。

2026-11-11 00:5x —— 他一句话点醒的：这不是一个「桌宠」，是**米鱼在这台电脑上的身体**。
身体的输入应该是**她自己的状态**，不是我随手加的功能开关。

她的心情本来就在 whale-memory 里（四根维度 + 总值 + 半衰 + 地板天花板），
但身体一直不知道 —— 摆在那儿，跟 tired=95 一点关系都没有。

这个脚本把那四个数搬到 `state/mood.json`，身体每 30 秒读一次，用来决定：
  · 姿态（累了就垮下来，不是永远挺着）
  · 动作幅度（累的时候被碰也弹不起来）
  · 表情基线（开心容容易脸红，累的时候只剩眨眼变慢）
  · 眨眼频率
  · 主动说话的意愿

只读，不改库。跑失败就什么都别写（身体会退回到「不知道心情」的默认行为）。
"""
import json
import os
import sqlite3
import sys
import time

DB = os.path.expanduser("~/.dsh/whale-memory/memory.db")
OUT = os.path.expanduser("~/.dsh/miyu/body/native3/state/mood.json")


def main():
    if not os.path.exists(DB):
        return 0
    try:
        con = sqlite3.connect("file:%s?mode=ro" % DB, uri=True, timeout=2.0)
        row = con.execute(
            "SELECT v FROM meta WHERE k LIKE 'mood.state.%' ORDER BY k DESC LIMIT 1"
        ).fetchone()
        con.close()
    except Exception:
        return 0
    if not row or not row[0]:
        return 0
    try:
        st = json.loads(row[0])
    except Exception:
        return 0

    mood = float(st.get("mood") or 50.0)
    dims = st.get("dims") or {}
    out = {
        "mood": round(mood, 1),
        "happy": round(float(dims.get("happy") or 0.0), 1),
        "tired": round(float(dims.get("tired") or 0.0), 1),
        "sad": round(float(dims.get("sad") or 0.0), 1),
        "care": round(float(dims.get("care") or 0.0), 1),
        "at": int(time.time()),
    }
    tmp = OUT + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False)
    os.replace(tmp, OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
