// alpha-probe: minimal per-pixel alpha test under XWayland + niri
// half-transparent red background, opaque green square on the left.
#include <stdio.h>
#include <stdlib.h>
#include <GLFW/glfw3.h>

static int W = 400, H = 400;

int main(int argc, char **argv) {
    int frames = (argc > 1 && argv[1][0] != 'p') ? atoi(argv[1]) : 0;   // 0 = run forever
    int premul = (argc > 1 && argv[1][0] == 'p');             // "pm" = premultiplied 0.5 red

    glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);   // force X11 (XWayland), not native Wayland
    if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
    fprintf(stderr, "glfw platform: %s\n",
            glfwGetPlatform() == GLFW_PLATFORM_X11 ? "X11" : "NOT-X11");

    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE); // request alpha visual
    glfwWindowHint(GLFW_FLOATING, GLFW_TRUE);

    GLFWwindow *w = glfwCreateWindow(W, H, "alpha-probe", NULL, NULL);
    if (!w) { fprintf(stderr, "create window failed\n"); glfwTerminate(); return 1; }

    glfwMakeContextCurrent(w);
    glfwSwapInterval(1);

    int n = 0;
    while (!glfwWindowShouldClose(w)) {
        glfwGetFramebufferSize(w, &W, &H);
        glViewport(0, 0, W, H);
        glDisable(GL_SCISSOR_TEST);

        // clear with real alpha via glClearColor + glClear on a transparent fb
        if (premul) glClearColor(0.5f, 0.0f, 0.0f, 0.5f);   // premultiplied half red
        else        glClearColor(1.0f, 0.0f, 0.0f, 0.5f);   // straight alpha half red
        glClear(GL_COLOR_BUFFER_BIT);

        // opaque green square on the left half
        glEnable(GL_SCISSOR_TEST);
        glScissor(0, 0, W / 2, H);          // left half, full height
        glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);

        glfwSwapBuffers(w);
        glfwPollEvents();

        if (frames && ++n >= frames) break;
        glfwWaitEventsTimeout(0.03);
    }

    if (frames) { glfwDestroyWindow(w); glfwTerminate(); }
    return 0;
}
