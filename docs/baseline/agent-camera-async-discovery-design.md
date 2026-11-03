# 相机异步发现调用路径设计

状态：`DESIGN_REVIEW_PASS`（独立 Agent 审核 PASS；仅认可 discovery 设计完整性，不授权真实驱动实现）。本文件不是实现承诺；SDK 线程/生命周期和接口 ABI 未经验证前不得进入真实驱动实现。

## 背景与已知阻塞点

- `CameraManager::discoverCameras()` 持 `m_mutex` 遍历所有插件，并在锁内调用插件虚函数。慢驱动会阻塞调用线程；插件同步回调 CameraManager 可重入死锁。
- `registerCameraPlugin()` 当前也在持锁期间调用 `isAvailable()` 和 `discoverCameras()`，因此加载相机插件本身会在 PluginManager/Agent UI 线程执行硬件探测。
- Hikvision `isAvailable()` 和 `availabilityMessage()` 会调用 `MV_CC_EnumDevices`；不能把“可用性判断”当作无设备 I/O 的快速元数据读取。
- V4L2 枚举会访问 `/dev`、打开节点并 ioctl。DirectShow 初始化并使用 COM apartment。把这些 QObject 插件的既有同步虚函数简单投递到通用线程池不满足线程与 SDK 生命周期约束。
- `ICameraPlugin` 当前同步接口版本为 1.0。直接向接口/vtable 加虚函数会破坏插件 ABI。
- 发现路径不止 Agent：`registerCameraPlugin()`、`discoverCameras()`、`refreshCameras()`、`connectCamera()`、`CameraSetView::loadCamerasInternal()` 和 `GrabImagePlugin::process()` 都存在同步设备探测入口。只把 Agent 工具异步化不足以消除 UI/Engine 阻塞和重入风险。

## 提议方案

### 1. 保持旧接口，新增可选版本化异步发现接口

新增独立 `ICameraDiscoveryAsync` 接口 IID，例如 `com.deeplux.ICameraDiscoveryAsync/1.0`，不修改 `ICameraPlugin` vtable。能力由 `qobject_cast` 检测。异步接口接受 request token 并以值类型结果回调，包含 `requestId/pluginId/available/cameras/errorCode/errorMessage`；回调必须排队回到调用方指定 QObject 上下文，不向 worker 暴露 CameraManager 裸指针。

当前内置 V4L2、Hikvision、DirectShow 驱动逐个实现此接口。没有该接口的 legacy driver 明确报告 `CAMERA_DISCOVERY_ASYNC_UNSUPPORTED`；Agent 发现工具绝不回退到旧同步 `discoverCameras()` 或 `isAvailable()`。新接口只在插件 QObject 所属线程收到快速的 start 请求；任何 worker 都不得直接调用该 QObject 的虚函数。

### 2. 每个驱动负责它的线程和底层 SDK 约束

- V4L2 在专用 worker 执行目录遍历/open/ioctl；worker 只持不可变值参数，不调用插件 QObject。
- Hikvision SDK 线程模型、重复 initialize/finalize、与已打开/采集相机并发枚举规则**尚未核实**。禁止照搬“每请求 worker 初始化/释放 SDK”；在查阅该 SDK 版本的厂商官方文档并有设备/SDK owner 确认前，该驱动异步发现保持 `unsupported`。
- DirectShow worker 在线程内初始化并配对释放 COM apartment，枚举结束释放全部 COM 接口；在实现前核对微软 COM apartment 官方规则。
- worker 不得更新 CameraManager/Project/UI；只返回值结果。取消若无法中断底层 SDK 调用，应返回 `cancelled/timeout_pending`，并让后台 worker 在完成后自行清理资源。

### 3. 迁移所有同步调用入口

- `registerCameraPlugin()` 只注册对象和静态 metadata，不做设备 I/O。插件加载完成后由显式异步发现调用扫描。
- `CameraSetView::loadCamerasInternal()` 改为等待异步请求结果后更新列表；不在 UI 线程调用旧同步接口。
- `connectCamera()` 不再调用 `isAvailable()/discoverCameras()`；从最近成功发现缓存中取 `CameraInfo` 并创建/连接。设备在两次操作间消失时由 `createCamera/connect` 返回真实失败，不同步重扫。
- `GrabImagePlugin::process()` 不再自行发现或连接相机；仅使用已连接 Manager lease。没有匹配设备时返回具体错误，要求上层 Agent/UI 显式发现、连接。该行为变化需单独验证既有流程与 UI 配置。
- `discoverCameras()/refreshCameras()` 要么在完成调用者迁移后删除/私有化，要么仅留作测试/第三方兼容且明确标 deprecated、owner-thread only、不得用于 UI/Agent/Engine 主链路。代码搜索和静态门禁证明交互入口没有遗漏旧调用。

### 4. CameraManager 异步协调

- 注册插件只记录对象和元数据，不再同步探测设备；首次发现由显式 async 请求触发。
- start 请求在插件 QObject 所属线程执行。开始请求时在锁内为每个插件建立 discovery lease、记录 plugin ID/实例代次和请求 generation，随后释放锁再调用插件的快速 start 方法；不得从 worker 线程调用 QObject 插件。
- 不持有 CameraManager 锁调用插件、SDK 或用户回调。结果回来后重新取锁，确认插件实例代次仍一致、请求未过期，再验证 pluginId/deviceId、跨插件重复 ID，并合并状态；同一插件批次内重复 ID 采用首条有效记录并计诊断。
- 所有 Qt signals 在锁外发出。未实现异步接口或 unavailable 作为结构化逐驱动结果返回，不把缺失驱动误报为“发现零设备”。
- 同一插件 discovery single-flight：活跃请求期间新请求明确返回 `CAMERA_DISCOVERY_BUSY`，不并发调用不明线程安全的 SDK。timeout 不释放 plugin lease、不代表 worker 停止；只有 worker 已完成并清理后释放 lease。
- unregister/unload 在 pending discovery lease 存在时返回 `CAMERA_PLUGIN_IN_USE`。Manager 正常 shutdown 停止接收新请求并等待 worker 完成；不得在 worker 仍执行插件代码时卸载动态库。若 SDK 调用不能被有界终止，驱动不声明 async 支持；不提供“强杀线程”。进程退出等待策略要由审核明确接受，任何可能无界等待都必须对用户可见并保持 loader 活着。

### 4. Agent 工具与确认/超时

- `discover_cameras` 标 dangerous；确认摘要显示批准时将调用的注册插件 ID 集合及每个插件 async capability。确认快照不得同步调用 `isAvailable()`；availability 来自最近完成的异步扫描，或者标 `unknown_before_first_scan`。待确认期间注册集合或 capability 变化则整批过期，零调用。
- 工具只启动一次请求，立即返回 `pending` + requestId，或由阶段 7 的等待工具按 requestId 查询最终结果；不阻塞 Agent/UI 事件线程。
- 等待超时返回 `DISCOVERY_TIMEOUT_PENDING`、`retryable:false` 和 requestId。底层任务仍可能运行，禁止自动重启同一发现或把超时说成“没有设备”。用户可以只读轮询同一 requestId。
- request ID 在进程生命周期内唯一；迟到结果只能更新其仍有效的缓存 generation，不能覆盖更新的扫描；已过期/取消结果只清理 lease，不改当前缓存。

## 阶段化交付和门禁

1. **接口/生命周期原型：**新增异步接口和一个 fake plugin；验证 token 关联、queued callback、插件 lease 阻止 unregister、无锁调用、取消后资源最终清理。独立审查后才扩展真实驱动。
2. **官方约束审查：**核对具体版本 Hikvision SDK 与微软 COM 官方文档，冻结 initialize/finalize、并发访问、apartment、取消/等待退出约束。未证实的驱动保持 unsupported。
3. **入口迁移：**迁移注册、CameraSetView、connectCamera 和 GrabImage 的同步入口；检索仓库证明没有 Agent/UI/Engine 路径调用 deprecated sync discovery。
4. **驱动实现：**逐个驱动实现 async 路径；每个驱动有 fake/backend 可控的成功、错误、延迟测试。只有底层有真实可中断/有界超时能力时才声称取消；否则单飞、timeout-pending、lease 保留直到自然结束。
5. **Manager 合并：**测试请求期间注册/卸载、重复 ID、跨驱动 ID、迟到结果、错误状态、generation 和单飞；至少安排一个 signal 回调重入 `knownCameras()` 并在限定时间内结束。覆盖应用关闭时 worker 未完结但插件库保持加载。
6. **Agent/确认：**测试确认前 driver start 次数为 0、批准后每个 async-capable driver 一次、目标集合/capability 变化确认过期、Observer 拒绝、超时不重扫、相同 requestId 最终结果一致、迟到结果不污染新 generation。真实设备仍归阶段 8。

## 待独立审核的问题

1. 独立原型须先冻结跨插件 callback 的 ABI 和值类型，确认 queued callback 接收者寿命；不能仅因旧接口同步传递 `QList<CameraInfo>` 就推断异步传输安全。
2. Hikvision SDK 的准确产品版本及官方接口文档尚未确认；驱动在此门通过前明确 unsupported。
3. 不可中断 worker 的进程退出可能无界等待；若该产品风险不可接受，则相关驱动不得启用异步发现。
4. GrabImage 停止自动发现/连接会改变行为，需明确用户操作路径并加迁移说明。
5. 阶段 7 的 wait_for_run 不应复用；新增 discovery request wait/poll API，避免混淆 run 生命周期。

## 实施边界

本设计只针对**相机发现**路径，不授权修改代码，也不认为 G5 已通过。它不解决以下独立且当前未关闭的风险，必须另行设计、实现和审核，才能声称 G5 相机链路线程标准通过：

- `CameraManager::connectCamera()` 除 discovery 外仍在持锁时调用 `ICameraPlugin::createCamera()` 和 `ICamera::connect()`；`AgentActor::connect_camera` 仍同步执行，可能阻塞调用线程或在插件重入时死锁。不能因本设计迁移了 discovery 就把 Slice 15 视为满足异步/线程安全要求。
- `CameraLease` 只保证对象生命周期，不会切换线程。`GrabImagePlugin::process()` 在 RunEngine 工作线程取得 lease 后直接调用相机参数、采集和触发方法；所有相机 QObject 虚函数都须由其 affinity thread 执行或由受审核的执行器安全转发。不能因发现异步化就把 GrabImage 视为满足 G5。
- 连接、参数设置、trigger、capture、stop 的同步/异步调用和超时/取消行为需要独立线程模型契约；硬件操作结果或超时不确定性要保持 request identity，不得自动重放。

以上未解决项在阶段 5 总体验收中持续阻断 G5。每个实现 Slice 仍需各自冻结契约和独立复审。
