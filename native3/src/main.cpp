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
#include <algorithm>
#include <ctime>      // nanosleep：帧率上限（第十六节）
#include <unistd.h>
#include <dirent.h>   // scanExpressions：扫 model/Exp3 给「切个表情」按钮
#include <string>
#include <vector>
#include <fstream>

#include <GL/glew.h>

#include "platform.h"
#include "ctl.h"    // 本地控制口：外面推「她在说什么/什么心情」，见第十一节
#include "plan.h"   // 动作计划播放器：外面推一份时间线，她照着演，见第十二节
#include "web.h"    // 本地设置页：127.0.0.1:8200，改完立刻生效，见第十四节

#include "CubismFramework.hpp"
#include "Model/CubismMoc.hpp"
#include "Model/CubismModel.hpp"
#include "Rendering/CubismRenderer.hpp"
#include "Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp"
#include "Motion/CubismExpressionMotion.hpp"
#include "Motion/CubismExpressionMotionManager.hpp"
#include "Math/CubismMatrix44.hpp"
#include "CubismModelSettingJson.hpp"
#include "ICubismAllocator.hpp"
#include "Physics/CubismPhysics.hpp"
#include "Effect/CubismEyeBlink.hpp"
#include "Motion/CubismMotion.hpp"
#include "Motion/CubismMotionManager.hpp"
#include "Id/CubismIdManager.hpp"

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


// ── 待机微动（自己写的节奏，不是 motion）────────────────────────
// 为什么不用 Animation/IDLE.motion3.json：把那 21 条曲线拆开看，驱动的是
// ShockR/L（电击）、冒汗1/2、泪花闪烁、流泪-膨胀/滴下、落泪、星星闪烁、光1~4、
// 呆滞 —— 这是一套表情特效开关，不是站立微摆。播了身体不动，只偶尔闪一下星星。
// 所以改成直接写她自己的参数：呼吸一直走，头/歪头/重心每隔几秒换一个姿态。
// 关键：**不是匀速正弦**，一段一段来、段间有停顿，否则一眼看出是程序。
struct IdleRig {
    Csm::csmInt32 iBreath = -1, iAx = -1, iAz = -1, iBz = -1;
    double segT0 = 0.0, segLen = 4.0;
    float fAx = 0, tAx = 0, fAz = 0, tAz = 0, fBz = 0, tBz = 0;
    float lastAx = 0, lastAz = 0, lastBz = 0, lastBr = 0;   // 供视线在此基础上叠加
    bool ready = false;
};
static IdleRig gIdle;

static void idleInit(Csm::CubismModel* model) {
    Csm::CubismIdManager* m = Csm::CubismFramework::GetIdManager();
    gIdle.iBreath = model->GetParameterIndex(m->GetId("ParamBreath"));
    gIdle.iAx = model->GetParameterIndex(m->GetId("AngleX"));
    gIdle.iAz = model->GetParameterIndex(m->GetId("AngleZ"));
    gIdle.iBz = model->GetParameterIndex(m->GetId("PositionX"));
    srand(static_cast<unsigned>(time(nullptr)));   // 不种的话每次启动抽到同一串数
    gIdle.ready = true;
    log_("[main] 待机参数索引 breath=%d angleX=%d angleZ=%d bodyZ=%d",
         gIdle.iBreath, gIdle.iAx, gIdle.iAz, gIdle.iBz);
}

static float idleRnd() { return static_cast<float>(rand()) / static_cast<float>(RAND_MAX); }

// 待机风格三档（网页设置页 idleStyle，第十六节）：
//   quiet   幅度 ×0.35、5~9 秒换一次姿态（几乎不动，只剩呼吸 + 一点点）
//   natural ×1.0、2.5~6 秒（默认；基数已从 ±1.2/±0.8/±1.5 放大到 ±4.0/±2.5/±3.0）
//   lively  ×2.0、1.5~4 秒（明显在晃）
// 网页上的 idle 数值倍率仍然保留，在风格之上再乘（style.amp * idle）。
struct IdleStyle { float amp; float segLo; float segHi; };
static IdleStyle idleStyleOf(const char* name) {
    if (name && !strcmp(name, "quiet"))  return IdleStyle{0.35f, 5.0f, 9.0f};
    if (name && !strcmp(name, "lively")) return IdleStyle{2.0f, 1.5f, 4.0f};
    return IdleStyle{1.0f, 2.5f, 6.0f};
}

static void idleTick(Csm::CubismModel* model, double now) {
    if (!gIdle.ready) idleInit(model);

    // 2026-11-09 晚：计划在播时这套待机**只让出身体三个参数**（AngleX/AngleZ/PositionX），
    // 呼吸（ParamBreath）照旧 —— 不然她演动作的两秒里是屏住气的，一眼看出是程序。
    // 姿态目标也不推进：不推进就不会在计划结束时从一个旧目标跳回正弦当前值。
    const bool planOn = plan::active();
    if (planOn) {
        const float brOnly = 0.5f + 0.5f * sinf(static_cast<float>(now * 2.0 * 3.14159265358979 / 4.2));
        gIdle.lastBr = brOnly;
        if (gIdle.iBreath >= 0) model->SetParameterValue(gIdle.iBreath, brOnly, 1.0f);
        return;
    }

    const IdleStyle style = idleStyleOf(web::settings().idleStyle.c_str());
    if (now - gIdle.segT0 >= gIdle.segLen) {
        gIdle.segT0 = now;
        gIdle.segLen = style.segLo + (style.segHi - style.segLo) * idleRnd();   // 按风格换姿态
        gIdle.fAx = gIdle.tAx; gIdle.fAz = gIdle.tAz; gIdle.fBz = gIdle.tBz;
        gIdle.tAx = (idleRnd() * 2.0f - 1.0f) * 4.0f;  // AngleX 是整体平移不是转头，±4 才看得出
        gIdle.tAz = (idleRnd() * 2.0f - 1.0f) * 2.5f;  // 歪头 ±2.5（多了像纸片歪）
        gIdle.tBz = (idleRnd() * 2.0f - 1.0f) * 3.0f;  // 重心 ±3
    }
    const float u = static_cast<float>((now - gIdle.segT0) / gIdle.segLen);
    const float s = u * u * (3.0f - 2.0f * u);           // smoothstep：起止都不突
    // 网页设置页的「待机幅度」倍率（0 = 完全不摆，只留呼吸）× 风格系数。
    // 倍率本身过一道低通：滑块一拖到底、风格一换，都不会定住或甩开，是几帧内收放。
    static float kAmp = 1.0f;
    {
        const float want = style.amp * web::settings().idle;
        kAmp += (want - kAmp) * 0.12f;
        if (fabsf(want - kAmp) < 0.002f) kAmp = want;          // 收敛后贴死，别永远差一点
    }
    const float ax = (gIdle.fAx + (gIdle.tAx - gIdle.fAx) * s) * kAmp;
    const float az = (gIdle.fAz + (gIdle.tAz - gIdle.fAz) * s) * kAmp;
    const float bz = (gIdle.fBz + (gIdle.tBz - gIdle.fBz) * s) * kAmp;
    const float br = 0.5f + 0.5f * sinf(static_cast<float>(now * 2.0 * 3.14159265358979 / 4.2));
    // 呼吸用 Set 不用 Add：ParamBreath 的默认值是 1.0，Add 上去直接顶到上限（实测一直 1.000）
    gIdle.lastAx = ax; gIdle.lastAz = az; gIdle.lastBz = bz; gIdle.lastBr = br;
    // 一律用 Set：AngleX/AngleZ 和 ParamEyeBall 一样**不会被 LoadParameters 归零**，
    // 用 Add 会每帧累加，几帧就顶死在 ±30（实测 ax 一直是 30.00）。
    if (gIdle.iBreath >= 0) model->SetParameterValue(gIdle.iBreath, br, 1.0f);
    if (gIdle.iAx >= 0)     model->SetParameterValue(gIdle.iAx, ax, 1.0f);
    if (gIdle.iAz >= 0)     model->SetParameterValue(gIdle.iAz, az, 1.0f);
    if (gIdle.iBz >= 0)     model->SetParameterValue(gIdle.iBz, bz, 1.0f);
    // 诊断：每 2 秒打一行，**在 Add 之后**读（在之前读是上一帧 Load 重置后的 0，会骗人）
    static double lastLog = -1e9;
    if (now - lastLog > 2.0) {
        lastLog = now;
        log_("[idle] 算 ax=%.2f | 写入后模型 ax=%.2f az=%.2f br=%.3f",
             ax,
             gIdle.iAx >= 0 ? model->GetParameterValue(gIdle.iAx) : -999.0f,
             gIdle.iAz >= 0 ? model->GetParameterValue(gIdle.iAz) : -999.0f,
             gIdle.iBreath >= 0 ? model->GetParameterValue(gIdle.iBreath) : -999.0f);
    }
}

// ── 视线：头 + 眼珠跟着鼠标（2026-11-09 21:xx）──────────────
// 每帧在 idleTick 之后、eyeBlink 之前跑：指针 → 相对窗口中心的归一化偏移 (-1..1)
//   · 头：ParamAngleX += nx*10 度，ParamAngleY += ny*6 度（幅度再大就像抽搐）
//   · 眼珠：ParamEyeBallX/Y += nx/ny * 0.7（这两个参数本来就是 -1..1）
//   · 一阶低通 cur += (target-cur)*min(1, dt*6)：鼠标瞬移、离开窗口回正，都靠它变软
//   · **全部走 AddParameterValue**：这 4 个参数默认值都是 0，Add 才叠在待机微动之上。
//     ParamBreath 那种默认 1.0 的才用 Set（见 idleTick 里那行注释）。
//   · MIYU_NO_LOOK=1 整套关掉（排查用：关掉后三张图应该几乎一致）
//
// 一个反直觉的点：「在窗口内」= 指针在她**身上**（input region 内）。窗口是 500x900
// 的矩形，但她只占 28%，指针落在透明区域时合成器根本不发 enter —— 平台层不知道
// 指针在哪。这是逐像素点击穿透的代价，不是 bug（第九节记了）。
struct LookRig {
    Csm::csmInt32 iAx = -1, iAy = -1, iEx = -1, iEy = -1;
    float cx = 0, cy = 0;      // 头（度，已平滑）
    float ex = 0, ey = 0;      // 眼珠（-1..1，已平滑）
    bool ready = false, on = true;
    bool wasIn = false;
};
static LookRig gLook;

static void lookInit(Csm::CubismModel* model) {
    Csm::CubismIdManager* m = Csm::CubismFramework::GetIdManager();
    gLook.iAx = model->GetParameterIndex(m->GetId("AngleX"));   // 注意：不是 ParamAngleX，那个是空壳
    gLook.iAy = model->GetParameterIndex(m->GetId("AngleY"));
    gLook.iEx = model->GetParameterIndex(m->GetId("ParamEyeBallX"));
    gLook.iEy = model->GetParameterIndex(m->GetId("ParamEyeBallY"));
    gLook.on = std::getenv("MIYU_NO_LOOK") == nullptr;
    gLook.ready = true;
    log_("[main] 视线：%s | 参数索引 angleX=%d angleY=%d eyeBallX=%d eyeBallY=%d",
         gLook.on ? "开（头 ±10°/±6°，眼珠 ±0.7，一阶低通 dt*6）" : "关（MIYU_NO_LOOK）",
         gLook.iAx, gLook.iAy, gLook.iEx, gLook.iEy);
}

static void lookTick(Csm::CubismModel* model, float dt, double now, int lw, int lh) {
    if (!gLook.ready) lookInit(model);
    if (!gLook.on) return;
    // 第十六节：网页上的「视线跟随」两件套
    //   lookOn=false → 总开关关掉：目标直接归零（同一个平滑回正），一个角度都不跟
    //   look=0–2     → 强度：眼珠幅度和身体微倾都乘它，0 等于关、2 是两倍
    const float lookK = web::settings().lookOn ? web::settings().look : 0.0f;
    // 排查用：MIYU_FORCE_PARAM="参数名=值" 直接把某个参数按死，看它到底驱动什么
    if (const char* spec = std::getenv("MIYU_FORCE_PARAM")) {
        char name[128] = {0};
        float val = 0.0f;
        if (sscanf(spec, "%127[^=]=%f", name, &val) == 2) {
            const Csm::csmInt32 idx =
                model->GetParameterIndex(Csm::CubismFramework::GetIdManager()->GetId(name));
            static const char* lastSpec = nullptr;
            if (spec != lastSpec) {
                lastSpec = spec;
                log_("[probe] 按死参数 %s = %.2f（index=%d）", name, val, idx);
            }
            if (idx >= 0) model->SetParameterValue(idx, val, 1.0f);
        }
        return;
    }

    float nx = 0.0f, ny = 0.0f;   // 不在窗口内 → 目标就是 0，同一个平滑慢慢回正
    float px = 0.0f, py = 0.0f;
    const bool in = platPointer(&px, &py) && lw > 0 && lh > 0;
    if (in) {
        // 分母优先用「她本人」的半宽（platPersonBox），拿不到才退回整块窗口。
        // 逐像素穿透之后鼠标只在她身上才算「在窗口内」，用窗口半宽只能到 ±0.5，
        // 头只偏一半幅度；换成她自己的半宽，在她身上挪一点点就是满幅。
        int pcx = 0, pcy = 0, phw = 0, phh = 0;
        float cx = static_cast<float>(lw) * 0.5f, cy = static_cast<float>(lh) * 0.5f;
        float hw = static_cast<float>(lw) * 0.5f, hh = static_cast<float>(lh) * 0.5f;
        if (platPersonBox(&pcx, &pcy, &phw, &phh) && phw > 0 && phh > 0) {
            cx = static_cast<float>(pcx); cy = static_cast<float>(pcy);
            hw = static_cast<float>(phw); hh = static_cast<float>(phh);
        }
        nx = (px - cx) / hw;
        ny = (py - cy) / hh;
        if (nx > 1.0f) nx = 1.0f;
        if (nx < -1.0f) nx = -1.0f;
        if (ny > 1.0f) ny = 1.0f;
        if (ny < -1.0f) ny = -1.0f;
    }

    // 诊断：Add 之前模型里是什么（LoadParameters 刚跑过；不为 0 就是有人在累加）
    const float preAx = gLook.iAx >= 0 ? model->GetParameterValue(gLook.iAx) : -999.0f;

    const float a = (dt * 6.0f > 1.0f) ? 1.0f : dt * 6.0f;
    // 身体微倾：±1.5（AngleX 是整个人左右平移，大一点就成「挪步」；这个量叠在待机 ±4 上
    // 只是「往鼠标那边探一点」）。眼珠 ±1.0 满幅，本来就是这个参数的范围。
    gLook.cx += (nx * 1.5f * lookK - gLook.cx) * a;
    gLook.cy += (ny * 1.5f * lookK - gLook.cy) * a;
    gLook.ex += (nx * 1.0f * lookK - gLook.ex) * a;
    gLook.ey += (ny * 1.0f * lookK - gLook.ey) * a;

    // 2026-11-09 21:5x：AngleX/AngleZ **也不会被 LoadParameters 归零**，用 Add 会累加，
    // 实测 ax 一直顶在 30.00（还带得她整个人歪）。所以这里也必须 Set：
    // 值 = 待机那本帧算出来的 + 视线偏移，两件事合成一次写。
    // 2026-11-09 22:0x：**大幅跟鼠标**被否掉（AngleX 是整体平移，-20/0/+20 连腿和尾巴
    // 都跟着挪）。第十六节改回来一点点：±1.5 的「试探性微倾」，乘 look 之后
    // look=0 / lookOn=false 时 AngleX **严格等于待机的值**（自测就比这一项）。
    if (gLook.iAx >= 0) model->SetParameterValue(gLook.iAx, gIdle.lastAx + gLook.cx, 1.0f);
    if (gLook.iAy >= 0) model->SetParameterValue(gLook.iAy, 0.0f, 1.0f);
    // 眼珠必须用 **Set**，不能用 Add —— 实测出来的，跟"默认 0 就用 Add"的经验相反：
    // ParamEyeBallX/Y（本模型索引 76/77）**不会被 LoadParameters 归零**，Add 一帧一帧累加，
    // 约 4 帧就顶到 -1/+1（日志里 eyeX 一直钉在 -1.00，平滑值才 -0.33）。
    // 这俩参数没有别的写入者（默认 0、眨眼只写 EyeLOpen），所以 Set 就是「写目标值」——
    // 等价于「在 0 上 Add」，但不累积。
    if (gLook.iEx >= 0) model->SetParameterValue(gLook.iEx, gLook.ex, 1.0f);
    if (gLook.iEy >= 0) model->SetParameterValue(gLook.iEy, gLook.ey, 1.0f);

    // 诊断：进出窗口各打一行（视线验证全靠它判断"她到底看没看见指针"），
    // 停留期间每 2 秒一行。
    if (in != gLook.wasIn) {
        gLook.wasIn = in;
        if (in) log_("[look] 指针进入 local=(%.0f,%.0f) nx=%+.2f ny=%+.2f", px, py, nx, ny);
        else    log_("[look] 指针离开，视线回正");
    }
    static double lastLog = -1e9;
    if (now - lastLog > 2.0) {
        lastLog = now;
        log_("[look] %s nx=%+.2f ny=%+.2f k=%.2f -> 身体偏移(%+.2f,%+.2f)° 眼(%+.2f,%+.2f)"
             " | 待机 ax=%+.2f 写入后 angleX=%+.2f eyeX=%+.2f%s",
             in ? "跟" : "回正", nx, ny, lookK, gLook.cx, gLook.cy, gLook.ex, gLook.ey,
             gIdle.lastAx, gLook.iAx >= 0 ? model->GetParameterValue(gLook.iAx) : -999.0f,
             gLook.iEx >= 0 ? model->GetParameterValue(gLook.iEx) : -999.0f,
             lookK <= 0.001f ? "（视线关：nx 不带动任何角度）" : "");
        (void)preAx;
    }
}

// ── 动作计划：外面推一份时间线，她照着演（2026-11-09 晚）──────────
// plan.h 只管「解析 + 排序 + 插值 + 时钟」，一行 Cubism 都不认识；写参数在这儿。
//
// 三条规矩（和第九/十一节同一个道理）：
//   · **全程 Set，绝不 Add**：AngleX/AngleZ/ParamEyeBall 这几个不会被 LoadParameters
//     归零，Add 一帧一帧累加，几帧就顶死在上下限。
//   · **计划是这一帧最后的写入者**：它排在 lookTick 之后、眨眼/physics 之前 ——
//     视线、待机正弦、SDK 的 IDLE motion 都先写完，再由计划盖过去，不然又是两套抢一个参数。
//   · 参数不在白名单里 → plan.cpp 就丢掉了，这里根本拿不到，所以不用再判一次。
//
// 起播那 0.2s 做一次交叉淡入：把接管瞬间模型里的值记成 snap[]，再按 w 线性混到
// 计划的目标值上。中途换计划也是同一段代码 —— 所以「播一半推新的」是平滑接过去的，
// 不会从 ±12 一下跳到 0。
struct PlanRig {
    Csm::csmInt32 idx[plan::kMaxParams];
    float snap[plan::kMaxParams];
    bool ready = false;
    double blendT0 = -1e9;
    bool hasSnap = false;
};
static PlanRig gPlanRig;

static void planInit(Csm::CubismModel* model) {
    Csm::CubismIdManager* m = Csm::CubismFramework::GetIdManager();
    for (int i = 0; i < plan::kMaxParams; ++i) {
        gPlanRig.idx[i] = model->GetParameterIndex(m->GetId(plan::kNames[i]));
        gPlanRig.snap[i] = 0.0f;
    }
    gPlanRig.ready = true;
    log_("[main] 计划参数索引：");
    for (int i = 0; i < plan::kMaxParams; ++i)
        log_("[main]   %-16s %d%s", plan::kNames[i], gPlanRig.idx[i],
             gPlanRig.idx[i] < 0 ? "（模型里没有，跳过）" : "");
}

// 收到一份新计划时调：清掉上一次的淡入快照，下一帧重抓。
static void planTakeover(double now) {
    gPlanRig.blendT0 = now;
    gPlanRig.hasSnap = false;
}

static Csm::CubismExpressionMotionManager* gExprMgr = nullptr;   // 前置声明：planTick 要用
static void exprPlay(const std::string& name);                   // 同上（定义在后面）

static void planTick(Csm::CubismModel* model, double now) {
    // 2026-11-09 22:3x：动作播完要把表情收回默认 —— 不然点一次脸红就一直红着。
    static bool wasActive = false;
    const bool isActive = plan::active();
    if (wasActive && !isActive) exprPlay("默认");
    wasActive = isActive;
    if (!isActive) return;
    if (!gPlanRig.ready) planInit(model);

    // 换了新计划就看它有没有指定表情（"expr":"＞＜"），有就切一次。
    // 这样「被戳一下」能同时换脸，光动身体不够明显。
    static double lastStart = -1e9;
    const double st = plan::startTime();
    if (st != lastStart) {
        lastStart = st;
        const char* en = plan::expr();
        log_("[plan] 起播「%s」%s%s", plan::title()[0] ? plan::title() : "(无题)",
             (en && *en) ? " 附带表情：" : "", (en && *en) ? en : "");
        if (en && *en) {
            // 走 exprPlay —— 和 MIYU_EXPR / ctl::takeExpr 是同一条路（自己 Create 那套
            // 不设 fadeIn、不进池子，实测切了看不出来）。
            // 网页设置页把「表情切换」关掉时，计划里带的 expr 也不切（第 14 节）。
            if (web::settings().expr) exprPlay(en);
        }
    }

    plan::Sample sm;
    if (!plan::sample(now, sm)) return;      // 播完（plan 内部已经 stop），交回待机

    // 接管那一帧的现场：模型里现在是多少（待机/视线刚写完的值）
    if (!gPlanRig.hasSnap) {
        gPlanRig.hasSnap = true;
        for (int i = 0; i < plan::kMaxParams; ++i)
            gPlanRig.snap[i] = gPlanRig.idx[i] >= 0 ? model->GetParameterValue(gPlanRig.idx[i]) : 0.0f;
    }
    const double bt = now - gPlanRig.blendT0;
    const float w = bt >= 0.2 ? 1.0f : static_cast<float>(bt < 0.0 ? 0.0 : bt / 0.2);

    for (int k = 0; k < sm.n; ++k) {
        const int slot = plan::slotOf(sm.name[k]);
        if (slot < 0 || gPlanRig.idx[slot] < 0) continue;
        float v = sm.value[k];
        if (w < 1.0f) v = gPlanRig.snap[slot] + (v - gPlanRig.snap[slot]) * w;
        model->SetParameterValue(gPlanRig.idx[slot], v, 1.0f);
    }

    static double lastLog = -1e9;
    if (now - lastLog > 1.0) {
        lastLog = now;
        char buf[160];
        int off = 0;
        for (int k = 0; k < sm.n && k < 3; ++k)
            off += std::snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off), "%s%s=%.2f",
                                 k ? " " : "", sm.name[k], sm.value[k]);
        log_("[plan] %s t=%.2f/%.2fs 写 %d 个参数（w=%.2f）| %s",
             plan::title()[0] ? plan::title() : "(无题)", now - plan::startTime(), plan::total(),
             sm.n, w, buf);
    }
}

// ── 表情：启动时 MIYU_EXPR + 运行时由 ctl 切换（2026-11-09 晚）──────
// 原来的写法是「启动读一次 MIYU_EXPR，之后再也不动」。现在收成一件事：
// 谁要表情（启动 MIYU_EXPR / socket 里的 {"expr":"前倾"}）都走 exprPlay()。
//
// 运行时切换靠的是 SDK 自己的机制，不要自己写淡入淡出：
//   CubismMotionQueueManager::StartMotion() 会给**已经在播的**每个 entry
//   打上 SetFadeout(它自己的 FadeOutTime)，然后新的 entry 排队等在后面。
//   CubismExpressionMotionManager::UpdateMotion() 按 fadeWeight 把前一个算淡、
//   算到最后一个权重到 1 才把前面的 entry 删掉。这就是「旧的自动 fadeOut」。
//   → 所以切换 = Create 一个新的 + StartMotion，别 StopAllMotions（那是硬切）。
//
// 两个坑：
//   · Create 出来的 motion 归我们管：StartMotion(m, false) 之后 SDK 不会替你删
//     （expression 的结束路径只 CSM_DELETE 队列 entry，不碰 motion 本体）。
//     全部塞进 gExprPool，退出时统一删 —— 切换次数是人给的，不会涨爆。
//   · 队列 entry 在管理器析构时才释放，所以先删管理器、再删 pool，别反过来。
static std::vector<Csm::ACubismMotion*> gExprPool;
static std::string gExprName = "(无)";
// 2026-11-09 22:5x：**空参数 exp3 清不掉表情**。表情改的那些参数（比如脸红用的那几根）
// 不会被 LoadParameters 归零，空表情什么都不写，于是红脸一直挂着。所以切走时得
// 把上一个表情碰过的参数手动写回各自的默认值。
static std::vector<std::string> gExprTouched;
static bool gExprResetPending = false;
// 第十六节：表情的停留截止时间（-1e9 = 不设防，默认表情就是它）。
// 基准 2.5s × 网页上的 exprHold（0.2–5）→ 0.5s ~ 12.5s。
static double gExprUntil = -1e9;
static constexpr double kExprBaseHold = 2.5;

// name 为空 / "默认" → 淡出回默认表情：用一份**参数表是空的** exp3 顶上，
// 它什么都不写，于是前一个表情按 fadeWeight 被算回默认值。
static const char* kDefaultExprJson =
    "{\"Type\":\"Live2D Expression\",\"FadeInTime\":0.35,\"FadeOutTime\":0.35,\"Parameters\":[]}";

static void exprPlay(const std::string& name) {
    std::string bytes;
    std::string label;
    if (name.empty() || name == "默认") {
        bytes = kDefaultExprJson;
        label = "(默认)";
    } else {
        const std::string ep = std::string(kModelDir) + "Exp3/" + name + ".exp3.json";
        std::vector<char> eb = slurp(ep);
        if (eb.empty()) {
            log_("[expr] 读不到 %s（保持现在的 %s）", ep.c_str(), gExprName.c_str());
            return;
        }
        bytes.assign(eb.begin(), eb.end());
        label = name;
    }

    if (label == "(默认)") {
        gExprResetPending = true;          // 让主循环把上次碰过的参数写回默认值
    } else {
        gExprTouched.clear();
        const std::string raw(bytes.begin(), bytes.end());
        size_t p = 0;
        while ((p = raw.find("\"Id\"", p)) != std::string::npos) {
            const size_t q = raw.find('"', raw.find(':', p) + 1);
            const size_t r = q == std::string::npos ? std::string::npos : raw.find('"', q + 1);
            if (r != std::string::npos) gExprTouched.push_back(raw.substr(q + 1, r - q - 1));
            p = r == std::string::npos ? raw.size() : r;
        }
    }

    Csm::CubismExpressionMotion* m = Csm::CubismExpressionMotion::Create(
        reinterpret_cast<const Csm::csmByte*>(bytes.data()),
        static_cast<Csm::csmSizeInt>(bytes.size()));
    if (m == nullptr) {
        log_("[expr] Create 失败 %s", label.c_str());
        return;
    }
    // exp3.json 里没写 FadeInTime 的时候默认是 0（硬切）。自己按住 0.35s，
    // 顺便让**下一个**表情切换时拿它当 fadeOut 时长（StartMotion 读的就是这个值）。
    m->SetFadeInTime(0.35f);
    m->SetFadeOutTime(0.35f);

    if (gExprMgr == nullptr) gExprMgr = new Csm::CubismExpressionMotionManager();
    // 2026-11-09 22:4x：**必须先清队列**。StartMotion 是排队不是替换，切三次以后
    // 队列里堆着好几个表情同时按权重写同一批参数，表现就是「换不了、也回不去默认」。
    // 清了之后权重从 0 起，配上 0.35s 的 fadeIn 正好是「松手回弹」那种过渡。
    // 2026-11-09 22:3x：**池子原来只进不出**（日志里一晚上涨到 34 个）。
    // StopAllMotions 已经把队列清空、管理器不再引用旧对象，所以这里可以放心回收。
    // autoDelete 仍是 false —— 本体的生死由这个池子管，管理器只管队列。
    gExprMgr->StopAllMotions();
    for (Csm::ACubismMotion* old : gExprPool) Csm::ACubismMotion::Delete(old);
    gExprPool.clear();
    gExprMgr->StartMotion(m, false /* autoDelete：归 gExprPool 管 */);
    gExprPool.push_back(m);
    gExprName = label;
    log_("[expr] -> %s（fade 0.35s，池里 %zu 个）", label.c_str(), gExprPool.size());

    // 第十六节的「表情停留时长」：以前只有动作计划结束会收回默认，socket/页面推来的表情
    // 会一直挂着（点一次脸红红到重启）。现在起一个截止时间，到点自己淡回默认。
    // 计划在播时不算时间（那时候由 planTick 负责收尾），所以主循环那边也要判 plan::active()。
    gExprUntil = (label == "(默认)") ? -1e9
                                    : platTime() + kExprBaseHold * double(web::settings().exprHold);
}

// ── 帧率上限（第十六节）────────────────────────────────
// 每帧末尾调一次：睡到「下一帧该来的时刻」。为什么要自己睡 —— 这个 EGL surface 在
// niri 下 eglSwapBuffers 不等 vsync，不睡就是几百到上千 fps（空转烧 CPU）。
// 三条细节：
//   · 落后超过 0.25s（切输出、拖动、第一次编译）就重设基准，不追帧；
//   · 上限变了（页面切 15/30/60）立刻重设基准，不因为旧基准睡一长觉；
//   · 每 5 秒打一行实测帧率，验「fps 真的生效」靠它，不靠肉眼。
static void framePace(int cap, double now) {
    static double t0 = -1e9;
    static unsigned long n = 0;
    static int lastCap = 0;
    if (t0 < -1e8) t0 = now;
    if (cap != lastCap) { lastCap = cap; log_("[fps] 上限 -> %d", cap); }
    ++n;
    // 基准是**本帧开头**的 now，不是累加出来的 nextFrame：
    // 上一版用 nextFrame += period 累加，被 eglSwapBuffers（niri 下会等 frame callback，
    // 每帧至少 16.7ms）一挤就永远落后，wait 恒为负 → 15 档实测跑出 29 帧（实测记录）。
    // 现在每帧重算「这一帧该占多久」，剩下的时间才睡，稳在目标值附近。
    // 目标时刻按周期累加；但只要这一帧已经落后超过 1.5 个周期（合成器 vsync 比周期还慢、
    // 或者这一帧本身干得久），就把基准拉回 now —— 不追帧、不叠加延迟。
    static double nextFrame = -1e9;
    const double period = 1.0 / double(cap);
    if (nextFrame < -1e8 || now - nextFrame > period * 1.5) nextFrame = now;
    nextFrame += period;
    const double wait = nextFrame - platTime();
    if (wait > 0.0005) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = static_cast<long>(wait * 1e9);
        nanosleep(&ts, nullptr);
    }
    if (now - t0 >= 5.0) {
        log_("[fps] 实测 %.1f 帧/秒（上限 %d，%lu 帧 / %.1fs）",
             double(n) / (now - t0), cap, n, now - t0);
        n = 0;
        t0 = now;
    }
}

// ── 本地设置页（web.cpp）真正生效的那一半 ────────────────────────────
// web 只管「收 HTTP → 改值 → 落盘」，然后喊一声 key；重活按 key 分给下面：
//   scale  → 重建 wl_egl_window + buffer_scale（platform）
//   tap    → 平台层的点击开关（等价 MIYU_NO_TAP，但运行时可变）
//   corner → platform 吸角（tl/tr/bl/br/free）
//   hide   → 全透明 + 输入区清空（platform 清区域，主循环那边整帧不画模型）
//   alpha / idle / expr → 这三项每帧直接读 web::settings()，不需要额外动作
//   ── 第十六节新增的九项 ──────────────────────────────────────
//   idleStyle / look / lookOn / mouth / exprHold / voice / vol → 每帧读，不需要额外动作
//   blink → 主循环每帧比一次 bk，变了才 SetBlinkingInterval
//   fps   → framePace 自己检测 cap 变化
//   所以 webApply 只认下面四个「要立刻重建/搬窗口」的，其余走一行日志就够。
static void webApply(const char* key) {
    if (!strcmp(key, "scale"))       platSetScale(web::settings().scale);
    else if (!strcmp(key, "tap"))    platSetTapEnabled(web::settings().tap);
    else if (!strcmp(key, "corner")) platSnapCorner(web::settings().corner.c_str());
    else if (!strcmp(key, "hide"))   platSetHidden(web::settings().hide);
    log_("[web] 生效：%s", key);
}

// 「切个表情」按钮要的是「有哪几个表情」：扫 model/Exp3/*.exp3.json 的文件名。
// 排个序，这样每次点都是同一个循环顺序，不会随机跳。
static std::vector<std::string> scanExpressions(const char* dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir);
    if (!d) { log_("[web] 扫不到表情目录 %s（那个按钮会空转）", dir); return out; }
    const std::string suf = ".exp3.json";
    while (struct dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() > suf.size() &&
            n.compare(n.size() - suf.size(), suf.size(), suf) == 0)
            out.push_back(n.substr(0, n.size() - suf.size()));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    log_("[web] 表情 %zu 个（切个表情按这个顺序循环）", out.size());
    return out;
}

int main() {
    // ── 窗口（窗口层接口，实现在 platform_x11.cpp）────────────
    if (!platInit(500, 900, "native2-test")) { log_("[main] 窗口层起不来"); return 1; }
    glewExperimental = GL_TRUE;
    const GLenum ge = glewInit();
    // ge 的含义：
    //   X11 版 → 0 (GLEW_OK)
    //   Wayland 版 → 4 (GLEW_ERROR_NO_GLX_DISPLAY)，**这是预期，不是错**。
    //     EGL 下没有 GLX display，但 GLEW 退回 glXGetProcAddress，那一步不需要 display，
    //     指针全都解析到了（实测 44 个常用入口 0 个空指针），照常画。
    log_("[main] GL=%s | %s | glew=%d", glGetString(GL_VERSION), glGetString(GL_RENDERER), ge);
    // 2026-11-09 20:1x：初始位置原来写死 (1900,150)，那是 X11 时代的屏幕坐标；
    // Wayland 的 layer-shell margin 是「相对单个 output」的，1900 在 1707 宽的逻辑屏上
    // 直接跑到屏外。改成读环境变量，默认落在屏幕左半边、竖直中间偏下一点。
    {
        const char* ex = getenv("MIYU_X");
        const char* ey = getenv("MIYU_Y");
        platSetPosition(ex ? atoi(ex) : 640, ey ? atoi(ey) : 120);
    }

    // ── 本地设置页：起服务 + 把落盘的设置先落一遍 ──────────────────
    // 顺序讲究：web::init 会读 state/settings.json，读回来的值要在这里就应用，
    // 否则「重启服务后仍是改后的值」只有页面显示对、实际行为回到默认。
    web::setExprNames(scanExpressions((std::string(kModelDir) + "Exp3").c_str()));
    web::init(&webApply);
    {
        // MIYU_NO_TAP 是命令行上的一次性覆盖，压过设置文件的 tap（排查时它该说了算）
        if (getenv("MIYU_NO_TAP") && web::settings().tap) web::settings().tap = false;
        const web::Settings& s0 = web::settings();
        platSetTapEnabled(s0.tap);
        if (s0.scale != 1.0f) platSetScale(s0.scale);
        if (s0.corner != "free") platSnapCorner(s0.corner.c_str());
        if (s0.hide) platSetHidden(true);
    }

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

    // ── 眨眼 ────────────────────────────────────────────────
    // Create() 收的是 ICubismModelSetting*，不是文件路径 —— 我们已经有解析好的 setting，
    // 直接交进去；它自己遍历 Groups 里 Target=="EyeBlink" 的 Ids
    // （本模型：ParamEyeLOpen / ParamEyeROpen）。
    // MIYU_NO_EYES=1 关掉，方便排查。
    const bool kNoEyes = std::getenv("MIYU_NO_EYES") != nullptr;
    Csm::CubismEyeBlink* eyeBlink = nullptr;
    if (kNoEyes) {
        log_("[main] 眨眼：MIYU_NO_EYES 已设 —— 关");
    } else {
        eyeBlink = Csm::CubismEyeBlink::Create(&setting);
        // 默认 4s 间隔、闭 0.1 / 合 0.05 / 开 0.15，一次眨眼 0.3s。
        const Csm::csmVector<Csm::CubismIdHandle>& ids = eyeBlink->GetParameterIds();
        std::string idl;
        for (Csm::csmUint32 i = 0; i < ids.GetSize(); ++i) { idl += ids[i]->GetString().GetRawString(); idl += ' '; }
        log_("[main] 眨眼：参数 %u 个 [%s]", static_cast<unsigned>(ids.GetSize()), idl.c_str());
    }

    // ── 待机动作：**只**播 IDLE ──────────────────────────────
    // model3.json 的 Motions 是空的（那 8 个 motion3 都没登记），所以只能自己 Create。
    // ⚠️ 绝对不能加载：脱捕.motion3.json / 举手切默认手 / 狗尾草* / 竖手指* ——
    //    它们是给另一版模型做服装/体型切换的，播了会让人散架。
    const bool kNoIdle = std::getenv("MIYU_NO_IDLE") != nullptr;
    // 2026-11-09 21:3x：默认播 Animation/脱捕.motion3 —— 13.15s、loop=true，
    // 驱动 AngleX/Y/Z + PositionX/Y/Z + 手臂四关节 + 眼睛嘴 + 呆滞/光效，
    // 是这整套素材里唯一一段「完整的动作」。之前把它当「会散架」是误判。
    // 2026-11-09 21:4x：这些都是**特殊动作**（脱捕/举手/竖手指/狗尾草），不该当待机一直播。
    // 所以默认回到手写正弦微动；要播动作显式设 MIYU_MOTION=文件名。
    //   MIYU_MOTION=脱捕.motion3.json   播指定动作（循环）
    //   MIYU_NO_IDLE=1                  全关，只有 physics
    const char* mfNow = std::getenv("MIYU_MOTION");
    const bool kUseSine = (mfNow == nullptr);
    const bool kUseLegacyIdle = false;
    Csm::CubismMotionManager* motionManager = nullptr;
    Csm::CubismMotion* idleMotion = nullptr;
    if (kNoIdle) {
        log_("[main] 待机：MIYU_NO_IDLE 已设 —— 关");
    } else if (kUseSine) {
        log_("[main] 待机：手写正弦微动（MIYU_IDLE_SINE）");
    } else {
        // MIYU_MOTION=文件名（不带路径）可以指定播哪一段，默认脱捕
        const char* mf = std::getenv("MIYU_MOTION");
        const std::string ip = std::string(kModelDir) + "Animation/" +
            (mf ? mf : (kUseLegacyIdle ? "IDLE.motion3.json" : "脱捕.motion3.json"));
        std::vector<char> ib = slurp(ip);
        if (ib.empty()) {
            log_("[main] 待机动作：读不到 %s", ip.c_str());
        } else {
            idleMotion = Csm::CubismMotion::Create(
                reinterpret_cast<const Csm::csmByte*>(ib.data()),
                static_cast<Csm::csmSizeInt>(ib.size()));
        }
        if (idleMotion == nullptr) {
            log_("[main] 待机动作：CubismMotion::Create 失败");
        } else {
            // motion3 里 Meta.Loop 是 true，但 CubismMotion::Create **不读它**
            // （SDK 里那行是注释掉的）→ 不自己 SetLoop(true) 就 5.983s 停住不动。
            idleMotion->SetLoop(true);
            idleMotion->SetLoopFadeIn(true);   // 循环接缝不跳
            idleMotion->SetFadeInTime(1.0f);   // 从静态站姿淡入，别一上来就弹
            idleMotion->SetFadeOutTime(1.0f);
            motionManager = new Csm::CubismMotionManager();
            const Csm::CubismMotionQueueEntryHandle h =
                motionManager->StartMotionPriority(idleMotion, false /* autoDelete */, 1 /* priority */);
            log_("[main] 待机动作：%s %zu 字节，时长 %.3fs，loop=1，句柄=%p", ip.c_str(),
                 ib.size(), idleMotion->GetLoopDuration(), h);
        }
    }

    // ── 表情（Exp3/*.exp3.json）────────────────────────────
    // 2026-11-09 22:0x：作者给的 23 个表情才是「她怎么看你」的正路 —— 尤其「前倾」。
    // MIYU_EXPR=前倾 启动就播那一个；之后 socket 里 {"expr":"..."} 可以随时换。
    // 2026-11-09 晚：创建逻辑收进 exprPlay()，启动和运行时走同一条路。
    if (const char* en = std::getenv("MIYU_EXPR")) {
        exprPlay(en);
    }

    // 眨眼状态翻转打点（一次眨眼两行；不进每帧日志）
    const Csm::csmInt32 eyeLi = model->GetParameterIndex(
        Csm::CubismFramework::GetIdManager()->GetId("ParamEyeLOpen"));
    bool eyesClosed = false;

    // 嘴：ParamMouthOpenY (0..1)，模型里存在（第 2 节参数表）。ctl 算好包络，这里只 Set。
    // socket 在这里建：模型/贴图都过了，接下来才有人往口子里推话。
    ctl::init();
    const Csm::csmInt32 mouthI = model->GetParameterIndex(
        Csm::CubismFramework::GetIdManager()->GetId("ParamMouthOpenY"));
    log_("[main] 嘴参数 ParamMouthOpenY 索引=%d（socket %s）", mouthI, ctl::sockPath());

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

        // 网页设置页：每帧收一个连接（非阻塞，最多两个 accept，绝不卡渲染）
        web::poll(now);
        const web::Settings& ws = web::settings();
        // 第十六节：这两项不归渲染管，直接喂给 ctl —— 说话那一下读的就是它们
        // （内部只在值变了才打日志，每帧调不刷屏）。
        ctl::setMouthScale(ws.mouth);
        ctl::setVoice(ws.voice, ws.vol);
        {   // 状态栏要的：输出名 + 窗口逻辑尺寸
            int lw = 0, lh = 0;
            platGetLogicalSize(&lw, &lh);
            web::setStatus(platOutputLabel(), lw, lh);
        }

        // 隐藏：整帧不画模型，只清成透明再提交 —— 像素层面和空桌面完全一致，
        // 输入区在 platform 那边已经清空（点击穿透）。模型状态冻结在最后一帧。
        // 不 continue 的话下面每个参数都要加一层 if，反而更容易漏。
        // 第十六节：隐藏期间也照样按 fps 上限睡，不然她「不在」的时候反而最烧 CPU。
        if (ws.hide) {
            platSwapBuffers();
            platPollEvents();
            framePace(ws.fps, platTime());
            continue;
        }

        // 2026-11-09：投影必须按**窗口宽高比**校正，光用画布尺寸会把模型拉扁。
        // 做法：先把画布等比缩放到窗口的 95%，再换成 NDC 系数。
        const float sH = static_cast<float>(H) * 0.95f / canvasH;
        const float sW = static_cast<float>(W) * 0.95f / canvasW;
        const float k = (sH < sW) ? sH : sW;
        Csm::CubismMatrix44 proj;
        proj.Scale(2.0f * k / static_cast<float>(W), 2.0f * k / static_cast<float>(H));

        // 顺序是死规矩：LoadParameters → 动作 → 眨眼 → physics → Update。
        // 眨眼必须夹在动作之后（动作会写参数，眨眼要压过它）、Update 之前（Update 才把
        // 参数算进顶点；反了就是不生效，而且不报错）。
        model->LoadParameters();

        // 本地控制口：非阻塞收一行（一行一条 JSON）。放这么早是因为下面几件事都吃它：
        // 表情请求要在 UpdateMotion 之前切、嘴的包络要在表情之后写。
        ctl::poll(now);
        std::string exprReq;
        // 设置页把「表情切换」关掉时，socket 推来的 expr 也不切（请求仍然被吃掉，
        // 不留到下次打开时突然补一刀）
        if (ctl::takeExpr(exprReq) && ws.expr) exprPlay(exprReq);
        std::string planReq;
        if (ctl::takePlan(planReq)) {
            if (planReq.empty()) {
                plan::stop();                       // "plan":null
            } else {
                plan::start(planReq, now);
                if (plan::active()) planTakeover(now);   // 起播/换播：记下淡入起点
            }
        }

        if (motionManager) motionManager->UpdateMotion(model, dt);
        // 手写正弦待机：计划在播时整段让位（别两套一起写 AngleX/AngleZ）
        else if (kUseSine) idleTick(model, now);   // 计划在播时它自己只写呼吸（见 idleTick）
        if (gExprMgr) gExprMgr->UpdateMotion(model, dt);   // 表情在动作之后、眨眼之前
        if (gExprResetPending) {
            gExprResetPending = false;
            for (const auto& nm : gExprTouched) {
                const Csm::csmInt32 idx = model->GetParameterIndex(Csm::CubismFramework::GetIdManager()->GetId(nm.c_str()));
                if (idx >= 0) model->SetParameterValue(idx, model->GetParameterDefaultValue(idx));
            }
            gExprTouched.clear();
        }
        // 嘴：在表情之后写（表情要是不小心也写了 ParamMouthOpenY，说话优先），
        // 眨眼之前（眨眼只碰眼睛，顺序其实无所谓，就近放）。一律 Set —— 这个参数
        // 不会被 LoadParameters 归零，Add 会一帧一帧累加顶死，和 ParamEyeBall 一个毛病。
        // 计划要是也驱动嘴（白名单里有 ParamMouthOpenY），静默时别用 0.02 去盖它；
        // 正在说话则照旧 —— 说话优先于计划（和第十一节的规矩一致）。
        const bool planDrivesMouth = plan::active() && plan::drives("ParamMouthOpenY");
        if (mouthI >= 0) {
            const float mv = ctl::mouth(now);
            if (mv >= 0.0f || !planDrivesMouth)
                model->SetParameterValue(mouthI, mv < 0.0f ? 0.02f : mv, 1.0f);   // 不说时只有一点呼吸
        }
        // 视线：夹在待机微动之后（叠在它上面，不覆盖）、眨眼之前。
        // 用逻辑尺寸归一化 —— platPointer 给的是 surface-local 逻辑 px，不是物理 px。
        int lw = 0, lh = 0;
        platGetLogicalSize(&lw, &lh);
        lookTick(model, dt, now, lw, lh);
        // 计划：最后一个写参数的（盖过待机/IDLE motion/视线），之后才是眨眼和 physics
        planTick(model, now);
        // 计划要是也写了嘴，正在说话时把嘴抢回来说话那一边（顺序上计划在后面，所以补这一下）
        if (mouthI >= 0 && planDrivesMouth) {
            const float mv = ctl::mouth(now);
            if (mv >= 0.0f) model->SetParameterValue(mouthI, mv, 1.0f);
        }
        // 第十六节：表情到点回收（只有「计划在播」时跳过 —— 那时候由 planTick 收尾，
        // 计划附带的那个表情不该在中途被计时器拽回默认）。
        if (gExprUntil > -1e9 && now >= gExprUntil && !plan::active()) {
            log_("[expr] 停留 %.1fs 到点（exprHold=%.2f），回默认",
                kExprBaseHold * double(ws.exprHold), ws.exprHold);
            gExprUntil = -1e9;
            exprPlay("默认");
        }
        if (eyeBlink) {
            // 第十六节：眨眼频率倍率。SDK 的间隔是「基准 4s」，真实间隔 0~2×基准 随机，
            // 所以倍率是反着除：blink=2 → 基准 2s（0~4s 一次，明显更勤），blink=0.5 → 8s。
            // blink≈0 直接**不调 UpdateParameters**（让它自己眨会永远闭着），
            // 眼睛参数由 LoadParameters 保持默认（睁），日志打点照旧 —— 所以是「0 条闭眼」。
            static float lastBlinkK = -1.0f;
            const float bk = ws.blink;
            if (bk != lastBlinkK) {
                lastBlinkK = bk;
                if (bk > 0.001f) eyeBlink->SetBlinkingInterval(4.0f / bk);
                log_("[blink] 频率倍率 %.2f -> 间隔基准 %.2fs（0 = 不眨）",
                     bk, bk > 0.001f ? 4.0f / bk : 0.0f);
            }
            if (bk > 0.001f) eyeBlink->UpdateParameters(model, dt);
            if (eyeLi >= 0) {
                const float v = model->GetParameterValue(eyeLi);
                if ((v < 0.5f) != eyesClosed) {
                    eyesClosed = (v < 0.5f);
                    log_("[blink] %s ParamEyeLOpen=%.3f t=%.2fs", eyesClosed ? "闭" : "睁", v, now);
                }
            }
        }
        if (physics && std::getenv("MIYU_NO_PHYS") == nullptr) physics->Evaluate(model, dt);
        model->Update();
        renderer->SetMvpMatrix(&proj);
        renderer->DrawModel();

        // ── 整体不透明度（网页设置页的 alpha，第 14 节）───────────────────
        // 「乘到最终颜色上」用一次全屏乘算做完，别去动每个 drawable 的 opacity：
        // 桌面 GL 的 framebuffer 带 alpha，混合是 premultiplied（GL_ONE,
        // ONE_MINUS_SRC_ALPHA），所以 dst = dst * k 同时缩了颜色和 alpha ——
        // 对合成器就是一个整体半透明的浮层，背后桌面照常透出来。
        //   glBlendFunc(GL_ZERO, GL_SRC_COLOR) → dst = 0*src + dst*srcColor = dst*k
        // 固定管线在 3.3 兼容画像里还在（EGL context 就是 COMPATIBILITY，见 platform），
        // 但 Cubism 那边留着 program 和 GL_TEXTURE_2D 绑定，得先拆掉。
        // 只在 k<1 时做：k=1 时这一趟纯属浪费（每帧一次全屏写）。
        if (ws.alpha < 0.999f) {
            glUseProgram(0);
            glDisable(GL_TEXTURE_2D);
            glBlendFunc(GL_ZERO, GL_SRC_COLOR);
            glColor4f(ws.alpha, ws.alpha, ws.alpha, ws.alpha);
            glMatrixMode(GL_PROJECTION);
            glPushMatrix();
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glPushMatrix();
            glLoadIdentity();
            glBegin(GL_QUADS);
            glVertex2f(-1.0f, -1.0f);
            glVertex2f( 1.0f, -1.0f);
            glVertex2f( 1.0f,  1.0f);
            glVertex2f(-1.0f,  1.0f);
            glEnd();
            glPopMatrix();
            glMatrixMode(GL_PROJECTION);
            glPopMatrix();
            glMatrixMode(GL_MODELVIEW);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // 复原，下面 input region 还要读
        }

        // ── 输入区域：只有她身上的像素接点击，其它地方点击穿透 ──────────
        // 每 10 帧（或 framebuffer 尺寸变了）把 back buffer 的 alpha 读回来，
        // 交给平台层转成 wl_surface 的 input region。
        //   · 必须在 platSwapBuffers 之前读 —— 读的是刚画完的这张
        //   · 逻辑尺寸 / 上下翻转由 platSetInputRegionFromAlpha 负责（它知道 scale）
        //   · MIYU_NO_INPUT_REGION=1 时整块跳过（整窗可点，用来对照排查）
        //   · **拖动期间整块跳过**：她一个像素都没变，重算纯浪费（一次读回 1000x1800
        //     再逐像素扫，是这层最贵的一步）；松手那一帧立刻补算一次。
        static const bool kNoRegion = std::getenv("MIYU_NO_INPUT_REGION") != nullptr;
        static int lastRW = -1, lastRH = -1;
        static unsigned long frameNo = 0;
        static bool wasDragging = false;
        static unsigned long dragFrames = 0;
        static bool wasHidden = false;
        const bool dragging = platIsDragging();
        const bool justReleased = wasDragging && !dragging;   // 松手这一帧
        // 取消隐藏那一帧也要立刻重算：隐藏期间平台层把输入区清空过（点击穿透），
        // 不补这一下她会「看得见但点不到」，要等 10 帧后才恢复。
        const bool justShown = wasHidden && !ws.hide;
        wasDragging = dragging;
        wasHidden = ws.hide;
        if (dragging) ++dragFrames;
        if (!kNoRegion && !ws.hide) {
            ++frameNo;
            if (!dragging &&
                (justReleased || justShown || frameNo == 1 || frameNo % 10 == 0 ||
                 W != lastRW || H != lastRH)) {
                static std::vector<unsigned char> pix;
                pix.resize(static_cast<size_t>(W) * static_cast<size_t>(H) * 4);
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
                platSetInputRegionFromAlpha(pix.data(), W, H, 8);
                if (W != lastRW || H != lastRH || justReleased || justShown)
                    log_("[main] 输入区重算（第 %lu 帧，framebuffer %dx%d%s，拖动期间跳过 %lu 帧）",
                         frameNo, W, H, justReleased ? "，刚松手" : (justShown ? "，刚取消隐藏" : ""),
                         dragFrames);
                if (justReleased) dragFrames = 0;
                lastRW = W;
                lastRH = H;
            }
        }

        platSwapBuffers();
        platPollEvents();
        // 第十六节：帧率上限（15/30/60）。睡在这里而不是循环开头 —— 这一帧的活已经干完。
        framePace(ws.fps, platTime());
    }
    web::shutdown();   // 先关设置页的监听，下面才是窗口/GL
    platShutdown();

    // ── 收尾：拿到的都还回去（顺序：渲染器 → 模型 → moc → 框架）────
    if (motionManager) {
        motionManager->StopAllMotions();
        delete motionManager;
        motionManager = nullptr;
    }
    if (idleMotion) { Csm::ACubismMotion::Delete(idleMotion); idleMotion = nullptr; }
    // 表情：先删管理器（它析构时扔掉队列 entry，不会碰 motion 本体 —— 我们 StartMotion
    // 时传的 autoDelete=false），再删池里那些自己 Create 出来的 motion。
    if (gExprMgr) { delete gExprMgr; gExprMgr = nullptr; }
    for (Csm::ACubismMotion* m : gExprPool) Csm::ACubismMotion::Delete(m);
    gExprPool.clear();
    if (eyeBlink) { Csm::CubismEyeBlink::Delete(eyeBlink); eyeBlink = nullptr; }
    Csm::Rendering::CubismRenderer::Delete(base);
    moc->DeleteModel(model);
    Csm::CubismMoc::Delete(moc);
    Csm::CubismFramework::Dispose();
    log_("[main] 收尾完成");
    return 0;
}
