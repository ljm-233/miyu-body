// 本地设置页：进程自己起一个极简 HTTP 服务（127.0.0.1:8200），浏览器打开就能调参数。
// 2026-11-09 22:5x 加的（NOTES 第十四节）。
//
// 分工：
//   · 这个头/实现只管「收 HTTP → 改 Setting → 落盘 → 通知谁去生效」。
//     它**不认识** Cubism、不碰 GL、不碰 Wayland —— 生效动作靠 main 注册的回调。
//   · 每帧只 poll(fd, 0) 一次、最多接两个连接，绝不阻塞渲染。
//   · 端口默认 8200，MIYU_WEB_PORT 可覆盖；被占用只在日志里说一句，不崩。
//   · bind 到回环（INADDR_LOOPBACK），外面连不进来。

#pragma once

#include <map>
#include <string>
#include <vector>

namespace web {

// 运行时设置。**改一项立刻生效**：main / platform 每帧直接读这里。
//
// 2026-11-09 23:xx（第十六节）：新增 12 项。规矩不变 —— 每一项都得有「谁在每帧读它」
// 或「改它时回调了谁」，只存不用的字段一个都不留（自测见 NOTES 15.4）。
struct Settings {
    float       scale  = 1.0f;      // 0.4–1.6  窗口视觉缩放（重建 wl_egl_window）
    float       alpha  = 1.0f;      // 0.3–1.0  整体不透明度（乘到最终颜色上）
    bool        tap    = true;      // 单击反应（等价 MIYU_NO_TAP，运行时可变）
    bool        expr   = true;      // 表情切换（plan 里的 expr 也受它管）
    float       idle   = 1.0f;      // 0.0–3.0  待机幅度倍率（0 = 只留呼吸）
    std::string corner = "center";  // center/tl/tr/bl/br/free
    bool        hide   = false;     // 整个隐藏（全透明 + 输入区清空，进程还在）

    // ── 第十六节新增 ──────────────────────────────────────────────
    std::string idleStyle = "natural";  // quiet/natural/lively：待机幅度×间隔的预设档
    float       look   = 1.0f;      // 0–2   视线跟随强度（眼珠 + 身体微倾都乘它）
    bool        lookOn = true;      // 视线跟随总开关（关掉只回正，不跟鼠标）
    float       blink  = 1.0f;      // 0–2   眨眼频率倍率（0 = 完全不眨）
    float       mouth  = 1.0f;      // 0.5–3 说话时嘴动时长倍率
    float       exprHold = 1.0f;    // 0.2–5 表情停留时长倍率（到点淡回默认）
    int         fps    = 240;       // 30/60/120/200/240。他喜欢拉满：副屏 200Hz 实际到 200，主屏 240
    bool        voice  = false;     // 说话时出声（播 voice/*.wav，没有就静默）
    float       vol    = 0.8f;      // 0–1   音量（pw-play --volume）

    // ── 第十七节新增：抗锯齿 + 后处理三件 ────────────────────────────
    // ── 第十八节改：ssaa → aa，主开关换成 FXAA ──────────────────────
    // off / fxaa / ssaa2x / ssaa3x。老 key `ssaa`（off/2x/3x）仍然读得进来，
    // 旧的 settings.json 不会炸（见 web.cpp 里 loadSettings / handleSet 的映射）。
    // 2026-11-10 00:1x：默认从 fxaa 改回 ssaa2x —— 他一试就说「fxaa 开了变模糊」。
    // fxaa 按亮度在整条边缘带上做宽带混合，这个模型的边本身就是贴图里的 alpha 渐变，
    // 一混就是 3–4 像素的糊。ssaa2x 反而更锐（贴图接近 1:1、不走 mipmap，见 17.6）。
    // 十九：新增 smaa（三遍：边缘检测 → 混合权重 → 邻域混合，只沿边缘混）。
    // 默认仍是 ssaa2x —— 他喜欢锐，SSAA 实测细节保留最好，SMAA 是可选档。
    std::string aa     = "ssaa2x";  // off/ssaa2x/ssaa3x/fxaa/smaa
    float       shadow = 0.0f;      // 0–1   地面软阴影强度（默认关：深色壁纸上基本看不见）
    float       rim    = 0.35f;     // 0–1   轮廓光强度
    float       particles = 0.3f;   // 0–1   粒子数量倍率

    // ── 二十：拖动体感 + 时间感（2026-11-10 01:xx）────────────────────
    // 四项都默认开。前三个只管「按住她」那一段的手感，第四个是她的作息。
    bool        grab     = true;    // 抓起反馈：开始拖那一瞬的短动作（0.42s 上提 + 后仰）
    bool        inertia  = true;    // 松手惯性：按最后 ~120ms 的速度再滑一段（×0.86/帧）
    bool        edgeSoft = true;    // 贴边软阻力：越过「留 120px」那条线后位移 ×0.55
    bool        timeAware= true;    // 时间感：按本地钟点换待机幅度 / 眨眼 / 深夜低头

    // ── 二十之二：看得见的摆动（动的是投影矩阵，不是参数）────────────
    // AngleX/AngleZ/PositionX 那些参数**不驱动任何 drawable**（16.2），所以
    // 「身体摆动」只能靠投影矩阵倾斜。这两项就是它的旋钮。
    float       dragSway = 1.0f;    // 0–2   拖动时身体倾斜的强度（1 = 500 px/s 给 3°）
    bool        swayIdle = false;   // 静止时极慢的轻摆（只在 lively 风格下生效；喂的是物理不是投影）
    float       swayTilt = 0.0f;    // 0–4.5° 整体倾斜（投影矩阵）。默认 0：整张图转会露馅
    float       dragSpring = 1.0f;
    float         chestSway = 1.0f;    // 胸部戳动的晃动幅度倍率（0–3，1 = 原样）
    float         legSway = 1.0f;      // 腿部戳动的晃动幅度倍率（0–3）  // 0–2   拖动弹簧增益（乘 ω）

    // ── 二十一：点哪动哪（2026-11-10 02:xx）──────────────────────────
    // 开着：戳她身上的部位 → 只让那一块的参数动（弹一下）。关掉回到整套反应。
    bool        tapPart = true;     // 部位级反应（关掉 = 老的一套 plan 照播）

    // 二十五：每个部位点下去播哪个表情。空串 = 只有动作、不放表情。
    // 存在 state/part-react.tsv（不是一个字段一个字段塞 settings.json，
    // 那玩意儿是定长结构，逐部位的东西塞进去只会越来越难维护）。
    // 网页设置页「点击反应」那一组直接读写它。
    std::map<std::string, std::string> partExpr;
};

// 一项设置改完要「真的生效」，重活在各层里（platform 重建 buffer、main 换绘制路径），
// 所以 web 只负责喊一声：key 是变了的那个字段名（"scale" / "alpha" / …）。
using ApplyFn = void (*)(const char* key);

// 读回 state/settings.json（没有就用默认值）+ 起监听。失败只记日志。
void init(ApplyFn apply);

// ── 十九：smaa 的可用性（main 在 postfx::init() 之后调一次）────────────
// false 时：页面「抗锯齿」下拉里不留 smaa 这一项，POST 传来 smaa 也按 ssaa2x 落。
// 默认 true —— 没调之前「表还没试过」不等于「不可用」，别在启动读盘那一步
// 把 settings.json 里的 smaa 吃掉。
void setSmaaOk(bool ok);

// 请求的是 smaa 但这一版用不了：写成 ssaa2x 并落盘（日志一句话，由 main 触发）。
void aaFallback();

// 每帧一次：非阻塞收一个连接。
void poll(double now);

// 收尾（关监听、关残留连接）。
void shutdown();

// main / platform 每帧直接读这些值（只读视图）。
Settings&       settings();
const Settings& settingsC();

// 状态栏要显示的东西：输出名 + 窗口逻辑尺寸（main 每帧喂一次，便宜）。
void setStatus(const char* output, int w, int h);

// 实测帧率（main 的 framePace 每 5 秒算完喂一次；顶栏「帧率」显示的就是它）。
// 没人喂过就保持 -1，页面上显示「—」。
void setFps(double fps);

// 表情清单（main 从 model/Exp3 扫出来给「切个表情」按钮用；空就退回默认）。
void setExprNames(std::vector<std::string> names);
// 二十五：给设置页用的「现在有哪几个表情可挑」
const std::vector<std::string>& exprNames();

// 设置落到哪个文件（日志/文档用）。
const char* statePath();

// 监听在哪个端口（0 = 没起来）。
int port();

}  // namespace web
