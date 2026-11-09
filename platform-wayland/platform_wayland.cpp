// 桌宠平台层 —— Wayland + wlr-layer-shell + EGL(desktop GL)
//
// 路线照抄 ../wayland-probe/probe.c（已在这台机器上验证跑通）：
//   zwlr_layer_shell_v1(OVERLAY) + eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR)
//   + eglBindAPI(EGL_OPENGL_API) + EGL_OPENGL_BIT + 3.3 core
//
// 三个坑（probe 里证实过）：
//   1) Cubism 每帧把 MIN_FILTER 设成 GL_LINEAR_MIPMAP_LINEAR，纹理上传后必须
//      glGenerateMipmap，否则采样恒黑 —— 本文件是平台层，只负责 context/窗口，
//      但 gpa() 里保留了 glGenerateMipmap 入口给上层用（见 platGLProc）。
//   2) 顶部 bar 占 exclusive zone（约 37 逻辑 px），anchor TOP|LEFT 的窗口
//      落在物理 y=56 而不是 0；贴边要自己算 margin。
//   3) 不要用 GLEW（Wayland 下 glewInit 报 "No GLX display"），GL 入口用
//      eglGetProcAddress + dlsym 手撸。

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cmath>
#include <poll.h>
#include <dlfcn.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include "layer-shell-client.h"

#define LOG(...) do { fprintf(stderr, "[plat-wl] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

/* ---------------- 状态 ---------------- */
struct OutInfo {
    wl_output* out = nullptr;
    uint32_t   name = 0;
    char       label[64] = {0};
    int        scale = 1;
    int32_t    x = 0, y = 0;
};

static struct {
    wl_display*              dpy      = nullptr;
    wl_registry*             reg      = nullptr;
    wl_compositor*           comp     = nullptr;
    wl_shm*                  shm      = nullptr;
    zwlr_layer_shell_v1*     lshell   = nullptr;
    wl_surface*              surf     = nullptr;
    zwlr_layer_surface_v1*   lsurf    = nullptr;
    wl_output*               chosen   = nullptr;

    wl_egl_window*           ewin     = nullptr;
    EGLDisplay               edpy     = EGL_NO_DISPLAY;
    EGLConfig                ecfg     = nullptr;
    EGLContext               ectx     = EGL_NO_CONTEXT;
    EGLSurface               esurf    = EGL_NO_SURFACE;

    OutInfo                  outs[8];
    int                      nouts    = 0;
    int                      scale    = 1;

    int                      reqW     = 0, reqH = 0;   // 逻辑尺寸（请求值）
    int                      cfgW     = 0, cfgH = 0;   // configure 给的逻辑尺寸
    int                      configured = 0;
    int                      closed   = 0;

    int                      posX     = 0, posY = 0;
    bool                     inited   = false;
} W;

/* ---------------- wl_output ---------------- */
static void out_geometry(void* d, wl_output*, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char* make, const char* model, int32_t tf) {
    (void)pw;(void)ph;(void)sub;(void)make;(void)tf;
    OutInfo* o = (OutInfo*)d;
    o->x = x; o->y = y;
    if (model) snprintf(o->label, sizeof o->label, "%s", model);
}
static void out_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
static void out_done(void*, wl_output*) {}
static void out_scale(void* d, wl_output*, int32_t f) { ((OutInfo*)d)->scale = f ? f : 1; }
static void out_name(void* d, wl_output*, const char* name) {
    if (name) snprintf(((OutInfo*)d)->label, sizeof ((OutInfo*)d)->label, "%s", name);
}
static void out_desc(void*, wl_output*, const char*) {}

static const wl_output_listener out_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_desc
};

/* ---------------- registry ---------------- */
static void reg_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    if (!strcmp(iface, wl_compositor_interface.name)) {
        W.comp = (wl_compositor*)wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        W.shm = (wl_shm*)wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        W.lshell = (zwlr_layer_shell_v1*)wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface,
                                                         ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        if (W.nouts < 8) {
            OutInfo* o = &W.outs[W.nouts++];
            o->name  = name;
            o->scale = 1;
            o->out   = (wl_output*)wl_registry_bind(r, name, &wl_output_interface, ver < 4 ? ver : 4);
            wl_output_add_listener(o->out, &out_listener, o);
        }
    }
}
static void reg_global_remove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener reg_listener = { reg_global, reg_global_remove };

/* ---------------- layer surface ---------------- */
static void ls_configure(void*, zwlr_layer_surface_v1* ls, uint32_t serial, uint32_t w, uint32_t h) {
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    W.cfgW = (int)w;
    W.cfgH = (int)h;
    W.configured = 1;
    LOG("configure %ux%u", w, h);
}
static void ls_closed(void*, zwlr_layer_surface_v1*) {
    LOG("合成器关闭了 layer surface");
    W.closed = 1;
}
static const zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

/* ---------------- GL 入口（eglGetProcAddress + dlsym） ---------------- */
static void* gpa(const char* n) {
    void* p = (void*)eglGetProcAddress(n);
    if (!p) p = dlsym(RTLD_DEFAULT, n);
    return p;
}

/* 给上层（Cubism）用的通用取址；平台层自己也用它拿需要的入口 */
extern "C" void* platGLProc(const char* name) { return gpa(name); }

/* ---------------- 内部 ---------------- */
static void apply_margin() {
    if (!W.lsurf) return;
    // anchor TOP|LEFT：margin top = y，left = x。
    // 注意：TOP 边被 exclusive zone（顶上 bar，约 37 逻辑 px）占着时，margin
    // 从 zone 的下沿起算 —— 也就是 y=0 实际落在物理 y≈56，不是屏顶。贴边自己减。
    zwlr_layer_surface_v1_set_margin(W.lsurf, (uint32_t)(W.posY < 0 ? 0 : W.posY),
                                              0, 0,
                                              (uint32_t)(W.posX < 0 ? 0 : W.posX));
    wl_surface_commit(W.surf);
    wl_display_flush(W.dpy);
}

bool platInit(int w, int h, const char* title) {
    if (W.inited) return true;
    if (w <= 0 || h <= 0) { LOG("非法尺寸 %dx%d", w, h); return false; }
    W.reqW = w; W.reqH = h;

    /* 1) Wayland 连接 + globals */
    W.dpy = wl_display_connect(nullptr);
    if (!W.dpy) {
        const char* wd = getenv("WAYLAND_DISPLAY");
        LOG("wl_display_connect 失败 (WAYLAND_DISPLAY=%s)", wd ? wd : "?");
        return false;
    }
    W.reg = wl_display_get_registry(W.dpy);
    wl_registry_add_listener(W.reg, &reg_listener, nullptr);
    wl_display_roundtrip(W.dpy);   // globals
    wl_display_roundtrip(W.dpy);   // output 事件

    if (!W.comp)   { LOG("缺 wl_compositor");   return false; }
    if (!W.lshell) { LOG("缺 wlr_layer_shell_v1"); return false; }
    if (!W.nouts)  { LOG("没有 wl_output");     return false; }

    /* 选 output：优先 eDP-1（本机主屏），否则第一个 */
    OutInfo* pick = nullptr;
    for (int i = 0; i < W.nouts; i++) {
        LOG("output[%d] %s scale=%d pos=%d,%d", i, W.outs[i].label, W.outs[i].scale,
            W.outs[i].x, W.outs[i].y);
        if (!pick && strstr(W.outs[i].label, "eDP-1")) pick = &W.outs[i];
    }
    if (!pick) pick = &W.outs[0];
    W.chosen = pick->out;
    W.scale  = pick->scale > 0 ? pick->scale : 1;

    /* 2) OVERLAY 层 surface */
    W.surf = wl_compositor_create_surface(W.comp);
    W.lsurf = zwlr_layer_shell_v1_get_layer_surface(W.lshell, W.surf, W.chosen,
                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, title ? title : "plat-wayland");
    zwlr_layer_surface_v1_add_listener(W.lsurf, &ls_listener, nullptr);
    zwlr_layer_surface_v1_set_size(W.lsurf, (uint32_t)w, (uint32_t)h);
    zwlr_layer_surface_v1_set_anchor(W.lsurf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(W.lsurf, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(W.lsurf,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(W.surf);

    while (!W.configured) {
        if (wl_display_dispatch(W.dpy) < 0) { LOG("dispatch 失败，configure 没来"); return false; }
    }

    /* 3) EGL：platform display + 桌面 GL */
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    W.edpy = getPlatformDisplayEXT
        ? getPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR, (void*)W.dpy, nullptr)
        : eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, (void*)W.dpy, nullptr);
    if (W.edpy == EGL_NO_DISPLAY) { LOG("拿不到 EGLDisplay 0x%x", eglGetError()); return false; }

    EGLint emaj = 0, emin = 0;
    if (!eglInitialize(W.edpy, &emaj, &emin)) { LOG("eglInitialize 失败 0x%x", eglGetError()); return false; }
    if (!eglBindAPI(EGL_OPENGL_API)) { LOG("eglBindAPI(EGL_OPENGL_API) 失败 0x%x", eglGetError()); return false; }

    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,     // 桌面 GL，不是 ES
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,                  // 透明必需
        EGL_DEPTH_SIZE,      24,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(W.edpy, cfgAttr, &W.ecfg, 1, &ncfg) || ncfg < 1) {
        LOG("eglChooseConfig 失败 0x%x", eglGetError());
        return false;
    }

    const EGLint ctxAttr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    W.ectx = eglCreateContext(W.edpy, W.ecfg, EGL_NO_CONTEXT, ctxAttr);
    if (W.ectx == EGL_NO_CONTEXT) {
        LOG("3.3 core 失败 0x%x，退回默认 context", eglGetError());
        W.ectx = eglCreateContext(W.edpy, W.ecfg, EGL_NO_CONTEXT, nullptr);
    }
    if (W.ectx == EGL_NO_CONTEXT) { LOG("eglCreateContext 彻底失败 0x%x", eglGetError()); return false; }

    // 按 ceil(scale) 建大 buffer，避免 scale>1 时糊
    const int pw = w * W.scale, ph = h * W.scale;
    // 关键：buffer 建大了，必须同时告诉合成器 buffer_scale=scale，
    // 否则它把 1000x1800 的 buffer 当成「1000x1800 逻辑」，窗口会比请求大一整倍。
    wl_surface_set_buffer_scale(W.surf, W.scale);
    W.ewin = wl_egl_window_create(W.surf, pw, ph);
    if (!W.ewin) { LOG("wl_egl_window_create 失败"); return false; }
    W.esurf = eglCreateWindowSurface(W.edpy, W.ecfg, (EGLNativeWindowType)W.ewin, nullptr);
    if (W.esurf == EGL_NO_SURFACE) { LOG("eglCreateWindowSurface 失败 0x%x", eglGetError()); return false; }
    if (!eglMakeCurrent(W.edpy, W.esurf, W.esurf, W.ectx)) {
        LOG("eglMakeCurrent 失败 0x%x", eglGetError());
        return false;
    }

    LOG("EGL %d.%d | %s", emaj, emin, eglQueryString(W.edpy, EGL_VENDOR));
    LOG("GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
    LOG("GL_RENDERER=%s", (const char*)glGetString(GL_RENDERER));
    LOG("逻辑尺寸 %dx%d  ->  物理尺寸 %dx%d (scale=%d)", w, h, pw, ph, W.scale);

    glViewport(0, 0, pw, ph);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);

    W.inited = true;
    return true;
}

void platPollEvents() {
    if (!W.dpy) return;

    // 非阻塞标准写法：prepare_read 协议，避免和 dispatch_pending 抢读
    while (wl_display_prepare_read(W.dpy) != 0) {
        if (wl_display_dispatch_pending(W.dpy) < 0) { W.closed = 1; return; }
    }
    wl_display_flush(W.dpy);

    struct pollfd pfd;
    pfd.fd = wl_display_get_fd(W.dpy);
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) > 0) {
        if (wl_display_read_events(W.dpy) < 0) { W.closed = 1; return; }
        if (wl_display_dispatch_pending(W.dpy) < 0) { W.closed = 1; return; }
    } else {
        wl_display_cancel_read(W.dpy);
    }
    wl_display_flush(W.dpy);
}

void platGetFramebufferSize(int* w, int* h) {
    // 物理像素：逻辑尺寸 * buffer scale（本机 eDP-1 报 2）
    if (w) *w = W.reqW * W.scale;
    if (h) *h = W.reqH * W.scale;
}

void platSwapBuffers() {
    if (W.edpy == EGL_NO_DISPLAY || W.esurf == EGL_NO_SURFACE) return;
    if (!eglSwapBuffers(W.edpy, W.esurf)) LOG("eglSwapBuffers 失败 0x%x", eglGetError());
    wl_display_flush(W.dpy);
}

bool platShouldClose() { return W.closed != 0; }

double platTime() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void platSetPosition(int x, int y) {
    W.posX = x;
    W.posY = y;
    if (!W.inited) return;
    apply_margin();
}

void platShutdown() {
    if (!W.dpy) return;
    if (W.edpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(W.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (W.esurf != EGL_NO_SURFACE) eglDestroySurface(W.edpy, W.esurf);
        if (W.ectx  != EGL_NO_CONTEXT) eglDestroyContext(W.edpy, W.ectx);
        eglTerminate(W.edpy);
        W.esurf = EGL_NO_SURFACE;
        W.ectx  = EGL_NO_CONTEXT;
        W.edpy  = EGL_NO_DISPLAY;
    }
    if (W.ewin) { wl_egl_window_destroy(W.ewin); W.ewin = nullptr; }
    if (W.lsurf) { zwlr_layer_surface_v1_destroy(W.lsurf); W.lsurf = nullptr; }
    if (W.surf)  { wl_surface_destroy(W.surf); W.surf = nullptr; }
    if (W.lshell){ zwlr_layer_shell_v1_destroy(W.lshell); W.lshell = nullptr; }
    if (W.shm)   { wl_shm_destroy(W.shm); W.shm = nullptr; }
    if (W.comp)  { wl_compositor_destroy(W.comp); W.comp = nullptr; }
    for (int i = 0; i < W.nouts; i++)
        if (W.outs[i].out) { wl_output_destroy(W.outs[i].out); W.outs[i].out = nullptr; }
    if (W.reg)   { wl_registry_destroy(W.reg); W.reg = nullptr; }
    wl_display_disconnect(W.dpy);
    W.dpy = nullptr;
    W.inited = false;
}

// 按 alpha 抠输入区域。算法由另一个子代理写 alpha_region.c，这里先留空。
// TODO(待接入 alpha_region.c)：算出 opaque 区域（矩形并集）后，
//   wl_region* r = wl_compositor_create_region(W.comp);
//   wl_region_add(r, x, y, w, h);            // 注意：用逻辑坐标 = 物理 / scale
//   wl_surface_set_input_region(W.surf, r);
//   wl_surface_commit(W.surf); wl_region_destroy(r);
// 抠不出区域时 set_input_region(surf, nullptr) 表示整窗可点。
void platSetInputRegionFromAlpha(const unsigned char* rgba, int w, int h, int threshold) {
    (void)rgba; (void)w; (void)h; (void)threshold;
    // 空实现 —— 待接入 alpha_region.c
}
