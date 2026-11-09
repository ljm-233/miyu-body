#!/usr/bin/env python3
# 判读 /tmp/wl-probe.png：先找红色画布的包围盒，再按逻辑坐标取三块区域
import sys, collections
from PIL import Image

path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/wl-probe.png"
im = Image.open(path).convert("RGB")
W, H = im.size
px = im.load()
print(f"shot = {W}x{H}")

def close(c, t, tol=18):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))

RED   = (255, 0, 0)
GREEN = (0, 255, 0)

# 1) 找红色背景的包围盒（限制在左上 1200x1200 内，避免别的红）
x0, y0, x1, y1 = W, H, -1, -1
for y in range(0, min(H, 1200), 2):
    for x in range(0, min(W, 1200), 2):
        if close(px[x, y], RED):
            if x < x0: x0 = x
            if y < y0: y0 = y
            if x > x1: x1 = x
            if y > y1: y1 = y
print(f"红底包围盒 = ({x0},{y0}) - ({x1},{y1})  尺寸 {x1-x0+1}x{y1-y0+1}")

if x1 < 0:
    print("!! 没找到红色画布 —— 黑屏或没上来")
    # 顺便报告左上角实际是什么
    print("左上角像素:", [px[i, i] for i in range(0, 200, 40)])
    sys.exit(1)

# 画布 -> 逻辑坐标（surface 是 400x400 逻辑）
cw = x1 - x0 + 1
sx = cw / 400.0
print(f"逻辑像素倍率 = {sx:.3f}")

def rect_hist(lx0, ly0, lx1, ly1, step=1):
    X0 = x0 + int(lx0 * sx); X1 = x0 + int(lx1 * sx)
    Y0 = y0 + int(ly0 * sx); Y1 = y0 + int(ly1 * sx)
    h = collections.Counter()
    n = 0
    for y in range(Y0, Y1, step):
        for x in range(X0, X1, step):
            h[px[x, y]] += 1
            n += 1
    return h, n, (X0, Y0, X1, Y1)

def px_at(lx, ly):
    return px[x0 + int(lx * sx), y0 + int(ly * sx)]

# 区域（逻辑坐标 400x400；GL 的 y 上正 → 图像 y 下正）
# 三角: NDC x[-0.95,-0.35] y[-0.85,0.85] -> 逻辑 x[10,130] y[30,370]
LY = lambda ndc_y: (1 - ndc_y) / 2 * 400

tri_c = (100, int(LY(0.0)))     # 三角腰上一点
print("\n=== 三角区域 (采样点 逻辑(100,200) 中心) ===")
p_tri = px_at(*tri_c)
print(f"  中心像素 = {p_tri}   是绿色? {close(p_tri, GREEN, 30)}")
h, n, r = rect_hist(60, 150, 120, 260)
print(f"  区域 {r} 主色 top3 = {h.most_common(3)}")

# 三个四边形：NDC x 区间 -> 逻辑 x = (ndc+1)/2*400
def ndc2lx(v): return (v + 1) / 2 * 400
quads = {
    "A LINEAR(无mip)":      (-0.30, -0.06),
    "B LM_LM(无mip)":       ( 0.00,  0.24),
    "C LM_LM(有mip)":       ( 0.30,  0.54),
}
print("\n=== 三个棋盘格 ===")
for name, (a, b) in quads.items():
    lx0, lx1 = ndc2lx(a), ndc2lx(b)
    h, n, r = rect_hist(lx0 + 3, 120, lx1 - 3, 300)
    top = h.most_common(4)
    print(f"\n  [{name}] 逻辑 x {lx0:.0f}..{lx1:.0f}  区域 {r}  像素数 {n}")
    print(f"    主色 top4 = {top}")
    print(f"    中心像素 = {px_at((lx0+lx1)/2, 200)}")
    blue  = sum(c for k, c in h.items() if k[2] > 150 and k[0] < 90 and k[1] < 90)
    white = sum(c for k, c in h.items() if k[0] > 200 and k[1] > 200 and k[2] > 200)
    black = sum(c for k, c in h.items() if k[0] < 30 and k[1] < 30 and k[2] < 30)
    print(f"    蓝={blue/n*100:.1f}%  白={white/n*100:.1f}%  黑={black/n*100:.1f}%  -> 判: ", end="")
    if black > 0.9 * n:
        print("全黑（贴图不完整）")
    elif blue > 0.15 * n and white > 0.15 * n:
        print("彩色棋盘格 OK")
    else:
        print("异常")

# 右侧没画东西的红底
print("\n=== 红底参照 (逻辑 x 340..380, y 120..300) ===")
print("  中心像素 =", px_at(360, 200))
