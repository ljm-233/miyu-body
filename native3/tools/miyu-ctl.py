#!/usr/bin/env python3
"""米鱼的身体 —— 一条命令做一件事。

2026-11-11 01:1x 他问「现在是不是能很简单就可以控制身体」。
答案是：不算。此前要控制她得手写 JSON、记得 socket 路径、还要处理中文转义，
连播个动作都得背出「默认手切举手.motion3.json」这种全名。那不叫简单。

这个脚本就是把那些全收起来。命令叫 `my`（全名 `miyu-ctl`）：

    my say 有点累            说一句话（有缓存就出声）
    my say 有点累 点头        说话 + 配一个动作
    my walk 900 200          走过去
    my do 举手               播一个动作（举手/竖手指/狗尾草/伸展…）
    my face 前倾             换表情
    my mood                  看她此刻什么状态
    my list                  看有哪些动作和表情

动作名用简称就行，全名太长记不住。列表见 `my list`。
"""
import json
import os
import socket
import sys

SOCK = "/run/user/%d/miyu-body.sock" % os.getuid()
HER = os.path.expanduser("~/.dsh/miyu/body")
STATE = os.path.join(HER, "native3/state")
MOTION_DIR = os.path.join(HER, "model/Animation")
EXPR_DIR = os.path.join(HER, "model/Exp3")

# 动作简称 → 真实的 motion3 文件名
MOTIONS = {
    "举手": "默认手切举手.motion3.json",
    "放手": "举手切默认手.motion3.json",
    "竖手指": "竖手指切出来动画.motion3.json",
    "收手指": "竖手指切回默认.motion3.json",
    "狗尾草": "狗尾草切换出来.motion3.json",
    "收狗尾草": "狗尾草切回默认手.motion3.json",
    "伸展": "脱捕.motion3.json",
}

# 我们自己写的动作（plans/*.json，走 plan 通道，不是 motion）
PLANS = {"点头", "害羞", "招呼"}


def send(obj):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(SOCK)
        s.sendall(json.dumps(obj, ensure_ascii=False).encode())
        s.close()
        return True
    except (FileNotFoundError, ConnectionRefusedError):
        print("她的身体没在跑（%s 连不上）" % SOCK, file=sys.stderr)
        return False


def cur_mood():
    """从 state/mood.json 读她此刻的心情（mood-sync.py 每 30 秒写一次）。"""
    try:
        with open(os.path.join(STATE, "mood.json"), encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return None


def cmd_say(args):
    if not args:
        print("说点什么：my say 有点累", file=sys.stderr)
        return 1
    text = args[0]
    m = cur_mood()
    mood = int(m["mood"]) if m else 50
    ok = send({"say": text, "mood": mood})
    if ok and len(args) > 1:
        cmd_do([args[1]])
    return 0 if ok else 1


def cmd_walk(args):
    if len(args) < 2:
        print("走到哪：my walk 900 200", file=sys.stderr)
        return 1
    return 0 if send({"walkTo": {"x": float(args[0]), "y": float(args[1])}}) else 1


def cmd_do(args):
    if not args:
        print("做什么：my do 举手（my list 看全部）", file=sys.stderr)
        return 1
    name = args[0]
    if name in PLANS:
        path = os.path.join(HER, "native3/plans/%s.json" % name)
        try:
            with open(path, encoding="utf-8") as f:
                body = f.read()
        except Exception:
            print("读不到动作 %s" % path, file=sys.stderr)
            return 1
        return 0 if send({"plan": body}) else 1
    full = MOTIONS.get(name, name)
    if not full.endswith(".motion3.json"):
        full += ".motion3.json"
    if not os.path.exists(os.path.join(MOTION_DIR, full)):
        print("没这个动作：%s（my list 看全部）" % name, file=sys.stderr)
        return 1
    return 0 if send({"motion": full}) else 1


def cmd_face(args):
    if not args:
        print("什么表情：my face 前倾", file=sys.stderr)
        return 1
    return 0 if send({"expr": args[0]}) else 1


def cmd_mood(_args):
    m = cur_mood()
    if not m:
        print("读不到心情（%s/mood.json）" % STATE)
        return 1
    print("心情 %.0f/100　累 %.0f　开心 %.0f　难过 %.0f　在乎 %.0f"
          % (m["mood"], m["tired"], m["happy"], m["sad"], m["care"]))
    win = os.path.join(STATE, "window.json")
    try:
        with open(win, encoding="utf-8") as f:
            w = json.load(f)
        print("位置 (%d, %d)" % (w["ox"], w["oy"]))
    except Exception:
        pass
    return 0


def cmd_list(_args):
    print("动作（my do 名字）：")
    for k in MOTIONS:
        print("  %s" % k)
    for k in sorted(PLANS):
        print("  %s  （自己写的）" % k)
    print("\n表情（my face 名字）：")
    try:
        names = sorted(n[:-len(".exp3.json")] for n in os.listdir(EXPR_DIR)
                       if n.endswith(".exp3.json"))
        print("  " + "　".join(names))
    except Exception:
        print("  读不到 %s" % EXPR_DIR)
    return 0


CMDS = {
    "say": cmd_say, "walk": cmd_walk, "do": cmd_do, "face": cmd_face,
    "mood": cmd_mood, "list": cmd_list, "ls": cmd_list,
}


def main(argv):
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0
    fn = CMDS.get(argv[0])
    if not fn:
        print("不认识：%s（试试 my help）" % argv[0], file=sys.stderr)
        return 1
    return fn(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
