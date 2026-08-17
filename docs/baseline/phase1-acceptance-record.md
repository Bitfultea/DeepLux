# 阶段 1 验收记录：强类型数据契约与插件 ABI v2

> 对应《DeepLux C++ 重构版完整能力建设计划》阶段 1。

## 1.1 公共数据类型

- 新增 `src/core/deeplux/DataContract.h/.cpp`：
  - `enum class DataType`（Image2D/HeightMap2D/PointCloud3D/Mask2D/Region2D/Point2D/Point3D/PointSet2D/Line2D/
    Circle2D/Ellipse2D/Plane3D/Transform2D/DetectionList/ClassScores/Number/Integer/Boolean/String/Binary/Table/Any）。
  - `struct PortSpec { id, displayName, type, required, multiple }`。
  - `using PortValueMap = QHash<QString, QVariant>`。
  - `struct ExecutionContext { runId, frameId, timestampMs, runMode, cancellationToken }`。
  - `struct ExecutionResult { success, errorCode, userMessage, diagnostics }` + `ExecError` 结构化错误码。
  - `registerDataContractMetaTypes()` 注册元类型；`dataTypeName()/dataTypeFromString()`。
- ImageData 保留为 Image2D 载荷；新 execute 以 `image` 端口承载 ImageData（保留全部数据键）。

## 1.2 插件接口 ABI v2

- `DEEPLUX_MODULE_INTERFACE_VERSION` 1→2；`Q_DECLARE_INTERFACE` 升为 `com.deeplux.IModule/2.0`。
- `IModule` 新增 `ExecutionResult execute(const PortValueMap&, PortValueMap&, ExecutionContext&)` 与
  `inputPorts()/outputPorts()`。
- `ModuleBase` 实现新 execute：提取 image 载体→从载体数据键补充命名输入→必需输入和类型校验
  （分别返回 `ExecError::MissingRequiredInput` / `ExecError::TypeMismatch`）→桥接旧 execute→导出声明输出端口。
- `RunEngine` 改用新 execute，携带 ExecutionContext（runId 每次运行生成、frameId 递增）。
- `PluginManager`：加载时解析 metadata `ports.inputs/outputs` 并校验（ID 唯一/类型有效/显示名非空）；
  对模块插件做 ABI 校验——cast 失败或 `interfaceVersion()!=2` 明确拒绝并提示重编（ABI v1 不崩溃）。
- 全部 55 个非相机插件重编为 ABI v2 并通过加载。

## 1.2b 端口元数据

- `tests/acceptance/generate_ports.py` 为 56 个插件生成 `ports` 声明（image 载体 + 几何/测量命名端口）。
- 原隐藏键 `point1/fit_points/line/plane` 等已显式声明为端口（不再"未声明"）。

## 1.3 契约测试（test_pluginparametercontracts 新增）

- `testAllPluginsAbiV2`：全部插件 interfaceVersion()==2。
- `testPortsWellFormed`：端口 ID 唯一、显示名非空、非源模块声明 image 输出。
- `testMissingRequiredInputReturnsStructuredError`：FitLine 空输入返回 MissingRequiredInput。
- `testWrongPortTypeReturnsStructuredError`：FitLine 接收字符串时返回 TypeMismatch。
- `testDeclaredOutputsAreExported`：DistancePP 的 `distance` 输出可由命名端口读取。

## 验证结果

```text
cmake --build build -j2               → 成功
ctest -R '^(test_pluginparametercontracts|test_acceptance_flows|test_projectmigration)$' → 3/3 通过
cmake --build build --target sync-plugins → prepared=2, copied=55, skipped=2
```

## 验收标准核对

| 标准 | 状态 |
| --- | --- |
| 内置模块插件全部通过 ABI v2 加载 | ✅（55 非相机模块；相机走 ICamera 路径） |
| ABI v1 插件不崩溃并显示明确诊断 | ✅（interfaceVersion/cast 校验拒绝） |
| 任意不匹配连接运行前识别 | ✅ 数据边在图装载期校验模块、端口、类型和多输入约束；显式控制边仍待控制调度实现 |
| 不再依赖未声明隐藏键 | ✅（point1/fit_points 等已声明为端口） |

## 遗留

1. 几何命名端口当前仍经 ImageData 数据键承载（桥接补充），未完全改为独立 PortValue 传递。
2. Mask、Region、检测列表等复杂载荷仍采用过渡性宽松类型检查，需随生产插件定义专用载荷类型。

## 回滚

- 本阶段为接口与元数据升级，回滚需整体 `git revert`（插件与核心需同版本重编）。
