// selftest：用 platform.h 开 500x900 透明窗口，画一个绿色三角，5 秒后自己退出。
// 目的：证明 platInit/platPollEvents/platSwapBuffers/platGetFramebufferSize/platSetPosition
// 这一整套在 niri(layer-shell + EGL desktop GL) 上真的出画、而且透明生效。
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <GL/gl.h>

extern "C" void* platGLProc(const char* name);

#define LOG(...) do { fprintf(stderr, "[selftest] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

#define GL_FNS \
  X(PFNGLATTACHSHADERPROC,          glAttachShader) \
  X(PFNGLBINDBUFFERPROC,            glBindBuffer) \
  X(PFNGLBINDVERTEXARRAYPROC,       glBindVertexArray) \
  X(PFNGLBUFFERDATAPROC,            glBufferData) \
  X(PFNGLCOMPILESHADERPROC,         glCompileShader) \
  X(PFNGLCREATEPROGRAMPROC,         glCreateProgram) \
  X(PFNGLCREATESHADERPROC,          glCreateShader) \
  X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
  X(PFNGLGENBUFFERSPROC,            glGenBuffers) \
  X(PFNGLGENVERTEXARRAYSPROC,       glGenVertexArrays) \
  X(PFNGLGETPROGRAMINFOLOGPROC,     glGetProgramInfoLog) \
  X(PFNGLGETPROGRAMIVPROC,          glGetProgramiv) \
  X(PFNGLGETSHADERINFOLOGPROC,      glGetShaderInfoLog) \
  X(PFNGLGETSHADERIVPROC,           glGetShaderiv) \
  X(PFNGLGETUNIFORMLOCATIONPROC,    glGetUniformLocation) \
  X(PFNGLLINKPROGRAMPROC,           glLinkProgram) \
  X(PFNGLSHADERSOURCEPROC,          glShaderSource) \
  X(PFNGLUNIFORM4FPROC,             glUniform4f) \
  X(PFNGLUSEPROGRAMPROC,            glUseProgram) \
  X(PFNGLVERTEXATTRIBPOINTERPROC,   glVertexAttribPointer)

#define X(type, name) static type p_##name = nullptr;
GL_FNS
#undef X

static bool load_gl() {
    int miss = 0;
#define X(type, name) p_##name = (type)platGLProc(#name); if (!p_##name) { LOG("缺 %s", #name); miss++; }
    GL_FNS
#undef X
    return miss == 0;
}

static const char* VS =
"#version 330 core\n"
"layout(location=0) in vec2 aPos;\n"
"uniform vec4 uColor;\n"
"out vec4 vColor;\n"
"void main(){ vColor=uColor; gl_Position=vec4(aPos,0.0,1.0); }\n";

static const char* FS =
"#version 330 core\n"
"in vec4 vColor;\n"
"out vec4 oColor;\n"
"void main(){ oColor=vColor; }\n";

static GLuint mk(GLenum t, const char* src) {
    GLuint s = p_glCreateShader(t);
    p_glShaderSource(s, 1, &src, nullptr);
    p_glCompileShader(s);
    GLint ok = 0;
    p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[1024]; GLsizei n = 0;
        p_glGetShaderInfoLog(s, sizeof buf, &n, buf);
        LOG("shader 编译失败: %.*s", (int)n, buf);
        exit(3);
    }
    return s;
}

int main(void) {
    const int LOGW = 500, LOGH = 900;

    if (!platInit(LOGW, LOGH, "pet-selftest")) {
        LOG("platInit 失败");
        return 1;
    }

    int fw = 0, fh = 0;
    platGetFramebufferSize(&fw, &fh);
    LOG("逻辑尺寸 = %dx%d", LOGW, LOGH);
    LOG("物理尺寸 = %dx%d  (期望 %dx%d, 倍数 %.2f)", fw, fh, LOGW * 2, LOGH * 2,
        (double)fw / LOGW);

    if (!load_gl()) { LOG("GL 入口缺失"); platShutdown(); return 1; }

    GLuint prog = p_glCreateProgram();
    p_glAttachShader(prog, mk(GL_VERTEX_SHADER, VS));
    p_glAttachShader(prog, mk(GL_FRAGMENT_SHADER, FS));
    p_glLinkProgram(prog);
    {
        GLint ok = 0;
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) {
            char buf[1024]; GLsizei n = 0;
            p_glGetProgramInfoLog(prog, sizeof buf, &n, buf);
            LOG("program 链接失败: %.*s", (int)n, buf);
            platShutdown();
            return 3;
        }
    }
    GLint u_color = p_glGetUniformLocation(prog, "uColor");

    GLuint vao = 0, vbo = 0;
    p_glGenVertexArrays(1, &vao);
    p_glBindVertexArray(vao);
    p_glGenBuffers(1, &vbo);
    p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    p_glEnableVertexAttribArray(0);
    p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);

    // 逻辑坐标里的三角：窗口左边三分之一处，尖朝上
    const float tri[] = {
        -0.75f, -0.80f,
        -0.25f, -0.80f,
        -0.50f,  0.80f,
    };

    p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof tri, tri, GL_STATIC_DRAW);

    int px0 = getenv("PET_POS_X") ? atoi(getenv("PET_POS_X")) : 100;
    int py0 = getenv("PET_POS_Y") ? atoi(getenv("PET_POS_Y")) : 100;
    platSetPosition(px0, py0);
    LOG("setPosition(%d,%d)", px0, py0);

    // PET_EDGE=1：画窗口四边 6px 青色边框，用来从截图量出窗口的真实物理矩形
    bool drawEdge = getenv("PET_EDGE") != nullptr;
    GLuint edgeVao = 0, edgeVbo = 0;
    if (drawEdge) {
        p_glGenVertexArrays(1, &edgeVao);
        p_glBindVertexArray(edgeVao);
        p_glGenBuffers(1, &edgeVbo);
        p_glBindBuffer(GL_ARRAY_BUFFER, edgeVbo);
        p_glEnableVertexAttribArray(0);
        p_glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);
        p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }

    const double t0 = platTime();
    long frames = 0;
    while (platTime() - t0 < 5.0 && !platShouldClose()) {
        platPollEvents();

        glViewport(0, 0, fw, fh);
        glClearColor(0.f, 0.f, 0.f, 0.f);        // 全透明
        glClear(GL_COLOR_BUFFER_BIT);

        p_glUseProgram(prog);
        p_glUniform4f(u_color, 0.f, 1.f, 0.f, 1.f);
        p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (drawEdge) {
            // 四边各一条 0.012 NDC 宽的边（青色 0,1,1）
            const float e = 0.012f;
            const float ring[] = {
                -1.f+e, 1.f-2*e,  1.f-e, 1.f-2*e,  1.f-e, 1.f,   -1.f+e, 1.f-2*e,  1.f-e, 1.f,  -1.f+e, 1.f,   // 顶
                -1.f+e, -1.f,     1.f-e, -1.f,     1.f-e, -1.f+2*e, -1.f+e, -1.f,  1.f-e, -1.f+2*e, -1.f+e, -1.f+2*e, // 底
                -1.f, 1.f,  -1.f+2*e, 1.f, -1.f+2*e, -1.f,  -1.f, 1.f, -1.f+2*e, -1.f, -1.f, -1.f,   // 左
                1.f-2*e, 1.f,  1.f, 1.f,  1.f, -1.f,  1.f-2*e, 1.f,  1.f, -1.f,  1.f-2*e, -1.f,     // 右
            };
            p_glBindVertexArray(edgeVao);
            p_glBindBuffer(GL_ARRAY_BUFFER, edgeVbo);
            p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof ring, ring, GL_DYNAMIC_DRAW);
            p_glUniform4f(u_color, 0.f, 1.f, 1.f, 1.f);
            glDrawArrays(GL_TRIANGLES, 0, 24);
            p_glBindVertexArray(vao);
            p_glBindBuffer(GL_ARRAY_BUFFER, vbo);
        }

        platSwapBuffers();
        if (frames == 1) LOG("READY");           // 截图脚本等这一行
        frames++;
        usleep(16000);
    }

    LOG("跑了 %ld 帧，退出", frames);
    platShutdown();
    return 0;
}
