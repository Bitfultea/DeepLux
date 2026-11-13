# 阶段 7 验收记录

> 历史验收快照：本文状态与结果对应其记录日期和基线；当前项目状态见 [docs/STATUS.md](../STATUS.md)。

## 当前状态

**PASS（软件/离线范围）。** G7.1–G7.6 均已通过自动化验收及独立只读复审；全量构建成功，CTest 109/109 通过。阶段 8 的真实 provider、真实设备/SDK、目标平台和现场行为仍须单独验收，不能由本阶段结果替代。

## 基线审计

- `AgentController::extendAgentLoop` 对 `AgentActor::executeTools` 的 `partial_failed` 批次当前立即进入 Idle 并发出错误；无受限参数纠错状态。
- `AgentActor::executeTools` 在调用任何工具前执行完整批次参数校验，可识别 `INVALID_PARAM`、`MISSING_PARAM` 和 `UNKNOWN_TOOL`，并为每个原始 callId 返回结果。该失败分支执行数为 0，适合作为自动修正的唯一初始范围。
- 工具调用后产生的失败可能已经有副作用；当前结果统一 `retryable=false`。G7.3 不得自动重试这些结果、部分失败批次、用户拒绝、权限拒绝、超时或 uncertain side effect。
- 自动化既有覆盖包括批次原子验证、调用 ID 对账、权限拒绝、超限和部分副作用错误；本次全量回归 108/108 通过。此前没有“模型修正参数后成功”“同错同参停止”或“纠错次数上限”断言。

## G7 状态矩阵

| 标准 | 状态 | 证据 |
|---|---|---|
| G7.1 生产代码执行和等待 | PASS | Slice 3：假模型不等待 RunEngine；wait_for_run 由生产控制器轮询；独立审核 PASS |
| G7.2 问答/建流程/取消/拒绝/断连/超时/迟到响应 | PASS | Slice 4 收口 Agent 取消和迟到响应；既有 controller/E2E 覆盖问答、建流程、拒绝、provider error/timeout；独立审核 PASS |
| G7.3 有界参数纠错与副作用保护 | PASS | Slice 1：有限纠错、相邻签名、权限/副作用门禁；独立审核 PASS |
| G7.4 严格 provider 请求/响应 fixture | PASS | Slice 2：OpenAI-compatible 请求/响应、图像/历史 Schema 与工具禁用门禁；独立审核 PASS |
| G7.5 最终结果有实际工具引用 | PASS | Slice 5：当前轮 callId 结果引用；缺失结果/不确定状态确定性收口；FindCircle 实际输出 E2E；独立复审 PASS |
| G7.6 UI 状态与凭据保护 | PASS | Slice 6：忙碌/取消/失败 UI 自动化；初次载入及保存前 API Key 密码回显像素对比；Agent 审计无凭据；独立复审 PASS |

## G7.3 Slice 1 验收

- 契约：本地逐切片契约（未纳入提交）；错误范围仅为全批预校验零执行且明确可纠正的 `INVALID_PARAM`、`MISSING_PARAM`、`UNKNOWN_TOOL`。
- 行为覆盖：`INVALID_PARAM`、`MISSING_PARAM`、`UNKNOWN_TOOL` 纠错成功且 callId 一一对应；相同签名连续两次停止；A→B→A 允许继续；单请求最多三次；新请求重置预算；provider error 清空预算和签名；Observer denied 与 `MISSING_PARAM` 同批不继续请求；已执行部分失败保留原副作用且不重试；`disconnect_camera` 参数错误可纠正但保留硬件未断开标记。
- 验证：全量构建退出码 0；`test_agentactor`、`test_agentcontroller` 聚焦 CTest 2/2；全量 CTest 在受限沙箱首次有 13 个依赖 loopback listener 的环境失败项，授权 loopback 后复跑 108/108、0 失败、0 跳过；clang-format 与 `git diff --check` 干净。
- 独立复核：`/root/slice176_final_review` 结论 PASS，三项首轮发现及后续 LLM error 清理均已确认关闭。
- 证据目录：`build-codex-agent/agent-phase7-slice1-final-evidence/`；包含受限/loopback 全量 CTest 日志、构建日志、聚焦 CTest 日志、复现日志、审核结论及摘要清单。

## G7.4 Slice 2 验收

- 契约：本地逐切片契约（未纳入提交）；只声明当前实现的 OpenAI-compatible Chat Completions，不外推原生 Claude 兼容或真实 provider 成功率。
- Fixture 精确核对真实 HTTP 请求方法/路径、Bearer 头、模型参数、reasoning 配置、完整 `ToolDefinition` function schema、system/user/assistant/tool 消息、历史 tool-call 与 tool_call_id、合成图像 data URL 字节；响应完整核对 tool-call 对象与 arguments、reasoning 和 token usage。
- 工具关闭：Settings fixture 核对请求省略 tools；AgentController 将禁用状态写入上下文，并对 provider 违规返回的 tool_calls 做执行门禁，逐 callId 回填 `TOOLS_DISABLED`、留下 denied 审计、零副作用且停止本轮。默认提示中依赖工具的能力规则都限定在工具可用时。
- 验证：全量构建退出码 0；provider/settings/controller 聚焦 CTest 3/3；完整 `test_agentcontroller` 86/86；全量 CTest 109/109、0 失败、0 跳过（需允许本机 loopback fixture）；clang-format 与 `git diff --check` 干净。
- 独立复核：`/root/slice176_final_review` 结论 PASS，检查完整请求/响应断言、工具禁用执行门禁、默认提示限定与合成凭据处理。
- 证据目录：`build-codex-agent/agent-phase7-slice2-evidence/`；包含全量构建、聚焦 CTest、完整控制器用例、全量 CTest、review 结论及清单。

## G7.1 Slice 3 验收

- 契约：本地逐切片契约（未纳入提交）；假 LLM 只按对话 tool result 产生下一条请求，不读取/轮询 RunEngine，也不预置业务输出。
- 实现调整：端到端脚本从 `run_flow` 结果提取实际 `runId` 并请求 `wait_for_run`；运行中的重试等待完全由 AgentController 的 pending-query 轮询完成，然后再把终态 tool result 发给模型。
- 验证：`test_agente2e` 建立 GrabImage→FindCircle 流程、真实运行固定图像、读取已完成统计和检测值；pending 与终态审计分别为 pending/success；定向重复 20/20；全量构建退出码 0；全量 CTest 109/109、0 失败、0 跳过；clang-format 与 `git diff --check` 干净。
- 独立复核：`/root/slice176_final_review` 结论 PASS，确认 fake LLM 无 Project/Actor/RunEngine 调用、无引擎状态读取/轮询，生产等待和 c7 终态闭环均有断言。
- 证据目录：`build-codex-agent/agent-phase7-slice3-evidence/`；包含全量构建、20 次重复、全量 CTest、review 结论及哈希清单。

## G7.2 Slice 4 验收

- 契约：本地逐切片契约（未纳入提交）；用户取消只取消 LLM 请求或 Agent 对 pending 运行结果的等待，不停止已经启动的 RunEngine 流程；待确认仍由确认卡管理。
- 请求隔离：每次模型请求携带仅供客户端/控制器关联的 requestId；OpenAI-compatible 成功响应和失败信号保留该 ID；取消/替换请求后，旧 reply 的迟到 finished/error 不可读取新 reply 或改变新状态。旧版未提供 correlation 的 ILLMClient 可继续工作，但不能获得强迟到响应隔离。
- UI/等待：Stop 点击取消，Enter 忙碌期间保留草稿和附件；pending wait tool message 与审计置 cancelled、旧 generation 失效、流程保持运行。
- 验证：全量构建退出码 0；定向 CTest 8/8；全量 CTest 109/109、0 失败、0 跳过；clang-format 与 `git diff --check` 干净；新增 OpenAI A 网络失败后同步启动 B 的回环竞态 fixture。
- 独立复核：`/root/slice176_final_review` 复审 PASS；首轮发现“旧 reply 的迟到 finished 可能消费新 reply”已通过 per-reply QPointer/requestId 回调、sender/current 校验及错误时断开回调关闭。
- 证据目录：`build-codex-agent/agent-phase7-slice4-evidence/`；包含完整构建、全量 CTest、OpenAI fixture 20 次重复、独立审查、二进制摘要与哈希清单。

## G7.5 Slice 5 验收

- 契约：本地逐切片契约（未纳入提交）；证据只来自当前用户轮次与同 callId 的真实 tool message。节点标识仅在工具响应不回显时从同一 callId 请求参数读取 instanceId；状态和值严格取工具结果。
- 实现：运行类调用缺少对应 tool message 时生成 `MISSING_TOOL_RESULT` 并拒绝模型成功文案；pending/failed/timeout/uncertain/cancelled/unavailable/truncated/stale/error/errorCode 同样由控制器确定性收口。最终文本同步写入 conversation history 与 UI 信号。
- 覆盖：控制器用例验证旧轮隔离、成功结果引用、uncertain、缺 tool result、缺 value；FindCircle E2E 从本轮 get_module_output 实际响应展示 callId、节点、key 和数值，并精确校验 conversation history 与 UI 回复一致。
- 验证：全量构建成功；`test_agentcontroller` 与 `test_agente2e` 定向 CTest 2/2；全量 CTest 109/109、0 失败、0 跳过；格式与 `git diff --check` 干净。
- 独立复核：`/root/slice176_final_review` PASS；首轮“相关 call 缺失 tool message 会放行成功文案”已补 fail-closed 逻辑和回归用例。
- 证据目录：`build-codex-agent/agent-phase7-slice5-evidence/`。

## G7.6 Slice 6 验收

- 契约：本地逐切片契约（未纳入提交）；只声明离线 Qt UI 和应用审计行为。
- UI 覆盖：`testAgentThinkingStatusStaysCompactAndSafe`、`testAgentComposerPreservesDraftAndAttachmentsWhileBusy`、`testAgentInputErrorPathDoesNotCrashOrStayThinking`、`testAgentPartialFailureSummaryAppearsInChat` 均由 `test_mainwindow` 覆盖；`test_mainwindow` 全套通过。
- 凭据：`testOpenCancelManualSaveAndModalReuse` 验证打开时已载入密钥与保存前输入密钥均为 Password echo；替换为等长 canary 后 API Key 输入框像素图像完全一致；审计 actor/action/params/result 不含合成凭据或模型名；保存后真实本地 HTTP fixture 请求携带新凭据。
- 验证：`test_agent_settings_handoff` loopback 授权运行通过；全量 CTest 109/109、0 失败、0 跳过；全量构建成功；格式与 `git diff --check` 干净。
- 独立复核：`/root/slice176_final_review` PASS；首次 PNG 压缩字节检查已改为输入框像素级对比，并覆盖初次载入和保存前两种状态。
- 证据目录：`build-codex-agent/agent-phase7-slice6-evidence/`。

## 放行规则

仅当 G7.1–G7.6 全部有可复现测试、证据归档并经独立审核后，阶段 7 才可标记 PASS。阶段 8 真实 provider 验收不得由本阶段的 HTTP fixture 替代。
