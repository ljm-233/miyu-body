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
#include <string>
#include <poll.h>
#include <dlfcn.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>

#include "layer-shell-client.h"
#include "xdg-output-client.h"
#include "alpha_region.h"
#include "plan.h"   // 单击反应：平台层直接推一份计划给播放器（第十三节）

#define LOG(...) do { fprintf(stderr, "[plat-wl] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

/* ---------------- 状态 ---------------- */
struct OutInfo {
    wl_output* out = nullptr;
    uint32_t   name = 0;
    char       label[64] = {0};    // wl_output.name（niri：eDP-1 / HDMI-A-3），拿不到退 model
    char       make[32] = {0};
    char       model[64] = {0};
    char       desc[96] = {0};
    int        scale = 1;
    int32_t    x = 0, y = 0;       // wl_output.geometry 的全局坐标（粗略，只进日志）
    int32_t    modeW = 0, modeH = 0;   // 物理像素的 mode（/scale 约等于逻辑尺寸）
    // zxdg_output_v1 给的**真**逻辑尺寸/位置（比 mode/scale 估的准：
    // niri 把 scale 向上取整成 2，估算会把 1706x1066 当成 1280x800）
    zxdg_output_v1* xdg = nullptr;
    int32_t    lw = 0, lh = 0;
    int32_t    gx = 0, gy = 0;     // 逻辑**全局**位置（xdg-output 权威，用来判断邻屏）
    bool       haveXdg = false;
};

// 跨屏相关常量
static constexpr int    KEEP_PX      = 120;   // 至少留多少逻辑 px 在屏内（原 apply_margin 的值）
static constexpr int    CROSS_PX     = 80;    // 越过边界多少逻辑 px 才搬家（防抖阈值）
static constexpr double CROSS_COOL_S = 0.3;   // 搬完多久内不再搬（300ms）

static struct {
    wl_display*              dpy      = nullptr;
    wl_registry*             reg      = nullptr;
    wl_compositor*           comp     = nullptr;
    wl_shm*                  shm      = nullptr;
    zwlr_layer_shell_v1*     lshell   = nullptr;
    wl_surface*              surf     = nullptr;
    zwlr_layer_surface_v1*   lsurf    = nullptr;
    wl_output*               chosen   = nullptr;
    zxdg_output_manager_v1*  xdgMgr   = nullptr;

    // ── 指针 / 拖动 ────────────────────────────────────────────
    wl_seat*                 seat     = nullptr;
    wl_pointer*              ptr      = nullptr;
    bool                     dragOn   = true;   // MIYU_DRAG=0 关掉
    bool                     dragging = false;  // 正在拖
    bool                     ptrIn    = false;  // 指针在 surface 内
    // 指针「相对当前 surface 位置」的最后已知 local 坐标（逻辑 px）。
    // 关键：它不是简单地等于最新一次 motion 给的 sx/sy —— 我们挪完窗口之后
    // 指针相对新位置的 local 要减掉自己挪的那一段，否则拖动会自己衰减到 0（见下面注释）。
    int                      refSx    = 0, refSy = 0;
    int                      pendX    = 0, pendY = 0;  // 本帧攒下、还没应用到 margin 的位移

    wl_egl_window*           ewin     = nullptr;
    EGLDisplay               edpy     = EGL_NO_DISPLAY;
    EGLConfig                ecfg     = nullptr;
    EGLContext               ectx     = EGL_NO_CONTEXT;
    EGLSurface               esurf    = EGL_NO_SURFACE;

    OutInfo                  outs[8];
    int                      nouts    = 0;
    int                      curIdx   = -1;   // 当前挂在哪个 output 上（outs 下标）
    int                      scale    = 1;
    int                      logicalW = 0, logicalH = 0;  // **当前** output 的逻辑尺寸
    char                     title[64] = {0};             // 重建 layer surface 要同一份

    // 跨屏防抖：越界累积量（逻辑 px，带符号）+ 冷却截止时间
    int                      overX    = 0, overY = 0;
    double                   coolUntil = 0.0;

    // 上一帧的逻辑 alpha 掩码：切屏时复用它，立刻把 input region 重算一遍
    // （buffer scale 变了，但逻辑尺寸还是 500x900，掩码可以直接用）
    unsigned char*           mask     = nullptr;
    size_t                   maskCap  = 0;
    int                      maskW    = 0, maskH = 0, maskTh = 8;

    int                      reqW     = 0, reqH = 0;   // 逻辑尺寸（请求值）
    // 基准逻辑尺寸（platInit 传进来的 500x900）。reqW/reqH = base * 网页设置页的 scale。
    // 基准必须单独存：拿 reqW 反算会累积误差（0.4 → 0.8 回不到 500）。
    int                      baseW    = 0, baseH = 0;
    int                      hidden   = 0;             // 设置页「隐藏」：全透明 + 输入区清空
    int                      cfgW     = 0, cfgH = 0;   // configure 给的逻辑尺寸
    int                      configured = 0;
    int                      closed   = 0;

    int                      posX     = 0, posY = 0;
    bool                     inited   = false;
} W;

/* ---------------- wl_output ---------------- */
static void out_geometry(void* d, wl_output*, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char* make, const char* model, int32_t tf) {
    (void)pw;(void)ph;(void)sub;(void)tf;
    OutInfo* o = (OutInfo*)d;
    o->x = x; o->y = y;
    if (make)  snprintf(o->make,  sizeof o->make,  "%s", make);
    if (model) {
        snprintf(o->model, sizeof o->model, "%s", model);
        if (!o->label[0]) snprintf(o->label, sizeof o->label, "%s", model);
    }
}
static void out_mode(void* d, wl_output*, uint32_t, int32_t w, int32_t h, int32_t) {
    ((OutInfo*)d)->modeW = w;
    ((OutInfo*)d)->modeH = h;
}
static void out_done(void*, wl_output*) {}
static void out_scale(void* d, wl_output*, int32_t f) { ((OutInfo*)d)->scale = f ? f : 1; }
static void out_name(void* d, wl_output*, const char* name) {
    if (name) snprintf(((OutInfo*)d)->label, sizeof ((OutInfo*)d)->label, "%s", name);
}
static void out_desc(void* d, wl_output*, const char* desc) {
    if (desc) snprintf(((OutInfo*)d)->desc, sizeof ((OutInfo*)d)->desc, "%s", desc);
}

static const wl_output_listener out_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_desc
};

/* ---------------- zxdg_output_v1：真逻辑尺寸 ----------------
 * wl_output 只给物理 mode + 整数 scale（niri 报 2，实际 1.5），
 * 所以「屏有多大」只能靠它估。估小了后果很实在：mode/scale = 1280x800，
 * 比窗口 (500x900) 还矮 → 竖直方向的 margin 永远被夹成 0，**拖不动**。
 * xdg-output 直接给逻辑尺寸 1706x1066，用它做边界才对。
 */
static void xo_pos(void* d, zxdg_output_v1*, int32_t x, int32_t y) {
    ((OutInfo*)d)->gx = x; ((OutInfo*)d)->gy = y;
}
static void xo_size(void* d, zxdg_output_v1*, int32_t w, int32_t h) {
    OutInfo* o = (OutInfo*)d;
    o->lw = w; o->lh = h;
    o->haveXdg = (w > 0 && h > 0);
}
static void xo_done(void*, zxdg_output_v1*) {}
static void xo_name(void*, zxdg_output_v1*, const char*) {}
static void xo_desc(void*, zxdg_output_v1*, const char*) {}
static const zxdg_output_v1_listener xo_listener = {
    xo_pos, xo_size, xo_done, xo_name, xo_desc
};

/* ---------------- wl_pointer / 拖动 ----------------
 *
 * 拖动的坐标账 —— 这块真机试了三版，前两版都是飞的，先把结论写在这儿：
 *
 *   `wl_pointer.motion` 给的是 **surface-local 逻辑坐标**，而窗口跟着指针走，
 *   参考系自己在动。所以「新 margin = 按下 margin + (当前 local − 按下 local)」
 *   这条路是死的：窗口一追上指针，local 回到按下时的值，算出来 0，拖动当场停住。
 *
 *   现在的做法：记「上一次收到的 local」refSx/refSy，delta = 这次 − 上次，
 *   攒进 pendX/pendY，一帧结束前一次性应用到 margin。
 *   两个反直觉但必须遵守的细节：
 *
 *   ① **同值事件直接丢弃**。niri 每 commit 一次 margin（layer surface 挪动）
 *      就把同一个 local 再发一遍。当成新位移 → 每帧多挪一格 → 几十帧飞到屏幕边上
 *      （实测注入 −100，窗口跑了 −620）。
 *   ② **不做「挪了多少就补偿多少」**。niri 算 local 用的是拖动开始时的 surface
 *      矩形，我们挪窗口它不改 —— local 的差本身就是指针纯位移，再补偿一次
 *      就把自己挪的重复算进下一格，位移指数放大（同一组实测里就是这条最凶）。
 *
 *   结果：注入多少就走多少，margin 一分不差（见 NOTES.md 第七节实测表）。
 */
#define BTN_LEFT 0x110
static void drag_apply_pending();   // 定义在 apply_margin 之后

/* ---------------- 单击反应（tap）—— 第十三节 ----------------
 *
 * 「点她一下有反应」和「拖她走」共用同一个 BTN_LEFT，所以要先把两件事分开：
 *
 *   · 按下：**只记账**，不进拖动（pressX/pressY/pressT）。
 *   · 按下后位移越过 TAP_SLOP_PX → 当场转成拖动，并且把「按下点 → 现在」这段
 *     位移整段补进 pend（少了这 6px，拖 100 就只走 94）。
 *   · 松开且从没越过阈值：位移 ≤ TAP_SLOP_PX 且按住 < TAP_MAX_S → 单击。
 *
 * 选了「先不拖、超阈值才开始拖」这条路（而不是「松开时再决定」）：手感上更接近
 * 普通窗口 —— 一动起来就跟手，不用先等松开。代价是前 6px 之后才真正开始挪，
 * 已经在转拖动那一帧补回来了，实测注入 100 → 窗口走 105（见 NOTES 13.7-B）。
 *
 * 反应计划：点左半边播 `被戳.json`（低头躲），右半边播 `被戳2.json`（抬头侧偏），
 * 每第 3 次播 `被戳3.json`（往上一惊 + 眼珠甩开 + 嘴微张 = 换个表情）。
 * 冷却 800ms；正在播别的计划时 plan::start() 走播放器自己的 0.2s 交叉淡入接管。
 */
static constexpr int    TAP_SLOP_PX = 6;      // 单击的位移上限（逻辑 px）
static constexpr int    TAP_JUMP_PX = 120;    // 单个 motion 事件跳这么远 = 不是手在拖（见下）
static constexpr double TAP_MAX_S   = 0.400;  // 按住多久以上就不算单击
static constexpr double TAP_COOL_S  = 0.800;  // 冷却

static bool        gTapOn = true;             // MIYU_NO_TAP=1 关
static std::string gPlansDir;                 // 计划目录（MIYU_PLANS_DIR 或按 exe 找）
static int         gTapN = 0;                 // 第几次点击（含被冷却挡掉的）
static double      gTapCoolUntil = 0.0;

// 网页设置页的缩放（NOTES 第十四节）。存「最后一次真的应用过」的值，
// 同一个值重复 POST 时不重建 buffer（页面滑块拖回来会发一串）。
static float       gScaleNow = 1.0f;

// 按下记账
static bool   gPressArmed = false;
static int    gPressX = 0, gPressY = 0;
static double gPressT = 0.0;

// 计划目录：不写死绝对路径。
//   ① MIYU_PLANS_DIR 指定的优先
//   ② /proc/self/exe 所在目录下的 plans/（二进制被装到和 plans 同层时）
//   ③ /proc/self/exe 的上一级 plans/（本项目就是这种：build/miyu-wayland ↔ ../plans）
//   ④ 兜底 "plans/"（launcher 已经 cd 到 native3 了）
static void tapInit() {
    const char* no = getenv("MIYU_NO_TAP");
    gTapOn = !(no && strcmp(no, "1") == 0);

    if (const char* d = getenv("MIYU_PLANS_DIR")) {
        gPlansDir = d;
    } else {
        std::string exeDir;
        char buf[4096];
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            const std::string e(buf);
            const size_t sl = e.rfind('/');
            exeDir = (sl == std::string::npos) ? std::string(".") : e.substr(0, sl);
        }
        const std::string cands[3] = {exeDir + "/plans", exeDir + "/../plans", "plans"};
        for (int i = 0; i < 3; ++i) {
            if (access((cands[i] + "/被戳.json").c_str(), R_OK) == 0) { gPlansDir = cands[i]; break; }
        }
        if (gPlansDir.empty()) gPlansDir = "plans";
    }
    LOG("点击反应：%s | plans=%s", gTapOn ? "开" : "关（MIYU_NO_TAP=1）", gPlansDir.c_str());
}

static bool tapLoadPlan(const char* name, std::string* out) {
    const std::string p = gPlansDir + "/" + name;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) { LOG("[tap] 打不开 %s（这次不播）", p.c_str()); return false; }
    out->clear();
    char b[4096];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) out->append(b, n);
    std::fclose(f);
    if (out->empty()) { LOG("[tap] %s 是空的（这次不播）", p.c_str()); return false; }
    return true;
}

// 真点下去那一下。lx/ly 是 surface-local 逻辑坐标。
static void tapFire(int lx, int ly) {
    ++gTapN;
    const double now = platTime();
    if (now < gTapCoolUntil) {
        LOG("[tap] 第 %d 次点击 local=(%d,%d) → 冷却中（还剩 %.2fs），不播",
            gTapN, lx, ly, gTapCoolUntil - now);
        return;
    }
    // 2026-11-09 22:3x：按**身体部位**分，不再按左右半边（他说那样太糙）。
    // 用她本人 bbox 把 y 归一化成 0..1，从上往下四段：
    //   <0.26 头 → 被戳（＞＜ 躲）     <0.46 脸/胸 → 被戳4（脸红）
    //   <0.72 腰/裙 → 被戳2（侧偏）   ≥0.72 腿/脚 → 被戳5（低头看你 + ＞＜）
    // 同一部位连点第 3 次换「一惊」（被戳3），避免看腻。
    int pcx = 0, pcy = 0, phw = 0, phh = 0;
    const bool haveBox = platPersonBox(&pcx, &pcy, &phw, &phh);
    const char* file;
    if (gTapN % 3 == 0 && haveBox) {
        file = "被戳3.json";               // 每第 3 次：整个人一惊
    } else if (!haveBox) {
        file = "被戳.json";
    } else {
        const int top = pcy - phh;
        float t = (phh > 0) ? float(ly - top) / float(2 * phh) : 0.5f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        if (t < 0.26f)      file = "被戳.json";    // 头
        else if (t < 0.46f) file = "被戳4.json";   // 脸 / 胸
        else if (t < 0.72f) file = "被戳2.json";   // 腰 / 裙
        else                file = "被戳5.json";   // 腿 / 脚
        LOG("[tap] 部位 t=%.2f（0 头 1 脚）", t);
    }
    std::string body;
    if (!tapLoadPlan(file, &body)) return;
    gTapCoolUntil = now + TAP_COOL_S;
    plan::start(body, now);
    LOG("[tap] 第 %d 次点击 local=(%d,%d) → 播 %s", gTapN, lx, ly, file);
}

// 2026-11-09 21:0x：不能只看 W.ptrIn —— input region 每 10 帧重算一次，
// region 一变合成器就会重判指针归属，enter/leave 会抖，ptrIn 经常是 false，
// 于是视线一直「回正」。改成看「最近 1.5 秒内有没有指针活动」。
static double gLastPtrT = -1e9;
static double nowSec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void ptr_enter(void*, wl_pointer*, uint32_t, wl_surface* s, wl_fixed_t sx, wl_fixed_t sy) {    if (!s || s != W.surf) return;
    W.ptrIn = true;
    gLastPtrT = nowSec();
    W.refSx = wl_fixed_to_int(sx);
    W.refSy = wl_fixed_to_int(sy);
    LOG("指针进入 surface，local=(%d,%d)", W.refSx, W.refSy);
}

static void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface* s) {
    if (!s || s != W.surf) return;
    W.ptrIn = false;
    // 拖动中指针可能短暂跑出 surface：**不要**在这里结束拖动，
    // 也别拿这一刻的 local 当基准（已经在窗外了）。回到 surface 时 enter 会重置基准。
    if (W.dragging) LOG("指针离开 surface（拖动中，保持拖动状态）");
}

static const bool dbg = getenv("MIYU_DRAG_DEBUG") != nullptr;
static void ptr_motion(void*, wl_pointer*, uint32_t, wl_fixed_t sx, wl_fixed_t sy) {
    const int x = wl_fixed_to_int(sx);
    const int y = wl_fixed_to_int(sy);
    // 合成器在 layer surface 挪动之后会把**同一个** local 重发一次（一次 commit 一个），
    // 值一模一样。当成新位移就会每帧再挪一格，几十帧就飞到屏幕边上
    // （实测：注入 −100，窗口跑了 −620）。同值直接丢。
    gLastPtrT = nowSec();           // 只要指针动过就算「在」
    if (x == W.refSx && y == W.refSy) return;
    const int dx = x - W.refSx;
    const int dy = y - W.refSy;
    W.refSx = x;
    W.refSy = y;
    if (!W.dragging) {
        // 按下后还没进拖动：位移越过阈值才转拖动（单击/拖动分流，见上面 tap 注释）
        if (gPressArmed && (std::abs(dx) > TAP_JUMP_PX || std::abs(dy) > TAP_JUMP_PX)) {
            // 一个事件就跳这么远，不是手在拖：是「按下之后指针被瞬移 / 窗口刚重排」
            // 带来的假位移（实测：注入器绝对定位把光标挪 600+ px，那一下会把单击
            // 变成拖动，把她甩到屏幕另一头）。把按点跟着挪同样一段，重新起算。
            gPressX += dx;
            gPressY += dy;
            if (dbg) LOG("[dbg] 按下后大跳 (%d,%d) → 按点重定位 local=(%d,%d)",
                         dx, dy, gPressX, gPressY);
            return;
        }
        if (gPressArmed && W.dragOn) {
            const int ax = x - gPressX, ay = y - gPressY;
            if (ax * ax + ay * ay > TAP_SLOP_PX * TAP_SLOP_PX) {
                W.dragging = true;
                W.pendX = ax;      // 按下点到现在整段补上，不丢前面那几像素
                W.pendY = ay;
                gPressArmed = false;
                LOG("开始拖动：margin=(%d,%d) 按点 local=(%d,%d)", W.posX, W.posY, gPressX, gPressY);
            }
        }
        if (!W.dragging) {
            if (dbg) LOG("[dbg] motion（未拖）local=(%d,%d) ref=(%d,%d) pos=(%d,%d) ptrIn=%d",
                         x, y, W.refSx, W.refSy, W.posX, W.posY, (int)W.ptrIn);
            return;
        }
    }
    if (!W.ptrIn) { W.ptrIn = true; return; }   // 刚回来，这一格不认（避免跳一下）
    if (dbg) LOG("[dbg] motion local=(%d,%d) ref=(%d,%d) d=(%d,%d) pos=(%d,%d) pend=(%d,%d)",
                 x, y, W.refSx - dx, W.refSy - dy, dx, dy, W.posX, W.posY, W.pendX, W.pendY);
    W.pendX += dx;
    W.pendY += dy;
}

static void ptr_button(void*, wl_pointer*, uint32_t, uint32_t, uint32_t button, uint32_t state) {
    if (button != BTN_LEFT) return;
    // 注意：这里**没有** `if (!W.dragOn) return;` 了 —— 拖动开关只管拖动，
    // 单击反应有自己的开关（MIYU_NO_TAP=1）；两个开关互不牵连。
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        drag_apply_pending();                    // 把按下前攒的零头清掉
        gPressArmed = false;
        W.pendX = W.pendY = 0;
        if (!W.ptrIn) return;                    // 不在她身上按下 → 既不当单击也不拖
        // 只记账，先不进拖动（等位移超阈值、或松开时判定单击）
        gPressX = W.refSx;
        gPressY = W.refSy;
        gPressT = nowSec();
        gPressArmed = true;
        LOG("按下 local=(%d,%d)（松开时判单击/拖动）", gPressX, gPressY);
    } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
        if (W.dragging) {
            drag_apply_pending();
            W.dragging = false;
            gPressArmed = false;
            LOG("结束拖动：margin=(%d,%d)", W.posX, W.posY);
            return;
        }
        if (!gPressArmed) return;
        gPressArmed = false;
        // 从没越过阈值 → 看位移和按住时长：短按 + 几乎没动 = 单击
        const int dx = W.refSx - gPressX;
        const int dy = W.refSy - gPressY;
        const double held = nowSec() - gPressT;
        if (dx * dx + dy * dy <= TAP_SLOP_PX * TAP_SLOP_PX && held < TAP_MAX_S) {
            if (gTapOn) tapFire(W.refSx, W.refSy);
        } else if (dbg) {
            LOG("[dbg] 不认作单击：位移(%.1f,%.1f) 按住%.0fms", (double)dx, (double)dy, held * 1000.0);
        }
    }
}

static void ptr_axis(void*, wl_pointer*, uint32_t, uint32_t axis, wl_fixed_t value) {
    LOG("pointer axis=%u value=%.2f（不处理）", axis, wl_fixed_to_double(value));
}

// 后面的 frame / axis_source / axis_stop / axis_discrete / axis_value120 /
// axis_relative_direction / warp 我们都不接，显式补 nullptr —— 不然 -Wextra 的
// -Wmissing-field-initializers 会一条一条报（协议头比这里用到的版本新）。
static const wl_pointer_listener ptr_listener = {
    ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis,
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
};

static void seat_caps(void*, wl_seat* s, uint32_t caps) {
    const bool want = (caps & WL_SEAT_CAPABILITY_POINTER) != 0;
    if (want && !W.ptr) {
        W.ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(W.ptr, &ptr_listener, nullptr);
        LOG("wl_pointer 就位（拖动%s）", W.dragOn ? "开" : "关：MIYU_DRAG=0");
    } else if (!want && W.ptr) {
        wl_pointer_destroy(W.ptr);
        W.ptr = nullptr;
        W.dragging = false;
        LOG("seat 撤了 pointer 能力");
    }
}
static void seat_name(void*, wl_seat*, const char* n) {
    LOG("seat=%s", n ? n : "?");
}
static const wl_seat_listener seat_listener = { seat_caps, seat_name };

/* ---------------- registry ---------------- */
static void reg_global(void*, wl_registry* r, uint32_t name, const char* iface, uint32_t ver) {
    if (!strcmp(iface, wl_compositor_interface.name)) {
        W.comp = (wl_compositor*)wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        W.shm = (wl_shm*)wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        // 绑 v4 而不是最新的：wl_pointer 的版本跟着 seat 走，v5 起多了 frame /
        // axis_source / axis_stop / axis_discrete 这些事件，listener 里少一个
        // libwayland 就当场 "listener function for opcode N is NULL" 断开连接
        // （第一次就是绑到 v7、只写了 5 个回调，进程直接死）。
        // v4 的 wl_pointer 正好是 enter/leave/motion/button/axis 五个，一一对上。
        W.seat = (wl_seat*)wl_registry_bind(r, name, &wl_seat_interface, ver < 4 ? ver : 4);
        wl_seat_add_listener(W.seat, &seat_listener, nullptr);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        W.lshell = (zwlr_layer_shell_v1*)wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface,
                                                         ver < 4 ? ver : 4);
    } else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
        W.xdgMgr = (zxdg_output_manager_v1*)wl_registry_bind(
            r, name, &zxdg_output_manager_v1_interface, ver < 3 ? ver : 3);
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

// 把 posX/posY 夹进**当前 output** 的逻辑边界，再作为 margin 提交。
// 2026-11-09 20:1x：以前这里是「整个窗口必须留在屏内」，拖到边就贴住，
// 他觉得像有一圈隐形边框。改成「至少留 KEEP_PX 在屏内」——允许她半个身子
// 探出屏幕，但不会整个跑丢。margin 在协议里是 int32，负数合法。
//
// 2026-11-09 21:xx（跨屏）：夹边界必须用**当前 output** 的逻辑尺寸。
// 以前 W.logicalW/H 只在 init 时按 eDP-1 的 1706x1066 填一次，搬到 HDMI-A-3
// 之后还拿 1706 去夹 1920 宽的屏，右边永远够不着。现在 buildSurface() 每次
// 换屏都会把 W.logicalW/H 换成新 output 的 xdg-output 值。
static void apply_margin() {
    if (!W.lsurf) return;
    // anchor TOP|LEFT：margin top = y，left = x。
    // 注意：TOP 边被 exclusive zone（顶上 bar，约 37 逻辑 px）占着时，margin
    // 从 zone 的下沿起算 —— 也就是 y=0 实际落在物理 y≈56，不是屏顶。贴边自己减。
    int mx = W.posX, my = W.posY;
    if (W.logicalW > 0) {
        const int lo = -(W.reqW - KEEP_PX), hi = W.logicalW - KEEP_PX;
        if (mx < lo) mx = lo;
        if (mx > hi) mx = hi;
    }
    if (W.logicalH > 0) {
        const int lo = -(W.reqH - KEEP_PX), hi = W.logicalH - KEEP_PX;
        if (my < lo) my = lo;
        if (my > hi) my = hi;
    }
    if (mx != W.posX || my != W.posY)
        LOG("位置夹回屏内：(%d,%d) -> margin (%d,%d)（%s 逻辑 %dx%d）",
            W.posX, W.posY, mx, my,
            (W.curIdx >= 0 ? W.outs[W.curIdx].label : "?"), W.logicalW, W.logicalH);
    // 夹过的值写回 posX/posY：拖动时靠「应用前后的位置差」算真实位移，
    // 不写回的话 pos 会一直在屏外涨，refSx 的补偿永远对不上（越拖越往外漂）。
    W.posX = mx;
    W.posY = my;
    zwlr_layer_surface_v1_set_margin(W.lsurf, my, 0, 0, mx);
    wl_surface_commit(W.surf);
    wl_display_flush(W.dpy);
}

// 把 W.mask（逻辑尺寸的 alpha 掩码）应用成 input region。
// 切屏时也调它：buffer scale 变了，但**逻辑尺寸没变**（500x900），
// 上一帧的掩码可以直接再用一次，不用等 main 下一轮 glReadPixels。
static size_t applyRegionFromMask(const char* why) {
    if (!W.surf || !W.comp || !W.mask || W.maskW <= 0 || W.maskH <= 0) return 0;
    const uint8_t th = (uint8_t)(W.maskTh < 0 ? 0 : (W.maskTh > 255 ? 255 : W.maskTh));
    const size_t stride = (size_t)W.maskW * 4;
    const size_t n = alpha_region_scan(W.mask, W.maskW, W.maskH, stride, th, nullptr, 0);
    alpha_region_apply(W.surf, W.comp, W.mask, W.maskW, W.maskH, stride, th, 4096);
    wl_surface_commit(W.surf);
    wl_display_flush(W.dpy);
    LOG("input region（%s）：逻辑 %dx%d | %zu 条矩形 | threshold=%u",
        why, W.maskW, W.maskH, n, th);
    return n;
}

// 等 layer surface 的 configure（非阻塞 poll 包一层，别把事件循环卡死）
static bool waitConfigure(int timeoutMs) {
    W.configured = 0;
    int waited = 0;
    while (!W.configured) {
        if (waited >= timeoutMs) return false;
        struct pollfd pfd;
        pfd.fd = wl_display_get_fd(W.dpy);
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 50) > 0) {
            if (wl_display_dispatch(W.dpy) < 0) return false;
        }
        waited += 50;
        wl_display_flush(W.dpy);
    }
    return true;
}

// 在 outs[idx] 上建 wl_surface + layer surface（OVERLAY / TOP|LEFT / 不占 zone / 不吃键盘）
static bool makeLayerSurface(int idx) {
    OutInfo* o = &W.outs[idx];
    W.curIdx   = idx;
    W.chosen   = o->out;
    W.scale    = o->scale > 0 ? o->scale : 1;
    W.logicalW = o->lw;
    W.logicalH = o->lh;

    W.surf = wl_compositor_create_surface(W.comp);
    if (!W.surf) { LOG("wl_compositor_create_surface 失败"); return false; }
    W.lsurf = zwlr_layer_shell_v1_get_layer_surface(W.lshell, W.surf, W.chosen,
                ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, W.title[0] ? W.title : "plat-wayland");
    if (!W.lsurf) { LOG("get_layer_surface 失败"); return false; }
    zwlr_layer_surface_v1_add_listener(W.lsurf, &ls_listener, nullptr);
    zwlr_layer_surface_v1_set_size(W.lsurf, (uint32_t)W.reqW, (uint32_t)W.reqH);
    zwlr_layer_surface_v1_set_anchor(W.lsurf,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
    zwlr_layer_surface_v1_set_exclusive_zone(W.lsurf, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(W.lsurf,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(W.surf);

    if (!waitConfigure(2000)) { LOG("等 configure 超时（%s）", o->label); return false; }
    LOG("layer surface 挂在 %s 上：逻辑 %dx%d / scale %d / 全局 %+d,%+d",
        o->label, W.logicalW, W.logicalH, W.scale, o->gx, o->gy);
    return true;
}

// 建 wl_egl_window + EGLSurface，并把**已有的** context 挂上去（context 不重建）
static bool createEGLWindow() {
    const int pw = W.reqW * W.scale, ph = W.reqH * W.scale;
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
    glViewport(0, 0, pw, ph);
    LOG("逻辑尺寸 %dx%d  ->  物理尺寸 %dx%d (scale=%d)", W.reqW, W.reqH, pw, ph, W.scale);
    return true;
}

// 搬到 outs[idx]：拆 surface 链 → 在目标 output 上重建 → 重新挂 context → 重算 region。
// **GL context 和它里面的纹理一个字节都不动**，只重建 surface（这是整件事的关键）。
static bool buildSurface(int idx, const char* why, int entryX, int entryY) {
    if (idx < 0 || idx >= W.nouts) return false;
    OutInfo* t = &W.outs[idx];
    LOG("搬家（%s）：output[%d] %s  ->  output[%d] %s",
        why, W.curIdx, W.curIdx >= 0 ? W.outs[W.curIdx].label : "?", idx, t->label);

    // ① 拆旧链。顺序是死的：EGLSurface 引用 wl_egl_window，必须先拆它；
    //    layer surface 引用 wl_surface，同样先拆。
    if (W.esurf != EGL_NO_SURFACE) {
        eglMakeCurrent(W.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, W.ectx);  // 先解绑，context 留着
        eglDestroySurface(W.edpy, W.esurf);
        W.esurf = EGL_NO_SURFACE;
    }
    if (W.ewin)  { wl_egl_window_destroy(W.ewin);  W.ewin  = nullptr; }
    if (W.lsurf) { zwlr_layer_surface_v1_destroy(W.lsurf); W.lsurf = nullptr; }
    if (W.surf)  { wl_surface_destroy(W.surf); W.surf = nullptr; }
    wl_display_flush(W.dpy);

    // ② 目标 output 上重建
    if (!makeLayerSurface(idx)) { W.closed = 1; return false; }
    if (!createEGLWindow())     { W.closed = 1; return false; }

    // ③ 位置：新屏上的初始 margin，夹一遍再提交
    W.posX = entryX;
    W.posY = entryY;
    W.ptrIn = false;          // 新 surface 上指针要重新 enter，基准重来
    W.overX = W.overY = 0;
    W.coolUntil = platTime() + CROSS_COOL_S;
    apply_margin();

    // 拖到一半搬家的，这次拖动到此为止。
    // 原因（实测）：surface 重建之后 niri 的 drag 坐标系还挂在**旧** surface 上，
    // 每帧会交错发两套 local（新 surface 一套、旧矩形一套），差值一对 +40 / −40
    // 互相抵消，窗口原地抖。不是我们的账算错，是那个锚点已经不存在了。
    // 所以搬完就松手：他重新按住就能在新屏上继续拖（按下 → 新 surface 上 enter → 新基准）。
    if (W.dragging) {
        W.dragging = false;
        LOG("跨屏后结束本次拖动（surface 重建了，niri 的 drag 锚点失效，重新按下才能继续拖）");
    }

    // ④ input region：立刻重算（buffer scale 变了；逻辑尺寸没变，掩码能直接复用）
    if (W.mask && W.maskW == W.reqW && W.maskH == W.reqH) applyRegionFromMask("切屏后立即");

    // 先把新 surface 刷成一张全透明，别让它露出上一帧的残留
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    eglSwapBuffers(W.edpy, W.esurf);

    LOG("已经搬到 %s 上：逻辑 %dx%d / scale %d / 物理 %dx%d / margin (%d,%d)",
        t->label, W.logicalW, W.logicalH, W.scale,
        W.reqW * W.scale, W.reqH * W.scale, W.posX, W.posY);
    return true;
}

// 找当前 output 在 axis(0=x,1=y) 那一侧、dir(+1/-1) 方向上的邻屏。
// 用 xdg-output 的**逻辑全局坐标**，要求另一条轴上有重叠（真的挨着）。
static int findNeighbor(int axis, int dir) {
    if (W.curIdx < 0) return -1;
    OutInfo* c = &W.outs[W.curIdx];
    const int32_t cStart      = axis == 0 ? c->gx : c->gy;
    const int32_t cLen        = axis == 0 ? c->lw : c->lh;
    const int32_t cCrossStart = axis == 0 ? c->gy : c->gx;
    const int32_t cCrossLen   = axis == 0 ? c->lh : c->lw;
    if (cLen <= 0) return -1;

    int best = -1;
    int64_t bestD = INT64_MAX;
    for (int i = 0; i < W.nouts; i++) {
        if (i == W.curIdx) continue;
        OutInfo* o = &W.outs[i];
        const int32_t oStart      = axis == 0 ? o->gx : o->gy;
        const int32_t oLen        = axis == 0 ? o->lw : o->lh;
        const int32_t oCrossStart = axis == 0 ? o->gy : o->gx;
        const int32_t oCrossLen   = axis == 0 ? o->lh : o->lw;
        if (oLen <= 0) continue;
        if (dir > 0 && oStart < cStart + cLen - 1) continue;          // 不在右边/下面
        if (dir < 0 && oStart + oLen > cStart + 1) continue;          // 不在左边/上面
        if (oCrossStart + oCrossLen <= cCrossStart ||
            oCrossStart >= cCrossStart + cCrossLen) continue;          // 另一条轴上不重叠
        const int64_t d = dir > 0 ? (int64_t)oStart - (cStart + cLen)
                                  : (int64_t)cStart - (oStart + oLen);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// 拖动越界判定 + 搬家。
// 为什么不能直接看 margin 有没有超出屏宽：margin 被 apply_margin() 夹住，永远超不过。
// 能反映「他还在往外推」的只有每帧攒下的 pend（want = 夹住的 pos + pend），
// 所以这里把它相对**夹边界**多出来的部分累积起来，累积到 CROSS_PX 才搬。
//   · 一旦往反方向拖（want 回到边界内），累积清零 → 不会在边界上来回抖
//   · 搬完 CROSS_COOL_S 内直接返回 → 300ms 内不可能连搬
static bool maybeCross(int wantX, int wantY) {
    if (W.nouts < 2 || W.curIdx < 0) return false;
    if (platTime() < W.coolUntil) return false;

    const int hiX = W.logicalW - KEEP_PX, loX = -(W.reqW - KEEP_PX);
    const int hiY = W.logicalH - KEEP_PX, loY = -(W.reqH - KEEP_PX);
    if (W.logicalW > 0) {
        if (wantX > hiX)      W.overX += wantX - hiX;
        else if (wantX < loX) W.overX += wantX - loX;
        else                  W.overX = 0;
    }
    if (W.logicalH > 0) {
        if (wantY > hiY)      W.overY += wantY - hiY;
        else if (wantY < loY) W.overY += wantY - loY;
        else                  W.overY = 0;
    }

    int axis = -1, dir = 0;
    if (W.overX >= CROSS_PX)        { axis = 0; dir = +1; }
    else if (W.overX <= -CROSS_PX)  { axis = 0; dir = -1; }
    else if (W.overY >= CROSS_PX)   { axis = 1; dir = +1; }
    else if (W.overY <= -CROSS_PX)  { axis = 1; dir = -1; }
    if (axis < 0) return false;

    const int nb = findNeighbor(axis, dir);
    if (nb < 0) {
        // 那一侧没屏：清零，不然掉头拖回来时会立刻误触
        if (axis == 0) W.overX = 0; else W.overY = 0;
        return false;
    }

    const int over = axis == 0 ? W.overX : W.overY;
    LOG("跨屏触发：%s越过边界 %d 逻辑 px（阈值 %d，冷却 %.1fs 内不再切）",
        axis == 0 ? (dir > 0 ? "向右" : "向左") : (dir > 0 ? "向下" : "向上"),
        over, CROSS_PX, CROSS_COOL_S);

    OutInfo* t = &W.outs[nb];
    int entryX = W.posX, entryY = W.posY;
    if (axis == 0) entryX = dir > 0 ? 0 : (t->lw > 0 ? t->lw - W.reqW : 0);
    else           entryY = dir > 0 ? 0 : (t->lh > 0 ? t->lh - W.reqH : 0);
    W.pendX = W.pendY = 0;
    buildSurface(nb, "拖动越界", entryX, entryY);
    return true;
}

// 把这一帧攒下的拖动位移一次应用掉。
// refSx/refSy 要用**实际**挪动的量补偿：屏幕上顶到边被夹住时，pend 比实际挪的多，
// 按 pend 补偿会让下一次 motion 再把这段重复算一遍 —— 窗口会持续往外顶。
static void drag_apply_pending() {
    if (!W.pendX && !W.pendY) return;
    const int bx = W.posX, by = W.posY;
    const int px = W.pendX, py = W.pendY;
    W.pendX = W.pendY = 0;
    if (maybeCross(bx + px, by + py)) {          // 越界先搬家，搬完这一格位移作废
        if (dbg) LOG("[dbg] 跨屏后新 margin=(%d,%d)", W.posX, W.posY);
        return;
    }
    platSetPosition(bx + px, by + py);
    const int ax = W.posX - bx, ay = W.posY - by;
    (void)ax; (void)ay;   // 只给 dbg 日志用
    // 注意：这里**不做** refSx -= 实际位移 的补偿。
    // niri 算 local 用的是「surface 拖动开始时的矩形」，我们挪窗口它不改，
    // 所以 local 的差本身就是指针的纯位移 —— 再补偿一次就把自己挪的重复算进去，
    // 位移会指数放大（真机实测过：注入 −100 → 实际跑 −620）。
    if (dbg) LOG("[dbg] 应用 pend=(%d,%d) 实际=(%d,%d) pos=(%d,%d)",
                 px, py, ax, ay, W.posX, W.posY);
}

/* ---------------- 多屏：对外接口 ---------------- */
// 把所有 output 打成一张表（make/model/geometry/scale/mode/逻辑位置/逻辑尺寸）。
// 跨屏出问题时先看这张表：逻辑尺寸和逻辑全局位置必须都对得上 `niri msg outputs`。
static void printOutputTable() {
    LOG("output 表（%d 个）：", W.nouts);
    LOG("  idx  name          scale  mode        logical      global        make / model");
    for (int i = 0; i < W.nouts; i++) {
        OutInfo* o = &W.outs[i];
        LOG("  [%d]  %-12s  %3d   %4dx%-4d  %4dx%-4d  %+5d,%+5d   %s / %s%s",
            i, o->label, o->scale, o->modeW, o->modeH, o->lw, o->lh, o->gx, o->gy,
            o->make[0] ? o->make : "?", o->model[0] ? o->model : "?",
            o->haveXdg ? "" : "  (逻辑尺寸是估算值)");
    }
}

int  platOutputCount()  { return W.nouts; }
int  platCurrentOutput(){ return W.curIdx; }

bool platMoveToOutput(int index) {
    if (!W.inited) { LOG("platMoveToOutput(%d)：还没 init", index); return false; }
    if (index < 0 || index >= W.nouts) {
        LOG("platMoveToOutput(%d)：越界（有效 0..%d）", index, W.nouts - 1);
        return false;
    }
    if (index == W.curIdx) {
        LOG("platMoveToOutput(%d)：已经在 %s 上", index, W.outs[index].label);
        return true;
    }
    return buildSurface(index, "platMoveToOutput", W.posX, W.posY);
}

bool platInit(int w, int h, const char* title) {
    if (W.inited) return true;
    if (w <= 0 || h <= 0) { LOG("非法尺寸 %dx%d", w, h); return false; }
    W.reqW = w; W.reqH = h;
    W.baseW = w; W.baseH = h;   // 缩放要能来回，基准单独存（见 WlState 里那段注释）

    // 拖动开关：默认开，只有 MIYU_DRAG=0（严格等于 "0"）才关。
    {
        const char* de = getenv("MIYU_DRAG");
        W.dragOn = !(de && strcmp(de, "0") == 0);
        LOG("拖动开关：%s (MIYU_DRAG=%s)", W.dragOn ? "开" : "关", de ? de : "(未设，默认开)");
    }
    tapInit();   // 单击反应：开关 + 计划目录（第十三节）

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

    // 每个 output 都建一条 xdg_output：拿真逻辑尺寸 + **逻辑全局位置**
    // （跨屏要靠全局位置判断「哪个屏在哪个方向、挨着谁」）
    if (W.xdgMgr) {
        for (int i = 0; i < W.nouts; i++) {
            W.outs[i].xdg = zxdg_output_manager_v1_get_xdg_output(W.xdgMgr, W.outs[i].out);
            zxdg_output_v1_add_listener(W.outs[i].xdg, &xo_listener, &W.outs[i]);
        }
        wl_display_roundtrip(W.dpy);
        wl_display_roundtrip(W.dpy);
    }
    // 没有 xdg-output 就退回 mode/scale 估算：niri 的 wl_output.scale 向上取整
    // （1.5 报 2），1706x1066 会估成 1280x800 —— 只够防跑出屏，做不了精确布局。
    for (int i = 0; i < W.nouts; i++) {
        OutInfo* o = &W.outs[i];
        if (!o->haveXdg) {
            const int s = o->scale > 0 ? o->scale : 1;
            o->lw = o->modeW > 0 ? o->modeW / s : 0;
            o->lh = o->modeH > 0 ? o->modeH / s : 0;
            o->gx = o->x;
            o->gy = o->y;
        }
    }
    printOutputTable();

    /* 选 output：MIYU_OUTPUT=N 指名；否则优先 eDP-1（本机主屏），再不然第一个 */
    int pick = -1;
    {
        const char* e = getenv("MIYU_OUTPUT");
        if (e && *e) {
            const int n = atoi(e);
            if (n >= 0 && n < W.nouts) {
                pick = n;
                LOG("MIYU_OUTPUT=%s -> output[%d] %s", e, n, W.outs[n].label);
            } else {
                LOG("MIYU_OUTPUT=%s 越界（有效 0..%d），忽略", e, W.nouts - 1);
            }
        }
        if (pick < 0) {
            for (int i = 0; i < W.nouts; i++)
                if (strstr(W.outs[i].label, "eDP-1")) { pick = i; break; }
        }
        if (pick < 0) pick = 0;
    }

    /* 2) OVERLAY 层 surface */
    snprintf(W.title, sizeof W.title, "%s", title ? title : "plat-wayland");
    if (!makeLayerSurface(pick)) return false;

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

    // 必须是 COMPATIBILITY 画像，不能是 CORE。
    // Cubism 的 GLES2 渲染器用的是**客户端顶点数组**那套老 API：
    //   glVertexAttribPointer(..., 0, CPU 指针) + glDrawElements(GL_TRIANGLES, n,
    //   GL_UNSIGNED_SHORT, indexArray)，indexArray 是栈上的 uint16 数组
    //   （CubismRenderer_OpenGLES2.cpp:1157）。
    // CORE profile 下这两条都是 GL_INVALID_OPERATION —— 460 个可见 drawable 一个也
    // 落不到 framebuffer 上，读回来全是 alpha=0（实测：region 0 条矩形、截图只有壁纸）。
    // X11 那条路（platform_x11.cpp 坑③）是同一个原因。
    const EGLint ctxAttrCompat[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
        EGL_NONE
    };
    W.ectx = eglCreateContext(W.edpy, W.ecfg, EGL_NO_CONTEXT, ctxAttrCompat);
    if (W.ectx == EGL_NO_CONTEXT) {
        const EGLint ctxAttrCore[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3,
            EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
            EGL_NONE
        };
        LOG("3.3 compat 失败 0x%x，退 core（大概率画不出来）", eglGetError());
        W.ectx = eglCreateContext(W.edpy, W.ecfg, EGL_NO_CONTEXT, ctxAttrCore);
    }
    if (W.ectx == EGL_NO_CONTEXT) {
        LOG("再退默认 context");
        W.ectx = eglCreateContext(W.edpy, W.ecfg, EGL_NO_CONTEXT, nullptr);
    }
    if (W.ectx == EGL_NO_CONTEXT) { LOG("eglCreateContext 彻底失败 0x%x", eglGetError()); return false; }

    // 按 ceil(scale) 建大 buffer，避免 scale>1 时糊；buffer_scale 同步告诉合成器
    if (!createEGLWindow()) return false;

    LOG("EGL %d.%d | %s", emaj, emin, eglQueryString(W.edpy, EGL_VENDOR));
    LOG("GL_VERSION=%s", (const char*)glGetString(GL_VERSION));
    LOG("GL_RENDERER=%s", (const char*)glGetString(GL_RENDERER));

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

    // 拖动：这一帧攒的位移在这里一次应用（一帧一次 commit，不是每个 motion 一次）
    drag_apply_pending();
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

bool platIsDragging() { return W.dragging; }

// 指针在哪（surface-local 逻辑 px）。只认 enter/leave 记的账：
//   · 不在窗口内 → false，调用方把视线目标归零（她不看人）
//   · 在窗口内 → refSx/refSy 是最后一次 enter/motion 收到的 local。
//     别在这里自己算「窗口挪了多少」：那个换算在 ptr_motion 里已经做过一遍
//     （同值去重 + 不补偿，见第七节的坐标账）。
bool platPointer(float* x, float* y) {
    if (!W.ptrIn && nowSec() - gLastPtrT > 1.5) return false;
    if (x) *x = (float)W.refSx;
    if (y) *y = (float)W.refSy;
    return true;
}

// 最近一次输入区计算时得到的「她本人」包围盒（逻辑坐标），供视线跟随用
static int gPcx = -1, gPcy = -1, gPhw = 0, gPhh = 0;

bool platPersonBox(int* cx, int* cy, int* hw, int* hh) {
    if (gPcx < 0 || gPhw <= 0) return false;
    if (cx) *cx = gPcx;
    if (cy) *cy = gPcy;
    if (hw) *hw = gPhw;
    if (hh) *hh = gPhh;
    return true;
}

void platGetLogicalSize(int* w, int* h) {
    if (w) *w = W.reqW;
    if (h) *h = W.reqH;
}

// ── 运行时设置（网页设置页，NOTES 第十四节）────────────────────────────
// 缩放：重建 wl_egl_window + 重设 buffer_scale。
// 为什么不是「只改 EGL 视口」：layer surface 的逻辑尺寸是 set_size 定的，
// 逻辑 px 就是屏幕上的可见尺寸；不改 set_size 只改 buffer 只会变糊/变锐，大小不变。
//   · EGL context 和里面的纹理一个字节都不动（和跨屏搬家同一套做法）
//   · 位置保持：posX/posY 不变，只是按新尺寸再夹一遍 margin
//   · 输入区立刻清空一次（旧掩码是旧尺寸的），main 下一帧会按新尺寸重算
bool platSetScale(float factor) {
    if (factor < 0.4f) factor = 0.4f;
    if (factor > 1.6f) factor = 1.6f;
    if (factor == gScaleNow) return true;                 // 同一个值不重建（防抖）
    gScaleNow = factor;
    if (!W.inited || !W.surf || W.baseW <= 0) {
        LOG("缩放记下 %.2f（窗口还没起来，init 时生效）", factor);
        return false;
    }
    const int nw = (int)((float)W.baseW * factor + 0.5f);
    const int nh = (int)((float)W.baseH * factor + 0.5f);
    if (nw < 64 || nh < 64) { LOG("缩放 %.2f 太小（%dx%d），忽略", factor, nw, nh); return false; }
    W.reqW = nw;
    W.reqH = nh;

    // ① 合成器那侧：新的逻辑尺寸
    if (W.lsurf) zwlr_layer_surface_v1_set_size(W.lsurf, (uint32_t)nw, (uint32_t)nh);
    wl_surface_commit(W.surf);

    // ② 我们这侧：拆 EGLSurface + wl_egl_window，按新尺寸重建（context 留着）
    if (W.esurf != EGL_NO_SURFACE) {
        eglMakeCurrent(W.edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, W.ectx);
        eglDestroySurface(W.edpy, W.esurf);
        W.esurf = EGL_NO_SURFACE;
    }
    if (W.ewin) { wl_egl_window_destroy(W.ewin); W.ewin = nullptr; }
    if (!createEGLWindow()) { LOG("缩放 %.2f：重建 EGL window 失败", factor); W.closed = 1; return false; }

    // ③ 输入区先清空（旧掩码是旧尺寸的）；main 下一帧按新 framebuffer 重算
    if (W.comp) {
        wl_region* empty = wl_compositor_create_region(W.comp);
        if (empty) {
            wl_surface_set_input_region(W.surf, empty);
            wl_region_destroy(empty);
        }
    }
    // ④ 位置保持（只重新夹一遍 margin）
    apply_margin();
    LOG("缩放 %.2f -> 逻辑 %dx%d / 物理 %dx%d / margin (%d,%d)",
        factor, nw, nh, nw * W.scale, nh * W.scale, W.posX, W.posY);
    return true;
}

void platSetTapEnabled(bool on) {
    gTapOn = on;
    LOG("点击反应：%s（网页设置页）", on ? "开" : "关");
}

// 吸角。anchor 一直是 TOP|LEFT，所以四角都靠 margin 表达：
//   · 顶部两角：y=0 —— TOP 会落在 exclusive zone（顶上的 bar，约 37 逻辑 px）下沿
//   · 底部两角：y = 屏高 - 窗高 - bar 高度，否则窗口一多半跑到屏外（bar 把可见区整体下推）
//     bar 高度默认 37（本机实测），MIYU_BAR_H 可覆盖，别的机器/关掉 bar 时自己调。
bool platSnapCorner(const char* corner) {
    if (!W.inited) { LOG("吸角：窗口还没起来"); return false; }
    if (!corner || !strcmp(corner, "free")) return true;      // free = 不动
    int barH = 37;
    if (const char* bh = getenv("MIYU_BAR_H")) barH = atoi(bh);
    const int W_ = W.logicalW, H_ = W.logicalH;
    if (W_ <= 0 || H_ <= 0) { LOG("吸角：还不知道当前 output 的逻辑尺寸"); return false; }

    int x = W.posX, y = W.posY;
    if (!strcmp(corner, "tl"))      { x = 0;                    y = 0; }
    else if (!strcmp(corner, "tr")) { x = W_ - W.reqW;          y = 0; }
    else if (!strcmp(corner, "bl")) { x = 0;                    y = H_ - W.reqH - barH; }
    else if (!strcmp(corner, "br")) { x = W_ - W.reqW;          y = H_ - W.reqH - barH; }
    else { LOG("吸角：不认识的角 %s", corner); return false; }

    LOG("吸角 %s：屏 %dx%d / 窗 %dx%d / bar %d -> margin (%d,%d)",
        corner, W_, H_, W.reqW, W.reqH, barH, x, y);
    platSetPosition(x, y);
    return true;
}

// 隐藏：不拆 surface（拆了要等 configure，还可能被合成器重置尺寸），
// 改成「画一张全透明 + 输入区清空」—— 像素层面和空桌面完全一致，点击直接穿透，
// 进程和渲染循环照常跑。main 那边在 hidden 时连模型都不画（省 CPU）。
void platSetHidden(bool hidden) {
    W.hidden = hidden ? 1 : 0;
    if (!W.inited || !W.surf) return;
    if (hidden) {
        if (W.comp) {
            wl_region* empty = wl_compositor_create_region(W.comp);
            if (empty) {
                wl_surface_set_input_region(W.surf, empty);
                wl_region_destroy(empty);
            }
        }
        wl_surface_commit(W.surf);
        wl_display_flush(W.dpy);
        LOG("隐藏：全透明 + 输入区清空（进程还在，点击穿透）");
    } else {
        LOG("显示：下一帧重算输入区");
    }
}

const char* platOutputLabel() {
    return W.curIdx >= 0 && W.curIdx < W.nouts ? W.outs[W.curIdx].label : "?";
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
    if (W.ptr)  { wl_pointer_destroy(W.ptr); W.ptr = nullptr; }
    if (W.seat) { wl_seat_destroy(W.seat); W.seat = nullptr; }
    if (W.lsurf) { zwlr_layer_surface_v1_destroy(W.lsurf); W.lsurf = nullptr; }
    if (W.surf)  { wl_surface_destroy(W.surf); W.surf = nullptr; }
    if (W.lshell){ zwlr_layer_shell_v1_destroy(W.lshell); W.lshell = nullptr; }
    if (W.shm)   { wl_shm_destroy(W.shm); W.shm = nullptr; }
    if (W.comp)  { wl_compositor_destroy(W.comp); W.comp = nullptr; }
    for (int i = 0; i < W.nouts; i++) {
        if (W.outs[i].xdg) { zxdg_output_v1_destroy(W.outs[i].xdg); W.outs[i].xdg = nullptr; }
        if (W.outs[i].out) { wl_output_destroy(W.outs[i].out); W.outs[i].out = nullptr; }
    }
    if (W.xdgMgr) { zxdg_output_manager_v1_destroy(W.xdgMgr); W.xdgMgr = nullptr; }
    if (W.reg)   { wl_registry_destroy(W.reg); W.reg = nullptr; }
    if (W.mask)  { free(W.mask); W.mask = nullptr; W.maskCap = 0; W.maskW = W.maskH = 0; }
    wl_display_disconnect(W.dpy);
    W.dpy = nullptr;
    W.inited = false;
}

// ── 按 alpha 抠输入区域：整个集成里唯一需要算坐标的地方 ─────────────
//
// 输入是 glReadPixels 读回来的整张 framebuffer：
//   · RGBA8888，**首行是底部**（GL 原点在左下）
//   · 尺寸是**物理像素**（本机 = 逻辑 × scale，scale=2 → 1000x1800）
// 而 wl_region 要的是 surface-local **逻辑像素**、原点在**左上**。
//
// 所以这里必须做两件事，缺一件就错：
//   ① 上下翻转：逻辑第 0 行取物理最后一行
//   ② 除以 scale：逻辑区域 = 物理 / 2。不除的话区域大一倍，
//      她旁边半张屏的空白也会吃掉点击，"穿透"就退化成"整窗可点"
//
// 每个逻辑像素取它覆盖的 s×s 块里 alpha 的**最大值**（不是最近邻），
// 免得头发丝、手指这些细结构被降采样削掉 —— 削掉就是那块点不到。
void platSetInputRegionFromAlpha(const unsigned char* rgba, int w, int h, int threshold) {
    if (!W.surf || !W.comp || !rgba || w <= 0 || h <= 0) return;

    // 隐藏期间不收输入区（设置页已经把区域清空了）。这里再建一遍就等于
    // 把整块可点区域还回去，隐藏就白做了。恢复显示时 main 会重算一次。
    if (W.hidden) return;

    const int s = W.scale > 0 ? W.scale : 1;
    const int lw = w / s;
    const int lh = h / s;
    if (lw <= 0 || lh <= 0) return;

    const size_t need = (size_t)lw * (size_t)lh * 4;
    if (need > W.maskCap) {
        unsigned char* p = (unsigned char*)realloc(W.mask, need);
        if (!p) { LOG("region 缓冲分配失败 (%zu 字节)", need); return; }
        W.mask = p;
        W.maskCap = need;
    }
    unsigned char* buf = W.mask;
    W.maskW = lw;
    W.maskH = lh;
    W.maskTh = threshold;

    for (int ly = 0; ly < lh; ly++) {
        unsigned char* out = buf + (size_t)ly * (size_t)lw * 4;
        for (int lx = 0; lx < lw; lx++) {
            unsigned char amax = 0;
            for (int dy = 0; dy < s; dy++) {
                const int sy = h - 1 - (ly * s + dy);   // 翻转
                if (sy < 0) continue;
                const unsigned char* row = rgba + (size_t)sy * (size_t)w * 4;
                for (int dx = 0; dx < s; dx++) {
                    const int sx = lx * s + dx;
                    if (sx >= w) continue;
                    const unsigned char a = row[(size_t)sx * 4 + 3];
                    if (a > amax) amax = a;
                }
            }
            out[(size_t)lx * 4 + 0] = 0;
            out[(size_t)lx * 4 + 1] = 0;
            out[(size_t)lx * 4 + 2] = 0;
            out[(size_t)lx * 4 + 3] = amax;
        }
    }

    const uint8_t th = (uint8_t)(threshold < 0 ? 0 : (threshold > 255 ? 255 : threshold));
    const size_t stride = (size_t)lw * 4;
    const size_t total = alpha_region_scan(buf, lw, lh, stride, th, nullptr, 0);
    alpha_region_apply(W.surf, W.comp, buf, lw, lh, stride, th, 4096);
    wl_surface_commit(W.surf);
    wl_display_flush(W.dpy);

    // 第一次算完把「条数 / 面积 / 包围盒」写进日志，用来对账：
    //   · 条数为 0 → 整张都是透明的（画没出来，或读错了 buffer）
    //   · 包围盒上下颠倒 → 翻转写反了
    //   · 包围盒超出 500x900 → 忘了除以 scale
    static bool detailed = false;
    if (!detailed) {
        detailed = true;
        struct alpha_rect* rs = (struct alpha_rect*)malloc(total * sizeof(*rs));
        if (rs) {
            const size_t n = alpha_region_scan(buf, lw, lh, stride, th, rs, total);
            int32_t bx0 = 1 << 30, by0 = 1 << 30, bx1 = -1, by1 = -1;
            for (size_t i = 0; i < n; i++) {
                if (rs[i].x < bx0) bx0 = rs[i].x;
                if (rs[i].y < by0) by0 = rs[i].y;
                if (rs[i].x + rs[i].w > bx1) bx1 = rs[i].x + rs[i].w;
                if (rs[i].y + rs[i].h > by1) by1 = rs[i].y + rs[i].h;
            }
            if (bx1 > bx0 && by1 > by0) {
                gPcx = (bx0 + bx1) / 2;
                gPcy = (by0 + by1) / 2;
                gPhw = (bx1 - bx0) / 2;
                gPhh = (by1 - by0) / 2;
            }
            LOG("input region: 物理 %dx%d -> 逻辑 %dx%d | %zu 条矩形 | 覆盖 %zu px (%.1f%%) | bbox=[%d,%d .. %d,%d] | threshold=%u",
                w, h, lw, lh, n, alpha_region_area(rs, n),
                (double)alpha_region_area(rs, n) * 100.0 / ((double)lw * (double)lh),
                bx0, by0, bx1, by1, th);
            free(rs);
        }
    }

    static int logged = 0;
    if (logged < 6 || (logged % 200) == 0) {
        LOG("input region 更新 #%d：%zu 条矩形", ++logged, total);
    }
}
