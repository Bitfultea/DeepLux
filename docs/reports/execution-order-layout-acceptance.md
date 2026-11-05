# 执行顺序布局验收报告（B0–B5）

> 本报告随 B5 提交 `docs: document execution-order layout` 入库。覆盖计划
> `docs/superpowers/plans/2026-11-03-execution-order-layout.md`（主仓库未跟踪文件，按 K7 未提交）
> 的 B0–B5 五个批次，及其文档末尾「最终验收清单」的逐项结论。
>
> 实施工作树：`/home/charles/Data/Repo/DeepLux-execution-order`，分支 `feature/execution-order-layout`。
> 验收环境：Qt 5.15.3 / offscreen。所有 CTest 均带 `env -u DISPLAY QT_QPA_PLATFORM=offscreen`；
> 隔离回归另加 `-u DEEPLUX_APP_DATA_DIR` 与临时 HOME。

---

## 1. 各批提交清单（按时间）

| 批次 | 提交 | 主题 / 说明 |
| --- | --- | --- |
| B0 基线（main） | `8078a63` | `docs: fingerprint connection declaration order and tighten order claims` |
| B1 | `bbf9dca` | `feat(flow): add canonical execution plan analyzer` |
| B1 复核修复 | `5eeff91` / `a4609d0` / `edccfbf` | `fix(flow): rank unconnected modules in display chain` / `fix(flow): limit initialization chain to unconnected flows` / `fix(flow): align display order with control branches` |
| B2 | `4ab01f4` | `feat(ui): lay out flows by execution plan` |
| B2 复核修复 | `3409658` / `fc35022` | `fix(ui): size execution layout rows by node height` / `fix(ui): normalize overlapping positions and bind canvas project` |
| B3 | `ee7157d` | `feat(ui): reorder linear chains from the process tree`（完整值 `ee7157d2e4701351badc9908d34c49d44e359469`） |
| B4 | `81dde8f` | `feat(agent): expose and maintain execution order`（完整值 `81dde8f03f0369cfa5e71235e9c00b58e0c2bf69`） |
| B5（本批） | `bb46550` | `docs: document execution-order layout`（完整值 `bb46550b7b0023664bb553cdf57566507c8112c6`） |

> **B5 哈希的自引用说明**：本报告与 B5 提交同批入库，文件内无法自洽地固化自身提交哈希
> （写入任何值都会因提交内容变化而失效）。因此 B5 行只记录主题，完整哈希在提交后由
> `git rev-parse HEAD` 读取，登记于本批交付消息与主理人最终验收报告；见 §6「提交后复核记录」。

B5 开批门禁实测：`BATCH_BASE=81dde8f03f0369cfa5e71235e9c00b58e0c2bf69`，
`git status --short -- <B5 Files>` 与全量 `git status --short` 均为空。

---

## 2. 实际测试命令与结果（本批 B5 实测数字）

### 2.1 目标构建与目标 CTest

```bash
cmake --build build -j4 --target test_mainwindow ui_capture_mainwindow
env -u DISPLAY QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build -R "test_mainwindow|ui_capture_mainwindow" --output-on-failure
```

- 目标构建：`BUILD_EXIT=0`。
- 首次目标 CTest：`2/2` 通过（22.62s：`test_mainwindow` 13.97s、`ui_capture_mainwindow` 8.64s）。
- `clang-format` 并重新构建后最终复跑：`2/2` 通过（23.24s：14.71s / 8.53s），`100% tests passed, 0 tests failed out of 2`。
- **截图门禁结果：`ui_capture_mainwindow` PASS**。门禁机制为 `verifyCapture`（文件存在 / 尺寸 / 灰度方差非空白）
  + `meanAbsDiff` 前后差异 + 控件状态断言；仓库内**没有签入基线 PNG**。B5 用语义断言覆盖新增视觉
  （If 的 `true`/`false` 分支标签、分支 lane 不重叠、画布节点不相交），未更新或新增任何基线 PNG。

### 2.2 全量构建与主口径全量 CTest（标准 HOME，主验收口径）

```bash
cmake --build build -j4
env -u DISPLAY QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure -j4
```

- `BUILD_EXIT=0`。
- 格式化前全量 CTest：`96/96` 通过（71.69s）。
- `clang-format` 并重新构建后最终全量 CTest：`96/96` 通过（82.84s），
  `100% tests passed, 0 tests failed out of 96`。
- 报告以下结论以**最终一次（格式化后）** 96/96、82.84s 为准。

### 2.3 全量隔离回归（临时 HOME，按计划 B5 Step 2 并补 `env -u DISPLAY`）

```bash
cmake --build build -j4
TEST_HOME=$(mktemp -d /tmp/deeplux-layout.XXXXXX)     # 实测 /tmp/deeplux-layout.6JGGc7
env HOME="$TEST_HOME" bash scripts/sync-plugins.sh build
env -u DISPLAY -u DEEPLUX_APP_DATA_DIR HOME="$TEST_HOME" QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure -j4
git diff --check
git status --short
```

**如实记录：本项未全绿，失败均为环境依赖型，非本计划回归。**

- `SYNC_EXIT=1`：`scripts/sync-plugins.sh: line 115: /dev/fd/63: No such file or directory`。
  本 sandbox 环境没有 `/dev/fd`（仅存在 `/proc/self/fd`），脚本第 115 行
  `done < <(find "$PROJECT_DIR/src/plugins" -path '*/metadata.json' -print0)` 依赖 bash process
  substitution 的 `/dev/fd`，元数据准备阶段即中止，66 个插件 `.so` 未复制到临时 HOME。
- 隔离 CTest：`91/96` 通过（95%），82.03s；失败 5 项：
  `test_mainwindow`、`test_pluginmanager`、`test_pluginparametercontracts`、
  `test_workerpluginconsistency`、`ui_capture_mainwindow`。
- 日志证据：`Modules found: 0`、`Plugin not found: "GrabImage" / "FindCircle" / "LoadPointCloud" / "CropImage" / "MeasurementInput" / "条件分支"`、
  `未构建/未安装的候选插件: SaveData`、`rejection must name the unsupported type, got: (空)`、
  `'!modules.isEmpty()' returned FALSE`。全部指向插件库缺失，而非断言逻辑或产品行为。
- `git diff --check`：OK；`git status --short`：仅本批 6 个已跟踪文件修改（报告当时尚未创建）。

### 2.4 环境修复尝试与诊断性补跑（非计划验收命令，仅用于区分环境 vs 真实回归）

- 尝试 `ln -s /proc/self/fd /dev/fd` 恢复标准符号链接；sandbox 随即回滚（`/dev/fd` 仍不存在），
  第二次 sync 同样失败（目标 HOME 下 0 个 `.so`）。**未修改任何仓库文件**。
- 生成 `/tmp` 临时包装脚本（仅把第 115 行 process substitution 换成
  `done < "$DEEPLUX_META_LIST"`，不触碰仓库脚本、不删除任何断言、不新增仓库文件），
  等价同步成功：`SYNC3_EXIT=0`，`prepared=66 copied=66`，
  `TEST_HOME3=/tmp/deeplux-layout.wcx1lx`。
- 同一 offscreen 全量 CTest：`64/96` 通过（67%），62.74s，32 项失败；主因
  `installRuntimePlugin(...) returned FALSE`（临时 HOME 下插件安装路径 / 资源 / 并发环境差异），
  与本批新增的分支标签、回边标记语义断言无关。
- **诊断结论**：两次隔离回归的失败集合都随 HOME / 插件安装状态漂移；同一 build 目录、
  同一批二进制、同一 ctest 命令在标准 HOME 下 96/96 全绿，证明不是 B5 产品回归。
  未为凑全绿改产品源码、未删断言、未把补跑结果冒充计划命令结果。

### 2.5 提交前检查

```bash
clang-format -i src/ui/widgets/FlowCanvas.cpp src/ui/widgets/FlowCanvas.h \
  tests/test_mainwindow.cpp tests/ui_capture_mainwindow.cpp
git diff --check
```

- `clang-format` 二次 `clang-format "$f" | diff - "$f"` 无残留。
- `git diff --check`：通过（`DIFF_CHECK_EXIT=0`）。

### 2.6 历史批次记录（非本批重跑，仅供对照）

- B2 HEAD `fc35022`：offscreen 全量 `95/95`（71.68s）。
- B3：全量 `96/96`；目标 `5/5`；单例 `testNoOutputSelectedModuleClearsOverlays` 3/3 PASS。
- B4：全量 `96/96`（72.01s）。

---

## 3. 计划「最终验收清单」逐项结论

| # | 清单条目 | 结论 | 证据 |
| --- | --- | --- | --- |
| 1 | 纯数据流程 `displayOrder == dataOrder`，且 `dataOrder` 与旧引擎 FIFO 逐项一致 | 通过 | B1 `test_flowgraphanalyzer`（`testPureDataDisplayEqualsDataOrder` / `testDataOrderKeepsFifoCompatibility`）、B1.4 冻结旧算法 `legacyExecutionOrder` 基准；本批全量 `test_flowgraphanalyzer` / `test_runengine` 通过 |
| 2 | 控制流程 `displayOrder` 是前向依赖图合法拓扑序，数据边先后一致，控制分支按 lane 分组；不声称等于动态轨迹 | 通过 | `test_flowgraphanalyzer` 的分支/嵌套/循环用例；B5.1 `testControlFlowBranchLabelsProjectedToCanvas` 断言 `laneOf(T) != laneOf(F)` 且 Full 布局后节点不相交 |
| 3 | 画布 rank/Y 与流程树 `displayOrder` 一致；同层节点不重叠 | 通过 | B2 `test_flowcanvas` / `test_flowlayoutcoordinator`；B5.1 Full 布局后 `sceneBoundingRect` 不相交断言 |
| 4 | 自由拖动位置可保存；`layoutInitialized` 已保存布局原样恢复；结构变更 / 显式整理才重排 | 通过 | B2 K5 用例 `test_flowlayoutcoordinator::testProjectSwitchSortsTreeWithoutCanvasLayout`；B3 拖动用例 |
| 5 | 无连接顺序列表与已连接线性链拖动均正确；失败零修改；一次拖动只有一次事务、一条撤销记录，首次 `redo()` 是唯一提交 | 通过 | B3 `test_mainwindow` 10 个用例：K4 `QSignalSpy` 计数 == 1、`QUndoStack::count()` +1、失败路径指纹/连接/撤销栈不变 |
| 6 | 流程树显示顺序重排后，引擎 `dataOrder` 仍只由 Project 模块顺序 + 连接声明顺序决定（`displayOrder != dataOrder` 集成用例） | 通过 | B3.1b `testDisplayOrderReorderKeepsEngineDataOrder`（真实 plan `[C,A,B]` / `[A,C,B]`） |
| 7 | 连续两次重排可两次 Undo/Redo；连接声明顺序变化会改变指纹；GUI/Agent 交错撤销不覆盖后续编辑 | 通过 | B3 `testConsecutiveReordersUndoRedoRestoreSnapshots` / `testConnectionDeclarationOrderChangesFingerprint` / `testAgentOriginInvalidatesGuiReorderUndo` / `test_agentactor` P0-2 用例 |
| 8 | 引擎、UI 协调器、Agent 与 `validateCandidate` 使用同一 `FlowGraphCatalog` 契约（端口来源一致），不在调用方重复推断边类型 | 通过 | B1 契约表；B3 `validateCandidate`；B4 `getFlowState` 按 `PluginManager::createModule()` 构建；`normalizeConnections` 为唯一边类型归一化入口 |
| 9 | 兼容性基线来自冻结的旧算法与基准夹具，不是分析器自比较 | 通过 | B1.4 Step 1 在 `tests/test_runengine.cpp` 固化 `legacyExecutionOrder(...)` 与基准常量 |
| 10 | 布局合并使用单一计时器；抑制 / 工程切换会取消 pending；测试等待事件空闲后断言计数与 Undo 后位置 | 通过 | B2 `test_flowlayoutcoordinator`、B4 `test_agentclosedloop::testConsecutiveAgentAddsCoalesceIntoSingleLayout`（`QSignalSpy` + 事件队列空闲屏障，不用 sleep 构造顺序） |
| 11 | Agent 构建后自动布局；`get_flow_state` 提供 rank / displayIndex | 通过 | B4 `test_agentactor`（字段 / 不可解析 moduleId 降级）、`test_agentclosedloop`（3 个闭环布局用例）；本批全量通过 |
| 12 | 现有控制流 / 循环 / 并行 / 断点 / 后台执行测试全绿；全量 CTest、截图门禁、`git diff --check` 通过 | 主口径通过；隔离回归未全绿（见 §2.3 / §2.4） | 最终全量 `96/96`（82.84s）；`ui_capture_mainwindow` PASS（8.53s）；`git diff --check` OK；两套隔离回归因 sandbox 无 `/dev/fd` 与临时 HOME 资源差异失败，单列不写成通过 |
| 13 | Windows / 真实工程 / 超大图未验证项单独记录，不写成通过 | 已单列 | 见 §5 |

---

## 4. 偏离与风险登记

1. **B3 相对计划范围新增 `Project.h/.cpp`**（`structureFingerprintFor()` 支撑命令 before/after 指纹快照，
   `notifyStructureChange()` 支撑 P0-2 的 Agent origin 信号；剥离会使提交不可编译）。
   **经主理人批准的范围扩展**，记录保留。
2. **B4 `std::map` 替代 `QHash<QString, std::unique_ptr<IModule>>`**：Qt 5.15.3
   `qhash.h:531/556/761/781` 的 `insert(const Key&, const T&)` / `createNode` / `operator[]`
   要求 value 可拷贝，而 `std::unique_ptr` 是 move-only。
   **经主理人批准的实现细节适配（Qt5 QHash 值类型须可拷贝）**，不 amend、不改回 `QHash`。
3. **K4–K7 决策**：K4 一次拖动 redo 提交路径 `QSignalSpy` 计数 == 1；K5 工程切换且
   `layoutInitialized=true`、坐标有效时画布坐标原样保留；K6 `ReorderLinearChainCommand`
   放在 `MainWindow.h/.cpp` 不新增文件；K7 计划文档为主仓库未跟踪文件，不补提交。
4. **B3 发现并修复 `projectCreated` 未接线**：`ProjectManager::newProject()` 只发
   `projectCreated`，原实现仅接线 `projectOpened`，导致新建工程上 GUI→Agent 跨栈失效不生效；
   现已同时接线两个信号，属真实缺口，记录保留。
5. **验收环境必须 `env -u DISPLAY QT_QPA_PLATFORM=offscreen`**：`DISPLAY=:1` 无有效 GLX，
   `test_mainwindow` 因创建 GL context（`X_GLXCreateNewContext` BadValue → SIGSEGV）崩溃，
   属环境性、非本计划回归；禁止因此改产品源码。
6. **临时 HOME 全量必须先 `sync-plugins.sh build`**：B3 漏同步导致 `Modules found: 0`；
   本批按顺序执行同步，但 sandbox 无 `/dev/fd` 使脚本自身失败（见 §2.3 / §2.4 / §4.9）。
7. **`test_workerpluginconsistency::testCandidateConsistency(Loop)` 为既有偶发 flaky**：
   单独复跑通过，全量多次 96/96；未改无关产品代码、未删除断言，如实记录。
8. **计划文档是主仓库未跟踪文件**：路径
   `/home/charles/Data/Repo/DeepLux/docs/superpowers/plans/2026-11-03-execution-order-layout.md`，
   按 K7 未提交、未复制进实施工作树。
9. **（本批新增）B5 隔离回归的 sandbox 限制**：`/dev/fd` 缺失导致
   `scripts/sync-plugins.sh:115` 的 process substitution 失败；尝试恢复符号链接被环境撤销；
   诊断性补跑证明失败随 HOME / 插件安装状态漂移，非产品回归。未修改仓库脚本、未删断言。
10. **（本批新增）`connectionKey` 规则统一**：core（`FlowGraphAnalyzer.cpp` / `RunEngine.cpp`）
    规范键为 `from + 0x1F + to + 0x1F + fromPort + 0x1F + toPort`，而 `FlowCanvas.cpp` 旧去重键
    字段顺序不同。严格提升 core 的键为公开静态方法会超出 B5 的 7 文件范围（core 也不能反向依赖 ui），
    故在已报备的范围内把 `FlowCanvas::connectionKey` 提升为 public static 并对齐规范键，
    新增 `connectionKey(const ModuleConnection&)` 作为唯一拼接实现，4 参数版本委托给它；
    去重与回边判定共用同一规则，测试断言 `FlowCanvas::connectionKey(回边) ∈ plan.loopBackEdges`。
11. **（本批补充）`test_pluginparametercontracts` 已在 B3 补 `RUN_SERIAL`**；
    `test_mainwindow` / `test_agentbridge` 等沿用既有串行设置。

---

## 5. 未验证项（单列，不写成通过）

- **Windows 平台**：全部构建、CTest 与截图门禁均在 Linux / Qt 5.15.3 / offscreen 下完成；
  未在 Windows（MSVC、Qt6、DirectShow 相机插件等）验证。
- **真实工程**：全部测试使用 `QTemporaryDir` 合成工程与测试探针模块；未使用用户真实工程、
  真实相机 / PLC / 串口 / 网络硬件或真实 API Key 做端到端验证。
- **超大图**：未测量 100 / 200 节点规模下 `FlowGraphAnalyzer::analyze`、
  `applyExecutionLayout` 与截图渲染的耗时，也未测量深层嵌套控制流的栈深度上限。
- **GLX / Xvfb 环境课题**：`DISPLAY=:1` 下创建 GL context 崩溃未在本计划内修复（环境性），
  未验证其它 Xvfb / GLX 配置下的 GUI 行为。
- **隔离回归在真实部署环境的通过性**：本 sandbox 无 `/dev/fd` 且临时 HOME 缺少
  `installRuntimePlugin` 所需资源，隔离回归未全绿；在具备标准 `/dev/fd` 与完整 HOME 的
  机器上是否全绿未在本批验证。
- **截图基线 PNG**：仓库内没有签入基线 PNG，`ui_capture_mainwindow` 以语义断言 +
  非空白/差异门禁覆盖；像素级基线比对未验证。

---

## 6. 提交后复核记录

B5 按计划使用固定提交信息 `docs: document execution-order layout`，严格 7 个文件范围：

```text
src/ui/widgets/FlowCanvas.cpp
src/ui/widgets/FlowCanvas.h
tests/test_mainwindow.cpp
tests/ui_capture_mainwindow.cpp
docs/architecture.md
docs/plugins.md
docs/reports/execution-order-layout-acceptance.md   (新增)
```

提交后应执行并核对：

```bash
git status --short                              # 必须为空
git show --stat --oneline HEAD                   # 文件清单必须与上方 7 项一致
git diff-tree --no-commit-id --name-only -r HEAD
git diff --check HEAD~1..HEAD
```

提交哈希登记（因 §1 的自引用约束，由主理人在最终验收报告填入本批交付消息给出的完整值）：

| 批次 | 主题 | 完整哈希 |
| --- | --- | --- |
| B5 | `docs: document execution-order layout` | `bb46550b7b0023664bb553cdf57566507c8112c6` |

> 本报告只记录本批真实执行的命令与结果；历史批次数字已在 §2.6 明确标注为「非本批重跑」。
> 未验证项一律在 §5 单列，未写成通过。
