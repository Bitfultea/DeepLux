# R2 NF 验收记录（负验证证据链）

本文件记录 R2 并发修复的负验证（NF）证据：版本、命令、预期断言与结果。
不存放原始日志；原始输出仅按需在评审时提供。

约定：
- 每个任务两个提交——① `test:`（NF 用例）② `fix:`（生产修复）。
- NF 三步：① 提交上用例在未修复代码失败于预期断言；② 修复后全量通过；③ 临时校验工作树 `git revert <fix>` 后复败于同一断言，随后删除该工作树。
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
| T2.3/T2.4 | 暂缓 |
| T2.7 WIP | 继续独立保留，`be90921` 不合入 |
| 结果 | ①–⑤ 全部完成（5.1–5.6）；修复后全量连续 10 次 CTest 门禁 **10/10 通过**（5.6 节），无遗留门禁项 |

### 5.1 失败诊断：捕获双方图像、首个不同像素与差异范围

- `b0b60ac`（受版本控制）新增 `dumpImageDifference()`：失败路径保存双方 PNG，输出首个不同像素、RGBA、差异像素数、差异包围盒；输出目录 `DEEPLUX_IMAGE_DIFF_DIR`（缺省 `/var/tmp/deeplux-image-diff`）。**严格判等未放宽**。
- 固定环境 `unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`，30 次复现 28 pass / 2 fail，日志 `/var/tmp/t21-diag-run-{1..30}.log`。两次失败均 `differentPixels=0/76800`、`firstDiffPixel=(-1,-1)`，双方 PNG sha256 相同（`23f4c911…`）：**失败时两图像素完全相同，但 `imageSignature` 判不等**。
- 临时诊断工作树（未提交，diff 存档 `/var/tmp/t21-image-signature-diagnostic-wip.diff`，含签名复算、`sigStable`、指针/格式/全图校验和/`convStable` 轨迹）：`/var/tmp/t21-diag2-run-{1..30}.log` 29 pass / 1 fail、`/var/tmp/t21-diag3-run-{1..30}.log` 27 pass / 3 fail，失败均 `sigStable=0` 且 dump 中 `sigEqual=1`。证明**同一 `ImageData` 连续两次 `imageSignature()` 结果不同**，与 GUI/worker 输出差异无关。

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

- 判别诊断（临时工作树，存档同上）：失败时四次签名调用的 `src`/`conv` 指针、`cacheKey`、`fmt`（RGB32=4 / RGBA8888=17）、`bpl=1280`、图像头完全相同；但 `sig` 在不同调用间取 `fd847c87…`（正确）、`47df5431…`、`6072a4f8…`、`1c586c0e…`。对**同一 `converted` 连续两次 SHA256** 出现 `convStable=0`（`47df…` → `fd84…`），而随后 FNV 全图校验和 `convCrc=db4b0a34` 四次恒定。
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
- 定位过程的临时诊断工作树不进入交付：diff 存档 `/var/tmp/t21-image-signature-diagnostic-wip.diff`；交付分支工作树已恢复到 `b0b60ac` 后仅保留上述最小修复与新增用例。

### 5.5 NF 三步证据链（修复版 / 缺陷变体 / 恢复）

脚本 `/var/tmp/t21-fix-verify.sh`，日志与逐次结果 `/var/tmp/t21-fix-verify/`。环境固定：`unset DISPLAY` + `QT_QPA_PLATFORM=offscreen` + `TMPDIR=/var/tmp/deeplux-ctest-tmp`。

| 阶段 | 版本 | 命令/次数 | 结果 | 退出码 |
|---|---|---|---|---|
| 修复版全套 | 修复在位 | `test_workerpluginconsistency` 1 次 | 49/49 通过 | 0 |
| 修复版重复 | 修复在位 | 单测连续 **100 次** | **0 失败**（`fixed-100.tsv` 全部 `rc=0`） | 0 |
| 缺陷变体复败 | 临时工作树把 `imageSignature` 改回逐行分块 `addData` | 单测连续 **60 次** | **37 次失败**（61.7%） | 1 |
| 恢复修复版 | 源码从备份还原并重编 | 单测 1 次 | 49/49 通过；源码 sha256 `8c7ede528728ad44…` 与修复版一致（`RESTORE_VERIFIED=yes`） | 0 |

- 缺陷变体 37 次失败中：**33 次**失败于新增用例 `testImageSignatureStabilityAndSensitivity() Compared values are not the same`（200 次签名不稳定），**5 次**失败于原有 `testCandidateConsistency(...): port 'image': image payload differs`（其中 1 次两者同时失败）。后者 `firstDiffPixel=(-1,-1)`、`differentPixels=0/76800`，与 #5 原始失败形态完全一致。
- 缺陷变体源码 sha256 `1ee233da03d51c5…`、构建 `VARIANT_BUILD_rc=0`，证明变体确实进入二进制；恢复后 `RESTORED_SOURCE_sha256=8c7ede528728ad44…` 与修复版相同。
- **负验证（比较强度未降低）**：新增用例在修复版上通过，输出 `firstDiffPixel=(5,5) refRGBA=000000ff actRGBA=ff0000ff; differentPixels=1/76800; diffBBox=(5,5)-(5,5)` —— 真实 1 像素差异仍被拒绝；相同内容仍判等。未排除任何测试、未放宽严格比较、未靠重试到绿。

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
| T2.3/T2.4 | 暂缓（评审裁定，未启动） |
| T2.7 WIP | 继续独立保留于 `be90921`，**未合入** |
| 门禁豁免 | 未授予 |
