# 本轮待办表（阶段 A 产出，2026-08-17）——实时状态页

> 状态枚举：完成 / 部分完成 / 未开始 / 外部阻塞。
> 本表为**统一实时状态页**；各 `phaseN-acceptance-record.md` 为**验收快照**（记录当时点结论，不随后续修改更新）。
> 基线：分支 `main`，HEAD `2a132d7`，工作树干净，`ctest` 58/58。

| 项 | 状态 | 说明 |
| --- | --- | --- |
| 阶段 0 基线/清理/验收工程 | 部分完成 | 矩阵+hotfix 映射+死入口清理+acceptance 工程+正式截图(1920/1280 深浅)+旧版输出端口静态对照已做；逐值运行结果等价核验、多类验收工程未完成 |
| 阶段 1 ABI v2/端口/契约 | 完成 | IModule/2.0、端口校验、契约测试 |
| 阶段 1 复杂载荷类型 | 部分完成 | Mask/Region/DetectionList 仍过渡性宽松类型 |
| 阶段 2 工程 3.0/迁移器 | 完成 | flows/resources/migration、幂等迁移、.v2.bak |
| 阶段 2 note/enabled/breakpoint | 完成 | 本轮阶段 B 已交付 |
| 阶段 3 数据 DAG/并行原语/Skipped | 部分完成 | 拓扑/扇出/汇合/预检/并发/Skipped 已交付 |
| 阶段 3 控制端口驱动执行/汇合 all-any | 完成 | 本轮阶段 C/D 已交付（控制边契约+控制图执行 D1/D2/D3） |
| 阶段 3 Parallel 接入主循环/解除实验性 | 完成 | Parallel 控制端口、主循环批次、线程安全白名单和回归测试已交付 |
| 阶段 E 多输入聚合/Parallel | 完成 | multiple 聚合、controlJoinPolicy、批量并行、blocking 排除已交付 |
| 阶段 F 画布端口交互 | 完成 | 字符串端口、四元组连接、拖线、数据/控制边样式已交付 |
| TSan 并发验证 | 部分完成 | 已实际执行（需 setarch -R 关 ASLR）；检出 20 处警告，多为未插桩 Qt5 误报，未清零不标通过；见 `tsan-report.md` 与 `tsan-runengine-full.txt` |
| 正式尺寸 GUI 截图(1920/1280 深浅) | 完成 | 环境可离屏渲染，已产出 4 张 `screenshots/formal_{1920,1280}_{dark,light}.png`，深浅像素差异已验证 |
| 阶段 G 13 重构插件/业务包 | 部分完成 | metadata execution 标记+hotfix 映射结论/证据+TimeSlice 修正+blocking 接入+13 插件行为级测试（64/64）已完成；旧版输出端口静态对照已完成（50 插件，见 `legacy-comparison.md`），逐值运行结果等价仍未做，故映射维持 partial |
| 阶段 H 生产验收/交接报告 | 完成 | 见 `phaseH-handover-report.md`；执行与交接闭环完成，64/64 测试、格式门禁、截图/TSan/旧版对照证据齐全；生产门禁遗留仍按本表“部分完成”项跟踪 |

## 本轮执行顺序（A→H）

- A 基线冻结（本表）→ B 模块字段 → C 控制边契约 → D 控制图执行(D1/D2/D3) →
  E 多输入+Parallel → F 画布端口交互 → G 插件收口 → H 生产验收+交接。

## 阶段 H 后复核整改（步1–6）

| 步 | 内容 | 状态 | 提交 |
| --- | --- | --- | --- |
| 1 | P0 修复：LinesDistance 有限线段、FreeformSurface 凸包面积 | 完成 | 8c0eb08/e45e881 |
| 2 | 点集→直线拟合验收流程 + 带结果工程截图 | 完成 | 2ce0e6c |
| 3 | 核心插件收口，四级结论判定 + Matching 输出补齐 | 完成 | 377a9d2 |
| 4 | Circle2D/DetectionList 强类型载荷 + FindCircle 端口式输出 | 完成 | 134489b |
| 5 | 长循环/停止/取消压力测试 + TSan 报告补充 | 完成 | 14db1a0 |
| 6 | CI 移除静默失败 + 格式/Windows 门禁 + 硬件模拟/业务包说明 | 完成 | 本提交 |

### 步6 说明

- **CI**：`ci.yml` 移除 `sync-plugins || true` 静默失败；新增 `format-gate`
  （clang-format 全量 `--dry-run --Werror`）与 `windows-msvc` 编译门禁。
- **硬件模拟**：相机验收以 `GrabImage(File)` 作为无硬件模拟源（固定测试图像）；
  PLC/AI 无模拟器，相关验收保持"依赖设备"标记，先契约测试。
- **业务包**：7 个业务包依赖已在 `hotfix-plugin-mapping.json` 记录
  （`dependency_recorded`），需现场硬件验收，不在本环境闭环。
