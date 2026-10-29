# DeepLux Agent 第二迭代（下半）设计冻结：审计落盘 + 失败报文 + MCP 桥 + 遗留修复

- 状态：**冻结（实现依据，改动需 architect 复核）**
- 上游文档：
  - [agent-execution-plan-v2.md](agent-execution-plan-v2.md)（任务清单与依赖：t18 审计 / t20 MCP / t19 遗留 / t21 验证 / t22 审查 / t16 集成）
  - [agent-iteration2-design.md](agent-iteration2-design.md)（2A 流式与上下文治理，t12/t13；本文不重复其契约）
  - [agent-improvement-roadmap.md](agent-improvement-roadmap.md) §11.6（遗留 L1~L10）
- 基线：`af56b5b`（第一迭代交付提交）。**本文所有行号以该提交为准**；t12/t13 正在并行修改 `AgentController.*` / `OpenAILLMClient.*` / `ILLMClient.*`，工作树行号会漂移——t18/t19 开工时请用 `git grep -n "<符号>"` 重新定位（每处引用都同时给了符号/函数名）。
- 读者：implementer-core（t18/t19）、implementer-tools（t20）、verifier、reviewer
- 适用代码：`src/core/agent/**`、`src/core/engine/RunEngine.{h,cpp}`、`src/app/**`（新增 MCP 桥）、`src/ui/views/MainWindow.cpp`（L3 接线）、`tests/**`
- 文档用途：实现者照此编码即可；凡与执行计划冲突处，以本文为准并在 §1.3 说明（当前无偏离，见 §1.3）。

---

## 0. 阅读顺序与生效规则

1. 先读 §2（现状基线，行号已核对）；再读对应契约：t18 → §3/§4；t20 → §5/§6；t19 → §7；§8/§9 所有人必读。
2. `必须`/`禁止`/`不得` 是验收项；`应当` 是强建议（偏离需 PR 说明）。
3. 本迭代**不改** 2A 已冻结契约（流式/上下文）；本文只做审计、失败报文、MCP、六项遗留。
4. 新增配置键、错误码、文件格式，必须先改本文再改代码。

---

## 1. 范围与硬不变量

### 1.1 交付

| 编号 | 交付 | 主责 |
|---|---|---|
| A | 审计 JSONL 落盘（按天轮转、10MB/文件、保留 14 天、字段映射、写失败不影响对话） | t18 |
| B | LLM 失败请求报文落盘（触发类别、命名、大小上限、脱敏） | t18 |
| C | MCP stdio 桥（`deeplux-mcp-bridge`，`src/app` 新 target，JSON-RPC 2.0 四方法 + ping，ToolSchema 工具清单，转发 AgentBridge v1.0） | t20 |
| D | MCP 权限模型（外部只读、写工具拒绝且不转发、不得绕过控制器判定） | t20 |
| E | 六项遗留修复 L1/L2/L3/L5/L8/L9（方案 + 文件 + 测试断言） | t19 |
| F | 文件所有权、测试清单、验证命令 | 本文 |

### 1.2 硬不变量（任何实现都不得破坏）

1. **插件 ABI 不变**：不改 `src/core/interface/IModule.h` / `DEEPLUX_MODULE_INTERFACE_VERSION` / `src/plugins/**`；本迭代**无需** `sync-plugins`。
2. **不引入第三方依赖**：JSONL/JSON-RPC/SSE 全部手写；仅用 Qt Core/Network/Widgets/Test。MCP 桥不引入 MCP SDK、不引入 httplib/asio。
3. **不改根 `CMakeLists.txt`**：MCP 目标写在 `src/app/CMakeLists.txt`（根文件已有 `add_subdirectory(src/app)`，`CMakeLists.txt:152`）。
4. **不改工具 schema 语义**：`ToolSchema.h/.cpp` 的工具名、参数、readOnly/dangerous、错误码**零改动**（仍 25 个工具）；MCP 只读其元数据，不新增/删除/改权限。
5. **无任意执行**：不新增 shell/进程执行能力；MCP 桥只转发 `tool_call`，`execute` 永久拒绝（沿用 `AgentBridge.cpp:302-305`）。
6. **密钥/图片不落盘**：审计与失败报文中不得出现 `Authorization`、API Key、图片二进制或 base64 data URL（§3.8/§4.4）。
7. **外部默认只读**：MCP 外部调用不得写工程/写文件；写工具返回 `code=permission_denied` 且**不转发**（§6）。
8. **既有接口与测试不回归**：`AgentActionLogEntry` 只允许**新增**字段；既有 `AgentBridge` v1.0 报文形状与认证语义不变；`ctest -R "test_agent|test_agentbridge|test_runengine"` 必须全绿。
9. **失败不阻塞**：审计/失败报文写盘失败只记 `Logger`，不得改变对话状态、不得弹错、不得抛出到 GUI 线程事件循环。

### 1.3 与执行计划的一致性

| 计划条目 | 本文落点 | 一致性 |
|---|---|---|
| t18 审计 JSONL + 失败报文（依赖 t13） | §3/§4/§8 | 一致 |
| t20 MCP（依赖 t17，与 t18/t19 并行） | §5/§6 | 一致；**不修改 core**（只读消费 ToolSchema），因此与 t18/t19 无文件交集 |
| t19 六项遗留（依赖 t18） | §7 | 一致 |
| 观测口径 `~/.deeplux/logs`、10MB、14 天、脱敏 | §3/§4 | 一致 |
| 外部只读、写工具拒绝、不绕过控制器 | §6 | 一致 |
| **无偏离** | —— | 若实现阶段发现需偏离（如必须改根 CMake），先报队长 |

---

## 2. 现状基线（提交 `af56b5b`，实现前必读）

> 说明：t12（流式）正在工作树中改 `AgentController.*` / `OpenAILLMClient.*` / `ILLMClient.*`（新增 `SseStreamParser.*`），t13 随后继续改 `AgentController.*`。因此**不要按工作树行号**改代码：先 `git show af56b5b:<path>` 或 `git grep -n "<符号>"` 确认语义与位置；本文所有行号均取自 `af56b5b`。

### 2.1 审计条目与 UI

- `AgentActionLogEntry` 现有 8 字段（`src/core/agent/GuiEvent.h:76-86`）：`timestamp/actor/action/params/result/undoable/durationMs/errorCategory/toolCallId`（`durationMs=-1` 表示未采集）。
- 构造点：`AgentController.cpp:90-103` `makeAuditEntry()`（读取并剥离 `_elapsedMs`、按注册表算 `undoable`）。
- 发射点（全部 `emit actionLogEntryAdded`，无统一审计钩子）：撤销 `AgentController.cpp:262-268`、确认批 `:566`、LLM 循环 `:633`、外部 IPC `:889-891`；`logAction()` 仅转发信号（`:217-219`）。
- UI 消费：`AgentActionLogWidget`（`src/ui/widgets/AgentActionLogWidget.h:20-51`，7 列 + 撤销/清空）；**只存内存**，无落盘。
- 路径工具：`PathUtils::appDataPath()`（`src/core/platform/PathUtils.cpp:10-26`，`DEEPLUX_APP_DATA_DIR` 可覆盖，默认 `~/.deeplux`）、`PathUtils::logPath()`（`:40-44`，= `<appData>/logs`）。
- 既有日志：`Logger` 写 `DeepLux_YYYYMMDD.log`，10MB 轮转（`src/core/common/Logger.cpp:47-56`、`:188-206`），与本文审计文件**互不复用**。

### 2.2 AgentBridge 协议 v1.0（MCP 的上游）

- 协议版本常量 `PROTOCOL_VERSION = "1.0"`（`src/core/agent/AgentBridge.h:117`）；服务启动/停止 `:117-211`。
- 传输：`QLocalServer`；Windows 命名管道 `\\.\pipe\deeplux_agent`（`AgentBridge.cpp:131`），Linux `<appData>/agent.sock`，失败回落 `deeplux_agent_<hash>`（`:124-168`）。
- 帧格式：**每行一个紧凑 JSON 对象**（`AgentConnection.cpp:35-44` 发送追加 `\n`；`:73-98` 按 `\n` 解析；单行 1MB、缓冲 8MB 上限 `:10-13`）。
- 入站报文：`{"version":"1.0","type":"tool_call|query|ping|subscribe|auth","id":"<reqId>","payload":{...}}`（`AgentBridge.cpp:265-323`）。
- 出站：`result`（`:460-480`）/`error`（`:482-504`）/`event`（`:506-535`）。
- `tool_call`：`payload={"tool":"<name>","params":{...}}`（`handleToolCall` `:424-439`，缺 `tool` 返回 `{"error":"Missing 'tool' field in tool_call"}`）。
- 认证：配置 `agent/bridgeToken` 非空时，首条必须是 `{"type":"auth","payload":{"token":...}}`；错误令牌断开（`AgentBridge.cpp:121-122`、`:273-300`）。
- 心跳：服务端每轮向客户端发 `ping`，连续 3 次未响应断连（`:354-402`）。
- 工具回调：`setToolCallCallback`（`AgentBridge.h:59-61`）由 `AgentController::initialize()` 设为 `handleToolCall`（`AgentController.cpp:159-160`）。

### 2.3 GUI 启动与构建

- `main.cpp` 双模式：CLI（`:140-184`）/ GUI（`:186-232`）；GUI 由 `MainWindow` 启动，AgentBridge 在 `TerminalBridge` 中 `start()`（`src/ui/bridge/TerminalBridge.cpp:88`）——**GUI 未启动则无管道/套接字**。
- 目标：`src/app/CMakeLists.txt:8-10` `add_executable(DeepLux ...)`，安装 `:44-46`；根 `CMakeLists.txt:152` `add_subdirectory(src/app)`；运行时输出目录 `CMakeLists.txt:43`（`bin/`）。
- `deeplux_core` PUBLIC 链接 Qt Core/Gui/Widgets/Network/SerialPort/Concurrent（`src/core/CMakeLists.txt:201`/`:206`）——MCP 桥链接 core 后可只用 Core/Network，不创建 QApplication/Widget。

### 2.4 RunEngine 快照与 `m_nodeOutputs`（L5 基线）

- 对外快照 API（第一迭代新增）：`moduleRuntimeSnapshot()`（`RunEngine.h:177`，实现 `RunEngine.cpp:1918-1925`，`!isBusy()` + `m_runtimeInfoMutex` 拷贝）、`nodePortOutputs()`（`:1927-1934`，`!isBusy()` + `m_lifecycleMutex` 拷贝）、`lastFrameId()`（`:1936-1943`，同锁）。
- `m_nodeOutputs` **写入点（当前无锁）**：`RunEngine.cpp:769`（移除模块）、`:826`（清空模块）、`:1040`（运行开始清空）、`:1355`（并行批次回写）、`:1583`（失效缓存）、`:1721`（`executeModule` 主写点）、`:1841`（禁用节点旁路写）；读取点 `:1230`（`collectModuleInputs`）、`:1565`、`:1820`。
- 执行线程只在持有生命周期租约时运行，但**租约是标志位而非全程持锁**（`tryAcquireLease` 在 `m_lifecycleMutex` 内取权后释放），因此当前「读加锁 + 写不加锁」仍有数据竞争 ⇒ L5 修复见 §7.4。

### 2.5 AgentActor 等待与端口序列化（L1/L2 基线）

- 唯一等待入口 `AgentActor::runAndWait(mode, timeoutMs, forceReload)`（`AgentActor.cpp:1409-1445`），把 `AgentRunWaiter::Outcome` 映射为私有 `RunWaitResult`（`AgentActor.h:102-117`，`timedOut/aborted/pausedAtBreakpoint/reentrant/rejectedStart/failedModule/ok/error/elapsedMs/totalRuns/successRuns`）。
- `tune_node_and_verify`（`:1572-1710`）在写参数后调用 `runAndWait`（`:1687`），并按 `timedOut/aborted/reentrant/pausedAtBreakpoint/run_failed` 映射错误码（`:1689-1702`）；超时/中止分支**无自动化测试**（L1）。
- 端口序列化：`serializePortValue`（`AgentActor.cpp:209-258`，支持 ImageData/数值/布尔/字符串/QVariantList，其它返回 false）→ `serializePortOutputs`（`:265-276`，未支持端口进 `unsupportedPorts` 并排序）→ `getNodeRuntimeOutputs` 写入结果（`:1499-1512`）。现有插件无自定义 QVariant 端口生产者，`unsupportedPorts` 分支无断言（L2）。
- 测试可见性：`DEEPLUX_TEST_VISIBILITY` 已用于暴露 handler（`AgentActor.h:128-155`），可沿用该机制加测试钩子。

### 2.6 视觉能力（L3 基线）

- `OpenAILLMClient::supportsImageInput()` = `m_visionEnabled && !m_visionDowngraded`（`OpenAILLMClient.h:27-30`）；`setVisionEnabled(true)` 清除降级（`OpenAILLMClient.cpp:104-110`）。
- 配置注入：`MainWindow::loadAgentSettings()`（`MainWindow.cpp:6140-6188`）用 `cfg.groupBool("agent","visionEnabled",true)`（`:6182` 区域）；`ConfigManager::containsGroupKey`（`src/core/manager/ConfigManager.h:38`）可用于判断"用户是否显式设置过"。
- 现状**无模型名启发式**（L3）。

### 2.7 控制器外部入口与轮次（L8/L9 基线）

- `handleToolCall`（`AgentController.cpp:862-893`）：busy 门禁 → 未注册工具早退 → `toolAccess`/`requiresConfirmation`（Observer 拒绝文案 `:872-874`；需确认时进入 `Confirming` 并返回 `pending_confirmation`）→ 执行 + 审计。
- 轮次控制：`sendUserMessage`/`WithImages`（`:344-439`）→ `extendAgentLoop`（`:586-673`）→ 续跑请求（`:668-672`）；上限 20 轮/10 calls（`AgentController.cpp:28-29`）。
- 中止：`abortCurrentTurn`（`:232-254`，幂等、generation 作废、`abortRequest`、`abortRunningTool`）。
- 现状**无单轮墙钟 deadline**（L9）；外部 `tool_call` 直接透传 `run_flow(wait=true)`，会阻塞外部客户端直到运行结束（L8）。

### 2.8 既有测试（不得回归）

| 测试 | 关键断言 |
|---|---|
| `tests/test_agentbridge.cpp:17-222` | execute 拒绝、query/ping/subscribe、tool_call 回调、控制器权限（pending_confirmation）、token 认证、1MB 入站断开、写响应不持锁 |
| `tests/test_agentcontroller.cpp` | 状态机/权限/裁剪/abort/审计（含 `_elapsedMs` 剥离） |
| `tests/test_agentactor.cpp` | 25 工具、run_flow wait、tune、capture/export、R1/R2/R3 修复 |
| `tests/test_runengine.cpp` | `validateFlow` 文案逐字、引擎生命周期 |
| `tests/test_agentretry.cpp` | 分类/重试/视觉降级 |

---

## 3. 审计 JSONL 落盘契约（t18）

### 3.1 位置、命名与轮转

| 项 | 规定 |
|---|---|
| 目录 | `PathUtils::logPath()`（= `<appData>/logs` = `~/.deeplux/logs`；`DEEPLUX_APP_DATA_DIR` 可覆盖，便于测试） |
| 当日文件 | `agent-audit-YYYYMMDD.jsonl`（本地日期，`QDate::currentDate()`；**按天轮转**：跨天自动切换新文件） |
| 轮转 | 当日文件 `>= agent/auditMaxFileBytes`（默认 **10 MiB = 10 MB = 10485760 字节**）时换分片：`agent-audit-YYYYMMDD-01.jsonl`、`-02` …；单日最多 99 片，超出则当日停止写入并 `Logger::warning` |
| 格式 | **JSON Lines**：一记录一行、紧凑 JSON、行尾 `\n`、UTF-8、无 BOM |
| 保留 | **14 天**：初始化时与跨天首次写入时，删除文件名日期 `< today-14d` 的 `agent-audit-*.jsonl`（解析文件名失败则按 `QFileInfo::lastModified` 兜底） |
| 目录创建 | 写入前 `PathUtils::ensureDirExists(logPath())`；失败按 §3.5 处理 |

### 3.2 字段映射（逐字段，验收基准）

`AgentActionLogEntry` → JSONL 记录（`v` 固定 1）：

| C++ 字段 | JSON 键 | 规则 |
|---|---|---|
| （常量） | `v` | 整数 `1`（格式版本） |
| `timestamp`（`QDateTime`） | `ts` | ISO-8601 带本地偏移与毫秒：`yyyy-MM-ddTHH:mm:ss.zzz+08:00`；无效时间用当前时间 |
| `actor` | `actor` | 原样（`Agent`/`User`/`System`） |
| `action` | `action` | 原样 |
| `params`（JSON 字符串） | `params` | 可解析为 JSON 对象/数组 → 原样嵌入；否则 `{"raw": "<前 8KB>"}`（超长截断并加标记） |
| `result` | `result` | 原样（`success`/`error`/`pending`/`denied`/`trimmed`/`truncated`） |
| `undoable` | `undoable` | bool |
| `durationMs` | `durationMs` | `>=0` 时写入；`-1` **省略该键** |
| `errorCategory` | `errorCategory` | 非空时写入；空省略 |
| `toolCallId` | `toolCallId` | 非空时写入；空省略 |

- **禁止**新增/改名其它键（保持与 `AgentActionLogEntry` 一一对应）；未来的字段扩展必须升 `v`。
- 记录顺序 = 信号发射顺序（GUI 线程串行），不做重排。
- 单条记录上限 **16 KiB**：`params` 先按 8 KiB 截断，仍超则 `params` 退化为 `{"raw":"…[truncated]"}`；不得截断 `ts/actor/action/result`。

**示例**

```json
{"v":1,"ts":"2026-10-27T22:41:12.345+08:00","actor":"Agent","action":"run_flow","params":{"mode":"once","wait":true},"result":"success","undoable":false,"durationMs":132,"toolCallId":"call_1"}
```

### 3.3 写入时机与实现位置

- 新类 `src/core/agent/AgentAuditLog.{h,cpp}`（t18 独占），API：

```cpp
class AgentAuditLog {
public:
    static AgentAuditLog& instance();
    bool initialize(QObject* controller);   // 连接 AgentController::actionLogEntryAdded
    void shutdown();
    void append(const AgentActionLogEntry& entry);   // 同步追加一行
    void flush();
    void setEnabledForTest(bool);           // DEEPLUX_TEST_VISIBILITY 或 static + 测试构建
    QString currentFilePath() const;         // 测试断言用
    int droppedRecords() const;              // 写失败计数（测试/诊断）
};
```

- `AgentController::initialize()` 末尾调用 `AgentAuditLog::instance().initialize(this)`；`shutdown()` 调用 `shutdown()`（t18 只改这两处，约 4 行）。
- 通过信号连接而非改 4 个发射点：保证**所有**审计条目（含撤销、系统错误、MCP 拒绝）都落盘，且不遗漏未来新增发射点。
- 写入线程 = GUI 线程（信号直连）；单条 `<16KiB` 同步 `write+flush` 可接受（审计频率 ≤ 每秒数十条）。

### 3.4 轮转与保留的实现细节

- 每次 `append`：若 `QDate::currentDate()` != `m_currentDate` → `rotateToNewDay()`（关旧文件、更新日期、执行保留清理）。
- 若当前文件 `size() >= maxBytes` → `rotateToNewPart()`（同一日期换 `-NN` 后缀）。
- 保留清理：只匹配 `^agent-audit-\d{8}(-\d{2})?\.jsonl$`；日期解析失败 → 用 mtime；删除失败只 `Logger::warning`。
- 不使用 `QSaveFile`（追加语义），用 `QFile` `Append` 模式；每条 `flush()`。

### 3.5 写盘失败语义（硬要求）

1. `open/mkpath/write/flush` 任一失败 → `qWarning` + `Logger::instance().warning("agent audit write failed: ...", "Agent")`；**不得** emit `llmErrorOccurred`、不得改 state、不得抛异常。
2. 连续失败 **3 次** → 本次进程内禁用审计落盘（`m_disabled=true`），直到下一个自然日或重新 `initialize`；`droppedRecords++` 累计。
3. 磁盘满/权限错误同上；`droppedRecords` 仅测试/诊断可见，不进 UI。
4. 关闭/析构：`flush()` + `close()`，失败仅日志。

### 3.6 并发与顺序

- 单线程写入（GUI 线程），无需锁；`append` 重入保护：若 `m_inAppend` 为真直接返回（防信号风暴递归）。
- 与 `Logger` 互不阻塞（不同 QFile 实例）。

### 3.7 配置键

| 键 | 类型 | 默认 | clamp |
|---|---|---|---|
| `agent/auditEnabled` | bool | true | —— |
| `agent/auditMaxFileBytes` | int | 10485760（10 MiB） | [1048576, 104857600] |
| `agent/auditRetentionDays` | int | 14 | [1, 365] |

### 3.8 脱敏（审计侧）

- `params` 中键名匹配 `(?i)^(api[_-]?key|authorization|token|secret|password|bearer)$` 的值替换为 `"***"`（工具参数一般无此键，但防御性处理）。
- **禁止**把图片 `AgentImageAttachment.data` 写盘：审计只记 `params` 字符串，本就无图片字节；若 `params` 出现 `data:image/...;base64,` 片段 → 替换为 `data:image/…;base64,[omitted]`。

### 3.9 测试断言（`tests/test_agentaudit.cpp`，t18 新建）

1. `testFileNameAndFormat`：`currentFilePath()` 形如 `.../logs/agent-audit-YYYYMMDD.jsonl`；写 1 条后文件存在，逐行 `QJsonDocument::fromJson` 成功。
2. `testFieldMapping`：构造 8 字段齐全/`durationMs=-1`/空 `errorCategory`/空 `toolCallId` 的条目 → 键集合与 §3.2 完全一致（省略键断言）。
3. `testParamsObjectAndRaw`：合法 JSON 字符串 → 嵌入对象；非法字符串 → `{"raw": ...}`。
4. `testRotationBySize`：把 `auditMaxFileBytes` 调到 1KB（测试配置）→ 写 3 条后出现 `-01` 分片，且原文件不超限。
5. `testRetention`：预置 15 天前的 `agent-audit-YYYYMMDD.jsonl` → `initialize/跨天` 后文件被删；14 天内的保留。
6. `testWriteFailureDoesNotAffectConversation`：把 `DEEPLUX_APP_DATA_DIR` 指到只读/不可写路径（或写入前把目录替换为文件）→ `append` 不抛、`llmErrorOccurred` 计数 0、`droppedRecords>0`、下次写入仍继续尝试（未到 3 次）。
7. `testDisabledConfig`：`agent/auditEnabled=false` → 不创建文件、`append` 无副作用。
8. `testRedaction`：`params={"apiKey":"sk-LEAKED","data":"data:image/png;base64,AAAA"}` → 文件中无 `sk-LEAKED`、无 `AAAA`。

---

## 4. LLM 失败请求报文契约（t18）

### 4.1 触发类别（只记录"需要复现"的持久性失败）

| 类别 | 是否落盘 | 理由 |
|---|---|---|
| `BadRequest`（含 `contextLimitSuspected`、`visionDowngradeHint`） | ✅ | 参数/上下文/多模态兼容问题，需要报文复现 |
| `Malformed`（非法 JSON/无 choices/空响应） | ✅ | 网关/代理问题，需要原始响应体 |
| `Auth` | ❌ | 可能包含密钥相关回显，禁止落盘（只记审计 `errorCategory`） |
| `RateLimit` / `Server` / `Network` / `Timeout` | ❌ | 瞬时错误、频率高；只记审计 |
| 用户主动中止（`abortRequest`） | ❌ | 不是失败 |

- 同一请求的多次重试只在**最终失败**时落一条（`handleFailure` 的收口分支，非 `retryScheduled` 分支）。

### 4.2 文件、结构与上限

| 项 | 规定 |
|---|---|
| 目录 | `<appData>/logs/` |
| 文件 | `llm-failed-YYYYMMDD.json`（本地日期） |
| 结构 | `{"v":1,"records":[<record>, ...]}`；追加方式 = 读取→解析→插入→原子重写（`QSaveFile`）；单进程串行，频率 ≤ 数十条/天 |
| 单条上限 | **256 KiB**（超限按 §4.4 截断） |
| 单日文件上限 | **1 MiB**；达到后停止追加并 `Logger::warning`（不轮转，保留首条现场） |
| 保留 | 14 天（与审计共用清理策略与配置） |
| 写失败 | 同 §3.5：只记日志、不影响对话 |

### 4.3 记录字段

```json
{
  "ts": "2026-10-27T23:01:02.345+08:00",
  "category": "BadRequest",
  "httpStatus": 400,
  "attempts": 1,
  "endpoint": "https://api.example.com/v1/chat/completions",
  "model": "gpt-4o",
  "contextLimitSuspected": false,
  "visionDowngradeHint": true,
  "request": {
    "messageCount": 12,
    "totalChars": 8421,
    "messages": [
      {"role": "user", "chars": 120, "head": "把 GrabImage 的曝光改成…", "images": 0, "toolCallId": ""},
      {"role": "assistant", "chars": 640, "toolCalls": 2},
      {"role": "tool", "chars": 2200, "toolCallId": "call_1"}
    ]
  },
  "response": {"chars": 320, "head": "The request was rejected ..."},
  "detail": "HTTP 400: … | body: …(截断 2KB)",
  "redactionApplied": ["authorization", "apiKey", "imageBase64"]
}
```

规则：

1. `endpoint`：只存 scheme+host+path（去 query，防 token 进 URL）。
2. `request.messages`：**不存完整历史**；逐条只存 `role`、`chars`、`head`（≤2KB，正文前段）、`images`（数量，**不存数据**）、`toolCalls`（数量）或 `toolCallId`。
3. `response.head`：响应体前 **64KB**；`chars` 为原始长度。
4. `detail`：沿用 `LLMErrorInfo.detail`（已截断 2KB，`OpenAILLMClient.cpp:17-21`）。
5. 总记录 ≤256KB：超限时先砍 `response.head` 到 16KB，再砍 messages 到前 8 条，仍超则 `response.head` 省略。

### 4.4 脱敏规则（硬要求）

对即将落盘的所有字符串依次执行：

| # | 规则 | 替换 |
|---|---|---|
| R1 | JSON/文本中的 `(?i)(authorization|api[_-]?key|token|secret|password)\s*[:=]\s*"?[^",\s}]+` | `$1: "***"` |
| R2 | `Bearer\s+[A-Za-z0-9._\-]+` | `Bearer ***` |
| R3 | `data:image/[^;]+;base64,[A-Za-z0-9+/=\s]+` | `data:image/…;base64,[omitted]` |
| R4 | 任意连续 base64 字符 ≥4096 且无空格（疑似图片/附件） | `[omitted-base64]` |
| R5 | `"images":[{"data":"..."}]` 类结构 | 只保留数量 |

- **禁止**记录请求头（`Authorization` 头从不进入记录）；`apiKey` 从不在 `LLMErrorInfo.detail` 中，但仍按 R1/R2 双层兜底。
- 落盘后必须做**自检**：若记录序列化文本仍命中 R1/R3/R4 任一模式 → 整条丢弃并 `Logger::warning("failed-request record dropped by redaction self-check")`（宁可无记录，不可泄密）。

### 4.5 实现位置

- 新类 `src/core/agent/FailedRequestLog.{h,cpp}`（t18 独占）：
  `void record(const LLMErrorInfo& info, const QByteArray& requestBody, const QString& endpoint, const QString& model, const QList<AgentMessage>& messages);`
- 调用点：`OpenAILLMClient::handleFailure`（`af56b5b` 的 `OpenAILLMClient.cpp:367-397`）的"最终失败"分支（非 `retryScheduled` 分支）调用 `FailedRequestLog::instance().record(...)`；仅当类别 ∈ §4.1 ✅ 且配置开启。**注意**：t12 流式改造会重写 `handleFailure`/新增流式失败收口（`streamFailed`），t18 必须挂到"最终失败且类别命中"的收口点，按符号检索定位，不要照抄基线行号。
- 需要把 `m_lastBody`（`:286`）与当前请求的消息列表传入；`messages` 可从 `m_lastRequestBody` 反序列化或额外保存 `m_lastMessages`（t18 改动 `OpenAILLMClient` 成员，**只加不改**）。
- 配置键：

| 键 | 默认 | clamp |
|---|---|---|
| `agent/failedRequestLogEnabled` | **false**（集成收口 B5：未显式配置默认关闭；已有显式值继续尊重） | —— |
| `agent/failedRequestMaxBytes` | 1048576（1 MiB/日） | [65536, 16777216] |
| `agent/failedRequestRecordMaxBytes` | 262144（256 KiB/条） | [16384, 1048576] |

### 4.6 测试断言（`tests/test_agentfailedrequest.cpp`，t18 新建）

1. `testBadRequestWritten`：本地假端点返回 400（含 image_url 文案）→ 文件 `llm-failed-YYYYMMDD.json` 存在、`records[0].category=="BadRequest"`、`visionDowngradeHint==true`。
2. `testMalformedWritten`：200 + 非 JSON → `category=="Malformed"`。
3. `testAuthNotWritten`：401 → 文件不存在（或 records 为空）。
4. `testTransientNotWritten`：429/500/超时（重试耗尽）→ 不落按 §4.1 排除的类别。
5. `testNoSecretsOnDisk`：请求带 `Authorization: Bearer sk-test-123` 与一张 base64 图片；文件文本不含 `sk-test-123`、`Bearer sk-`、`base64,` 之后的载荷、`data:image` 的原文。
6. `testSizeCaps`：把单日/单条上限调小 → 记录被截断且文件 ≤ 上限；超日上限后停止追加。
7. `testWriteFailure`：目录不可写 → 对话仍能收到 `errorOccurred`、状态回 Idle、无异常。
8. `testRetention`：15 天前文件被清理。

---

## 5. MCP stdio 桥契约（t20）

### 5.1 目标、放置与构建

| 项 | 规定 |
|---|---|
| 可执行名 | `deeplux-mcp-bridge`（Windows `deeplux-mcp-bridge.exe`） |
| 放置 | `src/app/mcp/**`（新目录）：`main.cpp`、`McpServer.{h,cpp}`、`JsonRpc.{h,cpp}`、`BridgeClient.{h,cpp}` |
| 构建 | 在 `src/app/CMakeLists.txt` **追加**：`add_library(deeplux_mcp STATIC …)` + `add_executable(deeplux-mcp-bridge main.cpp)` + `target_link_libraries(deeplux_mcp PUBLIC deeplux_core Qt::Core Qt::Network)` + `install(TARGETS deeplux-mcp-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})`；**不得改根 CMakeLists**（根已有 `add_subdirectory(src/app)`，`CMakeLists.txt:152`） |
| 子系统 | 控制台程序（不得加 `WIN32_EXECUTABLE`；stdout 为协议通道） |
| 依赖 | 链接 `deeplux_core`（复用 `ToolSchema`）与 Qt Core/Network；不创建 `QApplication`/QWidget；不初始化 OpenCV |
| 输出目录 | 沿用 `CMAKE_RUNTIME_OUTPUT_DIRECTORY=bin`（根 `CMakeLists.txt:43`） |

> 说明：`deeplux_core` 是 SHARED 且 PUBLIC 依赖 Qt Widgets/OpenCV（`src/core/CMakeLists.txt:201`），桥进程会同时加载这些 DLL（同目录部署），但**不实例化**任何 GUI 对象；这是复用 `ToolSchema` 单源的最小代价，替代方案（复制 schema）被禁止。

### 5.2 进程与 IO 协议

- 运行模型：由 MCP 客户端（Claude Desktop / Cursor 等）以 stdio 启动；`stdin` 读请求、`stdout` 写响应；**stderr 只用于日志**（严禁污染 stdout）。
- 帧格式：一行一个 JSON-RPC 2.0 对象（`\n` 结尾，UTF-8）；忽略空行；单行上限 **4 MiB**，超限回 `-32700` 并**丢弃到下一个换行**，下一帧仍可解析（不崩溃）。
- 并发（集成收口 B4 修订）：输入侧必须持续可接收；到 AgentBridge 的业务仍**单请求在途**。stdin 读取使用独立读取线程 + 有界增量分帧（Windows 管道与 Linux 均适用），主线程事件循环处理取消与后续请求。
- 队列（B0 冻结常量）：待处理 `tools/call` 最多 **8** 条、累计参数序列化字节最多 **8 MiB**；超限立即以 JSON-RPC error `-32009` 拒绝，不建立 IPC 连接、不静默丢弃。
- 取消：`notifications/cancelled` 按 `params.requestId` 只抑制**本地迟到响应**，不声称能中止 GUI 内已开始的操作；不得误取消下一个请求。未知/重复/过期 id 只记 stderr，不影响其他请求。
- 退出：stdin EOF（含半帧）→ 丢弃未完成输入 → `BridgeClient` 断开 → 进程退出码 0；SIGINT/终止 → 立即退出（不做半写）。
- `id` 区分：无 `id` 字段是通知（不回包）；显式 `"id":null` 仍按请求处理并回 `id:null`（保持既有兼容承诺）。
- 生命周期：**不常驻、不自启 GUI、不重试启动 GUI**。

### 5.3 方法集（JSON-RPC 2.0）

| 方法 | 请求 | 成功结果 | 备注 |
|---|---|---|---|
| `initialize` | `{protocolVersion, capabilities, clientInfo}` | `{protocolVersion:"2024-11-05", capabilities:{tools:{listChanged:false}}, serverInfo:{name:"deeplux-mcp-bridge", version:"<DEEPLUX_VERSION_STRING>"}}` | 忽略客户端版本差异（回服务器版本） |
| `notifications/initialized` | 无 id | 无响应 | 通知，静默 |
| `ping` | `{}` | `{}` | 不访问 GUI |
| `tools/list` | `{cursor?}` | `{tools:[{name, description, inputSchema, annotations:{readOnlyHint, destructiveHint}}]}` | 来源 `ToolSchema`（§5.4） |
| `tools/call` | `{name, arguments}` | `{content:[{type:"text", text:"<tool result JSON>"}], isError:bool}` | 转发 AgentBridge（§5.5） |
| `notifications/cancelled` | `{requestId, reason?}` | 无响应 | 取消在途请求（本地丢弃迟到响应） |
| 未知方法 | —— | error `-32601` | `"Method not found: <m>"` |
| 非法 JSON | —— | error `-32700`（id=null） | 解析失败 |
| 参数缺失/类型错 | —— | error `-32602` | 如 `tools/call` 缺 `name` |

### 5.4 工具清单来源与映射

- 启动时 `ToolSchema::instance().registerDefaultTools()`（幂等，`ToolSchema.cpp:122-131` 替换语义）→ `allTools()` 得到 **25** 个工具，顺序与 `registerDefaultTools()` 一致（稳定）。
- 每个工具映射：`name`、`description`、`inputSchema = ToolDefinition::toOpenAIFunction()["function"]["parameters"]`（`ToolSchema.cpp` 的 `toOpenAIFunction`，不含 `name/description` 外壳）。
- `annotations`：
  - `readOnlyHint = def.readOnly`
  - `destructiveHint = def.dangerous`
  - 不输出 `idempotentHint`（未知语义不猜）。
- **禁止**在此层过滤工具名/改参数（单源要求）；写工具在 `tools/call` 阶段拒绝（§6），列表仍展示以便调用方知道能力边界并看到拒绝原因。

### 5.5 转发到 AgentBridge v1.0

**连接与寻址**

| 平台 | 候选地址（按顺序尝试） |
|---|---|
| Windows | `\\.\pipe\deeplux_agent`（`AgentBridge.cpp:131`） |
| Linux | `<PathUtils::appDataPath()>/agent.sock`（首选），失败后 `deeplux_agent_<qHash(socketDir) 的 16 进制>`（`AgentBridge.cpp:126-129` 的同一算法） |
| 覆盖 | `DEEPLUX_APP_DATA_DIR` 经 `PathUtils::appDataPath()` 生效 |

> 路径算法在桥进程内**本地实现**（不新增/修改 core 的 `AgentBridge` API），以保持 t20 与 t18/t19 文件不相交；`tests/test_mcpbridge.cpp` 会启动真实 `AgentBridge` 并断言桥的候选列表包含 `serverName()`，防止两份算法漂移。

**认证**

- 启动时 `ConfigManager::instance().initialize()`（读 `<appData>/config/config.json`）→ `agent/bridgeToken`；非空则连接后先发 `{"version":"1.0","type":"auth","id":"mcp-auth","payload":{"token":"<token>"}}`，等待 `result.payload.status=="authenticated"`（超时/失败 → `-32003`）。
- 令牌**不落日志、不回显**；错误信息只说 `"Bridge authentication failed"`。

**`tools/call` → v1.0 映射**

| JSON-RPC | AgentBridge v1.0 |
|---|---|
| 请求 id | `id`（字符串化；数字 id 转字符串，响应时还原为原类型） |
| `name` | `payload.tool` |
| `arguments`（对象） | `payload.params` |
| 固定标记 | `version="1.0"`、`type="tool_call"`、`payload.origin="mcp"` |
| 响应 `type=="result"` | `payload` 即工具结果 → 包成 MCP `content/text`；若结果含 `"error"` 键 → `isError=true` |
| 响应 `type=="error"` | `isError=true`，text=`{"error":"<message>","code":"bridge_error"}` |
| 超过 `agent/mcpBridgeTimeoutMs` | JSON-RPC error `-32002`（见 §5.6） |

- 每行响应必须匹配在途请求 id；迟到/未知 id 丢弃并 `qWarning`。
- 一个进程内**串行**转发（同一时刻仅一个在途请求）；`notifications/cancelled` 只本地丢弃迟到响应，不额外发取消帧（v1.0 无取消语义）。
- 取消记录必须保存"当前请求 id + 已取消 id 集合"；当前请求完成/超时后即失效，不能掩盖下一请求。被取消请求不输出结果行（若实现选择回 `-32800 Request cancelled`，必须作为协议变更单独记录；本轮采用静默抑制）。
- 不使用 `query`/`subscribe`（MCP 本轮只需 tool 能力）。

### 5.6 错误码表（必须逐条实现）

| code | 含义 | 触发 |
|---|---|---|
| `-32700` | Parse error | 非 JSON / 帧超限 |
| `-32600` | Invalid Request | 非对象 / 缺 `jsonrpc:"2.0"` / 缺 `method` |
| `-32601` | Method not found | 未知方法 |
| `-32602` | Invalid params | `initialize/tools/call` 参数缺失或类型错；工具名不在 `ToolSchema` |
| `-32603` | Internal error | 其它异常（含 JSON 序列化失败） |
| `-32001` | GUI not running | 连接失败（`ServerNotFoundError`/`ConnectionRefusedError`/管道不存在），文案 `"DeepLux GUI is not running; start DeepLux first"` |
| `-32002` | Bridge timeout | 连接成功但 `agent/mcpBridgeTimeoutMs` 内无匹配响应 |
| `-32003` | Bridge auth failed | 令牌缺失/错误/认证超时 |
| `-32004` | Permission denied | **保留码**：仅当未来放开 MCP 写工具时使用；本轮写工具按 §6 返回工具级 `isError`（见下） |
| `-32009` | Queue full | 待处理 `tools/call` 超过 8 条或累计参数超过 8 MiB；请求未被转发 |

- **写工具拒绝的形状**（与验收一致）：`tools/call` 返回成功 JSON-RPC 结果，但 `isError=true`、`content[0].text = {"error":"Permission denied: MCP calls are read-only","code":"permission_denied","hint":"Write tools must be executed inside the DeepLux GUI where the user can confirm."}`；**不得**发 JSON-RPC error（保持 MCP 工具错误语义）。
- `-32004` 预留给"未来允许写工具但需要 GUI 确认"的场景，本轮不返回。

### 5.7 配置键

| 键 | 默认 | clamp | 说明 |
|---|---|---|---|
| `agent/mcpBridgeTimeoutMs` | 30000 | [1000, 300000] | 单次 `tools/call` 等待上限（含连接/认证） |
| `agent/bridgeToken` | 空 | —— | 既有键（`AgentBridge.cpp:122`），复用 |

> MCP 只读策略**不做配置开关**（不可被弱化）；若未来需要放开，必须新开任务并改本文。

### 5.8 安全与日志

- stdout 只输出协议帧；所有诊断走 stderr（`qInstallMessageHandler` 或直接 `fprintf(stderr, ...)`），**不得**输出令牌/API Key/图片。
- 不实现任意文件读写、shell、HTTP；除本机 `QLocalSocket` 外无网络出口。
- 单帧/队列上限：入站帧 4 MiB；待处理 `tools/call` 8 条且累计参数 8 MiB；到 AgentBridge 在途请求 1（串行）。超限拒绝而不是无限排队。
- 连接断开（GUI 退出）→ 在途请求返回 `-32001`；进程不退出（继续等下一个请求）。

### 5.9 测试（`tests/test_mcpbridge.cpp`，t20 新建，链接 `deeplux_mcp`）

用内存流（`QBuffer`）注入 stdin/stdout，用测试内 `QLocalServer` 假桥（或真实 `AgentBridge`）验证：

1. `testInitialize`：返回 `protocolVersion/serverInfo/capabilities`；`notifications/initialized` 无响应。
2. `testToolsList`：25 个工具、名称与 `ToolSchema` 全等；每个含 `description/inputSchema`；`readOnly/dangerous` 映射到 `annotations`。
3. `testPing`：返回 `{}` 且不访问桥（假桥连接计数 0）。
4. `testToolCallForwardFrame`：读只读工具（`get_flow_state`）→ 假桥收到 `{version:"1.0",type:"tool_call",id:...,payload:{tool,params,origin:"mcp"}}`；响应包装成 `content/text`、`isError=false`。
5. `testToolErrorIsError`：假桥回 `{"error":"No project opened"}` → `isError=true`、文本含 `error`。
6. `testWriteToolDeniedAndNotForwarded`：`add_module`/`run_flow`/`save_project` → `isError=true`、`code=="permission_denied"`、含 GUI 确认提示；假桥请求数 **0**（不转发）。
7. `testUnknownTool`：`-32602`。
8. `testGuiNotRunning`：无桥监听 → `-32001`。
9. `testTimeout`：假桥接受连接但不响应 → `-32002`（把 `mcpBridgeTimeoutMs` 调小到 100ms）。
10. `testAuthHandshake`：令牌配置时先发 auth 帧；错误令牌 → `-32003`；正确令牌后工具调用成功。
11. `testParseErrorAndMethodNotFound`：坏 JSON → `-32700`；未知方法 → `-32601`。
12. `testSocketCandidatesMatchBridge`：启动真实 `AgentBridge` 后桥的候选路径包含 `serverName()`（防路径算法漂移）。
13. `testStdoutPurity`：处理一条请求期间 stdout 只出现 1 行合法 JSON-RPC（日志未污染）。
14. `testCancelWhileBridgeHangs`（集成收口 B4）：假桥挂起时发送取消 → 在假桥释放前已消费取消；释放后原结果不输出；随后 ping 正常。用屏障/信号证明消费顺序，不用固定 sleep 碰撞窗口。
15. `testCancelledDoesNotAffectNextRequest`：未知/重复/过期取消不影响其他 id；取消后的下一请求正常返回。
16. `testFramingBoundsAndRecovery`：超长无换行输入缓冲有界并返回 `-32700`；错误帧后合法 `ping` 正常；EOF 半帧安全退出；`id:null` 回包、无 `id` 通知不回包。
17. `testQueueBounded`：注入较小限额，队列满返回 `-32009`，不建立 IPC 连接。
18. `testRealBridgeSubprocess`：启动真实 `deeplux-mcp-bridge` 子进程（非只调用 `processLine`），验证 initialize/ping/只读转发/写拒绝/取消；Linux 与 Windows 分别执行。取消用屏障证明顺序，测试超时只作防挂死保护。

---

## 6. MCP 权限模型（t20，与 §5 同任务）

### 6.1 三层执行路径（外部调用默认 **Observer（只读）**，必须按此实现）

> 口径：MCP 外部调用等价于"Observer 只读"——可调用全部 `readOnly==true` 的工具；任何写/危险工具一律拒绝（`code=permission_denied`）并提示在 GUI 内确认，且**不建立 IPC 连接、不转发**。

```text
MCP client ── tools/call ──> [1] MCP 桥本地预检（ToolSchema.readOnly）
                              │ 非只读 → isError + code=permission_denied，**不建连接/不转发**
                              ▼ 只读
                         [2] AgentBridge（v1.0 tool_call，origin="mcp"）
                              │ 既有认证/心跳/1MB 限制；不新增权限逻辑
                              ▼
                         [3] AgentController::handleToolCall（唯一权威判定）
                              │ busy gate + toolAccess/requiresConfirmation + 执行 + 审计
                              ▼
                         工具结果（读操作）
```

- **[1] 本地预检**：`!ToolSchema::findTool(name).readOnly` → 拒绝。未知工具 → `-32602`（不是 `permission_denied`）。
- **[2] AgentBridge**：保持 v1.0 语义不变；`origin="mcp"` 仅作标记与排障（不参与授权，避免客户端伪造标记绕过）。
- **[3] AgentController**：仍独立执行 `toolAccess`/`requiresConfirmation`（`AgentController.cpp:872-883`）；因为只读了只读工具，正常路径不会触发确认；若 GUI 处于 `Busy/Confirming` 则按既有语义返回 busy/等待——MCP 客户端得到 `isError=true` 与原文。

### 6.2 硬性不变量（不得绕过）

1. **不转发写工具**：桥进程在本地拒绝，**禁止**为"先转发再让 GUI 拒绝"而建立连接（测试 `testWriteToolDeniedAndNotForwarded` 断言假桥请求数 0）。
2. **不直连 AgentActor**：桥不链接/不调用 `AgentActor`、不经 `AgentController` 以外的入口执行工具。
3. **不新增 GUI 确认通道**：本轮不实现"外部请求 → GUI 弹窗确认"；提示语引导用户在 GUI 内操作（hint 文案见 §5.6）。
4. **不可配置放开**：无 `agent/mcpAllowWrite` 之类开关（§5.7）。
5. **审计留痕**：被拒绝的调用必须在 GUI 侧也可见吗？——**不**：不转发的请求不会到 GUI，因此不产生 `AgentActionLogEntry`；桥进程在 stderr 记一行 `mcp write tool denied: <name>`。这是"不转发"的必然结果，测试按此断言。
6. **令牌仍生效**：配置 `agent/bridgeToken` 后，MCP 桥必须完成认证才能调用（§5.5），否则 `-32003`。

### 6.3 测试断言（并入 `test_mcpbridge.cpp`）

- `testWriteToolDeniedAndNotForwarded`（§5.9 第 6 条）同时验证：`permission_denied`、hint 含 "GUI"、无 IPC。
- `testReadOnlyToolsAllowed`：全部 `readOnly==true` 的工具（`validate_workflow/get_node_runtime_outputs/get_execution_logs/capture_viewport_image/export_node_output` + 5 个既有只读）都被允许转发（用假桥逐个断言工具名）。
- `testControllerStillAuthoritative`：GUI 处于 `Thinking` 时转发只读工具 → 桥收到 `{"error":"Agent is busy processing another request"}` → MCP `isError=true`（证明 [3] 仍生效，未被桥短路径绕过）。
- `testUnknownToolNotPermissionDenied`：未知工具名 → `-32602`，不是 `permission_denied`。

### 6.4 与 AgentBridge 既有外部客户端的关系

- 既有 `AgentBridge` 直连客户端（CLI/脚本）语义**不变**：仍按 GUI 当前权限级别处理（含 `pending_confirmation`），L8 只对等待类工具强制异步（§7.5）。
- MCP 桥是**更严格的独立外部面**：只读 + 无确认通道。

---

## 7. 遗留修复契约（t19）

> 所有修复都必须保持第一迭代已交付语义；`ctest -R "test_agent|test_agentbridge|test_runengine"` 全绿是先决条件。

### 7.1 L1：`tune_node_and_verify` 超时/中止/断点/重入分支补自动化测试

**现状**：分支已实现（`AgentActor.cpp:1689-1702`），但测试夹具无法把可注入慢模块放进工程节点路径，故只能真实覆盖 `run_flow` 的超时。

**方案（测试注入点，生产零行为变化）**：

- `AgentActor.h`：把私有 `struct RunWaitResult` 与 `runAndWait` 声明移入 `DEEPLUX_TEST_VISIBILITY` 区（测试构建 public，生产仍 private）；新增：

```cpp
DEEPLUX_TEST_VISIBILITY :
    void setRunWaitResultForTest(const std::optional<RunWaitResult>& result);
    std::optional<RunWaitResult> m_runWaitResultForTest;
```

- `AgentActor::runAndWait` 首行：`if (m_runWaitResultForTest) return *m_runWaitResultForTest;`（**仅**测试构建可见的钩子；生产代码路径不变）。
- 断言点（`tests/test_agentactor.cpp` 追加）：

| 用例 | 注入 | 断言 |
|---|---|---|
| `testTuneTimeoutReturnsStructuredError` | `timedOut=true` | `code=="timeout"`、`success==false`、`before/after` 均存在（参数不回滚）、错误含 `did not finish` |
| `testTuneAbortedReturnsCancelled` | `aborted=true` | `code=="cancelled"` |
| `testTuneReentrantReturnsReentrantWait` | `reentrant=true` | `code=="reentrant_wait"` |
| `testTunePausedAtBreakpoint` | `pausedAtBreakpoint=true` | `code=="paused_at_breakpoint"` |
| `testTuneRunFailedCarriesFailedModule` | `finished=true, ok=false, failedModule="MeasureCircle_1"` | `code=="run_failed"`、`failedModule` 正确 |

**不变量**：注入只在测试构建存在；生产构建无该符号可写路径（`DEEPLUX_TEST_VISIBILITY` 展开为 `private`，`Platform.h:40-42`）。超时/中止后 `engine.isBusy()==false` 的既有保证不变。

### 7.2 L2：`unsupportedPorts` 断言

**现状**：`serializePortValue` 的 `default: return false`（`AgentActor.cpp:255-257`）把不支持类型归入 `unsupportedPorts`（`:265-276`），但无真实生产者。

**方案（单元级，覆盖契约分支）**：

- `AgentActor` 增测试可见静态包装：`DEEPLUX_TEST_VISIBILITY static QJsonObject serializePortsForTest(const PortValueMap& ports);`
  实现：调用匿名命名空间的 `serializePortOutputs`，返回 `{"outputs": {...}, "unsupportedPorts": [...]}`。
- 断言点（`tests/test_agentactor.cpp`）：

| 用例 | 输入 | 断言 |
|---|---|---|
| `testSerializePortsUnsupported` | `{"image": ImageData(4x3), "count": 3, "flag": true, "name": "abc", "list": QVariantList{1,2}, "rect": QRect}` | `outputs.image.type=="image"`、`count==3`、`flag==true`、`name=="abc"`、`list.type=="list" && list.count==2`；`unsupportedPorts == ["rect"]`（排序） |
| `testSerializePortsImageInvalid` | 无效 `ImageData` | `image.present==false` |
| `testSerializePortsStringTruncated` | 600 字符字符串 | `type=="string"`、`truncated==true`、`value` 长度 512 |
| `testGetNodeRuntimeOutputsUnsupportedIntegration`（可选） | 真实 `GrabImage` + 手工向引擎注册测试模块产出 `QRect` 端口 | `unsupportedPorts` 非空（若夹具不可行则跳过，不算失败） |

**不变量**：`get_node_runtime_outputs` 输出形状不变（`outputs`/`unsupportedPorts` 键名与顺序，`:1499-1512`）。

### 7.3 L3：视觉能力模型名启发式

**方案**：

- `OpenAILLMClient.h` 增 `static bool heuristicSupportsVision(const QString& model);`（纯函数，无网络）。
- 规则（与 2A 设计 §6.6 一致）：
  - `true`：`/gpt-4o|gpt-4\.1|claude-3|claude-4|gemini|qwen-vl|qwen2\.5-vl|glm-4v|internvl|llava|pixtral/i`
  - `false`：`/deepseek-(chat|reasoner)|o1-mini|text-davinci/i`
  - 其它：`true`（保守开放，靠 400 自动降级兜底）
- 接线：`MainWindow::loadAgentSettings()`（`MainWindow.cpp:6140-6188`）当 `!cfg.containsGroupKey("agent","visionEnabled")` 时用 `heuristicSupportsVision(model)` 注入 `client->setVisionEnabled(...)`，**不写回配置**（避免把猜测变成用户设置）。
- 断言点：
  - `tests/test_agentretry.cpp`：`testVisionHeuristicCommonModels`（gpt-4o/claude-3-5→true、deepseek-reasoner/o1-mini→false、unknown→true）。
  - 既有 `testVisionDowngradeHint`（R5）保持绿（降级标志与启发式互不覆盖：显式配置优先；降级仍可清）。
- 不变量：`supportsImageInput()` 语义不变（`m_visionEnabled && !m_visionDowngraded`，`OpenAILLMClient.h:27-30`）。

### 7.4 L5：RunEngine 快照加锁收口（明确锁范围、不变量、TSan 边界）

**现状（§2.4）**：读侧已加锁（`RunEngine.cpp:1918-1943`），**写侧 7 处未加锁**（`:769/:826/:1040/:1355/:1583/:1721/:1841`），执行线程只持"租约标志"而非全程持锁 ⇒ 仍有 data race。

**修复**：

1. **写侧**：上述 7 处 `m_nodeOutputs` 修改（含 `clear/remove/operator[]`）统一加 `QMutexLocker locker(&m_lifecycleMutex);`。`m_lifecycleMutex` 是 `mutable QRecursiveMutex`（`RunEngine.h:274`），允许在已持锁路径内重入，**禁止**改用新的非递归锁。
2. **读侧**：保持现状（`nodePortOutputs/lastFrameId` 在 `m_lifecycleMutex` 内拷贝；`moduleRuntimeSnapshot` 在 `m_runtimeInfoMutex` 内拷贝）。执行线程内部读（`:1230/:1565/:1820`）不加锁——它们只在执行线程内、与外部读是读-读关系；**若**实现时发现某读点可能被 GUI 线程并发调用，则同样加 `m_lifecycleMutex`。
3. **前置条件**：`!isBusy()` 仍由调用方（工具层）保证；API 内部保留防御性检查并返回空/`0`（`RunEngine.cpp:1919-1922`、`:1928-1931`、`:1937-1940`）。**TOCTOU 语义**：检查与取锁之间若运行开始，读取仍安全（锁内拷贝），可能读到"上一次运行"的快照——这是可接受语义，文档化为"快照可能滞后，不保证实时"。
4. **`m_runtimeInfoMutex`**：写点只有 `recordModuleRuntime`（`:1913-1916`）与清理（`:828-829`），已加锁；保持不变。

**不变量（reviewer 逐条核）**：

| 编号 | 不变量 |
|---|---|
| L5-I1 | 任何对 `m_nodeOutputs` 的**修改**必须在 `m_lifecycleMutex` 临界区内 |
| L5-I2 | 对外快照 API 必须在锁内**拷贝后返回**，禁止返回内部引用/指针 |
| L5-I3 | 锁顺序：`m_lifecycleMutex` → `m_runtimeInfoMutex`；禁止反向嵌套（防死锁） |
| L5-I4 | 执行线程持租约期间不得长时间持 `m_lifecycleMutex`（写点临界区只含拷贝/赋值，不做模块执行） |
| L5-I5 | 既有公开语义与 `validateFlow` 文案不变；`test_runengine` 全绿 |

**TSan 边界（明确延后）**：本轮**不**把 ThreadSanitizer 纳入 CI（无 TSan 环境、误报噪声历史见 `docs/baseline/tsan-report.md`）。本轮验收 = 代码层"读写均在同锁下 + 拷贝返回"+ 并发压力用例（`tests/test_runengine.cpp` 追加 `testSnapshotConcurrentWithRun`：快速模块循环运行 100 次，同时 1000 次读取快照，断言无崩溃、忙时返回空、停止后快照非空）。TSan 门禁列入第三迭代（roadmap 第三迭代条目）。

### 7.5 L8：外部 `tool_call` 的等待类工具强制异步

**问题**：`AgentBridge` 直连客户端调用 `run_flow(wait=true)`/`tune_node_and_verify(runMode=flow)` 时，v1.0 请求会阻塞到运行结束，可能超过客户端超时（设计文档附录 D 第 5 条 / roadmap L8）。

**方案（AgentBridge 侧，最小侵入）**：`AgentBridge::handleToolCall`（`AgentBridge.cpp:424-439`）在调用回调前改写参数：

```cpp
if (toolName == "run_flow")
    params.insert("wait", false);                 // 外部一律异步
else if (toolName == "tune_node_and_verify")
    params.insert("runMode", "node");             // 外部不跑流程，只回读
```

- 仅影响 `AgentBridge` 路径（GUI 内 Agent 的 LLM 循环不走这里）。
- 配置回滚开关：`agent/bridgeForceAsyncWaitTools`（默认 **true**）；为 false 时保持旧行为（仅用于排障，默认安全）。
- 响应形状不变（工具自身返回 `status=="started"` / `executed==false`）；stderr/`Logger` 记一行 `bridge forced async: <tool>`。
- 断言点（`tests/test_agentbridge.cpp` 追加）：

| 用例 | 断言 |
|---|---|
| `testBridgeForcesRunFlowAsync` | 回调捕获 `params["wait"]==false`；用真实回调（`AgentController::handleToolCall`）时返回 `status=="started"` 且 `RunEngine::isBusy()` 不因等待而阻塞（返回耗时 < 500ms） |
| `testBridgeForcesTuneNodeMode` | 回调捕获 `params["runMode"]=="node"` |
| `testBridgeForceAsyncCanBeDisabled` | 配置 `agent/bridgeForceAsyncWaitTools=false` → 参数原样（`wait` 未注入） |
| `testBridgeOtherToolsUntouched` | `get_flow_state`/`add_module` 参数不被改写 |

**不变量**：不改 `AgentController` 的工具契约；不改 `run_flow`/`tune` 的 schema 默认值（仍是 `wait=true`/`runMode=flow`）；外部调用语义变更是**明示行为变更**（记录于 §11 变更清单）。

### 7.6 L9：单轮全局 deadline

**方案**：

- 配置 `agent/turnDeadlineMs`：int，默认 **600000**（10 分钟），clamp [10000, 3600000]。
- `AgentController` 新增 `QElapsedTimer m_turnTimer;`（或 `qint64 m_turnStartedMs`）+ `int turnDeadlineMs() const`（ConfigManager 读取 + clamp）。
- 起点：`sendUserMessage` / `sendUserMessageWithImages` 成功进入 `Thinking` 后 `m_turnTimer.restart()`。
- 检查点（三处，任一超时即收口）：
  1. `extendAgentLoop` 入口（执行工具批之前，`:586-604` 区域）：超时 → 不执行本批，调用统一收口；
  2. `extendAgentLoop` 的 `executeTools` 返回后（等待类工具可能跨过 deadline，`:625-647`）；
  3. 续跑 LLM 请求之前（`:661-672`）。
- 收口：抽出私有 `void endTurnWithReason(const QString& reason)`（`abortCurrentTurn` 与 deadline 共用）：
  `++m_turnGeneration`；清 pending；`m_dispatchQueued=false`；`m_llmClient->abortRequest()`；`m_actor->abortRunningTool()`；`transitionTo(Idle)`；`emit llmResponseReceived(reason, {})`；`emit agentTurnAborted()`。
  - `abortCurrentTurn` 传入 `"Stopped by user."`（文案不变，`:252`）；
  - deadline 传入 `"已超过单轮总时长上限（%1 ms），本轮已中止"` → `emit llmErrorOccurred` 同一句（UI 可见）。
- 幂等：`endTurnWithReason` 在 `state()==Idle` 时直接返回；重复触发不发二次信号。
- 断言点（`tests/test_agentcontroller.cpp` 追加）：

| 用例 | 断言 |
|---|---|
| `testTurnDeadlineAbortsContinuation` | 设 `agent/turnDeadlineMs=1` + Mock 客户端；第一轮工具批后 sleep 5ms → 续跑前触发：`state()==Idle`、`llmErrorOccurred` 1 次含"单轮总时长"、`sendRequest` 未被再次调用 |
| `testTurnDeadlineResetsPerUserMessage` | 超时中止后再次 `sendUserMessage` → 计时重置、正常进入 Thinking |
| `testTurnDeadlineClamp` | 配置 1 → 实际按 10000（通过测试可见 getter 断言）；配置 10^9 → 3600000 |
| `testTurnDeadlineDoesNotFireEarly` | 默认配置 + 快速两轮工具循环 → 不触发、正常结束 |

**不变量**：20 轮/10 calls 上限不变（deadline 是**额外**上界）；`abortCurrentTurn` 对外行为不变（文案/信号顺序不变）。

---

## 8. 文件所有权与串行规则

### 8.1 所有权表（本迭代唯一有效版本）

| 任务 | 独占文件 |
|---|---|
| **t18 审计 + 失败报文** | `src/core/agent/AgentAuditLog.h`、`.cpp`（新增）、`src/core/agent/FailedRequestLog.h`、`.cpp`（新增）、`src/core/agent/AgentController.h`、`.cpp`（**仅** `initialize/shutdown` 的审计挂接，约 4 行）、`src/core/agent/OpenAILLMClient.h`、`.cpp`（**仅**失败报文钩子与 `m_lastMessages` 成员）、`src/core/CMakeLists.txt`（**仅**注册 4 个新文件：`AgentAuditLog.{h,cpp}`、`FailedRequestLog.{h,cpp}`）、`tests/test_agentaudit.cpp`（新增）、`tests/test_agentfailedrequest.cpp`（新增）、`tests/CMakeLists.txt`（第一批：两个新测试） |
| **t20 MCP 桥** | `src/app/mcp/**`（新增：`main.cpp`、`McpServer.{h,cpp}`、`JsonRpc.{h,cpp}`、`BridgeClient.{h,cpp}`）、`src/app/CMakeLists.txt`（**仅**追加 `deeplux_mcp` 静态库 + `deeplux-mcp-bridge` 目标 + install）、`tests/test_mcpbridge.cpp`（新增）、`tests/CMakeLists.txt`（第二批：`test_mcpbridge.cpp` + 条件链接 `deeplux_mcp`） |
| **t19 遗留修复** | `src/core/engine/RunEngine.h`、`.cpp`（L5 写侧加锁）、`src/core/agent/AgentActor.h`、`.cpp`（L1/L2 测试钩子）、`src/core/agent/AgentBridge.cpp`（L8 参数改写；**不改** `.h` 除非加配置读取，优先只改 `.cpp`）、`src/core/agent/AgentController.h`、`.cpp`（L9 deadline；在 t18 完成后接手）、`src/core/agent/OpenAILLMClient.h`、`.cpp`（L3 启发式；在 t18 完成后接手）、`src/ui/views/MainWindow.cpp`（L3 接线，t13 完成后接手）、`tests/test_agentactor.cpp`、`tests/test_agentcontroller.cpp`、`tests/test_agentbridge.cpp`、`tests/test_runengine.cpp`、`tests/test_agentretry.cpp` |

**只读（任何实现任务不得修改）**：`src/core/agent/ToolSchema.{h,cpp}`、`src/core/interface/**`、`src/plugins/**`、根 `CMakeLists.txt`、`docs/**`（architect 负责）、`src/core/agent/AgentRunWaiter.{h,cpp}`（除非架构师书面授权）。

### 8.2 串行规则（必须遵守）

1. **t18 → t19**：计划已定 t19 依赖 t18；同一批文件（`AgentController`、`OpenAILLMClient`）由 t18 先改、t19 在 t18 `completed` 后接手。t19 开工前 `grep -n "AgentAuditLog\|FailedRequestLog" src/core/agent/AgentController.cpp src/core/agent/OpenAILLMClient.cpp` 确认已落地。
2. **t20 与 t18/t19 并行**：t20 只改 `src/app/**` + 自己的测试；**不得**改 core。唯一共享是 `tests/CMakeLists.txt`：
   - t18 先加两行；t20 在其 `completed` 之后追加（若并行窗口重叠，t20 必须重新读取文件后再编辑，禁止同时写）；
   - 若冲突，t20 无条件等 t18 完成后再改该文件。
3. **`src/core/CMakeLists.txt`**：只有 t18 可改，且只允许新增 4 行（两个新类的 `.cpp`/`.h`）；reviewer 按 diff 行数审查。
4. **`src/app/CMakeLists.txt`**：只有 t20 可改；diff 只允许新增目标/install 块，不得改既有 `DeepLux` 目标。
5. **`RunEngine.{h,cpp}`**：只有 t19 可改（L5）；diff 只允许 7 处写点加锁 + 测试注释，不得改语义/签名。
6. **跨任务变更**：需要改对方文件时先报 architect；不得越界。

### 8.3 禁改清单

- 根 `CMakeLists.txt`、`src/plugins/**`、`IModule.h`、`ToolSchema.*`、`src/core/agent/AgentRunWaiter.*`。
- `AgentBridge` v1.0 报文形状/认证语义/心跳（L8 只改 `handleToolCall` 的参数改写）。
- `AgentActionLogEntry` 既有字段（只允许新增；t18 不需要新增）。
- 既有测试断言（只允许**追加**用例；`test_agentbridge.cpp` 的 `#define private public` 兼容写法保留）。

---

## 9. 测试清单与验证

### 9.1 新增/修改测试

| 文件 | 归属 | 内容 |
|---|---|---|
| `tests/test_agentaudit.cpp`（新增） | t18 | §3.9 的 8 条 |
| `tests/test_agentfailedrequest.cpp`（新增） | t18 | §4.6 的 8 条 |
| `tests/test_mcpbridge.cpp`（新增） | t20 | §5.9 的 13 条 + §6.3 的 3 条 |
| `tests/test_agentactor.cpp`（追加） | t19 | L1 5 条 + L2 3~4 条 |
| `tests/test_agentcontroller.cpp`（追加） | t19 | L9 4 条 |
| `tests/test_agentbridge.cpp`（追加） | t19 | L8 4 条 |
| `tests/test_runengine.cpp`（追加） | t19 | L5 并发快照 1 条 |
| `tests/test_agentretry.cpp`（追加） | t19 | L3 启发式 1 条 |

### 9.2 验收映射

| 验收条目 | 落点 |
|---|---|
| 审计路径/轮转/10MB/14 天/字段映射/写失败不影响对话 | §3 + `test_agentaudit` |
| 失败报文触发类别/文件名/大小/脱敏 | §4 + `test_agentfailedrequest` |
| MCP 目标与放置/stdio 方法集/工具来源/转发映射/token/错误码 | §5 + `test_mcpbridge` |
| MCP 权限（外部只读/写拒绝/不绕过控制器） | §6 + `test_mcpbridge`/`testControllerStillAuthoritative` |
| 六项遗留修复方案/文件/断言（L5 锁范围+不变量+TSan 边界） | §7 |
| 文件所有权与测试清单 | §8/§9 |
| 硬不变量（ABI/依赖/根 CMake/schema） | §1.2 |
| 与执行计划一致 | §1.3 |

### 9.3 验证命令（t21 复验用）

```powershell
# 改过头文件 → 先删对应 obj（第二迭代纪律，见 roadmap §8.1）
$env:PATH = "D:\deps\Qt\bin;D:\deps\opencv\build\x64\vc16\bin;" + $env:PATH
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul && "…\cmake.exe" --build build-win-tests -j 8'
& "…\ctest.exe" --test-dir build-win-tests -R "test_agent" --output-on-failure
& "…\ctest.exe" --test-dir build-win-tests -R "test_runengine" --output-on-failure
& "…\ctest.exe" --test-dir build-win-tests -R "test_mcpbridge" --output-on-failure
& "…\ctest.exe" --test-dir build-win-tests --output-on-failure
```

- 基线：`test_agent*` 12 个 + `test_runengine` 1 个；本轮新增 3 个测试文件（`test_agentaudit`、`test_agentfailedrequest`、`test_mcpbridge`）。
- 插件 ABI 未变 ⇒ 无需 `sync-plugins`。
- `test_mcpbridge` 需 `TIMEOUT 120`（含超时/认证用例）。

---

## 10. 实施顺序与风险

### 10.1 顺序

```text
t18-a  AgentAuditLog + 8 条测试（独立，可先跑）
t18-b  FailedRequestLog + OpenAILLMClient 钩子 + 8 条测试
t18-c  AgentController initialize/shutdown 挂接 + 回归 test_agent
       ──t18 completed──
t19-a  L5 RunEngine 写侧加锁 + 并发快照测试 + test_runengine
t19-b  L8 AgentBridge 强制异步 + test_agentbridge
t19-c  L1/L2 AgentActor 测试钩子 + 用例
t19-d  L3 启发式 + MainWindow 接线
t19-e  L9 deadline + test_agentcontroller
t20    （与 t18/t19 并行）MCP 桥 + test_mcpbridge；tests/CMakeLists 等 t18 completed 后追加
t21    清 obj 全量重建 + agent/runengine/mcp/全量
t22    合并 diff 审查（基线 af56b5b）
t16    文档回填 + 提交推送（队长）
```

### 10.2 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 审计写盘阻塞 GUI（同步 IO） | 卡顿 | 单条 ≤16KiB、频率低；失败 3 次自禁用；后续可改异步队列（第三迭代） |
| 失败报文泄露密钥/图片 | 安全事故 | §4.4 五条规则 + 落盘自检丢弃 + 单测断言无 `sk-`/Bearer/base64 |
| 轮转/保留误删用户文件 | 数据丢失 | 文件名严格正则匹配 + 只删审计/失败报文命名；测试覆盖 |
| MCP 桥链接 core 拉起 Widgets/OpenCV | 启动变慢/依赖增大 | 不实例化 GUI；文档声明；若不可接受，第三迭代抽 `deeplux_agent_api` 轻量库 |
| 桥路径算法与 AgentBridge 漂移 | 无法连接 | 桥内本地实现 + `testSocketCandidatesMatchBridge` 交叉断言 |
| 写工具被意外转发 | 安全 | 本地预检 + 假桥请求数 0 断言 + §6.2 不变量 |
| L5 加锁引入死锁 | 引擎卡死 | 只用 `QRecursiveMutex`；锁序固定；临界区仅拷贝；全量测试 + 并发用例 |
| L8 改写参数导致既有客户端行为变化 | 兼容 | 明示行为变更 + `agent/bridgeForceAsyncWaitTools` 回滚开关 + 4 条测试 |
| L9 误杀长工具（run_flow wait） | 体验 | 默认 10 分钟；deadline 与工具 timeout 叠加；错误文案明确 |
| 改头文件不重编（构建坑） | 假通过 | 沿用 roadmap §8.1：改 `.h` 先删 `build-win-tests/src/core/CMakeFiles/deeplux_core.dir/agent/<Name>.cpp.obj` |

---

## 11. 附录

### 附录 A：配置键总表（本迭代新增/复用）

| 键 | 类型 | 默认 | clamp | 引入 |
|---|---|---|---|---|
| `agent/auditEnabled` | bool | true | — | §3.7 |
| `agent/auditMaxFileBytes` | int | 10485760 | [1048576, 104857600] | §3.7 |
| `agent/auditRetentionDays` | int | 14 | [1, 365] | §3.7 |
| `agent/failedRequestLogEnabled` | bool | false | — | §4.5（B5 改为默认关闭） |
| `agent/failedRequestMaxBytes` | int | 1048576 | [65536, 16777216] | §4.5 |
| `agent/failedRequestRecordMaxBytes` | int | 262144 | [16384, 1048576] | §4.5 |
| `agent/mcpBridgeTimeoutMs` | int | 30000 | [1000, 300000] | §5.7 |
| `agent/bridgeToken` | string | 空 | — | 既有（`AgentBridge.cpp:122`） |
| `agent/bridgeForceAsyncWaitTools` | bool | true | — | §7.5 |
| `agent/turnDeadlineMs` | int | 600000 | [10000, 3600000] | §7.6 |

### 附录 B：MCP 报文示例

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_flow_state","arguments":{}}}
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"modules\":[],\"connections\":[]}"}],"isError":false}}
{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"add_module","arguments":{"plugin":"GrabImage"}}}
{"jsonrpc":"2.0","id":2,"result":{"content":[{"type":"text","text":"{\"error\":\"Permission denied: MCP calls are read-only\",\"code\":\"permission_denied\",\"hint\":\"Write tools must be executed inside the DeepLux GUI where the user can confirm.\"}"}],"isError":true}}
```

### 附录 C：代码锚点核对清单（12 处，按 `af56b5b` 复核）

> 核对方法：`git show af56b5b:<path>` 取基线内容逐行核对；工作树因 t12/t13 正在改动而漂移，实现前一律用 `git grep -n "<符号>"` 重新定位（下表同时给出符号名）。

| # | 位置 | 实测内容（截取） |
|---|---|---|
| 1 | `src/core/agent/GuiEvent.h:76-86` | `struct AgentActionLogEntry`（含 `durationMs/errorCategory/toolCallId`） |
| 2 | `src/core/agent/AgentController.cpp:90-103` | `makeAuditEntry()`（`_elapsedMs` 剥离、`undoable` 按注册表） |
| 3 | `src/core/agent/AgentController.cpp:217-219` | `logAction()` 仅 `emit actionLogEntryAdded` |
| 4 | `src/core/platform/PathUtils.cpp:10-26` / `:40-44` | `appDataPath()`（`~/.deeplux`，`DEEPLUX_APP_DATA_DIR` 覆盖）/`logPath()` |
| 5 | `src/core/agent/AgentBridge.h:117` | `PROTOCOL_VERSION = "1.0"` |
| 6 | `src/core/agent/AgentBridge.cpp:124-168` | 套接字/管道路径（Windows `\\.\pipe\deeplux_agent`） |
| 7 | `src/core/agent/AgentBridge.cpp:265-323` | `processMessage`（tool_call/query/ping/auth；execute 拒绝） |
| 8 | `src/core/agent/AgentBridge.cpp:424-439` | `handleToolCall`（`payload.tool/params`，回调缺省错误） |
| 9 | `src/core/agent/AgentConnection.cpp:35-44` / `:73-98` | 行分隔 JSON 发送/解析（1MB/8MB 上限 `:10-13`） |
| 10 | `src/core/engine/RunEngine.cpp:1918-1943` | 快照 API 读锁；写点无锁（`:769/:826/:1040/:1355/:1583/:1721/:1841`） |
| 11 | `src/core/agent/AgentActor.cpp:1409-1445` / `:1689-1702` | `runAndWait` 与 tune 的错误映射 |
| 12 | `src/core/agent/AgentActor.cpp:209-276` | 端口序列化与 `unsupportedPorts` |

（另核对：`src/app/CMakeLists.txt:8-10`/`:44-46`、根 `CMakeLists.txt:43`/`:152`、`AgentController.cpp:862-893`（外部入口）、`MainWindow.cpp:6140-6188`（视觉配置）、`OpenAILLMClient.h:27-30`/`.cpp:104-110`、`tests/test_agentbridge.cpp:17-222`。）

### 附录 D：本迭代明示行为变更

| # | 变更 | 影响面 | 回滚 |
|---|---|---|---|
| 1 | 外部 `AgentBridge` 的 `run_flow` 强制 `wait=false`、`tune_node_and_verify` 强制 `runMode="node"`（L8） | 仅外部 IPC 客户端 | `agent/bridgeForceAsyncWaitTools=false` |
| 2 | 单轮新增 10 分钟墙钟 deadline（L9） | GUI 内 Agent 对话 | `agent/turnDeadlineMs` 调大（无关闭开关，属安全上限） |
| 3 | MCP 外部调用只读（新外部面，无既有行为被改） | 新增能力 | 不适用 |

### 附录 E：遗留与后续（第三迭代入口）

1. MCP 写工具经 GUI 确认的通道（`-32004` 预留码）。
2. 审计异步队列/批量刷盘（当前同步小写入）。
3. TSan 纳入 CI（L5 已做代码层加锁，TSan 延后）。
4. MCP 桥的轻量依赖拆分（若拉起 Widgets/OpenCV 不可接受）。
5. 失败报文的自动清理/配额面板（当前按 14 天保留）。
