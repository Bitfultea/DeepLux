# 后台执行与 UI 刷新合并验收报告

- 日期：2026-11-01
- 分支：`feature/background-execution`
- 计划：[background-execution.md](../superpowers/plans/2026-10-30-background-execution.md)
- 设计：[background-execution-design.md](../superpowers/specs/2026-10-30-background-execution-design.md)
- 环境：Windows（MSVC 19.50 / C++17）、Qt 6.8.3、OpenCV；CTest `QT_QPA_PLATFORM=offscreen`，构建目录 `build-win-tests`。

## 1. 交付提交

| 批次 | 提交 | 内容 |
| --- | --- | --- |
| B0 | `c5eec83` 等文档提交 | 实施计划、设计与评审收口（基线冻结） |
| B1 | `08ca8ea` | `execution.affinity` 解析与模块执行分流 |
| B2 | `c0d4df2` | 执行收尾结果与异常安全（finalizer、失败/暂停/停止分类） |
| B3 | `243b4e5`、`12f27de` | 亲和性头注册；external job 租约与 owner 入口（`runOnceForJob/stepOnceForJob/resumeForJob`） |
| B4 | `2ab3731`、`ef051e3` | 用户暂停模块边界协议；job 启动与外部取消的串行化修复 |
| B5 | `acd29e8` | `FlowExecutionService`：专用线程、命令槽、job 租约、Single/Cycle/Step/Resume/Stop/Shutdown |
| B6 | `31234db` | Agent 接入：`run_flow/pause_flow/resume_flow/stop_flow` 经服务执行；`AgentRunWaiter` 监听服务信号 |
| B5/B6 修复 | `3ed2b2a`、`4549692` | `pauseRun` 仅 Running 接受 + shutdown 收尾加固；Agent `waitForIdle` 不阻塞 GUI |
| B7 | `beb7f79`、`9696af3` | UI 运行/单步/循环/停止改走异步服务；主窗口超时等待用例 |
| B8 | `ec6402b`、`83a4911` | `DisplayFrame` 合并器（约 50ms + latest-wins）、视口保持、暂停状态同步与迟到帧丢弃 |
| B9 | `a4fca32`、`8c14e5b`、`28c8fde` | 三批插件审计与 `execution.affinity=worker` 标记 |
| B9 修复 | `276e1dc` | StrFormatPlugin 移回 gui（UI 绑定未拆分），审计规则注释沉淀 |
| B10 | 本提交 | FindCircle 响应性验收、`-j4` 隔离回归、文档收口 |

## 2. 测试命令与结果

本轮实际执行（2026-11-01，环境同上）：

```bash
# 构建
cmake --build build-win-tests -j 8
# exit 0（无 error C/LNK）

# B10 Step2 定向
ctest --test-dir build-win-tests -R \
  "test_mainwindow|test_agentactor|test_flowexecutionservice|test_runengine|test_imagerefreshcoalescer|test_workerpluginconsistency" \
  --output-on-failure
# 100% tests passed, 0 failed out of 9（含 test_runenginefinalizer/joblease/userpause）
# 用时 31.34s

# B10 Step3 全量（-j4，启用隔离）
ctest --test-dir build-win-tests -j 4 --output-on-failure
# 100% tests passed, 0 failed out of 91
# 用时 38.60s（含 ui_capture_mainwindow）
```

实际通过数按本轮运行记录为 **91/91（`-j4`）**、定向 **9/9**；未硬编码历史数字。

B10 回归期间 `test_mainwindow` 曾在 `-j4` 全量中出现一次失败（`testBreakpointPauseAndContinueFromRunButton`：引擎断点命中后立刻期望 `FlowExecutionService::State::Paused`，但服务工作线程尚未提交 Paused，读到 `Running` 瞬态）。已修复测试等待方式（改为 `QTRY_COMPARE_WITH_TIMEOUT` 等待 Service 状态到达 Paused），并在修复后连续执行 **10 次** `ctest -j 4` 全量，**10/10 通过（91/91）**，耗时约 39s/次。

## 3. 性能/响应性验收（仅覆盖已审计 worker 流程）

- 用例：`tests/test_mainwindow.cpp::testBackgroundExecutionKeepsUiResponsive`。
- 流程：载入验收工程 `tests/acceptance/projects/accept_findcircle.json`（GrabImage→FindCircle，FindCircle 已标记 `worker`），点击运行；FindCircle 进入执行时通过 `RunEngine::moduleStarted` 的 `DirectConnection` 在工作者线程释放 `QSemaphore` 屏障；主线程以 16ms `QTimer` 计数。
- 断言：`ticks > 0`（GUI 事件循环在 worker 执行期间未被阻塞）；`jobFinished(stopped=false)`；`flowexecutionservice.state()==Idle` 且 `RunEngine::isBusy()==false`；运行按钮回到“运行”；FindCircle 输出 `Circle2D` 与验收期望一致（center≈(320,240)、radius≈100，容差 3px）。
- 顺序构造：只使用 `QSemaphore` 屏障、16ms 定时器与完成信号；没有用 `sleep` 构造顺序。本机为合成 640×480 验收图，仅验证“事件循环保持响应”，**不是**产线图像/硬件上的吞吐基准（见第 6 节）。

## 4. 已审计 worker 清单（30）

| 分类 | 插件 |
| --- | --- |
| 图像处理（5） | Blob、CropImage、JigsawPuzzle、LoadPointCloud、PerProcessing |
| 检测识别（6） | EdgeDefectDetection、FindCircle、JiErHanDefectsDet、Matching、MeasureLine、MeasureRect |
| 几何测量（7） | DistancePL、DistancePP、FitCircle、FitEllipse、FitLine、LinesDistance、RegionStats |
| 逻辑控制（4） | If、Loop、StopWhile、While |
| 系统工具（6） | DataCheck、Folder、SaveData、SystemTime、TimeSlice、WriteText |
| 变量（2） | CreateString、MathPlugin |

一致性验证入口：`tests/test_workerpluginconsistency.cpp`（GUI/worker 同实例 + 双实例 worker 输出对比；`ctest -R test_workerpluginconsistency`）。逐插件审计结论与保持 gui 的原因见 [插件手册 §执行亲和性](../plugins.md#执行亲和性executionaffinity与审计状态)。

## 5. 计划最终验收清单 10 条

| # | 验收项 | 结论 | 证据 |
| --- | --- | --- | --- |
| 1 | `RunEngine::runOnce/stepOnce/start` 同步语义与 CLI 回归 | 通过（自动化） | `test_runengine`、`test_clihandler`、全量 CTest 通过；`runOnce/stepOnce/start` 保留原同步入口并复用同一执行路径 |
| 2 | 单次/循环/单步/断点恢复经 `FlowExecutionService`，UI 不阻塞 | 通过 | `test_flowexecutionservice`（13 用例）、`test_mainwindow`（运行/单步/循环/断点/服务暂停用例）、定向 9/9 |
| 3 | `execution.affinity` 串行与并行入口遵守；无 worker→QWidget | 通过（审计+测试） | `test_executionaffinity`；B9 静态审计 + `test_workerpluginconsistency`；worker 名单见 §4 |
| 4 | job 租约覆盖整个 job；Cycle 轮间修改被拒；owner/非 owner 入口规则 | 通过 | `test_flowexecutionservice`（Cycle 轮间 `loadProject/clearModules` 被拒）、`test_runenginejoblease` |
| 5 | 用户暂停 A→B→C；Paused 不发 `runFinished` | 通过 | `test_flowexecutionservice::testUserPauseAndResumeAtModuleBoundary`、`test_runengineuserpause`、`test_agentactor` 暂停用例 |
| 6 | 异常注入后租约释放、状态回落、后续可运行 | 通过 | `test_flowexecutionservice::testJobInternalExceptionAllowsNextRun`、`test_runenginefinalizer` |
| 7 | 原生 `start()` 循环至少两轮；收尾窗口 stop 获胜 | 通过 | `test_flowexecutionservice::testCycleRunsAtLeastTwoRoundsAndKeepsEngineBusy`、`test_runengine`/`test_runenginefinalizer` |
| 8 | 图像 50ms 频率上限 + latest-wins；叠加同帧快照；旧 job 不污染新工程 | 通过 | `test_imagerefreshcoalescer`；`test_mainwindow::testProjectSwitchDropsLateDisplayFrames`、`testMeasurementOverlayReadsFrameSnapshotNotLiveOutput` |
| 9 | 性能验收只覆盖已审计 worker 流程（至少 FindCircle） | 通过 | §3：FindCircle 验收工程 + 16ms 定时器 tick>0 + 最终结果 + `isBusy()==false` |
| 10 | 全量 CTest、截图门禁、`git diff --check` 通过 | 通过 | `ctest -j 4`：91/91（含 `ui_capture_mainwindow`），修复测试瞬态竞态后压力复跑 10/10 全过；`git diff --check` 无输出 |

未验证项见第 6 节，均未写成已通过。

## 6. 偏差、限制与取舍

1. **Stopping 时机与 design §5.1 措辞差异（本轮不改实现）**：design §5.1 的状态列表含 `Stopping`；实现只在停止请求实际胜出时短暂提交 `Stopping` 后立即 `Idle`，正常/失败收尾不经过 `Stopping`。取舍：保持 `Stopping` 仅表示“停止正在收尾”，避免正常完成也对外呈现停止中；调用方以 `jobFinished(stopped)` 判定结果。该差异记录于此，若后续 UI 需要区分“收尾中”可再统一。
2. **`-j4` 隔离缺陷处置**：`tests/CMakeLists.txt` 对 `test_agentbridge` 与 `test_mainwindow` 设置 `RUN_SERIAL TRUE`（此前两者并行会因全局插件目录/窗口状态互相抢占出现管道隔离失败）。本轮 `ctest -j 4` 全量 91/91 通过，未降级为 `-j1`。计划 Step3 的 `mktemp HOME` + `scripts/sync-plugins.sh` 隔离脚本为 Unix 口径；Windows 下等价做法是测试内使用 `QTemporaryDir` 安装插件并配合 `RUN_SERIAL`，已在本轮采用。
3. **Matching 为 design §6.4 补审项**：plan B9B 候选清单未列 `Matching`，但 design §6.4 的耗时模块闸门点名；本轮补做静态审计 + 一致性冒烟并标记 `worker`，结论与证据并入 §4。
4. **审计规则以注释沉淀（非自动检查）**：t21 复核新增的“process() 不得创建/移交 QObject”“审计须扫描单例/全局状态”两条规则写在 `tests/test_workerpluginconsistency.cpp` 注释中，未实现自动静态检查；后续可考虑独立 lint/CI 规则。
5. **StrFormatPlugin 限制（不进入 worker）**：`process()` 在 worker 写 `GlobalVarManager` 并 `new VarModel(QObject)`，GUI 侧 `GlobalVarView` 读写/删除这些 QObject 且 `count()/isEmpty()` 无锁。保持 `gui`，与 QueueIn/QueueOut 按共享状态延后同类；若需迁 worker，应按“process 只做格式化 + queued 写入 GUI”或“GlobalVarManager/VarModel 线程安全化（另立专项）”处理。
6. **B10 回归发现并修复的测试竞态**：`test_mainwindow::testBreakpointPauseAndContinueFromRunButton` 原先在引擎断点命中后立即断言 Service 为 `Paused`；引擎先暂停、Service 工作线程随后才提交 `Paused`，存在短暂窗口（`-j4` 全量中偶发一次失败，实际读到 `Running`）。已改为等待 Service 状态到达 `Paused`（`QTRY_COMPARE_WITH_TIMEOUT`），随后 `ctest -j 4` 压力复跑 10/10 通过。该修复属测试等待方式的确定性修正，未改变产品行为。
7. **性能基准边界**：本轮响应性验收使用合成 640×480 验收图与 offscreen 环境，FindCircle 已 `worker`；未在产线相机图像/硬件上做吞吐或端到端时延基准。

## 7. 未验证项（不得视为已通过）

- Windows 实机（带真实显示器/DPI/桌面合成）手工验收：本轮为 offscreen 自动化。
- 真实相机采集、PLC/串口/TCP 通讯与 HALCON/ONNX 推理设备路径：仅既有测试与代码审计，未接硬件。
- TSan/其他线程检查器：未运行（Windows MSVC 环境未启用）。
- 长时稳定性/循环老化（小时级）：未执行。
- `DefectDetection`（hymson3d）：本构建 `ENABLE_HYMSON3D=OFF`，插件未构建，保持 `gui`，未做一致性冒烟。
- Linux/macOS 实机：本轮仅 Windows 构建；跨平台差异未验证。
- 产线性能基准（真实图像分辨率/节拍/并发）：未测，§3 仅验证 GUI 响应性。

## 8. 复现

```bash
cmake --build build-win-tests -j 8
ctest --test-dir build-win-tests -R "test_mainwindow|test_agentactor|test_flowexecutionservice|test_runengine|test_imagerefreshcoalescer|test_workerpluginconsistency" --output-on-failure
ctest --test-dir build-win-tests -j 4 --output-on-failure
git diff --check
```

截图门禁：`ctest -R ui_capture_mainwindow`（`ui_capture_mainwindow` 已包含在全量中）。
