# TSan 并发验证报告（收尾1）

> 日期：2026-08-24
> 目标：按 `round-todo.md` 要求实际执行 TSan 并发验证，保留完整证据。

## 结论（先说）

**TSan 门禁不能标记为"通过"。** 实际运行检出 **20 处 data race 警告**，全部集中在并行执行路径。
其中绝大部分栈帧落在**未经 TSan 插桩的 Qt5 类型**（QMutex/QHash/QRunnable/QThreadPool）上，
属于"未插桩同步原语"导致的**高概率误报**，但在不重编 Qt 的前提下无法逐一排除，故如实记录为"警告未清零"。

功能侧：`test_runengine` 在 TSan 构建下 **97/97 全部通过**（0 failed），说明竞争未导致功能性失败。

## 环境与方法

| 项 | 值 |
| --- | --- |
| 编译器 | gcc 11.4.0 (Ubuntu 22.04) |
| 内核 | 6.8.0-124-generic |
| Qt | 5.15.3（系统，**未**用 `-fsanitize=thread` 重编） |
| 构建 | `build-tsan`，`-fsanitize=thread -g`，Debug |
| 运行对象 | `build-tsan/bin/test_runengine`（97 用例） |

### 关键环境发现：必须关闭 ASLR 才能运行 TSan

直接运行 TSan 二进制（哪怕是无 Qt 的最小 4 线程程序）会立即报：

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x...-0x...
(exit 66)
```

- 最小复现程序（仅 `std::thread`，无任何项目/Qt 代码）同样触发，证明这是
  **内核 6.8 ASLR 与 gcc 11 TSan 运行时的环境级冲突**，与本项目代码无关。
- 解决办法：`setarch $(uname -m) -R <binary>` 关闭地址空间随机化后，TSan 正常工作
  （最小程序能正确检出人为植入的数据竞争）。
- 复现与验证命令：
  ```
  setarch $(uname -m) -R build-tsan/bin/test_runengine
  ```

## 检出结果

- 警告总数：**20** `WARNING: ThreadSanitizer: data race`
- 完整日志：`docs/baseline/tsan-runengine-full.txt`（1353 行）
- 功能测试：**97 passed / 0 failed**

### 警告分布（按我们代码的栈帧去重统计）

| 位置 | 出现次数 | 说明 |
| --- | ---: | --- |
| `RunEngine::executeBatchParallel` | 26 | 并行批次 lambda 内 |
| `RunEngine::executeParallel` | 16 | 旧并行原语 |
| `ModuleBase::execute` | 7 | 端口执行 |
| `ModuleBase::setCancellationToken` (ModuleBase.cpp:228) | 5 | 全局 token 表 |
| `RunEngine::executeRunWithControlGraph` / `executeRun` / `runOnce` | 若干 | 调度入口 |

### 误报判定依据

`ModuleBase::setCancellationToken` / `cancellationToken()` 对 `g_cancellationTokens`（QHash）的
读写**均已用 `g_cancellationTokensMutex`（QMutex）加锁**（ModuleBase.cpp:226/235），
但 TSan 仍报竞争。原因：**系统 Qt5 未用 TSan 插桩**，TSan 无法识别 QMutex 建立的
happens-before 关系，凡 QMutex/QReadWriteLock 保护的共享数据都会被误报。
栈帧中 42 处落在 `qhash.h` / `qrunnable.cpp` / QMutex 相关符号，进一步支持"未插桩 Qt 误报"判断。

### 需要人工复核的潜在真实问题

以下不属于"明显误报"，建议后续用插桩 Qt 或代码审查确认：

1. **`emit moduleStarted(mod)` 在工作线程内发射**（RunEngine.cpp:926）：
   若下游以 DirectConnection 连接并触碰 GUI/非线程安全状态，存在真实风险。
2. **`pipelineData`（ImageData）按值捕获进并行 lambda**：ImageData 隐式共享（COW），
   多线程拷贝/读取可能对引用计数产生竞争。建议确认 `collectModuleInputs` 是否只读。
3. **`m_frameId.fetch_add(relaxed)` 与 `m_runId` 读取**：`m_runId` 为 QString，
   工作线程读取时需确认无并发写。

## 步5 补充的压力/线程归属测试

在 `tests/test_runengine.cpp` 新增（100 用例全过）：

| 测试 | 验证 |
| --- | --- |
| `testLongLoopNoFramePollution` | 200 次循环无上一帧污染：body 计数=200、after 仅 1 次 |
| `testStopDuringLoopRun` | 长循环中 `stop()`（后台运行+主线程 stop）中止执行，迭代数远小于上限 |
| `testCancelDuringParallelBatch` | 并行批次中取消中止剩余模块，状态非 Running |

这些覆盖"连续循环、停止、取消"压力场景；线程归属（信号在工作线程发射）见上节
"需要人工复核的潜在真实问题#1"。

## 门禁判定

- 依据 `round-todo.md`："TSan（不支持则保留错误记录不写通过）"。
- 本次已**实际执行**并**保留完整错误记录**（本报告 + 全量日志）。
- **不写"通过"**：存在 20 处未清零警告；要清零需以 `-fsanitize=thread` 重编 Qt5 后复测，
   或逐条人工确认上述潜在真实问题并修复。

## 后续建议（非本轮强制）

1. 用 `-fsanitize=thread` 重编 Qt5.15.3 后复测，可消除绝大部分"未插桩"误报。
2. 将并行批次内的 `emit moduleStarted` 改为 `Qt::QueuedConnection` 或移出工作线程。
3. 审查 `pipelineData` / `m_runId` 在并行路径的读写安全性。

## 阶段 6 复核（并发风险收口）

> 日期：2026-09-05。对应提交见 `git log`（阶段 6 及 stop() 复核二~八轮）。功能侧
> `test_runengine` **115/115 通过**（含 13 个定向/回归测试与 50 次并行压力），全量 CTest 66/66。

### 风险处理与分类

| 风险 | 阶段 6 处理 | 分类 |
| --- | --- | --- |
| #1 工作线程 `emit moduleStarted` | 审计全部 RunEngine 信号连接均带上下文对象（Auto→Queued），无跨线程 DirectConnection 直操 QWidget；`testParallelSignalsDeliveredOnReceiverThread` 证明池线程信号排队回接收者线程 | 已证明（定向测试） |
| #2 `pipelineData`(ImageData) 按值进并行 lambda | 每任务独立副本、`collectModuleInputs` 对 `m_nodeOutputs`/连接只读、批次结果仅在等待结束后由调用线程写回 | 已证明（代码审计） |
| #3 工作线程读 `m_runId`(QString) | 调用线程快照 `runId`/`token` 按值捕获；`executeParallel()` **始终**自生成局部 runId（不读 `m_runId`，连续独立调用 ID 不同）；runId="毫秒+单调序号" | 已修复（定向测试） |
| #4 生命周期租约不排他（P0：executeParallel 绕过、维护 check-then-act、stop 被启动覆盖） | 四轮重构为**单一排他租约**：执行（runOnce/onTimerTick/resume/stepOnce）经 `tryBeginExecution()`/`tryAcquireLease()`、维护（add/remove/clear/load）经 `tryAcquireMaintenance()`、公开 `executeParallel()` 经 `tryAcquireLease()`，三者共用 `m_lifecycleMutex` 互斥；**取权与 runMode/state 提交、token 重置同临界区**（消除启动窗口）；维护检查与修改同租约原子（消除悬空指针/混合工程）；早退路径复位 state 防卡死 | 已修复（SEGV/HUAf 清零） |
| #5 并发生命周期回归（复核三/四/五轮，真信号量屏障） | 五轮改用 **QSemaphore 真实同步点**：`GateModule` 进入 process 释放 entered（执行租约必已持有），主线程 acquire 后 stop 再放行，断言停止获胜/后继未执行/状态 Stopped；`testConcurrentMaintenanceDuringRunStart` 工厂阻塞在维护租约内，断言 runOnce 被拒绝(runStarted=0)、stop 不并发清理、load 成功后模块未被清除。另含 `testStopSimultaneousWithBreakpointHit`、`testClearAndLoadRejectedWhileRunning` | 已证明（确定性屏障） |
| #7 收尾/恢复非原子（复核七轮）：正常结束先释放执行权再设 Idle、校验早退重复收尾、恢复判定与取权分离、断点信号释放后读成员、单步先释放后清理、维护期停止被丢弃 | 新增 finalizeRunTail()（清理+终态+释放同临界区）、tryBeginForRun()（恢复判定+暂停数据转移+取权同临界区）、校验早退仅一次 finalizeAbortedRun、断点模块 ID 锁内复制后发信号、单步清理与释放同临界区、releaseMaintenance() 补做维护期停止清理、恢复目标模块不存在时按中止收尾 | 已修复（TSan SEGV/HUAf=0） |
| #8 恢复入口未原子+循环断点退化+缺回归（复核八轮） | tryBeginForRun 引入 RunIntent{Single,CycleTick,Resume,Step} 并在锁内校验预期状态（Single/Step:Idle/Stopped；CycleTick:Running+RunCycle；Resume:Paused+完整暂停上下文）；commitPause 保存 m_pauseRunMode、Resume 还原（循环断点恢复仍循环）；resume()/start() 的定时器启动+信号+日志与状态提交同转换（stop 介入不启定时器、不误导日志）；Step 的 fresh 由 m_stepCurrentModuleName 锁内判定（连续单步共享 runId、frameId 递增） | 已修复（结构上关闭竞态） |
| #9 回归证据强度（复核九轮） | 新增 `testConsecutiveStepsShareRunIdAndIncrementFrameId`（同 runId+frameId 递增，证明 Step 不重置上下文）。**说明**：`testResumeAfterStopDoesNotReexecute` 与 `testStaleCycleTickAfterStopDoesNotExecute` 为**顺序行为测试**（先 stop 再 resume/等待），父提交亦可通过，仅作行为守卫；竞态的结构修复由 RunIntent 锁内意图校验保证，未以父提交负验证 | 顺序行为测试（非竞态证据） |
| #6 锁未闭合（复核五轮 P0/P1）：暂停态锁外读、stop 绕过维护租约、stepOnce 非原子、租约不隔离 Running/Paused、早退覆盖停止 | 暂停态（m_pausedAtBreakpoint/m_pauseResumeModule/m_pausePipelineData/m_breakpointPausedAt）全部改为 `m_lifecycleMutex` 内读写（resume/executeRun/内层 setInnerPauseLocked/isPausedAtBreakpoint）；stop() 尊重 m_maintenance 不并发清理；stepOnce 改用 tryBeginExecution 原子启动+锁内收尾（stop 时重置单步态）；tryAcquireLease 拒绝 Running/Paused；空模块/校验早退改 finalizeAbortedRun 原子收尾不覆盖 Stopped；isBusy() 含 m_maintenance；clearBreakpointPauseState()/removeModule 暂停态写入加锁（m_lifecycleMutex 改 QRecursiveMutex 允许重入） | 已修复（TSan SEGV/HUAf=0） |

### 门禁结论（仍不写"通过"）

`build-tsan` 复跑（`setarch -R`，原始日志见 `tsan-runengine-full.txt`）：
**heap-use-after-free 与 SEGV 均为 0**；功能侧 109/109 全过。data race 警告数**随调度
变化**（实测样本曾在 47–83 间波动），不维护封闭区间、不以此作确定结论。

按调用栈分类（均为高概率误报）：
- `ImageData` 隐式共享引用计数跨线程拷贝（Qt 未插桩，原子引用计数无法建立 happens-before）；
- `executeBatchParallel`/`executeParallel` 池任务拷贝 `ImageData`/`ExecutionContext`（同上）；
- `CancellationToken`/`ModuleBase` 的 `g_cancellationTokens` QHash（已用 QMutex 保护，TSan 不识别未插桩 QMutex）；
- `qthreadpool`/`qhash.h` 等未插桩 Qt5 内部。

依据门禁规则"不把未确认 TSan 警告写成通过"，阶段 6 维持 **TSan 不通过** 结论：
#3/#4 已修复、#1/#2/#5 已证明；残余误报需以 `-fsanitize=thread` 重编 Qt5 后复测清零。
