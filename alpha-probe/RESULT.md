# alpha-probe 结果（2026-11-09，机器：Arch + niri 26.04 + xwayland-satellite 0.8.3）

## 结论

1. **逐像素 alpha：成立。** X11（XWayland）窗口用 depth-32 ARGB visual，niri 确实按逐像素 alpha 合成它。
2. **但 buffer 必须写预乘（premultiplied）颜色。** 直通 alpha `glClearColor(1,0,0,0.5)` 会得到 R 恒被钳到 255 的"亮红/粉雾"，因为 niri 按 `out = src + dst*(1-a)` 合成。
   正确写法：`glClearColor(0.5,0,0,0.5)`。
3. **`_NET_WM_WINDOW_OPACITY` 整窗半透明：不成立。** xprop 设置成功，画面逐像素不变；niri 26.04 二进制里没有这个 atom，也没有 `wp_alpha_modifier`。

## 关键事实

- visual：id 0x95，depth 32，visual_class 4（TrueColor），mask R=0xff0000 G=0x00ff00 B=0x0000ff
- XGetImage 直读窗口内容（不是截图）：红区 RGBA = (255,0,0,128)，绿区 = (0,255,0,255) → 源码里确实带 per-pixel alpha
- 396 点拟合（红半区 vs 纯壁纸帧）：`A_G = 0.476*B_G`，`A_B = 0.481*B_B`，R 通道恒 255
- 预乘版实测 vs 理想 0.5 混合：差 8~20/255（误差来自 niri 对半透明窗口自动加的**背景模糊**，截图里能看到雾感）
- `xprop ... _NET_WM_WINDOW_OPACITY 0x7FFFFFFF` 后：绿块仍 (0,255,0)，纯绿像素 915235 个不变
- `strings /usr/bin/niri | grep -iE '_NET_WM_WINDOW_OPACITY|alpha_modifier'` 无命中；niri 自带 `window-rule { opacity }` + `toggle-window-rule-opacity`（未改用户配置，未验）

## 实验条件提醒

GLFW 3.5 默认走 Wayland 后端，`DISPLAY=:0 ./probe` 实际开出来的是**原生 Wayland 窗口**（X 树里查不到，`xprop -name` 找不到）。
必须 `glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11)`（或 unset `WAYLAND_DISPLAY`）才是真的 XWayland 客户端。

## 证据截图

- `shots/08-probe-on-wallpaper.png` + `shots/07-wallpaper-only.png`（同位置有/无窗口的两帧，逐点对比用）
- `shots/11-premul.png`（预乘版，数值与理想混合吻合）
- `shots/crop-over-kitty.png`（半透明区背后的模糊雾感）
- `shots/09-opacity.png`（设了 _NET_WM_WINDOW_OPACITY 之后，无变化）

## 文件

- `probe.c`：半透明红底 + 左侧不透明绿块；参数 `pm` = 预乘模式，数字 = 画 N 帧后退出
- `bg.c`：不透明纯蓝窗（做底用）
- `rawdump.py`：用 python-xlib 直读窗口 RGBA
