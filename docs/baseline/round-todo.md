# 本轮待办表（阶段 A 产出，2026-08-17）

> 状态枚举：完成 / 部分完成 / 未开始 / 外部阻塞。
> 基线：分支 `main`，HEAD `2a132d7`，工作树干净，`ctest` 58/58。

| 项 | 状态 | 说明 |
| --- | --- | --- |
| 阶段 0 基线/清理/验收工程 | 部分完成 | 矩阵+hotfix 映射+死入口清理+acceptance 工程已做；正式截图、插件等价核验、多类验收工程未完成 |
| 阶段 1 ABI v2/端口/契约 | 完成 | IModule/2.0、端口校验、契约测试 |
| 阶段 1 复杂载荷类型 | 部分完成 | Mask/Region/DetectionList 仍过渡性宽松类型 |
| 阶段 2 工程 3.0/迁移器 | 完成 | flows/resources/migration、幂等迁移、.v2.bak |
| 阶段 2 note/enabled/breakpoint | 完成 | 本轮阶段 B 已交付 |
| 阶段 3 数据 DAG/并行原语/Skipped | 部分完成 | 拓扑/扇出/汇合/预检/并发/Skipped 已交付 |
| 阶段 3 控制端口驱动执行/汇合 all-any | 完成 | 本轮阶段 C/D 已交付（控制边契约+控制图执行 D1/D2/D3） |
| 阶段 3 Parallel 接入主循环/解除实验性 | 完成 | Parallel 控制端口、主循环批次、线程安全白名单和回归测试已交付 |
| 阶段 E 多输入聚合/Parallel | 完成 | multiple 聚合、controlJoinPolicy、批量并行、blocking 排除已交付 |
| 阶段 F 画布端口交互 | 完成 | 字符串端口、四元组连接、拖线、数据/控制边样式已交付 |
| TSan 并发验证 | 部分完成 | 已实际执行（需 setarch -R 关 ASLR）；检出 20 处警告，多为未插桩 Qt5 误报，未清零不标通过；见 `tsan-report.md` 与 `tsan-runengine-full.log` |
| 正式尺寸 GUI 截图(1920/1280 深浅) | 完成 | 环境可离屏渲染，已产出 4 张 `screenshots/formal_{1920,1280}_{dark,light}.png`，深浅像素差异已验证 |
| 阶段 G 13 重构插件/业务包 | 部分完成 | metadata execution 标记+hotfix 映射结论/证据+TimeSlice 修正+blocking 接入已完成；13 插件行为级测试已补（64/64），旧版等价核验未完成 |
| 阶段 H 生产验收/交接报告 | 未开始 | 本轮阶段 H |

## 本轮执行顺序（A→H）

- A 基线冻结（本表）→ B 模块字段 → C 控制边契约 → D 控制图执行(D1/D2/D3) →
  E 多输入+Parallel → F 画布端口交互 → G 插件收口 → H 生产验收+交接。
