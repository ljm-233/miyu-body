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
#include "bubble.h"   // 二十六：说话时把这句话交给头顶气泡（直接调，不绕 main）
#include "platform.h"

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
#include <thread>
#include <mutex>
#include <fstream>
#include <string>

namespace ctl {

// 二十六：走路（main 的 walkTick 读、这边的 walkTo 命令写）
bool  gWalkWanted = false;
// 二十六：运行时播一个 Animation 里的动作（举手 / 竖手指 / 狗尾草 / 脱捕）
char  gMotionName[64] = {0};   // 空 = 没请求
float gWalkTargetX = 0.0f;
float gWalkTargetY = 0.0f;
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

// 第十六节：网页上的两个倍率/开关。都由 main 每帧喂进来（ctl 不反向 include web）。
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

// ── 出声（第十六节）────────────────────────────────────
// 只播**预生成**的短句（CosyVoice 合成一句要几十秒，实时不划算），和 tools/body-voice.sh
// 同一套约定：文件名 = md5(文本) 前 10 位 + .wav，索引在 $VOICE/index.tsv（key\ttab文本）。
// 拿不到文件 / 没有 pw-play → 一行日志，静默继续（绝不影响说话和渲染）。
//
// 用 double fork：孙进程 exec pw-play，子进程立刻退出并由父回收 —— 主循环一秒都不等它，
// 也不会攒一屋子僵尸。
// 二十六：读 wav 时长（只支持 PCM/float 的 RIFF，够本项目的合成输出用）。
// 为什么需要：嘴型的时长是按「字数 × 每字时长」估的，可实际音频往往短得多
// （单字「痒」只 0.5 秒，估出来 1.2 秒）→ **嘴还在动，声音早没了**，看着像假唱。
static double wavSeconds(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0.0;
    char hdr[44] = {0};
    f.read(hdr, 44);
    if (f.gcount() < 44) return 0.0;
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) return 0.0;
    auto u32 = [&](int off) {
        return static_cast<unsigned>(static_cast<unsigned char>(hdr[off])) |
               (static_cast<unsigned>(static_cast<unsigned char>(hdr[off + 1])) << 8) |
               (static_cast<unsigned>(static_cast<unsigned char>(hdr[off + 2])) << 16) |
               (static_cast<unsigned>(static_cast<unsigned char>(hdr[off + 3])) << 24);
    };
    auto u16 = [&](int off) {
        return static_cast<unsigned>(static_cast<unsigned char>(hdr[off])) |
               (static_cast<unsigned>(static_cast<unsigned char>(hdr[off + 1])) << 8);
    };
    const unsigned rate = u32(24);
    const unsigned byteRate = u32(28);
    if (rate == 0 || byteRate == 0) return 0.0;
    f.seekg(0, std::ios::end);
    const std::streamoff total = f.tellg();
    const std::streamoff data = total - 44;
    if (data <= 0) return 0.0;
    return static_cast<double>(data) / static_cast<double>(byteRate);
}

// 二十六（第二次修）：预热不解决问题 —— 每次 pw-play 都是**新建一条流**，
// pipewire 在流建起来的头几百毫秒里会丢样本，所以「先按裙子才有声」变成了
// 「先按腿才有声」，只是换了个牺牲品。
// 正解是让流一直活着：启动时拉起一个常驻的 `pw-cat --playback`，读 stdin，
// 之后每次说话只往里**写数据**，不再建流。
// 格式统一是 pcm_s16le / 24000 / 1ch（合成输出都是这个），所以能复用。
static FILE* gSink = nullptr;
static std::mutex gSinkMu;

[[maybe_unused]] static void sinkStart() {
    if (gSink) return;
    gSink = ::popen("pw-cat --playback --format s16 --rate 24000 --channels 1 -", "w");
    if (gSink) log_("[voice] 常驻输出流已建立（pw-cat）");
    else      log_("[voice] 常驻输出流建不起来，退回每次 pw-play");
}

// 读一个 wav，跳过 44 字节头，把数据段写进常驻流。放后台线程，别卡主循环。
[[maybe_unused]] static void sinkPlayFile(const std::string& path) {
    std::thread([path]() {
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        f.seekg(44, std::ios::beg);
        char buf[8192];
        std::lock_guard<std::mutex> lk(gSinkMu);
        if (!gSink) return;
        while (f.read(buf, sizeof buf) || f.gcount() > 0) {
            if (std::fwrite(buf, 1, static_cast<size_t>(f.gcount()), gSink) == 0) break;
            if (f.eof()) break;
        }
        std::fflush(gSink);
    }).detach();
}

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
    // 二十六（第四次）：`pw-play` 的文件参数是**单数**（用法里写的是 [<文件>|-]），
    // 传两个只会念第一个 —— 我上一版就是这样，结果每次都只播了静音，他一点声音都没有。
    // 回到单文件；「第一声被 pipewire 吞掉」这个先如实留着（下一次点就正常）。
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
    // 二十六：嘴跟着**音频的实际长度**走，不再按字数估
    const double sec = wavSeconds(path);
    if (sec > 0.05) {
        gSayDur = sec;
        gSayT0  = platTime();
        gTalking = true;
        log_("[voice] 播 %s.wav（vol %.2f，实长 %.2fs）: %s", key.c_str(), gVol, sec, text.c_str());
    } else {
        log_("[voice] 播 %s.wav（vol %.2f）: %s", key.c_str(), gVol, text.c_str());
    }
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
            // 第十六节的「嘴动时长倍率」：这里不改 gSayDur（那是这句话的原始时长），
            // 只报告实际会动多久，mouth() 里乘同一个系数。
            log_("[ctl] say %.1fs（×%.2f -> 嘴动 %.1fs）: %s",
                 dur, gMouthScale, dur * gMouthScale, say.c_str());
            // 2026-11-11 02:0x：气泡交给这里是**因为绕了一大圈都不生效** ——
            // main 每帧读 ctl::currentSay() 再转给 bubble，读到的永远是空
            //（gTalking 在 main 那一侧看不到，查了很久没查出来）。
            // 与其继续追那个分层，不如在「知道这句话」的地方直接给气泡。
            bubble::say(say.c_str(), now);
            if (gVoiceOn) voicePlay(say);
        }
    }

    // tap：外部（第二支指针 / 脚本）按她窗口内的 local 坐标点一下。
    // 2026-11-10 19:3x：她那层是 layer-shell 自绘，AT-SPI 里没有控件，
    // 所以外部指针点她只能走这条口子，不能靠合成器的按钮事件。
    {
        int tx = 0, ty = 0;
        const auto* kx = std::strstr(line.c_str(), "\"tap\"");
        if (kx) {
            const char* px = std::strstr(kx, "\"x\"");
            const char* py = std::strstr(kx, "\"y\"");
            if (px && py) {
                tx = std::atoi(std::strchr(px, ':') + 1);
                ty = std::atoi(std::strchr(py, ':') + 1);
                any = true;
                log_("[ctl] tap 请求 local=(%d,%d)", tx, ty);
                platTapAt(tx, ty);
            }
        }
    }

    // sway：诊断用 —— 直接给弹簧一个初速度，模拟「正在拖」，不碰真实鼠标。
    // 用法：echo '{"sway":{"vx":-400,"vy":80}}' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/miyu-body.sock
    {
        const auto* ks = std::strstr(line.c_str(), "\"sway\"");
        if (ks) {
            const char* px = std::strstr(ks, "\"vx\"");
            const char* py = std::strstr(ks, "\"vy\"");
            if (px && py) {
                any = true;
                const float vx = static_cast<float>(std::atof(std::strchr(px, ':') + 1));
                const float vy = static_cast<float>(std::atof(std::strchr(py, ':') + 1));
                int ms = 0;
                if (const char* pm = std::strstr(ks, "\"ms\"")) ms = std::atoi(std::strchr(pm, ':') + 1);
                log_("[ctl] sway 诊断注入 vx=%.0f vy=%.0f ms=%d", vx, vy, ms);
                platFakeDrag(vx, vy, ms);
            }
        }
    }

    // walkTo：让她**走过去**。
    // 用法：echo '{"walkTo":{"x":800,"y":300}}' | socat - UNIX-CONNECT:$XDG_RUNTIME_DIR/miyu-body.sock
    // 二十六（2026-11-11 00:4x）：他问「她还不可以走路呢，能不能去做走路，就是移动」。
    // 这里只收目标点，真正的位移和「走起来的样子」在 main 的 walkTick 里做。
    {
        const auto* kw = std::strstr(line.c_str(), "\"walkTo\"");
        if (kw) {
            const char* px = std::strstr(kw, "\"x\"");
            const char* py = std::strstr(kw, "\"y\"");
            if (px && py) {
                any = true;
                gWalkTargetX = static_cast<float>(std::atof(std::strchr(px, ':') + 1));
                gWalkTargetY = static_cast<float>(std::atof(std::strchr(py, ':') + 1));
                gWalkWanted  = true;
                log_("[ctl] walkTo 目标 (%.0f, %.0f)", gWalkTargetX, gWalkTargetY);
            }
        }
    }
    // 停止走：{"walkStop":1}
    if (std::strstr(line.c_str(), "\"walkStop\"")) { any = true; gWalkWanted = false; }

    // motion：播 Animation/ 下的一个 motion3（举手 / 竖手指切出来动画 / 狗尾草切换出来 / 脱捕 …）
    {
        std::string mn;
        if (keyStr(line, "motion", mn)) {
            any = true;
            std::snprintf(gMotionName, sizeof gMotionName, "%s", mn.c_str());
            log_("[ctl] motion 请求：%s", mn.c_str());
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
    // backlog 从 8 提到 128（2026-11-12 21:3x）。原因是查到一个真事故：
    // 主循环等 frame callback 时 ctl::poll 不跑，这期间来的连接只能排在 backlog 里；
    // 8 个一满，之后的 connect() 就会永久挂住，今天把 her-voice.py 卡死了四个半小时。
    // 128 治不了「主循环停着不 accept」这个根，但能把它从几分钟拖到几小时，
    // 再加上客户端两秒超时，就不会再出现一个进程卡住整个服务的情况。
    if (::listen(fd, 128) != 0) {
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

// 二十六：内部调用版「说一句」。逻辑和 socket 收到 {"say":...} 完全一样，
// 抽出来只为了让 main 也能直接让身体说话（点击时的那句台词）。
void say(const std::string& text, double now) {
    if (text.empty()) { gTalking = false; return; }
    double dur = static_cast<double>(utf8Chars(text)) * kPerChar;
    if (dur < kMinSay) dur = kMinSay;
    if (dur > 60.0) dur = 60.0;
    gSayText = text;
    gSayDur = dur;
    gSayT0 = now;
    gTalking = true;
    log_("[ctl] 自说 %.1fs: %s", dur, text.c_str());
    if (gVoiceOn) voicePlay(text);
}

float mouth(double now) {
    if (!gTalking) return -1.0f;
    const double t = now - gSayT0;
    // 第十六节：嘴动时长 = 原始时长 × 网页上的 mouth 倍率（0.5–3）。
    // 张合频率（0.22s 一张一合）不变，只是「这句话她动多久嘴」变了。
    const double dur = gSayDur * static_cast<double>(gMouthScale);
    if (t < 0.0 || t >= dur) {
        gTalking = false;
        return -1.0f;
    }
    // 二十六：嘴型重做。
    // 原来是一个**固定 0.22 秒周期**的正弦，和字数没关系 —— 说「嗯」（一个音）
    // 和说「别弄乱头发」（五个音）嘴动得一样勤，看着就是「嘴在抖」不是「在说话」。
    // 现在按**每个字一个开口**：
    //   · 字的时长 = 这句话的总时长 / 字数（和 kPerChar 同一个量级）
    //   · 每个字内部走一个 sin 半波（张开→合上），不是连续正弦
    //   · 每个字的峰值在 0.45–1.0 之间按字序伪随机（同一句每次一样，不闪）
    //   · 标点（，。…、？！）和空白当停顿：那一「字」不开口
    const int chars = utf8Chars(gSayText);
    double v = 0.0;
    if (chars <= 0) {
        v = 0.5 + 0.5 * std::sin(t * 2.0 * kPi / kMouthPeriod);
    } else {
        const double per = gSayDur / static_cast<double>(chars);
        int idx = static_cast<int>(t / per);
        if (idx < 0) idx = 0;
        if (idx >= chars) idx = chars - 1;
        const double inChar = (t - per * idx) / per;      // 0..1
        // 这一「字」是不是停顿（标点/空白）→ 不开口
        bool pause = false;
        {
            int k = 0;
            for (size_t i = 0; i < gSayText.size() && k <= idx; ) {
                const unsigned char c0 = static_cast<unsigned char>(gSayText[i]);
                size_t len = 1;
                if (c0 >= 0xF0) len = 4; else if (c0 >= 0xE0) len = 3; else if (c0 >= 0xC0) len = 2;
                if (k == idx) {
                    const std::string ch = gSayText.substr(i, len);
                    pause = (ch == "，" || ch == "。" || ch == "…" || ch == "、" ||
                             ch == "？" || ch == "！" || ch == " " || ch == "\t" ||
                             ch == "\n" || ch == "；" || ch == "：");
                    break;
                }
                i += len;
                ++k;
            }
        }
        if (!pause) {
            // 每个字的峰值：按字序伪随机，落回同一个数（同一句每次听/看都一样）
            unsigned h = static_cast<unsigned>(idx) * 2654435761u;
            h ^= h >> 13;
            const double peak = 0.45 + 0.55 * static_cast<double>(h & 0xFF) / 255.0;
            v = peak * std::sin(kPi * inChar);      // 半波：张开再合上
            if (v < 0.0) v = 0.0;
        }
    }
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

// 第十六节：网页设置页的两项，由 main 每帧喂进来。
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

// 二十六：预热。他报「先按腿才有声音，先按别的没声」—— 那是 pipewire 的
// 输出节点第一次要唤醒（几十毫秒～几百毫秒），第一声正好丢在这个窗口里。
// 启动时先播一段 0.12 秒的静音，把流建起来，后面就都正常。
const std::string& currentSay() {
    static const std::string kEmpty;
    // 调试用（已收工）：只在 talking 变化时记。想再看就打开下面这段。
    return gTalking ? gSayText : kEmpty;
}

void voiceWarmUp() {
    // 二十六（第四修）：根因是 pipewire 的 sink **空闲时会 suspend**，被唤醒的头几百
    // 毫秒丢样本 —— 所以「第一声没声音」每次换一个牺牲品（先是腿，后来是裙子）。
    // 单次预热没用（那条流建完就结束）。正解是让 sink **一直醒着**：
    // 起一个后台循环，极低音量反复播 0.12 秒静音。它占的是一条几乎无声的流，
    // 但对 sink 来说「有活跃流」= 不 suspend，于是真正说话时第一声就完整。
    {
    const char* env = std::getenv("MIYU_VOICE_DIR");
    const std::string dir = (env && *env) ? env : "/home/link/.dsh/miyu/body/voice";
    const std::string sil = dir + "/silence.wav";
    if (::access(sil.c_str(), R_OK) == 0) {
        std::string cmd = "while :; do pw-play --volume=0.01 '" + sil + "' 2>/dev/null; done &";
        if (::system(cmd.c_str()) == 0) log_("[voice] 保温流已起（sink 保持醒着，第一声不再被吞）");
    }
    }
    // 2026-11-11 20:4x：`sinkStart()` 这条常驻流**已经证死不删**：
    // pw-cat 读 stdin 会报 `sndfile: failed to open audio file "-"`，
    // 每次启动都往 journal 里留一条 error。真正让 sink 不睡的是上面那个
    // 循环播静音的保温流。这里只留一句记录，不再调用。
    // 想看当时的思路，往下翻到 sinkStart 的定义（保留作废代码）。
    const char* env = std::getenv("MIYU_VOICE_DIR");
    const std::string dir = (env && *env) ? env : "/home/link/.dsh/miyu/body/voice";
    const std::string path = dir + "/silence.wav";
    if (::access(path.c_str(), R_OK) != 0) return;
    const pid_t pid = ::fork();
    if (pid == 0) {
        const pid_t p2 = ::fork();
        if (p2 != 0) ::_exit(0);
        ::freopen("/dev/null", "w", stdout);
        ::freopen("/dev/null", "w", stderr);
        ::execlp("pw-play", "pw-play", "--volume=0.01", path.c_str(), (char*)nullptr);
        ::_exit(127);
    }
    int st = 0;
    ::waitpid(pid, &st, 0);
    log_("[voice] 输出流预热完成");
}


}  // namespace ctl
