// 窗口层的 X11 实现：GLFW 3.5 + XWayland。
//
// 这一层只做四件事：建透明无边框窗口、建 GL 上下文、跑事件循环、报尺寸。
// 渲染（Cubism / 纹理 / 投影）一概不进来。
//
// 坑，按踩过的顺序记：
//
// ① GLFW 3.5 默认优先 Wayland 后端。在 niri 这种纯 Wayland 会话里，
//    glfwInit() 会开出**普通 Wayland 窗口** —— 没有透明、没有 floating、
//    桌面 GL 的上下文也没有（只能拿到 EGL/GLES）。必须在 glfwInit 之前
//    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11)，强制走 X11(XWayland)。
//    注意：这是 init hint，不是 window hint；放在 glfwWindowHint 一起会静默无效。
//
// ② GLFW_TRANSPARENT_FRAMEBUFFER 只是"我要 ARGB visual"的请求。XWayland
//    给不到时会**静默降级**成不透明，症状是窗口底板变成一块渐变实色。
//    所以要读回来查（见下面第 3 帧的 [win] 日志里的 transparent_fb）。
//
// ③ 上下文版本/画像：3.3 + COMPAT profile。Cubism 官方 GLES2 渲染器在桌面 GL
//    上得走兼容画像才拿得到 glGenerateMipmap 那套老行为，CORE 画像下会有
//    纹理不完整 → 采样返回 (0,0,0,1) → 黑剪影。
//
// ④ 事件模型：GLFW 的 poll 必须在 swap 之后（现在的顺序就是
//    render → swap → poll），这样输入延迟一帧但与画面同步，别调换。

#include "platform.h"

#include <GLFW/glfw3.h>
#include <cstdio>
#include <cstdarg>

namespace {

GLFWwindow* g_win = nullptr;
bool g_inited = false;

void log_(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr);
}

}  // namespace

bool platInit(int w, int h, const char* title) {
    // ① 必须在 glfwInit 之前。不设这一行，niri 上开出来的是 Wayland 窗口。
    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);

    if (!glfwInit()) { log_("[win] glfwInit 失败"); return false; }
    g_inited = true;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);

    g_win = glfwCreateWindow(w, h, title, nullptr, nullptr);
    if (!g_win) { log_("[win] 建窗口失败"); return false; }

    glfwMakeContextCurrent(g_win);
    glfwSwapInterval(1);
    return true;
}

void platPollEvents() {
    if (!g_win) return;
    glfwPollEvents();
}

void platGetFramebufferSize(int* w, int* h) {
    if (!g_win) { if (w) *w = 0; if (h) *h = 0; return; }
    glfwGetFramebufferSize(g_win, w, h);

    // 第 3 帧打一次窗口真相（跟重构前同一时机），透明降级就靠它暴露。
    static int logged = 0;
    if (logged < 3 && ++logged == 3) {
        int wx = 0, wy = 0, ww = 0, wh = 0;
        glfwGetWindowPos(g_win, &wx, &wy);
        glfwGetWindowSize(g_win, &ww, &wh);
        log_("[win] pos=(%d,%d) size=%dx%d framebuffer=%dx%d",
             wx, wy, ww, wh, w ? *w : 0, h ? *h : 0);
        log_("[win] transparent_fb=%d (期望 1)",
             glfwGetWindowAttrib(g_win, GLFW_TRANSPARENT_FRAMEBUFFER));
    }
}

void platSwapBuffers() {
    if (!g_win) return;
    glfwSwapBuffers(g_win);
}

bool platShouldClose() {
    if (!g_win) return true;
    if (glfwWindowShouldClose(g_win)) return true;
    return glfwGetKey(g_win, GLFW_KEY_ESCAPE) == GLFW_PRESS;
}

double platTime() {
    return glfwGetTime();
}

void platSetPosition(int x, int y) {
    if (!g_win) return;
    glfwSetWindowPos(g_win, x, y);
}

// 走路要用（2026-11-11）：Wayland 那边读 W.posX/posY，X11 这边直接问 GLFW。
// 两个平台都有实现，否则 miyu 那个 target 链接不过。
void platSyncRect() {}

void platGetPosition(int* x, int* y, int* w, int* h) {
    if (x || y) {
        int px = 0, py = 0;
        if (g_win) glfwGetWindowPos(g_win, &px, &py);
        if (x) *x = px;
        if (y) *y = py;
    }
    if (w || h) {
        int ww = 0, wh = 0;
        if (g_win) glfwGetWindowSize(g_win, &ww, &wh);
        if (w) *w = ww;
        if (h) *h = wh;
    }
}

// X11 版不接管鼠标拖动：GLFW 那边有自己的窗口拖动（以后要接也是走
// glfwSetWindowUserPointer + 自己算标题栏），这一层恒 false。
// 存在的意义有两个：接口两个 target 一致；main.cpp 里「拖动时跳过 region 重算」
// 那段逻辑在 X11 版永远走「不是拖动」那条路（X11 本来也没有 region）。
bool platIsDragging() { return false; }

// X11 版没有指针层（GLFW 只给窗口级回调，没有 surface-local 这套），
// 视线功能在备胎上就是「不跟」：恒 false → main.cpp 那边目标永远是 0。
bool platPointer(float*, float*) { return false; }
bool platWindowOrigin(int*, int*, int*, int*) { return false; }
void platTapAt(int, int) {}

// 逻辑尺寸 = 窗口尺寸（X11 没有 buffer_scale 那套）。
void platGetLogicalSize(int* w, int* h) {
    if (!g_win) { if (w) *w = 0; if (h) *h = 0; return; }
    glfwGetWindowSize(g_win, w, h);
}

void platShutdown() {
    if (g_win) { glfwDestroyWindow(g_win); g_win = nullptr; }
    if (g_inited) { glfwTerminate(); g_inited = false; }
}

// X11 做不到逐像素点击穿透：X Shape 扩展的路子 xwayland-satellite 零实现
// （见 wayland-migration.md 第三节），所以这里是空实现，让两个 target 都能编。
void platSetInputRegionFromAlpha(const unsigned char*, int, int, int) {}

// X11 版没有「按 alpha 算她本人包围盒」这套，视线跟随在 X11 上退回到窗口坐标
bool platPersonBox(int*, int*, int*, int*) { return false; }

// ── 运行时设置（网页设置页）在 X11 备胎上的空实现 ────────────────────
// 这些能力是给 Wayland layer-shell 写的（重建 buffer / 输入区 / 输出名）。
// X11 那版只是备胎，一律「接受但什么都不做」，这样 main.cpp 两个 target 共用一套调用。
// alpha / idle / expr / hide 这几个是在 main 里落的（不经过这里），备胎上照常生效。
bool platSetScale(float) { return false; }
void platSetTapEnabled(bool) {}
bool platSnapCorner(const char*) { return false; }
void platSetHidden(bool) {}
const char* platOutputLabel() { return "X11"; }

// 二十：拖动三件（抓起反馈 / 松手惯性 / 贴边软阻力）。
// X11 这条路窗口拖动是 GLFW 自己的（platform.h 里写明 platIsDragging 恒 false），
// 三个旋钮在备胎上都没有落点 —— 接受调用、什么都不做，两个 target 共用一套调用。
void platSetDragFeel(bool, bool, bool) {}

// 二十：X11 备胎没有拖动速度（platIsDragging 恒 false），倾斜永远回 0。
void platSetDragSpring(float) {}

bool platDragVelocity(float* vx, float* vy) {
    if (vx) *vx = 0.0f;
    if (vy) *vy = 0.0f;
    return false;
}

void platFakeDrag(float vx, float vy, int dragMs) {   // X11 备胎不做手感，诊断口空着
    (void)vx; (void)vy; (void)dragMs;
}
