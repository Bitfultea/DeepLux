# R2 NF 验收记录（负验证证据链）

本文件记录 R2 并发修复的负验证（NF）证据：版本、命令、预期断言与结果。
不存放原始日志；原始输出仅按需在评审时提供。

约定：
- 每个任务两个提交——① `test:`（NF 用例）② `fix:`（生产修复）。
- NF 三步：① 提交上用例在未修复代码失败于预期断言；② 修复后全量通过；③ 临时校验工作树 `git revert <fix>` 后复败于同一断言，随后删除该工作树。

---

## T2.1 析构期协作关闭（禁止强杀 / 禁止 detach）

| 项 | 值 |
|---|---|
| 测试提交 | `e28e223` |
| 修复提交 | `c262d73` |
| 基线条目 | `f1b615f` |

命令（构建目录 `build/`，`QT_QPA_PLATFORM=offscreen`，环境清除 `DISPLAY`）：

```bash
./build/bin/test_flowexecutionservice testShutdownReportsDeadlineAndJoinsWorkerWithoutDetach
```

预期断言：注入 `setShutdownDeadlineForTest(0)` 后，`shutdown()` 仍返回 true 且
`shutdownDeadlineHitCountForTest() >= 1`（有界诊断命中），随后线程可重建再跑。

| 阶段 | 结果 |
|---|---|
| 测试提交（未修复） | **失败** — `'service.shutdownDeadlineHitCountForTest() >= 1' returned FALSE`，test_flowexecutionservice.cpp:388 |
| 修复后 | 通过；该套件 18/18 |
| 临时工作树 revert `c262d73` | **复败** — 同一断言、同一行 388 |
| 修复后全量 CTest | 96/96 |

---

## T2.2 GUI 回投生命周期（Pending/Running/Finished/Cancelled）

| 项 | 值 |
|---|---|
| 测试提交 | `f22e37b`（首版）；用例强化随本记录提交 |
| 修复提交 | `ac460c9` |
| 基线条目 | `c262d73` |

命令：

```bash
./build/bin/test_executionaffinity \
  testGuiDispatchTimeoutDropsLateCallback \
  testGuiDispatchTimeoutWhileRunningWaitsForCompletion
```

预期断言：
1. `DropsLateCallback`：不泵 GUI 事件致超时后，`runOnce` 必须已返回、模块 `runs==0`、错误码 `affinity_dispatch_timeout`；随后泵事件时迟到回调被丢弃（drop 计数 ≥1）且不阻塞。
2. `WhileRunningWaitsForCompletion`：独立控制线程在回调 Running 且超过超时后检查——`returnedBeforeRelease==false`（runOnce 未提前返回）、`runOnceAsync()` 被拒绝、放行后 `runs==1`、错误码 `affinity_dispatch_timeout`。

| 阶段 | 结果 |
|---|---|
| 测试提交（旧 `BlockingQueuedConnection`） | **失败** — `runOnce must return after the dispatch timeout`，test_executionaffinity.cpp:190/193 |
| 首版用例（评审发现假阳性） | 泵事件同步进入探针并阻塞至其 5s 超时，放行发生在回调结束之后；删除 `record->done.acquire()` 的缺陷变体仍通过 → 判为无效，已重写 |
| 重写后 + 修复在位 | 通过；亲和性套件 9/9（656 ms） |
| **缺陷变体**（临时注释 `record->done.acquire()`） | **失败** — `'!returnedBeforeRelease.load()' returned FALSE`，test_executionaffinity.cpp:264，254 ms |
| 临时工作树 revert `ac460c9` | **复败** — `runOnce must return after the dispatch timeout`（同一用例） |
| 修复后全量 CTest | 96/96 |

说明：首版用例的失败原因与修正过程一并记录，避免后续把假阳性当作已验收证据。
