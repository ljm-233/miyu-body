#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""验收：点每个部位一次，量它自己动多少像素、别的部位最多动多少。

判据（任务书硬指标）：自己 / 别的部位最大 > 2。

怎么量的（为什么不是 grim 截图）：
  身体带 MIYU_POKE=<部位> 起来 → 2 秒后**真的点一下那个部位**（走和鼠标点击
  同一条 gGen → partPokeStart 的路，弹簧-阻尼冲量），然后每隔 4 帧把自己这一帧
  导出来（`MIYU_POKE_DIR`，带 alpha、窗口坐标）。帧之间逐像素比，按
  state/part-zones.tsv 的格子统计到部位上。
   · 不截屏：桌面在动（我这边终端一直滚），截屏会把桌面变化算成「她动了」
   · 坐标系不换算：帧就是窗口，和格子图同一个坐标系

用法：
    tools/part-poke-check.py                 # 六个部位全量
    tools/part-poke-check.py chest tail      # 只测几个
"""

import argparse
import importlib.util
import json
import os
import shutil
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
THR = 14
PARTS = ["head", "chest", "arm", "skirt", "leg", "tail"]
DIR = "/tmp/pokecheck"


def log(*a):
    print(*a, flush=True)


def load_frame(path):
    with open(path, "rb") as f:
        d = f.read()
    try:
        j = d.index(b"\n")
    except ValueError:
        return None
    hdr = d[:j].split()
    if len(hdr) < 3 or hdr[0] != b"MIYUFRAME":
        return None
    w, h = int(hdr[1]), int(hdr[2])
    if len(d) - (j + 1) < w * h * 4:
        return None
    return np.frombuffer(d[j + 1: j + 1 + w * h * 4], np.uint8).reshape(h, w, 4)


def zone_masks(sw, sh):
    gw, gh = 60, 120
    grid = [["."] * gw for _ in range(gh)]
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
    masks = {}
    for p in PARTS:
        m = np.zeros((sh, sw), bool)
        for v in range(gh):
            y0, y1 = int(v * sh / gh), int((v + 1) * sh / gh)
            for u in range(gw):
                if grid[v][u] != p:
                    continue
                x0, x1 = int(u * sw / gw), int((u + 1) * sw / gw)
                m[y0:y1, x0:x1] = True
        masks[p] = m
    return masks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("parts", nargs="*", default=PARTS)
    ap.add_argument("--no-service", action="store_true")
    ap.add_argument("--keep", action="store_true", help="把帧留在 /tmp/pokecheck")
    args = ap.parse_args()

    cfg = os.path.join(STATE, "settings.json")
    bak = cfg + ".poke-bak"
    if os.path.exists(cfg):
        shutil.copyfile(cfg, bak)
    os.makedirs(DIR, exist_ok=True)

    results = {}
    chest_frames = []
    try:
        for part in args.parts:
            for f in os.listdir(DIR):
                os.remove(os.path.join(DIR, f))
            os.environ["MIYU_POKE"] = part
            os.environ["MIYU_POKE_DIR"] = DIR
            body = pp.Body(12, no_phys=False)
            try:
                if not args.no_service:
                    body.stop_service()
                body.start()
                t0 = time.time()
                while time.time() - t0 < 20:
                    n = len([f for f in os.listdir(DIR) if f.endswith(".raw")])
                    if n >= 40:
                        break
                    time.sleep(0.05)
                frames = []
                for i in range(40):
                    p2 = os.path.join(DIR, "poke-%03d.raw" % i)
                    for _ in range(20):
                        if os.path.exists(p2):
                            img = load_frame(p2)
                            if img is not None:
                                frames.append(img)
                                break
                        time.sleep(0.03)
                if len(frames) < 4:
                    log("[poke] %-6s 帧不够（%d），跳过" % (part, len(frames)))
                    results[part] = None
                    continue
                base = frames[0][:, :, :3].astype(np.int16)
                sh, sw = base.shape[:2]
                masks = zone_masks(sw, sh)
                sil = pp.dilate(frames[0][:, :, 3] > 8, 1)
                peak = {p: 0 for p in PARTS}
                for img in frames[1:]:
                    d = (np.abs(img[:, :, :3].astype(np.int16) - base).max(axis=2) > THR) & sil
                    for p in PARTS:
                        n = int((d & masks[p]).sum())
                        if n > peak[p]:
                            peak[p] = n
                own = peak.get(part, 0)
                others = {k: v for k, v in peak.items() if k != part}
                omax = max(others.values() or [0])
                oname = max(others, key=others.get) if others else "-"
                ratio = (own / omax) if omax > 0 else 99.0
                results[part] = {"own": own, "others": peak, "ratio": round(ratio, 2)}
                log("[poke] %-6s 自己 %5d px ｜ 别的最大 %-5s %5d px ｜ 倍数 %.2f %s"
                    % (part, own, oname, omax, ratio, "✓" if ratio > 2 else "✗"))
                if part == "chest":
                    chest_frames = frames[1:4]
            finally:
                body.stop()
        if args.no_service:
            log("[poke] --no-service：服务没起，记得自己起回来")
    finally:
        if os.path.exists(bak):
            shutil.copyfile(bak, cfg)
            os.remove(bak)
        try:
            subprocess_run = __import__("subprocess").run
            subprocess_run(["systemctl", "--user", "start", "miyu-body.service"],
                           capture_output=True)
        except Exception:
            pass

    if chest_frames:
        # 点胸部抓 3 帧（间隔约 65ms），存成一张竖排对比图
        strip = []
        for img in chest_frames:
            rgbx = img[:, :, :3].astype(np.float32)
            a = img[:, :, 3].astype(np.float32) / 255.0
            comp = rgbx * a[..., None] + np.array([18, 18, 24], np.float32) * (1 - a[..., None])
            strip.append(Image.fromarray(comp.astype(np.uint8)))
        w, h = strip[0].size
        crop = strip[0].crop((int(w * 0.30), int(h * 0.22), int(w * 0.72), int(h * 0.62)))
        cw, ch = crop.size
        out = Image.new("RGB", (cw * 3 + 8, ch), (10, 10, 12))
        for i, im in enumerate(strip):
            out.paste(im.crop((int(w * 0.30), int(h * 0.22), int(w * 0.72), int(h * 0.62))),
                      (i * (cw + 4), 0))
        out.save(os.path.join(STATE, "shot-chest-physics.png"))
        log("[poke] 胸部三帧 → state/shot-chest-physics.png")

    json.dump(results, open(os.path.join(STATE, "poke-check.json"), "w"),
              ensure_ascii=False, indent=1)
    log("[poke] 账本 → state/poke-check.json")


if __name__ == "__main__":
    main()
