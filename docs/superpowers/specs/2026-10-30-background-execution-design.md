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
    bool shutdown(); // 仅 Idle 时销毁线程并返回 true；非 Idle 时 stop 后返回 false

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
- `shutdown()` 只允许在服务 Idle 时销毁执行线程；非 Idle 时先 `stop()` 并返回 `false`（推迟销毁），由调用方在 `jobFinished` 后重试。不能在 GUI 同步等待仍可能 `BlockingQueuedConnection` 到 GUI 的工作线程。
- 服务不调用 `RunEngine::start()`（其 100ms `QTimer` 绑定 GUI 线程）；Cycle 由服务工作线程自行循环。
- `state()/currentJob()` 用互斥或原子快照读取，保证跨线程可见；信号在状态提交后发出。

### 4.2 `RunEngine` 的职责

- 保持现有同步语义：`runOnce()/stepOnce()/start()/stop()/pause()/resume()` 供 CLI、测试和兼容路径使用。
- 新增模块亲和性缓存与执行分流，不改变执行顺序、runId/frameId、断点与端口语义。
- 为整个 Service job 增加 **job 租约**：`beginExternalJob(JobToken*)/endExternalJob(JobToken)`（RAII）。
  - 租约持有者是 `FlowExecutionService` 的当前 job，用不可伪造的 `JobToken`（单调递增 id + 进程内随机盐）标识；Service 在工作线程内持有。
  - `m_externalJobActive + m_externalJobOwner` 纳入 `isBusy()`；从 job 启动到 job 结束（含 Cycle 两轮之间）始终为真。
  - **所有者入口**：`runOnceForJob(token, reason)`、`stepOnceForJob(token, reason)`、`resumeForJob(token, reason)`、`pauseForJob(token)` 在启动协议内校验 token；只有所有者可绕过“external job active”的忙检查并执行。
  - **非所有者入口**：公开 `runOnce/runOnceChecked/stepOnce/runOnceAsync/start/pause/resume` 在 external job 活跃时一律拒绝（忙或 `external_job_active`），不得绕过所有者检查。
  - **`stop()` 例外**：任何线程/入口都可调用，作为安全阀；它只置取消并唤醒，不执行新工作。
  - **job 级取消标志**：`stop()` 在 `m_lifecycleMutex` 内设置 `m_externalJobCancelRequested=true`（仅在 external job 活跃时），并取消当前 token。所有 `*ForJob` 启动入口在同一锁内检查该标志；**旧 job 不能清除它**。只有 `endExternalJob(token)` 成功结束旧租约后，新的 `beginExternalJob()` 才能复位该标志并重置 token。
  - 因此不存在“Service 检查完停止 → 调用 engine.stop() → 同一 token 又启动下一轮”的窗口：`runOnceForJob` 会因 job 取消标志拒绝启动；Service 将本轮结束视为 stopped，不再发起下一轮。
  - `stop_flow` 工具接入 Service：Service job 活跃时调用 `FlowExecutionService::stop()`；无 Service job 时保留直接 `RunEngine::stop()` 兼容路径。
  - 租约期间 `loadProject/addModule/removeModule/clearModules` 与 `tryAcquireMaintenance` 一律拒绝；ProjectManager 的工程切换也因此被挡。
  - `beginExternalJob()` 在引擎已执行/维护时失败；Service 启动失败必须显式上报。`endExternalJob` 只接受匹配 token，重复/过期 token 无效；结束时在锁内清理取消标志，下一次 begin 才允许复位。
- 保留 `void runOnce()` 兼容 CLI/测试；Service 不使用它，而使用 `runOnceForJob`（内部走 `tryBeginForRun(Single)` 并返回失败原因）。
- 用户暂停复用引擎的“模块边界暂停上下文”（见 5.3）；断点/用户恢复都走 `RunIntent::Resume`，该路径不触碰 `m_cycleTimer`。
- 单步/恢复同理：Service 内使用 `stepOnceForJob/resumeForJob`，避免 `stepOnce()` 在入口被 `isBusy()` 拒绝。`stepOnceForJob` 的实际推进复用一个内部 `stepOnceImpl(token)`，公开 `stepOnce()` 只是无 token 的包装。
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
| 单次 | `Single` | `runOnceForJob(token, reason)` | 正常/失败 `jobFinished`；断点/用户暂停则 `Paused`；启动失败显式 `jobFailed` |
| 循环 | `Cycle` | 循环调用 `runOnceForJob(token, reason)`；每轮后检查停止/暂停，立即续跑并短让出 | 停止/暂停/失败退出；`jobFinished(Cycle, stopped)` |
| 单步 | `Step` | `stepOnceForJob(token, reason)` | 一步结束 `jobFinished`；启动失败显式 `jobFailed` |
| 断点/用户继续 | `Resume` | `resumeForJob(token, reason)` | 若暂停前是 Cycle，Resume 完成后继续 Cycle |
| 暂停 | 状态切换 | 当前模块返回后停在模块边界 | `Paused(user)` |
| 停止 | `stop()` | `requestCancellation()` + `wakeAll()` | 立即返回；协作退出后 `jobFinished(stopped)` |
| Agent `run_flow(once, wait=true)` | `Single` + 等待 | 工作线程执行；GUI 只等完成信号 | 超时/取消调用 `stop()` |
| Agent `run_flow(mode=cycle)` | `Cycle` | 工作线程执行 | 立即返回 started；忽略 wait；stop_flow/abort 取消 |
| Agent `pause_flow/resume_flow` | service pause/resume | 同上 | 不再直接走引擎执行路径 |

### 5.3 关键语义

1. **循环节拍**：保持 UI 现状，上一轮结束立即下一轮，仅 `yieldCurrentThread()`。
2. **断点**：`runOnceForJob()` 命中内层断点返回 `Paused` 且不发 `runFinished`；服务记录“暂停前 job 类型”，继续后按原类型恢复。
3. **单步**：从 Idle 开始；暂停态不提供“单步越过断点”，保持现有“继续”语义。
4. **用户暂停（模块边界协议，复用断点暂停上下文）**：
   - `pauseForJob(token)`（公开 `pause()` 在 external job 活跃时拒绝）不再只改状态；它设置 `m_userPauseRequested`。执行循环在每个模块边界检查该标志，命中后调用现有 `setInnerPauseLocked(currentModule, pipelineData)` 保存**下一待执行模块、流水线数据、控制队列/循环索引、runId/frameId、暂停前 RunMode**，把暂停原因标为 `User`，然后返回。
   - 外层 `executeAcquiredRun` 在生命周期锁内提交 `Paused`，保留 `m_pauseResumeModule/m_pausePipelineData`，**不发 `runFinished`、不调用 `finalizeRunTail()`**；仅发 `pauseHit(module, reason)` 与 `stateChanged(Paused)`。
   - `tryBeginForRun(Resume)` 接受 `m_pauseResumeModule` 非空且暂停原因为 `User` 或 `Breakpoint`；恢复后从保存的模块继续，`m_skipBreakpointOnce` 只对断点原因设置，确保下游模块恰执行一次。
   - 区分 `pauseReason`（None/Breakpoint/User），`isPausedAtBreakpoint()` 仍只在 Breakpoint 时为真；UI/Service 用原因决定提示。
   - Cycle 轮与轮之间的暂停由 Service 自己的等待状态处理：若引擎已是 Idle，Service 在下一轮启动前等待；若引擎正在一轮中，则走上述边界暂停。
   - `stop()` 同时清除用户暂停请求与暂停上下文，防止旧 resume 重启。
   - 必测：A→B→C，A 执行中请求暂停，A 结束后停在 B 前；继续后 B、C 各执行一次，最终输出正确；暂停期间 `runFinished` 不触发。
5. **停止**：`stop()` 在生命周期锁内取消当前 token、设置 job 级取消标志并 `wakeAll()`，覆盖空闲/暂停/循环间隔等待；标志持续到旧 job 租约结束，`*ForJob` 不能启动下一轮。`Stopping` 期间拒绝新 job。
6. **执行收尾（必须区分结果）**：
   - `executeAcquiredRun/stepOnce` 使用 `ExecutionFinalizer(outcome)`，outcome 显式区分：
     - `Completed`：清理暂停上下文、释放执行权、发 `runFinished/stateChanged`。终态按运行模式决定：
       - `RunMode::RunOnce`（单次/单步，以及 Service 的每轮 Single）→ `Idle`；
       - `RunMode::RunCycle`（兼容路径 `RunEngine::start()` 的原生定时器循环）→ **保持 `Running`**，否则后续 tick 会被 `tryBeginForRun(CycleTick)` 拒绝；
       - 若 `m_stopPending` 或状态已是 `Stopped`，**stop 优先** → `Stopped`，不得被 Completed 覆盖为 Running/Idle。
     - `Paused`：**只释放本段执行权**，状态 Paused；保留 `m_pauseResumeModule/m_pausePipelineData/控制队列/循环索引/runMode` 与 job 租约；**不发 runFinished**；
     - `Stopped`：清理暂停上下文、状态 Stopped、释放执行权、发停止结果；
     - `Failed`：清理暂停上下文、记录失败、释放执行权、发失败结果。
   - finalizer 是 RAII：正常返回、提前 return、异常都会执行一次收尾；异常路径默认 outcome=`Failed`。`Paused` 必须显式提交，不能被析构函数误判为 Completed/Failed。终态提交与 `m_stopPending` 判定在同一 `m_lifecycleMutex` 临界区，保证并发 stop 优先。
   - Service Cycle 不使用 `RunMode::RunCycle` 的原生定时器路径；它每轮执行 Single（Completed→Idle），由 Service 循环下一轮，job 租约维持 busy。
   - 单模块 `module->execute()` 的异常仍由 `ModuleBase::execute` 捕获；引擎 finalizer 负责其余异常（容器/回调/内存）并保证租约释放。
   - Service 的 `try/catch` 只作最后兜底与诊断，不承担引擎租约清理。
   - 必测：引擎层故障注入（测试钩子从 `executeAcquiredRun` 内部抛异常，而非仅插件 `process()`）验证租约释放、状态回落、后续 run 可启动；`Paused` 收尾不触发 `runFinished` 且保留续跑上下文；**原生 `start()` 循环至少执行两轮**；**收尾窗口内并发 stop 必须获胜（终态 Stopped，不回到 Running/Idle）**。
7. **关闭与生命周期**：
   - `closeEvent` 以 **Service 状态**（非仅 `engine.isBusy()`）判断是否有 job；Cycle 两轮之间引擎可能 Idle，但 Service 仍 Running。
   - 有 job 时：`service.stop()` 后 `ignore()` 关闭，设置 `m_closeWhenRunFinishes`，**不得在 GUI 同步 wait**；由 `jobFinished` 再次触发 close。
   - GUI 可能正在执行 `gui` 亲和模块或 `BlockingQueuedConnection` 调用；closeEvent 若同步等待会与该调用互等，必须避免。
   - `shutdown()` 仅在 Service Idle 时销毁线程；非 Idle 时 stop 并返回 false，调用方在 jobFinished 后重试。超时不得继续销毁仍可能访问 RunEngine/Service 的线程。
   - 进程强制退出由 OS 终止；不提供 `QThread::terminate()` 路径。
8. **UI 循环续跑删除**：`MainWindow` 的 `runFinished` 不再 `singleShot(1, executeFlowOnce)`。`runFinished` 仍表示“一轮执行结束”（Cycle 每轮都会发），因此不能用它代表整个 Cycle job；job 级收尾只用 `FlowExecutionService::jobFinished`。
9. **job 级屏障**：Service 从 `beginExternalJob()` 到 `jobFinished` 持有引擎 job 租约；Cycle 轮间引擎可回 Idle，但 `isBusy()` 因租约保持为真，工程加载/清空/切换被拒绝。

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

### 6.3 并行入口覆盖

除串行 `executeModule` 外，以下入口也会直接执行模块，必须使用同一亲和性规则：

- `RunEngine::executeParallel()`：池任务直接调用 `mod->execute()`。
  - 进入池前过滤/校验：只有 `affinity=worker` 的模块允许进入并行池；存在 `gui` 亲和模块时返回结构化错误 `gui_affinity_not_parallelizable`，由调用方（如 Parallel 插件）回退串行。
  - **禁止**在池线程用 `BlockingQueuedConnection` 统一回 GUI；GUI 线程若正在同步调用 `executeParallel` 并等待线程池，会形成互等死锁。
- `RunEngine::executeBatchParallel()`（内部并行批次）：
  - 并行批次资格增加 `affinity=worker`；`threadSafe/blocking` 条件仍保留。
  - 不满足条件的模块留在串行路径，由 `executeModule` 的亲和性分流处理。
- 任何新增模块执行入口必须复用同一个 `executionAffinity` 判定；代码审查以“所有 `mod->execute(` 调用点”为检查清单。
- 必测：
  - GUI 线程调用 `executeParallel` 含 `gui` 模块 → 返回错误且不阻塞；
  - `executeBatchParallel` 不会把 `gui` 模块放进池；
  - worker 模块可正常并行，结果与串行一致。

### 6.3 审计与标记

只有满足以下条件才标记 `worker`：

1. `process()` 不创建/启动 QTimer、QEventLoop、QTcpSocket、QSerialPort、QProcess；
2. 不触碰 QWidget/QPixmap/QPainter/OpenGL/视口；
3. 不调用 `QCoreApplication::processEvents()`；
4. 不依赖 CameraManager/相机 SDK 线程亲和性；
5. 无未加锁的全局可变状态；
6. 两个实例在 worker 线程完成输入/输出一致性冒烟测试。

相机、通讯、串口、ShowImage/DisplayData/ShowPoint、MeasurementInput、SAM/脚本等保持 `gui`；未审计插件保持 `gui`。最终名单以逐插件审计结果为准，不按目录批量放行。

**耗时模块迁移闸门**

- UI 切换到异步执行**不能单独作为“界面保持响应”的验收依据**。在切换 UI 前，必须先按实测耗时审计并迁移常用耗时模块，至少包括：`FindCircle`、`Blob`、`PerProcessing`、`Matching`、`MeasureCircle/MeasureRect/MeasureLine/MeasureGap`、`FitLine/FitCircle/FitEllipse`、`DistancePL/DistancePP/LinesDistance`、`ColorRecognition`、`QRCode`、`RegionStats`、`DefectDetection/EdgeDefectDetection`、`NPointCalibration` 等（最终以日志实测耗时排序为准）。
- `FindCircle` 当前单次约 0.25–0.63s；在它仍为 `gui` 时，含它的流程不满足“UI 响应”验收。
- 对确实必须 GUI 亲和的长任务：
  1. 优先拆分“计算”和“UI 绑定部分”，把计算迁到 worker；
  2. 若无法拆分（相机/通讯的 QObject 亲和性是典型），本轮明确列为限制，不纳入响应性通过范围；
  3. 运行期记录 module 耗时；`affinity=gui` 且耗时超过阈值（建议 100ms）时写 warning 日志（模块名、耗时、建议审计），让现场可定位。
- 后续如需把相机/通讯迁出 GUI，应单独立项引入 `io` 亲和性与 `moveToThread` 的对象迁移协议，不在本轮伪装成已解决。

## 7. 大图刷新合并

### 7.1 执行线程快照与帧身份

- `RunEngine` 增加显示帧捕获开关，由 UI 启动时开启、窗口不可见/无头模式关闭。
- 模块成功且输出含 `image` 端口时，在执行线程完成 `ImageData → QImage` 深拷贝（`QImage::copy()`）。
- 帧必须携带不可变身份与同轮结果快照：

```cpp
struct DisplayFrame {
    quint64 jobId;        // Service job 代号（单调递增，跨 Cycle 轮稳定）
    QString runId;        // RunEngine 每轮唯一 runId（Cycle 每轮不同）
    quint64 frameId;      // 该 run 内的帧序号（不以全局 frameId 作唯一键）
    QString moduleId;     // 产生该图像的模块
    QString viewportId;   // 初始仅主视口
    QImage image;         // 深拷贝，脱离共享缓冲
    QMap<QString, QVariantMap> moduleDataSnapshot; // 本轮各模块 allData() 的不可变副本（不含大图）
};
```

- `moduleDataSnapshot` 在**该 run 结束时**（`runFinished` 前）由执行线程一次性抓取；中间帧可只带图像、`moduleDataSnapshot` 为空（UI 此时不绘制测量叠加）。
- Service 在 job 启动时把 `jobId` 注入 RunEngine 的显示帧上下文（`setDisplayJobId`），job 结束/停止时清空；帧的身份不依赖每轮会重置的全局 `frameId`。
- 快照写入受锁保护的“每视口最新帧槽位”，只保留最新一帧。初始实现只覆盖主视口；`viewportId` 作为键保留。
- 发出轻量 `displayFrameReady(viewportId, jobId, runId)`；不把大图作为信号参数反复排队。
- 本轮只覆盖 2D `image`；仅点云的 3D 输出保持现有路径。

### 7.2 UI 合并器

新增 `ImageRefreshCoalescer`（`src/ui/display/`）：

- 每视口一个“刷新节流器” + 待刷新槽位：50ms 是**刷新频率上限**，不是每次通知都重置的防抖延迟。
  - 距上次渲染不足 50ms 时只更新槽位；
  - 到点后取最新帧渲染，并记录本次渲染时间；若渲染期间又有新帧，则安排在 `lastRender + 50ms` 的下一个时点。
- `displayFrameReady` 只更新槽位并请求调度，不重置已排定的刷新时点。
- 同一窗口内多次通知只渲染最后一帧；不积压历史帧。
- **唯一显示入口**：明确移除 `MainWindow` 中 `moduleFinished → displayImage()` 的大图刷新路径；`moduleFinished` 只更新节点状态/耗时/检查器数据。所有大图刷新必须经合并器。
- 强制 flush 只发生在：Single/Step job 结束、Cycle job 结束/停止/暂停、窗口由不可见变为可见。**Cycle 每轮的 `runFinished` 不强制 flush**，否则会抵消合并；轮内刷新继续由 50ms 节流器决定。
- 测量叠加在对应 flush 之后更新；Cycle 轮内的叠加更新不得绕过合并器。

### 7.3 测量叠加只读帧内快照

- `MainWindow::updateMeasurementResultOnOverlay()` 不再调用 `RunEngine::moduleOutput()` 实时读取；改为消费当前 `DisplayFrame.moduleDataSnapshot`。
- 叠加结果与图像必须来自同一 `(jobId, runId)`；缺快照的中间帧清除叠加，避免 A 帧图像搭配 B 轮结果。
- 新 job 启动、工程切换、job 停止时清空合并器待处理帧与叠加；迟到帧按 `jobId/runId` 丢弃，不能污染新工程。
- 必测：
  - 延迟 GUI 消费 A 帧，worker 已开始 B 轮：A 帧渲染时只使用 A 的 `moduleDataSnapshot`，绝不读取 B 的实时输出；
  - 旧 job 的迟到 `displayFrameReady` 不改变新工程/新 job 的画面与叠加。

### 7.4 视图保持

- `HImageWidget/ViewportWidget/DisplayManager` 增加 `preserveView` 参数。
- 后台/周期刷新传 `true`：保留 zoom/offset，不重置 ROI/叠加。
- 首次显示、控件 resize、用户显式“适配”、点击模块选择时传 `false`。
- 移除 `MainWindow::displayImage` 中额外的 `toQImage()` 空判断，避免双转换。

### 7.5 降级

- 窗口不可见/最小化或没有显示消费者：关闭帧捕获，工作线程不做快照与合并调度。
- 槽位只存一帧，内存有界；UI 不消费时仅保留最新帧。

## 8. UI 与 Agent 接入

### 8.1 MainWindow

- `onRunOnce/onRunCycle/onStepRun/onStop` 改调 `FlowExecutionService`。
- `syncModulesToRunEngine()` 仍在启动前于 GUI 同步执行；服务非 Idle 或引擎忙时拒绝。
- `runFinished` 保留耗时/状态/叠加更新，删除循环自动续跑分支。
- 单步异步后，`m_isStepMode` 在 `jobStateChanged/Finished` 中维护；快速连点由服务拒绝并保持按钮状态。
- `breakpointHit/pauseHit` 继续使用引擎信号更新“继续”按钮与状态；暂停原因区分断点/用户。
- 测量叠加改为消费 `DisplayFrame.moduleDataSnapshot`，禁止在渲染路径回读 `RunEngine::moduleOutput()`。
- `closeEvent` 按 5.3 第 7 条：Service 非 Idle 时 stop + ignore，等 `jobFinished` 再关闭；不同步 wait。

### 8.2 Agent

- `AgentActor::runFlow()`：
  - `mode=once, wait=true`：通过服务启动 Single，`AgentRunWaiter` 等待完成信号；超时/中止调用 `service.stop()`。
  - `mode=once, wait=false`：服务启动 Single。
  - `mode=cycle`：保持现有兼容语义——立即返回 `{"status":"started","mode":"cycle","async":true,"note":"cycle mode ignores wait"}`，`wait` 仍被忽略；Cycle 由服务后台驱动，`timeoutMs` 不约束它，停止通过 `stop_flow`/abort。不得把 Cycle 改成阻塞等待。
- `pause_flow/resume_flow` 改为服务暂停/继续；暂停原因与断点暂停在返回中可区分（如增加 `pauseReason` 字段，保持既有字段兼容）。
- `abortRunningTool()`/`stop_flow` 优先调用 `FlowExecutionService::stop()`；仅当没有 Service job 时才回退到 `RunEngine::stop()`，不得绕过 job 级取消标志。

## 9. 兼容性

- `RunEngine::runOnce/stepOnce/start` 同步语义不变；CLI 与现有测试不改。
- 不修改 `IModule`/`ModuleBase` 内存布局，不递增 `DEEPLUX_MODULE_INTERFACE_VERSION`；无需重建插件。
- 行为变化仅限 UI/Agent 路径：按钮异步、循环由服务驱动、图像合并且默认保留视图。
- 新增 API/信号只增不改；现有信号签名不变。
- 帧捕获默认关闭；无 UI 消费者时零额外开销。

## 10. 测试矩阵

| 层 | 用例 |
|---|---|
| 服务 | 单次/循环/单步/继续；并发启动被拒；停止立即返回；异常后状态复位；shutdown 非 Idle 拒绝销毁；state/currentJob 跨线程可见 |
| 启动判定 | `runOnceForJob` 忙/租约不匹配时返回 false+原因；Service 不把未启动轮次报告为完成 |
| job 租约 | 所有者 `runOnceForJob/stepOnceForJob/resumeForJob` 成功；非所有者公开入口被拒；Cycle 两轮之间 `isBusy()==true`；load/clear/remove/add 与工程切换被拒绝；jobFinished 后释放；错误/过期 token 无效 |
| Job 取消 | “Service 检查停止后、启动下一轮前”窗口内调用 `engine.stop()`：同 token 的 `runOnceForJob` 必须失败，Service 不发起下一轮；仅 endExternalJob 后新 job 可复位 |
| 原生循环 | `RunEngine::start()` 正常完成一轮后仍为 Running，至少执行两轮；收尾窗口并发 stop 获胜，终态 Stopped |
| 线程 | worker 模块在执行线程；gui 模块在 GUI 线程；无 worker→QWidget 调用 |
| 并行入口 | `executeParallel` 含 gui 模块返回 `gui_affinity_not_parallelizable` 且不阻塞；`executeBatchParallel` 不把 gui 模块入池；worker 并行结果与串行一致 |
| 故障注入 | 引擎层注入异常（非插件 process）后租约释放、状态回落、后续 run 可启动；Paused 收尾保留上下文且不发 runFinished |
| 帧一致性 | 延迟消费 A 帧时 worker 已开始 B 轮：A 图像只用 A 的 moduleDataSnapshot；旧 job 迟到通知不污染新工程；`(jobId,runId)` 不匹配的帧被丢弃 |
| 控制 | 断点暂停/继续；**用户暂停：A→B→C 在 A 后暂停，恢复后 B/C 各执行一次且 runFinished 不误发**；循环中停止；单步只执行一个模块；快速连点不重入 |
| UI | >500ms 模块执行时 GUI 定时器仍触发、停止按钮可用；运行/循环/单步状态；关闭在 Cycle 轮间被阻止且不死锁 |
| 图像 | 50ms 频率上限（连续通知不重置时点）；每轮 runFinished 不强制 flush；jobFinished/stop/pause 强制 flush；zoom/offset 保持；首次/resize fit；截图最终帧 |
| Agent | `run_flow once wait=true/false`、timeout/cancel、pause/resume；`mode=cycle` 立即返回且忽略 wait 的兼容测试 |
| 异常 | 抛异常插件后引擎状态回落、租约释放、后续 run 可启动 |
| 回归 | CLI `runOnce` 同步；现有 RunEngine/控制流/断点测试；全量 CTest；Windows 单独记录 |

## 11. 实施顺序

1. 元数据解析 + `RunEngine` 串行/并行亲和性分流 + 契约/线程测试（默认 gui，行为不变）；并行入口过滤/回退。
2. `RunEngine` 收口：带所有者 token 的 job 租约、`runOnceForJob/stepOnceForJob/resumeForJob`、模块边界用户暂停协议、按 Completed/Paused/Stopped/Failed 区分的 RAII 收尾、引擎层故障注入测试。
3. `FlowExecutionService` + 状态机/异常/停止/关闭/跨线程状态测试（不接 UI/Agent）。
4. **耗时模块审计优先**：按实测耗时审计并标记 `FindCircle`、`Blob`、`PerProcessing`、`Matching`、Measure/Fit/Distance 等常用算法为 `worker`；每批带一致性冒烟测试。未完成前不得以“UI 响应”作为验收结论。
5. Agent 接入 + 回归测试（保持 Cycle 立即返回语义）。
6. UI 接入 + UI 响应/循环/单步/用户暂停/断点/关闭测试。
7. 显示帧快照 + `ImageRefreshCoalescer`（频率上限、job 级 flush）+ 视图保持 + 截图门禁。
8. 剩余插件审计与 `worker` 标记；全量回归、Windows/真实环境未验证项记录。

## 12. 风险与缓解

| 风险 | 缓解 |
|---|---|
| `gui` 模块仍阻塞 GUI | 耗时模块审计闸门；`FindCircle` 等先迁 worker；剩余 GUI 长任务明确列为限制并记录 >100ms warning |
| `BlockingQueuedConnection` 死锁/退出期调用 | 仅工作线程且应用事件循环存在时使用；失败按模块失败；closeEvent 不同步 wait；shutdown 仅在 Idle 销毁 |
| 引擎状态/租约被服务绕过 | 整个 job 持 `beginExternalJob` 租约；Cycle 轮间 `isBusy()` 仍为真；启动走 `runOnceForJob` |
| 用户暂停无法恢复 | 复用并扩展断点暂停上下文（模块/流水/控制队列/runMode），A→B→C 语义测试锁定 |
| 异常跳过租约释放 | 引擎内按 Completed/Paused/Stopped/Failed 区分的 RAII finalizer；引擎层故障注入测试 |
| job 租约阻止自身单步/被旁路绕过 | 所有者 `JobToken` + `*ForJob` 内部入口；公开入口统一拒绝 |
| Completed 误把原生循环置 Idle | finalizer 按 `RunMode` 决定终态：RunOnce→Idle，RunCycle→Running；锁内 stop 优先 |
| stop 被下一轮启动覆盖 | job 级取消标志持续到租约结束；`*ForJob` 锁内检查；只有新 job 复位 |
| 并行池执行 gui 模块导致死锁 | 并行资格含 `affinity=worker`；gui 模块拒绝入池并回退串行，禁止池内阻塞回 GUI |
| 快照与下一轮输出共享缓冲 | 执行线程深拷贝 QImage；单槽 latest-wins；本轮仅 2D |
| 图像与测量结果跨轮错配 | DisplayFrame 携带 `jobId/runId/frameId/moduleId/moduleDataSnapshot`；叠加只读帧内快照；旧帧按身份丢弃 |
| 刷新被每轮 runFinished 抵消 | 仅 job 级结束/停止/暂停强制 flush；50ms 为频率上限而非防抖 |
| 插件不响应取消 | 协作式停止 + 文档；不强制 terminate；长任务轮询 token |
| 现有测试依赖同步行为 | 同步 API 不变，仅新增异步路径测试 |
| 循环空转 | 每轮 `yieldCurrentThread()`；停止/暂停每轮检查 |
| 关闭时 GUI 模块阻塞 | closeEvent 非阻塞等待；Service 状态阻止关闭；jobFinished 后再关 |

## 13. 验收标准

1. 全部现有测试通过；新增服务/线程/租约/暂停/合并/UI 响应测试通过。
2. **在已审计并标记 worker 的常用耗时模块流程中**（至少包含 `FindCircle`），>500ms 计算期间 GUI 定时器持续触发，停止按钮可点击。
3. 含未迁移 `gui` 长模块的流程不作为“UI 响应”通过；运行期 warning 记录模块与耗时。
4. 用户暂停可恢复：A→B→C 在 A 后暂停，恢复后 B/C 各执行一次，暂停期间不误发 runFinished。
5. 租约所有者可单次/单步/恢复；非所有者入口被拒；Cycle 两轮之间工程加载/清空/切换被 job 租约拒绝；停止后无新 job 启动，最终状态 Idle/Stopped。
6. GUI/worker 并行入口均遵守 affinity：gui 模块不入池、不造成 GUI 线程同步调用线程池的死锁。
7. Cycle job 期间大图只按 50ms 频率上限渲染最新帧；每轮 runFinished 不额外强制刷新；job 结束/停止/暂停强制 flush；后台刷新不重置用户缩放。
8. 延迟消费 A 帧时不会读到 B 轮结果；旧 job 的迟到通知不污染新工程；叠加与图像来自同一 `(jobId,runId)`。
9. 引擎层故障注入后租约释放、状态回落、后续 run 可启动；Paused 收尾不发 runFinished 且保留续跑上下文。
10. 关闭在 Service 非 Idle 时被阻止且不阻塞 GUI；无 worker 线程直接访问 QWidget；ABI 版本不变。
11. Windows、真实相机/通讯插件与长时稳定性未验证项单独记录，不写成已通过。

## 14. 评审修订记录（2026-10-30）

针对“控制流与生命周期缺口”评审，设计已做以下修订：

1. **用户暂停可恢复**：改为复用/扩展断点的模块边界暂停上下文（保存待执行模块、流水线数据、控制队列/循环索引、runMode），暂停不发 runFinished、不清暂停上下文；新增 A→B→C 单次执行测试。
2. **GUI 亲和长任务**：增加耗时模块审计闸门，`FindCircle`/`Blob`/`PerProcessing`/`Matching` 等必须在 UI 切换前审计迁移；未迁移的 GUI 长任务不纳入响应性通过范围，并记录 >100ms warning；相机/通讯迁出 GUI 另立 `io` 亲和性项目。
3. **job 级互斥与启动判定**：新增 `beginExternalJob/endExternalJob` job 租约，Cycle 轮间 `isBusy()` 仍为真；新增 `runOnceForJob/stepOnceForJob/resumeForJob` 使启动失败可判定；Service 状态/currentJob 增加跨线程同步契约。
4. **关闭安全**：closeEvent 以 Service 状态判断，有 job 时 stop + ignore，由 jobFinished 再关；不在 GUI 同步 wait；`shutdown()` 仅在 Idle 销毁，非 Idle 返回 false 推迟。
5. **异常收尾**：异常边界下沉到 RunEngine 持有执行权的作用域，RAII finalizer 统一释放租约/清理暂停上下文/提交状态；Service 只兜底。
6. **刷新合并**：明确删除 `moduleFinished → displayImage()`；50ms 为频率上限而非防抖；仅 job 级结束/停止/暂停强制 flush，Cycle 每轮 runFinished 不 flush。
7. **Agent Cycle 兼容**：保持 `mode=cycle` 立即返回并忽略 wait 的现有语义；Service 后台驱动，stop_flow/abort 取消。

## 15. 第二轮评审修订（2026-10-30）

针对提交 `06641fa` 的 P1/P2 复核，进一步修订：

1. **job 租约所有者规则**：引入 `JobToken`；所有者使用 `runOnceForJob/stepOnceForJob/resumeForJob`，公开入口在 external job 活跃时统一拒绝；补充“租约内单步成功、Cycle 轮间外部启动/维护被拒”的测试。
2. **并行执行入口**：`executeParallel` 拒绝 `gui` 亲和模块并返回 `gui_affinity_not_parallelizable`；`executeBatchParallel` 的并行资格增加 `affinity=worker`；禁止池线程阻塞回 GUI；补充 GUI 线程调用并行原语的防死锁测试。
3. **异常与暂停收尾**：`ExecutionFinalizer` 区分 `Completed/Paused/Stopped/Failed`；`Paused` 只释放本段执行权、保留上下文与 job 租约、不发 `runFinished`；增加引擎层故障注入测试。
4. **帧与结果一致性**：`DisplayFrame` 携带 `jobId/runId/frameId/moduleId` 与不可变 `moduleDataSnapshot`；测量叠加只读帧内快照，禁止回读实时 `moduleOutput`；旧 job/旧轮帧按身份丢弃；补充 A/B 延迟消费一致性测试。
5. **P2 一致性**：`shutdown()` 统一为 `bool`；动作表与测试矩阵统一使用 `runOnceForJob` 系列，不再混用 `runOnce()`/`runOnceChecked`。

## 16. 第三轮评审修订（2026-10-30，版本 51bc0d7）

1. **Completed 终态按运行模式区分**：`RunMode::RunOnce`（单次/单步/Service 每轮 Single）→ Idle；`RunMode::RunCycle`（兼容的 `RunEngine::start()` 原生定时器循环）→ 保持 Running；`m_stopPending` 或已 Stopped 时 stop 优先 → Stopped，且终态提交与 stop 判定在同一生命周期锁内。新增“原生循环至少两轮”和“收尾窗口 stop 获胜”测试。
2. **job 级取消标志**：`stop()` 在生命周期锁内取消 token 并置 `m_externalJobCancelRequested`；所有 `*ForJob` 启动入口同锁检查，旧 job 不能清除，只有结束旧租约后的新 `beginExternalJob` 可复位。`stop_flow`/`abortRunningTool` 接入 Service。新增“检查停止后、启动下一轮前”窗口的屏障测试，确保同 token 无法启动下一轮。
