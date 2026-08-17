# 阶段 0 验收记录

> 对应《DeepLux C++ 重构版完整能力建设计划》阶段 0（基线与伪完成清理）。
> 验收规则：完整构建、全部自动测试、插件同步无错误、GUI 截图、文档与行为一致。

## 当前状态

阶段 0 处于补齐验收阶段，尚未达到完成门禁。本记录只陈述已复验的事实，不能替代提交记录。

## 0.1 能力基线

- 产物：`docs/baseline/capability-matrix.md`（当前 59 插件逐项：当前能力/状态/迁移决定/优先级）。
- hotfix 对照：`hotfix-plugin-mapping.json` 固定了旧版 110 个插件目录；当前得到 45 个直接匹配、
  5 个候选匹配、53 个缺失候选和 7 个业务包候选。
- 迁移决定汇总：保留 44、重构 13、业务包 3、替代 0、淘汰 0。
- **遗留问题（阻塞项）**：名称映射不等于能力等价；参数、端口和结果迁移仍待分批验收。

### 固定验收工程

- 目录：`tests/acceptance/`（数据/工程/预期/README，见 `tests/acceptance/README.md`）。
- 已接入自动化：找圆、两点距离、点云点面距离（`test_acceptance_flows`），数据不依赖用户目录；
  ROI→拟合、控制流、设备模拟三类流程待补。
- 测试数据生成：`python3 tests/acceptance/generate_data.py`（无随机性、可重复）。

## 0.2 清理伪完成功能

- 移除死代码入口（均无实际功能且未接线）：`onUIDesign`、`onLaserSet`、`onCanvasSettings`、
  `onSaveLayout`、`onLoadLayout`、`onLicenseManager`、`onHelp`、`onSchemeManagement`。
- Parallel 标记实验性：从工具箱隐藏、`metadata.json` 增加 `experimental: true`、
  `docs/plugins.md` 标注"实验性，未开放"，禁止加入生产流程（阶段 3 前）。
- 相机 CMake 选项：开启 `ENABLE_CAMERA_BASLER/HIKVISION` 但源码缺失时给出明确
  `FATAL_ERROR`（已实测），而非"目录不存在"。
- 文档修正：`docs/plugins.md`、`README.md` 中 QRCode 条码、并行的夸大描述改为实际能力。

## 测试结果

```text
cmake --build build -j2                       → 成功
ctest --test-dir build --output-on-failure    → 58/58 通过（阶段 A 基线，2026-08-17 复测）
```

- `test_acceptance_flows`：找圆（圆心 320,240 半径 100，误差≤3px）、两点距离（268.33px，误差≤2px）、
  点云点面距离（3.0，误差≤0.001）均有确定性断言。
- `test_agentbridge` 在本机 offscreen 环境已通过（QLocalServer::listen 正常），无需另行复验。
- 控制流（If/Loop/While/Parallel）状态以 phase3 记录为准，阶段 A 不对其作完成性结论。

## GUI 截图

截图存放于 `docs/baseline/screenshots/`：

| 文件 | 内容 |
| --- | --- |
| phase0_mainwindow_desktop_dark.png | 桌面 1440×900 主窗口，尺寸未达到正式门禁 |
| phase0_mainwindow_compact.png | 紧凑 1024×700 主窗口，尺寸未达到正式门禁 |
| phase0_toolbox.png | 当前与紧凑主窗口重复，需重新单独采集 |

截图通过 `build/bin/ui_capture_mainwindow <out-dir>`（offscreen）生成；
验收约定桌面 1920×1080、紧凑 1280×800 的正式截图需在有显示环境补拍。

## 遗留问题

1. 对 110 个旧版插件完成参数、端口和结果级人工核验。
2. ROI→拟合、2D/3D 测量、条件/循环、PLC/相机/AI 模拟流程的验收工程待建。
3. 正式尺寸 GUI 截图和 `test_agentbridge` 正常环境复验待补。

## 回滚方式

- 0.2 清理为删除死代码与文档/元数据标注，回滚：`git revert <本提交>`。
- 0.1 产物（矩阵/验收目录/测试）为纯新增，回滚不影响既有功能。
