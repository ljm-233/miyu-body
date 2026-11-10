#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""给模型加「局部输入」（NOTES 二十三）。

问题：physics3.json 里 131 条 setting 的输入全是**整体**参数（AngleX/Y/Z、
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
（这份模型从 0x38CE8 起 509 格），把四个**没有 key、没有 exp3 引用、没进 physics**
的分隔符参数的 Id 原地改写成 Poke*/，范围（-1..1，默认 0）和索引都不动。
改之前备份 moc3，tools/physics-revert.sh 会一起换回去。

用法：
    tools/physics-local.py            # 看一眼现在是什么状态（只读）
    tools/physics-local.py --apply    # 写入三个文件
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

# 部位 → 新参数。slot 是 moc3 里借来当载体的空转参数 Id；base 是要照抄的那条 setting。
# weight = 新输入在 setting 里的权重（100 = 归一化后的满量程 10）；
#   chest 100 ≈ ParamBodyAngleX 满量程（80 权重 × 归一化 10 ÷ ... 实际给的是 8）；
#   arm/skirt 给 60 是量出来「比原来的摆幅小一点、不抢戏」的位置。
LOCAL = [
    dict(part="chest",  name="PokeChest", slot="Divider18", cname="胸部戳动",
         group="ParamGroupPoke", base="PhysicsSetting64", weight=100, reflect=False,
         score=90, amp=1.0, scale_mul=2.0, note="胸X 的局部版；ChestX1/2/3/BS 同 Destination"),
    dict(part="arm",    name="PokeArmL",  slot="Divider9",  cname="左手戳动",
         group="ParamGroupPoke", base="PhysicsSetting72", weight=60, reflect=True,
         score=85, amp=1.0, scale_mul=1.5, note="左臂的局部版；LArm/LForearm/LWrist"),
    dict(part="arm",    name="PokeArmR",  slot="Divider10", cname="右手戳动",
         group="ParamGroupPoke", base="PhysicsSetting73", weight=60, reflect=False,
         score=84, amp=1.0, scale_mul=1.5, note="右臂的局部版；RArm/RForearm/RWrist"),
    dict(part="leg",    name="PokeLeg",   slot="Divider14", cname="腿戳动",
         group="ParamGroupPoke", base="PhysicsSetting17", weight=60, reflect=False,
         score=88, amp=1.0, note="双腿 Z 的局部版；LEGAngleZ2（原来拨 HipAngleX 会先拽裙）"),
    dict(part="tail",   name="PokeTail",  slot="Divider19", cname="尾巴戳动",
         group="ParamGroupPoke", base="PhysicsSetting130", weight=60, reflect=False,
         score=92, amp=1.0, note="尾巴的局部版；Param_Angle_Rotation84..89 + Param52"),
    dict(part="skirt",  name="PokeSkirt", slot="Divider20", cname="裙摆戳动",
         group="ParamGroupPoke", base="PhysicsSetting67", weight=60, reflect=False,
         score=80, amp=1.0, note="裙子X 摆动的局部版；SkirtX1..4/SkirtSpecial/2"),
]
NEW_IDS = ["PhysicsSetting%d" % (132 + i) for i in range(len(LOCAL))]
TAG = "Local"          # 新 setting 的 Name 后缀，删旧靠它认人


def load():
    return json.load(open(PHYS, encoding="utf-8")), json.load(open(CDI, encoding="utf-8"))


def patch_moc3():
    """把 moc3 定长参数 Id 表里的槽位改名（幂等）。返回改了几格。"""
    d = bytearray(open(MOC3, "rb").read())
    changed = 0
    for d0 in LOCAL:
        old, new = d0["slot"].encode(), d0["name"].encode()
        assert len(new) < 64
        # 找到 old 所在的 64 字节格子：从锚点（第一个参数 Id）按 64 对齐反推
        anchor = d.find(b"Paramshuiyin7")
        if anchor < 0:
            sys.exit("moc3 里找不到参数 Id 表锚点 Paramshuiyin7")
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


def strip(ph):
    """把上一轮加的 setting 全摘掉（幂等）。"""
    keep = []
    for s in ph["PhysicsSettings"]:
        if s["Id"] in NEW_IDS:
            continue
        keep.append(s)
    ph["PhysicsSettings"] = keep
    ph["Meta"]["PhysicsDictionary"] = [
        d for d in ph["Meta"]["PhysicsDictionary"] if d["Id"] not in NEW_IDS]
    return ph


def recount(ph):
    ph["Meta"]["PhysicsSettingCount"] = len(ph["PhysicsSettings"])
    ph["Meta"]["TotalInputCount"] = sum(len(s["Input"]) for s in ph["PhysicsSettings"])
    ph["Meta"]["TotalOutputCount"] = sum(len(s["Output"]) for s in ph["PhysicsSettings"])
    ph["Meta"]["VertexCount"] = sum(len(s["Vertices"]) for s in ph["PhysicsSettings"])
    return ph


def build(ph):
    by_id = {s["Id"]: s for s in ph["PhysicsSettings"]}
    for new_id, d in zip(NEW_IDS, LOCAL):
        base = by_id.get(d["base"])
        if base is None:
            sys.exit("找不到基础 setting %s" % d["base"])
        s = json.loads(json.dumps(base))          # 深拷贝
        s["Id"] = new_id
        s["Input"] = list(s["Input"]) + [{
            "Source": {"Target": "Parameter", "Id": d["name"]},
            "Weight": d["weight"],
            "Type": "X",
            "Reflect": d["reflect"],
        }]
        mul = d.get("scale_mul", 1.0)
        if mul != 1.0:
            for o in s["Output"]:
                o["Scale"] = round(o["Scale"] * mul, 4)
        ph["PhysicsSettings"].append(s)
        ph["Meta"]["PhysicsDictionary"].append({"Id": new_id, "Name": d["cname"]})
    return ph


def patch_cdi(cdi):
    """参数分组：建一个「戳动」组，五个 Poke* 都挂进去（局部输入是一套的东西）。"""
    if not any(g.get("Id") == "ParamGroupPoke" for g in cdi["ParameterGroups"]):
        cdi["ParameterGroups"].append({"Id": "ParamGroupPoke", "GroupId": "", "Name": "戳动"})
    return _patch_cdi_params(cdi)


def _patch_cdi_params(cdi):
    """Id 跟着 moc3 一起改（cdi3 的 Id 必须和 moc3 对得上，否则中文名挂空）。"""
    want = {}
    for d in LOCAL:
        want[d["slot"]] = d
        want[d["name"]] = d
    n = 0
    for p in cdi["Parameters"]:
        d = want.get(p["Id"])
        if not d:
            continue
        p["Id"] = d["name"]
        p["Name"] = d["cname"]
        p["GroupId"] = d["group"]
        n += 1
    n = len({p["Id"] for p in cdi["Parameters"]} & {d["name"] for d in LOCAL})
    if n != len(LOCAL):
        sys.exit("cdi3 里只找到 %d/%d 个槽位参数" % (n, len(LOCAL)))
    return cdi


def patch_parts():
    """部位行换成新参数（幂等：先摘掉自己写的 P 行和同部位的旧 P 行，再按部位补一次）。"""
    want = {}
    for d in LOCAL:
        want.setdefault(d["part"], []).append(d)
    names = {d["name"] for d in LOCAL}
    lines = open(PARTS, encoding="utf-8").read().splitlines()
    out, emitted = [], set()
    for ln in lines:
        if ln.startswith("P\t"):
            c = ln.split("\t")
            if c[1] in want:
                if c[1] in emitted:
                    continue
                emitted.add(c[1])
                for d in want[c[1]]:
                    out.append("\t".join(["P", c[1], d["name"], str(d["score"]),
                                           "%.3f" % d["amp"], d["note"]]))
                continue
        out.append(ln)
    missing = set(want) - emitted
    if missing:
        sys.exit("param-parts.tsv 里没找到这些部位的行：%s" % ", ".join(sorted(missing)))
    open(PARTS, "w", encoding="utf-8").write("\n".join(out) + "\n")


def report(ph, cdi):
    by_id = {s["Id"]: s for s in ph["PhysicsSettings"]}
    for new_id, d in zip(NEW_IDS, LOCAL):
        s = by_id.get(new_id)
        if not s:
            print("  %-14s 缺（没上）" % d["name"])
            continue
        ins = ", ".join("%s×%d%s" % (i["Source"]["Id"], i["Weight"],
                                     "(inv)" if i.get("Reflect") else "") for i in s["Input"])
        outs = ", ".join("%s×%g" % (o["Destination"]["Id"], o["Scale"]) for o in s["Output"])
        print("  %-10s %s ← %s" % (d["name"], s["Id"], ins))
        print("             → %s" % outs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    args = ap.parse_args()

    ph, cdi = load()
    if not args.apply:
        print("当前：%d 条 setting（Meta 说 %d）" % (len(ph["PhysicsSettings"]),
                                                ph["Meta"]["PhysicsSettingCount"]))
        report(ph, cdi)
        return

    # 备份只做一次：manifest 记着「改之前的原件」，重复 --apply 不会把改过的文件再备份
    # 成「原件」（那样 physics-revert.sh 就会换回一个改过的版本，等于没回滚）。
    if os.path.exists(MANIFEST) and all(
            os.path.exists(l.rstrip("\n").split("\t")[1]) for l in open(MANIFEST, encoding="utf-8") if "\t" in l):
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
    report(ph, cdi)


if __name__ == "__main__":
    main()
