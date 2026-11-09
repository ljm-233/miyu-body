// 动作计划播放器实现 —— 2026-11-09 晚。
//
// 三段：
//   1. 一个**小而稳**的递归下降 JSON 扫描器（不是正则）：对象/数组/字符串/数字，
//      带转义、深度上限、长度上限。计划的 frames 是嵌套数组，正则硬怼一撇就散。
//   2. 把解析出来的树收成「通道」：每个参数一条 (t, v) 关键帧表。缺省值的帧
//      不会把参数打成 0 —— 只认写了它的那些帧（边缘保持首/末帧的值）。
//   3. 播放：累加各 action 的 duration 排成一条总时间轴；每帧定位 action → 通道插值。
//
// 插值就线性（没叠 smoothstep）：计划里的时间点已经是人给的节奏，先让它可预测；
// 想要更软就在 JSON 里把关键帧排密一点。

#include "plan.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace plan {
namespace {

void log_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[plan] ", stderr);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

// ── 1. JSON ─────────────────────────────────────────────
struct J {
    enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    double num = 0.0;
    bool b = false;
    std::string str;
    std::vector<J> arr;
    std::vector<std::pair<std::string, J>> obj;

    const J* at(const char* k) const {
        if (t != OBJ) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct Parser {
    const char* s = nullptr;
    size_t i = 0, n = 0;
    int depth = 0;
    bool bad = false;

    void ws() {
        while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool lit(const char* w) {
        const size_t L = std::strlen(w);
        if (i + L <= n && std::strncmp(s + i, w, L) == 0) { i += L; return true; }
        return false;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    bool str(std::string& o) {
        if (i >= n || s[i] != '"') return false;
        ++i;
        while (i < n) {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (i >= n) return false;
            const char e = s[i++];
            switch (e) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u': {
                    if (i + 4 > n) return false;
                    unsigned cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s[i + k];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                        else return false;
                    }
                    i += 4;
                    utf8(o, cp);
                    break;
                }
                default: o += e; break;   // \" \\ \/ 都落到这里
            }
        }
        return false;   // 引号没闭上
    }
    bool value(J& out) {
        ws();
        if (i >= n) return false;
        if (depth > 24) { bad = true; return false; }
        const char c = s[i];
        if (c == '{') {
            ++i; ++depth;
            out.t = J::OBJ;
            ws();
            if (i < n && s[i] == '}') { ++i; --depth; return true; }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (i >= n || s[i] != ':') return false;
                ++i;
                J v;
                if (!value(v)) return false;
                out.obj.emplace_back(std::move(k), std::move(v));
                ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == '}') { ++i; --depth; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i; ++depth;
            out.t = J::ARR;
            ws();
            if (i < n && s[i] == ']') { ++i; --depth; return true; }
            for (;;) {
                J v;
                if (!value(v)) return false;
                out.arr.push_back(std::move(v));
                ws();
                if (i < n && s[i] == ',') { ++i; continue; }
                if (i < n && s[i] == ']') { ++i; --depth; return true; }
                return false;
            }
        }
        if (c == '"') { out.t = J::STR; return str(out.str); }
        if (lit("null")) { out.t = J::NUL; return true; }
        if (lit("true")) { out.t = J::BOOL; out.b = true; return true; }
        if (lit("false")) { out.t = J::BOOL; out.b = false; return true; }
        {
            char* end = nullptr;
            const double v = std::strtod(s + i, &end);
            if (end == s + i) return false;
            i = static_cast<size_t>(end - s);
            out.t = J::NUM;
            out.num = v;
            return true;
        }
    }
};

// 收到过的畸形参数名只警告一次（同一个名字不刷屏）。
std::vector<std::string> gWarnedNames;

void warnUnknownName(const std::string& n) {
    for (const auto& w : gWarnedNames)
        if (w == n) return;
    gWarnedNames.push_back(n);
    log_("参数「%s」不在白名单里，跳过（只警告这一次）", n.c_str());
}

// ── 2. 收成通道 ─────────────────────────────────────────
struct Key { double t; float v; };
struct Chan { int slot; std::vector<Key> keys; };
struct Act { double t0; double dur; std::vector<Chan> ch; };

std::vector<Act> gActs;
double gTotal = 0.0;
double gT0 = 0.0;
bool gOn = false;
char gTitle[128] = {0};

float interp(const std::vector<Key>& k, double t) {
    if (k.empty()) return 0.0f;
    if (t <= k.front().t) return k.front().v;
    if (t >= k.back().t) return k.back().v;
    size_t i = 1;
    while (i < k.size() && k[i].t < t) ++i;
    const Key& a = k[i - 1];
    const Key& b = k[i];
    const double span = b.t - a.t;
    if (span <= 1e-9) return b.v;
    const double u = (t - a.t) / span;
    return static_cast<float>(a.v + (b.v - a.v) * u);
}

// 一个 action 对象 → Act。失败返回 false（调用方决定整体丢还是留）。
bool buildAction(const J& a, double dur, Act& out, const char* label) {
    const J* frames = a.at("frames");
    if (frames == nullptr || frames->t != J::ARR || frames->arr.empty()) {
        log_("%s 没有 frames 数组，跳过这个 action", label);
        return false;
    }
    if (dur < 0.0) dur = 0.0;

    struct Frame { double t; std::vector<std::pair<int, float>> vals; };
    std::vector<Frame> fr;
    fr.reserve(frames->arr.size());
    double lastT = -1e9, maxT = 0.0;
    bool disorder = false;
    int idx = 0;

    for (const J& f : frames->arr) {
        ++idx;
        if (f.t != J::OBJ) { log_("%s 第 %d 帧不是对象，跳过", label, idx); continue; }
        const J* jt = f.at("time");
        if (jt == nullptr || jt->t != J::NUM) {
            log_("%s 第 %d 帧没有数字 time，跳过", label, idx);
            continue;
        }
        Frame cur;
        cur.t = jt->num;
        if (cur.t < 0.0) { log_("%s 第 %d 帧 time=%.3f 是负数，按 0 处理", label, idx, cur.t); cur.t = 0.0; }
        if (cur.t < lastT) disorder = true;
        lastT = cur.t;
        if (cur.t > maxT) maxT = cur.t;

        const J* vals = f.at("values");
        if (vals == nullptr || vals->t != J::OBJ) {
            log_("%s 第 %d 帧没有 values 对象，只当时间点用", label, idx);
        } else {
            for (const auto& kv : vals->obj) {
                const int slot = slotOf(kv.first.c_str());
                if (slot < 0) { warnUnknownName(kv.first); continue; }
                if (kv.second.t != J::NUM) {
                    log_("%s 参数 %s 不是数字，跳过（第 %d 帧）", label, kv.first.c_str(), idx);
                    continue;
                }
                cur.vals.emplace_back(slot, static_cast<float>(kv.second.num));
            }
        }
        fr.push_back(std::move(cur));
    }

    if (fr.empty()) {
        log_("%s 一帧可用的都没有，跳过这个 action", label);
        return false;
    }
    if (disorder)
        log_("%s frames 时间乱序 —— 已按时间重新排序播放（计划本身最好排好）", label);

    std::stable_sort(fr.begin(), fr.end(), [](const Frame& a_, const Frame& b_) { return a_.t < b_.t; });

    if (dur <= 0.0) {
        dur = maxT;
        log_("%s duration 缺失或为 0，按最后一帧 time=%.3fs 当长度", label, dur);
    }
    if (maxT > dur + 1e-6) {
        log_("%s 有关键帧排在 duration(%.3f) 之后（最晚 %.3f），长度按 %.3f 算",
             label, dur, maxT, maxT);
        dur = maxT;
    }
    if (dur <= 0.0) {
        log_("%s 长度算不出来（duration 和所有 time 都是 0），跳过", label);
        return false;
    }

    // 同一参数的多帧 → 一条通道
    out.t0 = 0.0;
    out.dur = dur;
    out.ch.clear();
    for (const Frame& f : fr) {
        for (const auto& sv : f.vals) {
            Chan* ch = nullptr;
            for (auto& c : out.ch)
                if (c.slot == sv.first) { ch = &c; break; }
            if (ch == nullptr) { out.ch.push_back(Chan{sv.first, {}}); ch = &out.ch.back(); }
            ch->keys.push_back(Key{f.t, sv.second});
        }
    }
    for (auto& c : out.ch)
        std::stable_sort(c.keys.begin(), c.keys.end(), [](const Key& a_, const Key& b_) { return a_.t < b_.t; });
    return true;
}

}  // namespace

const char* const kNames[kMaxParams] = {
    "AngleX", "AngleY", "AngleZ",
    "PositionX", "PositionY", "PositionZ",
    "ParamEyeBallX", "ParamEyeBallY",
    "ParamBodyAngleZ", "ParamMouthOpenY", "LeanInput",
};

int slotOf(const char* name) {
    if (name == nullptr) return -1;
    for (int i = 0; i < kMaxParams; ++i)
        if (std::strcmp(kNames[i], name) == 0) return i;
    return -1;
}

// 计划体可能是：{"version":1,"actions":[...]} / {"frames":[...]} / 外面还包了一层。
void start(const std::string& json, double now) {
    if (json.size() > 512 * 1024) {
        log_("计划 %zu 字节，太大（上限 512KB），忽略", json.size());
        return;
    }
    Parser p;
    p.s = json.c_str();
    p.n = json.size();
    J root;
    if (!p.value(root)) {
        log_("计划不是合法 JSON（第 %zu 字节附近），忽略：%.100s", p.i, json.c_str());
        return;
    }

    // 允许外面再包一层：{"plan":{...}} / {"trajectory":{...}}
    const J* body = &root;
    if (root.t == J::OBJ) {
        if (const J* w = root.at("plan"); w != nullptr && w->t == J::OBJ) body = w;
        else if (const J* w = root.at("trajectory"); w != nullptr && w->t == J::OBJ) body = w;
    }

    std::vector<Act> acts;
    if (body->t != J::OBJ) {
        log_("计划体不是对象，忽略");
        return;
    }

    const J* actions = body->at("actions");
    if (actions != nullptr && actions->t == J::ARR) {
        int k = 0;
        for (const J& a : actions->arr) {
            ++k;
            if (a.t != J::OBJ) { log_("actions[%d] 不是对象，跳过", k); continue; }
            char label[64];
            std::snprintf(label, sizeof(label), "actions[%d]", k);
            double dur = 0.0;
            if (const J* d = a.at("duration"); d != nullptr && d->t == J::NUM) dur = d->num;
            Act act;
            if (!buildAction(a, dur, act, label)) continue;
            acts.push_back(std::move(act));
        }
    } else if (body->at("frames") != nullptr) {
        // 单条 action（"trajectory":{...}）
        double dur = 0.0;
        if (const J* d = body->at("duration"); d != nullptr && d->t == J::NUM) dur = d->num;
        Act act;
        if (buildAction(*body, dur, act, "trajectory")) acts.push_back(std::move(act));
    } else {
        log_("计划里既没有 actions[] 也没有 frames[]，忽略：%.100s", json.c_str());
        return;
    }

    if (acts.empty()) {
        log_("计划里没有一个能播的 action，忽略（原来在播的继续）");
        return;
    }

    // 排总时间轴
    double cursor = 0.0;
    for (auto& a : acts) {
        a.t0 = cursor;
        cursor += a.dur;
    }
    if (cursor <= 0.0) {
        log_("计划总时长为 0，忽略");
        return;
    }

    gActs = std::move(acts);
    gTotal = cursor;
    gT0 = now;
    gOn = true;
    gTitle[0] = '\0';
    if (const J* t = body->at("title"); t != nullptr && t->t == J::STR && !t->str.empty()) {
        std::snprintf(gTitle, sizeof(gTitle), "%s", t->str.c_str());
    }
    log_("开始播放：%s%s%s，%zu 个 action，总时长 %.2fs",
         gTitle[0] ? "「" : "(无题)", gTitle, gTitle[0] ? "」" : "", gActs.size(), gTotal);
    for (size_t i = 0; i < gActs.size(); ++i) {
        log_("  action[%zu] t=%.2f..%.2f，参数 %zu 条", i + 1, gActs[i].t0,
             gActs[i].t0 + gActs[i].dur, gActs[i].ch.size());
    }
}

void stop() {
    if (gOn) log_("停止计划：%s", gTitle[0] ? gTitle : "(无题)");
    gOn = false;
    gActs.clear();
    gTotal = 0.0;
    gTitle[0] = '\0';
}

bool active() { return gOn; }
double startTime() { return gT0; }
double total() { return gTotal; }
const char* title() { return gTitle; }

bool drives(const char* name) {
    const int slot = slotOf(name);
    if (slot < 0) return false;
    for (const auto& a : gActs)
        for (const auto& c : a.ch)
            if (c.slot == slot) return true;
    return false;
}

bool sample(double now, Sample& out) {
    out.n = 0;
    if (!gOn) return false;
    const double t = now - gT0;
    if (t < 0.0) return false;
    if (t >= gTotal) {
        log_("播完：%s（%.2fs），交回待机", gTitle[0] ? gTitle : "(无题)", gTotal);
        stop();
        return false;
    }
    const Act* act = nullptr;
    for (const auto& a : gActs) {
        if (t >= a.t0 && t < a.t0 + a.dur) { act = &a; break; }
    }
    if (act == nullptr) {          // 浮点边界：就取最后一个
        act = &gActs.back();
    }
    const double local = t - act->t0;
    for (const auto& c : act->ch) {
        if (out.n >= kMaxParams) break;
        out.name[out.n] = kNames[c.slot];
        out.value[out.n] = interp(c.keys, local);
        ++out.n;
    }
    return out.n > 0;
}

}  // namespace plan
