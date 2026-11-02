# 执行顺序与页面布局统一设计

- 日期：2026-11-02
- 状态：设计已确认，待编写实施计划
- 目标代码：`src/core/engine/RunEngine.*`、`src/core/model/Project.*`、`src/ui/widgets/FlowCanvas.*`、`src/ui/controllers/ProcessTreeController.*`、`src/ui/views/MainWindow.*`
- 关联文档：`docs/architecture.md`、`docs/superpowers/specs/2026-10-30-background-execution-design.md`

## 1. 背景与问题

当前“页面顺序”和“实际执行顺序”来自三套互不一致的数据：

- 画布节点位置来自 `ModuleInstance.posX/posY`（用户拖动或新增时的插入序号），只影响显示。
- 流程树顺序来自插入/拖动顺序；`MainWindow::syncModulesToRunEngine()` 按流程树 top-level 顺序把模块加入引擎。
- 引擎 `RunEngine::buildExecutionOrder()` 按数据边做拓扑排序，If/Loop/While 的实际路径又由控制边和运行结果动态决定。

因此：

- 手动拖动画布或流程树后，视觉顺序可能变化，但执行顺序不变。
- Agent 通过 `add_module/connect_modules` 构建流程时，节点位置按插入序号生成，不反映数据流。
- 数据链、控制分支、循环回边在页面上没有统一的层级/区域表达。
- 节点拖动位置当前甚至没有写回 Project，保存/重载会丢失手工布局。

## 2. 目标与非目标

### 2.1 目标

1. 建立**单一执行计划事实源**：`RunEngine`、流程树、画布使用同一份 `FlowOrderPlan`。
2. 主数据链按依赖层级自上而下排列，每层一个节点；同层仅用于控制流分支/独立入口。
3. 流程树按控制流深度优先展开，保持扁平列表。
4. 用户自由拖动保留；结构变化后自动按执行层级归位。
5. 流程树跨层拖动在简单线性链上自动重连；其他情况明确拒绝并提示。
6. Agent 构建的流程与手动搭建的流程得到相同的规范化布局。
7. 位置可持久化，撤销/重做能恢复顺序、连接和位置。
8. 不改变引擎的控制流、参数、断点、并行和后台执行语义；不改变插件 ABI。

### 2.2 非目标

- 不改变 If/Loop/While 的运行期动态路径语义。
- 不把 If/Loop/While 流程树改成层级树或泳道容器；本轮保持扁平列表。
- 不改变数据端口的连接语义或自动猜测多个端口的映射。
- 不引入新的执行顺序文件格式；仍使用现有 Project JSON。
- 不把布局变更放入撤销栈；只有用户拖动引发的顺序/连接重连进入撤销栈。

## 3. 当前事实基线

- `FlowCanvas::loadFromProject()` 使用 `inst.posX/posY`（`FlowCanvas.cpp`）。
- 新节点默认位置为 `(-110, -220 + nodeCount * 96)`（`MainWindow.cpp`），即插入顺序。
- `FlowNodeItem::itemChange()` 只更新连线，不写回 Project 位置。
- `ProcessTreeController::addModule()` 使用 `addTopLevelItem()` 追加；拖动由 `MainWindow` 事件过滤器处理并调用 `Project::moveModule()`。
- `RunEngine::buildExecutionOrder()`：
  - 对数据边做拓扑排序；
  - 控制边不参与拓扑，单独收集；
  - 回边识别、loop region、reentrant 集合；
  - ready 队列初始顺序来自引擎 `m_modules` 顺序。
- `RunEngine::getNextModule()` 按 `flowControlType` 和运行结果决定动态下一步。
- 控制端口约定：If 输出 `true/false`，Loop/While 输出 `body/done`；控制边 `edgeType="control"`。

## 4. 总体架构

```text
Project（模块、连接、控制边）
        │
        ▼
FlowGraphAnalyzer（纯逻辑，单一事实源）
        │ FlowOrderPlan
        ├── RunEngine（dataOrder / 控制边 / loop region）
        ├── FlowCanvas（rank/lane → 节点位置）
        └── ProcessTreeController（displayOrder）
                ▲
                │ 结构变化信号（合并）
        FlowLayoutCoordinator
```

新增组件：

1. `FlowGraphAnalyzer`：核心纯逻辑，不依赖 QWidget。
2. `FlowLayoutCoordinator`：UI 层，监听 Project 信号，驱动重排。
3. `ReorderLinearChainCommand`：撤销/重做线性链拖动重连。

## 5. FlowOrderPlan 与层级算法

### 5.1 数据结构

```cpp
struct FlowOrderNode {
    QString instanceId;
    int rank = 0;              // 纵向层级
    int lane = 0;              // 同层横向槽位
    int displayIndex = 0;      // 扁平展示/流程树顺序
    QString branchLabel;       // true/false/body/done/空
    QString controlParent;     // 所属 If/Loop/While 实例 id
};

struct FlowOrderPlan {
    bool valid = false;
    QString error;
    QStringList dataOrder;      // 数据 DAG 拓扑序（引擎静态顺序）
    QStringList displayOrder;   // 控制流深度优先的扁平展示顺序
    QList<FlowOrderNode> nodes;
    QSet<QString> loopBackEdges;
    QSet<QString> reentrantModules;
    QMap<QString, QSet<QString>> loopRegions;
};
```

身份统一使用 `ModuleInstance.id`。若引擎内部使用 runtime name，分析器提供 `instanceId <-> runtimeName` 映射；禁止 UI 和引擎各自使用不同身份排序。

### 5.2 前向依赖图层级（data + 控制）

层级不能只看数据边，否则普通控制链会全部落在 rank 0。分析器先构造**前向依赖图**：

```text
forwardEdges = 所有 data 边 + 所有 control 前向边 - 已识别的循环回边
```

1. 收集所有模块，初始化 `rank=0`、`indegree=0`。
2. 对 `forwardEdges` 做拓扑排序。
3. `rank(v) = max(rank(pred)+1)`，因此：
   - 普通数据链 A→B→C：0,1,2；
   - 普通控制链 A.next→B.control→C.control：0,1,2；
   - If 的 true/false 分支入口都位于 opener.rank+1；
   - Loop/While 的 body 入口位于 opener.rank+1，LoopEnd/WhileEnd 不低于 body 最大 rank+1；
   - 回边不参与 rank，避免无限层级。
4. 无任何连接的节点：按 Project 稳定顺序形成 rank=0..N-1 的初始化链（仅用于展示；见 7.3 的无连接顺序列表）。
5. 环、非结构化控制流、端口映射错误 → `valid=false`，不生成布局、不运行新顺序。

### 5.3 统一 lane 分配

同层节点必须统一分配槽位，不能只处理 If/Loop：

1. 按 `rank` 分组。
2. 每组先确定“主链节点”（无控制分支标签且属于主要前向路径），lane=0。
3. 其余节点按以下键排序后分配 lane：
   - 控制区域路径（opener 的 displayIndex，嵌套时按路径字典序）；
   - 分支端口顺序：If 的 true 在 false 前；Loop/While 的 body 在 done 前；Parallel 按控制边声明顺序；
   - Project 稳定顺序。
4. lane 采用居中式分配：`0, -1, +1, -2, +2...`，或按组内数量整体平移到以 0 为中心。
5. 每个同层节点获得唯一 lane；即使数据 fan-out 产生多个同层节点，也不会重叠。
6. `branchLabel` 取控制端口 id（true/false/body/done）；`controlParent` 指向 opener；纯数据 fan-out 的 `branchLabel` 为空。

控制区域仍用于分组/标签：

- `Conditional/ConditionalElse/ConditionalEnd`
- `Loop/LoopEnd`
- `While/WhileEnd`
- `Parallel`

### 5.4 displayOrder：依赖约束下的稳定拓扑遍历

**不能使用纯 DFS**：菱形 A→B、A→C、B→D、C→D 若 DFS 得到 A,B,D,C，会违反“C 在 D 前”的数据依赖。

算法：

1. 在 `forwardEdges` 上计算 DFS 优先序 `preferredIndex`：
   - 控制边优先于数据边，按 If true/false、Loop body/done、Parallel 声明顺序；
   - 回边跳过；循环体只遍历一次。
2. 在 `forwardEdges` 上做 Kahn 拓扑排序；ready 队列按 `preferredIndex` 升序取节点。
3. 由此得到 `displayOrder`：
   - 所有前向依赖（数据+控制）都满足 `u` 在 `v` 前；
   - 在依赖允许的范围内尽量保持控制分支分组；
   - 菱形图稳定得到 A,B,C,D（B/C 按 preferredIndex），D 一定在 B、C 之后。
4. 同时生成 `dataOrder`：仅用数据边做 Kahn，使用同一 `preferredIndex` 作为 tie-break。
5. 一致性要求（测试断言）：
   - 对每个数据边 `u→v`，`indexOf(dataOrder,u) < indexOf(dataOrder,v)` 且 `indexOf(displayOrder,u) < indexOf(displayOrder,v)`；
   - 纯数据图（无控制边）`dataOrder == displayOrder`；
   - 控制图中 `displayOrder` 是前向依赖图的合法拓扑序，流程树不会出现“下游先于上游”。
6. `displayIndex` 按 `displayOrder` 赋值。

`RunEngine` 仍使用 `dataOrder` 作为静态执行顺序；控制分支的动态路径由控制边和运行结果决定。页面/树使用 `displayOrder`，两者对数据依赖的先后一致；控制分支只影响分组，不伪造数据顺序。

### 5.5 与 RunEngine 的关系

- `RunEngine::buildExecutionOrder()` 改为调用 `FlowGraphAnalyzer`：
  - 使用 `dataOrder` 作为 `m_executionOrder`；
  - 使用分析器给出的控制边、回边、loop region、reentrant 集合；
  - 端口兼容、断点、并行批次、运行期控制逻辑保持原有实现。
- 现有 `buildExecutionOrder` 的旧结果用于差分测试；新的分析器必须逐项匹配。
- 运行期动态路径不变：If 只执行一支、Loop 重复 body；静态计划展示完整结构。

## 6. 画布布局

### 6.1 布局规则

- 方向：自上而下。`rank` → Y，`lane` → X。
- 行高：该 rank 内节点最大 `boundingRect().height()` + 固定间距。
- 列宽：同层节点按 lane 居中排列；主链 lane=0 居中。
- 分支标签绘制在节点上方或端口旁；回边使用现有连线并加循环标记。
- 布局后调用 `updateConnectionsForNode()`；不改变选中节点和视口缩放。

### 6.2 API

```cpp
enum class LayoutMode { Full, Normalize };

void FlowCanvas::applyExecutionLayout(const FlowOrderPlan& plan, LayoutMode mode);
```

- `Full`：按 rank/lane 完整重排，写回位置。
- `Normalize`：只校正 Y 到 rank 行；若 X 不重叠则保留手工 X，否则按 lane 排列。
- 只移动节点，不重建 item，避免丢失选中/高亮状态。

### 6.3 位置持久化

新增 Project API：

```cpp
struct FlowGraphCandidate {
    QStringList orderedIds;
    QList<ModuleConnection> connections;
};

void Project::setModulePosition(const QString& instanceId, int x, int y);
void Project::reorderModules(const QStringList& orderedIds);
bool Project::applyStructureTransaction(const FlowGraphCandidate& candidate, QString* error);
quint64 Project::structureRevision() const; // 结构变更单调递增
bool Project::layoutInitialized() const;    // 持久化字段
void Project::setLayoutInitialized(bool);
signals:
    void modulePositionChanged(const QString& instanceId, int x, int y);
    void modulesReordered();
    void structureRevisionChanged(quint64 revision);
```

- `setModulePosition`：更新 `posX/posY`、`touch()`，发专用信号，不发 `moduleUpdated`（避免参数/引擎同步噪声）。
- `FlowNodeItem` 拖动结束写回 Project；自动布局也走该 API。
- `reorderModules`：一次性更新模块顺序并发 `modulesReordered()`；当前 `moveModule` 不发信号，不能支撑批量重排/撤销刷新。
- 自动布局的位置变更不进入撤销栈，但随工程保存并标记 modified。

**加载规则（保证自由布局可恢复）**

新增 Project 持久化字段 `layoutInitialized`（默认 false，`fromJson` 缺省 false）：

- `layoutInitialized == true` 且存在有效位置：**原样恢复保存的位置，不 Normalize、不 Full**；只有结构变更或用户显式“按执行顺序整理”才重排。
- `layoutInitialized == false`（新工程、Agent 初始工程、旧工程未初始化布局）：执行一次 `Full`，然后置 true。
- 位置缺失/非法（例如全部为 (0,0) 且节点数 > 1）：视为未初始化，执行 `Full`。
- 用户自由拖动后写回位置，保持 `layoutInitialized == true`。
- 结构变更自动布局后也置 true。

## 7. 流程树排序与拖动

### 7.1 排序

```cpp
void ProcessTreeController::applyExecutionOrder(const QStringList& displayOrder);
```

- 用 `takeTopLevelItem`/`insertTopLevelItem` 重排现有 item。
- 保留选中项、展开状态、滚动位置。
- 不在计划中的节点追加在末尾（防御性，不丢节点）。
- `Qt::UserRole+6` 写入 `displayIndex`，供 tooltip/测试使用。

### 7.2 拖动规则

拖动前取得源/目标节点的 rank 与 branchLabel：

1. **同层且相互独立**：允许交换稳定序；更新 Project 模块顺序，重算计划并刷新。
2. **同层但属于控制分支**（branchLabel 非空）：拒绝并复位；提示“分支顺序由控制边决定，请调整连线”。
3. **跨层且属于无连接顺序列表（7.3 A）**：只调整模块顺序。
4. **跨层且属于已连接简单线性链（7.3 B）**：执行候选图校验与自动重连。
5. **其他跨层拖动**：拒绝并复位；提示“执行顺序由连接决定，请调整连线”。

### 7.3 两种可拖动情形

**A. 无连接顺序列表**

- 条件：当前 Project 没有任何连接（`connections().isEmpty()`）。
- 所有节点按 Project 模块顺序作为隐式单链；允许任意跨层重排。
- 只修改模块顺序，不创建连接；引擎回退顺序与页面按同一 Project 顺序。
- 不需要“单一连通分量”条件。

**B. 已连接简单线性链**

- 所有节点属于一个连通分量，数量 ≥ 2；
- 数据/控制边的入度、出度均 ≤ 1；
- 不含 `Conditional/Loop/While/StopLoop/Parallel` 控制流节点；
- 所有边均为数据边，且所有相邻边使用同一组端口模式 `(fromPort, toPort)`；
- 无分支、汇合、环、未连接的多余节点。

两种情形互斥；判定顺序先 A 后 B。

### 7.4 候选图校验与事务提交（失败零修改）

禁止“先删除/重建真实连接，再校验”。流程：

1. 在内存构造候选图 `FlowGraphCandidate`：
   - `QStringList orderedIds`；
   - `QList<ModuleConnection> candidateConnections`。
2. 用 `FlowGraphAnalyzer::validateCandidate(project, candidate, &plan, &error)` 校验：
   - 模块存在、顺序是完整排列；
   - 端口存在、类型兼容、无重复边；
   - 前向图无环、控制流结构化；
   - 线性链端口模式无歧义。
3. 校验通过后调用一次 `Project::applyStructureTransaction(candidate)`：
   - 在同一事务内更新模块顺序与连接增删；
   - 只触发一次结构变更批次（现有 `moduleAdded/Removed/connectionAdded/Removed` 可由事务统一发出，或增加 `structureChanged` 汇总信号）；
   - 结构版本 +1；`modified` 只在该提交点改变。
4. 校验失败：不调用任何 Project 修改 API，不改变 `modified`、不发结构信号、不压撤销命令；树/画布复位并提示 `error`。
5. 自动布局在事务成功后由协调器执行；事务本身不直接移动 UI。

### 7.5 撤销/重做与跨历史失效

新增 `ReorderLinearChainCommand : QUndoCommand`，推入 MainWindow 现有 `m_paramUndoStack`：

- 保存旧/新模块顺序、旧/新连接、旧/新位置，以及 `projectId` 和 `structureRevision`。
- 命令应用期间设置布局抑制标志；成功提交后刷新树/连线，不重新计算布局。
- `Project` 增加单调递增 `structureRevision`，在所有结构变更（模块/连接/顺序）时 +1；参数修改不增加该版本。
- 命令保存 `m_expectedRevision` = 上次应用后 Project 的版本。
- `undo()/redo()` 前检查：
  - 当前 Project 身份（id/session）与命令保存的一致；
  - 当前 `structureRevision == m_expectedRevision`。
  - 任一不满足 → 命令失效：不执行、不修改工程，提示“结构已被其他操作修改，无法撤销此重排”，并从可用历史中跳过。
- 成功 undo/redo 后更新 `m_expectedRevision` 为新的结构版本。
- 因此 GUI 重排后 Agent 新增节点/连接，再执行 GUI Undo 会被拒绝，不会覆盖 Agent 的后续编辑；反之亦然。
- 自动布局本身不进入撤销栈。

## 8. FlowLayoutCoordinator

位置：`src/ui/controllers/FlowLayoutCoordinator.h/.cpp`。

职责：

- 监听 Project 的信号：`moduleAdded/moduleRemoved/connectionAdded/connectionRemovedWithPorts/modulesReordered`。
- 用 0–50ms single-shot 合并连续信号，避免 Agent 批量编辑时逐次重排。
- 调用 `FlowGraphAnalyzer`，成功则：
  - `FlowCanvas::applyExecutionLayout(plan, Full/Normalize)`；
  - `ProcessTreeController::applyExecutionOrder(plan.displayOrder)`；
  - 位置通过 `Project::setModulePosition` 持久化。
- 失败时保留最后有效布局，记录具体错误，不阻塞运行。
- 提供 `suppressAutoLayout(bool)` 供 `ReorderLinearChainCommand` 使用。

触发时机：

| 事件 | 行为 |
|---|---|
| 工程加载 | `layoutInitialized==true` 且有有效位置 → 原样恢复；否则 Full 并置 true |
| 模块新增/删除 | Full |
| 连接新增/删除 | Full |
| 模块顺序变化 | Full |
| 用户自由拖动 | 只写位置，不立即重排 |
| 显式“按执行顺序整理” | Full |
| Agent 批量构建 | 信号合并后 Full |
| ReorderLinearChainCommand | 抑制自动布局，恢复命令保存的位置 |
| 分析失败 | 不重排，保留旧布局并记录错误 |

## 9. Agent 集成

- Agent 继续通过 `add_module/connect_modules/disconnect_modules` 操作 Project；不计算坐标。
- 布局协调器通过 Project 信号自动整理，Agent 构建结束后页面与引擎顺序一致。
- `get_flow_state` 增加只读字段：
  - `executionRank`：数据/控制层级；
  - `displayIndex`：扁平展示顺序。
  保持现有字段兼容，只增不改。
- Agent 的撤销栈继续管理结构编辑；布局由信号重建，不进入 Agent 撤销栈。

## 10. 兼容性与行为变化

- 引擎同层 tie-break 从“流程树/插入顺序”改为 Project 规范顺序；只在多入口/同层独立节点时可能改变顺序。
- 跨层流程树拖动：线性链自动重连，其他图拒绝；不再允许“拖了但执行不变”。
- 用户自由拖动只改位置；下一次结构变化按执行层级归位。
- 工程加载默认恢复已保存的自由布局；只有 `layoutInitialized==false` 或位置非法的旧工程才 Full 初始化。结构变更/显式整理仍会重排。
- 自动布局写回位置并标记工程 modified，但不进入撤销栈。
- GUI 与 Agent 仍可使用各自撤销栈，但结构命令通过 `Project::structureRevision` 做跨栈失效：任一结构编辑交错后，旧重排命令拒绝 undo/redo，不覆盖后续编辑。
- 不改 `IModule`/`ModuleBase` ABI，不要求重建插件。
- If/Loop/While 的动态路径不变；静态计划展示完整分支，运行高亮显示实际路径。

## 11. 测试矩阵

| 层 | 用例 |
|---|---|
| 分析器 | 线性数据链 rank=0,1,2；**纯控制链 A.next→B.control→C.control rank=0,1,2**；无连接回退；多入口；**菱形 A→B/A→C/B→D/C→D 的 displayOrder 必须 A,B,C,D，D 在 B/C 后**；数据 fan-out 同层 lane 唯一；If true/false 同层分栏；**嵌套 If/Loop 分支**；ConditionalEnd；Loop/While body 与回边；Parallel；禁用节点；环/非结构化控制流报错 |
| 顺序一致性 | 对每个数据边 u→v，`dataOrder` 与 `displayOrder` 都满足 u 在 v 前；纯数据图两者完全相等；displayOrder 是前向依赖图合法拓扑序 |
| 差分回归 | 现有控制流/循环/并行测试工程：`plan.dataOrder` 与旧 `buildExecutionOrder` 逐项一致；控制边/loop region/reentrant 一致 |
| 画布 | rank 递增则 Y 递增；同 rank 同 Y；**所有同层节点 lane 唯一不重叠**；分支标签；回边；Normalize 保留 X；Full 重排；100/200 节点布局耗时有上限 |
| 位置 | 拖动写回；`layoutInitialized=true` 保存/重载原样恢复；未初始化/非法位置才 Full；modified 状态 |
| 流程树 | 顺序等于 displayOrder；选中/滚动保持；同层独立换序；分支换序拒绝；跨层非线性拒绝 |
| 线性链重连 | **无连接顺序列表**与**已连接简单线性链**分别覆盖；端口唯一模式成功；多端口歧义拒绝；失败时工程数据/modified/结构信号/撤销栈均未改变 |
| 撤销/重做 | 顺序、连接、位置完整恢复；多次 undo/redo 后计划与引擎一致；**GUI 重排后 Agent 新增节点/连接，再 GUI Undo 必须失效且不覆盖 Agent 编辑；反向交错同样** |
| Agent | 脚本化构建线性链/分支；计划、树、画布、引擎一致；`get_flow_state` 含 rank/displayIndex |
| UI | 线性、If、Loop、拖动后结构变更、显式整理截图门禁 |
| 回归 | `test_runengine`、`test_mainwindow`、`test_flowexecutionservice`、Agent 全套、全量 CTest |

竞态/UI 顺序测试使用确定性屏障（`QSemaphore`、`QSignalSpy + QTRY`、事件队列），不使用 sleep 构造顺序。

## 12. 迁移批次

| 批次 | 修改范围 | 独立提交 |
|---|---|---|
| B0 | 基线、分支、现有测试 | 无代码提交 |
| B1 | `FlowGraphAnalyzer` + RunEngine 接入 + 差分测试 | `feat(flow): add canonical execution plan analyzer` |
| B2 | Project 位置/排序信号 + `layoutInitialized` + `structureRevision` + `applyStructureTransaction` + FlowCanvas 布局 + FlowLayoutCoordinator | `feat(ui): lay out flows by execution plan` |
| B3 | 流程树排序 + 拖动规则 + 候选图校验 + ReorderLinearChainCommand（含跨栈失效） | `feat(ui): reorder linear chains from the process tree` |
| B4 | Agent 集成 + get_flow_state 字段 + 合并触发 | `feat(agent): expose and maintain execution order` |
| B5 | 控制分支视觉 + 截图门禁 + 文档 + 全量回归 | `docs: document execution-order layout` |

每批验收命令：

```bash
# B1
cmake --build build -j4
ctest --test-dir build -R "test_flowgraphanalyzer|test_runengine" --output-on-failure

# B2
cmake --build build -j4
ctest --test-dir build -R "test_project|test_flowcanvas|test_mainwindow" --output-on-failure

# B3
cmake --build build -j4
ctest --test-dir build -R "test_processtree|test_mainwindow" --output-on-failure

# B4
cmake --build build -j4
ctest --test-dir build -R "test_agentactor|test_agentcontroller|test_agentclosedloop" --output-on-failure

# B5 全量
cmake --build build -j4
TEST_HOME=$(mktemp -d /tmp/deeplux-layout.XXXXXX)
env HOME="$TEST_HOME" bash scripts/sync-plugins.sh build
env -u DEEPLUX_APP_DATA_DIR HOME="$TEST_HOME" QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure -j4
git diff --check
```

## 13. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 分析器与引擎语义漂移 | 抽取同一实现；旧算法结果差分回归；控制流测试全绿 |
| 控制流边角情况 | 未知/非结构化形式不重排并报错；不猜测顺序 |
| 自动重连改变数据路由 | 仅简单线性链；端口模式唯一；失败零修改；可撤销 |
| 布局抖动 | 信号合并；批量移动；布局期间暂停重绘 |
| 拖动与自动布局冲突 | 拖动只写位置；结构变化后归位；显式整理按钮 |
| 撤销栈混乱 | 命令保存完整旧/新状态；布局本身不入栈 |
| 大图性能 | O(V+E) 分析；100/200 节点基准；避免逐节点信号风暴 |
| 旧工程外观变化 | `layoutInitialized` 已保存的布局原样恢复；仅未初始化/非法位置执行 Full；文档说明 |

## 14. 验收标准

1. 对线性、If、Loop、Agent 构建、手动拖动后修改的代表流程，画布层级、流程树顺序、`FlowOrderPlan.dataOrder` 与引擎静态执行顺序一致。
2. 主链每层一个节点；同层只出现在控制分支/独立入口，并用 lane 区分。
3. 用户自由拖动可保存；结构变更后自动归位；无节点重叠。
4. 线性链跨层拖动可重连或明确拒绝；撤销/重做恢复顺序、连接和位置。
5. Agent 构建完成后无需手动整理，页面顺序即引擎顺序；`get_flow_state` 可读取 rank/displayIndex。
6. 控制流动态路径不受影响；现有控制流、循环、并行、断点、后台执行测试全绿。
7. 全量 CTest、截图门禁通过；Windows/真实工程未验证项单独记录。

## 15. 评审修订记录（2026-11-02）

针对提交 `e1663d4` 的 P1/P2 复核：

1. **displayOrder 依赖约束**：取消纯 DFS；改为“DFS preferredIndex + Kahn 拓扑”的稳定拓扑遍历，保证菱形图 A,B,C,D 且 D 在所有前驱之后；明确 dataOrder/displayOrder 对数据边的一致性与测试。
2. **层级覆盖控制链**：前向依赖图包含数据边与去回边后的控制边，普通控制链获得 rank 0,1,2；所有同层节点统一分配唯一 lane，覆盖数据 fan-out 与嵌套分支。
3. **撤销跨历史失效**：保留两套撤销栈，但引入 `Project::structureRevision` 与 `projectId`；重排命令在 revision 不匹配时失效，绝不覆盖 Agent/GUI 的后续结构编辑；补充交错测试。
4. **失败零修改**：先构造 `FlowGraphCandidate` 并完成端口/图/计划校验，再通过 `Project::applyStructureTransaction` 一次提交；失败时不改 modified、不发结构信号、不压撤销栈。
5. **加载位置恢复**：新增持久化 `layoutInitialized`；已保存布局默认原样恢复，只有未初始化/非法位置才 Full 布局，避免保存重载覆盖自由拖动。
6. **无连接拖动分支**：拆分为“无连接顺序列表”和“已连接简单线性链”，解决连通分量条件自相矛盾。
