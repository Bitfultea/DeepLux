#!/usr/bin/env python3
"""为全部插件的 metadata.json 生成 ABI v2 ports 声明（阶段 1.2b）。

设计：
- 所有插件声明主载体端口 image（输入/输出），类型按类别标注。
- 几何/测量/检测插件额外声明其读写的命名数据端口（把原隐藏键 point1/fit_points 等
  显式化），类型用 DataType 名称。
- 端口 ID 模块内唯一、displayName 非空、类型有效（PluginManager 加载时校验）。

运行：python3 tests/acceptance/generate_ports.py
幂等：重复运行重写 ports 块。
"""

import json
import os

ROOT = os.path.dirname(os.path.abspath(__file__))
PLUGINS = os.path.join(ROOT, "..", "..", "src", "plugins")

# 类别 -> 主载体端口类型
CAT_IMAGE_TYPE = {
    "image_processing": "Image2D",
    "detection": "Image2D",
    "calibration": "Image2D",
    "geometry": "Any",
    "system": "Any",
    "variable": "Any",
    "logic": "Any",
    "communication": "Any",
    "hymson3d": "PointCloud3D",
}

# 每个插件的命名数据端口（输入/输出）。未列出者仅声明 image 载体。
NAMED_PORTS = {
    "FitLine": {
        "in": [("fit_points", "PointSet2D", "拟合点集", True)],
        "out": [("line_row1", "Number", "线行1"), ("line_col1", "Number", "线列1"),
                ("line_row2", "Number", "线行2"), ("line_col2", "Number", "线列2"),
                ("line_phi", "Number", "角度"), ("line_rho", "Number", "距离"),
                ("line_error", "Number", "拟合误差")],
    },
    "FitCircle": {
        "in": [("fit_points", "PointSet2D", "拟合点集", True)],
        "out": [("circle_center_x", "Number", "圆心X"), ("circle_center_y", "Number", "圆心Y"),
                ("circle_radius", "Number", "半径"), ("circle_error", "Number", "拟合误差")],
    },
    "FindCircle": {
        "in": [],
        "out": [("circle_center_x", "Number", "圆心X"), ("circle_center_y", "Number", "圆心Y"),
                ("circle_radius", "Number", "半径"), ("circle_score", "Number", "得分")],
    },
    "MeasurementInput": {
        "in": [],
        "out": [("point1", "Point2D", "点1"), ("point2", "Point2D", "点2"),
                ("point", "Any", "点"), ("line", "Line2D", "线"),
                ("line1", "Line2D", "线1"), ("line2", "Line2D", "线2"),
                ("plane", "Plane3D", "平面"), ("fit_points", "PointSet2D", "拟合点集")],
    },
    "DistancePP": {
        "in": [("point1", "Point2D", "点1", True), ("point2", "Point2D", "点2", True)],
        "out": [("distance", "Number", "距离")],
    },
    "DistancePL": {
        "in": [("point", "Point2D", "点", True), ("line", "Line2D", "线", True)],
        "out": [("distance", "Number", "距离")],
    },
    "LinesDistance": {
        "in": [("line1", "Line2D", "线1", True), ("line2", "Line2D", "线2", True)],
        "out": [("distance", "Number", "距离")],
    },
    "MeasureGap": {
        "in": [("line1", "Line2D", "线1", True), ("line2", "Line2D", "线2", True)],
        "out": [("gap", "Number", "间隙")],
    },
    "PointSurfaceDistance": {
        "in": [("point", "Point3D", "点", True), ("plane", "Plane3D", "面", True)],
        "out": [("distance", "Number", "距离")],
    },
    "MeasureLine": {"in": [], "out": [("line_length", "Number", "线长"), ("line_angle", "Number", "角度")]},
    "MeasureRect": {"in": [], "out": [("rect_width", "Number", "宽"), ("rect_height", "Number", "高"),
                                      ("rect_area", "Number", "面积")]},
    "Blob": {"in": [], "out": [("blob_count", "Integer", "斑点数")]},
    "QRCode": {"in": [], "out": [("qr_result", "String", "解码文本")]},
    "LoadPointCloud": {"in": [], "out": [("image", "PointCloud3D", "点云")]},
}

# 阶段 3.2: 控制插件的控制端口（与数据端口分离）
CONTROL_PORTS = {
    "条件分支": {"out": [("true", "control"), ("false", "control")]},
    "条件判断": {"out": [("true", "control"), ("false", "control")]},
    "循环": {"out": [("body", "control"), ("done", "control")]},
    "条件循环": {"out": [("body", "control"), ("done", "control")]},
    "停止循环": {"out": [("stop", "control")]},
}


def build_ports(name, category):
    img_type = CAT_IMAGE_TYPE.get(category, "Any")
    inputs = []
    outputs = []

    named = NAMED_PORTS.get(name)
    # 源模块（无图像输入）：不声明 image 输入
    source_only = name in ("GrabImage", "LoadPointCloud", "SystemTime", "MeasurementInput")
    sink_no_output = False

    if not source_only:
        inputs.append({"id": "image", "displayName": "输入", "type": img_type,
                       "required": False, "multiple": False})
    outputs.append({"id": "image", "displayName": "输出", "type": img_type,
                    "required": False, "multiple": False})

    if named:
        for pid, ptype, disp, req in named.get("in", []):
            inputs.append({"id": pid, "displayName": disp, "type": ptype,
                           "required": bool(req), "multiple": False})
        for entry in named.get("out", []):
            pid, ptype, disp = entry[0], entry[1], entry[2]
            if pid == "image":
                continue  # 已有载体
            outputs.append({"id": pid, "displayName": disp, "type": ptype,
                            "required": False, "multiple": False})

    # 阶段 3.2: 控制端口（与数据端口分离）
    for entry in CONTROL_PORTS.get(name, {}).get("out", []):
        pid, kind = entry[0], entry[1]
        outputs.append({"id": pid, "displayName": pid, "type": "Boolean",
                        "required": False, "multiple": False, "control": True})

    return {"inputs": inputs, "outputs": outputs}


def main():
    count = 0
    for dirpath, _dirs, files in os.walk(PLUGINS):
        if "metadata.json" not in files:
            continue
        path = os.path.join(dirpath, "metadata.json")
        with open(path, "r", encoding="utf-8") as f:
            meta = json.load(f)
        name = meta.get("name")
        category = meta.get("category", "")
        if not name or category == "camera":
            continue  # 相机走 ICameraPlugin，不声明 IModule 端口
        meta["ports"] = build_ports(name, category)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(meta, f, indent=4, ensure_ascii=False)
            f.write("\n")
        count += 1
    print("ports declared for", count, "plugins")


if __name__ == "__main__":
    main()
