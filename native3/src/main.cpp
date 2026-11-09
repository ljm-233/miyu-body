// 米鱼的身体 —— 原生渲染（窗口层接口 + 桌面 GL + Cubism Native）
//
// 2026-11-09 定稿路线，2026-11-09 晚重构：窗口/上下文/事件三块抽到 src/platform.h，
// 现有实现 platform_x11.cpp（GLFW + XWayland）。渲染代码不再认识任何窗口系统类型，
// 下一步加 platform_wayland.cpp 即可换后端，main.cpp 不用动。
//
// 2026-11-09 定稿路线。前面走过的弯路记在这儿，免得下次再试：
//   · Wayland layer-shell + EGL：窗口能浮能透明，但 EGL 那条路上的纹理采样怎么都不出颜色
//     （桌面 GL / GLES2 / GLES3 全试过，连自造 2x2 纯红图都采不出来）
//   · GLFW 走 Wayland 后端：普通窗口，niri 不给浮层
//   · **X11（XWayland）+ GLFW + 桌面 GL**：GLEW 初始化成功、采样正常、GLFW_TRANSPARENT_FRAMEBUFFER
//     真的透明、niri 里用 window-rule 设 open-floating 就浮起来了。就是这条。

#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include <GL/glew.h>

#include "platform.h"

#include "CubismFramework.hpp"
#include "Model/CubismMoc.hpp"
#include "Model/CubismModel.hpp"
#include "Rendering/CubismRenderer.hpp"
#include "Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "Math/CubismMatrix44.hpp"
#include "CubismModelSettingJson.hpp"
#include "ICubismAllocator.hpp"
#include "Physics/CubismPhysics.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

const char* kModelDir = "/home/link/.dsh/miyu/body/model/";
const char* kModelJson = "/home/link/.dsh/miyu/body/model/大肥鱼.model3.json";

void log_(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap); va_end(ap);
    std::fputc('\n', stderr);
}

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

std::vector<char> slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

GLuint load_tex(const std::string& p) {
    int w = 0, h = 0, c = 0;
    unsigned char* d = stbi_load(p.c_str(), &w, &h, &c, 4);
    if (!d) { log_("[tex] 读不了 %s", p.c_str()); return 0; }
    GLuint t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    // Cubism 的 SetupTexture 会把 MIN_FILTER 设成 GL_LINEAR_MIPMAP_LINEAR：没有 mip 链
    // 的纹理是不完整的，桌面 GL 采样不完整纹理返回 (0,0,0,1) —— 那就是"黑剪影"。
    glGenerateMipmap(GL_TEXTURE_2D);
    stbi_image_free(d);
    log_("[tex] %dx%d id=%u", w, h, t);
    return t;
}

}  // namespace

int main() {
    // ── 窗口（窗口层接口，实现在 platform_x11.cpp）────────────
    if (!platInit(500, 900, "native2-test")) { log_("[main] 窗口层起不来"); return 1; }
    glewExperimental = GL_TRUE;
    const GLenum ge = glewInit();
    log_("[main] GL=%s | %s | glew=%d", glGetString(GL_VERSION), glGetString(GL_RENDERER), ge);
    platSetPosition(1900, 150);

    // ── Cubism ──────────────────────────────────────────────
    static Alloc alloc;
    Csm::CubismFramework::Option opt;
    opt.LogFunction = [](const Csm::csmChar* m) { log_("[cubism] %s", m); };
    opt.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
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
    opt.ReleaseBytesFunction = [](Csm::csmByte* d) { delete[] d; };
    Csm::CubismFramework::StartUp(&alloc, &opt);
    Csm::CubismFramework::Initialize();
    log_("[main] Cubism 框架起来了");

    auto json = slurp(kModelJson);
    if (json.empty()) { log_("[main] 读不到 model3.json"); return 1; }
    Csm::CubismModelSettingJson setting(reinterpret_cast<const Csm::csmByte*>(json.data()),
                                        static_cast<Csm::csmSizeInt>(json.size()));
    log_("[main] Moc=%s 贴图数=%d", setting.GetModelFileName(), setting.GetTextureCount());

    auto mocData = slurp(std::string(kModelDir) + setting.GetModelFileName());
    if (mocData.empty()) { log_("[main] 读不到 moc3"); return 1; }
    Csm::CubismMoc* moc = Csm::CubismMoc::Create(reinterpret_cast<const Csm::csmByte*>(mocData.data()),
                                                 static_cast<Csm::csmUint32>(mocData.size()));
    if (!moc) { log_("[main] CubismMoc::Create 失败"); return 1; }
    Csm::CubismModel* model = moc->CreateModel();
    if (!model) { log_("[main] CreateModel 失败"); return 1; }
    log_("[main] 部件 %d 个", model->GetDrawableCount());

    std::vector<GLuint> texs;
    for (Csm::csmInt32 i = 0; i < setting.GetTextureCount(); ++i) {
        GLuint t = load_tex(std::string(kModelDir) + setting.GetTextureFileName(i));
        if (t == 0) return 1;
        texs.push_back(t);
    }

    auto* base = Csm::Rendering::CubismRenderer::Create(500, 900);
    if (!base) { log_("[main] 渲染器创建失败"); return 1; }
    base->Initialize(model, 64);
    auto* renderer = dynamic_cast<Csm::Rendering::CubismRenderer_OpenGLES2*>(base);
    if (!renderer) { log_("[main] 不是 GL 渲染器"); return 1; }
    for (size_t i = 0; i < texs.size(); ++i)
        renderer->BindTexture(static_cast<Csm::csmUint32>(i), texs[i]);
    log_("[main] 渲染器就绪，%zu 张贴图", texs.size());

    const float canvasW = model->GetCanvasWidth();
    const float canvasH = model->GetCanvasHeight();
    log_("[main] 画布 %.2fx%.2f", canvasW, canvasH);

    // ── 物理演算：头发 / 尾巴 / 裙子 ────────────────────────
    Csm::CubismPhysics* physics = nullptr;
    {
        const Csm::csmChar* pf = setting.GetPhysicsFileName();
        if (pf != nullptr && pf[0] != '\0') {
            auto pj = slurp(std::string(kModelDir) + pf);
            if (!pj.empty()) {
                physics = Csm::CubismPhysics::Create(reinterpret_cast<const Csm::csmByte*>(pj.data()),
                                                     static_cast<Csm::csmSizeInt>(pj.size()));
            }
            log_("[main] physics = %s (%zu 字节) -> %s", pf, pj.size(), physics ? "ok" : "失败");
        } else {
            log_("[main] model3.json 里没有 Physics");
        }
        const Csm::csmChar* po = setting.GetPoseFileName();
        log_("[main] pose = %s", (po != nullptr && po[0] != '\0') ? po : "(model3.json 里没有 Pose)");
    }

    // ── 不播动作 / 不眨眼 ──────────────────────────────────
    // 2026-11-09 13:3x：这两块（CubismEyeBlink::Create + CubismMotionManager + 读 IDLE）
    // 都**没有被每帧调用**，对画面零影响，只是启动时多读几个文件、多打几行日志。
    // 全部删掉，免得下次有人以为"已经在眨眼/已经在播动作"。
    // 要接的话姿势、陷阱和正确做法见 NOTES.md 最后一节。
    log_("[main] 姿态 = 静态站姿（无动作 / 无眨眼 / 只跑 physics）");

    // ── 诊断：参数表 + drawable 包围盒（MIYU_DUMP=1 才打印）──
    const bool kDump = std::getenv("MIYU_DUMP") != nullptr;
    if (kDump) {
        const Csm::csmInt32 pc = model->GetParameterCount();
        int zero = 0;
        log_("[dump] 参数 %d 个", pc);
        for (Csm::csmInt32 i = 0; i < pc; ++i) {
            const float v = model->GetParameterValue(static_cast<Csm::csmUint32>(i));
            const float d = model->GetParameterDefaultValue(static_cast<Csm::csmUint32>(i));
            if (v == 0.0f) ++zero;
            log_("[param] %2d %-26s v=%7.3f def=%7.3f [%7.3f,%7.3f]",
                 i, model->GetParameterId(static_cast<Csm::csmUint32>(i))->GetString(), v, d,
                 model->GetParameterMinimumValue(static_cast<Csm::csmUint32>(i)),
                 model->GetParameterMaximumValue(static_cast<Csm::csmUint32>(i)));
        }
        log_("[dump] 值为 0 的参数 %d / %d", zero, pc);

        const Csm::csmInt32 dc = model->GetDrawableCount();
        float ux0 = 1e30f, uy0 = 1e30f, ux1 = -1e30f, uy1 = -1e30f;
        int drawn = 0;
        for (Csm::csmInt32 i = 0; i < dc; ++i) {
            const Csm::csmInt32 vc = model->GetDrawableVertexCount(i);
            const Csm::csmFloat32* v = model->GetDrawableVertices(i);
            const float op = model->GetDrawableOpacity(i);
            if (vc <= 0 || !v) { log_("[draw] %3d vc=%d 无顶点", i, vc); continue; }
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            for (Csm::csmInt32 k = 0; k < vc; ++k) {
                const float x = v[2 * k], y = v[2 * k + 1];
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
            log_("[draw] %3d vc=%2d op=%5.2f tex=%d mask=%d vis=%d bbox=[%7.3f,%7.3f .. %7.3f,%7.3f] w=%6.3f h=%6.3f",
                 i, vc, op, model->GetDrawableTextureIndex(i),
                 model->GetDrawableMaskCounts()[i], model->GetDrawableDynamicFlagIsVisible(i),
                 x0, y0, x1, y1, x1 - x0, y1 - y0);
            if (op > 0.01f) {
                ++drawn;
                if (x0 < ux0) ux0 = x0;
                if (y0 < uy0) uy0 = y0;
                if (x1 > ux1) ux1 = x1;
                if (y1 > uy1) uy1 = y1;
            }
        }
        log_("[dump] 可见 drawable %d / %d，并集 bbox=[%.3f,%.3f .. %.3f,%.3f] w=%.3f h=%.3f (画布 %.2fx%.2f)",
             drawn, dc, ux0, uy0, ux1, uy1, ux1 - ux0, uy1 - uy0, canvasW, canvasH);
    }

    double lastT = platTime();
    while (!platShouldClose()) {
        const double now = platTime();
        float dt = static_cast<float>(now - lastT);
        lastT = now;
        if (dt > 0.1f) dt = 0.1f;   // 首帧 / 卡顿不喂大步长，物理会炸
        // [win] pos/size/transparent_fb 的诊断日志搬到 platform_x11.cpp 里了
        // （第 3 帧打一次，时机跟重构前一样）。
        int W = 0, H = 0;
        platGetFramebufferSize(&W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // 2026-11-09：投影必须按**窗口宽高比**校正，光用画布尺寸会把模型拉扁。
        // 做法：先把画布等比缩放到窗口的 95%，再换成 NDC 系数。
        const float sH = static_cast<float>(H) * 0.95f / canvasH;
        const float sW = static_cast<float>(W) * 0.95f / canvasW;
        const float k = (sH < sW) ? sH : sW;
        Csm::CubismMatrix44 proj;
        proj.Scale(2.0f * k / static_cast<float>(W), 2.0f * k / static_cast<float>(H));

        model->LoadParameters();
        if (physics && std::getenv("MIYU_NO_PHYS") == nullptr) physics->Evaluate(model, dt);
        model->Update();
        renderer->SetMvpMatrix(&proj);
        renderer->DrawModel();

        // ── 输入区域：只有她身上的像素接点击，其它地方点击穿透 ──────────
        // 每 10 帧（或 framebuffer 尺寸变了）把 back buffer 的 alpha 读回来，
        // 交给平台层转成 wl_surface 的 input region。
        //   · 必须在 platSwapBuffers 之前读 —— 读的是刚画完的这张
        //   · 逻辑尺寸 / 上下翻转由 platSetInputRegionFromAlpha 负责（它知道 scale）
        //   · MIYU_NO_INPUT_REGION=1 时整块跳过（整窗可点，用来对照排查）
        static const bool kNoRegion = std::getenv("MIYU_NO_INPUT_REGION") != nullptr;
        static int lastRW = -1, lastRH = -1;
        static unsigned long frameNo = 0;
        if (!kNoRegion) {
            ++frameNo;
            if (frameNo == 1 || frameNo % 10 == 0 || W != lastRW || H != lastRH) {
                static std::vector<unsigned char> pix;
                pix.resize(static_cast<size_t>(W) * static_cast<size_t>(H) * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
                platSetInputRegionFromAlpha(pix.data(), W, H, 8);
                if (W != lastRW || H != lastRH)
                    log_("[main] framebuffer %dx%d，输入区重算（第 %lu 帧）", W, H, frameNo);
                lastRW = W;
                lastRH = H;
            }
        }

        platSwapBuffers();
        platPollEvents();
    }
    platShutdown();
    return 0;
}
