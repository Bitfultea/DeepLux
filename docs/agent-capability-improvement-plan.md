# DeepLux Agent 能力完善与验收执行计划

版本：1.0；日期：2026-09-28。面向外部实现 Agent、独立审核 Agent 和项目维护者。

状态：进行中。阶段 0–7 的软件验收范围已按对应验收记录通过；阶段 8 的逐插件业务覆盖与目标环境/现场验收仍未完成。各阶段通过范围不替代阶段 8 真实设备、模型、provider 和平台证据。

## 1. 目标、范围与验收口径

目标是让 Agent 能完成当前软件已支持的操作：发现可用插件和设备、配置参数与输入、搭建流程、控制执行、读取真实结果、处理失败、保存工程，以及调用数据源、测量、标注等界面操作对应的业务接口。

“支持全部插件”必须同时回答：能否发现、配置、提供有效输入、执行、读取业务输出，以及在失败、确认和撤销时保持正确状态。仅能添加节点、参数默认值通过校验、工具调用返回成功，均不能代表完整支持。

本计划以源代码、实际构建配置、运行时能力和可重复证据为准。已有插件的实验性算法、演示输出或未实现功能，不能通过新增 Agent 接口升级为生产能力；应在能力矩阵中明确标记并阻止错误承诺。

### 1.1 三个交付等级

| 等级 | 完成条件 | 可以使用的交付描述 |
| --- | --- | --- |
| A：离线流程可用 | 阶段 0–4 通过，并满足阶段 7 中与离线流程有关的标准；离线插件逐项具备参数、输入、运行、输出证据 | “已支持矩阵所列离线流程操作” |
| B：软件操作覆盖 | 阶段 0–7 全部通过；所有现有软件操作都有可执行入口或明确的人机协作入口；模拟设备接口通过 | “当前配置的软件接口覆盖完成，现场项另列” |
| C：目标环境验收 | 阶段 8 通过；目标系统、真实模型、相机和通讯设备完成验证 | “指定平台与设备配置通过验收” |

不得用 A 或 B 的证据宣称 C 完成。不同操作系统的相机驱动分别验收；不要求 Windows 驱动在 Linux 运行，但必须给出平台适用性证据。

### 1.2 操作范围和边界

- 覆盖当前仓库实际支持的插件、工程、流程、数据源、变量、相机、通讯、显示、测量、SAM、知识库和设置操作。
- 凭据输入、登录提权、SDK/驱动/模型安装以及系统级文件管理使用明确的人机协作入口；Agent 可以报告所需步骤和实际状态，不能伪报已完成。
- 任意 shell、任意代码执行、网络下载未知插件、无限制桌面点击不作为补齐覆盖率的手段。
- 本计划不扩建新的算法产品、不新增多 Agent 调度系统、不引入向量数据库。复用 `Project`、`RunEngine`、现有 Manager、Undo、Qt 信号和导入导出实现。
- 使用通用参数、端口和结果工具覆盖插件；只有已有 API 无法表达的真实操作才增加专门入口，不为 69 个插件各复制一套工具。

## 2. 当前基线与问题追踪

计划编写时工作区为 `DeepLux_codex`，分支为 `agent/development`，HEAD 为 `b516f54da28b63623ef6b293b71a4afa3b84fad0`。工作区另有未提交的手册、知识库和测试修改，因此 HEAD 单独不能完整重现本次审计对象。

当前源码有 69 份插件元数据：66 个模块插件、3 个相机驱动。当前 Agent 白名单有 19 个工具。最近一次定向构建测试结果为：`test_agentactor` 19/19、`test_agentcontroller` 26/26、`test_pluginparametercontracts` 20/20，退出码均为 0。参数契约测试实际加载 65 个模块、0 个相机；这些数字是各测试程序的 Qt Test 计数，不是逐插件操作通过数。未据此完成真实模型、设备或全量 CTest 验收。

现有 `test_agente2e` 使用脚本式模拟 LLM，并主动等待引擎空闲后才发出结果查询。它可验证固定调用链，不能证明真实模型会正确处理异步运行。

| 编号 | 问题或缺口 | 主要证据入口 | 收口阶段 |
| --- | --- | --- | --- |
| F01 | `get_run_results` 仅返回累计统计，没有模块输出和本次错误 | `AgentActor::getRunResults`、`RunEngine::moduleOutput/getOutput` | 3 |
| F02 | `set_param.value` 声明为 string，仅自动转换数字和布尔 | `ToolSchema::registerDefaultTools`、`AgentActor::setParam` | 2 |
| F03 | 参数 Schema 没有完整范围、枚举、单位、数组结构和输入输出端口；流程状态丢失连线端口 | `getModuleParamsSchema`、`getFlowState`、插件 metadata | 2、4 |
| F04 | N 点标定读取 `pointCount/imgX_i/...`，这些键不在默认参数和元数据白名单 | `NPointCalibrationPlugin::process`、`AgentActor::setParam` | 2 |
| F05 | 只枚举模块，不能管理相机、插件加载状态或设备连接 | `PluginManager::availableModules/availableCameras`、`CameraManager` | 5 |
| F06 | 保存失败返回 `status: failed`，控制器只检查 `error`，可能记录成功并继续 | `AgentActor::saveProject/executeTools`、`AgentController::extendAgentLoop` | 1 |
| F07 | 新建/打开工程可能关闭未保存工程；加载失败也可能丢失当前工程 | `ProjectManager::newProject/openProject` | 1 |
| F08 | Observer 聊天路径不执行只读工具，Bridge 路径却允许，策略不一致 | `onLLMResponse`、`handleToolCall` | 1 |
| F09 | 白名单缺少单步、断点、节点状态、变量和大量界面业务操作 | `ToolSchema`、`Project`、`RunEngine`、`MainWindow` | 4、6 |
| F10 | 部分工具无操作仍返回成功；失败批次未为后续未执行 tool call 补齐结果 | `removeModule/pauseFlow/resumeFlow`、`extendAgentLoop` | 1、7 |
| F11 | Agent 闭环尚未覆盖完整的异步等待、有限重试、用户取消和恢复 | `AgentController`、`test_agente2e` | 3、7 |
| F12 | 全插件参数契约测试会跳过缺失二进制，且未走逐插件 Agent 业务链路 | `test_pluginparametercontracts`、`test_agentactor` | 0、8 |
| F13 | `run_flow` 可间接触发文件删除、写文件和 PLC 写入；仅按工具名分级不充分 | `FolderPlugin::process`、`PLCWritePlugin`、`ToolDefinition::dangerous` | 1、5 |

补充：存在“高级配置”按钮不等于该插件所有参数都无法被 Agent 设置。例如 `ImageScript.scriptType` 是标量。必须按参数和动作逐项判断，不能把高级配置插件整组判为不支持。

代码入口：[AgentActor](../src/core/agent/AgentActor.cpp)、[ToolSchema](../src/core/agent/ToolSchema.cpp)、[AgentController](../src/core/agent/AgentController.cpp)、[ProjectManager](../src/core/manager/ProjectManager.cpp)、[插件手册](plugins.md)。

## 3. 外部 Agent 的执行规则

### 3.1 工作区和责任

1. 只在 `/home/charles/Data/Repo/DeepLux_codex` 工作；不修改同级 `DeepLux`，不复用指向它的构建缓存。
2. 阶段 0 记录 HEAD、分支、已修改/未跟踪文件、相关文件摘要和构建配置。保留已有工作，不擅自 reset、覆盖、stash drop 或把 `.claude/` 纳入提交。
3. 每阶段一个实现负责人；审核由另一个 Agent 或维护者完成。共享文件如 `ToolSchema`、`AgentActor`、`AgentController`、`tests/CMakeLists.txt` 同一时段只允许一个写入负责人。
4. 如安排并行开发，先按文件划分所有权；所有参与者保留他人修改。没有明确分工时按本计划顺序执行。
5. 每阶段提交仅含本阶段范围。若获得提交授权，使用 `fix:`/`feat:`/`test:`/`docs:` 前缀；未获提交授权时交付限定文件的补丁和摘要。推送、合并、发布遵循维护者已有授权。

### 3.2 阶段门槛

状态固定为 `NOT_STARTED → IN_PROGRESS → READY_FOR_REVIEW → PASS`，未通过为 `FAIL`，缺少环境证据为 `BLOCKED_EXTERNAL`。

- 实现 Agent 只能标记 `READY_FOR_REVIEW`；独立审核者检查代码差异、测试原始输出和实际结果后填写 `PASS`。
- 前一阶段未通过，不进入依赖它的后续阶段。审核发现问题，修复后重新提交本阶段证据。
- 阶段 0 的 PASS 表示基线完整且可重复，不表示现有缺陷已修复；已知失败必须与问题编号绑定。后续阶段的新增和受影响用例必须通过，最终声明范围内不得遗留基线失败。
- 环境缺失允许保存已完成代码，但相应现场验收项保持 `BLOCKED_EXTERNAL`。模拟验证通过可以放行后续软件阶段，不能放行最终现场交付。
- 每阶段都更新问题编号、能力矩阵和验收记录；计划性描述、截图中的文字、实现 Agent 的总结不能代替原始证据。
- 不把所有不相关历史缺陷纳入改造；若历史问题阻断某项需求，记录可复现证据、影响范围和最小修复，不能通过删测、放宽断言或省略插件隐藏它。

### 3.3 所有阶段通用的完成定义

- 目标操作从实际 Agent 入口可达，Schema、权限、执行、结果和审计一致；不是只调用私有函数通过。
- 成功、非法输入、前置条件缺失和取消至少有与本阶段风险相称的回归证据。
- 失败不产生未声明的部分写入，不伪造成功；可撤销项确实可撤销，不可撤销项明确标识。
- GUI、Project 与 RunEngine 对同一实例使用稳定 ID，不通过显示名或节点位置猜测归属。
- 新增测试按 [仓库测试约定](../AGENTS.md) 接入 CTest；优先扩展已有测试，只有独立主题才新增测试文件。
- 相关构建及测试退出码为 0，`git diff --check` 通过；C++ 修改遵循 `.clang-format`。
- 文档和工具描述仅宣称已通过的能力；无 TODO、空实现、固定成功返回或模拟输出冒充实际操作。

## 4. 阶段总览

| 阶段 | 目标 | 主要交付 | 硬门槛 |
| --- | --- | --- | --- |
| 0 | 固定基线与覆盖分母 | 插件/操作矩阵、构建与测试基线、复现清单 | 69 项可核对，缺项有理由 |
| 1 | 修复错误返回、权限和工程保护 | 一致执行策略、可靠失败语义、工程切换保护 | 失败无假成功，确认无旁路 |
| 2 | 补齐参数、Schema 和端口发现 | 原子 JSON 设参、完整插件描述、复杂参数迁移 | 数组/空值/跨字段配置可用 |
| 3 | 建立运行与结果查询链路 | 本次运行标识、结构化输出、错误和等待 | 结果不串轮次、分支或工程 |
| 4 | 补齐流程编辑与调试 | 单步、断点、启用状态、顺序、撤销重做 | 与 GUI/运行引擎行为一致 |
| 5 | 接入插件、相机和通讯 | 能力探测、生命周期和设备工具 | 未确认不产生外部副作用 |
| 6 | 补齐界面业务和人机协作 | 数据源、测量、标注、变量、知识库和设置入口 | 每项操作均有状态及结果证据 |
| 7 | 完成模型执行闭环 | 错误恢复、取消、协议兼容、真实工具回传 | 模拟 LLM 不替生产代码补逻辑 |
| 8 | 完成逐项与现场验收 | 自动化覆盖报告、真实 LLM/设备证据、交接包 | 所声明范围无漏项与未解阻断 |

阶段 2–6 在受影响用例中同步增加模型模拟测试；阶段 7 收口全套交互协议，不能把接口可调用性拖到最后才检查。

### 4.1 分批交付和依赖

默认依次执行 `0 → 1 → 2 → 3 → 4 → 5 → 6 → 7 → 8`。阶段 1 的保护措施是后续全部写操作的前提，阶段 2 的类型契约是设备、变量和标注配置的前提，阶段 3 的结果身份和等待机制是所有耗时操作的前提。

需要先交付等级 A 时，在阶段 4 审核中同时验证阶段 7 的离线请求解析、等待、错误返回和最终结果引用要求；只标记这些子项通过，阶段 7 整体仍为未完成。阶段 5 的真实设备和阶段 6 的真实 SAM 验证可集中在阶段 8，但软件行为和模拟测试不能延期。

### 4.2 跨阶段接口约定

以下是验收所需语义，工具名称可在所属阶段确定。实现者在编码前随该阶段记录给出请求/响应样例；同等简单且兼容的方案可采用，不能通过改名或简化返回字段删除验收要求。

| 接口 | 最小契约 | 冻结阶段 |
| --- | --- | --- |
| 工具结果 | 成功与 error 明确区分；失败有 errorCode/error；批次保留 call ID、已执行/未执行状态 | 1 |
| 确认 | 绑定具体动作、目标和工程/配置版本；拒绝及过期零副作用 | 1；5 补设备目标 |
| 插件描述 | 稳定 pluginId、可用性、参数完整类型及约束、输入输出端口；实例另返 current/effective 参数 | 2 |
| 原子参数更新 | `instanceId + values(object)`；整体校验、整体提交、一条 Undo；reset 与 null 分离 | 2 |
| 执行和结果 | started + runId；按工程/配置/运行/实例/端口定位；状态、错误、分页和 stale 标记 | 3 |
| 流程编辑 | 稳定实例 ID、完整端口边、幂等规则、实际 Undo/Redo 范围 | 4 |
| 设备操作 | 真实能力探测、当前状态、明确目标、参数回读、超时/不确定执行状态 | 5 |
| 人机协作 | awaiting_user_input + requestId + 目标/步骤；完成/取消/过期可追踪 | 6 |

给模型发送的 Schema、Bridge 输入和内部 Actor 调用必须遵守同一合同。已有调用方依赖的成功字段应保留；确需破坏性变化时，增加兼容层或显式协议版本及迁移测试，不能只更新当前测试绕过兼容问题。

### 4.3 合并远端 main 的兼容约束（2026-11）

当前集成基线为本地 `4f9e332` 与远端 `e1663d4`。本节记录整合要求；历史各阶段 PASS 与构建数字仅属于合并前版本，不能作为当前合并版本的验收证据。

- 后台执行按 `docs/superpowers/specs/2026-10-30-background-execution-design.md` 的 job 所有权、暂停和停止协议实现，同时保留本计划的 runId/sessionId、有界循环、工程维护互斥及逐调用副作用授权。启动成功返回的 ID 必须立即可查询；取消发生在真正执行前也必须有终态。
- Agent 保留 confirmationId、原始 callId 与工程/设备/文件快照校验，融合远端流式、上下文预算、图片回灌、审计及 MCP；不能用旧工具注册表覆盖本地扩展工具。隐式执行流程的工具同样执行确认门禁。
- 后台显示帧携带坐标来源与同轮结果快照；旧轮图像不得配上新轮测量结果。视口刷新保留缩放和平移，源图改变时作废旧采点请求。
- 本地逐 slice 契约继续作为忽略文件保留，不随本次合并重新入库。集成日志放在 `build-codex-agent/merge-main-evidence/`。
- 当前环境缺少 ONNX Runtime SDK 头文件，构建暂用 `DEEPLUX_NATIVE_SAM=OFF`；这不构成原生 SAM、CUDA、Windows、真实设备或真实模型服务的验收。

## 5. 阶段 0：基线、能力矩阵与复现样例

**责任范围：** 测试清单、构建配置和审计文档；本阶段不改产品行为。

### 实施内容

1. 扫描全部 `metadata.json`，分别记录模块和相机；与 CMake target、当前二进制、`PluginManager` 运行时加载结果核对。
2. 对未构建、加载失败、SDK 缺失、平台不适用、算法演示模式逐项标记，禁止静默跳过。
3. 建立一份机器可读矩阵 `docs/baseline/agent-capability-matrix.json`，并生成供审核的 Markdown。JSON 是唯一数据源，禁止维护两份互相独立的结论。
4. 扫描 `MainWindow`、流程树、检查器、数据源、相机/通讯、测量、标注、知识库及设置页面中的用户动作，建立操作清单。将每项映射为直接工具、通用工具组合、人机协作或未支持。
5. 为 F01–F13 建立最小复现。特别保留：字符串数组设参、保存无路径、打开损坏工程、Observer 查询手册、异步运行立即查结果、有副作用流程的确认。
6. 构建并执行当前全量 CTest 获取真正基线；外部依赖相关失败另记，保留原始退出码和失败测试名。

### 矩阵的必需字段

每个插件记录 `pluginId/name/kind/sourcePath/cmakeTarget/platform/dependencies`；每个操作记录 `operationId/ownerApi/toolMapping`。共同记录：

| 维度 | 需要的证据 |
| --- | --- |
| 发现 | 能否枚举，是否区分 installed/loaded/unavailable 及原因 |
| 配置 | 默认值、完整可编辑参数、复杂结构、非法值、序列化往返 |
| 输入 | 有效输入或受控 fixture，端口及类型；相机改为设备前置条件 |
| 执行 | 成功、失败、超时/取消；步骤计数或实际副作用 |
| 输出 | 业务值、类型、单位、容差、来源、完整性/截断标记 |
| 权限 | 三种模式、用户确认、拒绝后零副作用 |
| 撤销/持久化 | 对适用项验证 Undo/Redo 和保存重开；不可撤销有理由 |

每格状态为 `PASS/FAIL/BLOCKED/NOT_APPLICABLE/NOT_TESTED`，并附 `testId/evidencePath/reason`。不适用必须说明依据并经审核，不计入通过数。按维度报告分母，不能仅给一个总体百分比。

### 验收标准 G0

**状态：PASS（G0 基线范围；见 `docs/baseline/agent-phase-0-acceptance.md`）。**

- [x] 源码清单与 69 份元数据一致；若仓库新增/删除插件，差异有记录，不硬编码 69 为永久标准。
- [x] 确认当前 65 个加载模块与其余 4 项的差异原因；不能把“缺二进制”写成“通过”。
- [x] GUI 操作清单覆盖上述所有页面，每项有稳定编号和责任阶段。
- [x] 保存基线的原始命令、stdout/stderr、退出码、环境、源码/二进制摘要；相关回归用例可复现。
- [x] 独立审核者能够按记录重现至少 F02、F06、F08。

## 6. 阶段 1：失败语义、权限一致性与工程保护

**责任范围：** `AgentActor`、`AgentController`、`ToolSchema`、`ProjectManager`；必要的确认 UI 和相关测试。

### 实施内容

1. **统一失败返回。** 保留现有成功字段；失败至少包含稳定 `errorCode` 和人类可读 `error`，并标记是否可重试。修复 `save_project` 等只有 `status: failed` 的路径。未知实例、无工程、非法状态必须失败或返回明确的幂等 `no_op`，不得谎称完成动作。
2. **统一入口策略。** 聊天、Bridge、手动确认共用权限判定；Actor 的前置条件验证不能被批准回调绕过。Observer 执行只读工具并继续问答，阻止写操作；Advisor 确认写操作；Autopilot 自动执行允许的编辑，对有副作用/不可逆操作确认。
3. **保护工程切换。** 新建、打开、关闭前检查未保存状态。提供“保存后继续 / 放弃后继续 / 取消”语义；保存失败不继续。用户未明确选择放弃时默认保留当前工程。打开工程先读入并验证候选对象，成功后才替换当前对象和清空撤销栈。
4. **确认内容具体化。** 显示工程、实例、路径、设备目标和操作摘要。确认绑定当前工程及配置版本；确认后目标发生变化则重新校验，不能凭陈旧确认执行。
5. **覆盖间接副作用。** 当前 `Folder` 的 Delete 会递归删除目录，PLC/文件输出插件也有写操作。本阶段先对 Agent 发起的 `run_flow` 和其他执行入口采用保守确认；阶段 5 再按流程实际副作用精细判定。不能因 `run_flow` 本身未标 dangerous 而绕过。
6. **真实审计与撤销标记。** `undoable` 由实际命令能力决定。保存、网络发送、PLC 写入、模型推理和设备动作不能因“非只读”就显示可撤销。
7. **批次协议完整。** 遇错停止后，为每个原始 tool call 返回一次结果；尚未执行的调用返回 `skipped_due_to_previous_error`。参数解析失败也保留原始 call ID。此阶段以停止并报告为基线，恢复策略在阶段 7 完善。

### 验收标准 G1

**状态：PASS（见 `docs/baseline/agent-phase-1-acceptance.md`）。**

- [x] 新工程未指定路径、不可写路径、无工程保存都返回 error；控制器记录失败，后续写操作不执行。
- [x] 打开不存在文件、损坏文件或加载失败，原工程对象、修改标记和撤销历史保持可用。
- [x] 未保存工程切换的保存/放弃/取消均有回归；保存失败不能关闭原工程。
- [x] 三种权限 × 聊天/Bridge/确认入口有测试；Observer 可读手册和状态，所有写入被拒绝。
- [x] 同轮只读+写调用不会绕过权限；拒绝、过期确认、权限降级均不产生未批准操作。
- [x] 包含 Folder Delete 或 PLCWrite 的流程确认前没有文件/设备副作用，测试只使用临时目录和模拟设备。
- [x] 保存失败、缺失实例和非法 pause/resume 的日志与返回值一致；每个 call ID 恰有一个结果。

**验证入口：** 扩展 `test_agentactor`、`test_agentcontroller`、`test_agentpermissions`、`test_agentbridge`、`test_agentundo`、`test_project`。

## 7. 阶段 2：完整参数契约和可发现性

**责任范围：** `ToolSchema`、`AgentActor`、相关 Project 参数写入入口，以及确实缺少参数契约的插件；不重写通用插件框架。

### 实施内容

1. **增加原子批量设参。** 建议工具 `set_module_params(instanceId, values)`，其中 `values` 为 JSON object，值可为 string/number/bool/array/object/null。先合并默认值和当前覆盖值，在最终完整候选参数上验证一次，成功后整体提交并生成一条 Undo；失败时全部不变。
2. **保留兼容入口。** `set_param` 复用同一实现，保留既有数字/布尔字符串兼容。新的明确类型值不做猜测转换；字符串 `"001"`、空字符串和脚本文本按声明保留。区分字段缺失、显式 null 和恢复默认值；恢复默认值使用明确的 reset 操作，不将 null 隐式当删除。
3. **补齐 Schema。** 合并默认值、metadata 和插件明确契约，返回稳定 pluginId、参数类型、默认/当前/有效值、枚举、范围、单位、是否可编辑、数组元素结构和跨字段约束说明。无法推断的结构显式补充，不能从空数组猜元素类型。
4. **补齐端口发现。** `get_module_params_schema` 扩展或增加插件描述工具，暴露输入输出端口、类型、required、multiple、control、结果说明以及可用性。支持在创建实例前查询插件类型，创建后读取实例覆盖值。
5. **修复复杂配置。** 逐项验证 `MeasurementInput` 点/线/面/点集、`CropImage.rectangles`、`StrFormat.inputVariables`、`PLCWrite.values`、`SamPredict.points/negativePoints/box`。SAM 专用工具支持数组不代表 SamPredict 模块参数链路通过。
6. **规范 N 点标定参数。** 将标定点收敛到可持久化的明确结构，例如 `calibrationPoints: [{image:[x,y], world:[x,y]}]`；GUI、Agent 和 process 共用。旧 `pointCount/imgX_i/...` 读取时迁移到新结构；按当前算法检查点数、有限值和退化情况，不仅仅放宽未知参数白名单。
7. **避免半配置死锁。** 例如 min/max、ROI 宽高、SAM prompt 的互斥关系必须能够一次变更；单参数中间态不合法不能阻碍合法的原子更新。
8. **缓存正确性。** 缓存仅存插件静态描述；实例参数实时读取。插件重载或元数据版本变化使缓存失效。

### 验收标准 G2

**状态：PASS（见 `docs/baseline/agent-phase-2-acceptance.md`）。**

- [x] 从工具的 JSON Schema 构造请求，经控制器实际解析进入 Actor；数组/对象/数值/布尔保持类型，禁止测试绕过声明。
- [x] 空字符串可按插件契约设置；缺 value、null、恢复默认分别有测试，错误提示准确。
- [x] 上述五类复杂参数全部完成写入、读取、撤销、重做、保存重开，值与类型完全一致。
- [x] 至少三组跨字段原子更新成功；任一字段错误时整个更新不变且不增加 Undo 记录。
- [x] N 点标定可由 Agent 提供点对并得到有效标定；旧格式迁移和退化点拒绝有数值证据。
- [x] 非法键、超范围、类型错误、非有限数、过大数组拒绝；合法配置不因只检查默认值而漏检。
- [x] 模块描述与 metadata 端口一致；动态菜单值、外部资源不可用情况有清晰描述。

**验证入口：** 扩展 `test_toolschema`、`test_agentactor`、`test_pluginparametercontracts`、`test_measurementinput`、相关插件测试及 `test_projectmigration`；批量用例通过新增的 Agent 入口，而非直接设置插件成员。

## 8. 阶段 3：异步运行、业务结果和错误回读

**责任范围：** `AgentActor`、`AgentController/Observer`、`RunEngine` 的必要查询状态、现有输出数据类型及测试。

### 实施内容

1. **明确运行标识。** 复用已有执行上下文标识；若不能满足查询则补最小 runId。一次运行绑定工程和配置版本，循环模式区分会话与每次执行；不得用累计次数猜测本次结果。
2. **返回实际状态。** `run_flow` 返回 started 和 runId；查询包含 idle/running/paused/stopping/completed/failed/cancelled 等明确状态、isBusy、当前模块和错误。以引擎真实状态派生，不额外维护不同步的布尔字段。
3. **读取模块输出。** 新增按 runId、instanceId、port/key 查询的入口；复用 `moduleOutput/getOutput`，提供测量值、检测列表、表格、类型、单位和来源。错误包含节点、端口、错误码和原始诊断。
4. **防止旧结果混入。** 参数修改、节点删除、工程切换后，结果按快照/版本判定 stale；运行中、失败节点、未执行分支不能返回上次结果冒充本次成功。
5. **控制大结果。** 数值和小对象直接返回；图像、掩膜、点云提供摘要和受控预览/导出引用。表格分页并显式返回总数与截断标记。初始建议每页 100 行、最多 1000 行、单次 JSON 响应 64 KiB；集中定义限制并测试，调整需记录理由。图像尺寸、坐标系、NoData 和单位不得因摘要丢失。
6. **等待和取消。** 提供有界、非阻塞的等待路径，超时返回 pending/timeout，不能把超时当运行失败。无头和 GUI 都能继续响应取消；Agent 轮数预算不能被内部等待空耗。
7. **事件详情可见。** Observer 已收集的失败内容要进入查询/上下文，不能只给 LLM 一个 `RunFinished` 名称。使用受控字段和最近相关事件，不无限导入日志。

### 验收标准 G3

**状态：PASS（见 `docs/baseline/agent-phase-3-acceptance.md`）。**

- [x] 找圆、两点距离、拟合、Blob、3D 平面等用例，Agent 返回值与引擎输出及固定期望值在明确容差内一致。
- [x] 慢模块中立即查询返回 running，不假报成功；完成后查询同一 runId 得到对应结果。
- [x] A 成功 → B 失败/取消、分支未执行、暂停、改参数、换工程后，不能读取 A 的值作为 B 的新结果。
- [x] 缺输入、类型不符、算法失败、服务超时均可定位到具体节点和原因；无结果时明确返回不可用。
- [x] 64 KiB 边界、分页和预览坐标映射有测试；无法序列化的类型显式说明，不静默丢掉。
- [x] 运行及等待期间的事件循环心跳可响应；本机受控慢模块测试中 50 ms 定时任务延迟不超过 500 ms，记录最大值。现场吞吐与推理时延另测，不以此阈值替代。
- [x] 模拟 LLM 立即发结果查询，不调用引擎等待辅助逻辑，生产 Agent 路径仍完成正确闭环。

**验证入口：** `test_agentactor`、`test_agentcontroller`、`test_agente2e`、`test_runengine`、`test_acceptance_flows`；复用 `tests/acceptance/data` 和对应 expected 文件。

## 9. 阶段 4：流程编辑、调试与撤销完整性

**责任范围：** Agent 流程工具、`Project`、`RunEngine`、`ProcessTreeController`、`FlowCanvas` 和相关 Undo；复用现有行为。

### 实施内容

1. 扩展流程状态：工程身份及修改态、节点稳定 ID/类型/名称/启用/断点/备注/位置/顺序、完整连接端口和 edgeType。
2. 提供节点重命名、排序/移动、启用禁用、断点、备注；名称变化不改变实例 ID，不破坏引用。
3. 暴露单步和重置单步状态，复用 `RunEngine::stepOnce/resetStepState`；暂停/继续/停止返回真实状态。运行中禁止结构及参数修改；若现有引擎安全支持暂停时调整断点，仅开放这一明确例外。
4. 提供运行前校验，返回未连接必需端口、类型错误、控制图错误和定位信息；连接与断开保留多端口语义和幂等性。
5. 暴露 Agent Undo/Redo 的明确范围。项目编辑可撤销，外部副作用不可撤销；不从 Agent 撤销按钮悄悄跳过中间不可逆动作。
6. 批量编辑采用已声明的语义：单个参数更新原子；多工具批次失败保留已完成步骤并报告。不要宣称 QUndoStack macro 等于数据库事务或自动回滚。
7. 保存重开恢复节点、边、断点、顺序和参数；兼容当前实际支持的 ProjectFlow。未支持的多流程切换须返回 unavailable，不能仅因模型字段存在就宣称支持。

### 验收标准 G4

**状态：PASS（见 `docs/baseline/agent-phase-4-acceptance.md`）。**

- [x] 同一对节点多条边可查询、精确断开和撤销；数据边、控制边不会混淆。
- [x] 单步仅推进预期节点；断点停在正确位置，继续/重置/停止与 GUI 行为一致。
- [x] 禁用旁路、条件分支、循环、并行和失败传播使用既有运行语义，不通过 Agent 另造调度器。
- [x] 删除节点再 Undo 恢复参数和所有连接；重命名和排序不造成悬空 ID；Redo 重复结果一致。
- [x] 运行中结构/参数编辑被拒绝；两入口交替操作不会产生 GUI、Project、Engine 三套状态。
- [x] 保存重开与操作前后序列化对比只出现预期差异。

**验证入口：** `test_agentundo`、`test_agentactor`、`test_runengine`、`test_project`、`test_flowcanvas`、`test_mainwindow`、`test_acceptance_flows`。

## 10. 阶段 5：插件、相机、通讯与副作用控制

**责任范围：** `PluginManager`、`src/core/device/CameraManager`、`ICamera/ICameraPlugin`、`CommunicationManager`、必要插件元数据和 Agent 工具。

### 实施内容

1. **统一可用性查询。** 返回插件类别、稳定 ID、已安装/已加载、ABI、平台/SDK/模型要求、不可用原因及可执行操作；`installed` 不等于 `loaded`，`loaded` 不等于设备可用。
2. **插件生命周期。** 允许扫描既定本地插件目录、加载、卸载/重载；有活动实例、采集或执行时拒绝卸载，避免对象和动态库悬空。安装只接入现有受控本地包入口；不存在该入口时明确返回需维护者安装，不能承诺自动下载和安装任意动态库。
3. **相机链路。** 发现 → 连接 → 查询能力/当前参数 → 设置 → 回读 → 采集 → 停止 → 断开。参数范围和支持性来自设备 API；设备不支持的设置明确失败。采集输出复用阶段 3 结果通路。
4. **通讯链路。** 复用配置 CRUD、连接状态、发送/接收/超时 API；区分全局通讯配置和流程通信插件的实际连接，不假定两者天然共享。
5. **细化执行确认。** 给运行路径补齐参数相关的副作用描述：文件写入/删除、网络发送/监听、PLC/串口写入、设备参数改变。未知插件效果默认需确认；已证明纯计算的流程可按 Autopilot 策略执行。
6. **确认绑定实际配置。** 预览执行模块、目标、范围和不可撤销性；循环运行以受控会话授权，参数、路径、变量解析后的设备目标改变必须失效或重新确认。重复调用、超时后重试不允许重复发送 PLC 写指令或重复文件删除。
7. **资源释放和线程。** 遵守 QObject/相机 SDK 线程亲和性；不得简单把持有 UI/网络对象的整个 Actor 丢进线程池。连接/采集/推理耗时动作使用已有异步能力或最小异步封装。

### 验收标准 G5

**状态：PASS（计划 §10 软件与证据分流范围；见 `docs/baseline/agent-phase-5-acceptance.md`）。** 阶段 8 的真实设备、模型/provider、目标平台与性能证据仍单独验收。

- [x] 矩阵覆盖 66 模块和 3 类相机驱动的发现/适用性；缺 SDK 或平台不符有机器可读原因（Slice 38 重跑隔离运行时探针，清单 66/3、增删为 0；平台/构建选项/运行时原因码与具体说明均写入唯一 JSON）。
- [x] 插件缺库、ABI 错误、加载失败、使用中卸载、重载后 Schema 失效均可复现并正确处理（Slice 14、27、28；不代表第三方 SDK/二进制兼容）。
- [x] 模拟相机完成 Agent 异步发现/连接/详情/参数确认、采集、停止与释放；无设备、未连接、超时、参数越界和 SDK/读回失败可见；GrabImage 消费同一 Agent async session 的采集结果（Slice 29；无 request-aware 帧 ID 时一次 capture 后退役 session，硬件/厂商 SDK 仍归阶段 8）。
- [x] 本地模拟 TCP/PLC 和可用的串口测试端完成收发校验；生产 IP、真实串口和现场设备不用于自动单元测试（Slice 6 的 PLC Modbus/TCP 假服务往返与 Slice 11 的 POSIX PTY 双向串口字节回归均已独立 PASS）。
- [x] 对直接工具和“配置插件后 run/step/resume”两条路径，确认前副作用计数为 0；确认一次产生预期次数，拒绝为 0。（Slice 127 汇总及独立审核 PASS；成功多轮 cycle 专项空格单独披露。）
- [x] 目标或配置变化使旧确认失效；无法确认真实执行是否已发生的超时返回不确定状态，不自动重复不可逆动作。（独立审核 PASS，边界与剩余 TOCTOU 限制见 Slice 127 记录。）
- [x] 真实硬件证据单列为阶段 8 门槛；模拟通过仅证明接口和协议。（阶段 8 现场项目仍为 NOT_RUN，不据此宣称现场通过。）

**验证入口：** `test_pluginmanager`、`test_cameramanager`、`test_communicationmanager`、`test_agentpermissions`、`test_agente2e` 及通讯插件测试。新增独立测试时在 CMake 中显式依赖对应插件。

## 11. 阶段 6：界面业务、人机协作与知识库操作

**责任范围：** `MainWindow`、`DataSourcePanel`、测量交互、Annotation/SAM、变量、设置、知识库的既有业务实现与 Agent 接口。

**状态：PASS（阶段 6 软件验收范围）。** G6 标准 1–6 全部通过；全量构建成功，Linux/Qt 5.15.3 隔离环境 CTest 108/108 通过。真实设备、provider、GPU 和 Windows 验收留在阶段 8。

### 实施内容

1. 从阶段 0 操作清单逐项补齐业务入口；尽量让 GUI 和 Agent 调用同一现有方法。只对实际重复逻辑抽取共享函数，不建设通用 UI 自动化框架。
2. 数据源：导入、枚举、显示、可见性、绑定模块、移除引用。明确移除工程引用与删除磁盘文件的区别，默认保留源文件。
3. 测量/ROI：可用坐标直接配置；需要用户在图像/点云采点时返回 `awaiting_user_input` 和具体对象、步骤、坐标系。支持确认、取消、图像切换失效，不能返回“采点完成”占位。
4. SAM：复用会话、MaskOps、导入和现有 LabelMe/YOLO/COCO 导出器，支持对象查询、标签修改、删除、掩膜编辑/合并拆分和导出中当前产品已实现的动作。先核验各动作现有实现，缺实现的项目明确登记，不因导出类文件存在就自动视为可用。
5. 全局变量与队列：按实际类型查询、创建、更新、移除；表达式和值要区分，运行期修改遵守既有锁和业务约束。消息中不暴露凭据或与任务无关的敏感配置。
6. 视图和设置：选择节点、查看输出、2D/3D 渲染模式、面板显示/主题、打开相关设置页面。纯界面动作依赖 GUI；无 GUI 时返回 `requires_gui`，不能宣称成功。
7. 知识库：列出、预览、检索、导入和删除用户文档；保留来源，内置资源只读，删除不影响导入前的源文件。继续使用当前 Markdown/TXT 检索，结果分页或限长。
8. 登录、凭据、SDK/驱动/模型环境安装：提供具体的人机交接状态和跳转入口；完成状态来自真实会话/资源检查。不能读取密码转发给 LLM，也不能在缺少权限时伪造登录。

### 验收标准 G6

- [x] 阶段 0 每个界面操作都能映射到工具/组合/人机协作入口；未完成项显式列出，不从矩阵移除（G6 标准 1 独立复审 PASS，结论限定于 126 项清单和静态引用门禁；见阶段 6 验收记录）。
- [x] 数据源与变量操作从 Agent 发起后，GUI 及时反映真实状态；适用的工程状态可保存重开（Agent 数据源/GrabImage 保存重开、数据源 GUI 同步、全局变量进程态 CRUD 信号刷新经独立复审 PASS；排序/表达式执行/应用仍为人工操作，见阶段 6 验收记录 G6 标准 2）。
- [x] ROI、测量和标注坐标来自原图/点云坐标；缩放、裁剪、多视口、换图和取消有回归，不能用屏幕坐标冒充算法坐标（G6-3：Slice196–199 20行矩阵及独立 AUDIT_PASS；范围限定当前仓库69项内置插件，动态第三方插件和阶段8设备/GPU验证另行验收）。
- [x] 人机采点等待期间可取消；迟到回调不会把结果写入已换掉的工程、图像或模块（阶段 6 Slice54：Project/module identity、target generation/snapshot、viewport QPointer/contentGeneration 与运行 generation 集成回归，独立实现复核 PASS；Slice201 在相同 Project/viewport 与完全相同模块 JSON 下替换目标实例，独立验证 3D `parameterInstanceGeneration` guard，并要求后续新请求成功、runId 更新且可核对，当前聚焦用例 6/6、完整 `test_mainwindow` CTest 1/1、独立实现复核 PASS。证据见 `build-codex-agent/agent-phase6-slice201-refresh-evidence/`；范围限模拟 world-pick，不替代真实相机/GPU 点击验证）。
- [x] 标注导出内容与会话逐对象比对，包含标签、尺寸、坐标、数量；失败不破坏已有目标文件（阶段 6 Slice53 独立实现复核 PASS；见 `docs/baseline/agent-phase-6-acceptance.md`）。
- [x] 知识库导入/预览/检索一致；内置文档不能删除，源文件不受用户库删除操作影响（阶段 6 Slice 1 独立复审 PASS；见 `docs/baseline/agent-phase-6-acceptance.md`）。
- [x] 无 GUI/无图像/无登录权限/无模型环境时返回真实前置条件缺失，界面测试包含操作前后截图和行为断言（阶段 6 Slice 163 独立复核 PASS；不同入口按实际错误码验收；Slice 200 修复 viewport 确认执行中的数据源悬空指针，最终 108/108 CTest 包含该套件；见阶段 6 验收记录）。

**验证入口：** `test_mainwindow`、`test_agent_draft_restore`、`test_annotation_coordinate_mapping`、`test_annotation_session`、`test_annotationmaskops`、`test_samannotatordialog`、现有 exporter 测试、`test_knowledgebasedialog`、变量测试和新增 Agent 入口用例。

## 12. 阶段 7：LLM 协议、失败恢复和可解释闭环

**责任范围：** `AgentController`、`ILLMClient/OpenAILLMClient`、`ToolSchema`、Agent 对话/日志、知识库能力说明和 E2E 测试。

### 实施内容

1. **以已注册能力生成提示。** 告知模型先发现能力、读 Schema、原子配置、校验、运行、等待、读结果再下结论。查询型需求不创建工程、不启动设备；无工具能力时明确退回问答。
2. **统一调用协议。** 测试真实发送的 JSON Schema 与响应 arguments 解析；非法 JSON、未知工具、缺参和类型错误形成带 call ID 的错误结果。只声明已验证的 OpenAI-compatible 协议，不把一个 `toClaudeTools` 函数名当作原生 Claude 协议已支持的证明。
3. **有限恢复。** 可修正参数错误反馈给模型继续，默认每次用户请求最多 3 次纠错重试，并受已有 20 轮/每轮 10 工具上限约束；同一错误连续 2 次未改变有效参数则停止并说明原因。用户拒绝、权限失败、不可逆操作执行状态不明不得自动重试。
4. **正确处理中断。** 用户取消、模型断连、关闭对话、关闭工程和关闭窗口时，清理或拒绝迟到响应；明确“取消 Agent 请求”和“停止流程”作用范围。已完成的副作用保留审计，不谎称自动回滚。
5. **保留对话合法性。** 每个 tool call 恰有一个结果；批次未执行、用户拒绝、超限和上下文裁剪也不留下孤立消息。裁剪保留当前工程、当前任务、运行/确认标识和最近错误所需上下文。
6. **兼容和限制。** 用本地 HTTP fixture 验证当前支持的 provider payload、工具开关和图像输入。文本连接测试成功不代表 Function Calling 成功，增加无副作用工具回传测试；真实服务在阶段 8 验证。
7. **用户可见状态。** 展示当前动作、等待原因、待确认目标和失败位置；结果引用 runId/节点/实际值。日志不把“请求已发送”“开始运行”显示为“任务完成”。

### 验收标准 G7

**状态：PASS（软件/离线范围；见 `docs/baseline/agent-phase-7-acceptance.md`）。**

- [x] 模拟 LLM 仅产出响应，不直接调用 Project/Engine，不等待引擎、不预置实际业务结果；生产代码独立完成执行和等待。
- [x] 覆盖问答、从零建流程、修正参数、读本次结果、取消、拒绝确认、模型断连、超时、轮数上限和迟到响应。
- [x] 参数错误可在有界次数内纠正；重复错误可靠停止；PLC/网络写入不因模型重发被无条件重复。
- [x] 用严格的请求/响应 fixture 验证最终发送内容，数组参数不依赖模型违反 Schema；工具关闭时能力描述正确。
- [x] 对真实测量结果的最终回复能追溯到工具返回，任何缺结果/不确定执行都不会生成“已成功”的固定收尾。
- [x] Agent UI 的取消、等待、确认和失败行为有自动化断言；凭据不进入测试日志或截图。

**验证入口：** `test_toolschema`、`test_agentcontroller`、`test_agentpermissions`、`test_agentbridge`、`test_agente2e`、`test_agent_sam_tools` 和必要 UI 测试。

## 13. 阶段 8：覆盖验收、现场验证与交接

**责任范围：** 自动化验收、逐插件矩阵、目标平台/设备实测、用户手册与发布说明。原则上不混入新功能；缺陷修复回到所属阶段补证。

### 13.1 自动化验收

1. 按源代码清单生成测试数据，遍历所有目标配置启用的模块；对每个模块走 Agent 参数配置、输入连接、运行、结果查询，并断言业务输出。相机走设备操作链，不强行套 `add_module`。
2. 每个插件至少一个有效业务 fixture 和一个关键失败 fixture；多模式插件覆盖所有对外模式。枚举、数组/对象参数和跨字段关系使用边界数据，不能只测试第一个参数。
3. 多实例/分支、撤销、保存重开、异步取消在通用层覆盖，并对复杂插件补针对性回归。拒绝通过“创建插件成功”代替运行与输出断言。
4. 报告同时给出源码总数、目标平台适用数、构建数、加载数、配置通过数、业务执行通过数、现场已验收数。增删插件导致清单差异应让覆盖检查失败并提示补项。
5. Linux Qt 5/Qt 6 完成全量 CTest；Windows 至少按既有 CI 完成产品构建，并在宣称支持的 Windows 驱动/界面环境执行相应用例。配置不足时保留未验证状态。

### 13.2 必须保留的代表场景

| 场景 | 核心断言 |
| --- | --- |
| 文件图像 → FindCircle → 查询结果 | 圆心/半径与 expected 一致，回复引用本次运行 |
| MeasurementInput → FitLine/FitCircle | 数组点集完整输入，拟合数值正确，撤销/重开保真 |
| CropImage 多矩形 | 裁剪数量和尺寸正确，部分越界与全部无交集行为正确 |
| N 点标定 | 点对可配置，矩阵/误差真实，退化数据失败 |
| 高度图 → 3DPreProcessing → FitPlane/GapMeasure3D | NoData、单位和输出容差不丢失 |
| 条件/循环/并行/断点/单步 | 执行节点序列、分支结果及取消状态正确 |
| SaveImage/SaveData/WriteText/Folder | 权限、文件内容与失败语义正确；删除只测试专用临时目录 |
| 相机 → GrabImage → 检测 | 设备选择、参数回读、实际帧、断连处理正确 |
| 结果 → PLC/TCP/串口 | 协议载荷与寄存器值正确，确认前零发送，失败无盲目重试 |
| SamPredict → RegionStats；SAM 会话 → 导出 | 实际模型输出、标签/坐标、会话和导出一致 |
| Observer 查询手册与工程 | 可查询、可继续问答，不能改变工程或外部系统 |
| 未保存工程切换、损坏工程打开 | 数据保留，保存失败不继续，不丢撤销历史 |

以上是必选组合场景，不替代其余插件的逐项矩阵。

### 13.3 真实 LLM 与现场证据

- 每个声明支持的 API/provider 配置，执行固定的至少 10 项自然语言任务，每项重复 3 次，记录模型标识、配置、完整脱敏 tool calls、工具结果、最终回复与任务断言。
- 任务包括问答、建流程、数组配置、结果读取、改错、拒绝确认、取消、设备前置条件缺失、保存/重开和标注；测试文案固定，不能观察结果后改任务来提高通过率。
- 初始门槛：任务完成率至少 90%；数据丢失、越权副作用、错误结果被断言成功为 0。重复任务的不同运行需隔离状态；失败案例全部保留并解释，不选择性汇报。
- 真实相机记录型号、驱动/SDK、固件、参数回读、帧摘要和断连恢复；真实 PLC/串口/TCP 在批准的测试设备验证目标地址、载荷和回读。
- SAM 使用真实权重与声明的 CPU/GPU 环境，记录首次加载、首次编码、重复预测、错误/取消和输出。模拟服务仅证明传输和参数，不证明分割质量或真实性能。
- 时延报告包含样本数、冷/热启动、P50/P95 和环境；业务速度阈值由目标场景给定，未给定时只报告测量值，不编造性能达标。

### 验收标准 G8

当前进度（2026-11-03）：G8 正式当前验收矩阵已生成并经 `/root/defect_detection_g8_review` 独立复审 PASS：`docs/baseline/agent-g8-current-acceptance-matrix.json` 是唯一数据源，Markdown 由生成器确定性渲染。当前 CMake 请求 Qt6，但生成的 test_mainwindow link line 证明实际构建链接 Qt5；该回退在矩阵单独列出，Qt6 未测试。矩阵对账 69 项源码插件、当前 67 个构建二进制与历史 66 个二进制基线；新增可见项为 DefectDetection demo-mode framework binary，不代表算法 SDK 可用。66 个插件的有效业务 fixture 与关键失败 fixture 均经独立审核通过；`DefectDetection` 的 demo-mode 二进制和 SDK 缺失失败路径也经审核，但有效算法 fixture 为 FAIL（HymsonVision3D SDK 未提供）；其操作手册、插件手册和 Agent 内置知识库现已同步标明算法不可用，`read_documentation` 三源检索通过。DirectShow 缺 Windows 平台，Hikvision 缺 MVS SDK。G8 Operation Evidence Slice 1–20 共 97 项测试映射经 `/root/slice176_final_review` 独立复审 AUDIT_PASS；Slice 21、Slice 22 和 Slice 23 各增加的 1 项映射均已独立复审 PASS；Slice 24 新增的 GUI 全流程重跑行为记录已独立复审 PASS，Agent 单模块重跑继续为未支持；Slice 25 新增的合成凭据登录提交 GUI 记录已独立复审 PASS；本地简化 UserManager 不构成密码认证。 Slice 26 通讯配置增删记录已在修复“销毁首个窗口后重新创建视图”的审核意见后独立复审 PASS；Slice 27 状态栏局部显示记录已独立复审 PASS；Slice 28 设置窗口的本地合成 HTTP 连接测试已独立复审 PASS，不代表真实 provider 或 Function Calling；Slice 29 通讯设置窗口的 loopback TCP 连接/断开 GUI 证据及异步状态同步修复已独立复审 PASS，Agent 通讯工具仍未支持。Slice 30 为 `FILE_EXIT` 增加独立进程中的真实菜单退出/窗口 close 与空闲相机 shutdown gate 测试，focused CTest 1/1、完整 CTest 113/113；它只证明用户关闭窗口路径，不授权 Agent 终止宿主，并经 `/root/slice176_final_review` 独立复审 PASS。Slice39 扩展 ImageScript Agent fixture 覆盖反转、灰度、模糊、锐化四个公开模式，以固定像素矩阵和 same-run provenance 验证；Slice40 扩展 Math fixture 覆盖 Add/Subtract/Multiply/Divide/Modulo/Power/Min/Max，并验证 Divide/Modulo 零除失败；Slice41 扩展 PerProcessing fixture 覆盖 GaussianBlur/MedianBlur/BilateralFilter/Sobel/Laplacian/Canny/Dilate/Erode/Open/Close/Gradient 十一个 OpenCV 分支；Slice42 通过 Agent 真实数据流覆盖 Condition 九个比较符，保留 Equal false 与缺变量失败。Slice42 已完成 Linux/Qt 5.15.3 全量回归和独立终审；Slice43 则通过 StopWhile 受控流覆盖 While 六个 comparator 真值路径并验证空值实际传递，当前全量 113/113 与矩阵重生成通过、契约、实现和最终归档均经独立审核通过，证据见 `slice43-full-regression-summary.txt`。Slice44 已通过 Agent 合成图像覆盖 ColorRecognition 六种目标色和红色 wraparound 区间，契约、实现及归档均独立审核通过；Slice45–50 随后补齐 Blob、GrabImage、FitLine、NPointCalibration 和 MeasurementInput 的公开离散模式，并经独立契约/实现/归档审核。真实相机、SDK、provider、SAM 权重/GPU和现场门槛仍未完成。当前 126 项 GUI 操作均有可解析证据链接；`TERMINAL_INPUT` 保持 `HUMAN_HANDOFF`，证据只确认 Agent 禁止 shell/命令执行；相机配置删除/应用的 GUI-only 证据也不改变其 `UNSUPPORTED` 映射；映射类型另列为 66 直接工具、37 人机协作、18 未支持、5 工具组合。所有切片保持原映射分类；相机/通信设置操作的 GUI 测试没有被当作 Agent 配置或真实设备能力，用户测量仍是人机协作。现场验收 0/69，矩阵总状态 `NOT_READY`，真实 LLM/现场证据未完成，G8 保持未通过。候选库存仅用于源码候选索引，不替代正式矩阵。逐插件记录见 `docs/baseline/agent-g8-current-acceptance-matrix.md`。

Slice45 已覆盖 Blob 的 Otsu、Fixed、Adaptive 三种阈值分支；Slice46 已覆盖 GrabImage Path、Demo、Folder 模式；Slice47 补强 Agent 假相机 same-run 原始像素证据，并将 supporting test IDs 纳入自动校验。三个切片的契约、实现和最终归档均独立审核通过；Slice47 Linux/Qt 5.15.3 全量 CTest 113/113。G8 整体验收仍未通过：实体相机/SDK/provider/SAM 权重与 GPU/现场验收均需要当前构建环境之外的证据。

Slice48 已补齐 FitLine Agent fixture 的公开拟合方法分支：固定六个水平内点和一个离群点，分别经 `run_flow` 执行 RANSAC、LS，并按各自 runId 核对 `line_rho`、模 180° 的 `line_phi` 和包含全体七点的 `line_error`；既有退化输入仍明确走 RANSAC。契约、实现、最终归档分别经 `/root/slice176_final_review` 独立审核 `CONTRACT_PASS` / `IMPLEMENTATION_PASS` / `FINAL_ARCHIVE_PASS`。全量构建成功，聚焦 CTest 1/1；受限环境全量 CTest 首次 96/113（17 项 loopback/socket 与相机管理 SIGSEGV 环境失败），其原始日志保留，获准 loopback 复跑 113/113。G8 矩阵用本轮全量日志重生成且 `--check` 通过，总状态仍 `NOT_READY`。证据见 `build-codex-agent/agent-phase8-slice48-evidence/`。该切片仅验证固定软件点集，不证明真实数据精度或现场能力。

Slice49 扩展 `testG3NPointCalibrationOutputPortsByRunId`：同一组固定梯形点对分别经 Agent Perspective 与 Affine 模式运行；Perspective 的 reprojection error 小于 0.001，Affine 在当前 `estimateAffine2D` 默认 RANSAC 下为 3.4–3.6，且按本次 runId 通过 key/port 检查标定有效性、点数与误差，既有空点失败路径保留。契约、实现、最终归档分别经 `/root/slice176_final_review` `CONTRACT_PASS` / `IMPLEMENTATION_PASS` / `FINAL_ARCHIVE_PASS`。全量构建成功，`test_agentphase0_repro` 在获准 loopback 下 1/1、全量 CTest 113/113；受限环境首轮 PLC listener 失败原始日志单独归档。G8 矩阵以本次日志重生成并 `--check` 通过，overall 仍 `NOT_READY`。证据见 `build-codex-agent/agent-phase8-slice49-evidence/`。仅证明固定软件点集与当前 OpenCV 默认行为，不代表现场标定精度。

Slice50 扩展 `testAgentSuppliesPointPairAndReportsDimensionMismatch` 覆盖 MeasurementInput `custom` 模式：Agent 一次配置 point1/point2/point、line/line1/line2、plane，逐项经同一成功 runId 的 key/port 返回原值，并按 key 确认模式为 `custom`；后续维度不匹配失败 run 的八个结果 key 与七个结果 port 全部 `unavailable` 且无 value，保留原模块归因和精确诊断。契约在编码前经 `/root/slice176_final_review` `CONTRACT_PASS`。全量构建成功，visual-plugin focused CTest 1/1、获准 loopback 全量 CTest 113/113；current matrix 与库存 `--check`、两套 auditor 测试 20/20、格式与 diff 检查通过。矩阵仍为 69 插件、126 操作、123 linked/3 not linked、现场 0/69、overall `NOT_READY`。证据见 `build-codex-agent/agent-phase8-slice50-evidence/`。实现、最终归档分别经 `/root/slice176_final_review` `IMPLEMENTATION_PASS` / `FINAL_ARCHIVE_PASS`。

Slice51 将已有 `testCameraSetViewDeleteAndApplyRemainGuiOnly` 挂接到 `CAM_DELETE_CONFIG` 与 `CAM_APPLY_PARAMS`：该测试验证删除仅移除表格行、应用仅记录 IP/端口并关闭窗口，CameraManager 与相机设备调用均不变。两项映射继续为 `UNSUPPORTED`，证据仅代表 GUI handler 行为。矩阵检查、候选库存检查、两套审计测试 20/20 与 `git diff --check` 通过；`build-codex-agent/agent-phase8-slice51-evidence/` 内 14 项 SHA 校验通过；独立审查 `/root/slice176_final_review` PASS。当前为 125/126 linked、1/126 not linked（人类专用终端输入）、现场 0/69，G8 仍 `NOT_READY`。

Slice52 在 Slice201 测试补强后重跑完整构建和授权 loopback CTest，113/113 通过、0 失败/跳过；当前验收矩阵已由本轮原始 CTest 输出生成，Markdown 同步刷新。矩阵维持 69 插件、67 当前二进制、66 业务 fixture PASS、1 SDK-blocked failure、2 项未审（平台/SDK）、125/126 GUI 证据链接、现场 0/69、overall `NOT_READY`。完整 CTest 日志、构建结果、矩阵/审计验证及哈希见 `build-codex-agent/agent-phase8-slice52-evidence/`。

Slice53 将 `TERMINAL_INPUT` 关联到 `testBuiltinDocsStaySourcedAndAuditArtifactsStayExcluded`：该测试只证明 Agent schema 白名单拒绝 shell/命令执行及 terminal tab selector 不是执行入口，不测试终端 UI、PTY 或命令，也不启动用户 shell。`TERMINAL_INPUT` 仍为 `HUMAN_HANDOFF`、`currentTool: null`。用 Slice52 113/113 全量日志确认 suite 通过后重生成矩阵，证据链接现为 126/126；其余映射不变，现场仍 0/69、G8 `NOT_READY`。契约 `本地逐切片契约（未纳入提交）` 与 Slice53 证据包经独立审核。

Slice54 修正当前矩阵对 Qt 版本的环境描述：CMake 的 `USE_QT6=ON` 是请求而非已选版本；生成的 `test_mainwindow` link line 显示实际链接 Qt5，因此矩阵记录 requested=6、selected=5、fallback=true 和证据路径。新增 Qt5/Qt6/fallback/ambiguous/unknown 与默认日志路径一致性测试后，两套 auditor tests 22/22；current matrix 重生成、无参数 `--check` 与 inventory `--check` 通过。全量 113/113 仍来自 Slice52 同一日志；未声称 Qt6 已验证。契约与证据见 `本地逐切片契约（未纳入提交）`。

- [ ] 每个声明支持的插件和软件操作都有通过证据，矩阵无未说明缺格；不适用项有审核依据。
- [ ] 目标配置适用插件的 Agent 契约测试通过率为 100%，关键业务 fixture 全部通过。
- [x] 全量构建/CTest/格式检查通过；记录命令、退出码和实际执行数量，测试过滤器匹配 0 项视为失败。（最新全量回归 Slice52；请求 Qt6、实际回退 Qt5.15.3 全量构建退出 0，visual-plugin focused CTest 1/1、获准全量 CTest 113/113、0 失败/跳过；矩阵/库存生成与 --check、21 项 auditor tests、格式和差异检查通过。其他平台/现场门槛仍单独验收。）
- [ ] 真实 LLM 达到上述门槛；所有现场设备/模型项具有独立证据，否则只能交付 A/B 等级。
- [x] 用户手册、插件说明、Agent 内置知识库和 Schema 同步；审计文档不作为运行时操作事实自动加入知识库。（Slice31–36；69 篇插件文章逐项独立 PASS，runtime 来源/隔离、缺陷检测可用性说明及 shell 能力边界测试独立 PASS。G8 其他门槛仍未通过。）
- [ ] 最终审核列出交付平台、设备、模型、已知限制和可复现问题；声明范围内没有未解决的 P1 阻断。

## 14. 构建、测试和证据要求

### 14.1 使用本工作区构建目录

执行前先检查路径和工作区：

```bash
pwd
git status --short --branch
git rev-parse HEAD
rg 'CMAKE_HOME_DIRECTORY' build-codex-agent/CMakeCache.txt
```

`CMAKE_HOME_DIRECTORY` 必须为当前 checkout。若需要新的 Qt 版本配置，建立当前 checkout 专属 build 目录，不覆盖另一配置。Qt 实际版本同时以 CMake 找到的包路径及测试启动日志核验，不能只看 `USE_QT6` 布尔值。

阶段 0 和最终验收先完整构建所有启用插件及测试：

```bash
cmake --build build-codex-agent -j2
env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure
git diff --check
```

日常阶段可用以下已存在的测试组作为起点，另加阶段新增或受影响的测试：

```bash
env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent -N -R 'test_(agent.*|toolschema|pluginparametercontracts|runengine|project|projectmigration)$'
env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure -R 'test_(agent.*|toolschema|pluginparametercontracts|runengine|project|projectmigration)$'
```

仅构建 `test_pluginparametercontracts` 未必会重建所有被动态加载的库；需核对依赖和库摘要，不能读取旧插件后宣布当前源码通过。测试使用临时 app data/plugin 目录；需要 `sync-plugins` 时限定隔离目录，避免覆盖用户日常插件。需要图形系统的截图测试按环境运行，offscreen 不支持的测试单独记录原因和替代证据。

### 14.2 每阶段交付包

摘要建议落在 `docs/baseline/agent-phase-N-acceptance.md`；大日志、截图、结果 JSON 放在 CI artifact 或忽略的构建目录内，摘要引用其可获取位置及 SHA-256。不要提交密钥、个人设置、编译产物或无关日志。

每阶段必须包含：

1. 需求/F 编号与本阶段改动文件、原因、兼容性变化。
2. 起始和结束 revision；若有脏树，附限定范围 diff、相关未跟踪文件清单和摘要。
3. 构建/测试原始命令、退出码、实际用例数、失败/跳过明细；不只提供截图或一句“通过”。
4. 输入 fixture、工具调用 JSON、实际输出、期望值/容差和业务断言结果；GUI 变化附前后截图。
5. 能力矩阵变化，代码测试/模拟服务/真实服务/真实设备证据分类。
6. 需要进一步验证的事项和明确责任人；回退方式只作用于本阶段改动，保留此前和用户工作。
7. 独立审核结论及证据位置。

可复用记录模板：

```text
阶段：
实现负责人：
审核负责人：
开始 revision / 工作区摘要：
结束 revision / 工作区摘要：
覆盖 F 编号与操作编号：
改动文件与行为：
构建命令 / 退出码：
测试命令 / 实际用例数 / 退出码：
输入 / 实际输出 / 期望 / 容差：
原始证据位置与 SHA-256：
矩阵 PASS / FAIL / BLOCKED / NOT_APPLICABLE / NOT_TESTED：
现场依赖与未验证项：
回退方式：
审核结论：PASS / FAIL / BLOCKED_EXTERNAL
审核依据、日期和后续阶段放行范围：
```

## 15. 交给外部实现 Agent 的起始任务

> 阅读本计划、当前 AGENTS.md 和相关源码。首先仅执行阶段 0：核验工作区与构建目录，保护现有修改，生成完整插件与操作矩阵，复现 F01–F13 并记录真实构建/测试基线。交付阶段 0 验收记录及原始证据，状态为 READY_FOR_REVIEW。不得自行标记 PASS 或跳到后续阶段。后续每阶段复用原有业务 API，按对应门槛实现、测试、更新矩阵并交给独立审核；只有审核通过后进入下一阶段。缺硬件/模型时准确记录阻断，不用模拟结果冒充现场验证。

独立审核 Agent 的任务是复核范围、代码、退出码、业务值及副作用证据；不得仅根据实现 Agent 的完成声明放行。发现错误时给出文件位置、复现步骤和必须补充的证据。
