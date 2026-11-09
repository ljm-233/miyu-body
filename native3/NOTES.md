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

> 2026-11-09 晚已落地，实际填法见第六节（这一节留着当设计意图，别当待办）。

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

---

## 六、Wayland target 集成完成（2026-11-09 晚）

**X11 版是备胎，Wayland 版是正路。**
X11(XWayland)+GLFW 那条路撞的是硬边界：`_NET_WM_WINDOW_OPACITY` niri 不认、
X Shape 输入区 xwayland-satellite 零实现 —— 逐像素点击穿透在 X11 上永远做不到。
Wayland 版透明真生效、`wl_surface.set_input_region` 真管用，所以以后默认走它，
X11 目标只留着做对照（它还能编、还能跑，别删）。

### 6.1 两个 target 怎么编

```bash
cd /home/link/.dsh/miyu/body/native3
cmake -S . -B build && make -C build -j4
#   build/miyu          ← X11(XWayland)+GLFW，备胎
#   build/miyu-wayland  ← Wayland layer-shell + EGL 桌面 GL，正路
./run.sh              # 起 X11 版
./run.sh --wayland    # 起 Wayland 版（别 unset WAYLAND_DISPLAY）
```

`miyu-wayland` 的源：`main.cpp platform_wayland.cpp layer-shell-protocol.c
xdg-shell-protocol.c alpha_region.c`；链 `wayland-client wayland-egl egl GL GLEW dl m`。
**main.cpp 一份代码编两个后端**，靠 `platform.h` 那 10 个自由函数分流，渲染部分零分叉。

三条不能省：

1. **GLEW 还得链**。`main.cpp` 和 Cubism 的 GL 渲染器都 `#include <GL/glew.h>`，`gl*` 是宏，
   展开成 libGLEW 里的 `__glew*` 变量。EGL 下 `glewInit()` 返回 **4 = GLEW_ERROR_NO_GLX_DISPLAY**，
   但指针**全都解析到了**（实测 44 个常用入口 0 个空指针：GLEW 退回 `glXGetProcAddress`，
   那一步不需要有 GLX display）。所以返回值忽略掉就行，日志里那个 `glew=4` 不是错。
2. **xdg-shell 协议代码必须一起链** —— layer-shell 的 `get_popup` 引用 `xdg_popup_interface`。
3. `alpha_region.c` 交给 C 编译器（`set(CMAKE_C_STANDARD 11)`），别塞进 g++。

### 6.2 今晚踩的两个真坑

**① EGL context 必须 COMPATIBILITY，不能 CORE。**
`platform_wayland.cpp` 原来是 3.3 core（probe 里够用，因为 probe 只用 VAO+VBO）。
Cubism 的 GLES2 渲染器用的是**客户端顶点数组**那套老 API
（`CubismRenderer_OpenGLES2.cpp:1157`：`glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_SHORT, 栈上数组)`），
core profile 下这就是 `GL_INVALID_OPERATION`。症状极有辨识度：
**460 个可见 drawable、包围盒正常、一条 GL 报错都没打，但 framebuffer 全是 alpha=0**
—— region 算出 0 条矩形、截图只有壁纸。改成 `EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT`
立刻出人。X11 那边是同一个原因（platform_x11.cpp 坑③）。

**② layer-shell 的 margin 是相对单个 output 的逻辑坐标，不是全屏坐标。**
`main.cpp` 里那句 `platSetPosition(1900,150)` 是 X11 那套屏幕坐标；在 1706 宽的 eDP-1 上
就是屏外，niri 把整块放到看不见的地方（截图只有壁纸，而且看不出哪里错了）。
现在 `apply_margin()` 用 `wl_output.mode / scale` 粗估逻辑屏尺寸把位置夹回屏内
（日志会打 `位置夹回屏内：(1900,150) -> margin (780,0)`）。粗估偏保守（scale 被 niri 向上取整成 2），
只用来防跑出屏外，不做精确布局。

另外：`alpha_region.h` 补了 `extern "C"` 守卫。它是 C 头，被 C++ 的 `platform_wayland.cpp`
include 时不给 C 链接，调用点会被 name-mangle，和 `alpha_region.c` 里的符号对不上（链接期才炸）。

### 6.3 platSetInputRegionFromAlpha 的坐标换算（整块集成唯一的算术）

`main.cpp` 每 10 帧（或 framebuffer 尺寸变化时）做：

```cpp
glPixelStorei(GL_PACK_ALIGNMENT, 1);
glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());  // W,H = 物理像素
platSetInputRegionFromAlpha(pix.data(), W, H, 8);
```

（必须在 `platSwapBuffers()` **之前**读 —— 读的是刚画完的这张 back buffer。
环境变量 `MIYU_NO_INPUT_REGION=1` 整块跳过，用来对照排查。）

换算全在 `platSetInputRegionFromAlpha()` 里，**因为它知道 scale，main 不知道**：

| 维度 | glReadPixels 给的是 | wl_region 要的是 | 做法 |
| --- | --- | --- | --- |
| 尺寸 | 物理像素 1000x1800 | surface-local 逻辑 500x900 | 除以 `buffer scale`（本机 2） |
| 原点 | 左下 | 左上 | 上下翻转：逻辑第 ly 行取物理第 `h-1-(ly*s+dy)` 行 |
| 采样 | 逐像素 | 逐像素 | 每个逻辑像素取它那 s×s 块里 alpha 的**最大值**（最近邻会把头发丝、手指削掉，削掉就是点不到） |

不除 scale 的后果：区域大一倍，她旁边大半张空白也吃点击，"穿透"退化成"整窗可点"；
不翻转的后果：可点区域上下镜像 —— 头的位置能吃点击、脚的位置吃不到。
两件事都不会报错，只能靠对账发现（见 6.4 的 bbox）。

### 6.4 验证记录（2026-11-09 19:3x）

```
[plat-wl] 选中 eDP-1，逻辑屏约 1280x800 (mode 2560x1600 / scale 2)
[plat-wl] configure 500x900
[plat-wl] GL_VERSION=4.6 (Compatibility Profile) Mesa 26.2.6-arch1.1
[plat-wl] 逻辑尺寸 500x900 -> 物理尺寸 1000x1800 (scale=2)
[main] GL=4.6 (Compatibility Profile) ... | glew=4          ← 4 是预期，见 6.1
[main] Cubism 框架起来了 / Moc=大肥鱼.moc3 贴图数=5 / 部件 715 个
[tex] 1024x2048 id=1 / 1024x2048 id=2 / 512x512 id=3 / 2048x2048 id=4 / 1024x1024 id=5
[plat-wl] input region: 物理 1000x1800 -> 逻辑 500x900 | 841 条矩形 | 覆盖 128502 px (28.6%) | bbox=[108,107 .. 446,837] | threshold=8
```

**三条对账，都用数字，不看"截图还挺好看"**：

1. **她身上不是黑剪影**：物理点 `(1461,325)`（头发）＝ `(55,142,232)`，B 远大于 R；
   同一坐标在"没窗口"的对照图里是 `(175,179,204)`（壁纸）→ 真的画了东西，而且是蓝的。
2. **窗口空白处＝壁纸**：`(1180,100) (1180,1300) (1900,1200) (1880,60)` 四个点，
   有窗口/无窗口**逐字节相同** → 透明生效，不是黑底。
3. **region 和画面对得上**（这条同时验了 scale 除法和 Y 翻转）：
   截图里变化的像素 → surface-local 逻辑 `[104,144 .. 438,874]`；
   region 报的 `[108,107 .. 446,837]`，加上顶部 bar 的 37 逻辑 px exclusive zone（layer surface
   margin top=0 落在 bar 下面）→ `[108,144 .. 446,874]`，跟画面差 ≤8 px（2x2 块取 alpha 最大值 + 3 px 抽样步长）。

`MIYU_NO_INPUT_REGION=1` 再跑一次：日志里 `input region` 相关行 **0 条**（开关有效），
她照样在（头发像素同为 `(55,142,232)`）。

收尾：`pkill -x miyu-wayland; pkill -x miyu`，`ps` 里两种进程各 0 个，屏幕不留窗口。

---

## 七、鼠标拖动（2026-11-09 20:0x）

`wl_seat` + `wl_pointer`，按 BTN_LEFT + 指针在她身上 → 拖；`MIYU_DRAG=0` 关掉；
拖动期间 main.cpp 跳过 input region 重算（`platIsDragging()`），松手那一帧补算一次。

### 7.1 三个必须记住的坑

**① wl_seat 只能绑 v4，不能绑最新的。**
`wl_pointer` 的版本跟着 seat 走；v5 起多了 `frame / axis_source / axis_stop /
axis_discrete`，listener 里少写一个 libwayland 就当场
`listener function for opcode 5 of wl_pointer is NULL` 然后**断开连接、进程直接死**。
v4 的 wl_pointer 正好是 `enter/leave/motion/button/axis` 五个，一一对上。

**② layer surface 的 local 坐标，差要**去重**，而且不能做「自己挪了多少就补偿多少」。**
niri 算 `motion` 的 local 用的是**拖动开始那会儿的 surface 矩形**，我们挪窗口它不改；
并且我们每 commit 一次 margin，它还会把**同一个 local 重发一遍**（一次 commit 一个事件）。
两种写法都试过，都炸：
- 按「上次 local」累加 + 挪完补偿：每个重发的同值事件都被当成新位移，
  注入 −100 窗口跑 −620（几十帧飞到屏幕边上）。
- 现在的写法：**同值事件直接丢**（`x == refSx && y == refSy` 就 return），
  差值就是指针的纯位移，不做补偿。实测注入 −100 → margin −100，一格不差。
  代价：真鼠标停住不动时不会有 motion 事件，也就不会漂。

**③ 屏幕逻辑尺寸要问 xdg-output，不能用 mode/scale 估。**
`wl_output.scale` 被 niri 向上取整成 2（真实 1.5），`mode/scale = 1280x800`，
比窗口 500x900 还矮 → 竖直方向 margin 永远被 `apply_margin` 夹成 0，**拖着不动**。
改成 `zxdg_output_manager_v1` 拿真逻辑尺寸（1707x1067）后竖直方向正常。
顺带：`apply_margin` 现在把夹过的值**写回** posX/posY，不然拖动时 pos 会一直在屏外涨。

### 7.2 验证怎么做（工具都在 `tools/`，input-probe/ 只读）

```bash
gcc -O2 -I../.. -I../../input-probe $(pkg-config --cflags wayland-client) \
    -o tools/draginject tools/draginject.c \
    ../../input-probe/gen/virtual-pointer-protocol.c $(pkg-config --libs wayland-client)

tools/dragtest.sh DX DY STEPS STEP_MS     # 等静默 → grim → 注入 → grim → 比对
```

- 注入用 `--rel`（relative motion）。**别用 motion_absolute**：那是瞬移，
  合成器那边 surface 位置还没跟上时算出来的假位移会把结论带偏。
- bbox 用「有窗截图 − 没窗参照图」的差，别按头发色筛 —— 壁纸也是蓝的，会命中整块屏；
  顶部 bar 要切掉（时钟每分钟变）。

实测（eDP-1 scale 1.5，窗口 500x900 逻辑）：

| 注入 | margin 前 → 后 | 截图 bbox 前 → 后（物理） |
| --- | --- | --- |
| (−100, 0) | (1207,150) → (1107,150) | [1967,442..2468,1537] → [1817,442..2318,1537]（−150 px = −100 逻辑）|
| (−80, −50) | (1061,94) → (981,44) | [1751,358..2249,1453] → [1631,283..2129,1378]（−120,−75 px）|
| (−80, +30) | (592,1) → (512,31) | [1044,218..1545,1312] → [927,263..1425,1358] |
| `MIYU_DRAG=0`，(−100,0) | 无变化 | bbox 完全相同，日志 0 条拖动记录 |

拖动期间 region 重算：`0` 次（松手那行会打「拖动期间跳过 N 帧」，实测 N=149）。

---

## 八、眨眼 + 待机动作 IDLE（2026-11-09 20:xx）

### 8.1 主循环顺序（死规矩，写反了不报错、就是不生效）

```cpp
model->LoadParameters();
motionManager->UpdateMotion(model, dt);     // ① 动作先写参数
eyeBlink->UpdateParameters(model, dt);      // ② 眨眼压过动作
physics->Evaluate(model, dt);               // ③ 物理读参数算头发/尾巴
model->Update();                            // ④ 最后才算进顶点
```

- `CubismEyeBlink::Create()` 收的是 **`ICubismModelSetting*`，不是文件路径**。
  `main.cpp` 里已经有解析好的 `CubismModelSettingJson setting`，直接 `Create(&setting)`；
  它自己遍历 `model3.json` 的 `Groups` 里 `Target == "EyeBlink"` 的 `Ids`
  （本模型：`ParamEyeLOpen` / `ParamEyeROpen`）。日志会打 `眨眼：参数 2 个 [...]`，
  这个数字是 0 就说明 `Groups` 没读到，别往下查渲染。
- `UpdateParameters` 必须在 `model->Update()` **之前**；顺序反了画面一动不动，且没有任何报错。
- 默认节奏：间隔 4s（`rand` 随机 0..2×间隔），闭 0.1 / 合 0.05 / 开 0.15 = 一次 0.3s。
  所以**验证窗口至少要 8s**，30 张 100ms 的窗口有一半概率一次都没眨。
- 眨眼状态翻转会打一行 `[blink] 闭/睁 ParamEyeLOpen=... t=...`（一次眨眼两行），
  排查时先看这个，比截图快。

### 8.2 IDLE 怎么加载（`Motions` 是空的，只能自己 Create）

`model3.json` 里 `Motions` 是 `null` —— 那 8 个 `motion3.json` 一个都没登记，
`setting.GetMotionCount()` 是 0，**不能**指望 `setting` 帮忙加载。

```cpp
auto buf = slurp(std::string(kModelDir) + "Animation/IDLE.motion3.json");
CubismMotion* m = CubismMotion::Create(buf.data(), buf.size());   // 只吃内存，不吃路径
m->SetLoop(true);        // ← 必须自己设，见下
m->SetLoopFadeIn(true);
m->SetFadeInTime(1.0f);  // 从静态站姿淡入，别一上来就弹
m->SetFadeOutTime(1.0f);
motionManager = new CubismMotionManager();
motionManager->StartMotionPriority(m, false /* autoDelete */, 1 /* priority */);
```

**`CubismMotion::Create` 不读 `Meta.Loop`**：`CubismMotion.cpp` 里那两行是注释掉的
（`// ret->_loop = (ret->_motionData->Loop > 0);`）。IDLE 的 `Meta.Loop` 明明是 `true`，
不自己 `SetLoop(true)` 就是播 5.983s 停住、`GetDuration()` 变 -1 之外什么都不发生 —— 看起来像"没接上"。

### 8.3 哪几个 motion 绝对不能播

`Animation/` 里这 6 个是**给另一版模型做服装/体型切换**的，本模型播了会散架
（部件/参数结构对不上，不是"动作不好看"）：

- `脱捕.motion3.json`
- `举手切默认手.motion3.json`
- `默认手切举手.motion3.json`
- `狗尾草切换出来.motion3.json`
- `狗尾草切回默认手.motion3.json`
- `竖手指切出来动画.motion3.json`
- `竖手指切回默认.motion3.json`

**只播 `Animation/IDLE.motion3.json`。** 加新动作前先看它改的是不是这几个部件。

### 8.4 两个开关

| 环境变量 | 作用 | 关掉后日志 |
| --- | --- | --- |
| `MIYU_NO_EYES=1` | 不建 `CubismEyeBlink`，不眨眼 | `眨眼：MIYU_NO_EYES 已设 —— 关`，`[blink]` 行 0 条 |
| `MIYU_NO_IDLE=1` | 不读 `IDLE.motion3.json`，不建 `motionManager` | `待机动作：MIYU_NO_IDLE 已设 —— 关` |

配合已有的 `MIYU_NO_PHYS` / `MIYU_NO_INPUT_REGION` / `MIYU_DRAG` 用，排查时一次只关一个。

### 8.5 收尾

`main()` 结束前按 动作 → 眨眼 → 渲染器 → 模型 → moc → `CubismFramework::Dispose()` 的顺序释放。
`StartMotionPriority(..., autoDelete=false, ...)`：循环动作不会自己结束，所有权留在我们手里，
所以是 `StopAllMotions()` + `delete motionManager` + `ACubismMotion::Delete(idle)` 三步，
不能只靠 `autoDelete`。

### 8.6 验证怎么做（别靠肉眼，靠像素）

`grim` 连拍太慢（一张 ~0.6s，100ms 的眨眼直接漏掉），改用 **wf-recorder 录 60fps + ffmpeg 裁帧**：

```bash
wf-recorder -o eDP-1 -r 60 -f /tmp/rec.mp4      # 录 20s+，前 12s 是窗口/物理沉降期
ffmpeg -ss 14 -t 6 -i /tmp/rec.mp4 -vf "crop=550:1124:1952:428" -pix_fmt rgb24 -f rawvideo -
```

**坑**：`crop` 的宽高必须是**偶数**。`yuv420p` 下 ffmpeg 会悄悄把它调成偶数，
按奇数宽高去 `reshape` 就会整帧错位 —— 表现为"每一帧都变、86% 像素都在动"，
而单独看每张图又完全正常。实测裁剪宽高取 550×1124 时 `len(raw) % (w*h*3) == 0`。

实测（eDP-1，窗口物理 bbox `[1967,442 .. 2468,1537]` = 502×1096）：

| 项 | 结果 |
| --- | --- |
| 眼部区域（全屏物理） | `x 2060..2314, y 516..612`（254×96） |
| 眨眼：30 张 / 间隔 100ms 窗口 | 最多 **20/30 张**眼部有变化（中位 14/30），单步峰值 **4690 px** |
| 眨眼：眼部平均亮度差 | 峰值 **13.2**（0–255），肉眼即闭眼（截图 `eye_closed.png`） |
| 眨眼频率 | 22s 里 4 次，日志 `[blink] 闭/睁` 各 4 条 |
| IDLE：逐帧变化（60fps） | 中位 **649–888 px** / 帧，最大 3936 px（618k 像素的 0.1–0.6%） |
| IDLE：30 张 / 3s 累计 | **6422 px（1.04%）**，分布在整只身上（bbox 48..548 × 68..994） |
| 散架检查 | 模型连通块 11 个，最大块占 **99.96%**；bbox 13s / 16s 都是 502×1096，一格没变 |
| 两个开关都关 | 沉降后（t>13s）逐帧变化 **中位 0**，`[blink]` 0 条 |

**注意**：起窗后前 **10–13s** 内画面本来就在变（compositor configure 500x900 → 500x880、
物理沉降），这期间做差分会得出"整帧都在动"的假结论。取证一律从 t≥13s 开始。
另外即使两个开关全关，稳定期偶尔仍有 1–2 帧整屏重绘（17–21 万 px 一跳然后衰减），
这是平台层既有的重绘，不是动作，别把它算进"她在动"。

---

## 九、视线跟着鼠标（2026-11-09 21:xx）

一句话：**能跟**。指针在她身上时头部偏过去、眼珠转过去；离开后同一个平滑回正。
两个硬边界记住了：① 只有她**身上**的像素才算「在窗口内」；② 眼珠那两个参数**不能用 Add**。

### 9.1 加了什么

| 文件 | 变化 |
| --- | --- |
| `src/platform.h` | 加 `bool platPointer(float*, float*)`、`void platGetLogicalSize(int*, int*)` |
| `src/platform_wayland.cpp` | 两个都实现：前者读 `W.ptrIn / W.refSx / W.refSy`（enter/leave 记的账），后者读 `W.reqW/reqH` |
| `src/platform_x11.cpp` | 空壳：`platPointer` 恒 false、`platGetLogicalSize` = `glfwGetWindowSize` |
| `src/main.cpp` | 加 `LookRig / lookInit / lookTick`，主循环里夹在 `idleTick` 之后、`eyeBlink` 之前 |

`platGetLogicalSize` 是**顺手加的第二个函数**，理由写死在头注释里：`platPointer` 给的是
surface-local **逻辑**坐标（500x900），`platGetFramebufferSize` 给的是**物理**像素（1000x1800）。
拿物理宽度去归一化逻辑坐标，`nx` 永远落在 −1..0 —— 看起来像"她只会往左看"，而且不报错。

主循环顺序（新增的那一行夹在死规矩中间，位置不能动）：

```cpp
model->LoadParameters();
if (motionManager) ... else idleTick(model, now);   // 待机微动：写 AngleX/Z、BodyZ、Breath
int lw, lh; platGetLogicalSize(&lw, &lh);
lookTick(model, dt, now, lw, lh);                   // ← 视线：叠在微动之上、眨眼之前
if (eyeBlink) eyeBlink->UpdateParameters(model, dt);
physics->Evaluate(model, dt);
model->Update();
```

手感参数（都按需求写死）：头 `nx*10° / ny*6°`，眼珠 `nx*0.7 / ny*0.7`，
一阶低通 `cur += (target-cur) * min(1, dt*6)`（时间常数 ≈0.17s），离开窗口目标归 0。
`MIYU_NO_LOOK=1` 整套关掉。

### 9.2 坑①：`ParamEyeBallX/Y` 不能 Add，会把值累加到 ±1

需求说「默认值 0 的参数用 Add」——头那两个（`ParamAngleX/Y`）确实如此，Add 前读到的正好是
idle 本帧算的值，叠得干干净净。但**眼珠那两个不是**：

```
[look] 跟 nx=-0.47 ... 头(-4.72) 眼(-0.33) | Add前 angleX=-4.07 | 写入后模型 angleX=-8.77 eyeX=-1.00
                                                 ↑ 正常组合            ↑ 钉死在 -1
```

`ParamEyeBallX/Y`（本模型索引 76/77）**不被 `LoadParameters()` 归零**，Add 一帧加一次，
约 4 帧就顶到最小/最大值，之后一直是 −1（观感：眼珠永远瞟在一边，跟鼠标位置无关）。
`ParamAngleX/Y`（28/29）在同一帧里被正确归零，所以这不是"Load 没调用"，是那两个索引
没被恢复（`CubismModel::LoadParameters` 只恢复 `_savedParameters` 覆盖到的下标）。

**改法**：眼珠改用 `SetParameterValue`（没有别的写入者，默认 0，Set 等价于"在 0 上 Add"），
头继续用 Add 叠在待机微动上。诊断留在日志里：`[look] ... | Add前 angleX=… | 写入后模型 …`，
以后再有人给参数加 Add，一眼能看出有没有被累加。

### 9.3 坑②：「在窗口内」= 在她身上（逐像素穿透的必然代价）

`wl_pointer` 的 enter/leave 跟着 **input region** 走，而 input region 是按 alpha 抠的她本人
（500x900 的窗口里只占 28.6%）。指针停在窗口的透明区域时，合成器**根本不发 enter**，
平台层不知道指针在哪 —— 这不是 bug，是第八节那套穿透的直接后果。

实测对照（同一位置、只切开关）：

| 场景 | 结果 |
| --- | --- |
| `MIYU_NO_INPUT_REGION=1`（整窗可点） | 注入窗口正中 → `指针进入 surface，local=(251,452)` |
| 默认（按她抠区域） | 同样位置 → 有 enter；但窗口左右两侧的透明区 → **0 条 enter** |

由此带来一个幅度事实：可达的 `nx` 只有 **±0.5 左右**（她只占窗口中间那条），
所以头最多偏 **±5°**、眼珠 **±0.35**，只有需求里 ±10°/±0.7 的一半。
想要满幅，就得把归一化分母从「窗口半宽」换成「她本人的半宽」（input region 的 bbox 平台层
已经有了）—— 这次没改，因为需求写的是「相对窗口中心」。

### 9.4 验证（像素，不靠肉眼）

`tools/draginject` 会按下+松开；做视线验证时按在她身上等于起一次 0 位移拖动（无害但会刷两条日志）。
这次另写了个只动不按的注入器（把 `draginject.c` 里 `btn()` 那两行去掉就是），
`/tmp` 里编译，没进仓库。三个点：左侧 `(2692,611)`、右侧 `(2931,611)`、窗外 `(2400,300)`
（全局 layout 逻辑坐标，eDP-1 从 1920,0 起），每次按住 6s、等 2.6s 再 `grim`。

**A. 隔离测量**（`MIYU_NO_IDLE=1 MIYU_NO_EYES=1`，画面里只剩视线在动）：

| 对比（头部区域 538x160 px） | 变化像素 | 平均绝对差 |
| --- | --- | --- |
| 左 vs 右 | **752 px（0.87%）** | 0.48/255 |
| 左 vs 回正 | **649 px（0.75%）** | 0.30/255 |
| 右 vs 回正 | **657 px（0.76%）** | 0.32/255 |
| 纵向：看她头顶 vs 看她脚下 | 676 px（0.79%） | 0.40/255 |

三张互不相同，`nx`/`ny` 的正负也和图像一致。同配置加 `MIYU_NO_LOOK=1` 再跑一遍：
**三张逐像素完全相同（0 px，0.00%）** → 那 700 多像素确实是视线造成的，不是渲染抖动。

同日 `[look]` 日志（跟/回正各一行，每 2s 一行）：
`跟 nx=-0.47 → 头(-4.70) 眼(-0.33)` / `跟 nx=+0.49 → 头(+4.86) 眼(+0.34)` /
`跟 ny=-0.66 → 头Y(-3.95) 眼(-0.46)` / `指针离开 → 头(0.00) 眼(0.00)`。

**B. 生产配置**（idle + 眨眼 + 物理全开，跟实际跑的一样）：

| 对比（头部区域 564x160 px） | 视线开 | 噪声底（同配置 + `MIYU_NO_LOOK=1`） |
| --- | --- | --- |
| 左 vs 右 | 14138 px（15.67%） | 10181 px（11.16%） |
| 左 vs 回正 | 16940 px（18.77%） | 5513 px（6.04%） |
| 右 vs 回正 | 12484 px（13.83%） | 8505 px（9.33%） |

读法：待机微动本身就让头部区域动 6–11%（头 ±7°、歪头 ±3.5° 随机换姿态），
视线那 ±5° 叠进去只多几成 —— **参数确实在跟，但肉眼上会被待机动作盖掉一半**。
想让它一眼看见，先把 9.3 的归一化分母换掉，不是把幅度调大。

**C. 散架检查**：隔离配置下三张 bbox 都是 **499x1095，一个像素不差**（换姿态的待机动作关掉了，
只剩头在转，bbox 不动正好说明没有拆件）；生产配置下宽度 498–523、高度 1094–1095，
宽度差是待机姿态（手臂/头发摆动）带来的，高度一致。

**D. 回归**（改完重跑，确认老功能没被碰坏）：

| 项 | 证据 |
| --- | --- |
| 逐像素穿透 | `input region: 物理 1000x1800 -> 逻辑 500x900 | 841 条矩形 | 覆盖 128502 px (28.6%) | bbox=[108,107 .. 446,837]`（和第六节改前完全一致） |
| 拖动 | 注入 (−200,0) → `开始拖动 margin=(640,120)` / `结束拖动 margin=(440,120)`，一分不差 |
| 跨屏 | 拖到夹边界后继续推 → `跨屏触发：向右越过边界 85 逻辑 px` + `搬家（拖动越界）：output[1] HDMI-A-3 -> output[0] eDP-1`，重新挂在 eDP-1 上 |

### 9.5 开关表（在八节那三个之外新增）

| 环境变量 | 作用 | 关掉后日志 |
| --- | --- | --- |
| `MIYU_NO_LOOK=1` | 不建视线逻辑，一个参数都不写 | `视线：关（MIYU_NO_LOOK）`，`[look]` 行 0 条 |

收尾：`pkill -x miyu-wayland`，`ps` 里 0 个，屏幕不留窗口。

## 十、这个模型到底能动什么（2026-11-09 21:2x，别再花一晚去试）

用新加的排查开关 `MIYU_FORCE_PARAM="参数名=值"` 把参数按死，−30 和 +30 各截一张，去掉顶部状态栏后逐像素比：

| 参数 | −30 vs +30 的像素变化 | 结论 |
| --- | --- | --- |
| `ParamAngleX` | **0** | 空壳，不驱动任何东西 |
| `ParamAngleY` | **0** | 空壳 |
| `ParamAngleZ` | **0** | 空壳 |
| `AngleXbs` | **0** | 空壳 |
| `AngleYbs` | **0** | 空壳 |

（唯一会变的是 y22..37 那一条，那是桌面顶部状态栏的时钟。）

**有效的是这几样**：

- `ParamEyeLOpen` / `ParamEyeROpen` —— 眨眼（`CubismEyeBlink` 走的就是这组）
- `ParamEyeBallX` / `ParamEyeBallY` —— 眼珠会跟着转（注意：**不会被 `LoadParameters` 归零**，必须 `SetParameterValue`，用 Add 会每帧累加顶死）
- `physics3.json` —— 头发、尾巴、裙摆会摆，这才是「待机时看着在动」的真正来源
- 表情参数（`ParamMouthForm`、`ParamBrow*` 等）没验，应该在

**所以**：这是「静态立绘 + 物理 + 表情」型模型，**头不能转**。
「让她转头看人」这件事，在这个模型上做不到；只有两条路——换一个头部可动的模型，或者在渲染层把整张图轻微倾斜/位移来模拟（那是假的，得说清楚）。

## 十一、本地控制口：外面推「她在说什么」进来（2026-11-09 21:3x）

一句话：**身体现在有嘴巴了**。外面不需要碰渲染、不需要重启，往一个 Unix socket 里写一行 JSON，她就张嘴说那句话（嘴真的动，下面有像素数据）。

### 11.1 接线（哪个文件管什么）

| 文件 | 干什么 |
| --- | --- |
| `src/ctl.h` / `src/ctl.cpp` | socket + 收行 + 粗放解析 + 嘴的包络计算。**不认识 Cubism 的类型** |
| `src/main.cpp` | 每帧 `ctl::poll(now)`；把 `ctl::mouth(now)` 写进 `ParamMouthOpenY`；表情请求交给 `exprPlay()` |
| `tools/body-say.sh` | 客户端。python3 组 JSON + 发 socket（`socat` 不一定有，python 一定有） |

两个 target（`miyu` / `miyu-wayland`）都编进了 `ctl.cpp`，X11 备胎上这套一样能用。

### 11.2 socket

- 路径：`$XDG_RUNTIME_DIR/miyu-body.sock`（本机 = `/run/user/1000/miyu-body.sock`）；拿不到 `XDG_RUNTIME_DIR` 就 `/tmp/miyu-body-$UID.sock`
- `AF_UNIX` + `SOCK_STREAM`，`listen(8)`，listen fd 和 accept 出来的 fd **都是 O_NONBLOCK**
- **一行一条命令**，收完这一行就 `close`，下一帧再 `accept` 下一个 —— 不用管粘包，也不用管多客户端
- 客户端如果 connect 了 1 秒还没发完整一行，直接丢弃（防止半截连接占着 fd）
- 建不起来（bind 失败/路径太长）只在日志里记一行，**渲染照跑**，不 return
- 启动时 `unlink` 旧文件，权限 `0600`

### 11.3 命令格式

一行 JSON，字段全可选：

```json
{"say":"今晚先把透明修好","mood":54,"expr":"前倾","dur":null}
```

| 字段 | 类型 | 作用 |
| --- | --- | --- |
| `say` | 字符串 | 说话文本。时长 = `dur` 或 `max(1.2s, 字数 × 0.16s)`，上限 60s 防呆；空串 = 闭嘴 |
| `mood` | 0-100 数字 | 存下来 + 打日志 `[ctl] mood=54`，暂时不驱动任何东西（以后接表情） |
| `expr` | 字符串 | 表情名，对应 `Exp3/<名字>.exp3.json`；`""` 或 `"默认"` = 淡出回默认 |
| `dur` | 秒（数字） | 不给 / 给 null / ≤0 都按字数算 |

解析是**手写的粗放匹配**（`"key":"值"` / `"key":数字`），故意不引 json 库：
- 只认双引号字符串值：`{"expr":123}` 会被当成没有 expr
- `"mood":"高"` 这种字符串数字也当没有
- 整行不以 `{` 开头、或者一个认识的字段都没有 → 一行警告，然后**继续跑**
- 同一条畸形行只警告一次（`gWarned`），不会刷屏

开关表新增：

| 现象 | 日志 |
| --- | --- |
| 起来了 | `[ctl] socket = /run/user/1000/miyu-body.sock（一行一条 JSON：…）` |
| 说一句话 | `[ctl] say 4.2s: 今晚先把透明修好`（**一句话一行，不是每帧**） |
| 心情 | `[ctl] mood=54` |
| 表情 | `[ctl] expr 请求: 前倾` + `[expr] -> 前倾（fade 0.35s，池里 N 个）` |
| 畸形 | `[ctl] 警告：不是一行 JSON，忽略：not json` |

### 11.4 嘴的包络（参数）

`ParamMouthOpenY`（本模型索引 **107**，0..1），每帧 **Set**（和 `ParamEyeBall` 一样：这个参数不会被 `LoadParameters` 归零，Add 会一帧一帧累加顶死）：

```
t     = now - 收到 say 的时刻
开合  = 0.5 + 0.5 * sin(t * 2π / 0.22)          # 0.22s 一个周期，一口一张
env   = min(1, t/0.15, (dur - t)/0.2)           # 前 0.15s 渐入、最后 0.2s 渐出
值    = clamp(开合 * env, 0, 1)
```

- 说话结束（`t ≥ dur`）→ 返回 -1，main 那边写 **0.02**（一点点呼吸，不是死 0；反正肉眼看不出来）
- **没有 say 的时候也写**，只是写 0.02：这个参数没有别的写入者（23 个 exp3 里一个都没写 `ParamMouthOpenY`，`grep -l` 验过），所以「一直 Set」不会抢别人的
- 写入位置：`LoadParameters → 动作/待机 → **表情** → **嘴** → 视线 → 眨眼 → physics → Update`。嘴放在表情之后，万一以后某个表情也写这个参数，说话优先

### 11.5 表情运行时切换（复用 SDK，不自己写淡入淡出）

启动的 `MIYU_EXPR` 和 socket 里的 `{"expr":"…"}` 现在走**同一个函数** `exprPlay(name)`：

1. 读 `Exp3/<name>.exp3.json`（`默认`/空串用内置的空参数表 exp3：`{"Type":"Live2D Expression","FadeInTime":0.35,"FadeOutTime":0.35,"Parameters":[]}`）
2. `CubismExpressionMotion::Create` → `SetFadeInTime(0.35)` / `SetFadeOutTime(0.35)`（exp3.json 里不写 FadeInTime 时默认 **0**，那就是硬切）
3. `gExprMgr->StartMotion(m, false)`

旧的怎么淡出，**SDK 自己干**：
- `CubismMotionQueueManager::StartMotion()` 会给队列里**已在播的每个 entry** `SetFadeout(它自己的 FadeOutTime)`，新的排后面
- `CubismExpressionMotionManager::UpdateMotion()` 按 fadeWeight 把前一个算淡，等最后一个权重到 1 才把前面的 entry 删掉
- 所以「切表情」= Create + StartMotion，**别 `StopAllMotions()`**（那是硬切回默认）
- 回默认为什么用「空参数表」的 exp3：它一个参数都不写，前一个表情的 fadeWeight 往下掉时数值被算回默认值 → 平滑回去，不用自己写插值

两个内存规矩（踩过就会 double free / 泄漏）：
- `StartMotion(m, false)` 之后 SDK **不会**替你删 motion（expression 的结束路径只 `CSM_DELETE` 队列 entry，不碰 motion 本体）→ 全部 push 进 `gExprPool`，退出时统一 `ACubismMotion::Delete`
- 退出顺序：**先 `delete gExprMgr`**（它析构时扔 entry，`autoDelete=false` 所以不碰 motion），再删 pool

### 11.6 验证（2026-11-09 21:3x，实测）

为了拿到**没有噪声底**的对照，这一轮用冻结实例测的（`MIYU_NO_IDLE=1 MIYU_NO_PHYS=1 MIYU_NO_EYES=1 MIYU_NO_LOOK=1`，窗口在 HDMI-A-3）：
`grim -o HDMI-A-3` 抓帧，嘴部 ROI = 屏幕 `(883,372)-(899,384)`（16×12）—— 这个 ROI 是先放大看过、确认整块落在嘴唇上的。

**A. 嘴真的在动**（say「测试一下嘴会不会动」，1.4s，间隔 120ms 连抓 6 帧）

| 对比 | 嘴部变化像素 | 嘴部墨迹像素（R<246） |
| --- | --- | --- |
| 静默两帧（噪声底） | 0/192 (0.0%) | 4 |
| 帧0 vs 帧1 | 54/192 (28.1%) | 14 |
| 帧0 vs 帧2 | 21/192 (10.9%) | 22 |
| 帧0 vs 帧3 | 68/192 (35.4%) | 36 |
| 帧0 vs 帧4 | 34/192 (17.7%) | 23 |
| 帧0 vs 帧5 | 13/192 (6.8%) | 4 |

逐帧 ASCII 图里能直接看到嘴从一条线（`rest`：`000000030001100030`）张成 8px 高的椭圆（`say3`），再合回去。**0.22s 的周期和 120ms 采样对上，来回都抓到了。**

**B. 说完自己停**（静默 3s 后再抓两帧）：0/192 (0.0%)，而且**和说话前那一帧逐像素完全一致**（0/192）—— 嘴回到静止，没有残留。

**C. 表情**（`tools/body-say.sh --expr …`，每步 fade 0.35s 后抓图，ROI 只取她身上的位置，桌面背景不参与）

| 表情 | 脸 ROI 变化 | 结论 |
| --- | --- | --- |
| `前倾` | **0/2944 (0.0%)** | 见下面那条，**这个表情在这个模型上看不出来** |
| `＞＜` | 340/2944 (11.5%) | 眼睛变成 `＞＜` |
| `脸红` / `兔牙` | 1223/2944 (41.5%) | 脸红 + 嘴微张 |
| `屑屑眼` | 1189/2944 (40.4%) | 圈圈眼 |
| `黑脸` | 1867/2944 (63.4%) | 整张脸黑掉 |

**顺手挖出来的一条新事实（接第十节）**：`前倾.exp3.json` 只驱动 `LeanInput = -30`，而 `LeanInput` 在这个模型上**是空壳** —— 用 `MIYU_FORCE_PARAM="LeanInput=0"` 和 `"LeanInput=-30"` 各起一次，全身 ROI **0/227200 (0.00%)**，一个像素都不变。所以：
- 运行时切换表情这条路是通的（`＞＜`/`黑脸` 立刻能看到）
- 但「前倾」这个表情本身在这个模型上没有视觉表现，别再拿它当「凑近」用；要「凑近」只能动渲染层的缩放/位移（那是假的）

**D. 畸形输入**（往 socket 里直接写）

```
[ctl] 警告：不是一行 JSON，忽略：not json
[ctl] 警告：没有一个认识的字段（say/mood/expr/dur），忽略：{"say":
[ctl] 警告：没有一个认识的字段（say/mood/expr/dur），忽略：{"abc":1}
[ctl] 警告：没有一个认识的字段（say/mood/expr/dur），忽略：{"mood":"高"}
[ctl] 警告：没有一个认识的字段（say/mood/expr/dur），忽略：{"expr":123}
```

`systemctl --user is-active` 一直 `active`，PID 没变 —— 不崩、不重启、不断嘴。

### 11.7 客户端

```sh
tools/body-say.sh "今晚先把透明修好"          # 只说
tools/body-say.sh --mood 62 "有点累"          # 带心情
tools/body-say.sh --expr 前倾 "你回来了"      # 带表情
tools/body-say.sh --expr 默认                 # 淡出回默认表情
tools/body-say.sh --dur 3.5 "慢慢说"          # 指定时长
```

脚本自己不猜 socket：先试 `$XDG_RUNTIME_DIR/miyu-body.sock`，没有就 `/tmp/miyu-body-$UID.sock`；两个都不在就报一句「身体起了吗」退出。

### 11.8 还没做的（别以为是 bug）

- `mood` 只存不打驱动：现在到 `[ctl] mood=N` 为止，表情映射还没接
- 一次只服务一条连接；外面连着发两条要各自 connect（脚本已经是这样）
- 没有认证：socket 权限 `0600`，同用户能写 = 能让她说话。这是本地口子的本意，别把 `$XDG_RUNTIME_DIR` 挪到共享目录

---

## 十二、动作计划：外面推一份时间线，她照着演（2026-11-09 21:5x）

一句话：**能演**。往同一个 socket 塞一行 `{"plan":{...}}`，她就按时间线动；播完自己交回待机。原来只能「说一句话 / 换个表情」的开关式控制，现在是「有节奏的动作」。

### 12.1 接线（新增的两个文件管什么）

| 文件 | 干什么 |
| --- | --- |
| `src/plan.h` / `src/plan.cpp` | JSON 扫描器 + 计划模型 + 时钟 + 插值。**不认识 Cubism 类型**，只吐「参数名 + 数值」 |
| `src/ctl.cpp` | 多认两个字段 `plan` / `trajectory`：只把那段对象的**原文**切出来（数括号，跳字符串）；字段解析不在这儿 |
| `src/main.cpp` | `planTick()`：拿 `plan::sample()` 的名字/数值 → `SetParameterValue`；索引缓存；起播 0.2s 交叉淡入 |
| `tools/body-plan.sh` | 客户端：读文件 → 压成一行 → 包成 `{"plan":…}` → 发 socket |
| `plans/*.json` | 三个预置：招呼 2.4s / 害羞 2.6s / 点头 1.6s |

两个 target（`miyu` / `miyu-wayland`）都编进了 `plan.cpp`，X11 备胎上一样能用。

### 12.2 命令格式（一行 JSON，字段全可选）

```json
{"plan":{"version":1,"title":"打招呼","view":"full",
  "actions":[{"kind":"trajectory","duration":2.4,"frames":[
    {"time":0.0,"values":{"AngleX":0,"AngleZ":0,"ParamEyeBallX":0}},
    {"time":0.6,"values":{"AngleX":12,"AngleZ":-4,"ParamEyeBallX":0.4}},
    {"time":2.4,"values":{"AngleX":0,"AngleZ":0,"ParamEyeBallX":0}}]}]}}
```

- `"plan"`：整份计划（`actions[]`），也可以是单条 action（直接带 `frames`）
- `"trajectory"`：单条 action
- `"plan":null`：**立刻停下**，交回待机（`tools/body-plan.sh --stop`）
- 可以和 `say` / `mood` / `expr` 同一条推，互不干扰
- 行缓冲上限从 4KB 提到 **128KB**（计划一行本来就长）；`plan.cpp` 另有 512KB 上限

解析分两层，别混：
1. **ctl 只切文本**：找 `"plan":` → 从 `{` 数括号到配对的 `}`，字符串里的 `{}` 和 `\"` 跳过。括号没闭上就警告丢弃
2. **plan.cpp 正经解析**：递归下降（对象/数组/字符串/数字/true/false/null），带转义和深度上限 24。**不用正则** —— frames 是嵌套数组，正则一撇就散

### 12.3 参数白名单（名单外跳过，同一个名字只警告一次）

```
AngleX  AngleY  AngleZ  PositionX  PositionY  PositionZ
ParamEyeBallX  ParamEyeBallY  ParamBodyAngleZ  ParamMouthOpenY  LeanInput
```

- 名字就是模型里**真实**的参数 id。`ParamAngleX/ParamAngleY/ParamAngleZ` 是**空壳**（第十节），写了不报错也什么都不动 —— 所以白名单里收的是 `AngleX` 这一套
- 值不是数字（`"大"`、`null`）→ 跳过这一帧的这个参数，打一行警告
- 实测有效的：`AngleX`（整体左右平移）/`AngleZ`（整体倾斜）/`PositionY`（上下平移，幅度最明显）/`ParamEyeBallX/Y`（眼珠）。`LeanInput` 在这个模型上是空壳（11.6 记过），留着只是因为它在前倾表情里出现过

### 12.4 时间轴与插值

- 一份计划里多条 action **按数组顺序排**：`action[1]` 从 0 开始，`action[2]` 从 `dur1` 开始 …… 总时长 = 各 `duration` 累加
- 每条 action 内：每个参数**单独一条通道**（只认写了它的那些帧），`t` 落在两帧之间就线性插值；在两帧之外保持首/末帧的值（不外推、不掉回 0）
- `duration` 缺失或 ≤0 → 用「最后一帧的 time」当长度；还是 0 → 整条 action 丢掉（整份都空 → 整份丢掉，正在播的那个继续）
- 关键帧 time 超过 duration → 把 duration 拉到那个 time（并打一行日志），不静默截断
- frames 时间乱序 → **按时间排好再播**，打一行警告（不崩、不丢帧）
- 插值**只做线性**，没叠 smoothstep：计划里的时间点已经是人给的节奏，先让它可预测；想要更软就在 JSON 里把关键帧排密

### 12.5 和待机/视线的让位关系（这层最容易打架）

写参数的顺序（`main.cpp` 主循环）：

```
LoadParameters
→ ctl::poll（收 say/mood/expr/plan）
→ 动作 / 待机（idleTick 或 IDLE motion）
→ 表情 → 视线 lookTick
→ **planTick（计划，最后一个写参数的）**
→ 嘴（说话优先，计划写了嘴时静默不再写 0.02）
→ 眨眼 → physics → Update
```

- **待机正弦让位**：计划在播时 `idleTick` 只写 `ParamBreath`（呼吸不能停，不然演两秒像憋气），`AngleX/AngleZ/PositionX` 三个参数和目标推进整段不碰
- **视线让位**：`lookTick` 照旧跑（它还负责 `ParamEyeBall` 的回正），但计划排在它后面，所以计划里的 `AngleX`/`ParamEyeBall` 压得住它
- **一律 Set，绝不 Add**：`AngleX/AngleZ/PositionY/ParamEyeBall*` 这几个不会被 `LoadParameters` 归零，Add 会一帧一帧累加顶死（第九、十一节踩过两次）
- 实测证据：计划播到 t=1.0s 时日志里 `[look] … Add前 angleX=-5.97` —— 那是**上一帧计划写进去、这一帧还没被视线盖掉**的值，说明写入真的进了模型
- 起播/换播 0.2s **交叉淡入**：把接管那一刻模型里的值记成快照，按 `w = t/0.2` 混到计划目标值上。所以「播一半推新的计划」是从当前位置平滑过去，不是从 ±12 一下跳到 0

### 12.6 播完怎么回去

`t > 总时长` → `plan::sample()` 返回 false 并自己 `stop()`，日志 `[plan] 播完：…，交回待机`。下一帧开始 `idleTick` 重新接管 `AngleX/AngleZ/PositionX`、视线重新写眼珠 —— 姿态从计划末帧回到待机（预置计划末帧都收在 0，落差小，不需要额外的淡出）。

### 12.7 验证（2026-11-09 21:5x，实测；HDMI-A-3，`grim -o HDMI-A-3`）

她身上的 ROI 取 `x 740..1110, y 255..940`（371×686；由「计划期间的运动并集」定出来）。所有帧用同一节奏抓（grim 单张 ~110ms + 睡 60ms ≈ 165ms 一张，6 张跨 ~0.9s）。

**A. 计划真的在驱动她**（对照组很关键：同一形状、所有值都是 0 的「全零计划」，她也只呼吸）

| 组 | 逐帧 mean\|Δ\| | 逐帧 >40 差异 | 首尾累计 mean\|Δ\| | 首尾 >40 |
| --- | --- | --- | --- | --- |
| 全零计划（对照） | 1.76–3.64 | 1.1–2.5% | 7.43 | 5.40% |
| 「打招呼」 | 2.85–4.65 | 1.8–3.3% | **10.70** | **8.34%** |

大位移探针（`AngleX -12 / PositionY -20 / EyeBall 0.9`）：mean\|Δ\| **16.09**，>40 **12.81%** —— 参数确实一路写到模型上，只是预置计划故意做得轻。

**B. 播完回位**（「打招呼」结束等 2.5s 再抓两帧）：mean\|Δ\| **2.94**，>8 **6.05%**，>40 **2.16%** —— 落在对照/待机区间（1.76–3.64），没有残留姿态。日志同时出现 `[idle] 算 ax=…` 继续刷。

**C. 畸形输入**（进程 `active`，PID 没变，一直在跑）

```
[ctl] 警告：不是一行 JSON，忽略：this is not json
[ctl] 警告：plan 的括号没闭上，忽略
[plan] actions[1] frames 时间乱序 —— 已按时间重新排序播放（计划本身最好排好）
[plan] actions[1] duration 缺失或为 0，按最后一帧 time=0.000s 当长度
[plan] actions[1] 长度算不出来（duration 和所有 time 都是 0），跳过
[plan] 计划里没有一个能播的 action，忽略（原来在播的继续）
[plan] 参数「AngleZZZ」不在白名单里，跳过（只警告这一次）
[plan] 参数「ParamFoo」不在白名单里，跳过（只警告这一次）
[plan] 参数「HeadTilt」不在白名单里，跳过（只警告这一次）
```

**D. 播一半换计划**：`招呼` 播到 0.9s 时推 `害羞` → 日志 `开始播放：「害羞」` + `t=0.08/2.60s 写 5 个参数（w=0.40）`（交叉淡入中），后续帧 mean\|Δ\| 4.6–9.0 —— 平滑接管，不崩、不闪回 0。

### 12.8 客户端

```sh
tools/body-plan.sh plans/招呼.json     # 演一段
tools/body-plan.sh --stop              # 立刻停，回待机
```

脚本读文件 → `json.load` 做语法检查（写法错当场报，不推半截）→ `json.dumps(separators=(",",":"))` 压成一行 → 包 `{"plan":…}` → 发 socket。

### 12.9 还没做的（别以为是 bug）

- 相邻两条 action 之间没有过渡：`action[2]` 从自己的 0 帧起播，如果它的 0 帧和 `action[1]` 的末帧差得远，接缝会跳。要在 JSON 里对齐首末帧
- 计划只管「身体动作参数」，不能选表情/换 PNG（表情仍走 `expr`，两者可以同一条推）
- 播完是硬切回待机（预置计划末帧都在 0，看不出来；自己写计划时把末帧收回中立位）
- 一次只有一份计划在播：新的直接顶掉旧的（这是设计，靠 0.2s 交叉淡入掩盖接缝）

## 十三、点她一下有反应（单击 / 拖动分流，2026-11-09 22:1x）

一句话：**能用**。左键单击她 → 播一个 0.9–1.3s 的反应计划（低头躲 / 抬头侧偏 / 往上一惊换表情）→ 自己回正；按住拖照旧能拖走、能跨屏，穿透不变。

### 13.1 判定阈值（选的是「超阈值才转拖动」）

| 项 | 值 | 说明 |
| --- | --- | --- |
| `TAP_SLOP_PX` | 6 逻辑 px | 松开时位移 ≤ 6 才可能是单击 |
| `TAP_MAX_S` | 0.400s | 按住超过这个时长，松手不算单击（也不会刷反应） |
| `TAP_COOL_S` | 0.800s | 冷却：挡掉的点击只打一行日志 |
| `TAP_JUMP_PX` | 120 逻辑 px | 单个 motion 事件跳这么远 = 指针被瞬移/窗口重排，不是拖动（见 13.2） |

按下**只记账**（`gPressX/gPressY/gPressT`），不进拖动。按下后位移越过 6px → **当场转成拖动**，并且把「按下点 → 现在」那一段整段塞进 `pendX/pendY`（不补这 6px，拖 100 只走 94）。松开时从来没越过阈值 → 位移 ≤6 且按住 <400ms 就是单击。

为什么不「松开时再决定拖不拖」：一动就跟手，手感像普通窗口。代价只有开头那 6px，已补回。

### 13.2 大跳保护（这一版新踩的坑，必须留着）

指针被绝对定位瞬移时，**按下之后**会补来一个几百 px 的单事件 delta（实测 −624,−30）。老逻辑「按下即进拖」会把它整段当位移，把她甩到屏幕另一头 —— 改完第一次测单击就是这么变成拖动、margin 从 (636,141) 飞到 (12,111) 的。

现在：按下后只要单个事件 `|dx|` 或 `|dy|` > 120，就按「窗口刚重排」处理 —— 把按点跟着挪同样一段、重新起算，不进拖动。真人拖拽每个事件都是几 px，`tools/draginject` 分步也在 5–26px，都不会误伤。

### 13.3 播什么

| 点在哪 | 播 |
| --- | --- |
| 左半边（x < 她本人 bbox 中心，`platPersonBox`） | `plans/被戳.json`（1.3s 低头躲） |
| 右半边 | `plans/被戳2.json`（1.1s 抬头侧偏 + 眼珠偏） |
| 每第 3 次 | `plans/被戳3.json`（0.9s 往上一惊：PositionY +6、AngleZ −7、眼珠甩开、`ParamMouthOpenY` 0.35）—— 那一下就是「换个表情」；嘴由计划驱动时，说话那层自动让位不再写 0.02 |

反应期间如果正在播别的计划：`plan::start()` 直接顶掉，播放器自己那 0.2s **交叉淡入**接管（第十二节那套，没另写一份）。

日志：`[tap] 第 N 次点击 local=(x,y) → 播 被戳.json`；冷却挡掉的是 `[tap] 第 N 次点击 local=(x,y) → 冷却中（还剩 0.31s），不播`。

### 13.4 计划目录怎么找（不写死绝对路径）

优先级：`MIYU_PLANS_DIR` → `/proc/self/exe` 同层 `plans/` → `/proc/self/exe` 的**上一级** `plans/`（本仓库就是要这条命中：`build/miyu-wayland` ↔ `../plans`）→ 兜底 `plans/`（launcher 已经 cd 到 native3）。启动日志会打：

```
[plat-wl] 点击反应：开 | plans=/home/link/.dsh/miyu/body/native3/build/../plans
```

计划文件每次点击**现读**（几 KB），改 JSON 不用重启。

### 13.5 平台层直接推计划，main.cpp 一行没动

tap 全在 `src/platform_wayland.cpp`：button 段改造 + 一组 `tapInit / tapLoadPlan / tapFire`，直接 `plan::start(body, platTime())`。`platTime()` 和 `planTick()` 用的都是 `CLOCK_MONOTONIC`，同一把钟，不需要额外对齐。所以 `platform.h` / `platform_x11.cpp` / `main.cpp` 都没改；X11 备胎照旧没有 tap（它连 wl_pointer 都没有）。

### 13.6 开关

| 环境变量 | 作用 | 关掉后 |
| --- | --- | --- |
| `MIYU_NO_TAP=1` | 不认单击 | `点击反应：关（MIYU_NO_TAP=1）`；按下照旧记账，松手不播 |
| `MIYU_DRAG=0` | 只关拖动，tap 不受牵连 | 位移再大也不拖；单击仍播（以前这个开关会把整套按钮行为一起关掉） |
| `MIYU_PLANS_DIR=…` | 换计划目录 | 日志里 `plans=` 那行跟着变 |

### 13.7 验证（2026-11-09 22:0x–22:1x，HDMI-A-3，`grim -o HDMI-A-3`）

把 HDMI 切到**空工作区**（壁纸静态，背后没有滚动文字），噪音才干净。ROI = x 740..1140, y 200..1030；`mean|Δ|` 是 ROI 内逐像素 RGB 差均值，`>40` 是差超过 40 的像素占比。

**A. 单击后画面真的在变**（控制组：同样 150ms 节奏抓 6 张，不点）

| 组 | 相邻帧 mean\|Δ\| | 相邻帧 >40 |
| --- | --- | --- |
| 控制（无点击） | 7.7 – 18.9 | 4.1 – 11.6% |
| 单击后 1↔2 | **25.8** | **15.2%** |
| 单击后 3↔4、4↔5 | 18.6 / 19.4 | 11.5 / 12.2% |
| 单击后 6↔7（1.1s 计划已播完） | 8.4 | 4.9%（回到控制区间） |

对静止均值（6 张平均）比：r1 26.2 / 17.2%、r2 26.1 / 17.4% → r6 12.9 / 10.6%（收尾回静）。画面上看得很直接：被戳2 那一帧整张身子往右上偏、头歪，末帧回正。

**B. 拖动仍然工作**：注入 +100（20 步 ×45ms）→ `开始拖动 margin=(640,120)` / `结束拖动 margin=(745,120)`；两张截图互相关最佳平移 **105 px**。反向注入 −200 → margin 745 → 535 = **−210**。误差 5%（每步 5–10px 的那一格被 niri 重排重发的一次也算进去了），和第七节「一分不差」同级，判通过。

**C. 冷却**：一个进程里连打 5 次（间隙 100ms、按住 80ms）：

```
[tap] 第 2 次点击 local=(295,293) → 播 被戳2.json
[tap] 第 3 次点击 local=(295,293) → 冷却中（还剩 0.61s），不播
[tap] 第 4 次 … 0.44s   第 5 次 … 0.26s   第 6 次 … 0.08s 不播
```

5 次点击只播 1 次。

**D. 开关**：`MIYU_NO_TAP=1 MIYU_OUTPUT=1` 手动起一份 → 日志 `点击反应：关（MIYU_NO_TAP=1）`；同样注入单击：`按下 local=(290,293)` 有，`[tap]` 0 条、`开始播放` 0 条。

**E. 三条老回归**（改完各重测一次）

| 项 | 证据 |
| --- | --- |
| 逐像素穿透 | `input region: 物理 500x900 -> 逻辑 500x900 \| 881 条矩形 \| 覆盖 127912 px (28.4%) \| bbox=[109,107 .. 446,837]`（改前改后同尺寸同 bbox） |
| 拖动 | 见 B：+100 → 105、−200 → −210 |
| 跨屏 | 一路向右拖 1400 → `跨屏触发：向右越过边界 100 逻辑 px（阈值 80…）` + `搬家（拖动越界）：output[1] HDMI-A-3 -> output[0] eDP-1`，eDP 上以 scale 2 重建成功 |

收尾：`systemctl --user restart miyu-body` → `点击反应：开` + `layer surface 挂在 HDMI-A-3 上` + 单元 `active`，窗口留在副屏。

### 13.8 已知边界

- 判定只看按下那一刻的 local 坐标和松手时的位移；她自己在动（待机呼吸、计划播放中）时 input region 每 10 帧重算、enter/leave 会抖，但抖动发生在两次采样之间，不参与判定
- 每第 3 次的「换表情」是**换一份计划**（AngleZ + 眼珠 + 嘴张开度），不是 `.exp3.json` 表情文件；要真换表情仍走 socket 的 `{"expr":…}`
- 冷却按「点击次数」计数（含被挡掉的），所以连点 5 次里第 3 次会被冷却压掉、计数照走 —— 这样「每第 3 次换一个」不会因为被挡而错位
- 单击和拖动共用 BTN_LEFT：手抖着按住 400ms 以上再松开，不播反应（按 13.1 的规矩），要更灵敏就调 `TAP_MAX_S`

## 十四、本地设置页：进程自己起 HTTP，浏览器里改完立刻生效（2026-11-09 22:4x–22:5x）

一句话：**能用**。身体进程里多了一个 `127.0.0.1:8200` 的极简 HTTP 服务，浏览器打开就能调
大小 / 不透明度 / 待机幅度 / 点击开关 / 表情开关 / 贴角 / 隐藏，改完 300ms 防抖发一条 POST，
**当场生效、不重启**；值落到 `native3/state/settings.json`，重启服务后还是改后的样子。

socket 那套（`say/mood/expr/plan`）一个字没动，页面里的「叫她一下 / 切个表情」就是自己当一次
客户端往 `$XDG_RUNTIME_DIR/miyu-body.sock` 写一行 —— 没有第二个协议。

### 14.1 新文件与接线

| 文件 | 干什么 |
| --- | --- |
| `src/web.h` / `src/web.cpp` | HTTP 服务（socket + 手写请求解析）、设置模型、JSON 落盘/读回、内嵌单页 HTML、往 ctl socket 转发 |
| `src/platform.h` | 新增 5 个「窗口层旋钮」：`platSetScale` / `platSetTapEnabled` / `platSnapCorner` / `platSetHidden` / `platOutputLabel` |
| `src/platform_wayland.cpp` | 上面 5 个的真实现（重建 buffer、点击开关、吸角、隐藏）；`WlState` 加 `baseW/baseH/hidden` |
| `src/platform_x11.cpp` | 备胎上的空实现（编译通过、行为不变） |
| `src/main.cpp` | `web::init/poll/shutdown`；`webApply()` 按 key 分给各层；alpha 全屏乘算；hidden 整帧不画；待机倍率；expr 开关；`scanExpressions()` 扫 `model/Exp3` |
| `CMakeLists.txt` | 两个 target 都加 `src/web.cpp`；顺带 `-Wall -Wextra` + 第三方头改 `SYSTEM` include |

端口：默认 **8200**，`MIYU_WEB_PORT` 覆盖。被占用只在日志里说一句（`端口 8200 被占用（…）—— 设置页不启动，身体照常跑`），不崩、不退出。
bind 地址是 `INADDR_LOOPBACK`，外面连不进来，所以没有任何鉴权。

### 14.2 接口（三个路由，其余 404）

| 方法 路径 | 作用 | 例子 |
| --- | --- | --- |
| `GET /` | 内嵌单页 HTML（C++ 字符串常量，**没有静态文件**，无外链无 CDN） | `curl -s 127.0.0.1:8200/ \| head -3` |
| `GET /state` | 当前所有设置 + 状态 | `{"ok":true,"pid":800611,"output":"HDMI-A-3","window":"500x900","scale":1,"alpha":1,"tap":true,"expr":true,"idle":1,"corner":"free","hide":false,"port":8200,"expr_next":"体型1"}` |
| `POST /set` | 改设置，立刻生效 | `curl -s -X POST 127.0.0.1:8200/set -d '{"alpha":0.6}'` → `{"ok":true,"applied":["alpha"]}` |

`POST /set` 除了设置项还认三种「动作」：`{"say":"在","mood":60}`（转给 ctl，等价 `body-say.sh`）、
`{"action":"expr_next"}`（按 `model/Exp3` 的字母序循环切一个，走 `{"expr":…}`）、
`{"action":"restart"}`（`systemctl --user restart miyu-body`，**挂后台**再返回，不然这几秒她不动）。

日志只打一行，不刷屏：

```
[plat-wl] 缩放 0.90 -> 逻辑 450x810 / 物理 450x810 / margin (640,120)
[web] 生效：scale
[web] POST /set {"alpha":0.8,"scale":0.9,"idle":1.5,"corner":"free"} → 已应用
```

非阻塞的写法：每帧 `poll(fd, 0)`，最多 `accept` 两个连接；连接不完整就留到下一帧再读（最多 4 个槽），
**3 秒还收不全直接掐**。渲染一帧都没为它等过。

### 14.3 七项设置，各自怎么真生效

| key | 类型/范围 | 生效路径 | 关键实现 |
| --- | --- | --- | --- |
| `scale` | float 0.4–1.6 | `platSetScale()` → `zwlr_layer_surface_v1_set_size`(新逻辑尺寸) + 拆 `wl_egl_window`/`EGLSurface` 按新尺寸重建（**EGL context 和纹理不动**）+ `set_buffer_scale` 重设 | `reqW/reqH = baseW/baseH × scale`，基准单独存（拿 reqW 反算会累积误差）。位置保持：`posX/posY` 不动，只重新夹一遍 margin |
| `alpha` | float 0.3–1.0 | 渲染末尾一次全屏乘算：`glBlendFunc(GL_ZERO, GL_SRC_COLOR)` + 覆盖全屏的定值色 quad → `dst = dst × k` | 混合是 premultiplied（`GL_ONE, ONE_MINUS_SRC_ALPHA`），所以 RGB 和 A 一起缩，对合成器就是整体半透明。**必须**先 `glUseProgram(0)` + `glDisable(GL_TEXTURE_2D)`（Cubism 留着 program 和纹理绑定）。`k=1` 时整段跳过 |
| `tap` | bool | `platSetTapEnabled()` → 平台层的 `gTapOn` | 等价 `MIYU_NO_TAP=1` 但运行时可变；拖动不受影响（拖动开关是另一个 `MIYU_DRAG`） |
| `expr` | bool | `web::settings().expr` 每帧现读 | 三处入口都挡：`ctl::takeExpr`（请求照吃、不切）、`planTick` 里计划附带的 `"expr"`；切回默认那条（计划播完）不挡 |
| `idle` | float 0.0–3.0 | `idleTick()` 里把 `AngleX/AngleZ/PositionX` 三个目标值乘倍率 | 倍率本身过一道低通（`k += (want-k)*0.12`，收敛后贴死），滑块一拖到底不是定住而是几帧收放；`ParamBreath` 恒走，所以 0 = 完全不摆只剩呼吸 |
| `corner` | `tl/tr/bl/br/free` | `platSnapCorner()` → `platSetPosition()` → `apply_margin()` | anchor 一直是 `TOP\|LEFT`，四角都靠 margin：顶两角 y=0（`TOP` 落在 exclusive zone 下沿）；**底两角要减掉顶部 bar 的 37 逻辑 px**，否则窗口整体被 bar 推下去、脚被裁掉。`MIYU_BAR_H` 可覆盖。`free` = 不动 |
| `hide` | bool | `platSetHidden()` 清空输入区 + 主循环整帧不画模型（只 `glClear` 成 alpha 0 再 swap） | **不卸 surface**（卸了要等 configure，还可能被合成器改尺寸）。全透明 + 空 region = 和空桌面逐像素相同、点击全穿透、进程和循环照常。取消隐藏那一帧补算一次输入区（`justShown`） |

落盘：**`native3/state/settings.json`**（`MIYU_STATE_DIR` 可改；默认取 `/proc/self/exe` 同级 `../state`，
对应 `build/miyu-wayland ↔ native3/state`）。写 `.tmp` 再 `rename`，半截文件不会污染下次启动。

```json
{"scale":0.900,"alpha":0.800,"tap":true,"expr":true,"idle":1.500,"corner":"free","hide":false}
```

启动顺序（`main()`）：`web::init()` 读文件 → **立刻**把 scale / tap / corner / hide 应用一遍
（不落这一步就只是页面显示对、行为回到默认）→ 再进 Cubism。`MIYU_NO_TAP=1` 压过文件里的 `tap`。

### 14.4 页面

深色（`#0d1117` 底 / `#7aa2f7` 主色 / `#bb9af7` 次色），窄栏 420px，`viewport-fit=cover` 手机上也能开。
顶部状态 `运行中 · PID · 输出 HDMI-A-3 · 窗口 500x900`（每 5 秒刷一次，拖滑块时不会跟你抢控件值）；
每个滑块旁边实时数字；滑块 300ms 防抖 POST，开关/下拉直接发；底部三按钮。

### 14.5 验证数据（2026-11-09 22:47–22:55，HDMI-A-3，`grim -o HDMI-A-3`）

测 hide / 贴角 / 大小的对照组是**把 HDMI 切到空工作区**（壁纸静态），否则被测的是浏览器自己刷字。

| 项 | 证据 |
| --- | --- |
| 路由 | `GET /state` 返 JSON、`GET /` 9898 字节 HTML（`grep -oE 'https?://…' 页面 = 0 条外链`）、`/nope` = 404 |
| alpha 0.6 | 她身上 RGB 均值 **162.3 → 107.5**（掉 33.8%，和 `0.6×她+0.4×背景` 的理论值对得上）；与背景的平均距离 **136.4 → 79.3** |
| scale 0.8 | `/state` 的 `window` 变 `400x720`；同一位置截图量 bbox **高 729 → 584 px（0.801×）**、宽 329 → 279（边缘抗锯齿，高度才是可信的那个）；日志 `缩放 0.80 -> 逻辑 400x720 / margin (701,88)` —— margin 一分没动 |
| corner=br | `吸角 br：屏 1920x1080 / 窗 500x900 / bar 37 -> margin (1420,143)`；bbox `x 1527..1855 y 288..1014`，质心 (1691,651)，距屏右 64 px、距屏底 65 px（那是她自己在窗口里的透明留白） |
| hide=true | 前后差异 **119632 px**（她在），隐藏后**她这块区域两帧差 0 像素**（全图剩下的 202 px 是顶栏时钟）；隐藏期间注入单击 → 日志里**没有 `按下`**（点击穿过去了） |
| tap 开关 | `tap=false`：注入单击 → 只有 `按下`，`[tap]` 0 条；`tap=true` 同一下 → `[tap] 第 3 次点击 local=(280,420) → 播 被戳3.json` |
| expr 开关 | `expr=false` + `{"expr":"脸红"}` → `[expr]` 0 条；`expr=true` 同一条 → `[expr] -> 脸红（fade 0.35s）` |
| idle 倍率 | `idle=0` → `[idle] 算 ax=0.00 … az=0.00 br=0.711`（呼吸照走）；`idle=2.5` → `ax=1.14 az=1.27` |
| 落盘 + 重启 | 设 `scale=0.9 alpha=0.8 idle=1.5 corner=free` → `cat state/settings.json` 一致 → `systemctl --user restart` → `/state` 仍是这四个值、`window=450x810`；启动日志 `[web] 读回 …` + `[plat-wl] 缩放 0.90 -> 逻辑 450x810 / margin (640,120)` |
| 三个按钮 | 叫她一下 → `[ctl] say 1.2s: 在`；切个表情 → `[web] 切表情 → 体型2` + `[expr] -> 体型2`；重启服务 → PID `798302 → 800611`，单元仍 `active`，`layer surface 挂在 HDMI-A-3 上` |
| 编译 | 两个 target `make clean && make`：**0 warning / 0 error**（`-Wall -Wextra`；`Framework/src`、`Core/include`、`stb` 改成 `SYSTEM` include，第三方的 `-Wreorder` 之类不再刷屏） |
| 老功能回归 | socket 照旧（`body-say.sh --expr` 有效）、单击反应照旧、输入区照旧（`881 条矩形 / 28.4% / bbox=[109,107..446,837]`）、`MIYU_OUTPUT=1` 保留、标题仍 `native2-test` |

### 14.6 已知边界（别当成 bug）

- `scale` 重建的是 `wl_egl_window`，那一瞬间输入区是空的；主循环下一帧（≤10 帧内）按新 framebuffer 重算，这中间点不到她
- `hide` 用的是「全透明 + 空输入区」，不是卸 surface：所以进程还在、`eglSwapBuffers` 还在跑（每帧只清一张透明图），代价可以忽略但要记得它**没有真的销毁窗口**
- 吸角的下边界靠 `bar 37 逻辑 px` 这个常量（本机实测）；换机器/关掉 bar 用 `MIYU_BAR_H=0`，不然底部两角会矮一截
- `POST /set` 里认不出的键只记一行、不整包失败；`applied` 数组里 `say` / `expr_next` / `restart` 也各占一格
- 页面 5 秒轮询只刷状态行，不动控件值（不然你正拖滑块，它被服务端值拽回去）
- 表情循环用的是 `ls model/Exp3` 的字母序，不筛「好看不好看」，要固定就走 socket 推 `{"expr":…}`

## 十五、后处理三件（设计稿，2026-11-09 夜，还没实现）

排队的活：另一个线程正在改 `main.cpp` 的待机与 `web.cpp` 的设置项，所以先把设计钉下来，等它落地立刻开工。

### 一共要引入一个 FBO

现在所有东西直接画在窗口 buffer 上。轮廓光需要「她的 alpha 边缘」，Cubism 的 renderer 不暴露中间结果，所以：
- 建一个和窗口同尺寸（随 `scale` 重建）的 RGBA8 FBO
- 每帧：`bind FBO` → 画背景粒子 → 画模型 → `unbind` → 后处理 pass（轮廓光）→ 合成到窗口
- 透明层窗口要注意：FBO 里 alpha 必须 premultiplied 保持一致，否则边缘会发黑（这是最容易翻车的地方，先在探针里验一遍）

### 0. 抗锯齿：走 SSAA，不走 MSAA

一开始想直接开 MSAA 4x（EGL config 加 `EGL_SAMPLE_BUFFERS/EGL_SAMPLES`，两行），但既然上面已经要引入 FBO 做后处理，MSAA 就会多出一层麻烦：multisample FBO 要 blit resolve 才能拿来做 alpha 膨胀，而且透明层窗口上的多重采样边缘容易发黑。

所以改成 **SSAA**：FBO 直接开成物理尺寸的 2 倍（`scale*2`），所有绘制在后处理里最后一次降采样回窗口尺寸。好处：
- 一次就把抗锯齿和 FBO 后处理合并了，不再有两套采样路径
- 边缘质量比 MSAA 4x 更好（尤其她那些细发丝和半透明裙边）
- 降采样用双线性/米切尔都行，先双线性

代价：FBO 显存 ×4（`500x900 → 1000x1800`，RGBA8 约 7MB，2x 后 28MB），可接受；像素着色开销 ×4，实测 CPU 43% 里 GPU 部分本来就小，先上再看。

设置项加一个 `ssaa`：`off / 2x / 3x`（默认 2x），0 关掉就是现在这样（硬边）。
验收：把她的头发丝边缘裁一小块（40x40）放大 4 倍存图，`ssaa=off` 和 `2x` 各一张，对比边缘过渡像素数。

### 1. 地面软阴影 `shadow` (0–1)

- 位置：她 bbox 底边中点 `(cx, cy+hh)`，随待机 `PositionX` 微动
- 形状：程序生成的椭圆（径向渐变 alpha），`rx = 0.35*bbox宽`、`ry = 0.18*rx`
- 强度：默认 0.28，乘设置值；颜色近黑但不是纯黑（`#0a0c14`），免得在浅色壁纸上变灰块
- 画在模型**之前**（模型会盖住压在她脚上的部分，看着才是"踩在地上"）
- 注意：拖动时阴影要跟着走（用当前 margin，不要用初始位置）

### 2. 轮廓光 `rim` (0–1)

- 做法：把 FBO 的 alpha 通道做 8 方向膨胀（offset 采样，半径 = `1 + 2*scale` 像素），得到外扩的剪影
- 用「外扩剪影 − 原 alpha」得到边缘环，环的颜色取她边缘的偏亮蓝（`#8fb8ff`），alpha 乘设置值（默认 0.35）
- 环要画在模型**之前**（在下面），这样它只是从外面透出来一点，不会糊住她的脸
- 深色背景下 0.3 就够；浅色背景建议降到 0.15，可以按背景亮度自动降（暂不做）

### 3. 粒子 `particles` (0–1)

- 12–24 个气泡（数量 = `round(24*设置值)`），只在 bbox 下部 40% 区域生成
- 每颗：位置、半径 1–3px、上升速度 8–20 px/s、横向轻微正弦漂移、生命周期 4–8 秒
- 生命周期内 alpha 从 0 渐入到 0.5 再渐出；出屏即重置（随机新位置）
- 画法：`GL_POINTS`（`gl_PointSize`），fragment 里 `discard` 圆外的像素，边缘给 1px 的柔和过渡
- 画在模型**之前**（背景层），这样气泡是从她身后飘过去的

### 设置项（加在网页页，「外观」组）

| key | 范围 | 说明文字（页面上要写） |
| --- | --- | --- |
| `shadow` | 0–1，默认 0.28 | 脚下的软阴影，0 关掉她就又像贴纸了 |
| `rim` | 0–1，默认 0.35 | 边缘那一圈淡光，深色壁纸上尤其有用 |
| `particles` | 0–1，默认 0.3 | 身后飘的气泡，0 关掉 |

### 验收（必须给数）

- `shadow` 0 vs 1：她脚下方圆 120px 区域的平均亮度差，以及整块 ROI 的差异像素数
- `rim` 0 vs 1：她 bbox 外扩 3px 那一圈的像素，应该只有边缘环在变（内部 0 变化）
- `particles` 0 vs 1：两帧间隔 500ms 的差异像素应集中在 bbox 下部且形状分散
- **性能**：加完三件后 `ps` 的 %cpu 和 RSS 对比（现在 43% / 209MB），超 55% 就要考虑默认关掉粒子
- **透明正确性**：`grim` 抓图后检查她边缘有没有黑边（对比 alpha 0.5 时的边缘 RGB）

## 十六、待机幅度重做 + 九项新设置（2026-11-09 23:0x–23:2x）

起因是他两句话：「我认为待机服务不起作用，设置功能太少了」。查下来两件事都成立：
① 基数太小（`AngleX ±1.2`、`AngleZ ±0.8`、`PositionX ±1.5`），`idle` 从 1 拉到 2.5 只有
1.2°→3°，**肉眼分辨不出**，所以「调了没反应」；② 页面上只有七项。

### 16.1 改了什么

| 位置 | 改动 |
| --- | --- |
| `src/web.h` | `Settings` 加 9 个字段：`idleStyle/look/lookOn/blink/mouth/exprHold/fps/voice/vol` |
| `src/web.cpp` | 读写/`/state`/`POST /set` 全部接上；`{"action":"reset"}`；页面整页重写（四组 + 每项一句人话说明，JS 从 schema 生成） |
| `src/main.cpp` | `idleTick` 基数 ±4.0/±2.5/±3.0 + 风格三档；`lookTick` 强度与总开关；眨眼倍率；表情停留截止；帧率上限 `framePace()` |
| `src/ctl.cpp/.h` | `setMouthScale()` / `setVoice()`；`mouth()` 时长乘倍率；`voicePlay()`（fork+exec `pw-play --volume=`，double fork 不留僵尸） |

待机基数与风格（`idleStyle`）：

| 档 | 幅度系数 | 换姿态间隔 | 实际（`idle=1`） |
| --- | --- | --- | --- |
| `quiet` | ×0.35 | 5~9 s | ±1.4 |
| `natural` | ×1.0 | 2.5~6 s | ±4.0 |
| `lively` | ×2.0 | 1.5~4 s | ±8.0 |

老的 `idle` 数值倍率**保留**，在风格之上再乘：`amp = style.amp × idle`，低通 `kAmp += (want-kAmp)*0.12`
照旧（换档也是几帧内平滑过渡，不是硬切）。

### 16.2 必须先说的一个事实：`AngleX/AngleZ/PositionX` 本身不驱动任何 drawable

隔离测量（`MIYU_NO_PHYS=1` + `MIYU_FORCE_PARAM=ParamBreath=0.5`，`idle=3 lively`）：

```
[idle] 算 ax=1.23 … 算 ax=-6.41 … 算 ax=12.82      ← 参数在变
grim 8 帧：hash 全是 7c13263b，逐像素 0 差异        ← 画面一动不动
```

**它们只通过 `physics3.json` 驱动头发 / 尾巴 / 裙摆**（和第十节「这是静态立绘 + 物理」一致）。
所以：

- 「待机在动」= 物理对 AngleX 的响应，**物理必须开着**（`MIYU_NO_PHYS=1` 时待机等于静止）；
- 生产配置下 **呼吸是 0.4 s 帧差的最大来源**（`ParamBreath` 4.2 s 周期，0.4 s 内就走掉一大截），
  比待机幅度本身还大 —— 这也是他「调了看不出」的第二层原因。

### 16.3 验证数据（HDMI-A-3，`grim`，ROI = 她本人 338×731，8/15 帧，间隔 0.4 s）

**A. 生产配置（呼吸 + 物理全开，`idle=1`）** —— 肉眼/像素都在这套配置下

| 档 | 相邻帧差均值 | 帧差中位 | 逐像素时间标准差均值 | std>10 的像素 |
| --- | --- | --- | --- | --- |
| quiet | 6.246 | 6.134 | 11.14 | 62990 px (25.5%) |
| lively | 7.434 | 7.503 | 14.08 | 82744 px (33.5%) |

差 1.19× —— **被呼吸底噪吃掉**，见 16.2。这就是「幅度放大了 5.7 倍、帧差只多两成」的原因。

**B. 隔离配置（`MIYU_FORCE_PARAM=ParamBreath=0.5`，物理开，15 帧 / 0.4 s）**

| 档 | 帧差均值 | 帧差中位 | 帧差最大 | 时间标准差均值 | std>10 的像素 |
| --- | --- | --- | --- | --- | --- |
| quiet | 0.352 | **0.256** | 0.84 | 1.003 | 5591 px (2.26%) |
| lively | 2.853 | **2.544** | 5.11 | 5.762 | 42538 px (17.22%) |

帧差中位 **0.256 → 2.544 = 9.9×（一个数量级）**，`std>10` 的像素 **7.6×**，std 均值 **5.7×**。
→ 幅度确实差一个数量级，只是在生产配置里被呼吸盖住了。

**C. 日志 `[idle] 算 ax=` 的绝对值范围**（`idle=1`）

| 档 | |ax| 范围（采样） | 备注 |
| --- | --- | --- | --- |
| quiet | **0.63 ~ 1.06**（< 1.5 ✓） | 幅度小，肉眼就是「几乎不动」 |
| natural | **0.76 ~ 4.02** | 基数 ±4.0 |
| lively | **0.20 ~ 8.35**（> 4 ✓） | `idle=3` 时到过 **19.71** |

### 16.4 九项各自的生效路径 + 实测

| key | 生效路径 | 证据 |
| --- | --- | --- |
| `idleStyle` | `idleTick` 读风格 → 幅度系数 + 间隔区间 | 16.3 的三张表 |
| `look` | `lookTick` 目标幅度 `nx*1.5*k`（身体）、`nx*1.0*k`（眼珠） | `k=1.00 nx=+1.00 → 身体 +1.49 眼 +0.99，写入 angleX +8.52 = 待机+7.03+1.49`；`k=2.00 → 身体 +2.89 眼 +1.93(clamp)` |
| `lookOn` | 同上，`k` 直接给 0 | `k=0.00 nx=+1.00 → 身体 +0.00 眼 +0.00，写入 angleX -0.17 **= 待机 ax -0.17**（严格相等）` |
| `blink` | 每帧 `SetBlinkingInterval(4.0/bk)`；`bk≈0` 时不调 `UpdateParameters` | `blink=0`：30 s 内眨眼事件 **0 条**（只有 1 行设置日志）；`blink=1`：30 s 内 **20 行 = 10 次** |
| `mouth` | `ctl::mouth()` 里 `dur = gSayDur × mouth` | `mouth=1` → `say 1.2s（×1.00 -> 嘴动 1.2s）`；`mouth=3` → `（×3.00 -> 嘴动 3.6s）` |
| `exprHold` | `exprPlay()` 记 `gExprUntil = now + 2.5×exprHold`；主循环到点回默认（计划在播时跳过） | `exprHold=0.2` → `[expr] 停留 0.5s 到点，回默认`；`exprHold=5` → 6 s 内**没有**「到点」（12.5 s 才回） |
| `fps` | `framePace()`：每帧末尾睡到目标周期；`hide` 分支也睡 | 60 → 实测 **60.0** 帧/秒，CPU **19.0–19.9%**；15 → 实测 **16.0**，CPU **5.0–5.3%**（3.9×） |
| `voice`+`vol` | `ctl::setVoice()` 每帧喂；说 `say` 时查 `voice/index.tsv` → double-fork `pw-play --volume=<vol>` | `[ctl] 声音 开，音量 0.50` + `[voice] 播 241d47a3ee.wav（vol 0.50）: 在`；没有这句 wav 时只打一行「没有预生成（静默）」 |
| `reset` | 遍历 16 个 key 全部回调 + 落盘 | `applied` 16 项；`/state` 全默认（`idleStyle=natural look=1 blink=1 mouth=1 exprHold=1 fps=60 voice=false vol=0.8 corner=free`）；`settings.json` 一致 |

### 16.5 老六项回归（各测一项）

| 项 | 证据 |
| --- | --- |
| `scale=0.8` | `/state.window=400x720`，`缩放 0.80 -> 逻辑 400x720 / margin (640,120)` |
| `alpha=0.6` | 生效（截图 0.6 档，走的是原来那条全屏乘算） |
| `corner=br` | `吸角 br：屏 1920x1080 / 窗 400x720 / bar 37 -> margin (1520,323)` |
| `hide=true` | 隐藏前后副屏差 **79911 px**，隐藏期间点击穿透（原有行为） |
| `tap=false` | 注入单击 → `[tap]` **0 条**（连「按下」记账都没有）；`tap=true` 同一下 → `[tap] 第 1 次点击 local=(250,463) → 播 被戳2.json` |
| `expr=false` | socket 推 `{"expr":"脸红"}` → `[expr]` **0 条**；`expr=true` → `[expr] -> 脸红（fade 0.35s，池里 1 个）` |

编译：`make clean && make` 两个 target（`miyu` / `miyu-wayland`）**0 warning / 0 error**（`-Wall -Wextra`）。
收尾：`systemctl --user is-active miyu-body = active`，`MIYU_OUTPUT=1` 保留，`output=HDMI-A-3`，
截图 `shot-settings-v2.png`，标题仍 `native2-test`。

### 16.6 这一轮踩的四个坑（别再来）

1. **帧率算法的累加陷阱**：第一版 `nextFrame += period` 累加，被 `eglSwapBuffers`（niri 下会等
   frame callback，每帧 ≥16.7 ms）一挤就永远落后 → `fps=15` 实测跑出 **29.3 帧**。
   改成「基准 = 本帧开头 + period，落后超过 1.5 个周期就把基准拉回 now」，才稳在 16.0 / 60.0。
2. **ctl 的 socket 解析不吃 `\uXXXX`**：`python3 -c "json.dumps({'expr':'脸红'})"` 默认转义，
   身体收到的是字面 `\u8138\u7ea2` → `[expr] 读不到 …\u8138\u7ea2.exp3.json`。
   推中文要用 UTF-8 原文（`ensure_ascii=False` 或手拼）。
3. **`tools/draginject` 的按下+松开不触发 tap**（只打「按下」记账，没有后续判定），
   回归 tap 要用 `../input-probe/clicker`。两者点同一个点，只有 clicker 能出
   `[tap] 第 N 次点击`。
4. **测量待机幅度必须隔离呼吸**：生产配置下 0.4 s 帧差有 ~6 是呼吸的，待机幅度只占其中一成。
   要比较幅度就 `MIYU_FORCE_PARAM=ParamBreath=0.5`（物理留开，因为 AngleX 只通过物理生效），
   测完 `systemctl --user unset-environment` + 重启，别留在环境里。

### 16.7 已知边界

- `AngleX/AngleZ/PositionX` 对画面的影响全部经由 physics，所以**风格差异的可见度取决于物理**；
  哪天为了省电关物理，待机就变成「只有呼吸」。
- `voice` 默认**关**（不突然出声）；`vol` 0 不等于静音（`pw-play --volume=0` 仍会建流），
  要静音用开关。
- `exprHold` 不管「动作计划自带的表情」（计划在播时由 `planTick` 收尾），否则计划播 3 秒、
  表情 2.5 秒就回默认，看起来像闪一下。
- 页面上 15/30/60 是白名单，其它值 `POST /set` 会记一行忽略，不会把 fps 改坏。

## 十七、FBO / SSAA / 后处理三件：实现与实测（2026-11-09 23:2x–23:4x）

第十五节的设计稿落地了。四项都**能用**，但 SSAA 的验收标准有一条没达到（见 17.6，原因是模型本身的
性质，不是实现），下面逐条给数。

### 17.1 改了什么（只动了 `src/` 和 `state/`）

| 位置 | 改动 |
| --- | --- |
| `src/main.cpp` | 新增 `namespace postfx`（约 550 行）：离屏 FBO、合成、阴影贴图、rim / 粒子两个着色器、`readAlpha`；主循环加「选目标 → 三件 → 抄 alpha → 合成」 |
| `src/web.h` | `Settings` 加 4 项：`ssaa`(off/2x/3x) / `shadow` / `rim` / `particles` |
| `src/web.cpp` | 读写 `/state` `POST /set` `reset` 全部接上；页面「外观」组加 4 行（带人话说明） |
| `state/settings.json` | 落盘多了 4 个键 |

没碰：socket 协议、service 文件、`native2/`、`platform_wayland.cpp` 一行都没改（**不需要**改：
EGL 那边已经是 Compatibility Profile，FBO / 着色器 / 固定管线全都能用）。

### 17.2 四个关键决定（每个都有代价）

1. **代码顺序是「模型先画、三件后垫」**，用的混合是 `blend(GL_ONE_MINUS_DST_ALPHA, GL_ONE)`
   即 `out = src*(1-dstA) + dst` —— 模型不透明的像素上 src 贡献 0，等价于「画在模型之前」。
   为什么不真按字面顺序（先画影子再画模型）：那样抄下来的 alpha 里混着影子(0.55)和粒子(0.5)，
   轮廓光会把它们也当成「她的轮廓」去膨胀，输入区掩码也会被影子撑出一块假的可点区域。
   现在抄的是**纯模型 alpha**，两件事一起干净了。
2. **轮廓光读上一帧的 alpha（`prevTex`）**：715 个 drawable 再画一遍 ≈2ms CPU，是这层最贵的
   一块（19% 里占一成多）。一帧延迟在「每帧位移 <1px」的呼吸/待机里看不出来。
3. **alpha 全程 premultiplied**：所有 pass 用 `(ONE, ONE_MINUS_SRC_ALPHA)`；合成那一趟是
   `GL_MODULATE` 全乘（`glColor4f(a,a,a,a)` 同时缩 RGB 和 A），所以 `alpha` 设置天然合在里面。
4. **输入区改成从 `prevTex` 读**（GPU 先 blit 降到窗口物理尺寸再 `glReadPixels`）：
   阈值仍然是 8、掩码来源变成纯模型 alpha，影子/粒子/环一律进不去。

阴影贴图是启动时生成的一张 128² 单位圆渐变，**峰值 alpha 0.55**（不是 1.0）：设置拉到 1 也不会
盖成一块硬黑饼，默认 0.28 时峰值只有 0.15，浅色壁纸上也不变灰块。椭圆的
`rx = 0.35×bbox宽`、`ry = 0.18×rx`（默认 118×21 逻辑 px），中心 = bbox 底边中点。

### 17.3 验证数据（HDMI-A-3 1920x1080 scale=1，`grim -o HDMI-A-3`，窗口 500x900 @ margin(640,120)）

**测法说明**：这个模型的呼吸 `ParamBreath` 是 0.4s 帧差的最大来源（16.2），A/B 对比必须把它冻住，
否则「她自己在动」会被算成后处理的差异。冻结方式沿用 16.6 那套：
`MIYU_FORCE_PARAM=ParamBreath=0.5` + `MIYU_NO_PHYS=1` + 网页里 `idle=0 blink=0 lookOn=false`，
冻完两帧差 **0 像素**（改之前先验过）。测完 `unset-environment` + 重启。

| # | 项 | 数字 |
| --- | --- | --- |
| 1 | **SSAA 生效** | off→2x 全窗口差 **34,747 px**（>6）；切 3x 时日志 `FBO 重建 1500x2700 … 15.4 MB` |
| 1b | 轮廓过渡像素（每行 5px 带内 25<a<250 的像素数） | off **2.64/行** → 2x **2.56/行** → 3x **2.37/行** |
| 1c | 硬跳变（相邻像素 a 变化>250 的对数） | off **10,895** → 2x **11,358** → 3x **13,372** |
| 2 | **阴影** ROI 120×120（脚下居中） | 平均亮度 **59.89 → 57.92（Δ1.97）**；变化像素 **2,818/14,400 (19.6%)**，最大差 51 |
| 2b | 阴影跟着拖动走 | 拖动前后阴影质心 **x 916 → 706（Δ−210）**，窗口 margin **640→423（Δ−200）** |
| 3 | **轮廓光** 2x | rim 0→1 **10,469 px** 变化；她实体区（107,064 px，`|她−背景|>30` 再内缩 2px）变化 **0** |
| 3b | **轮廓光 ssaa=off** | rim 0→1 **13,837 px**；实体区（106,808 px）变化 **0**；96% 变化落在她外面 |
| 4 | **粒子** | 数量 = `round(24×值)`：0.3 → **7 颗**，1.0 → **24 颗**（日志 `[post] 粒子 N 颗`） |
| 4b | 两帧间隔 500ms | particles=0 时差 **0 px**；=1 时 **138 px**，**98.6% 落在 bbox 下部 40%**，**32 个连通块**（中位 4px，最大 9px）= 分散 |
| 5 | **透明正确性** alpha=0.5 | 实测 vs `0.5×她+0.5×桌面`：平均误差 **0.50/255**、最大 **1.5**、边缘偏暗 **−0.34**（若 premultiplied 破了会是 **63.0**） |
| 6 | **性能**（60fps，/proc 30s 采样） | ssaa=off+三件全关 **18.5% / 211,948 KB**；ssaa=2x+三件全开 **18.7% / 211,576 KB**；基线（改之前）19.0–19.9% / 209,568 KB |
| 6b | fps | 两个配置都 **59.3–60.1**（没掉帧） |
| 8 | 编译 | 两个 target **0 warning / 0 error**（`-Wall -Wextra`，`/tmp` 里全新目录重编验证过） |

**17.4 回归（各测一项）**

| 项 | 证据 |
| --- | --- |
| `scale=0.8` | `/state.window=400x720`，`FBO 重建 800x1440（窗口 400x720 × SSAA 2x）` ← FBO 真的跟着缩放重建 |
| `alpha=0.6` | `[web] 生效：alpha`，视觉整体变淡 |
| `corner=br` | `吸角 br：屏 1920x1080 / 窗 500x900 / bar 37 -> margin (1420,143)` |
| `hide=true` | 窗口区 134,450 px 变化（她整个消失，桌面透出来）；`hide=false` 恢复 |
| `tap` | 点 (916,500) → `[tap] 第 1 次点击 local=(276,343) → 播 被戳4.json`，随后 `[plan] 起播「被戳·脸」` |
| `expr` | `切表情 → 体型2` |
| 穿透 | 点窗口内但她不在的位置 (660,300)：没有 `[tap]`、没有 `指针进入` → 点击穿到桌面 |
| 拖动 | margin `(640,120) → (423,120)`，阴影跟到新位置（见 2b） |
| 跨屏 | 拖出右边缘 → `搬家（拖动越界）：output[1] HDMI-A-3 -> output[0] eDP-1`，FBO 跟着重建 **2000x3600 / 27.5MB**（eDP-1 是 buffer scale 2），无崩溃；重启回 HDMI-A-3 |
| `reset` | **20 项**全部回默认（含新 4 项），`[web] reset：全部设置恢复默认（20 项）` |

### 17.5 踩到的两个坑（都不是崩，但会静默错）

1. **`ssaa=off` 时 `capturePrev` 抄错尺寸**：一开始用 FBO 的尺寸 `C.dw/dh` 去抄，
   从 2x 切到 off 之后这两个值还是 1000×1800，而当时画的是 500×900 的窗口 buffer →
   `glCopyTexSubImage2D` 越界读，`prevTex` 里是垃圾，于是 **ssaa=off 时轮廓光糊满她全身
   （86,810 px 变化）、输入区掩码也可能是错的**。修法：加一对 `tw/th`「本帧实际绘制目标尺寸」，
   抄 alpha 一律按它抄。**这个坑只有在 off 和 2x 之间来回切才暴露，单测 2x 一辈子看不到。**
2. **建 `prevFbo` 会改掉当前的 framebuffer 绑定**，建完不还回去，后面的 `glCopyTexSubImage2D`
   就变成「从空 prevFbo 抄自己」。修法：进函数先 `glGetIntegerv(GL_FRAMEBUFFER_BINDING)`，
   建完再绑回去。

### 17.6 SSAA 那条验收没达到，原因

设计稿期望「`ssaa=off` 明显更少过渡、更多硬边」。实测**过渡像素基本不变**（2.64 → 2.56/行），
硬跳变反而**变多**（10,895 → 11,358）。查下来两条原因，都不是实现问题：

- 这个模型的轮廓不是几何边，是**贴图里的 alpha 渐变**（画师画的软化边）。SSAA 超采样的是三角形
  边和纹理采样，对「贴图自带的 alpha 斜坡」无能为力 —— 斜坡宽度在屏幕像素上就是不变。
- 2x 渲染让**贴图不再走 mipmap**（1x 时模型被缩小 ~1.6×，`GL_LINEAR_MIPMAP_LINEAR` 会取到
  mip 层级 → 发糊；2x 时接近 1:1 → 锐）。所以画质上是「纹理更清楚 + 亚像素位置更准」，
  不是「轮廓更软」。

**结论**：SSAA 值得留着（2x 默认），但别期待它在贴图型立绘上治锯齿 —— 它治的是**纹理糊**。
真要让轮廓更软，得对 alpha 做一遍形态学（那是另一个活）。

### 17.7 已知边界

- FBO 显存：500×900@2x = 6.9 MB；**笔记本屏（eDP-1，buffer scale 2）上是 2000×3600 = 27.5 MB**，
  再加 prevTex / alphaTex 同量级。跨屏到 eDP-1 时显存翻 4 倍，这是 max(缓冲区)。
- 轮廓光半径 = `(1+2×scale)` 逻辑像素（默认 3px）。`scale=1.6` 时 4.2px，小窗口上会显得厚。
- 粒子只在有 bbox 时才生成；`platPersonBox` 靠输入区那一步算（每 10 帧 / 松手 / 尺寸变化），
  所以她的 bbox 有最多 10 帧的延迟 —— 拖动时影子和粒子会滞后一帧量级，肉眼看不到。
- 粒子数量上限 24；`particles=0` 时 vector 清空，`[post] 粒子 0 颗` 只在变化时打一行日志。

## 十八、阴影默认关 + 抗锯齿（FXAA/SSAA）+ 设置页重排版 + 明暗主题（2026-11-09 23:4x–2026-11-10 00:2x）

他三句话定了这一轮的走向：①「黑色界面几乎看不到脚下的阴影，所以那个就没有必要去开」；
②「fxaa 开了之后，变模糊了」；③「界面的利用率太低了 / 设置界面是不是太暗色调了，看起来不舒服」。

### 18.1 改了什么（只动 `src/` 和 `state/`）

| 位置 | 改动 |
| --- | --- |
| `src/web.h` | `ssaa` → `aa`（`off/fxaa/ssaa2x/ssaa3x`，**默认 `ssaa2x`**）；`shadow` 默认 `0.28` → **`0`**；新增 `setFps()` |
| `src/web.cpp` | `aa` 读写 / `/state` / `POST /set` / `reset`；**老 key `ssaa` 仍吃**（`off/2x/3x` → `off/ssaa2x/ssaa3x`，`aa` 优先）；`/state` 多 `fps_now`；设置页整页重写（多列 + 明暗双主题 + 实时帧率） |
| `src/main.cpp` | `postfx` 加 FXAA pass（着色器 / 第二个 FBO / `ensureFxTargets` / `applyFxaa`）；`factorOf(aa)` 四档；`framePace` 每 5 秒 `web::setFps()` |
| `state/settings.json` | 落盘键 `ssaa` → `aa`，`shadow: 0.000`，`aa: "ssaa2x"` |

没碰：socket 协议、service 文件、`native2/`、`platform_wayland.cpp`（一行没改）。

### 18.2 抗锯齿：为什么 fxaa 留着但**不是默认**

`aa` 四档 = `off` / `ssaa2x` / `ssaa3x` / `fxaa`。默认 **ssaa2x**。

FXAA 是这一轮加的：1x 的离屏 FBO → 一次全屏 FXAA（3.11 简化版：相对亮度阈值 + 四邻域边缘方向 +
±8 px 夹紧 + 3+4 抽样二选一）→ 合成回窗口。它**不用**第二张搜索纹理，也不需要 2x 超采样。

**但它糊，而且是他一眼看出来的**。原因是我们早知道的那件事（17.6）：这个模型的轮廓不是几何边，
是**贴图里的 alpha 渐变**（画师画的 2–4 px 软边）。FXAA 按亮度在这条带上做宽带混合，等于把本来
就软的地方再糊一遍 —— 脸 72×72 的高频细节（拉普拉斯方差）从 **2654.9 → 991.1（掉 63%）**。
SSAA 反过来：2x 渲染让贴图接近 1:1、不走 mipmap，边缘更硬更清楚。

所以页面上诚实地写着：`ssaa2x 最锐（贴图不走 mipmap），代价是帧率；fxaa 最快但糊——它按亮度整块混合；off 最省`。

**管线**：`FBO(1x 或 2x) → （只有 aa=fxaa 时）FXAA 到同尺寸 fxTex → 双线性合成/降采样到窗口`。
FXAA 排在「垫在模型下面的三件」之后、合成之前（它要混整张成品），`prevTex`（轮廓光/输入区吃的
纯模型 alpha）仍然在 DrawModel 紧接着那一步抄，不受影响。FXAA 输出必须另开一张 FBO —— GL 不允许
同一张纹理同时当读写目标。

### 18.3 设置页：按使用频率重排 + 响应式多列 + 明暗主题

**分组**（顺序 = 使用频率，5 组 20 项）：常用（显示/大小/透明度/吸角）→ 外观（抗锯齿/轮廓光/粒子/阴影）
→ 行为（待机风格/待机幅度/视线/视线强度/眨眼/点击/表情/表情停留/嘴动）→ 性能（帧率）→ 声音（出声/音量）；
最下面是「恢复默认」，页脚 `改完即时生效 · 设置存在 state/settings.json`。

**每项一行三栏**：`grid-template-columns: 88px minmax(0,1fr) 62px` —— 左标签、中控件、右当前值
（`tabular-nums` 数字对齐），下一行 11px 浅灰人话说明。

**布局**：`.wrap{max-width:1400px}`，`.grid` 单列；`@media (min-width:900px)` 两列；`@media (min-width:1400px)`
三列（`repeat(3, minmax(0,1fr))`，`align-items:start` 免得矮卡片被拉成一大片空白）。状态条和底部按钮
横跨整宽。**1920 宽实测铺开**：`state/shot-settings-dark.png` / `state/shot-settings-light.png`。

**配色**（默认暗色，不再是纯黑的 `#0d1117`）：

| 变量 | 暗（`data-theme="dark"`，默认） | 亮（`data-theme="light"`） |
| --- | --- | --- |
| 页面底 | `#16181d` | `#f5f6f8` |
| 卡片底 | `#1e2128` | `#ffffff` |
| 边框 | `rgba(255,255,255,.06)` | `rgba(0,0,0,.08)` |
| 主文字 | `#d7dce6` | `#1f2328` |
| 说明文字 | `#8b93a1` | `#6b7280` |
| 强调 | `#7aa2f7` | `#3b6fd4` |
| 滑块轨道 | `#2c313a` | `#d8dbe1` |

**对比度**（WCAG 相对亮度算的）：

| 组合 | 比值 | 要求 |
| --- | --- | --- |
| 暗 主文字/卡片 `#d7dce6` on `#1e2128` | **11.72:1** | ≥7 ✓ |
| 暗 说明/卡片 `#8b93a1` on `#1e2128` | **5.21:1** | ≥4.5 ✓ |
| 亮 主文字/卡片 `#1f2328` on `#fff` | **15.80:1** | ≥7 ✓ |
| 亮 说明/卡片 `#6b7280` on `#fff` | **4.83:1** | ≥4.5 ✓ |

右上角 `☀/☾` 切换，写 `localStorage('miyu-theme')`，刷新保持（`<body>` 里第一段脚本先读再上屏，
不闪白）。另加 `?theme=light|dark` 只对当前页面生效、**不写** localStorage（截图/调试用）。

### 18.4 验证数字

**默认值（`reset` 后 `settings.json` 原文）**

```
{"scale":1.000,"alpha":1.000,"tap":true,"expr":true,"idle":1.000,"idleStyle":"natural",
 "look":1.000,"lookOn":true,"blink":1.000,"mouth":1.000,"exprHold":1.000,"fps":60,
 "voice":false,"vol":0.800,"corner":"free","hide":false,"aa":"ssaa2x",
 "shadow":0.000,"rim":0.350,"particles":0.300}
```

新装（删掉 `state/settings.json` 再启动）：`shadow` = **0.0**、`aa` = **`ssaa2x`**。

**帧率（`fps=200`，同一次运行里交错测，取进程自己算的 5 秒窗口）**

| aa | 实测 | 备注 |
| --- | --- | --- |
| `off` | **138.4 / 135.7 / 132.9 / 134.3** | 和 十七 的 140.4 对得上 |
| `fxaa` | **133.6 / 134.9 / 133.1 / 137.0** | 比 off 少 1–3 帧（≈2%） |
| `ssaa2x` | **125.0 / 126.9**（抖动样本 66.6 / 114.8） | 十七 记的 82.2 今天没复现 |

每帧 CPU（utime+stime ÷ 帧数）：`off` **3.28 ms**、`fxaa` **3.21 ms**、`ssaa2x` **3.27 ms** —— 同一量级。
**这台机器今天很吵**（msedge 37% + node 20% 常驻），同一配置 6 分钟内测到过 81.6 和 125.0 两个数，
所以只能**同一次运行里交错比**，跨时间比绝对值没意义。

日常档（`fps=60`，`aa=ssaa2x`）：**60.0 帧/秒**，CPU **18%**，RSS **213 MB**。

**画质（同一块头发 40×40，6 倍最近邻，`state/shot-aa-compare.png`）**

| aa | 脸 72×72 拉普拉斯方差（越高越清楚） | 头发硬跳变（相邻亮度差>60） | 头发过渡像素（60<L<110） |
| --- | --- | --- | --- |
| `off` | **2654.9** | 54 | 763 |
| `ssaa2x` | **2516.1** | 74 | 776 |
| `fxaa` | **991.1（−63%）** | 37 | 810 |

FXAA 把细节抹掉六成 —— 这就是他说的「变模糊了」，数字和肉眼对上了。
`off` 的 2654.9 略高于 ssaa2x，是因为 1x 的锯齿本身就是假高频（边缘更脏但方差更高）；
ssaa2x 的硬跳变更多（74 vs 54）才是「更锐」的来源（贴图不走 mipmap）。
**没有黑边**：三档最暗像素都是 16.5，「比左右邻居暗 3 以上」的凹坑 off 23 → fxaa 17 → ssaa2x 17。

**回归（每项验完立刻复原成默认）**

| 项 | 证据 |
| --- | --- |
| `scale=0.8` | `window=400x720`，`缩放 0.80 -> 逻辑 400x720 / margin (-2,62)`，FBO/FXAA 目标跟着重建 400x720 |
| `alpha=0.6` | 她 bbox 内变化 **112,468/246,010** px，平均亮度 **72.3 → 39.5** |
| `hide=true` | 她 bbox 内变化 **124,376** px，平均亮度 **99.0 → 30.3**（桌面透出来） |
| `corner=br` | `吸角 br：屏 1920x1080 / 窗 500x900 / bar 37 -> margin (1420,143)` |
| `tap=false` | 点她的位置 → `[tap]` **0 条**；`tap=true` → `[tap] 第 1 次点击 local=(270,413) → 播 被戳4.json` |
| `expr=false` | socket 推两次 `{"expr":"脸红"}` → `[expr] ->` **0 条**；`true` → **1 条** `[expr] -> 脸红` |
| `lookOn=false` | `[look] 回正 nx=+0.00 ny=+0.00 k=0.00 … （视线关：nx 不带动任何角度）` |
| `blink=0` | 10 秒内闭眼 **0 次**；`blink=1` 8 秒内 **2 次** |
| `idleStyle` | `quiet` max\|ax\| = **0.99**；`lively` = **4.83**（4.9×） |
| `fps` | 30 → **30.0**；60 → **60.1**（真机不稳时 57–60） |
| `voice=true` | `[ctl] 声音 开，音量 0.80` |
| `reset` | `applied` **20 项**，`/state` + `settings.json` 全回上表 |
| 拖动 | margin `(1220,104) → (1015,104)`（拖 −200，实际 −205，y 不动） |
| 跨屏 | 拖出边缘 → `搬家（拖动越界）：output[1] HDMI-A-3 -> output[0] eDP-1`，`FBO 重建 2000x3600`（eDP-1 是 scale 2）；再拖回来 `-> output[1] HDMI-A-3`，`FBO 重建 1000x1800`，无崩溃 |
| 逐像素穿透 | 点窗口内她不在的地方 → `[tap]` 0 条、`指针进入` 0 条 |
| 输入区 | `input region: 物理 500x900 -> 逻辑 500x900 | 881 条矩形 | 覆盖 127912 px (28.4%) | bbox=[109,107 .. 446,837]` —— 和上一轮**逐字一致** |

**页面自检**

| 检查 | 结果 |
| --- | --- |
| `GET /` | **200**，**16239 字节**，单文件、零外链（`https?:// | src= | <link | @import | url(` 0 命中） |
| `GET /state` | 200，合法 JSON |
| `GET /nope` | **404** |
| `POST /set` 逐项 | 21 个 key 全 `ok:true` + `applied` 非空（含老键 `ssaa=3x` → `applied:["aa"]`） |
| `reset` | 一次回全默认（见上表） |
| 分组/顺序 | 5 组 20 项，顺序 hide/scale/alpha/corner → aa/rim/particles/shadow → idleStyle/idle/lookOn/look/blink/tap/expr/exprHold/mouth → fps → voice/vol |
| 主题 | `localStorage('miyu-theme')`，`?theme=light|dark` 只影响当页；两套截图见 `state/shot-settings-{dark,light}.png`（1920×1080） |

**进程/编译**：`systemctl --user is-active miyu-body` = **active**，输出 **HDMI-A-3**，`MIYU_OUTPUT=1` 保留，
标题 `native2-test`；近 10 分钟日志里 error/warning/失败/不完整 **0 条**；`/tmp/native3-clean` 全新目录重编
两个 target **0 warning / 0 error**（`-Wall -Wextra`）。

### 18.5 边界 / 没做的

- **SMAA 没做**。它要官方的 160×560 area 贴图 + 64×16 search 贴图（内嵌成 C 数组）才准；没有那两张
  图我只能手搓一个「差不多」的东西再叫它 SMAA —— 那正好是他会一眼看出问题的地方，不值当。
  真要做：把官方 `AreaTex.h` / `SearchTex.h` 的数据带进来，两个 pass（边缘 → 混合）替换掉现在的
  `applyFxaa`，接口不用动（`fxaaWanted` 那套加一个分支就行）。
- `aa=fxaa` 比 `aa=off` 多一张同尺寸 FBO（500×900 时 1.7 MB；eDP-1 上 1000×1800 = 6.9 MB）。
  `aa=off` 根本不建 FBO，是「绝对最省」那条路。
- 拖动是 platform 层的老逻辑（按 surface-local 位移累加），连着重启/快速注入点击时偶发「一次拖动跨两次屏」
  —— 那是注入器把光标瞬移造成的（代码里有「按下后大跳 → 按点重定位」兜底），手拖不会。本轮没改它。
- 这台机器**没有能跑的浏览器**（他的 Edge 在另一个 namespace，直接 exec `/proc/<pid>/root/...` 报
  ICU fd 错），所以页面截图是靠 `niri msg action spawn -- xdg-open <url>` 让**他的浏览器**开新标签、
  再 `grim` 抓的 —— 副作用是浏览器里多了几个 `米鱼 · 设置` 标签页。

---

## 十九、SMAA 抗锯齿（`aa=smaa`，2026-11-10 00:5x）

18.5 里那句「SMAA 没做，要官方查找表」作废：两张官方表已经放进 `assets/smaa/`，这一节就是把它接进去的
全部决定和实测。**默认仍是 `ssaa2x`**（他喜欢锐），`smaa` 是并列的第五档。

### 19.1 查找表：最朴素的 DDS，128 字节头 + 原始像素

两张表都是 `flags=0x81007`、无 fourcc、无压缩、无 mipmap，所以不需要任何图像库，按偏移读就行：

| 文件 | 头里读到 | 实际排布 | 上传成 |
| --- | --- | --- | --- |
| `AreaTexDX10.dds` | h=560 w=160 bpp=24，文件 268928 = 128 + 160×560×3 | 见下（**BGR 序**） | `GL_RGB8` + `GL_LINEAR` + `GL_CLAMP_TO_EDGE` |
| `SearchTex.dds` | h=16 w=64 bpp=8，文件 1152 = 128 + 64×16 | 单字节灰度，值只有 {0,127,254} | `GL_R8` + 同上 |

容易踩的一条：AreaTex 的像素头写的是 `Rmask=0x00ff0000 / Gmask=0x0000ff00 / Bmask=0x000000ff`，
**小端下 byte0 是 B、byte2 是 R**。实测 `byte0`（B）全 0、`byte1/byte2` 才有数据 —— 也就是说
「R/G 两个通道装了两份面积表、B 是补位的 0」。上传前把 b0/b2 对调（`std::swap`，89600 个像素，一次性），
就不用依赖 `GL_BGR`。判据：拿 Python 数一遍通道分布，B 通道 distinct=1（全 0）就是它。

不动点：**不建 mipmap**、`GL_LINEAR`（SearchTex 的 2×2 打包查询和 edgesTex 的 1.25 像素 gather 都靠线性过滤）。

### 19.2 三遍 pass，挂在 FXAA 同一条缝

输入 `targetTex()`（模型 + 垫在下面的影子/粒子/轮廓光都在里面），输出 `smaaOutTex` 交给 `composite`。
和 `applyFxaa` 完全同位置、同接口（`aaMul=1` 的 offscreen）：

| pass | 目标 | 做什么 |
| --- | --- | --- |
| ① edges | `RG8` 500×900 | luma 边缘检测（`SMAA_THRESHOLD 0.1`）：r = 西边有边、g = 北边有边。平坦区 `discard`（目标每帧先清 0） |
| ② weights | `RGBA8` 500×900 | `SearchTex` 决定沿 x/y 各走多长（最多 8 步、一次跨 2 px），`AreaTex` 把（左距、右距、两端的交叉边）换成两侧覆盖面积：r/g = 左右，b/a = 上下 |
| ③ neigh | `RGBA8` 500×900 | 用 weights 对**原始颜色**只混一个像素，且只往权重更大的那一边 |

三个目标的显存：`RG8 + 2×RGBA8 = 8 B/px`，500×900 = **3.4 MB**（日志有打印）。

### 19.3 两个移植上的坑

- **y 轴朝向**。官方 `SMAA.hlsl` 假设 texcoord y 向下；我们这个 FBO 的 `v=0` 在 NDC −1（屏幕上朝下），
  也就是 **y 向上**。整条链自己是一致的（edges/weights/neigh 都吃同一套坐标，面积表只区分「两侧」不区分
  「上下」），但 weights 那遍里 `±y` 的四个偏移（`-0.25/0.125`、`-0.125/-1.25`）和 neigh 的
  `offset.y = a.g > a.r ? -a.g : a.r` 是**带符号**的，翻错就是竖边往反方向拉丝。做法：照 three.js 的
  WebGL 移植版抄（同一份算法，注释里逐条标了 `Changed sign`），不自己推。
- **premultiplied + gamma**。参考实现的 neigh 那遍有 `pow(2.2) → mix → pow(1/2.2)`，那是给线性工作流准备
  的。我们的颜色是**预乘**的（rgb 已经被 alpha 缩过），pow 一遍等于把「颜色×alpha」拆开再拼，边上会偏色，
  所以这一趟直接 `mix()` 两个预乘色 —— 和 FXAA 现在的做法一致，实测整块平均亮度 off 91.25 → smaa 90.49
  （−0.8%，只是边缘像素的插值差，没有整体漂移）。

### 19.4 设置链路

| 层 | 改了什么 |
| --- | --- |
| `postfx` | `smaaWanted(v)`（用户的意思）/ `smaaAvailable()`（表 + shader 在不在）/ `applySmaa(DW,DH)` / `smaaTex()` |
| `main` | 每帧探一次可用性（只探一次）：`web::setSmaaOk(...)`，不可用就 `web::aaFallback()`（写回 ssaa2x 并落盘、日志一句话） |
| `web.h/cpp` | `normAa` 收 `smaa`；页面下拉加 `['smaa','smaa（只沿边缘混，比 fxaa 清楚）']`；`/state` 多一个 `smaa_ok`，页面拿到 `false` 就把这一项从下拉里删掉；`POST aa=smaa` 而不可用时按 ssaa2x 落 |

「不可用」是**两级**处理，缺一不可：日志（给人看）+ 设置回落（不然重启一次还是它、每次都白转一圈）。
启动读盘那一步**不**过滤 smaa —— 那时还不知道表在不在，先吃进来，等 GL 起来再判断。

### 19.5 实测（1920×1080 HDMI-A-3，窗口 500×900，scale=1.0）

**画质**：她头顶那一块 40×40（自动挑的「她身上梯度最强」的框，四档同一个框，先按 SAD 对齐到 ±0 像素），
6 倍放大并排 → `state/shot-smaa-compare.png`；脸 72×72（左眼 + 颊 + 一缕头发）算拉普拉斯方差：

| aa | 脸 72×72 拉普拉斯方差 | 过渡像素/行（40×40 内，覆盖率 0.05–0.95） |
| --- | --- | --- |
| off | **3038.5** | 5.90 |
| fxaa | 1460.0 | 6.79 |
| **smaa** | **2710.0** | 6.70 |
| ssaa2x | 3045.3 | 6.01 |

- smaa 保住 off 的 **89%** 锐度，fxaa 只有 48%（和 18 节「他一试就说 fxaa 糊」对得上）。
- 过渡像素这一项四档差别很小（5.9–6.8），原因写在这里免得下个人当成 bug：**这个模型的边本身就是贴图里的
  alpha 渐变（4 px 左右）**，换成头顶那条更干净的边再量是 4.07–4.59，还是分不开。真正分开四档的是
  拉普拉斯方差和图里的锯齿。
- 重复性：同一档截两次，脸方差 3038.5 / 2905.7（±4%），过渡像素 5.90 / 6.63 —— 她的呼吸一帧一帧在动，
  这是**测量噪声**，不是这一档不稳。

**帧率**（`fps=200` 上限，每档等 8 秒，取 `[fps] 实测`）：off **178.0** / fxaa **177.3** /
smaa **165.4** / ssaa2x **167.8**。全部够不到 200：这台机器整帧是 CPU 侧（模型 715 个 drawable）卡在 178，
所以四档的差别就是 GPU 侧那点后处理的差别 —— smaa 比 off 贵 **7%**，和 ssaa2x 基本同级。

**切换安全**：`ssaa2x → smaa → off → smaa → ssaa2x → smaa` 连切 5 次，主进程 pid 一直 1548471 没变，
每次截图她都在（变化像素 127903–128244），不崩、不黑屏、不错位。

**落盘**：`aa=smaa` 写进 `state/settings.json`，`systemctl --user restart miyu-body` 之后日志
`读回 … aa=smaa`，`/state` 也是 `smaa`。

**找不到查找表**（把 `assets/smaa/` 改名为 `assets/smaa.off` 再重启）：
`[post] smaa 不可用：assets/smaa/ 下没有（或不是）AreaTexDX10.dds …（读到 area=0 0x0 0bpp）` →
`[post] 就绪：… | smaa=不可用` → `[web] smaa 不可用：抗锯齿下拉里不留这一档，请求 smaa 的一律按 ssaa2x 走` →
`[web] smaa 不可用 → aa 回落到 ssaa2x（已写回 …settings.json）`。进程 active、PID 换了新的、
她照样在（128300 像素）、`/state` = `aa=ssaa2x / smaa_ok=false`、`settings.json` 也变成 `ssaa2x`。
把目录改回来重启，`smaa=ok`。

**编译**：`cmake --build build --clean-first` 两个 target **0 warning / 0 error**（顺手把 17 节留下的
`tIn/tAll` 两个没用到的变量删了，不然 `-Wall` 不干净）。

### 19.6 边界 / 没做的

- **没有对角检测（diag）和圆角（corner rounding）**：现在是 SMAA 1x **Medium** 预设（`MAX_SEARCH_STEPS 8`、
  只看横竖）。diag 那两段要 `SMAADecodeDiagBilinearAccess` 那套 0.25 偏移解码，抄错的方向不是崩而是
  「斜边往一边拉丝」，风险比收益大；真要加，把 `SMAACalculateDiagWeights` 整段搬进 weights 那遍即可
  （`e.g > 0` 分支里先算 diag，`weights.r + weights.g == 0` 才走横竖）。
- **edges/weights 没有降到半分辨率**。任务里写「半分辨率即可」，但这里 500×900 全分辨率也就多花 7% 帧率，
  而降分辨率要在 edges 与 neigh 之间多一套 texel 换算（weights 的 uv 全在 edges 的像素网格上），
  很容易把 `pixcoord` 那几行算错 —— 先在真机上量完，不值得为 3% 再冒一次错位的风险。
- `aa=smaa` 和 `aa=fxaa` 都多一组同尺寸目标；`aa=off` 仍然**一个 FBO 都不建**（最省那条路没变）。
- 拾取/输入区、拖动、跨屏、标题 `native2-test`、service 文件、socket 协议：**一个字没动**。
