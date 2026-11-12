# Agent 能力基线矩阵（阶段 0）

> 本文件由 `scripts/audit_agent_capability_baseline.py` 从 `agent-capability-matrix.json`（唯一数据源）渲染，禁止手工编辑结论。

- 生成时间：2026-10-28T07:31:10+00:00；HEAD：`b516f54da28b63623ef6b293b71a4afa3b84fad0`；分支：agent/development；构建目录：build-codex-agent
- 元数据总数：**69**（模块 66 / 相机 3）；已构建二进制：66；运行时探针报告加载模块：65
- 未构建项：DirectShowCamera（平台不适用（camera/CMakeLists.txt: WIN32 分支；Linux 构建不适用））, HikvisionCamera（ENABLE_CAMERA_HIKVISION=OFF（需海康 MVS SDK；默认 OFF））, DefectDetection（ENABLE_HYMSON3D=OFF（hymson3d/CMakeLists.txt: ENABLE_HYMSON3D=OFF 时提前 return()，注释宣称的 demo 模式构建实际不可达））
- 清单核验：期望 `{'metadataTotal': 69, 'modules': 66, 'cameras': 3}`；实际 `{'metadataTotal': 69, 'modules': 66, 'cameras': 3}`；匹配：**True**；新增：无；移除：无
- 构建原因码：DirectShowCamera=`PLATFORM_UNSUPPORTED`, HikvisionCamera=`BUILD_OPTION_DISABLED`, DefectDetection=`BUILD_OPTION_DISABLED`

- GUI 操作清单：126 项；按映射：{'直接工具': 66, '人机协作': 37, '未支持': 18, '通用工具组合': 5}；按阶段：{'1': 5, '6': 84, '4': 12, '5': 10, '3': 4, '2': 4, '7': 7}

## 维度状态分母（计划 §5：按维度报告，不给单一总百分比）

| 维度 | PASS | FAIL | BLOCKED | NOT_APPLICABLE | NOT_TESTED | 分母 |
| --- | --- | --- | --- | --- | --- | --- |
| discovery | 67 | 0 | 0 | 0 | 2 | 69 |
| configuration | 6 | 0 | 0 | 0 | 63 | 69 |
| input | 0 | 0 | 4 | 0 | 65 | 69 |
| execution | 12 | 0 | 4 | 0 | 53 | 69 |
| output | 19 | 0 | 3 | 0 | 47 | 69 |
| permission | 12 | 0 | 0 | 54 | 3 | 69 |
| undo_persistence | 0 | 0 | 0 | 11 | 58 | 69 |

## 插件矩阵（69 项逐项，含未构建原因）

| 插件 | 类别 | 二进制 | 构建原因码 | 运行时 | 运行时原因码 | 副作用 | 台账状态 | 发现 | 配置 | 输入 | 执行 | 输出 | 权限 | 撤销 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CalculateOffset | module/calibration | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| N点标定 | module/calibration | ✓ | — | loaded | — | none | 可用 | PASS | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| DirectShow Camera | camera/camera | ✗ 平台不适用（camera/CMakeLists.txt: WIN32 分支；Linux 构建不适用） | PLATFORM_UNSUPPORTED | n/a | PLATFORM_UNSUPPORTED | device | 依赖硬件 | NOT_TESTED | NOT_TESTED | BLOCKED | BLOCKED | BLOCKED | NOT_TESTED | NOT_APPLICABLE |
| Hikvision Camera | camera/camera | ✗ ENABLE_CAMERA_HIKVISION=OFF（需海康 MVS SDK；默认 OFF） | BUILD_OPTION_DISABLED | n/a | BUILD_OPTION_DISABLED | device | 依赖硬件 | NOT_TESTED | NOT_TESTED | BLOCKED | BLOCKED | BLOCKED | NOT_TESTED | NOT_APPLICABLE |
| Video4Linux2 Camera | camera/camera | ✓ | — | loaded | — | device | 依赖硬件 | PASS | NOT_TESTED | BLOCKED | BLOCKED | BLOCKED | NOT_TESTED | NOT_APPLICABLE |
| PLC通信测试 | module/communication | ✓ | — | loaded | — | device | 依赖硬件 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| PLC读取 | module/communication | ✓ | — | loaded | — | device | 依赖硬件 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| PLC写入 | module/communication | ✓ | — | loaded | — | device | 依赖硬件 | PASS | PASS | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| 串口通信 | module/communication | ✓ | — | loaded | — | device | 依赖硬件 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| TCP客户端 | module/communication | ✓ | — | loaded | — | network | 可用 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| TCP服务器 | module/communication | ✓ | — | loaded | — | network | 可用 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| ColorRecognition | module/detection | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| EdgeDefectDetection | module/detection | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| FindCircle | module/detection | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| JiErHanDefectsDet | module/detection | ✓ | — | loaded | — | none | 实验性 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| Matching | module/detection | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| MeasureCircle | module/detection | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| MeasureLine | module/detection | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| MeasureRect | module/detection | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| QRCode | module/detection | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| SamPredict | module/detection | ✓ | — | loaded | — | network | — | PASS | PASS | BLOCKED | PASS | PASS | PASS | NOT_APPLICABLE |
| DistancePL | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| DistancePP | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| FitCircle | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| FitEllipse | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| FitLine | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| FitPlane | module/geometry | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| FreeformSurface | module/geometry | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| GapMeasure3D | module/geometry | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| LinesDistance | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| MeasureGap | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| MeasurementInput | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | PASS | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| PointSurfaceDistance | module/geometry | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| RegionStats | module/geometry | ✓ | — | loaded | — | none | — | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| DefectDetection | module/hymson3d | ✗ ENABLE_HYMSON3D=OFF（hymson3d/CMakeLists.txt: ENABLE_HYMSON3D=OFF 时提前 return()，注释宣称的 demo 模式构建实际不可达） | BUILD_OPTION_DISABLED | n/a | BUILD_OPTION_DISABLED | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | BLOCKED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| Blob | module/image_processing | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | PASS | NOT_APPLICABLE | NOT_TESTED |
| CropImage | module/image_processing | ✓ | — | loaded | — | none | 部分 | PASS | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| DisplayData | module/image_processing | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| GrabImage | module/image_processing | ✓ | — | loaded | — | device | 可用 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_APPLICABLE |
| ImageScript | module/image_processing | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| JigsawPuzzle | module/image_processing | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| LoadPointCloud | module/image_processing | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| PerProcessing | module/image_processing | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 3DPreProcessing | module/image_processing | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| SaveImage | module/image_processing | ✓ | — | loaded | — | file | 可用 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_TESTED |
| ShowImage | module/image_processing | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 条件判断 | module/logic | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 延时 | module/logic | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 条件分支 | module/logic | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 循环 | module/logic | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| Parallel | module/logic | ✓ | — | loaded | — | none | — | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 队列输入 | module/logic | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 队列输出 | module/logic | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 停止循环 | module/logic | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 条件循环 | module/logic | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| DataCheck | module/system | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| Folder | module/system | ✓ | — | loaded | — | file | 可用 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_TESTED |
| SaveData | module/system | ✓ | — | loaded | — | file | 部分 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_TESTED |
| ShowPoint | module/system | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| SystemTime | module/system | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| TableOutPut | module/system | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| TimeSlice | module/system | ✓ | — | loaded | — | none | 部分 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| WriteText | module/system | ✓ | — | loaded | — | file | 可用 | PASS | NOT_TESTED | NOT_TESTED | PASS | PASS | PASS | NOT_TESTED |
| 创建字符串 | module/variable | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 数学运算 | module/variable | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 分割字符串 | module/variable | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 字符串格式化 | module/variable | ✓ | — | loaded | — | none | 可用 | PASS | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 变量定义 | module/variable | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |
| 变量赋值 | module/variable | ✓ | — | loaded | — | none | 可用 | PASS | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_TESTED | NOT_APPLICABLE | NOT_TESTED |

## 维度单元格理由（FAIL/BLOCKED 摘要，全量见 JSON）

- **DirectShow Camera** / input = BLOCKED：相机输入=设备前置条件（计划 §5）；本机无对应设备/平台不适用，模拟相机属阶段 5、真机属阶段 8，收口阶段 5
- **DirectShow Camera** / execution = BLOCKED：二进制未构建：平台不适用（camera/CMakeLists.txt: WIN32 分支；Linux 构建不适用），收口阶段 8
- **DirectShow Camera** / output = BLOCKED：采集输出通路属阶段 5；真机属阶段 8，收口阶段 5
- **Hikvision Camera** / input = BLOCKED：相机输入=设备前置条件（计划 §5）；本机无对应设备/平台不适用，模拟相机属阶段 5、真机属阶段 8，收口阶段 5
- **Hikvision Camera** / execution = BLOCKED：二进制未构建：ENABLE_CAMERA_HIKVISION=OFF（需海康 MVS SDK；默认 OFF），收口阶段 8
- **Hikvision Camera** / output = BLOCKED：采集输出通路属阶段 5；真机属阶段 8，收口阶段 5
- **Video4Linux2 Camera** / input = BLOCKED：相机输入=设备前置条件（计划 §5）；本机无对应设备/平台不适用，模拟相机属阶段 5、真机属阶段 8，收口阶段 5
- **Video4Linux2 Camera** / execution = BLOCKED：设备链路（发现→连接→采集→断开）属阶段 5（模拟）/阶段 8（真机）；运行时库加载成功仅证明 ABI/接口（见探针日志），收口阶段 5
- **Video4Linux2 Camera** / output = BLOCKED：采集输出通路属阶段 5；真机属阶段 8，收口阶段 5
- **SamPredict** / input = BLOCKED：需运行中的 SAM 服务与真实权重；模拟服务证据属阶段 5/7，真实模型属阶段 8，收口阶段 8
- **DefectDetection** / execution = BLOCKED：二进制未构建：ENABLE_HYMSON3D=OFF（hymson3d/CMakeLists.txt: ENABLE_HYMSON3D=OFF 时提前 return()，注释宣称的 demo 模式构建实际不可达），收口阶段 8

## GUI 操作清单

| 操作 ID | 区域 | 操作 | ownerApi | 工具映射 | 阶段 |
| --- | --- | --- | --- | --- | --- |
| FILE_NEW_SOLUTION | 文件菜单 | 新建方案(工程) | `MainWindow::onNewSolution` | 直接工具 | 1 |
| FILE_SOLUTION_LIST | 文件菜单 | 方案列表(浏览既有工程) | `MainWindow::onSolutionList / ProjectManager::recentProjects` | 直接工具 | 6 |
| FILE_OPEN | 文件菜单 | 打开工程 | `MainWindow::onOpenProject` | 直接工具 | 1 |
| FILE_SAVE | 文件菜单 | 保存工程 | `MainWindow::onSaveProject` | 直接工具 | 1 |
| FILE_EXIT | 文件菜单 | 退出应用 | `QApplication::quit` | 人机协作 | 6 |
| PARAM_UNDO | 参数菜单 | 撤销(参数撤销栈) | `MainWindow m_paramUndoStack` | 未支持 | 4 |
| PARAM_REDO | 参数菜单 | 重做 | `MainWindow m_paramUndoStack` | 未支持 | 4 |
| PARAM_GLOBAL_VAR | 参数菜单 | 全局变量管理(查询/增删改) | `GlobalVarView` | 通用工具组合 | 6 |
| PARAM_USER_LOGIN | 参数菜单 | 用户登录 | `LoginDialog/UserManager` | 人机协作 | 6 |
| VIEW_TOGGLE_TOOL_PANEL | 视图菜单 | 显示/隐藏工具面板 | `MainWindow::onToggleToolPanel` | 直接工具 | 6 |
| VIEW_TOGGLE_PROCESS_PANEL | 视图菜单 | 显示/隐藏流程面板 | `MainWindow::onToggleProcessPanel` | 直接工具 | 6 |
| VIEW_TOGGLE_BOTTOM_PANEL | 视图菜单 | 显示/隐藏底部面板 | `MainWindow::onToggleBottomPanel` | 直接工具 | 6 |
| VIEW_QUICK_MODE | 视图菜单 | 快捷运行当前流程一次 | `MainWindow::onQuickMode → executeFlowOnce / AgentActor::runFlow` | 直接工具 | 6 |
| VIEW_TOGGLE_THEME | 视图菜单 | 深浅主题设置 | `MainWindow::onToggleTheme` | 直接工具 | 6 |
| VIEW_RENDER_MODE | 视图菜单 | 3D 渲染模式(统一色/RGB/高度/强度/法向/BlinnPhong) | `MainWindow::onRenderModeChanged` | 直接工具 | 6 |
| TOOL_CAMERA_SETTINGS | 工具菜单 | 打开相机设置页 | `MainWindow::onCameraSettings→CameraSetView` | 未支持 | 5 |
| TOOL_COMM_SETTINGS | 工具菜单 | 打开通讯设置页 | `MainWindow::onCommSettings→CommunicationSetView` | 未支持 | 5 |
| TOOL_HARDWARE_CONFIG | 工具菜单 | 硬件配置(系统参数:自动加载/自动保存/语言/主题/循环间隔/日志) | `SystemParamView/SystemConfig` | 直接工具 | 6 |
| AGENT_SETTINGS_OPEN | Agent 设置 | 打开/复用 Agent 设置窗口 | `MainWindow::executeAgentDataSourceViewAction / AgentSettingsDialog` | 直接工具 | 6 |
| TOOL_AGENT_SETTINGS | 工具菜单 | Agent 设置(端点/密钥/模型/温度/maxTokens/权限级/系统提示/开关) | `AgentSettingsDialog/ConfigManager[agent]` | 直接工具 | 6 |
| TOOL_KNOWLEDGE_BASE | 工具菜单 | 打开知识库管理对话框 | `AgentSettingsDialog knowledgeBtn→KnowledgeBaseDialog` | 直接工具 | 6 |
| HELP_ABOUT | 帮助菜单 | 关于对话框 | `MainWindow::onAbout→QMessageBox::about` | 直接工具 | 6 |
| TB_RUN_ONCE | 主工具栏 | 单次运行流程 | `MainWindow→RunEngine::start` | 直接工具 | 3 |
| TB_STEP | 主工具栏 | 单步运行 | `RunEngine::stepOnce` | 直接工具 | 4 |
| TB_LOOP_RUN | 主工具栏 | 循环运行 | `RunEngine(循环模式)` | 未支持 | 4 |
| TB_STOP | 主工具栏 | 停止运行 | `RunEngine::stop` | 直接工具 | 3 |
| TB_QUICK_MEASURE | 主工具栏 | 快速测量入口(进入拾取模式) | `MainWindow::onQuickMeasure` | 人机协作 | 6 |
| TB_QUICK_ANNOTATE | 主工具栏 | 快速标注入口(打开 SAM 对话框) | `MainWindow::onQuickAnnotate/SamAnnotatorDialog` | 直接工具 | 6 |
| TOOLBOX_SEARCH | 工具箱 | 搜索过滤工具(模块) | `MainWindow::filterToolBox` | 通用工具组合 | 5 |
| TOOLBOX_DRAG_ADD | 工具箱 | 拖拽模块到画布/流程树添加节点 | `PluginDragPayload/FlowCanvas/ProcessTreeController` | 直接工具 | 2 |
| PTREE_SELECT | 流程树 | 选中节点(联动检查器) | `ProcessTreeController::setCurrentItem` | 直接工具 | 6 |
| PTREE_DELETE | 流程树 | 删除节点(右键菜单) | `ProcessTreeController 删除动作` | 直接工具 | 4 |
| PTREE_DBLCLICK_INSPECTOR | 流程树 | 双击打开检查器参数页 | `MainWindow::eventFilter→selectModule` | 直接工具 | 6 |
| PTREE_REORDER | 流程树 | 节点排序/移动 | `Project::moveModule/ProcessTreeController` | 直接工具 | 4 |
| CANVAS_SELECT_MOVE | 流程画布 | 框选/移动节点(位置持久化) | `FlowCanvas/FlowNodeItem::mouseMoveEvent` | 未支持 | 4 |
| CANVAS_CONNECT_PORTS | 流程画布 | 端口拖拽连线/断开 | `FlowCanvas 端口交互` | 直接工具 | 4 |
| INSP_PIN | 检查器 | 显式固定/取消固定当前检查器 | `ModuleInspectorPanel::setPinned` | 直接工具 | 6 |
| INSP_COLLAPSE | 检查器 | 折叠/展开检查器 | `ModuleInspectorPanel::setCollapsed / collapseToggled splitter handler` | 直接工具 | 6 |
| INSP_VISIBILITY | 检查器 | 关闭/重新打开检查器 | `ModuleInspectorPanel::closeRequested / MainWindow::setInspectorVisibility` | 直接工具 | 6 |
| INSP_TAB_PARAMS | 检查器 | 参数页编辑(Schema 生成控件) | `ModuleInspectorPanel/PropertyPanel/ConfigWidgetFactory` | 直接工具 | 2 |
| INSP_TAB_RESULTS | 检查器 | 结果页查看模块输出 | `ModuleInspectorPanel m_resultsTab` | 未支持 | 3 |
| INSP_ENABLED | 检查器 | 启用/禁用当前节点 | `ModuleInspectorPanel InspectorEnabledCheck` | 直接工具 | 4 |
| INSP_BREAKPOINT | 检查器 | 设置/取消断点 | `ModuleInspectorPanel m_breakpointBtn` | 直接工具 | 4 |
| INSP_NOTE | 检查器 | 编辑节点备注 | `ModuleInspectorPanel InspectorNoteEdit` | 直接工具 | 4 |
| INSP_RERUN | 检查器 | 重跑当前模块 | `ModuleInspectorPanel InspectorRerunBtn` | 未支持 | 4 |
| INSP_ADVANCED_CONFIG | 检查器 | 高级配置弹窗(草稿副本编辑,单条可撤销提交) | `ModuleInspectorPanel::onAdvancedClicked/MainWindow::openAdvancedPluginConfig` | 通用工具组合 | 2 |
| INSP_RESET_PARAMS | 检查器 | 恢复默认参数 | `ModuleInspectorPanel::onResetClicked` | 未支持 | 2 |
| DS_IMPORT | 数据源面板 | 导入数据源并加载到主视图 | `MainWindow::importFile / importImageFile / importPointCloudFile` | 通用工具组合 | 6 |
| DS_BIND_GRABIMAGE | 数据源面板 | 将图像路径绑定到指定 GrabImage 实例 | `MainWindow::autoConfigureGrabImage / Project::setModuleParam` | 通用工具组合 | 6 |
| DS_LIST | 数据源面板 | 列举工程已登记数据源 | `Project::dataSources` | 直接工具 | 6 |
| DS_DISPLAY_MAIN | 数据源面板 | 在主视口显示已登记数据源 | `MainWindow::onDisplayDataSource / displayDataSourceById` | 直接工具 | 6 |
| DS_VIEW_STATE | 数据源面板 | 查询主视图显示与数据源可见状态 | `MainWindow::executeAgentDataSourceViewAction` | 直接工具 | 6 |
| DS_SET_VISIBILITY | 数据源面板 | 设置主视图数据源可见性 | `MainWindow::onDataSourceVisibilityChanged / setDataSourceVisibilityById` | 直接工具 | 6 |
| DS_DISPLAY_NEW_VIEWPORT | 数据源面板 | 在新视口显示数据源 | `DataSourcePanel m_displayInNewViewportAction / MainWindow::displayDataSourceInDedicatedViewportById` | 直接工具 | 6 |
| DS_SHOW_IN_FOLDER | 数据源面板 | 在文件夹中显示 | `DataSourcePanel::requestShowInFolder / MainWindow::openDesktopUrl` | 人机协作 | 6 |
| DS_COPY_PATH | 数据源面板 | 复制路径 | `DataSourcePanel::requestCopyPath / MainWindow::copyDataSourcePathById` | 直接工具 | 6 |
| DS_DELETE_REF | 数据源面板 | 移除数据源引用(不删磁盘文件) | `Project::removeDataSource / DataSourcePanel m_deleteAction` | 直接工具 | 6 |
| VP_ZOOM_FIT | 主视口工具栏 | 放大/缩小/适应窗口/1:1 | `MainWindow::executeAgentDataSourceViewAction / ViewportWidget zoom API` | 直接工具 | 6 |
| VP_POINT_SIZE | 主视口工具栏 | 点云点大小调整 | `MainWindow::executeAgentDataSourceViewAction / ViewportPointSizeSpinBox` | 直接工具 | 6 |
| VIEWPORT_LIST | 视口状态 | 列举视口 ID、模式、可见性与显示内容供测量目标选择 | `MainWindow::executeAgentQuickMeasurementAction/list_viewports` | 直接工具 | 6 |
| VP_TOGGLE_2D3D | 主视口工具栏 | 2D/3D 视图切换 | `ViewportWidget::setDisplayMode / m_toggleViewAction` | 直接工具 | 6 |
| VP_SNAPSHOT | 主视口工具栏 | 保存视图快照 | `ViewportWidget::saveSnapshotForUser / m_snapshotAction` | 人机协作 | 6 |
| VP_CLOSE | 主视口工具栏 | 关闭当前视口 | `ViewportWidget::viewportClosed / MainWindow::closeViewportById` | 直接工具 | 6 |
| MEAS_PP | 快速测量 | 两点距离(2D/3D) | `MainWindow 测量拾取(2279/3062)` | 人机协作 | 6 |
| MEAS_PL | 快速测量 | 点到线距离 | `MainWindow 测量拾取` | 人机协作 | 6 |
| MEAS_LL | 快速测量 | 两线段间距 | `MainWindow 测量拾取` | 人机协作 | 6 |
| MEAS_ANGLE3 | 快速测量 | 三点角度 | `MainWindow 测量拾取` | 人机协作 | 6 |
| MEAS_FIT_CIRCLE3 | 快速测量 | 三点拟合圆 | `MainWindow 测量拾取` | 人机协作 | 6 |
| MEAS_RECT_AREA | 快速测量 | 矩形面积(拖拽) | `MainWindow 测量拾取` | 人机协作 | 6 |
| LOG_CLEAR | 日志面板 | 清空运行日志 | `MainWindow ClearLogButton / Logger::clearLogs` | 直接工具 | 6 |
| LOG_COLLAPSE | 日志面板 | 折叠/展开底部面板 | `MainWindow CollapseLogButton / WorkspaceBottomPanelAction` | 直接工具 | 6 |
| LOG_FILTER | 日志面板 | 日志级别过滤菜单 | `MainWindow::onLogFilterChanged` | 直接工具 | 6 |
| LOG_TABS | 日志面板 | 运行日志/Agent 日志/终端页签切换 | `MainWindow m_logTerminalTabs` | 直接工具 | 6 |
| TERMINAL_INPUT | 终端 | 内嵌终端交互(人类专用) | `TerminalWidget/BashProcess/PtyImpl` | 人机协作 | 7 |
| CHAT_SEND_TEXT | Agent 对话 | 发送文本消息 | `AgentChatPanel::onSendClicked→userMessageSent` | 人机协作 | 7 |
| CHAT_ATTACH_IMAGE | Agent 对话 | 附加图片(≤4 张,超限提示) | `AgentChatPanel::addImageAttachment` | 人机协作 | 7 |
| CHAT_SCROLL_LATEST | Agent 对话 | 回到最新消息 | `AgentChatPanel::scrollToBottom` | 直接工具 | 6 |
| CHAT_TOOL_CONFIRM | Agent 对话 | 工具预览卡确认/拒绝 | `AgentToolPreviewCard confirmed/rejected→AgentController` | 人机协作 | 1 |
| AGENTLOG_VIEW | Agent 日志 | 审计日志查看 | `AgentActionLogWidget` | 未支持 | 7 |
| SAM_ENV_SETUP | SAM 标注 | 打开 SAM 环境设置并检查前置条件 | `MainWindow::executeAgentDataSourceViewAction / SamAnnotatorDialog::environmentHandoffStatus` | 直接工具 | 6 |
| SAM_OPEN_IMAGE | SAM 标注 | 打开图片 | `SamAnnotatorDialog::onOpenImage` | 人机协作 | 6 |
| SAM_INIT_ENV | SAM 标注 | 初始化 Python 环境 | `SamAnnotatorDialog::onInitializeEnvironment/SamBackendClient` | 人机协作 | 6 |
| SAM_IMPORT_MODEL | SAM 标注 | 导入 SAM 权重 | `SamAnnotatorDialog::onImportModel` | 人机协作 | 6 |
| SAM_RESTART_SERVER | SAM 标注 | 重启 SAM 服务 | `SamAnnotatorDialog::onRestartSam` | 人机协作 | 6 |
| SAM_PROMPT_POINTS_BOX | SAM 标注 | 正点/负点/框选提示生成掩膜 | `AnnotationOverlayWidget→SamBackendClient` | 直接工具 | 6 |
| SAM_CANDIDATE_NAV | SAM 标注 | 多掩膜候选上一个/下一个 | `SamAnnotatorDialog m_btnPrev/NextCandidate` | 人机协作 | 6 |
| SAM_MASK_OPS | SAM 标注 | 编辑活动 SAM 会话掩膜:膨胀/腐蚀/橡皮擦/合并/拆分 | `SamAnnotatorDialog::listAgentAnnotationObjects/applyAgentMaskEdit + QUndoStack` | 人机协作 | 6 |
| SAM_CONFIRM_OBJECT | SAM 标注 | 确认提示为标注对象(写入会话) | `SamAnnotatorDialog::onConfirm` | 直接工具 | 6 |
| SAM_CANCEL_PROMPT | SAM 标注 | 取消当前提示(GUI 编辑态) | `SamAnnotatorDialog::cancelAgentPrompt + AgentController confirmation snapshot` | 人机协作 | 6 |
| SAM_CATEGORY_MGMT | SAM 标注 | 添加/删除类别,对象标签修改 | `SamAnnotatorDialog::onAddCategory/onRemoveCategory` | 人机协作 | 6 |
| SAM_SESSION_SAVE | SAM 标注 | 保存当前标注会话(.deeplux-anno.json) | `SamAnnotatorDialog::onSaveSession` | 直接工具 | 6 |
| SAM_SESSION_OPEN | SAM 标注 | 打开既有标注会话 | `SamAnnotatorDialog::onOpenAnnotation` | 直接工具 | 6 |
| SAM_SESSION_QUERY | SAM 标注 | 查询指定标注会话及对象摘要 | `AnnotationSession::loadFromBytes / AgentActor::getAnnotationSession` | 直接工具 | 6 |
| SAM_EXPORT | SAM 标注 | 导出 LabelMe/YOLO/COCO | `SamAnnotatorDialog::onExportLabelMe/onExportYoloSeg/onExportCoco` | 直接工具 | 6 |
| KB_SEARCH | 知识库 | 按主题检索知识库(分节匹配) | `AgentActor::readDocumentation` | 直接工具 | 6 |
| KB_LIST_DOCS | 知识库 | 列举知识库文档(内置+用户) | `AgentActor::listKnowledgeDocuments / KnowledgeBaseDialog::reloadList` | 直接工具 | 6 |
| KB_PREVIEW_DOC | 知识库 | 预览指定文档内容 | `AgentActor::previewKnowledgeDocument / KnowledgeBaseDialog::showPreview` | 直接工具 | 6 |
| KB_ADD_DOC | 知识库 | 添加用户文档(复制入库) | `AgentActor::addKnowledgeDocument / KnowledgeBaseDialog::onAddClicked` | 直接工具 | 6 |
| KB_REMOVE_DOC | 知识库 | 移除用户文档(内置只读) | `AgentActor::removeKnowledgeDocument / KnowledgeBaseDialog::onRemoveClicked` | 直接工具 | 6 |
| KB_OPEN_DIR | 知识库 | 打开用户知识库目录 | `KnowledgeBaseDialog::onOpenLocationClicked` | 人机协作 | 6 |
| ASET_LLM_CONFIG | Agent 设置 | 端点/API Key/模型/温度/maxTokens 配置与持久化 | `AgentSettingsDialog::saveSettings→ConfigManager[agent]` | 人机协作 | 6 |
| ASET_PERMISSION_LEVEL | Agent 设置 | 权限级别(Observer/Advisor/Autopilot) | `AgentSettingsDialog→AgentController::setPermissionLevel` | 人机协作 | 1 |
| ASET_SYSTEM_PROMPT | Agent 设置 | 系统提示词编辑 | `AgentSettingsDialog→AgentController::setSystemPrompt` | 人机协作 | 7 |
| ASET_TOGGLES | Agent 设置 | 工具/视觉/思考开关 | `AgentSettingsDialog::m_toolsCheck/m_visionCheck/m_thinkingCheck→OpenAILLMClient::setToolsEnabled/setVisionEnabled/setThinkingEnabled` | 人机协作 | 7 |
| ASET_TEST_CONNECTION | Agent 设置 | 测试 LLM 连接 | `AgentSettingsDialog testBtn(直连 HTTP)` | 人机协作 | 7 |
| CAM_REFRESH | 相机设置 | 刷新相机列表(发现) | `CameraSetView::onRefreshClicked→CameraManager::discoverCameras` | 未支持 | 5 |
| CAM_CONNECT_DISCONNECT | 相机设置 | 连接/断开相机 | `CameraSetView::onConnectClicked/CameraSetView::onDisconnectClicked→CameraManager` | 未支持 | 5 |
| CAM_DELETE_CONFIG | 相机设置 | 删除相机配置 | `CameraSetView::onDeleteClicked` | 未支持 | 5 |
| CAM_APPLY_PARAMS | 相机设置 | 应用相机参数(IP/端口/协议) | `CameraSetView::onApplyClicked` | 未支持 | 5 |
| COMM_ADD_DELETE | 通讯设置 | 添加/删除通讯配置(TCP/串口/PLC) | `CommunicationSetView::onAddClicked/CommunicationSetView::onDeleteClicked→CommunicationManager` | 未支持 | 5 |
| COMM_CONNECT_DISCONNECT | 通讯设置 | 连接/断开通讯通道 | `CommunicationSetView::onConnectClicked/CommunicationSetView::onDisconnectClicked` | 未支持 | 5 |
| COMM_APPLY | 通讯设置 | 应用通讯配置 | `CommunicationSetView::onApplyClicked` | 未支持 | 5 |
| GV_CREATE | 全局变量 | 创建类型化全局变量 | `GlobalVarView::onAddClicked/GlobalVarManager::tryCreateUserVariable` | 直接工具 | 6 |
| GV_READ | 全局变量 | 列出变量元数据或读取单个变量值 | `GlobalVarManager::userVariableSnapshots/userVariableSnapshot` | 直接工具 | 6 |
| GV_SET | 全局变量 | 以严格类型字面值设置现有全局变量 | `GlobalVarManager::trySetUserVariable` | 直接工具 | 6 |
| GV_REMOVE | 全局变量 | 移除现有全局变量 | `GlobalVarView::onDeleteClicked/GlobalVarManager::tryRemoveUserVariable` | 直接工具 | 6 |
| GV_MOVE | 全局变量 | 上移/下移变量显示顺序 | `GlobalVarView::onMoveUpClicked/GlobalVarView::onMoveDownClicked` | 人机协作 | 6 |
| GV_EXECUTE | 全局变量 | 执行变量表达式 | `GlobalVarView::onExecuteClicked→GlobalVarManager::evaluateVariable` | 人机协作 | 6 |
| GV_APPLY | 全局变量 | 应用变量变更 | `GlobalVarView::onApplyClicked` | 人机协作 | 6 |
| SYS_GENERAL | 系统设置 | 自动加载/自动保存/间隔 | `SystemParamView→SystemConfig` | 人机协作 | 6 |
| SYS_DISPLAY | 系统设置 | 语言/主题设置 | `SystemParamView→ConfigManager[appearance]` | 人机协作 | 6 |
| SYS_RUN_LOG | 系统设置 | 循环间隔/日志级别/文件日志 | `SystemParamView→SystemConfig` | 人机协作 | 6 |
| SYS_RESET_SAVE | 系统设置 | 重置/保存系统参数 | `SystemParamView::onResetClicked/SystemParamView::onSaveClicked` | 人机协作 | 6 |
| LOGIN_DIALOG_OPEN | 登录 | 打开/复用登录窗口 | `MainWindow::executeAgentDataSourceViewAction / LoginDialog` | 直接工具 | 6 |
| LOGIN_SUBMIT | 登录 | 用户登录提交(用户/密码) | `LoginDialog→UserManager` | 人机协作 | 6 |
| STATUSBAR_INFO | 状态栏 | 运行状态/用户/时间显示 | `MainWindow RunStatusLabel/UserStatusLabel/TimeStatusLabel` | 未支持 | 3 |
