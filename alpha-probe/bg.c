// opaque pure-blue plate used as a known backdrop for the alpha test
#include <GLFW/glfw3.h>
#include <stdio.h>
int main(void){
    if(!glfwInit()) return 1;
    glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_FALSE);
    GLFWwindow *w=glfwCreateWindow(1000,900,"alpha-bg",NULL,NULL);
    if(!w) return 1;
    glfwMakeContextCurrent(w); glfwSwapInterval(1);
    while(!glfwWindowShouldClose(w)){
        int fbw,fbh; glfwGetFramebufferSize(w,&fbw,&fbh);
        glViewport(0,0,fbw,fbh);
        glClearColor(0.0f,0.0f,1.0f,1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(w); glfwPollEvents(); glfwWaitEventsTimeout(0.03);
    }
    return 0;
}
