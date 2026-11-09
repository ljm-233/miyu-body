# native3 —— 窗口层可替换的版本

2026-11-09 晚。从 `native2/` 复制出来的一份，**只做了一件事**：把「窗口 / GL 上下文 / 事件循环」
这三块从 `main.cpp` 抽成接口，渲染部分一行没动（Cubism、纹理、投影、物理全保持原样）。
`native2/` 从头到尾没有被修改，也没在那里编译过。

改动清单（就这些）：

| 文件 | 变化 |
| --- | --- |
| `src/platform.h` | 新增。纯 C++ 头，9 个自由函数，不含 GLFW/X11/EGL 任何类型 |
| `src/platform_x11.cpp` | 新增。GLFW 3.5 + XWayland 实现，就是原来 `main.cpp` 里那段窗口代码搬过来 |
| `src/main.cpp` | 26 处 `glfw*` 调用全部换成 `plat*`；`#include <GLFW/glfw3.h>` 删掉；别的没动 |
| `CMakeLists.txt` | `add_executable(miyu src/main.cpp src/platform_x11.cpp)`，链接库不变 |

`main.cpp` 现在**不认识任何窗口系统**。它只知道：起来 → `platInit` → 每帧 `platGetFramebufferSize`
→ 画 → `platSwapBuffers` → `platPollEvents` → 收工 `platShutdown`。

---

## 一、接口职责（src/platform.h）

```cpp
struct WindowHandle;                                            // 不透明句柄，main 永不碰内部
bool   platInit(int w, int h, const char* title);               // 建窗口 + 建 GL 上下文 + 设位置
void   platPollEvents();                                        // 每帧事件（含关窗/ESC）
void   platGetFramebufferSize(int* w, int* h);                   // 真实像素尺寸，喂 glViewport
void   platSwapBuffers();                                        // 提交一帧
bool   platShouldClose();                                        // 点关闭 / ESC
double platTime();                                               // 单调秒，给 dt
void   platSetPosition(int x, int y);                            // 挪到屏幕坐标
void   platShutdown();                                           // 销毁窗口
```

约定（两个实现都必须满足，写 Wayland 时照着做）：

1. `platInit` 返回时 **GL 上下文已经 current**，调用方直接 `glewInit()` / `gl*`。
2. 窗口必须**带 alpha 的 framebuffer**，清成 `(0,0,0,0)` 时背后是桌面。
3. 窗口**无边框 + floating**（niri 用 window-rule 认 app-id，见下）。
4. 帧循环由调用方驱动，顺序不能反：渲染 → `platSwapBuffers` → `platPollEvents`。
5. `platInit` 失败后调用其它函数必须安全（现在都是空实现，不崩）。

`[win] pos=/size=/transparent_fb=` 这条诊断日志搬进了 `platform_x11.cpp` 的
`platGetFramebufferSize`，还是第 3 帧打一次，跟重构前同一时机——它属于「窗口层的事实」，
不该让 main 去读窗口属性。

---

## 二、x11 实现踩过的坑

### 1. GLFW 3.5 默认走 Wayland 后端（最要命的那个）

```cpp
glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);   // 必须在 glfwInit() 之前
glfwInit();
```

本机 glfw 是 3.5.1，niri 是纯 Wayland 会话。不设这一行，`glfwInit()` 会优先挑 Wayland 后端，
开出来的是**普通 Wayland 窗口**：没有 `GLFW_TRANSPARENT_FRAMEBUFFER`（那 hint 只在 X11 后端生效）、
niri 不给 floating、而且拿不到桌面 GL 上下文（只有 EGL/GLES 那条路，之前已经证明纹理采样出不来颜色）。

注意它是 **init hint 不是 window hint**：写成 `glfwWindowHint` 会静默无效，程序照跑，窗口照错。
判断方法就是看 `[win] transparent_fb=` 是不是 1，以及窗口在 niri 里 `Is floating` 是不是 yes。

### 2. `GLFW_TRANSPARENT_FRAMEBUFFER` 是「请求」不是「保证」

```cpp
glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
```

它只是要求 X11 给一个 ARGB visual。XWayland 给不到时会**静默降级**，不报错，症状是窗口底板
变成一块不透明的深色/渐变。所以必须读回来验证：

```cpp
glfwGetWindowAttrib(win, GLFW_TRANSPARENT_FRAMEBUFFER)   // 期望 1
```

现在这行日志在 `platform_x11.cpp` 里，第 3 帧打 `[win] transparent_fb=1 (期望 1)`。
真掉成 0 的话，先查 `glxinfo`/`DISPLAY` 是不是真到了 XWayland，再看 GLFW 平台 hint 有没有生效。

### 3. 上下文画像：3.3 + `GLFW_OPENGL_COMPAT_PROFILE`

Cubism 官方 GLES2 渲染器那边要 `glGenerateMipmap` 那条老路径和兼容画像；
换成 CORE 画像会出现「纹理不完整 → 采样返回 (0,0,0,1) → 黑剪影」。
（本项目实际的 mipmap 修补在 `load_tex()` 里，跟这层无关，但画像别乱改。）

### 4. 位置

`glfwSetWindowPos(win, 1900, 150)` 在 `platInit` 之后由 `platSetPosition(1900,150)` 调用，
时机跟原来一样（窗口已创建、上下文已 current、GLEW 已初始化之后）。
这个坐标是**请求**，niri 会按浮动窗口规则再摆一次，`[win] pos=` 读到 (0,0) 属于正常现象
（WM 还没把窗口挪到位就读了），重构前后一模一样。

### 5. 会话环境

`run.sh` 里是 `env -u WAYLAND_DISPLAY DISPLAY=:0 ./build/miyu`：
把 `WAYLAND_DISPLAY` 摘掉，逼 GLFW/XWayland 走 X11 那条路。这一条别删。

---

## 三、为什么 X11 做不到「逐像素点击穿透」

这是写这个接口的**原因**，记清楚，免得下次又绕回来。

- X11（包括走 XWayland）的输入形状只有 **`XShape` 矩形区域**：`XShapeCombineRectangles` 给的是
  一组矩形，命中的是「点进这个矩形就算点中窗口」。它能做整窗穿透
  （`XShapeCombineRectangles(..., ShapeInput, ...)` 传空）或矩形级挖洞，
  但**表达不了一个 alpha 通道的形状**——模型的发梢、裙摆的镂空、透明边角都算矩形。
- 想要「按 alpha 阈值逐像素穿透」，需要的是 **`wl_surface.set_input_region`**，而且它接受的也是
  `wl_region`（矩形集合）。Wayland 上真正的逐像素方案是：
  每帧算一张 alpha mask（CPU 阈値或 GPU 读回），把它转成**一串矩形**（按行/按块合并）再
  `set_input_region`。矩形够细时视觉上等价于逐像素。niri 支持这个。X11 上这条路同样能写，
  但 XWayland 对 shape 的合并性能差、且透明窗口的 shape 更新会和合成器打架，实测不可用。
- 更关键的一条：X11 下**没有 layer-shell**。窗口是不是浮在最上层、跨工作区、不占焦点，
  全靠 niri 的 `window-rule`（`open-floating`、`no-border`、`no-shadow`、`no-background-blur`），
  这是「配置」不是「程序能控制的」。而 Wayland 原生 + `wlr-layer-shell` 可以让程序自己声明
  `layer=overlay`、`keyboard-interactivity=none`、`input-region`，不再依赖用户配置。

**结论**：X11 这条路能出图、能透明、能浮，但点击穿透只能到矩形级；要逐像素就得换 Wayland 原生。
所以这一层必须抽出来——渲染代码不用再动第二次。

---

## 四、下一步：Wayland 实现要填什么

新文件 `src/platform_wayland.cpp`，实现同一组 9 个函数，然后把 CMake 里的
`platform_x11.cpp` 换成它（或者按 `MIYU_BACKEND=wayland` 二选一）。`main.cpp` **一行不用改**。

| 函数 | Wayland 里怎么做 |
| --- | --- |
| `platInit` | 连 `wl_display` → 拿 `wl_compositor`/`wl_shm`/`wl_seat`/`xdg_wm_base`/`zwlr_layer_shell_v1` → 建 `wl_surface` → 建 **EGL** context（`eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR)`）→ `eglMakeCurrent` → 建 `wl_egl_window`（或直接 `EGLSurface` on surface） |
| `platPollEvents` | `wl_display_dispatch_pending` + `wl_display_flush`；**不要阻塞**，先 `prepare_read` 再 poll |
| `platGetFramebufferSize` | 用 `wl_surface` 的 buffer scale × 逻辑尺寸（2560x1600 scale 1.5 → 逻辑 1706x1066，窗口 500x900 逻辑 → 750x1350 像素） |
| `platSwapBuffers` | `eglSwapBuffers`，然后 `wl_surface_commit` |
| `platShouldClose` | 记 `xdg_toplevel.close` / `wl_keyboard` 的 ESC（layer-shell 下没有 ESC，靠信号或退出键） |
| `platTime` | `clock_gettime(CLOCK_MONOTONIC)`，别用 GLFW 的 |
| `platSetPosition` | **layer-shell 没有这个**：位置由 anchor + margin 决定，写成「下次 init 用的 anchor/margin」即可；xdg_toplevel 只能用 `xdg_toplevel.move`（需要 seat 抓取，不适合开机定位） |
| `platShutdown` | 按相反顺序销毁 + `eglTerminate` + `wl_display_disconnect` |

实现时要注意的三件事（都是这次摸出来的）：

1. **透明**：`wl_surface` 的 buffer 要 `WL_SHM_FORMAT_ARGB8888` 或 EGL 的 `EGL_ALPHA_SIZE=8`；
   layer-shell 还要设 `set_exclusive_zone(-1)` 才不会把窗口撑成一条。
2. **逐像素点击穿透**：每帧把 alpha mask（阈值 α>0.1）压成矩形列表 → `wl_region` →
   `wl_surface_set_input_region`。只在 mask 变化超过一定面积时重算，别每帧都发。
3. **纹理采样**：`native2` 时期在 Wayland + EGL 上栽过一次「怎么都采不出颜色」（桌面 GL/GLES2/GLES3
   全试过）。这次换后端时**先**用一张 2x2 纯红纹理把 GLEW/EGL 那条采样链跑通，再上 Cubism，
   否则会把窗口问题误判成模型问题。

---

## 五、这次的验证记录（2026-11-09 19:2x）

构建：`cd build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4` → `build/miyu`，无警告。

`./run.sh` 起，5 秒后 `/tmp/miyu-run.log`：

```
[main] GL=4.6 (Compatibility Profile) Mesa 26.2.6-arch1.1 | Mesa Intel(R) Graphics (ARL) | glew=0
[main] Cubism 框架起来了
[main] Moc=大肥鱼.moc3 贴图数=5
[main] 渲染器就绪，5 张贴图
[main] physics = 大肥鱼.physics3.json (217421 字节) -> ok
[win] pos=(0,0) size=500x900 framebuffer=500x900
[win] transparent_fb=1 (期望 1)
```

niri 那边：`App ID: native2-test`（标题没改，window-rule `^native2-test$` 才认得）、
`Is floating: yes`、`Window size: 500 x 900`（scale 1 的 HDMI）／`334 x 600`（scale 1.5 的 eDP）。

A/B 定位（同一会话、背靠背跑 `native2/build/miyu` 和 `native3/build/miyu`，niri 报的落点）：

```
native2 二进制： Is floating: yes  Workspace-view position: 686.67, 252  Window size: 334 x 600
native3 二进制： Is floating: yes  Workspace-view position: 686.67, 252  Window size: 334 x 600
```

两次完全一致 → 窗口几何和落点没被重构碰到。

截图判读（`grim`，两屏 5439x1619，eDP-1 scale 1.5）：

- `shot-window.png`：从开窗截图里按窗口矩形（设备像素 750x1350 → 逻辑 500x900）裁出来，
  模型头到鞋完整在同一位置，`shot-vs-native2.png` 是和 `native2/shot-noframe-crop.png` 的并排对照。
- `shot-window-off.png`：同一块区域**关掉窗口后**的样子，只剩桌面/后面的页面。
  两张一比：模型区域 127460 像素（窗口的 28.3%）发生变化，模型像素亮度均值 164/255、最大 255
  → **不是全黑**；窗口空白处与关窗后的像素完全相同（差 0）→ **是真透明**，不是画的黑底。
- 窗口外的部分就是桌面（`shot-native3-full.png` 可以看全屏）。

没有留窗口：验证完 `pkill -x miyu`，`ps` 里 0 个 `build/miyu`。
