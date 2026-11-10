// 部位级反应（第二十一节 → 二十二节重做 → Step 3/4 收进 partapi）
//
// 这个头现在只剩三样东西 + 一层兼容壳：
//   ① 邮筒：gGen / gPart —— 平台层判完「点到哪个部位」，渲染层照着播
//   ② 格子图：gZoneCells（渲染层每帧刷、平台层只读）和它的尺寸
//   ③ 兼容壳：classify / hasParams / top —— 转调 partapi，**签名不变**，
//      所以 main.cpp / platform_wayland.cpp 的调用点一行都不用改
//
// **数据全在 state/parts.tsv**，由 src/partapi.h 一次读进来。这个文件里现在
// 没有任何表、没有任何 `state/...` 路径、没有任何按部位名写死的判断。
//
// 为什么还是 header-only、而且只有头：CMakeLists.txt 两个 target 都把源文件列死
// （`add_executable(miyu …)` / `add_executable(miyu-wayland …)`），加 .cpp 要同时动两处。
// C++17 的 `inline` 变量在所有 TU 里是同一个对象 —— 这个头的老套路。
//
// 分工，谁的数据在谁手里：
//   · 平台层（platform_wayland.cpp）：有 lx/ly → classify 判「点到哪个部位」
//   · 渲染层（main.cpp）：有 Cubism 模型 → 参数索引、默认值、弹簧积分
//   中间的线就是 gGen（第几次请求）+ gPart（哪个部位）。
//
// 二十二节的两条规矩还在这儿（别丢）：
//   ① **判定按剪影**：不再用「她本人 bbox 里切 7 个矩形」—— bbox 被右边那条大尾巴
//      撑宽，横向全偏，点胸口会被判成手臂。现在查渲染层按 drawable→part 光栅化出来的
//      格子图（128×240，alpha 裁过、判定带钉过）。
//   ② **只拨根**：param 行给的是 physics3.json 里只被输入、从不被输出的**根**
//      （PokeChest 这种局部输入），绝不写 ChestX1/SkirtY3 那些物理算出来的输出。

#pragma once

#include "partapi.h"   // 部位数据的唯一来源（state/parts.tsv）

#include <string>
#include <vector>

namespace partpoke {

// ── 邮筒 ────────────────────────────────────────────────────────────
inline int  gGen = 0;          // 0 = 还没点过；每点一次 +1
inline char gPart[32] = {0};   // 部位名（parts.tsv 里的名字）

// 2026-11-10 23:0x：设置页「诊断」那组要显示「最近一次点击判成了哪个部位」。
// 平台层写、main 每帧转给 web。存 u/v 是为了能看出「点的是哪儿、判的是哪儿」——
// 今晚所有串位都是靠这两个数才定位出来的。
inline bool  gDoubleTap = false;  // 二十六：这一下是不是「双击」（同一部位 0.4 秒内第二下）
inline int   gLastHitId = 0;      // 0 = 还没点过
inline float gLastHitU = 0.0f;
inline float gLastHitV = 0.0f;

// ── 格子图 ──────────────────────────────────────────────────────────
// 二十四：60×120 → 120×240。一格从 8.3×7.5 px 变成 4.2×3.75 px，因为脸要拆细：
// 眉毛（12px 高）、眼睛（22px）、嘴（10px）三个在 60×120 里各自只有 1–2 行格子，
// 分不开 —— 眉毛一动就整片算到眼睛头上（实测 ParamBrowLY3 154px 里 114px 落进眼睛格子）。
// 网格尺寸也写进了 part-zones.tsv 的 `#grid` 行，工具读它、判定也读它。
inline int gGridW = 120;
inline int gGridH = 240;

// Step 7（2026-11-10 22:4x）：**部位名单不再写在这里**。
// 名字的唯一来源是 state/parts.tsv 的 part 行，由 partapi 提供：
//   partapi::count()        现在有几个部位
//   partapi::nameOf(id)     名字（id 从 1 起，0 = 空白）
//   partapi::labelOf(id)    中文标签
// 加一个部位 = 表里加几行，这里和 main.cpp 都不用动。
//
// 只留一个**编译期上限**给数组用（main.cpp 里有几处按部位下标的统计数组）。
// 选 256 是因为格子图的值类型是 unsigned char —— 上限必须盖住它的全部取值，
// 否则 g[j] 拿来当下标会越界。真正的部位数看 partapi::count()。
inline constexpr int kZoneMax = 256;

inline const char* zoneName(int id) { return partapi::nameOf(id); }

// 值的下标：0 = 空白（她身上没东西）。渲染层每帧刷，平台层只读。
inline std::vector<unsigned char> gZoneCells;
inline unsigned long gZoneStamp = 0;   // 每刷一次 +1（调试用）
inline int gZoneWinW = 0, gZoneWinH = 0;   // 刷这张图时的窗口逻辑尺寸

// ── 兼容壳：类型别名 ────────────────────────────────────────────────
// Step 4：参数记录搬到 partapi::Param（id / name / score / amp / up）。
// 这个别名让 main.cpp 里 `std::vector<partpoke::PartParam>` 这类声明一个字都不用改。
using PartParam = partapi::Param;

// ── 兼容壳：三个查询 ────────────────────────────────────────────────
// 窗口归一化坐标（u = lx/窗口宽, v = ly/窗口高）→ 部位名；空白返回空串。
//
// Step 3：判定逻辑在 partapi::classify（scope=any 的带 → 格子图 → 文件 Z 行兜底）。
// 这里只做一件事：把每帧刷出来的格子图递过去。
//   · 递指针而不是拷贝：gZoneCells 是渲染层每帧覆盖写的同一个 vector，但**可能重新分配**
//     （尺寸变了会 assign），所以每次调用前都重新 set 一次，不缓存 data()。
inline std::string classify(float u, float v) {
    partapi::setGrid(gZoneCells.empty() ? nullptr : gZoneCells.data(), gGridW, gGridH);
    return partapi::classify(u, v);
}

// 这个部位有没有活着的根参数（没有就别抢，让整套反应去播）。
// Step 4：读 state/parts.tsv 的 param 行，不再是自读 param-parts.tsv。
inline bool hasParams(const std::string& part) {
    return partapi::hasParams(part);
}

// 这个部位比值最高的前 n 个根（partapi 里已按 score 降序）。空 = 这个部位没有可拨的。
inline std::vector<PartParam> top(const std::string& part, int n) {
    return partapi::top(partapi::indexOf(part), n);
}

}  // namespace partpoke
