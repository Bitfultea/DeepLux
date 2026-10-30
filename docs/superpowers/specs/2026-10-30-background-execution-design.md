# 后台执行线程与 UI 刷新收口设计

- 日期：2026-10-30
- 状态：设计已确认，待编写实施计划
- 关联代码：`src/core/engine/RunEngine.*`、`src/ui/views/MainWindow.cpp`、`src/core/agent/AgentActor.cpp`、`src/ui/display/DisplayManager.*`、`src/ui/widgets/ViewportWidget.*`、`src/ui/widgets/HImageWidget.*`
- 参考实现：`/home/charles/Data/Repo/DeepLux_original`（C# / WPF）

## 1. 背景与问题

当前 GUI 的“单次、循环、单步”在 GUI 线程同步执行：

- `MainWindow::executeFlowOnce()` 直接调用 `RunEngine::runOnce()`（`MainWindow.cpp:5812-5848`）。
- UI 循环由 `runFinished` 触发 `QTimer::singleShot(1, executeFlowOnce)` 再同步重入（`MainWindow.cpp:586-588`）。
- `onStepRun()` 直接调用 `RunEngine::stepOnce()`（`MainWindow.cpp:5773-5796`）。
- 单次/循环/单步执行期间，模块 `process()`、状态更新和大图刷新全部占用 GUI 事件循环。
- 部分插件在 `process()` 内调用 `QCoreApplication::processEvents()` 或 `QEventLoop::exec()`，进一步制造嵌套事件泵与重入风险。

同时，`moduleFinished → MainWindow::displayImage()` 对每个产图模块都执行一次大图刷新：`MainWindow::displayImage` 可能转换一次，`DisplayManager::displayData` 再转换一次；`HImageWidget::setImage()` 每次 `fitToWindow()` 会重置用户缩放/平移（`HImageWidget.cpp:23-38、63-79`）。执行快时会造成 UI 事件队列积压与刷新抖动。

## 2. 目标与非目标

### 2.1 目标

1. 单次、循环、单步、断点恢复统一在**一个专用后台执行线程**运行；UI 不再同步执行流程。
2. 引擎信号通过排队连接回到 GUI；模块状态与大图刷新不阻塞执行。
3. 大图刷新按视口做 **latest-wins + 50ms 合并**，默认保留用户缩放/平移。
4. 停止/取消路径保持可用：`stop()` 立即返回，UI 不冻结；执行线程协作退出后收尾。
5. 按模块元数据 `execution.affinity` 分级执行：纯算法模块后台执行，GUI/相机/通讯等模块回 GUI 线程执行。
6. 不破坏 CLI 与现有同步 API/测试；不递增插件 ABI。
7. 借鉴 C# 原版的常驻工作线程、模块边界暂停/继续/单步、快照排队更新、有界最新帧与定时批量范式，同时修正其无 join/超时、工作线程直接碰控件、刷新无合并等缺陷。

### 2.2 非目标

- 不改造相机/通讯插件内部的 SDK 回调线程模型。
- 不实现“强制终止线程”；停止仍为协作式。
- 不把 3D 点云刷新纳入本轮快照热路径；3D 仍走现有刷新方式。
- 不引入 QuickMode 配置、日志批量改造、异步日志或新的渲染后端。
- 不修改 `IModule` 虚函数或 `ModuleBase` 内存布局，不递增 `DEEPLUX_MODULE_INTERFACE_VERSION`。

## 3. 当前事实基线（设计依据）

### 3.1 执行线程

- `RunEngine` 单例无自管线程；`runOnce()/stepOnce()` 在调用者线程同步执行。
- `runOnceAsync()` 使用 `m_runPool`（`setMaxThreadCount(1)`）。
- GUI 循环是“GUI 线程反复同步 `runOnce()`”，不是引擎 `start()` 的 100ms 定时器循环。
- Agent `run_flow(wait=false)` 已使用 `runOnceAsync()`；`run_flow(wait=true)` 由 `AgentRunWaiter` 在 GUI 嵌套 `QEventLoop` 等待。
- `stop()` 置取消 token 并立即返回，不 join；取消是协作式的。

### 3.2 模块与线程亲和性

- `ModuleBase::isThreadSafe()` 仅表示并行批次可并发，不等于可离开 GUI 线程。
- `GrabImagePlugin` 构造时创建 `QTimer`，`process()` 内启动/停止；相机路径调用 `processEvents()`。
- TCP/PLC 插件在 `process()` 内使用 `QEventLoop::exec()`；串口/TCP 成员对象在创建线程有亲和性。
- `Delay` 标记 `threadSafe=true`，但 `process()` 调用 `QCoreApplication::processEvents()`，不能作为“后台安全”依据。
- 相机/通讯/显示类模块必须保持 GUI 线程亲和性。

### 3.3 图像链路

- `moduleFinished` → `MainWindow::displayImage` → `DisplayManager::displayData` → `ViewportWidget::displayImage` → `HImageWidget::setImage`。
- 可能重复 `ImageData::toQImage()`；`HImageWidget::setImage` 每次 `fitToWindow()`。
- `ImageData` 拷贝深拷贝 Impl，但 QImage/cv::Mat 像素缓冲共享；`toMat()` 在 const 方法中惰性写缓存，不适合并发只读。
- 现状不存在按视口合并或最新帧优先机制。

### 3.4 C# 原版可借鉴点

- `Project.Process()`：常驻后台线程 + `AutoResetEvent` 门闩。
- 模块边界断点：暂停/继续/单步共用同一事件 + bool 状态机。
- `ShowImage`：快照 + `BeginInvoke` + 单一显示锁。
- 日志：`ConcurrentQueue + 100ms DispatcherTimer` 批量 drain。
- `DataOut`：有界队列满时丢最旧。
- Basler：33ms 节流。

原版缺陷（本设计明确规避）：停止只设 bool、不 join/无超时；引擎无异常兜底；工作线程直接操作控件；刷新无合并且重置缩放/ROI；零超时等待；无界队列；每模块多次 UI 回调。

## 4. 总体架构

```text
UI 线程                        执行线程                         GUI 线程
MainWindow/Agent  ──job──> FlowExecutionService ──> RunEngine（同步核心）
                                      │                    │
                                      │              worker 模块：直接执行
                                      │              gui 模块：BlockingQueuedConnection ──> module->execute
                                      │                    │
                                      │<── engine signals ─┘（Auto/Queued）
                                      │
                                      └── displayFrameReady（轻量）──> ImageRefreshCoalescer（50ms）
                                                                          │
                                                                          └──> DisplayManager/ViewportWidget
```

### 4.1 新增 `FlowExecutionService`

位置：`src/core/engine/FlowExecutionService.{h,cpp}`。

职责：

- 持有应用级常驻工作线程；空闲时阻塞在 `QWaitCondition`，不忙等、不为每次运行反复创建线程。
- 串行执行 Single/Cycle/Step/Resume job。
- 统一停止/暂停/继续/关闭语义。
- 对每次 job 做异常兜底，保证状态释放。
- 不直接修改 RunEngine 内部成员；只调用公开同步入口。

建议 API：

```cpp
class FlowExecutionService : public QObject {
    Q_OBJECT
public:
    enum class JobType { None, Single, Cycle, Step, Resume };
    enum class State { Idle, Running, Paused, Stopping };

    static FlowExecutionService& instance();

    bool startSingle();
    bool startCycle();
    bool stepOnce();
    bool continueRun();      // 断点/暂停后继续
    bool pauseRun();
    void stop();
    void shutdown();

    State state() const;
    JobType currentJob() const;

signals:
    void jobStateChanged(State state, JobType job);
    void jobFinished(JobType job, bool stopped);
    void jobFailed(JobType job, const QString& error);
};
```

约束：

- 重复启动/单步在非 Idle（或 Paused）时返回 `false`，不排队；Paused 状态只接受 `continueRun()` 或 `stop()`。
- `stop()` 在任何状态下都安全、幂等；立即返回。
- `shutdown()` 设置退出标志、停止、唤醒等待者，`wait(timeout)`；超时记录诊断，不强制 terminate。
- 服务不调用 `RunEngine::start()`（其 100ms `QTimer` 绑定 GUI 线程）；Cycle 由服务工作线程自行循环。

### 4.2 `RunEngine` 的职责

- 保持现有同步语义：`runOnce()/stepOnce()/start()/stop()/pause()/resume()` 供 CLI、测试和兼容路径使用。
- 新增模块亲和性缓存与执行分流，不改变执行顺序、runId/frameId、断点与端口语义。
- 为外部循环驱动新增最小 API：`resumeForExternalDriver()`（等价于非断点 resume，但**不启动** `m_cycleTimer`）。服务暂停仍可调用现有 `pause()`（其 `stopCycleTimer()` 已做跨线程安全处理）；断点恢复继续使用 `resume()`。
- 不新增内部线程；执行线程由 `FlowExecutionService` 提供。

### 4.3 模块元数据

`metadata.json`：

```json
"execution": {
  "threadSafe": false,
  "blocking": false,
  "affinity": "gui"
}
```

- `affinity` 取值 `"gui"`（默认）或 `"worker"`。
- 非法值按 `"gui"` 处理并记录警告。
- `threadSafe`、`blocking` 语义不变。

## 5. Job 状态机与执行路径

### 5.1 状态

```text
Idle ──startSingle/startCycle/stepOnce/continueRun──> Running
Running ──模块边界命中 breakpoint──> Paused(breakpoint)
Running ──用户 pauseRun──> Paused(user)
Running ──stop──> Stopping ──当前模块返回/循环退出──> Idle
Paused ──continueRun──> Running（Resume job）
Paused ──stop──> Stopping ──> Idle
```

- 断点事实来源仍是 `RunEngine::isPausedAtBreakpoint()`；服务不复制断点判定。
- 服务状态与 `RunEngine` 状态最终一致；服务不绕过引擎租约/状态机。

### 5.2 动作映射

| 动作 | Job | 执行线程调用 | 完成处理 |
|---|---|---|---|
| 单次 | `Single` | `RunEngine::runOnce()` | 正常/失败 `jobFinished`；断点则 `Paused` |
| 循环 | `Cycle` | 循环调用 `runOnce()`；每轮后检查停止/暂停，立即续跑并短让出 | 停止/暂停/失败退出；`jobFinished(Cycle, stopped)` |
| 单步 | `Step` | `RunEngine::stepOnce()` | 一步结束 `jobFinished` |
| 断点继续 | `Resume` | `RunEngine::resume()` | 若暂停前是 Cycle，Resume 完成后继续 Cycle |
| 暂停 | 状态切换 | 当前模块返回后停在模块边界 | `Paused(user)` |
| 停止 | `stop()` | `requestCancellation()` + `wakeAll()` | 立即返回；协作退出后 `jobFinished(stopped)` |
| Agent `run_flow(wait=true)` | `Single/Cycle` + 等待 | 工作线程执行；GUI 只等完成信号 | 超时/取消调用 `stop()` |
| Agent `pause_flow/resume_flow` | service pause/resume | 同上 | 不再直接走引擎执行路径 |

### 5.3 关键语义

1. **循环节拍**：保持 UI 现状，上一轮结束立即下一轮，仅 `yieldCurrentThread()`。
2. **断点**：`runOnce()` 命中内层断点返回 `Paused` 且不发 `runFinished`；服务记录“暂停前 job 类型”，继续后按原类型恢复。
3. **单步**：从 Idle 开始；暂停态不提供“单步越过断点”，保持现有“继续”语义。
4. **用户暂停**：服务调用现有 `RunEngine::pause()`（内部跨线程停表）；恢复非断点暂停时调用新增 `resumeForExternalDriver()`，只提交 Running 状态、不启动 `m_cycleTimer`，避免在 worker 线程操作 GUI 亲和定时器。断点恢复仍调用 `RunEngine::resume()`（执行 `RunIntent::Resume`，不触碰定时器）。
5. **停止**：`stop()` 置取消并 `wakeAll()`，覆盖空闲/暂停/循环间隔等待；`Stopping` 期间拒绝新 job。
6. **异常**：工作线程对每次 `runOnce/stepOnce/resume` 外层 `try/catch`；异常转为失败并释放 job 状态。
7. **关闭**：维持 `isBusy()` 关闭保护；`shutdown()` 先 stop 再 wait，超时只记录。
8. **UI 循环续跑删除**：`MainWindow` 的 `runFinished` 不再 `singleShot(1, executeFlowOnce)`。

## 6. 模块亲和性分流

### 6.1 解析与缓存

- `PluginInfo` 增加 `executionAffinity`。
- `PluginManager` 解析 `execution.affinity`，默认 `gui`。
- `RunEngine` 在 `addModule/loadProject` 时按 `moduleId()` 查询并缓存 `QHash<QString, ExecutionAffinity>`。
- 未注册/测试模块默认 `gui`。
- 不修改 `ModuleBase`/`IModule` 布局，ABI 不变。

### 6.2 执行分流

在 `RunEngine::executeModule()` 调用 `module->execute(...)` 处：

```text
if affinity == worker or currentThread == appThread:
    execute inline
else:
    QMetaObject::invokeMethod(app, [&]{ execute }, Qt::BlockingQueuedConnection)
```

- `moduleStarted/moduleFinished` 由执行线程发出。
- 模块内部 QObject 保持在创建线程（通常 GUI），不跨线程启动 QTimer/socket。
- 取消 token 由执行线程设置；GUI 线程中的模块通过 `ModuleBase::isCancellationRequested()` 读取同一 token。
- `BlockingQueuedConnection` 失败（无应用对象/事件循环/正在退出）时模块按失败处理并记录，不静默跳过。
- 若当前已在 GUI 线程（CLI/测试同步路径），直接执行，避免自等待死锁。

### 6.3 审计与标记

只有满足以下条件才标记 `worker`：

1. `process()` 不创建/启动 QTimer、QEventLoop、QTcpSocket、QSerialPort、QProcess；
2. 不触碰 QWidget/QPixmap/QPainter/OpenGL/视口；
3. 不调用 `QCoreApplication::processEvents()`；
4. 不依赖 CameraManager/相机 SDK 线程亲和性；
5. 无未加锁的全局可变状态；
6. 两个实例在 worker 线程完成输入/输出一致性冒烟测试。

相机、通讯、串口、ShowImage/DisplayData/ShowPoint、MeasurementInput、SAM/脚本等保持 `gui`；未审计插件保持 `gui`。最终名单以逐插件审计结果为准，不按目录批量放行。

## 7. 大图刷新合并

### 7.1 执行线程快照

- `RunEngine` 增加显示帧捕获开关，由 UI 启动时开启、窗口不可见/无头模式关闭。
- 模块成功且输出含 `image` 端口时，在执行线程完成 `ImageData → QImage` 深拷贝（`QImage::copy()`）。
- 快照写入受锁保护的“每视口最新帧槽位”，只保留最新一帧。初始实现只覆盖主视口；`viewportId` 作为键保留，多视口后续可直接扩展。
- 发出轻量 `displayFrameReady(viewportId, frameId)`；不把大图作为信号参数反复排队。
- 本轮只覆盖 2D `image`；仅点云的 3D 输出保持现有路径。

### 7.2 UI 合并器

新增 `ImageRefreshCoalescer`（`src/ui/display/`）：

- 每视口一个 50ms 单次定时器 + 待刷新槽位。
- `displayFrameReady` 只登记并启动/续期定时器。
- 到点后从引擎槽位取一次快照，调用 `DisplayManager`。
- 同一窗口内多次通知只渲染最后一帧；不积压历史帧。
- `runFinished`、停止完成、断点暂停时强制 flush；测量叠加在 flush 后更新。

### 7.3 视图保持

- `HImageWidget/ViewportWidget/DisplayManager` 增加 `preserveView` 参数。
- 后台/周期刷新传 `true`：保留 zoom/offset，不重置 ROI/叠加。
- 首次显示、控件 resize、用户显式“适配”、点击模块选择时传 `false`。
- 移除 `MainWindow::displayImage` 中额外的 `toQImage()` 空判断，避免双转换。

### 7.4 降级

- 窗口不可见/最小化或没有显示消费者：关闭帧捕获，工作线程不做快照与合并调度。
- 槽位只存一帧，内存有界；UI 不消费时仅保留最新帧。

## 8. UI 与 Agent 接入

### 8.1 MainWindow

- `onRunOnce/onRunCycle/onStepRun/onStop` 改调 `FlowExecutionService`。
- `syncModulesToRunEngine()` 仍在启动前于 GUI 同步执行；服务非 Idle 或引擎忙时拒绝。
- `runFinished` 保留耗时/状态/叠加更新，删除循环自动续跑分支。
- 单步异步后，`m_isStepMode` 在 `jobStateChanged/Finished` 中维护；快速连点由服务拒绝并保持按钮状态。
- `breakpointHit` 继续使用引擎信号更新“继续”按钮与状态。

### 8.2 Agent

- `AgentActor::runFlow()`：
  - `wait=true`：通过服务启动 Single/Cycle，`AgentRunWaiter` 等待完成信号；超时/中止调用 `service.stop()`。
  - `wait=false`：服务启动 Single。
  - `mode=cycle`：服务启动 Cycle，不再调用引擎 `start()` 定时器循环。
- `pause_flow/resume_flow` 改为服务暂停/继续。
- `abortRunningTool()` 优先服务停止。

## 9. 兼容性

- `RunEngine::runOnce/stepOnce/start` 同步语义不变；CLI 与现有测试不改。
- 不修改 `IModule`/`ModuleBase` 内存布局，不递增 `DEEPLUX_MODULE_INTERFACE_VERSION`；无需重建插件。
- 行为变化仅限 UI/Agent 路径：按钮异步、循环由服务驱动、图像合并且默认保留视图。
- 新增 API/信号只增不改；现有信号签名不变。
- 帧捕获默认关闭；无 UI 消费者时零额外开销。

## 10. 测试矩阵

| 层 | 用例 |
|---|---|
| 服务 | 单次/循环/单步/继续；并发启动被拒；停止立即返回；异常后状态复位；shutdown 超时诊断 |
| 线程 | worker 模块在执行线程；gui 模块在 GUI 线程；无 worker→QWidget 调用 |
| 控制 | 断点暂停/继续；循环中停止；暂停后继续；单步只执行一个模块；快速连点不重入 |
| UI | >500ms 模块执行时 GUI 定时器仍触发、停止按钮可用；运行/循环/单步状态；关闭保护 |
| 图像 | 50ms latest-wins；runFinished/stop 强制 flush；zoom/offset 保持；首次/resize fit；截图最终帧 |
| Agent | `run_flow wait=true/false/cycle`、timeout/cancel、pause/resume |
| 回归 | CLI `runOnce` 同步；现有 RunEngine/控制流/断点测试；全量 CTest；Windows 单独记录 |

## 11. 实施顺序

1. 元数据解析 + `RunEngine` 亲和性分流 + 契约/线程测试（默认 gui，行为不变）。
2. `FlowExecutionService` + 状态机/异常/停止/关闭测试（不接 UI/Agent）。
3. Agent 接入 + 回归测试。
4. UI 接入 + UI 响应/循环/单步/断点测试。
5. 显示帧快照 + `ImageRefreshCoalescer` + 视图保持 + 截图门禁。
6. 插件审计与 `worker` 标记（分批，每批带一致性冒烟测试）。
7. 全量回归、Windows/真实环境未验证项记录。

## 12. 风险与缓解

| 风险 | 缓解 |
|---|---|
| `gui` 模块仍阻塞 GUI | 既定代价；逐插件审计，纯算法迁 `worker` |
| `BlockingQueuedConnection` 死锁/退出期调用 | 仅工作线程且应用事件循环存在时使用；失败按模块失败；关闭前先 stop |
| 引擎状态/租约被服务绕过 | 服务只调用公开同步入口，单 job 串行 |
| 快照与下一轮输出共享缓冲 | 执行线程深拷贝 QImage；单槽 latest-wins；本轮仅 2D |
| 插件不响应取消 | 协作式停止 + 文档；不强制 terminate；长任务轮询 token |
| 现有测试依赖同步行为 | 同步 API 不变，仅新增异步路径测试 |
| 循环空转 | 每轮 `yieldCurrentThread()`；停止/暂停每轮检查 |
| 关闭时 GUI 模块阻塞 | 维持现有 closeEvent 保护；等待当前模块返回 |

## 13. 验收标准

1. 全部现有测试通过；新增服务/线程/合并/UI 响应测试通过。
2. >500ms 模块执行期间 GUI 定时器持续触发，停止按钮可点击。
3. 循环运行不阻塞 UI；停止后无新 job 启动，最终状态 Idle/Stopped。
4. 大图 50ms 窗口内只渲染最新帧，后台刷新不重置用户缩放。
5. 无 worker 线程直接访问 QWidget；ABI 版本不变。
6. Windows、真实相机/通讯插件与长时稳定性未验证项单独记录，不写成已通过。
