# Agent 改进计划阶段 5 验收记录

依据 `docs/agent-capability-improvement-plan.md` §10。**当前状态：Slice 1–130 全部通过独立实现审核；G5 标准 1–7 均有 PASS 证据，G5 阶段 5 PASS（限定于计划 §10 的软件与证据范围）；阶段 6 可进入，阶段 8 现场验收仍未通过。**标准 5/6/7 的独立只读审核结论已归档于 `build-codex-agent/agent-phase5-slice127-evidence/g5-standards-audit.txt`。Slice 127 已补齐 WriteText、SaveData、SaveImage 的批准后执行边界复核；Slice 128 关闭 SaveData/SaveImage 成功 bounded cycle 扩展证据缺口，Slice 129 关闭 PLCRead/PLCCommunicate 成功 bounded cycle 扩展证据缺口，Slice 130 关闭 TCPServer 成功 bounded cycle 逐轮收发计数缺口；标准 5 的直接工具/插件入口对账见 `docs/baseline/agent-phase-5-side-effect-crosswalk.md`，阶段 8 外部门槛见 `docs/baseline/agent-phase-8-external-validation-gates.md`。阶段 8 真实设备、生产模型与 LLM provider、Windows/Qt 6 和性能门槛均保持 `NOT_RUN`，不由软件审核 PASS 推定通过。工作区仍包含前序阶段和既有未提交修改；本工作未提交、未推送。

## 1. 前置门

G4 已于 2026-10-22 经独立六项总审判定 PASS，阶段 4 验收记录为 `docs/baseline/agent-phase-4-acceptance.md`。该前置结论只放行阶段 5，不替代本阶段验收。

## 2. Slice 1：插件模块只读可用性查询

契约：`本地逐切片契约（未纳入提交）`，已于 2026-10-22 经独立 Agent `/root/slice27_contract_audit` 批准实施。

实现只扩展 `get_available_plugins`：兼容保留原 `plugins` 名称数组及排序，新增结构化 `items`，每项区分已扫描元数据、当前平台候选库文件存在、PluginManager 实时加载状态和已声明 requirements。`PluginInfo` 保存扫描时 requirements 快照；缺失字段输出 `not_declared`，错误 platform 声明输出 `invalid_declaration` 与 `PLUGIN_REQUIREMENT_METADATA_INVALID`。加载状态优先于磁盘文件存在状态。返回不含相机，且不将库文件存在/插件已加载声称为 ABI、SDK、模型或设备就绪；Agent 生命周期 `executableActions` 保持空数组。

测试覆盖：旧 `plugins` 字段与 `items` 次序一一对应；临时无全局状态 fixture 的有库/缺库、单字符串 `platform` 数组归一化、requirements 保留/缺省、非法类型与 loaded 优先规则；真实 Agent 查询使用 `isPluginLoaded()`，并断言无工程变化、Undo count/index、Logger 记录及插件加载集合均不变。

## 3. 验证与独立审核

- `cmake --build build-codex-agent -j2`：最终改动快照构建成功，退出码 0。
- 定向 CTest：`test_agentactor`、`test_pluginmanager`、`test_toolschema`，3/3 通过。
- 受限环境全量 CTest：73/78 通过；5 个失败为已知本机 listener/socket 绑定限制：`test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro`。相同构建快照在此前已授权的 loopback 环境全量复跑：78/78 通过、0 失败、0 跳过；修复非法 metadata 标记及补充零副作用断言后，最终改动快照再次 loopback 全量复跑仍为 78/78、0 失败、0 跳过。
- `clang-format --dry-run --Werror`（切片涉及 C++ 文件）与 `git diff --check` 通过。
- 独立审核者 `/root/slice27_contract_audit` 对最终快照判定 **PASS，阻断 0**；独立构建三个目标、定向 CTest 3/3、全量构建成功，并核验受限环境五个 listener 失败及授权 loopback 78/78 证据。审核者未修改文件。

## 4. 明确未覆盖

Slice 1 本身不纳入相机驱动。Slice 1/2 均不报告扫描失败/无效 metadata 的插件，不主动探测 ABI/SDK/模型依赖，不验证实际硬件或相机/通讯状态，不实现扫描、安装、加载、卸载、重载操作，也未验证插件使用中卸载、确认前外部副作用或不确定超时。上述均仍属于 G5 后续切片或 G8 真实硬件门槛；G5 验收框保持未完成，能力矩阵不据此整体改 PASS。

## 5. Slice 2：相机驱动目录与平台适用性

契约：`本地逐切片契约（未纳入提交）`，2026-10-22 经独立 Agent `/root/slice27_contract_audit` 批准实施。

`get_available_plugins` 新增与模块分离的 `cameras` 数组，反映 PluginManager 当前扫描到的相机 metadata；保留 `plugins/items` 旧值、排序和模块语义。相机项增加 `platformCompatibility=supported|unsupported|not_declared`，未加载且平台声明不匹配时返回 `PLUGIN_PLATFORM_UNSUPPORTED`；库缺失可同时报告，已加载状态优先。ToolSchema 明确描述相机目录与平台字段，但不宣称设备、SDK 或模型就绪。阶段 0 F05 测试已随之后续切片更新为断言目录可查、平台字段存在，并核对当前支持的相机/插件工具名。

测试覆盖纯 helper 的大小写不敏感平台匹配、不匹配、未声明、畸形声明、loaded 优先，以及平台不符和缺库原因并存；Agent 查询前后核验模块/相机加载集合、CameraManager 注册插件、设备指针和状态、Project、Undo 与 Logger 均不变。F05 lifecycle 白名单定向用例通过。当前部署若未扫描某类相机 metadata（例如平台专属/SDK 关闭插件），查询不会伪造该项目为已发现；具体设备发现、加载相机插件及真实硬件均未覆盖。

Slice 2 最终全构建成功，定向 CTest（Actor/PluginManager/ToolSchema）3/3，更新后的 `test_agentphase0_repro` loopback 单套件通过，最终 loopback 全量 CTest 78/78、0 失败、0 跳过。独立 Agent 对最终实现审核判定 **Slice 2 PASS**，并独立重跑最终 `test_agentactor` 1/1；此前定向三套件与全构建证据通过。格式门禁和 `git diff --check` 通过。该 PASS 仅关闭 Slice 2，不表示 G5 通过。

## 6. Slice 3：模块插件实例租约与安全卸载

契约：`本地逐切片契约（未纳入提交）`。原 ModuleBase 子对象跟踪令牌因销毁点早于完整 `delete` 返回而被独立审核否决；修订后的管理器销毁边界经独立审核 **PASS** 后实施。

`createModule()` 对每个非空克隆登记单调实例 ID 并增加插件占用。新增 `destroyModule()`/`createManagedModule()`：管理器标记 Destroying，完整 `delete` 返回后才释放占用；同步 QObject destroyed 回调只清理身份登记，不会提前释放。外部旧调用方若直接 `delete` 裸指针，则永久 pin 插件（稳定拒绝原因 `PLUGIN_LIFECYCLE_UNTRACKED`）；未知指针由管理器拒绝且不删除。仓库内 Agent、RunEngine、流程树、主窗口、插件测试对话框及相关测试所有者已迁移到受管销毁；非 PluginManager 创建的 RunEngine/流程树测试模块仍由原 owner 删除。相机插件在本切片仍 fail-closed，不宣称相机可卸载。

验证：`cmake --build build-codex-agent -j2` 全构建成功；定向 `test_pluginmanager`、`test_pluginparametercontracts`、`test_plugincontracts`、`test_runengine` 4/4 通过，授权 loopback 后 `test_agentphase0_repro` 1/1 通过。全量授权 loopback CTest 为 77/78；唯一失败是 `test_mainwindow::testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme` 在 `tests/test_mainwindow.cpp:5878` 对“圆检测/找圆”按钮文案的既有断言不匹配，与本切片生命周期断言无关。单独复跑 `test_mainwindow` 为 104 passed、1 failed；其中本切片涉及的 `testRunningFlowRejectsModuleDeletion` 已通过。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。独立 Agent `/root/slice27_contract_audit` 审核实现判定 **PASS，阻断 0**，并重跑 `test_pluginmanager`、`test_plugincontracts`、`test_runengine`、`test_agentactor`、`test_pluginparametercontracts` 全通过；其因受限环境不能绑定本地 socket，`test_agentphase0_repro` 的 PLC listen 用例受限，本任务已在获批 loopback 环境补跑通过。

Slice 3 的结果仅关闭模块克隆卸载竞态这一切片；全量 CTest 的单一 UI 文案失败如实保留，不据此宣称全仓测试全绿。G5 其他插件操作、扫描/安装/重载、相机设备生命周期与真实硬件门槛仍未完成。

## 7. Slice 4：相机驱动归属、lease 与安全注销

契约：`本地逐切片契约（未纳入提交）`，独立 Agent `/root/slice27_contract_audit` 最终复审 **PASS、阻断 0**。`CameraManager` 以全局 `deviceId` 维护设备归属，空元数据 `pluginId` 规范化为提供驱动 ID，冲突归属被拒绝；刷新时保留仍在线原 owner，仅释放不再报告且无活动实例的旧归属。`tryUnregisterCameraPlugin()` 对未知插件、活动相机、跨线程和生命周期信号槽重入返回稳定错误；legacy `void` 接口不再隐式删除相机。相机连接/断开与所有 Manager 状态事件按提交顺序 FIFO 入队，并由 CameraManager 线程无锁派发。

设备访问新增 RAII `CameraLease`。lease 期间 disconnect 延迟到最后一个 lease 释放；pending disconnect 与 `disconnectAll()` sweep 期间拒绝新 lease/连接，批量释放结束后正确解除屏障。采集中相机严格先 stop 再 disconnect。GrabImage 已迁移到 lease，覆盖采集等待及事件处理期间的对象使用期。相机动态库 unload 仍 fail-closed；真实设备、厂商 SDK、Agent camera tools 和完整图像产出通路不在本切片验收范围。

Fake 驱动回归覆盖双驱动隔离、空/冲突 pluginId/deviceId、活动与采集中拒绝注销、刷新归属保持/释放、失败实例清理及信号槽重试、lease/pending 生命周期、disconnectAll 双设备 sweep、生命周期/状态信号重入、线程归属与并发状态事件顺序。最终 `cmake --build build-codex-agent -j2` 成功；定向 `test_cameramanager`、`test_grabimage`、`test_agente2e`、`test_pluginmanager`、`test_agentactor` 为 5/5；格式门禁及 `git diff --check` 通过。独立 Agent 对最终实现重跑 `test_cameramanager` 1/1 并判定 PASS。

当前快照全量 CTest 为 72/78：5 个 loopback listener/socket 绑定用例受当前沙箱限制（`test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro`）；剩余 1 个是既有 `test_mainwindow::testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme` 按钮文案断言（104 passed、1 failed），非 Slice 4 失败。未在本轮要求网络/权限升级，未将受限结果伪报为全绿。

（当时状态快照）G5 §10 共 7 条标准中，标准 2 已由 Slice 14/27/28 的缺库、ABI cast/版本拒绝、加载失败、使用中卸载与重载 Schema 回归证据闭合。其余标准及真实硬件门槛当时仍未全部通过：插件目录/平台适用性矩阵有未测驱动；相机完整模拟设备/GrabImage 与 SDK 可见错误、TCP/PLC/串口往返、确认前后副作用与拒绝路径、配置/目标变化及不确定超时重放策略、阶段 8 真实硬件仍需完成。后续 Slice 11 已补充 PTY 串口证据，Slice 29 已补齐模拟相机到 GrabImage 的集成证据；G5 当前汇总状态以本记录头部及计划 §10 为准。该历史快照不放行阶段 6。

## 8. Slice 5：GrabImage 模拟相机结果路径与超时

契约：`本地逐切片契约（未纳入提交）`，经独立 Agent `/root/slice27_contract_audit` 三轮复审 PASS；最终实现独立复审 PASS。

实现：`GrabImage` 对 CameraManager 未能连接的显式非数字 `cameraId` fail-closed，避免误映射为 OpenCV index 0；已有整数 OpenCV index 路径保留。增加可覆盖的 fallback opener seam，仅供验证实际调用入口。fake 回归注入相机创建失败和连接失败，断言 OpenCV opener 计数为 0；timeout 和成功采集也在 `QTimer` 事件回调中请求断开并断言 lease 保持对象直至执行返回、随后析构恰好一次。超时有可观察错误和有界时长；成功帧的像素/尺寸进入 `ImageData`；最后以 opener seam 验证整数 ID `7` 会原样映射为 index 7，不触碰真实设备。测试使用 scope guard 释放 CameraManager 单例资源。

验证：全量构建成功；最终定向 `test_grabimage` 与 `test_cameramanager` 2/2 通过，格式门禁与 `git diff --check` 通过。全量 CTest 为 72/78：5 个受限环境的 loopback listener/socket 用例失败（`test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro`），另有既有 MainWindow“圆检测/找圆”文案断言失败（104 passed、1 failed）。这些失败均不归因于 Slice 5；未将全量测试宣称为通过。独立 Agent 对最终实现复审 PASS，无阻断，并独立重跑定向两测试 2/2。

## 9. Slice 6：流程 PLC Modbus/TCP 本地收发

契约：`本地逐切片契约（未纳入提交）`。本片覆盖流程 `PLCReadPlugin` 的 FC03 和 `PLCWritePlugin` 的 FC16 请求/响应，不覆盖全局 `CommunicationManager`、通用 TCP/串口、Agent 确认/重试策略或真实 PLC。假服务仅绑定 `127.0.0.1` 临时端口。

实现补齐 Modbus 响应 transaction/protocol/unit/function/address/count/长度校验；TCP 接收按 MBAP Length 累计完整 ADU 后才解析。回归覆盖读写正确往返、首个 transaction ID、格式自洽但寄存器数量超请求的 FC03 响应、transaction/echo-address 错配、可观察错误、有效 FC03/FC16 响应分段发送，以及读写超时与服务端恰收一个请求。

验证：`test_communicationplugins` 与 `test_communicationmanager` 定向构建成功；授权 loopback 环境 CTest 2/2 通过；`clang-format --dry-run --Werror` 和 `git diff --check` 通过。独立 Agent 复审判定 **Slice 6 PASS，阻断 0**。独立审核执行环境本身不能监听 loopback，因此其 `test_communicationplugins` 运行受环境限制；产品测试通过结果来自本工作区获批 loopback 执行，不用 QSKIP 掩盖限制。本片没有重跑全量 CTest，不宣称全仓测试全绿。

Slice 6 复审提示的 PLCWrite `valueSource=variable` 风险由 Slice 7 单独收口；Slice 6 本身不覆盖该路径。Slice 6 仅关闭流程 PLC Modbus/TCP 本地模拟收发，不关闭 G5 的确认前零副作用、重试安全、串口/其他 TCP、配置与目标变化或 G8 真实设备标准。

## 10. Slice 7：PLCWrite 变量输入 fail-closed

契约：`本地逐切片契约（未纳入提交）`。变量模式现在只接受契约限定的原生数值标量或 `QJsonArray` 数字，校验 finite/integer/uint16、1–123 寄存器、`startAddress` 原始值范围与地址末端；缺失变量不再回退成 `{0}`。校验在连接前执行。

回归覆盖合法 scalar 和 123 项 array（末地址恰为 65535），缺失变量、空数组、bool/string、分数/负数/超 65535/NaN/Inf、array 非数字/越界/124 项、寄存器地址和范围溢出；无效用例同时断言 loopback 服务端 accepted connection 与请求数均为 0。Slice 6 immediate JSON 数组成功用例保留。

验证：两个通信测试目标构建成功；授权 loopback CTest `test_communicationplugins` 与 `test_communicationmanager` 2/2 通过；格式门禁与 `git diff --check` 通过。独立 Agent 对最终实现复审 **PASS，阻断 0**；其自身受限环境无法绑定 loopback，未独立重跑网络测试。本轮未重跑全量 CTest，不宣称全仓通过。独立审查建议可再为 `Float`/`Double` 正整数标量补白名单测试行；该项为非阻断增强，不影响本 Slice PASS。

## 11. Slice 8：cycle runtime failure 后 fail-stop

契约：`本地逐切片契约（未纳入提交）`；契约实施前经独立 Agent 批准，最终实现复审 **PASS**。流程已进入执行后，任一迭代失败不再安排下一周期：RunEngine 停止 cycle timer、清理 cycle mode/session 并结束为非忙碌；`runFinished` 保留失败结果，随后恰发一次 `cycleStopped`。通知待发期间阻止并发 stop 覆盖结果或新 cycle 越过旧停止信号；同步 DirectConnection 槽分发期间门仍关闭。

回归用 5 ms 周期失败模块，等待跨越多个周期并断言测试替身调用、`runFinished` 与 `cycleStopped` 均恰一次，`isCycleMode()==false`、`isBusy()==false`，session `runObservation.finished==true` 且保存失败结果及匹配的 `lastCompletedRunId`。额外验证 `runFinished` DirectConnection 中调用 stop 不会破坏失败 observation，以及 `cycleStopped` DirectConnection 中尝试重启被拒绝。既有 `testDeferredCycleContinuesAfterFlowValidationFailure` 与 `testCycleBreakpointResumeKeepsCycleMode` 保持通过。

验证：`cmake --build build-codex-agent --target test_runengine -j2` 成功；offscreen CTest `test_runengine` 1/1；`clang-format --dry-run --Werror` 与 `git diff --check` 通过。独立 Agent `/root/slice27_contract_audit` 对最终快照复审 **PASS**，并独立运行 `test_runengine` 1/1。本轮未重跑全量 CTest，不宣称全仓通过，也未使用真实 PLC/设备。

## 12. Slice 9：PLCWrite 结果不确定状态

契约：`本地逐切片契约（未纳入提交）`，实现前经两次独立复审批准，最终实现独立静态复审 **PASS**。新增 `ExecError::SideEffectUncertain=1006`，仅复用既有 `ExecutionResult.errorCode` 与 `RunResult.errorCode` 传递，不改模块接口或结果布局。PLCWrite 在完整 FC16 请求由本地 socket 接受后，未收到有效成功回显时（含无应答、0x90 exception、无效/错配回显）向 Agent 返回 `status=uncertain`、`sideEffectStatus=uncertain` 和 `retryable=false`；请求前连接失败仍普通失败，正常回显仍成功。原 JSON 字段路径/类型保持不变。

回归包括 loopback 假服务的 timeout、exception、错 transaction、成功回显和请求前连接失败；Agent synthetic failed module 验证 `get_run_results`、`wait_for_run` 与 `get_module_output` 保留 uncertain 状态、错误码和不可重试提示。验证：关联构建目标成功；获批本地 loopback 下 `test_communicationplugins` 1/1；offscreen `test_runengine`、`test_agentactor`、`test_agentcontroller`、`test_toolschema` 4/4；格式门禁及 `git diff --check` 通过。独立 Agent `/root/slice9_final_audit` 复审 **PASS**。本片不推断 PLC 是否实际写入，未跑全量 CTest 或真实设备。

## 13. Slice 10：PLCWrite cycle 超时不重放的端到端验证

契约：`本地逐切片契约（未纳入提交）`，实现前经独立审核批准；最终集成测试复审 **PASS**。测试通过 `RunEngine::loadProject` 加载真实 `PLCWritePlugin`，以 5 ms cycle 对本机 loopback 假服务发 FC16；服务完整收取请求后不回包。等待超时并跨多个周期后，断言服务端请求数恰为 1、失败 run 与 `cycleStopped` 各一次、引擎结束 cycle/busy，RunObservation 错误码为 1006，Agent `get_run_results` 返回 `uncertain`/`retryable=false`。这补上 Slice 8 的合成失败回归与 Slice 9 的独立插件结果测试之间的真实运行链路证据，不推断 PLC 寄存器结果。

验证：获批 loopback 环境 `test_communicationplugins` 1/1；offscreen `test_runengine`、`test_agentactor`、`test_toolschema`、`test_agentcontroller` 4/4；构建、格式门禁与 `git diff --check` 通过。独立 Agent `/root/slice9_final_audit` 对最终集成快照判定 **PASS**，并确认测试路径无 Qt socket/timer 线程亲和性警告。没有运行全量 CTest 或真实设备测试。

## 14. Slice 11：全局 CommunicationManager PTY 串口收发

契约：`本地逐切片契约（未纳入提交）`，实现前经独立 Agent 批准，最终测试实现复审 **PASS**。新增 POSIX PTY loopback 测试，验证真实全局 `CommunicationManager` SerialPort 打开状态、原始二进制数据 manager→PTY 和 PTY→`dataReceived` 双向精确一致、接收分块按 configId 聚合，以及断开后状态和底层 `QSerialPort` 对象删除。测试通过 scope guard 确保失败路径先 disconnect/remove config 再关闭 PTY fd。非 POSIX 平台显式不适用；POSIX 运行失败不会被跳过。

验证：`test_communicationmanager` 目标构建成功；PTY 用例独立执行 3 passed，含 TCP 回归的本机套件在获批 loopback 环境 CTest 1/1 通过；格式门禁与 `git diff --check` 通过。独立 Agent `/root/slice9_final_audit` 对最终测试复审 **PASS**。该 PTY 测试不代表真实串口电气/设备验收；`cfmakeraw` 在精简 POSIX libc 的移植性仍需关注。未重跑全量 CTest。

## 15. Slice 12：Agent 查询已发现相机状态

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 PASS。新增 Observer 只读工具 `list_cameras`，返回 `CameraManager::knownCameras()` 的状态快照，并显式标记 `discoveryPerformed=false`；不扫描驱动、不连接设备、不调用相机接口。测试覆盖空缓存、connected/disconnected 两台相机、发现计数不变，以及连接/采集/曝光/增益状态未改变。

验证：`test_cameramanager`、`test_toolschema`、`test_agentactor`、`test_agentpermissions`、`test_agentcontroller` 定向 CTest 5/5 通过；相关构建目标成功。独立最终审核 `/root/slice12_final_review` **PASS**，并在复审阶段独立重跑 `test_cameramanager` 1/1。未跑全量 CTest 或真实设备；G5 仍为 `IN_PROGRESS`。

## 16. Slice 13：Agent 查询已连接相机能力和参数

契约：`本地逐切片契约（未纳入提交）`，独立契约审核最终 PASS。新增只读 `get_camera_details`，仅对已连接相机在正确 QObject 线程内使用 `CameraLease` 返回驱动能力及实时参数；不发现或连接设备。稳定错误覆盖缺参/空字符串、未知设备、未连接/已断连和线程亲和性错误。fake 回归逐字段比对能力与参数，并断言扫描、连接、断开、采集、触发、参数 setter 和工程状态不被查询改变。

验证：`test_cameramanager`、`test_toolschema`、`test_agentactor`、`test_agentpermissions`、`test_agentcontroller` 定向 CTest 5/5；相关目标构建成功；格式和 `git diff --check` 通过。独立最终审核 `/root/slice13_final_review` **PASS**，独立重跑上述 CTest 5/5。未运行全量 CTest 或真实设备；G5 仍为 `IN_PROGRESS`。

## 17. Slice 14：Agent 本地模块插件加载/卸载

契约：`本地逐切片契约（未纳入提交）`，独立实施前审核最终 PASS。新增 dangerous 工具 `load_module_plugin` / `unload_module_plugin`，目标限于已扫描模块插件名；拒绝任意路径、相机插件及跨类别同名。加载/卸载确认卡展示 `plugin_load:<name>` 或 `plugin_unload:<name>`。加载错误带 `pluginLoadFailed` 诊断；活动实例、Engine busy、线程不匹配均受保护。`executableActions` 暂保持空，等待后续准确覆盖当前可卸载状态。

测试包含真实本地 fixture 插件从卸载到加载、already-loaded 幂等、创建实例期间拒绝卸载、确认后卸载和重载、Schema 可用、缺库诊断、跨类别同名、相机插件拒绝、未知名、首尾空白、忙碌和线程错误；权限测试覆盖 Observer 拒绝、Advisor/Autopilot 确认前无 loader signal、批准与拒绝行为。

验证：`test_pluginmanager`、`test_agentactor`、`test_agentpermissions`、`test_agentcontroller`、`test_toolschema` 定向 CTest 5/5；相关目标构建成功。独立最终审核 `/root/slice14_final_review` **PASS**，独立重跑 5/5；未运行全量 CTest。

## 18. Slice 15：Agent 连接/断开已发现相机

契约：`本地逐切片契约（未纳入提交）`，独立实施前契约审核 PASS。新增 dangerous 工具 `connect_camera`/`disconnect_camera`，目标只能是 CameraManager 当前已知设备；设备 ID 精确绑定确认副作用。连接验证 manager 与 driver 状态，断开结果区分 Manager disconnected 与 disconnect_pending，并明确所有断开执行响应均未获得硬件物理断开确认。Manager thread 和 Engine busy 门禁优先执行。

fake 回归覆盖审批前零调用、批准一次、重复连接 no-op、lease 期间重复断开 pending 且计数不增、释放 lease 后 manager 断开、连接失败诊断、未知设备、线程错误和 Engine busy。真实相机及物理断开不在证据范围。

验证：`test_cameramanager`、`test_toolschema`、`test_agentpermissions`、`test_agentactor`、`test_agentcontroller` 定向 CTest 5/5；相关构建成功，格式与 `git diff --check` 通过。独立最终审核 `/root/slice15_final_review` **PASS**；复审确认批次预校验失败和前序工具失败后的跳过结果也包含 `hardwareDisconnectConfirmed:false`。新增 `testBatchValidationAndSkipKeepDisconnectHardwareStatus` 覆盖这些路径。未运行全量 CTest 或真实相机；G5 仍为 `IN_PROGRESS`。

## 19. Slice 16：相机发现 Agent 工具（PASS）

原契约因 `CameraManager::discoverCameras()` 持互斥锁同步调用驱动、可能阻塞 Agent/UI 并有回调重入死锁风险而暂停。修订版 2 在 `本地逐切片契约（未纳入提交）` 冻结为只调用已注册 async-capable plugin 的批次 API；契约独立审核通过后实施。新增 Observer 工具 `get_camera_discovery_targets`、需确认工具 `discover_cameras` 与 Observer 查询工具 `get_camera_discovery_operation`。Manager 批次先登记所有 child/plugin reservation 再调用插件；保持非空非法 job 到销毁的 lease；聚合 terminal 写缓存后再释放 lease 并发 settled；对 stale generation、legacy-only、错 ID、忙状态、shutdown 和重入采取 fail-closed。请求 ID 共用全局 allocator，工具结果不泄漏内部 child ID。F05 测试 ID、生成器引用和重渲染矩阵已同步。

验证：`cmake --build build-codex-agent -j2` 全量构建成功；`test_cameramanager`、`test_camera_discovery_shutdown`、`test_toolschema`、`test_agentactor`、`test_agentpermissions`、`test_agentcontroller`、`test_v4l2discovery` 定向 CTest 7/7 通过；阶段 0 F05/F09 复现用例 2/2 通过。新增回归覆盖稳定快照批准后每个 plugin 恰启动一次（并通过 Agent 工具按 canonical request ID 查询终态）、Observer 拒绝零启动、批次错线程/Engine busy 零启动、多个同步启动失败栅栏、cancelled 子结果、重复 terminal 只聚合结算一次、重复 shutdown 只向 child 请求一次取消并 drain，以及目标查询/批次不调用 legacy 和 async 扩展虚函数。`CameraManager` 对 pending discovery 记录取消请求，重复 shutdown 的后续排空不再重复调用驱动 `requestCancel()`；单独的 `test_camera_discovery_shutdown` 回归覆盖另一个 plugin 在 `startDiscovery()` 内同步重入 shutdown。能力矩阵生成器引用校验 483/483 单元格通过。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。原始日志位于 `build-codex-agent/agent-phase-5-slice-16-evidence/`，包括 build、CTest、F05/F09、矩阵生成与格式/差异门禁。最终实现独立复审 `slice16_implementation_review` **PASS**。

限制：仅当前已注册 async-capable plugin 参与发现，当前产品驱动支持证据为 V4L2 测试路径；Fake 和 Linux adapter 测试不代表真实相机或 Hikvision SDK / DirectShow COM 线程安全。legacy 驱动、注册时扫描和通用同步 Manager 入口仍可能阻塞。未覆盖 GrabImage 采集线程安全，不据此关闭 G5 或放行阶段 6/8。

## 20. Slice 17：插件清单可执行操作

契约：`本地逐切片契约（未纳入提交）`，实现前独立审核 PASS。`get_available_plugins` 现在根据扫描、二进制、加载状态、线程、Engine busy 和现有卸载保护返回当前可尝试的 `load`/`unload` 动作；`blockedActions` 给出稳定原因码。查询不试加载，不创建模块实例、不触碰设备或工程。对活动实例、生命周期未追踪、busy、线程错误、相机插件 lifecycle unsupported 和模块/相机同名均 fail-closed；实际加载仍可能因 ABI/SDK/插件初始化失败，工具输出不把“可尝试”当成功保证。

验证：`test_agentactor`、`test_pluginmanager`、`test_agentpermissions`、`test_agentcontroller`、`test_toolschema` 定向 CTest 5/5；构建成功，格式和 `git diff --check` 通过。独立最终审核 `/root/slice17_final_review` **PASS**，独立重跑 `test_agentactor` 1/1。未跑全量 CTest；G5 仍为 `IN_PROGRESS`。

## 21. Slice 18：全局通讯配置与状态只读查询

契约：`本地逐切片契约（未纳入提交）`，实现前独立审核 PASS。新增 Observer 工具 `list_communications`，返回全局 `CommunicationManager` 配置的 ID、名称、类型、连接状态；不返回 IP、端口、串口路径或扩展配置。通讯管理器状态现在会记录 Connecting、TCP Connected/Error、server Listening/Error、serial Connected/Error 和显式 Disconnected；TCP 失败后迟到的 disconnected 通知不会覆盖 Error。

验证：`test_communicationmanager`、`test_agentactor`、`test_agentpermissions`、`test_agentcontroller`、`test_toolschema` 定向 CTest 5/5；构建、格式和 `git diff --check` 通过。独立最终审核 `/root/slice18_final_review` **PASS**，并独立复跑新增状态用例和 `test_communicationmanager` 1/1。当前沙箱禁止本机 TCP bind，TCP server bind 冲突失败分支未测；TCP client closed-port、串口失败和状态信号一致性已测。ID/name 原样返回，若用户把敏感文本写入这两个字段不会被过滤。

## 22. Slice 19：异步相机发现扩展接口 fake 原型

契约：`本地逐切片契约（未纳入提交）`，经独立契约复核通过。新增可选 `ICameraDiscoveryAsync/1.0` 扩展与 `CameraDiscoveryJob`，不改现有 `ICameraPlugin` ABI；Manager 对插件线程不一致 fail-closed，job 回调强制 queued connection，按启动 requestId 绑定归属，错误/重复终态不能遗留请求和 plugin lease。payload 使用严格 JSON schema（requestId 为无损十进制字符串），插件取消仅是请求；Manager shutdown 排空 pending job 后仅发一次完成信号。GUI 标题栏关闭与文件菜单退出均进入异步排空路径。

验证：新增 fake 用例覆盖可选接口缺失、Manager 调用线程错误、plugin 与 Manager 线程不一致、single-flight、requestCancel 不篡改实际成功结果、pending unregister 拒绝、JSON rawInfo 结构拒绝、信号 requestId 错配回收、重复终态去重、shutdown 排空和二次调用只完成一次。`test_cameramanager`、`test_pluginmanager` 强制重建后 CTest 2/2 通过；构建目标成功，`git diff --check` 与格式门禁通过。独立实现最终复核 `/root/slice19_contract_review` **PASS**。在同轮 `test_mainwindow` 全套用例中 103 项通过、2 项失败：工具中文名文案断言与 `testG4FailedGuiCycleIterationDoesNotEndSession` 的 runId 期望；本切片未改这些断言/业务路径，未据此宣称该套件全通过。

本片验收时仅验证扩展和 lease 生命周期原型，不迁移 `registerCameraPlugin()` 初始同步扫描、`discoverCameras()/refreshCameras()`、V4L2/Hikvision/DirectShow 实驱、连接或采集；当时未新增 Agent 相机发现工具。该切片不证明相机完整性和 G5；无真实硬件/SDK/COM 线程安全证据，不能据此进入阶段 6 或阶段 8。

## 23. Slice 20：Linux V4L2 异步发现 worker

契约：`本地逐切片契约（未纳入提交）`，独立契约及实现复核均 **PASS**。`V4L2CameraPlugin` 新增 Slice 19 可选 async 接口；`IV4L2DiscoverySystem` 把 `/dev` 路径枚举和 stat/open/QUERYCAP/G_FMT/close 抽成值类型 adapter，测试 fake 返回固定路径，不依赖宿主机设备。worker 在受管 `QThread` 执行，不访问 plugin/Manager/job QObject；只把 JSON bytes queued 回 job 线程，并在 fd 已关闭且 worker 退出后发布终态。取消令牌在设备边界、stat/open 后及两次 ioctl 之间检查；阻塞中的系统调用仍可能让 request/shutdown pending。V4L2 plugin/test CMake target 限定 Linux。

验证：`test_v4l2discovery` 覆盖 success、多设备/无有效设备、stat/open/capability/format失败、异常、open 阶段取消、QUERYCAP 阶段取消并跳过 G_FMT、fd 清理与工作线程身份；和 `test_cameramanager`、`test_pluginmanager` 定向 CTest 3/3 通过。格式门禁与 `git diff --check` 通过。独立实现审核 `/root/slice19_contract_review` **PASS**。

本片验收时未迁移 `registerCameraPlugin()` 初始同步探测、`discoverCameras()/refreshCameras()`、CameraSetView 或 Agent 工具，也没有把 V4L2 worker 集成到 CameraManager plugin lease/shutdown 路径；后者由 Slice 19 fake 插件覆盖。Hikvision 与 DirectShow 未加 async 支持；没有真实设备/USB拔插验证。G5 仍 IN_PROGRESS。

## 24. Slice 21：CameraManager 与 CameraSetView 接入 async discovery

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约复审通过。本片保留 `ICameraPlugin` ABI；async 插件注册时不调用同步 availability/discovery，Manager 自动启动初次扫描。Manager discover/refresh 只对 legacy 插件走同步路径；worker discover 只读缓存，worker refresh/disconnect 排队；async connect 用已验证的 `CameraInfo` 缓存。对 async 重复 deviceId 使用 pluginId 字典序仲裁，live camera/lease 锁定原 owner；设备在 live 时从结果消失会保留缓存和真实连接状态，释放后下一次空扫描清理。线程外 connect/acquire fail-closed。启动器调用失败和所有已接受请求终态均发单次 `cameraDiscoverySettled`；纯预校验拒绝不接受请求且不发 settled。CameraSetView 显示聚合 loading、响应 cache/list 和连接状态更新，lease 延迟断开时不提前显示 Disconnected。

回归新增/扩展覆盖自动初次扫描无同步调用、worker discover/refresh、async cache connect、线程外 connect/acquire/disconnect、两种完成顺序的重复设备仲裁、active lease 缺失后的延迟清理、success/error/cancelled/start_failed/重复终态/shutdown 的 settled 次数，以及双插件 UI 首次加载与刷新时“首个完成仍 loading、最后完成收起”。`test_cameramanager`、`test_v4l2discovery` 定向 CTest 2/2 通过；三目标 `test_cameramanager test_v4l2discovery test_mainwindow` 均构建成功；格式门禁与 `git diff --check` 通过。最终 `test_mainwindow -silent` 为 103 passed、2 failed：既有工具中文文案断言（`test_mainwindow.cpp:5878`）和 G4 failed GUI cycle runId 期望（`test_mainwindow.cpp:7299`）；与本片相机路径无关，未修改相关断言。独立实现复审 `/root/slice19_contract_review` **PASS**，独立重跑 `test_cameramanager` 与 `test_v4l2discovery` CTest 2/2。

本片不新增 Agent discovery 工具、不关闭 Slice 16/G5，不声称连接/采集已异步化，也未做真实硬件/SDK/COM/拔插验证。V4L2 之外的相机驱动仍走既有 legacy 路径，Slice 21 不据 fake 插件结果宣称它们具备异步发现能力。

## 25. Slice 22：相机专属线程与异步连接生命周期

契约：`本地逐切片契约（未纳入提交）`，实现前设计复审 PASS，最终实现复审 PASS。异步连接为每台相机建立独立 QObject/QThread session；连接、状态确认、断开、停止采集和析构均在相机线程完成。Manager 通过缓存的 `CameraInfo` 调用快速 camera factory；拒绝 parented、非 ICamera、错误 affinity，以及复用 plugin/Manager QObject 的返回值。非法对象以 affinity-safe `deleteLater()` 清理，并在销毁回调前保留 pending 请求和插件卸载保护。连接失败状态不确定或断开未确认时保留 Error session，允许显式 disconnect 重试；成功清理后等待 QObject 析构与线程退出再 settle。shutdown 关闭新请求、取消 discovery、取消 pending connect 并排空全部新 session。非 Manager 线程 `disconnectAll()` 整体排队回 Manager 线程；managed discovery ID 分配避开 camera operation request ID。

回归覆盖：父对象/错误 affinity factory 清理、connect 返回 false、connect/isConnected 异常导致 Error session、disconnect 错误保留与重试、同设备 single-flight、连接期间 Manager event loop 响应、线程外 disconnectAll、cache/session getter 与 lease fail-closed、operation 状态与线程身份、连接中的 generation 因 MainWindow close 失效后在相机线程断开并销毁、session-only window close 等待 drain。独立 `test_camera_shutdown` 检查空 shutdown 一次性完成并锁 admission；`test_camera_discovery_shutdown` 检查取消与 job 销毁前 shutdown 不提前完成。

验证：`test_cameramanager`、`test_camera_shutdown`、`test_camera_discovery_shutdown`、`test_v4l2discovery` 独立 CTest 4/4 通过；相关目标强制重建成功，clang-format 与 `git diff --check` 通过。独立实现复审 `/root/slice19_contract_review` **PASS**。`test_mainwindow` 本轮 103 项通过、2 项失败，仍为既有工具中文标签断言和 G4 failed GUI cycle runId 断言；Slice 22 的 session-only MainWindow 关闭回归通过。

限制：本片没有迁移 Agent、CameraSetView、GrabImage 或同步 `connectCamera()` 调用方；旧连接路径仍可能阻塞，不能据此宣称 G5 完成。没有真实相机/SDK/COM 验证。CameraManager 静态析构仍只能调用同步 legacy `disconnectAll()`；应用退出必须继续经过 MainWindow 关闭排空，当前源码未发现绕过入口。Slice 16 及阶段 6–8 的门禁未通过。

## 26. Slice 23：Agent 相机连接/断开异步请求

契约：`本地逐切片契约（未纳入提交）`，实现前契约审核与实现后独立审核均 **PASS**。Agent `connect_camera` / `disconnect_camera` 对支持 async session 的缓存设备使用 CameraManager 分配并提交请求的入口，立即回 `{status:"pending",requestId}`；Observer 新增 `get_camera_operation`，只接受规范无损十进制字符串并返回 pending/终态历史。Manager 保留最多 256 条终态记录，按 settle 插入顺序 FIFO 淘汰，且在发 `cameraOperationSettled` 前写入查询历史；发现和相机操作使用同一单调 ID allocator。

Observer malformed 写调用先于参数和设备校验返回 `PERMISSION_DENIED`。Agent 在已确认连接/断开的幂等短路前检查相反方向是否有在途操作，均以 `CAMERA_OPERATION_BUSY` 拒绝；disconnect Error session 可用新 ID 重试。旧同步会话（包括 async-discovery 插件通过旧路径创建的实例）fail closed 为 `CAMERA_SESSION_REQUIRED`。disconnect 查询的 `hardwareDisconnectConfirmed` 始终为 false。Controller 确认执行只提交一次，pending 审计为 pending 且 undoable=false。

验证：定向重建 `test_cameramanager`、`test_agentcontroller`、`test_agentpermissions`、`test_agentactor`、`test_toolschema`；CTest 5/5 通过。相机 Manager 测试含两个方向的 gated opposite request、disconnect error/retry、legacy async-capable 插件同步会话、Agent 查询 pending/success/error/cancelled、257 个终态请求（慢请求最后完成）验证按 settle FIFO 淘汰、信号槽内同步查询，以及 Autopilot 批准后恰好一次请求和 pending/undoable=false 审计。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。独立实现复审 `/root/slice19_contract_review` **PASS**。

限制：Slice 23 仅迁移 Agent 相机连接/断开请求；CameraSetView、get_camera_details、GrabImage、参数、触发、图像采集及其 legacy 同步线程安全仍未收口。V4L2 以外驱动的 async discovery/connection 实际适配情况及真实设备、SDK、COM 线程约束仍未验证。G5 继续 IN_PROGRESS；当时 Slice 16 尚未实施，阶段 6–8 未放行。

## 27. Slice 24：CameraSetView 异步连接状态

契约：`本地逐切片契约（未纳入提交）`，实现前契约审核 PASS；首轮实现审核提出三项问题后全部修复，最终独立复审 `/root/slice19_contract_review` **PASS**。CameraSetView 对 async-capable session 走异步 connect/disconnect，按 Manager pending 快照及缓存状态渲染，可恢复 Agent 已启动的操作；pending 时禁用两个按钮，失败/取消显示稳定错误码，Error session 只开放断开重试。已修复 UI 按钮状态查询误触 legacy camera `isAcquiring()`/`isConnected()` 的问题；disconnect started 信号在 worker 清理动作入队前发出；Error session 重试获 Manager 接受后清除旧错误。

验证：`test_cameramanager` 全套 CTest 1/1 通过（包含 legacy camera 状态虚函数零调用、started 先于 worker disconnect entry、disconnectAll/shutdown 路径时序、错误重试 pending tooltip 清除）；Agent/权限/schema/controller 相关定向 CTest 在 Slice 24 同批目标此前 4/4 通过。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。`test_mainwindow` 此工作区仍有此前记录的两项断言失败：工具中文标签实际“圆检测”与测试预期“找圆”，以及 G4 failed-cycle runId；不归因于 Slice 24。

限制：本片仅迁移 CameraSetView 对 async-capable session 的连接/断开；legacy plugin/实例仍保留同步 fallback，可能阻塞。未迁移 `get_camera_details`、GrabImage、设置/回读参数、触发与采集，不涉及真实相机、厂商 SDK、COM 或现场拔插。G5 IN_PROGRESS，不放行阶段 6 或阶段 8。

## 28. Slice 25：async session 相机实时详情查询

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约复审 PASS；实现后独立复审 `/root/slice19_contract_review` **PASS**。`get_camera_details` 对已连接 async session 在 camera QObject worker 线程串行读取，立即返回 pending；`get_camera_operation` 返回完整 details JSON 或稳定错误。断连时先停读并返回 `CAMERA_NOT_CONNECTED`；getter 异常、非有限参数各有稳定错误且无半成品 details。详情查询使用既有 pending/request ID/256 终态 FIFO，专用终态分支不改 CameraStatus、lastError、连接信号或 generation。查询期间同设备生命周期操作互斥，CameraSetView 显示 Reading details 并禁用连接按钮；查询完成后恢复 Manager 缓存状态。已接受查询会在 shutdown/disconnectAll 中 drain，之后自动继续 session disconnect sweep。

验证：全量构建 `cmake --build build-codex-agent -j2` 成功；`test_cameramanager` 与 AgentActor/ToolSchema/Permissions/Controller 定向 CTest 共 5/5 通过，独立实现复审目标构建及复跑亦通过。新增 fake camera 用例覆盖 worker 线程归属、event loop 响应、重复详情请求拒绝、同设备断开忙拒绝、完整 payload、结果先于 settled 可查、物理断连不读取其余字段、getter throw、非有限参数、无生命周期状态/信号变化、disconnectAll 和 MainWindow shutdown drain。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。当前 `test_mainwindow` 为 103 passed、2 failed，两个失败仍是工具中文按钮文案“圆检测/找圆”断言及 G4 failed-cycle runId 断言；Slice 25 未改这些行为。

限制：legacy camera 仍沿用 Slice 13 同步详情查询；getter 若永久阻塞，Manager shutdown 会等待 camera worker，不能安全强杀。未实现参数写入、采集/停止或 GrabImage async 化；无真实相机、SDK、COM 和现场拔插证据。G5 IN_PROGRESS，阶段 6–8 仍未放行。


## 29. Slice 26：相机参数单项设置与回读

契约：`本地逐切片契约（未纳入提交）`，实现前契约复审及实现后独立复审 `/root/slice19_contract_review` 均 **PASS**。新增危险工具 `set_camera_parameter`，每次只修改一个参数；Agent 确认摘要包含设备、序列号、generation、参数名和值。可选 `ICameraParameterControl` 扩展按稳定 IID 查询具体驱动的范围验证，不改变旧插件加载 ABI；没有该扩展的旧插件明确拒写。统一 helper 按能力映射检查、驱动范围验证、读取 before、调用一个 setter 并回读，区分拒绝前失败、读回 mismatch 与写后不确定结果，不自动重试。

绑定 generation 由共享的 `CameraParameterBinding` 管理。每设备互斥锁把 legacy/async Agent 写入、worker 线程复核及 GrabImage 直接 setter 前的失效通知串行化；async worker 在同一临界区内核对批准时的 generation 并完成验证/写入/回读。实现复审指出的“请求已 pending 后版本变化、旧批准仍写入”缺口已用确定性回归封住：在 `cameraOperationStarted` 同步递增版本，终态为 `CAMERA_TARGET_CHANGED`，setter 计数保持不变。GrabImage 的第二个 setter 抛异常也有绑定失效回归。

测试覆盖 legacy 与 async 写入、Observer 拒绝、批准前零 setter、五类参数成功、接口缺失、能力/范围/类型/身份/generation 拒绝、验证和 setter/readback 异常、clamp mismatch、设备批准期间断连、single-flight、event-loop 响应、pending UI、GrabImage 部分参数修改、旧版本 pending 竞态，以及阶段 0 F05/F09 工具清单随已实现能力更新。

验证：全量构建 `cmake --build build-codex-agent -j2` 成功；`test_cameramanager`、`test_agentactor`、`test_agentpermissions`、`test_agentcontroller`、`test_toolschema`、`test_grabimage`、`test_pluginparametercontracts` 定向 CTest 7/7 通过；阶段 0 F05/F09 两个 QtTest 用例单独 2/2 通过；格式门禁与 `git diff --check` 通过。独立实现复审确认并发修复、字段/映射/副作用语义，无阻断项；queue rejection 终态字段已有统一补齐，但没有专门的 invoke-queue rejection 注入测试。

本轮初次受限沙箱全量 CTest 为 75/82；随后在获准的本机 loopback 环境于 F05/F09 更新后重跑全量 CTest，81/82 通过。TCP/Unix socket、SAM 与 `test_agentphase0_repro` loopback 用例全部通过。唯一失败仍是既有 `test_mainwindow` 的两项断言：FindCircle 显示名“圆检测/找圆”（`tests/test_mainwindow.cpp:5878`）和 G4 failed-cycle runId（`tests/test_mainwindow.cpp:7299`），与 Slice 26 无关。没有真实相机或厂商 SDK 证据，不据此声称硬件参数范围通过。

限制：legacy 相机路径仍只允许在 Manager 所在线程同步短调用；异步支持取决于 plugin async session。GrabImage 本身仍不属于本片的 async 化范围。Slice 16 虽已 PASS，但 G5 仍 IN_PROGRESS，阶段 6 与阶段 8 未放行。


## 30. Slice 27：插件 ABI 拒绝与重载 Schema 缓存回归

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 PASS。新增两个仅供测试的 Qt 插件 fixture，并注册独立 `test_agent_plugin_g5` 进程，生产代码零改动。ABI fixture 可由 Qt 实例化但未实现 `IModule`；经 Agent `load_module_plugin` 后返回 `PLUGIN_LOAD_FAILED`，详情为 ABI v2 interface cast 失败，`pluginLoadFailed` 恰一次、`pluginLoaded` 零次，Manager 最终保持未加载。另一有效 ABI v2 fixture 使用稳定 pluginId；同一个 AgentActor 先取得 Schema v1，PluginManager 卸载后切换 fixture 环境版本并重载，Schema 返回 v2 新字段且移除旧字段，验证 load/unload 信号使真实缓存失效。

隔离：测试在首次创建 PluginManager 前把 `DEEPLUX_APP_DATA_DIR` 指向临时目录。日志和断言确认只扫描两个 fixture、没有相机或默认/用户插件；测试结束先 shutdown 并断言两者均已卸载，再由 RAII 恢复环境变量并销毁临时目录。独立实现复审 `/root/slice16_implementation_review` 判定 **PASS，无阻断**。

验证证据位于 `build-codex-agent/agent-phase-5-slice-27-evidence/`：全量构建 exit 0；`test_agent_plugin_g5`、`test_pluginmanager`、`test_agentactor` 定向 CTest 3/3 通过；fixture 独立 offscreen 执行 4/4；格式门禁、`git diff --check` 通过；源码 SHA-256 清单 8/8 校验通过，并记录两个 fixture 库及测试程序 SHA-256。一次未设置 `QT_QPA_PLATFORM` 的手工启动因当前会话无 X display 而在 Qt 平台初始化失败，原始日志保留；按 CTest 的 offscreen 环境重跑成功，不是产品测试失败。

限制：该合成 fixture 只证明当前 ABI cast 拒绝路径和 Agent Schema 生命周期缓存失效；不证明第三方 ABI、厂商 SDK/符号兼容，也不覆盖真实模块 Schema。仅关闭 G5 标准 2 的两个软件证据缺口；G5 仍 IN_PROGRESS，阶段 6/8 不放行。


## 31. Slice 28：IModule interfaceVersion 不匹配拒绝

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核及实现后复审 `/root/slice16_implementation_review` 均 **PASS**。新增 cast 成功但 `interfaceVersion()` 返回 1 的测试插件；通过 Agent `load_module_plugin` 触发 `PLUGIN_LOAD_FAILED`，日志诊断为 `expected=2, actual=1`，证明拒绝来自版本号检查而不是 Qt 实例化或接口 cast。失败插件 `pluginLoadFailed` 恰一次、`pluginLoaded` 零次且状态未加载；随后有效 Schema fixture 经 Agent 返回 `loaded`，信号恰一次，显式卸载后状态清空。QtTest 顺序固定为版本拒绝和有效加载在 Schema v1→v2 回归之前，证明失败路径之后缓存测试仍正常。

验证证据位于 `build-codex-agent/agent-phase-5-slice-28-evidence/`：全量构建 exit 0；`test_agent_plugin_g5`、`test_pluginmanager`、`test_agentactor`、`test_plugincontracts` 定向 CTest 4/4；独立 fixture 输出 5/5；临时扫描恰为三个 fixture，shutdown 后全卸载；格式与 `git diff --check` 通过；源码 SHA-256 11/11 校验通过，二进制清单含三个动态库和测试可执行文件。

限制：只关闭 G5 标准 2 中接口版本号拒绝分支，并与 Slice 14/27 的缺库、cast 拒绝、加载失败、使用中卸载和 Schema 重载证据合并后关闭标准 2。仍不证明第三方 ABI/厂商 SDK/跨平台兼容；G5 其余标准未完，不放行阶段 6 或阶段 8。

## 32. Slice 29：Agent 异步相机 Session 到 GrabImage 的安全采集

契约：`本地逐切片契约（未纳入提交）`。实现前独立契约审核及迟到帧隔离补充审核 `/root/slice16_implementation_review` 均 **PASS**；实现复审初次指出触发前可能误收旧帧，修复后复审 **PASS，无阻断**。CameraOperationSession 在专属 worker 执行相机参数预检/写读、start/trigger、stop/disconnect；Manager API 只返回 `QImage`/状态值。`imageReceived` 没有请求号，因此每次 capture 建立绑定 requestId 和 generation 的直接连接，回调只复制到共享请求帧状态；帧接收门在参数预检/写入和 `startAcquisition()` 期间关闭，只在软件触发前打开；fake 在 start 时发出的延迟旧帧被丢弃。驱动还必须保证软件触发模式下本次触发前不会主动推送非请求帧，否则须增加请求关联协议。capture 结束后退役该 session，迟到帧不会被后续请求认领。GrabImage 同时支持 Manager/UI 局部事件等待和非 Manager worker 条件等待；取消、超时后保留 session，直到 worker 完成合作清理。shutdown 会取消 capture 并在相机对象销毁、worker 退出、session 移除后唯一发出完成信号。

独立测试 `test_agent_camera_lifecycle_g5` 在临时 app-data 目录创建 fake async discovery/plugin，真实经过 Agent discovery/connect/details、AgentController Autopilot 确认后的参数写入/读回，再由 GrabImage 对同一 session 采集；相机每个虚调用检查线程身份。覆盖完整像素/尺寸、start 阶段延迟旧帧隔离、验证器异常（`CAMERA_PARAMETER_VALIDATE_FAILED` 且 setter/trigger 零调用）、参数第二项越界时零 setter/trigger、后续 setter 不确定及读回不匹配、trigger 失败、无帧超时、Engine token 合作取消、Manager/UI 与 worker 两种等待、Engine 忙时嵌套 step/run 拒绝、超时中断开 busy、请求 A 超时且迟到帧后 drain/重连请求 B、pending capture shutdown 和唯一 shutdownCompleted。成功/失败结束都退役 capture session，后续采集需重新连接。

验证：全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agent_camera_lifecycle_g5`、`test_cameramanager`、`test_grabimage`、`test_camera_shutdown`、`test_camera_discovery_shutdown`、`test_runengine` 6/6 通过；格式门禁和 `git diff --check` 通过。逐项构建/测试原始日志、源码 SHA-256、二进制清单位于 `build-codex-agent/agent-phase-5-slice-29-evidence/`。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。

补充复审关闭项：首轮实现复审指出触发前接收门开放的旧帧竞争；修订后审核确认接收门关闭至软件触发前，`startAcquisition()` 延迟旧帧被测试丢弃，且验证器异常映射与零副作用断言正确。独立复审只读检查，未替代本节列出的构建和测试证据。

限制：只关闭 G5 标准 3 的模拟相机到 GrabImage 本地软件集成部分。没有真实相机、V4L2/Hikvision/DirectShow SDK、物理断连或现场图像质量证据；驱动须满足软触发前不主动推帧且 disconnect 完成后不再回调旧帧的契约。capture 按当前无 request-aware 帧 ID 的 ICamera 信号设计为一次性 session，重复采集需重新连接。G5 标准 1、5–7 和阶段 8 硬件门槛仍未完成；标准 4 已由 Slice 6/11 本地模拟 TCP/PLC 与 POSIX PTY 证据闭合，不据本片放行阶段 6 或阶段 8。

## 33. Slice 30：暂停流程恢复确认与断点代际绑定

契约：`本地逐切片契约（未纳入提交）`，独立契约复审 `/root/slice19_contract_review` **CONTRACT_PASS**。`resume_flow` 纳入 Autopilot 执行确认；确认摘要包含通用恢复副作用，并按工程配置列出 Folder Delete 路径及 PLCWrite 的 host/port/slave/address。Controller 保存暂停时的 runId、sessionId、Paused 状态和 lifecycle generation（pause epoch），确认前复核，Actor 将同一快照交给 RunEngine；Engine 在生命周期锁内原子校验后才恢复。工程/目标漂移或暂停点变化会失效并零恢复副作用；旧批准不可能恢复同一 RunOnce 的后续断点。

测试：覆盖 Observer 拒绝、Advisor 待确认/拒绝、Autopilot 确认/拒绝、Folder 目标变化、Folder 与 PLCWrite 精确目标摘要、确认批准后恰好恢复一次、停止后的迟到确认，以及同一 RunOnce 从断点 A 到 B 后批准 A 的旧确认必须记 `expired`、保持 B 暂停且 B 模块调用数为 0。RunEngine 单测还直接证明 A 的快照不能原子恢复 B。`CONFIRMATION_EXPIRED` 在聊天批次与 Bridge 确认审计中统一映射为 `expired`。

验证证据位于 `build-codex-agent/agent-phase-5-slice-30-evidence/`：全量构建 exit 0；Slice30 定向 CTest 6/6；全量 CTest 83/84。唯一失败目标为长期已知的 `test_mainwindow`，其中两个既有断言失败（FindCircle 文案“圆检测/找圆”；G4 failed-cycle runId），该测试其余 103 项通过；其余 83 个 CTest 通过。`clang-format --dry-run --Werror`、`git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。

限制：该片只关闭 Autopilot `resume_flow` 确认与会话/断点绑定子路径，不关闭 G5 标准 5 或 6 整体。`run_flow`、`step_once` 对已配置插件副作用的逐路径实际调用计数、直接写工具逐类验收、目标超时结果不确定时禁止重复动作仍待后续切片；真实硬件仍由阶段 8 验证。全量 CTest 的两个 MainWindow 断言失败不是本片回归，也没有在本片中改写其既有语义。

## 34. Slice 31：Folder Delete 经 run_flow 与 step_once 的真实副作用计数

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。新增独立进程 `test_agent_folder_execution_g5`，先将 `DEEPLUX_APP_DATA_DIR` 指向临时目录，再把构建出的真实 Folder 插件库和 metadata 安装到该隔离插件根；断言 PluginManager 仅扫描 Folder、没有相机插件，不读取用户插件目录。

同一个真实 Folder Delete 实例分别经 `run_flow(mode=once)` 和 `step_once` 执行到两个临时目录。两条路径在待确认和拒绝后目标/探测文件保持存在、模块 `started/finished` 都为 0、Engine 未运行；批准后目标确实删除，按精确 instanceId `started/finished` 均恰为 1 且成功。测试在批准前连接异步 runFinished，并将其 `RunResult.runId` 与工具接受结果的 runId 匹配；每个批准调用恰有一条成功审计。失败清理若 Engine 仍 busy 会保留临时 app-data 和目标目录、跳过卸载；只有 Engine idle 后才清空模块、关闭工程并 shutdown PluginManager。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。

验证证据位于 `build-codex-agent/agent-phase-5-slice-31-evidence/`：全量构建 exit 0；新测试 CTest 1/1；全量 CTest 84/85，唯一失败仍为长期已知 `test_mainwindow` 的两个旧断言（FindCircle 文案“圆检测/找圆”及 G4 failed-cycle runId），`test_mainwindow` 其余 103 项通过；未发现新增失败。格式门禁、`git diff --check` 和源码/二进制 SHA-256 校验通过。

限制：只证明 Folder Delete 经 run_flow / step_once 的临时目录本地软件闭环；不代表 PLCWrite 或其他插件、直接写工具、变量解析目标、timeout 不确定状态和重试策略完成，不关闭 G5 标准 5 或 6。真实硬件仍属阶段 8。

## 35. Slice 32：PLCWrite 经 run_flow 与 step_once 的确认报文及线程亲和

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。审查发现 PLCWrite 原成员 `QTcpSocket` 在模块创建线程构造，但 `run_flow` 会由 RunEngine 工作线程执行。现改为每次 `process()` 在调用线程创建、连接、读写并销毁局部 socket，连接及响应等待使用同线程 event loop/timer；FC16 帧和回显校验、timeout 配置、无自动重试及写请求接受后无有效回显时的 `SideEffectUncertain` 保持不变。

新增隔离进程 `test_agent_plcwrite_execution_g5`：临时 app-data 只安装真实 PLCWrite 插件；Fake Modbus/TCP peer 仅监听 loopback 临时端口，解析完整 MBAP 帧并返回 FC16 echo。`run_flow(mode=once)` 与 `step_once` 分别验证待确认及拒绝后请求数/插件调用数为 0，批准后各恰有 1 个有效 FC16 报文、一次成功模块调用和与工具 runId 匹配的成功 RunResult。两种执行线程均通过真实动态插件完成收发。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。

验证证据位于 `build-codex-agent/agent-phase-5-slice-32-evidence/`：全量构建 exit 0；`test_agent_plcwrite_execution_g5`、`test_communicationplugins`、`test_agent_folder_execution_g5` 全量 CTest 中均通过；全量 CTest 85/86。唯一失败仍为已记录的 `test_mainwindow` 两个旧断言，测试内 103 项通过。格式门禁、`git diff --check` 及源码/插件/测试二进制 SHA-256 校验通过。

限制：仅覆盖 PLCWrite 的本地 FC16 run_flow/step_once 成功路径和 socket 线程归属。超时、断线等 SideEffectUncertain 各错误分支仍由既有通讯插件测试覆盖，本片不将其升级为 G5 标准 6 的完整端到端确认/不重试通过；未覆盖 PLCRead、串口、生产网络、真实 PLC 或其他插件。G5 标准 5/6 仍未完成，真实硬件仍归阶段 8。

## 36. Slice 33：PLCWrite 配置漂移失效与不确定结果不重放

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。同一真实 PLCWrite 隔离测试新增两个场景：

- 待确认 `run_flow` 的旧 confirmationId 在工程中将 PLC 端口改到另一 loopback peer 后被批准，Controller 记录 `expired` 并发出失效提示；Engine 状态保持不变，模块 started/finished 为 0，旧、新 peer 均为 0 请求。
- 另一次批准把 FC16 请求交给不回 echo 的 loopback peer。peer 收到恰好一条有效写请求后，Engine 返回 `SideEffectUncertain`；Agent `get_run_results` 对工具返回的 runId 报 `status=uncertain`、`sideEffectStatus=uncertain`、`retryable=false`。运行结束后请求数仍是 1，无自动重试。

独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。证据位于 `build-codex-agent/agent-phase-5-slice-33-evidence/`：全量构建 exit 0；PLCWrite Agent 测试与既有 `test_communicationplugins` 定向 2/2；全量 CTest 85/86。唯一失败仍为已记录的 `test_mainwindow` 两个旧断言，测试内其余 103 项通过。格式、`git diff --check` 与 SHA-256 清单通过。

限制：这只闭合 PLCWrite run_flow 下已绑定配置变化及“写请求发出但无成功 echo”的 Agent 不确定/不重试子路径，不说明真实 PLC 最终是否提交数据，也不覆盖其他插件、真实设备、生产网络、所有直接写工具或所有确认目标。不关闭 G5 标准 6 整体，阶段 8 硬件门槛仍未完成。

## 37. Slice 34：resume_flow 恢复时真实 Folder Delete 副作用计数

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约复审 `/root/slice19_contract_review` **CONTRACT_PASS**；实现后独立复审 `/root/slice16_implementation_review` **PASS，无阻断**。新增用例复用 `test_agent_folder_execution_g5` 的隔离 app-data 和真实 Folder 动态插件：Delete 实例在 RunOnce 中设置断点，实际 `runOnce` 到断点后进入 Paused。测试在暂停期间读取运行观测中的 runId，先拒绝 `resume_flow`：探针文件仍存在，Folder started/finished 及 `runFinished` 仍为 0。随后对新 confirmationId 批准恢复，目标目录确实删除，Folder started/finished 恰为 1 且成功，唯一成功 RunResult 使用暂停前同一 runId，审计为 `resume_flow=success`，Engine 回到 Idle。异常 busy teardown 保留临时插件和目标树。

验证证据位于 `build-codex-agent/agent-phase-5-slice-34-evidence/`：Slice34 定向 CTest 1/1；全量构建 exit 0；获准本机 loopback 的全量 CTest 85/86，唯一失败为既有 `test_mainwindow` 中 FindCircle 显示文案与 G4 failed-cycle runId 两个断言（其余 103 项通过）；全量 CTest 的其余 85 个目标通过。格式门禁、`git diff --check` 和 SHA-256 清单通过。受限沙箱全量运行另有 AF_UNIX/TCP/SAM 测试因 listen 被拒而失败，故不作为产品回归结论；原始日志保留。

限制：本片只证明一个真实 Folder Delete 模块经 Autopilot `resume_flow` 恢复后的临时目录副作用计数，不覆盖 PLCWrite 经 resume 的实际报文、其他模块/插件组合或目标硬件。不关闭 G5 标准 5/6，阶段 6 与阶段 8 仍未放行。

## 38. Slice 35：resume_flow 恢复时真实 PLCWrite FC16 报文计数

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约复审 `/root/slice19_contract_review` **CONTRACT_PASS**；实现后独立复审 `/root/slice16_implementation_review` **PASS，无阻断**。在隔离进程的真实 PLCWrite 插件测试中，单 PLCWrite 实例于 RunOnce 断点暂停。暂停时读取运行观测的 runId；拒绝 `resume_flow` 后 Engine 仍 Paused、peer 请求数为 0、模块 started/finished 与 runFinished 均为 0。批准新的 confirmationId 后恢复同一 runId，loopback peer 收到并回显恰一条合法 FC16，字段核对 unit=7、address=4660、数量=1、值=0x1234；模块 started/finished 恰为 1 且成功，RunResult 唯一且成功并匹配旧 runId，审计为 `resume_flow=success`，Engine idle。

验证证据位于 `build-codex-agent/agent-phase-5-slice-35-evidence/`：Slice35 loopback 定向 CTest 1/1；全量构建 exit 0；获准本机 loopback 的全量 CTest 85/86，唯一失败仍为 `test_mainwindow` 的两个既有断言，其他 85 个目标通过；格式门禁、`git diff --check` 与 SHA-256 校验通过。受限环境下该 loopback 测试因监听被拒无法启动，失败日志保留并不作为产品回归结论。

限制：本片只证明 PLCWrite 单次 FC16 经 Autopilot `resume_flow` 的 loopback 软件闭环，不证明真实 PLC 状态，不覆盖其他通讯协议、循环模式、多模块编排或目标超时语义；G5 标准 5/6 和阶段 8 硬件门槛继续未完成。

## 39. Slice 36：直接 save_project 的确认门与工程文件写入

契约：`本地逐切片契约（未纳入提交）`，经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。`test_agentcontroller` 新用例调用 AgentController 的真实 direct tool 路径：测试进程在 `ProjectManager` 首次初始化前把 QSettings User/System INI 路径隔离到临时目录。pending 时确认载荷准确列出 `file_write:<目标>`，目标文件、Project filePath、recent-projects 缓存和设置持久值均不变；拒绝后这些值仍不变、`projectSaved` 零次、审计 rejected/undoable=false 且没有 undo barrier。批准新 confirmationId 后 `projectSaved` 恰一次，JSON 中工程 ID/名称正确，当前 Project 的 filePath 指向目标，隔离 recent-projects 持久值包含目标，审计 success/undoable=false。

验证证据位于 `build-codex-agent/agent-phase-5-slice-36-evidence/`：`test_agentcontroller` 定向 1/1；全量构建 exit 0；全量 CTest 在获准 loopback 环境下 85/86，唯一失败仍为已记录的 `test_mainwindow` 两个旧断言、其余 103 项测试通过；其余 85 个 CTest 目标通过。格式门禁、`git diff --check` 和源码/可执行文件 SHA-256 校验通过。

限制：本片只验证直接 `save_project` 对临时工程文件的确认/拒绝/批准闭环，不代表 SaveImage、SaveData、WriteText 等流程输出插件逐项内容和失败语义已闭合；也不覆盖相机参数之外其他 direct device 操作或物理设备。不关闭 G5 标准 5/6 或放行阶段 6/8。

## 40. Slice 37：相机 direct connect/disconnect 确认与 fake 驱动调用计数

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；实现后独立复审 `/root/slice16_implementation_review` **PASS，无阻断**。现有 `test_agent_camera_lifecycle_g5` 在已终态 discovery 的 fake async 相机上，通过 AgentController Autopilot 对 connect/disconnect 分别走 pending、拒绝和新批次批准。确认载荷绑定精确 deviceId；拒绝审计为 `rejected`、fake 驱动调用不增、CameraManager session 状态保持不变；批准批次 ID 不复用拒绝 ID，Agent 审计以 `pending` 准确表示异步请求已入队，再按 `cameraOperationStarted` 的 requestId 轮询到操作 `success`。批准连接/断开分别只调用 fake driver 一次，并将 session/connected 状态变更到预期终态。既有设备线程身份检查、busy-drain 和安全 teardown 保留。

验证证据位于 `build-codex-agent/agent-phase-5-slice-37-evidence/`：`test_agent_camera_lifecycle_g5` 定向 CTest 1/1；全量构建 exit 0；全量 CTest 85/86，唯一失败仍为 `test_mainwindow` 的两个既有断言，其他 103 个该套件断言通过且其余 85 个目标通过；格式门禁、`git diff --check` 与源码/二进制 SHA-256 校验通过。

限制：本片只验证 fake async camera 的 Agent connect/disconnect 确认门和调用计数，不是物理相机、V4L2/Hikvision/DirectShow SDK 或拔插证据。不关闭 G5 标准 3/5/6，阶段 6/8 仍未放行。

## 41. Slice 38：能力矩阵清单、原因码与实际执行证据刷新

契约：`本地逐切片契约（未纳入提交）`，实现前由 `/root/slice19_contract_review` **CONTRACT_PASS**。生成器在本阶段构建和独立 app-data 插件树上重跑 `DeepLux list-plugins`；清单审计结果为 69 项（66 模块、3 相机）、相对前次清单无新增或移除。未构建项逐项具有 `PLATFORM_UNSUPPORTED` 或 `BUILD_OPTION_DISABLED` 稳定原因码和具体说明；运行时失败沿用构建原因或使用 `RUNTIME_NOT_REPORTED`/`RUNTIME_LOAD_FAILED`，所有 483 个维度单元格的测试与证据引用经生成器校验。Markdown 由 JSON 自动重生，并展示原因码。

矩阵只将 Folder 与 PLCWrite 的执行和权限状态更新为 Slice31/34、Slice32/33/35 的真实动态插件证据：分别覆盖 `run_flow`、`step_once`、`resume_flow`，确认拒绝前副作用计数为零，批准后仅一次。V4L2 执行单元只刷新探针证据路径，没有提升状态。SaveImage、SaveData、WriteText、PLCRead、TCP、串口及其他副作用插件仍为 NOT_TESTED/BLOCKED，没有从代表性用例外推。独立实现复审 `/root/slice16_implementation_review` 最终结论 **PASS**；审核接受已披露的旧矩阵快照缺失作为证据限制，并核对重建行级差异与当前矩阵。

证据位于 `build-codex-agent/agent-phase-5-slice-38-evidence/`：隔离运行时 probe exit 0（66 个插件库：65 个模块 + 1 个 V4L2 相机；3 个未构建项归因）；全量构建 exit 0；Folder/PLCWrite 定向 loopback CTest 2/2；全量 CTest 85/86，唯一失败为 `test_mainwindow` 中两项既有断言，其余 85 个目标通过；沙箱受限 PLC loopback 失败日志另行保留。生成器引用检查 483/483、清单计数核验、格式门禁及 `git diff --check` 通过；源/证据 SHA 与 66 个插件二进制 SHA 清单均校验通过。G5 标准 1 的数据条件已满足。

差异证据限制：首次重生成前，旧矩阵是未跟踪文件，未先留存完整快照；`matrix-generation-diff.md` 因此提供基于旧生成器逻辑、旧 phase0 探针日志和当时保留的只读计数/单元格输出所重建的行级差异，并明确旧原始文件及其 `generatedAt` 不可恢复。不可将这份重建文件称为旧 JSON 原始副本；历史字节级 diff 不可得。

限制：本片只刷新元数据/构建/运行时矩阵和 Folder/PLCWrite 的代表性执行、权限证据；旧 JSON 原始快照缺失的差异已如实记录，完整字节级历史 diff 不可验证。未关闭 G5 标准 5/6 的其他插件和 direct-tool 子路径，也不代表真实相机、真实 PLC、厂商 SDK 或阶段 8 硬件通过。

## 42. Slice 39：direct discover_cameras 的确认、拒绝与一次扫描

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。在 `test_agent_camera_lifecycle_g5` 的 fake async driver 上，将直接 discovery 从 Actor 内部工具调用改为真实 Autopilot `AgentController::handleToolCall` 链路。测试先等注册时自动 discovery 完成，并记录 `startDiscovery` 初始计数；待确认 payload 精确标识 `camera_discovery:<pluginId>`，pending 和拒绝后启动次数不增、没有新 batch，设备 ID 快照不变。对新 confirmationId 批准后，fake driver 恰增加一次扫描，batchSettled 恰成功一次，按其 requestId 轮询到同一 success 及目标 camera，最终设备集合无重复；审核日志将异步 enqueue 记录为 pending，将拒绝记录为 rejected。

验证证据位于 `build-codex-agent/agent-phase-5-slice-39-evidence/`：定向 CTest 1/1；全量构建 exit 0；获准本机 loopback 全量 CTest 85/86，唯一失败仍为 `test_mainwindow` 中两项既有断言，其余 85 个目标通过；受限环境不完整全量尝试单独保留，未纳入统计。`clang-format --dry-run --Werror` 和 `git diff --check` 通过。独立实现审核 `/root/slice16_implementation_review` **PASS**；核对了初始扫描基数、零调用拒绝、批准恰一次、requestId 终态与哈希。

限制：只关闭 fake async 相机的 direct discovery 确认门及调用计数子路径；不证明真实相机发现或厂商 SDK，不关闭 G5 标准 5/6 整体。

## 43. Slice 40：direct set_camera_parameter 的拒绝零写入

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。扩展 `testLegacyAgentCameraParameterWriteContract`：Observer 仍拒绝；Autopilot payload 的副作用目标精确为 `camera_parameter:parameter-legacy-device:exposureTime=4200`。待确认与拒绝后 fake setter 计数不增、实际曝光维持原值、审计为 rejected/undoable=false；新 confirmationId 批准后 setter 恰一次、读回 4200，审计 success/undoable=false。保留原有范围/类型/验证/写后不确定/回读错误测试。

证据位于 `build-codex-agent/agent-phase-5-slice-40-evidence/`：`test_cameramanager` 定向 CTest 1/1，全量构建 exit 0，获准本机 loopback 全量 CTest 85/86（唯一失败仍是 `test_mainwindow` 中两个既有断言，其余 85 个目标通过）；`clang-format --dry-run --Werror` 与 `git diff --check` 通过。独立实现审核 `/root/slice16_implementation_review` **PASS**，核验了副作用目标、拒绝零写入、ID 不复用、批准一次写入及终态审计。

限制：只关闭 direct 相机参数写的用户拒绝子路径，不覆盖 pending 状态期间 generation 变化或真实驱动结果，不关闭 G5 标准 5/6 整体。

## 44. Slice 41：sam_annotate 文件与网络副作用确认闭环

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；实现后独立审核 `/root/slice16_implementation_review` **PASS，无实现阻断**。扩展 `test_agent_sam_tools::samAnnotateWritesSessionAndAppends`，通过 Autopilot `AgentController::handleToolCall` 执行直接 `sam_annotate`。pending payload 的有序副作用为 `file_write:<sessionPath>`、`inference:sam_server`；pending/拒绝阶段文件不存在、SAM loopback 服务无请求且 set_image 计数为零。拒绝审计为 rejected/undoable=false；重试使用不同 confirmationId，批准后真实 AgentActor 工具恰执行一次批处理，写入可由 `AnnotationSession::load` 完整读取的两个对象，会话字段和 server 计数均按原断言核验，成功审计 undoable=false。既有追加对象测试保留。`toolExecuted` 结果连接以局部 QObject 为上下文，随用例结束断开，避免捕获局部结果的悬空引用。

证据位于 `build-codex-agent/agent-phase-5-slice-41-evidence/`：目标和全量构建 exit 0；`test_agent_sam_tools` 获准本机 loopback 定向 CTest 1/1；全量 CTest 85/86，唯一失败为 `test_mainwindow` 两项既有断言（工具中文标签比较、G4 失败迭代 runId 期望），其余 85 个目标通过；受限环境的 SAM loopback bind 失败原始记录单独保留。格式门禁、`git diff --check` 和源码/测试二进制/契约 SHA-256 清单通过。实现审核指出的摘要与验收条目缺失已补正，`test-summary.txt` 明列每项结果。

限制：仅验证本地 fake SAM 服务与临时会话文件下直接 `sam_annotate` 的确认/拒绝/批准路径；不证明真实 SAM 服务、模型质量、正式目录权限或其他文件/设备插件，不关闭 G5 标准 5/6 或阶段 8 门槛。

## 45. Slice 42：WriteText 流程文件副作用确认信息

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。`AgentController` 对流程副作用载荷新增 metadata ID `com.deeplux.plugin.writetext` 的明确目标：配置了 `filePath` 时列出 `file_write:<path>`；配置为空时列出 `file_write:<运行时输入路径或未配置>`，不把动态输入假报为未配置或已精确绑定，也不泄漏 `textContent`。`test_agent_folder_execution_g5` 的隔离插件树安装真实构建的 Folder 和 WriteText 动态库：Autopilot 对显式目标路径 pending/拒绝时 Engine 与模块未启动且文件不存在；拒绝审计 rejected/undoable=false；新 ID 批准后 RunEngine/WriteText 模块恰执行一次，内容精确为配置文本加换行，RunResult 和 success 审计匹配且 undoable=false。第二次将 `filePath` 清空，核验未知目标标记并拒绝，目标不创建、模块计数不变。

验证位于 `build-codex-agent/agent-phase-5-slice-42-evidence/`：`test_agent_folder_execution_g5` 定向 CTest 1/1；全量构建 exit 0；获准本机 loopback 全量 CTest 85/86，唯一失败仍为 `test_mainwindow` 两项既有断言（工具中文标签比较、G4 失败迭代 runId 期望），其余 85 个目标通过；格式与 `git diff --check` 通过。独立实现复审发现的归档缺项已经补齐；SHA 清单覆盖本片契约、验收记录、实现源文件、测试源/CMake、二进制和摘要，全部校验通过；实现审核最终结论记录于证据目录。

限制：只验证 WriteText 的明确配置路径写入与空配置保守标注/拒绝；不解析上游运行时 `file_path`、不覆盖 SaveImage/SaveData 或其他插件、不证明正式目录权限和写入失败/不确定语义，不关闭 G5 标准 5/6 整体或阶段 8。

## 46. Slice 43：SaveData 流程文件副作用确认

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。流程副作用映射新增 metadata ID `com.deeplux.plugin.savedata`：配置路径显示 `file_write:<path>`，空路径因插件可回退运行时 `input.data("file_path")` 而保守显示 `<运行时输入路径或未配置>`。同时将 Folder 删除副作用识别收紧为 Folder metadata ID 与 `operation=Delete` 双条件，防止其他插件携带同名参数被误判。隔离真实插件测试安装 Folder/WriteText/SaveData 动态库；SaveData 用例还故意带有插件不认识的 `operation:"Delete"` 参数，确认载荷仍只有通用 execution effect 与 SaveData 文件目标、没有 `folder_delete`。pending/reject 文件不存在且 Engine/模块零运行，拒绝审计 rejected/undoable=false；新 confirmationId 批准后单次运行、同一 runId 成功、结果文件为可解析 JSON object、完成信号成功、审计 success/undoable=false。空路径重新 pending 后显示未知目标，拒绝不运行且不创建文件。

证据位于 `build-codex-agent/agent-phase-5-slice-43-evidence/`：`test_agent_folder_execution_g5` 定向 1/1；全量构建 exit 0；获准 loopback 全量 CTest 85/86，唯一失败仍为 `test_mainwindow` 两项既有断言（工具中文标签比较、G4 失败迭代 runId 期望），其余 85 个目标通过；格式、`git diff --check` 与源码/测试/CMake/二进制/验收文档 SHA-256 清单通过。实现复审确认防误分类修复正确；最终复审归档结论见该片证据记录。

限制：只验证 SaveData 的 JSON 配置路径确认、拒绝和批准，以及动态路径的保守说明；不解析上游运行时 `file_path`，不覆盖 SaveImage、格式/追加变体、写入失败或不确定结果、正式目录权限和真实设备，不关闭 G5 标准 5/6 整体或阶段 8。

## 47. Slice 44：SaveImage 确认门与 OpenCV 输入图像保存修复

契约：`本地逐切片契约（未纳入提交）`，独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。流程副作用载荷对 metadata ID `com.deeplux.plugin.saveimage` 追加精确 `file_write:<filePath>`；测试在隔离动态插件树中以 4×3 临时纯色 PNG 作为 GrabImage(File) 输入，连接 GrabImage.image → SaveImage.image。Autopilot pending/reject 前目标不存在、Engine 与两个模块均未启动，拒绝审计 rejected/undoable=false；重试使用不同 confirmationId，批准后两个模块恰各执行一次，RunResult 的 runId 对应，输出 PNG 的尺寸/像素与输入一致，`engine.moduleOutput(saveId)` 也为有效且像素一致的透传图像，success 审计 undoable=false。

端到端测试揭示并修复 SaveImage 原有 OpenCV 缺陷：`input.hasMat()` 分支曾将未赋值的 `m_mat` 传给 `imwrite`，现在改为编码本次 `input.toMat()`；成功后明确执行 `output=input`，保证 Engine 输出透传。空 `ImageData`/Mat、null QImage 和转换失败现在均报错并返回失败；图像有效性检查移到创建父目录之前。新增直接动态模块测试验证 invalid Mat 执行失败且嵌套目录、输出文件均未创建。非 OpenCV QImage 路径保留，OpenCV 编码格式与质量参数保持原行为。

证据位于 `build-codex-agent/agent-phase-5-slice-44-evidence/`：`test_agent_folder_execution_g5` 定向 1/1；全量构建 exit 0；获准 loopback 全量 CTest 85/86，唯一失败仍为 `test_mainwindow` 两项既有断言（工具中文标签比较、G4 失败迭代 runId 期望），其余 85 个目标通过；格式、`git diff --check` 与本片源码/测试/CMake/契约/验收/二进制 SHA-256 清单通过。实现审核记录于该目录。

限制：只验证本地临时图像文件和 SaveImage 的 PNG OpenCV 路径；不覆盖真实相机、其他编码格式/质量组合、磁盘失败/原子替换、正式目录权限或阶段 8，不关闭 G5 标准 5/6 整体。

## 48. Slice 45：WriteText、SaveData、SaveImage 能力矩阵回写

契约：`本地逐切片契约（未纳入提交）`，实现前经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。只修改能力矩阵生成器对三项插件的 `execution` 与 `permission` 单元格赋值；证据逐项指向已独立审核通过的 Slice 42/43/44 测试和 CTest 日志。生成器运行后 reference validation 为 483/483；库存仍为 69 项（66 模块、3 相机）、66 个构建二进制、111 项操作。

差异审计对比 `build-codex-agent/agent-phase-5-slice-45-evidence/matrix-before-reconstructed.json` 与本片生成结果：六个且仅六个状态变化，分别是 WriteText、SaveData、SaveImage 的 execution 与 permission 从 NOT_TESTED 到 PASS；其余插件/维度状态、库存和操作分母不变。由于本片开始时能力矩阵是未跟踪生成文件，未保留原始字节级前快照；before 文件是用禁用本片新增赋值逻辑的生成器规则重建的状态快照，只能支持单元格状态语义差异，不能冒称原文件的字节级副本。生成命令日志、重建前快照、生成后快照、行级差异及 SHA-256 清单保存在上述 Slice45 证据目录。

独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**。审核确认前后恰有六个目标状态变化，库存、111 项操作和其他单元格状态未变，六格引用均对应 Slice42–44 的 focused CTest 证据；重建快照限制和输出维度未测试的边界已披露。SHA 清单全部校验通过。本片无产品代码、插件实现或测试断言改动，故不声称新构建或 CTest 结果；已执行生成器引用验证、69/111/483 定点校验、六格状态差异断言、Python 编译检查、矩阵源文件重生成和 `git diff --check`。

限制：三个插件的 `output`、`input`、配置、持久化等维度保持原状；代表性本地文件测试不证明 Agent `get_module_output` 业务结果、正式目录权限、其他输出插件或真实设备。Slice45 不关闭 G5 标准 5/6/7 整体，不放行阶段 6/8。历史差异仅有上述重建状态比较，不具备原矩阵字节级比较证据。

## 49. Slice 46：文件输出插件业务结果经 Agent 回读

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。在 WriteText、SaveData、SaveImage 的已批准 `run_flow` 运行完成后，测试经 `AgentController::handleToolCall("get_module_output")`，使用相同 runId 和实例 ID 读取结果；查询在 Observer 模式执行。WriteText 回读并交叉核对 `write_result`、路径和长度，SaveData 回读 `save_result` 与路径并核对 JSON 文件，SaveImage 回读同 run 的图像摘要并核对实例、像素坐标系、尺寸与 `hasOutput`；PNG 像素仍由既有文件和引擎输出断言验证。

能力矩阵生成器只将三项插件的 `output` 从 NOT_TESTED 更新为 PASS；execution、permission 与其他维度没有状态变化。矩阵 validator 483/483，库存 69 项（66 模块、3 相机）、66 个构建二进制及 111 项 GUI 操作保持不变。生成前 JSON/Markdown 快照、三格行级差异、生成日志和 SHA-256 清单位于 `build-codex-agent/agent-phase-5-slice-46-evidence/`。

验证：Slice46 focused CTest `test_agent_folder_execution_g5` 1/1 通过；全量构建 exit 0；获准 loopback 的全量 CTest 共 86 个目标，85/86 通过。唯一失败目标 `test_mainwindow` 内 103 项通过、2 项既有断言失败：FindCircle/ProcessCircle 文案比较，以及 `testG4FailedGuiCycleIterationDoesNotEndSession` 的 runId 期望为空；完整日志及 failed-test 名单留档。其余 85 个 CTest 目标均通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**，核实三项输出按同 runId/instanceId 经 Observer 工具入口回读、矩阵仅三格状态变化且无超范围声明；证据 SHA 全部校验通过。

限制：仅覆盖三个隔离文件插件的已知成功输出与 Agent 读取；不证明其他插件输出、写入失败/不确定状态、正式文件系统权限或物理设备，不关闭 G5 标准 5/6/7 整体或阶段 8。

## 50. Slice 47：WriteText 文件打开失败传播到 Agent 运行结果

契约：`本地逐切片契约（未纳入提交）`，实现前独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。修复 `WriteTextPlugin::process` 在 `QFile::open` 失败时虽设置 `write_result=false` 并发出错误、却仍返回成功的问题；现在返回实际写入结果。测试使用隔离临时根下确定不存在的父目录，经 Autopilot `run_flow` 验证 pending/拒绝时模块启动为零且目录不存在；新 confirmationId 批准后模块恰执行一次并失败，同一 runId 的 RunResult 与 `get_run_results` 状态为 failed、failedModule 为 WriteText、错误含目标路径，`get_module_output` 返回 unavailable/run_failed，且目录/文件均未创建。

验证：目标构建与全量 `cmake --build build-codex-agent -j2` 成功；Slice47 focused CTest `test_agent_folder_execution_g5` 1/1；获准 loopback 全量 CTest 85/86，唯一失败仍是 `test_mainwindow` 中既有两个断言（FindCircle/ProcessCircle 标签期望不一致；G4 失败迭代 runId 期望为空），该套件另有 103 项通过，其余 85 个 CTest 目标通过。全量日志、失败目标名单及源码/测试/契约/验收/二进制 SHA 留存在 `build-codex-agent/agent-phase-5-slice-47-evidence/`。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**，确认成功写入语义未变，失败状态与 runId/failedModule 传播正确；证据 SHA 全部校验通过。

限制：只处理 QFile 打开失败这一确定性错误，不证明短写、磁盘满、网络文件系统、其他插件失败语义、正式用户目录权限或真实设备；本片不更新矩阵状态，不关闭 G5 标准 5/6/7 整体或阶段 8。

## 51. Slice 48：TCPClient 线程亲和、目标确认与 Agent 收发回读

契约：`本地逐切片契约（未纳入提交）`，修订版经 `/root/slice19_contract_review` **CONTRACT_PASS**。修复实际线程冲突：RunEngine 把模块执行派发到线程池，TCPClient 旧版却从构造线程创建的成员 `QTcpSocket` 发起网络调用。本片改为每次 `process()` 在当前执行线程创建、使用和销毁局部 socket，删除共享 socket 的跨线程槽/事件循环状态，并保留连接/读取 timeout、写入/读取模式与输出键；执行处有 socket affinity 断言。

新增 `test_agent_tcpclient_execution_g5`，动态加载构建出的 TCPClient 插件，并由两个 localhost ephemeral peer 计数。pending 显示实例/host/port/operation 且不显示发送负载；待确认期间改端口后旧 confirmationId expired，两个 peer 均零连接/请求、模块零启动；新确认绑定新端口，旧 peer 仍为零，批准后新 peer 恰收一条指定 UTF-8 请求并回固定响应，模块恰执行一次。测试确认模块运行在线程池（非 Qt 测试主线程）；socket 在该 process 线程内栈构造并带 affinity 断言；同一 runId 的 RunResult 成功，Observer 经 `get_module_output` 读回响应和 UTF-8 字节数。另以 zero-timeout loopback 测试触发 `SocketTimeoutError`，确认失败消息保留 host、port 和旧 timeout 文案并进入 Agent `get_run_results`。

能力矩阵只将 TCP客户端的 execution、permission、output 三格从 NOT_TESTED 改为 PASS。生成器引用验证 483/483；清单仍为69插件（66模块+3相机）、66构建二进制、111操作；生成前后仅三格变化，JSON/Markdown 与行级 diff 留在 `build-codex-agent/agent-phase-5-slice-48-evidence/`。

验证：重新配置后目标构建成功；Slice48 focused CTest loopback 1/1；全量构建 exit 0；获准 loopback 全量 CTest 86/87 通过，唯一失败仍是 `test_mainwindow` 两项既有断言（FindCircle/ProcessCircle 文案期望不一致；G4 失败迭代 runId 期望为空），该套件还有103项通过，其余86个 CTest 目标通过。受限沙箱中的 listen 被拒日志、修正测试期望前的初次失败、超时消息修复前的审阅记录、完整全量日志和 SHA 清单均保留。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；复审确认零超时用例覆盖 host/port/预算旧诊断，矩阵三格、483 引用与 SHA 清单准确。

限制：只覆盖 TCPClient loopback 的 WriteRead 成功与端口漂移；不代表 TCPServer、PLCRead、PLCWrite、串口、真实网络/PLC、TLS/认证、其他操作模式或 retry/uncertain 路径已通过。线程修复仅针对 TCPClient；不关闭 G5 标准 5/6/7 整体或阶段 8。


## 52. Slice 49：TCPServer 本机确认、监听与 Agent 回读

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。TCPServer 不再从创建线程持有并跨线程调用 `QTcpServer/QTcpSocket`；每次 `process()` 在 RunEngine 执行线程内构造局部服务器，由其接收同线程 socket，并以线程断言和 worker-thread 信号测试作约束。原默认 `Any` 监听范围保持不变；新增 `bindAddress=Any|LocalHost` 配置和 GUI 下拉，metadata 声明 Any/LocalHost 选项并经 `get_module_params_schema` 断言 enum，参数验证与 GUI 一致，允许收窄到 loopback。测试从运行中 `QTcpServer::serverAddress()` 记录验证实际 LocalHost 为 `127.0.0.1`，并拒绝 `0.0.0.0` 监听记录。流程确认摘要披露实际接口范围、port、timeout、operation，不含发送负载。

新增 `test_agent_tcpserver_execution_g5` 动态加载真实构建插件：默认 Any 只 pending 后拒绝，确认摘要准确标示全接口，且零模块启动、无 listener；LocalHost 流程在待确认时改 port，旧 confirmationId expired，旧/新端口均无 listener；重新确认后只在新端口批准，loopback peer 与模块恰进行一次 UTF-8 收发，RunResult 成功，同一 runId 的 `get_module_output` 返回请求文本和字节数。另批准一个不连接 client 的流程，明确以“等待客户端连接超时”失败并经 `get_run_results` 回读 failed，避免把端口 bind 失败误作等待超时。线程测试确认模块运行在 worker thread。能力矩阵只将 TCP服务器 execution、permission、output 三格更新为 PASS；行级语义差异恰为三格，库存 69 插件（66模块+3相机）、66 构建二进制、111 GUI 操作、483 引用保持不变。

界面核对：TCPServer 配置控件在 420px/260px 宽、浅/深调色板下均可见且两项选择可写入参数；深色插件配置主题规则增加 QComboBox 弹出视图颜色，覆盖选中/文本对比。截图文件临时存于 `/tmp/deeplux-tcpserver-config-visual/`。尝试在 offscreen 模式自动打开完整 MainWindow 模态配置框并截图时测试卡住，失败日志保存在 `build-codex-agent/agent-phase-5-slice-49-evidence/ctest-focused-ui-modal-timeout.log`；该次不计成功，实际应用模态弹窗的 screenshot QA 未完成。

验证：全量构建 exit 0；Slice49 focused CTest 1/1 通过；最终 loopback 全量 CTest 87/88 通过。唯一失败为 `test_mainwindow` 两个既有断言：工具栏 FindCircle/ProcessCircle 标签期望不一致（`test_mainwindow.cpp:5878`）；失败 GUI cycle 迭代的 runId 期望为空、实际得到新 runId（`:7299`）。本片之前的限制环境 listen 拒绝和模态截图超时原始日志均保留。格式门禁、Python 编译检查、`git diff --check` 和矩阵 483 格引用验证通过。独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无阻断**；审核核对了真实 LocalHost 绑定地址与 metadata schema enum，矩阵仍恰改三格，全部 SHA 校验通过。

限制：loopback only 的 TCPServer WriteRead、绑定范围摘要、端口配置漂移和等待连接超时；未验证全接口真实网络、多个并发客户端、断连后不确定写入、TLS/认证、其他 operation、PLC/串口/真实设备。新增 UI 控件的配置面板 standalone screenshot 已核对，但 MainWindow 实际模态对话框截图无法在 offscreen 环境完成。G5 整体仍 IN_PROGRESS，不放行阶段 6/8。

## 53. Slice 50：PLCRead Modbus/TCP Agent 闭环与原生数组分页

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。修复 PLCRead 在模块创建线程持有 socket、随后由 RunEngine worker 调用所造成的线程亲和冲突；每次 `process()` 现创建并使用局部 `QTcpSocket`，保留 FC03、MBAP 校验、默认参数、JSON 数组/`_count` 与 timeout 语义。`run_flow` 将 PLCRead 作为需确认的设备访问，摘要列出 host/port/slave/address/count/timeout 且不含读回值。

新增隔离动态插件回归 `test_agent_plcread_execution_g5`：port-only 和 slaveId-only 漂移分别使旧确认过期；显式拒绝后及漂移后均无连接、FC03 或模块启动。批准的 loopback 成功执行验证 worker thread、精确 FC03 字段、同 runId Observer 数组输出分页（`limit=2` 两页，校验 `totalRows/offset/limit/truncated` 和全部三个寄存器值）及 `_count`。同一实例下一次批准执行收到一条 FC03 后超时，不重试；失败 `runId` 的数组与 `_count` 均返回 `unavailable/run_failed` 且没有 `value`。测试保留之前成功数组作为内存基线，不要求 RunEngine 保留旧运行历史。

为使既有 FC03 协议测试匹配生产执行模型，`test_communicationplugins` 中三个 PLCRead fake-peer 场景改为在 worker thread 调用同步插件，并由测试主线程运行 peer 事件循环；FC03 roundtrip、transaction/byte-count mismatch 和 no-retry timeout 断言均保留。定向结果：PLCRead Agent focused CTest 1/1；`test_communicationplugins` 1/1。

矩阵生成器将 PLCRead (`PLC读取`) 的 execution、output、permission 三格从 NOT_TESTED 改为 PASS，483 个引用校验通过。前快照取 Slice49 最终矩阵；行级语义差异恰为三格，69 插件（66 模块、3 相机）、66 构建二进制、111 操作保持不变。

验证：契约 `CONTRACT_PASS`；Slice50 focused CTest 1/1，既有 `test_communicationplugins` 1/1；全量构建 exit 0；loopback 全量 CTest 最终 89/89 通过。完整测试首次重跑定位到两项旧 MainWindow 断言：流程树按实例名显示“圆检测”，不应等于工具箱“找圆”；G4 cycle 执行失败现按已验收 Slice8 fail-stop 结束而非继续执行。只修正测试预期/取值源，没有改 UI 或运行逻辑；`test_mainwindow` 定向 1/1、完整 CTest 89/89 均通过。之前失败运行日志保留，最终通过日志另存。矩阵 483/483 引用验证和仅三格状态差异校验通过；格式、Python 编译与 `git diff --check` 通过。源码/证据/二进制 58 项 SHA 全部通过；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无阻断**。

限制：仅 loopback fake Modbus peer；未连现场 PLC，不代表不同 PLC 固件、网络、中继/网关或硬件表现。RunEngine 只保证当前运行可读，失败 `runId` 不会泄漏前次成功数组；本片不改变历史运行保留策略。G5 标准 5/6/7 和阶段 8 仍未关闭。

## 54. Slice 51：PLCWrite 成功与不确定结果的 Agent 输出回读

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。扩展 `test_agent_plcwrite_execution_g5`：`run_flow`、`step_once`、`resume_flow` 各自批准成功后，都使用该路径的真实 runId/模块实例经 Agent `get_module_output` 读取 `plc_write_success=true` 与 `plc_write_count=1`；不以 RunResult 或 peer 收包替代 Agent 输出证据。FC16 已发但无 echo 的 uncertain timeout 查询这两个键均返回 `status=uncertain`、`retryable=false` 且没有成功 `value`，peer 请求仍恰一条。未修改 PLCWrite 生产输出或重试逻辑。

矩阵生成器只将 PLCWrite (`PLCWritePlugin`) 的 output 从 NOT_TESTED 改为 PASS；相较 Slice50 快照，语义差异恰一格。库存 69 插件（66 模块、3 相机）、66 构建二进制、111 GUI 操作不变；483 格引用校验通过。

验证：Slice51 focused CTest 1/1；全量构建 exit 0；loopback 全量 CTest 89/89；格式、Python 编译、`git diff --check` 和矩阵差异校验通过。源码/证据/二进制 27 项 SHA 全部通过；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无阻断**。

限制：仅 loopback fake Modbus peer；不连接真实 PLC，不证明现场写入效果、厂商响应差异或硬件可靠性。只补足 PLCWrite 成功/不确定输出回读维度，不单独关闭 G5 标准 5/6/7。

## 55. Slice 52：SerialPort PTY 闭环与执行线程亲和

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。`SerialPortPlugin` 不再跨 RunEngine 线程共享构造线程创建的 `QSerialPort`；串口对象、配置、开关、读写均在 `process()` 的执行线程局部创建/完成，并带线程 affinity 断言。Agent `run_flow` 确认摘要披露模块实例、端口名、baud/data/parity/stop/flow、operation 和 timeout，不披露 `writeData` 或运行时 payload。

新增隔离动态加载测试 `test_agent_serialport_execution_g5`，在 POSIX PTY 上验证：拒绝确认后无模块启动/收发字节；待确认时 `portName` 漂移使旧 confirmationId expired，旧/新 PTY 均零字节；重新确认后插件在线程池执行一次，PTY master 恰收到一条 `CMD-7` 并回复 UTF-8 `OK-✓`，Agent 按该成功 runId/instance 读取 `agent_serial_reply` 和 `_bytes` 均与 peer 相符。确认摘要不含发送 payload。物理串口未打开。

矩阵只将 `SerialPort` execution、output、permission 三格从 NOT_TESTED 改为 PASS；其余语义差异为零。69 插件（66 模块、3 相机）、66 构建二进制、111 操作及 483 个矩阵引用不变，生成器逐引用校验通过。JSON/Markdown 前后快照、差异与生成日志保存在 `build-codex-agent/agent-phase-5-slice-52-evidence/`。

验证：Slice52 focused CTest 1/1；全量构建成功；完整 CTest 90/90 通过；格式门禁、Python 编译、`git diff --check` 和 SHA-256 检查通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；复审确认线程亲和、PTY 收发、Agent 确认摘要、同 runId 输出读取及三格矩阵差异。26 项源码/文档/证据 SHA 与 3 项二进制 SHA 均校验通过；完整校验输出保存在 `build-codex-agent/agent-phase-5-slice-52-evidence/sha256sum-check.log`。

限制：POSIX PTY 只证明受控本机串口路径，不代表物理串口适配器、驱动、波特率容差、拔插或现场设备已验证。超时后的“写已发送但无响应”不确定结果语义/重试策略仍未收口，归后续切片；G5 标准 5/6/7 和阶段 8 仍未关闭。

## 56. Slice 53：SerialPort 超时不确定结果与禁止重放

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。`SerialPortPlugin` 记录本次执行是否有任意字节被 `QSerialPort::write()` 接受；`WriteRead` 已写后无响应不再返回成功，而以 `ExecError::SideEffectUncertain` 结束。写入前失败保持确定性错误；`Write` 单向模式不要求不存在的协议 ACK；`Read` 无响应超时是普通失败。没有加入自动重试。

扩展 `test_agent_serialport_execution_g5`：批准流程到 PTY 后 master 收到一次完整 `CMD-53` 且不回数据，RunResult 与 Agent `get_run_results` 都为 uncertain、`retryable=false`；`get_module_output` 不提供成功值，重复查询不再次执行，PTY 请求数仍为一。另验证单向 `Write` 无回复仍成功且只发一条；纯 `Read` timeout 的 RunResult 为 Processing/failed、PTY 无出站字节且结果不标 uncertain。

矩阵生成后所有维度状态保持不变；库存 69 插件、66 构建二进制、111 操作及 483 格引用均未变化。验证：SerialPort Agent focused CTest 1/1；全量构建成功；loopback 全量 CTest 90/90；格式、Python 编译、`git diff --check` 与矩阵状态零差异验证通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；针对首次复审指出的文本 `readVariable` 未查询问题，已与 `_bytes` 一起按同一 runId/instanceId 查询并分别断言无值。最终源码/证据/二进制 SHA 均通过，归档于 `build-codex-agent/agent-phase-5-slice-53-evidence/`。

限制：PTTY 无响应可重复模拟响应超时，但不能证明所有 USB/串口驱动的部分写时序；不会自动重放无法确认的指令。未连接真实设备；G5 标准 5/6/7 和阶段 8 仍未关闭。

## 57. Slice 54：PLCCommunicate Agent 确认与 FC03 loopback

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。`PLCCommunicatePlugin` 已移除构造线程创建并由 worker 跨线程使用的 `QTcpSocket`、成员事件循环与槽；每次 `process()` 在线程内创建局部 socket，同步连接、发送固定单寄存器 FC03、等待响应并关闭，带 affinity 断言。保留 `ipAddress/port/timeout` 与 `plc_connected/plc_response_time_ms` 输出键。Agent 确认摘要披露模块实例、host/port、固定 FC03 address/count 和 timeout，不披露响应值。

新增隔离动态插件测试 `test_agent_plccommunicate_execution_g5`：两个本机 `QTcpServer` 分别代表旧/新 port；确认拒绝后两端零连接；待确认时 port 漂移使旧 confirmationId expired，仍零连接；批准新确认后只有新 peer 收到一次 unit=1/function=03/address=0/count=1 请求并回有效响应。测试确认模块在线程池执行、RunResult 成功，Agent 按同一 runId/instanceId 读到 `plc_connected=true` 与 response time。

矩阵只将 `PLCCommunicatePlugin` execution/output/permission 三格从 NOT_TESTED 改为 PASS；前后语义差异恰三格，库存 69 插件、66 构建二进制、111 操作、483 格引用不变，生成器引用校验通过。验证：focused CTest 1/1；全量构建成功；loopback 全量 CTest 91/91；格式门禁、Python 编译、`git diff --check` 与矩阵差异验证通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；确认局部 socket 在线程亲和正确的执行线程工作，拒绝/漂移均零连接，批准后只向新 peer 发一次精确 FC03 请求并按本次 runId 回读输出。27 项源码/证据/二进制 SHA 全部通过，结果保存在 `build-codex-agent/agent-phase-5-slice-54-evidence/`。

限制：只测试固定 FC03 连通性探测，不等价于 PLCRead 的业务寄存器配置或 PLCWrite；无现场 PLC/网关/网络验证。G5 标准 5/6/7 和阶段 8 仍未关闭。

## 58. Slice 55：SamPredict 外部推理确认与确认卡摘要脱敏

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。直接 Agent 工具 `sam_predict` 已标为 dangerous；Observer 拒绝，Advisor 与 Autopilot 均先进入确认。确认卡摘要只显示工具名及安全 server origin（scheme、host、effective port），不序列化原始参数；HTTP 缺省端口显示 80。副作用列表显示 imagePath 与同一个安全 origin。独立实现复审初次发现通用参数摘要会把 URL 凭据、路径和 query 暴露；现已修复，并经复审 `/root/slice16_implementation_review` **PASS，无剩余 finding**。

扩展 `test_agent_sam_tools::samPredictRequiresConfirmationAndDisclosesOnlySafeOrigin`：含 userInfo、base path、query、fragment 的 URL 在拒绝前确认摘要不含凭据、路径、token、fragment 或 label；显式端口与默认 HTTP 端口均被规范化；两次拒绝服务请求数和 set-image 次数为零。干净 URL 获新 confirmationId 批准后，fixture 服务只收到一次 set-image 与一次 predict，返回预测结果，审计恰为 rejected×2、success×1。`test_agentpermissions` 和 `test_toolschema` 同步验证 Autopilot 确认策略和 dangerous schema。

能力矩阵未改动：SamPredict 插件 execution/output/permission 仍为 NOT_TESTED，本片只证明直接 Agent 工具路径，不把它外推为 `SamPredictPlugin` RunEngine 证据。69 插件、66 构建二进制、111 GUI 操作、483 格引用及矩阵状态均不变；生成前后 JSON/Markdown 快照和零差异 TSV 留存于 `build-codex-agent/agent-phase-5-slice-55-evidence/`。

验证：Slice55 focused CTest 3/3（`test_agent_sam_tools`、`test_agentpermissions`、`test_toolschema`）；最终全量构建 exit 0；loopback 全量 CTest 91/91。`clang-format --dry-run --Werror`、Python 编译、`git diff --check`、矩阵 483/483 引用验证、矩阵状态零差异验证通过。最终独立实现复审 **PASS，无剩余 finding**；源码、文档、最终测试日志和相关二进制 SHA 清单归档于 `build-codex-agent/agent-phase-5-slice-55-evidence/`。

限制：仅本机 fixture SAM HTTP 服务，不证明真实模型、远端服务凭据处理、服务端数据保留、GPU/CPU 性能或预测质量；`SamPredictPlugin` 逐插件执行/输出/权限矩阵维度仍 NOT_TESTED。G5 标准 5/6/7 和阶段 8 仍未关闭。

## 59. Slice 56：GrabImage Agent 确认目标、空目标拒绝与配置漂移

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。`AgentActor::flowSelectorPreflight` 现在对 Camera 模式 GrabImage 的 `run_flow`、`step_once`、`resume_flow` 要求显式非空 `cameraId`；缺失/空白时确认前返回 `MISSING_PARAM`。这样 Agent 不会因隐式选机而扫描设备、连接首个相机或落到 OpenCV index 0。GUI/手动 GrabImage 原行为保持不变。已有不支持的 `flowId` 错误优先级保留为 `FLOW_UNAVAILABLE`。

`test_agent_camera_lifecycle_g5` 将构建出的 GrabImage metadata/库复制进临时 app-data 插件树，经 `PluginManager` 加载并由 Agent run_flow 使用 RunEngine 默认动态工厂。fake camera 已发现但未连接时，空 cameraId 的三种 Agent 执行入口均在确认前返回 `MISSING_PARAM`；没有 pending/toolExecuted/moduleStarted，engine 不运行/恢复，discovery/connect/session/trigger 计数不变。显式 cameraId 的确认副作用项包含实例、设备、exposure、gain、timeout；拒绝后无启动/触发。pending 后改 cameraId，旧 confirmationId 消费后审计 `expired`，模块和采集计数仍为零。恢复显式目标并用新 confirmationId 批准后，RunResult 成功，fake async 相机恰触发一帧、按当前 exposure/gain 写入；Agent 用同 runId 回读 completed 及 4×3 图像输出摘要，session 按既有契约退役。

能力矩阵只将 GrabImage execution 与 permission 两格从 NOT_TESTED 更新为 PASS；configuration/input 保持 NOT_TESTED，output 原为 PASS。前后状态差异恰两格；69 插件（66模块+3相机）、66 构建二进制、111 GUI 操作和 483 引用库存不变。审计脚本同步了 Slice56 规则，使以后从源码/探针重生成时保留这两格；JSON 唯一数据源重新渲染 Markdown，483/483 引用验证通过。前后快照、两格 TSV 差异及验证输出在 `build-codex-agent/agent-phase-5-slice-56-evidence/`。

验证：定向 `test_agent_camera_lifecycle_g5` 与 `test_agentactor` 2/2；最终全量构建 exit 0；全量 CTest 91/91、0 失败、0 跳过。格式门禁、Python 编译与 `git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余代码阻断**；契约复审 **CONTRACT_PASS**。源码、契约、审核记录、测试日志及相关二进制 SHA 校验结果归档在 Slice56 证据目录。

限制：只用本机 fake async camera 和动态加载的真实 GrabImage 插件二进制；不代表 V4L2/Hikvision/DirectShow、现场断连或物理相机图像质量。空 cameraId 只在 Agent 流程入口被 fail-closed；真实设备连接、隐式目标运行、其他插件及 G5 标准 5/6/7 的总体闭合仍未验收。阶段 8 不放行。

## 60. Slice 57：SamPredict 插件流程确认、脱敏与 endpoint 漂移

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。RunEngine `SamPredict` 插件现在进入流程副作用确认清单，确认项绑定模块 instanceId 与 SAM 服务的 scheme/host/effective port，并说明上游图像会发送到该服务。URL 的 userInfo、路径、query、fragment、图像路径与 prompt 均不进入确认信息；GrabImage Path 输入不会伪报为 file_write。

`test_agent_sam_tools::samPredictPluginFlowConfirmationBindsNetworkTarget` 将真实构建的 GrabImage/SamPredict metadata 与动态库安装到临时 app-data 插件树，经 PluginManager 加载，使用临时 PNG 构造 `GrabImage(Path) -> SamPredict` 流程并由 Agent Autopilot/RunEngine 默认插件工厂执行。拒绝路径确认摘要脱敏且两 fake peer 均零请求；pending 时将服务从 peer A 改到 peer B 后，旧 confirmationId 被消费并审计为 expired，仍无模块启动或网络请求；新确认显示 peer B，批准后只有 B 收到恰一次 set_image 与 predict。最终 Flow RunResult 成功，并由 Agent 按同一 runId 回读 completed。

独立实现复审首次发现测试提前失败时可能在 RunEngine 仍执行时释放 fake peer/临时插件目录。已增加失败路径 scope cleanup：请求停止后等待 engine idle，再清空模块、关闭工程、卸载插件；`DEEPLUX_APP_DATA_DIR` 无论测试结果如何都恢复原值。复审者确认脱敏、目标漂移和请求断言符合契约；最终复审结论与哈希记录归档于 Slice57 证据目录。

能力矩阵只将 SamPredict 插件 execution 与 permission 从 NOT_TESTED 改为 PASS；input 仍 BLOCKED，output 仍 NOT_TESTED，configuration 不变。矩阵逐格前后差异恰为这两格；各维度状态汇总中对应 PASS 各加 1、NOT_TESTED 各减 1；inventory 保持 69 插件（66 模块+3 相机）、66 构建二进制、111 操作、483 引用。审计生成器已加入 Slice57 规则，483/483 引用校验通过，JSON 为唯一数据源并重新渲染 Markdown。测试不写入持久运行配置。

验证：定向 `test_agent_sam_tools` 1/1；全量构建 exit 0；loopback 全量 CTest 91/91、0 失败、0 跳过；`clang-format` 修改行门禁、Python 编译和 `git diff --check` 通过。最终独立实现复审及源码/契约/验收记录/矩阵/测试日志/相关二进制 SHA-256 结果保存在 `build-codex-agent/agent-phase-5-slice-57-evidence/`。

限制：只用本机 HTTP fake peers 与真实构建的 SamPredict 插件，不证明真实 SAM 服务、模型权重、网络访问控制、GPU/CPU 性能或预测质量；不据此更新 input/output/configuration，不关闭其他插件的 G5 网络/文件/设备风险，也不放行阶段 8。

## 61. Slice 58：SamPredict Region2D 与 score 的 Agent 输出回读

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。在 Slice57 的动态插件流程成功后，测试通过 Agent `get_module_output` 按 SamPredict instanceId 与同一成功 runId 分别读取 `region`、`score`。Region2D 字段核对 label、bbox、四点 polygon、score 和 `image_pixels` 坐标系；bounded preview 提供同一 fake peer 的 polygon 点，RLE 内容不内联。拒绝/expired 流程没有 runId、模块未启动。输出回读前后 peer 请求数、临时 fixture 文件列表和 `serverUrl` 配置保持不变。

能力矩阵只将 SamPredict 插件 output 从 NOT_TESTED 更新为 PASS；input 保持 BLOCKED，configuration、execution、permission 保持 Slice57 后状态。逐格差异恰一格；库存仍为 69 插件、66 构建二进制、111 操作，审计生成器 483/483 引用验证通过，Markdown 从 JSON 重渲染。Slice58 验证日志、矩阵前后快照及一格差异表保存在 `build-codex-agent/agent-phase-5-slice-58-evidence/`。

验证：定向 `test_agent_sam_tools` 1/1；全量构建 exit 0；loopback 全量 CTest 91/91、0 失败、0 跳过；修改行 clang-format 门禁、Python 编译和 `git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；源码、证据和构建二进制 SHA-256 校验记录归档于 Slice58 证据目录。

限制：只用 fake SAM peer 与动态加载的 SamPredict 插件；不证明真实模型/服务、分割质量或 mask 全量导出，不据此改变 input/configuration，不关闭其他 G5 风险或放行阶段 8。

## 62. Slice 59：Folder 删除结果的 Agent 输出回读

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。Folder 成功删除临时目录后，测试在修改工程进入后续 `step_once` 之前，使用 Agent `get_module_output` 和同一 `runId`、`instanceId`，分别回读 `operation_result` 与 `folder_path`。结果是 JSON 原生 boolean `true` 和获批目录的精确路径；读取后目录仍不存在，Folder 模块启动/完成次数未增加。两次只读查询的审计事件也按预期记录。

能力矩阵只将 Folder output 从 NOT_TESTED 更新为 PASS；Folder execution/permission 及所有其他维度保持原值。前后维度差异恰一格；库存仍为 69 插件、66 构建二进制、111 GUI 操作，审计脚本引用校验 483/483 通过，JSON 重新渲染 Markdown。Slice59 前后矩阵、精确差异表和校验日志保存在 `build-codex-agent/agent-phase-5-slice-59-evidence/`。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；loopback 全量 CTest 91/91、0 失败、0 跳过；修改行 clang-format 门禁、Python 编译与 `git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；28 项最终源码快照、测试日志及二进制 SHA 校验全部通过，结果归档于同目录。

限制：仅验证 Folder Delete 的一条业务输出路径；不外推至 List/Create/Exists 或其他文件插件，不关闭其他 G5 风险或放行阶段 8。

## 63. Slice 60：Folder Delete 的 folderPath 漂移保护

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。`test_agent_folder_execution_g5::testFolderDeleteRequiresConfirmationForRunFlowAndStepOnce` 在拒绝首个删除请求后，对目录 A 再次创建 pending confirmation；确认摘要绑定 A。pending 期间将工程 `folderPath` 改为独立目录 B 后消费旧 confirmationId，审计结果为 `expired`，A/B 及 probe 文件全部保留，模块启动/完成、RunFinished 与 toolExecuted 计数均为零，engine idle。配置恢复到 A 并取得新 confirmationId 后批准，只删除 A，B 仍存在；同一成功 runId 的 Folder `operation_result` 与 `folder_path` Agent 输出回读继续通过，随后既有 step_once 回归未受影响。

矩阵只更新 Folder permission cell 的 reason/evidence：PASS 状态不变，证据引用 Slice31/34/60；所有维度状态、汇总计数、69 插件/66 构建二进制/111 操作库存均不变。矩阵 483/483 引用校验通过，差异和前后快照保存在 `build-codex-agent/agent-phase-5-slice-60-evidence/`。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；loopback 全量 CTest 91/91、0 失败、0 跳过；修改行 clang-format 门禁、Python 编译和 `git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**；31 项最终源码快照、测试日志和二进制 SHA 校验全部通过，归档于 Slice60 证据目录。

限制：只证明 Folder Delete 的路径变化会使旧确认失效；不外推到其它 Folder 操作、其他文件插件或网络文件系统，不关闭 G5 全部硬件门槛或放行阶段 8。

## 64. Slice 61：TCPClient 写入后响应超时标为不确定且禁止重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。`TCPClientPlugin` 新增专属结构化 `execute()` 映射，不改变通用 `ModuleBase` 失败语义。执行开始重置写入追踪；仅当 `QTcpSocket::write()` 接受至少一个字节后发生后续失败，才返回 `ExecError::SideEffectUncertain`。结构化错误携带实际目标与等待超时。

`test_agent_tcpclient_execution_g5` 使用真实动态加载 TCPClient 插件和 loopback peer。Autopilot 请求在待确认与显式拒绝后均保持模块启动/连接/请求为 0；批准后 peer 接收恰好一份请求但不响应，RunResult 与 Agent 查询以同一 runId 报告 `uncertain`、`sideEffectStatus=uncertain`、`retryable=false`，lastError 含地址、端口和 timeout。重复查询两次后模块启动、连接和请求仍恰为 1。额外边界用例精确断言：纯 Read 无写入超时为 `failed`、非空单向 Write 无响应为 `completed`、空 WriteRead 超时为 `failed`，三种均使用同一 runId 读取结果且模块启动一次。

矩阵 JSON/Markdown 状态无变化；TCPClient execution/output/permission 仍为 PASS。库存与引用保持 69 插件、66 构建二进制、111 GUI 操作和 483 格引用；矩阵 before/after 字节一致，483/483 引用校验通过。无矩阵状态升级。

验证：`cmake --build build-codex-agent -j2` 全量构建成功；`test_agent_tcpclient_execution_g5` loopback CTest 1/1；最终全量 loopback CTest 91/91、0 失败、0 跳过；clang-format 修改文件门禁、`git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。构建、定向/全量 CTest、矩阵校验、二进制清单与源快照均在 `build-codex-agent/agent-phase-5-slice-61-evidence/`。

限制：本片只证明本机 loopback 下 TCPClient `WriteRead` 的 Agent 结果分类和查询不重放。它不证明远端最终是否执行请求，不等同于业务协议确认；不覆盖 TCPServer、PLC、串口、真实网络、`step_once`/`resume_flow` 或真实硬件，不关闭 G5 标准 5/6/7，也不放行阶段 6/8。

## 65. Slice 62：direct sam_predict 上传后超时的不确定结果

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；共享批量预测阶段标记修正也获复核确认。`SamBackendClient` 为每个 `/set_image` 和 `/predict` reply 以 `QNetworkReply::uploadProgress(bytesSent>0)` 发出阶段绑定的上传事件。`runSamPredictBatch` 在当前请求阶段发生客户端或本地等待超时后标记 `uncertainStage`；set_image 成功响应和每个成功 predict 响应都会清空前一请求阶段，避免跨请求串用。只读 direct `sam_predict` 将这类失败返回为 `status=uncertain`、`sideEffectStatus=uncertain`、`retryable=false`、`errorCode=SIDE_EFFECT_UNCERTAIN`，不伪造区域；参数/图像预检失败仍是普通失败。

`SamTestServer` 增加“完整读取并记录 body 后不回复”的 request barrier。Agent Autopilot 测试覆盖 set_image body 已收全后超时（1 set_image、0 predict、stage=set_image）、set_image 成功后 predict body 收全再超时（1 set_image、1 predict、stage=predict）；结果均 uncertain 且不返回 polygon/score。两条确认批准后请求均不重放。另验证批量 helper 第一项成功、第二项完整请求超时（1 set_image、2 predict），确保阶段值只属于当前请求；非法图像在发请求前失败且 server 请求数为 0。

能力矩阵状态不变：direct 工具证据不升级 `SamPredictPlugin` 的 execution/output/permission；69 插件/66 构建二进制/111 GUI 操作及 483 格引用不变。全构建成功；`test_agent_sam_tools` loopback CTest 1/1；最终全量 loopback CTest 91/91、0 失败、0 跳过；格式门禁与 `git diff --check` 通过。独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。证据位于 `build-codex-agent/agent-phase-5-slice-62-evidence/`。

限制：本片只证明本地 fake server 下已上传请求的超时分类和不自动重发；不证明真实服务是否完成推理、不改变 SamPredictPlugin 行为、不覆盖 `sam_annotate` 会话写入、不升级插件矩阵状态，也不关闭 G5 标准 5/6/7 或阶段 8。

## 66. Slice 63：direct sam_annotate 不确定结果、会话零写入与审计

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；最终实现经 `/root/slice16_implementation_review` **PASS，无阻断**。`AgentActor::samAnnotate()` 在共享预测结果含 `uncertainStage` 时返回与 direct `sam_predict` 一致的 `SIDE_EFFECT_UNCERTAIN` 结构，不再将上传后的远端超时降为普通 `PRECONDITION_FAILED`。该失败分支早于标注对象追加与 `session.save()`。

`test_agent_sam_tools` 覆盖两阶段：predict 上传后超时，existing session 字节完全不变、重载标注数不变，返回无 objectIds/annotated；set_image 上传后超时，新 session 文件仍不存在且无 predict 请求。两条路径均断言同一工具结果含 `status=uncertain`、`sideEffectStatus=uncertain`、`retryable=false`、稳定 errorCode 与正确 stage；`agentActionCompleted` 信号保留这些字段，确认审计 `result=uncertain` 且 `undoable=false`。终态后继续处理事件，请求数仍精确为 1/1 或 1/0。

同时修正 AgentController confirmed audit 与工具结果摘要：`status=uncertain` 优先于 `error` 字段，因此不会记成普通 error，也不会显示为确定失败。Slice62 的 direct `sam_predict` 测试同步断言 audit uncertain。

矩阵状态不变；direct `sam_annotate` 不升级 SamPredictPlugin execution/output/permission。全构建成功；focused `test_agent_sam_tools` loopback CTest 1/1；最终全量 loopback CTest 91/91、0 失败、0 跳过；clang-format 门禁和 `git diff --check` 通过。证据位于 `build-codex-agent/agent-phase-5-slice-63-evidence/`。

限制：只证明本机 fake server 超时情况下的 direct Agent 返回、审计及本机会话零写入；不证明真实 SAM 服务是否完成推理，不升级插件矩阵状态，也不关闭 G5 标准 5/6/7 或阶段 8。

## 67. Slice 64：SaveData step_once 文件目标漂移保护

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。隔离动态插件 fixture 使用真实 SaveData 与两个预先存在的 JSON 文件：A/B 分别保留不同 sentinel，参数附带无效用法的 `operation=Delete`，验证 Agent 仍将流程副作用归类为 `file_write` 而不是 Folder 删除。

Autopilot `step_once` 对 A 生成待确认后，测试将工程 `filePath` 改为 B 并消费旧 confirmationId。审计为 `expired`，模块 started/finished、RunFinished 与 toolExecuted 均为零，Engine idle，A/B 字节不变。新确认载荷的 `sideEffects` 绑定 B；批准后模块只启动/完成一次，`step_once` 与 RunFinished 共用成功 runId，B 被覆盖为合法 JSON、A 字节保持；审计 success 且 undoable=false。

能力矩阵只更新 SaveData permission 的 reason/evidence，PASS 状态不变；其他维度状态、69 插件/66 模块/3 相机、111 项 GUI 操作和 483 格引用均保持不变，引用校验通过。Matrix before/after、精确差异、测试日志和哈希清单位于 `build-codex-agent/agent-phase-5-slice-64-evidence/`。

验证：`cmake --build build-codex-agent -j2` 成功；定向 `test_agent_folder_execution_g5` 1/1；获批 loopback 全量 CTest 91/91、0 失败、0 跳过；格式门禁、Python 编译和 `git diff --check` 通过。独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；最终实现独立审核 `/root/slice16_implementation_review` **PASS、无阻断**。10 项源/证据 SHA-256 与 2 项二进制 SHA-256 全部通过；证据位于 `build-codex-agent/agent-phase-5-slice-64-evidence/`。

限制：本片只证明隔离临时文件下 SaveData `step_once` 的目标漂移失效与一次性覆盖，不外推至 SaveData 其他模式、其他文件插件或网络文件系统，不关闭 G5 标准 5/6/7，也不放行阶段 6/8。

## 68. Slice 65：SaveData resume_flow 拒绝、路径漂移和一次性写入

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。隔离真实 SaveData 插件将 `breakpoint` 设在模块执行之前；RunOnce 取得 runId 后进入 Paused，文件 A/B 保留各自 sentinel，SaveData 尚未启动。

Autopilot `resume_flow` pending 的 `sideEffects` 绑定 A 且不含 Folder 删除。显式拒绝保持 Paused，审计 `rejected/undoable=false`，模块与 RunFinished 仍为零。再次 pending 后工程 `filePath` 从 A 改为 B，消费旧 confirmationId 得到 `expired`；Engine 继续 Paused，模块、RunFinished 与 toolExecuted 仍为零，A/B 均不变。恢复工程配置 A 后再发起新 `resume_flow`，新载荷重新绑定 A；批准后 SaveData 恰执行一次，Engine Idle，RunResult 成功且沿用暂停前 runId，A 被覆盖为合法 JSON，B sentinel 保持。

此处只验证运行时加载的 A 配置被恢复后安全续跑；暂停的 RunEngine 模块实例持有 loadProject 时复制的参数，本片不宣称可在同一暂停运行中热切换到 B。矩阵只更新 SaveData execution 的 reason/evidence，PASS 状态不变；其他维度、库存及 483 格引用不变。

验证：`cmake --build build-codex-agent -j2` 成功；focused `test_agent_folder_execution_g5` 1/1；获批 loopback 全量 CTest 91/91、0 失败、0 跳过；格式门禁、Python 编译及 `git diff --check` 通过。独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；最终实现独立审核 `/root/slice16_implementation_review` **PASS、无阻断**。10 项源/证据与 2 项二进制 SHA-256 全部通过；证据位于 `build-codex-agent/agent-phase-5-slice-65-evidence/`。

限制：只证明隔离临时文件下 SaveData `resume_flow` 的拒绝、配置漂移后旧确认失效及恢复原配置后的单次写入；不覆盖磁盘失败、追加/其他格式、网络文件系统、真实硬件或 G5 标准 5/6/7 整体，不放行阶段 6/8。

## 69. Slice 66：WriteText/SaveData 运行时文件目标不可见时 fail closed

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。`AgentController` 的 Autopilot 专属共享 preflight 对 `run_flow` 检查所有启用的 WriteText/SaveData 模块；如果 `filePath` 为空或仅空白，返回稳定错误 `SIDE_EFFECT_TARGET_UNRESOLVED`、`retryable=false`，附受影响 instance IDs。聊天与 Bridge 两入口共用该拒绝逻辑，因此无法进入 pending confirmation；Advisor 的既有确认/拒绝行为保留，静态显式路径不变。

隔离动态插件测试分别覆盖两个插件的 Bridge 与聊天入口。Autopilot Bridge 直接收到同码错误并审计 `error`；Advisor Bridge 仍生成待确认项并在拒绝后保持零执行。聊天测试提交真实格式的单个 assistant tool call，确认没有 Autopilot 待确认批次、模块启动/完成、RunFinished 或文件新增，审计为 error；随后读取对话历史，验证该 call 恰有一条匹配 callId 的结构化 tool 结果且携带相同错误码。WriteText/SaveData 显式路径既有拒绝与批准写入回归继续通过。

能力矩阵只更新 WriteText 与 SaveData 的 permission reason/evidence；两个维度状态仍 PASS，其他维度、69 插件/111 操作库存和 483 格引用保持不变。

验证：`cmake --build build-codex-agent -j2` 成功；focused `test_agent_folder_execution_g5` 1/1；获批 loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 编译和 `git diff --check` 通过。独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；最终实现独立审核 `/root/slice16_implementation_review` **PASS、无阻断**。13 项源/证据与 4 项二进制 SHA-256 全部通过；证据位于 `build-codex-agent/agent-phase-5-slice-66-evidence/`。

限制：仅覆盖 Autopilot `run_flow` 的聊天与 Bridge 入口及 WriteText/SaveData 空目标；不解析上游运行时 `file_path`，不外推到 `step_once`、`resume_flow`、SaveImage、GUI 手动操作或其他插件，不关闭 G5 标准 5/6/7，也不放行阶段 6/8。

## 70. Slice 67：WriteText/SaveData step_once 与 resume_flow 空目标 fail-closed

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。Autopilot 文件目标预检扩展到 `step_once` 与 `resume_flow`，覆盖 Slice66 已保护的 WriteText/SaveData 空 `filePath`。Advisor 确认策略保持不变。

`test_agent_folder_execution_g5::testUnresolvedFileTargetsFailClosedForStepAndResume` 对两插件、两工具和 Chat/Bridge 入口做组合验证。`step_once` 每个调用都在运行前返回 `SIDE_EFFECT_TARGET_UNRESOLVED`，无 pending、模块启动/完成、RunFinished、toolExecuted 或文件变化；Chat 每 callId 保留恰一条结构化 tool error，Bridge 直接返回稳定错误并审计。`resume_flow` 先由真实 RunEngine 在模块 breakpoint 前暂停；拒绝 fail-closed 后 Engine 仍 Paused、原 runId 不变、模块/RunFinished 为零。Advisor 的 step/resume 仍可 pending 并由用户拒绝，暂停状态与空目标文件保持不变。

矩阵只更新 WriteText/SaveData permission 的 reason/evidence；PASS 状态、其他维度状态、69 插件/111 GUI 操作库存和 483 格引用不变。

验证：`cmake --build build-codex-agent -j2` 成功；focused `test_agent_folder_execution_g5` 1/1；获批 loopback 全量 CTest 91/91、0 失败、0 跳过；格式/Python/diff 门禁通过。独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；最终实现独立审核 `/root/slice16_implementation_review` **PASS、无阻断**。13 项源/证据与 4 项二进制 SHA-256 全部通过；证据位于 `build-codex-agent/agent-phase-5-slice-67-evidence/`。

限制：仅证明 Autopilot `step_once`/`resume_flow` 对 WriteText/SaveData 空目标 fail-closed；不解析上游运行时路径，不外推其他插件、GUI 手动入口或静态路径的所有失败语义，不关闭 G5 标准 5/6/7，也不放行阶段 6/8。

## 71. Slice 68：PLCRead 运行时变量驱动 PLCWrite 的确认与单次执行

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。实现扩展真实动态 PLCRead/PLCWrite 插件 fixture：PLCRead 的 loopback FC03 输出 `agent_registers` JSON 数组，经 `image` 流程端口传给 PLCWrite 的 variable 模式。PLCWrite 确认副作用摘要新增 `valueSource=variable:<name>`；现有 immediate 模式摘要字符串保持不变。确认展示 PLCRead 完整读取目标与 PLCWrite 固定写入目标/变量来源名，不展示运行时数组值。

测试以两个本机 loopback peer 提供确定性数据：地址 A=256 返回 `0x1234`、地址 B=257 返回 `0xBEEF`，`readCount=1`。待确认与拒绝均无 TCP 连接、FC03/FC16、模块启动/完成或 RunFinished；pending 期间 PLCRead 地址 A→B 后消费旧 confirmationId 审计为 `expired`，仍无网络和模块副作用；fresh 确认明确显示读取地址 B。批准后同一 `runId` 成功，PLCRead 对 B 恰一条连接/FC03，PLCWrite 恰一条连接/目标正确的 FC16 且值为 `0xBEEF`，两模块各执行一次且成功。

能力矩阵只更新 PLCWrite execution/permission 两格的 reason/evidence，PASS 状态、其他维度状态、69 插件/111 操作库存及 483 引用不变；生成器检查 483/483 引用通过。源码扫描行号变化使 TCPClient 的 sideEffectEvidence 引用同步刷新，不涉及其维度单元格。

验证：全量构建 exit 0；`test_agent_plcwrite_execution_g5` loopback CTest 1/1；全量 loopback CTest 91/91、0 失败、0 跳过；clang-format、矩阵引用检查、Python 编译、`git diff --check` 与源/证据/二进制 SHA-256 校验通过。证据位于 `build-codex-agent/agent-phase-5-slice-68-evidence/`。独立实现审核 `/root/slice16_implementation_review` 最终 **PASS，无剩余发现**。

限制：只验证本机 loopback PLCRead→PLCWrite `QJsonArray` 流程的确认披露、配置漂移失效和一次性 FC16；不连接现场 PLC，不将确认绑定到非确定性实时输入值，不覆盖 variable 模式 `step_once`/`resume_flow`、其他通讯协议或 G5 标准 5/6/7 整体，不放行阶段 6/8。

## 72. Slice 69：PLCWrite 变量来源跨 step_once 保留数据与 runId

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。测试使用 `VariableArraySourceFixture` 产生确定性 `agent_registers=[0x1234]`，通过 `image` 连线接入真实动态 PLCWrite 插件。此前真实 PLCRead peer 同线程同步 step 实测无法服务入站事件，出现读取超时；因此 Slice69 将验证目标限定为 RunEngine 跨步数据传递，真实 PLCRead→PLCWrite loopback 链路仍由 Slice68 证明。

首个 `step_once` 确认被拒绝时，PLCWrite peer 零连接/报文、两个模块均未启动、RunFinished=0。重新确认后第一步只运行源模块一次并成功，RunFinished=1、runId=R，PLCWrite 未连接。第二步确认拒绝后源不重跑、PLCWrite 仍零连接/报文、RunFinished 保持1；批准新确认后不重跑源，真实 PLCWrite 恰连接/发送一条 FC16，值为 `0x1234`，第二步 Agent 与 RunFinished 仍为 runId=R，RunFinished=2。

确认载荷显示 PLCWrite 固定设备目标和 `valueSource=variable:agent_registers`，不泄露运行时值；immediate 模式预览保持兼容。能力矩阵只更新 PLCWrite execution/permission 两格 reason/evidence，状态仍 PASS，库存、其他维度和 483 引用保持不变。全量构建、定向 1/1、全量 91/91、格式/Python/diff 门禁和源/证据/二进制 SHA 校验通过；独立实现审核 `/root/slice16_implementation_review` 最终 **PASS，无阻断**。

## 73. Slice 70：PLCWrite 变量来源经断点 resume_flow 恢复

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。测试工程使用确定性 `VariableArraySourceFixture` 输出 `agent_registers=[0x1234]` 并连接真实动态 PLCWrite；PLCWrite breakpoint 使真实 RunEngine 先执行源模块后暂停。

暂停时源模块恰执行一次、PLCWrite peer 零连接/报文且 RunFinished=0。Autopilot `resume_flow` 拒绝后 Engine 仍 Paused、runId 不变，源不重跑，PLCWrite 和 peer 无副作用。新 confirmationId 获批后同一 runId 恢复；源仍仅一次，PLCWrite 成功一次，peer 恰收到一条 address=8192、slave=7、value=0x1234 的 FC16，RunFinished 恰一次且成功，Engine 回到 Idle。确认摘要显示固定目标和 `valueSource=variable:agent_registers`，不包含运行时值；审计结果为 success。

能力矩阵仅 PLCWrite execution/permission 两格的 reason/evidence 更新，状态维持 PASS；其他单元、69 插件/111 操作库存与 483 个引用不变。全量构建 exit 0；定向 loopback CTest 1/1、全量 loopback CTest 91/91，格式/Python/diff 门禁及源/证据/二进制 SHA 校验通过。证据位于 `build-codex-agent/agent-phase-5-slice-70-evidence/`；独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**。本片不证明真实 PLC 状态，不覆盖真实 PLCRead resume、其他协议或 G5 标准 5/6/7 整体，也不放行阶段 6/8。

## 74. Slice 71：SaveImage step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。在真实 GrabImage 文件输入→SaveImage 输出的隔离工程里新增两条 Agent 路径：两步 `step_once` 分别拒绝与批准；RunEngine `loadProject()` 成功后 `runOnce()` 到 SaveImage breakpoint 并经 `resume_flow` 拒绝与批准。使用不同临时 PNG 目标。

step_once 拒绝时无模块启动、无文件；批准 GrabImage 后 RunFinished=1、记录 runId `R` 且无输出；拒绝 SaveImage 后计数仍 1、目标仍不存在；批准新确认后 SaveImage 恰运行一次，RunFinished=2，两个单步共用 `R`，像素和尺寸与输入一致。resume_flow 拒绝保持 Paused、runId 不变且零文件副作用；批准后恢复同一 runId，RunFinished 恰一次，SaveImage 恰写一张像素/尺寸正确的 PNG，Engine idle。确认载荷绑定具体文件路径，拒绝审计 `rejected/undoable=false`，批准审计 `success/undoable=false`。

能力矩阵只更新 SaveImage execution/permission 两格的 reason/evidence，状态保持 PASS；其他单元及 69 插件/111 操作库存不变，483 引用有效。全量构建 exit 0；定向 CTest 1/1、全量 CTest 91/91 通过，格式/Python/diff 门禁及 SHA 快照通过。证据目录为 `build-codex-agent/agent-phase-5-slice-71-evidence/`；独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**。本片仅覆盖临时 PNG 和这两条入口，不宣称 G5 整体闭合，也不放行阶段 6/8。

## 75. Slice 72：WriteText step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。新增有效静态 `filePath` 的 WriteText `step_once` 与 `resume_flow` 测试，均走 Agent 确认入口。step 拒绝后目标、模块、RunFinished、toolExecuted 均为零；批准后恰一模块执行/一次 RunFinished，runId 一致且文件内容精确。resume 通过 `loadProject()` 后运行到 breakpoint；拒绝保持 Paused、原 runId 与零文件副作用，批准后同一 runId 完成，恰一写入正确内容，审计不可撤销。

能力矩阵仅 WriteText execution/permission 两格 reason/evidence 更新，状态保持 PASS；其余矩阵状态、69 插件/111 操作库存不变，483 个引用有效。全量构建 exit 0；定向 CTest 1/1、全量 CTest 91/91 通过；格式/Python/diff 与 SHA 校验通过。证据目录为 `build-codex-agent/agent-phase-5-slice-72-evidence/`；独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**。本片不涵盖动态 file_path、appendMode 与 G5 整体关闭。

## 76. Slice 73：TCPClient step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。新增独立线程事件循环的 `ThreadedTcpPeer`，避免同步 Agent 单步/恢复阻塞测试主线程 socket 事件；其连接/收件状态加锁，批准用有界收件屏障，不依赖固定 sleep，结束时关闭 peer 并 join。

TCPClient `WriteRead` 的 `step_once` 与 `resume_flow` 均在 pending/拒绝时零连接、零请求字节、零模块执行；批准后 peer 恰连接一次、收到完整请求一次并回传固定响应。模块和 RunFinished 恰成功一次，runId 相同；resume 拒绝保持 Paused 与原 runId，批准后恢复同一 runId。审计拒绝/批准分别为 rejected/success 且不可撤销。

矩阵只更新 TCPClient execution/permission 两格的 reason/evidence，状态保持 PASS；其他维度和 69 插件/111 操作库存不变，483 引用通过。全量构建 exit 0；定向 CTest 1/1、全量 CTest 91/91；格式/Python/diff 与 18 项源/证据、3 项二进制 SHA 校验通过。证据目录为 `build-codex-agent/agent-phase-5-slice-73-evidence/`。独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**。本片不证明真实网络业务完成状态，不关闭 G5 整体。

## 77. Slice 74：TCPServer step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。测试引入线程隔离客户端：在批准前 armed，定时器驱动连接重试且有 4000ms deadline；TCPServer timeout 为 5000ms。客户端连接成功后只发一次请求，收齐固定响应后退出；线程状态用 mutex/condition 同步，结束时等待线程回收。

`step_once` 和 breakpoint `resume_flow` pending/拒绝时 listener 均不可连、模块未启动、RunFinished 为零；resume 拒绝保持 Paused 和原 runId。批准后各恰一次 loopback 连接与精确请求/响应，模块及 RunFinished 恰一次成功；resume 复用暂停 runId。确认摘要限制在 LocalHost/端口/timeout/operation，不泄漏响应内容；审计不可撤销。

矩阵仅 TCPServer execution/permission 两格 reason/evidence 更新，状态保持 PASS；其他维度与 69 插件/111 操作库存不变，483 引用有效。定向 CTest 1/1、全量构建 exit 0、全量 CTest 91/91 通过；格式/Python/diff 和最终 21 项 SHA 门禁通过。独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**。证据目录为 `build-codex-agent/agent-phase-5-slice-74-evidence/`。本片不关闭 G5 整体或阶段 8。

## 78. Slice 75：SerialPort step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。新增 `ThreadedPtyEchoPeer`：worker 在独立线程以 `poll()` 服务 POSIX PTY master，等待固定 UTF-8 请求并回传响应；不依赖测试主线程 Qt timer，状态加锁，停止并 join 有界。

`step_once` 和 breakpoint `resume_flow` 均经 Autopilot 确认。拒绝后没有模块启动、RunFinished 或 PTY 字节；resume 保持 Paused 和原 runId。新确认批准后每条路径恰收一份完整请求并回一份响应，模块/RunFinished 恰成功一次；resume 沿用暂停 runId。以本次 runId 与模块 instanceId 调用 `get_module_output`，UTF-8 内容和 `_bytes` 精确匹配；审计拒绝/批准均不可撤销。

矩阵仅 SerialPort execution/permission 两格 reason/evidence 更新，状态保持 PASS；69 插件/111 操作库存不变，483 引用有效。全量构建 exit 0；定向 CTest 1/1、全量 CTest 91/91；格式/Python/diff 与最终 21 项 SHA 门禁通过。独立实现审核 `/root/slice16_implementation_review` **PASS，无阻断**，核实 PTY worker 有界退出、拒绝零副作用、批准请求/响应恰一次、runId+instanceId 输出绑定及矩阵精确变化。证据目录为 `build-codex-agent/agent-phase-5-slice-75-evidence/`。本片仅为 PTY 证据，不证明物理串口硬件或 G5 整体完成。

## 79. Slice 76：PLCRead step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。本片在真实动态 PLCRead 插件上补充 `step_once` 和断点 `resume_flow`，用线程隔离的 loopback Modbus peer 验证 FC03 请求。peer 的 listener/socket 事件均在线程内处理，避免同步 RunEngine 执行阻塞服务回调；teardown 在 3000 ms deadline 内退出并 join，超时立即使测试失败而不挂住。

两条路径均验证拒绝后零 TCP 连接/FC03 请求/响应、零模块执行；批准后恰一次有效 FC03 与响应。step 返回本次 runId，断点路径沿用暂停 runId；RunFinished 和按 runId+instanceId 读取的寄存器分页/计数与 peer 数据相符。矩阵只更新 PLCRead execution/permission 的 provenance，PASS 状态与其他单元不变。定向测试 1/1、全量 CTest 91/91、全量构建、格式/Python/diff、矩阵引用/精确差异和最终 21 项 SHA 均通过；默认沙箱复跑被 loopback 监听限制拦截，已在获批环境复跑通过。证据目录为 `build-codex-agent/agent-phase-5-slice-76-evidence/`。独立实现审核 `/root/slice16_implementation_review` **PASS，无剩余发现**；无界 teardown 的 Low 发现已改为 3000 ms deadline 并复核通过。本片仅覆盖 loopback fake PLC，不证明真实硬件或 G5 整体闭合。

## 80. Slice 77：PLCCommunicate step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。本片在真实动态 PLCCommunicate 插件上补足 `step_once` 与断点 `resume_flow`，使用独立线程 loopback Modbus peer，逐帧校验固定 FC03 连通性请求与一次性响应。

`step_once` 验证端口 A→B 漂移使旧 confirmationId expired，拒绝新目标零连接，重新确认批准只向 B 发一条请求。`resume_flow` 的 pending 载荷验证专用暂停续跑副作用标记；漂移使旧 id 过期后，恢复暂停快照的原端口，再验证拒绝零连接、批准沿用原 runId 执行原目标一次。两路径均核对 RunFinished、按 runId+instanceId 回读 `plc_connected` 和 response time、审计不可撤销。恢复配置是必要步骤：paused flow preflight 对工程指纹变化 fail-closed。

定向 CTest 1/1、全量构建 exit 0、全量 CTest 91/91、格式/Python/diff、矩阵引用/精确差异与 21 项最终 SHA 校验均通过。能力矩阵只更新 PLCCommunicate execution/permission 两格 provenance，PASS 状态及其他单元不变；69 插件、111 操作、483 引用不变。独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现审核 `/root/slice16_implementation_review` **PASS，无代码阻断**，确认端口漂移过期、恢复暂停快照后 resume 确认、reject 零副作用、批准单次 FC03 和 runId+instanceId 输出核对。本片不证明现场 PLC/网络或 G5 整体完成。

## 81. Slice 78：SamPredict 插件 step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。本片扩展 `test_agent_sam_tools`：动态加载真实 GrabImage 与 SamPredict 插件，使用本地 PNG 和 SamTestServer。

多模块 `step_once` 先执行 GrabImage，再对 SamPredict 上传上游图像请求确认；拒绝零 HTTP，请求批准后沿用 step runId，恰一组 `/set_image` 与 `/predict`。resume 流程显式加载项目并在 SamPredict 断点暂停；服务 URL 漂移使旧 confirmationId 失效，恢复原暂停工程指纹后，拒绝无请求、批准恢复同 runId 并只访问原确认服务一次。两条路径均按 runId+instanceId 回读 Region2D 与 score，回读不增加网络请求。

最终定向 CTest 1/1、全量构建 exit 0、全量 CTest 91/91、格式/Python/diff、483 个引用及矩阵仅两格 provenance 差异校验通过；源/证据/二进制共 22 项 SHA 校验通过。能力矩阵仅更新 SamPredict execution/permission，状态保持 PASS；其他单元不变，69 插件/111 操作/483 格不变。契约独立审核 `/root/slice19_contract_review` **CONTRACT_PASS**；实现独立审核 `/root/slice16_implementation_review` **PASS，无遗留发现**，确认 step/resume 的 Region2D label、score、polygon 与 score key 按 runId+instanceId 回读，查询不重发请求。本片只证明测试服务接口行为，不证明真实 SAM 服务端结果或模型质量。

## 82. Slice 79：GrabImage 相机 step_once 与 resume_flow 确认

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。在 `test_agent_camera_lifecycle_g5` 中复用真实动态 GrabImage 插件和 fake async camera。既有 run_flow 的单次 session 完成退役后，测试等待 session 归零，再通过 `agentConnect()` 建立并验证下一条执行所需 session，避免 RunFinished 先于相机 worker 退役造成的 `CAMERA_OPERATION_BUSY` 竞态。

`step_once` pending 绑定 instance/device/exposure/gain/timeout；拒绝保持预连接 session、连接计数和采集计数不变，批准不建立第二连接，只采一帧并沿用成功 runId，按 runId+instanceId 回读 4×3 图像且无额外采集。`resume_flow` 由真实 `loadProject()`/`runOnce()` 进入 camera breakpoint；cameraId 漂移使旧 confirmationId expired，暂停 runId、session 和设备计数不变；恢复原工程指纹后拒绝仍零采集，批准沿用 paused runId、复用现有 session、仅采一帧并正常退役。两路径核对 RunFinished、模块信号、Agent 结果、审计及 session 清理。现有后续 timeout/迟到帧生命周期断言按新增两次成功 fake capture 更新。

全量构建 exit 0、定向 CTest 1/1、获准 loopback 全量 CTest 91/91 通过。审计生成器补充保留此前矩阵中仍由通用 `NOT_TESTED` fallback 覆盖的既有维度单元；当前显式 PASS/FAIL/BLOCKED 规则优先。前后矩阵完整单元对比仅 GrabImage execution/permission 两格 provenance 更新，两格状态保持 PASS；其余单元、69 插件/111 操作/483 引用不变，引用校验通过。clang-format、Python 编译与 `git diff --check` 通过；21 项源/证据 SHA-256 与 3 项二进制 SHA-256 清单全部校验通过。独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现审核 `/root/slice16_implementation_review` 最终 **PASS、无遗留发现**。首次实现复审指出的缺失 SHA 清单已补齐并复核关闭。本片仅证明 fake async camera 与动态 GrabImage 路径，不关闭 G5 标准 5/6/7，也不放行阶段 6/8。

限制：只证明本机 fake async camera 与动态 GrabImage 插件路径，不证明物理相机、厂商 SDK、曝光精度或图像质量；G5 标准 5/6/7 仍待闭合，不放行阶段 6/8。

## 83. Slice 80：SerialPort 单步与恢复的不确定写入不重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。对照既有 `run_flow` 不确定超时路径，补充 Agent `step_once` 与断点 `resume_flow`：线程隔离 PTY peer 精确接收完整请求且不发送响应；确认拒绝时零字节，批准后每条入口恰一次写入并在 timeout 后报告不确定。

复现时发现 `RunEngine::stepOnce()` 将模块失败统一映射为 `errorCode=-1`，覆盖插件返回的 `SideEffectUncertain`。修复后 RunFinished 保留结构化错误码、失败模块和诊断；step 工具按现有协议返回 `errorCode=STEP_FAILED`、`details.executionAttempted=true` 及 runId。测试核对对应 RunFinished 为 SideEffectUncertain，`get_run_results`/输出回读为 `uncertain` 且 `retryable=false`；重复查询不增加 PTY 请求字节或模块运行次数。resume 的 pending/拒绝保持 Paused 和原 runId，批准后沿用该 runId，并同样保留不确定结果。

能力矩阵仅更新 SerialPort execution/permission 两格 provenance，PASS 状态不变；逐字段前后比对确认其余 481 格、操作清单、库存与计数不变（69 插件/111 操作/483 引用）。全量构建 exit 0；定向 `test_agent_serialport_execution_g5` 1/1、获准 loopback 全量 CTest 91/91 通过；clang-format、Python 编译、矩阵引用检查、矩阵精确差异检查、`git diff --check` 通过。独立实现复审首轮发现的 PTY peer 过早退出可能漏记重试及缺少 RunFinished 超时诊断断言，现已修复；peer 持续 drain 至 RunFinished 后 250ms 静默窗口，两条路径均断言 timeout diagnostics，修订契约经复审再次 CONTRACT_PASS。证据目录为 `build-codex-agent/agent-phase-5-slice-80-evidence/`；33 项源/证据与 3 项二进制 SHA-256 校验通过，独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无剩余发现**。

限制：仅证明本机 POSIX PTY 和动态 SerialPort 插件的 Agent/RunEngine 错误分类与不重放；不证明物理串口或外设最终是否执行命令，不单独关闭 G5 标准 5/6/7 或阶段 8。

## 84. Slice 81：TCPClient 单步与恢复的不确定写入不重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**，包含确认摘要 timeout 最小扩围。新增 TCPClient `WriteRead` step_once 与 breakpoint resume_flow 无响应超时路径：线程隔离 loopback 服务端拒绝时零连接/字节，批准后完整请求恰发送一次且服务端不响应；服务端继续 accept/read 至 RunFinished 后 250ms 静默窗口，重连或重复字节会使断言失败。RunFinished、Agent 查询与输出回读均核对不确定状态、不可重试、runId、failedModule 和 timeout diagnostics。

实现中发现 TCPClient 流程确认摘要遗漏已配置 timeout；`AgentController` 仅在该摘要增加 timeout 字段，插件协议与实际执行不变。此前 `run_flow`/`step_once`/`resume_flow` TCPClient 摘要断言同步更新为 host/port/operation/timeout。

证据目录 `build-codex-agent/agent-phase-5-slice-81-evidence/`。定向 loopback CTest 1/1、全量构建 exit 0、全量 CTest 91/91、clang-format、Python 编译、矩阵引用与精确差异检查、`git diff --check` 均通过；普通沙箱因本机 listener 权限受限的失败日志也已保留。证据清单 23 项源/证据 SHA 与 3 项二进制 SHA 全部校验通过；独立实现审核 `/root/slice16_implementation_review` **PASS，无剩余发现**。本片不证明远端业务最终执行状态，不关闭 G5 标准 5/6/7 整体或阶段 8。

## 85. Slice 82：PLCWrite 单步与恢复的不确定 FC16 不重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。在真实动态 PLCWrite 插件上扩展本机 loopback ModbusWritePeer 测试，覆盖 `step_once` 和 breakpoint `resume_flow`：确认拒绝时连接/FC16/模块执行均为零；批准后 peer 恰收到一条 slave=11、address=8192、value=0x2468 的 FC16 且不 echo。RunFinished 为 `SideEffectUncertain`，failedModule、runId 和插件现有的无响应/响应超时错误文案均核对；Agent 结果与 `plc_write_success/count` 输出为 uncertain、retryable=false。RunFinished 后 peer 持续处理 Qt 事件 250ms，重复连接/报文会使测试失败；resume 拒绝保持原 Paused/runId，批准后沿用该 runId 并回到 Idle。

能力矩阵只更新 PLCWrite execution/permission 两格 provenance，PASS 状态不变；69 插件/111 操作/483 引用和其他维度未变。loopback 定向测试 1/1、全量构建与全量 CTest 91/91 通过。证据目录 `build-codex-agent/agent-phase-5-slice-82-evidence/`；最终复审首轮指出的静默窗口未观察原始分片字节问题已修复：readyRead raw-byte 计数变化会重置 250ms 窗口，完成时要求单连接、单完整帧及累计字节恰等单帧长度。33 项源/证据 SHA 与 5 项二进制 SHA 校验通过；独立实现审核 `/root/slice16_implementation_review` 最终 **PASS，无剩余发现**。本片只证明 loopback fake Modbus 语义，不证明 PLC 已提交写入或现场状态，不关闭 G5 整体或阶段 8。

## 86. Slice 83：direct camera parameter 不确定写入防重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。复用 fake async camera 与真实 CameraManager/Agent direct tool 路径：先连接并读取 binding generation；确认前 gain setter 为零次。fixture setter 将模拟 gain 改为 3.0 后抛异常，批准操作最终报告 `CAMERA_PARAMETER_WRITE_UNCONFIRMED`、`sideEffectStatus=uncertain`、`retryable=false`，读回 before/requested/actual 与 fixture 相符，setter 仅一次。重复 get_camera_operation 返回同一终态；旧 generation 经新的用户确认再次提交时被 `CAMERA_TARGET_CHANGED` 拒绝，不排入新 camera operation，也不再调用 setter。

本 fixture 不代表具体库存驱动，能力矩阵保持原状。定向 `test_agent_camera_lifecycle_g5` 1/1、全量构建 exit 0、全量 CTest 91/91、格式/Python/diff 门禁均通过；矩阵逐字节不变，483 个引用仍有效。21 项源/证据 SHA 与 3 项二进制 SHA 校验通过；独立实现审核 `/root/slice16_implementation_review` 最终 **PASS，无剩余发现**。证据目录为 `build-codex-agent/agent-phase-5-slice-83-evidence/`。不证明物理相机状态或 G5 标准 3/5/6/7 整体完成。

## 87. Slice 84：direct camera connect 部分建立后的不确定结果

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。fake async camera 的 `connect()` 在设置模拟已连接状态后返回 false，复现“调用未成功但设备可能已经连接”。CameraOperationSession 将该操作定为 `CAMERA_CONNECT_UNCONFIRMED`；CameraManager 保留会话并返回 `sideEffectStatus=uncertain`、`connectionState=uncertain`、`retryable=false`。重复轮询保留同一终态；新的 connect 经确认后仍被 `CAMERA_OPERATION_BUSY` 拒绝，驱动 connect 调用不增加。另覆盖 `connect()` 先改变模拟状态再抛异常：返回保留具体 `CAMERA_OPERATION_EXCEPTION`，仍带 uncertain/不可重试状态，不会被普通 false-return 码覆盖。用户再明确确认 disconnect 后，Manager 清理并等待 sessionCount 归零，结果 `hardwareDisconnectConfirmed=true`；之后才能重新连接。

定向 `test_cameramanager` 与 `test_agent_camera_lifecycle_g5` 2/2、全量构建 exit 0、最终 loopback 全量 CTest 91/91。能力矩阵逐字节不变：69 插件、111 操作、483 引用；引用校验仍为 483/483。`hardwareDisconnectConfirmed` 只在 disconnect 终态 success 时为 true，失败或待处理保持 false。clang-format、Python 编译、`git diff --check` 均通过；证据目录 `build-codex-agent/agent-phase-5-slice-84-evidence/` 内 29 项源/证据 SHA 与 4 项二进制 SHA 全部校验通过。独立实现审核 `/root/slice16_implementation_review` **PASS，无剩余发现**。该结果仅来自 fake async camera，不证明实体设备状态，不关闭 G5 标准 5/6/7 整体或阶段 8。

## 88. Slice 85：Agent cycle 有界确认与 Folder Create 目标保护

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无剩余发现**。Agent 的 `run_flow(mode=cycle)` 现在在确认前校验 `maxCycles` 为 1..10 整数；once 不接受该参数，明确 `flowId` 仍先返回 `FLOW_UNAVAILABLE`。确认摘要列出 cycle 次数、100ms 间隔，以及 Folder `Create` 的精确静态路径。Autopilot 下空路径 `Create` 在 run_flow/step_once/resume_flow 三入口确认前失败关闭，Advisor 路径不变。

RunEngine 限定成功迭代数，到上限前不排第 N+1 tick；session 查询给出 maxCycles、completedCycles、lastCompletedRunId 和 lastSuccessfulRunId。成功上限、模块失败、运行前校验失败及用户 stop 的终态/计数都有回归。审查中发现并修复了 bounded cycle 的无效流程会无限重试，以及 cycleStopped 同步观察者读到未提交终态的问题。真实动态 Folder 插件集成测试验证：拒绝零模块启动与零目录创建；批准后恰执行 3 次、只发一次 cycleStopped、没有第 4 次启动；确认载荷绑定目标路径和循环边界。

全量构建 exit 0；定向 CTest `test_runengine`、`test_agentactor`、`test_agent_folder_execution_g5`、`test_agentphase0_repro` 4/4；获准 loopback 环境全量 CTest 91/91。默认受限沙箱全量尝试因本机 loopback 监听权限失败，另有一项 cycle 参数与显式 flowId 错误优先级回归；优先级已修复，最终获准全量结果 91/91。格式、Python 编译、483/483 矩阵引用验证和 `git diff --check` 通过；矩阵 JSON/Markdown 与操作清单哈希不变（69 插件、111 操作、483 引用）。证据位于 `build-codex-agent/agent-phase-5-slice-85-evidence/`，含最终构建、CTest、聚焦测试、格式、引用和 SHA-256 记录。

本片不证明 G5 标准 5/6 的其余不确定副作用路径、不证明真实设备或硬件驱动，不关闭 G5 整体或阶段 8。

## 89. Slice 86：SamPredict 插件上传后超时不确定分类与防重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。SamPredict 插件现在将共享预测器报告的 `uncertainStage` 保留在模块执行边界；仅在预测失败且已上传请求阶段非空时，把普通处理错误映射为 `ExecError::SideEffectUncertain`，并附 `uncertainStage=<stage>` 诊断。请求发送前失败仍保持原失败分类，成功路径不变。

真实动态 GrabImage→SamPredict 流程先走 Autopilot 确认。timeout 路径拒绝时零网络请求/模块启动；批准后，fake server 完整收到第二个 `/predict` 请求体并扣留响应，随后验证 RunFinished 的 failedModule/runId/errorCode/诊断、Agent `get_run_results` 与 `get_module_output` 的 uncertain/不可重试结果。`/set_image` 与 `/predict` 各恰两次（第一次成功、第二次超时），模块启动次数不再增加；重复状态/输出查询及超过 timeout 的等待均不重放，也不提供 Region2D 输出。

验证：全量构建 exit 0；获准 localhost fake-server 环境全量 CTest 91/91；单项动态插件回归与 `test_agent_sam_tools` 通过；clang-format、483/483 矩阵引用检查与 `git diff --check` 通过，能力矩阵及操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-86-evidence/`，包含独立契约审核 `CONTRACT_PASS`、实现复审 `PASS`，以及源/证据与插件/测试二进制 SHA-256 清单并通过校验。本地 fake HTTP 服务不证明生产 SAM 服务端的最终推理状态或模型质量；不关闭 G5 标准 5/6/7 整体或阶段 8。

## 90. Slice 87：Folder Delete 部分失败不确定分类与防重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**。Folder 插件现在记录 Delete 是否实际开始；目标目录存在且 `removeRecursively()` 返回失败时，模块以 `ExecError::SideEffectUncertain` 和 `operation=Delete result=unconfirmed` 诊断结束。不存在目标路径仍保持既有执行成功、`operation_result=false` 语义；Create/Exists/List 不变。

真实动态 Folder Autopilot 测试先用删除 probe 验证 protected 子目录的 owner-write 屏障；屏障无效的平台/进程会恢复权限后 `QSKIP`。独立 writable/protected 子目录保证测试不依赖枚举顺序；批准后 writable 子树消失、protected marker 保留。pending/拒绝时零启动、目录树不变；RunFinished 与 Agent 查询报告 uncertain/不可重试。恢复权限后重复查询并运行 500ms 事件循环静默观察，目录、moduleStarted、RunFinished 数均保持不变。另验证不存在目录仍可成功执行并返回 `operation_result=false`。

首轮实现复审发现 Slice85 bounded-cycle 测试回调使用过长的 QObject context，可能在捕获变量离开作用域后被下一测试触发；已将测试局部 signalContext 作为连接上下文，并确保它先于被捕获变量析构。复审最终 **PASS，无剩余发现**。证据目录 `build-codex-agent/agent-phase-5-slice-87-evidence/`；定向 Folder G5 测试 1/1、全量构建 exit 0、获准 loopback 全量 CTest 91/91、格式、Python、483/483 矩阵引用和 `git diff --check` 均通过；31 项源/证据及 2 项插件/测试二进制 SHA-256 校验通过，能力矩阵和操作清单未变。

限制：权限屏障基于 POSIX 临时目录，权限模型不可用时测试会 `QSKIP`；只证明本机文件系统的 Folder 插件流程，不证明网络挂载/其他平台，不关闭 G5 整体或阶段 8。

## 91. Slice 88：SaveData JSON 写失败不确定分类与防重放

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无 findings**。SaveData JSON 路径现在验证实际写入字节数、flush 和关闭后的 `QFileDevice::error()`。文件打开后短写/写错误/flush 错误返回 `ExecError::SideEffectUncertain`；诊断只含文件格式、写入/预期字节数、flush 状态和文件错误码。文件打开前失败仍为普通 `ExecError::Processing`。成功 JSON、CSV/text 和 JSON append 合并逻辑未改。

动态 SaveData Autopilot 测试把 `/dev/full` 先验证为字符设备，显式设置 `appendMode=false`；确认拒绝零模块启动，批准后 `flushOk=false` 的失败按 uncertain/`retryable=false` 返回。重复 `get_run_results` / `get_module_output` 并进行 500ms 事件循环静默观察，没有第二次模块启动或 RunFinished。另以临时目录作为文件目标，确认 open 前失败仍是 failed/Processing 且没有 uncertain 状态；既有有效 JSON 保存回归继续通过。

全量构建 exit 0；`test_agent_folder_execution_g5` 定向 1/1、获准 localhost loopback 全量 CTest 91/91；clang-format、Python 编译、483/483 矩阵引用、`git diff --check` 均通过。能力矩阵和操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-88-evidence/` 含源/证据和二进制 SHA-256 清单。

限制：`/dev/full` 只模拟 Linux ENOSPC 写 sink，不代表真实磁盘满载、网络文件系统或其他平台；不关闭 G5 标准 5/6/7、阶段 6 或阶段 8。

## 92. Slice 89：文件写入确认显示覆盖/追加/JSON 合并模式

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无剩余 findings**。Autopilot 为已配置的 WriteText 与 SaveData side effect 摘要保留 `file_write:<path>`，并增加非敏感 format/mode：WriteText 显示 text 与 overwrite/append；SaveData 显示实际格式、overwrite/append，JSON 追加明确描述为“目标为已有 JSON object 时按 key 合并，否则覆盖写入”的配置策略。SaveImage 摘要、插件执行语义及能力矩阵库存不变，也不把文本或数据内容放进确认卡。

动态 WriteText 流程核对 overwrite 与 append 两种摘要、不泄露 textContent；确认期间 appendMode 改变后旧 id 审计为 expired，模块未启动且原文件字节不变；拒绝新 append 确认同样不改字节。动态 SaveData 流程核对 JSON overwrite 与条件 merge 摘要；appendMode 漂移使旧 id 过期且零模块启动/文件变化，拒绝 merge 不修改文件；批准 merge 后真实 GrabImage 输入保留已有 `existing_key`、新增 `source_path`、把预置旧 `source_file_name` 更新为输入实际文件名。既有批准 overwrite 路径也继续验证有效 JSON。

全量构建 exit 0；`test_agent_folder_execution_g5` 22/22 用例通过，获准本机 loopback 环境全量 CTest 91/91；`clang-format --dry-run --Werror`、Python 编译、483/483 矩阵引用和 `git diff --check` 通过。矩阵及 GUI 操作清单无改动。证据目录 `build-codex-agent/agent-phase-5-slice-89-evidence/` 保存构建/全量 CTest 日志、审核、契约快照和源/二进制 SHA-256 清单。

限制：只证明本机动态插件和临时文件行为；不验证网络存储及跨平台文件系统，不关闭 G5 标准 5/6/7、阶段 6 或阶段 8。

## 93. Slice 90：SamPredict 完整上传后的取消按不确定副作用报告

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无剩余 findings**。`runSamPredictBatch()` 在取消轮询发现请求已经开始上传时保留对应 `uncertainStage`；SamPredict 插件复用既有 typed `SideEffectUncertain` 映射，未上传时仍不设置不确定阶段。

动态 GrabImage→SamPredict Autopilot 测试先确认拒绝路径零 HTTP/零模块启动；批准后 fake server 完整解析并记录 `/predict` 请求体、扣留响应，再触发 `RunEngine::stop()`。RunFinished 与 Agent 查询绑定相同 runId，失败模块为 SamPredict，错误为 `SideEffectUncertain`，诊断含 `uncertainStage=predict`，状态 uncertain 且不可重试。`/set_image` 与 `/predict` 均只增加一次。重复查询运行状态和区域输出，并进行静默观察，网络请求和模块启动数不变。测试清理最多等待 5 秒，超时保留隔离插件目录，避免因失败而无界卡住。

全量构建 exit 0；定向 `test_agent_sam_tools` 1/1；最终全量 CTest、格式门禁、483/483 矩阵引用与 `git diff --check` 结果保存在 `build-codex-agent/agent-phase-5-slice-90-evidence/`。能力矩阵与操作清单未改。

限制：客户端 stop 不能撤销已上传请求；服务端是否已开始或完成推理仍未知。本机 fake server 只证明本地状态分类与不自动重放，不关闭 G5 标准 5/6/7、阶段 6 或阶段 8。

## 94. Slice 91：文件确认绑定符号链接最终目标

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无阻断发现**。AgentController 为 `run_flow`、`step_once`、`resume_flow` 中 WriteText/SaveData/SaveImage 的静态配置路径保存按 instance ID 解析的最终目标路径。解析跟随相对/绝对符号链接；即使叶文件尚不存在，也解析链接目标的现存父目录并补上剩余路径。确认摘要在链接目标不同于配置路径时显示 `resolved_target`，且 pending 期间始终保留原确认目标。审批前再解析；目标改变后确认失效。

真实动态 WriteText 回归分别验证父目录 symlink 与 dangling 叶 symlink：A 改指 B 后旧确认都审计为 expired，旧 payload 仍显示 A，模块未启动且 A/B 都未写入；新确认显示 B，批准只在 B 创建文件。叶链接名和目标文件名包含首尾空格，保证快照与插件实际路径一致。普通路径的既有摘要断言不变；文件目标快照只应用于流程执行确认，不会让无关 direct-tool 确认失效。

定向 `test_agent_folder_execution_g5` 24/24；全量构建 exit 0；最终 loopback 全量 CTest 91/91；格式门禁、483/483 矩阵引用和 `git diff --check` 通过。能力矩阵及操作清单未变。证据目录为 `build-codex-agent/agent-phase-5-slice-91-evidence/`，含构建/CTest日志、审核、契约快照和源/二进制 SHA 清单。

限制：仅覆盖本机文件符号链接与确认窗口内目标漂移；不证明 junction、hard link、网络文件系统或确认后并发替换安全，批准后的 TOCTOU 明确未关闭，不单独关闭 G5 或放行阶段 6/8。

## 95. Slice 92：有界循环中的 PLCWrite 不确定写入停止

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。新增真实动态 PLCWrite 插件集成回归，使用本机 loopback ModbusWritePeer：Autopilot 循环确认被拒时，TCP 连接、FC16 与模块启动均为零；同一请求重新确认后，第一轮收到有效 echo，第二轮完整收到匹配的 FC16 但不响应。第二轮 RunFinished 保留 `SideEffectUncertain` 与 PLC instance ID，bounded cycle 发出一次 `cycleStopped` 并停止在第二轮，不启动第三轮。

测试以 cycle session ID 查询终态，核对 `status=uncertain`、`isBusy=false`、`retryable=false`、`completedCycles=1`，`lastSuccessfulRunId` 指向第一轮，`lastCompletedRunId/lastIterationRunId` 指向第二轮。第二轮 `plc_write_success` 与 `plc_write_count` 回读均为 uncertain、`reason=side_effect_outcome_unknown`、不可重试且无业务值。peer 同时核对 TCP 连接数、解析帧数和原始字节数；RunFinished 后静默观察 500ms，再次读取 Agent 结果/输出后仍总计仅两次连接、两条 FC16、两次模块启动。

定向 `test_agent_plcwrite_execution_g5` 1/1；全量构建 exit 0；获准本机 loopback 全量 CTest 91/91、0 失败；`clang-format --dry-run --Werror`、Python 编译、483/483 矩阵引用和 `git diff --check` 通过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-92-evidence/` 保存契约/实现审核、构建/CTest/定向测试日志、验收快照及 SHA-256 清单。

限制：loopback peer 仅证明客户端可观测的第二轮请求已发出、响应未知且本地循环停止；不证明真实 PLC 最终是否写入。G5 标准 5/6/7 仍未整体闭合；阶段 6 和阶段 8 未放行。

## 96. Slice 93：直接文件工具确认绑定符号链接最终目标

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。AgentController 现在将待确认 `save_project` 显式路径、空/省略路径的当前工程 filePath 回退，以及 `sam_annotate.sessionPath` 解析成最终文件目标，按唯一 `tool_calls[].id` 冻结；仅对应直接写文件工具或流程入口的模块输出目标参与校验。确认摘要保留配置路径并补充 `resolved_target`，确认期间 payload 保持冻结值。目标漂移或解析失败会使旧批次过期，阻止保存及 SAM 网络请求。

`test_agentcontroller` 覆盖 save_project 显式 symlink 和当前工程默认 filePath symlink：pending 后 link 从 target B 改指 C 时旧 id 过期、无保存信号和文件；新 id 绑定新目标并只保存到新目标。省略 path 与显式空字符串均走默认回退；当前工程存在但 filePath 为空时，两种输入保持批准后 `SAVE_FAILED` 且零文件写入。`test_agent_sam_tools` 使用单 prompt 与本机 fake server：旧 id 过期时 `/set_image`、`/predict` 都为 0，新确认批准后仅新目标保存会话，两个 endpoint 各恰一次。路径名称包含空格，保持原路径不被 trim。

定向 CTest `test_agentcontroller` + `test_agent_sam_tools` 2/2；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败；`clang-format --dry-run --Werror`、Python 编译、483/483 矩阵引用和 `git diff --check` 通过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-93-evidence/` 包含契约/实现审核、构建/全量与定向 CTest 日志、验收快照和 SHA-256 清单。

限制：只验证本机 POSIX symlink 与假 SAM 服务的确认窗口路径；不证明 junction、hard link、网络文件系统或批准后并发替换安全，不关闭 G5 标准 5/6/7、阶段 6 或阶段 8。

## 97. Slice 94：有界循环中的 PLCWrite 成功上限次数

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。新增真实动态 PLCWrite 集成测试：Autopilot 拒绝时连接、FC16、响应、模块启动/完成、RunFinished 和 cycleStopped 均为 0；新的 confirmationId 获批后，`run_flow(mode=cycle,maxCycles=3)` 恰完成三轮，产生三次成功 RunFinished、三条 FC16 请求、三条标准 echo、三次模块启动/完成及一次 cycleStopped，没有第四轮。

测试逐帧校验 FC16 请求 transaction/protocol/unit/function/address/count/value；响应按 Modbus 0x10 回显协议校验 transaction/protocol/unit/function/address/quantity（响应不含寄存器 value）。cycle session 返回 completed、非 busy、completedCycles=3，lastSuccessfulRunId 与 lastCompletedRunId 指向第三轮。该 runId 的 Agent 输出为 `plc_write_success=true`、`plc_write_count=1`。peer 在 550ms 事件静默观察期间持续处理请求，并在重复 Agent 查询后仍只有三次连接/请求/回显。

定向 `test_agent_plcwrite_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败；格式门禁、Python 编译、483/483 矩阵引用与 `git diff --check` 通过。能力矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-94-evidence/` 含契约/实现审核、构建/CTest 日志、验收快照和 SHA-256 清单。

限制：loopback echo 只验证客户端协议处理与有界执行次数，不证明 PLC 的实际写入事务或设备端最终值；G5 标准 5/6/7 仍未整体关闭，阶段 6 与阶段 8 未放行。

## 98. Slice 95：直接 SAM 确认绑定输入图像内容

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。`AgentController` 对每个待确认的 `sam_predict`/`sam_annotate` 按 tool call ID 保存输入图像的符号链接解析目标、可读状态、字节数和流式 SHA-256。确认卡摘要/副作用显示路径、内容摘要与脱敏 SAM origin；批准前快照变化时审计为 expired，并不发 SAM 请求、不写 session。

新增同尺寸同路径替图回归分别覆盖两种直接工具：替图后旧 confirmationId 过期、假服务请求为 0；重新确认后 `/set_image` 和 `/predict` 各恰一次。`sam_annotate` 的 session 只在新确认批准后产生，并验证 `imageDigest` 对应新图解码后的像素内容；它与输入文件原始字节 SHA-256 分属不同摘要口径。

定向 `test_agent_sam_tools` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败/跳过。格式门禁、Python 编译、483/483 矩阵引用、`git diff --check` 与 Slice95 SHA 清单通过。矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-95-evidence/` 保存契约和实现审核、构建/定向/全量 CTest 日志及摘要清单。

限制：仅证明本机可读文件、POSIX symlink 和假 SAM 服务下的确认窗口内容漂移；不可读文件的内部内容变化无法检测。批准校验后到请求/服务读取路径之间仍存在 TOCTOU，不保证服务最终读取的字节与摘要相同；不证明生产 SAM 模型/推理、不关闭 G5 标准 5/6/7，也不放行阶段 6/8。

## 99. Slice 96：TCPClient WriteRead 有界循环恰好发送上限次数

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。Autopilot `run_flow(mode=cycle,maxCycles=3)` 的确认载荷新增不可撤销标签 `不可撤销：Agent Undo 不会回滚外部副作用`，MainWindow 从真实 payload 将它传到确认卡，并在按钮上方显示；端到端 UI 用例创建真实 pending run_flow 并核对文本与布局。

新增真实动态 TCPClient `WriteRead` 集成测试和线程隔离 loopback peer。pending 期间把工程端口由 A 改为 B 后，旧 confirmationId 审计为 expired；A/B 均零连接和字节，模块启动/完成、RunFinished、cycleStopped 和 toolExecuted 全为 0。新确认批准后 B 恰收 3 个独立完整请求并各回一份固定响应，RunFinished/模块执行恰 3 次；cycleStopped 一次，cycle session `completedCycles=3` 且最后成功/完成 runId 均指第三轮。末轮 `agent_reply` 与固定响应完全相等。

peer 持续工作线程服务并在 cycleStopped 后静默观察 550ms；累计原始字节严格等于 `3 × request.size()`，已回复 socket 再收字节即判无效。重复查询 session/output 后连接、请求、响应仍恰 3，不存在第四轮。矩阵及 GUI 操作清单未改；本片只为 G5 标准 5/6 补充一个 TCPClient 成功循环切片，不代表它们整体闭合。

定向 `test_agent_tcpclient_execution_g5` 与 `test_mainwindow` 2/2；全量构建 exit 0；获准 loopback 全量 CTest 91/91、0 失败/跳过。格式、Python、483/483 矩阵引用、`git diff --check` 及最终 SHA 清单通过。证据目录 `build-codex-agent/agent-phase-5-slice-96-evidence/` 保存构建、定向/全量测试和审核证据。

限制：loopback peer 只证明 TCPClient 发出的字节和本地成功响应次数，不证明远端业务事务；无响应/部分响应仍沿用 Slice81 的单次 no-retry 证据，本片不扩大其语义；真实网络、G5 标准 5/6/7 全部闭合、阶段 6 与阶段 8 均未放行。

## 100. Slice 97：SerialPort WriteRead 有界循环恰好执行上限次数

契约 `本地逐切片契约（未纳入提交）` 经独立契约审核 `/root/slice19_contract_review` **CONTRACT_PASS**；实现经 `/root/slice16_implementation_review` **PASS，无剩余发现**。新增真实动态 SerialPort 插件 + POSIX PTY worker peer 的 Agent Autopilot 集成测试，覆盖确认拒绝零写入、`portName` 漂移使旧确认 expired 且无收发/模块启动/完成/RunFinished/cycleStopped/toolExecuted，随后新确认批准后成功完成有界循环。

批准的 `WriteRead` cycle 恰好交换 3 个完整 UTF-8 请求/响应帧；peer 累计原始请求字节严格为 `3 × request.size()`，重复查询输出和 session、cycleStopped 后 550ms 静默观察都没有额外字节或第四轮。模块启动、完成和成功 RunFinished 各 3 次，cycleStopped 一次；session `maxCycles=3`、`completedCycles=3` 且末轮 runId 正确；末轮 `serial_data` 与 `_bytes` 和固定响应及其 UTF-8 长度一致。确认载荷披露 PTY 目标、串口格式、循环上限和不可撤销标签，不含请求/响应业务文本；MainWindow 的真实 pending `run_flow` 确认路径及不可撤销标签 UI 由 Slice96 的端到端用例复用验证。PTY worker 的 HUP/EIO 空转路径增加 5ms 退避，并以 5 秒上限 join，只有线程退出后才释放 PTY fd。

验证：定向 `test_agent_serialport_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 编译、483/483 矩阵引用检查和 `git diff --check` 均通过。证据目录 `build-codex-agent/agent-phase-5-slice-97-evidence/` 包含构建、定向/全量测试、独立审核、矩阵引用检查和 SHA-256 清单；矩阵及 GUI 操作清单未改。

限制：PTY 只证明本机动态插件的串口字节协议与 RunEngine 有界循环，不证明真实串口适配器、设备响应语义或外部事务最终状态；本片仅补充 G5 标准 5/6 的一个串口成功循环场景，标准 5/6/7 仍未整体闭合，不放行阶段 6 或阶段 8。

## 101. Slice 98：WriteText append 有界循环恰好追加三次

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；实现经 `/root/slice16_implementation_review` **最终 PASS，无剩余发现**。实现复审指出 terminal session busy 字段断言应使用 `isBusy`；已修正后重新构建并复跑定向用例 1/1。

新增真实动态 WriteText 插件测试，使用临时普通文件与 Linux/POSIX `QIODevice::Text` 语义。Autopilot 拒绝时原有 sentinel 文件字节不变，模块/RunFinished/cycleStopped/toolExecuted 均为 0；待确认后将 `appendMode` 改为 false，旧 confirmationId 变为 expired，依然零写入、零运行信号。恢复配置后新确认批准，cycle `maxCycles=3` 恰完成三轮；目标文件原始字节严格等于 sentinel 加三次固定 UTF-8 payload（payload 显式含结尾 LF），没有第四次追加。模块启动/完成与成功 RunFinished 各 3 次、cycleStopped 一次；session 终态 completed、非 busy、completedCycles=3，末轮 runId 与最后成功 runId 一致。末轮输出分别核对 `write_result=true`、`write_path`、`write_length=textContent.length()`；该字符数断言与 UTF-8 文件字节断言分离。重复查询后静默 550ms 文件和运行计数保持不变；确认载荷披露路径、append 模式、循环次数和不可撤销标签，不泄露文本内容。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；格式门禁、Python 编译、483/483 矩阵引用及 `git diff --check` 通过。首轮受限沙箱全量运行因 localhost listener 创建失败有 11 项环境性失败（原始日志及明细保留在证据目录），随后获准 loopback 全量复跑 91/91 全绿。矩阵和 GUI 操作清单未变。证据目录 `build-codex-agent/agent-phase-5-slice-98-evidence/` 保存两轮全量日志、构建/定向日志、独立审核、矩阵检查与 SHA 清单。

限制：只证明 Linux/POSIX 本机普通文件系统下 WriteText append 的拒绝、配置漂移和有界成功循环，不证明掉电原子性、网络文件系统、磁盘故障后的最终状态或其他平台的文本换行语义；不单独关闭 G5 标准 5/6/7，不放行阶段 6 或 8。

## 102. Slice 99：GrabImage(Path) → SamPredict 上游文件绑定确认

Slice99 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。Autopilot `run_flow(mode=once)` 的上游图像文件现按解析目标、可读状态、字节数和完整 SHA-256 绑定 confirmationId。候选源只包括启用数据图中沿启用连接可达启用 `SamPredict.image` 输入的 `GrabImage(Path)`；确认摘要显示实际输入路径、必要时的解析目标、字节数和 SHA-256 前缀，SamPredict 端点只显示安全 origin。

新增动态插件 + 本地 fake HTTP 测试：确认载荷带凭据、path、query、fragment 的服务 URL 时，序列化 `context.sideEffects` 只含 URL origin，不含任何敏感 URL 字段或图像内容。测试夹具还含一个启用但未连接、一个禁用的 GrabImage(Path)；pending 后只改变这两份无关图像文件，保持项目配置及实际输入字节不变，原 confirmationId 仍可批准成功，且两条无关路径未出现在摘要中。随后在实际输入路径覆盖同尺寸红图，旧 confirmationId 变为 expired，GrabImage/SamPredict 模块启动、RunFinished 和 fake HTTP 请求均为 0；新确认摘要反映新图 hash，批准后 fake server 在 `/set_image` 处理时解码到 64×48 红图，恰收到一次 `/set_image` 与一次 `/predict`。

验证：定向 `test_agent_sam_tools` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；格式门禁、Python 编译、483/483 矩阵引用、`git diff --check` 及 Slice99 SHA-256 清单通过。能力矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-99-evidence/` 保存完整构建、定向/全量测试、两项独立审核和 G5 标准5/6覆盖审计。

限制：仅覆盖直接已验证的 `run_flow(mode=once)` 路径，不覆盖 `step_once/resume_flow` 的输入文件快照；文件确认重校验之后、GrabImage 实际打开之前仍有并发 TOCTOU；fake SAM 不证明生产部署或模型质量。G5 标准5/6/7 仍未整体闭合，阶段6/8不放行。覆盖审计列出的后续软件缺口包括 TCPClient bounded cycle 第二轮已发送但无响应时不启动第三轮，以及 WriteText/SaveImage 写入开始后失败的 uncertain 分类与不重放证据；真实设备继续归阶段8。

## 103. Slice 100：TCPClient 有界循环第二轮不确定后停止且不重放

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；实现经 `/root/slice16_implementation_review` **PASS，无发现**。扩展线程隔离 TCP cycle peer，第一轮完整收到固定请求并回固定响应，第二轮完整收到相同请求后扣留响应；peer 保持监听，能看到任何第三连接。

Autopilot `run_flow(mode=cycle,maxCycles=3)` 拒绝后连接/字节/模块/RunFinished/cycleStopped 均为 0。批准后第一轮 RunFinished 成功；第二轮 `RunResult.errorCode=SideEffectUncertain` 且 failedModule 正确，cycleStopped 后不启动第三轮。Peer 精确观察两条连接、两条完整请求、两份请求字节以及一份响应。cycle session 为 uncertain、非 busy、`maxCycles=3`、`completedCycles=1`；lastSuccessfulRunId 指向第一轮，lastCompletedRunId/lastIterationRunId 指向第二轮。第二轮输出和 session 查询为 uncertain、`retryable=false` 且无伪成功值；550ms 静默观察及重复查询期间连接、字节、模块和 RunFinished 计数不变。

定向 `test_agent_tcpclient_execution_g5` 1/1；全量构建 exit 0；全量 CTest、格式/Python/矩阵引用、差异检查和 SHA 清单结果归档于 `build-codex-agent/agent-phase-5-slice-100-evidence/`。这只证明 loopback TCPClient 客户端行为，不证明远端业务事务；不能与 PLCWrite 的独立不确定循环证据互相外推，也不关闭 G5 标准5/6/7或放行阶段6/8。


## 104. Slice 101：WriteText open 后写入失败归类不确定且 cycle 不重放

Slice101 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **最终 PASS，无剩余发现**。WriteText 在目标成功打开后标记写入结果待确认；写出后显式 flush QTextStream，检查流状态、QFile flush 和错误状态，并在 close 后再次读取 QFile 错误。任何已开始写入后的异常都以 `ExecError::SideEffectUncertain` 返回；目标打开失败仍是普通确定失败，保留既有测试覆盖。

新增 Linux `/dev/full` 动态 WriteText + Agent bounded-cycle 集成测试，使用非空固定文本、overwrite、`maxCycles=3`。拒绝时零模块启动/完成、RunFinished、cycleStopped 与 run_flow 执行。批准后仅有一轮 WriteText 尝试并以 `SideEffectUncertain` 失败；cycle session `completedCycles=0`、终态 uncertain、非 busy，失败 runId 一致且没有 lastSuccessfulRunId。Agent run 查询报告 uncertain/sideEffectStatus uncertain/retryable=false 与结构化错误码；模块输出报告 `side_effect_outcome_unknown`、retryable=false 且无 value。重复查询和 550ms 静默窗口未启动额外轮次或产生额外运行信号。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；独立实现审核、格式门禁、Python 编译、483/483 矩阵引用、`git diff --check` 和 SHA-256 清单通过。矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-101-evidence/` 保存契约/审核、完整构建、定向与全量测试、矩阵检查、二进制摘要和源码摘要。

限制：`/dev/full` 证明当前 Linux Qt/QFile 路径能检测该写入/flush 失败并不重放；它没有单独模拟“只在 close 才报错”的故障，也不证明部分持久化字节、真实磁盘故障、掉电、网络文件系统或其他平台语义。本片只覆盖 WriteText 失败边界，不覆盖 SaveImage 或 SaveData CSV/text，不关闭 G5 标准 5/6/7，不放行阶段 6/8。


## 105. Slice 102：SaveData CSV/text 写入失败归类不确定且 cycle 不重放

Slice102 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无发现**。SaveData CSV/text 成功打开目标后即标记写入结果待确认；完成 QTextStream flush 后检查流状态、QFile flush 与关闭前后错误，所有状态正常才报告成功并清除标记。失败诊断只记录格式、状态与输入 key 名，不记录输入值；`execute()` 将打开后失败转为 `ExecError::SideEffectUncertain`。JSON 写入实现保持现状，open failure 不会误标 uncertain。

新增 Linux `/dev/full` 动态插件 + Agent 测试，真实 GrabImage(Path) → SaveData.image 流程为非空数据输入提供 `source_path` 和 `source_file_name`；断言这两个 key 进入失败诊断。CSV 与 text 各自覆盖确认拒绝零执行、批准后恰一次失败轮、uncertain 输出和 `maxCycles=3` cycle 不再启动后续轮次；重复 run/session/module-output 查询及 550ms 静默观察无重放。每个格式另将已存在目录作为输出目标，批准后验证 open failure 仍为 `ExecError::Processing` 和 Agent `failed`，没有 `sideEffectStatus=uncertain`。Slice88 JSON 不确定及 open failure 回归同时保留。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；独立契约/实现审核、格式门禁、Python 编译、483/483 矩阵引用、`git diff --check` 与 SHA-256 清单通过。矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-102-evidence/` 保存契约、审核、构建、定向/全量测试及摘要。

限制：`/dev/full` 只证明当前 Linux Qt/QFile CSV/text 写入路径能检测写入失败并防止 Agent 重放；不证明部分落盘字节、close-only 单独故障、掉电、网络文件系统或其他平台。本片不覆盖 SaveImage、所有写入插件或目标替换 TOCTOU；G5 标准 5/6/7 仍未整体闭合，不放行阶段 6/8。


## 106. Slice 103：SaveImage 编码后写入失败归类不确定且 cycle 不重放

Slice103 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。当前 Linux/OpenCV 分支把图像编码与文件写出分开：完整编码先在内存完成，之后 QFile 打开输出目标；成功 open 后开始跟踪未确认副作用，检查完整字节数、flush、关闭前后错误，全部成功才返回成功。写入阶段失败通过 `ExecError::SideEffectUncertain` 传播，open failure 仍为普通 Processing。保留旧格式语义：带扩展名路径仍由扩展名选编码器，配置 `format` 沿用 JPEG/PNG 编码参数选择，TIFF/BMP 继续用 OpenCV 默认参数；无扩展名 `/dev/full` fixture 明确使用 `format=png`。非 OpenCV fallback 的 `QImage::save(filePath)` 保持原实现。

新增动态 GrabImage(Path) → SaveImage 测试：pending 拒绝时零模块执行；批准后仅一轮 PNG 编码/写入尝试，`/dev/full` 写入错误以 SideEffectUncertain 结束 bounded cycle，不出现第二/第三轮。失败诊断含格式、预期/实际字节数与 QFile/flush 状态，不含像素内容。run/session 查询和 module-output 均断言 uncertain、`retryable=false`、无伪成功 value；550ms 静默查询不重放。附加有效 PNG 输入到已存在目录的 open-failure 子用例，断言 `ExecError::Processing`、Agent `failed` 且没有 `sideEffectStatus=uncertain`。原成功 PNG 文件重载尺寸/像素断言继续通过。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；独立契约/实现审核、格式、Python、483/483 矩阵引用、`git diff --check` 及 SHA-256 清单通过。矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-103-evidence/` 保存构建、定向/全量测试、审核、矩阵检查与二进制/源码摘要。

限制：只证明当前 Linux/OpenCV + QFile 与 `/dev/full` 的错误分类和 Agent 不重放，不证明部分持久化字节、掉电、网络文件系统、close-only 故障或真实存储状态；非 OpenCV fallback 未覆盖，需另行闭环。目标路径仍有批准到 open 之间的 TOCTOU；G5 标准 5/6/7 仍未整体闭合，不放行阶段 6/8。


## 107. Slice 104：Agent 插件生命周期确认后恰执行一次

Slice104 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无发现**。复用现有隔离扫描的 ABI v2 `AgentSchemaCacheFixture`，新增 `test_agent_plugin_g5` 的 AgentController 端到端测试：Autopilot 发起 load 时 pending/拒绝前后 fixture 保持未加载，生命周期成功/失败信号均为零；新 confirmationId pending 后，提交旧被拒 ID 被拒绝且仍保留当前 pending 批次。批准当前 ID 后 Agent 结果为 loaded、manager 状态 loaded、`pluginLoaded` 恰一次且无 load failure。随后 unload 同样覆盖拒绝零变化、新 pending 下旧 ID 不消费批次、批准后 Agent 结果 unloaded、状态 unloaded、`pluginUnloaded` 恰一次。重复批准已消费 ID 被拒，最后 fixture 卸载且 Engine 空闲。

验证：定向 `test_agent_plugin_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；ABI mismatch、interface-version mismatch 与 schema unload/reload 原测试通过；格式/Python/矩阵引用、`git diff --check` 和 SHA-256 清单通过。能力矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-104-evidence/` 保存构建、定向/全量日志、独立审核与摘要。

限制：只证明一个隔离 ABI v2 fixture 经 Controller 确认后的真实 load/unload 成功路径；不代表所有业务插件分别经过 Agent 批准操作，不验证确认窗口内 PluginManager registry、二进制或 loaded 状态漂移。本 slice 只关闭这一实际批准路径证据缺口，G5 标准5/6/7仍未整体闭合，不放行阶段6/8。


## 108. Slice 105：插件生命周期确认绑定目标摘要并对不可哈希快照 fail closed

Slice105 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **最终 PASS，无剩余发现**。`load_module_plugin` / `unload_module_plugin` 的 pending 快照现在绑定 PluginManager 扫描身份、metadata 完整路径/大小/SHA-256、候选动态库集合完整路径/大小/SHA-256 和 loaded 状态。预览显示 metadata 与二进制完整路径、SHA-256 前缀及 loaded 状态；metadata 或任一候选库无法读取并计算摘要时，即使 pending 与确认时都不可哈希，批准也会过期并 fail closed。加载状态或可哈希目标变化同样使旧 confirmationId 过期。

测试在隔离 `AgentSchemaCacheFixture` 上覆盖：pending 后原子替换为接口版本不匹配库时旧 load 确认过期，恢复原库后新 ID 可正常加载；外部卸载导致 pending unload 过期且不重复卸载；metadata 不可哈希时 zero Agent 调用；Linux 下先最多读取 1 字节预检 `/proc/kcore`，若不可读则在正常 pending 后将候选库改为该不可读目标，确认过期，若可读则跳过该候选库子用例且不启动完整读取。所有 fixture 文件都有恢复清理。

验证：全量构建 exit 0；定向 `test_agent_plugin_g5` 1/1；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 编译、483/483 矩阵引用、`git diff --check` 及 SHA-256 清单校验通过。矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-105-evidence/` 保存构建、定向/全量测试、两项独立审核及哈希清单。

限制：Linux `/proc/kcore` 子用例仅在安全预检确认不可读时运行；本片不覆盖 PluginManager registry 扫描结果在确认窗口内变化，也不消除快照重算后到 `QPluginLoader::load/unload` 之间的 TOCTOU，不证明第三方插件可信、真实 SDK/硬件或 G5 整体闭合；阶段 6/8 仍未放行。


## 109. Slice 106：确认期间重新扫描同名插件使旧目标批准过期

Slice106 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。新增 `testPluginLifecycleConfirmationExpiresAfterRegistryRescan`：Agent 对隔离 `AgentSchemaCacheFixture` 建立 load pending 后，测试向 PluginManager 增加第二个临时根并同步重扫；同名插件 ID、metadata 路径及候选库切换到另一目标。批准旧 confirmationId 后，确认过期、Agent 回到 Idle，插件仍未加载，且 `toolExecuted`、`pluginLoaded`、`pluginLoadFailed` 均为零。删除临时根并重扫后，原 ID、metadata 路径和未加载状态恢复。

验证：定向 `test_agent_plugin_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；格式、Python 编译、483/483 矩阵引用、`git diff --check` 与证据 SHA-256 校验通过。矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-106-evidence/` 保存构建、定向/全量测试、契约/实现审核和清单。

限制：PluginManager 没有移除搜索路径的接口；测试删除临时根后重扫恢复注册项，但该已删除路径保留在本测试进程的搜索路径列表中。用例是 `test_agent_plugin_g5` 独立 CTest 进程中最后一个更改注册表的测试，不影响其他测试进程或用户配置。本片不解决并发扫描与实际 load 之间的 TOCTOU，也不关闭 G5 标准 5/6/7。

## 110. Slice 107：相机确认绑定 IP 并覆盖聊天确认链路

Slice107 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **最终 PASS，无剩余发现**。CameraManager lifecycle snapshot 将 `ipAddress` 与 found、deviceId、owner、name/serial、连接状态及设备/会话/参数代次共同绑定；Autopilot 预览展示 IP。connect pending 期间单独改变 IP 并等待异步重扫，除 IP 外其余快照字段保持相同，旧确认过期；Manager expected-target admission 返回 `CAMERA_TARGET_CHANGED`、requestId=0，工厂、驱动和 Agent tool 均无调用。恢复原 IP 并重扫后，快照精确恢复。

新增 scripted LLM 聊天端到端覆盖：通过 `sendUserMessage` 产生 connect confirmation，pending 后改变 IP；旧确认过期，聊天路径没有 connect operation/tool execution。恢复 IP 后 fresh 聊天确认批准，Manager 恰启动并完成一次连接，LLM 收到工具结果后回到 Idle。原 Bridge/手动确认与 Manager session drift 用例继续通过。

验证：全量构建 exit 0；定向 `test_agent_camera_lifecycle_g5`、`test_agentpermissions`、`test_cameramanager` 3/3；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用校验、`git diff --check` 和证据 SHA-256 清单通过。矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-107-evidence/` 保存构建、定向/全量测试、独立审核及校验记录。

限制：测试使用 fake async camera，不证明厂商驱动所报告 IP/序列号的真实性、物理拔插竞态、真实 SDK/硬件或 DirectShow/Hikvision/V4L2 实物。Manager 检查到请求排队之间仍以 CameraManager 的单线程同步入口为原子边界；本片不关闭 G5 标准5/6/7，不放行阶段6或8。

## 111. Slice 108：Folder Delete 确认绑定符号链接解析目标

Slice108 契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断**。流程确认快照现在对 `Folder(operation=Delete)` 按模块 ID 保存解析后的 `folderPath` 目标；确认摘要保留配置路径，并在解析目标不同或不可用时明确显示 `resolved_target`。其它 Folder 操作和原有普通绝对路径摘要未改变。

新增真实动态 Folder 插件回归：同一配置路径 `symlink/target` 起初解析到 A/target，确认后以同目录 POSIX `rename()` 原子覆盖链接使其改指 B。批准旧 ID 后审计 expired，Agent tool、模块启动/完成、RunFinished 均为 0，A/B sentinel 字节不变，Engine idle。fresh confirmation 显示 B/target，批准后 Folder 恰运行一次、RunResult 成功，只删除 B/target；A marker 保留。`operation_result` 与 `folder_path` 均按相同 runId 经 Agent 输出查询回读。测试在不支持 symlink 或原子替换时清理后 skip。

验证：全量构建 exit 0；定向 `test_agent_folder_execution_g5` 1/1；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用、`git diff --check` 与 SHA-256 清单通过。独立契约/实现审核均通过，矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-108-evidence/` 保存完整日志、审核记录和清单。

限制：解析目标在批准前复核，但批准后到 `QDir::removeRecursively()` 之间仍有文件系统 TOCTOU；本片不证明该并发窗口安全，不外推到其它平台/文件系统或其它插件，也不关闭 G5 标准5/6/7、不放行阶段6/8。

## 112. Slice 109：step/resume 确认绑定 SamPredict 实际图像输入

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现审核 `/root/slice16_implementation_review` **PASS，无剩余发现**。确认目标沿 image 端口追溯至可达 `GrabImage(Path)`；禁用图像处理节点依照 RunEngine 的 image bypass 语义继续追溯，禁用或未连接的 GrabImage 源不进入快照。`run_flow(mode=once)` 保持文件原始字节摘要，`run_flow(mode=cycle)` 不纳入缓存图像路径。

`step_once`/`resume_flow` 在源 GrabImage 尚未执行时绑定文件解析目标、可读状态、字节数与 SHA-256；SamPredict.image 输入已物化时，从 RunEngine 当前 step/paused run 的实际端口输入生成 RGBA8888 像素摘要（宽高前缀、按行哈希），磁盘文件随后换色不会改变缓存输入。如果 GrabImage 已执行但 CropImage 等变换尚未执行，摘要显示 `pipeline_input_pending`，不以磁盘图像冒充未来输入。执行上下文快照采用五态结果；上下文捕获期间失效将产生不可批准的 invalidated 快照并在确认复核时 fail closed。生命周期锁、运行节点输出和模块图读取不以相反顺序嵌套，并在读取后复核上下文代次。

真实动态 GrabImage/CropImage/SamPredict 插件与本地 `SamTestServer` 测试覆盖：新单步尚未读源文件时的旧确认过期与新文件颜色；直接链和 CropImage 变换链的 step 缓存输入摘要/尺寸/像素与服务实收一致；GrabImage→CropImage→SamPredict 的 paused resume 在磁盘换色后仍发送同一运行缓存裁剪图；disabled CropImage 的配置摘要保留文件来源，GrabImage/SamPredict 各运行一次、Crop 恰跳过一次且 fake SAM 收到旁路原图。既有 URL 安全 origin、不可达源过滤、`run_flow(mode=once)` 和结果 runId 回读继续通过。

验证：全量构建 exit 0；定向 `test_agent_sam_tools` 1/1；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用、66 模块 + 3 相机库存无增删、`git diff --check` 及证据 SHA-256 清单通过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase-5-slice-109-evidence/` 保存构建、定向/全量日志、两项独立审核和清单。

限制：测试只证明本机 Qt/OpenCV、临时 PNG、动态插件和 fake SAM；不证明生产 SAM 服务/模型。文件确认复核至 GrabImage 实际打开之间仍有 TOCTOU。本 slice 不关闭 G5 标准 5/6/7，不放行阶段 6 或 8。

## 113. Slice 110：有界 cycle 确认绑定 SamPredict 上游文件输入

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。Autopilot `run_flow(mode=cycle,maxCycles>0)` 现在与 `run_flow(mode=once)`、step/resume 一样，将可达 `GrabImage(Path) → SamPredict.image` 上游文件摘要纳入待确认快照，并在批准前复核。复用 Slice109 的 disabled image bypass、不可达源过滤及路径/字节数/SHA-256 规则；不改变 cycle 调度与重试。

动态 GrabImage/SamPredict 插件与本地 fake SAM 回归覆盖 bounded `maxCycles=2`：pending 后同路径同尺寸换图，旧 ID expired，cycle session 为空，Engine idle、模块/RunFinished/cycleStopped/tool 和 HTTP 均零副作用；fresh 摘要包含新图 hash，拒绝无副作用。再次 fresh confirmation 批准后恰完成两轮，GrabImage/SamPredict 启动/完成与 RunFinished 各两次，cycleStopped 一次；fake SAM 的两份 `/set_image` 均解码为 64×48 红图，`/predict` 两次，末轮 `lastSuccessfulRunId` 可读回 score=0.97。cycleStopped 后静默观察 350ms，session 仍 completed 且无第三轮。测试只证明批准时快照：cycle 启动后每轮仍按现有路径读取，文件可逐轮变化；文件复核到每轮实际打开之间保留 TOCTOU。

验证：全量构建 exit 0；定向 `test_agent_sam_tools` 1/1；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用、`git diff --check` 和 Slice110 SHA-256 清单通过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice110-evidence/` 保存构建、定向/全量测试、契约/实现审核及清单。

限制：只证明 Linux Qt/OpenCV 临时 PNG、隔离动态插件和 fake SAM 的有界 cycle 文件输入确认；不证明生产 SAM 或真实硬件。批准后文件可逐轮变化且仍有文件系统 TOCTOU。本 slice 不关闭 G5 标准 5/6/7，不放行阶段 6/8。

## 114. Slice 111：确认工程切换时的隐式保存目标

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。Autopilot 的 Bridge 与 LLM 入口现按工具参数检查：只有当前工程 dirty、`onUnsaved=save` 且旧保存路径非空/可解析时，`create_project`/`open_project` 才进入确认门。pending context 按实际执行顺序先披露旧工程 `file_write`，再披露 project switch；确认快照绑定当前工程内容、配置路径和解析后的目标。确认期间 symlink 解析目标漂移会使旧 ID expired。空/不可解析目标保留既有 `SAVE_FAILED`，不生成虚假目标确认。

`test_agentcontroller` 新增 Bridge 与 LLM 回归：Bridge create 拒绝时文件与工程零变化、无合成聊天 tool message；确认期间把旧工程 symlink 从 A 原子改指 B 后，旧确认 expired、A/B 字节不变、无 save/switch。fresh confirmation 批准后 `projectSaved` 先于 `projectCreated`，B 的 v3 JSON 含窗口前修改，A 保持原字节且没有额外 `.v2.bak`。另覆盖 clean+save 不新增确认、dirty+cancel 保持 `PROJECT_UNSAVED`、dirty+discard 仍显式丢弃，以及空路径和自引用 symlink 的 `SAVE_FAILED`/零切换。LLM `open_project + get_flow_state` 混合批次在批准前整批不执行；批准后 `projectSaved` 先于 `projectOpened`，每个原始 callId 恰有一条 tool result，mock 终结响应后 Agent 回到 Idle。

验证：定向 `test_agentcontroller`、`test_agent_folder_execution_g5`、`test_agent_sam_tools` 3/3；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用、`git diff --check` 与 SHA-256 清单通过。能力矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice111-evidence/` 保存构建、定向/全量测试、契约/实现审核及清单。

限制：只证明本机普通临时文件和 POSIX symlink 确认窗口；批准前重校验到保存打开文件之间仍有 TOCTOU。候选 `open_project` 文件自身内容在确认窗口内被外部替换，以及旧项目保存成功但候选工程随后加载失败的事务顺序，不由本片解决。本 slice 不关闭 G5 标准 5/6/7，不放行阶段 6/8。

## 115. Slice 112：Folder 有界 cycle 部分删除失败后不重放

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。新增真实动态 Folder 插件回归 `testFolderDeletePartialFailureBoundedCycleStopsWithoutReplay`：POSIX 权限屏障令递归删除先删掉可写子树，再在受保护子树失败。pending 拒绝时无模块启动、run/cycle 信号或删除；批准 `run_flow(mode=cycle,maxCycles=3)` 后仅启动/完成一次 Folder，结果为 `SideEffectUncertain`，cycle session 终态 `uncertain`、不可重试且 `completedCycles=0`。恢复目录权限后驱动 Qt event loop 静默观察 550ms，目录和保护标记保留，模块/RunFinished/cycleStopped/tool 启动计数不增加，Engine idle。新增 `FolderTestStateRestore` 在权限恢复前先 stop 并等待 Engine idle；超时则保留临时插件/目标目录，避免失败断言路径破坏执行对象。

验证：定向 `test_agent_folder_execution_g5` 1/1；完整构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用、`git diff --check` 均通过。当前环境权限屏障建立成功，新增用例未跳过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice112-evidence/` 保存构建、定向/全量测试、独立审核及校验记录。

限制：权限屏障依赖本机 POSIX 临时目录权限语义；不外推到其他平台、网络文件系统或其他插件。只补 Folder bounded-cycle 部分删除失败的“不重放”证据，不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或 8。

## 116. Slice 113：open_project 确认绑定候选工程文件

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无剩余发现**。每个 `open_project` 候选快照现在按批内位置绑定配置路径、canonical target、可读状态、完整字节数和 SHA-256；确认摘要显示候选路径、解析目标及 SHA 前缀。批准时重算所有候选快照，任一候选内容或 symlink 目标漂移会使整批过期；快照键不依赖 LLM callId，空/重复 ID 不会互相覆盖。一个批次包含多个 `open_project(onUnsaved=save)` 时，共享旧 dirty 工程的隐式保存只披露一次。

`test_agentcontroller` 新增三条回归：Bridge 候选原子内容替换使旧 ID expired，旧工程不保存/不切换且不产生合成对话工具消息，fresh Bridge 确认只加载新内容；稳定 missing path 批准后仍先保存 dirty V3 工程一次，再返回 `OPEN_FAILED`、保留原工程对象且不发 projectOpened；LLM 三次独立批次分别使用新 callId，覆盖双候选 + `get_flow_state`，第二候选 symlink 从 B 改指同内容 C 使批次 stale，下一轮 C 内容原子替换再 stale，均为全批零副作用且每个 callId 恰一个 `CONFIRMATION_EXPIRED`。最终 fresh 批次批准后旧工程保存一次、候选 A 和最新 C 依序打开，SHA 摘要与目标映射正确，三 callId 各恰一结果，Agent 收到终结文本后回到 Idle。

验证：定向 `test_agentcontroller` 1/1，三个新用例各通过且未跳过；完整构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用、`git diff --check` 均通过。能力矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice113-evidence/` 保存构建、定向/全量测试、独立审核及校验记录。

限制：只验证本机 Qt 项目文件、临时目录和 POSIX symlink 的确认窗口；确认重校验到 `ProjectManager::openProject()` 实际读取之间仍存在文件系统 TOCTOU。missing candidate 仍保持既有“先保存当前工程再返回 OPEN_FAILED”语义。此 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或 8。

## 117. Slice 114：异步相机多轮 Autopilot cycle fail closed

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。Autopilot 与 Advisor 的 `run_flow(mode=cycle,maxCycles>1)` 在启用 GrabImage(Camera) 使用或无法判定异步设备时，于确认前拒绝并返回 `CAMERA_CYCLE_UNSUPPORTED`。有效来源按插件 `defaultParams()` 与工程实例参数合并；显式未知目标、Manager 线程/查询不可用、未完成 discovery 均 fail closed。空 `cameraId` 若当前自动选择语义可选中活动 async session，也先于通用 selector 缺参检查拒绝；无活动 async session 时保留 `MISSING_PARAM`，不触发发现、连接或采集。已知 legacy camera 候选把目标能力与 pending discovery 绑定到确认，漂移会使批准过期。LLM 同批包含 discovery/connect 与 bounded camera cycle 时整批在确认前拒绝，所有原 callId 仍有结构化结果。

新增 `test_agent_camera_lifecycle_g5` 覆盖：Bridge/Advisor 拒绝、LLM 的 `discover_cameras + connect_camera + run_flow(cycle)` 混合批次零副作用及五个 callId 的完整结果、默认 source、空 ID 有/无活动 async session、未知设备、未完成 discovery、能力漂移、cycle=1 拒绝/批准，以及 fake legacy camera cycle=2 的拒绝/批准和 350ms 无第三轮静默观察。legacy 批准路径恰两次采集与 `RunFinished`，终态 `completedCycles=2`；拒绝路径不增加相机、流程或 Agent 执行计数。

验证：定向 `test_agent_camera_lifecycle_g5`、`test_agentcontroller`、`test_cameramanager`、`test_grabimage` 4/4；完整构建 exit 0；在允许 localhost loopback socket 的环境下全量 CTest 91/91、0 失败、0 跳过。受限执行环境中的首轮全量运行有 11 个本机 listen/bind 失败，随后在此前授权的 localhost loopback 环境重跑全量通过。clang-format、Python 审计脚本编译、483/483 矩阵引用与库存核验、`git diff --check` 及 Slice114 SHA-256 清单通过。矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice114-evidence/` 保存构建、定向/全量日志、独立审核和摘要。

限制：只证明本地 fake async/legacy camera 的软件路径，不证明任何真实相机、厂商 SDK 或硬件驱动。未实现异步 session 的多轮续接；once、cycle=1 和其他 camera lifecycle 路径保留现状。此 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或 8。

## 118. Slice 115：Folder Create 确认绑定符号链接解析目标

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。Agent 的 `run_flow`、`step_once`、`resume_flow` 现在把 Folder `Create` 的 `resolvedFileTargetPath(folderPath)` 纳入共享确认目标快照；摘要保留配置路径，并在 symlink 解析目标不同时显示 `resolved_target`。批准前统一重算快照，目录父 symlink 改指会令旧 confirmationId 整批过期。

新增 `test_agent_folder_execution_g5::testFolderCreateConfirmationExpiresWhenSymlinkParentChangesTarget`：POSIX 临时目录配置 `link/created/nested`，pending 时 link 指向 A；使用同目录 symlink 与 `rename()` 原子改指 B 后批准旧确认，审计为 expired，A/B 均未创建，模块、RunFinished、toolExecuted 计数为零且 Engine idle。fresh confirmation 预览 B；用户拒绝后仍零副作用；再次 fresh 确认批准后 Folder 和 RunFinished 恰一次成功，嵌套目标仅在 B 创建，`operation_result=true` 且 `folder_path` 回读保持配置路径。目录 symlink 不可用时按明确能力缺失 skip；能力存在但 rename 意外失败会使测试失败。

验证：定向 `test_agent_folder_execution_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format、Python 审计脚本编译、483/483 矩阵引用与库存核验、`git diff --check` 和 Slice115 SHA-256 清单通过。矩阵和 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice115-evidence/` 保存构建、定向/全量日志、独立审核与摘要。

限制：只验证本机 POSIX 临时目录和 Folder `Create` 的 Agent 确认路径；批准前重校验到 `QDir::mkpath()` 实际创建之间仍存在 TOCTOU，不证明网络文件系统或其他平台。本 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或 8。

## 119. Slice 116：Agent flow 使用已结算的异步相机会话

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS，无阻断发现**。`run_flow(once/cycle=1)`、`step_once`、`resume_flow` 在执行前按实际候选模块检查有效 `GrabImage(Camera)` 目标：已知 async 相机必须已由 Agent 异步连接并处于 settled session；不得回退到同步连接/发现。单帧 session 对多个候选或循环重复采集 fail closed。确认快照绑定候选计划的项目指纹和相机生命周期，批准前漂移使整批过期。暂停流程可单独 `connect_camera` 后再发起 resume；运行中的 flow 仍禁止改变相机生命周期。测试现等待 async connect 同时满足已连接、有 session 且 `operationPending=false` 后再发起 resume，以避免操作结果先于生命周期 settled 的竞态。legacy 非 async 相机保持原行为。

复现覆盖无 session 的 run/step/resume 零副作用拒绝、多相机/回边限制、确认期间外部断连过期、已连接 `GrabImage → Delay` 的 step/run/resume 批准，以及已执行相机后暂停于非相机节点再恢复。另修正确认摘要：只有本次候选内的 GrabImage/SamPredict 输入，或已物化流水线中尚待变换的 SAM 图像输入，才绑定其路径/图像快照；未来未执行的来源不进入确认摘要，也不会使旧确认因未来文件改变而过期。

验证：最终全量构建 exit 0；定向 `test_agent_camera_lifecycle_g5`、`test_agentcontroller`、`test_cameramanager`、`test_grabimage` 4/4，以及 SAM 确认范围与 POSIX 串口 PTY 定向测试 2/2；最终 loopback 全量 CTest 91/91、0 失败、0 跳过。PTY one-way 测试改用有界等待以避免在 RunFinished 与 QTimer 读取之间检查过早。clang-format、Python 脚本编译、矩阵引用验证 483/483、`git diff --check` 均通过。证据目录 `build-codex-agent/agent-phase5-slice116-evidence/` 保存最终构建、定向/全量测试、独立审核及哈希清单。

限制：证据来自 Linux Qt 5.15.3、隔离 fake async/legacy camera 和本地测试插件；不证明厂商 SDK、真实设备、GUI/CLI 同步调用或异步多轮 session。此 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或 8。

## 120. Slice 117：TCPServer 响应写入失败返回不确定结果

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。TCPServer 现在等待 Qt 本地写队列排空；零字节失败仍为确定失败，当前 `execute()` 已接受响应字节而后续写入/drain 失败时返回 `ExecError::SideEffectUncertain`。每次执行前清零 accepted-byte 状态，诊断仅含阶段和字节数，不含响应正文。`DEEPLUX_ENABLE_TEST_HOOKS` 只在 BUILD_TESTS 目标中启用，以窄 writer seam 在真实动态插件 execute/Agent/RunEngine 路径注入部分接受故障。

TCPServer bounded cycle 用例在 `maxCycles=3` 下于首次响应写入故障后终止，session 为 `uncertain`、`retryable=false`、`completedCycles=0`。终态后 Qt event loop 静默观察 250ms，accept/响应写尝试/模块/RunFinished/cycleStopped/tool 计数不增加。随后同一个运行时模块再次执行零字节失败，仍返回确定失败，证明状态不会从前次调用残留。既有 TCPServer `run_flow`/`step_once`/`resume_flow` 成功及拒绝/漂移测试通过。

验证：最终全量构建 exit 0；定向 TCPServer 与 camera lifecycle 2/2；最终全量 CTest 91/91、0 失败、0 跳过。clang-format、Python 审计脚本编译、矩阵引用验证 483/483、`git diff --check` 和二进制/源证据 SHA-256 校验均通过。证据目录 `build-codex-agent/agent-phase5-slice117-evidence/`。

限制：部分接受/Drain 失败由测试 seam 注入，证明 typed error 传播与 Agent cycle 不重放策略，不证明 OS/TCP 栈某次故障实际发送了多少字节，也不证明远端收到/处理响应。此 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或 8。

## 121. Slice 118：异步相机 disconnect 抛异常后返回不确定状态

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。`CameraOperationSession` 仅当驱动 `disconnect()` 本身抛异常时返回稳定 `CAMERA_DISCONNECT_UNCONFIRMED`；断开前的 stop/status 查询异常保持原错误码，正常返回但仍连接仍为确定 `CAMERA_DISCONNECT_NOT_CONFIRMED`。CameraManager 对这个特定保留会话错误返回 `sideEffectStatus=uncertain`、`connectionState=uncertain`、`retryable=false`，Agent 查询保持 `hardwareDisconnectConfirmed=false`，避免 Error 状态下 `isConnected=false` 被误读为已确认断开。

扩展 `test_agent_camera_lifecycle_g5` 的 Controller 确认链路：拒绝 disconnect 时驱动调用为 0；批准后 fake 驱动先改变连接状态再抛异常，disconnect 恰一次，操作结果为 error/uncertain 且查询稳定。旧 session 阻止新 connect；300ms 静默观察后没有再次调用。用户再次确认 disconnect 后，Manager 查询驱动状态、清理并退役 session，结果才报告 `connectionState=disconnected` 和 `hardwareDisconnectConfirmed=true`；驱动 disconnect 总调用仍恰一次。

验证：`test_agent_camera_lifecycle_g5` 1/1；全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过。普通受限沙箱首轮 11 个 localhost listener/socket 测试失败，保留原始日志，loopback 复跑全部通过。clang-format、Python 审计脚本编译、矩阵引用验证 483/483 与库存核验、`git diff --check` 通过。能力矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice118-evidence/` 保存构建、双环境全量测试、定向结果、独立审核与哈希清单。

限制：只证明本地 fake async camera 的 Agent 状态分类和显式清理流程，不证明真实驱动/设备在抛异常时的实际连接状态；错误终态仍要求用户明确确认后清理。本 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或阶段 8。

## 122. Slice 119：异步相机连接超时返回有界不确定终态

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` **PASS，无阻断发现**。CameraOperationSession 在 CameraManager 线程增加单次 connect deadline；正式默认 30 秒，测试短期限 override 受 `BUILD_TESTS` 编译开关下的 `DEEPLUX_ENABLE_TEST_HOOKS` 保护，不进入工具 Schema 或发布构建。期限到达后清除原 request 占用并将会话置为 Error，向 `get_camera_operation` 返回 `CAMERA_CONNECT_TIMEOUT_UNCONFIRMED`、`sideEffectStatus=uncertain`、`connectionState=uncertain`、`retryable=false`；设备对象与仍执行驱动调用的 worker 保留，不强杀线程。

fake gate 经 AgentController 验证拒绝时零 connect；批准后 deadline 前可查询 pending，超时后得到稳定 error，重复查询不变。确认新的 connect 返回 `CAMERA_OPERATION_BUSY` 且没有第二次驱动调用。随后在 gate 仍阻塞时确认 disconnect，Manager 接受并将其排队到同一相机 worker；释放 gate 后迟到 connect 不覆盖原结果，排队的 disconnect 才接触驱动，操作成功并使 session 归零。

验证：`test_agent_camera_lifecycle_g5` 连续 5/5；最终全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过。clang-format、Python 审计脚本编译、矩阵引用与库存核验 483/483、`git diff --check` 通过。矩阵及 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice119-evidence/` 保存最终构建、定向重复、全量 CTest、独立审核及 SHA-256 清单。

限制：deadline 只终结 Agent 等待，不中断厂商 `connect()`；若驱动永久阻塞，session 和线程会保留，确认后的 disconnect 也只能排队等待该调用返回。fake gate 证明本机线程队列、状态和不重试语义，不证明真实硬件状态或驱动可取消性。本 slice 不关闭 G5 标准 5/6/7 整体，不放行阶段 6 或阶段 8。

## 123. Slice 120：异步相机断连 deadline 与 shutdown/drain 协调

契约 `本地逐切片契约（未纳入提交）` 最终经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 对最终代码 **PASS，无阻断或非阻断发现**。显式 Agent async disconnect 到期后返回稳定 `CAMERA_DISCONNECT_TIMEOUT_UNCONFIRMED`、uncertain 且不可重试，仍保留 session、相机、worker 和插件 lease；排队时间包含在期限内，操作不会被移除或强杀。worker 迟到回调只清除活跃标记并发内部 idle 通知，不改旧 request 结果、不自动退休会话。若 Manager 正在 shutdown，则重新驱动清理；若 `disconnectAll()` 正在 sweep，则在 worker 活跃时保持 sweep，idle 后重扫。再次清理先查询硬件状态，若已断开不重复调用驱动。

`test_cameramanager::testAsyncCameraSessionLifecycleAndWindowShutdown` 新增两条 fake gate 回归：`disconnectAll()` 期间断连超时后旧结果保持不变、Manager 保持 admission barrier，gate 释放后 sweep 重新清理且真实驱动断连只调用一次；另一条先让显式 disconnect 超时，再关闭窗口进入 Manager shutdown，worker gate 未释放时 `shutdownCompleted` 不发出，释放后 timeout 结果仍稳定、会话清零且 `shutdownCompleted` 恰发一次。原有 shutdown 中 pending connect、details、discovery drain 测试仍保留并通过。测试目标仅增加 `DEEPLUX_ENABLE_TEST_HOOKS` 编译定义，运行期限 hook 不进入发布 Schema。

验证：shutdown 用例定向通过；`test_cameramanager` 与 `test_agent_camera_lifecycle_g5` 定向 CTest 2/2；最终全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过。相机生命周期用例修正了一处既有时间边界不稳：项目参数改回原值时 `modified` 时间戳仍改变，旧暂停快照因此正确返回 `FLOW_CONFIGURATION_CHANGED`；测试现在以恢复后的工程创建新的暂停快照，再验证 stale confirmation、拒绝和批准，连续重复 10/10 通过。clang-format、Python 审计脚本编译、能力矩阵引用/库存核验 483/483、`git diff --check` 均通过。矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice120-evidence/` 保存最终构建、定向/全量测试、格式、矩阵检查、独立审核与 SHA-256 清单以及先前失败诊断日志。

限制：验证使用 fake async camera，不证明厂商驱动会返回；deadline 只限制 Agent 等待，不可中断已进入驱动的调用，因此真实驱动永久阻塞仍会令 shutdown 等待 worker。此 slice 不整体关闭 G5 标准 5/6/7，也不放行阶段 6 或 8。

## 124. Slice 121：工程保存失败原子化

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；实现独立审核 `/root/slice16_implementation_review` 最终 **PASS**。`Project::save()` 改用同目录 `QSaveFile`，关闭 direct-write fallback，检查完整写入与 `commit()`；只有提交成功才设置 `filePath`、清 dirty 并发 `modifiedChanged(false)`。现有文件 symlink 保存到当前解析目标并保留链接和用户提供的 alias 路径；悬空链接 fail closed。`ProjectMigrator::saveWithBackup()` 先原子写完旧版 `.v2.bak`，再原子替换正式文件，失败不截断原文件。失败注入接口仅在 `BUILD_TESTS` 的 core 构建中编译。

测试覆盖正式文件部分写入失败时原字节、路径、dirty 和信号保持；现存 symlink 更新目标与悬空 symlink 拒绝；迁移备份阶段失败保留原源文件及既有 sidecar；正式保存失败保留 legacy 源文件及其原始备份；ProjectManager `new/open/close(UnsavedPolicy::Save)` 失败不切换对象、不发成功信号；Agent Autopilot 与 Advisor 在确认拒绝后均不消费失败注入，批准失败返回 `SAVE_FAILED`、审计为 error，fresh 批准成功；Agent `open_project(onUnsaved=save)` 保存失败也保留当前 dirty 工程、不打开候选。

验证：全量构建 exit 0；定向 `test_project`、`test_projectmigration`、`test_agentcontroller` 3/3；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；`clang-format --dry-run --Werror` 与 `git diff --check` 通过。普通沙箱首轮全量运行因拒绝 localhost listen 有 11 个环境失败；保留日志，授权 loopback 复跑同一构建快照后全部通过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice121-evidence/` 保存构建、普通/loopback 与定向测试日志、审核结论和哈希清单。

限制：验证覆盖 Linux Qt 5.15.3、POSIX 临时目录与受控临时写入故障；不证明断电、网络文件系统、控制器缓存或真实硬件故障下的 fsync 持久性。此 slice 只关闭工程保存语义这一项，不整体关闭 G5 标准 5/6/7，也不放行阶段 6 或阶段 8。

## 125. Slice 122：Advisor 未解析运行时文件目标 fail closed

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 最终 **PASS**。`unresolvedFileTargetPreflight()` 现在同时覆盖 Advisor 与 Autopilot，并复用 `runExecutionCandidates()`、`stepExecutionCandidates()`、`resumeExecutionCandidates()` 所对应的现有执行快照。完整候选只检查当前候选且启用的模块；动态 WriteText/SaveData 路径或 Folder Create 目标未解析时，在 Chat 与 Bridge 确认/执行前返回 `SIDE_EFFECT_TARGET_UNRESOLVED`。候选不完整或工程指纹不匹配时，若存在可能参与运行的未解析副作用模块，则返回 `EXECUTION_SCOPE_UNRESOLVED`；不把全工程扫描误当作实际执行范围。

测试覆盖 Advisor/Autopilot 的 Chat 与 Bridge `run_flow`、`step_once`、`resume_flow`，动态 WriteText/SaveData/Folder Create 的零启动、无 pending 和零外部副作用拒绝；不完整候选 scope 的 fail-closed；禁用模块和启用但位于当前 step 候选外的空路径 WriteText 不造成误拒绝。对 step/resume 失败逐字段核对 RunResumeSnapshot 的 run/session/state/lifecycleGeneration/project/fingerprint 保持原值。现有已配置文件目标仍按既有流程进入确认。

验证：全量构建 exit 0；定向 `test_agent_folder_execution_g5`、`test_agentcontroller`、`test_agentpermissions` 3/3；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format 和 `git diff --check` 通过。普通沙箱 loopback 限制沿用 Slice121 记录的诊断，本 Slice 最终快照的完整 loopback 结果归档在证据目录。证据目录 `build-codex-agent/agent-phase5-slice122-evidence/` 保存构建、定向/全量日志、独立审核、二进制清单与 SHA-256。

限制：执行候选来自当前 RunEngine 计划；对不完整 scope 采取保守拒绝，不证明上游动态输入实际运行时路径，也不消除已知路径文件操作的 TOCTOU。本 slice 不整体关闭 G5 标准 5/6/7，也不放行阶段 6 或阶段 8。

## 126. Slice 123：PLCWrite 确认摘要披露寄存器写入范围

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；独立实现复审 `/root/slice16_implementation_review` 对最终实现 **PASS**。PLCWrite 确认摘要按插件的 0-based holding-register 地址显示数量、inclusive 末地址和来源。immediate 数组按实际数组长度计算；越界起始地址或地址末端溢出显示未回绕的计算值并标 `range=invalid`。variable 与未知/缺省来源显示 runtime 数量和末地址及 `range=unresolved-at-confirmation`；未知值显示 `unknown:<source>`，缺省值显示 `unknown:<unset>`。确认摘要不包含寄存器值。

新增 `testImmediatePLCWritePreviewShowsInclusiveRegisterRange` 覆盖合法上边界单寄存器、合法双寄存器、末地址溢出、起始地址 65536，以及未知来源。fake peer 验证批准后的合法 FC16 起始地址、数量、数据字节数和寄存器值；非法范围及未知来源在批准后失败且不建立 PLC 连接、不发送请求。payload 对 immediate 测试值作不泄漏断言。既有 `test_agentcontroller` fixture 也核对缺省来源的 `unknown:<unset>` 文案。实现审核首轮提出未知来源和越界起始地址两项发现，均已补齐实现及回归后复审关闭。

验证：定向 `test_agentcontroller`、`test_agent_folder_execution_g5`、`test_agent_plcwrite_execution_g5` 3/3；最终全量构建 exit 0；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过；clang-format 与 `git diff --check` 通过。能力矩阵与 GUI 操作清单未改。证据目录 `build-codex-agent/agent-phase5-slice123-evidence/` 保存最终构建、定向/全量 CTest、审核结论、二进制清单和 SHA-256 校验材料。

限制：FC16 仅由本机 fake Modbus peer 验证，不证明真实 PLC 接受写入或现场寄存器状态；variable 来源的实际长度只在运行时确定。此 slice 不整体关闭 G5 标准 5/6/7，也不放行阶段 6 或阶段 8。

## 127. Slice 124：Folder Delete 执行边界绑定批准操作与目标

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` 最终 **CONTRACT_PASS**；最终实现由 `/root/slice16_implementation_review` 独立复审 **PASS**。RunEngine 不再用全局“下一次运行”槽暂存 Agent Folder 授权；`run_flow(mode=once)`、`step_once` 与断点 `resume_flow` 将独立 `executionAuthorizationId`（绑定 confirmationId/callId）作为启动参数，并在 `tryBeginForRun` 同一生命周期租约中原子提交。普通 GUI/CLI Engine run 不会领取该 token。竞争测试固定顺序：Agent 获租约并到达 `moduleStarted` 屏障，普通 `runOnceAsync` 随后竞争被拒、无 runId；Agent 调用仍携带自己的 token。

Folder 确认快照冻结 operation、配置路径、批准时目录存在状态与解析目标。Folder 执行入口按实例核对批准 operation/path；未授权的 Create/Delete、批准后 Delete→Create、路径变更、目录在批准后新建、批准目标 symlink 改向均返回 `CONFIRMED_TARGET_CHANGED`，不进入创建/递归删除。批准和边界复核时目标均不存在保留 no-op。动态 Folder Delete 路径拒绝执行。执行完成和 breakpoint pause 清除 active token；如果带 Folder Delete 授权的 run 在删除前断点暂停，无确认 resume 被拒且保留同一 paused runId/sentinel；fresh Agent resume confirmation 恢复同一 runId、断点前探针不重跑，之后才执行 Delete。非断点 cycle resume 保持既有行为且不携带本切片 Folder token。

新增/扩展 `test_agent_folder_execution_g5` 覆盖 `run_once`、`step_once`、断点 `resume_flow` 三入口的 symlink A→B 执行边界注入、批准操作 Delete→Create 零创建、批准时目录不存在但启动屏障新建 sentinel 时拒绝删除、暂停后无 token resume 被拒与 fresh confirmation 连续性、普通 GUI run 租约竞争失败且无授权，以及后续普通 Engine run 不继承 token。确认前/拒绝后仍零启动和零删除；新确认只作用于 B；错误码在 RunResult、get_run_results、同步 step tool result 可观察。

验证：最终 `cmake --build build-codex-agent -j2` 成功；`test_runengine`、`test_agentactor`、`test_agentcontroller`、`test_agent_folder_execution_g5` 定向 CTest 4/4；最终获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过。受限沙箱首轮全量运行的 11 个 listener/socket 失败均由无法绑定本地端口导致，已保留环境说明；最终 loopback 快照全绿。clang-format、`git diff --check`、SHA-256 校验通过。证据目录 `build-codex-agent/agent-phase5-slice124-evidence/` 保存最终构建、定向/全量 CTest、合同/实现审核、源文件和关键二进制哈希。

限制：本 Slice 证明批准后到 Folder 插件删除入口前的配置/解析漂移，不消除最后一次路径检查与 OS 递归删除系统调用之间的 TOCTOU，不绑定目录 inode/内容，不覆盖其他文件插件或真实文件系统 adversarial race。此 slice 不关闭 G5 标准 5/6/7，也不放行阶段 6 或阶段 8。

## 128. Slice 125：Folder Delete bounded cycle 每轮复核批准目标

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。实现由 `/root/slice16_implementation_review` 独立复核 **PASS**；复核提出的测试 signal teardown Low 风险已改为局部 QObject receiver 生命周期自动断开，并由复核者确认关闭。

RunEngine 将 Agent 批准快照绑定到 bounded cycle session，并只对匹配 session 的每个 `CycleTick` 租约传递同一授权快照。授权不放入全局 next-run 槽；普通 GUI cycle 收到空授权。cycle 被拒、启动失败、停止、取消、运行失败或完成时清除 session 授权；带旧 session 身份的迟到 tick 不会启动。Agent bounded cycle 开始的每轮 Folder Delete 均复核同一 Folder instance、operation、配置路径和批准目标。

新增 `testAgentFolderDeleteBoundedCycleAuthorization`：

- `maxCycles=1` 静态 Folder Delete 在批准后恰执行一次成功；module 边界拿到 confirmationId/callId/executionAuthorizationId 与批准解析目标；
- 后续普通 GUI bounded cycle 的 Folder authorization 为空；
- `maxCycles=3` 临时目录用例在第一轮成功删除 A 后、下一 100ms tick 前原子将父 symlink 从 A 改指 B。第二轮在 Folder 边界以 `CONFIRMED_TARGET_CHANGED` 失败，B sentinel 保留，session `status=failed`、`completedCycles=1`、错误码/错误名/Folder instance 可查询，第三轮不启动；两轮 module authorization 中 confirmationId/callId/executionAuthorizationId 和 A 目标保持一致；
- `cycleStopped` 后立即查询失败 session，再静默观察 350ms，模块、runFinished、cycleStopped、run_flow 调用及审计计数保持不变。

验证：最终全量构建 exit 0；最终获准 localhost loopback CTest 91/91、0 失败、0 跳过。Folder G5 定向 CTest 在稳定性修订后连续 5/5 通过。开发过程中 `testFolderDeleteRechecksApprovedTargetAtExecutionBoundary` 曾间歇地在 `resume_flow` 用例中未进入 pending-confirmation：测试先修改再还原工程参数，却复用旧 breakpoint fingerprint；`Project` 修改时间变化会使 resume 快照失效。测试现在在恢复前重新加载工程并建立新的暂停快照，消除旧快照误用。首轮失败日志及修订后的 5 次定向与全量 loopback 通过日志均保留。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。证据目录 `build-codex-agent/agent-phase5-slice125-evidence/` 保存契约/实现审核、构建日志、质量门禁、定向/重复/全量 CTest 日志、源码与二进制 SHA-256。

限制：本 Slice 防止 bounded Agent cycle 中的确定性 symlink A→B 漂移，不消除最终路径检查与操作系统递归删除之间的 TOCTOU，也不绑定目录 inode/内容。它只补上 Folder Delete 的 bounded cycle 覆盖；G5 标准 5 全量直接工具与运行入口证据对账、标准 6 其它静态文件写插件边界、标准 7 阶段 8 外部门槛清单仍未闭合。此 Slice 不标记 G5 标准 5/6/7 PASS，不放行阶段 6 或阶段 8。

## 129. Slice 126：Folder Create 执行边界复核批准目标

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**；最终实现由 `/root/slice16_implementation_review` 独立复核 **PASS，无阻断发现**。`PathUtils::resolveFileTargetPath()` 成为确认快照与 Folder 插件共享的解析实现，跟随已有 symlink 并补齐不存在的多级尾部。Agent 对 Folder `Create` 的 once、step、断点 resume 和 bounded cycle 授权携带批准解析目标；插件在 `ModuleBase::execute()`/`QDir::mkpath()` 前核对操作、实例、配置路径与当前解析目标，A→B 漂移返回 `CONFIRMED_TARGET_CHANGED`（1007）。普通无 Agent 授权的引擎运行沿用原有 Create 语义；每次执行在拒绝检查前清零结果状态，失败不能复用前一轮的 `operation_result=true`。

路径工具回归覆盖不存在后缀、目录 symlink 原子替换及 symlink 环；Folder G5 集成回归覆盖 run once、step、真实断点 resume 的边界改向拒绝，bounded cycle 的首轮启动后改向拒绝与第一轮成功后第二轮改向 fail-stop，静态 `maxCycles=1` 批准成功，以及普通引擎无 Agent 授权的成功路径。已有 Slice115 继续证明改向后 fresh confirmation 预览 B、拒绝零副作用、批准后仅在 B 创建。第二轮失败查询不会返回成功的 `operation_result`；cycle session 错误码、failed module 和 completedCycles 可查询，`cycleStopped` 后静默观察 350ms 无额外轮次。

验证：最终全量构建 `cmake --build build-codex-agent -j2` exit 0；定向 `test_pathtutils` 与 `test_agent_folder_execution_g5` 2/2；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过。clang-format 与 `git diff --check` 通过。证据目录 `build-codex-agent/agent-phase5-slice126-evidence/` 保存最终构建、定向/全量日志、合同/实现审核、关键二进制清单与 SHA-256。标准 5 的直接工具及插件×运行入口最终快照对账、标准 6 其它静态文件写插件的执行边界和标准 7 阶段 8 外部门槛清单仍未闭合；本 Slice 不标记 G5 标准 5/6/7 PASS，不放行阶段 6 或阶段 8。

限制：只证明本机 Linux Qt 5.15.3、POSIX 临时目录 symlink 及 Folder `Create` Agent 路径；最后一次解析与 `QDir::mkpath()` 仍存在文件系统 TOCTOU，不证明 Windows、网络文件系统或真实设备。

## 130. Slice 127：静态文件输出执行边界授权与暂停续接

契约 `本地逐切片契约（未纳入提交）` 经 `/root/slice19_contract_review` **CONTRACT_PASS**。独立实现审核 `/root/slice16_implementation_review` 最终 **PASS**；审核提出的测试回调生命周期与 cycle 续接成功断言缺口已修复，复审确认关闭。

Agent 确认快照现在将 WriteText、SaveData、SaveImage 的 pluginId、instanceId、effective `currentParams()`、配置 `filePath` 与 symlink 解析目标绑定到 confirmation/call/execution authorization。RunEngine 把所属调用授权传到对应模块；三个插件都在 `ModuleBase::execute()` 之前复核身份、全部参数和当前解析目标，目标/参数漂移返回 `CONFIRMED_TARGET_CHANGED`（1007），并在复核之前清除旧写入结果状态。无 Agent 授权的普通 `RunEngine::runOnce()` 仍可按原配置写入。

新增 G5 回归覆盖：

- `testStaticFileOutputsRejectExecutionBoundarySymlinkDrift`：WriteText/run_flow、SaveData/step_once、SaveImage/fresh resume 分别在批准后、模块开始后执行原子目录 symlink A→B；三者均在任何文件副作用前以 1007 失败，A/B sentinel 保持原始字节，失败模块业务输出不可用。resume 场景还证明带 side-effect authorization 的断点运行拒绝普通无 token `RunEngine::resume()`。
- `testStaticFileOutputFreshTargetConfirmationAndCycleNoReplay`：执行边界改向后 fresh confirmation 预览解析目标 B；拒绝仍零写入，批准后只写 B。运行期 effective 参数漂移被拒且 sentinel 不变；静态 `maxCycles=1` 仅写一次；`maxCycles=3` 在第一轮成功写 A 后改向 B，第二轮以 1007 fail-stop，session 错误码/名称/模块/`completedCycles=1` 可查，B 未改，350ms 静默观察中无第三轮或额外工具/审计事件。
- `testPausedAgentSideEffectsRequireFreshResumeAuthorization`：WriteText 与 Folder Create 的 breakpoint run 在副作用前拒绝普通 resume；fresh `resume_flow` 使用不同 execution authorization 恢复同一 run，断点前 probe 不重跑，批准副作用恰执行一次。bounded cycle 第一轮成功后手动暂停；旧 timer 与普通 resume 均不启动第二轮；fresh Agent resume 使用新 token 继续原 session，第二轮结果成功、session `completedCycles=2`、最终文件恰追加一次，静默观察确认终态稳定。

验证证据：最终 `cmake --build build-codex-agent -j2` exit 0；定向 `test_agent_folder_execution_g5` CTest 1/1；获准 localhost loopback 全量 CTest 91/91、0 失败、0 跳过。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。证据目录 `build-codex-agent/agent-phase5-slice127-evidence/` 保存完整 build 与 CTest 原始日志、合同/实现/标准 5 和 7 的独立审核、质量门禁、关键二进制 manifest 和源/证据 SHA-256。

G5 标准 5 的直接工具及插件 once/step/resume 证据对账见 `docs/baseline/agent-phase-5-side-effect-crosswalk.md`；成功多轮 cycle 空格显式列为扩展模式证据缺口。阶段 8 的设备、SDK、真实 SAM、provider、跨平台与性能门槛见 `docs/baseline/agent-phase-8-external-validation-gates.md`，全部标记 `NOT_RUN` 直至现场证据归档。独立只读审核者 `/root/phase4_slice1_independent_audit` 对 G5 标准 5、6、7 判定 **PASS**，限定于计划 §10 软件与证据分流口径；G5 阶段 5 因此 PASS 并允许进入阶段 6，但该判定不自动放行阶段 8 现场验收。Slice 127 具体实现与边界仍受上述限制约束。

限制：只证明 Linux Qt 5.15.3、POSIX 临时目录 symlink 和本地动态插件。最后一次路径复核到 OS 文件打开/目录创建之间仍有 TOCTOU；不证明 inode 锁定、网络文件系统、其他平台、掉电/磁盘故障、真实设备/模型/provider 或 G5 外部门槛已通过。

## 131. Slice 128：SaveData 与 SaveImage 成功 bounded cycle 精确计数

契约 `本地逐切片契约（未纳入提交）` 经独立审核者复核为 **CONTRACT_PASS**。首轮审核指出 `runFinished` 是流程级而非模块级信号；契约已改为 GrabImage 与 SaveImage 各自的 started/finished 各 3 次、整个 cycle 的 `runFinished` 共 3 次后通过。实现经独立复审为 **IMPLEMENTATION_PASS**；复审提出的拒绝确认消费断言和静默窗口后 session 复查均已补齐。

新增 `testSaveDataAndSaveImageSuccessfulBoundedCyclesExactSideEffects`，动态加载实际 GrabImage、SaveData、SaveImage 插件，使用隔离目录和三张颜色/文件名不同的 PNG 输入；SaveData 与 SaveImage 分别执行自己的 `maxCycles=3` 流程。

- 两个流程都验证确认 pending 时输出 sentinel 保持原字节；拒绝后 pending/payload 消费、文件字节不变、Engine idle，模块、runFinished、cycleStopped、tool 执行计数均为零，并记录 rejected 审计。
- SaveData 批准后恰有三轮成功，输出精确等于 sentinel 加三组按 key 排序的 `folder_count`、`folder_path`、`folder_position`、`source_file_name`、`source_path` 文本行；文件大小与字节数一致。
- SaveImage 批准后 GrabImage 与 SaveImage 各 started/finished 三次、`runFinished` 共三次；最终输出可解码，尺寸正确，逐像素等于第三张输入帧。
- 两条流程均验证 cycle session `completed`、`completedCycles=3`、`maxCycles=3`、`isBusy=false`，末次 run ID 对齐；run_flow 与成功审计各恰一次。350ms 静默后重读 session 并复核终态，文件/信号/tool/audit 计数不变。

验证：全量构建 `cmake --build build-codex-agent -j2` exit 0；目标 QtTest `testSaveDataAndSaveImageSuccessfulBoundedCyclesExactSideEffects` 通过，最终获准 localhost loopback 全量 CTest 为 **99/100**。唯一失败套件 `test_mainwindow` 中有两个范围外断言失败：`testAgentInputErrorPathDoesNotCrashOrStayThinking` 在 `tests/test_mainwindow.cpp:8435` 未收到预期 `errorSpy`，`testDarkThemeCloseAndDestroyWindowDirectly` 在 `:3294` 的窗口样式与深色主题不符。初次受限沙箱运行的 12 个 localhost/HTTP listener 套件在获准 loopback 全量复跑均通过。格式检查、`git diff --check` 与独立审核通过。证据目录 `build-codex-agent/agent-phase5-slice128-evidence/` 保存 build/CTest/定向日志、exit code、独立审核结论及可校验 SHA-256 清单。

本 Slice 关闭 SaveData/SaveImage 成功 bounded cycle 扩展证据缺口；PLCRead 与 PLCCommunicate 的成功多轮 cycle 仍未覆盖。它不改变 G5 标准 5/6/7 的既有独立审核状态，不将 cycle 扩展项混入 once/step/resume 矩阵，也不把阶段 8 真实设备、provider、模型、平台与性能门槛标记为通过；这些外部门槛仍为 `NOT_RUN`。

## 132. Slice 129：PLCRead 与 PLCCommunicate 成功 bounded cycle 精确计数

契约 `本地逐切片契约（未纳入提交）` 经独立审核 **CONTRACT_PASS**。实现由独立审核 **IMPLEMENTATION_PASS**，未发现断言缺口。

新增 `testPLCReadSuccessfulBoundedCycleUsesExactlyThreeFC03Requests` 与 `testPLCCommunicateSuccessfulBoundedCycleUsesExactlyThreeFC03Requests`，均用测试目标真实动态插件和 loopback Modbus/TCP fake peer，分别验证 `run_flow(mode=cycle,maxCycles=3)`：

- pending confirmation 和拒绝路径均核对 side-effect 预览，peer 请求/响应及模块、RunEngine、工具执行计数为 0；拒绝后 pending 消费，工程运行保持 idle，审计为 rejected。
- fresh 批准后恰好有 3 次有效 FC03 请求/响应、模块 started/finished 各 3 次、成功 `runFinished` 共 3 次，`cycleStopped` 恰 1 次；每个请求帧都核对协议字段和批准配置。
- PLCRead 查询末轮 run-bound registers 并核对 fake peer payload；PLCCommunicate 核对有效响应及 `plc_connected=true`。PLCCommunicate 的 unit ID 固定为 1，按请求帧断言，不误写成可配置或确认摘要字段。
- 两条流程均核对 bounded-cycle session 完成、`completedCycles=3`、`maxCycles=3`、末次 run ID 对齐，350ms 静默后重查 session，网络/模块/run/tool/audit 计数无额外变化。

验证：全量 `cmake --build build-codex-agent -j2` exit 0；两个新用例逐项通过，两套完整 G5 CTest 目标 2/2 通过。获准 localhost loopback 全量 CTest 首轮 **98/100**：失败项包括 `test_mainwindow` 中两个范围外断言，以及 `test_agent_datasource_dedicated_viewport` 的一次 signal 11；失败项复跑时 datasource viewport 通过，`test_mainwindow` 仍有两项断言失败（输入错误路径 `errorSpy` 计数，`tests/test_mainwindow.cpp:8435`；深色主题窗口样式，`:3294`）。完整与重跑日志均保留，未将失败改写为通过。`clang-format --dry-run --Werror` 与 `git diff --check` 通过；证据目录 `build-codex-agent/agent-phase5-slice129-evidence/` 保存 build、定向/全量 CTest、复跑日志、独立审核和 SHA-256 清单。

本 Slice 关闭 PLCRead/PLCCommunicate 成功 bounded-cycle 扩展证据缺口，不替代 once/step/resume 入口矩阵，也不改变 G5 标准 5/6/7 的既有 PASS 范围。TCPServer 成功多轮扩展证据尚未建立；阶段 8 真实设备、SDK、模型/provider、平台与性能门槛继续保持 `NOT_RUN`。

## 133. Slice 130：TCPServer 成功 bounded cycle 逐轮收发计数

契约 `本地逐切片契约（未纳入提交）` 经独立契约复核后冻结；实现由独立审核者判定 **IMPLEMENTATION_PASS**。复核关闭两项阻断：客户端须读到连接关闭后才比对完整响应字节，避免漏掉尾随数据；每次 `runFinished` 都以对应 run ID 读取 RunEngine 服务端输出，逐轮核对实际收到的请求内容与字节长度。客户端同时要求发送缓冲区排空。

新增 `testSuccessfulBoundedCycleUsesThreeExactExchanges`，通过真实动态 TCPServer 插件和本机 loopback 验证 `run_flow(mode=cycle,maxCycles=3)`。确认前无监听与模块执行；拒绝后 pending 消费、无连接/模块/run/tool/cycle 副作用，且仅一条 rejected 审计。重新确认批准后，fake client 建立 3 条独立连接并发送 3 个不同请求；三轮服务端输出依次与请求及长度一致，每轮收到的完整响应精确等于配置字节。模块 started/finished 各 3 次且成功、`runFinished` 3 次且成功、`cycleStopped` 1 次，成功工具与审计各 1 次。session 完成且 `completedCycles=3`、末轮 run ID 对齐；350ms 静默观察确认 session、网络、模块、run、cycle、tool、audit 计数稳定。

验证：`cmake --build build-codex-agent -j2` exit 0；`test_agent_tcpserver_execution_g5` CTest 1/1 通过，单独 QtTest 用例通过（含 init/cleanup 共 3/3）；格式检查和 `git diff --check` 通过。获准本机 loopback 全量 CTest 为 **107/108**；唯一失败套件 `test_mainwindow` 重跑仍有两条既有断言失败：输入错误路径 `errorSpy` 实际 0/预期 1（`tests/test_mainwindow.cpp:9801`），以及深色主题 stylesheet 不匹配（`:4413`）。两项各自单独运行均通过，其他 MainWindow 断言通过。全量、整套重跑和两项隔离日志归档于 `build-codex-agent/agent-phase5-slice130-evidence/`；未把全量结果记为通过。

本 Slice 关闭 TCPServer 成功 bounded cycle 扩展证据缺口，不替代 once/step/resume 入口矩阵或 G5 标准 5/6/7 的既有审核口径。证据仅覆盖本机 Linux loopback、Qt 5.15.3 和软件 cycle 协议；阶段 8 真实网络、设备、SDK、模型/provider、跨平台与性能门槛继续 `NOT_RUN`。本工作未提交、未推送。

## G5 软件验收后的当前完整回归（2026-11-03）

Slice 130 记录的 107/108 是该切片当时的一次全量 CTest 结果，历史原始日志和失败项说明保留不变。之后在同一当前工作树执行的完整构建 `cmake --build build-codex-agent -j2` exit 0，完整 CTest `ctest --test-dir build-codex-agent --output-on-failure -j2` 为 112/112、0 失败、0 跳过；当前日志 `build-codex-agent/agent-phase8-current-matrix-evidence/ctest-full-slice29.log` 明确显示 `test_mainwindow` 与 `test_agent_tcpserver_execution_g5` 均通过。该较新结果确认先前两条 MainWindow 失败断言在当前完整回归中未复现；它不删除或改写 Slice 130 当时的失败记录。

阶段 5 软件范围仍按计划 §10 的独立审核结果为 PASS；本次完整回归补强当前工作树的回归证据，不扩展真实网络、相机/PLC 硬件、模型/provider 或阶段 8 现场验收范围。
