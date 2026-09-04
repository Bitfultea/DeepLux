# 本轮待办表（阶段 A 产出，2026-08-17）——实时状态页

> 状态枚举：完成 / 部分完成 / 未开始 / 外部阻塞。
> 本表为**统一实时状态页**；各 `phaseN-acceptance-record.md` 为**验收快照**（记录当时点结论，不随后续修改更新）。
> 基线：分支 `main`，HEAD `2a132d7`，工作树干净，`ctest` 58/58。

| 项 | 状态 | 说明 |
| --- | --- | --- |
| 阶段 0 基线/清理/验收工程 | 部分完成 | 矩阵+hotfix 映射+死入口清理+acceptance 工程+正式截图(1920/1280 深浅)+旧版输出端口静态对照已做；逐值运行结果等价核验、多类验收工程未完成 |
| 阶段 1 ABI v2/端口/契约 | 完成 | IModule/2.0、端口校验、契约测试 |
| 阶段 1 复杂载荷类型 | 部分完成 | DetectionList 已严格校验；Mask/Region 等暂无生产者并失败关闭，专用载荷仍待实现 |
| 阶段 2 工程 3.0/迁移器 | 完成 | flows/resources/migration、幂等迁移、.v2.bak |
| 阶段 2 note/enabled/breakpoint | 完成 | 本轮阶段 B 已交付 |
| 阶段 3 数据 DAG/并行原语/Skipped | 部分完成 | 拓扑/扇出/汇合/预检/并发/Skipped 已交付 |
| 阶段 3 控制端口驱动执行/汇合 all-any | 完成 | 本轮阶段 C/D 已交付（控制边契约+控制图执行 D1/D2/D3） |
| 阶段 3 Parallel 接入主循环/解除实验性 | 完成 | Parallel 控制端口、主循环批次、线程安全白名单和回归测试已交付 |
| 阶段 E 多输入聚合/Parallel | 完成 | multiple 聚合、controlJoinPolicy、批量并行、blocking 排除已交付 |
| 阶段 F 画布端口交互 | 完成 | 字符串端口、四元组连接、拖线、数据/控制边样式已交付 |
| TSan 并发验证 | 部分完成 | 已实际执行（需 setarch -R 关 ASLR）；阶段 6 生命周期同步点重构后复跑（108 用例）SEGV/heap-use-after-free 清零、data race 单次样本 49/复跑范围 49–62（未插桩 Qt5 误报），未清零不标通过；见 `tsan-report.md` 与 `tsan-runengine-full.txt` |
| 正式尺寸 GUI 截图(1920/1280 深浅) | 完成 | 环境可离屏渲染，已产出 4 张 `screenshots/formal_{1920,1280}_{dark,light}.png`，深浅像素差异已验证 |
| 阶段 G 13 重构插件/业务包 | 部分完成 | metadata execution 标记+hotfix 映射结论/证据+TimeSlice 修正+blocking 接入+13 插件行为级测试（64/64）已完成；旧版输出端口静态对照已完成（50 插件，见 `legacy-comparison.md`），逐值运行结果等价仍未做，结论分布见映射清单 |
| 阶段 H 生产验收/交接报告 | 完成 | 见 `phaseH-handover-report.md`；执行与交接闭环完成，64/64 测试、格式门禁、截图/TSan/旧版对照证据齐全；生产门禁遗留仍按本表“部分完成”项跟踪 |

## 本轮执行顺序（A→H）

- A 基线冻结（本表）→ B 模块字段 → C 控制边契约 → D 控制图执行(D1/D2/D3) →
  E 多输入+Parallel → F 画布端口交互 → G 插件收口 → H 生产验收+交接。

## 阶段 H 后复核整改（步1–6）

| 步 | 内容 | 状态 | 提交 |
| --- | --- | --- | --- |
| 1 | P0 修复：LinesDistance 有限线段、FreeformSurface 凸包面积 | 完成 | 8c0eb08/e45e881 |
| 2 | 点集→直线拟合验收流程 + 带结果工程截图 | 完成 | 2ce0e6c |
| 3 | 核心插件收口，四级结论判定 + Matching 输出补齐 | 完成 | 377a9d2 |
| 4 | Circle2D/DetectionList 强类型载荷 + FindCircle 端口式输出 | 完成 | 134489b |
| 5 | 长循环/停止/取消压力测试 + TSan 报告补充 | 完成 | 14db1a0 |
| 6 | CI 移除静默失败 + 格式/Windows 门禁 + 硬件模拟/业务包说明 | 完成 | 本提交 |

### 步6 说明

- **CI**：`ci.yml` 移除 `sync-plugins || true` 静默失败；格式门禁改为只查 PR/提交
  变更的 C++ 文件（受控列表，`set -e` 不静默）；`windows-msvc` 补 `qtserialport`。
- **硬件模拟**：相机验收以 `GrabImage(File)` 作为无硬件模拟源（固定测试图像）；
  PLC/AI 无模拟器，相关验收保持"依赖设备"标记，先契约测试。
- **业务包**：7 个业务包依赖已在 `hotfix-plugin-mapping.json` 记录
  （`dependency_recorded`），需现场硬件验收，不在本环境闭环。

## 第二轮七阶段整改（2026-08-25）

| 阶 | 内容 | 状态 | 提交 |
| --- | --- | --- | --- |
| 1 | LinesDistance 确定公共点（交叉/接触/共线重叠）+五组测试 | 完成 | bd2ae32^.. |
| 2 | 可取消短循环停止测试 + 50 次稳定性 | 完成 | 同上 |
| 3 | GUI 验收证据链（runFinished+误差+选节点+xvfb） | 完成 | 65333a6 |
| 4 | 迁移结论修正 + 只读一致性测试 | 完成 | 37a6e37 |
| 5 | CI 格式门禁受控 + Windows SerialPort | 完成 | 8e6170d |
| 7 | 控制流 GUI 验收工程接入自动化 | 完成 | bd2ae32 |

### 最终门禁（第二轮）

- 全量测试 **64/64** 通过。
- 格式门禁（变更文件 `clang-format --dry-run --Werror`）**0 违规**。
- 截图像素+内容：1920/1280 截图含图像/圆叠加/节点状态/耗时/检查器（已验证）。
- 映射一致性：`testMappingConclusionConsistency` 通过（JSON 与 MD 数量一致）。

### 仍保留（产品级，非门禁阻塞）

- 图像 ROI/边缘点提取→FitLine/FitCircle 的完整 GUI 交互验收；
  阶段 4/5 已覆盖 FitCircle 自动创建 `point_set` 输入、3 次主窗口鼠标拾取写参、
  圆拟合结果叠加与流程语义（阶段 5 另有 Agent/SAM/截图端到端，见下节口径）。
- SAM 真实 GPU 模型（权重）现场/夜间验收；CI 不下载权重，协议路径由测试内 HTTP 服务覆盖。
- PLC/AI 设备模拟器契约（需现场硬件）。
- TSan 残余误报清零（阶段 6 后范围 49–62 处，需插桩 Qt 复测）。

## 生产闭环轮次（分支 agent/production-closure，基线 dc146a0）

| 阶段 | 内容 | 状态 | 提交 |
| --- | --- | --- | --- |
| 0 | 冻结基线（干净构建+64 测试） | 完成 | 分支创建 |
| 1 | 冻结旧版能力迁移范围（53 missing 逐项决策+生成器幂等） | 完成 | d7dad13..d1edf84（五轮复核） |
| 2 | 消除公开插件假配置/假成功（ImageScript/JiErHan/ColorRecognition+13 项复核清单） | 完成 | 1dacf1f..0d350f5（四轮复核） |
| 3 | 收口数据与构建契约（未实现类型加载期拒绝、点云键值校验、端口数组门禁、OpenCV 必需） | 完成 | 5fb386d..ff238eb（二轮复核） |
| 4 | 补齐流程验收：Loop 固定次数/While 条件退出/StopWhile 提前退出/停止取消时限 + Parallel all/any/失败分支/blocking 不并行 + 拾取→圆拟合真实工作流 | 完成 | d6e9028..02bc644（含复核收口） |
| 5 | Agent、SAM 与 GUI 端到端验收：确定性假 LLM 完成"创建 GrabImage→FindCircle→连接→运行→读取结果"；SAM 测试内 HTTP 服务覆盖四端点（成功/超时/崩溃恢复）；ui_capture 注册 CTest 且截图自校验；GUI 真实交互（鼠标拾取→圆拟合叠加、条件分支画布状态、像素断言） | 完成 | 58fadba..cf6dc87（六轮复核收口） |
| 6 | 并发风险收口：审计 RunEngine 工作线程信号连接（带上下文 Auto→Queued，无跨线程直操 QWidget）；并行批次 ImageData 只读边界；runId 固化（毫秒+单调序号，池线程不读成员字符串）；完整生命周期协议（tryBeginExecution 唯一取权/stop/start/load/clear 同锁/断点外层提交）；executeParallel 恒自生成 runId；7 个定向测试+50 次并行压力+并发 stop/load/clear/断点同时停止回归；TSan 分节（SEGV/HUAf 清零，范围 49–62 误报不标通过） | 完成 | adff5b5 + 本提交（stop() 复核二/三轮） |

### 阶段 4 流程验收口径

- 控制流验收工程位于 `tests/acceptance/projects/`，全部固定工程+确定性预期，
  由 `test_acceptance_flows` 执行；引擎级细节（单步/断点/回边拒绝等）仍由
  `test_runengine` 覆盖。
- 此前"控制流/并行已交付"仅指实现与引擎级回归（阶段 3/E 行）；**流程级验收**
  自本阶段起才有工程证据，勿将"已交付"表述当作"已验收"。
- 循环验收含重复运行 50 次无跨帧污染；停止/取消时限以 500ms 期限断言。

### 阶段 5 端到端验收口径

- **Agent**：`test_agente2e` 用确定性假 LLM（`ScriptedLLMClient`，无任何网络请求）
  驱动 Autopilot 完成"创建 GrabImage→FindCircle→连接→运行→读取结果"完整闭环，
  断言流程结构、参数写入、运行成功、圆心/半径结果（固定验收图）与 `get_run_results`
  统计回传。
- **SAM**：`test_sambackendclient` 新增测试内 HTTP 服务（`SamTestServer`），
  覆盖 `/health`、`/set_image`、`/predict`、`/unload_image` 四端点；
  成功路径断言状态机、embedding、polygon/bbox/score/mask 解析；
  超时为真实超时（`setTimeoutMs` 可调，断言 300ms 时限）；
  崩溃恢复断言原端口重启后回到 Ready 并预测成功。
- **GUI**：`ui_capture_mainwindow` 已注册 CTest（offscreen），程序自校验截图
  存在/尺寸/非空白/关键界面状态，任一失败测试即失败；鼠标拾取→圆拟合叠加与
  条件分支画布状态由 `test_mainwindow` 真实交互用例+像素断言覆盖。
- **边界**：CI 不下载 SAM 权重；真实 GPU 模型保持现场/夜间验收。
