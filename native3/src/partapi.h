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
        } else if (kind == "follow") {
            if (c.size() < 6) { note(r, ln, "follow 列数不够", s); continue; }
            const int id = idOf(r, c[1]);
            if (id < 0) { note(r, ln, "follow 引用了未定义部位", c[1]); continue; }
            Follow fo;
            fo.id = static_cast<unsigned char>(id);
            fo.name = c[2];
            fo.gain = toF(c[3]);
            fo.clampv = toF(c[4]);
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

// 「点下去有反应」的唯一判据：有动作参数**或**有表情（现在 main.cpp 里是 hasParams || hasExpr）
inline bool hasAction(int id) {
    if (id <= 0) return false;
    const Registry& r = reg();
    for (const Param& p : r.params) if (p.id == id) return true;
    for (const Expr& e : r.exprs) if (e.id == id && !e.pick.empty()) return true;
    return false;
}

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
inline std::string classify(float u, float v) {
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return std::string();
    const Registry& r = reg();

    // ① scope=any 的带先判（原文是 classify 第一步的 ear 硬判，无视剪影）
    for (const Band& b : r.bands) {
        if (!b.any) continue;
        if (v >= b.v0 && v <= b.v1 && u >= b.u0 && u <= b.u1)
            return std::string(nameOf(b.id));
    }

    // ② 渲染层每帧刷出来的格子图
    if (gGrid && gGridW > 0 && gGridH > 0) {
        int gu = static_cast<int>(u * static_cast<float>(gGridW));
        int gv = static_cast<int>(v * static_cast<float>(gGridH));
        if (gu >= gGridW) gu = gGridW - 1;
        if (gv >= gGridH) gv = gGridH - 1;
        const unsigned char c0 = gGrid[static_cast<size_t>(gv) * gGridW + gu];
        if (c0 != 0) return std::string(nameOf(c0));
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
        if (best) return std::string(nameOf(best));
        return std::string();
    }

    // ③ 文件里的 Z 行（半开区间）。真机上走不到这里 —— 只有自检/工具没有内存格子图时才用
    const int gw = r.zoneGw, gh = r.zoneGh;
    const int gu = static_cast<int>(u * static_cast<float>(gw));
    const int gv = static_cast<int>(v * static_cast<float>(gh));
    for (const Zone& z : r.zones)
        if (gv >= z.v0 && gv < z.v1 && gu >= z.u0 && gu < z.u1)
            return std::string(nameOf(z.id));
    return std::string();
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
inline bool pickExpr(int id, unsigned seed, std::string* out, double* holdOut) {
    if (id <= 0) return false;
    const Registry& r = reg();
    std::vector<std::string> pool;
    double hold = holdOf(id);
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
