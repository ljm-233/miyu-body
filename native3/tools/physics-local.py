#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给模型加「局部输入」（NOTES 二十三 → 二十四）。

问题：physics3.json 里原生的 setting 的输入全是**整体**参数（AngleX/Y/Z、
PositionX/Y/Z、ParamBodyAngleX、HipAngleX…）。点胸 → 拨 ParamBodyAngleX →
尾巴（20k px）比胸（8k px）动得还多，所以「点哪儿只动哪儿」量出来 0.46×。

做法：给每个部位加一条**只驱动自己那块**的 physics setting：
  Input  = 原 setting 的全部输入（照抄，权重一个不改）+ 一条 Poke*（-1..1，默认 0）
  Output = 原 setting 的全部 Destination（Scale 照抄，Weight 100）
后加的那条按 SDK 的顺序求值**覆盖**同名 Destination；Poke*=0 时两条链的输入
逐位相同、输出也逐位相同，所以原有行为（待机/拖动/身体角度带胸）一点没变，
Poke*≠0 时才多出一份**只落在这块**的位移。

参数 Id 从哪来：Cubism 的参数**只在 moc3 里定义**。physics3.json 写一个 moc3 没有的
Id，CubismModel::GetParameterIndex 不会返回 -1 而是返回「模型参数数 + 虚拟序号」
（Framework/src/Model/CubismModel.cpp:172 的 _notExistParameterId），物理层随后拿这个
序号去读 Core 的 parameterValues[] —— 越界读，值是 0，新 setting 永远是死的
（实测：PokeChest[509]，胸 0.18×）。
所以本源做法是**改名**：moc3 里参数 Id 是每格 64 字节、NUL 补齐的定长表
（这份模型从 0x38CE8 起 509 格），把**没有 key、没有 exp3 引用、没进 physics** 的
分隔符参数的 Id 原地改写成 Poke*/，范围（-1..1，默认 0）和索引都不动。
改之前备份 moc3，tools/physics-revert.sh 会一起换回去。

二十四（2026-11-10 下午）新加七个：脸拆成眼睛 / 嘴 / 眉毛，另加兽耳（左右各一）、
腰、头饰。脸和兽耳都是「一条 Poke 带两条链」（左右各一条，输出各管一边）。

用法：
    tools/physics-local.py            # 看一眼现在是什么状态（只读）
    tools/physics-local.py --apply    # 写入四个文件
"""

import argparse
import json
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = os.path.join(ROOT, "..", "model")
PHYS = os.path.join(MODEL, "大肥鱼.physics3.json")
MOC3 = os.path.join(MODEL, "大肥鱼.moc3")
CDI = os.path.join(MODEL, "大肥鱼.cdi3.json")
PARTS = os.path.join(ROOT, "state", "param-parts.tsv")
MANIFEST = os.path.join(ROOT, "state", "physics-local.manifest")

POKE_GROUP = "ParamGroupPoke"
POKE_GROUP_NAME = "戳动"

# ── 一、借来当载体的空转参数（moc3 里原地改名）──────────────────────────────
# 选的条件（二十四里重新核过一遍）：**没被 key 引用、没进 exp3、没进 physics**，
# 而且范围是 -1..1、默认 0。Divider1/Divider3 是 0..1（单极），不能用来表示「往两边戳」，
# 所以跳过。核对的脚本见 NOTES 第二十四节（19 个合格，这里用掉 7 个）。
SLOTS = [
    dict(name="PokeEye",      slot="Divider4",  cname="眼睛戳动"),
    dict(name="PokeMouth",    slot="Divider5",  cname="嘴戳动"),
    dict(name="PokeBrow",     slot="Divider8",  cname="眉毛戳动"),
    dict(name="PokeEarL",     slot="Divider31", cname="左兽耳戳动"),
    dict(name="PokeEarR",     slot="Divider6",  cname="右兽耳戳动"),
    dict(name="PokeWaist",    slot="Divider7",  cname="腰戳动"),
    dict(name="PokeHeadWear", slot="Divider15", cname="头饰戳动"),
]

# ── 二、每个 Poke 带哪几条链 ────────────────────────────────────────────────
# base   = 照抄哪条原生 setting（输入输出全抄，只多一条 Poke）
# weight = Poke 那条输入的权重（归一化 Angle 是 ±57.3°，100 就是满量程）
#          脸/头饰给小一点：这几块本来就小，满量程会拽变形（二十三的胸就是这么调的）
# scale_mul = 输出 Scale 的倍率（不动静止姿态 —— 静止时这条链输出就是 0）
# template/outputs = 「没有原生 setting 可抄」时自己拼一条（嘴就是这种：
#          ParamMouthOpenY 是根，没有任何 setting 输出它，只能自己写一条）
CHAINS = [
    # 脸 · 眼睛：左右各一条（24 管右眼珠、25 管左眼珠），加一条 PokeEye。
    # 眨眼那一下不在这儿 —— 物理只能摆瞳孔，睁眼闭眼由 src/main.cpp 的包络直接给。
    dict(part="eye", poke="PokeEye", base="PhysicsSetting24", cname="眼睛戳动(右眼珠)",
         weight=100, reflect=True, scale_mul=1.0),
    dict(part="eye", poke="PokeEye", base="PhysicsSetting25", cname="眼睛戳动(左眼珠)",
         weight=100, reflect=False, scale_mul=1.0),
    # 脸 · 嘴：**不进物理**。原因写在这儿，省得下一个人再踩：
    # main.cpp 写说话口型那一段（physics 之后）每帧无条件 SetParamMouthOpenY ——
    # 物理算出来的值当场被盖掉，链子写什么都没有用（实测两个方向都是 0 px）。
    # 所以嘴是「参数在 moc3、幅度在 C++ 桥」：partPokeTick 之后读 PokeMouth，
    # 把 |v|×0.85 叠到 ParamMouthOpenY 上（见 src/main.cpp 的 mouthPokeTick）。
    # 脸 · 眉毛：抄 36/37（眉上下缓动链，输出 ParamBrowRY2 / ParamBrowLY2）。
    # 为什么不用 34/35（输出 ParamBrowLY3/RY3）：那个参数**同时带动上眼睑**，
    # 实测它的变化区域是 v .198..0.228 —— 一路盖进眼睛那一条，眉毛自己的格子里只剩
    # 一点点（量出来 0.00×）。36/37 这两个是纯眉线：实测
    #   ParamBrowLY2 25 px，v .196..0.208，u .522..0.534
    #   ParamBrowRY2 同上镜像，v .192..0.209
    # 像素少（眉毛在画面上就那么细），但干净：别的部位一动不动。
    dict(part="brow", poke="PokeBrow", base="PhysicsSetting37", cname="眉毛戳动(左)",
         weight=100, reflect=True, scale_mul=1.0),
    dict(part="brow", poke="PokeBrow", base="PhysicsSetting36", cname="眉毛戳动(右)",
         weight=100, reflect=False, scale_mul=1.0),
    # 兽耳：83 = 左耳（LControl + AngleX/Y/Z + 呼吸 + 右眼开闭 → LEar1/2/3）、
    #        85 = 右耳。原链的输入一条不改，只多一条 Poke。
    # 只留 LEar2/LEar3 两个输出（耳中段和耳尖）—— 量过：LEar1 单独拨满量程只有
    # 1.29×（它的动带动了发饰那一片），LEar2 2.74×、LEar3 3.65×。三个一起写会把
    # 掉进头饰格子的像素也一起放大（实测 0.62×）。
    dict(part="ear", poke="PokeEarL", base="PhysicsSetting83", cname="左兽耳戳动",
         weight=80, reflect=False, scale_mul=1.0,
         outputs=[dict(id="LEar2", vertex=2, scale=1.0, type="Angle", reflect=False),
                  dict(id="LEar3", vertex=3, scale=1.0, type="Angle", reflect=False)]),
    dict(part="ear", poke="PokeEarR", base="PhysicsSetting85", cname="右兽耳戳动",
         weight=80, reflect=True, scale_mul=1.0,
         outputs=[dict(id="REar2", vertex=2, scale=1.0, type="Angle", reflect=False),
                  dict(id="REar3", vertex=3, scale=1.0, type="Angle", reflect=False)]),
    # 头饰（Part161 女仆发饰，不是帽子 —— 作者把这条链命名成「帽子 X」，它动的
    # 就是发饰）。117 = 帽子 X → Hat_XA/B/C，118 = 帽子 Y → Hat_YA/YB/YC。
    dict(part="headwear", poke="PokeHeadWear", base="PhysicsSetting117", cname="头饰戳动(X)",
         weight=55, reflect=False, scale_mul=1.0),
    # 118 的归一化角度只有 ±10（117 是 ±57.3），同样的权重只走出 1/6 的位移，
    # 所以这里权重给满、Scale 再乘 6 补回来。
    dict(part="headwear", poke="PokeHeadWear", base="PhysicsSetting118", cname="头饰戳动(Y)",
         weight=100, reflect=True, scale_mul=1.0, reps=6),
    # 腰：抄「裙子层级 Y」（PhysicsSetting71：HipAngleY → SkirtLayerY1..5，五层裙片）。
    # 但只保留 **SkirtLayerY2** 这一个输出 —— 这是量出来的：SkirtLayerY2 单独拨到
    # 满量程时，腰带那一圈 4551 px、裙子其它地方只有 559 px（8.1×）；
    # 而 Y1 只有 1.9×、Y3 3.2×，五个一起写就把裙子搅浑了。
    # 注意 base 仍然照抄 71 的输入：Poke=0 时这条链算出来的值和 71 一模一样，
    # 所以原来那点随呼吸/姿势的层级摆动一点没丢。
    dict(part="waist", poke="PokeWaist", base="PhysicsSetting71", cname="腰戳动",
         weight=100, reflect=False, scale_mul=1.0, reps=5,
         # reps=5：71 的归一化角度只有 ±10°（不是 ±57.3），一条输入只走出 0.18 rad，
         # 落到 SkirtLayerY2 上不到 0.2；重复 5 条 = 50°，才到 0.87 那个「看得见但不崩」
         # 的量级。用 reps 而不是乘 Scale，是为了 Poke=0 时输出和 71 逐位相同。
         outputs=[dict(id="SkirtLayerY2", vertex=2, scale=1.0, type="Angle", reflect=False)]),
]

# 新 setting 的 Id 从 138 起（132–137 是二十三那六个，不许碰）
FIRST_ID = 138
NEW_IDS = ["PhysicsSetting%d" % (FIRST_ID + i) for i in range(len(CHAINS))]

# 部位表里每行一个 P：部位 → 用哪条 Poke、比值、幅度
PART_ROWS = [
    ("eye",      "PokeEye",      95, 1.000, "眼睛：眼珠被物理带走 + 点一下眨一次（0.24s 包络）"),
    ("mouth",    "PokeMouth",    94, 1.000, "嘴：ParamMouthOpenY 自己拼的一条链"),
    ("brow",     "PokeBrow",     93, 1.000, "眉毛：34/35 两条链，上下 + 角度"),
    ("ear",      "PokeEarL",     91, 1.000, "左兽耳：PhysicsSetting83 的局部版"),
    ("ear",      "PokeEarR",     91, 1.000, "右兽耳：PhysicsSetting85 的局部版"),
    ("waist",    "PokeWaist",    90, 1.000, "腰：腰带那一圈的局部版"),
    ("headwear", "PokeHeadWear", 90, 1.000, "头饰（女仆发饰，不是帽子）：117/118 两条链"),
]


def load():
    return json.load(open(PHYS, encoding="utf-8")), json.load(open(CDI, encoding="utf-8"))


def patch_moc3():
    """把 moc3 定长参数 Id 表里的槽位改名（幂等）。返回改了几格。"""
    d = bytearray(open(MOC3, "rb").read())
    changed = 0
    anchor = d.find(b"Paramshuiyin7")
    if anchor < 0:
        sys.exit("moc3 里找不到参数 Id 表锚点 Paramshuiyin7")
    for d0 in SLOTS:
        old, new = d0["slot"].encode(), d0["name"].encode()
        assert len(new) < 64
        off = d.find(old + b"\0")
        while off >= 0 and (off - anchor) % 64 != 0:
            off = d.find(old + b"\0", off + 1)
        if off < 0:
            if d.find(new + b"\0") >= 0:
                continue                      # 已经改过
            sys.exit("moc3 里找不到 %s 的格子" % d0["slot"])
        d[off:off + 64] = new + b"\0" * (64 - len(new))
        changed += 1
    if changed:
        open(MOC3, "wb").write(bytes(d))
    return changed


def _ours(sid):
    """编号 ≥ 138 的都算这一层加的（132–137 是二十三那六个，不许碰）。
    按编号而不是按 NEW_IDS 列表：CHAINS 条数改了以后，列表法会留下孤儿 setting。"""
    try:
        return int(sid.replace("PhysicsSetting", "")) >= FIRST_ID
    except ValueError:
        return False


def strip(ph):
    """把上一轮加的 setting 全摘掉（幂等）。"""
    ph["PhysicsSettings"] = [s for s in ph["PhysicsSettings"] if not _ours(s["Id"])]
    ph["Meta"]["PhysicsDictionary"] = [
        d for d in ph["Meta"]["PhysicsDictionary"] if not _ours(d["Id"])]
    return ph


def recount(ph):
    ph["Meta"]["PhysicsSettingCount"] = len(ph["PhysicsSettings"])
    ph["Meta"]["TotalInputCount"] = sum(len(s["Input"]) for s in ph["PhysicsSettings"])
    ph["Meta"]["TotalOutputCount"] = sum(len(s["Output"]) for s in ph["PhysicsSettings"])
    ph["Meta"]["VertexCount"] = sum(len(s["Vertices"]) for s in ph["PhysicsSettings"])
    return ph


def build(ph):
    by_id = {s["Id"]: s for s in ph["PhysicsSettings"]}
    for new_id, c in zip(NEW_IDS, CHAINS):
        src = c.get("base") or c.get("template")
        tpl = by_id.get(src)
        if tpl is None:
            sys.exit("找不到基础 setting %s" % src)
        s = json.loads(json.dumps(tpl))            # 深拷贝
        s["Id"] = new_id
        # 带 base 的：输入照抄 base 全部输入；不带 base 的（template）：输入从这里开始
        s["Input"] = list(s["Input"]) if c.get("base") else []
        # reps：同一条 Poke 输入写几次。物理把每条 Angle 输入**加**到 totalAngle 上，
        # 所以重复 n 次 = 摆幅 ×n。为什么不用「输出 Scale 乘 n」——那会把原有的输入
        # 也一起放大（Poke=0 时输出就不是原值了，等于改了作者写的物理）。
        for _ in range(c.get("reps", 1)):
            s["Input"].append({
                "Source": {"Target": "Parameter", "Id": c["poke"]},
                "Weight": c["weight"],
                "Type": "Angle",
                "Reflect": c["reflect"],
            })
        if c.get("outputs"):
            s["Output"] = [{"Destination": {"Target": "Parameter", "Id": o["id"]},
                            "VertexIndex": o.get("vertex", 1),
                            "Scale": o["scale"],
                            "Weight": 100,
                            "Type": o.get("type", "Angle"),
                            "Reflect": o.get("reflect", False)} for o in c["outputs"]]
        # outputs 里写了 Scale 的就不再多乘一次 scale_mul（两个都算会变成 ×2.25 这种）
        mul = 1.0 if c.get("outputs") else c.get("scale_mul", 1.0)
        if mul != 1.0:
            for o in s["Output"]:
                o["Scale"] = round(o["Scale"] * mul, 4)
        ph["PhysicsSettings"].append(s)
        ph["Meta"]["PhysicsDictionary"].append({"Id": new_id, "Name": c["cname"]})
    return ph


def patch_cdi(cdi):
    """参数分组：建一个「戳动」组，所有 Poke* 都挂进去。"""
    if not any(g.get("Id") == POKE_GROUP for g in cdi["ParameterGroups"]):
        cdi["ParameterGroups"].append({"Id": POKE_GROUP, "GroupId": "", "Name": POKE_GROUP_NAME})
    return _patch_cdi_params(cdi)


def _patch_cdi_params(cdi):
    """Id 跟着 moc3 一起改（cdi3 的 Id 必须和 moc3 对得上，否则中文名挂空）。"""
    want = {}
    for d in SLOTS:
        want[d["slot"]] = d
        want[d["name"]] = d
    for p in cdi["Parameters"]:
        d = want.get(p["Id"])
        if not d:
            continue
        p["Id"] = d["name"]
        p["Name"] = d["cname"]
        p["GroupId"] = POKE_GROUP
    n = len({p["Id"] for p in cdi["Parameters"]} & {d["name"] for d in SLOTS})
    if n != len(SLOTS):
        sys.exit("cdi3 里只找到 %d/%d 个槽位参数" % (n, len(SLOTS)))
    return cdi


def patch_parts():
    """部位表：新部位补 P 行（幂等：先摘掉自己写的、再补一次）。"""
    want = {}
    for part, poke, score, amp, note in PART_ROWS:
        want.setdefault(part, []).append((poke, score, amp, note))
    lines = open(PARTS, encoding="utf-8").read().splitlines()
    out, emitted = [], set()
    for ln in lines:
        if ln.startswith("P\t"):
            c = ln.split("\t")
            if c[1] in want:
                if c[1] in emitted:
                    continue
                emitted.add(c[1])
                for poke, score, amp, note in want[c[1]]:
                    out.append("\t".join(["P", c[1], poke, str(score), "%.3f" % amp, note]))
                continue
        out.append(ln)
    # 表里原来没有的部位（眼/嘴/眉/耳/腰/头饰）直接追加到末尾
    for part in want:
        if part in emitted:
            continue
        for poke, score, amp, note in want[part]:
            out.append("\t".join(["P", part, poke, str(score), "%.3f" % amp, note]))
    open(PARTS, "w", encoding="utf-8").write("\n".join(out) + "\n")


def report(ph):
    by_id = {s["Id"]: s for s in ph["PhysicsSettings"]}
    for new_id, c in zip(NEW_IDS, CHAINS):
        s = by_id.get(new_id)
        if not s:
            print("  %-22s 缺（没上）" % new_id)
            continue
        ins = ", ".join("%s×%d%s" % (i["Source"]["Id"], i["Weight"],
                                     "(inv)" if i.get("Reflect") else "") for i in s["Input"])
        outs = ", ".join("%s×%g" % (o["Destination"]["Id"], o["Scale"]) for o in s["Output"])
        print("  %-12s %s ← %s" % (c["part"], new_id, ins))
        print("               → %s" % outs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    ph, cdi = load()
    if not args.apply:
        print("当前：%d 条 setting（Meta 说 %d）" % (len(ph["PhysicsSettings"]),
                                                ph["Meta"]["PhysicsSettingCount"]))
        report(ph)
        return

    # 备份只做一次：manifest 记着「改之前的原件」，重复 --apply 不会把改过的文件再备份
    # 成「原件」（那样 physics-revert.sh 就会换回一个改过的版本，等于没回滚）。
    if os.path.exists(MANIFEST) and all(
            os.path.exists(l.rstrip("\n").split("\t")[1])
            for l in open(MANIFEST, encoding="utf-8") if "\t" in l):
        print("备份沿用 %s" % os.path.basename(MANIFEST))
    else:
        stamp = __import__("time").strftime("%H%M")
        rows = []
        for f in (PHYS, CDI, MOC3, PARTS):
            bak = "%s.bak-%s" % (f, stamp)
            shutil.copyfile(f, bak)
            rows.append("%s\t%s" % (f, bak))
            print("备份 %s" % os.path.basename(bak))
        open(MANIFEST, "w", encoding="utf-8").write("\n".join(rows) + "\n")

    ph = recount(build(strip(ph)))
    nmoc = patch_moc3()
    patch_cdi(cdi)
    json.dump(ph, open(PHYS, "w", encoding="utf-8"), ensure_ascii=False, indent="\t")
    json.dump(cdi, open(CDI, "w", encoding="utf-8"), ensure_ascii=False, indent="\t")
    patch_parts()

    print("moc3 改名 %d 格" % nmoc)
    print("写好：%d 条 setting / %d 输入 / %d 输出 / %d 顶点"
          % (ph["Meta"]["PhysicsSettingCount"], ph["Meta"]["TotalInputCount"],
             ph["Meta"]["TotalOutputCount"], ph["Meta"]["VertexCount"]))
    report(ph)


if __name__ == "__main__":
    main()
