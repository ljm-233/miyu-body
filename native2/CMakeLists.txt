cmake_minimum_required(VERSION 3.16)
project(miyu_native C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(CSM_TARGET_LINUX_GL)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLFW REQUIRED glfw3)
pkg_check_modules(GL REQUIRED gl glew)

add_library(Live2DCubismCore STATIC IMPORTED)
set_target_properties(Live2DCubismCore PROPERTIES
  IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/Core/lib/linux/x86_64/libLive2DCubismCore.a
  INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/Core/include)

add_executable(miyu src/main.cpp)
target_include_directories(miyu PRIVATE
  ${GLFW_INCLUDE_DIRS} ${GL_INCLUDE_DIRS}
  Framework/src Core/include stb .)
target_link_directories(miyu PRIVATE lib ${GLFW_LIBRARY_DIRS} ${GL_LIBRARY_DIRS})
target_link_libraries(miyu PRIVATE
  Framework Live2DCubismCore
  ${GLFW_LIBRARIES} ${GL_LIBRARIES} GLEW X11)
