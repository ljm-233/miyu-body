// 本地控制口实现 —— 2026-11-09 晚。
//
// 设计要点（都是踩出来/想清楚的，别随手改）：
//   · Unix domain socket + SOCK_STREAM，**一行一条命令**。一条连接只服务一行，
//     收完立刻 close，然后回 listen 等下一个 —— 客户端可以一次连接一条，不用管粘包、
//     也不用管多客户端排队（accept 队列 8）。
//   · listen fd 和 accept 出来的 fd 都设 O_NONBLOCK。渲染是每秒几十帧的主循环，
//     任何一次阻塞等待都会让动画卡顿；宁可下一帧再收。
//   · 解析**故意粗放**：找 `"key":"..."` 和 `"key":数字`，不引 json 库。
//     认不出就记一行警告继续跑 —— 外面写错一个字节不该把她弄死。
//   · 说话包络（嘴）在这里算，main.cpp 只负责 Set 参数。参数本身归 main 管，
//     免得两个文件都去碰 Cubism 的类型。

#include "ctl.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ctl {
namespace {

constexpr double kPi = 3.14159265358979323846;

// ── 嘴的包络参数（NOTES 第十一节记了一份）──────────────
constexpr double kMouthPeriod = 0.22;   // 一口一张的周期（秒）：0.5+0.5*sin(t*2π/0.22)
constexpr double kFadeIn = 0.15;        // 前 0.15s 从 0 渐入
constexpr double kFadeOut = 0.20;       // 最后 0.2s 渐出
constexpr double kPerChar = 0.16;       // 一个字 0.16s
constexpr double kMinSay = 1.2;         // 再短也撑 1.2s

int gListen = -1;
int gConn = -1;
double gConnT0 = 0.0;
std::string gPath;
std::string gBuf;            // 当前连接已经收到、还没成行的字节
char gWarned[4096] = {0};    // 同一行畸形只警告一次，别刷屏

// 说话状态
double gSayT0 = -1e9;
double gSayDur = 0.0;
std::string gSayText;
bool gTalking = false;

// 表情请求
std::string gExprReq;
bool gExprPending = false;

int gMood = -1;

void log_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
}

// ── 粗放解析：只要三种形状 ─────────────────────────────

// "key":"值" —— 处理反斜杠转义（\" \\ \n \t），\uXXXX 原样跳过（客户端都用 UTF-8）
bool keyStr(const std::string& s, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size() || s[p] != '"') return false;
    ++p;
    std::string v;
    while (p < s.size() && s[p] != '"') {
        if (s[p] == '\\' && p + 1 < s.size()) {
            const char c = s[p + 1];
            if (c == 'n') v += '\n';
            else if (c == 't') v += '\t';
            else if (c == 'r') v += '\r';
            else if (c == 'u') { v += "\\u"; }   // 不展开，留着给人看
            else v += c;
            p += 2;
            continue;
        }
        v += s[p++];
    }
    out = v;
    return true;
}

// "key":数字 —— 也认 2.5e1 这种，null/字符串一律当「没有」
bool keyNum(const std::string& s, const char* key, double& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size()) return false;
    if (s.compare(p, 4, "null") == 0) return false;
    if (!((s[p] >= '0' && s[p] <= '9') || s[p] == '-' || s[p] == '+')) return false;
    char* end = nullptr;
    const double v = std::strtod(s.c_str() + p, &end);
    if (end == s.c_str() + p) return false;
    out = v;
    return true;
}

int utf8Chars(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ── 一行命令 ────────────────────────────────────────────
void handleLine(const std::string& raw, double now) {
    const std::string line = trim(raw);
    if (line.empty()) return;

    if (line[0] != '{') {
        log_("[ctl] 警告：不是一行 JSON，忽略：%.120s", line.c_str());
        return;
    }

    bool any = false;

    // say / dur
    std::string say;
    const bool hasSay = keyStr(line, "say", say);
    if (hasSay) {
        any = true;
        if (say.empty()) {
            gTalking = false;
            log_("[ctl] say 空串 -> 闭嘴");
        } else {
            double dur = 0.0;
            if (!keyNum(line, "dur", dur) || dur <= 0.0)
                dur = static_cast<double>(utf8Chars(say)) * kPerChar;
            if (dur < kMinSay) dur = kMinSay;
            if (dur > 60.0) dur = 60.0;      // 防呆：一句话不许占住嘴 10 分钟
            gSayText = say;
            gSayDur = dur;
            gSayT0 = now;
            gTalking = true;
            log_("[ctl] say %.1fs: %s", dur, say.c_str());
        }
    }

    // expr（空串 / "默认" = 淡出回默认）
    std::string en;
    if (keyStr(line, "expr", en)) {
        any = true;
        gExprReq = en;
        gExprPending = true;
        log_("[ctl] expr 请求: %s", en.empty() ? "(默认)" : en.c_str());
    }

    // mood（0..100）：不只记账，直接翻成表情 —— 这是「她跟着我情绪走」的那一步。
    // 档位只挑实测有效的表情（＞＜ / 流冷汗 / 兔牙 / 脸红 / 黑脸），
    // 前倾、ParamAngle 那些在这个模型上是空壳，别用。
    double mv = 0.0;
    if (keyNum(line, "mood", mv)) {
        any = true;
        int v = static_cast<int>(mv < 0 ? mv - 0.5 : mv + 0.5);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        gMood = v;
        const char* face = "";
        if (v < 30) face = "黑脸";
        else if (v < 45) face = "流冷汗";
        else if (v < 60) face = "";
        else if (v < 78) face = "兔牙";
        else face = "脸红";
        // 只在换档时才动，别每次心跳都重启一次表情（会一直闪）
        static int lastBucket = -1;
        const int bucket = (v < 30) ? 0 : (v < 45) ? 1 : (v < 60) ? 2 : (v < 78) ? 3 : 4;
        if (bucket != lastBucket) {
            lastBucket = bucket;
            gExprReq = face;
            gExprPending = true;
            log_("[ctl] mood=%d -> 表情「%s」", v, *face ? face : "默认");
        } else {
            log_("[ctl] mood=%d（本档不变）", v);
        }
    }

    if (!any) {
        // 认不出的形状只警告一次（同一条不刷屏）
        if (std::strncmp(gWarned, line.c_str(), sizeof(gWarned) - 1) != 0) {
            std::snprintf(gWarned, sizeof(gWarned), "%s", line.c_str());
            log_("[ctl] 警告：没有一个认识的字段（say/mood/expr/dur），忽略：%.120s", line.c_str());
        }
    }
}

void closeConn() {
    if (gConn >= 0) ::close(gConn);
    gConn = -1;
    gBuf.clear();
}

}  // namespace

void init() {
    const char* rt = std::getenv("XDG_RUNTIME_DIR");
    if (rt != nullptr && rt[0] != '\0') gPath = std::string(rt) + "/miyu-body.sock";
    else gPath = "/tmp/miyu-body-" + std::to_string(static_cast<long>(::getuid())) + ".sock";

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (gPath.size() + 1 > sizeof(addr.sun_path)) {
        log_("[ctl] socket 路径太长，起不来：%s", gPath.c_str());
        return;
    }
    std::memcpy(addr.sun_path, gPath.c_str(), gPath.size() + 1);

    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        log_("[ctl] socket() 失败：%s（继续渲染，没有口子）", std::strerror(errno));
        return;
    }
    ::unlink(gPath.c_str());          // 上一轮崩溃留下的死文件
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        log_("[ctl] bind 失败 %s：%s（继续渲染，没有口子）", gPath.c_str(), std::strerror(errno));
        ::close(fd);
        return;
    }
    ::chmod(gPath.c_str(), 0600);
    if (::listen(fd, 8) != 0) {
        log_("[ctl] listen 失败：%s", std::strerror(errno));
        ::close(fd);
        ::unlink(gPath.c_str());
        return;
    }
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    gListen = fd;
    log_("[ctl] socket = %s（一行一条 JSON：{\"say\":\"...\",\"mood\":54,\"expr\":\"前倾\",\"dur\":null}）",
         gPath.c_str());
}

void poll(double now) {
    if (gListen < 0) return;

    if (gConn < 0) {
        pollfd p{};
        p.fd = gListen;
        p.events = POLLIN;
        if (::poll(&p, 1, 0) <= 0) return;
        const int c = ::accept(gListen, nullptr, nullptr);
        if (c < 0) return;
        const int fl = ::fcntl(c, F_GETFL, 0);
        ::fcntl(c, F_SETFL, fl | O_NONBLOCK);
        gConn = c;
        gConnT0 = now;
        gBuf.clear();
    }

    char tmp[512];
    for (;;) {
        const ssize_t n = ::recv(gConn, tmp, sizeof(tmp), 0);
        if (n > 0) {
            gBuf.append(tmp, static_cast<size_t>(n));
            if (gBuf.find('\n') != std::string::npos) break;   // 成行了，去处理
            if (gBuf.size() > 4096) {                          // 超长：当畸形，丢掉
                log_("[ctl] 警告：一行超过 4KB，丢弃");
                closeConn();
                return;
            }
            continue;
        }
        if (n == 0) {            // 对端关了：剩下的就当这一行
            if (!gBuf.empty()) handleLine(gBuf, now);
            closeConn();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 还没收到换行。客户端可能是先 connect 再慢慢写 —— 最多等 1 秒。
            if (now - gConnT0 > 1.0) {
                if (!gBuf.empty()) handleLine(gBuf, now);
                else log_("[ctl] 警告：连接 1s 没发完整一行，丢弃");
                closeConn();
            }
            return;
        }
        log_("[ctl] recv 出错：%s，断开", std::strerror(errno));
        closeConn();
        return;
    }

    const size_t nl = gBuf.find('\n');
    handleLine(gBuf.substr(0, nl), now);
    closeConn();
}

float mouth(double now) {
    if (!gTalking) return -1.0f;
    const double t = now - gSayT0;
    if (t < 0.0 || t >= gSayDur) {
        gTalking = false;
        return -1.0f;
    }
    // 一张一合：0.5 + 0.5*sin(2π t / 0.22)
    double v = 0.5 + 0.5 * std::sin(t * 2.0 * kPi / kMouthPeriod);
    // 整体淡入淡出（前 0.15 渐入、最后 0.2 渐出）
    double env = 1.0;
    if (t < kFadeIn) env = t / kFadeIn;
    const double left = gSayDur - t;
    if (left < kFadeOut) env = left / kFadeOut;   // 收尾取更小的那个
    if (env < 0.0) env = 0.0;
    v *= env;
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    return static_cast<float>(v);
}

bool takeExpr(std::string& out) {
    if (!gExprPending) return false;
    gExprPending = false;
    out = gExprReq;
    gExprReq.clear();
    return true;
}

int mood() { return gMood; }

const char* sockPath() { return gPath.empty() ? "(未建)" : gPath.c_str(); }

}  // namespace ctl
