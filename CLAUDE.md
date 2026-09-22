# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 语言偏好

以后所有对话都使用中文进行。

## 项目概述

DeepLux Vision 是 C++17 + Qt 的跨平台工业机器视觉软件：可视化流程画布组织采集/处理/检测/测量模块，算法以动态插件交付，另含受控 Agent（工具调用 + 撤销 + 审计）与 SAM 快速标注（独立 Python FastAPI 进程）。

依赖：CMake 3.16+、Qt 6.6+（或 Qt 5.15.3+，需 Core/Gui/Widgets/Network/Sql/Test/Concurrent/SerialPort）、**OpenCV 必需**（缺失时 configure 直接失败）。Halcon 与相机 SDK（Basler/海康）可选。

## 常用命令

```bash
# 配置（DEEPLUX_QT_PATH 可选，留空用系统 Qt；默认 USE_QT6=ON，找不到 Qt6 自动回落 Qt5）
cmake -S . -B build -DDEEPLUX_QT_PATH=/path/to/Qt/6.6.0/gcc_64 -DBUILD_TESTS=ON

# 构建
cmake --build build -j$(nproc)

# 全部测试（GUI 测试默认 QT_QPA_PLATFORM=offscreen，由 DEEPLUX_TEST_QPA_PLATFORM 控制）
ctest --test-dir build --output-on-failure

# 单个测试（测试名 = 文件名去扩展名）
ctest --test-dir build -R test_findcircle --output-on-failure

# 同步插件到 ~/.deeplux/plugins（验证运行时插件加载必需）
cmake --build build --target sync-plugins

# 启动 GUI / CLI
./build/bin/DeepLux --gui
./build/bin/DeepLux --cli info   # CLI 命令见 docs/cli.md
```

核心改动的最低验证集：build → ctest → sync-plugins。

**格式门禁**：CI 只检查本次变更的 C++ 文件。提交前对改动文件执行 `clang-format -i path/to/file.cpp`（.clang-format：LLVM 基础、4 空格、120 列、附着大括号）。

## 架构

分层（详见 docs/architecture.md）：

- `src/app` — 应用入口（GUI/CLI 双模式）。
- `src/core` — 业务核心，编译为 `deeplux_core`：
  - `model/` Project（节点、连线、参数、布局）、ImageData、ProjectMigrator
  - `engine/RunEngine` — 单次/单步/循环/停止执行，发 moduleStarted/moduleFinished/runFinished 信号
  - `manager/` PluginManager（插件发现、接口校验、实例化）、ProjectManager、ConfigManager、GlobalVarManager
  - `interface/IModule.h` + `base/ModuleBase` — 插件 ABI 契约
  - `deeplux/DataContract.h` — 跨模块强类型端口数据契约（ABI v2）
  - `agent/` AgentController → ToolSchema/权限 → AgentActor → QUndoStack/Project；AgentBridge 提供 IPC（tool_call/query/subscribe/ping）；OpenAILLMClient 对接 LLM
  - `io/`、`device/`、`communication/` — PLY/TIFF 加载、LabelMe/YOLO 导出、相机、PLC/串口
- `src/ui` — 编译为 `deeplux_ui`：MainWindow、FlowCanvas（流程画布）、ViewportWidget/HImageWidget（主视图）、ModuleInspectorPanel（检查器）、AgentChatPanel、TerminalWidget（内嵌终端，仅供人类使用）、SamAnnotatorDialog + AnnotationOverlayWidget（SAM 标注）。
- `src/plugins/<domain>/<PluginName>/` — 10 个领域（image_processing/detection/geometry/calibration/variable/communication/logic/system/hymson3d/camera），共 67 个运行时插件。
- `tools/sam_server/` — SAM FastAPI 服务（Python 依赖与 C++ 构建完全隔离，模型权重不入库）。

### 插件契约（改动插件必读）

- 每个插件目录含 `CMakeLists.txt`、`metadata.json`、`*Plugin.{h,cpp}`；插件类继承 `ModuleBase`，实现 `process(ImageData, ImageData)` 与 `cloneImpl()`。
- `metadata.json` 描述身份、`ui.parameters`（检查器控件由 Schema 生成，含 label/min/max/step/order）、`ui.results` 和 `ports`（输入/输出端口类型与必需性）。
- **ABI 版本**：`IModule.h` 中 `DEEPLUX_MODULE_INTERFACE_VERSION`（当前 2）。IModule 虚函数任何增删改都必须递增该版本并重建全部插件；PluginManager 加载时校验 interfaceVersion，不匹配即拒绝（防跨 DLL 虚表错位）。因此**插件接口或 metadata 变更后必须完整重建 + sync-plugins**，否则 `~/.deeplux/plugins` 里的旧库会与主程序不一致。
- 端口执行由 `ModuleBase::execute(PortValueMap...)` 桥接：输入按 ports 声明做必需性/类型校验（不匹配返回结构化错误码，不进 `process()`）；输出只导出 metadata 声明过的命名端口。`Mask2D`/`Ellipse2D` 等未实现类型在加载期即被拒绝。
- 参数统一契约（由 `test_pluginparametercontracts` 持续校验）：默认参数必须通过 `validateParams()`（只约束类型/枚举/范围/字段关系，外部资源留空由 `process()` 运行期报告）；`clone()` 完整保留参数且实例独立；每个非 `_` 前缀参数须有 Schema 条目，不渲染的显式标 `hidden: true`；枚举默认值必须在 `_options` 中，数值默认值必须落在 `min`/`max` 内。
- 工程节点由插件模板 `clone()` 而来，模块实例间不共享状态。

### Agent 边界

Agent 只能调用已注册的内部工具（读流程、加模块、设参数等），写操作按权限等级确认、经 QUndoStack 可撤销、全部落审计日志；任意 shell `execute` 被明确拒绝。详见 docs/Terminal_Agent_Design.md。

## 测试

- Qt Test 框架，每个 `tests/test_<feature>.cpp` 是独立可执行文件，统一注册在 `tests/CMakeLists.txt` 的 `TEST_SOURCES`；测试用例是名为 `testSomething` 的私有槽。
- 需要真实插件的测试：链接插件 target，并通过 `target_compile_definitions` + `$<TARGET_FILE:...>` 注入库路径与 metadata 路径（跨平台约定，**不要在测试代码里拼接 .so 文件名**）。
- `ui_capture_mainwindow` 是注册为 CTest 的 GUI 截图验收，截图自校验失败即测试失败。

## 约定

- 命名：`DeepLux` 命名空间、类 `PascalCase`、函数 `lowerCamelCase`、成员 `m_` 前缀；插件类与目录同名模式（如 `FitLinePlugin`）。
- 提交信息：短前缀 `fix:` / `feat:` / `refactor:` / `chore:` / `debug:`，主题具体，最终分支不留 `WIP:`。
- 不提交构建产物（`build*`、`make_debug` 均已 ignore）、本地 SDK 路径、日志、模型权重；用户插件状态在 `~/.deeplux/plugins`。
- 贡献细则见 AGENTS.md；插件用途/配置/失败条件见 docs/plugins.md。
