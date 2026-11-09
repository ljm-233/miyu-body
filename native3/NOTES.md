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
