#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""点哪动哪的验收：点一个部位，量「这块动了多少」和「别的块动了多少」。

判据（任务书的硬指标）：被点部位的质心位移 > 其它部位 × 3。

做法：
  1. hide=true 拍一张，得到墙纸；hide=false 时「她和墙纸的差」就是她的剪影。
  2. 剪影按部位切块（和 param-probe.py 里同一套归一化矩形），每块算一个
     **前景质心**（不是变化像素的质心 —— 那等于用结论证明结论）。
  3. 静止时拍几帧 → 基准质心 + **噪声底**（待机呼吸自己会动多少）。
  4. 用 input-probe/clicker 点下去，紧接着连拍 4 帧，每帧算各块质心相对基准的位移。
  5. 报「被点部位最大位移 / 其它部位最大位移」。

为了只量「被戳那一下」，测量期间把待机/视线/眨眼临时关掉（idle=0 look=0 blink=0），
量完恢复原值 —— 否则呼吸和摇摆会把 3 倍这个判据吃掉（NOTES 16.6 的老坑）。

用法：
    tools/part-tap-check.py                 # 四个部位都测
    tools/part-tap-check.py head skirt      # 只测这几个
    tools/part-tap-check.py --empty         # 对照组：点空白处，应该什么也不动
"""

import argparse
import json
import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "pp", os.path.join(ROOT, "tools", "param-probe.py"))
pp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pp)
STATE = os.path.join(ROOT, "state")
CLICKER = os.path.join(os.path.dirname(ROOT), "input-probe", "clicker")
WEB = "http://127.0.0.1:8200"
THR = 12

# 和 param-probe.py / src/partpoke.h 同一套（TSV 里 R 行是权威，这里只用来选测试点）
PARTS = {
    "tail":   [(0.56, 0.60, 1.00, 0.90), (0.04, 0.52, 0.23, 0.99), (0.22, 0.88, 0.72, 1.00)],
    "armL":   [(0.20, 0.24, 0.35, 0.52)],
    "armR":   [(0.61, 0.24, 0.78, 0.52)],
    "legL":   [(0.26, 0.62, 0.45, 0.96)],
    "legR":   [(0.45, 0.62, 0.63, 0.96)],
    "head":   [(0.18, 0.00, 0.80, 0.25)],
    "chest":  [(0.30, 0.25, 0.62, 0.41)],
    "waist":  [(0.28, 0.41, 0.64, 0.47)],
    "skirt":  [(0.08, 0.45, 0.84, 0.64)],
}
ORDER = []          # 运行时按 TSV 的行序填（探针生成什么顺序就按什么顺序判）

# 每个部位手挑一个「明显在那块里、不压边界」的测试点（归一化 u,v）
POINTS = {
    "head":  (0.50, 0.12),
    "armL":  (0.27, 0.38),
    "skirt": (0.50, 0.55),
    "tail":  (0.78, 0.72),
    "chest": (0.46, 0.33),
    "leg":   (0.35, 0.78),
    "armR":  (0.69, 0.38),
    "tail2": (0.80, 0.66),
}


def post(d):
    subprocess.run(["curl", "-s", "-X", "POST", "-d", json.dumps(d), WEB + "/set"],
                   capture_output=True)


def grim(path, geom):
    subprocess.run(["grim", "-g", geom, path], check=True)


def rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def boxes_from_tsv():
    """TSV 里的 R 行才是权威 —— 和 C++ 判定用的是同一份。"""
    out = {}
    p = os.path.join(STATE, "param-parts.tsv")
    with open(p, encoding="utf-8") as f:
        for line in f:
            if not line.startswith("R\t"):
                continue
            c = line.rstrip("\n").split("\t")
            out.setdefault(c[1], []).append(tuple(float(x) for x in c[2:6]))
    return {k: v for k, v in out.items() if v}


def person_box():
    # 日志里可能混着非 UTF-8 字节（早期调试输出），别让解码炸掉整个验收
    r = subprocess.run(["journalctl", "--user", "-u", "miyu-body", "--no-pager", "-o", "cat"],
                       capture_output=True)
    out = r.stdout.decode("utf-8", errors="replace")
    last = None
    for line in out.splitlines():
        if "[box] 全局" in line:
            last = line
    if not last:
        raise SystemExit("日志里没有 [box] 全局（身体没重起过？）")
    import re
    m = re.search(r"她=\[(-?\d+),(-?\d+) \.\. (-?\d+),(-?\d+)\]", last)
    x0, y0, x1, y1 = map(int, m.groups())
    return x0, y0, x1 - x0, y1 - y0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("parts", nargs="*", default=["head", "armL", "skirt", "tail"])
    ap.add_argument("--empty", action="store_true", help="对照组：点窗口内空白处")
    ap.add_argument("--frames", type=int, default=4)
    ap.add_argument("--box", default=None,
                    help="她本人的全局矩形 x,y,w,h（不给就从 journalctl 里捞）")
    args = ap.parse_args()

    if args.box:
        bx, by, bw, bh = [int(v) for v in args.box.split(",")]
    else:
        bx, by, bw, bh = person_box()
    pad = 8
    geom = "%d,%d %dx%d" % (bx - pad, by - pad, bw + 2 * pad, bh + 2 * pad)
    tmp = "/tmp/tapcheck"
    os.makedirs(tmp, exist_ok=True)
    print("[check] 她 = %d,%d %dx%d" % (bx, by, bw, bh))

    # ── 1. 剪影（她 vs hide）─────────────────────────────────────────
    grim(tmp + "/a.png", geom)
    post({"hide": True})
    time.sleep(0.45)
    grim(tmp + "/bg.png", geom)
    post({"hide": False})
    time.sleep(0.45)
    grim(tmp + "/b.png", geom)
    BG = rgb(tmp + "/bg.png")
    A = rgb(tmp + "/a.png")
    B = rgb(tmp + "/b.png")
    sil = (np.abs(A - BG).max(axis=2) > THR) | (np.abs(B - BG).max(axis=2) > THR)
    h, w = sil.shape
    yy, xx = np.mgrid[0:h, 0:w]

    boxes = boxes_from_tsv()
    ORDER[:] = list(boxes.keys())
    un = (xx + 0.5 + pad) / w if False else (xx + 0.5) / w
    vn = (yy + 0.5) / h
    masks = {}
    left = sil.copy()
    for name in ORDER:
        if name not in boxes:
            continue
        m = np.zeros((h, w), bool)
        for (u0, v0, u1, v1) in boxes[name]:
            m |= (un >= u0) & (un <= u1) & (vn >= v0) & (vn <= v1)
        masks[name] = m & left
        left &= ~m
    print("[check] 剪影 %d px；各块像素：%s" %
          (int(sil.sum()), {k: int(v.sum()) for k, v in masks.items()}))

    # ── 2. 关掉会自己动的东西，量基准 ────────────────────────────────
    # 原值从**落盘的** settings.json 读：中途崩掉的那一轮已经把 /state 改成 0 了，
    # 拿 /state 当"原值"会把 0 当成用户设置，越跑越歪（踩过一次）。
    try:
        with open(os.path.join(STATE, "settings.json"), encoding="utf-8") as f:
            st = json.load(f)
    except Exception:
        st = json.loads(subprocess.run(["curl", "-s", WEB + "/state"], capture_output=True,
                                       text=True).stdout)
    keep = {k: st[k] for k in ("idle", "look", "blink") if k in st}
    post({"idle": 0, "look": 0, "blink": 0})
    time.sleep(0.4)
    grim(tmp + "/base0.png", geom)
    base = rgb(tmp + "/base0.png")

    # ── 两块量法：位移（相位相关）+ 变化量（像素数）────────────────────
    # 为什么不用「剪影质心」当主指标：她的身体在格子里是**实心**的，前景掩码剪出来的
    # 就是格子本身 —— 头动 10 px，格子里的质心几乎不动（实测只走 0.6 px，把「动了」
    # 误判成「没动」）。所以主指标改成相位相关求「这块图整体挪了几个像素」，
    # 质心只留作辅助。
    def prep(img):
        """灰度 + 每块窗口（乘汉宁窗、去均值：不去掉直流分量，相关峰永远钉在 (0,0)）。"""
        return img.astype(np.float32).mean(axis=2)

    windows = {}
    for name, m in masks.items():
        if m.sum() < 300:
            continue
        ys, xs = np.nonzero(m)
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        w2 = (m[y0:y1, x0:x1].astype(np.float32)
              * np.outer(np.hanning(y1 - y0), np.hanning(x1 - x0)).astype(np.float32))
        windows[name] = (y0, y1, x0, x1, w2)

    def shifts(img, ref):
        g1, g0 = prep(img), prep(ref)
        out = {}
        for name, (y0, y1, x0, x1, w2) in windows.items():
            a = g0[y0:y1, x0:x1] * w2
            b = g1[y0:y1, x0:x1] * w2
            wsum = w2.sum() or 1.0
            a = a - (a.sum() / wsum) * w2
            b = b - (b.sum() / wsum) * w2
            R = np.fft.rfft2(a) * np.conj(np.fft.rfft2(b))
            R /= (np.abs(R) + 1e-9)
            r = np.fft.irfft2(R, s=a.shape)
            iy, ix = np.unravel_index(int(np.argmax(r)), r.shape)
            dy = iy - (a.shape[0] if iy > a.shape[0] // 2 else 0)
            dx = ix - (a.shape[1] if ix > a.shape[1] // 2 else 0)
            out[name] = (float(dx), float(dy), float((dx * dx + dy * dy) ** 0.5))
        return out

    # 质心：**按矩形算**（不乘剪影掩码）。乘掩码的话格子里是实心的，头动十像素
    # 质心也只动零点几 —— 那是"掩码没动"，不是"她没动"。按矩形算，前景形状一变
    # 质心就跟着走。
    rects_z = {}
    un = (np.mgrid[0:masks["tail"].shape[0], 0:masks["tail"].shape[1]][1] + 0.5) \
        / masks["tail"].shape[1]
    vn = (np.mgrid[0:masks["tail"].shape[0], 0:masks["tail"].shape[1]][0] + 0.5) \
        / masks["tail"].shape[0]
    for name, rs in PARTS.items():
        m = np.zeros(un.shape, bool)
        for (a, b, c, d) in rs:
            m |= (un >= a) & (un <= c) & (vn >= b) & (vn <= d)
        rects_z[name] = m
    yy, xx = np.mgrid[0:un.shape[0], 0:un.shape[1]]

    def centroid(img, ref):
        fg = np.abs(img - ref).max(axis=2) > THR
        out = {}
        for name, m in rects_z.items():
            mm = fg & m
            n = int(mm.sum())
            out[name] = None if n < 40 else (float((xx * mm).sum() / n),
                                             float((yy * mm).sum() / n))
        return out

    def changed(img, ref):
        d = np.abs(img - ref).max(axis=2) > THR
        return {k: int((d & m).sum()) for k, m in masks.items()}

    # 噪声底
    grim(tmp + "/b1.png", geom)
    grim(tmp + "/b2.png", geom)
    B1, B2 = rgb(tmp + "/b1.png"), rgb(tmp + "/b2.png")
    noise = max(v[2] for v in shifts(B2, B1).values())
    cn = changed(B2, B1)
    print("[check] 静止两帧：位移最大 %.2f px，变化像素最大 %d px（噪声底）"
          % (noise, max(cn.values())))

    results = {}
    targets = [] if args.empty else args.parts
    if args.empty:
        pts = [("empty", (0.045, 0.35))]      # 窗口内、她不占的地方（左边空地）
    else:
        pts = [(p, POINTS[p]) for p in targets]

    for name, (u, v) in pts:
        gx = int(bx + u * bw)
        gy = int(by + v * bh)
        grim(tmp + "/warm.png", geom)          # 预热，别让第一张慢
        time.sleep(1.05)      # 等冷却（TAP_COOL_S = 0.8s），不然下一枪会被吞掉
        # 每一枪都重新拍基准，并等物理余振停（连拍两张互相对得上才算安静）
        local_base = None
        for _ in range(6):
            grim(tmp + "/b1.png", geom)
            grim(tmp + "/b2.png", geom)
            q1, q2 = rgb(tmp + "/b1.png"), rgb(tmp + "/b2.png")
            if max(v[2] for v in shifts(q2, q1).values()) < 1.0:
                local_base = q2
                break
            time.sleep(0.6)
        if local_base is None:
            local_base = rgb(tmp + "/b2.png")
        r = subprocess.run([CLICKER, str(gx), str(gy)], capture_output=True)
        if r.returncode != 0:
            print("  !! clicker 失败：%s" % r.stderr.decode(errors="replace"))
        frames = []
        for i in range(args.frames):
            pth = tmp + "/f%d.png" % i
            grim(pth, geom)
            frames.append(rgb(pth))
        peak = {k: 0.0 for k in masks}
        chg = {k: 0 for k in masks}
        cbase = centroid(local_base, BG)
        cdisp = {k: 0.0 for k in masks}
        for img in frames:
            cimg = centroid(img, BG)
            for k in cdisp:
                if cbase.get(k) and cimg.get(k):
                    d2 = ((cbase[k][0] - cimg[k][0]) ** 2
                          + (cbase[k][1] - cimg[k][1]) ** 2) ** 0.5
                    cdisp[k] = max(cdisp[k], d2)
            for k, v in shifts(img, local_base).items():
                peak[k] = max(peak[k], v[2])
            for k, v in changed(img, local_base).items():
                chg[k] = max(chg[k], v)
        results[name] = peak
        hit = peak.get(name, 0.0) if name in peak else 0.0
        others = {k: v for k, v in peak.items() if k != name}
        omax = max(others.values() or [0.0])
        omax_name = max(others, key=others.get) if others else "-"
        chi = chg.get(name, 0)
        cmax = max([v for k, v in chg.items() if k != name] or [0])
        cd = cdisp.get(name, 0.0)
        cdo = max([v for k, v in cdisp.items() if k != name] or [0.0])
        print("  %-6s 点 (%d,%d)  质心位移 %.2f px（其它最大 %.2f，%s）| 变化像素 %d（其它最大 %d，%s）"
              % (name, gx, gy, cd, cdo,
                 ("%.1f×" % (cd / cdo)) if cdo > 1e-6 else "∞", chi, cmax,
                 ("%.1f×" % (chi / cmax)) if cmax > 0 else "∞"))

    # ── 3. 恢复设置 ────────────────────────────────────────────────
    post(keep)
    print("[check] 设置已恢复：%s" % keep)
    with open(os.path.join(STATE, "tap-check.json"), "w", encoding="utf-8") as f:
        json.dump({"noise": noise, "peak": results}, f, ensure_ascii=False, indent=1)
        f.write("\n")


if __name__ == "__main__":
    main()
