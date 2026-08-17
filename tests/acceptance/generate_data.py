#!/usr/bin/env python3
"""生成固定验收工程所需的确定性测试数据。

设计约束（对应总体计划阶段 0.1）：
- 测试数据不依赖用户目录中的临时文件；全部落在 tests/acceptance/data/。
- 所有几何参数固定、无随机性，保证可重复与可断言。
- 每张图的真实几何写入 tests/acceptance/expected/*.json 供 C++ 验收测试比对。

运行方式：
    python3 tests/acceptance/generate_data.py
"""

import json
import os

import cv2
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_DIR = os.path.join(HERE, "data")
EXPECTED_DIR = os.path.join(HERE, "expected")

# 统一图像尺寸（同时作为 GUI 截图验收的参考内容尺寸）
IMG_W = 640
IMG_H = 480


def _write_image(name: str, img: np.ndarray) -> str:
    path = os.path.join(DATA_DIR, name)
    if not cv2.imwrite(path, img):
        raise RuntimeError(f"failed to write {path}")
    return path


def _write_expected(name: str, payload: dict) -> None:
    path = os.path.join(EXPECTED_DIR, name)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(payload, f, ensure_ascii=False, indent=2)
        f.write("\n")


def make_circle_image() -> None:
    """找圆流程用图：深色背景 + 一个高亮圆。

    真实值：圆心 (320, 240)，半径 100。
    """
    img = np.zeros((IMG_H, IMG_W), dtype=np.uint8)
    cx, cy, r = 320, 240, 100
    cv2.circle(img, (cx, cy), r, 255, thickness=-1)
    # 加一圈清晰边缘，利于 Canny/Hough 稳定检出
    cv2.circle(img, (cx, cy), r, 255, thickness=2)
    _write_image("circle_640x480.png", img)
    _write_expected(
        "circle_640x480.json",
        {
            "image": "circle_640x480.png",
            "circle_center_x": cx,
            "circle_center_y": cy,
            "circle_radius": r,
            "tolerance_center_px": 3.0,
            "tolerance_radius_px": 3.0,
        },
    )


def make_line_image() -> None:
    """2D 几何测量用图：一条已知角度的直线。

    真实值：从 (120, 360) 到 (520, 120) 的线段。
    """
    img = np.zeros((IMG_H, IMG_W), dtype=np.uint8)
    p1 = (120, 360)
    p2 = (520, 120)
    cv2.line(img, p1, p2, 255, thickness=3)
    _write_image("line_640x480.png", img)
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    _write_expected(
        "line_640x480.json",
        {
            "image": "line_640x480.png",
            "line_x1": p1[0],
            "line_y1": p1[1],
            "line_x2": p2[0],
            "line_y2": p2[1],
            "line_length": float(np.hypot(dx, dy)),
            "tolerance_endpoint_px": 4.0,
            "tolerance_length_px": 4.0,
        },
    )


def make_two_points_image() -> None:
    """两点距离测量用图：两个高亮点。

    真实值：P1 (200, 200)，P2 (440, 320)。距离 = 280.0（勾股）。
    """
    img = np.zeros((IMG_H, IMG_W), dtype=np.uint8)
    p1 = (200, 200)
    p2 = (440, 320)
    for p in (p1, p2):
        cv2.circle(img, p, 6, 255, thickness=-1)
    _write_image("two_points_640x480.png", img)
    dist = float(np.hypot(p2[0] - p1[0], p2[1] - p1[1]))
    _write_expected(
        "two_points_640x480.json",
        {
            "image": "two_points_640x480.png",
            "point1": list(p1),
            "point2": list(p2),
            "distance": dist,
            "tolerance_distance_px": 2.0,
        },
    )


def make_pointcloud_plane() -> None:
    """3D 点云测量用数据：z=5 平面上的规则网格点云（PLY，ascii）。

    真实值：所有点 z=5.0；x∈[0,10]，y∈[0,6]，步长 0.5。
    """
    xs = np.arange(0.0, 10.0 + 1e-9, 0.5)
    ys = np.arange(0.0, 6.0 + 1e-9, 0.5)
    zz = 5.0
    pts = [(x, y, zz) for y in ys for x in xs]
    path = os.path.join(DATA_DIR, "plane_z5.ply")
    with open(path, "w", encoding="utf-8") as f:
        f.write("ply\n")
        f.write("format ascii 1.0\n")
        f.write(f"element vertex {len(pts)}\n")
        f.write("property float x\n")
        f.write("property float y\n")
        f.write("property float z\n")
        f.write("end_header\n")
        for x, y, z in pts:
            f.write(f"{x:.3f} {y:.3f} {z:.3f}\n")
    _write_expected(
        "plane_z5.json",
        {
            "point_cloud": "plane_z5.ply",
            "vertex_count": len(pts),
            "plane_z": zz,
            "probe_point": [4.0, 3.0, 8.0],
            "point_surface_distance": 3.0,
            "tolerance_z": 1e-3,
            "tolerance_distance": 1e-3,
        },
    )


def main() -> None:
    os.makedirs(DATA_DIR, exist_ok=True)
    os.makedirs(EXPECTED_DIR, exist_ok=True)
    make_circle_image()
    make_line_image()
    make_two_points_image()
    make_pointcloud_plane()
    print("acceptance data generated in", DATA_DIR)


if __name__ == "__main__":
    main()
