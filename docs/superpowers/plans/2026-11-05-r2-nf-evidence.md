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
- 全量 CTest 与 `-j4` 稳定性结果见第 4 节。

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

### 4.3 失败归因：既有 flaky 测试，非本轮回归

- 失败集中在同一测试 `test_workerpluginconsistency::testCandidateConsistency`，断言 `failures.isEmpty()`（:689），错误恒为 `port 'image': image payload differs`；但**插件与场景各不相同**（`DataCheck/same-instance`、`MeasureLine/two-instances` 等），呈非确定性。
- 该测试对 `ImageData::toQImage()` 逐像素 SHA256（`imageSignature`，:255），把 GUI 执行 / 同实例 worker 执行 / 另一实例 worker 执行三者严格比对（:660–670）。
- **与本轮改动无关的证据**：
  - `git diff 96e02fa HEAD -- tests/test_workerpluginconsistency.cpp` 为**空**；
  - 该文件不引用 `FlowExecutionService`、退出屏障 hook 或任何本轮新增符号；
  - 本轮生产改动仅为新增 test-only 空钩子（`FlowExecutionService.{h,cpp}`），既有函数体未变；
  - 该文件最后一次改动来自 execution-order 线（`96a5036` 等），早于 R2。
- **独立量化（排除 `-j4` 并行干扰）**：单独执行 `./build/bin/test_workerpluginconsistency` 20 次（同一环境），**16 通过 / 4 失败（20% flake）**，失败插件为 `MeasureRect`、`If`、`DistancePL`、`MeasureLine`，错误同为 `image payload differs`（日志 `/var/tmp/t21-wpc-single-{1..20}.log`）。与 10× 全量中 2 次失败吻合。
- 结论：`test_workerpluginconsistency` 是**既有非确定性测试**，不是 T2.1 引入的回归；T2.1 自身的确定性证据（1.4 节：修复在位 20/20、两个变体各 10/10 复败、服务套件 18/18、全量构建 exit 0）不受影响。但 **R2 批次「全量 CTest 连续 10 次通过」门禁因该 flake 未达成**，须作为独立已证实问题单列，待评审裁定是先行修复还是纳入门禁豁免；本轮**不擅自放宽或修改该测试**。

### 4.4 其他门禁

| 项 | 命令 | 结果 |
|---|---|---|
| 格式 | `clang-format 14 --dry-run --Werror` 于 `FlowExecutionService.{h,cpp}`、`test_flowexecutionservice.cpp` | **exit 0** |
| 全量构建 | `cmake --build build -j4` | **exit 0** |
| T2.1 单测 | `./build/bin/test_flowexecutionservice testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach` | 20/20 通过，exit 0 |
| T2.1 套件 | `./build/bin/test_flowexecutionservice` | 18/18 通过 |
