# 阶段 1 验收记录：失败语义、权限一致性与工程保护

依据 `docs/agent-capability-improvement-plan.md` §6/§4.2/§14.2 编制。前置:阶段 0 已 PASS(2026-09-29,见 `agent-phase-0-acceptance.md`)。
**状态:PASS**（独立审核 Agent 于 2026-09-30 对 r6 给出 G1 PASS；结论与边界见 §12。此处仅归档审核结论，非实现 Agent 自行放行）。

```text
阶段：1（失败语义、权限一致性与工程保护）
实现负责人：Claude Code（原阶段实现）、Codex（独立审核后的整改）
审核负责人：独立审核 Agent（`/root/phase1_independent_audit`）
开始 revision / 工作区摘要：b516f54 @ agent/development + 阶段0未提交资产 + 他人在先脏文件（见阶段0记录 §2）
结束 revision / 工作区摘要：仍为 b516f54（阶段1未提交、未推送）；改动文件清单见 §2/§6/§7，
  包含 GUI 工程切换入口、Project::load 校验及相关测试；既有脏树保留
覆盖 F 编号与操作编号：F06/F07/F08/F10/F13（收口）；F01 相关的 run_flow 保守确认口径（精细判定留阶段5）；
  操作编号 CHAT_TOOL_CONFIRM/ASET_PERMISSION_LEVEL/FILE_NEW_SOLUTION/FILE_OPEN/FILE_SAVE/TB_RUN_ONCE
构建命令 / 退出码：cmake --build build-codex-agent -j2 → BUILD_EXIT=0（agent-phase1-evidence-r6/build.log）
测试命令 / 实际用例数 / 退出码：
  env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure
    → 78/78 套件通过，CTEST_EXIT=0（agent-phase1-evidence-r6/ctest-full.log）
  ./build-codex-agent/bin/test_agentphase0_repro → 17/17 用例，REPRO_EXIT=0（agent-phase1-evidence-r6/repro-test-run.log）
输入 / 实际输出 / 期望 / 容差：见 §1 契约样例（实现与样例一致）与 §3 逐条证据
原始证据位置与 SHA-256：build-codex-agent/agent-phase1-evidence-r6/sha256sums.txt；原始、r2、r3、r4、r5 目录均保留
矩阵 PASS / FAIL / …：已回写并重渲染——权限维度 FAIL 15→PASS 2（Folder/PLCWrite 代表证据）+
  NOT_TESTED 13（机制全局生效,逐项/设备证据属阶段5）；撤销维度 FAIL 4→NOT_TESTED 4（undoable 误标已修复,
  保存重开保真属阶段4/8）；其余维度不变（属阶段2/3/5）。reference validation OK: 483 cells
现场依赖与未验证项：PLCWrite 测试只监听本机 127.0.0.1 临时端口，确认前和拒绝后无连接；
  未发送 Modbus 报文，真实设备/协议联调仍属阶段5/8
回退方式：按 §2/§6/§7 文件清单逐一还原本阶段 diff；
  阶段0资产与他人在先修改不受影响
审核结论：PASS（独立审核 Agent 2026-09-30，见 §12；仅阶段 1 G1）
审核依据、日期和后续阶段放行范围：r6 构建/CTest/复现/格式/哈希与产品路径复核通过；允许开始阶段 2，
  不代表阶段 2 或真实 PLC/Modbus、真实显示环境已验收。
```

## 1. 接口契约样例(编码前冻结;§4.2 阶段 1 行)

### 1.1 工具结果:成功与失败明确区分

兼容承诺:**保留既有成功字段**(如 `status/instanceId/…`)与既有失败字段 `error`;新增字段一律增量,不删除、不改名。

失败(所有工具统一):

```json
{
  "error": "No path specified and project has never been saved",
  "errorCode": "SAVE_FAILED",
  "retryable": false,
  "details": { "tool": "save_project" }
}
```

`errorCode` 稳定枚举(阶段 1 冻结子集;新增须走矩阵更新):

| errorCode | 语义 | retryable |
| --- | --- | --- |
| MISSING_PARAM / INVALID_PARAM | 参数缺失/非法值 | false(需修正参数,非原样重试) |
| UNKNOWN_TOOL | 未注册工具 | false |
| NO_PROJECT | 无当前工程 | false |
| PROJECT_UNSAVED | 当前工程有未保存修改,需 `onUnsaved` 决策 | false |
| INSTANCE_NOT_FOUND | 未知目标(模块实例、连线;details.target 区分) | false |
| PRECONDITION_FAILED | 外部前置条件缺失(图像不可读、SAM 服务/推理失败、会话文件状态冲突、会话写入失败) | false(前置满足后可重试) |
| PLUGIN_NOT_FOUND / PLUGIN_NOT_LOADED | 插件不存在/未加载 | false |
| VALIDATION_FAILED | 参数校验失败(含跨字段) | false |
| SAVE_FAILED / OPEN_FAILED | 保存/打开失败 | false |
| INVALID_STATE | 引擎/工程状态不允许该操作 | false |
| ENGINE_BUSY | 运行中,暂不可执行 | **true** |
| PERMISSION_DENIED | 权限级拒绝 | false |
| CONFIRMATION_REQUIRED / _EXPIRED / _TARGET_CHANGED | 需确认/确认过期/确认后目标已变化 | false |
| SKIPPED_DUE_TO_PREVIOUS_ERROR | 批次中未执行项 | false |
| BATCH_LIMIT_EXCEEDED | 单轮工具数超限(>10),整批未执行,逐 call 回结果 | false |
| TURN_LIMIT_EXCEEDED | 自动执行轮数超限(≥20),本批未执行,逐 call 回结果 | false |
| INTERNAL_ERROR | 未预期内部错误 | true(一次) |

显式幂等 `no_op`(非法状态但操作目的已达成/无害,不得谎称完成动作):

```json
{ "status": "no_op", "reason": "engine not running; nothing to pause" }
```

适用:`pause_flow/resume_flow/stop_flow` 在 incompatible 状态。**未知实例不适用 no_op**,一律 `INSTANCE_NOT_FOUND`(F10 修复口径:不存在的对象不能被"移除成功")。

### 1.2 批次协议:每个原始 call 恰有一个结果

请求(控制器内部,来自 LLM tool_calls,保留原始 `id`):

```json
[ {"callId": "call_abc", "tool": "add_module", "params": {"plugin": "GrabImage"}},
  {"callId": "call_def", "tool": "set_param",  "params": {"instanceId": "x", "key": "k", "value": "1"}} ]
```

响应(遇错停止;未执行项补位;参数解析失败同样保留 callId):

```json
{
  "status": "partial_failed",
  "executed": 1,
  "failedTool": "set_param",
  "error": "Module not found: x",
  "errorCode": "INSTANCE_NOT_FOUND",
  "results": [
    { "callId": "call_abc", "tool": "add_module", "executed": true,  "result": {"status": "added", "instanceId": "grab"} },
    { "callId": "call_def", "tool": "set_param",  "executed": true,  "result": {"error": "Module not found: x", "errorCode": "INSTANCE_NOT_FOUND", "retryable": false} },
    { "callId": "call_ghi", "tool": "run_flow",   "executed": false, "result": {"error": "Batch stopped after previous error", "errorCode": "SKIPPED_DUE_TO_PREVIOUS_ERROR", "retryable": false} }
  ]
}
```

不变量(G1):`results.length == 原始 calls.length`;每个 callId 恰好出现一次;`executed:false` 的项零副作用。既有字段 `status/executed/failedTool/error/results[i].tool/results[i].result` 全部保留。

### 1.3 确认:绑定动作、目标与工程/配置版本

确认请求(聊天路径经 `toolsPendingConfirmation`,Bridge 路径经 `pending_confirmation` 响应;同一载荷):

```json
{
  "confirmationId": "cfm_9f2c…",
  "tools": [ {"id": "call_abc", "name": "save_project", "arguments": {"path": "/data/p.dproj"}} ],
  "context": {
    "summary": "保存工程「检测A」到 /data/p.dproj(写文件,不可撤销)",
    "projectName": "检测A",
    "projectPath": "/data/p.dproj",
    "projectVersion": { "filePath": "/data/p.dproj", "contentFingerprint": "<sha1(Project::toJson)>",
                        "undoIndex": 7, "undoCount": 9 },
    "sideEffects": [ "file_write:/data/p.dproj" ],
    "undoable": false
  }
}
```

- `projectVersion` = 工程身份与配置版本快照:filePath + **工程内容 sha1 指纹** + Agent 撤销栈 index/count。内容指纹可检出任何来源(Agent 工具、GUI 编辑、迁移)的窗口期修改;撤销栈位置提供第二重校验与更精确的失效原因。
- 执行前重校验:快照不一致 → 整个批次以 `CONFIRMATION_TARGET_CHANGED` 拒绝,**零副作用**,不凭陈旧确认执行。
- **confirmationId 贯穿批准链路(整改 R2)**:`confirmPendingTools(confirmationId)` / `rejectPendingTools(confirmationId)` 校验提交的 id 与当前批次一致;错配(旧批次迟到事件)返回 false、零副作用、当前批次保持待确认;确认卡展示 context(工程/副作用/摘要)并携带绑定 id 回传;Bridge `pending_confirmation` 响应内嵌完整载荷。
- 拒绝/过期(新请求覆盖、工程切换、对话清理)→ 待确认批次丢弃,零副作用,状态可追踪(审计条目 `result:"rejected"/"expired"`)。
- 阶段 1 保守口径(§6.5):`run_flow`(含 cycle)一律视为 `sideEffects:["execution:may_touch_files_devices_network"]`,Autopilot 也必须确认;流程内实际副作用精细判定属阶段 5。

### 1.4 权限判定:三入口共用单一策略

`AgentController::evaluateToolGate(tool, level) → Allow | RequireConfirmation | Deny`(聊天路径 onLLMResponse/extendAgentLoop、Bridge handleToolCall、确认回调执行前复验,三处共用;Actor 前置条件验证不因批准而跳过):

| 级别 | 只读工具 | 非危险写 | 危险/执行类(run_flow、save_project、sam_annotate…) |
| --- | --- | --- | --- |
| Observer | **Allow(执行并继续问答,F08 修复)** | Deny | Deny |
| Advisor | Allow | RequireConfirmation | RequireConfirmation |
| Autopilot | Allow | Allow | RequireConfirmation(阶段1保守,含 run_flow) |

### 1.5 工程切换保护语义

`create_project/open_project` 新增可选参数 `onUnsaved`(默认 `"cancel"`):

```json
// 当前工程未保存且未指定 → 失败,零副作用(默认保留当前工程)
{"error": "Current project has unsaved changes", "errorCode": "PROJECT_UNSAVED", "retryable": false,
 "details": {"options": ["save", "discard", "cancel"]}}
// onUnsaved="save":先保存当前工程;保存失败 → SAVE_FAILED,不继续切换
// onUnsaved="discard":显式放弃后继续
```

`ProjectManager::openProject` 重构为**先验证后替换**:候选工程完整读入并通过 load/格式版本/迁移校验后,才关闭当前工程、替换指针、清撤销栈;任一环节失败当前工程原样保留(F07 修复)。GUI 切换入口也接入同一未保存保护并提供三态选择（见 §6）。

## 2. 实施项 → 代码映射(全部落地)

| 计划 §6 实施项 | 改动文件与要点 |
| --- | --- |
| 1 统一失败返回 | `AgentActor.cpp`:makeToolError 助手(error+errorCode+retryable+details),63 处原始错误返回全部改造(含 SAM 工具 PRECONDITION_FAILED 分类);`saveProject` 失败携带 SAVE_FAILED+status:failed 兼容;`removeModule` 未知实例→INSTANCE_NOT_FOUND(不再压空撤销命令);pause/resume/stop 非法状态→显式 `{"status":"no_op","reason":…}` |
| 2 统一入口策略 | `AgentController`:新增公开 `evaluateToolGate`(三态),聊天路径 onLLMResponse、Bridge handleToolCall、确认执行前复验(doConfirmPendingTools)三处共用;Observer 只读在聊天路径执行并继续问答(F08 修复),写操作逐项 PERMISSION_DENIED 结构化结果入对话与审计 |
| 3 工程切换保护 | `ProjectManager::openProject` 重构为先加载验证候选(含迁移)成功后才 closeProject+替换(F07 修复,undo 栈经既有 projectClosed 信号联动清理);`AgentActor` create/open_project 增加 `onUnsaved=save\|discard\|cancel`(默认 cancel→PROJECT_UNSAVED 零副作用;save 失败→SAVE_FAILED 不继续);`ToolSchema` 注册 onUnsaved 枚举参数 |
| 4 确认具体化 | `beginPendingConfirmation` 生成 confirmationId+快照(工程身份/内容 sha1 指纹/Agent 撤销栈位置);`pendingConfirmationPayload()` 输出 §1.3 载荷(summary/projectVersion/sideEffects),Bridge 响应内嵌;`confirmationContextStillValid` 执行前重校验,陈旧→整批 expired 拒绝零副作用 |
| 5 间接副作用 | `isExecutionEntryPoint(run_flow)` 并入 Autopilot 确认类(保守口径,不因 schema 未标 dangerous 绕过);阶段5 精细化已在矩阵登记 |
| 6 真实审计/undoable | `isUndoCapableToolName`(仅 5 个工程编辑命令);审计 undoable = 成功∧可撤销命令;save/run/sam/open/create 一律 false;新增 rejected/expired/denied 审计结果值 |
| 7 批次协议 | `AgentActor::ToolInvocation{callId,tool,params}` + 新 executeTools 重载(旧签名兼容包装);预校验失败保留全部 callId 零执行;遇错停止后未执行项补 SKIPPED_DUE_TO_PREVIOUS_ERROR;`extendAgentLoop` 逐项门禁、按原始顺序归并结果,每个 call 恰有一条 tool 消息+一条审计 |

既有兼容:成功字段全部保留;`toolsPendingConfirmation(QJsonArray)` 信号签名不变(确认卡/批准链路已扩展);executeTools 旧签名可用;阶段0 复现套件中未收口阶段(F01–F05/F09/F11/F12)的用例原样通过。

## 3. G1 自检清单(逐条证据;判定权在审核者)

- [x] **未指定路径/不可写路径/无工程保存都返回 error;控制器记录失败,后续写操作不执行** — `test_agentactor::testSaveProjectErrorPaths`(NO_PROJECT/SAVE_FAILED×2);`testF06FixedControllerLogsFailedSaveAsError`(控制器审计 result=error,undoable=false);批次停走见 `testBatchProtocolKeepsCallIdIntegrity`
- [x] **打开不存在/损坏/加载失败,原工程、修改标记和撤销历史保持可用** — `testF07FixedOpenCorruptProjectKeepsCurrent`(OPEN_FAILED+模块保留+canUndo);不存在文件在触碰当前工程前即失败(ProjectManager.cpp exists 检查先于任何关闭)
- [x] **未保存切换的保存/放弃/取消均有回归;保存失败不关闭原工程** — `test_agentactor::testUnsavedProjectSwitchPolicies`(默认拒绝/显式 cancel/save 失败中止/discard 放行/save 成功后免询问,5 条路径)
- [x] **三种权限 × 聊天/Bridge/确认入口有测试;Observer 可读手册和状态,所有写入被拒绝** — `test_agentpermissions`(Bridge×3 级)、`test_agentcontroller::testObserverModeExecutesReadOnlyAndDeniesWriteTools`(聊天)、`testF08FixedObserverChatExecutesReadOnlyLikeBridge`(两入口一致性+写拒绝 PERMISSION_DENIED)、`testPermissionDowngradeDuringConfirmationDenies`(确认入口复验)
- [x] **同轮只读+写不绕过权限;拒绝、过期确认、权限降级零未批准操作** — 混合批次见 Observer 聊天用例(读执行/写拒绝各恰一结果);`testRejectPendingZeroSideEffects`、`testConfirmationStaleWhenProjectEditedInWindow`(内容指纹检出窗口期编辑→expired 零执行)
- [x] **Folder Delete / PLCWrite 流程确认前零文件/设备副作用** — `testF13FixedAutopilotRunFlowRequiresConfirmation`(临时目录:确认前存在、拒绝后存在、批准后才删除);`testG1PlcFlowZeroSideEffectBeforeConfirmation`(确认前引擎零启动、运行计数零变化、拒绝后仍零，回环模拟 PLC 无连接；批准后 Modbus 协议联调属阶段5 G5)
- [x] **保存失败/缺失实例/非法 pause-resume 日志与返回值一致;每个 call ID 恰有一个结果** — `testF06Fixed*`(返回 error↔审计 error 一致)、`testF10FixedUnknownInstanceFailsAndIdleControlIsNoOp`(INSTANCE_NOT_FOUND+撤销栈零增长;no_op 显式)、`testBatchProtocolKeepsCallIdIntegrity`(results 数=原始 calls 数、callId 一一对应、executed 标志、预校验失败同样保全 callId)

## 4. 证据与复现

| 证据 | 位置(sha256 见 agent-phase1-evidence-r6/sha256sums.txt) |
| --- | --- |
| 全量构建日志(BUILD_EXIT=0) | `build-codex-agent/agent-phase1-evidence-r6/build.log` |
| 全量 CTest(78/78,CTEST_EXIT=0) | `build-codex-agent/agent-phase1-evidence-r6/ctest-full.log` |
| 复现套件(17 用例:阶段0 保留+翻转+G1 新增,REPRO_EXIT=0) | `build-codex-agent/agent-phase1-evidence-r6/repro-test-run.log` |
| 命令与退出码 | `build-codex-agent/agent-phase1-evidence-r6/exit-codes.txt` |
| 矩阵(回写后,483 格引用校验 OK) | `docs/baseline/agent-capability-matrix.json` / `.md` |
| 阶段0 原始证据 | `build-codex-agent/agent-phase0-evidence/`(未触碰,含 v1) |

复现命令:

```bash
cmake --build build-codex-agent -j2
env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure
env QT_QPA_PLATFORM=offscreen ./build-codex-agent/bin/test_agentphase0_repro -o build-codex-agent/agent-phase1-evidence-r6/repro-test-run.log,txt
python3 scripts/audit_agent_capability_baseline.py generate \
    --runtime-log build-codex-agent/agent-phase0-evidence/runtime-list-plugins.log
sha256sum -c build-codex-agent/agent-phase1-evidence-r6/sha256sums.txt
```

阶段0 复现用例翻转对照(禁止删测口径):F06→`testF06FixedSaveFailureCarriesErrorCode`+`testF06FixedControllerLogsFailedSaveAsError`;F07→`testF07FixedOpenCorruptProjectKeepsCurrent`;F08→`testF08FixedObserverChatExecutesReadOnlyLikeBridge`;F10→`testF10FixedUnknownInstanceFailsAndIdleControlIsNoOp`;F13→`testF13FixedAutopilotRunFlowRequiresConfirmation`(Actor 层底层事实用例保留为架构特征记录);未收口项(F01–F05/F09/F11/F12)断言原样保留并通过。

## 5. 遗留与后续阶段衔接

1. run_flow 保守确认的**精细化**(按流程实际模块副作用判定、纯计算流程 Autopilot 直通)→ 阶段 5 §5。
2. 批次**恢复策略**(可修正错误反馈模型继续)→ 阶段 7 §3;本阶段按"停止并报告"基线。
3. `PROJECT_UNSAVED` 守卫暴露的既有事实:新建工程经 setName 即处于已修改态(与 GUI 语义一致),已在测试注释记录。
4. 确认卡已展示工程/副作用/摘要并绑定 confirmationId(整改 R2);本轮对 PLCWrite 补充了实际配置 IP/端口、从站与地址，其他设备目标级摘要仍属阶段 5。

## 6. 整改轮变更记录(阶段 1 首轮审核 FAIL → 本轮送审)

| 审核发现 | 整改 | 验证 |
| --- | --- | --- |
| R1:未保存保护只在 AgentActor,GUI 新建/打开(MainWindow.cpp:5387 等)与 ProjectManager 低层入口仍直接丢弃;onUnsaved=save 成功分支无测试 | 保护下沉为 `ProjectManager::resolveUnsavedForSwitch` 共享判定 + `newProjectChecked/openProjectChecked/closeProjectChecked` 受保护入口;MainWindow 新建/打开/方案列表/退出四处接三态对话框(保存后继续/放弃后继续/取消,保存失败不继续);CLIHandler 6 处入口接 Cancel 策略;AgentActor 守卫改为委托同一实现;低层 newProject/openProject 保留为测试/内部构件并在头文件标注 | `test_project::testUnsavedSwitchPoliciesAtManagerLevel`(Cancel/Save失败/Save成功落盘内容校验/Discard/损坏文件先验证后替换/closeProjectChecked 全路径);`test_agentactor::testUnsavedProjectSwitchPolicies` 补 **onUnsaved=save 成功切换分支**(文件含最新状态) |
| R2:confirmationId 未贯穿批准链路(confirmPendingTools 不接收不校验);确认卡不展示工程/目标/副作用 | `confirmPendingTools/rejectPendingTools` 接收 confirmationId 并校验(错配→false,零副作用,当前批次保持);`pendingConfirmationId()` 公开;Bridge pending 响应内嵌完整载荷;AgentToolPreviewCard 新增 ConfirmationContext(工程名/路径/副作用/摘要/绑定 id)展示;AgentChatPanel 信号携带展示时绑定的 id;MainWindow 捕获 id 并对错配给用户可见反馈 | `test_agentcontroller::testConfirmationIdMismatchRejectedZeroSideEffects`(旧批次迟到确认被拒/零副作用/批次保持;正确 id 批准执行;消费后重复 id 拒绝);Bridge 载荷断言并入 `testF06FixedControllerLogsFailedSaveAsError` |
| R3:超限批次(>10 calls)与轮数上限路径不给任何 callId 补结果,assistant 消息留下孤立 tool_calls | extendAgentLoop 两个上限路径逐 call 追加结构化失败 tool 消息(`BATCH_LIMIT_EXCEEDED`/`TURN_LIMIT_EXCEEDED`,已入 §1.1 枚举)+ skipped 审计;无 id 协议垃圾留审计不发消息 | `testAutopilotRejectsTooManyToolCallsInOneTurn`:11/11 callId 各恰一条 tool 消息+11 条 skipped 审计;`testAutopilotStopsAtTurnLimit`:第 21 轮 call_20 获 TURN_LIMIT_EXCEEDED 结果,tool 消息总数 21=20 执行+1 补位 |

整改后全量回归:**78/78 套件通过(CTEST_EXIT=0)**;复现套件 **17/17(REPRO_EXIT=0)**;构建 **BUILD_EXIT=0**;格式门禁与 `git diff --check` 干净;矩阵 483 格引用校验 OK(本轮无矩阵状态变化——R1/R2/R3 属阶段 1 内部收口完整性,不改变维度评分)。

## 7. 独立审核后整改（r2，已复审 FAIL）

独立审核结论为 FAIL，本表只记录整改事实；是否转为 PASS 仍由独立审核者判定。旧 `agent-phase1-evidence/` 保留为历史记录，不再用它证明当前源码/二进制。

| 审核发现 | 最小整改 | 可复核断言 |
| --- | --- | --- |
| 有效 JSON `{}` 被 `Project::load` 视为工程，打开时替换了当前未保存工程 | `Project::load` 拒绝缺失名称、模块/连线数组、错误类型或不支持显式版本的候选；仍允许旧版缺省版本/id | `test_project::testOpenStructurallyInvalidProjectKeepsCurrent`：先观察失败，再修复通过；原工程指针、名称、修改标记和 `projectClosed` 计数不变。`test_projectmigration` 覆盖旧版兼容 |
| LLM `function.arguments` 非法 JSON 静默变 `{}`，`run_flow` 可在确认后执行 | 统一参数解析标记非法；确认前整批转入错误回填，原 call ID 各得一个工具消息及审计，Actor 零执行 | `test_agentcontroller::testMalformedToolArgumentsRejectWholeBatch`：有效调用标 `SKIPPED_DUE_TO_PREVIOUS_ERROR`，坏参数标 `INVALID_PARAM`，无待确认、无引擎启动 |
| `run_flow` 确认只显示泛化副作用，未显示 PLC 目标 | 从工程实例参数与插件默认值合成 PLCWrite 目标；Folder Delete 同时显示目标目录 | `test_agentphase0_repro::testG1PlcFlowZeroSideEffectBeforeConfirmation`：确认载荷包含回环模拟端点 IP/端口；确认前和拒绝后本机模拟 PLC 均无连接；不连接真实设备 |
| GUI 无路径工程“保存后继续”缺少成功链路证据 | 保留已有取消路径测试，补完整保存到临时 `.dproj` 后新建方案的交互测试 | `test_mainwindow::testUnsavedNewSolutionSaveAsSuccessSwitchesProject`：旧工程落盘且关闭一次，新工程生效；无路径另存为取消用例继续通过 |
| 上轮证据哈希与当前源码/二进制不一致，记录含过期“GUI 零改动”说明 | 保留旧证据；另建 `agent-phase1-evidence-r2/`，重新运行构建/全量 CTest/复现，重算源码、二进制及日志哈希；本记录纠正范围说明 | `sha256sum -c` 两份清单须全部通过；日志实际套件数、退出码与本记录一致 |

边界：模拟 PLC 用例只证明未批准前没有连接或运行；它没有证明批准后的 Modbus 报文正确，阶段 5/8 仍需协议和真实设备证据。GUI 用例在 Qt offscreen 环境验证行为，没有截图或真实显示服务器的视觉验收结论。

## 8. 独立复审 FAIL 后的工程数组校验整改（r3，已复审 FAIL）

独立复审确认 r2 的构建、CTest、复现与哈希均通过，但指出 `Project::load` 只检查 `modules`/`connections` 是数组；元素为字符串或空对象时，`fromJson` 会静默转成无身份实例，候选仍会替换当前工程。因此 r2 不放行阶段 2。

`Project::load` 现于替换前逐项要求模块为对象且 `id/moduleId` 非空字符串、连线为对象且 `fromModuleId/toModuleId` 非空字符串。`test_project::testOpenStructurallyInvalidProjectKeepsCurrent` 增加四个损坏候选（非对象/缺身份字段 × 模块/连线），修复前在非对象模块用例退出 1；修复后全部拒绝，原工程指针、名称、未保存标记及关闭信号计数保持。`test_projectmigration` 9/9 通过，旧工程迁移兼容未回退。本轮产品改动仅此加载边界；r3 证据重新生成，r2 原始材料未覆盖。

该轮复审仍为 FAIL；独立审核者发现模块 `params` 若为字符串会被 `fromJson` 静默转成空对象，因此 r3 不放行阶段 2。

## 9. 工程文件已存在字段的类型校验整改（r4，已复审 FAIL）

在加载文件的信任边界，对 `Project::fromJson` 会读取的已知字段先校验 JSON 类型：模块 `params` 必须是对象；模块/连线的可选数值、字符串、布尔字段，以及流程、相机、数据源、迁移记录的已存在字段，均拒绝不匹配类型；保留旧版工程缺省新字段的兼容性。未知扩展字段不作本阶段判断。这样候选文件不会因为 Qt 的 `toObject()` / `toArray()` 空值转换而静默丢失已存在数据，再替换当前未保存工程。

`test_project::testOpenStructurallyInvalidProjectKeepsCurrent` 在 r4 用 10 个损坏候选逐一验证打开失败、原工程指针/名称/修改标记/关闭信号不变；其中 `params` 为字符串的候选在修复前定向退出 1、修复后退出 0。`test_projectmigration` 9/9 通过。r4 的构建、CTest、复现与摘要均重新生成；独立审核指出 `importTime`/日期内容不可解析仍会静默降级，故该轮 FAIL。

## 10. 时间值及整数语义校验整改（r5，格式门禁未过）

`Project::load` 对已存在且非空的 `created`、`modified`、`migratedAt` 字符串要求按 ISO 日期解析有效；保留旧版缺省或空字符串语义。数据源 `importTime` 字符串必须可转换为 64 位整数，数字必须是有限、范围内的整数。模块坐标与连线旧序号也必须是 32 位整数，防止小数截断。仍只在文件加载边界拦截，不变更直接构造工程和旧版缺省字段。

`test_project::testOpenStructurallyInvalidProjectKeepsCurrent` 扩至 16 个损坏候选，新增无效日期、非数值时间和小数；无效 `created` 在修复前定向退出 1，修复后 16 个候选均拒绝并保留原工程。`test_projectmigration` 9/9 通过。独立 Agent 对产品路径已给定向 PASS，r5 构建、CTest、复现与哈希当时也通过；但两处新增测试格式门禁失败，后续格式化使 r5 哈希不再对应当前源码，因此 r5 不作最终放行。

## 11. 新增测试格式门禁整改（r6，独立复审 PASS）

独立审核确认 r5 的 G1 七项功能门槛与原始证据，但发现本阶段新加的 `tests/test_agentactor.cpp` 和 `tests/test_agente2e.cpp` 两处代码未通过 `clang-format --dry-run --Werror`。只对这两处做定向格式化；`test_agentpermissions.cpp` 的既有 HEAD 格式债务未借机改写。格式化后 r5 源码哈希过期，因此保留其原始日志，不再作为当前交付证据。r6 已重新构建、运行全量 CTest/复现并锁定源码和二进制摘要，独立审核结论见 §12。

## 12. 独立审核结论归档（2026-09-30）

独立审核 Agent 对 r6 给出 **PASS：阶段 1 G1 放行，仅此范围**。审核方只读核实构建退出 0、CTest 78/78、复现 17/17、二进制清单 70/70、`git diff --check` 退出 0；除 HEAD 原有 `test_agentpermissions.cpp` 格式债务外，本阶段其余改动 C++ 的格式门禁退出 0。产品路径的 16 个损坏候选均拒绝且保留原工程，旧版迁移 9/9 通过。审核时源码/证据哈希 31/31；归档时仅补入本阶段已修改的 `tests/test_agentpermissions.cpp` 哈希并刷新本记录哈希，增至 32/32，未改产品代码、测试断言、二进制或原始测试日志。

限制：PLC 仅本机回环模拟端点验证确认前/拒绝后无连接，未验证批准后 Modbus 协议及真实硬件；GUI 仅 offscreen 行为验证，未完成真实显示环境视觉验收。阶段 2 可以开始，但不得将此结论扩展为后续阶段通过。
