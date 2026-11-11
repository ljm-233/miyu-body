#!/bin/sh
# 摄像头模式的启动包装：把环境变量都收在这儿，C++ 那边只管起这个脚本。
#
# 2026-11-12 01:2x：他要「不要手动启动，跟着开关来」。所以这个脚本由
# miyu-wayland 在设置里把「摄像头模式」打开时 fork 出来，关掉时 kill。
#
# 三个必须的环境变量：
#   __EGL_VENDOR_LIBRARY_FILENAMES —— 指定用 N 卡（RTX 5060）跑 mediapipe 的 GPU delegate。
#       注意不能用 GLX 那套 __NV_PRIME_RENDER_OFFLOAD，mediapipe 走的是 EGL，那个不生效。
#   GLOG_minloglevel —— mediapipe 的日志太吵，只留警告以上。
#   MIYU_FACE_FPS —— 帧率，默认 30（摄像头硬上限）。
cd "$HOME/.dsh/miyu/body/native3" || exit 1
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/10_nvidia.json
export GLOG_minloglevel=2
exec ./.venv/bin/python -u tools/face-track.py --gpu --fps "${MIYU_FACE_FPS:-30}"
