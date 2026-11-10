// 部位级反应（第二十一节 → 二十二节重做）—— 两个文件共用的一小块状态。
//
// 为什么要单开一个头、而且只有头：CMakeLists.txt 这一轮不许动（任务书：只改
// src/ tools/ state/ NOTES.md），所以不能加 .cpp。C++17 的 `inline` 变量在
// 所有 TU 里是**同一个对象**，正好拿来当「平台层 ⇄ 渲染层」的邮筒。
//
// 分工，谁的数据在谁手里：
//   · 平台层（platform_wayland.cpp）：有 lx/ly → 查格子图，判「点到哪个部位」
//   · 渲染层（main.cpp）：有 Cubism 模型 → 参数索引、默认值、弹簧积分
//   中间的线就是下面这三样：gGen（第几次请求）、gPart（哪个部位）。
//
// 二十二节的两处改动（他说的「去学学自带的那些文件」）：
//   ① **判定改成按剪影**：不再用「她本人 bbox 里切 7 个矩形」。bbox 被右边那条
//      大尾巴撑宽，横向全偏，点胸口会被判成手臂。现在读 `state/part-zones.tsv`
//      的格子图（tools/part-probe.py 量出来的：拨哪个叶子参数，哪些格子里的像素动了）。
//   ② **只拨根**：P 行给的不再是 ChestX1/SkirtY3 这种 **物理输出**，而是
//      physics3.json 里只被输入、从不被输出的**根**（PositionX / ParamBreath /
//      AngleX…）。往输出里写数 = 顶掉物理算出来的结果，形状会发尖；拨根才是让
//      模型自己的物理把这块带起来。清单在 `state/physics-map.json` 的 part_roots。
//
// 两份数据的格式（都由 tools/ 生成，C++ 只读）：
//   state/part-zones.tsv
//       #grid<TAB>60<TAB>120           格子表尺寸（不写就按 60x120）
//       Z<TAB>v0<TAB>v1<TAB>u0<TAB>u1<TAB>部位     格子下标区间，u 右 v 下
//       不在任何 Z 行里 = 空白（剪影外或没量到的格子）→ 交回整套反应
//   state/param-parts.tsv
//       P<TAB>部位<TAB>根参数<TAB>比值×10<TAB>冲量幅度<TAB>备注
//       幅度 = 绝对偏移量（度 / 比例单位），正负每点一次交替

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace partpoke {

// ── 邮筒 ────────────────────────────────────────────────────────────
inline int  gGen = 0;          // 0 = 还没点过；每点一次 +1
inline char gPart[32] = {0};   // 部位名（part-zones.tsv 里的名字）

// ── 格子图 ──────────────────────────────────────────────────────────
// 二十四：60×120 → 120×240。一格从 8.3×7.5 px 变成 4.2×3.75 px，因为脸要拆细：
// 眉毛（12px 高）、眼睛（22px）、嘴（10px）三个在 60×120 里各自只有 1–2 行格子，
// 分不开 —— 眉毛一动就整片算到眼睛头上（实测 ParamBrowLY3 154px 里 114px 落进眼睛格子）。
// 网格放在文件里（#grid 那一行），工具读它，判定也读它，所以改这里 + 删旧文件即可。
inline int gGridW = 120;
inline int gGridH = 240;

// 二十二：格子图由**渲染层**每帧刷（main.cpp 用模型自带的 drawable → part 分区
// 光栅化出来的，见 partZoneTick）。平台层只读。0 = 空白（她身上没东西）。

// 二十四（2026-11-10）：8 → 14。脸拆成眼睛 / 嘴 / 眉毛，另加兽耳 / 腰 / 头饰。
// 下标已经写进格子图（grid 里存的就是这些数字），只能往后加，不能插队、不能重排。
inline const char* const kZoneNames[14] = {
    "", "head", "chest", "arm", "skirt", "leg", "tail", "other",
    "eye", "mouth", "brow", "ear", "waist", "headwear"};
inline const int kZoneCount = 14;

// 40×80 的标签格，值 = kZoneNames 的下标；空 vector = 还没算出来
inline std::vector<unsigned char> gZoneCells;
inline unsigned long gZoneStamp = 0;   // 每刷一次 +1（调试用）
inline int gZoneWinW = 0, gZoneWinH = 0;   // 刷这张图时的窗口逻辑尺寸

struct Zone {
    int v0 = 0, v1 = 0, u0 = 0, u1 = 0;   // 格子下标，左闭右开
    std::string part;
};

// ── 参数 ────────────────────────────────────────────────────────────
struct PartParam {
    std::string name;   // 模型里的参数 id（**根输入**）
    std::string part;
    int   score = 0;    // 比值×10（探针量出来的「该部位动 / 别的部位最多动」）
    float amp = 0;      // 冲量幅度（绝对偏移量，不是百分比）
};

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

// 找表：环境变量 > 可执行文件旁边的 state/ > ../state/ > state/
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

inline std::string path() { return findFile("param-parts.tsv", "MIYU_PARTS_FILE"); }
inline std::string zonePath() { return findFile("part-zones.tsv", "MIYU_ZONES_FILE"); }

// 制表符分列（名字里有空格和中文，所以只按 '\t' 切）
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

struct Table {
    bool loaded = false;
    std::string file, zfile;
    std::vector<Zone> zones;
    std::vector<PartParam> params;   // 已按 score 从大到小
};

// 惰性加载一次（进程生命周期内只读一次；探针重跑之后要重启身体才生效）。
inline const Table& table() {
    static Table t;
    if (t.loaded) return t;
    t.loaded = true;

    // ① 格子图
    t.zfile = zonePath();
    if (FILE* fz = std::fopen(t.zfile.c_str(), "rb")) {
        char line[1024];
        while (std::fgets(line, sizeof line, fz)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty()) continue;
            if (s[0] == '#') {
                if (s.rfind("#grid", 0) == 0) {          // #grid<TAB>40<TAB>80
                    const std::vector<std::string> c = splitTab(s);
                    if (c.size() >= 3) {
                        const int gw = atoi(c[1].c_str()), gh = atoi(c[2].c_str());
                        if (gw > 0 && gh > 0) { gGridW = gw; gGridH = gh; }
                    }
                }
                continue;
            }
            const std::vector<std::string> c = splitTab(s);
            if (c[0] == "Z" && c.size() >= 6) {
                Zone z;
                z.v0 = atoi(c[1].c_str());
                z.v1 = atoi(c[2].c_str());
                z.u0 = atoi(c[3].c_str());
                z.u1 = atoi(c[4].c_str());
                z.part = c[5];
                t.zones.push_back(z);
            }
        }
        std::fclose(fz);
    }

    // ② 部位 → 根参数
    t.file = path();
    if (FILE* f = std::fopen(t.file.c_str(), "rb")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            const std::vector<std::string> c = splitTab(s);
            if (c[0] == "P" && c.size() >= 5) {
                PartParam pp;
                pp.part = c[1];
                pp.name = c[2];
                pp.score = atoi(c[3].c_str());
                pp.amp = static_cast<float>(atof(c[4].c_str()));
                t.params.push_back(pp);
            }
        }
        std::fclose(f);
    }
    // 稳定排序：分高的在前；同分按名字，保证每次点的效果一样
    for (size_t i = 0; i + 1 < t.params.size(); ++i)
        for (size_t j = 0; j + 1 < t.params.size() - i; ++j)
            if (t.params[j + 1].score > t.params[j].score) {
                const PartParam tmp = t.params[j];
                t.params[j] = t.params[j + 1];
                t.params[j + 1] = tmp;
            }
    return t;
}

// 窗口归一化坐标（u = lx/窗口宽, v = ly/窗口高）→ 部位名；空白返回空串。
// 优先用渲染层刷出来的格子图（模型自带分区）；还没刷出来时退回文件里的格子。
inline std::string classify(float u, float v) {
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) return std::string();
    int gu = static_cast<int>(u * static_cast<float>(gGridW));
    int gv = static_cast<int>(v * static_cast<float>(gGridH));
    if (gu >= gGridW) gu = gGridW - 1;
    if (gv >= gGridH) gv = gGridH - 1;
    if (gZoneCells.size() == static_cast<size_t>(gGridW) * static_cast<size_t>(gGridH)) {
        const unsigned char c = gZoneCells[static_cast<size_t>(gv) * gGridW + gu];
        if (c == 0 || c >= kZoneCount) return std::string();
        return std::string(kZoneNames[c]);
    }
    const Table& t = table();
    for (const Zone& z : t.zones)
        if (gv >= z.v0 && gv < z.v1 && gu >= z.u0 && gu < z.u1) return z.part;
    return std::string();
}

// 这个部位有没有活着的根参数（没有就别抢，让整套反应去播）
inline bool hasParams(const std::string& part) {
    for (const PartParam& p : table().params)
        if (p.part == part) return true;
    return false;
}

// 这个部位比值最高的前 n 个根
inline std::vector<PartParam> top(const std::string& part, int n) {
    std::vector<PartParam> out;
    for (const PartParam& p : table().params) {
        if (p.part != part) continue;
        out.push_back(p);
        if (static_cast<int>(out.size()) >= n) break;
    }
    return out;
}

}  // namespace partpoke
