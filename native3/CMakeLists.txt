cmake_minimum_required(VERSION 3.16)
project(miyu_native C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
add_compile_definitions(CSM_TARGET_LINUX_GL)
# 自己写的代码一律 -Wall -Wextra（要求「0 警告」）。第三方头（Cubism Framework /
# stb / 协议头）用 SYSTEM include 排除，那些警告改不动也不该改。
add_compile_options(-Wall -Wextra)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FREETYPE REQUIRED freetype2)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GL REQUIRED gl glew)
# Wayland 版要的：wayland-client（协议）+ wayland-egl（wl_egl_window）+ EGL
pkg_check_modules(WL REQUIRED wayland-client wayland-egl egl)

add_library(Live2DCubismCore STATIC IMPORTED)
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/Core/lib/linux/x86_64/libLive2DCubismCore.a
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Core/include)

# ── 目标 1：miyu —— X11(XWayland) + GLFW。备胎，行为已和 native2 对齐。
add_executable(miyu src/main.cpp src/ctl.cpp src/plan.cpp src/web.cpp src/bubble.cpp src/platform_x11.cpp)
target_include_directories(miyu PRIVATE ${FREETYPE_INCLUDE_DIRS}
  ${GLFW_INCLUDE_DIRS} ${GL_INCLUDE_DIRS}
  .)
target_include_directories(miyu SYSTEM PRIVATE Framework/src Core/include stb)
target_link_directories(miyu PRIVATE lib ${GLFW_LIBRARY_DIRS} ${GL_LIBRARY_DIRS})
target_link_libraries(miyu PRIVATE
  Framework Live2DCubismCore
  ${GLFW_LIBRARIES} ${GL_LIBRARIES} GLEW X11 ${FREETYPE_LIBRARIES})

# ── 目标 2：miyu-wayland —— 原生 Wayland layer-shell + EGL 桌面 GL。
# 正路：透明真生效、逐像素点击穿透（wl_surface.set_input_region）。
#
# 三件不能省的事：
#   · GLEW 仍然要链 —— main.cpp 和 Cubism 的 GL 渲染器都 #include <GL/glew.h>，
#     gl* 在那里是宏，展开成 libGLEW 里的 __glew* 变量，不链就是一堆未定义符号。
#     EGL 下 glewInit() 返回 4(GLEW_ERROR_NO_GLX_DISPLAY)，但指针**全都解析到了**
#     （实测 44 个常用入口 0 个空指针：GLEW 退回 glXGetProcAddress，那一步不需要
#     有 GLX display），所以返回值忽略掉就行 —— 见 main.cpp 里那行注释。
#   · xdg-shell 协议代码必须链 —— layer-shell 的 get_popup 引用 xdg_popup_interface。
#   · alpha_region.c 是纯 C（按 alpha 扫矩形带），别塞进 C++ 编译器。
add_executable(miyu-wayland
  src/main.cpp
  src/ctl.cpp
  src/plan.cpp
  src/web.cpp
  src/platform_wayland.cpp
  src/bubble.cpp
  src/layer-shell-protocol.c
  src/xdg-shell-protocol.c
  src/xdg-output-protocol.c
  src/alpha_region.c)
target_include_directories(miyu-wayland PRIVATE
  ${GL_INCLUDE_DIRS} ${WL_INCLUDE_DIRS}
  .)
target_include_directories(miyu-wayland SYSTEM PRIVATE Framework/src Core/include stb)
target_link_directories(miyu-wayland PRIVATE lib ${GL_LIBRARY_DIRS} ${WL_LIBRARY_DIRS})
find_package(PkgConfig REQUIRED)
target_include_directories(miyu-wayland PRIVATE ${FREETYPE_INCLUDE_DIRS})
target_link_libraries(miyu-wayland PRIVATE
  Framework Live2DCubismCore
  ${WL_LIBRARIES} ${GL_LIBRARIES} GLEW dl m ${FREETYPE_LIBRARIES})
