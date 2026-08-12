# DeepLux Engineering Skills

这组技能是从 DeepLux 的实际架构、界面迭代、插件交付、测量交互、Agent 与 SAM 标注实践中提炼出的可复用工作方式。每张图按该技能的真实模块、数据流、状态、风险边界与验收证据展开，不使用固定步骤模板。

| 技能 | 解决的问题 | 示意图 |
| --- | --- | --- |
| Qt 界面审计 | 主题、间距、裁切、交互状态不一致 | [PNG](01-qt-ui-audit.png) / [SVG](01-qt-ui-audit.svg) |
| 插件交付 | 动态插件的元数据、实例化、配置与部署契约 | [PNG](02-plugin-delivery.png) / [SVG](02-plugin-delivery.svg) |
| 2D/3D 几何测量 | 取点、几何计算、叠加结果与单位语义 | [PNG](03-measurement-geometry.png) / [SVG](03-measurement-geometry.svg) |
| 流程执行与检视 | 单次、单步、循环、停止和中间结果 | [PNG](04-flow-observability.png) / [SVG](04-flow-observability.svg) |
| Agent 受控协作 | 对话、工具调用、确认、审计和撤销 | [PNG](05-agent-governance.png) / [SVG](05-agent-governance.svg) |
| SAM 快速标注 | 主视图提示、模型推理、预览和导出闭环 | [PNG](06-sam-annotation.png) / [SVG](06-sam-annotation.svg) |
| 根因定位与验证 | 数据流追踪、最小修复、测试与二进制核对 | [PNG](07-root-cause-qa.png) / [SVG](07-root-cause-qa.svg) |

其中 `deeplux-qt-ui-audit` 和 `root-cause-first` 已具备可执行工作流；其余图示定义了适合继续固化为 DeepLux 专项 skill 的边界与验收标准。

## 使用统计

基于可追溯的任务级显式启用记录生成的使用图表与统计口径见：[专项 Skill 使用统计](skill-usage-report.md)（[PNG](08-skill-usage.png) / [SVG](08-skill-usage.svg)）。
