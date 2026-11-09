// 桌宠平台层接口（Wayland / wlr-layer-shell 版）
// 签名固定，别改；别的文件按这个调用。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

bool   platInit(int w, int h, const char* title);
void   platPollEvents();
void   platGetFramebufferSize(int* w, int* h);   // 物理像素（已乘 buffer scale）
void   platSwapBuffers();
bool   platShouldClose();
double platTime();                               // CLOCK_MONOTONIC 秒
void   platSetPosition(int x, int y);            // 逻辑像素，相对屏幕左上角
void   platShutdown();

// 桌宠专用：按 RGBA 的 alpha 设置输入区域（X11 版是空实现）
void   platSetInputRegionFromAlpha(const unsigned char* rgba, int w, int h, int threshold);

#ifdef __cplusplus
}
#endif
