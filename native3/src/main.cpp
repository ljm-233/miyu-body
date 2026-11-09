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
#include <cmath>      // 后处理：lround / sin / sqrt（第十七节）
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
        const double fps = double(n) / (now - t0);
        log_("[fps] 实测 %.1f 帧/秒（上限 %d，%lu 帧 / %.1fs）", fps, cap, n, now - t0);
        web::setFps(fps);          // 十八：设置页顶栏的「帧率」就吃这一个数（同一个 5 秒窗口）
        n = 0;
        t0 = now;
    }
}

// ══════════════════════════════════════════════════════════════════════
// 十七、离屏 FBO + SSAA + 后处理三件（地面软阴影 / 轮廓光 / 粒子）
// 设计稿：NOTES 第十五节；实现决定与实测：NOTES 第十七节。
//
// 四个关键决定（每个都有代价，写在这里免得下次又试一遍）：
//  ① 代码顺序是「模型先画、背景三件后垫」：三件用 blend(ONE_MINUS_DST_ALPHA, ONE)
//     画到模型**下面**（dst 盖住 src）。视觉上和设计稿的「画在模型之前」逐像素等价
//     （被模型挡住的地方 src 贡献 0），但换来两件设计稿当时没想到的东西：
//       · 抄给轮廓光的 alpha 是**纯模型 alpha**，阴影/粒子的 alpha 不会被当成她的轮廓去膨胀；
//       · 输入区（点击穿透掩码）可以继续用阈值 8，影子不会在桌面垫出一块假的可点区域。
//  ② 轮廓光读的是**上一帧**抄下来的 prevTex：715 个 drawable 再画一遍 ≈2ms CPU，
//     那是这层最贵的一块。一帧延迟在「每帧位移 <1px」的呼吸/待机里看不出来。
//  ③ alpha 全程 premultiplied：所有 pass 用 (ONE, ONE_MINUS_SRC_ALPHA)，合成那一趟
//     是「纹理 × 颜色」全乘（GL_MODULATE），RGB 和 A 一起缩 —— 边缘不发黑。
//  ④ FBO 重建（改 scale / 改 ssaa / 换屏换尺寸）在帧内同步做完，失败就退回直接画窗口，
//     不丢帧、不崩。
// ══════════════════════════════════════════════════════════════════════
namespace postfx {

struct P { float bx, y, vy, ph, freq, amp, r, life, age; };

struct Ctx {
    // 离屏目标（SSAA）
    GLuint fbo = 0, tex = 0;
    int    dw = 0, dh = 0;
    // 上一帧的**纯模型**合成图 + 它的 FBO（轮廓光取 alpha、输入区也取它）
    GLuint prevTex = 0, prevFbo = 0;
    int    pw = 0, ph = 0;
    // 输入区用的降采样目标（窗口物理尺寸，和以前从窗口 buffer 读回来的东西等价）
    GLuint alphaTex = 0, alphaFbo = 0;
    int    aw = 0, ah = 0;
    bool   prevValid = false;

    // 十八：FXAA 的输出目标（和输入同尺寸，逐像素一一对应）。只有在 aa=fxaa 时才分配。
    GLuint fxTex = 0, fxFbo = 0;
    int    fxw = 0, fxh = 0;
    GLuint progFxaa = 0;
    GLint  uFxTex = -1, uFxTexel = -1;

    // 十九：SMAA 1x —— 两张官方查找表 + 三遍 pass + 三个同尺寸目标。
    // edges 是 RG8（r = 西边、g = 北边有边），weights 是 RGBA8（r/g 左右、b/a 上下），
    // out 是最终结果（交给 composite 当 src，和 fxTex 一个角色）。
    GLuint smaaAreaTex = 0, smaaSearchTex = 0;
    bool   smaaOk = false;                  // 表读到了 && 三个 shader 都编出来了
    GLuint progSmaaEdge = 0, progSmaaBlend = 0, progSmaaNeigh = 0;
    GLint  uSeTex = -1, uSeTexel = -1;
    GLint  uSbEdges = -1, uSbArea = -1, uSbSearch = -1, uSbTexel = -1, uSbSize = -1;
    GLint  uSnColor = -1, uSnBlend = -1, uSnTexel = -1;
    GLuint smaaEdgesTex = 0, smaaEdgesFbo = 0;
    GLuint smaaBlendTex = 0, smaaBlendFbo = 0;
    GLuint smaaOutTex = 0, smaaOutFbo = 0;
    int    smaaW = 0, smaaH = 0;

    GLuint shadowTex = 0;
    GLuint progRim = 0, progPt = 0;
    GLuint vaoQuad = 0, vboQuad = 0;
    GLuint vaoPt = 0, vboPt = 0;
    GLint  uRimTex = -1, uRimTexel = -1, uRimR = -1, uRimColor = -1, uRimStr = -1;
    GLint  uPtColor = -1;

    // 坐标系：lw/lh = 逻辑（surface-local，y 向下）；ww/wh = 窗口物理；
    // dw/dh = FBO 尺寸（只在 offscreen 时有意义）；tw/th = **本帧实际在画的那块**的尺寸
    // （ssaa=off 时等于窗口物理尺寸 —— capturePrev 必须按它抄，按 dw 抄会越界读成垃圾）
    int    lw = 1, lh = 1, ww = 1, wh = 1, tw = 1, th = 1;
    float  pxl = 1.0f;          // 绘制像素 / 逻辑像素

    std::vector<P> ps;
    int    lastCount = -1;
    bool   warnedPrev = false;
};
static Ctx C;

static float rnd01() { return float(rand()) / float(RAND_MAX); }
static float rnd(float a, float b) { return a + (b - a) * rnd01(); }

// 逻辑坐标（左上原点、y 向下）→ NDC
static float nx(float lx) { return (lx / float(C.lw)) * 2.0f - 1.0f; }
static float ny(float ly) { return 1.0f - (ly / float(C.lh)) * 2.0f; }

// 十八：aa 的值 → 离屏倍率。fxaa 在 **1x** 上做（它治的是「贴图自带的 alpha 渐变」这类
// 像素级过渡，靠邻域亮度混合，不靠超采样；开 2x 再 fxaa 只是白多花一倍填充率）。
// 老的 ssaa 值（"2x"/"3x"）也认 —— 旧 settings.json / 旧脚本传进来不能炸。
static int factorOf(const std::string& s) {
    if (s == "ssaa2x" || s == "2x") return 2;
    if (s == "ssaa3x" || s == "3x") return 3;
    return 1;                    // off / fxaa / 不认识的一律 1x
}

static bool fxaaWanted(const std::string& s) { return s == "fxaa"; }
// 十九：aa=smaa。查找表没读到时这里仍然是 true（用户的意思），可用性由
// smaaAvailable() 单独回答 —— 两件事别混，否则日志里说不清是「没选」还是「坏了」。
static bool smaaWanted(const std::string& s) { return s == "smaa"; }

static GLuint mkShader(GLenum t, const char* src, const char* tag) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char b[1024];
        GLsizei n = 0;
        glGetShaderInfoLog(s, static_cast<GLsizei>(sizeof b), &n, b);
        log_("[post] %s 着色器编译失败: %s", tag, b);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint mkProgram(const char* vs, const char* fs, const char* tag) {
    const GLuint v = mkShader(GL_VERTEX_SHADER, vs, tag);
    const GLuint f = mkShader(GL_FRAGMENT_SHADER, fs, tag);
    if (v == 0 || f == 0) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char b[1024];
        GLsizei n = 0;
        glGetProgramInfoLog(p, static_cast<GLsizei>(sizeof b), &n, b);
        log_("[post] %s 链接失败: %s", tag, b);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ── 阴影渐变贴图：单位圆、premultiplied、峰值 alpha 0.55 ─────────────
// 峰值压到 0.55 是因为「强度 = 设置值」要能拉到 1：真按 1.0 就是一块硬黑饼，
// 而且在浅色壁纸上会像贴了张纸。0.55 × 1.0 在 #0a0c14 下已经够沉，0.55 × 0.28（默认）是柔的一层。
static const int    kShadowN = 128;
static const float  kShadowPeak = 0.55f;

static void buildShadowTex() {
    std::vector<unsigned char> px(static_cast<size_t>(kShadowN) * kShadowN * 4);
    // 颜色 #0a0c14
    const float cr = 10.0f / 255.0f, cg = 12.0f / 255.0f, cb = 20.0f / 255.0f;
    for (int y = 0; y < kShadowN; ++y) {
        for (int x = 0; x < kShadowN; ++x) {
            const float u = (x + 0.5f) / kShadowN * 2.0f - 1.0f;
            const float v = (y + 0.5f) / kShadowN * 2.0f - 1.0f;
            float a = 1.0f - std::sqrt(u * u + v * v);
            if (a < 0.0f) a = 0.0f;
            a = a * a * (3.0f - 2.0f * a);          // smoothstep，边缘软
            a *= kShadowPeak;
            unsigned char* o = &px[(static_cast<size_t>(y) * kShadowN + x) * 4];
            o[0] = static_cast<unsigned char>(cr * a * 255.0f + 0.5f);
            o[1] = static_cast<unsigned char>(cg * a * 255.0f + 0.5f);
            o[2] = static_cast<unsigned char>(cb * a * 255.0f + 0.5f);
            o[3] = static_cast<unsigned char>(a * 255.0f + 0.5f);
        }
    }
    glGenTextures(1, &C.shadowTex);
    glBindTexture(GL_TEXTURE_2D, C.shadowTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kShadowN, kShadowN, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static const char* kRimVS =
    "#version 330\n"
    "layout(location=0) in vec2 aPos;\n"
    "out vec2 vUV;\n"
    "void main(){ vUV = aPos * 0.5 + 0.5; gl_Position = vec4(aPos, 0.0, 1.0); }\n";

// 8 方向膨胀 − 原 alpha = 边缘环。掩码用 smoothstep(0.02,0.30)：prevTex 里除了她
// 只有背景透明，0.02 以上就算「有东西」，软边不硬切。
static const char* kRimFS =
    "#version 330\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "uniform float uR;\n"
    "uniform vec3 uColor;\n"
    "uniform float uStrength;\n"
    "out vec4 o;\n"
    "float m(vec2 uv){ return smoothstep(0.02, 0.30, texture(uTex, uv).a); }\n"
    "void main(){\n"
    "  float c = m(vUV);\n"
    "  float d = 0.0;\n"
    "  vec2 r = uTexel * uR;\n"
    "  float k = 0.70710678;\n"
    "  d = max(d, m(vUV + vec2( r.x, 0.0)));\n"
    "  d = max(d, m(vUV + vec2(-r.x, 0.0)));\n"
    "  d = max(d, m(vUV + vec2(0.0,  r.y)));\n"
    "  d = max(d, m(vUV + vec2(0.0, -r.y)));\n"
    "  d = max(d, m(vUV + vec2( r.x*k,  r.y*k)));\n"
    "  d = max(d, m(vUV + vec2(-r.x*k,  r.y*k)));\n"
    "  d = max(d, m(vUV + vec2( r.x*k, -r.y*k)));\n"
    "  d = max(d, m(vUV + vec2(-r.x*k, -r.y*k)));\n"
    "  float ring = d * (1.0 - c);\n"
    "  if (ring < 0.004) discard;\n"
    "  float a = ring * uStrength;\n"
    "  o = vec4(uColor * a, a);\n"          // premultiplied
    "}\n";

static const char* kPtVS =
    "#version 330\n"
    "layout(location=0) in vec2  aPos;\n"
    "layout(location=1) in float aSize;\n"
    "layout(location=2) in float aAlpha;\n"
    "out float vA;\n"
    "out float vS;\n"
    "void main(){ gl_Position = vec4(aPos, 0.0, 1.0); gl_PointSize = aSize; vA = aAlpha; vS = aSize; }\n";

static const char* kPtFS =
    "#version 330\n"
    "in float vA;\n"
    "in float vS;\n"
    "uniform vec3 uColor;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  vec2 d = gl_PointCoord * 2.0 - 1.0;\n"
    "  float r = length(d);\n"
    "  float feather = 2.0 / max(vS, 2.0);\n"      // 边缘 1 物理像素过渡
    "  float a = 1.0 - smoothstep(1.0 - feather, 1.0, r);\n"
    "  a *= vA;\n"
    "  if (a <= 0.002) discard;\n"                 // 圆外丢掉
    "  o = vec4(uColor * a, a);\n"
    "}\n";

// ── FXAA（3.11 简化版：亮度阈值 + 边缘方向混合）──────────────────────
// 十八：SSAA 2x 把 140 帧压到 82，而它治不了这个模型的软边（贴图里的 alpha 渐变，
// 见 17.6），性价比太低。FXAA 在 1x 上做，一次全屏 pass、最多 13 次纹理采样。
//
// 三个和「透明图层」有关的取舍：
//  ① 整条链 alpha 都是 premultiplied，所以 rgb 本身已经被 alpha 缩过 —— luma 天然是
//     「覆盖率×颜色亮度」，边缘两侧差得明显；四通道一起做线性插值也是对的（不会发黑）。
//  ② 阈值用**相对**的 max(0.0312, lMax*0.125)：只混真边缘，平坦区原样输出（她脸上一大
//     片肤色不会被磨平）。
//  ③ 混合核固定 3+4 抽样、dir 夹到 ±8 像素 —— 再大就会把细发丝抹成糊块，这个模型的
//     发丝只有 1–2 px 宽。
static const char* kFxaaFS =
    "#version 330\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "out vec4 o;\n"
    "float luma(vec3 c){ return dot(c, vec3(0.299, 0.587, 0.114)); }\n"
    "vec4 tap(vec2 d){ return texture(uTex, vUV + d * uTexel); }\n"
    "void main(){\n"
    "  vec4 m = texture(uTex, vUV);\n"
    "  float lM = luma(m.rgb);\n"
    "  float lN = luma(tap(vec2( 0.0, -1.0)).rgb);\n"
    "  float lS = luma(tap(vec2( 0.0,  1.0)).rgb);\n"
    "  float lE = luma(tap(vec2( 1.0,  0.0)).rgb);\n"
    "  float lW = luma(tap(vec2(-1.0,  0.0)).rgb);\n"
    "  float lMin = min(lM, min(min(lN, lS), min(lE, lW)));\n"
    "  float lMax = max(lM, max(max(lN, lS), max(lE, lW)));\n"
    "  if (lMax - lMin < max(0.0312, lMax * 0.125)) { o = m; return; }\n"   // 平坦区：原样
    "  float lNW = luma(tap(vec2(-1.0, -1.0)).rgb);\n"
    "  float lNE = luma(tap(vec2( 1.0, -1.0)).rgb);\n"
    "  float lSW = luma(tap(vec2(-1.0,  1.0)).rgb);\n"
    "  float lSE = luma(tap(vec2( 1.0,  1.0)).rgb);\n"
    "  vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)),\n"
    "                   ((lNW + lSW) - (lNE + lSE)));\n"
    "  float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);\n"
    "  float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);\n"
    "  dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * uTexel;\n"
    "  vec4 a = 0.5 * (texture(uTex, vUV + dir * (1.0 / 3.0 - 0.5)) +\n"
    "                  texture(uTex, vUV + dir * (2.0 / 3.0 - 0.5)));\n"
    "  vec4 b = a * 0.5 + 0.25 * (texture(uTex, vUV - dir * 0.5) +\n"
    "                             texture(uTex, vUV + dir * 0.5));\n"
    "  float lB = luma(b.rgb);\n"
    "  o = (lB < lMin || lB > lMax) ? a : b;\n"    // 混过头就退回短核
    "}\n";

// ══════════════════════════════════════════════════════════════════════
// 十九、SMAA 1x（可选档 aa=smaa）
//
// 三遍 pass，全部在**和 targetTex 同尺寸**（1x）上做，位置和 FXAA 一模一样：
//   ① edges   : 亮度边缘检测 → RG8（r = 西边有边，g = 北边有边）。平坦区 discard。
//   ② weights : 查 AreaTex（按左右/上下距离 + 交叉边取预计算的覆盖面积）
//               和 SearchTex（同行/同列还要沿着线走几步）→ RGBA8
//                （r/g = 左右两个混合权重，b/a = 上下两个）。
//   ③ neigh   : 拿 weights 对**原始 FBO 颜色**做一次混合 —— 只沿边缘混，
//                所以不像 FXAA 那样把整条边带糊成 3–4 px。
//
// 和这个工程的三条约定（踩过才写）：
//   · 输入是 targetTex()（模型 + 垫在下面的影子/粒子/轮廓光都在里面），输出另起
//     一个同尺寸纹理交给 composite —— 和 applyFxaa 同一条缝，合成那一趟不用改。
//   · 颜色是 premultiplied：邻域混合直接对 RGBA 一起 lerp。**不做 gamma 展平**
//     （参考实现里有 pow(2.2) 那一步，但那是给「非预乘、线性工作流」准备的；
//     预乘色 pow 一遍会把「颜色×alpha」拆开，边缘会偏色）。
//   · 查找表读不到 / shader 编不过 → smaaOk = false：颜色链一个字都不变，
//     aa 当 ssaa2x（main 里一次性回落，见 setSmaaOk / aaFallback）。
//
// 纹理朝向：参考实现是 y 向下的 texcoord，我们是 y 向上（FBO 的 v=0 在 NDC -1）。
// 按 three.js 的 WebGL 移植版（同一份算法，注释里标了 "Changed sign"）抄 ——
// 三遍 pass 之间的 splat 是各自独立的，朝向只影响 weights 那遍里 ±y 的几个偏移。
// ══════════════════════════════════════════════════════════════════════

// ① 边缘检测（luma 版，SMAA_THRESHOLD 0.1）
static const char* kSmaaEdgeFS =
    "#version 330\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "uniform vec2 uTexel;\n"
    "out vec4 o;\n"
    "const vec3 kLuma = vec3(0.2126, 0.7152, 0.0722);\n"
    "float lum(vec2 d){ return dot(texture(uTex, vUV + d * uTexel).rgb, kLuma); }\n"
    "void main(){\n"
    "  float L  = lum(vec2( 0.0,  0.0));\n"
    "  float Ll = lum(vec2(-1.0,  0.0));\n"
    "  float Lt = lum(vec2( 0.0, -1.0));\n"
    "  vec2 delta = vec2(abs(L - Ll), abs(L - Lt));\n"
    "  vec2 edges = step(vec2(0.1), delta);\n"
    "  if (edges.x + edges.y == 0.0) discard;\n"
    "  float Le  = lum(vec2( 1.0,  0.0));\n"
    "  float Lb  = lum(vec2( 0.0,  1.0));\n"
    "  float Lll = lum(vec2(-2.0,  0.0));\n"
    "  float Ltt = lum(vec2( 0.0, -2.0));\n"
    "  vec2 md = max(vec2(abs(L - Le), abs(L - Lb)),\n"
    "                vec2(abs(Ll - Lll), abs(Lt - Ltt)));\n"
    "  float finalDelta = max(md.x, md.y);\n"
    "  edges *= step(0.5 * finalDelta, delta);\n"   // 局部对比度自适应：别被更强的边带着乱混
    "  o = vec4(edges, 0.0, 1.0);\n"
    "}\n";

// ② 混合权重：SearchTex 决定「这条线往两边各有多长」，AreaTex 把
//    （左距离、右距离、两侧交叉边）映射成两侧各自的覆盖面积。
static const char* kSmaaBlendFS =
    "#version 330\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uEdges, uArea, uSearch;\n"
    "uniform vec2 uTexel;\n"          // 1/edges 尺寸（三张目标同尺寸）
    "uniform vec2 uSize;\n"           // edges 像素尺寸
    "out vec4 o;\n"
    "const vec2  AREA_PIXEL  = vec2(1.0/160.0, 1.0/560.0);\n"
    "const float AREA_SUBTEX = 1.0/7.0;\n"
    "const float MAXD        = 16.0;\n"
    "const int   STEPS       = 8;\n"
    "float searchLen(vec2 e, float bias){\n"   // SearchTex 是 64x16 的打包表：x=边组合，y=偏移
    "  return 255.0 * texture(uSearch, vec2(bias + e.r * 0.5, e.g)).r;\n"
    "}\n"
    // 沿线往左走（一次跨两像素，-0.25/-0.125 的偏移是 @PSEUDO_GATHER4：一次采样拿四个边）
    "float searchXLeft(vec2 tc, float end){\n"
    "  vec2 e = vec2(0.0, 1.0);\n"
    "  for (int i = 0; i < STEPS; ++i){\n"
    "    e = texture(uEdges, tc).rg;\n"
    "    tc -= vec2(2.0, 0.0) * uTexel;\n"
    "    if (!(tc.x > end && e.g > 0.8281 && e.r == 0.0)) break;\n"
    "  }\n"
    "  tc.x += 0.25 * uTexel.x;\n"
    "  tc.x += uTexel.x;\n"
    "  tc.x += 2.0 * uTexel.x;\n"
    "  tc.x -= uTexel.x * searchLen(e, 0.0);\n"
    "  return tc.x;\n"
    "}\n"
    "float searchXRight(vec2 tc, float end){\n"
    "  vec2 e = vec2(0.0, 1.0);\n"
    "  for (int i = 0; i < STEPS; ++i){\n"
    "    e = texture(uEdges, tc).rg;\n"
    "    tc += vec2(2.0, 0.0) * uTexel;\n"
    "    if (!(tc.x < end && e.g > 0.8281 && e.r == 0.0)) break;\n"
    "  }\n"
    "  tc.x -= 0.25 * uTexel.x;\n"
    "  tc.x -= uTexel.x;\n"
    "  tc.x -= 2.0 * uTexel.x;\n"
    "  tc.x += uTexel.x * searchLen(e, 0.5);\n"
    "  return tc.x;\n"
    "}\n"
    "float searchYUp(vec2 tc, float end){\n"
    "  vec2 e = vec2(1.0, 0.0);\n"
    "  for (int i = 0; i < STEPS; ++i){\n"
    "    e = texture(uEdges, tc).rg;\n"
    "    tc += vec2(0.0, 2.0) * uTexel;\n"
    "    if (!(tc.y > end && e.r > 0.8281 && e.g == 0.0)) break;\n"
    "  }\n"
    "  tc.y -= 0.25 * uTexel.y;\n"
    "  tc.y -= uTexel.y;\n"
    "  tc.y -= 2.0 * uTexel.y;\n"
    "  tc.y += uTexel.y * searchLen(e.gr, 0.0);\n"
    "  return tc.y;\n"
    "}\n"
    "float searchYDown(vec2 tc, float end){\n"
    "  vec2 e = vec2(1.0, 0.0);\n"
    "  for (int i = 0; i < STEPS; ++i){\n"
    "    e = texture(uEdges, tc).rg;\n"
    "    tc -= vec2(0.0, 2.0) * uTexel;\n"
    "    if (!(tc.y < end && e.r > 0.8281 && e.g == 0.0)) break;\n"
    "  }\n"
    "  tc.y += 0.25 * uTexel.y;\n"
    "  tc.y += uTexel.y;\n"
    "  tc.y += 2.0 * uTexel.y;\n"
    "  tc.y -= uTexel.y * searchLen(e.gr, 0.5);\n"
    "  return tc.y;\n"
    "}\n"
    "vec2 area(vec2 dist, float e1, float e2, float off){\n"
    "  vec2 tc = MAXD * round(4.0 * vec2(e1, e2)) + dist;\n"
    "  tc = AREA_PIXEL * tc + 0.5 * AREA_PIXEL;\n"
    "  tc.y += AREA_SUBTEX * off;\n"
    "  return texture(uArea, tc).rg;\n"
    "}\n"
    "void main(){\n"
    "  vec2 pixcoord = vUV / uTexel;\n"
    "  vec4 o0 = vUV.xyxy + uTexel.xyxy * vec4(-0.25,  0.125, 1.25,  0.125);\n"
    "  vec4 o1 = vUV.xyxy + uTexel.xyxy * vec4(-0.125, 0.25, -0.125, -1.25);\n"
    "  vec4 o2 = vec4(o0.xz, o1.yw) + vec4(-2.0, 2.0, -2.0, 2.0) * uTexel.xxyy * float(STEPS);\n"
    "  vec2 e = texture(uEdges, vUV).rg;\n"
    "  vec4 w = vec4(0.0);\n"
    "  if (e.g > 0.0){\n"                       // 横向的线：沿 x 找两端
    "    vec2 coords;\n"
    "    coords.x = searchXLeft(o0.xy, o2.x);\n"
    "    coords.y = o1.y;\n"
    "    float dl = coords.x;\n"
    "    float e1 = texture(uEdges, coords).r;\n"
    "    coords.x = searchXRight(o0.zw, o2.y);\n"
    "    float dr = coords.x;\n"
    "    vec2 d = vec2(dl, dr) / uTexel.x - pixcoord.x;\n"
    "    vec2 sd = sqrt(abs(d));\n"
    "    coords.y -= uTexel.y;\n"
    "    float e2 = textureLodOffset(uEdges, coords, 0.0, ivec2(1, 0)).r;\n"
    "    w.rg = area(sd, e1, e2, 0.0);\n"
    "  }\n"
    "  if (e.r > 0.0){\n"                       // 竖向的线：沿 y 找两端
    "    vec2 coords;\n"
    "    coords.y = searchYUp(o1.xy, o2.z);\n"
    "    coords.x = o0.x;\n"
    "    float dt = coords.y;\n"
    "    float e1 = texture(uEdges, coords).g;\n"
    "    coords.y = searchYDown(o1.zw, o2.w);\n"
    "    float db = coords.y;\n"
    "    vec2 d = vec2(dt, db) / uTexel.y - pixcoord.y;\n"
    "    vec2 sd = sqrt(abs(d));\n"
    "    coords.y -= uTexel.y;\n"
    "    float e2 = textureLodOffset(uEdges, coords, 0.0, ivec2(0, 1)).g;\n"
    "    w.ba = area(sd, e1, e2, 0.0);\n"
    "  }\n"
    "  o = w;\n"
    "}\n";

// ③ 邻域混合：只在真有权重的地方混，而且只往权重更大的那一侧混一像素。
static const char* kSmaaNeighFS =
    "#version 330\n"
    "in vec2 vUV;\n"
    "uniform sampler2D uColor, uBlend;\n"
    "uniform vec2 uTexel;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  vec4 a;\n"
    "  a.xz = texture(uBlend, vUV).xz;\n"                                    // 本像素：左 / 右
    "  a.y  = texture(uBlend, vUV + vec2(0.0, -1.0) * uTexel).g;\n"         // 上一行给的「上」
    "  a.w  = texture(uBlend, vUV + vec2(1.0,  0.0) * uTexel).a;\n"         // 右一列给的「下」
    "  if (dot(a, vec4(1.0)) < 1e-5){ o = texture(uColor, vUV); return; }\n"
    "  vec2 dir;\n"
    "  dir.x = a.a > a.b ? a.a : -a.b;\n"
    "  dir.y = a.g > a.r ? -a.g : a.r;\n"
    "  if (abs(dir.x) > abs(dir.y)) dir.y = 0.0; else dir.x = 0.0;\n"
    "  vec4 c0 = texture(uColor, vUV);\n"
    "  vec4 c1 = texture(uColor, vUV + sign(dir) * uTexel);\n"
    "  o = mix(c0, c1, max(abs(dir.x), abs(dir.y)));\n"
    "}\n";

// 两张官方查找表的 DDS 只有「128 字节头 + 原始像素」这一种最朴素的写法：
// 没有 fourcc、没有压缩、没有 mipmap，所以不需要任何图像库。
// 头里要的四个数：高(12)、宽(16)、像素位数(88)。24bpp 是 BGR 排的，手动换成 RGB。
static bool readDds(const char* path, int* w, int* h, int* bpp,
                    std::vector<unsigned char>& px) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    unsigned char hd[128];
    f.read(reinterpret_cast<char*>(hd), sizeof hd);
    if (f.gcount() != static_cast<std::streamsize>(sizeof hd)) return false;
    if (std::memcmp(hd, "DDS ", 4) != 0) return false;
    const auto u32 = [&hd](int off) {
        return int(hd[off]) | (int(hd[off + 1]) << 8) | (int(hd[off + 2]) << 16) |
               (int(hd[off + 3]) << 24);
    };
    *h   = u32(12);
    *w   = u32(16);
    *bpp = u32(88) / 8;
    if (*w <= 0 || *h <= 0 || (*bpp != 1 && *bpp != 3)) return false;
    const size_t want = static_cast<size_t>(*w) * static_cast<size_t>(*h) *
                        static_cast<size_t>(*bpp);
    px.resize(want);
    f.read(reinterpret_cast<char*>(px.data()), static_cast<std::streamsize>(want));
    if (f.gcount() != static_cast<std::streamsize>(want)) return false;
    if (*bpp == 3) {
        for (size_t i = 0; i + 2 < px.size(); i += 3) std::swap(px[i], px[i + 2]);
    }
    return true;
}

static bool loadSmaaTextures() {
    int aw = 0, ah = 0, ab = 0, sw = 0, sh = 0, sb = 0;
    std::vector<unsigned char> ap, sp;
    const bool gotArea = readDds("assets/smaa/AreaTexDX10.dds", &aw, &ah, &ab, ap);
    const bool gotSearch = readDds("assets/smaa/SearchTex.dds", &sw, &sh, &sb, sp);
    if (!gotArea || !gotSearch || aw != 160 || ah != 560 || ab != 3 ||
        sw != 64 || sh != 16 || sb != 1) {
        log_("[post] smaa 不可用：assets/smaa/ 下没有（或不是）AreaTexDX10.dds 160×560 RGB888"
             " + SearchTex.dds 64×16 灰度（读到 area=%d %dx%d %dbpp / search=%d %dx%d %dbpp）",
             int(gotArea), aw, ah, ab * 8, int(gotSearch), sw, sh, sb * 8);
        return false;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &C.smaaAreaTex);
    glBindTexture(GL_TEXTURE_2D, C.smaaAreaTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, aw, ah, 0, GL_RGB, GL_UNSIGNED_BYTE, ap.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &C.smaaSearchTex);
    glBindTexture(GL_TEXTURE_2D, C.smaaSearchTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, sw, sh, 0, GL_RED, GL_UNSIGNED_BYTE, sp.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    log_("[post] smaa 查找表：Area %dx%d RGB8（%zu 字节）/ Search %dx%d R8（%zu 字节），"
         "GL_LINEAR 不建 mipmap", aw, ah, ap.size(), sw, sh, sp.size());
    return true;
}

// 三张同尺寸目标（edges RG8 / blend RGBA8 / out RGBA8）。改尺寸时整组重建。
static bool ensureSmaaTargets(int w, int h) {
    if (C.smaaOutFbo != 0 && C.smaaW == w && C.smaaH == h) return true;
    if (C.smaaEdgesFbo) { glDeleteFramebuffers(1, &C.smaaEdgesFbo); C.smaaEdgesFbo = 0; }
    if (C.smaaEdgesTex) { glDeleteTextures(1, &C.smaaEdgesTex); C.smaaEdgesTex = 0; }
    if (C.smaaBlendFbo) { glDeleteFramebuffers(1, &C.smaaBlendFbo); C.smaaBlendFbo = 0; }
    if (C.smaaBlendTex) { glDeleteTextures(1, &C.smaaBlendTex); C.smaaBlendTex = 0; }
    if (C.smaaOutFbo) { glDeleteFramebuffers(1, &C.smaaOutFbo); C.smaaOutFbo = 0; }
    if (C.smaaOutTex) { glDeleteTextures(1, &C.smaaOutTex); C.smaaOutTex = 0; }
    C.smaaW = C.smaaH = 0;

    const auto make = [w, h](GLuint* tex, GLuint* fbo, GLint internal, GLenum fmt,
                             const char* tag) -> bool {
        glGenTextures(1, tex);
        glBindTexture(GL_TEXTURE_2D, *tex);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (st != GL_FRAMEBUFFER_COMPLETE) {
            log_("[post] smaa 的 %s 目标不完整（0x%04x），这一帧不走 smaa",
                 tag, static_cast<unsigned>(st));
            glDeleteFramebuffers(1, fbo); *fbo = 0;
            glDeleteTextures(1, tex);     *tex = 0;
            return false;
        }
        return true;
    };
    if (!make(&C.smaaEdgesTex, &C.smaaEdgesFbo, GL_RG8, GL_RG, "edges") ||
        !make(&C.smaaBlendTex, &C.smaaBlendFbo, GL_RGBA8, GL_RGBA, "blend") ||
        !make(&C.smaaOutTex, &C.smaaOutFbo, GL_RGBA8, GL_RGBA, "out")) {
        return false;
    }
    C.smaaW = w;
    C.smaaH = h;
    log_("[post] smaa 目标重建 %dx%d（RG8 + 2×RGBA8，共 %.1f MB）", w, h,
         double(w) * double(h) * 8.0 / 1048576.0);
    return true;
}

// 返回 true 表示「成品已经在 smaaOutTex 里，合成要改吃它」。
static bool applySmaa(int w, int h) {
    if (!C.smaaOk || C.progSmaaEdge == 0 || C.progSmaaBlend == 0 || C.progSmaaNeigh == 0)
        return false;
    if (!ensureSmaaTargets(w, h)) return false;
    const float tx = 1.0f / float(w), ty = 1.0f / float(h);
    glDisable(GL_BLEND);
    glBindVertexArray(C.vaoQuad);

    // ① edges
    glBindFramebuffer(GL_FRAMEBUFFER, C.smaaEdgesFbo);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(C.progSmaaEdge);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, C.tex);
    glUniform1i(C.uSeTex, 0);
    glUniform2f(C.uSeTexel, tx, ty);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ② blend weights
    glBindFramebuffer(GL_FRAMEBUFFER, C.smaaBlendFbo);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(C.progSmaaBlend);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, C.smaaEdgesTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, C.smaaAreaTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, C.smaaSearchTex);
    glUniform1i(C.uSbEdges, 0);
    glUniform1i(C.uSbArea, 1);
    glUniform1i(C.uSbSearch, 2);
    glUniform2f(C.uSbTexel, tx, ty);
    glUniform2f(C.uSbSize, float(w), float(h));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // ③ neighborhood blending：颜色读**原始** targetTex，权重读 blendTex
    glBindFramebuffer(GL_FRAMEBUFFER, C.smaaOutFbo);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(C.progSmaaNeigh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, C.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, C.smaaBlendTex);
    glUniform1i(C.uSnColor, 0);
    glUniform1i(C.uSnBlend, 1);
    glUniform2f(C.uSnTexel, tx, ty);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

static bool init() {
    if (C.progRim != 0 || C.shadowTex != 0) return true;
    buildShadowTex();
    C.progRim = mkProgram(kRimVS, kRimFS, "rim");
    C.progFxaa = mkProgram(kRimVS, kFxaaFS, "fxaa");
    C.progPt  = mkProgram(kPtVS, kPtFS, "particles");
    if (C.progFxaa) {
        C.uFxTex   = glGetUniformLocation(C.progFxaa, "uTex");
        C.uFxTexel = glGetUniformLocation(C.progFxaa, "uTexel");
    }
    if (C.progRim) {
        C.uRimTex   = glGetUniformLocation(C.progRim, "uTex");
        C.uRimTexel = glGetUniformLocation(C.progRim, "uTexel");
        C.uRimR     = glGetUniformLocation(C.progRim, "uR");
        C.uRimColor = glGetUniformLocation(C.progRim, "uColor");
        C.uRimStr   = glGetUniformLocation(C.progRim, "uStrength");
    }
    if (C.progPt) C.uPtColor = glGetUniformLocation(C.progPt, "uColor");

    // 十九：SMAA —— 三遍 pass 的 program + 两张查找表。任何一步失败都只是
    // 「没有 smaa 这一档」，不影响别的 aa。
    C.progSmaaEdge  = mkProgram(kRimVS, kSmaaEdgeFS,  "smaa-edges");
    C.progSmaaBlend = mkProgram(kRimVS, kSmaaBlendFS, "smaa-weights");
    C.progSmaaNeigh = mkProgram(kRimVS, kSmaaNeighFS, "smaa-blend");
    const bool texOk = (C.smaaAreaTex != 0 && C.smaaSearchTex != 0) || loadSmaaTextures();
    C.smaaOk = texOk && C.progSmaaEdge && C.progSmaaBlend && C.progSmaaNeigh;
    if (C.smaaOk) {
        C.uSeTex    = glGetUniformLocation(C.progSmaaEdge, "uTex");
        C.uSeTexel  = glGetUniformLocation(C.progSmaaEdge, "uTexel");
        C.uSbEdges  = glGetUniformLocation(C.progSmaaBlend, "uEdges");
        C.uSbArea   = glGetUniformLocation(C.progSmaaBlend, "uArea");
        C.uSbSearch = glGetUniformLocation(C.progSmaaBlend, "uSearch");
        C.uSbTexel  = glGetUniformLocation(C.progSmaaBlend, "uTexel");
        C.uSbSize   = glGetUniformLocation(C.progSmaaBlend, "uSize");
        C.uSnColor  = glGetUniformLocation(C.progSmaaNeigh, "uColor");
        C.uSnBlend  = glGetUniformLocation(C.progSmaaNeigh, "uBlend");
        C.uSnTexel  = glGetUniformLocation(C.progSmaaNeigh, "uTexel");
        log_("[post] smaa 就绪：edges / weights / blend 三遍（SMAA 1x，1x 分辨率）");
    } else if (texOk) {
        log_("[post] smaa 不可用：着色器编译/链接失败，aa 里不留这一档");
    }

    // 全屏四边形（rim 用）—— 直接 NDC，不走矩阵
    {
        static const float q[8] = {-1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
        glGenVertexArrays(1, &C.vaoQuad);
        glGenBuffers(1, &C.vboQuad);
        glBindVertexArray(C.vaoQuad);
        glBindBuffer(GL_ARRAY_BUFFER, C.vboQuad);
        glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    // 粒子：一次一条 VBO，stride 5 float（x,y,size,alpha,pad）
    {
        glGenVertexArrays(1, &C.vaoPt);
        glGenBuffers(1, &C.vboPt);
        glBindVertexArray(C.vaoPt);
        glBindBuffer(GL_ARRAY_BUFFER, C.vboPt);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<const void*>(0));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<const void*>(2 * sizeof(float)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float),
                              reinterpret_cast<const void*>(3 * sizeof(float)));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    log_("[post] 就绪：阴影贴图 %d²（峰值 alpha %.2f）| rim=%s | particles=%s | fxaa=%s | smaa=%s",
         kShadowN, double(kShadowPeak), C.progRim ? "ok" : "失败", C.progPt ? "ok" : "失败",
         C.progFxaa ? "ok" : "失败", C.smaaOk ? "ok" : "不可用");
    return C.progRim != 0;
}

static GLuint targetFbo() { return C.fbo; }
static GLuint targetTex() { return C.tex; }
static GLuint fxTex()    { return C.fxTex; }
static GLuint smaaTex()  { return C.smaaOutTex; }

// 十九：smaa 能不能用（查找表 + shader 都在）—— main 拿它决定 aa 落不落回 ssaa2x。
static bool smaaAvailable() { return C.smaaOk; }

// 每帧一次：告诉这层「这次画多大、逻辑多大」。
static void setMetrics(int ww, int wh, int lw, int lh) {
    C.ww = ww > 0 ? ww : 1;
    C.wh = wh > 0 ? wh : 1;
    C.lw = lw > 0 ? lw : 1;
    C.lh = lh > 0 ? lh : 1;
    C.pxl = float(C.ww) / float(C.lw);
}

// 本帧的绘制目标尺寸（FBO 或窗口）。必须在绑目标之后、抓 alpha 之前调一次。
static void setDrawSize(int w, int h) {
    C.tw = w > 0 ? w : 1;
    C.th = h > 0 ? h : 1;
}

static void destroyTargets() {
    if (C.fbo) { glDeleteFramebuffers(1, &C.fbo); C.fbo = 0; }
    if (C.tex) { glDeleteTextures(1, &C.tex); C.tex = 0; }
    C.dw = C.dh = 0;
}

static bool ensureTargets(int dw, int dh) {
    if (C.fbo != 0 && C.dw == dw && C.dh == dh) return true;
    destroyTargets();
    glGenTextures(1, &C.tex);
    glBindTexture(GL_TEXTURE_2D, C.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &C.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, C.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, C.tex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        log_("[post] FBO 不完整（0x%04x），这一帧退回直接画窗口", static_cast<unsigned>(st));
        destroyTargets();
        return false;
    }
    C.dw = dw;
    C.dh = dh;
    log_("[post] FBO 重建 %dx%d（窗口 %dx%d × SSAA %dx），显存约 %.1f MB",
         dw, dh, C.ww, C.wh, C.ww > 0 ? dw / C.ww : 1,
         double(dw) * double(dh) * 4.0 / 1048576.0);
    return true;
}

// 每帧画完模型立刻调：把「纯模型」这一张抄成纹理，下一帧轮廓光和输入区都吃它。
static void capturePrev() {
    const int w = C.tw;
    const int h = C.th;
    GLint drawFb = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &drawFb);   // 建 prevFbo 会改绑定，之后要还回去
    if (!C.prevTex || C.pw != w || C.ph != h) {
        if (C.prevTex) { glDeleteTextures(1, &C.prevTex); C.prevTex = 0; }
        if (C.prevFbo) { glDeleteFramebuffers(1, &C.prevFbo); C.prevFbo = 0; }
        glGenTextures(1, &C.prevTex);
        glBindTexture(GL_TEXTURE_2D, C.prevTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &C.prevFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, C.prevFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, C.prevTex, 0);
        C.pw = w;
        C.ph = h;
        C.prevValid = false;
        C.warnedPrev = false;
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(drawFb));
    }
    glBindTexture(GL_TEXTURE_2D, C.prevTex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glBindTexture(GL_TEXTURE_2D, 0);
    C.prevValid = true;
}

// 把 2D 层的状态复位（Cubism 的 program / 纹理绑定还留着）。
static void begin2D() {
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glBindVertexArray(0);
}

// 背景三件统一用「垫到模型下面」的混合：dst 盖 src。
//   out = src*(1-dstA) + dst   → 模型不透明的像素上 src 贡献 0
static void beginUnder() {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE_MINUS_DST_ALPHA, GL_ONE);
}

// ── B. 地面软阴影 ────────────────────────────────────────────────────
// 中心 = 她 alpha bbox 的底边中点，rx = 0.35×bbox宽、ry = 0.18×rx，颜色 #0a0c14。
// 每帧都从 platPersonBox 重新取 → 拖动、待机、缩放之后它自己就跟上了。
static void drawShadow(float strength, int cx, int cy, int hw, int hh) {
    if (C.shadowTex == 0 || strength <= 0.004f) return;
    const float rx = 0.35f * float(2 * hw);
    const float ry = 0.18f * rx;
    if (rx < 1.5f || ry < 0.6f) return;
    const float yc = float(cy + hh);
    const float x0 = nx(float(cx) - rx), x1 = nx(float(cx) + rx);
    const float y0 = ny(yc + ry),         y1 = ny(yc - ry);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, C.shadowTex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    const float a = strength;               // MODULATE 把 RGB 和 A 一起乘 → premultiplied 一致
    glColor4f(a, a, a, a);
    beginUnder();
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(x0, y0);
    glTexCoord2f(1.0f, 0.0f); glVertex2f(x1, y0);
    glTexCoord2f(1.0f, 1.0f); glVertex2f(x1, y1);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(x0, y1);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

// ── D. 粒子 ─────────────────────────────────────────────────────────
static void spawnOne(P& p, int cx, int cy, int hw, int hh) {
    const float top = float(cy) + 0.2f * float(hh);    // 下部 40%：cy+0.2hh .. cy+hh
    const float bot = float(cy + hh);
    p.bx   = rnd(float(cx - hw), float(cx + hw));
    p.y    = rnd(top, bot);
    p.vy   = rnd(8.0f, 20.0f);
    p.ph   = rnd(0.0f, 6.2831853f);
    p.freq = rnd(0.7f, 1.7f);
    p.amp  = rnd(2.0f, 7.0f);
    p.r    = rnd(1.0f, 3.0f);
    p.life = rnd(4.0f, 8.0f);
    p.age  = 0.0f;
}

static void drawParticles(float strength, float dt, int cx, int cy, int hw, int hh) {
    int want = static_cast<int>(std::lround(24.0f * strength));
    if (want < 0) want = 0;
    if (want > 24) want = 24;
    if (want != C.lastCount) {
        const int old = C.lastCount;
        C.lastCount = want;
        C.ps.resize(static_cast<size_t>(want));
        for (int i = (old < 0 ? 0 : old); i < want; ++i) {
            spawnOne(C.ps[static_cast<size_t>(i)], cx, cy, hw, hh);
            C.ps[static_cast<size_t>(i)].age = rnd(0.0f, C.ps[static_cast<size_t>(i)].life);
        }
        log_("[post] 粒子 %d 颗（particles=%.2f，只在 bbox 下部 40%% 生成）", want, double(strength));
    }
    if (C.ps.empty() || C.progPt == 0) return;

    static std::vector<float> v;
    v.clear();
    v.reserve(C.ps.size() * 5);
    for (P& p : C.ps) {
        p.age += dt;
        p.y   -= p.vy * dt;
        if (p.age >= p.life || p.y < float(cy) - 0.2f * float(hh)) {
            spawnOne(p, cx, cy, hw, hh);          // 出屏 / 寿终 → 随机新位置
            continue;
        }
        const float t = p.age / p.life;
        float env = 1.0f;
        if (t < 0.18f) env = t / 0.18f;           // 渐入
        else if (t > 0.72f) env = (1.0f - t) / 0.28f;   // 渐出
        if (env < 0.0f) env = 0.0f;
        const float a = 0.5f * env;               // 峰值 0.5
        const float x = p.bx + p.amp * std::sin(p.ph + p.freq * p.age);
        v.push_back(nx(x));
        v.push_back(ny(p.y));
        v.push_back(2.0f * p.r * C.pxl);          // gl_PointSize 是绘制像素
        v.push_back(a);
        v.push_back(0.0f);
    }
    if (v.empty()) return;

    glUseProgram(C.progPt);
    glUniform3f(C.uPtColor, 0.70f, 0.86f, 1.0f);
    glActiveTexture(GL_TEXTURE0);
    beginUnder();
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(C.vaoPt);
    glBindBuffer(GL_ARRAY_BUFFER, C.vboPt);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(v.size() * sizeof(float)),
                 v.data(), GL_STREAM_DRAW);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(v.size() / 5));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glDisable(GL_PROGRAM_POINT_SIZE);
    glUseProgram(0);
}

// ── C. 轮廓光 ────────────────────────────────────────────────────────
static void drawRim(float strength, float uiScale) {
    if (strength <= 0.004f || C.progRim == 0) return;
    if (!C.prevValid) {
        if (!C.warnedPrev) {
            C.warnedPrev = true;
            log_("[post] 轮廓光等第一帧 alpha（本帧先不画）");
        }
        return;
    }
    const float rDraw = (1.0f + 2.0f * uiScale) * C.pxl;   // 半径：1+2*scale 逻辑 px
    glUseProgram(C.progRim);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, C.prevTex);
    glUniform1i(C.uRimTex, 0);
    glUniform2f(C.uRimTexel, 1.0f / float(C.pw), 1.0f / float(C.ph));
    glUniform1f(C.uRimR, rDraw);
    glUniform3f(C.uRimColor, 0x8f / 255.0f, 0xb8 / 255.0f, 0xff / 255.0f);
    glUniform1f(C.uRimStr, strength);
    beginUnder();
    glBindVertexArray(C.vaoQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glUseProgram(0);
}

// ── FXAA：把本帧 FBO 的成品（模型 + 垫在下面的三件）混一遍，写进同尺寸的 fxTex ──
// 输入输出同尺寸、逐像素一一对应；之后合成那一趟再（可能）降采样回窗口。
// 单独一个 FBO 而不是就地改：GL 不允许同一张纹理同时当读写目标。
static bool ensureFxTargets(int w, int h) {
    if (C.fxFbo != 0 && C.fxw == w && C.fxh == h) return true;
    if (C.fxFbo) { glDeleteFramebuffers(1, &C.fxFbo); C.fxFbo = 0; }
    if (C.fxTex) { glDeleteTextures(1, &C.fxTex); C.fxTex = 0; }
    glGenTextures(1, &C.fxTex);
    glBindTexture(GL_TEXTURE_2D, C.fxTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &C.fxFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, C.fxFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, C.fxTex, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        log_("[post] FXAA 目标不完整（0x%04x），这一帧不走 fxaa", static_cast<unsigned>(st));
        if (C.fxFbo) { glDeleteFramebuffers(1, &C.fxFbo); C.fxFbo = 0; }
        if (C.fxTex) { glDeleteTextures(1, &C.fxTex); C.fxTex = 0; }
        return false;
    }
    C.fxw = w;
    C.fxh = h;
    log_("[post] FXAA 目标重建 %dx%d（%.1f MB）", w, h,
         double(w) * double(h) * 4.0 / 1048576.0);
    return true;
}

// 返回 true 表示「成品已经在 fxTex 里，合成要改吃它」。
static bool applyFxaa(int w, int h) {
    if (C.progFxaa == 0) return false;
    if (!ensureFxTargets(w, h)) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, C.fxFbo);
    glViewport(0, 0, w, h);
    glUseProgram(C.progFxaa);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, C.tex);
    glUniform1i(C.uFxTex, 0);
    glUniform2f(C.uFxTexel, 1.0f / float(w), 1.0f / float(h));
    glBindVertexArray(C.vaoQuad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    return true;
}

// ── 合成：FBO → 窗口 buffer（顺带把 alpha 乘进去）────────────────────
// 全乘（GL_MODULATE）：RGB 和 A 一起缩，premultiplied 一致；缩放就是双线性。
// src：aa=off 不会走到这里；fxaa 时吃 fxTex，SSAA 时吃 tex（降采样那一趟）。
static void composite(float alpha, int w, int h, GLuint src) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glUseProgram(0);
    glDisable(GL_BLEND);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, src);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    const float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    glColor4f(a, a, a, a);
    glBegin(GL_QUADS);
    glTexCoord2f(0.0f, 0.0f); glVertex2f(-1.0f, -1.0f);
    glTexCoord2f(1.0f, 0.0f); glVertex2f( 1.0f, -1.0f);
    glTexCoord2f(1.0f, 1.0f); glVertex2f( 1.0f,  1.0f);
    glTexCoord2f(0.0f, 1.0f); glVertex2f(-1.0f,  1.0f);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

// 输入区：从**纯模型 alpha**（prevTex）里读，不再读窗口 buffer。
// 好处是影子 / 粒子 / 边缘环都不可能把掩码撑大（它们画在模型下面，不写在 prevTex 里）。
// GPU 先 blit 降到窗口物理尺寸（和以前读回来的东西一模一样），再 glReadPixels。
static bool readAlpha(std::vector<unsigned char>& out, int* ow, int* oh) {
    if (!C.prevValid || C.prevFbo == 0) return false;
    const int w = C.ww, h = C.wh;
    if (w <= 0 || h <= 0) return false;
    if (C.alphaFbo == 0 || C.aw != w || C.ah != h) {
        if (C.alphaFbo) { glDeleteFramebuffers(1, &C.alphaFbo); C.alphaFbo = 0; }
        if (C.alphaTex) { glDeleteTextures(1, &C.alphaTex); C.alphaTex = 0; }
        glGenTextures(1, &C.alphaTex);
        glBindTexture(GL_TEXTURE_2D, C.alphaTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &C.alphaFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, C.alphaFbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, C.alphaTex, 0);
        C.aw = w;
        C.ah = h;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, C.prevFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, C.alphaFbo);
    glBlitFramebuffer(0, 0, C.pw, C.ph, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, C.alphaFbo);
    out.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (ow) *ow = w;
    if (oh) *oh = h;
    return true;
}

static void shutdown() {
    destroyTargets();
    if (C.prevFbo) { glDeleteFramebuffers(1, &C.prevFbo); C.prevFbo = 0; }
    if (C.prevTex) { glDeleteTextures(1, &C.prevTex); C.prevTex = 0; }
    if (C.alphaFbo) { glDeleteFramebuffers(1, &C.alphaFbo); C.alphaFbo = 0; }
    if (C.alphaTex) { glDeleteTextures(1, &C.alphaTex); C.alphaTex = 0; }
    if (C.fxFbo) { glDeleteFramebuffers(1, &C.fxFbo); C.fxFbo = 0; }
    if (C.fxTex) { glDeleteTextures(1, &C.fxTex); C.fxTex = 0; }
    if (C.progFxaa) { glDeleteProgram(C.progFxaa); C.progFxaa = 0; }
    // 十九：SMAA 的三张目标 + 两张查找表 + 三个 program
    if (C.smaaEdgesFbo) { glDeleteFramebuffers(1, &C.smaaEdgesFbo); C.smaaEdgesFbo = 0; }
    if (C.smaaEdgesTex) { glDeleteTextures(1, &C.smaaEdgesTex); C.smaaEdgesTex = 0; }
    if (C.smaaBlendFbo) { glDeleteFramebuffers(1, &C.smaaBlendFbo); C.smaaBlendFbo = 0; }
    if (C.smaaBlendTex) { glDeleteTextures(1, &C.smaaBlendTex); C.smaaBlendTex = 0; }
    if (C.smaaOutFbo) { glDeleteFramebuffers(1, &C.smaaOutFbo); C.smaaOutFbo = 0; }
    if (C.smaaOutTex) { glDeleteTextures(1, &C.smaaOutTex); C.smaaOutTex = 0; }
    if (C.smaaAreaTex) { glDeleteTextures(1, &C.smaaAreaTex); C.smaaAreaTex = 0; }
    if (C.smaaSearchTex) { glDeleteTextures(1, &C.smaaSearchTex); C.smaaSearchTex = 0; }
    if (C.progSmaaEdge) { glDeleteProgram(C.progSmaaEdge); C.progSmaaEdge = 0; }
    if (C.progSmaaBlend) { glDeleteProgram(C.progSmaaBlend); C.progSmaaBlend = 0; }
    if (C.progSmaaNeigh) { glDeleteProgram(C.progSmaaNeigh); C.progSmaaNeigh = 0; }
    C.smaaW = C.smaaH = 0;
    if (C.shadowTex) { glDeleteTextures(1, &C.shadowTex); C.shadowTex = 0; }
    if (C.progRim) { glDeleteProgram(C.progRim); C.progRim = 0; }
    if (C.progPt) { glDeleteProgram(C.progPt); C.progPt = 0; }
    if (C.vboQuad) { glDeleteBuffers(1, &C.vboQuad); C.vboQuad = 0; }
    if (C.vaoQuad) { glDeleteVertexArrays(1, &C.vaoQuad); C.vaoQuad = 0; }
    if (C.vboPt) { glDeleteBuffers(1, &C.vboPt); C.vboPt = 0; }
    if (C.vaoPt) { glDeleteVertexArrays(1, &C.vaoPt); C.vaoPt = 0; }
}

}  // namespace postfx

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
        const double tLoop0 = platTime();   // 整个循环体的总耗时
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

        // ── 十七/十八/十九：选这一帧画到哪儿（aa）──────────────────
        // off   → 直接画窗口 buffer（最快，边缘就是画师画的软边）
        // fxaa  → 画进 1x 的离屏 FBO，一帧末尾跑一次 FXAA，再原尺寸合成回窗口
        // smaa  → 同上位置，跑 edges → weights → blend 三遍（只沿边缘混）
        // 2x/3x → 画进 N 倍 FBO，最后一趟双线性降采样合成回窗口（老 SSAA）
        // FBO 尺寸跟着窗口物理尺寸变（scale / 跨屏 / 换 output 都会变），
        // 重建在帧内同步做完，失败立刻退回直接画。
        postfx::init();

        // 十九：smaa 的可用性只探一次（表在不在、shader 通不通）。
        // 用不了时：日志喊一句、页面下拉里不留这一档、设置里请求的 smaa 拉回 ssaa2x
        // 并**落盘**（否则下次重启还是它，白等一次）。其它 aa 一个字不动。
        {
            static bool probed = false;
            if (!probed) {
                probed = true;
                web::setSmaaOk(postfx::smaaAvailable());
                if (!postfx::smaaAvailable()) web::aaFallback();
            }
        }
        const int aaMul = postfx::factorOf(ws.aa);
        const bool wantFxaa = postfx::fxaaWanted(ws.aa);
        const bool wantSmaa = postfx::smaaWanted(ws.aa) && postfx::smaaAvailable();
        bool offscreen = aaMul > 1 || wantFxaa || wantSmaa;
        int LW = 0, LH = 0;
        platGetLogicalSize(&LW, &LH);
        int DW = W, DH = H;
        postfx::setMetrics(W, H, LW, LH);      // 先告诉这层坐标系，日志/尺寸才准
        if (offscreen) {
            DW = W * aaMul;
            DH = H * aaMul;
            if (!postfx::ensureTargets(DW, DH)) { offscreen = false; DW = W; DH = H; }
        }
        postfx::setDrawSize(DW, DH);           // aa=off 时就是窗口尺寸
        if (offscreen) {
            glBindFramebuffer(GL_FRAMEBUFFER, postfx::targetFbo());
            glViewport(0, 0, DW, DH);
            glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
            glClear(GL_COLOR_BUFFER_BIT);
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
        // 2026-11-10：分段计时（每 5 秒打一行），用来找出每帧真正的开销在哪。
        // 关帧率不重要时，这段能指出 physics / Update / Draw / 后处理 各占多少。
        static double tPhys = 0, tUpd = 0, tDraw = 0, tPost = 0;
        static int nFrames = 0;
        static double lastProf = 0;
        const double tA = platTime();
        if (physics && std::getenv("MIYU_NO_PHYS") == nullptr) physics->Evaluate(model, dt);
        const double tB = platTime();
        model->Update();
        const double tC = platTime();
        renderer->SetMvpMatrix(&proj);
        renderer->DrawModel();
        const double tD = platTime();

        // ── 十七：抄模型 alpha → 垫背景三件 → （十八）FXAA → 合成回窗口 ─────
        // 顺序不能换：capturePrev 必须紧跟在 DrawModel 后面（这时目标里只有她，
        // 抄下来的 alpha 才是干净的）；三件必须在这里画（它们在模型下面，见
        // postfx 顶部注释①）；FXAA 必须在**三件之后**（它要混整张成品，包括影子边和
        // 气泡边）；合成必须在 swap 之前。
        const double tE0 = platTime();
        postfx::capturePrev();
        {
            int pcx = 0, pcy = 0, phw = 0, phh = 0;
            const bool hb = platPersonBox(&pcx, &pcy, &phw, &phh);
            postfx::begin2D();
            if (hb) {
                postfx::drawShadow(ws.shadow, pcx, pcy, phw, phh);
                postfx::drawParticles(ws.particles, dt, pcx, pcy, phw, phh);
            }
            postfx::drawRim(ws.rim, ws.scale);
        }
        if (offscreen) {
            GLuint src = postfx::targetTex();
            // 十八/十九：同一条缝，二选一（wantFxaa 和 wantSmaa 互斥：aa 只有一个值）
            if (wantFxaa && postfx::applyFxaa(DW, DH)) src = postfx::fxTex();
            else if (wantSmaa && postfx::applySmaa(DW, DH)) src = postfx::smaaTex();
            postfx::composite(ws.alpha, W, H, src);
        }
        {
            const double tF = platTime();
            tPhys += tB - tA; tUpd += tC - tB; tDraw += tD - tC; tPost += tF - tE0;
            ++nFrames;
            if (getenv("MIYU_PROF") && now - lastProf > 5.0) {
                lastProf = now;
                const double n = nFrames > 0 ? nFrames : 1;
                log_("[prof] 每帧 物理%.2f 更新%.2f 绘制%.2f 后处理%.2f ms | %d 帧/5s",
                     tPhys/n*1000, tUpd/n*1000, tDraw/n*1000, tPost/n*1000, nFrames);
                tPhys = tUpd = tDraw = tPost = 0; nFrames = 0;
            }
        }

        // ── 整体不透明度（网页设置页的 alpha，第 14 节）───────────────────
        // 「乘到最终颜色上」用一次全屏乘算做完，别去动每个 drawable 的 opacity：
        // 桌面 GL 的 framebuffer 带 alpha，混合是 premultiplied（GL_ONE,
        // ONE_MINUS_SRC_ALPHA），所以 dst = dst * k 同时缩了颜色和 alpha ——
        // 对合成器就是一个整体半透明的浮层，背后桌面照常透出来。
        //   glBlendFunc(GL_ZERO, GL_SRC_COLOR) → dst = 0*src + dst*srcColor = dst*k
        // 固定管线在 3.3 兼容画像里还在（EGL context 就是 COMPATIBILITY，见 platform），
        // 但 Cubism 那边留着 program 和 GL_TEXTURE_2D 绑定，得先拆掉。
        // 只在 k<1 时做：k=1 时这一趟纯属浪费（每帧一次全屏写）。
        // SSAA 那一趟已经在合成里乘过 alpha 了，这里只处理「直接画窗口」的情况。
        if (!offscreen && ws.alpha < 0.999f) {
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
                // 十七：掩码改成从「纯模型 alpha」（prevTex）读，不再读窗口 buffer。
                // 三个理由：① 影子/粒子/边缘环画在模型下面，不会混进掩码把可点区域撑大；
                // ② SSAA 的降采样在 GPU 上做（blit），读回来的还是窗口物理尺寸，
                //    platform 那边除以 buffer scale 的算法一个字都不用改；
                // ③ 阈值仍然是 8（她身上 3% 以上不透明就算可点），和改动前完全一致。
                static std::vector<unsigned char> pix;
                int aw = W, ah = H;
                if (!postfx::readAlpha(pix, &aw, &ah)) {
                    // 第一帧（prevTex 还没抄下来）：退回读窗口 buffer，和改动前一样
                    aw = W;
                    ah = H;
                    pix.resize(static_cast<size_t>(W) * static_cast<size_t>(H) * 4);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pix.data());
                }
                platSetInputRegionFromAlpha(pix.data(), aw, ah, 8);
                if (W != lastRW || H != lastRH || justReleased || justShown)
                    log_("[main] 输入区重算（第 %lu 帧，framebuffer %dx%d%s，拖动期间跳过 %lu 帧）",
                         frameNo, W, H, justReleased ? "，刚松手" : (justShown ? "，刚取消隐藏" : ""),
                         dragFrames);
                if (justReleased) dragFrames = 0;
                lastRW = W;
                lastRH = H;
            }
        }

        {
            const double tG = platTime();
            platSwapBuffers();
            const double tH = platTime();
            platPollEvents();
            const double tI = platTime();
            // swap 和事件处理也计时（niri 下 swap 可能等 frame callback，事件里可能重算输入区）
            static double tSwap = 0, tEv = 0; static int nSw = 0;
            static double lastSw = 0;
            tSwap += tH - tG; tEv += tI - tH; ++nSw;
            if (getenv("MIYU_PROF") && now - lastSw > 5.0) {
                lastSw = now;
                const double n = nSw > 0 ? nSw : 1;
                log_("[prof2] 每帧 swap%.2f 事件%.2f ms | %d 帧/5s", tSwap/n*1000, tEv/n*1000, nSw);
                tSwap = tEv = 0; nSw = 0;
            }
            static double tLoopSum = 0; static int nLoop = 0; static double lastLoop = 0;
            tLoopSum += platTime() - tLoop0; ++nLoop;
            if (getenv("MIYU_PROF") && now - lastLoop > 5.0) {
                lastLoop = now;
                log_("[prof3] 循环体总耗时 %.2f ms/帧 | %d 帧/5s | 折合单核 %.1f%%",
                     tLoopSum/nLoop*1000, nLoop, tLoopSum/5.0*100);
                tLoopSum = 0; nLoop = 0;
            }
        }
        // 第十六节：帧率上限（15/30/60）。睡在这里而不是循环开头 —— 这一帧的活已经干完。
        framePace(ws.fps, platTime());
    }
    web::shutdown();   // 先关设置页的监听，下面才是窗口/GL
    postfx::shutdown();   // 十七：FBO / 纹理 / 着色器（必须在窗口销毁之前，GL 还活着）
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
