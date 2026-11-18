# 阶段 6 验收记录

> 历史验收快照：本文状态与结果对应其记录日期和基线；当前项目状态见 [docs/STATUS.md](../STATUS.md)。
>
> **证据可达性：**文中 `build-codex-agent/agent-phase6-*-evidence/` 是当时被 `.gitignore` 排除的本地构建输出目录，未随仓库归档；目录当前不在交付树中。本文保留当时记录的摘要结果，但读者无法从本仓库复核其原始日志、哈希清单或独立审核附件。当前候选的可复核门禁见 [docs/STATUS.md](../STATUS.md) 及其链接证据。

## 当前状态

**PASS（阶段 6 软件验收范围）。** G6 标准 1–6 均通过；G6-3 限定于当前仓库 69 项内置插件，第三方动态插件、真实设备物理标定和 GPU 验证仍归阶段 8。最近完整全量回归为 Slice200：全量构建退出码 0；在允许本机 loopback listener 的隔离环境运行全量 CTest **108/108 通过、0 失败、0 跳过**。Slice201 随后加强 3D queued-continuation 回归断言，当前源码重建和四个相关用例 6/6 通过；此后未重跑全量 CTest。Slice200 全量证据见 `build-codex-agent/agent-phase6-slice200-evidence/`，Slice201 刷新证据见 `build-codex-agent/agent-phase6-slice201-refresh-evidence/`。

Slice194 补齐普通 GUI `MeasurementInput` 换图/停止/迟到 release 生命周期；Slice195 完成固定假服务 SAM GUI 提示、会话保存重开与 LabelMe 导出闭环。Slice196 建立坐标生产者矩阵并经独立审核 **AUDIT_PASS**；Slice197 覆盖校准 provenance 组合溢出 fail-closed，Slice198 覆盖当前支持的 v3、v2 与缺省版本 markerless line 输入。Slice199 将69项内置插件源码/metadata哈希审计和现有算法坐标输出用例纳入矩阵；G6-3 经独立终审 **AUDIT_PASS**，已在计划中勾选，限定于当前内置插件清单。

## Slice 1：知识库文档管理

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**。最终实现经 `/root/slice16_implementation_review` **PASS，无阻断问题**。F09 仅核验尚缺失的 GUI 操作映射；工具目录由 `test_toolschema` 单独覆盖，不再依赖易碎的硬编码总数断言。

Agent 与 `KnowledgeBaseDialog` 共用 `AgentActor` 的文档列表、预览、导入、移除入口。新增只读 `list_knowledge_documents`、`preview_knowledge_document`；新增需确认的 `add_knowledge_document`、`remove_knowledge_document`。导入只接受普通、可读的 Markdown/TXT 文件，以原 basename 复制且不覆盖；保留源文件。移除只作用于用户知识库副本，内置文档只读。列表/检索排除用户目录的 symlink；预览/删除重新核验 ID、根目录、普通文件、内容摘要与文件身份。确认窗口内的内容变更、路径替换和同路径同字节替换均使批准过期。Agent 与 GUI 的同进程导入用互斥锁序列化；不声明跨进程写入协调。

`read_documentation` 兼容保留 `topic/content/sections`，其中 `sections` 为当前页条数；增加有界 `offset/limit/totalSections/hasMore` 分页（默认 0/10，limit 1–50）。未接入表达式/语义搜索、Markdown 渲染、文档编辑、多用户权限或 Agent 自主磁盘目录扫描。

### 验证

- 全量构建：`cmake --build build-codex-agent -j2`，退出码 0。
- 全量回归：在允许本地 TCP/socket listener 的执行环境运行 `QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure`，**91/91 通过，0 失败，0 跳过**。先前受限沙箱中 `test_agentphase0_repro` 的 listener 被环境拒绝；获批回环环境复跑通过。
- 定向：`test_toolschema`、`test_agentactor`、`test_agentcontroller`、`test_knowledgebasedialog`、`test_agentphase0_repro`，**5/5 通过**。
- 覆盖：ID/路径遍历、内置只读、symlink、预览上限、源文件不变、同名/并发导入、写入失败、权限确认、确认快照过期和文件替换、分页边界、GUI 刷新。
- `clang-format --dry-run --Werror`、`git diff --check`、基线矩阵引用校验均通过；矩阵为 483 个证据单元格引用校验通过、66 模块/3 类相机、111 项 GUI 操作。
- 原始日志、审核结论、脚本输出、哈希清单与相关二进制清单见 `build-codex-agent/agent-phase6-slice1-evidence/`。

### 边界与剩余工作

本机验证基于 Linux、Qt 5.15.3 和本地文件系统。稳定文件身份不可用时 Agent 导入/移除确认 fail-closed；同进程操作互斥，但不锁定其它进程写入，也不消除所有文件系统 TOCTOU。知识库切片只关闭列表、预览、导入、移除及检索分页，不代表阶段 6 的数据源、变量、测量/ROI、SAM、视图/设置、登录/模型环境交接已完成。

## Slice 2：全局变量类型化 CRUD

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**。实现经 `/root/slice16_implementation_review` 第二轮独立复核：上轮五项发现全部关闭，未发现新的 manager 锁序、权限门或确认过期阻断。Slice 2 软件范围完成；阶段 6 仍为 `IN_PROGRESS`。

新增 `list_global_variables`、`get_global_variable`、`create_global_variable`、`set_global_variable`、`remove_global_variable`。Agent 与 `GlobalVarView` 共用按值快照及 `try*UserVariable` 管理入口；八种 scalar/array JSON 类型严格往返，不做表达式求值，set 保留 expression/link/note。修改需要确认，绑定按批次位置和变量稳定身份检查；缺失或重复的 LLM call ID 在写入历史前归一化。变量值从普通审计参数中脱敏。管理写事务与 RunEngine 通过线程无关 lease 门线性化，暂停、cycle 间隙及停止通知完成前保持互斥；GUI 自动名在编号出现空洞时选择可用名，忙碌时告知用户。

### 验证

- 完整构建：`cmake --build build-codex-agent -j2`，退出码 0。
- 全量 CTest：允许 localhost listener 的环境中 `ctest --test-dir build-codex-agent --output-on-failure`，**92/92 通过，0 失败，0 跳过**。
- 定向覆盖：`test_globalvarview`、`test_runengine`、`test_toolschema`、`test_agentactor`、`test_agentcontroller` 均通过。新增测试覆盖全部八种类型 CRUD、逐项类型错误零副作用、Observer 与确认拒绝/批准、变量漂移与删除重建过期、重复 call ID 下批次首项漂移过期、审计脱敏、GUI 空洞命名与 ENGINE_BUSY 可见提示，以及 Running/Paused/cycle/停止通知阶段 lease 状态。
- `docs/baseline/agent-capability-matrix.json` 重新生成：69 插件（66 模块 + 3 相机）、115 GUI 操作；483/483 单元格的 `testId/evidencePath` 引用校验通过。
- `clang-format --dry-run --Werror`、`git diff --check` 和 SHA-256 清单均须在最终证据目录中复核；日志与独立审核结论见 `build-codex-agent/agent-phase6-slice2-evidence/`。

### 边界与后续

全局变量是进程态，不随 Project 保存/重开。表达式执行、队列、复杂变量值及所有遗留裸指针调用点的全面清理不在 Slice 2。阶段 6 的数据源、ROI/测量、人机采点、SAM 行为/导出、视图/设置和真实登录/模型环境交接仍待实施或现场验证；不可把本切片的 PASS 扩大为整个阶段 6 完成。

## Slice 3：数据源枚举与安全移除引用

契约：本地逐切片契约（未纳入提交），`/root/slice19_contract_review` **CONTRACT_PASS**。最终实现经 `/root/slice16_implementation_review` 独立复审 **PASS**；其首轮发现确认对象地址可复用 ABA 风险后，已改为 Project 进程级单调 `runtimeIdentity` 并补同路径同内容重开回归，最终复审未发现新阻断。

新增只读 `list_data_sources({type?})`，按当前工程顺序返回 ID、名称、类型、路径和文件存在状态；仅列元数据，不加载或解码文件。新增需逐项确认的 `remove_data_source_reference({dataSourceId})`，只移除工程引用并发出现有 `dataSourceRemoved` 信号；源文件原字节和路径保留。重复 ID 明确拒绝。移除通过 RunEngine 生命周期锁下的维护租约与运行启动互斥；确认绑定 Project 运行时身份、内容指纹和数据源集合代次，关闭重开、集合替换或内容变化均使旧确认过期。Observer 可列举但不能移除。

### 验证

- 最终全量构建：`cmake --build build-codex-agent -j2`，退出码 0。
- 修正后的全量 CTest：允许本地 loopback listener 的环境中 `ctest --test-dir build-codex-agent --output-on-failure`，**92/92 通过，0 失败，0 跳过**；最终日志见 `build-codex-agent/agent-phase6-slice3-evidence/ctest-full.log`。
- 定向：`test_mainwindow`、`test_runengine`、`test_toolschema`、`test_agentactor`、`test_agentcontroller`，5/5 通过。覆盖双向运行/维护租约竞争、GUI 数据源树刷新、列表过滤、Observer 拒绝、Advisor 确认拒绝/批准、重复 ID 拒绝、工程代次漂移和同路径同内容关闭重开失效、混合危险工具批次漂移时整批零副作用、源文件字节保留。
- 基线矩阵重新生成并验证：69 插件（66 模块 + 3 相机）、116 项 GUI 操作、483/483 单元格引用有效；`DS_LIST` 与 `DS_DELETE_REF` 已映射到对应工具。
- 构建、格式、diff 检查、独立审核文本与哈希清单见 `build-codex-agent/agent-phase6-slice3-evidence/`。

### 边界与后续

本切片不实现数据源导入/解码、显示/新视口、可见性、绑定模块或 Explorer/剪贴板操作；不表示图像或点云已加载或显示。这些操作仍保留在能力矩阵，阶段 6 继续 `IN_PROGRESS`。

## Slice 4：数据源主视口显示与可见性

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**。最终实现经 `/root/slice16_implementation_review` 独立复审 **PASS**。首轮发现的数据源面板重复 ID 行会被错误重置为 Checked 已关闭：重复行现在保留既有 checkbox 状态，并由回归验证隐藏数据源加入同 ID 不同路径记录后，面板、视口和工程状态均保持稳定。

新增只读 `get_data_source_view_state`，查询当前存活 `MainWindow` 对应工程的运行时显示缓存、隐藏集合和已有专用视口；重复 ID 整体返回 `DATA_SOURCE_ID_AMBIGUOUS`，不按 ID 缓存猜测对象。新增需逐项确认的 `display_data_source` 与 `set_data_source_visibility`，仅操作已登记的数据源，并复用现有图像/点云 loader 与主视口；不会新建视口、修改 Project/GrabImage 或写入源文件。窗口 handler 由 `QPointer` 绑定 owner，拒绝非 owner thread 调用；确认快照同时绑定主窗口身份，窗口注销或替换后批准失效。Observer 可查询但写操作被统一权限门拒绝。

### 验证

- 最终完整构建：`cmake --build build-codex-agent -j2`，退出码 0。
- 最终全量 CTest：`ctest --test-dir build-codex-agent --output-on-failure`，**92/92 通过，0 失败，0 跳过**；完整输出在 `build-codex-agent/agent-phase6-slice4-evidence/ctest-full.log`。
- 定向：`test_mainwindow` 两项 Slice 4 用例所在调用 **4/4**（含 init/cleanup），`test_toolschema` **20/20**，`test_agentpermissions` **8/8**。覆盖图像与 PLY 点云进入主视图、未缓存点云通过可见性入口加载、Observer 拒绝和 Advisor 批准、副作用审计 `undoable=false`、可见性 UI 同步、解码失败回滚、重复 ID 查询/显示/可见性拒绝、工程和源文件不变、无 GUI owner/owner 失效/跨线程拒绝。日志分别位于 Slice 4 证据目录。
- 基线矩阵由审计脚本生成：69 插件、119 项 GUI 操作；483/483 单元格的 `testId/evidencePath` 引用验证通过。`DS_DISPLAY_MAIN`、`DS_VIEW_STATE` 和 `DS_SET_VISIBILITY` 已映射到对应工具。
- 独立审核、构建/测试日志、哈希清单及相关二进制清单存放于 `build-codex-agent/agent-phase6-slice4-evidence/`。

### 边界与后续

本地回归使用 Qt offscreen 和小型测试文件。它验证了点云 loader、缓存和 Auto3D 视图状态，但 offscreen 环境无法创建 `QOpenGLWidget` context，因此不作为真实 GPU 绘制、超大文件性能或现场格式兼容性的验收。数据源绑定模块、ROI/测量、人机采点、SAM 对象与导出、视图/设置及登录/模型环境交接仍属阶段 6 后续；阶段 6 保持 `IN_PROGRESS`。

## Slice 5：验证并登记数据源引用

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**。最终实现经 `/root/slice16_implementation_review` **PASS**，软件范围验收 **PASS**。两轮实现复审关闭了截断 PLY 误接收、非 owner 线程错误优先级、非法元素声明、float32 越界值和顶点后元素载荷未完整校验问题。

新增危险工具 `import_data_source({path})`，只在当前工程登记文件引用。逐项确认绑定本地普通文件的 canonical path、稳定文件身份、字节数和 SHA-256，并绑定 Project 身份/内容/数据源代次；执行前在 Project owner thread 下取得 RunEngine 维护租约并复核。验证和完整解码都在唯一的 `Project::addDataSource` 提交之前完成；Observer 拒绝，Advisor/Autopilot 逐项确认。混合批次仅把本批次精确成功登记的引用作为后续 Project 基线前移；同步监听器引起的无关 Project 变化会令后续项过期。

支持 PNG/JPG/JPEG/BMP 图像、PLY 点云和 TIFF 高度图点云。PLY 接受 ASCII 与 little-endian binary，验证声明的 vertex 及后续 scalar/list 元素载荷、记录长度、类型范围和尾随内容；不完整或不支持的编码拒绝且不留下部分点云。该工具不显示或切换视口、不配置 GrabImage、不写入/复制/移动源文件；需要显示时另调 Slice 4 `display_data_source`。成功审计 `undoable=false`，只写 Project 引用。

### 验证

- 完整构建：`cmake --build build-codex-agent -j2`，退出码 0；构建日志见 `build-codex-agent/agent-phase6-slice5-evidence/build.log`。
- 定向与完整回归：`test_plyloader`、`test_agentactor`、`test_agentcontroller`、`test_agentpermissions`、`test_runengine`、`test_toolschema` 均通过；允许本机 loopback TCP/socket 的全量 CTest **92/92 通过，0 失败，0 跳过**，日志 `build-codex-agent/agent-phase6-slice5-evidence/ctest-full-loopback.log`。沙箱内首轮因 listener 权限失败的单独日志保留为 `ctest-full.log`，不计作产品回归结果。
- 回归覆盖完整与截断 ASCII/binary PLY、截断后续 face/list 元素、非法 element 计数、float32 越界、失败清空部分点云、辅助线程在 maintenance lease 期间优先返回 `THREAD_AFFINITY`；并覆盖无 GUI handler 导入、权限/确认、文件变化、Project 漂移、幂等、源文件字节、`GrabImage` 与视图状态、两种 lease 竞争和混合批次同步监听器漂移。
- 基线矩阵重生成：69 插件（66 模块 + 3 相机）、120 项 GUI 操作；483/483 单元格 `testId/evidencePath` 引用校验通过。`DS_IMPORT` 映射为 `import_data_source` 后接 `display_data_source`；单独的 `DS_BIND_GRABIMAGE` 映射到 `list_data_sources` + `set_module_params`。
- `clang-format --dry-run --Werror`、`git diff --check`、SHA-256 和二进制清单见 Slice 5 evidence 目录。

### 边界与后续

本机验证基于 Linux、Qt 5.15.3、离线小文件和本地文件系统；不覆盖大文件性能、其他平台文件身份实现、PLY big-endian/未支持 scalar 编码、真实 GPU 呈现或现场设备。数据源到 `GrabImage` 的模块绑定及其他 GUI 操作仍归阶段 6 后续；不能据 Slice 5 推断阶段 6 整体完成。

## Slice 6：将已登记图像绑定到指定 GrabImage

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**。最终实现经 `/root/slice16_implementation_review` 独立复审 **PASS**；复审指出 GUI 自动绑定的运行时模式值仍写为 `File`，而 Project 写为 `Path`。现已统一写入 `Path`，并增加重复导入已绑定图像的回归：先只将运行时值改为 `File`，确认 Project 中路径和模式已是 `Path`，再重导入同一文件，验证运行时值仍能修正为 `Path`，且数据源数量不增加。

Agent 使用现有 `list_data_sources`、`get_flow_state` 与 `set_module_params` 工具组合。绑定只接受明确指定的 GrabImage 实例和唯一已登记、存在的图像数据源，并把列表返回的 `filePath` 原值与 `grabSource=Path` 写入目标模块；不把 `set_module_params` 伪称为携带数据源 ID，也不触发显示、采集或流程运行。GUI 导入自动绑定、Project、运行时模块和模块检查器均保持 `Path` 与文件路径一致。权限沿用现有普通参数修改策略：Observer 只读且写入被拒，Advisor 拒绝时零副作用、批准时逐项提交，Autopilot 沿用现有工具分类。

### 验证

- 完整构建：`cmake --build build-codex-agent -j2`，退出码 0；日志见 `build-codex-agent/agent-phase6-slice6-evidence/build.log`。
- 完整 CTest：在允许本地 loopback TCP/socket listener 的环境中 **92/92 通过，0 失败，0 跳过**；日志见 `build-codex-agent/agent-phase6-slice6-evidence/ctest-full.log`。
- 定向回归：`test_mainwindow`、`test_toolschema`、`test_agentactor`、`test_agentcontroller` **4/4 通过**。覆盖多 GrabImage 时只修改用户指定实例、无候选与歧义拒绝、Observer/Advisor/Autopilot 策略、拒绝零副作用、确认批准和目标漂移、审计实例与实际参数、保存重开、GUI 检查器同步及重复导入时 Project 无变化信号仍修复运行时模式。
- GUI 操作矩阵维持 120 项，`DS_BIND_GRABIMAGE` 映射为 `list_data_sources + set_module_params`；矩阵生成器的 483 个证据单元格引用校验通过。独立映射复核由 `/root/slice19_contract_review` 确认通过。
- `clang-format --dry-run --Werror`、`git diff --check`、源码/证据 SHA-256 与本切片二进制清单见 Slice 6 evidence 目录。

### 边界与后续

本切片只验证工程参数绑定和 GUI 自动配置的一致性，不证明源图像已解码、已显示、GrabImage 已执行或输出已生成。数据源导入、显示与模块绑定是三个明确的操作。阶段 6 仍为 `IN_PROGRESS`；ROI/测量、人机采点、SAM 会话/对象/导出、视图/设置和登录/模型环境交接仍待各自契约、实现、审核和验证。

## Slice 7：只读分页查询既有 SAM 会话

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**。实现独立复审先发现长重复 ID 可撑大错误响应、增长型设备未走满 64 KiB 分块、SAM_SESSION_OPEN 说明过期及不可读文件分支缺少用例；整改后复审又发现解析上下文会带入长路径。最终 Agent 调用不附加路径上下文，重复 ID 错误不回显 ID，五项问题全部关闭，最终复审 **PASS**。

新增只读 `get_annotation_session({sessionPath,offset,limit,includePolygon})`，只读取用户明确给出的普通非 symlink 会话文件。用同一个已打开文件句柄分块读取，parser 输入最多 64 MiB + 1，Qt 固定内部缓冲预读最多 64 KiB；`load` 与新 `loadFromBytes` 共用严格 schema 和 ID 唯一性校验。返回分页对象摘要、原图坐标 bbox、prompt 计数及可选完整 polygon；永不返回 maskRle，不打开 GUI、不解码图像、不调用 SAM、不改写会话或 Project。完整成功和错误响应均受 65,536 UTF-8 字节上限约束，解析错误不回显对象字段或路径。Observer、Advisor、Autopilot 均可直接查询，不需确认。

### 验证

- 最终全量构建 `cmake --build build-codex-agent -j2` 退出码 0；最终全量 CTest **92/92 通过，0 失败，0 跳过**。日志见 `build-codex-agent/agent-phase6-slice7-evidence/`。
- 定向 `test_annotation_session`、`test_agent_sam_tools`、`test_agentpermissions`、`test_toolschema` 均通过；覆盖分页边界、参数类型/范围、原图坐标、maskRle 隔离、polygon 开关、完整响应上限、70 KiB 重复 ID、文件缺失/目录/不可读/超 64 MiB、畸形会话、摘要存在性、Observer/Advisor/Autopilot 只读权限与审计脱敏。
- 增长型随机访问 QIODevice 以 128 KiB 上限触发多次 64 KiB 分块，验证 parser 哨兵和固定预读界限。
- 基线矩阵已重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；新增 `SAM_SESSION_QUERY` 映射到 `get_annotation_session`，GUI `SAM_SESSION_OPEN` 仍明确标注为未支持的 GUI 打开动作；483/483 个测试与证据引用均解析通过。
- `clang-format --dry-run --Werror`、`git diff --check` 干净；独立最终审核结论、构建、CTest、矩阵生成日志及 SHA-256 清单见 Slice 7 evidence 目录。

### 边界与后续

该工具只提供指定会话的只读摘要，不修改标签、不删除对象、不编辑或合并掩膜、不导出数据，也不表示图片已加载或 SAM 服务可用。上述 SAM 对象操作、导出以及 ROI/测量和人机采点仍需各自契约、实现、复审和验证；阶段 6 继续 `IN_PROGRESS`。

## Slice 8：导出既有 SAM 标注会话

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` 两轮独立契约审核 **CONTRACT_PASS**。实现经独立复审 **PASS**；复审先发现临时文件名替换、Windows 父目录重解析及 FIFO 阻塞风险。整改后改用 0700 私有暂存目录、Windows 已核验父目录句柄相对的 `FILE_LINK_INFO` no-replace 发布、Unix `O_NONBLOCK` 加 `fstat` 普通文件验证；契约明确恶意同 UID 进程不在暂存项防篡改保证范围内。

新增危险工具 `export_annotation_session({sessionPath,format,outputPath,categories?})`，只读取用户明确指定的既有会话，复用现有 LabelMe、YOLO Seg、COCO exporter 生成格式内容，再原子创建用户指定的新目标。目标已存在、会话/目标父目录身份变化、源或目标为 symlink、类别不合约、格式/扩展名不一致及内容超界均 fail closed；Observer 拒绝，Advisor 与 Autopilot 均须逐项确认。拒绝、过期或提交失败不写最终目标；成功 `undoable=false`，不修改会话、原图或 Project，也不返回导出正文或 maskRle。`SAM_EXPORT` 已映射到该工具。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 CTest **92/92 通过，0 失败，0 跳过**，日志见 `build-codex-agent/agent-phase6-slice8-evidence/`。
- 定向 `test_agent_sam_tools`、`test_toolschema`、`test_agentcontroller` **3/3 通过**。覆盖三种输出与既有 exporter 字节一致、类别顺序及未知 YOLO 标签拒绝、Observer/Advisor/Autopilot 权限和确认、拒绝零副作用、源快照及目标父目录漂移、源和暂存 symlink 拒绝、已占目标及并发占位保全、RLIMIT_FSIZE 注入的暂存写入失败清理、路径长度限制及响应/审计不回传正文或 maskRle。
- 独立复审确认 no-replace 目标发布、父目录身份复核、失败清理和权限入口无阻断。复审证据见 `build-codex-agent/agent-phase6-slice8-evidence/implementation-review.txt`。
- 基线矩阵重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；`SAM_EXPORT` 映射为 `export_annotation_session`，483/483 个 `testId/evidencePath` 引用解析通过。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；SHA-256 和二进制清单见 Slice 8 evidence 目录。

### 边界与后续

本机验证基于 Linux、Qt 5.15.3 和本地文件系统；Windows 发布分支未在本机执行，FIFO 替换竞态与单独的 flush 故障未独立模拟。实现以非阻塞打开和失败清理路径处理相关故障。契约仅保证暂存目录隔离其他 OS 用户，不覆盖恶意同 UID 进程篡改暂存项；跨进程最终目标竞争仍由原子 no-replace 保证不覆盖。该切片仅增加现有会话导出入口，不实现 SAM 对象编辑、掩膜操作或 GUI 会话打开；阶段 6 仍为 `IN_PROGRESS`。

## Slice 9：创建改标签/删对象后的新会话副本

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**；实现独立复审 **PASS**。复审指出确认快照初版保存原始标签而执行会 trim；现已将快照、确认预览和实际写入统一为 trim 后标签，最终复审仍为 PASS。

新增 Agent 专属危险工具 `transform_annotation_session`，支持 `set_label` 和 `delete_objects`。调用者提供精确对象 ID 与新输出路径；工具先严格读取并验证源会话，再直接修改原始 JSON 树中的目标字段或对象列表，保留所有未修改的已知字段、掩膜数据和未知扩展字段，最终原子写入一个全新 `.deeplux-anno.json` 文件。源文件、原图和 Project 保持不变；Observer 拒绝，Advisor/Autopilot 均需确认，成功审计 `undoable=false`。该能力不映射 GUI 临时类别列表或对话框内存对象删除，也不自动打开新副本。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 CTest **92/92 通过，0 失败，0 跳过**。日志见 `build-codex-agent/agent-phase6-slice9-evidence/`。
- 定向 `test_agent_sam_tools`、`test_toolschema`、`test_agentcontroller` **3/3 通过**。覆盖标签规范化、对象删除、未知根/对象/prompt 扩展字段保留、maskRle 与源文件保留、重复/未知 ID 拒绝、源快照过期、Observer 拒绝、Advisor 批准/拒绝、Autopilot 确认拒绝、确认后目标被占用时字节不变、审计不含对象正文。
- 能力矩阵维持 69 插件（66 模块 + 3 相机）和 121 项 GUI 操作；`SAM_CATEGORY_MGMT` 继续明确标为未支持，备注解释 GUI 临时类别面板与 Agent 新会话副本转换不等价。483/483 `testId/evidencePath` 引用校验通过。
- 独立契约与实现审核、构建/回归日志、格式检查及 SHA-256/二进制清单见 Slice 9 evidence 目录。

### 边界与后续

工具只生成新的会话文件，不改变 GUI 内存会话、类别面板或撤销栈。阶段 6 尚有 ROI/测量与用户采点、GUI 内存对象/提示操作、MaskOps 膨胀/腐蚀/擦除/合并/拆分、视图与设置、登录/模型环境交接等工作；阶段 6 继续 `IN_PROGRESS`。

## Slice 10：通过 Agent 选择流程节点并显示参数页

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。实现复核发现矩阵映射未更新后，已通过仓库矩阵生成器修复；最终独立实现复审 **PASS，无阻断问题**。

新增只读 `select_flow_module({instanceId})`。在 GUI owner thread 中使用当前工程唯一实例 ID，复用 `MainWindow::selectModule` 同步流程树、画布和检查器，并切换到参数页；检查器固定在其他模块时返回 `INSPECTOR_PINNED` 且不改变选择。Observer、Advisor、Autopilot 均可通过 Controller 直接调用，不需确认。成功响应包含 `paramsTabVisible:true`。审计参数白名单仅保留 `instanceId`，额外未声明字段不落日志。`PTREE_SELECT` 与 `PTREE_DBLCLICK_INSPECTOR` 均映射到该工具；不绕过固定检查器状态。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 退出码 0。
- 全量 CTest **92/92 通过，0 失败，0 跳过**；定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。
- 新增 GUI 测试覆盖树/画布/检查器同步、参数页显示、固定状态拒绝、缺失节点、Project JSON/Undo/运行态不变；Controller 入口按 Observer/Advisor/Autopilot 调用并确认 `private=secret` 不进入审计日志。
- 能力矩阵已重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；流程树两项映射均为 `select_flow_module`，483/483 `testId/evidencePath` 引用校验通过。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建、CTest、矩阵输出、独立复审结论及哈希清单归档于 `build-codex-agent/agent-phase6-slice10-evidence/`。

### 边界与后续

此工具只导航 GUI 并显示当前节点参数页，不编辑参数、不返回节点输出、不启动流程。Agent 仍不能通过它打开高级配置对话框或多个应用窗口。阶段 6 继续 `IN_PROGRESS`。

## Slice 11：通过 Agent 设置主工作区面板显示状态

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。独立实现复核第一轮发现窄窗下 QAction 已是 unchecked 时显式隐藏意图未保存、以及不可用面板错误码不一致；两项均修复并补回归，最终实现复核 **PASS，无阻断问题**。

新增 GUI-only 工具 `set_workspace_panel_visibility({panel,visible})`，支持 `tools`、`process`、`bottom` 三个主面板。使用显式 set，三种权限均无需确认；不要求 Project，也不写 Project 内容。返回 QAction 期望状态、实际有效可见状态与焦点模式抑制标志。焦点模式隐藏面板时保留期望状态并在退出后恢复；工具面板显式隐藏会同步记录用户关闭意图，窗口变宽后不被响应式布局自动重开。调用不直接写用户设置，关窗仍走原有布局保存路径。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 CTest **92/92 通过，0 失败，0 跳过**。
- 定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。新增覆盖无 Project、Observer/Advisor/Autopilot、幂等 set、审计额外字段脱敏、焦点模式期望/实际可见分离、窄窗口响应式隐藏后显式关闭再拉宽不重开，以及 headless `REQUIRES_GUI`。
- 能力矩阵重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；3 个 `VIEW_TOGGLE_*` 操作映射到 `set_workspace_panel_visibility`；483/483 `testId/evidencePath` 引用校验通过。
- 格式与 `git diff --check` 通过。独立复审、构建、定向/全量测试、矩阵和 SHA-256/二进制清单见 `build-codex-agent/agent-phase6-slice11-evidence/`。

### 边界与后续

该工具只控制三个主工作区面板。焦点模式可暂时抑制实际显示，其他窗口遮挡也不在工具保证范围；它不控制快速模式、主题、3D 渲染、检查器、视口或系统设置。阶段 6 继续 `IN_PROGRESS`。

## Slice 12：通过 Agent 设置运行日志级别筛选

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；实现独立复核 **PASS，无阻断问题**。

新增 GUI-only 工具 `set_log_level_filter({minimumLevel})`，支持 `all`、`debug`、`info`、`warning`、`error`。只调用当前 `MainWindow` 现有筛选逻辑，不清空日志或改变 Logger。`visibleRows` 是筛选后 `m_logTable` 中未隐藏行数。`SUCCESS` 按 `Info` 过滤；未来低于当时阈值的日志不会插入表格，所以调低阈值不会补回，但 Logger 记录不受影响。`LOG_FILTER` 映射到此工具。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 CTest **92/92 通过，0 失败，0 跳过**。
- 定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。覆盖五级过滤、已有行恢复显隐、SUCCESS=Info、过滤期间未来低级别日志不插入/不补回、三种权限、审计脱敏、无 Project、headless `REQUIRES_GUI` 和 Logger 记录不被工具自身改写。
- 矩阵重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；`LOG_FILTER` 映射到 `set_log_level_filter`，483/483 `testId/evidencePath` 引用通过。
- 格式门禁与 `git diff --check` 通过；审查、构建、定向/全量测试、矩阵及摘要清单见 `build-codex-agent/agent-phase6-slice12-evidence/`。

### 边界与后续

该工具只筛当前窗口的运行日志表，不筛 Agent 日志、终端或磁盘文件；低于阈值的新消息不在表格缓存，之后调低阈值不会重新读取 Logger 历史。阶段 6 仍为 `IN_PROGRESS`。

## Slice 13：通过 Agent 选择底部面板页签

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；实现独立复核 **PASS，无阻断问题**。复审要求补齐非法参数、目标页缺失和跨线程负例，现均有回归覆盖。

新增 GUI-only 工具 `select_log_panel_tab({tab})`，支持运行日志 `runtime`、Agent 操作日志 `agent` 和终端 `terminal`。实现按目标 QWidget 身份查找当前 tab 索引并复用 `setCurrentWidget`；不硬编码序号或本地化标签。不包括 Agent 对话页，也不自动打开底部 dock；响应报告当前索引与真实 dock 可见状态。`LOG_TABS` 已映射到此工具。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 CTest **92/92 通过，0 失败，0 跳过**。
- 定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。覆盖三页切换、三种权限、无 Project、dock 隐藏时切页但不展开、Agent 对话页排除、审计脱敏、非法 tab 保持选择、目标页移除返回 `GUI_UNAVAILABLE`、跨线程返回 `THREAD_AFFINITY` 且无选择副作用、headless `REQUIRES_GUI`。
- 矩阵重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；`LOG_TABS` 映射到 `select_log_panel_tab`，483/483 `testId/evidencePath` 引用有效。
- 格式门禁、`git diff --check`、SHA-256 与二进制摘要校验通过；证据见 `build-codex-agent/agent-phase6-slice13-evidence/`。首次全量运行中 PLCWrite 集成项短暂失败，独立复跑与最终全量复跑均通过。

### 边界与后续

该工具只切换三种底部页签；不会切换 Agent 对话页、打开底部 dock、执行终端命令或打开磁盘日志文件。阶段 6 仍为 `IN_PROGRESS`。

## Slice 14：通过 Agent 将对话滚动到最新消息

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；独立实现复核 **PASS，无阻断问题**。复审发现可独立销毁的 QWidget 裸指针存在潜在 UAF；相关面板及对话容器句柄已改为 `QPointer`，消息、预览和主题回调对已销毁滚动区安全返回。

新增无参数 GUI-only 工具 `scroll_agent_chat_to_latest`，复用 AgentChatPanel 统一的滚动到底、恢复跟随尾部和隐藏“回到最新消息”按钮行为。成功响应报告实际滚动条 `scrollValue`、`scrollMaximum` 与 `atLatest:true`；不要求 Project，不发送消息、不切换底部页签或展开 dock。`CHAT_SCROLL_LATEST` 已映射到该工具。控制器保留其既有通用工具调用日志，工具执行本身不另外写 Logger。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 退出码 0；允许本机回环 TCP/Unix socket 的执行环境中，全量 CTest **92/92 通过，0 失败，0 跳过**。受限沙箱内的初次运行有 11 个网络/监听测试因 `listen()` 被系统拒绝，获批环境复跑全部通过。
- 定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。覆盖 Observer/Advisor/Autopilot、无 Project、非底部滚动恢复、后续消息自动跟随、空态、headless、跨线程、删去滚动按钮/滚动区/整个面板后的 `GUI_UNAVAILABLE`，以及滚动区销毁后主题/消息/预览回调安全返回。
- 测试确认成功调用不切换底部页签或 dock 显示状态；审计参数为空对象且不记录额外字段；Actor 直接执行不额外写 Logger。
- 能力矩阵重生成：69 插件（66 模块 + 3 相机）、121 项 GUI 操作；`CHAT_SCROLL_LATEST` 映射正确，483/483 `testId/evidencePath` 引用通过。格式、`git diff --check`、SHA-256 与二进制摘要校验通过。
- 构建、全量/定向 CTest、矩阵输出、最终独立复审及摘要清单见 `build-codex-agent/agent-phase6-slice14-evidence/`。

### 边界与后续

工具只滚动当前主窗口中已创建的 Agent 对话面板，不自动打开面板或创建窗口。滚动内容容器已销毁时消息、预览和主题回调安全忽略无法显示的对话内容。阶段 6 仍为 `IN_PROGRESS`。

## Slice 15：映射底部面板收起操作

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。本切片不新增工具，只将 `LOG_COLLAPSE` 映射到已有 `set_workspace_panel_visibility`：`panel=bottom, visible=false/true` 分别收起/展开同一底部 dock，避免再造 toggle 工具。矩阵以 GUI 操作 JSON 为唯一数据源重生成。

### 验证

- 定向 GUI 测试 `testAgentWorkspacePanelVisibilityWithoutProjectAndInFocusMode` 通过。覆盖三种权限、无 Project、显式 set 幂等、实际底部收起按钮隐藏 dock 后工具可重新展开、QAction 状态同步以及焦点模式行为。
- 矩阵生成器盘点为 69 插件（66 模块 + 3 相机）、121 项 GUI 操作；`LOG_COLLAPSE` 映射到 `set_workspace_panel_visibility`，483/483 `testId/evidencePath` 引用校验通过。
- `clang-format --dry-run --Werror`、`git diff --check`、摘要哈希验证通过；证据见 `build-codex-agent/agent-phase6-slice15-evidence/`。

### 边界与后续

此映射只覆盖同一个底部 dock 的收起和展开；不控制拖动高度、底部页签、其它 dock 或焦点模式开关。阶段 6 仍为 `IN_PROGRESS`。

## Slice 16：确认后清空运行日志内存

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；最终实现独立复核 **PASS，无阻断问题**。

新增危险 GUI 工具 `clear_runtime_logs`，Observer 拒绝，Advisor/Autopilot 通过标准确认卡逐批批准后才能执行。工具清空 Logger 当前持锁快照和连接的运行日志表；同一锁临界区读取并清除 `Logger` 快照以准确返回清除条目数。磁盘日志文件和独立 Agent 操作历史保留。前置 GUI/线程/表格检查失败时零副作用。运行中并发日志不受全局屏障保护，可能在清理后重新进入内存或表格；聊天循环也可能追加清理之后的标准 AgentLoop bookkeeping。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 CTest **92/92 通过，0 失败，0 跳过**。
- 定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。覆盖权限/确认、日志表和 Logger 一致清理、磁盘文件与 Agent 操作历史保留、并发语义、线程亲和、GUI/Table 不可用零副作用、手动按钮同源清理及聊天循环清理后的新 bookkeeping。
- `LOG_CLEAR` 映射到 `clear_runtime_logs`；能力矩阵重生成后须通过 483/483 证据引用校验。格式门禁、`git diff --check`、相关源码/证据及二进制哈希校验记录于 `build-codex-agent/agent-phase6-slice16-evidence/`。

### 边界与后续

该工具清理整个 Logger 内存快照，无法恢复被清除的内存记录；它不删除磁盘日志，也不清空 Agent 操作历史。并发日志可能在清理后重新出现。阶段 6 仍为 `IN_PROGRESS`。

## Slice 17：查询最近工程列表

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；最终实现独立复审 **PASS，无阻断问题**。

新增无参只读工具 `list_recent_projects`，按 `ProjectManager::recentProjects()` 当前顺序返回路径、文件名及查询时的 `exists`/`isFile` 状态。即使近期条目已失效也不删除或过滤。工具通过 MainWindow GUI owner/thread handler 读取，不要求当前 Project，不加载项目文件，不改当前工程、最近列表或 QSettings。Observer、Advisor、Autopilot 均可直接调用。打开仍单独使用 `open_project`，遵循其既有权限门、未保存策略和候选工程验证；仅在规则要求时确认。

### 验证

- 全量构建成功；全量 CTest **93/93 通过，0 失败，0 跳过**。定向 `test_mainwindow`、`test_agentcontroller`、`test_toolschema`、`test_agent_recent_projects` **4/4 通过**。
- 独立 MRU 测试覆盖两项顺序与 index、普通文件 (exists/isFile=true)、missing 文件保留、目录状态、空数组、三种权限、空审计参数、无 Project、不加载/切换、无 GUI 和线程错误；测试目标在独立进程中清理其测试专属设置。
- 矩阵校验 483/483 单元格引用通过；格式、`git diff --check`、二进制及 SHA-256 校验与独立实现复核通过，证据见 `build-codex-agent/agent-phase6-slice17-evidence/`。
- `FILE_SOLUTION_LIST` 映射到 `list_recent_projects`；生成器校验矩阵的 483 个 `testId/evidencePath` 引用。

### 边界与后续

工具只报告内存中的最近工程列表，不扫描文件系统、不解析工程 JSON；文件状态可能在响应后改变。阶段 6 继续 `IN_PROGRESS`。

## Slice 18：映射快捷运行入口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；最终实现/映射复核 **PASS，无阻断问题**。

GUI 操作 `VIEW_QUICK_MODE` 实际是一次运行当前流程的快捷菜单动作，不是持久模式切换。能力清单将描述改为“快捷运行当前流程一次”，映射到已有 `run_flow({mode:"once"})`；Agent 端继续使用现有执行入口权限门/确认、Project/Engine 检查和异步 runId 结果查询，不模拟同步阻塞或 GUI 高亮清理。未新增产品代码或工具。

### 验证

- 定向 `test_toolschema`、`test_agentpermissions`、`test_agente2e` 覆盖 run_flow 默认/显式 once、Observer 拒绝及 Advisor/Autopilot 确认与实际运行；**3/3 通过**。
- 矩阵重生成：69 插件、121 项 GUI 操作，`VIEW_QUICK_MODE` 映射到 `run_flow`；483/483 证据引用通过。
- Slice 17 完成后的全量构建与 CTest **93/93** 继续适用于本次无产品代码改动；映射、定向结果、复审、格式/差异与哈希证据见 `build-codex-agent/agent-phase6-slice18-evidence/`。

### 边界与后续

快捷运行入口只启动当前流程一次；Agent 用异步 RunEngine API 并返回 runId，不复现 GUI 同步执行期间的阻塞/高亮状态。阶段 6 仍为 `IN_PROGRESS`。

## Slice 19：显式设置深浅主题

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；最终实现独立复核 **PASS，无阻断问题**。

新增 GUI-only 工具 `set_theme({theme:"light"|"dark"})`，设置显式目标而非反转状态。Observer 拒绝，Advisor 使用标准逐批确认，Autopilot 直接执行。操作在当前 MainWindow owner thread 复用 `applyTheme()`，不要求 Project；已初始化配置时持久化 `appearance.darkTheme`。保存失败时界面和内存值保持已应用状态，返回 `CONFIG_SAVE_FAILED` 与 `applied:true,persisted:false`。`ConfigManager::save()` 改用 `QSaveFile` 完整写入和 commit，并解析符号链接目标以保持既有目标写入语义；失败不清除 modified 状态或发出 `configSaved`。`VIEW_TOGGLE_THEME` 映射到此工具，人工菜单仍是反转切换。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功；全量 CTest **94/94 通过，0 失败，0 跳过**。
- 新增隔离用例覆盖 schema 枚举及 non-dangerous、Observer 拒绝、Advisor 批准前零执行、Autopilot 直接执行、显式幂等、未初始化配置、成功保存 JSON、符号链接目标更新且链接保留、确定性保存失败及主题/内存 partial effect、审计参数白名单、无 GUI 与跨线程拒绝。
- 能力矩阵映射 `VIEW_TOGGLE_THEME → set_theme`；生成器审计为 69 插件、121 项 GUI 操作，483/483 `testId/evidencePath` 引用有效。
- `clang-format --dry-run --Werror`、`git diff --check` 与独立实现复核通过；构建、定向/全量测试、矩阵、格式/差异和摘要证据位于 `build-codex-agent/agent-phase6-slice19-evidence/`。

### 边界与后续

无 GUI 时主题工具返回 `REQUIRES_GUI`；ConfigManager 未初始化时只影响当前窗口生命周期。阶段 6 继续 `IN_PROGRESS`。

## Slice 20：设置检查器固定状态

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；最终实现复核 **PASS，无阻断问题**。

新增 GUI view-state 工具 `set_inspector_pinned({pinned:boolean})`，通过 `ModuleInspectorPanel::setPinned` 显式固定/取消固定检查器。Observer、Advisor、Autopilot 均可直接调用，不要求当前 Project 或选中模块。handler 严格验证 JSON boolean；缺参返回 `MISSING_PARAM`，null/number/string 返回 `INVALID_PARAM`，不将错误类型隐式转换为 false。MainWindow 改用 `QPointer` 跟踪检查器；panel 在窗口存活期间销毁时安全返回 `GUI_UNAVAILABLE`。审计只保留规范化 `pinned`。`INSP_PIN` 映射到此工具，人工图钉仍保留 toggle。

### 验证

- 独立 `test_agent_inspector_pin` 覆盖 schema、三种权限直执、显式 true/false 与幂等、缺参/null/number/string 零副作用、无 Project、pin 按钮和 panel 状态同步、audit 白名单、无 GUI/跨线程，以及窗口存活但 panel 销毁时 `GUI_UNAVAILABLE`。
- 全量构建成功；全量 CTest **95/95 通过，0 失败，0 跳过**。
- 能力矩阵将 `INSP_PIN` 映射到 `set_inspector_pinned`；69 插件、121 GUI 操作，483/483 `testId/evidencePath` 引用有效。
- `clang-format --dry-run --Werror`、`git diff --check`、源码/文档/二进制摘要校验与独立复核通过；证据见 `build-codex-agent/agent-phase6-slice20-evidence/`。

### 边界与后续

pin 状态沿用主窗口关闭时的既有 QSettings 保存；工具不选择模块、不展开或关闭检查器，也不更改参数或工程数据。阶段 6 继续 `IN_PROGRESS`。

## Slice 21：映射全局变量管理入口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。本切片为文档/能力清单映射，不改产品代码：`PARAM_GLOBAL_VAR` 映射到 Slice 2 已实现的五个查询/CRUD 工具组合；现有每项工具的权限、类型、确认/审计及运行期锁语义保持不变。明确不模拟打开 GlobalVarView，也不覆盖排序、表达式执行和手动应用，后三项继续由 `GV_MOVE`、`GV_EXECUTE`、`GV_APPLY` 单独记录。

### 验证

- Slice 2 已有五个原子工具和类型化 CRUD 测试继续是本映射的行为依据；Slice 20 完成后的全量构建与 CTest **95/95** 继续适用，因为本切片未修改产品代码或测试。
- 矩阵由唯一 GUI 操作清单重生成：69 插件、121 项 GUI 操作，483/483 引用有效；`PARAM_GLOBAL_VAR` 的组合和未覆盖动作已在能力清单与用户手册标明。
- 独立契约复核通过；格式/`git diff --check` 与哈希记录见 `build-codex-agent/agent-phase6-slice21-evidence/`。

### 边界与后续

此映射只说明 Agent 已有原子工具如何承担全局变量查询和 CRUD；不表示全局变量随工程持久化，也不把表达式求值或 UI 排序视为已支持。阶段 6 继续 `IN_PROGRESS`。

## Slice 22：映射知识库管理对话框入口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。`TOOL_KNOWLEDGE_BASE` 打开模态 `KnowledgeBaseDialog`，映射为“人机协作”且 `currentTool:null`：用户须在 GUI 手动打开。现有 `list_knowledge_documents`、`preview_knowledge_document`、`add_knowledge_document`、`remove_knowledge_document` 仍可直接管理内容，但不等价于打开/聚焦窗口。

### 验证

- 源码确认 AgentSettingsDialog 的知识库按钮执行 `KnowledgeBaseDialog::exec()`；Agent Schema 只注册文档列表、预览、导入和移除工具，没有对话框导航工具。
- Slice 1 的知识库工具、权限与内容安全测试继续作为直接文档管理能力依据；本切片只更正能力映射，不改产品代码。
- 矩阵生成器 69 插件、121 项 GUI 操作，483/483 引用有效；`git diff --check`、SHA-256 和独立复核通过，证据见 `build-codex-agent/agent-phase6-slice22-evidence/`。

### 边界与后续

Agent 无法自动打开或聚焦模态知识库窗口；要在 GUI 中查看该窗口时由用户手动打开。文档 CRUD 仍按 Slice 1 的确认及只读边界执行。阶段 6 继续 `IN_PROGRESS`。

## Slice 23：控制主图像视口缩放

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。新增只读权限门下可直接执行的 `set_primary_viewport_zoom(action)`，支持 `zoom_in`、`zoom_out`、`fit`、`actual`。仅控制 MainWindow 主数据源的 Auto2D 图像视口；执行前验证 `Auto2D`、图像控件存在且含图像。Auto3D 即使保留双数据缓存图像也先返回 `NO_IMAGE`，不调用缩放 API。

### 验证

- 定向测试覆盖 schema、三种权限直执、缩放倍率、fit、1:1、无工程运行、专用视口隔离、Auto3D 缓存图像拒绝且倍率不变、缺参/非法值、无 GUI、线程亲和性、无主视口和审计参数白名单。
- 全量构建：`cmake --build build-codex-agent -j2`，退出码 0；获批 loopback 环境全量 CTest **96/96 通过，0 失败，0 跳过**。受限沙箱内 85/96，11 项依赖本地网络/socket/SAM 服务的测试失败；同一套件在获批环境复跑均通过。
- 定向：`test_agent_viewport_zoom` **5/5 通过**；clang-format、`git diff --check` 和矩阵 483/483 引用校验通过。
- 独立契约复核 **CONTRACT_PASS**、独立实现复核 **PASS**；实现复核确认 handler 在调用缩放 API 前拒绝 Auto3D。测试断言错误码和倍率不变，但 API 在 Auto3D 下本身也是 no-op，因此“未调用 API”由源码顺序复核支撑，不由该断言单独区分。构建、全量 CTest、定向结果、源码/二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice23-evidence/`。

### 边界与后续

不控制专用数据源视口、3D 点大小/渲染模式、平移、ROI 或标注。阶段 6 继续 `IN_PROGRESS`。

## Slice 24：设置主 3D 点云点大小

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。新增只读权限门下可直接执行的 `set_primary_viewport_point_size(size)`。范围 1.0–8.0、最多一位小数，0.5 仅是 GUI 步进；成功值同步到主视口 SpinBox 与 renderer。非 Auto3D 返回 `NO_POINT_CLOUD_VIEW`，专用视口不受影响。

### 验证

- 定向测试覆盖 schema、三种权限直执、1.0/8.0 边界、3.7、拒绝 3.14、SpinBox/renderer/响应/审计同步、非法类型和越界、非 3D、无 GUI、线程亲和性、无主视口及专用视口隔离。
- 全量构建：`cmake --build build-codex-agent -j2`，退出码 0；获批 loopback 环境全量 CTest **97/97 通过，0 失败，0 跳过**。定向 `test_agent_viewport_point_size` **5/5 通过**；格式、`git diff --check`、矩阵 483/483 引用校验通过。
- 独立实现复核 **PASS**；全量构建/CTest、定向测试、格式、矩阵引用、独立复核及源码/二进制摘要证据见 `build-codex-agent/agent-phase6-slice24-evidence/`。

### 边界与后续

只调整主 3D 点云显示大小，不改变点坐标或点云数据，也不作用于专用视口。阶段 6 继续 `IN_PROGRESS`。

## Slice 25：切换主视口 2D/3D 显示模式

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；独立实现复核 **PASS**。新增 `set_primary_viewport_display_mode(mode)`，以显式目标设置主视口模式。`ViewportWidget::setDisplayMode` 是 GUI 按钮和 Agent 共享的切换逻辑：同模式在当前数据有效时幂等且不刷新；真正切换要求同一视口同时有非空图像与非空点云，转回 2D 时恢复图像并恰好发出一次 `imageDisplayed`。失败按缺失内容返回 `NO_IMAGE` / `NO_POINT_CLOUD`，不改变模式或信号计数。

### 验证

- 定向 `test_agent_viewport_display_mode` **6/6 通过**，覆盖 schema、三种权限、显式双向切换、同模式 `changed:false`、GUI QAction 共用路径、图像恢复与信号次数、单缓存/无效双缓存、非法参数、无 GUI/线程/主视口及专用视口隔离。
- 全量构建退出码 0；获批 loopback 环境全量 CTest **98/98 通过，0 失败，0 跳过**。clang-format、`git diff --check` 和矩阵 483/483 引用校验通过。
- 构建、定向/全量日志、独立复核、二进制清单和 SHA-256 见 `build-codex-agent/agent-phase6-slice25-evidence/`。

### 边界与后续

只切换主视口已有缓存内容，不加载文件、不更改工程或数据源，也不影响专用视口。阶段 6 继续 `IN_PROGRESS`。

## Slice 26：设置 3D 点云渲染模式

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；独立实现复核 **PASS**。新增只读视图状态工具 `set_point_cloud_render_mode(mode)`，mode 为 `uniform/rgb/height/intensity/normal/blinn_phong`。工具复用现有“视图”菜单 QAction 和 `onRenderModeChanged()`；只有当前点云菜单可见且启用的模式可用。模式应用到主窗口管理的全部数据视口，并保存在尚未创建 3D 控件的双数据 2D 视口中，之后创建时应用。重复请求会检查所有受管理视口；若后来新增视口造成目标模式不一致，仍触发共享 QAction 路径同步。只改变渲染状态，不修改点云、工程或专用视口。Observer、Advisor、Autopilot 均可直执。

### 验证

- 全量构建：`cmake --build build-codex-agent -j2`，退出码 0。
- 定向 `test_agent_viewport_render_mode` 通过；覆盖 schema enum、三种权限、菜单动作同步、数据不支持模式零副作用、无 GUI/无点云/跨线程/非法参数、Auto2D 懒创建控件继承非默认 RGB，以及选 RGB 后新建默认 BlinnPhong 视口再重复请求 RGB 时全体视口同步。
- 获准的本机 loopback 环境全量 CTest **99/99 通过，0 失败，0 跳过**。受限沙箱中的网络监听测试失败仅是环境拒绝 listener；最终全量结果以 loopback 日志为准。
- 独立复审首轮指出重复选择当前 QAction 会漏同步晚创建视口及懒创建测试只覆盖默认值；两项均已修复并回归，复审最终 **PASS，无剩余阻断**。
- `clang-format --dry-run --Werror`、`git diff --check`、矩阵 483/483 证据引用校验通过。构建、定向/全量 CTest、矩阵生成、摘要与 reviewer 记录见 `build-codex-agent/agent-phase6-slice26-evidence/`。

### 边界与后续

可用模式按主视图聚合点云和既有菜单显示规则判断；显示设置作用于全部受管理数据视口。未进行真实 GPU/不同平台视觉输出验收。阶段 6 继续 `IN_PROGRESS`。

## Slice 27：在专用视口显示数据源

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；独立实现复核代码审查未发现阻断，补齐本验收记录后完成最终复核。新增 dangerous 工具 `display_data_source_in_dedicated_viewport(dataSourceId)`，通过确认后加载/复用缓存，在数据源专用视口显示；重复调用复用同一 viewportId。DataSourcePanel 与 Agent 共用 `displayDataSourceInDedicatedViewportById()`。冷缓存加载只调用已有图像/PLY 解码器，不重建主视图、不改变主视口内容/模式、不修改工程或源文件。隐藏数据源拒绝且不自动取消隐藏。确认绑定 Project runtime identity、数据源集合代次和 GUI owner；确认拒绝、工程集合变化或 GUI owner 变化均无加载/视口副作用。

### 验证

- 定向 `test_agent_datasource_dedicated_viewport` 与 `test_agent_viewport_render_mode` **2/2 通过**；专用视口测试覆盖冷缓存图像/PLY、主视图不变、重复调用复用、Advisor 批准/拒绝、Autopilot 危险操作确认/拒绝、Observer 拒绝、工程代次与 GUI owner 确认过期、损坏图像、隐藏数据源、重复 ID、无工程、无 GUI、线程亲和性及参数错误。
- 全量构建 `cmake --build build-codex-agent -j2` 退出码 0；获准 loopback 环境全量 CTest **100/100 通过，0 失败，0 跳过**。受限沙箱拒绝 listener 的限制与前述阶段一致，最终证据采用 loopback 执行日志。
- 矩阵重新生成，69 插件（66 模块+3 相机）、121 项 GUI 操作；483/483 单元格 `testId/evidencePath` 引用校验通过；格式门禁与 `git diff --check` 通过。
- 构建、定向/全量日志、矩阵生成、独立复核 **PASS** 结论、源码与二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice27-evidence/`。

### 边界与后续

专用视口按每个数据源最多一个来复用；不支持任意文件浏览、目录扫描、数据源显隐修改或工程引用修改。尚未验证真实硬件/大文件现场性能。阶段 6 继续 `IN_PROGRESS`。

## Slice 28：检查器折叠与显隐

契约：本地逐切片契约（未纳入提交），经 `/root/slice19_contract_review` **CONTRACT_PASS**；实现独立复核 **PASS**，测试补齐真实运行时模块的清空/恢复断言后确认 PASS。

将复合 `INSP_COLLAPSE_CLOSE` 拆为 `INSP_COLLAPSE` 与 `INSP_VISIBILITY`，分别映射到 `set_inspector_collapsed(collapsed)` 和 `set_inspector_visibility(visible)`。两项均为无需工程、无需确认的只读 UI 状态工具，Observer、Advisor、Autopilot 可执行。折叠复用检查器和 splitter 的既有状态路径；关闭按钮与 Agent 共用 `MainWindow::setInspectorVisibility()`。关闭清空检查器当前内容，但保留 pin、当前模块 ID、流程树和画布选择；打开后重新绑定当前运行时模块并恢复检查器。严格校验 boolean 参数、GUI 可用性与线程亲和性，审计只记录目标状态。

### 验证

- 最终全量构建 `cmake --build build-codex-agent -j2` 成功；定向 `test_agent_inspector_pin` **6/6 通过**。
- 获准本机 loopback 环境全量 CTest **100/100 通过**。受限环境首轮为 89/100；11 个监听型套件仅因沙箱拒绝本地 TCP/Unix socket 绑定失败，loopback 复跑全部通过。
- 覆盖三类权限、无工程/无 GUI/检查器缺失、线程错误、缺参/错误类型、折叠尺寸与幂等、GUI 折叠及关闭入口、两种关闭/重开路径、固定状态、模块内容恢复、树与画布选择保持和审计白名单。
- 能力矩阵重新生成：69 插件（66 模块 + 3 相机）、122 项 GUI 操作，483/483 单元格的 `testId/evidencePath` 引用校验通过；格式检查与 `git diff --check` 通过。
- 构建、定向/全量日志、矩阵生成、独立实现复核及 SHA-256 摘要见 `build-codex-agent/agent-phase6-slice28-evidence/`。

### 边界与后续

本切片只控制检查器折叠与开关，不执行检查器内的参数、运行、标注或高级配置操作；阶段 6 继续 `IN_PROGRESS`。

## Slice 29：按 ID 关闭视口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，独立实现复核 **PASS**。新增非危险工具 `close_viewport(viewportId)`，按当前 MainWindow 中精确的视口 ID 操作，不猜焦点或标题。GUI 的关闭按钮与 Agent 共用 `MainWindow::closeViewportById()`。关闭前先解析并固定 lazy primary ID：主视口或唯一视口只清空并保留；附加视口销毁。销毁专用数据源视口时清理对应 ID 映射；工程数据、数据源引用、缓存和源文件保留。无工程也可执行，三种权限可直执。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功；定向 `test_agent_viewport_render_mode` 与 `test_agent_datasource_dedicated_viewport` **2/2 通过**。
- loopback 环境全量 CTest **100/100 通过**。
- 覆盖主视口身份尚未初始化时仍保留、附加及专用视口销毁后重新创建获得新 ID、GUI 按钮共享路径、单视口清空保留、工程 JSON 不变、三种权限、重复/未知 ID、缺参/空白/非字符串、无 GUI 与线程错误。
- 能力矩阵 69 插件、122 项 GUI 操作；483/483 `testId/evidencePath` 引用有效。格式门禁、`git diff --check` 和 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice29-evidence/`。

### 边界与后续

该工具只关闭视口，不保存快照或修改工程数据。阶段 6 仍为 `IN_PROGRESS`。

## Slice 30：复制数据源路径到剪贴板

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，独立实现复核 **PASS**。新增危险工具 `copy_data_source_path_to_clipboard(dataSourceId)`，与 DataSourcePanel 共用 `MainWindow::copyDataSourcePathById()`。严格按唯一数据源 ID 取路径；确认卡展示数据源和完整路径，并披露会替换系统剪贴板。缺路径、重复 ID、无工程或 GUI/线程不可用时拒绝且不写剪贴板。确认绑定 Project runtime identity、数据源集合代次及 GUI owner；数据源集合变化或窗口替换会使批准过期。

### 验证

- 全量构建成功；定向 `test_toolschema` 与 `test_agent_datasource_dedicated_viewport` **2/2 通过**；loopback 环境全量 CTest **100/100 通过**。
- 覆盖 dangerous Schema、Observer 拒绝、Advisor 确认拒绝/批准、确认卡路径与剪贴板副作用披露、集合代次漂移过期、审批前哨兵剪贴板不变、成功后路径精确写入、空路径/未知/重复 ID/缺参/类型错误 fail-closed、Project JSON 不变。
- 矩阵 69 插件、122 项 GUI 操作，483/483 证据引用有效；格式检查、`git diff --check`、SHA-256 摘要与独立复核记录见 `build-codex-agent/agent-phase6-slice30-evidence/`。

### 边界与后续

此工具会覆盖系统剪贴板，必须按确认卡逐项批准；不读取剪贴板、不访问文件管理器。阶段 6 仍为 `IN_PROGRESS`。

## Slice 31：快速标注入口与视口快照的人机协作

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。能力清单将 `TB_QUICK_ANNOTATE` 与 `VP_SNAPSHOT` 标为“人机协作”，均无直接 Agent 工具。快速标注需由用户打开 `SamAnnotatorDialog`；源码选择有焦点的图像视口，否则选择首个有图像的视口，不保证是主视口。现有 GUI 测试只证明单视口绑定，不声称覆盖多视口焦点优先和回退行为。已有 `sam_predict`、`sam_annotate` 等工具不等价于打开 GUI。

视口快照由用户在目标视口触发，并在原生保存对话框中选择路径、确认覆盖提示；Agent 不伪造文件写入结果。快速测量生命周期已由 Slice 32 补齐；此切片本身仍不执行自动 GUI 点击或文件保存。

### 验证

- 现有 `test_mainwindow` 用例 `testQuickAnnotateOpensSamDialogOnMainViewportImage` 验证单视口下的图像绑定；快照 action 的启用断言由 `testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme` 覆盖。焦点优先/回退规则通过 `MainWindow::onQuickAnnotate` 源码核对，未声称由双视口测试覆盖。
- 能力矩阵重新生成并通过 483/483 证据引用校验；格式、`git diff --check`、定向测试、全量 CTest、独立复审与 SHA-256 记录见 `build-codex-agent/agent-phase6-slice31-evidence/`。

### 边界与后续

本切片只提供快速标注与快照的人机协作指引，不自动点击 GUI、不控制本机文件保存。阶段 6 继续 `IN_PROGRESS`。

## Slice 32：快速测量人机采点生命周期

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；实现独立复审整改后 **PASS**。新增 `list_viewports`、`begin_quick_measurement`、`get_quick_measurement_status`、`cancel_quick_measurement`。Agent 按稳定视口 ID 和 2D/3D 维度创建一次性请求，用户仍需在目标视口手工点选或拖拽；状态接口只返回步骤与终态，不返回测量值。2D 坐标为图像像素，3D 为点云原生坐标，不推断物理单位；3D 矩形面积明确拒绝。

请求绑定 GUI owner、Project runtime identity、精确 viewport identity/content generation、维度及测量类型。Advisor 确认前后复验快照；漂移拒绝执行。等待请求在视口销毁/清空/内容替换/模式切换、工程变化或 owner 销毁时过期；最近终态可在窗口关闭后按 requestId 查询。终态不可变、ID 不复用、其他视口点击不计入请求；GUI 快速测量和 Agent 请求互斥。取消只清理本请求已开始生成的测量覆盖，未采点时保留已有测量覆盖；非矩形取消也保留既有 ROI。

### 验证

- 最终完整构建 `cmake --build build-codex-agent -j2` 退出码 0；定向 `test_mainwindow`、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller`、`test_agentactor` **5/5 套件通过**；loopback 环境全量 CTest **100/100 通过，0 失败，0 跳过**。沙箱内原始全量运行的 11 个本地监听型测试受 socket bind 限制失败，loopback 复跑全部通过。
- 回归覆盖权限/Schema、2D 六类和 3D 支持的五类测量步骤、3D 目标外点击忽略、矩形 ROI、覆盖保留、取消/过期/终态稳定、Advisor 的 viewport content/Project/owner 漂移、owner 销毁后的无 GUI 查询、视口关闭/清空/切换/工程更换及 GUI/Agent 互斥。
- 基线矩阵由脚本重生成：69 插件、123 项 GUI 操作，483/483 `testId/evidencePath` 引用校验通过；`VIEWPORT_LIST`、`TB_QUICK_MEASURE` 与各测量类型操作均映射到对应工具/人机协作组合。格式门禁、`git diff --check`、完整日志、独立复审和哈希清单见 `build-codex-agent/agent-phase6-slice32-evidence/`。

### 边界与后续

用户必须亲自完成 GUI 采点。状态 API 不读取最终数值；用户需查看视口覆盖和运行日志。offscreen CTest 验证信号路由和生命周期，不代替现场 GPU/人机交互可用性验收。阶段 6 仍为 `IN_PROGRESS`。

## Slice 33：活动 SAM 会话掩膜编辑

契约：本地逐切片契约（未纳入提交），实现独立复审 **PASS，无阻断发现**。新增 `list_active_annotation_objects` 与危险工具 `edit_active_annotation_masks`，只操作当前可见 SAM 对话框绑定图像上的内存会话，复用既有 `MaskOps` 和 GUI 撤销栈。支持膨胀、腐蚀、笔画擦除、连通合并、连通分量拆分。Advisor/Autopilot 编辑前确认；确认绑定窗口/对话框、图像像素摘要、完整会话摘要及精确参数。失败或快照漂移零副作用。确认卡、结果及审计隐藏笔画坐标和掩膜数据；用户可在既有 GUI Undo/Redo 中恢复操作。

### 验证

- 全量构建成功；Slice33 定向 CTest 6/6 通过，SAM 对话框算法套件 60/60、MainWindow 确认/隐私/撤销用例通过、控制器图像摘要漂移用例通过。`clang-format --dry-run --Werror`、`git diff --check` 和矩阵 483/483 引用校验通过。
- 回环环境全量 CTest 为 99/100 套件通过；唯一失败套件 `test_mainwindow` 中两个顺序敏感用例在单独运行时均通过：`testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`。因此不宣称全量通过，也未将此结果归因于 Slice33。原始回环及受限沙箱日志见 Slice33 证据目录；受限沙箱另有本地 socket/listener 测试失败。
- 独立复审只检查代码和契约；构建、测试由执行者完成。证据目录含复审结论、测试日志、格式与差异检查、二进制清单及源码/证据 SHA-256。

### 边界与后续

不打开/保存磁盘会话，不修改类别，不自动持久化，不绕过用户确认。响应仅返回操作状态和对象 ID。阶段 6 仍为 `IN_PROGRESS`；SAM 类别管理、提示取消、会话打开/保存继续保持独立状态。

## Slice 34：活动 SAM 类别与对象标签

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；实现独立复审 **PASS，无阻断发现**。新增只读 `list_active_annotation_categories` 与需确认的 `edit_active_annotation_categories`。类别增删仅作用于活动对话框临时类别列表；对象标签可按精确 ID 批量修改，只更新 `label` 字段，通过 GUI Undo/Redo 恢复。操作确认绑定 owner、dialog、类别数组、会话摘要和规范化参数；确认前任一绑定状态漂移均拒绝执行。确认卡和执行使用同一规范化类别名/标签。

### 验证

- 全量构建成功；定向 CTest 6/6 通过；新增类别/标签算法、Schema 和 MainWindow 确认/漂移/权限/Undo 用例通过。完整回环 CTest **99/100 套件通过**；唯一失败套件 `test_mainwindow` 内的 `testAgentInputErrorPathDoesNotCrashOrStayThinking`、`testDarkThemeCloseAndDestroyWindowDirectly` 在单独/相邻定向运行中通过。Slice33 已记录过相同两项顺序敏感失败；不宣称完整 CTest 全通过。
- 类别接口覆盖无图像只读查询、完整类别数组与对象分页、UTF-8 字节长度、类别数上限、重复/未知类别错误、混合标签更新、unchanged 撤销索引不变、多选/当前项保持及撤销/重做字段保留。GUI 集成覆盖规范化值预览、错误 confirmationId、类别/session 漂移、dialog/owner 替换、Observer 只读允许/写入拒绝及磁盘会话字节不变。
- 矩阵重新生成，69 插件、123 GUI 操作，483/483 引用校验通过；格式门禁、全量日志、二进制及 SHA-256 清单见 Slice34 证据目录。独立实现复审 **PASS**。

### 边界与后续

类别列表为对话框临时状态，不持久化；对象标签更新只在内存会话中生效，用户需显式保存。阶段 6 仍为 `IN_PROGRESS`。

## Slice 35：取消活动 SAM 提示

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，实现独立复核 **PASS**。新增只读 `get_active_annotation_prompt_status` 与需逐项确认的危险工具 `cancel_active_annotation_prompt`。取消只清除当前 SAM 对话框未确认的点/框、预测预览和候选，保留已确认对象、类别、选择、对象掩膜与 Undo/Redo 栈。状态、确认载荷和审计不含坐标、掩膜、图像或路径。

取消期间绑定原预测请求 ID；迟到的自动 `invalid_embedding` 重试 `set_image` 成功或失败响应会在改变 embedding 或发信号前丢弃。取消后的重试超时只清理该过期请求；其他并行请求仍获自己的超时窗口。对话框确认快照用单调递增实例 ID，重建对话框后旧确认失效。提示内容 generation 与 Undo/Redo 命令失效 epoch 分离，同一 epoch 内的多个点/框命令仍可依栈序撤销/重做。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功；SAM 对话框 **63/63**、SAM 后端客户端 **26/26**、`test_toolschema` **33/33** 和 MainWindow prompt 取消/重建确认用例 **3/3** 通过。全量 loopback CTest 为 **99/100 套件通过**；唯一失败为 `test_mainwindow` 中的 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`，与 Slice33/34 记录的顺序敏感项相同，二者此前单独运行通过，不归因于 Slice35。
- 新增/扩展测试覆盖迟到重试成功与失败、超时、旧成功/错误预测回调、同一坐标重新提示、dialog 重建身份、权限/确认/隐私、已确认标注与撤销栈保留，以及多点/框 Undo/Redo。重试响应通过测试服务响应记录和 pending-request 状态屏障验证。
- 能力矩阵为 69 插件、123 项 GUI 操作，483/483 证据引用有效；格式门禁与 `git diff --check` 通过。构建、全量/定向测试、矩阵、复审、二进制与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice35-evidence/`。

### 边界与后续

不取消 SAM 服务、模型加载或图像 embedding；不保存/打开会话，不改工程及已确认对象。阶段 6 仍为 `IN_PROGRESS`；活动 SAM 会话打开/保存仍单独未完成。

## Slice 36：保存活动 SAM 会话到新文件

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，独立实现复核 **PASS**。新增危险工具 `save_active_annotation_session(outputPath)`，将当前可见 SAM 对话框的完整内存会话原样保存到新的 `.deeplux-anno.json`。Observer 拒绝；Advisor 与 Autopilot 需要确认。输出只能是用户给出的绝对路径，父目录必须存在，目标不能是文件或符号链接。保存复用 staged write 与原子 no-replace 发布，不覆盖现有文件，也不改活动会话、图像绑定、类别、选择、撤销栈或源文件。

确认快照绑定主窗口 owner、单调 dialog ID、完整会话 JSON SHA-256、图像像素摘要及目标 canonical parent/身份。批准时控制器保存完整快照；写入 handler 在执行边界再与完整批准快照比对，并让发布器再次校验输出目标快照。这样同批次“先改标签、后保存”会在保存处返回 `CONFIRMATION_EXPIRED`，不会把未在原确认快照中的内容写入文件。确认卡与审计不暴露坐标、掩膜或会话正文；成功审计只补充对象数、字节数和 SHA-256，`undoable=false`。

### 验证

- 最终全量构建 `cmake --build build-codex-agent -j2` 成功；`test_mainwindow::testAgentSaveActiveSamSessionAsNewFile` **通过**（QtTest 总计 3 passed / 0 failed，含 init/cleanup），覆盖完整输出字节/摘要、Observer 拒绝、Advisor 确认、Autopilot 拒绝、无 dialog/无图像/无对象、错误/超长路径、owner-thread、目标被抢占、父目录 symlink 改向、目标为普通/悬空 symlink、Linux `/proc` 写失败、审计隐私与 metadata，以及批次先改标签后保存的过期拒绝。
- `test_toolschema`、`test_agentcontroller`、`test_annotation_session` **3/3 CTest suites 通过**；获准 loopback 环境下 `test_samannotatordialog` 与 `test_sambackendclient` **2/2 suites 通过**。
- 完整 loopback CTest **99/100 suites 通过**。唯一失败 suite 为 `test_mainwindow`，其中 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 和 `testDarkThemeCloseAndDestroyWindowDirectly` 顺序执行时失败；本轮将两项分别隔离重跑均通过，Slice36 新用例在完整 suite 中也通过。因此不声称完整 CTest 全通过。
- 能力清单将 `SAM_SESSION_SAVE` 改映射为 `save_active_annotation_session({outputPath})`，用户手册已说明权限、路径、不覆盖和当前会话不变；矩阵 69 插件/123 操作，483/483 `testId/evidencePath` 引用通过校验。格式门禁与 `git diff --check` 通过。
- 构建、定向测试、完整 CTest、格式/矩阵校验、独立复核、二进制清单与源码/证据摘要见 `build-codex-agent/agent-phase6-slice36-evidence/`。

### 边界与后续

该工具只保存当前可见 SAM 窗口中至少含一个已确认对象的活动内存会话到新文件；不打开既有会话，不覆盖/追加既有文件，不自动保存未确认提示。阶段 6 继续 `IN_PROGRESS`；继续处理下一项仍未映射或仅有人机协作的操作。

## Slice 37：打开活动 SAM 会话

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；实现独立复核 **PASS**。新增危险工具 `open_active_annotation_session(sessionPath)`，只替换当前可见 SAM 对话框的内存会话。Observer 拒绝；Advisor/Autopilot 逐项确认。会话 JSON 和绑定图像各单次读取并按同一缓冲区校验；确认绑定窗口 owner、dialog instance、当前会话/图像、源会话文件与绑定图像身份及内容。确认期间发生漂移则 `CONFIRMATION_EXPIRED`，零替换副作用。

成功打开会清除旧的未确认提示、预测预览/候选、选择、掩膜缓存与 GUI Undo/Redo，按候选数据替换会话；不改源会话/图像文件。Agent 后端只使用已验证候选像素生成的固定临时 PNG，源图路径后续变化不会改变其像素。临时图像在 GUI 替换/清空或窗口关闭时自动删除。加载新图会立即失效旧 embedding；新 `set_image` 成功前拒绝预测，失败也不退回旧 embedding。若 pending `set_image` 来自取消的 `invalid_embedding` 重试，打开新会话会 latest-wins 替换它；旧延迟响应不能覆盖新 embedding 或重放预测。

### 验证

- `cmake --build build-codex-agent -j2` 成功。`test_mainwindow::testAgentOpenActiveSamSession` 与 `testAgentSaveActiveSamSessionAsNewFile` 单独运行通过；`test_toolschema` 33/33；`test_sambackendclient` 27/27；SAM 对话框提示取消、重试取消、在途预测会话替换及 cancelled retry 替换用例全部通过（4 个选定用例，含 init/cleanup 共 6 passed）。SAM 假后端回归需 loopback 监听权限；未运行全量 CTest。
- GUI 集成覆盖确认卡隐私/副作用、Observer 拒绝、Autopilot 拒绝、确认图像/会话漂移、缺失会话及图像错误码、空未绑定草稿字段保留、批次先编辑后打开过期但保留前序编辑、错误 confirmationId、后端延迟初始化固定像素、旧预测取消、新 embedding 的预测、清空图像时快照立即释放。
- SAM 后端覆盖新图 `set_image` 延迟成功/失败期间旧 embedding 不可预测；取消 invalid-embedding 重试后成功返回只恢复当前图 embedding、不重放预测，失败时保持无 embedding。集成覆盖 NotStarted 状态下新会话替换 pending retry，旧 retry 延迟响应计时器触发后 embedding 不回滚。
- 能力清单 `SAM_SESSION_OPEN` 映射到 `open_active_annotation_session`；矩阵 69 插件、123 项 GUI 操作，483/483 testId/evidencePath 引用通过。格式门禁、`git diff --check` 和 32/32 源码/测试/文档/二进制及日志 SHA-256 校验通过。定向构建/测试、矩阵、独立复审和摘要见 `build-codex-agent/agent-phase6-slice37-evidence/`。

### 边界与后续

该工具只加载用户明确给出的原生会话，不猜测路径、不迁移或保存源文件、不自动推理。阶段 6 仍为 `IN_PROGRESS`；尚有其他标注和仅人机协作操作待处理。

## Slice 38：打开系统设置窗口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；Slice38 范围内验收 **PASS**，阶段 6 整体仍为 `IN_PROGRESS`。新增只读 GUI 工具 `open_system_settings()`，复用现有 `SystemParamView`，由 MainWindow 持有一个非模态实例。重复调用复用该窗口并请求 show/raise/activate；工具结果只报告显示请求已发出及需要用户操作，不代表窗口管理器已置前或设置已保存。窗口关闭后同步既有主题配置；设置控件的查看、修改、保存与重置仍由用户自行完成。

`TOOL_HARDWARE_CONFIG` 映射到该工具；通用、显示、日志、重置/保存菜单操作保留为“打开设置窗口 + 用户手动操作”的协作路径，不伪称工具可单独选择分区、保存或重置。用户手册已补充行为边界。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功；`test_toolschema` **34/34** 通过；定向 MainWindow 新用例与 owner/线程用例 **4/4 QtTest 计数通过**（含 init/cleanup），覆盖 Observer/Advisor/Autopilot、窗口复用、仅打开时设置值不变、用户保存主题后同步到 MainWindow、关闭重开与 MainWindow 销毁时子对话框释放。无 GUI 路由断言 `REQUIRES_GUI`，线程归属复用既有 handler 测试。
- 基线脚本重生成后为 69 插件（66 模块 + 3 相机）、123 GUI 操作，483/483 证据引用有效。
- 最终全量构建成功；`test_toolschema` 34/34、MainWindow 新用例与 owner/线程用例 4/4 QtTest 计数通过，clang-format 与 `git diff --check` 通过。独立实现复审首轮提出的保存后主题同步回归缺口已补齐、权限状态恢复改为 RAII，最终复审 **PASS**。最新获准 localhost loopback 全量 CTest 结果 98/100，详见 Slice129 evidence；其中 `test_mainwindow` 的两个失败位于范围外的 Agent 输入错误路径和直接深色主题销毁用例，`test_agent_datasource_dedicated_viewport` 的一次 signal 11 在失败项复跑通过。Slice38 契约未要求全仓零失败，这些失败不命中 Slice38 验收；因此只将 Slice38 标记 PASS，不把阶段 6 总体标记 PASS。
- Slice38 原始构建/定向测试日志、矩阵审计、二进制清单、实现复审结论及 SHA-256 清单见 `build-codex-agent/agent-phase6-slice38-evidence/`。

### 边界与后续

该入口只展示通用 `SystemParamView`，不读取/修改设置值，也不打开相机、通信、Agent 或登录对话框；工具成功不代表用户已保存。硬件/设备环境核验仍属于阶段 8。

## Slice 39：测量与 SAM 提示的视图坐标闭环

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，最终实现独立复核 **IMPLEMENTATION_PASS**。将快速测量既有回归由直接发信号改为真实 Qt 鼠标按下/移动/释放事件；用固定尺寸图像、2× 缩放和中键平移，独立预先算出窗口事件坐标。断言多视口测距点 `(13,17)`、`(63,47)`、线段端点及 `58.310` 标签均为图像像素，并验证结束后重复点击不改变终态或覆盖层。`rectangle_area` 的拖框事件验证四条覆盖边对应 `(10,12)` 至 `(30,32)`。

SAM 主视图回归同样改为真实鼠标事件和固定像素期望：缩放/平移后正提示点为 `(80,60)`，拖框的两个端点为 `(40,30)`、`(100,90)`；MainWindow 集成用例再将会话绑定在实际主视口，并点击 DisplayManager 创建的第二视口，确认提示摘要不变。快速测量点击第二视口时也明确断言非目标视口测量点/线覆盖为空。快速测量的换图、显示模式变化、清空、关闭视口、工程切换及 owner 销毁失效路径复用既有 `testAgentQuickMeasurementCancellationExpiryAndTerminalStability` 与相邻回归；SAM 迟到请求和会话替换仍由既有假后端用例覆盖。当前内容若为 CropImage 输出，坐标保持 crop-local 像素，本切片不声称有父图偏移。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功。两个定向 QtTest 用例各 **3 passed / 0 failed**（含 init/cleanup）：`testAgentQuickMeasurementLifecycleAndTargetIsolation`、`mainViewOverlayUsesMainViewCoordinates`。
- 获准 loopback 全量 CTest **99/100 suites 通过**。唯一失败为既有 `test_mainwindow` 的两项范围外断言：`testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`；本切片两个用例在同一 suite 内通过。其余 99 suites 通过，未将全量结果记为 PASS。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建/CTest 日志、独立复审和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice39-evidence/`。

### 边界与后续

本切片只证明当前显示图像像素坐标经缩放/平移交互可闭环，并补充特定两点测量、矩形 ROI 与 SAM 主视图事件证据；不覆盖所有测量类型、裁剪来源偏移、多图像派生链、GPU/显示服务器或阶段 8 真实模型验收。G6 标准 3 仍为部分证据，阶段 6 继续 `IN_PROGRESS`。

## Slice 40：SAM 环境前置条件与用户交接

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；独立实现复核 **IMPLEMENTATION_PASS**。新增只读 GUI 工具 `open_sam_environment_setup()`，首次创建 setup-only SAM 对话框，重复调用复用原对话框及其会话/绑定。工具只回报脱敏的点时快照；不初始化 Python、不导入权重、不启动服务、不请求健康检查或预测。

状态区分外部 Python 路径有效与托管环境已验证、权重配置/文件/格式、服务状态与 `/health` 返回的 `mode`。只有 `Ready + mode=sam` 才报告 `modelLoaded=true`；stub/unknown 不会被当成真实模型。健康检查及服务启动/模型加载/预测进行中优先返回等待动作，不会建议重启或改配置。安全绑定现有图像只更新对话框快照和 overlay，不准备或卸载后端；视口清空、换图、销毁都会同步图像状态和会话摘要。既有手动快速标注入口保留自动准备行为。该工具已列为只读工具，Observer、Advisor、Autopilot 可直接使用。

### 验证

- 最终 `cmake --build build-codex-agent -j2` 成功；`test_toolschema` **35/35**、`test_samannotatordialog` **66/66**、`test_sambackendclient` **27/27** 通过。MainWindow 新增路由/无图像/带图像焦点复用用例与 owner/线程用例 **6/6 QtTest 计数通过**；带图像测试明确验证焦点实际转到第二视口后重复调用仍保留原绑定，并验证清空原视口后状态变为无图像。全量 MainWindow suite 为 **128 passed / 2 failed**，失败为既有 `testAgentInputErrorPathDoesNotCrashOrStayThinking`、`testDarkThemeCloseAndDestroyWindowDirectly`，Slice40 新用例通过。
- 全量获准 loopback CTest **99/100 suites 通过**；唯一失败 suite 为 `test_mainwindow`，仅含上述两个既有失败。其余 99 套件包括 SAM 假健康服务与新操作通过。未将全量回归称为通过。
- 新增状态用例在隔离且原先无配置文件的 app-data 目录验证 ConfigManager 初始未初始化、setup-only 首次及重复构造不创建/改写配置；MainWindow 重复工具调用也逐次比较配置初始化状态、文件存在性及原始字节。覆盖缺失环境、外部 Python 无效及恢复按钮、模型缺失/不支持、服务错误、stub、真实 SAM、health 在途、预测 Busy、图像清空/重载/视口销毁及提示/会话摘要同步。safe attach 和之后换图/清空均不调用后端；保留真实 Qt 对话框 before/after 截图。`git diff --check` 与新增/变更行范围的 clang-format 门禁通过；未声称整个既有大文件均通过全文件格式检查。
- 能力矩阵由审计脚本重生成：69 插件、124 项 GUI 操作；483/483 单元格 `testId/evidencePath` 引用有效。SHA-256 清单 **34/34 校验通过**，另有 7 个构建/测试二进制摘要；完整日志、截图与复审记录归档于 `build-codex-agent/agent-phase6-slice40-evidence/`。

### 边界与后续

fake HTTP 与临时模型文件只证明状态协议/UI 交接，不证明真实模型推理、GPU、安装网络或设备可用。全量获准 loopback CTest **99/100 suites 通过**；唯一失败 suite 为 `test_mainwindow`，只含上述两条既有失败，不计作全量通过。Slice40 完成时 G6 标准 7 仍需登录窗口导航，该入口已由 Slice41 处理；凭据提交仍由用户完成。G6 标准 3 的 CropImage 父图偏移未关闭。Slice40 验收范围通过，阶段 6 保持 `IN_PROGRESS`。

## Slice 41：登录窗口的人机交接

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**；独立实现复核首轮发现关闭后延迟销毁窗口可被误复用及旧实例销毁回调可能清空新实例指针，均已修复并通过复审 **IMPLEMENTATION_PASS**。Slice41 范围验收 **PASS**。新增无参数只读 GUI 工具 `open_user_login_dialog()`，由 Agent 打开/复用登录窗口，用户自行输入并提交。既有菜单模态登录路径保持不变；工具不读取表单、不连接凭据提交信号、不调用 `UserManager::login/logout`，响应及审计不包含凭据，也不声称登录或认证成功。

窗口生命周期覆盖首次创建、重复调用复用、当前菜单模态实例复用、关闭后在 Qt 延迟删除前立即新建、旧实例销毁后仍复用新实例，以及关闭后重开。三种权限级别均可直接调用。使用合成边界字符串检查 Agent 调用不会触发 `loginRequested`；调用前后比较用户状态及代表性权限字段，均未变化。当前 `UserManager::login()` 忽略密码并按用户名本地选择角色仍是既有产品限制，本切片没有修改或验证认证算法。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功。定向 MainWindow 新用例与 owner/线程用例 **5/5 QtTest 计数通过**；`test_toolschema` 登录工具用例 **3/3**、`test_agentpermissions` 权限门用例 **3/3** 通过。包含新回归的完整 MainWindow suite 为 **130 passed / 2 failed**，两项既有失败仍是 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`；Slice41 新用例通过。
- 修复后全量 CTest **99/100 suites 通过**；唯一失败 suite 为上述 `test_mainwindow` 的两项已知范围外失败，不将全量回归称为通过。具体日志在 `build-codex-agent/agent-phase6-slice41-evidence/ctest-full-loopback-final.log`。
- 矩阵重生成至 69 插件、125 项 GUI 操作；483/483 矩阵单元的 `testId/evidencePath` 引用校验通过。格式范围检查、`git diff --check`、27/27 源码与证据 SHA-256 校验及 6/6 相关二进制摘要见 `build-codex-agent/agent-phase6-slice41-evidence/`。

### 边界与后续

`LOGIN_SUBMIT` 仍为用户本人完成人机协作操作；工具不能获得登录表单或认证结果。此 Slice 仅关闭登录窗口导航缺口，不修复当前本地角色选择逻辑，不声称实现密码验证或服务端认证。阶段 6 与 G6 总体验收继续为 `IN_PROGRESS`，尚有 Agent 设置、关于窗口及其他 GUI 人机协作入口待逐项核查。

## Slice 42：Agent 设置窗口的人机交接

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，独立实现复核 **IMPLEMENTATION_PASS**；Slice42 验收 **PASS**。新增无参数只读 GUI 工具 `open_agent_settings_dialog()`，用户可通过现有 Agent 设置界面查看和修改配置。工具只导航窗口，不读取字段、不保存、不测试连接；用户手动 OK 才沿用既有配置持久化及运行时应用路径，Cancel 不产生配置或运行时副作用。已有菜单模态入口保持原行为；若工具窗口与菜单模态窗口同时可见，工具优先复用模态实例。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功。`test_agent_settings_handoff` **4/4 QtTest 计数通过**，覆盖隔离配置目录、打开/复用、三种权限、owner/线程/参数错误、关闭后延迟销毁重开、modeless 与菜单 modal 并存、Cancel 零副作用、手动保存和审计隐私。设置保存后的运行时 client 参数通过仅绑定 `127.0.0.1` 的合成 HTTP fixture 验证；工具打开和取消本身不发送请求。`test_toolschema` **37/37**、`test_agentpermissions` **11/11** 通过。
- 全量 loopback CTest **100/101 suites 通过**，Slice42 独立套件通过。唯一失败 suite 为 `test_mainwindow`，含既有范围外断言 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 和 `testDarkThemeCloseAndDestroyWindowDirectly`（该 suite 130 passed / 2 failed）；不将全量回归记为通过。
- 能力矩阵已加入 `AGENT_SETTINGS_OPEN` 并映射为 `open_agent_settings_dialog()`；69 个插件、126 项 GUI 操作，483/483 矩阵证据引用通过审计。构建、定向测试、全量 CTest、矩阵审计、SHA-256 清单和独立复核记录见 `build-codex-agent/agent-phase6-slice42-evidence/`。`git diff --check` 与本切片变更行格式检查通过。

### 边界与后续

API Key、端点、权限、System Prompt、连接测试和配置保存均仍由用户直接操作；工具成功不代表已保存或连接可用。阶段 6 与 G6 总体验收继续 `IN_PROGRESS`，关于窗口和剩余 GUI 协作操作仍需逐项审计。

## Slice 43：打开关于窗口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，最终独立实现复核 **IMPLEMENTATION_PASS**；Slice43 验收 **PASS**。新增无参数只读工具 `open_about_dialog()`，以非模态窗口导航到固定的 DeepLux 产品说明。帮助菜单仍使用原有 `QMessageBox::about()` 标题、富文本正文和模态行为；若工具调用到达时菜单 About 窗口正显示，工具复用该模态实例，不创建第二个窗口或改变模态性。

### 验证

- 全量构建成功。`test_agent_about_handoff` **5/5 QtTest 计数通过**，覆盖 REQUIRES_GUI、owner/线程/参数、三种权限、打开/复用、延迟销毁竞态、菜单模态嵌套循环复用及 MainWindow 销毁时子窗口销毁；`test_toolschema` **38/38**、`test_agentpermissions` **12/12** 通过。
- 全量 loopback CTest **101/102 suites 通过**。唯一失败 suite 为 `test_mainwindow`，其中既有范围外失败 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`；Slice43 独立 suite 通过，故不将全量回归称为全通过。
- `HELP_ABOUT` 已映射为 `open_about_dialog()`；矩阵审计为 69 插件、126 项 GUI 操作，483/483 证据引用有效。格式/差异检查通过；26/26 源码与证据 SHA-256、7/7 相关二进制摘要校验通过。构建、定向测试、全量 CTest 和独立复核日志归档于 `build-codex-agent/agent-phase6-slice43-evidence/`。

### 边界与后续

关于窗口只展示硬编码产品说明；工具不核验动态版本、许可证、更新、联网或安装状态。阶段 6/G6 继续 `IN_PROGRESS`；知识库管理窗口、路径/原生文件对话框等人机协作操作仍需逐项审计。

## Slice 44：打开知识库管理窗口

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**，最终独立实现复核 **IMPLEMENTATION_PASS**；Slice44 验收 **PASS**。新增只读工具 `open_knowledge_base_dialog()`，导航或复用现有 `KnowledgeBaseDialog`。用户手动添加/移除文档或打开知识库目录；工具响应和审计不返回文档列表、文件名、路径或内容。首次打开会沿用现有 `userKnowledgeDir()` 行为，在 app-data 下初始化空目录。

### 验证

- 全量构建成功。`test_agent_knowledgebase_handoff` **5/5 QtTest 计数通过**，覆盖临时 app-data、空目录初始化、GUI 中显示内置文档但工具结果/审计不泄漏文档信息、用户文档字节/数量不变、GUI/owner/线程/参数、三权限、关闭后延迟销毁重开、设置窗口嵌套 modal 复用及 MainWindow owner 销毁。`test_toolschema` **39/39**、`test_agentpermissions` **13/13**、`test_knowledgebasedialog` **4/4**、`test_agentcontroller` **77/77** 通过。
- 最终源码全量 loopback CTest **102/103 suites 通过**；Slice44 suite 通过。唯一失败仍是 `test_mainwindow` 中既有的范围外断言 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`（该 suite 130 passed / 2 failed）；不声称全量 CTest 通过。
- `TOOL_KNOWLEDGE_BASE` 已映射为 `open_knowledge_base_dialog()`；矩阵 69 个插件、126 项 GUI 操作，483/483 证据引用通过。格式/差异检查通过；30/30 源码与证据 SHA-256、9/9 相关二进制摘要校验通过，完整证据见 `build-codex-agent/agent-phase6-slice44-evidence/`。

### 边界与后续

工具只显示管理 UI，不执行文档添加、删除或外部文件管理器导航。文档操作安全语义继续由已有内容工具和 GUI 操作本身承担。阶段 6/G6 继续 `IN_PROGRESS`；SAM 快速标注 GUI 入口及剩余手动操作仍需处理。

## Slice 45：SAM 快速标注窗口的人机交接

契约：本地逐切片契约（未纳入提交），独立契约审核 **CONTRACT_PASS**，独立实现复核 **IMPLEMENTATION_PASS**；Slice45 验收 **PASS**。新增只读导航工具 `open_sam_annotator_dialog()` 并映射 `TB_QUICK_ANNOTATE`。首次打开创建 setup-only 对话框，通过环境设置安全绑定入口挂接当前焦点图像视口（无焦点时回退到第一个有图像视口）；不准备后端、不初始化 Python、不启动进程、不发送 HTTP、不加载模型或预测。已有窗口及标注会话优先保留；若窗口内已有会话，即使所有主视口无图也继续复用；否则无图返回 `NO_IMAGE`。响应只含 `status`、`dialogAction`、`userActionRequired`、`imageAvailable`。原工具栏的用户快速标注行为保持原样。

生命周期覆盖工具创建和工具栏创建的窗口；`finished` 与 `destroyed` 均按实例身份清理 owner 指针，验证关闭后 Qt 延迟销毁前立即重开会创建新实例，旧实例销毁不会清空新实例引用。

### 验证

- 最终全量构建 `cmake --build build-codex-agent -j2` 成功。定向 CTest **5/5 suites 通过**：`test_agent_sam_annotator_handoff`、`test_toolschema`、`test_agentpermissions`、`test_samannotatordialog`、`test_agent_sam_tools`。Slice45 handoff 的 5 个功能用例均通过（QtTest 含 init/cleanup 共 7 passed）。`test_mainwindow` 内既有 `testQuickAnnotateOpensSamDialogOnMainViewportImage` 也通过。
- 全量 loopback CTest **103/104 suites 通过**。唯一失败 suite `test_mainwindow` 有两项既有失败：`testAgentInputErrorPathDoesNotCrashOrStayThinking`（期望 errorSpy 1、实际 0）和 `testDarkThemeCloseAndDestroyWindowDirectly`（窗口样式与深色主题样式不一致）；其余 103 suites 通过，包含 Slice45 handoff。该结果不记为全量通过。
- 验收验证 Agent 打开前 `ConfigManager` 未初始化且配置文件不存在，打开/复用后条件仍成立；无新 SAM 子进程/后端图像准备，已运行客户端的 `LoadingModel` 状态与待处理请求数不变。响应及完整动作审计不含合成图像路径、像素/标注/id/mask/session-model 标记、SAM modelPath 或伪 API key。测试不访问外网。
- `TB_QUICK_ANNOTATE` 映射为 `open_sam_annotator_dialog()`；基线仍为 69 plugins、126 GUI operations，483/483 证据引用通过校验。格式与 `git diff --check` 通过；23/23 源码/文档/证据 SHA-256 和 9/9 相关二进制摘要校验通过，完整证据见 `build-codex-agent/agent-phase6-slice45-evidence/`。

### 边界与后续

工具成功表示窗口已显示，不表示 SAM 环境、模型或服务可用。初始化、导入模型、预测、编辑和保存由用户在现有界面操作；阶段 8 的真实模型/GPU/现场验证不在本切片范围。阶段 6/G6 仍为 `IN_PROGRESS`。

## Slice 46：视口快照保存的人机交接

契约：本地逐切片契约（未纳入提交），独立契约审核 **CONTRACT_PASS**；独立实现复核两轮整改后 **IMPLEMENTATION_PASS**。新增危险工具 `save_viewport_snapshot_for_user({viewportId})` 并映射 `VP_SNAPSHOT`。只按精确视口 ID 操作；Advisor/Autopilot 先确认打开保存窗口，随后用户在 Qt 保存对话框选择路径并最终保存，Observer 拒绝。Schema 和执行入口只接受 `viewportId`；聊天/Bridge 中的额外路径参数在生成确认/审计前以 `INVALID_PARAM` 拒绝。响应及审计不含目标路径、文件名或像素内容。

确认批次绑定当时的 MainWindow owner；确认前 owner 替换会使批准过期，即使新窗口复用了相同 viewportId 也不执行。Agent 3D 捕获在 framebuffer 无效时失败关闭；原工具栏仍保留旧行为，在 3D framebuffer 不可用时回退到仍缓存的 2D 图像。用户取消文件选择或拒绝覆盖时，工具结果和审计均记 `cancelled`，不标成功；写入失败为 `SAVE_FAILED`，审计 `undoable=false`。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功。最终定向 CTest **4/4 suites 通过**：`test_agent_viewport_snapshot_handoff`、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller`。新 handoff 用例 **9/9 QtTest 计数通过**，覆盖严格参数/Observer 零副作用、Chat 非法参数、确认后真实 Qt 文件选择器 2D 保存与取消/审计、确认 owner 替换、合成 3D 成功和无帧失败、写入失败、toolbar 2D fallback 像素一致，以及最终 `.png` 目标路径被拒时原始字节保持不变。
- 最终全量 loopback CTest **104/105 suites 通过**。唯一失败 suite `test_mainwindow` 的两条既有失败仍为 `testAgentInputErrorPathDoesNotCrashOrStayThinking`（errorSpy 期望 1、实际 0）和 `testDarkThemeCloseAndDestroyWindowDirectly`（窗口样式与深色主题期望不符）；MainWindow suite 其余 **130** 项通过，Slice46 suite 通过，不将全量 CTest 记作全通过。
- 能力矩阵生成器已将 `VP_SNAPSHOT` 从未支持人机操作同步到 `save_viewport_snapshot_for_user({viewportId})`；基线为 69 插件、126 项 GUI 操作，483/483 证据引用有效。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建/CTest/矩阵/格式/差异/复审日志、源码哈希和相关二进制摘要见 `build-codex-agent/agent-phase6-slice46-evidence/`。

### 边界与后续

离屏 Qt 的原生覆盖提示会进入平台文件对话框辅助循环，无法在该运行环境用 Qt 定时器可靠点击拒绝按钮；覆盖拒绝回归因此通过路径注入 seam 对最终带 `.png` 后缀的既存目标执行同一拒绝决策，并逐字节核验不变。生产执行仍使用 `QFileDialog` 默认覆盖提示且设置 `defaultSuffix("png")`，没有关闭原生确认。该回归不声称覆盖不同桌面系统的原生对话框外观。此 Slice 验收 **PASS**；阶段 6 总体及 G6 继续 **IN_PROGRESS**。

## Slice 47：在文件管理器中显示数据源

契约：本地逐切片契约（未纳入提交），独立契约审核 **CONTRACT_PASS**，独立实现复核修复悬垂指针后 **IMPLEMENTATION_PASS**；Slice47 验收 **PASS**。新增危险工具 `open_data_source_folder_for_user({dataSourceId})` 并映射 `DS_SHOW_IN_FOLDER`。工具严格只接受一个非空 `dataSourceId`，Advisor/Autopilot 需先确认，Observer 拒绝。按唯一数据源 ID 向桌面服务请求打开父目录；只报告 `open_requested`，不声称目录已显示。结果、确认载荷和审计不含路径、目录名或文件名；操作不读写文件、不修改工程，审计 `undoable=false`。原数据源面板入口继续复用相同 opener。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功。Slice47 handoff **7/7 QtTest 计数通过**；定向 CTest **5/5 suites 通过**：handoff、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller`、`test_agent_datasource_dedicated_viewport`。
- 受限环境全量 CTest 因 loopback listener 被沙箱拒绝而有 14/106 suites 失败；获准本地回环后复跑为 **105/106 suites 通过**。唯一失败 `test_mainwindow` 中两项既有断言仍失败：`testAgentInputErrorPathDoesNotCrashOrStayThinking`（errorSpy 实际 0、期望 1）和 `testDarkThemeCloseAndDestroyWindowDirectly`（窗口样式不符）；不将全量回归称为通过。全量明细在 `build-codex-agent/Testing/Temporary/LastTest.log`。
- 矩阵审计重生成结果为 69 插件、126 项 GUI 操作，483/483 证据引用有效；`DS_SHOW_IN_FOLDER` 映射为新工具。独立审核指出临时 `dataSources()` 副本导致的悬垂指针风险，已通过局部持有 QList 修复并复审通过。定向与全量验证证据见 `build-codex-agent/agent-phase6-slice47-evidence/`。

### 边界与后续

桌面服务仅确认接受打开请求，无法保证具体文件管理器窗口已可见。所有 Slice47 验收项通过；阶段 6 与 G6 总体仍为 **IN_PROGRESS**，尚有其他 GUI 人机协作操作需要继续逐项推进。

## Slice 48：打开用户知识库目录

契约：本地逐切片契约（未纳入提交），独立契约审核 **CONTRACT_PASS**、独立实现审核 **IMPLEMENTATION_PASS**；Slice48 验收 **PASS**。新增危险工具 `open_user_knowledge_folder_for_user()` 并映射 `KB_OPEN_DIR`。严格不接受参数；Observer 拒绝，Advisor/Autopilot 确认后由当前 GUI owner 请求桌面服务打开用户知识库目录。目录只在批准后通过既有 `userKnowledgeDir()` 首次创建。结果只表示平台接受请求；工具响应、专属确认摘要和审计不暴露知识库路径或文档信息。共享确认载荷仍遵循阶段 1 通用工程快照协议。

### 验证

- 全量构建成功；Slice48 handoff **6/6 QtTest 计数通过**。定向 CTest **4/4 suites 通过**：`test_agent_knowledge_folder_handoff`、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller`。
- 全量 loopback CTest **106/107 suites 通过**。唯一失败是 `test_mainwindow` 中已知范围外断言 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`；不将全量回归记为通过。日志见 `build-codex-agent/agent-phase6-slice48-evidence/ctest-full-loopback.log`。
- 受限 sandbox 的 loopback 监听失败与上一切片相同，因此使用获准本地回环运行全量套件。矩阵重生成结果为 69 插件、126 项 GUI 操作，483/483 证据引用有效；格式门禁与 `git diff --check` 通过。独立审核、构建/测试日志和摘要见 `build-codex-agent/agent-phase6-slice48-evidence/`。

### 边界与后续

平台接受请求不保证特定文件管理器已显示目录。工具不列举、读取或管理知识库文档；阶段 6/G6 总体继续 **IN_PROGRESS**，剩余 GUI 人机协作操作仍需继续处理。

## Slice 49：同步既有人机交接操作映射

独立契约与映射审核均 **PASS**。补齐 9 项已经有工具和 UI 回归的操作清单映射：SAM 环境初始化、模型导入、服务重启映射到 `open_sam_environment_setup()`；参数菜单登录映射到 `open_user_login_dialog()`；Agent LLM 配置映射到 `open_agent_settings_dialog()`；4 类系统设置映射到 `open_system_settings()`。这些都只导航到现有窗口，SAM 配置动作、凭据提交和配置保存仍由用户本人完成；未将它们描述为 Agent 已执行。`LOGIN_SUBMIT` 与 `FILE_EXIT` 保留为明确的用户本人操作，不提供凭据提交或终止宿主进程的工具；`GV_MOVE/EXECUTE/APPLY` 后由 Slice50 补充人机交接。Agent 权限、System Prompt、设置开关与连接测试维持各自阶段原状。

### 验证

- 操作清单重生成能力矩阵：69 插件、126 项 GUI 操作，483/483 `testId/evidencePath` 引用通过。9 条新增映射经独立复核，所有测试槽和证据文件均真实存在，导出 Markdown 仍保留 `人机协作` 分类。
- 本切片无产品代码变更，复用 Slice48 的全量构建与获准 loopback CTest 证据：106/107 suites 通过；唯一失败仍是 `test_mainwindow` 中两项既有范围外断言。SAM、登录、系统设置具体测试以及 `test_agent_settings_handoff` 均通过。证据见 `build-codex-agent/agent-phase6-slice49-evidence/`。

### 边界与后续

这 9 项仅表示 Agent 能把用户带到适当界面，不代表 Agent 安装/重启 SAM、导入权重、提交登录凭据或保存任何设置。阶段 6/G6 总体验收继续 **IN_PROGRESS**，其余未覆盖的人机协作操作继续逐项审计。

## Slice 50：全局变量界面的人机交接

独立契约与实现复核均 **PASS**。新增无参只读导航工具 `open_global_variables_dialog()`，三权限级均可请求打开既有 `GlobalVarView`。Agent 窗口由 MainWindow 以父子所有权持有并通过 `QPointer` 追踪；重复调用复用同一窗口，关闭后可重新显示，MainWindow 销毁后 guard 清空且后续调用返回 `REQUIRES_GUI`。已有菜单产生的可见模态窗口会被复用并保持模态状态。工具本身不读取或修改变量；响应、调用参数和审计均不包含变量名称、表达式、值、类型或注释。

`GV_MOVE`、`GV_EXECUTE`、`GV_APPLY` 已映射到该人机协作入口。Agent 只打开窗口；用户自行调整顺序、执行表达式或完成应用流程，映射没有声称这些操作已由 Agent 执行。

### 验证

- 全量构建成功；Slice50 定向 CTest 5/5 suites 通过，新增 `test_agent_globalvars_handoff` 覆盖无 GUI owner、错误参数/线程、三权限、变量快照与隐私、创建/复用/关闭重开、原菜单模态窗口复用和 MainWindow 销毁。
- 全量 CTest 107/108 suites 通过。唯一失败 suite 为既有 `test_mainwindow`，其中 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两项断言失败；Slice50 套件及其余 107 suites 通过。不得将该全量结果记为全通过。详细日志见 `build-codex-agent/agent-phase6-slice50-evidence/`。
- 能力矩阵重新生成：69 插件、126 项 GUI 操作，483/483 引用校验通过；格式门禁和 `git diff --check` 通过。独立实现复审 **PASS**。

### 边界与后续

本切片只提供全局变量界面导航，不由 Agent 重排、执行、应用或保存变量。阶段 6/G6 总体验收继续 **IN_PROGRESS**；其余未映射或仅允许用户本人执行的 GUI 操作仍需逐项审计。

## Slice 51：明确 SAM 图像坐标的局部语义

独立契约与实现复核均 **PASS**。`edit_active_annotation_masks` 的笔画点和半径现在明确使用活动标注图像像素单位；活动图像来自 CropImage 时为 crop-local。`get_annotation_session` 的 bbox/polygon 明确使用已保存会话绑定图像的像素坐标，裁剪图会话也为 crop-local，不表示父图坐标。ToolSchema 与用户手册均同步该语义；会话格式和坐标值没有变化，也未加入隐式偏移。

### 验证

- 全量构建成功。最终 Schema 修改后 `test_toolschema`、`test_agent_sam_tools` 和 MainWindow 活动掩膜编辑用例均通过；其中 MainWindow 单例 3/3 QtTest（含 init/cleanup）。
- 全量 CTest 第二轮 107/108 suites 通过，唯一失败仍是 `test_mainwindow` 的两项既有范围外断言。首轮另有 `test_agent_viewport_snapshot_handoff` 的间歇性 SIGSEGV，单独复跑和第二轮全量 CTest 均通过。最终 `radiusPx` 仅为 Schema 文案补充，之后重新全量构建并通过上述定向用例；全量结果不宣称全通过。
- `clang-format`、`git diff --check` 通过。契约复核与实现复核均 PASS；构建、全量/定向测试、复跑、矩阵和哈希证据见 `build-codex-agent/agent-phase6-slice51-evidence/`。

### 边界与后续

本切片修正文案，不恢复父图坐标。CropImage 单次/连续裁剪与任意下游几何变换的可靠坐标追踪仍未实现；G6 标准 3 继续部分完成，阶段 6 总体验收继续 **IN_PROGRESS**。

## Slice 52：SAM 打开图片的人机交接

契约：本地逐切片契约（未纳入提交），独立契约复核 **CONTRACT_PASS**。新增无参数只读 GUI 工具 `open_sam_image_picker_for_user()`，将 `SAM_OPEN_IMAGE` 从不等价的 `sam_predict(imagePath)` 更正为人机协作映射。工具创建/复用 SAM 对话框并显示已有“打开图片”按钮；用户自行点击和选图。它不传路径、不读图片、不替换当前图像/会话/提示/撤销栈、不启动 backend、不运行预测。既有 `open_sam_annotator_dialog()`/`TB_QUICK_ANNOTATE` 行为保持原样。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` 成功；Slice52 定向 CTest 3/3 suites 通过：`test_agent_sam_annotator_handoff`、`test_toolschema`、`test_agentpermissions`。
- 回环环境最终全量 CTest **107/108 suites 通过**。唯一失败 suite `test_mainwindow` 中两项既有断言失败：`testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`。`test_agent_viewport_snapshot_handoff` 在最终全量运行中通过。全量不记为通过。最终原始日志见 `build-codex-agent/agent-phase6-slice52-evidence/ctest-full-loopback-final.log`。
- 空白 setup-only 窗口回归覆盖按钮可见/可用、无图、无配置初始化、无 SAM 进程；用户取消选图后，session/prompt/image/backend/config/env 状态不变，隔离 app-data 文件列表与字节相对打开 picker 后完全相同。picker 打开前后的 app-data 差异只允许 `logs/` 下脱敏日志追加。无图非空 session 由公开会话 API 拒绝，拒绝不改变空 session；已有 viewport 图像与标注会话复用用例覆盖图片、session JSON、提示、撤销栈、backend 状态及主视口像素均不变。另覆盖新工具自身的无 GUI、非法参数、错误线程和三种权限；Schema 与无参数契约通过。
- 矩阵重新生成：69 插件、126 GUI 操作；`SAM_OPEN_IMAGE` 已映射到 `open_sam_image_picker_for_user()`，483/483 evidence 引用校验通过。构建/定向与全量日志、矩阵审计、源码和二进制摘要归档于 Slice52 evidence 目录。
- 独立实现复核 **IMPLEMENTATION_PASS**，未发现阻断；Slice52 范围验收 **PASS**。G6 与阶段 6 整体继续 **IN_PROGRESS**，不得由本切片通过推定其他标准完成。

### 边界

图片路径选择、图片读取及选择新图片后的既有 UI 行为都由用户显式点击完成。setup-only 构造允许读取已经初始化的 ConfigManager 中已保存 SAM 模型路径以填充 UI 状态；不初始化或写配置、不泄露路径。此 Slice 不表示图片已加载或 SAM 推理已完成。

## Slice 53：SAM 导出逐对象语义断言

契约：本地逐切片契约（未纳入提交），独立契约审核 **CONTRACT_PASS**，独立实现复核 **IMPLEMENTATION_PASS**。本切片只增强测试，不改导出器或 Agent 导出逻辑。现有双对象会话经 Agent 路径导出 LabelMe、YOLO Seg 与 COCO 后，测试直接解析生成物并和固定夹具比较：LabelMe 核对图像尺寸、对象数、标签及全部原图坐标；YOLO 核对对象数、类别 ID 及以图像宽高归一化的全部顶点；COCO 核对图像尺寸、类别名称/ID、对象数、每对象 category、完整 segmentation、bbox 与 area。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 成功。导出相关定向 CTest **4/4 通过**：`test_agent_sam_tools`、`test_labelme_exporter`、`test_yoloseg_exporter`、`test_cocoexporter`。
- 最终全量 CTest **107/108 suites 通过**。唯一失败 suite `test_mainwindow` 中两项断言失败：`testAgentInputErrorPathDoesNotCrashOrStayThinking`（`errorSpy.count()` 实际为 0，期望 1）和 `testDarkThemeCloseAndDestroyWindowDirectly`（实际浅色样式表与预期深色样式表不同）。两项均不属于本切片；全量结果不记作全通过。此前全量中的 `test_agent_datasource_dedicated_viewport` 一次 SIGSEGV 后，单独复跑通过，最终获准全量运行也通过。日志见 `build-codex-agent/agent-phase6-slice53-evidence/`。
- Slice8 已有保护断言继续覆盖源会话字节不变、目标已存在时 sentinel 保留、no-replace 发布竞争不覆盖目标及失败清理；本切片未削弱或替代这些断言。
- `clang-format --dry-run --Werror tests/test_agent_sam_tools.cpp`、`git diff --check`、矩阵引用校验通过；源码与相关二进制摘要见 Slice53 evidence 清单。

### 边界与后续

G6 标准 5 现可标记 PASS。该结果只证明固定语义夹具与现有失败保全行为，不替代不同文件系统/平台的现场验证，也不关闭裁剪坐标、迟到回调、GUI 前置条件等其他 G6 标准。阶段 6 整体保持 **IN_PROGRESS**。

## Slice 54：MeasurementInput 迟到续跑回调隔离

契约：本地逐切片契约（未纳入提交），补充契约独立复核 **CONTRACT_PASS**，最终实现复核 **IMPLEMENTATION_PASS**。最后一次有效采点排入的零延时续跑回调现在绑定 Project runtime identity、目标模块实例代次与精确 `ModuleInstance` 快照，并绑定单调运行代次。等待期间也绑定真实 viewport 实例及 `contentGeneration`：换图、清空或销毁会拒绝采点/终止同代等待；跨 viewport 点击不混入本轮，原视口仍可继续。旧回调遇到已被新运行替代的代次时静默退出；同代次下工程、目标模块或绑定视口身份失效时停止本轮并清理 UI。Project 打开/关闭立即清理旧运行 UI，但不增加运行代次。显式重新发起 `awaitUserPick` 会重置本轮采点数量和游标，允许停止后重新采点。

### 验证

- 完整构建 `cmake --build build-codex-agent -j2` 成功。Slice54 定向 9 个 QtTest 加 init/cleanup 共 **11/11** 通过：工程/模块替换、stop 后新一轮、无关工程字段、首点前/后换图、跨 viewport 拒点、排队后销毁 viewport，及有效流程对照 `testRunCreatesMeasurementInputForConsumer`。
- 隔离临时 HOME 的全量 CTest **105/108 suites 通过**。`test_mainwindow` 两项范围外主题断言失败；`test_samannotatordialog` 两项既有异步 embedding/session 断言失败（embedding 仍为 `emb-1`，`setImageCount` 少 1）；`test_agent_datasource_dedicated_viewport` 全量运行中一次 SIGSEGV，单独复跑通过。全量结果不记为通过。全量与失败 suite 单独复跑日志见 Slice54 evidence 目录。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；全量测试使用临时 HOME 同步构建插件并 unset `DEEPLUX_APP_DATA_DIR`，未触碰用户 `~/.deeplux`。源码、契约、验收记录、构建/测试日志及二进制摘要见 Slice54 evidence 目录。

### 边界

本 Slice 证明本地 GUI 的 2D 图像 viewport 采点队列身份失效保护；3D 点击路径接入同一校验 helper，但没有单独的 3D 回归。它不替代不同桌面平台的 GUI 现场验证，也不关闭 CropImage 父图坐标或无 GUI/图像/登录/模型环境前置条件审计。G6 标准 4 现标记 PASS；阶段 6 整体保持 **IN_PROGRESS**。

## Slice 201：3D MeasurementInput 目标代次迟到续跑隔离（PASS，限模拟 world-pick）

契约：本地逐切片契约（未纳入提交）。测试通过 `point3DWorldClicked` 驱动真实 MainWindow 3D 采点连接，在 point-cloud viewport 中完成两点请求后，立即以相同 ID、完全相同的 `ModuleInstance::toJson()` 移除并重建目标模块；同一 Project 和 viewport 保持不变，断言参数实例 generation 变化。处理最终采点排入的零延时续跑后，runStarted/runFinished/moduleStarted 均无新增，替换模块 JSON 仍与完成快照一致。之后安装一个新的 `awaitUserPick=true` 请求，采集两个新的 World3D 点并验证流程恰好启动/完成一次、两点精确写入新实例。

此用例隔离 `parameterInstanceGeneration` 检查：若仅移除 generation 判断，Project identity、viewport identity/contentGeneration 与模块 JSON 快照检查仍会通过，迟到回调会继续执行并使运行计数断言失败。独立实现复审 **IMPLEMENTATION_PASS**。`test_mainwindow` 目标构建成功；4 个定向 G6 3D 测试函数共 **6/6 QtTest 项通过**（含 init/cleanup），包括新增代次回归、点云替换取消、跨视口拒绝和 World3D 输出对照。`clang-format --dry-run --Werror`、`git diff --check` 通过。完整日志见 `build-codex-agent/agent-phase6-slice201-evidence/`。

边界：点由测试直接发射 world-pick 信号，验证 MainWindow 连接与续跑语义；不验证真实鼠标/相机/GPU 的 3D 命中，也未运行全项目 CTest。G6 标准 4 现覆盖 2D viewport 失效与 3D 目标实例代次失效；阶段 6 其它标准和阶段 8 外部设备门槛仍按各自记录执行，阶段 6 整体保持 **IN_PROGRESS**。

## G6 标准 1/2：GUI 操作映射与状态同步审计

### 标准 1：界面操作映射

- 来源清单 `docs/baseline/agent-gui-operations.json` 包含 126 个唯一 operationId；每项具有 ownerApi、toolMapping、phase，所有未直接执行项仍留在清单并注明人机协作或不支持原因。
- 分类统计：直接工具 66、需人工协作 37、未支持 18、工具组合 5；矩阵重生成并与清单同步。工具映射及矩阵引用由 `scripts/audit_agent_capability_baseline.py` 校验，当前 `ownerApi source validation OK: 102 explicit C++ references`、`reference validation OK: 483 cells`。
- 首轮独立复审确认映射数据正确，但指出原门禁不能验证方法所属类，也会漏掉斜线后的复合方法（NOT_PASS）。已将复合操作的各方法展开为完整类限定引用，并把校验改为按剥除注释/字面量及 C/C++ 常量预处理条件后的类体声明或类限定实现查找；重复类名歧义拒绝通过；控件名要求在去注释和 raw string 后的源码里 `setObjectName` 精确赋值。新增 `scripts/test_audit_agent_capability_baseline.py`，当前 **6/6 通过**；覆盖错类、复合方法缺失、伪控件、注释/raw string、连续死分支、`0 && 1`、`0xFFFFFFFFU + 1 != 0x100000000`、`-1/2`、`~0U < 0`、三种空字符转义、三元表达式、`!1 == 2` 优先级和 `!-!0` 连续一元运算等反例及活分支正例。求值遵循预处理器 intmax_t/uintmax_t、字符常量、`?:` 及一元运算优先级语义，未知宏分支按“可能有效”处理。主生成器使用 `build-codex-agent/agent-phase6-slice46-evidence/runtime-list-plugins.log`，结果为 69 插件、126 操作；108 个显式 C++ 引用、483 个矩阵引用均通过。
- **状态：PASS（限定为静态来源门禁与当前 126 项映射清单）。** 独立复审确认类归属、复合方法、预处理条件求值和 108 个源码引用校验通过；6/6 持久化边界测试通过。此结论不证明操作语义逐项正确，也不证明 UI 操作枚举无遗漏。

### 标准 2：数据源/变量状态与持久化 — PASS（限定范围）

- Agent 数据源导入、GrabImage 显式图像绑定保存并重开验证：`tests/test_agentcontroller.cpp::testDataSourceImportHeadlessAndConfirmedForSupportedFormats`、`testRegisteredImageCanBeBoundToExplicitGrabImageInstance`。
- Agent 数据源增删、显示和可见性反映到 GUI：`tests/test_mainwindow.cpp::testAgentDataSourceRemovalRefreshesGuiAndRejectsBusyMutation`、`testAgentDataSourceDisplayAndVisibilityTools`。
- 全局变量读写经 `GlobalVarManager::try*UserVariable` 发出变更信号，由 `GlobalVarView` 订阅刷新；类型 CRUD/往返测试见 `tests/test_agent_globalvars_handoff.cpp`、`tests/test_globalvarview.cpp`、`tests/test_agentactor.cpp::testGlobalVariableTypedCrudIsLiteralAndFailClosed`、`testGlobalVariableAllSupportedTypesRoundTrip`。
- 全局变量明确为进程级状态，不随 Project 保存/重开；队列排序、表达式执行和手动应用仍由用户在人机协作入口完成，不宣称为 Agent 可执行操作。
- 独立复审：**PASS**。本结论限定于上述数据源/绑定与变量 CRUD、GUI 信号刷新路径；不扩展到所有数据源格式、外部文件生命周期或全局变量排序/执行/应用。

## Slice 55：ImageData 坐标来源与裁剪平移（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），契约独立复核 **CONTRACT_PASS**。运行期 `ImageData` 新增源图 UUID、尺寸及局部到源图的平移；字节序列化仍仅含像素，反序列化后来源明确失效。GrabImage 每帧建立新根；CropImage 行输出及首图输出组合裁剪原点；拼图重排和透视/仿射标定输出失效来源；DisplayManager 将 `DisplayData` 原样交给 viewport，避免转换 `QImage` 时丢失来源。测量点与矩形 ROI 继续以 `ImageLocalPixel` 为权威；有映射时额外报告 `SourceImagePixel`、来源 UUID 和尺寸，无映射时明确 `sourceCoordinateAvailable=false`。MeasurementInput 下游算法输出仍保留局部坐标，并新增来源坐标旁路字段。`HImageWidget` 现在也持有运行期来源映射；活动 SAM 对象页保持局部 bbox，并在映射有效时附带 `sourceBbox`、源图 ID 和源尺寸。

### 验证

- `cmake --build build-codex-agent -j2` 全量构建成功；之后 `test_mainwindow` 与 `test_samannotatordialog` 定向目标也重新构建成功。
- `test_batch4` 与 `test_himagewidget` CTest **2/2 suites 通过**，覆盖连续/多行裁剪平移、几何失效、留边点击拒绝、ROI 越界裁剪和零面积拒绝。SAM 活动对象来源映射定向 QtTest **3/3（含 init/cleanup）通过**；快速测量生命周期/视口换图 QtTest **3/3**，旧请求过期且新请求只返回新图来源 ID。MainWindow 全套在隔离 HOME 并同步 66 个插件后 **138 passed / 2 failed**（CTest 退出码 8）；剩余仅主题断言 `testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme` 和 `testDarkThemeCloseAndDestroyWindowDirectly`，与本 Slice 无关。此前因插件未同步而失败的 Agent 错误/部分失败单项，在正确隔离插件环境重跑通过。
- 独立实现复审发现并推动修复三项：留边坐标被当作有效像素；`point_plane` 三维点被套用图像平移；SAM 对话框直接替换共享 HImageWidget 图像时 viewport 缓存、来源和代次未更新。现在只接受图像矩形内点选；ROI 终点裁剪且拒绝零面积；MeasurementInput 按 mode 标记 `ImageLocalPixel`、`World3D` 或 `Unspecified`；ViewportWidget 监听共享控件的换图/清空信号并同步缓存、来源和 `contentGeneration`，自身显示入口保留携带的 provenance。回归已通过。
- 独立复审曾指出活动 SAM 对象页缺少来源 bbox；补齐后，最终实现复审 **PASS**：绑定视口的有效 provenance 产生 `sourceBbox/sourceImageId/sourceImageSize`，失效时返回 `sourceCoordinateAvailable=false` 并省略源字段。回归覆盖外部绑定 `HImageWidget` 的平移 bbox 与无映射 fail-closed。
- 全量 `test_mainwindow` 采用隔离 HOME 和 `scripts/sync-plugins.sh build-codex-agent` 同步的 66 个插件，结果 **138 passed / 2 failed，CTest 退出码 8**。两项均为主题样式断言：`testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme`（`:9640`）与 `testDarkThemeCloseAndDestroyWindowDirectly`（`:4009`）；完整原始输出在 `build-codex-agent/agent-phase6-slice55-evidence/test-mainwindow-full-synced.log`。旧 `point2DClicked` 注入已改为 `point2DSourceClicked`，快速测量取消用例现在通过。
- `scripts/test_audit_agent_capability_baseline.py` **6/6 通过**；矩阵审计仍为 69 插件、126 操作、108 个显式 C++ 来源引用及 483 个证据引用通过。格式检查与 `git diff --check` 通过。构建、定向测试、全套 UI 测试及 SHA-256 证据位于 `build-codex-agent/agent-phase6-slice55-evidence/`。
- 独立实现复审最终 **PASS**。Slice55 状态：**PASS（限运行期来源映射、裁剪派生、点选/ROI、MeasurementInput 与活动 SAM 对象响应）**；独立文件会话读取不会从旧磁盘数据推测父图来源，坐标仍为会话图像局部坐标。G6 标准 3 仍未通过，阶段 6 整体保持 `IN_PROGRESS`。

### 边界

当前验证覆盖 CropImage 连续平移、多裁剪原点、MeasurementInput 模式坐标语义、拼图/标定几何失效、留边点击与 ROI、SAM 活动对象来源 bbox、绑定视口换图和 Agent 点选路径。磁盘会话文件不会保存运行期父图 provenance，`get_annotation_session` 不推测该映射；全量派生插件审计、缩放/换图/多视口完整矩阵仍待补齐。G6 标准 3 继续未通过，阶段 6 整体保持 **IN_PROGRESS**。

## Slice 56：2D 视口变换与迟到鼠标释放隔离（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**，独立实现复审 **IMPLEMENTATION_PASS**。`HImageWidget::cancelPendingPointerInput()` 清除待释放点击、ROI 绘制和中键平移状态；图像替换/清空、ROI 模式切换及 Agent 快速测量取消均调用它。请求 A 的旧 mouse release 不会写入请求 B；取消平移后迟到的 move 也不会改变新图偏移，并恢复继承光标。

### 验证

- 全量 `cmake --build build-codex-agent -j2` 成功；`test_mainwindow`、`test_himagewidget` 定向目标构建成功。
- `test_himagewidget` CTest **1/1 suite 通过**，包括新增的取消中键平移回归：按下/拖动后取消，光标恢复，迟到 move/release 均不改变图像坐标映射。变换用例在初始 fit、程序 zoom、wheel zoom、中键 pan、resize 五种状态下点击 4 个非对称图像点，实际 zoom ≥2/3 且局部点误差 ≤0.75 像素。
- MainWindow 定向 QtTest：视口变换/来源隔离 **3/3（含 init/cleanup）通过**，覆盖 wheel/zoom/pan/resize、外视口拒绝、A/B 独立 UUID/尺寸/origin、ROI 四角局部坐标及 source pixel = 实际 local + origin；旧 click/ROI 跨请求释放 **3/3（含 init/cleanup）通过**，随后 B 的新事件仍可完成。ROI 四角均校验来源 ID、尺寸及坐标映射。
- 隔离 HOME 并同步 66 个插件后完整 `test_mainwindow` **140 passed / 2 failed，退出码 2**；仅 `testMainWindowLayoutKeepsConfirmedWorkflowTabsAndReadableTheme` 和 `testDarkThemeCloseAndDestroyWindowDirectly` 两项主题样式断言失败，与本 Slice 无关。`testAgentInputErrorPathDoesNotCrashOrStayThinking` 本轮通过。原始输出与插件同步日志位于 Slice56 evidence 目录；全量结果不记为通过。
- 完整构建、`clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建/测试日志和源码及二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice56-evidence/`。
- 独立实现复审 **IMPLEMENTATION_PASS**；本 Slice **PASS（限当前 2D 视口变换、来源隔离与 pointer-input 取消）**，不扩大为 G6 标准 3 或阶段 6 PASS。

### 边界

Slice56 只证明当前本机 Qt 5.15.3 的 2D widget 变换、来源隔离和 click/ROI 取消边界。独立文件会话父图映射、全部派生插件几何审计、多平台桌面事件与真实显示器 DPI 仍未完成；G6 标准 3 和阶段 6 整体保持 **IN_PROGRESS**。

## Slice 57：3D 点云原生坐标拾取闭环（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 **IMPLEMENTATION_PASS**。`Viewport3DContent` 将精确的 `PointCloudData::points` 世界坐标通过双精度信号提供给 Agent，同时保留既有 `QVector3D` GUI 兼容信号。取消、清空或替换点云会使按下未释放手势失效并清除旧候选点；MainWindow Agent 快速测量只接收绑定 viewport 的有效拾取，并将 `World3D/pointCloudWorld` 原始 double 坐标写入可查询状态。

### 验证

- 最终 `cmake --build build-codex-agent -j2` **退出码 0**。
- `test_3d_interaction` **7/7 QtTest 通过**，CTest **1/1 suite 通过**；覆盖实际 press/release、相机 wheel/orbit 后 CPU 投影拾取、取消/换云/清空时旧 release 丢弃。
- 最终代码定向 MainWindow Agent 3D 测试 **4/4（含 init/cleanup）通过**：Agent 快速测量生命周期/视口隔离及 World3D 拾取、过期迟到释放；点云 X=`16777217` 在 Agent 状态中保持双精度。
- 全 MainWindow 在隔离插件 HOME、获准本地 socket 环境的最终源码状态运行 **122 passed / 21 failed，退出码 21**；完整日志为 `build-codex-agent/agent-phase6-slice57-evidence/final-test-mainwindow-full.log`。失败包括 17 项 G4 Inspector/Undo/config/drop/running 断言（其中 2 项报告 `CropImage` 未加载）、`testAgentPartialFailureSummaryAppearsInChat`、两项主题断言和 `testInspectorRuntimeStateSyncsToRunEngine`。它们不在 Slice57 改动路径；全量结果不记为通过。此前 141/2 仅为较早快照，不作为本轮结果。
- 离屏 Qt 报告无法创建 QOpenGLWidget context；本切片通过契约规定的 CPU 投影事件路径验证，**不声明 GPU 或真实桌面渲染拾取通过**。格式门禁及 `git diff --check` 通过；构建和定向测试日志及后续 SHA-256 清单位于 Slice57 evidence 目录。

### 边界

Slice57 只证明当前 Linux/Qt 5.15.3 环境下 CPU 投影拾取、Agent 3D 状态坐标精度、viewport 隔离及迟到 release 作废。它不完成全部 3D 算法坐标语义审计、GPU 拾取、多平台显示器验证、G6 标准 3 或阶段 6 总体验收；阶段 6 继续 **IN_PROGRESS**。

## Slice 58：NPointCalibration 到来源图像的逆映射（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复审 **IMPLEMENTATION_PASS**。`ImageCoordinateProvenance` 现以可组合的双精度 3×3 齐次矩阵表示当前像素到来源像素的映射，支持逐点失败和拒绝跨 horizon 的矩形包围盒映射。NPointCalibration 只有在逆仿射/透视关系有效时才把输出局部坐标映回输入，再组合已有 CropImage 来源；无法映射仍 fail closed。MeasurementInput 点、线、点集和 ROI/视口/SAM bbox 消费者改用通用映射，局部算法输入、图像坐标系、旧字段键和图像像素保持不变。运行期来源映射不写入 `ImageData` 序列化。

### 验证

- 最终全量构建 `cmake --build build-codex-agent -j2` **退出码 0**；增加组合顺序回归后，`test_batch4` 目标增量构建和 CTest **1/1 suite 通过**（QtTest 全部用例在同证据目录记录）。全量构建仅出现一个未触及文件 `FreeformSurfacePlugin.cpp` 的既有 `unused variable 'd'` 编译警告。
- `test_samannotatordialog::activeAnnotationObjectsReportSourceBboxWhenVerified` **3/3（含 init/cleanup）通过**：覆盖平移与仿射 bbox、四角映射有限但矩形跨 horizon 时省略来源 bbox并保留局部 bbox。
- MainWindow 变换来源点、迟到 click/ROI release 和图像视口采点定向测试 **5/5（含 init/cleanup）通过**。
- `test_batch4` 覆盖 Crop→Affine→MeasurementInput、Calibration→Crop 反向顺序、透视非共线坐标映射、原图 ID/尺寸、局部坐标不变、无来源时 fail closed、单点 horizon 失败及点集 null 对齐；校正图像序列化后来源仍失效。
- 独立实现审核 **IMPLEMENTATION_PASS**。构建、定向测试、格式、差异和 SHA-256 证据位于 `build-codex-agent/agent-phase6-slice58-evidence/`。

### 边界

Slice58 只关闭 NPointCalibration 的可逆仿射/透视像素来源映射及其当前 consumers。其它派生插件完整几何审计、SAM 会话磁盘保存/重开后的 parent lineage、点云算法坐标语义和物理世界坐标转换仍未完成；G6 标准 3 与阶段 6 整体保持 **IN_PROGRESS**。

## Slice 59：SAM 会话持久化来源坐标 lineage（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复审 **IMPLEMENTATION_PASS**。`.deeplux-anno.json` 可选保存版本 1 的 `sourceCoordinates`：来源 ID/尺寸和行优先 3×3 local-to-source 矩阵。严格解析仅接受合法、有限、可逆矩阵；畸形或未知版本只丢来源记录，不丢合法图像和标注。保存时从当前绑定视口取 provenance；普通打开和 Agent 快照打开只在图像非空摘要与会话摘要一致时恢复，并与同一 QImage 一起设到当前 `HImageWidget`。旧会话、无摘要记录、错误记录都不推测来源映射；局部标注 bbox、mask 与导出格式不变。

### 验证

- 全量构建 `cmake --build build-codex-agent -j2` **退出码 0**。
- 相关 CTest **6/6 suites 通过，0 失败**：`test_annotation_session`、`test_samannotatordialog`、`test_agent_sam_annotator_handoff`、LabelMe、YOLO-Seg 与 COCO 导出器。SAM 假后端测试在允许 localhost TCP listener 的环境运行。
- 会话模型回归覆盖版本 1 往返、版本缺失/非整数或未知、source ID/尺寸错误、矩阵缺项/非数值/奇异时仅丢 provenance 并保留对象；SAM 对话框回归覆盖实际保存重开、摘要验证、legacy 无记录、有效记录但摘要为空，以及 horizon 时局部 bbox 保留且 sourceBbox 省略；Agent handoff 回归覆盖有效摘要下的绑定快照映射恢复。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建和 CTest 日志、独立审核结论与哈希见 `build-codex-agent/agent-phase6-slice59-evidence/`。

### 边界

Slice59 只关闭 SAM 会话在已绑定同摘要图像上的来源映射持久化与恢复。它不验证父来源文件仍存在或内容未变，不改动 SAM 局部标注坐标和 mask。其它派生插件完整几何审计、3D 算法坐标语义和物理坐标仍未完成；G6 标准 3 与阶段 6 整体保持 **IN_PROGRESS**。

## Slice 60：JigsawPuzzle 顺序网格像素来源（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复审 **IMPLEMENTATION_PASS**。源码核实当前 JigsawPuzzle 按行优先顺序切片并放回原网格位置，输出只是丢弃不能整除网格的右/下尾部，所有保留像素的局部坐标均与输入同坐标。插件现保留输入 provenance。若行列网格超过输入尺寸，插件在创建 ROI 前清空输出、发出明确错误并失败，避免此前 OpenCV 零尺寸 ROI 异常和复制来的旧 provenance 泄漏到失败输出。

### 验证

- 全量 `cmake --build build-codex-agent -j2` 退出码 **0**。
- `test_batch4` CTest **1/1 suite 通过**；新增奇数尺寸非对称像素逐点比较、非 identity 来源矩阵映射、无来源输入不造来源，以及分别超高/超宽网格失败边界。
- 失败边界断言 `errorOccurred` 非空、预置输出的图像/metadata/provenance 整体清空、输入像素/metadata/来源映射不变。其余 CropImage/NPointCalibration/MeasurementInput 回归在同一套件通过。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；构建、测试、独立审核和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice60-evidence/`。

### 边界

Slice60 只适用于当前“按原顺序拼回”的实现，不实现真实碎片排序。未来任何重排、缩放、旋转或空间变换必须更新或失效 provenance。其它派生插件和 3D 算法坐标语义审计仍在进行；G6 标准 3 和阶段 6 保持 **IN_PROGRESS**。

## Slice 61：内置图像滤波的坐标网格保持（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复审 **IMPLEMENTATION_PASS**。源码审计确认 `ImageScript` 是反转/灰度/模糊/锐化四种内置操作，不执行用户脚本；`PerProcessing` 的 11 种已实现滤波、边缘和形态学操作均保留二维网格。此前依赖 `output=input` 隐式复制 provenance，本 Slice 为全部操作补上回归锁定。

### 验证

- 全量 `cmake --build build-codex-agent -j2` 退出码 **0**。
- `test_finalplugins` 与 `test_imageplugins` CTest **2/2 suites 通过**，覆盖 4 种 ImageScript 与 11 种 PerProcessing 操作；统一使用 61×47 三通道非对称图像。
- 每种操作均验证真实执行成功、输出尺寸、来源 ID/尺寸、完整 9 项 local-to-source 矩阵及两个非对称位置映射。PerProcessing 输入适配 Sobel/Laplacian/Canny 通道要求。
- 格式门禁与 `git diff --check` 通过；构建、测试、独立审核与 SHA-256 见 `build-codex-agent/agent-phase6-slice61-evidence/`。

### 边界

Slice61 只证明这两组当前内置算子的像素网格与运行期来源映射保持，不证明灰度/边缘/形态滤波结果仍具有原像素的测量含义，也不实现用户可执行脚本。其它图像空间插件、3D 算法坐标语义仍未全部审计；G6 标准 3 和阶段 6 保持 **IN_PROGRESS**。

## Slice 62：Agent 图像输出来源坐标摘要（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复审 **IMPLEMENTATION_PASS**。`get_module_output` 的图像摘要现对有效 provenance 输出来源 UUID、来源尺寸及行优先 `localToSource` 九系数矩阵，使 Agent 能按该矩阵解释同一输出中的局部图像坐标。只透传已有运行期映射，不变换测量/检测/ROI 值。为避免任意外部 ID 中的路径或文本被转发给模型，仅接受固定格式 UUID；无 provenance、路径样式和超长 ID 均不输出 `sourceCoordinates`。递归 list/map 中的嵌套图像共用该摘要逻辑。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**。
- `test_agentactor` CTest **1/1 suite 通过**，QtTest **69 passed / 0 failed / 0 skipped**。真实工具调用覆盖非 identity 矩阵及两个局部点映射、来源 ID/尺寸、nested list/map、路径样式/超长/无来源 ID 省略，以及 `preview=false/true`；预览 PNG 可解码且尺寸、sourceRect 和 scale 字段保持。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过。契约、构建/测试日志、源码/测试/二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice62-evidence/`。

### 边界

Slice62 只在图像摘要中提供来源映射上下文，不将每个标量/检测/几何输出自动转换成来源坐标，也不改变已有坐标值、预览和磁盘格式。它不完成其它图像插件及 3D 算法坐标语义审计；G6 标准 3 与阶段 6 继续 **IN_PROGRESS**。

## Slice 63：图像坐标来源映射使用指引（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复审 **IMPLEMENTATION_PASS**。`get_module_output` 的工具描述在原有 cycle、key/port、分页、预览、STALE_OUTPUT 及 PLC uncertain 要求后追加坐标说明：`image_pixels` 是当前图像局部像素；有效 `sourceCoordinates.localToSource` 以行优先齐次矩阵将列向量 `[x,y,1]^T` 映到来源坐标，整数索引按像素中心理解；分母按 `ImageCoordinateProvenance::mapLocalToSource()` 的精确相对阈值判定，缺映射或点映射失败时不得声称来源坐标。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**。
- `test_toolschema` CTest **1/1 suite 通过**，QtTest **42 passed / 0 failed / 0 skipped**。实际 OpenAI-compatible function payload 回归覆盖矩阵方向/公式/w 阈值/像素中心/fail-closed 提示、旧描述全部运行要求，以及 8 个参数的完整 JSON Schema（类型、描述、default 缺失、required 列表）。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；独立审核、构建/测试日志及 SHA-256 见 `build-codex-agent/agent-phase6-slice63-evidence/`。

### 边界

Slice63 只让 LLM 获得正确使用 Slice62 来源矩阵的指引，不在工具执行中自动变换数值结果，不保证模型对任意提示都能无误进行数值计算。其它图像插件与 3D 算法来源坐标审计仍在继续；G6 标准 3 和阶段 6 保持 **IN_PROGRESS**。

## Slice 64：Circle2D 与 Region2D 来源图像坐标（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 **IMPLEMENTATION_PASS**。`get_module_output` 序列化 `Circle2D`、`Region2D` 时使用同一模块/同一 run 的图像快照 provenance。有效 UUID 下，Circle 的 `sourceImageCoordinates` 返回映射圆心；Region 返回 `mapRectToSourceBounds()` 计算的来源轴对齐外包 bbox。局部中心、半径、bbox、置信度、标签、polygon 点数和 mask 长度原样保留。来源 ID 不合 UUID、映射无效、圆心落在 horizon 或 Region bbox 跨 horizon 时省略来源对象并保留局部值。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**。
- `test_agentactor` CTest **1/1 suite 通过**，QtTest **70 passed / 0 failed / 0 skipped**。覆盖非 identity 透视矩阵映射、来源 ID/坐标系及嵌套字段位置、原局部字段保持、无来源/路径 ID 隐私处理、圆心 horizon 与 bbox 跨 horizon 失败关闭。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；源码/二进制快照、独立审核、构建/测试日志及 SHA-256 见 `build-codex-agent/agent-phase6-slice64-evidence/`。

### 边界

Slice64 只为 `Circle2D` 圆心和 `Region2D` bbox 增加来源图像坐标；不推导来源圆半径，不输出 polygon/mask 内容，不覆盖 `DetectionList` 或普通标量值。其它图像空间插件和 3D 算法坐标语义仍需审计；G6 标准 3 与阶段 6 保持 **IN_PROGRESS**。

## Slice 65：分页 DetectionList 来源图像坐标（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**。直接分页和递归 list/map 中的每个 detection row 保留根级局部框、分数与标签，增加 `coordinateSystem=image_pixels`；有效同 run/module provenance 和标准 UUID 下，增加映射后的来源轴对齐 bbox。UUID/provenance 无效或框跨映射 horizon 时省略整个来源对象，保留局部值。来源字段计入既有 64 KiB 响应限制。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；`test_agentactor` CTest **1/1 suite 通过**，QtTest **70 passed / 0 failed / 0 skipped**。
- 覆盖直接分页和嵌套容器两条路径的来源 UUID、坐标系、映射 bbox 数值及局部字段；覆盖无来源、路径 ID、horizon 失败关闭，以及 33,000 字符双行触发缩页、65,500 字符单行 `ROW_TOO_LARGE`，两类响应均不超过 65,536 bytes。
- `clang-format --dry-run --Werror`、`git diff --check` 通过；构建/测试日志、独立复核、源与二进制快照及 SHA-256 见 `build-codex-agent/agent-phase6-slice65-evidence/`。

### 边界

Slice65 只为 `DetectionList` 增加局部坐标系和来源轴对齐 bbox；不对 bbox 做精确透视四边形表达、不改变其它类型或算法输出。G6 标准 3 其余图像空间插件与 3D 算法坐标审计、环境条件核验以及阶段 6 整体仍为 **IN_PROGRESS**。

## Slice 66：Point2D/PointSet2D 数值列表不推测图像坐标（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**。本切片只覆盖 `Point2D` 的两数列表与 `PointSet2D` 的嵌套数值列表兼容形式：即使同一运行存在有效、非 identity 的 ImageData 来源映射，Agent 也完整保留原数组数值、层级和顺序，不猜测来源坐标。`QPointF`/`QVector<QPointF>` 等其它合法载荷、插件间坐标契约差异继续列为后续审计范围。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；`test_agentactor` CTest **1/1 suite 通过**，QtTest **70 passed / 0 failed / 0 skipped**。
- 真实 `get_module_output` 查询在有效来源 UUID 和非 identity 矩阵下完整比较 Point2D 数值对及嵌套 PointSet2D 数值列表；精确 JSON 数组比较同时验证没有增加来源坐标字段。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；构建/测试日志、独立复核、测试源码/二进制快照与 SHA-256 见 `build-codex-agent/agent-phase6-slice66-evidence/`。

### 边界

Slice66 证明的是旧式数值列表序列化不进行语义猜测，不统一 `Point2D`、`PointSet2D` 的生产者坐标系，不调整 MeasurementInput 现存元数据/运行时维度差异，不覆盖 Qt 点类型及其它 2D/3D 载荷。G6 标准 3 和阶段 6 继续 **IN_PROGRESS**。

## Slice 67：Qt Point2D/PointSet2D 载荷序列化（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**。Agent 输出序列化现在支持 `QPointF` 与 `QVector<QPointF>`：点返回 `[x,y]`，点集按原顺序返回/分页为 `[[x,y],...]`；map/list 容器递归支持。所有坐标必须有限，否则整次查询以 `UNSUPPORTED_OUTPUT_TYPE` 失败且无部分 `value`。在有效图像来源 provenance 下仍不猜测坐标系或添加来源映射。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；`test_agentactor` CTest **1/1 suite 通过**，QtTest **70 passed / 0 failed / 0 skipped**。
- 覆盖直接点、两页点集与完整分页字段、递归 map/list、有效来源下不推断坐标、PointF 和点集 NaN/Inf（包括坏点位于请求页之外）、以及 double 正负极值与响应 64 KiB 上限。
- `clang-format --dry-run --Werror`、`git diff --check` 通过；构建/测试日志、独立复核、源码与二进制快照及 SHA-256 见 `build-codex-agent/agent-phase6-slice67-evidence/`。

### 边界

Slice67 只解决 Agent 无法读取 DataContract 已接受的两种 Qt 点类型；不判断像素/世界/物理坐标系，不做来源映射，也不为 `Point3D`、`Line2D`、`Plane3D` 或点云推导坐标单位。G6 标准 3 和阶段 6 继续 **IN_PROGRESS**。

## Slice 68：Region2D polygon 有界分页与来源顶点（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**（含最后空页来源对象清除增量）。新增 `polygonOffset`/`polygonLimit` 仅用于直接 `Region2D` 输出；不带新参数时保持旧响应。每页给出原顺序局部像素点，且仅在规范 UUID 有效、整页每点都可映射时给来源点。非法参数返回 `INVALID_PARAM`；非有限局部点返回 `UNSUPPORTED_OUTPUT_TYPE`；horizon 页面保留完整局部点、整体省略来源对象。64 KiB 缩页同步截短两份点数组。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**。定向 CTest **2/2 suites 通过**：`test_agentactor` **70/70**、`test_toolschema` **42/42**。
- 覆盖两页及空页、分页字段、来源逐点矩阵映射、无 provenance/路径式 UUID/混合可映射与 horizon 顶点、非有限点拒绝、非 Region/旧参数混用及非法新参数；1000 个接近 double 极值的点实际触发局部/来源页同步缩页，响应不超过 65,536 bytes。
- `clang-format --dry-run --Werror`、`git diff --check` 通过；构建、复审和测试日志（含修正夹具前日志）、源码/测试/二进制快照与 SHA-256 见 `build-codex-agent/agent-phase6-slice68-evidence/`。

### 边界

Slice68 只提供直接 `Region2D` 的 polygon 局部页与可验证的逐点来源页；不改变预览、mask 导出、嵌套 Region2D 或插件轮廓算法。G6 标准 3 和阶段 6 继续 **IN_PROGRESS**。

## Slice 69：点云预览显式标记坐标语义未知（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**。成功的 PointCloudData 样本预览现在显式包含 `coordinateSystem:"unspecified"`，`get_module_output` 说明也禁止据此推断 world frame、物理单位或图像来源映射。原有摘要、点值、采样顺序、64 点上限及 NaN/Inf 可用性行为保持不变。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**。定向 CTest **2/2 suites 通过**：`test_agentactor` **70/70**、`test_toolschema` **42/42**。
- 同模块有效 UUID 和非 identity 图像 provenance 下，回归验证摘要/预览坐标语义仍是 `unspecified` 且不含 `sourceImageCoordinates`/`sourceCoordinates`；65 点输入仍只采样前 64 点且顺序/计数不变。样本内 NaN 返回既有 `previewUnavailable`，仅第 65 点为 Inf 时预览仍有效。
- `clang-format --dry-run --Werror`、`git diff --check` 通过；构建/测试日志、独立复核、源码/测试/二进制快照及 SHA-256 见 `build-codex-agent/agent-phase6-slice69-evidence/`。

### 边界

Slice69 只澄清预览点值坐标语义未知；不提供 3D 坐标框架/单位元数据，不修改点云数据或算法，也不完成 3D 坐标审计。G6 标准 3 和阶段 6 保持 **IN_PROGRESS**。

## Slice 70：Point3D/Plane3D 不套用图像来源矩阵（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**。在同模块图像具有有效 UUID 与非 identity provenance 时，Agent 对 3 数值 Point3D 与 9 数值 Plane3D 列表仍原样返回，不加图像来源/坐标声明。切片只锁定序列化边界，不改生产代码。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；`test_agentactor` CTest **1/1 suite 通过**，QtTest **70 passed / 0 failed / 0 skipped**。
- 真实 `get_module_output` 查询逐元素比较 Point3D/Plane3D 原数组，并检查不存在 `sourceImageCoordinates`、`sourceCoordinates` 或 `coordinateSystem`。
- `clang-format --dry-run --Werror`、`git diff --check` 通过；构建/测试日志、独立复核、测试源码/二进制快照及 SHA-256 见 `build-codex-agent/agent-phase6-slice70-evidence/`。

### 边界

Slice70 不统一 Point3D/Plane3D 的世界框架或单位，不证明任何具体 3D 插件的坐标单位，也不完成 G6 标准 3 或阶段 6 总体验收；两者继续 **IN_PROGRESS**。

## Slice 71：Matching 绘图输出保留检测来源坐标（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 **CONTRACT_PASS**；独立实现复核 `/root/slice54_contract_review_round2` **IMPLEMENTATION_PASS**。测试运行真实 `MatchingPlugin`，用带有效 UUID 和非 identity provenance 的合成图匹配并绘制结果；输出尺寸不变，图像来源 ID/尺寸/矩阵及左上、右下点映射保持。`DetectionList` 首框的局部 x/y/width/height/score 对应 scalar 输出、label 为 `Match 1`，来源 bbox 按相同矩阵映射。没有改生产代码。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；`test_imageplugins` CTest **1/1 suite 通过**，QtTest **11 passed / 0 failed / 0 skipped**。
- `clang-format --dry-run --Werror`、`git diff --check` 通过；构建、测试、独立复核及测试源码/二进制快照和 SHA-256 见 `build-codex-agent/agent-phase6-slice71-evidence/`。

### 边界

Slice71 只验证 Matching 绘图叠加不改变该插件的像素网格及 DetectionList 局部 bbox；不覆盖其它 detection/image plugins，不表示所有输出都有完整 provenance，也不标记 G6 标准 3 或阶段 6 PASS。

## Slice 72：FindCircle 绘图输出保持局部圆坐标及来源网格（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），独立契约复审 `/root/slice54_contract_review_round2` **CONTRACT_PASS**；独立实现复核 **IMPLEMENTATION_PASS**。测试执行真实 `FindCirclePlugin`，对合成圆输入附加 x=1.5/y=2.0 的非相似仿射 provenance。验证绘图后输出尺寸、来源 UUID/尺寸/矩阵及两个局部点映射保持；圆心与半径仍是局部像素数值，Circle2D 与标量输出相等，不增加来源半径字段。没有修改插件生产代码。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；`test_findcircle` CTest **1/1 suite 通过**，QtTest **7 passed / 0 failed / 0 skipped**。
- `clang-format --dry-run --Werror tests/test_findcircle.cpp` 与 `git diff --check` 通过；构建、测试、独立复核、源码/契约/验收快照、相关二进制及 SHA-256 见 `build-codex-agent/agent-phase6-slice72-evidence/`。
- 初次构建因测试将 `cv::Mat` 误用为具有 `width/height` 的对象而失败，修正为 `cols/rows` 后完整重建通过；该首轮失败日志保留在 evidence。直接启动 QtTest 时缺少显示服务器，使用 `QT_QPA_PLATFORM=offscreen` 后 7 项测试通过；CTest 环境下目标测试亦通过。

### 边界

Slice72 只证明 FindCircle 当前同尺寸绘图输出保留输入图像网格 provenance，并证明 Circle2D 圆心和半径仍是插件局部像素值；不推导非相似变换后的来源圆半径，不覆盖其它插件或 G6 标准 3 全部内容。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 73：Blob 最大目标输出带来源可映射检测框（PASS，限定本切片）

契约：本地逐切片契约（未纳入提交），经独立契约复核 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，独立实现复核 **IMPLEMENTATION_PASS**。`BlobPlugin` 新增 `largest_blob: DetectionList`，只包含既有面积最大 contour 的 `cv::boundingRect()`；label 为 `largest_blob`，score 等于原圆度 scalar。Agent 使用新增输出进行真实 GrabImage→非零原点 CropImage→Blob 流程并读取来源 bbox；原 scalar、筛选、最大目标规则及图像处理不变。

### 验证

- 全量 `cmake --build build-codex-agent -j2` **退出码 0**；定向 CTest `test_acceptance_flows`、`test_plugincontracts`、`test_agentactor` **3/3 suites 通过**。
- 双方形合成图回归断言两目标均检测、Agent 返回唯一框恰为较大方形局部 bbox `(40,40,41,41)`，中心仍为 `(60,60)`，score 与既有 circularity 相同；非零裁剪原点下来源 UUID、`source_image_pixels` 标记和来源 bbox 与裁剪 provenance 精确映射结果相符，且来源原点与局部原点不同。
- `clang-format --dry-run --Werror` 与 `git diff --check` 通过；构建/测试/独立复核、源码/契约/验收快照、Blob 测试二进制及 SHA-256 见 `build-codex-agent/agent-phase6-slice73-evidence/`。

### 边界

Slice73 只导出最大 Blob 的轴对齐外接矩形，不代表精确轮廓或其它 Blob 框，不推导来源物理坐标。它不关闭其余图像空间插件/3D 算法坐标审计，也不标记 G6 标准 3 或阶段 6 PASS。

## Slice 74：MeasureCircle 强类型圆输出与来源圆心映射（PASS，限定本切片）

契约 `本地逐切片契约（未纳入提交）` 经独立复核 **CONTRACT_PASS**；最终实现经独立复核 **IMPLEMENTATION_PASS**。`MeasureCirclePlugin` 成功时新增 `circle: Circle2D` 命名数据，centerX/centerY/radius 与既有三个标量完全一致，score 使用既有 `circle_roundness`；metadata 声明 `Circle2D`。没有改动拟合算法、旧标量或 Agent 通用序列化。

新增 `testAgentMeasureCircleMapsCenterThroughCropOrigin`，执行真实动态插件 GrabImage→CropImage→MeasureCircle 并通过 Agent 查询。对合成圆的局部圆心真值误差小于 5 px，强类型圆心/半径/score 与 scalar 精确相等。Agent JSON 根级标记 `image_pixels`，来源对象包含匹配 GrabImage source ID 的 `source_image_pixels` 圆心，精确等于 crop provenance 对实际拟合圆心的映射；根和来源对象均不提供来源半径。Agent 旧 scalar key 仍可查且返回相同数值；metadata 输出类型断言通过。

验证：`cmake --build build-codex-agent -j2` exit 0；定向 `test_acceptance_flows`、`test_plugincontracts` CTest 2/2，新增用例 QtTest 3/3（含 init/cleanup）。全量 CTest **107/108 suites 通过**；唯一失败套件 `test_mainwindow` 重跑仍有两项既有断言失败：Agent 输入错误路径 `errorSpy` 实际 0/预期 1（`tests/test_mainwindow.cpp:9801`）和深色主题样式表不符（`:4413`）。两项单独运行均通过；全量其它套件通过。格式与 `git diff --check` 干净；原始构建、全量/定向 CTest、独立复跑、审核与哈希见 `build-codex-agent/agent-phase6-slice74-evidence/`。

边界：只补 MeasureCircle 的 typed 圆输出与裁剪后圆心映射；半径仍是 MeasureCircle 局部像素单位，不推导非相似变换后的来源半径。MeasureLine、MeasureRect、其它检测算法和具体 3D 坐标框架仍需分别审计；G6 标准 3 与阶段 6 继续 `IN_PROGRESS`。

## Slice 75：MeasureLine 端点强类型输出与来源坐标映射（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经独立契约复核 **CONTRACT_PASS**，最终实现经 `/root/slice54_contract_review_round2` 独立复核 **IMPLEMENTATION_PASS**。`MeasureLinePlugin` 新增 `line=[col1,row1,col2,row2]` 命名输出并在 metadata 声明 `Line2D`；Agent 只对 metadata 明确声明的 Line2D 输出生成带 `image_pixels` 的起终点对象。两端来源变换均通过 provenance horizon guard 且齐次分母同号时，才添加 `source_image_pixels` 端点；越过或接近 horizon 时保留完整局部结果并省略来源对象。算法与原有标量输出不变。

验收包括真实动态 GrabImage→非零原点 CropImage→MeasureLine 流程及 Agent 查询，验证局部端点与旧标量一致、来源端点与 provenance 映射一致、来源 ID 与 GrabImage 一致和 metadata 类型。边界测试覆盖未标注的同形状普通数组、每个坐标位置的数字字符串/布尔/NaN/Inf、错误长度、跨 horizon 和 near-horizon；失败不带 `value`，horizon 情况完整保留局部起终点。

验证：全量构建 `cmake --build build-codex-agent -j2` **退出码 0**；定向 `test_acceptance_flows` CTest **1/1 suite 通过**（新增切片用例与既有验收用例均通过）；`clang-format --dry-run --Werror` 与 `git diff --check` 通过。受限沙箱全量 CTest 有 14 suites 因本地 listener 权限限制失败；在允许本地 listener 的复跑中 **107/108 suites 通过**，唯一失败套件 `test_mainwindow` 内有两项断言失败：`testAgentInputErrorPathDoesNotCrashOrStayThinking` 的 `errorSpy` 计数，以及 `testDarkThemeCloseAndDestroyWindowDirectly` 的样式表比较。之后在允许 listener 的环境中单独运行这两项，均通过，因此全量套件仍不记 PASS。两次全量日志、单项复跑、构建/聚焦测试输出、独立审核和 SHA-256 见 `build-codex-agent/agent-phase6-slice75-evidence/`。

边界：只关闭 MeasureLine 的端点坐标语义，不覆盖 MeasureRect、其它图像空间输出或 3D 算法；不推导来源线段长度。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 76：MeasureRect Region2D 顶点与来源坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经独立复核 **CONTRACT_PASS**，最终实现经 `/root/slice54_contract_review_round2` 复核 **IMPLEMENTATION_PASS**。`MeasureRectPlugin` 成功时新增 `rectangle: Region2D`，polygon 保存 `cv::RotatedRect::points()` 原始循环四角，bbox 从四点极值计算；metadata 声明 `Region2D`。检测/筛选、角点排序和所有既有 scalar 代码均保留。

动态插件验收使用 110×60、27° 的旋转矩形，经 GrabImage→非零原点 CropImage→MeasureRect，再由 Agent 查询 Region2D 和四点 polygon page。测试用已知父图几何独立生成真值，按 crop 原点计算局部真值；四点循环匹配误差 ≤6 px，独立重算 bbox 四分量，来源 page 的每个点与父图真值和 `mapLocalToSource()` 对照，核对 source ID/坐标系。另有无 provenance 图像 fixture：真实动态 MeasureRect 仍经 Agent 查询，完整保留局部区域与 polygon page、无 source 字段。同一实例成功后把相同输入文件替换为空图再运行，第二个 runId 失败且不泄漏上次 rectangle/polygon。

旧 width/height/area/angle 仍有独立兼容基线（OpenCV 4.8.1，容差见契约），不以理想矩形尺寸改写历史排序语义。全量构建 **退出码 0**；定向 `test_acceptance_flows` **1/1 suite 通过**；允许 localhost listener 的全量 CTest **107/108 suites 通过**，唯一失败 `test_mainwindow` 中两项原有断言失败（Agent 输入错误通知、深色主题 stylesheet）；两项随后隔离重跑通过，因此全量不记 PASS。格式、`git diff --check` 干净；日志、两轮全量记录、独立复核和 SHA-256 见 `build-codex-agent/agent-phase6-slice76-evidence/`。

边界：Slice76 只补 MeasureRect 区域四顶点及其图像来源页，不修复该插件既有 scalar 角点排序算法、不将来源 bbox 当成透视变换后的精确几何。其它 ROI/测量插件、G6 标准 3 全覆盖和阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 77：MeasurementInput 点集坐标语义与来源映射（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 已经独立复核 **CONTRACT_PASS**。本切片只在 `get_module_output` 使用 `port:"fit_points"`、模块为 `MeasurementInput` 且同次运行 `coordinate_frame=ImageLocalPixel` 时，在响应顶层增加 `type:PointSet2D` 和 `coordinateSystem:image_pixels`；有效 provenance 与安全 UUID 存在时再增加逐页对齐的 `sourceImageCoordinates.points`。原 `value` 二维数组、顺序及分页语义保持不变；`key:"fit_points"`、非目标模块、非图像坐标框架、无来源和不安全来源 ID 均不获来源坐标标记。

测试使用真实动态 MeasurementInput 插件，经非零原点裁剪验证局部值、来源 UUID、逐点映射和插件 `fit_points_source` 一致；覆盖分页、同次运行透视 horizon 的 null 对齐、无 provenance、无效 UUID、1024 点导致 64 KiB 响应限长后局部/来源页同步缩短、其它模块 PointSet2D 与 World3D 门控负例，以及非有限点集返回 `UNSUPPORTED_OUTPUT_TYPE` 且不返回局部部分值。限长测试核对缩短页为插件点集的同序前缀。

验证：全量构建 **退出码 0**；定向 `test_acceptance_flows` 与 `test_agentactor` **2/2 通过**；格式门禁及 `git diff --check` 通过。独立契约复核 **CONTRACT_PASS**、独立实现复核 **IMPLEMENTATION_PASS**。全量 CTest **94/108 suites 通过、14 项失败**：13 个 suite 因受限环境无法创建 localhost socket/listener（handoff、通信/PLC/TCP、AgentBridge、SAM 与复现套件）；`test_mainwindow` 有 3 项断言失败（Agent 错误通知计数、部分失败摘要计数、深色主题 stylesheet），三项随后逐项隔离运行均通过，显示全量运行存在跨测试/环境交互；因此全量套件仍不记 PASS。完整日志和隔离复跑见 `build-codex-agent/agent-phase6-slice77-evidence/`。

边界：只为 MeasurementInput 已有 `PointSet2D` 图像局部点集查询补充明确坐标语义和来源页；不推断其它点集/标量/三维坐标，不更改插件算法、局部点值或磁盘格式。G6 标准 3 和阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 78：FitCircle 强类型圆输出及来源圆心（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 独立契约复审 **CONTRACT_PASS**；实现经 `/root/slice54_contract_review_round2` 独立复核 **IMPLEMENTATION_PASS**。`FitCircle` 仅在输入明确声明 `coordinate_frame=ImageLocalPixel` 时新增 `circle: Circle2D`，其圆心/半径与原有标量完全相同，score 固定为 0。输入框架缺失或非图像局部时，不把上游 `circle` 透传；这一清理在任何失败返回前执行。Agent 仅映射来源圆心，不推导来源半径。

验收使用动态 `GrabImage → CropImage(非零原点) → MeasurementInput(point_set) → FitCircle`，覆盖局部和来源圆心、旧标量兼容、无 provenance、缺少坐标框架、`World3D`、上游 sentinel 清理、同一插件实例成功后失败及错误框架下失败清理。所有切片检查通过。

验证：全量构建退出码 **0**；聚焦 `test_acceptance_flows` 与 `test_agentactor` **2/2 suites 通过**；`clang-format --dry-run --Werror` 与 `git diff --check` 通过。全量 CTest 为 **94/108 suites 通过，14 项失败**：13 项在本环境无法监听 localhost socket（设置/通信、PLC/TCP、Bridge、SAM 与复现套件）；`test_mainwindow` 有 3 项断言失败（Agent 错误通知、部分失败摘要、深色主题 stylesheet），与 Slice77 同类，其中本次全量运行没有重新逐项隔离。故全量不记 PASS。构建、聚焦与全量测试、格式、复核和哈希证据存于 `build-codex-agent/agent-phase6-slice78-evidence/`。

边界：Slice78 只为 `FitCircle` 明确为图像局部像素的拟合结果补强类型与来源圆心映射；不改变拟合算法或来源半径，不覆盖其它拟合插件。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 79：FitLine 强类型 Line2D 输出与来源端点（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 独立复核 **CONTRACT_PASS**；实现经 `/root/slice54_contract_review_round2` 独立复核 **IMPLEMENTATION_PASS**。`FitLine` 在输出继承后先清除同名 `line`，只在输入精确声明 `coordinate_frame=ImageLocalPixel` 且拟合成功时新增 `line: Line2D`，载荷顺序为 `[col1,row1,col2,row2]`。旧 scalar、算法和参数保持原样。Agent 来源映射复用 Line2D 序列化器；跨 horizon 只省略来源对象，保留局部端点。

真实动态验收执行 GrabImage→非零原点 CropImage→MeasurementInput(point_set)→FitLine。端点同时对照既有 scalars、独立局部真值 `(-75,25)/(125,25)` 和独立父图真值 `(-25,65)/(175,65)`（允许端点换序）；来源 UUID 直接核对 GrabImage。另覆盖无 provenance 局部结果、缺失坐标框架与 `World3D` 上游 sentinel 清理、同实例成功后不足点失败且失败输出无 `line`，以及跨 horizon 时逐端点 scalar 保持。

验证：全量构建退出码 **0**；聚焦 `test_fitline`、`test_agentactor`、`test_acceptance_flows` **3/3 suites 通过**；格式门禁及 `git diff --check` 通过。最终全量 CTest 为 **94/108 suites 通过、14 项失败**：13 项因受限运行环境无法绑定 localhost listener/socket（settings、通信/PLC/TCP、Bridge、SAM、复现套件）；`test_mainwindow` 有 3 项断言失败（Agent 错误通知、部分失败摘要、深色主题 stylesheet）。全量不记 PASS。日志、独立复核、源码/测试/工程 fixture/相关二进制哈希见 `build-codex-agent/agent-phase6-slice79-evidence/`。

边界：Slice79 仅声明 FitLine 成功输出端点为图像局部像素并映射来源端点；不推导来源长度或角度，也不声明任意 PointSet2D 都是图像坐标。G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 80：FitEllipse Ellipse2D 契约与 Agent 坐标语义（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 独立复核 **CONTRACT_PASS**；实现由 `/root/slice54_contract_review_round2` 独立复核 **IMPLEMENTATION_PASS**。核心新增强类型 `Ellipse2D`，严格限定 finite center/half-axes/angle、有序正半轴和 `本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 `CONTRACT_PASS`；实现复核 `IMPLEMENTATION_PASS`。Agent 仅对 `FitPlane` metadata 声明的 `plane: Plane3D` **port** 查询附加顶层 `type=Plane3D`、`coordinateSystem=height_map_local_scaled_xyz` 和 `unitBasis` 参数名列表，原 `value` 保持九个数及顺序。该路径校验恰好九个有限数；非法长度、NaN/Inf 返回 `UNSUPPORTED_OUTPUT_TYPE`，`offset`/`limit` 返回 `INVALID_PARAM`。`key:"plane"` 与其他模块 Plane3D 保留通用语义。ToolSchema 说明公式及单位未知边界；FitPlane 参数标签说明标定比例而不臆造具体单位。拟合算法、标量和数值未改。

端到端回归执行真实 FitPlane 插件，合成高度图满足 `height=3*column+4*row+10`，使用 `pixelSizeX=0.5`、`pixelSizeY=2`、`zScale=0.25`，输入带有效非 identity 平移 provenance。测试独立验证三组平面点均满足 `Z=1.5*X+0.5*Y+2.5`，Agent 九值与 RunEngine 输出逐项相同，且不应用二维来源变换；核对六个旧 scalar key、key 查询、另一模块 Plane3D、分页拒绝及 8/10 项、NaN/Inf 失败关闭。ToolSchema 测试核实 OpenAI function payload 的公式、缩放参数名与单位说明。

验证：`cmake --build build-codex-agent -j2` 退出码 **0**；聚焦 `test_plugincontracts`、`test_3d_batch3`、`test_toolschema`、`test_agentactor`、`test_acceptance_flows` **5/5 suites 通过**；clang-format、JSON 解析和 `git diff --check` 通过。全量 CTest **94/108 suites 通过，14 suites 失败**：13 suites 因当前受限沙箱无法绑定 localhost listener/socket；`test_mainwindow` 有三项断言失败（Agent 错误通知、部分失败摘要、深色主题 stylesheet）。全量日志和退出码保存在 `build-codex-agent/agent-phase6-slice81-evidence/ctest-full.log`；契约、复核和 SHA-256 证据同目录。

边界：本切片只定义 FitPlane 输出点的高度图局部索引缩放坐标，不声明 mm、世界坐标或裁剪后来源图像坐标。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 82：SamPredict Region2D 非零裁剪后的来源坐标集成（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 `CONTRACT_PASS`；实现复核 `IMPLEMENTATION_PASS`。测试新增 CropImage 到 `test_sam_region_flow` 的真实动态插件依赖，生产代码与通用 Region2D serializer 未改。

真实流程 `GrabImage(640×480) → CropImage → SamPredict` 使用裁剪中心 `(280,220)`、半长 `(200,150)`，固定原点 `(80,70)` 和输出 `(400×300)`。同线程 `SamTestServer` 返回局部 bbox `[10,10,10,10]`、score `0.97` 和四点 polygon。测试确认 CropImage 与服务端解码的 `/set_image` 文件均为 400×300；crop provenance UUID 与 GrabImage 一致，矩阵为 `(80,70)` 平移。Agent 同 run 查询保留局部 Region2D；来源 bbox 精确 `[90,80,10,10]`，来源 polygon 为 `[(90,80),(100,80),(100,90),(90,90)]`，并与 Crop provenance 的 `mapRectToSourceBounds`、`mapLocalToSource` 输出交叉核对。回读前后 server 请求数相同。

验证：全量构建退出码 **0**；完整 `test_sam_region_flow` QtTest **17 passed / 0 failed**，CTest **1/1 suite 通过**（本地 fake server 在允许 localhost 的验证运行中执行）；clang-format 与 `git diff --check` 通过。允许 localhost 的全量 CTest 为 **107/108 suites 通过**；唯一失败套件 `test_mainwindow` 有两项断言失败（Agent 输入错误路径与深色主题销毁窗口 stylesheet），其余 107 套件均通过。日志、独立复核与 SHA-256 见 `build-codex-agent/agent-phase6-slice82-evidence/`。

边界：只证明 SamPredict 的这条成功 Region2D 输出在固定非零平移裁剪后能映射回父图；不评估真实 SAM 模型质量、会话保存、mask 全量导出、其他 detection 插件或 G6 全部操作。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 83：ColorRecognition 中心点来源坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经独立复核 **CONTRACT_PASS**，实现经 `/root/slice54_contract_review_round2` 独立复核 **IMPLEMENTATION_PASS**。`ColorRecognition` 保留 `color_center_x/y` 两个兼容标量，新增 metadata `Point2D` 端口 `color_center`，载荷为与旧标量一致的 `QPointF`。Agent 仅在稳定插件 ID `com.deeplux.plugin.colorrecognition` 和 `color_center` Point2D 端口同时匹配时返回 `image_pixels`，并在安全 provenance 可映射时返回来源点；普通 QPointF/Point2D 输出不获取此语义。

真实动态 `GrabImage → CropImage → ColorRecognition` 使用 320×240 黑图及闭区间红色矩形 `[110,150]×[90,130]`，CropImage 中心 `(180,130)`、半长 `(100,70)`、角度 0；验证裁剪原点 `(80,60)`、尺寸 `200×140`，裁剪局部中心 `(50,50)`，来源中心 `(130,110)`。旧标量与 typed 点相等，来源 UUID 与 GrabImage 一致，并由 provenance 映射函数交叉核对。另覆盖无 provenance、有效投影 provenance 的 horizon 映射失败、非有限点拒绝 (`UNSUPPORTED_OUTPUT_TYPE`)、`offset`/`limit` 拒绝 (`INVALID_PARAM`)，以及其它模块 Point2D 仍不添加图像坐标语义。

验证：全量构建退出码 **0**；`test_acceptance_flows`、`test_remainingplugins`、`test_agentactor`、`test_toolschema` **4/4 suites 通过**；格式门禁及 `git diff --check` 通过。最终全量 CTest 为 **107/108 suites 通过**，唯一失败套件 `test_mainwindow` 内两项断言（Agent 输入错误通知、深色主题 stylesheet）；分别隔离运行两项均通过，且全量其余套件包括本切片 acceptance 测试通过。全量日志、两项隔离日志、构建日志、独立复核与 SHA-256 见 `build-codex-agent/agent-phase6-slice83-evidence/`。

边界：Slice83 只关闭 ColorRecognition 中心点的图像局部/来源坐标路径；不扩展颜色轮廓几何、不改检测算法或兼容标量，不覆盖其它检测插件。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 84：JiErHanDefectsDet 首个缺陷框来源坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经独立复核 **CONTRACT_PASS**，实现经 `/root/slice54_contract_review_round2` 独立复核 **IMPLEMENTATION_PASS**。`JiErHanDefectsDet` 新增 metadata 声明的 `defect_region: Region2D`，描述首个保留候选的局部 bbox 与 confidence，旧标量、候选顺序、阈值、算法及绘图保持不变；Agent 复用通用 Region2D 来源映射。

真实动态 `GrabImage → CropImage → JiErHanDefectsDet` 使用 400×300 灰底单矩形图像，Crop 原点 `(40,30)`、输出 320×240。当前 OpenCV 4.8.1 构建得到局部 bbox `[59,49,61,61]`、score `1.0`，来源 bbox `[99,79,61,61]`。测试核对旧标量与 Region2D 一致、来源 UUID/映射一致；无 provenance 时仍保留局部 bbox 与 score 且不输出来源坐标。该精确 Canny/轮廓 bbox 是本环境基线，OpenCV 未锁版本，升级后需复核该数值。

验证：全量构建退出码 **0**；`test_acceptance_flows`、`test_remainingplugins`、`test_agentactor`、`test_toolschema` **4/4 suites 通过**；单项 acceptance QtTest 通过；格式门禁与 `git diff --check` 通过。全量 CTest **107/108 suites 通过**，唯一失败套件 `test_mainwindow` 中 Agent 输入错误路径和深色主题 stylesheet 两项断言；两项各自隔离运行均通过。日志、隔离复跑、独立复核与哈希见 `build-codex-agent/agent-phase6-slice84-evidence/`。

边界：本切片只为该插件首个候选框增加 typed 几何与来源坐标，不验证启发式缺陷检测质量，也不覆盖其他候选框或插件。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 85：MeasurementInput point_pair 二维点来源坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**；实现经同一审核者独立复核 **IMPLEMENTATION_PASS**。Agent 只在 MeasurementInput 稳定插件 ID、metadata 的 `Point2D` `point1`/`point2` port 与运行输出模式 `point_pair` 同时匹配时，将恰好两个有限数值序列化为 `Point2D` `image_pixels`，并复用现有安全 provenance 点映射。普通 key、`custom` 模式和非目标模块的同名 `point1` 保持通用列表语义。原子点拒绝五种分页参数；错误类型/维数/非数字/非有限值以 `UNSUPPORTED_OUTPUT_TYPE` 失败关闭。

真实动态 `GrabImage → CropImage → MeasurementInput(point_pair)` 在 160×120 图像上裁剪出原点 `(50,40)`、尺寸 60×40；局部点 `(1.25,2.5)`、`(7,8)` 映射到来源点 `(51.25,42.5)`、`(57,48)`，Agent 响应逐字段对照固定真值、来源 UUID 与 `mapLocalToSource()`。另覆盖无 provenance、固定投影 horizon、二维 point_pair 下五种分页拒绝、`custom` 模式、普通 key、非目标模块同名 Point2D `point1`，以及 malformed output 注入回归。

三坐标边界：真实 MeasurementInput 的参数解析可以接受三值，但当前 metadata `Point2D` 的运行输出门禁会令真实三值运行以 `TypeMismatch` 失败；本切片显式验证并保留这一既有行为，不扩大修改端口契约。另用 MeasurementInput 身份和 Point2D metadata、但运行时输出端口设为 Any 的兼容夹具令三值列表到达 Agent，验证它仍使用通用列表且不附加二维来源语义。

验证：全量构建退出码 **0**；新增及相关 Point2D QtTest 用例 **6/6** 通过；`test_acceptance_flows`、`test_remainingplugins`、`test_agentactor`、`test_toolschema` **4/4 suites 通过**；clang-format、JSON 解析及 `git diff --check` 通过。允许本地服务的全量 CTest **107/108 suites 通过**；唯一失败套件 `test_mainwindow` 中 Agent 输入错误通知和深色主题 stylesheet 两项断言，最终二进制下分别隔离运行均通过。构建、定向/全量测试、隔离复跑、独立复核与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice85-evidence/`。

边界：Slice85 只为二维 `point_pair` port 查询补充图像坐标语义；不改变 MeasurementInput 的参数解析或 3D 端口契约，不覆盖其它模式、测量插件或 G6 标准 3 的其余 3D 审计。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 86：DistancePL 垂足图像局部与来源坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**；实现经同一审核者独立复核 **IMPLEMENTATION_PASS**。`DistancePL` 保留原 distance 和 `foot_x/foot_y` 结果；仅输入 `coordinate_frame == ImageLocalPixel` 时新增 `foot_point: QPointF`，处理开始先移除继承的旧同名值。Agent 仅在稳定插件 ID、metadata 声明的 Point2D port 和相同图像坐标框架同时成立时输出 `Point2D/image_pixels`，安全 provenance 可映射时附加来源点。非有限值失败关闭；key 查询、非目标同名 port 不取得该坐标语义。

真实动态流程固定为 `GrabImage → CropImage → MeasurementInput(point_line) → DistancePL`，且 `MeasurementInput.image/point/line` 分别连接到 DistancePL 同名输入，使 image 上的 provenance 与 `ImageLocalPixel` 坐标框架沿流程传递。160×120 来源图裁剪原点 `(50,40)`、尺寸 60×40；点 `(20,10)` 到线段 `(5,30)→(45,30)` 的段内垂足为 `(20,30)`、距离 20，来源垂足 `(70,70)`。typed 输出逐项对照旧 scalar、固定真值、来源 UUID 与 `mapLocalToSource()`。另覆盖无 provenance、投影 horizon、MeasurementInput `custom/Unspecified` 帧不产生垂足、输入 sentinel 清理、非有限 Agent 输出、五种分页拒绝以及非目标模块同名 `foot_point`。

验证：全量构建退出码 **0**；`test_acceptance_flows`、`test_distancepl`、`test_remainingplugins`、`test_agentactor`、`test_toolschema` **5/5 suites 通过**；最终全量 CTest **107/108 suites 通过**，`test_acceptance_flows` 通过。唯一失败套件 `test_mainwindow` 中 Agent 输入错误通知和深色主题 stylesheet 两项断言；最终二进制下分别隔离运行均通过。clang-format、metadata/fixture JSON 解析与 `git diff --check` 通过；构建、聚焦/全量测试、两项隔离复跑、独立复核与哈希证据见 `build-codex-agent/agent-phase6-slice86-evidence/`。

边界：切片只为 DistancePL 当前图像局部输入下的垂足增加 typed 输出及来源映射，不改变插件已有垂足/线段距离算法，不声明物理单位，也不扩大到其它几何插件。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 87：MeasurementInput Line2D 坐标框架门控（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**，实现经同一审核者复核 **IMPLEMENTATION_PASS**。Agent 仅对稳定插件 ID `com.deeplux.plugin.measurementinput`、metadata 声明的 `line`/`line1`/`line2:Line2D` 且同次输出 `coordinate_frame=ImageLocalPixel` 时使用既有 Line2D 序列化与安全 provenance 映射。其他坐标框架（本切片覆盖 `custom/Unspecified`）保留原始列表，不增加 Line2D 或图像/来源坐标语义。`port` 和普通 `key` 路径一致；目标输出在两路径均拒绝 `offset`、`limit`、`maskOffset`、`polygonOffset`、`polygonLimit`。FitLine、MeasureLine 和其他 Line2D 生产者未改。

真实动态 `GrabImage → CropImage → MeasurementInput` 使用 160×120 图像、裁剪原点 `(50,40)`、尺寸 60×40。`point_line` 的 `line=[5,30,45,30]` 和 `line_pair` 的两线分别对照固定局部端点及父图端点 `(55,70)→(95,70)`、`(60,45)→(60,75)`；port/key 响应均核对 `image_pixels`、来源 UUID 和 `mapLocalToSource()`。`custom` 模式确认继承的有效 provenance 不会替 `Unspecified` 数据赋予图像坐标，三个四数列表的 port/key 响应保持原数组，并逐项验证五种分页参数都以 `INVALID_PARAM` 失败且不返回 `value`。

验证：全量构建退出码 **0**；`test_acceptance_flows`、`test_agentactor`、`test_toolschema`、`test_measureline`、`test_fitline` **5/5 suites 通过**，新验收 QtTest 单项通过。clang-format、JSON 解析及 `git diff --check` 通过。允许本机 socket listener 的全量 CTest 为 **107/108 suites 通过**；唯一失败套件 `test_mainwindow` 的 Agent 输入错误与深色主题窗口销毁两项断言，在最终二进制下分别隔离运行均通过。受限沙箱的全量尝试有 14 个环境类失败，均因 listener/socket 被拒；未将其当作产品失败。构建、聚焦/全量测试、隔离复跑、独立复核与哈希证据见 `build-codex-agent/agent-phase6-slice87-evidence/`。

边界：只修正 MeasurementInput 三个既有 Line2D 输出的坐标框架解释，不改变插件输出、参数、辅助来源键或任何测量算法，也不全局重新定义 Line2D。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 88：MeasurementInput point_plane 三维端口语义（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**，实现经同一审核者复核 **IMPLEMENTATION_PASS**。仅当目标稳定插件 ID、metadata 声明的 `point:Any`/`plane:Plane3D` port、同次 `measurement_input_mode=point_plane` 和 `coordinate_frame=World3D` 同时成立，Agent 才分别输出 `Point3D`/`Plane3D` 及 `coordinateFrame=World3D`。point 为恰好三个有限数；plane 为恰好九个有限数，代表顺序排列的三个 XYZ 点。数值必须是白名单 Qt 数值类型；不接受数字字符串或布尔值。原数组与顺序保持不变，不应用图像 provenance、不添加来源坐标。普通 key、其它模式、框架与插件维持既有通用行为。ToolSchema 明确单位、轴定义和外部参考未知，且该标签不证明值来自点云。

真实动态 `GrabImage → CropImage → MeasurementInput(point_plane)` 带有效非 identity 图像 provenance。测试逐项核对 `point=[1.25,-2,30.5]` 与九值平面数组、类型、`coordinateFrame`、port/key 差异和无图像来源字段；`custom/Unspecified` 下两个 port 回退通用列表。语义门控下两个 port 均拒绝五种分页参数。隔离输出夹具覆盖二值 point、point 的 NaN/Inf/数字字符串/bool，以及 plane 长度错误、NaN/Inf/数字字符串/bool；全部以 `UNSUPPORTED_OUTPUT_TYPE` 失败且不返回 `value`。既有 FitPlane 平面输出专属坐标、key 通用语义继续由原验收覆盖。

验证：全量构建退出码 **0**；`test_acceptance_flows`、`test_toolschema`、`test_agentactor`、`test_measurementinput`、`test_3d_batch3` **5/5 suites 通过**；新增两条 QtTest 单项通过。格式、ToolSchema 与 fixture JSON、`git diff --check` 通过。允许本机 socket 的全量 CTest **107/108 suites 通过**；唯一失败 `test_mainwindow` 内 Agent 输入错误与深色主题窗口销毁两项断言，Slice88 最终二进制下分别隔离运行均通过。构建、定向/全量测试、隔离复跑、独立复核与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice88-evidence/`。

边界：Slice88 只给 MeasurementInput `point_plane` 的两个 port 补充三维形状和该插件的框架标签，不改变输入解析或几何算法，不宣称物理单位/全局基准，不建立点云来源链路。G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 89：PointSurfaceDistance World3D 垂足输出（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现经同一审核者 **IMPLEMENTATION_PASS**。`PointSurfaceDistance` 新增 metadata 输出 `foot_point:Point3D`；每次执行先清除继承值，且仅当输入框架为 `World3D`、平面法向长度超过既有阈值、垂足三坐标均有限时写入。Agent port 查询再核验稳定插件 ID、metadata 声明、三值白名单数值和有限性，返回 `Point3D/World3D`；不映射图像 provenance、不宣称单位。普通 key 仍返回列表；五个分页参数在 typed port 上拒绝。既有距离及 `foot_x/y/z` 不变。

真实动态流程 `GrabImage → CropImage → MeasurementInput(point_plane) → PointSurfaceDistance` 带非 identity 裁剪 provenance，核对距离 7、垂足 `(1,2,3)`、旧 scalar 与新 port 一致，响应不含图像坐标或单位字段。缺失、`Unspecified`、`ImageLocalPixel` 框架均携带旧垂足 sentinel 通过运行，输出已清除且 port 返回 `OUTPUT_NOT_FOUND`。同稳定插件 ID 的运行时 `Any` fixture 覆盖错误长度、字符串、bool、NaN、Inf，Agent 均以 `UNSUPPORTED_OUTPUT_TYPE` 拒绝且不含 value。

验证：全量构建退出码 **0**；定向 CTest `test_pointsurfacedistance`、`test_acceptance_flows`、`test_toolschema`、`test_agentactor`、`test_3d_batch3` **5/5 通过**。本机允许 socket listener 的全量 CTest **107/108 通过**；唯一失败为 `test_mainwindow`，其中 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 在整套运行时失败，但分别以新进程单项运行均通过，呈测试状态/顺序敏感。受限沙箱首次全量运行有 14 个 listener/SAM/UI 环境类失败；本机复跑中这些均通过。格式门禁、metadata/fixture JSON 解析、`git diff --check` 通过。完整构建、测试、隔离复跑、复审及哈希证据见 `build-codex-agent/agent-phase6-slice89-evidence/`。

边界：本切片只增加该插件垂足的受框架约束 typed 输出，不改几何算法，不把图像 provenance 转成 3D 基准，也不说明点值的单位或外部坐标参考。G6 标准 3 和阶段 6 总体继续 `IN_PROGRESS`。

## Slice 90：GapMeasure3D 水平测量单位透传（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现经同一审核者 **IMPLEMENTATION_PASS**。`GapMeasure3D` metadata 仅为 `gap_width`、`gap_offset_width`、`corner_dx_mm` 补 `unit:"mm"`，单位来源分别是 `pixelSizeX` 与 `offsetMm` 参数；不改算法和值。Agent ToolSchema 要求先检查 `gap_found`：未检出时三个字段均为配置的 `measureFailValue` 哨兵，静态单位不能当作有效测量声明。`corner_dz_mm`、`corner_dist_mm` 不增加单位，未推断 `zScale` 的物理含义。

真实 TIFF 高度图流程检出槽宽，设置 `pixelSizeX=0.5`、`offsetMm=0.25`，验证宽约 20 mm、偏移宽约 20.5 mm、`corner_dx_mm==gap_width`。三个字段的 port/key 查询均带 `unit:"mm"`，值逐项等于同次 RunEngine 输出，不带坐标或图像 provenance 字段；两个 Z 相关字段仍不含 `unit`。另一个真实未检出流程设 `measureFailValue=-3.5`，核对 `gap_found=false` 且三个字段 port/key 仍返回 `-3.5` 和静态 `mm`。

验证：全量构建退出码 **0**；定向 CTest `test_3d_batch3`、`test_toolschema`、`test_pluginparametercontracts` **3/3 通过**。允许本机 socket listener 的全量 CTest **107/108 suites 通过**；唯一失败为 `test_mainwindow` 中 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两项，最终二进制下分别单独运行均通过。受限沙箱首轮造成的 socket/SAM/UI 环境问题使用本机复跑排除。格式门禁、metadata JSON、`git diff --check` 通过。构建、定向/全量回归、单项隔离、复审与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice90-evidence/`。

边界：只为有明确水平毫米依据的三个数值恢复 Agent 单位；不改算法，不宣称 Z 轴单位、角点距离单位或其他插件的坐标/单位语义。G6 标准 3 和阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 91：MeasureCircle 标量像素单位透传（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现经同一审核者 **IMPLEMENTATION_PASS**。`MeasureCircle` metadata 只给 `circle_center_x`、`circle_center_y`、`circle_radius`、`circle_diameter` 四项图像像素标量添加 `unit:"px"`。不改算法、数值或 typed Circle2D 与来源圆心映射。`circle_roundness` 和 `edge_point_count` 不添加单位，不声明毫米、来源半径或物理尺寸。

真实动态 `GrabImage → CropImage → MeasureCircle` 合成已知圆并经非零裁剪；四个标量的 port/key 响应均带 `px`，数值逐项等于同次 RunEngine 输出，不包含坐标/来源映射字段。既有 Circle2D key 响应继续核对局部 `image_pixels` 圆心、来源 UUID 与父图圆心，port/key 响应都无 `unit`；圆度和边缘点数查询也无单位。

验证：全量构建退出码 **0**；定向 CTest `test_acceptance_flows`、`test_agentactor`、`test_toolschema`、`test_pluginparametercontracts` **4/4 通过**。允许本机 socket listener 的全量 CTest **107/108 suites 通过**；唯一失败为 `test_mainwindow` 中 Agent 输入错误与深色主题销毁两个断言，最终二进制下各自单独运行均通过。格式门禁、metadata JSON、`git diff --check` 通过。构建、定向/全量测试、单项隔离、独立复核与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice91-evidence/`。

边界：只标注四个 MeasureCircle 图像局部像素标量；不推导来源图半径或物理尺寸，不更改 G6 标准 3 的其他测量、ROI、点云或 3D 语义。G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 92：MeasureLine 长度与角度单位透传（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现经同一审核者 **IMPLEMENTATION_PASS**。`MeasureLine` metadata 仅为 `line_length` 增加 `unit:"px"`、为 `line_angle` 增加 `unit:"deg"`；不改算法或端点，也不推导来源图线长。Agent 对两个 scalar 的 port/key 查询均核对为同次 RunEngine 的真实值与单位。

沿用真实 `GrabImage → CropImage → MeasureLine` 合成线及非零裁剪来源端点验收。从本次图像局部 `Line2D` 端点计算 `dx=x2-x1`、`dy=y2-y1`（图像 Y 向下），独立按 `hypot(dx,dy)` 计算线长、按 `atan2(dy,dx)` 归一 `本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复核 **CONTRACT_PASS**，实现及补充的同次 RunEngine 值一致性断言经复核 **IMPLEMENTATION_PASS**。仅为现有 `rect_width`、`rect_height`、`rect_area`、`rect_angle` 在 metadata `ui.results` 分别声明 `px`、`px`、`px^2`、`deg`。前三项沿用原有输出 port；角度仅 key 查询，不新增 port。保留 legacy 宽高配对语义，不保证对应真实旋转矩形边长；不改算法、数值、`Region2D` 来源映射或失败运行隔离。

真实 `GrabImage → CropImage → MeasureRect` 合成旋转矩形继续核验局部角点真值及父图来源点映射。三个已有 scalar port 与 key、以及角度 key 的每个响应均验证单位和数值等于同次 RunEngine 输出；scalar 不含来源坐标字段。typed `Region2D` 不带 scalar 单位。失败运行仍不泄漏上一轮输出。

验证：全量构建退出码 **0**；定向 CTest `test_acceptance_flows`、`test_agentactor`、`test_toolschema`、`test_pluginparametercontracts` **4/4 通过**。允许本机 socket 的全量 CTest **107/108 suites 通过**；唯一失败为 `test_mainwindow` 内 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两项上下文断言，最终二进制下隔离运行均通过。clang-format、metadata JSON、`git diff --check` 通过。完整构建、定向/全量 CTest、两项隔离复测、review 与 SHA-256 见 `build-codex-agent/agent-phase6-slice93-evidence/`。

边界：仅标注 MeasureRect 四个现有局部 scalar 的单位，不解释 legacy 宽高为真实旋转边，不推导来源图或物理尺寸；G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 94：Matching 检测框局部像素单位（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**，实现及测试经同一审核者复核 **IMPLEMENTATION_PASS**。Matching metadata 仅为 `match_x`、`match_y`、`match_width`、`match_height` 四个既有匹配框 scalar 增加 `unit:"px"`。`match_score`、`match_count`、`match_threshold` 不声明单位，算法和历史值不变。

真实动态 `GrabImage` 图像夹具（不含有效 `template_qimage`）→ `CropImage` 非零裁剪 → `Matching` 使用唯一中心图案及现有默认模板规则。四项 port/key 查询逐个验证 `status=available`、单位 `px`、值与同次 RunEngine 输出一致，且不带 coordinate/provenance 字段；它们与 `DetectionList` 首框局部 bbox 一致。DetectionList 来源 bbox 仍按 CropImage provenance 映射；分数、数量、阈值三项查询均成功、值一致且不含单位。

验证：全量构建退出码 **0**；定向 CTest `test_acceptance_flows`、`test_agentactor`、`test_toolschema`、`test_pluginparametercontracts`、`test_imageplugins` **5/5 通过**，新验收 QtTest 单项通过。允许本机 socket 的全量 CTest **107/108 suites 通过**；唯一失败仍是 `test_mainwindow` 中 Agent 输入错误和深色主题 stylesheet 两项上下文断言，最终二进制下分别隔离运行均通过。格式门禁、metadata JSON、`git diff --check` 通过。构建、定向/全量回归、隔离复测、独立复核和 SHA-256 见 `build-codex-agent/agent-phase6-slice94-evidence/`。

边界：仅声明 Matching 检测框处于当前输入图像的局部像素网格；父图/来源坐标继续来自 DetectionList 的 provenance 映射，不推导物理尺寸。G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 95：DistancePL 图像坐标距离单位门控（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**，实现经同一审核者复核 **IMPLEMENTATION_PASS**。Agent 只对稳定插件 ID `com.deeplux.plugin.distancepl` 的 `distance` port/key 查询，在同次 RunEngine 输出的 `coordinate_frame` 精确为 `ImageLocalPixel` 时动态添加 `unit:"px"`。不在 metadata 静态声明，其他坐标框架下单位 fail-closed；scalar 不作来源图变换。

沿用真实动态裁剪后的 `MeasurementInput(point_line) → DistancePL` 流程验证 ImageLocalPixel 下 port/key 均返回 `20 px`、与 RunEngine 一致，并保留垂足来源坐标映射。另在具有图像 provenance 的 `MeasurementInput(custom/Unspecified)` 流程验证 port/key 距离仍为 `20`、保持可读但不包含 unit，且既有 typed foot point 不产生断言继续通过。

验证：全量构建退出码 **0**；定向 CTest `test_distancepl`、`test_toolschema`、`test_agentactor`、`test_pluginparametercontracts`、`test_acceptance_flows` **5/5 通过**。允许本机 socket 的全量 CTest **107/108 suites 通过**；唯一失败仍是 `test_mainwindow` 内 Agent 输入错误与深色主题 stylesheet 两项上下文断言，最终二进制下隔离复跑均通过。格式、`git diff --check` 通过。构建、定向/全量测试、隔离复测、独立审核及 SHA-256 见 `build-codex-agent/agent-phase6-slice95-evidence/`。

边界：仅为 ImageLocalPixel 坐标中的 DistancePL scalar 距离声明 px；不为 Unspecified/自定义/世界坐标距离声明单位，不映射来源距离。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 96：DistancePP 坐标框架单位门控（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**，实现经同一审核者复核 **IMPLEMENTATION_PASS**。DistancePP metadata 移除 `distance`、`delta_x`、`delta_y` 的静态 `px` 声明，保留其它结果显示元数据；Agent 仅在 stable plugin ID 匹配、字段为三项之一且同次 RunEngine 输出 `coordinate_frame == ImageLocalPixel` 时动态加 `px`。其它框架不带 unit，数值不变。

真实非零裁剪 `GrabImage → CropImage → MeasurementInput(point_pair) → DistancePP` 显式连接 MeasurementInput 的 image/point1/point2 到 DistancePP 对应输入，验证 DistancePP 输出仍为 ImageLocalPixel，距离 port/key 与差值 key 的单位和值、局部数学真值均通过，原点来源映射保持。另一个带有效图像 provenance 的 `MeasurementInput(custom/Unspecified) → DistancePP` 也显式传 image carrier，验证 DistancePP 输出框架为 Unspecified，三项距离/差值均可读、值正确但无 unit，custom 点仍保留通用数组语义。

验证：全量构建退出码 **0**；定向 CTest `test_distancepp`、`test_toolschema`、`test_agentactor`、`test_pluginparametercontracts`、`test_acceptance_flows` **5/5 通过**。允许本机 socket 的全量 CTest **107/108 suites 通过**；唯一失败仍是 `test_mainwindow` 中 Agent 输入错误与深色主题 stylesheet 两项上下文断言，最终二进制下分别隔离复跑均通过。格式、metadata JSON、`git diff --check` 通过。构建、定向/全量测试、隔离复测、独立审核和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice96-evidence/`。

边界：仅在 DistancePP 的 ImageLocalPixel 输入坐标下为距离与差值标 px；不对自定义/未知/世界坐标声明单位，也不映射来源距离或差值。G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 97：FitLine 平均拟合误差单位门控（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS**，实现经同一审核者复核 **IMPLEMENTATION_PASS**。`line_error` 为 FitLine 输入点到拟合线的平均距离，单位沿用点坐标。Agent 仅在 FitLine 稳定插件 ID、查询字段 `line_error`、且同次输出 `coordinate_frame == ImageLocalPixel` 时动态加 `unit:"px"`；不在 metadata 作静态声明，也不应用来源坐标变换。

真实非零原点裁剪 `GrabImage → CropImage → MeasurementInput(point_set) → FitLine` 既有 Line2D 来源映射保持通过，`line_error` port/key 均带 px 且逐项等于同次 RunEngine 输出。另一组同次执行 fixture 覆盖 `ImageLocalPixel` 正例、坐标框架缺失与 `World3D`：port/key 在后两者下仍可读但不带单位；已有失败清理与 typed Line2D 语义保持通过。

验证：全量构建退出码 **0**；定向 CTest `test_fitline`、`test_acceptance_flows`、`test_agentactor`、`test_toolschema`、`test_pluginparametercontracts` **5/5 通过**。允许本机 socket 的全量 CTest **107/108 suites 通过**；唯一失败仍是 `test_mainwindow` 的 Agent 输入错误与深色主题 stylesheet 两项上下文断言，最终二进制下分别隔离复跑均通过。格式、`git diff --check` 通过。构建、定向/全量回归、隔离复测、独立复核及 SHA-256 证据见 `build-codex-agent/agent-phase6-slice97-evidence/`。

边界：仅为 ImageLocalPixel 输入点集的 FitLine 平均误差标 px；不证明拟合准确度，不对其它输出字段、框架或来源误差声明单位。G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 98：RegionStats 结构化质心（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核为 **CONTRACT_PASS**。RegionStats 新增可选 `image:Image2D` 载体输入与 `centroid:Point2D` 输出，保留既有 `region` 必需输入、`centroid_x/y` 和其他算法/输出。插件运行前清除从输入载体继承的 stale `centroid`，本次成功才写入质心 QPointF。Agent 仅对 RegionStats metadata 声明的 centroid port 输出结构化图像局部点；分页参数拒绝，错误类型/非有限值失败关闭。

来源坐标门控经契约复审收紧：RegionStats 的 `region` 和 `image` 必须各有唯一直接上游，且均来自稳定插件 ID `com.deeplux.plugin.sampredict` 的同一模块实例；同次输出还须有有效 sourceImageId 且质心可映射。其它生产者即使同时输出带 provenance 的 image 和 Region2D，也仅返回局部质心，不伪造来源映射。

验证：契约及实现分别经 `/root/slice54_contract_review_round2` 独立复核为 **CONTRACT_PASS**、**IMPLEMENTATION_PASS**。完整构建退出码 **0**；Slice98 关联 CTest `test_sam_region_flow`、`test_acceptance_flows`、`test_agentactor`、`test_toolschema`、`test_plugincontracts`、`test_pluginparametercontracts` **6/6 通过**。真实非零原点裁剪 `GrabImage → CropImage → SamPredict → RegionStats` 核对新旧质心值及局部 `(15,15)` 到来源 `(95,85)`；普通 `key:"centroid"` 保持 `[15,15]` 与通用 `QPointF` 类型标签、不带结构化坐标或来源字段。旧的无 image carrier 链路仍可返回局部质心、不带来源对象；使用不同 image 上游时不映射；未知 fixture 同时输出错配 Region2D 与带/不带 provenance 的 image，均不生成来源对象；分页参数、错误形态/非有限 Point2D 均拒绝，失败 region 输入也确认清除了继承质心。全量本机 CTest **106/108 suites 通过**：`test_mainwindow` 内两项顺序敏感断言失败，`test_agent_viewport_snapshot_handoff` 在全量运行中 subprocess abort；这两项 MainWindow 测试及 viewport suite 分别隔离运行均通过。格式、metadata JSON 与 `git diff --check` 通过。构建、定向/全量测试、隔离复跑、复审和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice98-evidence/`。

边界：结构化质心是 Region2D 当前坐标网格中的图像局部像素点；来源图映射只对白名单 SamPredict 双端同源连线开放。本切片不改统计算法、不为面积/bbox/其它统计值赋予坐标或单位，也不表示 G6 标准 3 或阶段 6 整体完成。

## Slice 99：LinesDistance 最近点连线坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核为 **CONTRACT_PASS**，实现复核为 **IMPLEMENTATION_PASS**。LinesDistance 新增可选 `nearest_segment:Line2D` 输出，保留旧四个最近点 scalar。Agent 仅对该插件的 port 查询校验四个有限数值，并在三条直接输入边来自同一个 stable `MeasurementInput` 实例且同次输出确认为 `line_pair`/`ImageLocalPixel` 时赋予局部坐标语义；有效 provenance 下映射来源端点。未知生产者、分裂来源或其他模式/框架只返回通用列表。跨投影无穷远线段保留局部线段并省略来源对象；同名 key 保持原有通用列表格式。

验证：全量构建退出码 **0**；定向 CTest `test_acceptance_flows`、`test_linesdistance`、`test_agentactor`、`test_plugincontracts`、`test_toolschema` **5/5 通过**。端到端覆盖非零裁剪来源映射、来源门控、地平线、port/key 兼容、五种分页拒绝、错误形状/字符串/bool/NaN/Inf 序列化拒绝，以及 stale 值清除。全量 CTest **107/108 suites 通过**；唯一失败 suite `test_mainwindow` 内 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两项顺序上下文断言，分别隔离运行均 **3/3 QtTest cases 通过**。其余 107 suites（含 Slice99 验收流）通过。格式、metadata JSON、`git diff --check` 通过；全量构建/测试、隔离复跑、复审与 SHA-256 清单见 `build-codex-agent/agent-phase6-slice99-evidence/`。

边界：仅提供 LinesDistance 最近点线段的坐标类型与受门控来源映射；不为 scalar distance 声明单位，不将 metadata 的 Line2D 类型视作坐标证明，也不表示 G6 标准 3 或阶段 6 整体验收完成。

## Slice 100：FindCircle 裁剪后 Agent 坐标验收（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核为 **CONTRACT_PASS**，实现复核为 **IMPLEMENTATION_PASS**。本切片只新增验收流测试，不修改 FindCircle 算法、metadata 或 Agent 序列化。测试复用固定圆图，在内存工程中构造 `GrabImage → CropImage(中心320,240；半长150,150) → FindCircle`，以真实 `run_flow` 运行；直接检查 CropImage/FindCircle provenance 有效，输出尺寸、sourceSize、source UUID 及逐项 3×3 映射矩阵相同。

Agent 的 Circle2D port 返回当前裁剪网格 `image_pixels` 圆心/半径和来源圆心，不推导来源半径。独立真值核验 CropImage 原点 `(170,90)`、尺寸 `300×300`，局部圆心 `(150,150)` 与来源圆心 `(320,240)` 均使用既有 `±3 px` Hough 容差交叉验证；Circle2D 局部中心/半径和三个 RunEngine scalar 一致。`circle_center_x/y/radius` 的 port/key 查询均返回 `px` 和同次 RunEngine 值，scalar 不附带坐标或来源字段。

验证：全量构建退出码 **0**；定向 CTest `test_acceptance_flows`、`test_findcircle`、`test_agentactor`、`test_toolschema` **4/4 通过**，新增 QtTest 单项通过。最终全量 CTest **107/108 suites 通过**；唯一失败 suite `test_mainwindow` 中 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两项顺序上下文断言，最终二进制下分别隔离运行均 **3/3 QtTest cases 通过**。格式与 `git diff --check` 通过；构建、最终全量/定向测试、隔离复跑、复审和 SHA-256 见 `build-codex-agent/agent-phase6-slice100-evidence/`。

边界：只补 FindCircle 当前像素网格及受真实 provenance 支持的来源圆心 Agent 验收；不推导来源半径、不推广至其它 Circle2D 生产者或其它空间变换，不表示 G6 标准 3 或阶段 6 整体验收完成。

## Slice 101：EdgeDefectDetection 偏差标量单位门控（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核为 **CONTRACT_PASS**，实现复核为 **IMPLEMENTATION_PASS**。Agent 仅对 EdgeDefectDetection 稳定插件 ID 的 `max_deviation`、`mean_deviation`、`deviation_stddev`，并同时满足 `point_set` / `ImageLocalPixel`、`image` 与 `reference_edge` 各自恰一条数据连线、同一 MeasurementInput 实例及正确来源端口时添加 `unit:"px"`。不对 `defect_regions` 表格、计数、图像输出或其它字段加单位，也不推导物理量或来源距离。

验证：全量构建退出码 **0**；新增 Agent 验收 QtTest 正例和 source mismatch、mode mismatch、frame mismatch 三类独立负例通过，覆盖三个 scalar 的 port/key 查询与同次 RunEngine 数值一致性；完整 `test_acceptance_flows` CTest **1/1 通过**。全量 CTest **107/108 suites 通过**；唯一失败 suite `test_mainwindow` 内 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly`，最终二进制下分别隔离运行均 **3/3 QtTest cases 通过**。格式和 `git diff --check` 通过；本切片构建、定向/全量 CTest、隔离复跑、复审和 SHA-256 见 `build-codex-agent/agent-phase6-slice101-evidence/`。

边界：只为可证明来自同一图像局部点集网格的 EdgeDefectDetection 偏差标量声明像素单位；不表示 G6 标准 3 或阶段 6 整体验收完成。

## Slice 102：Blob 最大目标质心坐标（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核为 **CONTRACT_PASS**，实现复核为 **IMPLEMENTATION_PASS**。Blob 新增 `largest_blob_center:Point2D`，从 OpenCV 轮廓矩质心写入 QPointF；处理开始清理继承的同名 stale 值，失败时不保留旧中心。Agent 仅对 Blob 稳定插件 ID 和 metadata 声明的精确 port 将有限 QPointF 序列化为 `Point2D/image_pixels`，provenance 可安全映射时附带来源点。旧 scalar 和 `largest_blob` bbox 不变；同名 key 仍走普通 QPointF 数组语义。

验证：真实 `GrabImage → CropImage → Blob` 最大方块中心局部真值 `(60,60)`、裁剪来源真值 `(90,80)`，且有意区别 bbox 几何中心 `(60.5,60.5)`；Agent port/source UUID 与 provenance 映射一致，key 仍无坐标语义。由 `UnprovenancedImageFixtureModule` 替换运行时 GrabImage 时局部点可读且不附来源对象。图像插件直接回归证明无 Blob 失败时清掉输入 sentinel。全量构建退出码 **0**；定向 `test_acceptance_flows`、`test_imageplugins` **2/2 通过**；全量 CTest **107/108 suites 通过**，唯一失败套件 `test_mainwindow` 的 Agent 输入错误通知与深色主题样式两项断言在最终二进制下分别隔离运行均 **3/3 QtTest cases 通过**。格式、metadata JSON、`git diff --check` 通过；复审、构建/测试日志、隔离复跑和 SHA-256 见 `build-codex-agent/agent-phase6-slice102-evidence/`。

边界：仅为最大 Blob 轮廓质心增加局部与可映射来源点；不将 bbox 中心替代质心、不推导面积/半径/物理尺寸，也不表示 G6 标准 3 或阶段 6 整体验收完成。

## Slice 103：RegionStats 标量像素单位门控（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。仅当 RegionStats 的 `region` 与 `image` 输入各有一条 data 边，精确来自同一稳定插件 ID `com.deeplux.plugin.sampredict` 实例的 `region` 与 `image` 输出时，Agent 为 `bbox_x/y/w/h`、`centroid_x/y` 添加 `unit:"px"`，为 `region_area` 添加 `unit:"px^2"`。标量数值保持 RegionStats 当前输入网格的局部值，不变换坐标或面积；`point_count`、`score`、`label` 与既有结构化 `centroid` 序列化不变。

真实非零原点裁剪 `GrabImage → CropImage → SamPredict → RegionStats` 对七字段逐项验证 port/key 查询单位及同次 RunEngine 值一致。错配 image 上游、缺少可选 image 和带/不带 provenance 的未知生产者均保持数值可读且不带单位。重复 image/region 上游测试先确认有效基准图可加载，再验证新增兼容 SamPredict 上游后因 `multiple:false` 被拒绝。`point_count`、`score`、`label` port/key 均无单位；既有 centroid 局部/来源坐标断言继续通过。

验证：完整构建 `cmake --build build-codex-agent -j2` 退出码 **0**；获准本机 loopback 环境下定向 CTest `test_sam_region_flow` **1/1 通过**。全量 CTest **107/108 suites 通过**；唯一失败 suite 为 `test_mainwindow`，含 `testAgentInputErrorPathDoesNotCrashOrStayThinking`、`testDarkThemeCloseAndDestroyWindowDirectly` 和 `testG4AgentCanPauseGuiCycleBetweenIterations` 的四条断言失败。三个测试函数在最终二进制中隔离复跑 **5/5 QtTest cases 通过**。格式检查与 `git diff --check` 通过。完整构建、全量/定向日志、隔离复跑、独立审核与 SHA-256 见 `build-codex-agent/agent-phase6-slice103-evidence/`。

边界：只为受同一 SamPredict 图像/区域输入门控的 RegionStats 局部标量声明像素单位；不变换来源坐标、不换算来源面积、不扩展至其它生产者，也不表示 G6 标准 3 或阶段 6 整体验收完成。

## Slice 104：Blob 标量局部单位（PASS，限定本切片）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。Agent 只对稳定 Blob 插件 ID 的四个 key 查询补单位：`blob_center_x/y` 和 `blob_perimeter` 为 `px`，`blob_area` 为 `px^2`；显式保持 key-only，四个同名 port 查询仍报 `OUTPUT_NOT_FOUND` 且无单位。标量只描述 Blob 当前输入图像网格，不作来源变换。算法、metadata、图像结果、DetectionList 与 `largest_blob_center` 不变；`blob_count`、`blob_circularity` 不加单位。

真实非零裁剪 Blob 流对四个 scalar key 逐项核对单位与 RunEngine 本次结果，并断言无坐标/来源字段；41×41 白色方块的矩质心为局部 `(60,60)`，OpenCV `contourArea=1600 px^2`、`arcLength=160 px`，与前景像素计数 `1681` 不混淆。既有来源中心 `(90,80)` 与 DetectionList bbox 映射保持通过。无 provenance fixture 仍返回四个局部单位和值，不附坐标/来源字段。

验证：全量构建退出码 **0**；定向 CTest `test_acceptance_flows` **1/1 通过**，覆盖裁剪及无 provenance 流程。全量 CTest **107/108 suites 通过**；唯一失败 suite `test_mainwindow` 的 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两条断言，在最终二进制下隔离复跑 **4/4 QtTest cases 通过**。clang-format 与 `git diff --check` 通过。构建、定向/全量日志、隔离复跑、独立复核及 SHA-256 见 `build-codex-agent/agent-phase6-slice104-evidence/`。

边界：只声明 Blob 当前输入网格内轮廓质心、几何面积与周长单位；不声明来源图面积/周长或物理单位，不把面积当像素数量，不扩展至其它插件。G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 105：无 GUI owner 时拒绝快速测量（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，测试实现复核 **IMPLEMENTATION_PASS**；仅修改 `test_mainwindow`。在删除并确认 GUI owner 已失效后，通过真实 `AgentActor::executeTool("begin_quick_measurement")` 提交 schema 有效的 2D 两点距离请求，返回 `REQUIRES_GUI` 且 `retryable` 明确为 Boolean `false`；无 requestId、status、steps、completed/remainingSteps、nextStep 或 pickedCoordinates，handler 调用次数不变。owner 存活时的既有成功对照用例保持原样。

新增测试在最终二进制隔离运行 **3/3 QtTest cases 通过**，且在全量 `test_mainwindow` 运行中通过。完整构建退出码 **0**；全量 CTest **107/108 suites 通过**；唯一失败 suite 的 `testAgentInputErrorPathDoesNotCrashOrStayThinking` 与 `testDarkThemeCloseAndDestroyWindowDirectly` 两项断言最终隔离运行 **4/4 QtTest cases 通过**。格式检查与 `git diff --check` 通过。构建、完整回归、隔离和独立审核证据见 `build-codex-agent/agent-phase6-slice105-evidence/`。

边界：只验证没有活 GUI owner 时快速测量不会伪造等待请求。G6 最后一项仍未整体通过；无图像、无登录权限、无模型环境和截图行为断言仍待逐项验收。阶段 6 保持 `IN_PROGRESS`。

## Slice 106：2D 空视口拒绝快速测量（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，测试实现复核 **IMPLEMENTATION_PASS**；仅新增 `test_mainwindow` 用例，不改产品代码。测试由 `list_viewports` 核实目标为 2D 且 `hasImage:false`，AgentActor 发起快速测量得到 `VIEWPORT_CONTENT_UNAVAILABLE` 和 Boolean `retryable:false`，不返回 requestId、等待或步骤状态。随后同一视口显示图像，同一参数成功进入 `awaiting_user_input`，并按 requestId 取消为 `cancelled`，证明前次拒绝没有留下忙碌请求。

新增用例在最终二进制隔离运行 **3/3 QtTest cases 通过**，且全量 `test_mainwindow` 中通过。全量构建退出码 **0**；全量 CTest **107/108 suites 通过**；唯一失败 suite 的两项已知断言 `testAgentInputErrorPathDoesNotCrashOrStayThinking`、`testDarkThemeCloseAndDestroyWindowDirectly` 在最终二进制隔离运行 **4/4 QtTest cases 通过**。clang-format 与 `git diff --check` 通过。构建、全量/隔离测试、独立复核和 SHA-256 见 `build-codex-agent/agent-phase6-slice106-evidence/`。

边界：只验证有 GUI owner 但 2D 视口无图像时的快速测量前置条件；无图 GUI 行为及无登录权限/模型环境仍未验证，G6 最后一项与阶段 6 整体保持 `IN_PROGRESS`。

## Slice 107：SAM 环境缺失时的人机交接与截图（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS** 和实现复核 **IMPLEMENTATION_PASS**。只扩展 `test_mainwindow`，无产品代码改动。以临时 `DEEPLUX_APP_DATA_DIR` 和清除的 SAM 环境变量确保受管 Python 缺失；首次通过真实 Agent 工具创建设置窗，随后清空该窗口 client 可能继承的模型路径，再通过真实 Agent 工具复用窗口并核验 readiness：环境未配置/未验证、没有当前可检查模型路径、模型未加载、server `not_started`、下一步 `initialize_environment`、需要用户操作。对话框呈现“初始化环境”交接提示，初始化按钮可用、重启按钮禁用；复用前后后端仍未启动，没有健康检查或环境初始化进程。模型文件不可用的断言仅表示应用会话未配置路径，不宣称磁盘任意位置没有模型文件。

测试自动保存并断言操作前 MainWindow 截图与设置窗截图非空，截图可在 [`agent-phase6-slice107-evidence`](../../build-codex-agent/agent-phase6-slice107-evidence/) 查看。专用用例最终二进制隔离运行 **3/3 QtTest cases 通过**。最终完整构建退出码 **0**，格式检查和 `git diff --check` 通过。获准 loopback 环境的串行全量 CTest **107/108 suites 通过**；唯一失败仍是 `test_mainwindow` 的两个顺序敏感断言 `testAgentInputErrorPathDoesNotCrashOrStayThinking`、`testDarkThemeCloseAndDestroyWindowDirectly`，两者在最终二进制隔离运行 **4/4 QtTest cases 通过**。并行全量运行另有一次 `test_agent_datasource_dedicated_viewport` 子进程崩溃，该 suite 串行及隔离复跑通过。完整构建、并行/串行 CTest、隔离日志、复审、截图及 SHA-256 见 `build-codex-agent/agent-phase6-slice107-evidence/`。

边界：只核验 SAM 环境缺失时真实状态交接和操作前后截图；不启动安装、不导入模型、不执行推理，不定义登录失败权限门禁，也不代表 G6 最后一项或阶段 6 整体完成。G6 与阶段 6 保持 `IN_PROGRESS`。

## Slice 108：Observer 权限拒绝 GUI 快速测量（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS** 和实现复核 **IMPLEMENTATION_PASS**。只新增 `test_mainwindow` 用例，无产品代码改动。活 MainWindow 和有图像的 2D viewport 下，Agent 对 schema 有效的测量请求在 Observer 返回 `PERMISSION_DENIED`、Boolean `retryable:false`，没有 request/steps/coordinates、pending confirmation，Controller 保持 Idle。随后相同参数在 Autopilot 下启动为 `awaiting_user_input`，按 requestId 成功取消。Observer 在 MainWindow 构造后设置并核实，消除读取本机已保存偏好造成的测试依赖。

测试自动保存并断言拒绝操作前后两张 MainWindow 截图非空，专用用例最终二进制隔离运行 **3/3 QtTest cases 通过**，且完整 `test_mainwindow` 执行中通过。最终全量构建退出码 **0**；获准 loopback 环境串行全量 CTest **107/108 suites 通过**，唯一失败是 `test_mainwindow` 两项既有顺序敏感断言；最终二进制隔离运行 **4/4 cases 通过**。格式检查、`git diff --check` 和独立复审通过；证据见 `build-codex-agent/agent-phase6-slice108-evidence/`。

边界：Observer/Advisor/Autopilot 是 Agent 自身权限档位，不等同于 `UserManager` 登出。当前源码没有基于 `UserManager::isLoggedIn()` 或 `hasPermission()` 的 Agent 业务工具门禁；本 Slice 不宣称关闭“未登录/无应用登录权限”条件，不新增该策略，G6 最后一项和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 109：未登录用户的登录窗口人机交接（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS** 和实现复核 **IMPLEMENTATION_PASS**。仅增加 `test_mainwindow` 用例，无产品代码改动。测试以真实 `UserManager::logout()` 建立 Guest/未登录状态，确认 `Edit`、`Save`、`SystemConfig` 均无权限；Agent 只打开登录窗，响应严格保留 Slice41 的三字段，不含用户名、密码或登录状态。工具不提交登录，`loginRequested` 未触发，操作后仍是未登录 Guest。

登录 UI 固定预填硬编码 `Operator`，并非当前已登出用户信息；测试核验后在保存操作后截图前清空该文本，密码框始终为空。测试使用局部状态恢复器恢复原登录/登出状态，凭据不进入截图或日志。专用用例隔离运行 **3/3 QtTest cases 通过**，且完整 `test_mainwindow` 中通过。最终全量构建退出码 **0**；获准 loopback 串行 CTest **106/108 suites 通过**：`test_mainwindow` 两项既有顺序敏感断言和 `test_agent_datasource_dedicated_viewport` 一次子进程崩溃。前两项最终二进制隔离运行 **4/4 cases 通过**；viewport suite 独立 CTest 复跑 **1/1 通过**。格式、`git diff --check`、独立复审通过。证据见 `build-codex-agent/agent-phase6-slice109-evidence/`。

边界：这是未登录登录窗口的人机交接验收，不表示用户已通过身份认证，也不表示其它 Agent 业务操作由 `UserManager` 登入状态统一门控。当前认证仍为本地现有实现；具体业务授权策略未定义。G6 最后一项和阶段 6 整体保持 `IN_PROGRESS`。

## Slice 110：FitEllipse 局部标量单位门控（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立复核 **CONTRACT_PASS** 和实现复核 **IMPLEMENTATION_PASS**。Agent 仅在稳定 FitEllipse 插件 ID、同次成功输出 `coordinate_frame=ImageLocalPixel` 且同次输出包含有效 `Ellipse2D` 时，给 `ellipse_center_x/y`、`ellipse_major_r/minor_r` 添加 `unit:"px"`，给 `ellipse_phi` 添加 `unit:"deg"`。key 和 scalar port 查询一致；`ellipse_ellipticity`、`ellipse_error` 不附单位。算法、metadata、静态输出及椭圆来源映射未改动。

真实非零原点裁剪 `GrabImage → CropImage → MeasurementInput → FitEllipse` 同次 RunEngine 结果验证五个标量与 Ellipse2D 字段数值一致、key/port 单位相同；无 provenance 的成功 `ImageLocalPixel` fixture 仍具有局部单位。缺失坐标框架和 `World3D` 的标量可读但 key/port 均无单位；ellipticity/error 在图像框架下也无单位。

全量构建退出码 **0**；定向 CTest `test_acceptance_flows` **1/1 通过**。获准 loopback 的全量 CTest **107/108 suites 通过**；唯一失败 suite 为 `test_mainwindow`：有两项既有顺序敏感断言失败后，一项 SAM 保存测试在套件上下文中出现子进程崩溃。隔离 `test_mainwindow` CTest 复现那两项断言但不崩溃；相关三项用例的最终二进制隔离运行 **5/5 QtTest cases 通过**。格式门禁、`git diff --check` 和 SHA-256 清单通过。日志、复审和二进制摘要见 `build-codex-agent/agent-phase6-slice110-evidence/`。

边界：只声明 FitEllipse 当前 ImageLocalPixel 网格的中心、半轴和角度单位；误差、来源网格半轴/角度与物理单位不在范围。其它空间插件和 3D 坐标审计仍未完成，G6 标准 3 与阶段 6 整体保持 `IN_PROGRESS`。

## Slice 111：MainWindow 测试环境与主题状态隔离（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 `CONTRACT_PASS`、实现复核 `IMPLEMENTATION_PASS`。仅改 `tests/test_mainwindow.cpp`、Linux CTest 启动配置及临时 HOME runner，不改产品代码。

Agent 输入错误测试改为在 `MainWindow` 构造后清空 LLM client，避免加载本地设置后意外发起真实模型请求，并在窗口销毁后恢复原 client/权限。系统设置、布局主题、深色关闭用例均恢复原 ConfigManager 主题键和值；布局和深色关闭测试在断言/切换前建立浅色基线。Linux CTest runner 为 MainWindow 单独创建临时 HOME，将本构建目录已生成的插件同步进去，清除 `DEEPLUX_APP_DATA_DIR` 后运行测试，退出时删除临时 HOME；Windows/macOS 仍走原启动方式。

最终全量构建退出码 **0**；独立 `test_mainwindow` CTest **1/1 通过**。获准本机 loopback 串行全量 CTest 为 **106/108 suites 通过**：`test_agent_datasource_dedicated_viewport` 在全量中一次 SIGSEGV、隔离复跑 **1/1 通过**；`test_samannotatordialog` 中两个会话替换异步断言在全量及隔离复跑中重复失败（旧 embedding 保留、新 set_image 未到达服务端），列为后续竞态调查，不归因于本切片。标准受限沙箱运行的 13 个 listener 测试因 localhost `listen()` 被拒失败，loopback 复跑通过。格式门禁、runner `bash -n`、`git diff --check` 和 SHA-256 校验通过；日志、复审和二进制摘要见 `build-codex-agent/agent-phase6-slice111-evidence/`。

边界：只修复 MainWindow 测试对持久 LLM client 和全局主题配置的依赖。此前曾在未隔离 HOME 下运行 CTest；测试可能更新过该机 `~/.deeplux/config/config.json` 的主题时间戳/设置，之后的验证均使用临时 HOME。SAM 会话替换异步失败仍待查；阶段 6/G6 继续 `IN_PROGRESS`。

## Slice 112：SAM 会话替换测试模型夹具（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 `CONTRACT_PASS`、实现复核 `IMPLEMENTATION_PASS`。只改 `tests/test_samannotatordialog.cpp`，产品代码未改。

两个异步会话替换用例各自在 `QTemporaryDir` 创建受支持命名的占位 `.pth` 文件，并显式设给 `SamBackendClient` 与 `DEEPLUX_SAM_MODEL`。这让会话替换经过真实文件存在性 readiness 检查；请求仍发到测试 HTTP 服务，不启动真实 SAM 推理，也不依赖用户保存的模型路径。修复前临时 HOME 下两个用例分别因旧 embedding 未替换、新 `/set_image` 未发出而失败。

定向 `test_samannotatordialog` CTest **1/1 通过**；最终全量构建退出码 **0**；获准本机 loopback、临时 HOME 下串行 CTest **108/108 suites 通过**，包括先前偶发失败的 dedicated viewport 与 SAM 会话替换用例。`clang-format --dry-run --Werror`、`git diff --check` 和 SHA-256 校验通过；独立复审通过。完整构建、定向/全量测试、插件同步、复审和二进制摘要见 `build-codex-agent/agent-phase6-slice112-evidence/`。

边界：仅消除 SAM 对话框测试对本机模型路径的依赖；不代表真实 SAM 模型兼容性或真实推理通过。G6 标准 3 的其它空间算法与阶段 6 剩余 GUI、登录/权限和模型环境条件仍待验收，阶段 6 保持 `IN_PROGRESS`。

## Slice 113：3D MeasurementInput 保留双精度拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 `CONTRACT_PASS`，实现复核 `IMPLEMENTATION_PASS`。MainWindow 不再消费 3D viewport 的 `QVector3D` 兼容拾取信号；普通 GUI 测量改由 `point3DWorldClicked(double,double,double)` 唯一处理，原始 double 同时写回运行时 `MeasurementInput` 和 Project。兼容信号与旧 QVector3D slot 保留；直接调用旧 slot 的值仍受其 float 精度限制。Agent 快速测量响应仍保存 double 世界坐标，其既有 `QVector3D` 叠加算术不在本切片范围。

新增 MainWindow 回归通过实际 viewport 左键 press/release 触发同一拾取的两种信号。`point_plane` 的 `point` 与 `point_pair` 的 `point1` 均逐值保留 `(16777217.125,12.5,-3.75)`；各测试中非目标 `plane` / `point2` 未前进或改变。信号计数、Project 参数和运行一次后 RunEngine 中两个模块的运行时参数均核对。新用例最终隔离 QtTest **3/3（含 init/cleanup）通过**。

最终全量构建退出码 **0**。获准本机 loopback、临时 HOME 下的串行全量 CTest **107/108 suites 通过**；唯一失败 `test_agent_datasource_dedicated_viewport` 在完整套件中间歇性 SIGSEGV，其隔离 `ctest --rerun-failed` **1/1 通过**。全量中的 `test_mainwindow`（含新用例）通过。格式检查、`git diff --check` 通过；构建、focused/full CTest、隔离复跑、插件同步、审查和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice113-evidence/`。

边界：offscreen 测试环境不能创建 QOpenGLWidget context；合成的真实鼠标事件验证拾取索引和双精度信号路由，不代表 GPU 绘制现场验收。3D 快速测量叠加内部仍使用 QVector3D float；其数值精度与全部 3D 算法的坐标单位/外部基准仍需独立审计。G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 114：MeasureGap 数据端口与实现对齐（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**。实现复审首轮指出验收没有覆盖 Agent port/key 查询；修正后同一 reviewer 复审 **IMPLEMENTATION_PASS**。

`MeasureGap` metadata 与实现统一为必需 `point1`/`point2:Any`，声明 `gap_distance`、XYZ delta、维度及兼容 `gap` 输出；插件将 `gap` 写为 `gap_distance` 同值 alias，并拒绝 NaN/Inf 坐标。`tests/acceptance/generate_ports.py` 同步输出声明。CLI 和插件手册现在给出可运行的二维图流，并注明当前 `MeasurementInput.point_pair` 的 `Point2D` 端口不支持三坐标图连线。

新增动态 `MeasurementInput(point_pair) → MeasureGap` 工程，以 Agent `run_flow` 执行并检查 RunObservation 成功；同一 runId 上分别按 key 和 port 读取 `gap`、`gap_distance`、delta 与维度并对照 RunEngine 输出。直接插件测试覆盖二维/三维计算、六个坐标位置的 NaN/Inf 拒绝及错误点形状拒绝，失败不生成间隙结果。

完整构建退出码 **0**；`test_measuregap` 和新增 acceptance 流用例均通过。全量 CTest **107/108 suites 通过**；唯一失败为既有间歇性 `test_agent_datasource_dedicated_viewport` 子进程异常退出，隔离复跑 **1/1 通过**。格式检查、JSON 解析、生成器语法检查与 `git diff --check` 通过；构建、CTest、插件同步、独立审核与哈希证据见 `build-codex-agent/agent-phase6-slice114-evidence/`。

边界：本切片仅验收合法二维 `MeasurementInput` 图流；虽 `MeasureGap` 算法支持三坐标数组，现有 `MeasurementInput.point_pair` 的 `Point2D` 类型门禁仍阻止该图流输入三维点，三维 GUI/Agent 生产端口契约未解决。G6 标准 3 和阶段 6 仍为 `IN_PROGRESS`。

## Slice 115：MeasurementInput 点对二维/三维语义（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**，实现复审首轮提出的有效 provenance 覆盖缺口已关闭，复审最终 **IMPLEMENTATION_PASS**。

`MeasurementInput.point1`/`point2` 端口与生成器声明为 `Any`，实际 `point_pair` 仅接受两个同维度、长度均为 2 或 3 的有限数值点。二维按 `ImageLocalPixel` 处理并保留有效图像来源映射；三维按 `World3D` 处理，不产生图像来源映射。Agent port 查询仅在稳定插件 ID、metadata `Any`、同次运行 mode 和坐标框架均匹配时返回 Point2D/Point3D 语义；三维保持原始 double 数值，不带来源图、单位或 coordinateSystem。普通 key 与非目标模块仍按通用输出处理。

动态验收从具有有效非零平移 provenance 的图像源运行至 `MeasurementInput(point_pair) → MeasureGap`。精确点 `(16777217.125,12.5,-3.75)`、`(16777220.125,16.5,-0.75)` 在两端口、运行输出和 Agent 响应中逐值一致；输出仍带 `World3D`，`source_coordinate_available=false`，无来源图坐标。`MeasureGap` 得到 `(3,4,3)`、距离 `√34`。同用例还核验 3D 点连接到 `FitCircle.fit_points` 时连线结构可接受、运行期在该端口拒绝，以及带伪造 mode/frame 的非目标模块不能获得 Point3D 语义。二维 MeasureGap、裁剪来源映射、坏输出拒绝和原有 MeasurementInput/MeasureGap 插件用例继续通过。

最终全量构建退出码 **0**；全量串行 CTest **108/108 suites 通过**，使用临时 HOME 同步本构建插件并取消 `DEEPLUX_APP_DATA_DIR`。Slice115 定向插件和 acceptance 用例均通过；metadata/生成器一致、clang-format、`git diff --check`、独立复审及 SHA-256 清单通过。日志、插件同步、复审与二进制摘要见 `build-codex-agent/agent-phase6-slice115-evidence/`。

边界：`Any` 允许先建立连线，消费者仍按运行时载荷形状执行其类型门禁；`World3D` 不定义外部坐标基准和物理单位。未覆盖真实 GUI/GPU 与真实设备坐标校准；G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 116：3D 快速测量保留双精度世界坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 `CONTRACT_PASS`，实现复审 `IMPLEMENTATION_PASS`。Agent world-click 回调现在把原始 double 直接交给 MainWindow 快速测量；点缓存及距离、点到线段、线段间距、角度和三点圆计算均使用 `Eigen::Vector3d`/double，仅组装既有 overlay 几何时转为 `QVector3D`。旧 `QVector3D` 兼容 slot 保留。

新增真实 MainWindow/Agent 请求路径回归，覆盖上述五种可用测量，逐值核对 Agent `pickedCoordinates`，并通过既有 `measurementLines3D()` label 字段验证固定双精度 oracle：距离 `5.831`、点到线段 `3.375`、线段间距 `3.187`、角度 `18.43°`、三点圆半径 `R=1.12`。新用例隔离 QtTest **3/3 通过**；既有真实鼠标 world-pick 用例和 GUI/Agent 模式互斥用例均 **3/3 通过**。

最终全量构建退出码 **0**；获准本机 loopback、临时 HOME、同步 66 个构建插件并 unset `DEEPLUX_APP_DATA_DIR` 后，串行全量 CTest **108/108 suites 通过**。clang-format（仅本切片改动区间）、`git diff --check`、独立实现复审及证据摘要校验通过；构建、定向/全量测试、插件同步、复审和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice116-evidence/`。

边界：overlay 几何仍使用现有 `QVector3D` 和投影路径，本切片不声明 GPU/屏幕绘制具有双精度，也不定义 World3D 的物理单位或设备/工件外部基准。G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 117：GapMeasure3D ROI 中心坐标网格说明（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 `CONTRACT_PASS`、实现复审 `IMPLEMENTATION_PASS`。只将 `GapMeasure3D.roiCenterX/Y` metadata 标签补充为“当前输入高度图局部像素”；PropertyPanel 和 Agent `get_module_params_schema` 共用该描述。`GapMeasure3DPlugin::process()` 将其直接用作当前输入矩阵列/行索引。

新增插件参数契约回归同时读取 metadata 与 Agent schema，断言两个标签均包含局部像素语义且内容一致。参数值/范围、ROI 算法、provenance、结果和单位均未改动。定向 `test_pluginparametercontracts` **1/1 suite 通过**；最终全量构建退出码 **0**，获准 loopback、临时 HOME、同步 66 个构建插件并 unset `DEEPLUX_APP_DATA_DIR` 的串行全量 CTest **108/108 suites 通过**。JSON、clang-format、`git diff --check`、独立复审和 SHA-256 清单通过；完整证据见 `build-codex-agent/agent-phase6-slice117-evidence/`。

边界：这只说明 ROI 中心相对于当前输入高度图的局部像素坐标，不自动映射到裁剪前来源图，不定义高度/深度物理单位或 World3D 外部基准。G6 标准 3 其余坐标审计及阶段 6 整体继续 `IN_PROGRESS`。

## Slice 118：3DPreProcessing 与 FitPlane ROI 坐标网格说明（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 `CONTRACT_PASS`、实现复审 `IMPLEMENTATION_PASS`。`3DPreProcessing` 与 `FitPlane` 的 ROI 中心标签现说明是当前输入深度/高度图的局部像素坐标；矩形宽高及拟合 ROI 长短边说明为像素尺寸，并保留 `0=全图`。PropertyPanel 与 Agent `get_module_params_schema` 透传同一 metadata。

新增 `testHeightMapRoiMetadataMatchesAgentSchema` 覆盖两个插件的八个 ROI 标签，核对中心局部网格、尺寸像素、全图语义和 Agent/metadata 文案一致。`roiAngle` 度单位、默认值、范围、插件算法、输出与 provenance 未改。定向 `test_pluginparametercontracts` **1/1 suite 通过**；全量构建退出码 **0**；获准 loopback、临时 HOME、同步 66 个构建插件并 unset `DEEPLUX_APP_DATA_DIR` 的串行全量 CTest **108/108 suites 通过**。JSON、clang-format、`git diff --check`、独立复审和 SHA-256 校验通过；完整证据见 `build-codex-agent/agent-phase6-slice118-evidence/`。

边界：参数仅相对于插件当前输入矩阵，不自动映射裁剪前父图；本切片不定义高度值/标定比例的物理单位或 World3D 外部基准，也不关闭 G6 标准 3 其余空间坐标审计。

## Slice 119：MeasureCircle 初始测量参数像素语义（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 `CONTRACT_PASS`、实现复审 `IMPLEMENTATION_PASS`。五项 metadata 标签说明 `initialCenterX/Y`、`initialRadius`、`searchLength` 与 `exclusionRadius` 均以当前输入图像局部像素为基准；保留“搜索半长”和“残差剔除半径”语义。PropertyPanel 和 Agent `get_module_params_schema` 共享同一 metadata 文案。新增参数契约测试逐项核对 schema/metadata 一致、局部像素语义和无来源图/物理单位/World3D 声明。

定向 `test_pluginparametercontracts` **1/1 suite 通过**，全量构建退出码 **0**，metadata JSON、格式和 `git diff --check` 通过。两次隔离 HOME 的全量串行 CTest 均为 **107/108 suites**：唯一失败是既有 `test_agent_datasource_dedicated_viewport` 在同一确认链路 SIGSEGV；该 suite 单独隔离重跑 **1/1 通过**。因此全量回归明确记录为未通过，不归因于本切片。独立契约/实现复审通过；两次全量日志、隔离复跑、二进制与源码/证据哈希见 `build-codex-agent/agent-phase6-slice119-evidence/`。

边界：仅澄清 MeasureCircle 参数的当前输入图像局部像素语义，不将半径推导到来源图、不提供物理尺度。本切片未修复或解释 `test_agent_datasource_dedicated_viewport` 的间歇性 SIGSEGV；G6 标准 3 和阶段 6 整体保持 `IN_PROGRESS`。

## Slice 120：CropImage 旋转矩形参数坐标与单位（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 `CONTRACT_PASS`、实现复审 `IMPLEMENTATION_PASS`。Agent schema metadata 标签及 CropImage 自定义配置页现统一说明 `[cx,cy,l1,l2,deg]` 中心为当前输入图像局部像素坐标、`l1/l2` 为像素半长、`deg` 为度；保留 HALCON `Length1/Length2` 半长口径和“整边长=2×半长”说明。测试分别核对 Agent/metadata 标签、帮助说明与表单行标签。

Slice120 只修改既有 `rectangles.label` 与自定义配置页说明/行标签。metadata 中的数组约束字段和 `ui.results` 描述在 Slice120 开始前已存在，本切片予以保留。定向 `test_pluginparametercontracts` 最终 **1/1 suite 通过**；首次仅构建测试目标时加载了旧 `CropImagePlugin` 二进制而失败，补建插件后通过，原始失败和修正日志均保留。最终全量构建退出码 **0**，获准 loopback、临时 HOME、同步 66 个构建插件并 unset `DEEPLUX_APP_DATA_DIR` 的串行 CTest **108/108 suites 通过**。JSON、clang-format、`git diff --check`、独立复审和 SHA-256 证据通过，见 `build-codex-agent/agent-phase6-slice120-evidence/`。

边界：仅说明 CropImage 参数相对于当前输入图像的局部像素语义；输出 crop 到来源图映射继续由既有 provenance 提供。不把参数自动折算到父图、不推导来源尺寸或物理单位；G6 标准 3 其余空间审计及阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 121：GapMeasure3D Z 向结果不推断毫米单位（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**、实现复审 **IMPLEMENTATION_PASS**。`zScale` 未定义物理单位，因此 `corner_dz_mm` 与 `corner_dist_mm` 的兼容键名继续保留，但端口显示名不再标为 mm；Agent 工具说明其单位未知，并明确 `corner_dist_mm` 仅在 X/Z 经相同物理单位标定时才可解释为物理欧氏距离。三个已有水平结果在检出成功时的 mm 声明、所有键/端口、数值、算法和失败哨兵行为均未改变。

新增 metadata 契约测试核对 `zScale` 标签说明及两个 Z 输出端口显示名；ToolSchema 测试核对单位未知与 hypot 适用条件；真实 `GrabImage → GapMeasure3D` Agent 流程验证两个 Z 键仍返回 RunEngine 原值且不带 `unit`，并保留水平结果的 port/key 毫米单位断言。既有算法用例继续校验 `corner_dist_mm == hypot(corner_dx_mm, corner_dz_mm)`。定向三套件 **3/3 通过**，全量构建退出码 **0**。

受限沙箱全量 CTest 因 localhost 监听限制为 **95/108 suites 通过**；获准 loopback 环境全量为 **107/108 suites 通过**，唯一失败是 `test_agent_datasource_dedicated_viewport` 确认执行期间子进程 SIGSEGV，隔离复跑 **1/1 通过**。因此全量结果保留为未完全通过，不归因于 Slice121。metadata JSON、clang-format、`git diff --check`、独立复审及 SHA-256 校验通过；构建、定向/全量/隔离复跑、插件同步和二进制证据见 `build-codex-agent/agent-phase6-slice121-evidence/`。

边界：本切片不定义输入高度值或 `zScale` 的物理单位，不改变历史 `_mm` 键名和算法数值，也不关闭 G6 标准 3 的其它坐标/单位审计；阶段 6 整体继续 `IN_PROGRESS`。

## Slice 122：FreeformSurface 点云标量不猜测单位（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**、实现复审 **IMPLEMENTATION_PASS**。`get_module_output` 现说明 FreeformSurface 的 `surface_roughness` 和 `surface_area` 依赖输入点云 XYZ 尺度；仅 XYZ 使用共同物理单位/尺度时才有对应的长度/面积量纲。由于 `PointCloudData` 没有单位字段且 `LoadPointCloud` 可以分别缩放轴，Agent 不推断 mm、mm² 或 world frame；输出键、算法和值不变。

新增真实 PLY 流程以非等比例 XYZ 缩放执行 `LoadPointCloud → FreeformSurface`，并通过 Agent key 查询两个标量。测试确认 RunEngine 与 Agent 值一致、点数为 4、响应不附带 `unit` 或坐标框架。Acceptance suite 显式依赖并安装 FreeformSurface 插件。定向 `test_acceptance_flows`、`test_toolschema`、`test_freeformsurface` **3/3 suites 通过**；全量构建退出码 **0**；获准 loopback 临时 HOME 下串行全量 CTest **108/108 suites 通过**。格式、`git diff --check`、独立复审和源码/证据/二进制 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice122-evidence/`。

边界：本切片只说明 FreeformSurface 标量的尺度来源和单位未知，不补充点云单位/坐标基准字段，不改变算法或 `LoadPointCloud` 独立轴缩放行为；G6 标准 3 其余空间算法仍需逐项审计，阶段 6 整体保持 `IN_PROGRESS`。

## Slice 123：CalculateOffset 原始坐标差单位边界（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**、实现复审 **IMPLEMENTATION_PASS**。Agent 说明 `offset_x/y` 是 `current-target` 原始数值差，只有当前与目标坐标处于同框架同单位时才具备相应物理意义；插件不做坐标/单位转换。`current_point` 覆盖 X/Y 并忽略 Z，`current_angle` 覆盖参数角度；`offset_a` 按度归一化至 `(-180,180]`，metadata 仅为此结果声明 `deg`。

扩展 `MeasurementInput(point_line) → CalculateOffset(current_point)` 实流回归，三个输出均按 port 与 key 两路查询；六项数值与同次 RunEngine 输出一致，X/Y 两路都不带单位或坐标框架，角度两路均带 `deg`。ToolSchema 回归锁定单位/框架转换边界、参数覆盖和忽略 Z 的说明。定向 `test_batch4`、`test_toolschema` **2/2 suites 通过**，最终全量构建退出码 **0**，获准 loopback 临时 HOME 下全量 CTest **108/108 suites 通过**。metadata JSON、clang-format、`git diff --check`、独立复审及源码/证据/二进制 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice123-evidence/`。

边界：本切片不新增坐标框架兼容门禁或变换，不声明 X/Y 的物理单位，不使用 Point3D 的 Z；项目中剩余几何/图像空间算法与阶段 6 环境前置条件仍需审计，阶段 6 保持 `IN_PROGRESS`。

## Slice 124：MeasureRect 角度补齐输出端口（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**、实现复审 **IMPLEMENTATION_PASS**。`MeasureRect` 原已产生 `rect_angle` 并在 `ui.results` 声明 `deg`，但漏列 `ports.outputs`，造成按 key 可读、按 port 不可读。本切片仅新增可选 `rect_angle:Number` 输出端口，不改算法、角度值或键。

现有旋转矩形真实 RunEngine/Agent 流程已对四个矩形标量均按 port/key 查询；核对值与同次 RunEngine 输出一致、角度两路均为 `deg`。metadata 断言检查 `rect_angle` 为可选 Number。定向 `test_acceptance_flows`、`test_measurerect` **2/2 suites 通过**；全量构建退出码 **0**；获准 loopback 临时 HOME 下串行全量 CTest **108/108 suites 通过**。JSON、clang-format、`git diff --check`、独立复审及 SHA-256 证据通过；证据见 `build-codex-agent/agent-phase6-slice124-evidence/`。

边界：只补齐已有标量结果端口，不改变坐标/来源语义或新增算法；其它插件未配对的结果键与端口仍待逐项对账，G6 标准 3 和阶段 6 整体保持 `IN_PROGRESS`。

## Slice 125：DistancePP ΔX/ΔY 补齐输出端口（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约复审 **CONTRACT_PASS**、实现复审 **IMPLEMENTATION_PASS**。`DistancePP` 原算法已产生 `delta_x/y`，results metadata 也已有两项，但 outputs 漏列端口。本切片只新增两个可选 Number 端口，保留原算法、键和值及既有坐标框架单位门控。

现有 ImageLocalPixel 实流与 Unspecified 自定义点流均对 `distance`、`delta_x`、`delta_y` 按 port/key 查询，值与同次 RunEngine 输出逐项一致。ImageLocalPixel 三项均带 `px` 且不带来源坐标；Unspecified 三项不带单位、坐标系、坐标框架或来源坐标。定向 `test_acceptance_flows`、`test_distancepp` **2/2 suites 通过**；全量构建退出码 **0**；获准 loopback 临时 HOME 下全量 CTest **108/108 suites 通过**。JSON、clang-format、`git diff --check`、独立复审和源码/证据/二进制 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice125-evidence/`。

边界：只补全已有 delta 输出的端口访问；不改变 Point2D 载荷的坐标框架门禁，也不推断 Unspecified 坐标的物理单位。其它未配对的结果键/端口继续逐项审计；G6 标准 3 与阶段 6 整体保持 `IN_PROGRESS`。

## Slice 126：N 点标定诊断值补齐输出端口（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。`NPointCalibration` 原已写出 `reprojection_error` 与 `point_count` 并声明于 `ui.results`，但未列入输出端口，导致 Agent 只能按 key 查询。本切片只增加两个可选端口：`reprojection_error:Number`、`point_count:Integer`；`calibration_valid` 保持结果面板字段，不新增端口。失败运行不发布端口快照，新增业务值在失败时均不可用。

新增真实 Agent/RunEngine 回归：临时图像经 `GrabImage → NPointCalibration`。成功运行中两个值均按 port/key 查询，和同次 RunEngine 输出一致，点数保持整数；重投影误差单位保持原有“世界坐标单位”，不推断毫米。成功状态下既有 `calibration_valid=true` key 保留。随后将标定点设为空数组并显式保持 `clearPointsOnRun=true`，触发真实执行失败；Agent 对三个 result keys 与两个新端口均返回 `status=unavailable` 且无 `value`。metadata schema 同时验证端口类型/可选性及不暴露 `calibration_valid` 端口。

定向 Agent 复现用例 **3/3 QtTest 项通过**（含 fixture setup/cleanup）；全量构建退出码 **0**；获准本机 loopback、临时 HOME、同步 66 个插件并 unset `DEEPLUX_APP_DATA_DIR` 后全量串行 CTest **108/108 suites 通过**。metadata JSON、改动区 clang-format、`git diff --check`、独立复核及 SHA-256 校验通过。完整日志、审核意见、70+ 构建二进制清单和摘要见 `build-codex-agent/agent-phase6-slice126-evidence/`。

边界：只开放已有成功诊断值端口；不改变标定算法、结果键、失败语义或世界坐标单位，不把失败状态伪装成成功端口值。其它未配对结果键/端口及 G6 标准 3 其余坐标审计仍待逐项处理，阶段 6 整体保持 `IN_PROGRESS`。

## Slice 127：PLC 写入数量补齐输出端口（PASS，限定子项；全量回归有遗留失败）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。`PLCWritePlugin` 只在收到有效 Modbus 写响应后产生 `plc_write_count`，原仅可按 key 查询。本切片只新增可选 `plc_write_count:Integer` 端口；不新增 `plc_write_success` 端口，因为该字段仅在成功时为 true，运行状态已表达该信息。PLC 协议、确认、权限、超时、不确定语义和重试行为未变。

本地模拟 TCP 的批准成功写入测试核验 schema 可选 Integer、成功 run 的 key/port 数量一致且请求仍恰一次；无有效响应的测试核验 `get_run_results`、结果 key 与新增 port 均显式返回 `retryable=false`，port/key 均为 `uncertain` 且无 `value`，查询后请求数仍为 1。两条最终定向 QtTest 各 **3/3 项通过**（含 setup/cleanup）；全量构建退出码 **0**。

最终全量串行 CTest **107/108 suites 通过**；唯一失败是既有 `test_agent_datasource_dedicated_viewport` 在执行中 SIGSEGV（CTest #61），其隔离复跑 **1/1 通过**。按失败前序 CTest #60→#61 重放 20 轮复现 1 次；GDB 下 6 轮未复现后停止长循环，未捕获到栈，根因未定位。该失败不归因于 PLC 切片，也不把全量回归记作通过。构建、全量/隔离回归、定向日志、独立审核和 SHA-256 见 `build-codex-agent/agent-phase6-slice127-evidence/`。

边界：验证使用本地 Modbus 模拟器，未连接真实 PLC；标量只在有效回执成功时有值。MeasurementInput 的模式 key 与 LoadPointCloud 的点数/路径 key 仍待逐项决定是否适合做端口；路径不新增为下游可连线值。阶段 6 整体保持 `IN_PROGRESS`。

## Slice 128：LoadPointCloud 点数输出端口（PASS，限定子项；全量回归有遗留失败）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。`LoadPointCloud` 产生的 `point_count` 原先只在结果 key 中可查。本切片新增可选 `point_count:Integer` 输出端口，不新增 `point_cloud_path` 端口；本地文件路径的 key 行为、点云解析与缩放均不变。

扩展真实 PLY → `LoadPointCloud → FreeformSurface` Agent/RunEngine 流程：同一 runId 对 `point_count` 做 key/port 查询，两者均为 4，与 Loader 的 RunEngine 输出一致；响应不附加 unit、coordinateSystem、coordinateFrame 或 sourceImageCoordinates。schema 核验端口类型为 Integer、required=false，且无 `point_cloud_path` 端口。定向 `testAgentFreeformSurfaceResultsRetainUnknownUnits` **3/3 QtTest 项通过**（含 setup/cleanup）；全量构建退出码 **0**。串行全量 CTest **107/108 suites 通过**，唯一失败仍为 `test_agent_datasource_dedicated_viewport` SIGSEGV；隔离复跑 **1/1 通过**。由于该失败在两次相邻切片全量回归均出现，二者全量结果都如实记录为未通过。JSON、clang-format、`git diff --check`、独立复核和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice128-evidence/`。

边界：点数不携带点云物理单位或坐标框架；未把源文件路径作为下游数据端口传播。viewport 间歇性 SIGSEGV 根因仍未定位；阶段 6 整体保持 `IN_PROGRESS`。

## Slice 129：3D MeasurementInput 坐标往返与点云替换隔离（PASS，限定子项；全量回归有遗留失败）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。本切片仅新增 `tests/test_mainwindow.cpp` 两项回归，不改产品代码：第一项经真实 MainWindow/RunEngine 等待拾取流程发送高精度 3D 点，核对工程参数和由 `runFinished.runId` 读取的本次 RunEngine 快照一致，且 `coordinate_frame=World3D`；第二项在首点后将绑定视口换成坐标不同且点数从 3 变为 4 的新点云，确认视口显示新内容后再发旧等待的次点，等待状态清除、工程次点不变、流程未启动。

新增两项 QtTest 定向运行 **4/4 项通过**（含 init/cleanup）；全量构建退出码 **0**。全量串行 CTest **107/108 suites 通过**，唯一失败为既有 `test_agent_datasource_dedicated_viewport` SIGSEGV；使用相同隔离 HOME 单独复跑 **1/1 通过**，全量仍记为未通过。格式、`git diff --check`、独立复核和证据摘要见 `build-codex-agent/agent-phase6-slice129-evidence/`。

边界：这是 MainWindow 的 UI 交互输入回归，不覆盖 Agent 发起 `run_flow`；不覆盖 2D 缩放/裁剪、多视图切换、全部空间算法或 G6 环境前置条件。间歇性 viewport SIGSEGV 根因仍未定位；阶段 6 整体保持 `IN_PROGRESS`。

## Slice 130：缺失环境前置条件的人机交接证据（PASS，限定证据子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**、实现/证据复核 **IMPLEMENTATION_PASS**。本切片不改产品行为；仅为两个既有无图像 GUI 回归增加 before/after 截图保存。MainWindow 指定用例（无 GUI、空视口快速测量、缺 SAM 环境、配置不变、未登录交接）**7/7 QtTest 项通过**；SAM handoff 无图像用例 **3/3 通过**。全量构建退出码 **0**，格式与 `git diff --check` 通过。

8 张 PNG 持久化于 `build-codex-agent/agent-phase6-slice130-evidence/screenshots/`，逐张可打开并完成视觉核验；截图和日志 SHA-256 校验通过。行为覆盖 `REQUIRES_GUI`、空视口 `VIEWPORT_CONTENT_UNAVAILABLE` 且无请求载荷、无图像 SAM `NO_IMAGE` 且不创建标注对话框、Guest 状态权限查询、登录人机交接，以及缺模型/解释器配置时后端不启动且配置状态/字节不变。环境变量与 HOME 隔离方式、逐测试边界及证据清单见该目录 `summary.txt`。

边界：`UserManager::hasPermission()` 当前没有生产调用点，因此 Slice130 仅验证 Guest 与权限查询值，不能证明真实 Agent/GUI 操作会被登录权限拒绝；G6 最后一项继续未勾选。无 GUI 本身无可截图窗口，使用直接返回断言。G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 131：3D MeasurementInput 跨视口采点隔离（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。仅新增 `testG6MeasurementInput3DCrossViewportPickIsIgnored`，不改产品代码。两个具有不同点数的非空 3D 点云视口均通过前置核验：首点只接受来自目标视口；第二点若来自另一个视口，工程参数保持不变、行状态仍为“等待拾取 2/2”、运行未启动；目标视口的第二点之后只触发一次运行，并按该次 `runId` 核对工程参数和 RunEngine 输出及 `World3D` 标签。

定向用例 **3/3 QtTest 项通过**（含 init/cleanup）；全量构建退出码 **0**；完整 `test_mainwindow` 串行 CTest **1/1 通过**（隔离 HOME、同步插件、loopback 环境）。clang-format、`git diff --check`、独立复核和 SHA-256 见 `build-codex-agent/agent-phase6-slice131-evidence/`。

边界：只验证 MainWindow 的 3D `MeasurementInput` 多视口拒点，不代表 Agent 快速测量、2D 缩放/裁剪或其它空间算法均通过。G6 标准 3、G6 最后一项和阶段 6 整体仍保持 `IN_PROGRESS`。

## Slice 132：3D MeasurementInput 点平面四点交互输入（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。仅新增 `testG6MeasurementInput3DPointPlanePicksAllWorldVertices`，不改产品代码。合成非空点云上按 point、三个平面顶点顺序执行四次真实拾取：等待计数依次为 2/4、3/4、4/4，前三次均未启动运行；第四次恰好运行一次。按 `runFinished` 的 runId 读取输出，逐项核对 point、九个 plane 坐标及 `World3D`，并同时核验工程参数。

定向 QtTest **3/3 项通过**（含 init/cleanup）；全量构建退出码 **0**；完整 `test_mainwindow` 串行 CTest **1/1 通过**（隔离 HOME、同步插件、offscreen）。clang-format、`git diff --check`、独立复核和 SHA-256 见 `build-codex-agent/agent-phase6-slice132-evidence/`。

边界：仅关闭 MainWindow 的 3D `point_plane` 四点输入回归，不外推到其它模式、单位/外部坐标基准、全部空间算法或 G6 标准 3 全面完成。最近全量 CTest 的单项间歇性 viewport SIGSEGV 尚未定位；G6 最后一项仍缺少真实登录权限保护操作拒绝证据。阶段 6 保持 `IN_PROGRESS`。

## Slice 133：MeasurementInput 3D point_line 坐标框架与来源映射（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。`point_line` 的二维 GUI 拾取继续标记 `ImageLocalPixel` 并保留来源映射；三维 GUI 拾取仍保留 XYZ 点与 XY Line2D（端点 Z 忽略）的既有参数/算法行为，但将混合输出标记 `Unspecified`，禁止图像来源坐标映射。旧 marker 缺失的工程延续旧 ImageLocalPixel 解释；已说明旧版 3D 数据若没有 marker 无法从历史坐标反推拾取维度。隐藏 marker 由 UI 写入，独立 mode/point/line 编辑使其失效，Agent 不能直接设置。

全量构建退出码 **0**；受影响完整 CTest suites **7/7 通过**：`test_mainwindow`、`test_project`、`test_agentactor`、`test_measurementinput`、`test_acceptance_flows`、`test_toolschema`、`test_pluginparametercontracts`。覆盖 2D 多次拾取 marker 保持、3D 混合坐标、独立 mode/point/line 修改失效、离开并返回 point_line、未知/错误类型 marker fail-closed、有效/无效 provenance 下的 plugin 和 Agent 响应，以及 Agent readOnly 拒绝。JSON 校验、clang-format 与 `git diff --check` 通过；独立审核、日志、二进制时间戳及 SHA-256 见 `build-codex-agent/agent-phase6-slice133-evidence/`。

边界：本切片不将混合 point_line 解释为 `World3D`，不改 2D 行为或旧工程默认语义。未运行全量 CTest；最近已知全量回归有 `test_agent_datasource_dedicated_viewport` 的间歇性 SIGSEGV。旧版无 marker 的 3D point_line 保存数据无法自动纠正；G6 标准 3 其余插件/坐标审计、G6 最后一项及阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 134：MeasurementInput 3D line_pair 坐标框架与来源映射（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。复用隐藏 `pick_coordinate_frame`：二维四次 GUI 拾取逐次提交 `ImageLocalPixel` 并保持已有来源映射；三维四次拾取仍按既有接口写 XY、忽略不同输入 Z，提交 `Unspecified`，插件不再从附加图像 provenance 伪造来源线。Project 与 Agent 修改 mode/line1/line2 时使已有 marker 失效；Agent 不能直接设置 marker。

完整构建退出码 **0**；受影响完整 CTest suites **5/5 通过**：`test_mainwindow`、`test_project`、`test_agentactor`、`test_measurementinput`、`test_acceptance_flows`。插件和 Agent 测试覆盖二维来源映射、Unspecified/未知/非字符串 marker fail-closed、line1/line2 port/key 原数组输出且不附图像坐标字段。JSON、clang-format、`git diff --check` 与相关二进制 SHA-256 校验通过；契约/实现复核、日志和清单见 `build-codex-agent/agent-phase6-slice134-evidence/`。

边界：旧版缺 marker 的 3D line_pair 不能仅凭保存数据追认或纠正，继续按旧兼容规则处理。未运行全量 CTest；最近已知全量回归有独立的间歇性 viewport SIGSEGV。G6 标准 3 其余坐标审计、G6 最后一项及阶段 6 整体保持 `IN_PROGRESS`。

## Slice 135：Agent 2D segment_gap 变换坐标闭环（PASS，限定测试子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。仅新增 `testAgentQuickSegmentGapCoordinatesSurviveViewportTransforms`，不改生产代码：固定来源 ID/尺寸/偏移，在缩放并平移后的 2D 视口上经 Agent 工具启动 `segment_gap`，再执行四次真实鼠标点击。逐步核对 1/2/3/4 个完成步骤及等待/完成状态，四个 `ImageLocalPixel` 局部坐标和精确来源映射；overlay 端点、两条线长度、最近点和三位小数间距标签均按实际拾取坐标独立计算期望值。

新增 QtTest 用例单独 **3/3 项通过**（含 init/cleanup）；完整 `test_mainwindow` CTest **1/1 通过**；全量构建退出码 **0**。clang-format、`git diff --check`、独立审核及源码/二进制/证据 SHA-256 校验通过。构建、完整/单用例测试日志及审核记录见 `build-codex-agent/agent-phase6-slice135-evidence/`。

边界：只补 Agent 2D `segment_gap` 的缩放/平移与来源映射闭环，不外推到其它测量类型、3D 算法、全部图像空间插件或 G6 标准 3 整体完成。未运行全项目 CTest；其既有 viewport 间歇性 SIGSEGV 风险仍未定位，G6 最后一项仍缺真实登录权限保护操作的拒绝证据，阶段 6 保持 `IN_PROGRESS`。

## Slice 136：Agent 2D angle_3_points 变换坐标闭环（PASS，限定测试子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。仅新增 `testAgentQuickAngleCoordinatesSurviveViewportTransforms`，不改生产代码：固定来源 ID/尺寸/偏移，在缩放并平移后的 2D 视口经 Agent 工具启动三点角度请求，再执行三次真实鼠标点击。逐步核对请求状态、三个 `ImageLocalPixel` 拾取及来源映射；overlay 三个点和两条射线与实际拾取一致，期望角度由实际三点独立计算为度数，并核对近 90°边界和两位小数标签。

完整构建退出码 **0**；完整 `test_mainwindow` CTest **1/1 通过**。clang-format、`git diff --check`、独立审核及源码/二进制/证据 SHA-256 校验通过。构建、CTest、审核记录和摘要见 `build-codex-agent/agent-phase6-slice136-evidence/`。

边界：只补 Agent 2D `angle_3_points` 变换坐标/来源映射闭环，不外推到 `point_to_line`、`circle_3_points`、其它测量类型或 G6 标准 3 整体完成。未运行全项目 CTest；既有 viewport 间歇性 SIGSEGV 风险仍未定位，G6 最后一项仍缺真实登录权限保护操作拒绝证据，阶段 6 保持 `IN_PROGRESS`。

## Slice 137：Agent 2D point_to_line 变换坐标闭环（PASS，限定测试子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。仅新增 `testAgentQuickPointToLineCoordinatesSurviveViewportTransforms`，不改生产代码：固定来源 ID/尺寸/偏移，在缩放并平移后的 2D 视口通过 Agent 请求和三次真实鼠标点击完成点到有限线段测量。逐步核对状态、`ImageLocalPixel` 及来源映射；overlay 点、输入线段与 P 到垂足的线按实际拾取坐标验收。测试独立计算投影参数、垂足和距离，并断言 `0<t<1`。

全量构建退出码 **0**；完整 `test_mainwindow` CTest **1/1 通过**。clang-format、`git diff --check`、独立审核和源码/二进制/证据 SHA-256 校验通过；日志与摘要见 `build-codex-agent/agent-phase6-slice137-evidence/`。

边界：只补 Agent 2D `point_to_line` 变换坐标/有限线段投影，不外推到 `circle_3_points`、3D 点线算法或 G6 标准 3 整体验收。未运行全项目 CTest；既有 viewport 间歇性 SIGSEGV 风险仍未定位，G6 最后一项仍缺真实登录权限保护操作拒绝证据，阶段 6 保持 `IN_PROGRESS`。

## Slice 138：Agent 2D circle_3_points 变换坐标闭环（PASS，限定测试子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。仅新增 `testAgentQuickCircleCoordinatesSurviveViewportTransforms`，不改生产代码：固定来源 fixture，在缩放/平移后的 2D 视口通过 Agent 工具及三次真实鼠标点击完成圆测量，核对逐步状态和来源映射。测试从实际拾取点用独立向量公式计算圆心/半径，并核对三个输入点、圆心 `C` 标记及 64 条圆周线段的逐段几何与 `R/C` 标签。

首次完整 `test_mainwindow` 运行暴露契约/测试遗漏：生产 overlay 还有圆心标记。契约补入第 4 个 `C` 点及实现复核后，聚焦用例 **3/3 QtTest 项通过**，完整 `test_mainwindow` CTest **1/1 通过**；首轮失败日志保留在同一证据目录。最终全量构建退出码 **0**，clang-format、`git diff --check` 与最终源码/二进制/证据 SHA-256 校验通过。完整/聚焦运行、首轮失败及复跑日志见 `build-codex-agent/agent-phase6-slice138-evidence/`。

边界：仅补 Agent 2D `circle_3_points` 变换坐标/overlay 证据，不外推到其它圆生产者、3D 圆算法或 G6 标准 3 整体完成。未运行全项目 CTest；既有 viewport 间歇性 SIGSEGV 风险仍未定位，G6 最后一项仍缺真实登录权限保护操作拒绝证据，阶段 6 保持 `IN_PROGRESS`。

## Slice 139：ShowPoint 输出局部像素单位与 Agent key 查询（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立审核 **CONTRACT_PASS**、实现复核 **IMPLEMENTATION_PASS**。ShowPoint metadata 仅为 `point_x` / `point_y` 声明当前输入图像网格的 `px` 单位；`point_displayed` 保持 Boolean 且无单位，未新增输出端口或改变绘制算法。Acceptance flow 通过测试专用 GrabImage fixture 将有效 `QPointF` 放入图像 sidecar，经真实 RunEngine 执行 ShowPoint，再由 Agent 以同一 runId 按 key 查询三项结果。带非零 provenance 时，两个标量仍等于局部输入值、不附来源映射；输出图像 provenance 原样保留。

全量构建退出码 **0**；`test_acceptance_flows` 与既有 ShowPoint `test_finalplugins` **2/2 suites 通过**；新增用例单独 **3/3 QtTest 项通过**（含 init/cleanup）。Metadata JSON、clang-format、`git diff --check`、独立复核和源码/二进制/证据 SHA-256 校验通过。证据见 `build-codex-agent/agent-phase6-slice139-evidence/`。

边界：仅声明有效二维输入点相对于当前 ShowPoint 输入图像的像素单位，不推导父图坐标、物理尺度或三维含义，也不覆盖其它插件。未运行全项目 CTest；G6 标准 3 仍有其余插件/3D 路径待审，G6 最后一项仍缺真实登录权限保护操作拒绝证据，阶段 6 保持 `IN_PROGRESS`。

## Slice 140：FitCircle 标量动态像素单位（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**；实现经同一独立审核 **IMPLEMENTATION_PASS**。Agent 仅对稳定 FitCircle 插件 ID 的 `circle_center_x`、`circle_center_y`、`circle_radius`、`circle_error`，在同次输出 `coordinate_frame=ImageLocalPixel` 且存在有效 `Circle2D` 时，为 key/port 查询附加 `px`。未知、缺失或 World3D 框架不附单位；独立负例在 ImageLocalPixel 但无 Circle2D 时仍可读原值且不返回 `unit`、`coordinateSystem`、`coordinateFrame` 或 `sourceImageCoordinates`。不改 metadata、拟合算法、端口或标量值；Circle2D 的来源圆心映射保持原样，标量不映射来源坐标。

全量构建退出码 **0**；`test_acceptance_flows` CTest **1/1 通过**；三个相关 QtTest 用例 **5/5 通过**（含 init/cleanup）。clang-format、`git diff --check`、独立复核和最终源码/二进制/证据 SHA-256 校验通过。证据见 `build-codex-agent/agent-phase6-slice140-evidence/`。

边界：只补 FitCircle 输出在 Agent 查询时的条件性单位，不外推至其它圆插件或 World3D 物理尺度。未运行全项目 CTest；既有 `test_agent_datasource_dedicated_viewport` 间歇性 SIGSEGV 风险仍未定位，G6 标准 3 其它插件/3D 路径和 G6 最后一项真实登录权限保护操作拒绝证据仍未完成，阶段 6 保持 `IN_PROGRESS`。

## Slice 141：3DPreProcessing 高度统计单位边界（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。`get_module_output` 说明 `min_height`、`max_height` 是有效输入高度值原数值尺度的统计量；预处理不做物理单位换算，输入物理单位与外部 Z 基准未知，不得推断 mm 或 World3D。明确排除 `valid_pixel_count`、`filtered_pixel_count` 与 `height_invalid_value`。

扩展现有真实 TIFF `GrabImage → 3DPreProcessing → FitPlane` RunEngine 流：在同一成功 runId 上对两个高度统计分别按 key/port 查询，与同次 `moduleOutput("pre")` 值完全一致，并确认响应不含单位、坐标框架或来源映射字段。ToolSchema 回归锁定说明片段。全量构建退出码 **0**；受影响 CTest `test_3d_batch3`、`test_toolschema` **2/2 通过**；两个聚焦用例分别 **3/3、3/3 QtTest 项通过**（含 init/cleanup）。clang-format、`git diff --check`、独立复核和最终源码/二进制/证据 SHA-256 通过。证据见 `build-codex-agent/agent-phase6-slice141-evidence/`。

边界：不声明高度值的物理尺度、外部 Z 参考系或 mm 单位，不改变预处理与下游算法。未运行全项目 CTest；已知 viewport suite 间歇性 SIGSEGV 风险仍未定位，G6 标准 3 其它空间算法以及 G6 最后一项真实登录权限拒绝证据未完成，阶段 6 仍为 `IN_PROGRESS`。

## Slice 142：FitPlane 五项标量的缩放坐标单位语义（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。ToolSchema 说明 `plane_d`、`flatness`、`max_deviation`、`min_deviation`、`rms` 保留 `pixelSizeX/pixelSizeY/zScale` 缩放坐标中的数值，只有三轴具有一致外部工程单位依据时才可作物理量解释；不得根据参数名推断毫米。法向量分量无量纲、`valid_pixel_count` 为计数，说明范围限定在五项标量。FitPlane 源码的“物理系数”“残差 mm”“步长 1mm”三处改为缩放坐标数值语义，公式/输出不变。

真实 HeightMap→FitPlane 流对五项标量分别按 key 与 port 查询，共十项；Agent 值与同次 RunEngine 输出一致，响应均无 `unit`、坐标框架或来源坐标字段。`test_toolschema` 与 `test_acceptance_flows` CTest **2/2 通过**；两个聚焦 QtTest 用例分别 **3/3、3/3 通过**（含 init/cleanup）；全量构建退出码 **0**。clang-format、`git diff --check`、独立复核与源码/二进制/证据 SHA-256 通过。证据见 `build-codex-agent/agent-phase6-slice142-evidence/`。

边界：不校准或赋予缩放轴物理单位，不改 FitPlane 数值和算法。未运行全项目 CTest；`test_agent_datasource_dedicated_viewport` 间歇性 SIGSEGV 仍未定位，G6 标准 3 与阶段 6 仍有其它验收项，整体保持 `IN_PROGRESS`。

## Slice 143：PointSurfaceDistance 距离单位边界（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。`get_module_output` 明确 `PointSurfaceDistance.distance` 是输入 XYZ 数值中的垂直距离，插件不换算单位；`coordinate_frame=World3D` 仅复述输入框架，不声明单位、轴定义或外部基准，因此不得推断 mm 或物理单位。说明只针对 `distance`，未扩展到 `foot_x/y/z` 或 `foot_point`。

扩展真实 `GrabImage → CropImage → MeasurementInput(point_plane) → PointSurfaceDistance` Agent `run_flow`：使用 `runOutputSnapshot(runId, "distance", ...)` 作同 run oracle，按 key/port 查询距离，值均等于快照 `7.0`，且响应不含 `unit`、坐标框架或来源坐标字段。既有 foot_point World3D/provenance 验收保持通过。全量构建退出码 **0**；`test_acceptance_flows` 与 `test_toolschema` CTest **2/2 通过**；聚焦 QtTest 分别 **3/3、3/3 通过**（含 init/cleanup）。clang-format、`git diff --check`、独立复核及源码/二进制/证据 SHA-256 通过。证据见 `build-codex-agent/agent-phase6-slice143-evidence/`。

边界：不赋予距离毫米单位或外部世界基准，不改变算法、垂足或其它字段。未运行全项目 CTest；既有 viewport suite 间歇性 SIGSEGV 风险未定位，G6 标准 3 和阶段 6 仍有其余验收项，保持 `IN_PROGRESS`。

## Slice 144：PointSurfaceDistance 旧垂足标量键框架标签（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。Agent 只对稳定 PointSurfaceDistance 插件的 `foot_x/y/z` key 查询在全部条件满足时添加 `coordinateFrame=World3D`：同次输出框架精确 World3D、`foot_point` 为三个有限数值，查询 scalar 是有限数值 QVariant，且严格等于对应垂足分量。缺任一证据时仍返回原 key 值但不添加框架、单位或来源映射；恶意/异常字符串不会通过 `toDouble()` 被误标。ToolSchema 明确该标签只重复输入框架、不声明物理单位/外部基准或图像来源。

真实 Agent RunEngine 流通过 `runOutputSnapshot(runId, "distance", ...)` 对照三个旧 key 和 `foot_point` 同次分量；无框架、Unspecified、ImageLocalPixel 流仍保留 scalar 且不标框架；独立 fixture 证明 World3D 下字符串 scalar 保持原字符串而不标框架。既有 `foot_point` typed 输出断言保持通过。全量构建最终退出码 **0**；`test_acceptance_flows` 与 `test_toolschema` CTest **2/2 通过**；三个相关接受用例 **5/5 QtTest 项通过**（含 init/cleanup），schema 用例 **3/3**。首轮编译因 Qt5 `QStringList` 花括号赋值歧义失败，修正后构建通过；首轮日志保留。clang-format、`git diff --check`、独立复核和源码/二进制/证据 SHA-256 通过。证据见 `build-codex-agent/agent-phase6-slice144-evidence/`。

边界：不为垂足 scalar 声明单位或外部世界坐标，不改算法、旧数值、端口或 typed `foot_point` 行为。未运行全项目 CTest；viewport suite 间歇性 SIGSEGV 仍未定位，G6 标准 3 及阶段 6 的其余验收仍待完成，整体保持 `IN_PROGRESS`。

## Slice 145：EdgeDefectDetection 缺陷表列单位（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**。在 Slice101 现有同源门禁上扩展 `defect_regions`：仅稳定 EdgeDefectDetection 的同次输出为 `point_set` / `ImageLocalPixel`，且唯一 `image` 与 `reference_edge` 数据连接分别来自同一个 `MeasurementInput.image` / `MeasurementInput.fit_points` 时，key 或 port 查询返回 `columnUnits={max_deviation:px, mean_deviation:px}`；表响应不添加顶层 `unit`。角度、索引、计数和极性列不继承 px，也不推导坐标或来源映射。现有顶层三个偏差标量单位规则改为复用同一来源 gate。

扩展真实 RunEngine/Agent 用例：半径 100 px 的测试图配 95 px 参考点、`searchLength=10` 与 `threshold=2`。正例及来源拆分、point_pair、World3D 三个负例都断言同 run 快照至少含一行；key/port 查询各取全页，以同 run `runOutputSnapshot` 逐行核对 `max_deviation` 和 `mean_deviation`。正例只出现两项列单位，负例不返回 `columnUnits`，所有表响应均无顶层 `unit`。

验证：完整 `cmake --build build-codex-agent -j2` 退出码 0；`test_acceptance_flows` 与 `test_toolschema` CTest **2/2 通过**；Slice145 聚焦 QtTest 用例及 `test_toolschema` 已通过。`clang-format` 与 `git diff --check` 干净。构建、定向测试和聚焦用例原始日志、SHA-256 清单与独立实现复核结论归档于 `build-codex-agent/agent-phase6-slice145-evidence/`。

边界：不改 EdgeDefectDetection 算法、表字段、顶层数值、分页协议或其它插件。未运行全项目 CTest；已知 `test_agent_datasource_dedicated_viewport` 间歇性 SIGSEGV 风险未定位，G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 146：FitLine 兼容端点 scalar 单位门控（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现独立复核 **IMPLEMENTATION_PASS**。四个 legacy `line_row1/line_col1/line_row2/line_col2` key/port 仅在稳定 FitLine 插件、同次 `coordinate_frame=ImageLocalPixel`、metadata 声明 Line2D，且 `line` 为四个有限数值 `[col1,row1,col2,row2]`、查询 scalar 有限并精确匹配对应分量时添加 `unit:px`。缺少框架或 typed 输出、非图像框架、无效端点和 scalar 错配均保留原值但不添加单位或坐标/来源声明。`line_error` Slice97 行为以及 typed Line2D 和来源映射保持不变；`line_phi/rho` 未扩展。

真实 `GrabImage → CropImage → MeasurementInput(point_set) → FitLine` Agent 流通过同一 runId 的 `runOutputSnapshot` 核对四个 scalar 与 Line2D 对应分量，key/port 均检查 finite 值、`px` 和不附来源坐标。新增独立错配 fixture：稳定 FitLine ID、真实 Line2D metadata、ImageLocalPixel、有效有限四端点，但四个 scalar 全部错配；四个 key/port 保持可读且不被误标。已有缺失框架、World3D、局部无 provenance 和失败清理回归继续覆盖。

验证：`cmake --build build-codex-agent -j2` 退出码 **0**；定向 CTest `test_fitline`、`test_toolschema`、`test_acceptance_flows` **3/3 通过**；错配、真实正例、缺失/World3D 聚焦 QtTest 各 **3/3 通过**。clang-format 与 `git diff --check` 干净。构建、测试、独立复审和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice146-evidence/`。

边界：不改拟合算法、端点、角度/法向距离定义、typed Line2D 或来源映射。未运行全项目 CTest；G6 标准 3 及阶段 6 整体验收继续 `IN_PROGRESS`，已知 viewport suite 间歇性 SIGSEGV 风险仍未定位。

## Slice 147：LinesDistance 最近点 scalar 单位门控（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现独立复核 **IMPLEMENTATION_PASS**。四个 `nearest_x1/y1/x2/y2` key/port 仅在稳定 LinesDistance、同次 `line_pair` / `ImageLocalPixel`、metadata Line2D、唯一且 from/to 端口对应的 image/line1/line2 数据连接来自同一稳定 MeasurementInput，以及有限 `nearest_segment=[x1,y1,x2,y2]` 与被查 scalar 精确匹配时附加 `unit:px`。Scalar 不附来源坐标映射；`distance` 仍无单位；typed Line2D 既有来源映射不变。

真实 `GrabImage → CropImage → MeasurementInput(line_pair) → LinesDistance` Agent 流按同一 runId 快照逐一核对四个 scalar 与 `nearest_segment` 分量；四个 key/port 均验证数值有限、值一致、px 和无来源坐标。source 分裂/未知生产者、模式不匹配、World3D 框架负例保持原 scalar 可读但不带单位；独立同源 ImageLocalPixel fixture 让四个 scalar 与有效线段全部错配，验证不会误标。

验证：完整构建退出码 **0**；定向 CTest `test_linesdistance`、`test_toolschema`、`test_acceptance_flows` **3/3 通过**；真实裁剪正例、source/mode/frame 门控、错配 fixture 三个聚焦 QtTest 各 **3/3 通过**。clang-format、`git diff --check` 干净。构建、测试、独立复审与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice147-evidence/`。

边界：不为 `distance` 标单位，不改最近点算法、Line2D 或来源映射。未运行全项目 CTest；G6 标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 148：NPointCalibration 标定点坐标域说明（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现独立复核 **IMPLEMENTATION_PASS**。真实 `get_module_params_schema` 现在说明 `calibrationPoints.image` 是当前输入图像局部像素 `(x,y)`，不是屏幕坐标或自动映射的原始相机像素；`world` 是调用者提供的目标平面 `(x,y)`，单位、原点和参考框架由调用者定义，插件不转换为毫米或固定外部世界坐标。`reprojection_error` 明确沿用 `calibrationPoints.world` 的调用者定义数值尺度，不表示固定物理单位。ToolSchema 要求使用嵌套参数说明解释坐标字段，禁止从字段名推断毫米、固定 world reference 或图像来源 provenance。

扩展既有真实 `GrabImage → NPointCalibration` Agent/RunEngine 用例，逐层断言 image/world 参数说明和误差单位标签；同次运行的 `reprojection_error`、`point_count` key/port 值和失败后结果不可用断言保持通过。标定矩阵、数值、端口和 provenance 行为未变。

验证：完整构建退出码 **0**；`test_toolschema` CTest 通过；聚焦 `testG3NPointCalibrationOutputPortsByRunId` 与兼容 `testF04CalibrationPointPairsRunAndMigrate` 各 **3/3 QtTest 项通过**。`test_agentphase0_repro` CTest **32 项通过、1 项环境失败**，仅 `testG1PlcFlowZeroSideEffectBeforeConfirmation` 因本环境不能绑定 localhost listener 失败，与本切片无关。metadata JSON、clang-format 与 `git diff --check` 通过。构建、回归、独立复审与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice148-evidence/`。

边界：只澄清坐标域和调用者定义单位，不实现图像到世界反变换、不声明外部标定基准或固定物理单位。未运行全项目 CTest；G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 149：MeasureGap 二维点对单位门控（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现独立复核 **IMPLEMENTATION_PASS**。稳定 MeasureGap 的 `gap`、`gap_distance`、`gap_delta_x/y` 仅当同 run 的 MeasurementInput 源快照证明 `point_pair`、`ImageLocalPixel`、二维有限点，唯一连接来自稳定 MeasurementInput 且 from/to 端口对应，同时 MeasureGap 报告 `measurement_dimension=2d` 并且被查 scalar 精确匹配该点对期望值时，按 key/port 标 `px`。距离校验使用与 MeasureGap 插件一致的 `sqrt(dx*dx + dy*dy + dz*dz)` 运算式，避免 `hypot` 的舍入差异。`gap_delta_z` 不标单位；不添加坐标框架或来源图映射。

真实二维流程对四个 scalar 使用同 run `runOutputSnapshot` 核对 MeasureGap key/port 数值，并核实 MeasurementInput 源快照的模式、框架和两点。新增非整数坐标二维真实流，四个 scalar 的 key/port 均须返回 `px`。负例覆盖三维非零 Z、三元素且 Z=0（源仍标 World3D 而 MeasureGap 报 2d）、交叉 fromPort、伪造 ImageLocalPixel 的非目标来源，以及同源输入的有限错配输出；各 scalar 仍可读但不得带单位或坐标/来源声明。ToolSchema 描述同步明确这些边界。编码初期测试揭示 MeasureGap 输出未保留 MeasurementInput 的 mode/frame 元数据，门控现已改为读取同 run 上游快照；独立复审另发现 `hypot` 舍入差异，已按插件算式修复并加非整数回归。

验证：`cmake --build build-codex-agent -j2` 全量构建成功；定向 CTest `test_acceptance_flows`、`test_toolschema`、`test_measuregap` **3/3 通过**，其中接受流套件包含二维、三维及上述门控用例。clang-format 与 `git diff --check` 干净。构建、测试、实现复核及 SHA-256 证据见 `build-codex-agent/agent-phase6-slice149-evidence/`。

边界：不改 MeasureGap 算法、metadata 数值或输出字段，不给三维结果赋像素单位、不生成来源坐标映射。未运行全项目 CTest；G6 标准 3 和阶段 6 仍有其他验收项，阶段状态继续为 `IN_PROGRESS`。

## Slice 150：Agent 工程/流程工具遵守产品用户权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` **CONTRACT_PASS**，实现独立复核 **IMPLEMENTATION_PASS**。`UserManager::isLoggedIn()` / `hasPermission()` 现在在 AgentController 请求门和 AgentActor 实际执行门共用：`create_project→NewSolution`、`open_project→Open`、`save_project→Save`、工程编辑及数据源引用/导入→`Edit`、单次运行/单步→`RunOnce`、cycle→`RunCycle`、pause/resume/stop→`Stop`。待确认批次会在执行时重查当前权限；权限拒绝返回 `PERMISSION_DENIED`、`retryable=false` 与 `details.requiredPermission`，批次和确认后拒绝保留所需权限标识。用户角色权限表和密码策略未修改。

回归经 Agent 工具与确认路径验证：Operator（`Open=true`、`Edit/Save/NewSolution=false`）不能新建或保存，且拒绝保存不改文件字节；Operator 能打开已有工程；Developer 可新建并保存；未登录拒绝受保护操作而只读工具仍可用。Advisor 批次批准前，Operator 降权或登出都会拒绝待执行的模块备注修改，工程状态不变。既有 AgentActor、Controller、E2E、Undo、接受流及 Bridge 相关测试基线改为显式 Developer，避免默认 Operator 的差异造成隐式测试前置条件。

验证：全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_toolschema`、`test_agentactor`、`test_agentpermissions`、`test_agentundo`、`test_agentcontroller`、`test_agente2e`、`test_acceptance_flows` **7/7 通过**；Bridge 权限确认聚焦 QtTest 通过。定向扩展运行中，`test_agent_sam_tools`、`test_agentphase0_repro`、`test_agent_plcwrite_execution_g5` 的失败均为本环境拒绝本地 TCP listener，`test_agentbridge` 的 listener 用例因本地 socket 监听被拒而失败；Bridge 的权限确认用例基线已改为 Developer。clang-format 与 `git diff --check` 干净。构建、测试、复审和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice150-evidence/`。

边界：只关闭 Agent 工程/流程工具这组产品权限绕过路径；GUI 按钮、全局变量、相机、硬件、通信、SAM、知识库和插件加载/卸载权限不属于此切片。G6 最后一项及阶段 6 整体继续 `IN_PROGRESS`。

## Slice 151：Agent 全局变量工具产品权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**，实现后独立复核 **IMPLEMENTATION_PASS**。Agent 的全局变量列举、读取、创建、更新、删除及对话框入口统一要求登录且有 `GlobalVar` 权限；对 Guest 类型额外 fail closed，以覆盖 Developer 切换到 Guest 后权限位残留的情况。普通工具拒绝返回 `PERMISSION_DENIED`、`retryable:false`、`status:denied` 和 `details.requiredPermission`。手动/Bridge 确认没有原始 assistant tool call，因此不合成 tool 消息；权限变化后通过既有响应信号告知未执行，并记 `denied` 审计，回归检查两者。待确认创建在操作员权限降级或登出后批准均拒绝，变量不创建。Administrator/Operator/未登录状态的查询和变更拒绝且既有变量快照不变；Developer 通过工具路径列举、读取和增删改。Observer 混合批次在残留权限位的 Guest 状态验证全局变量项按 callId 拒绝、无关插件只读项成功，变量快照不变且不泄露名称和值。GUI 全局变量对话框对 Operator 拒绝、Developer 可打开。

验证：`cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agent_globalvars_handoff`、`test_toolschema`、`test_agentactor`、`test_agentpermissions`、`test_agentundo`、`test_agentcontroller` **6/6 通过**。`clang-format` 与 `git diff --check` 干净。原始构建/测试日志和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice151-evidence/`。阶段 6 与 G6 最后一项继续 `IN_PROGRESS`。

## Slice 152：Agent 相机工具产品权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**，实现后独立复核 **IMPLEMENTATION_PASS**。Agent 相机枚举、发现、连接/断开、相机详情和操作查询要求 `Camera`；参数写入要求 `CameraSet`。共享权限门还对 Guest 的继承权限位 fail closed，但保留已登录 Guest 明确授予的 `RunOnce`、`RunCycle`、`Stop`；登出后这些权限也不能替代登录态。Developer→Guest 混合 Observer 批次里，`list_cameras` 按 callId 返回 `PERMISSION_DENIED`，`get_available_plugins` 成功，且变量快照不变。Operator/Administrator、Guest 和未登录状态拒绝相机操作；确认期间降权拒绝连接，不改变 CameraManager 已知设备列表。相机生命周期既有回归显式登录 Developer。

验证：`cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agent_globalvars_handoff`、`test_toolschema`、`test_agentactor`、`test_agentpermissions`、`test_agentundo`、`test_agentcontroller`、`test_agent_camera_lifecycle_g5` **7/7 通过**。`clang-format` 与 `git diff --check` 干净。构建/测试日志与 SHA-256 清单见 `build-codex-agent/agent-phase6-slice152-evidence/`。阶段 6 和 G6 最后一项继续 `IN_PROGRESS`。

## Slice 153：Agent 模块插件生命周期系统配置权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice54_contract_review_round2` 独立契约审核 **CONTRACT_PASS**，实现后独立复核 **IMPLEMENTATION_PASS**。`load_module_plugin` 与 `unload_module_plugin` 现在映射到 `SystemConfig`，只读 `get_available_plugins` 不变。实现沿用共享产品权限门和执行时复验；Guest 即使继承旧的 `SystemConfig` 位也被拒绝。Reviewer 确认角色映射与 `UserManager` 既有定义一致：Developer/Administrator 允许，Operator 拒绝。基于本地 `AgentSchemaCacheFixture` 的批准前切换 Guest、降为 Operator 和登出三条回归均证明插件未加载、没有 loaded/unloaded/loadFailed/toolExecuted 信号，返回明确未执行说明并记 denied 审计；不补造 tool 消息。

验证：`cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agentpermissions`、`test_agent_plugin_g5`、`test_agentcontroller`、`test_toolschema` **4/4 通过**。`clang-format` 与 `git diff --check` 干净。原始构建/测试日志及源码/二进制 SHA-256 清单见 `build-codex-agent/agent-phase6-slice153-evidence/`。阶段 6 与 G6 最后一项继续 `IN_PROGRESS`。

## Slice 154：Agent 知识库文档工具文件权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 已独立契约审核 **CONTRACT_PASS**。`read_documentation`、`list_knowledge_documents`、`preview_knowledge_document` 映射 `OpenFile`；`add_knowledge_document`、`remove_knowledge_document` 映射 `SaveFile`。沿用 Slice150 的登录态与执行时复验，以及对 Guest 残留权限位 fail closed 的共享逻辑；工具描述同步说明所需产品文件权限。

隔离临时资料库测试覆盖 Operator 可列举/预览/查阅但不能导入或删除，拒绝为 `PERMISSION_DENIED` 且包含 `requiredPermission`、不泄露文档内容，源文件与已存在资料库目标保持不变；Developer 成功导入，Administrator 成功导入；Guest 在继承 `OpenFile`/`SaveFile` 位时仍被拒绝，未登录读取被拒绝。Developer 提交的新增和删除待确认操作在降为 Operator 后批准均被执行门拒绝；另有删除在批准前登出后拒绝。源文件和已有资料库文件逐字节不变，发出未执行说明和 denied 审计，手动确认路径没有合成会话 tool 消息。

全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agent_knowledge_folder_handoff`、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller` **4/4 通过**。独立实现审核 **IMPLEMENTATION_PASS**；审核指出的登出复验和源文件逐字节校验已补齐。AgentController 混合权限回归同步改为 Guest 的知识库读取明确按 `OpenFile` 拒绝，并在每个测试前重置 Developer 登录，避免角色状态串扰。`clang-format`、`git diff --check` 干净，9 项源码/二进制/文档 SHA-256 校验通过。证据见 `build-codex-agent/agent-phase6-slice154-evidence/`。阶段 6 和 G6 最后一项仍为 `IN_PROGRESS`。

## Slice 155：Agent 标注会话文件工具权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**。`get_annotation_session` 与 `open_active_annotation_session` 要求 `OpenFile`；`save_active_annotation_session`、`sam_annotate`、`export_annotation_session`、`transform_annotation_session` 要求 `SaveFile`。执行前统一复验登录/角色权限，Guest 的残留文件权限位 fail closed；工具说明同步标明权限需求。

回归覆盖 Operator 能读会话、不能经四种写工具创建目标；Developer/Administrator 的权限门允许，Guest/未登录即使权限位残留也拒绝。Developer 提交打开会话待确认后切 Guest，批准时拒绝且文件、活动会话、prompt/candidate 和 Undo 状态不变；Operator 仍可确认打开。新增导出在确认后降为 Operator、转换在确认后登出均无目标文件且源会话不变；保存活动会话在确认后降为 Operator也不落盘。AgentController 混合权限测试现在在 Guest 下断言知识库读取按 `OpenFile` 拒绝，并在各测试前恢复 Developer 登录。

全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agent_sam_annotator_handoff`、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller` **4/4 通过**。独立实现审核 **IMPLEMENTATION_PASS**。SAM 撤权回归不启动网络服务，以确认路径未发出 `toolExecuted` 证明未进入推理实现。`clang-format`、`git diff --check` 干净，12 项源码/二进制/文档 SHA-256 校验通过。证据见 `build-codex-agent/agent-phase6-slice155-evidence/`。单独运行 `test_agent_sam_tools` 时，本机受限环境拒绝测试 TCP stub 监听（`peerA.listen()`）；失败日志保存在证据目录，该套件未作为通过项计入。阶段 6/G6 仍 `IN_PROGRESS`。

## Slice 156：Agent 活动 SAM 查看/编辑权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**。`sam_predict` 和活动对象/类别/提示状态查询使用 `OpenFile`；活动 mask/category 编辑使用 `Edit`。取消瞬态提示不增加 UserManager 产品登录/权限门，但保留 `dangerous` 确认策略。

新增回归覆盖 Operator 活动读取允许、编辑拒绝，Developer/Administrator 权限门放行，Guest 残留位与未登录读取/编辑拒绝；`sam_predict` 在批准前切 Guest、`sam_annotate` 在批准前降为 Operator 均不触发 `toolExecuted`/SAM 请求，输入图片不变。MainWindow/SAM 对话框回归覆盖 Guest 下待确认打开拒绝、Operator 仍可打开，Operator 降级后待确认编辑零副作用，以及登出状态下经 Autopilot 确认取消瞬态提示，保留已确认标注、Undo/Redo。

全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agent_sam_annotator_handoff`、`test_toolschema`、`test_agentpermissions`、`test_agentcontroller` **4/4 通过**。独立实现审核 **IMPLEMENTATION_PASS**。SAM 本地 stub 测试没有计为通过项；撤权回归在工具执行前阻断，未连接 SAM 服务。`clang-format`、`git diff --check` 干净，11 项源码/二进制/文档 SHA-256 校验通过。证据见 `build-codex-agent/agent-phase6-slice156-evidence/`。阶段 6/G6 仍 `IN_PROGRESS`。

## Slice 157：Agent 通信模块配置权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立契约审核 **CONTRACT_PASS**。Agent 添加 `metadata.category=communication` 的插件，以及通过 `set_param`、`set_module_params`、`reset_module_params` 修改通信模块参数，除既有 `Edit` 外还要求 `CommunicationSet`。普通非通信模块继续只要求 `Edit`；流程拓扑、布局及其它编辑不变。判定基于 `PluginManager::pluginInfo()` 的 metadata 类别，因此当前六类通信插件与后续通信类别插件共用同一规则。权限顺序先 `Edit`，Admin 因缺少 `CommunicationSet` 得到明确拒绝。

权限回归覆盖 Developer 允许、Administrator 通信操作拒绝但普通模块允许、Operator/Guest/登出拒绝；检查 `add_module` 和三种参数工具的权限结果、Admin 拒绝前后工程快照不变。另验证 Developer 发起参数修改待确认后切换为 Administrator，批准时重新校验并拒绝，工程快照不变、审计结果为 denied；没有连接设备。

全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agentpermissions`、`test_agentcontroller`、`test_toolschema` **3/3 通过**。`clang-format --dry-run --Werror`、`git diff --check` 均通过。独立契约审核 **CONTRACT_PASS**，整改后独立实现复核 **IMPLEMENTATION_PASS**；构建、测试、门禁原始日志及源码/二进制/文档 SHA-256 见 `build-codex-agent/agent-phase6-slice157-evidence/`。未连接真实设备。阶段 6/G6 仍 `IN_PROGRESS`。

## Slice 158：GUI 通信设置遵守产品权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立契约审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。MainWindow 的通信设置入口要求已登录且拥有 `CommunicationSet`；Admin、Operator、Guest 和登出时不创建通信对话框并显示权限提示。直接构造的 `CommunicationSetView` 未授权时不读取配置列表，禁用配置/连接控件；对话框打开后权限变化会清空列表、名称、IP、端口、串口和状态，并在每个操作槽执行时再次检查权限。Developer 保留现有查看、配置和连接入口。

回归使用隔离配置；覆盖 Developer 创建/应用，窗口中 Developer 切 Operator 后字段清空（包括已加载端口显示为“—”）、所有操作按钮禁用，直接调用 apply/delete/connect/disconnect 槽不改变配置和连接状态；Admin/Operator/Guest/登出构造的视图无配置数据。真实 MainWindow 入口验证四类未授权拒绝及 Developer 授权打开。排查并修正了撤权清表发出 selection 信号导致权限刷新重入的问题。

全量构建 `cmake --build build-codex-agent -j2` 成功；`test_communicationmanager` CTest **1/1 通过**（该套件内 9 项通过、1 项既有 PLC TCP listener 测试因本机无法监听而按条件跳过）；MainWindow 新增用例单独运行 **3/3 通过**，未将全套 `test_mainwindow` 记为通过。`clang-format --dry-run --Werror`、`git diff --check` 通过。构建/测试原始日志和源码、测试二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice158-evidence/`。未连接真实设备。阶段 6/G6 仍为 `IN_PROGRESS`。

## Slice 159：共享系统参数页遵守 SystemParam 权限（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立契约审核 **CONTRACT_PASS**，实现复核 **IMPLEMENTATION_PASS**。审查确认 `SystemParamView` 当前只含通用自动加载/保存、语言、主题、运行周期和日志设置，HardwareConfig、DeviceParam、SystemParam 菜单及 Agent `open_system_settings` 都是同一页面的入口别名；因此四个入口统一要求已登录且拥有 `SystemParam`，消除同一控件通过别的菜单绕权。Developer/Administrator 可打开；Operator、Guest 和未登录拒绝。真正的硬件/设备专属配置页尚未实现，本 Slice 不声称对应权限域已完成。

MainWindow 三个入口在显示前进行门控，Agent `open_system_settings` 在发起 GUI 事件前由 AgentActor 权限预检拒绝；直接构造的 `SystemParamView` 未授权时不读取配置并显示空白禁用控件。窗口打开后切换 Guest 或登出会立即清空/禁用全部设置控件，保存/重置槽再次检查权限。测试使用区分度较高的 SystemConfig sentinel 值，验证未授权构造不泄露，Developer 授权后能加载，降权和登出后不能保存/重置或改变配置；Guest 的真实 Controller 请求返回 `PERMISSION_DENIED` 且不发 GUI action。

全量构建 `cmake --build build-codex-agent -j2` 成功；定向 CTest `test_agentpermissions` **1/1 通过**；MainWindow 权限矩阵和 Agent 系统设置生命周期/主题刷新两个定向测试各 **3/3 通过**。格式门禁与 `git diff --check` 通过。证据及源码/测试二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice159-evidence/`。完整 `test_mainwindow` 未计为通过。阶段 6/G6 仍为 `IN_PROGRESS`。

## Slice 160：GUI 相机设置遵守 Camera 与 CameraSet 权限（PASS）

契约 本地逐切片契约（未纳入提交） 经独立契约审核 **CONTRACT_PASS**，实现独立复核 **IMPLEMENTATION_PASS**。MainWindow 相机设置入口及直接构造的 `CameraSetView` 均要求登录且同时具有 `Camera`、`CameraSet`；Administrator、Operator、Guest 和登出状态拒绝。视图打开后撤权会清空设备列表及连接参数、禁用操作控件，所有操作槽再次检查权限。插件加载完成回调启动发现前复核权限；撤权期间迟到的相机列表、状态、发现和操作信号不会读取/展示设备缓存或重填表格；恢复权限后须显式刷新。已经启动的异步相机操作保持既有生命周期语义。

回归覆盖 MainWindow 角色入口及可见拒绝提示；Administrator/Operator/Guest/登出直接构造和操作槽零发现副作用；撤权前创建、撤权后收到插件加载完成信号不启动发现；迟到 `cameraListChanged` 不重填；恢复权限不自动发现。已有四个 CameraSetView/异步交互用例显式使用 Developer。定向执行这四个 CameraSetView QtTest 和 MainWindow 入口 QtTest，各 **3/3 通过**。全量构建成功。完整 `test_cameramanager` CTest **1/1 通过**（测试用例全量运行，22.22 秒）；首次运行暴露新增异步假相机发现未排空及计数基线不充分，已补注册后等待、`asyncStarts()` 门禁断言和退出前排空，首次失败日志保留为 v1。上述五个切片相关定向用例也单独通过。构建与定向测试证据见 `build-codex-agent/agent-phase6-slice160-evidence/`。实现独立审核待完成，阶段 6/G6 继续 `IN_PROGRESS`。


## Slice 161：GUI 全局变量遵守 GlobalVar 权限（PASS）

契约 本地逐切片契约（未纳入提交） 已经独立契约审核 **CONTRACT_PASS**。MainWindow 全局变量菜单和共享 `GlobalVarView` 要求登录及 `GlobalVar`；当前角色表仅 Developer 获准。视图初始未授权时不显示变量；撤权立即清空表格和类型选择并禁用变量操作，Close 保持可用。变量管理信号在撤权后不回填数据；所有读取及操作槽在读取快照/产生副作用前检查权限。恢复权限本身不加载变量，Agent 对隐藏视图的明确授权重开请求会受保护地刷新。

测试覆盖 Administrator/Operator/Guest/登出直接构造与所有操作槽、独特 sentinel 不泄露、撤权后新增/删除/值变更/清空信号不重填，以及 Agent 隐藏对话框撤权后拒绝、恢复后授权复用并刷新。`test_globalvarview`、`test_agent_globalvars_handoff` 已定向通过 **2/2**；MainWindow 全局变量菜单入口测试 **3/3** 通过。全量构建成功，独立实现审核 **IMPLEMENTATION_PASS**；定向 CTest `test_globalvarview`、`test_agent_globalvars_handoff` **2/2 通过**，MainWindow 入口用例 **3/3 通过**。格式门禁、`git diff --check` 退出码均为 0，18 项源码/二进制/文档/日志 SHA-256 校验通过。证据见 `build-codex-agent/agent-phase6-slice161-evidence/`。阶段 6/G6 继续 `IN_PROGRESS`。

## Slice 162：GUI 工程文件操作遵守产品权限（PASS）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**，实现复审 **IMPLEMENTATION_PASS**。MainWindow 新建方案要求 `NewSolution`，方案列表要求 `SolutionList` 与 `Open`，打开要求 `Open`，保存要求 `Save`；所有 slot 先验证登录、非 Guest 和所需权限，再触及 busy 状态、工程、日志、桥接或对话框。Developer→Guest/登出时 `UserManager::updatePermissions()` 先清除旧权限位；入口仍检查身份以 fail closed。未保存切换与退出仅向有 `Save` 权限的用户显示“保存后继续”；选择保存后、另存为对话框返回后、实际写盘前再次验证权限。直接 `onSaveProject` 的另存为路径也执行相同复验。

回归覆盖 Administrator/Operator/Guest/登出权限矩阵，Administrator 保存/列表/打开授权对照，Operator 打开授权入口，以及拒绝时工程对象、JSON、路径、修改标志、最近项目、磁盘字节均保持不变。Developer 在未保存确认框或 Save As 对话框打开时降权，切换路径和直接保存路径都拒绝写盘且不创建目标文件。Operator 退出未保存工程时没有 Save 按钮，Cancel 保留窗口和工程，Discard 允许退出且原工程字节不变。

全量构建 `cmake --build build-codex-agent -j2` 成功；`test_mainwindow` CTest **1/1 通过**（163 个 QtTest，0 失败）；四个 Slice162 聚焦用例组合 **6/6 通过**。`clang-format --dry-run --Werror` 和 `git diff --check` 通过。构建/测试日志及源码、测试二进制 SHA-256 见 `build-codex-agent/agent-phase6-slice162-evidence/`。阶段 6/G6 仍为 `IN_PROGRESS`。

## Slice 163：GUI、内容、权限与模型环境前置条件证据（PASS，限定 G6 子项）

契约 本地逐切片契约（未纳入提交） 与证据经独立复核 **PASS**。无 GUI 的纯 UI 请求断言返回 `REQUIRES_GUI`；无图像的错误码按入口区分：viewport zoom 与 SAM annotator 返回 `NO_IMAGE`，二维/三维快速测量缺少图像/点云时返回 `VIEWPORT_CONTENT_UNAVAILABLE`。无 SAM Python/模型时，环境交接报告 `environmentSource=missing`、`serverState=not_started`、`userActionRequired=true`，不泄露本地路径且不启动后端。登录权限不只检查提示或权限查询值：Agent 工具和 MainWindow GUI 操作均有实际 `UserManager` 权限门控；阶段 150–162 的角色矩阵、撤权复验和拒绝零副作用回归作为依据。无 GUI 场景没有窗口可截图，使用直接返回断言；其余界面测试保存操作前后截图并验证状态和行为。

全量构建成功；`test_mainwindow` CTest **1/1 通过**（163 个 QtTest），`test_agent_viewport_zoom` CTest **1/1 通过**，SAM 前置条件定向用例 **3/3 通过**；修正 Guest/登出旧权限预期和逐测试 Developer fixture 后，SAM handoff、知识库 handoff、权限与插件相关 CTest **4/4 通过**。测试截图、初始失败日志、构建/测试日志和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice163-evidence/`；清单校验通过。独立复审确认修正后的错误码映射、权限证据及截图充分。

边界：`test_agent_datasource_dedicated_viewport` 在当前 offscreen 环境仍因 OpenGL context 失败/SIGSEGV，不计为通过证据；无 GUI 路径由独立 MainWindow handler 测试覆盖。完整 `test_agent_sam_annotator_handoff` 的初始失败日志保留，失败根因为测试断言仍预期 Guest 保留旧权限；修正后已由相关 4 套 CTest 通过。此切片只关闭 G6 前置条件子项，不表示 ROI/测量/标注坐标与单位标准（G6 标准 3）或阶段 6 整体完成；阶段 6 仍为 `IN_PROGRESS`。

## Slice 164：SAM 标注 GUI 鼠标坐标贯通裁剪来源映射（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**，测试实现经独立审核 **IMPLEMENTATION_PASS**。仅新增 `testSamAnnotatorDialog::mainViewOverlayCoordinatesMapThroughCropProvenance`，不改产品代码或 Agent 协议：共享 `HImageWidget` 携带来源 ID `slice164-parent`、320×240 来源尺寸和 `(50,40)` 裁剪原点；在 200×150 图像、320×240 viewport 上 zoom=2 并通过真实中键拖动 `(10,10)→(25,5)` 平移。真实屏幕点 `(235,160)` 被核实为图像局部像素 `(80,60)`、父图 `(130,100)`；真实屏幕框 `(155,100)→(275,220)` 被核实为图像局部端点 `(40,30)→(100,90)`、父图 bbox `(90,70,60,60)`。局部与来源坐标都对照固定真值，并以 provenance 映射函数交叉核对。

构建 `test_samannotatordialog` 目标成功；新增聚焦 QtTest **3/3 通过**（含 init/cleanup），完整套件运行时新用例也通过。完整 `test_samannotatordialog` CTest **失败**：64 项通过、5 项既有本地 SAM TCP stub 用例因沙箱拒绝 `SamTestServer::listen()` 失败；该 CTest 未记为通过。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建/测试日志、源码/二进制/契约 SHA-256 与复审结论见 `build-codex-agent/agent-phase6-slice164-evidence/`。

边界：只闭合 SAM 标注真实 GUI 提示输入在 zoom/pan 下与 crop provenance 的组合测试，不表示其它 ROI/测量/3D/标注路径全部通过。G6 坐标标准 3 与阶段 6 仍为 `IN_PROGRESS`。

## Slice 165：共享图像 provenance 更新与像素状态隔离（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立 **CONTRACT_PASS**，实现经独立 **IMPLEMENTATION_PASS**。根因是共享 `HImageWidget::setImage(image, provenance)` 在同像素图重新绑定来源时仍发 `imageLoaded`，而 `SamAnnotatorDialog` 旧监听只比较 `QImage`，导致 annotation session 保存的来源映射过期。现在 provenance 等价按有效性、来源 ID、来源尺寸和有效 3×3 映射矩阵比较；同像素 provenance 变化只更新 session lineage，保留局部提示、标注、mask、preview 和撤销栈；像素图变化仍完整清理旧状态并更新图像摘要。等价的 origin 与显式 matrix 编码不触发状态变化。

新增共享图像视口回归通过真实鼠标事件创建点/框提示，并用 `confirmObject` helper 注入对象和 mask，无需连接 SAM 服务。用例分别覆盖等价映射、sourceSize、矩阵、source ID、valid→invalid→valid 更新；逐次核对完整 prompt snapshot（含 box）、对象、mask 图像、preview、undo count/index 和 session provenance。来源 B 下旧对象 bbox 重新映射到新父图坐标，新鼠标点按局部固定值映射至 B；同像素 provenance 变化保留局部工作。像素改变时状态清空、Undo/Redo 不复活旧对象，`imageDigest` 与新像素摘要相等。既有 `activeAnnotationObjectsReportSourceBboxWhenVerified` 与 `switchImageSameSizeResetsAnnotationState` 回归保持通过。

构建 `test_samannotatordialog` 目标成功；Slice165、既有来源 bbox、同尺寸换图清理及 Slice164 GUI crop provenance 聚焦用例各 **3/3 QtTest 项通过**（含 init/cleanup）。完整 `test_samannotatordialog` CTest **未通过**：65 项通过，5 项既有 SAM 本地 TCP stub 测试因环境禁止 `SamTestServer::listen()` 失败；未将整套 CTest 标为通过。clang-format、`git diff --check` 和证据 SHA-256 校验通过；构建/测试日志与源码、契约、二进制清单见 `build-codex-agent/agent-phase6-slice165-evidence/`。

边界：此切片修复 SAM 共享视口 provenance 持久化与像素换图隔离，不覆盖其它标注窗口或所有 ROI/测量插件，不关闭 G6 坐标标准 3 或阶段 6 整体验收。

## Slice 166：裁剪来源下真实 SAM 鼠标提示与 Agent 取消（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立 **CONTRACT_PASS**，测试经独立 **IMPLEMENTATION_PASS**。新增 `sharedViewMousePromptsCancelWithoutLosingConfirmedAnnotation`：共享视口在 zoom/pan 和非零 crop provenance 下通过真实鼠标输入正点、负点与 box，局部/来源坐标均对照固定真值。测试通过 `confirmObject` 注入一个已确认对象/mask，再注入本地 preview，不启动 SAM server。调用 Agent `cancelAgentPrompt()`（不混用会清空 Undo 的 GUI Cancel 按钮）后，点/框/preview/候选清空、二次取消返回 unchanged；确认对象、mask 像素及 Undo 栈数量/索引保持原值。随后真实鼠标点只落入当前局部坐标并映射到当前来源 `(140,110)`。

`test_samannotatordialog` 构建成功；Slice166 聚焦 QtTest **3/3 通过**（含 init/cleanup）。完整 `test_samannotatordialog` CTest **未通过**：66 项通过、5 项既有 SAM 本地 TCP stub 用例因环境禁止 `SamTestServer::listen()` 失败；失败套件未计为通过。格式门禁、`git diff --check` 与 SHA-256 清单通过，日志与复审结论见 `build-codex-agent/agent-phase6-slice166-evidence/`。

边界：只补真实 GUI 提示输入与 Agent prompt cancel 的组合证据，不覆盖真实 server 取消/迟到网络响应或所有 ROI/测量/标注路径；G6 坐标标准 3 与阶段 6 整体验收仍为 `IN_PROGRESS`。

## Slice 167：Agent 矩形 ROI 固定屏幕坐标与来源映射（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；新增 MainWindow GUI 用例经独立实现审核 **IMPLEMENTATION_PASS**。测试先显示带来源 provenance 的 200×150 图像，再固定 `HImageWidget` 为 320×240，执行 `actualSize()`、zoom=2 和真实中键 pan `(10,10)→(25,5)`。Agent `rectangle_area` 使用固定屏幕拖框 `(155,100)→(275,220)`，不调用产品坐标转换函数构造事件或预期。断言四角局部坐标 `(40,30),(100,30),(100,90),(40,90)` 与来源坐标 `(90,70),(150,70),(150,130),(90,130)`、四角坐标帧及可用性字段、`completedSteps=1`，并逐项验证 TL/BR overlay、闭合四边和 `BL→TL` 上 `W=60.00, H=60.00, A=3600.00` 标签。

`test_mainwindow` 目标构建成功；Slice167 聚焦 QtTest **3/3 通过**（含 init/cleanup），全套 MainWindow CTest **1/1 通过**。`clang-format --dry-run --Werror`、`git diff --check` 与 Slice167 源码/测试/二进制/日志 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice167-evidence/`。

边界：只提供 Agent 二维矩形 ROI 在缩放、平移与平移 provenance 下的固定屏幕坐标证据，不覆盖其它 ROI/测量类型、旋转/缩放 provenance、SAM 以外的全部标注入口或 3D 路径；G6 坐标标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 168：3D 测量 overlay 保留双精度世界坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立审核 **IMPLEMENTATION_PASS**。`MeasurementOverlayPoint3D` 和 `MeasurementOverlayLine3D` 改为双精度 `Eigen::Vector3d`；Agent 五种 3D 快速测量保留原始采点与直接连接线端点精度，普通 `MeasurementInput(point_pair)` 从 JSON 参数创建 overlay 时也保留双精度。仅在最终 Qt/OpenGL 投影入口将点转换为 `QVector3D`；不声明相机矩阵、GPU 或大坐标可视投影具备双精度。

`testAgent3DQuickMeasurementArithmeticUsesDoublePrecision` 覆盖距离、点线、线段间隙、角度、三点圆五种模式，逐分量核对状态响应、全部原始 overlay 点和对应直接测量边。`testG6MeasurementInput3DWorldCoordinatesReachRunOutput` 核对固定双精度点对的 ordinary MeasurementInput JSON 参数、P1/P2 overlay 与连线端点，并保持同次 RunEngine 输出断言。

构建 `test_mainwindow` 目标成功；两个聚焦 QtTest 组合 **4/4 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**。格式门禁、`git diff --check` 与源码/测试/二进制/日志 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice168-evidence/`。

边界：双精度保证止于 overlay 数据模型，不代表 Qt/OpenGL 相机矩阵、屏幕投影或 GPU 绘制的精度提升，也不修复旧 `QVector3D` 点选信号本身的 float 限制；不定义 World3D 物理单位或外部基准。G6 坐标标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 169：相机变换后的 Agent 3D 真实拾取闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立审核 **IMPLEMENTATION_PASS**。新增 MainWindow Agent 3D 距离测量 GUI 回归：请求启动后向真实 `Viewport3DContent` 注入 wheel zoom 和 orbit 拖拽；独立 `CameraController` oracle 按同一包围盒和变换投影点云目标。对每个取整点击位置，测试证明其到目标小于 12 px、到其它每个点至少 14 px。相机拖动保持 0 个采点步骤；之后两个真实 press/release 都命中确定的原始点云点，Agent 状态返回精确 `World3D/pointCloudWorld`，overlay 的 P1/P2 及连线端点与点云值逐分量一致。

Slice169 聚焦 QtTest **3/3 通过**（含 init/cleanup）；完整 `test_mainwindow` CTest **1/1 通过**。在 offscreen 环境会记录 OpenGL context 不可用警告；本测试仅验证 CPU 投影/点拾取和 Agent 状态，不将 GPU 渲染视为通过证据。格式门禁、`git diff --check` 与源码/测试/二进制/日志 SHA-256 校验通过；证据见 `build-codex-agent/agent-phase6-slice169-evidence/`。

边界：只验收 3D 两点距离在 zoom/orbit 后的真实 Agent 采点链路，不扩展至其他测量类型、多个 Agent viewport、GPU 渲染、大坐标投影精度、物理单位或 World3D 外部基准；G6 坐标标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 170：相机变换后的普通 MeasurementInput 真实拾取闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；新增普通 `MeasurementInput(point_pair)` GUI 用例经独立实现复核 **IMPLEMENTATION_PASS**。根因是 `ProjectManager::newProject()` 会清空视口显示；测试先显示点云、后创建工程时，实际采点阶段已没有候选点。用例现在在工程创建后重新显示同一合成点云，并保持普通视口默认非 pick mode，通过 Ctrl+左键拾取。

用例通过 `onRunOnce()` 进入等待，注入真实 wheel zoom 和左键 orbit 后由独立 `CameraController` oracle 计算点击位置；检查点击与目标距离小于 12 px、与其它候选至少 14 px。orbit 不产生采点。两个 Ctrl+左键 press/release 后，核对工程参数、同一个成功 runId 的 RunEngine 快照、P1/P2 和线段端点均逐分量等于点云目标 XYZ。未修改产品代码或坐标协议。

`test_mainwindow` 目标构建成功；Slice170 聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**。切片代码范围 `clang-format --dry-run --Werror`、`git diff --check` 与独立复审通过；证据见 `build-codex-agent/agent-phase6-slice170-evidence/`。

边界：仅验收普通 GUI `MeasurementInput(point_pair)` 在 zoom/orbit 后的两点拾取与同 run 输出，不覆盖其它模式、多个视口、GPU 绘制、大坐标投影精度、World3D 物理单位或外部基准；G6 坐标标准 3 与阶段 6 整体继续 `IN_PROGRESS`。

## Slice 171：普通 2D MeasurementInput 变换与来源映射真实拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；新增普通 `MeasurementInput(point_pair)` GUI 用例经独立实现复核 **IMPLEMENTATION_PASS**。测试在工程创建后显示带有效 translation provenance 的 80×60 图像，把 `HImageWidget` 固定为 240×180，执行 `actualSize()`、zoom=2 和真实中键 pan `(15,-5)`。两个固定屏幕点击 `(121,89)`、`(221,149)` 独立对应局部像素 `(13,17)`、`(63,47)`，再映射来源图 `(23,37)`、`(73,67)`。

真实 `point2DSourceClicked` 信号核对局部/来源坐标与来源 ID；普通流程等待状态、工程 `point1/point2`、同一成功 runId 的 MeasurementInput/DistancePP 快照，以及 P1/P2 和连线 overlay 均分别核实。DistancePP 距离为 `sqrt(50²+30²)`，overlay 和工程运行值保持当前图像局部像素坐标，不被来源映射改写。

`test_mainwindow` 目标构建成功；Slice171 聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**。切片代码范围格式检查、`git diff --check` 与独立复核通过；证据见 `build-codex-agent/agent-phase6-slice171-evidence/`。

边界：只验收普通二维点对在缩放、平移和 translation provenance 下的拾取闭环，不覆盖 Agent 快速测量、其它模式/视口、其它 ROI、GPU 或外部标定；G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 172：真实 PLY 逐轴缩放与 Agent 点云预览坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；扩展 `testAgentFreeformSurfaceResultsRetainUnknownUnits` 经独立实现复核 **IMPLEMENTATION_PASS**。本地四点 PLY 经 `LoadPointCloud(scaleX=2, scaleY=3, scaleZ=4)` 后，固定坐标为 `(0,0,0)`、`(2,0,1)`、`(2,3,2)`、`(0,3,0.5)`。测试使用同一成功 runId 的 `runOutputSnapshot` 读取真实 LoadPointCloud `PointCloudData`，核对四点与固定真值。

Agent 对同一 run 查询 `point_cloud` 并打开 preview；summary 的 `value.pointCount/hasNormals/hasColors` 与快照一致，summary 与 preview 坐标系统均为 `unspecified`，preview 四点逐项等于同一快照和固定值。两者均不附物理单位、`coordinateFrame` 或来源图坐标。没有改插件、算法或 Agent 协议。

`test_acceptance_flows` 目标构建成功；Slice172 聚焦 QtTest **3/3 通过**（含 init/cleanup），所属 `test_acceptance_flows` CTest **1/1 通过**。切片代码范围格式检查、`git diff --check` 与独立复核通过；证据见 `build-codex-agent/agent-phase6-slice172-evidence/`。

边界：只验收真实 PLY 加载器的逐轴缩放值与 Agent 点云预览，不扩展至 TIFF 高度物理标定、其它格式/GPU/外部基准；G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 173：普通 GUI 点集输入与 FitCircle 局部坐标闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；扩展 `testRunCreatesMeasurementInputForConsumer` 经独立实现复核 **IMPLEMENTATION_PASS**。工程在 80×60 图像、来源尺寸 120×100、translation provenance `(10,20)` 下自动创建 `MeasurementInput(point_set)`。`HImageWidget` 固定 240×180，经过 `actualSize()`、zoom=2 与真实中键 pan `(15,-5)` 后，固定屏幕点击 `(215,115)`、`(175,155)`、`(135,115)` 对应局部点 `(60,30)`、`(40,50)`、`(20,30)` 和来源点 `(70,50)`、`(50,70)`、`(30,50)`。来源点只由真实 `point2DSourceClicked` 信号核验。

三次点击驱动同一流程完成一次成功运行。按同一 runId 读取 MeasurementInput 与 FitCircle 快照，逐点核对局部点集、MeasurementInput 的 `ImageLocalPixel` 和 FitCircle 圆心 `(40,30)`、半径 20；UI overlay 的三个输入点与圆周线段仍在局部像素坐标。Viewport provenance 未被假定会进入 RunEngine 上游，因此 FitCircle 输出只按局部数值验收，不要求其复制框架标签或来源映射。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**。切片代码范围格式检查、`git diff --check` 与独立复核通过；证据见 `build-codex-agent/agent-phase6-slice173-evidence/`。

边界：只验收普通 GUI 三点集到 FitCircle 的缩放/平移和来源点击信号，不覆盖 Agent 快速测量、其它点集消费者/视口、透视 provenance、GPU 或物理单位；G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 174：FitPlane 缩放坐标法向量与 Agent 查询闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。保留真实 RunEngine `HeightMapFixture → FitPlane` 流：固定高度面和 `pixelSizeX=0.5`、`pixelSizeY=2.0`、`zScale=0.25` 推出 `Z=1.5X+0.5Y+2.5`。测试从平面方程独立计算朝 `+Z` 的单位法向量 `(-1.5,-0.5,1)/sqrt(3.5)`，并逐项核对同一成功 runId 快照中的 `plane_nx/plane_ny/plane_nz`，绝对误差小于 `1e-8`。

三个分量各自通过 Agent `get_module_output` 的 key 和 port 查询；每个响应均为 `available`，数值同时等于 RunEngine 快照与固定真值，且不含物理单位、坐标系统/框架或来源图映射。ToolSchema 明确这些值是在缩放 XYZ 轴中的无量纲单位法向量分量、方向按 FitPlane 算法朝 `+Z`，但不声明物理单位或外部世界参考系。算法、数值、Plane3D 端口和坐标转换均未更改。

两个目标构建成功。聚焦 QtTest：`testAgentFitPlaneUsesHeightMapScaledCoordinates` **3/3**、ToolSchema 语义断言 **3/3**（均含 init/cleanup）；`test_acceptance_flows` 与 `test_toolschema` CTest **2/2 通过**。切片代码范围格式检查、`git diff --check` 和独立实现审核通过；证据见 `build-codex-agent/agent-phase6-slice174-evidence/`。

边界：只验收当前 HeightMap fixture 的缩放平面和三个法向量查询，不代表所有平面拟合输入、硬件坐标标定或外部物理参考；G6 标准 3 和阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 175：普通 GUI 3D point_line 相机变换后真实采点（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**，实现经独立复核 **IMPLEMENTATION_PASS**。新增普通 `MeasurementInput(point_line)` 真实视口测试，精确复用 Slice170 的候选点云及 wheel zoom/orbit 相机事件。使用 Ctrl+左键依次命中固定 XYZ 点 `(5,-3,2)`、线端点 `(7,-3,2)` 和 `(5,-3,4)`，投影距离门禁确保目标与其它候选可区分。

等待阶段在前三次点击完成前不启动流程，进度依次为 1/3、2/3、3/3；最终只启动一次运行。工程及同一 runId 快照均为 point `[5,-3,2]`、line `[7,-3,5,-3]`，证明点保留 XYZ、线端点只取 XY；3D overlay 线端点 Z 为 0。坐标标记为 `Unspecified`，快照中 `source_coordinate_available` 明确存在且为布尔 false，来源 ID/尺寸/原点及 point/line 来源映射字段均不存在。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**。切片范围格式检查、`git diff --check` 与独立实现审核通过；offscreen 日志中的 OpenGL context 警告不影响本测试的真实事件/参数/运行快照断言。证据见 `build-codex-agent/agent-phase6-slice175-evidence/`。

边界：只覆盖普通 GUI 的此类 3D 混合点/线交互，不扩展至 Agent 发起采点、物理单位、来源图坐标或其它视口/模式；G6 标准 3 与阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 176：普通 GUI 3D line_pair 相机变换后真实采点（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；最终实现经独立审核 **IMPLEMENTATION_PASS**。新增普通 `MeasurementInput(line_pair)` 真实视口测试，使用四个目标 `(4,-4,1)`、`(6,-4,1)`、`(4,-2,3)`、`(6,-2,3)` 和两个边界候选固定点云包围盒；相机经过真实 wheel zoom 与轨道旋转后，独立 oracle 投影检查视口边界、点击误差和对全部候选的唯一命中间距。

真实 Ctrl+左键依次采集两条线的四个端点，进度 1/4 至 4/4，前三次点击不会启动流程，第四次后 `runStarted`、`moduleStarted`、`runFinished` 均恰好一次且运行成功。工程参数与同一 runId 快照均为 `line1=[4,-4,6,-4]`、`line2=[4,-2,6,-2]`；线端点 Z 忽略，overlay 四点和两条线均位于 Z=0。工程及输出标为 `Unspecified`，来源坐标可用标志显式为 false，且没有来源图 ID/尺寸/原点或线来源字段。

目标构建成功；聚焦 QtTest **3/3**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（43.99 秒）。切片范围格式检查、`git diff --check` 与独立实现审核通过。首轮 bbox 角点在 zoom/orbit 后投影到视口外，失败日志留在 `focused-test.v1.log`；修订为 bbox 内目标与边界候选后通过。offscreen OpenGL context 警告记录在日志中，不影响鼠标事件、命中和数据断言。证据见 `build-codex-agent/agent-phase6-slice176-evidence/`。

边界：仅覆盖普通 GUI 三维 line_pair 混合坐标语义，不覆盖 Agent 发起采点、物理单位、来源图坐标或其它视口/模式；G6 标准 3 和阶段 6 整体继续 `IN_PROGRESS`。

## Slice 177：MeasurementInput 未标记坐标 fail-closed（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立契约审核 **CONTRACT_PASS**；实现经独立复核 **PASS（仅限 Slice177）**。`MeasurementInput(point_line/line_pair)` 默认与 markerless 旧参数均保持 `Unspecified`；只有显式 `ImageLocalPixel` marker 才产生 typed 图像坐标和来源映射。Agent 修改坐标参数会使旧 marker 失效，三种参数工具均不能设置或清除 readOnly marker，未知 marker 也 fail-closed。测试模拟 GUI 写入既有 marker 后确认 Agent reset 被拒且 marker 保留。

`DistancePL` 对有限输入继续输出相同垂足 `QPointF` 数值。框架为 `Unspecified` 时 Agent 对 `key=foot_point` 和 `port=foot_point` 均返回 `type=QVariantList` 与原始 `[x,y]`，不附坐标系或来源映射；明确 `ImageLocalPixel` 时保留 port 的 typed `Point2D` 与映射，key 保持通用 `QPointF` 序列化。带有效图像 provenance 的 markerless fixture 验证来源变换不能补足缺失的 marker。

`test_acceptance_flows` 目标构建成功；Slice177 聚焦 QtTest **7/7 通过**（含 init/cleanup），`test_acceptance_flows` 与 `test_mainwindow` CTest **2/2 通过**（分别 6.66 秒、45.34 秒）。相关文件 clang-format dry-run、`git diff --check` 与独立实现复核通过。构建、测试、审查、格式和哈希证据见 `build-codex-agent/agent-phase6-slice177-evidence/`。

边界：本切片只收紧 MeasurementInput `point_line/line_pair` 的坐标来源声明并保留 DistancePL 原始结果，不扩展到其他几何插件、ROI、SAM 标注、物理单位或其它来源映射路径；G6 标准 3 仍需覆盖其余操作清单路径，阶段 6 保持 `IN_PROGRESS`。

## Slice 178：Agent 矩形 ROI 旋转/缩放来源映射（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；新增 MainWindow 用例经独立实现审核 **IMPLEMENTATION_PASS**。测试使用固定 320×240 图像控件、真实 actual-size/2x zoom/中键 pan 和固定屏幕矩形事件；在行优先 provenance 矩阵 `[0,-2,300,1.5,0,10,0,0,1]` 下，四角 image-local 坐标保持 `(40,30)`、`(100,30)`、`(100,90)`、`(40,90)`，对应来源坐标为 `(240,70)`、`(240,160)`、`(120,160)`、`(120,70)`。逐角断言坐标框架、来源 ID、尺寸及可用性；四边 overlay 与 `W=60.00, H=60.00, A=3600.00` 标签继续使用局部像素。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（45.26 秒）。`clang-format --dry-run --Werror`、`git diff --check` 与独立实现审核通过。证据见 `build-codex-agent/agent-phase6-slice178-evidence/`。

边界：仅覆盖 Agent `rectangle_area` 在该仿射来源变换下的 2D ROI 证据，不外推至透视变换、其它 ROI/测量/标注类型或 3D；G6 标准 3 和阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 179：Agent 3D point_to_line 相机变换真实拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。两个 3D viewport 各自具有独立 oracle 和固定候选点云；wheel `+480` 与相同 orbit 后，对目标/非目标投影应用 `<12 px` 目标误差和每个非目标点 `≥14 px` 的唯一命中门禁。非目标有效点 Q 的真实鼠标点击不推进目标请求。目标请求点选 P 后在下一次 press 与 release 之间取消；迟到 release 后请求仍为 `cancelled`、进度为 1/3，唯一坐标仍是 `World3D [5,-3,2]`，overlay 状态不变。

第二个请求在同样相机变换后真实点选 P、L1、L2，进度 1/3、2/3、3/3，三项均逐值核对点云 XYZ。完成 overlay 验证 P/L1/L2 标记、线段 L1→L2、测点到垂足 `(6,-3,3)`，参数 `t=0.5`、距离 `sqrt(2)`（标签 1.414）。完成后的迟到 press/release 前后，完整 pickedCoordinates 和 overlay 点/线标签与端点快照逐项一致。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（45.48 秒）。格式门禁、`git diff --check` 与独立实现审核通过；证据见 `build-codex-agent/agent-phase6-slice179-evidence/`。初始两轮分别发现第二视口布局未完成、默认视角 Q 与 Z 邻点投影重合；通过显式等待布局，并同步第二视口的 zoom/orbit 与 oracle 修正，过程记录在 `iteration-notes.txt`。

边界：仅关闭 Agent 3D `point_to_line` 一类的相机变换/取消/跨视口交互子项，不外推至 `segment_gap`、`angle_3_points`、`circle_3_points` 或其它测量/标注路径；G6 标准 3 和阶段 6 仍为 `IN_PROGRESS`。

## Slice 180：Agent 3D segment_gap 相机变换真实拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。真实执行 `+480` wheel zoom 与固定轨道拖动后，独立相机 oracle 对 A1、A2、B1、B2 四次屏幕点击逐点执行 viewport 内、目标误差 `<12 px`、非目标至少 `14 px` 命中门禁。工具状态按 1/4、2/4、3/4、completed/4 推进，四项都逐值断言为固定的 `World3D/pointCloudWorld` XYZ。

overlay 四点标签和值为 L1-1/A1、L1-2/A2、L2-1/B1、L2-2/B2；三条线端点分别是 A1→A2、B1→B2 与唯一最近点 A2→B2。两段长度各 2，最近距离 `sqrt(5)`；对应标签 `2.000`、`2.236`。所有点和线端点逐分量核对 XYZ。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（46.27 秒）。格式门禁、`git diff --check` 与独立实现审核通过；证据见 `build-codex-agent/agent-phase6-slice180-evidence/`。

边界：仅覆盖 Agent 3D `segment_gap` 四步真实拾取和最近距离 overlay，不外推至 `angle_3_points`、`circle_3_points` 或其它坐标路径；G6 标准 3 与阶段 6 整体保持 `IN_PROGRESS`。

## Slice 181：Agent 3D angle_3_points 相机变换真实拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。单个 3D viewport 显示固定九点点云；相机执行真实 wheel `+480` 与 orbit 后，独立 oracle 为 V、P2、P3 三次鼠标事件逐一校验视口范围、目标误差 `<12 px` 和其余八个候选 `≥14 px` 距离门禁。

Agent 请求按 1/3、2/3、completed/3 推进；三项 `pickedCoordinates` 均逐值核对 `World3D` 与固定 XYZ。overlay 按顺序保存 V/P2/P3 三个双精度点与 V→P2、V→P3 两边；第二边标签断言包含固定真值 `90.00`。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（46.34 秒）。格式门禁、`git diff --check` 与独立实现审核通过；证据见 `build-codex-agent/agent-phase6-slice181-evidence/`。

边界：仅覆盖 Agent 3D `angle_3_points` 三点真实交互和 90° overlay，不外推至 `circle_3_points` 或其它坐标路径；G6 标准 3 和阶段 6 整体保持 `IN_PROGRESS`。

## Slice 182：Agent 3D circle_3_points 相机变换真实拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；新增 MainWindow 用例经独立实现审核 **IMPLEMENTATION_PASS**。固定点云含三个目标和七个非目标候选；视口真实执行 wheel zoom 与 orbit，独立 CameraController oracle 对 P1/P2/P3 的每次点击核验视口内位置、目标误差 `<12 px`，并核验到其余九个候选点的投影距离均 `≥14 px`。

Agent 请求按 1/3、2/3、completed/3 推进；三个 `pickedCoordinates` 逐值匹配 `World3D` 与固定 XYZ。overlay 点依次为 P1/P2/P3/C，圆心固定真值为 `(5,-3,2)`；独立有序基向量和 64 点圆周参数式逐项核对全部圆周线段的起终点与闭合顺序，端点误差 `<1e-9`。首段标签验证 `R=2.00` 和 `C=12.57`。

`test_mainwindow` 目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（46.01 秒）。切片范围格式门禁、`git diff --check` 和独立实现复核通过。offscreen 日志记录 AgentBridge 本地监听及 OpenGL context 告警；不影响事件、拾取、世界坐标和 overlay 断言。证据见 `build-codex-agent/agent-phase6-slice182-evidence/`。

边界：仅关闭 Agent 3D `circle_3_points` 三点真实采集和当前圆周 overlay 路径，不覆盖普通 GUI 圆测量、退化共线分支、物理单位、GPU 绘制或其它 3D 类型；G6 标准 3 和阶段 6 仍为 `IN_PROGRESS`。

## Slice 183：普通 GUI point_line 真实采点与 DistancePL 闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。复核和真实流程测试发现普通 GUI 2D `MeasurementInput(point_line)` 原先将图像像素点保存为三值 `[x,y,0]`，但 `DistancePL` 的 `parsePoint2D` 只接受二维点，无法完成预期连接。本切片将 `onPoint2DPicked` 的 2D `point_line` 点改为二维 `[x,y]`，不改变 3D 点线或 point_plane 路径。

新增 GUI 回归在 80×60 图像上执行 actual-size、2x zoom、真实中键 pan 和三次真实左键采点，运行 `MeasurementInput(point_line) → DistancePL`。固定局部点/线为 `[18,20]` 与 `[0,35,40,35]`；每次拾取信号都核对 source ID/尺寸、可用标志及局部/来源坐标 `(28,40)`、`(10,55)`、`(50,55)`。前两点仍等待且不运行；第三点后恰好一次成功运行。同一 run 快照核对二维点参数、ImageLocalPixel 框架、距离 `15`、垂足 `(18,35)` 和局部坐标 overlay。

来源映射边界也有负断言：MeasurementInput 与 DistancePL 输出的 `source_coordinate_available` 字段明确存在且为 Bool false，来源 ID、尺寸、原点和点/线映射字段均不存在。真实 viewport 点击信号所带 provenance 单独逐项核验；不把 viewport provenance 伪称为流程输入 provenance。新增回归同时保留既有 3D point_line XYZ 检查。

目标构建成功；聚焦 QtTest **4/4 通过**（含两个测试的 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（44.92 秒）。切片范围 clang-format、`git diff --check` 与独立实现复核通过；证据见 `build-codex-agent/agent-phase6-slice183-evidence/`。

边界：仅关闭普通 GUI 2D `point_line` 的点击维数兼容和当前 DistancePL 闭环，不覆盖 `line_pair`、Agent 快速测量、其它来源图像输入拓扑或单位标定；G6 标准 3 与阶段 6 仍为 `IN_PROGRESS`。

## Slice 184：普通 GUI line_pair 四点采集与 LinesDistance 闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。普通 GUI `MeasurementInput(line_pair)` 使用实际 2x zoom、中键平移及四次真实点击，流程按 1/4→2/4→3/4→4/4 前进，仅第四点后启动一次成功运行。source ID/尺寸 `slice184-source-image`/120×100、四个 image-local 与来源坐标、数据可用标志和 UI 参数均逐项核验。Slice183 的 `point`/`line` 与 Slice184 的 `line1`/`line2` 流程连接端口索引均按各插件 metadata 复核。

同 run `LinesDistance` 输出最近距离 `sqrt(125)` 和唯一最近段 `[30,10,35,20]`。MainWindow line_pair 结果 overlay 现在优先绘制消费者真实 `nearest_segment`；其不存在时继续沿用既有 midpoint fallback。测试证明第三条结果线端点为 `(30,10)→(35,20)`，而非两输入线段中点连线，标签为 `11.180`。MeasurementInput 和 LinesDistance 运行输出的 `source_coordinate_available` 明确存在且为布尔 false，来源 ID/尺寸/原点及 `line1_source/line2_source` 均不存在；viewport 点击信号单独携带并核验有效来源映射。

目标构建成功；Slice183/184 聚焦 QtTest **4/4 通过**（含初始化/清理），完整 `test_mainwindow` CTest **1/1 通过**（44.91 秒）。切片范围 clang-format、`git diff --check` 和独立实现复核通过；证据见 `build-codex-agent/agent-phase6-slice184-evidence/`。

边界：仅验证此普通 GUI `line_pair → LinesDistance` 路径的四点输入与精确最近段结果 overlay，不外推到其它消费者、midpoint fallback之外的分支或物理单位；G6 标准 3 与阶段 6 仍为 `IN_PROGRESS`。

## Slice 185：普通 GUI 3D point_plane 四点相机变换真实拾取（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**；新增 MainWindow 用例经独立实现审核 **IMPLEMENTATION_PASS**。固定九点点云含查询点 P 和非共线平面点 A/B/C；3D viewport 真实执行 wheel zoom 与 orbit，独立 CameraController oracle 对四次 Ctrl+左键按次检查 viewport 边界、目标误差 `<12 px` 和其它八个候选的最小投影间距 `≥14 px`。

GUI 等待状态依次 1/4、2/4、3/4、4/4，前三点后 RunEngine 未启动，第四点后恰好一次成功执行。world click 信号、Project 与同一 runId 的 MeasurementInput 快照逐值匹配 P `(4,-4,3)` 和 plane `[4,-4,0,6,-4,0,4,-2,0]`；坐标框架为 World3D。PointSurfaceDistance 同 run 输出 `distance=3` 和 `foot_point=[4,-4,0]`，typed 坐标框架为 World3D。输入/结果都明确无 source-image provenance；overlay 核验 P/A/B/C 和三条平面边的世界坐标。

目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（44.69 秒）。切片格式门禁、`git diff --check` 和独立实现复核通过。offscreen 日志记录本地 listener 和 OpenGL context 警告；本切片不宣称 GPU 呈现已验收。证据见 `build-codex-agent/agent-phase6-slice185-evidence/`。

边界：仅验证普通 GUI 3D `point_plane → PointSurfaceDistance` 固定点云交互路径，不外推至其它候选布局、Agent 快速测量、退化平面、物理单位或 GPU；G6 标准 3 和阶段 6 整体仍为 `IN_PROGRESS`。

## Slice 186：凹多边形 Region2D 裁剪 provenance 与 Agent 分页（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` 独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。扩展既有 `GrabImage → CropImage → SamPredict → RegionStats` 真 RunEngine 集成测试，只改测试。Fake SAM 固定返回六顶点凹 L 形，局部点顺序为 `(10,10),(20,10),(20,14),(14,14),(14,20),(10,20)`，bbox `(10,10,10,10)`；SAM 标签、分数分别为 `concave-L`、`0.97`。

CropImage 的非零裁剪把局部顶点按 `(80,70)` 平移到来源图像。Agent `get_module_output` 第一页与第二页逐项断言 `available`、六点总数、页范围、正常截断标记和 `image_pixels`/`source_image_pixels` 坐标系；拼接局部页与来源页后分别与六个固定有序顶点和逐点 provenance oracle 一致。来源 bbox 为 `(90,80,10,10)`。同 run RegionStats 的真实多边形面积为 `64 px²`、点数为 6、质心为 `(13.875,13.875)`；bbox 四项和面积/质心的 Agent key/port 返回值均逐项对照固定真值，质心 port provenance 为来源图像 `(93.875,83.875)`，key 保持普通 `QPointF` 响应。

目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_sam_region_flow` QtTest **20/20**，CTest **1/1 通过**。`clang-format --dry-run --Werror`、`git diff --check` 与独立实现复核通过；证据见 `build-codex-agent/agent-phase6-slice186-evidence/`。测试使用本地 fake SAM，不代表真实模型质量或 GPU/设备验收。

边界：仅验证当前非矩形 `Region2D` 经 CropImage 的平移 provenance、分页和 RegionStats 同 run 查询；不外推至透视映射、其它 ROI/标注生产者、GUI 手绘输入或 G6 标准 3/阶段 6 总体通过。

## Slice 187：GUI 凹多边形标注保存与来源 bbox（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` 独立审核 **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。测试在固定 320×240 共享视口中执行 actual-size、2× zoom 和真实中键平移，再以真实鼠标点击/拖拽生成 crop-local 正提示 `(80,60)` 和框 `(40,30)→(100,90)`。测试固定注入六顶点凹 L 轮廓；确认按钮前核对完整 preview 顶点顺序，确认后及会话保存/重开后继续核对 polygon、bbox、提示、对象 ID 和标签。

标注图像绑定来源 ID `slice187-parent-image`、尺寸 800×600 和仿射矩阵 `[2,0,10,0,3,5,0,0,1]`。保存和新对话框重开后的 AnnotationSession 均核对 provenance 完整矩阵、尺寸与来源 ID；对象 polygon/bbox/提示保持 crop-local。只读活动对象摘要明确标记 `ImageLocalPixel` 和来源可用，source bbox 为 `(50,65,80,105)`，不将 bbox 冒充来源 polygon。

目标构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整 `test_samannotatordialog` QtTest **72/72**，CTest **1/1 通过**。`clang-format --dry-run --Werror`、`git diff --check` 与独立实现复核通过；证据见 `build-codex-agent/agent-phase6-slice187-evidence/`。预测 polygon 为固定测试夹具注入，不代表真实 SAM 推理。

边界：仅验证当前 SAM GUI 对话框在该仿射 crop 图上的非矩形标注坐标与保存/重开 provenance；不外推至真实 SAM、透视变换、其它标注/ROI生产者、Agent 文件读取工具或 G6 标准 3/阶段 6 总体通过。

## Slice 188：GUI 流程运行入口权限门（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` **CONTRACT_PASS**；实现经同一独立 reviewer 最终 **IMPLEMENTATION_PASS**。MainWindow 快捷模式现在检查 `QuickMode`；单次运行及单步检查 `RunOnce`；循环运行检查 `RunCycle`。所有入口都在测量输入、模块同步、UI running state 和 RunEngine 调用前复验已登录且非 Guest。Operator 的单次/循环/单步权限保留，快捷模式拒绝；Guest 与登出拒绝四个新启动入口。

断点恢复权限按 RunEngine 实际暂停模式选择，而不按点击的按钮选择；RunOnce/RunCycle 两种暂停态均通过两个运行槽复测。Guest 拒绝后暂停状态、模式、runId 和模块执行数不变；Operator 批准后继续同一个运行。单次或循环运行活跃期间，Guest 与登出用户点击 `onRunOnce()` 均继续走既有安全停止路径。

目标构建成功；三个聚焦用例 **5/5 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**（45.45 秒）。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。构建、聚焦、完整 CTest、review 和 SHA-256 证据见 `build-codex-agent/agent-phase6-slice188-evidence/`。

边界：本切片不改变角色权限矩阵、Agent 工具权限、动态菜单/按钮启用状态、停止实现或设备专属配置权限；只关闭 GUI 流程执行入口的授权缺口，不代表阶段 6/G6 整体完成。项目全量 CTest 与现场设备未在本切片重新验证。

## Slice 189：GUI 快速矩形面积 ROI 局部坐标与连接生命周期（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经独立契约审核 **CONTRACT_PASS**；实现经 `/root/slice176_final_review` 独立复审 **IMPLEMENTATION_PASS**。普通 GUI 快速测量回调此前以 Qt5 不支持的 lambda `Qt::UniqueConnection` 连接，导致 ROI 结果处理器不可靠。本切片保存每个 viewport 的连接句柄，在退出快速测量或切换测量类型前显式断开并清空，再为当前会话建立一次连接；不改 `HImageWidget` 或面积算法。

新增用例通过真实菜单 data=5 选择矩形面积模式，并在 200×150 图像上完成 actual-size、2× zoom、中键平移 `(12,8)` 和真实左键拖拽 `(40,30)→(120,90)`。断言 ROI 恰产生一次且仍是图像局部坐标；overlay 恰有 TL/BR 两点和四条矩形边；结果标签与 Measurement 日志给出宽 80、高 60、面积 4800 px²。退出后重进并再次拖拽，第二会话只增加一条记录、不累积连接；无图模式仍能选取，但真实拖拽不产生 ROI 或 overlay。

目标构建退出码 0；新增用例聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_mainwindow` CTest **1/1 通过**（45.32 秒）；切片格式门禁与 `git diff --check` 通过。构建、测试、独立复审及 SHA-256 清单见 `build-codex-agent/agent-phase6-slice189-evidence/`。

边界：仅关闭普通 GUI 矩形面积 ROI 的局部坐标和回调连接生命周期子项；不外推至其它快速测量类型、Agent ROI、透视映射、物理单位或 GPU 呈现。G6 标准 3 与阶段 6 继续 `IN_PROGRESS`；项目全量 CTest、设备及 Windows 未在本切片验证。

## Slice 190：普通 GUI 两点距离缩放/平移坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` 独立审核 **CONTRACT_PASS**；纯测试实现经第二轮独立复核 **IMPLEMENTATION_PASS**。新增普通 GUI 快速“两点距离”回归，在 200×150 图像、320×240 视口执行 actual-size、zoom=2 和真实中键平移 `(12,8)`，从真实菜单选择 data=0，并以真实左键点击固定变换后的屏幕点 `(152,113)`、`(232,193)`。

断言图像局部点为 `(40,30)`、`(80,70)`，overlay P1/P2、线方向和距离标签 `56.569` 均匹配固定值。独立 provenance oracle 将两点映射到来源 `(90,95)`、`(170,215)`；GUI 快速测量仍使用 ImageLocalPixel。所有 Measurement 类快速测量结果日志恰有一条，全文精确匹配 `快速测量: 40.00,30.00 → 80.00,70.00 = 距离: 56.569`，并明确排除屏幕坐标串。

`test_mainwindow` 目标构建退出码 0；聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_mainwindow` CTest **1/1 通过**（45.19 秒）；格式门禁和 `git diff --check` 通过。构建、测试、审核结论及 SHA-256 证据见 `build-codex-agent/agent-phase6-slice190-evidence/`。

边界：仅验证普通 GUI 两点距离在该缩放/平移场景中的 image-local 点击和日志；不外推至其它测量类型、Agent/流程输入、来源坐标输出、多视口、换图/取消或物理单位。G6 标准 3 和阶段 6 继续 `IN_PROGRESS`；不代表项目全量 CTest、设备、Windows 或 GPU 已验证。

## Slice 191：普通 GUI 点到线与线段间距坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` 独立复核 **CONTRACT_PASS**；纯测试实现经独立复核 **IMPLEMENTATION_PASS**。同一 200×150 图像在 320×240 视口经过 actual-size、zoom=2 与真实中键平移后，新增普通 GUI 快速“点到线距离”“两线段间距”真实菜单与鼠标用例。

点到线模式真实点击屏幕 `(152,113)/(112,193)/(272,193)`，overlay 固定为局部点 `P=(40,30)`、`L1=(20,70)`、`L2=(100,70)`；线段 `L1→L2`、垂足 `(40,70)`、垂线和距离 `40.000` 均逐项断言。两线段模式真实点击 `(112,113)/(192,113)/(232,153)/(232,233)`，局部四点、两条输入线、最近连接 `(60,30)→(80,50)`、两段长度 `40.000` 和间距 `28.284` 均逐项断言。每模式只产生一条 Measurement 结果日志，文本与现有 GUI 实现逐字匹配，且明确排除对应屏幕坐标。仿射来源 oracle 的七个坐标逐项验证但不作为算法输入。

目标构建退出码 0；聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_mainwindow` CTest **1/1 通过**（45.14 秒）；格式与 `git diff --check` 通过。证据与哈希清单见 `build-codex-agent/agent-phase6-slice191-evidence/`。

边界：只覆盖普通 GUI 点到线和两线段间距的上述 2D 变换场景，不外推到角度、拟合圆、Agent/流程输入、换图/取消、多视口或 3D；G6 标准 3 和阶段 6 保持 `IN_PROGRESS`，项目全量 CTest、GPU、设备及 Windows 未在本切片验证。

## Slice 192：普通 GUI 三点角度与拟合圆坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` **CONTRACT_PASS**；实现经独立复审 **IMPLEMENTATION_PASS**。在同一仿射来源图、actual-size、zoom=2 与真实中键平移场景中，通过真实菜单和鼠标事件覆盖普通 GUI 快速三点角度及三点拟合圆。

角度三点的屏幕 oracle、局部坐标、V/P2/P3 overlay、两条射线、90.00° 结果标签与唯一日志均逐项断言。拟合圆固定三点 `(50,40)/(70,60)/(50,80)`，断言圆心 `(50,60)`、半径 20、周长 125.664、P1/P2/P3/C 标记和首段标签。64 条圆周线按固定参数式逐个核对起终点，坐标容差 `1e-9`，闭合端点也按相同容差断言。来源图像映射 oracle 独立逐点核验，overlay/算法输入仍为 image-local。

目标构建退出码 0；聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_mainwindow` CTest **1/1 通过**（45.32 秒）；格式与 `git diff --check` 通过。证据和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice192-evidence/`。

边界：只覆盖这两类普通 GUI 2D 快速测量的固定仿射 zoom/pan 场景；不覆盖退化共线圆、物理单位、来源坐标输出、Agent/流程路径、换图/取消、多视口、GPU 或设备。G6 标准 3 与阶段 6 继续 `IN_PROGRESS`。

## Slice 192：普通 GUI 三点角度与拟合圆坐标（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` **CONTRACT_PASS**；实现经独立复审 **IMPLEMENTATION_PASS**。在仿射来源图、actual-size、zoom=2 与真实中键平移场景中，通过真实菜单和鼠标事件覆盖普通 GUI 快速三点角度及三点拟合圆。

角度三点的屏幕 oracle、局部坐标、V/P2/P3 overlay、两条射线、90.00° 结果标签与唯一日志均逐项断言。拟合圆固定三点 `(50,40)/(70,60)/(50,80)`，断言圆心 `(50,60)`、半径 20、周长 125.664、P1/P2/P3/C 标记和首段精确标签。64 条圆周线按固定参数式逐个核对起终点，坐标容差 `1e-9`，闭合端点也按相同容差断言。来源图像映射 oracle 独立逐点核验，overlay/算法输入仍为 image-local。

目标构建退出码 0；聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_mainwindow` CTest **1/1 通过**（45.32 秒）；格式与 `git diff --check` 通过。证据和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice192-evidence/`。

边界：只覆盖这两类普通 GUI 2D 快速测量的固定仿射 zoom/pan 场景；不覆盖退化共线圆、物理单位、来源坐标输出、Agent/流程路径、换图/取消、多视口、GPU 或设备。G6 标准 3 与阶段 6 继续 `IN_PROGRESS`。

## Slice 193：CropImage 输出到普通 GUI 采点测量闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` **CONTRACT_PASS**；实现经独立复核 **IMPLEMENTATION_PASS**。新增 MainWindow 集成回归实际执行 `CropImagePlugin::execute`：160×120 来源图裁出 100×80 图像，裁剪原点 `(30,20)`，仿射来源矩阵按 CropImage 组合为 `[2,0,70,0,3,65,0,0,1]`。这同一个插件输出 `ImageData` 直接显示到 2D 视口，不以模拟裁剪 fixture 替代。

在裁剪输出上执行 actual-size、zoom=2、中键平移 `(12,8)` 和真实 `MeasurementInput(point_pair) → DistancePP` 单次运行。固定 screen 点击 `(142,108)/(262,188)` 对应 crop-local `(10,10)/(70,50)` 与 source `(90,95)/(210,215)`。首点后流程仍等待 2/2 且未启动；第二点后恰好一次成功运行。Project 参数、同 run 的 MeasurementInput/DistancePP 快照、`ImageLocalPixel` 框架、距离 `sqrt(5200)` 与 overlay 均逐项核验为 crop-local 值。

目标构建退出码 0；聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_mainwindow` CTest **1/1 通过**（46.25 秒）；格式与 `git diff --check` 通过。证据和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice193-evidence/`。

边界：只关闭实际 CropImage 输出到普通 GUI 点对流程测量的这一条 2D provenance 闭环；不外推至旋转裁剪、其它采点类型、换图/取消、多视口、SAM 标注、Agent quick tools、物理标定或设备。G6 标准 3 与阶段 6 仍为 `IN_PROGRESS`。

## Slice 194：普通 GUI 2D MeasurementInput 换图取消与迟到 release（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` 独立审核 **CONTRACT_PASS**；实现经同一审核者复审 **IMPLEMENTATION_PASS**。`ViewportWidget` 通过 `advanceContentGeneration()` 对显示数据、图像、双数据、视图切换和清空统一递增并发出变更通知；内部同步 `HImageWidget` 更新受抑制，外部图像替换/清空各通知一次。MainWindow 仅在待拾取的目标视口内容代次变化时取消运行；`onStop()` 清除各 2D 图像视口的按压、ROI 绘制和平移状态，不清掉已有测量 overlay。

新增真实 MainWindow/HImageWidget 回归覆盖 A 图鼠标按下后换成 B 图、在同屏幕位置迟到 release，以及 B 图上的正常完整重跑；另覆盖首点后换图立即取消、用户停止后的迟到 release，和 3D 点云替换立即取消。测试逐项检查 Project 点位、source-pick 数量与局部/来源坐标、运行次数、overlay 和等待状态。A→B 旧事件不污染 B 的新运行；B 正常点对 `(30,20)/(60,40)` 映射到来源 `(100,100)/(190,180)`，同 run `DistancePP=sqrt(1300)`，算法与 overlay 保持 B 的 `ImageLocalPixel`。

目标构建退出码 0；完整 `test_mainwindow` CTest **1/1 通过**（46.39 秒）；Slice194 相关聚焦 QtTest、`clang-format --dry-run --Werror`、`git diff --check` 与独立实现复审通过。构建、CTest、聚焦测试、格式门禁、review 与 SHA-256 证据见 `build-codex-agent/agent-phase6-slice194-evidence/`。普通 GUI 2D 跨视口拒点继续由 `testG6MeasurementInputRejectsCrossViewportPick` 覆盖。

边界：仅关闭普通 GUI 2D 流程测量在视口内容替换、停止和迟到 release 下的生命周期子项，并补充 3D 替换取消回归；不外推至 Agent 快速测量、多视口同步、SAM 标注、设备或 GPU。G6 标准 3 与阶段 6 仍为 `IN_PROGRESS`。

## Slice 195：SAM GUI 假服务生成、会话重开与 LabelMe 导出闭环（PASS，限定子项）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` **CONTRACT_PASS**；实现经同一独立审核者 **IMPLEMENTATION_PASS**。测试通过真实 `AnnotationOverlayWidget` 鼠标点击在 GUI 产生 SAM 正提示；本地 `SamTestServer` 记录唯一 `/set_image` 与 `/predict`，请求绑定真实 embedding 且携带局部 `(40,35)`。固定非对称来源矩阵把该点映射到 `(110,145)`，测试同时核对坐标并确保算法请求未改用来源坐标。

假服务固定返回局部四边形、bbox、score 与 2×2 白色 PNG mask；GUI preview、确认对象和 overlay mask 逐项一致，类别固定为 `slice195-defect`。生产 `saveAnnotationFile`/`AnnotationSession::load` 保存重开后，摘要、图像尺寸、来源 ID/尺寸/矩阵、object ID、标签、局部 polygon/bbox 和提示保持一致。生产 `LabelMeExporter::exportToFile` 生成的文件严格解析成功，image basename `slice195-source.png`、200×150 尺寸、类别、polygon 类型及四个局部顶点精确匹配。LabelMe schema 当前不含 DeepLux provenance；只在 DeepLux 会话格式中声明并核验该 lineage。

目标构建退出码 0；聚焦 QtTest **3/3 通过**（含初始化/清理）；完整 `test_samannotatordialog` CTest **1/1 通过**（6.43 秒）；格式、`git diff --check`、源码/测试二进制 SHA-256 和独立复审通过。测试使用隔离 HOME 与 loopback 假服务；由于默认沙箱禁止 TCP listen，两个 SAM 测试命令在审批提权后执行。原始构建、聚焦/完整测试、格式、diff、review 与哈希见 `build-codex-agent/agent-phase6-slice195-evidence/`。

边界：只覆盖固定正点提示和一个假服务轮廓的 GUI→会话往返→LabelMe 导出链路，不代表模型质量、GUI 原生文件选择器、COCO/YOLO、其它提示/编辑、设备或 GPU 验收。G6 标准 3 与阶段 6 继续 `IN_PROGRESS`。

## Slice 196：G6 坐标生产者与证据覆盖矩阵（AUDIT_PASS；G6-3 未通过）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**。JSON 是唯一数据源，生成 19 行 GUI、Agent、MeasurementInput、ROI、图像 provenance、SAM 会话/exporter、点云和生命周期矩阵；逐行列出坐标框架、局部/来源规则、变换、消费者/overlay、生命周期边界、精确测试 ID、验收 Slice 和对应证据目录。审计脚本拒绝重复 ID、缺字段、无法解析的测试符号、缺失验收标题/证据目录，并要求每个 Slice 引用都有同号证据目录；同时从 JSON 确定性生成 Markdown。

矩阵静态审计、JSON 解析、Python 编译、JSON/Markdown 一致性及 `git diff --check` 均通过。独立只读复审 **AUDIT_PASS（仅矩阵与证据引用审计）**。矩阵状态为 17 行 `covered`、2 行 `partial`；未以覆盖行数推定总标准通过。

**Slice196 时点的判定为 `NOT_READY_FOR_PASS`，计划 checkbox 当时保持未勾选。** 当时矩阵为19行、17 covered/2 partial；缺少 provenance 数值组合失败和 markerless 旧工程逐版本迁移证据。Slice197 补入 NPointCalibration 溢出 fail-closed 正/负控制，Slice198 覆盖当前支持的 3.0、2.0 与缺省 version markerless line 输入。Slice199 又限定并审计当前69项内置插件，补全内置算法坐标输出的既有 Agent 测试引用。当前状态以 Slice199 记录和20行矩阵为准，不沿用本历史快照的缺口结论。

矩阵、生成脚本、契约、独立审核结论和 SHA-256 清单见 `build-codex-agent/agent-phase6-slice196-evidence/`。本 Slice 不改产品代码和现有测试；相关产品 suites 未重跑，静态审核不代表运行时全量回归通过。

## Slice 197：校准 provenance 组合失败与 markerless 坐标往返（IMPLEMENTATION_PASS）

契约 本地逐切片契约（未纳入提交） 经独立审核 **CONTRACT_PASS**。测试 A 真实执行 `NPointCalibrationPlugin` 的 affine warp：普通 identity provenance 正控制确认 output→input 映射 `(20,40)→(40,40)` 且来源映射可用；overflow 路径输入有效 `diag(1e308,1,1)` 来源矩阵，与真实 2×X inverse warp 组合溢出后，校准图仍成功生成，输出 provenance 被清除，局部像素正确，输入像素和来源矩阵不变。

测试 B 从现有 v3 markerless `accept_measurementinput_pointset.json` fixture 创建 `point_line`（3D point + 2D line）和 `line_pair` 两个精确参数工程，分别使用生产 `Project::save`/`ProjectManager::openProject` 保存重开，再由 Agent `run_flow` 得到各自 runId，使用同一 runId 查询全部 key/port。CropImage 实际输出具有非 identity 平移矩阵 `[1,0,50,0,1,40,0,0,1]`，但两模式仍输出 `Unspecified`、`source_coordinate_available=false`，原值保持通用 `QVariantList` 且不附坐标系或来源字段；工程版本、marker 缺失、参数、模块和流程连接前后不变。

首轮完整 `test_batch4` 暴露一处旧 fixture 不匹配：`testMeasurementInputReportsLocalAndSourceCoordinates` 缺少 `pick_coordinate_frame`，按 Slice177 新契约实际返回 `Unspecified`，但旧断言意图验证 `ImageLocalPixel` source mapping。只给该测试补显式 `ImageLocalPixel` marker，保留所有原有坐标/来源断言，没有放宽期望。首轮原始日志和修正后日志均保存在 Slice197 evidence。

目标构建成功；两个新增回归聚焦 QtTest 各 **3/3 通过**（含 init/cleanup）；完整 `test_batch4` 与 `test_acceptance_flows` CTest **2/2 suites 通过**。`clang-format --dry-run --Werror` 与 `git diff --check` 通过。独立实现复审 **IMPLEMENTATION_PASS**；9 项源码/契约/矩阵/二进制 SHA-256 **9/9 校验通过**，G6 矩阵审计 19 行通过。源码、构建、首轮失败、聚焦/完整测试及哈希见 `build-codex-agent/agent-phase6-slice197-evidence/`。

边界：仅证明该 affine 数值组合失败分支和一份 v3 markerless fixture 的两个 line mode，不证明所有几何重排/不可逆插件路径或 v1/v2 所有迁移版本。G6-3 暂不勾选，待独立复审确认矩阵更新及剩余项是否为适用范围外后再评估。

## Slice 198：受支持旧版 markerless line 工程迁移（IMPLEMENTATION_PASS）

契约 本地逐切片契约（未纳入提交） 经 `/root/slice176_final_review` 独立契约审核 **CONTRACT_PASS**。新增集成回归从 markerless v3 工程语料生成两份临时旧格式工程：显式 2.0 与缺省 version；缺省 version 按生产解析规则记录迁移来源 `2.0`。每个工程都包含 `input_point_line` 与 `input_line_pair` 两个 MeasurementInput 节点，接入真实 CropImage 输出并经 `ProjectManager::openProject` 生产迁移到 3.0。

两份工程分别运行一次 Agent `run_flow`。同一 runId 查询两个节点的 key/port，原始 point/line/line1/line2 JSON 数值逐项保持，坐标框架为 `Unspecified` 且无来源坐标字段；真实 crop provenance 非 identity，平移量 `(50,40)`。原始临时 JSON 的 version 字段及两个节点缺少 `pick_coordinate_frame` 均有直接断言。显式 1.0/未知版本不属于当前支持输入，G6-3 仍为 `NOT_READY_FOR_PASS`。

`test_acceptance_flows` 构建成功；聚焦 QtTest **3/3 通过**（含 init/cleanup），完整该 CTest suite **1/1 通过**。实现独立复审 **IMPLEMENTATION_PASS**；7 项源码/契约/矩阵/测试二进制 SHA-256 **7/7 校验通过**，G6 矩阵审计 19 行通过。构建、聚焦/完整测试、格式、矩阵审计及 SHA-256 证据见 `build-codex-agent/agent-phase6-slice198-evidence/`。

边界：仅覆盖当前 loader 接受的 2.0、缺省 version 和已由 Slice197 覆盖的 3.0 输入，以及两个 line mode；不代表其它历史 payload/未来版本、第三方插件、真实 provider 或 G6-3 整体验收通过。

## Slice 199：当前内置插件坐标变换源码审计（AUDIT_PASS；G6-3 PASS）

只读审计核对 `src/plugins` 下 **69 个插件实现源码和 69 份 metadata**；138 项文件 SHA-256 全部通过。空间几何扫描只在 `NPointCalibrationPlugin` 找到 affine/perspective warp；来源坐标建立/修改点为 GrabImage、CropImage 和 NPointCalibration。JigsawPuzzle 按原行列顺序拼回；其他当前内置图像滤波/检测维持同一像素网格。现有 CropImage、NPointCalibration、JigsawPuzzle、PerProcessing 与 ImageScript 回归覆盖这些内置网格/provenance 行为；矩阵另新增算法输出行，逐项引用 Matching、FindCircle、Blob、ColorRecognition、JiErHanDefectsDet、MeasureCircle、MeasureLine、MeasureRect 和 SamPredict 的已有来源坐标集成测试。

范围限定为当前仓库 69 项内置清单；动态安装的第三方插件不在阶段0内置插件分母中，GPU绘制和设备物理基准属于阶段8。更新后的 G6 矩阵20行均为 covered；独立终审 **AUDIT_PASS**，G6-3 在当前仓库69项内置插件范围内通过。源码扫描、138 项哈希清单与校验日志见 `build-codex-agent/agent-phase6-slice199-evidence/`。

## Slice 200：坐标帧与 GUI 确认回归根因修复（IMPLEMENTATION_PASS）

全量 CTest 首轮暴露 5 项可复现失败及 1 项间歇性崩溃。修复后的 `DistancePL` 只在坐标帧为 `ImageLocalPixel` 或携带有效 provenance 时输出 `foot_point`：markerless 但有 provenance 的输入仍保留原始点值，且不会因此声称图像像素/source 坐标。`MeasurementInput::writesPointLine` fixture 显式声明原断言所需的 `ImageLocalPixel` marker。权限相关用例按真实用户权限建置前置条件：Guest 断言无 GlobalVar/Camera 权限，Agent 参数契约在 Developer 下执行并恢复 Operator；相机 binding 测试在查询/批准相机操作时登录 Developer 并在后续恢复 Operator。

间歇性 viewport SIGSEGV 的根因是 `MainWindow::copyDataSourcePathById` 遍历按值返回的临时 `Project::dataSources()`，并在循环结束后解引用元素指针。现改为遍历局部 `QList<DataSource>` 快照。`test_agent_datasource_dedicated_viewport` 的已批准 clipboard copy 流程覆盖此执行路径。

验证：全量 `cmake --build build-codex-agent -j2` 退出码 0；六个相关 CTest **6/6 通过**；viewport 集成 CTest 在修复后重复 **100/100 通过**；全量 CTest **108/108 通过**，含本机 listener 测试。G6 坐标矩阵审计 `20 rows` 通过；格式门禁与 `git diff --check` 通过。独立实现复审 **IMPLEMENTATION_PASS**：确认快照指针生命周期正确、未发现其他临时 `dataSources()/modules()/connections()` 元素地址逃逸，并确认 DistancePL markerless provenance 输出符合 Slice197 坐标契约。构建、全量/定向测试、重复测试、插件同步、审计及哈希证据见 `build-codex-agent/agent-phase6-slice200-evidence/`。

边界：本轮是 Linux/Qt 5.15.3 的软件验证；真实相机、PLC、SAM 模型/provider、GPU、Windows 和跨平台验证仍未执行，按计划归阶段 8。

## Slice 201：3D queued-continuation 同 ID 替换隔离（IMPLEMENTATION_PASS）

契约见 本地逐切片契约（未纳入提交）。独立复审最初指出新请求只断言开始/结束，不能排除失败结束；修订契约与测试后，`/root/slice176_final_review` 复审 **IMPLEMENTATION_PASS**。

`testG6MeasurementInput3DQueuedContinuationRejectsSameIdModuleReplacement` 使用真实 MainWindow `point3DWorldClicked` 信号和点云视口完成旧请求。测试保持 Project、viewport、模块 ID 与序列化 JSON 不变，只替换模块实例并断言 generation 改变；处理已排队 continuation 后，run/module 计数不变、引擎空闲、替换模块 JSON 不变。随后新请求注入两点，必须恰好启动/结束一次，`lastRunResult().success` 为 true，`runId` 非空且不同于先前 run，并与 `lastRunId()` 相同；替换实例中的两点值逐项核验。

当前源码重建 `test_mainwindow` 成功；四个相关 3D QtTest 用例 **6/6 通过**（含 init/cleanup），完整 `test_mainwindow` CTest **1/1 通过**，`clang-format --dry-run --Werror` 与 `git diff --check` 通过。该刷新未重跑项目全量 CTest；108/108 仍以 Slice200 记录为最近全量证据。源码、二进制、构建日志、聚焦/CTest 日志和摘要见 `build-codex-agent/agent-phase6-slice201-refresh-evidence/`。

边界：world-pick 由视口信号注入，不证明物理鼠标命中、相机/GPU 输入或其它每种 project/viewport 失效组合；这些仍按各自切片与阶段 8 门槛验收。
