# 搬到 Wayland 原生：已验证的结论与坑（2026-11-09 晚）

> 起因：X11（XWayland）那条路撞了两个硬边界——`_NET_WM_WINDOW_OPACITY` niri 不认、
> X11 Shape 输入区 xwayland-satellite 零处理（源码 grep 过，全仓库唯一的 `Shape` 是触摸事件字段）。
> 所以逐像素点击穿透只能走 Wayland 原生。
> 本文是三个验证实验的结论汇总，代码在 `body/wayland-probe/`、`body/input-probe/`。

## 一、EGL + 桌面 GL + layer-shell：通了

**结论**：`zwlr_layer_shell_v1`(OVERLAY) + `eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR)`
+ `eglBindAPI(EGL_OPENGL_API)` 在 niri 26.04 上拿到 **GL 4.6 Core**，红底、绿三角、彩色棋盘格全部正确。

实测像素：绿三角内部 `(0,255,0)`、三角外 `(255,0,0)`；
`eglChooseConfig` 实拿 config：`ALPHA_SIZE=8`、RGB 8/8/8、`CONFIG_CAVEAT=EGL_NONE`、
`EGL_RENDERABLE_TYPE=0x4d`（含 `EGL_OPENGL_BIT`，另带 ES2/ES3，Mesa 合在一个 config 里，不影响）。

**当年 `body/native/`「纹理采样恒黑」的根因，这次用对照实验坐实了**：

| 组 | 设置 | 结果 |
| --- | --- | --- |
| A | MIN_FILTER=`GL_LINEAR`，无 mip | 正常（蓝 48.0% / 白 46.2% / 黑 0%） |
| B | MIN_FILTER=`GL_LINEAR_MIPMAP_LINEAR`，**不生成 mip** | **纯黑 100.0%** |
| C | MIN_FILTER=`GL_LINEAR_MIPMAP_LINEAR` + `glGenerateMipmap` | 正常（蓝 47.1% / 白 35.1%） |

原因是 Cubism 每帧把 MIN_FILTER 设成 `GL_LINEAR_MIPMAP_LINEAR`，纹理不带 mip 链就是 incomplete，
采样恒 `(0,0,0)`；配 premultiplied 混合正好是黑剪影。**所以不是 EGL 的锅，也不是 GLES 的锅。**

## 二、逐像素点击穿透：通了，物理点击实测

`wl_surface.set_input_region()` 在 niri 上生效，协议零错误。

验证方法（不用 ydotool、不用 root）：用 niri 自带的 `zwlr_virtual_pointer_v1` 注入点击，
下层垫一个整块收点击的 recorder overlay，上层放带 region 的 probe：

| 全局坐标 | 期望 | 实际 |
| --- | --- | --- |
| 圆心 | 被浮层吃掉 | `BUTTON probe (200,200)` ✓ |
| 圆内靠左 | 被吃掉 | `BUTTON probe (50,200)` ✓ |
| 四角 | 穿透 | `leave probe → enter recorder → BUTTON recorder` ✓ |
| 圆外 3px 临界 | 穿透 | `BUTTON recorder` ✓ |

region 算法：按行扫描 alpha>阈值的连续段、竖直同 x/w 合并成竖带（**不逐像素 add**）。
一个 R=150 的圆生成 **175 条矩形，覆盖 70672 / 160000 px**，理论 π·150²=70686，误差 0.02%。
单测 27 项零失败（全透明/全不透明/一个洞/斜线/阈值边界/抗锯齿圆，每例都断言面积等于真值 + 逐像素不重不漏）。

接口（`body/input-probe/alpha_region.h`）：
```c
size_t alpha_region_scan(const uint8_t*, int32_t w, int32_t h, size_t stride,
                         uint8_t threshold, struct alpha_rect* out, size_t max);
struct wl_region* alpha_region_build(struct wl_compositor*, const uint8_t*, int32_t w,
                                     int32_t h, size_t stride, uint8_t threshold, size_t max);
void alpha_region_apply(struct wl_surface*, struct wl_compositor*, const uint8_t*, int32_t w,
                        int32_t h, size_t stride, uint8_t threshold, size_t max);
```

## 三、踩过的坑（别再走一遍）

1. **GLFW 3.5 默认走 Wayland 后端**：`DISPLAY=:0 ./程序` 也会开出 Wayland 窗口，X 树里查不到。
   要么 `unset WAYLAND_DISPLAY`，要么 `glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11)`。
2. **GL 入口别用 GLEW**：Wayland 下 `glewInit` 报 "No GLX display"，用 `eglGetProcAddress` + `dlsym` 手撸。
3. **`wl_output.scale` 报 2，实际是 1.5**：niri 向上取整。逻辑尺寸以 `niri msg outputs` 的
   `Logical size: 1706x1066` 为准。要锐就按 `ceil(scale)` 建 buffer + `wp_fractional_scale_v1` + `wp_viewport`。
4. **顶部 bar 占 exclusive zone**（约 37 逻辑 px）：`anchor TOP|LEFT` 落在物理 y=56 不是 0，贴边靠 `set_margin`。
5. **`wl_display_dispatch` 是阻塞读**：要 `poll(fd, 200ms)` 包一层，否则 pkill 后窗口留在屏幕上（改完 SIGTERM 1ms 退出）。
6. **同一 overlay 层后创建的在上**：垫底的 surface 必须先建。
7. **`zwlr_layer_shell_v1.get_popup` 引用 `xdg_popup_interface`**：链接必须带 xdg-shell 协议代码。
8. **`zwlr_virtual_pointer_v1.frame` 不带时间参数**（v2 签名），照老 wlroots 代码写编不过。
9. **region 坐标是 surface-local 逻辑像素**：buffer 用 scale=2 渲染时要先降到逻辑尺寸，否则区域大一倍。

## 四、X11 那条路（现在还能用，作备胎）

- 透明：不用动代码，靠 niri 的 `window-rule` 给 `^native2-test$` 关掉 `background-effect { blur false; xray false }`
  （`blur.kdl` 里那条全局规则会把桌面糊在半透明像素后面，看着像一块脏底板）。
- 圆角/阴影：同一条规则里 `geometry-corner-radius 14` + `clip-to-geometry true` + `shadow { off }`。
- 改完生效用 `niri msg action load-config-file`（**不是 `reload-config`，那个子命令不存在**）。
- 做不到：逐像素点击穿透、`_NET_WM_WINDOW_OPACITY`。
