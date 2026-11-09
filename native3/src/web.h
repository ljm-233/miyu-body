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
    std::string corner = "free";    // tl/tr/bl/br/free
    bool        hide   = false;     // 整个隐藏（全透明 + 输入区清空，进程还在）

    // ── 第十六节新增 ──────────────────────────────────────────────
    std::string idleStyle = "natural";  // quiet/natural/lively：待机幅度×间隔的预设档
    float       look   = 1.0f;      // 0–2   视线跟随强度（眼珠 + 身体微倾都乘它）
    bool        lookOn = true;      // 视线跟随总开关（关掉只回正，不跟鼠标）
    float       blink  = 1.0f;      // 0–2   眨眼频率倍率（0 = 完全不眨）
    float       mouth  = 1.0f;      // 0.5–3 说话时嘴动时长倍率
    float       exprHold = 1.0f;    // 0.2–5 表情停留时长倍率（到点淡回默认）
    int         fps    = 60;        // 15/30/60 帧率上限（省电）
    bool        voice  = false;     // 说话时出声（播 voice/*.wav，没有就静默）
    float       vol    = 0.8f;      // 0–1   音量（pw-play --volume）
};

// 一项设置改完要「真的生效」，重活在各层里（platform 重建 buffer、main 换绘制路径），
// 所以 web 只负责喊一声：key 是变了的那个字段名（"scale" / "alpha" / …）。
using ApplyFn = void (*)(const char* key);

// 读回 state/settings.json（没有就用默认值）+ 起监听。失败只记日志。
void init(ApplyFn apply);

// 每帧一次：非阻塞收一个连接。
void poll(double now);

// 收尾（关监听、关残留连接）。
void shutdown();

// main / platform 每帧直接读这些值（只读视图）。
Settings&       settings();
const Settings& settingsC();

// 状态栏要显示的东西：输出名 + 窗口逻辑尺寸（main 每帧喂一次，便宜）。
void setStatus(const char* output, int w, int h);

// 表情清单（main 从 model/Exp3 扫出来给「切个表情」按钮用；空就退回默认）。
void setExprNames(std::vector<std::string> names);

// 设置落到哪个文件（日志/文档用）。
const char* statePath();

// 监听在哪个端口（0 = 没起来）。
int port();

}  // namespace web
