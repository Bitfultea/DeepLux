# 阶段 4 验收记录：流程编辑、调试与撤销完整性

> 历史验收快照：本文状态与结果对应其记录日期和基线；当前项目状态见 [docs/STATUS.md](../STATUS.md)。

依据 `docs/agent-capability-improvement-plan.md` §9。前置阶段 G3 已于 2026-10-12 经独立审核 PASS。**当前状态：G4 PASS（2026-10-22 独立六项总审）；第 1–27 片均获各自切片 PASS；允许进入阶段 5。**工作区仍有阶段 0–3 和用户既有未提交修改；本工作未提交、未推送。

## 1. 第 1 片：可查询的工程与连线状态

数据流：`get_flow_state` → `AgentActor::executeTool`（白名单和参数校验；只读查询不受忙态编辑门限制）→ `ProjectManager::currentProject` → `Project::modules/connections` → JSON。此前模型保存了节点位置、备注、启用、断点、顺序和连线端口/边类型，但 Agent 只返回节点 ID/名称/参数与连线两端 ID；问题位于最后的序列化分支，而不是工程持久化层。

本片保留既有 `modules`/`connections` 与 `from`/`to` 字段，同时增加工程 ID/名称/路径/修改态、按列表顺序编号的节点完整持久化字段和端口声明，以及连线的 `fromPort`、`toPort`、`edgeType`。`executionScope=project_modules` 与 `flowSwitchSupported=false` 明确当前运行路径未支持多流程选择，不因文件格式含 `flows` 就宣称可切换。对于旧连接中未保存的 `edgeType`，当前先原样报告空值，不猜测为 data/control；有效类型推断及可定位校验留待本阶段后续片。

`test_agentphase0_repro::testF03SchemaAndFlowStateExposePorts` 翻转阶段 0 的连线字段缺失断言；`test_agentactor::testG4FlowStateReflectsProjectModel` 覆盖工程身份、脏标记、节点顺序/位置/备注/启用/断点/端口、数据边和已有控制边类型回读，以及保存重开一致性；`testG4ParallelPortsDisconnectPreciselyAndUndo` 覆盖同一节点对的两条不同端口边、模糊断开拒绝、精确断开及 Undo/Redo。控制边由测试直接加入 Project，只证明查询与持久化，不证明 Agent 能创建。定向结果分别为 3/3、4/4，退出码 0（均包含 init/cleanup）；`test_toolschema` 9/9，退出码 0。

## 2. 第 1 片原始验证与边界

- 起始/当前 HEAD 均为 `b516f54`，分支 `agent/development`；工作区含先前阶段的未提交修改，本片只新增/调整 `AgentActor.cpp`、`ToolSchema.cpp`、`test_agentactor.cpp`、`test_agentphase0_repro.cpp`、操作清单和本记录。没有修改同级 `DeepLux` 工作区。
- `cmake --build build-codex-agent -j2`：最终轮退出码 0，原始日志 `build-codex-agent/agent-phase4-evidence/build-slice1-final.log`。构建缓存的 `CMAKE_HOME_DIRECTORY` 指向本工作区；已核对本地 `DeepLux` 与测试二进制时间戳，系统 PATH 另有同名安装版，验证始终使用 `build-codex-agent/bin/` 路径。
- `QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure -j2`：最终轮 78/78、0 失败、0 跳过、退出码 0，原始日志和 JUnit 为同目录 `ctest-slice1-final.log`/`ctest-slice1-final.xml`。首轮执行时自动权限审核超时，测试未启动；重试与最终轮均成功。`actor-slice1-final.log`、`f03-slice1-final.log` 和 `toolschema-slice1-final.log` 保留定向原始输出。
- 矩阵重生成后 69 插件 × 7 维度、109 GUI 操作，483/483 引用校验通过；本片未将流程状态查询通过外推为其他编辑/调试操作 PASS。`clang-format --dry-run --Werror` 和 `git diff --check` 均退出码 0。
- 阶段 3 的 `sha256sums.txt`/`binary-manifest.txt` 锁定的是进入阶段 4 前的快照；本片修改同一源码并重建二进制后，不把旧清单对当前工作树的失配解释为 G3 回归，也不声称它们仍是当前摘要。阶段 4 使用独立证据目录。
- 第 1 片当时的源码/证据与二进制分别由同目录 `sha256sums-slice1-final.txt`、`binary-manifest-slice1-final.txt` 锁定；进入第 2 片后相关源码和二进制已有新版本，旧清单仅作历史快照，不用于校验当前工作树。此前轮次日志和清单保留，不覆盖历史。

## 3. G4 当前总审状态

- Slice1–27 已分别获得切片 PASS；这不自动代表阶段总验收通过。
- 2026-10-22 首轮独立阶段总审判为 NEEDS_CHANGES：G4 #1/#6 发现 Agent 断开中间连接后 Undo 改变序列化连接顺序；G4 #5 发现 note、enabled、breakpoint 和节点顺序的 Agent 旧 Undo/Redo 可能覆盖后续 GUI 编辑。对应整改见后续切片合同与结论。
- 2026-10-22 按累计整改证据完成独立 G4 六项总审，结论 **PASS**：#1/#6 由 Slice25 精确连接索引恢复覆盖；#2 由 Slice10/19 同 runId 单步/断点交替恢复覆盖；#3 由 Slice13 复用 RunEngine 验证；#4 由 Slice3/5/25/27 覆盖；#5 由 Slice6–8/18–21/26–27 覆盖字段与顺序 stale 保护及宏隔离；#6 由 Slice1/2/3/5/21/25 保存重开和序列化对比覆盖。总审未独立重跑全量构建/CTest，依赖已归档切片证据；Slice27 最终快照 loopback 78/78、受限 73/78（五项已知 listener 限制）、定向 3/3。
- 第 25 片针对连接原索引恢复，已获独立切片 PASS；全量构建、受限/授权 loopback CTest 和摘要校验完成。
- 第 26 片 `note`/`enabled`/`breakpoint` 异源 Undo 保护已获独立切片 PASS；该片仅覆盖三个运行字段。第 27 片另行关闭模块顺序跨入口 stale Undo 风险，两个切片均不替代 G4 阶段总审。
- 第 27 片模块顺序异源 Undo 保护已获独立切片 PASS：外部顺序代次、成功/失败 reload、重复 ID fail-closed、GUI 与外部 ABA、Agent move/add/remove LIFO、批次隔离均有回归；执行方全量构建 exit 0、受限 CTest 73/78、授权 loopback 78/78、定向套件 3/3。独立复核者核验实现、源码/证据摘要 23/23、二进制摘要 6/6、XML/格式/diff 门禁，但未独立重跑构建/测试；证据见 `build-codex-agent/agent-phase4-slice27-evidence/`。
- 能力矩阵已按 §5 重新生成（JSON 唯一数据源，69 插件 × 7 维、111 项操作，483/483 引用验证通过）。Slice27 是共享模块结构 Undo 机制，不将其外推成逐插件参数/配置持久化 PASS；`undo_persistence` 仍为 PASS 0、NOT_TESTED 58、NOT_APPLICABLE 11，未验证的逐插件保存重开留在阶段 4/8，并在单元格理由中链接本片证据。
- 阶段总审确认 G4 #1–#6 均达到 §9 标准；本次独立总审核验累积切片 PASS 与阶段 4 收尾证据。该结论只放行阶段 5，不覆盖阶段 5–8 的插件、设备、真实硬件或最终发布验收。
- 多流程执行并未实现；Slice24 仅确保显式 flowId 选择得到 unavailable 且现有全工程行为兼容。本次 G4 PASS 不扩张对多流程执行或真实设备的支持声明。

## 4. 第 1 片独立审核结论

2026-10-15，独立 Agent `/root/phase4_slice1_independent_audit` 只读复核判定**第 1 片 PASS**：核对最终 JUnit 78/78、定向 4/4 与 3/3、ToolSchema 9/9、17/17 源码/证据摘要和 75/75 二进制摘要；确认已有控制边 `next→control` 的回读与保存重开保真，以及 `executionScope=project_modules` 与引擎实际范围一致。审核者未修改文件，未独立重跑全量构建/CTest。**该结论仅允许继续阶段 4；不等于 G4 PASS，不放行阶段 5。**Agent 创建控制边、旧边类型推断、编辑/调试/撤销完整性和真实设备仍未验收。

## 5. 第 2 片：显式控制端口的连线保真

本片沿用 `connect_modules` 既有按插件端口声明判别 data/control 的逻辑，补足对真实 `If` 与 `Parallel` 插件的回归测试，并使连接、重复连接和断开结果显式返回 `edgeType`。`get_flow_state` 与参数 schema 共用的端口序列化补上 `joinPolicy`，使 `Parallel.control` 的 `all` 汇合策略可查询。没有开放隐式 `next/control` 端口的 Agent 创建入口，也没有改变 GUI 的连线规则。

`testG4ControlAndDataEdgesStayDistinct` 验证 `If.true → Parallel.control` 为控制边、`If.image → Parallel.image` 为数据边、两边可同时存在、跨类型连接拒绝且零改动、重复连接幂等（返回 `already_connected` 且 Undo 栈 count/index 不变）、精确断开与 Undo/Redo 保留类型，以及保存重开后按端口四元组核对类型和 `joinPolicy=all`。审核整改后 `test_agentactor` 通过 31/31，退出码 0；全量 CTest 最终轮 78/78、0 失败、0 跳过，退出码 0。全量构建与最终受测目标增量构建均退出码 0，Qt 5.15.3，所有验证均使用本工作区 `build-codex-agent/bin/`。`clang-format --dry-run --Werror` 与 `git diff --check` 通过；矩阵重生成并完成 483/483 引用校验。

原始证据在 `build-codex-agent/agent-phase4-evidence/`：全量构建 `build-slice2-final.log`、审核整改目标构建 `build-slice2-reviewfix.log`、最终 `actor-slice2-reviewfix.log`、`ctest-slice2-reviewfix.log` 与同名 JUnit XML。首轮受限沙箱导致 5 个本地 socket 监听套件失败，`ctest-slice2.log` 原样保留；获本地监听权限后 78/78 通过。中间一次新增测试把 Undo 后的连接列表顺序误作持久化要求，`actor-slice2-final.log` 保留失败原貌；改为按端口四元组断言后通过。独立审核还要求补强重复连接零副作用断言，整改前 `actor-slice2-verified.log` 与 `ctest-slice2-verified.log` 均保留。第 2 片摘要由 `sha256sums-slice2-verified.txt` 和 `binary-manifest-slice2-verified.txt` 单独锁定，不改写第 1 片快照。

第 2 片已获独立 PASS；G4 的节点编辑、单步/重置、运行前校验、旧边类型推断、两入口交替操作和撤销边界仍未完成，不能进入阶段 5。

## 6. 第 2 片独立审核结论

2026-10-15，独立 Agent `/root/phase4_slice2_independent_audit` 只读复核判定**第 2 片 PASS，阻断发现 0**。审核中先发现记录首页仍称“仅第 1 片”及重复连接测试未锁定 Undo 栈不变；两项均整改后再次核对。最终 `test_agentactor` 31/31、CTest JUnit 78/78 且无失败/跳过；源码/证据 35/35、二进制 75/75 摘要校验退出码均为 0。审核者将显式控制边类型、`joinPolicy` 和端口兼容性与 GUI/RunEngine 现有规则逐项对照；未修改文件，也未独立重跑构建/CTest。

**放行范围仅是继续阶段 4，不是 G4 PASS，不放行阶段 5。**尚未用真实 If/Parallel 执行序列验证控制图，隐式 `next/control`、旧边类型推断、节点编辑与调试、整体撤销边界及真实设备亦未验收。本段归档后的第 2 片 35 项源码/证据摘要已重新生成。

## 7. 第 3 片：删除节点 Undo 的顺序与 GUI 一致性

根因链：`remove_module` → `AgentRemoveModuleCmd::redo` 删除关联连接与节点 → `undo` 用 `Project::addModule/addConnection` 追加备份 → `moduleAdded` 使 `ProcessTreeController` 追加树项。删除末尾节点时偶然保序；删除中间节点时，Project 节点/连接顺序和 GUI 树顺序改变。本片为 `Project::addModule/addConnection` 增加默认末尾追加、指定索引插入的兼容入口；删除命令记录节点及每条关联连接的原索引，在 Undo 时按索引恢复。`ProcessTreeController::onModuleAddedFromProject` 仅对信号新建的树项按 Project 当前列表定位。独立审核发现工具箱拖放会预插入临时树项，若也重排它会让 GUI 指定插入行丢失；整改后保留预插入树项位置，由 MainWindow 既有 `moveModule` 路径把 Project 调整至同一行。未变更流程执行调度或新增 Agent 工具。

`testG4RemoveUndoRestoresExactModuleAndConnectionOrder` 对中间节点的删除→Undo→Redo→Undo 比较节点和连接的完整序列化数组，覆盖名称、备注、参数、数据边与控制边端口/类型，并保存重开核对；`testG4AgentRemoveUndoKeepsProcessTreeOrder` 覆盖同一操作下 GUI 流程树与画布节点同步；整改新增 `testG4PreInsertedToolboxDropKeepsRequestedRow` 锁定工具箱拖放的预插入分支及首/中/末顺序。最终 `test_agentactor` 32/32、UI 定向 4/4（含 init/cleanup）、全量 CTest 78/78 且 0 失败/跳过，退出码均为 0；完整构建退出码 0，`clang-format --dry-run --Werror` 和 `git diff --check` 通过。构建缓存仍指向本工作区；系统 PATH 的同名安装版未用于验证。

原始证据位于 `build-codex-agent/agent-phase4-evidence/`：整改最终 `build-slice3-reviewfix-final.log`、`actor-slice3-verified.log`、`mainwindow-slice3-reviewfix-final.log`、`ctest-slice3-reviewfix-final.log` 与同名 JUnit XML；整改前的 `build-slice3-verified.log`/`ctest-slice3-verified.log` 及中间测试日志均保留。第 3 片摘要独立于第 1/2 片快照。**本片整改后独立复审 PASS；G4 仍 IN_PROGRESS，不放行阶段 5。**

## 8. 第 3 片独立审核结论与范围

2026-10-16，独立 Agent `/root/phase4_slice2_independent_audit` 首轮判定第 3 片 **FAIL**：新增树项按 Project 位置重排时误伤工具箱拖放预插入的树项。整改为仅重排由 `moduleAdded` 信号新建的树项，保留 GUI 预插入行；补充首/中/末位置回归。复审判定**第 3 片 PASS，阻断发现 0**。审核者复核最终定向 UI 4/4、Actor 32/32、CTest JUnit 78/78 且无失败/跳过，以及当时源码/证据 62/62、二进制 75/75 摘要；审核者未独立重跑构建或全量 CTest。归档本段与操作清单后刷新第 3 片源码/证据摘要，不改动历史清单。

预插入测试模拟的是与实际拖放相同的树项分支和后续 `moveModule`，尚非完整 `QDropEvent` 端到端测试。Agent/GUI 交替撤销、运行中编辑保护、更多流程编辑与调试入口、真实设备均待 G4 后续片验证。**本次只放行继续阶段 4，不构成 G4 PASS，不放行阶段 5。**

## 9. 第 4 片：Agent 节点排序与流程树同步（切片 PASS）

冻结契约：`move_module(instanceId, index)` 把现有节点移动到 0 基索引，索引必须是当前节点列表内的整数。成功返回 `status=moved`、`instanceId`、`oldIndex` 和 `index`，写入一条 Agent Undo 命令；位置不变返回 `status=no_op` 且不写 Undo；无工程、未知实例、无效索引及引擎忙态分别返回 `NO_PROJECT`、`INSTANCE_NOT_FOUND`、`INVALID_PARAM`、`ENGINE_BUSY`，均不改动工程。节点稳定 ID、参数和连接不变。`Project::moveModule` 在真实移动后发出 `moduleMoved`，`ProcessTreeController` 仅在树项尚未处于目标行时移动它；GUI 已预先移动树项的拖放路径不会被重复重排。模型排序仍是运行顺序的事实源。

测试：`testG4MoveModuleValidatesAndRestoresOrder` 验证非法输入零副作用、移动、幂等、Undo/Redo 和保存重开；既有 `testCloseProjectCheckedReflectsEngineBusy` 增加排序忙态拒绝；`testG4AgentMoveUndoKeepsProcessTreeOrder` 验证 Agent 移动及撤销/重做同步流程树；既有 `testG4PreInsertedToolboxDropKeepsRequestedRow` 回归保持。审核整改新增 `testMoveModuleIndexIsIntegerInPublishedSchema` 核对模型可见 Schema，`testG4MoveModuleControllerGateAndUndoAudit` 核对控制器三档权限与 moved/no_op/错误审计标记。F09 基线保留其他未支持工具断言，只把本次新工具从缺失清单移出。定向 Actor 4/4、UI 4/4、整改后控制器 3/3、Schema 3/3（均含 init/cleanup），最终全量 CTest 78/78、0 失败/跳过，退出码均为 0；矩阵 69 插件、109 操作、483/483 引用有效。

原始证据在 `build-codex-agent/agent-phase4-evidence/`：首次构建 `build-slice4.log` 因新增 UI 测试漏声明槽而失败，修正后 `build-slice4-fixed.log` 退出码 0；`build-slice4-reprofix.log` 为更新 F09 断言后的目标构建；格式调整后的最终全量构建为 `build-slice4-final.log`，退出码 0。首次 `ctest-slice4.log` 同时包含旧 F09 断言与受限沙箱本地 socket 监听失败，均保留；允许本地监听后的 `ctest-slice4-final.log` 与最终构建后的 `ctest-slice4-verified.log`（均含 JUnit XML）记录 78/78。定向日志为 `actor-slice4-targeted.log`、`mainwindow-slice4-targeted.log`。受测二进制均来自本工作区 `build-codex-agent/bin/`，未使用 PATH 中安装版。

本片只覆盖 Agent 排序与 GUI 树同步，不宣称 Agent/GUI 交替撤销、真实拖放事件、运行中全部编辑保护或 G4 的重命名、位置、断点、单步和校验已完成。未提交、未推送；切片审核仅放行继续阶段 4。

## 10. 第 4 片独立审核整改与结论

独立审核首轮指出：`move_module.index` 原声明为 `number`，而执行器仅接受整数；已改为导出 JSON Schema `integer` 并加测试。审核者还要求从 `AgentController::handleToolCall` 锁定权限和审计链路；新增测试覆盖 Observer 零执行拒绝、Advisor 确认前零执行并拒绝、Autopilot 实际移动，及成功可撤销、no_op/失败不可撤销。整改最终全量构建 `build-slice4-reviewfix.log`、控制器定向 `controller-slice4-reviewfix.log` 3/3、Schema 定向 `toolschema-slice4-reviewfix.log` 3/3、全量 `ctest-slice4-reviewfix.log` 与同名 JUnit XML 78/78，退出码均为 0。当前快照的 `sha256sums-slice4-reviewfix.txt` 锁定 35 项源码/证据，`binary-manifest-slice4-reviewfix.txt` 锁定 75 项二进制，校验退出码均为 0；此前第 4 片首轮 28/75 清单只作历史快照。

2026-10-19，独立 Agent `/root/phase4_slice2_independent_audit` 复审判定**第 4 片 PASS，阻断问题 0**：核对发布 Schema 的整数类型、控制器三档权限/确认/审计、Actor 校验和 Undo、Project 信号及 GUI 树同步链路；独立校验整改源码/证据 35/35、二进制 75/75，并检查最终 JUnit 78/78、0 失败/跳过和 `git diff --check`。审核者未独立重跑构建或全量 CTest；真实 `QDropEvent`、Agent/GUI 交替撤销、其余 G4 能力和真实设备均未验收。**只放行继续阶段 4，不是 G4 PASS，不放行阶段 5。**归档本段后刷新第 4 片摘要。

## 11. 第 5 片：节点重命名与可见名称保真（切片 PASS）

契约：`rename_module(instanceId, name)` 只修改实例显示名，输入必须为非空字符串，保存时去除首尾空白；实例 ID、插件类型、参数和连接不变。成功返回 `status=renamed`、`instanceId`、`oldName`、`name`，产生一条 Agent Undo；相同名称返回 `status=no_op` 且不写 Undo。无工程、未知实例、非法名称和运行中分别返回 `NO_PROJECT`、`INSTANCE_NOT_FOUND`、`INVALID_PARAM`（空白必填值可由统一入口返回 `MISSING_PARAM`）、`ENGINE_BUSY`。Schema 对模型发布必填字符串名称，控制器三档权限与现有工程写操作一致，审计仅在真实重命名成功时标记可撤销。

`Project::updateModule` 已提供稳定 ID 的更新信号；新增命令在 Undo/Redo 时仅替换当前实例的 `name`，保留其他字段。GUI 之前在树和画布建项时优先按插件名显示，可能遮蔽自定义实例名；本片在建项、收到更新信号和保存重开后都以非空实例名显示，旧项目空名仍回退插件显示名。检查器标题仍表示插件类型，不被冒充为实例别名。内置 Agent 知识库同步说明稳定 ID、名称可重复及保存不可由 Agent Undo 撤销。

`testG4RenameKeepsIdentityConnectionsAndUndo` 覆盖错误零副作用、稳定 ID/参数/连接、幂等、Undo/Redo 和保存重开；`testG4AgentRenameUpdatesTreeCanvasAndReopen` 覆盖树/画布即时更新、撤销/重做和 GUI 重开；控制器与 Schema 定向测试分别核对权限、确认前零执行、审计撤销标记和发布参数形状；既有忙态测试增加重命名拒绝，F09 基线仅移出已实现的 `rename_module`，其余未支持项保留。定向 Actor 4/4、UI 5/5、控制器 3/3、Schema 3/3、F09 3/3（均含 init/cleanup）；最终构建退出码 0，全量 CTest 78/78、0 失败/跳过，矩阵 483/483 引用有效，`clang-format --dry-run --Werror` 与 `git diff --check` 通过。

原始日志与 JUnit 位于 `build-codex-agent/agent-phase4-evidence/`：`build-slice5.log`、知识库资源更新后的 `build-slice5-final.log`、`actor-slice5-targeted.log`、`mainwindow-slice5-targeted.log`、`controller-slice5-targeted.log`、`toolschema-slice5-targeted.log`、`f09-slice5-targeted.log`、`ctest-slice5.log/.xml` 和最终 `ctest-slice5-final.log/.xml`；首轮 `sha256sums-slice5.txt` 锁定 28 项源码/证据，`binary-manifest-slice5.txt` 锁定 75 项二进制，仅作历史快照，整改版证据见 §12。均使用本工作区 `build-codex-agent/bin/`；PATH 中安装版未用于验证。真实 GUI 内没有原生重命名菜单，本片是 Agent 入口驱动 Project 与 GUI 显示同步；Agent/GUI 交替撤销、字段编辑与调试其余 G4 项均未验收。未提交、未推送；不放行阶段 5。

## 12. 第 5 片首轮审核整改与独立复审结论

独立审核首轮判定**第 5 片 FAIL**：旧工程实例 `name` 为空时，流程树有插件名回退，但 `FlowCanvas::loadFromProject` 以空名称建画布节点；重命名后 Undo 再保存重开会重现树/画布分歧。修复沿用 MainWindow 的 `toolDisplayName` 作为同一回退来源：工程载入画布后仅对空名实例补上画布标题，非空实例名仍原样显示。新增 `testG4LegacyEmptyNameStaysVisibleAfterRenameUndoAndReopen` 覆盖旧空名打开→rename→Undo→保存重开，比较树与画布可见名。整改构建 `build-slice5-reviewfix.log` 退出码 0、定向 `mainwindow-slice5-reviewfix.log` 4/4、全量 `ctest-slice5-reviewfix.log/.xml` 78/78 且 0 失败/跳过，退出码均为 0。首轮 28/75 清单仅作历史快照；整改版 `sha256sums-slice5-reviewfix.txt` 锁定 32 项源码/证据，`binary-manifest-slice5-reviewfix.txt` 锁定 77 项二进制，额外覆盖本片定向运行的 MainWindow 与 Controller 测试程序。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 整改复审判定**第 5 片 PASS，阻断问题 0**：复核 Schema、Actor、Controller、Undo/Redo、GUI 名称回退及稳定 ID/连接；独立运行新增旧空名用例 3/3，独立校验整改版源码/证据 32/32、二进制 77/77、最终 JUnit 78/78 且无失败/跳过、`git diff --check` 均通过。审核者未独立重跑全量构建或 CTest；Agent/GUI 交替撤销与其余 G4 条目仍未验收。**本片只放行继续阶段 4，G4 仍 IN_PROGRESS，不放行阶段 5。**归档后刷新整改版源码/证据摘要。

## 13. 第 6 片：节点启用/禁用（切片 PASS）

冻结契约：`set_module_enabled(instanceId, enabled)` 的 `enabled` 是必填 JSON 布尔值，不接受字符串或数字。无工程、未知实例、非法值、运行中分别返回 `NO_PROJECT`、`INSTANCE_NOT_FOUND`、`INVALID_PARAM`（缺参 `MISSING_PARAM`）、`ENGINE_BUSY`，均零修改；同值返回 `status=no_op`，不入 Agent Undo 栈。真实变更返回 `status=updated`、`instanceId`、`oldEnabled`、`enabled`，仅修改实例启用字段。Agent Undo/Redo 同步 `Project` 与现有 `RunEngine::setModuleDisabled`；不另造禁用执行语义。运行中禁止修改，本片不开放暂停时修改例外。控制器沿用 Observer 拒绝、Advisor 确认、Autopilot 执行及真实成功才标可撤销的现有入口策略。

MainWindow 的 `moduleUpdated` 信号路径在参数刷新之外，仅同步当前检查器的启用控件，并按检查器实际 `currentInstanceId` 限定更新；`ModuleInspectorPanel::setEnabledState` 抑制回写信号，不覆盖用户尚未提交的备注草稿。断点/备注控件的 Agent 编辑与同步由后续片处理。保存重开恢复 `enabled` 字段；引擎下次载入工程仍使用既有禁用模块逻辑。内置知识库补充此工具。阶段 0 的 109 项 GUI 操作清单漏列检查器的启用和备注控件，本片补稳定 ID `INSP_ENABLED`（直接工具）与 `INSP_NOTE`（仍未支持），不把历史阶段 0 数字改称当前数字；重生成矩阵为 69 插件、111 操作，483/483 引用校验通过。

首轮定向验证：Actor `testG4SetModuleEnabledSyncsEngineAndUndo` + 忙态路径 4/4，Controller `testG4SetModuleEnabledControllerGateAndAudit` 3/3，Schema `testSetModuleEnabledPublishedSchemaRequiresBoolean` 3/3，MainWindow 3/3，F09 更新后 3/3，均 0 失败/跳过且退出码 0。首轮构建 `build-slice6-final.log` 退出码 0，全量 `ctest-slice6-final.log/.xml` 为 78/78、0 失败/跳过、退出码 0；此为检查器草稿整改前快照，不能替代整改后的全量结果。首轮 `sha256sums-slice6.txt` 26 项、`binary-manifest-slice6.txt` 77 项亦仅是历史快照。日志均在 `build-codex-agent/agent-phase4-evidence/`。

边界：本片未证明 Agent/GUI 交替撤销、暂停时断点编辑、真实流程中的禁用旁路、位置/断点/备注工具，也不等于 G4 PASS。未提交、未推送；独立审核与补证过程见 §14，不放行阶段 5。

## 14. 第 6 片独立审核整改与复测结论

独立审核指出首轮 `moduleUpdated` 使用 `setRuntimeState` 会把尚未失焦写回的备注草稿覆盖为 Project 旧值。整改为 `setEnabledState` 只更新本片所需的复选框，不触碰备注/断点；`testG4AgentEnabledUpdatesInspectorAndEngine` 加入未提交草稿保留断言，`testG4AgentEnabledLeavesOtherInspectorStateAlone` 覆盖非目标节点更新不污染当前检查器。该第二用例不宣称验证固定（pin）检查器的特殊分支。整改构建 `build-slice6-reviewfix2.log` 退出码 0，UI 定向 `mainwindow-slice6-reviewfix2.log` 4/4、退出码 0。

整改后的本地受限环境全量 `ctest-slice6-restricted.log/.xml` 为 73/78 通过、5 个套件因 `server.listen()`/`QLocalServer::listen` 权限失败，退出码 8；失败套件是 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro`，后者仅 `testG1PlcFlowZeroSideEffectBeforeConfirmation` 的 loopback 监听失败，其本片 F09 定向通过。明确不把它写作 78/78，也不把环境故障归为产品回归。整改后二次申请本地监听权限均在自动审核阶段超时，测试未启动；受限环境排除上述 5 套件的 `ctest-slice6-nosocket.log/.xml` 为 73/73、退出码 0。首轮完整 78/78 是 UI 草稿修复前的同版本其他代码结果；整改后仍欠本地监听可用环境下的全量 78/78。整改版 `sha256sums-slice6-reviewfix2.txt` 锁定 36 项源码/证据，`binary-manifest-slice6-reviewfix2.txt` 锁定 77 项二进制。

独立 Agent `/root/phase4_slice2_independent_audit` 复核整改实现和两项 UI 用例（独立运行 4/4）、111 项操作 ID/映射、整改摘要 36/36、二进制 77/77、`git diff --check`，未见新的功能阻断；但因整改后缺完整 78/78，正式切片结论为 **FAIL / NOT_READY_FOR_PASS**（验收环境受限，不定性为产品回归）。剩余验收条件：在允许本地 socket/loopback 监听的环境，对同一锁定代码与二进制跑完整 CTest，记录退出码与 JUnit 78/78；刷新新增证据摘要并重新送独立复审。此门槛未满足前，不将第 6 片标 PASS，不以它作为后续 G4 切片的通过依赖，G4 仍 IN_PROGRESS，不放行阶段 5。

2026-10-20，用户授权在可监听本地 socket/loopback 的环境继续验收。运行前再次校验整改版 `sha256sums-slice6-reviewfix2.txt` 36/36、`binary-manifest-slice6-reviewfix2.txt` 77/77，退出码均为 0；随后在同一 `build-codex-agent` 构建目录运行完整 CTest，`ctest-slice6-authorized.log/.xml` 记录 **78/78、0 失败、0 跳过、退出码 0**。此前受限环境的 73/78 原始失败日志保留，不覆盖、不抹除；本次补的是整改后同一代码/二进制快照的完整回归。`sha256sums-slice6-authorized.txt` 已将新日志/JUnit 纳入，锁定 38 项源码/证据；二进制继续由整改版清单锁定 77 项，校验均退出码 0。重新送独立复审；在收到新的独立结论前，本片仍不标 PASS，G4 仍 IN_PROGRESS，不放行阶段 5。

独立 Agent `/root/phase4_slice2_independent_audit` 补证复审判定**第 6 片 PASS，阻断项 0**：独立校验授权版源码/证据 38/38、整改二进制 77/77、`git diff --check`；核对最终 JUnit 78/78、0 失败/跳过及此前五个监听依赖套件均在授权日志中通过，确认受测二进制摘要对应测试前的锁定快照。审核者未独立重跑授权环境全量 CTest；真实流程禁用旁路、Agent/GUI 交替撤销、断点/备注/位置仍不在本片范围。**仅放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。**归档后刷新授权版源码/证据摘要。

## 15. 第 7 片：节点断点设置（切片 PASS）

冻结契约：`set_breakpoint(instanceId, enabled)` 的 `enabled` 为必填 JSON 布尔值；无工程、未知实例、非法值、运行中分别返回 `NO_PROJECT`、`INSTANCE_NOT_FOUND`、`INVALID_PARAM`（缺参 `MISSING_PARAM`）、`ENGINE_BUSY`，均零修改。同值返回 `status=no_op` 且不产生 Agent Undo；真实变更返回 `status=updated`、`instanceId`、`oldBreakpoint`、`breakpoint`，仅修改实例 `breakpoint` 字段。Agent Undo/Redo 同步 Project 和既有 `RunEngine::setBreakpoint`；不另造断点调度器，运行中及暂停时均不开放 Agent 修改。Controller 复用 Observer 拒绝、Advisor 确认、Autopilot 执行及真实成功才标可撤销的既有策略。

MainWindow 的 `moduleUpdated` 路径对当前检查器调用专用 `setBreakpointState`，只更新断点按钮并抑制回写信号，不覆盖未提交的备注草稿；非当前节点更新不污染检查器。工程保存重开保留断点字段，运行时下次载入工程继续使用既有 Project→RunEngine 断点同步。知识库补充此工具；操作清单 `INSP_BREAKPOINT` 从未支持改为直接工具，111 项 ID 数量不变；矩阵由审计脚本重生成，69 插件、111 操作、483/483 引用有效。

定向测试：Actor `testG4SetBreakpointSyncsEngineAndUndo` 与既有忙态路径 4/4；Controller `testG4SetBreakpointControllerGateAndAudit` 3/3；Schema `testSetBreakpointPublishedSchemaRequiresBoolean` 3/3；MainWindow `testG4AgentBreakpointUpdatesInspectorWithoutLosingNoteDraft` 3/3；F09 移出已实现 `set_breakpoint` 后 3/3，均退出码 0。`cmake --build build-codex-agent -j2` 日志 `build-slice7.log` 退出码 0；允许本地监听的完整 `ctest-slice7.log/.xml` 为 78/78、0 失败/跳过、退出码 0；`clang-format --dry-run --Werror` 与 `git diff --check` 干净。证据均在 `build-codex-agent/agent-phase4-evidence/`，新增定向日志以 `*-slice7-targeted.log` 命名；`sha256sums-slice7.txt` 锁定 29 项源码/证据，`binary-manifest-slice7.txt` 锁定 77 项二进制，校验退出码均为 0。

边界：本片仅验证断点字段编辑和状态同步，不证明实际断点命中/单步推进、GUI 运行中断点操作与 Agent 策略一致、Agent/GUI 交替撤销或 G4 整体验收。待独立审核；未提交、未推送，不放行阶段 5。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 审核判定**第 7 片 PASS，阻断项 0**：核对 JSON 布尔契约、Actor 错误/no_op/Undo、RunEngine 同步、忙态门、Controller 权限审计、GUI 断点按钮同步与草稿保留、F09 及矩阵映射；独立运行 GUI 定向用例 3/3，并独立校验 JUnit 78/78、源码/证据 29/29、二进制 77/77 和 `git diff --check`。审核者未独立重跑全量 CTest。只放行继续阶段 4；实际断点命中、单步时序、GUI 运行中断点策略及 Agent/GUI 交替撤销仍待验证。**G4 仍 IN_PROGRESS，不放行阶段 5。**归档后刷新切片 7 源码/证据摘要。

## 16. 第 8 片：节点备注编辑（切片 PASS）

冻结契约：`set_module_note(instanceId, note)` 的 `note` 是必填字符串；非空值覆盖当前备注，空字符串明确清除备注。无工程、未知实例、非字符串和运行中分别返回 `NO_PROJECT`、`INSTANCE_NOT_FOUND`、`INVALID_PARAM`、`ENGINE_BUSY`，零修改；同值返回 `status=no_op`，不产生 Agent Undo。真实变更返回 `status=updated`、`instanceId`、`oldNote`、`note`，Undo/Redo 只修改实例备注字段，不影响其他字段。为支持空字符串清除，统一参数校验仅对该工具的 `note` 字段豁免“必填字符串不可空”检查；执行器仍严格拒绝非字符串。Observer 拒绝、Advisor 确认、Autopilot 执行及可撤销审计沿用现有控制器策略。

MainWindow 收到当前实例 `moduleUpdated` 时通过 `ModuleInspectorPanel::setNoteState` 同步检查器备注；如果输入框已有未提交的用户草稿（`QLineEdit::isModified()`），保留草稿，Agent Undo/Redo 也不覆盖。用户提交备注后清除 modified 标记，后续 Agent 改动可正常刷新；断点和启用更新仍使用各自专用控件同步。保存重开保留备注字段。内置知识库和操作清单 `INSP_NOTE` 已映射到直接工具；操作数仍为 111、矩阵 483/483 引用有效。

定向测试：Actor `testG4SetModuleNoteSupportsClearAndUndo` + 运行中拒绝路径 4/4；Controller `testG4SetModuleNoteControllerGateAndAudit` 3/3；Schema `testSetModuleNotePublishedSchemaRequiresString` 3/3；MainWindow `testG4AgentBreakpointUpdatesInspectorWithoutLosingNoteDraft` 同时覆盖未提交草稿保留、设置、清除及重开 3/3；F09 工具清单更新 3/3，均退出码 0。构建日志 `build-slice8.log` 退出码 0；授权本地监听完整 CTest `ctest-slice8.log/.xml` 78/78、0 失败/跳过、退出码 0；矩阵、格式和 `git diff --check` 均通过。`sha256sums-slice8.txt` 锁定 29 项源码/证据，`binary-manifest-slice8.txt` 锁定 77 项二进制，均校验退出码 0。

边界：本片验证备注字段编辑/清空、撤销和检查器草稿保护，不覆盖 Agent/GUI 交替撤销整体范围、模块位置、实际断点命中、运行前流程校验或 G4 整体验收。未提交、未推送，不放行阶段 5。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 判定**第 8 片 PASS，阻断项 0**：复核空字符串例外仅限本工具备注字段、执行器非字符串拒绝、no_op/Undo、忙态门、权限审计、GUI 草稿保留及序列化；独立校验源码/证据 29/29、二进制 77/77、全量 CTest 78/78、矩阵 483/483 和 `git diff --check`。审核者未独立重跑全量 CTest。边界：GUI 保存重开用例未直接断言备注文本，但 Actor 保存重开 JSON 测试覆盖备注序列化，GUI 选择路径由实现可见。只放行继续阶段 4；**G4 仍 IN_PROGRESS，不放行阶段 5。**归档后刷新切片 8 源码/证据摘要。

## 17. 第 9 片：单步执行与重置（切片 PASS）

新增 `step_once` 与 `reset_step`。`step_once` 复用 `RunEngine::stepOnce()`，每次同步执行一个引擎节点；执行前请求 MainWindow 使用既有同步路径，只有 GUI 确认同步成功后才更新共享 Actor 的工程快照。无 MainWindow 的调用方在工程变化或运行时模块与 Project 不匹配时回退到 `RunEngine::loadProject()`。这避免重复载入丢失连续单步位置，也避免 GUI 首次同步因 `m_modulesNeedSync` 清空引擎状态。成功返回 `status=stepped`、本次 `runId`、引擎给出的 `nextModule` 和 `async=false`；工程不存在、流程为空、校验失败、引擎忙和模块执行失败分别失败关闭。`reset_step` 复用 `resetStepState()`，下一次单步从流程入口开始并保留当前工程快照。两工具都不修改 Project，不入 Agent Undo；忙态拒绝。`step_once` 因可执行文件/设备/网络副作用按执行入口要求 Observer 拒绝、Advisor/Autopilot 确认；`reset_step` 是普通写操作，沿用权限门。没有另造调度器，分支/循环/断点语义仍来自 RunEngine。

`testG4StepOnceAdvancesOneModuleAndResetReturnsToEntry` 使用两个 Demo 图像源，核对连续两次分别仅执行 `step_a`、`step_b`，重置后回到 `step_a`；`testG4AgentAndGuiSingleStepSharePosition` 核对 Agent→GUI 执行 B、重置后 GUI→Agent 执行 B，防止重复执行入口节点；`testG4StepOnceRequiresExecutionConfirmation` 覆盖 Observer 拒绝、Advisor 和 Autopilot 确认前不执行。Schema/F09 测试将两个工具列为已支持，Agent 内置知识库和 `TB_STEP` 操作映射同步更新。矩阵重生成后为 69 插件、111 操作，483/483 引用校验通过。

验证：最终格式化版本全量构建退出码 0，日志 `build-slice9.log`；完整 CTest `ctest-slice9-final.log/.xml` 在获准 loopback 环境通过 78/78、0 失败、0 跳过，退出码 0。首次受限运行日志 `ctest-slice9.log` 保留，其中 `test_agentphase0_repro` 因本地 PLC listener 权限失败；允许 loopback 后该套件定向复跑通过，之后完整套件也全部通过。矩阵审计 `matrix-slice9.log` 报告 69 插件、111 操作及 483/483 引用有效；`clang-format --dry-run --Werror`、`git diff --check` 和两份 JSON 解析均通过。源码/证据清单 `sha256sums-slice9.txt` 为 21/21，二进制清单 `binary-manifest-slice9.txt` 为 77/77，校验退出码均为 0。测试只用 Demo 图像源，不覆盖真实相机/PLC/外部副作用。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 整改复审判定**第 9 片 PASS，阻断项 0**：确认同步信号只在 MainWindow 存在时走 GUI 既有流程同步；工程身份/内容和运行时实例顺序检查避免不必要重载；独立运行新增 GUI/Agent 交替用例 3/3，独立校验源码/证据 21/21、二进制 77/77、JUnit 78/78（0 失败/跳过）、矩阵 483/483 与 `git diff --check`。边界：验证只覆盖 Demo 两节点和共享 MainWindow/Controller Actor 路径，未覆盖真实设备/外部副作用及其他直接调用 RunEngine 的入口。**仅放行继续阶段 4；G4 整体仍 IN_PROGRESS，不放行阶段 5。**

## 18. 第 10 片：运行前流程校验（切片 PASS）

新增只读白名单工具 `validate_flow`，Observer 可执行。它不请求 GUI 同步、不清输出、不重置单步位置；`RunEngine::validateProjectFlow` 获取维护租约，确认当前运行时模块与工程的实例 ID、插件类型及顺序一致，并在临时 `RunEngine` 上复用 `buildExecutionOrder` 与 `validateFlow`。不在共享引擎上重建执行顺序/控制图，不初始化或执行模块；无工程、忙态、运行时不同步分别明确失败关闭。空工程返回 `valid=false`。报告所有缺失的必需非 image 输入，以及至多第一个结构图/端口构建错误（例如端口/边类型错误、重复边、数据环、未结构化控制环）；每项包含原始消息和 project 作用域，端口类错误尽量提供模块/端口 ID，类型不兼容同时提供源/目标端点。该工具不修改 Project、执行结果或 Agent Undo 栈。F09 白名单复现从缺失工具翻转为支持；内置知识库补充使用说明。阶段 0 GUI 操作清单没有独立校验按钮，本片不虚构新的 GUI 操作映射；操作数仍为 111。

新增 `testG4ValidateFlowReportsLocationsAndPreservesRuntimeModules` 覆盖无工程、相同实例 ID 但插件类型不同的陈旧运行时、模块集合不同步、必需 `fit_points` 缺失、合法连接、数据边类型不兼容、未结构化控制环，并断言校验前后共享运行模块地址不变且 idle 校验不发 GUI 同步信号；单步用例在运行 A 后调用校验，断言已有输出保留且后续仍执行 B。`testG4ValidateFlowDoesNotRequestGuiSyncWhileBusy` 覆盖忙态拒绝、零同步信号、零模块替换。插件身份不再错误依赖可能为空/表示内部模块名的 `ModuleBase::moduleId()`：PluginManager 创建和 `RunEngine::loadProject` 通过 `deeplux.pluginId` 动态属性记录工程声明类型，手工注册入口补齐；未知类型仍 fail closed。GUI→RunEngine 同步改为一次维护租约内校验/替换/构图，忙态或候选列表不匹配时不碰共享运行图，避免逐项 add 被拒后仍重建图。Observer 对话用例覆盖该只读工具不被权限门拒绝。Schema、F09 复现同时更新。

复审发现整改后的构建日志 `build-slice10-reviewfix.log` 退出码 0；Actor 定向 `actor-slice10-reviewfix-targeted-offscreen.log` 5/5 通过。一次不带 `QT_QPA_PLATFORM=offscreen` 的直接定向启动在 Qt 初始化阶段退出码 134、未执行用例，原始日志保留于 `actor-slice10-reviewfix-targeted.log`。首轮受限环境全量日志 `ctest-slice10-reviewfix.log` 有 5 个仅依赖本地监听的套件因 bind 权限失败；授权 loopback 环境下的最终串行快照 `ctest-slice10-reviewfix-locked.log` 与 JUnit `ctest-slice10-reviewfix-permitted.xml` 均为 78/78、0 失败、0 跳过、退出码 0，MainWindow 全套也通过。此前首轮 `-j2` 的 `testStopStability50Repeats` 时序失败及受限监听失败日志继续保留，不以重试覆盖。矩阵审计仍为 69 插件、111 操作、483/483 引用有效；格式门禁和 `git diff --check` 通过。源码/证据最终摘要 37/37、二进制摘要 77/77 校验通过，见 `sha256sums-slice10-reviewfix.txt` 与 `binary-manifest-slice10-reviewfix.txt`。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 对整改后快照判定**第 10 片 PASS，阻断项 0**：独立检查 busy 前置拒绝、只读校验不清输出/单步位置、稳定插件身份和原子 GUI 同步；独立运行 Actor 关键用例 4/4、GUI 单步交替 3/3，并校验源码/证据 37/37、二进制 77/77、JUnit/CTest 78/78、矩阵 483/483 与 `git diff --check`。审核结论只放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。边界：未覆盖真实设备与外部副作用。

## 19. 第 11 片：旧工程连线类型解析（切片 PASS）

冻结契约：`get_flow_state.connections[].edgeType` 保留 Project 中持久化的原始值；新增 `effectiveEdgeType` 与 `edgeTypeResolution`。显式类型原样标为 `explicit`；旧项目的空类型仅在两端工程节点及插件端口元数据可查时，按 RunEngine 共用的纯推断规则报告 `data` 或 `control` 并标为 `inferred`；缺失端点或端口元数据时标为 `unavailable`，不猜测，也不写回 Project。`PluginInfo::portsDeclared` 表示两个数组存在且解析成功；合法的两个空数组也算已声明。相机 metadata 当前不经过端口解析，因此不满足推断条件。缺失端口 ID 与引擎一致按 `image` 解析。该改动只补足状态回读，不新增 GUI 操作或连接能力，操作数仍为 111。

`testG4FlowStateResolvesLegacyEdgeTypesWithoutMutatingProject` 覆盖旧数据边、隐式 `next→control` 边、默认 `image` 端口、与端口推断相反的显式边类型、缺少端口声明的相机端点及无法解析端点；验证原始连接不变且查询无 Project 副作用。`test_pluginmanager` 断言合法空端口数组仍设置 `portsDeclared`。首轮整改定向测试因显式边与既有边复用了相同端点/端口四元组，被 Project 按现有去重规则拒绝，计数断言失败；原始日志 `ctest-slice11-reviewfix-targeted.log` 保留。将显式边改为不同端口组合后定向测试通过，未改 Project 的去重语义。

`ToolSchema` 的 `get_flow_state` 描述与内置知识库同步明确有效类型/推断状态。初次送审状态为 `READY_FOR_REVIEW`；首轮审核发现端口声明判定问题并已整改，整改版复审结论见下方。本片切片 PASS 不外推为 G4 PASS。

验证：整改后 `cmake --build build-codex-agent -j2` 退出码 0（`build-slice11-reviewfix2.log`）；MainWindow/PluginManager/RunEngine/ToolSchema/Actor 定向 CTest 5/5，完整串行 CTest 78/78、0 失败/跳过，分别见 `ctest-slice11-reviewfix2-targeted.log` 与 `ctest-slice11-reviewfix2.log/.xml`。另保留原 READY 快照全量 78/78 (`ctest-slice11-final.log/.xml`) 与首轮缺陷复现日志。`clang-format --dry-run --Werror`、XML 解析与 `git diff --check` 均通过。`sha256sums-slice11-reviewfix2.txt` 源码/证据 49/49、`binary-manifest-slice11-reviewfix2.txt` 二进制 77/77 校验通过。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 对整改版判定**第 11 片 PASS，阻断项 0**：确认缺失端口声明的相机边返回 `unavailable`，合法空数组仍可区分为已声明；显式类型优先、空端口按 `image`、推断共用 RunEngine 规则且不写 Project。独立运行 Actor 用例 3/3、PluginManager 3/3、RunEngine 3/3、Schema 14/14；核验摘要 49/49、二进制 77/77、完整 JUnit/CTest 78/78、格式门禁与 `git diff --check`。非阻断边界：未端到端构造合法零端口模块之间的旧连接场景，只单测其 `portsDeclared=true`；本片不验证此类边的运行有效性。**该结论仅放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。**

## 20. 第 12 片：Agent 暂停、继续与停止的真实状态（READY_FOR_REVIEW）

按编码前冻结契约 `本地逐切片契约（未纳入提交）` 实施。`pause_flow` 调用后复核实际 Paused 状态，若生命周期竞争使暂停未生效则 no-op；`resume_flow` 不再把 `isRunning()==false` 错当作“不可继续”（Paused 本来就不是 Running），仅在实际离开 Paused 后报告 resumed；空闲、已运行或已停止为 no-op。`stop_flow` 发出协作取消后，仍有实际执行权返回 `stopping`，执行权释放后返回 `stopped`；纯工程维护租约不构成流程执行权，停止请求为 no-op。没有修改 RunEngine 调度、权限门、Project 或 Undo。

`testG4AgentRunControlsReflectRunEngineLifecycle` 覆盖 Idle/Stopped no-op、cycle 的 Running→Paused→Running→Stopped、重复和不适用动作；`testG4AgentStopReportsStoppingUntilNonCooperativeModuleSettles` 用不轮询取消令牌的模块验证停止期间 `stopping`、异步收尾后引擎不忙且 `get_run_results` 报告 cancelled。`testG4AgentStopIsNoOpDuringMaintenanceWithoutExecution` 并发持有 `loadProject` 的纯维护租约，确认引擎虽 `isBusy()` 但无执行权时 `stop_flow` 返回 no-op，维护操作继续成功且状态未变。Schema 对三项工具的生命周期行为作了说明。首轮定向测试暴露测试夹具的生命周期基线是 Stopped（CTest `init()` 会调用 stop），而非 Idle；原失败日志 `ctest-slice12-targeted.log` 保留，将契约和断言修正为 Idle/Stopped 均须 no-op 且状态不变后通过。

首轮独立审核判定 FAIL：指出 `isBusy()` 包含工程维护租约，导致仅加载工程也会被误报为 `stopping`。已将停止分类改为检查 Running/Paused/实际执行权，并新增上述并发维护租约回归；维护占用但无执行权现在 no-op。原审核失败及复现信息保留在交付上下文，不将该轮作为 PASS。

整改验证：格式化后构建 `build-slice12-reviewfix2.log` 退出码 0；Actor/ToolSchema/RunEngine/Controller 定向 CTest `ctest-slice12-reviewfix2-targeted.log` 4/4；在获准的本地 loopback 环境下，串行全量 CTest `ctest-slice12-reviewfix2.log/.xml` 为 78/78、0 失败、0 跳过、退出码 0。`clang-format --dry-run --Werror`、XML 解析与 `git diff --check` 通过；`sha256sums-slice12-reviewfix2.txt` 源码/证据 61/61、`binary-manifest-slice12-reviewfix2.txt` 二进制 77/77 校验通过。

2026-10-20，独立 Agent `/root/phase4_slice2_independent_audit` 对整改快照判定**第 12 片 PASS，阻断项 0**：核对 `stop_flow` 只以 Running/Paused/实际执行权作为停止依据，纯维护租约为 no-op；新增并发 `loadProject` 用例独立重复 10 次通过。Actor lifecycle 5/5、RunEngine 6/6、Controller 39/39、Schema 14/14；独立核验全量 JUnit/CTest 78/78、源码/证据 61/61、二进制 77/77、格式门禁与 `git diff --check`。边界：未在真实设备验证，亦未逐一覆盖所有维护 API。**该结论仅放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。**

## 21. 第 13 片：Agent run_flow 控制流一致性（切片 PASS）

编码前契约 `本地逐切片契约（未纳入提交）` 经独立审核 APPROVE 后实施。本片仅扩展 `test_acceptance_flows` 黑盒覆盖，未改产品调度或 Schema。所有新增场景均把流程装入 `ProjectManager` 当前工程，不预载/直启 RunEngine；统一通过 Agent `run_flow` 获取 runId，以同一 runId 等待终态并读取 Agent 结果。

新增覆盖：`testAgentRunFlowPreservesIfBranchSemantics` 用两个 Math 分支区分 true/false，断言选中分支的同 runId 标量输出可用且值正确、未选分支 skipped 且输出不可用；`testAgentRunFlowPreservesDisabledImageBypass` 用 GrabImage→禁用 CropImage→Blob 验证 Agent 禁用操作、节点跳过、Crop 未启动/未完成、下游只运行一次，并以固定圆心容差证明原图绕过；`testAgentRunFlowPreservesLoopCount` 验证 Loop(3) body 恰三次、done 后继一次，并锁定结束顺序 `loop,body,loop,body,loop,body,loop,after`；`testAgentRunFlowPreservesParallelAllJoin` 验证两分支真实并发、两者完成后才汇合且下游一次；`testAgentRunFlowPreservesParallelFailurePropagation` 断言结果 failed、慢 Delay 已派发且 `moduleFinished(false)`、运行耗时短于其 500ms 全时长、汇合与下游均未启动/未完成/成功执行。失败 run 的统一 unavailable 不被用来冒充取消证据。并行失败用例独立进程、offscreen 重复 10/10；Qt 无 display 的首次直接启动、当前 QtTest 不支持 `-repeat` 的尝试均保留原始日志，未计为产品失败。

一次测试断言曾要求 Blob 质心精确等于 320，而确定性图像实际值为 320.024405468；已按同数据集既有验收口径改为 3 px 容差，初次失败日志保留。另一次独立测试启动发现用例隐式依赖前一测试注册 ToolSchema；五个新增用例现在各自注册默认工具，单独启动可运行。审核首轮指出 loop 次序和失败取消/禁用绕过断言不够强，已补齐完整结束顺序与未启动、未完成断言。最终目标构建 `build-slice13-reviewfix3.log` 退出码 0，定向验收 `ctest-slice13-reviewfix3-targeted.log` 通过；受限环境首轮完整 CTest 中 5 个本地监听依赖用例因 socket bind 限制失败，原始日志保留；已获 loopback 授权在同一构建快照串行复跑，`ctest-slice13-reviewfix3-authorized-junit.log/.xml` 为 **78/78、0 失败/跳过、退出码 0**。加入最终断言后的并行失败用例独立进程 offscreen 重复 10/10，记录于 `agent-parallel-failure-slice13-repeat10-reviewfix3.log`。独立 Agent `/root/phase4_slice2_independent_audit` 对最终快照判定**第 13 片 PASS，阻断项 0**：独立运行五个新增用例 7/7、整套 `test_acceptance_flows` 24/24；核对 Agent `run_flow` 当前工程入口、同一 runId 结果、If 分支、禁用旁路、Loop 顺序、Parallel 并发/汇合/失败传播；独立核验授权 JUnit 78/78、源码/证据 85/85、二进制 77/77、格式与 `git diff --check`。非阻断边界：失败分支证明慢模块已派发且未成功完成、运行早于 500ms 全时长，不声称其 `process()` 必然已进入；未覆盖真实设备。本片仅放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。归档结论后将刷新源码/证据摘要。

## 22. 第 14 片：Agent 设置节点画布位置（切片 PASS）

按编码前冻结且经独立审核批准的 `本地逐切片契约（未纳入提交）` 实施。新增 `set_module_position(instanceId, x, y)`，坐标严格为 int32 整数；同坐标请求是 no-op，并允许 MainWindow 仅同步默认 `(0,0)` 的画布视觉位置而不污染 Project 修改态或 Undo。真实变化经 `Project::updateModule` 发出一次模型更新和专用位置事件，使主窗口节点及其相邻连线端点同步；Agent Undo/Redo 仅改目标节点坐标。忙态拒绝、控制器权限/确认/审计与保存重开均沿用并覆盖既有路径。没有改变鼠标拖动行为或实现跨入口撤销。

测试覆盖 ToolSchema 必填整数参数，Actor 的无工程/未知实例/缺参/非法类型与范围/忙态/no-op/更新/信号/Undo/Redo/保存重开；新增严格断言确保 `instanceId` 的数字、布尔、空白值及坐标的空串、布尔值均返回 `INVALID_PARAM`（真正缺字段仍为 `MISSING_PARAM`）。Controller 覆盖 Observer 零副作用拒绝、Advisor 拒绝与批准后恰执行一次（重复确认拒绝），以及 Autopilot 权限与撤销审计；MainWindow 通过共享 `AgentController::instance().actor()` 验证原点视觉对齐、连线端点、撤销重做和重开。首次全量 CTest 命中 F09 固定工具数断言仍为 31；更新为包含新工具的 32 后，复现套件在授权全量复跑通过。独立审核又发现上述参数类型边界及 Advisor 批准路径覆盖不足，均已整改并在定向 4/4 中复验。一次直接受限环境启动复现套件仍因其 loopback PLC fixture 无法监听而失败；该环境日志保留，不作为产品回归。最终完整构建退出码 0，授权串行全量 CTest **78/78、0 失败、0 跳过**，JUnit XML 可解析；`clang-format --dry-run --Werror` 与 `git diff --check` 均通过。构建有一条未修改文件 `tests/test_agent_sam_tools.cpp` 的 range-loop copy warning，不影响构建或测试。本片最终日志/XML和源码/证据、二进制摘要在 `build-codex-agent/agent-phase4-evidence/` 的 `*-slice14-*` 文件中。

独立 Agent `/root/phase4_slice2_independent_audit` 对整改后最终快照判定**第 14 片 PASS，阻断项 0**。审核复核先前发现的类型错误及 Advisor 确认路径：缺字段为 `MISSING_PARAM`，已提供但类型错误/空白 ID、非 int32 坐标（含空串/布尔）为 `INVALID_PARAM`；Advisor 拒绝后再次请求批准仅执行一次，重复批准无效并可撤销恢复。审核者独立运行 Actor、Controller、MainWindow、Schema 定向套件均通过，独立核验源码/证据 28/28、二进制 77/77、最终 JUnit 78/78 且 0 失败/跳过、格式与 `git diff --check`。非阻断边界：未覆盖真实 GUI 鼠标拖动、跨入口撤销及设备行为；UI 连线验证是 Canvas 可视连线，模型连接零变更另由 Actor 用例核对。**本片仅放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。**未提交、未推送。归档后刷新 Slice14 摘要。

## 23. 第 15 片：GUI 画布拖动持久化与撤销（切片 PASS）

编码前冻结契约 `本地逐切片契约（未纳入提交）` 原契约与混合批次 Undo 隔离增补均获独立 Agent `/root/phase4_slice2_independent_audit` 批准；增补复核阻断项 0。批准仅覆盖契约，不代表实现通过。

实现以 Project 运行时双 generation 区分 GUI/Agent/未标记的位置更新；GUI 拖动在提交时验证 generation、忙态与坐标范围后持久化为单条 GUI Undo 命令，Agent 位置 Undo 绑定工程对象、实例、坐标及 Agent generation，跨来源修改后的 stale 命令永久 no-op 并可见反馈。无实际拖动的点击/轻微移动不写 Project；`(0,0)` 保存重开、相邻可视连线同步、运行忙态取消、GUI/Agent 同源 LIFO 均有状态/序列化断言。真实 viewport 拖动在 4x 变换下释放到 +10.25/-10.25 scene units 并分别量化为 +10/-10；量化 helper 覆盖正负半整数，Project 保存重开精确保留 `INT32_MIN/INT32_MAX`。另覆盖拖动按下后 Agent 改位并不移动即释放时画布对齐最新位置，以及拖动越阈值后 Agent 改位并 ABA 返回原坐标时 generation 检查拒绝旧手势；还覆盖 Agent 设位→真实 GUI 鼠标拖动→Controller Undo，核对失败返回、expired 审计、Agent 栈正常导航且 Project/Canvas 不变。stale 命令完成 Undo→Redo 后再次导航仍返回 expired/显示失效提示，且不改数据。Agent 批次中的 `set_module_position` 从相邻宏隔离；新增 `rename_before → set_module_position → rename_after` 三调用测试，逐项断言 callId、三个栈项、位置 stale 后前后两个 rename 各自 Undo/Redo，证明两侧宏边界均有效。

定向 QtTest 全套：Project 15/15、FlowCanvas 22/22、MainWindow 74/74、AgentActor 48/48、AgentController 41/41，均 0 失败/跳过；完整构建 `build-slice15.log` 退出码 0。已授权 loopback 环境串行全量 CTest `ctest-slice15.log/.xml` 为 **78/78、0 失败、0 跳过、退出码 0**；JUnit XML 可解析。格式门禁覆盖本片相关 Project、AgentActor/Controller、FlowCanvas、MainWindow 及对应测试文件，`clang-format --dry-run --Werror` 与 `git diff --check` 均通过。定向全套日志保存在 `build-codex-agent/agent-phase4-evidence/*-slice15-targeted.log`；并发/stale 重点复现另有 `test_mainwindow-slice15-racefix.log`、`test_agentcontroller-slice15-racefix.log`、端点序列化 `test_project-slice15-endpoint.log`。

源码/证据 SHA-256 清单 `sha256sums-slice15.txt` 锁定 28 项，二进制清单 `binary-manifest-slice15.txt` 锁定 79 项，均已逐项校验通过。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对最终快照判定**第 15 片 PASS，阻断项 0**。审核复核重复 stale Agent/GUI 导航仍逐次返回 expired/显示提示、真实 4x viewport ±10.25 拖动量化及 int32 端点保存重开；独立运行五组关键定向用例且全部通过，校验源码/证据 28/28、二进制 79/79、JUnit/CTest 78/78（无失败/跳过）、五套定向全套、格式门禁与 `git diff --check`。审核者未独立重跑全量构建或 CTest，系核验已归档的构建/全量测试原始日志及摘要。非阻断边界：本片不覆盖 GUI/Agent 全局时间线 Undo、真实设备流程及其他工具跨入口 Undo。

**仅放行继续阶段 4；G4 整体仍 IN_PROGRESS，不放行阶段 5。**未提交、未推送。

## 24. 第 16 片：Agent Undo 不跨越不可逆动作（切片 PASS）

编码前冻结合同 `本地逐切片契约（未纳入提交）` 于 2026-10-21 经独立 Agent `/root/phase4_slice2_independent_audit` 批准（阻断项 0）。合同把 Agent 撤销屏障限定为：非空目标路径并进入 `ProjectMigrator::saveWithBackup` 的保存尝试、RunEngine 接受的 `run_flow`、以及 `RunEngine::stepOnce()` 已执行并发回本次结果的单步。保存实现前拒绝与执行前失败不设屏障；因迁移备份可能先被修改，进入保存实现后即使返回 `SAVE_FAILED` 也保守设屏障；已执行失败单步仍返回 `STEP_FAILED`，但包含 `details.executionAttempted=true` 和 `runId`。

实现新增 Agent Undo 屏障索引，并在新建/打开/关闭工程时随栈清空。Actor 的用户撤销入口在屏障处拒绝导航；Controller 记录 `blocked_by_undo_barrier` 并发出 `undoRejected`，MainWindow 状态栏显示原因。Agent 批次在保存、运行或单步前关闭当前 Undo macro，使屏障前后编辑分别保留；失败调用仍按既有规则停止批次并为后续 callId 补 skipped，不自动回滚。`undoStack()` 继续保留兼容，但产品源码审计确认 Agent 栈导航仍只在 `AgentActor::undoLastAction()` 内；SAM 标注器的独立 Undo 栈不在本片范围。

新增测试：`testStepFailureAfterExecutionEstablishesUndoBarrier` 使用只写临时文件后失败的测试模块，核对失败结果、执行标记、runId、文件副作用及 Undo 拒绝；`testAcceptedRunFlowEstablishesUndoBarrier` 验证异步接受后即设屏障；`testSuccessfulStepOnceEstablishesUndoBarrier` 验证成功单步亦设屏障；三者均再编辑 B、Undo B 后确认下一次 Undo 在屏障处拒绝，A 保持不变。`testPreExecutionRunFailureDoesNotEstablishUndoBarrier` 与 `testPreExecutionStepFailureDoesNotEstablishUndoBarrier` 分别验证运行/单步执行前失败仍可撤销先前编辑。`testExecutedStepFailureBatchKeepsCallIdsAndBarrier` 覆盖已执行失败单步批次的 callId、STEP_FAILED 详情、后续 skipped 与屏障；`testReadOnlyQueryBetweenEditsDoesNotEstablishUndoBarrier` 验证只读查询不分割前后两次编辑的 Undo 序列。Controller 测试覆盖保存成功后 B 可撤销但不能跨过保存撤销 A、且保存文件只含 before；无路径保存前置失败不设屏障且批次 after 被 skipped；保存实现后失败设屏障；成功/失败批次的 callId/skipped 完整性；以及旧工程保存屏障后切换新工程可正常撤销新编辑。MainWindow 测试验证拒绝原因为状态栏可见。

验证：`cmake --build build-codex-agent -j2` 全量构建退出码 0（最终整改日志 `build-slice16-reviewfix4.log`）；受限沙箱全量 CTest 日志 `ctest-slice16-reviewfix4.log/.xml` 原样保留，其中 5 项仅因本地监听 `listen()` 权限失败；在此前授权的 loopback 环境复跑串行全量 CTest 为 **78/78、0 失败、0 跳过、退出码 0**（`ctest-slice16-reviewfix4-loopback.log/.xml`）。整改后 Actor 定向 9/9、Controller 定向 6/6（含 init/cleanup，日志 `test_agentactor-slice16-reviewfix4.log`、`test_agentcontroller-slice16-reviewfix4.log`）；相关源码 `clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建出现一条既有未修改文件 `tests/test_agent_sam_tools.cpp:262` 的 range-loop copy warning。当前 Agent 栈调用点审计未发现 Actor/Controller 保护路径外的产品导航；`SamAnnotatorDialog` 独立栈已明确排除。切片摘要 `sha256sums-slice16.txt` 共 18 项，包含实现源码、合同、验收记录及构建/受限与 loopback CTest/定向原始证据；`binary-manifest-slice16.txt` 共 79 项；两份清单均逐项校验通过。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对第四轮最终快照判定**第 16 片 PASS，阻断项 0**。审核核对合同 §11–25 与 Actor/Controller/MainWindow 实现，独立运行 Actor 9/9、Controller 6/6、MainWindow 3/3，验证 JUnit 78/78（0 失败/跳过）、源码/证据 18/18、二进制 79/79、格式门禁和 `git diff --check`。第三轮提出的最低验收缺口均已补齐：成功单步屏障、执行前 run 失败可撤销、只读调用不设屏障、切换工程清除旧屏障，以及成功批次保存文件内容核验。

审核者未独立重跑全量构建或 CTest，依赖已锁定的成功日志/JUnit 与摘要；非阻断边界包括未为忙态/确认拒绝分别增加与既有 Undo 历史组合的专用用例（静态路径确认这些拒绝分支不设屏障），设备副作用归阶段 5。**仅放行继续阶段 4；G4 整体仍 IN_PROGRESS，不放行阶段 5。**未提交、未推送。

## 25. 第 17 片：运行忙态保护 GUI 写入入口（PASS）

按已批准契约 `本地逐切片契约（未纳入提交）` 实施。检查器参数/默认值、启用/断点/备注提交，图像导入，测量拾取，画布连线与文本节点 Drop，流程树排序/工具箱 Drop，以及高级配置确认，均在实际提交边界检查 `RunEngine::isBusy()`；拒绝时提供状态栏反馈并避免 Project、运行时模块、Undo 或视图发生部分写入。测量执行协议登记的 pending input 保留既有拾取例外。高级配置允许编辑对话框草稿，但忙态确定会保留对话框和草稿、拒绝参数命令及清点/清空副作用。

流程树工具箱插入用 token 绑定 placeholder、当前 Project 与延迟回调；引擎同步时保留仍有效的 pending 树项映射，提交前再次验证引擎、工程身份和树项归属，失效后仅清理本次占位状态。树 Drop 的 MIME 类型互斥：插件载荷进入工具箱新增分支；流程树载荷必须含有效 instance ID，且与当前选中流程项 ID 相同。故 source 为空或非空时，插件载荷不会被当作树排序，载荷 A/选择 B 也不会错移 B。运行态字段拒绝时使用字段专用控件恢复，保留未提交备注草稿；此为草稿保留例外，不将其强制刷新为 Project 旧备注。

新增/补强回归覆盖 Running、Paused 和维护租约；忙态 inspector 编辑与停止后的同操作提交；工具箱 queued callback 遇 busy、外部移除 placeholder、Agent 同步、工程替换；有效 MIME 树排序；source-less MIME 冲突；真实 FlowCanvas 端口鼠标拖线；高级配置草稿和 N 点标定清点副作用；pending 测量输入规则。新回归 `testG4SourceLessDropRequiresMatchingPayload` 验证树 MIME A/当前选中 B 时拒绝且无副作用，以及有流程项选中时 GrabImage 工具箱 MIME 仍新增模块。Qt5 的 `QDropEvent` 测试通过 offscreen 构造事件；未宣称覆盖操作系统级 drag source。

最终构建 `build-slice17-final.log` 退出码 0；完整 MainWindow QtTest `test_mainwindow-slice17-final.log` 为 84/84，完整授权 loopback CTest `ctest-slice17-final.log/.xml` 为 78/78、0 失败、0 跳过、退出码 0；JUnit XML 可解析。相关 C++ `clang-format --dry-run --Werror` 与 `git diff --check` 通过。源码/证据 SHA-256 清单 `sha256sums-slice17-final.txt` 16/16、二进制清单 `binary-manifest-slice17-final.txt` 79/79 校验通过。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对最终快照判定**第 17 片 PASS，阻断项 0**：实际校验两份 SHA 清单、JUnit XML（78 tests/cases，0 failure/skipped）、MainWindow 日志 84/84、构建日志、C++ 格式门禁及 `git diff --check`；核对 §25 实现/测试描述、备注草稿例外、offscreen 限定及未提交/未推送边界均与证据相符。审核者未独立重跑构建或 CTest，而是核验执行方锁定的日志、JUnit 与二进制摘要。非阻断边界：Qt offscreen 合成事件不是系统级 drag source 验证。**仅放行继续 G4；G4 整体仍 IN_PROGRESS，不放行阶段 5。**未提交、未推送。

## 26. 第 18 片：参数 Undo 的 GUI/Agent 来源隔离（PASS）

编码前合同 `本地逐切片契约（未纳入提交）` 经独立 Agent 批准，阻断项 0。Project 为每个实例及参数键维护运行态 GUI/Agent 来源代数；`setModuleParam`、`removeModuleParam`、`updateModule` 的直接未标注写入按 Unknown 同时使两来源旧命令失效，模块增删/重载另以实例代数隔离同 ID 替换。代数不进入工程 JSON。no-op 不递增。

Agent 参数 Undo 从整份快照回写改为仅记录/应用真实变化的键差量；GUI 参数命令同样只改本命令键。两类命令均检查当前 Project/实例身份、跨来源代数和期望键状态；多键命令先构造完整候选参数并进行插件验证，冲突、ABA、实例替换或验证失败均永久 stale，原子 no-op。Agent 参数工具从混合批次 Undo macro 中隔离。GUI 高级配置改为单个原子栈项，保留键缺失与显式 null 的区别；Project 参数更新后同步运行时模块及检查器。Agent stale Undo 经 Controller 记为 `expired`，GUI stale 提示状态栏/配置错误区；忙态和 Slice16 屏障行为保持原规则。

新增回归：`testParameterSourceGenerationsTrackAllModelWrites` 验证 Unknown/GUI/Agent/no-op 代数及不序列化；`testAgentParameterUndoUsesKeyDeltasAndExpiresAtomically` 覆盖跨入口不同键保留、同键/ABA stale、多键冲突原子性、完整候选 min/max 约束、两层 LIFO、同 ID 实例替换及 Slice16 屏障后 stale/Redo；`testAgentParameterUndoPreservesExplicitJsonNull` 验证 Agent Undo 与保存重开保留显式 null。MainWindow 的 `testG4AgentGuiParameterUndoIsKeyScopedAndStaleSafe` 使用真实检查器控件与 Controller Actor 验证双向交错；现明确覆盖 `Agent a → GUI b → Agent Undo/Redo` 和 `GUI b → Agent a → GUI Undo/Redo` 两个不同键序列，并核对 Project 参数、检查器值、工程修改态及双撤销栈 count/index；同时覆盖同键 stale 与 expired 审计。`testG4ResetDefaultsIsOneAtomicUndoCommand` 验证恢复默认是一个多键 Undo 项，Agent 同键冲突后 GUI Undo 全组零副作用；`testG4AdvancedConfigRejectsCrossSourceConflictAtomically` 在高级配置草稿提交前注入 Agent 同键修改，验证整批不写入且草稿/错误反馈可见。真实调用链 `testMeasurementPickingUpdatesInputAndClipboard` 和 `testG4NewImageImportIsRejectedWhileBusyThenCommitsAfterStop` 分别将 Agent 参数命令置于实际测量拾取/图像导入之前，验证直接 `Unknown` Project 写入使 Controller Undo 返回 expired 且不改回实际 GUI 值。最终定向日志为 `test_project-slice18.log`、`test_agentactor-slice18.log`、`test_mainwindow-slice18-reviewfix5-targeted.log`。

独立审核首轮发现检查器“恢复默认参数”仍用逐键 `beginMacro()`，不满足多键全有或全无。已改为单个 `SetModuleParamCommand`，新增 stale 原子性回归。复审又指出真实测量/图像导入 Unknown 写入链路、GUI/Agent 交错栈索引/检查器刷新断言不足，最终复审再指出最低合同要求的双向不同键 Undo/Redo 序列未完整覆盖；均已补入测试。现新增前向 Agent Redo 保留 GUI 键及反向 GUI Undo/Redo 保留 Agent 键的真实 MainWindow 检查器断言，并核对两栈索引。一次新增栈断言曾因 `PropertyPanel::setModule()` 重建控件后测试继续使用旧指针而失败；保留日志 `test_mainwindow-slice18-reviewfix3-targeted.log`，测试改为在刷新后重新查找控件，非产品代码故障。

最终复核快照：全量构建 `build-slice18-reviewfix5-full.log` 退出码 0；双向交错定向 MainWindow 用例 `test_mainwindow-slice18-reviewfix5-targeted.log` 为 3/3（含 init/cleanup）。受限完整 CTest 原始日志/JUnit `ctest-slice18-reviewfix5-restricted.log/.xml` 为 73/78 通过，失败项仅 5 个本地 listener/socket 测试（`test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro`）；获准 loopback 串行复跑 `ctest-slice18-reviewfix5-loopback.log/.xml` 为 78/78、0 失败、0 跳过，JUnit 可解析。最终源码/证据 SHA-256 清单 `sha256sums-slice18.txt` 20/20、二进制清单 `binary-manifest-slice18.txt` 79/79，均重新生成并逐项校验通过；相关 C++ 格式门禁与 `git diff --check` 通过。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对 reviewfix5 最终快照判定 **PASS，阻断项 0**。审核者校验两份摘要清单、完整构建日志、定向 MainWindow 3/3、loopback JUnit 78/78 与受限环境五项 listener 失败，并独立重跑 MainWindow/Project/AgentActor 定向 CTest；格式门禁和 `git diff --check` 均通过。冻结合同 §24 双向不同键 Undo/Redo 序列已核对。**仅放行继续 G4；G4 仍 IN_PROGRESS，不放行阶段 5。**

边界：本片只关闭参数键的 GUI/Agent Undo 交错，不声称所有字段/结构命令已全局隔离；本片 PASS 只放行继续 G4。G4 其余已知缺口仍包括 Agent `set_breakpoint→run_flow` 实际命中并与 GUI 在同一 runId 交替恢复、`partial_failed` 的逐项用户可见结果，以及混合操作/失败批次/撤销后的完整保存重开差异核验。**G4 仍 IN_PROGRESS，不放行阶段 5。**本片未提交、未推送。

## 27. 第 19 片：Agent 断点与 GUI/Agent 同次运行交替恢复（PASS）

编码前合同 `本地逐切片契约（未纳入提交）` 经独立复审 `APPROVE`，阻断项 0。最终测试使用 MainWindow 共享 Agent Actor 和真实 CreateString 插件：Agent 新增两个当前工程节点、分别设置断点，再通过 `run_flow(mode=once)` 自行加载并启动；首断点前没有任何模块开始/结束。GUI 运行按钮继续后，首节点恰开始并结束一次，第二断点真实命中；Agent `resume_flow` 后第二节点恰开始并结束一次。三段均断言同一 runId；每次暂停的 `breakpointHit`、Engine `runObservation.currentModule`、Agent `get_run_results.currentModule` 和流程树暂停项一致，最终只有一个成功 `runFinished`。

复现时发现断点发生于 `moduleStarted` 前，因此运行观察此前可能空缺或仍指向上一节点。`RunEngine` 现仅在断点 `Paused` 已提交后、发出状态/命中信号前，将当前 runId 的运行观察 `currentModule` 更新为断点节点；不改 `RunResult`、调度顺序或暂停/恢复语义。回归为 `testG4AgentBreakpointRunAlternatesGuiAndAgentResumeSameRunId`，同时保留既有 RunEngine 断点和 Actor 生命周期用例。

验证：全量构建 `build-slice19-final2.log` 退出码 0；MainWindow 新闭环 QtTest 3/3、RunEngine 断点定向 3/3、AgentActor 断点/生命周期定向 4/4，CTest MainWindow 套件 1/1。受限 CTest/JUnit `ctest-slice19-final2-restricted.log/.xml` 为 73/78，唯一失败为五个既知本地 listener 测试；获准 loopback 全量 CTest/JUnit `ctest-slice19-final2-loopback.log/.xml` 为 78/78、0 失败、0 跳过。源码/证据 SHA-256 清单 `sha256sums-slice19.txt` 13/13、二进制清单 `binary-manifest-slice19.txt` 79/79 均重新生成并逐项校验通过。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对最终快照判定**第 19 片 PASS，阻断项 0**：核对合同与实现，确认共享 Actor、当前工程两个真实 CreateString、无预载/直启、Agent→GUI→Agent 同一 runId 的两次真实断点暂停及最终成功完成；独立校验全量 JUnit 78/78、源码/证据 13/13、二进制 79/79、格式门禁和 `git diff --check`。非阻断边界：未单独窥探 Engine 断点容器（由两次真实 breakpointHit 与暂停作功能验证），未覆盖设备/stop 竞争。**仅放行继续阶段 4；G4 仍 IN_PROGRESS，不放行阶段 5。**

## 28. 第 20 片：partial_failed 逐项用户可见反馈（PASS）

按独立批准合同 `本地逐切片契约（未纳入提交）` 实施。`AgentController` 对执行失败、Actor schema 整批预校验拒绝、Controller malformed-arguments 整批拒绝统一生成按原始 callId 顺序的用户摘要；每项包含调用 ID、工具名与安全状态。同步完成、started/pending/stopping、no_op、failed、skipped 与权限拒绝分别呈现；无 `status` 的成功返回使用保守文案“调用已返回”，不推断为完成；失败文案只使用稳定 errorCode 映射，不拼接原始 error/details/params。批次部分失败时明确提示已发生副作用未自动回滚；预校验拒绝明确零执行。原有每个 callId 的结构化 tool 消息、失败停止、权限门与 Undo 语义未改变。

新增 AgentController 回归覆盖：两次工程编辑成功后调用失败并 skipped（canary 敏感实例值不进入摘要）、no_op 单列、Actor 参数 schema 预检零副作用、Controller malformed JSON 整批拒绝、Observer 的只读成功/写拒绝/只读失败/后续 skipped 按原始顺序归并，并精确验证无 `status` 的 `get_flow_state` 显示“调用已返回”。MainWindow 集成用例通过真实 Mock LLM 响应、确认并调用共享 Controller；用户聊天区可见逐项结果和不自动回滚提示，状态条显示“请求失败”，Agent 操作日志记录 LLM 错误；第二次单工具失败恰产生一个包含 callId 的可见摘要及一条新增错误日志。纯计算 CreateString 的 `run_flow=started` 显示为进行中，不误报完成。

最终复核轮：全量构建 `build-slice20-final.log` 成功；QtTest AgentController 48/48、MainWindow 89/89；受限全量 CTest/JUnit `ctest-slice20-restricted.log/.xml` 为 73/78，5 项失败均为既知本地 socket/HTTP listener bind 限制；已授权 loopback 全量 CTest/JUnit `ctest-slice20-authorized.log/.xml` 为 **78/78、0 失败、0 跳过**。相关 C++ 格式门禁、`git diff --check` 通过；Slice20 源码/证据 SHA-256 清单 15/15、二进制清单 79/79 均逐项校验通过。由于新增 MainWindow 集成回归共用 `tests/test_mainwindow.cpp`，Slice18/19 源码清单同步刷新该文件与验收记录摘要；其历史二进制清单未改写。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对 Slice20 最终快照判定 **PASS，阻断项 0**：复核空 `status` 保守回退、Observer 逐项反馈、MainWindow 错误状态/操作日志/单工具失败去重，并独立核验源码/证据 15/15、二进制 79/79、Slice18 20/20、Slice19 13/13、QtTest 48/48 与 89/89、授权 JUnit 78/78 且无失败/跳过、受限环境仅五项既知 listener 失败、构建日志、格式和 diff 门禁。**仅放行继续 G4；G4 仍 IN_PROGRESS，不放行阶段 5。**未提交、未推送。

## 29. 第 21 片：混合编辑与失败批次的完整 JSON 保存重开闭环（PASS）

编码前合同及空 Undo macro 修订均经独立 Agent `/root/phase4_slice2_independent_audit` 批准，阻断项 0。合同限定只修复失败/no-op/skipped 留下空宏的问题，保持连续成功普通编辑分组以及 position、parameter、barrier 隔离边界。

实现改动：`AgentActor::executeTools` 延迟到首条真实撤销命令入栈时才创建普通批次 macro，全部 AgentActor 撤销命令统一由 `pushUndoCommand()` 处理。新增共享 JSON 结构差异 helper，失败信息可定位对象/数组路径。Controller 回归通过真实 tool-call 路径执行 rename 与坐标设置成功、未知实例 rename 运行期失败、note 调用 skipped；核对逐 callId 恰一条 tool message、`partial_failed`、Undo 栈边界、Controller Undo、测试层 Redo 及每个关键状态的新 `Project` 保存重开。真实 MainWindow/Inspector 回归交错 Agent 键 A、GUI 键 B、Agent 键 C，保留未修改模块的显式 null，逐状态核对完整 Project JSON 唯一差异、检查器值和两来源 Undo 栈，并对混合状态及每个跨来源 Undo/Redo 关键状态保存到临时文件并加载新对象严格比较。另覆盖独立 no-op、普通首项运行失败 + skipped 零栈变化，以及成功普通编辑跨 no-op 保持一个 macro。

最终复审整改快照：全量构建 `build-slice21-reviewfix2.log` 退出码 0；授权 loopback CTest/JUnit `ctest-slice21-reviewfix2-loopback.log/.xml` 为 **78/78、0 失败、0 跳过**。受限 CTest/JUnit `ctest-slice21-reviewfix2-restricted.log/.xml` 为 73/78，唯一失败是已知需本地 socket/listener 的 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro`；本地监听授权复跑五项全通过。完整 QtTest 定向套件：AgentActor 57/57、AgentController 49/49、Project 16/16、AgentUndo 6/6、MainWindow 90/90。最终全量构建、两轮 CTest/JUnit、定向 QtTest 原始日志、格式与 XML 校验结果及摘要清单均保存在 `build-codex-agent/agent-phase4-evidence/`。源码/证据清单 `sha256sums-slice21.txt` **21/21**、二进制清单 `binary-manifest-slice21.txt` **79/79**，在该快照上逐项校验通过。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对 reviewfix2 最终快照判定 **Slice21 PASS，阻断项 0**：核实三项整改——Controller 每个关键状态的 dirty 与 Undo count/index、MainWindow 检查器 A/B/C 实际值、保存前后 Project JSON 严格相等且保存后 clean 并经新对象重开；另独立核验宏延迟创建/隔离语义、源码证据 21/21、二进制 79/79、授权 JUnit 78/78、受限仅五项 listener、五套 QtTest、格式与 diff。边界：本片使用 Project 直接保存/新对象重开，不代表 GUI 菜单文件操作或 G4 总验收。**仅放行继续 G4；G4 仍 IN_PROGRESS，不放行阶段 5。**未提交、未推送。

## 30. 第 22 片：旧工程隐式边类型的 GUI/Agent/Engine 一致性（PASS）

按已批准的 `本地逐切片契约（未纳入提交）` 实施；不改 `Project` 持久化格式或推断写回。`FlowCanvas::resolveConnectionEdgeType()` 对显式类型原样返回，对空类型读取源/目标画布节点的端口声明并调用唯一规则源 `RunEngine::inferEdgeType()`；全量加载与 MainWindow `Project::connectionAdded` 增量同步共用此解析器。

验收覆盖两种端口情形。实际 `条件分支.true` 和 `Parallel.control` 元数据都声明 `control`，同一旧工程快照通过 Agent `get_flow_state`、RunEngine `loadProject` 和 FlowCanvas 得到一致控制边；保存类型仍为空，Project JSON 不变。MainWindow 增量测试确认端口来自真实 PluginManager 元数据、连接按控制边绘制、数据边仍为 data、Project 只出现调用方新增的空类型连接、Agent Undo 栈 count/index 不变。独立全量画布测试另用无端口声明模块覆盖兼容端口名 `next→control`，并核对隐式规则、显式 data 优先和 Project JSON/modified 零副作用。

旧实现先红：`repro-red-flowcanvas.log`、`repro-red-mainwindow.log`、`repro-red-crosslayer.log` 均在 `isControlEdge()==false` 失败；修复后 `repro-green-flowcanvas.log`、`repro-green-mainwindow.log`、`repro-green-crosslayer.log` 均为 3/3（含 init/cleanup）。最终全量构建 `build-full.log` 退出码 0；包含最终测试源码的目标构建 `build-targets.log` 退出码 0。受限 CTest/JUnit `ctest-restricted-final.log/.xml` 为 73/78，五项 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro` 因受限环境本地监听权限失败；此前授权 loopback 复跑 `ctest-loopback-final.log/.xml` 为 78/78，0 失败、0 跳过。两份 JUnit XML 均可解析，格式门禁和 `git diff --check` 通过。最终源码/证据 SHA-256 清单 `sha256sums-slice22-final.txt` 22/22、二进制 manifest `binary-manifest-slice22-final.txt` 7/7，均逐项校验通过，待独立审核复核。

2026-10-21，独立 Agent `/root/phase4_slice2_independent_audit` 对最终快照判定 **Slice22 PASS，阻断项 0**：独立核对真实控制端口元数据、Actor/Engine/Canvas 同快照、两条 GUI 入口共用推断、Project 无推断写回、MainWindow Undo/JSON、红绿日志、22/22 源码证据、7/7 二进制摘要、授权 JUnit 78/78 与受限环境五项已知 listener 失败，并独立重跑三个定向用例各 3/3。审核者未独立重跑完整构建/全量 CTest，系核验本片归档原始日志。**仅本 Slice22 PASS；G4 仍 `IN_PROGRESS`，不放行阶段 5。**未提交、未推送。

## 31. 第 23 片：GUI 循环运行共享生命周期与轮间忙态（PASS，仅本片）

依照已批准合同 `本地逐切片契约（未纳入提交）` 实施。GUI 循环改由 RunEngine 的共享 cycle session 管理：每个会话拥有绑定 sessionId 的 timer，首轮 GUI 行为保持立即执行，后续每轮在 `runFinished` 回调返回后再等待 1 ms；Agent 的 pause/resume/stop 因而与 GUI 循环共享同一生命周期。每次 tick 在 Engine 生命周期锁内核对 sessionId，旧会话排队/迟到的 timer 不会启动新会话的轮次。GUI cycle 执行失败或运行前校验失败均保留会话并继续调度，失败 iteration runId 可查询为 failed，而 cycle session 仍 running/busy。Agent 原有默认 100 ms cycle timer 路径不变。

ProjectManager 底层与 checked 新建/打开/关闭入口共用 Engine 忙态门；checked 门在未保存策略前拒绝。MainWindow 新建、方案列表、打开及关闭入口在未保存询问/文件选择器前拒绝活动 GUI cycle；流程树 Delete 键真实入口也在运行时显示忙态拒绝。每个 GUI cycle iteration 开始时重置该轮高亮/耗时展示。Engine 模块同步记录当前工程身份；MeasurementInput 等待拾取时创建 deferred shared session，支持 Agent pause/resume/stop，拾取完成前不启动模块。若为消费者自动创建 MeasurementInput，先写入 Project，再同步运行图；拾取提交后刷新共享 session 的工程指纹并续跑，查询不误报 stale。停止后 UI 由 Engine cycleStopped 收敛，原 GUI 自管的 runFinished→1 ms continuation 已移除。

新增确定性复现：MainWindow 六项 cycle 用例覆盖轮间 pause→resume、pause→stop、执行期失败继续、预先存在及消费者自动创建的 MeasurementInput、Delete 入口拒绝、stop 后恢复/保存重开、旧 GUI timer 对新 GUI 会话无影响；RunEngine 两项覆盖旧 timer 身份及必需输入缺失导致的校验失败后继续。针对独立复审发现的“Agent 在 deferred MeasurementInput 等待期停止后，旧 pending ID 可让下一次忙态运行中的点选绕过拒绝门”，统一停止清理现在同时复位 pending/active 目标和树项状态；新增 Agent stop→启动新忙态 cycle→两次旧目标点选的回归，断言完整 Project JSON 与 Engine 执行计数不变。定向 QtTest MainWindow 8/8、RunEngine 4/4（均含 init/cleanup）。最终全量构建成功，受限 CTest/JUnit 为 73/78；五项 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro` 在受限环境因本机 socket/listener 绑定失败。按既有授权 loopback 全量 CTest/JUnit 为 78/78、0 失败、0 跳过。两份 XML、原始日志、退出码、格式门禁与 diff 检查及 SHA-256/二进制清单归档于 `build-codex-agent/agent-phase4-slice23-evidence/`。

2026-10-22，独立 Agent `/root/phase4_slice2_independent_audit` 对整改后最终快照判定 **Slice23 PASS（仅限本片）**：核实 deferred stop 清除待拾取状态、下一忙态会话点选被拒且 Project JSON/运行计数不变；独立校验源码/证据 SHA-256 29/29、二进制 7/7、loopback JUnit 78/0/0、受限 JUnit 78/5/0（仅五项已知本机 listener 限制）、定向 MainWindow 8/8，并确认 diff 检查干净。其指出定向回归通过直接槽调用而非 viewport 鼠标事件；点选忙态门实际位于该槽，故不构成本片阻断。

Slice23 PASS 仅放行继续阶段 4：`flowId` 多流程缺口仍按合同为 G4 阻断项，G4 不标 PASS、不放行阶段 5。未提交、未推送。

## 32. 第 24 片：ProjectFlow 元数据与 flowId fail-closed（PASS，仅本片）

按独立审核批准的 `本地逐切片契约（未纳入提交）` 实施。本片不实现多流程选择或执行。`get_flow_state` 新增来自 `Project::flows()` 的有序 `flows[{id,name,nodeIds}]` 元数据；工程未存储 flow 时按持久化兼容规则返回隐式 `main`（全模块 ID 顺序），同时保留 `executionScope=project_modules` 与 `flowSwitchSupported=false`。

五个流程工具的 OpenAI/Claude 发布 Schema 均增加可选字符串 `flowId`，并明确 selector 不可用。Actor 先校验非字符串/空白值为 `INVALID_PARAM`；任意显式非空 selector（包括 `main`）都在执行、Engine 同步、校验与 Undo 屏障前返回 `FLOW_UNAVAILABLE`、`status=unavailable`、`retryable=false`。Actor 批次在执行前整体预检并为后续 callId 补 `SKIPPED_DUE_TO_PREVIOUS_ERROR`。聊天/LLM 与 Bridge 路径先走权限门，再做纯 selector 预检；Observer 写请求优先得到 `PERMISSION_DENIED`。聊天批次若存在权限允许但 selector 不可用的调用，则确认和工具执行均不发生，逐 callId 回填错误/跳过/权限结果和审计。安全摘要只使用静态 `FLOW_UNAVAILABLE` 文案，不包含调用方 flowId。

回归新增：`test_toolschema` 验证五个工具双格式 schema；`test_agentactor` 验证两个显式 flowId 对五个工具全部 fail-closed、零 Project/Undo/Engine/run 信号副作用、多流程原始元数据（含交叠与未归属 node ID）、隐式 main、Actor 批次 callId/skipped 及非法类型优先级；`test_agentcontroller` 通过 Mock LLM 实际发起请求并核对请求 Schema 已携带 `flowId`，验证混合批次不确认/不执行、Observer 写 selector 权限优先、Bridge 不建确认队列以及聊天批次非法类型优先。无 flowId 的既有全工程路径由原有测试继续覆盖。

首轮独立实现复审判定 NEEDS_CHANGES：发现确认前 selector 批次中的未执行调用虽返回 `SKIPPED_DUE_TO_PREVIOUS_ERROR`，操作日志却记为 `error`；并指出缺少同一 Observer 批次 unavailable/skipped/denied、`run_flow` 的 `mode=cycle` 及完整 flow 元数据断言。整改将跳过项审计结果改为 `skipped`，补充每个 callId 恰有一条 tool 消息及对应审计值断言、Observer 三类结果混合批次、once/cycle 双模式拒绝和 `flows` 全字段精确比较。整改后定向 AgentActor 59/59、AgentController 53/53、ToolSchema 16/16。

整改快照的验证证据归档于 `build-codex-agent/agent-phase4-slice24-evidence/`：最终全量构建 `build-full.log` 退出码 0；AgentActor 59/59、AgentController 53/53、ToolSchema 16/16；受限 CTest/JUnit 为 73/78，五项 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro` 仅因本机 socket/listener 限制失败；经授权 loopback 全量 CTest/JUnit 为 78/78、0 失败、0 跳过。XML 解析、`clang-format --dry-run --Werror`、`git diff --check` 均通过。源码/证据 SHA-256 清单 `sha256sums-slice24.txt` 为 19/19、构建二进制摘要 `binary-manifest-slice24.txt` 为 6/6，整改后均逐项校验通过；独立复审待完成。

2026-10-22，独立 Agent `/root/phase4_slice2_independent_audit` 对整改快照复审判定 **Slice24 PASS（仅本片），阻断项 0**。审核确认 skipped 审计分类正确；Autopilot/Observer 混合批次逐 callId 恰有一条 tool 消息且错误、跳过、权限拒绝的审计分类分别正确；`mode=once/cycle` 均 fail-closed；ProjectFlow 的 id/name/nodeIds 全字段精确比较。审核独立重跑新增 Controller 用例 5/5，校验定向 Actor 59/59、Controller 53/53、Schema 16/16，源码/证据摘要 19/19、二进制 6/6、loopback JUnit 78/78、受限 JUnit 78/5/0（仅五项已知监听限制）、格式门禁与 `git diff --check`。审核者未独立重跑全量构建或 CTest，而是核验归档日志/XML与摘要。该结论只关闭 Slice24 切片门，不代表真实多流程执行，不构成 G4 PASS，也不放行阶段 5。

未提交、未推送。

## 33. 第 25 片：Agent 断开连接 Undo 恢复原序（PASS，仅本片）

依照独立批准合同 `本地逐切片契约（未纳入提交）` 实施。阶段总审明确发现，`AgentDisconnectCmd` 只保存连接对象，Undo 默认追加到 Project 连接列表末尾，断开原序中间项会改变 `Project::toJson().connections` 顺序。

`AgentDisconnectCmd` 现在与连接对象一起保存原始列表索引；Undo 调用 `Project::addConnection(connection, originalIndex)`，Redo 仍按完整端口四元组精确删除，不改变其他边的相对顺序。扩展 `testG4ParallelPortsDisconnectPreciselyAndUndo` 使用真实 `connect_modules`/`disconnect_modules` 建立三条有效连接（其中同一节点对存在两条不同端口边），精确断开中间项，逐项比较断开、Undo、Redo、再次 Undo 的完整连接 JSON 数组，并保存重开验证顺序。

测试先行复现：扩展回归在旧实现上运行并于 Undo 后完整 `connections` 数组比较失败；该次失败 stdout 未单独归档。修复后定向用例 3/3。最终全量构建 `build-codex-agent/agent-phase4-slice25-evidence/build-full.log` 退出码 0，AgentActor 59/59；受限 CTest/JUnit 73/78，五项 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro` 因本机 socket/listener 限制失败；此前用户授权的 loopback 全量 CTest/JUnit 78/78、0 失败、0 跳过。相关证据及源码/证据 SHA 与二进制清单归档在同目录，格式门禁与 `git diff --check` 通过。

2026-10-22，独立 Agent `/root/phase4_slice2_independent_audit` 判定 **Slice25 PASS，仅本片，阻断项 0**。审核者核对原索引捕获、三边中间断开及完整 Undo/Redo/保存重开断言，独立重跑定向用例 3/3，并校验源码/证据 12/12、二进制 6/6、Actor 59/59、授权 JUnit 78/78、受限仅五项既知 listener 失败、格式和 diff 检查。审核者未独立重跑全量构建/CTest；非阻断文档备注：首次旧实现失败轮没有单独归档原始 stdout。该切片结论只关闭 G4 #1/#6 的中间边顺序问题；字段/排序跨入口 stale Undo 风险由后续切片处理，G4 总审另见 §3。

## 34. 第 26 片：模块运行字段异源 Undo 保护（PASS，仅本片）

编码前契约 `本地逐切片契约（未纳入提交）` 经独立 Agent `/root/phase4_slice2_independent_audit` 批准后实施。Project 对每个模块的 `note`、`enabled`、`breakpoint` 维护字段级 GUI/外部代次；Agent 字段命令绑定外部代次、当前预期值、Project 身份和模块生命周期代次。Agent 自身 Undo/Redo 不推进外部代次，保留同字段多层 LIFO；GUI 字段变化、ABA、Project 替换或同 ID 删除重建会使旧命令永久 stale。过期 Undo 通过 Controller 返回失败并审计 `expired`，不写 Project/Inspector/RunEngine。三个字段命令从 `executeTools()` 混合宏中隔离为独立 Undo 项，避免 stale 子命令造成兄弟操作部分撤销。

先行红测原始输出 `build-codex-agent/agent-phase4-slice26-evidence/red-test-mainwindow.log` 以旧实现复现了五项失败：Inspector 改字段后 Agent Undo 仍成功并覆盖 GUI；同字段 GUI ABA 后 Agent Redo 覆盖最终值；rename/note/rename 批次仍合成一项；ProcessTree 删除并重建同 ID 后旧 Undo 作用于新实例。修复后 `test_mainwindow` 四项定向回归 6/6（含 init/cleanup）：三字段 stale Undo 与完整 JSON/控件/RunEngine 对比、三字段 Agent LIFO、启用/断点 ABA、备注 ABA、同/跨模块字段隔离、混合宏零覆盖、真实 ProcessTree 删除与同 ID 重建均通过。既有 `test_agentundo`、`test_agentcontroller` 批次宏断言已更新为运行字段独立栈项。

验证证据归档于 `build-codex-agent/agent-phase4-slice26-evidence/`：最终全量构建退出码 0；受限 CTest/JUnit 73/78，五个失败套件 `test_agentbridge`、`test_sambackendclient`、`test_sam_region_flow`、`test_agent_sam_tools`、`test_agentphase0_repro` 均因受限环境不能绑定本机 socket/listener；同一最终重建快照的授权 loopback 全量 CTest/JUnit 78/78、0 失败、0 跳过。Slice26 定向 MainWindow 回归 6/6；ABA 后 stale Undo/Redo 对目标字段的 Project JSON、Inspector 和 RunEngine 状态均断言无变化。Slice26 相关 C++ 格式门禁、XML 解析、`git diff --check`、源码/证据 SHA-256 21/21 与二进制摘要 7/7 均通过。独立 Agent `/root/phase4_slice2_independent_audit` 于 2026-10-22 判定 **Slice26 PASS，仅本片，阻断项 0**；其独立重跑四项 MainWindow 定向用例 6/6，并核验最终清单与归档 CTest/JUnit。审核者未独立重跑全量构建/CTest，依赖锁定日志和 JUnit。非阻断边界：同 ID 重建测试通过真实树删除后 Project API 重加实例，未模拟工具箱拖放；部分 Redo 验证使用测试层 Undo 栈入口。Slice26 仅关闭 `note`/`enabled`/`breakpoint` 三个运行字段异源 stale Undo 风险；模块顺序 stale Undo 和 G4 历史记录/矩阵同步仍未完成。**G4 阶段总审仍为 NEEDS_CHANGES，不放行阶段 5。**未提交、未推送。
