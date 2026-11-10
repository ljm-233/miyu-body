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

// 现在是不是正被按住拖动（鼠标拖窗）。
//   · Wayland 实现：按下 BTN_LEFT 且指针在她身上 → true，松开 → false
//   · X11 实现：恒 false（GLFW 那边有自己的窗口拖动，这一层不接管）
// 调用方拿它省事：拖动期间别重算 input region（她一个像素都没动，算了浪费），
// 松手那一帧再补算一次。
bool platIsDragging();

// 指针当前是否在窗口内；在的话通过 out 返回 surface-local 逻辑坐标。
//   · Wayland 实现：靠 wl_pointer 的 enter/leave 记账。注意**只有她身上的像素**才算
//     「在窗口内」—— input region 之外的点合成器根本不发 enter（见 NOTES 第九节）。
//   · X11 实现：恒 false（X11 那版没有指针层，视线功能自然不生效）。
bool platPointer(float* x, float* y);

// 窗口的**逻辑**尺寸（surface-local 坐标系，跟 platPointer 同一个单位）。
// 为什么要单独一个：platGetFramebufferSize 给的是物理像素（逻辑 × buffer scale，
// 本机 2 倍）。拿物理宽度去归一化逻辑坐标，nx 永远落在 -1..0，看起来像"她只会往左看"。
void platGetLogicalSize(int* w, int* h);

// 她本人（不透明像素）在窗口里的包围盒，逻辑坐标。没有就返回 false。
// 用途：视线跟随要用「她本人」当坐标系，不是用整块窗口——逐像素穿透之后，
// 鼠标只在她身上才会被算作「在窗口内」，用窗口半宽当分母只能到 ±0.5。
bool platPersonBox(int* cx, int* cy, int* hw, int* hh);

// 当前 output 的全局原点（逻辑坐标）+ 窗口尺寸。给「第二支指针」换算用：
// pos.txt 是全局坐标，她的 local = 全局 - (output 原点 + 窗口 margin)。
bool platWindowOrigin(int* ox, int* oy, int* ww, int* wh);

// 从外部（第二支指针走 ctl.sock）触发一次「点在她身上 local=(lx,ly)」。
// 和真鼠标点击同一条路：冷却、部位判定、只动那一块全都一致。
void platTapAt(int lx, int ly);

// 销毁窗口（调用方负责先释放 GL 资源）。
void platShutdown();

// ── 多屏（只有 Wayland 实现提供这三个；X11 实现里没有定义，main.cpp 也不调）──
// 起因：layer-shell 的 get_layer_surface 把 surface **绑死在单个 wl_output** 上，
// 协议没有「改 output」这个动作。跨屏的唯一做法是拆掉整条 surface 链、在目标
// output 上重建一条新的 —— EGL context 和它里面的纹理一个字节都不动。
int  platOutputCount();            // 扫到几个 wl_output
int  platCurrentOutput();          // 当前挂在第几个（-1 = 还没起来）
bool platMoveToOutput(int index);  // 整个浮层搬到第 index 个 output 上

// 只有 Wayland 实现会真的用：把当前 framebuffer 的 alpha 转成输入区域。
// X11 实现里是空函数。
//
// 参数是 glReadPixels 读回来的整张 framebuffer：RGBA8888、**首行是底部**
// （GL 原点在左下），尺寸是物理像素。实现负责两件事：
//   ① 上下翻转（wl_region 的原点在左上）
//   ② 物理 → 逻辑：除以 buffer scale，再交给 alpha_region_apply
//      （scale=2 时逻辑区域是物理尺寸的一半，算错了区域会大一倍）
void platSetInputRegionFromAlpha(const unsigned char* rgba, int w, int h, int threshold);

// ── 运行时设置（网页设置页 / src/web.cpp 用，2026-11-09 22:5x）──────────
// 这一组和上面那些不同：上面是「窗口层的能力」，这一组是「窗口层的旋钮」。
// 全部要求：调用后立刻生效、不重启、失败只记日志不崩。

// 视觉缩放 0.4–1.6：重建 wl_egl_window（+ set_buffer_scale），窗口视觉大小跟着变。
// 位置保持（margin 不变，只按新尺寸重新夹一遍）。X11 实现里是空操作。
bool platSetScale(float factor);

// 单击反应开关（等价 MIYU_NO_TAP，但运行时可变）。拖动不受影响。
void platSetTapEnabled(bool on);

// 吸到当前 output 的四角之一：tl / tr / bl / br / free（free = 不动）。
// 左上锚点靠 margin 实现；下方两角会自动减掉顶部 bar 的 exclusive zone。
bool platSnapCorner(const char* corner);

// 整个隐藏：全透明（framebuffer 清成 alpha 0）+ 输入区清空，点击直接穿透，
// 进程照常跑、每帧照常换 buffer。false 恢复显示。
void platSetHidden(bool hidden);

// 当前挂在哪个 output 的名字（"HDMI-A-3"），设置页状态栏要显示。
const char* platOutputLabel();

// ── 二十：拖动体感三件（2026-11-10 01:xx）────────────────────────────
// grab     : 开始拖那一瞬的短动作（判断在 main：它每帧问 platIsDragging）
// inertia  : 松手后按最后 ~120ms 的速度继续滑一段再停（platform 自己算）
// edgeSoft : 贴边软阻力（越往外拖位移按 0.55 倍生效，不再是硬卡死）
// 三项都要求「改完立刻生效、不重启」；X11 备胎上是空实现。
void platSetDragFeel(bool grab, bool inertia, bool edgeSoft);

// ── 二十：拖动速度（逻辑 px/秒），给「身体倾斜」用 ────────────────────
// 没在拖 / 拖但停住 → false（调用方把倾斜目标归 0）。
// 只给水平方向也一起算：倾斜用 vx，轻微缩放用 |vy|。
bool platDragVelocity(float* vx, float* vy);

// 弹簧增益（设置页 dragSpring 0–2）：乘在 ω 上。0.5 更软更重、2 更硬更跟手。
void platSetDragSpring(float gain);
