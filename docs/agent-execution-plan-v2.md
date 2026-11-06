# DeepLux Agent 第二迭代执行计划（v2：对话质量 + 审计 + MCP + 遗留收口）

- 状态：**已交付**（2026-10-28；t21 复验全绿、t22 审核 pass、t16 集成完成；待队长统一提交推送）
- 基线：`af56b5b`（第一迭代已推送 `origin/Windows_work`，2026-10-27）
- 规划依据：[agent-improvement-roadmap.md](agent-improvement-roadmap.md) §11.6 / §11.7；[agent-iteration1-design.md](agent-iteration1-design.md)
- 交付记录：见 [agent-improvement-roadmap.md](agent-improvement-roadmap.md) §12（交付项、偏差、构建测试原始输出、MCP 使用方式、审计格式、第三迭代入口）
- 本文替代 roadmap §11.7 的建议顺序，作为本轮团队执行的唯一任务清单

---

## 1. 目标

在第一迭代"能看见结果"的基础上，补齐四类能力，并收口已知遗留：

1. **对话质量**：SSE 流式输出、reasoning 折叠、增量渲染、取消与回退；
2. **上下文治理**：工具调用组原子裁剪、单条 tool 结果 6k 截断、预算可配、system prompt 摘要化；
3. **可观测**：审计 JSONL 落盘（按天轮转）+ 失败请求报文落盘；
4. **对外能力**：MCP stdio 桥（工具来自 `ToolSchema`，外部调用默认只读/需确认）；
5. **遗留收口**：L1/L2/L3/L5/L8/L9 六项；L4/L6/L7 按第 5 节处理。

完成定义：全部任务终态、审核 pass、复验绑定当前字节、文档回填、`commit + push`、团队归档。

## 2. 阶段与任务

| 阶段 | 任务 | 内容 | 负责 | 依赖 |
|---|---|---|---|---|
| 2A 对话质量 | t11 | 设计冻结（SSE/取消/上下文契约） | architect | — |
| | t12 | SSE 流式 + reasoning + 取消/回退 | implementer-core | t11 |
| | t13 | 上下文治理（组原子裁剪/6k/预算/摘要化） | implementer-core | t12 |
| 2B 审计 | t17 | 设计冻结（审计 JSONL/MCP/遗留修复契约） | architect | t11 |
| | t18 | 审计 JSONL 落盘 + 失败请求报文落盘 | implementer-core | t13,t17 |
| 2C MCP | t20 | `deeplux-mcp-bridge`（stdio JSON-RPC → AgentBridge） | implementer-tools | t17 |
| 2D 遗留 | t19 | L1/L2/L3/L5/L8/L9 修复与测试 | implementer-core | t18 |
| 2A 质量 | t14 | 2A 验证（流式 + 上下文） | verifier | t13 |
| | t15 | 2A 审查（t12/t13） | reviewer | t14 |
| 2A 修复 | t23 | 修复 R2/R4/R5 + 补三项测试缺口 | implementer-core | t15 |
| 2A 复审 | t24 | 复审 t23（只核销 R4/R5/R2；R1/R3 归 t19） | reviewer | t23 |
| 2E 质量 | t21 | 最终验证（清 obj 全量重建 + agent/runengine/mcp/全量） | verifier | t19,t20 |
| | t22 | 最终审查（合并 diff，基线 af56b5b） | reviewer | t21 |
| | t16 | 整轮集成（文档回填 + 最终构建测试 + 变更摘要） | architect | t22 |

并行说明：t20（MCP，新增 `src/app/mcp/**`）与 t18/t19（core 侧）文件不相交，可并行；t19 必须在 t18 之后（同一批文件）。

## 3. 关键契约（摘要，详见 t11/t17 设计文档）

### 3.1 流式（t12）
- 解析 OpenAI SSE：`data:` 行、跨 read 半包、`[DONE]`、`delta.content` / `delta.reasoning_content` / `delta.tool_calls[index]` 累加；
- UI 约 50ms 合批追加；reasoning 仅展示、默认不回传（`agent/reasoningEcho=none`，可设 current-turn/full；第二迭代明示行为变更，需在主文档记录）；
- 流中 `abortCurrentTurn` 立即停止并保留已收内容；**重试只在首个 delta 之前**；
- `agent/streamEnabled`（默认 true）关闭或服务端不支持时回退一次性请求。

> 队长裁决（t11 完成时）：① `reasoningEcho` 默认 `none`（DeepSeek 思考模式回传有 400 风险，保守关闭）；② 6k 截断在**写入历史时**执行（审计与 UI 保留全文）；③ 摘要阈值维持 24。

### 3.2 上下文（t13）
- 裁剪单位 = "助手工具调用组"（assistant(tool_calls) + 全部 tool 同生共死），禁止孤儿 tool 消息；
- 单条 tool 结果 >6000 字符截断保留首尾并标注省略量（只影响回传模型）；
- `agent/contextBudgetTokens` 默认 128000（clamp [16000, 1000000]）；
- 模块数 >24 时 system prompt 摘要化（计数 + 前 24 个 + 提示 `get_flow_state`）。

### 3.3 审计（t18）
- `~/.deeplux/logs/agent-audit-YYYYMMDD.jsonl`，按天轮转、单文件 10MB 上限、保留 14 天；
- 字段复用 `AgentActionLogEntry`（含 durationMs / errorCategory / toolCallId）；
- LLM 失败请求完整报文落盘 `llm-failed-YYYYMMDD.json`（脱敏：Authorization/Key 不得入盘）；
- 写盘失败只记日志，不影响对话。

### 3.4 MCP（t20）
- 新目标 `deeplux-mcp-bridge`：stdin/stdout JSON-RPC 2.0（`initialize` / `tools/list` / `tools/call` / `ping`），转发到本机 `AgentBridge`（QLocalServer，沿用 v1.0 协议与 `agent/bridgeToken`）；
- 工具清单来自 `ToolSchema`（只导出元数据 + JSON Schema）；
- **安全口径**：外部调用默认 Observer（只读），写工具返回 `code=permission_denied` 并提示在 GUI 内确认；不得绕过权限直接执行；
- 与 GUI 未运行时：返回 `-32001` 明确错误，不重试挂死。

### 3.5 遗留修复（t19）
| ID | 修复 | 验收 |
|---|---|---|
| L1 | `tune_node_and_verify` 超时/中止分支补自动化测试（抽纯函数化等待策略或测试注入点） | 用例覆盖超时返回结构 |
| L2 | 测试专用 `ModuleBase` 子类产出自定义端口类型，覆盖 `unsupportedPorts` | 用例断言 unsupportedPorts 非空 |
| L3 | `OpenAILLMClient::heuristicSupportsVision(model)`，配置缺省时使用 | 单测覆盖常见模型名 |
| L5 | `RunEngine` 快照 API 的 `m_nodeOutputs` 读写加锁（`m_lifecycleMutex`），保留 `!isBusy()` 前置 | 并发单测/断言；TSan 门禁延后 |
| L8 | `AgentBridge` 外部 `tool_call` 对 `run_flow`/`tune` 等 `wait=true` 工具强制 `wait=false` | IPC 协议测试 |
| L9 | 单轮全局 deadline（`agent/turnDeadlineMs`，默认 600000）到点中止并回报 | 用例覆盖超时中止 |

## 4. 质量门

1. 每个实现任务：vcvars64 全量构建 exit 0；`ctest -R "test_agent"` 全绿；改动过 `.h` 先删对应 `*.obj`（roadmap §11.5 L7）。
2. 验证任务：清空 `build-win-tests` 全部 obj → 全量重建 → 跑 agent / runengine / 全量 ctest，给出 mtime 证据链。
3. 审核：以设计文档为基准逐条核对；needs_revision 自动进入 repair + 下一轮 review。
4. 复验：review 修复后必须重新验证，避免旧验证结论绑定旧字节。
5. 集成：只改 docs；最终构建测试通过后由队长提交推送。

## 5. 明确不做（本轮）

| 项 | 理由 |
|---|---|
| 多服务商预设 / Key 系统凭据存储 | 工作量独立，列入第三迭代；本轮只做 MCP 最小可用 |
| 可取消单节点执行 + `sweep_node_parameter` | 依赖 RunEngine 异步节点作业与取消协议，L5 锁定后再做（第三迭代） |
| L4 `create_project` 死分支 | 防御性代码，`DEEPLUX_TEST_VISIBILITY` 已覆盖，保持现状 |
| L7 构建环境根治 | 本轮继续用"改头文件删 obj"纪律；CI 改造另立任务 |
| TSan 门禁 | 先完成 L5 加锁；TSan 纳入 CI 需独立环境 |
| `.agent-teams/` 入库 | 团队运行态，不入库；如需忽略另加 `.gitignore` |

## 6. 完成与收尾

1. t16 集成通过后，队长执行：`git add src tests docs`（排除 `.agent-teams/`、`build*`）→ 单个 `feat:` 提交 → `git push origin Windows_work`；
2. 推送成功后归档团队 `agent-iteration1`；
3. 交付说明包含：变更摘要、测试证据、偏差与遗留、第三迭代入口（多服务商、可取消单节点执行、sweep、TSan）。

## 7. 估算

| 阶段 | 人日 | 说明 |
|---|---|---|
| 2A 流式 + 上下文 | 2.5~3.5 | 设计 0.5 + 实现 2 + 测试 |
| 2B 审计 | 1 | 设计并入 t17，实现 1 |
| 2C MCP | 1.5~2 | 桥接进程 + 权限映射 + 协议测试 |
| 2D 遗留 | 1.5~2 | 六项含测试 |
| 2E 质量 + 集成 | 1 | 验证/审查/文档/提交 |
| 合计 | 7.5~10.5 人日 | 团队并行后墙钟更短 |

## 8. 交付结果（2026-10-28）

### 8.1 任务结果

| 任务 | 结果 | 关键产物 / 说明 |
|---|---|---|
| t11 | **pass** | `docs/agent-iteration2-design.md`（843 行：流式、回退、渲染、取消/重试、上下文、摘要化、测试与所有权） |
| t12 | 完成（并入 t22 审查） | SSE 流式 + reasoning + 取消/回退；`SseStreamParser`、`OpenAILLMClient` 流式路径、`test_agentstream`（26 断言） |
| t13 | 完成（并入 t22 审查） | 上下文治理：工具组原子裁剪、6k 截断、预算可配、`buildContext` 摘要化；`test_agentcontroller` +275 行 |
| t14 | pass | 2A 验证（流式 + 上下文） |
| t15 | needs_revision（findings 转 t23） | 2A 审查第一轮；R2/R4/R5 + 三项测试缺口交由 t23；R1/R3 归 t19 |
| t17 | **pass** | `docs/agent-iteration2b-design.md`（840 行：审计、失败报文、MCP 桥/权限、L1~L9 契约） |
| t18 | 完成 | `AgentAuditLog`、`FailedRequestLog`、失败钩子；`test_agentaudit`（16 断言） |
| t19 | 完成 | L1/L2/L3/L5/L8/L9 + R1/R3 + 设置页数值框；新增 20 个测试槽 |
| t20 | 完成 | `deeplux-mcp-bridge`（`src/app/mcp/**` + `src/app/CMakeLists.txt`）；`test_mcpbridge` 15 槽≈90 断言 |
| t23 | 完成 | R4（回退后停止消费同批事件）、R5（首 delta 前失败终止流）、R2（truncated 审计条目）+ 三项测试缺口 |
| t24 | **pass** | t23 复审（只核销 R4/R5/R2） |
| t21 | **pass** | 清 obj 全量重建 + 全量回归（见 §8.2） |
| t22 | **pass（无 findings）** | 合并 diff 审查（基线 af56b5b），覆盖 t12/t13/t18/t19/t20/t23 |
| t16 | **pass** | 本文档 + roadmap §12 回填、最终构建测试、变更摘要与第三迭代入口 |

### 8.2 最终验证证据

```text
t21 复验：删 414 obj → [573/573] 414 TU，0 error，2.98 min，exit 0
          test_agent 15/15、test_mcpbridge 1/1、test_runengine 1/1、全量 84/84，exit 0
          SHA256 守卫 88 个二进制 0 变化；头文件 mtime ≤00:10:19 < obj 00:15:28–00:17:41 < 二进制 00:15:37–00:18:05
t16 集成：build exit 0（[1/1] Staging；源文件 00:10:19 < obj 00:15:12 < dll 00:15:37 < mcp 00:18:03）
          ctest -R test_agent → 15/15，0 failed，31.41 s，exit 0
t20 隔离目录：build-win-mcp 全量 619 目标 exit 0；test_mcpbridge 1/1
```

- 测试总数 80 → **84**；`test_agent*` 12 → **15**；插件 ABI 未变（无需 `sync-plugins`）；19 个改动 C++ 文件 clang-format 0 violations。
- 45 条验收（t12 9 + t13 6 + t18 7 + t19 10 + t20 7 + t23 6）全部 passed。

### 8.3 偏差与裁决记录（详见 roadmap §12.2）

1. `agent/reasoningEcho` 默认 `none`（明示行为变更，队长批准；可设 current-turn/full）。
2. 6k 截断在写入历史时执行（队长批准；审计/UI 保留全文）。
3. 摘要阈值/预览 24/24（队长批准）。
4. t20 契约文本把 `-32002/-32003` 写反、默认超时写 120s；实现按冻结设计（-32002 超时 / -32003 鉴权 / 默认 30000ms clamp[1000,300000]），记为文本笔误。
5. MCP 只读在桥内预检（不改 core）；只读调用仍经 AgentBridge→`AgentController` 权威判定；写工具不转发。
6. L1 用 `AgentRunWaiter` 注入（等价、生产不可达）；L2 用测试 ModuleBase + RunEngine 锁内注入钩子。
7. 失败报文按"脱敏 + 上限"存储（不存完整历史；Auth 不落盘）。
8. 新增 t23/t24 两个补轮（修复 + 复审），交付范围未变。
9. t20（MCP）按队长指示在**隔离目录 `build-win-mcp`** 完成构建与自测（未影响 `build-win-tests` 的 2A 快照）；t21 已把 MCP 目标并入 `build-win-tests` 做全量验证（`Linking deeplux-mcp-bridge.exe` / Test #49）。

### 8.4 收尾状态

- ✅ 文档：本文档状态更新、roadmap §12 交付记录、两份设计文档（iteration2/2b）已入库（`docs/`）。
- ✅ 代码/测试已在 `Windows_work` 提交为 `860523b`；该提交以 `af56b5b` 为基线，Linux 集成时还需保留 `7e9ee05` 的客户端生命周期修复。
- 后续工作见 roadmap §12.8；`.agent-teams/` 和 `build*` 不属于交付物。
