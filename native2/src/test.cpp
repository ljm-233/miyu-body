// 最小验证：X11 + GLX + 桌面 GL + 透明窗口 + 贴图采样
// 目标只有一个——三角形上能不能看见 texture_00 的颜色。
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static const char* kTex = "/home/link/.dsh/miyu/body/model/大肥鱼.4096/texture_00.png";

int main() {
    if (!glfwInit()) { std::fprintf(stderr, "glfwInit 失败\n"); return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);
    GLFWwindow* w = glfwCreateWindow(500, 700, "native2-test", nullptr, nullptr);
    if (!w) { std::fprintf(stderr, "建窗口失败（X11 可用吗？DISPLAY=%s）\n", getenv("DISPLAY") ?: "空"); return 1; }
    glfwMakeContextCurrent(w);
    glewExperimental = GL_TRUE;
    GLenum ge = glewInit();
    std::fprintf(stderr, "[t] GL=%s | %s | glew=%d\n", glGetString(GL_VERSION), glGetString(GL_RENDERER), ge);

    // 贴图
    int iw = 0, ih = 0, n = 0;
    unsigned char* d = stbi_load(kTex, &iw, &ih, &n, 4);
    if (!d) { std::fprintf(stderr, "[t] 读不到贴图\n"); return 1; }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, d);
    stbi_image_free(d);
    std::fprintf(stderr, "[t] 贴图 %dx%d id=%u err=0x%x\n", iw, ih, tex, glGetError());

    const char* vs =
        "#version 120\n"
        "attribute vec2 p; attribute vec2 uv; varying vec2 v;\n"
        "void main(){ v=uv; gl_Position=vec4(p,0.0,1.0); }\n";
    const char* fs =
        "#version 120\n"
        "varying vec2 v; uniform sampler2D tex;\n"
        "void main(){ gl_FragColor=texture2D(tex, v); }\n";
    auto mk = [](GLenum t, const char* src) {
        GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, nullptr); glCompileShader(s);
        GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, nullptr, log); std::fprintf(stderr, "[t] shader 失败: %s\n", log); }
        return s;
    };
    GLuint v = mk(GL_VERTEX_SHADER, vs), f = mk(GL_FRAGMENT_SHADER, fs);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v); glAttachShader(prog, f);
    glBindAttribLocation(prog, 0, "p");
    glBindAttribLocation(prog, 1, "uv");
    glLinkProgram(prog);
    GLint lk = 0; glGetProgramiv(prog, GL_LINK_STATUS, &lk);
    if (!lk) { char log[512]; glGetProgramInfoLog(prog, 512, nullptr, log); std::fprintf(stderr, "[t] link 失败: %s\n", log); }

    const GLint lp = glGetUniformLocation(prog, "tex");
    std::fprintf(stderr, "[t] uniform tex=%d uv槽=%d\n", lp, glGetAttribLocation(prog, "uv"));

    const float verts[] = {
        -1.0f, -1.0f, 0.0f, 0.0f,
         0.0f,  1.4f, 0.5f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
    };

    while (!glfwWindowShouldClose(w)) {
        int W = 0, H = 0;
        glfwGetFramebufferSize(w, &W, &H);
        glViewport(0, 0, W, H);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(prog);
        glUniform1i(lp, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, verts);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glfwSwapBuffers(w);
        glfwPollEvents();
        if (glfwGetKey(w, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
    }
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
