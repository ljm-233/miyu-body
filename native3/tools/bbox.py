#!/usr/bin/env python3
"""找她的 bbox —— 「有窗截图 − 没窗参照图」的差，不靠颜色阈值
（壁纸本身也是蓝的，按头发色筛会命中整块壁纸，第一版就是这么翻车的）。

用法: bbox.py SHOT.png REF.png [--head]
  · 打印她的包围盒（物理 px / 逻辑 px / 全局逻辑 px）
  · --head 时再打印头顶区（包围盒上 18% 那些行）的质心，当「按住她」的注入点
"""
import sys
import numpy as np
from PIL import Image

SCALE = 1.5      # 真实缩放（wl_output.scale 报 2，实际 1.5）
OUT_X = 1920     # eDP-1 在 layout 里的逻辑起点
OUT_Y = 0
BAR_PX = 60      # 顶部 bar 的物理高度：时钟每分钟变，必须切掉，否则 bbox 顶到屏顶


def mask_of(shot_png, ref_png):
    a = np.asarray(Image.open(shot_png).convert("RGB")).astype(np.int16)
    r = np.asarray(Image.open(ref_png).convert("RGB")).astype(np.int16)
    d = np.abs(a - r).sum(axis=2) > 8
    d[:BAR_PX] = False                        # 去 bar
    col = d.sum(axis=0)
    row = d.sum(axis=1)
    d[:, col <= 3] = False                     # 去孤立噪点列
    d[row <= 3, :] = False                     # 去孤立噪点行
    return d


def main():
    shot, ref = sys.argv[1], sys.argv[2]
    d = mask_of(shot, ref)
    ys, xs = np.nonzero(d)
    if len(xs) == 0:
        print("她不在画面上（差为 0）")
        return 1
    x0, y0, x1, y1 = int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())
    print(f"变化像素 {len(xs)}")
    print(f"bbox(物理) = [{x0},{y0} .. {x1},{y1}]  {x1-x0+1}x{y1-y0+1}")
    print(f"bbox(逻辑) = [{x0/SCALE:.1f},{y0/SCALE:.1f} .. {x1/SCALE:.1f},{y1/SCALE:.1f}] "
          f"  {(x1-x0+1)/SCALE:.1f}x{(y1-y0+1)/SCALE:.1f}")
    print(f"bbox(全局逻辑) = [{OUT_X+x0/SCALE:.1f},{OUT_Y+y0/SCALE:.1f} .. "
          f"{OUT_X+x1/SCALE:.1f},{OUT_Y+y1/SCALE:.1f}]")

    if "--head" in sys.argv:
        head_h = max(1, int((y1 - y0 + 1) * 0.18))
        hm = d.copy()
        hm[y0 + head_h:] = False
        hy, hx = np.nonzero(hm)
        cx, cy = float(hx.mean()), float(hy.mean())
        print(f"头顶质心(全局逻辑) = ({OUT_X + cx/SCALE:.1f}, {OUT_Y + cy/SCALE:.1f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
