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

评审按 `57f8438` 复核，**仍有 1 项 P1，暂不批准本轮整体收口**。5.7 / 6.5 / 7.8 三处「T2.4 方案二已收口」为整改前的历史记录，**原样保留不改写，其状态以本节 8.6 为准**：T2.4 行为契约的技术实现已复核通过，但整体收口需在本节 P1 / P2 整改后重新送审。

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
| **本轮整体收口** | **待评审复核** —— 按裁定原为「暂不批准」，本节整改后重新送审，**不改写为已批准/已收口** |
| T2.7 WIP / R2 整批 | `be90921` 继续独立保留、未合入、不作硬前置；**R2 整批未完成** |
