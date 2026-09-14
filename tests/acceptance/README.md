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

## 验收流程与状态

| 流程 | 数据 | 验收工程 | 预期/误差 | 状态 |
| --- | --- | --- | --- | --- |
| 找圆流程 | `circle_640x480.png` | `projects/accept_findcircle.json` | `expected/circle_640x480.json` | ✅ 已接入自动化 |
| 点集→直线拟合 | 固定共线点集 | `projects/accept_fitline.json` | `expected/fitline_points.json` | ✅ 已接入自动化 |
| 2D 几何测量 | `two_points_640x480.png` | `projects/accept_distancepp.json` | `expected/two_points_640x480.json` | ✅ 已接入自动化 |
| 3D 点云测量 | `plane_z5.ply` | `projects/accept_point_surface.json` | `expected/plane_z5.json` | ✅ 已接入自动化 |
| 条件分支流程 | 无需图像 | `projects/accept_controlflow.json` | 执行顺序断言 | ✅ 已接入自动化（引擎级；If 真分支执行/假分支跳过） |
| Loop 固定次数 | 无需图像 | `projects/accept_loop.json` | 执行顺序断言+50 次重跑无污染 | ✅ 已接入自动化（阶段 4） |
| While 条件退出 | 无需图像 | `projects/accept_while.json` | 计数器数据驱动退出断言 | ✅ 已接入自动化（阶段 4） |
| StopWhile 提前退出 | 无需图像 | `projects/accept_stopwhile.json` | 仅 1 次迭代即退出断言 | ✅ 已接入自动化（阶段 4） |
| 停止/取消时限 | 无需图像 | `projects/accept_stop_cancel.json` | 500ms 内停止+迭代数远小于上限 | ✅ 已接入自动化（阶段 4） |
| Parallel all 汇合 | 无需图像 | `projects/accept_parallel_all.json` | 真实并发≥2+汇合晚于两分支完成 | ✅ 已接入自动化（阶段 4） |
| 控制汇合 any 策略 | 无需图像 | `projects/accept_parallel_any.json` | 单条边触发即汇合且仅一次 | ✅ 已接入自动化（阶段 4） |
| Parallel 失败分支 | 无需图像 | `projects/accept_parallel_failure.json` | 失败取消同组+耗时上限断言 | ✅ 已接入自动化（阶段 4） |
| Parallel blocking 不并行 | 无需图像 | `projects/accept_parallel_blocking.json` | 两分支均输出+并发度≤1 | ✅ 已接入自动化（阶段 4） |
| 拾取点集→圆拟合 | 固定圆周采样点 | `projects/accept_fitcircle_pick.json` | `expected/fitcircle_pick.json` | ✅ 已接入自动化（阶段 4 引擎级 + 阶段 5 GUI 鼠标拾取） |
| GUI 条件分支状态 | 无需图像 | 动态确定性工程 | 运行按钮+画布状态点/文字 | ✅ 阶段 5 截图验收 |
| Agent 假 LLM 端到端 | 固定验收图 | `tests/test_agente2e.cpp` | 建流→连接→运行→读结果全链路断言 | ✅ 已接入自动化（阶段 5，无网络） |
| SAM 四端点端到端 | 测试内 HTTP 服务 | `tests/test_sambackendclient.cpp` | 成功路径+真实超时+崩溃恢复 | ✅ 已接入自动化（阶段 5，无权重） |
| GUI 截图自校验 | 动态确定性工程 | `ui_capture_mainwindow`（CTest #66） | 截图存在/尺寸/非空白/关键状态 | ✅ 已接入自动化（阶段 5，失败即测试失败） |
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

## 已接入：循环/并行控制流与拾取→圆拟合（阶段 4）

- 全部为固定工程 + 确定性预期：循环/并行工程不依赖图像；拾取工程内点集初始为空。
- **Loop**：`loopCount=3`，断言执行序列 `loop,body×3,loop,after` 且重复 50 次无污染。
- **While**：种子模块写 `counter=0`，条件 `counter<3`，循环体每次 +1；断言恰好 3 次迭代后
  由数据条件退出（非 `maxIterations`），最终 `counter=3`。
- **StopWhile**：恒真循环（上限 1000）内首次迭代即提前退出，断言 `after` 仅执行一次。
- **停止/取消时限**：恒真长循环（100×30ms）运行中请求取消，断言 500ms 内停止、
  迭代数远小于上限、不进入 `done` 分支。
- **Parallel all**：两个 60ms 线程安全分支，断言最大并发度≥2、汇合点晚于两分支完成。
- **控制汇合 any**：If 真分支触发一条控制边即激活汇合点，假分支被跳过且仅汇合一次。
- **Parallel 失败分支**：快速失败分支取消同组 500ms 慢分支，断言总耗时 < 400ms、
  汇合点不执行。
- **Parallel blocking 不并行**：两个 blocking（SaveData）分支均成功输出且并发度≤1。
- **拾取→圆拟合**：空点集运行必须失败；提交 3 个圆周点后，拟合圆心/半径与已知圆一致。
  `test_mainwindow` 另行覆盖 FitCircle 自动创建 `point_set` 输入、3 次主窗口拾取处理写参与继续运行；
真实鼠标/视口端到端由阶段 5 的 `ui_capture_mainwindow` 覆盖。

## 阶段 5 端到端验收（Agent / SAM / GUI）

- **GUI**：使用真实 `FlowRunButton` 启动运行，使用 `HImageWidget` 的真实鼠标点击提交 3 个圆周点；
  断言包括圆心/半径误差、主视图拟合圆与拾取点像素、运行完成信号和流程节点状态。
  条件分支使用真实运行按钮和画布 Tab，截图保留控制边、成功状态点和跳过状态。
  专项截图：`fitcircle_pick_result.png`、`controlflow_canvas_result.png`；截图期间临时调整流程栏宽度，结束后恢复。
- **Agent**：`test_agente2e` 使用确定性假 LLM（无网络），Autopilot 完成
  "创建 GrabImage→FindCircle→连接→运行→读取结果"；断言模块/连接结构、取图参数、
  运行成功、圆心/半径与 `get_run_results` 统计。
- **SAM**：`test_sambackendclient` 内置 `SamTestServer`（QTcpServer HTTP 服务），
  覆盖 `/health`、`/set_image`、`/predict`、`/unload_image`；成功路径断言状态机与
  embedding/polygon/bbox/score/mask 解析；超时为真实超时（`setTimeoutMs(300)`）；
  崩溃（停止监听）→ Error → 原端口重启 → Ready → 预测成功。
- **截图即测试**：`ui_capture_mainwindow` 注册为 CTest（offscreen，TIMEOUT 300s），
  截图自校验（存在/尺寸/非空白/关键界面状态），任一失败退出码非零 → 测试失败。
  CI 不下载 SAM 权重；真实 GPU 模型保留现场/夜间验收。

## 待办

1. 为图像→点集提取（ROI/特征点）补充验收工程；当前点集由 MeasurementInput 拾取会话提供。
2. 为图像 ROI/边缘点提取补充真实 GUI 交互和 FitLine/FitCircle 验收；阶段 5 当前只覆盖 MeasurementInput 点集鼠标拾取。
3. PLC/相机/AI 先建设备模拟器与契约测试，再接入模拟流程验收。
