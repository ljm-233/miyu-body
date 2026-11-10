#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 state/parts.tsv 解析结果打出来，和 C++ 侧（tools/parts-selftest.cc）逐行对账。

    python3 tools/parts-dump.py > /tmp/dump.py.txt
    (cd ~/.dsh/miyu/body/native3 && /tmp/parts-selftest --dump > /tmp/dump.cpp.txt)
    diff /tmp/dump.py.txt /tmp/dump.cpp.txt && echo 一致

两边是**两套独立实现**：这边是 python 的文本解析 + 排序，那边是 src/partapi.h 的
C++ 解析 + stable_sort。输出格式逐字节对齐（浮点分别用 %.3f / %.4f / %.1f）。

解析和排序规则复用 tools/parts-migrate.py 里的 parse_parts()/dump() —— 那是同一套
格式定义的唯一定义处，不在第二个文件里抄一遍。
"""

import importlib.util
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_migrate():
    path = os.path.join(ROOT, "tools", "parts-migrate.py")
    spec = importlib.util.spec_from_file_location("parts_migrate", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    pm = load_migrate()
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "state", "parts.tsv")
    reg = pm.parse_parts(path)
    sys.stdout.write(pm.dump(reg))
    if reg["bad"]:
        print("有 %d 处坏记录" % len(reg["bad"]), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
