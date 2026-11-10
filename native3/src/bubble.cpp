// 说话气泡的实现。见 bubble.h 顶部的说明。
#include "bubble.h"

#include <GL/glew.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>

namespace bubble {

// 调试输出直接写文件：主循环日志太多，journald 会限速丢行（今晚已经栽过一次）
static void blog(const char* fmt, ...) {
    FILE* f = std::fopen("state/bubble.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fclose(f);
}

namespace {

// ── 可调的几个数 ────────────────────────────────────────────────
// 2026-11-11 01:5x：「聊天气泡有点糊」—— 因为我按**逻辑像素**生成纹理（22px 的字），
// 画的时候又放大 2 倍贴到 framebuffer（窗口是 500 逻辑 / 1000 物理），等于把 22px 的字
// 撑成 44px，必糊。现在整张图按**物理像素**做：字号 44、内边距圆角全部 ×2，
// 贴的时候 1:1 不动，字就是渲染器原生的清晰度。
const int   kFontPx     = 44;      // 字号（物理像素，对应逻辑 22）
const int   kPadX       = 36;      // 左右内边距
const int   kPadY       = 22;      // 上下内边距
const int   kRadius     = 32;      // 圆角半径（2026-11-11 02:2x 他说 46 那版难看，改回 32）
const int   kTailW      = 32;      // 底下小尖角的宽度
const int   kTailH      = 0;       // 2026-11-11 02:2x 他说去掉小三角，改成 0（画尖角那段自然就不执行了）
const float kInDur      = 0.16f;   // 淡入 + 放大
const float kOutDur     = 0.25f;   // 淡出 + 上浮
const float kHoldExtra  = 0.9f;    // 说完之后再挂这么久才淡出
const float kMaxWidthPx = 1120.0f; // 单行最宽（物理像素；超了就压字距，不换行）

const char* kFontPaths[] = {
    "/usr/share/fonts/adobe-source-han-sans/SourceHanSansCN-Bold.otf",
    "/usr/share/fonts/adobe-source-han-sans/SourceHanSansCN-Regular.otf",
    "/usr/share/fonts/adobe-source-han-sans/SourceHanSansCN-Light.otf",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/noto-cjk/NotoSansCJK-Light.ttc",
};

FT_Library gLib = nullptr;
FT_Face    gFace = nullptr;

GLuint gTex = 0;
GLuint gVao = 0, gVbo = 0;
bool   gReady = false;      // 字体 + GL 资源都好了
bool   gOn = true;          // 设置里的开关

// 当前这句
std::string gText;
double gT0 = 0.0;           // 这句话什么时候开始
double gDur = 0.0;          // 这句话要挂多久（由 ctl 的嘴型时长给）
int    gTexW = 0, gTexH = 0;
bool   gDirty = false;      // 文字变了、需要重画纹理
double gNow = 0.0;          // 这一帧的时间（draw 里算动画要用）
double gHold = 2.6;         // 这句话挂到什么时候（say 时按字数算）

// ── 圆角矩形（画进 CPU 缓冲，不是 GL） ──────────────────────────
// 用「到圆角圆心的距离」算覆盖率，简单可靠：角落平滑，不会有锯齿硬边。
inline float roundRectCoverage(float x, float y, float w, float h, float r) {
    const float cx = w * 0.5f, cy = h * 0.5f;
    const float dx = std::fabs(x - cx) - (cx - r);
    const float dy = std::fabs(y - cy) - (cy - r);
    if (dx <= 0.0f || dy <= 0.0f) {
        // 在十字区域内：只要没出边界就是满覆盖
        if (x >= 0.0f && x <= w && y >= 0.0f && y <= h) return 1.0f;
        return 0.0f;
    }
    const float d = std::sqrt(dx * dx + dy * dy) - r;
    if (d <= -1.0f) return 1.0f;
    if (d >= 1.0f) return 0.0f;
    return 0.5f - d * 0.5f;      // 1px 的抗锯齿过渡
}

void putPx(std::vector<unsigned char>& buf, int w, int h, int x, int y,
           float r, float g, float b, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0.0f) return;
    unsigned char* p = &buf[(static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)) * 4];
    // 预乘 alpha 的普通 alpha 混合（src over）
    const float sa = a;
    const float da = 1.0f - sa;
    p[0] = static_cast<unsigned char>(std::fmin(255.0f, r * sa * 255.0f + p[0] * da));
    p[1] = static_cast<unsigned char>(std::fmin(255.0f, g * sa * 255.0f + p[1] * da));
    p[2] = static_cast<unsigned char>(std::fmin(255.0f, b * sa * 255.0f + p[2] * da));
    p[3] = static_cast<unsigned char>(std::fmin(255.0f, a * 255.0f + p[3] * da));
}

// 把一句话画成一张 RGBA 纹理
bool buildTexture(const std::string& text, double lastSeconds) {
    (void)lastSeconds;
    if (!gFace || text.empty()) return false;

    // ① 量一下宽度（按字逐个推进，不做 kerning —— 中文用不上）
    FT_Set_Pixel_Sizes(gFace, 0, kFontPx);
    float penX = 0.0f;
    struct Glyph { unsigned int cp; float x; };
    std::vector<Glyph> glyphs;
    for (size_t i = 0; i < text.size();) {
        unsigned int cp = 0;
        const unsigned char c = static_cast<unsigned char>(text[i]);
        int len = 1;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0xE) { cp = c & 0x0F; len = 3; }
        else { cp = c & 0x07; len = 4; }
        for (int k = 1; k < len && i + k < text.size(); ++k)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += len;
        if (FT_Load_Char(gFace, cp, FT_LOAD_RENDER)) continue;
        glyphs.push_back({cp, penX});
        penX += static_cast<float>(gFace->glyph->advance.x >> 6);
    }
    if (glyphs.empty()) return false;

    // 太宽就整句压一下字距（她的话都很短，不会出现换行需求）
    float advanceScale = 1.0f;
    const float innerW = penX;
    if (innerW > kMaxWidthPx) advanceScale = kMaxWidthPx / innerW;

    const int textW = static_cast<int>(innerW * advanceScale + 0.5f);
    const int w = textW + kPadX * 2;
    const int h = kFontPx + kPadY * 2;
    const int fullH = h + kTailH;      // 底下小尖角占的额外高度

    std::vector<unsigned char> buf(static_cast<size_t>(w) * fullH * 4, 0);

    // ② 底：先画一圈亮边（外扩 2px），再在内层填深色。
    // 之前那版用「距圆心距离」判描边，算出来的覆盖度太弱，边框基本看不见 ——
    // 改成两层实心：外圈亮一点当描边，内圈深色当底，边缘靠 roundRectCoverage 做抗锯齿。
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float covOut = roundRectCoverage(x + 0.5f, y + 0.5f,
                                                   static_cast<float>(w), static_cast<float>(h),
                                                   static_cast<float>(kRadius));
            if (covOut <= 0.0f) continue;
            // 外圈：柔和的蓝灰描边（比之前淡一档，不要硬边）
            putPx(buf, w, fullH, x, y, 0.74f, 0.84f, 1.0f, 0.95f * covOut);
            // 内圈：深底（往里缩 2px）
            // 描边宽度：外扩 7px（之前 4px，他说太细）
            const float covIn = roundRectCoverage(x + 0.5f - 7.0f, y + 0.5f - 7.0f,
                                                  static_cast<float>(w) - 14.0f,
                                                  static_cast<float>(h) - 14.0f,
                                                  static_cast<float>(kRadius) - 7.0f);
            if (covIn > 0.0f) putPx(buf, w, fullH, x, y, 0.07f, 0.09f, 0.14f, 0.94f * covIn);
        }
    }
    // 底下的小尖角（指向她的头）
    {
        const int cx = w / 2;
        for (int y = 0; y < kTailH; ++y) {
            const float t = 1.0f - static_cast<float>(y) / static_cast<float>(kTailH);
            const int half = static_cast<int>(kTailW * 0.5f * t);
            for (int x = cx - half; x <= cx + half; ++x) {
                const bool edge = (x <= cx - half + 1 || x >= cx + half - 1 || y == 0 || y == kTailH - 1);
                if (edge) putPx(buf, w, fullH, x, h + y, 0.74f, 0.84f, 1.0f, 0.95f);
                else      putPx(buf, w, fullH, x, h + y, 0.07f, 0.09f, 0.14f, 0.94f);
            }
        }
    }

    // ③ 字：白色，逐字贴上去（FreeType 给的是 8 位灰度）
    for (const Glyph& gl : glyphs) {
        if (FT_Load_Char(gFace, gl.cp, FT_LOAD_RENDER)) continue;
        FT_GlyphSlot sl = gFace->glyph;
        const int gx = kPadX + static_cast<int>(gl.x * advanceScale + 0.5f) + sl->bitmap_left;
        const int gy = kPadY + (kFontPx - sl->bitmap_top) + sl->bitmap_top - sl->bitmap_top;
        // 基线：FreeType 的 bitmap_top 是从基线往上，这里让字垂直居中在正文区
        const int baseline = kPadY + (kFontPx * 4) / 5;
        const int by = baseline - sl->bitmap_top;
        (void)gy;
        for (unsigned int row = 0; row < sl->bitmap.rows; ++row) {
            for (unsigned int col = 0; col < sl->bitmap.width; ++col) {
                const unsigned char v = sl->bitmap.buffer[row * sl->bitmap.pitch + col];
                if (!v) continue;
                const int px = gx + static_cast<int>(col), py = by + static_cast<int>(row);
                // 先铺一层几乎透明的黑在下方 2px，字就有了「压在上面」的厚度
                putPx(buf, w, fullH, px, py + 2, 0.0f, 0.0f, 0.0f,
                      0.35f * static_cast<float>(v) / 255.0f);
                putPx(buf, w, fullH, px, py, 1.0f, 1.0f, 1.0f,
                      static_cast<float>(v) / 255.0f);
            }
        }
    }

    // ④ 上传
    if (!gTex) {
        glGenTextures(1, &gTex);
        glBindTexture(GL_TEXTURE_2D, gTex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, gTex);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, fullH, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    gTexW = w;
    gTexH = fullH;
    return true;
}

}  // namespace

void setEnabled(bool on) { gOn = on; }
bool enabled() { return gOn && gReady; }
bool alive() { return gOn && gReady && gT0 > 0.0; }

void init() {
    if (FT_Init_FreeType(&gLib) != 0) { std::fprintf(stderr, "[bubble] FreeType 初始化失败\n"); return; }
    for (const char* p : kFontPaths) {
        if (FT_New_Face(gLib, p, 0, &gFace) == 0) {
            std::fprintf(stderr, "[bubble] 字体 %s\n", p);
            break;
        }
    }
    if (!gFace) { std::fprintf(stderr, "[bubble] 一个字体都打不开，气泡关掉\n"); return; }

    // 一个 quad + shader（固定管线风格，少写点代码）
    glGenVertexArrays(1, &gVao);
    glGenBuffers(1, &gVbo);
    glBindVertexArray(gVao);
    glBindBuffer(GL_ARRAY_BUFFER, gVbo);
    // 6 个顶点 × (x,y,u,v) = 24 个 float —— 之前只分配了 16 个，glBufferSubData 写 96 字节
    // 越界 → GL_INVALID_VALUE(0x501)，画不出来还不报错，就是这个。
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 24, nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, (void*)(sizeof(float) * 2));
    glBindVertexArray(0);

    gReady = true;
    blog("[init] ready, face=%p\n", (void*)gFace);
}

void say(const char* text, double now) {
    if (!text || !*text) return;
    if (gT0 > 0.0 && gText == text) return;      // 同一句还在挂：不重置计时
    gText = text;
    gT0 = now;
    gDirty = true;
    // 挂多久：按字数估说话时长（每字 0.16s）+ 收尾停 1.4s。
    // 之前写死 2.6 秒，十来个字的长句还没看完就没了。
    int chars = 0;
    for (unsigned char c : gText) if ((c & 0xC0) != 0x80) ++chars;
    gHold = 1.4 + 0.16 * static_cast<double>(chars < 1 ? 1 : chars);
}

void clear() { gT0 = 0.0; gText.clear(); }

void tick(double now, double dt) {
    (void)dt;
    gNow = now;
    if (!gReady || !gOn) return;
    if (gT0 <= 0.0) return;
    if (gDirty) {
        if (buildTexture(gText, 0.0)) gDirty = false;
    }
    // 一句最长挂 6 秒（防止某句话把气泡钉在那儿）
    if (now - gT0 > gHold + 1.5) gT0 = 0.0;
}

// 2026-11-11 01:4x：固定管线（glMatrixMode/glOrtho/glEnable(GL_TEXTURE_2D)）在这个
// context 里全是 GL_INVALID_VALUE —— 日志里 GL_VERSION 写着 Compatibility，
// 但 EGL 实际建出来的这层不吃那一套。所以老老实实写 shader，20 行的事。

static GLuint gProg = 0;

static GLuint compile(GLenum type, const char* src) {
    const GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof log, nullptr, log);
        blog("[shader] %s\n", log);
    }
    return sh;
}

static void ensureProgram() {
    if (gProg) return;
    static const char* vs =
        "#version 330 core\n"
        "layout(location=0) in vec2 aPos;\n"
        "layout(location=1) in vec2 aUv;\n"
        "uniform vec2 uRes;\n"
        "out vec2 vUv;\n"
        "void main(){ vUv=aUv; gl_Position=vec4(aPos/uRes*2.0-1.0, 0.0, 1.0); }\n"
        "\n"
        "void _unused(){}";
    static const char* fs =
        "#version 330 core\n"
        "in vec2 vUv;\n"
        "uniform sampler2D uTex;\n"
        "uniform float uAlpha;\n"
        "out vec4 o;\n"
        "void main(){ vec4 c=texture(uTex, vUv); o=vec4(c.rgb, c.a*uAlpha); }\n";
    const GLuint v = compile(GL_VERTEX_SHADER, vs);
    const GLuint f = compile(GL_FRAGMENT_SHADER, fs);
    gProg = glCreateProgram();
    glAttachShader(gProg, v);
    glAttachShader(gProg, f);
    glLinkProgram(gProg);
    GLint ok = 0;
    glGetProgramiv(gProg, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(gProg, sizeof log, nullptr, log); blog("[link] %s\n", log); }
    glDeleteShader(v);
    glDeleteShader(f);
}

void draw(int winW, int winH) {
    if (!alive() || gTexW <= 0 || winW <= 0 || winH <= 0) return;
    ensureProgram();
    if (!gProg) return;

    // 纹理已经是**物理像素**（见上面字号那段），这里 1:1 贴，不再放大 —— 这是不糊的关键
    const float scale = static_cast<float>(winW) / 1000.0f;
    const float bw = static_cast<float>(gTexW) * scale;
    const float bh = static_cast<float>(gTexH) * scale;
    const float cx = static_cast<float>(winW) * 0.5f;
    // 她的头顶在窗口里大约 0.90 高处（framebuffer 坐标从底部算）
    // 窗口顶部只剩约 110 物理像素，气泡（含小尖角）大约 100 高 —— 所以下沿贴着
    // 她头顶往上一点点，上沿正好压到窗口边。再高就裁掉了。
    const float by = static_cast<float>(winH) * 0.880f;   // 2026-11-11 02:5x 他说「把人物盖住了」——再往上，气泡底边必须在她头饰之上
    const float x0 = cx - bw * 0.5f, x1 = cx + bw * 0.5f;
    const float y0 = by, y1 = by + bh;

    // 淡入淡出
    const double t = gNow - gT0;
    float alpha = 1.0f;
    if (t < 0.0) alpha = 0.0f;
    else if (t < 0.16) alpha = static_cast<float>(t / 0.16);
    else if (t > gHold) alpha = static_cast<float>(std::max(0.0, 1.0 - (t - gHold) / 0.35));

    const float verts[] = {
        x0, y0, 0.0f, 1.0f,
        x1, y0, 1.0f, 1.0f,
        x1, y1, 1.0f, 0.0f,
        x0, y0, 0.0f, 1.0f,
        x1, y1, 1.0f, 0.0f,
        x0, y1, 0.0f, 0.0f,
    };

    glUseProgram(gProg);
    glUniform2f(glGetUniformLocation(gProg, "uRes"), static_cast<float>(winW), static_cast<float>(winH));
    glUniform1f(glGetUniformLocation(gProg, "uAlpha"), alpha);
    glUniform1i(glGetUniformLocation(gProg, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gTex);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(gVao);
    glBindBuffer(GL_ARRAY_BUFFER, gVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);
}

}  // namespace bubble
