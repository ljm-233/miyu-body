#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""物理图：把 大肥鱼.physics3.json 的 131 条 PhysicsSetting 串成一张有向图。

为什么要有这一步（第二十二节）：

    以前点胸口是往 `ChestX1/ChestY1` 里写数。可这些参数在 physics3.json 里是
    **Output.Destination** —— 物理算完写进去的结果。手写等于把物理的输出顶掉：
    网格被直接拉过去，形状就发尖、发假。

    正确的口子是 **Input.Source**：那些只被输入、从不被输出的参数（根），
    拨一下，物理自己会把胸腔带起来，还带着延迟、惯性、回弹。

所以这个脚本产出的是「拨哪几个」的清单，不是「写哪几个」。

产出
    state/physics-map.json  机器读：nodes / edges / roots / leaves / 每个部位的根
    state/physics-map.md    人看的：统计 + 每个部位的根输入清单

用法
    tools/physics-map.py
    tools/physics-map.py --json state/physics-map.json --md state/physics-map.md
"""

import argparse
import json
import os
import re
import sys
from collections import defaultdict, deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODEL = os.environ.get("MIYU_MODEL_DIR",
                       os.path.join(os.path.dirname(ROOT), "model"))
PHYS = os.path.join(MODEL, "大肥鱼.physics3.json")
CDI3 = os.path.join(MODEL, "大肥鱼.cdi3.json")

# ── 参数 → 部位 ────────────────────────────────────────────────────────
# 两路证据：① cdi3 里的**分组名**（作者自己分的：胸 / 裙子 / 手臂 / 尾巴…）
#           ② 参数名里的词（英文 Id + 中文显示名）
# 名字优先于分组：`LLaceX1` 在「上衣」组里，但它其实是袖子上的花边。
PART_ORDER = ["head", "chest", "arm", "waist", "skirt", "leg", "tail"]

GROUP_PART = {
    "胸": "chest",
    "上衣": "chest",
    "裙子": "skirt",
    "手臂": "arm",
    "兽耳": "head",
    "前发物理": "head",
    "侧发物理": "head",
    "后发物理": "head",
    "五官物理": "head",
    "头配饰": "head",
    "眼睛": "head",
    "嘴": "head",
    "眼睛动画": "head",
    "尾巴": "tail",
    "身体轴": "leg",       # 盆骨 / 双腿 / 并腿 —— 下半身
    "闪光": "head",
    "动画参数": "tail",    # 草物理（尾巴尖上那丛）+ 动画开关
    "VB": "head",
    "开关": "head",
    "待机动画": "other",
    "": "other",
}

# 名字规则：先长后短，命中第一个赢
NAME_RULES = [
    (r"Chest|胸", "chest"),
    (r"Skirt|裙|Lace|蕾丝|缎带|Bow|蝴蝶结|Ribbon", "skirt"),
    (r"LEG|Leg|腿|Hip|盆骨|Knee|Foot|脚", "leg"),
    (r"Tail|尾|Ponytail|Rotation8[4-9]|草", "tail"),
    (r"Arm|Forearm|Wrist|Hand|Sleeve|Shoulder|手臂|小臂|手腕|手|袖|Flip|Control", "arm"),
    (r"Hair|发", "head"),
    (r"Eye|Brow|Mouth|Cheek|Tongue|Jaw|Lip|Face|眼|眉|嘴|舌|下巴|脸|颊|Pout|Press|Smile|Emotion|Shock", "head"),
    (r"Hat|Ear|Shine|Kira|Gem|Glow|HighLight|耳|帽|闪光|宝石|光环", "head"),
    (r"Waist|腰", "waist"),
]


def load_json(p):
    with open(p, "rb") as f:
        return json.loads(f.read().decode("utf-8-sig"))


def cdi3_index():
    """参数 id → (显示名, 分组名)"""
    try:
        d = load_json(CDI3)
    except Exception as e:                                # pragma: no cover
        print("读不到 cdi3：%s" % e, file=sys.stderr)
        return {}
    gname = {}
    for g in d.get("ParameterGroups", []):
        gname[g["Id"]] = g.get("Name", "") or ""
    out = {}
    for p in d.get("Parameters", []):
        out[p["Id"]] = (p.get("Name", "") or "", gname.get(p.get("GroupId", ""), ""))
    return out


def part_of(pid, cdi):
    name, group = cdi.get(pid, ("", ""))
    hay = "%s %s" % (pid, name)
    for pat, part in NAME_RULES:
        if re.search(pat, hay):
            return part
    return GROUP_PART.get(group, "other")


def build(phys):
    settings = phys["PhysicsSettings"]
    edges = []           # (src, dst, settingId, weight, scale)
    out_deg = defaultdict(int)   # 这个参数**驱动**了几条 setting
    in_deg = defaultdict(int)    # 这个参数被几条 setting **写**
    settings_of = defaultdict(list)
    for s in settings:
        sid = s.get("Id", "?")
        ins = s.get("Input", []) or []
        outs = s.get("Output", []) or []
        for i in ins:
            src = i.get("Source", {}).get("Id")
            if not src:
                continue
            out_deg[src] += 1
            settings_of[src].append(sid)
            for o in outs:
                dst = o.get("Destination", {}).get("Id")
                if not dst:
                    continue
                edges.append((src, dst, sid, i.get("Weight", 100), o.get("Scale", 1)))
        for o in outs:
            dst = o.get("Destination", {}).get("Id")
            if dst:
                in_deg[dst] += 1
                settings_of[dst].append(sid)
    return settings, edges, in_deg, out_deg, settings_of


def adjacency(edges):
    adj = defaultdict(set)
    for src, dst, *_ in edges:
        adj[src].add(dst)
    return adj


def scc_nodes(adj, nodes):
    """Tarjan，用来找环（物理里确实有反馈环，不能假定是 DAG）"""
    index, low, on, st, comp = {}, {}, {}, [], []
    counter = [0]

    def strong(v):
        work = [(v, 0)]
        while work:
            node, pi = work.pop()
            if pi == 0:
                index[node] = low[node] = counter[0]
                counter[0] += 1
                st.append(node)
                on[node] = True
            recurse = False
            nbrs = sorted(adj.get(node, ()))
            for i in range(pi, len(nbrs)):
                w = nbrs[i]
                if w not in index:
                    work.append((node, i + 1))
                    work.append((w, 0))
                    recurse = True
                    break
                elif on.get(w):
                    low[node] = min(low[node], index[w])
            if recurse:
                continue
            if low[node] == index[node]:
                c = []
                while True:
                    w = st.pop()
                    on[w] = False
                    c.append(w)
                    if w == node:
                        break
                if len(c) > 1:
                    comp.append(sorted(c))
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])

    for v in sorted(nodes):
        if v not in index:
            strong(v)
    return comp


def longest_chain(adj, roots):
    """从任一节点出发的最长简单路径（按节点数算层数）。环上的边跳过。"""
    best = {"len": 0, "path": []}
    memo = {}

    def walk(v, depth, seen_path, onstack):
        memo_key = v
        if v in memo:
            l, p = memo[memo_key]
            return l, p
        best_len, best_path = 0, []
        for w in adj.get(v, ()):
            if w in onstack:
                continue
            l, p = walk(w, depth + 1, None, onstack | {w})
            if l + 1 > best_len:
                best_len, best_path = l + 1, [w] + p
        memo[memo_key] = (best_len, best_path)
        return best_len, best_path

    for r in roots:
        l, p = walk(r, 0, None, {r})
        if l > best["len"]:
            best = {"len": l, "path": [r] + p}
    return best


def hops_to(adj, root, targets):
    """从 root 出发，最少几跳碰到 targets 里的节点；碰不到返回 None。"""
    if not targets:
        return None
    seen = {root}
    q = deque([(root, 0)])
    while q:
        n, d = q.popleft()
        for w in adj.get(n, ()):
            if w in seen:
                continue
            if w in targets:
                return d + 1
            seen.add(w)
            q.append((w, d + 1))
    return None


def reach_set(adj, root):
    seen, q = set(), deque([root])
    while q:
        n = q.popleft()
        for w in adj.get(n, ()):
            if w not in seen:
                seen.add(w)
                q.append(w)
    return seen


def ancestors(adj, part_leaves):
    """反图 BFS：哪些参数（递归地）能传到这些叶子"""
    rev = defaultdict(set)
    for a, bs in adj.items():
        for b in bs:
            rev[b].add(a)
    seen, q = set(), deque(part_leaves)
    while q:
        n = q.popleft()
        for p in rev.get(n, ()):
            if p not in seen:
                seen.add(p)
                q.append(p)
    return seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json", default=os.path.join(ROOT, "state", "physics-map.json"))
    ap.add_argument("--md", default=os.path.join(ROOT, "state", "physics-map.md"))
    args = ap.parse_args()

    phys = load_json(PHYS)
    cdi = cdi3_index()
    settings, edges, in_deg, out_deg, settings_of = build(phys)
    adj = adjacency(edges)

    all_nodes = set(in_deg) | set(out_deg)
    roots = sorted(n for n in all_nodes if in_deg.get(n, 0) == 0 and out_deg.get(n, 0) > 0)
    leaves = sorted(n for n in all_nodes if out_deg.get(n, 0) == 0 and in_deg.get(n, 0) > 0)
    both = sorted(n for n in all_nodes if in_deg.get(n, 0) > 0 and out_deg.get(n, 0) > 0)

    part = {n: part_of(n, cdi) for n in all_nodes}
    leaf_by_part = defaultdict(list)
    for n in leaves:
        leaf_by_part[part[n]].append(n)
    leaf_set = set(leaves)

    # 每个部位：能传到它叶子的根（= 拨这几个，这块会动）
    part_roots = {}
    for p in PART_ORDER + ["other"]:
        lv = leaf_by_part.get(p, [])
        if not lv:
            continue
        anc = ancestors(adj, lv)
        ry = sorted(n for n in anc if n in roots)
        rows = []
        for r in ry:
            got = reach_set(adj, r)
            share = {}
            for n in got:
                if n in leaf_set:
                    q = part[n]
                    share[q] = share.get(q, 0) + 1
            tot = sum(share.values()) or 1
            rows.append({
                "id": r,
                "name": cdi.get(r, ("", ""))[0],
                "hops": hops_to(adj, r, set(lv)),          # 几跳摸到这块的叶子
                "own_leaves": sum(1 for n in got if part[n] == p),
                "leaf_share": round(share.get(p, 0) / tot, 3),   # 它的影响有多大比例落在这块
                "settings": sorted(set(settings_of.get(r, [])))[:6],
            })
        # 最直接的根排前面：跳数少的优先，其次「影响落在本块的比例」高的优先
        rows.sort(key=lambda d: (d["hops"] if d["hops"] is not None else 99, -d["leaf_share"]))
        part_roots[p] = {"leaves": len(lv), "roots": rows}

    cycles = scc_nodes(adj, all_nodes)
    chain = longest_chain(adj, roots)

    # ── 写 JSON ────────────────────────────────────────────────────────
    data = {
        "source": PHYS,
        "settings": len(settings),
        "nodes": len(all_nodes),
        "edges": len(edges),
        "roots": roots,
        "leaves": leaves,
        "intermediate": both,
        "cyclic_components": cycles,
        "longest_chain_layers": chain["len"] + 1,
        "longest_chain": chain["path"],
        "part_roots": part_roots,
        "part_of": part,
        "adjacency": {k: sorted(v) for k, v in sorted(adj.items())},
        "edge_detail": [{"from": a, "to": b, "setting": s, "weight": w, "scale": sc}
                        for a, b, s, w, sc in edges],
    }
    os.makedirs(os.path.dirname(args.json), exist_ok=True)
    with open(args.json, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")

    # ── 写 MD ─────────────────────────────────────────────────────────
    L = []
    A = L.append
    A("# 物理图（tools/physics-map.py 生成）\n")
    A("源：`%s`\n" % PHYS)
    A("| 项 | 数 |")
    A("|---|---|")
    A("| PhysicsSetting | %d |" % len(settings))
    A("| 参数节点 | %d |" % len(all_nodes))
    A("| 边（输入→输出） | %d |" % len(edges))
    A("| 根（只被输入，从不被输出） | %d |" % len(roots))
    A("| 叶子（只被输出，从不被输入） | %d |" % len(leaves))
    A("| 中间节点（两头都占） | %d |" % len(both))
    A("| 反馈环（强连通分量 >1） | %d |" % len(cycles))
    A("| 最长传导链 | %d 层 |" % (chain["len"] + 1))
    A("")
    A("## 根：可以拨的（%d）\n" % len(roots))
    A("| 参数 | 显示名 | 部位 | 驱动几条 setting |")
    A("|---|---|---|---|")
    for r in roots:
        A("| `%s` | %s | %s | %d |" % (r, cdi.get(r, ("", ""))[0] or "—", part[r], out_deg[r]))
    A("")
    A("## 每个部位的根输入\n")
    A("「拨这几个，这块就会自己动起来」—— 从叶子反推祖先，只留根。\n")
    for p in PART_ORDER + ["other"]:
        if p not in part_roots:
            continue
        d = part_roots[p]
        A("### %s（叶子 %d 个，根 %d 个）\n" % (p, d["leaves"], len(d["roots"])))
        if not d["roots"]:
            A("（没有可拨的根：这块完全由别的部位带动）\n")
            continue
        A("| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |")
        A("|---|---|---|---|")
        for r in d["roots"]:
            A("| `%s` | %s | %s | %.0f%% |" % (r["id"], r["name"] or "—",
              r["hops"] if r["hops"] is not None else "—", 100 * r["leaf_share"]))
        A("")
    A("## 最长传导链（%d 层）\n" % (chain["len"] + 1))
    A(" → ".join("`%s`" % n for n in chain["path"]))
    A("")
    if cycles:
        A("## 反馈环\n")
        for c in cycles:
            A("- " + " → ".join("`%s`" % n for n in c))
        A("")
    with open(args.md, "w", encoding="utf-8") as f:
        f.write("\n".join(L) + "\n")

    print("settings=%d nodes=%d edges=%d roots=%d leaves=%d mid=%d cycles=%d chain=%d"
          % (len(settings), len(all_nodes), len(edges), len(roots), len(leaves),
             len(both), len(cycles), chain["len"] + 1))
    print("roots: " + ", ".join(roots))
    for p in PART_ORDER:
        if p in part_roots:
            rs = [r["id"] for r in part_roots[p]["roots"]]
            print("  %-6s leaves=%3d roots=%s" % (p, part_roots[p]["leaves"], ", ".join(rs) or "(无)"))
    print("写出 %s / %s" % (args.json, args.md))


if __name__ == "__main__":
    main()
