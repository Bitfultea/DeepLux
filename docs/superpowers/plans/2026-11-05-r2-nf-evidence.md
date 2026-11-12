# R2 NF 验收记录（负验证证据链）

本文件记录 R2 并发修复的负验证（NF）证据：版本、命令、预期断言与结果。
不存放原始日志；原始输出仅按需在评审时提供。

约定：
- 每个任务两个提交——① `test:`（NF 用例）② `fix:`（生产修复）。
- NF 三步：① 提交上用例在未修复代码失败于预期断言；② 修复后全量通过；③ 临时校验工作树 `git revert <fix>` 后复败于同一断言，随后删除该工作树。
- **缺陷态／诊断态只允许存在于独立临时工作树**（或工作区外独立副本 + 独立构建目录）；**严禁在交付工作树切换缺陷态**，交付分支工作树在任意时刻只能保留受审的修复态。NF 记录必须写明实际执行位置（工作树路径、脚本路径与行号），不得用「临时工作树」泛指。（2026-11-06 评审追加，见 5.5.1 偏差记录。）
- 缺陷变体必须与具体生产缺陷一一对应（如「缺失诊断」「提前返回」），重复运行并记录退出码；失败须**干净退出**（无 SIGSEGV、无 hang、无残留线程）。
- 预防性重构／行为契约类任务不得制造 NF 红灯；须在记录中注明契约性质（见「T2.7 性质修正」）。

---

## 0. 全局更正（2026-11-06 外部评审裁定）

### 0.1 R0 归因纠正

`testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach`（本文件 T2.1 所讨论的不稳定用例）由 **R2 的 `e28e223`** 新增；**R0 基线不包含该用例**。此前汇报中曾把该用例的单次 96/96 归因于 R0 并对 R0 作不利推断，**该推断作废**：不得据 R2 新增用例否定 R0。R0 结论以其自身报告 `docs/superpowers/plans/2026-11-05-r0-baseline.md` 为准（外部复核 67.18s 与本沙箱复跑 76.17s 分别保留来源）。

### 0.2 T2.7 任务性质修正

- **撤回「已证实 `wakeOne` 唤错等待者」**。事实：`291f30d` 与 `96e02fa` 的 `FlowExecutionService.cpp` 完全一致；旧实现有 3 个等待点（`enqueue` 启动握手、`workerLoop` 空闲、`waitForResumeOrStop` 暂停）、**1 个 `wakeOne`**（`enqueue` 入队，目标正是关心 `m_pendingJob` 的 worker）、**9 个 `wakeAll`**；`settleStart()` 用 `wakeAll()` 广播。该缺陷方向**不可确定性复现**，旧实现通过功能测试是允许的，**不应制造 NF 红灯**。
- 任务性质改为 **预防性重构／唤醒通道契约**（把唤醒定向投递到谓词发生变化的等待者通道），属契约性质，非已证实生产缺陷。
- **T2.7 不再自动作为 T2.3/T2.4 的硬前置**；若仍保留拆分，须先有稳定契约测试；否则延后，优先处理已证实问题。
- WIP 独立保存，不混入 T2.1 修正；**不批准无测试的生产改动直接合入**（详见第 3 节）。

---

## 1. T2.1 析构期协作关闭（禁止强杀 / 禁止 detach）

### 1.1 状态与撤回说明

| 项 | 值 |
|---|---|
| 原测试提交 | `e28e223`（R2 新增，R0 不含） |
| 原修复提交 | `c262d73`（生产实现，**仍为在位修复**） |
| 基线条目 | `f1b615f` |
| 旧验收结论 | **已撤回** |
| 2026-11-06 裁定状态 | 待整改 → 本文件 1.3 为本轮整改证据，**待用户复核** |
| 整改测试提交 | `e92fcd4` |

**撤回原因**：`shutdown()` 在 `joinWorkerThread()` 调用 `wait(0)` 之前已置 `m_quitRequested` 并唤醒 worker；`wait(0)` **成功代表 worker 正常退出，不是缺陷**，不应强制产生诊断。外部复核用现有构建产物单跑 30 次，17 次失败于同一断言；沙箱基线 8/10 pass、2/10 fail。旧用例不具确定性，**不能据此判定生产 `joinWorkerThread()` 有缺陷**。

生产实现（`c262d73`：有界 `wait(deadline)` → 命中则 `m_shutdownDeadlineHits++` + warning → 继续无界 `wait()` → `delete thread`，绝不 detach/terminate）**未改动，无新的 `fix:` 提交**；本轮整改对象是**验收用例与证据链**。

### 1.2 旧证据（撤回，不作为验收依据）

命令（构建目录 `build/`，`QT_QPA_PLATFORM=offscreen`，清除 `DISPLAY`）：

```bash
./build/bin/test_flowexecutionservice testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach
```

| 阶段 | 结果 |
|---|---|
| 测试提交（未修复） | 失败 — `'service.shutdownDeadlineHitCountForTest() >= 1' returned FALSE`，test_flowexecutionservice.cpp:388 |
| 修复后 | 通过；该套件 18/18 |
| 临时工作树 revert `c262d73` | 复败 — 同一断言、同一行 388 |
| 修复后全量 CTest | 96/96 |

旧用例注入 `setShutdownDeadlineForTest(0)` 隐含「`wait(0)` 必然失败」的前提，该前提不成立，故以上三行**全部撤回**。「0ms 确定触发」的验收结论作废。

### 1.3 整改设计（`e92fcd4`）

- **退出屏障 hook**（`DEEPLUX_TEST_VISIBILITY`）：`WorkerThread::run()` 在 `workerLoop()` 返回之后、QThread 结束之前调用 `runWorkerExitBarrierForTest()`；钩子经「锁内快照 → 锁外调用」实现，**屏障等待不持有服务互斥锁**。生产构建该钩子恒为空，行为不变。
- **独立控制线程放行**：主线程调用 `shutdown()`；控制线程先有界等待 `entered`(3s)（worker 已到屏障）与本轮诊断增量(2s)，再用 **500ms 有界观测窗口**判定「提前返回」（与 T2.2 已批准的有界窗口同构），记录放行前状态后由该线程放行屏障。
- **所有等待有界**：屏障本身 `release.tryAcquire(1, 5000)`，故任何失败路径下 worker 都在 5s 内退出 → `shutdown()` 必返回 → `join()` 不挂死。
- **RAII `ShutdownBarrierGuard`**：放行屏障、join 控制线程、清 hook、恢复 `m_shutdownDeadlineMs=10000`，覆盖所有 `QVERIFY` 提前返回路径；钩子捕获的测试栈变量在销毁前一定被清空。

断言（`tests/test_flowexecutionservice.cpp`，`e92fcd4`）：

| 行 | 断言 | 判据 |
|---|---|---|
| 482 | `shutdownResult` | idle shutdown 返回 true |
| 483 | `elapsedMs < 5000` | 不挂死 |
| 484 | `workerAtBarrier == 1` | 退出屏障确实生效 |
| 485 | `shutdownDeadlineHitCountForTest() >= baselineHits + 1` | **本轮诊断计数增量** |
| 487 | `returnedBeforeRelease == 0` | **放行前 shutdown 未返回** |
| 488 | `workerFinishedBeforeRelease == 0` | 放行前 worker 未退出 |
| 489 | `workerRunFinished == 1` | **worker 真正退出后 shutdown 才返回** |
| 收尾 | 重建线程再跑、`probe.runs == 2` | join 而非 detach |

命令：

```bash
unset DISPLAY; export QT_QPA_PLATFORM=offscreen
./build/bin/test_flowexecutionservice testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach
```

### 1.4 本轮 NF 证据（`e92fcd4`）

| 阶段 | 版本 / 缺陷变体 | 预期断言 | 实测结果 | 退出码 | 重复 |
|---|---|---|---|---|---|
| 修复在位（交付分支） | `e92fcd4` | 全部 | 通过，约 607ms/次；服务套件 18/18 | **0** | **20/20 通过** |
| ① 未修复代码（撤销修复） | 临时工作树 `git revert --no-commit c262d73`（干净应用；析构与 `shutdown()` 恢复为 `thread->wait(); delete thread;`，无诊断；退出屏障 hook 保留） | `:485` 诊断计数增量 | **失败** `'service.shutdownDeadlineHitCountForTest() >= baselineHits + 1' returned FALSE`，约 2.5s，干净退出 | **1** | **10/10 失败于同一断言** |
| ② 修复在位（同一工作树恢复） | `e92fcd4` | 全部 | 通过 | **0** | **10/10 通过** |
| ③ 缺陷变体「提前返回」 | 局部补丁 `/var/tmp/t21-v2-early-return.diff`：保留 `m_shutdownDeadlineHits.fetch_add`，但 `shutdown()` 只做有界 `wait(0)` 后即返回、不 join 到 worker 真正退出；保留 `m_thread` 供析构回收以保证干净退出 | `:487` `returnedBeforeRelease == 0` | **失败** `Actual 1 / Expected 0`，约 54ms，干净退出 | **1** | **10/10 失败于同一断言** |
| ③ 缺陷变体「缺失诊断」 | 同 ①（`git revert c262d73`） | `:485` 诊断计数增量 | **失败** | **1** | **10/10 失败于同一断言** |
| 格式门禁 | `clang-format 14 --dry-run --Werror` × 3 文件 | — | 通过 | **0** | 1 |

说明：
- 临时校验工作树 `/var/tmp/t21-nf-worktree`（detached `e92fcd4`）已用 `git worktree remove --force` 删除；**交付分支始终为 `e92fcd4` 通过态，无 revert 缺陷态**。
- `git revert --no-commit c262d73` 在 `e92fcd4` 上**干净应用、无冲突**（与后续退出屏障 hook 提交不重叠）。
- 「缺失诊断」与「提前返回」两个变体分别命中**不同**预期断言（`:485` / `:487`），证明用例能区分两类缺陷，而非单一红灯。
- 缺陷变体均干净退出（rc=1，无 SIGSEGV、无 hang、无残留线程）；「提前返回」变体保留 `m_thread` 使进程退出时析构仍能 join 回收线程对象。
- 本专项结果（1.4 节）与全量 CTest 连续 10 次门禁（4.2 节，**未达成**）**分开记录**；专项结论不替代门禁，门禁也不否定专项证据。

---

## 2. T2.2 GUI 回投生命周期（Pending/Running/Finished/Cancelled）

| 项 | 值 |
|---|---|
| 修复提交 | `ac460c9` |
| 用例首版 | `f22e37b`（**历史证据，已被判定为假阳性并弃用**） |
| 用例当前版 | `8cba2fe`（有界同步 + 收尾后再断言） |

命令（`QT_QPA_PLATFORM=offscreen`，清除 `DISPLAY`）：

```bash
./build/bin/test_executionaffinity \
  testGuiDispatchTimeoutDropsLateCallback \
  testGuiDispatchTimeoutWhileRunningWaitsForCompletion
```

### 历史证据（首版用例，不作为验收依据）

`f22e37b` 版本在旧 `BlockingQueuedConnection` 下失败于 `runOnce must return after the dispatch timeout`（test_executionaffinity.cpp:190/193）。但复核发现：主线程 `processEvents()` 会同步进入阻塞探针并停留到探针自身 5s 超时，放行发生在回调结束之后；删除 `record->done.acquire()` 的缺陷变体仍通过。故该版判为假阳性，已由 `8cba2fe` 取代。

### 当前版证据（`8cba2fe`）

当前判据：
- Pending：以**不泵 GUI 事件**的 `QSemaphore` 等待任务返回；随后放行探针、投递迟到回调、回收线程，**再断言**。
  - `returned == true`（超时后 runOnce 已返回）；
  - `runs == 0`、错误码 `affinity_dispatch_timeout`、迟到回调 drop ≥1。
- Running：以 `guiDispatchTimeoutHitCount`（有界 3s）为屏障，再用 `taskDone` 的**有界窗口（1s）**判定是否提前返回；断言 `guiDispatchRunningWaitEnteredCount >= 1`、`!returnedBeforeRelease`、重跑被拒；收尾时对意外接受的重跑执行 `stop()` 并等待引擎空闲。

| 阶段 | 预期断言 | 实测结果 | 退出码 |
|---|---|---|---|
| 旧实现（临时工作树局部替换为 `BlockingQueuedConnection`，保留钩子） | DropsLate：`returned` | **失败** `'returned' returned FALSE`（3/3） | **1** |
| 同上 | WhileRunning：`runningWaitHits >= 1` | **失败** `'runningWaitHits >= 1' returned FALSE`（3/3） | **1** |
| 缺陷变体（仅注释 `record->done.acquire()`） | WhileRunning：`!returnedBeforeRelease` | **失败** `'!returnedBeforeRelease.load()' returned FALSE`（5/5） | **1** |
| 修复在位 | 全部 | 通过；亲和性套件 9/9（约 1.4s） | 0 |
| 修复在位 | 全量 CTest | 96/96 | 0 |

说明：`ac460c9` 的 revert 与后续钩子提交在同文件重叠产生冲突，旧实现对照改为在临时工作树中**仅局部替换分发实现**（保留全部钩子），其余代码与当前版一致；临时工作树用完即删。

---

## 3. T2.7 等待者拆分（性质修正，暂停）

| 项 | 值 |
|---|---|
| 原计划条目 | `docs/superpowers/plans/2026-11-05-review-hardening.md` v1.4：「消除 `wakeOne` 唤错等待者」+「实施顺序必须先于 T2.3/T2.4」 |
| 裁定 | **撤回该缺陷表述**；改为预防性重构／唤醒通道契约；**不再作为 T2.3/T2.4 硬前置** |
| WIP 保存 | 分支 `wip/t2.7-wake-channels-do-not-merge`，提交 `be90921`（标注 `DO NOT MERGE`、无稳定契约测试） |
| 工作区外备份 | `/var/tmp/t2.7-wip-wake-channels-DO-NOT-MERGE.patch`、`/var/tmp/t2.7-wip-wake-channels-DO-NOT-MERGE.diff` |

- 复核事实：旧实现 `settleStart()` 用 `wakeAll()` 广播；3 个等待点 / 1 个 `wakeOne` / 9 个 `wakeAll`，`wakeOne` 位于 `enqueue()` 且目标正是关心 `m_pendingJob` 的 worker。「`wakeOne` 唤错等待者」**不可确定性复现**，不得作为 NF 依据；旧实现通过功能测试是允许的。
- 曾创建的假阳性测试提交 `291f30d`（`testStartHandshakeWakesWorkerNotJustWaiter`）已 reset，仅在 reflog 中，未进入交付链。
- WIP 三通道设计（`m_startWake` / `m_commandWake` / `m_resumeWake` + `wakeChannelOne/All(int channels)` 位掩码）**独立保存，不混入 T2.1 修正**；不批准无测试的生产改动直接合入。
- 后续顺序：**优先处理已证实问题**（T2.3/T2.4 等）；如仍保留 T2.7 拆分，须先产出稳定契约测试，否则延后。

---

## 4. 本轮回归门禁

### 4.1 门禁命令与一次环境根因更正

```bash
unset DISPLAY
export QT_QPA_PLATFORM=offscreen
export TMPDIR=/var/tmp/deeplux-ctest-tmp
cmake --build build -j4
ctest --test-dir build --output-on-failure -j4
```

**更正（重要）**：本轮最初启动的 10× 全量 CTest 前 8 轮均为 7–8 个插件相关测试失败（rc=8），曾一度被误判为「`sync-plugins` 未执行、插件未同步」。**该诊断错误，已作废**：

- 测试并不从 `build/plugins` 读取插件。插件来源是 CMake 注入的编译期清单 `DEEPLUX_TEST_PLUGIN_MANIFEST`（`$<TARGET_FILE:...>`，指向 `build/lib/lib*Plugin.so`，编译产物内含 66 项、文件全部存在），由各测试用 `QFile::copy` 自行复制到自己的 `QTemporaryDir`；`sync-plugins` 的目标目录是 `$HOME/.deeplux/plugins`，与测试路径无关。
- 真实根因是 `/tmp` 为 **10M tmpfs**，复制十几个约 1.5MB 的插件 `.so` 时空间耗尽（`test_acceptance_flows` 恰在第 7 个 `FitLine` 失败）。
- **决定性 A/B（同一构建产物）**：

| 场景 | 命令 | 结果 | 退出码 |
|---|---|---|---|
| A 未设 `TMPDIR` | `ctest --test-dir build -R '^test_acceptance_flows$' --output-on-failure` | `FAIL! 'installPlugin("FitLine")' returned FALSE`，0% passed | **8** |
| B `TMPDIR=/var/tmp/deeplux-ctest-tmp` | 同上 | 100% passed | **0** |

- 修正环境后单跑全量：**96/96，rc=0，94.40s**（`/var/tmp/t21-ctest-fixed-probe.log`）。
- 原始失败日志 `/var/tmp/t21-ctest-{1..9}.log` **全部作废，不作为验收证据**；对应后台循环已停止。
- 后续纪律补充：CTest 一律带 `unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR` 指向大容量目录。

### 4.2 修正环境后 10 次连续回归

| 轮次 | 结果 | 退出码 | 失败测试 |
|---|---|---|---|
| 1 | 96/96 | 0 | — |
| 2 | 96/96 | 0 | — |
| 3 | 96/96 | 0 | — |
| 4 | 96/96 | 0 | — |
| 5 | 95/96 | 8 | `test_workerpluginconsistency`：`DataCheck/system/same-instance: port 'image': image payload differs`（:689） |
| 6 | 96/96 | 0 | — |
| 7 | 95/96 | 8 | `test_workerpluginconsistency`：`MeasureLine/detection/two-instances: port 'image': image payload differs`（:689） |
| 8 | 96/96 | 0 | — |
| 9 | 96/96 | 0 | — |
| 10 | 96/96 | 0 | — |

日志：`/var/tmp/t21-ctest-fixed-{1..10}.log`。**合计 8/10 全通过、2/10 各 1 项失败；「连续 10 次全通过」门禁未达成。**

### 4.3 失败归因（**已完成**，2026-11-06 干净构建对照 + 独立复现后确定）

**归因状态：已完成。** 2026-11-06 评审裁定的「归因未完成」降级已由本轮实验证据闭合。上一版「既有非确定性测试、不是 T2.1 引入的回归」曾因证据不足被撤回；现由下面两项**互相独立**的新证据重新确立，并把根因收敛到测试辅助函数本身。

- 已观测事实：失败集中在 `test_workerpluginconsistency::testCandidateConsistency`，断言 `failures.isEmpty()`（:689），错误恒为 `port 'image': image payload differs`；插件与场景各不相同（`DataCheck/same-instance`、`MeasureLine/two-instances` 等）。该测试对 `ImageData::toQImage()` 逐像素 SHA256（`imageSignature`，:255），把 GUI 执行 / 同实例 worker 执行 / 另一实例 worker 执行三者严格比对（:660–670）。
- **归因证据（新，5.2 节）**：`96e02fa` 干净构建与当前提交在**同一环境、同一输入、同一插件集合（各 65 个）、同一测试源码 sha256** 下交错各跑 30 次，基线 5/30 失败、当前 3/30 失败，两侧失败均为 `sigStable=0`。**#5 在 `96e02fa` 已存在，非 T2.1（`e92fcd4`）引入的回归**；原先「测试源码未改只能排除直接修改」的证据缺口由干净构建对照补上。
- **根因证据（新，5.3 节）**：独立 Qt 探针（不含任何 DeepLux 代码、插件、`ImageData`、跨线程）在第 87 轮复现出**完全相同**的 `fd847c8734577aef…` / `47df5431dc839a63…` 签名对；5000 轮定量对比显示「`convertToFormat` 输出 + 240 次分块 `addData`」26 次签名不稳定、而聚合一次 `addData` 0 次，且图像字节 FNV 校验和 0 次变化。**根因是 `tests/test_workerpluginconsistency.cpp::imageSignature()` 在 Qt 5.15.3 上的分块哈希调用方式，与生产代码、共享库重链、`FlowExecutionService.h` 类布局、插件、并发环境、系统负载均无关。**
- **复现条件已锁定**：无需 `-j4`、无需特定插件；触发条件是「`convertToFormat` 输出 + 多次分块 `addData`」这一组合本身，与堆复用时序相关，故单跑 30 次可能不复现而 5000 轮探针稳定复现。
- **裁定保持不变**：不给门禁豁免；不放宽严格比较；不排除测试；不靠重试到绿收口。「既有」不等于「无害」，按第 5 节做最小修复并重跑 10× 门禁（**已完成，见 5.4–5.6：10/10 通过**）。

### 4.4 T2.1 专项结果与全量门禁分开记录

| 项 | 结论 |
|---|---|
| T2.1 专项 NF（1.4 节） | 整改用例证据完整：修复在位 20/20、两个变体各 10/10 复败、服务套件 18/18、全量构建/格式 exit 0。**保留 `e92fcd4` 整改提交，专项结论不受门禁结论牵连。** |
| 全量 CTest 连续 10 次（4.2 节） | **未达成**（8/10 全通过、2/10 各 1 项失败）。与专项结果分开记录，**不得以专项通过替代门禁**。 |
| 修复后全量 CTest 连续 10 次（5.6 节） | **已达成**（10/10 全通过、每轮 `0 tests failed out of 96`、`NONZERO_RUNS=0`）。这是 #5 最小修复后的**重新执行**，与上一行历史记录分开保留，**不改写 4.2/4.4 节原文**。 |

### 4.5 其他门禁

| 项 | 命令 | 结果 |
|---|---|---|
| 格式 | `clang-format 14 --dry-run --Werror` 于 `FlowExecutionService.{h,cpp}`、`test_flowexecutionservice.cpp` | **exit 0** |
| 全量构建 | `cmake --build build -j4` | **exit 0** |
| T2.1 单测 | `./build/bin/test_flowexecutionservice testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach` | 20/20 通过，exit 0 |
| T2.1 套件 | `./build/bin/test_flowexecutionservice` | 18/18 通过 |

---

## 5. #5 图像一致性差异：定位与修复（已完成，2026-11-06）

| 项 | 状态 |
|---|---|
| 门禁豁免 | **不授予**（评审裁定；全程未使用） |
| 归因 | **已完成**（见 4.3 / 5.2 / 5.3）：#5 在 `96e02fa` 已存在、非 T2.1 引入；根因为 Qt 5.15.3 上 `imageSignature()` 的分块哈希调用方式 |
| T2.3/T2.4 | **已获准继续**（2026-11-06 裁定，#5 收口后）；本轮未启动 |
| T2.7 WIP | 继续独立保留，`be90921` 不合入 |
| 结果 | ①–⑤ 全部完成（5.1–5.6）；修复后全量连续 10 次 CTest 门禁 **10/10 通过**（5.6 节），无遗留门禁项 |

### 5.1 失败诊断：捕获双方图像、首个不同像素与差异范围

- `b0b60ac`（受版本控制）新增 `dumpImageDifference()`：失败路径保存双方 PNG，输出首个不同像素、RGBA、差异像素数、差异包围盒；输出目录 `DEEPLUX_IMAGE_DIFF_DIR`（缺省 `/var/tmp/deeplux-image-diff`）。**严格判等未放宽**。
- 固定环境 `unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`，30 次复现 28 pass / 2 fail，日志 `/var/tmp/t21-diag-run-{1..30}.log`。两次失败均 `differentPixels=0/76800`、`firstDiffPixel=(-1,-1)`，双方 PNG sha256 相同（`23f4c911…`）：**失败时两图像素完全相同，但 `imageSignature` 判不等**。
- **临时诊断代码（在交付工作树内直接改写、未提交；执行位置更正见 5.5.1）**，diff 存档 `/var/tmp/t21-image-signature-diagnostic-wip.diff`（含签名复算、`sigStable`、指针/格式/全图校验和/`convStable` 轨迹）：`/var/tmp/t21-diag2-run-{1..30}.log` 29 pass / 1 fail、`/var/tmp/t21-diag3-run-{1..30}.log` 27 pass / 3 fail，失败均 `sigStable=0` 且 dump 中 `sigEqual=1`。证明**同一 `ImageData` 连续两次 `imageSignature()` 结果不同**，与 GUI/worker 输出差异无关。

### 5.2 干净构建对照：`96e02fa` vs 当前提交

- 基线工作树 `git worktree add --detach /var/tmp/t21-baseline-wt 96e02fa`；`git cherry-pick b0b60ac` 得临时提交 `0cbd730`（仅测试诊断）；再把当前工作树测试源码复制过去重编**测试目标**，使两侧测试源码 sha256 完全一致（`e92ae2e1b973…`）。基线生产代码仍为单一 `QWaitCondition m_wake`，当前交付分支含 T2.1 hook（`setWorkerExitBarrierForTest`）——**唯一变量是生产二进制**。
- 基线构建 `/var/tmp/t21-baseline-build`，CMake 关键参数与当前 `build/CMakeCache.txt` 一致（Debug / `USE_QT6=ON` / `Qt5_DIR` / `ONNXRUNTIME_DIR` / `OpenCV_DIR` / `DEEPLUX_NATIVE_SAM=ON` / 三个相机开关 OFF）；`CONFIGURE_rc=0`、`BUILD_rc=0`。
- 交错 30 对（奇数轮 BASE→CUR、偶数轮 CUR→BASE，抵消漂移），脚本 `/var/tmp/t21-ab-compare.sh`，逐次结果 `/var/tmp/t21-ab-compare/summary.tsv`、环境 `/var/tmp/t21-ab-compare/environment.txt`。环境固定：实测 `ldd` 链接 `libQt5Gui.so.5`（Qt 5.15.3）、`DISPLAY` unset、`QT_QPA_PLATFORM=offscreen`、`TMPDIR=/var/tmp/deeplux-ctest-tmp`；两侧插件集合各 65 个。

| 臂 | 运行 | 失败 | 失败率 | 失败特征 |
|---|---|---|---|---|
| BASE `96e02fa` | 30 | 5 | 16.7% | 全部 `image payload differs` + `sigStable=0` |
| CUR 当前提交 | 30 | 3 | 10.0% | 全部 `image payload differs` + `sigStable=0` |

**结论**：#5 在 `96e02fa` 已存在，非 T2.1 引入，两侧失败率无显著差异；原先「测试源码未改只能排除直接修改」的证据缺口由干净构建对照补上。按裁定「既有」不等于「无害」，仍须定位并修复、不给门禁豁免。

### 5.3 根因定位：Qt 5.15.3 分块哈希非确定性

- 判别诊断（**交付工作树内直接改写**，存档同上；执行位置更正见 5.5.1）：失败时四次签名调用的 `src`/`conv` 指针、`cacheKey`、`fmt`（RGB32=4 / RGBA8888=17）、`bpl=1280`、图像头完全相同；但 `sig` 在不同调用间取 `fd847c87…`（正确）、`47df5431…`、`6072a4f8…`、`1c586c0e…`。对**同一 `converted` 连续两次 SHA256** 出现 `convStable=0`（`47df…` → `fd84…`），而随后 FNV 全图校验和 `convCrc=db4b0a34` 四次恒定。
- 独立 Qt 探针 `/var/tmp/qt_image_sig_probe.cpp`（仅 `QImage` + `QPainter` + `QCryptographicHash`，**无 DeepLux 代码 / 插件 / `ImageData` / 跨线程**）：5000 轮在第 87 轮复现 `UNSTABLE at 87: 47df5431dc839a63… vs fd847c8734577aef…`，与测试观测值逐字节一致。日志 `/var/tmp/qt_probe_run.log`。
- 六方式定量对比 `/var/tmp/qt_image_sig_probe3.cpp`，每种方式独立进程、各 5000 轮（`QT_QPA_PLATFORM=offscreen`），日志 `/var/tmp/qt_probe3_run.log`：

| 方式 | 不稳定次数 | 图像字节 FNV 变化 |
|---|---|---|
| 原实现：`convertToFormat` + 240 次分块 `addData` | **26** | 0 |
| `convertToFormat` + `.copy()` + 分块 | 0 | 0 |
| `convertToFormat` + 逐像素分块（76800 次） | **33** | 0 |
| `convertToFormat` + **聚合一次** `addData` | **0** | 0 |
| 源图（不转换）+ 逐像素分块 | 0 | 0 |
| 源图（不转换）+ 逐行分块 | 0 | 0 |

- **判定**：图像字节内容自始至终稳定（FNV 0 次变化；`converted.sizeInBytes()==307200==w*4*h`、`bpl==1280` 无行 padding）；非确定性来自 Qt 5.15.3 上「`convertToFormat` 输出 + 多次分块 `QCryptographicHash::addData`」这一组合。把 240 次分块改为**一次聚合 `addData`** 后，喂入 SHA256 的字节序列与原先逐行版本**完全相同**（尺寸前缀 + 240×1280 字节），正确签名仍为 `fd847c8734577aef…`，因此严格比较强度不变。

### 5.4 最小修复（不改变比较强度）

- 文件：`tests/test_workerpluginconsistency.cpp::imageSignature()`。把「尺寸前缀一次 `addData` + 240 次逐行 `addData(row, width*4)`」改为**把同样的字节序列（尺寸前缀 + 240×1280 字节）聚合进一个 `QByteArray` 后一次 `addData`**。喂入 SHA256 的字节序列与修复前**逐字节相同**，正确签名仍为 `fd847c8734577aef…`；只改变 `QCryptographicHash` 的调用次数，不改变哈希内容与严格判等。
- 新增受版本控制用例 `testImageSignatureStabilityAndSensitivity()`：① 同一 `ImageData` 连续 200 次签名必须完全一致；② 真实 1 像素差异必须仍被 `valuesEqual` 拒绝且报告 `image payload differs`；③ 相同内容（不同 `ImageData` 实例）必须判等（防止修复引入假阳性）。
- `clang-format 14 --dry-run --Werror` 对该文件 **exit 0**；全量构建 **exit 0**；修复版全套 **49/49**（原 48 + 新增 1）**rc=0**。
- 定位过程的临时诊断代码**是在交付工作树内直接改写的**（并非独立临时工作树），未提交；diff 存档 `/var/tmp/t21-image-signature-diagnostic-wip.diff`，随后以 `git checkout HEAD -- tests/test_workerpluginconsistency.cpp` 恢复到 `b0b60ac`，交付分支最终仅保留上述最小修复与新增用例。执行位置更正见 5.5.1。

### 5.5 NF 三步证据链（修复版 / 缺陷变体 / 恢复）

脚本 `/var/tmp/t21-fix-verify.sh`，日志与逐次结果 `/var/tmp/t21-fix-verify/`。环境固定：`unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`。

| 阶段 | 版本 | 命令/次数 | 结果 | 退出码 |
|---|---|---|---|---|
| 修复版全套 | 修复在位 | `test_workerpluginconsistency` 1 次 | 49/49 通过 | 0 |
| 修复版重复 | 修复在位 | 单测连续 **100 次** | **0 失败**（`fixed-100.tsv` 全部 `rc=0`） | 0 |
| 缺陷变体复败 | **交付工作树内**把 `imageSignature` 改回逐行分块 `addData` 并在交付 `build/` 重编（见 5.5.1 偏差记录） | 单测连续 **60 次** | **37 次失败**（61.7%） | 1 |
| 恢复修复版 | **交付工作树内**从 `/var/tmp/t21-fix-verify/fixed-source.cpp` 备份 `cp` 还原并重编 | 单测 1 次 | 49/49 通过；源码 sha256 `8c7ede528728ad44…` 与修复版一致（`RESTORE_VERIFIED=yes`） | 0 |

- 缺陷变体 37 次失败中：**33 次**失败于新增用例 `testImageSignatureStabilityAndSensitivity() Compared values are not the same`（200 次签名不稳定），**5 次**失败于原有 `testCandidateConsistency(...): port 'image': image payload differs`（其中 1 次两者同时失败）。后者 `firstDiffPixel=(-1,-1)`、`differentPixels=0/76800`，与 #5 原始失败形态完全一致。
- 缺陷变体源码 sha256 `1ee233da03d51c5…`、构建 `VARIANT_BUILD_rc=0`，证明变体确实进入二进制；恢复后 `RESTORED_SOURCE_sha256=8c7ede528728ad44…` 与修复版相同。
- **负验证（比较强度未降低）**：新增用例在修复版上通过，输出 `firstDiffPixel=(5,5) refRGBA=000000ff actRGBA=ff0000ff; differentPixels=1/76800; diffBBox=(5,5)-(5,5)` —— 真实 1 像素差异仍被拒绝；相同内容仍判等。未排除任何测试、未放宽严格比较、未靠重试到绿。

#### 5.5.1 隔离纪律偏差：NF 执行位置与记录不一致（P2，2026-11-06 评审指出并更正）

- **记录不实**：5.1 / 5.3 / 5.4 / 5.5 原先把本轮临时诊断代码与缺陷变体的执行位置记为「临时工作树」，**实际并非如此**。`/var/tmp/t21-fix-verify.sh:4` 为 `cd /home/charles/Data/Repo/DeepLux-hardening`，即**交付工作树**：`:33` 把修复版源码备份到 `/var/tmp/t21-fix-verify/fixed-source.cpp`，`:34–52` 用 Python **直接在交付工作树内**改写 `tests/test_workerpluginconsistency.cpp`、把 `imageSignature` 改回逐行分块 `addData`，`:55` 在交付 `build/` 重编测试目标，`:60–65` 连续跑 60 次，`:71` 用 `cp` 从备份还原源码，`:76` 校验 `RESTORE_VERIFIED=yes`。5.1 / 5.3 的临时诊断代码同样是在交付工作树内改写、再用 `git checkout HEAD -- tests/test_workerpluginconsistency.cpp` 恢复，只是把 diff 另存为 `/var/tmp/t21-image-signature-diagnostic-wip.diff`；**没有建立任何独立临时工作树**。5.2 的 `git worktree add --detach /var/tmp/t21-baseline-wt 96e02fa` 是唯一真实的独立工作树，仅用于干净构建对照，不用于 NF 缺陷态。
- **性质**：这是**隔离纪律的偏差 + 证据记录与执行事实不一致（P2）**，不是代码缺陷。按本文件开头「约定」与本条后续纪律，缺陷态／诊断态只允许出现在独立临时工作树或工作区外副本；在交付工作树切换到缺陷态，一旦脚本中断、备份丢失或被并发提交，缺陷态就有进入交付链的风险，且记录失真会削弱 NF 证据链的可复核性。
- **影响评估（不改变已采信结论）**：① 缺陷态**未进入任何提交**——`567b363` 与门禁 `AvKq6Y` 均基于修复版源码 sha256 `8c7ede528728ad44…`，提交前后 `git status` 均干净；② 恢复证据充分——备份 `/var/tmp/t21-fix-verify/fixed-source.cpp` 现存且 sha256 与修复版一致，`RESTORE_VERIFIED=yes`，恢复后全套 49/49、`RESTORED_FULL_rc=0`；③ 全量 10 次门禁在恢复后的干净交付树上执行，与缺陷态切换无交集；④ 评审独立复核确认「格式、`git diff --check` 通过，交付工作树干净」。
- **后续纪律（必须遵守）**：**以后不得在交付工作树切换缺陷态**。所有 NF 缺陷变体、负验证、临时诊断一律在 `git worktree add --detach` 建立的独立临时工作树（或工作区外独立副本目录 + 独立构建目录）内进行，用完即删；交付分支工作树在任意时刻只保留受审的修复态。记录中必须写明实际执行位置（工作树路径、脚本路径与行号），不得用「临时工作树」泛指。该禁令已同步写入本文件开头「约定」、主仓计划文档 v1.8 与项目长期记忆。
- **本条不重跑十轮门禁**：偏差只涉及记录与执行位置，不改变修复版源码、不改变任何测试结果；评审裁定「无须为纯文档更正重跑十轮」，本次仅提交文档更正。

### 5.6 修复后全量连续 10 次门禁（**已达成 10/10**）

脚本 `/var/tmp/t21-gate10.sh`，后台任务 `AvKq6Y`，逐次日志 `/var/tmp/t21-gate10/run-{1..10}.log`，汇总 `/var/tmp/t21-gate10/summary.tsv`。固定环境：`unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`；命令为 `ctest --test-dir build -j4 --output-on-failure`，**未加 `-R`/`-E`、未设置跳过、未放宽严格比较强度、10 次均为独立完整执行**。

| 前置门禁 | 命令 | 结果 |
|---|---|---|
| 全量构建 | `cmake --build build -j4` | `FULL_BUILD_rc=0` |
| 格式 | `clang-format --dry-run --Werror tests/test_workerpluginconsistency.cpp` | `FORMAT_rc=0` |

| 轮次 | rc | 结果 |
|---|---|---|
| run-1 | 0 | `100% tests passed, 0 tests failed out of 96`（`Total Test time (real) = 102.40 sec`） |
| run-2 ～ run-10 | 0 | `100% tests passed, 0 tests failed out of 96` |

- **汇总**：`NONZERO_RUNS=0`、`GATE10_DONE`，**10/10 全通过**。逐轮日志独立核验：每轮均为 `0 tests failed out of 96`、每轮 `test_workerpluginconsistency` 均出现并通过、`skipped`/`Not Run`/`Disabled` 行数均为 0（10 轮合计 0）。
- `git diff --check` **exit 0**；交付测试源码 sha256 `8c7ede528728ad44…`，与 5.4/5.5 节修复版完全一致，证明门禁运行的就是受审的修复版本。
- **收口方式**：#5 最小修复后重新执行完整 10 次门禁并全部通过；**未靠重试到绿、未排除测试、未放宽比较强度、未授予门禁豁免**。4.2/4.4 节 8/10 的历史记录原样保留、不改写，与本轮 10/10 分开记录。
- **T2.1 专项结果与门禁分开**：`e92fcd4` 整改提交的专项结论（1.4 / 4.4 节）不受本轮门禁结论牵连，二者独立记录、不互相替代。

### 5.7 本轮收口状态

| 项 | 状态 |
|---|---|
| #5 归因 | 已完成（5.2 / 5.3） |
| #5 根因 | 已定位：Qt 5.15.3 上「`convertToFormat` 输出 + 多次分块 `QCryptographicHash::addData`」非确定性 |
| #5 最小修复 + 负验证 | 已完成（5.4 / 5.5），严格比较强度不变 |
| 修复后全量连续 10 次门禁 | **已达成 10/10**（5.6） |
| T2.3 | **方案一已收口**（第 6 节）：原计划缺陷前提不成立，改为行为契约／回归保护；基线已绿、非 NF、无生产修复提交 |
| T2.4 | **方案二已收口**（第 7 节）：三项预期行为已满足，尚未证实计划中的缺陷；固化为 Service 层行为契约，不做生产修复、不制造 NF 红灯 |
| T2.7 WIP | 继续独立保留于 `be90921`，**未合入**（裁定重申） |
| R2 整批 | **未完成** —— 本次仅 #5 收口，不代表 R2 整批已完成 |
| 门禁豁免 | 未授予 |
| 隔离纪律偏差 | **P2 已更正**（5.5.1）：纯文档更正，按裁定不重跑十轮 |

---

## 6. T2.3 暂停通知窗口与直接信号回调（行为契约／回归保护，2026-11-06 评审裁定）

### 6.1 任务性质：**基线已绿、非 NF、无生产修复提交**

原计划条目假设 `FlowExecutionService::commitState(Paused)` 在持有 `m_mutex` 时 `emit jobStateChanged`，因而 `Qt::DirectConnection` 槽内调用 `continueRun()` / `stop()` 会自死锁，且「Paused 已提交、worker 尚未进入 `m_wake.wait`」存在丢失恢复请求的窗口。**该缺陷前提经静态与动态核实均不成立**，评审裁定采用方案一：T2.3 改为**行为契约／回归保护**，不做生产重构、不制造 NF 红灯、无生产修复提交，仅提交测试与本节处置记录。

### 6.2 前提核实（不制造 NF 红灯）

**静态事实**（`src/core/engine/FlowExecutionService.{h,cpp}`，对 `acd29e8`、R2 基线 `96e02fa`、当前 `a0ad393` 三个版本一致）：

- `commitState()` 为 `m_state.store(atomic) + emit jobStateChanged`，**从不持有 `m_mutex` 发射信号**；5 个调用点全部在锁块之外。
- `waitForResumeOrStop()`（`:526`–`:546`）：先在独立锁块内清 `m_pauseRequested`（`:529`–`:532`），随后在**锁外** `commitState(State::Paused, pausedJob)`（`:533`），再进入 `m_wake.wait(&m_mutex, ...)` 的谓词循环（`:535`–`:539`）。不存在「加锁提交／无锁通知」两阶段，也不存在「已加锁未通知」中间态。
- `continueRun()`（`:199`–`:207`）在锁内把恢复请求保存为谓词 `m_resumeRequested = true` 并 `m_wake.wakeAll()`；恢复请求由**谓词**而非信号时序承载，因而不存在「提交窗口丢恢复」。
- 现有 `testCyclePauseAndResume`（`tests/test_flowexecutionservice.cpp:606`）与 `testRepeatedPauseWhilePausedDoesNotRePauseCycle`（`:666`）已使用 `Qt::DirectConnection` 连接 `jobStateChanged` 并在槽内调用 `continueRun()`，长期通过。

**动态实证（隔离纪律：独立临时工作树 + 独立构建目录，未进入交付链）**：

| 项 | 值 |
|---|---|
| 独立工作树 | `/var/tmp/t23-probe-wt`（`git worktree add --detach a0ad393`） |
| 独立构建目录 | `/var/tmp/t23-probe-build` |
| 探针 patch 存档 | `/var/tmp/t23-probe-precondition.patch`（81 行，不进交付链） |
| 构建脚本 | `/var/tmp/t23-probe-build.sh` |
| 产物校验 | `ldd` 指向 `/var/tmp/t23-probe-build` 的 `libdeeplux_core.so.1`，非交付 `build/` |
| 用例 | `testT23ProbePauseWindowDirectSignalContinue`、`testT23ProbeDirectSignalStop` |
| 结果 | 各 20 次，合计 **40/40 `rc=0`**、`3 passed, 0 failed`，外部 `timeout -k 5 60` 未触发，`NONZERO_RUNS=0` |
| 汇总日志 | `/var/tmp/t23-probe-out/summary.tsv` |

结论：两个目标性质（暂停窗口恢复不丢失、直接信号回调中恢复／停止不死锁）在当前代码上**成立且无缺陷可复现**。按「无法复现时先核实任务前提，不制造 NF 红灯」的裁定，**未写 NF 红灯、未做修复提交、未在交付工作树切换任何状态**。用户已核对 40/40 汇总并独立运行两个探针，均通过。

### 6.3 固化内容（仅 `tests/test_flowexecutionservice.cpp`）

- 新增 `#include <QCoreApplication>`、`<QEventLoop>`。
- 新增 RAII `T23ContractCleanupGuard`（`:228`–`:255`）：析构时先 `QObject::disconnect(connection)`，再 `service->stop()`，最后有界（5000ms）等待 `state()==Idle`。**任何 `QVERIFY`/`QCOMPARE` 提前返回时**，DirectConnection 槽不再被调用、worker 不再持有本用例栈上 `probe`／原子变量的引用。guard 在被捕获变量之后声明（析构逆序 ⇒ 最先析构）。
- 固化两个行为契约用例（`:773`–`:851`）：
  - `testContinueImmediatelyAfterPauseIsNotLost()`：在 `Paused` 的 DirectConnection 回调里立即 `continueRun()`，断言恢复被接受（`continueResult == 1`）、模块继续执行（`probe.runs` 增长）、`jobFinished(stopped=true)` 到达、`state()==Idle`。
  - `testPauseResumeDoesNotDeadlockUnderDirectSignal()`：在 `Paused` 的 DirectConnection 回调里立即 `stop()`，断言 `jobFinished(stopped=true)` 与 `state()==Idle`。
- 用例注释明确性质：**基线已绿、非 NF、无生产修复提交**；作用为防止未来重构（如 T2.7 等待者拆分）重新引入持锁通知。
- **后续更正（见 7.2）**：评审指出该初版 guard 只等 `state()==Idle`，在「job 已出队、state 仍 Idle」的启动窗口会误判收尾完成（P1）。`1a76293` 已改为以 `!isActive()` + `shutdown()` 成功 join 为依据；T2.4 提交进一步泛化重命名为 `ServiceContractCleanupGuard` 并复用。本节保留初版描述作为历史记录，不作为当前实现依据。

### 6.4 验证记录（2026-11-06，固化后重跑）

脚本 `/var/tmp/t23-gate.sh`，日志目录 `/var/tmp/t23-gate-out/`，汇总 `/var/tmp/t23-gate-out/summary.tsv`。环境沿用 4.1 更正后的固定门禁环境：`unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`。

| 项 | 命令 | 结果 |
|---|---|---|
| 格式门禁 | `clang-format --dry-run --Werror tests/test_flowexecutionservice.cpp` | `FMT_rc=0` |
| 差异检查 | `git diff --check` | `rc=0` |
| 全量构建 | `cmake --build build -j4` | `BUILD_rc=0`；`[100%] Built target DeepLux` |
| 全量回归 | `ctest --test-dir build -j4 --output-on-failure` | `CTEST_rc=0`；`100% tests passed, 0 tests failed out of 96`，`Total Test time (real) = 89.91 sec` |
| 相关套件重复 | `./build/bin/test_flowexecutionservice` × 10（外部 `timeout -k 5 180`） | **10/10 `rc=0`**；每轮 `Totals: 20 passed, 0 failed, 0 skipped, 0 blacklisted` |
| 两个新用例专项 | 各 × 20（外部 `timeout -k 5 60` 单用例调用） | **40/40 `rc=0`**；每份 `Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted`（3 = 隐式 `initTestCase` / `cleanupTestCase` + 该用例）；两用例 `PASS` 行各 20/20 |
| 汇总 | `grep -v 'rc=0' summary.tsv` | `NONZERO_LINES=0`，无输出 |

**独立核验（不依赖脚本汇总）**：对 `/var/tmp/t23-gate-out/ctest-full.log` 得到的原文为 `0 tests failed out of 96`，`grep -ciE 'skipped|Not Run|Disabled|\*\*\*Failed|Errors while'` 为 `0`；10 份 `suite-*.log` 与 40 份 `case-*.log` 全文扫描 `FAIL!` / `failed>0` / `skipped>0` / `Not Run` 均为 `none`；`case-*.log` 共 40 份，每份 `3 passed, 0 failed`。

**统计口径更正**：脚本首版有一行 `CTEST_FAILED=$(grep -c 'tests failed out of' ...)`，字段名会让读者误以为是失败数，实际输出 `1`（即包含该字符串的行数）。已改为 `CTEST_RESULT_LINE=$(grep -o '[0-9]* tests failed out of [0-9]*' ... | tail -1)`，输出 `0 tests failed out of 96`。上述表格引用的是日志原文，不引用该误导字段。

### 6.5 状态

| 项 | 状态 |
|---|---|
| 任务性质 | **基线已绿、非 NF、无生产修复提交**；未做生产重构、未制造 NF 红灯 |
| 交付改动 | 仅 `tests/test_flowexecutionservice.cpp`（+111 行，含 RAII guard 与两个行为契约用例）与本文件第 6 节处置记录；**测试提交 `1798f08`**（`test: add T2.3 pause/resume behavior-contract regression cases`，1 文件 / +111），处置记录随本文件独立提交（hash 见送审汇报与工作日志） |
| 隔离纪律 | 前提核实探针仅在 `/var/tmp/t23-probe-wt` + `/var/tmp/t23-probe-build` 执行，patch 存档不进交付链；交付工作树全程只保留受审的测试增量，未切换任何缺陷态 |
| T2.4 | **方案二已收口**（第 7 节）：三项预期行为已满足，尚未证实计划中的缺陷；固化为 Service 层行为契约，未做生产修复 |
| T2.7 WIP | 继续独立保留于 `be90921`，**未合入**，不作为硬前置 |
| R2 整批 | **未完成** —— 本次仅 T2.3 guard P1 修正与 T2.4 行为契约收口 |

---

## 7. T2.3 guard 收尾 P1 修正与 T2.4 启动握手超时（2026-11-06 评审裁定）

### 7.1 裁定与执行顺序

评审裁定：T2.4 采用**方案二**，固化为 Service 层行为契约，不做生产修复、不制造 NF 红灯；但先补齐 T2.3 收尾 P1。评审用现有启动钩子构造握手超时，独立复现 `stateAfterGuard=Idle` 而 `activeAfterGuard=true` —— guard 已析构，worker 仍有在途任务，被捕获的栈变量不安全。要求：

1. **收尾依据**：以「实际任务结束 `!isActive()`」及必要时 `shutdown()` 成功 join 为准，不能在仍活跃时只等 `state()==Idle` 直接返回；只修测试收尾，不改生产行为。
2. **T2.4 记录口径**：写「**三项预期行为已满足，尚未证实计划中的缺陷**」，不得写成「缺陷前提成立」。
3. **补分支**：当前探针只覆盖「任务已出队后的 stop requested」，须补上「尚未出队的 pending withdrawn」分支。
4. **屏障**：用进入／放行／完成屏障替代固定 600ms 延迟，验证撤回、恢复启动及旧任务不执行。
5. **复用**：复用修正后的安全收尾方式；仅提交测试与处置记录。
6. **顺序**：修正 T2.3 guard 并验证失败路径 → 固化 T2.4 契约 → 重跑相关套件与全量回归。

据此产生 3 个交付提交：`1a76293`（T2.3 guard 修正，测试）、`1769ad6`（T2.4 契约测试，测试）、本文档处置记录提交（hash 见送审汇报与工作日志）。**全程无生产代码改动、无 NF 红灯**。

### 7.2 T2.3 guard 收尾 P1：修正与负验证

**问题**：`1798f08` 的 `T23ContractCleanupGuard` 析构只等 `state()==Idle`。`workerLoop()` 已把 `m_pendingJob` 取走并置 `m_job`（`FlowExecutionService.cpp:292`–`:312`）但尚未 `commitState(Running)`（`:342`）的启动窗口里，`state()` 仍为 Idle 而 `isActive()` 为 true；guard 因此立即结束收尾，栈上 probe／原子变量／DirectConnection 槽可能在 worker 在途时被销毁。

**修正（测试提交 `1a76293`，1 文件 / +48 −13）**：把 guard 改为统一 `finish()`，返回 true 当且仅当「实际任务已结束且 `shutdown()` 已成功 join worker」：

1. `QObject::disconnect(connection)`；
2. `service->stop()`；
3. 有界（10000ms）等待 `!isActive()`，覆盖「已出队但 state 仍 Idle」窗口；
4. 有界（15000ms）循环 `shutdown()` 直到返回 true（`shutdown()` 在任务仍活跃时只请求停止并返回 false；`joinWorkerThread()` 采用 `wait(deadline)` + 必要时无限 `wait()`，不强杀、不 detach，返回 true 即线程已退出并 delete）；
5. 返回 `joined && !service->isActive()`，仍活跃或未能 join **不得静默返回**。

正常路径以 `QVERIFY2(guard.finish(), ...)` 显式断言；析构作为失败路径兜底，失败时 `QTest::qFail`。**提交前核验**：`clang-format --dry-run --Werror` `FMT_rc=0`、`cmake --build build -j4 --target test_flowexecutionservice` `BUILD_rc=0`、`./build/bin/test_flowexecutionservice` `SUITE_rc=0`（`Totals: 20 passed, 0 failed, 0 skipped, 0 blacklisted`）。

**负验证（独立临时工作树，不进交付链）**：

| 项 | 值 |
|---|---|
| 独立工作树 | `/var/tmp/t23-guard-verify-wt`（`git worktree add --detach b3927af`） |
| 独立构建目录 | `/var/tmp/t23-guard-verify-build`（`ldd` 指向该目录 `libdeeplux_core.so.1`） |
| 探针 patch | `/var/tmp/t23-guard-verify-probe.patch`（163 行，不进交付链） |
| 脚本 / 后台任务 | `/var/tmp/t23-guard-verify.sh` / `DYaTRN` |
| 注入内容 | `OldGuardProbe`（只等 Idle 的 P1 缺陷态）、`NewGuardProbe`（等价修正逻辑）、三个探针用例 |
| **A 旧 guard** | **20/20** `GUARD-PROBE variant=old stateAfterGuard=0 activeAfterGuard=1 joined=0` → `OLD_P1_REPRODUCED=20/20`，P1 精确复现 |
| **B 修正后逻辑** | **20/20** `stateAfterGuard=0 activeAfterGuard=0 joined=1` → `NEW_SAFE=20/20` |
| **C 强制断言提前返回** | **20/20** `rc=1`、`FAIL=1`，且 guard 仍输出 `activeAfterGuard=0 joined=1` → `FORCED_FAILURE_SAFE=20/20` |
| 汇总 | `OLD_P1_REPRODUCED=20/20`、`NEW_SAFE=20/20`、`FORCED_FAILURE_SAFE=20/20`、`VERIFY_DONE`；`CONFIGURE_rc=0`、`BUILD_rc=0` |

### 7.3 T2.4 任务性质与静态事实（方案二）

**定性口径（按裁定）**：**三项预期行为已满足，尚未证实计划中的缺陷**。`enqueue()` 超时分支（`FlowExecutionService.cpp:118`–`:132`）`return false` 时未显式置 `m_startSettled=true; m_startOk=false`，二者残留 false/false；但下一次 `enqueue()` 的 `:106`–`:107` 会重置二者，残留值不污染下一次握手。旧 job 迟到的 `settleStart(true)`（`:343`）发生在 `runJob()` 内，此时 `m_job` 已置（`:312`），任何并发 `enqueue()` 会被 `:100` 的 `currentJob()!=None` 拒绝；等旧 job 收尾清零 `m_job` + `commitState(Idle)`（`:397`）后新 `enqueue()` 才可能成功，且它自己会重置握手标志 —— **不存在「迟到 settle 污染下一次握手」的可达窗口**。缺少赋值本身不足以证明缺陷。

超时分支按 `withdrawn = (m_pendingJob == job)` 分两路：`withdrawn=true`（pending 尚未出队）→ `m_pendingJob = None` 直接撤回；`withdrawn=false`（job 已出队）→ `m_stopRequested = true`，旧 job 醒来后 `runSingleJob()` 在 `:413` 首个模块边界前检查 `stopRequested()` 并 `stopped=true` 返回，模块一次都不执行。

**结论**：不做生产修复、不制造 NF 红灯、无生产修复提交；固化为 Service 层行为契约，仅提交测试与本节处置记录。

### 7.4 分支覆盖探针：进入／放行／完成屏障（独立临时工作树）

**难点**：交付链现有的 `setStartHandshakeHookForTest` 在 `runJob()` 内执行（`:314`–`:322`），即 `workerLoop()` 已于 `:292`–`:293` 把 `m_pendingJob` 取走之后；无法在取 job 之前注入屏障，因此**不能确定性构造 `withdrawn=true`**。按「不做生产修复、仅提交测试与处置记录」，交付链不新增生产注入点；该分支改由独立工作树的 **test-only 注入**确定性覆盖。

| 项 | 值 |
|---|---|
| 独立工作树 | `/var/tmp/t24-branch-probe-wt`（`git worktree add --detach 1a76293`） |
| 独立构建目录 | `/var/tmp/t24-branch-probe-build`；`ldd` → `/var/tmp/t24-branch-probe-build/lib/libdeeplux_core.so.1`（非交付 `build/`） |
| 交付链测试补丁 | `/var/tmp/t24-delivery-tests.patch`（276 行，先应用到独立工作树，保证探针复用同一 guard） |
| test-only 注入 patch | `/var/tmp/t24-branch-probe.patch`（538 行，sha256 `d2fd9b878877dfd2…`，**不进交付链**）；注入脚本 `/var/tmp/t24-branch-inject.py` / `-inject2.py` / `-inject3.py` |
| 注入内容 | `FlowExecutionService.{h,cpp}` 新增 `setPreDequeueBarrierForTest` / `clearPreDequeueBarrierForTest` / `runPreDequeueBarrierForTest`，在 `workerLoop()` 循环顶端、取 job 之前、**不持有 `m_mutex`** 时执行；仅独立工作树存在 |
| 运行脚本 / 后台任务 | `/var/tmp/t24-branch-probe.sh` / `vxIEEn`；日志 `/var/tmp/t24-branch-probe-out/` |
| 屏障 | `entered`（进入）/ `release`（放行）/ `completed`（完成）/ `startReturned` 四个 `QSemaphore`，**无 `QThread::msleep(600)` 等固定延迟**；启动放由独立 `std::thread` 执行，guard 统一 join |
| 分支判据 | 用 `Logger::logs("Service")` 中 `pending withdrawn` / `stop requested` 日志计数**增量**做 `QVERIFY2` 断言，机器可核验地证明命中了对应分支，而非仅靠行为推断 |
| 全套（含探针） | `FULL_rc=0`，`Totals: 24 passed, 0 failed, 0 skipped, 0 blacklisted, 2777ms` |

**两个分支各 20 次，合计 40/40 `rc=0`**，`NONZERO_RUNS=0`，外部 `timeout -k 5 60` 未触发；40 份日志独立扫描 `FAIL!` / `failed>0` / `skipped>0` / `Not Run` 均为 0，每份 `Totals: 3 passed, 0 failed`：

- `testT24ProbePendingWithdrawn`（**`withdrawn=true`，pending 尚未出队**）20/20 恒为 `T24-PROBE-A branch=pending_withdrawn startResult=0 hookRuns=1 oldRuns=0 restartRuns=1 state=0 active=0`。进入屏障处断言 `currentJob()==None`、`state()==Idle`（证明 worker 未出队）；`startSingle()` 返回 false 后断言 `!isActive()`（证明 pending 已撤回、无在途 job）与 `pending withdrawn` 日志计数增量；放行后 `oldRuns=0`（旧任务不执行），随后 `startSingle()` 成功、`restartRuns=1`（恢复启动）。
- `testT24ProbeStopRequested`（**`withdrawn=false`，job 已出队**）20/20 恒为 `T24-PROBE-B branch=stop_requested startResult=0 hookRuns=1 oldRuns=0 restartRuns=1 state=0 active=0`。进入屏障处断言 `currentJob()==Single`、`isActive()`（证明已出队）；超时后断言 `stop requested` 日志计数增量；放行后 `jobFinished(stopped=true)`、`oldRuns=0`；清钩子后恢复启动成功。

### 7.5 交付链固化内容（仅 `tests/test_flowexecutionservice.cpp`）

- guard 泛化重命名为 **`ServiceContractCleanupGuard`**，新增 `barrierRelease`（停止前无条件放行进入／放行屏障）、`clearHooks`（清除捕获栈变量的测试钩子，`runJob` 已把钩子复制到局部副本，clear 不影响在途调用）、`starter`（统一 join 启动线程，避免失败路径 `std::thread` 析构触发 `std::terminate`）。`finish()` 的 `!isActive()` + `shutdown()` 成功 join 依据不变。**T2.3 与 T2.4 四个契约用例复用同一收尾**。
- `testStartHandshakeTimeoutStopsDequeuedJobBeforeModulesRun()`：进入屏障断言 `currentJob()==Single` / `state()==Idle` / `isActive()`；放行前 `startReturned` 断言 `startSingle()` 在握手上限返回 false 且不阻塞调用方（`startTimer.elapsed() < 2000`）；完成屏障后断言 `jobFinished(stopped=true)`、`probe.runs==0`（旧任务不执行）、`state()==Idle` / `!isActive()`；清钩子后 `startSingle()` 成功且 `probe.runs==1`（恢复启动）。
- `testHandshakeTimeoutDoesNotLeaveResidualPendingState()`：超时返回 false 后断言第二次 `startSingle()` 被拒（**不留排队 pending**）；旧任务结束后 `currentJob()==None` / `!isActive()`；清钩子后**连续两次**启动均成功、各自完成、`probe.runs` 依次为 1 / 2（超时遗留的 `m_startSettled` / `m_startOk` 不污染后续握手）。
- **覆盖边界（如实记录）**：交付链仅能确定性覆盖 `withdrawn=false`；`withdrawn=true` 需出队前注入点，按裁定未在交付链新增生产注入，由 7.4 独立工作树探针覆盖并存档。
- 首次落地核验：`FMT_rc=0`、`BUILD_rc=0`、`./build/bin/test_flowexecutionservice` → `Totals: 22 passed, 0 failed, 0 skipped, 0 blacklisted`（服务套件由 20 增至 22）。

### 7.6 隔离事实

探针执行期间与执行后独立核对：交付 `HEAD` 恒为 `1a76293`；`git status --short` 仅 `M tests/test_flowexecutionservice.cpp`；`git diff --check` `rc=0`；`git diff --name-only | grep '^src/'` 为**空**（交付链零生产改动）；`git merge-base --is-ancestor be90921 HEAD` 为 **NO**（T2.7 WIP 未合入）。独立工作树 `HEAD=1a76293`，其 `src/` 改动仅为 `FlowExecutionService.{cpp,h}` 的 test-only 注入，**仅存在于该工作树**。

### 7.7 验证记录（最终门禁，2026-11-06）

**执行方式如实记录**：初版脚本 `/var/tmp/t24-final-gate.sh` 以后台任务 `BkMXrt` 启动，日志 `/var/tmp/t24-final-gate-run.log` 显示完成 `FMT_rc=0` / `DIFFCHECK_rc=0` / `BUILD_rc=0` 及 CTest `run-1` 后被会话切换中断（`suite.tsv` 未生成），后台 shell 被回收。**该次中断结果不采信、不计入验收**。改用 `/var/tmp/t24-final-gate2.sh` 分两个**前台批次**重跑完整门禁：`a` 批次 = CTest `run-1`–`run-5`；`b` 批次 = CTest `run-6`–`run-10` + 服务套件 ×10 + 四个契约用例各 ×20。两批次共用同一交付构建树 `build/` 与同一固定环境（`unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`），10 轮连续执行，不重试、不排除测试、不降低比较强度。日志目录 `/var/tmp/t24-final-gate-out/`。

**执行来源更正（2026-11-09 评审 P2）**：`gate2.sh` **不含** `clang-format` / `git diff --check` / `cmake --build` 命令（`grep -n` 验证无匹配），本版此前把「格式/差异/全量构建复核」归入 `a` 批次**表述不符**。这三项的实际来源分两处：① 初版 `/var/tmp/t24-final-gate.sh:19,21,25` 含三项命令，输出在 `/var/tmp/t24-final-gate-run.log`（`FMT_rc=0` / `DIFFCHECK_rc=0` / `BUILD_rc=0`），但该次整体已中断、结果不采信；② 重跑时在调用 `gate2.sh a` **之前**，由外层 Bash 命令单独执行 `clang-format --dry-run --Werror tests/test_flowexecutionservice.cpp`、`git diff --check`、`cmake --build build -j4`（其 stdout 输出 `FMT_rc=0` / `DIFFCHECK_rc=0` / `BUILD_rc=0`，构建日志 `/var/tmp/t24-final-gate-out/build2.log`），这项结果作为本轮格式/构建验收依据。下表「格式门禁 / 差异检查 / 全量构建」三行的来源据此更正；**十轮 CTest 结论不变**（由 `gate2.sh` a/b 两批次产生，日志 `ctest-run-1..10.log`）。

| 项 | 命令 | 结果 |
|---|---|---|
| 格式门禁 | `clang-format --dry-run --Werror tests/test_flowexecutionservice.cpp`（**外层独立命令**，非 `gate2.sh`；见上文来源更正） | `FMT_rc=0` |
| 差异检查 | `git diff --check`（**外层独立命令**，非 `gate2.sh`） | `DIFFCHECK_rc=0` |
| 全量构建 | `cmake --build build -j4`（**外层独立命令**，非 `gate2.sh`；日志 `build2.log`） | `BUILD_rc=0`；`[100%] Built target DeepLux` |
| **全量连续 10 次 CTest** | `ctest --test-dir build -j4 --output-on-failure` × 10 | **10/10 `rc=0`**；每轮 `100% tests passed, 0 tests failed out of 96`，耗时 70.31–80.27s；`GATE10_NONZERO=0` |
| 服务套件重复 | `./build/bin/test_flowexecutionservice` × 10（外部 `timeout -k 5 180`） | **10/10 `rc=0`**；每轮 `Totals: 22 passed, 0 failed, 0 skipped, 0 blacklisted`；`SUITE_NONZERO=0` |
| 四个契约用例专项 | 各 × 20（外部 `timeout -k 5 60` 单用例调用） | **80/80 `rc=0`**；`testContinueImmediatelyAfterPauseIsNotLost` / `testPauseResumeDoesNotDeadlockUnderDirectSignal` / `testStartHandshakeTimeoutStopsDequeuedJobBeforeModulesRun` / `testHandshakeTimeoutDoesNotLeaveResidualPendingState` 各 20/20；`CASES_NONZERO=0` |
| 汇总 | `gate10.tsv` / `suite.tsv` / `cases.tsv` | 10 / 10 / 80 行，非零退出码均为 0 |

**独立核验（不依赖脚本汇总）**：10 份 `ctest-run-*.log` 逐份原文均为 `100% tests passed, 0 tests failed out of 96`；对 `/var/tmp/t24-final-gate-out/*.log` 全文扫描 `FAIL!` / `Not Run` / `Disabled` / `***Failed` / `Errors while` / `skipped>0` / `failed>0` **无匹配**；10 份 `suite-*.log` 每份 `22 passed, 0 failed`；80 份 `case-*.log` 每份 `3 passed, 0 failed`（3 = 隐式 `initTestCase` / `cleanupTestCase` + 该用例）；日志份数 `CTEST_LOGS=10`、`SUITE_LOGS=10`、`CASE_LOGS=80`、`CASE_3PASS=80`。

### 7.8 状态

| 项 | 状态 |
|---|---|
| T2.3 guard P1 | **已修正并负验证**（7.2）：测试提交 `1a76293`；旧 guard 20/20 复现 P1，修正后 20/20 安全，强制断言提前返回 20/20 安全 |
| T2.4 任务性质 | **三项预期行为已满足，尚未证实计划中的缺陷**；方案二固化为 Service 层行为契约，**基线已绿、非 NF、不做生产修复、不制造 NF 红灯** |
| T2.4 分支覆盖 | `pending withdrawn`（尚未出队）与 `stop requested`（已出队）**两分支均由独立工作树探针各 20/20 确定性覆盖**，以进入／放行／完成屏障 + `Logger` 日志计数增量判据；patch 存档不进交付链 |
| 交付链覆盖边界 | 交付链仅覆盖 `stop requested`；`pending withdrawn` 需出队前注入点，按裁定未新增生产注入，由 7.4 探针覆盖并如实记录（7.5） |
| 交付改动 | 仅 `tests/test_flowexecutionservice.cpp`（`1a76293` +48 −13；`1769ad6` +186 −13，guard 泛化 + 两个 T2.4 契约用例）与本文件第 7 节处置记录；**零生产代码改动**；测试与处置记录分提交 |
| 隔离纪律 | 所有 test-only 生产注入与探针仅在 `/var/tmp/t23-guard-verify-wt`、`/var/tmp/t24-branch-probe-wt` + 各自独立构建目录执行；交付工作树全程只保留受审的测试增量与处置记录，未切换任何缺陷态 |
| 最终门禁 | **全绿**（7.7）：全量连续 10 次 CTest 10/10、服务套件 10/10、四个契约用例 80/80；格式与 `git diff --check` 通过 |
| T2.7 WIP | 继续独立保留于 `be90921`，**未合入**（`merge-base --is-ancestor` = NO），不作为硬前置 |
| R2 整批 | **未完成** —— 本次仅 T2.3 guard P1 修正与 T2.4 行为契约收口，不代表整批完成 |

---

## 8. 按 `57f8438` 复核的二次整改（2026-11-09）

### 8.1 复核裁定（如实记录）

评审按 `57f8438` 复核，**仍有 1 项 P1，暂不批准本轮整体收口**。5.7 / 6.5 / 7.8 三处「T2.4 方案二已收口」为整改前的历史记录，**原样保留不改写**：T2.4 行为契约的技术实现已复核通过，但整体收口需在本节 P1 / P2 整改后重新送审。**后续进展见 8.6（送审时状态）与 8.7（2026-11-09 复核批准，当前有效结论）**。

评审同时确认：助手上一轮提出的两点复核说明（交付链未覆盖 `pending withdrawn`；门禁 10 轮为重跑而非 `BkMXrt` 首次 `run-1`）**均可接受**，并给出两点裁定：

1. **接受独立工作树探针对 `pending withdrawn` 的本次核验**：已核对补丁哈希、隔离库路径、两分支各 20 次日志，并独立重跑两探针通过。**但它不是交付链的持续 CI 覆盖**，后续修改相关路径时需重新验证。
2. **接受完整重跑的十轮门禁**：已逐份核对当前全部 96 项、无缺项；不是沿用 `BkMXrt` 的首次 `run-1`；服务套件 10 轮、契约用例 80 次也与原始日志一致。

### 8.2 P1：guard 诊断超时后仍放行栈展开

**问题（评审独立复现）**：`tests/test_flowexecutionservice.cpp:258` 在 `finish()` 开头即 `armed = false`；有界诊断（10s 等 `!isActive()` + 15s 重试 `shutdown()`）超时后 `return joined && !isActive()` 返回 false；析构兜底只 `QTest::qFail()` 后返回。**正常路径的 `QVERIFY2(guard.finish(), ...)` 与失败路径的析构都会让栈继续展开**，而 worker 仍在途、仍引用栈上 probe / 信号量 / 原子变量。评审输出：

```text
ACTUAL-GUARD-DEADLINE finished=0 active=1 armed=0
ACTUAL-GUARD-DESTRUCTOR returned=1 active=1
```

**最小修正（仅 `tests/test_flowexecutionservice.cpp`）**：把不变量从「返回 false 表示失败」改为「**未 join 时绝不返回**」。

- `armed` 仅在「实际任务结束 + `shutdown()` 成功 join」之后清除；此前任何 `QVERIFY`/`QCOMPARE` 提前 return 都会让析构兜底重新进入 `finish()`。
- 有界诊断（10000ms / 15000ms）保留，但**只用于产生 `qWarning` 告警**；超时后进入无界等待循环，持续 `stop()` + `shutdown()` 直到真正 join，由外层 watchdog（CTest `TIMEOUT` / `timeout -k`）限时。
- 硬上限 60000ms 到达仍未 join 时，`qCritical` 输出诊断后 `std::abort()` **直接终止测试进程**，不返回、不栈展开（不依赖 `qFatal` 是否被 Qt Test 消息处理器拦截）。
- 析构改为无条件 `(void)finish();`，不再 `qFail()` 后返回。
- 四个正常路径的 `QVERIFY2(guard.finish(), ...)` 改为显式 `guard.finish();`，因为 `finish()` 现在恒返回 true（否则不返回），恒真断言会掩盖语义。

### 8.3 P1 负验证（独立工作树，不进交付链）

| 项 | 值 |
|---|---|
| 独立工作树 | `/var/tmp/t25-guard-p1-wt`（`git worktree add --detach 57f8438`） |
| 独立构建目录 | `/var/tmp/t25-guard-p1-build`；`ldd` → `/var/tmp/t25-guard-p1-build/lib/libdeeplux_core.so.1` |
| 注入脚本 / patch | `/var/tmp/t25-guard-p1-inject.py`；`/var/tmp/t25-guard-p1-probe.patch`（440 行，sha256 `1e720d12c332be53…`，**不进交付链**） |
| 运行脚本 / 日志 | `/var/tmp/t25-guard-p1.sh`；`/var/tmp/t25-guard-p1-out/`、`summary.tsv` |
| guard 来源 | `OldGuardProbe` / `NewGuardProbe` 均**逐字提取**自 `57f8438` 与整改后的交付源码；仅把三个 deadline 字面量换成运行期可配置 getter（`p1ActiveMs()` / `p1JoinMs()` / `p1HardMs()`），**控制流逐字不变**；另设一组直接实例化 `57f8438` 中**原封不动**的 `ServiceContractCleanupGuard`，用真实 10s/15s 阈值精确复现 |

结果（`CONFIGURE_rc=0`、`BUILD_rc=0`）：

| 组 | 用例 | 次数 | 结果 |
|---|---|---|---|
| A | `testGuardP1OldDeadlineExact`（交付中实际 guard，10s/15s） | 1 | `ACTUAL-GUARD-DEADLINE finished=0 active=1 armed=0 kind=actual_guard_exact_10s_15s` —— **与评审复现逐字一致** |
| B | `testGuardP1OldDeadlineFast`（逐字副本，200/400ms） | 20/20 | 恒为 `finished=0 active=1 armed=0 kind=copy_fast` |
| C | `testGuardP1OldDestructor`（旧 guard 只走析构） | 20/20 | 恒为 `ACTUAL-GUARD-DESTRUCTOR returned=1 active=1`，`rc=1` —— **与评审复现逐字一致** |
| D | `testGuardP1NewFinishWaitsForRealExit` | 20/20 | 恒为 `finished=1 active=0 armed=0 joined=1 diagHit=1 earlyReturn=0`：`diagHit=1` 证明确实进入「诊断超时后继续等待」分支，`earlyReturn=0` 证明**未提前返回**，`joined=1` 证明最终真实 join |
| E | `testGuardP1NewDestructorWaitsForRealExit` | 20/20 | 恒为 `finished=1 active=0 joined=1 diagHit=1 earlyReturn=0`：析构路径同样不提前返回（`armed=-1` 表示该路径不检查已销毁对象的 `armed`） |
| F | `testGuardP1NewAbortsIfWorkerNeverExits` | 20/20 | `rc=134`（`SIGABRT`），日志均含 `bounded diagnostics expired` 与 `terminating the test process`；`P1-ABORT-UNREACHABLE` 出现 **0** 次 —— worker 永不退出时进程被终止，**未发生栈展开** |

汇总：`A_EXACT_PASS=1`、`B_OLD_PASS=20`、`C_OLD_DESTRUCTOR_FAIL=20`、`D_NEW_FINISH_PASS=20`、`E_NEW_DESTRUCTOR_PASS=20`、`F_ABORT_TERMINATED=20`、`F_NO_UNREACHABLE=20`。

### 8.4 P2：门禁执行来源更正

评审指出 7.7 原文把「格式/差异/全量构建复核」归入 `gate2.sh a`，但 `/var/tmp/t24-final-gate2.sh` 不含 `clang-format` / `git diff --check` / `cmake --build`（`grep -n` 无匹配）。已在 7.7 增补「执行来源更正」并在表格三行标注：三项实际来自 ① 初版 `/var/tmp/t24-final-gate.sh:19,21,25`（日志 `/var/tmp/t24-final-gate-run.log`，该次整体中断、不采信）与 ② 重跑时调用 `gate2.sh a` **之前的外层独立 Bash 命令**（`FMT_rc=0` / `DIFFCHECK_rc=0` / `BUILD_rc=0`，构建日志 `build2.log`）。**十轮 CTest 结论不变**，且评审已逐份核对 96 项无缺项。

### 8.5 整改后全量回归（不沿用上一轮结果）

因 `guard` 已改动，重新完整重跑。脚本 `/var/tmp/t26-final-gate.sh`（**格式/差异/构建/CTest/套件/用例全部在本脚本内**，执行来源无歧义，吸取 8.4 教训），分 a / b 两个前台批次，共用同一交付 `build/` 与固定环境（`unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`）。日志 `/var/tmp/t26-final-gate-out/`。

| 项 | 结果 |
|---|---|
| `clang-format --dry-run --Werror` | `FMT_rc=0` |
| `git diff --check` | `DIFFCHECK_rc=0` |
| `cmake --build build -j4` | `BUILD_rc=0`；`[100%] Built target DeepLux` |
| 全量连续 10 次 CTest | **10/10 `rc=0`**；每轮 `100% tests passed, 0 tests failed out of 96`；`GATE10_NONZERO=0` |
| 服务套件 ×10 | **10/10 `rc=0`**；每轮 `Totals: 22 passed, 0 failed, 0 skipped, 0 blacklisted` |
| 四个契约用例各 ×20 | **80/80 `rc=0`**；`CASES_NONZERO=0` |

**独立核验**：10 份 `ctest-run-*.log` 逐份原文均为 `100% tests passed, 0 tests failed out of 96`；对 `/var/tmp/t26-final-gate-out/*.log` 全文扫描 `FAIL!` / `Not Run` / `Disabled` / `***Failed` / `Errors while` / `skipped>0` / `failed>0` **0 个文件命中**；`CTEST=10`、`SUITE=10`（10 份 `22 passed`）、`CASE=80`（80 份 `3 passed, 0 failed`）；`gate10.tsv` / `suite.tsv` / `cases.tsv` 为 10 / 10 / 80 行，非零退出码均为 0。

### 8.6 状态

| 项 | 状态 |
|---|---|
| P1 | **已修正并负验证**（8.2 / 8.3）：交付中实际 guard 精确复现 `finished=0 active=1 armed=0`、析构复现 `returned=1 active=1`；修正后 `finish()` / 析构各 20/20 不提前返回并最终 join，worker 永不退出时 20/20 进程终止、无栈展开 |
| P2 | **已更正来源**（8.4），不否定十轮 CTest |
| 两点裁定 | 均按评审原文记录（8.1）：`pending withdrawn` 探针核验为**本次**接受、**不是持续 CI 覆盖**，后续改动相关路径须重新验证；十轮门禁为完整重跑、非 `BkMXrt` 首次 `run-1` |
| 交付改动 | 仅 `tests/test_flowexecutionservice.cpp` 与本文件第 8 节；**零生产代码改动**；测试与处置记录分提交 |
| 隔离纪律 | 旧/新 guard 探针仅在 `/var/tmp/t25-guard-p1-wt` + 独立构建目录执行；patch 存档不进交付链；交付工作树全程只保留受审的修复态 |
| 门禁 | **整改后全绿**（8.5）：全量连续 10 次 CTest 10/10、服务套件 10/10、契约用例 80/80 |
| **本轮整体收口** | **待评审复核** —— 按裁定原为「暂不批准」，本节整改后重新送审，**不改写为已批准/已收口**（此为送审时历史状态；最终结论见 8.7） |
| T2.7 WIP / R2 整批 | `be90921` 继续独立保留、未合入、不作硬前置；**R2 整批未完成** |

### 8.7 复核批准（2026-11-09，最终结论）

评审复核提交链 `94658ca → 1ab8787`，**通过；P1、P2 均可关闭，未发现新的阻塞项**。8.6 中「待评审复核」为整改后送审时的历史状态，**原样保留不改写**；本节的批准结论为当前有效状态。

**P1 已闭合**：评审独立验证了 `tests/test_flowexecutionservice.cpp:335` 的实际 guard —— 仅在成功 join 后解除保护；显式收尾与析构两条路径超过 15 秒后均等待到 `active=0`、`joined=1` 才返回；缩短阈值的等价探针也验证了无法退出时直接终止进程。

**P2 已闭合**：证据文档已更正执行来源（7.7），新门禁脚本 `/var/tmp/t26-final-gate.sh` 确实包含格式检查、构建及回归命令。

**验证通过**（评审独立实跑与逐项核对）：服务套件 **22/22**；整改后原始日志逐项核对为 **10 × 96/96**、服务套件 **10/10**、契约专项 **80/80**；格式与差异检查通过；交付工作树干净、**零生产代码改动**。

**裁定**：**批准 T2.3 guard 整改及 T2.4 方案二本轮收口**，可以继续 R2 剩余任务。**T2.7 仍保持隔离**（`be90921` 未合入、不作硬前置），**R2 整批尚未完成** —— 本轮批准仅为 T2.3 / T2.4 局部收口，不代表 R2 批次验收门禁（主仓计划文档 `2026-11-05-review-hardening.md:156` 定义的 `git revert` 抽查、整批连续 10 次门禁等）已完成。

R2 剩余任务（依计划文档 `:121`–`:156`，T2.1 / T2.2 已于第 1 / 2 节收口，T2.3 / T2.4 本轮收口）：**T2.5** worker 异常兜底（P2）、**T2.6** 帧身份随信号携带（P1）、**T2.8** `CancellationToken` 语义（P2）、**T2.9** runId 跨线程快照（P2）；**T2.7** 保持隔离不推进。另有 R2 批次级验收门禁（阻塞）待整批完成后执行。起点与顺序待评审指定。

---

## 9. T2.6 帧身份随信号携带（行为契约／回归保护，2026-11-09 评审裁定②）

### 9.1 裁定与任务性质

评审裁定原文：**「裁定：采用②，允许进入『仅测试＋处置记录』的实施，不新增信号参数、不做生产修复，不制造 NF 红灯。当前尚不算 T2.6 收口。」** 起点与顺序按评审指定 `T2.6 → T2.5 → T2.8 → T2.9`；`T2.7`（`be90921`）继续隔离、不作硬前置；涉及共享执行路径，暂不并行修改生产代码。

基线 `e548c10`。任务性质同第 6 / 7 节：**基线已绿、非 NF、无生产修复提交**；只提交测试与本节处置记录，**分两个提交**。原计划 `2026-11-05-review-hardening.md:147` 的「`jobStateChanged` 增加 `jobSerial` 负载」本轮**不实施**；按裁定，「新增信号参数导致基线编译失败」**不算 NF 红灯**。

### 9.2 前提核实：静态事实与隔离探针

**静态枚举**（回传材料 `/var/tmp/t26-probe-out/T26-STATIC-ENUM.md`，不进交付链）：

- 三类 ID 严格区分：`jobSerial`（Service 层单调递增，`currentJobSerial()` / `lastFinishedJobSerial()` / `lastFailedJobSerial()`）、显示 `jobId`（`RunEngine::m_displayJobId`，`FlowExecutionService.cpp:337` 注入、`:376` 清零，帧在 publish 时刻快照进 `DisplayFrame::jobId`）、`runId`（`RunEngine::m_runId`，每轮唯一，帧在 publish 时刻快照进 `DisplayFrame::runId`）。
- `FlowExecutionService.h:91` 的 `jobStateChanged(State, JobType)` **不携带序号**；`MainWindow.cpp:781` 在排队槽执行时回读 `RunEngine::instance().displayJobId()`，**不是 emit 时刻快照**，故与「通知所属序号」存在偏差。
- 帧身份两道校验：`displayFrameReady(viewportId, jobId, runId)` 参数 + `MainWindow.cpp:762` 用 `DisplayFrame::matchesIdentity()` 比对当前槽位帧；`ImageRefreshCoalescer::accepts()` 另有高水位 / runId 过滤，但 `MainWindow.cpp:781` 传空 runId，**合并器的 runId 锁定从未启用**。
- `AgentRunWaiter.cpp` 以 `lastFinishedJobSerial() != expectedSerial` **比较**过滤迟到 `jobFinished/jobFailed`，与 MainWindow 的「回读值直接采用」形成对比。

**隔离探针**（前提核实，**不进交付链**）：独立工作树 `/var/tmp/t26-probe-wt`（`git worktree add --detach e548c10`）+ 独立构建目录 `/var/tmp/t26-probe-build`，`ldd` 指向该目录 `libdeeplux_core.so.1`；脚本 `/var/tmp/t26-probe-build.sh`、`/var/tmp/t26-probe-build-v3.sh`；日志 `/var/tmp/t26-probe-out/`、`/var/tmp/t26-probe-out-v3/`。补丁存档：v1 / v2 依次写入 `/var/tmp/t26-probe.patch`（v1 哈希 `656212b6…` / 613 行，已被覆盖；当前留存 v2 sha256 `fd5c5d81…` / 620 行，差异仅日志字段）；v3 `/var/tmp/t26-probe-v3.patch` sha256 `fd8f5ffb…` / 686 行；首次编译失败现场 `/var/tmp/t26-probe-v3-attempt1.patch`、`console-attempt1.log`、`build-attempt1.log` 原样保留。探针文件 `tests/test_t26_identity_probe.cpp` 及其 `tests/CMakeLists.txt` 注册**仅存在于独立工作树**。

v3 结果：`FULL_rc=0`、`Totals: 5 passed, 0 failed, 0 skipped, 0 blacklisted`、三个用例各 **20/20 `rc=0`**、`NONZERO_RUNS=0`、`PIPESTATUS=0 0`。

**三处评审指出的证据边界（如实记录，已全部修正）**：

1. **A 变体有效帧路径未验证**：初版放行后只泵有限轮事件、guard 随即断连，日志仍为 `finishedNotifies=4 renders=0`，`validDropped=0` **不能证明第 5 个任务的有效帧未被丢弃**。→ v3 补测：把「确定性等待 `barrier.completed == 5` + 消费最终通知直到 `finishedNotifyCount == 5 && renderedFrames` 非空」作为放行后前置条件，并把最后任务渲染身份 / 图像首像素 / run-level 快照 / `lateAccepted=0` / `validDropped=0` **变成断言**。判别日志 `T26-SUMMARY variant=A_at_next_job batch=2 … finishedNotifies=5 renders=1 lateAccepted=0 validDropped=0 completed=5 lastRenderedJob=5`、`T26-RENDER … jobId=5 … pixel0=ff64283c snapshotK=5`；`T26-REPORT.md` §4.4 记为「批评 1 闭合」。
2. **「结构上互斥、必然覆盖后果」过强**：已撤回（`T26-REPORT.md` §4.3、`T26-STATIC-ENUM.md:51`）。`RunEngine.cpp:2574-2578` 的 `m_displayJobId.store()` 与随后加锁的 `clearLatestDisplayFrames()` 是**两步、非原子转换**，中间存在「新 displayJobId 已生效、槽位仍是旧帧」的窗口；A/B **只覆盖两种特定交错，不能推广为所有交错均安全**；该窗口**存在不等于已证实造成显示缺陷**，也不得反向断言它一定安全。
3. **`T26-STATIC-ENUM.md:57` 事实错误「传 0 保留高水位」**：已更正为 `ImageRefreshCoalescer::setExpectedIdentity()` 中 `m_jobId = jobId;`（`:22`）**无条件执行**，传 0 会把高水位清零；`jobChanged` 只控制 `clearPending()`；真正保留高水位的是 `clearExpectedIdentity()`（`:26-30`）。主报告 §4.3 与判别用例描述原本正确，两份材料口径已统一。

**前提核实结论（裁定②的事实基础）**：在已覆盖的两种交错（A：GUI 处理旧通知时回读到「下一任务」的序号；B：回读到 0、高水位归零）下，内部「通知所属序号 ≠ 处理时回读值 / 合并器高水位」的偏差**可复现并记录**（`mismatch` / `hwWrong`），但**未观察到显示错配**：无迟到帧被误接收、无有效帧被误丢弃、最后任务真实渲染且图像与快照对应。**尚未证实计划中的缺陷**。

### 9.3 交付链固化内容（仅 `tests/`，零生产改动）

| 文件 | 改动 | 内容 |
|---|---|---|
| `tests/test_flowidentitycontract.cpp` | 新增 621 行 | 3 个行为契约用例（A / B / 帧身份参数判别） |
| `tests/test_mainwindow.cpp` | +164 行 | 真实 `MainWindow` 连接回归 + `T26RealConnectionGuard` |
| `tests/CMakeLists.txt` | +1 行 | 在 `TEST_SOURCES` 注册 `test_flowidentitycontract` |

预验证补丁 `/var/tmp/t26-contract-tests.patch` sha256 `01e78ca6…` / 828 行；交付实际补丁 `/var/tmp/t26-delivery-tests.patch` sha256 `6bdec72a…` / 835 行。两者差异**仅为 clang-format 排版**：新文件全文件格式化；`test_mainwindow.cpp` 只格式化本次新增行范围（`--lines=65:65 / 370:429 / 502:504 / 4732:4831`），**历史行零改动**（`DELETED_HISTORICAL_LINES=0`）。该文件在 HEAD 的全文件 `clang-format --dry-run --Werror` 本就失败（19747 行历史违规，见 `/var/tmp/t26-mw-baseline-fmt.log`），故按既有门禁口径只检查新增行范围，**未降低既有检查强度**。逻辑与预验证版本一致，交付树门禁为格式化后版本的完整重跑。

**冻结为断言的契约**：

- 连续 5 个短 job + GUI 事件队列延迟（进入／放行／完成屏障 `QSemaphore`，**无固定 sleep**）后，确定性等待 `completed == 5` 并消费全部最终通知，断言 `runNotifyCount == 5`、`finishedNotifyCount == 5`、渲染记录非空；
- 每条渲染记录 `jobId == lastFinishedJobSerial()`、首像素等于第 5 任务期望色、`runId` 非空、至少一条 `snapshotK == 5`；`lateAccepted == 0`、`validDropped == 0`；
- `displayFrameReady` 参数 + `DisplayFrame::matchesIdentity()` 为帧身份事实源：迟到旧通知不调度、不改写已渲染帧（含图像像素与 `moduleDataSnapshot` 断言）。

**明确不固化为永久契约**：当前错配次数 / 高水位错误次数（`mismatch` / `hwWrong` / `identityDrops` 只写日志、不断言固定值）、当前 `jobStateChanged` 信号签名（文件头注释明确 "the current signal signature is never frozen as a contract"）。不新增信号参数、不新增生产注入点。

**真实 MainWindow 连接回归**（`tests/test_mainwindow.cpp::testDelayedQueueThroughRealMainWindowConnections`）：刻意走 `MainWindow.cpp` 自身的 `jobStateChanged` / `displayFrameReady` / `jobFinished` 连接，**不复制槽逻辑**，避免「仅复制槽逻辑后自证」；5 个短 job 期间不泵 GUI 队列，之后等待 `finishedNotifies == 5 && imageWidget->hasImage()`，断言槽位帧 `jobId == lastSerial`、图像首像素与渲染画面一致、旧 `(jobId, runId)` 迟到通知不改写画面。

**收尾 guard 不变量**（沿用第 8 节）：`ContractCleanupGuard` / `T26RealConnectionGuard` 均为「**未 join 时 `finish()` 绝不返回**」——先断连、无条件放行屏障、join 驱动线程、循环 `stop()` + `shutdown()` 直到成功 join；有界诊断超时只 `qWarning` 并继续等待（外层 CTest `TIMEOUT` / `timeout -k` 限时），60s 硬上限 `qCritical` + `std::abort()` 终止进程、不栈展开；析构无条件 `finish()`。

### 9.4 隔离事实

| 项 | 值 |
|---|---|
| 前提核实探针 | `/var/tmp/t26-probe-wt`（detached `e548c10`）+ `/var/tmp/t26-probe-build`；`ldd` 指向独立 `libdeeplux_core.so.1` |
| 探针文件 | `tests/test_t26_identity_probe.cpp` 及其 CMake 注册**仅在独立工作树**，未进交付链 |
| 交付链契约测试预验证 | `/var/tmp/t26-contract-verify.sh`，日志 `/var/tmp/t26-contract-out/`；`CONTRACT_FULL_rc=0`（5 passed）、4 用例各 ×20 = **80/80**、相关套件 `test_imagerefreshcoalescer` 19 passed / `test_flowexecutionservice` 22 passed；脚本初版缺陷（未构建相关套件 target、`grep '1 passed'` 模式不匹配实际 `3 passed`）如实保留于 `console-attempt1.log`，修正后干净重跑 |
| 交付工作树 | 全程只保留受审的测试态，**从未切换缺陷态**；`PROD_DIFF=[]`，零生产代码改动 |
| 门禁 | `/var/tmp/t26-delivery-gate.sh`，**全部检查命令自含**（吸取 8.4 P2 教训）；环境 `unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp` |

### 9.5 验证记录（交付树，2026-11-09）

三次执行全部保留原始日志，**均为全绿，不存在「重试到绿」**：

| 批次 | 定位 | HEAD | 日志目录 |
|---|---|---|---|
| 1 | 落地预验证 | `e548c10` + 未提交的 3 个测试改动 | `/var/tmp/t26-delivery-gate-out/` |
| 2 | 脚本计数口径修正后完整重跑 | 同上 | `/var/tmp/t26-delivery-gate-out-v2/` |
| 3 | **提交态正式送审门禁** | **`42ed898`**（`STATUS=` 干净） | `/var/tmp/t26-delivery-gate-out-final/` |

正式门禁（批次 3）结果：

| 检查 | 结果 |
|---|---|
| `clang-format --dry-run --Werror tests/test_flowidentitycontract.cpp` | `FMT_CONTRACT_rc=0` |
| 新增行范围格式检查（`test_mainwindow.cpp`） | `FMT_MW_ADDED_rc=0`；`DELETED_HISTORICAL_LINES=0` |
| `git diff --check` | `DIFFCHECK_rc=0` |
| `cmake -S . -B build` / `cmake --build build -j4` | `CONFIGURE_rc=0` / `BUILD_rc=0`；`[100%] Built target DeepLux` |
| 相关套件（各一次全量） | `test_flowidentitycontract` **5 passed**、`test_mainwindow` **86 passed / 1 skipped**、`test_imagerefreshcoalescer` **19 passed**、`test_flowexecutionservice` **22 passed**；`SUITES_NONZERO=0` |
| **一次全量 CTest** | **`100% tests passed, 0 tests failed out of 97`**，`CTEST_rc=0`，81.74s |
| 新增用例各 ×20（附加稳定性证据） | 4 × 20 = **80/80 `rc=0`**；`CASE_TOTALS_ALLPASS=80`、`CASES_NONZERO=0` |
| 严格失败关键字扫描 | `FAIL_KEYWORD_STRICT_FILES=0` |

**数字口径说明（如实记录）**：

- 全量 CTest 由基线 96 项变为 **97 项**：`tests/CMakeLists.txt:463` 对 `TEST_SOURCES` 中每个文件自动 `add_test`，新增 `test_flowidentitycontract` 即 +1。三份 `ctest-N.log` / `ctest-full.log` 均为 `Total Tests: 97`。**不得沿用「96/96」表述。**
- 批次 1 的 `CTEST_REGISTERED=88` 是**脚本计数口径缺陷**：CTest 对 `#1`–`#9` 输出 `  Test  #1:`（双空格）、`#10` 起输出 `  Test #10:`（单空格），`grep -c '^  Test #'` 漏计 9 项，实际 `Total Tests: 97`。已修正为读取 `Total Tests:` 行 + 空白容忍模式 `Test *#`，批次 2 / 3 均为 `CTEST_LISTED=97`。该项仅为 FACTS 计数，**不影响任何检查结论**；首次日志原样保留、并完整重跑。
- `FAIL_KEYWORD_FILES=2` 为字面误报，逐行核对：`configure.log:10` 的 `-- HymsonVision3D plugins: Disabled (set ENABLE_HYMSON3D=ON to enable)`，以及 `suite-test_imagerefreshcoalescer.log:14-15` 测试函数名 `testCaptureDisabledPublishesNothing` 含 `Disabled`（`DISABLED_LITERAL_HITS=3`）；**严格扫描 0 命中**。
- `test_mainwindow` 的 `1 skipped` 为 HEAD 基线既有的 `QSKIP`（`testQuickAnnotateNativeRealImageShowsMask` 需 `DEEPLUX_TEST_REAL_SAM_IMAGE` 真实图 GUI 验收），本次新增用例**不含 QSKIP**；CTest 层面 `0 failed`。
- 十轮连续全量门禁**仍留作 R2 整批验收**，本轮按裁定**未执行**；80 次单用例为附加稳定性证据，**不替代、不冒充**十轮门禁。

### 9.6 状态与送审

| 项 | 状态 |
|---|---|
| 交付提交 | ① `42ed898`（`test: add T2.6 frame-identity behavior-contract regressions`，3 文件 / +786，纯测试）② 本节处置记录提交（纯文档，按 `a0ad393` / `e548c10` 先例不重跑门禁） |
| 生产改动 | **零**；无新增信号参数、无 NF 红灯、无独立探针文件进入交付链 |
| 结论措辞 | **「已覆盖时序中未观察到显示错配，尚未证实计划中的缺陷；内部回读偏差已记录。」** |
| **T2.6 收口** | **当前尚不算收口**，待评审收口复审；通过后再进入 `T2.5` |
| T2.7 / R2 整批 | `be90921` 继续隔离、未合入、不作硬前置；**R2 整批未完成** |

**送审复核要点**：① A 放行后确定性等待与最后任务渲染 / 图像 / 快照断言是否充分；② A/B 正确性已全部成为断言、且未固化错配次数或信号签名；③ 真实 `MainWindow` 连接用例确实走生产连接而非复刻槽逻辑；④ 三次门禁日志与提交链一致；⑤ 十轮门禁按裁定留待整批验收、本轮未做是否接受。

### 9.7 P2 整改：真实窗口用例的判别性（2026-11-09 评审 P2，仅改测试）

**评审裁定原文**：真实窗口用例「无法识别身份校验失效」——`test_mainwindow.cpp:4815` 及迟到通知后的断言只比较图像内容；五次任务输出完全相同，即使错误接受迟到通知也只是重新显示最新槽位中的同一张图。评审用临时故障库强制实际 `matchesIdentity()` 始终返回 `true`，该用例仍通过（`actualJob=5 expectedJob=1 runMatches=0 return=true`、`Totals: 3 passed, 0 failed`），复现日志 `/tmp/deeplux-4fe4ede-review.doqBRX/mainwindow-bypass.log`（评审侧提供，本沙箱不可访问）。**这是测试缺口，不是基线产品缺陷的 NF 证据**；最小修正、仅改测试、不新增信号参数、不做生产修复、不制造 NF 红灯。其余审核项（A/B 最终任务/像素/快照断言、三批 97/97 与专项 80/80、真实图 SAM 既有跳过项、十轮门禁留整批）评审已确认通过。

#### 9.7.1 判别性设计（补齐两处，均只在测试内）

1. **五次输出必须不同**：`MainWindowOutputProbeModule` 增加 `distinctImages` 开关与 `runs` 计数；开启时第 k 次 `process()` 输出 `QColor(min(255, 20k), 40, 60)` 并把 `k` 写入 run 级快照 `t26_k`。断言 `runs >= 5`、`lastK > 1`、`expectedPixel != firstPixel`、槽位帧与可见画面首像素均等于 `expectedPixel`、且不等于第 1 个 job 的像素——「画面来自最后任务」成为可判别断言而非日志观察。
2. **迟到通知不得产生渲染事件**：用 `QSignalSpy` 观察真实 `DisplayManager::dataDisplayed`（`renderDisplayFrame()` → `displayImage()` → `emit dataDisplayed`）。记录迟到通知派发前的 `displayedBeforeLate`，派发并泵事件 + `qWait(ThrottleMs*2)` 后断言 `displayedSpy.count() == displayedBeforeLate`。身份校验一旦失效，迟到通知会经 `schedule → renderDisplayFrame → displayImage` 再次 emit，计数变 6 而失败。

**不改变既有行为**：`distinctImages` 默认 `false`；探针的**历史填色行 `image.fill(QColor("#22C55E"))` 原样保留**（该行由 `a902a0d` 引入、早于 T2.6），开启时仅在**其后**覆盖颜色，`T2.6` 之前的历史行**零删除**。

#### 9.7.2 补丁版本与「历史行零改动」的更正（如实记录）

- **v1 补丁**（`/var/tmp/t26-p2-tests.patch`，sha256 `5c7f174e…` / 107 行）在独立工作树三段式已证明判别性（A 5/5 通过、B 0/5、C 5/5，日志 `/var/tmp/t26-p2-out/`）。但落地交付树时暴露两个必须修正的问题：① 它把 `4fe4ede:112` 的历史填色行改成 `if/else`，即**删除了 1 行早于 T2.6 的历史代码**（`PRE_T26_HISTORICAL_DELETIONS=1`），与 9.3 的「历史行零改动」口径冲突；② 新增的链式 `value().value().toInt()` 断行**未通过 clang-format**（`FMT_ADDED_rc=1`）。v1 因此**未提交**，其判别性结论仅作过程记录。
- **v2 补丁**（`/var/tmp/t26-p2-tests-v2.patch`，sha256 `1450f5b6…` / 102 行，`1 file changed, 41 insertions(+), 3 deletions(-)`）：历史填色行保持原样、仅在开启时覆盖；新增行改为一行的 `QVariantMap lastSnapshot` 取快照。`PRE_T26_HISTORICAL_DELETIONS=0`（deleted 的 3 行经核对均为 `42ed898` 自身引入的 T2.6 行，非历史行）；`FMT_ADDED_rc=0`、`git clang-format --diff 4fe4ede` 输出 `clang-format did not modify any files`、`DIFFCHECK_rc=0`。**交付树实际 diff 与该补丁逐字节一致**（`cmp` 返回 0）。

#### 9.7.3 三段式负验证（v2 交付态代码，独立工作树）

隔离事实：工作树 `/var/tmp/t26-p2-v2-wt`（`git worktree add --detach 4fe4ede`）+ 独立构建目录 `/var/tmp/t26-p2-v2-build`；脚本 `/var/tmp/t26-p2-v2-verify.sh`（sha256 `173db427…`）；生产侧旁路补丁 `/var/tmp/t26-p2-inject.patch`（sha256 `8eb8a849…` / 21 行，**仅存在于独立工作树，未进交付链**）；日志 `/var/tmp/t26-p2-v2-out/`。`ldd` 三次构建均指向 `/var/tmp/t26-p2-v2-build/lib/libdeeplux_{core,ui}.so.1`。

| 阶段 | 内容 | 结果 |
|---|---|---|
| A | 旧测试（`4fe4ede`）+ 旁路注入 | **5/5 `rc=0`**（`Totals: 3 passed`）——独立复现评审指出的测试缺口 |
| B | v2 测试 + 旁路注入 | **0/5**（5/5 `rc=1`、`Totals: 2 passed, 1 failed`）——`displayedSpy.count(): 6` vs `displayedBeforeLate: 5`，`Loc: /var/tmp/t26-p2-v2-wt/tests/test_mainwindow.cpp(4861)` |
| C | v2 测试 + 恢复生产 | **5/5 `rc=0`**（`Totals: 3 passed`），`bypass=0` |

汇总：`A_OLD_BYPASS_RC0=5`、`B_V2_BYPASS_NONZERO=5`、`C_V2_RESTORED_RC0=5`、`A_BYPASS_LOG_LINES=55`、`B_BYPASS_LOG_LINES=55`、`B_FAIL_ASSERT_LINES=5`、`C_BYPASS_LOG_LINES=0`；`WT_STATUS_AFTER=[ M tests/test_mainwindow.cpp]`（旁路已 `git checkout` 恢复，工作树只剩测试改动），`DELIVERY_STATUS_AFTER=[ M tests/test_mainwindow.cpp]`——**交付工作树全程未切换缺陷态，生产侧零改动**。

#### 9.7.4 交付门禁（提交态，2026-11-10）

**提交**：`37903c9`（`test: make T2.6 real-MainWindow regression discriminate an identity bypass`，1 文件 / +41 −3，纯测试）；`STATUS=[]`、`PROD_DIFF=[]`、`HEAD_FILES=[tests/test_mainwindow.cpp]`。

**门禁脚本与执行来源（自含全部命令）**：`/var/tmp/t26-p2-delivery-gate-v2.sh`（sha256 `9c4e1486…`），日志 `/var/tmp/t26-p2-gate-out/`，环境 `unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`。

| 检查 | 结果 |
|---|---|
| `clang-format --dry-run --Werror tests/test_flowidentitycontract.cpp` | `FMT_CONTRACT_rc=0` |
| 新增行范围格式检查（`test_mainwindow.cpp`） | `FMT_MW_ADDED_rc=0`；`GIT_CLANG_FORMAT_DIFF=[clang-format did not modify any files]` |
| `git diff --check` | `DIFFCHECK_rc=0` |
| 配置（全新工作区外构建目录 `/var/tmp/t26-p2-gate-build`） | `CONFIGURE_rc=0` |
| 全量构建 | `BUILD_rc=0`；`[100%] Built target DeepLux` |
| 相关套件（各一次全量） | `test_flowidentitycontract` **5 passed**、`test_mainwindow` **86 passed / 1 skipped**、`test_imagerefreshcoalescer` **19 passed**、`test_flowexecutionservice` **22 passed**；`SUITES_NONZERO=0` |
| **一次全量 CTest** | **`100% tests passed, 0 tests failed out of 97`**，`CTEST_rc=0`；`CTEST_TOTAL=Total Tests: 97`、`CTEST_LISTED=97` |
| 四个用例各 ×20 | **80/80 `rc=0`**；`CASE_TOTALS_ALLPASS=80`、`CASES_NONZERO=0` |
| 严格失败关键字扫描 | `FAIL_KEYWORD_STRICT_FILES=0` |
| `test_mainwindow` 既有跳过 | `MW_SKIP_LINES=1`（`testQuickAnnotateNativeRealImageShowsMask` 需真实图环境，基线既有；新增用例不含 QSKIP） |

**门禁 attempt 1 的失败与处置（如实记录，不作为验收依据）**：首次门禁在交付树内 `build/` 增量目录上运行，CMake **Generate** 阶段报 `Cannot open file for write: …/build/tests/CMakeFiles/test_flowexecutionservice_autogen_timestamp_deps.dir/progress.make.tmpa8769`、`: System Error: Inappropriate ioctl for device`，`CONFIGURE_rc=1`（而 `BUILD_rc=0`、97/97 仍通过）。该次日志保全于 `/var/tmp/t26-p2-gate-attempt1/`。为**不携带未解释的非零退出码送审**，正式门禁改为工作区外**全新构建目录**（`/var/tmp/t26-p2-gate-build`）完整重跑，Generate 阶段无陈旧状态，`CONFIGURE_rc=0`。另：清理 attempt 1 的 `build/` 时，一个 `mv` 操作被沙箱安全策略中断，该目录（git 忽略的构建产物）处于部分迁移状态；**仓库工作树状态不受影响（`STATUS=[]`）**，正式门禁不使用该目录。

#### 9.7.5 状态

| 项 | 状态 |
|---|---|
| 改动性质 | **纯测试**（1 文件 / +41 −3）；零生产改动、无新增信号参数、无 NF 红灯、不新增生产注入点 |
| 结论措辞 | **「已覆盖时序中未观察到显示错配，尚未证实计划中的缺陷；内部回读偏差已记录。」**（本轮 P2 只补测试判别性，不改变该结论） |
| **T2.6 收口** | **仍不算收口**，待评审对本 P2 整改的收口复审；通过后再进入 `T2.5` |
| 十轮门禁 | 按裁定**仍留作 R2 整批验收**，本轮未执行；80 次单用例为附加稳定性证据，不冒充十轮门禁 |
| T2.7 / R2 整批 | `be90921` 继续隔离、未合入、不作硬前置；**R2 整批未完成** |

**本轮送审要点**：① 五次不同像素 + `t26_k` 快照是否足以判别「画面来自最后任务」；② `QSignalSpy(DisplayManager::dataDisplayed)` 的「无额外渲染」断言是否使身份校验失效必然失败（三段式 A/B/C 已给出 5/5、0/5、5/5）；③ 历史填色行保持原样、`PRE_T26_HISTORICAL_DELETIONS=0` 是否满足「历史行零改动」；④ 正式门禁在全新构建目录重跑、attempt 1 的 `CONFIGURE_rc=1` 仅作现场保留是否可接受。

### 9.8 复核批准（2026-11-10，T2.6 收口）

评审复核 `37903c9 → 221ec3f`：**通过。P2 判别性缺口已闭合，未发现新的阻塞项，批准 T2.6 按「行为契约／回归保护」口径收口。**

评审独立验证结果（原样记录）：

- 正常用例通过；**强制绕过实际身份校验后，在渲染计数断言（`/home/charles/Data/Repo/DeepLux-hardening/tests/test_mainwindow.cpp:4861`）处以 `6 != 5` 失败；取消旁路后再次通过**。
- 契约套件实跑 **5/5**。
- 正式门禁原始日志核对为 **97/97 CTest、专项 80/80**；真实图 SAM 的既有跳过项记录准确。
- **接受使用全新构建目录重跑**；失败尝试保留但不计入验收。
- 交付仍为**纯测试与文档改动，工作树干净**。

| 项 | 结论 |
|---|---|
| 提交链 | `… → 42ed898 → 4fe4ede → 37903c9 → 221ec3f`（test / docs 分提交） |
| 改动性质 | 纯测试 + 文档；零生产改动、无新增信号参数、无 NF 红灯 |
| 结论措辞（沿用） | **「已覆盖时序中未观察到显示错配，尚未证实计划中的缺陷；内部回读偏差已记录。」** |
| **T2.6** | **已按行为契约／回归保护口径收口**（局部收口，不等于 R2 整批完成） |
| 下一步 | 进入 **T2.5 前提核实**；后续顺序 `T2.5 → T2.8 → T2.9`，**T2.7 继续隔离**、`be90921` 未合入、不作硬前置 |
| 整批 | **R2 整批尚未完成**；十轮连续门禁与批次验收门禁待整批完成后执行 |

---

## 10. T2.5 worker 异常兜底（真实缺陷 NF，进行中）

### 10.1 证据口径更正（2026-11-10 评审裁定，优先于一切推理）

- **A/B/D 直连槽探针不作正式红灯，仅作诊断记录。** Qt 5.15 明确规定：经信号槽机制调用的槽，若让异常逸出槽边界属**未定义行为**；`Qt::DirectConnection` 不等于普通 C++ 函数调用。
- **「生产可达性」推论撤回。** `MainWindow` 等 worker→GUI 为**排队连接**，GUI 槽在 GUI 线程执行，其异常（本属 UB）不会返回 worker 线程；据此推断「任何直连槽抛异常即整进程终止」不成立。
- **正式红灯口径**：仅限 **Service 自有代码的普通 C++ 异常**；启动段复用既有测试钩子 `setStartHandshakeHookForTest()`（`std::function<void()>`，由 `runJob()` 直接调用，不经过任何 Qt 槽边界）。
- **修复承诺边界**：统一恢复启动、主体、收尾的资源与状态；**不承诺**从任意 Qt 槽异常中恢复。

被撤回的原始推断与探针数据保留在 `/var/tmp/t25-probe-out/T25-REPORT.md`、`T25-STATIC-ENUM.md`（含「撤回」标注），不因后续改写而删除。

### 10.2 静态事实（基线 `1a2a02a`）

| 段落 | 行号 | 是否在 try 内 | 说明 |
|---|---|---|---|
| 启动段 | `:306-343` | **否** | `startHook()`、`beginExternalJob()`、`setDisplayJobId()`、`commitState(Running)` |
| 主体 | `:348-371` | 是 | 仅此段有 catch |
| 收尾段 | `:373-408` | **否** | `endExternalJob()`、`clearDisplayJobId()`、状态复位、`emit jobFinished/jobFailed` |
| 线程入口 | `:18-23` | **无 catch** | `WorkerThread::run()` 直接调用 `workerLoop()` |
| 命令循环 | `:281-304` | **无 catch** | `workerLoop()` 内 `runJob(job)` |

全文件 `catch` 仅 `:365`、`:368` 两处（均在主体 try 内）。主体异常另有 `RunEngine::executeAcquiredRun()`（`RunEngine.cpp:1455-1467`）兜底，故主体有双保险、启动/收尾零保险。

### 10.3 正式红灯（提交 `39b8c37` 单独检出）

注入：`setStartHandshakeHookForTest([]{ throw std::runtime_error("injected startup callback fault"); })`。

隔离事实：工作树 `/var/tmp/t25-probe-wt`（detached `1a2a02a`），以 `git checkout 39b8c37 -- tests/test_flowexecutionservice.cpp src/core/engine/FlowExecutionService.{h,cpp}` 取用**提交内字节**；哈希核对 `tests/test_flowexecutionservice.cpp = d353639b…`、`src/core/engine/FlowExecutionService.cpp = f2c66808…`（与 `git show 1a2a02a:…` 一致）。构建目录 `/var/tmp/t25-probe-build`。

```
run-1 rc=134 terminate=1 pass=0
run-2 rc=134 terminate=1 pass=0
run-3 rc=134 terminate=1 pass=0
run-4 rc=134 terminate=1 pass=0
run-5 rc=134 terminate=1 pass=0
```

原始日志 `/var/tmp/t25-probe-out/commit1_red-run-{1..5}.log`：

```
terminate called after throwing an instance of 'std::runtime_error'
  what():  injected startup callback fault
```

另有独立探针（不依赖 QtTest）`/var/tmp/t25-startup-red-out/startup_probe` 在基线库上同样 **5/5 `rc=134`**，见 `T25-STARTUP-RED.md`。

### 10.4 修复内容（提交 `324a282`）

- 启动段纳入 `try`：异常时立即 `settleStart(false)`，不让 `startX()` 等到握手超时后按「已出队」请求停止。
- 主体保持既有错误文本；启动/收尾分别使用可判别的 `service_startup_exception` / `service_teardown_exception` 前缀，拒绝分支保留原拒绝原因并追加故障。
- 收尾段纳入 `try`；对租约释放与显示 jobId 清理做尽力二次尝试；**资源与状态复位无条件执行**（atomic store + 锁，不调用可抛接口）。
- `WorkerThread::run()` 外层 `catch(...)` → `handleWorkerLoopFault()`：复位 `Idle`、放行启动握手、上报 `jobFailed`，绝不让异常逃出线程入口；退出屏障钩子亦包一层。
- 非承诺：`beginExternalJob()` 自身抛出时无 token 可释放，引擎半初始化状态不承诺完全复位；槽异常不在承诺范围。

### 10.5 绿灯证据（提交 `324a282`）

| 套件 | 结果 |
|---|---|
| `test_flowexecutionservice`（含新增用例） | **23 passed, 0 failed** |
| 新增用例单独 ×5 | **5/5 `rc=0`，PASS** |
| `test_runengine` | **118 passed, 0 failed** |
| `test_runenginejoblease` | **8 passed, 0 failed** |
| `test_runengineuserpause` | **6 passed, 0 failed** |
| `test_runenginefinalizer` | **6 passed, 0 failed** |

日志：`/var/tmp/t25-probe-out/tip_*.log`、`commit2_green-run-{1..5}.log`。

### 10.6 构建环境如实记录

- `cmake -G "Unix Makefiles"` 与 `-G Ninja` 在**全新构建目录**的 Generate 阶段均出现停滞（`Configuring done` 后长时间无 `Generating done`，实测约 1 文件/秒），本机 load average ≈ 9。为避免把环境问题混入测试结论，本轮绿灯改用**基线已生成的构建目录** `/var/tmp/t25-probe-build` 的增量目标（`make <target>/fast`，只做编译/链接，不重跑 Generate）。
- 该偏离**不满足**正式门禁「工作区外全新构建目录」的要求，故**不作为 T2.5 收口门禁**；收口时须在能完成 Generate 的环境中重跑全新目录的 97 项 CTest。
- `make` 首次报 `No space left on device`（`/tmp` 为 10M tmpfs）与插件目标缺失，均为环境/目标选择问题，设置 `TMPDIR=/var/tmp/deeplux-ctest-tmp` 后重跑通过；**不是测试失败、不是重试到绿**。
- **后续**：本轮收尾时已在全新目录 `/var/tmp/t25-close-build` 完成 `CONFIGURE_rc=0` / `BUILD_rc=0` / 97 项 CTest 全通过（见 10.12）；上面的增量构建仅用于快速迭代，正式门禁以上述全新目录结果为准。

### 10.7 收尾段注入点方案（待核准，未实施）

收尾段无任何既有合法注入位置（启动钩子只在启动段调用；`setExecutionFaultInjectionForTest` 属引擎主体；`setWorkerExitBarrierForTest` 属线程退出段；`Logger` 无 sink）。方案 A：新增与既有钩子同型的 `setJobTeardownHookForTest()` / `clearJobTeardownHookForTest()`（`DEEPLUX_TEST_VISIBILITY`，生产构建为 `private`、恒为空），调用点置于收尾 `try` 入口、`endExternalJob()` 之前。详见 `/var/tmp/t25-probe-out/T25-TEARDOWN-HOOK-PROPOSAL.md`。

### 10.7.1 方案 A 核准与落实约束（2026-11-10）

评审核准方案 A，附五条约束，逐条落实如下：

| 约束 | 落实 |
|---|---|
| 普通 C++ 回调；锁内快照、锁外调用；收尾 `try` 内、`endExternalJob()` 前；每 job 只调一次，恢复/重试路径不得再触发 | 钩子为 `std::function<void()>`；`{ QMutexLocker locker(&m_mutex); teardownHook = g_jobTeardownHookForTest; }` 快照后锁外调用；调用点是收尾 `try` 第一句；两个 `catch` 分支只调用 `bestEffortTeardownRelease()`，不再触碰钩子 |
| 保持类布局不变；钩子存储放 `.cpp` 私有测试状态，沿用现有互斥锁；`DEEPLUX_TEST_VISIBILITY` 只控访问权限 | 存储为 `.cpp` 匿名命名空间中的 `g_jobTeardownHookForTest`，由 `m_mutex` 保护；**未新增任何类数据成员**；头文件只加两个成员函数声明（非虚，vtable/布局不变） |
| 覆盖标准与非标准异常；`catch(...)` 分支同样要释放租约 | 新增两个用例（`std::runtime_error` 与 `throw 42`）；`catch(...)` 补齐 `bestEffortTeardownRelease()`（提交 `70c6490`） |
| 断言真实恢复，不以 `state==Idle` 替代资源释放 | 钩子内记录「租约仍持有 / Service 仍 Running / 显示 jobId 未清零」；异常后断言 `jobFailed` 恰好一次、`jobFinished` 零次、`!isActive()`、`!engine.isBusy()`、`displayJobId()==0`，清钩子后下一任务完成 |
| 保留可编译的 NF 证据链；钩子与测试单独提交；撤销修复时保留钩子与用例 | 钩子+用例为 `732640c`，修复为 `70c6490`；撤销复败在独立工作树 `git revert --no-edit 70c6490`，钩子与用例保留、编译通过 |

### 10.8 收尾段红灯（提交 `732640c` 单独检出，可编译）

工作树 `/var/tmp/t25-probe-wt` 取 `732640c` 的三份文件；构建目录 `/var/tmp/t25-probe-build`；每个用例独立进程运行 5 次。

| 用例 | 结果 |
|---|---|
| `testExceptionInJobTeardownEmitsJobFailedNotTerminate`（`std::runtime_error`） | **5/5 `rc=0` PASS**（既有 `catch` 分支已重试释放） |
| `testNonStandardExceptionInJobTeardownEmitsJobFailedNotTerminate`（`throw 42`） | **5/5 `rc=1` FAIL**，`'!engine.isBusy()' returned FALSE`，`Loc: tests/test_flowexecutionservice.cpp(397)` |

失败断言原文：

```
FAIL!  : TestFlowExecutionService::testNonStandardExceptionInJobTeardownEmitsJobFailedNotTerminate()
         '!engine.isBusy()' returned FALSE. (the engine lease must be released after teardown recovery)
   Loc: [/var/tmp/t25-probe-wt/tests/test_flowexecutionservice.cpp(397)]
```

**是可编译的测试失败，不是编译失败**：同一提交上标准异常用例正常通过。

### 10.9 修复与绿灯（提交 `70c6490`）

`catch(...)` 与 `catch (const std::exception&)` 走同一条尽力释放路径（`endExternalJob` + `clearDisplayJobId`，各自独立 `try` 包裹）。

| 套件/用例 | 结果 |
|---|---|
| 两个收尾用例各 ×5 | **10/10 `rc=0` PASS** |
| `test_flowexecutionservice`（全套） | **25 passed, 0 failed** |
| `test_runengine` | **118 passed, 0 failed** |
| `test_runenginejoblease` / `userpause` / `finalizer` | **8 / 6 / 6，全过** |

### 10.10 撤销修复复败（独立工作树，保留钩子与用例）

- 工作树 `/var/tmp/t25-revert-wt`（`git worktree add --detach 70c6490`），执行 `git revert --no-commit 70c6490`，**仅撤销修复、钩子与两个用例保留**；`git status` 显示 `M src/core/engine/FlowExecutionService.cpp`（单文件），编译 `MAKE_rc=0`。
- 结果：`std::runtime_error` 用例 **3/3 `rc=0`**；非 `std::exception` 用例 **3/3 `rc=1`**，均失败于 `!engine.isBusy()`。
- 复败是**可编译的测试失败**，不是编译失败。

### 10.11 提交顺序声明（不得追溯改写）

- `324a282` 的验证发生在收尾钩子存在之前：当时只有启动段用例（`39b8c37` 红灯 5/5、`324a282` 绿灯 5/5 + 相关套件）。**不得表述为「用收尾钩子验证过 `324a282`」。**
- 收尾段证据链只对应 `732640c`（红灯）→ `70c6490`（绿灯）→ `revert 70c6490`（复败）。
- 本次核准**不代表** `324a282` 的整体实现已通过审核；其启动段/主体/收尾结构仍待本轮收口复审一并裁定。


### 10.12 正式门禁（全新工作区外构建目录，2026-11-10）

| 项 | 值 |
|---|---|
| 交付树 | `HEAD=70c6490`、`STATUS=[]`（门禁时点；其后仅 `572ea80` 文档提交，不改变任何二进制） |
| 构建目录 | `/var/tmp/t25-close-build`（**工作区外全新目录**） |
| 配置 | `CONFIGURE_rc=0`（选项集与既有隔离构建一致；耗时 8m11s，Generate 较慢） |
| 构建 | `BUILD_rc=0`、`[100%] Built target DeepLux` |
| 格式 | `FMT_CPP_rc=0`、`FMT_H_rc=0`、`FMT_TEST_rc=0`、`GIT_CLANG_FORMAT_DIFF=[clang-format did not modify any files]`、`DIFFCHECK_rc=0` |
| CTest 列举 | `Total Tests: 97`、`CTEST_LISTED=97` |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time = 201.48 sec`） |
| 相关套件（同一交付态，独立运行） | service **25/25**、runengine **118/118**、joblease 8/8、userpause 6/6、finalizer 6/6 |

日志：`/var/tmp/t25-close-out/{configure,build,ctest,ctest-N}.log`、`tip_*.log`。

### 10.13 状态

| 项 | 状态 |
|---|---|
| 提交链 | `… → 1a2a02a → 39b8c37 → 324a282 → f556dfb → 732640c → 70c6490` |
| 启动段 NF | **红灯 5/5、绿灯 5/5 已闭合**（对应 `39b8c37 → 324a282`） |
| 收尾段 NF | **红灯 5/5（std 通过 / 非 std 失败）、绿灯 5/5、撤销复败 3/3 已闭合**（对应 `732640c → 70c6490`） |
| 全量 CTest（97） | **已执行：97/97 通过**（全新目录） |
| T2.5 | **待评审收口裁定**（本轮只提交证据，不自行宣告收口） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成** |

## 11. T2.5 P1 整改：线程入口兜底后必须能再次执行任务（2026-11-10 评审裁定）

### 11.1 裁定的问题

按 `b52bcc9` 复核，评审给出 1 项 P1，暂不批准 T2.5 整体收口：

> 线程入口兜底后，Service 无法再次执行任务。线程入口捕获异常后退出 worker；
> 故障收尾将状态置为 Idle，却未处理残留 pending 命令和退出线程的生命周期。
> `ensureThreadLocked()` 又仅检查 `m_thread` 是否非空，不会重建已退出的线程。

评审侧用普通 C++ 故障注入 + 现有退出屏障 3/3 复现：故障后 `Idle, active=1,
engineBusy=0, 重试失败`；仅 `stop` 后仍失败（握手 100ms）；显式 `stop+shutdown`
后重新启动成功。并明确两条禁止的偷懒路径：**只清 pending**、**提前清空线程指针绕过 join**。

### 11.2 静态事实（基线 `b52bcc9`）

| 事实 | 位置 | 结论 |
|---|---|---|
| `handleWorkerLoopFault()` 中 `m_pendingJob` 出现次数 | `FlowExecutionService.cpp` | **0 次**——完全没有清理残留命令 |
| `ensureThreadLocked()` 判定 | `FlowExecutionService.cpp` | 仅 `if (m_thread) { return; }`，不判断线程是否已结束 |
| `enqueue()` 拒绝条件 | `FlowExecutionService.cpp` | `state()!=Idle \|\| m_pendingJob!=None \|\| currentJob()!=None` |
| 生产代码调用 `FlowExecutionService::shutdown()` 的点 | `src/` 全量 grep | **0 处**（只有测试调用） |

最后一条决定了「无需调用者手动 shutdown 即可重新执行」是**生产语义要求**，不是测试便利。

### 11.3 注入点方案核准（方案 A）

`workerLoop()` 骨架自身（取队列/加锁）抛出，在生产代码中没有合法注入位置：
既有 `setStartHandshakeHookForTest()` 在 `runJob()` 内（只覆盖 job 级异常），
`setWorkerExitBarrierForTest()` 在 `workerLoop()` 返回之后（且其异常被 `run()` 显式吞掉）。
按纪律先提方案、经裁定后实施：

- **方案 A（核准）**：新增 `setWorkerLoopFaultHookForTest()`，钩子存储在 `.cpp` 匿名命名空间
  （`g_workerLoopFaultHookForTest`），由既有 `m_mutex` 保护，**不新增类数据成员**，
  类布局/ABI 不变；在 `workerLoop()` 每轮循环入口、命令已出队而 `runJob()` 尚未进入处执行。
- 方案 B（复用退出屏障）与方案 C（只做行为契约、不造 NF 红灯）经评估未采纳。

### 11.4 正式红灯（提交 `fc28456` 单独检出）

隔离工作树 `/var/tmp/t25-p1-probe-wt`（detached `fc28456`）+ 独立构建目录
`/var/tmp/t25-p1-build`（`CONFIGURE_rc=0`，`BUILD_rc=0`）。

原始 5 次运行（未加诊断代码）：

| 运行 | 退出码 | 结果 |
|---|---|---|
| red-1..5 | **`rc=1`（5/5）** | `Totals: 2 passed, 1 failed`，`Total 15114–15138ms` |

失败断言（`tests/test_flowexecutionservice.cpp:1030`）：

```text
FAIL!  : testServiceRestartsWithoutManualShutdownAfterWorkerFault()
   Actual   ((failedSpy.count())): 0
   Expected (1)                  : 1
```

这是**可编译的测试失败**，不是编译失败，也不是崩溃。

### 11.5 红灯现场诊断（隔离工作树临时插桩，用完即恢复）

在隔离工作树临时插入 `qInfo` 诊断块（`diag-2.log`），恢复后 `grep -c "P1-DIAG"` = 0：

```text
[P1-DIAG]         failed=0 finished=0 state=0(Idle) currentJob=0(None)
                  isActive=0 engineBusy=0 displayJobId=0 hookRuns=1
[P1-DIAG-200ms]   state=0 isActive=0 engineBusy=0
[P1-DIAG-RESTART] startSingle=0  state=0 isActive=0 engineBusy=0
[P1-DIAG-LATER]   probeRuns=0 finished=0 failed=0
```

诊断确认了**两个独立缺陷**：

1. **主缺陷**：`startSingle=0` 且 `probeRuns=0`——重启被拒绝、任务从未执行；
   单次耗时 ~15s 与握手超时后按「已出队」撤回一致。
2. **次缺陷**：`failed=0 finished=0`——`jobFailed` 根本没有发出。原因是 `m_job` 在
   `runJob()` 内才置位，而钩子位于「已出队、`runJob()` 未进入」窗口，`handler` 读到的
   `m_job` 仍是 `None`，于是按「没有 job」处理、不上报终态。调用方拿到
   `startX()==false` 却得不到任何终态信号。

**口径说明**：本节诊断输出中 `isActive=0`，与评审现场 `active=1` 不同。差异来自注入点位置
（本钩子在 `m_pendingJob` 已清空、`m_job` 尚未置位的窗口内），但重启失败与任务未执行这两项
**主症状完全一致**。该差异不改变缺陷性质，据实记录。

### 11.6 修复内容（提交 `9bd3aad`）

| # | 根因 | 修复 |
|---|---|---|
| 1 | `ensureThreadLocked()` 只判 `m_thread` 非空，已结束的线程被复用，新命令进入死线程队列后只能等握手超时 | `enqueue()` 先「摘指针 → **锁外** join 回收 → 锁内重建」。判定用 `isFinished()` 而非 `!isRunning()`（Qt 保证 `run()` 返回后 `isFinished()` 才为 true，此后绝不再消费命令）。回收在锁外：`join` 会阻塞到线程真正退出，持锁 join 会卡死所有服务入口。**不提前清空指针绕过 join**：旧线程仍由 `joinWorkerThread()` 等待真正退出后 `delete`，不 detach、不 terminate |
| 2 | `handleWorkerLoopFault()` 不清理 `m_pendingJob` | handler 内 `m_pendingJob = JobType::None`，就地丢弃出错命令 |
| 3 | `m_job` 在 `runJob()` 内才置位，出队→`runJob` 窗口的故障漏报终态 | `workerLoop(JobType* faultyJob)` 在出队时记录当前命令，`handleWorkerLoopFault(faultyJob, what)` 以此为准（`faultyJob` 为空时回落到 `m_job`）。参数带默认值，不改变既有调用点 |

类布局核对：`git diff fc28456 -- src/core/engine/FlowExecutionService.h` 只显示两个方法签名
（`workerLoop(JobType* = nullptr)`、`handleWorkerLoopFault(JobType, const QString&)`），
**零数据成员新增**。

清零边界（逐轮重置，避免误报）：

```cpp
for (;;) {
    JobType job = JobType::None;
    if (faultyJob) { *faultyJob = JobType::None; }   // 每轮重置
    ... 等待 / 出队 ...
    if (faultyJob) { *faultyJob = job; }             // 出队成功后才置位
```

- 阻塞在 `m_wake.wait()` 时抛出 → `faultyJob == None`，不误报；
- 出队后 `runJob()` 前抛出 → `faultyJob == job`，正确上报；
- `runJob()` 内部抛出 → 由其自身 try 收敛，不会到达 handler。

### 11.7 绿灯证据（提交 `9bd3aad`）

隔离工作树同步修复后 `BUILD_rc=0`：

| 运行 | 退出码 | 结果 |
|---|---|---|
| green-1..5 | **`rc=0`（5/5）** | `Totals: 3 passed, 0 failed`，`53–54ms` |
| 全套件 | **`SUITE_rc=0`** | `Totals: 26 passed, 0 failed`（用例数由 25 → **26**） |

耗时从 15114–15138ms 降到 53–54ms，说明「握手超时撤回」分支已不再被触及。

### 11.8 撤销修复复败（独立工作树，保留钩子与用例）

独立工作树 `/var/tmp/t25-p1-revert-wt`（detached `9bd3aad`）执行
`git revert --no-commit 9bd3aad`，只回退两个生产文件：

- 钩子保留：`grep -c "setWorkerLoopFaultHookForTest\|clearWorkerLoopFaultHookForTest\|g_workerLoopFaultHookForTest"` → `.cpp` **6**、`.h` **2**；
- 用例保留：`grep -c "testServiceRestartsWithoutManualShutdownAfterWorkerFault"` → **1**；
- 修复确已撤销：`enqueue()` 恢复为原实现。

独立构建目录 `/var/tmp/t25-p1-revert-build`：`CONFIGURE_rc=0`、**`BUILD_rc=0`（编译通过，
不属编译失败）**。

| 运行 | 退出码 | 结果 |
|---|---|---|
| revert-1..3 | **`rc=1`（3/3）** | `Totals: 2 passed, 1 failed`，`15114–15124ms` |

失败位置与红灯一致（`test_flowexecutionservice.cpp:1030`，`failedSpy.count()` 实际 0 / 期望 1）。

### 11.9 提交顺序声明（不得追溯改写）

```text
… → 732640c → 70c6490 → 572ea80 → b52bcc9
  → fc28456（test：钩子 + 回归用例，红灯）
  → 9bd3aad（fix：线程回收重建 + 命令清理 + 出队记录 job）
```

- `fc28456` 是**先于**修复提交的独立红灯提交，不得与 `9bd3aad` 合并追溯。
- 第 11.7 节的绿灯证据对应 `9bd3aad`，第 11.8 节的复败证据对应 `revert 9bd3aad`。
- 本次整改**不代表** T2.5 整体已通过审核，仍需评审收口裁定。

### 11.10 正式门禁（全新工作区外构建目录，2026-11-10）

| 项 | 值 |
|---|---|
| 交付树 | `HEAD=a5514a7`、`STATUS=[]`（门禁时点；`a5514a7` 仅含文档，二进制等价于 `9bd3aad`） |
| 构建目录 | `/var/tmp/t25-p1-close-build`（**工作区外全新目录**） |
| 配置 | `CONFIGURE_rc=0`（`Generating done (392.7s)`） |
| 构建 | `BUILD_rc=0`、`[100%] Built target test_mainwindow`（耗时 15m24s） |
| 格式 / 差异 | `FMT_CPP_rc=0`、`FMT_H_rc=0`、`FMT_TEST_rc=0`、`DIFFCHECK_rc=0` |
| CTest 列举 | `Total Tests: 97`、`LISTED=97`（口径 `grep -c "Test *#"`，避开 `^  Test #` 双空格漏计） |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time = 137.89 sec`） |
| 相关套件（同一交付态，独立运行） | service **26/26**、runengine **118/118**、joblease 8/8、userpause 6/6、finalizer 6/6（全部 `rc=0`） |

日志：`/var/tmp/t25-p1-close-out/{configure,build,ctest,ctest-list,tip_*}.log`。

### 11.11 状态

| 项 | 状态 |
|---|---|
| P1 主缺陷（线程不重建） | **红灯 5/5、绿灯 5/5、撤销复败 3/3 已闭合**（`fc28456 → 9bd3aad`） |
| P1 次缺陷（出队窗口漏报终态） | **同上链条一并闭合**（诊断 `failed=0` 佐证，修复后 `failedSpy.count()==1`） |
| 全量 CTest（97） | **已执行：97/97 通过**（全新工作区外目录） |
| T2.5 | **待评审收口裁定**（本轮不自行宣告收口） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成** |

## 12. T2.5 P1 第二轮回：故障通知后立即重试的窗口（2026-11-10 复核裁定）

### 12.1 裁定的问题

按 `901b222` 复核，评审仍给出 1 项 P1，暂不批准 T2.5 收口：

> 故障通知后立即重试，仍可能把命令交给正在退出的线程。`FlowExecutionService.cpp:647`
> 发出 `jobFailed` 时旧 worker 尚未真正退出；`FlowExecutionService.cpp:143` 只回收已经
> `isFinished()` 的线程。立即重试可能跳过回收，继续向不会再消费命令的旧线程入队，
> 最终握手超时、启动失败。

评审独立复现：标准异常 **19/200**、非标准异常 **3/200**、标准异常加诊断复跑 **28/200**。
诊断确认失败时旧线程尚未结束，等待约 100ms 后返回 `false`，模块执行增量为 0。并指出
现有 `tests/test_flowexecutionservice.cpp:1030` 用 `QTRY_COMPARE` 等通知，容易错过这个短窗口。

### 12.2 缺陷机理（静态事实）

| 事实 | 位置 | 结论 |
|---|---|---|
| `jobFailed` 发出点 | `handleWorkerLoopFault()`（`FlowExecutionService.cpp`） | 在 **worker 线程的 `run()` 内部**发出 |
| 此刻 `run()` 状态 | `WorkerThread::run()` → `catch` → `handleWorkerLoopFault()` | `run()` **尚未返回**，`QThread::isFinished()` 仍为 `false` |
| `9bd3aad` 的 `enqueue()` 回收判据 | 单次 `m_thread->isFinished()` 快照 | 在窗口期内必然**漏判** |
| 漏判后果 | `ensureThreadLocked()` 只判 `m_thread` 非空 | 复用不会再消费队列的旧线程 → 握手 100ms 超时撤回 |

即：**用一次 `isFinished()` 快照去判定「线程是否还能接收命令」，在语义上就不成立**——
`isFinished()` 描述的是「线程已经结束」，而窗口期需要的是「线程是否已不可能再消费命令」。

### 12.3 回归手法的三轮试跑（过程如实保留）

第一版（`ef1adae`）用 `jobFailed` 的 `DirectConnection` 槽在 worker 线程栈内同步重试。
试跑即否定了这个手法，两条失败都必须记录：

| 轮次 | 手法 | 结果 | 结论 |
|---|---|---|---|
| v1 | `DirectConnection` 槽内同步 `startSingle()` | **5/5 `rc=134`** | 槽在正在退出的 worker 线程上执行，`enqueue()` 去 join 自己 → `QThread::wait: Thread tried to wait on itself` → `QThread: Destroyed while thread is still running` → abort |
| v2 | 槽内 `invokeMethod(..., Qt::QueuedConnection)` 投递到主线程 | **撤销态 3/3 全绿** | 主线程须等 worker 的 `emit` 返回、`run()` 走完才处理事件，窗口被完全绕过 → **该用例没有判别力** |
| v3 | 用退出屏障把 worker 停在窗口正中，主线程重试 | 撤销态 5/5 红灯、修复态 5/5 绿灯 | 确定性复现，采用 |

v2 的教训写入纪律：**「撤销态仍全绿」必须当作「用例无效」处理，不得因为修复后被绿就收货。**

### 12.4 采用的回归手法

`WorkerThread::run()` 在 `handleWorkerLoopFault()` 发出 `jobFailed` **之后**、`run()` 返回
**之前**调用既有退出屏障钩子 `runWorkerExitBarrierForTest()`。该点正是缺陷窗口本体：
`jobFailed` 已发出，`isFinished()` 仍为 false。

用例 `testImmediateRetryDuringWorkerRetirementMustSucceed`：

1. 置 `holdBarrier`，注入一次 `workerLoop` 故障，`startSingle()`（首启必被放行并返回 false）；
2. 用 `barrierReached.tryAcquire(1, 3000)` 等到 worker 停在屏障上；
3. 确认 `failureNotified` 为真后，**在主线程**调用 `startSingle()`——此刻旧线程尚未结束；
4. 放行屏障、等重试任务跑完，统计。

判据（全部落在 `startSingle()` 返回值与 `probe.runs()`，与异常语义无关）：

| 判据 | 含义 | 失败即说明 |
|---|---|---|
| `windowRetryFailed == 0` | 窗口内重试一次都不许被拒绝 | 命令被交给了正在退出的旧线程 |
| `retryNotExecuted == 0` | 被接受的重试必须真的执行过任务 | 命令进了黑洞，接受但空转 |
| `windowRetries >= 5` | 判别性前提：窗口必须真的被覆盖 | 用例空转，无判别力 |

`DirectConnection` 槽在本用例中**只做状态观测**（计数 + 置标志），不含任何阻塞动作、
不抛异常——槽内抛异常属 Qt 未定义行为，不得作为缺陷证据。

### 12.5 红灯（撤销态）

执行位置：隔离工作树 `/var/tmp/t25-p1-revert-wt`（`9bd3aad` + `9f4afba` 的用例，
`git apply` 净 diff，**不含任何修复**），独立构建目录 `/var/tmp/t25-p1-revert-build`。

```
BUILD_rc=0
for i in 1..5: ./bin/test_flowexecutionservice testImmediateRetryDuringWorkerRetirementMustSucceed
```

| 样本 | 退出码 | `barrierEntries` | `windowRetries` | `retryOk` | `retryFailed` | 失败断言 |
|---|---|---:|---:|---:|---:|---|
| red-1 | **1** | 10 | 10 | **0** | **10** | `windowRetryFailed.load() == 0` |
| red-2 | **1** | 10 | 10 | **0** | **10** | 同上 |
| red-3 | **1** | 10 | 10 | **0** | **10** | 同上 |
| red-4 | **1** | 10 | 10 | **0** | **10** | 同上 |
| red-5 | **1** | 10 | 10 | **0** | **10** | 同上 |

窗口命中率 **10/10 = 100%**（评审现场约 10%，因靠事件循环碰运气）。日志：
`/var/tmp/t25-p1b-red-v3/red-{1..5}.log`。

### 12.6 修复内容（分提交）

1. **线程自标记 retiring**（`WorkerThread`，`.cpp` 内定义的 private 嵌套类）。
   新增 `std::atomic_bool m_retiring` 与 `isRetiring()`；置位点选在 `run()` 两个 `catch`
   分支的**第一句**，早于 `handleWorkerLoopFault()`（即早于 `emit jobFailed`）；正常返回
   路径也补一次。`WorkerThread` 是 `.cpp` 内的私有嵌套类，`FlowExecutionService` 的类
   布局与 ABI 不变，头文件未改动。
2. **`enqueue()` 回收循环**。由单次快照改为 `for (;;)`：锁内判
   `isRetiring() || isFinished()` 并摘指针 → `if (!retired) break;` → **锁外**
   `joinWorkerThread(retired)` → 回到顶部重判。回收始终在锁外（持锁 join 会卡死所有
   服务入口）；只摘指针、不放弃 join 语义，旧线程一定等它真正退出后 `delete`，
   绝不 detach/terminate。

### 12.7 绿灯（修复态）

执行位置：隔离工作树 `/var/tmp/t25-p1-probe-wt`（`ef1adae` + 修复 + v3 用例），
独立构建目录 `/var/tmp/t25-p1-build`。

| 样本 | 退出码 | `barrierEntries` | `windowRetries` | `retryOk` | `retryFailed` | `retryNotExecuted` |
|---|---|---:|---:|---:|---:|---:|
| green-1 | **0** | 10 | 10 | **10** | **0** | **0** |
| green-2 | **0** | 10 | 10 | **10** | **0** | **0** |
| green-3 | **0** | 10 | 10 | **10** | **0** | **0** |
| green-4 | **0** | 10 | 10 | **10** | **0** | **0** |
| green-5 | **0** | 10 | 10 | **10** | **0** | **0** |

service 全套件：`Totals: 27 passed, 0 failed`、`SUITE_rc=0`（52846ms）。
日志：`/var/tmp/t25-p1b-green-v3/{green-1..5,suite}.log`。

### 12.8 提交顺序与纪律

```
… → 901b222(docs) → ef1adae(test v1) → 9f4afba(test v2/v3 手法更正) → 06c1ff8(fix) → <本提交>(docs)
```

- `ef1adae` 保留在链上，其手法缺陷在本章 12.3 如实记录，**不改写历史**。
- `9f4afba` 只改测试（不新增生产注入点，复用既有 `setWorkerExitBarrierForTest`），
  `128 insertions(+), 64 deletions(-)`。
- `06c1ff8` 只改 `src/core/engine/FlowExecutionService.cpp`（`55 insertions(+), 16 deletions(-)`），
  头文件、类布局、ABI 均无变化。
- 缺陷态只在隔离工作树 + 独立构建目录执行，交付工作树全程只保留受审的修复态。

### 12.9 格式与差异检查

| 检查 | 命令 | 结果 |
|---|---|---|
| 格式（生产） | `clang-format --dry-run --Werror src/core/engine/FlowExecutionService.cpp` | `FMT_CPP_rc=0` |
| 格式（测试） | `clang-format --dry-run --Werror tests/test_flowexecutionservice.cpp` | `FMT_TEST_rc=0` |
| 差异 | `git diff --check` | `DIFFCHECK_rc=0` |

### 12.10 全量 CTest 门禁

| 项 | 值 |
|---|---|
| 源提交 | `06c1ff8` |
| 构建目录 | `/var/tmp/t25-p1b-gate-build`（**工作区外全新目录**） |
| 配置 | `CONFIGURE_rc=0`（耗时 7m36s）；选项 `-DUSE_QT6=ON -DQt5_DIR=… -DONNXRUNTIME_DIR=… -DOpenCV_DIR=… -DDEEPLUX_NATIVE_SAM=ON -DENABLE_CAMERA_CAPTURE=OFF -DENABLE_CAMERA_SERVICE=OFF -DENABLE_HYMSON3D=OFF` |
| 构建 | `BUILD_rc=0`、`[100%] Built target test_mainwindow`（耗时 18m56s） |
| 格式 / 差异 | `FMT_CPP_rc=0`、`FMT_TEST_rc=0`、`DIFFCHECK_rc=0` |
| CTest 列举 | `Total Tests: 97`、`LISTED=97`（口径 `grep -c "Test *#"`，避开 `^  Test #` 双空格漏计） |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time (real) = 260.63 sec`） |
| 相关套件（同一交付态，独立运行） | service **27/27**（52795ms）、runengine **118/118**（8363ms）、joblease 8/8、userpause 6/6、finalizer 6/6（全部 `rc=0`） |

日志：`/var/tmp/t25-p1b-gate-{configure,build,ctest}.log`、`/var/tmp/t25-p1b-suite-test_*.log`。

### 12.11 状态

| 项 | 状态 |
|---|---|
| P1（故障通知后立即重试） | **红灯 5/5、绿灯 5/5 已闭合**（`9f4afba → 06c1ff8`），撤销态命中率 100% |
| 首版手法（`ef1adae`） | **已作废并留档**：直连槽内 join 自己（abort）、Queued 投递绕过窗口（无判别力） |
| service 套件 | **27/27**（较 `901b222` 的 26/26 多出本用例，属新增而非修复既有失格） |
| 全量 CTest（97） | **已执行：97/97 通过**（全新工作区外目录，`06c1ff8`） |
| T2.5 | **待评审收口裁定**（本轮不自行宣告收口） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成** |

---

## 13. T2.8 CancellationToken 借用语义 / T2.9 runId 跨线程快照（2026-11-10 裁定）

### 13.1 裁定与口径

两项均为 P2，经裁定按以下口径处置，**均不制造 NF 红灯**：

| 任务 | 裁定方案 | 记录口径（固定措辞） |
|---|---|---|
| T2.8 | 按契约整改 + 行为契约测试 | 「**行为契约／预防性重构**」——承认借用表语义缺陷并按契约整改 |
| T2.9 | 作为「预防性加固」收口 | 「**预防性加固，未发现真实竞态**」——不修复任何已知缺陷 |

按本文件全局约定第 11 条：预防性重构／行为契约类任务**不得制造 NF 红灯**，须注明契约性质。
两项任务的判别性证明因此采用**撤销态失败**（T2.8）与**故意脱节反证**（T2.9）两种手法，
而不是制造缺陷变体——因为 T2.8 的缺陷态就是「整改前的真实代码」，T2.9 则本就无缺陷态可造。

### 13.2 T2.8 缺陷机理（静态事实）

| 事实 | 位置 | 结论 |
|---|---|---|
| 借用表写入 | `ModuleBase::execute()` 入口 `setCancellationToken(context.cancellationToken)` | 每次执行都写入全局表 `g_cancellationTokens`（`QHash<const ModuleBase*, CancellationToken*>`） |
| 写入后是否清理 | 同函数 7 个 `return` 出口**均无**摘除动作 | **只设不清**：本次执行结束后表内条目永久留存 |
| `CancellationToken` 析构 | 整改前**无析构函数** | token 销毁时不会通知借用表 |
| `ModuleBase::~ModuleBase()` | 只删自己那一条记录 | 反过来（模块先死）能清；token 先死则清不掉 |
| 全仓 `setCancellationToken(nullptr)` | 整改前 `src/` 生产路径**从不**主动摘除 | 仅测试 `test_sam_region_flow.cpp` 有一处 |
| 读取方 | `DelayPlugin`、`FitPlanePlugin`、`FitEllipsePlugin`、`SamPredictPlugin` 的 `isCancellationRequested()` | 表内有悬垂指针即解引用野指针 |
| 可达路径 | `RunEngine` 析构后模块仍被插件管理器持有；或调用方传入栈上 token | token 先于模块消失 → 悬垂 |

即：**借用表里存的是裸指针，而「借用」却没有与任何一方的生命周期绑定**。任一方先消失，
留下的条目就变成悬垂指针；而 `isCancellationRequested()` 是插件在自己的执行循环里反复调用的，
不保证发生在任何一方的析构之前。

### 13.3 T2.8 整改（双重防线，`463d6b7`）

1. **执行期限定**：`ModuleBase::execute()` 内用 RAII 作用域守卫，退出时还原借用。
   - 用 RAII 而非逐出口手工清表：该函数有 **7 个提前 `return`**，手工清必漏路径。
   - 只在本次**确实安装了** token 时还原（`tokenInstalledHere`），避免抹掉别人装的 token。
2. **析构兜底**：`CancellationToken::~CancellationToken()` 调用新增的
   `ModuleBase::detachCancellationToken(this)`，遍历整表用 `it = g_cancellationTokens.erase(it)`
   摘除**所有**指向该 token 的条目——同一 token 可能被多个模块借用过（并行批次、多次执行）。

两道防线互补，覆盖两个方向：守卫负责「执行结束」，析构负责「token 提前消失」。
不变式：**借用表在任意时刻只允许出现「活的、当前正在执行的」token。**

ABI：`ModuleBase` 只新增静态成员函数（非虚、无数据成员），对象布局不变；
`CancellationToken` 新增虚析构（基类 `QObject` 本就有虚析构，vtable 布局不变）。

### 13.4 T2.8 必须记录的语义变更（诚实边界）

守卫会在 `execute()` 返回时把 token 摘掉，因此**执行结束后模块不再持有 token**——这正是整改目标，
但它同时改变了既有调用方可见的行为，须明确记录：

| 调用方 | 既有写法 | 整改后实际行为 | 影响判定 |
|---|---|---|---|
| `RunEngine::executeModule()` | `:2210` 执行前 `setCancellationToken(m_cancellationToken)`，随后 `execCtx.cancellationToken = m_cancellationToken`（`:2247`）再设置一次 | 守卫在 `execute()` 返回时把这**同一** token 摘除；`:2210` 那次设置的残留被一并清掉 | **无功能影响**：`execCtx` 每次都传入同一 token，执行期可见性不变；插件读取只发生在执行期 |
| `RunEngine::executeBatchParallel()` 池任务 | `:1815` `setCancellationToken(batchToken)`，随后 `context.cancellationToken = batchToken` | 同上 | **无功能影响**，理由同上 |
| `tests/test_sam_region_flow.cpp:416` | 显式 `setCancellationToken(nullptr)` | 语义不变（`nullptr` → `remove`） | 无影响 |

结论：`RunEngine` 的两处冗余前置设置成为**冗余但无害**；由于 `execCtx` 已携带 token，
执行期可见性（插件 `isCancellationRequested()` 的唯一使用场景）完全保持。这**不是**回归，
而是把原先「泄漏到执行结束后」的借用收敛到执行期内的预期结果。
**本轮不改动 `RunEngine` 的这两处前置设置**，以避免把 T2.8 与 T2.9 的改动面混在一起；
它们作为后续可选清理项记录在此。

### 13.5 T2.8 回归与判别性（撤销态失败）

用例（`8706b41`，`tests/test_modulebase.cpp`）：

| 用例 | 断言要点 |
|---|---|
| `testCancellationTokenDetachedAfterExecute` | 先证「**装得上**」（`process()` 执行期 `cancellationToken() != nullptr`），再证「**退得掉**」（返回后为 `nullptr`） |
| `testCancellationTokenDtorLeavesNoDanglingBorrow` | 手工建立借用后销毁 token，模拟「token 先于模块消失」，断言借用已被兜底清空 |

「装得上」这一半是**防恒真**用的：只断言「返回后为空」时，一个从不写表的实现也能通过。

`isCancellationRequested()` 是 `protected`（只给插件自己在执行期用），测试侧加公开转发
`cancellationRequestedForTest()`，**不改生产访问权限**。

撤销态验证（隔离工作树 `/var/tmp/t28-revert-wt` = `f641a51` + 仅测试补丁，
独立构建目录 `/var/tmp/t28-revert-build`，`BUILD_rc=0`）：

| 样本 | 退出码 | 失败位置 | 失败断言 |
|---|---|---|---|
| red-1..2 / **2 项用例** | **2** | `test_modulebase.cpp:298`、`:322` | `module.cancellationToken() == nullptr`（执行后与析构后均残留借用） |

失败语义正确：撤销态下两条用例都因**表内残留条目**失败，正是缺陷本体。
修复态：`test_modulebase` **14/14**（原 12，+2）、`rc=0`。

#### 13.5.1 守卫覆盖范围更正（2026-11-11 复核 P2-1）

本轮整改的守卫**当时建在输入校验之后**，覆盖面不足。按真实调用顺序：
`RunEngine::executeModule()` 在调用 `execute()` **之前**就 `setCancellationToken()`
（`RunEngine.cpp:2210`），而 `execute()` 里的输入校验（`ModuleBase.cpp` 原 `:196` 之前）
有 4 个早退出口，全部**先于**守卫返回：

| 早退出口 | 错误码 | 原覆盖情况 |
|---|---|---|
| 缺少必需输入 | `MissingRequiredInput` **1001** | **未覆盖**：借用残留 |
| 输入类型不匹配（`multiple` 元素） | `TypeMismatch` **1002** | **未覆盖**：借用残留 |
| 输入类型不匹配（整值） | `TypeMismatch` **1002** | **未覆盖**：借用残留 |
| 多输入元素类型不匹配 | `TypeMismatch` **1002** | **未覆盖**：借用残留 |

因此「合法执行才清除、两条早退路径执行结束后借用仍存在」成立，只有合法路径被保护。

**整改（`96829f9`）**：把 token 安装与 RAII 守卫一起前置到 `execute()` 的**函数入口**，
使全部 7 个 `return` 出口（含上述 4 个校验早退）都在守卫作用域内。语义不变：仍只在
本次确实安装了 token 时还原。

**新增回归**（`1411b8c`，两条）：

| 用例 | 构造（按真实调用顺序） | 断言 |
|---|---|---|
| `testCancellationTokenDetachedOnMissingRequiredInput` | 先 `setCancellationToken()`，再省略必需端口 `count` | `errorCode == 1001`、`process()` 未调用、`cancellationToken() == nullptr` |
| `testCancellationTokenDetachedOnTypeMismatch` | 先 `setCancellationToken()`，再给 `count`（声明 `Integer`）传 `QString` | `errorCode == 1002`、`process()` 未调用、`cancellationToken() == nullptr` |

两条都**先确认打中的是目标早退分支**（错误码 + `process()` 未执行），避免用例空转。

撤销态验证（隔离工作树 `/var/tmp/t28p1-probe-wt` + 独立构建 `/var/tmp/t28p1-probe-build`，
撤销态 = `0bf7516` 原 `ModuleBase.cpp`，仅叠加测试）：

| 样本 | 退出码 | 失败位置 | 失败断言 |
|---|---|---|---|
| red-1..2 / **2 项用例** | **2** | `test_modulebase.cpp:357`、`:384` | `module.cancellationToken() == nullptr`（两条早退路径借用均残留） |

修复态：`test_modulebase` **16/16**（原 14，+2）、`rc=0`。

### 13.6 T2.9 静态排查结论：未发现真实竞态

| 事实 | 位置 | 结论 |
|---|---|---|
| `m_runId` 唯一写入点 | `RunEngine::tryBeginForRun()`（`RunEngine.cpp:654`） | 单一写者，且在执行开始前写定 |
| 读取点 1 | `executeModule()` 的 `execCtx.runId` | 串行执行路径 |
| 读取点 2 | `publishDisplayFrame()` 的 `frame.runId` | 串行路径的调用者 |
| 池线程路径 | `executeBatchParallel()` 早在 `:1787` 就 `const QString batchRunId = m_runId;`，池 lambda 内**只用 `batchRunId`**、不读 `m_runId`（`:1800`） | 池线程**从未**读 `m_runId` |

**结论：未发现真实竞态。** 本任务因此不制造缺陷变体、不制造红灯，按「预防性加固」收口。

### 13.7 T2.9 加固内容（`b50b653`）

把「池线程不得读可能并发变化的成员」这条**已有边界**从一条路径推广到全部路径：

1. `executeModule()` 开头一次快照 `const QString execRunId = m_runId;`，`execCtx.runId` 用快照；
2. `publishDisplayFrame()` 签名增加 `const QString& runId`，`frame.runId = runId`，不再读成员；
3. 效果：读 `m_runId` 的地方从两处收敛为**调用线程上的一处**，后续重构无法在不改签名的前提下
   把跨线程读取带回来。

**有意不动 `publishFinalDisplayFrame()`**（`:2646`，仍 `frame.runId = m_runId;`）：它的两个调用点
（`:410`、`:1642`）都在执行入口收尾处，此时 runId 早已写定且无并发写者；为保持改动最小可审，
本轮不动并在此记录判定依据。

行为不变：帧 runId 取值与改动前完全一致（同一份 `m_runId` 快照）。
ABI：`publishDisplayFrame` 是 private 成员函数，仅一处调用点，非公开 ABI 面。

### 13.8 T2.9 判别性反证（两次尝试，如实保留）

契约测试断言「执行期看到的 runId 与最终发布帧的 runId 同源且非空」。判别性用**故意脱节**验证：

| 轮次 | 注入 | 结果 | 结论 |
|---|---|---|---|
| v1 | 仅把 `publishDisplayFrame` 的 `frame.runId` 改为 `m_runId + "-PROBE"` | **`PROBE_rc=0` 仍绿** | **该轮反证无效** |
| v2 | 再对 `publishFinalDisplayFrame` 注入 `"-PROBE2"` | **`PROBE2_rc=1` 失败**：`Actual "1794308809179-0-PROBE2"` vs `Expected "1794308809179-0"` | 断言确实读实值，但只证明了最终快照那条路 |

#### 13.8.1 v1 归因更正（2026-11-11 复核 P2）

本节此前把 v1 失效归因于「`publishFinalDisplayFrame()` 随后**覆盖**槽位帧，因此单点脱节被
最终帧掩盖」。**该归因不准确，现予更正。**

真实原因：用例的模块只传递**空图像和元数据**，`publishDisplayFrame()` 在
`frameImage.isNull()` 处**直接返回**——本轮修改的图像帧赋值 `frame.runId = runId`
**一处都没有执行**。评审独立观测为「图像帧 0 个、最终快照帧 1 个」，可是原断言仍通过。
换言之 v1 的「仍绿」不是因为覆盖链，而是因为**那段代码根本没跑**。

同理，v2 让用例变红只证明 `publishFinalDisplayFrame()` 受到了断言保护，
**不能**证明「新增的快照传参（`publishDisplayFrame(..., runId)`）受到保护」——
它改的是本轮**未改动**的函数。

#### 13.8.2 修正与新的判别性证明

用例已按复核要求重做（`3424252`，仅测试）：

- 模块产出**真实 8×8 小图**（`enableSyntheticImage(true)`），使 `publishDisplayFrame`
  真的走到帧 runId 赋值；
- 用 `displayFrameReady` 信号在**每帧发布时**捕获 `(runId, 是否带图像)`，不依赖
  `latestDisplayFrame()`——后者会被最终快照帧覆盖，无法区分中间帧；
- 三条判据分开断言：① 至少 1 个**带图像**的模块帧；② 每个带图像帧的 runId == 执行期 runId；
  ③ 最终快照帧的 runId 另行同源断言。

**只破坏本轮改动的那一处**即可判红（`RunEngine.cpp:2628` 加 `"-PROBE-MODULEFRAME"`
后缀，`publishFinalDisplayFrame()` 完全不动）：

| 注入范围 | 结果 |
|---|---|
| 仅 `publishDisplayFrame`（本轮改动处） | **`rc=1`**，`Actual "1794361136936-0-PROBE-MODULEFRAME"` vs `Expected "1794361136936-0"`，失败位在带图像帧的 runId 断言 |
| 反证清除后（`grep -c PROBE` = 0，`RunEngine.cpp` 无差异） | 通过，`test_runengine` **119/119** |

**教训（写入纪律，已按复核意见修订）**：单点反证通过 ≠ 用例有判别力；但更根本的是——
**反证之前必须先确认被测代码路径真的被执行**。旧用例的问题不是「测了中间值」，
而是「被测赋值零次执行」还照样绿。凡涉及发布/回调的断言，必须先有一条
「该路径确实发生过」的正向前提（上文的判据①），再谈值比较；且反证必须落在
**本轮真正改动的那一处**，打在未改动处变红不构成保护证据。

反证随后**完全清除**：`grep -c PROBE` = 0，`RunEngine.cpp` 自 `/var/tmp/RunEngine.fixed.bak` 回滚，
与交付树 diff 逐字节一致。修复态：`test_runengine` **119/119**（原 118，+1）、`rc=0`。

另记录一处测试前提：`DisplayFrame` 默认**不采集**（`m_displayCaptureEnabled` 默认 `false`），
用例内须显式 `setDisplayCaptureEnabled(true)` 并在结束前复位——首跑正是因此失败于
`!frame.runId.isEmpty()`（`rc=1`），补上采集开关后转绿。

### 13.9 格式与差异检查

| 检查 | 结果 |
|---|---|
| `clang-format -i` + `--dry-run --Werror`（8 个文件） | 全部 `rc=0` |
| `git diff --check` | `DIFFCHECK_rc=0` |

### 13.10 提交链

```
… → f641a51(docs) → 8706b41(test T2.8+T2.9) → 463d6b7(fix T2.8) → b50b653(refactor T2.9) → <本章>(docs)
```

| 提交 | 类型 | 改动面 |
|---|---|---|
| `8706b41` | test | `tests/test_modulebase.cpp`、`tests/test_runengine.cpp`（`177 insertions(+), 67 deletions(-)`），不含任何修复 |
| `463d6b7` | fix T2.8 | `ModuleBase.{h,cpp}`、`CancellationToken.{h,cpp}`（`62 insertions(+)`），无数据成员、无布局变化 |
| `b50b653` | refactor T2.9 | `RunEngine.{h,cpp}`（`14 insertions(+), 5 deletions(-)`），private 函数签名变更 |

### 13.11 全量 CTest 门禁

| 项 | 值 |
|---|---|
| 源提交 | `b50b653` |
| 构建目录 | `/var/tmp/t28-gate-build`（**工作区外全新目录**） |
| 配置 | `CONFIGURE_rc=0`（耗时 8m26s）；选项 `-DUSE_QT6=ON -DQt5_DIR=… -DONNXRUNTIME_DIR=… -DOpenCV_DIR=… -DDEEPLUX_NATIVE_SAM=ON -DENABLE_CAMERA_CAPTURE=OFF -DENABLE_CAMERA_SERVICE=OFF -DENABLE_HYMSON3D=OFF` |
| 构建 | `BUILD_rc=0`、`[100%] Built target test_finalplugins`（耗时 16m11s） |
| 格式 / 差异 | `FMT_rc=0`（8 个文件 `--dry-run --Werror`）、`DIFFCHECK_rc=0` |
| CTest 列举 | `Total Tests: 97`、`LISTED=97`（口径 `grep -c "Test *#"`，避开 `^  Test #` 双空格漏计） |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time (real) = 259.82 sec`） |
| 相关套件（同一交付态，独立运行） | `test_modulebase` **14/14**（1ms）、`test_runengine` **119/119**（8393ms）、`test_flowexecutionservice` **27/27**（52918ms）、`test_runenginejoblease` 8/8、`test_runengineuserpause` 6/6、`test_runenginefinalizer` 6/6（全部 `rc=0`） |

日志：`/var/tmp/t28-gate-{configure,build,list,ctest}.log`、`/var/tmp/t28-suite-test_*.log`。
门禁时点：交付工作树 `/home/charles/Data/Repo/DeepLux-hardening`，`HEAD=b50b653`，`git status --porcelain` 为空
（第 13 章文档改动在门禁之后提交，不影响被测源码）。

**关于仍为 97 项的口径说明**：本轮新增的 3 条用例（T2.8 两条 + T2.9 一条）落在**已有**的测试可执行
文件内（`test_modulebase`、`test_runengine`），未新增 CTest 测试目标，故总数与上一轮的 97 一致。
**不得**据此认为「未加入新测试」——增量体现在套件内用例数（modulebase 12→14、runengine 118→119）。

### 13.12 状态

| 项 | 状态 |
|---|---|
| T2.8 | **已按契约整改**（`463d6b7`）：撤销态 2/2 失败、修复态 modulebase 14/14；性质为「行为契约／预防性重构」 |
| T2.9 | **已预防性加固**（`b50b653`）：未发现真实竞态，判别性反证 `PROBE2_rc=1` 后清除；runengine 119/119 |
| T2.5 | **待评审收口裁定**（本轮不自行宣告收口） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成**（十轮连续门禁、`git revert` 抽查待整批完成后执行） |

---

## 14. T2.5 修复绕过启动等待上限（2026-11-11 复核 P1）

### 14.1 裁定的问题

复核至 `0bf7516` 给出 1 项 P1：

> `FlowExecutionService.cpp:187` 在启动计时前调用 `joinWorkerThread()`，其等待可以无限
> 延长。独立探针将退出阶段延迟 500ms，`startSingle()` 就阻塞了 500ms，超过既有 100ms
> 预算。当前回归每轮依赖退出屏障的 5 秒超时，十轮约 50 秒，并未验证 GUI 响应性。

**验收口径需明确调整**：恢复未完成时允许明确拒绝，但不得入队或迟到执行；旧线程退出后，
应无需手动 shutdown 即可再次启动。**不能为了「窗口内重试必成功」阻塞 GUI。**

### 14.2 缺陷机理（静态事实）

| 事实 | 位置 | 结论 |
|---|---|---|
| 回收循环位置 | `enqueue()` 开头 `for(;;)`，在 `StartHandshakeTimeoutMs` 计时**之前** | 回收等待完全不计入启动预算 |
| 回收调用 | `joinWorkerThread(retired)`（原 `:187`） | 该函数在诊断阈值后**无条件** `thread->wait()`（原 `:135`） |
| `m_shutdownDeadlineMs` | 默认 **10000ms**，且超时后继续 `wait()` | 实际上界是「无限」，与 100ms 启动预算无关 |
| `StartHandshakeTimeoutMs` | **100ms** | 只约束握手，不约束回收 |

即：**启动路径的等待被拆成两段，只有后一段有界**。第一段（回收）用的是关机预算、
且超时后仍继续等，于是旧线程的退出耗时被完整转嫁给调用 `startSingle()` 的 GUI 线程。

### 14.3 缺陷复现（独立探针，先于修复）

执行位置：隔离工作树 `/var/tmp/t28p1-probe-wt`（`0bf7516`）+ 独立构建
`/var/tmp/t28p1-probe-build`，`PROBE_CONFIGURE_rc=0`、`PROBE_BUILD_rc=0`。

探针手法：退出屏障内 `QThread::msleep(500)` 模拟「退出阶段延迟 500ms」，随后在窗口内
计时一次 `startSingle()`。

| 状态 | `startSingleElapsedMs` | `retryOk` |
|---|---|---|
| 撤销态（`0bf7516`） | **500** | 1 |
| 修复态 | **100** | 0（明确拒绝） |

**与评审观测一致**：500ms 阻塞、超过 100ms 预算。

### 14.4 整改内容（`1471620`，仅 `FlowExecutionService`）

1. 新增私有 `retireWorkerThreadBounded(thread, budgetMs)`：预算内退出则 `delete` 并返回
   `true`；否则**不阻塞调用线程**——把线程交给一个自持有指针的后台收尾线程
   `thread->wait()` 后 `delete`，返回 `false`。**不 detach、不 terminate**，join 语义保留，
   只是把**等待者**从 GUI 线程挪到非 GUI 线程。
2. `enqueue()` 回收循环改用上述有界回收，预算 = `StartHandshakeTimeoutMs` 减去已耗时；
   预算耗尽即 `return false`（明确拒绝）。
3. 握手等待与回收**共用同一个** `QElapsedTimer`，总等待仍不超过 `StartHandshakeTimeoutMs`，
   不再是「回收 100ms + 握手 100ms」两段叠加。
4. `joinWorkerThread()` **保持原样**，仅用于析构（`:118`）与 `shutdown()`（`:434`）——
   那两处必须等 worker 真正退出才能 `delete` 服务对象，不能改为有界。

无数据成员变化、无公开接口变化。

### 14.5 验收口径重写（`dbcfb77`，仅测试）

旧判据「窗口内重试必须成功（`retryFailed == 0`）」是**错误**的口径——它只能靠无限期
阻塞 GUI 达成。现改为五条：

| 判据 | 内容 | 观测字段 |
|---|---|---|
| ① | 单次 `startSingle()` 阻塞 ≤ `StartHandshakeTimeoutMs`（回收+握手共用） | `maxRetryBlockMs` |
| ② | 窗口内允许**明确拒绝**（返回 false） | `windowRetryFailed` |
| ③ | 拒绝后必须 Idle 且无 pending | `rejectedLeftResidue` |
| ④ | 被拒绝的那次**绝不能事后执行** | `rejectedButExecuted` |
| ⑤ | 旧线程退出后再次启动必须成功，无需手动 shutdown() | `restartOk` |

判据①的调度误差许可 `kSchedulingSlackMs = 60ms`：远小于 500ms 量级，仍能可靠区分
「有界」与「跟着退出延迟走」，同时不至于把预算当硬实时。

用例更名 `testImmediateRetryDuringWorkerRetirementMustSucceed` →
`testRetryDuringWorkerRetirementIsBoundedAndNotDeferred`。

### 14.6 撤销态复败

撤销态 = `0bf7516`（生产代码原样），仅叠加 `dbcfb77` 的测试，同一独立构建目录。

| 样本 | 退出码 | `maxRetryBlockMs` | 失败断言 |
|---|---|---|---|
| red-1 | **1** | **5000** | `maxRetryBlockMs <= budget + kSchedulingSlackMs` |
| red-2 | **1** | **5000** | 同上 |

`maxBlock=5000ms vs budget=100ms`——**5000 正是退出屏障的 5 秒超时**，精确印证评审
「每轮依赖退出屏障的 5 秒超时，十轮约 50 秒」的说法：撤销态下该调用会一直等到屏障
自己超时。red-3 因 `timeout` 命令自身发出 SIGTERM 而中断，不影响前两次的结论。

### 14.7 修复态绿灯

| 指标 | 值 |
|---|---|
| `windowRetries` | 10 |
| `windowRetryOk` / `windowRetryFailed` | 0 / **10**（全部明确拒绝） |
| `maxRetryBlockMs` | **100** |
| `rejectedResidue` | 0 |
| `rejectedExecuted` | 0 |
| `restartOk` / `restartNotExecuted` | **10** / 0 |
| 用例耗时 | 约 **9s**（原约 50s） |
| 退出码 | **0** |

### 14.8 提交链与门禁

```
… → 0bf7516(docs) → 3424252(test T2.9) → 1411b8c(test T2.8) → 96829f9(fix T2.8)
   → dbcfb77(test P1) → 1471620(fix P1) → <本章>(docs)
```

测试与修复分提交：`3424252`/`1411b8c`/`dbcfb77` 只含测试，`96829f9`/`1471620` 只含生产代码。

全量 CTest 门禁见 14.9。

### 14.9 全量 CTest 门禁

| 项 | 值 |
|---|---|
| 源提交 | `1471620` |
| 构建目录 | `/var/tmp/t28p1-gate2-build`（**工作区外全新目录**） |
| 配置 | `CONFIGURE2_rc=0`（耗时 9m18s）；选项同前（`-DUSE_QT6=ON -DQt5_DIR=… -DONNXRUNTIME_DIR=… -DOpenCV_DIR=… -DDEEPLUX_NATIVE_SAM=ON -DENABLE_CAMERA_CAPTURE=OFF -DENABLE_CAMERA_SERVICE=OFF -DENABLE_HYMSON3D=OFF`） |
| 构建 | `BUILD_rc=0`、`[100%] Built target test_mainwindow`（耗时 16m00s） |
| 格式 / 差异 | `FMT_rc=0`（6 个文件 `--dry-run --Werror`）、`DIFFCHECK_rc=0` |
| CTest 列举 | `Total Tests: 97`、`LISTED=97` |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time (real) = 221.55 sec`） |
| 相关套件（同一交付态，独立运行） | `test_modulebase` **16/16**（1ms）、`test_runengine` **119/119**（8386ms）、`test_flowexecutionservice` **27/27**（**9795ms**）、`test_runenginejoblease` 8/8、`test_runengineuserpause` 6/6、`test_runenginefinalizer` 6/6（全部 `rc=0`） |

日志：`/var/tmp/t28p1-gate2-{configure,build,list,ctest}.log`、`/var/tmp/t28p1-suite-test_*.log`。
门禁时点：交付工作树 `HEAD=1471620`，源码文件 `git status` 为空（第 14 章文档改动在门禁之后提交）。

**收益可量化**：`test_flowexecutionservice` 套件耗时由上一轮的 **52918ms 降到 9795ms**——
少的正是「每轮等 5 秒屏障超时 × 10 轮」。这从套件耗时一侧独立印证了 P1 的修复效果。

**仍为 97 项的口径**：本轮新增 2 条用例（`test_modulebase` 14→16）并重写 2 条（服务层窗口用例
更名、T2.9 用例加强），均落在**已有**测试可执行文件内，未新增 CTest 目标。

**说明**：首次配置曾以 `CONFIGURE_rc=1` 失败于
`CMake Error: Cannot open file for write: …/tests/CMakeFiles/test_fitline.dir/compiler_depend.ts.tmp3b774`
+ `System Error: Inappropriate ioctl for device`——这是本项目已知的沙箱 I/O 干扰（见工作记忆的
「门禁环境教训」），与代码无关。该目录已删除，改在全新目录 `t28p1-gate2-build` 重跑，
**不携带非零退出码送审**。

### 14.10 状态

| 项 | 状态 |
|---|---|
| P1（启动等待上限） | **已复现（500ms）、已修复（100ms）、撤销态 2/2 红灯（5000ms）**，性质为真实缺陷 |
| P2-1（早退守卫） | **已整改**：撤销态 2/2 失败、修复态 modulebase 16/16 |
| P2-2（图像帧路径） | **已整改**：只破坏本轮改动处即 `rc=1`，runengine 119/119 |
| P1（第三轮，见第 15 章） | **已复现、已修复、撤销态 5/5 红灯**，性质为真实缺陷；**待评审收口裁定** |
| T2.5 / T2.8 / T2.9 | **暂不收口**（按复核要求） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成** |

---

## 15. 后台回收失去 Service 生命周期管理（2026-11-11 二次复核 P1）

### 15.1 裁定的问题

复核至 `152a4a7` 认定第 14 章的修复本身通过（启动预算已解决、两项 P2 已关闭），
但**该修复引入了新的 P1**：

> `FlowExecutionService.cpp:145` 将旧 worker 交给 detached 回收线程，而 `m_thread` 已清空。
> `shutdown():423` 和析构只检查 `m_thread`，因此漏等这个仍在运行的 worker。
> 独立探针连续 3/3 复现：`retry=0 retryMs=100` / `shutdownOk=1 shutdownMs=0` /
> `oldWorkerStillRunning=1`。这会破坏「shutdown 成功后可以销毁 worker 所访问对象」的保证，
> 也使既有测试清理 guard 可能提前释放捕获对象。后台线程最终执行 `wait()`，不能替代
> Service 销毁前的同步。

**要求**：恢复期间仍保留旧 worker 的受控所有权，启动有界拒绝且不入队；shutdown/析构必须
覆盖所有未退出 worker；补充「拒绝启动后立即 shutdown」的回归。

### 15.2 缺陷机理（静态事实）

| 事实 | 位置（`152a4a7`） | 结论 |
|---|---|---|
| 预算内未退出时的处置 | `retireWorkerThreadBounded()` `:145` `std::thread([thread]{ wait(); delete; }).detach()` | 等待者被挪到后台，**指针不再被任何 Service 侧结构持有** |
| `m_thread` 状态 | `enqueue()` `:209` 已在锁内置空 | 该 worker 在 Service 视角「已不存在」 |
| `shutdown()` Idle 分支 | `:423` 只取 `thread = m_thread` | 漏等 retiring worker，立即返回 `true` |
| `~FlowExecutionService()` | `:114` 同样只取 `m_thread` | 析构可能在仍在 `run()` 的 worker 之下销毁其访问的成员与模块 |

「detach + 后台 wait」只解决了**内存回收**，没有解决**生命周期同步**。`shutdown()` 返回
`true` 的语义是「可以安全销毁 worker 所访问的对象」，而后台线程的 `wait()` 发生在这个
时点之后，无法履行该语义。

### 15.3 缺陷复现（独立探针，先于修复）

**这是本轮最重要的证据纪律**：探针通过**现有测试可执行文件**运行新用例，被测代码路径由
`barrierEntries` 显式证明「真的被执行」，而不是只比较某个中间值。

执行位置：隔离工作树 `/var/tmp/t25p1-owner-wt`（`--detach` 于 `152a4a7`）+ 构建目录
`/var/tmp/t25p1-owner-b3`。**交付工作树全程未切换缺陷态。**

- `CONFIGURE_rc=0`（11m21s，`t25p1-owner-b2` 目录，源码同工作树）
- `BUILD_rc=0`（全量）
- 缺陷态切换方式：`git stash push -- src/core/engine/FlowExecutionService.{cpp,h}`（仅隔离工作树）
- 缺陷态确认：`grep -c "detach();" → 1`（`:148`）、`grep -c "m_retiringThreads" → 0`

探针时序（全部有界，不靠 sleep 撞窗口）：

1. `setWorkerLoopFaultHookForTest()` 注入一次 `workerLoop` 故障 → `jobFailed` 发出；
2. `setWorkerExitBarrierForTest()` 把旧 worker 停在「通知已发出、`run()` 未返回」的窗口正中，
   并在屏障内把 `oldWorkerRunning` 置 `true`；
3. 主线程在该窗口内 `startSingle()` 重试 → 100ms 预算内等不到旧 worker → **明确拒绝**；
4. 断言「拒绝后立即 `shutdown()`」在当前 worker 退出前不得返回 `true`：
   在 `shutdown()` 返回**那一刻**采样 `oldWorkerRunning`。

| 状态 | 运行次数 | `rc` | `barrierEntries` | `shutdownRounds` | `earlyReturns` |
|---|---|---|---|---|---|
| 撤销态（`152a4a7`） | 5 | **1 ×5** | 3/3 | 3/3 | **3/3** |
| 修复态 | 5 | **0 ×5** | 3/3 | 3/3 | **0** |

缺陷态现场（`defectv4-run-1.log`），与评审探针**数值一致**：

```
[P1-OWNER] round 0: shutdown returned true after 0 ms while the retired worker was still running
[P1-OWNER] round 1: shutdown returned true after 0 ms while the retired worker was still running
[P1-OWNER] round 2: shutdown returned true after 0 ms while the retired worker was still running
[P1-OWNER] rounds=3 barrierEntries=3 shutdownRounds=3 earlyReturns=3 notified=3
FAIL!  : ... 'earlyReturns.load() == 0' returned FALSE.
```

`shutdownMs=0` 对应评审的 `shutdownMs=0`，`earlyReturns=3/3` 对应 `oldWorkerStillRunning=1`。

### 15.4 修复方案

**受控所有权**：`retireWorkerThreadBounded()` 不再 detach，把指针登记到服务自持容器。

| 改动 | 位置 | 作用 |
|---|---|---|
| 新增 `QList<WorkerThread*> m_retiringThreads`（`m_mutex` 保护） | `FlowExecutionService.h` | 旧 worker 的所有权留在 Service 手上 |
| `retireWorkerThreadBounded()` 改为 `m_retiringThreads.append(thread)`，仍返回 `false` | `.cpp:157` | 启动侧行为不变：有界、明确拒绝、不入队 |
| 新增 `detachLiveWorkerThreadsLocked()` | `.cpp:161` | 锁内把 `m_thread` + 全部 retiring 摘出，锁外 join |
| `~FlowExecutionService()` 改为遍历 retiring 列表 | `.cpp:105-122` | 析构覆盖所有未退出 worker |
| `shutdown()` Idle 分支改为遍历 retiring 列表 | `.cpp:491` | **本轮 P1 的直接修复点** |
| `enqueue()` 只对**已结束**的 retiring 做零等待 `delete` | `.cpp:236-262` | 见 15.5 |

`joinWorkerThread()` 保持原样：仍只在 `shutdown()`/析构使用，允许长期等待、绝不 detach。

### 15.5 过程中发现并修正的两个自身问题（保留现场）

修复第一版（`enqueue()` 在启动预算内等待 retiring 线程）导致上一轮窗口用例回归：

```
[P1-WINDOW] barrierEntries=6 windowRetries=6 retryOk=0 retryFailed=6 restartOk=2
FAIL!  : ... 'restartOk.load() >= 5' returned FALSE. (restartOk=2, rounds=10)
```

两个原因都必须记录，否则会被误读成「新修复引入回归」：

1. **重复登记**：清理阶段用 `retireWorkerThreadBounded()` 回收「已在 retiring 里」的线程，
   预算用尽时又把它 `append` 一次，同一指针出现两次 → 后续可能二次 `delete`。
   改为「已结束直接 delete，其余原样保留所有权、不重复登记」。
2. **把 retiring 线程计入下一次启动的预算**：这是更要紧的语义错误。retiring 线程已不再
   消费命令，**退出与否不决定本次启动能否成功**；把它算进预算会形成链式拒绝——前一次
   预算耗尽留下一个 retiring 线程，下一次启动又在它身上耗掉整份预算。这与验收口径⑤
   「旧线程退出后再次启动必须成功、无需手动 shutdown()」直接冲突。

修正后的口径：`enqueue()` 对 retiring 列表只做**零等待**清理，等待一律留给
`shutdown()`/析构。修正后同一用例恢复为 `barrierEntries=10 windowRetries=10 restartOk=10`，
服务套件 **28/28、`rc=0`**。

### 15.6 反向验证（判别性）

| 验证 | 结果 |
|---|---|
| 撤销态（只回滚生产修复，保留用例） | **5/5 `rc=1`**，失败于 `earlyReturns.load() == 0`（`earlyReturns=3/3`） |
| 修复态 | **5/5 `rc=0`**，`earlyReturns=0`，`barrierEntries=3`（路径确实执行） |
| 服务套件（修复态） | **28/28、`rc=0`**（23400ms） |
| 上一轮窗口用例 | `barrierEntries=10 / windowRetries=10 / restartOk=10`，判据①②③④⑤ 全绿 |

**判别性说明**：不变量「撤销态仍全绿」在此未出现——撤销态稳定 5/5 红灯。且判据以
`barrierEntries` 正向前提保证被测路径确实执行（第 13.8 节 T2.9 教训），不是「测了未执行的代码」。

**用例耗时**：`testShutdownCoversWorkerHandedOffByRejectedStart` 修复态约 6.6s（3 轮）。
首版因控制线程用固定等待窗口，曾达 15s；现改为「控制线程仅在 `shutdown()` 返回后放行，
兜底 10s」，正常路径无固定延迟。

### 15.7 全量门禁

| 项 | 值 |
|---|---|
| 源提交 | `f2a162b`（test）/ `17bd15b`（fix） |
| 构建目录 | `/var/tmp/t25p1-gate`（**工作区外全新目录**） |
| 配置 | `CONFIGURE_rc=0`（Generate 阶段 559.2s） |
| 构建 | `BUILD_rc=0`（→ `[100%] Built target test_mainwindow`） |
| 格式 / 差异 | `FMT_rc=0`（3 个文件 `--dry-run --Werror`）、`DIFFCHECK_rc=0` |
| CTest 列举 | `Total Tests: 97`、`LISTED=97` |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time (real) = 249.84 sec`） |
| 相关套件 | `test_flowexecutionservice` **28/28**、`test_runengine` **119/119**、`test_modulebase` **16/16**、`test_runenginejoblease` 8/8、`test_runengineuserpause` 6/6、`test_runenginefinalizer` 6/6（全部 `rc=0`） |

日志：`/var/tmp/t25p1-gate-{configure,build2,list,ctest}.log`、`/var/tmp/t25p1-suite-test_*.log`。
门禁时点：交付工作树 `HEAD=17bd15b`，源码文件 `git status` 为空（第 15 章文档改动在门禁之后提交）。

**仍为 97 项**：本轮新增 1 条用例落在**已有**可执行文件 `test_flowexecutionservice` 内
（27 → 28），未新增 CTest 目标。「97 项」不等于没有新测试，汇报须同时给套件内用例数。

**两个环境陷阱（本轮实测，均非代码问题）**：

1. **`/tmp` 只有 10M tmpfs**：全量构建中途出现
   `fatal error: error writing to /tmp/ccXXXXXX.s: No space left on device`，连带
   `test_runengine`/`test_fitellipse`/`test_flowexecutionservice` 编译失败（`BUILD_rc=2`），
   但**不是**任何源码错误。解决：`TMPDIR=/var/tmp/gcc-tmp` 重新构建 → `BUILD2_rc=0`，零空间错误。
   **后续任何全量构建都应先设 `TMPDIR` 到大容量目录。**
2. **CMake Generate 阶段的沙箱 I/O 干扰**：另一独立探针目录首次配置以 `CONFIGURE_rc=1` 失败于
   `CMake Error: Cannot open file for write: …/NPointCalibration/Makefile.tmp7082b`
   + `System Error: Inappropriate ioctl for device`；同一批残缺 Makefile 还会导致
   `No rule to make target … progress.make`（`BUILD_rc=2`）。均以换全新目录名重跑解决，
   **不携带非零退出码送审**。

### 15.8 状态

| 项 | 状态 |
|---|---|
| P1（后台回收失去生命周期管理） | **已复现（`shutdownMs=0` + `oldWorkerStillRunning`）、已修复、撤销态 5/5 红灯**，性质为真实缺陷 |
| 第 15 章修复的复核结论 | **通过**（2026-11-11 第三段复核：「拒绝启动后立即 shutdown」已被覆盖并验证） |
| 但第 15 章修复遗留更早的窗口 | 见**第 16 章**（在途回收的所有权不可见窗口），本轮 P1 |
| T2.5 / T2.8 / T2.9 | **暂不收口**（按复核要求，不自行宣告） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成**（十轮连续门禁、`git revert` 抽查留待整批） |

## 16. 有界回收期间旧 worker 的所有权不可见窗口（2026-11-11 第四段复核 P1）

### 16.1 裁定的问题

复核至 `a688718` 认定第 15 章的修复**通过**——「拒绝启动后立即 shutdown」已被覆盖并验证。
但**新发现一个更早的窗口**，因此仍不能收口：

> `FlowExecutionService.cpp:273` 先清空 `m_thread`，随后锁外等待；直到等待超时，`:157` 才
> 登记进 retiring 列表。这段等待期间，并发 `shutdown()` 仍会漏掉旧 worker。
> 独立同步探针 3/3 复现：`shutdownOk=1` / `returnedBeforeWaitReleased=1` /
> `oldWorkerRunningAtReturn=1`。
> 现有回归只覆盖「启动拒绝返回后再 shutdown」，没有覆盖「回收等待中并发 shutdown」。

**要求**：进入锁外等待前，就建立 Service 可见的在途回收状态；与 `shutdown()` 协调唯一回收者，
保证不漏等、不重复删除，同时保持启动预算。补对应屏障回归。

### 16.2 缺陷机理（静态事实，`a688718`）

第 15 章把「预算内未退出」的线程登记进 `m_retiringThreads`，但登记发生在**锁外等待之后**：

| 时序 | 位置（`a688718`） | 此刻该 worker 存在于 |
|---|---|---|
| ① 锁内摘除 | `enqueue()` `:271-274` `retired = m_thread; m_thread = nullptr;` | 仅调用线程的栈局部 `retired` |
| ② 释放锁，锁外等待 | `retireWorkerThreadBounded()` `:128` `thread->wait(budgetMs)` | 仍只有栈局部（**`m_thread` 与 `m_retiringThreads` 都为空**） |
| ③ 等待结束、未退出，登记 | `retireWorkerThreadBounded()` `:157` `m_retiringThreads.append(thread)` | 此时才回到 Service 可见结构 |

在 ②③ 之间的整段等待里，Service 的任何结构都不持有该 worker。并发 `shutdown()` 走到
`:491` 的 `detachLiveWorkerThreadsLocked()`，锁内看到 `m_thread == nullptr`、
`m_retiringThreads` 为空，于是**立刻** `return true`——而那条 worker 仍在 `run()` 内。

这与第 15 章修的是**两段不同的时间窗**：第 15 章覆盖的是「回收已失败返回、线程已在 retiring」，
本章覆盖的是「回收正在进行、尚未有结论」。后者更早，也更狭窄，因此需要把可见性前移到
**摘除的同一次加锁内**。

### 16.3 缺陷复现（判别性证据来自本轮新增用例）

评审的独立同步探针目录 `/tmp/deeplux-t25-owner-recheck.Z62LiJ/` **已不存在**
（`/tmp` 为 10M tmpfs，见 15.7）。因此本轮复现**不依赖该目录**，改用可复跑的判据：
新增用例 `testShutdownDuringInflightReclaimMustNotReportSuccess()` 在撤销态下稳定红灯，
其输出与评审探针数值一一对应。

| 评审探针字段 | 本轮用例对应字段 | 撤销态实测 |
|---|---|---|
| `shutdownOk=1` | `shutdownOk` | 3/3 返回 `true` |
| `returnedBeforeWaitReleased=1` | `shutdownMs=0`（返回那一刻回收等待仍未结束） | `shutdown returned true after 0 ms` |
| `oldWorkerRunningAtReturn=1` | `stillRunning \|\| retryStillRunning` → `earlyReturns` | `earlyReturns=3/3` |
| — | `inflightRounds`（正向前提：确实进入过回收等待） | `3` |

执行位置：隔离工作树 `/var/tmp/t25r4-wt`（`--detach` 于 `a688718`）+ 构建目录 `/var/tmp/t25r4-b1`。
**交付工作树全程未切换缺陷态**（缺陷态切换只在 `t25r4-wt` 内以 `git checkout` 回滚 `src/` 两个文件实现）。

撤销态现场（`defect-run-1.log`，与 `defect-run-2/3` 逐字一致）：

```
[P1-INFLIGHT] round 0: shutdown returned true after 0 ms while the retired worker was still running
[P1-INFLIGHT] round 1: shutdown returned true after 0 ms while the retired worker was still running
[P1-INFLIGHT] round 2: shutdown returned true after 0 ms while the retired worker was still running
[P1-INFLIGHT] rounds=3 inflightRounds=3 earlyReturns=3 notified=3
FAIL!  : ... 'earlyReturns.load() == 0' returned FALSE.
```

### 16.4 回归手法（为什么必须换一种写法）

第 15 章的 `testShutdownCoversWorkerHandedOffByRejectedStart` **无法**覆盖本窗口：它在
`startSingle()` **已经返回**（即回收已超时并登记完毕）之后才调用 `shutdown()`，此时线程
早已在 `m_retiringThreads` 里，构造不出 ②③ 之间的窗口。

本轮用例的三点关键设计：

| 设计 | 原因 |
|---|---|
| `startSingle()` 放在独立 `std::thread starter` 上发起 | 回收（含锁外 `wait()`）发生在 starter 线程内，主线程才能**并发**调用 `shutdown()` |
| 正向前提 `barrierEntered && !retryReturned` 轮询 + 短促停留 | 证明「此刻确实处于回收等待中」；不靠固定 sleep 撞窗口，判据缺失即视为无判别力 |
| 判据 = `shutdownOk && (stillRunning \|\| retryStillRunning)` | 在 `shutdown()` 返回**那一刻**采样 worker 是否仍在跑，与「返回过快」直接对应 |

`starter` 通过 `ServiceContractCleanupGuard` 的新增第 5 参数（`std::thread*`）纳入兜底 join，
避免用例失败时线程泄漏影响后续用例。

### 16.5 修复方案

**核心原则**：摘除与「在途回收」标记必须在**同一次加锁**内完成；所有权判定必须在锁内按
「是否仍在 inflight 列表」进行，而不是按等待前的 `m_reclaimAborted` 快照。

| 改动 | 位置 | 作用 |
|---|---|---|
| 新增 `QList<WorkerThread*> m_inflightReclaim` | `.h` | 正在被有界回收（已摘出 `m_thread`、正锁外等待）的 worker，Service 可见 |
| 新增 `QWaitCondition m_reclaimWake` | `.h` | 与 `m_wake` 分开，避免 job 通知误唤醒/误判 |
| 新增 `bool m_reclaimAborted` | `.h` | 有线程正在 `shutdown()`/析构中收归 worker，回收线程须让出所有权 |
| 新增 `beginInflightReclaimLocked()` | `.cpp:181` | 在摘除的同一次加锁内登记，**窗口消除** |
| `enqueue()` 回收循环改为「锁内摘除 + 标记（`:320-331`）→ 锁外有界等待」 | `.cpp:297-352` | 见下 |
| `retireWorkerThreadBounded()` 改为锁内 `removeOne` 判所有权 | `.cpp:139` | 唯一回收者判定，避免 use-after-free |
| `detachLiveWorkerThreadsLocked()` 改为 `int`，锁内搬运 `m_inflightReclaim` | `.cpp:195-203` | shutdown 立即接管在途线程 |
| `shutdown()` 收尾后复位 `m_reclaimAborted` | `.cpp:567-568` | shutdown 后 Service 恢复可用 |
| `enqueue()` 入口 `m_reclaimAborted` 即拒绝 | `.cpp:320` | 不与 shutdown 同时 join/delete |
| retiring 零等待清理加 `!m_reclaimAborted` 保护 | `.cpp:297` | 避免与 shutdown 各删一次 |

**唯一回收者判定（本轮最容易写错的地方）**：

```cpp
const bool finished = thread->wait(static_cast<unsigned long>(qMax(0, budgetMs)));
bool owned = false;
{
    QMutexLocker locker(&m_mutex);
    owned = m_inflightReclaim.removeOne(thread);   // 锁内判定，不能用品等待前的快照
    if (owned && !finished) { m_retiringThreads.append(thread); }
    m_reclaimWake.wakeAll();
}
if (!owned) { return false; }   // shutdown 已接管：不 delete、不登记、不再触碰指针
if (finished) { delete thread; return true; }
```

若改用「等待前采样 `m_reclaimAborted`」做判定：等待期间 `shutdown()` 可能已经把这条线程
join 完并 `delete`，回收线程再碰它就是 **use-after-free**。`removeOne` 的结果天然携带
「谁删」的信息，因此不需要额外的引用计数。

**预算耗尽的特例**：`reclaimBudget <= 0` 时必须先在锁内 `m_inflightReclaim.removeOne(retired)`
再 `m_retiringThreads.append(retired)`，否则同一条线程同时挂在两个列表里，会被 join 两次、
delete 两次。

### 16.6 过程中修正的自身设计问题（保留现场）

首版方案设计了 `waitForNoInflightReclaimLocked()`——置中止位后**等待**回收线程让出所有权。
这是错的：回收线程此刻正阻塞在 `thread->wait()` 内，不会立即让出，该等待会被拖到 5s 上限，
**等于把启动预算换个地方又花掉**（与第 14 章修的 P1 同类）。

改为：`detachLiveWorkerThreadsLocked()` 在**锁内直接把 `m_inflightReclaim` 搬进
`m_retiringThreads`**（所有权立即转交），回收线程醒来后 `removeOne` 失败即放弃。同时删除
`waitForNoInflightReclaimLocked()` 与 `endInflightReclaimLocked()`，保持实现最小。
**教训**：凡是「为了拿所有权而等待」的设计，都要先问清楚对方此刻是否正阻塞在别处。

#### 16.6.1 更正（2026-11-11 第五段复核裁定）：生命周期等待 ≠ 启动预算

本节初稿曾把两件事混为一谈，按复核意见更正——

- **启动预算 `StartHandshakeTimeoutMs`（100ms）**：语义是「GUI 多久之内必须拿到一个明确
  答复（成功启动 / 明确拒绝）」。它**必须有界**，超时就必须拒绝，不得把 GUI 拖住。
- **生命周期等待（`waitForReclaimBorrowsReturned()`、`joinWorkerThread()` 的 `thread->wait()`）**：
  语义是「这个对象什么时候才真的可以销毁」。它**允许长期无界等待**，因为它的正确性判据是
  「无人再使用该对象」，而不是「多久之内答复」。

两者共用同一个数字会造成一个必然会犯的错误取舍：**为了不超预算，而在仍有回收调用正在
使用 worker 时放行 `shutdown()` 并删除它**。第五段复核正是抓到了这个取舍——当时 `shutdown()`
在 `FlowExecutionService.cpp:128` 的 `thread->wait()` 仍在进行时就接管了删除权，评审探针
3/3 观察到 `shutdownOk=1` / `shutdownBeforeReclaimReturned=1` / `workerDeletedWhileWaitActive=1`。

因此把两者的适用边界写死如下（**本条只澄清 16.6 自身未区分二者的问题，不引用任何
此前不存在的原文**）：

- 第 14 章的 `StartHandshakeTimeoutMs`（100ms）**只**约束 `enqueue()` 启动路径上的
  有界回收 + 握手，二者共用同一个 `QElapsedTimer`（见 16.5 表格与第 14 章）。
- `shutdown()` / 析构对活跃回收借用的等待**不属于该预算**，它与 `joinWorkerThread()`
  一样属于生命周期同步，允许无界等待。
- 混用的后果是可预期的错误取舍：**为了不超预算，而在仍有回收调用正在使用 worker 时
  放行 `shutdown()` 并删除它**。第五段复核抓到的正是这一点——`shutdown()` 在
  `FlowExecutionService.cpp:128` 的 `thread->wait()` 仍在进行时就接管了删除权，
  评审探针 3/3 观察到 `shutdownOk=1` / `shutdownBeforeReclaimReturned=1` /
  `workerDeletedWhileWaitActive=1`。

第 17 章按此实现 `waitForReclaimBorrowsReturned()` 的无界等待。

### 16.7 反向验证（判别性）

| 验证 | 结果 |
|---|---|
| 撤销态（只回滚生产修复，保留用例） | **3/3 `rc=1`**，失败于 `earlyReturns.load() == 0`（`inflightRounds=3` / `earlyReturns=3`） |
| 修复态 | **5/5 `rc=0`**，`inflightRounds=3`、`earlyReturns=0`（路径确实执行） |
| 服务套件（修复态） | **29/29、`rc=0`**（40553ms） |
| 既有 `P1-WINDOW`（`testRetryDuringWorkerRetirementIsBoundedAndNotDeferred`） | `barrierEntries=10 windowRetries=10 retryFailed=10 restartOk=10`，无回归 |
| 既有 `P1-OWNER`（`testShutdownCoversWorkerHandedOffByRejectedStart`） | `barrierEntries=3 shutdownRounds=3 earlyReturns=0`，无回归 |

**判别性说明**：撤销态未出现「仍全绿」，故不适用第 12/13 章的「无判别力」判定；且
`inflightRounds=3` 作为正向前提，证明被测路径（锁外回收等待）确实执行过，不是「测了未执行的代码」。

### 16.8 格式与差异检查

| 项 | 结果 |
|---|---|
| `clang-format --dry-run --Werror`（3 个文件） | `FMT_rc=0` |
| 补丁与隔离工作树逐字节一致 | `git diff` sha256 `cff95d07…` 两侧相同 |
| 交付树源码改动指纹 | `src/` `1a918f97…`、`tests/` `d56b999d…` |

**现场保留**：应用补丁后首次 `clang-format --dry-run --Werror` 曾报 `FMT_rc=1`
（`tests/test_flowexecutionservice.cpp:1820`、`:1821`、`:1942`）。执行 `clang-format -i`
后 `FMT_rc=0`、`DIFFCHECK_rc=0`。**格式化改变了源码字节，因此格式化之后重新跑了一遍全量门禁**，
不复用格式化前的构建结果。

### 16.9 全量门禁

| 项 | 值 |
|---|---|
| 构建目录 | `/var/tmp/t25r4-gate`（**工作区外全新目录**） |
| 配置 | `CONFIGURE_rc=0`（`Configuring done (15.4s)`，Generate 阶段完整通过） |
| 构建 | `BUILD_rc=0`（36m47s；前置 `TMPDIR=/var/tmp/gcc-tmp`，见 15.7 陷阱 1） |
| 格式 / 差异 | `FMT_rc=0`（clang-format 14.0.0，3 个文件 `--dry-run --Werror`）、`DIFFCHECK_rc=0`（`git diff --check`） |
| CTest 列举 | `Total Tests: 97`、`LISTED=97` |
| **CTest 全量** | **`100% tests passed, 0 tests failed out of 97`**、`CTEST_rc=0`（`Total Test time (real) = 177.48 sec`） |
| 相关套件 | `test_flowexecutionservice` **29/29**（44754ms）、`test_runengine` **119/119**（8231ms）、`test_modulebase` **16/16**、`test_runenginejoblease` 8/8、`test_runengineuserpause` 6/6、`test_runenginefinalizer` 6/6（全部 `rc=0`） |

日志：`/var/tmp/t25r4-gate-{configure,build,list,ctest}.log`、`/var/tmp/t25r4-suite-*.log`、
用例判别性日志 `/var/tmp/t25r4-ev/{defect,fix}-run-*.log`。
门禁时点：交付工作树 `HEAD=a688718` + 3 个未提交文件（`src/` 两文件与 `tests/`），
即**第 16 章文档改动在门禁之后单独提交**，门禁所构建的源码与送审源码逐字节一致（`cff95d07…`）。

**仍为 97 项**：本轮新增用例落在**已有**可执行文件 `test_flowexecutionservice` 内
（28 → 29），未新增 CTest 目标。「97 项」不等于没有新测试，汇报须同时给套件内用例数。

### 16.10 状态

| 项 | 状态 |
|---|---|
| P1（在途回收的所有权不可见窗口） | **已复现（撤销态 3/3 红灯，与评审探针三字段一一对应）、已修复、修复态 5/5 绿 + 套件 29/29**，性质为真实缺陷 |
| 第 15 章 P1 | 复核已通过（本轮不改动其结论） |
| 验收口径 | ①–⑦ 全部满足，其中 ⑦「回收的锁外等待期间并发 `shutdown()` 也不得提前返回」为本轮新增 |
| T2.5 / T2.8 / T2.9 | **暂不收口**（按复核要求，不自行宣告） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成**（十轮连续门禁、`git revert` 抽查留待整批） |

---

## 17. T2.5 P1 第七次整改：回收借用必须在「移交指针」的同一临界区内登记

### 17.1 复核提出的问题（第六段）

第 16 章把「在途标记」提前到了 `enqueue()` 的锁内，使 `shutdown()` **看得见**正在被回收的
worker。但「看得见」不等于「知道有人正在用它」——借用计数虽然加了，却**登记得太晚**：

```
enqueue()：
  { QMutexLocker locker(&m_mutex);
    retired = m_thread;
    m_thread = nullptr;                      // ① 指针移交
    beginInflightReclaimLocked(retired); }   // ② 在途可见 —— 解锁
  ...
retireWorkerThreadBounded(retired, ...)：
  ReclaimBorrow borrow(this, retired, ...);  // ③ 此刻才登记借用
  thread->wait(...);                         // ④ 才使用对象
```

`shutdown()` 若插在 ② 与 ③ 之间，它读到的借用数为零 → `waitForReclaimBorrowsReturned()`
立即返回 → 接管所有权并 `delete`；随后 ③ 才登记、④ 才 `thread->wait()`，
**访问已释放对象**。评审给出的位置是 `FlowExecutionService.cpp:413`（移交）与 `:191`（使用）。

评审同时指出新用例的三处缺陷：

1. 钩子（`tests/test_flowexecutionservice.cpp:2034`）位于**借用登记之后**，覆盖不到上述空档；
2. 测试**同步**调用 `shutdown()` 时，回收钩子要等最多 5 秒才自行放行，正常修复路径被拖住；
3. 判据（`:2137`、`:2141`）观察**整个 `startSingle()` 是否返回**，而不是**借用何时归还**，
   可能把正确实现误判为失败。

### 17.2 修复

**① 借用登记移进同一个临界区。** 新增 `registerReclaimBorrowLocked(WorkerThread*)`，
调用方必须持 `m_mutex`；`enqueue()` 在同一次加锁内按序完成三件事：

```cpp
retired = m_thread;
m_thread = nullptr;                     // 指针移交
beginInflightReclaimLocked(retired);    // 在途可见
registerReclaimBorrowLocked(retired);   // 借用随即可见 —— 三件事同一临界区
```

**② RAII 守卫改为「接管」而非「新建」。** `ReclaimBorrow` 增加
`enum class Registration { AdoptAlreadyRegistered, RegisterNow }`：
启动路径传 `AdoptAlreadyRegistered`（只接管已登记的借用，不再登记）；
`borrowRegistered == false` 时回退 `RegisterNow`，并以 `Q_ASSERT_X` 显式声明该前提。

**③ 删除前必须等借用归还。** `waitForReclaimBorrowsReturned()` 插入两处：
`joinWorkerThread()` 开头（真正删除之前）与 `shutdown()` 在
`detachLiveWorkerThreadsLocked()` 之后、`joinWorkerThread()` 循环之前。
语义是「等回收**函数返回**」，不是「等 worker 退出」——两者并不同时发生。

**④ 生命周期等待与启动预算彻底分开**（见 16.6.1 更正）：`waitForReclaimBorrowsReturned()`
无界等待，不复用 `StartHandshakeTimeoutMs`(100ms)。

### 17.3 测试判据的重做（三处缺陷逐一对应）

| 评审指出的缺陷 | 本轮的改法 |
|---|---|
| 钩子在借用登记之后，覆盖不到空档 | 新增 `setInCriticalSectionHookForTest()`，**在 `enqueue()` 的临界区之内、持锁调用**；同时新增 `inflightWithoutBorrowCountForTest()`（锁内扫描 `m_inflightReclaim`，其中每条线程都必须已有 ≥1 笔借用） |
| 同步 `shutdown()` 被回收钩子拖住 5 秒 | `shutdown()` 改到独立 `std::thread` 上发起，主线程按**状态**等待（≤300ms，确认它没有提前返回）后放行钩子；实测单轮耗时从 5.3s 降到 458ms 量级，5 次全量运行共 38s |
| 判据观察 `startSingle()` 是否返回 | 判据改为**采样 `shutdown()` 返回那一刻的借用/守卫状态**，完全不看 `startSingle()` 的返回值 |

### 17.4 过程中被自己否掉的两个无效判据（保留现场）

**无效判据 A：`reclaimStillActive`（回收线程尚未从 `startSingle()` 返回）。**
缺陷态 3/3 仍绿。原因：`startSingle()` 在回收之后还要走握手，天然会晚一点返回，
这条判据对「借用是否已归还」不敏感。

**无效判据 B：`activeReclaimBorrowCountForTest() > 0`（计数表非零）。**
缺陷态仍绿，原因更本质：**计数表就是 `shutdown()` 的等待条件**。正确的 `shutdown()`
在 `waitForReclaimBorrowsReturned()` 之后才继续，所以它返回时计数表必然为 0；
缺陷态的 `shutdown()` 走「非 Idle 早退」分支时不删任何东西，计数表同样可能是 0。
于是「计数表 > 0」在两种实现下**都**读不到，判据没有判别力。

**有效判据：`liveReclaimGuardCountForTest() > 0`（借用守卫是否仍在作用域内）。**
它与 `shutdown()` 的等待没有耦合假设：缺陷态下 `shutdown()` 不等就返回，回收守卫
仍停在钩子上（作用域内）→ 必然 > 0。实现上是 `ReclaimBorrow` 的一个
`std::atomic_bool m_alive` + `.cpp` 匿名空间里的全局原子计数，**不新增类数据成员**
（遵守测试钩子纪律），构造时在借用生效**之后**置位、析构时在归还**之前**撤销。

同时把缺陷判据**从「`shutdownOk && 借用>0`」改为「`shutdown` 已返回即算」**：缺陷态的
`shutdown()` 在回收活跃时会走非 Idle 早退并返回 **false**，把判据挂在 `shutdownOk` 上
会漏掉这条最典型的缺陷路径。

> **更正（2026-11-12 第六段复核 P2，第 18.4 节）**：上面这条改法是**错的**，已撤回。
> 既定契约里 `shutdown()` 返回 `false` 只表示「非 Idle：已请求停止，未销毁任何对象」，
> 这种情形下**允许**仍有回收借用、也允许 worker 还在跑——它们本来就该留着。
> 把「返回 `false` 且仍有借用」算作缺陷，等于把 `false` 的语义扩大成「也已安全关闭」。
> 正确做法是**让用例进入成功关闭路径**（首启不入队任何 job、服务处于 Idle），
> 判据只挂在 `shutdownOk == true` 上，并以「至少一轮真的走了成功关闭路径」为正向前提。
> 详见第 18 章。

### 17.5 缺陷态的构造（隔离工作树，不在交付树）

位置：`/var/tmp/t25r5-wt`（`git worktree` **detach 于 `199bbfb`**）、
构建目录 `/var/tmp/t25r5-b1`。缺陷态**从交付树源码用脚本派生**，只动三处：

1. `enqueue()` 去掉 `registerReclaimBorrowLocked(retired)`，改 `borrowRegistered = false;`
   （即第六次整改原貌：借用登记推迟到 `retireWorkerThreadBounded()` 内）；
2. `retireWorkerThreadBounded()` 把 `runReclaimWaitHookForTest()` 移到 `ReclaimBorrow`
   构造**之前**，使钩子正好落在「指针已移交、借用未登记」的窗口正中；
3. 关闭 `Q_ASSERT_X(borrowRegistered, ...)`——该断言的前提正是本变体要撤销的那条不变量。

> 现场保留：派生脚本第一次跑时锚点匹配失败（`AssertionError: hook block not found`），
> 是因为锚点文本中间夹了一行注释；随后又出现「钩子被调用两次 → 第一处等 5s、
> 主线程永久 join」的挂死（`rc=124`）。都已逐个定位修正，最终缺陷态三处标记
> 与交付树 diff 逐字节可核对。

### 17.6 反向验证（判别性）

| 验证 | 结果 |
|---|---|
| **撤销态**（借用登记推迟到 `retireWorkerThreadBounded()` 内） | **5/5 `rc=1`**，全部失败于 `invariantViolations.load() == 0`（`violations=3`） |
| **修复态**（借用登记与指针移交同临界区） | **5/5 `rc=0`**，`windowHits=3`、`earlyDeletes=0`、`invariantViolations=0` |
| 服务套件（修复态，格式化前） | **30/30、`SUITE_rc=0`**（31919ms） |
| 正向前提 | `windowHits=3`：确实进入过「移交指针 + 登记借用」的同一临界区，证明被测路径执行过 |

**判别性说明**：撤销态未出现「仍全绿」，故不适用第 12/13 章的「无判别力」判定。
`windowHits=3` 作为正向前提，排除了「测了未执行的代码」。

日志：缺陷态 `/var/tmp/t25r5-defect-final-{1..5}.log`、
修复态 `/var/tmp/t25r5-fix-final-{1..5}.log`、套件 `/var/tmp/t25r5-suite-fix.log`。

### 17.7 状态

| 项 | 状态 |
|---|---|
| P1（借用登记晚于指针移交） | **已复现（撤销态 5/5 红灯）、已修复、修复态 5/5 绿**，性质为真实缺陷 |
| 第 16 章 P1 | 本轮整改覆盖其「删除前未等借用归还」的一半；**第 16 章结论需随本章一并复审** |
| T2.5 / T2.8 / T2.9 | **暂不收口**（按复核要求，不自行宣告） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成**（十轮连续门禁、`git revert` 抽查留待整批） |

---

## 18. T2.5 P1 第八次整改：关闭期间禁止启动 + 用例判别性重做

基线 `199bbfb`（第 17 章结论）。第六段复核给出 1 项 P1、2 项 P2，本章逐条对应。

### 18.1 P1：shutdown 期间仍能启动新任务（真实缺陷）

**机理。** `shutdown()` 的 Idle 路径置 `m_quitRequested = true`，但 `enqueue()` 的最终入队
调用 `ensureThreadLocked()`，后者把 `m_quitRequested` **复位成 false**——那是为
「shutdown 之后可以重新启动」准备的。于是关闭期间到达的启动请求照样重建 worker、
照样执行任务。另一道 `m_reclaimAborted` 只在 `detachLiveWorkerThreadsLocked()` 发现
**存在在途回收**时才置位，普通 Idle 关闭根本不置位，`enqueue()` 入口的检查形同虚设。

复核实测 3/3：`shutdown=true` 但新任务在跑、引擎仍忙。同一现象在上一轮构建也能复现，
因此是**既有缺口**，不是本轮新引入。

**修复（四步）。**

| 步骤 | 位置 | 内容 |
|---|---|---|
| 1 | `FlowExecutionService.h:311` | 新增独立标记 `bool m_shutdownInProgress = false`（**不**复用 `m_reclaimAborted`：后者语义是「让回收线程让出所有权」，与「服务正在关闭」不是一回事） |
| 2 | `shutdown()` 入口 `:775` | 在锁内**一开始**就置位，覆盖所有关闭路径（Idle 与非 Idle） |
| 3 | 非 Idle 分支 `:800` | **撤销标记**。该分支按既定契约「只请求停止并返回 false」，不销毁任何对象；把标记留在置位状态会把 false 的语义扩大成「服务已关闭」，使后续正常启动被永久拒绝 |
| 4 | 成功关闭结尾 `:821` + `enqueue()` 入口 `:454` + **最终入队临界区 `:526`** | 关闭完成时清除；入口与最终入队各检查一次。**两处都要**：入口检查发生在回收之前，之后 shutdown 可能已完成收尾并返回；最终入队才是「命令即将交给 worker」的最后一刻 |

### 18.2 P2：用例放行点耦合，掩盖了「不等待借用就删除」

**评审手法。** 只把「等待借用归还」的助手替换为空操作，交付测试仍 **3/3 通过**；
独立生命周期探针则 3/3 检出提前删除。

**根因。** 旧用例把「放行旧 worker」写在 starter 线程里 `startSingle()` **之后**：

```cpp
starter = std::thread([&] {
    (void) service.startSingle();
    reclaimReturned.store(true, ...);
    holdBarrier.store(false, ...);   // <-- 放行屏障
    releaseBarrier.release();
});
```

这隐含了「`startSingle()` 已返回 => worker 已退出」这一**测试自己制造的**顺序，
于是借用必然先归还、缺陷无从显现。

**整改。** 两个控制点彻底解耦，都不由 `startSingle()` 的返回驱动：

| 控制点 | 控制对象 | 由谁放行 |
|---|---|---|
| A `releaseReclaimHook` | 回收调用是否归还借用 | 主线程，在 shutdown 已确认停在窗口内之后 |
| B `releaseBarrier` | 旧 worker 是否退出 | 主线程，在判据采样并 join 之后 |

配套的顺序修正（第一版实测暴露）：必须先把**退出屏障关掉**、建好常驻 worker
（跑一次正常 job 回到 Idle），**再**武装屏障。否则首启的启动握手会撞上
「回收调用等 worker 退出、而 worker 正卡在屏障里」，触发
`QThread::wait: Thread tried to wait on itself`，用例全轮跳过（`windowHits=0`）。
同理，预热阶段必须先 `armed.store(0)`，否则故障注入让 worker 第一轮就退役。

### 18.3 P2：测试与证据扩大了 `shutdown(false)` 的保证

**问题。** 旧判据把「`shutdown()` 返回时仍有借用」一律算缺陷，且**刻意不要求**
`shutdownOk`。但既定契约里 `return false` 只表示「非 Idle：已请求停止，未销毁任何
对象」——这种情形下允许仍有借用、也允许 worker 还在跑，它们本来就该留着。
把它算作缺陷，等于把 false 的语义扩大成「也已安全关闭」。

**整改。** 不改 `shutdown()` 的 false 语义，而是**让用例进入成功关闭路径**：

1. 用例每轮先跑一次正常 job 回到 Idle，服务处于 Idle 且无在途 job => 走 Idle 关闭分支；
2. 判据只挂在 `shutdownOk == true` 上，并要求「返回时守卫数为 0 **且** 被借用的 worker
   不在运行」（新增 `workerRunningAtReturn` 采样，直接对应「提前删除」的后果）；
3. 新增正向前提 `shutdownOkRounds >= 1`：缺这一条，`earlyDeletes==0` 也可能只是
   「一次都没走过成功关闭路径」；
4. 走 false 分支的轮次打印诊断但**不计缺陷**。

同一条更正同步写回 §17.4（见该节的「更正」块）。

### 18.4 新增用例：`testStartDuringShutdownMustBeRejected`

停靠点用 **worker 退出屏障**，不是回收等待钩子。第一版用回收钩子实测 `windowsEntered=0`：
Idle 关闭路径下 `m_thread` 直接进 retiring，**根本不经过** `retireWorkerThreadBounded()`，
钩子不触发。退出屏障位于 `WorkerThread::run()` 末尾（`workerLoop()` 已返回、线程尚未
结束），Idle 关闭必须经 `joinWorkerThread()` 等它，因此一定能停住 `shutdown()`，
且屏障等待不持 `m_mutex`。

用例流程（3 轮）：预热 job -> 武装退出屏障 -> 独立线程发起 `shutdown()` -> 等「worker 已停
在屏障上」-> **主线程发起 `startSingle()`** -> 断言被拒绝且模块执行次数不变 ->
放行屏障并收敛。

判据：`startedDuringClose == 0`、`runsDuringClose == 0`，正向前提 `windowsEntered >= 1`。

### 18.5 反向验证（判别性）

缺陷态位置：`/var/tmp/t25r7-defect-wt`（`git worktree` detach 于 `199bbfb`，**仅**同步
本轮三个源码文件）+ 构建目录 `/var/tmp/t25r7-defect-build`。两处缺陷用**环境变量**切换
（未设置时行为与交付树完全一致），避免为每个变体各付一次 Generate：

| 变体 | 派生方式 | 对应评审判点 |
|---|---|---|
| A | `waitForReclaimBorrowsReturned()` 在 `DEEPLUX_DEFECT_NO_WAIT_BORROW` 非空时**直接 return**（即评审的「把等待助手换成空操作」） | P2 判别性 |
| B | 删去 `enqueue()` 最终入队临界区的 `if (m_shutdownInProgress) return false;` | P1 |

**验收口径**：变体 A 下 `testShutdownMustWaitForActiveReclaimBorrowBeforeDeleting` 必须
变红；变体 B 下 `testStartDuringShutdownMustBeRejected` 必须变红；两者在对应环境变量
未设置时都必须复绿。

### 18.6 结果

**缺陷态构造的实际形态（修正记录）。** 第一版把两处缺陷写成**无条件**撤销，结果
「缺陷态构建 + 不设环境变量」仍然红灯（`accepted=3`），无法与修复态对照。改为
**环境变量门控**后的最终形态：

| 变体 | 变量 | 生效内容 | 未设置时 |
|---|---|---|---|
| A | `DEEPLUX_DEFECT_NO_WAIT_BORROW` | `waitForReclaimBorrowsReturned()` 直接 `return` | 与交付树等价 |
| B | `DEEPLUX_DEFECT_ALLOW_START_DURING_SHUTDOWN` | `enqueue()` 入口与最终入队**两处**都不看 `m_shutdownInProgress` | 与交付树等价 |

> 现场保留：变体 B 起初只撤销**最终入队**的检查，缺陷态仍 5/5 全绿——因为入口
> `:466` 的检查先把启动拒掉了。这说明「最终入队检查」单独存在时并非唯一防线，
> 撤销必须同时覆盖两处，才对应评审所说的「既有缺口」。

**判别性打磨过程中的三次失败（保留现场，均已在交付测试中修正）。**

1. **`windowHits=0`（用例全轮跳过）**：停靠点最初用回收等待钩子，但 Idle 关闭路径下
   `m_thread` 直接进 retiring，**不经过** `retireWorkerThreadBounded()`，钩子永不触发。
   改为 worker 退出屏障。
2. **`windowHits=0`（预热吃掉了钩子）**：`reclaimHookEnabled` 在循环开头就置 true，
   预热那次 `startSingle()` 把钩子消费掉了。改为预热之后才武装。
3. **`windowHits=3` 但 `at return: liveGuards=0`（缺陷态仍全绿）**——最本质的一次：
   旧版让旧 worker 一直卡在退出屏障上，于是 `shutdown()` 里的 `joinWorkerThread() ->
   thread->wait()` 恰好被这条 worker 挡住。**「等 worker 退出」与「等借用归还」在
   worker 尚未退出时恰好同效**，缺陷被掩盖。修正：先放行 worker 退出并确认它真正结束
   （`oldWorkerRunning==false`），此时 `thread->wait()` 立即返回，shutdown 唯一的阻挡
   就只剩「等待借用归还」；回收调用仍停在钩子上，借用确定处于持有状态。

**实测（最终形态）。**

| 验证 | 命令 | 结果 |
|---|---|---|
| 修复态 · 借用用例 | `./bin/test_flowexecutionservice testShutdownMustWaitForActiveReclaimBorrowBeforeDeleting` ×5 | **5/5 `rc=0`**，`windowHits=3 earlyDeletes=0 shutdownOkRounds=3` |
| 缺陷态 A · 借用用例 | `DEEPLUX_DEFECT_NO_WAIT_BORROW=1` ×5 | **5/5 `rc=134`**（SIGSEGV，use-after-free），`Totals: 1 passed, 1 failed` |
| 缺陷态 A · 不设变量 | 同上 ×3 | **3/3 `rc=0`**，`windowHits=3 earlyDeletes=0`（对照成立：门控关闭时等价修复态） |
| 修复态 · P1 用例 | `... testStartDuringShutdownMustBeRejected` ×5 | **5/5 `rc=0`**，`windowsEntered=3 rejected=3 accepted=0 runsDuringClose=0` |
| 缺陷态 B · P1 用例 | `DEEPLUX_DEFECT_ALLOW_START_DURING_SHUTDOWN=1` ×5 | **5/5 `rc=1`**，`windowsEntered=3 rejected=0 accepted=3 runsDuringClose=3` |
| 缺陷态 B · 不设变量 | 同上 ×5 | **5/5 `rc=0`**，`accepted=0 runsDuringClose=0`（对照成立） |
| 服务套件（交付树） | `./bin/test_flowexecutionservice` | **31/31、`SUITE_rc=0`** |

**判别性结论。** 两个用例在各自缺陷态下均**未出现「仍全绿」**：借用用例以
`rc=134`（真实 use-after-free）失败，P1 用例以 `accepted=3 runsDuringClose=3`
（关闭期间模块确实执行了）失败。正向前提 `windowHits>=1` / `windowsEntered>=1`
每次都成立，排除了「测了未执行的代码」。

**环境变量未设置时的等价性**是本章自查补上的一环：第一版缺陷态是无条件撤销，
「不设变量」也红灯，等于对照组失效；改为门控后三组对照全部复绿，双向才真正成立。

日志（均在 `/var/tmp/`）：缺陷态 A `r7d-borrow-{1..5}.log`、A 对照 `r7A-off-{1..3}.log`、
缺陷态 B `r7B-on-{1..5}.log`、B 对照 `r7B-off-{1..5}.log`、修复态
`r7f-borrow-{1..5}.log` / `r7f-p1-{1..5}.log`、套件 `r7-suite-final.log`。

### 18.7 状态

| 项 | 状态 |
|---|---|
| P1（关闭期间可启动新任务） | **已复现（缺陷态 B 5/5 红灯）、已修复、修复态 5/5 绿**，性质为真实缺陷 |
| P2（用例放行点耦合） | **已整改，判别性双向成立**（空操作化后 5/5 `rc=134`） |
| P2（扩大 `shutdown(false)` 语义） | **已整改**：判据只挂成功关闭路径，并以 `shutdownOkRounds>=1` 为前提；§17.4 已同步更正 |
| §17 的判定 | 本轮**不改写** §17 的通过记录，但其中「判据不要求 shutdownOk」一条已被 §18.3 取代 |
| T2.5 / T2.8 / T2.9 | **暂不收口**（按复核要求，不自行宣告） |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成**（十轮连续门禁、`git revert` 抽查留待整批） |

---

## 19. T2.5 P1 第九次整改：排他关闭权 + 可独立编译的 NF 基线 + 死变量清理

> **本章更正第 18 章的收口判断。** 第 18 章基于已撤销的提交链
> `45369b5 / 5a6f987 / 7b6b22c / 0a1d940`，宣称「P1 已修复、判别性双向成立」。
> 第八段复核（`/tmp/deeplux-t25-r8-review.xNyqjd/REVIEW.md`，Reviewed `0a1d940`）
> 用独立探针推翻了这一结论：单次关闭期间拒绝启动**成立**，但**第二次 `shutdown()`
> 会提前解除第一次关闭的保护**；同一条链上的**测试提交不能独立编译**，用例还有
> **固定约 15s 死等**。故整条链以 `git reset --mixed 199bbfb` 撤销重做。
> 第 18 章保留为过程记录，其结论以本章为准；§18.7 中「P1 已修复」不再成立。

### 19.1 复核提出的三项问题（逐条只读核对）

复核证据 `REVIEW.md` 的关键输出：

```text
Single shutdown, start attempted while old worker held at exit | 3/3 correctly rejected
Second shutdown during first shutdown, then attempt start       | 3/3 reproduced early success and accepted start
Borrow-wait helper replaced with no-op via LD_PRELOAD          | rc=134, fatal signal 11
Current service suite                                          | 31/31, 46824ms
Test-only commit 45369b5 syntax check                          | Compile failure: seven missing test-hook methods
clang-format dry-run; git diff --check                         | Passed
secondShutdownOkBeforeWorkerReleased=1 shutdownOk=1 restartOk=1
activeAtShutdownReturn=1 engineBusyAtShutdownReturn=1
```

| 编号 | 复核判点 | 位置 | 本轮只读核对结果 |
|---|---|---|---|
| P1 | 第二次 `shutdown()` 提前解除第一次关闭的保护；应在锁内取得**排他关闭权**，重入调用不得清除原关闭者的标记 | `FlowExecutionService.cpp:775`（入口）、`:821`（标记清除） | 属实。旧入口无条件 `m_shutdownInProgress = true`；第二次调用看到空 `retiring`/`inflight`，走到成功结尾返回 `true` 并清标记，随后 `enqueue()` 放行新任务 |
| P2 | 测试提交 `45369b5` 用了直到 `5a6f987` 才提供的 7 个接口，单独检出**编译失败**，不构成 NF 红灯基线；撤销完整修复也会删除测试基础设施 | `tests/test_flowexecutionservice.cpp:2036` | 属实。`45369b5` 只改测试文件（+627），`5a6f987` 才改 `.h`(+126)/`.cpp`(+304) 提供 hook |
| P3 | `reclaimReturned` 初始化为 `false` 后无任何写入，三轮等待各耗尽 5s，后续「仍在回收」诊断恒真 | `tests/test_flowexecutionservice.cpp:2186`、`:2305` | 属实。套件耗时 46824ms 中含约 15s 无效等待 |

**已确认改善（复核明确列出，本轮不改）**：单次关闭期间启动被正确拒绝；借用测试撤销等待后已变红
（`rc=134`）；`shutdown(false)` 判据已纠正；服务套件 31/31、格式与差异检查通过。

### 19.2 提交链重做：钩子基础设施与行为修复必须分离

**旧链（已撤销）：**

```text
199bbfb docs: record the fourth-round P1 (off-lock reclaim ownership window)
45369b5 test: cover start-during-shutdown and decouple the reclaim-borrow window
5a6f987 fix: forbid starting a job once shutdown has begun (P1)
7b6b22c docs: record the eighth-round P1 fix and the corrected borrow-window judgement
0a1d940 docs: drop a stray blank line at end of the evidence file
```

**新链（`git reset --mixed 199bbfb` 后重做）：**

```text
199bbfb docs: record the fourth-round P1 (off-lock reclaim ownership window)
c60f76e test: provide lifecycle test hooks and land the borrow-ownership hardening (infra)
2ef8011 test: cover start-during-shutdown, reclaim-borrow window, and shutdown reentry
b2bfbec fix: forbid starts during shutdown and serialize shutdown ownership (P1)
293d69b style: clang-format the shutdown-owner regression test
C4      docs/证据（本章）
```

`293d69b` 是门禁自查发现的**本轮真实退步**：重做 C2 时新增用例未经
`clang-format`，`--dry-run --Werror` 报 5 处违规（`:2787`–`:2822`）。按既有惯例
（旧链曾为文档末尾空行单独补 `0a1d940`），用独立 `style` 提交修正，只改测试文件
（4 insertions / 6 deletions，纯换行与空白，**语义与判据零改动**），不动已拆好的
C1/C2/C3 链。`FMT_rc=0`、`DIFFCHECK_rc=0`。

拆分口径：测试基础设施（hook / `ReclaimBorrow` / 借用登记时序）**先于**测试提交落地，
行为修复（含 `m_shutdownInProgress` 与排他关闭权）**最后**落地。于是「测试提交单独检出」
既能编译、又必然因运行断言变红；撤销 `b2bfbec` 不再删除任何测试基础设施。

**可恢复性归档**（执行 `git reset --mixed` 前完成）：

- `git branch backup/r8-pre-resplit 0a1d940`（旧链 HEAD 仍可检出）。
- `/var/tmp/deeplux-r9-backup/`：四个源文件的完整副本 + `OLD_SHA_MAP.txt`（旧 SHA 映射）。
- 五份提交快照导出并逐字节核对（`git show <c>:<p> | sha256sum` vs `sha256sum <snap>`，**5/5 MATCH**）：

| 快照 | commit:path | SHA-256 |
|---|---|---|
| `C1.FlowExecutionService.h` | `c60f76e:src/core/engine/FlowExecutionService.h` | `70fbfa04cac85e38c9d38996fddcc95eb37217252333169a216391c49d5b0fdd` |
| `C1.FlowExecutionService.cpp` | `c60f76e:src/core/engine/FlowExecutionService.cpp` | `9614fbe831f130e7097043e1854a28e39f9f0c019364aab2756a8d43fa104e95` |
| `C2.test_flowexecutionservice.cpp` | `2ef8011:tests/test_flowexecutionservice.cpp` | `6fc3c33b47f24d22384e118bcbc10651efe4af2e65682bbca0407855cb481eb1` |
| `C3.FlowExecutionService.h` | `b2bfbec:src/core/engine/FlowExecutionService.h` | `2581e71a4de5b9568cc54eca4bbc5ced0744a0a5c5e43d34aff95283a78c20e4` |
| `C3.FlowExecutionService.cpp` | `b2bfbec:src/core/engine/FlowExecutionService.cpp` | `1766e089b00f05363189efa5c560844e8e0c4340955ce0b89d18d18908000e89` |

**半初始化工作树的修复（现场保留）。** `git worktree add --detach /var/tmp/t25r9-wt b2bfbec`
在 `Updating files: 93%` 被 SIGTERM（`Exit Code 137`），残留 0 字节 `index.lock`、
无 index、862 条 staged deletion、`locked` 标记。处理顺序：确认无存活 git 进程 →
删除陈旧 `index.lock` → `git -C /var/tmp/t25r9-wt reset --hard HEAD`（`RESET_rc=0`，
`Updating files: 100% (850/850), done.`）→ `status --short` **0 行** →
`git worktree unlock`（`UNLOCK_rc=0`）。**未带半初始化状态做任何验证。**

### 19.3 P1 修复：关闭权是排他的（`b2bfbec`）

**根因。** `m_shutdownInProgress` 旧语义只是「正在关闭」的布尔标记，不区分「谁持有本次关闭」。
第一次关闭已在 `detachLiveWorkerThreadsLocked()` 把旧 worker 移进 `retiring`，随后阻塞在
`joinWorkerThread()` 的 `thread->wait()`；此时第二次 `shutdown()` 进入同一临界区，无条件
重新置位，看到的 `retiring` 是**自己的空局部列表**，于是一路走到成功结尾：返回 `true`、
清掉第一次关闭者的标记、`m_reclaimWake.wakeAll()` 并报告「清理已完成」。
第一次关闭者仍持有旧 worker，新 `enqueue()` 却已通过两道检查并执行。

**修复**：在 `shutdown()` 入口的**同一临界区**内先取得排他关闭权（`b2bfbec`）：

```cpp
if (m_shutdownInProgress) {
    return false;   // 重入调用不是 owner：不清标记、不改收尾状态、不报告完成
}
m_shutdownInProgress = true;
```

只有 owner 在成功结尾清除标记并恢复服务；非 Idle 分支仍按契约撤销标记后返回 `false`。
`waitForReclaimBorrowsReturned()` 的注释同步更正：`m_reclaimAborted` **只在存在在途回收时**
由 `detachLiveWorkerThreadsLocked()` 置位，普通 Idle 关闭不能依赖它——「禁止新启动」与
「等借用归还」的先后关系由 `m_shutdownInProgress` 建立，二者仍是生命周期等待，不复用
`StartHandshakeTimeoutMs(100ms)`。

**新增回归** `testSecondShutdownMustNotReleaseFirstShutdownGuard()`（C2，3 轮）：
用 `setWorkerExitBarrierForTest()` 把第一次 `shutdown()` 停在关闭段中间，叠加第二次
`shutdown()`，随后发起 `startSingle()`。判据：

```cpp
QVERIFY2(windowsEntered.load() >= 1, ...);              // 正向前提：被测路径真被执行
QVERIFY2(firstShutdownOkRounds.load() >= 1, ...);
QVERIFY2(secondShutdownOk.load() == 0, ...);            // 重入不得报告完成
QVERIFY2(restartAccepted.load() == 0, ...);             // 不得放行新启动
QVERIFY2(runsDuringGuard.load() == 0, ...);             // 关闭期间模块不执行
QVERIFY2(restartAfterRelease, ...);                     // owner 释放后服务恢复可用
```

### 19.4 P2 修复：NF 红灯基线必须来自运行断言

**C1 `c60f76e`（infra）**：保留全部测试钩子、`ReclaimBorrow` RAII 守卫、
`liveReclaimGuardCountForTest()`、`waitForReclaimBorrowsReturned()` 与既有借用登记时序，
但**不含任何 `m_shutdownInProgress` 行为修复**。核对结果：

```text
C1.h   ForTest decls           : 21
C1.h   m_shutdownInProgress    : 0
C1.cpp m_shutdownInProgress    : 0
```

C2 测试引用的 21 个 `ForTest` 符号中，4 个不在 `FlowExecutionService.h`
（`setExecutionFaultInjectionForTest` / `clearExecutionFaultInjectionForTest` /
`clearFinalizeHookForTest` / `setModuleExecutionAffinityForTest`），已逐一核对为
`RunEngine.h:291/296/298/299` 的**既有**声明——不是本轮新增、不会随行为修复一起消失。

**C2 `2ef8011`（test）**：在 C1 之上可独立编译；含 P3 清理、排他关闭权用例与既有两项借用/关闭用例。
**C3 `b2bfbec`（fix）**：仅改 `.h/.cpp`（`git diff --stat 2ef8011 b2bfbec` =
2 files changed, 70 insertions(+), 4 deletions(-)），**不碰测试文件**。

因此「测试提交单独检出」= `c60f76e` 的 `.h/.cpp` + `2ef8011` 的测试源文件，
等价命令 `git checkout -f 2ef8011`，其红灯只能来自 `QVERIFY2` 运行断言。

### 19.5 P3 修复：`reclaimReturned` 只作收尾兜底

P3 是 §18.2「放行点解耦」的残留：starter 线程不再写完成信号，但变量、等待循环和
`reclaimStillActive` 诊断被保留下来，导致 `false` 恒值 → 三轮各空等 5s → 约 15s 固定开销、
诊断恒真。修复（C2）：

```cpp
// P3 更正：本变量**只**用于下方 join 的有界兜底，绝不参与任何判据。
std::atomic_bool reclaimReturned{false};
starter = std::thread([&] {
    (void) service.startSingle();
    reclaimReturned.store(true, std::memory_order_release);
});
```

同时删除 `reclaimStillActive` 与 `qInfo` 中的 `(reclaimReturned=%d)` 字段，等待循环注释
明确「只用于避免主线程在 join 上永久阻塞，绝不参与判据」。核对：
`reclaimReturned` 写入次数 = 1，`reclaimStillActive` 出现次数 = 0。

### 19.6 缺陷态构造：隔离工作树 + 环境变量门控

- 工作树：`/var/tmp/t25r9-wt`（detach，出工作区），构建目录：`/var/tmp/t25r9-gate`（全新）。
- 配置：`TMPDIR=/var/tmp/gcc-tmp`（`/tmp` 为 10M tmpfs），
  `-DUSE_QT6=ON -DQt5_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5
  -DONNXRUNTIME_DIR=/home/charles/Data/onnxruntime
  -DOpenCV_DIR=/usr/local/lib/cmake/opencv4 -DDEEPLUX_NATIVE_SAM=ON
  -DENABLE_CAMERA_BASLER=OFF -DENABLE_CAMERA_HIKVISION=OFF -DENABLE_HYMSON3D=OFF`。
  （`OpenCV_DIR` 用真实可用路径 `/usr/local/lib/cmake/opencv4`，实测
  `OpenCV: Found (version 4.8.1)`、`Qt version: 5.15.3`。）
- CTest：`unset DISPLAY`、`QT_QPA_PLATFORM=offscreen`、`TMPDIR` 指向大容量 `/var/tmp` 目录。
- 门控缺陷（**只在隔离工作树应用**，脚本 `make_defect.py`）：

```cpp
static const bool allowReentry =
    !qgetenv("DEEPLUX_DEFECT_ALLOW_SHUTDOWN_REENTRY").isEmpty();
if (!allowReentry && m_shutdownInProgress) {
    return false;
}
m_shutdownInProgress = true;
```

未设置变量时 `!allowReentry && m_shutdownInProgress` 等价于交付树的
`m_shutdownInProgress`；设置时跳过排他检查，复现「第二次 shutdown 清标记并接受启动」。
脚本锚点先在副本上 dry-run 通过（`DRYRUN_rc=0`，两处替换各 1 次匹配）。

### 19.7 门禁环境事故与处置（如实留档，不重试到绿）

第一次门禁跑在**交付树既有局部构建目录** `build/`（Qt5 Makefile，非全新目录），
命令 `TMPDIR=/var/tmp/gcc-tmp make -j24` + `ctest --output-on-failure`，得到三个红信号：

| 项 | 结果 | 原始输出 |
|---|---|---|
| 全量构建 | `BUILD_ALL_rc=2`、`ERROR_COUNT=1`，38% 中断 | `Cannot create temporary file in /var/tmp/gcc-tmp/: Operation not permitted` → `c++: internal compiler error: Aborted signal terminated program collect2` → `libLinesDistancePlugin.so] Error 4` |
| 全量 CTest | `CTEST_rc=8`，`96% tests passed, 4 tests failed out of 97`，430.61s | 失败：`4 - test_mainwindow`、`38 - test_imagerefreshcoalescer`、`39 - test_flowidentitycontract`、`46 - test_agentactor (SEGFAULT)` |
| 格式 | `FMT_rc=1` | 5 处 `clang-format-violations`，全在新增用例 `:2787`–`:2822` |

**本轮改动的服务套件本身通过**：

```text
18/97 Test #18: test_flowexecutionservice ............   Passed   38.90 sec
```

**根因判定（环境，非源码）。** `/var/tmp/gcc-tmp` 为 `root:root 775`，磁盘 456G 可用、
inode 使用率 6%，空间/权限均正常；但目录内 `cc*.s` / `cc*.cdtor.c` 临时文件**全部为
0 字节**，对应 GCC `mkstemp` 拿到 `EPERM`（沙箱对并发临时文件创建的拦截，`-j24` 放大）。
构建在 38% 中断后，已更新的 `libdeeplux_core.so` 与大量**未重编**的旧测试二进制处于
半更新状态，那 4 个失败（含 `test_agentactor` SEGFAULT、`test_mainwindow` 大量
`BashProcess::Failed`）是**构建不完整的连锁反应**，不是本轮源码回归的证据。

**处置（严守两条纪律）。**

1. **不在旧目录重试到绿**——旧的 `build/` 结果不作为门禁结论，原样留档：
   `/var/tmp/deeplux-r9-backup/delivery_build_all.log`、`delivery_ctest.log`、
   `delivery_fmt.log`、`delivery_gate_result.log`。
2. **门禁移到工作区外全新目录** `/var/tmp/t25r9-gate`
   （`CONFIGURE_rc=0`，`Generating done (1342.7s)`，`Build files have been written`），
   源码为独立工作树 `/var/tmp/t25r9-wt`（detach `293d69b`，`status --short` **0 行**），
   TMPDIR 改为构建目录私有 `/var/tmp/t25r9-gate/tmp`，并发降为 `-j8`，
   CTest 用 `unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + 大容量 TMPDIR。
3. **那 4 个失败是否消失，由该全新目录的完整 CTest 判定，不预设结论**；若仍红，
   按真实回归深挖，不得收口。

### 19.8 NF 红灯基线：测试提交单独检出（P2 判定）

**执行位置**：工作树 `/var/tmp/t25r9-wt`（detach `2ef8011`）、构建目录
`/var/tmp/t25r9-gate`、脚本 `/var/tmp/deeplux-r9-backup/iso_experiments.sh`、
结果 `iso_experiments_result.log`、日志 `iso_nf_*.log`。

命令：`git -C /var/tmp/t25r9-wt checkout -f 2ef8011` →
`make test_flowexecutionservice -j8`（`TMPDIR` 为构建目录私有）。

前置核对（证明「无行为修复」且「可独立编译」）：

```text
NF_HEAD=2ef801191e9057d4e1630730dccaf6f287782566
NF_DIRTY_LINES=0
NF_HAS_FIX_MARKER=0              # .cpp 中 m_shutdownInProgress 出现 0 次
BUILD_rc=0  ERROR_COUNT=0  NF_BUILD_MISSING_METHODS=0  TST_EXISTS=1
```

七个 hook 接口由 C1 `c60f76e` 提供，不再依赖 `5a6f987` 里与行为修复捆绑的那份。
对照旧链：复核实测 `45369b5` 单独检出为 **compile failure**（seven missing
test-hook methods）；新链 `2ef8011` 单独检出为 **`BUILD_rc=0` + 运行断言红灯**。

| 验证 | 命令 | 结果 |
|---|---|---|
| 全套件 | `./bin/test_flowexecutionservice`（`timeout 300`） | **`rc=2`**，`Totals: 30 passed, 2 failed, 0 skipped, 0 blacklisted, 42945ms` |
| 定向 ×3 | `... testStartDuringShutdownMustBeRejected testSecondShutdownMustNotReleaseFirstShutdownGuard` | **3/3 `rc=2`、`FAILS=2`** |

FAIL 断言原文（**来自 `QVERIFY2` 运行断言，不是编译错误**）：

```text
FAIL! : ...testStartDuringShutdownMustBeRejected()
       'startedDuringClose.load() == 0' returned FALSE. (accepted=3/3 ...)
       Loc: [/var/tmp/t25r9-wt/tests/test_flowexecutionservice.cpp(2606)]
FAIL! : ...testSecondShutdownMustNotReleaseFirstShutdownGuard()
       'secondShutdownOk.load() == 0' returned FALSE. (secondShutdownOk=3/3)
       Loc: [...(2805)]
```

**P2 判定：修复成立。** 撤销完整行为修复提交不再删除测试基础设施；NF 红灯来自
断言而非 `no member named` / `not declared in this scope`（`NF_BUILD_MISSING_METHODS=0`）。

### 19.9 反向验证：双门控缺陷态判别性（P1 判定）

缺陷态**只在隔离工作树应用**，脚本 `make_defect_dual.py`（两处替换各 1 次匹配，
`DEFECT_DERIVE_rc=0`，`DEFECT_MARKERS=2`），两个**互相独立**的环境变量：

| 门控 | 生效内容 | 未设置时 |
|---|---|---|
| `DEEPLUX_DEFECT_ALLOW_SHUTDOWN_REENTRY=1` | 跳过排他关闭权检查，复现第二次 shutdown 清标记并接受启动 | `!allowReentry && m_shutdownInProgress` 等价交付树 |
| `DEEPLUX_DEFECT_NO_WAIT_BORROW=1` | `waitForReclaimBorrowsReturned()` 直接 `return`（评审的 LD_PRELOAD 手法） | 与交付树等价 |

| 组 | 命令 | 结果 |
|---|---|---|
| reentry ON ×5 | 变量置 1 + `testSecondShutdownMustNotReleaseFirstShutdownGuard` | **5/5 `rc=1 FAILS=1`**，`secondShutdownOk=3 restartAccepted=3 runsDuringGuard=3` |
| borrow ON ×5 | 变量置 1 + `testShutdownMustWaitForActiveReclaimBorrowBeforeDeleting` | **5/5 `rc=134`**，`QFATAL Received signal 11`（SIGSEGV / use-after-free），`Totals: 1 passed, 1 failed` |
| 交叉特异性 ×3 | reentry ON 跑 borrow 用例 / borrow ON 跑 owner 用例 / reentry ON 跑 start 用例 | **3/3 `rc=0 FAILS=0`**（每个门控只打断自己对应的用例） |
| 双门控 OFF ×5 | 两变量 `unset` + owner 用例 | **5/5 `rc=0 FAILS=0`** |
| 双门控 OFF 全套件 | 两变量 `unset` | **`rc=0`**，`32 passed, 0 failed`，38039ms |
| 恢复 pristine | `git checkout -f 293d69b` 后全套件 | **`rc=0`**，`32 passed`，34145ms |

reentry ON 的 FAIL 原文：`'secondShutdownOk.load() == 0' returned FALSE ... Loc: [...(2804)]`
—— 行号比 NF 基线的 `(2805)` 少 1，正是 `293d69b` 删去一行的结果，恰好旁证
两次构建用的是不同 commit 的源码。borrow ON 的 `signal 11` 与复核报告
`fatal signal 11` 一致。

**双向成立**：两个用例在各自缺陷态下**均未出现「仍全绿」**；门控关闭时全部复绿。
修复态三个套件（`iso_probe_suite.log` / `iso_defect_off_suite.log` /
`iso_restore_suite.log`）的正向前提计数一致，排除「测了未执行的代码」：

```text
[P1-SHUTDOWN-START]  rounds=3 windowsEntered=3 rejected=3 accepted=0 runsDuringClose=0 shutdownOkRounds=3
[P1-SHUTDOWN-OWNER]  rounds=3 windowsEntered=3 secondShutdownOk=0 restartAccepted=0 runsDuringGuard=0 firstShutdownOkRounds=3
```

**重编真实性核实**（防止「其实没重编」的假阳性）：`iso_nf_build.log`、
`iso_defect_build.log`、`iso_restore_build.log` 三份日志均含
`[  7%] Building CXX object src/core/CMakeFiles/deeplux_core.dir/engine/FlowExecutionService.cpp.o`
与 `[ 14%] Linking CXX shared library ../../lib/libdeeplux_core.so`。
脚本运行期出现 7 条 `touch: setting times ... Operation not permitted`（沙箱拦截改
mtime），但 `git checkout -f` 写文件本身已更新 mtime，重编确实发生；行为随源码状态
变化（NF 基线 30/2 → 缺陷态红 → 恢复态 32/0）也独立佐证。**该噪声如实记录，未隐藏。**

**如实记录的异常信号**（2026-11-12 按评审建议更正根因口径）：上述三个修复态套件各有 **6 条**
`QWARN : ...testShutdownMustWaitForActiveReclaimBorrowBeforeDeleting() QThread::wait: Thread tried to wait on itself`。
Qt 告警后继续，用例仍 `PASS`、套件 `SUITE_rc=0`。不隐藏、不因告警宣告失败、也不因全绿而忽略。

**根因：仅记录观察到的事实与可能解释，不作为已确认结论。** 本段原文曾写「成因见
§18.2（预热/退出屏障时序下的 Qt 自等待告警）」，属确定性归因，按 2026-11-12 评审建议
**撤回**。评审独立追踪给出的观察事实：告警对应的目标线程**已经结束**，且**不是当前
`QThread` 对象**；本机 Qt 5.15.3 的 `wait()` 会先比较原生线程 ID、再检查线程结束状态，
因此该现象**更符合线程 ID 复用**（原生 ID 被新线程重用后与当前线程 ID 相等），而非
「自等待」。以上为观察事实与一种可能解释，**未经独立探针确证**，须待后续验证。
本更正为**非阻塞文案修正**：不改变任何断言、测试结果或 `SUITE_rc=0` 结论，也不影响
T2.5 收口（见 §21）。§18.2 的相关表述按历史记录纪律原样保留，不再作为已确认根因引用。

### 19.10 全量门禁（工作区外全新目录）

**执行位置**：构建目录 `/var/tmp/t25r9-gate`（全新、`CONFIGURE_rc=0`）、
源码 `/var/tmp/t25r9-wt`（detach `293d69b`，`SRC_DIRTY_LINES=0`）、
脚本 `/var/tmp/deeplux-r9-backup/iso_gate.sh`、结果 `iso_gate_result.log`。

| 步骤 | 命令 | 结果 |
|---|---|---|
| STEP 0 源码身份 | `git rev-parse HEAD` / `git status --short` | `293d69b5ec1d9116b20c80e1f15d03cd57d62942`、`SRC_DIRTY_LINES=0` |
| STEP 1 全量构建 | `TMPDIR=/var/tmp/t25r9-gate/tmp make -j8` | **`BUILD_ALL_rc=0`**、`ERROR_COUNT=0` |
| STEP 2 清单 | `ctest -N` | **`CTEST_N_rc=0`**、`Total Tests: 97` |
| STEP 3 全量 CTest | `unset DISPLAY; QT_QPA_PLATFORM=offscreen; TMPDIR=<大容量>; ctest --output-on-failure` | **`CTEST_rc=0`**、`100% tests passed, 0 tests failed out of 97`、`304.19 sec` |
| STEP 4 格式 | `clang-format --dry-run --Werror` 三个源文件 | **`FMT_rc=0`** |
| STEP 4 差异 | `git diff --check` | **`DIFFCHECK_rc=0`** |

```text
18/97 Test #18: test_flowexecutionservice ............   Passed   36.47 sec
The following tests FAILED: (none)
```

**§19.7 那 4 个失败的最终判定**：`4 - test_mainwindow`、`38 - test_imagerefreshcoalescer`、
`39 - test_flowidentitycontract`、`46 - test_agentactor (SEGFAULT)` 在全新目录**完整构建后
全部消失**（97/97）。据此确认它们是交付树旧 `build/` 在 `BUILD_ALL_rc=2` 中断后、
`libdeeplux_core.so` 与未重编旧测试二进制半更新造成的连锁，**不是本轮源码回归**。
该结论由全新目录的完整 CTest 给出，不是「在旧目录重试到绿」。

**「97 项」口径**：新增用例 `testSecondShutdownMustNotReleaseFirstShutdownGuard`
落在既有可执行文件 `test_flowexecutionservice` 内，故 `ctest -N` 仍是 97；
服务套件**用例数由 31 增至 32**（`Totals: 32 passed`），两者须同时给出，
不得以「97 项」断言没有新增测试。

**本轮未做的整批项**：十轮连续门禁、`git revert` 抽查属 R2 整批验收，仍留待整批，
本轮不宣称完成。

### 19.11 状态

| 项 | 状态 |
|---|---|
| P1 第二次 `shutdown()` 提前解除第一次关闭的保护 | **真实缺陷**：reentry 缺陷态 5/5 `rc=1`（`secondShutdownOk=3 restartAccepted=3 runsDuringGuard=3`）复现；`b2bfbec` 排他关闭权修复；修复态 5/5 绿 + 全量 97/97 |
| P2 测试提交 `45369b5` 不能独立编译 | **已整改**：`c60f76e` 先落地 hook infra，`2ef8011` 单独检出 `BUILD_rc=0` / `NF_BUILD_MISSING_METHODS=0`，NF 红灯来自 `QVERIFY2` 运行断言（`30 passed, 2 failed`） |
| P3 `reclaimReturned` 恒 false 固定约 15s 死等 | **已整改**：恢复完成通知仅作 join 兜底、删恒真诊断；修复态套件 34–38s、`32 passed` |
| 新增用例格式违规 | **已整改**：`293d69b` 独立 style 提交，`FMT_rc=0` |
| 门禁环境事故 | **如实留档且不重试到绿**；全新目录 `BUILD_ALL_rc=0` + 97/97 复绿，4 个失败确认为构建不完整连锁 |
| 借用用例判别性（新链重验） | **双向成立**：borrow 缺陷态 5/5 `rc=134`（signal 11）、门控关闭 5/5 绿 + 全套件 32/32 |
| 6 条 `QThread::wait: Thread tried to wait on itself` QWARN | **如实记录**，Qt 告警后用例 `PASS`、套件 `SUITE_rc=0`；不隐藏、不据此宣告失败 |
| T2.5 | **仍待评审收口，不自行宣告** |
| T2.8 / T2.9 | 按既定契约口径可收口；本轮未改动，收口记录另案 |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先） |
| R2 整批 | **尚未完成**（十轮连续门禁、`git revert` 抽查留待整批） |

> **状态提示（2026-11-12 评审后追加；上表内容按历史记录纪律原样保留）**：本表是
> 第九次整改当时（`d799ff1`）的状态。十轮连续全量门禁与 `git revert` 抽查的客观执行结果
> 见 §20（`938fb22`）；T2.5 已收口、R2 整批未收口的最终裁定见 §21。**状态以 §21 为准。**

**证据文件清单**（`/var/tmp/deeplux-r9-backup/`）：
`iso_configure.log`、`iso_gate_result.log`、`iso_build_all.log`、`iso_ctest_N.log`、
`iso_ctest.log`、`iso_fmt.log`、`iso_diffcheck.log`、`delivery_gate_result.log`、
`delivery_build_all.log`、`delivery_ctest.log`、`delivery_fmt.log`、
`iso_experiments_result.log`、`iso_nf_{checkout,build,suite,target_1..3}.log`、
`iso_defect_{derive,build}.log`、`iso_defect_reentry_on_{1..5}.log`、
`iso_defect_borrow_on_{1..5}.log`、`iso_spec_*.log`、`iso_defect_off_{1..5}.log`、
`iso_defect_off_suite.log`、`iso_restore*.log`、`iso_probe_{build,suite,result}.log`、
`CH19_part{1..4}.md`、五份提交快照与 `OLD_SHA_MAP.txt`。

---

## 20. R2 整批验收门禁（2026-11-12：十轮连续 CTest + `git revert` 抽查）

本章记录 R2 **批次级验收门禁**的完整执行。此前各章状态表中反复出现的「R2 整批尚未完成（十轮连续门禁、`git revert` 抽查留待整批）」由本章闭合；**历史章节原文一律保留、不改写**，状态以本章为准。

### 20.1 门禁定义与执行位置

主仓计划文档 `docs/superpowers/plans/2026-11-05-review-hardening.md:159` 原文：

> **R2 批次验收门禁（阻塞）：** §负验证纪律三步（测试/修复分提交版本）全部留痕；全量 CTest 连续 10 次通过（以 R0 固定的基线数为准，当前 96 项 / NATIVE_SAM=ON）；`test_executionaffinity`/`test_flowexecutionservice`/`test_mainwindow` 在 `-j4` 下稳定；无 watchdog 触发；`git revert`（仅修复提交）抽查任一任务，对应 NF 用例复败于同一断言。

| 项 | 位置 |
|---|---|
| 隔离工作树 | `/var/tmp/t25r9-wt`（detached `d799ff1`）；**交付工作树全程不切换缺陷态/撤销态** |
| 构建目录 | `/var/tmp/t25r9-gate`（工作区外全新目录，`CMAKE_HOME_DIRECTORY:INTERNAL=/var/tmp/t25r9-wt`） |
| 十轮脚本 | `/var/tmp/r2-batch-gate/r2_gate10.sh`（自含全部命令） |
| revert 脚本 | `/var/tmp/r2-batch-gate/r2_revert_spotcheck.sh`（自含全部命令） |
| 结果目录 | `/var/tmp/r2-batch-gate/`：`gate10_result.log`、`summary.tsv`、`run-{1..10}.log`、`revert_result.log`、`revert_nf_{1..3}.log`、`revert_suite.log`、`restore_*.log`、`probe_*.log` |
| 环境探针（**不计入十轮**） | `probe_checkout.log` / `probe_build.log` / `probe_ctest.log`：`CHECKOUT_rc=0`、`WT_HEAD=d799ff1…`、`BUILD_rc=0 ERROR_COUNT=0`、`PROBE_CTEST_rc=0`、`100% tests passed, 0 tests failed out of 97`、188s、三个关键套件全 `Passed` |

脚本对 §19.7 记录的口径缺陷作了修正：`grep -c` 无匹配时以 `|| true` 归一为 `0`（不把退出码 1 当失败）；关键套件用 `grep -E 'test_...' | grep -c 'Passed'` 匹配，规避 CTest `Test  #` 双空格与 `Test #` 单空格混用。

### 20.2 固定环境与前置门禁

固定环境（脚本内逐条 `export`，十轮与 revert 抽查一致）：

```text
unset DISPLAY
export QT_QPA_PLATFORM=offscreen
export TMPDIR=/var/tmp/t25r9-gate/tmp      # 构建目录私有大容量 TMPDIR；/tmp 为 10M tmpfs
ulimit -c 0
```

构建目录 `CMakeCache.txt` 实测：`CMAKE_BUILD_TYPE=Debug`、`USE_QT6:BOOL=ON`、`DEEPLUX_NATIVE_SAM:BOOL=ON`、`Qt 5.15.3`、`OpenCV 4.8.1`、`ONNXRUNTIME_DIR=/home/charles/Data/onnxruntime`、`OpenCV_DIR=/usr/local/lib/cmake/opencv4`、`CMAKE_CXX_COMPILER=/usr/bin/c++`。配置结论沿用 §19.10（`CONFIGURE_rc=0`）。

| 前置门禁 | 命令 | 结果 |
|---|---|---|
| 源码身份 | `git -C /var/tmp/t25r9-wt rev-parse HEAD` vs 交付树 | `WT_HEAD=SRC_HEAD=d799ff1ed957a08743572aa149a3ac257b81ac9f`；`WT_DIRTY_LINES=0`、`SRC_DIRTY_LINES=0` |
| 三份关键源码逐字节 | `sha256sum` 对齐隔离工作树与交付树 | `SRC_MATCH_COUNT=3/3`：`FlowExecutionService.cpp`=`1766e089…`、`FlowExecutionService.h`=`2581e71a…`、`test_flowexecutionservice.cpp`=`472db8d7…` |
| 构建源目录 | `grep '^CMAKE_HOME_DIRECTORY' CMakeCache.txt` | `/var/tmp/t25r9-wt`（与隔离工作树一致） |
| 全量构建 | `( cd $BLD && make -j8 )` | `BUILD_ALL_rc=0 ERROR_COUNT=0` |
| 格式 | `clang-format --dry-run --Werror` 于两个生产文件 + 测试文件 | `FMT_rc=0` |
| 差异检查 | `git diff --check` | `DIFFCHECK_rc=0` |
| 枚举 | `ctest --test-dir $BLD -N` | `CTEST_N_rc=0`、`LISTED=97`；`Test  #4: test_mainwindow`、`Test #14: test_executionaffinity`、`Test #18: test_flowexecutionservice` 均在列 |

### 20.3 基线数 96 → 97 的核对

计划文档 `:90` / `:159` 记载「撰写本文时枚举 96 项 / NATIVE_SAM=ON」，并要求「`ctest -N` 枚举数与实跑通过数一致并记录」。本次实测 **97 项**，逐项核对如下：

```text
OLD_COUNT 96 NEW_COUNT 97
ADDED ['test_flowidentitycontract']
REMOVED []
```

- 对比方法：Python 正则 `Test\s+#?(\d+):\s+(\S+)` 分别提取 `/var/tmp/t21-gate10/run-1.log`（§5.6 历史十轮，96 项）与本次 `ctest --test-dir /var/tmp/t25r9-gate -N` 全量枚举（97 项）的测试名集合，取差集。
- 差异来源：`git log --oneline -S 'test_flowidentitycontract' -- tests/CMakeLists.txt` → `42ed898 test: add T2.6 frame-identity behavior-contract regressions`（T2.6 帧身份契约，见第 9 节），**无移除项**。
- `ctest -N` 枚举数 97 = 十轮每轮实跑通过数 97，符合 R0 `:90` 口径。另按既有口径说明：T2.5 第九次整改新增的 `testSecondShutdownMustNotReleaseFirstShutdownGuard` 落在**既有** `test_flowexecutionservice` 可执行文件内，服务套件用例数由 31 → 32，但**不增加 CTest 项数**；「97 项 CTest」不等于「无新测试」。

### 20.4 十轮连续全量 CTest（**10/10 全通过**）

命令（每轮独立完整执行，**未加 `-R`/`-E`、未设置跳过、未放宽严格比较强度、未重试**）：

```bash
ctest --test-dir /var/tmp/t25r9-gate -j4 --output-on-failure
```

| 轮次 | rc | failed | passed_line | affinity | service | mainwindow | notrun | watchdog_hit | watchdog_any | secs |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | 0 | `100% tests passed, 0 tests failed out of 97` | 1 | 1 | 1 | 0 | 0 | 0 | 182 |
| 2 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 175 |
| 3 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 172 |
| 4 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 150 |
| 5 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 126 |
| 6 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 99 |
| 7 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 87 |
| 8 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 87 |
| 9 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 84 |
| 10 | 0 | 0 | 同 | 1 | 1 | 1 | 0 | 0 | 0 | 85 |

- 汇总：`NONZERO_RUNS=0`、`NOTRUN_TOTAL=0`、`WATCHDOG_HIT_TOTAL=0`；脚本 `GATE10_DONE 2026-11-12T14:36:22+08:00`，全程 21m41s。
- **逐轮日志完整性扫描**（10 份 `run-*.log`）：每轮 `100% tests passed, 0 tests failed out of 97` 命中 1 次；`FAIL!`=0、`Not Run`=0、`(Disabled)`=0、`skipped`=0，**10 轮合计 0**。
- **三套件在 `-j4` 下稳定**：`test_executionaffinity` / `test_flowexecutionservice` / `test_mainwindow` 每轮各 `Passed` 1 次，10/10 轮无缺项。
- **无 watchdog 触发**：窄口径 `watchdog.*(timeout|trigger|fired|hit|expired)` 与宽口径任意 `watchdog` 字样，10 轮均为 **0**。
- **QWARN 口径如实说明**：十轮 CTest 汇总日志用 `--output-on-failure`，通过的用例不转发 Qt 输出，故 10 份日志 `QWARN=0`；§19.9 记录的服务套件**直跑** 6 条 `QThread::wait: Thread tried to wait on itself` 告警属于通过用例的 Qt 输出，二者口径不同，**本轮未隐藏、未消失、也未据此宣告失败**（Qt 告警后用例 `PASS`、套件 `SUITE_rc=0`）。

### 20.5 `git revert` 抽查（仅修复提交 `b2bfbec`）

**抽查对象选择**：最终链 `199bbfb → c60f76e → 2ef8011 → b2bfbec → 293d69b → d799ff1` 中，`b2bfbec` 是**唯一的行为修复提交**（`293d69b` 仅改测试文件格式、`d799ff1` 仅改文档，均不重叠），符合计划文档「`git revert`（仅修复提交）抽查任一任务」。对应 NF 用例为 `testStartDuringShutdownMustBeRejected` 与 `testSecondShutdownMustNotReleaseFirstShutdownGuard`（§19.8 已建立可编译 NF 基线）。

| 步骤 | 命令 | 结果 |
|---|---|---|
| 1 撤销 | `git -C /var/tmp/t25r9-wt revert --no-commit b2bfbec` | `REVERT_rc=0`，**干净应用**；`git status --short` 仅 `M src/core/engine/FlowExecutionService.cpp`、`M src/core/engine/FlowExecutionService.h` |
| 1 前置断言 | `grep -c m_shutdownInProgress` / `grep -c ForTest` / 两个用例名 | `FIX_MARKER_IN_CPP=0`、`FIX_MARKER_IN_H=0`（修复已撤销）；`HOOK_INFRA_DECLS=21`、`NF_CASE_1=1`、`NF_CASE_2=1`（**钩子与用例完整保留**） |
| 2 重编 | `make test_flowexecutionservice -j8` | `REVERT_BUILD_rc=0 ERROR_COUNT=0`，日志含 `[ 14%] Linking CXX shared library ../../lib/libdeeplux_core.so`、`[100%] Built target test_flowexecutionservice` —— **可编译的测试失败，不是编译错误** |
| 3 NF ×3 | `$TST testStartDuringShutdownMustBeRejected testSecondShutdownMustNotReleaseFirstShutdownGuard` | **3/3 `rc=2`、`FAILS=2`**，`Totals: 2 passed, 2 failed, 0 skipped, 0 blacklisted` |
| 3 全服务套件 | `$TST` | `REVERT_SUITE_rc=2`，`Totals: 30 passed, 2 failed, 0 skipped, 0 blacklisted, 36994ms` |
| 4 恢复 | `git revert --quit` + `git reset --hard d799ff1` | `RESTORE_rc=0`、`RESTORE_HEAD=d799ff1…`、`RESTORE_DIRTY_LINES=0`、`RESTORE_FIX_MARKER_IN_CPP=8` |
| 4 恢复重编 | `make test_flowexecutionservice -j8` | `RESTORE_BUILD_rc=0 ERROR_COUNT=0`，日志含 `[  7%] Building CXX object .../FlowExecutionService.cpp.o` 与 `[ 14%] Linking CXX shared library ../../lib/libdeeplux_core.so`（证明恢复态真实重编） |
| 4 恢复复验 | `$TST` / `ctest -j4 --output-on-failure` | `RESTORE_SUITE_rc=0`、`32 passed, 0 failed`；`RESTORE_CTEST_rc=0`、`100% tests passed, 0 tests failed out of 97` |
| 4 收尾 | `git rev-parse HEAD` / `git status --short` | `WT_FINAL_HEAD=d799ff1…`、`WT_FINAL_DIRTY_LINES=0` |

**复败于同一断言**（`revert_nf_1.log` 原文）：

```text
FAIL!  : TestFlowExecutionService::testStartDuringShutdownMustBeRejected()
         'startedDuringClose.load() == 0' returned FALSE. (accepted=3/3 ...)
         Loc: [/var/tmp/t25r9-wt/tests/test_flowexecutionservice.cpp(2606)]
FAIL!  : TestFlowExecutionService::testSecondShutdownMustNotReleaseFirstShutdownGuard()
         'secondShutdownOk.load() == 0' returned FALSE. (secondShutdownOk=3/3)
         Loc: [.../test_flowexecutionservice.cpp(2804)]
```

与 §19.8 的 NF 基线 `(2606)` / `(2805)` 对比：第一条行号完全相同；第二条 `(2804)` 比 `(2805)` 少 1，正是 `293d69b` 删去一行所致（§19.9 已记录同一现象），**两次构建用的是不同 commit 的源码，断言本身一致**。全套件 `30 passed, 2 failed` 亦与 §19.8 基线一致。**判定：满足「对应 NF 用例复败于同一断言」。**

### 20.6 NF 三步留痕汇总（计划文档 `:159` 第 1 项）

| 任务 | 测试提交 / 修复提交 | 第①步 红灯 | 第②步 全绿 | 第③步 revert 复败 |
|---|---|---|---|---|
| T2.1 | `e92fcd4` / `c262d73`（在位修复） | 断言 `'service.shutdownDeadlineHitCountForTest() >= 1' returned FALSE`（:388） | §5.6 十轮 10/10 | `git revert --no-commit c262d73` 干净应用、10/10 复败于同一断言（§1、§5.5） |
| T2.3 / T2.4 | 行为契约／回归保护（§6、§7） | **按裁定不制造 NF 红灯**：缺陷前提不成立、基线已绿 | 行为契约用例与 40/40 探针 | 无生产修复提交，按 §0 纪律不适用 |
| T2.6 | `42ed898` 帧身份契约（§9） | 同上，按裁定不新增信号参数、不制造 NF 红灯 | 三批 97/97、专项 80/80 | 同上 |
| T2.5（最终链） | `2ef8011` / `b2bfbec` | `BUILD_rc=0` + `30 passed, 2 failed`，FAIL 来自 `QVERIFY2`（§19.8） | §19.10 全量门禁 97/97、§20.4 十轮 10/10 | **§20.5 本轮**：`revert --no-commit b2bfbec` 3/3 `rc=2` 复败于同一断言 |
| T2.8 / T2.9 | `463d6b7` / `b50b653`（§13 行为契约／预防性加固） | 按契约口径，撤销态验证见 §13（`/var/tmp/t28-revert-wt` 等） | 服务套件与全量门禁 | 同章留痕；收口记录另案 |
| T2.7 | `be90921` WIP（§3、计划文档 `:152`） | **预防性重构，不可制造 NF 红灯**；未合入、不作硬前置 | — | — |

§0 纪律第 11 行与 §6/§7/§9 裁定一致：**预防性重构／行为契约类任务不得制造 NF 红灯，须注明契约性质**。上表不把「无红灯」写成缺陷，也不把行为契约冒充真实 NF。

### 20.7 五项批次门禁逐条判定

| # | 门禁项（计划文档 `:159`） | 判据 | 本轮结果 |
|---|---|---|---|
| 1 | 负验证纪律三步全部留痕 | 各任务测试/修复分提交、撤销修复复败 | **达成**：§20.6 汇总 + §20.5 本轮批次级抽查 |
| 2 | 全量 CTest 连续 10 次通过（NATIVE_SAM=ON） | 10 次独立完整执行、`0 tests failed` | **达成**：§20.4 `NONZERO_RUNS=0`，10/10 每轮 `0 tests failed out of 97` |
| 3 | 三套件在 `-j4` 下稳定 | 每轮 `test_executionaffinity` / `test_flowexecutionservice` / `test_mainwindow` 均 `Passed` | **达成**：10/10 轮各 1 次 `Passed`，无缺项 |
| 4 | 无 watchdog 触发 | 窄/宽口径命中数 | **达成**：`WATCHDOG_HIT_TOTAL=0`、`watchdog_any` 10 轮全 0 |
| 5 | `git revert`（仅修复提交）抽查任一任务，NF 用例复败于同一断言 | 干净应用、可编译、同一 `QVERIFY2` 与行号 | **达成**：§20.5 `b2bfbec` 抽查 3/3 `rc=2`、全套件 `30 passed, 2 failed` |

### 20.8 与历史门禁记录的关系及纪律核对

- §4.2 的 **8/10**（2 轮 `test_workerpluginconsistency` 图像签名非确定性）与 §4.4 的结论**原样保留、不改写**；#5 最小修复后 §5.6 的 10/10（96 项）是 #5 局部收口的重新执行，同样不改写。
- 本章的 10/10（97 项）是 **R2 整批验收**，在最终交付态 `d799ff1` 上独立执行，与上述历史记录分开记录、不互相替代。
- 纪律核对：**未靠重试到绿**（10 轮一次连续执行，无补跑）、**未排除测试**（无 `-R`/`-E`、无 `Not Run`/`Disabled`/`skipped`）、**未放宽严格比较强度**、**未授予门禁豁免**、**未在交付工作树切换缺陷态/撤销态**（全部在 `/var/tmp/t25r9-wt` + `/var/tmp/t25r9-gate`）、**缺陷态/撤销态用完即恢复**（`WT_FINAL_DIRTY_LINES=0`）。
- 与 §19.7 的门禁环境事故分开：本轮前置大容量 `TMPDIR`（构建目录私有）+ `-j8` 构建 / `-j4` CTest，`BUILD_ALL_rc=0`，无构建中断连锁。

### 20.9 状态

| 项 | 状态 |
|---|---|
| R2 批次验收门禁（五项） | **全部达成**（§20.7）。这是批次级客观门禁结果，**不是收口裁定** |
| T2.5 | **仍待评审收口，不自行宣告**（§19.11 口径不变） |
| T2.8 / T2.9 | 按既定契约口径可收口；本轮未改动，收口记录另案 |
| T2.7 | 继续隔离（`be90921` 非 HEAD 祖先、不作硬前置） |
| 本轮交付改动 | **仅本章文档**，零生产代码改动、零测试改动 |
| 交付工作树 | 本章随 `938fb22` 落地（+164 行，零生产/测试改动）；提交后 `git status --short` 为 0 行 |

### 20.10 证据文件清单（`/var/tmp/r2-batch-gate/`）

脚本 `r2_gate10.sh`、`r2_revert_spotcheck.sh`；
十轮 `gate10_result.log`、`summary.tsv`、`run-{1..10}.log`、`ctest_N.log`、`build_all.log`、`fmt.log`、`diffcheck.log`、`checkout.log`；
探针 `probe_checkout.log`、`probe_build.log`、`probe_ctest.log`；
revert `revert_result.log`、`revert_apply.log`、`revert_checkout_base.log`、`revert_build.log`、`revert_nf_{1..3}.log`、`revert_suite.log`、`restore.log`、`restore_build.log`、`restore_suite.log`、`restore_ctest.log`。

---

## 21. 2026-11-12 评审裁定与收口记录

### 21.1 裁定与本章执行范围

评审结论（原文要点）：

> **T2.5 可以收口；R2 整批不能算收口。** 复核 `feature/review-hardening` 的 `d799ff1`，
> 没有发现阻止 T2.5 收口的代码问题……有一项非阻塞的证据文案建议修正（§19.9 QWARN
> 归因）……因此可以记录 T2.5 已收口，并按既定契约分别记录 T2.8/T2.9；不要把它们或
> 单次 97/97 等同于 R2 完成。

按裁定落地三件事：① §19.9 的 QWARN 根因改为「观察到的事实与可能解释」（已在本章之前
完成）；② 记录 **T2.5 已收口**；③ 记录 **R2 整批未收口**，并把 T2.8/T2.9 按既定契约
**分别记录**。**未宣告 R2 整批收口，未把单次全量 CTest 97/97 冒充整批门禁。**

### 21.2 评审复核证据（评审侧独立执行，如实转录）

| 项 | 评审报告结果 |
|---|---|
| 代码复核 | `d799ff1` 未发现阻止 T2.5 收口的代码问题 |
| 排他关闭权 | `FlowExecutionService.cpp:772` 的新增检查能阻止第二次 `shutdown()` 提前释放第一次关闭的保护 |
| 服务测试 | **32/32** |
| 独立探针（连续 3 次） | 重入关闭返回 `false`；关闭期间启动被拒绝；首次关闭随后成功 |
| NF 基线 `2ef8011` | 两条针对用例**能独立编译**，并在**预期断言处失败** |
| `test_modulebase` | **16/16** |
| `test_runengine` | **119/119** |
| 格式 / 差异 | `clang-format` 检查与 `git diff --check` 通过 |
| 工作树 | 保持干净 |

以上为评审侧独立复跑结果，与助手侧 §19.8 / §19.10 证据**相互印证但不互相替代**。

### 21.3 T2.5 收口

| 项 | 记录 |
|---|---|
| 状态 | **已收口**（2026-11-12 评审批准） |
| 批准依据 | 复核 `d799ff1` 无阻断代码问题；服务 32/32、独立探针 3/3、NF 基线可独立编译且失败于预期断言、`test_modulebase` 16/16、`test_runengine` 119/119、格式与 `diff --check` 通过、工作树干净 |
| 修复链 | `199bbfb → c60f76e(infra) → 2ef8011(test) → b2bfbec(fix) → 293d69b(style) → d799ff1(docs §19)` |
| 缺陷性质 | 真实缺陷（九轮整改）；NF 红灯来自 `QVERIFY2` 运行断言，非编译错误 |
| 收口范围 | **仅 T2.5**；不代表 R2 整批收口 |

### 21.4 T2.8 / T2.9 按既定契约分别记录

| 任务 | 性质与提交 | 记录口径 |
|---|---|---|
| T2.8 `CancellationToken` 语义 | `463d6b7`，行为契约／预防性重构；执行结束后模块不再持有 token | 按 §13 既定契约口径**分别记录**：基线已绿、不制造 NF 红灯、撤销态验证见 §13；**不与 R2 整批完成挂钩** |
| T2.9 `runId` 跨线程快照 | `b50b653`，预防性加固 | 同上，按 §13 契约口径分别记录；**不与 R2 整批完成挂钩** |

二者**分别记录、不合并叙述**，按裁定「不要把它们……等同于 R2 完成」。本轮未改动二者
源码，亦未替评审宣告其独立收口；其收口与否不与 R2 整批状态联动。

### 21.5 R2 整批：未收口

| 项 | 状态 |
|---|---|
| R2 整批 | **未收口**（2026-11-12 评审裁定） |
| T2.7 | `be90921` WIP 继续隔离、未合入、不作硬前置 |
| 单次全量 CTest 97/97（§19.10） | **有效证据，但不能替代整批要求的十轮连续门禁**（评审明确） |
| 十轮连续门禁 / `git revert` 抽查 | 客观执行与结果见 §20（`938fb22`）；**执行完成 ≠ 批次收口获批** |

### 21.6 时间线澄清（避免误读 §19）

评审复核基于 **`d799ff1`**（第九次整改 HEAD）。该提交的 §19.11 状态表与 `:2780`「本轮
未做的整批项：十轮连续门禁、`git revert` 抽查……仍留待整批」是**当时的真实状态**，按
「历史章节原文保留、不改写」纪律原样保留，故文中仍可见该表述。

本会话后续（2026-11-12 14:14–14:49）已按「继续完成 r2 整批」执行完毕并在 **§20** 留痕：

```text
938fb22  §20：十轮连续全量 CTest 10/10（NONZERO_RUNS=0）+ git revert 抽查复败于同一断言
8253e9d  §20.3 命令笔误更正
```

两者关系：**§20 记录批次门禁的客观执行结果；§21 记录评审对「整批是否收口」的裁定。**
评审未批准收口，故 R2 整批状态为「未收口」——这与 §19 当时的「尚未完成」不是同一
含义，也不以 §20 的执行完成去覆盖裁定。后续状态**以本章为准**。

### 21.7 本轮改动与遗留

| 项 | 内容 |
|---|---|
| 改动 | 仅 `docs/superpowers/plans/2026-11-05-r2-nf-evidence.md`：§19.9 根因口径更正、§19.11 后状态提示、新增本章 |
| 生产 / 测试代码 | **零改动** |
| 交付树 | 提交后 `git status --short` 为 0 行 |
| 非阻塞遗留 | QWARN 根因按「观察事实 + 可能解释」记录，线程 ID 复用解释**待后续独立探针验证**；不影响 T2.5 收口 |
| 临时工作树 | `/var/tmp/t25r9-wt` 仍保留为干净 `d799ff1` 状态供复核，清理待授权 |

## 22. T2.8 多借用者析构回归补充（2026-11-12）

### 22.1 新增覆盖

在 §13.5 的单模块 token 先析构用例之外，新增
`testCancellationTokenDtorDetachesAllBorrowers`：两个已初始化模块同时借用同一个栈上
`CancellationToken`；先确认两条借用均指向该 token，再销毁 token，最后分别确认两个模块的
借用均已清空，且调用取消查询安全并返回 `false`。这直接覆盖析构清理必须遍历并摘除所有借用者的语义。

该用例只验证顺序生命周期，不证明并发析构安全。独立只读复核结论为 **Ship**；复核明确了
这一证据边界。

### 22.2 交付与验证

| 项 | 结果 |
|---|---|
| 测试提交 | `ba30cbf`（仅 `tests/test_modulebase.cpp`，`+21`） |
| 工作树 | `feature/review-hardening`，`HEAD=ba30cbf`，提交后干净 |
| 格式与差异 | `clang-format --dry-run --Werror tests/test_modulebase.cpp`、`git diff --check` 通过 |
| 专项测试 | 全新构建目标 `test_modulebase` **17/17** |
| 全量构建 | 工作区外新目录 `/tmp/deeplux-r2-final-build`，`cmake --build ... -j4` 成功；`DEEPLUX_NATIVE_SAM=ON` |
| 全量 CTest | 沙箱外 `ctest --test-dir /tmp/deeplux-r2-final-build --output-on-failure -j4`，**97/97**，60.34s |
| 日志 | `/tmp/deeplux-r2-after-ba30cbf-build.log`、`/tmp/deeplux-r2-after-ba30cbf-ctest.log` |

第一次在沙箱内运行时，Qt 临时目录指向不可写的 `/var/tmp/gcc-tmp`，并且本地 socket 测试被沙箱拒绝；
该次结果不作为验收。改用可写 `TMPDIR=/tmp/deeplux-ctest-tmp` 并在沙箱外重跑后，完整 97 项通过。

### 22.3 状态边界

本节只补充 T2.8 多借用者行为证据，不代替 T2.8 独立收口裁定，也不改变 §21.5 的结论：
**R2 整批仍未收口**。本次 CTest 是新增回归后的单次全量验证，不替代 §20 的十轮批次门禁或 `git revert` 抽查。

## 23. T2.8 / T2.9 独立复核与最终态门禁（2026-11-12）

### 23.1 T2.8 独立复核

独立只读复核对 `463d6b7` 的 token 借用清理及 `ba30cbf` 的多借用者测试给出 **Ship**：析构遍历并清除所有指向该 token 的模块借用，顺序析构用例覆盖两个模块。复核确认 `96829f9` 已修正早期作用域守卫晚于输入校验的问题。

证据边界：测试不证明并发析构安全。映射锁释放后，模块仍通过裸指针读取 token；调用方必须保证 token 活到所有借用执行完成。测试也不证明同一模块重叠执行安全；批内重复实例由 T3.2 的引擎侧拒绝任务处理。该边界不扩大为“任意并发 token 生命周期均安全”的承诺。

### 23.2 T2.9 独立复核及 NF 复败

独立只读复核对 `2286519` / `571657b` / `42661b0` / `cd7cdd7` / `373d9dd` 提交链给出 **Ship**。复核检查两个正常完成入口均在 `finalizer.finish()` 释放执行租约前快照完整 `DisplayFrame`；发布时在生命周期锁下验证 `runId` 与 `jobId`，过期帧丢弃；生命周期锁→帧锁顺序一致，信号在锁外发射。回归用屏障先确认第二轮最终帧已发布，再放行第一轮迟到发布，并比较帧身份、图像与模块数据快照。

撤销复败在隔离工作树 `/tmp/deeplux-t29-revert-wt` 执行：基线 `373d9dd`，仅撤销生产修复 `cd7cdd7`；运行 `testFinalDisplayFrameKeepsCompletedRunIdWhenNextRunStarts` 返回 `rc=1`，同一断言失败：`publishedRunIds.size() == 6`，预期 `frameCountBeforeFirstPublish == 5`（测试文件约第 6276 行）。原始输出：`/tmp/deeplux-t29-revert-nf.log`。该失败与旧实现迟到最终帧额外发出通知相符；交付工作树未切换缺陷态。独立复核未能从旧 `build/bin/test_runengine` 找到该用例，因此测试执行证据采用本轮隔离撤销构建与下述最终态全量 CTest，而非该复核者的本机单测。

### 23.3 最终交付态连续门禁

| 项 | 结果 |
|---|---|
| 交付 HEAD | `373d9dd`；除本证据文档外，交付工作树无未提交源码改动 |
| CTest 注册数 | `ctest -N`：**97**（`DEEPLUX_NATIVE_SAM=ON`） |
| 连续全量门禁 | 正确启用本机 Unix socket 的环境中，`ctest --output-on-failure -j4` 连续 **10/10** 轮，每轮 **97/97**；日志：`/tmp/deeplux-r2-final-10x-socket-enabled/run-{1..10}.log` |
| 超时/跳过 | 10 轮日志中无失败、超时或跳过项；各轮耗时约 59.70–61.52 秒 |
| 关键套件 / watchdog | 每轮 `test_executionaffinity`、`test_flowexecutionservice`、`test_mainwindow` 均 Passed；10 轮无 watchdog 触发标记 |
| 初始沙箱尝试 | 受限环境拒绝 Agent/MCP 本地 socket，出现 9 个 socket 相关用例失败；该轮单独保留，不计入连续 10 轮 |
| 最终帧 NF | 仅撤销 `cd7cdd7` 后按同一回归断言复败，见 §23.2 |
| 格式/差异 | `clang-format --dry-run --Werror`（此前提交态验证）与本轮 `git diff --check` 通过 |

CTest 所用构建目录 `/tmp/deeplux-r2-final-build` 的 `RunEngine.cpp/.h` 与交付 HEAD 字节一致；测试源码与 `373d9dd` 的差异仅为该提交中的 clang-format 换行/对齐，不改变测试逻辑。10 轮门禁在生产代码和测试逻辑与当前提交一致的已构建二进制上执行。

### 23.4 复核结论与范围

- T2.8：独立复核 **Ship**，按顺序生命周期契约收口；并发 token 析构不在测试证明范围内。
- T2.9：独立复核 **Ship**；撤销生产修复后同一回归断言复败，最终态 10×97/97 通过。
- R2 批次门禁：最终态连续 CTest 与撤销复败抽查均满足；T2.5 的撤销证据仍见 §20.5。T2.7 是经裁定的预防性重构，继续隔离且非 R2 收口前置。
- 本节记录 R2 各任务与批次客观门禁的复核结果；不代表 R3、R4、D 线或整体计划完成。
