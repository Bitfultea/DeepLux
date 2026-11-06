# DeepLux Agent 第二迭代设计冻结（流式对话 + 上下文治理）

- 状态：**冻结（实现依据，改动需 architect 复核）**
- 上游文档：
  - [agent-improvement-roadmap.md](agent-improvement-roadmap.md) §6（第二迭代规划）、§11.7（第一迭代交付后的入口建议）
  - [agent-iteration1-design.md](agent-iteration1-design.md)（第一迭代已交付契约；本文只覆盖增量，未重申部分仍以其为准）
- 基线：`af56b5b`（第一迭代交付提交，2026-10-27；本文件所有源码行号以此提交为准）
- 任务映射：[agent-execution-plan-v2.md](agent-execution-plan-v2.md)（t12 流式与渲染 / t13 上下文治理；两者串行，t13 依赖 t12）
- 读者：implementer-core（t12 / t13）、verifier、reviewer
- 适用代码：`src/core/agent/**`、`src/ui/widgets/AgentChatPanel.*`、`src/ui/widgets/AgentMessageBubble.*`、`src/ui/views/MainWindow.cpp`、`src/ui/dialogs/AgentSettingsDialog.*`
- 文档用途：实现者照此编码即可；凡与 roadmap §6 冲突处，以本文为准并注明理由。

---

## 0. 阅读顺序与生效规则

1. 先读 §2（现状基线，全部行号已在 `af56b5b` 上核对）；再读你任务对应的契约章：t12 → §3/§4/§5/§6.2；t13 → §7/§8；§9/§10 所有人必读。
2. `必须`/`禁止`/`不得` 是验收项；`应当` 是强建议，偏离需在 PR 描述说明。
3. 本迭代**不修改**第一迭代已冻结的契约（§3.2 权限表、§4 工具契约、§5 异步协议、§6 图像回灌、§7 错误分类与重试矩阵）；本文只做增量与两处**明示的行为变更**（§1.2 第 8 条）。
4. 任何新增配置键、信号、错误码，必须先改本文再改代码。

---

## 1. 范围与硬不变量

### 1.1 本迭代交付

| 编号 | 交付 | 主责 |
|---|---|---|
| A | SSE 流式解析与增量回调（含工具调用分片累加、半包/多包、异常容错） | t12 |
| B | 非流式回退（配置开关、服务端不支持/解析失败回退、用户可见提示） | t12 |
| C | 增量渲染（50ms 合批、reasoning 折叠块、终态与中止） | t12 |
| D | 取消/重试边界（流中 abort 保留已收内容；首 delta 后可重试性收紧） | t12 |
| E | 上下文治理（工具组原子裁剪、单条 tool 结果 6k 截断、预算可配、首条非 system 为 user） | t13 |
| F | `buildContext()` 摘要化（模块数阈值、前 N 个、提示改用 `get_flow_state`） | t13 |
| G | 测试（分片 SSE stub、取消、回退、工具组裁剪不变量、6k 截断、摘要化）与所有权 | t12/t13 |

> **不在本文范围**：审计 JSONL 落盘、MCP stdio 桥、L1/L2/L3/L5/L8/L9 遗留修复（分别属 t18/t20/t19，契约另见 t17 设计文档，见 [agent-execution-plan-v2.md](agent-execution-plan-v2.md) §2）。本文若与那些契约冲突，以各自文档为准；共同约束（硬不变量）以 §1.2 为准。

### 1.2 硬不变量（任何实现都不得破坏）

1. **插件 ABI 不变**：不改 `src/core/interface/IModule.h`、不改 `DEEPLUX_MODULE_INTERFACE_VERSION`、不改 `src/plugins/**` ⇒ 本迭代**无需** `sync-plugins`。
2. **不新增第三方依赖**：SSE 解析手写（`QByteArray`/`QJsonDocument`），仅用 Qt Core/Network/Widgets/Test；禁止引入事件流库或 HTTP 客户端库。
3. **不改 `RunEngine`**：`src/core/engine/RunEngine.h/.cpp` 本迭代零改动（工具等待/快照 API 维持第一迭代形态）。
4. **不改工具 schema**：`ToolSchema.h/.cpp` 与 `AgentActor.h/.cpp` 的工具集合、参数、权限、错误码**零改动**（仍是 25 个工具）。流式只影响"模型如何返回"，不影响"工具如何被调用"。
5. **既有信号不删不改签名**：`llmResponseReceived / llmErrorOccurred / toolsPendingConfirmation / agentLoopIterating / agentStateChanged / agentTurnAborted / llmUsageReported` 全部保留；只允许**新增**信号。
6. **历史协议合法性**：`assistant(tool_calls)` 与它的全部 `tool` 消息必须**同生共死**；发给模型的列表（system 由 `AgentConversation.systemPrompt` 单独承载）**首条必须为 `user`**。任何裁剪/截断都不得破坏这两条。
7. **UI 不冻结**：流式增量只做"追加文本"，不得在 GUI 线程忙等或每 delta 全量重排；合批刷新周期 50ms（§5.2）。
8. **两处明示行为变更**（属第二迭代目标，必须在 roadmap/交付记录里写明）：
   - `agent/reasoningEcho` 默认 `none` ⇒ reasoning **不再回传请求**（第一迭代基线是"有则回传"，见 `ILLMClient.cpp:44-47`）；需要回传的服务商用配置恢复（§3.7）。
   - `agent/contextBudgetTokens` 默认 **128000**（第一迭代硬编码 256000，见 `AgentController.cpp:803`）；预算收紧会让修剪更早发生（§7.1）。
   - **集成收口 B3 追加**：删除"最新 user turn 永不裁剪"的单条规则，改为保护集合（§7.3 I4）；保护集合超限时以 `context_budget_exceeded` 失败关闭。自动回灌图片消息不再产生新的用户任务边界。
   除这些条目外，不配置任何新键时行为与第一迭代等价。
9. **单一事实源**：错误分类与重试策略仍只在 `OpenAILLMClient::classifyError / maxRetriesFor / retryDelayMs`（`OpenAILLMClient.h:49-55`）；预算/阈值只经 `AgentController` 的配置读取函数；SSE 解析只经 `SseStreamParser`。

---

## 2. 现状基线（基线 `af56b5b`，实现前必读）

### 2.1 LLM 客户端（非流式）

- `ILLMClient` 抽象：`AgentResponse`（`ILLMClient.h:99-108`，含 `reasoningContent`/`errorCategory`/token 字段）、`LLMErrorInfo`（`:60-69`）、信号 `responseReceived/errorOccurred/errorOccurredClassified/retryScheduled`（`:141-147`）、`abortRequest()`（`:139`，非纯虚空实现）。
- `OpenAILLMClient` 请求体：`buildRequestBody`（`OpenAILLMClient.cpp:243-269`），其中 **`:249` 明确注释"暂不使用 stream"**；当前无 `stream`/`stream_options`/`Accept` 头。
- 发送：`sendRequest`（`OpenAILLMClient.cpp:271-289`，重置重试状态、保存 `m_lastBody/m_lastTools`）→ `postRequestBody`（`:291-305`，`setTransferTimeout(30000)` 在 `:297`，`finished/errorOccurred` 连接在 `:303-304`）。
- 中止：`abortRequest`（`:307-319`）置 `m_aborted`、停重试计时器、断开并 abort reply；`onReplyFinished`/`onNetworkError`/`handleFailure` 首行均判 `m_aborted`（`:325`、`:351`、`:368`）——主动中止不分类、不重试。
- 失败与重试：`onReplyFinished`（`:323-348`）→ `classifyError`（`:120-194`）→ `handleFailure`（`:367-397`，退避重试只重发 `m_lastBody`）。
- 非流式解析：`parseResponse`（`:399-450`）一次性解析 `choices[0].message`，`tool_calls` 中 `arguments` 若为对象则压成紧凑字符串（`:433-443`）；usage 在 `:445-447`。
- 重试策略：`maxRetriesFor`（`:196-212`）、`retryDelayMs`（`:214-239`）。
- 客户端成员：`OpenAILLMClient.h:67-87`（`m_currentReply`、`m_lastBody`、`m_retryTimer`、`m_aborted` 等）。

### 2.2 控制器

- 常量：`kMaxAgentToolTurns=20`、`kMaxToolCallsPerTurn=10`、图像预算（`AgentController.cpp:28-36`）。
- 发送：`sendUserMessage`（`:344-371`）/`sendUserMessageWithImages`（`:373-439`）→ `trimHistoryIfNeeded()`（`:361`/`:429`）→ 构造 `AgentConversation`（`ctx.messages = m_conversationHistory; ctx.systemPrompt = buildContext()`，`:367-369`/`:435-437`）→ `sendRequest`。
- 响应收口：`onLLMResponse`（`:443-498`）——唯一把 assistant 消息写入历史的地方（`:454-460`），随后按 tool_calls/权限分支到 `Confirming`/`Executing`/`Idle`。
- 工具循环：`extendAgentLoop`（`:586-673`）——批量执行后为**每条** tool call 追加一条 `role=tool` 消息（`:630-640`，内容为紧凑 JSON `:638`），再 `trimHistoryIfNeeded()`（`:647`）并续跑。
- 中止：`abortCurrentTurn`（`:232-254`）——`++m_turnGeneration`、清 pending、`abortRequest()`、`abortRunningTool()`、回 `Idle`、发 `llmResponseReceived("Stopped by user.")` 与 `agentTurnAborted()`（`:251-253`）。`m_suppressNextLlmError`（`.h:158`）用于抑制紧随其后的错误。
- 分类错误槽：`onLLMErrorClassified`（`:899-927`）与 `onLLMError`（`:929-942`）。
- 上下文：`buildContext`（`:316-340`）——当前把**全部**模块名拼进 `## Current State`（`:320-326`），`m_observer->recentEvents(5)`（`:333`）。
- 裁剪：`trimHistoryIfNeeded`（`:800-855`）——硬编码 `maxEstTokens=256000`（`:803`）；估算 = 0.3 token/字符 + 20/消息 + 图片字节/3（`:805-815`）；按"第 2 条 user 消息"整轮丢弃（`:823-835`）；单 turn 内**逐条**删除 `removeAt(1)` 再删紧跟的 tool（`:849-853`）——**这一步可能拆散 tool 组，是本迭代必须替换的已知缺陷**；硬上限 `kMaxMessages=64`（`:842`）。
- 历史容器与可见性：`m_conversationHistory`（`.h:144`，测试构建下 public，见 `.h:121` 的 `DEEPLUX_TEST_VISIBILITY`）。

### 2.3 UI

- 面板接口：`addMessage`（`AgentChatPanel.h:32`；实现 `AgentChatPanel.cpp:190-204`，内部置 `m_lastAgentBubble`）、`appendToLastMessage`（`.h:33`；实现 `.cpp:206-209`）、`setThinking/setExecuting/setStopped/setError`（`.h:57-62`）、信号含 `stopRequested`（`.h:72`）。
- 气泡接口：`AgentMessageBubble`（`AgentMessageBubble.h:56-68`）当前只有 `setText/appendText/text/setImages/applyTheme/markdownToHtml`，**无 reasoning 折叠块**。
- `reasoningContent` 目前**完全不展示**：控制器只写入历史（`AgentController.cpp:458`）与估算（`:809-810`）。
- MainWindow 接线：设置应用 `MainWindow.cpp:838-840`（`setToolsEnabled/setVisionEnabled/setThinkingEnabled`）；响应处理 `:1716-1723`（`llmResponseReceived` → `setThinking(false)` + `addMessage(Agent, content)`）；状态机 `:1783-1801`；设置加载 `:6140-6188`（`agent/…` 键，`:6182` 起）。
- 设置对话框：getter 在 `AgentSettingsDialog.h:28-37`，成员控件 `:44-53`；`streamEnabled`/预算控件需新增。

### 2.4 历史/裁剪的既有测试（不得回归）

| 测试（`tests/test_agentcontroller.cpp`） | 行号 | 现有断言 |
|---|---|---|
| `testTrimHistoryPreservesMessagePairs` | `:427-493` | 裁剪后首条为 user；每条 tool 都能在**最近的** assistant toolCalls 里找到匹配 id |
| `testTrimHistoryEnforcesHardMessageLimit` | `:495-513` | 80 条消息 → 裁剪后 `size<=64`、首条 user、末条 `assistant-39` |
| `testTrimHistoryEnforcesHardLimitWithinSingleUserTurn` | `:515-538` | 单 user turn + 35 组 assistant/tool → `size<=64`、首条 user、`[1]` 为 assistant |
| `testTrimHistoryAccountsForImageBytes` | `:540-560` | 2MB 图片旧轮被整体丢弃，只剩最新 user |

> 迭代 2 的新算法**必须**让这四个用例保持绿（§7.9 有逐条兼容性说明）。

### 2.5 测试基础设施

- `test_agentretry.cpp:22-110` 的 `LlmHttpStub` 提供最小 HTTP 服务（按脚本返回状态码/响应体、记录 `requestBodies/requestCount`）——流式 stub 应复用其骨架并扩展"分片写"能力（§9.1）。
- 所有测试 target 同时链接 `deeplux_core` 与 `deeplux_ui`（`tests/CMakeLists.txt:133-137`），因此流式测试可以同时覆盖客户端与面板。

---

## 3. 流式协议（t12 主责）

### 3.1 请求形状

`sendRequest` 在开启流式时（§4.1）构造：

```json
{
  "model": "...", "temperature": 0.3, "max_tokens": 4096,
  "messages": [...], "tools": [...], "tool_choice": "auto",
  "stream": true,
  "stream_options": {"include_usage": true}
}
```

- `stream_options` **仅**当 `agent/streamIncludeUsage=true` 时发送（默认 false，避免部分网关因未知字段 400）。
- 请求头追加 `Accept: text/event-stream`。
- **超时策略变更**：流式请求 `setTransferTimeout(0)`（关闭 Qt 的整段传输超时），改由 §3.6 的空闲看门狗 + 总时长上限控制；非流式保持 `30000`（`OpenAILLMClient.cpp:297` 不变）。

### 3.2 SSE 解析：`SseStreamParser`（新类，纯逻辑、无网络）

```cpp
// src/core/agent/SseStreamParser.h
namespace DeepLux {
class SseStreamParser {
public:
    struct Event {
        QString name;        // "event:" 字段，缺省空 → 视为 "message"
        QByteArray data;     // 多行 data 以 '\n' 连接后的原始负载
        bool done = false;   // data == "[DONE]"
    };

    // 喂入任意字节片（可以是半个字段、半个 UTF-8 字符、多个事件）
    QList<Event> feed(const QByteArray& bytes);
    void reset();
    int droppedLines() const { return m_droppedLines; }  // 超长行/非法行计数
    int eventCount() const { return m_eventCount; }

private:
    QByteArray m_buffer;     // 未完结的行（半包）
    QByteArray m_data;       // 当前事件已累积的 data 行
    QString m_eventName;
    bool m_doneEmitted = false;
    int m_droppedLines = 0;
    int m_eventCount = 0;
};
} // namespace DeepLux
```

**逐条规则（验收项）**

| # | 规则 |
|---|---|
| P1 | 以 `'\n'` 切行；行尾单个 `'\r'` 必须剥离（兼容 CRLF）；**缓冲中不完整的最后一行必须留在 `m_buffer`**，等下一次 `feed` 拼上（半包）。 |
| P2 | 空行 = 事件边界：若已累积 `data` 行或 `event` 名 → 派发一个 `Event`（`data` 多行用 `'\n'` 连接），然后清空当前事件状态；无累积则忽略（连续空行/保活）。 |
| P3 | 以 `':'` 开头的行是注释（含 `: ping` 心跳）→ **丢弃但算作活动**（外部看门狗据此判断连接健康）。 |
| P4 | 字段解析：`field: value`，冒号后若有**一个**空格则剥离之；无冒号的行按"字段名=整行、值为空"处理（SSE 规范），未知字段忽略。 |
| P5 | `data:` 行追加到当前事件（多行 join `'\n'`）；`event:` 行设置事件名（覆盖前值）；`id:`/`retry:` 忽略。 |
| P6 | `data` 恰好为 `[DONE]` → 派发 `Event{done=true}`；此后 `reset()` 前所有输入丢弃（幂等）。 |
| P7 | 单行长度上限 **1 MiB**：超限的行整行丢弃、`droppedLines++`、继续解析（不中断流）。 |
| P8 | 编码：只在**整行**完成后 `QString::fromUtf8(line)` 解码 ⇒ 不会切裂 UTF-8 多字节字符（半包测试必须覆盖中文）。 |
| P9 | 解析器**不做 JSON 解析、不做业务判断**（把 JSON 错误留给客户端，便于单测与容错分级）。 |
| P10 | `feed` 返回的事件按到达顺序；一次 `feed` 可返回 0..N 个事件（多包）。 |

### 3.3 delta 累加规则

客户端收到事件后：`data` 先 trim；空 → 跳过；`QJsonDocument::fromJson` 失败 →

- 若 `sawDelta == false` → 记录一次 `parseErrors++`；**连续 3 次**解析失败且仍无 delta → 视为 `Malformed`（走 §4.2 回退判定）；
- 若已有 delta → 记录并**忽略该行**（不中断整轮）。

JSON 对象的取值路径（`choices[0]`，`n>1` 不支持：取 index 0 并 `qWarning`）：

| 字段 | 规则 |
|---|---|
| `choices[0].delta.content` | 非空字符串 → 追加到 `content` 累加器并 `emit streamContentDelta(text)`；`sawDelta=true` |
| `choices[0].delta.reasoning_content` | 非空 → 追加到 `reasoning` 并 `emit streamReasoningDelta(text)`；`sawDelta=true`（兼容字段名 `reasoning`、`reasoning_text`：若 `reasoning_content` 缺失则依次读取） |
| `choices[0].delta.tool_calls` | 数组；每个元素按 `index` 累加（见下）；`sawDelta=true`（首个分片到达即算） |
| `choices[0].delta.role` | 忽略 |
| `choices[0].finish_reason` | 非 null → 记录 `m_finishReason`（`stop`/`tool_calls`/`length`/`content_filter`），**不立即结束**（等 `[DONE]` 或连接关闭） |
| `choices` 为空数组 | usage-only 块（部分网关）→ 跳过，不报错 |
| `usage` | 存在则记录 `prompt_tokens`/`completion_tokens`（仅 `streamIncludeUsage` 时通常出现） |
| 顶层 `error` 对象 | 立即失败：`classifyError(0, NoError, errMsg, data)`（分类见 §6.2），不重试若已有 delta |

**工具调用参数分片拼接规则（`tool_calls` 按 index 累加，核心验收项）**

```cpp
struct ToolCallAcc {
    QString id;        // 首个非空 id 生效；后续分片里的空串/重复值不动它
    QString type;      // 首个非空生效，缺省 "function"
    QString name;      // 首个非空 function.name 生效
    QString arguments; // 每个分片的 function.arguments 字符串按到达顺序 **append**
};
QMap<int, ToolCallAcc> m_toolCallAcc;  // index 为键，必须按 index 归组
```

- `index` 缺失：若当前只有一个已见 index → 归入该 index；否则按 `0` 处理并 `qWarning`。
- `function.arguments` 若为**对象**（个别网关一次性给完整对象）→ `QJsonDocument(obj).toJson(Compact)` 后 append（与第一迭代 `parseResponse` 的对象压缩口径一致，`OpenAILLMClient.cpp:433-443`）。
- 分片顺序由 TCP 保证，**禁止**按 index 重排拼接（同一 index 内的 append 顺序即协议顺序）。
- 最终组装：按 index **升序**输出为 `[{id,type,function:{name,arguments}}]`：
  - `id` 为空 → 合成 `call_<index>`；若与已用 id 冲突 → 追加 `_<n>` 保证唯一；
  - `name` 为空 → **丢弃该 tool call** 并计 `droppedToolCalls++`（无法派发）；
  - `arguments` 原样保留字符串；若为非法 JSON，**不在客户端报错**——由 `AgentActor` 的参数校验给出结构化工具错误（第一迭代行为，`AgentController.cpp` 的 `toolCallParams` 对非法字符串退化为空参数）。
- 工具调用分片只在**结束时**对外可见（`AgentResponse.toolCalls`）；流中只允许发 `streamToolCallsUpdated(int count)` 供 UI 显示"正在准备工具调用…"。

### 3.4 结束与终态判定（优先级从上到下）

| # | 条件 | 结果 |
|---|---|---|
| 1 | 收到 `[DONE]` | 聚合成功 → `finalizeStream()` |
| 2 | 连接 `finished` 且 `m_finishReason` 非空 | 聚合成功（网关省略 `[DONE]` 的容错） |
| 3 | 连接 `finished` 且已收到**任意**有效事件（eventCount>0） | 聚合成功（最后兜底，记 `qWarning("stream ended without [DONE]")`） |
| 4 | 连接 `finished` 且 `eventCount==0` 且 `droppedLines==0` | 空流：`Malformed`（若尚未回退过 → 走 §4.2 回退非流式） |
| 5 | 看门狗/总时长触发 | `Timeout`（§3.6） |
| 6 | 主动 abort | 不发终态信号，由控制器收口（§6.1） |

`finalizeStream()` 组装 `AgentResponse{success=true, content, reasoningContent, toolCalls, promptTokens, completionTokens, errorCategory=None}` 并 `emit responseReceived(resp)`；**必须**先置 `m_streamFinalized=true` 做幂等（`readyRead` 与 `finished` 可能双触发）。

`finish_reason == "length"`：仍算成功（内容被模型截断），但在最终响应里置 `resp.truncatedByMaxTokens = true`（新增字段）→ 控制器记审计 `result="truncated"`，UI 状态条提示"输出达到 max_tokens 上限"。
`finish_reason == "content_filter"`：`success=false`，`LLMErrorInfo{category=BadRequest, retryable=false, message="The response was blocked by the content filter."}`（不重试）。

### 3.5 异常事件容错（明确不中断流的情形）

| 情形 | 行为 |
|---|---|
| 注释行/空行/`event:` 行 | 忽略（P3/P5） |
| 单行超 1 MiB | 丢行 + `droppedLines++`，继续 |
| JSON 非法（已有 delta） | 跳过该事件 + `qWarning`，继续 |
| `choices` 为空（usage 块） | 跳过 |
| 未知 delta 字段 | 忽略 |
| `tool_calls` 元素缺 `index` | 按 §3.3 兜底归组 |
| `data:` 里混入 `\r`（CRLF 在 data 内） | 行级剥离，不进入 JSON |

### 3.6 超时与保活

| 参数 | 键 | 默认 | clamp | 语义 |
|---|---|---|---|---|
| 流空闲看门狗 | `agent/streamIdleTimeoutMs` | 60000 | [5000, 600000] | 距**上一字节/上一事件**超过该值 → 判 `Timeout` |
| 单次流总时长 | `agent/streamMaxDurationMs` | 600000 | [60000, 3600000] | 从请求发出算起；超时判 `Timeout` 并 abort reply |

- 两个计时器都是 `QTimer` 成员（`m_streamIdleTimer` / `m_streamDeadlineTimer`），`reset`/`stop` 于 `sendRequest`/`finalize`/`abort`。
- 注释心跳（P3）刷新空闲计时器。
- 触发超时后：`abortRequest()` 内部路径复用（断开 reply），若 `!sawDelta` 允许按 §6.2 重试一次（Timeout 类别在基线允许 1 次重试）。

### 3.7 reasoning 回传策略（`agent/reasoningEcho`）

`reasoningContent` 的**唯一用途**：UI 折叠展示 + 历史裁剪的 token 估算。是否回传请求由配置决定：

| 取值 | 语义 | 默认 |
|---|---|---|
| `none` | **不回传**：`toOpenAIMessages` 不输出 `reasoning_content` | **默认** |
| `current-turn` | 只回传"最后一个 user 消息之后"的 assistant 的 reasoning（同一工具循环内） | —— |
| `full` | 全部 assistant 的历史 reasoning 都回传（**第一迭代基线行为**） | —— |

实现：`AgentConversation::toOpenAIMessages(ReasoningEcho echo)` 重载（默认参数 `None`）；`OpenAILLMClient` 按自身策略调用。**不得**把 reasoning 拼进 `content` 或 system prompt（这是"不回流"的第一层保证）。

> **迁移提示**：若某服务商在思考模式下要求回传 `reasoning_content`（表现为 400 且 body 提到 reasoning），把 `agent/reasoningEcho` 设为 `current-turn` 或 `full`；错误分类会给出 `contextLimitSuspected/visionDowngradeHint` 之外的通用 `BadRequest` 提示，UI 文案附"若使用思考模型，请检查 reasoningEcho 配置"（§12 文案表）。

### 3.8 状态机（客户端）

```text
Idle ──sendRequest(stream=true)──> Requesting(stream)
Requesting(stream) ──首个有效事件──> Streaming
Requesting(stream) ──400(stream 相关)/JSON 响应/首 delta 前解析失败──> Fallback ──> Requesting(non-stream)
Streaming ──[DONE]/finish_reason/连接关闭──> Finalized ──emit responseReceived──> Idle
Streaming ──错误/超时/abort──> Failed/Aborted ──emit(分类或静默)──> Idle
Requesting(non-stream) ──完成──> parseResponse（第一迭代路径不变）
```

---

## 4. 非流式回退（t12）

### 4.1 配置开关

- `agent/streamEnabled`：bool，**默认 true**。`ILLMClient` 增 `virtual void setStreamEnabled(bool)`（非纯虚，默认实现存成员；既有 Mock/Scripted 无需实现）+ `virtual bool isStreamEnabled() const`。
- **设置界面（t13，与流式同批）**：`AgentSettingsDialog` 新增两个控件，并在 `loadSettings/saveSettings` 中读写：
  - 复选框「启用流式响应（实时显示生成过程）」→ `agent/streamEnabled`；
  - 数字框「上下文预算（tokens）」（范围 16000–1000000、步进 8000）→ `agent/contextBudgetTokens`；
  - `MainWindow::loadAgentSettings()`（`MainWindow.cpp:6140-6188`）与设置应用路径（`:838-840` 相邻）同步 `client->setStreamEnabled(...)`；预算不需要注入客户端（控制器自读配置）。
- `OpenAILLMClient::buildRequestBody` 仅在 `streamEnabled && m_streamFallbackUsed==false` 时加 `"stream": true` 与（可选）`stream_options`。
- 默认关闭场景：用户在网络/网关不稳定时手动关闭 → 完全走第一迭代非流式路径（**零行为变化**）。

### 4.2 回退触发条件（按优先级）

| # | 触发 | 回退方式 | 说明 |
|---|---|---|---|
| F1 | `streamEnabled == false` | 不发送 `stream` | 配置级，非"回退" |
| F2 | HTTP 200 且 `Content-Type` 为 `application/json*`（网关忽略 stream） | **不重发**：读取完整 body → `parseResponse(body)` | 省一次请求；发 `streamFallback("server returned non-stream body")` |
| F3 | HTTP 400/422 且 body 命中 `/stream|event-stream|not support.*stream|streaming.*not.*support/i`，且 `sawDelta==false` | **重发一次**非流式（去掉 `stream`/`stream_options`） | 计 `m_streamFallbackUsed=true` |
| F4 | 首个有效事件前连续 3 次 JSON 解析失败（§3.3），或 SSE 首事件前连接被关闭且 `eventCount==0` | **重发一次**非流式 | 同上 |
| F5 | 服务端 404/405 对 SSE（少见） | 同 F3 | 归入同一回退提示（附录 B） |

**禁止回退**：`sawDelta == true` 之后（内容已展示给用户，重发会造成重复/错位）。此时按 §6.2 收口。

### 4.3 回退实现约束

- 回退**只允许一次/请求**（`m_streamFallbackUsed`，在 `sendRequest` 重置）；回退重发**不消耗**重试次数（重试计数只在真正失败时累加）。
- 回退重发沿用 `m_lastBody`（同一对话、同一工具列表），只删 `stream`/`stream_options` 两个键，并 `setTransferTimeout(30000)` 回到非流式。
- **Content-Type 分流（F2 的实现细节）**：首次 `readyRead` 时读取 `QNetworkReply::header(QNetworkRequest::ContentTypeHeader)`：
  - 以 `text/event-stream` 开头 → 交给 `SseStreamParser`；
  - 以 `application/json` 开头（或为空但 body 以 `{` 开头）→ **不喂解析器**，把字节累积到 `m_rawBody`，`finished` 时整包 `parseResponse(m_rawBody)`；
  - 头尚未就绪（`metaDataChanged` 前）→ 先缓存字节，拿到头后再分流（**不得**把 JSON 字节喂给 SSE 解析器）。
- 回退必须重置流累加器与解析器（避免半截内容混入），并 `emit streamFallback(reason)`。
- 审计：`AgentController` 收到回退后写一条 `AgentActionLogEntry{action="LLM stream fallback", params=reason, result="success"}`（不弹错误）。

### 4.4 用户可见提示

- 新增信号 `AgentController::agentStreamFallback(const QString& reason)`；`MainWindow` 在状态条显示"流式不可用，已回退整包响应"，并在对话面板追加一条 **System 气泡**：`"流式不可用（<reason>），已回退为整包响应。"`
- **例外**：F1（用户自己把 `streamEnabled` 设为 false）**不**发回退信号、不显示提示——那不是降级，是用户选择。
- **禁止**用 `llmErrorOccurred` 报回退（它是正常降级，不是错误）；日志用 `Logger::instance().info(..., "Agent")`。
- 非流式回退后，`llmResponseReceived` 路径与第一迭代完全一致（`MainWindow.cpp:1716-1723` 零改动即可继续工作）。

---

## 5. 增量渲染契约（t12 主责）

### 5.1 信号面（只增不改）

`ILLMClient` 新增信号：

```cpp
signals:
    void streamStarted();                              // 收到首个有效 SSE 事件（含仅 role 的首块）；UI 据此建流式气泡
    void streamContentDelta(const QString& text);
    void streamReasoningDelta(const QString& text);
    void streamToolCallsUpdated(int count);            // 仅计数，不含参数（工具名/参数在终态才可见）
    void streamFinished();                             // 聚合成功且 responseReceived 即将发出（先此信号，后 responseReceived）
    void streamFailed();                               // 已收 delta 后失败（随后仍发 errorOccurred/Classified）
    void streamFallback(const QString& reason);        // 已切非流式
```

`AgentController` 转发（UI 只与控制器交互，禁止直连客户端）：

```cpp
signals:
    void agentStreamStarted();
    void agentStreamContentDelta(const QString& text);   // 已按 50ms 合批
    void agentStreamReasoningDelta(const QString& text); // 已按 50ms 合批
    void agentStreamFinished();
    void agentStreamAborted();
    void agentStreamFallback(const QString& reason);
```

**顺序约定**：`streamStarted`（首个事件）→ N×(delta) → `streamFinished` → `responseReceived`。控制器在 `onLLMResponse` 之前先把残留缓冲 flush 掉，保证 UI 收到的文本 = 历史里的文本。

### 5.2 约 50ms 合批刷新

- 控制器成员：`QTimer* m_streamFlushTimer`（`Qt::CoarseTimer`，`setSingleShot(true)`，间隔 `kStreamFlushIntervalMs = 50`，定义在 `AgentController.cpp` 匿名命名空间）、`QString m_pendingContent`、`QString m_pendingReasoning`。**禁止**在 UI 层再加一层合批定时器（只允许一处合批）。
- delta 到达：追加到 pending；若 timer 未激活 → `start()`。
- timer 超时：`emit agentStreamContentDelta(m_pendingContent)` / `...ReasoningDelta(...)` 并清空。
- `streamFinished` / `agentStreamAborted` / 错误收口：**先** flush（停 timer、若有 pending 立即发）→ 再发终态信号。
- 约束：**不得**每个 delta 都触发 `QTextBrowser::setHtml` 全量重排；面板侧只做 `appendText`（增量追加，`AgentMessageBubble::appendText` 已存在，`AgentMessageBubble.h:66`）。
- 测试不依赖精确 50ms：断言"1 秒内 1000 个 delta 的 UI 刷新次数 ≤ 30"，或直接断言控制器的 `agentStreamContentDelta` 次数远小于 delta 数（§9.1 第 10 条）。

### 5.3 reasoning 折叠块

`AgentMessageBubble` 新增（**不得**改既有方法签名）：

```cpp
void appendReasoning(const QString& text);      // 流式追加
void setReasoning(const QString& text);         // 一次性设置（非流式/历史回放）
void setReasoningStreaming(bool streaming);     // 标题在「思考中…」与「已思考（N 字）」间切换
QString reasoningText() const;                  // 测试断言
```

UI 规则：

1. reasoning 块位于气泡正文**上方**，默认**折叠**；标题行可点击展开/收起（`QToolButton` + `setCheckable`，箭头图标复用 `AppIconProvider`）。
2. 流式期间标题显示 `思考中…`；结束显示 `已思考（<字符数> 字）`；为空则整块隐藏。
3. reasoning 文本**只读展示**：不进入 `content`、复制按钮只复制正文、不参与 Markdown 渲染的高亮（可用等宽字体 + 弱化配色）。
4. 非流式响应若 `AgentResponse.reasoningContent` 非空，同样渲染该块（`addMessage` 后调用 `setReasoning`）——保证两条链路 UI 一致。
5. 主题：颜色取自 `ChatTheme`（复用 `timestampColor/statusColor`，不新增主题字段）。

### 5.4 终态映射（必须逐条实现）

| 终态 | 控制器/客户端事件 | 面板动作 | 历史 |
|---|---|---|---|
| 成功（纯文本） | `streamFinished` → `responseReceived` → `llmResponseReceived(content, {})` | `finalizeStreamedMessage(content)`：正文与最终文本对齐、移除光标、reasoning 结束态 | 追加 assistant（`AgentController.cpp:454-460`） |
| 成功（含 tool_calls，content 可能为空） | 同上（`toolCalls` 非空） | 若无正文则**移除**流式气泡（不产生空气泡）；若有正文则 finalize | 追加 assistant（含 toolCalls） |
| 失败（首 delta 后） | `streamFailed` → `errorOccurred` | `failStreamedMessage("已中断")`：保留已收文本 + 追加中断标记；`setThinking(false)`；System 气泡显示错误 | **不提交**（无 assistant 消息） |
| 中止 | `agentTurnAborted`（控制器已 flush） | `abortStreamedMessage("已停止")`：保留已收文本 + 标记 | **不提交** |
| 回退 | `streamFallback` | `abortStreamedMessage("已回退")` 移除空气泡 + System 提示 | 由后续非流式响应正常提交 |
| `finish_reason=length` | `responseReceived{truncatedByMaxTokens=true}` | 正文保留 + 状态条提示"达到 max_tokens 上限" | 追加 assistant（正常） |

### 5.5 与既有 `AgentChatPanel` 接口的兼容约束

**必须保留**（现有调用点不动即可编译）：`addMessage(Sender, text, images)`（`AgentChatPanel.h:32`）、`appendToLastMessage(text)`（`.h:33`）、`setThinking/setExecuting/setStopped/setError`、全部既有信号。

**新增**（t13 实现）：

```cpp
void beginAgentStream();                                  // 创建空 Agent 气泡，标记 streaming
bool isStreamActive() const;
void appendStreamContent(const QString& text);            // 追加正文（每 50ms 一批）
void appendStreamReasoning(const QString& text);          // 追加 reasoning
void finalizeStreamedMessage(const QString& finalContent);// 用最终文本对齐（处理"最终文本比流式累积多/少"）
void failStreamedMessage(const QString& note);            // 失败/回退：移除空气泡或追加标记
void abortStreamedMessage(const QString& note);           // 中止：保留 + 标记
```

- `addMessage` 在流式进行中**禁止**被调用（会绕过流式气泡）；由 `MainWindow` 的 `llmResponseReceived` 分支保证：`if (panel->isStreamActive()) panel->finalizeStreamedMessage(content); else panel->addMessage(...)`。
- `appendToLastMessage` 保留原语义（追加到最后一条 Agent 气泡），流式期间内部转向 `appendStreamContent`（避免两条追加路径打架）。
- 空文本保护：`beginAgentStream` 创建的气泡若最终无正文且无图片 → 必须销毁（不增加 `m_messageCount`）。

### 5.6 双终态去重

- 客户端 `m_streamFinalized` 保证 `responseReceived` 只发一次。
- 控制器 `m_streamActive`（bool）在 `agentStreamStarted` 置真、`streamFinished/streamFailed/agentTurnAborted` 置假；重复的 `streamFinished` 直接忽略。
- 回退后 `m_streamActive=false`，后续非流式响应走 `addMessage` 路径。

---

## 6. 取消与重试边界（t12 主责）

### 6.1 流中 `abortCurrentTurn()`

现有骨架（`AgentController.cpp:232-254`）已满足 80%，本迭代新增/明确：

| 步骤 | 行为 | 新增点 |
|---|---|---|
| 1 | `state()==Idle` → 直接返回（幂等） | 保持 |
| 2 | `++m_turnGeneration`；清 pending；`m_dispatchQueued=false`；`m_suppressNextLlmError=true` | `m_suppressNextLlmError` 在流式下同样生效（客户端 abort 后不再分类） |
| 3 | **flush 流式缓冲**（50ms timer 立即触发，残留文本先发给 UI） | **新增** |
| 4 | `m_llmClient->abortRequest()`：client 置 `m_aborted`、断开 reply、停看门狗/重试、重置解析器与累加器，**不发终态信号** | 保持 + 扩展 |
| 5 | `m_actor->abortRunningTool()`（等待类工具） | 保持 |
| 6 | `transitionTo(Idle)`；`emit llmResponseReceived("Stopped by user.", {})`；`emit agentTurnAborted()` | 保持 |
| 7 | **`emit agentStreamAborted()`（在 agentTurnAborted 之前）** | **新增** |

**语义保证（验收项）**：

1. **保留已收内容**：已 flush 的正文/reasoning 留在 UI 气泡上，追加"已停止"标记；**不**从 UI 回滚。
2. **状态回 Idle**：`state()==Idle`，停止按钮隐藏，输入框恢复可用。
3. **历史不提交**：半截 assistant 消息**不**写入 `m_conversationHistory`（与第一迭代"工具批被中止后仍保留 tool 消息但不再续跑"不同：那里是完整批次，这里是半截生成）。因此下一轮模型看不到被中止的半截回答——这是刻意的（避免把截断文本当成已完成回答）。
4. **不误报错误**：`m_suppressNextLlmError` + 客户端 `m_aborted` 双保险，UI 不得出现 "Network error/Timeout" 之类假错误。
5. **工具等待期不涉及流**：`run_flow(wait)` 的嵌套事件循环期间无流式请求（状态 `Executing`），中止走 `abortRunningTool()`（第一迭代行为）。

### 6.2 可重试阶段与不可重试阶段

| 阶段 | RateLimit / Server / Network / Timeout | Auth / BadRequest / Malformed |
|---|---|---|
| 非流式（含回退后） | 按第一迭代矩阵重试（`OpenAILLMClient.cpp:196-239`） | 不重试 |
| 流式，**首个 delta 之前**（`sawDelta==false`） | 按同一矩阵重试；重试仍以 `stream=true` 发送（不因失败改模式，回退单独判断） | 不重试；若属于 §4.2 的"stream 不支持"则回退一次 |
| 流式，**已有任意 delta**（`sawDelta==true`） | **不重试**（已展示内容，重试会产生重复/错位） | 不重试、不回退 |

- 重试与回退的互斥：一次请求内 `m_streamFallbackUsed` 与 `m_retryAttempt` 独立计数；总请求数上限 = `1 + maxRetries(≤3) + fallback(≤1)`，且**回退不在重试之后发生**（重试只在同模式内；回退只在首 delta 前、尚未重试成功时）。
- **已收内容后失败不重试**（no retry after partial content）：`sawDelta==true` 后任何失败（HTTP 错误、断连、超时、`error` 对象）都只做一次收口——`streamFailed` + 错误分类上报 + UI 标记"已中断" + 历史不提交；**禁止**重试、**禁止**回退。
- `abortRequest()` 优先级最高：任何阶段中止都不重试、不回退、不分类。

### 6.3 状态机与既有状态

- 流式期间控制器仍处于 `Thinking`（`sendUserMessage` 进 `Thinking`，`onLLMResponse` 才离开）；因此流中 `sendUserMessage` 被 busy 拒绝、`handleToolCall` 返回 busy（第一迭代语义，`AgentController.cpp:353-356` 与 `:862-893`）——**不新增状态**。
- 流成功且含 tool_calls → 走既有 `Confirming`/`Executing` 分支（`onLLMResponse` 内，`:466-493`），流式不改变权限/确认语义。
- 流式与 `agentStateChanged`：`Thinking` 期间的 UI 由 `setThinking(true)` 与流式气泡共同表达；不新增 `Streaming` 状态（减少状态机回归面）。

---

## 7. 上下文治理（t13 主责）

### 7.1 预算配置

| 键 | 类型 | 默认 | clamp | 读取位置（唯一） |
|---|---|---|---|---|
| `agent/contextBudgetTokens` | int | **128000** | [16000, 1000000] | `AgentController::contextBudgetTokens()`（新增，内部 `ConfigManager::groupInt` + `qBound`） |
| `agent/toolResultMaxChars` | int | **6000** | [1000, 64000] | 同一处（供测试下调） |
| `agent/flowSummaryThreshold` | int | **24** | [1, 1000] | `AgentController::flowSummaryThreshold()` |
| `agent/flowSummaryPreviewCount` | int | **24** | [1, 200] | 同上 |

- 提供测试可见 setter：`DEEPLUX_TEST_VISIBILITY void setContextLimitsForTest(int budgetTokens, int toolResultMaxChars, int summaryThreshold, int summaryPreviewCount)`（任一参数 `-1` = 沿用配置），避免测试依赖 `ConfigManager` 初始化时序。
- 估算公式保持第一迭代（`AgentController.cpp:805-815`）：每字符 ≈0.3 token、每消息 +20、图片 `(bytes+2)/3`；**新增**：reasoning 只在 `reasoningEcho != none` 时计入（`none` 下不发送 ⇒ 不占预算）。
- 预算与硬消息上限（64）同时生效，取二者更严者。

### 7.2 消息单元定义

- **user turn**：一条**真实用户请求**（`role=user` 且未被标记为自动回灌）到**下一条真实用户请求**之前的全部消息。
- **自动回灌消息**：工具产图后由控制器追加的 `role=user` 消息（含图片或文本降级），通过 `AgentMessage` 的内部来源标记识别；它属于**原用户任务**，不构成新的用户任务边界，也不得出现在对外模型协议中（序列化仍只发 role/content/images）。
- **tool group**：一条 `role=assistant` 且 `toolCalls` 非空的消息，加上其后**紧邻的、toolCallId ∈ 该 assistant 的 tool_call ids** 的全部 `role=tool` 消息。
- 其它（无 toolCalls 的 assistant、孤立 user）各自成单元。

### 7.3 工具组原子裁剪（必须逐条实现）

**不变量**

| 编号 | 不变量 |
|---|---|
| I1 | 历史中**不存在**孤立 `tool` 消息：每条 `tool.toolCallId` 必须能在**前一条** assistant 的 `toolCalls[].id` 中找到 |
| I2 | `assistant(tool_calls)` 的每个 id 必须有对应 `tool` 消息；缺失即视为不完整组 → **整组移除**（防御性修复，防上一轮中止留下的半组） |
| I3 | 裁剪只能以"单元"为粒度删除；**禁止**单删 `tool` 或单删带 toolCalls 的 assistant |
| I4 | **保护集合**（集成收口 B3 修订，替代"最新 user turn 永不裁剪"）：当前真实用户请求、在途 assistant/tool 调用组、最近一个完整工具结果组一律不得删除；自动回灌 user 消息并入其所属任务，不单独形成保护边界。 |
| I5 | 裁剪结束后 `history.first().role == "user"`（system 由 `systemPrompt` 单独承载，不在列表里） |
| I6 | 发送前必须校验：每个 assistant(tool_calls) 的 id 与 tool 结果**恰好一一配对**，且当前保护集合可完整发送；不满足则不发请求。 |
| I7 | 保护集合自身超过 token/消息数/图片预算 → 本轮以 `context_budget_exceeded` 确定性结束，不裁剪保护集合、不发送超限请求。 |

**算法（替换 `AgentController.cpp:800-855`）**

```text
trimHistoryIfNeeded():
    budget = contextBudgetTokens()                     # clamp；toolResultMaxChars 只在 §7.4 的写入点用，不参与裁剪循环
    repairOrphanToolGroups()                           # I1/I2：删除不完整组（含孤立 tool）
    while (!history.isEmpty() && history.first().role != "user"): removeFirst()   # I5
    protected = computeProtectedUnits()                # I4：当前真实请求 + 在途组 + 最近完整工具组
    while ((estimateTokens() > budget || history.size() > kMaxHistoryMessages) && protectedOverBudget() == false):
        if (dropOldestUserTurnOutsideProtected()): continue   # 只删完整旧任务
        if (dropOldestToolGroupOutsideProtected()): continue  # 只删当前任务内更早的完整组
        break                                          # 无可删单元：保护集合超限由发送前检查收口
    while (!history.isEmpty() && history.first().role != "user"): removeFirst()   # 最终保证
```

- `dropOldestUserTurnOutsideProtected()`：找保护集合之前的**真实用户请求**边界，整段删除；自动回灌 user 消息不作为边界。没有可删旧任务 → false。
- `dropOldestToolGroupOutsideProtected()`：从 `index=1` 起找第一个不在保护集合内的完整 tool group 删除；不得拆组、不得删除在途组或最近完整组。
- `repairOrphanToolGroups()`：单趟扫描，遇到 `tool` 且前一条不是匹配的 assistant（或 id 不在其 toolCalls）→ 删除该 tool；遇到 assistant(toolCalls) 但后续缺 id → 整组删除。只在裁剪入口执行一次；**在途** assistant/tool 组不得当作历史损坏删除。
- `sendRequest` 前的唯一检查入口：先 `repairOrphanToolGroups()` + 配对校验（I6），再计算保护集合预算；超限发 `context_budget_exceeded`（I7），记录本地可诊断日志并结束本轮。
- 复杂度 O(n²) 可接受（n ≤ 数百）；**禁止**为了性能改变单元语义。
- 日志：每次裁剪 `Logger::instance().addLog("history trimmed: -N msgs (-M tokens), budget=…", Debug, "Agent")`（便于现场核账）。

### 7.4 单条 tool 结果 6k 截断

**唯一实现点**：tool 消息写入历史的时刻（`AgentController.cpp:635-640`）。

```cpp
// 保留首尾，中间用标记说明省略量（字符数，QChar 计）
QString truncateToolResult(const QString& json, int maxChars /* default 6000, clamp 1000..64000 */) {
    if (json.size() <= maxChars) return json;
    const int head = maxChars * 2 / 3;      // 6000 → 4000
    const int tail = maxChars - head;       // 2000
    return json.left(head)
         + QStringLiteral("\n…[truncated %1 chars]…\n").arg(json.size() - maxChars)
         + json.right(tail);
}
```

规则：

1. 只作用于 `role == "tool"` 的 `content`；user/assistant/system 不截断。
2. 截断**不影响**审计（`AgentActionLogEntry` 在截断前生成，使用完整结果）与 UI 的"工具预览"（预览只显示参数，不显示结果）。
3. 标记格式固定：`\n…[truncated <N> chars]…\n`（英文、单行、可被模型理解；N = 被省略的字符数）。
4. 对 JSON 结果会破坏 JSON 合法性——**这是有意的**：模型按文本阅读并知道被截断；若模型需要完整数据，应改用 `get_execution_logs`/更窄的查询（提示词见 §8.4）。
5. 与 `imagePath` 无冲突：图片回灌在 `attachToolImages` 里处理（`AgentController.cpp:677-788`），不依赖 tool 消息全文。

### 7.5 首条非 system 必须为 user

- `AgentConversation.toOpenAIMessages()` 把 `systemPrompt` 放在 `messages` 之前（`ILLMClient.cpp:8-13`），因此消息列表本身**没有 system 角色**；"首条非 system 为 user" 等价于 `m_conversationHistory.first().role == "user"`。
- 三个裁剪入口（`sendUserMessage` 后、`onLLMResponse` 后、`extendAgentLoop` 后，`AgentController.cpp:361`/`:460`/`:647`）都必须满足；算法最后一步强制保证（I5）。
- 所有 `sendRequest` 调用点（用户消息、工具续跑、流式回退重发）之前必须经过同一个上下文发送检查：配对合法（I6）且保护集合在预算内；否则不发请求并以 `context_budget_exceeded` 结束本轮。
- 新增测试断言直接检查：`history.isEmpty() || history.first().role == "user"`。

### 7.6 硬消息上限

- 保留 `kMaxHistoryMessages = 64`（原 `kMaxMessages`，`AgentController.cpp:842`），**值不变**（既有测试 `:510`/`:534` 依赖 `<=64`）。
- 优先级：先按 token 预算裁剪，再按消息数裁剪；两者都走同一单元算法（令牌计数与条数计数共享 `dropOldest*`）。
- `testTrimHistoryEnforcesHardMessageLimit`（80 条 → ≤64、末条 assistant-39）与 `testTrimHistoryEnforcesHardLimitWithinSingleUserTurn`（单 turn 内 71 条 → ≤64、`[1]` 为 assistant）在算法下均成立（逐对/逐组删除，从头部开始）。

### 7.7 图片字节预算

- 沿用 `(image.data.size()+2)/3` 计入（`AgentController.cpp:811-812`）；单条 tool 结果的 6k 截断**不**作用于图片附件（图片在 user 消息上）。
- 回灌上限（2 张/轮、长边 1280、单张 4MB、单轮 8MB）不变（第一迭代 §6.3）。

### 7.8 与既有测试的兼容（必读）

| 既有用例 | 新算法下的结果 | 处理 |
|---|---|---|
| `testTrimHistoryPreservesMessagePairs`（`:427-493`） | 7 轮 × ~100k 字符 ≈ 210k token > 128k → 丢弃若干最老 user turn；pair 关系保持；首条 user | **保持绿**（断言不变） |
| `testTrimHistoryEnforcesHardMessageLimit`（`:495-513`） | 80 条 ≤64 → 逐对删除至 64，末条 assistant-39 | **保持绿** |
| `testTrimHistoryEnforcesHardLimitWithinSingleUserTurn`（`:515-538`） | 单 turn 71 条 → 逐组删 4 组至 63，`[1]` assistant | **保持绿**（B3 只额外保护最后 1 组；删的是更早的完整组） |
| `testTrimHistoryAccountsForImageBytes`（`:540-560`） | 旧轮整体丢弃，仅剩 latest | **保持绿**（latest 是当前真实请求） |
| 6k 截断 | 既有 tool 消息都远小于 6k → 内容不变 | **保持绿** |

> 若实现后发现某条既有断言与新不变量冲突，**不得**改既有断言来迁就实现；必须先报 architect 更新本文（这是契约冻结的意义）。

### 7.9 预算的额外提示（可选但建议）

- 当单次裁剪丢弃 > 50% 历史时，审计写一条 `result="trimmed"` 的条目，UI 状态条提示"上下文超预算，已裁剪较早对话（可 /clear 重置）"。
- **不**自动摘要历史（第三迭代候选），避免引入额外 LLM 调用与不确定性。

---

## 8. `buildContext()` 摘要化（t13）

### 8.1 规则

```text
if (!project): "- No project opened."（保持）
else if (modules.size() <= flowSummaryThreshold):
    全量列出：每个 "moduleId(id=instanceId)"——**小工程保持全量**（与第一迭代行为逐字一致）
else:
    "- Modules (N): <前 K 个>, … (+M more; call get_flow_state for the full list)"
    其中 N=总数，K=flowSummaryPreviewCount（默认 24），M=N-K
```

- 连接数与 RunEngine 状态行保持（`AgentController.cpp:327-329`）。
- 最近事件：`m_observer->recentEvents(5)`（`:333`）**保持 ≤5 条**，格式不变。
- 硬不变量：`buildContext()` 返回的字符串长度 ≤ 16384 字符（防超大工程把 system prompt 撑爆）：若超过，先把模块行替换为摘要（即使模块数未过阈值），再超出则截断事件段并追加 `…[context truncated]`。

### 8.2 配置与默认

| 键 | 默认 | clamp |
|---|---|---|
| `agent/flowSummaryThreshold` | 24 | [1, 1000] |
| `agent/flowSummaryPreviewCount` | 24 | [1, 200] |

### 8.3 提示词配合

`defaultSystemPrompt()`（`AgentController.cpp:943-973`）追加一条（第二迭代，t13 顺带）：

```text
If the project summary says "(+M more)", call get_flow_state before assuming a module does not exist.
```

### 8.4 测试断言

- 构造 3 个模块的工程 → `buildContext()` 含全部 3 个 `moduleId`，不含 "(+", 不含 "get_flow_state for the full list"。
- 构造 25 个模块（阈值默认 24；或把阈值调小到 2 便于测试）→ 含 `"(25)"`、含前 24 个、含 `"(+1 more; call get_flow_state for the full list)"`、不含第 25 个模块名。
- 事件 6 条 → 输出中 `[hh:mm:ss]` 行数 ≤5。
- 超长工程（200 模块、阈值 1000）→ 输出长度 ≤16384 且含 `…[context truncated]`。

---

## 9. 测试清单

### 9.1 新增 `tests/test_agentstream.cpp`（t12，客户端级：真实 HTTP stub）

> 面板/控制器级的流式用例（合批、中止、折叠块、空气泡）放 t13 的 `tests/test_agentstreamui.cpp`（§9.1.1），用信号注入式 Mock 客户端，避免两个任务改同一个测试文件。

**stub**：复用 `test_agentretry.cpp:22-110` 的 `LlmHttpStub` 结构，扩展 `SseHttpStub`：

```cpp
struct SseScript {
    QByteArray headers;              // 例如 "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n"
    QList<QByteArray> chunks;        // 按序 write+flush（模拟半包）
    int chunkDelayMs = 5;            // 用 QTimer 逐块写，制造真实分包
};
```

断言点（编号即用例名后缀）：

| # | 用例 | 断言 |
|---|---|---|
| 1 | `testSseSingleEvent` | 一个 `data:` 事件（content="Hello"）+ `[DONE]` → `responseReceived.content=="Hello"`，`streamFinished` 先于 `responseReceived` |
| 2 | `testSseHalfPacket` | 整个响应按 **1 字节** 切片发送 → 仍解析出完整 content（覆盖半包/UTF-8 边界） |
| 3 | `testSseMultiEventOnePacket` | 3 个事件 + `[DONE]` 在**同一** chunk → content 拼接正确、事件顺序正确 |
| 4 | `testSseCrlfAndComments` | `\r\n` 行尾 + `: ping` 注释 + 空行 → 正确解析且注释不产生 delta |
| 5 | `testSseMultiLineData` | 单个事件两条 `data:` 行 → JSON 按 `\n` 拼接后解析成功（构造合法 JSON 跨行） |
| 6 | `testSseToolCallFragments` | 两个 tool call（index 0/1），`arguments` 各分 3 片跨包 → 终态 `toolCalls.size()==2`、id/name/arguments 完整且顺序正确 |
| 7 | `testSseToolCallMissingIndex` | 单工具分片缺 `index` → 归入 0，仍产出 1 个 tool call |
| 8 | `testSseDroppedLineAndBadJson` | 一行超长（>1 MiB）+ 一行坏 JSON（已有 delta）→ 流不中断、最终 content 正确 |
| 9 | `testSseFinishReasonWithoutDone` | 有 `finish_reason:"stop"` 但**无** `[DONE]`，随后连接关闭 → 成功终态 |
| 10 | `testStreamCoalescing`（实现见 §9.1.1 U1） | 300 个 1 字符 delta → 控制器 `agentStreamContentDelta` 次数 ≤ 300/3 且最终文本完整（50ms 合批生效） |
| 11 | `testStreamAbortKeepsPartial`（实现见 §9.1.1 U2） | 收到 "Hel" 后 `abortCurrentTurn()` → `state()==Idle`、无 `errorOccurred`、UI 气泡文本含 "Hel" 与"已停止"、历史**无** assistant 消息、`agentStreamAborted` 发出 1 次 |
| 12 | `testStreamRetryBeforeFirstDelta` | 首个事件前 500 → 按矩阵重试；第二次返回正常 SSE → 成功（`retryScheduled` 1 次） |
| 13 | `testNoRetryAfterFirstDelta` | 收到 content 后服务端 500/断连 → 不重试（请求数 1）、`errorOccurred` 1 次、UI 保留已收内容并标"已中断"、历史无 assistant |
| 14 | `testNonStreamFallbackOn400` | 400 + body "stream is not supported" → 自动重发非流式（请求数 2，第二次 body 无 `"stream":true`）→ 成功、`agentStreamFallback` 1 次、UI 有 System 提示 |
| 15 | `testNonStreamFallbackOnJsonContentType` | 200 + `Content-Type: application/json` 整包 → 不重发（请求数 1）、解析成功、回退提示 |
| 16 | `testStreamDisabledByConfig` | `agent/streamEnabled=false` → 请求 body 无 `stream` 键、无流式信号、走第一迭代路径 |
| 17 | `testReasoningNotEchoedByDefault` | 两轮对话带 reasoning_content → 第二轮请求 body 的 assistant 消息**不含** `reasoning_content`；设 `reasoningEcho=full` 后**含** |
| 18 | `testReasoningRenderedCollapsible`（实现见 §9.1.1 U3） | 流式 reasoning delta → 面板气泡 `reasoningText()` 完整、默认折叠、正文 `text()` 不含 reasoning |
| 19 | `testEmptyStreamToolCallBubbleRemoved`（实现见 §9.1.1 U4） | 仅 tool_calls、无 content → 流式气泡被移除（不产生空气泡），`m_messageCount` 不增加 |
| 20 | `testLengthFinishFlag` | `finish_reason:"length"` → `AgentResponse.truncatedByMaxTokens==true`、状态条提示文案出现 |

#### 9.1.1 t12 新增 `tests/test_agentstreamui.cpp`（控制器/面板级，信号注入）

用 `MockStreamingClient : ILLMClient`（仿 `test_agentcontroller.cpp` 的 `MockLLMClient`）直接 `emit streamContentDelta/streamReasoningDelta/streamFinished`，无需 HTTP，专门断言 UI 与合批：

| # | 用例 | 断言 |
|---|---|---|
| U1 | `testCoalescing`（= §9.1 #10） | 连续 300 个 1 字符 delta → `agentStreamContentDelta` 次数 ≤ 100、最终面板文本完整 |
| U2 | `testAbortKeepsPartial`（= §9.1 #11） | 半截内容 + `abortCurrentTurn()` → `Idle`、无错误、面板保留文本并标"已停止"、历史无 assistant |
| U3 | `testReasoningCollapsible`（= §9.1 #18） | `reasoningText()` 完整、默认折叠、正文不含 reasoning |
| U4 | `testEmptyToolCallBubbleRemoved`（= §9.1 #19） | 无正文的流式气泡被移除、`m_messageCount` 不增 |
| U5 | `testFinalizeUsesFinalText` | 流式累积 "Hel" 但 `llmResponseReceived` 给 "Hello" → 面板正文 = "Hello"（以终态为准） |
| U6 | `testNoDuplicateBubbleOnFinish` | 流式成功 + `llmResponseReceived` → 只有 1 个 Agent 气泡（§5.6 去重） |

### 9.2 新增 `tests/test_agentcontext.cpp`（t13）

> 通过 `DEEPLUX_TEST_VISIBILITY` 直接构造 `m_conversationHistory`（与 `test_agentcontroller.cpp:427-560` 同法），并用 §7.1 的测试 setter 固定预算。

| # | 用例 | 断言 |
|---|---|---|
| 1 | `testToolGroupAtomicRemoval` | assistant(tool_calls: a,b) + tool a + tool b 位于被裁单元 → 三者**同时**消失；不得只删其中一条 |
| 2 | `testNoOrphanToolAfterTrim` | 裁剪后遍历：每条 tool 都能在前一条 assistant 的 toolCalls 找到 id（I1） |
| 3 | `testIncompleteGroupRepaired` | 人为插入孤立 tool（无 assistant）与缺 tool 的 assistant 组 → `repairOrphanToolGroups` 清掉两者，其余保留 |
| 4 | `testFirstNonSystemIsUser` | 各裁剪场景下 `history.first().role=="user"`（或历史为空） |
| 5 | `testBudgetClamp` | 配置 1000 → 实际按 16000；配置 10^9 → 按 1000000（通过测试 setter 暴露的 `contextBudgetTokens()` 断言） |
| 6 | `testToolResultTruncatedAt6k` | 8000 字符 tool 结果 → `content.size() < 8000`、含 `…[truncated 2000 chars]…`、首 4000 与末 2000 字符保留 |
| 7 | `testTruncationOnlyForToolRole` | 8000 字符 user/assistant 不截断 |
| 8 | `testTruncationMarkerCount` | 恰好 6000 字符 → 不截断；6001 → 标记 `[truncated 1 chars]` |
| 9 | `testProtectedSetNeverTrimmed` | 极端预算（16000）+ 巨大当前任务 → 当前真实用户请求、在途调用组、最近完整工具组完整保留；旧任务整段丢弃 |
| 9b | `testProtectedSetOverBudgetFailsClosed` | 保护集合自身超预算 → 无模型请求，终态 `context_budget_exceeded`，本地有诊断记录 |
| 9c | `testAutoInjectedImageKeepsTaskBoundary` | 自动回灌图片 user 消息不产生新任务边界；原任务意图仍保留 |
| 10 | `testImageBytesCounted` | 2MB 图片轮在超预算时被整体丢弃（沿用 `:540-560` 的构造） |
| 11 | `testBuildContextSummary` | 25 模块 → 含 `(25)`、前 24 个、`(+1 more; call get_flow_state for the full list)`，不含第 25 个 |
| 12 | `testBuildContextFullForSmallProject` | 3 模块 → 全量、无摘要、无 `get_flow_state` 提示 |
| 13 | `testBuildContextRecentEventsAtMost5` | 注入 6 条 GUI 事件 → 输出事件行 ≤5 |
| 14 | `testBuildContextHardCap` | 200 模块 + 阈值 1000 → 输出 ≤16384 字符且含 `…[context truncated]` |
| 15 | `testExistingTrimTestsStillGreen` | 见 §7.8（可由 verifier 用既有测试覆盖，不重复实现） |

### 9.3 既有测试影响（必读）

| 测试 | 影响 | 处理 |
|---|---|---|
| `tests/test_agentcontroller.cpp:427-560` 四个 trim 用例 | 新算法下应保持绿（§7.8） | **不得改断言**；若红 → 实现 bug |
| `tests/test_agentcontroller.cpp` 其余用例 | 非流式 Mock 不触发流式信号 | 不改 |
| `tests/test_agentretry.cpp`（11 例） | 非流式路径不变 | 不改；可追加 1 例"streamEnabled=true 但服务端 400 → 回退"（放 test_agentstream #14，不重复） |
| `tests/test_agentclosedloop.cpp`（6 例） | 视觉降级重发路径不变 | 不改 |
| `tests/test_agentpermissions.cpp` / `test_agenttoolcontract.cpp` | 无涉及 | 不改 |
| `tests/test_mainwindow.cpp` | 新增信号/面板方法不影响既有断言 | 若编译失败，仅允许最小适配（不得改断言语义） |
| `ui_capture_mainwindow` | 面板默认无流式气泡 → 截图不变 | 不改 |

### 9.4 验收条目 ↔ 测试映射

| 验收条目 | 覆盖用例 |
|---|---|
| SSE 事件解析（`data:`/`[DONE]`/delta 三分支/半包多包/分片拼接/异常容错） | #1~#9 |
| 非流式回退（开关、服务端不支持、解析失败、用户提示） | #14~#16 |
| 增量渲染（50ms 合批、reasoning 折叠、终态、面板接口兼容） | `test_agentstream.cpp` #20 + `test_agentstreamui.cpp` U1~U6、§5.4 映射 |
| 取消与重试边界 | `test_agentstream.cpp` #11~#13（#11 的实现见 U2） |
| 上下文治理（工具组原子、6k、预算、首条 user） | §9.2 #1~#10 |
| buildContext 摘要化 | §9.2 #11~#14 |
| 硬不变量（ABI/依赖/RunEngine/schema） | 静态检查：`git diff --name-only` 不含 `src/plugins/**`、`IModule.h`、`RunEngine.*`、`ToolSchema.*`、`AgentActor.*` |

---

## 10. 文件所有权与并行写冲突

### 10.1 所有权表（本迭代唯一有效版本）

| 任务 | 独占文件 |
|---|---|
| **t12 流式与渲染** | `src/core/agent/ILLMClient.h`（新增流式信号/`setStreamEnabled`/`AgentResponse.truncatedByMaxTokens`/`ReasoningEcho` 枚举与 `toOpenAIMessages` 重载声明）、`src/core/agent/ILLMClient.cpp`（`toOpenAIMessages(ReasoningEcho)` 实现）、`src/core/agent/SseStreamParser.h`、`src/core/agent/SseStreamParser.cpp`（新增）、`src/core/agent/OpenAILLMClient.h`、`src/core/agent/OpenAILLMClient.cpp`、`src/core/agent/AgentController.h`、`src/core/agent/AgentController.cpp`（**仅**流式转发/合批/中止/回退相关段）、`src/ui/widgets/AgentChatPanel.h`、`src/ui/widgets/AgentChatPanel.cpp`、`src/ui/widgets/AgentMessageBubble.h`、`src/ui/widgets/AgentMessageBubble.cpp`、`src/ui/views/MainWindow.h`、`src/ui/views/MainWindow.cpp`（**仅**流式接线/回退提示/`setStreamEnabled` 注入）、`src/ui/dialogs/AgentSettingsDialog.h`、`src/ui/dialogs/AgentSettingsDialog.cpp`（**仅**流式复选框）、`src/core/CMakeLists.txt`（**仅**注册 `SseStreamParser.h/.cpp` 两行，队长授权口径同第一迭代 §8.2 第 8 条）、`tests/test_agentstream.cpp`（客户端级 #1~#9、#12~#17、#20）、`tests/test_agentstreamui.cpp`（新增：§9.1.1 U1~U6）、`tests/CMakeLists.txt`（第一批：两个测试文件） |
| **t13 上下文治理** | `src/core/agent/AgentController.h`、`src/core/agent/AgentController.cpp`（t12 完成**之后**接手：裁剪/6k/预算/`buildContext` 摘要）、`src/ui/dialogs/AgentSettingsDialog.h`、`src/ui/dialogs/AgentSettingsDialog.cpp`（t12 完成后加预算数字框）、`src/ui/views/MainWindow.cpp`（t12 完成后加预算配置读写）、`tests/test_agentcontext.cpp`（新增）、`tests/CMakeLists.txt`（第二批：`test_agentcontext.cpp`） |

**只读（任何实现任务不得修改）**：`src/core/agent/ToolSchema.{h,cpp}`、`src/core/agent/AgentActor.{h,cpp}`、`src/core/agent/AgentRunWaiter.{h,cpp}`、`src/core/agent/AgentToolImage.{h,cpp}`、`src/core/engine/RunEngine.{h,cpp}`、`src/core/interface/**`、`src/plugins/**`、`docs/**`（architect 负责）、根 `CMakeLists.txt`。

### 10.2 串行规则（必须遵守）

1. **总体串行**：按执行计划 t13 **依赖** t12（`docs/agent-execution-plan-v2.md:28`）；t12 报告 `completed` 后 t13 再开工。t13 只做 §7/§8，不得顺手改流式行为。
2. **`ILLMClient.{h,cpp}` 单写者 = t12**：t13 不碰这两个文件；需要额外序列化行为时由 architect 判断是否改本文。
3. **`AgentController.{h,cpp}` 两阶段**：t12 阶段只加流式转发/合批/中止/回退；t13 在 t12 `completed` 后接手，加裁剪/6k/预算/`buildContext`，且开工前必须 `grep -n "m_streamActive\|agentStreamContentDelta" src/core/agent/AgentController.cpp` 确认 t12 已落地。
4. **`AgentSettingsDialog.*` / `MainWindow.cpp` 两阶段**：t12 加流式开关与接线；t13 接手加预算数字框与读写（顺序执行，不做并行同文件）。
5. **`tests/CMakeLists.txt` 串行**：t12 先加 `test_agentstream.cpp` 与 `test_agentstreamui.cpp`；t13 再加 `test_agentcontext.cpp`。
6. **`src/core/CMakeLists.txt` 有限授权**：只有 t12 可改，且只允许在 `CORE_SOURCES`/`CORE_HEADERS` 各加 1 行（`agent/SseStreamParser.cpp` / `.h`）；其余内容禁止改；reviewer 按"diff 只含这两行"审查。
7. **跨任务变更**：需要改对方文件时先报 architect；不得越界。

### 10.3 禁改清单（避免误伤第一迭代成果）

- `ToolSchema.*` / `AgentActor.*`：工具契约、25 个工具、权限/错误码。
- `RunEngine.*`：本迭代无引擎改动。
- `IModule.h` / `src/plugins/**`：ABI 与插件。
- 第一迭代的 §3.2 权限表、§6 图像回灌数值、§7 重试矩阵（除本文 §3.6/§6.2 明确的新增边界）。

---

## 11. 实施顺序与风险

### 11.1 顺序

```text
t12-a  SseStreamParser + 单测骨架（无网络，可先跑）
t12-b  ILLMClient.{h,cpp} 公开面（信号/ReasoningEcho/truncatedByMaxTokens）冻结并编译通过
t12-c  OpenAILLMClient 流式/回退/看门狗/abort + test_agentstream #1~#9,#12~#17,#20
t12-d  控制器流式转发/合批/中止 + 面板/气泡/MainWindow/设置（流式开关）+ test_agentstreamui U1~U6
       ──t12 completed（reviewer 可先审流式）──
t13-a  §7 上下文治理（工具组原子/6k/预算/首条 user）
t13-b  §8 buildContext 摘要化
t13-c  设置对话框预算数字框 + MainWindow 配置读写
t13-d  test_agentcontext + 回归既有 4 个 trim 用例
verifier  构建 + test_agent + 全量回归 + 假通过检查（改 .h 先清 obj）
reviewer  逐条比对 §1.2 / §5.4 / §6 / §7.3 / §10
```

### 11.2 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 各家 SSE 实现差异（缺 `[DONE]`、`\r\n`、多行 data、usage 块） | 解析失败/卡住 | §3.4 多级终态 + §3.5 容错 + §4 回退 + stub 覆盖 #1~#9 |
| `reasoningEcho` 默认 `none` 让 DeepSeek 思考模式 400 | 该 provider 不可用 | 配置开关三档（§3.7）+ BadRequest 文案提示 + 迁移说明 |
| 预算收紧（256k→128k）导致长会话裁剪更激进 | 上下文丢失 | 最新 turn 永不裁剪（I4）+ 裁剪审计/提示（§7.9）+ 可配 |
| 6k 截断破坏 JSON | 模型读到不完整数据 | 标记说明省略量 + 提示词引导用 `get_execution_logs`；只截 tool 结果 |
| 50ms 合批导致末字延迟 | 观感 | `streamFinished/abort` 前强制 flush（§5.2） |
| 流式与非流式双路径引入状态泄漏 | 重复终态/错位 | `m_streamFinalized` + 控制器 `m_streamActive` 双去重（§5.6） |
| `setTransferTimeout(0)` 后卡死 | 挂起 | 空闲看门狗 60s + 总时长 600s（§3.6） |
| 与既有 4 个 trim 测试冲突 | 回归 | §7.8 逐条兼容；红即实现 bug，不得改断言 |
| 改头文件后依赖追踪失效 | 假通过 | 沿用第一迭代 §9.7：改 `.h` 先删对应 `*.obj`（`build-win-tests/src/core/CMakeFiles/deeplux_core.dir/agent/<Name>.cpp.obj`）再构建 |

---

## 12. 附录

### 附录 A：配置键总表

| 键 | 类型 | 默认 | clamp | 引入 |
|---|---|---|---|---|
| `agent/streamEnabled` | bool | true | — | §4.1 |
| `agent/streamIdleTimeoutMs` | int | 60000 | [5000, 600000] | §3.6 |
| `agent/streamMaxDurationMs` | int | 600000 | [60000, 3600000] | §3.6 |
| `agent/streamIncludeUsage` | bool | false | — | §3.1 |
| `agent/reasoningEcho` | string | `none` | none/current-turn/full | §3.7 |
| `agent/contextBudgetTokens` | int | 128000 | [16000, 1000000] | §7.1 |
| `agent/toolResultMaxChars` | int | 6000 | [1000, 64000] | §7.1/§7.4 |
| `agent/flowSummaryThreshold` | int | 24 | [1, 1000] | §8 |
| `agent/flowSummaryPreviewCount` | int | 24 | [1, 200] | §8 |

### 附录 B：用户可见文案（英文进模型、中文进 UI）

| 场景 | 文案 |
|---|---|
| 流式回退 | UI：`流式不可用（<reason>），已回退为整包响应。`；reason 例：`server returned non-stream body` / `stream disabled by config` / `stream rejected by server` |
| 中止 | UI：`已停止`（气泡标记 + 状态条）；历史占位文本仍为 `Stopped by user.`（`AgentController.cpp:252`，第一迭代既有文案） |
| 流中断（首 delta 后失败） | 气泡标记：`已中断`；System 气泡：既有 `llmErrorOccurred` 文本（回退不弹） |
| 达到 max_tokens | 状态条：`输出达到 max_tokens 上限` |
| reasoning 配置提示（BadRequest 且 body 含 reasoning） | 错误文案追加：`If you use a reasoning model, check agent/reasoningEcho.` |
| 上下文裁剪 | 状态条：`上下文超预算，已裁剪较早对话`（仅当丢弃 >50% 时） |

### 附录 C：代码锚点核对清单（12 处，实现前复核）

> 核对方法：在基线 `af56b5b`（第一迭代交付提交）上逐行读取；实现前用 `read` 或 `git grep -n` 复核，行号漂移以实际为准。

| # | 位置 | 实测内容（截取） |
|---|---|---|
| 1 | `src/core/agent/OpenAILLMClient.cpp:249` | `// 暂不使用 stream（DeepSeek 兼容性；流式回调已保留以备未来启用）` |
| 2 | `src/core/agent/OpenAILLMClient.cpp:271-289` | `void OpenAILLMClient::sendRequest(...)`（重置重试、保存 `m_lastBody`） |
| 3 | `src/core/agent/OpenAILLMClient.cpp:291-305` | `postRequestBody()`，`:297` `setTransferTimeout(30000)`，`:302` `post(...)` |
| 4 | `src/core/agent/OpenAILLMClient.cpp:307-319` | `abortRequest()`（停重试、断开并 abort reply） |
| 5 | `src/core/agent/OpenAILLMClient.cpp:399-450` | `parseResponse()`（`choices[0].message`，tool_calls 对象压缩，usage） |
| 6 | `src/core/agent/ILLMClient.h:141-147` | 既有信号 `responseReceived/errorOccurred/errorOccurredClassified/retryScheduled` |
| 7 | `src/core/agent/ILLMClient.cpp:44-47` | `reasoning_content` 回传（`msg.role=="assistant" && !isNull`） |
| 8 | `src/core/agent/AgentController.cpp:443-460` | `onLLMResponse` 唯一历史提交点（assistant+reasoning） |
| 9 | `src/core/agent/AgentController.cpp:630-640` | 每条 tool call 追加一条 `role=tool`（`QJsonDocument(result).toJson(Compact)`） |
| 10 | `src/core/agent/AgentController.cpp:800-855` | `trimHistoryIfNeeded`（`maxEstTokens=256000` `:803`；单 turn 逐条删除 `:849-853`） |
| 11 | `src/core/agent/AgentController.cpp:232-254` | `abortCurrentTurn`（generation/pending/abortRequest/Idle/agentTurnAborted） |
| 12 | `src/ui/widgets/AgentChatPanel.cpp:190-209` | `addMessage` 置 `m_lastAgentBubble`；`appendToLastMessage` 走 `appendText` |

（另核对：`AgentController.cpp:316-340` buildContext 全量模块与 `recentEvents(5)`；`:586-673` extendAgentLoop；`OpenAILLMClient.cpp:196-239` 重试矩阵；`OpenAILLMClient.h:67-87` 成员；`AgentMessageBubble.h:56-68` 气泡 API；`MainWindow.cpp:838-840/1716-1723/1783-1801/6140-6188` 接线；`tests/test_agentcontroller.cpp:427-560` 四个 trim 用例；`tests/test_agentretry.cpp:22-110` HTTP stub。）

### 附录 D：与第一迭代的兼容矩阵

| 第一迭代能力 | 第二迭代影响 | 兼容结论 |
|---|---|---|
| 25 个工具契约 / 权限矩阵 | 无改动 | 完全兼容 |
| 错误分类与重试矩阵 | 流式新增"首 delta 前可重试"边界，非流式不变 | 兼容（更严格） |
| `abortCurrentTurn` | 新增 flush + `agentStreamAborted` + 半截不提交 | 兼容（语义增强） |
| 图像回灌 / 视觉降级 | 不涉及（流式终态后才回灌） | 兼容 |
| 审计字段 | 新增 `result="trimmed"/"truncated"` 取值；新增回退条目 | 向后兼容（枚举扩展） |
| `AgentBridge` 外部 IPC | `wait` 类工具期间无流式；外部调用仍走非流式路径（客户端实例共享，但 IPC 不触发 LLM 请求） | 兼容 |
| 构建/测试 | 新增 2 个测试文件；`tests/CMakeLists.txt` 两批追加 | 兼容 |
