#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给每个部位挑参数：不选「像素最多」，选「点下去这一块真的挪得最多」。

为什么要多这一道：param-probe.py 量的是「设成 min/max 时，这个部位有多少像素变了」。
可是验收看的是**质心位移** —— 一个只在原地变形的参数像素不少、质心却几乎不动
（比如手臂参数只是让袖子皱一下）。反过来，一个把整块平移的参数像素可能不多，
质心位移却很大。两件事不是一回事，所以拿真机再量一遍。

做法（和正式运行时同序）：
  · 起一个探针实例，**physics 开着**，参数在 physics 之后写（和生产完全一样）；
  · 先拍 hide 帧得到剪影，再逐参数设到「弹到 60%」，拍一张，算各块剪影质心；
  · 记 该块位移 / 其它块最大位移 的比值，比值大的留；
  · 结果写回 state/param-parts.tsv 的 P 行（并且打一份 state/part-rank.json 留证）。

    tools/param-rank.py              # 全部部位
    tools/param-rank.py --parts tail armL
    tools/param-rank.py --per-part 10   # 每个部位试几个候选（默认 8）
"""

import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import importlib.util

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("pp", os.path.join(HERE, "param-probe.py"))
pp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pp)          # 复用 PARTS / norm_per / gate_for / Body / Prober 等

STATE = pp.STATE
THR = 12


def rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def candidates(raw, per_part=8):
    """每个部位的候选参数（像素口径先粗筛，后面再用质心位移精选）。"""
    out = {}
    for part in pp.PART_NAMES:
        rows = []
        for nm, r in raw.items():
            if r["total"] < 200:
                continue
            if nm.startswith(("key", "Clothes", "Paramshuiyin")):
                continue
            if part not in pp.gate_for(nm):
                continue
            per = pp.norm_per(r["per"])
            if per.get(part, 0) < 400:
                continue
            vals = sorted(per.values(), reverse=True)
            tot = max(1, sum(per.values()))
            share = per[part] / tot
            ratio = per[part] / max(1, vals[1])
            if share < 0.28 or ratio < 1.1:
                continue
            rows.append((per[part], nm))
        rows.sort(reverse=True)
        out[part] = [nm for _px, nm in rows[:per_part]]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--parts", nargs="*", default=None)
    ap.add_argument("--per-part", type=int, default=8)
    ap.add_argument("--settle", type=int, default=12)
    ap.add_argument("--emit", action="store_true", help="顺手把结果写回 param-parts.tsv")
    ap.add_argument("--no-service", action="store_true")
    args = ap.parse_args()

    raw = json.load(open(os.path.join(STATE, "probe-raw.json"), encoding="utf-8"))["result"]
    rng = pp.read_ranges()
    cand = candidates(raw, args.per_part)
    parts = args.parts or [p for p in pp.PART_NAMES if cand.get(p)]

    cfg = os.path.join(STATE, "settings.json")
    cfg_bak = cfg + ".rank-bak"
    if os.path.exists(cfg):
        import shutil
        shutil.copyfile(cfg, cfg_bak)

    body = pp.Body(args.settle, no_phys=False)     # ← 和生产一致：physics 开着
    prober = pp.Prober(body, args.settle)
    keep_path = "/tmp/miyu-rank"
    os.makedirs(keep_path, exist_ok=True)
    best = {}
    try:
        if not args.no_service:
            body.stop_service()
        body.start()
        geom = body.geom()   # 和 prober.shot 用的同一块（pad 6）
        w, h = body.box[2] + 16, body.box[3] + 16

        # 剪影
        prober.set(None, 0)
        a = prober.shot(keep_path + "/a.png")
        subprocess.run(["curl", "-s", "-X", "POST", "-d", '{"hide":true}',
                        "http://127.0.0.1:8200/set"], capture_output=True)
        time.sleep(0.4)
        bgp = keep_path + "/bg.png"
        pp.grim(bgp, geom)
        subprocess.run(["curl", "-s", "-X", "POST", "-d", '{"hide":false}',
                        "http://127.0.0.1:8200/set"], capture_output=True)
        time.sleep(0.4)
        prober.set(None, 0)
        basep = prober.shot(keep_path + "/base.png")

        BG, A, BASE = rgb(bgp), rgb(a), rgb(basep)
        sil = pp.dilate((np.abs(A - BG).max(axis=2) > THR), 2)
        masks = pp.make_masks(*BASE.shape[:2])
        for k in masks:
            masks[k] &= sil
        yy, xx = np.mgrid[0:BASE.shape[0], 0:BASE.shape[1]]
        print("[rank] 剪影 %d px；各块 %s" % (int(sil.sum()),
              {k: int(v.sum()) for k, v in masks.items() if k != "other"}))

        def measure(nm, amp):
            """设到「peak = 默认 + amp×摆幅」，和**同一时刻的安静帧**比质心位移。"""
            lo, hi, dflt = rng[nm]
            r = raw[nm]
            ext = lo if r["dir"] == "min" else hi
            swing = ext - dflt
            if abs(swing) < 1e-3:
                return None
            prober.set(None, 0)                     # ← 每次都先回到安静态，
            rest = prober.shot(keep_path + "/rest.png")   #   物理摆动的影响就抵消掉了
            peak = dflt + amp * swing
            if not prober.set(nm, "%.4f" % peak):
                return None
            shot = prober.shot(keep_path + "/p.png")
            c0 = pp.part_centroids(rgb(rest), BG, masks)
            c1 = pp.part_centroids(rgb(shot), BG, masks)
            d = {}
            for k in masks:
                d[k] = 0.0 if (c0.get(k) is None or c1.get(k) is None) else \
                    (abs(c1[k][0] - c0[k][0]) ** 2 + abs(c1[k][1] - c0[k][1]) ** 2) ** 0.5
            return d, peak, swing

        for part in parts:
            rows = []
            for nm in cand[part]:
                if nm not in rng:
                    continue
                got = None
                for amp in (0.6, 1.0):               # 60% 不够就试 100%（任务书只要求
                    # 「默认 60%」，但手臂这种被头发挡住的部位 60% 看不出来，允许加档）
                    m = measure(nm, amp)
                    if not m:
                        break
                    d, peak, swing = m
                    hit = d.get(part, 0.0)
                    others = max([v for k, v in d.items() if k != part] or [0.0])
                    ratio = hit / others if others > 1e-6 else 999.0
                    rec = {"param": nm, "amp": amp, "hit": round(hit, 2),
                           "others": round(others, 2), "ratio": round(ratio, 2),
                           "peak": round(peak, 4), "swing": round(swing, 4)}
                    if got is None or ratio > got["ratio"]:
                        got = rec
                    if ratio >= 3.0 and hit >= 1.0:
                        break
                if not got:
                    continue
                rows.append(got)
                print("  %-6s %-22s amp %.1f  该块 %5.2f px  其它 %5.2f px  比值 %s"
                      % (part, got["param"], got["amp"], got["hit"], got["others"],
                         got["ratio"]))
            rows.sort(key=lambda r: (-r["ratio"], -r["hit"]))
            best[part] = rows
    finally:
        body.stop()
        if os.path.exists(cfg_bak):
            import shutil
            shutil.copyfile(cfg_bak, cfg)
            os.remove(cfg_bak)
        if not args.no_service:
            body.start_service()

    with open(os.path.join(STATE, "part-rank.json"), "w", encoding="utf-8") as f:
        json.dump(best, f, ensure_ascii=False, indent=1)
        f.write("\n")

    print("\n[rank] 每个部位最好的三个：")
    for part, rows in best.items():
        s = ", ".join("%s(×%.1f, %.1fpx)" % (r["param"], r["ratio"], r["hit"])
                      for r in rows[:3])
        print("  %-6s %s" % (part, s))

    if args.emit:
        emit(best)


def emit(best):
    """把精选结果写回 param-parts.tsv（保留 R 行，重写 P 行）。"""
    p = os.path.join(STATE, "param-parts.tsv")
    lines = []
    with open(p, encoding="utf-8") as f:
        for line in f:
            if line.startswith(("R\t", "#")):
                lines.append(line.rstrip("\n"))
    rng = pp.read_ranges()
    for part in pp.PART_NAMES:
        rows = [r for r in best.get(part, []) if r["hit"] >= 1.0]
        for r in rows[:4]:
            rg = rng.get(r["param"])
            if not rg:
                continue
            lo, hi, dflt = rg
            lines.append("P\t%s\t%s\t%d\t%.4f\t%.4f"
                         % (r["param"], part, int(r["hit"] * 100), r["peak"],
                            dflt - 0.2 * r["swing"]))
    header = ("# 部位级反应的数据表（tools/param-probe.py + tools/param-rank.py 生成，别手改）\n"
              "# R<TAB>部位<TAB>u0<TAB>v0<TAB>u1<TAB>v1   —— 归一化矩形，按行序优先判定\n"
              "# P<TAB>参数<TAB>部位<TAB>分数<TAB>peak<TAB>dip —— 分 = 质心位移×100（rank 量出来的）\n")
    with open(p, "w", encoding="utf-8") as f:
        f.write(header + "\n".join(lines) + "\n")
    print("[rank] 已写回 %s（P 行 %d 条）" % (p, sum(1 for l in lines if l.startswith("P"))))


if __name__ == "__main__":
    main()
