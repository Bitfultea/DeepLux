# 阶段 2 验收记录：工程格式 3.0 与自动迁移

> 对应《DeepLux C++ 重构版完整能力建设计划》阶段 2。

## 2.1 新工程结构（3.0）

- `Project` 升级：顶层 `version=3.0`，新增 `flows`（至少 main）、`resources`（相机+数据源）、
  `recipes`、`dashboard`、`migration`。
- `ModuleConnection` 增加稳定字符串端口 `fromPort/toPort` 与 `edgeType`（data/control），
  保留 `fromOutput/toInput` 整数序号用于 2.0 兼容与迁移。
- 新增 `ProjectFlow`、`MigrationRecord` 结构及序列化。
- 读取兼容 2.0：`resources` 缺失时回退顶层 `cameras/dataSources`。

## 2.2 迁移器（ProjectMigrator）

- `migrate()`：2.0 节点全部放入 main 流程；`fromOutput=0/toInput=0` 映射到插件默认输出/输入端口；
  线性图像流程 image->image；下游必需命名输入无法由上游确定时记为 blockers（待修复），不静默猜测；
  写入 `migration` 记录；幂等。
- `saveWithBackup()`：覆盖保存时若原文件为 2.0 生成 `.v2.bak`。
- 迁移报告含 succeeded/autoFixed/warnings/blockers。
- `ProjectManager::openProject()` 在加载旧工程后调用迁移器；`saveProject()` 与 `saveAsProject()` 统一使用
  `saveWithBackup()`，因此 GUI、CLI 和 Agent 共用同一迁移/备份路径。

## 测试（test_projectmigration）

- 线性图像流程迁移：version 3.0、main 流程、image->image、无阻塞。✅
- 幂等：重复迁移 flows/connections/version 一致。✅
- 覆盖保存生成 .v2.bak 且备份为 2.0。✅
- 仅加载不修改原文件。✅
- ProjectManager 端到端：打开 v2 工程后内存格式为 3.0；首次保存生成 `.v2.bak`。✅

## 验证

```text
cmake --build build -j2  → 成功
ctest -R '^test_projectmigration$' → 1/1 通过
```

## 验收标准核对

| 标准 | 状态 |
| --- | --- |
| 历史工程可加载、原文件不被加载修改 | ✅ |
| 迁移后找圆/图像/测量流程结果一致 | ⚠️ 迁移保持节点/连接/参数不变；运行一致性依赖阶段 3 端口路由 |
| 歧义连接不能运行并定位节点端口 | ✅ 迁移期标记 blockers（含节点+端口）；图装载期拒绝 `pending` 连接 |
| 保存/关闭/重开后节点/连接/参数/断点/备注一致 | ⚠️ 节点/连接/参数已覆盖；断点/备注字段待加入 ModuleInstance |

## 遗留

1. `ModuleInstance` 尚未含备注(note)/启用(enabled)/断点(breakpoint) 字段（3.0 要求），待补。
2. 运行期"歧义连接阻断"与真实端口路由属阶段 3，待实现。

## 回滚

- 本阶段为模型与迁移器新增，回滚 `git revert` 即可；2.0 工程仍可读取。
