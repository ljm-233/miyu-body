// wayland-probe: niri + wlr-layer-shell + EGL(desktop GL 3.3) 最小验证
// 目标：验证 EGL + EGL_OPENGL_API + layer-shell 在 niri 上能出正确颜色
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <poll.h>
#include <dlfcn.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "layer-shell-client.h"

#define LOG(...) do { fprintf(stderr, "[probe] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

/* ---------------- GL 入口点 ---------------- */
#define GL_FNS \
  X(PFNGLACTIVETEXTUREPROC,             glActiveTexture) \
  X(PFNGLATTACHSHADERPROC,              glAttachShader) \
  X(PFNGLBINDBUFFERPROC,                glBindBuffer) \
  X(PFNGLBINDVERTEXARRAYPROC,           glBindVertexArray) \
  X(PFNGLBUFFERDATAPROC,                glBufferData) \
  X(PFNGLCOMPILESHADERPROC,             glCompileShader) \
  X(PFNGLCREATEPROGRAMPROC,             glCreateProgram) \
  X(PFNGLCREATESHADERPROC,              glCreateShader) \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC,   glEnableVertexAttribArray) \
  X(PFNGLGENERATEMIPMAPPROC,            glGenerateMipmap) \
  X(PFNGLGENBUFFERSPROC,                glGenBuffers) \
  X(PFNGLGENVERTEXARRAYSPROC,           glGenVertexArrays) \
  X(PFNGLGETPROGRAMINFOLOGPROC,         glGetProgramInfoLog) \
  X(PFNGLGETPROGRAMIVPROC,              glGetProgramiv) \
  X(PFNGLGETSHADERINFOLOGPROC,          glGetShaderInfoLog) \
  X(PFNGLGETSHADERIVPROC,               glGetShaderiv) \
  X(PFNGLGETUNIFORMLOCATIONPROC,        glGetUniformLocation) \
  X(PFNGLLINKPROGRAMPROC,               glLinkProgram) \
  X(PFNGLSHADERSOURCEPROC,              glShaderSource) \
  X(PFNGLUNIFORM1IPROC,                 glUniform1i) \
  X(PFNGLUNIFORM4FPROC,                 glUniform4f) \
  X(PFNGLUSEPROGRAMPROC,                glUseProgram) \
  X(PFNGLVERTEXATTRIBPOINTERPROC,       glVertexAttribPointer)

#define X(type, name) static type p_##name;
GL_FNS
#undef X

static void *gpa(const char *n) {
    void *p = (void *)eglGetProcAddress(n);
    if (!p) p = dlsym(RTLD_DEFAULT, n);
    return p;
}

static int load_gl(void) {
    int miss = 0;
#define X(type, name) p_##name = (type)gpa(#name); if (!p_##name) { LOG("缺 GL 入口 %s", #name); miss++; }
    GL_FNS
#undef X
    return miss;
}

/* ---------------- Surface 尺寸 ---------------- */
#define SURF_W 400
#define SURF_H 400

/* ---------------- Wayland 状态 ---------------- */
struct output_info {
    struct wl_output *out;
    uint32_t name;          /* registry global name */
    char     model[64];
    int      scale;
};

static struct {
    struct wl_display              *dpy;
    struct wl_registry             *reg;
    struct wl_compositor           *comp;
    struct wl_shm                  *shm;
    struct zwlr_layer_shell_v1     *lshell;
    struct wl_surface              *surf;
    struct zwlr_layer_surface_v1   *lsurf;
    struct wl_output               *chosen;
    struct output_info              outs[8];
    int                             nouts;
    int                             configured;
    uint32_t                        lshell_ver;
    int                             shm_present;
} S;

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *make,
                         const char *model, int32_t tf) {
    (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)tf;
    snprintf(((struct output_info *)d)->model, sizeof(((struct output_info *)d)->model), "%s", model ? model : "?");
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d;(void)o;(void)f;(void)w;(void)h;(void)r;
}
static void out_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t f) {
    (void)o; ((struct output_info *)d)->scale = f;
}
static void out_name(void *d, struct wl_output *o, const char *name) {
    (void)o;
    if (name) snprintf(((struct output_info *)d)->model, sizeof(((struct output_info *)d)->model), "%s", name);
}
static void out_desc(void *d, struct wl_output *o, const char *desc) { (void)d;(void)o;(void)desc; }
static const struct wl_output_listener out_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_desc
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        S.comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
        LOG("wl_compositor v%u", ver);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        S.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
        S.shm_present = 1;
        LOG("wl_shm v%u (备用，本 probe 用 EGL 不走 shm)", ver);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        S.lshell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
        S.lshell_ver = ver;
        LOG("zwlr_layer_shell_v1 v%u  (合成器支持 layer-shell)", ver);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        if (S.nouts < 8) {
            struct output_info *oi = &S.outs[S.nouts++];
            memset(oi, 0, sizeof(*oi));
            oi->name = name;
            oi->scale = 1;
            oi->out = wl_registry_bind(r, name, &wl_output_interface, ver < 4 ? ver : 4);
            wl_output_add_listener(oi->out, &out_listener, oi);
        }
    }
}
static void reg_global_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d;(void)r;(void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_global_remove };

/* ---------------- layer surface ---------------- */
static void ls_configure(void *d, struct zwlr_layer_surface_v1 *ls,
                         uint32_t serial, uint32_t w, uint32_t h) {
    (void)d;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    LOG("layer_surface configure: %ux%u (我请求 %dx%d)", w, h, SURF_W, SURF_H);
    S.configured = 1;
}
static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
    (void)d;(void)ls;
    LOG("合成器关闭了 layer surface");
    exit(2);
}
static const struct zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

/* ---------------- GLSL ---------------- */
static const char *VS =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"
"layout(location=1) in vec2 aUV;\n"
"out vec2 vUV;\n"
"void main(){ vUV=aUV; gl_Position=vec4(aPos,0.0,1.0); }\n";

static const char *FS =
"#version 330 core\n"
"in vec2 vUV;\n"
"uniform sampler2D uTex;\n"
"uniform int uMode;\n"
"uniform vec4 uColor;\n"
"out vec4 oColor;\n"
"void main(){ oColor = (uMode==0) ? uColor : texture(uTex, vUV); }\n";

static GLuint mk_shader(GLenum t, const char *src) {
    GLuint s = p_glCreateShader(t);
    p_glShaderSource(s, 1, &src, NULL);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024]; GLsizei n = 0;
        p_glGetShaderInfoLog(s, sizeof buf, &n, buf);
        LOG("shader 编译失败: %.*s", (int)n, buf);
        exit(3);
    }
    return s;
}

static GLuint prog, vao, vbo, tex;
static GLint  u_tex, u_mode, u_color;

static void upload_verts(const float *v, int n) {
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 4 * sizeof(float)), v, GL_DYNAMIC_DRAW);
}

static void draw_tri_green(void) {
    float v[] = {
        -0.95f, -0.85f, 0.f, 0.f,
        -0.35f, -0.85f, 0.f, 0.f,
        -0.65f,  0.85f, 0.f, 0.f,
    };
    upload_verts(v, 3);
    p_glUniform1i(u_mode, 0);
    p_glUniform4f(u_color, 0.f, 1.f, 0.f, 1.f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void draw_quad(float x0, float y0, float x1, float y1) {
    float v[] = {
        x0, y0, 0.f, 0.f,  x1, y0, 1.f, 0.f,  x1, y1, 1.f, 1.f,
        x0, y0, 0.f, 0.f,  x1, y1, 1.f, 1.f,  x0, y1, 0.f, 1.f,
    };
    upload_verts(v, 6);
    p_glUniform1i(u_mode, 1);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

static void build_checker(unsigned char *px, int W, int H, int cell) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int c = ((x / cell) + (y / cell)) & 1;
            unsigned char *p = px + (size_t)(y * W + x) * 4;
            if (c) { p[0] = 0;   p[1] = 0;   p[2] = 255; }   /* 蓝 */
            else   { p[0] = 255; p[1] = 255; p[2] = 255; }   /* 白 */
            p[3] = 255;
        }
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* 1) Wayland */
    S.dpy = wl_display_connect(NULL);
    if (!S.dpy) { LOG("wl_display_connect 失败 (WAYLAND_DISPLAY=%s)", getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "?"); return 1; }
    LOG("wl_display_connect ok");

    S.reg = wl_display_get_registry(S.dpy);
    wl_registry_add_listener(S.reg, &reg_listener, NULL);
    wl_display_roundtrip(S.dpy);   /* globals */
    wl_display_roundtrip(S.dpy);   /* output 事件 */

    if (!S.comp)    { LOG("没有 wl_compositor"); return 1; }
    if (!S.lshell)  { LOG("没有 wlr_layer_shell_v1（合成器不支持）"); return 1; }

    for (int i = 0; i < S.nouts; i++)
        LOG("output[%d] %s scale=%d", i, S.outs[i].model, S.outs[i].scale);

    S.chosen = NULL;
    for (int i = 0; i < S.nouts; i++)
        if (!strncmp(S.outs[i].model, "eDP-1", 5)) { S.chosen = S.outs[i].out; break; }
    if (!S.chosen && S.nouts) S.chosen = S.outs[0].out;
    LOG("选中的 output: %p", (void *)S.chosen);

    /* 2) layer surface */
    S.surf = wl_compositor_create_surface(S.comp);
    S.lsurf = zwlr_layer_shell_v1_get_layer_surface(S.lshell, S.surf, S.chosen,
                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "wayland-probe");
    zwlr_layer_surface_v1_add_listener(S.lsurf, &ls_listener, NULL);
    zwlr_layer_surface_v1_set_size(S.lsurf, SURF_W, SURF_H);
    zwlr_layer_surface_v1_set_anchor(S.lsurf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(S.lsurf, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(S.lsurf,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(S.surf);

    while (!S.configured) {
        if (wl_display_dispatch(S.dpy) < 0) { LOG("dispatch 失败, configure 没来"); return 1; }
    }

    /* 3) EGL —— 关键：platform display + 桌面 GL API */
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayEXT =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    EGLDisplay edpy = EGL_NO_DISPLAY;
    if (getPlatformDisplayEXT) {
        edpy = getPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR, (void *)S.dpy, NULL);
        LOG("用了 eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_KHR)");
    } else {
        edpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, (void *)S.dpy, NULL);
        LOG("用了 eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR)");
    }
    if (edpy == EGL_NO_DISPLAY) { LOG("拿不到 EGLDisplay, err=0x%x", eglGetError()); return 1; }

    EGLint emaj = 0, emin = 0;
    if (!eglInitialize(edpy, &emaj, &emin)) { LOG("eglInitialize 失败 0x%x", eglGetError()); return 1; }
    LOG("EGL %d.%d | vendor=%s", emaj, emin, eglQueryString(edpy, EGL_VENDOR));
    LOG("EGL_CLIENT_APIS=%s", eglQueryString(edpy, EGL_CLIENT_APIS));

    if (!eglBindAPI(EGL_OPENGL_API)) { LOG("eglBindAPI(EGL_OPENGL_API) 失败 0x%x", eglGetError()); return 1; }
    LOG("eglBindAPI(EGL_OPENGL_API) ok");

    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,      /* <<< 桌面 GL，不是 ES */
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      24,
        EGL_NONE
    };
    EGLConfig cfg = NULL; EGLint ncfg = 0;
    if (!eglChooseConfig(edpy, cfgAttr, &cfg, 1, &ncfg) || ncfg < 1) {
        LOG("eglChooseConfig 失败 0x%x", eglGetError());
        return 1;
    }

    EGLint rt = 0, msaa = 0, asz = 0, r = 0, g = 0, b = 0, st = 0, cav = 0, cid = 0;
    eglGetConfigAttrib(edpy, cfg, EGL_RENDERABLE_TYPE, &rt);
    eglGetConfigAttrib(edpy, cfg, EGL_SAMPLE_BUFFERS,  &msaa);
    eglGetConfigAttrib(edpy, cfg, EGL_CONFIG_ID,       &cid);
    eglGetConfigAttrib(edpy, cfg, EGL_ALPHA_SIZE,      &asz);
    eglGetConfigAttrib(edpy, cfg, EGL_RED_SIZE,        &r);
    eglGetConfigAttrib(edpy, cfg, EGL_GREEN_SIZE,      &g);
    eglGetConfigAttrib(edpy, cfg, EGL_BLUE_SIZE,       &b);
    eglGetConfigAttrib(edpy, cfg, EGL_SURFACE_TYPE,    &st);
    eglGetConfigAttrib(edpy, cfg, EGL_CONFIG_CAVEAT,   &cav);
    LOG("CHOSEN_CONFIG id=%d renderable=0x%x(%s%s%s) sample_buffers=%d alpha=%d rgb=%d/%d/%d surface_type=0x%x caveat=0x%x",
        cid, rt,
        (rt & EGL_OPENGL_BIT) ? "EGL_OPENGL_BIT " : "!OPENGL ",
        (rt & EGL_OPENGL_ES2_BIT) ? "+ES2 " : "",
        (rt & EGL_OPENGL_ES3_BIT) ? "+ES3" : "",
        msaa, asz, r, g, b, st, cav);

    EGLint ctxAttr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctxAttr);
    if (ctx == EGL_NO_CONTEXT) {
        LOG("3.3 core context 失败 0x%x，退回默认 context", eglGetError());
        ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, NULL);
    }
    if (ctx == EGL_NO_CONTEXT) { LOG("eglCreateContext 彻底失败 0x%x", eglGetError()); return 1; }

    struct wl_egl_window *ew = wl_egl_window_create(S.surf, SURF_W, SURF_H);
    EGLSurface esurf = eglCreateWindowSurface(edpy, cfg, (EGLNativeWindowType)ew, NULL);
    if (esurf == EGL_NO_SURFACE) { LOG("eglCreateWindowSurface 失败 0x%x", eglGetError()); return 1; }
    if (!eglMakeCurrent(edpy, esurf, esurf, ctx)) { LOG("eglMakeCurrent 失败 0x%x", eglGetError()); return 1; }

    { EGLint sb = -1; eglQuerySurface(edpy, esurf, EGL_SWAP_BEHAVIOR, &sb);
      LOG("SURFACE swap_behavior=0x%x (%s)  -> 双缓冲 window surface（eglSwapBuffers 成功即证）",
          sb, sb == EGL_BUFFER_PRESERVED ? "BUFFER_PRESERVED" : "BUFFER_DESTROYED(默认双缓冲)"); }
    LOG("GL_VERSION=%s", (const char *)glGetString(GL_VERSION));
    LOG("GL_RENDERER=%s", (const char *)glGetString(GL_RENDERER));
    LOG("GL_VENDOR=%s", (const char *)glGetString(GL_VENDOR));
    LOG("GLSL=%s", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

    if (load_gl()) { LOG("GL 入口缺失，终止"); return 1; }

    /* 4) 渲染资源 */
    prog = p_glCreateProgram();
    p_glAttachShader(prog, mk_shader(GL_VERTEX_SHADER, VS));
    p_glAttachShader(prog, mk_shader(GL_FRAGMENT_SHADER, FS));
    p_glLinkProgram(prog);
    { GLint ok = 0; p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
      if (!ok) { char buf[1024]; GLsizei n = 0; p_glGetProgramInfoLog(prog, sizeof buf, &n, buf);
                 LOG("program 链接失败: %.*s", (int)n, buf); return 3; } }
    u_tex   = p_glGetUniformLocation(prog, "uTex");
    u_mode  = p_glGetUniformLocation(prog, "uMode");
    u_color = p_glGetUniformLocation(prog, "uColor");

    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    p_glEnableVertexAttribArray(1);
    p_glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    static unsigned char px[64 * 64 * 4];
    build_checker(px, 64, 64, 8);            /* 8x8 个 8px 格子 */

    glGenTextures(1, &tex);
    p_glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    /* 5) 画 */
    glViewport(0, 0, SURF_W, SURF_H);
    glClearColor(1.f, 0.f, 0.f, 1.f);        /* 红底 */
    glClear(GL_COLOR_BUFFER_BIT);

    p_glUseProgram(prog);
    p_glUniform1i(u_tex, 0);

    /* 左：绿三角 */
    draw_tri_green();

    /* A) GL_LINEAR，没有 mip 链 —— 老代码就靠这个活着 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    draw_quad(-0.30f, -0.90f, -0.06f, 0.90f);
    LOG("draw A (MIN=GL_LINEAR)            err=0x%x", glGetError());

    /* B) GL_LINEAR_MIPMAP_LINEAR 但**不**生成 mip —— 复现当年「恒黑」 */
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    draw_quad(0.00f, -0.90f, 0.24f, 0.90f);
    LOG("draw B (MIN=LM_LM, 无mip=贴图不完整) err=0x%x", glGetError());

    /* C) 生成 mip 之后再画同样 LM_LM */
    p_glGenerateMipmap(GL_TEXTURE_2D);
    draw_quad(0.30f, -0.90f, 0.54f, 0.90f);
    LOG("draw C (MIN=LM_LM + glGenerateMipmap) err=0x%x", glGetError());

    if (!eglSwapBuffers(edpy, esurf)) { LOG("eglSwapBuffers 失败 0x%x", eglGetError()); return 1; }
    LOG("READY");   /* 截图脚本等这一行 */

    /* 6) 保持 8 秒，让 grim 截 */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct pollfd pfd = { wl_display_get_fd(S.dpy), POLLIN, 0 };
    for (;;) {
        if (poll(&pfd, 1, 100) > 0) {
            if (wl_display_dispatch(S.dpy) < 0) break;
        }
        wl_display_flush(S.dpy);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double el = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (el > 8.0) break;
    }
    LOG("退出");
    return 0;
}
