cmake_minimum_required(VERSION 3.16)
project(miyu_body C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WL REQUIRED wayland-client)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GL REQUIRED gl glew)

# Cubism Core（Live2D 官方静态库，linux x86_64）
add_library(Live2DCubismCore STATIC IMPORTED)
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/Core/lib/linux/x86_64/libLive2DCubismCore.a
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Core/include)

# Cubism 官方运行时的编译选项（Linux 桌面 GL）
add_compile_definitions(CSM_TARGET_LINUX_GL)

add_executable(body
  src/main.cpp
  src/layer-shell-protocol.c
  src/xdg-shell-protocol.c
)
target_include_directories(body PRIVATE
  ${WL_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS} ${GL_INCLUDE_DIRS}
  src Framework/src Core/include stb .
)
target_link_directories(body PRIVATE lib ${WL_LIBRARY_DIRS} ${EGL_LIBRARY_DIRS} ${GL_LIBRARY_DIRS})
# 静态库要按「依赖方在前、被依赖方在后」排：Framework 依赖 Core
target_link_libraries(body PRIVATE
  Framework
  Live2DCubismCore
  ${WL_LIBRARIES} ${EGL_LIBRARIES} ${GL_LIBRARIES} wayland-egl GLEW
)
