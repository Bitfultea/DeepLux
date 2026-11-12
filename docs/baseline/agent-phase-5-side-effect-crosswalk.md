# G5 标准 5：副作用入口证据对账

## 判定口径

按计划 §10，标准 5 要求覆盖直接副作用工具，以及配置插件后的 `run_flow`、`step_once`、`resume_flow`：确认前副作用为 0，拒绝为 0，批准后调用次数与批准目标一致。本文把成功 bounded cycle 作为扩展模式单列，不用 cycle 空格推导 once/step/resume 未覆盖，也不把 cycle 结果混入这三条入口的计数。

计数以每个切片的专用临时路径、fake peer、PTY 或 fake 服务原始调用/字节、插件开始/结束计数为准。仅看到 tool 返回 `started` 不计为副作用已成功；不确定状态按不可自动重放检查。

## 已有最终快照测试映射

| 目标 | `run_flow` | `step_once` | `resume_flow` | 拒绝与批准计数证据 |
| --- | --- | --- | --- | --- |
| Folder `Create` / `Delete` | `testFolderCreateRechecksApprovedTargetAtExecutionBoundary`；`testFolderDeleteRequiresConfirmationForRunFlowAndStepOnce` | 同左 | `testFolderDeleteRequiresConfirmationForResumeFlow`；Slice 126 Create 边界循环含 resume | 启动前/拒绝后无目录创建或删除；批准后目标恰一次；A→B 执行边界拒绝见 Slice 124–127 |
| WriteText | `testWriteTextRunFlowConfirmsConfiguredAndRejectsUnresolvedTarget` | `testWriteTextStepOnceConfirmsConfiguredFileTarget` | `testWriteTextResumeFlowConfirmsBreakpointFileTarget` | 文件内容与 `write_result`；拒绝零启动；执行失败不返回可用成功业务结果；Slice 127 加执行边界、fresh B 和不重放 |
| SaveData | `testSaveDataRunFlowConfirmsConfiguredAndRejectsUnresolvedTarget` | `testSaveDataStepOnceRejectsStaleTargetAndWritesApprovedTargetOnce` | `testSaveDataResumeFlowRejectsAndExpiresStalePathThenWritesOriginalOnce` | JSON 文件/sentinel 与 `save_result`；拒绝零启动；Slice 127 增加批准后目标漂移拒绝；Slice 128 增加三轮成功追加的精确文本/元数据计数 |
| SaveImage | `testSaveImageRunFlowConfirmsFileOutput` | `testSaveImageStepOnceConfirmsEachOutputStep` | `testSaveImageResumeFlowConfirmsBreakpointOutput` | PNG 像素/文件是否存在；拒绝零启动；Slice 127 增加批准后目标漂移拒绝；Slice 128 增加三轮覆盖及最终帧逐像素核验 |
| PLCWrite | `testPLCWriteRequiresConfirmationForRunFlowAndStepOnce` | 同左及 `testAutopilotStepOnceRetainsDeterministicArrayForPLCWrite` | `testPLCWriteRequiresConfirmationForResumeFlow`；`testAutopilotResumeFlowRetainsDeterministicArrayForPLCWrite` | fake Modbus FC16 peer 请求/寄存器计数；拒绝/旧配置不发送，批准按批准长度发送一次；timeout `uncertain` 且不重试 |
| PLCRead | `testRunFlowReadsRegistersAndTimeoutDoesNotReuseOrRetryOutput` | `testStepOnceConfirmsSingleFC03AndReturnsRunBoundRegisters` | `testResumeFlowConfirmsSingleFC03AndReturnsPausedRunRegisters` | fake Modbus FC03 peer 请求次数和返回寄存器；拒绝/漂移为 0，批准恰一次；Slice 129 bounded cycle 恰 3 次请求/响应并读取末轮寄存器 |
| PLCCommunicate | `testAgentConfirmationRejectsAndExpiresOldPLCCommunicateTarget` | `testStepOnceExpiresDriftRejectsAndConfirmsSingleFC03` | `testResumeFlowExpiresDriftRejectsAndConfirmsSingleFC03` | fake Modbus peer 请求/响应计数；拒绝和目标漂移不发送，fresh 批准一次 FC03；Slice 129 bounded cycle 恰 3 次请求/响应并逐帧核对固定 unit ID=1 |
| TCPClient | `testRunFlowBindsEndpointExpiresOnPortChangeAndReadsResponse` | `testStepOnceConfirmsTCPClientSingleSend` | `testResumeFlowConfirmsTCPClientSingleSend` | loopback peer 原始请求/响应计数；拒绝/端点漂移 0，批准恰一次；timeout/不完整响应不重试 |
| TCPServer | `testRunFlowBindsLocalEndpointExpiresOnPortChangeAndReadsRequest` | `testStepOnceConfirmsTCPServerSingleExchange` | `testResumeFlowConfirmsTCPServerSingleExchange` | loopback client/server 请求、响应字节计数；批准后启动监听及单次收发，拒绝不启动；Slice 130 成功 bounded cycle 逐轮核对服务端收到的三个不同请求和三条完整响应 |
| SerialPort | `testRunFlowRejectsAndExpiresSerialTargetThenReadsPTYReply` | `testStepOnceConfirmsSerialPortSingleExchange` | `testResumeFlowConfirmsSerialPortSingleExchange` | POSIX PTY 字节计数；目标漂移/拒绝 0，批准精确一次请求与回复；超时和部分收发不重放 |
| SAM 推理插件 | `samPredictPluginFlowConfirmationBindsNetworkTarget` | `samPredictPluginStepOnceAndResumeConfirmInputAndNetworkTargets` | 同一组合用例覆盖 fresh resume | Fake 服务请求数、输入摘要/结果对象与批准网络目标；拒绝不请求；生产模型另归阶段 8 |
| GrabImage / 相机操作 | `test_agent_camera_lifecycle_g5::testAgentAsyncSessionFeedsGrabImage` | 同一测试中的 `step_once` 批次 | 同一测试中的 breakpoint `resume_flow` 批次 | 每条入口各有拒绝零采集与批准后单次采集计数；fake 相机连接、停止和释放；异步多轮 cycle 不支持时确认前拒绝；不作为真实设备证据 |

## 直接副作用工具对账

| 工具类别 | 主要测试/切片 | 计数判定 |
| --- | --- | --- |
| 工程保存及 `create_project` / `open_project` 隐式保存 | `testSaveProjectConfirmationGatesDiskWrite`；Slice 36、93、111、113、121 | 确认前与拒绝后目标文件字节不变；批准保存一次；候选内容/路径漂移令批次过期 |
| 插件加载/卸载 | `testPluginLifecycleApprovalLoadsAndUnloadsExactlyOnce`；`testAgentModulePluginLifecycleRequiresConfirmationAndHonorsInstanceLease`；Slice 104–106 | 拒绝无 load/unload；批准恰一次；活动实例阻止卸载 |
| 相机发现、连接、断开、参数写入 | `test_agent_camera_lifecycle_g5`；Slice 37、39、40、83、84、118–120 | fake 驱动 discover/connect/set/readback/disconnect 计数；拒绝和身份代次漂移为零，批准按请求一次 |
| `sam_predict` | `test_agent_sam_tools`；Slice 41、55、62、63、93、95 | fake 服务请求/取消计数；拒绝不请求；结果与批准输入相符；生产模型另归阶段 8 |
| `sam_annotate` 直接工具/会话 | `samAnnotateWritesSessionAndAppends`、`samAnnotateAllOrNothing`、`samAnnotateConfirmationBindsSessionSymlinkTarget` | 会话对象/文件字节与原子替换 sentinel；拒绝或目标漂移不写；生产模型另归阶段 8 |

## 周期模式扩展覆盖与边界

成功有界周期精确计数已覆盖 Folder Create/Delete 的 `maxCycles=1` 恰一次；Folder 两操作多轮分别覆盖稳定或首轮成功后漂移 fail-stop。WriteText、PLCWrite、PLCRead、PLCCommunicate、TCPClient、TCPServer、SerialPort、SamPredict、fake legacy GrabImage、SaveData、SaveImage 有成功多轮计数。Slice 128 的 SaveData/SaveImage 测试使用真实动态插件和三张不同输入帧，验证拒绝零副作用、批准后三轮成功、SaveData 精确追加五项文件夹/源元数据、SaveImage 最终 PNG 与第三帧逐像素一致，并核验终态静默窗口。Slice 129 的两个 PLC cycle 测试分别核验三次有效 FC03 请求/响应、批准参数、拒绝零请求、成功 session 和静默终态；PLCCommunicate 的 unit ID 是实现固定值 1，不属于确认摘要字段。Slice 130 的 TCPServer 测试逐轮核验三次独立连接、RunEngine 服务端实际收到的不同请求、读至连接关闭的完整响应字节、确认拒绝零副作用及 cycle 静默终态。TCPServer 还覆盖不确定响应失败后停轮。计划 §10 标准 5 的字面矩阵是直接工具与 once/step/resume 三入口，以上仍按扩展模式单列。

## 最终状态

独立只读审核已判定 G5 标准 5、6、7 在计划 §10 的软件和证据范围内 **PASS**。Slice 127 与 Slice 128 的证据分别位于对应 slice 证据目录；Slice 128 获准 localhost loopback 全量 CTest 为 **99/100**，唯一失败套件 `test_mainwindow` 中两项范围外 UI/错误路径断言失败。Slice 129 证据位于 `build-codex-agent/agent-phase5-slice129-evidence/`：全量构建 exit 0，PLCRead/PLCCommunicate 定向 CTest 2/2；获准 localhost loopback 全量 CTest 首轮 **98/100**，其中 `test_agent_datasource_dedicated_viewport` 子进程 signal 11 在仅重跑失败项时通过，`test_mainwindow` 的两个断言仍失败（`tests/test_mainwindow.cpp:8435` 与 `:3294`）。Slice 129 契约与实现独立审核均通过；完整日志、复跑结果及哈希清单归档于证据目录。普通受限环境的 localhost/HTTP listener 失败已在获准 loopback 重跑中通过。模拟 peer、fake 模型或 fake camera 结果不升级为现场证据，阶段 8 现场门槛仍为 `NOT_RUN`。

后续当前工作树回归（2026-11-03）：完整构建 exit 0；获准 loopback 全量 CTest **112/112**、0 失败、0 跳过。该轮包含 `test_mainwindow` 与 `test_agent_tcpserver_execution_g5`，两套均通过，原始日志为 `build-codex-agent/agent-phase8-current-matrix-evidence/ctest-full-slice29.log`。这确认 Slice 130 记录的两条 MainWindow 断言在后续完整回归中未复现；历史 107/108 结果仍作为当时记录保留。G5 标准 5/6/7 的软件证据判定不变，阶段 8 真实现场门槛仍未执行。
