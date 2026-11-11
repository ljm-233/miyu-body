#!/usr/bin/env python3
"""摄像头模式：把镜头里你的脸，换算成身体能用的几个数。

2026-11-11 23:2x 他提的设计——两套模式。米鱼模式是现在这样（自己动、自己的状态），
摄像头模式是识别他的动作和表情，映到身体上。

**为什么写这个而不是用现成的**：算法全都有现成的（mediapipe 一次给 52 个 blendshape
加一个 4×4 变换矩阵），但接到我们这具身体上的那一公里没有——现成的
`facial-landmarks-for-cubism` 是 JS 的、给 Web SDK 用，我们这个是 C++ Native。
所以这里只做「算 + 写文件」，映射到 Cubism 参数在 C++ 那边读。

输出 `state/face.json`（和 mood.json、world.json 一个套路，main 每帧读）：

    {"ok":true,
     "yaw":-12.3, "pitch":4.1, "roll":-1.8,     头转，单位度
     "eyeL":0.92, "eyeR":0.88,                  眼睛睁开 0 闭 1 睁
     "mouth":0.15,                             嘴张 0~1
     "eyeX":0.30, "eyeY":-0.10,                眼球看哪 -1~1
     "at":1794400000}

**不装 cv2、不算 EAR/MAR、不做 solvePnP**。mediapipe 的 tasks API 直接给：
- `output_face_blendshapes` → eyeBlinkLeft/Right、jawOpen、eyeLookXxx
- `output_facial_transformation_matrixes` → 头姿（4×4，取旋转部分解欧拉角）

用法：
    face-track.py                 # 一直跑，30fps 写 face.json
    face-track.py --once          # 只跑一帧，打印结果（调试）
    face-track.py --fps 15        # 降频（省 CPU）
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time

import numpy as np

HOME = os.path.expanduser("~")
STATE = os.path.join(HOME, ".dsh/miyu/body/native3/state")
OUT = os.path.join(STATE, "face.json")
MODEL = os.path.join(HOME, ".dsh/miyu/body/native3/tools/face_landmarker.task")
DEV = "/dev/video0"

# 标定：正对镜头时各量的中位值。第一次跑的时候用 --calibrate 采一遍，存这儿。
CALIB = os.path.join(STATE, "face-calib.json")


def load_calib():
    try:
        with open(CALIB, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {"yaw": 0.0, "pitch": 0.0, "roll": 0.0, "open": 1.0}


def grab(w, h, dev):
    """ffmpeg 抓一帧 rawvideo。不依赖 cv2。"""
    raw = subprocess.run(
        ["ffmpeg", "-loglevel", "quiet", "-f", "v4l2", "-video_size", f"{w}x{h}",
         "-i", dev, "-frames:v", "1", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
        capture_output=True, timeout=10).stdout
    if len(raw) != w * h * 3:
        return None
    return np.frombuffer(raw, np.uint8).reshape(h, w, 3)


def euler_from_matrix(m):
    """4×4 → (pitch, yaw, roll)，度。
    相机坐标：X 右、Y 上、Z 朝镜头外。人的脸朝 -Z。
    """
    r = np.array(m, dtype=float).reshape(4, 4)[:3, :3]
    # 转成「脸相对相机」的旋转，再拆
    sy = math.sqrt(r[0, 0] ** 2 + r[1, 0] ** 2)
    if sy > 1e-6:
        pitch = math.degrees(math.atan2(r[2, 1], r[2, 2]))
        yaw = math.degrees(math.atan2(-r[2, 0], sy))
        roll = math.degrees(math.atan2(r[1, 0], r[0, 0]))
    else:
        pitch = math.degrees(math.atan2(-r[1, 2], r[1, 1]))
        yaw = math.degrees(math.atan2(-r[2, 0], sy))
        roll = 0.0
    return pitch, yaw, roll


def read_face(res):
    """从一次检测结果里取我们要的标量。"""
    if not res.face_blendshapes:
        return None
    bs = {c.category_name: c.score for c in res.face_blendshapes[0]}
    blink_l = bs.get("eyeBlinkLeft", 0.0)
    blink_r = bs.get("eyeBlinkRight", 0.0)
    # blink 是「闭的程度」，我们要「睁的程度」= 1 - blink
    eye_l = max(0.0, min(1.0, 1.0 - blink_l))
    eye_r = max(0.0, min(1.0, 1.0 - blink_r))
    mouth = max(0.0, min(1.0, bs.get("jawOpen", 0.0)))
    # 眼球：右眼的左右上下（左眼那套在镜像下不稳定，取平均）
    ex = (bs.get("eyeLookOutLeft", 0.0) - bs.get("eyeLookInLeft", 0.0)
          + bs.get("eyeLookInRight", 0.0) - bs.get("eyeLookOutRight", 0.0)) * 0.5
    ey = (bs.get("eyeLookUpLeft", 0.0) + bs.get("eyeLookUpRight", 0.0)
          - bs.get("eyeLookDownLeft", 0.0) - bs.get("eyeLookDownRight", 0.0)) * 0.5
    yaw = pitch = roll = 0.0
    if res.facial_transformation_matrixes:
        pitch, yaw, roll = euler_from_matrix(res.facial_transformation_matrixes[0])
    return dict(yaw=yaw, pitch=pitch, roll=roll,
                eyeL=eye_l, eyeR=eye_r, mouth=mouth,
                eyeX=max(-1.0, min(1.0, ex)), eyeY=max(-1.0, min(1.0, ey)))


def write_state(d, ok=True):
    d = dict(d)
    d["ok"] = ok
    d["at"] = int(time.time())
    tmp = OUT + ".tmp"
    os.makedirs(STATE, exist_ok=True)
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False)
    os.replace(tmp, OUT)      # 原子替换，读的一边不会读到半个文件


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--dev", default=DEV)
    ap.add_argument("--w", type=int, default=640)
    ap.add_argument("--h", type=int, default=480)
    a = ap.parse_args()

    # mediapipe 的日志太吵，警告级别以上才出
    os.environ.setdefault("GLOG_minloglevel", "2")
    import mediapipe as mp
    from mediapipe.tasks import python as mpp
    from mediapipe.tasks.python import vision

    if not os.path.exists(MODEL):
        print(f"缺模型：{MODEL}（下载后要 unzip -t 验一下完整性）", file=sys.stderr)
        return 1
    opts = vision.FaceLandmarkerOptions(
        base_options=mpp.BaseOptions(model_asset_path=MODEL),
        running_mode=vision.RunningMode.VIDEO, num_faces=1,
        output_face_blendshapes=True,
        output_facial_transformation_matrixes=True)
    lm = vision.FaceLandmarker.create_from_options(opts)

    if a.once:
        raw = grab(a.w, a.h, a.dev)
        if raw is None:
            print("抓帧失败", file=sys.stderr)
            return 1
        img = mp.Image(image_format=mp.ImageFormat.SRGB, data=raw)
        res = lm.detect_for_video(img, int(time.time() * 1000))
        d = read_face(res)
        print(json.dumps(d, ensure_ascii=False, indent=1) if d else "没检测到脸")
        return 0 if d else 2

    period = 1.0 / max(1, a.fps)
    lost = 0
    smooth = None
    alpha = 0.45          # 一阶低通，太跟手会抖、太慢会拖
    calib = load_calib()
    print(f"跑起来了：{a.dev} @ {a.w}x{a.h} {a.fps}fps → {OUT}")
    try:
        while True:
            t0 = time.time()
            raw = grab(a.w, a.h, a.dev)
            if raw is None:
                lost += 1
                if lost > 5:
                    write_state({"yaw": 0, "pitch": 0, "roll": 0, "eyeL": 1, "eyeR": 1,
                                 "mouth": 0, "eyeX": 0, "eyeY": 0}, ok=False)
                time.sleep(period)
                continue
            img = mp.Image(image_format=mp.ImageFormat.SRGB, data=raw)
            res = lm.detect_for_video(img, int(time.time() * 1000))
            d = read_face(res)
            if d is None:
                lost += 1
                if lost > 5:
                    # 脸离开画面：写 ok=false，C++ 那边渐变回默认，不硬切
                    write_state({"yaw": 0, "pitch": 0, "roll": 0, "eyeL": 1, "eyeR": 1,
                                 "mouth": 0, "eyeX": 0, "eyeY": 0}, ok=False)
                    smooth = None
            else:
                lost = 0
                for k in ("yaw", "pitch", "roll"):
                    d[k] -= calib.get(k, 0.0)
                if smooth is None:
                    smooth = dict(d)
                else:
                    for k in smooth:
                        smooth[k] += (d[k] - smooth[k]) * alpha
                write_state(smooth)
            dt = time.time() - t0
            if dt < period:
                time.sleep(period - dt)
    except KeyboardInterrupt:
        print("\n停了")
        write_state({"yaw": 0, "pitch": 0, "roll": 0, "eyeL": 1, "eyeR": 1,
                     "mouth": 0, "eyeX": 0, "eyeY": 0}, ok=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
