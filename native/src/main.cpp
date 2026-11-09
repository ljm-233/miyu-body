// 米鱼的身体 —— 原生渲染（Wayland layer-shell + EGL + Cubism Native）
// 2026-11-09 深夜：窗口链路已通，这里往上放模型。

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GL/glew.h>
#include "layer-shell-client.h"

#include "CubismFramework.hpp"
#include "Model/CubismMoc.hpp"
#include "Model/CubismModel.hpp"
#include "Rendering/CubismRenderer.hpp"
#include "Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "Math/CubismMatrix44.hpp"
#include "CubismModelSettingJson.hpp"
#include "ICubismAllocator.hpp"

namespace {
class Alloc final : public Csm::ICubismAllocator {
public:
    void* Allocate(const Csm::csmSizeType size) override { return std::malloc(size); }
    void Deallocate(void* p) override { std::free(p); }
    void* AllocateAligned(const Csm::csmSizeType size, const Csm::csmUint32 alignment) override {
        void* p = nullptr;
        if (posix_memalign(&p, alignment, size) != 0) return nullptr;
        return p;
    }
    void DeallocateAligned(void* p) override { std::free(p); }
};
}  // namespace

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace {

const char* kModelDir = "/home/link/.dsh/miyu/body/model/";
const char* kModelJson = "/home/link/.dsh/miyu/body/model/大肥鱼.model3.json";

void log_(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr);
}

struct State {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_surface* surface = nullptr;
    zwlr_layer_shell_v1* layer_shell = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;
    wl_egl_window* egl_window = nullptr;
    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLContext egl_context = EGL_NO_CONTEXT;
    int width = 505, height = 972;
    bool configured = false, running = true;
};

void reg_global(void* d, wl_registry* r, uint32_t n, const char* i, uint32_t v) {
    auto* s = static_cast<State*>(d);
    if (std::strcmp(i, wl_compositor_interface.name) == 0)
        s->compositor = static_cast<wl_compositor*>(wl_registry_bind(r, n, &wl_compositor_interface, v < 4 ? v : 4));
    else if (std::strcmp(i, zwlr_layer_shell_v1_interface.name) == 0)
        s->layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(r, n, &zwlr_layer_shell_v1_interface, v < 4 ? v : 4));
}
void reg_remove(void*, wl_registry*, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

void ls_configure(void* d, zwlr_layer_surface_v1* ls, uint32_t serial, uint32_t w, uint32_t h) {
    auto* s = static_cast<State*>(d);
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    if (w > 0) s->width = static_cast<int>(w);
    if (h > 0) s->height = static_cast<int>(h);
    s->configured = true;
}
void ls_closed(void* d, zwlr_layer_surface_v1*) { log_("[main] compositor 把 surface 关了"); static_cast<State*>(d)->running = false; }
const zwlr_layer_surface_v1_listener kLS = {ls_configure, ls_closed};

std::vector<char> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

GLuint load_tex(const std::string& p) {
    int w = 0, h = 0, c = 0;
    unsigned char* d = stbi_load(p.c_str(), &w, &h, &c, 4);
    if (!d) { log_("[tex] 读不了 %s", p.c_str()); return 0; }
    {
        long nz = 0, colored = 0;
        for (long i = 0; i < (long)w * h; ++i) {
            if (d[i * 4 + 3] > 0) { ++nz; if (d[i * 4] | d[i * 4 + 1] | d[i * 4 + 2]) ++colored; }
        }
        log_("[tex] 非透明像素 %ld / %ld，其中有颜色的 %ld（尺寸 %dx%d）", nz, (long)w * h, colored, w, h);
    }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    stbi_image_free(d);
    // 2026-11-09 第二步：读回纹理参数。设不上（比如这环境里 glTexParameteri 是空实现）
    // 纹理就是不完整的，采样会直接返黑——昨晚卡在这儿。
    {
        GLint mn = -1, mg = -1, ws = -1;
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &mn);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &mg);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &ws);
        log_("[tex] 参数读回 MIN=%d MAG=%d WRAP_S=%d err=0x%x", mn, mg, ws, glGetError());
    }
    // 验证：上传后立刻读纹理尺寸，0 就说明压根没传进 GL
    GLint tw = 0, th = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &tw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &th);
    GLenum te = glGetError();
    log_("[tex-diag] 纹理 %u 读回 %dx%d err=0x%x", t, tw, th, te);

    log_("[tex] %dx%d %s", w, h, p.c_str());
    return t;
}

// 独立的纹理采样测试，插进主循环里跑
#include <cstdio>
#include <GL/glew.h>

static GLuint mkprog() {
    const char* vs = "#version 120\nattribute vec2 p; attribute vec2 uv; varying vec2 v;\nvoid main(){ v=uv; gl_Position=vec4(p,0.0,1.0); }\n";
    const char* fs = "#version 120\nvarying vec2 v; uniform sampler2D tex;\nvoid main(){ gl_FragColor=texture2D(tex, v); }\n";
    GLuint v = glCreateShader(GL_VERTEX_SHADER); glShaderSource(v,1,&vs,0); glCompileShader(v);
    GLuint f = glCreateShader(GL_FRAGMENT_SHADER); glShaderSource(f,1,&fs,0); glCompileShader(f);
    GLint ok=0; glGetShaderiv(f,GL_COMPILE_STATUS,&ok);
    if(!ok){ char log[512]; glGetShaderInfoLog(f,512,0,log); std::fprintf(stderr,"[gltest] fs 编译失败: %s\n",log); }
    GLuint p = glCreateProgram();
    glAttachShader(p,v); glAttachShader(p,f);
    // GLSL 120 不保证 attribute 的槽位，必须显式绑死，否则 uv 可能落在别的槽上
    
    glLinkProgram(p);
    glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){ char log[512]; glGetProgramInfoLog(p,512,0,log); std::fprintf(stderr,"[gltest] link 失败: %s\n",log); }
    return p;
}

bool init_egl(State& s) {
    s.egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(s.display));
    if (s.egl_display == EGL_NO_DISPLAY) return false;
    EGLint ma = 0, mi = 0;
    if (!eglInitialize(s.egl_display, &ma, &mi)) return false;
    // 2026-11-09 定稿：**桌面 GL + #version 120** 才是这套 Framework 的正确组合。
    // 之前一会儿 GLES 一会儿把 shader 改成 100，两边版本对不上，采样当然全黑。
    if (!eglBindAPI(EGL_OPENGL_API)) { log_("[egl] 绑桌面 GL 失败"); return false; }
    const EGLint attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_NONE,
    };
    EGLConfig cfg = nullptr; EGLint n = 0;
    if (!eglChooseConfig(s.egl_display, attr, &cfg, 1, &n) || n == 0) return false;
    s.egl_context = eglCreateContext(s.egl_display, cfg, EGL_NO_CONTEXT, nullptr);
    if (s.egl_context == EGL_NO_CONTEXT) return false;
    s.egl_window = wl_egl_window_create(s.surface, s.width, s.height);
    s.egl_surface = eglCreateWindowSurface(s.egl_display, cfg, reinterpret_cast<EGLNativeWindowType>(s.egl_window), nullptr);
    if (s.egl_surface == EGL_NO_SURFACE) return false;
    if (!eglMakeCurrent(s.egl_display, s.egl_surface, s.egl_surface, s.egl_context)) return false;
    log_("[egl] %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    // 2026-11-09：一定要在 GL context 建好之后初始化 GLEW，
    // 否则 glGenFramebuffers 这些是空指针，渲染器 Initialize 里直接段错误。
    // GLEW 在纯 EGL/Wayland 下会报 No GLX display，但函数指针可以自己用 eglGetProcAddress 补。
    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    log_("[egl] glewInit 返回 %d（非 0 是正常的，Wayland 下没有 GLX）", ge);
    // 关键：把 Cubism 渲染器会用到的几个入口手动填上
    struct { void** slot; const char* name; } fns[] = {
        {reinterpret_cast<void**>(&glGenFramebuffers), "glGenFramebuffers"},
        {reinterpret_cast<void**>(&glBindFramebuffer), "glBindFramebuffer"},
        {reinterpret_cast<void**>(&glFramebufferTexture2D), "glFramebufferTexture2D"},
        {reinterpret_cast<void**>(&glDeleteFramebuffers), "glDeleteFramebuffers"},
        {reinterpret_cast<void**>(&glUseProgram), "glUseProgram"},
        {reinterpret_cast<void**>(&glEnableVertexAttribArray), "glEnableVertexAttribArray"},
        {reinterpret_cast<void**>(&glDisableVertexAttribArray), "glDisableVertexAttribArray"},
        {reinterpret_cast<void**>(&glVertexAttribPointer), "glVertexAttribPointer"},
        {reinterpret_cast<void**>(&glCreateShader), "glCreateShader"},
        {reinterpret_cast<void**>(&glShaderSource), "glShaderSource"},
        {reinterpret_cast<void**>(&glCompileShader), "glCompileShader"},
        {reinterpret_cast<void**>(&glCreateProgram), "glCreateProgram"},
        {reinterpret_cast<void**>(&glAttachShader), "glAttachShader"},
        {reinterpret_cast<void**>(&glLinkProgram), "glLinkProgram"},
        {reinterpret_cast<void**>(&glGetUniformLocation), "glGetUniformLocation"},
        {reinterpret_cast<void**>(&glUniform1i), "glUniform1i"},
        {reinterpret_cast<void**>(&glUniformMatrix4fv), "glUniformMatrix4fv"},
        {reinterpret_cast<void**>(&glUniform4f), "glUniform4f"},
        {reinterpret_cast<void**>(&glGenBuffers), "glGenBuffers"},
        {reinterpret_cast<void**>(&glBindBuffer), "glBindBuffer"},
        {reinterpret_cast<void**>(&glBufferData), "glBufferData"},
        {reinterpret_cast<void**>(&glBlendFuncSeparate), "glBlendFuncSeparate"},
        {reinterpret_cast<void**>(&glCheckFramebufferStatus), "glCheckFramebufferStatus"},
        // 2026-11-09：只补 GLEW 的函数指针变量（__glewXxx）。
        // glBindTexture / glTexImage2D 这些是 GL 核心符号，& 取到的是函数地址，
        // 往那儿写会写进只读段，直接段错误。
        {reinterpret_cast<void**>(&glActiveTexture), "glActiveTexture"},
        {reinterpret_cast<void**>(&glGenerateMipmap), "glGenerateMipmap"},
    };
    int fixed = 0;
    // 不能判断「指针是否为空」：GLEW 失败时给的是内部转发桩，非空但一调就崩。
    // 直接用 eglGetProcAddress 的结果无脑覆盖。
    // 2026-11-09：`补了 0/25` 说明所有槽位都非空——GLEW 初始化失败时给的是内部转发桩，
    // 不报错、但可能根本没调到真的 GL。只强制补那两个最可疑的（切单元、绑纹理）。
    // 2026-11-09：GLES 上下文下 GLEW 基本不可用（它只认桌面 GL），全部自己用
    // eglGetProcAddress 取。这时拿到的是 GLES 版本，正是我们要的。
    for (auto& f : fns) {
        void* p2 = reinterpret_cast<void*>(eglGetProcAddress(f.name));
        if (p2) { *f.slot = p2; ++fixed; }
    }
    log_("[egl] 手动补了 %d/%zu 个 GL 入口", fixed, sizeof(fns) / sizeof(fns[0]));
    log_("[egl] 补了 %d/%zu 个 GL 入口", fixed, sizeof(fns) / sizeof(fns[0]));
    return true;
}

}  // namespace

int main() {
    Csm::CubismFramework::Option opt;
    opt.LogFunction = [](const Csm::csmChar* m) { log_("[cubism] %s", m); };
    opt.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
    // 2026-11-09：必须给读文件/释放的回调。Cubism 读物理、表情、动作都走这里，
    // 缺了就是满屏 "Byte releaser is not set"，模型内部那一堆东西全读不进来。
    opt.LoadFileFunction = [](const std::string path, Csm::csmSizeInt* size) -> Csm::csmByte* {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) { if (size) *size = 0; return nullptr; }
        const auto n = static_cast<std::streamsize>(f.tellg());
        f.seekg(0, std::ios::beg);
        auto* buf = new Csm::csmByte[static_cast<size_t>(n)];
        f.read(reinterpret_cast<char*>(buf), n);
        if (size) *size = static_cast<Csm::csmSizeInt>(n);
        return buf;
    };
    opt.ReleaseBytesFunction = [](Csm::csmByte* data) { delete[] data; };
    static Alloc g_alloc;
    Csm::CubismFramework::StartUp(&g_alloc, &opt);
    Csm::CubismFramework::Initialize();
    log_("[main] Cubism 框架起来了");

    State s;
    s.display = wl_display_connect(nullptr);
    if (!s.display) { log_("[main] 连不上 Wayland"); return 1; }
    s.registry = wl_display_get_registry(s.display);
    wl_registry_add_listener(s.registry, &kReg, &s);
    wl_display_roundtrip(s.display);

    s.surface = wl_compositor_create_surface(s.compositor);
    s.layer_surface = zwlr_layer_shell_v1_get_layer_surface(s.layer_shell, s.surface, nullptr,
                                                            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "miyu-body");
    zwlr_layer_surface_v1_add_listener(s.layer_surface, &kLS, &s);
    zwlr_layer_surface_v1_set_size(s.layer_surface, 505, 972);
    zwlr_layer_surface_v1_set_anchor(s.layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(s.layer_surface, 0);
    wl_surface_commit(s.surface);
    wl_display_flush(s.display);
    while (!s.configured && wl_display_dispatch(s.display) != -1) {}
    log_("[main] configure %dx%d", s.width, s.height);

    if (!init_egl(s)) { log_("[main] EGL 起不来"); return 1; }
    GLuint redTex = 0;

    // ── 载模型 ──────────────────────────────────────────────
    auto json = slurp(kModelJson);
    if (json.empty()) { log_("[main] 读不到 model3.json"); return 1; }
    Csm::CubismModelSettingJson setting(reinterpret_cast<const Csm::csmByte*>(json.data()), static_cast<Csm::csmSizeInt>(json.size()));
    log_("[main] Moc=%s 贴图数=%d", setting.GetModelFileName(), setting.GetTextureCount());

    std::string mocPath = std::string(kModelDir) + setting.GetModelFileName();
    auto mocData = slurp(mocPath);
    if (mocData.empty()) { log_("[main] 读不到 moc3"); return 1; }
    Csm::CubismMoc* moc = Csm::CubismMoc::Create(reinterpret_cast<const Csm::csmByte*>(mocData.data()), static_cast<Csm::csmUint32>(mocData.size()));
    if (!moc) { log_("[main] CubismMoc::Create 失败"); return 1; }
    Csm::CubismModel* model = moc->CreateModel();
    if (!model) { log_("[main] CreateModel 失败"); return 1; }
    log_("[main] 部件 %d 个", model->GetDrawableCount());

    std::vector<GLuint> texs;
    for (Csm::csmInt32 i = 0; i < setting.GetTextureCount(); ++i) {
        std::string p = std::string(kModelDir) + setting.GetTextureFileName(i);
        GLuint t = load_tex(p);
        if (t == 0) return 1;
        texs.push_back(t);
    }

    // Create 直接返回实例（工厂内部按平台建 OpenGL 渲染器）
    auto* base = Csm::Rendering::CubismRenderer::Create(static_cast<Csm::csmUint32>(s.width),
                                                       static_cast<Csm::csmUint32>(s.height));
    log_("[main] Create 返回 %p", (void*)base);
    if (!base) { log_("[main] 渲染器创建失败"); return 1; }
    log_("[main] 准备 Initialize(model, 512)");
    base->Initialize(model, 64);   // 这模型要 49 个蒙版，默认只给 1
    log_("[main] Initialize 完成");
    auto* renderer = dynamic_cast<Csm::Rendering::CubismRenderer_OpenGLES2*>(base);
    if (!renderer) { log_("[main] 不是 OpenGL 渲染器"); return 1; }
    for (size_t i = 0; i < texs.size(); ++i)
        renderer->BindTexture(static_cast<Csm::csmUint32>(i), texs[i]);

    log_("[main] 渲染器就绪");

    // 2026-11-09：Cubism 的顶点是**画布像素**坐标（原点在画布中心），不是 [-1,1]。
    // 之前按窗口尺寸做 NDC 系数，等于把 3749 宽的模型放大到 7 倍屏宽，直接甩出视野。
    // 正确做法：用画布尺寸做 NDC 系数，让它正好铺满 [-1,1]。
    // 2026-11-09：真相在这儿 —— 这模型的画布是 **1.0 x 2.1**，坐标是归一化的。
    // 之前按 3749x7697 算 NDC 系数，等于把模型缩到万分之一，屏幕上一片空白。
    const float canvasW = model->GetCanvasWidth();
    const float canvasH = model->GetCanvasHeight();
    Csm::CubismMatrix44 proj;
    proj.Scale(2.0f / canvasW, 2.0f / canvasH);   // y 取正：Cubism 的 y 朝上，NDC 也朝上
    log_("[main] 投影按画布 %.0fx%.0f 映射到窗口 %dx%d", canvasW, canvasH, s.width, s.height);

    // 诊断：把前几个 drawable 的顶点范围打出来，看模型坐标在哪
    {
        const Csm::csmInt32 n = model->GetDrawableCount();
        for (Csm::csmInt32 i = 0; i < n && i < 3; ++i) {
            const Csm::csmInt32 vc = model->GetDrawableVertexCount(i);
            const Csm::csmFloat32* v = model->GetDrawableVertices(i);
            if (!v || vc == 0) { log_("[diag] drawable %d 无顶点", i); continue; }
            float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
            for (Csm::csmInt32 k = 0; k < vc; ++k) {
                const float x = v[k * 2], y = v[k * 2 + 1];
                if (x < minx) minx = x; if (x > maxx) maxx = x;
                if (y < miny) miny = y; if (y > maxy) maxy = y;
            }
            log_("[diag] drawable %d: x %.1f..%.1f  y %.1f..%.1f  (%d 顶点)", i, minx, maxx, miny, maxy, vc);
        }
        log_("[diag] 画布信息: W=%.1f H=%.1f", model->GetCanvasWidth(), model->GetCanvasHeight());
    }

    while (s.running) {
        wl_display_dispatch_pending(s.display);
        wl_display_flush(s.display);

        glViewport(0, 0, s.width, s.height);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);   // 全透明，只留模型
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        static int fc = 0;
        ++fc;
        if (fc == 30 || fc == 120) {
            const Csm::csmInt32 dc = model->GetDrawableCount();
            int vis = 0;
            float gminx = 1e9f, gmaxx = -1e9f, gminy = 1e9f, gmaxy = -1e9f;
            for (Csm::csmInt32 i = 0; i < dc; ++i) {
                if (model->GetDrawableOpacity(i) > 0.01f) ++vis;
                const Csm::csmInt32 vc = model->GetDrawableVertexCount(i);
                const Csm::csmFloat32* v = model->GetDrawableVertices(i);
                if (!v || vc == 0) continue;
                for (Csm::csmInt32 k = 0; k < vc; ++k) {
                    const float x = v[k*2], y = v[k*2+1];
                    if (x < gminx) gminx = x; if (x > gmaxx) gmaxx = x;
                    if (y < gminy) gminy = y; if (y > gmaxy) gmaxy = y;
                }
            }
            log_("[diag] 第 %d 帧：可见部件 %d/%d，整体 x %.3f..%.3f y %.3f..%.3f",
                 fc, vis, dc, gminx, gmaxx, gminy, gmaxy);
        }
        model->LoadParameters();
        model->Update();
        renderer->SetMvpMatrix(&proj);
        // 独立测试：自己的 shader + 自己的贴图，不经过 Cubism
        static GLuint tp = 0;
        if (tp == 0) { tp = mkprog(); log_("[gltest] 程序=%u", tp); }
        {
            const float verts[] = {
                -1.0f, -1.0f,  0.0f, 0.0f,
                 0.0f,  1.5f,  0.5f, 1.0f,
                 1.0f, -1.0f,  1.0f, 0.0f,
            };
            glUseProgram(tp);
            GLint lp = glGetUniformLocation(tp, "tex");
            glUniform1i(lp, 0);
            {
            GLint au = -1, bt = -1;
            glGetIntegerv(GL_ACTIVE_TEXTURE, &au);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bt);
            static int once2 = 0;
            if (once2 < 2) { ++once2; log_("[gltest] active单元=%d 当前绑的纹理=%d", au, bt); }
        }
        glBindTexture(GL_TEXTURE_2D, redTex);   // 诊断：绑自造的红纹理
        {
            GLint sv = -999;
            glGetUniformiv(tp, lp, &sv);
            static int once3 = 0;
            if (once3 < 2) { ++once3; log_("[gltest] sampler 读回=%d（期望 0） err=0x%x", sv, glGetError()); }
        }
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, verts);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            static int once = 0;
            if (!once) {
                once = 1;
                log_("[gltest] uniform tex=%d 贴图=%u  uv槽=%d  p槽=%d",
                     lp, texs[0], glGetAttribLocation(tp, "uv"), glGetAttribLocation(tp, "p"));
            }
        }

        static int fr = 0;
        if (fr < 2) log_("[main] 第 %d 帧，准备 DrawModel", fr);
        renderer->DrawModel();
        if (fr < 2) { log_("[main] 第 %d 帧画完", fr); fr++; }

        if (eglSwapBuffers(s.egl_display, s.egl_surface) != EGL_TRUE) {
            static int n = 0;
            if (n++ < 3) log_("[main] swapBuffers 失败 0x%x", eglGetError());
        }
        usleep(16000);
    }
    return 0;
}
