# 相机连接与采集调用线程设计

状态：`DESIGN_REVIEW_PASS`（独立 Agent 审核 PASS；仅认可设计完整性，不授权实现）。该设计与 `agent-camera-async-discovery-design.md` 配套，不计入 G5。

## 现状和风险

- `CameraManager::connectCamera()` 持 manager mutex 调用驱动 `isAvailable()/discoverCameras()/createCamera()`，随后同步 `ICamera::connect()`。
- `disconnectCamera()` 在持锁状态下同步 `isAcquiring()/stopAcquisition()/disconnect()` 并 delete 对象。
- `CameraLease::get()` 暴露 QObject 裸指针；lease 只阻止删除，不保证调用线程正确。
- `AgentActor::getCameraDetails()` 在 Manager 线程直接同步调用 camera getters；`connect_camera/disconnect_camera` 同步调用 Manager API。
- `GrabImagePlugin::process()` 在 RunEngine 执行线程直接设置曝光/增益、启动采集、切触发模式、软触发并轮询 `lastImage()`；其内 `processEvents()` 不能修复 QObject affinity。

## 硬性不变量

1. 每次 `ICamera` QObject 虚调用都在 `camera->thread()` 执行；不得因持有 `CameraLease` 就跨线程调用。
2. CameraManager 的 mutex 只保护 map、代次、lease 和状态元数据。持锁期间不得调用 plugin/camera 虚函数、等待 queued callback、发出可同步重入的用户信号或销毁设备对象。
3. Agent/UI 事件线程不得同步等待硬件 connect/disconnect/acquire/trigger/setter 的完成。操作返回 requestId/pending 或快速接受结果；超时状态不确定时 `retryable:false`，不自动重放。
4. 动态库只可在所有 camera QObject、操作 request、SDK worker 退出并清理后卸载。当前 camera plugin unload unsupported 的门继续保留。
5. RunEngine/GrabImage 结果通路仍能在有限超时或用户 stop 下收口；每个 request 恰一次完成/失败/取消结果，不丢图像、不过期结果写回新设备会话。

## 候选线程模型：每台相机一个专属 QObject 线程

1. CameraManager 在其 owner thread 请求 plugin QObject 的 owner thread 创建设备对象，使用异步 discovery 缓存快照；不重复同步扫描。跨线程插件工厂调用使用有 request ID 的 queued request，并在插件线程完成。`createCamera(cachedInfo)` 必须返回无 parent 的 camera QObject，且 QObject 创建线程等于工厂当前线程；返回 parented、线程不符或工厂线程没有事件循环时 fail closed，不猜测修正。
2. `moveToThread()` 只从 camera QObject 当前所属线程执行。设备对象在所属插件线程创建后，由该线程把无 parent camera 移到每台设备专属 `QThread`，成功确认 affinity 后才启动线程 event loop 和调用 `connect()`。连接/断开、能力 getter、参数 getter/setter、acquisition、trigger、lastImage 和设备销毁统一通过 queued invocation 投递到 camera QObject affinity thread。若插件 QObject 与 Manager 不在同一线程，Manager 不得直接调用工厂虚函数，必须通过插件自身线程。
3. CameraManager 先锁内取得 device object、operation generation 和 lease token，释放锁，再发起 queued operation。结果通过有 request ID 的 queued callback 返回 Manager；重新锁内校验 generation 后提交状态，锁外 emit 状态信号。
4. `CameraLease` 改成仅持有设备身份和生命周期令牌，不再暴露 `ICamera*`。Lease 可在任何线程析构，但释放只排入 CameraManager 所属线程的 queued release request；在该 queued request 实际处理前 lease 计数保持有效，不在析构调用线程触发断开、SDK 调用或 QObject delete。Manager 关闭时 stop accepting lease、等待所有 release request drain。Agent 详情由有界 read request 生成 value snapshot；旧裸指针访问器先查找并迁移所有调用者，再删除或标 deprecated。
5. Agent connect/disconnect/get-details/set-parameter/trigger/capture/stop 使用统一的相机 request table；批准后只启动一次，`wait_camera_operation(requestId, timeoutMs)` 等待同一操作，超时返回 pending/uncertain 且保留令牌。request 在 CameraManager 生命周期/进程生命周期有界保存，过期结果按 device generation 丢弃。
6. `GrabImage::process()` 可在 RunEngine worker 等待 Manager 的异步操作完成值，绝不直接调用 camera QObject 或运行 `QCoreApplication::processEvents()`。等待使用可被 RunEngine stop 唤醒的 condition/future 和 cancellation token；取消必须取消/失效当前 capture request 并执行有序相机 stop。每个 frame/request 绑定 `runGeneration + deviceGeneration`，超时或迟到图像不写回新 run/新连接。Manager/camera QThreads继续处理 queued calls。
7. GUI 的 `MainWindow`/`MainViewModel` 执行入口和 `stepOnce` 必须改用异步 Engine 生命周期，不得在主线程同步调用 `runOnce()/stepOnce()` 后等待需要回到主线程的相机 callback；所有 run 完成通过 queued signal 更新 UI。若某条无法改异步的同步 Engine 入口继续存在，则该入口必须拒绝包含需等待相机操作的流程，不能静默阻塞 UI。
8. 超时不强杀线程。单相机操作 single-flight；并发 set/trigger/disconnect 根据当前操作状态返回 `CAMERA_OPERATION_BUSY` 或安全排队。断开需等待同一设备的在途操作结束/取消确认后再 stop/disconnect/delete，保留现有 lease 保护语义。
9. Camera QObject 的 deleteLater 必须在其 affinity thread 调用并等待 destroyed/线程退出确认；QThread 不得在仍运行时销毁。保持 PluginManager loader lease，直到所有 camera QObject destructors、SDK finalize 和 worker QThread join 完成。应用 shutdown 明确先停止 UI/Agent/Engine 新请求，停止并 join CameraManager 的全部设备线程，清空设备实例/插件引用，然后才允许 PluginManager 卸载 camera libraries；若 driver 调用无法结束，则不卸载其动态库并显式报告 shutdown pending/failure。

## 主要替代方案和待审核选择

- **主线程 QObject + blocking queued invoke：**不会跨线程调用，但 Agent/UI 调用仍可能阻塞主线程且回调重入风险高；不建议用于慢硬件动作。
- **每相机专属 QThread（候选）：**事件循环与设备调用隔离清晰，但要求 camera QObject 可迁移且 factory object/parent 规则可证；插件 unload 生命周期、GUI/Engine 异步入口和设备线程退出顺序要精确实现。
- **整个 RunEngine 移到 Manager 线程：**会把项目/GUI QObject 一起跨线程，禁止采用。

## 必须拆分的验证门

1. 查找全部 `CameraLease::get()/operator->`、`getCamera/allCameras` 和所有 `ICamera` 调用点；包括同步 run/step、Agent Actor、UI、GrabImage、CameraSetView、关闭路径，按 owner thread 迁移，最终静态搜索无未批准跨线程调用。
2. 先用 fake camera 验证 QObject 创建线程、parent 约束、owner-thread move、queued operation、跨线程 lease release、快/慢 operation、timeout、迟到回调、disconnect pending、worker event-loop 退出/对象销毁线程。
3. 用阻塞 fake 证明 Agent 和 UI event loop 可响应；RunEngine cancel 唤醒等待；等待到期不自动重试；每个 run generation 恰一次完成；超时期间模块/相机对象和动态库都不会释放。
4. 验证同步 run/step 路径要么转为 async，要么明确拒绝需等待相机的流程；测试 GUI 状态更新、单步、取消、关闭窗口和运行期间关闭。
5. 逐个审查 V4L2/Hikvision/DirectShow 相机对象创建和 SDK 线程约束；不支持迁移的 driver 明确 unavailable/unsupported，不调用错线程。
6. 独立审核 CameraManager 状态机、request generation、批次/确认绑定、跨线程 lease 释放、RunEngine 等待策略、对象销毁和 shutdown/unload 次序后，才建立实施契约。

## 明确边界

本设计不声称 CameraManager 当前支持 QObject 移线程；不假设任一 vendor SDK 可跨线程；不新增无界等待或强杀线程；不把“测试 fake camera 通过”替代真实设备/SDK 的阶段 8 验收。G5 在所有发现、连接、参数、采集、停止和释放门通过前持续未完成。
