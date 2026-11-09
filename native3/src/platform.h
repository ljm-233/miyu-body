// 窗口层接口 —— 纯 C++ 头，不引入任何 GLFW / X11 / EGL 类型。
//
// 目的：把 main.cpp 里的「窗口 / GL 上下文 / 事件循环」三件事抽到这一层，
// 让渲染代码（Cubism + 桌面 GL）跟窗口系统解耦。现在只有 X11(XWayland) 实现，
// 下一步加 Wayland 原生实现（layer-shell 或 xdg_toplevel + EGL），只要再写一个
// platform_wayland.cpp 实现同样这几个自由函数，main.cpp 一行都不用改。
//
// 约定期望（两个实现都必须满足）：
//   · platInit 之后 GL 上下文已经 current，调用方直接 glewInit() / gl* 即可
//   · 窗口必须透明（framebuffer 带 alpha），清成 (0,0,0,0) 时背后是桌面
//   · 窗口必须无边框、不进平铺布局（floating）
//   · 帧循环由调用方驱动：render → platSwapBuffers → platPollEvents
//   · 任何函数在 platInit 失败后调用都不许崩（空实现）

#pragma once

// 不透明句柄。main.cpp 永远不碰它的内部，只当它存在。
// X11 实现里它其实是 GLFWwindow*；Wayland 实现里会是 wl_surface + EGLSurface 的包装。
struct WindowHandle;

// 建窗口 + 建 GL 上下文 + 设位置。w/h 是逻辑尺寸（像素），title 是窗口标题
// （niri 的 window-rule 按 app-id 匹配，GLFW 会把 title 同时写进 WM_CLASS）。
// 返回 false 表示这一层起不来，调用方直接退出。
bool platInit(int w, int h, const char* title);

// 处理这一帧的事件（含关窗、ESC）。每帧调一次。
void platPollEvents();

// 当前 framebuffer 的真实像素尺寸（要喂给 glViewport）。
void platGetFramebufferSize(int* w, int* h);

// 提交这一帧。
void platSwapBuffers();

// 该收工了吗（点关闭 / ESC）。
bool platShouldClose();

// 单调时间（秒），给 dt 用。
double platTime();

// 把窗口挪到屏幕坐标 (x, y)。
void platSetPosition(int x, int y);

// 销毁窗口（调用方负责先释放 GL 资源）。
void platShutdown();

// 只有 Wayland 实现会真的用：把当前 framebuffer 的 alpha 转成输入区域。
// X11 实现里是空函数。
//
// 参数是 glReadPixels 读回来的整张 framebuffer：RGBA8888、**首行是底部**
// （GL 原点在左下），尺寸是物理像素。实现负责两件事：
//   ① 上下翻转（wl_region 的原点在左上）
//   ② 物理 → 逻辑：除以 buffer scale，再交给 alpha_region_apply
//      （scale=2 时逻辑区域是物理尺寸的一半，算错了区域会大一倍）
void platSetInputRegionFromAlpha(const unsigned char* rgba, int w, int h, int threshold);
