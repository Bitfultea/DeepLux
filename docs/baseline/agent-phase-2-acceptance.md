# 阶段 2 验收记录：完整参数契约和可发现性

依据 `docs/agent-capability-improvement-plan.md` §7。阶段 1 G1 已由独立审核 Agent 于 2026-09-30 判 PASS（见 `agent-phase-1-acceptance.md` §12）。**状态：G2 PASS（2026-09-30，第 2 轮独立复审）；仅放行阶段 3。**本文件的接口契约在阶段 2 编码前冻结。

## 1. 接口契约（编码前冻结）

### 1.1 原子批量配置

`set_module_params({"instanceId":"m","values":{"min":10,"max":20}})`：`values` 是 JSON object，其值原样保持 JSON 类型。先合并插件默认值、当前实例覆盖值及本次变更，在最终候选上调用插件校验一次；成功后一次提交、一条撤销记录。任一字段非法、候选整体非法或模块不存在时，覆盖值、工程修改标记和撤销栈均不变。成功保留 `status`、`instanceId`，返回变更键及最终有效值；空对象明确 `no_op`。

`set_param({"instanceId":"m","key":"caption","value":""})`：兼容旧入口，内部走同一原子实现。仅旧入口对字符串到已知 number/bool 默认类型保留既有转换；明确类型值不猜测转换。字段缺失返回 `MISSING_PARAM`；显式 `null` 不删除覆盖值，只有插件/参数声明可空时允许，否则返回 `INVALID_PARAM`。合法空字符串按插件契约原样保存。

`reset_module_params({"instanceId":"m","keys":["min","max"]})`：原子移除指定覆盖键，恢复插件默认值；先验证最终候选，一条撤销记录。未知键、空列表、非字符串或重复键有明确失败/幂等语义，不把 `null` 当重置。

### 1.2 发现和描述

`get_module_params_schema` 支持 `plugin`（创建前）或 `instanceId`（创建后）两种入口，二者同时提供时返回 `INVALID_PARAM`。返回稳定 `pluginId`、版本、加载/适用状态、参数逐项类型/默认值/当前覆盖值/最终值、枚举/范围/单位/可编辑性/数组项结构/跨字段约束，以及与插件 metadata 一致的输入/输出端口（类型、required、multiple、control）和结果说明。缓存只含插件静态信息；实例覆盖值每次实时读取；加载/卸载或 metadata 变化须失效。

### 1.3 测试与审核边界

请求须从模型实际收到的 JSON Schema 生成，经 `AgentController` 解析再进入 `AgentActor`；不得只对 Actor 直接造参数证明类型链路。`MeasurementInput`、`CropImage`、`StrFormat`、`PLCWrite`、`SamPredict` 复杂值，以及 N 点标定新旧格式、有效点对和退化拒绝，均按计划 §7/G2 逐项留业务值、撤销/重做、保存重开证据。模拟输入不能替代真实 SAM/PLC 运行，后者仍受阶段 5/8 门槛约束。

## 2. G2 检查清单（第 2 轮独立审核通过）

- [x] Schema → Controller → Actor 模拟工具调用保持数组、嵌套对象、数值、布尔类型；未声称真实 LLM 生成成功。
- [x] 空字符串、缺 value、显式 null、恢复默认的语义和错误码有测试。
- [x] 五类复杂配置经 Agent 写入、读回、撤销、重做、保存重开一致；测量输入五种模式另有逐项读回。
- [x] FitCircle 半径上下界、PLC 地址+寄存器值、PLC 来源+值三组跨字段原子更新；无效最终状态零变更、零 Undo。
- [x] N 点标定新结构、旧格式迁移、有效数值输出，以及共线/重复点拒绝；损坏旧字段不吞数据。
- [x] 非法键、范围、类型、非有限数和过大数组拒绝。
- [x] 参数/端口/结果描述与 metadata 一致，动态值/资源缺失状态有测试。

审核结论：第 2 轮独立复审 G2 PASS，限定参数契约与可发现性；勾选对应的原始证据见下文。

## 3. 实现及兼容性（2026-09-30）

`ToolSchema`/`AgentActor` 增加 `set_module_params`、`reset_module_params`、实例前后参数描述；旧 `set_param` 共用原子写入，保留旧数字/布尔字符串转换。Schema 静态部分在插件扫描或加载变化时失效，实例覆盖值和文件/相机可用性实时获取。`AgentController` 的真实 Undo 审计识别新增操作。相关插件补上数组元素、跨字段及边界校验；PLC 端口/slaveId/timeout 元数据与实现对齐，SamPredict `box` 明示数组或 null。N 点标定使用持久化 `calibrationPoints`；旧编号坐标只在完整、有限时迁移，坏工程加载拒绝且 `ModuleInstance::fromJson` 保留旧字段。

本阶段主要落点：`src/core/agent/{ToolSchema.cpp,AgentActor.cpp,AgentActor.h,AgentController.cpp}`、`src/core/model/Project.cpp`，六类插件的实现/metadata，以及 `tests/{test_agentactor.cpp,test_agentphase0_repro.cpp,test_pluginparametercontracts.cpp,test_toolschema.cpp}`。未修改同级 `DeepLux` 工作区，未提交/推送。阶段 1 与更早的脏树原样保留，当前 `git diff` 含前阶段修改；起止 HEAD 均为 `b516f54da28b63623ef6b293b71a4afa3b84fad0`，不能把整份 diff 冒充纯阶段 2 差异。

## 4. 命令、原始证据与摘要

- 构建：`cmake --build build-codex-agent -j2`，退出码 0；构建目录 `CMAKE_HOME_DIRECTORY=/home/charles/Data/Repo/DeepLux_codex`，没有复用同级工作区缓存。
- 全量：`QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure --output-junit /home/charles/Data/Repo/DeepLux_codex/build-codex-agent/agent-phase2-evidence/ctest-full.xml -j2`，78/78，0 失败、0 跳过，退出码 0；本机回环监听测试在获准环境执行。Qt 测试运行时为 5.15.3。
- 复现：`QT_QPA_PLATFORM=offscreen ./build-codex-agent/bin/test_agentphase0_repro -o build-codex-agent/agent-phase2-evidence/repro-test-run.log,txt`，20/20，0 失败、0 跳过，退出码 0。
- 格式和引用：`git diff --check` 退出码 0；`python3 scripts/audit_agent_capability_baseline.py generate --runtime-log build-codex-agent/agent-phase0-evidence/runtime-list-plugins.log` 退出码 0，483/483 矩阵格引用有效。
- 原始日志、JUnit 与清单在 `build-codex-agent/agent-phase2-evidence/`。`sha256sums.txt` 覆盖 28 项源码/矩阵/日志，`binary-manifest.txt` 覆盖 72 项应用/插件/测试二进制；两份 `sha256sum -c` 均逐项 OK。摘要 SHA-256：以两份清单中的值为准。该目录被构建目录忽略，不提交。

补测过程中，新增 `testG2MeasurementModesThroughAgent` 首次定向运行因测试夹具漏调 `registerDefaultTools()` 报 `Unknown tool`；补齐初始化后定向 2/2 和最终全量 78/78 通过。前一次失败不是产品路径通过证据，亦未被当作回归失败隐去。

## 5. 业务输入与断言

- `testG2TypedArrayTraversesController`：从 mock LLM 实际收到的 `set_module_params`/`set_param` JSON Schema 构造三次工具调用，经 Controller 解析；点数组 `[10,20]`/`[30,40]`、`grabTimeout=2300`、`folderLoop=true` 和嵌套 `calibrationPoints` 在工程中保留原 JSON 类型。
- `testG2ComplexConfigRoundTrip`：MeasurementInput 点集、CropImage 双矩形、StrFormat 字符串数组、PLCWrite 两寄存器 `[0,65535]`、SamPredict 正负点与 box 五类经 Agent 写入/读回、Undo/Redo、保存重开再读回完全一致；错误元素均使工程参数和 Undo 计数不变。`testG2MeasurementModesThroughAgent` 再覆盖点对、点线、双线、点面、点集模式。
- `testCrossFieldRadiusUpdateIsAtomic`：FitCircle 从默认最大半径 1000 原子写入 `minRadius=1200,maxRadius=1500` 成功；随后 `minRadius=1600,maxRadius=1500` 拒绝且零改动。`testG2PlcRegisterValuesValidateWithoutConnecting`：地址 65534 + 两寄存器成功、地址 65535 + 两寄存器拒绝；`valueSource=variable,values="register_buffer"` 成功，改为 immediate 但保留字符串拒绝，均断言零部分提交。全程不连接 PLC。
- `testF04CalibrationPointPairsRunAndMigrate`：四对点表示平移 `(5,7)`，运行得到 `calibration_valid=true`、`point_count=4`、重投影误差 `<0.001`；共线/重复点拒绝且零 Undo。完整旧坐标迁移为新结构；缺失 `worldY_3` 的旧工程拒绝加载，单个实例解析仍保留全部原始旧字段。
- `testSchemaBeforeInstanceIncludesPluginIdentityAndPorts` 与合同测试核对插件 ID、端口/结果和实例实时覆盖值；文件资源从 unconfigured → missing → available，空相机列表标 unavailable。

## 6. 矩阵、限制与审核

矩阵 69 插件 × 7 维、109 GUI 操作。配置维度仅 6 项 PASS（上述五类复杂插件加 NPointCalibration），其余 63 项 NOT_TESTED；未以共享 API 替代逐插件证据。真实 LLM、PLC 发报、SAM 模型、相机设备、其余插件业务运行仍未验证，分别保留在后续阶段/现场门槛。旧工程损坏时现在拒绝加载，这是防止隐式丢数据的兼容性变化，需 GUI 告知用户并保留源文件。

独立审核结论：**第 1 轮 FAIL；第 2 轮 G2 PASS**。首次请求的审核 Agent 因服务用量限制未产出判定；随后独立审核 Agent `/root/phase2_independent_audit` 于 2026-09-30 判 FAIL，发现下述两项本地可整改问题，并在整改后第 2 轮只读复审中判 G2 PASS。放行范围仅为进入阶段 3；不代表 G3、其余 63 插件配置、真实 LLM、PLC 发报、SAM 模型或相机设备通过。回退仅针对本阶段上述代码和测试，保留阶段 0/1、既有用户修改与原始证据，不执行整树 reset。

## 7. 第 1 轮独立审核整改与第 2 轮结论

| 发现 | 修正及回归断言 |
| --- | --- |
| P1：CropImage、StrFormat、PLCWrite、SamPredict、NPoint 的 `results` 为空；测试只比较两个空对象 | 五份 metadata 按插件实际 `output.setData` 补入结果键、类型和标签。StrFormat 以 `keyFromParameter=outputVariable` 描述动态键，`get_module_params_schema` 按实例有效参数解析；测试断言五插件具体键及非空标签，并检查自定义输出变量在写入、Undo 和重开后的结果键切换。 |
| P2：NPoint 宽高 Schema 暗示小数且无上界，与实现的整数 `[0,16384]` 不符 | metadata 改为 `max=16384, decimals=0`；F04 回归通过 Agent Schema 断言宽高上下界、精度及三个结果键。自查同时更正重投影误差的单位为“世界坐标单位”（与 `imageToWorld` 数值计算一致），覆盖 metadata、GUI 文案和日志。 |

整改后全量构建退出码 0、CTest 78/78 退出码 0、复现 20/20 退出码 0；矩阵 483/483 引用有效。两份 SHA 清单已重生成，仍为 28 项源码/证据、72 项二进制（逐项校验结果见证据目录）。独立审核 Agent 第 2 轮复核源码、测试、JUnit、SHA 与 `git diff --check` 均通过，未发现新的 G2 阻断项，结论为 **G2 PASS**。真实设备/模型未验证范围不变；允许进入阶段 3，但阶段 3 须独立验收。
