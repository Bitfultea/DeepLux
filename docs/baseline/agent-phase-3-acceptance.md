# 阶段 3 验收记录：异步运行、业务结果和错误回读

> 历史验收快照：本文状态与结果对应其记录日期和基线；当前项目状态见 [docs/STATUS.md](../STATUS.md)。

依据 `docs/agent-capability-improvement-plan.md` §8。阶段 2 已于 2026-09-30 经独立审核判 G2 PASS，放行范围仅限进入阶段 3。**状态：PASS；2026-10-12 独立审核放行进入阶段 4。**第 1、2 片记录保留为过程证据；当前完整实现及复审证据见 §7。

## 1. 编码前基线（F01/F11）

- `RunEngine` 已在 `tryBeginForRun` 为每次新运行生成 `m_runId`，并传入模块 `ExecutionContext.runId`；但没有线程安全的公开查询 API，`RunResult` 和 `runStarted` 信号也不携带 ID。
- Agent 的 `run_flow` 仅返回 `status=started, mode, async=true`；`get_run_results` 只返回累计次数、上次耗时和 `isRunning`，无法按本次运行取状态、失败节点或输出。
- 引擎 `moduleOutput(moduleName)`、`getOutput(moduleName,key)` 是最新缓存，未按 runId 分区；单次运行工作线程在发 `runStarted` 前清理模块输出。立即查询或失败/取消/未执行分支都不能把旧缓存当本次结果。
- 引擎实际状态有 `RunState::{Idle,Running,Paused,Stopped}`、`isBusy()`、`runFinished(RunResult)`、`moduleStarted/Finished/Skipped` 和 `errorOccurred`；须复用这些信号/状态建立可审计快照，而不是从累计计数推测。

## 2. 契约草案（按片冻结）

1. **第 1 片已冻结：**`run_flow(mode=once)` 成功启动返回现有执行上下文的非空 `runId`、`status=started`、`mode=once` 和 `async=true`；`RunResult.runId` 在完成信号中与启动响应一致。连续单次运行 ID 不相同；启动失败不返回伪 runId。工程/配置指纹、循环模式会话 ID 和每轮 ID 属后续片，当前不声明完成。
2. `get_run_results({runId})` 返回该次运行的 `state`（至少 running/paused/completed/failed/cancelled）、`isBusy`、开始/结束时间、当前/失败模块、错误码与诊断；保留旧累计字段作兼容。未知 ID、旧工程或修改后的工程分别明确返回 not_found/stale，不回退到最近一次成功值。
3. 模块结果入口以 `runId + instanceId + port/key` 定位；运行中为 pending，失败节点/未执行分支为 unavailable，成功时带来源、类型、单位和值。图像/掩膜/点云返回摘要与受控引用，不内嵌大载荷；表格默认每页 100 行、最多 1000 行，响应不超过 64 KiB，返回总数/截断标记。
4. 等待路径必须保持 Qt 事件循环和取消响应；超时是 pending/timeout，不是 run failed。模拟 LLM 立即追问结果的测试不得在测试体代替生产 Agent 等待。最近相关失败事件进入查询和模型上下文，但有数量/字节上限。

## 3. G3 门槛核对（实现方自检，非独立 PASS）

- [x] 固定 fixture 的找圆、两点距离、拟合、Blob、3D 平面：Agent 值与引擎/期望值在声明容差内一致。
- [x] 慢模块立即查询 running，完成后同一 runId 可读；A 成功后 B 失败/取消、分支跳过、暂停、改参或换工程不得返回 A 的成功值。
- [x] 缺输入、端口类型错误、算法失败、服务超时有节点/端口/原始诊断；无结果明确 unavailable。
- [x] 大结果 64 KiB、表格分页、图像/坐标/NoData 摘要及不可序列化类型均有边界测试。
- [x] 无头和 GUI 事件循环心跳：`QT_QPA_PLATFORM=offscreen` 的模拟 LLM 自动续接测得 50 ms 定时器最大间隔 51 ms；显示的 `MainWindow` 在慢运行期间测得 60 ms，均低于 500 ms。当前命令行子命令不提供 Agent 交互入口，未把它冒充为另一个已验证的 Agent 路径。
- [x] 全量构建/CTest、原始证据、矩阵及独立审核 G3 PASS；放行范围仅限进入阶段 4。

## 4. 风险与未定点

`m_runId`、当前模块和模块输出分属不同锁域；公开查询必须拿一致快照，不能直接无锁读取。循环模式每帧 ID 与会话 ID 的关系、`get_module_output` 的序列化白名单、非阻塞等待的控制器续接协议，须在实现对应片前定稿并先写失败用例。第 1 片只在取到引擎执行租约后复制 ID，不开放尚未一致的状态/输出快照。真实 PLC/SAM/相机仍待后续现场阶段。

## 5. 第 1 片：单次运行身份（非 G3 验收）

`RunEngine::runOnceAsync` 以可选出参在取执行租约后返回本次 ID；既有无参调用保持兼容，拒绝启动清空出参。`RunResult` 增加 `runId`，已取得运行权的正常完成、校验失败、中断路径均以本次局部 ID 发信号；未取得运行权的早期错误不伪造 ID。`AgentActor::run_flow(mode=once)` 直接返回该出参。复现测试 F11 保留原用例名并翻转启动无 ID 的断言，验证连续两次 ID 不同且各自完成信号匹配；`test_runengine::testAsyncFailureCarriesAcceptedRunId` 验证失败运行亦与启动 ID 一致。`get_run_results` 仍只有旧累计字段，F01 仍按未收口缺口保留。

第 1 片 `cmake --build build-codex-agent -j2` 退出码 0；全量 `QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure -j2` 为 78/78，0 失败、0 跳过，退出码 0；复现套件 20/20，退出码 0；`git diff --check` 退出码 0。原始构建日志、JUnit、CTest 和复现日志在 `build-codex-agent/agent-phase3-evidence/`。本记录不会因这些测试通过而标记 G3 PASS。未提交、未推送，也未进入阶段 4。

阶段 2 SHA 清单锁定的是进入阶段 3 前的未提交工作树；阶段 3 修改了同一源码/二进制后，不能再拿该清单对当前文件执行校验并声称阶段 2 仍是同一快照。阶段 2 的独立 PASS 和原始清单保留不动，阶段 3 使用独立证据目录和清单。

## 6. 第 2 片：按运行 ID 查询与最小输出快照（2026-10-08，仍非 G3 验收）

在已有未验收的 `get_run_results`、`get_module_output`、`wait_for_run` 草案上收紧语义：`RunEngine` 在取执行租约时记录运行观察值，模块/状态/完成信号以同步连接更新受锁保护的快照；`get_run_results({runId})` 可区分 running、paused、stopping、completed、failed、cancelled，未知 ID 返回 `RUN_NOT_FOUND`。仅保留当前运行及最近一次完成运行，旧 ID 被淘汰后明确 `RUN_NOT_FOUND`，不回退到累计统计或最新成功值。

`wait_for_run` 现为**单次非阻塞轮询**，运行中返回 `pending` 与 `retryAfterMs=50`，不在 UI 线程执行 `processEvents`/`msleep` 循环；它还不是控制器自动续接协议。新一轮开始时清空命名输出与模块输出；`get_module_output` 要求 `runId`，只有当前成功完成的运行可取，且输出身份校验和图像/命名数据复制在锁内完成。运行中为 `pending`、失败或取消为 `unavailable`、旧轮次为 `RUN_NOT_FOUND`。Agent 发起运行时记录工程内容 SHA-1；即使由 GUI 改动工程，读取也以 `STALE_OUTPUT` 失败关闭。新增 MeasurementInput 两点坐标读回的定向测试。

本片只证明上述路径，不宣称完整 G3：尚未覆盖跨 Actor 的工程版本、循环会话/每轮 ID、所有端口值及测量单位、错误节点/端口、不可序列化值、表格分页与 64 KiB 全响应上限，也未覆盖固定算法 fixture、Observer 失败事件和模拟 LLM 自动续接。现有 `get_module_output` 对不支持的 QVariant 仍可能退化为类型名，不能把它视为完整的类型契约。G3 清单仍全为待验；阶段 4 不放行。

本片最终全量构建 `cmake --build build-codex-agent -j2` 退出码 0；全量 `QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure --output-junit agent-phase3-evidence/ctest-slice2-final.xml -j2` 为 78/78、0 失败、退出码 0（在允许本地 socket 的环境执行）。新增定向 `testG3MeasurementOutputByRunId` 通过；`git diff --check` 退出码 0。原始 JUnit 位于 `build-codex-agent/agent-phase3-evidence/ctest-slice2-final.xml`；前轮 `ctest-slice2.xml` 原样保留。尚未生成本片完整 SHA/二进制清单或取得独立 G3 审核结论。

## 7. 完整阶段 3 实施与复审（2026-10-12，G3 PASS）

- `run_flow` 返回运行 ID；循环模式区分会话和每轮 ID。`get_run_results` 按 ID 提供运行/暂停/完成/失败/取消、工程陈旧、当前/失败节点、失败端口与原始诊断。`get_module_output` 绑定运行及工程指纹，端口与元数据键分开，失败/取消/跳过分支均不回退旧值；相邻循环轮次保留最近完成快照。
- 输出白名单支持测量值、强类型几何、检测列表、表格、图像/NoData 摘要；表格默认 100、最多 1000 行，完整响应不超过 64 KiB，超大单行返回 `ROW_TOO_LARGE`。未知 ID/key/port 的超长请求在 256 字符处拒绝，错误也不突破 64 KiB。点云只读取并预览前 64 点，不复制整份点云；区域返回 polygon 预览，常规 RLE 掩膜可缩到 64×64 PNG。超过 1 MP 或 1 MiB RLE 时预览明确 `maskUnavailable`，不解码整份大掩膜；`maskExport` 提供同一运行/实例/键的 `maskOffset` 分块读取入口（每块 8192 字符），测试将超过 1 MiB 的有效 RLE 逐块重建并解码，逐次响应均不超过 64 KiB。
- `wait_for_run` 为一次非阻塞查询；控制器在内部定时续接立即追问，不消耗新的模型轮次。待查询审计先记 `pending`，完成/失败/超时后追加终态；同批后续工具失败、清空对话、关闭控制器或 LLM 报错会追加 `cancelled`，不会留下假成功或无终态的记录。最近失败事件有上下文长度上限。复现测试包含控制器模拟 LLM、连续 50 ms 心跳、取消/陈旧/跳过、端口冲突、缺输入/类型不符/服务超时，以及真实 FitPlane 成功后共线失败的 Agent 诊断。
- 证据：`build-codex-agent/agent-phase3-evidence/build-current.log`（全量构建退出码 0）、`ctest-current.log` 和 `ctest-current.xml`（全量 78/78、0 失败、0 跳过、退出码 0）、`repro-current.log`（32/32、模拟 LLM 心跳最大间隔 51 ms）、`gui-heartbeat-current.log`（可见 MainWindow 定向 3/3、最大间隔 60 ms）、`acceptance-current.log`（19/19）、`fitplane-current.log`（52/52）；原始测试退出码均为 0。此前证据另名保留。矩阵经生成脚本 483/483 引用校验，69 插件 × 7 维度，输出为 8 PASS / 58 NOT_TESTED / 3 BLOCKED；没有将共享读取机制外推为全部插件 PASS。
- 来源与二进制摘要见同目录 `sha256sums.txt`（29 项）、`binary-manifest.txt`（73 项）。本阶段未提交、未推送；放行进入阶段 4 不代表全部插件或现场设备已验收。

**独立审核结论：G3 PASS。** 审核者：独立 Agent `/root/phase2_independent_audit`；时间：2026-10-12。审核者只读核对当前工作树的 29/29 源码与证据 SHA、73/73 二进制 SHA、483/483 矩阵引用、CTest 78/78、复现 32/32、验收 19/19、3D 52/52、可见 GUI 心跳最大 60 ms，以及 `git diff --check`；审核者未独立重跑全量构建和测试。上轮的大 RLE/点云复制、掩膜分块、待查询终态和可见 GUI 心跳阻断已收口；同批后续工具失败与清空对话的 `pending → cancelled` 另有回归。**放行范围仅限进入阶段 4；不代表全部插件 PASS，不授权提交或推送，真实设备与现场吞吐未查。**
