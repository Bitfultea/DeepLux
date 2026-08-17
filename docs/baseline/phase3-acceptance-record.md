# 阶段 3 验收记录：图执行引擎与真实端口路由（数据 DAG 已交付，控制边待交付）

> 对应《DeepLux C++ 重构版完整能力建设计划》阶段 3。
> 本阶段采取增量安全策略：保持现有顺序/控制流路径不变，新增并行原语与状态，避免回归。

## 已交付

### 3.3 真正并行执行（核心）
- `RunEngine` 新增受控 `QThreadPool`：`setParallelThreadCount(n)`，默认 `max(1, CPU核心数-1)`。
- `executeParallel(names, sharedInput)`：并发执行独立模块；任一失败取消同组（经 CancellationToken）
  并返回首个错误+诊断；等待全部完成。
- 测试 `testParallelExecutesConcurrently`：4 个睡眠模块，`lastParallelMaxConcurrency()>1` 证明**真实并发**。
- 测试 `testParallelFailureCancelsGroup`：失败模块使组返回失败且 userMessage 非空。

### 3.2 显式控制流（声明层+Skipped 已交付）
- `PortSpec` 增加 `control` 标志，数据/控制端口分离；PluginManager 解析 `control` 字段。
- 控制插件声明控制端口：If/Condition→true/false；Loop/While→body/done；StopLoop→stop
  （由 generate_ports.py 的 CONTROL_PORTS 生成）。
- `moduleSkipped(moduleId)` 信号：执行循环中控制节点前向跳过时，被跳过的未激活分支节点
  发射 Skipped（非失败）；测试 `testSkippedBranchEmitsSkippedNotFailed` 验证。
- 数据图环检测：buildExecutionOrder 拒绝连接环；循环仅经控制节点（flowControlType）形成。
- 汇合 all/any 策略与控制端口驱动执行**未做**（现控制流仍基于 flowControlType，见遗留）。

### 3.1 数据 DAG 端口路由（已交付，载体兼容式）
- `buildExecutionOrder()` 使用稳定拓扑排序，支持扇出、多个独立链和不同命名端口的汇合；数据环明确拒绝。
- 图装载期校验源/目标节点、端口存在性、数据类型与单输入端口的多连接约束；旧工程未标端口仅在内存中回退为 `image -> image`。
- `executeModule` 仅从当前节点入边读取显式图数据，不再继承其他分支输出；每节点每端口输出缓存供下游/单步使用。
- `validateFlow()` 已接入 `runOnce()`、循环执行和 `stepOnce()`，必需输入缺失会在执行前报错并定位节点+端口。

## 验证

```text
cmake --build build -j2  → 成功
ctest -R '^test_runengine$' → 1/1 通过
```

## 验收标准核对

| 标准 | 状态 |
| --- | --- |
| 顺序/扇出/多链/类型拒绝自动测试 | ✅ `test_runengine` 覆盖稳定拓扑、独立链、扇出汇合和类型拒绝 |
| If/Loop/While/StopLoop 显式控制边自动测试 | ⚠️ 旧 `flowControlType` 顺序路径已覆盖；控制端口图调度未实现 |
| 1000 循环无上一帧污染 | ✅ 新帧清除 m_nodeOutputs 端口缓存 |
| 并行证明真实并发 | ✅ testParallelExecutesConcurrently |
| 单步/停止/错误传播/耗时正确 | ✅ 现有测试 |
| ThreadSanitizer 无数据竞争 | ⚠️ 未在 TSan 下验证（环境未启用） |

## 遗留（需专项重写）

1. 汇合节点 all/any 控制策略、控制端口驱动执行未实现；现控制流仍基于 flowControlType 的 getNextModule
   （控制端口已声明但未驱动执行）。
2. Parallel 控制节点未接入主循环（executeParallel 为独立原语）；`multiple=true` 端口需要定义聚合载荷后才能运行期传递多个同名值。
3. 建议在独立分支做 RunEngine 图重写，配 ThreadSanitizer 验证。

## 回滚

- 并行原语为新增 API，不影响现有路径；回滚 `git revert` 即可。
