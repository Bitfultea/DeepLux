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
