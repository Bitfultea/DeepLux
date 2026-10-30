# DeepLux Agent 第一迭代设计冻结（工具契约 / 异步底座 / 闭环与观测 / 图像回灌 / LLM 重试）

- 状态：**已冻结并交付**（2026-10-27；实现/审核 pass/复验均通过，交付记录见 [agent-improvement-roadmap.md](agent-improvement-roadmap.md) §11）
- 上游文档：[agent-improvement-roadmap.md](agent-improvement-roadmap.md) §5
- 基线提交：`d381129`（分支 `Windows_work`）
- 适用代码：`src/core/agent/**`、`src/core/engine/RunEngine.{h,cpp}`（仅新增只读快照与结构化校验 API）、`src/ui/widgets/AgentChatPanel.*`、`src/ui/views/MainWindow.cpp`（Agent 接线）
- 读者：implementer-core（T2/T4）、implementer-tools（T3）、verifier、reviewer
- 文档用途：**实现者照此编码即可，不需要再回读 roadmap 猜语义**。凡本文与 roadmap 冲突处，以本文为准并已注明理由。

---

## 0. 阅读顺序与生效规则

1. 先读 §2（代码事实基线）确认你要改的位置；所有行号均在基线 `d381129` 上核对过（核对清单见附录 A）。
2. 再读你任务对应的契约章：T2 → §3、§7；T3 → §4；T4 → §5、§6。§8/§9 是所有人必读（文件所有权、测试断言）。
3. 契约中出现 `必须`/`禁止`/`不得` 的句子是**验收项**，reviewer 会逐条比对；`应当` 是强烈建议，偏离需在 PR 描述里说明。
4. 任何"先实现再说"的语义变更（例如新增第 7 个工具、改变错误 JSON 形状）都必须先改本文，再改代码。

---

## 1. 范围与硬不变量

### 1.1 本迭代交付（只有这些）

| 编号 | 交付 | 主责 |
|---|---|---|
| A | 工具契约单源化：`ToolDefinition` 元数据 + 权限派生 + 派发注册表 + 契约测试 | T2（schema 元数据/权限派生/**新工具注册**）/ T3（`AgentActor` 派发注册表与 handler） |
| B | 异步执行底座 + 6 个闭环/观测工具 + `run_flow(wait)` | T3（工具）/ T4（排队与取消） |
| C | 图像回灌（工具产图 → 下一轮多模态输入） | T4 |
| D | LLM 错误分类 + 重试 + 视觉能力自愈降级 | T2 |
| E | 既有缺陷 D1~D4 顺手修（§2.6） | T3（D1~D3）/ T4（D4 耗时）/ T2（D4 用量信号） |

### 1.2 硬不变量（任何实现都不得破坏）

1. **治理不降级**：三级权限（Observer/Advisor/Autopilot）× 只读/写/危险 的判定必须在执行前生效，且判定来源**只有** `ToolSchema`。禁止在 UI 层或 Actor 层再维护第二份名单（`src/core/agent/AgentController.cpp:29-34` 这份硬编码名单必须被删除，见 §3.3）。
2. **UI 不冻结**：任何等待流程完成的工具调用都不得让 GUI 线程进入忙等；等待必须走嵌套事件循环（`QEventLoop`）+ 超时 + 可取消（§5.4）。
3. **无 shell**：本迭代不新增任何进程/命令执行能力；`AgentBridge` 继续拒绝 `execute`（`src/core/agent/AgentBridge.h:39-125`）。
4. **写入落点封闭**：工具产图只写 `QDir::tempPath() + "/deeplux-agent/"`，文件名由工具生成，**不接受外部路径参数**（§6.2）。这与 `sam_annotate` 的"显式磁盘写入 ⇒ dangerous"口径不冲突：危险与否按**落点归属**判定（§3.2.2）。
5. **单源**：工具名、参数、权限、超时只在 `ToolSchema` 注册一处；`AgentActor` 只持有"名字 → 处理器"的映射，不得再写第二份参数表。
6. **可回滚**：项目状态变更仍必须经 `QUndoStack`（`src/core/agent/AgentActor.cpp:234-244` 既有撤销栈）；本迭代新增的写工具必须入栈。

### 1.3 非目标（第二/三迭代）

流式 SSE、reasoning 折叠、上下文组内原子裁剪与 6k 截断、审计落盘 JSONL、MCP 外放、多服务商、参数扫描（`sweep_node_parameter`）、按节点隔离的**可取消**执行原语（§4.4.5 说明为何本轮不做）。

---

## 2. 代码事实基线（实现前必读，全部为已核实行号）

### 2.1 线程与状态机

- `AgentController` 是单例，`AgentObserver`/`AgentActor`/`ILLMClient` 全部**运行在 GUI 线程**（`src/core/agent/AgentController.cpp:69-70` 构造、`:79-102` 初始化）。
- 四态状态机 `Idle/Thinking/Confirming/Executing`（`src/core/agent/AgentController.h:42-46`；迁移实现 `AgentController.cpp:174-191`；`stateName` `:193-205`）。
- busy gate 就是 `state() != Idle`：新用户消息在非 Idle 被拒（`AgentController.cpp:246-249`、`:286-289`），外部 IPC 工具调用在非 Idle 返回 busy（`:597-599`）。
- 结论：**不要引入第二个 busy 布尔量**（[agent-loop-design-review.md](agent-loop-design-review.md) §Current Decision 已锁定该决策）。

### 2.2 工具与权限

- 19 个工具注册在 `ToolSchema::registerDefaultTools()`（`src/core/agent/ToolSchema.cpp:129-258`）。
- `ToolDefinition` 目前只有 `dangerous` 一个权限字段（`src/core/agent/ToolSchema.h:36-45`，`dangerous` 在第 40 行）。
- 只读名单是**硬编码**的匿名命名空间函数（`AgentController.cpp:29-34`，工具集 = `get_flow_state/get_available_plugins/get_module_params_schema/get_run_results/read_documentation`）。
- `dangerous` 走 schema（`AgentController.cpp:55-57`）。
- 权限判定在两个地方重复实现：
  - LLM 循环内 `onLLMResponse`（`AgentController.cpp:352-360`，其中 355/356 行为判定）；
  - 外部 IPC `handleToolCall`（`AgentController.cpp:603-616`）。
- 另有审计 `undoable` 用只读名单反推（`AgentController.cpp:428`、`:499`、`:627`）。
- 派发是 `if/else` 长链（`src/core/agent/AgentActor.cpp:246-316`，链体 `:270-312`）；参数校验只有 required/enum（`AgentActor.cpp:38-55`）；引擎忙时拒绝变更类工具（`:260-267`）。

### 2.3 执行引擎（RunEngine）

- 运行控制：`runOnce/runOnceAsync/stepOnce/start/pause/resume/stop`（`src/core/engine/RunEngine.h:122-130`）。
- 忙判定：`isBusy()`（`RunEngine.h:106-110`）。
- 运行前校验：`QStringList validateFlow() const`（`RunEngine.h:143`，实现 `RunEngine.cpp:1830-1865`），**只遍历引擎已加载的模块**。
- 输出访问：`getOutput/hasOutput`（`RunEngine.h:147-148`）、`lastOutput/lastOutputModuleName/moduleOutput`（`RunEngine.h:152-155`）。
- 每节点端口输出缓存 `m_nodeOutputs`（`RunEngine.h:286`，写入点 `RunEngine.cpp:1351`、`:1707`、`:1816`）——**私有且无锁读取**，这是 §4.8 必须新增只读快照 API 的原因。
- 公开并行原语 `executeParallel(names, sharedInput)`（`RunEngine.h:168`，实现 `RunEngine.cpp:95-171`）：**同步阻塞调用线程**（`:159-163` 等 `QWaitCondition`），故不得直接在 GUI 线程用于长任务（§4.4.5）。
- 单节点输入组装已存在但私有：`collectModuleInputs(moduleName, pipelineData)`（`RunEngine.cpp:1196-1240`），依赖 `m_nodeOutputs` 与 `m_connections`。
- 信号：`runStarted/runFinished(RunResult)/moduleStarted(moduleId)/moduleFinished(moduleId,success,elapsedMs)/errorOccurred/breakpointHit`（`RunEngine.h:186-197`）。
- 异步执行走 `QThreadPool`：`runOnceAsync()`（`RunEngine.cpp:183-188`）→ `executeRunAsync()`（`:977-988`，`:984` 入池），因此 `runFinished` 是从**工作线程**发出的，GUI 线程接收为排队信号。`RunResult` 已 `qRegisterMetaType`（`RunEngine.cpp:53-54`）。

### 2.4 LLM 客户端

- `ILLMClient` 抽象：`AgentImageAttachment`（`src/core/agent/ILLMClient.h:19-23`）、`AgentMessage`（`:28-35`）、`AgentConversation`（`:40-45`）、`AgentResponse`（`:50-58`）、`supportsImageInput()`（`:81-83`）、`sendRequest`（`:86`）、信号 `responseReceived/errorOccurred`（`:88-90`）。
- 多模态序列化已就绪：`AgentConversation::toOpenAIMessages()` 把 `msg.images` 编成 `content:[{type:text},{type:image_url}]`（`src/core/agent/ILLMClient.cpp:19-36`）。
- `OpenAILLMClient`：非流式；请求体 `buildRequestBody`（`OpenAILLMClient.cpp:68-94`）；发送 `sendRequest`（`:96-121`，`setTransferTimeout(30000)` 在 `:113`）；成功收包 `onReplyFinished`（`:123-138`）；解析 `parseResponse`（`:140-183`）；网络错误 `onNetworkError`（`:185-198`）。
- 现状**无错误分类、无重试、无取消**（`sendRequest` 只在 `:98-103` 中止上一请求）。
- 视觉开关：`setVisionEnabled`（`OpenAILLMClient.h:27`，成员 `:48`），由配置注入（`src/ui/views/MainWindow.cpp:6118`、`:837`），配置项 `agent/visionEnabled` 默认 true（`src/ui/dialogs/AgentSettingsDialog.cpp:198`）。

### 2.5 循环、上下文与 UI 接线

- 上限：20 轮 / 单轮 10 calls（`AgentController.cpp:24-25`），检查位置 `:455-464`，轮次自增 `:465`。
- 工具结果写回为独立 `role=tool` 消息（`AgentController.cpp:491-507`），全量 JSON（`:505`）。
- 续跑：`emit agentLoopIterating()` 后进入 `Thinking` 并再次请求（`AgentController.cpp:519-531`）。
- 历史裁剪：256k 估算 + 图片字节计入（`AgentController.cpp:535-590`，图片字节 `:546-547`，64 条硬上限 `:577-589`）。
- 确认链路：`confirmPendingTools()`（`AgentController.cpp:382-397`，当前**同步**调用 `doConfirmPendingTools`，`:395`）→ `doConfirmPendingTools()`（`:399-432`）→ `transitionTo(Executing)`（`:405`）→ `extendAgentLoop`（`:406-409`）。
- 拒绝链路：`rejectPendingTools()`（`:434-445`）。
- 图像附件上限：`AgentController::kMaxImageAttachments = 4`（`AgentController.h:28`；**注意** `AgentController.cpp:26` 又重复定义了一份 `constexpr int kMaxImageAttachments = 4`，本迭代由 T4 删除重复定义，只保留头文件那份）；请求体上限 `kMaxImageAttachmentBytes = 8MB`（`AgentController.cpp:27`）。
- UI 接线：面板创建 `MainWindow.cpp:1634-1635`；发送 `:1683-1706`；响应/错误 `:1707-1729`；待确认卡片 `:1730-1761`；确认/取消连接 `:1762-1765`；日志与循环提示 `:1771-1778`；Agent 设置加载 `MainWindow::loadAgentSettings` `:6096-6128`。
- 面板信号：`userMessageSent/userMessageWithImagesSent/toolPreviewConfirmed/toolPreviewCancelled`（`src/ui/widgets/AgentChatPanel.h:62-66`），附件上限 `AgentChatPanel.h:37`。
- 视口：`ViewportWidget::currentImage()`（`src/ui/widgets/ViewportWidget.h:57`）、`HImageWidget::currentImage()`（`src/ui/widgets/HImageWidget.h:43-45`）、主数据源视口选择 `MainWindow::primaryDataSourceViewport()`（`MainWindow.cpp:4364-4383`）。
- 审计结构：`AgentActionLogEntry` 只有 timestamp/actor/action/params/result/undoable（`src/core/agent/GuiEvent.h:76-83`）。
- 日志源：`Logger::logs()` / `logs(level)` / `logs(category)`（`src/core/common/Logger.h:50-53`；实现 `Logger.cpp:106-126`、`:151-154`），内存环形上限 1000（`Logger.h:92`），`LogEntry{timestamp,level,message,category}`（`Logger.h:23-28`）。`RunEngine` 已用 category `"Run"` 打日志（`RunEngine.cpp:59`、`:178`、`:186`、`:372`、`:664` 等）。

### 2.6 已确认缺陷（本迭代顺手修）

| 编号 | 现状（行号） | 修法 | 主责 |
|---|---|---|---|
| D1 | `removeModule` 不校验实例存在即入撤销栈并返回 `removed`（`AgentActor.cpp:411-417`） | 先 `proj->moduleById()`，缺失返回 `{error, code:"node_not_found"}`（§3.6） | T3 |
| D2 | `create/open/save_project` 不入撤销栈，但审计 `undoable` 用"非只读"反推为 true（`AgentActor.cpp:374-381`、`:650-654`、`:699-710`；判定 `AgentController.cpp:428`、`:499`、`:627`） | 审计 `undoable` 改由派发注册表的 `ToolHandler::undoable` 提供（单一事实来源，§3.3/§3.4），工程级三工具为 false | T2（判定 + 注册表接口）/ T3（注册表取值） |
| D3 | `createProject` 失败无结构化错误码（`AgentActor.cpp:376-378`） | 失败返回 `{error:"Failed to create project", code:"project_create_failed"}` | T3 |
| D4 | 工具调用无耗时、无 token 用量展示（`AgentController.cpp:618` 现只有一条日志；`OpenAILLMClient.cpp:178-180` 已解析 usage 但未上报） | `AgentActionLogEntry` 增 `durationMs`/`errorCategory`；`AgentController` 增 `llmUsageReported(prompt,completion)` 信号；UI 状态条显示（§3.7） | T2（信号/字段）+ T4（耗时采集/UI） |

---

## 3. 工具契约单源化（T2 主责，T3 配合）

### 3.1 `ToolDefinition` 新增字段（含 C++17 兼容约束）

```cpp
// src/core/agent/ToolSchema.h
struct ToolDefinition {
    QString name;
    QString description;
    QList<ToolParam> params;
    bool dangerous = false;   // 既有：破坏性/写外部资源（保持第 4 位，理由见下）
    bool readOnly = false;    // 新增：只观察，不改变工程状态、不写外部资源
    int timeoutMs = 0;        // 新增：单次调用允许的最长同步等待；0 表示默认 30000
};
```

**必须保持 `dangerous` 在第 4 位。** roadmap §5.1 的示例把 `readOnly` 放在 `dangerous` 之前，但本工程是 C++17，`registerTool({...})` 使用**按位置的聚合初始化**（`ToolSchema.cpp:141-144`、`:185-188`、`:237-257` 用第 4 个位置 `true` 表示 dangerous）。若照抄 roadmap 顺序，`remove_module`/`save_project`/`sam_annotate` 会被静默改成 `readOnly=true`，等价于**把危险工具降级为只读**——直接击穿 §1.2 第 1 条不变量。因此：

- 既有 19 个注册点**一字不动**（位置语义保持 `dangerous`）。
- 新增工具一律通过工厂函数注册，禁止裸聚合初始化：

```cpp
// src/core/agent/ToolSchema.h 新增自由函数（namespace DeepLux）
ToolDefinition makeReadOnlyTool(const QString& name, const QString& description, const QList<ToolParam>& params = {},
                                int timeoutMs = 0);   // readOnly=true,  dangerous=false
ToolDefinition makeWriteTool(const QString& name, const QString& description, const QList<ToolParam>& params = {},
                             int timeoutMs = 0);      // readOnly=false, dangerous=false
ToolDefinition makeDangerousTool(const QString& name, const QString& description, const QList<ToolParam>& params = {},
                                 int timeoutMs = 0);  // readOnly=false, dangerous=true
```

- 工厂函数在实现里 `Q_ASSERT(!(readOnly && dangerous))`，并在 `toJsonSchema()` 输出三个字段（`ToolSchema.cpp:60-75` 扩展：`readOnly`、`timeoutMs` 一并序列化，供契约测试与 IPC 自描述使用）。
- 默认超时常量：`ToolSchema` 内 `static constexpr int kDefaultToolTimeoutMs = 30000;`，并提供
  `static int effectiveTimeoutMs(const ToolDefinition&)`：`timeoutMs > 0 ? timeoutMs : kDefaultToolTimeoutMs`。

**字段语义（必须逐条实现，reviewer 会核对）：**

| 字段 | 为 true 的含义 | 允许的副作用 |
|---|---|---|
| `readOnly=true` | 只观察应用状态 | 读内存/读文件；可写**工具自己命名**的临时产物（§1.2 第 4 条） |
| `readOnly=false` 且 `dangerous=false` | 改变工程状态，但可撤销、落点在工程内 | 经 `QUndoStack` 写工程；触发流程运行 |
| `dangerous=true` | 破坏性/写外部资源/不可撤销 | 删除节点、保存工程、写用户指定路径（`save_project`、`sam_annotate`） |

不变式：`dangerous ⇒ !readOnly`（工厂函数保证；契约测试断言全部工具满足）。

### 3.2 权限派生与判定表

#### 3.2.1 单一判定函数（替换两处重复实现）

在 `src/core/agent/AgentController.cpp` 匿名命名空间内，删除 `isReadOnlyToolName`（`:29-34`），保留 `isDangerousToolName`（`:55-57`）但改为内部实现细节，新增：

```cpp
namespace {
struct ToolAccess {
    bool readOnly = false;
    bool dangerous = false;
};

// 唯一的权限事实来源：ToolSchema。未注册的工具按"写工具"处理（fail closed）。
ToolAccess toolAccess(const QString& toolName) {
    const ToolDefinition def = ToolSchema::instance().findTool(toolName);
    if (def.name.isEmpty())
        return {false, false};
    return {def.readOnly, def.dangerous};
}

// 唯一的确认判定；onLLMResponse 与 handleToolCall 必须都调用它。
bool requiresConfirmation(AgentController::PermissionLevel level, const ToolAccess& access) {
    using Level = AgentController::PermissionLevel;
    switch (level) {
    case Level::Observer:
        return !access.readOnly;                                  // 只读放行，写/危险拒绝（不在本函数决定"拒绝"）
    case Level::Advisor:
        return !access.readOnly;                                  // 任何写操作都要确认
    case Level::Autopilot:
        return access.dangerous;                                  // 只对危险确认
    }
    return true;
}
} // namespace
```

> Observer 的"拒绝"仍由调用点显式返回 `Permission denied: Observer mode`（保持既有错误文案，`test_agentpermissions.cpp:38` 依赖它）。`requiresConfirmation` 在 Observer 下返回 `!readOnly` 只用于让调用点区分"拒绝"与"确认"两条路径，不得直接当作放行条件。

#### 3.2.2 完整判定表（验收基准）

| 权限级别 | `readOnly=true` | 写（`readOnly=false, dangerous=false`） | `dangerous=true` |
|---|---|---|---|
| Observer | **执行** | **拒绝**（`{"error":"Permission denied: Observer mode"}`） | **拒绝**（同上） |
| Advisor | 执行 | **待确认**（`pending_confirmation` + `toolsPendingConfirmation`） | **待确认** |
| Autopilot | 执行 | **直接执行** | **待确认** |

落点归属规则（判定 `dangerous` 的唯一口径）：写**用户指定路径**、或写**工程目录之外的持久位置** ⇒ dangerous；只写**工具自己命名、位于 `temp/deeplux-agent/` 的临时产物** ⇒ 不算危险，可以是 readOnly。据此：

- `sam_annotate`（用户传 `sessionPath`）保持 `dangerous=true`（`ToolSchema.cpp:237-257`）。
- `sam_predict`（触发外部推理、不写文件）保持 `readOnly=false, dangerous=false`（`ToolSchema.cpp:218-233`）：Observer 拒绝、Advisor 确认、Autopilot 直执行——与 `test_agentpermissions.cpp:80-110` 既有断言一致，**不得改动**。
- `capture_viewport_image` / `export_node_output`：`readOnly=true, dangerous=false`（只写 temp 产物）。

#### 3.2.3 既有 19 个工具的目标元数据快照（契约测试据此断言）

| 工具 | readOnly | dangerous | timeoutMs |
|---|---|---|---|
| create_project | false | false | 0 |
| add_module | false | false | 0 |
| remove_module | false | **true** | 0 |
| set_param | false | false | 0 |
| connect_modules | false | false | 0 |
| disconnect_modules | false | false | 0 |
| run_flow | false | false | **60000**（扩展后，§4.7） |
| stop_flow | false | false | 0 |
| get_flow_state | **true** | false | 0 |
| get_available_plugins | **true** | false | 0 |
| save_project | false | **true** | 0 |
| get_module_params_schema | **true** | false | 0 |
| get_run_results | **true** | false | 0 |
| open_project | false | false | 0 |
| pause_flow | false | false | 0 |
| resume_flow | false | false | 0 |
| read_documentation | **true** | false | 0 |
| sam_predict | false | false | 0（工具自带 `timeoutMs` 参数，见 `ToolSchema.cpp:233`） |
| sam_annotate | false | **true** | 0（同上，`ToolSchema.cpp:256`） |
| validate_workflow | **true** | false | 30000 |
| get_node_runtime_outputs | **true** | false | 0 |
| get_execution_logs | **true** | false | 0 |
| tune_node_and_verify | false | false | 300000 |
| capture_viewport_image | **true** | false | 0 |
| export_node_output | **true** | false | 0 |

（前 19 行是既有工具；后 6 行是本迭代新增。`timeoutMs=0` 表示"非等待类工具，用默认 30000 但实际不产生等待"——只有 `run_flow(wait=true)` 与 `tune_node_and_verify` 会真正消费该字段，见 §5.4。）

### 3.3 替换硬编码只读名单：精确文件与行号

| # | 文件:行 | 现状 | 替换为 |
|---|---|---|---|
| 1 | `src/core/agent/AgentController.cpp:29-34` | `isReadOnlyToolName()` + 5 项硬编码 `QStringList` | **删除**；改由 `toolAccess()`（§3.2.1） |
| 2 | `src/core/agent/AgentController.cpp:55-57` | `isDangerousToolName()` | 保留为 `toolAccess().dangerous` 的薄封装或直接内联删除 |
| 3 | `src/core/agent/AgentController.cpp:352-360` | `onLLMResponse` 内联判定（355/356） | `const ToolAccess a = toolAccess(name); if (level==Advisor ? !a.readOnly : (level==Autopilot ? a.dangerous : false))`，或直接 `requiresConfirmation(level, a)` |
| 4 | `src/core/agent/AgentController.cpp:603-616` | `handleToolCall` 内联判定（603/604/605-608） | 同一 `requiresConfirmation(m_permissionLevel, toolAccess(toolName))`；Observer 分支保持 `:605-606` 文案 |
| 5 | `src/core/agent/AgentController.cpp:428` | `e.undoable = !isReadOnlyToolName(...)` | `e.undoable = isAgentUndoable(m_actor, tools[i].first)` |
| 6 | `src/core/agent/AgentController.cpp:499` | 同上 | 同上 |
| 7 | `src/core/agent/AgentController.cpp:627` | `e.undoable = !readOnly && !result.contains("error")` | `e.undoable = isAgentUndoable(m_actor, toolName) && !result.contains("error")` |
| 8 | `src/core/agent/AgentActor.cpp:260-267` | `mutatesFlow` 硬编码 7 个工具名 | 移到派发注册表的 `mutatesProject` 标志（§3.4），行为集合保持一致且追加 `tune_node_and_verify` |

`isAgentUndoable()` 的唯一定义（放在 `AgentController.cpp` 匿名命名空间，**事实来源仍是注册表**）：

```cpp
// D2 修复：审计 undoable 必须与"是否真的压入了 QUndoStack"一致，
// 而不是用"非只读"反推（create/open/save 三个工程级工具不入撤销栈）。
// 实现：AgentController 持有一个可访问的 m_actor，直接问注册表。
bool isAgentUndoable(const AgentActor* actor, const QString& toolName) {
    return actor && actor->toolUndoable(toolName);
}
```

`AgentActor::toolUndoable(name)` 返回注册表 `ToolHandler::undoable`（§3.4），未注册返回 `false`。目标值：

| 工具 | `undoable` | 说明 |
|---|---|---|
| add_module / remove_module / set_param / connect_modules / disconnect_modules | true | 压 `QUndoStack`（`AgentActor.cpp:407`、`:415`、`:484`、`:489-547`、`:549-580`） |
| tune_node_and_verify | true | 宏内多条 `AgentSetParamCmd`（§4.4） |
| create_project / open_project / save_project | **false** | 清空撤销历史或完全不入栈（D2 修正点） |
| 其余（读工具、run/stop/pause/resume、sam_*） | false | 不产生可撤销命令 |

### 3.4 `AgentActor` 派发注册表（替换 if/else）

```cpp
// src/core/agent/AgentActor.h（private 区，新增）
struct ToolHandler {
    std::function<QJsonObject(const QJsonObject&)> fn;
    bool mutatesProject = false;   // 引擎 busy 时拒绝（对齐既有 AgentActor.cpp:260-267 集合 + tune）
    bool undoable = false;         // 是否压入 QUndoStack（D2：审计 undoable 的唯一事实来源）
};
QHash<QString, ToolHandler> m_handlers;

public:
    // 契约测试用：返回已注册处理器名集合（必须与 ToolSchema 名称集合相等）
    QStringList supportedTools() const;

    // D2/审计：该工具是否产生可撤销命令（未注册 → false）
    bool toolUndoable(const QString& toolName) const;

    // 宿主注入：视口图像提供者（MainWindow 启动时设置；未设置时 capture_viewport_image 失败关闭）
    using ViewportImageProvider = std::function<QImage(QString* error)>;
    void setViewportImageProvider(ViewportImageProvider provider);

    // 中止正在等待的运行（由 AgentController::abortCurrentTurn 调用）
    void abortRunningTool();
```

> 注册表两个布尔量的目标值（与 §3.3 表、§3.2.3 表一致）：
> `mutatesProject=true` → create_project / open_project / add_module / remove_module / set_param / connect_modules / disconnect_modules / tune_node_and_verify；
> `undoable=true` → add_module / remove_module / set_param / connect_modules / disconnect_modules / tune_node_and_verify。
> 契约测试断言 `undoable ⇒ mutatesProject`（§3.5 第 11 条）。

> 头文件新增依赖：`#include <QHash>`、`#include <QImage>`、`#include <functional>`、`#include <QStringList>`；`AgentRunWaiter* m_runWaiter` 用前置声明 + 原始指针（在 `AgentActor` 构造时创建、析构时 delete，或持有 `std::unique_ptr` 并前置声明配合 cpp 内联析构）。

**实现要求：**

1. 构造函数（`AgentActor.cpp:234-242`）内一次性建立注册表，19 个既有 + 6 个新增 = 25 条，**每条用 `[this](const QJsonObject& p) { return this->createProject(p); }` 形式**，禁止 lambda 捕获外部状态。
2. `executeTool()` 保留 `ToolSchema::hasTool` 与 `validationError` 两道前置校验（`AgentActor.cpp:246-258`），随后：
   - 注册表命中 → 执行 `handler.fn(params)`；
   - 注册表未命中 → `{error: "Tool not registered: X", code: "tool_not_registered"}` 并 `emit toolError`（防"schema 有、实现无"的漂移）。
3. `mutatesProject` 为 true 且 `RunEngine::instance().isBusy()` → `{error:"Flow is running; stop it before changing the project", code:"engine_busy"}`（沿用 `AgentActor.cpp:263-267` 文案）。
4. 删除 `:270-312` 的 if/else 链与 `:308-311` 的 `Tool not yet implemented` 分支。
5. `supportedTools()` 返回 `m_handlers.keys()`，按名排序后返回（稳定顺序便于断言）。

### 3.5 契约测试 `tests/test_agenttoolcontract.cpp`（T2 创建）

必测断言（逐条一个 `testXxx`）：

1. `testToolNamesUnique`：`allTools()` 名称无重复、无空串。
2. `testDescriptionsNonEmpty`：每个 description `trimmed().size() >= 8`（防占位符）。
3. `testRequiredParamsExist`：用 `toOpenAIFunction()` 取出 `parameters.required`，每个名字都必须在 `properties` 内。
4. `testParamTypesValid`：每个参数 `type ∈ {string,integer,number,boolean,array,object}`，或 `enumValues` 非空（enum 时 `toJson()` 输出 `type=="string"` 且含 `enum`，见 `ToolSchema.cpp:5-25`）。
5. `testDangerousImpliesNotReadOnly`：全部工具 `!(dangerous && readOnly)`。
6. `testMetadataSnapshot`：逐项比对 §3.2.3 表格的 readOnly/dangerous（19 既有 + 6 新增）。
7. `testSchemaMatchesHandlers`：`ToolSchema::instance().allTools()` 名称集合 == `AgentActor().supportedTools()` 集合（**双向**）。构造 `AgentActor` 前需先 `registerDefaultTools()`；注册表在构造函数内建立，不依赖插件加载，因此本测试无需安装任何插件。
8. `testNewToolsRegistered`：6 个新工具全部 `hasTool()`，且 `timeoutMs` 与表格一致。
9. `testToJsonSchemaExposesMetadata`：`findTool(x).toJsonSchema()` 含 `readOnly`/`dangerous`/`timeoutMs` 三键。
10. `testPermissionMatrix`：对**每个工具** × 3 个权限级别，断言 `requiresConfirmation` 结果与 §3.2.2 表格一致（需要把该函数暴露到测试可见：在 `AgentController` 加
    `DEEPLUX_TEST_VISIBILITY static bool toolRequiresConfirmation(PermissionLevel, const QString& toolName);`，实现转调匿名命名空间函数）。
11. `testRegistryFlagsConsistent`：对每个工具断言 `undoable ⇒ mutatesProject`，且 §3.4 末尾列出的两个集合逐项相等（`supportedTools()` 全量覆盖）。

`tests/CMakeLists.txt`：在 `TEST_SOURCES`（`:14` 起，agent 组在 `:49-56`、`:79`）追加 `test_agenttoolcontract.cpp` 与 `test_agentretry.cpp`；无需额外 target 依赖（不加载真实插件）。

### 3.6 错误 JSON 统一口径（新工具强制，旧工具不回归）

- **工具失败**（无法完成调用）：
  ```json
  {"error": "<可直接给用户看的一句话>", "code": "<机器可读的错误码>"}
  ```
- **读工具成功**：不强制 `success`；按各自契约返回字段（与既有 `get_flow_state` 一致）。
- **写/执行工具**：必须含 `"success": true|false`。`success:false` 时**同时**给 `error` + `code`（可另带诊断字段）。
- **既有 19 个工具保持原形状**（只有 `error`，无 `code`），避免破坏既有测试与提示词；D1/D3 修复时**追加** `code` 而不是替换 `error`。
- **成功判定单源（集成收口 B1 修订）**：所有消费方（批量执行、对话历史、直接 IPC、MCP、审计、UI）必须调用同一个判定入口，不得各自 `contains("error")`。判定顺序：存在布尔 `success` → 以 `success` 为准；否则存在 `error` → 失败；否则成功。由此 `{"success":false,...}` 永远不会被记成成功。
- **批量执行语义（集成收口 B1 修订，替代原 `partial_failed` 提前返回）**：
  - 参数预校验失败时，本批**零执行**，但必须为每个 tool_call 生成结构化结果（`code:"skipped_after_failure"`），保证 assistant tool_call 与 tool 结果一一配对。
  - 执行中首次失败后停止本批剩余调用，并为剩余每个调用生成同一个跳过结果；返回的 `results` 与输入 `tools` 一一对应，附带 `executed`（只计真实执行数）、`failedTool`、`error`、`failedCode`。
  - 跳过不记为执行成功；审计必须为每个调用留一条记录（成功/失败/跳过/拒绝分别可区分）。

### 3.7 审计与用量（D2/D4）

```cpp
// src/core/agent/GuiEvent.h:76-83 扩展
struct AgentActionLogEntry {
    QDateTime timestamp;
    QString actor;
    QString action;
    QString params;
    QString result;
    bool undoable = false;
    int durationMs = -1;             // 新增：工具执行耗时；-1 = 未采集
    QString errorCategory;           // 新增：LLM 错误类别名（Auth/RateLimit/...）；空 = 无
    QString toolCallId;              // 新增：与 LLM tool_call 对齐，便于排障
};
```

**耗时采集（D4）**：`AgentActor::executeTool` 用 `QElapsedTimer` 计单次工具耗时，返回前写入内部字段 `QJsonObject{{"_elapsedMs", ms}}`。消费方读取后**必须**立刻 `remove("_elapsedMs")`：
- `AgentController::doConfirmPendingTools`（`:418-430`）/ `extendAgentLoop`（`:487-500`）：读审计后移除，再序列化进 `role=tool` 历史（`AgentController.cpp:505`）；
- `AgentController::handleToolCall`：返回给外部 IPC 之前移除（`:620-629`）。

`_elapsedMs` 在任何情况下都不得进入模型上下文或 IPC 响应体（`_` 前缀 = 内部字段，与 `AGENTS.md` 的插件参数约定一致）。批量执行时 `executeTools` 的 `results[i]` 与 `tools[i]` 一一对应（`AgentActor.cpp:346-350`），因此逐条读 `_elapsedMs` 即可，无需按比例摊分。

- `AgentController` 新增 `void llmUsageReported(int promptTokens, int completionTokens);`，在 `onLLMResponse`（`:328`）读取 `resp.promptTokens/completionTokens`（已解析，`OpenAILLMClient.cpp:178-180`）后发出；`MainWindow` 在状态条显示 `tokens: 1234+567`（T4）。

---

## 4. 6 个新工具契约（T3 主责）

### 4.0 通用约定（6 个工具全部适用）

**节点标识 `nodeName` 解析（唯一规则，先到先得）：**

1. `ProjectManager::currentProject()->modules()` 中 `ModuleInstance.name == nodeName`（`ModuleInstance{id,moduleId,name}`，`src/core/model/Project.h:62-65`）；
2. 否则 `ModuleInstance.id == nodeName`；
3. 得到 `ModuleInstance` 后确定**引擎运行名** `runtimeName`：
   - 若 `RunEngine::instance().getModule(inst.name)` 非空 → `runtimeName = inst.name`；
   - 否则若 `getModule(inst.id)` 非空 → `runtimeName = inst.id`（引擎侧 `runtimeModuleName()` 取 `instanceName()`，为空才回退 `name()`，见 `RunEngine.cpp:21-26`）；
   - 两者都不在引擎里（引擎未加载该工程）→ 先 `ensureEngineLoaded()` 再重试一次；
4. 仍失败 → `{"error":"Module not found: <nodeName>", "code":"node_not_found"}`。

返回 JSON 的 `node` 字段统一填实际使用的 `runtimeName`；同时返回 `instanceId`（工程 id）便于模型下次直接引用。

**时间/单位**：`elapsedMs` 为整数毫秒；`time` 为本地时间字符串 `yyyy-MM-dd HH:mm:ss.zzz`。

**空工程/未加载引擎**：需要引擎的工具（validate_workflow、get_node_runtime_outputs、tune_node_and_verify、export_node_output）必须保证引擎已加载当前工程：

```cpp
// AgentActor 内部复用同一套"确保引擎已加载"逻辑（源自 runFlow 既有做法 AgentActor.cpp:582-595）
bool AgentActor::ensureEngineLoaded(QString* error);   // project 为空 → false；isRunning() → 不改动；否则 loadProject
```

**引擎忙规则（`RunEngine::instance().isBusy()`，`RunEngine.h:106-110`）**：默认返回 `code:"engine_busy"`，仅下列三个工具例外：

| 工具 | 忙时行为 | 理由 |
|---|---|---|
| `get_execution_logs` | 正常执行 | 只读 `Logger` 内存副本，不接触引擎/工程 |
| `capture_viewport_image` | 正常执行（`lastOutput` 为上一帧图像） | 只读观察，不改变运行状态 |
| `run_flow` | 见 §4.7：`wait=true` → `already_running`；`wait=false` → 保留既有 `"Flow is already running"` | 由该工具自身状态码处理 |

`get_node_runtime_outputs` / `export_node_output` / `validate_workflow` / `tune_node_and_verify` 在忙时**必须**返回 `engine_busy`——前两者依赖的快照 API 前置条件就是 `!isBusy()`（§4.8）。

**超时口径**：`timeoutMs` 只约束"等待"（§5.4）。工具本身的同步计算不设抢占式超时（GUI 线程无法安全抢占），因此任何"可能长"的等待都必须走 `AgentRunWaiter`。

**参数表风格**：与 `ToolSchema.cpp:129-258` 一致（`ToolParam{name, type, description, required, defaultValue, enumValues}`）；类型用 `enum` 表示枚举（内部转 `string` + `enum`）。

**返回体积上限**：任一工具结果 JSON 序列化后 ≤ 8KB（`get_execution_logs` 除外，≤ 32KB），超限按"截断 + `truncated:true`"处理，不得静默丢字段。

---

### 4.1 `validate_workflow`（readOnly）

**注册（工厂）**

```cpp
registerTool(makeReadOnlyTool(
    "validate_workflow",
    "Validate the current flow without running it: missing required input ports, connection/type conflicts "
    "for disabled nodes, and empty projects. Returns structured issues with code/node/message.",
    {}, 30000));
```

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| —— | —— | —— | —— | 无参数 |

**返回（成功调用）**

```json
{
  "ok": true,
  "moduleCount": 3,
  "issues": [{"code": "missing_required_input", "node": "consumer", "port": "value",
              "message": "节点 consumer 的必需输入端口 value 无上游连接"}]
}
```

- `ok = issues.isEmpty() && moduleCount > 0`。
- `moduleCount` 取自当前工程 `modules().size()`。
- `issues[].code` 取值：`missing_required_input`、`disabled_output_bypass_conflict`（来源：`validateFlowIssues()`，§4.8）；`node` 为运行名，`port` 可为空串。

**失败语义**（工具无法完成校验）

| 场景 | 返回 |
|---|---|
| 无工程 | `{"error":"No project opened","code":"no_project"}` |
| 工程无模块 | `{"ok": false, "issues":[{"code":"no_modules","node":"","message":"Project has no modules"}], "moduleCount":0}`（**不是** error，是校验结论） |
| 引擎忙（`isBusy()`） | `{"error":"Flow is running; stop it before validating","code":"engine_busy"}` |
| 加载失败 | `{"error":"<loadProject 的错误>","code":"engine_load_failed"}` |

**权限**：`readOnly=true` → Observer 允许、Advisor 允许、Autopilot 允许；`dangerous=false`。

**实现要点**：先 `ensureEngineLoaded()`，再调 `RunEngine::validateFlowIssues()`；**不得**用字符串解析 `validateFlow()` 的中文错误（脆弱）。

---

### 4.2 `get_node_runtime_outputs`（readOnly）

**注册**

```cpp
registerTool(makeReadOnlyTool(
    "get_node_runtime_outputs",
    "Read the last runtime port outputs and status of one module node: named output ports, elapsed time, "
    "success/failure and frame id. Requires a finished run; returns engine_busy while the flow is running.",
    {{"nodeName", "string", "Module instance name (preferred) or instance id", true}}));
```

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `nodeName` | string | 是 | —— | 节点名优先，其次实例 id |

**返回**

```json
{
  "node": "MeasureCircle_1",
  "lastStatus": "success",
  "elapsedMs": 42,
  "frameId": 17,
  "outputs": {
    "image": {"type": "image", "width": 1280, "height": 1024},
    "circleCount": 3,
    "ok": true,
    "scoreList": {"type": "list", "count": 3, "first": 0.982}
  },
  "unsupportedPorts": ["contour"]
}
```

**端口值序列化规则（必须逐条实现）**

| QVariant 内容 | 输出 |
|---|---|
| `ImageData`（`QVariant::fromValue`） | `{"type":"image","width":W,"height":H,"present":valid}` |
| `int/double/bool` | 原生 JSON 数字/布尔 |
| `QString` | 字符串，超 512 字符截断并置 `truncated:true` |
| `QVariantList` | `{"type":"list","count":N,"first":<首元素若可序列化>}` |
| 其它类型（含自定义结构） | 列入 `unsupportedPorts`，不猜类型 |

**`lastStatus`** 取值：`success` / `failed` / `skipped` / `unknown`（从未运行或引擎重载后）。附带 `errorMessage`（`failed` 时）、`finishedAt`。

**失败语义**

| 场景 | 返回 |
|---|---|
| 节点不存在 | `{"error":"Module not found: X","code":"node_not_found"}` |
| 无工程 | `{"error":"No project opened","code":"no_project"}` |
| 引擎忙 | `{"error":"Flow is running; stop it before reading node outputs","code":"engine_busy"}` |
| 从未运行过 | 成功返回，`lastStatus:"unknown"`、`outputs":{}`、`note:"node has no runtime output yet; run the flow first"` |

**实现要点**：`RunEngine::moduleRuntimeSnapshot()` + `RunEngine::nodePortOutputs(runtimeName)`（§4.8），**必须**在 `!isBusy()` 时调用。

---

### 4.3 `get_execution_logs`（readOnly）

**注册**

```cpp
registerTool(makeReadOnlyTool(
    "get_execution_logs",
    "Read recent in-memory application/run logs with filters. Use category 'Run' for run-engine events, "
    "'Agent' for agent actions. Returns the newest matching entries, truncated flag when older entries exist.",
    {{"category", "string", "Exact log category, e.g. 'Run', 'Agent', 'Camera'", false},
     {"level", "enum", "Minimum level filter", false, QJsonValue("debug"),
      {"debug", "info", "warning", "error", "success"}},
     {"keyword", "string", "Case-insensitive substring match on message", false},
     {"limit", "integer", "Max entries to return, clamped to [1,200]", false, QJsonValue(50)}}));
```

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `category` | string | 否 | 空=全部 | **精确匹配** `LogEntry.category`（`Logger.cpp:117-126` 语义） |
| `level` | enum | 否 | `debug` | **最小级别**过滤（按 `LogLevel` 枚举序 `Debug<Info<Warning<Error<Success`） |
| `keyword` | string | 否 | 空=全部 | 对 `message` 做 `contains(..., Qt::CaseInsensitive)` |
| `limit` | integer | 否 | 50 | `qBound(1, limit, 200)` |

**返回**

```json
{
  "entries": [
    {"time": "2026-10-26 22:28:01.123", "level": "info", "category": "Run", "message": "Starting single run"}
  ],
  "total": 37,
  "truncated": true,
  "source": "Logger::memory"
}
```

- 顺序：**时间正序**（最旧 → 最新），取匹配集合的**最后 `limit` 条**。
- `total` = 过滤后总条数；`truncated = total > entries.size()`。
- 数据源只有 `Logger::instance().logs()`（内存上限 1000 条，`Logger.h:92`）；**不读磁盘日志文件**（文件 IO 留第二迭代）。

**失败语义**：正常永不失败（无匹配 → `entries:[]`）；非法 `limit`/`level` 由参数校验层拦截（`AgentActor.cpp:38-55` 的 enum 校验已覆盖 `level`）。

**实现要点**：`Logger::logs()` 返回副本（`Logger.cpp:151-154`），在 GUI 线程过滤即可，无需加锁。

---

### 4.4 `tune_node_and_verify`（写，Advisor 确认）

**注册**

```cpp
registerTool(makeWriteTool(
    "tune_node_and_verify",
    "Apply a parameter patch to one node atomically and verify the effect. runMode=flow (default) writes the "
    "parameters, runs the whole flow once waiting for completion, and returns before/after plus this node's "
    "runtime outputs. runMode=node only applies the patch and reports the previous frame's outputs without "
    "running anything (no side effects).",
    {{"nodeName", "string", "Module instance name (preferred) or instance id", true},
     {"parameters", "object", "Parameter patch: key -> value (string/number/bool), 1..32 keys", true},
     {"runMode", "enum", "Verification mode", false, QJsonValue("flow"), {"flow", "node"}},
     {"timeoutMs", "integer", "Wait timeout for runMode=flow, clamped to [1000,300000]", false, QJsonValue(60000)}},
    300000));
```

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `nodeName` | string | 是 | —— | 同 §4.0 解析规则 |
| `parameters` | object | 是 | —— | 键值对；值为标量（string/number/bool）。键数 1..32，重复键以 JSON 语义为准（后者覆盖） |
| `runMode` | enum | 否 | `flow` | `flow`：改参+跑整流程；`node`：只改参、不执行 |
| `timeoutMs` | integer | 否 | 60000 | `qBound(1000, timeoutMs, 300000)`，且不得超过工具 `timeoutMs`（300000） |

**返回（`runMode=flow`，成功）**

```json
{
  "success": true,
  "node": "MeasureCircle_1",
  "runMode": "flow",
  "applied": 2,
  "before": {"threshold": "50", "minArea": "100"},
  "after":  {"threshold": "80", "minArea": "100"},
  "outputs": {"image": {"type":"image","width":1280,"height":1024}, "circleCount": 3},
  "lastStatus": "success",
  "elapsedMs": 118,
  "undoable": true
}
```

**返回（`runMode=node`）**

```json
{
  "success": true, "node": "MeasureCircle_1", "runMode": "node", "executed": false, "applied": 2,
  "before": {"threshold": "50"}, "after": {"threshold": "80"},
  "staleOutputs": {"image": {"type":"image","width":1280,"height":1024}},
  "lastStatus": "success",
  "note": "runMode=node does not execute the flow; outputs are from the previous run. Use runMode=flow to verify."
}
```

**执行顺序（必须严格按此顺序，任何一步失败都不得留下半改状态）**

1. 解析节点（§4.0）→ 失败 `node_not_found`。
2. `ensureEngineLoaded()`；`isBusy()` → `engine_busy`。
3. 取出 `ModuleInstance` 当前参数，记录 `before`（仅涉及键，值统一转字符串，与 `set_param` 的 `value` 为字符串的口径一致）。**先做形状校验**：`parameters` 不是 JSON object、或键数为 0 或 > 32 → `{"success":false,"error":"'parameters' must be a non-empty object with 1..32 keys","code":"invalid_parameter"}`（`ToolSchema` 只校验 required/enum，object 的形状必须由 handler 自检，见 `AgentActor.cpp:38-55`）。
4. **预校验全部键**：对插件实例副本调用 `validateParams()`（复用 `setParam` 的类型转换与校验逻辑，`AgentActor.cpp:419-487`；把转换抽成私有 `normalizeParamValue` + `validateParamPatch`）。任一键失败 →
   `{"success": false, "error": "Invalid value for <key>: <原因>", "code": "invalid_parameter", "parameter": "<key>"}`，**工程参数零修改**。
5. 原子写入：`m_undoStack->beginMacro("Agent tune <node>")` → 每键 push 一条 set 命令 → `endMacro()`（一次撤销回滚整组）。
6. `runMode == "node"` → 直接返回（带 `staleOutputs`，取自 §4.2 的快照逻辑；若从未运行则 `staleOutputs:{}`）。
7. `runMode == "flow"`：
   - 走与 `run_flow(wait=true)` **完全相同**的执行路径（`ensureEngineLoaded` → `AgentRunWaiter::waitForRun(engine, timeoutMs, start)`，`start` 内调 `runOnceAsync()`）；
   - 等待结果 `timedOut` → 调 `RunEngine::instance().stop()`，返回 `{"success": false, "code": "timeout", "error": "Flow did not finish within <N> ms", "before": {...}, "after": {...}}`（**参数不回滚**，见下）；
   - `aborted` → `{"success": false, "code": "cancelled", ...}`；
   - 流程失败 → `{"success": false, "code": "run_failed", "failedModule": "<name>", "errorMessage": "<引擎错误>", "before": {...}, "after": {...}, "outputs": {...}}`（**参数不回滚**）；
   - 成功 → 组装 `outputs/lastStatus/elapsedMs` 返回。

**失败语义汇总**

| code | 场景 |
|---|---|
| `node_not_found` | 节点不存在 |
| `no_project` / `engine_load_failed` | 无工程 / 引擎加载失败 |
| `engine_busy` | 调用时引擎正在运行 |
| `invalid_parameter` | 任一键未通过插件 `validateParams`（含 `parameter` 字段） |
| `timeout` | 等待超时（已请求停止） |
| `cancelled` | 用户中止 |
| `run_failed` | 流程运行失败（含 `failedModule`） |

**参数不回滚的理由**：模型需要看到"改了参数后效果变差"这一事实，才能自己反驳并改回；静默回滚会让模型误以为参数没生效（visionflow 的 `tune` 也是保留改动的口径）。用户仍可一键撤销（宏）。

> **失败续跑（集成收口 B1）**：所有普通工具失败（包括流程失败、参数/节点错误及通用工具错误）停止旧批次余项并补齐 `skipped_after_failure`，将具体结果回灌模型诊断和重新规划；不自动重放失败调用。已应用参数不会因流程失败回滚。恢复轮次重新经过权限/确认，并共享原轮数、调用数和 deadline。`timeout/cancelled/paused_at_breakpoint/reentrant_wait/already_running`、权限拒绝及工程上下文失效结束本轮。撤销只覆盖工程编辑，不承诺撤销已发生的文件或设备副作用。

> 并发保护：本工具在宏未关闭时进入嵌套等待，其间用户若触发撤销，`AgentController::undoLastAgentAction()` 会因 `RunEngine::isBusy()` 为真而拒绝（`AgentController.cpp:156-170`），不会出现"运行中撤销"的中间态。

**权限**：`readOnly=false, dangerous=false` → Advisor 需确认；Autopilot 直接执行（属于"修改参数"，不是破坏性操作）。`mutatesProject=true`（引擎忙时在入口即被拒）。

**实现要点**：`tune_node_and_verify` 内部复用 `run_flow` 的等待逻辑，不得复制两份等待代码（抽 `AgentActor::runAndWait(mode, timeoutMs, QString* failedModule, QString* error)` 私有辅助）。

#### 4.4.5 为什么 `runMode=node` 不执行节点（与 roadmap 的差异及理由）

roadmap §5.2 的括号是 `runMode?`(node/flow)，暗示 node 表示"只运行该节点"。本轮**不实现**"只执行该节点"，理由是可核实的技术约束：

- 引擎里唯一能执行指定节点的原语是 `RunEngine::executeParallel(names, sharedInput)`（`RunEngine.h:168`），它**同步阻塞调用线程**直到所有任务结束（`RunEngine.cpp:159-163` 的 `QWaitCondition`），且在租约内不可中断（`RunEngine.cpp:95-102`）。
- 在 GUI 线程直接调用它，遇到相机采集/通讯类节点会**冻结界面**，违反 §1.2 第 2 条；若为它新开线程，则 `m_nodeOutputs`/生命周期的跨线程写读边界需要新一轮 TSan 结论（见 `docs/baseline/tsan-report.md` 对 `executeParallel` 的历史噪声记录），属于第二迭代"可取消的单节点执行原语"。
- 因此本轮把 `runMode` 定义为**验证范围**：`node` 是"零副作用的改参核对"（对模型仍有价值：确认写入正确、查看上一帧输出、避免误触发相机/通讯），`flow` 才是真验证。**默认 `flow`，提示词与工具描述均已写明。**

---

### 4.5 `capture_viewport_image`（readOnly，产图回灌）

**注册**

```cpp
registerTool(makeReadOnlyTool(
    "capture_viewport_image",
    "Capture the image currently shown in the main data-source viewport (falls back to the last flow output) "
    "into a temporary PNG. The returned imagePath is attached to the next model turn automatically when the "
    "model supports images.",
    {}));
```

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| —— | —— | —— | —— | 无参数（**禁止**接受路径/尺寸参数，见 §1.2 第 4 条） |

**返回**

```json
{
  "imagePath": "C:/Users/x/AppData/Local/Temp/deeplux-agent/agent_capture_viewport_image_20261026-223001-123_7.png",
  "width": 1280,
  "height": 1024,
  "source": "viewport",
  "bytes": 381204
}
```

- `source`：`viewport`（来自宿主注入的视口提供者）或 `last_output`（回退到 `RunEngine::lastOutput()`，`RunEngine.h:152`）。
- `width/height`：来源 QImage 的**原始**尺寸（回灌时的缩放只影响回灌副本，不改磁盘文件）。

**失败语义**

| 场景 | 返回 |
|---|---|
| 宿主未注入视口提供者且无 `lastOutput` | `{"error":"No image available in the viewport","code":"no_image"}` |
| 视口有控件但无图像且 `lastOutput` 无效 | 同上 |
| 写盘失败 | `{"error":"Failed to write image: <原因>","code":"image_write_failed"}` |
| 引擎忙 | **允许**（按 §4.0 忙规则例外表：只读观察，取上一帧 `lastOutput` 即可） |

**权限**：`readOnly=true, dangerous=false`。**理由**：只读观察 + 产物落在工具自命名临时目录（§3.2.2 落点归属规则）。与 `sam_predict`（readOnly=false）的区别：不触发任何外部推理/设备动作。

**实现要点（宿主注入，T4 在 MainWindow 落地）**

```cpp
// MainWindow 构造/Agent 面板创建处（MainWindow.cpp:1634 附近）注入一次：
AgentController::instance().actor()->setViewportImageProvider([this](QString* error) -> QImage {
    if (ViewportWidget* vp = primaryDataSourceViewport())          // MainWindow.cpp:4364-4383
        if (QImage img = vp->currentImage(); !img.isNull())        // ViewportWidget.h:57
            return img;
    const ImageData last = RunEngine::instance().lastOutput();     // RunEngine.h:152
    if (last.isValid())
        return last.toQImage();                                    // ImageData.h:51
    if (error) *error = QStringLiteral("No image available in the viewport");
    return {};
});
```

未注入（CLI/测试/无 UI）时工具返回 `no_image`，**不得**崩溃。已知限制（本轮接受并写入工具描述之外的文档）：叠加标注/ROI/测量线不参与截图，只取底层显示图像；3D 视口不支持（返回 `no_image`）。

---

### 4.6 `export_node_output`（readOnly，产图回灌）

**注册**

```cpp
registerTool(makeReadOnlyTool(
    "export_node_output",
    "Export one node's last runtime image output (named output port 'image', or the module's recorded output) "
    "to a temporary PNG/JPG. The returned imagePath is attached to the next model turn automatically.",
    {{"nodeName", "string", "Module instance name (preferred) or instance id", true},
     {"format", "enum", "Image format", false, QJsonValue("png"), {"png", "jpg"}}}));
```

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `nodeName` | string | 是 | —— | 同 §4.0 |
| `format` | enum | 否 | `png` | `png` 无损 / `jpg` 质量 90 |

**返回**：`{"imagePath": "...", "width": W, "height": H, "source": "module_output", "format": "png", "bytes": N}`

取值优先级：`RunEngine::nodePortOutputs(runtimeName)["image"]` → `RunEngine::moduleOutput(runtimeName)`（`RunEngine.h:155`）。两者都无效 → 失败。

**失败语义**

| 场景 | 返回 |
|---|---|
| 节点不存在 | `{"error":"Module not found: X","code":"node_not_found"}` |
| 节点无图像输出 | `{"error":"Node '<name>' produced no image output","code":"no_image_output"}` |
| 引擎忙 | `{"error":"Flow is running; stop it before exporting node output","code":"engine_busy"}` |
| 写盘失败 | `{"error":"Failed to write image: <原因>","code":"image_write_failed"}` |

**权限**：`readOnly=true, dangerous=false`（只写 temp 产物）。

**备注**：本工具**不落工程目录**、不接受 `outputPath`。若用户要保存到工程/磁盘，走既有 `save_project`（dangerous）或未来第二迭代的显式导出工具（需 dangerous + 用户确认）。

---

### 4.7 `run_flow` 扩展契约

现状：先 `loadProject` 再 `runOnceAsync()` 立即返回 `{"status":"started","async":true}`（`AgentActor.cpp:582-606`）。

**新注册**（保持参数顺序，追加在末尾，兼容旧提示词）

```cpp
registerTool({"run_flow",
              "Run the current flow. wait=true (default) waits for completion and reports success/elapsed/"
              "failed node; wait=false returns immediately.",
              {{"mode", "enum", "Run mode: once or cycle", false, QJsonValue("once"), {"once", "cycle"}},
               {"wait", "boolean", "Wait for completion before returning", false, QJsonValue(true)},
               {"timeoutMs", "integer", "Wait timeout in ms (wait=true), clamped to [1000,300000]", false,
                QJsonValue(60000)}},
              false, false, 60000});   // 第4位 dangerous=false，第5位 readOnly=false，第6位 timeoutMs=60000
```

> 注意：这里刻意用位置聚合（第 4、5、6 位分别为 `dangerous/readOnly/timeoutMs`），与 §3.1 的字段顺序一致；新增工具仍建议走 `makeWriteTool(...)` 工厂。

| 参数 | 类型 | 必填 | 默认 | 说明 |
|---|---|---|---|---|
| `mode` | enum | 否 | `once` | `once` 单次 / `cycle` 连续 |
| `wait` | boolean | 否 | **true** | 是否等待完成 |
| `timeoutMs` | integer | 否 | 60000 | `qBound(1000, timeoutMs, 300000)`；仅 `wait=true` 有效 |

**返回（`wait=true`，`mode=once`）**

```json
{"success": true, "mode": "once", "elapsedMs": 96, "failedModule": "", "errorMessage": "",
 "waitTimedOut": false, "totalRuns": 12, "successRuns": 11}
```

失败：`{"success": false, "mode": "once", "failedModule": "MeasureCircle_1", "errorMessage": "...", "error": "...", "elapsedMs": 96, "code": "run_failed"}`。

> **失败续跑（集成收口 B1）**：任意普通工具失败都将失败结果回灌模型继续诊断和重新规划；失败批次余项跳过，不自动重放。流程失败可能已有外部副作用，新的写操作仍按当前权限重新确认，且不得重置既有轮数、工具数量与全局 deadline。`already_running/reentrant_wait/timeout/cancelled/paused_at_breakpoint`、权限拒绝及工程上下文失效一律结束本轮，不续跑。

超时：`{"success": false, "code": "timeout", "waitTimedOut": true, "error": "Flow did not finish within 60000 ms; a stop was requested"}`（并已调用 `RunEngine::stop()`；不声称同步执行已立即停止）。

断点/重入：等待被断点中断 → `{"success": false, "code": "paused_at_breakpoint", "error": "Flow paused at a breakpoint; continue or stop it from the UI"}`；同线程已在等待（嵌套等待）→ `{"success": false, "code": "reentrant_wait", "error": "Another run wait is already active on this thread"}`。

**返回（`wait=false`）**：保持既有 `{"status":"started","mode":...,"async":true}`（**不得**改动，提示词与既有测试依赖）。

**`mode=cycle` 与 `wait=true`**：连续运行没有"完成"概念 → `wait` 被忽略，返回 `{"status":"started","mode":"cycle","async":true,"note":"cycle mode ignores wait"}`。

**引擎忙**：
- `wait=true, mode=once` 且引擎已在跑 → `{"success": false, "code": "already_running", "error": "Flow is already running"}`；
- `wait=false` 保持既有 `{"error":"Flow is already running"}`（`AgentActor.cpp:602-603`），仅追加 `code:"already_running"`。

**`failedModule` 来源**：`AgentRunWaiter` 在等待期间记录首个 `moduleFinished(name,false,elapsedMs)`（`RunEngine.h:193`）的名字（§5.4）。

---

### 4.8 RunEngine 新增只读快照 / 结构化校验 API（T3 独占）

**必须**新增以下声明与实现（`RunEngine.h:142-156` 区域附近；实现放 `RunEngine.cpp:1830` 前后），**不得**改动既有函数签名与语义：

```cpp
// --- 结构化运行前校验（validateFlow 的派生，保持既有字符串输出不变）---
struct FlowIssue {
    QString code;      // "missing_required_input" | "disabled_output_bypass_conflict"
    QString node;      // 运行名
    QString port;      // 可为空
    QString message;   // 与既有 validateFlow() 完全一致的中文文案
};
QList<FlowIssue> validateFlowIssues() const;
// validateFlow() 改为：map(issues, message).join 的字符串列表，输出必须与基线逐字一致

// --- 只读快照（前置条件：!isBusy()；GUI 线程调用）---
struct ModuleRuntimeInfo {
    bool valid = false;
    bool success = false;
    bool skipped = false;
    int elapsedMs = 0;
    QString errorMessage;
    QDateTime finishedAt;
};
QMap<QString, ModuleRuntimeInfo> moduleRuntimeSnapshot() const;
PortValueMap nodePortOutputs(const QString& moduleName) const;   // m_nodeOutputs.value(name) 的副本
qint64 lastFrameId() const;                                      // m_frameId 快照
```

**实现约束**

1. `validateFlowIssues()` 必须是 `validateFlow()` 的**唯一**实现来源；后者只做格式化，确保 `test_runengine.cpp:2005-2035`、`:2179`、`:2332` 的既有断言（非空 + 含节点名）不回归。
2. `m_moduleRuntimeInfo`：新增成员 `QMap<QString, ModuleRuntimeInfo> m_moduleRuntimeInfo;` + `mutable QMutex m_runtimeInfoMutex;`，写入点集中在模块执行结束处（`RunEngine.cpp:1720-1763` 附近，`executeModule`）与并行批次结果回写处（`RunEngine.cpp:1340-1375` 附近），跳过分支写 `skipped=true`（`:1816` 旁路处）。
3. 快照读取在 `m_runtimeInfoMutex` 下做**深拷贝后返回**；`nodePortOutputs()` 与 `lastFrameId()` 在 `m_lifecycleMutex`（`RunEngine.h:249`，`mutable QRecursiveMutex`）下拷贝。
4. 前置条件显式检查：若 `isBusy()` 为真 → 三个快照 API 返回空/`false` 并 `qWarning()`（防御性；上层工具已在调用前返回 `engine_busy`）。**禁止**在引擎运行中读取，避免与工作线程写 `m_nodeOutputs`（`RunEngine.cpp:1351`、`:1707`）构成数据竞争。
5. 新增 API 不改变 ABI 版本：`RunEngine` 是 `deeplux_core` 内部类，不在插件 ABI（`IModule.h` 的 `DEEPLUX_MODULE_INTERFACE_VERSION`）内，因此**无需** `sync-plugins`。

---

### 4.9 新增注册清单（由 T2 注册，T3 只实现 handler）

**职责裁决**：6 个新工具与 `run_flow` 的 `wait`/`timeoutMs` 参数由 **T2 注册**（`ToolSchema.h/.cpp` 是 T2 独占文件，§8.1）；**T3 只实现 handler**（`AgentActor` 中的 6 个处理函数 + `run_flow` 的 wait 分支），注册时使用 T2 提供的工厂函数（§3.1），**不得修改 `ToolSchema.*`**。§4.1~§4.6 的"注册"代码块是 **T2 依据本节写入 `ToolSchema.cpp` 的内容**，不是 T3 的交付物；T3 以其参数表/返回契约为实现依据。

在 `ToolSchema::registerDefaultTools()`（`ToolSchema.cpp:129-258`）末尾追加 6 条（`run_flow` 就地改，见 §4.7），顺序固定为：
`validate_workflow`、`get_node_runtime_outputs`、`get_execution_logs`、`tune_node_and_verify`、`capture_viewport_image`、`export_node_output`。

> 顺序固定是为了让 `toOpenAIFunctions()` 输出稳定，便于提示词缓存与快照测试。
> T2 注册完成后必须通知 T3（T3 的 handler 与测试以"schema 中已存在该工具"为前提），见 §10 第 1 步。

### 4.10 系统提示词同步要求（T4，`AgentController.cpp:638-657`）

新工具必须被"教会"使用，否则闭环不会被模型走通。`defaultSystemPrompt()` 追加以下规则（英文，与既有提示词风格一致）：

1. `Before running a flow you just modified, call validate_workflow. If ok=false, fix the reported issues instead of running.`
2. `run_flow returns only after the run finishes (wait defaults to true). Do not claim a run succeeded without checking its success/elapsedMs/failedModule fields.`
3. `After a run, use get_node_runtime_outputs to read the actual numbers you need (never guess module outputs).`
4. `When the user asks about image quality/content, call capture_viewport_image or export_node_output and look at the returned image. At most 2 tool images are attached per turn.`
5. `If a tool returns an error with a "code", read the code and adapt; do not repeat the same failing call.`

同时更新 6 个工具的 `description`（§4.1~§4.6 已给出）与 `read_documentation` 提示词中"工具清单"段落（若存在）。**禁止**在提示词里描述工具未实现的参数（visionflow 的 `nodeName` 漂移教训，roadmap §2.3）——提示词与 schema 的一致性本轮**无法自动化**（提示词是自由文本），由 reviewer 逐句比对 §3.2.3/§4.1~§4.7 的参数表；契约测试只覆盖 schema 与 handler 之间的一致性。

---

## 5. 异步执行与取消协议（T4 主责）

### 5.1 排队调用点与不变式

**唯一排队点**：`AgentController::confirmPendingTools()`（`AgentController.cpp:382-397`）。

```cpp
void AgentController::confirmPendingTools() {
    const AgentState currentState = state();
    if (currentState != AgentState::Confirming) {
        qWarning() << "[AgentController] confirmPendingTools called in state" << stateName(currentState);
        return;
    }
    if (m_dispatchQueued)      // 连点保护：必须在清空 pending 之前判断
        return;
    if (m_pendingToolCalls.isEmpty()) {
        transitionTo(AgentState::Idle);
        return;
    }
    QJsonArray calls = m_pendingToolCalls;
    m_pendingToolCalls = QJsonArray();
    m_dispatchQueued = true;
    m_queuedContinueAfterPending = m_continueAfterPendingTools;  // 生产者的意图：true=续跑循环，false=只执行本批
    m_continueAfterPendingTools = true;                          // 复位给下一次交互
    const quint64 turn = m_turnGeneration;
    QMetaObject::invokeMethod(
        this,
        [this, calls, turn] {
            m_dispatchQueued = false;
            if (turn != m_turnGeneration || state() != AgentState::Confirming) {
                qWarning() << "[AgentController] queued dispatch dropped (turn aborted or state changed)";
                if (state() == AgentState::Confirming)
                    transitionTo(AgentState::Idle);
                return;
            }
            m_continueAfterPendingTools = m_queuedContinueAfterPending;  // 供 doConfirmPendingTools 读取
            doConfirmPendingTools(calls);
        },
        Qt::QueuedConnection);
}
```

**不变式**

1. `confirmPendingTools` 签名不变（`AgentController.h:76`；`MainWindow.cpp:1762-1763` 直接取地址连接，改签名会编译失败）。
2. 排队前**必须**先把 `calls` 拷出并清空 `m_pendingToolCalls`（保持既有防重入语义，`AgentController.cpp:393-394`）。
3. 排队后状态**在 `doConfirmPendingTools` 内**才迁到 `Executing`（沿用 `:405`），因此 UI 在"确认点击 → 排队 tick"之间仍看到 `Confirming`（毫秒级）。**必须**同时禁用确认/取消按钮（`AgentChatPanel` 在 `showToolPreview/clearToolPreview` 已有生命周期，T4 在 `toolPreviewConfirmed` 发出后立刻 `clearToolPreview()` 即可）。
4. `doConfirmPendingTools` 保留既有状态校验（`:400-404`）与执行分支。
5. **Autopilot 路径保持同步**（`onLLMResponse` `:369-372` 直接 `extendAgentLoop`）：不排队。理由：既有测试 `testAutopilotStopsAtTurnLimit`（`test_agentcontroller.cpp:552-577`）与 `testAutopilotRejectsTooManyToolCallsInOneTurn`（`:579-601`）依赖同步链；且 Autopilot 没有"按钮连点"入口。此决策写入代码注释，防止后人"统一成排队"。
6. `m_continueAfterPendingTools` 的**所有权在"生产者"**，不由确认入口决定：
   - `onLLMResponse` 需确认时置 `true`（`:363`，确认后要续跑 LLM 循环）；
   - `handleToolCall`（外部 IPC）需确认时置 `false`（`:612`，确认后只执行本批、不续跑）；
   - `confirmPendingTools` 用 `m_queuedContinueAfterPending` 捕获生产者意图，并把 `m_continueAfterPendingTools` 复位为 `true`；排队 lambda 内再回填给 `doConfirmPendingTools` 读取（旧代码 `:396` 的"执行后再置 true"在排队模型下会读到错误值，必须按此改造）；
   - `rejectPendingTools` 继续置 `true`（`:442`，复位给下一次交互）。

### 5.2 状态迁移（含排队/等待/中止）

```text
Idle --sendUserMessage--> Thinking
Thinking --(纯文本)--> Idle
Thinking --(tool_calls, Observer)--> Idle           （只展示不执行）
Thinking --(tool_calls, 需确认)--> Confirming
Thinking --(tool_calls, Autopilot 免确认)--> Executing（同步执行工具）
Confirming --confirmPendingTools(排队 tick)--> Executing
Confirming --rejectPendingTools--> Idle             （同步，不排队）
Executing --(工具批次结束, 继续循环)--> Thinking
Executing --(普通工具失败且预算/取消/工程检查通过)--> Thinking（失败结果回灌，允许诊断和重新规划）
Executing --(运行控制终态/上限/中止/权限拒绝/工程失效)--> Idle
任何非 Idle 状态 --abortCurrentTurn()--> Idle
```

| 迁移 | 触发 | 允许的重入 |
|---|---|---|
| → Thinking | LLM 请求发出（`:259`、`:323`、`:526-530`） | `onLLMResponse` 只在 Thinking 接受（`:331-335`） |
| → Confirming | 需确认（`:361-367`）或外部 IPC（`:609-615`） | 二次 IPC 调用被 busy 拦（`:598`） |
| → Executing | `doConfirmPendingTools`（`:405`）/ Autopilot（`:370`） | 期间 `handleToolCall` 返回 busy |
| → Idle | 纯文本、拒绝、上限、错误、中止 | —— |

### 5.3 `abortCurrentTurn()` 契约

```cpp
// src/core/agent/AgentController.h
public:
    void abortCurrentTurn();                 // "停止"按钮 / 外部 IPC 可选入口
    quint64 turnGeneration() const;          // 测试断言用（可选）

signals:
    void agentTurnAborted();                 // UI 复位状态条 + 提示

private:
    quint64 m_turnGeneration = 0;            // 每次用户消息 +1；中止再 +1
    bool m_dispatchQueued = false;           // §5.1 连点保护
    bool m_queuedContinueAfterPending = true; // §5.1 生产者意图快照
    bool m_visionRetryDone = false;          // 视觉降级重发一次
    bool m_visionDisabledForSession = false; // §6.6
```

**语义（必须按序执行）**

1. 取 `state()`；若已是 `Idle` → `qWarning` + 返回（幂等，不报错）。
2. `++m_turnGeneration`（中止代号；排队 lambda 与循环续跑都用它作废）。
3. `m_pendingToolCalls = QJsonArray(); m_continueAfterPendingTools = true; m_dispatchQueued = false;`
4. `if (m_llmClient) m_llmClient->abortRequest();`（§7.5 新增，非纯虚，默认空实现）。
5. `if (m_actor) m_actor->abortRunningTool();` → 内部：`if (m_runWaiter && m_runWaiter->isActive()) m_runWaiter->abort();` 并 `RunEngine::instance().stop();`
6. `transitionTo(AgentState::Idle);`
7. `emit llmResponseReceived("Stopped by user.", {}); emit agentTurnAborted();`

**中止后的一致性**

- 正在等待的 `run_flow(wait)` → `AgentRunWaiter` 以 `aborted=true` 退出 → 工具返回 `{"success":false,"code":"cancelled"}` → `extendAgentLoop` 在写回历史前检查 `turn != m_turnGeneration` → **丢弃结果、不发下一次 LLM 请求**（状态已是 Idle）。
- 已在 `Executing` 的同步工具无法被打断（例如 `save_project` 正在写盘）；`abortCurrentTurn` 只保证**等待可被打断 + 后续不再续跑**，审计仍记录该工具完成（`durationMs` 正常）。
- 中止后 `m_visionDisabledForSession` **不重置**（会话级记忆）。
- 新用户消息会把 `m_turnGeneration` 再 +1（§5.1 的 `turn` 比较自然失效旧队列）。

### 5.4 `AgentRunWaiter` 契约（新类 `src/core/agent/AgentRunWaiter.{h,cpp}`）

```cpp
class DEEPLUX_API AgentRunWaiter : public QObject {
    Q_OBJECT
public:
    struct Outcome {
        bool finished = false;      // runFinished 到达
        bool timedOut = false;
        bool aborted = false;
        bool reentrantRejected = false;
        bool rejectedStart = false; // start() 返回 false（引擎忙）
        bool pausedAtBreakpoint = false;
        RunResult result;           // finished 时有效
        QString failedModule;       // 等待期间首个 moduleFinished(success=false)
        int moduleFailures = 0;
    };

    explicit AgentRunWaiter(QObject* parent = nullptr);
    ~AgentRunWaiter() override;

    // 启动一次运行并阻塞当前线程的嵌套事件循环，直到 runFinished / 超时 / abort() / 断点命中。
    // 必须先连接再调用 start()，以消除"运行在连接建立前就结束"的丢失信号竞态。
    // start 返回 false 表示引擎拒绝启动（忙/停止中）→ outcome.rejectedStart=true，不做等待。
    // 仅允许 GUI 线程调用；重入或非 GUI 线程 → 立即返回且不改引擎状态。
    Outcome waitForRun(RunEngine& engine, int timeoutMs, const std::function<bool()>& start);

    void abort();                   // 幂等；由 abortCurrentTurn 调用
    bool isActive() const;
    QString lastReason() const;     // "finished" | "timeout" | "aborted" | "reentrant" | "breakpoint" | "rejected"
};
```

**实现要求（逐条验收）**

1. **只连不拥有**：`waitForRun` 先连接 `RunEngine::instance()` 的 `runFinished`（`RunEngine.h:189`）、`moduleFinished`（`:193`）、`breakpointHit`（`:195`），**再**调用 `start()`，最后 `m_loop.exec()`；用 `QMetaObject::Connection` 保存并在返回前 `disconnect`。跨线程（工作线程发信号）自动排队，嵌套 `QEventLoop` 能收到。
2. **超时**：`QTimer::singleShot(clampedTimeout, &m_loop, [this]{ m_timedOut = true; m_loop.quit(); })`；`clampedTimeout = qBound(1000, timeoutMs, 300000)`。定时器以 `m_loop` 为上下文，循环退出后自动失效，不会打到已析构对象。
3. **重入保护**：进程内 `static QSet<const QThread*> s_waitingThreads;`（`QMutex` 保护）——同一线程已在等待则返回 `{reentrantRejected=true}` 并 `qWarning`；非 GUI 线程（`QCoreApplication::instance()->thread() != QThread::currentThread()`）同样拒绝。
4. **断点**：收到 `breakpointHit` → 记录 `pausedAtBreakpoint=true` 并 `quit()`（避免在断点处永久等待）。
5. **正常结束**：`runFinished` → `m_result = result; m_finished = true; quit()`。
6. **失败节点**：`moduleFinished(id,false,ms)` → `if (failedModule.isEmpty()) failedModule = id; ++moduleFailures;`
7. **`abort()`**：置 `m_aborted=true` 并 `quit()`（幂等；未在等待时只置标志，由 `waitForRun` 入口检查并立即返回 `aborted`）。
8. **析构**：若 `isActive()`，先 `quit()` 再断开（栈对象，正常不会命中；防御性）。
9. **不得**在 `waitForRun` 内部调用 `RunEngine::stop()`（职责分离：调用方按 `timedOut/aborted` 决定是否 stop）。
10. **不得**在等待期间修改 Agent 状态（状态由 `AgentController` 拥有）。
11. **`start` 为 false** → 立即返回 `{rejectedStart=true}`，不建立事件循环，不碰线程守卫。

**AgentActor 侧使用（唯一入口，禁止复制等待代码）**

```cpp
AgentActor::RunOutcome AgentActor::runAndWait(const QString& mode, int timeoutMs);  // private
// 1) ensureEngineLoaded(); 2) mode=="cycle" → start() 并立即返回 started；
// 3) once: m_runWaiter->waitForRun(engine, timeoutMs, [&]{ return engine.runOnceAsync(); })
//    返回 rejectedStart → already_running；
// 4) timedOut → RunEngine::stop() 后返回 timeout；aborted → cancelled；
//    pausedAtBreakpoint → paused_at_breakpoint；reentrantRejected → reentrant_wait；
//    finished → success/failed（带 failedModule）。
```

### 5.5 对现有 20 轮 / 单轮 10 calls 上限的影响

**结论：常量与计数逻辑不变**（`AgentController.cpp:24-25`、`:455-465`），只改变"一轮的墙钟时长上界"。

| 维度 | 变更前 | 变更后 | 说明 |
|---|---|---|---|
| 轮次上限 | 20（每次用户消息重置） | **不变** | `m_agentTurnCount` 每批 +1（`:465`），排队不额外计轮 |
| 单轮 calls | 10 | **不变** | 排队不拆分批次；超限仍在 `:455-459` 拒绝 |
| 单轮时长 | 同步工具，毫秒级 | **上界 = Σ 该批等待类工具的超时** | `run_flow.wait` 默认 60s / `tune_node_and_verify` 默认 60s、上限 300s |
| 极端上界 | 秒级 | 20 轮 × (2×300s) ≈ 3.3 小时（理论） | 缓解：默认超时 60s；`abortCurrentTurn` 随时中止；审计记录 `durationMs` 便于发现异常 |

**实现侧的附加约束（必须）：**

1. 单批内等待类工具（`run_flow` with `wait=true`、`tune_node_and_verify` with `runMode=flow`）**建议**由提示词引导为单发；代码层面不强制拆分（拆分会让 tool_call 配对复杂化）。
2. 每次 `waitForRun` 返回后，`AgentController` 必须重新读取 `state()`：若已非 `Executing`（被中止）→ 丢弃结果不续跑（§5.3）。
3. `m_agentTurnCount` 的自增仍在**执行前**（`:465`），保证"工具抛异常也不会把轮次算漏"。

### 5.6 UI 停止按钮（T4）

- `AgentChatPanel` 新增 `QToolButton* m_stopButton`（与发送按钮同区）、`signal stopRequested()`；
  `setThinking(true)` 时显示（`AgentChatPanel.h:57`），收到 `llmResponseReceived`/`llmErrorOccurred`/`agentTurnAborted` 时隐藏。
- `MainWindow` 接线（`MainWindow.cpp:1683-1706` 区域旁）：
  ```cpp
  connect(m_agentChatPanel, &AgentChatPanel::stopRequested, &AgentController::instance(),
          &AgentController::abortCurrentTurn);
  connect(&AgentController::instance(), &AgentController::agentTurnAborted, this, [this, setAgentTabStatus]() {
      setAgentTabStatus(tr("已停止"), false);
      m_agentChatPanel->setThinking(false);
  });
  ```
- 状态文案：`Agent 正在思考` / `等待确认` / `执行中（可停止）`。

### 5.7 重入与事件风暴防护清单（reviewer 逐条核对）

1. `confirmPendingTools` 的 `m_dispatchQueued` 连点保护（§5.1）。
2. 排队 lambda 内的 `turn` 与 `state()` 双校验。
3. `doConfirmPendingTools` 既有的 `state()!=Confirming` 早退（`:400-404`）。
4. `AgentRunWaiter` 的线程内重入拒绝（§5.4 第 3 条）。
5. 等待期间 `handleToolCall` 返回 busy（`:598`）——外部 IPC 不能插队。
6. 等待期间 `sendUserMessage` 被 busy 拦（`:246-249`）。
7. 工具执行期间引擎 `isBusy()` → 变更类工具返回 `engine_busy`（`:260-267` + §3.4）。
8. `abortCurrentTurn` 幂等（Idle 下 no-op）。
9. 断点暂停不会让等待无限挂起（§5.4 第 4 条）。
10. 等待结束必须 `disconnect` + 停表；`AgentRunWaiter` 不可跨批复用（每批新建或显式 reset）。

---

## 6. 图像回灌契约（T4 主责）

### 6.1 结果字段（唯一入口）

- 工具结果**顶层**字段 `imagePath`（string，绝对路径）被 `AgentController` 收集。
- 保留字段（本轮**不消费**，仅为第二迭代预留，工具**不得**输出）：`images`（数组 `[{path,mimeType}]`）。
- 只有 `capture_viewport_image` 与 `export_node_output` 产出 `imagePath`；其它工具即使返回同名字段也会被忽略（白名单：`kImageProducingTools`）。

### 6.2 临时目录与命名规则

```cpp
// src/core/agent/AgentToolImage.h（T3 提供，T4 复用；放 core/agent 下）
namespace AgentToolImage {
QString imageDir();                                   // QDir::tempPath() + "/deeplux-agent"
QString writeImage(const QImage& image, const QString& toolName);   // 成功返回绝对路径，失败返回空
int cleanupOlderThanDays(int days);                   // 启动时 best-effort 清理
}
```

- 目录：`QDir::tempPath() + "/deeplux-agent"`（与既有 `QDir::tempPath()` 用法一致：`src/plugins/detection/SamPredict/SamPredictPlugin.cpp:101`、`src/ui/dialogs/SamAnnotatorDialog.cpp:1648`），`QDir().mkpath()` 确保存在。
- 文件名：`agent_<tool>_<yyyyMMdd-HHmmss-zzz>_<seq>.<ext>`
  - `<tool>` 只保留 `[a-z0-9_]`（小写化，其它字符替换为 `_`）；
  - `<seq>` = `static std::atomic<int>` 单调自增（同毫秒多次调用不撞名）；
  - `ext` = `png`（默认）或 `jpg`（`format=jpg`）。
  - 例：`agent_capture_viewport_image_20261026-223001-123_7.png`。
- **禁止**接受调用方路径；写出后再校验 `QFileInfo(path).canonicalFilePath().startsWith(QFileInfo(imageDir()).canonicalFilePath())`（防御性，防未来误改）。
- 时间戳用本地时间（用户可见、无需时区解释）；同目录内按文件名排序即时间序。

### 6.3 尺寸 / 数量 / 体积上限（常量集中在 `AgentController.cpp` 匿名命名空间）

| 常量 | 值 | 语义 |
|---|---|---|
| `kMaxToolImagesPerTurn` | **2** | 每轮最多回灌 2 张（按 tool_calls 顺序取前 2 个不同路径） |
| `kMaxToolImageLongEdge` | **1280** | 回灌副本长边上限（`Qt::KeepAspectRatio` + `Qt::SmoothTransformation`）；**不改磁盘原件** |
| `kMaxToolImageBytes` | **4 MB** | 单张编码后上限；超限先降 PNG→JPEG(q=85)，仍超 → 丢弃该图并写文字说明 |
| `kMaxToolImageTotalBytes` | **8 MB** | 单轮图片总量上限（与既有 `kMaxImageAttachmentBytes`「`AgentController.cpp:27`」同一预算，**不再新增第二个 8MB 常量**，直接复用） |

处理算法（必须按序）：

```text
for each result in batchResults (工具调用顺序):
    path = result.imagePath; if empty or not in whitelist → skip
    if path already attached this turn or attached count == 2 → skip
    img = QImage(path); if null → skip + 记 warning
    scaled = img.scaled(longEdge<=1280, KeepAspectRatio, SmoothTransformation)
    bytes = encode(scaled, PNG)
    if bytes > 4MB: bytes = encode(scaled, JPEG, 85)
    if bytes > 4MB: skip + 追加文字说明（"image too large to attach"）
    if total + bytes > 8MB: skip + 文字说明
    attach
```

### 6.4 回灌消息构造

在 `extendAgentLoop` 写完全部 `role=tool` 消息、且**通过 `partial_failed` 早退检查之后**（即 `AgentController.cpp:510-514` 之后、`:527-530` 构造下一次请求之前）调用：

```cpp
void AgentController::attachToolImages(const QJsonArray& results);   // private
```

> **顺序很关键**：`partial_failed` 会直接回 `Idle` 且不再发请求（`:510-514`）。若在其之前插入带图 `user` 消息，会留下一条与任何 assistant 请求都不配对的孤儿消息，污染下一轮上下文。因此 `attachToolImages` 必须放在早退检查之后、`extendAgentLoop` 循环续跑（`emit agentLoopIterating()`，`:520`）之前。

- 有图且视觉可用：
  ```cpp
  AgentMessage m;
  m.role = "user";
  m.content = "Images produced by the tool calls above (paths kept for reference):\n- <path> (<w>x<h>)";
  m.images.append({pngOrJpegData, mimeType, path});   // description = 磁盘路径
  m_conversationHistory.append(m);
  ```
  `AgentImageAttachment.description`（`ILLMClient.h:19-23`）填路径，非视觉链路可用它兜底。
- 有图但视觉不可用：只插文字（§6.5），不插 `images`。
- 无图：不插任何消息（不产生空 user 消息，避免破坏 tool_calls 配对）。

**顺序约束**：带图 `user` 消息必须紧跟在该批 `role=tool` 消息之后——OpenAI 兼容网关对 `tool` 消息后接 `user` 是合法的；**不得**把图片塞进 `role=tool` 消息（当前序列化不支持，`ILLMClient.cpp:19-39` 只对任意 role 加 content 数组，但 tool 消息加图会被多数网关拒绝）。

### 6.5 视觉关闭 / 非视觉模型的降级文本

- 判定：`!m_llmClient->supportsImageInput()`（`ILLMClient.h:81-83`）或 `m_visionDisabledForSession` 为真。
- 插入的**唯一**文本（英文，模型侧；路径必须保留）：

  ```text
  Tool produced an image, but the configured model does not support image input.
  Image path: <absolute path> (<w>x<h>). If the user needs visual inspection, ask them to enable
  image input in Agent settings or switch to a vision-capable model.
  ```

- 同时 `emit llmErrorOccurred` **禁止**（这是正常降级，不是错误）；改用一次 `Logger::instance().info(..., "Agent")`。
- UI 提示（可选，T4）：状态条显示"当前模型不支持图片，已改为文字说明"。

### 6.6 视觉能力判定与自动降级

优先级（高 → 低），实现位置明确如下：

1. **用户显式配置** `agent/visionEnabled`：当 `ConfigManager::instance().containsGroupKey("agent", "visionEnabled")` 为真（`ConfigManager.h:38`）→ 直接用它（`MainWindow.cpp:6118` 注入；`AgentSettingsDialog` 保存时写入，`AgentSettingsDialog.cpp:217`）。用户设置**永远**优先，不做启发式覆盖。
2. **模型名启发式**：仅当配置键**不存在**（首次运行、用户从未保存过设置）时使用。实现为 `OpenAILLMClient::heuristicSupportsVision(const QString& model)`（T2，`static`，纯函数便于单测）：
   - 命中 `/gpt-4o|gpt-4\.1|claude-3|claude-4|gemini|qwen-vl|qwen2\.5-vl|glm-4v|internvl|llava|pixtral/i` → `true`；
   - 命中 `/deepseek-(chat|reasoner)|o1-mini|text-davinci/i` → `false`；
   - 其它 → `true`（保守开放，真正不支持时由 §6.6 自动降级兜底）。
   `MainWindow::loadAgentSettings()`（`MainWindow.cpp:6096-6128`）在 `containsGroupKey` 为假时调用它，并把结果 `setVisionEnabled(...)`；**不得**把启发式结果写回配置（避免把猜测变成用户设置）。
3. 无配置、无启发式命中 → 默认 `true`。

自动降级（一次/会话）：

- 触发条件：HTTP 400/422 且响应体命中 `/image_url|content.*type|vision|multimodal|does not support image/i`（§7.2 的 `BadRequest` 子类）。
- 动作：`OpenAILLMClient` 置 `m_visionDowngraded = true` → `supportsImageInput()` 变 false；`emit LLM error (BadRequest, visionDowngradeHint=true)`；
  `AgentController::onLLMErrorClassified` 收到 hint 且 `!m_visionRetryDone` → 置 `m_visionRetryDone = true`、`m_visionDisabledForSession = true`，**用同一请求去掉最近一条带图 user 消息的 `images` 后重发一次**。
- 再失败（第二次）→ 正常报错回 Idle，`m_visionRetryDone=true` 防止循环。
- `m_visionRetryDone` 随每次用户消息重置（`:237-260`、`:262-324` 的入口处）。

### 6.7 与历史裁剪 / 字节预算的关系

- `trimHistoryIfNeeded()` 已按 `(data.size()+2)/3` 估算图片 token（`AgentController.cpp:546-547`），回灌图同样计入，**无需改公式**。
- 回灌产生的 `role=user` 消息会改变"按 user turn 裁剪"的边界（`trimHistoryIfNeeded` 的 `dropOldestTurn` 以 user 消息为切点，`:558-570`）：带图 user 消息与工具结果同属一个 turn，被整组丢弃是正确行为，**不得**为此改动裁剪逻辑（第二迭代再优化）。
- 64 条硬上限（`:577-589`）可能把带图消息裁掉：接受（图片有磁盘路径，模型可再次 `capture_viewport_image`）。

### 6.8 清理策略

- `AgentController::initialize()`（`AgentController.cpp:79-102`）末尾调用 `AgentToolImage::cleanupOlderThanDays(7)`（best-effort，失败仅 `qDebug`）。
- `clearConversation()` / `shutdown()` **不删除**图片（用户可能还要看）。
- 目录内文件总量不做硬限制（第二迭代配配额）。

---

## 7. LLM 错误分类与重试（T2 主责）

### 7.1 枚举与结构

```cpp
// src/core/agent/ILLMClient.h
enum class LLMErrorCategory { None, Auth, RateLimit, Server, Network, BadRequest, Timeout, Malformed };

struct LLMErrorInfo {
    LLMErrorCategory category = LLMErrorCategory::None;
    int httpStatus = 0;          // 0 = 无 HTTP 状态（网络层）
    QString message;             // 面向用户的英文短句
    QString detail;              // 原始错误体/错误串（审计用，截断 2KB）
    bool retryable = false;
    bool contextLimitSuspected = false;   // BadRequest 且疑似上下文超限
    bool visionDowngradeHint = false;     // BadRequest 且疑似不支持图片
    int attempts = 0;            // 已尝试次数（含首次）
};

static QString llmErrorCategoryName(LLMErrorCategory c);   // 声明在 ILLMClient.h，实现在 ILLMClient.cpp
```

`AgentResponse` 增加：`LLMErrorCategory errorCategory = LLMErrorCategory::None;`（roadmap §5.5 要求），成功响应该字段为 `None`。

`ILLMClient` 接口变更（**全部向后兼容**）：

```cpp
// 新增（非纯虚，默认空实现；既有 Mock/Scripted 测试类无需改动即可编译）
virtual void abortRequest() {}

// 新增信号（携带分类信息；既有 errorOccurred(QString) 保留，仅在"最终失败"时发出）
signals:
    void errorOccurredClassified(const LLMErrorInfo& info);
    void retryScheduled(const LLMErrorInfo& info, int attempt, int delayMs);
```

`Q_DECLARE_METATYPE(DeepLux::LLMErrorInfo)` + `qRegisterMetaType<LLMErrorInfo>("LLMErrorInfo")`（`AgentController::initialize()` 内，与 `:82-85` 的注册并列）。

### 7.2 分类映射（唯一实现：`OpenAILLMClient::classifyError`）

| 类别 | 触发条件（`QNetworkReply::attribute(QNetworkRequest::HttpStatusCodeAttribute)` / `networkError()` / body） | 用户提示（`message`） |
|---|---|---|
| `Auth` | 401 / 403 | `"Authentication failed (401/403). Check the API key and its permissions."` |
| `RateLimit` | 429 | `"Rate limit or quota exceeded (429). Retrying…"` |
| `Server` | 500 / 502 / 503 / 504，或其它 5xx | `"The model service returned a server error (<code>). Retrying…"` |
| `Network` | `networkError ∈ {ConnectionRefusedError, HostNotFoundError, RemoteHostClosedError, SslHandshakeFailedError, TemporaryNetworkFailureError, UnknownNetworkError(无状态码)}` | `"Network error while contacting the model service. Check connectivity/proxy."` |
| `Timeout` | `networkError == TimeoutError`、`OperationCanceledError`（且非用户 abort），或 `errorString` 含 `"timed out"` | `"The model service did not respond in time."` |
| `BadRequest` | 400 / 422 / 413 | `"The request was rejected (<code>). "` + 子提示（见下） |
| `Malformed` | HTTP 200 但：JSON 解析失败、无 `choices`、`message` 缺失（对应 `OpenAILLMClient.cpp:141-158`）、空响应体（`:132-135`） | `"The model service returned an invalid response. This is usually a temporary gateway issue."` |

`BadRequest` 子判定：

- `contextLimitSuspected = true`：body 命中 `/context[ _-]?length|too many tokens|maximum context|reduce the length|token limit/i` → `message` 追加 `"The conversation may exceed the model's context window; clear the conversation or shorten the request."`
- `visionDowngradeHint = true`：body 命中 `/image_url|content.*type|vision|multimodal|does not support image/i`（§6.6）。

`Malformed`/`Auth`/`BadRequest` 一律 `retryable=false`；`RateLimit`/`Server`/`Network`/`Timeout` 按 §7.3 设置。

### 7.3 重试矩阵与退避（`OpenAILLMClient` 内部实现）

| 类别 | 最大重试次数（首次之外） | 退避序列 | 总尝试上限 |
|---|---|---|---|
| `RateLimit` | 3 | 500 / 1000 / 2000 ms | 4 |
| `Server` | 3 | 1000 / 2000 / 4000 ms | 4 |
| `Network` | 2 | 1000 / 3000 ms | 3 |
| `Timeout` | 1 | 2000 ms | 2 |
| `Auth` / `BadRequest` / `Malformed` / `None` | 0 | —— | 1 |

实现要求：

1. `sendRequest()` 记录 `m_lastBody`（`QByteArray`）与 `m_lastTools`（`QList<ToolDefinition>`），并 `m_retryAttempt = 0`、`m_retryCategory = None`、`m_aborted = false`（重置）。
2. 失败分类后：`retryable && m_retryAttempt < max` → `++m_retryAttempt`；`QTimer::singleShot(delay, this, [this]{ if (!m_aborted) repost(); })`；`emit retryScheduled(info, m_retryAttempt, delay)`；**不发** `errorOccurred`。
3. 不可重试或次数用尽 → `emit errorOccurredClassified(info)` + `emit errorOccurred(info.message)`（既有链路保持可用，`AgentController.cpp:134` 的槽仍工作）。
4. 退避期间收到新的 `sendRequest()` → 取消待触发定时器（用 `QTimer` 成员而非 `singleShot` 静态版本，便于 `stop()`）。
5. `abortRequest()`：`m_aborted = true; m_retryTimer->stop(); if (m_currentReply) { disconnect; abort(); deleteLater; m_currentReply=nullptr; }`；`onReplyFinished`/`onNetworkError` **首行必须** `if (m_aborted) return;`，保证主动中止不会被分类成 `Timeout` 或触发重试（否则"停止"按钮会弹出网络错误）。
6. 每次 `sendRequest` 把 `m_aborted=false`。
7. `QNetworkRequest::setTransferTimeout(30000)`（`:113`）保持；`Timeout` 类别因此可得。
8. 重试**不重发**已消费的 `AgentConversation` 之外的任何状态；`AgentController` 无感知（不重复计轮）。

> 退避期间 UI 必须可停止：`retryScheduled` → `MainWindow` 状态条显示 `"模型服务限流，1s 后重试（第 2/4 次）"`。

### 7.4 哪些错误回灌给模型，哪些只上报

| 来源 | 是否回灌模型 | 说明 |
|---|---|---|
| LLM 传输/协议错误（Auth/RateLimit/Server/Network/Timeout/BadRequest/Malformed） | **否** | 通过 `errorOccurred` → `AgentController::onLLMError` → `Idle` + `llmErrorOccurred`（`:632-636`）→ UI/审计；模型侧对话就此终止，下一条用户消息开始时模型仍能看到历史（失败请求从未进入历史） |
| 工具执行错误 | **是** | 已经是 `role=tool` 的 `{"error":...}`（`AgentController.cpp:502-507`），模型可自我纠正 |
| 工具被 Observer 拒绝 / 待确认被用户拒绝 | 是（既有） | 拒绝路径写 `Tool execution cancelled by user.`（`:444`）；Observer 路径只展示工具调用（`:346-350`） |
| 视觉不支持（BadRequest 子类） | **否**，但自动降级重发一次 | §6.6；重发不是"回灌错误" |
| 轮数/单轮上限 | 否 | `llmErrorOccurred`（`:457`、`:462`） |

审计：`AgentActionLogEntry.errorCategory` 记录类别名（T4 在 `MainWindow` 的错误分支填，`:1715-1729`）。

### 7.5 测试 `tests/test_agentretry.cpp`（T2 创建）

用本地 `QTcpServer` 假 HTTP 服务（参照 `tests/test_agent_sam_tools.cpp` 的既有 stub 写法）：

1. `testRateLimitRetriesThenSucceeds`：429 → 200；断言 `retryScheduled` 1 次、`responseReceived` 成功、`errorOccurred` 0 次、请求计数 2。
2. `testServerErrorsExhaustRetries`：连续 500；断言 `retryScheduled` 3 次、`errorOccurred` 1 次、`errorOccurredClassified.category == Server`、请求计数 4。
3. `testAuthNotRetried`：401；断言 0 次 retry、`category == Auth`、message 含 `"API key"`。
4. `testBadRequestContextHint`：400 + body `{"error":{"message":"maximum context length exceeded"}}`；断言 0 retry、`contextLimitSuspected == true`。
5. `testMalformedResponseNotRetried`：200 + `not-json`；断言 0 retry、`category == Malformed`。
6. `testAbortCancelsPendingRetry`：429 后立即 `abortRequest()`；断言后续无第二次请求、无 `errorOccurred`。
7. `testClassificationNote`: 单位测试 `llmErrorCategoryName()` 全枚举往返。
8. `testVisionDowngradeHint`：400 + `{"error":{"message":"Invalid content type for image_url"}}`；断言 `visionDowngradeHint == true`、`supportsImageInput()` 变 false。

（`AgentController` 侧的"去图重发一次"断言放 `test_agentclosedloop.cpp`，见 §9.5。）

---

## 8. 文件所有权与并行写冲突

### 8.1 所有权表（本迭代唯一有效版本，覆盖 roadmap §5.6）

| 任务 | 独占文件（只有该任务可写） |
|---|---|
| **T2 契约 + 重试** | `src/core/agent/ToolSchema.h`、`src/core/agent/ToolSchema.cpp`（**含 6 个新工具与 `run_flow` 的 `wait`/`timeoutMs` 参数注册**，§4.9；T3 不得改这两个文件）、`src/core/agent/ILLMClient.h`、`src/core/agent/OpenAILLMClient.h`、`src/core/agent/OpenAILLMClient.cpp`、`src/core/agent/GuiEvent.h`、`src/core/agent/AgentController.h`（**仅** §3.3 表 1-7 行涉及的权限/审计辅助段 + `toolRequiresConfirmation` 声明）、`src/core/agent/AgentController.cpp`（**仅** §3.3 表 1-7 行 + `onLLMError`/分类槽）、`tests/test_agenttoolcontract.cpp`、`tests/test_agentretry.cpp`、`tests/test_agentpermissions.cpp`、`tests/CMakeLists.txt`（第一批） |
| **T3 工具** | `src/core/agent/AgentActor.h`、`src/core/agent/AgentActor.cpp`、`src/core/agent/AgentRunWaiter.h`、`src/core/agent/AgentRunWaiter.cpp`、`src/core/agent/AgentToolImage.h`、`src/core/agent/AgentToolImage.cpp`、`src/core/engine/RunEngine.h`、`src/core/engine/RunEngine.cpp`、**`src/core/CMakeLists.txt`（仅注册新增 agent 源文件：`AgentRunWaiter`/`AgentToolImage` 的 `.h`/`.cpp` 四行，见 §8.2 第 8 条；其余内容禁止改）**、`tests/test_agentactor.cpp` |
| **T4 异步 + 图像 + UI** | `src/core/agent/AgentController.h`、`src/core/agent/AgentController.cpp`（T2 完成后接手全文件）、`src/ui/widgets/AgentChatPanel.h`、`src/ui/widgets/AgentChatPanel.cpp`、`src/ui/widgets/AgentActionLogWidget.h`、`src/ui/widgets/AgentActionLogWidget.cpp`、`src/ui/views/MainWindow.cpp`、`src/ui/views/MainWindow.h`、`tests/test_agentclosedloop.cpp`、`tests/test_agentcontroller.cpp`、`tests/CMakeLists.txt`（第二批） |

**只读（任何实现任务都不得修改）**：`src/core/engine/RunEngine.h` 的**既有**声明（仅允许 §4.8 的追加，见 §8.2 第 4/7 条）、`src/core/model/**`、`src/plugins/**`、`docs/**`（文档变更只由 architect 提交）、`CMakeLists.txt`（根）。

### 8.2 同文件串行规则（必须遵守，否则冲突）

1. **`AgentController.h/.cpp` 两阶段**：
   - 阶段 1（T2）：只改 §3.3 表列出的 7 处 + 新增 `toolRequiresConfirmation` + 错误分类槽 `onLLMErrorClassified`。
   - 阶段 2（T4）：在 T2 报告 `completed` 之后接手，改排队/中止/回灌/图像降级。
   - T4 开工前**必须** `git log --oneline -1` 与 `grep -n "toolAccess\|requiresConfirmation" src/core/agent/AgentController.cpp` 确认 T2 已落地。
2. **`tests/CMakeLists.txt` 串行**：T2 先加两行（`test_agenttoolcontract.cpp`、`test_agentretry.cpp`），T4 再加 `test_agentclosedloop.cpp`；T3 **不得**改这个文件（它把新用例挂在 `test_agentactor.cpp` 里）。
3. **`ToolSchema.{h,cpp}` 单写者 = T2**：6 个新工具与 `run_flow` 的 `wait`/`timeoutMs` 参数由 T2 注册（§4.9、§8.1）；T3 只实现 handler，**不得**修改 `ToolSchema.*`；T2 注册完成并通知 T3 后，T3 才能开工（§10 第 1 步）。
4. **`RunEngine.{h,cpp}` 单写者 = T3**：只允许 §4.8 的**新增**；改动若涉及既有函数行为（例如 `validateFlow` 重构），必须保留逐字相同的字符串输出，并在 PR 描述里贴 `test_runengine` 通过证据。
5. **`MainWindow.cpp` 单写者**：只有 T4。T3 需要的视口注入是 `AgentActor::setViewportImageProvider` 调用，由 T4 在 `MainWindow` 落地；T3 只提供接口与测试注入。
6. **跨任务契约变更**：任何"需要改对方文件"的诉求，先发消息给 architect，由 architect 更新本文并指派，**不得**自行越界修改。
7. **T3 的 `RunEngine` 追加已获队长授权**：本次由队长在 t3 契约修订记录中授权 T3 追加 `RunEngine.{h,cpp}`（§4.8 的只读快照/结构化校验 API）。reviewer 按 §4.8 的口径审查：只允许新增声明/成员与 `validateFlow` 的等价重构（字符串输出逐字不变），既有公开签名与语义（`isBusy/runOnceAsync/executeParallel/moduleOutput…`）不得改动；PR 必须附 `ctest -R test_runengine` 全绿证据。
8. **`src/core/CMakeLists.txt` 对 T3 的有限授权**：依据**队长在 t3 契约修订记录第 2 条**的授权，T3 只允许在该文件的 `CORE_SOURCES`（`src/core/CMakeLists.txt:11`）与 `CORE_HEADERS`（`:62`）列表中各加两行，注册本迭代新增的 agent 源文件——`agent/AgentRunWaiter.cpp`、`agent/AgentToolImage.cpp` 与 `agent/AgentRunWaiter.h`、`agent/AgentToolImage.h`（当前工作树已含这四行，位置 `:51-52`、`:98-99`）。**禁止**改动该文件的任何其它内容（target、依赖库、编译选项、安装规则等）；reviewer 按"diff 只含这四行"审查，T2/T4 不得修改该文件。新 `.cpp` 未注册会直接导致链接缺符号，属 t3 的必修项，不是可选范围。

### 8.3 禁改清单（避免误伤）

- `AgentController.cpp:24-25` 的两个上限常量（本迭代不改）。
- `AgentActor.cpp:582-606` 的 `run_flow` 旧返回值分支（`wait=false` 路径必须逐字保留）。
- `test_agentpermissions.cpp:30-39`、`:41-55`、`:57-69`、`:80-110` 的既有断言（只允许**追加**新 test 槽）。
- `ILLMClient.h` 既有纯虚函数签名（只允许**追加**非纯虚与信号）。
- 插件 ABI（`src/core/interface/IModule.h` 的 `DEEPLUX_MODULE_INTERFACE_VERSION`）——本迭代不碰，因此**无需** `sync-plugins`。

---

## 9. 测试清单（每个新工具的断言点）

### 9.1 新增/修改测试文件

| 文件 | 归属 | 说明 |
|---|---|---|
| `tests/test_agenttoolcontract.cpp`（新增） | T2 | §3.5 的 11 条断言 |
| `tests/test_agentretry.cpp`（新增） | T2 | §7.5 的 8 条断言 |
| `tests/test_agentactor.cpp`（追加 test 槽） | T3 | 6 个工具的行为断言（§9.3） |
| `tests/test_agentclosedloop.cpp`（新增） | T4 | 端到端闭环 + 排队 + 中止 + 回灌（§9.5） |
| `tests/test_agentcontroller.cpp`（追加/微调） | T4 | 排队执行、abort、视觉降级重发（§9.5 第 10~13 条 + §9.6） |
| `tests/test_agentpermissions.cpp`（追加） | T2 | 新工具 × 3 级权限（§9.4） |

### 9.2 `test_agenttoolcontract` 断言点

见 §3.5（11 条，逐条一个 `testXxx`）。

### 9.3 `test_agentactor` 新增断言点（逐工具）

> 复用既有夹具：临时 appdata + 安装 `GrabImage/MeasureCircle/MeasurementInput/FitCircle` 真实插件（`tests/CMakeLists.txt:303-313`），以及 `BlockingAgentModule`（`test_agentactor.cpp:22-49`）模拟慢节点。

**`validate_workflow`**

1. 无工程 → `code == "no_project"`。
2. `create_project` 后空流程 → `ok == false`、`issues[0].code == "no_modules"`、`moduleCount == 0`。
3. 建 `GrabImage` 单节点（无必需输入）→ `ok == true`、`issues.isEmpty()`。
4. 加一个必需输入端口未连接的模块 → `ok == false`、`issues` 含 `code=="missing_required_input"`、`node` 为该节点运行名、`port` 非空。
5. 引擎忙（用 BlockingAgentModule 起一次运行）→ `code == "engine_busy"`。

**`get_node_runtime_outputs`**

6. 未知节点 → `code == "node_not_found"`。
7. 从未运行 → `lastStatus == "unknown"`、`outputs` 为空对象、含 `note`。
8. 跑一次后 → `lastStatus == "success"`、`elapsedMs >= 0`、`outputs.image.type == "image"` 且 `width/height > 0`。
9. 失败节点 → `lastStatus == "failed"`、`errorMessage` 非空。
10. 端口值序列化：列表型/图片型/不支持型分别落到 `QVariantList`/`image`/`unsupportedPorts`（用两个输出端口的模块构造）。
11. 用实例 id 传 `nodeName` 也能解析（与名称等价）。

**`get_execution_logs`**

12. `category="Run"` 只返回 `LogEntry.category=="Run"`。
13. `level="error"` 只返回 Error/Success 级别（按枚举序）。
14. `keyword` 大小写不敏感匹配。
15. `limit=2` 且匹配 5 条 → `entries.size()==2`、`truncated==true`、`total==5`、且返回的是**最新**两条（时间升序）。
16. 空匹配 → `entries==[]`、`truncated==false`。

**`tune_node_and_verify`**

17. 非法参数键 → `success==false`、`code=="invalid_parameter"`、`parameter=="<key>"`，且工程参数**未被修改**（再 `get_flow_state` 对比）。
18. 正常改 2 个键 + `runMode=flow` → `success==true`、`before/after` 值不同、`applied==2`、`elapsedMs>=0`、`outputs` 非空。
19. 一次撤销回滚整组：`actor()->undoStack()->undo()` 后两个键都回到 `before`。
20. `runMode=node` → `executed==false`、不触发运行（`RunEngine::totalRuns()` 不变）、`note` 存在。
21. 慢节点 + `timeoutMs=1000` → `code=="timeout"`，随后 `QTRY_VERIFY(!RunEngine::instance().isBusy())`。
22. 引擎忙 → `code=="engine_busy"`。

**`capture_viewport_image`**

23. 未注入 provider 且无 lastOutput → `code=="no_image"`。
24. 注入 provider 返回 100×80 图 → `imagePath` 文件存在、`width==100`、`height==80`、路径前缀 == `QDir::tempPath()+"/deeplux-agent"`、`source=="viewport"`。
25. 无 provider 但有 `lastOutput` → `source=="last_output"`。
26. 连续两次调用 → 两个不同 `imagePath`（seq 递增）。

**`export_node_output`**

27. 未知节点 → `node_not_found`。
28. 节点无图像输出 → `code=="no_image_output"`。
29. 跑一次后 `format=png` → 文件存在且文件名以 `.png` 结尾、`width/height` 与节点输出一致。
30. `format=jpg` → 后缀 `.jpg`。
31. 引擎忙 → `engine_busy`。

**`run_flow` 扩展**

32. 默认 `wait=true`：返回含 `success/elapsedMs/failedModule`，且 `RunEngine::isBusy()==false`。
33. `wait=false`：返回 `status=="started"` 且 `async==true`（旧语义逐字保持）。
34. `mode=cycle` + `wait=true`：返回 `status=="started"` 且含 `note`。
35. 失败节点 → `success==false`、`failedModule` 为该节点名、`code=="run_failed"`。
36. 慢节点 + `timeoutMs=1000` → `code=="timeout"` 且已 `stop()`。

### 9.4 `test_agentpermissions` 追加断言点

37. `validate_workflow` / `get_node_runtime_outputs` / `get_execution_logs` / `capture_viewport_image` 在 **Observer** 下**直接执行**（无 `pending_confirmation`）。
38. `tune_node_and_verify` 在 **Advisor** 下 → `status=="pending_confirmation"`；**Autopilot** 下 → 直接执行（不待确认，但会遇到 `no_project`/`node_not_found` 等业务错误，断言"不是 pending"）。
39. 既有 4 个用例（`test_agentpermissions.cpp:30-69`）保持逐字通过。
40. `sam_predict/sam_annotate` 既有断言（`:80-110`）保持通过。

### 9.5 `test_agentclosedloop`（T4）断言点

端到端脚本（与 roadmap §8.2 第 2 条对齐）：

1. `create_project` → `add_module(GrabImage)` → `add_module(MeasureCircle)` → `connect_modules` → `validate_workflow`（`ok==true`）。
2. `run_flow(wait=true)` → `success==true`。
3. `get_node_runtime_outputs(MeasureCircle)` → `lastStatus=="success"`、`outputs` 非空。
4. `tune_node_and_verify(flow)` → `after` 生效、`success==true`。
5. `capture_viewport_image` → 返回 `imagePath`；断言 `AgentController` 的历史里出现一条 `role=="user"` 且 `images.size()==1` 的消息，其 `description == imagePath`。
6. **视觉关闭**：把 mock 的 `supportsImageInput()` 设为 false → 回灌消息为纯文本、`images.isEmpty()`、文本含 `"does not support image input"` 与路径。
7. **数量上限**：一次批量返回 3 个工具结果各带 `imagePath` → 历史里只出现 2 张图。
8. **体积上限**：造一张 6000×4000 图 → 回灌副本长边 ≤1280（断言 `images[0]` 解码后 `width<=1280 && height<=1280`）。
9. **超时**：`run_flow(wait=true, timeoutMs=1000)` 对慢节点 → `code=="timeout"`，`QTRY_VERIFY(!isBusy())`。
10. **中止**：慢节点运行中调 `abortCurrentTurn()` → 工具结果 `code=="cancelled"`、状态 `Idle`、无后续 LLM 请求（mock 的 `sendRequest` 计数不再增加）。
11. **重入**：等待期间调 `handleToolCall("get_flow_state", {})` → `error` 含 `"busy"`。
12. **排队**：`confirmPendingTools()` 后**立即**断言状态仍为 `Confirming` 或已 `Executing`（不得为 `Idle`），`QTRY_COMPARE(state(), Thinking)` 最终收敛（若工具批次会续跑）。
13. **视觉降级重发一次**：mock 先抛 `LLMErrorInfo{BadRequest, visionDowngradeHint=true}` → `AgentController` 去掉图片重发一次（断言 `sendRequest` 调用 2 次，第二次历史中该消息 `images` 为空），第三次不再重发。

### 9.6 既有测试影响与必改点

| 测试 | 影响 | 处理 |
|---|---|---|
| **`test_agentactor.cpp:428-451` `testRunFlowKeepsEventLoopResponsive`** | **必然失败**：该用例断言 `run_flow(mode=once)` 立即返回 `status=="started"` 且 500ms 内不阻塞事件循环（`:442-448`）。改为默认 `wait=true` 后它会等待 `BlockingAgentModule` 的 900ms 运行，且需要 30ms 后外部 `stop()` 才能结束 | **必须改**：调用点显式传 `wait=false`（保留"事件循环不被阻塞"的原始意图），并在同一文件新增 `testRunFlowWaitReturnsCompletion` 覆盖 `wait=true` 语义（见 §9.3 第 32~33 条）。这是**唯一允许的既有测试语义调整**，PR 描述必须引用本行 |
| `test_agentcontroller.cpp:288-316` | 确认后走排队：`confirmPendingTools()` + 10 次 `processEvents()`，现状**已满足** | 不改（若失败，改为 `QTRY_COMPARE(state(), Thinking)`） |
| `test_agentcontroller.cpp:318-345` | `rejectPendingTools` 仍同步 | 不改 |
| `test_agentcontroller.cpp:552-577` | Autopilot 同步链保住 → 通过 | 不改 |
| `test_agentcontroller.cpp:579-601` | 单轮 10 calls 上限不变 | 不改 |
| `test_agentpermissions.cpp:30-69` | 权限派生改为 schema 后语义不变 | 不改；**追加** §9.4 用例 |
| `test_agente2e.cpp:211`（`run_flow` 工具调用） | `wait=true` 默认下该轮会等待运行完成；断言 `runFinishedSpy.count()==1`（`:234`）仍成立，`successRuns>=1`（`:250`）更稳 | **预期仍通过**，verifier 必须实测；若 ScriptedLLMClient 的轮询节奏受影响，只允许把该调用改为显式 `wait=false`，不得改 `run_flow` 语义 |
| `test_toolschema.cpp` | 只断言 `hasTool/findTool/dangerous/params.size()==1/create_project`（`:15-44`），不涉及新增字段 | **禁改**（它验证的是注册表本身；新契约由 `test_agenttoolcontract` 承担） |
| `test_mainwindow.cpp:2441`（`compactTool.name="run_flow"`） | 只用于预览卡片渲染，与 schema 参数无关 | 不改 |
| `test_runengine.cpp:2010-2035`、`:2179`、`:2332` | `validateFlow()` 重构后必须逐字同输出 | 只允许 `validateFlowIssues()` 新增；跑 `ctest -R test_runengine` 必须全绿 |
| `test_agent_sam_tools.cpp` | 不涉及 | 不改 |

> **重要**：`run_flow` 默认 `wait=true` 是行为变更。T3 必须先 `grep -rn "run_flow" tests/ src/ui/` 找出所有调用点，在 PR 描述里列出清单与处理方式。

### 9.7 验证命令与预期基线

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$env:PATH = "D:\deps\Qt\bin;D:\deps\opencv\build\x64\vc16\bin;" + $env:PATH

& $cmake --build build-win-tests -j 8
& $ctest --test-dir build-win-tests -R "test_agent" --output-on-failure      # 9 → 12 个测试
& $ctest --test-dir build-win-tests -R "test_runengine" --output-on-failure  # 必须全绿（RunEngine 重构）
```

- 基线：77 个测试，其中 9 个 `test_agent*` 全绿（roadmap §8.1）。
- 本迭代后：`test_agent*` 共 12 个（新增 `test_agenttoolcontract`、`test_agentretry`、`test_agentclosedloop`）。
- 插件 ABI 不变 ⇒ **无需** `sync-plugins`。

> **构建环境事实（t3/t10 实测，Windows + MSVC + Ninja）**：
> 1. **必须经 vcvars64 注入 MSVC 环境**——本机裸 PowerShell 的 `INCLUDE`/`LIB` 为空，直接 `cmake --build` 会找不到 `cl` 与系统头文件；标准写法 `cmd /c 'call "…\vcvars64.bat" >nul 2>nul && "…\cmake.exe" --build build-win-tests -j 8'`。
> 2. `build-win-tests` 的 CMake 因中文 `showIncludes` 前缀乱码，**头文件依赖追踪实际失效**——改动任何 `.h` 后不会自动重编对应 `.cpp`，会出现"改了头文件却不重编"的假通过。因此**改头文件后必须删除对应 `*.obj` 再构建**，实际落盘路径已核实为 `build-win-tests/src/core/CMakeFiles/deeplux_core.dir/agent/<Name>.cpp.obj`（例：`.../deeplux_core.dir/agent/AgentActor.cpp.obj`；定位命令 `Get-ChildItem build-win-tests -Recurse -Filter <Name>.cpp.obj`），改完目标库头文件后亦可直接删 `build-win-tests/src/core/CMakeFiles/deeplux_core.dir` 再 `cmake --build`。
> 3. 跑测试前必须把 `D:\deps\Qt\bin` 与 `D:\deps\opencv\build\x64\vc16\bin` 加入 `PATH`（否则测试进程加载 DLL 失败）；且**不得并发**跑 `cmake --build` / `ctest`（会争抢构建目录产生偶发失败）。
>
> verifier 与 reviewer 收报告时，凡"改了 `.h`"的任务必须看到"清 obj → 重建 → 测试"的证据链（头文件 mtime < obj mtime < 二进制 mtime），否则按假通过处理；t10 的完整证据见 `build-win-tests/verify-t10-report.md`。

---

## 10. 实施顺序（依赖图）

```text
T2（契约 + 重试 + §4.9 新工具注册）───┬──> T3（handler + RunEngine 追加）───┬──> T4（异步 + 图像 + UI）───> verifier ─> reviewer
                                     │                                    │
                                     └── 契约测试绿 + 注册完成通知 T3 ────┘（T3 不再改 ToolSchema.*）
```

1. **T2 第一批（1 天内可交付）**：§3.1 元数据 + §3.2 权限派生 + §3.3 替换 + **§4.9 注册 6 个新工具与 `run_flow` 的 `wait`/`timeoutMs` 参数** + 契约测试跑绿 → **注册完 schema 后通知 T3**（T3 依赖工厂函数与已注册的 schema 实现 handler；T3 不改 `ToolSchema.*`）。
2. **T2 第二批**：§7 错误分类与重试 + `test_agentretry`。
3. **T3**：§4.8 RunEngine API → 6 个 handler + `run_flow` 的 wait 分支（schema 已由 T2 注册，T3 **只读** `ToolSchema.*`）→ §5.4 `AgentRunWaiter` → `test_agentactor` 断言。
4. **T4**：等 T2、T3 都 `completed` → §5.1 排队 → §5.3 中止 → §5.6 UI → §4.10 提示词 → §6 图像回灌 → `test_agentclosedloop`。
5. **verifier**：§9.7 全量命令 + 回归清单。
6. **reviewer**：逐条比对 §1.2、§3.2.2、§5.7、§8。

---

## 11. 风险与缓解（本迭代新增部分）

| 风险 | 影响 | 缓解 |
|---|---|---|
| `ToolDefinition` 字段顺序被误改成 roadmap 顺序 | **P0**：危险工具变只读 | §3.1 明文禁止 + 契约测试断言 `remove_module.dangerous==true, readOnly==false` |
| `validateFlow()` 重构改变错误文案 | 中：既有测试回归 | 文案逐字保留 + `ctest -R test_runengine` 门禁 |
| 排队执行导致既有确认测试异步化失败 | 中 | 既有测试已 `processEvents()`；失败时改 `QTRY_*`（§9.6 已授权） |
| 嵌套事件循环期间外部 IPC 插入工具调用 | 中 | 状态机 busy gate（`:598`）+ §5.7 第 5 条 |
| 嵌套等待期间 AgentActor 被析构/引擎重载 | 低 | `AgentRunWaiter` 栈对象 + 每次等待前后 `isActive()` 校验；`shutdown()` 调 `abortCurrentTurn()` |
| `run_flow` 默认 `wait=true` 破坏既有脚本 | 中 | §9.6 逐条排查 + PR 清单；`wait=false` 保留旧语义 |
| 图像回灌撑爆上下文 | 中 | §6.3 三重上限（2 张 / 1280 / 8MB）+ 既有 token 估算已计图片字节 |
| `m_nodeOutputs` 无锁读的数据竞争 | 高 | §4.8 前置条件 `!isBusy()` + 锁内拷贝；后续 TSan 独立跟进（第二迭代） |
| 重试把一轮拖长（4 次尝试 × 30s 超时） | 中 | 仅对 4 类可重试错误；`abortRequest` 可随时打断；`retryScheduled` 让用户看到进度 |

---

## 12. 附录

### 附录 A：代码锚点核对清单（12 处，全部实测有效）

核对方法：`read` 工具逐段读取基线 `d381129` 工作区文件，下列行号为读取时的实际行号；引用文本为原文截取。**任何实现任务在改代码前应先用同样方法复核自己要用到的行号**（文件可能已被同一迭代的其他任务改动，行号会漂移）。

| # | 位置 | 实测内容（截取） |
|---|---|---|
| 1 | `src/core/agent/AgentController.cpp:24-25` | `constexpr int kMaxAgentToolTurns = 20;` / `constexpr int kMaxToolCallsPerTurn = 10;` |
| 2 | `src/core/agent/AgentController.cpp:29-34` | `bool isReadOnlyToolName(...)` + `static const QStringList readOnlyTools = {...}` |
| 3 | `src/core/agent/AgentController.cpp:55-57` | `bool isDangerousToolName(const QString& toolName) { return ToolSchema::instance().findTool(toolName).dangerous; }` |
| 4 | `src/core/agent/AgentController.cpp:382-397` | `void AgentController::confirmPendingTools()`（`:395` 现为同步 `doConfirmPendingTools(calls)`） |
| 5 | `src/core/agent/AgentController.cpp:455-464` | 单轮 10 calls（`:455`）与 20 轮（`:460`）上限检查 |
| 6 | `src/core/agent/AgentController.cpp:603-616` | `const bool readOnly = isReadOnlyToolName(toolName); const bool dangerous = isDangerousToolName(toolName);` + 确认/拒绝分支 |
| 7 | `src/core/agent/AgentActor.cpp:246-316` | `QJsonObject AgentActor::executeTool(...)`，`:270-312` 为 `if (toolName == ...) else if ...` 链 |
| 8 | `src/core/agent/ToolSchema.cpp:129-258` | `void ToolSchema::registerDefaultTools()`（19 个 `registerTool`，`:144`/`:188`/`:257` 用第 4 位 `true` 表 dangerous） |
| 9 | `src/core/engine/RunEngine.h:106-110` | `bool isBusy() const { return isExecuting() || m_maintenance... || state() == RunState::Running || ... Paused; }` |
| 10 | `src/core/engine/RunEngine.h:186-197` | signals 区：`:189 runFinished(const RunResult&)`、`:192 moduleStarted`、`:193 moduleFinished`、`:195 breakpointHit` |
| 11 | `src/core/agent/ILLMClient.h:50-58` | `struct AgentResponse { bool success; QString errorMessage; ... int promptTokens; int completionTokens; }` |
| 12 | `src/ui/widgets/AgentChatPanel.h:37` | `static constexpr int kMaxImageAttachments = 4;` |

（另有 §2 全部行号同样实测：`AgentController.cpp:79-102/174-191/209-233/237-260/262-324/328-378/399-432/434-445/449-531/535-590/632-636`；`AgentActor.cpp:38-55/234-242/374-381/411-417/582-606/650-654/689-697/699-710`；`RunEngine.cpp:53-54/95-171/159-163/183-188/977-988/1196-1240/1707/1763/1830-1865`；`OpenAILLMClient.cpp:68-94/96-121/113/123-138/140-183/185-198`；`MainWindow.cpp:590/837/1634-1635/1683-1765/1771-1778/4364-4383/6096-6128`；`Logger.h:23-28/50-53/92`；`Logger.cpp:106-126/151-154`；`GuiEvent.h:76-83`；`tests/*` 如 §9.6 所列。）

### 附录 B：错误码总表（新工具 + 修复项）

| code | 含义 | 出现工具 |
|---|---|---|
| `node_not_found` | 节点名/实例 id 都无法解析 | get_node_runtime_outputs / tune_node_and_verify / export_node_output / remove_module(D1) |
| `no_project` | 无当前工程 | validate_workflow / get_node_runtime_outputs / tune_node_and_verify |
| `no_modules` | 工程无模块（校验结论，不是工具失败） | validate_workflow（`issues[].code`） |
| `engine_busy` | 引擎忙 | validate_workflow / get_node_runtime_outputs / tune_node_and_verify / export_node_output |
| `engine_load_failed` | 引擎加载工程失败 | validate_workflow / tune_node_and_verify / export_node_output |
| `already_running` | 流程已在运行 | run_flow |
| `invalid_parameter` | 参数未通过插件校验 | tune_node_and_verify |
| `run_failed` | 流程运行失败 | run_flow / tune_node_and_verify |
| `timeout` | 等待超时（已请求停止） | run_flow / tune_node_and_verify |
| `cancelled` | 用户中止 | run_flow / tune_node_and_verify |
| `paused_at_breakpoint` | 等待被断点中断 | run_flow / tune_node_and_verify |
| `reentrant_wait` | 同线程嵌套等待被拒 | run_flow / tune_node_and_verify |
| `no_image` | 无视口/无 lastOutput 图像 | capture_viewport_image |
| `no_image_output` | 节点无图像输出 | export_node_output |
| `image_write_failed` | 临时图写盘失败 | capture_viewport_image / export_node_output |
| `tool_not_registered` | schema 有但注册表无（防漂移） | 所有（`AgentActor::executeTool`） |
| `project_create_failed` | 建工程失败（D3） | create_project |

### 附录 C：`docs/` 之外的零改动承诺

- 不改插件、不改 `IModule` ABI、不改 `CMakeLists.txt`（根）、不改 `Project`/`ModuleInstance` 数据模型。
- `RunEngine` 仅新增 API + 内部成员，不改既有公开函数签名与字符串输出。
- 唯一的行为性破坏变更：`run_flow` 默认 `wait=true`（已在 §4.7/§9.6 标注处理方式）。

### 附录 D：待决问题（记录，不在本迭代实现）

1. 可取消的单节点执行原语（`tune_node_and_verify(runMode=node)` 真执行版）——需要 RunEngine 侧的异步节点作业 + 取消协议。
2. 工具结果的组内原子裁剪与 6k 截断（roadmap 第二迭代）。
3. 审计落盘 JSONL 与 token 用量面板的完整版。
4. 图片临时目录配额与 LRU 清理。
5. `AgentBridge` 外部调用对 `wait` 类工具的默认策略（当前会阻塞一次 IPC 请求直到完成，第二迭代加"外部调用强制 `wait=false`"开关）。
