#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""分区图体检：把 state/part-zones.tsv（窗口归一化格子）画到她本人的实拍上。

为什么单独一个工具：格子图现在是**她在跑的时候**现算的（main.cpp: partZoneTick，
按 drawable→part 光栅化），不是探针量的。要肉眼确认「每块是不是贴着身体」，
就得拿同一时刻、同一个窗口的实拍来叠。

用法：
    tools/zone-shot.py                 # 拍一张 → state/shot-zones.png
    tools/zone-shot.py --out /tmp/x.png
"""

import argparse
import json
import os
import re
import subprocess

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE = os.path.join(ROOT, "state")
COLOR = {
    "head":  (255, 96, 96),
    "chest": (255, 200, 40),
    "arm":   (90, 230, 90),
    "skirt": (70, 140, 255),
    "leg":   (200, 90, 255),
    "tail":  (40, 230, 230),
}


def log_tail(unit="miyu-body", n=600):
    r = subprocess.run(["journalctl", "--user", "-u", unit, "--no-pager",
                        "-o", "cat", "-n", str(n)], capture_output=True)
    return r.stdout.decode("utf-8", errors="replace")


def window_rect():
    txt = log_tail()
    win = size = None
    for line in txt.splitlines():
        m = re.search(r"\[box\] 全局：window=\((-?\d+),(-?\d+)\) output=\(\+?(-?\d+),\+?(-?\d+)\)", line)
        if m:
            wx, wy, ox, oy = map(int, m.groups())
            win = (wx + ox, wy + oy)
        m2 = re.search(r"物理 (\d+)x(\d+) -> 逻辑 (\d+)x(\d+)", line)
        if m2:
            size = (int(m2.group(3)), int(m2.group(4)))
    if not win or not size:
        raise SystemExit("日志里没有 window/逻辑尺寸 那两行（身体没起来？）")
    # 日志里的 window 没算顶上 bar 占的 exclusive zone（约 39px），
    # state/window-origin.json 是 tools/part-roots.py 用整屏 hide 比对校准出来的真实原点。
    o = os.path.join(STATE, "window-origin.json")
    if os.path.exists(o):
        try:
            w = json.load(open(o))
            return int(w["x"]), int(w["y"]), size[0], size[1]
        except Exception:
            pass
    return win[0], win[1], size[0], size[1]


def audit(shot, geom, grid, gw, gh):
    """分区 vs 她本人的剪影：对不上的格子有多少。这是「贴不贴身体」的硬数。"""
    import time
    subprocess.run(["curl", "-s", "-X", "POST", "-d", '{"hide":true}',
                    "http://127.0.0.1:8200/set"], capture_output=True)
    time.sleep(0.5)
    subprocess.run(["grim", "-g", geom, "/tmp/zoneshot-bg.png"], check=True)
    subprocess.run(["curl", "-s", "-X", "POST", "-d", '{"hide":false}',
                    "http://127.0.0.1:8200/set"], capture_output=True)
    time.sleep(0.5)
    subprocess.run(["grim", "-g", geom, "/tmp/zoneshot-her.png"], check=True)
    her = np.asarray(Image.open("/tmp/zoneshot-her.png").convert("RGB"), dtype=np.int16)
    bg = np.asarray(Image.open("/tmp/zoneshot-bg.png").convert("RGB"), dtype=np.int16)
    sil = (np.abs(her - bg).max(axis=2) > 14)
    sh, sw = sil.shape
    tot = off = 0
    per = {}
    for v in range(gh):
        y0, y1 = int(v * sh / gh), int((v + 1) * sh / gh)
        for u in range(gw):
            lab = grid[v][u]
            if lab == ".":
                continue
            x0, x1 = int(u * sw / gw), int((u + 1) * sw / gw)
            inside = int(sil[y0:y1, x0:x1].sum())
            area = max(1, (y1 - y0) * (x1 - x0))
            tot += 1
            if inside * 2 < area:
                off += 1
                per[lab] = per.get(lab, 0) + 1
    print("[check] 分区格子 %d 个，落在剪影外的 %d 个（%.1f%%）%s"
          % (tot, off, 100.0 * off / max(1, tot),
             ("，" + str(per)) if per else ""))
    # 反过来：剪影里有多少像素没被任何分区盖住（会点成「空白」）
    covered = np.zeros_like(sil)
    for v in range(gh):
        y0, y1 = int(v * sh / gh), int((v + 1) * sh / gh)
        for u in range(gw):
            if grid[v][u] == ".":
                continue
            x0, x1 = int(u * sw / gw), int((u + 1) * sw / gw)
            covered[y0:y1, x0:x1] = True
    miss = int((sil & ~covered).sum())
    print("[check] 剪影 %d px，其中没被分区盖住 %d px（%.1f%%，点上去会走整套反应）"
          % (int(sil.sum()), miss, 100.0 * miss / max(1, int(sil.sum()))))


def frame_overlay(args):
    """用 MIYU_DUMP_FRAME / MIYU_POKE_DIR 导出来的帧画分区（窗口坐标，一个像素都不差）。"""
    with open(args.frame, "rb") as f:
        d = f.read()
    j = d.index(b"\n")
    hdr = d[:j].split()
    w, h = int(hdr[1]), int(hdr[2])
    a = np.frombuffer(d[j + 1: j + 1 + w * h * 4], np.uint8).reshape(h, w, 4)
    rgb = a[:, :, :3].astype(np.float32)
    al = a[:, :, 3].astype(np.float32) / 255.0
    bg = np.array([18, 18, 24], np.float32)
    img = rgb * al[..., None] + bg * (1 - al[..., None])
    grid = [["."] * 60 for _ in range(120)]
    gw, gh = 60, 120
    for line in open(os.path.join(STATE, "part-zones.tsv"), encoding="utf-8"):
        if line.startswith("#grid"):
            c = line.split()
            gw, gh = int(c[1]), int(c[2])
            grid = [["."] * gw for _ in range(gh)]
            continue
        if not line.startswith("Z\t"):
            continue
        c = line.rstrip("\n").split("\t")
        v0, v1, u0, u1 = int(c[1]), int(c[2]), int(c[3]), int(c[4])
        for v in range(v0, min(v1, gh)):
            for u in range(u0, min(u1, gw)):
                grid[v][u] = c[5]
    sh, sw = h, w
    out = img.copy()
    for v in range(gh):
        y0, y1 = int(v * sh / gh), int((v + 1) * sh / gh)
        for u in range(gw):
            lab = grid[v][u]
            if lab == ".":
                continue
            x0, x1 = int(u * sw / gw), int((u + 1) * sw / gw)
            col = np.array(COLOR.get(lab, (128, 128, 128)), np.float32)
            out[y0:y1, x0:x1] = out[y0:y1, x0:x1] * 0.45 + col * 0.55
    im = Image.fromarray(out.astype(np.uint8))
    d2 = ImageDraw.Draw(im)
    n = {k: sum(r.count(k) for r in grid) for k in list(COLOR) + ["."]}
    d2.rectangle([0, 0, 340, 16], fill=(0, 0, 0))
    d2.text((4, 3), "zones over her (window coords)  " +
            " ".join("%s%d" % (k[0], n[k]) for k in COLOR), fill=(255, 255, 255))
    y = 20
    for k in COLOR:
        d2.rectangle([4, y, 14, y + 9], fill=COLOR[k])
        d2.text((18, y), "%s %d" % (k, n[k]), fill=(255, 255, 255))
        y += 12
    im.save(args.out)
    print("→ %s（帧 %s，%dx%d）" % (args.out, args.frame, w, h))
    print("格子统计：%s" % n)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(STATE, "shot-zones.png"))
    ap.add_argument("--geom", default=None, help="x,y WxH（不给就按日志里的窗口）")
    ap.add_argument("--check", action="store_true", help="另外拍一张 hide 图，核对分区在不在剪影里")
    ap.add_argument("--frame", default=None,
                    help="用身体自己导的帧（.raw，窗口坐标）当底图 —— 比 grim 截屏准"
                         "（桌面在动、顶栏还有 39px 偏移）")
    args = ap.parse_args()

    if args.frame:
        return frame_overlay(args)
    wx, wy, ww, wh = window_rect()
    geom = args.geom or "%d,%d %dx%d" % (wx, wy, ww, wh)
    shot = "/tmp/zoneshot.png"
    subprocess.run(["grim", "-g", geom, shot], check=True)
    img = np.asarray(Image.open(shot).convert("RGB"), dtype=np.uint8).copy()
    sh, sw = img.shape[:2]

    grid = [["."] * 60 for _ in range(120)]
    gw, gh = 60, 120
    for line in open(os.path.join(STATE, "part-zones.tsv"), encoding="utf-8"):
        if line.startswith("#grid"):
            c = line.split()
            gw, gh = int(c[1]), int(c[2])
            grid = [["."] * gw for _ in range(gh)]
            continue
        if not line.startswith("Z\t"):
            continue
        c = line.rstrip("\n").split("\t")
        v0, v1, u0, u1 = int(c[1]), int(c[2]), int(c[3]), int(c[4])
        for v in range(v0, min(v1, gh)):
            for u in range(u0, min(u1, gw)):
                grid[v][u] = c[5]

    over = img.astype(np.float32)
    for v in range(gh):
        y0 = int(v * sh / gh)
        y1 = int((v + 1) * sh / gh)
        for u in range(gw):
            lab = grid[v][u]
            if lab == ".":
                continue
            x0 = int(u * sw / gw)
            x1 = int((u + 1) * sw / gw)
            col = np.array(COLOR.get(lab, (128, 128, 128)), np.float32)
            over[y0:y1, x0:x1] = over[y0:y1, x0:x1] * 0.42 + col * 0.58
    out = Image.fromarray(over.astype(np.uint8))
    d = ImageDraw.Draw(out)
    for u in range(0, gw + 1, 2):
        d.line([(u * sw / gw, 0), (u * sw / gw, sh)], fill=(255, 255, 255, 30))
    for v in range(0, gh + 1, 5):
        d.line([(0, v * sh / gh), (sw, v * sh / gh)], fill=(255, 255, 255, 30))
    n = {k: sum(row.count(k) for row in grid) for k in list(COLOR) + ["."]}
    d.rectangle([0, 0, 260, 15], fill=(0, 0, 0))
    d.text((4, 3), "grid %dx%d  %s" % (gw, gh, " ".join("%s%d" % (k[0], n[k]) for k in COLOR)),
           fill=(255, 255, 255))
    # 图例
    y = 18
    for k in COLOR:
        d.rectangle([3, y, 13, y + 9], fill=COLOR[k])
        d.text((17, y), "%s %d" % (k, n[k]), fill=(255, 255, 255))
        y += 12
    if args.check:
        audit(shot, geom, grid, gw, gh)

    out.save(args.out)
    print("→ %s（窗口 %d,%d %dx%d）" % (args.out, wx, wy, ww, wh))
    print("格子统计：%s" % {k: v for k, v in n.items()})


if __name__ == "__main__":
    main()
