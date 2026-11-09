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
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

// 动作计划请求（2026-11-09 晚）：原文 + 是否有新的一份 + 是否是「停」
std::string gPlanReq;
bool gPlanPending = false;

int gMood = -1;

// 第十五节：网页上的两个倍率/开关。都由 main 每帧喂进来（ctl 不反向 include web）。
float gMouthScale = 1.0f;    // 嘴动时长倍率 0.5–3
bool  gVoiceOn = false;      // 说话时要不要出声
float gVol = 0.8f;           // 音量 0–1

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

// ── 计划体切出来（2026-11-09 晚）─────────────────────────
// 这一层只负责「找到 `"plan":` 后面那一段对象文本」，从 `{` 数括号数到配对的 `}`，
// 字符串里的 `{` `}` 和转义要跳过 —— 计划的 frames 是嵌套数组，切错一个字节就整份废掉。
// 真正的字段解析归 plan.cpp（它带一个正经的 JSON 扫描器）；ctl 只搬文本，
// 因为它不认识 Cubism，也不该替播放器决定「哪些字段有用」。
//
// 返回值：0 = 没有这个键；1 = 切到对象（out 有内容）；2 = 值是 null（= 停止）。
int takeObjText(const std::string& s, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return 0;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return 0;
    ++p;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
    if (p >= s.size()) return 0;
    if (s.compare(p, 4, "null") == 0) return 2;
    if (s[p] != '{') {
        log_("[ctl] 警告：%s 的值不是对象（%.20s…），忽略", key, s.c_str() + p);
        return 0;
    }
    int depth = 0;
    bool inStr = false;
    for (size_t q = p; q < s.size(); ++q) {
        const char c = s[q];
        if (inStr) {
            if (c == '\\') { ++q; continue; }     // \" \\ 之类整对跳过
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') ++depth;
        else if (c == '}') {
            if (--depth == 0) { out = s.substr(p, q - p + 1); return 1; }
        }
    }
    log_("[ctl] 警告：%s 的括号没闭上，忽略", key);
    return 0;
}

// ── 出声（第十五节）────────────────────────────────────
// 只播**预生成**的短句（CosyVoice 合成一句要几十秒，实时不划算），和 tools/body-voice.sh
// 同一套约定：文件名 = md5(文本) 前 10 位 + .wav，索引在 $VOICE/index.tsv（key\ttab文本）。
// 拿不到文件 / 没有 pw-play → 一行日志，静默继续（绝不影响说话和渲染）。
//
// 用 double fork：孙进程 exec pw-play，子进程立刻退出并由父回收 —— 主循环一秒都不等它，
// 也不会攒一屋子僵尸。
void voicePlay(const std::string& text) {
    const char* env = std::getenv("MIYU_VOICE_DIR");
    const std::string dir = (env && *env) ? env : "/home/link/.dsh/miyu/body/voice";

    std::string key;
    {
        std::ifstream f(dir + "/index.tsv");
        if (!f) { log_("[voice] 没有 %s/index.tsv（静默）", dir.c_str()); return; }
        std::string line;
        while (std::getline(f, line)) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            if (line.compare(tab + 1, std::string::npos, text) == 0) {
                key = line.substr(0, tab);
                break;
            }
        }
    }
    if (key.empty()) { log_("[voice] 没有预生成「%s」（静默）", text.c_str()); return; }

    const std::string path = dir + "/" + key + ".wav";
    if (::access(path.c_str(), R_OK) != 0) {
        log_("[voice] index 里有 %s 但文件不在（静默）", key.c_str());
        return;
    }
    char volbuf[16];
    std::snprintf(volbuf, sizeof volbuf, "%.2f", gVol);
    const pid_t pid = ::fork();
    if (pid < 0) { log_("[voice] fork 失败（静默）"); return; }
    if (pid == 0) {
        const pid_t p2 = ::fork();
        if (p2 != 0) ::_exit(0);                   // 中间子进程立刻走，孙进程被 init 收养
        ::freopen("/dev/null", "w", stdout);
        ::freopen("/dev/null", "w", stderr);
        const std::string volarg = std::string("--volume=") + volbuf;
        ::execlp("pw-play", "pw-play", volarg.c_str(), path.c_str(), (char*)nullptr);
        ::_exit(127);                               // 没有 pw-play 就走这条，什么都不响
    }
    int st = 0;
    ::waitpid(pid, &st, 0);                        // 只等中间子进程（毫秒级）
    log_("[voice] 播 %s.wav（vol %.2f）: %s", key.c_str(), gVol, text.c_str());
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
            // 第十五节的「嘴动时长倍率」：这里不改 gSayDur（那是这句话的原始时长），
            // 只报告实际会动多久，mouth() 里乘同一个系数。
            log_("[ctl] say %.1fs（×%.2f -> 嘴动 %.1fs）: %s",
                 dur, gMouthScale, dur * gMouthScale, say.c_str());
            if (gVoiceOn) voicePlay(say);
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

    // plan / trajectory（2026-11-09 晚）：整份动作计划，或者单个 action。
    // 这里只把对象文本切出来交给 main → plan::start()，字段解析不在这儿做。
    {
        std::string body;
        int r = takeObjText(line, "plan", body);
        if (r == 0) r = takeObjText(line, "trajectory", body);
        if (r != 0) {
            any = true;
            gPlanReq = (r == 2) ? std::string() : body;
            gPlanPending = true;
            if (r == 2) log_("[ctl] plan=null -> 停止当前计划");
            else log_("[ctl] plan 收到 %zu 字节，交给播放器", body.size());
        }
    }

    if (!any) {
        // 认不出的形状只警告一次（同一条不刷屏）
        if (std::strncmp(gWarned, line.c_str(), sizeof(gWarned) - 1) != 0) {
            std::snprintf(gWarned, sizeof(gWarned), "%s", line.c_str());
            log_("[ctl] 警告：没有一个认识的字段（say/mood/expr/dur/plan/trajectory），忽略：%.120s", line.c_str());
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
            if (gBuf.size() > 128 * 1024) {                    // 超长：当畸形，丢掉
                log_("[ctl] 警告：一行超过 128KB，丢弃");
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
    // 第十五节：嘴动时长 = 原始时长 × 网页上的 mouth 倍率（0.5–3）。
    // 张合频率（0.22s 一张一合）不变，只是「这句话她动多久嘴」变了。
    const double dur = gSayDur * static_cast<double>(gMouthScale);
    if (t < 0.0 || t >= dur) {
        gTalking = false;
        return -1.0f;
    }
    // 一张一合：0.5 + 0.5*sin(2π t / 0.22)
    double v = 0.5 + 0.5 * std::sin(t * 2.0 * kPi / kMouthPeriod);
    // 整体淡入淡出（前 0.15 渐入、最后 0.2 渐出）
    double env = 1.0;
    if (t < kFadeIn) env = t / kFadeIn;
    const double left = dur - t;
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

bool takePlan(std::string& out) {
    if (!gPlanPending) return false;
    gPlanPending = false;
    out.swap(gPlanReq);
    gPlanReq.clear();
    return true;
}

int mood() { return gMood; }

// 第十五节：网页设置页的两项，由 main 每帧喂进来。
//   · mouth 倍率只影响「接下来的」说话时长；正在说话的这句当场生效（t 和 dur 同一把尺）
//   · voice/vol 在说话那一刻读，改了下一句就变
void setMouthScale(float s) {
    if (s < 0.1f) s = 0.1f;
    if (s > 5.0f) s = 5.0f;
    if (s == gMouthScale) return;
    gMouthScale = s;
    log_("[ctl] 嘴动时长倍率 %.2f", gMouthScale);
}

void setVoice(bool on, float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    if (on != gVoiceOn || vol != gVol) log_("[ctl] 声音 %s，音量 %.2f", on ? "开" : "关", vol);
    gVoiceOn = on;
    gVol = vol;
}

const char* sockPath() { return gPath.empty() ? "(未建)" : gPath.c_str(); }

}  // namespace ctl
