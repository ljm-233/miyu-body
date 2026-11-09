#!/usr/bin/env python3
"""把几个动作片段拼成一份动作计划（plan JSON），打印到 stdout。

    plan-build.py 抬头 点头 > /tmp/p.json
    plan-build.py 害羞 0.8 | ...        # 片段后面可跟时长倍率

片段是「相对值 + 时长」，拼接时自动累加时间；每个片段首尾都回到 0，
所以任意顺序组合都不会跳。
"""
import json, sys

# 每段: (时长秒, [(相对时间0..1, {参数: 值})])
SEG = {
    "抬头":   (1.0, [(0, {"AngleX": 0}), (0.35, {"AngleX": 10}), (0.7, {"AngleX": 9}), (1, {"AngleX": 0})]),
    "低头":   (1.0, [(0, {"AngleX": 0}), (0.4, {"AngleX": -8}), (1, {"AngleX": 0})]),
    "歪左":   (0.9, [(0, {"AngleZ": 0}), (0.4, {"AngleZ": 7}), (1, {"AngleZ": 0})]),
    "歪右":   (0.9, [(0, {"AngleZ": 0}), (0.4, {"AngleZ": -7}), (1, {"AngleZ": 0})]),
    "点头":   (1.2, [(0, {"AngleY": 0}), (0.25, {"AngleY": 7}), (0.5, {"AngleY": 0}), (0.75, {"AngleY": 6}), (1, {"AngleY": 0})]),
    "看左":   (0.8, [(0, {"ParamEyeBallX": 0}), (0.4, {"ParamEyeBallX": 0.8}), (1, {"ParamEyeBallX": 0})]),
    "看右":   (0.8, [(0, {"ParamEyeBallX": 0}), (0.4, {"ParamEyeBallX": -0.8}), (1, {"ParamEyeBallX": 0})]),
    "躲眼":   (0.9, [(0, {"ParamEyeBallX": 0, "ParamEyeBallY": 0}), (0.4, {"ParamEyeBallX": -0.7, "ParamEyeBallY": -0.5}), (1, {"ParamEyeBallX": 0, "ParamEyeBallY": 0})]),
    "靠近":   (1.0, [(0, {"PositionY": 0}), (0.4, {"PositionY": 6}), (1, {"PositionY": 0})]),
    "退开":   (1.0, [(0, {"PositionY": 0}), (0.4, {"PositionY": -6}), (1, {"PositionY": 0})]),
    "肩膀":   (1.0, [(0, {"PositionX": 0}), (0.4, {"PositionX": 4}), (1, {"PositionX": 0})]),
    "害羞":   (1.4, [(0, {"AngleX": 0, "AngleZ": 0, "ParamEyeBallX": 0}),
                     (0.4, {"AngleX": -6, "AngleZ": 6, "ParamEyeBallX": -0.7}),
                     (1, {"AngleX": 0, "AngleZ": 0, "ParamEyeBallX": 0})]),
}

def main(argv):
    if not argv:
        print("片段: " + " ".join(SEG), file=sys.stderr); return 2
    frames, t0, used = [], 0.0, []
    i = 0
    while i < len(argv):
        name = argv[i]; scale = 1.0
        if i + 1 < len(argv):
            try: scale = float(argv[i + 1]); i += 1
            except ValueError: pass
        if name not in SEG:
            print("没有这个片段: %s（有: %s）" % (name, " ".join(SEG)), file=sys.stderr); return 2
        dur, keys = SEG[name]
        dur *= scale
        start = frames[-1]["time"] if frames else 0.0
        for rel, vals in keys:
            t = start + rel * dur
            if frames and abs(t - frames[-1]["time"]) < 1e-6:
                frames[-1]["values"].update(vals)          # 与上一段末帧同刻，合并
            else:
                frames.append({"time": round(t, 3), "values": dict(vals)})
        used.append(name); i += 1
    plan = {"version": 1, "title": " + ".join(used), "view": "full",
            "actions": [{"kind": "trajectory", "duration": frames[-1]["time"], "frames": frames}]}
    print(json.dumps(plan, ensure_ascii=False))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
