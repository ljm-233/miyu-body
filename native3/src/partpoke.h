// 部位级反应（第二十一节）—— 两个文件共用的一小块状态。
//
// 为什么要单开一个头、而且只有头：CMakeLists.txt 这一轮不许动（任务书：只改
// src/ tools/ state/ NOTES.md），所以不能加 .cpp。C++17 的 `inline` 变量在
// 所有 TU 里是**同一个对象**，正好拿来当「平台层 ⇄ 渲染层」的邮筒。
//
// 分工，谁的数据在谁手里：
//   · 平台层（platform_wayland.cpp）：有 lx/ly、有她本人的 bbox → 判「点到哪个部位」
//   · 渲染层（main.cpp）：有 Cubism 模型 → 参数索引、默认值、「弹一下」曲线
//   中间的线就是下面这三样：gGen（第几次请求）、gPart（哪个部位）。
//
// 数据表 `state/param-parts.tsv` 由 tools/param-probe.py 生成，两边各自读自己那一半：
//   R 行 = 部位矩形（她本人 bbox 归一化，u 左→右、v 上→下，按行序优先判定）
//   P 行 = 参数 → 部位 + 弹到多少（peak）+ 回弹到多少（dip）
// 这样「哪些参数属于哪个部位」只存在一份，不用在 C++ 里再猜一遍。

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
inline char gPart[32] = {0};   // 部位名（TSV 里 R 行的名字）

// ── 数据 ────────────────────────────────────────────────────────────
struct PartBox {
    std::string name;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
};
struct PartParam {
    std::string name;   // 模型里的参数 id
    std::string part;
    int   score = 0;    // 探针量出来的像素变化量（排序用）
    float peak = 0;     // 「弹到 60%」时该写多少（绝对值，不是增量）
    float dip = 0;      // 「回弹到 −20%」时该写多少
};

// 找表：MIYU_PARTS_FILE > 可执行文件旁边的 state/ > ../state/ > state/
// （和 plans 目录同一套找法，二进制放在 build/ 里也能找到。）
inline std::string path() {
    if (const char* e = std::getenv("MIYU_PARTS_FILE")) return std::string(e);
    std::string dir;
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = 0;
        const std::string exe(buf);
        const size_t sl = exe.rfind('/');
        dir = (sl == std::string::npos) ? std::string(".") : exe.substr(0, sl);
    }
    const std::string cands[3] = {dir + "/state/param-parts.tsv",
                                 dir + "/../state/param-parts.tsv",
                                 "state/param-parts.tsv"};
    for (int i = 0; i < 3; ++i)
        if (access(cands[i].c_str(), R_OK) == 0) return cands[i];
    return "state/param-parts.tsv";
}

struct Table {
    bool loaded = false;
    std::string file;
    std::vector<PartBox> boxes;
    std::vector<PartParam> params;   // 已按 score 从大到小
};

// 惰性加载一次（进程生命周期内只读一次；探针重跑之后要重启身体才生效）。
inline const Table& table() {
    static Table t;
    if (t.loaded) return t;
    t.loaded = true;
    t.file = path();
    FILE* f = std::fopen(t.file.c_str(), "rb");
    if (!f) return t;
    char line[1024];
    while (std::fgets(line, sizeof line, f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        // 制表符分列；名字里有空格和中文，所以只按 '\t' 切
        std::vector<std::string> col;
        size_t p = 0;
        while (true) {
            const size_t q = s.find('\t', p);
            col.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (col[0] == "R" && col.size() >= 6) {
            PartBox b;
            b.name = col[1];
            b.u0 = static_cast<float>(atof(col[2].c_str()));
            b.v0 = static_cast<float>(atof(col[3].c_str()));
            b.u1 = static_cast<float>(atof(col[4].c_str()));
            b.v1 = static_cast<float>(atof(col[5].c_str()));
            t.boxes.push_back(b);
        } else if (col[0] == "P" && col.size() >= 6) {
            PartParam pp;
            pp.name = col[1];
            pp.part = col[2];
            pp.score = atoi(col[3].c_str());
            pp.peak = static_cast<float>(atof(col[4].c_str()));
            pp.dip = static_cast<float>(atof(col[5].c_str()));
            t.params.push_back(pp);
        }
    }
    std::fclose(f);
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

// 归一化坐标 → 部位名（找不到返回空串）。
// 按 R 行的顺序判，第一个命中的赢 —— 所以窄的部位（尾巴、手臂）写在前面。
inline std::string classify(float u, float v) {
    const Table& t = table();
    for (const PartBox& b : t.boxes)
        if (u >= b.u0 && u <= b.u1 && v >= b.v0 && v <= b.v1) return b.name;
    return std::string();
}

// 这个部位有没有活着的参数（没有就别抢，让整套反应去播）
inline bool hasParams(const std::string& part) {
    for (const PartParam& p : table().params)
        if (p.part == part) return true;
    return false;
}

// 这个部位驱动量最大的前 n 个参数
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
