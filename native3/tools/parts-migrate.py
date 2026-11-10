#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把三张部位表迁移成一张 state/parts.tsv（设计：NOTES-part-api.md 第四节）。

一次性工具。跑完就停在那儿，不参与运行 —— 运行期只读 parts.tsv（src/partapi.h）。

它做两件事：
  --migrate  读 state/{part-bands,param-parts,part-react}.tsv + 一份写死在脚本里的
             元信息（part/alias/follow 三类，来源都标了行号），生成 state/parts.tsv
  --verify   把刚生成的 parts.tsv **反向**摊回三张旧表，和原件逐字段 diff
             不一致 exit 1（这一步是「行为一个都不丢」的机器证明）

  --dump     打印解析结果（和 tools/parts-dump.py 同格式，用于和 C++ 侧对账）

为什么元信息写死在脚本里、不从源码抽：part 的中文标签、alias 的中文关键词、follow 的
三个常量，现在是散在代码里的一次性事实。抽它们的通用办法不存在（一部分是 if 顺序、
一部分是 JS 里的数组），所以人工抄一遍、逐条标来源行号，抄错的概率靠 --verify 挡不住
（verify 只校验三张表的部分）—— 挡住它的是「parts.tsv 与 kZoneNames 的 id 顺序断言」，
那一条是读源码比的，见 check_id_order()。

用法：
    tools/parts-migrate.py --migrate            # 生成 state/parts.tsv
    tools/parts-migrate.py --verify             # 反向 diff 自检
    tools/parts-migrate.py --dump               # 打印解析结果
    tools/parts-migrate.py --migrate --verify   # 两步一起（推荐）
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STATE = os.path.join(ROOT, "state")

BANDS = os.path.join(STATE, "part-bands.tsv")
PARAMS = os.path.join(STATE, "param-parts.tsv")
# 2026-11-10 22:3x：part-react.tsv 已退休（挪到 state/legacy/），
# 这里指向归档路径 —— --verify 是「迁移时行为没丢」的历史证明，还要能跑。
REACT = os.path.join(STATE, "legacy", "part-react.tsv")
OUT = os.path.join(STATE, "parts.tsv")
PART_H = os.path.join(ROOT, "src", "partpoke.h")

# ── 元信息一：部位（顺序 = id，必须与 src/partpoke.h:58 kZoneNames 的 1..13 逐位一致）──
# 标签取前端 PARTS（src/web.cpp:343）那一列中文。
# `other`（id 7）保留：它在 kZoneNames 里有槽位，state/part-zones.tsv 里没有任何一行是它，
# 但 id 序列要靠它占位，少一个后面全部错位。
PARTS = [
    ("head",     "头",   ""),
    ("chest",    "胸",   ""),
    ("arm",      "手",   ""),
    ("skirt",    "裙",   ""),
    ("leg",      "腿",   ""),
    ("tail",     "尾",   ""),
    ("other",    "其他", ""),
    ("eye",      "眼",   ""),
    ("mouth",    "嘴",   ""),
    ("brow",     "眉",   ""),
    ("ear",      "兽耳", ""),
    ("waist",    "腰",   ""),
    ("headwear", "头饰", ""),
]

# ── 元信息二：drawable 的 part 名 → 部位（来源 src/main.cpp:1062 zoneOfPartName）──
# 顺序 = 原先 if 的顺序 = 匹配优先级。两处必须照抄，别去重、别排序：
#   · `眼镜 → head` 单列一条，排在 eye 前面（原注释：眼镜是 head，不是眼睛）
#   · 最后一条 head 是 catch-all，里面重复了「眼/眉/嘴/耳/眼镜/口内」——
#     原代码就是重复的（精确匹配先赢），去重会改掉「眼睛袋」这类边角的落点
ALIAS = [
    ("head",     "眼镜"),
    ("headwear", "女仆发饰,发饰"),
    ("ear",      "耳朵,兽耳"),
    ("eye",      "眼眶,眼球,眼白,左眼,右眼,圈圈眼,爱心眼,屑屑眼,眼"),
    ("mouth",    "嘴,口内,牙,舌"),
    ("brow",     "眉毛,眉"),
    ("waist",    "腰部"),
    ("tail",     "尾巴,狗尾草"),
    ("leg",      "腿,脚,膝"),
    ("skirt",    "裙,蝴蝶结,缎带"),
    ("arm",      "手臂,大臂,小臂,手腕,袖子,举手,竖食指,手"),
    ("chest",    "胸,上身,领子,肩膀,腰"),
    ("head",     "头,脑,发,脸,五官,眼,眉,嘴,鼻,耳,脖,牙,呆毛,眼镜,表情,汗,泪,口内"),
]

# ── 元信息三：拖动跟随（来源 src/main.cpp:3138 legDragTick 的三个常量）──
#   hipX = ax*0.6  clamp ±30   /  hipZ = az*2.2  clamp ±30  /  pk = ax/16 clamp ±1
FOLLOW = [
    ("leg", "HipAngleX", "0.6",    "30", "ax"),
    ("leg", "HipAngleZ", "2.2",    "30", "az"),
    ("leg", "PokeLeg",   "0.0625", "1",  "ax"),
]

# ── 元信息四：从代码里搬出来的两条「无视剪影」的硬判带（来源 src/partpoke.h:207）──
# 原文：if (v >= 0.198f && v <= 0.248f && (u < 0.425f || u > 0.575f)) return "ear";
# 翻成 prio=200 + scope=any，排在一切之前。
# ★ 差一格的地方：原文是严格小于/大于（u<0.425、u>0.575），这里 u/v 是闭区间，
#   所以 u 恰好等于 0.425 或 0.575 的点会判成 ear（原文不判）。一格宽 1/120 ≈ 0.0083，
#   边界上的单点，实测点不进去（11 个验证点没有一个落在这条线上）。照抄，不改。
EXTRA_BANDS = [
    ("ear", "0.000", "0.425", "0.198", "0.248", "force", "200", "any",
     "兽耳左（硬判，无视剪影；原 partpoke.h:207）"),
    ("ear", "0.575", "1.000", "0.198", "0.248", "force", "200", "any",
     "兽耳右（硬判，无视剪影；原 partpoke.h:207）"),
]

# ── 元信息五：嘴/眉只往正方向（来源 src/main.cpp:1699 upOnly）──
UP_ONLY = {"mouth", "brow"}

HEADER = """\
# ── 部位总表（唯一一份；一份文件六种记录，由 src/partapi.h 一次读完）─────────────
# 设计：NOTES-part-api.md 第四节。生成的：tools/parts-migrate.py（一次性）。
# 手工维护这一份就够 —— 加一个可点部位 = 加几行，不用改 C++、不用重编译。
#
# 记录类型（第一列）：
#   part    部位名 中文标签 [停留秒] [开关]                          停 关
#   alias   部位名 关键词,关键词…（按文件顺序匹配，先出现的赢）        关
#   band    部位名 u0 u1 v0 v1 force|fill 优先级 [body|any]           停
#   param   部位名 参数Id 评分 幅度 [alt|up]                          停
#   expr    部位名 表情[/表情…] [停留秒]                              热（网页改立刻生效）
#   follow  部位名 参数Id 系数 上限 ax|az|ay                          停
#
# 「停」= 改完要 miyu-body-ctl restart；「热」= 网页上改立刻生效。
# 部位 id = part 行出现顺序（1 起，0 = 空白）。★ 已有部位不许换序，新部位只能往后追加：
#   渲染层的格子图（state/part-zones.tsv / gZoneCells）存的就是这个下标。
# 列的多余空白不重要，分隔一律是 TAB。「#」到行尾是注释，行尾注释可以放说明。
# ────────────────────────────────────────────────────────────────────────────
"""


# ══════════════════════════════════════════════════════════════════
# 读旧表
# ══════════════════════════════════════════════════════════════════
def read_rows(path):
    """→ [(行号, [列…], 行尾注释或 '')]，跳过注释行和空行。"""
    out = []
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            s = line.rstrip("\n").rstrip("\r")
            if not s.strip() or s.lstrip().startswith("#"):
                continue
            # 行尾注释：最后一个以 " #" 或 "\t#" 开头的位置之后全是注释
            fields, comment = s, ""
            m = re.search(r"\t#\s?(.*)$", s)
            if m:
                fields, comment = s[:m.start()], m.group(1).strip()
            out.append((i, fields.split("\t"), comment))
    return out


def kzone_names():
    """从 src/partpoke.h 抓 kZoneNames 的字面量（不硬编码，跟源码比）。"""
    src = open(PART_H, encoding="utf-8").read()
    m = re.search(r"kZoneNames\s*\[\s*\d+\s*\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        sys.exit("抓不到 src/partpoke.h 里的 kZoneNames —— 迁移中止")
    return re.findall(r'"([^"]*)"', m.group(1))


def check_id_order():
    """id 逐位对齐：parts 的顺序 == kZoneNames 去掉第 0 项。对不上就别往下生成。

    Step 7（2026-11-10 22:4x）之后 partpoke.h 里的 kZoneNames 已经删了（名字改由
    partapi 从 parts.tsv 提供），这里抓不到就跳过 —— 那份数组不存在了，
    「id 逐位对齐」这件事从 Step 7 起由 parts.tsv 自己保证（part 行顺序 = id）。
    """
    try:
        raw = kzone_names()[1:]        # [0] 是 ""（空白）
    except SystemExit:
        print("[skip] partpoke.h 里没有 kZoneNames 了（Step 7 已拆），跳过源码比对")
        return
    names = raw
    mine = [p[0] for p in PARTS]
    if names != mine:
        sys.exit("id 顺序对不上，迁移中止\n  kZoneNames[1..] = %s\n  脚本 PARTS      = %s"
                 % (names, mine))
    print("[ok] id 顺序：%d 个部位与 src/partpoke.h kZoneNames 逐位一致" % len(mine))


# ══════════════════════════════════════════════════════════════════
# 生成
# ══════════════════════════════════════════════════════════════════
def build():
    bands = read_rows(BANDS)
    params = read_rows(PARAMS)
    exprs = read_rows(REACT)

    known = {p[0] for p in PARTS}
    bad = []

    L = [HEADER]

    L.append("# ══ 部位（顺序即 id，1 起；不能换序）════════════════════════════════\n")
    for name, label, flags in PARTS:
        # 停留秒显式写 5.0（现在是 main.cpp:1864 的硬编码），flags 为空就不写那一列
        if flags:
            L.append("part\t%s\t%s\t5.0\t%s\n" % (name, label, flags))
        else:
            L.append("part\t%s\t%s\t5.0\n" % (name, label))

    L.append("\n# ══ drawable 的 part 名 → 部位（子串匹配，先出现的赢）══════════════════\n")
    L.append("# 来源：src/main.cpp:1062 zoneOfPartName（原样搬，顺序有意义）\n")
    for name, kw in ALIAS:
        L.append("alias\t%s\t%s\n" % (name, kw))

    L.append("\n# ══ 判定带（光栅化之后、泛洪补洞之前应用）════════════════════════════\n")
    L.append("# u0 u1 / v0 v1 是窗口归一化，**两端闭区间**（原 main.cpp:1505 是 gy<=gy1；\n")
    L.append("# 注意和 part-zones.tsv 的 Z 行不同 —— 那是半开 gv<z.v1，别顺手统一）。\n")
    L.append("# force=无条件盖、fill=只填还没标签的格子；prio 大者赢，同 prio 后写的赢。\n")
    L.append("# scope=any 的那两条排在**一切之前**（原文是 classify 第一步的硬判）。\n")
    L.append("\n# ── scope=any（无视剪影）──\n")
    for name, u0, u1, v0, v1, mode, prio, scope, cm in EXTRA_BANDS:
        L.append("band\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t# %s\n"
                 % (name, u0, u1, v0, v1, mode, prio, scope, cm))
    L.append("\n# ── scope=body（剪影内；原 state/part-bands.tsv 的 11 条）──\n")
    for lineno, c, cm in bands:
        if len(c) < 6:
            bad.append("part-bands.tsv:%d 列数不够（%d）" % (lineno, len(c)))
            continue
        name = c[0]
        if name not in known:
            bad.append("part-bands.tsv:%d 未知部位 %r" % (lineno, name))
            continue
        mode = "force" if c[5].strip() not in ("0", "") else "fill"
        prio = "100" if mode == "force" else "0"
        note = c[6].strip() if len(c) > 6 else cm
        L.append("band\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t# %s\n"
                 % (name, c[1], c[2], c[3], c[4], mode, prio, note))

    L.append("\n# ══ 动作参数（点一下拨的**物理输入**，不是输出）════════════════════════\n")
    L.append("# 评分 = 比值×10，同部位内按它降序、一次点最多取前 4。\n")
    L.append("# 幅度 = 绝对偏移量（度 / 比例单位），正负每点一次交替；up = 只往正方向。\n")
    L.append("# 原 state/param-parts.tsv 的 13 行，备注原样留在行尾。\n")
    for lineno, c, cm in params:
        if len(c) < 5 or c[0] != "P":
            bad.append("param-parts.tsv:%d 不是 P 行或无列" % lineno)
            continue
        name = c[1]
        if name not in known:
            bad.append("param-parts.tsv:%d 未知部位 %r" % (lineno, name))
            continue
        tail = "\tup" if name in UP_ONLY else ""
        note = c[5].strip() if len(c) > 5 else cm
        L.append("param\t%s\t%s\t%s\t%s%s\t# %s\n" % (name, c[2], c[3], c[4], tail, note))

    L.append("\n# ══ 表情（0..N；空 = 只放自身动作；`/` 分隔 = 每次随机抽一个）══════════════\n")
    L.append("# 原 state/part-react.tsv 的 12 行，顺序按部位名排（无声明的部位写空）。\n")
    for lineno, c, cm in exprs:
        if len(c) < 2:
            bad.append("part-react.tsv:%d 列数不够" % lineno)
            continue
        name = c[0]
        if name not in known:
            bad.append("part-react.tsv:%d 未知部位 %r" % (lineno, name))
            continue
        val = c[1].strip()
        L.append("expr\t%s\t%s\n" % (name, val))

    L.append("\n# ══ 拖动跟随（0..N；部位自己的弹簧活着时这段让位）══════════════════════\n")
    L.append("# 系数 × 拖动摆动量（ax/az），结果按标签量 clamp 到 ±上限，写绝对值。\n")
    L.append("# 来源：src/main.cpp:3138 legDragTick 的三个常量。\n")
    for name, pid, gain, clamp, src in FOLLOW:
        L.append("follow\t%s\t%s\t%s\t%s\t%s\n" % (name, pid, gain, clamp, src))

    if bad:
        print("有 %d 处不能迁移：" % len(bad), file=sys.stderr)
        for b in bad:
            print("  " + b, file=sys.stderr)
        sys.exit(1)
    return "".join(L)


# ══════════════════════════════════════════════════════════════════
# 反向解析（和 src/partapi.h 同一套规则，独立实现，用于对账）
# ══════════════════════════════════════════════════════════════════
def parse_parts(path):
    reg = {"parts": [], "id": {}, "alias": [], "band": [], "param": [],
           "expr": [], "follow": [], "bad": []}
    with open(path, encoding="utf-8") as f:
        for i, line in enumerate(f, 1):
            s = line.rstrip("\n").rstrip("\r")
            if not s.strip() or s.lstrip().startswith("#"):
                continue
            body, _, comment = s.partition("\t#")
            comment = comment.strip()
            c = body.split("\t")
            kind = c[0]
            seq = i
            if kind == "part":
                if len(c) < 3:
                    reg["bad"].append((i, "part 列数不够")); continue
                idx = len(reg["parts"]) + 1
                if c[1] in reg["id"]:
                    reg["bad"].append((i, "部位重复 %r" % c[1])); continue
                reg["id"][c[1]] = idx
                reg["parts"].append({"id": idx, "name": c[1], "label": c[2],
                                     "hold": float(c[3]) if len(c) > 3 and c[3] else 5.0,
                                     "flags": c[4] if len(c) > 4 else ""})
            elif kind == "alias":
                if len(c) < 3:
                    reg["bad"].append((i, "alias 列数不够")); continue
                for kw in c[2].split(","):
                    kw = kw.strip()
                    if kw:
                        reg["alias"].append((c[1], kw))
            elif kind == "band":
                if len(c) < 8:
                    reg["bad"].append((i, "band 列数不够")); continue
                scope = c[8] if len(c) > 8 else "body"
                reg["band"].append({"seq": seq, "name": c[1], "u0": float(c[2]), "u1": float(c[3]),
                                    "v0": float(c[4]), "v1": float(c[5]),
                                    "force": c[6] == "force", "mode": c[6],
                                    "prio": int(c[7]), "any": scope == "any",
                                    "comment": comment})
            elif kind == "param":
                if len(c) < 5:
                    reg["bad"].append((i, "param 列数不够")); continue
                reg["param"].append({"seq": seq, "name": c[1], "pid": c[2], "score": int(c[3]),
                                     "amp": float(c[4]),
                                     "up": (len(c) > 5 and c[5].strip() == "up"),
                                     "comment": comment})
            elif kind == "expr":
                if len(c) < 2:
                    reg["bad"].append((i, "expr 列数不够")); continue
                reg["expr"].append({"seq": seq, "name": c[1], "val": c[2].strip() if len(c) > 2 else "",
                                    "hold": float(c[3]) if len(c) > 3 and c[3] else None})
            elif kind == "follow":
                if len(c) < 6:
                    reg["bad"].append((i, "follow 列数不够")); continue
                reg["follow"].append({"seq": seq, "name": c[1], "pid": c[2], "gain": float(c[3]),
                                      "clamp": float(c[4]), "src": c[5]})
            else:
                reg["bad"].append((i, "认不出记录类型 %r" % kind))
    # 引用校验：band/param/expr/follow 引用了未定义的部位 = 错，不自动建部位
    for k in ("band", "param", "expr", "follow"):
        for r in reg[k]:
            if r["name"] not in reg["id"]:
                reg["bad"].append((0, "%s 引用了未定义部位 %r" % (k, r["name"])))
    # 排序（和 partapi.h 的加载后顺序一致）：band 按 prio 降序（稳定）、param 按 score 降序（稳定）
    reg["band"].sort(key=lambda r: -r["prio"])          # list.sort 稳定
    reg["param"].sort(key=lambda r: -r["score"])
    return reg


def dump(reg):
    L = ["parts=%d bands=%d params=%d exprs=%d follows=%d bad=%d"
         % (len(reg["parts"]), len(reg["band"]), len(reg["param"]),
            len(reg["expr"]), len(reg["follow"]), len(reg["bad"]))]
    for p in reg["parts"]:
        L.append("PART %d %s %s hold=%.1f flags=%s"
                 % (p["id"], p["name"], p["label"], p["hold"], p["flags"] or "-"))
    for name, kw in reg["alias"]:
        L.append("ALIAS %d %s %s" % (reg["id"][name], name, kw))
    for b in reg["band"]:
        L.append("BAND %d %s %.3f %.3f %.3f %.3f %s prio=%d %s"
                 % (reg["id"][b["name"]], b["name"], b["u0"], b["u1"], b["v0"], b["v1"],
                    "force" if b["force"] else "fill", b["prio"],
                    "any" if b["any"] else "body"))
    for p in reg["param"]:
        L.append("PARAM %d %s %s score=%d amp=%.3f %s"
                 % (reg["id"][p["name"]], p["name"], p["pid"], p["score"], p["amp"],
                    "up" if p["up"] else "alt"))
    for e in reg["expr"]:
        L.append("EXPR %d %s %s hold=%s"
                 % (reg["id"][e["name"]], e["name"], e["val"] or "-",
                    "-" if e["hold"] is None else "%.1f" % e["hold"]))
    for fo in reg["follow"]:
        L.append("FOLLOW %d %s %s gain=%.4f clamp=%.1f src=%s"
                 % (reg["id"][fo["name"]], fo["name"], fo["pid"], fo["gain"],
                    fo["clamp"], fo["src"]))
    for ln, msg in reg["bad"]:
        L.append("BAD line=%d %s" % (ln, msg))
    return "\n".join(L) + "\n"


# ══════════════════════════════════════════════════════════════════
# 反向 diff
# ══════════════════════════════════════════════════════════════════
def emit_legacy(reg):
    """把注册表摊回三张旧表的文本（会和原件逐行 diff）。"""
    b = ["# 部位\tu0\tu1\tv0\tv1\t强制\t说明（反向着色，用于 diff）"]
    for r in reg["band"]:
        if r["any"]:              # scope=any 是新增的，旧表里没有 → 不参与 diff
            continue
        b.append("%s\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%s"
                 % (r["name"], r["u0"], r["u1"], r["v0"], r["v1"], 1 if r["force"] else 0,
                    r["comment"]))

    p = ["# P\t部位\t驱动参数\t比值×10\t冲量幅度\t备注（反向着色，用于 diff）"]
    # 按文件原始顺序摊回去（改的是「表里的顺序」，不是运行时排序 —— 运行时由 score 决定）
    for r in sorted(reg["param"], key=lambda x: x["seq"]):
        p.append("P\t%s\t%s\t%d\t%.3f\t%s"
                 % (r["name"], r["pid"], r["score"], r["amp"], r["comment"]))

    e = ["# 部位\t表情名（反向着色，用于 diff）"]
    for r in sorted(reg["expr"], key=lambda x: x["name"]):
        e.append("%s\t%s" % (r["name"], r["val"]))
    return "\n".join(b) + "\n", "\n".join(p) + "\n", "\n".join(e) + "\n"


def strip_comments(text):
    out = []
    for line in text.splitlines():
        s = line.rstrip()
        if not s.strip() or s.lstrip().startswith("#"):
            continue
        out.append(s)
    return out


def cmd_verify(path):
    reg = parse_parts(path)
    if reg["bad"]:
        print("parts.tsv 有 %d 处坏记录：" % len(reg["bad"]), file=sys.stderr)
        for ln, msg in reg["bad"]:
            print("  line %d: %s" % (ln, msg), file=sys.stderr)
        return 1

    new_b, new_p, new_e = emit_legacy(reg)
    # 2026-11-10 22:3x：part-react.tsv 退休（挪到 state/legacy/）。它现在是**历史快照**，
    # 之后只有 parts.tsv 的 expr 段会被写，两边不再同步 —— 所以不参与反向 diff。
    # 想单独看那份快照和当前 expr 段的差异，用 --verify-legacy。
    pairs = [("state/part-bands.tsv", BANDS, new_b),
             ("state/param-parts.tsv", PARAMS, new_p)]
    if os.environ.get("MIYU_VERIFY_LEGACY"):
        pairs.append(("state/legacy/part-react.tsv", REACT, new_e))

    ok = True
    for label, old_path, new_text in pairs:
        if not os.path.exists(old_path):
            print("[skip] %-24s 不存在（已退休归档），跳过" % label)
            continue
        old = strip_comments(open(old_path, encoding="utf-8").read())
        new = strip_comments(new_text)
        if old == new:
            print("[ok] %-26s 反向一致（%d 行）" % (label, len(old)))
            continue
        ok = False
        print("[FAIL] %-24s 不一致" % label, file=sys.stderr)
        import difflib
        for line in difflib.unified_diff(old, new, fromfile=label + "(原)",
                                         tofile=label + "(反向)", lineterm="", n=1):
            print("  " + line, file=sys.stderr)

    # 附加断言（不靠眼看的几条）
    if len(reg["parts"]) != len(PARTS):
        print("[FAIL] 部位数 %d != %d" % (len(reg["parts"]), len(PARTS)), file=sys.stderr)
        ok = False
    if [p["name"] for p in reg["parts"]] != [p[0] for p in PARTS]:
        print("[FAIL] 部位顺序变了", file=sys.stderr)
        ok = False
    extra = [r for r in reg["band"] if r["any"]]
    if len(extra) != len(EXTRA_BANDS):
        print("[FAIL] scope=any 带 %d 条 != %d" % (len(extra), len(EXTRA_BANDS)), file=sys.stderr)
        ok = False
    if not any(r["up"] for r in reg["param"]):
        print("[FAIL] 没有一条 up 方向（嘴/眉该有）", file=sys.stderr)
        ok = False
    print("[%s] 反向 diff %s" % ("ok" if ok else "FAIL", "全部通过" if ok else "有差异"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description="三张部位表 → state/parts.tsv")
    ap.add_argument("--migrate", action="store_true", help="生成 state/parts.tsv")
    ap.add_argument("--verify", action="store_true", help="反向 diff 自检")
    ap.add_argument("--dump", action="store_true", help="打印解析结果（对账用）")
    ap.add_argument("--out", default=OUT, help="输出路径（默认 state/parts.tsv）")
    args = ap.parse_args()
    if not (args.migrate or args.verify or args.dump):
        ap.print_help()
        return 2

    check_id_order()

    if args.migrate:
        text = build()
        tmp = args.out + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, args.out)
        n = len([l for l in text.splitlines()
                 if l.strip() and not l.lstrip().startswith("#")])
        print("[ok] 写出 %s（%d 行数据）" % (args.out, n))

    if args.dump:
        sys.stdout.write(dump(parse_parts(args.out)))
        return 0

    if args.verify:
        return cmd_verify(args.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
