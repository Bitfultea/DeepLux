# 固定验收工程与测试数据（阶段 0.1）

本目录承载"固定验收工程 + 固定测试数据 + 预期结果/允许误差"的自动化验收基础，对应
《DeepLux C++ 重构版完整能力建设计划》阶段 0.1。

## 目录结构

```text
tests/acceptance/
├── generate_data.py        # 生成确定性测试数据（无随机性、可重复）
├── data/                   # 固定测试数据（图像 / 点云），不依赖用户目录
├── projects/               # 固定验收工程（JSON，图像路径用 @ACCEPTANCE_DATA@ 占位）
├── expected/               # 每个数据文件的真实几何值与允许误差（JSON）
└── README.md
```

对应自动化测试：`tests/test_acceptance_flows.cpp`（CTest 目标 `test_acceptance_flows`）。

## 设计约束

- 测试数据全部落在本目录，不读取用户目录中的临时文件。
- 几何参数固定、无随机性，保证结果可断言、可回归。
- 验收工程文件可移植：图像路径使用 `@ACCEPTANCE_DATA@` 占位符，测试运行时替换为
  `tests/acceptance/data` 的绝对路径。
- 预期结果与允许误差独立于工程文件存放，便于复核与调整。

## 重新生成测试数据

```bash
python3 tests/acceptance/generate_data.py
```

> 生成脚本无随机性，重复运行产物一致。若修改几何参数，请同步更新 `expected/*.json`
> 与下文"各验收流程"中的预期值。

## 运行验收测试

```bash
cmake --build build --target test_acceptance_flows
ctest --test-dir build -R test_acceptance_flows --output-on-failure
```

## 六个验收流程与状态

| 流程 | 数据 | 验收工程 | 预期/误差 | 状态 |
| --- | --- | --- | --- | --- |
| 找圆流程 | `circle_640x480.png` | `projects/accept_findcircle.json` | `expected/circle_640x480.json` | ✅ 已接入自动化 |
| 点集→直线拟合 | 固定共线点集 | `projects/accept_fitline.json` | `expected/fitline_points.json` | ✅ 已接入自动化 |
| 2D 几何测量 | `two_points_640x480.png` | `projects/accept_distancepp.json` | `expected/two_points_640x480.json` | ✅ 已接入自动化 |
| 3D 点云测量 | `plane_z5.ply` | `projects/accept_point_surface.json` | `expected/plane_z5.json` | ✅ 已接入自动化 |
| 条件/循环/并行流程 | 无需图像 | `projects/accept_controlflow.json` | 执行顺序断言 | ✅ 已接入自动化（If 真分支执行/假分支跳过） |
| PLC/相机/AI 模拟流程 | 模拟器 | 相机以 `GrabImage(File)` 为无硬件模拟源 | 契约测试 | 部分：相机模拟已用文件源；PLC/AI 需设备模拟器 |

## 已接入：找圆流程

- 流程：`GrabImage(File)` → `FindCircle`。
- 数据：640×480 深色背景，白色实心圆，圆心 (320, 240)，半径 100。
- 断言：`circle_center_x/y`、`circle_radius` 与预期差 ≤ 允许误差（中心 3px、半径 3px）。

## GUI 截图验收约定

- 桌面窗口截图尺寸：`1920×1080`；紧凑窗口截图尺寸：`1280×800`。
- 每张截图应能体现：主视图叠加结果、检查器参数与结果页、流程画布节点状态、运行日志。
- 截图文件命名：`<流程名>_<desktop|compact>_<主题>.png`，存放于阶段验收记录目录。

## 已接入：点集→直线拟合流程

- 流程：`MeasurementInput(point_set)` → `FitLine`。
- 数据：沿 (120,360)→(520,120) 直线采样的 9 个共线点（固定、无随机）。
- 断言：`line_error` ≤ 允许误差（共线点 LS 拟合误差≈0）；拟合线经过已知线段中点附近。
- 说明：为支撑该流程，MeasurementInput 新增 `point_set` 模式（输出 `fit_points` 点集），
  并注册 `QVector<QPointF>` 元类型。

## 待办

1. 为图像→点集提取（ROI/特征点）补充验收工程；当前点集由 MeasurementInput 直接提供。
2. 为条件、循环和 Parallel 补充可视化 GUI 验收工程；核心执行顺序与并发语义已有自动测试。
3. PLC/相机/AI 先建设备模拟器与契约测试，再接入模拟流程验收。
