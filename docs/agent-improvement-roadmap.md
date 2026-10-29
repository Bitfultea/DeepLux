# DeepLux Agent 能力改进路线图（对标 visionflow-studio）

- 状态：**第二迭代已交付**（2026-10-28；t22 审核 pass、t21 复验全绿）；第一迭代交付于 2026-10-27
- 设计文档：[agent-iteration1-design.md](agent-iteration1-design.md)（第一迭代）、[agent-iteration2-design.md](agent-iteration2-design.md)（2A 流式+上下文）、[agent-iteration2b-design.md](agent-iteration2b-design.md)（2B 审计/2C MCP/2D 遗留）
- 基线提交：第二迭代基线 `af56b5b`（第一迭代交付提交）；第一迭代基线 `d381129`（分支 `Windows_work`）
- 对标对象：<https://github.com/agentthink/visionflow-studio>（Halcon Vision Studio，WPF/.NET 10 + Halcon）
- 适用范围：`src/core/agent/**`、`src/core/engine/RunEngine`、`src/ui` 中 Agent 相关部件、`src/app/mcp/**`（第二迭代新增）
- 本文用途：①现状与差距的事实基线；②第一迭代的可执行设计契约；③后续迭代的优先级依据；④第一迭代交付记录（§11）；⑤第二迭代交付记录与第三迭代入口（§12）

---

## 1. 结论摘要

DeepLux 的 Agent 与 visionflow 的 Agent 是两种产品形态：

| | visionflow-studio | DeepLux |
|---|---|---|
| 定位 | **能替代人操作软件的视觉工程师** | **受控的流程编辑助手** |
| 工具面 | 56 个（执行/诊断/图像/通讯/导出） | **25 个**（第一迭代后：19 工程/运行/SAM + 6 闭环/观测） |
| 闭环 | 改参数→执行→读指标→再看图 | `run_flow` 只触发不等待，执行后看不到任何结果 |
| 图像 | 工具产图→自动进入下一轮多模态输入 | 只有用户手工粘贴图片，模型看不到中间结果 |
| 对外 | 内置 MCP Server，Cursor/Claude 可驱动 | 自有 IPC，无外部生态 |
| 治理 | **无确认、撤销不全、任意路径写** | 三级权限 + 危险确认 + 撤销宏 + 审计 |

**关键判断：DeepLux 的差距不在工具数量，而在两个断层——"执行→观测→反馈"的闭环缺失，以及图像闭环与对外暴露面缺失。** 治理能力（权限/确认/撤销）是 DeepLux 的资产，改进过程中不得降级。

第一迭代（本路线图的执行范围）只做三件事，目标是把 Agent 从"能改流程"变成"能看见结果"：

1. **工具契约单源化**（为扩工具做地基，消除 schema/派发/权限三处漂移）；
2. **异步执行底座 + 闭环工具**（`validate_workflow`、`get_node_runtime_outputs`、`get_execution_logs`、`tune_node_and_verify`、`run_flow(wait)`）；
3. **图像闭环**（`capture_viewport_image`、`export_node_output` + 工具产图自动回灌）+ **LLM 错误分类与重试**。

**第一迭代已于 2026-10-27 交付**：上述三项全部落地，`test_agent*` 12/12、全量 ctest 80/80 通过，两轮审核结论均为 pass。实际契约以 [agent-iteration1-design.md](agent-iteration1-design.md) 为准；与本文原计划的偏差、验证证据、变更摘要、遗留问题与第二迭代入口见 **§11**。

**第二迭代已于 2026-10-28 交付**：流式对话 + 上下文治理（2A）、审计落盘 + 失败报文（2B）、MCP stdio 桥（2C）、L1/L2/L3/L5/L8/L9 六项遗留收口（2D）全部落地；`test_agent*` 15/15、`test_mcpbridge` 1/1、全量 ctest 84/84，t22 审核 pass。交付记录、偏差、MCP 使用方式、审计格式与第三迭代入口见 **§12**。

---

## 2. 竞品基线（只记录与本路线图相关的事实）

### 2.1 visionflow 的 Agent 分层

```text
VisionCopilotPanel/ViewModel
  → VisionCopilotEngine            工具循环 / 多模态回灌 / 上下文裁剪
  → VisionToolCatalog (56 tools)   schema + executor 同表
  → IVisionStudioAutomationService 唯一语义门面
  → IVisionStudioContext           宿主适配器（UI 线程编组、抓图、执行并等待）
  → McpServer (HTTP+SSE)           tools/list、tools/call 与 Copilot 同源
```

### 2.2 值得借鉴的机制（附证据）

| 机制 | 位置 | 价值 |
|---|---|---|
| 执行工具"等待真正完成"再返回 | `IVisionStudioContext.cs:47-50` | 闭环验证的前提 |
| 工具产图 → 下一轮自动作为多模态输入 | `VisionCopilotEngine.cs:21-22, 225-249, 311-336` | "看图调参"成立的原因 |
| 纯文本模型自动降级 + 报错自愈 | `VisionCopilotEngine.cs:173-192, 291-303` | 多服务商可用性 |
| 上下文按"工具调用组"整组裁剪 + 单结果 6k 截断留首尾 | `VisionCopilotEngine.cs:343-441` | 长会话不破坏 tool_calls 配对 |
| 流式增量 50ms 合批 | `VisionCopilotEngine.cs:124-169` | UI 不重入、不布局崩溃 |
| 429/5xx 指数退避重试、4xx 带可操作提示 | `OpenAiCompatibleClient.cs:29, 92-159, 459-471` | 模型侧抖动的容错 |
| 单一工具目录 + 单一执行/审计入口 | `VisionToolCatalog.cs:583-1307` | 内外部 Agent 行为一致 |
| 审计含耗时/成败/堆栈、失败请求报文落盘 | `ExecutionLogService.cs:57-58, 151-156`；`OpenAiCompatibleClient.cs:121-124` | 现场可追溯 |

### 2.3 明确不照搬的部分

| 问题 | 证据 | 结论 |
|---|---|---|
| 无权限分级、无危险确认 | 全 AI 项目无确认代码；`comm_write_value`/`clear_workflow` 直执行 | **禁止**降级 DeepLux 的权限模型 |
| 撤销覆盖不全 | 改参数反射直写不入栈；`clear_workflow` 不可撤销 | "靠 undo 兜底"不成立 |
| 限流实现缺陷 | `McpServer.cs:36, 255-260` 与 SSE 长连接占槽 | 若做 MCP，按请求计数而非连接占槽 |
| 令牌明文落盘 | `AiSettings.cs:303-323`（注释称 DPAPI，实现为明文） | 凭据不得明文写盘 |
| 工具契约三处漂移 | schema 漏 `nodeName` 而提示词声称支持（`VisionToolCatalog.cs:984-997` vs `VisionCopilotEngine.cs:44`） | 必须单源 + 契约测试 |
| 24k 上下文预算 | `VisionCopilotEngine.cs:30` | 对长流程偏小，DeepLux 保持可配置的大预算 |

---

## 3. DeepLux 现状（代码核对）

> 本节是**第一迭代开始前**（基线 `d381129`）的事实快照，保留作为差距证据；行号与结论均已随第一迭代失效，交付后的现状见 §11。原文中的"缺陷 D1~D4"已在第一迭代修复（见 §11.1）。

### 3.1 分层与线程

- `AgentController`（单例，四态 `Idle/Thinking/Confirming/Executing`）持有 `AgentObserver`、`AgentActor`、`ILLMClient`，**全部运行在 GUI 线程**（`src/core/agent/AgentController.cpp:174-205`）。
- `AgentActor` 同步调用 `ProjectManager/PluginManager/RunEngine`，写操作包 `QUndoCommand`（`AgentActor.cpp:246-316`）。
- `AgentBridge` 提供本地 IPC（`tool_call/query/subscribe/ping`，明确拒绝 `execute`），可选握手令牌、心跳（`AgentBridge.h:93-108`）。
- `ILLMClient` 抽象存在，但当前只有 OpenAI 兼容实现；`OpenAILLMClient` 非流式、30s 超时、无取消（`OpenAILLMClient.cpp:74, 113`）。

### 3.2 工具与权限

- 19 个工具注册在 `ToolSchema::registerDefaultTools`（`ToolSchema.cpp:129-258`），派发在 `AgentActor::executeTool` 的 `if/else` 链（`AgentActor.cpp:270-312`）。
- **schema 与派发双处维护**；权重信息分散：只读名单硬编码在 `AgentController.cpp:29-34`，`dangerous` 在 schema。
- 参数校验只有 required/enum（`AgentActor.cpp:38-55`），整型/数组范围在 handler 内另判。
- 错误返回 `{"error": "..."}`，批量返回 `partial_failed`。

### 3.3 循环、上下文与可观测

- 单轮上限 20 轮 / 10 calls（`AgentController.cpp:24-25, 455-464`）；超限报错回 Idle。
- 上下文：0.3 token/字符估算、256k 上限、按 user turn 裁剪、硬上限 64 条，无摘要（`AgentController.cpp:535-589`）；工具结果**全量 JSON** 回灌（`:502-507`）。
- `buildContext()` 每轮把工程全部模块名 + 5 条 GUI 事件拼进 system prompt（`AgentController.cpp:209-233`），大工程会膨胀。
- 审计 `AgentActionLogEntry` 仅内存 5 列表格，无落盘、无耗时、无 token 用量展示。
- `run_flow` 先 `loadProject` 再 `runOnceAsync()` 即返回 `{"status":"started","async":true}`（`AgentActor.cpp:582-606`）；`get_run_results` 只有计数（`:689-697`）。**执行结果对模型不可见**。

### 3.4 已确认的缺陷（第一迭代顺手修）

| # | 问题 | 位置 |
|---|---|---|
| D1 | `remove_module` 对不存在的 ID 仍返回 `removed` | `AgentActor.cpp:411-417` |
| D2 | `create/open/save_project` 不入撤销栈，审计 `undoable` 语义与实现不一致 | `AgentActor.cpp:374-381` |
| D3 | `create_project` 失败路径无结构化错误码 | `AgentActor.cpp` 对应 handler |
| D4 | 工具调用无耗时、无 token 用量展示 | `AgentController.cpp:618`；`OpenAILLMClient.cpp:178` |

---

## 4. 差距矩阵（摘录）

| 能力 | visionflow | DeepLux | 第一迭代 |
|---|---|---|---|
| 工具契约 | 手写 schema + 双协议导出（仍会漂移） | 手写 schema + if/else 派发 | ✅ 单源化 + 契约测试 |
| 执行闭环 | `tune`/`sweep`/`validate`/`outputs`/`logs` | 只有触发 | ✅ 5 个闭环工具 |
| 图像闭环 | 工具产图自动回灌 | 仅用户附件 | ✅ 截图/导出 + 回灌 |
| LLM 容错 | 429/5xx 退避重试 | 一次失败即中断 | ✅ 错误分类 + 重试 |
| 权限治理 | 无 | 三级 + 确认 + 撤销宏 | 保持并强化（元数据单源） |
| 上下文 | 24k、组内原子裁剪、6k 截断 | 256k、turn 裁剪、全量回灌 | 第二迭代 |
| 流式/取消 | 流式 + reasoning | 非流式、无取消 | 第二迭代 |
| MCP 外放 | 有 | 无 | 第三迭代 |
| 多服务商 | 6 预设 + 独立 Key | 单 endpoint + 明文 Key | 第三迭代 |

---

## 5. 第一迭代设计契约（团队执行依据）

> **状态（2026-10-27）**：本节已由 [agent-iteration1-design.md](agent-iteration1-design.md) 冻结并全部落地。落地时有三处**有意偏差**（`ToolDefinition` 字段顺序、`tune_node_and_verify` 的 `runMode=node` 语义、`run_flow` 默认 `wait=true`），理由与影响见 §11.2；与本节冲突处一律以设计文档为准。

### 5.1 工具契约单源化

`ToolDefinition` 增加元数据，**权限判定只读 schema**：

```cpp
struct ToolDefinition {
    QString name;
    QString description;
    QList<ToolParam> params;
    bool readOnly = false;      // 新增：只允许观察
    bool dangerous = false;     // 既有：破坏性/写外部资源
    int  timeoutMs = 0;         // 新增：0 = 默认（30s），长任务单独声明
};
```

权限派生规则（替换 `AgentController::isReadOnlyToolName/isDangerousToolName`）：

| 权限级别 | readOnly | 写工具 | dangerous |
|---|---|---|---|
| Observer | 允许 | 拒绝 | 拒绝 |
| Advisor | 允许 | 需确认 | 需确认 |
| Autopilot | 允许 | 直接执行 | 需确认 |

配套：
- `AgentActor` 用 `QHash<QString, Handler>` 注册表派发，替代 `if/else`，并提供 `supportedTools()` 供契约测试交叉校验；
- 契约测试 `tests/test_agenttoolcontract.cpp`：名称唯一、description 非空、required ⊆ properties、`dangerous ⇒ !readOnly`、schema ↔ handler 一一对应、每个工具都在 `supportedTools()` 中。

### 5.2 新增工具契约（6 个）

| 工具 | 参数 | 返回（关键字段） | 权限 |
|---|---|---|---|
| `validate_workflow` | 无 | `{ok, issues:[{code,node,message}]}` | readOnly |
| `get_node_runtime_outputs` | `nodeName` | `{node, outputs:{k:v}, elapsedMs, lastStatus}` | readOnly |
| `get_execution_logs` | `category?`, `level?`, `keyword?`, `limit?` | `{entries:[{time,level,category,message}], truncated}` | readOnly |
| `tune_node_and_verify` | `nodeName`, `parameters{}`, `runMode?`(node/flow) | `{before:{}, after:{}, outputs:{}, elapsedMs, success}` | 写（Advisor 确认） |
| `capture_viewport_image` | 无 | `{imagePath, width, height}` + `imagePath` 触发回灌 | readOnly |
| `export_node_output` | `nodeName`, `format?`(png/jpg) | `{imagePath, width, height}` + 回灌 | readOnly |

约束：
- 图片只写 `QStandardPaths::TempLocation` 下的 `deeplux-agent/` 子目录，文件名由工具生成，**不接受外部路径**（区别于 `sam_annotate` 的显式磁盘写入）；
- `run_flow` 增加 `wait`(bool, 默认 true) 与 `timeoutMs`；`wait=true` 时返回 `{success, elapsedMs, failedModule, errorMessage}`。

### 5.3 异步执行与取消协议

- 工具执行改为排队调用：`confirmPendingTools()` 保留签名，内部 `QMetaObject::invokeMethod(this, [..]{ doExecute(calls); }, Qt::QueuedConnection)`；状态在执行期间保持 `Executing`，期间拒绝新用户消息（既有语义）。
- 新增 `AgentController::abortCurrentTurn()`：中止 LLM 请求（`ILLMClient::abortRequest()`）+ 清空 pending + 回 `Idle`；UI 提供"停止"按钮。
- `run_flow(wait)` / `tune_node_and_verify` 通过 `AgentRunWaiter`（新类，`QEventLoop` + `runFinished` + `QTimer` 超时 + 重入保护）等待，超时返回 `{success:false, reason:"timeout"}`，**不得卡死 UI**。
- 工具执行期间若引擎 `isBusy()`，写工具返回结构化忙碌错误（沿用现有口径）。

### 5.4 图像回灌契约

- 工具结果顶层字段 `imagePath`（单图）被 `AgentController` 收集；每轮最多 2 张，长边压到 ≤1280，总量 ≤8MB。
- 下一轮请求前，若视觉开启：插入 `role=user` 的带图消息（`AgentImageAttachment`，PNG/JPEG）；
- 若视觉关闭：插入文字说明"图像已生成但当前模型不支持图片输入"，并保留路径；
- 视觉能力判定优先级：用户设置 `visionEnabled` > 模型名启发式；遇到 `content.type`/`image_url` 类错误自动降级一次并记住本会话。

### 5.5 LLM 错误分类与重试

```cpp
enum class LLMErrorCategory { None, Auth, RateLimit, Server, Network, BadRequest, Timeout, Malformed };
```

| 类别 | 触发 | 策略 |
|---|---|---|
| Auth | 401/403 | 不重试，提示检查 Key |
| RateLimit | 429 | 退避重试 3 次（0.5s/1s/2s） |
| Server | 5xx | 退避重试 3 次 |
| Network | 连接失败/超时 | 重试 2 次 |
| BadRequest | 400/422 | 不重试，返回可操作提示（含是否疑似上下文超限） |
| Malformed | 响应非法 JSON/无 choices | 不重试 |

`AgentResponse` 增加 `errorCategory`；UI 与审计记录类别。

### 5.6 第一迭代文件所有权（避免并行写冲突）

| 任务 | 独占文件 |
|---|---|
| T2 契约 + 重试 | `src/core/agent/ToolSchema.h/.cpp`、`AgentController.h/.cpp`（权限段）、`ILLMClient.h`、`OpenAILLMClient.h/.cpp`、`tests/test_agenttoolcontract.cpp`、`tests/CMakeLists.txt` |
| T3 工具实现 | `src/core/agent/AgentActor.h/.cpp`、新增 `AgentRunWaiter.h/.cpp` |
| T4 异步 + 图像 | `src/core/agent/AgentController.h/.cpp`、新增 `tests/test_agentclosedloop.cpp`、`tests/CMakeLists.txt`（依赖 T2/T3 完成后） |

---

## 6. 后续迭代（非本次执行范围）

> 第一迭代已交付（2026-10-27）。第二迭代的推荐入口、优先级与依赖见 §11.7；本节其余内容为原始规划，未变。

### 第二迭代：对话质量
1. 流式 SSE + 增量渲染 + 取消/重试（`appendToLastMessage` 已存在未接线）；
2. reasoning 折叠块、token 用量与单工具耗时面板；
3. 工具结果 6k 截断留首尾、按工具组原子裁剪、预算可配；
4. `buildContext()` 改为摘要 + 按需详情（大工程防膨胀）；
5. 审计落盘（JSONL，按天轮转）。

### 第三迭代：对外与配置
1. `DeepLux --mcp-stdio`（JSON-RPC 2.0 over stdio → `AgentBridge.tool_call`），工具来自 `ToolSchema`，**外部调用默认 Observer 或强制确认**；
2. 多服务商预设 + 每 provider 独立 Key（系统凭据存储）+ 模型列表拉取 + 连接测试；
3. `sweep_node_parameter`（依赖第二迭代的执行等待能力）；
4. 方案模板、自动布局（纯 UI 增强）。

### 暂不做
- 代码导出（HDevelop/C# 等价物）；
- 参数扫描并行化；
- 无确认直执行模式。

---

## 7. 排期与角色（第一迭代）

| 阶段 | 角色 | 产出 | 预估 |
|---|---|---|---|
| 需求/设计 | architect | `docs/agent-iteration1-design.md` | 0.5 天 |
| 实现 A（契约+重试） | implementer-core | 契约元数据、权限单源、错误分类重试、契约测试 | 1~1.5 天 |
| 实现 B（工具） | implementer-tools | 派发注册表、6 个工具、`AgentRunWaiter` | 1.5~2 天 |
| 实现 C（异步+图像） | implementer-core | 排队执行、取消、图像回灌、闭环测试 | 1~1.5 天 |
| 验证 | verifier | 构建 + 测试报告 + 回归清单 | 0.5 天 |
| 审核 | reviewer | 代码审查结论（pass / needs_revision + findings） | 0.5 天 |
| 集成 | architect | 文档更新、最终构建与测试、变更摘要 | 0.5 天 |

合计约 5~7 人日（团队并行后墙钟时间更短）。

> **实际执行（2026-10-26 ~ 2026-10-27）**：团队并行单人日量级完成；期间含 2 轮实现修复（t6/t8 findings 7 条）与 2 轮审核（t6 needs_revision → t9 pass），另加 2 轮复验（t5/t10）。角色分工与文件所有权按设计文档 §8 执行，落地时新增 `src/core/CMakeLists.txt` +4 行（队长授权，见 §11.2 第 4 条）。

---

## 8. 验收与测试计划

### 8.1 环境（本机 Windows）

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ctest = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
$env:PATH = "D:\deps\Qt\bin;D:\deps\opencv\build\x64\vc16\bin;" + $env:PATH

# 本机 INCLUDE/LIB 为空：必须先经 vcvars64 注入 MSVC 环境，否则 cl/头文件都找不到
cmd /c 'call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul && "…\cmake.exe" --build build-win-tests -j 8'
& $ctest --test-dir build-win-tests -R "test_agent" --output-on-failure
```

- 配置：`build-win-tests`（Ninja + MSVC，`BUILD_TESTS=ON`，`DEEPLUX_QT_PATH=D:/deps/Qt`）。
- 基线：第一迭代前 77 个测试（9 个 agent）；**交付后 80 个测试，其中 `test_agent*` 12 个**（新增 `test_agenttoolcontract`/`test_agentretry`/`test_agentclosedloop`，`test_agentretry` 设 `TIMEOUT 120`）。
- 插件 ABI 未改动，无需 `sync-plugins`。

> **构建环境坑（第一迭代实测，务必照做，否则会出现"改了不重编"的假通过）**
>
> 1. **必须 vcvars64 注入 MSVC 环境**：本机裸 PowerShell 的 `INCLUDE`/`LIB` 为空，直接调 `cmake --build` 会找不到 `cl` 或系统头文件；必须用 `cmd /c 'call "…\vcvars64.bat" && …'` 包裹。
> 2. **头文件依赖追踪失效**：`build-win-tests` 的 CMake 因中文 `showIncludes` 前缀乱码，头文件依赖实际未被追踪——**改动任何 `.h` 后必须删除对应 `*.obj` 再构建**。obj 实际路径：`build-win-tests/src/core/CMakeFiles/deeplux_core.dir/agent/<Name>.cpp.obj`（定位：`Get-ChildItem build-win-tests -Recurse -Filter <Name>.cpp.obj`）。保守做法：改核心头文件后删整个 `build-win-tests/src/core/CMakeFiles/deeplux_core.dir`（或全部 `*.obj`）再 `cmake --build`。
> 3. **测试需要运行时库 PATH**：`D:\deps\Qt\bin` 与 `D:\deps\opencv\build\x64\vc16\bin` 必须在 `PATH` 中，否则测试进程加载 DLL 失败。
> 4. **不要并发跑构建/测试**：多个 `ctest`/`cmake --build` 会争抢同一个构建目录，导致偶发失败。
>
> 复验证据链要求：凡"改了 `.h`"的任务，报告里必须有"清 obj → 重建 → 测试"的 mtime 证据（头文件 mtime < obj mtime < 二进制 mtime），见 `build-win-tests/verify-t10-report.md`。

### 8.2 验收条目（第一迭代实际结果，2026-10-27）

| # | 验收条目 | 结论 | 证据 |
|---|---|---|---|
| 1 | `test_agenttoolcontract` 通过：schema ↔ handler 一一对应、元数据自洽 | ✅ passed | 16 个用例（名称唯一/描述非空/required⊆properties/类型合法/`dangerous⇒!readOnly`/元数据快照/schema↔`supportedTools()` 双向/新工具注册/`toJsonSchema` 暴露三字段/权限矩阵/注册表 flags） |
| 2 | `test_agentclosedloop` 通过：建流程 → validate → run_flow(wait) → node outputs → tune → capture 断言 `imagePath` 且被回灌 | ✅ passed | 6 个用例：真实 GrabImage 建流程全链路 + schema↔supportedTools + 回灌长边缩放/单消息/审计耗时 + 3 张→2 张限流 + 视觉关闭文本降级 + 停止按钮状态条 |
| 3 | 权限：Observer 拒绝写、Advisor 需确认、Autopilot 仅 dangerous 确认 | ✅ passed | `test_agentpermissions` 7 用例（既有 5 零改动 + 新工具×三级 2 个）；判定唯一来源 `ToolSchema.readOnly/dangerous`，硬编码名单已删除 |
| 4 | 错误分类：429/500 触发重试、401 不重试 | ✅ passed | `test_agentretry` 11 用例（本地 QTcpServer 假端点）：429→200 重试成功、连续 500 重试耗尽、401/400/畸形 200 零重试、abort 取消待触发重试、视觉降级 hint |
| 5 | 全量 `test_agent*` 无回归 | ✅ passed | `ctest -R test_agent` → `100% tests passed, 0 tests failed out of 12`，`Total Test time (real) = 28.51 sec`，exit 0；全量 ctest 80/80（t10 复验） |
| 6 | 文档：设计文档与本文状态更新 | ✅ passed | [agent-iteration1-design.md](agent-iteration1-design.md)（端到端契约）+ 本文 §1/§3/§5/§7/§8/§10 状态更新与 §11 交付记录 |
| 7 | 审核：两轮 review（t6 needs_revision → t9 pass），7 条 findings 全部修复 | ✅ passed | t8 逐条修复（R1~R7）；t10 复验：删 398 obj → `[551/551]` 全量构建 exit 0 → 12/12、80/80 |
| 8 | 回归构建证据（避免"改头文件不重编"假通过） | ✅ passed | t10：头文件 mtime ≤ 23:57:31 < obj 00:04:50–00:07:26 < 二进制 00:04:55–00:07:59；报告 `build-win-tests/verify-t10-report.md` |

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 异步改造引入重入/状态机漏洞 | 高 | 沿用 `AgentController::state()` 作为 busy gate；排队执行只在 `Executing` 中发生；新增状态机测试 |
| `run_flow(wait)` 在 GUI 线程等待 | UI 卡顿 | 事件循环等待 + 超时 + 取消；长任务独立超时 |
| 图像回灌撑爆上下文 | 中 | 2 张/轮、≤1280 长边、≤8MB；非视觉模型降级 |
| 两人并行改同文件 | 中 | 见 5.6 文件所有权；`tests/CMakeLists.txt` 后置给 T4 |
| 契约重构破坏既有 9 个测试 | 中 | 先跑基线；契约测试覆盖全部工具；逐步替换硬编码名单 |

---

## 10. 附录：关键代码位置

> 下表行号为**第一迭代前基线**（`d381129`，核对于 2026-10-26）。交付后这些位置已变化，当前入口建议用 `git grep -n "registerDefaultTools\|supportedTools\|requiresConfirmation\|classifyError\|validateFlowIssues" src/` 定位；第一迭代新增文件的权威清单见 §11.3。

| 主题 | 位置 |
|---|---|
| 工具注册 | `src/core/agent/ToolSchema.cpp:129-258` |
| 工具派发 | `src/core/agent/AgentActor.cpp:246-316` |
| 权限判定 | `src/core/agent/AgentController.cpp:29-34, 352-366, 603-616` |
| 循环与历史 | `src/core/agent/AgentController.cpp:237-260, 449-531, 535-589` |
| LLM 请求 | `src/core/agent/OpenAILLMClient.cpp:68-200` |
| 运行引擎 | `src/core/engine/RunEngine.h:122-160`（runOnce/stepOnce/getOutput/validateFlow） |
| 外部 IPC | `src/core/agent/AgentBridge.h:39-125` |
| 既有设计说明 | `docs/Terminal_Agent_Design.md`、`docs/agent-loop-design-review.md` |

---

## 11. 第一迭代交付记录（2026-10-27）

- 交付日期：**2026-10-27**（实现 2026-10-26 ~ 27，含 2 轮修复与 2 轮审核）
- 质量门：t6 review `needs_revision`（7 findings）→ t8 修复 → t9 review **pass** → t10 复验通过 → 本集成（t7）
- 提交状态：Windows 交付已提交为 `af56b5b`；Linux 集成复验发现的客户端生命周期问题及修复见 §11.4。`.agent-teams/` 为团队运行时状态，不应提交。

### 11.1 交付项（对照 §1 的三件事）

| 计划项 | 交付内容 | 关键文件 | 状态 |
|---|---|---|---|
| ① 工具契约单源化 | `ToolDefinition` 增 `readOnly`/`timeoutMs` + 工厂函数 + `toJsonSchema` 暴露元数据；删除硬编码只读名单，权限判定收敛为 `toolAccess()`/`requiresConfirmation()` 单源；`AgentActor` 改 `QHash` 注册表（25 工具，`mutatesProject`/`undoable`）+ `supportedTools()`/`toolUndoable()`；契约测试 16 用例 | `ToolSchema.h/.cpp`、`AgentController.h/.cpp`、`AgentActor.h/.cpp`、`tests/test_agenttoolcontract.cpp` | ✅ |
| ② 异步底座 + 闭环工具 | 排队执行（`QueuedConnection`）、`abortCurrentTurn()`、`AgentRunWaiter`（等待/超时/重入/断点/取消）；6 工具 `validate_workflow`/`get_node_runtime_outputs`/`get_execution_logs`/`tune_node_and_verify`/`capture_viewport_image`/`export_node_output`；`run_flow` 增 `wait`(默认 true)/`timeoutMs`；`RunEngine` 4 个只读 API（`validateFlowIssues`/`moduleRuntimeSnapshot`/`nodePortOutputs`/`lastFrameId`，`validateFlow` 输出逐字不变） | `AgentActor.cpp`、`AgentRunWaiter.{h,cpp}`、`RunEngine.{h,cpp}`、`tests/test_agentactor.cpp`（31 槽） | ✅ |
| ③ 图像闭环 + LLM 容错 | `imagePath` 回灌（白名单 2 工具、≤2 张/轮、长边≤1280、单张 4MB→JPEG、单轮 8MB 预算、视觉关闭文本降级、视觉错误自动降级重发一次）；`LLMErrorCategory` 7 类 + 重试矩阵（RateLimit/Server 3、Network 2、Timeout 1、Auth/BadRequest/Malformed 0）+ `abortRequest()`；审计 `durationMs`/`errorCategory`/`toolCallId`；UI 停止按钮/四阶段状态/7 列审计/token 用量 | `AgentController.cpp`、`OpenAILLMClient.{h,cpp}`、`ILLMClient.h`、`MainWindow.cpp`、`AgentChatPanel.*`、`AgentActionLogWidget.cpp`、`tests/test_agentclosedloop.cpp`、`tests/test_agentretry.cpp` | ✅ |
| 顺手修 D1~D4 | D1 `remove_module` 缺失实例失败关闭（`code=node_not_found`，不入撤销栈）；D2 审计 `undoable` 改以注册表为事实源；D3 `create_project` 失败带 `code=project_create_failed`；D4 工具耗时采集接通（`_elapsedMs` 消费即剥离）+ token 用量信号 | `AgentActor.cpp`、`AgentController.cpp`、`GuiEvent.h` | ✅ |

### 11.2 与计划的偏差（逐条，含理由与影响）

| # | 原计划（§5） | 实际实现 | 理由与影响 |
|---|---|---|---|
| 1 | `ToolDefinition` 字段顺序 `readOnly/dangerous/timeoutMs` | **`dangerous` 保持第 4 位**，`readOnly`/`timeoutMs` 追加在后 | C++17 按位置聚合初始化：照抄原顺序会让 `remove_module`/`save_project`/`sam_annotate` 的 `true` 落到 `readOnly`，等价于把危险工具降级为只读（治理降级）。已由契约测试锁死 |
| 2 | `tune_node_and_verify(runMode=node)` 表示只运行该节点 | `node` = **零副作用改参核对**（只写参数 + 回填上一帧输出，不执行） | `executeParallel` 同步阻塞调用线程且租约内不可中断，GUI 线程执行会冻结界面；可取消单节点执行列入 §11.7 第 4 项 |
| 3 | `run_flow` 增加 `wait` 默认 true | 同计划（唯一行为性破坏变更） | 既有 `test_agentactor` 的 run_flow 用例显式改 `wait=false` 保原意图，另增 `wait=true` 用例；`test_agente2e` 实测兼容 |
| 4 | 文件所有权表（§5.6） | 新增 `src/core/CMakeLists.txt` **+4 行**（注册 `AgentRunWaiter`/`AgentToolImage` 的 cpp/h），队长授权；新增 4 个源文件 | 新 `.cpp` 不进 `CORE_SOURCES` 会链接缺符号；范围限定为"仅这四行" |
| 5 | 审计 `undoable` 用"非只读且非工程级"推导 | 改为 `AgentActor` 注册表 `ToolHandler::undoable` 单一事实源 | 更贴近"是否真的压入 `QUndoStack`"，失败工具不会被审计成可撤销 |
| 6 | 视觉能力：配置 > 模型名启发式 > 默认 | **模型名启发式本轮未实现**；判定 = 配置 `agent/visionEnabled`（默认 true）+ 400 视觉类错误自动降级一次（客户端 `supportsImageInput() = enabled && !downgraded`） | 时间/收益权衡：自动降级已覆盖"配置与模型能力不符"的主要场景，启发式只省一次失败往返。列为 §11.6 L3 |
| 7 | 契约测试 6 条断言、重试/闭环用例未定规模 | 契约测试 16 用例、重试 11 用例、闭环 6 用例；`test_agentactor` 18→31 槽、`test_agentcontroller` 追加 8 槽 | 逐条覆盖 §9 的断言清单（含 R1~R7 回归断言） |
| 8 | `create_project` 失败返回结构化错误 | 已实现 `code`+`message`，但该失败分支在生产公开路径**不可达**（`mutatesProject` 门禁先返回 `engine_busy`），测试经 `DEEPLUX_TEST_VISIBILITY` 覆盖 | 属防御性代码；契约（`code` 就位）不受影响，见 §11.6 L4 |
| 9 | `export_node_output` 只认节点输出（§5.2 未提及视口） | 实现初期曾加"视口回退"，审查 R3 指出后已**收紧**为 `nodePortOutputs["image"] → moduleOutput`，无图一律 `no_image_output`；`capture_viewport_image` 的 provider→lastOutput 回退按 §4.5 保留 | 回退会掩盖"节点无输出"的真实失败；已由 t8 修正并加"即使注入 provider 也必须失败"的断言 |

### 11.3 变更摘要（按文件/功能）

**修改文件（`git diff --stat`，改动行数）**

| 功能组 | 文件（改动行数） |
|---|---|
| 契约/权限/审计 | `ToolSchema.h`(28)、`ToolSchema.cpp`(150)、`AgentController.h`(40)、`AgentController.cpp`(449)、`GuiEvent.h`(3) |
| 工具实现/派发/引擎 | `AgentActor.h`(93)、`AgentActor.cpp`(1002)、`RunEngine.h`(28)、`RunEngine.cpp`(90)、`src/core/CMakeLists.txt`(+4) |
| LLM 容错 | `ILLMClient.h`(64)、`OpenAILLMClient.h`(38)、`OpenAILLMClient.cpp`(312) |
| UI | `MainWindow.cpp`(65)、`MainWindow.h`(1)、`AgentChatPanel.cpp`(61)、`AgentChatPanel.h`(10)、`AgentActionLogWidget.cpp`(17) |
| 测试 | `tests/CMakeLists.txt`(14)、`test_agentactor.cpp`(492)、`test_agentcontroller.cpp`(261)、`test_agentpermissions.cpp`(39) |

合计：**22 files changed, 3006 insertions(+), 255 deletions(-)**

**新增文件**

| 文件 | 行数 | 功能 |
|---|---|---|
| `src/core/agent/AgentRunWaiter.h` / `.cpp` | 80 / 174 | 嵌套事件循环等待运行完成；超时/重入拒绝/断点退出/abort；`waitForRun(engine,timeout,start)` 先连接后启动 |
| `src/core/agent/AgentToolImage.h` / `.cpp` | 37 / 74 | `temp/deeplux-agent` 落盘、命名、按天清理 |
| `tests/test_agenttoolcontract.cpp` | 383 | 契约/权限矩阵/注册表一致性（16 用例） |
| `tests/test_agentretry.cpp` | 472 | LLM 错误分类与重试（本地 QTcpServer 假端点，11 用例） |
| `tests/test_agentclosedloop.cpp` | 416 | 建流程→validate→run_flow(wait)→outputs→tune→capture 闭环（6 用例） |
| `docs/agent-iteration1-design.md` | ~1.6k | 第一迭代契约冻结（本文件的实现依据） |

### 11.4 最终构建与测试（原始输出摘要，2026-10-27）

**集成复跑（t7，本机 Windows + vcvars64）**

```text
# 构建（增量：无待编译 TU，最新源文件 00:00:44 < 最旧 obj 00:04:24，deeplux_core.dll 00:04:55）
[1/1] Staging SAM server runtime files
EXIT=0

# ctest -R "test_agent" --output-on-failure
1/12 test_agentobserver  Passed 0.11 sec     7/12 test_agentcontroller Passed 0.06 sec
2/12 test_agentactor     Passed 1.64 sec     8/12 test_agentbridge     Passed 0.16 sec
3/12 test_agentpermissions Passed 0.04 sec   9/12 test_agente2e        Passed 0.17 sec
4/12 test_agenttoolcontract Passed 0.05 sec 10/12 test_agent_draft_restore Passed 0.26 sec
5/12 test_agentretry     Passed 24.81 sec   11/12 test_agentclosedloop Passed 0.35 sec
6/12 test_agentundo      Passed 0.04 sec    12/12 test_agent_sam_tools Passed 0.79 sec

100% tests passed, 0 tests failed out of 12
Total Test time (real) =  28.51 sec
EXIT=0
```

**完整重编 + 全量回归（t10 复验，证据链见 `build-win-tests/verify-t10-report.md`）**

```text
删 398 个 *.obj → cmd/vcvars64 + cmake --build build-win-tests -j 8
[551/551] 398 TU，0 error C/LNK/FAILED，3.68 min，EXIT=0
ctest -R test_agent     → 100% passed, 0 failed out of 12（29.32 s），EXIT=0
ctest -R test_runengine → 1/1 Passed（5.75 s），EXIT=0
ctest（全量）           → 100% passed, 0 failed out of 80（78.79 s），EXIT=0
头文件 mtime ≤ 23:57:31 < obj 00:04:50–00:07:26 < 二进制 00:04:55–00:07:59（无假通过）
```

**格式门禁**：t4/t8 分别对 12 / 8 个改动或新增 C++ 文件执行 `clang-format --dry-run --Werror`，0 violations。

**Linux 集成复验（2026-10-27，`af56b5b` 加客户端生命周期修复）**

- 独立工作树从零构建：GCC 11.4、Qt 5.15.3、OpenCV 4.8.1，`DEEPLUX_NATIVE_SAM=ON`，ONNX Runtime CPU SDK；完整构建退出码 0。
- 首次重点回归 17/18：`test_agentclosedloop` 的视觉关闭用例发生 SIGSEGV。GDB 定位到 `cleanup → setLLMClient(nullptr) → QObject::disconnect`：栈上客户端先析构，控制器的非拥有裸指针仍指向已释放对象。
- 控制器改用 `QPointer<ILLMClient>`，保持外部所有权不变；新增 `testDestroyedLLMClientIsCleared`，已验证修复前失败、修复后通过。原闭环套件 8/8 通过。
- 测试用独立临时 HOME，先用 `scripts/sync-plugins.sh build` 同步本次构建的 66 个插件，再清除 `DEEPLUX_APP_DATA_DIR` 覆盖并运行全量 CTest：**82/82 通过，48.62s**，包含 Agent、RunEngine、SAM CPU 数值对照和 GUI 截图自校验。
- 测试环境注意：默认路径测试断言 `~/.deeplux`；插件克隆与截图测试需要部署插件。直接指向空的应用数据目录会导致测试前置条件失败，不代表产品回归。
- 本次修改的 C++ 文件 `clang-format --dry-run --Werror` 和 `git diff --check` 通过。CUDA 专项、真实 LLM 服务及 Windows 修复后重测未在本轮执行；82/82 不代表这些门禁通过。

### 11.5 构建环境坑（已同步写入 §8.1，此处为执行摘要）

1. **vcvars64 必须**：本机 `INCLUDE`/`LIB` 为空，裸 PowerShell 调 `cmake --build` 会找不到 `cl`/系统头文件。
2. **头文件依赖追踪损坏**：CMake 中文 `showIncludes` 前缀乱码 → 改 `.h` 后必须删对应 `*.obj`（`build-win-tests/src/core/CMakeFiles/deeplux_core.dir/agent/<Name>.cpp.obj`）再构建，否则"改了不重编"假通过；保守做法删整个 `deeplux_core.dir`。
3. **运行时 PATH**：`D:\deps\Qt\bin`、`D:\deps\opencv\build\x64\vc16\bin` 必须在 `PATH`。
4. **禁止并发构建/测试**：会争抢同一构建目录导致偶发失败。

### 11.6 遗留问题与已知风险

| # | 类型 | 问题 | 影响 | 建议（第二迭代） |
|---|---|---|---|---|
| L1 | 未完成（测试缺口） | `tune_node_and_verify` 的超时/中止/断点分支有实现但无自动化断言（测试夹具只能装真实插件，无法把可注入慢模块放进工程节点路径）；`run_flow` 超时分支已覆盖 | 该分支仅经 code review | 在 `RunEngine` 增加测试专用注入点（可注册内存模块到节点路径），或抽出 `runAndWait` 的纯函数化等待策略做单测 |
| L2 | 未完成（测试缺口） | `unsupportedPorts` 分支无真实生产者（现有插件无自定义 QVariant 端口类型） | 无断言 | 用一个测试专用 `ModuleBase` 子类产出自定义类型端口补断言 |
| L3 | 未完成（功能） | 模型名视觉启发式（`§6.6` 第 2 档）未实现 | 未显式配置且模型无视觉能力时，只能靠 400 自动降级兜底（多一次失败往返） | 在 `OpenAILLMClient` 增 `static bool heuristicSupportsVision(model)`，`MainWindow` 仅在配置键缺失时使用 |
| L4 | 已知限制 | `create_project` 失败分支生产路径不可达（`mutatesProject` 门禁先返回 `engine_busy`） | 防御性代码，测试经 `DEEPLUX_TEST_VISIBILITY` 覆盖 | 保持现状；若未来放开"运行中建工程"，该分支自然可达 |
| L5 | 已知风险 | `RunEngine::nodePortOutputs` 等快照 API 前置条件 `!isBusy()`，`m_nodeOutputs` 本身无锁保护 | 越过前置条件会产生数据竞争 | 加锁或改为"运行中禁止读取"断言；TSan 门禁纳入 CI |
| L6 | 流程（已完成） | Windows 交付已提交为 `af56b5b`；Linux 集成复验记录见 §11.4 | 不再是未提交工作树 | 保留平台验收边界；排除 `.agent-teams/`、`build*` |
| L7 | 环境 | `build-win-tests` 头依赖追踪损坏（本机 CMake 本地化问题） | 易产生假通过 | CI/本地统一"改头文件先清 obj"；或修复 VS 本地化（英文语言包 / 设 `VSLANG=1033`）后重配构建目录 |
| L8 | 已知限制 | `AgentBridge` 外部 IPC 调 `wait=true` 工具会阻塞整次请求直到运行结束 | 外部客户端可能超时 | 外部调用默认强制 `wait=false`，或桥接层加"异步 tool_call + 轮询"（设计文档附录 D 第 5 条） |
| L9 | 已知风险 | 单轮墙钟上界 = Σ 等待超时（理论 20 轮 × 2×300s） | 极端配置下会话可长达数小时 | 已有停止按钮；第二迭代加单轮/单消息全局 deadline |
| L10 | 未完成（规划内） | 审计仅内存表（7 列）、无落盘；上下文裁剪仍按 user turn 整组丢弃 | 现场追溯与长会话质量受限 | §11.7 第 1~3 项 |

### 11.7 第二迭代入口（建议顺序）

> 第二迭代已于 2026-10-28 交付；实际执行以 [agent-execution-plan-v2.md](agent-execution-plan-v2.md) 为准，结果与偏差见 §12。以下为当时的建议顺序，保留作历史记录。

1. **对话质量**：流式 SSE + reasoning 折叠 + 增量渲染（`appendToLastMessage` 已存在未接线）；token/单工具耗时面板完善（当前已有 `llmUsageReported` 与 7 列审计，缺聚合视图）。
2. **上下文治理**：按"工具调用组"原子裁剪 + 单结果 6k 截断留首尾 + 预算可配（替换现有 turn 级裁剪，`trimHistoryIfNeeded` 已按图片字节计费，可直接扩展）。
3. **审计落盘**：JSONL 按天轮转 + 失败请求报文落盘（`AgentActionLogEntry` 字段已就绪）。
4. **可取消的单节点执行原语**：解锁 `tune_node_and_verify(runMode=node)` 真执行与 `sweep_node_parameter`（需 `RunEngine` 异步节点作业 + 取消协议 + 跨线程快照加锁，与 L5 一并解决）。
5. **视觉启发式 + 目录配额**：L3、图片临时目录配额/LRU（设计文档附录 D 第 4 条）。
6. **对外与配置**：`DeepLux --mcp-stdio`（工具来自 `ToolSchema`，外部默认 Observer 或强制确认）；多服务商预设 + 每 provider 独立 Key（系统凭据存储）+ 连接测试。
7. **治理强化**：`AgentBridge` 外部 `wait` 类工具默认 `wait=false`（L8）；单轮全局 deadline（L9）。

---

## 12. 第二迭代交付记录（2026-10-28）

- 交付日期：**2026-10-28**（实现 2026-10-27 ~ 28；含 t23 修复、t24 复审两个补轮）
- 质量门：t15 2A 审查 needs_revision → t23 修复、t24 复审 pass；t22 全量审查 pass，t21 清 obj 全量复验通过。
- 基线：`af56b5b`（第一迭代交付提交）；第二迭代已在 `Windows_work` 提交为 `860523b`。
- 设计依据：[agent-iteration2-design.md](agent-iteration2-design.md)（2A）、[agent-iteration2b-design.md](agent-iteration2b-design.md)（2B/2C/2D）、[agent-execution-plan-v2.md](agent-execution-plan-v2.md)（任务清单）

### 12.1 交付项（对照执行计划 2A/2B/2C/2D）

| 阶段 | 交付内容 | 关键文件 | 状态 |
|---|---|---|---|
| **2A 对话质量** | SSE 流式解析（`SseStreamParser`：半包/多包/CRLF/注释/多行 data/`[DONE]`/1MiB 丢行）、`delta.content`/`reasoning_content`/`tool_calls[index]` 累加、首 delta 前重试 + 已有 delta 不重试不回退、F1~F5 非流式回退、50ms 合批、reasoning 折叠块（默认不回传，`agent/reasoningEcho=none`）、六终态映射、abort 保留已收内容 | `SseStreamParser.{h,cpp}`、`OpenAILLMClient.{h,cpp}`、`ILLMClient.{h,cpp}`、`AgentController.{h,cpp}`、`AgentChatPanel.{h,cpp}`、`AgentMessageBubble.{h,cpp}`、`MainWindow.cpp`、`tests/test_agentstream.cpp`、`tests/test_agentstreamui.cpp` | ✅ |
| **2A 上下文治理** | 工具组原子裁剪（I1~I5，禁止孤儿 tool）、单条 tool 结果 6k 首尾截断+省略量、预算可配（`contextBudgetTokens` 默认 128000/[16000,1000000]）、裁剪后首条非 system 为 user、`buildContext` 摘要化（阈值 24/前 24/提示 `get_flow_state`/事件≤5/16384 硬上限）、设置页两个数值框 | `AgentController.{h,cpp}`、`AgentSettingsDialog.{h,cpp}`、`MainWindow.cpp`、`tests/test_agentcontroller.cpp`（+275 行） | ✅ |
| **2B 审计** | 审计 JSONL（`agent-audit-YYYYMMDD[-NN].jsonl`、按天轮转、10MB 分片、14 天保留、v=1 全字段、写失败 3 次自禁用）+ LLM 失败报文（`llm-failed-YYYYMMDD.json`、仅 BadRequest/Malformed、QSaveFile 原子写、单条 256KiB/单日 1MiB、R1~R5 脱敏+自检丢弃） | `AgentAuditLog.{h,cpp}`、`FailedRequestLog.{h,cpp}`、`AgentController.cpp`（挂接 4 行）、`OpenAILLMClient.{h,cpp}`（钩子）、`tests/test_agentaudit.cpp` | ✅ |
| **2C MCP** | `deeplux-mcp-bridge`（stdio JSON-RPC 2.0：initialize/ping/tools/list/tools/call/notifications；25 工具来自 `ToolSchema`+annotations；转发 AgentBridge v1.0；token 认证；错误码 -32001/-32002/-32003；外部只读，写工具 `permission_denied` + GUI 提示且不转发） | `src/app/mcp/**`（4 文件 5 源）、`src/app/CMakeLists.txt`（+27 行）、`tests/test_mcpbridge.cpp`（15 槽≈90 断言） | ✅ |
| **2D 遗留收口** | L1 tune 五分支测试（AgentRunWaiter 注入）、L2 `unsupportedPorts`（测试 ModuleBase + RunEngine 锁内注入钩子）、L3 视觉启发式（7 模型单测、仅配置缺省时使用）、L5 `m_nodeOutputs` 全部 9 处写加 `m_lifecycleMutex` + 并发快照用例、L8 外部 `run_flow` 强制 `wait=false`/`tune` 强制 `runMode=node`（含回滚配置）、L9 单轮 deadline（默认 600000 clamp[10000,3600000]）；另补 R1（reasoning 展示）、R3（流参数注入） | `AgentRunWaiter.{h,cpp}`、`RunEngine.{h,cpp}`、`AgentBridge.cpp`、`AgentController.{h,cpp}`、`OpenAILLMClient.{h,cpp}`、`MainWindow.cpp`、`tests/*`（20 个新槽） | ✅ |

### 12.2 与执行计划/设计的偏差（逐条）

| # | 计划/设计 | 实际实现 | 理由与影响 |
|---|---|---|---|
| 1 | `reasoning` 是否回传未定 | `agent/reasoningEcho` 默认 **none**（不回流请求），可设 current-turn/full | **明示行为变更**（第一迭代是回传）；DeepSeek 思考模式回传有 400 风险，队长批准保守默认；附录 B 文案与开关兜底 |
| 2 | 6k 截断时机未定 | 在**写入历史时**截断（审计/UI 保留全文） | 队长批准；等价"只影响回传模型"且省内存 |
| 3 | 摘要阈值初稿 40/前 12 | **24/24** | 队长批准（执行计划口径） |
| 4 | MCP 契约文本写 `-32002/-32003` 与"默认 120s" | 按冻结设计：**-32002=超时、-32003=鉴权失败、默认 30000ms clamp[1000,300000]** | t20 契约文本笔误；t22 审查确认按设计为准、不判 failed |
| 5 | MCP 权限"不得绕过控制器" | 只读在 **MCP 桥内预检**（未改 core），只读调用仍经 AgentBridge→`AgentController::handleToolCall` 权威判定 | 与计划"t20 与 t18/t19 文件不相交"一致；写工具不建连接不转发（假桥断言 0 请求），不存在绕过路径 |
| 6 | L1 计划用 `AgentActor` 注入钩子 | 实际用 **`AgentRunWaiter` Outcome 注入**（生产 private、默认空） | 等价覆盖 tune 五分支，生产零行为变化；未改 out-of-scope 的 `AgentActor` 派发 |
| 7 | L2 计划"测试专用 ModuleBase 产出端口" | 实际 `UnsupportedPortModule` + `RunEngine::setNodePortOutputsForTest`（锁内写） | 真实覆盖 `unsupportedPorts` 分支 |
| 8 | L5 设计列 7 处写点 | 实际 **9 处**（+stepOnce clear +测试钩子） | 全量清点后补齐，避免遗漏 |
| 9 | 失败报文"完整报文" | 按验收做脱敏+上限：不存完整历史（逐条 head≤2KB、图片只记数量、响应头≤64KB） | "完整"与"Authorization/Key/图片 base64 不得入盘"冲突，安全优先；见 §12.7 |
| 10 | 计划任务表 t11~t22 | 实际增加 **t23（repair round 2）/ t24（review round 2）** | R2/R4/R5 修复与三项测试缺口补齐；不改变交付范围 |
| 11 | 设置页只列预算 | 同时加了 `AgentToolResultMaxChars` 数值框 | 与 6k 截断可配置对齐，避免只改配置文件 |
| 12 | 审计为"按天单文件" | 单文件 10MB 后分片 `-01/-02…`（单日≤99） | 防单日爆量写满磁盘 |

### 12.3 变更摘要（按文件/功能）

**修改文件**：`git diff --stat af56b5b -- src tests` = **25 files changed, 2201 insertions(+), 118 deletions(-)**

| 功能组 | 文件（改动行数） |
|---|---|
| 流式/LLM | `OpenAILLMClient.cpp`(580)、`OpenAILLMClient.h`(79)、`ILLMClient.h`(30)、`ILLMClient.cpp`(25) |
| 控制器/上下文/审计挂接 | `AgentController.cpp`(545)、`AgentController.h`(69)、`AgentRunWaiter.cpp`(30)、`AgentRunWaiter.h`(5) |
| 引擎/桥接（遗留） | `RunEngine.cpp`(30)、`RunEngine.h`(4)、`AgentBridge.cpp`(24) |
| UI | `AgentChatPanel.cpp`(128)、`AgentChatPanel.h`(15)、`AgentMessageBubble.cpp`(79)、`AgentMessageBubble.h`(15)、`MainWindow.cpp`(54)、`AgentSettingsDialog.cpp`(42)、`AgentSettingsDialog.h`(6) |
| 构建 | `src/app/CMakeLists.txt`(+27)、`src/core/CMakeLists.txt`(+6)、`tests/CMakeLists.txt`(+13) |
| 测试（追加） | `test_agentcontroller.cpp`(275)、`test_agentactor.cpp`(131)、`test_agentbridge.cpp`(68)、`test_runengine.cpp`(39) |

**新增文件（行数）**

| 文件 | 行数 | 功能 |
|---|---|---|
| `src/core/agent/SseStreamParser.{h,cpp}` | 56 / 115 | SSE 解析（纯逻辑，P1~P10） |
| `src/core/agent/AgentAuditLog.{h,cpp}` | 89 / 352 | 审计 JSONL 落盘/轮转/保留/脱敏 |
| `src/core/agent/FailedRequestLog.{h,cpp}` | 86 / 368 | 失败请求报文/脱敏/上限 |
| `src/app/mcp/main.cpp`、`McpStdioServer.{h,cpp}`、`AgentBridgeClient.{h,cpp}` | 89 / 71+229 / 74+267 | MCP stdio 桥与 AgentBridge 客户端 |
| `tests/test_agentstream.cpp` / `test_agentstreamui.cpp` | 713 / 423 | 客户端流式 / 控制器·面板流式 |
| `tests/test_agentaudit.cpp` / `test_mcpbridge.cpp` | 607 / 567 | 审计+失败报文 / MCP 协议 |
| `docs/agent-iteration2-design.md` / `iteration2b-design.md` | 843 / 840 | 第二迭代契约冻结（2A / 2B+2C+2D） |

### 12.4 最终构建与测试（原始输出摘要）

**t16 集成复跑（本机 Windows + vcvars64；构建为增量，mtime 证明无待编译 TU）**

```text
# cmd /c 'call vcvars64.bat && cmake --build build-win-tests -j 8'
[1/1] Staging SAM server runtime files
EXIT=0
# 证据：最新源文件 test_runengine.cpp 00:10:19 < 最旧 obj 00:15:12 < deeplux_core.dll 00:15:37 < deeplux-mcp-bridge.exe 00:18:03

# ctest --test-dir build-win-tests -R "test_agent" --output-on-failure
 1/15 test_agentobserver       Passed  0.10 sec
 2/15 test_agentactor          Passed  1.65 sec
 3/15 test_agentpermissions    Passed  0.04 sec
 4/15 test_agenttoolcontract   Passed  0.04 sec
 5/15 test_agentretry          Passed 24.80 sec
 6/15 test_agentundo           Passed  0.04 sec
 7/15 test_agentcontroller     Passed  0.12 sec
 8/15 test_agentbridge         Passed  0.26 sec
 9/15 test_agente2e            Passed  0.18 sec
10/15 test_agent_draft_restore Passed  0.27 sec
11/15 test_agentclosedloop     Passed  0.31 sec
12/15 test_agentstream         Passed  2.03 sec
13/15 test_agentstreamui       Passed  0.39 sec
14/15 test_agentaudit          Passed  0.40 sec
15/15 test_agent_sam_tools     Passed  0.78 sec

100% tests passed, 0 tests failed out of 15
Total Test time (real) =  31.41 sec
EXIT=0
```

**t21 复验（清 obj 全量重建；证据链见 `build-win-tests/verify-t21-report.md`）**

```text
删 414 个 *.obj → vcvars64 全量重编 [573/573]，414 TU，0 error，exit 0（2.98 min）
头文件 mtime ≤00:10:19 < obj 00:15:28–00:17:41 < 二进制 00:15:37–00:18:05（无假通过；SHA256 守卫 88 个二进制 0 变化）
ctest -R test_agent     → 15/15 Passed（含 stream 2.09s / streamui 0.41s / audit 0.44s），exit 0
ctest -R test_mcpbridge → 1/1 Passed 0.18s，exit 0
ctest -R test_runengine → 1/1 Passed 10.54s，exit 0
ctest（全量）           → 84/84 Passed，0 skipped/disabled，exit 0
特性 -V 45 项点名全 PASS：8 套 Totals = runengine 116 / controller 48 / actor 39 / audit 18 / mcp 17 / stream 26 / streamui 15 / bridge 14，0 failed
```

- 测试总数：80（第一迭代）→ **84**（新增 `test_agentstream`、`test_agentstreamui`、`test_agentaudit`、`test_mcpbridge`；`test_agent*` 12 → 15）。
- 验收条目：t12 9 + t13 6 + t18 7 + t19 10 + t20 7 + t23 6 = **45 条全部 passed**；t22 审查 pass、t24 复审 pass。
- 插件 ABI 未改动 ⇒ 无需 `sync-plugins`；格式门禁：各任务累计对 19+8 个改动 C++ 文件执行 `clang-format --dry-run --Werror`，0 violations。

**Linux 集成复验（2026-10-28）**

- 将 `860523b` 在隔离工作树 rebase 到含 `7e9ee05` 的本地基线，确认 `QPointer<ILLMClient>` 和析构回归仍存在。
- 首轮全量测试在 `AgentAuditLog` 遍历临时 JSON 对象时 SIGSEGV；改为持有对象副本后，审计用例通过。
- 补齐多工具结果配对、聊天拒绝/排队中止的结果、确认前权限降级拒绝，以及 Bridge 拒绝不污染聊天历史的回归；修复对应问题。
- 扩展常见 access/refresh/id token、client secret、带空格的密码及小写 bearer 脱敏；请求终端日志不再打印服务响应正文或 URL 的用户信息/查询参数/片段。合成凭据测试均先失败后通过；脱敏仍不能识别任意业务文本，见 §12.7。
- 系统 Qt 5.15.3、OpenCV 4.8.1、GCC 11.4、原生 SAM ONNX Runtime CPU；插件部署在临时 HOME，完整构建成功，全量 CTest **86/86 通过（49.05s）**，含 Agent、MCP、RunEngine、SAM CPU 和 GUI 截图自校验。
- 未复验修复后的 Windows 构建、CUDA 专项、真实 LLM 服务或真实工业数据；历史 Windows 84/84 不等于本次跨平台验收。

### 12.5 遗留状态更新（对照 §11.6 L1~L10）

| ID | 第一迭代状态 | 第二迭代结果 |
|---|---|---|
| L1 | tune 超时/中止无断言 | ✅ **关闭**：AgentRunWaiter Outcome 注入，5 个分支用例（timeout/cancelled/reentrant_wait/paused_at_breakpoint/run_failed） |
| L2 | `unsupportedPorts` 无生产者 | ✅ **关闭**：测试 ModuleBase 产 QRect 端口 + RunEngine 测试钩子，断言 `unsupportedPorts` 含 `rect` |
| L3 | 视觉启发式未实现 | ✅ **关闭**：`heuristicSupportsVision` 纯函数 + 仅配置缺省时使用（7 模型单测） |
| L4 | create_project 死分支 | ⏸ 保持现状（防御性代码，`DEEPLUX_TEST_VISIBILITY` 覆盖） |
| L5 | `m_nodeOutputs` 无锁 | ✅ **代码层关闭**：9 处写统一 `m_lifecycleMutex` + 快照锁内深拷贝 + 并发用例；**TSan 门禁仍延后**（第三迭代） |
| L6 | 未提交 git | ✅ Windows 交付提交为 `860523b`；Linux 集成状态以本地实际提交为准 |
| L7 | 构建头依赖追踪损坏 | ⏸ 保留纪律（改 `.h` 先删 obj）；CI 根治另立 |
| L8 | 外部 wait 阻塞 | ✅ **关闭**：`run_flow` 强制 `wait=false`、`tune` 强制 `runMode=node`（`agent/bridgeForceAsyncWaitTools` 可回滚） |
| L9 | 单轮无全局 deadline | ✅ **关闭**：`agent/turnDeadlineMs` 默认 600000 clamp[10000,3600000] + 三检查点 + 统一收口 |
| L10 | 审计无落盘/裁剪 turn 级 | ✅ **关闭**：审计 JSONL + 失败报文落盘；上下文改为工具组原子裁剪 + 6k 截断 + 预算可配 + 摘要化 |

**第二迭代新增遗留（第三迭代候选）**

| # | 类型 | 问题 | 建议 |
|---|---|---|---|
| N1 | 功能 | MCP 写工具无 GUI 确认通道（`-32004` 预留未返回） | 第三迭代实现"外部请求 → GUI 弹窗确认" |
| N2 | 测试缺口 | GUI 流式回退提示无自动化断言（仅 controller 级） | test_mainwindow 增加面板提示断言 |
| N3 | 测试缺口 | 流空闲看门狗 60s 未测（仅回退/超时分支） | 注入可调看门狗时间做单测 |
| N4 | 测试缺口 | MCP `-32603` 无稳定触发场景、单行 >4MiB 处理未单独断言 | 增加故障注入用例 |
| N5 | 治理 | TSan 未纳入 CI（L5 仅代码层） | 独立 TSan 环境 + 门禁 |
| N6 | 性能 | MCP 桥链接 `deeplux_core` 会拉起 Qt Widgets/OpenCV DLL | 第三迭代抽轻量 `deeplux_agent_api` |
| N7 | 观测 | 审计为同步小写入、无聚合视图；失败报文按 14 天清理无配额面板 | 异步队列 + 设置页查看/导出 |
| N8 | 功能 | 多服务商预设 + 每 provider 独立 Key（系统凭据）+ 连接测试未做 | 第三迭代（执行计划 §5 明确不做） |

### 12.6 MCP 使用方式（客户端挂载配置）

**前置条件**

1. 本机正在运行 **DeepLux GUI**（`AgentBridge` 由 GUI 启动；GUI 未运行 → MCP 返回 `-32001`，不重试挂死）。
2. 产物位置：构建目录 `build-win-tests/bin/deeplux-mcp-bridge.exe`（安装后为 `<install>/bin/deeplux-mcp-bridge`）。
3. 可选鉴权：在配置文件 `<appData>/config/config.json` 的 `agent` 组设置 `bridgeToken`（GUI 与 MCP 桥读同一配置）；未设置则跳过握手。

**Claude Desktop（Windows）**：编辑 `%APPDATA%\Claude\claude_desktop_config.json`

```json
{
  "mcpServers": {
    "deeplux": {
      "command": "C:\\path\\to\\DeepLux\\bin\\deeplux-mcp-bridge.exe",
      "args": []
    }
  }
}
```

**Cursor**：编辑 `~/.cursor/mcp.json`（或项目 `.cursor/mcp.json`），结构同上。

**Linux**

```json
{
  "mcpServers": {
    "deeplux": {
      "command": "/opt/deeplux/bin/deeplux-mcp-bridge"
    }
  }
}
```

**能力与限制**：`tools/list` 列出全部 **25** 个工具（含 `readOnlyHint`/`destructiveHint`）；**只有 `readOnly==true` 的工具可调用**；写/危险工具在桥内直接返回 `isError=true` + `{"code":"permission_denied","hint":"…inside the DeepLux GUI…"}`，**不建立连接、不转发**。`tools/call` 示例（只读）：

```json
{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"get_flow_state","arguments":{}}}
{"jsonrpc":"2.0","id":1,"result":{"content":[{"type":"text","text":"{\"modules\":[...]}"}],"isError":false}}
```

错误码：`-32001` GUI 未运行、`-32002` 桥超时（默认 30000ms，`agent/mcpBridgeTimeoutMs` clamp[1000,300000]）、`-32003` 认证失败、`-32700`/`-32600`/`-32601`/`-32602`/`-32603` 标准 JSON-RPC。

### 12.7 审计文件格式与脱敏说明

**审计（每次工具/撤销/错误动作一条）**

- 位置：`~/.deeplux/logs/agent-audit-YYYYMMDD.jsonl`（Windows `C:\Users\<user>\.deeplux\logs\`；`DEEPLUX_APP_DATA_DIR` 可覆盖）。
- 轮转/保留：按天切换；当日 ≥10MB 换 `-01/-02…` 分片（单日≤99）；保留 **14 天**。
- 单行示例：

```json
{"v":1,"ts":"2026-10-28T00:31:02.123+08:00","actor":"Agent","action":"run_flow","params":{"mode":"once","wait":true},"result":"success","undoable":false,"durationMs":132,"toolCallId":"call_1"}
```

- 字段与 `AgentActionLogEntry` 一一映射（`durationMs=-1` / 空 `errorCategory` / 空 `toolCallId` 时省略键）；单条 ≤16KiB；写盘失败只写 `Logger`、连续 3 次自禁用（跨天或重启恢复），**不影响对话**。
- 脱敏：`apiKey/authorization/token/secret/password` 值替换 `***`；`data:image/...;base64,...` 替换为 `data:image/…;base64,[omitted]`；审计从不写入图片字节。

**失败请求报文（仅 BadRequest / Malformed）**

- 位置：`~/.deeplux/logs/llm-failed-YYYYMMDD.json`，结构 `{"v":1,"records":[...]}`；单条 ≤256KiB、单日 ≤1MiB、保留 14 天；`QSaveFile` 原子重写。
- 记录内容：`ts/category/httpStatus/attempts/endpoint(去 query)/model/contextLimitSuspected/visionDowngradeHint/request(逐条 role+chars+head≤2KB+图片数量)/response(head≤64KB)/detail(≤2KB)/redactionApplied`。
- 脱敏规则覆盖常见 `Authorization`、API Key、access/refresh/id token、client secret、密码赋值、Bearer 和图片数据；落盘前另有模式自检。Auth 类别**不落盘**。这些模式不能识别任意自由文本中的凭据、客户数据或业务机密，不应视为完全脱敏。

**生产隐私注意**：审计日志默认开启，失败请求日志对未显式配置的用户默认**关闭**（集成收口 B5；显式已有配置值继续尊重）。审计可包含工具参数；失败请求日志开启时，BadRequest/Malformed 可包含每条消息的前 2048 字符及错误详情，即使匹配规则已脱敏，普通业务内容仍可能留在磁盘 14 天。敏感机台应先评估目录访问权限和保留策略；可在 Agent 设置页或 `<appData>/config/config.json` 的 `agent` 对象中分别设置 `auditEnabled`、`failedRequestLogEnabled`，并重启应用。关闭只停止写入新正文，不自动删除已有文件。修改时保留该对象的其他配置。详见 [agent-integration-hardening-plan.md](agent-integration-hardening-plan.md) B5。

### 12.8 第三迭代入口（建议顺序）

1. **MCP 写能力 + GUI 确认**：实现 `-32004` 预留通道（外部请求 → GUI 确认 → 执行 → 回传），仍不放开"无确认直执行"。
2. **多服务商与凭据**：provider 预设、每 provider 独立 Key（系统凭据存储，不明文落盘）、模型列表拉取与连接测试。
3. **可取消的单节点执行原语**：解锁 `tune_node_and_verify(runMode=node)` 真执行与 `sweep_node_parameter`（RunEngine 异步节点作业 + 取消协议；与 TSan 一起收口）。
4. **TSan 门禁 + 并发加固**：L5 已做代码层加锁，第三迭代把 TSan 纳入 CI（`docs/baseline/tsan-report.md` 的误报基线需先收敛）。
5. **可观测增强**：审计异步队列/批量刷盘、失败报文与审计的配额/导出面板、流看门狗与 MCP 异常注入测试（N2~N4）。
6. **MCP 依赖拆分与扩展**：抽 `deeplux_agent_api` 轻量库（避免拉起 Widgets/OpenCV）；如需支持 MCP 流式/分页再扩展。
7. **对话质量继续**：上下文摘要/记忆（替代纯裁剪）、reasoning 回传策略按 provider 自适应（当前默认 none + 手动开关）。
8. **隐私与现场验收**：失败请求正文默认关闭与设置页开关已并入集成收口 B5；真实供应商、Windows 和带 CUDA 的 Linux 构建的现场验收仍未完成，不能用离线 stub 或另一平台结果替代。

### 12.9 集成收口（2026-10-29）

两轮迭代后的集成更新已按 [agent-integration-hardening-plan.md](agent-integration-hardening-plan.md) 完成 B0~B6 工程验收：失败闭环、权限/确认有效期、上下文保护、MCP 取消与输入边界、隐私开关均已落地；全量 CTest 86/86 通过（隔离 HOME），B1~B4 关键边界定向重复 20/20。Windows、真实模型供应商、实际 MCP 客户端与真实 GUI 手工场景仍未验证，详见 [验收记录](reports/agent-integration-hardening-acceptance.md)，不得据此宣称使用验收通过。
