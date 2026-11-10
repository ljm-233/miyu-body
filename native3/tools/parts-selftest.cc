// 部位 API 的自检程序（Step 2 用）：不起身体、不开窗口、不碰 layer surface。
//
//   g++ -std=c++17 -Wall -Wextra -O1 -I src -o /tmp/parts-selftest tools/parts-selftest.cc
//   cd ~/.dsh/miyu/body/native3 && /tmp/parts-selftest
//
//   /tmp/parts-selftest --dump   # 只打注册表（给 tools/parts-dump.py 对账用）
//
// 为什么单独一个程序而不是在 main.cpp 里挂钩子：main.cpp 一改就得整套编译，
// 而这套东西只需要 partapi.h —— 一条 g++ 命令 1 秒出结果，也不碰正在跑的二进制。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "partapi.h"

namespace {

int gFail = 0;

void check(bool cond, const char* what) {
    std::printf("  %s  %s\n", cond ? "[ok]  " : "[FAIL]", what);
    if (!cond) ++gFail;
}

void checkEq(int got, int want, const char* what) {
    const bool ok = (got == want);
    std::printf("  %s  %-34s got=%d want=%d\n", ok ? "[ok]  " : "[FAIL]", what, got, want);
    if (!ok) ++gFail;
}

// 她窗口 500×900（state/window.json 的 w/h），下面是验证过的 11 个点（窗口内坐标）
struct Pt { const char* label; int x, y; const char* want; };

const Pt kPts[] = {
    {"头",     247, 150, nullptr},
    {"头饰",   215, 140, nullptr},
    {"兽耳",   180, 205, "ear"},     // scope=any 的硬判带，必须对
    {"眼",     247, 193, nullptr},
    {"眉",     252, 174, nullptr},
    {"嘴",     252, 230, nullptr},
    {"胸",     247, 294, nullptr},
    {"腰",     235, 414, nullptr},
    {"右飘带", 370, 535, nullptr},
    {"左飘带", 175, 535, nullptr},
    {"腿",     240, 650, nullptr},
};

}  // namespace

int main(int argc, char** argv) {
    const bool dumpOnly = (argc > 1 && std::strcmp(argv[1], "--dump") == 0);

    if (dumpOnly) {
        std::fputs(partapi::dump().c_str(), stdout);
        return partapi::reg().ok ? 0 : 1;
    }

    std::printf("── 注册表 ─────────────────────────────────────────\n");
    std::printf("%s", partapi::dump().c_str());

    std::printf("\n── 结构断言 ───────────────────────────────────────\n");
    check(partapi::reg().ok, "parts.tsv 读到了部位");
    checkEq(partapi::reg().bad, 0, "坏行数");
    checkEq(partapi::count(), 13, "部位数");
    checkEq(partapi::indexOf("head"), 1, "id(head)");
    checkEq(partapi::indexOf("headwear"), 13, "id(headwear)");
    checkEq(partapi::indexOf("other"), 7, "id(other)");
    check(std::strcmp(partapi::nameOf(7), "other") == 0, "nameOf(7) == other");
    check(std::strcmp(partapi::labelOf(13), "头饰") == 0, "labelOf(13) == 头饰");
    checkEq(static_cast<int>(partapi::bandsOf(11).size()), 4, "ear 的带数（2 条 any + 2 条 body）");
    checkEq(static_cast<int>(partapi::bandsOf(2).size()), 0, "chest 的带数（0 条，靠 drawable）");
    checkEq(static_cast<int>(partapi::top(2, 4).size()), 1, "chest 的根参数个数");
    check(partapi::top(2, 4).at(0).amp == 1.0f, "PokeChest 幅度 1.000");
    check(!partapi::top(2, 4).at(0).up, "PokeChest 方向 alt");
    checkEq(static_cast<int>(partapi::top(1, 4).size()), 0, "head 的根参数个数（二十五拿掉了）");
    check(partapi::hasAction("head"), "head 有动作（靠表情，不靠参数）");
    check(partapi::hasAction("arm"), "arm 有动作");
    check(!partapi::hasAction("other"), "other 没有动作 → 退回整套反应");
    check(partapi::holdOf(1) == 5.0, "hold(head) == 5.0");

    std::printf("\n── 表情 ───────────────────────────────────────────\n");
    {
        std::string ex;
        double hold = 0;
        const bool ok = partapi::pickExpr(partapi::indexOf("mouth"), 0, &ex, &hold);
        std::printf("  %s  mouth → %s（hold %.1f）\n", ok ? "[ok]  " : "[FAIL]",
                    ok ? ex.c_str() : "(无)", hold);
        if (!ok || ex != "＞＜") ++gFail;
        check(!partapi::pickExpr(partapi::indexOf("arm"), 0, nullptr, nullptr),
              "arm 不放表情（空行 = 只放动作）");
        std::string h;
        check(partapi::pickExpr(partapi::indexOf("head"), 0, &h, nullptr) && h == "前倾",
              "head → 前倾");
    }

    std::printf("\n── alias（drawable 的 part 名 → 部位）──────────────\n");
    {
        struct { const char* in; int want; } cases[] = {
            {"女仆发饰A", 13}, {"发饰/蝴蝶结", 13}, {"眼镜框", 1}, {"左眼球", 8},
            {"眉毛", 10}, {"腰部衣服", 12}, {"尾巴A", 6}, {"短裙", 4}, {"LEG", 0},
            {"组12", 0}, {"", 0},
        };
        for (const auto& c : cases) {
            const int got = partapi::indexOfAlias(c.in);
            const bool ok = (got == c.want);
            std::printf("  %s  %-12s → %d（want %d）\n", ok ? "[ok]  " : "[FAIL]", c.in, got, c.want);
            if (!ok) ++gFail;
        }
    }

    std::printf("\n── 判定带进格子图 ─────────────────────────────────\n");
    {
        const int gw = 120, gh = 240;
        std::vector<unsigned char> grid(static_cast<size_t>(gw) * gh, 0);
        std::vector<bool> ok(grid.size(), true);
        const int applied = partapi::applyBands(grid, gw, gh, &ok);
        checkEq(applied, 11, "应用了 11 条 body 带（any 那两条不进格子）");
        // 抽查几格：眼睛带 u .415-.590 / v .208-.228
        const int x = static_cast<int>(0.494f * gw), y = static_cast<int>(0.214f * gh);
        checkEq(static_cast<int>(grid[static_cast<size_t>(y) * gw + static_cast<size_t>(x)]), 8,
                "格(59,51) = eye(8)");
        // 补白：先在 head 带里放一个别的标签，fill 不该盖它
        std::vector<unsigned char> g2(grid);
        const int hx = static_cast<int>(0.450f * gw), hy = static_cast<int>(0.120f * gh);
        g2[static_cast<size_t>(hy) * gw + static_cast<size_t>(hx)] = 6;   // tail
        std::vector<bool> ok2(g2.size(), true);
        partapi::applyBands(g2, gw, gh, &ok2);
        checkEq(static_cast<int>(g2[static_cast<size_t>(hy) * gw + static_cast<size_t>(hx)]), 6,
                "fill 带不抢已有标签");
    }

    std::printf("\n── 11 个已验证点（用文件里的 Z 行 + any 带）────────\n");
    for (const Pt& p : kPts) {
        const float u = static_cast<float>(p.x) / 500.0f;
        const float v = static_cast<float>(p.y) / 900.0f;
        const std::string got = partapi::classify(u, v);
        const bool ok = (p.want == nullptr) || (got == p.want);
        std::printf("  %s  %-6s (%3d,%3d) u%.3f v%.3f → %-9s%s\n", ok ? "[ok]  " : "[FAIL]",
                    p.label, p.x, p.y, u, v, got.empty() ? "(空白)" : got.c_str(),
                    p.want ? (ok ? "" : "  ← 应该落在别的部位（文件格子和内存格子不同，记下来）") : "");
        if (!ok) ++gFail;
    }

    std::printf("\n%s\n", gFail ? "有 FAIL —— 别接入，先修" : "全部通过");
    return gFail ? 1 : 0;
}
