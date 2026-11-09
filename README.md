# miyu-body

桌面上的一个 Live2D 小人在走路说话——Wayland 原生浮层，不经过 WebView。

两件事是这套代码的真正目的：

- **真透明**：窗口没有底，她站在桌面上，不是贴在一块黑底上
- **逐像素点击穿透**：只有她身上的像素接收鼠标，身体轮廓外的透明区域点下去直接穿到下面的窗口

## 目录

| 目录 | 是什么 |
| --- | --- |
| `native3/` | **正路**。窗口层被抽成 `src/platform.h` 九个函数，`platform_x11.cpp` 和 `platform_wayland.cpp` 是两套实现，渲染代码（Cubism）一行都不用改 |
| `native2/` | X11(XWayland) + GLFW 版，最早跑通的那套，留着当备胎 |
| `native/` | 更早的 Wayland + EGL + GLES 尝试，**已废弃**，留着是因为里面有现成的 layer-shell 协议绑定 |
| `platform-wayland/` | Wayland 窗口层的独立验证程序（EGL + layer-shell 出颜色） |
| `input-probe/` | 点击穿透的验证程序 + `alpha_region.c`（按 alpha 生成 `wl_region` 的最小实现，带单测） |
| `wayland-probe/` | EGL + 桌面 GL + layer-shell 的最小可运行样例 |
| `alpha-probe/` | 当年排查「X11 上能不能透明」的实验 |
| `wayland-migration.md` | **先读这个**。所有已验证的结论、对照实验、九条坑 |
| `pet/`、`body*.html` | 更早的 WebView 方案（GTK + WebKit + pixi-live2d），能跑但基线内存 350MB，不是终点 |

## 依赖

这两样**不在仓库里**，需要自己拿：

1. **Live2D Cubism Native SDK**（`Core/` `Framework/` `lib/`）—— 从 Live2D 官网获取，遵守其许可条款。本仓库只包含调用它的代码，不包含 SDK 本体
2. **一个 Cubism 4.2+ 的模型**（`*.moc3` + `*.model3.json` + 贴图）—— 作者开发时用的是 B 站免费公开模型「大肥鱼公皮」（作者：神宫凉子），授权与使用限制请以原作者说明为准，本仓库不分发模型文件

系统依赖：Mesa（桌面 GL 4.6）、`wayland-client`、`egl`、`glfw`（仅 X11 版需要）、`grim`（截图调试用）

## 编译

```sh
cd native3
mkdir -p build && cd build
cmake .. && make
./miyu            # X11(XWayland) 版
./miyu-wayland    # Wayland 原生版
```

跑之前先确认同名进程都杀干净了——两扇透明窗口叠在同一点，看起来就像模型散架了（这不是 bug，是两个人）。

## 几件真正卡过人的事

完整清单在 `wayland-migration.md`，这里只放最容易再踩的：

- **GLFW 3.5 默认走 Wayland 后端**：`DISPLAY=:0 ./程序` 也会开出 Wayland 窗口，X 树里查不到。要么 `unset WAYLAND_DISPLAY`，要么 `glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11)`
- **纹理恒黑不是 EGL 的锅**：Cubism 每帧把 `MIN_FILTER` 设成 `GL_LINEAR_MIPMAP_LINEAR`，贴图不生成 mip 链就是 incomplete，采样恒 `(0,0,0)`。每个纹理上传后必须 `glGenerateMipmap`
- **niri 的 `background-effect { xray true; blur true }` 会把桌面糊在透明像素后面**，看着像一块脏底板。给她单独关掉
- **`wl_surface_set_buffer_scale()` 不能省**：只按 `w*scale` 建大 buffer 不够，不加这句 niri 会把它当逻辑尺寸，窗口反而被放大更糊
- **逐像素穿透只有 Wayland 能做到**：X11 的 Shape 扩展 xwayland-satellite 完全不处理，`_NET_WM_WINDOW_OPACITY` niri 也不认

## 状态

- 透明、圆角、无阴影、浮动：成立（X11 与 Wayland 两版都能）
- 逐像素点击穿透：Wayland 版成立（`wl_surface.set_input_region`，物理点击实测过）
- 眨眼、待机动作、口型：还没做
