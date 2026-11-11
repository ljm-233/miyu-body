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
#include <signal.h>      // kill/SIGTERM：摄像头跟踪子进程的启停
#include <map>
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
#include "ctl.h"
#include "bubble.h"
#include "plan.h"   // 动作计划播放器：外面推一份时间线，她照着演，见第十二节
#include "web.h"    // 本地设置页：127.0.0.1:8200，改完立刻生效，见第十四节
#include "partpoke.h" // 二十一：部位级反应（平台层 ⇄ 渲染层的邮筒 + 参数表）

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

// ── 二十：时间感 —— 她知道自己几点（2026-11-10 01:xx）──────────────────
// 只调**参数**，不改帧率、不改动作库：时段给出两个乘数（待机幅度 / 眨眼频率），
// 在 idleStyle 和网页 idle 之上**再乘一次**（不是覆盖），所以「活泼 + 白天」还是活泼，
// 「活泼 + 深夜」就是打六折的活泼。
//
// 三件必须遵守的事：
//   ① 取 **本地时间**（localtime_r），不是 UTC；机器的时区就是他的时区。
//   ② 每 60 秒重判一次（每帧调 localtime 是浪费，但每分钟一次完全无所谓），
//      跨时段时打一行日志 —— 不然「她怎么突然安静了」查不出来。
//   ③ MIYU_FAKE_HOUR=2 覆盖钟点，只给验证用（真机上没人在 02:00 蹲着看待机幅度）。
struct TimeSeg {
    const char* name;   // 深夜 / 清晨 / 白天 / 傍晚
    const char* span;   // 日志里显示的时段
    float amp;          // 待机幅度倍率
    float blink;        // 眨眼频率倍率
    bool  doze;         // 每 40–90 秒来一次 3 秒的低头
};
static TimeSeg timeSegOf(int hour) {
    if (hour >= 0  && hour < 6)  return TimeSeg{"深夜", "0–5",   0.60f, 0.55f, true};
    if (hour >= 6  && hour < 9)  return TimeSeg{"清晨", "6–9",   1.15f, 1.00f, false};
    if (hour >= 9  && hour < 18) return TimeSeg{"白天", "9–18",  1.00f, 1.00f, false};
    if (hour >= 18 && hour < 23) return TimeSeg{"傍晚", "18–23", 0.90f, 0.90f, false};
    return TimeSeg{"深夜", "23–5", 0.60f, 0.55f, true};   // 23 点往后也算深夜（表里没写的半小时段）
}

static TimeSeg gSeg{"白天", "9–18", 1.0f, 1.0f, false};
static int    gSegHour    = -999;      // 上次判出来的钟点（-999 = 还没判过）
static double gSegCheckT  = -1e9;      // 上次判的时刻（60 秒一次）
static double gDozeNext   = 0.0;       // 下一次低头的时刻（0 = 还没排）

static float timeAmp()   { return web::settings().timeAware ? gSeg.amp   : 1.0f; }
static float timeBlink() { return web::settings().timeAware ? gSeg.blink : 1.0f; }

// 每帧调，但只有到点（60 秒）才真的去问钟
static void timeTick(double now) {
    if (now - gSegCheckT < 60.0) return;
    gSegCheckT = now;
    int h = -1;
    if (const char* fh = std::getenv("MIYU_FAKE_HOUR")) {
        h = std::atoi(fh);                       // 验证用：假钟点，真逻辑
        if (h < 0) h = 0;
        if (h > 23) h = 23;
    } else {
        const std::time_t t = std::time(nullptr);
        struct tm lt{};
        localtime_r(&t, &lt);
        h = lt.tm_hour;
    }
    if (h == gSegHour) return;
    const TimeSeg seg = timeSegOf(h);
    const bool first = (gSegHour == -999);
    gSegHour = h;
    gSeg = seg;
    gDozeNext = 0.0;                             // 换时段：低头重新排期
    char who[48];
    if (std::getenv("MIYU_FAKE_HOUR")) std::snprintf(who, sizeof who, "假钟 %d 点", h);
    else                               std::snprintf(who, sizeof who, "本地 %d 点", h);
    log_("[time] %s时段：%s（%s，%s）待机 ×%.2f 眨眼 ×%.2f%s | timeAware=%s",
         first ? "首次判定 → " : "跨时段 → ", seg.name, who, seg.span,
         seg.amp, seg.blink, seg.doze ? " + 每 40–90 秒一次低头（3.0s）" : "",
         web::settings().timeAware ? "开" : "关（乘数不生效）");
}

// ── 二十之二：拖动 → 让**各个部位**甩起来（喂 physics，不是整体歪）────────
// 他最后那句话是准的：「是身体各个部位，不是整体」。整张图转（投影矩阵）看着就是
// 一张画在转，不是她。**部位级摆动只有一条路：把 AngleX/AngleZ/PositionY 喂给 physics。**
//
// 原理（16.2 那条反过来用）：
//   · AngleX/AngleY/AngleZ/PositionX/Y/Z 自己**不驱动任何 drawable**
//   · 但它们是 physics3.json 的**输入**，输出是头发、裙摆、尾巴、配饰那几十根摇摆参数
//   · 所以「这些参数一直是常数 → 物理没有输入变化 → 头发裙摆纹丝不动 → 看起来像
//     一张图在平移」，这正是他觉得呆的原因。喂进去，physics 每帧 Evaluate 就会把各
//     部位甩起来，而且每根的刚度/相位不同 → 天然的「各部位，不是整体」
//
// 数值按他给的（v 是**弹簧速度** px/s，不是鼠标速度）：
//   AngleX    = clamp(-vx * 0.035, ±16)   ← 往右拖取反 = 往后仰
//   AngleZ    = clamp( vx * 0.02,  ±10)
//   PositionY = clamp( vy * 0.02,  ±12)
// 有速度就每帧 Set；弹簧停稳后以 0.15s 时间常数衰减回 0。
struct SwayRig {
    float ax = 0, az = 0, py = 0;
    float vxKeep = 0, vyKeep = 0;
};
static SwayRig gSway;
static constexpr float  SWAY_TAU_S = 0.15f;   // 停稳后回 0 的时间常数

// 每帧一次：算目标 → Set 进模型。必须在 idleTick/lookTick 之后（它们每帧先写绝对值），
// 在 planTick 之前（计划仍是这一帧最后的写入者）。
static void swayTick(Csm::CubismModel* model, float dt, double now) {
    static Csm::csmInt32 iAz = -2, iPy = -2;
    if (iAz == -2) {
        Csm::CubismIdManager* m = Csm::CubismFramework::GetIdManager();
        iAz = model->GetParameterIndex(m->GetId("AngleZ"));
        iPy = model->GetParameterIndex(m->GetId("PositionY"));
    }
    const web::Settings& ws = web::settings();
    const float gain = ws.dragSway;

    float vx = 0.0f, vy = 0.0f;
    // 注意是 platDragVelocity（弹簧速度），**不要求正在拖** ——
    // 松手滑行那一段物理还要接着甩，那才是「惯性看得出来」的地方
    const bool vActive = platDragVelocity(&vx, &vy);

    float tax = 0.0f, taz = 0.0f, tpy = 0.0f;
    if (vActive) {
        tax = -vx * 0.035f; if (tax >  16.0f) tax =  16.0f; if (tax < -16.0f) tax = -16.0f;
        taz =  vx * 0.02f;  if (taz >  10.0f) taz =  10.0f; if (taz < -10.0f) taz = -10.0f;
        tpy =  vy * 0.02f;  if (tpy >  12.0f) tpy =  12.0f; if (tpy < -12.0f) tpy = -12.0f;
        tax *= gain; taz *= gain; tpy *= gain;
        gSway.vxKeep = vx; gSway.vyKeep = vy;
        // 一点点平滑（0.05s）：弹簧速度本身是连续的，但撞边会在两帧之间反号，
        // 直接 Set 会让头发「咔」一下。0.05s 只压尖峰，不影响跟手。
        const float a = (dt / 0.05f > 1.0f) ? 1.0f : dt / 0.05f;
        gSway.ax += (tax - gSway.ax) * a;
        gSway.az += (taz - gSway.az) * a;
        gSway.py += (tpy - gSway.py) * a;
    } else {
        // 停稳了：0.15s 时间常数回 0（物理会自己晃回去，不用我们给反向脉冲）
        const float a = (dt / SWAY_TAU_S > 1.0f) ? 1.0f : dt / SWAY_TAU_S;
        gSway.ax += (0.0f - gSway.ax) * a;
        gSway.az += (0.0f - gSway.az) * a;
        gSway.py += (0.0f - gSway.py) * a;
        if (std::fabs(gSway.ax) < 0.02f) gSway.ax = 0.0f;
        if (std::fabs(gSway.az) < 0.02f) gSway.az = 0.0f;
        if (std::fabs(gSway.py) < 0.02f) gSway.py = 0.0f;
    }

    // 静止时的极慢轻摆（swayIdle，只在 lively 下）：同样喂 AngleX/AngleZ 让部位晃，
    // 而不是整张图歪。15 秒一个来回。
    float eax = 0.0f, eaz = 0.0f;
    if (ws.swayIdle && !vActive && ws.idleStyle == "lively") {
        eax = 1.2f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 / 15.0 + 1.7));
        eaz = 0.6f * std::sin(static_cast<float>(now * 2.0 * 3.14159265 / 15.0 + 0.4));
    }

    const float ax = gSway.ax + eax, az = gSway.az + eaz, py = gSway.py;
    if (gIdle.iAx >= 0) model->SetParameterValue(gIdle.iAx, model->GetParameterValue(gIdle.iAx) + ax, 1.0f);
    if (iAz >= 0)       model->SetParameterValue(iAz, model->GetParameterValue(iAz) + az, 1.0f);
    // PositionX 只影响物理（重心），给一点横向的力，裙摆会跟着拖
    if (gIdle.iBz >= 0) model->SetParameterValue(gIdle.iBz, model->GetParameterValue(gIdle.iBz) + ax * 0.25f, 1.0f);
    // PositionY：写绝对值（它不会被 LoadParameters 归零，读回来加会累到参数上限）
    if (iPy >= 0) model->SetParameterValue(iPy, py, 1.0f);

    // 日志：每 0.5 秒一行（他点名要的格式）
    static double lastLog = -1e9;
    if ((vActive || std::fabs(ax) > 0.05f || std::fabs(az) > 0.05f || std::fabs(py) > 0.05f)
        && now - lastLog > 0.5) {
        lastLog = now;
        log_("[sway] vx=%.0f vy=%.0f -> AngleX=%.1f AngleZ=%.1f PositionY=%.1f%s",
             gSway.vxKeep, gSway.vyKeep, ax, az, py, vActive ? "（弹簧在动）" : "（衰减中）");
    }
}

// 投影整体倾斜（swayTilt，默认 0 = 关）。整张图转看着像画在转，所以默认不给。
static float swayTiltDeg(double now, float dt) {
    static float ang = 0, vel = 0;
    const web::Settings& ws = web::settings();
    const float maxDeg = ws.swayTilt;
    if (maxDeg <= 0.001f) { ang = vel = 0; return 0.0f; }
    float vx = 0.0f, vy = 0.0f;
    const bool vActive = platDragVelocity(&vx, &vy);
    if (vActive) {
        float deg = (vx / 500.0f) * 3.0f * (maxDeg / 4.5f);
        if (deg >  maxDeg) deg =  maxDeg;
        if (deg < -maxDeg) deg = -maxDeg;
        const float a = (dt / 0.06f > 1.0f) ? 1.0f : dt / 0.06f;
        ang += (deg - ang) * a;
        vel = (deg - ang) / 0.06f;
    } else {
        const float acc = -18.0f * 18.0f * ang - 2.0f * 0.65f * 18.0f * vel;
        vel += acc * dt;
        ang += vel * dt;
        if (std::fabs(ang) < 0.005f && std::fabs(vel) < 0.02f) { ang = 0; vel = 0; }
    }
    (void)now;
    return ang;
}

// 二十一：探针模式（定义在下面「参数探针」那一节；这几个 tick 要先读到它）
static bool probeMode();

// ── 二十：两段「小动作」（抓起来那一下 / 深夜低头）──────────────────────
// 都是**短脉冲**：一个平滑的 0…1…0 包络，乘到 AngleX / PositionY 的增量上。
// 为什么不用 plans/*.json：计划是「外面的时间线」，会盖掉待机/视线一整段；
// 这两个是叠在待机上的几十毫秒抖动，写死在这儿更省事，也不用新增文件。
// PositionY 只驱动 physics（头发/裙摆），所以视觉上就是「被拎起来晃了一下」——
// 这一条是他要的效果，不是妥协。
struct Pulse { bool on; double t0; double dur; float aAngle; float aPosY; };
static Pulse gGrab{false, 0, 0.42, -6.0f, 14.0f};  // 0.42s：上提 + 后仰（原先 -2.4/6 他看不出变化，翻了一倍多）
static Pulse gDoze{false, 0, 3.00, -1.5f, -7.0f};   // 3.00s：低头（和 点头.json 同一套参数）

// 返回这个脉冲这一帧的 0…1 包络（缓入缓出），跑完自己关掉
static float pulseEnv(Pulse& p, double now) {
    if (!p.on) return 0.0f;
    const double u = (now - p.t0) / p.dur;
    if (u >= 1.0) { p.on = false; return 0.0f; }
    if (u < 0.0)  return 0.0f;
    return 0.5f - 0.5f * std::cos(static_cast<float>(2.0 * 3.14159265358979 * u));
}

static void feelTick(Csm::CubismModel* model, double now) {
    if (probeMode()) return;                 // 二十一：探针要静止的帧色，别来抖
    static Csm::csmInt32 iAx = -2, iPy = -2;
    if (iAx == -2) {
        Csm::CubismIdManager* m = Csm::CubismFramework::GetIdManager();
        iAx = model->GetParameterIndex(m->GetId("AngleX"));
        iPy = model->GetParameterIndex(m->GetId("PositionY"));
    }

    // 起播判定：开始拖的那一瞬（platform 每帧告诉我们的）
    static bool wasDrag = false;
    const bool nowDrag = platIsDragging();
    if (nowDrag && !wasDrag) {
        if (web::settings().grab) {
            gGrab.on = true; gGrab.t0 = now;
            log_("[grab] 抓起反馈起播：%.2fs（AngleX %+.1f° / PositionY %+.1f，"
                 "视觉上是头发裙摆被拎起来晃一下）", gGrab.dur, gGrab.aAngle, gGrab.aPosY);
        } else {
            log_("[grab] 开始拖动（抓起反馈关着，不播）");
        }
    }
    wasDrag = nowDrag;

    // 深夜低头：40–90 秒一次，只在它自己的时段里排
    if (web::settings().timeAware && gSeg.doze) {
        if (!gDoze.on) {
            if (gDozeNext <= 0.0) gDozeNext = now + 40.0 + 50.0 * double(idleRnd());
            if (now >= gDozeNext) {
                gDoze.on = true; gDoze.t0 = now;
                gDozeNext = 0.0;
                log_("[doze] 深夜低头起播：%.1fs 缓入缓出（AngleX %+.1f / PositionY %+.1f）",
                     gDoze.dur, gDoze.aAngle, gDoze.aPosY);
            }
        }
    } else if (gDoze.on) {
        gDoze.on = false;                      // 换时段/关掉 timeAware：立刻收
        log_("[doze] 低头被打断（时段变了或 timeAware 关了）");
    }

    const float eGrab = pulseEnv(gGrab, now);
    const float eDoze = pulseEnv(gDoze, now);
    const float ea = eGrab * gGrab.aAngle + eDoze * gDoze.aAngle;
    const float ep = eGrab * gGrab.aPosY + eDoze * gDoze.aPosY;
    // AngleX：叠在**当前值**上再 Set（待机/视线这一帧已经写过它了）。
    // PositionY：**直接写绝对值**，绝对不要读回来加 —— 它和 AngleX 一样不会被
    // LoadParameters 归零，加了就一帧一帧累积（实测 3 帧就从 0 顶到参数上限 30.00，
    // 她整个人往上飘出去）。待机不碰 PositionY，所以「脉冲值 = 绝对值」是对的。
    static float lastPy = 0.0f;
    if (ea != 0.0f || ep != 0.0f || lastPy != 0.0f) {
        if (iAx >= 0) model->SetParameterValue(iAx, model->GetParameterValue(iAx) + ea, 1.0f);
        if (iPy >= 0) model->SetParameterValue(iPy, ep, 1.0f);
        lastPy = ep;
        // 诊断：每 0.1 秒一行，把「包络走到哪、最终写进去多少」报出来 ——
        // 「抓起那一下到底动了没有」只能靠这几个数，截图看不出 0.4 秒的过程
        static double lastFeel = -1e9;
        if (now - lastFeel > 0.1) {
            lastFeel = now;
            log_("[grab] 进行中：%s包络%.2f/%s包络%.2f → ΔAngleX=%+.2f ΔPositionY=%+.2f"
                 " | 写入后 angleX=%.2f positionY=%.2f",
                 gGrab.on ? "" : "（抓起已完）", eGrab, gDoze.on ? "" : "（低头已完）", eDoze, ea, ep,
                 iAx >= 0 ? model->GetParameterValue(iAx) : -999.0f,
                 iPy >= 0 ? model->GetParameterValue(iPy) : -999.0f);
        }
    }
}


static void idleTick(Csm::CubismModel* model, double now) {
    if (!gIdle.ready) idleInit(model);
    // 二十一：探针模式整段让位 —— 呼吸是 0.4s 帧差的最大来源，不关掉就没法比像素
    if (probeMode()) return;

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
    // 二十：再乘一次**时段**系数（深夜 0.6 / 清晨 1.15 / 白天 1.0 / 傍晚 0.9）。
    // 三层是**相乘**不是覆盖 —— 「活泼 + 深夜」= 打六折的活泼，不是退回「自然」。
    // 倍率本身过一道低通：滑块一拖到底、风格一换、跨时段，都不会定住或甩开，是几帧内收放。
    static float kAmp = 1.0f;
    {
        const float want = style.amp * web::settings().idle * timeAmp();
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
    // 诊断：每 2 秒打一行，**在 Add 之后**读（在之前读是上一帧 Load 重置后的 0，会骗人）。
    // 二十：多打 amp 一项 —— 时段到底有没有生效，就看这个数（深夜应 ≈ 白天的 0.6 倍）。
    static double lastLog = -1e9;
    if (now - lastLog > 2.0) {
        lastLog = now;
        log_("[idle] 算 ax=%.2f amp=%.3f（风格 %.2f × 设置 %.2f × 时段 %.2f %s）"
             " | 写入后模型 ax=%.2f az=%.2f br=%.3f",
             ax, kAmp, style.amp, web::settings().idle, timeAmp(), gSeg.name,
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
    if (probeMode()) return;                 // 二十一：探针不做视线（会写 AngleX/眼珠）
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
    bool in = platPointer(&px, &py) && lw > 0 && lh > 0;
    // 2026-11-10 19:1x：她看不见「第二支指针」——那支是自绘 overlay，不是合成器的真光标，
    // 所以收不到 wl_pointer 事件（Wayland 一个 seat 只有一份指针焦点，README 第 8 节）。
    // 但它每次移动都会把全局逻辑坐标写进 pointer/pos.txt，直接读那个文件就行。
    if (!in) {
        static double lastPtrFile = 0.0;
        if (now - lastPtrFile > 0.033) {
            lastPtrFile = now;
            if (FILE* f = std::fopen("/home/link/.dsh/miyu/pointer/pos.txt", "r")) {
                float gx = 0, gy = 0;
                if (std::fscanf(f, "%f %f", &gx, &gy) == 2) {
                    // 全局逻辑 → 她自己窗口的 local：减去当前 output 原点 + 窗口 margin
                    int ox = 0, oy = 0, ww = 0, wh = 0;
                    platWindowOrigin(&ox, &oy, &ww, &wh);
                    const float lx = gx - static_cast<float>(ox);
                    const float ly = gy - static_cast<float>(oy);
                    if (lx >= -40 && ly >= -40 && lx <= static_cast<float>(ww) + 40 &&
                        ly <= static_cast<float>(wh) + 40) {
                        px = lx; py = ly; in = true;
                    }
                }
                std::fclose(f);
            }
        }
    }
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
static double gExprStart = -1e9;   // 这一条表情什么时候开始播（日志用）
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
        // 二十五：**累积**，不再每次清空。
        // 原来这里 `gExprTouched.clear()` —— 只记住「最后一次切的表情」碰过哪些参数。
        // 于是点胸（脸红 key7）再点腰（圈圈眼 key3）之后回默认，只把 key7 写回去，
        // key3 留在 1.0，她的眼睛就一直卡在圈圈眼上。他截图里那双白圈就是这么来的。
        // 现在改成并集（去重），回默认时把这一段时间碰过的参数全写回默认值。
        const std::string raw(bytes.begin(), bytes.end());
        size_t p = 0;
        while ((p = raw.find("\"Id\"", p)) != std::string::npos) {
            const size_t q = raw.find('"', raw.find(':', p) + 1);
            const size_t r = q == std::string::npos ? std::string::npos : raw.find('"', q + 1);
            if (r != std::string::npos) {
                const std::string id = raw.substr(q + 1, r - q - 1);
                bool dup = false;
                for (const auto& t : gExprTouched) if (t == id) { dup = true; break; }
                if (!dup) gExprTouched.push_back(id);
            }
            p = r == std::string::npos ? raw.size() : r;
        }
        if (gExprTouched.size() > 64) gExprTouched.resize(64);   // 别无限长
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
    gExprStart = platTime();
    gExprUntil = (label == "(默认)") ? -1e9
                                    : platTime() + kExprBaseHold * double(web::settings().exprHold);
}

// ── 二十一：参数探针（`MIYU_PROBE=1`，给 tools/param-probe.py 用）─────────
// 同一件事以前只能用 `MIYU_FORCE_PARAM` 做，但那是环境变量 —— 换一个参数要重启一次
// 进程。509 个参数就是 509 次重建窗口 + 重载模型（每次 3 s ≈ 25 分钟），而且每次
// 窗口位置都可能不一样，截出来的图对不上。所以改成一个**文件协议**：
//
//   外面写 state/probe.txt：
//       gen=12
//       Param36 R大臂z=-30
//   程序撑满 settle 帧后写 state/probe-done.txt（tmp + rename，原子）：
//       gen=12
//   外面看到 done 里的 gen 和自己写的一样，就截图。
//
// 为什么是「帧」不是「毫秒」：物理/眨眼/表情都按帧推，按毫秒在卡帧时会对不上号。
//
// 探针模式下**所有自己会动的东西全关**（待机正弦、视线、眨眼、小动作、粒子）——
// 否则 min/max 两张图的差里有大半是呼吸和粒子，根本分不清是谁在动。
//
// 参数名里有空格和中文（`Param36 R大臂z`），所以按**最后一个 `=`** 切。
struct ProbeRig {
    bool        on = false;
    std::string inPath, donePath, rangePath;
    int         gen = -1;            // 已经吃下的那一代
    int         idx = -1;            // 正在按死的参数索引
    std::string name;
    float       val = 0.0f;
    int         held = 0;            // 这个值撑了多少帧
    int         settle = 16;
    double      lastPoll = -1e9;
    bool        warned = false;
};
static ProbeRig gProbe;
static bool probeMode() { return gProbe.on; }

// 二十二：探针每一代可以把**自己这一帧**导成 PPM（P6）。
// 为什么不继续用 grim 截图：她是一层透明浮层，桌面上排着我的终端，滚动一行字
// 外部截图就跟着变，比「她动没动」还吵；而且截图坐标系和窗口坐标系还差一个
// 顶栏高度。直接从 GL 读回来，像素 = 窗口坐标，一个都差不了。
static int  gDrawW = 0, gDrawH = 0;      // 这一帧的 framebuffer 尺寸
static std::string gPendingDump;         // 这一帧要不要导出来（等合成回窗口再读）
static void dumpFrameRGBA(const std::string& path) {
    if (gDrawW <= 0 || gDrawH <= 0) return;
    std::vector<unsigned char> px(static_cast<size_t>(gDrawW) * gDrawH * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, gDrawW, gDrawH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    // 头一行自带尺寸；后面是 top-down 的 RGBA。**带 alpha** ——
    // 有 alpha 就不用再拍一张 hide 的图来求剪影，外面拿 alpha>8 就是她的形状。
    std::fprintf(f, "MIYUFRAME %d %d\n", gDrawW, gDrawH);
    for (int y = gDrawH - 1; y >= 0; --y)              // GL 首行在底部
        std::fwrite(&px[static_cast<size_t>(y) * gDrawW * 4], 1,
                    static_cast<size_t>(gDrawW) * 4, f);
    std::fclose(f);
}

// 每帧把「上一代那个参数」还回默认值。理由：AngleX / AngleZ / PositionX/Y/Z /
// ParamEyeBallX/Y 这几个本来就不吃 LoadParameters 的归零（NOTES 16.2），
// 不显式还原就会把上一个参数的后果带进下一个参数的截图里。
static void probeResetAll(Csm::CubismModel* model) {
    const Csm::csmInt32 n = model->GetParameterCount();
    for (Csm::csmInt32 i = 0; i < n; ++i)
        model->SetParameterValue(i, model->GetParameterDefaultValue(i), 1.0f);
}

static void probeInit(Csm::CubismModel* model) {
    gProbe.on = std::getenv("MIYU_PROBE") != nullptr;
    if (!gProbe.on) return;
    const char* f = std::getenv("MIYU_PROBE_FILE");
    gProbe.inPath = (f && *f) ? f : "state/probe.txt";
    const char* d = std::getenv("MIYU_PROBE_DONE");
    gProbe.donePath = (d && *d) ? d : "state/probe-done.txt";
    const char* s = std::getenv("MIYU_PROBE_SETTLE");
    gProbe.settle = (s && *s) ? atoi(s) : 16;
    if (gProbe.settle < 1) gProbe.settle = 1;
    gProbe.rangePath = "state/param-range.tsv";

    // 参数范围表：cdi3 里只有 Id 和中文名，min/max/默认值都在 moc3 里。
    // 外面拼「弹一下」的曲线要知道能推多远，所以启动时导一份。
    FILE* r = std::fopen(gProbe.rangePath.c_str(), "wb");
    if (r) {
        std::fprintf(r, "# idx\tname\tmin\tmax\tdefault\n");
        const Csm::csmInt32 n = model->GetParameterCount();
        for (Csm::csmInt32 i = 0; i < n; ++i) {
            const Csm::CubismIdHandle id = model->GetParameterId(static_cast<Csm::csmUint32>(i));
            const char* nm = id ? id->GetString().GetRawString() : "?";
            std::fprintf(r, "%d\t%s\t%.3f\t%.3f\t%.3f\n", static_cast<int>(i), nm,
                         model->GetParameterMinimumValue(static_cast<Csm::csmUint32>(i)),
                         model->GetParameterMaximumValue(static_cast<Csm::csmUint32>(i)),
                         model->GetParameterDefaultValue(static_cast<Csm::csmUint32>(i)));
        }
        std::fclose(r);
        log_("[probe] 参数范围表 -> %s（%d 个参数）", gProbe.rangePath.c_str(),
             static_cast<int>(n));
    } else {
        log_("[probe] 写不了 %s（继续跑，只是外面拿不到范围）", gProbe.rangePath.c_str());
    }
    log_("[probe] 开：%s -> %s | settle=%d 帧 | 待机/视线/眨眼/小动作/粒子全关",
         gProbe.inPath.c_str(), gProbe.donePath.c_str(), gProbe.settle);
}

// 读 probe.txt。格式宽松：只认 `gen=<整数>` 和 `名字=<浮点>` 两种行，其余忽略。
static bool probeRead(int* gen, std::string* name, float* val, bool* haveName) {
    FILE* f = std::fopen(gProbe.inPath.c_str(), "rb");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        const size_t eq = s.rfind('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string k = s.substr(0, eq);
        const std::string v = s.substr(eq + 1);
        if (k == "gen") {
            *gen = atoi(v.c_str());
            ok = true;
        } else {
            *name = k;
            *val = static_cast<float>(atof(v.c_str()));
            *haveName = true;
            ok = true;
        }
    }
    std::fclose(f);
    return ok;
}

// 每帧一次，位置在 **planTick 之后、physics->Evaluate 之前** —— 它是这一帧
// 最后一个写参数的人，谁也盖不过它（探针要的就是「只有它在动」）。
static void probeTick(Csm::CubismModel* model, double now) {
    if (!gProbe.on) return;
    // 4ms 一次握手就够（240 fps 也就 4ms 一帧）；每帧 fopen 太浪费。
    if (now - gProbe.lastPoll < 0.004) {
        if (gProbe.idx >= 0) model->SetParameterValue(gProbe.idx, gProbe.val, 1.0f);
        return;
    }
    gProbe.lastPoll = now;
    int gen = gProbe.gen;
    std::string name;
    float val = 0.0f;
    bool haveName = false;
    if (!probeRead(&gen, &name, &val, &haveName)) {
        if (!gProbe.warned) {
            gProbe.warned = true;
            log_("[probe] 读不到 %s（等她收到第一条指令）", gProbe.inPath.c_str());
        }
        return;
    }
    if (gen != gProbe.gen) {
        gProbe.gen = gen;
        gProbe.held = 0;
        gProbe.warned = false;
        probeResetAll(model);          // 上一代那个参数还回默认
        gProbe.idx = -1;
        gProbe.name = name;
        gProbe.val = val;
        if (haveName) {
            gProbe.idx = model->GetParameterIndex(
                Csm::CubismFramework::GetIdManager()->GetId(name.c_str()));
            log_("[probe] gen=%d 按死 `%s` = %.3f（index=%d，min=%.2f max=%.2f）",
                 gen, name.c_str(), val, static_cast<int>(gProbe.idx),
                 gProbe.idx >= 0 ? model->GetParameterMinimumValue(static_cast<Csm::csmUint32>(gProbe.idx)) : 0.0f,
                 gProbe.idx >= 0 ? model->GetParameterMaximumValue(static_cast<Csm::csmUint32>(gProbe.idx)) : 0.0f);
            if (gProbe.idx < 0) log_("[probe] gen=%d 模型里没有 `%s`（这一代什么都不会变）", gen, name.c_str());
        } else {
            log_("[probe] gen=%d 没有参数行（基准帧）", gen);
        }
    }
    if (gProbe.idx >= 0) model->SetParameterValue(gProbe.idx, gProbe.val, 1.0f);
    if (++gProbe.held >= gProbe.settle) {
        // 撑够了才报 —— 外面看到这行就去截图，图和这个值一定对得上。
        const std::string tmp = gProbe.donePath + ".tmp";
        FILE* d = std::fopen(tmp.c_str(), "wb");
        if (d) {
            std::fprintf(d, "gen=%d\n", gProbe.gen);
            std::fclose(d);
            if (std::rename(tmp.c_str(), gProbe.donePath.c_str()) != 0)
                std::remove(tmp.c_str());
            if (const char* dp = std::getenv("MIYU_DUMP_FRAME")) {
                char pbuf[512];
                std::snprintf(pbuf, sizeof pbuf, "%s/frame-%d.raw", dp, gProbe.gen);
                // 不能在这儿读：这一帧还在离屏 FBO 里（SSAA/SMAA），窗口 buffer 是空的。
                // 记下来，等到 swap 之前、合成回窗口之后再读（见主循环里的 gPendingDump）。
                gPendingDump = pbuf;
            }
        }
    }
}

// ── 二十二：部位格子图（用模型自带的 part 分区，不再猜包围盒）──────────────
//
// 他说的「去学学自带的那些文件」指的是这个：模型里每个 drawable 都挂着一个
// **part**（大肥鱼.cdi3.json 的 Parts：头 / 前发 / 后发 / 胸 / 短裙 / 长裙 /
// 手臂 / 手 / 腿 / 尾巴 / 腰部衣服 …）。part 名字就是作者画的解剖分区 ——
// 比「在她 bbox 里切 7 个矩形」准得多，也不受右边那条大尾巴把 bbox 撑宽的影响。
//
// 做法：每帧（限流）把每个 drawable 的**顶点**按渲染用的同一个投影换算成
// 「窗口归一化坐标」（u = x/窗口宽、v = y/窗口高），光栅化进 40×80 的格子，
// 后画的盖先画的（等于她脸上最上层的那个部位）。平台层点下去只查这张格子。
//
// 顶点在哪：model->Update() 之后 GetDrawableVertexPositions() 就是最终形状
// （physics 已经算进去了），所以放在 Update 之后、DrawModel 旁边跑。
struct PartZoneRig {
    bool ready = false;
    std::vector<unsigned char> lut;      // drawable 索引 → 部位编号
    std::vector<unsigned char> grid;     // 40×80
    std::vector<unsigned char> last;     // 上一张（判断要不要重写文件）
    double lastBuild = -1e9;
    double lastDump = -1e9;
    int    built = 0;
    int    clips = 0;
    std::vector<unsigned char> cellOk;   // 这个格子里有没有她（来自 alpha 读回）
    bool   cellOkReady = false;
    int    lastFillIn = 0;               // 上一帧「剪影内补洞」填了多少格（只给日志看）

};
static PartZoneRig gZones;

// 模型 part 的中文名 → 我们这十四个部位。名字是作者写的，按它分最不会错。
static unsigned char zoneOfPartName(const char* nm) {
    // Step 3（2026-11-10 22:xx）：drawable 的 part 名 → 部位，改成查 state/parts.tsv 的
    // alias 记录。原来这 20 行 if 是**第三份部位名单**（另一份是 partpoke.h 的 kZoneNames、
    // 还有 web.cpp 前端那份），顺序敏感（`眼镜` 必须先于 `眼`，`腰部` 必须先于 `腰`）。
    // 现在顺序由 alias 行的先后决定，加部位不用改这里、也不用重编译。
    // 边界：id 的值域是格子图的 unsigned char（1..255）。Step 7 之后部位数由
    // partapi::count() 决定，没有「槽位」了 —— 表里写多少个部位就有多少个。
    const int id = partapi::indexOfAlias(nm ? std::string(nm) : std::string());
    if (id <= 0 || id > 255) return 0;
    return static_cast<unsigned char>(id);
}

// 读 cdi3.json 的 Parts：把 "Part88" 这种 id 翻成中文名，再翻成部位编号。
static std::map<std::string, std::string> loadPartNames() {
    std::map<std::string, std::string> out;
    const std::string dir(kModelDir);
    const std::string p = dir + "大肥鱼.cdi3.json";
    std::ifstream f(p);
    if (!f) { log_("[zones] 读不到 %s（退回文件里的格子图）", p.c_str()); return out; }
    std::string js((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const size_t at = js.find("\"Parts\"");
    if (at == std::string::npos) return out;
    size_t i = at;
    std::string lastId;
    while (i < js.size()) {
        const size_t idPos = js.find("\"Id\"", i);
        const size_t nmPos = js.find("\"Name\"", i);
        if (nmPos == std::string::npos) break;
        if (idPos != std::string::npos && idPos < nmPos) {
            size_t q = js.find('"', js.find(':', idPos) + 1);
            if (q == std::string::npos) break;
            const size_t e = js.find('"', q + 1);
            if (e == std::string::npos) break;
            lastId = js.substr(q + 1, e - q - 1);
            i = e + 1;
            continue;
        }
        size_t q = js.find('"', js.find(':', nmPos) + 1);
        if (q == std::string::npos) break;
        const size_t e = js.find('"', q + 1);
        if (e == std::string::npos) break;
        if (!lastId.empty()) out[lastId] = js.substr(q + 1, e - q - 1);
        lastId.clear();
        i = e + 1;
        if (js.find("\"Parts\"", i) != std::string::npos &&
            js.find("\"Parts\"", i) < js.find("\"Id\"", i)) break;   // 出了 Parts 段
    }
    return out;
}

// ── 纹理 alpha 表（二十三）：为什么需要它 ─────────────────────────────
// 格子图原来按「drawable 的多边形覆盖」上色，后画的盖先画的。问题是这个模型里
// 有些 mesh 是**一大块方片 + 大部分透明纹理**（比如大腿那几片正好压在裙摆上），
// 于是裙摆那一圈格子被判成 leg —— 点裙子会去踹腿，量「点裙子」也永远输给腿。
// 修法：光栅化时按顶点 UV 去查**纹理的 alpha**，一个三角形里 4 个采样点（重心 +
// 三条边中点）都没料就整块跳过。只影响格子图，不影响渲染。
struct Atlas { int w = 0, h = 0; std::vector<unsigned char> a; };
static std::vector<Atlas> gAtlas;

static void loadAtlases() {
    if (!gAtlas.empty()) return;
    const std::string jp = std::string(kModelDir) + "大肥鱼.model3.json";
    std::ifstream jf(jp);
    if (!jf) { log_("[zones] 读不到 %s（跳过纹理 alpha 过滤）", jp.c_str()); return; }
    std::string js((std::istreambuf_iterator<char>(jf)), std::istreambuf_iterator<char>());
    const size_t at = js.find("\"Textures\"");
    if (at == std::string::npos) return;
    const size_t lb = js.find('[', at), rb = js.find(']', at);
    if (lb == std::string::npos || rb == std::string::npos) return;
    std::vector<std::string> files;
    for (size_t q = lb; q < rb;) {
        const size_t a = js.find('"', q);
        if (a == std::string::npos || a > rb) break;
        const size_t b = js.find('"', a + 1);
        if (b == std::string::npos) break;
        files.push_back(js.substr(a + 1, b - a - 1));
        q = b + 1;
    }
    for (const std::string& f : files) {
        int w = 0, h = 0, c = 0;
        unsigned char* d = stbi_load((std::string(kModelDir) + f).c_str(), &w, &h, &c, 4);
        Atlas A;
        if (d) {
            A.w = w; A.h = h;
            A.a.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
            for (int i = 0; i < w * h; ++i) A.a[static_cast<size_t>(i)] = d[i * 4 + 3];
            stbi_image_free(d);
        }
        gAtlas.push_back(std::move(A));
    }
    size_t tot = 0;
    for (const Atlas& A : gAtlas) tot += A.a.size();
    log_("[zones] 纹理 alpha 表 %zu 张 / %zu px（用来把透明的大方片从格子图里剔掉）",
         gAtlas.size(), tot);
}

// 一个点不透明吗（UV → 纹理像素；UV 的 v=0 在图片第一行，和 load_tex 的上传顺序一致）
static bool uvOpaque(const Atlas& A, float u, float v) {
    if (A.w <= 0 || A.h <= 0) return true;          // 没载入就当它不透明（退回老行为）
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return false;
    const int x = static_cast<int>(u * (A.w - 1));
    const int y = static_cast<int>(v * (A.h - 1));
    return A.a[static_cast<size_t>(y) * static_cast<size_t>(A.w) + static_cast<size_t>(x)] > 40;
}

static void zoneInit(Csm::CubismModel* model) {
    loadAtlases();
    const std::map<std::string, std::string> names = loadPartNames();
    const Csm::csmInt32 nd = model->GetDrawableCount();
    gZones.lut.assign(static_cast<size_t>(nd) < 1 ? 1 : static_cast<size_t>(nd), 0);
    int mapped = 0, parts = 0;
    std::map<unsigned char, int> per;
    for (Csm::csmInt32 d = 0; d < nd; ++d) {
        const Csm::csmInt32 pi = model->GetDrawableParentPartIndex(static_cast<Csm::csmUint32>(d));
        if (pi < 0) continue;
        ++parts;
        const Csm::CubismIdHandle id = model->GetPartId(static_cast<Csm::csmUint32>(pi));
        const char* pid = (id && id->GetString().GetRawString()) ? id->GetString().GetRawString() : "";
        const auto it = names.find(pid);
        const unsigned char z = zoneOfPartName(it == names.end() ? "" : it->second.c_str());
        gZones.lut[static_cast<size_t>(d)] = z;
        if (z) { ++mapped; ++per[z]; }
    }
    gZones.ready = true;
    std::string s2;
    for (const auto& kv : per) {
        char b[64];
        std::snprintf(b, sizeof b, "%s:%d ", partpoke::zoneName(kv.first), kv.second);
        s2 += b;
    }
    log_("[zones] drawable %d 个，挂了 part 的 %d 个，认得出部位的 %d 个（%s）",
         static_cast<int>(nd), parts, mapped, s2.c_str());
}

// 把一件 drawable 的一个三角形光栅化进格子（窗口归一化坐标，v 向下）。
static void zoneTri(std::vector<unsigned char>& g, float ax, float ay, float bx, float by,
                    float cx, float cy, unsigned char lab) {
    const int GW = partpoke::gGridW, GH = partpoke::gGridH;
    float x0 = std::min(std::min(ax, bx), cx), x1 = std::max(std::max(ax, bx), cx);
    float y0 = std::min(std::min(ay, by), cy), y1 = std::max(std::max(ay, by), cy);
    int gx0 = static_cast<int>(x0 * GW) - 1, gx1 = static_cast<int>(x1 * GW) + 1;
    int gy0 = static_cast<int>(y0 * GH) - 1, gy1 = static_cast<int>(y1 * GH) + 1;
    if (gx0 < 0) gx0 = 0;
    if (gy0 < 0) gy0 = 0;
    if (gx1 > GW - 1) gx1 = GW - 1;
    if (gy1 > GH - 1) gy1 = GH - 1;
    const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    if (std::fabs(d) < 1e-9f) return;
    for (int gy = gy0; gy <= gy1; ++gy) {
        const float py = (gy + 0.5f) / GH;
        for (int gx = gx0; gx <= gx1; ++gx) {
            const float px = (gx + 0.5f) / GW;
            const float l1 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / d;
            const float l2 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / d;
            const float l3 = 1.0f - l1 - l2;
            if (l1 < -0.02f || l2 < -0.02f || l3 < -0.02f) continue;
            g[static_cast<size_t>(gy) * GW + gx] = lab;
        }
    }
}

// 用「她本人的 alpha」算出「哪些格子是她身上的」：每 10 帧（像素读回来的时候）
// 更新一次，5×5 采样、过四分之一不透明才算数。透明地方画的东西（眼镜、帽子、
// 素材盒里那些看不见的图层）几何上存在，但肉眼什么都没有，点上去不该有反应。
//   · 不在这里直接改格子：格子 20Hz 重刷，改完下一刷就没了（踩过）。
//   · 只算一张 cellOk 表，partZoneTick 光栅化完 AND 一下。
static void partZoneClip(const unsigned char* rgba, int w, int h, int th) {
    const int GW = partpoke::gGridW, GH = partpoke::gGridH;
    gZones.cellOk.assign(static_cast<size_t>(GW) * GH, 0);
    int ok = 0;
    const int N = 5;
    for (int gy = 0; gy < GH; ++gy) {
        for (int gx = 0; gx < GW; ++gx) {
            int hits = 0;
            for (int sy = 0; sy < N; ++sy) {
                for (int sx = 0; sx < N; ++sx) {
                    const int px = static_cast<int>((gx + (sx + 0.5f) / N) * w / GW);
                    const int py = static_cast<int>((gy + (sy + 0.5f) / N) * h / GH);
                    if (px < 0 || px >= w || py < 0 || py >= h) continue;
                    const int ry = h - 1 - py;             // 读回来首行在底部
                    if (rgba[(static_cast<size_t>(ry) * w + px) * 4 + 3] > th) ++hits;
                }
            }
            if (hits * 4 >= N * N) {
                gZones.cellOk[static_cast<size_t>(gy) * GW + gx] = 1;
                ++ok;
            }
        }
    }
    gZones.cellOkReady = true;
    if (gZones.clips < 3) {
        ++gZones.clips;
        log_("[zones] 按 alpha 算格子白名单：%d 格在她身上（%dx%d alpha，阈值 %d）",
             ok, w, h, th);
    }
}

// 每帧（限流 ~20Hz）：顶点 → 格子。k/W/H 和渲染用的是同一套投影。
// 二十五：补「内部的洞」。
// 症状：腿上有几片点下去判空白（他试了两次都是膝上那段和袜口），因为那几行
// 三角形没覆盖到，alpha 白名单又把边上的格子清掉了，中间就留下 3~10 格高的空条。
// 做法：从窗口四边把空白泛洪标成"外面"，剩下的空白块是封闭洞，整块填成邻居里
// 出现最多的那个部位。外面那一大片透明区连通到边界，永远不会被填。
// 太大的块也不填（>1/8 张图）：那不是洞，是腿之间或裙子底下真的空着。
static int zoneFillHoles(std::vector<unsigned char>& g, int GW, int GH) {
    const size_t N = g.size();
    if (N == 0) return 0;
    std::vector<unsigned char> out(N, 0), done(N, 0);
    std::vector<size_t> q;
    auto pushOut = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= GW || y >= GH) return;
        const size_t i = static_cast<size_t>(y) * GW + x;
        if (g[i] || out[i]) return;
        out[i] = 1; q.push_back(i);
    };
    for (int x = 0; x < GW; ++x) { pushOut(x, 0); pushOut(x, GH - 1); }
    for (int y = 0; y < GH; ++y) { pushOut(0, y); pushOut(GW - 1, y); }
    for (size_t h = 0; h < q.size(); ++h) {
        const int x = static_cast<int>(q[h] % GW), y = static_cast<int>(q[h] / GW);
        pushOut(x + 1, y); pushOut(x - 1, y); pushOut(x, y + 1); pushOut(x, y - 1);
    }
    const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int filled = 0;
    for (size_t s = 0; s < N; ++s) {
        if (g[s] || out[s] || done[s]) continue;
        std::vector<size_t> blob{s};
        done[s] = 1;
        int cnt[partpoke::kZoneMax] = {0};
        for (size_t h = 0; h < blob.size(); ++h) {
            const int x = static_cast<int>(blob[h] % GW), y = static_cast<int>(blob[h] / GW);
            for (int k = 0; k < 4; ++k) {
                const int nx = x + nb[k][1], ny = y + nb[k][0];
                if (nx < 0 || ny < 0 || nx >= GW || ny >= GH) continue;
                const size_t j = static_cast<size_t>(ny) * GW + nx;
                if (g[j]) { if (g[j] < partpoke::kZoneMax) ++cnt[g[j]]; }
                else if (!out[j] && !done[j]) { done[j] = 1; blob.push_back(j); }
            }
        }
        if (blob.size() > N / 8) continue;
        int best = 0, bn = 0;
        for (int i = 1; i < partpoke::kZoneMax; ++i) if (cnt[i] > bn) { bn = cnt[i]; best = i; }
        if (!best) continue;
        for (size_t i : blob) g[i] = static_cast<unsigned char>(best);
        filled += static_cast<int>(blob.size());
    }
    return filled;
}

static void partZoneTick(Csm::CubismModel* model, float k, int W, int H, double now) {
    if (!gZones.ready) { zoneInit(model); if (!gZones.ready) return; }
    if (now - gZones.lastBuild < 0.05) return;
    gZones.lastBuild = now;
    const int GW = partpoke::gGridW, GH = partpoke::gGridH;
    gZones.grid.assign(static_cast<size_t>(GW) * GH, 0);
    const Csm::csmInt32 nd = model->GetDrawableCount();
    // 二十四：每 20 帧打一次「每个部位这次画进格子多少、被哪道闸挡掉多少」。
    // 没有这行就只能看到「eye: 91 个 drawable」却一个格子都没有，猜不出是看不见、
    // 全透明、还是被后画的盖掉 —— 这一轮就是靠它定位的。
    int dbgTri[partpoke::kZoneMax] = {0}, dbgHide[partpoke::kZoneMax] = {0},
        dbgOpa[partpoke::kZoneMax] = {0}, dbgAlpha[partpoke::kZoneMax] = {0};
    float dbgBB[partpoke::kZoneMax][4];   // u0,v0,u1,v1（顶点坐标范围，验「三角形在不在画里」）
    for (int i = 0; i < partpoke::kZoneMax; ++i) {
        dbgBB[i][0] = 9; dbgBB[i][1] = 9; dbgBB[i][2] = -9; dbgBB[i][3] = -9;
    }
    // 二十四：**必须按 renderOrder 排，不能按下标**。旧注释说「drawable 下标就是
    // SDK 的绘制顺序」，在这个模型上是错的：渲染器（CubismRenderer_OpenGLES2::
    // DrawObjectLoop）是先用 GetRenderOrders() 把下标重排一遍再画的。
    // 按下标光栅化的后果实测过：眼睛/嘴/眉毛三个部位画进了 397/119/30 个三角形，
    // 格子却是 0 —— 全被后面画的「脸 / 五官」整片盖掉了。
    static std::vector<Csm::csmInt32> byOrder;
    byOrder.clear();
    {
        const Csm::csmInt32* order = model->GetRenderOrders();
        const Csm::csmInt32 total = nd + model->GetOffscreenCount();
        static std::vector<Csm::csmInt32> idxOf;
        idxOf.assign(static_cast<size_t>(total > nd ? total : nd), -1);
        if (order) {
            for (Csm::csmInt32 d0 = 0; d0 < nd; ++d0) {
                const Csm::csmInt32 o = order[d0];
                if (o >= 0 && o < total) idxOf[static_cast<size_t>(o)] = d0;
            }
            for (Csm::csmInt32 o = 0; o < total; ++o)
                if (idxOf[static_cast<size_t>(o)] >= 0) byOrder.push_back(idxOf[static_cast<size_t>(o)]);
        }
        if (byOrder.empty())
            for (Csm::csmInt32 d0 = 0; d0 < nd; ++d0) byOrder.push_back(d0);   // 没有 order 表就按下标
    }
    // 二十四：分两遍画。第一遍是老七块（head/chest/arm/skirt/leg/tail），第二遍是
    // 新拆出来的六块（eye/mouth/brow/ear/waist/headwear）—— 后一遍压在前一遍上面。
    // 为什么非要两遍：脸上那几个「脸黑/脸红/阴影/五官」也是 head，它们画在眼睛外面，
    // 单按 renderOrder 一遍画下来，眼睛/嘴/眉毛的格子照样被盖成 head（实测三个都是
    // 画进了 397/119/30 个三角形、格子 0）。小部位优先，才点得准。
    auto paintRange = [&](unsigned char lo, unsigned char hi) {
    for (size_t od = 0; od < byOrder.size(); ++od) {
        const Csm::csmInt32 d = byOrder[od];
        if (d < 0 || static_cast<size_t>(d) >= gZones.lut.size()) continue;
        const unsigned char lab = gZones.lut[static_cast<size_t>(d)];
        if (lab == 0 || lab >= partpoke::kZoneMax) continue;
        if (lab < lo || lab > hi) continue;          // 这一遍只画自己那一段标签
        if (!model->GetDrawableDynamicFlagIsVisible(d)) { ++dbgHide[lab]; continue; }
        if (model->GetDrawableOpacity(d) < 0.02f) { ++dbgOpa[lab]; continue; }
        const Csm::csmInt32 nv = model->GetDrawableVertexCount(d);
        const auto* vp = model->GetDrawableVertexPositions(d);
        if (!vp || nv < 3) continue;
        const Csm::csmUint16* idx = model->GetDrawableVertexIndices(d);
        const Csm::csmInt32 ni = model->GetDrawableVertexIndexCount(d);
        if (!idx || ni < 3) continue;
        // 画布坐标（y 向上）→ 窗口归一化（y 向下）
        auto nx = [&](int i) { return 0.5f + vp[idx[i]].X * k / static_cast<float>(W); };
        auto ny = [&](int i) { return 0.5f - vp[idx[i]].Y * k / static_cast<float>(H); };
        const Csm::csmInt32 ti = model->GetDrawableTextureIndex(d);
        const auto* uvs = model->GetDrawableVertexUvs(d);
        const Atlas* atlas = (ti >= 0 && static_cast<size_t>(ti) < gAtlas.size())
                             ? &gAtlas[static_cast<size_t>(ti)] : nullptr;
        for (Csm::csmInt32 t = 0; t + 2 < ni; t += 3) {
            const int a = idx[t], b2 = idx[t + 1], c2 = idx[t + 2];
            if (a >= nv || b2 >= nv || c2 >= nv) continue;
            // 二十三：整块透明的三角形别往上涂（大腿那种方片压着裙摆）
            if (atlas && uvs) {
                const float u0 = uvs[a].X, v0 = uvs[a].Y, u1 = uvs[b2].X, v1 = uvs[b2].Y,
                            u2 = uvs[c2].X, v2 = uvs[c2].Y;
                int hit = 0;
                if (uvOpaque(*atlas, (u0 + u1 + u2) / 3.0f, (v0 + v1 + v2) / 3.0f)) ++hit;
                if (uvOpaque(*atlas, (u0 + u1) / 2.0f, (v0 + v1) / 2.0f)) ++hit;
                if (uvOpaque(*atlas, (u1 + u2) / 2.0f, (v1 + v2) / 2.0f)) ++hit;
                if (uvOpaque(*atlas, (u2 + u0) / 2.0f, (v2 + v0) / 2.0f)) ++hit;
                if (hit < 2) { ++dbgAlpha[lab]; continue; }
            }
            ++dbgTri[lab];
            {
                const float vx[3] = {nx(t), nx(t + 1), nx(t + 2)};
                const float vy[3] = {ny(t), ny(t + 1), ny(t + 2)};
                for (int q = 0; q < 3; ++q) {
                    if (vx[q] < dbgBB[lab][0]) dbgBB[lab][0] = vx[q];
                    if (vy[q] < dbgBB[lab][1]) dbgBB[lab][1] = vy[q];
                    if (vx[q] > dbgBB[lab][2]) dbgBB[lab][2] = vx[q];
                    if (vy[q] > dbgBB[lab][3]) dbgBB[lab][3] = vy[q];
                }
            }
            zoneTri(gZones.grid, nx(t), ny(t), nx(t + 1), ny(t + 1), nx(t + 2), ny(t + 2), lab);
        }
    }
    };
    paintRange(1, 7);     // 老七块（下标 1..7，7 = other）
    paintRange(8, 13);    // 二十四：眼睛 / 嘴 / 眉毛 / 兽耳 / 腰 / 头饰
    // 二十四：光栅化完、还没被 alpha 白名单和「借标签」动过的那一份，拿来对数
    static std::vector<unsigned char> dbgRaw;
    if (gZones.built < 20) dbgRaw = gZones.grid;
    // 二十五：记一份「各部位光栅化出来的原始格数」。
    // 补洞要靠它当闸门 —— 小部位（发饰只有 120 来格）一旦被允许向外借，
    // 8 圈下来会把整个头顶的头发都染成 headwear，「头」就只剩半张脸（6.47 掉到 0.55 那次）。
    int rawCnt[partpoke::kZoneMax] = {0};
    for (size_t i = 0; i < gZones.grid.size(); ++i)
        if (gZones.grid[i] && gZones.grid[i] < partpoke::kZoneMax) ++rawCnt[gZones.grid[i]];

    // 只留她身上的格子（alpha 白名单）
    if (gZones.cellOkReady && gZones.cellOk.size() == gZones.grid.size()) {
        for (size_t i = 0; i < gZones.grid.size(); ++i)
            if (!gZones.cellOk[i]) gZones.grid[i] = 0;
        // 二十四：脸拆三块 —— 按**位置**切三条横带，不靠 drawable 的 bbox 自适应。
        // 为什么不用 drawable 的范围：眼睛那条缝只有 20 px 高，眼睑（眨下去那层）
        // 挂在「五官」上被判成 head，嘴唇和眉线也混在 surrounding 的 head 格子里。
        // 设 bbox 自适应那版量出来是：眨眼 225 px 落在 eye、561 px 落在 head（0.40×）。
        // 三条带的上下界是拿参数实测出来的：
        //   眉毛 min..max  u .442..0.562  v .197..0.228
        //   眼睛（眨）    u .448..0.564  v .208..0.229
        //   嘴（开）      u .490..0.510  v .240..0.251
        // 三条带**互不重叠**，都只压在剪影里（cellOk）。眉毛那条只到 0.207 ——
        // 眉毛抬起来时往上走，留在这条带里；它往下甩才会掉进眼睛那条，所以眉毛的
        // 方向固定成「往上」（见 state/parts.tsv 里 PokeBrow 的 up 标记）。
        // 二十五 → Step 3：判定带改成**从 partapi 读**（唯一一份 state/parts.tsv）。
        //
        // 原来这里有 70 行：自读 state/part-bands.tsv + 一份 9 条的内置兜底 kFallback。
        // 那份兜底比文件少两条（缺 skirt 左右飘带尖端）—— 文件读不到时会被判成 leg/tail，
        // 正是今天「飘带判成 leg」的一半来源（另一半是格子图把裙摆标成腿，已在 23.5 修掉）。
        // 现在整段删掉：读不到表 = 空注册表 = 一条带都没有 = 点击回整套反应。
        // 「用一份可能过期的表」换成「什么都不做」，这才是安全的降级。
        //
        // 规则没变：只填「剪影内(cellOk)且还没有标签」的格子；force 无条件盖；u/v 两端闭区间。
        // scope=any 的那两条（兽耳硬判）不进格子图 —— 它们只在 classify 的第一步用，
        // 所以这里应用的是 11 条，日志里能把这两个数分开看。
        {
            const int applied = partapi::applyBands(gZones.grid, GW, GH, &gZones.cellOk);
            static int lastApplied = -1;
            if (applied != lastApplied) {
                lastApplied = applied;
                log_("[zones] 判定带：partapi 应用 %d 条（scope=body，剪影内、补白不抢已有标签）",
                     applied);
            }
        }
        // 剪影里、但没有任何部位 drawable 盖到的格子（皮肤、缝隙、作者的「组 N」素材）
        // → 从最近的已标格借标签，最多借两圈。不补的话点在她身上却判成「空白」，
        // 走整套反应（实测 17% 的剪影是这样）。借到剪影外也没关系：wl_surface 的
        // input region 本来就是按 alpha 建的，剪影外的点根本到不了这里。
        std::vector<size_t> q;
        std::vector<signed char> depth(gZones.grid.size(), -1);
        for (size_t i = 0; i < gZones.grid.size(); ++i)
            if (gZones.grid[i]) { q.push_back(i); depth[i] = 0; }
        for (size_t h2 = 0; h2 < q.size(); ++h2) {
            const size_t i = q[h2];
            if (depth[i] >= 2) continue;
            const int gy = static_cast<int>(i / GW), gx = static_cast<int>(i % GW);
            const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int k = 0; k < 4; ++k) {
                const int ny = gy + nb[k][0], nx = gx + nb[k][1];
                if (ny < 0 || ny >= GH || nx < 0 || nx >= GW) continue;
                const size_t j = static_cast<size_t>(ny) * GW + nx;
                if (gZones.grid[j] || depth[j] >= 0) continue;
                gZones.grid[j] = gZones.grid[i];
                depth[j] = static_cast<signed char>(depth[i] + 1);
                q.push_back(j);
            }
        }
        // 二十五：剪影里**还剩下**的空白再借一次，这次最远 8 圈。
        // 腿上那 11 行空条（袜口附近）两边都是 leg，两圈够不着，所以才漏；而第一遍
        // 不敢放宽，是怕把裙子/手臂的轮廓往外撑。这一遍只走 cellOk（她身上），
        // 剪影外一格都不动，口径正好。
        {
            std::vector<size_t> q2;
            std::vector<signed char> d2(gZones.grid.size(), -1);
            for (size_t i = 0; i < gZones.grid.size(); ++i)
                if (gZones.grid[i]) { q2.push_back(i); d2[i] = 0; }
            int filledIn = 0;
            for (size_t h2 = 0; h2 < q2.size(); ++h2) {
                const size_t i = q2[h2];
                if (d2[i] >= 8) continue;
                const int gy = static_cast<int>(i / GW), gx = static_cast<int>(i % GW);
                const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (int k = 0; k < 4; ++k) {
                    const int ny = gy + nb[k][0], nx = gx + nb[k][1];
                    if (ny < 0 || ny >= GH || nx < 0 || nx >= GW) continue;
                    const size_t j = static_cast<size_t>(ny) * GW + nx;
                    if (!gZones.cellOk[j]) continue;          // 只补剪影里
                    if (gZones.grid[j] || d2[j] >= 0) continue;
                    if (rawCnt[gZones.grid[i]] < 150) continue;   // 小部位不许向外借
                    gZones.grid[j] = gZones.grid[i];
                    d2[j] = static_cast<signed char>(d2[i] + 1);
                    q2.push_back(j);
                    ++filledIn;
                }
            }
                    if (filledIn) gZones.lastFillIn = filledIn;
        }
    }
    // 二十五：落盘/判定之前把内部空洞补上
    {
        const int filled = zoneFillHoles(gZones.grid, GW, GH);
        // 2026-11-11 20:4x：改成只在**补洞格数真的变了**才报（原来每 10 秒一条，
        // 一天下来几千行，翻日志时全是它）。呼吸带来的 1~2 格抖动不看。
        static int lastFilled = -1;
        if (std::abs(filled - lastFilled) >= 8) {
            lastFilled = filled;
            log_("[zones] 补洞：封闭块 %d 格 / 剪影内 %d 格", filled, gZones.lastFillIn);
        }
    }
    // 交给平台层（同一个 inline 对象）
    partpoke::gZoneCells = gZones.grid;
    partpoke::gZoneWinW = W;
    partpoke::gZoneWinH = H;
    ++partpoke::gZoneStamp;
    ++gZones.built;
    if (gZones.built == 20) {   // 二十帧后打一次各部位的实际范围，验「格子贴不贴身体」
        int rawN[partpoke::kZoneMax] = {0}, finN[partpoke::kZoneMax] = {0};
        if (dbgRaw.size() == gZones.grid.size())
            for (size_t i = 0; i < dbgRaw.size(); ++i)
                if (dbgRaw[i] && dbgRaw[i] < partpoke::kZoneMax) ++rawN[dbgRaw[i]];
        for (size_t i = 0; i < gZones.grid.size(); ++i)
            if (gZones.grid[i] && gZones.grid[i] < partpoke::kZoneMax) ++finN[gZones.grid[i]];
        for (int i = 1; i < partpoke::kZoneMax; ++i)
            if (dbgTri[i] || dbgHide[i] || dbgOpa[i] || dbgAlpha[i])
                log_("[zones] %-8s 画进 %d 个三角形，挡掉：不可见 %d / 透明 %d / 纹理全透 %d ｜ 格子 %d → %d ｜ 画到 u %.3f..%.3f v %.3f..%.3f",
                     partpoke::zoneName(i), dbgTri[i], dbgHide[i], dbgOpa[i], dbgAlpha[i],
                     rawN[i], finN[i], dbgBB[i][0], dbgBB[i][2], dbgBB[i][1], dbgBB[i][3]);
        float bb[partpoke::kZoneMax][4];
        for (int i = 0; i < partpoke::kZoneMax; ++i) {
            bb[i][0] = 2; bb[i][1] = 2; bb[i][2] = -2; bb[i][3] = -2;
        }
        for (int gy = 0; gy < GH; ++gy)
            for (int gx = 0; gx < GW; ++gx) {
                const unsigned char c = gZones.grid[static_cast<size_t>(gy) * GW + gx];
                if (!c || c >= partpoke::kZoneMax) continue;
                const float u = (gx + 0.5f) / GW, v = (gy + 0.5f) / GH;
                if (u < bb[c][0]) bb[c][0] = u;
                if (v < bb[c][1]) bb[c][1] = v;
                if (u > bb[c][2]) bb[c][2] = u;
                if (v > bb[c][3]) bb[c][3] = v;
            }
        for (int i = 1; i < partpoke::kZoneMax; ++i)
            if (bb[i][2] > bb[i][0])
                log_("[zones] %s: u %.3f..%.3f  v %.3f..%.3f", partpoke::zoneName(i),
                     bb[i][0], bb[i][2], bb[i][1], bb[i][3]);
    }
    // 二十四：**先等 alpha 白名单算出来再落盘**（built≥30，约 0.5 秒后）。
    // 以前第 1 帧就写，那一版没有 cellOk、也没有脸部横带和腰那条带子 —— 工具拿它
    // 当分区图，量的和判的不是一份数据（这一轮就是被这个坑过一次：文件里嘴只有 1 格）。
    if (gZones.built >= 30 && now - gZones.lastDump > 3.0) {
        gZones.lastDump = now;
        // 2026-11-10 13:3x：这份文件是给人看/给工具读的**快照**，判定用的是内存里的 grid。
        // 之前它每 8 秒重写一次，把我手工标定的矩形版盖掉了，于是判定又回到那套
        // 列值越界（>60）的坏数据上——点胸口被认成手臂，就是这么来的。
        // 二十四：那次之后改成「只写一次」，可那样又会出现相反的问题——工具量倍数时
        // 读的是很久以前那版；而判「点到哪块」用的是此刻内存里的 grid，两边可能对不上。
        // 现在折中：内容**变了才写**，最多 8 秒一次 —— 文件始终跟判定用的是同一份，
        // 又不会像原来那样把任何手工数据无条件盖掉。
        if (gZones.last != gZones.grid) {
            gZones.last = gZones.grid;
            FILE* fz = std::fopen("state/part-zones.tsv", "wb");
            if (fz) {
                std::fprintf(fz, "#grid\t%d\t%d\n", GW, GH);
                std::fprintf(fz, "# 部位格子图（身体自己按 drawable→part 光栅化出来的，"
                                 "窗口归一化：u=lx/窗口宽、v=ly/窗口高）\n");
                for (int gy = 0; gy < GH; ++gy) {
                    int run = 0;
                    for (int gx = 1; gx <= GW; ++gx) {
                        const unsigned char a = gZones.grid[static_cast<size_t>(gy) * GW + run];
                        if (gx == GW || gZones.grid[static_cast<size_t>(gy) * GW + gx] != a) {
                            if (a) std::fprintf(fz, "Z\t%d\t%d\t%d\t%d\t%s\n", gy, gy + 1, run, gx,
                                                partpoke::zoneName(a));
                            run = gx;
                        }
                    }
                }
                std::fclose(fz);
            }
        }
    }
}

// ── 二十一：点哪动哪（部位级反应）───────────────────────────────────────
// 以前点她是 `plans/被戳*.json` —— 那几份计划写的全是 AngleX/AngleZ/ParamEyeBallX
// 这种**整体**参数，所以不管点哪儿，都是「整个人躲一下」。
//
// 探针（tools/param-probe.py + state/param-map.json）把 509 个参数逐个按到
// min/max 截图，量出「这个参数动的是她身上哪一块、动多少像素」。有了那张表，
// 点哪儿就只挑那一块的 2–4 个参数做一次「弹一下」：
//
//   u = 0      →  0.16 到 60% 的极值（快，60ms 量级）
//   0.16..0.42 →  回过到 −20%（那一下回弹，看着才像被戳了）
//   0.42..1.0  →  归零（0.62s 结束，参数回到默认值）
//
// 三件事必须同时成立才走这条路，否则照旧播整套 plan：
//   ① 设置里 `tapPart` 开着  ② 点在部位矩形里  ③ 那个部位**确实有**活着的参数
struct PartPokeRig {
    bool   on = false;
    int    lastGen = 0;
    double t0 = 0.0, dur = 0.62, lastT = 0.0;
    std::string part;
    // 2026-11-10 02:4x：他要「物理运动」，不是线性弹一下。
    // 每个参数带自己的位移 x 和速度 v，点一下给冲量，之后靠弹簧-阻尼自己晃回去，
    // 会过冲、会震荡两下才停——那才像被戳过的软地方。
    struct Ch { Csm::csmInt32 idx; float base, peak, dip; float x, v; };
    Ch     ch[4];
    int    n = 0;
    // 二十四：点眼睛时额外补一个三角形眨眼包络（见 partPokeTick）。眼珠那条物理链
    // 只能摆瞳孔，睁眼/闭眼是 ParamEyeLOpen/ROpen 的事，而且那两个不吃物理输出
    // （每帧硬覆盖会把眨眼动画整段摁掉），只能从这里直接给。
    bool   eyeOn = false;
    double eyeT0 = 0.0;
    Csm::csmInt32 eyeL = -1, eyeR = -1;
};
static PartPokeRig gPartPoke;

// ═══════════════════════════════════════════════════════════════════
// 二十六：**她的状态**。
//
// 2026-11-11 00:5x 他说「这个是米鱼在我电脑上的身体，你要好好思考」。
// 在这之前我一直在往里加功能（双击、连击、心情随机切换），方向是错的：
// 那些都是「点她会有反应」，但**她自己的状态跟身体没有关系**。
// 心情在 whale-memory 里明明有四个真实的数（此刻 tired=95、happy=8.9），
// 身体却一直挺着、一直在笑、被碰就弹 —— 那不像一个人，像个玩具。
//
// 所以这一段是「身体的输入」：每 30 秒把她的心情搬进来，然后
//   · 累了 → 整个人沉下去、被碰也弹不起来、眨眼变慢
//   · 开心 → 挺起来、反应快、容易脸红
//   · 难过 → 头低一点、动作更小
//   · 在乎 → 更愿意看过来
// 全部是**连续的**，不是开关。她不会「突然切换成累的样子」，而是随着数字慢慢沉。
struct HerState {
    float mood = 50, happy = 0, tired = 0, sad = 0, care = 0;
    double at = 0;
    bool  valid = false;
};
static HerState gHer;

static void parseMoodJson(const std::string& t) {
    auto num = [&](const char* key) -> float {
        const std::string k = std::string("\"") + key + "\":";
        const size_t p = t.find(k);
        if (p == std::string::npos) return 0.0f;
        return static_cast<float>(std::atof(t.c_str() + p + k.size()));
    };
    gHer.mood  = num("mood");
    gHer.happy = num("happy");
    gHer.tired = num("tired");
    gHer.sad   = num("sad");
    gHer.care  = num("care");
    gHer.at    = num("at");
    gHer.valid = gHer.at > 0;
}

// 每 30 秒跑一次同步脚本并读回来。脚本只读库、写一个小 json，失败就保持上次的值。
static void herStateTick(double now) {
    static double lastTry = -1e9;
    if (now - lastTry < 30.0) return;
    lastTry = now;
    ::system("python3 /home/link/.dsh/miyu/body/native3/tools/mood-sync.py >/dev/null 2>&1 &");
    std::ifstream f("state/mood.json");
    if (!f) return;
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!t.empty()) parseMoodJson(t);
}

// 二十六：动作要**排他**。
// 2026-11-11 01:0x 他说「不要同时播放两个动作呀」—— 我让她飘过去的同时又播「脱捕」，
// 而脱捕自己也写 AngleX/Y/Z、PositionX/Y/Z，两边抢同一批参数，画面就是在打架。
//
// 规则（优先级从高到低）：
//   motion（举手/竖手指/脱捕） > plan（点头/害羞） > 走路 > 待机
// 位置（窗口在哪）和姿态（身体怎么写）是两件事：走路时**位置照走**，
// 但姿态让给正在播的 motion/plan。
static bool postureBusy(Csm::CubismMotionManager* mm) {
    if (mm && !mm->IsFinished()) return true;      // 有 motion 在播
    if (plan::active()) return true;               // 有计划动作在播
    return false;
}

// ── 二十六：走路 ────────────────────────────────────────────────
//
// 2026-11-11 00:4x：「她还不可以走路呢，能不能去做走路，就是移动」。
//
// 模型里**没有迈步的骨头**（只有站着待机 + 拖动的物理跟随），所以「走」只能是
// 三件事叠出来的感觉，缺一不可：
//   ① 窗口真的在动（platSetPosition），而且速度是人在走的速度（~110 px/s），不是瞬移
//   ② 身体在动：上下起伏 + 左右轻摆 + 腿交替张合（用 PokeLeg 正负交替）
//   ③ 面朝方向：往左走向左偏一点、往右走向右偏 —— 不面朝就走是滑行
// 缺了 ②③ 就是「一个立绘被平移」，那正是他之前说的「挪图片」。
//
// 到点容差 2px，到了就停，停在原地呼吸。
static const float kWalkSpeed = 110.0f;     // px/s
// ── 二十六：摄像头模式 ──────────────────────────────────────────
//
// 2026-11-11 23:5x 他提的设计：两套模式。米露模式是现在这样（自己动、自己的状态），
// 摄像头模式是镜头里他的动作直接映到身上。
//
// 分工：`tools/face-track.py` 用 mediapipe 把脸算成几个标量写 `state/face.json`；
// 这里每帧读它、平滑、写进参数。和 mood.json / world.json 一个套路。
//
// **谁是这一帧最后写参数的人**：faceTick 必须在 physics->Evaluate 之后、Update 之前，
// 而且要**压过**视线跟随、眨眼、待机 —— 摄像头模式开着的时候，姿态由他决定，不由程序猜。
struct FaceRig {
    Csm::csmInt32 iAx = -1, iAy = -1, iAz = -1;
    Csm::csmInt32 iEyeL = -1, iEyeR = -1, iMouth = -1;
    Csm::csmInt32 iEx = -1, iEy = -1;
    Csm::csmInt32 iBx = -1, iBy = -1, iBz = -1;      // ParamBodyAngleX/Y/Z：身子跟着转
    Csm::csmInt32 iCtrlL = -1, iCtrlR = -1;          // 耳物理链的权重（0 = 兽耳不跟头）
    Csm::csmInt32 iBrowL = -1, iBrowR = -1;          // 眉毛（笑/皱眉）
    Csm::csmInt32 iSmile = -1;                        // 嘴角
    // 平滑后的当前值（度 / 0..1 / -1..1）
    float ax = 0, ay = 0, az = 0, eyeL = 1, eyeR = 1, mouth = 0, ex = 0, ey = 0;
    float bx = 0, bz = 0;            // 身体转
    float brow = 0, smile = 0;       // 表情：抬头纹 / 嘴角
    bool  ready = false;
    bool  live = false;      // 这一次读到的 face.json 是「有脸」的
    double lastRead = -1e9;
    // 2026-11-12 00:3x：他要 60fps。**摄像头最高只有 30fps**（v4l2 查过，所有分辨率都是
    // 30 封顶），采集端提不上去。所以改成在渲染侧插值：记住「上一份数据」和「这一份数据」
    // 加各自的时间戳，每帧按 now 在两者之间线性推进 —— 运动曲线就是 200fps 采样的，
    // 不再有 33ms 一次的阶梯。
    float pv[9] = {0, 0, 0, 1, 1, 0, 0, 0, 0};   // 上一份（ax ay az eyeL eyeR mouth ex ey smile）
    float cv[9] = {0, 0, 0, 1, 1, 0, 0, 0, 0};   // 这一份
    double tNew = -1e9, tPrev = -1e9;
};
static FaceRig gFace;

// 2026-11-12 01:3x：他要「不要手动启动，跟着开关来」。
// 设置里把「摄像头模式」打开 → fork 一个 tools/face-track.sh；关掉 → SIGTERM。
// PR_SET_PDEATHSIG 保证身体进程死了，它也不会变成孤儿占着摄像头。
static pid_t gFacePid = -1;
static bool  gFaceSpawned = false;

static void faceSpawn(bool on) {
    if (on == gFaceSpawned) return;
    gFaceSpawned = on;
    if (on) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            ::setsid();
            ::execl("/bin/sh", "sh", "tools/face-track.sh", (char*)nullptr);
            ::_exit(127);                       // execl 失败
        }
        gFacePid = pid;
        log_("[face] 摄像头跟踪已启动（pid %d）", (int)pid);
    } else {
        if (gFacePid > 0) {
            ::kill(gFacePid, SIGTERM);
            log_("[face] 摄像头跟踪已停（pid %d）", (int)gFacePid);
            gFacePid = -1;
        }
        // 顺手把 face.json 清成「没脸」，免得关掉之后身体还在读残留数据
        FILE* f = std::fopen("state/face.json", "w");
        if (f) { std::fputs("{\"ok\": false}", f); std::fclose(f); }
    }
}

static void faceInit(Csm::CubismModel* model) {
    Csm::CubismIdManager* m = Csm::CubismFramework::GetIdManager();
    // 2026-11-12 00:1x：**第三次才对**。查 physics3.json 看明白了整条链——
    //   PhysicsSetting1: 输入 AngleX  → 输出 ParamAngleX
    //   PhysicsSetting2: 输入 AngleY  → 输出 ParamAngleY
    //   PhysicsSetting3: 输入 AngleZ  → 输出 ParamAngleZ
    // 也就是 **AngleX 是物理输入、ParamAngleX 是物理输出**（带弹性的那一个）。
    // 写给 ParamAngleX：当帧能动，下一帧被 physics 算回去 → 变成抖。
    // 写给 AngleX：physics 基于它算出 ParamAngleX → 头动，而且头发/兽耳跟着甩。
    // 所以这里写 AngleX/Y/Z，并且必须放在 physics->Evaluate 之前。
    gFace.iAx  = model->GetParameterIndex(m->GetId("AngleX"));
    gFace.iAy  = model->GetParameterIndex(m->GetId("AngleY"));
    gFace.iAz  = model->GetParameterIndex(m->GetId("AngleZ"));
    gFace.iEyeL = model->GetParameterIndex(m->GetId("ParamEyeLOpen"));
    gFace.iEyeR = model->GetParameterIndex(m->GetId("ParamEyeROpen"));
    gFace.iMouth = model->GetParameterIndex(m->GetId("ParamMouthOpenY"));
    gFace.iEx  = model->GetParameterIndex(m->GetId("ParamEyeBallX"));
    gFace.iEy  = model->GetParameterIndex(m->GetId("ParamEyeBallY"));
    // 2026-11-12 00:4x：他问「身体怎么不会跟着我动」—— 第一版只映了头。
    // 真人是头转多少、肩和胯跟着转一部分（大约 1/3），脖子才不会扭成 90 度。
    gFace.iBx  = model->GetParameterIndex(m->GetId("ParamBodyAngleX"));
    gFace.iBy  = model->GetParameterIndex(m->GetId("ParamBodyAngleY"));
    gFace.iBz  = model->GetParameterIndex(m->GetId("ParamBodyAngleZ"));
    // 2026-11-12 00:1x：他说「兽耳不应该受到影响」。查 physics3 的耳链：
    //   PhysicsSetting83: 输入 [LControl, AngleX, AngleY, AngleZ, ParamBreath, ParamEyeROpen]
    // 也就是说兽耳本来就被设计成「跟着头甩」。LControl/RControl 是那条链的权重，
    // 设成 0 就不跟了 —— 摄像头模式下断开，让她自己的待机物理继续管耳朵。
    gFace.iCtrlL = model->GetParameterIndex(m->GetId("LControl"));
    gFace.iCtrlR = model->GetParameterIndex(m->GetId("RControl"));
    gFace.iBrowL = model->GetParameterIndex(m->GetId("ParamBrowLY"));
    gFace.iBrowR = model->GetParameterIndex(m->GetId("ParamBrowRY"));
    gFace.iSmile = model->GetParameterIndex(m->GetId("MouthSmile_L"));
    gFace.ready = true;
    log_("[face] 摄像头模式：AngleX=%d AngleY=%d AngleZ=%d 眼=%d/%d 嘴=%d 眼球=%d/%d",
         gFace.iAx, gFace.iAy, gFace.iAz, gFace.iEyeL, gFace.iEyeR, gFace.iMouth, gFace.iEx, gFace.iEy);
}

// 从 face.json 取一个 float 字段（文件很小，不值当上 JSON 库）
static float jnum(const std::string& t, const char* key, float dflt) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = t.find(k);
    if (p == std::string::npos) return dflt;
    return static_cast<float>(std::atof(t.c_str() + p + k.size()));
}

static void faceTick(Csm::CubismModel* model, float dt, double now, bool on) {
    if (!gFace.ready) faceInit(model);
    faceSpawn(on);          // 开关一变就起/停跟踪进程，不用手动跑
    if (!on) { gFace.live = false; return; }

    // 每 1/30 秒读一次就够（py 那边写的是原子替换，不会读到半个文件）
    if (now - gFace.lastRead > 0.010) {   // 2026-11-12 00:1x：33ms 太疏，延迟堆起来像「不动」
        gFace.lastRead = now;
        std::ifstream f("state/face.json");
        if (f) {
            const std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // 2026-11-12 00:5x：这里踩了个小坑 —— python 的 json.dump 写出来是
            // `"ok": true`（冒号后面有空格），我原来只找 `"ok":true`，一个字都不差地
            // 匹配不上，于是永远判成「没脸」。现在两种写法都认。
            const bool ok = t.find("\"ok\": true") != std::string::npos
                         || t.find("\"ok\":true") != std::string::npos;
            gFace.live = ok;
            if (ok) {
                gFace.ax = jnum(t, "yaw", 0) * -1.0f;    // 他往右转 → 我也往右（镜像）
                // 2026-11-12 01:0x：他说「我的头向上，她的头向下，反过来了」——实测反的，取负。
                // 之前我在 FORCE_PARAM 上试过 ParamAngleY 正 = 低头，但那是「直接按死输出」；
                // 走 AngleY（物理输入）这条路时符号是反的，两个通道不是一回事。
                gFace.ay = jnum(t, "pitch", 0) * -1.0f;
                gFace.az = jnum(t, "roll", 0);
                gFace.eyeL = jnum(t, "eyeL", 1);
                gFace.eyeR = jnum(t, "eyeR", 1);
                gFace.mouth = jnum(t, "mouth", 0);
                gFace.ex = jnum(t, "eyeX", 0);
                gFace.ey = jnum(t, "eyeY", 0);
                gFace.brow = jnum(t, "brow", 0);
                gFace.smile = jnum(t, "smile", 0);
                // 把旧的挪成 prev，新的填进 cv
                for (int i = 0; i < 9; ++i) gFace.pv[i] = gFace.cv[i];
                gFace.tPrev = gFace.tNew;
                gFace.cv[0] = gFace.ax; gFace.cv[1] = gFace.ay; gFace.cv[2] = gFace.az;
                gFace.cv[3] = gFace.eyeL; gFace.cv[4] = gFace.eyeR; gFace.cv[5] = gFace.mouth;
                gFace.cv[6] = gFace.ex;  gFace.cv[7] = gFace.ey;  gFace.cv[8] = gFace.smile;
                gFace.tNew = now;
            }
        }
    }

    // 脸丢了就慢慢回默认（硬切会像断电）
    static float idle = 0.0f;
    const float a = (dt * 22.0f > 1.0f) ? 1.0f : dt * 22.0f;   // 22：延迟每降一点，跟手感都好一截
    const float at = gFace.live ? a : (dt * 3.0f > 1.0f ? 1.0f : dt * 3.0f);

    static float sax = 0, say = 0, saz = 0, sEyeL = 1, sEyeR = 1, sMouth = 0, sex = 0, sey = 0;
    static float sbrow = 0, ssmile = 0;
    const float tax = gFace.live ? gFace.ax : 0.0f;
    const float tay = gFace.live ? gFace.ay : 0.0f;
    const float taz = gFace.live ? gFace.az : 0.0f;
    const float tEyeL = gFace.live ? gFace.eyeL : 1.0f;
    const float tEyeR = gFace.live ? gFace.eyeR : 1.0f;
    const float tMouth = gFace.live ? gFace.mouth : 0.0f;
    const float tex = gFace.live ? gFace.ex : 0.0f;
    const float tey = gFace.live ? gFace.ey : 0.0f;
    sax += (tax - sax) * at;         say += (tay - say) * at;       saz += (taz - saz) * at;
    sEyeL += (tEyeL - sEyeL) * a;    sEyeR += (tEyeR - sEyeR) * a;
    sMouth += (tMouth - sMouth) * a;
    sex += (tex - sex) * a;          sey += (tey - sey) * a;
    const float tBrow = gFace.live ? gFace.brow : 0.0f;
    const float tSmile = gFace.live ? gFace.smile : 0.0f;
    sbrow += (tBrow - sbrow) * a;    ssmile += (tSmile - ssmile) * a;

    // 幅度：模型 AngleX 满量程 ±30，但真人转 30° 已经很夸张，压到 0.7 更像活人
    const float kAng = 0.9f;
    if (gFace.iAx >= 0) model->SetParameterValue(gFace.iAx, sax * kAng, 1.0f);
    if (gFace.iAy >= 0) model->SetParameterValue(gFace.iAy, say * kAng, 1.0f);
    if (gFace.iAz >= 0) model->SetParameterValue(gFace.iAz, saz * 0.5f, 1.0f);
    if (gFace.iEyeL >= 0) model->SetParameterValue(gFace.iEyeL, sEyeL, 1.0f);
    if (gFace.iEyeR >= 0) model->SetParameterValue(gFace.iEyeR, sEyeR, 1.0f);
    // 嘴：他说话时我们的 ctl 也在写嘴，摄像头模式**不抢嘴**（不然她自己说话时嘴会抖）
    if (gFace.iMouth >= 0 && ctl::mouth(now) < 0.0f) {
        model->SetParameterValue(gFace.iMouth, sMouth, 1.0f);
    }
    if (gFace.iEx >= 0) model->SetParameterValue(gFace.iEx, sex, 1.0f);
    if (gFace.iEy >= 0) model->SetParameterValue(gFace.iEy, sey, 1.0f);

    // 2026-11-12 00:3x：**60fps 的插值**。摄像头只能给 30fps，所以这里不变频，
    // 而是在两份数据之间按 now 线性推进 —— 渲染侧每帧（现在 180+）都拿到一个新值，
    // 运动曲线是连续的一点一点走，不再是每 33ms 跳一格。
    // 插值比例超过 1 就停住（数据还没来，不能预测过头，否则会甩）。
    {
        // 2026-11-12 00:4x：他说「头转动我要流畅」。摄像头硬上限就是 30fps
        // （v4l2 查过，MJPG/YUYV 所有分辨率都是 30 封顶），采集端提不上去。
        // 光做线性插值不够 —— 数据点处方向会突变，还是能看出 33ms 一拐。
        // 所以改成：**线性插值给目标 + 临界阻尼弹簧跟随**。
        // 弹簧有速度，过数据点时不会急停急拐，曲线是顺的；而且它能"吃掉"抖动。
        const double span = (gFace.tNew > gFace.tPrev) ? (gFace.tNew - gFace.tPrev) : 0.033;
        // 2026-11-12 01:0x：他说「现在延迟好高」。插值那一步是元凶之一 ——
        // 它按 (now - tNew) 在「上一份」和「这一份」之间推进，等于把画面再往后拖 33ms。
        // 数据本来就晚 33ms 了，再插一次就 66ms。改成**直接用最新一份**，
        // 阶梯交给弹簧去抹平（弹簧本来就是低通）。
        (void)span;
        const float k = 1.0f;
        const float iv[9] = {
            gFace.pv[0] + (gFace.cv[0] - gFace.pv[0]) * k,
            gFace.pv[1] + (gFace.cv[1] - gFace.pv[1]) * k,
            gFace.pv[2] + (gFace.cv[2] - gFace.pv[2]) * k,
            gFace.pv[3] + (gFace.cv[3] - gFace.pv[3]) * k,
            gFace.pv[4] + (gFace.cv[4] - gFace.pv[4]) * k,
            gFace.pv[5] + (gFace.cv[5] - gFace.pv[5]) * k,
            gFace.pv[6] + (gFace.cv[6] - gFace.pv[6]) * k,
            gFace.pv[7] + (gFace.cv[7] - gFace.pv[7]) * k,
            gFace.pv[8] + (gFace.cv[8] - gFace.pv[8]) * k,
        };
        if (gFace.live) {
            // 临界阻尼弹簧：k1 定快慢，k2 = 2*sqrt(k1) 刚好不过冲。
            // 角度要跟得紧（人转头很快），眼睛和嘴可以慢一点。
            static float sv2[9] = {0, 0, 0, 1, 1, 0, 0, 0, 0};   // 当前值
            static float vv2[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};   // 速度
            const float k1Ang = 2400.0f, k2Ang = 98.0f;    // 角度：约 40ms 到位（延迟优先）
            const float k1Fast = 3200.0f, k2Fast = 113.0f; // 眼/嘴：更快
            const float d = (dt > 0.05f) ? 0.05f : dt;
            for (int i = 0; i < 9; ++i) {
                const bool isAng = (i <= 2);
                const float k1 = isAng ? k1Ang : k1Fast;
                const float k2 = isAng ? k2Ang : k2Fast;
                vv2[i] += ((iv[i] - sv2[i]) * k1 - vv2[i] * k2) * d;
                sv2[i] += vv2[i] * d;
            }
            if (gFace.iAx >= 0) model->SetParameterValue(gFace.iAx, sv2[0] * 0.9f, 1.0f);
            if (gFace.iAy >= 0) model->SetParameterValue(gFace.iAy, sv2[1] * 0.9f, 1.0f);
            if (gFace.iAz >= 0) model->SetParameterValue(gFace.iAz, sv2[2] * 0.5f, 1.0f);
            if (gFace.iEyeL >= 0) model->SetParameterValue(gFace.iEyeL, sv2[3], 1.0f);
            if (gFace.iEyeR >= 0) model->SetParameterValue(gFace.iEyeR, sv2[4], 1.0f);
            if (gFace.iMouth >= 0 && ctl::mouth(now) < 0.0f)
                model->SetParameterValue(gFace.iMouth, sv2[5], 1.0f);
            if (gFace.iEx >= 0) model->SetParameterValue(gFace.iEx, sv2[6], 1.0f);
            if (gFace.iEy >= 0) model->SetParameterValue(gFace.iEy, sv2[7], 1.0f);
            if (gFace.iSmile >= 0) model->SetParameterValue(gFace.iSmile, sv2[8], 1.0f);
        }
    }

    // 26：身体 —— **直写没用**，查 physics3 才知道：
    //   PhysicsSetting4: 输入 PositionX → 输出 ParamBodyAngleX
    //   PhysicsSetting9: 输入 PositionZ + PositionX → 输出 ParamBodyAngleZ
    // 也就是身体的「转」是从「整人平移」推出来的，没有独立的转向参数。
    // 他第一次说「身体也没有动」就是这个原因。要么驱动 PositionX（人整个挪），
    // 要么接受身体不转 —— 先不动，等他想清楚要哪种。
    (void)0;

    // 26：表情 —— 嘴角。**眉毛那块撤了**（2026-11-12 00:2x）。
    // 原因：查 physics3 才发现眉毛 Y 是耳物理链的控制量——
    //   PhysicsSetting82: ParamBrowRY (权重 100) → LControl
    //   PhysicsSetting84: ParamBrowLY (权重 100) → RControl
    // 我原来写 `-sbrow * 18`（负值），LControl 直接被打到 -1，耳链整个被压死，
    // 兽耳平贴在脸两侧。他说「耳朵根本没修」指的就是这个 —— 是我加的表情映射弄塌的。
    if (gFace.iSmile >= 0) model->SetParameterValue(gFace.iSmile, ssmile, 1.0f);

    static double lastLog = -1e9;
    if (now - lastLog > 5.0) {
        lastLog = now;
        // 打「我写进去多少」和「模型里现在是多少」——两者不一致就说明有人在我后面覆盖
        Csm::CubismIdManager* im2 = Csm::CubismFramework::GetIdManager();
        auto gv = [&](const char* n) {
            const Csm::csmInt32 i = model->GetParameterIndex(im2->GetId(n));
            return i >= 0 ? model->GetParameterValue(i) : -999.0f;
        };
        log_("[face] %s yaw=%+.1f pitch=%+.1f 眼 %.2f/%.2f（读回 AngleX=%.1f）"
             " | 耳 LControl=%.2f LEar1=%.1f LEar2=%.1f REar1=%.1f",
             gFace.live ? "跟上了" : "没脸（回默认）", sax, say, sEyeL, sEyeR,
             gFace.iAx >= 0 ? model->GetParameterValue(gFace.iAx) : -999.0f,
             gv("LControl"), gv("LEar1"), gv("LEar2"), gv("REar1"));
    }
}

static void walkTick(Csm::CubismModel* model, Csm::CubismMotionManager* mm, double now, float dt) {
    static double lastLog = -1e9;
    if (!ctl::gWalkWanted) return;

    int wx = 0, wy = 0, ww = 0, wh = 0;
    platGetPosition(&wx, &wy, &ww, &wh);
    const float dx = ctl::gWalkTargetX - static_cast<float>(wx);
    const float dy = ctl::gWalkTargetY - static_cast<float>(wy);
    const float dist = std::sqrt(dx * dx + dy * dy);

    if (dist < 2.0f) {
        ctl::gWalkWanted = false;
        log_("[walk] 到了 (%.0f, %.0f)", ctl::gWalkTargetX, ctl::gWalkTargetY);
        return;
    }

    const float step = std::min(dist, kWalkSpeed * dt);
    platSetPosition(wx + static_cast<int>(std::lround(dx / dist * step)),
                    wy + static_cast<int>(std::lround(dy / dist * step)));
    // 每 0.25 秒把 window.json 同步一次（第二支指针按它算局部坐标；不写就会指着旧位置）
    static double lastSync = -1e9;
    if (now - lastSync > 0.25) { lastSync = now; platSyncRect(); }

    // 2026-11-11 01:2x：他一句「这不就是平移吗，脚和腿根本没动」把我打回来了。
    // 他说得对 —— 这具模型**没有迈步的骨头**：腿只有一条对称的「张开」链
    // （LEGAngleZ2），两条腿绑在一起，做不出「左脚迈出去、右脚跟上」。
    // 硬把它当走路，就是「平移 + 腿一张一合」，比不动更假。
    //
    // 所以改成一个诚实的东西：**飘**。她有尾巴、裙摆、长发，飘着走不违和，
    // 而且这三样都真的能动：
    //   · 身体前倾一点（ParamAngleY 往下压 = 上半身前倾）
    //   · 尾巴跟着步伐甩（PokeTail 最灵敏，实测 16000px）
    //   · 裙摆被带着飘（PokeSkirt）
    //   · 上下起伏比走路更缓、幅度更小（飘不是踩）
    // 说白了：承认她不会走，但让她「移过去」这件事本身好看。
    static double walkT0 = -1.0;
    if (walkT0 < 0.0) walkT0 = now;
    const double ph = (now - walkT0) * 2.0 * 3.14159265358979 * 1.1;   // 1.1 次/秒
    const float bob   = std::sin(ph * 2.0f) * 3.4f;      // 上下：飘的浮沉
    const float sway  = std::sin(ph) * 4.2f;             // 左右摆
    const float tailS = std::sin(ph - 0.6f) * 0.55f;     // 尾巴晚半拍跟着甩
    const float skirt = std::fabs(std::sin(ph)) * 0.35f; // 裙摆被带走
    const float lean  = 5.0f + std::sin(ph) * 1.2f;      // 前倾（常量 + 微动）
    const float faceX = (dx > 0 ? 1.0f : -1.0f) * std::min(1.0f, std::fabs(dx) / 60.0f);

    // 姿态让位：有 motion/plan 在写身子时，这边只负责「位置在动」，不去抢参数
    if (postureBusy(mm)) {
        static double lastBusyLog = -1e9;
        if (now - lastBusyLog > 2.0) {
            lastBusyLog = now;
            log_("[walk] 有动作在播 —— 只移动，不写姿态");
        }
        return;
    }
    auto setP = [&](const char* name, float v) {
        const Csm::csmInt32 i = model->GetParameterIndex(
            Csm::CubismFramework::GetIdManager()->GetId(name));
        if (i >= 0) model->SetParameterValue(i, v, 1.0f);
    };
    setP("PositionY", bob);
    setP("ParamAngleZ", sway);
    setP("ParamAngleX", faceX * 7.0f);
    setP("ParamAngleY", -lean);
    setP("PokeTail", tailS);
    setP("PokeSkirt", skirt);

    if (now - lastLog > 0.8) {
        lastLog = now;
        log_("[walk] 飘向 (%.0f,%.0f) 剩 %.0f px｜浮沉 %+.1f 摆 %+.1f 尾 %+.2f 裙 %+.2f",
             ctl::gWalkTargetX, ctl::gWalkTargetY, dist, bob, sway, tailS, skirt);
    }
}

// 她的状态 → 身体上的几个连续量（都是 0.0–1.0 之间的「程度」）
static float herTired() { return gHer.valid ? std::min(1.0f, std::max(0.0f, gHer.tired / 100.0f)) : 0.35f; }
static float herHappy() { return gHer.valid ? std::min(1.0f, std::max(0.0f, gHer.happy / 100.0f)) : 0.20f; }
static float herSad()   { return gHer.valid ? std::min(1.0f, std::max(0.0f, gHer.sad   / 100.0f)) : 0.10f; }

// 动作幅度的倍率：累的时候被碰也弹不起来，开心的时候反应更大
static float herReactionScale() {
    if (!gHer.valid) return 1.0f;
    return std::max(0.45f, std::min(1.45f, 1.0f - 0.55f * herTired() + 0.45f * herHappy()));
}

// ── 二十六：她知道外面在发生什么 ──────────────────────────────
//
// 2026-11-11 01:2x：他说「二三同时做」，二 = 看得见你在干什么，三 = 记忆接进来。
// 数据由 tools/world-sync.py 每 40 秒写一次 state/world.json（只读，不碰任何东西）：
//   focus  你当前在用什么窗口
//   clock  几点、什么时段
//   today  今天日记的第一句 + 账本最后一条
// 身体这边只做两件事：**知道**（打日志、供别处判断），**据此调整自己**。
struct World {
    std::string app, title, period, diary, ledger;
    int hour = -1;
    bool valid = false;
};
static World gWorld;

static std::string jstr(const std::string& t, const char* key) {
    // 宽松解析：找 "key" 之后跳过空白和冒号，再跳过一个引号，取到下一个引号为止。
    // 之前写死 `"key":"` 匹配不上 —— python 的 json.dump 默认是 `"key": "value"`（冒号后有空格），
    // 结果就是全空。这是我这行代码的错，不是文件没写。
    const std::string k = std::string("\"") + key + "\"";
    const size_t p = t.find(k);
    if (p == std::string::npos) return "";
    size_t i = p + k.size();
    while (i < t.size() && (t[i] == ' ' || t[i] == ':' || t[i] == '\t')) ++i;
    if (i >= t.size() || t[i] != '"') return "";
    ++i;
    std::string out;
    while (i < t.size() && t[i] != '"') {
        if (t[i] == '\\' && i + 1 < t.size()) ++i;
        out += t[i++];
    }
    return out;
}

static void worldTick(double now) {
    static double last = -1e9;
    if (now - last < 40.0) return;
    last = now;
    ::system("python3 /home/link/.dsh/miyu/body/native3/tools/world-sync.py >/dev/null 2>&1 &");
    std::ifstream f("state/world.json");
    if (!f) return;
    std::string t((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (t.empty()) return;
    gWorld.app    = jstr(t, "app");
    gWorld.title  = jstr(t, "title");
    gWorld.period = jstr(t, "period");
    gWorld.diary  = jstr(t, "diary");
    gWorld.ledger = jstr(t, "ledger");
    {
        const size_t p = t.find("\"hour\":");
        if (p != std::string::npos) gWorld.hour = std::atoi(t.c_str() + p + 7);
    }
    gWorld.valid = true;
    // 2026-11-11 01:3x：主循环的 look/blink 日志太多，journald 会限速丢行 ——
    // 这条一直被丢，我在 journalctl 里怎么都看不到，白排查半小时。
    // 所以这一行**直接写文件**，不经过 journald。
    {
        FILE* wf = std::fopen("state/world-read.log", "a");
        if (wf) {
            std::fprintf(wf, "%s %d点 | 你在 %s | 日记: %s\n",
                         gWorld.period.c_str(), gWorld.hour, gWorld.app.c_str(),
                         gWorld.diary.empty() ? "(没有)" : gWorld.diary.substr(0, 40).c_str());
            std::fclose(wf);
        }
    }
}

// ── 二十六：她自己会动 ────────────────────────────────────────────
//
// 2026-11-11 01:2x 他说「都是米鱼，去试试解决」。缺的第一条是：
// **现在她动一下都要人推**，没人碰就只剩呼吸，那是摆设不是人。
//
// 这一段给她一个自己的时序：不靠外部定时器、不靠点击，按她**此刻的状态**
// 决定什么时候动、动什么。
//   · 累（tired 高）→ 很久才动一次，动的幅度也小，多半只是换个重心
//   · 精神（happy 高）→ 动得勤，会换表情、会看过来
//   · 正在播动作/计划/被人拖着 → 一律让位，别抢
// 动作只用「微动」级别：换重心、偏头、扫一眼。不做大事 —— 她不是在表演。
struct Autonomy {
    double nextAt = -1.0;      // 下一次想动的时间
    double lastAt = -1.0;
    int    kind = 0;           // 上一次做了什么（避免连着重复）
};
static Autonomy gAuto;

static void autonomyTick(Csm::CubismModel* model, Csm::CubismMotionManager* mm, double now) {
    if (gAuto.nextAt < 0.0) {          // 第一次：定一个不太近的点
        gAuto.nextAt = now + 45.0;
        return;
    }
    if (now < gAuto.nextAt) return;
    // 有动作在播 / 被人拖着 / 正在被戳 —— 都让位，别跟人抢
    if (postureBusy(mm) || platIsDragging()) return;
    if (gPartPoke.on) return;

    // 累了就隔久一点，精神就勤一点
    const float ty = herTired(), hp = herHappy();
    double gap = 420.0 - 260.0 * hp + 300.0 * ty;   // 4.4 分钟（精神）~ 12 分钟（累）
    // 外面是深夜（0-4 点）就再安静一点 —— 她知道现在几点，这属于「知道外面的事」
    if (gWorld.valid && (gWorld.hour >= 0 && gWorld.hour < 5)) gap *= 1.8;
    if (gap < 120.0) gap = 120.0;
    if (gap > 900.0) gap = 900.0;
    gAuto.nextAt = now + gap;
    gAuto.lastAt = now;

    // 挑一件小事做。累的时候只剩「换重心」，精神时才换表情/说话
    const int pick = static_cast<int>((gAuto.kind + 1 + (static_cast<int>(now) % 3)) % 4);
    gAuto.kind = pick;
    auto setP = [&](const char* name, float v) {
        const Csm::csmInt32 i = model->GetParameterIndex(
            Csm::CubismFramework::GetIdManager()->GetId(name));
        if (i >= 0) model->SetParameterValue(i, v, 0.6f);
    };
    const char* what = "";
    switch (pick) {
        case 0:   // 换个重心
            setP("ParamAngleZ", (ty > 0.6f ? 2.0f : 4.0f) * ((static_cast<int>(now) % 2) ? 1.0f : -1.0f));
            what = "换个重心";
            break;
        case 1:   // 偏一下头
            setP("ParamAngleX", (static_cast<int>(now) % 2 ? 6.0f : -6.0f));
            setP("ParamAngleY", 3.0f);
            what = "偏了下头";
            break;
        case 2:   // 换个表情（只在不太累时）
            if (ty < 0.7f) {
                const char* pool[] = {"＞＜", "圈圈眼", "爱心眼", "屑屑眼"};
                exprPlay(pool[static_cast<int>(now) % 4]);
                what = "换了个表情";
            } else {
                setP("ParamAngleZ", -2.0f);
                what = "换了个重心（累，不想换表情）";
            }
            break;
        default:  // 看过来一眼
            setP("ParamAngleX", 0.0f);
            setP("ParamAngleY", -2.0f);
            what = "抬头看了一眼";
            break;
    }
    log_("[auto] %s（累 %.0f / 开心 %.0f，下次 %.0f 秒后）", what, gHer.tired, gHer.happy, gap);
}



// 二十六：连击时放大冲量（partPokeTick 起手时设，partPokeStart 用）
static float gPokeBoost = 1.0f;

static void partPokeStart(Csm::CubismModel* model, const std::string& part, double now) {
    // 二十二：这里拿到的已经是**根输入**（PositionX / ParamBreath / AngleX…），
    // 不是 ChestX1/SkirtY3 那些物理算出来的输出。拨根 → 物理自己把这块带起来。
    const std::vector<partpoke::PartParam> ps = partpoke::top(part, 4);
    gPartPoke.n = 0;
    std::string names;
    // 每点一次换个方向（+1 / −1 交替）：连点同一个地方不会老是往一边塌。
    // 二十四：嘴和眉毛例外，固定往正方向 —— 嘴只能往上开（ParamMouthOpenY 是 0..1，
    // 往下压等于闭着，点了没反应）；眉毛那几格只有十几像素高，往下甩会甩进眼睛的
    // 格子（实测 79px 落眼睛 / 62px 落眉毛），抬起来才是它自己的动作。
    // Step 4：这个「例外」从 `part == "mouth" || part == "brow"` 的现场判断，改成
    // 读 **param 行自己的 up 标记**（state/parts.tsv 里 PokeMouth / PokeBrow 带 up）。
    // 好处是一个部位有多个根、方向要求不同时，每个参数各算各的，不再共用同一个 dir。
    for (const partpoke::PartParam& p : ps) {
        const float dir = p.up ? 1.0f : ((partpoke::gGen & 1) ? -1.0f : 1.0f);
        const Csm::csmInt32 idx = model->GetParameterIndex(
            Csm::CubismFramework::GetIdManager()->GetId(p.name.c_str()));
        if (idx < 0) continue;
        PartPokeRig::Ch& c = gPartPoke.ch[gPartPoke.n++];
        c.idx  = idx;
        // base 取**模型默认值**，不是"当前值"：AngleX/ParamAngleX/PositionX/ParamEyeBall
        // 这几个不吃 LoadParameters 的归零（NOTES 16.2），当前值很可能是上一次弹完
        // 留下的残值 —— 拿它当基准，弹一次漂一点，几十次点击之后她就歪在那儿了
        // （实测：连点 40 多次以后，静止帧和基准帧差 4.8 万像素）。
        c.base = model->GetParameterDefaultValue(idx);
        // 2026-11-10 02:1x：**必须按参数真实范围 clamp**。表和曲线给的是「摆幅的百分之几」，
        // 遇到范围窄的参数就会冲出去——比如 param53 只有 0..1、默认 0，peak 0.8×全幅 = 1.6
        // 直接越界，网格被拉翻，画面上就是点一下胸口黑一下。
        {
            static std::map<std::string, std::pair<float, float>> rng;
            static bool rngLoaded = false;
            if (!rngLoaded) {
                rngLoaded = true;
                std::ifstream rf("state/param-range.tsv");
                std::string line;
                while (std::getline(rf, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    // 格式：序号 <TAB> 名字 <TAB> min <TAB> max <TAB> 默认
                    std::vector<std::string> f;
                    size_t a = 0;
                    for (;;) {
                        const size_t b = line.find('\t', a);
                        f.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
                        if (b == std::string::npos) break;
                        a = b + 1;
                    }
                    if (f.size() >= 4) rng[f[1]] = { std::stof(f[2]), std::stof(f[3]) };
                }
                log_("[partpoke] 读到 %zu 个参数的范围", rng.size());
            }
            const auto it = rng.find(p.name);
            float lo = -1e9f, hi = 1e9f;
            if (it != rng.end()) { lo = it->second.first; hi = it->second.second; }
            // p.amp 是**偏移量**（表和探针量出来的「看得见但不变形」），不是目标值。
            // 顶到参数真实范围就缩回来 —— 越界会把网格拉翻（NOTES 二十一踩过）。
            float amp = p.amp * dir;
            if (c.base + amp > hi) amp = hi - c.base;
            if (c.base + amp < lo) amp = lo - c.base;
            // 单极参数（ParamBreath 是 0..1、默认 0）负方向一压就变成 0 —— 点了没反应。
            // 撞到边界就把方向翻过来，别把冲量吃掉。
            if (std::fabs(amp) < std::fabs(p.amp) * 0.5f) {
                amp = -p.amp * dir;
                if (c.base + amp > hi) amp = hi - c.base;
                if (c.base + amp < lo) amp = lo - c.base;
            }
            // 二十六：连击时把峰值放大（gPokeBoost 由 partPokeTick 在起手时设）
            amp *= gPokeBoost;
            if (c.base + amp > hi) amp = hi - c.base;
            if (c.base + amp < lo) amp = lo - c.base;
            c.peak = amp;
            c.dip = -amp * 0.35f;
        }
        // 冲量：欠阻尼弹簧从 0 起步，峰值 ≈ v/ω（ω = √150 ≈ 12.25），
        // 所以直接把初速度给成 amp×ω 就能弹到 amp 那么大，不用另写曲线。
        c.x = 0.0f;
        // PokeChest 本身是 -1..1，冲量再大也是撞界，所以「幅度」交给弹簧刚度去表达（见 tick）
        c.v = c.peak * 12.25f;
        char buf[128];
        std::snprintf(buf, sizeof buf, "%s(%+.2f) ", p.name.c_str(), c.peak);
        names += buf;
    }
    if (part == "eye") {
        // 二十四：眼睛这一下 = 瞳孔被物理带着摆（上面那条 PokeEye 链）+ 真的眨一下。
        auto idOf = [](const char* nm) {
            return Csm::CubismFramework::GetIdManager()->GetId(nm);
        };
        if (gPartPoke.eyeL < 0) gPartPoke.eyeL = model->GetParameterIndex(idOf("ParamEyeLOpen"));
        if (gPartPoke.eyeR < 0) gPartPoke.eyeR = model->GetParameterIndex(idOf("ParamEyeROpen"));
        gPartPoke.eyeOn = (gPartPoke.eyeL >= 0 || gPartPoke.eyeR >= 0);
        gPartPoke.eyeT0 = now;
        log_("[part] 眼睛：眨一下（%.0fms 包络）", 180.0);
    }
    if (gPartPoke.n == 0) {
        // 表里这个部位一个活参数都没有 —— 别装作动了，交回整套反应
        log_("[part] 部位 %s 没有可用参数（表是空的？），这次不播", part.c_str());
        return;
    }
    gPartPoke.on   = true;
    gPartPoke.t0   = now;
    gPartPoke.part = part;
    log_("[part] 点中部位 %s -> %d 个根输入：%s", part.c_str(), gPartPoke.n, names.c_str());
}

// 二十四：点眼睛那一下的眨眼包络（0.18s 三角形：闭到 0 再睁回 1）。
// 单独一个函数、在 partPokeTick 最前面调 —— 因为它和弹簧那套没有关系：
// 弹簧晃完 `gPartPoke.on` 一关，partPokeTick 就提前 return 了，包络会断在半路。
static void partPokeEyeTick(Csm::CubismModel* model, double now) {
    if (!gPartPoke.eyeOn) return;
    const double t = (now - gPartPoke.eyeT0) / 0.18;
    if (t >= 1.0) {
        gPartPoke.eyeOn = false;
        // 睁回去：ParamEyeLOpen/ROpen 的模型默认值就是 1（睁眼）
        if (gPartPoke.eyeL >= 0) model->SetParameterValue(gPartPoke.eyeL, 1.0f, 1.0f);
        if (gPartPoke.eyeR >= 0) model->SetParameterValue(gPartPoke.eyeR, 1.0f, 1.0f);
        log_("[part] 眼睛眨完，睁回 1.000");
    } else {
        const float v = static_cast<float>(std::fabs(1.0 - 2.0 * t));
        if (gPartPoke.eyeL >= 0) model->SetParameterValue(gPartPoke.eyeL, v, 1.0f);
        if (gPartPoke.eyeR >= 0) model->SetParameterValue(gPartPoke.eyeR, v, 1.0f);
    }
}

// 二十四：嘴那一下的幅度（0..1）。PokeMouth 是 -1..1 的弹簧位移，取绝对值再乘 0.85 ——
// 满量程时嘴巴张到八成，看得见，也不至于把嘴拉破。
static float mouthPokeAmount(Csm::CubismModel* model) {
    static Csm::csmInt32 idx = -2;
    if (idx == -2) {
        idx = model->GetParameterIndex(
            Csm::CubismFramework::GetIdManager()->GetId("PokeMouth"));
        if (idx < 0) log_("[part] 模型里没有 PokeMouth（嘴这一块不会动）");
    }
    if (idx < 0) return 0.0f;
    const float v = model->GetParameterValue(idx);
    const float a = (v < 0.0f ? -v : v) * 0.85f;
    return a > 1.0f ? 1.0f : a;
}

// 二十五：点部位出表情（他在 2026-11-10 20:1x 改的口径）。
// 原先点头会让她的头转一下 + 头发甩一下，他的反应是「很奇怪」——
// 头本来就在那儿，被点一下不该整块动，该是**脸色变了**。
// 于是这八个部位改成「点一下换一个表情，2 秒后淡回默认」，只用情绪系表情，
// 功能型的（裙子切换/脱鞋/尾巴开关/竖食指/眼镜/体型）一个都不碰。
// 头是「被摸头」，所以给随机；其余每个部位固定一种，互不重复。
// Step 5：表情从 **partapi** 读（parts.tsv 的 expr 段 + 网页覆盖层），
// 不再读 `web::settings().partExpr` —— 那份只是设置页的镜像（part-react.tsv 已退休）。
// 斜杠 `a/b/c` = 每次随机抽一个的语义留在 partapi::pickExpr 里，这里只拿结果。
// hold 也一起从表里来（expr 行没写就用 part 行的 hold，默认 5.0，和原来写死的 5 一致）。
static double gTapExprHold = 5.0;

static const char* tapExprFor(const std::string& part, unsigned seed) {
    static std::string pick;
    double hold = 5.0;
    if (!partapi::pickExpr(partapi::indexOf(part), seed, &pick, &hold)) return nullptr;
    gTapExprHold = hold;
    return pick.empty() ? nullptr : pick.c_str();
}

static double gTapExprUntil = 0.0;   // 表情留到什么时候
static bool   gTapExprOn = false;

static void partPokeTick(Csm::CubismModel* model, double now) {
    // ① 平台层点了新的部位
    if (partpoke::gGen != gPartPoke.lastGen) {
        gPartPoke.lastGen = partpoke::gGen;
        const std::string part(partpoke::gPart);
        if (!part.empty() && web::settings().tapPart) {
            static unsigned gTapSeed = 0;
            // 二十六：连击。连着点同一个部位，她会有脾气 ——
            //   第 3 下起：动作幅度 ×1.6（越点越明显）
            //   第 5 下起：脸强制换成「黑脸」（不管这个部位原来配了什么）
            //   松手隔 1.5 秒算断，从头数。
            // 为什么只到「黑脸」不到「躲开」：躲开要改窗口位置，那会跟拖动打架。
            static int    gComboPart = 0, gCombo = 0;
            static double gComboLast = -1e9;
            const int curId = partapi::indexOf(part);
            if (!web::settings().combo) gCombo = 0;     // 开关关着：不计数，也不影响表情
            else if (curId == gComboPart && now - gComboLast < 1.5) ++gCombo;
            else gCombo = 1;
            if (web::settings().combo) gComboPart = curId;
            gComboLast = now;
            if (gCombo >= 3) log_("[combo] %s 连点第 %d 下", part.c_str(), gCombo);

            // ① 表情（有就播；没有就跳过，不影响下面）
            const char* ex = (gCombo >= 5) ? "黑脸" : tapExprFor(part, gTapSeed++);
            if (ex && web::settings().expr) {
                exprPlay(ex);
                // 2026-11-10 23:2x：exprPlay 内部会按「全局基准 × 设置里的倍率」设 gExprUntil
                // （现在是 2.5 秒），把我们在表里配的每条时长盖掉 —— 实测点发饰本该 6 秒，
                // 结果 2.5 秒就淡回去了。所以这里调完 exprPlay 再按表里的 hold 覆盖一次。
                gExprUntil = now + gTapExprHold;
                gTapExprUntil = now + gTapExprHold;
                gTapExprOn = true;
                log_("[tap] 点 %s -> 表情「%s」", part.c_str(), ex);
            }
            // 连击加剧：把这一下的冲量放大（partPokeStart 里按 gPokeBoost 乘）
            gPokeBoost = ((gCombo >= 3) ? 1.6f : 1.0f) * herReactionScale();
            // 二十六：双击 —— 同一个地方连着戳两下，她的反应不一样：
            // 台词换成「啊」这种被打断的短音，冲量再放大 1.4 倍。
            const bool dbl = partpoke::gDoubleTap && web::settings().dblTap;
            if (dbl) {
                gPokeBoost *= 1.4f;
                log_("[tap] %s 双击", part.c_str());
            }
            // 二十六：让身体自己说一句（parts.tsv 的 part 行第 5 列）。
            // 和外部推 {"say":...} 同一套：嘴型包络走 ctl，有缓存就出声。
            // 连点第 5 下（黑脸那下）改成「够了吧」，这比部位台词更像有脾气。
            if (const char* line = partapi::sayOf(curId)) {
                if (*line) {
                    std::string s2 = (gCombo >= 5) ? std::string("够了吧") : std::string(line);
                    if (dbl && gCombo < 5) s2 = "啊";
                    ctl::say(s2, now);
                    web::setLastSay(s2);
                }
            }
            // ② 这个部位自己的动作（有根参数就晃）—— **不是二选一**，两个一起播
            partPokeStart(model, part, now);
        }
    }
    // 表情到点回默认
    if (gTapExprOn && now > gTapExprUntil) {
        gTapExprOn = false;
        if (web::settings().expr) exprPlay("默认");
        log_("[tap] 表情回默认");
    }
    partPokeEyeTick(model, now);
    if (!gPartPoke.on) return;

    // 弹簧-阻尼积分：a = -k*x - c*v，欠阻尼 → 冲出去、回来时过冲、震两下停。
    // k=150 / c=9 → ω≈12.2 rad/s、ζ≈0.37，周期约 0.5s，看得见两下回弹。
    // 2026-11-10 13:4x：他说「晃起来怪怪的」。追下去发现是**两个振荡器叠在一起**——
    // 我这个弹簧（ζ0.14，会晃五六下）驱动的是 PokeChest，而它下游的 ChestX 自己还有一套
    // 物理（Mobility 0.85-0.93，更慢）。两套频率不同的振动串起来，看着就是"怪"。
    // 修法：弹簧只负责**冲一下、平滑回位**，振动交给模型自己的物理。
    // ζ 0.14 → 0.68（C=11.2），基本不过冲，一个动作就是一次位移。
    // 2026-11-10 13:4x：胸晃档位改成**调弹簧刚度**。原因是 PokeChest 值域只有 -1..1，
    // 冲量给多大都撞界，实测 0/0.6/1.2/2.0 四档位移一模一样（5662/5469/4878/5153）。
    // 刚度不同 → 冲出去的速度、回弹的快慢都不同，这才看得出档位。
    float swayK = 1.0f;
    // 2026-11-10 13:5x：他说「0 是最大档、2 是最小档」，方向反了——
    // K 越大弹簧越硬、同样冲量下位移越小。所以档位越大 K 越小（越软、晃得越大）。
    if (gPartPoke.part == "leg") {
        const float ls = web::settings().legSway;
        const float lo = ls < 2.0f ? ls : 2.0f;
        const float hi = ls > 2.0f ? (ls - 2.0f) : 0.0f;
        swayK = (1.75f - 0.75f * lo) - 0.15f * hi;
    }
    if (gPartPoke.part == "chest") {
        // 0→2 段：K 从 1.75 平滑降到 0.25（原来那段手感不动）；2→3 段继续往软走，接到 0.10。
        const float cs = web::settings().chestSway;
        const float lo = cs < 2.0f ? cs : 2.0f;
        const float hi = cs > 2.0f ? (cs - 2.0f) : 0.0f;
        swayK = (1.75f - 0.75f * lo) - 0.15f * hi;
    }
    const float K = 68.0f * swayK, C = 11.2f * swayK;
    static double lastSwayLog = -1e9;
    if (gPartPoke.part == "chest" && now - lastSwayLog > 0.5) {
        lastSwayLog = now;
        log_("[part] chest 档位=%.2f -> swayK=%.2f K=%.1f C=%.1f", web::settings().chestSway, swayK, K, C);
    }
    const float h = 1.0f / 240.0f;
    // 2026-11-10 02:5x：子步数必须**按真实帧间隔**算。她跑 200Hz（5ms 一帧），
    // 固定走 6 个子步等于把时间加速 6 倍，弹簧 0.5s 的震荡挤进 0.08s——看着就是秒停。
    float dt = static_cast<float>(now - gPartPoke.lastT);
    gPartPoke.lastT = now;
    if (dt < 0.0f || dt > 0.1f) dt = 0.005f;
    int subs = static_cast<int>(dt / h + 0.5f);
    if (subs < 1) subs = 1;
    if (subs > 16) subs = 16;
    bool alive = false;
    for (int i = 0; i < gPartPoke.n; ++i) {
        PartPokeRig::Ch& c = gPartPoke.ch[i];
        for (int sub = 0; sub < subs; ++sub) {
            const float a = -K * c.x - C * c.v;
            c.v += a * h;
            c.x += c.v * h;
        }
        if (std::fabs(c.x) > 0.002f || std::fabs(c.v) > 0.05f) alive = true;
        model->SetParameterValue(c.idx, c.base + c.x, 1.0f);
    }
    if (!alive) {
        for (int i = 0; i < gPartPoke.n; ++i) {
            model->SetParameterValue(gPartPoke.ch[i].idx, gPartPoke.ch[i].base, 1.0f);
            gPartPoke.ch[i].x = gPartPoke.ch[i].v = 0.0f;
        }
        gPartPoke.on = false;
        log_("[part] %s 晃完，参数回默认", gPartPoke.part.c_str());
    }
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
    // 二十：拖动体感三件一起推给平台层（`grab` 的判定在 main，但平台也要知道，
    // 好让日志和「关掉时立刻收」这件事在同一个地方说完）；timeAware 每帧读，不用动作。
    else if (!strcmp(key, "grab") || !strcmp(key, "inertia") || !strcmp(key, "edgeSoft"))
        platSetDragFeel(web::settings().grab, web::settings().inertia,
                        web::settings().edgeSoft);
    else if (!strcmp(key, "dragSpring")) platSetDragSpring(web::settings().dragSpring);
    log_("[web] 生效：%s", key);
}

// 二十五：设置页上的「动作按钮」。
// 和 webApply 分开：改设置要落盘，动作不要。四个命令：
//   part  试一下某个部位（走和真点击同一条 partPokeStart）
//   expr  切一个表情（2 秒后回默认，和点部位同一套）
//   plan  播一份动作计划（plans/*.json）
//   say   说一句话（含嘴型包络，不出声 —— 出声由 voice 设置那套管）
static void webCmd(const char* cmd, const char* arg) {
    const std::string c = cmd ? cmd : "", a = arg ? arg : "";
    if (c == "part") {
        if (!a.empty()) {
            std::snprintf(partpoke::gPart, sizeof partpoke::gPart, "%s", a.c_str());
            ++partpoke::gGen;
            log_("[web] 试一下部位：%s", a.c_str());
        }
    } else if (c == "expr") {
        if (web::settings().expr) {
            exprPlay(a.empty() ? "默认" : a);
            gTapExprUntil = platTime() + 5.0;
            gTapExprOn = true;
        }
    } else if (c == "plan") {
        if (!a.empty()) {
            const std::string p = std::string(kModelDir) + "../plans/" + a + ".json";
            std::vector<char> raw = slurp(p);
            if (raw.empty()) {
                log_("[web] 找不到计划：%s", p.c_str());
            } else {
                const double now = platTime();
                plan::start(std::string(raw.begin(), raw.end()), now);
                if (plan::active()) planTakeover(now);
                log_("[web] 播计划：%s（%.1fs）", p.c_str(), plan::total());
            }
        }
    }
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

// 二十五 → Step 6：拖动跟随通用化。
//
// 原来这里只给**腿**写了三个常量（HipAngleX = ax*0.6 / HipAngleZ = az*2.2 /
// PokeLeg = ax/16），别处要跟随就得再写一份 C++。现在数据在 state/parts.tsv 的
// `follow` 行：`follow <部位> <参数> <系数> <上限> ax|az|ay`，加一处跟随不用改代码。
//
// 为什么腿需要这个：腿那条物理链（PhysicsSetting135）只吃 HipAngleX 和 PositionZ，
// 而拖动/滑行只写 AngleX / AngleZ / PositionX / PositionY —— 腿一个输入都没喂到，
// 他拎着她走的时候腿是死的，手和尾巴却在动。
//
// 三个量都从 swayTick 已经平滑/衰减好的结果里取（`gSway.ax` / `.az`，±16 / ±10 量级），
// 写**绝对值**：待机和视线都不碰它们，读回来加会一帧一帧累积。
//
// 让位规则**通用**：正在被点的那个部位，它自己的 follow 整段跳过 ——
// 点腿的弹簧在跑时不该被拖动压掉。原来是 `gPartPoke.part == "leg"` 认名字，现在按 id。
// 放在 partPokeTick **之后**调（顺序不能换）。
static void followTick(Csm::CubismModel* model, double now, float dt) {
    const auto& fs = partapi::reg().follows;
    if (fs.empty()) return;
    const int pokingId = gPartPoke.on ? partapi::indexOf(gPartPoke.part) : 0;

    static std::map<std::string, Csm::csmInt32> idxCache;   // 参数 id → 索引（查一次就够）
    // 2026-11-10 23:2x：他两次说「拖动时腿不够灵动」。
    // 原来这里是 `写 = 源 × 系数`，**跟上身完全同步** —— 上身摆多少腿立刻摆多少，
    // 看起来像焊在一起。真人被拎着走，腿是**追不上、会过冲、松手还在晃**的。
    // 改成每个参数自己挂一个弹簧去追目标：ω≈13 rad/s、ζ≈0.42，
    // 于是腿比上身晚半拍、甩过去会回来一点、停下再收住。
    struct Spr { float x = 0.0f, v = 0.0f; };
    static std::map<std::string, Spr> spr;
    std::string line;
    char buf[160];
    for (const partapi::Follow& f : fs) {
        if (pokingId > 0 && static_cast<int>(f.id) == pokingId) continue;   // 自己的弹簧活着 → 让位
        auto it = idxCache.find(f.name);
        if (it == idxCache.end()) {
            const Csm::csmInt32 idx = model->GetParameterIndex(
                Csm::CubismFramework::GetIdManager()->GetId(f.name.c_str()));
            it = idxCache.emplace(f.name, idx).first;
        }
        const float src = (f.src == 'z') ? gSway.az : (f.src == 'y') ? gSway.py : gSway.ax;
        float tgt = src * f.gain;
        if (tgt >  f.clampv) tgt =  f.clampv;
        if (tgt < -f.clampv) tgt = -f.clampv;

        Spr& sp = spr[f.name];
        // 每条 follow 可以带自己的刚度/阻尼；没写就用默认那组
        const float K = (f.K > 0.0f) ? f.K : 170.0f;
        const float C = (f.C > 0.0f) ? f.C : 10.5f;
        float h = dt;
        if (h > 0.02f) h = 0.02f;               // 卡帧别炸
        const int sub = 4;                      // 4 次子步，低帧率也稳
        const float hh = h / sub;
        for (int k = 0; k < sub; ++k) {
            const float a = -K * (sp.x - tgt) - C * sp.v;
            sp.v += a * hh;
            sp.x += sp.v * hh;
        }
        if (it->second >= 0) model->SetParameterValue(it->second, sp.x, 1.0f);
        std::snprintf(buf, sizeof buf, "%s=%+.2f ", f.name.c_str(), sp.x);
        line += buf;
    }

    static double lastLog = -1e9;
    if ((std::fabs(gSway.ax) > 0.02f || std::fabs(gSway.az) > 0.02f) && now - lastLog > 1.0) {
        lastLog = now;
        log_("[follow] 拖动跟随：%s（来源 ax=%+.2f az=%+.2f）", line.c_str(), gSway.ax, gSway.az);
    }
}


int main() {
    // ── 窗口（窗口层接口，实现在 platform_x11.cpp）────────────
    if (!platInit(500, 900, "native2-test")) { log_("[main] 窗口层起不来"); return 1; }
    ctl::voiceWarmUp();   // 二十六：先把声音输出流唤醒，否则第一声会被吞
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
    // 二十五：动作计划名单（设置页「动作」那一组用它出按钮）
    {
        std::vector<std::string> names;
        const std::string dir = std::string(kModelDir) + "../native3/plans/";
        if (DIR* d = opendir(dir.c_str())) {
            while (dirent* e = readdir(d)) {
                std::string f = e->d_name;
                if (f.size() > 5 && f.substr(f.size() - 5) == ".json")
                    names.push_back(f.substr(0, f.size() - 5));
            }
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        web::setPlanNames(names);
        log_("[web] 动作计划 %zu 个", names.size());
    }
    web::init(&webApply, &webCmd);
    {
        // MIYU_NO_TAP 是命令行上的一次性覆盖，压过设置文件的 tap（排查时它该说了算）
        if (getenv("MIYU_NO_TAP") && web::settings().tap) web::settings().tap = false;
        const web::Settings& s0 = web::settings();
        platSetTapEnabled(s0.tap);
        // 二十：拖动手感三项开机就要落一遍（settings.json 里存的是关的话要真关掉）
        platSetDragFeel(s0.grab, s0.inertia, s0.edgeSoft);
        platSetDragSpring(s0.dragSpring);
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
    bubble::init();   // 说话气泡：载字体、建 quad

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
    // 二十六：motionManager **总是**创建 —— 之前只在 MIYU_MOTION 指定待机动作时才 new，
    // 于是默认配置（手写正弦待机）下它一直是 nullptr，运行时想播「举手」根本没地方播。
    Csm::CubismMotionManager* motionManager = new Csm::CubismMotionManager();
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

    probeInit(model);          // 二十一：探针（没设 MIYU_PROBE 就什么都不做）

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
        // 二十：时段判定（内部只在 60 秒到点时才真的问钟，每帧调不花钱）。
        // 必须放在 idleTick / 眨眼之前 —— 它们这一帧就要读乘数。
        timeTick(now);
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
        // 二十之二：可选的整体倾斜（swayTilt，默认 0 = 关）。乘在 Scale 之后 =
        // 在 NDC 里绕画面中心转，画布怎么缩放都不影响角度。
        const float swayDeg = swayTiltDeg(now, dt);
        if (std::fabs(swayDeg) > 0.001f) {
            const float rad = swayDeg * 3.14159265f / 180.0f;
            const float cs = std::cos(rad), sn = std::sin(rad);
            Csm::CubismMatrix44 rot;
            Csm::csmFloat32 rm[16] = { cs, -sn, 0, 0,
                                             sn,  cs, 0, 0,
                                              0,   0, 1, 0,
                                              0,   0, 0, 1 };
            rot.SetMatrix(rm);
            proj.MultiplyByMatrix(&rot);
        }

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
            // 二十五：写**三帧**。原来只写一帧，而 exp3 里那些 Blend=Add 的参数
            // （屑屑眼的 key3/key5/key6 就是）在下一帧又被加起来，盖不住 —— 表现是
            // 点完那个表情、回默认之后她一直闭着眼，只有重启才好。
            static int resetFrames = 0;
            if (resetFrames == 0) resetFrames = 3;
            for (const auto& nm : gExprTouched) {
                const Csm::csmInt32 idx = model->GetParameterIndex(Csm::CubismFramework::GetIdManager()->GetId(nm.c_str()));
                if (idx >= 0) model->SetParameterValue(idx, model->GetParameterDefaultValue(idx), 1.0f);
            }
            if (--resetFrames <= 0) {
                resetFrames = 0;
                gExprResetPending = false;
                gExprTouched.clear();
            }
        }
        // 嘴：在表情之后写（表情要是不小心也写了 ParamMouthOpenY，说话优先），
        // 眨眼之前（眨眼只碰眼睛，顺序其实无所谓，就近放）。一律 Set —— 这个参数
        // 不会被 LoadParameters 归零，Add 会一帧一帧累加顶死，和 ParamEyeBall 一个毛病。
        // 计划要是也驱动嘴（白名单里有 ParamMouthOpenY），静默时别用 0.02 去盖它；
        // 正在说话则照旧 —— 说话优先于计划（和第十一节的规矩一致）。
        const bool planDrivesMouth = plan::active() && plan::drives("ParamMouthOpenY");
        if (mouthI >= 0) {
            const float mv = ctl::mouth(now);
            float mout = mv < 0.0f ? 0.02f : mv;      // 不说时只有一点呼吸
            // 二十四：点嘴。**这一块不能靠物理** —— 上面这句 Set 每帧无条件写
            // ParamMouthOpenY，物理的输出（physics 更早跑）当场被盖掉，实测两个
            // 方向都是 0 px。所以「嘴戳动」的落地就在这儿：读物理弹簧写好的
            // PokeMouth，取 |v| 叠上去（嘴只能往开的方向走，0..1 是单极的）。
            const float pk = mouthPokeAmount(model);
            if (pk > 0.0f && pk > mout) mout = pk;
            if (mv >= 0.0f || !planDrivesMouth)
                model->SetParameterValue(mouthI, mout, 1.0f);
        }
        // 视线：夹在待机微动之后（叠在它上面，不覆盖）、眨眼之前。
        // 用逻辑尺寸归一化 —— platPointer 给的是 surface-local 逻辑 px，不是物理 px。
        int lw = 0, lh = 0;
        platGetLogicalSize(&lw, &lh);
        lookTick(model, dt, now, lw, lh);
        // 二十之二：拖动 → 喂 physics，让各部位甩起来（必须在待机/视线之后，
        // 计划之前；它是在当前值上加，不覆盖待机）
        swayTick(model, dt, now);
        // 二十：抓起来那一下 + 深夜低头。位置就在这儿 —— 待机/视线已经写完，
        // 它俩是**在结果上再加一点**（读当前值再 Set），不会把待机抹掉；
        // 计划还在后面，所以「计划在播」时这两个小脉冲照样会被计划盖过去（对的）。
        feelTick(model, now);
        // 计划：最后一个写参数的（盖过待机/IDLE motion/视线），之后才是眨眼和 physics
        planTick(model, now);
        // 二十一：探针 / 部位级反应都放到 physics 之后（见下面那两行调用）
        // —— 它们要当这一帧**最后**一个写参数的人。
        // 计划要是也写了嘴，正在说话时把嘴抢回来说话那一边（顺序上计划在后面，所以补这一下）
        if (mouthI >= 0 && planDrivesMouth) {
            const float mv = ctl::mouth(now);
            if (mv >= 0.0f) model->SetParameterValue(mouthI, mv, 1.0f);
        }
        // 第十六节：表情到点回收（只有「计划在播」时跳过 —— 那时候由 planTick 收尾，
        // 计划附带的那个表情不该在中途被计时器拽回默认）。
        if (gExprUntil > -1e9 && now >= gExprUntil && !plan::active()) {
            // 2026-11-10 23:3x：这里原来打的是「全局基准 × 倍率」（2.5s），可点部位出表情时
            // 实际停留是按 parts.tsv 里那一行的 hold 算的（可能是 6s）。日志和事实对不上，
            // 我差点被它骗一次。改成打真正用掉的那段时间。
            log_("[expr] 停留 %.1fs 到点（这一条实际用的时长），回默认",
                 (gExprStart > -1e8) ? (now - gExprStart) : 0.0);
            gExprUntil = -1e9;
            exprPlay("默认");
        }
        if (eyeBlink && !probeMode()) {
            // 第十六节：眨眼频率倍率。SDK 的间隔是「基准 4s」，真实间隔 0~2×基准 随机，
            // 所以倍率是反着除：blink=2 → 基准 2s（0~4s 一次，明显更勤），blink=0.5 → 8s。
            // blink≈0 直接**不调 UpdateParameters**（让它自己眨会永远闭着），
            // 眼睛参数由 LoadParameters 保持默认（睁），日志打点照旧 —— 所以是「0 条闭眼」。
            static float lastBlinkK = -1.0f;
            // 二十：时段再乘一次（深夜 ×0.55 = 更少眨）。乘出来的值才是有效倍率，
            // 所以 lastBlinkK 比的就是乘完的 —— 跨时段时它会变，间隔跟着重设。
            const float bk = ws.blink * timeBlink();
            if (bk != lastBlinkK) {
                lastBlinkK = bk;
                if (bk > 0.001f) eyeBlink->SetBlinkingInterval(4.0f / bk);
                log_("[blink] 频率倍率 %.2f（设置 %.2f × 时段 %.2f %s）-> 间隔基准 %.2fs（0 = 不眨）",
                     bk, ws.blink, timeBlink(), gSeg.name, bk > 0.001f ? 4.0f / bk : 0.0f);
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
        // 二十六：摄像头模式。**必须在 physics 之前**（2026-11-12 01:0x 改过来的）：
        // 第一版放在 physics 之后，结果他问「头发这么没有重力」——因为头发是物理链，
        // 吃的输入是 ParamAngleX，physics 先算完、头后转，头发永远慢一帧、看起来没惯性。
        // 现在先把头写进去，physics 基于这一帧的新头姿算头发。
        faceTick(model, dt, now, ws.face);
        // 2026-11-12 00:2x：兽耳那段掐权重的改动**撤了**。他说「兽耳应该回到默认情况」——
        // 把 LControl/RControl 设 0 之后，耳链整个不跑了，耳朵僵在那里，比跟着甩更假。
        // 默认就是它跟着头一起动，那是美术在设计时就定好的。
        // 二十一：探针和「点哪动哪」必须排在 physics **后面**。
        // CubismPhysics 写输出参数时 weight=1.0 是**硬覆盖**（Framework/src/Physics/
        // CubismPhysics.cpp 里 `*parameterValue = value`），而模型里有一大半好看的
        // 参数（ParamHipAngleZ / HipAngleX2 / ParamAngleX…）正是 physics 的输出。
        // 放在 physics 之前写 = 同一个参数一帧里被写两遍、physics 赢 —— 点了没反应
        // （实测：点头部位时 ParamAngleX 写 18°，画面 0.13 px 都没动）。
        // 2026-11-12 00:4x：**这里少了一行 physics->Evaluate**。
        // 是我今天用 python 批量改代码时，那个删诊断日志的正则把这段一起吞掉了。
        // 后果是整套物理根本没跑：头发不甩（他说「头发这么没有重力」）、
        // 兽耳僵在脸两侧（他说「耳朵根本没修」）、身体转不动 —— 三个症状同一个根因。
        // 位置就在这里：faceTick 写完头（physics 的输入）→ physics 算 → 探针/点点在后面。
        if (physics && std::getenv("MIYU_NO_PHYS") == nullptr) physics->Evaluate(model, dt);
        probeTick(model, now);
        partPokeTick(model, now);
        herStateTick(now);
        worldTick(now);
        autonomyTick(model, motionManager, now);
        // 她的状态 → 姿态。连续量，不是开关：累了整个人往下沉、头低一点；
        // 开心的时候反过来。这是「身体反映她」最直接的一层。
        if (gHer.valid) {
            const float ty = herTired(), hp = herHappy(), sd = herSad();
            const float sink = (-6.0f * ty + 4.0f * hp);          // PositionY：累→沉，开心→提
            const float lean = (-4.0f * ty - 5.0f * sd + 3.0f * hp);
            const Csm::csmInt32 iPos = model->GetParameterIndex(
                Csm::CubismFramework::GetIdManager()->GetId("PositionY"));
            const Csm::csmInt32 iAng = model->GetParameterIndex(
                Csm::CubismFramework::GetIdManager()->GetId("ParamAngleX"));
            if (iPos >= 0) {
                const float cur = model->GetParameterValue(iPos);
                model->SetParameterValue(iPos, cur + sink * 0.35f, 0.35f);
            }
            if (iAng >= 0) {
                const float cur = model->GetParameterValue(iAng);
                model->SetParameterValue(iAng, cur + lean * 0.25f, 0.25f);
            }
            static double lastMoodLog = -1e9;
            if (now - lastMoodLog > 60.0) {
                lastMoodLog = now;
                log_("[her] 心情 %.0f（累 %.0f / 开心 %.0f / 难过 %.0f）→ 姿态下沉 %+.1f 前倾 %+.1f，反应倍率 %.2f",
                     gHer.mood, gHer.tired, gHer.happy, gHer.sad, sink, lean, herReactionScale());
            }
        }
        // 二十六：运行时播动作（举手 / 竖手指 / 狗尾草 / 脱捕）。
        // 之前只能 MIYU_MOTION 在启动时指定一个，跑起来就换不了 ——
        // 那些 motion 是「手的切换」，本来就是要随手播的。
        if (ctl::gMotionName[0] != '\0' && motionManager) {
            const std::string mn(ctl::gMotionName);
            ctl::gMotionName[0] = '\0';
            const std::string mp = std::string(kModelDir) + "Animation/" + mn;
            std::vector<char> mb = slurp(mp);
            if (mb.empty()) {
                log_("[motion] 读不到 %s", mp.c_str());
            } else {
                auto* mo = Csm::CubismMotion::Create(
                    reinterpret_cast<const Csm::csmByte*>(mb.data()),
                    static_cast<Csm::csmSizeInt>(mb.size()));
                if (mo) {
                    mo->SetFadeInTime(0.25f);
                    mo->SetFadeOutTime(0.25f);
                    motionManager->StartMotionPriority(mo, true /* autoDelete */, 3 /* 高优先级，压过待机 */);
                    log_("[motion] 播 %s（%.2fs）", mn.c_str(), mo->GetLoopDuration());
                }
            }
        }
        followTick(model, now, dt);
        // 走路放在 followTick **之后**：follow 里有一条 `follow leg PokeLeg`，
        // 它在拖动的每一帧都会把 PokeLeg 写掉 —— 走路写在这之前等于白写，
        // 他看到的「脚和腿根本没动」就是这个。
        walkTick(model, motionManager, now, dt);
        // 二十二：验收用 —— MIYU_POKE=chest 时，起来 2 秒后**真点一下那个部位**，
        // 然后连拍若干帧导出（窗口坐标、带 alpha）。这样「点一下这块动多少」是量的，
        // 不是嘴上说的；也不用 grim 截图（桌面会动、坐标系还差一个顶栏）。
        if (const char* pk = std::getenv("MIYU_POKE")) {
            static double tPoke = -1.0;
            static int    dumped = 0, sincePoke = -1;
            static bool   fired = false, preDumped = false;
            if (tPoke < 0.0) tPoke = now + 2.0;
            // 二十四：点**之前**先导一帧当基准。以前第一帧是点之后第 8 帧（约 88ms），
            // 那时弹簧已经走出去一截、嘴已经张开一半 —— 拿它当「点之前」量出来的倍数
            // 天然偏小（尤其是嘴这种变化本来就小的部位）。
            if (!preDumped && now >= tPoke - 0.12) {
                if (const char* dp = std::getenv("MIYU_POKE_DIR")) {
                    char pbuf[512];
                    std::snprintf(pbuf, sizeof pbuf, "%s/poke-pre.raw", dp);
                    gPendingDump = pbuf;
                }
                preDumped = true;
            }
            if (!fired && now >= tPoke) {
                const std::string part(pk);
                if (partpoke::hasParams(part)) {
                    std::snprintf(partpoke::gPart, sizeof partpoke::gPart, "%s", part.c_str());
                    ++partpoke::gGen;
                    log_("[poke-eval] 点 %s（gen=%d）", part.c_str(), partpoke::gGen);
                } else {
                    log_("[poke-eval] 部位 %s 没有可拨的根", part.c_str());
                }
                fired = true;
                sincePoke = 0;
            }
            if (fired && dumped < 40 && ++sincePoke % 8 == 0) {
                if (const char* dp = std::getenv("MIYU_POKE_DIR")) {
                    char pbuf[512];
                    std::snprintf(pbuf, sizeof pbuf, "%s/poke-%03d.raw", dp, dumped);
                    // 和探针一样：这一帧还在离屏 FBO 里，等合成回窗口再读
                    gPendingDump = pbuf;
                }
                ++dumped;
            }
        }
        const double tB = platTime();
        gDrawW = W;
        gDrawH = H;
        model->Update();
        // 二十二：部位格子图（模型自带 part 分区 → 40×80 格子，给平台层点的时候查）
        partZoneTick(model, k, W, H, now);
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
                // 二十一：探针模式不给粒子 —— 它们是随机运动的，会把两张图的差搅浑
                postfx::drawParticles(probeMode() ? 0.0f : ws.particles, dt, pcx, pcy, phw, phh);
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
        if (!gPendingDump.empty()) {          // 二十二：探针要的那一帧（窗口内容已合成好）
            dumpFrameRGBA(gPendingDump);
            gPendingDump.clear();
        }
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
                // 二十二：同一份 alpha 顺手把部位格子裁到她的剪影里
                partZoneClip(pix.data(), aw, ah, 32);
                if (W != lastRW || H != lastRH || justReleased || justShown)
                    log_("[main] 输入区重算（第 %lu 帧，framebuffer %dx%d%s，拖动期间跳过 %lu 帧）",
                         frameNo, W, H, justReleased ? "，刚松手" : (justShown ? "，刚取消隐藏" : ""),
                         dragFrames);
                if (justReleased) dragFrames = 0;
                lastRW = W;
                lastRH = H;
            }
        }

        // 二十六：说话气泡。她一开始说话就把这句交给气泡（只在换了新句时才重建纹理）
        {
            // 每帧都把「她现在说的这句」交给气泡，去重由 bubble 自己做 ——
            // 之前用 lastSaid 在外面判，结果某一次状态没对上就整段失效，
            // 而且失效时一点痕迹都没有（日志里什么都看不到）。
            const std::string& said = ctl::currentSay();
            if (!said.empty()) bubble::say(said.c_str(), now);
        }
        // 二十六：说话气泡。**在 swap 之前**画 —— 那时窗口 buffer 是最终的成品，
        // 画在她上面、也画在后处理之上（后处理只处理后两者，不碰这里）。
        bubble::setEnabled(ws.bubble);
        bubble::tick(now, dt);
        bubble::draw(W, H);

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
