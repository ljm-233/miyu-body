cmake_minimum_required(VERSION 3.16)
project(platform-wayland C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(WL REQUIRED wayland-client wayland-egl egl gl)

add_library(layershell STATIC layer-shell-protocol.c xdg-shell-protocol.c)
target_include_directories(layershell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(layershell PRIVATE ${WL_INCLUDE_DIRS})

add_library(platform_wayland STATIC platform_wayland.cpp)
target_include_directories(platform_wayland PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${WL_INCLUDE_DIRS})
target_link_libraries(platform_wayland PUBLIC layershell ${WL_LINK_LIBRARIES} dl)

add_executable(selftest selftest.cpp)
target_link_libraries(selftest PRIVATE platform_wayland)
