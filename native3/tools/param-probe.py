#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""参数探针：把 509 个参数逐个按死，量出「哪个参数驱动她身上哪一块」。

一句话：这是「点哪动哪」的地基。没有这张表，点她只能播一整套写死的动作。

怎么跑（一条命令，自己在副屏上起一个探针实例，跑完收摊）：

    ~/.dsh/miyu/body/native3/tools/param-probe.py            # 全部 509 个
    ~/.dsh/miyu/body/native3/tools/param-probe.py --named    # 只跑名字像部位的
    ~/.dsh/miyu/body/native3/tools/param-probe.py --limit 40 # 先试一小批

产出（都在 native3/state/）：
    param-map.json      活性参数 → 各部位的变化像素量（机器读，交付物）
    param-map.md        人看的表：一行一个参数，主要影响哪块、多少像素
    param-parts.tsv     给 C++ 读的扁平表（部位矩形 R 行 + 参数 P 行）
    probe-run.json      这一轮的运行记录（跑了几条、噪声底、耗时）

原理（为什么是截图而不是读顶点）：参数 → 顶点 的绑定在 moc3 里，SDK 不暴露；
但「设成 min / 设成 max / 各截一张 / 逐像素比」能直接量出这块皮动没动、动在哪。
setup 阶段先拍一张「她在」和一张「hide=true」，两张相减就是**她本人的剪影**——
后面所有比对的像素都只算剪影里的，桌面/浏览器页面的变化一概不算数。

坐标：所有部位矩形都用**她本人 bbox 的归一化坐标**（u 左→右、v 上→下，0..1）。
C++ 那边的点击判定（src/partpoke.h classify）用的就是同一批数字。
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import time

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE = os.path.join(ROOT, "state")
BIN = os.path.join(ROOT, "build", "miyu-wayland")
OUTPUT = "HDMI-A-3"
PROBE_IN = os.path.join(STATE, "probe.txt")
PROBE_DONE = os.path.join(STATE, "probe-done.txt")
PROBE_LOG = "/tmp/miyu-param-probe.log"
CDI3 = "/home/link/.dsh/miyu/body/model/大肥鱼.cdi3.json"

THR = 14          # 单像素差异阈值（JPEG 级噪声不会到这么高，肉眼可见的位移会）
MIN_TOTAL = 200   # 总变化像素低于这个 = 死参数（任务书定的线）

# ── 部位矩形（她本人 bbox 归一化，**按顺序判定，第一个命中赢**）──────────────
# 顺序为什么是这样：窄的、贴边的先判（尾巴 / 手臂 / 腿），大块的后判（头 / 裙）。
# 数字是照着 build 出来的 337x730 实拍图量的（state/shot-part-grid.png 那种九宫格）。
# 为什么腿只算一块、腰并进胸：探针量出来腿的参数是**对称**的（LEGAngleZ2 两条腿一起动，
# 左右占比 1:1），分开算只会让「该部位 > 其它部位」这条判据被自己人打自己人。腰同理——
# 腰上那点像素本来就是胸衣的一部分（Param53 在胸 6734 / 腰 3394），并起来才是一整块。
PARTS = [
    ("tail",   [(0.56, 0.60, 1.00, 0.90), (0.04, 0.66, 0.20, 0.99), (0.22, 0.90, 0.72, 1.00)]),
    ("armL",   [(0.20, 0.24, 0.35, 0.52)]),
    ("armR",   [(0.61, 0.24, 0.78, 0.52)]),
    ("leg",    [(0.26, 0.62, 0.63, 0.96)]),
    ("head",   [(0.18, 0.00, 0.80, 0.25)]),
    ("chest",  [(0.30, 0.25, 0.64, 0.47)]),
    ("skirt",  [(0.08, 0.45, 0.84, 0.64)]),
]
PART_NAMES = [p[0] for p in PARTS]

# 早期一版把 leg 拆成 legL/legR、chest 拆出 waist；读老 raw 时并回去（并起来和现在的
# 判定格子逐像素等价：那几张掩码本来就是互斥的）。
PER_MERGE = {"leg": ("legL", "legR"), "chest": ("waist",)}

# 名字闸门：掩码是按**画面位置**切的，可她的长发就垂在两条手臂的位置上——不问名字的话
# `LateHairX4` 会被当成「手臂的参数」，点手臂结果甩头发。模型的参数名是作者写的、
# 有语义的，这里就拿它把「这块位置上的像素其实属于别的部位」这个歧义拆开。
NAME_GATE = [
    ("Hair|Ear|Hat|Bow|Flip|Eye|Brow|Mouth|Cheek|Tongue|Emotion", {"head"}),
    ("Skirt|Lace", {"skirt"}),
    ("Chest", {"chest"}),
    ("Sleeve|Arm|Hand|Forearm|Shoulder", {"armL", "armR"}),
    ("Leg", {"leg"}),
]

# 一个参数在某部位上至少要有这么多像素才算数（200 的死线太松：几百像素是抗锯齿抖动）
MIN_PART_PX = 1500


def norm_per(per):
    """把 raw 里的 per 字典规整成当前 PART_NAMES 的口径。"""
    out = {}
    for k in PART_NAMES:
        out[k] = per.get(k, 0)
        for old_k in PER_MERGE.get(k, ()):
            out[k] += per.get(old_k, 0)
    return out

# 「名字像部位」的筛子（--named 用）：只跑看得懂的这些，跑不完时的退路
NAMED_RE = ("手臂|大臂|小臂|手腕|食指|中指|无名指|小指|肩|身体|头|耳|尾|裙|腿|脚|手|指|胸|腰|脖|背|眼|嘴|眉|脸")


# ── 小工具 ────────────────────────────────────────────────────────────────
def log(*a):
    print(*a, flush=True)


def write_atomic(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, path)


def grim(path, geom=None):
    cmd = ["grim"]
    if geom:
        cmd += ["-g", geom]
    else:
        cmd += ["-o", OUTPUT]
    cmd.append(path)
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise SystemExit("grim 失败：%s" % r.stderr.decode(errors="replace"))


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)


def dilate(m, n=2):
    for _ in range(n):
        m = m | np.roll(m, 1, 0) | np.roll(m, -1, 0) | np.roll(m, 1, 1) | np.roll(m, -1, 1)
    return m


# ── 探针实例（起 / 停）──────────────────────────────────────────────────────
class Body:
    def __init__(self, settle, no_phys):
        self.proc = None
        self.settle = settle
        self.no_phys = no_phys
        self.box = None          # (x, y, w, h) 她的全局矩形

    def stop_service(self):
        subprocess.run(["systemctl", "--user", "stop", "miyu-body.service"],
                       capture_output=True)
        time.sleep(0.6)

    def start_service(self):
        subprocess.run(["systemctl", "--user", "start", "miyu-body.service"],
                       capture_output=True)

    def start(self):
        if self.proc:
            return
        xdg = os.environ.get("XDG_RUNTIME_DIR") or "/run/user/%d" % os.getuid()
        wd = os.environ.get("WAYLAND_DISPLAY") or "wayland-1"
        if not os.path.exists(os.path.join(xdg, wd)):
            cands = [f for f in os.listdir(xdg) if f.startswith("wayland-")]
            if not cands:
                raise SystemExit("没有 wayland socket（合成器起来了没？）")
            wd = cands[0]
        env = dict(os.environ)
        env.update({
            "XDG_RUNTIME_DIR": xdg, "WAYLAND_DISPLAY": wd,
            "MIYU_OUTPUT": "1",
            "MIYU_PROBE": "1",
            "MIYU_PROBE_FILE": PROBE_IN,
            "MIYU_PROBE_DONE": PROBE_DONE,
            "MIYU_PROBE_SETTLE": str(self.settle),
            "MIYU_NO_IDLE": "1",
            "MIYU_NO_LOOK": "1",
            "MIYU_NO_TAP": "1",
            "MIYU_NO_EYES": "1",
        })
        if self.no_phys:
            env["MIYU_NO_PHYS"] = "1"
        else:
            env.pop("MIYU_NO_PHYS", None)
        env.pop("DISPLAY", None)
        if os.path.exists(PROBE_DONE):
            os.remove(PROBE_DONE)
        write_atomic(PROBE_IN, "gen=0\n")
        lf = open(PROBE_LOG, "wb")
        self.proc = subprocess.Popen([BIN], cwd=ROOT, env=env, stdout=lf, stderr=lf,
                                     start_new_session=True)
        # 等她挂上、等她报出全局矩形
        t0 = time.time()
        while time.time() - t0 < 25:
            time.sleep(0.3)
            try:
                with open(PROBE_LOG, "r", encoding="utf-8", errors="replace") as f:
                    txt = f.read()
            except OSError:
                continue
            for line in reversed(txt.splitlines()):
                if "[box] 全局" in line:
                    import re
                    m = re.search(r"她=\[(-?\d+),(-?\d+) \.\. (-?\d+),(-?\d+)\]", line)
                    if m:
                        x0, y0, x1, y1 = map(int, m.groups())
                        self.box = (x0, y0, x1 - x0, y1 - y0)
                        log("[probe] 她本人全局矩形 = %s" % (self.box,))
                        return
            if self.proc.poll() is not None:
                raise SystemExit("探针实例退出了，看 %s" % PROBE_LOG)
        raise SystemExit("等不到 [box] 全局 那行，看 %s" % PROBE_LOG)

    def geom(self, pad=6):
        x, y, w, h = self.box
        return "%d,%d %dx%d" % (x - pad, y - pad, w + 2 * pad, h + 2 * pad)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


# ── 一步一次：写 gen → 等 done → 截图 ──────────────────────────────────────
class Prober:
    def __init__(self, body, settle):
        self.body = body
        self.settle = settle
        self.gen = 0
        self.shots = 0
        self.t0 = time.time()

    def set(self, name, value):
        """把 name 按到 value（name 为空 = 只拍照，不动参数）。"""
        self.gen += 1
        body = "gen=%d\n" % self.gen
        if name is not None:
            body += "%s=%s\n" % (name, value)
        write_atomic(PROBE_IN, body)
        want = "gen=%d" % self.gen
        t0 = time.time()
        while time.time() - t0 < 20:
            if os.path.exists(PROBE_DONE):
                try:
                    with open(PROBE_DONE, "r", encoding="utf-8") as f:
                        got = f.read().strip()
                except OSError:
                    got = ""
                if got == want:
                    return True
            time.sleep(0.005)
        log("  !! gen=%d 等不到 done（跳过这一张）" % self.gen)
        return False

    def shot(self, path):
        grim(path, self.body.geom())
        self.shots += 1
        return path


# ── 分析 ──────────────────────────────────────────────────────────────────
def part_of(u, v):
    for name, rects in PARTS:
        for (u0, v0, u1, v1) in rects:
            if u0 <= u <= u1 and v0 <= v <= v1:
                return name
    return "other"


def make_masks(h, w):
    """每个部位的布尔掩码（她本人 bbox 归一化 → 像素下标）。"""
    vv, uu = np.mgrid[0:h, 0:w]
    un = (uu + 0.5) / w
    vn = (vv + 0.5) / h
    masks = {}
    left = np.ones((h, w), bool)
    for name, rects in PARTS:          # 按优先级，先出的格子从后面扣掉
        m = np.zeros((h, w), bool)
        for (u0, v0, u1, v1) in rects:
            m |= (un >= u0) & (un <= u1) & (vn >= v0) & (vn <= v1)
        masks[name] = m & left
        left &= ~m
    masks["other"] = left
    return masks


def part_centroids(img, BG, masks):
    """每块剪影的质心（和 part-tap-check.py 共用一个实现，省得两套口径对不上）。"""
    fg = np.abs(img - BG).max(axis=2) > THR
    yy, xx = np.mgrid[0:img.shape[0], 0:img.shape[1]]
    out = {}
    for k, m in masks.items():
        mm = fg & m
        n = int(mm.sum())
        out[k] = None if n < 50 else (float((xx * mm).sum() / n), float((yy * mm).sum() / n))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--named", action="store_true", help="只跑名字像部位的（跑不完的退路）")
    ap.add_argument("--all", action="store_true", help="509 个全跑（默认行为）")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--settle", type=int, default=10, help="每一代撑多少帧再截图")
    ap.add_argument("--physics-pass", action="store_true", help="额外跑 AngleX/PositionY 等（物理开）")
    ap.add_argument("--no-service", action="store_true", help="不管 systemd，外面自己起停")
    ap.add_argument("--keep", type=int, default=0, help="保留前 N 张截图到 state/probe-shots/")
    ap.add_argument("--emit-only", action="store_true",
                    help="只从 state/probe-raw.json 重新出表（不重新截图）")
    args = ap.parse_args()

    if not os.path.exists(BIN):
        raise SystemExit("先编译：cmake --build build")

    cdi = json.load(open(CDI3, encoding="utf-8"))
    names = [p["Id"] for p in cdi["Parameters"]]
    cn = {p["Id"]: p.get("Name", "") for p in cdi["Parameters"]}
    if args.named:
        import re
        rx = re.compile(NAMED_RE)
        names = [n for n in names if rx.search(cn.get(n, "")) or rx.search(n)]
        log("[probe] --named 筛出 %d 个（509 → %d）" % (len(names), len(names)))
    if args.limit:
        names = names[:args.limit]

    shotdir = os.path.join(STATE, "probe-shots")
    os.makedirs(shotdir, exist_ok=True)
    keep = args.keep

    if args.emit_only:
        emit_from_raw()
        return

    # 探针实例会读 state/settings.json 并且**把它写回去**（hide 那种 POST 会落盘），
    # 而它带着 MIYU_NO_TAP / MIYU_NO_IDLE 这些排查开关 —— 不备份的话探针跑完
    # 她的「点击反应」会被永久关掉（踩过一次）。收工原样还回去。
    cfg = os.path.join(STATE, "settings.json")
    cfg_bak = cfg + ".probe-bak"
    if os.path.exists(cfg):
        shutil.copyfile(cfg, cfg_bak)

    body = Body(args.settle, no_phys=True)
    prober = Prober(body, args.settle)
    t_start = time.time()
    result = {}
    nshots_before = 0
    noise_floor = None
    mask = None
    masks = None

    try:
        if not args.no_service:
            body.stop_service()
        body.start()
        geom = body.geom()
        log("[probe] 截图区域 = %s（%s）" % (geom, OUTPUT))

        # ── setup：剪影 + 噪声底 ───────────────────────────────────────
        if not prober.set(None, 0):
            raise SystemExit("第一张基准图都拍不出来")
        a = prober.shot(os.path.join(shotdir, "s0.png"))
        # hide 的时候主循环整帧不画，探针的 done 也就不会来 —— 别等它，睡一下直接拍
        subprocess.run(["curl", "-s", "-X", "POST", "-d", '{"hide":true}',
                        "http://127.0.0.1:8200/set"], capture_output=True)
        time.sleep(0.4)
        b = prober.shot(os.path.join(shotdir, "s1.png"))
        subprocess.run(["curl", "-s", "-X", "POST", "-d", '{"hide":false}',
                        "http://127.0.0.1:8200/set"], capture_output=True)
        prober.set(None, 0)
        base = prober.shot(os.path.join(shotdir, "base.png"))

        A, B, BASE = load_rgb(a), load_rgb(b), load_rgb(base)
        h, w = BASE.shape[:2]
        mask = dilate(np.abs(A - B).max(axis=2) > THR, 2)
        masks = make_masks(h, w)
        sil = int(mask.sum())
        log("[probe] 剪影 %d px（%dx%d 图里 %.1f%%）" % (sil, w, h, 100.0 * sil / (w * h)))
        if sil < 10000:
            log("[probe] !! 剪影太小，hide 可能没生效 —— 后面的数字不可信")

        # 噪声底：同一个参数值拍两次，差多少像素才算「真的有变化」
        prober.set(None, 0)
        n1 = prober.shot(os.path.join(shotdir, "noise1.png"))
        prober.set(None, 0)
        n2 = prober.shot(os.path.join(shotdir, "noise2.png"))
        noise_floor = int((np.abs(load_rgb(n1) - load_rgb(n2)).max(axis=2) > THR)[mask].sum())
        log("[probe] 噪声底 = %d px（静止两帧之间自己动的像素，阈值 %d）" % (noise_floor, THR))

        # ── 扫 ────────────────────────────────────────────────────────
        rng = read_ranges()
        total = len(names)
        t0 = time.time()
        for i, nm in enumerate(names):
            t_mark = time.time()
            r = rng.get(nm)
            if not r:
                continue
            lo, hi, dflt = r
            if abs(hi - lo) < 1e-4:
                continue
            if prober.set(nm, "%.4f" % lo):
                pmin = os.path.join(shotdir, "p.png")
                prober.shot(pmin)
            else:
                continue
            if prober.set(nm, "%.4f" % hi):
                pmax = os.path.join(shotdir, "q.png")
                prober.shot(pmax)
            else:
                continue
            IMIN, IMAX = load_rgb(pmin), load_rgb(pmax)
            d = np.abs(IMIN - IMAX).max(axis=2) > THR
            d = d & mask
            tot = int(d.sum())
            per = {k: int((d & m).sum()) for k, m in masks.items()}
            dmin = int((np.abs(IMIN - BASE).max(axis=2) > THR)[mask].sum())
            dmax = int((np.abs(IMAX - BASE).max(axis=2) > THR)[mask].sum())
            result[nm] = {"per": per, "total": tot, "dir": "min" if dmin >= dmax else "max",
                          "dmin": dmin, "dmax": dmax}

            # 每 25 个刷一次基准图（物理/AA 可能有极慢的漂移，基准跟着走）
            if i and i % 25 == 0:
                prober.set(None, 0)
                BASE = load_rgb(prober.shot(os.path.join(shotdir, "base.png")))

            t_set = time.time() - t_mark
            if (i + 1) % 10 == 0 or i + 1 == total:
                el = time.time() - t0
                eta = el / (i + 1) * (total - i - 1)
                log("[probe] %d/%d  %s=%.2f/%.2f  %d px  (%.0f ms/个)  已用 %.0fs，预计还要 %.0fs"
                    % (i + 1, total, nm, lo, hi, tot, t_set * 1000.0, el, eta))

        nshots_before = prober.shots
    finally:
        body.stop()
        if os.path.exists(cfg_bak):
            # 先还文件，再让服务起来（服务启动时读的就是还回去的那份）
            shutil.copyfile(cfg_bak, cfg)
            os.remove(cfg_bak)
        if not args.no_service:
            body.start_service()

    # ── 出表 ──────────────────────────────────────────────────────────
    write_atomic(os.path.join(STATE, "probe-raw.json"),
                 json.dumps({"result": result, "noise_floor": noise_floor,
                             "box": body.box}, ensure_ascii=False) + "\n")
    rng = read_ranges()
    live, dead = {}, []
    for nm, r in result.items():
        if r["total"] < MIN_TOTAL:
            dead.append(nm)
            continue
        per = norm_per(r["per"])
        live[nm] = {k: per.get(k, 0) for k in PART_NAMES}
        live[nm]["other"] = r["per"].get("other", 0)

    # 按「总变化」排序后写 json（顺序对 C++ 没意义，对人看有意义）
    ordered = sorted(live.items(), key=lambda kv: -result[kv[0]]["total"])
    with open(os.path.join(STATE, "param-map.json"), "w", encoding="utf-8") as f:
        f.write("{\n")
        f.write('  "_about": "tools/param-probe.py 生成：每个参数设成 min/max 各截一张，'
                '差出来的像素按部位统计。键是参数 id，值是该部位的变化像素量",\n')
        f.write('  "_noise_floor": %d,\n' % noise_floor)
        for idx, (nm, per) in enumerate(ordered):
            f.write("  %s: %s%s\n" % (json.dumps(nm, ensure_ascii=False),
                                      json.dumps(per, ensure_ascii=False, sort_keys=False),
                                      "," if idx + 1 < len(ordered) else ""))
        f.write("}\n")

    # ── 人看的 md ─────────────────────────────────────────────────────
    write_md(ordered, result, dead, cn, noise_floor)

    # ── C++ 读的扁平表 ────────────────────────────────────────────────
    write_tsv(ordered, result, rng)

    run = {
        "params_seen": len(names), "shots": prober.shots,
        "seconds": round(time.time() - t_start, 1),
        "noise_floor": noise_floor, "live": len(live), "dead": len(dead),
        "min_total": MIN_TOTAL, "settle_frames": args.settle,
        "physics_off": True, "named_filter": bool(args.named),
        "box": body.box,
    }
    write_atomic(os.path.join(STATE, "probe-run.json"),
                 json.dumps(run, ensure_ascii=False, indent=1) + "\n")

    log("")
    log("[probe] 完事：%d 个参数里 %d 个真的驱动画面（>%d px），%d 个是空壳"
        % (len(names), len(live), MIN_TOTAL, len(dead)))
    log("[probe] 截图 %d 张，用时 %.0fs，噪声底 %d px"
        % (prober.shots, time.time() - t_start, noise_floor))
    log("[probe] -> state/param-map.json  state/param-map.md  state/param-parts.tsv")
    for nm, per in ordered[:15]:
        hits = ", ".join("%s=%d" % (k, v) for k, v in per.items() if v > 0)
        log("  %-28s %6d px  %s" % (nm, result[nm]["total"], hits))


def emit_from_raw():
    """从 probe-raw.json 重出 param-map.json / .md / param-parts.tsv（不出图）。"""
    raw = json.load(open(os.path.join(STATE, "probe-raw.json"), encoding="utf-8"))
    result = raw["result"]
    cdi = json.load(open(CDI3, encoding="utf-8"))
    cn = {p["Id"]: p.get("Name", "") for p in cdi["Parameters"]}
    live = {}
    dead = []
    for nm, r in result.items():
        if r["total"] < MIN_TOTAL:
            dead.append(nm)
            continue
        per = norm_per(r["per"])
        live[nm] = {k: per.get(k, 0) for k in PART_NAMES}
        live[nm]["other"] = r["per"].get("other", 0)
    ordered = sorted(live.items(), key=lambda kv: -result[kv[0]]["total"])
    with open(os.path.join(STATE, "param-map.json"), "w", encoding="utf-8") as f:
        f.write("{\n")
        f.write('  "_about": "tools/param-probe.py 生成：每个参数设成 min/max 各截一张，'
                '差出来的像素按部位统计。键是参数 id，值是该部位的变化像素量",\n')
        f.write('  "_noise_floor": %d,\n' % raw.get("noise_floor", 0))
        for idx, (nm, per) in enumerate(ordered):
            f.write("  %s: %s%s\n" % (json.dumps(nm, ensure_ascii=False),
                                      json.dumps(per, ensure_ascii=False),
                                      "," if idx + 1 < len(ordered) else ""))
        f.write("}\n")
    write_md(ordered, result, dead, cn, raw.get("noise_floor", 0))
    write_tsv(ordered, result, read_ranges())
    log("[emit] %d 活 / %d 死 -> param-map.json / .md / param-parts.tsv" % (len(live), len(dead)))


def read_ranges():
    """state/param-range.tsv（二进制启动时导的：idx/name/min/max/default）。"""
    out = {}
    p = os.path.join(STATE, "param-range.tsv")
    if not os.path.exists(p):
        return out
    with open(p, "r", encoding="utf-8") as f:
        for line in f:
            if line.startswith("#"):
                continue
            c = line.rstrip("\n").split("\t")
            if len(c) < 5:
                continue
            try:
                out[c[1]] = (float(c[2]), float(c[3]), float(c[4]))
            except ValueError:
                continue
    return out


def write_md(ordered, result, dead, cn, noise_floor):
    p = os.path.join(STATE, "param-map.md")
    with open(p, "w", encoding="utf-8") as f:
        f.write("# 参数 → 部位 映射表（tools/param-probe.py 自动生成，别手改）\n\n")
        f.write("每个参数设成 min / max 各截一张，只数**她本人剪影内**的像素差；\n")
        f.write("部位是按她 bbox 归一化切的 9 块（tail/armL/armR/legL/legR/head/chest/waist/skirt）。\n")
        f.write("噪声底 = %d px（静止两帧自己动的像素），低于 %d px 的算死参数。\n\n"
                % (noise_floor, MIN_TOTAL))
        f.write("## 活的参数（%d 个，按总变化从小到大排）\n\n" % len(ordered))
        f.write("| 参数 | 中文名 | 总像素 | 主要影响 | 分块明细 | 方向 |\n")
        f.write("| --- | --- | ---: | --- | --- | --- |\n")
        for nm, per in ordered:
            r = result[nm]
            top = max(PART_NAMES, key=lambda k: per.get(k, 0))
            detail = " ".join("%s:%d" % (k, per[k]) for k in PART_NAMES if per.get(k, 0) > 0)
            f.write("| `%s` | %s | %d | **%s** | %s | %s |\n"
                    % (nm, cn.get(nm, ""), r["total"], top, detail,
                       "min 侧更明显" if r["dir"] == "min" else "max 侧更明显"))
        f.write("\n## 死参数（%d 个，设成 min/max 画面几乎不动）\n\n" % len(dead))
        for i, nm in enumerate(sorted(dead)):
            f.write("`%s`%s%s" % (nm, "" if i + 1 == len(dead) else "、",
                                  "\n" if (i + 1) % 6 == 0 else ""))


def gate_for(name):
    """这个参数名只允许落在哪些部位（没命中闸门 = 哪块都行）。"""
    import re
    for rx, parts in NAME_GATE:
        if re.search(rx, name):
            return parts
    return set(PART_NAMES)


def write_tsv(ordered, result, rng):
    p = os.path.join(STATE, "param-parts.tsv")
    with open(p, "w", encoding="utf-8") as f:
        f.write("# 部位级反应的数据表（tools/param-probe.py 生成，别手改）\n")
        f.write("# R<TAB>部位名<TAB>u0<TAB>v0<TAB>u1<TAB>v1   —— 归一化矩形，按行序优先判定\n")
        f.write("# P<TAB>参数id<TAB>部位<TAB>分数<TAB>peak<TAB>dip  —— peak/dip 是绝对参数值，\n")
        f.write("#    peak = 默认值 + 60% 的摆幅，dip = 默认值 − 20%（「弹一下」曲线的两个锚点）\n")
        for name, rects in PARTS:
            for (u0, v0, u1, v1) in rects:
                f.write("R\t%s\t%.3f\t%.3f\t%.3f\t%.3f\n" % (name, u0, v0, u1, v1))

        # 挑「专一」的参数：这个部位占它全部变化的 45% 以上，且是第二名的 1.5 倍以上。
        #   · 没有这条，`ParamBodyAngleZ`（整个人一起转）会被当成尾巴的参数 —— 点尾巴
        #     整个人平移，那就不叫点哪动哪了。
        #   · 排序按**专一度**（share）而不是像素总量：尾巴那根 `Param_Angle_Rotation88`
        #     只有一万像素，但它 95% 都在尾巴上，比三万像素散在全身的髋关节参数更该被选。
        #   · 排掉 `key*`（按键组：表情开关）和 `Clothes*`（换装）：那是「换一张皮」，
        #     不是「这个部位动一下」。
        for name in PART_NAMES:
            cands = []
            for nm, per in ordered:
                if nm.startswith("key") or nm.startswith("Clothes") or nm.startswith("Paramshuiyin"):
                    continue
                if name not in gate_for(nm):
                    continue
                if per.get(name, 0) < MIN_PART_PX:
                    continue
                vals = sorted((per.get(k, 0) for k in PART_NAMES), reverse=True)
                tot = max(1, sum(per.get(k, 0) for k in PART_NAMES))
                share = per.get(name, 0) / tot
                ratio = per.get(name, 0) / max(1, vals[1] if len(vals) > 1 else 1)
                if share < 0.45 or ratio < 1.5:
                    continue            # 全局参数（整个人一起动）不进来
                cands.append((per.get(name, 0), nm))
            cands.sort(reverse=True)
            for px, nm in cands[:4]:
                r = result[nm]
                rg = rng.get(nm)
                if not rg:
                    continue
                lo, hi, dflt = rg
                ext = lo if r["dir"] == "min" else hi
                swing = ext - dflt
                if abs(swing) < 1e-3:
                    continue
                peak = dflt + 0.6 * swing
                dip = dflt - 0.2 * swing
                f.write("P\t%s\t%s\t%d\t%.4f\t%.4f\n" % (nm, name, r["total"], peak, dip))


if __name__ == "__main__":
    main()
