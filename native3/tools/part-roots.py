#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""选「点哪儿拨哪几个根」：把 physics-map 里的根参数逐个拨一次，量这块动多少、别的块动多少。

和第二十一节那版探针的区别（这次是重做）：
  · 以前量的是**所有 509 个参数**，然后挑「这块变化最大」的那个 —— 挑出来的多半是
    `ChestX1` / `SkirtY3` 这类**物理输出**。往输出里写数 = 顶掉物理算出来的结果，
    形状发尖、发假（他说的「胸部不行」就是这个）。
  · 现在只拨 `physics-map.json` 里的 **31 个根**（只被输入、从不被输出的参数），
    也就是模型的物理自己认的「外面能拨的旋钮」。拨它们，胸腔/裙摆/尾巴是**物理带起来的**。

部位格子来自 `state/part-zones.tsv`（身体自己在跑的时候按 drawable→part 光栅化出来的，
见 src/main.cpp: partZoneTick），窗口归一化，和点击判定用的是同一份。

产出
    state/param-parts.tsv   部位 → 根参数 + 冲量幅度（C++ 读这个）
    state/part-roots.json   账本：每个根在每个部位上量到多少像素、比值多少

用法
    tools/part-roots.py                 # 全跑（31 个根 × 正负两边）
    tools/part-roots.py --roots AngleX PositionY
"""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
_spec = importlib.util.spec_from_file_location("pp", os.path.join(ROOT, "tools", "param-probe.py"))
pp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pp)

STATE = os.path.join(ROOT, "state")
MAP = os.path.join(STATE, "physics-map.json")
ZONES = os.path.join(STATE, "part-zones.tsv")
THR = 14
QUIET = 500
PARTS = ["head", "chest", "arm", "skirt", "leg", "tail"]
# 冲量幅度（绝对偏移量）。根的量程大多是 ±30（Position*/Angle*）、0..1（ParamBreath、
# Clothes1）。给「看得见但不变形」的量：身体轴 3°，呼吸 0.35，其它按量程 30%。
AMP_FIXED = {"ParamBreath": 0.35, "Clothes1": 0.12, "Clothes2": 0.12, "Param42": 0.25}
# 开关类的根不能当「戳一下」用：这三个是动作开关，一拨她整条手臂就摆成那个姿势
NEVER = {"Movement1", "Movement2", "Movement3", "Paramshuiyin7", "Clothes1", "Clothes2"}


def log(*a):
    print(*a, flush=True)


def rgb(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)


DUMP = "/tmp/partroots/frames"


def load_frame(path):
    """读身体自己导的帧：`MIYUFRAME w h\n` + top-down RGBA → (h,w,4) int16。
    文件是「边写边读」的：长度不够就当没写好，返回 None 让调用方重试。"""
    with open(path, "rb") as f:
        data = f.read()
    try:
        j = data.index(b"\n")
    except ValueError:
        return None
    hdr = data[:j].split()
    if len(hdr) < 3 or hdr[0] != b"MIYUFRAME":
        return None
    w, h = int(hdr[1]), int(hdr[2])
    if len(data) - (j + 1) < w * h * 4:
        return None
    buf = np.frombuffer(data[j + 1: j + 1 + w * h * 4], dtype=np.uint8)
    return buf.reshape(h, w, 4).astype(np.int16)


def zones():
    """读 part-zones.tsv（窗口归一化格子）→ {部位: (u0,v0,u1,v1) 列表}"""
    gw, gh = 60, 120
    lab = [["."] * gw for _ in range(gh)]
    for line in open(ZONES, encoding="utf-8"):
        if line.startswith("#grid"):
            c = line.split()
            gw, gh = int(c[1]), int(c[2])
            lab = [["."] * gw for _ in range(gh)]
            continue
        if not line.startswith("Z\t"):
            continue
        c = line.rstrip("\n").split("\t")
        v0, v1, u0, u1 = int(c[1]), int(c[2]), int(c[3]), int(c[4])
        for v in range(v0, min(v1, gh)):
            for u in range(u0, min(u1, gw)):
                lab[v][u] = c[5]
    return lab, gw, gh


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--roots", nargs="*", default=None)
    ap.add_argument("--settle", type=int, default=60)
    ap.add_argument("--no-service", action="store_true")
    args = ap.parse_args()

    pm = json.load(open(MAP, encoding="utf-8"))
    cdi = json.load(open(pp.CDI3, encoding="utf-8"))
    cn = {p["Id"]: p.get("Name", "") for p in cdi["Parameters"]}
    rng = pp.read_ranges()
    roots = args.roots or [r for r in pm["roots"] if r not in NEVER]

    cfg = os.path.join(STATE, "settings.json")
    bak = cfg + ".roots-bak"
    if os.path.exists(cfg):
        shutil.copyfile(cfg, bak)

    os.makedirs(DUMP, exist_ok=True)
    for f in os.listdir(DUMP):
        os.remove(os.path.join(DUMP, f))
    os.environ["MIYU_DUMP_FRAME"] = DUMP     # 让身体每代自己导一帧出来（带 alpha）

    body = pp.Body(args.settle, no_phys=False)
    prober = pp.Prober(body, args.settle)
    tmp = "/tmp/partroots"
    os.makedirs(tmp, exist_ok=True)

    try:
        if not args.no_service:
            body.stop_service()
        body.start()
        log("[roots] 她 = %s｜每代的帧由身体自己导到 %s" % (body.box, DUMP))

        def frame(gen):
            """这一代导出来的那一帧（窗口坐标，P6 PPM）"""
            for ext in ("ppm", "raw"):
                p2 = os.path.join(DUMP, "frame-%d.%s" % (gen, ext))
                for _ in range(60):
                    if os.path.exists(p2) and os.path.getsize(p2) > 1000:
                        img = load_frame(p2)
                        if img is not None:
                            return img
                    time.sleep(0.05)
            return None

        def shoot(name, val):
            if not prober.set(name, val):
                return None
            return frame(prober.gen)

        # 剪影直接从 alpha 拿（帧里带 alpha，不用再拍 hide）
        BASE = shoot(None, 0)
        if BASE is None:
            raise SystemExit("帧没导出来（MIYU_DUMP 没生效？）")
        sil = pp.dilate(BASE[:, :, 3] > 8, 1)
        sh, sw = sil.shape
        log("[roots] 剪影 %d px（%dx%d，比 grim 干净：没有桌面）" % (int(sil.sum()), sw, sh))

        lab, gw, gh = zones()
        masks = {}
        for p in PARTS:
            m = np.zeros((sh, sw), bool)
            for v in range(gh):
                y0, y1 = int(v * sh / gh), int((v + 1) * sh / gh)
                for u in range(gw):
                    if lab[v][u] != p:
                        continue
                    x0, x1 = int(u * sw / gw), int((u + 1) * sw / gw)
                    m[y0:y1, x0:x1] = True
            masks[p] = m & sil
        log("[roots] 各块像素：%s" % {p: int(masks[p].sum()) for p in PARTS})

        rows = {}
        t0 = time.time()
        for i, rt in enumerate(roots):
            r = rng.get(rt)
            if not r:
                continue
            lo, hi, dflt = r
            if abs(hi - lo) < 1e-4:
                continue
            amp = AMP_FIXED.get(rt)
            if amp is None:
                amp = 0.30 * (hi - lo) * 0.5
            hits = {}
            for sgn in (+1, -1):
                val = dflt + sgn * amp
                val = max(lo, min(hi, val))
                img = shoot(rt, "%.4f" % val)
                if img is None:
                    continue
                d = (np.abs(img[:, :, :3] - BASE[:, :, :3]).max(axis=2) > THR) & sil
                for p in PARTS:
                    n = int((d & masks[p]).sum())
                    if n > hits.get(p, (0, 0))[0]:
                        hits[p] = (n, round(val, 3))
            rows[rt] = {"name": cn.get(rt, ""), "amp": amp,
                        "per": {p: hits.get(p, (0, 0))[0] for p in PARTS},
                        "val": {p: hits.get(p, (0, 0))[1] for p in PARTS}}
            log("[roots] %2d/%d %-12s amp=%.2f  %s  (%.0fs)" %
                (i + 1, len(roots), rt, amp,
                 " ".join("%s:%d" % (p, rows[rt]["per"][p]) for p in PARTS), time.time() - t0))
            if (i + 1) % 8 == 0:
                BASE = shoot(None, 0)     # 基准跟着物理漂移

        # ── 选：own / 其它最大 ────────────────────────────────────────
        pick = {}
        for p in PARTS:
            ranked = []
            for rt, d in rows.items():
                own = d["per"][p]
                if own < 200:
                    continue
                others = max([v for k, v in d["per"].items() if k != p] or [0])
                ratio = own / others if others > 0 else 99.0
                ranked.append((ratio, own, rt))
            ranked.sort(reverse=True)
            pick[p] = ranked
            log("[roots] %-6s %s" % (p, ", ".join("%s %.1f×(%d/%d)"
                % (rt, rr, own, max([rows[rt]['per'][k] for k in PARTS if k != p] or [0]))
                for rr, own, rt in ranked[:4]) or "(无)"))

        write_tsv(os.path.join(STATE, "param-parts.tsv"), pick, rows, cn)
        json.dump({"masks": {p: int(masks[p].sum()) for p in PARTS},
                   "roots": rows,
                   "pick": {p: [(round(r, 2), own, rt) for r, own, rt in v] for p, v in pick.items()}},
                  open(os.path.join(STATE, "part-roots.json"), "w", encoding="utf-8"),
                  ensure_ascii=False, indent=1)
        log("[roots] 写出 state/param-parts.tsv / state/part-roots.json")
    finally:
        body.stop()
        if os.path.exists(bak):
            shutil.copyfile(bak, cfg)
            os.remove(bak)
        if not args.no_service:
            body.start_service()


def write_tsv(path, pick, rows, cn):
    lines = [
        "# 部位级反应的数据表（tools/part-roots.py 生成，第二十二节）",
        "# 格子图在 part-zones.tsv（身体按 drawable→part 光栅化出来的，判「点到哪个部位」）",
        "# P<TAB>部位<TAB>根参数<TAB>比值×10<TAB>冲量幅度<TAB>备注",
        "#   幅度 = 给**根参数**的绝对偏移量（度 / 比例单位），方向每点一次交替",
        "#   只写根（physics3.json 里 Input.Source 那些）；Chest*/Skirt*/Hair* 是物理输出，不写",
    ]
    for p, ranked in pick.items():
        if not ranked:
            lines.append("# %s：没有能单独带起这块的根，点了退回整套反应" % p)
            continue
        for ratio, own, rt in ranked[:2]:
            amp = rows[rt]["amp"]
            lines.append("P\t%s\t%s\t%d\t%.3f\t%s 比值%.1f× 自己%d px"
                         % (p, rt, int(ratio * 10), amp, cn.get(rt, ""), ratio, own))
    tmp = path + ".tmp"
    open(tmp, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    os.replace(tmp, path)


if __name__ == "__main__":
    main()
