// 部位 API —— 一个部位的所有属性，从 state/parts.tsv 一次读进来（设计见 NOTES-part-api.md）
//
// 为什么是 header-only、而且只有头：CMakeLists.txt 的两个 target 都把源文件列死
// （`add_executable(miyu …)` / `add_executable(miyu-wayland …)`），加 .cpp 要同时动
// 两处。C++17 的 `inline` 变量在所有 TU 里是同一个对象，正好够用（partpoke.h 也是这个套路）。
//
// 这个头**不依赖 Cubism、不依赖 GL**，只用标准库 + POSIX —— 所以 tools/parts-selftest.cc
// 可以拿 g++ 一条命令编出来对账，不用起那具身体。
//
// 一份文件六种记录（列定义见 state/parts.tsv 表头）：
//   part / alias / band / param / expr / follow
//
// 分工（和现在一样，只是数据改成从这里来）：
//   · 渲染层：applyBands() 刷格子图（每帧）、top()/pickExpr() 点下去那一下
//   · 平台层：classify() 判「点到哪个部位」
//
// 加载失败的三层兜底（设计文档 4.4）：
//   ① 文件读不到 → 空注册表，classify 恒空 → 点击退回整套 plan（老路），不崩
//   ② 单行语法错 → 跳过该行，bad+1，启动时一次性打印（最多 20 条）
//   ③ 记录引用了未定义的部位 → 跳过并记 bad。**不自动建部位** ——
//      自动建的话一个笔误就长出第 14 个部位，格子图的 id 会漂。
//
// 待做（后续步骤，先留接口位置不让本文件变成半成品）：
//   Step 6 followTick() —— 拖动跟随（现在还在 main.cpp 的 legDragTick 里）

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <unistd.h>
#include <vector>

namespace partapi {

// 格子值存 unsigned char（和 partpoke 的 gZoneCells 同一个类型），0 = 空白
constexpr int kMaxParts = 250;

// ── 记录 ────────────────────────────────────────────────────────────
struct Band {
    unsigned char id = 0;
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0;   // 两端**闭区间**（照抄 main.cpp:1505 的 gy<=gy1）
    bool  force = false;                    // true=无条件盖 / false=只填还没标签的格子
    int   prio = 0;                         // 同格多命中：大者赢；同 prio 后写的赢
    bool  any = false;                       // true=无视剪影，classify 第一步就判
};

struct Param {
    unsigned char id = 0;
    std::string   name;      // 模型参数 id（**物理输入**，不是输出）
    int           score = 0; // 比值×10
    float         amp = 0;   // 绝对偏移量
    bool          up = false;// 只往正方向（嘴/眉）
};

struct Expr {
    unsigned char id = 0;
    std::string   pick;      // "脸红" 或 "脸红/兔牙/＞＜"
    double        hold = 0;  // 0 = 用 part 行的 hold
};

struct Follow {
    unsigned char id = 0;
    std::string   name;      // 参数 id
    float         gain = 0;
    float         clampv = 0;
    char          src = 'x'; // ax / az / ay → 'x' / 'z' / 'y'
    // 2026-11-10 23:4x：每条 follow 可以带自己的弹簧刚度/阻尼（第 7、8 列，可省）。
    // 省略 = 用默认 170 / 10.5。给脚那几条调软，它就跟得更晚、甩得更明显。
    float         K = 0;     // 0 = 用默认
    float         C = 0;
};

struct Alias { unsigned char id = 0; std::string kw; };

struct Part {
    std::string name, label;
    double      hold = 5.0;
    bool        dragOnly = false;
};

struct Zone {   // part-zones.tsv 的 Z 行：格子区间（半开！和 band 不同，见设计文档 4.1）
    int v0 = 0, v1 = 0, u0 = 0, u1 = 0;
    unsigned char id = 0;
};

struct Registry {
    bool        loaded = false;
    bool        ok = false;          // 真的读到至少一个部位
    std::string file;                // 用的哪一份（日志/自检要能说出路径）
    std::string zoneFile;
    int         bad = 0;
    std::string notes;               // 坏行原文（最多 20 条，拼好的）
    std::vector<Part>   parts;       // parts[i] 的 id = i+1
    std::vector<Alias>  aliases;     // 文件顺序 = 匹配优先级
    std::vector<Band>   bands;       // 已按 prio 降序稳定排序
    std::vector<Param>  params;      // 已按 score 降序稳定排序
    std::vector<Expr>   exprs;
    std::vector<Follow> follows;
    std::vector<Zone>   zones;
    // 设置页的显示顺序（display 记录，一行列完）。空 = 按 id 顺序。
    // 它只是「页面上怎么排」，和 id / 格子里存的数字毫无关系 —— 改它不影响判定。
    std::vector<unsigned char> display;
    int zoneGw = 120, zoneGh = 240;   // part-zones.tsv 的 #grid（没有那行就用这个默认）
};

// ── 小工具（不依赖 partpoke.h，两边互不牵连）──────────────────────────
inline std::string exeDir() {
    std::string dir = ".";
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        const std::string exe(buf);
        const size_t sl = exe.rfind('/');
        if (sl != std::string::npos) dir = exe.substr(0, sl);
    }
    return dir;
}

// 环境变量 > 可执行文件旁边的 state/ > ../state/ > state/（和 partpoke::findFile 同一套候选）
inline std::string findFile(const char* name, const char* env) {
    if (const char* e = std::getenv(env)) return std::string(e);
    const std::string dir = exeDir();
    const std::string cands[3] = {dir + "/state/" + name,
                                  dir + "/../state/" + name,
                                  std::string("state/") + name};
    for (int i = 0; i < 3; ++i)
        if (access(cands[i].c_str(), R_OK) == 0) return cands[i];
    return std::string("state/") + name;
}

inline std::vector<std::string> splitTab(const std::string& s) {
    std::vector<std::string> col;
    size_t p = 0;
    for (;;) {
        const size_t q = s.find('\t', p);
        col.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 1;
    }
    return col;
}

inline void chomp(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

// 行尾注释：从第一个以 '#' 开头的列起整段丢掉（值里不会出现 '#'）
inline void dropComment(std::vector<std::string>& c) {
    for (size_t i = 0; i < c.size(); ++i)
        if (!c[i].empty() && c[i][0] == '#') { c.resize(i); return; }
}

inline float toF(const std::string& s) { return s.empty() ? 0.0f : static_cast<float>(std::atof(s.c_str())); }
inline int   toI(const std::string& s) { return s.empty() ? 0 : std::atoi(s.c_str()); }

inline int idOf(const Registry& r, const std::string& name) {
    for (size_t i = 0; i < r.parts.size(); ++i)
        if (r.parts[i].name == name) return static_cast<int>(i) + 1;
    return -1;
}

inline void note(Registry& r, int line, const char* what, const std::string& detail) {
    ++r.bad;
    if (r.bad > 20) return;
    char buf[256];
    std::snprintf(buf, sizeof buf, "line %d %s %s", line, what, detail.c_str());
    if (!r.notes.empty()) r.notes += " | ";
    r.notes += buf;
}

// ── 加载 ────────────────────────────────────────────────────────────
inline void loadParts(Registry& r) {
    r.file = findFile("parts.tsv", "MIYU_PARTS_FILE");
    FILE* f = std::fopen(r.file.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[partapi] 读不到 %s —— 部位功能关闭，点击回整套反应\n",
                     r.file.c_str());
        r.ok = false;
        return;
    }
    char line[4096];
    int ln = 0;
    while (std::fgets(line, sizeof line, f)) {
        ++ln;
        std::string s(line);
        chomp(s);
        if (s.empty() || s[0] == '#') continue;
        std::vector<std::string> c = splitTab(s);
        dropComment(c);
        if (c.empty()) continue;
        const std::string kind = c[0];

        if (kind == "part") {
            if (c.size() < 3) { note(r, ln, "part 列数不够", s); continue; }
            if (idOf(r, c[1]) > 0) { note(r, ln, "部位重复", c[1]); continue; }
            Part p;
            p.name = c[1];
            p.label = c[2];
            p.hold = (c.size() > 3 && !c[3].empty()) ? std::atof(c[3].c_str()) : 5.0;
            if (c.size() > 4) p.dragOnly = (c[4].find("dragonly") != std::string::npos);
            if (static_cast<int>(r.parts.size()) >= kMaxParts) {
                note(r, ln, "部位超过上限", c[1]);
                continue;
            }
            r.parts.push_back(p);
        } else if (kind == "alias") {
            if (c.size() < 3) { note(r, ln, "alias 列数不够", s); continue; }
            const int id = idOf(r, c[1]);
            if (id < 0) { note(r, ln, "alias 引用了未定义部位", c[1]); continue; }
            size_t p0 = 0;
            const std::string& kws = c[2];
            for (;;) {
                const size_t p1 = kws.find(',', p0);
                std::string kw = kws.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
                while (!kw.empty() && (kw.front() == ' ' || kw.front() == '\t')) kw.erase(0, 1);
                while (!kw.empty() && (kw.back() == ' ' || kw.back() == '\t')) kw.pop_back();
                if (!kw.empty()) { Alias a; a.id = static_cast<unsigned char>(id); a.kw = kw; r.aliases.push_back(a); }
                if (p1 == std::string::npos) break;
                p0 = p1 + 1;
            }
        } else if (kind == "band") {
            if (c.size() < 8) { note(r, ln, "band 列数不够", s); continue; }
            const int id = idOf(r, c[1]);
            if (id < 0) { note(r, ln, "band 引用了未定义部位", c[1]); continue; }
            Band b;
            b.id = static_cast<unsigned char>(id);
            b.u0 = toF(c[2]); b.u1 = toF(c[3]);
            b.v0 = toF(c[4]); b.v1 = toF(c[5]);
            b.force = (c[6] == "force");
            b.prio = toI(c[7]);
            if (c.size() > 8) b.any = (c[8] == "any");
            r.bands.push_back(b);
        } else if (kind == "param") {
            if (c.size() < 5) { note(r, ln, "param 列数不够", s); continue; }
            const int id = idOf(r, c[1]);
            if (id < 0) { note(r, ln, "param 引用了未定义部位", c[1]); continue; }
            Param p;
            p.id = static_cast<unsigned char>(id);
            p.name = c[2];
            p.score = toI(c[3]);
            p.amp = toF(c[4]);
            if (c.size() > 5) p.up = (c[5] == "up");
            r.params.push_back(p);
        } else if (kind == "expr") {
            if (c.size() < 2) { note(r, ln, "expr 列数不够", s); continue; }
            const int id = idOf(r, c[1]);
            if (id < 0) { note(r, ln, "expr 引用了未定义部位", c[1]); continue; }
            Expr e;
            e.id = static_cast<unsigned char>(id);
            e.pick = (c.size() > 2) ? c[2] : std::string();
            while (!e.pick.empty() && (e.pick.back() == ' ' || e.pick.back() == '\r')) e.pick.pop_back();
            e.hold = (c.size() > 3 && !c[3].empty()) ? std::atof(c[3].c_str()) : 0.0;
            r.exprs.push_back(e);
        } else if (kind == "display") {
            // display<TAB>部位1<TAB>部位2…（顺序 = 设置页从上到下）
            for (size_t i = 1; i < c.size(); ++i) {
                if (c[i].empty()) continue;
                const int id = idOf(r, c[i]);
                if (id < 0) { note(r, ln, "display 引用了未定义部位", c[i]); continue; }
                r.display.push_back(static_cast<unsigned char>(id));
            }
        } else if (kind == "follow") {
            if (c.size() < 6) { note(r, ln, "follow 列数不够", s); continue; }
            const int id = idOf(r, c[1]);
            if (id < 0) { note(r, ln, "follow 引用了未定义部位", c[1]); continue; }
            Follow fo;
            fo.id = static_cast<unsigned char>(id);
            fo.name = c[2];
            fo.gain = toF(c[3]);
            fo.clampv = toF(c[4]);
            fo.K = (c.size() > 6 && !c[6].empty()) ? toF(c[6]) : 0.0f;
            fo.C = (c.size() > 7 && !c[7].empty()) ? toF(c[7]) : 0.0f;
            fo.src = (c[5].size() > 1) ? c[5][1] : 'x';   // "ax" → 'x'
            r.follows.push_back(fo);
        } else if (c.size() == 1) {
            // 手改表最常见的错：用空格分隔。报一句人话，别让人对着「认不出记录类型」猜
            note(r, ln, "这一行没有 TAB（是不是用空格分隔了？）", kind);
        } else {
            note(r, ln, "认不出记录类型", kind);
        }
    }
    std::fclose(f);

    // 顺序即语义：band 大 prio 先（稳定 → 同 prio 保持文件顺序，= 现状「后写的赢」由
    // 遍历顺序决定，这里先排大的，同 prio 里文件靠前的先应用、靠后的最后应用并覆盖）；
    // param 分高先。
    std::stable_sort(r.bands.begin(), r.bands.end(),
                     [](const Band& a, const Band& b) { return a.prio > b.prio; });
    std::stable_sort(r.params.begin(), r.params.end(),
                     [](const Param& a, const Param& b) { return a.score > b.score; });

    r.ok = !r.parts.empty();
    std::fprintf(stderr, "[partapi] %s：部位 %zu / 带 %zu / 参数 %zu / 表情 %zu / 跟随 %zu%s\n",
                 r.file.c_str(), r.parts.size(), r.bands.size(), r.params.size(),
                 r.exprs.size(), r.follows.size(), r.bad ? "（有坏行）" : "");
    if (r.bad) std::fprintf(stderr, "[partapi] 坏行 %d 条：%s\n", r.bad, r.notes.c_str());
    if (!r.ok)
        std::fprintf(stderr, "[partapi] parts.tsv 里一个部位都没有 —— 点击回整套反应\n");
}

inline void loadZones(Registry& r) {
    r.zoneFile = findFile("part-zones.tsv", "MIYU_ZONES_FILE");
    FILE* f = std::fopen(r.zoneFile.c_str(), "rb");
    if (!f) return;   // 格子文件没有也能跑（classify 还有 any 带 + 空）
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        chomp(s);
        if (s.empty()) continue;
        if (s[0] == '#') {
            if (s.rfind("#grid", 0) == 0) {
                std::vector<std::string> c = splitTab(s);
                if (c.size() >= 3) {
                    const int gw = toI(c[1]), gh = toI(c[2]);
                    if (gw > 0 && gh > 0) { r.zoneGw = gw; r.zoneGh = gh; }
                }
            }
            continue;
        }
        std::vector<std::string> c = splitTab(s);
        dropComment(c);
        if (c.empty() || c[0] != "Z" || c.size() < 6) continue;
        const int id = idOf(r, c[5]);
        if (id < 0) continue;
        Zone z;
        z.v0 = toI(c[1]); z.v1 = toI(c[2]);
        z.u0 = toI(c[3]); z.u1 = toI(c[4]);
        z.id = static_cast<unsigned char>(id);
        r.zones.push_back(z);
    }
    std::fclose(f);
}

inline const Registry& reg() {
    static Registry r;
    if (r.loaded) return r;
    r.loaded = true;
    loadParts(r);
    loadZones(r);
    return r;
}

// ── 格子图（渲染层每帧刷，平台层只读；和 partpoke::gZoneCells 同一个约定）──
inline const unsigned char* gGrid = nullptr;
inline int gGridW = 0, gGridH = 0;
inline void setGrid(const unsigned char* g, int w, int h) {
    gGrid = g; gGridW = w; gGridH = h;
}

// ── 查询 ────────────────────────────────────────────────────────────
inline int count() { return static_cast<int>(reg().parts.size()); }

inline const char* nameOf(int id) {
    const Registry& r = reg();
    if (id <= 0 || id > static_cast<int>(r.parts.size())) return "";
    return r.parts[static_cast<size_t>(id) - 1].name.c_str();
}

inline const char* labelOf(int id) {
    const Registry& r = reg();
    if (id <= 0 || id > static_cast<int>(r.parts.size())) return "";
    return r.parts[static_cast<size_t>(id) - 1].label.c_str();
}

inline int indexOf(const std::string& name) {
    if (name.empty()) return -1;
    return idOf(reg(), name);
}

inline double holdOf(int id) {
    const Registry& r = reg();
    if (id <= 0 || id > static_cast<int>(r.parts.size())) return 5.0;
    return r.parts[static_cast<size_t>(id) - 1].hold;
}

inline bool dragOnly(int id) {
    const Registry& r = reg();
    if (id <= 0 || id > static_cast<int>(r.parts.size())) return false;
    return r.parts[static_cast<size_t>(id) - 1].dragOnly;
}

// drawable 的 part 名 → 部位 id（子串匹配，文件顺序 = 优先级；0 = 认不出）
inline int indexOfAlias(const std::string& partName) {
    if (partName.empty()) return 0;
    for (const Alias& a : reg().aliases)
        if (partName.find(a.kw) != std::string::npos) return a.id;
    return 0;
}

// 这个部位有没有动作参数（0..N 条 param）
inline bool hasParams(int id) {
    if (id <= 0) return false;
    for (const Param& p : reg().params) if (p.id == id) return true;
    return false;
}
inline bool hasParams(const std::string& name) { return hasParams(indexOf(name)); }

// 这个部位有没有非空表情（expr 行第二列不是空）
inline bool hasExpr(int id) {
    if (id <= 0) return false;
    for (const Expr& e : reg().exprs) if (e.id == id && !e.pick.empty()) return true;
    return false;
}
inline bool hasExpr(const std::string& name) { return hasExpr(indexOf(name)); }

// 「点下去有反应」的唯一判据：有动作参数**或**有表情。
// 取代 platform_wayland.cpp 里那段 `hasParams(part) || hasExpr(part)` 的现场拼装（Step 5 换过去）。
inline bool hasAction(int id) { return hasParams(id) || hasExpr(id); }

inline bool hasAction(const std::string& name) { return hasAction(indexOf(name)); }

// 这个部位比值最高的前 n 个根（params 已按 score 降序）
inline std::vector<Param> top(int id, int n) {
    std::vector<Param> out;
    if (id <= 0 || n <= 0) return out;
    for (const Param& p : reg().params) {
        if (p.id != id) continue;
        out.push_back(p);
        if (static_cast<int>(out.size()) >= n) break;
    }
    return out;
}

inline std::vector<Band> bandsOf(int id) {
    std::vector<Band> out;
    for (const Band& b : reg().bands) if (b.id == id) out.push_back(b);
    return out;
}

// ── 判定：部位名（唯一入口，平台层调）────────────────────────────────
// 判定：返回**部位 id**（0 = 空白）。这是真正的入口 —— 冷却、hasAction、
// 日志全都按 id，名字只在打日志时用（Step 5：名字以后可能改，id 不会）。
inline int classifyId(float u, float v) {
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return 0;
    const Registry& r = reg();

    // ① scope=any 的带先判（原文是 classify 第一步的 ear 硬判，无视剪影）
    for (const Band& b : r.bands) {
        if (!b.any) continue;
        if (v >= b.v0 && v <= b.v1 && u >= b.u0 && u <= b.u1)
            return static_cast<int>(b.id);
    }

    // ② 渲染层每帧刷出来的格子图
    if (gGrid && gGridW > 0 && gGridH > 0) {
        int gu = static_cast<int>(u * static_cast<float>(gGridW));
        int gv = static_cast<int>(v * static_cast<float>(gGridH));
        if (gu >= gGridW) gu = gGridW - 1;
        if (gv >= gGridH) gv = gGridH - 1;
        const unsigned char c0 = gGrid[static_cast<size_t>(gv) * gGridW + gu];
        if (c0 != 0) return static_cast<int>(c0);
        // 边缘兜底：5×5 邻域里最近的已标格（照抄 partpoke.h:215-230）
        int bestD = 99;
        unsigned char best = 0;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const int x2 = gu + dx, y2 = gv + dy;
                if (x2 < 0 || y2 < 0 || x2 >= gGridW || y2 >= gGridH) continue;
                const unsigned char c = gGrid[static_cast<size_t>(y2) * gGridW + x2];
                if (c == 0) continue;
                const int d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                if (d < bestD) { bestD = d; best = c; }
            }
        if (best) return static_cast<int>(best);
        return 0;
    }

    // ③ 文件里的 Z 行（半开区间）。真机上走不到这里 —— 只有自检/工具没有内存格子图时才用
    const int gw = r.zoneGw, gh = r.zoneGh;
    const int gu = static_cast<int>(u * static_cast<float>(gw));
    const int gv = static_cast<int>(v * static_cast<float>(gh));
    for (const Zone& z : r.zones)
        if (gv >= z.v0 && gv < z.v1 && gu >= z.u0 && gu < z.u1)
            return static_cast<int>(z.id);
    return 0;
}

// 兼容壳：名字版（日志、调试、旧调用点用）
inline std::string classify(float u, float v) {
    const int id = classifyId(u, v);
    return id > 0 ? std::string(nameOf(id)) : std::string();
}

// ── 判定带应用到格子图（渲染层，每帧一次；alpha 白名单之后、泛洪补洞之前）──
// 返回应用的条数（日志用）。ok 可以为 nullptr = 不做剪影限制（调试用）。
inline int applyBands(std::vector<unsigned char>& grid, int gw, int gh,
                      const std::vector<unsigned char>* ok) {
    const Registry& r = reg();
    if (gw <= 0 || gh <= 0 || grid.size() != static_cast<size_t>(gw) * static_cast<size_t>(gh))
        return 0;
    int applied = 0;
    for (const Band& b : r.bands) {
        if (b.any) continue;      // scope=any 只在点击判定里用，不进格子
        const int gy0 = static_cast<int>(b.v0 * static_cast<float>(gh));
        const int gy1 = static_cast<int>(b.v1 * static_cast<float>(gh));
        const int gx0 = static_cast<int>(b.u0 * static_cast<float>(gw));
        const int gx1 = static_cast<int>(b.u1 * static_cast<float>(gw));
        for (int gy = gy0; gy <= gy1 && gy < gh; ++gy)
            for (int gx = gx0; gx <= gx1 && gx < gw; ++gx) {
                if (gy < 0 || gx < 0) continue;
                const size_t i = static_cast<size_t>(gy) * static_cast<size_t>(gw) +
                                 static_cast<size_t>(gx);
                if (ok && i < ok->size() && !(*ok)[i]) continue;   // 剪影外不动
                if (!b.force && grid[i]) continue;    // 补白：不抢已有标签
                grid[i] = b.id;
            }
        ++applied;
    }
    return applied;
}

// ── 表情（热的那一项）──────────────────────────────────────────────
// pick 里 `/` 分隔 = 每次随机抽一个；同一部位多条 expr 行 = 合并成候选池。
// hold = expr 行自己的值（>0）否则用 part 行的 hold。
// ── 表情覆盖层（Step 5）─────────────────────────────────────────────
// 为什么要有这一层：网页改一个部位的表情要**立刻生效**，不能等重启。
//   · 文件（parts.tsv 的 expr 段）是唯一真源，重启后的事实
//   · 这层 map 只在「本次进程里被网页改过的部位」上有值，生命周期 = 进程
//   · 落盘和覆盖同时写同一个值，所以重启前后一致
// 这样就不会出现「文件一份、内存一份、谁先谁后」的老问题 —— 覆盖层是**同一份数据
// 的未落盘版本**，不是第二份数据。
inline std::map<unsigned char, std::string>& exprOverlay() {
    static std::map<unsigned char, std::string> m;
    return m;
}

inline void setExprOverride(int id, const std::string& val) {
    if (id <= 0) return;
    // 空串是**显式**的「这个部位不放表情」，不是「删掉覆盖、回落到文件里的值」——
    // 网页上把下拉选回「无」必须真的把 parts.tsv 那行写成空，否则清不掉。
    // （踩过：早先 erase 掉覆盖，结果落回注册表里文件原有的值，删不掉）
    exprOverlay()[static_cast<unsigned char>(id)] = val;
}

inline void clearExprOverlay() { exprOverlay().clear(); }

// 某个部位当前该播什么（覆盖层优先，否则合并注册表里的所有 expr 行，用 `/` 连接）
inline std::string exprValueOf(int id) {
    if (id <= 0) return std::string();
    const auto it = exprOverlay().find(static_cast<unsigned char>(id));
    if (it != exprOverlay().end()) return it->second;
    std::string out;
    for (const Expr& e : reg().exprs) {
        if (e.id != id || e.pick.empty()) continue;
        if (!out.empty()) out += '/';
        out += e.pick;
    }
    return out;
}

inline bool pickExpr(int id, unsigned seed, std::string* out, double* holdOut) {
    if (id <= 0) return false;
    const Registry& r = reg();
    std::vector<std::string> pool;
    double hold = holdOf(id);
    // 覆盖层：网页改过的值直接当候选池（可能还是 a/b/c 的形态）
    const auto ov = exprOverlay().find(static_cast<unsigned char>(id));
    if (ov != exprOverlay().end()) {
        const std::string& val = ov->second;
        if (val.empty()) return false;
        size_t p0 = 0;
        for (;;) {
            const size_t p1 = val.find('/', p0);
            const std::string one = val.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
            if (!one.empty()) pool.push_back(one);
            if (p1 == std::string::npos) break;
            p0 = p1 + 1;
        }
        if (pool.empty()) return false;
        if (out) *out = pool[seed % pool.size()];
        if (holdOut) *holdOut = hold;
        return true;
    }
    for (const Expr& e : r.exprs) {
        if (e.id != id) continue;
        if (e.hold > 0) hold = e.hold;
        if (e.pick.empty()) continue;
        size_t p0 = 0;
        for (;;) {
            const size_t p1 = e.pick.find('/', p0);
            std::string one = e.pick.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
            if (!one.empty()) pool.push_back(one);
            if (p1 == std::string::npos) break;
            p0 = p1 + 1;
        }
    }
    if (pool.empty()) return false;
    if (out) *out = pool[seed % pool.size()];
    if (holdOut) *holdOut = hold;
    return true;
}

// ── 落盘：只重写 expr 段（全套里唯一有写入风险的路径）──────────────
// 四条规矩，少一条就会吃掉别人手写的配置：
//   ① 非 expr 行**逐字节原样保留**（注释、空行、行尾空白、\r 都算）
//   ② 新 expr 行写回原位置（原来按什么顺序排就还什么顺序，新部位追加到末尾）
//   ③ 先写 .tmp，再**逐行 diff 断言**：两边去掉 expr 行后必须完全相等，且新 expr 行
//      每一行的部位名都在注册表里、值里没有 TAB
//   ④ 断言通过才 rename；不通过删 tmp、原文件一个字节都没动、返回 false
inline std::vector<std::string> readLinesRaw(const std::string& path, bool* ok) {
    std::vector<std::string> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (ok) *ok = false; return out; }
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t p = 0;
    while (p < all.size()) {
        const size_t q = all.find('\n', p);
        if (q == std::string::npos) { out.push_back(all.substr(p)); break; }
        out.push_back(all.substr(p, q - p + 1));      // 含 '\n'
        p = q + 1;
    }
    if (ok) *ok = true;
    return out;
}

inline bool isExprLine(const std::string& line) {
    return line.compare(0, 5, "expr\t") == 0;
}

// "expr\t<部位>\t<值>\n" → 部位名（不含 TAB 之前/之后的东西）
inline std::string exprLinePart(const std::string& line) {
    const size_t a = line.find('\t');
    if (a == std::string::npos) return std::string();
    const size_t b = line.find('\t', a + 1);
    std::string s = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

inline bool saveExprRows(const std::string& path, std::string* err) {
    auto fail = [&](const char* why) {
        if (err) *err = why;
        return false;
    };
    const Registry& r = reg();
    if (!r.ok) return fail("注册表没读到部位，拒绝写（免得把文件写成空的）");

    bool ok = false;
    const std::vector<std::string> oldLines = readLinesRaw(path, &ok);
    if (!ok) return fail("读不到原文件");

    // ① 找出原 expr 行的位置和顺序
    size_t firstExpr = std::string::npos;
    std::vector<std::string> order;
    for (size_t i = 0; i < oldLines.size(); ++i) {
        if (!isExprLine(oldLines[i])) continue;
        if (firstExpr == std::string::npos) firstExpr = i;
        const std::string nm = exprLinePart(oldLines[i]);
        if (!nm.empty()) order.push_back(nm);
    }
    // 原顺序 + 注册表里还没出现过的部位（新部位追加到末尾）
    std::vector<int> ids;
    for (const std::string& nm : order) {
        const int id = idOf(r, nm);
        if (id > 0) ids.push_back(id);
    }
    for (size_t i = 0; i < r.parts.size(); ++i) {
        const int id = static_cast<int>(i) + 1;
        bool seen = false;
        for (int x : ids) if (x == id) { seen = true; break; }
        if (seen) continue;
        // 原来没有 expr 行、现在也没有值的部位，不凭空造一行空 expr
        // （否则每写一次文件就多 13 行空行，diff 全花，幂等也破了）
        if (exprValueOf(id).empty()) continue;
        ids.push_back(id);
    }

    // ② 生成新 expr 行
    std::vector<std::string> newExpr;
    for (int id : ids) {
        const std::string val = exprValueOf(id);
        if (val.find('\t') != std::string::npos) return fail("表情名里有 TAB，拒绝写");
        newExpr.push_back("expr\t" + std::string(nameOf(id)) + "\t" + val + "\n");
    }

    // ③ 拼新文件：非 expr 行原样，expr 块写回原位置
    std::vector<std::string> out;
    bool inserted = false;
    for (size_t i = 0; i < oldLines.size(); ++i) {
        if (isExprLine(oldLines[i])) {
            if (!inserted) {
                for (const std::string& l : newExpr) out.push_back(l);
                inserted = true;
            }
            continue;                       // 原 expr 行全部丢掉（已由 newExpr 取代）
        }
        out.push_back(oldLines[i]);
    }
    if (!inserted) {
        // 原来一条 expr 行都没有：追加到末尾（保证前一行以 '\n' 结尾）
        if (!out.empty() && (out.back().empty() || out.back().back() != '\n')) out.back() += '\n';
        for (const std::string& l : newExpr) out.push_back(l);
    }

    // ④ 逐行 diff 断言：去掉 expr 行后，新旧必须完全相等
    std::vector<std::string> a, b;
    for (const std::string& l : oldLines) if (!isExprLine(l)) a.push_back(l);
    for (const std::string& l : out)      if (!isExprLine(l)) b.push_back(l);
    if (a != b) return fail("非 expr 行被动了，拒绝写");

    // ⑤ 写 tmp → 校验 → rename
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return fail("建不了 tmp");
        for (const std::string& l : out) f.write(l.data(), static_cast<std::streamsize>(l.size()));
        f.flush();
        if (!f) { std::remove(tmp.c_str()); return fail("写 tmp 失败"); }
    }
    // 读回来再断言一次（防写到一半被截断）
    bool ok2 = false;
    const std::vector<std::string> back = readLinesRaw(tmp, &ok2);
    if (!ok2 || back.size() != out.size()) { std::remove(tmp.c_str()); return fail("tmp 读回来行数不对"); }
    std::vector<std::string> c;
    for (const std::string& l : back) if (!isExprLine(l)) c.push_back(l);
    if (a != c) { std::remove(tmp.c_str()); return fail("tmp 的非 expr 行和原件不一致"); }

    std::remove(path.c_str());
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::rename(tmp.c_str(), path.c_str());     // 再来一次（同分区几乎不会失败）
        return fail("rename 失败（原文件已删，tmp 还在）");
    }
    return true;
}

// ── 自检（MIYU_PARTS_DUMP=1 时启动打印；格式和 tools/parts-dump.py 逐行一致）──
inline std::string dump() {
    const Registry& r = reg();
    std::string s;
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "parts=%zu bands=%zu params=%zu exprs=%zu follows=%zu bad=%d\n",
                  r.parts.size(), r.bands.size(), r.params.size(), r.exprs.size(),
                  r.follows.size(), r.bad);
    s += buf;
    for (size_t i = 0; i < r.parts.size(); ++i) {
        const Part& p = r.parts[i];
        std::snprintf(buf, sizeof buf, "PART %zu %s %s hold=%.1f flags=%s\n", i + 1,
                      p.name.c_str(), p.label.c_str(), p.hold, p.dragOnly ? "dragonly" : "-");
        s += buf;
    }
    if (!r.display.empty()) {
        s += "DISPLAY";
        for (unsigned char d : r.display) { s += " "; s += nameOf(d); }
        s += "\n";
    }
    for (const Alias& a : r.aliases) {
        std::snprintf(buf, sizeof buf, "ALIAS %d %s %s\n", static_cast<int>(a.id),
                      nameOf(a.id), a.kw.c_str());
        s += buf;
    }
    for (const Band& b : r.bands) {
        std::snprintf(buf, sizeof buf, "BAND %d %s %.3f %.3f %.3f %.3f %s prio=%d %s\n",
                      static_cast<int>(b.id), nameOf(b.id), b.u0, b.u1, b.v0, b.v1,
                      b.force ? "force" : "fill", b.prio, b.any ? "any" : "body");
        s += buf;
    }
    for (const Param& p : r.params) {
        std::snprintf(buf, sizeof buf, "PARAM %d %s %s score=%d amp=%.3f %s\n",
                      static_cast<int>(p.id), nameOf(p.id), p.name.c_str(), p.score, p.amp,
                      p.up ? "up" : "alt");
        s += buf;
    }
    for (const Expr& e : r.exprs) {
        if (e.hold > 0)
            std::snprintf(buf, sizeof buf, "EXPR %d %s %s hold=%.1f\n",
                          static_cast<int>(e.id), nameOf(e.id),
                          e.pick.empty() ? "-" : e.pick.c_str(), e.hold);
        else
            std::snprintf(buf, sizeof buf, "EXPR %d %s %s hold=-\n",
                          static_cast<int>(e.id), nameOf(e.id),
                          e.pick.empty() ? "-" : e.pick.c_str());
        s += buf;
    }
    for (const Follow& f : r.follows) {
        std::snprintf(buf, sizeof buf, "FOLLOW %d %s %s gain=%.4f clamp=%.1f src=a%c\n",
                      static_cast<int>(f.id), nameOf(f.id), f.name.c_str(), f.gain,
                      f.clampv, f.src);
        s += buf;
    }
    return s;
}

}  // namespace partapi
