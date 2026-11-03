# 阶段 8 外部验证门槛清单

## 状态

**总体状态：NOT_RUN。** 本清单用于阶段 5 G5 标准 7 的证据分流，并作为阶段 8 执行记录模板。fake peer、临时文件、fake SAM 服务、模拟相机与本地 LLM fixture 只证明软件接口和协议，不计作真实设备、模型或 provider 现场验收。目标平台、设备、模型权重和 provider 凭据均需由维护者在目标环境提供；本文不包含真实凭据或生产网络地址。

## 阶段 8 必须收集的证据

| 门槛 | 必须记录并通过的内容 | 交付证据 | 状态 |
| --- | --- | --- | --- |
| 自动化插件矩阵 | 从源码清单自动对账模块总数、目标平台适用数、构建数、加载数、配置通过数、业务执行通过数和现场验收数；每个适用插件至少有一个有效业务 fixture、一个关键失败 fixture；多模式插件覆盖所有公开模式。 | `docs/baseline/agent-g8-current-acceptance-matrix.json/.md`、生成器与校验测试、构建/CTest日志、逐插件结果与差异清单。 | PARTIAL：正式当前矩阵已生成；66 插件 fixture PASS、DefectDetection FAIL、DirectShow/Hikvision BLOCKED；Slice53 后 126/126 操作有证据链接、0/126 未链接；现场 0/69 |
| Linux Qt 5 / Qt 6 | 两个受支持 Qt 主版本分别全量构建和 CTest；保存版本、CMake 配置、CTest 原始日志、跳过项和失败归因。 | 每套环境的配置缓存、构建日志、全量 CTest、源/二进制摘要。 | PARTIAL：Qt 5.15.3 PASS；Qt 6 NOT_TESTED（本机未安装） |
| Windows 平台 | 按既有 CI 完成产品构建；仅对声明支持的 Windows 相机驱动、Qt UI 与相关插件环境执行适用回归；未提供驱动/SDK 的项保留未验证，不按 Linux 结果推断。 | CI 构建链接/日志、Windows 驱动/SDK 版本、适用测试及未测原因。 | NOT_RUN |
| 真实相机/SDK | 对每个声明支持的型号记录设备型号、厂商驱动/SDK/固件、接口类型；验证发现、连接、能力查询、参数设置及读回、实际帧、停止、断开、拔插/重连与错误恢复。任何状态均以设备 API/实测读回为准。 | 设备/SDK 清单、脱敏操作日志、参数读回、帧摘要/校验和、断连和恢复记录、责任人/日期。 | NOT_RUN |
| 现场 PLC、串口与 TCP | 仅在授权测试设备/隔离网络中记录设备型号、固件、非凭据目标标识、协议与寄存器/载荷；确认前不发送；批准后核对请求计数、响应/读回值、超时分类、取消与不重放。现场值班人确认测试窗口和恢复方式。 | 设备与固件、脱敏配置、抓包/PTY 或设备日志、发送计数、目标读回、故障与恢复记录。 | NOT_RUN |
| 真实 SAM 模型 | 记录模型/权重来源与 SHA-256、服务版本、CPU/GPU 与驱动；验证加载、编码缓存、预测、重复请求、标签/坐标/掩膜输出、错误、超时和取消。Fake 服务不计入模型质量或推理正确性。 | 模型清单/摘要、脱敏请求与结果、固定测试集和预期、资源/时延数据、失败样本。 | NOT_RUN |
| 每个声明支持的真实 LLM provider | 对固定自然语言任务至少 10 项，每项独立重复 3 次；覆盖问答、建流程、数组参数、结果读取、改错、拒绝、取消、缺少设备前置条件、保存/重开与标注。记录模型标识、provider 配置、完整脱敏 tool calls/结果/最终回复，不按观察结果改写提示。 | 固定任务文本/断言、脱敏逐轮记录、失败案例、provider/model/version、原始成功率计算。 | NOT_RUN |
| 真实 LLM 质量门槛 | 对每个声明支持的 provider 配置分别计算固定任务完成率，至少 90%；每个配置的数据丢失、越权副作用、错误结果被断言成功均为 0。不得跨 provider 汇总抵消失败配置；任何不达标项保留失败记录并返回所属阶段修复，不通过选择性删除样本达标。 | 每个 provider 的完成率分母/分子、三类严重错误清单及 0 计数证据、缺陷回归链接。 | NOT_RUN |
| 时延与现场操作边界 | 记录样本数、冷/热启动、请求/运行/设备阶段分段、P50/P95 和超时；说明硬件、网络、模型、并发与缓存状态。运行中数据不得用单次最佳值代替分布。 | 可复算原始样本、采集脚本/配置、统计方法与结果。 | NOT_RUN |
| 发布与交接 | 更新与已验收版本一致的软件操作手册、目标平台/SDK/插件要求、限制、已知风险和恢复说明；报告区分模拟、本地服务、真实模型、现场设备和 provider 证据。 | 手册/发布说明版本、最终清单、已知限制、维护者签核。 | NOT_RUN |

## 执行记录字段

每个门槛完成时新增一条不可覆盖的执行记录，包含：`gateId`、日期/时区、负责人、硬件/软件/模型/provider 版本、配置摘要、原始证据相对路径、SHA-256、命令与退出码、样本数量、PASS/FAIL/NOT_TESTED、失败说明、复测链接和复核人。测试前确认设备授权、测试窗口、停止/复位方式；日志和截图必须移除密码、令牌、个人信息与生产网络敏感字段。

## 本地证据边界

本机 fake Modbus、TCP loopback、POSIX PTY、fake camera、临时 PNG、fake SAM HTTP 服务和 CTest 只能支持对应软件协议/状态机结论。它们不证明 PLC 已提交业务写入、串口转换器可靠、真实 TCP 对端完成业务事务、相机画面质量、厂商 SDK 线程安全、SAM 模型精度或真实 LLM 任务成功率。所有上述外部结论保持 `NOT_TESTED`，直到本清单挂接可复核的原始现场证据。

## 阶段 8 本地执行记录

### G8-OPERATION-EVIDENCE-LINK-SLICE51-2026-11-03

- 范围：为 `CAM_DELETE_CONFIG` 与 `CAM_APPLY_PARAMS` 挂接既有 QtTest `testCameraSetViewDeleteAndApplyRemainGuiOnly`，修复矩阵缺少可解析测试引用的问题；没有改动测试或产品代码。
- 语义边界：测试证明删除按钮只移除表格行、应用按钮只记录填写的 IP/端口并关闭窗口；它断言 CameraManager 状态不变，且没有相机参数写入、连接或采集调用。两项操作仍为 `UNSUPPORTED`，测试只链接现有 GUI 行为，不声称 Agent、持久化或设备能力。
- 验证：使用 Slice50 的全量 CTest 113/113 日志重生成当前矩阵；生成器 `--check`、候选库存 `--check`、两套 auditor tests 20/20、JSON 校验、`git diff --check` 通过。Slice51 摘要、结果与 14 项 SHA 清单位于 `build-codex-agent/agent-phase8-slice51-evidence/`，`sha256sum -c` 14/14 通过。矩阵为 69 插件、126 操作、125 `EVIDENCE_LINKED`/1 `NOT_LINKED`、现场 0/69、overall `NOT_READY`。
- 独立审核：`/root/slice176_final_review` **PASS**，确认测试引用、notes、操作映射及总状态一致。
- 剩余未链接项：人类专用 `TERMINAL_INPUT`；不启动用户环境 shell 来制造证据。

### G8-ENVIRONMENT-PREFLIGHT-2026-11-03

- 当前 checkout `DeepLux_codex`、分支 `agent/development`；可见 `/dev/video*`、`/dev/ttyUSB*`、`/dev/ttyACM*`、`/dev/nvidia*` 均为空，未发现 Qt 6 (`qmake6`/`qmake-qt6`) 或 Windows PowerShell runner (`pwsh`)。原始检查结果见 `build-codex-agent/agent-phase8-slice51-evidence/environment-preflight.txt`。
- 判定：本轮不能执行真实相机、串口/PLC、GPU/SAM 权重、Qt 6 或 Windows 验收；这些仍是 `NOT_TESTED`。未检查 provider 凭据或外部账户，真实 LLM/provider 验收仍为 `NOT_RUN`。已通过的 Linux/Qt 5.15.3 软件 CTest 不提升这些门槛状态。

### G8-LINUX-QT5-2026-10-30

- 状态：PASS（仅 Linux / Qt 5.15.3 这一环境）。
- 日期/时间：2026-10-30 18:09（Asia/Shanghai）。
- 执行者：Codex；复核：G7 Slice 5/6 独立只读复审 PASS。
- 环境：Ubuntu 22.04；源码 HEAD `b516f54` 加当前工作树；Qt 5.15.3；插件/产品 SDK 和现场硬件不纳入此子门。
- 构建：`cmake --build build-codex-agent -j2`，退出码 0。
- 测试：`QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure`，退出码 0，109/109 通过、0 失败、0 跳过；本机 loopback fixture 在授权本机环境中运行。
- 工具链：Qt 5 Core `pkg-config --modversion` 输出 `5.15.3`；Qt 6 Core 未发现，本机没有 Qt 6 可用套件，故 Qt 6 子项未测试。
- 原始证据：`build-codex-agent/agent-phase7-slice5-evidence/build-final.log`、`ctest-full.log`、`sha256sums.txt`、`binary-manifest.txt`。
- 复核：G7 Slice 5/6 独立复审 PASS；本记录只把对应 Qt 5 构建/CTest 结果用于 G8 Linux Qt 5 子项，不推断插件现场验收或外部设备/provider 状态。

### G8-PLUGIN-COVERAGE-PRE-AUDIT-2026-10-30

- 状态：预审记录；G8 自动化插件矩阵仍为 `NOT_RUN`。
- 工具：`scripts/audit_agent_g8_plugin_coverage.py` 从当前 69 项源码 metadata 与阶段 0 基线做目录名和插件 ID 身份集合对账；候选测试来源只扫描 `tests/CMakeLists.txt` 已注册源码，并把候选 suite 与完整 CTest 日志关联。清单同时列出候选文件定义/声明的 QtTest 用例名，便于人工定位；用例名仍只属于文件级索引，不代表该用例已映射到对应插件。
- 结果：源码清单 69（66 模块、3 相机）；当前构建目录有 66 个 metadata 对应插件二进制；65 项找到测试源码候选、4 项无候选（DirectShowCamera、HikvisionCamera、QRCode、ShowImage）；所有被引用 suite 都在 110/110 全量 CTest 中通过。候选共有 262 个插件/测试文件关联项、952 个 suite/QtTest 用例名索引。
- 判定限制：逐插件夹具审核当前仅覆盖 2/69；测试源码提及及其他候选用例名不等于覆盖了 Agent 参数配置/输入/执行/输出。该清单用于找缺口和防止目录变化静默漏项，不是 G8 PASS 证据。
- 独立复核：`/root/slice176_final_review` 只读 PASS；确认 110 项日志、用例索引和两个变量插件 Agent 闭环测试/失败断言；两项工具/清理 finding 已修复并复审关闭。
- 验证：`python3 -m unittest scripts/test_audit_agent_g8_plugin_coverage.py -v` 11/11；测试包含“失败/重复/非连续编号日志”拒绝、metadata 身份集合核对、中心测试列表和显式 `add_executable(test_*.cpp)` 源码登记、QtTest 用例名提取、默认 CTest 日志与清单一致性、两项显式 fixture audit 记录解析；脚本无参数 `--check` 与显式 110 项日志 `--check` 均通过；Python 编译通过。预审/交叉清单：`docs/baseline/agent-g8-plugin-coverage-inventory.{json,md}`；人工审核数据源：`docs/baseline/agent-g8-plugin-fixture-audits.json`；摘要：`build-codex-agent/agent-phase8-local-evidence/plugin-coverage-preaudit.md`。

### G8-LOCAL-VARIABLE-PLUGINS-2026-10-30

- 范围：`VarDefinePlugin` 与 `VarSetPlugin` 两个本机 Qt 5 插件；只证明 Agent 软件链路，不是现场设备/provider 验收。
- E2E：`test_agent_variable_plugins_g8` 在隔离临时插件根目录加载真实构建二进制；Agent 分别以稳定 ID 和 metadata 注册名添加插件，通过 `set_module_params` 配置，`run_flow` 执行，并以 `get_global_variable` 读到 7.25；另验证 VarDefine 空变量名和 VarSet 不存在变量均以 failed 终态返回对应失败节点及诊断。
- 结果：定向 CTest 1/1 PASS；全量构建命令退出码 0；授权本机 loopback 环境的全量 CTest 110/110 通过、0 失败、0 跳过。原始日志与摘要位于 `build-codex-agent/agent-phase8-variable-plugin-evidence/`。
- 审核状态：`/root/slice176_final_review` 独立只读 PASS；审核数据写入 `docs/baseline/agent-g8-plugin-fixture-audits.json` 并由清单生成器校验引用。正式自动化插件矩阵仍 `NOT_RUN`，因为其余适用插件、平台与现场门槛未完成。

### G8-LOCAL-VISUAL-PLUGINS-2026-10-30

- 范围：`GrabImage`、`QRCode`、`ShowImage` 的 Linux/Qt 5 Agent 软件链路；不代表真实相机、跨平台或现场验收。
- E2E：`test_agent_visual_plugins_g8` 在独立临时插件目录加载真实构建库；Agent 配置文件采集和两个图像插件，通过两条 `image` 端口连线运行。二维码结果精确匹配 `DEEPLUX-G8-QR-2026`，ShowImage 同一 runId 的 `image_info` 包含 `296x296`；空白图失败归因到 QRCode，禁用上游后空输入失败归因到 ShowImage，均有非空诊断断言。
- 结果：全量 CTest 111/111、0 失败、0 跳过；格式化后定向 CTest 1/1；全量构建退出码 0；Python 库存测试 11/11；生成器 `--check`、Python 编译、clang-format、二进制 SHA-256 和 `git diff --check` 均通过。证据位于 `build-codex-agent/agent-phase8-visual-plugin-evidence/`。
- 独立审核：`/root/slice176_final_review` 只读 PASS，无阻断 finding。两个插件的业务/关键失败 fixture 已记录到 `docs/baseline/agent-g8-plugin-fixture-audits.json` 并由库存生成器校验。
- 库存：69 项插件中有 67 项测试源码候选、2 项无候选；当前 4/69 项有已审核业务和关键失败 fixture；现场验收为 0。G8 自动化插件矩阵仍 `NOT_RUN`，本记录不构成 G8 PASS。

### G8-LOCAL-PLCREAD-2026-10-30

- 范围：`PLCReadPlugin` 的 Linux/Qt 5 Agent 配置和执行闭环；使用隔离临时插件根及受控 loopback Modbus peer，不代表真实 PLC 现场验收。
- E2E：`test_agent_core_plugins_g8::testAgentConfigurationReadsModbusAndReportsProtocolFailure` 经 Agent 加载插件、创建模块、设置参数、运行并查询结果。精确断言请求从站 7、FC03、地址 `0x1234`、数量 2，并读回 `[0x1234, 0xABCD]`。失败轮由同一 peer 返回 FC `0x83`、异常码 2，断言 run failed、失败实例及 `解析PLC响应失败` 诊断。
- 结果：定向 CTest 连续 5 次 PASS；全量构建退出码 0；最终全量 CTest 112/112、0 失败、0 跳过；G8 库存工具测试 12/12、`--check`、格式、Python 编译、哈希和 diff 检查通过。
- 独立审核：`/root/slice176_final_review` PASS；初次固定 TCP 1 失败夹具被审核指出可能受监听服务影响，已改为受控 Modbus exception frame 并复审关闭。
- 库存：经独立审核的完整 Agent 配置/业务和关键失败 fixture 为 5/69；现场验收为 0。PLC/TCP/串口既有 G5 测试仍只计执行层候选，因为其建图/参数由测试直接写入 Project；G8 自动化插件矩阵仍 `NOT_RUN`。

### G8-LOCAL-MATH-2026-10-30

- 范围：`MathPlugin` 的 Linux/Qt 5 Agent 参数配置、运行和结果查询；不宣称覆盖其它数学模式或现场环境。
- E2E：`testAgentConfigurationCalculatesAndRejectsDivisionByZero` 通过 Agent 加载插件、添加模块、设置 `Add(12.5, 2.5)`，并从同一 run 读回精确结果 `15.0`；随后将同一实例设为 `Divide(12.5, 0)`，运行失败归因到该实例，错误文本精确等于 `除数不能为0`。
- 独立审核：`/root/slice176_final_review` PASS；曾建议将子串诊断检查收紧为精确匹配，现已收紧并复审确认。
- 验证：PLCRead/Math G8 suite 连续 3 次定向 CTest PASS；最终全量构建退出码 0，CTest 112/112、0 失败、0 跳过；库存工具测试 12/12、默认生成器 `--check`、Python 编译、clang-format、二进制及源码/证据哈希检查均通过。
- 库存：本条通过后已审核 G8 Agent 业务/关键失败 fixture 共 6/69，现场验收仍为 0；G8 自动化插件矩阵保持 `NOT_RUN`。

### G8-LOCAL-STRING-AND-TIME-PLUGINS-2026-10-30

- 范围：`CreateString`、`SplitString`、`StrFormatPlugin` 和 `SystemTime` 的 Linux/Qt 5 Agent 参数配置、业务结果及失败返回。
- E2E：共享 suite `test_agent_core_plugins_g8` 经 Agent 加载真实插件、添加模块、设置参数、运行并查询结果。CreateString 精确读到 `Lot-42-检测`、长度 9；空输出名被 `VALIDATION_FAILED` 拒绝。SplitString 精确读到 `A-17|B-29|C-41` 与 `B-29`；空输入失败归因到模块实例并返回精确诊断。StrFormat 精确输出 `lot=Lot-42|qty=3|ratio=0.5`；空输出名精确返回参数校验失败。SystemTime 按 `yyyyMMdd HHmmss` 输出当前格式字符串，时间戳落在运行前后采样范围；空格式被拒绝。
- 独立审核：`/root/slice176_final_review` PASS；六个共享插件使用临时插件根，测试间先清空 RunEngine 模块、关闭工程并清理全局变量，suite teardown 有界等待后再清理。
- 结果：完整 112 项 CTest、全量构建、库存校验、格式、哈希和差异检查在本代码版本完成；当前 G8 已独立审核 fixture 10/69，现场验收 0，自动化插件矩阵 `NOT_RUN`。

### G8-LOCAL-WRITETEXT-2026-10-30

- 范围：`WriteText` 的 Linux/Qt 5 Agent 参数配置、文件副作用确认、内容落盘和打开失败；所有目标均位于 suite 自有临时目录，不触碰工作区用户文件。
- E2E：Agent 加载真实构建插件、添加模块并设置目标路径/文本/覆盖模式。拒绝 `run_flow` 确认后目标不存在；新批次批准后读回精确文本及插件追加的末尾换行。随后将目标设为不存在父目录下的文件，批准执行后断言失败归因到 WriteText 实例，诊断包含目标路径，且目录和文件都未创建。
- 独立审核：`/root/slice176_final_review` 只读 PASS，检查了 Agent 配置、拒绝零副作用、批准内容、失败路径归因、隔离插件根和清理顺序。
- 验证：定向 WriteText QtTest 通过；本次全量构建退出码 0、CTest 112/112、0 失败、0 跳过；原始日志位于 `build-codex-agent/agent-phase8-visual-plugin-evidence/`。
- 当前 G8 已审核 fixture 增至 11/69；现场验收仍为 0，自动化插件矩阵仍为 `NOT_RUN`。

### G8-LOCAL-TIMESLICE-2026-10-30

- 范围：`TimeSlice` 的 Linux/Qt 5 Agent 参数、连线、计时输出和缺少上游输入失败；无真实设备或外部服务。
- E2E：Agent 加载真实插件，添加 Start/Stop 实例、配置模式和切片名，并连接 image 端口。一次 `run_flow` 完成后，从对应模块/同一 `runId` 读到正的 `timeslice_start_time` 和非负 `timeslice_elapsed_ms`。清理工程后仅运行 Stop，断言运行失败，失败实例为 Stop，诊断精确指出缺少 `timeslice_start_time`。
- 独立审核：`/root/slice176_final_review` 只读 PASS；确认单调时钟字段经显式数据边传播，Stop-only 的失败归因和隔离清理正确。
- 验证：TimeSlice 定向 fixture PASS；`test_agent_core_plugins_g8` 经 CTest `--repeat until-fail:3` 三次通过；全量构建退出码 0，本轮全量 CTest 112/112、0 失败、0 跳过。
- 当前 G8 已审核 fixture 增至 12/69；现场验收仍为 0，自动化插件矩阵仍为 `NOT_RUN`。

### G8-LOCAL-CONDITION-2026-10-30

- 范围：`Condition` 的 Linux/Qt 5 Agent 参数配置、上游数据读取、true/false 业务输出和缺少变量失败；使用真实 CreateString 插件作为受控数据源。
- E2E：Agent 配置 CreateString 输出 `PASS-42`，配置 Condition 的 Equal 比较并显式连接 image 端口。首轮在同一 `runId` 读到 `condition_result=true`；修改比较值后第二轮按其自身 `runId` 读到 `false`。再将 `variableName` 置空，插件允许配置但运行失败，精确归因 Condition 实例和 `变量名不能为空` 诊断。
- 独立审核：`/root/slice176_final_review` 只读 PASS，核验了数据流、同次运行读回、否定业务结果和运行失败边界。
- 验证：Condition 定向 fixture PASS；真实插件库和 metadata 从临时插件根加载，suite 启动时发现模块数与清单一致。`test_agent_core_plugins_g8` 在修正插件已加载状态断言后经 CTest `--repeat until-fail:3` 三次通过；全量构建退出码 0、CTest 112/112、0 失败、0 跳过。
- 当前 G8 已审核 fixture 增至 13/69；现场验收仍为 0，自动化插件矩阵仍为 `NOT_RUN`。

### G8-LOCAL-DELAY-2026-10-30

- 范围：`Delay` 的 Linux/Qt 5 Agent 参数配置、执行等待和零值校验；通过 TimeSlice 单调时钟测量，不涉及外部设备。
- E2E：Agent 配置并连接 TimeSlice Start → Delay（40 ms）→ Stop。该次完成运行中 Stop 输出 elapsed 不低于 40 ms 且低于 5000 ms。随后以 Agent 将 delayMs 设为 0，参数设置以 `VALIDATION_FAILED` 和精确诊断 `Delay time must be greater than 0` 拒绝。
- 独立审核：`/root/slice176_final_review` 只读 PASS，确认 elapsed 下界能验证真实延时已执行，真实插件从独立根加载，清理无状态残留。
- 验证：Delay 定向 fixture PASS；`test_agent_core_plugins_g8` 经 CTest `--repeat until-fail:3` 三次通过；全量构建退出码 0，CTest 112/112、0 失败、0 跳过。
- 当前 G8 已审核 fixture 增至 14/69；现场验收仍为 0，自动化插件矩阵仍为 `NOT_RUN`。

### G8-LOCAL-DATACHECK-2026-10-30

- 范围：`DataCheck` 的 Linux/Qt 5 Agent 参数配置、上游字段读取、范围业务判定和缺少输入失败；由真实 CreateString 插件提供测试数据。
- E2E：Agent 将 CreateString 的输出字段配置为 `check_data`，连接至 Range DataCheck `[40,45]`。同一 run 中 42 得到 `check_passed=true`；将上游设为 99 后，运行成功但业务输出为 false，精确错误为 `值 99 超出范围 [40, 45]`。再把上游字段改名使 `check_data` 缺失，运行失败并精确归因 DataCheck 实例、诊断 `未提供校验数据`。
- 独立审核：`/root/slice176_final_review` 只读 PASS，确认真实插件加载、显式数据边、同 run 结果和缺失字段失败断言。
- 验证：DataCheck 定向 fixture PASS；core suite 经 CTest `--repeat until-fail:3` 三次通过；全量构建退出码 0，CTest 112/112、0 失败、0 跳过。
- 当前 G8 已审核 fixture 增至 15/69；现场验收仍为 0，自动化插件矩阵仍为 `NOT_RUN`。

### G8-LOCAL-TABLEOUTPUT-2026-10-30

- 范围：`TableOutPut` 的 Linux/Qt 5 Agent 行列配置、空表生成和尺寸参数校验。
- E2E：Agent 创建真实 TableOutPut 实例，设置 2 行 × 3 列并运行；从同一 `runId` 读回行数 2、列数 3 和精确空表文本 `\t\t\n\t\t\n`。将 rowCount 设为 0 时，`set_module_params` 返回 `VALIDATION_FAILED` 及精确诊断 `行数必须在1到100之间`。
- 独立审核：`/root/slice176_final_review` 只读 PASS，核对输出字符串构造、Agent 读回和参数校验边界。
- 验证：TableOutPut 定向 fixture PASS；全量构建退出码 0，CTest 112/112、0 失败、0 跳过。
- 当前 G8 已审核 fixture 增至 16/69；现场验收仍为 0，自动化插件矩阵仍为 `NOT_RUN`。

### G8-GUI-TEST-HOME-ISOLATION-2026-10-30

- 发现并修复一项测试宿主隔离问题：`test_agent_viewport_snapshot_handoff` 默认继承真实 HOME，初始化 MainWindow 时启动 AgentBridge 并扫描用户插件目录；一次全量运行在聊天拒绝路径中进程中止。工作区内用户插件目录时间未变，未发现遗留 `agent.sock`。
- Linux CTest 现通过既有 `run-mainwindow-test-isolated.sh` 使用临时 HOME 和临时插件根执行该 suite。隔离 CTest 连续 3/3 通过；完整 CTest 112/112 通过。
- 原始复测记录：`build-codex-agent/agent-phase8-visual-plugin-evidence/handoff-isolation-repeat.log`。该修正只隔离测试宿主，不改变产品逻辑或用户插件数据。

### G8-LOCAL-PLUGIN-FIXTURE-SNAPSHOT-2026-10-30

- 当前综合快照：69 项插件身份（66 模块、3 相机）；66 个构建二进制存在；67 个插件有测试源码候选、2 个无候选；74 个候选 suite 在全量 CTest 通过；候选源码索引 1376 个 QtTest 用例名。
- 经独立审核的 G8 Agent 配置/业务/关键失败 fixture：VarDefine、VarSet、QRCode、ShowImage、PLCRead、Math、CreateString、SplitString、StrFormat、SystemTime、WriteText、TimeSlice、Condition、Delay、DataCheck、TableOutPut、SaveData、SaveImage、DisplayData、ColorRecognition、GrabImage、Blob，共 22/69。真实现场验收 0/69；全量自动化插件矩阵及其它插件 fixture 尚未完成。
- 最新验证：`cmake --build build-codex-agent -j2` 退出码 0；`QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure` 112/112、0 失败、0 跳过；`test_agent_core_plugins_g8`、`test_agent_visual_plugins_g8` 与隔离 GUI handoff suite 均重复三次通过；库存工具测试 12/12；生成器 `--check`、Python 编译、clang-format、SHA-256 和 `git diff --check` 全通过。
- 证据：`build-codex-agent/agent-phase8-visual-plugin-evidence/`；清单由 `scripts/audit_agent_g8_plugin_coverage.py` 校验，fixture 审核引用在 `docs/baseline/agent-g8-plugin-fixture-audits.json`。


### G8-LOCAL-SAVEDATA-2026-10-30

- 范围：Linux / Qt 5 Agent 配置 CreateString→SaveData，确认门控、JSON 序列化与目标写入失败。
- E2E：Agent 加载真实插件、创建两个模块，配置 `payload-42` 为 `g8_payload` 并连接 image 端口。确认前文件不存在；批准后 JSON 精确包含 `g8_payload=payload-42`、`string_length=10`。批准写入无父目录目标时运行失败归因到 SaveData，目标及父目录均未创建。
- 独立审核：`/root/slice176_final_review` PASS；复审 finding 已关闭，覆盖业务字段、重复加载状态、失败归因与副作用边界。
- 验证：定向用例及 core suite 重复 3/3 通过；全量构建退出码 0；全量 CTest 112/112、0 失败、0 跳过。
- 当前 G8 已审核 fixture 为 17/69；真实现场验收 0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-SAVEIMAGE-2026-10-30

- 范围：Linux / Qt 5 Agent GrabImage→SaveImage 图像输出、批准门控、PNG 内容与编码失败。
- E2E：Agent 从临时 PNG 路径采图并连接 SaveImage。确认前无输出文件；批准后重新解码输出，尺寸为 64×48 且逐像素与源图相同。批准无效扩展名后运行失败并归因到 SaveImage，目标父目录未创建。
- 独立审核：`/root/slice176_final_review` PASS；确认链路、精确图像比较、无副作用失败和生命周期清理均通过复核。
- 验证：SaveImage 定向用例通过，visual suite 连续 3/3 通过；最终全量构建和 CTest 结果见最新快照。
- 当前 G8 已审核 fixture 为 18/69；真实现场验收 0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-DISPLAYDATA-2026-10-30

- 范围：Linux / Qt 5 Agent GrabImage→DisplayData 文本覆盖与缺失输入失败。
- E2E：Agent 从隔离临时 PNG 采集 100×80 图像，连接 DisplayData 并设置两行叠加文本；同一 runId 读回精确文本 `lot=42\nOK` 和行数 2。禁用上游后运行失败，准确归因到 DisplayData 并匹配 `输入图像无效`。
- 独立审核：`/root/slice176_final_review` PASS；Agent 连线、输出读回、失败语义与清理均复核通过。
- 验证：DisplayData 定向用例及 visual suite 连续 3/3 通过；最终全量构建和 CTest 结果见本轮综合快照。
- 当前 G8 已审核 fixture 为 19/69；真实现场验收 0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-COLORRECOGNITION-2026-10-30

- 范围：Linux / Qt 5 Agent GrabImage→ColorRecognition 红色检测、业务输出与缺色失败。
- E2E：Agent 读取暗底红矩形图并配置红色检测；同一 runId 精确核对颜色名、单个轮廓、面积阈值和中心 `(34,34)±1`。换成纯白图后运行失败，归因于识别实例，诊断精确为 `未检测到目标颜色`。
- 独立审核：`/root/slice176_final_review` PASS；图像构造、中心容差、失败归因、临时插件根与清理均确认。
- 验证：定向 fixture 通过，visual suite 连续 3/3 通过；最终全量构建和 CTest 结果见本轮综合快照。
- 当前 G8 已审核 fixture 为 20/69；真实现场验收 0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-GRABIMAGE-2026-10-30

- 范围：Linux / Qt 5 Agent GrabImage Path 模式的成功来源信息与文件缺失错误。
- E2E：Agent 加载临时 PNG 后完成运行，同一 runId 精确读回绝对 `source_path` 和文件名；配置缺失路径后运行失败，归因 GrabImage 并返回精确“文件不存在”诊断。输入和路径均限于测试 QTemporaryDir。
- 独立审核：`/root/slice176_final_review` PASS；Agent 加载/配置、真实运行、失败归因、隔离和清理均通过复核。
- 验证：定向 fixture 通过，visual suite 连续 3/3 通过；最终全量构建和 CTest 结果见本轮综合快照。
- 当前 G8 已审核 fixture 为 21/69；真实现场验收 0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-BLOB-2026-10-31

- 范围：Linux / Qt 5 Agent GrabImage→Blob 配置、Otsu 阈值、面积/圆度过滤及无目标失败。
- E2E：Agent 对暗底白圆图设置 Otsu、minArea=200、maxArea=1000、minCircularity=0.7；同一 runId 读回一个 Blob、面积大于 600、中心 `(35,40)±1`。全暗图运行失败，归因 Blob，精确诊断为 `未检测到Blob`。
- 独立审核：`/root/slice176_final_review` PASS；参数、中心点容差、失败归因与临时插件生命周期均复核通过。
- 验证：定向 fixture 通过，visual suite 连续 3/3 通过；最终全量构建和 CTest 结果将在本轮综合快照记录。
- 当前 G8 已审核 fixture 为 22/69；真实现场验收 0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-REGIONSTATS-2026-10-31

- 范围：Linux / Qt 5 RegionStats 真实插件的 Agent 数值读回、坐标来源边界及 required-port 失败。
- Fixture：固定 Region2D 方形的面积 400、bbox `(120,120,20,20)`、质心 `(130,130)` 均同时对 runtime 输出和 Agent key/port 读回做固定 oracle；缺少 region 数据时，RunEngine 在 `stats` 的框架 required-port gate 失败，Agent 报告 run failed 且 `region_area` unavailable。
- 独立审核：`/root/slice176_final_review` PASS；指出并关闭固定面积/bbox oracle 缺口，复核隔离插件根、失败归属和清理。
- 验证：RegionStats 所在 `test_sam_region_flow` 连续 3/3 通过；全量构建成功；CTest 112/112、0 失败、0 跳过（attempt18）。受限沙箱回环绑定失败记录单独保留，成功验证在允许本机回环的环境执行。
- 当前 G8 已独立审核 fixture 为 49/69；现场验收仍为 0/69，G8 自动化插件矩阵及外部设备/provider/platform 验证仍未完成。


### G8-LOCAL-PARALLEL-2026-10-31

- 范围：Linux / Qt 5 Parallel Agent 配置、all-join 汇合与并行失败取消。
- Fixture：Agent 将 `par`、`merge` 两节点配置为 `parallelCount=3`，完成运行后通过 `key=parallel_count` 读回 3；两个分支均在 merge 前完成，且实际最大并发数不小于 2。失败 fixture 确认 `bad` 分支开始并失败，`slow` 同批开始后被取消，`merge`/`after` 未启动，Agent 同 run 的 slow image 输出不可用。
- 独立审核：`/root/slice176_final_review` PASS；审核确认 key/port 类型、失败归因及插件清理生命周期表述均准确。
- 验证：`test_acceptance_flows` 连续 3/3 通过；全量构建成功；CTest 112/112、0 失败、0 跳过（attempt19）。
- 当前 G8 已独立审核 fixture 为 50/69；现场验收仍为 0/69，G8 自动化插件矩阵及外部设备/provider/platform 验证仍未完成。


### G8-LOCAL-MATCHING-2026-10-31

- 范围：Linux / Qt 5 Matching Agent 参数配置失败语义、图像匹配数值和坐标来源读回。
- Fixture：Agent set_module_params 对阈值 1.1 返回 VALIDATION_FAILED 与精确诊断，工程参数仍为 0.9/2 且引擎未运行；有效 0.8/5 返回 params_set。测试使用同一自定义图像 factory 在参数写入后重载工程，再由 RunEngine 执行，避免丢失确定性图像和 provenance；不把此用例计作 Agent run_flow 编排覆盖。固定匹配框 (65,45,30,30)、得分 1.0；Agent key/port 坐标值带 px，DetectionList 源图框精确 (170.5,167,45,60)。
- 独立审核：/root/slice176_final_review PASS；确认 STALE_OUTPUT 根因是参数更新后未刷新运行工程指纹，使用同 factory reload 修正，输出合同与测试断言一致。
- 验证：test_acceptance_flows 连续 3/3 通过；全量构建成功；CTest 112/112、0 失败、0 跳过（attempt20）。此前 stale-output 失败日志保留。
- 当前 G8 已独立审核 fixture 为 51/69；现场验收仍为 0/69，真实采集、现场图像准确率及其他平台/provider 验证未完成。


### G8-LOCAL-SAMPREDICT-2026-10-31

- 范围：Linux / Qt 5 SamPredict Agent 本地模拟 peer 软件 fixture；合同与实现经 `/root/slice176_final_review` 只读复审 PASS。
- 验证：全量构建退出码 0；`test_sam_region_flow` 连续 3/3；全量 CTest attempt22 为 112/112、0 失败、0 跳过；日志与二进制/源码哈希登记在 `build-codex-agent/agent-phase8-visual-plugin-evidence/`。
- 结果：空 URL 拒绝及参数无变更、缺 prompt 运行失败且零 HTTP 请求、真实插件链、精确 SAM 请求体、裁剪后坐标和 Agent 区域/统计读回均通过。独立审核结论为 PASS。
- 边界：本地 peer 不代表真实权重、分割质量、GPU/provider、生产图像、相机硬件、其他平台或现场验收。当前独立审核软件 fixture 为 52/69，现场验收 0/69，G8 自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-EDGEDEFECTDETECTION-2026-10-31

- 范围：Linux / Qt 5 EdgeDefectDetection Agent 本地软件 fixture，合同及实现由 `/root/slice176_final_review` 独立只读审核 PASS。测试身份为 Developer，仅覆盖授权执行。
- 验证：全量构建退出码0；`test_acceptance_flows` 连续3/3；全量 CTest attempt24 为112/112、0失败、0跳过。
- 结果：Agent 阈值拒绝/无变更、阈值4成功配置、固定缺陷数值与区域 oracle、key/port row 全字段同run snapshot一致、image-local点集单位边界、Agent两点输入导致插件精确失败及 failed-run 输出失效均通过。
- 限制：仅生成图像的软件测试，不证明生产图像缺陷准确率、真实采集/性能或其他平台。G8 独立审核软件 fixture 为53/69，现场验收0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-JIERHANDEFECTSDET-2026-10-31

- 范围：Linux / Qt 5 JiErHanDefectsDet Agent本地合成图软件 fixture；启发式实验算法，合同和实现经 `/root/slice176_final_review` 只读复审 PASS。
- 验证：全量构建成功；`test_acceptance_flows` 连续3/3；全量 CTest attempt25 为112/112、0失败、0跳过。
- 结果：阈值1.5拒绝/无修改；Agent threshold 0.1/0.9分别产生2/1候选；Region2D本地与来源图 bbox、来源ID正确；仅弱候选时插件精确失败且 Agent 标记 failed/unavailable；无provenance时不生成来源图坐标。
- 边界：不证明工业缺陷识别准确率、产线图像、现场相机/平台或现场验收。独立审核软件 fixture 为54/69，现场验收0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-IMAGESCRIPT-2026-11-02

- 范围：Linux / Qt 5.15.3 ImageScript Agent 软件 fixture；测试通过隔离临时插件目录动态加载真实 GrabImage/ImageScript 二进制与仓库 metadata。契约和实现均经 `/root/slice176_final_review` 独立只读审核 PASS。
- Fixture：Agent 配置 Path GrabImage 与 ImageScript scriptType=0，运行同一 flow；Agent 读取 `script_executed=true`、`script_type=0`，RunEngine 同 run 图像逐像素等于固定输入的 `cv::bitwise_not`，provenance 的 sourceImageId、sourceSize、localToSourceMatrix 完全相同。`scriptType=4` 精确返回 VALIDATION_FAILED，参数不变、无新 moduleStarted。断开图像边并禁用 GrabImage 后，可选输入通过预检，ImageScript 运行时以 `输入图像无效` 失败，Agent 输出为 unavailable/run_failed。
- 验证：全量构建成功；`test_agent_visual_plugins_g8` CTest 1/1，套件 QtTest 用例 24/24；获准 localhost loopback 全量 CTest 112/112、0失败、0跳过。受限沙箱全量日志单独保留：97/112，其中 15 个本地 socket/HTTP listener 套件受环境限制失败，ImageScript 所在 suite 通过。格式检查、`git diff --check`、库存生成与检查通过。
- 边界：仅测试生成的 4×3 图像和反转模式，不代表其他脚本模式、生产图像质量或现场/模型/provider/platform 验收。当前 G8 独立审核软件 fixture 为55/69，现场验收仍为0/69，完整插件矩阵及真实外部验证保持未完成。


### G8-LOCAL-LOADPOINTCLOUD-FREEFORMSURFACE-2026-11-02

- 范围：Linux / Qt 5.15.3 LoadPointCloud→FreeformSurface Agent 软件 fixture；通过隔离临时插件目录加载真实二进制与 metadata。移除没有坐标单位或算法定义的 `samplingInterval` 参数；旧工程字段仍可加载但不应用。契约和实现经 `/root/slice176_final_review` 独立只读审核 PASS。
- Fixture：Agent 用生成的四点单位正方形 PLY配置 LoadPointCloud→FreeformSurface，读回 Loader point_count=4、FreeformSurface point_count=4/surface_area=1/surface_roughness=0。缺失 PLY 路径时失败精确归因到 LoadPointCloud、point_count unavailable/run_failed；禁用 Loader 后，FreeformSurface 对缺失点云以 `未提供点云数据` 失败。Agent 参数 schema 不再发布采样间隔；尝试写入时以 INVALID_PARAM/Unknown parameter 拒绝且不运行；插件单测另验证旧 JSON 可加载且 runtime params 不含字段。
- 验证：全量构建退出码0；G8 visual suite QtTest 26/26、FreeformSurface 8/8、final plugins 16/16；全量 CTest 在获准 localhost loopback 环境112/112、0失败、0跳过。受限沙箱全量日志单独保留：97/112，15 个本地 socket/HTTP listener 用例受环境限制失败。格式、JSON、`git diff --check` 通过。
- 边界：不实现空间降采样；点云坐标单位若未声明则不假定为毫米，粗糙度单位为输入坐标单位，面积单位为其平方。现场点云、不同输入来源和真实设备仍待验收。当前 G8 独立审核软件 fixture 为57/69，现场验收0/69，自动化插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-JIGSAWPUZZLE-2026-11-02

- 范围：Linux / Qt 5.15.3 JigsawPuzzle Agent 软件 fixture；隔离目录动态加载实际 GrabImage/JigsawPuzzle 插件。契约与实现经 `/root/slice176_final_review` 独立只读审核 PASS。
- Fixture：Agent 设置 rows=3、cols=4，输入生成的非均匀19×17图像；按同一 run 读取 puzzle_rows=3、puzzle_cols=4、puzzle_piece_count=12。RunEngine同run输出像素逐项等于源图左上16×15裁剪，明确验证不可整除余量会丢弃。改用2×2源图后，运行在 JigsawPuzzle 精确失败`图像尺寸不足以按当前行列数切分`，Agent失败run输出为 unavailable/run_failed。
- 验证：全量构建成功；visual suite 27/27；获准 localhost loopback 全量 CTest 112/112、0失败、0跳过。受限沙箱结果分开保留，15个本地 socket/HTTP listener 用例因环境权限失败。格式与差异检查通过。
- 边界：当前实现只将单图切成等大网格再按行顺序原位重组；不识别多张独立碎片或恢复乱序拼图。产品文档和能力矩阵已明确此限制。当前 G8 独立审核软件 fixture 为58/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-QUEUEOUT-2026-11-02

- 范围：Linux / Qt 5.15.3 QueueOut 空队列 Agent fixture；隔离目录加载真实 QueueOut 二进制与 metadata。契约和实现经 `/root/slice176_final_review` 独立只读审核 PASS。
- Fixture：Agent 配置 UUID 唯一队列名和自定义 `outputVariable`，空队列运行成功；同一 run 的输出为 `queue_empty=true`、自定义输出键为空字符串、`queue_<name>_size=0`，执行前后队列大小保持0。修复了空队列分支忽略自定义输出键并缺少 size=0 的问题；非空出队和 `peekOnly` 路径未改动。
- 验证：全量构建退出码0；QueueOut Agent 定向用例3/3、`test_queueplugins` 13/13；全量 CTest 在受限环境96/112（16个本地 listener/相机发现相关测试受限制，QueueOut所在视觉套件通过），获准 loopback 全量 CTest 112/112、0失败、0跳过；G8 inventory auditor 12/12，库存生成/检查通过。
- 边界：验证空队列行为，不代表并发顺序或真实设备/现场验收。当前 G8 独立审核软件 fixture 为59/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-QUEUEIN-2026-11-02

- 范围：Linux / Qt 5.15.3 QueueIn 字符串入队 Agent fixture；隔离目录动态加载真实 CreateString、QueueIn、QueueOut 二进制与 metadata。契约与实现经 `/root/slice176_final_review` 独立审核 PASS。
- Fixture：Agent 配置 CreateString 输出 `lot-42-检测` 到 ImageData 键 `message`，QueueIn 从该键入队，QueueOut 同轮取出；同 run 可读 QueueIn size=1、QueueOut 精确字符串与 `queue_empty=false`，运行后队列 size=0。禁用 CreateString 后，QueueIn 精确以 `队列数据为空` 失败，失败 run 输出为 unavailable/run_failed，队列仍为0。
- 验证：QueueIn 定向 Agent 用例3/3，visual suite 29/29，全量构建成功；全量 CTest 结果记录于本节证据文件，受限与获准 loopback 结果分别列于 summary。
- 边界：只覆盖字符串路径；QueueIn 当前把选中值转换为 QString，非字符串 QVariant 保真、并发顺序和现场验收不在本 fixture 范围。独立审核软件 fixture 60/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-GAPMEASURE3D-2026-11-02

- 范围：Linux / Qt 5.15.3 GapMeasure3D Agent 合成高度图 fixture；隔离目录动态加载真实 GrabImage/GapMeasure3D 插件。契约与实现经 `/root/slice176_final_review` 独立审核 PASS。
- Fixture：40像素宽、深10的合成槽，X像素间距0.5，Agent同run读出 gap_width≈20.0 mm、offset后≈20.5 mm、corner_dx_mm一致且 is_pass=true。平面图正常完成，gap_found=false并返回配置哨兵-3.5。roiCenterX=2000但仍在参数合法范围内时，精确失败`截面 ROI 与图像无交集`且 failed-run 输出 unavailable/run_failed。明确核对 gap_width、gap_offset_width、corner_dx_mm 为 mm；corner_dz_mm、corner_dist_mm 未声明单位。
- 验证：定向 Agent 用例3/3、visual suite30/30；本节全量构建与CTest结果以 `build-codex-agent/agent-phase8-gapmeasure3d-evidence/summary.md` 为准。
- 边界：仅软件合成高度图，不证明真实设备、工业测量精度、标定或现场验收。当前 G8 独立审核软件 fixture 为61/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-PERPROCESSING-2026-11-02

- 范围：Linux / Qt 5.15.3 PerProcessing Agent GaussianBlur fixture；隔离目录加载真实 GrabImage/PerProcessing 插件。修复非法 processType 静默直通成功，并同步 metadata 的11种选项和参数范围。契约、实现均经 `/root/slice176_final_review` 独立审核 PASS。
- Fixture：5×7确定性BGR图按公式生成，与同参数 OpenCV GaussianBlur 默认边界逐像素一致且不同于原图；同run输出尺寸、类型和有效图像 provenance 均核对。Agent验证 kernelSize 1/31、sigmaX 0/100、iterations 1/10 被实际写入；越界与小数整数值拒绝且不变更。UnsupportedFilter 精确返回 VALIDATION_FAILED/`不支持的处理类型: UnsupportedFilter`，无运行启动。无输入时以 `输入图像无效` 在插件处失败且输出 unavailable/run_failed。
- 验证：PerProcessing Agent 定向用例3/3，`test_imageplugins` 12/12，visual suite31/31；当前快照全量构建及 CTest 记录见 `build-codex-agent/agent-phase8-perprocessing-evidence/summary.md`。
- 边界：像素 oracle 仅覆盖 GaussianBlur，不说明其他滤波器质量或真实图像性能。G8 独立审核软件 fixture 为62/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-PREPROCESSING3D-2026-11-02

- 范围：Linux / Qt 5.15.3 PreProcessing3D Agent 高度图 fixture；隔离目录动态加载真实 GrabImage/PreProcessing3D 插件。契约及实现经 `/root/slice176_final_review` 独立审核 PASS。
- Fixture：Agent 从7×9单通道float TIFF读取线性高度图，same-run读出 valid_pixel_count=63、filtered_pixel_count=0、min_height=10、max_height=44；Agent schema 声明 image 类型 HeightMap2D，运行时 port 查询返回 ImageData 摘要9×7，RunEngine同run像素快照逐项等于源高度图。改用三通道 PNG 后，在 PreProcessing3D 精确失败`高度图须为 1 或 2 通道，收到 3 通道`，输出 unavailable/run_failed。
- 验证：定向 Agent 用例3/3、visual suite32/32；全量构建和 CTest 见 `build-codex-agent/agent-phase8-preprocessing3d-evidence/summary.md`。
- 边界：覆盖单通道浮点输入及三通道拒绝，不覆盖双通道提取、NoData复杂分布、过滤/ROI行为或现场精度。G8独立审核软件 fixture 为63/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-WHILE-2026-11-02

- 范围：Linux / Qt 5.15.3 While Agent 假条件 fixture；隔离临时插件目录加载真实 CreateString/While 二进制与仓库 metadata。契约和实现均经 `/root/slice176_final_review` 独立只读审核 PASS。
- Fixture：CreateString 在同 run 生成可读回的空 `loopGate`，While 配置 `NotEmpty` 后完成并返回 `while_result=false`。Agent set 参数接受并写入 maxIterations=1/100000；0、100001、小数和未知 comparison 均精确返回 `VALIDATION_FAILED`，工程参数保持不变；拒绝未知 comparison 时 RunEngine idle 且无 `moduleStarted`。
- 验证：全量构建退出码0；While 定向 QtTest 用例及 init/cleanup 共3/3；视觉 Agent suite 33/33；获准 localhost loopback 全量 CTest 112/112、0失败、0跳过。G8 inventory auditor 12/12，库存生成/检查通过；`git diff --check` 干净。完整日志、构建输出和摘要见 `build-codex-agent/agent-phase8-while-evidence/`。
- 边界：只测 `NotEmpty` 对空字符串、参数边界和非法枚举，不证明 While body/done 后继调度、StopWhile、其他比较的语义或现场性能。G8 独立审核软件 fixture 为64/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-STOPWHILE-2026-11-02

- 范围：Linux / Qt 5.15.3 StopWhile Agent 控制流 fixture；隔离目录加载真实 While、StopWhile、Delay 二进制与 metadata。为让 Agent 能搭建引擎已有的控制流，`connect_modules` 现支持隐式 `next` 输出和 `control` 输入，控制目标可有多条来源；Agent 工具说明区分 implicit/Any 的 OR 激活与 `joinPolicy=all` 的全边等待。实现与契约经 `/root/slice176_final_review` 独立只读审核 PASS。
- Fixture：Agent 建 While(空条件变量比较 Empty、maxIterations=1000)→Delay→StopWhile，并将 StopWhile.stop 与 While.done 汇入 after Delay.control。四条边均报告 control，错误 image→control 被拒绝且图未变；get_flow_state 读回四边。完成后 validate_flow=valid，StopWhile 同 run 输出 stop_while_requested=true、while_result=true；body、StopWhile、after 各启动一次。
- 验证：全量构建退出码0；visual Agent suite 34/34；loopback 全量 CTest 112/112、0失败、0跳过；G8 inventory auditor 12/12、生成/检查通过；格式检查和 `git diff --check` 干净。日志与摘要见 `build-codex-agent/agent-phase8-stopwhile-evidence/`。
- 边界：只验证 StopWhile 在一个显式 While 中的首次提前退出和 implicit control 多源 OR；不覆盖嵌套循环、取消、显式 `joinPolicy=all` 行为本身或现场验收。G8 独立审核软件 fixture 为65/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-LOCAL-V4L2-2026-11-02

- 范围：Linux 下生产 V4L2CameraPlugin 的 fake-system Agent discovery fixture。插件通过构造器注入 `IV4L2DiscoverySystem`，由 CameraManager 托管异步发现，再通过 Agent `list_cameras` 查询；契约和实现经 `/root/slice176_final_review` 独立只读审核 PASS。测试使用 RAII 注册守卫，提前失败时也不会让 manager 留下悬空插件指针。
- Fixture：假系统提供 `/fake/video0` 有效 V4L2 与 `/fake/video1` open 失败设备。Agent 仅列出一个 disconnected `Fake camera`；适配器记录2次 open、成功描述符仅1次 close。随后 `tryUnregisterCameraPlugin` 成功、manager 插件表和 Agent 列表均已清除后才销毁插件。
- 验证：全量构建退出码0；`test_v4l2discovery` 8个测试及 init/cleanup 共9/9；loopback 全量 CTest 112/112、0失败、0跳过；库存 auditor 12/12、库存生成/检查通过；格式、JSON、`git diff --check` 干净。证据见 `build-codex-agent/agent-phase8-v4l2-evidence/`。
- 边界：Linux-only fake adapter discovery，不加载应用打包插件，不访问真实 `/dev/video*`，不连接或采帧。真实摄像头连接、参数、图像和现场验收仍为 `NOT_RUN`。G8 独立审核软件 fixture 为66/69，现场验收0/69，完整插件矩阵仍为 `NOT_RUN`。


### G8-REMAINING-GATES-2026-11-02

- 软件 fixture 盘点：69项中66项有独立审核通过的有效业务 fixture，67项有独立审核通过的关键失败 fixture；当前构建存在67个插件二进制。`DefectDetection` 的 SDK 缺失失败已审核，但有效业务 fixture 失败；另有2项平台/SDK 插件未获 fixture 审核：
  - `DirectShowCamera`：Linux 不适用，需 Windows 构建/运行环境。
  - `HikvisionCamera`：`ENABLE_CAMERA_HIKVISION=OFF`；构建需要 MVS SDK（`MVS_SDK_ROOT`）。
  - `DefectDetection`：当前会构建 demo-mode 插件并对 Agent 明确报告 SDK 缺失；有效算法 fixture 需要 HymsonVision3D SDK。现场质量/模型验收仍需目标环境。
- 外部现场门槛：真实 LLM/provider 固定任务、真实 V4L2/相机设备、声明支持平台与 SDK、设备/模型质量性能、PLC/串口现场验证均为 `NOT_RUN`；现场接受计数仍为0/69。
- 全量 Agent 能力矩阵仍为 `NOT_RUN`。以上项目需要相应平台、SDK、设备和 provider 配置才能取得有效证据；不得由本地 fake/synthetic fixture 推断通过。
- 本次仓内可复现范围的最后快照：Linux / Qt 5.15.3，全量构建成功，获准 localhost loopback CTest 112/112；有效业务 fixture 66/67（已审核），关键失败 fixture 67/67（已审核），现场接受 0/69。G8 保持未通过，直到适用插件业务 fixture、外部门槛与完整矩阵门槛完成。


### G8-LOCAL-DEFECTDETECTION-2026-11-02

- 范围：修正 `ENABLE_HYMSON3D=OFF` 时 CMake 提前返回、导致注释声明的 demo-mode `DefectDetectionPlugin` 根本不构建的问题。现在无 SDK 时仍构建框架插件；运行时明确以 `HymsonVision3D SDK 未启用，缺陷检测不可用` 失败，不伪造算法结果。
- Agent 链路：`LoadPointCloud` 从 suite 临时目录读取四点 PLY，经 `image` carrier 连至 DefectDetection；输入/输出 carrier metadata 设为 `Any`，使 Agent 连接与 `ImageData` 载体语义一致。DefectDetection 的 `defect_point_cloud`、`has_defects`、`defect_count` 已声明为输出端口，并由 Agent `get_flow_state` fixture 核验。
- 失败 fixture：`testAgentReportsHymsonSdkUnavailableForDefectDetection` 确认 Agent 加载/配置两个真实动态插件、连接输入并启动；本次失败归因 DefectDetection 实例，`lastError` 和插件日志精确报告 SDK 未启用。它是关键失败路径 PASS；有效算法业务 fixture 为 FAIL，不能算作业务通过。
- 独立审核：`/root/defect_detection_g8_review` 第二轮 **PASS**，限 demo-mode 构建、输入/输出 metadata 和 SDK 缺失失败语义；明确不证明算法成功或整体 G8 通过。
- 验证：Linux / Qt 5.15.3；全量构建退出码 0；获准 loopback 全量 CTest 112/112、0 失败、0 跳过；定向 Agent 用例通过；库存工具测试 12/12、清单生成与 `--check` 通过；C++ clang-format、metadata JSON、Python 编译、`git diff --check` 通过。库存为 69 插件、67 个当前二进制、66 个有效业务 PASS / 67 个已审核业务 fixture、67 个关键失败 PASS、现场接受 0/69。
- 证据：`build-codex-agent/agent-phase8-defectdetection-evidence/` 与当前 canonical 全量 CTest `build-codex-agent/agent-phase8-visual-plugin-evidence/ctest-full.log`。
- 限制：当前环境 `ENABLE_HYMSON3D=OFF`，未验证成功算法路径；CMake 插件子目录仍以 `AND FALSE` 禁用 SDK 代码，真实 HymsonVision3D 集成、算法输出/质量、设备和目标平台须另行完成。DirectShow 仍需 Windows，Hikvision 仍需 MVS SDK；真实 provider/设备/现场及完整 G8 能力矩阵均未完成。


### G8-LOCAL-DEFECTDETECTION-DOCS-2026-11-03

- 契约：`docs/baseline/agent-phase-8-defectdetection-docs-contract.md`。
- 变更：`docs/user-manual.md`、`docs/plugins.md` 和 QRC 内置 `DEEPLUX.md` 一致说明当前 SDK-disabled 状态、准确错误消息、`AND FALSE` 构建守卫，以及 `defect_point_cloud`、`has_defects`、`defect_count` 仅为 schema-visible ports；当前运行不会生成可查询结果。未将算法描述成已可用，也未提升其业务 fixture 状态。
- 自动化：`test_agentactor::testReadDocumentationSearchesBuiltInAndUserKnowledge` 通过真实 `read_documentation(topic="DefectDetection")` 响应断言三个来源标签、错误消息、守卫文本和输出端口名称。重建 QRC 后 `test_agentactor` CTest 1/1 通过。
- 限制：这是文档和知识库同步验收，不证明算法成功、真实 SDK 可用、现场质量或 G8 整体通过；这些门槛仍按 G8-LOCAL-DEFECTDETECTION 记录保持未完成。


### G8-LOCAL-CURRENT-MATRIX-2026-11-03

- 契约：`docs/baseline/agent-phase-8-current-matrix-contract.md`。
- 产物：`docs/baseline/agent-g8-current-acceptance-matrix.json` 为唯一结构化数据源；Markdown 由 `scripts/audit_agent_g8_current_acceptance_matrix.py` 生成。旧的 `agent-g8-plugin-coverage-inventory` 保留为候选扫描清单，不再作为验收矩阵。
- 当前对账：69 项插件（66 modules + 3 cameras），67 个当前二进制；历史基线 66 个，差异为新增可见的 DefectDetection demo-mode framework binary。软件 fixture 状态：66 `PASS_SOFTWARE_FIXTURES`、DefectDetection `FAIL_BUSINESS_FIXTURE`、DirectShow `BLOCKED_PLATFORM`、Hikvision `BLOCKED_SDK_OR_BUILD_OPTION`。现场验收 0/69。
- GUI 操作对账：126 项保留稳定 operationId 与原映射；16 项 test source/testId/evidence suite 能解析到当前全量 CTest，110 项明确 `NOT_LINKED`。映射另计 66 direct tool、37 human handoff、18 unsupported、5 tool composition；evidence-linked 不等同于语义审核 PASS。
- 验证：全量构建 `cmake --build build-codex-agent -j2` 成功；当前源码全量 CTest 在 loopback 获准环境 **112/112 通过、0 失败、0 跳过**。默认受限环境复跑的 15 个失败套件都在业务前置阶段因 `peer.listen()` / localhost listener 被拒而失败，原始日志保留，未计作产品回归失败，也未隐藏。G8 inventory auditor 单测 **12/12**、current-matrix auditor 单测 **6/6**；两个矩阵生成器 `--check`、Python 编译与 `git diff --check` 通过。`/root/defect_detection_g8_review` 独立复审 **PASS**，检查了注册来源校验、fail-closed 引用、计数和 DefectDetection 二进制 SHA；整体 G8 仍未通过。
- 限制：矩阵明确列出 110 项尚未关联单项测试，且不替代完整维度矩阵、多模式插件逐模式覆盖、真实 LLM/provider、真实设备、GPU、Windows 或现场门槛；`overallStatus` 保持 `NOT_READY`。完整数据、授权/受限 CTest 原始日志、命令输出及 SHA-256 见 `build-codex-agent/agent-phase8-current-matrix-evidence/`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE1-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`agent-gui-operations.json` 为 10 项既有操作补入测试函数与注册源链接：三个工作区面板、3D 渲染模式、主视口 zoom/fit、点大小、检查器固定、知识库对话框导航、About 导航、Agent 设置打开/复用/取消/手动保存。未改变工具映射、owner API 或行为说明。
- 边界：链接仅证明测试函数及 suite 可解析；Agent 设置不代表凭据/模型配置和连接已自动完成，知识库导航不代表文档导入/删除已完成；这类用户操作保持人机协作。
- 验证：当前完整 CTest 日志的相关 suites 为 PASS，操作源均已注册；矩阵重生成后 126 项中 26 项为 `EVIDENCE_LINKED`、100 项为 `NOT_LINKED`，映射分布不变。current-matrix 单测 6/6、`--check` 和 `git diff --check` 通过。`/root/slice176_final_review` 独立语义复审 **AUDIT_PASS**，确认 panel 三种值逐一驱动，Settings/KnowledgeBase 测试没有把导航扩写为自动配置/文档导入通过。整体 G8 仍 `NOT_READY`。
- 后续：继续为其余可由现有自动化证据支持的操作建立逐项引用；未支持和人机协作项须保留真实边界，不能只为降低 NOT_LINKED 计数而挂接不相关测试。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE2-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `VIEW_TOGGLE_THEME`、`VP_TOGGLE_2D3D`、`GV_CREATE/READ/SET/REMOVE`、`KB_LIST/PREVIEW/ADD/REMOVE_DOC` 增加十条已注册测试用例引用；操作 ID、owner API、映射、阶段和说明未改。
- 边界：主题链接覆盖 Agent `set_theme`，不证明菜单点击；视口链接覆盖显式主视口模式设置，不证明所有图像/点云组合；变量证据覆盖测试中的 int/string/int-array 样例，不代表全部类型或 GUI 顺序；知识库证据覆盖被测副本导入、列举、预览、删除和安全边界，不代表所有格式、UI 布局或每种环境权限。危险操作确认 UI 也不在该测试断言范围内。
- 验证：current-matrix auditor 生成和 `--check` 通过，单测 6/6；126 项中 `EVIDENCE_LINKED` 为 36、`NOT_LINKED` 为 90，映射分布仍为 66 direct tool、37 human handoff、18 unsupported、5 tool composition。引用 suite 在当前全量 CTest 日志通过（112/112）。`/root/slice176_final_review` 独立语义复审 **PASS**，确认十条引用与测试断言匹配且契约未扩大覆盖结论。整体 G8 仍 `NOT_READY`。
- 后续：继续筛查现有注册测试可直接支持的操作引用；外部设备、平台、真实 provider 和现场验收仍按 G8 门槛单独处理。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE3-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `INSP_COLLAPSE/INSP_VISIBILITY`、`DS_DELETE_REF`、`DS_DISPLAY_MAIN/DS_VIEW_STATE/DS_SET_VISIBILITY`、`PTREE_DELETE/PTREE_REORDER`、`KB_SEARCH`、`SAM_SESSION_QUERY` 增加十条现有 QtTest 引用。operationId、owner API、工具映射和行为说明保持原值。
- 边界：测试覆盖 Agent 工具和对应受测 GUI/model 状态；不把工具证据描述为鼠标右键/拖拽手势、磁盘引用物理删除、全部数据格式或所有 viewport 组合。SAM 项只证明分页只读会话查询，不证明图像重载、掩膜重算、模型推理或导出。
- 验证：current-matrix 生成、`--check` 和单测 6/6 通过；矩阵为 126 项中 46 `EVIDENCE_LINKED`、80 `NOT_LINKED`，插件和 mapping 分类计数不变。`test_mainwindow`、`test_agentactor`、`test_agent_inspector_pin`、`test_agent_sam_tools` 已在当前全量 CTest 日志通过，完整日志 112/112。`/root/slice176_final_review` 独立复审 **PASS**，逐项确认映射断言和范围边界。
- 限制：引用是可追踪测试索引，不表示整个 GUI 操作或 G8 验收 PASS。整体 G8 仍 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE4-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `INSP_ENABLED/INSP_BREAKPOINT/INSP_NOTE`、`DS_LIST/DS_DISPLAY_NEW_VIEWPORT`、`VIEWPORT_LIST`、`SAM_CANCEL_PROMPT/SAM_SESSION_SAVE/SAM_SESSION_OPEN` 和 `TB_STEP` 增加十条既有测试引用；未更改操作映射或行为说明。
- 边界：检查器字段引用证明 Agent 工具和 Project/RunEngine/Undo 行为，不声称 widget 绘制；视口列举限于受测 GUI 状态；单步共用位置测试通过直接调用 MainWindow slot，不是工具栏点击；SAM 保存/打开测试不覆盖文件选择器或真实推理。
- 验证：矩阵生成、`--check`、单测 6/6 与 `git diff --check` 通过；当前矩阵有 56 项 `EVIDENCE_LINKED`、70 项 `NOT_LINKED`，126 项总数和其它分类分布不变。引用 suite 在当前完整 CTest 日志通过（112/112）。`/root/slice176_final_review` 独立语义复审 **PASS**，尤其确认 `DS_LIST` 后续删除断言没有被误写成列表 UI 证据。
- 限制：这是测试索引，不构成整体 GUI 或 G8 PASS；真实设备、provider、Windows/GPU 和现场接受仍未完成。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE5-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：新增 8 项已有测试引用，覆盖流程树选择、运行日志清理/过滤/tab 切换、最近工程查询/Agent 新建工程、SAM 单正点预测工具路径和 `sam_annotate` 写入路径；未更改工具映射、owner API 或操作描述。
- 边界：工程创建测试只证明 Agent `create_project` 与 Undo 历史重置，不证明 File 菜单或未保存对话框；最近工程测试不证明列表窗口绘制；SAM 提示只覆盖一个正点输入且使用本地模拟服务，确认测试不证明真实模型或手工对话框确认。
- 验证：矩阵生成、`--check`、单测 6/6、`git diff --check` 通过；126 项操作中 64 `EVIDENCE_LINKED`、62 `NOT_LINKED`，插件与 mapping 分类总数不变。相关 CTest suites 见当前完整 112/112 日志。`/root/slice176_final_review` 独立语义复审 **PASS**，检查了全部八条引用和范围限制。
- 排除：未关联 toolbox drag-add；当前测试使用纯文本 payload，不等价于插件工具箱真实拖放载荷。整体 G8 仍 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE6-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：增加 8 项现有 GUI 测试引用：工程保存、单次/循环运行、节点拖动、参数页/默认值重置、相机发现刷新和连接生命周期；原 `toolMapping` 原样保留，尤其循环 Agent 启动、画布移动、参数重置和相机控制仍是未支持。
- 边界：循环运行只覆盖 GUI 启动与 Agent 停止；节点拖动只覆盖单节点；相机覆盖使用假插件，不证明真实硬件、驱动/SDK或 Agent 设备控制。
- 验证：current-matrix 生成、`--check`、单测 6/6 和 `git diff --check` 通过；矩阵为 126 项中 72 `EVIDENCE_LINKED`、54 `NOT_LINKED`，mapping 分类计数与插件计数不变。对应 suite 在当前完整 CTest 日志通过（112/112）。`/root/slice176_final_review` 独立审查 **PASS**，确认 unsupported 能力没有因 GUI 测试被误标为 Agent 支持。
- 限制：这些本地测试索引不关闭真实设备、Windows、现场和整体 G8 门槛。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE7-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为两点距离、点到线、线段间距、三点角度、三点拟合圆、矩形面积和 SAM 候选切换增加 7 项测试引用；不改变原有人工点选映射。
- 边界：测量测试证明受测 2D GUI 手工点选、坐标转换和结果显示，不证明 Agent 自主选择测点；SAM 候选由测试注入，不证明真实后端/模型生成质量。
- 验证：current-matrix 生成、`--check`、单测 6/6、`git diff --check` 通过；当前矩阵 126 项中 79 `EVIDENCE_LINKED`、47 `NOT_LINKED`。`test_mainwindow` 与 `test_samannotatordialog` 对应 suite 在全量 CTest 112/112 日志中通过。`/root/slice176_final_review` 独立复审 **PASS**，确认没有把人机交互误报为 Agent 自动操作。
- 限制：现场接受仍为 0/69；overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE8-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为登录窗口导航、对话滚动、视口关闭、相机/通信设置页入口、Agent 设置人工保存、单个 TCP Client 表单应用和快速测量互斥增加 8 项测试引用。所有既有 mapping 及描述保留。
- 边界：登录测试只验证打开/复用，不验证登录成功；工具设置页与 `COMM_APPLY` 仍是 Agent 不支持，相关测试仅验证 GUI 路径；Agent 设置是用户操作表单；快速测量测试不验证数值或 Agent 自动拾取。
- 验证：矩阵生成、`--check`、单测 6/6、`git diff --check` 通过；当前矩阵为 126 项中 87 `EVIDENCE_LINKED`、39 `NOT_LINKED`。相关 suites 在当前全量 CTest 日志通过（112/112）。`/root/slice176_final_review` 独立复审 **PASS**，确认没有将导航或表单测试扩大为 Agent 配置/硬件能力。
- 限制：真实登录认证、相机/通信设备、Agent provider、Windows/GPU 与现场接受不在此本地切片范围；整体 G8 仍 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE9-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：新增 4 项引用，覆盖 CropImage 高级配置草稿校验/提交、GUI 导入图像绑定到 GrabImage、聊天文本 composer 本地发送行为及附件上限反馈；operation identity 和 mapping 原值保留。
- 边界：GUI 拖图未覆盖 Agent 工具组合/确认；聊天发送仅是本地 composer 信号，未断言 AgentController/LLM 交付；附件只验证上限反馈，不代表图片随消息送达。结果页测试只验证结果表渲染，不足以链接 tab 导航，保持 `NOT_LINKED`。
- 验证：矩阵生成和 `--check`、单测 6/6、`git diff --check` 通过；126 项中 91 `EVIDENCE_LINKED`、35 `NOT_LINKED`。涉及 `test_mainwindow` 与 `test_agent_draft_restore`，当前全量 CTest 日志为 112/112。`/root/slice176_final_review` 独立审查 **PASS**，确认引用范围没有越过 GUI 本地行为。
- 限制：不证明 chat backend/LLM 或视觉输入成功；整体 G8 仍 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE10-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `SAM_EXPORT` 增加一个引用到注册的共享导出执行器测试；operation ID、工具映射和行为说明未改。
- 边界：LabelMe/YOLO-seg/COCO 内容和安全目标验证覆盖 `AnnotationSessionExport::execute`，AgentActor 生产分派也调用该执行器；测试直接调用执行器，不经过完整 AgentActor/Controller 确认或 GUI 导出按钮/文件选择器。
- 验证：矩阵生成/`--check`、单测 6/6、`git diff --check` 通过；126 项中 92 `EVIDENCE_LINKED`、34 `NOT_LINKED`。`test_agent_sam_tools` 与全量 112/112 CTest 均通过。`/root/slice176_final_review` 独立复审 **PASS**，确认测试覆盖格式和目标边界且没有误称端到端链路。
- 限制：不改变 G8 的 `NOT_READY` 状态。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE11-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 Inspector 结果表渲染和 SAM 环境状态/用户交接增加 2 项引用；operation mapping 不变。
- 边界：结果表渲染不证明 Agent 能读取模块输出；SAM 工具测试证明检查/打开引导，不安装环境、不启动服务或模型。
- 验证：当前矩阵生成、`--check`、单测 6/6 和 `git diff --check` 通过；126 项中 94 `EVIDENCE_LINKED`、32 `NOT_LINKED`。对应 suite 属于当前全量 112/112 CTest。`/root/slice176_final_review` 独立审查 **PASS**。
- 限制：不改变 G8 的 `NOT_READY` 状态。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE12-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：基于已注册测试中的额外独立断言，为数据源路径复制、底部面板折叠、参数撤销/重做、全局变量组合管理、用户手动 Agent 设置字段、Agent stop 工具、系统设置页及 Agent 日志页增加 11 项引用。复用 testId 的前提是同一个测试函数对各自 operation 有明确断言。
- 边界：GUI 参数 undo/redo、Agent 日志查看等 operation 的 `toolMapping` 仍为 unsupported；设置表单字段仍是人工交接；证据链接不提升能力映射。
- 验证：current-matrix 生成、`--check`、单测 6/6、`git diff --check` 通过；126 项中 105 `EVIDENCE_LINKED`、21 `NOT_LINKED`，其他 counts 不变。相关 suites 在全量 CTest 112/112 中通过。`/root/slice176_final_review` 独立复审 **PASS**，逐项核对共享 testId 的断言归属。
- 限制：剩余未链接项保留；overall G8 仍 `NOT_READY`，现场验收 0/69。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE13-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `CHAT_TOOL_CONFIRM` 增加现有预览卡实际拒绝/批准控件测试的引用；人机协作映射保持不变。
- 边界：只证明 `clear_runtime_logs` 预览卡样例中点击取消和确认的链路，不声称所有工具卡或所有过期/错配确认情形均由此测试覆盖。
- 验证：矩阵生成、`--check`、单测 6/6、`git diff --check` 通过；126 项中 106 `EVIDENCE_LINKED`、20 `NOT_LINKED`。`test_mainwindow` suite 与完整 CTest 112/112 通过。`/root/slice176_final_review` 独立复审 **PASS**。
- 当前未链接复核：最终逐项清点识别 15 项只有局部/弱断言、5 项缺少操作级测试或依赖外部条件；保持这些 operation 为 `NOT_LINKED`，未以不等价测试降低缺口计数。完整 operation list 和原因见当前生成矩阵。
- 限制：overall G8 仍 `NOT_READY`，现场验收 0/69。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE14-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `TOOLBOX_SEARCH` 新增真实 `ToolSearchEdit`/`ToolBoxTree` 操作测试，覆盖模块 ID 与去空格显示名过滤、匹配/非匹配项目及分类显隐、清空恢复。
- 边界：仅覆盖 `MainWindow::filterToolBox` GUI 行为；`get_available_plugins` 仍为工具组合映射，不声称 Agent 有等价过滤入口或满足阶段 5 工具契约。
- 验证：focused test `testToolBoxSearchFiltersItemsAndRestoresOnClear` 通过（3/3 QtTest 生命周期/用例检查）；current-matrix 生成、`--check`、单测 6/6、`git diff --check` 通过，`/root/slice176_final_review` 独立复审 **PASS**。矩阵 126 项中 107 `EVIDENCE_LINKED`、19 `NOT_LINKED`，mapping 分类不变。完整 CTest 112/112 为此前授权基线，本片测试后未重跑全量套件。
- 未关联复核：15 项仍只有局部/弱断言，4 项仍缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE23-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testFlowCanvasPortDragAddsProjectConnection` 经实际 FlowNodeItem scene press/move/release 事件完成显式兼容端口连接；MainWindow 路由后工程恰有一条带预期源/目标及端口 ID 的边。
- 边界：仅 GUI 创建连接；不覆盖 GUI 断开、隐式端口、Agent 权限/确认或完整 G4 验收；`DIRECT_TOOL` mapping 不变。
- 验证：`test_mainwindow` 定向构建成功，focused QtTest 3/3；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。矩阵 126 项中 116 `EVIDENCE_LINKED`、10 `NOT_LINKED`，mapping 分类不变；全量 CTest 112/112 是前序基线，本片未重跑全量套件。
- 独立复审：`/root/slice176_final_review` **PASS**；确认 MainWindow 路由写入唯一正确的工程连接，未扩大到 GUI 断开或完整 G4。
- 未关联复核：6 项仍只有局部/弱断言；4 项缺少操作级证据或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE15-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `VIEW_QUICK_MODE` 增加 `testQuickModeRunsCurrentFlowOnce`，通过开发者权限调用真实 `MainWindow::onQuickMode`，断言 RunOnce、启动/完成各一次且探针模块仅执行一次。
- 边界：是本地 GUI 快捷模式证据，不替代 Agent `run_flow` 工具契约、确认行为、硬件或插件业务接受；既有直接工具映射保留。
- 验证：focused test 通过（QtTest 3/3）；当前 matrix 生成、`--check`、单测 6/6，inventory 生成/`--check`、单测 12/12 与 `git diff --check` 通过。矩阵 126 项中 108 `EVIDENCE_LINKED`、18 `NOT_LINKED`；`/root/slice176_final_review` 独立复审 **PASS**。全量 CTest 112/112 为测试源更新前的记录基线，本片未重跑全量套件。
- 未关联复核：14 项仍只有局部/弱断言；4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE16-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：将 `TOOLBOX_DRAG_ADD` 关联到 `testG4SourceLessDropRequiresMatchingPayload`，该测试从真实工具箱模型取 GrabImage MIME，投递至流程树并断言事件接受、新模块加入且旧顺序不变。
- 边界：只覆盖合成 QDropEvent 的流程树目的地，不覆盖 OS 原生拖动手势或 FlowCanvas 目标；不声称 Agent 插入位置等价，`add_module` 仍末尾追加。
- 验证：focused test 通过（QtTest 3/3）；矩阵生成、`--check`、单测 6/6 与 inventory `--check` 通过；矩阵 126 项中 109 `EVIDENCE_LINKED`、17 `NOT_LINKED`，mapping 分类不变。`/root/slice176_final_review` 独立复审 **PASS**。全量 CTest 112/112 是测试源更新前的记录基线，本片未重跑全量套件。
- 未关联复核：13 项仍只有局部/弱断言；4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE17-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `FILE_OPEN` 增加 GUI 正向打开测试：临时工程经真实 `onOpenProject` 文件选择器选取后载入，且模块同步到流程树和画布。
- 边界：只覆盖正常 GUI 打开与视图同步，不声称覆盖损坏工程、全部未保存策略或 Agent `open_project` 工具契约。
- 验证：focused test 通过（QtTest 3/3）；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6 与 `git diff --check` 通过。矩阵 126 项中 110 `EVIDENCE_LINKED`、16 `NOT_LINKED`，mapping 分类不变；`/root/slice176_final_review` 独立复审 **PASS**。完整 CTest 112/112 是测试源更新前基线，本片未重跑全量套件。
- 未关联复核：12 项仍只有局部/弱断言，4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE18-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `PTREE_DBLCLICK_INSPECTOR` 增加真实流程树 viewport 双击测试，断言检查器定位到目标模块、可见并展示 Params 页。
- 边界：只证明 GUI 双击路线；Agent `select_flow_module` 及 pinned inspector 策略有独立测试，本链接不扩展其语义，也不表示工程发生修改或流程被运行。
- 验证：focused test 通过（QtTest 3/3）；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6 与 `git diff --check` 通过。矩阵 126 项中 111 `EVIDENCE_LINKED`、15 `NOT_LINKED`，mapping 分类不变；`/root/slice176_final_review` 独立复审 **PASS**。全量 CTest 112/112 是测试源更新前基线，本片未重跑全量套件。
- 未关联复核：11 项仍只有局部/弱断言；4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE19-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `SAM_OPEN_IMAGE` 增加真实 SAM 对话框打开图片按钮/文件选择器测试，确认用户选择后加载图片并写入当前会话图像路径和尺寸。
- 边界：仅证明人工文件选择 GUI 路径；Agent 不能选路径或自行载入图片，human handoff 映射不变；未证明推理后端、模型或预测质量。
- 验证：focused test 通过（QtTest 3/3；offscreen sandbox 记录 AF_NETLINK 权限警告但无用例失败）；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6 与 `git diff --check` 通过。矩阵 126 项中 112 `EVIDENCE_LINKED`、14 `NOT_LINKED`，mapping 分类不变；`/root/slice176_final_review` 独立复审 **PASS**。全量 CTest 112/112 是测试源更新前基线，本片未重跑全量套件。
- 未关联复核：10 项仍只有局部/弱断言；4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE20-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `SAM_MASK_OPS` 关联 `dilateGrowsMaskAndUndoRestores`：实际点击 GUI 膨胀按钮，断言掩膜面积增长、撤销精确恢复、重做重新应用。
- 边界：仅支持膨胀子操作的 UI 证据；腐蚀、橡皮擦、合并、拆分和 Agent 确认策略未由此链接覆盖，human handoff 映射不变。
- 验证：focused test 通过（QtTest 3/3；offscreen sandbox AF_NETLINK 权限警告，无断言失败）；current matrix 生成/`--check`/单测 6/6，inventory `--check` 和 `git diff --check` 通过。矩阵 126 项中 113 `EVIDENCE_LINKED`、13 `NOT_LINKED`，mapping 分类不变；`/root/slice176_final_review` 独立复审 **PASS**。全量 CTest 112/112 是测试源更新前基线，本片未重跑全量套件。
- 未关联复核：9 项仍只有局部/弱断言；4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE21-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `DS_IMPORT` 增加真实“导入图像”文件选择器用例，PNG 被登记为工程数据源、DataSourceTree 出现记录且主视图显示对应像素。
- 边界：只覆盖人工 PNG 路径；不覆盖 PLY/TIFF、Agent 两步确认/工具组合或 GrabImage 参数自动配置；mapping 保持 `TOOL_COMPOSITION`。
- 验证：focused test 通过（QtTest 3/3）；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6 与 `git diff --check` 通过。矩阵 126 项中 114 `EVIDENCE_LINKED`、12 `NOT_LINKED`，mapping 分类不变；`/root/slice176_final_review` 独立复审 **PASS**，确认只覆盖人工 PNG GUI 导入，没有扩大到 Agent 两步组合。全量 CTest 112/112 是测试源更新前基线，本片未重跑全量套件。
- 未关联复核：8 项仍只有局部/弱断言；4 项缺少操作级测试或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE22-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：为 `SAM_CATEGORY_MGMT` 增加真实类别输入框、添加按钮、类别列表选择和删除按钮用例；确认临时类别加入并删除。对象重标不在本切片覆盖范围。
- 边界：仅 GUI 类别增删；不覆盖对象标签重标、会话持久化、Agent 权限/确认或模型质量；mapping 保持 `HUMAN_HANDOFF`。
- 验证：`test_samannotatordialog` 定向构建成功，focused QtTest 3/3；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。矩阵 126 项中 115 `EVIDENCE_LINKED`、11 `NOT_LINKED`，mapping 分类不变；全量 CTest 112/112 是前序基线，本片未重跑全量套件。
- 独立复审：`/root/slice176_final_review` **PASS**；复审修复计划文档中的旧状态后，再次确认测试范围、矩阵计数和证据摘要一致。
- 未关联复核：7 项仍只有局部/弱断言；4 项缺少操作级证据或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE24-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testInspectorRerunButtonRunsWholeFlow` 选中第二个模块后点击检查器真实重新运行按钮，断言两个模块均按项目顺序各执行一次；矩阵备注同步记录按钮触发完整流程，Agent 仍无单模块重跑工具。
- 边界：GUI 行为事实，不增加 Agent 工具；`UNSUPPORTED` mapping 保持。`run_flow` 与重跑单个模块语义不等价。
- 验证：`test_mainwindow` 定向构建成功，focused QtTest 3/3；受限沙箱 AgentBridge 本地 socket 监听失败并回退也失败的警告保留在原始日志，测试断言通过；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。矩阵 126 项中 117 `EVIDENCE_LINKED`、9 `NOT_LINKED`，mapping 分类不变；全量 CTest 112/112 是前序基线，本片未重跑全量套件。
- 独立复审：`/root/slice176_final_review` **PASS**；确认按钮实际重跑完整流程，Agent 单模块重跑保持未支持。
- 未关联复核：5 项仍只有局部/弱断言；4 项缺少操作级证据或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE25-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testUserSubmitsLoginThroughDialogUi` 使用合成用户/密码，点击实际登录按钮，断言恰发出一条带两字段的 `loginRequested` 并发生本地 UserManager 用户名切换；测试结束恢复原用户。
- 边界：仅验证提交 UI plumbing；UserManager 当前接受任意密码，不证明密码正确性、真实身份验证、凭据存储或 Agent 获取/发送凭据；`HUMAN_HANDOFF` mapping 不变。
- 验证：`test_mainwindow` 定向构建成功，focused QtTest 3/3；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。矩阵 126 项中 118 `EVIDENCE_LINKED`、8 `NOT_LINKED`，mapping 分类不变；全量 CTest 112/112 是前序基线，本片未重跑全量套件。
- 独立复审：`/root/slice176_final_review` **PASS**；确认真实 GUI 提交链路与认证能力边界，未将本地模拟登录夸称密码认证。
- 未关联复核：4 项仍只有局部/弱断言；4 项缺少操作级证据或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE26-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testCommunicationViewAddsAndDeletesConfigThroughGui` 点击真实 Add/Apply 创建本地 TCP 配置，销毁初始视图后创建新视图并确认配置重载，再点击 Delete；断言管理器与表格都先创建、后删除。
- 边界：仅本地 GUI 配置 CRUD；不连接目标、不打开 socket、不测串口/PLC；Agent 配置工具仍未支持，mapping 不变。
- 验证：`test_communicationmanager` 定向构建成功，focused QtTest 3/3；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。矩阵 126 项中 119 `EVIDENCE_LINKED`、7 `NOT_LINKED`，mapping 分类不变；全量 CTest 112/112 是前序基线，本片未重跑全量套件。
- 独立复审：`/root/slice176_final_review` **PASS**；首轮指出必须创建新视图验证持久配置重载，修复后复审通过。
- 未关联复核：3 项仍只有局部/弱断言；3 项缺少操作级证据或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE27-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testStatusBarShowsRunDurationClockAndUserPlaceholder` 运行已注册测试模块，断言运行耗时标签更新、本地时钟可按界面格式解析，并精确记录用户标签仍为静态“用户：未登录”占位。
- 边界：只覆盖状态栏运行耗时/时间局部行为，不表示动态用户身份或 Agent 查询能力；`UNSUPPORTED` mapping 不变。日志保留受限 sandbox AgentBridge/fallback socket 与相机插件生命周期警告，断言通过。
- 验证：`test_mainwindow` 定向构建成功，focused QtTest 3/3；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。矩阵 126 项中 120 `EVIDENCE_LINKED`、6 `NOT_LINKED`，mapping 分类不变；全量 CTest 112/112 是前序基线，本片未重跑全量套件。
- 独立复审：`/root/slice176_final_review` **PASS**；确认只记录耗时/时钟局部显示和静态用户占位，未声称动态身份信息或 Agent 查询能力。
- 未关联复核：3 项仍只有局部/弱断言；3 项缺少操作级证据或依赖外部条件，均保持 `NOT_LINKED`。
- 当前边界：现场验收仍 0/69，overall G8 保持 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE28-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testConnectionButtonSendsLocalRequestAndReportsResponse` 点击真实 Agent 设置窗口连接测试按钮，由本机 loopback fixture 接收请求并返回合成响应；断言 POST 路径、Authorization、模型、token 数、用户消息、thinking 字段、成功提示与按钮恢复。
- 边界：仅 GUI 到本地合成 HTTP fixture 的请求/响应行为；不证明真实 provider、凭据有效性、Function Calling、视觉能力或生产网络，也不新增 Agent 工具；`HUMAN_HANDOFF` mapping 不变。
- 验证：定向 build 成功；新增用例 3/3 QtTest 计数通过，完整 `test_agent_settings_handoff` 套件 5/5 通过；inventory 生成/`--check`/单测 12/12，current matrix 生成/`--check`/单测 6/6，`git diff --check` 通过。详见 `agent-settings-handoff-current.log`、`inventory-tests.log`、`matrix-tests.log`、`inventory-check.log` 和 `matrix-check.log`。全量 CTest 112/112 是此前基线，本片未运行全量 CTest。
- 独立复审：`/root/slice176_final_review` **PASS**；确认矩阵计数、状态、测试边界、77 项源/证据哈希和 67 项二进制哈希一致，且全量 CTest 仅作为旧基线。
- 当前矩阵：126 项中 121 `EVIDENCE_LINKED`、5 `NOT_LINKED`；现场验收 0/69，overall G8 仍 `NOT_READY`。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE29-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`。
- 更新：`testCommunicationViewConnectsAndDisconnectsLoopbackTcp` 通过真实 Communication Settings 按钮建立并断开本机 TCP client；设置页现在订阅 manager 状态信号、显示 Connecting/Connected/Error/Disconnected、连接中允许断开，并在重载列表后保留选中配置。TCP connected 回调复制配置快照，避免异步回调持有可失效配置指针。
- 边界：只证明 127.0.0.1 上的人工 GUI TCP 生命周期；Agent 连接工具仍未支持，不证明外部网络、串口、PLC 或现场设备。`COMM_CONNECT_DISCONNECT` mapping 仍为 `UNSUPPORTED`。
- 验证：完整构建成功；focused QtTest 3/3，`test_communicationmanager` 全套 13/13；当前完整 CTest 112/112、零跳过；inventory auditor 13/13、current-matrix auditor 6/6，生成器/`--check` 与 `git diff --check` 通过。
- 独立复审：`/root/slice176_final_review` **PASS**；确认本地 TCP GUI 状态转移、按钮同步、配置快照生命周期及证据边界。
- 当前矩阵：126 项中 122 `EVIDENCE_LINKED`、4 `NOT_LINKED`；mapping 仍为 66/37/18/5，现场验收 0/69，overall G8 仍 `NOT_READY`。
- 最终 residual 复核：`/root/slice176_final_review` **PASS**；确认 `FILE_EXIT` 通过 `QWidget::close→closeEvent` 且有未保存保护，TERMINAL_INPUT/CAM_DELETE_CONFIG/CAM_APPLY_PARAMS 的未链接理由与源码一致。


### G8-LOCAL-OPERATION-EVIDENCE-SLICE30-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，独立审核 **CONTRACT_PASS**。
- 更新：新增独立 CTest 可执行文件 `test_fileexit_handoff`。`testFileExitMenuClosesMainWindowWithoutQuittingTestHost` 点击实际“文件→退出”菜单项，断言空闲相机 `shutdownCompleted` 恰好一次、queued close 前窗口仍可见、随后通过正常关闭路径隐藏，且 `aboutToQuit` 未发出；UserScope QSettings 重定向到临时目录，测试宿主 quit 策略用 RAII 恢复。`FILE_EXIT` 保持 `HUMAN_HANDOFF`，没有新增 Agent 退出工具。
- 验证：定向构建成功，focused CTest 1/1；完整构建退出码 0。受限 sandbox 全量 CTest 16 个 loopback/socket 依赖套件失败的原始日志保留在 `ctest-full-slice30.log`；允许本机 loopback 的复跑 `ctest-full-slice30-authorized.log` 为 113/113 通过、0 跳过。矩阵生成/`--check`、matrix auditor 6/6、plugin inventory auditor 13/13、`git diff --check` 通过。
- 边界：只证明用户菜单到主窗口 close 和空闲相机 shutdown gate；不覆盖活动相机关闭、未保存提示选项、进程终止或 Agent 控制宿主。
- 当前矩阵：126 项中 123 `EVIDENCE_LINKED`、3 `NOT_LINKED`；映射计数仍为 66/37/18/5，现场验收 0/69，overall G8 仍 `NOT_READY`。未关联项为人类专属 `TERMINAL_INPUT` 和当前实现不执行持久删除/设备写入的 `CAM_DELETE_CONFIG`、`CAM_APPLY_PARAMS`。
- 独立实现复审：`/root/slice176_final_review` **PASS**；确认独立进程隔离、真实菜单/closeEvent 链路、shutdown exactly-once、窗口关闭顺序、QSettings 临时隔离与退出策略恢复，且未夸大 Agent 或进程终止能力。

### G8-ENVIRONMENT-PREFLIGHT-2026-11-03

- 执行环境只读预检：Linux 6.8.0-138-generic x86_64；当前 checkout 的构建使用 Qt 5.15.3，`pkg-config` 未发现 Qt 6 Core；没有可访问的 `/dev/video*`、`/dev/ttyUSB*`、`/dev/ttyACM*`、`/dev/ttyS*` 或 `/dev/nvidia*` 设备节点。`nvidia-smi` 可执行文件存在，但当前容器未暴露 NVIDIA 设备节点。
- 原始记录：`build-codex-agent/agent-phase8-current-matrix-evidence/g8-environment-preflight-20261103.txt`。
- 定性：仅为环境可用性预检，不是相机/串口/GPU/Qt 6/provider/现场验收；相关外部门槛继续 `NOT_RUN`。未读取 provider 凭据，也未访问外部服务。

### G8-CURRENT-REGRESSION-MATRIX-REFRESH-SLICE52-2026-11-03

- 触发：Slice201 加强 `test_mainwindow` 断言后，重新验证完整当前树回归并刷新 G8 矩阵。
- 验证：`cmake --build build-codex-agent -j2` 退出码 0；获准本机 loopback `ctest --test-dir build-codex-agent --output-on-failure` **113/113 通过、0 失败、0 跳过**。G8 current matrix 使用该轮标准原始 CTest 日志重新生成，生成器 `--check`、candidate inventory `--check` 和两套 auditor tests 20/20 通过。
- 矩阵：69 插件、67 当前二进制、66 业务 fixture PASS、DefectDetection 业务 fixture FAIL（Hymson SDK 缺失）、DirectShow/Hikvision 各受平台/SDK 阻断；126 项 GUI 操作中 125 `EVIDENCE_LINKED`、1 `NOT_LINKED`（人类专用终端输入）；现场 0/69，overall `NOT_READY`。
- 原始构建/CTest、退出码、矩阵刷新及哈希见 `build-codex-agent/agent-phase8-slice52-evidence/`。本地全量回归不升级任何真实 provider、设备、GPU、Qt 6/Windows 或现场验收状态。



### G8-TERMINAL-HUMAN-HANDOFF-EVIDENCE-SLICE53-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 更新：`TERMINAL_INPUT` 链接到 `testBuiltinDocsStaySourcedAndAuditArtifactsStayExcluded` (`tests/test_agentphase0_repro.cpp`)。该用例存在于 Slice52 全量 CTest 通过的 `test_agentphase0_repro` suite；只证明 shell/命令执行能力不在 Agent 工具白名单内，且 terminal tab 导航不能充当执行入口。未启动用户 shell。
- 判定：操作仍为 `HUMAN_HANDOFF`、`currentTool: null`；该行 `EVIDENCE_LINKED` 不代表 Agent 可操作终端。矩阵 126/126 linked、mapping 分类不变、现场 0/69、overall `NOT_READY`。
- 验证：current matrix 使用 Slice52 原始全量日志重新生成；生成器 `--check`、inventory `--check`、两套 auditor tests 20/20、`git diff --check` 通过。终审另见 Slice53 evidence summary/hash。

### G8-QT-SELECTION-MATRIX-SLICE54-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 更新：正式矩阵新增 `currentBuild.qtSelection`，把 CMake requested major 与当前构建实际链接 major 分离。Qt 版本优先从 `test_mainwindow` 生成 link line 推导，仅当该文件不可用时使用明确的 Qt core package cache；link evidence 缺失/冲突时保留 unknown，不猜测。
- 当前实证：`USE_QT6=ON` 请求 Qt6，但 link line 指向 Qt 5.15.3，矩阵正确输出 requested 6 / selected 5 / fallback true；不代表 Qt6 验收通过。
- 验证：矩阵由 Slice52 全量 CTest 原始日志重生成；无参数 matrix `--check`、inventory `--check`、矩阵和 inventory auditor tests **22/22**、Python 编译、JSON 校验与 `git diff --check` 通过。插件、操作、现场计数不变；G8 仍 `NOT_READY`。

### G8-DOCUMENTATION-SYNC-AUDIT-SLICE31-2026-11-03

- 契约：`docs/baseline/agent-phase-8-documentation-sync-audit-contract.md`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 审计结果：69/69 插件 metadata 与插件手册链接一一对应，无漏项、额外项或重复项；逐条文章索引在 `documentation-plugin-audit.tsv`。QRC 与 AgentActor 运行时发现逻辑限定在三份内置 Markdown 和用户知识目录；新增测试断言三份来源标注、临时目录不枚举审计材料及工具目录无 shell 工具。三份 DefectDetection 文档与 `ENABLE_HYMSON3D=OFF`、`HymsonVision3D_FOUND AND FALSE` 的双重禁用及当前失败/未产生结果语义一致。
- 维护：当前测试源码增加用例后，G8 候选库存与 CMake 登记不同步；已根据既有审核 fixture CTest 日志重生成。仅候选用例索引从 1420 增至 1421，关联行 9 条；69 项 fixture 状态无变化。新增用例提及 DefectDetection 仅形成候选索引，不构成插件业务证据，其 fixture 仍为 FAIL。
- 验证：三套 focused CTest 3/3；完整构建成功、授权 loopback 全量 CTest 113/113，0 失败/跳过；current matrix/inventory `--check`、对应 auditor 单测及 `clang-format`、`git diff --check` 通过。详见 `docs/baseline/agent-phase-8-documentation-sync-audit-record.md` 与 `agent-phase8-current-matrix-evidence/`。
- 状态：**PASS（G8 criterion 5 文档/运行时资源同步子项）**；Slice32–36 修正并关闭了发现，69 项插件文章逐行独立 PASS。计划标准 5 已勾选；G8 其他门槛、现场验收和 overall 状态仍独立保持未通过/`NOT_READY`。

### G8-DOCUMENTATION-SYNC-MARKER-CORRECTION-SLICE32-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 修改：仅将 `docs/user-manual.md` 第 5 行的过期适用基线标记改为“内容核对日期：2026-11-03（DeepLux_codex 工作区快照，不代表产品发布版本）”，保留现场样本、设备、模型和性能验收提示。
- 验证：重新构建 `test_toolschema`、`test_knowledgebasedialog`、`test_agentphase0_repro`（QRC 资源已重新编译，退出码 0）；在 loopback 可用环境 focused CTest 3/3；文本标记检查和 `git diff --check` 通过。日志见 `build-codex-agent/agent-phase8-current-matrix-evidence/slice32-focused-build.log` 与 `slice32-focused-ctest.log`。
- 状态：日期修正纳入 Slice36 最终独立复核，文档/运行时资源同步子项 PASS；G8 其他门槛未受影响。

### G8-DOCUMENTATION-ACCURACY-SLICE33-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 修正：TCPServer 文档补充 `bindAddress=Any`（默认、所有网络接口）与 `LocalHost`（仅本机）及部署范围说明；运行时知识文档测试精确断言隔离目录仅有三份 builtin ID，并扫描全工具目录命令/脚本/进程执行表面。
- 语义审计资产：69 行 crosswalk 按 metadata ID 唯一，记录文章声明、metadata 路径、具体实现符号、平台/SDK 评估；当前均为 `NOT_REVIEWED`，等待独立逐项核验，不得当作 PASS。
- 验证：QRC 相关目标构建成功；loopback 可用环境 focused CTest 3/3；current-matrix 与插件库存 `--check` 通过，审计器单测 19/19；69 行 crosswalk 结构/源码符号校验通过；哈希、格式和 `git diff --check` 通过。证据日志见 `build-codex-agent/agent-phase8-current-matrix-evidence/slice33-*`。
- 状态：69/69 插件文章语义已由 Slice36 最终独立审核确认并登记 crosswalk `PASS`；文档/运行时资源同步子项最终 PASS，overall G8 仍 `NOT_READY`。

### G8-DOCUMENTATION-EVIDENCE-CLOSURE-SLICE34-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 修正：GrabImage 文档补充文件夹按名排序取图、`folderLoop` 默认开启及末尾行为；完整工具目录扫描纳入工具名；交叉表 source refs 改为具体非构造函数配置/执行符号。因用户手册未跟踪且无 Git 前版本，另生成 Slice32 单行标记的重建前态及 diff，并明确其非同期快照。
- 验证：QRC focused build 成功、CTest 3/3；matrix/inventory checks 通过、审计器单测 19/19；69 行 crosswalk 结构和符号、GrabImage 源语义、重建手册 diff、格式和 `git diff --check` 均通过。最终 SHA 校验在文档记录更新后执行。
- 状态：Slice36 最终复核确认 69/69 插件文章及修复内容 PASS；文档/运行时资源同步子项 PASS，overall G8 仍 `NOT_READY`。

### G8-DOCUMENTATION-SHELL-GUARD-SLICE35-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 修改：完整注册工具名/说明/参数检测加入明确的名字归一化规则和 payload 规则；三个未注册危险工具合成例必须判为可执行，UI-only `select_log_panel_tab` 保持允许，其加 payload 或启动进程描述的副本必须拒绝。
- 验证：QRC focused build 成功、loopback 可用 CTest 3/3、matrix/inventory `--check` 通过、审计器单测 19/19、格式与 `git diff --check` 通过。证据日志见 `build-codex-agent/agent-phase8-current-matrix-evidence/slice35-*`。
- 状态：Slice36 独立实现复审确认执行能力回归断言 PASS；文档/运行时资源同步子项 PASS，overall G8 仍 `NOT_READY`。

### G8-DOCUMENTATION-SHELL-DESCRIPTION-GUARD-SLICE36-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 修改：对有可执行 payload 的工具，从归一化名称、工具描述和每项参数描述检查执行动词；增加两个无执行型名称的 `value_summary(code)` 合成测试，分别在工具描述和参数描述中包含运行代码的动词。原三种工具名、terminal-tab 的允许与变体拒绝用例保留。
- 验证：QRC focused build 成功、loopback 可用 CTest 3/3、matrix/inventory `--check` 通过、审计器单测 19/19、格式与 `git diff --check` 通过。证据日志见 `build-codex-agent/agent-phase8-current-matrix-evidence/slice36-*`。
- 状态：`/root/slice176_final_review` **IMPLEMENTATION_PASS**；Slice36 文档子项最终 SHA 清单 140 项通过。全量回归证据刷新后，151 项 SHA 清单通过（`slice36-final-archive-sha256.log`）；文档/运行时资源同步子项 PASS。G8 其他门槛未完成，overall 仍 `NOT_READY`。

### G8-FULL-SOFTWARE-REGRESSION-SLICE36-2026-11-03

- 全量构建：`cmake --build build-codex-agent -j2`，退出码 0，所有 targets 构建完成。
- 全量测试：`ctest --test-dir build-codex-agent --output-on-failure -j2`，113/113 通过，0 失败、0 跳过；完整结果位于 `slice36-full-ctest.log`。
- 当前验收矩阵已重生成并引用该 CTest 日志（SHA 匹配），矩阵检查使用同一 `--ctest-log` 参数通过；matrix overall 仍 `NOT_READY`。
- 复现命令、退出码和数量：`slice36-full-regression-summary.txt`。矩阵/库存检查与 19 项 auditor tests、定向 clang-format、diff 检查均通过。
- 状态：`/root/slice176_final_review` 独立 PASS；计划 G8 标准 3 已勾选。证据仅覆盖当前 Linux/Qt 5.15.3 工作区，跨平台和现场仍分开验收。

### G8-CAMERA-GUI-UNSUPPORTED-BOUNDARY-SLICE37-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- GUI 测试：`testCameraSetViewDeleteAndApplyRemainGuiOnly` 通过 fake async camera 驱动真实 Delete/Apply 按钮，断言 Delete 仅移除当前表格行、Apply 仅接受对话框，CameraManager 仍保留设备且没有设备连接/断开/采集/参数写入调用。
- 边界：该 QtTest 不代表 Agent 功能、持久化或相机验收；`CAM_DELETE_CONFIG` 与 `CAM_APPLY_PARAMS` 继续 `NOT_LINKED`，`TERMINAL_INPUT` 继续人类专用。
- 验证：focused `test_cameramanager` 1/1；全量构建退出 0、CTest 113/113；候选库存刷新后 QtTest 候选索引 1421→1422，fixture 审核结果不变（66 业务 PASS、DefectDetection 1 FAIL、67 关键失败 PASS、2 平台/SDK camera NOT_AUDITED）；matrix/inventory checks、19 项 auditor tests、格式/diff/SHA 均通过。证据位于 `build-codex-agent/agent-phase8-current-matrix-evidence/slice37-*`。
- 状态：Slice38 修复完整状态对照后，`/root/slice176_final_review` 对 Slice37/38 组合测试 **IMPLEMENTATION_PASS**；G8 标准 1/2 仍未通过，overall G8 为 `NOT_READY`。

### G8-CAMERA-GUI-STATE-SNAPSHOT-SLICE38-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**。
- 修改：Slice37 GUI-only 测试在 Delete 和 Apply 后比较完整 `CameraStatus`（deviceId/name/state/lastGrabTime/lastError），并断言 CameraManager 仍注册同一 fake plugin。
- 验证：focused `test_cameramanager` 1/1；全量构建成功、CTest 113/113 通过；current matrix 指向最新日志且仍 `NOT_READY`。候选库存索引和 69 项 fixture 状态已复核，fixture 结果无变化；matrix/inventory checks 与 19 项 auditor tests、格式/diff/SHA 通过。
- 边界/状态：`CAM_DELETE_CONFIG`、`CAM_APPLY_PARAMS` 仍 `NOT_LINKED`；证据只证明 GUI handler 对 fake camera 的效果，不能代表 Agent 或真实设备能力。Slice38 状态快照加固后经 `/root/slice176_final_review` **IMPLEMENTATION_PASS**。

### G8-IMAGESCRIPT-PUBLIC-MODES-SLICE39-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/slice176_final_review` **CONTRACT_PASS**；固定灰度、模糊、锐化像素 oracle 已独立对照当前 OpenCV 实现。
- 更新：扩展现有 `testAgentImageScriptInvertAndRejectsInvalidConfigurationAndMissingImage`；Agent 对 ImageScript 四个公开 `scriptType`（0–3）逐一配置并运行，按各自 `runId` 读取 `script_executed`、`script_type` 与完整输出矩阵。模式 1–3 使用固定 9×9 BGR 图像，分别断言灰度 103、5×5 box blur 中心支撑区 84/背景 80、sharpen 中心 180/四邻域 60/其余 80；每种模式同时比较来源图与输出图的 `sourceImageId`、`sourceSize`、`localToSourceMatrix`。模式 0、非法参数零执行、缺图运行失败与失效输出断言保留。
- 验证：定向 CTest 1/1；`cmake --build build-codex-agent -j2` 退出码 0；本地 loopback 受限运行因测试 listener 被沙箱拒绝而失败的原始日志保留为 `slice39-sandbox-restricted-ctest.log`，随后在获准的本机 loopback 测试环境全量 CTest 113/113 通过、0 失败/跳过（`slice39-full-ctest.log`）。候选库存和当前矩阵均引用该全量日志并通过 `--check`；19 项 auditor tests、JSON、格式和 `git diff --check` 通过。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**；最终归档 **FINAL_ARCHIVE_PASS**。ImageScript fixture assertion 已更新为全 4 模式，并与本次全量 CTest 证据绑定。共享 `summary.md` 已更正为 Slice39 最新/Slice30 历史；刷新后的 26 项 SHA 清单全部通过。业务 fixture 数量及其它插件判定未变。
- 边界/状态：本证据仅覆盖 Linux/Qt 5.15.3 下 ImageScript 四种内置模式的软件链路，不覆盖任意图像质量、其他插件的多模式、跨平台、现场验收或模型/provider。矩阵仍为 69 插件/126 操作、123 `EVIDENCE_LINKED`/3 `NOT_LINKED`、现场验收 0/69，overall G8 继续 `NOT_READY`；G8 多模式门槛仍需覆盖 Math 等其他多模式插件。

### G8-MATH-PUBLIC-OPERATIONS-SLICE40-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。Modulo oracle 经审核修正为非零的 `14 % 5 = 4`，避免错误 fallback 0 假通过。
- 更新：扩展 `testAgentConfigurationCalculatesAndRejectsDivisionByZero`；Agent 对 Add(12,3)=15、Subtract(12,3)=9、Multiply(12,3)=36、Divide(12,3)=4、Modulo(14,5)=4、Power(12,3)=1728、Min(12,3)=3、Max(12,3)=12 分别 set params、执行 flow、按各自 `runId` 读取数值。Divide 与 Modulo 的零除各自失败于 Math 实例，诊断精确为 `除数不能为0`，失败 run 输出为 `unavailable/run_failed`。
- 验证：focused `test_agent_core_plugins_g8` 1/1；完整构建退出码 0；授权本机 loopback 的全量 CTest 113/113，0 失败/跳过。69 项 fixture evidencePath 已更新为 `slice40-full-ctest.log`，候选库存/当前矩阵均由此日志重生成；矩阵维持 69 插件、126 操作、123 `EVIDENCE_LINKED`/3 `NOT_LINKED`、现场验收 0/69、overall `NOT_READY`。库存与矩阵 `--check`、19 项 auditor tests、JSON、格式、diff 和 SHA 清单均通过。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**；最终归档 **FINAL_ARCHIVE_PASS**。34 项 SHA 清单与校验通过，Slice40 证据路径和矩阵状态已独立复核。
- 边界/状态：只覆盖 MathPlugin 的 8 个内置运算及 Divide/Modulo 零除失败，不覆盖其他插件的模式。G8 的多模式门槛仍未完成，整体状态不变为 PASS。


### G8-PERPROCESSING-PUBLIC-MODES-SLICE41-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。独立计算确认 9×9 合成 BGR 输入、bilateral sigma 参数映射、11 个 OpenCV 参考输出形状/通道均正确且全部互异、均非 identity。
- 更新：扩展 `testAgentPerProcessingGaussianBlurAndRejectsUnsupportedTypeAndMissingImage`；Agent 对 GaussianBlur、MedianBlur、BilateralFilter、Sobel、Laplacian、Canny、Dilate、Erode、Open、Close、Gradient 十一种 mode 分别设置参数、运行、按各自 `runId` 读取 `processed_type` 和尺寸，并对比完整像素矩阵、通道数及 GrabImage 同 run provenance。保留 GaussianBlur 原有断言、numeric 边界和 unsupported mode 零执行/不变更、缺图失败与输出失效断言。
- 验证：focused `test_agent_visual_plugins_g8` 1/1；完整构建退出码 0；授权本机 loopback 的全量 CTest 113/113、0 失败/跳过（`slice41-full-ctest.log`）。67 项 fixture evidencePath 已绑定该日志；inventory/current matrix 重生成，生成器默认与 `--check` 均指向当前日志；19 项 auditor tests、JSON、Python 编译、格式、diff 通过；40 项 SHA-256 清单当前逐项通过。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**，evidencePath finding 已关闭；最终归档 **FINAL_ARCHIVE_PASS**。40 项 SHA 清单经独立终审全部通过。
- 当前状态：矩阵仍为 69 插件/126 操作，123 `EVIDENCE_LINKED`/3 `NOT_LINKED`，现场验收 0/69，overall `NOT_READY`。PerProcessing 的 11 个本地 OpenCV mode 有软件 fixture；Condition/While/ColorRecognition/GrabImage 及其他离散模式和外部现场门槛仍未全部完成。

### G8-COLORRECOGNITION-PUBLIC-COLORS-SLICE44-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。独立核对红色 H0/H175、绿色 H60、蓝色 H120、黄色 H30、橙色独占 H12、紫色 H150；七个 fixture 均命中预期范围，面积与中心断言合理。
- 更新：扩展 `testAgentRecognizesConfiguredColorAndReportsAbsentTarget`；Agent 使用 GrabImage Path 加载七张生成图，覆盖全部六种 UI 颜色和红色高色相 wraparound。每个 `runId` 检查 source_path、color_name、color_count=1、area>500、center `(34±1,34±1)`。白图未检出失败归因与诊断保留。
- 验证：全量构建退出码 0；定向 QtTest slot 3/3；focused visual-plugin CTest 1/1；授权本机 loopback 全量 CTest 113/113、0 失败/跳过（`slice44-full-ctest.log`）。67 项 fixture evidencePath 已绑定本轮日志；inventory/current matrix 重生成及 `--check`、19 项 auditor tests、JSON、Python 编译、格式与 `git diff --check` 通过。矩阵仍为 69 插件、126 操作、123 `EVIDENCE_LINKED`/3 `NOT_LINKED`、现场 0/69、overall `NOT_READY`。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**；最终归档 **FINAL_ARCHIVE_PASS**。81 项 SHA 清单与校验日志经终审全部通过。
- 边界：仅为 Linux/Qt 合成图像上的颜色分支软件证据；不代表现场照明、颜色标定、真实相机或 G8 整体验收。

### G8-BLOB-THRESHOLD-MODES-SLICE45-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。OpenCV 独立计算验证 Otsu 低对比圆、Fixed 梯度图与 Adaptive 梯度图的阈值/轮廓 oracle；Adaptive case 显式设 `fixedThreshold=90`，错误 Fixed fallback 无合格小圆。
- 更新：扩展 `testAgentDetectsConfiguredBlobAndReportsEmptyDetection`；Agent 对 Otsu、Fixed、Adaptive 逐一配置 GrabImage/Blob 参数并运行，按同一 runId 检查 source_path、blob_count、面积与中心；保留空图未检出失败实例归因和精确诊断。
- 验证：全量构建退出码 0；focused visual-plugin CTest 1/1；授权本机 loopback 全量 CTest 113/113、0 失败/跳过（`slice45-full-ctest.log`）。67 项 fixture evidencePath 已绑定本轮日志；inventory/current matrix 重生成及 `--check`、19 项 auditor tests、JSON、Python 编译、格式与 `git diff --check` 通过。矩阵仍为 69 插件、126 操作、123 `EVIDENCE_LINKED`/3 `NOT_LINKED`、现场 0/69、overall `NOT_READY`。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**；最终归档审核 `/root/slice176_final_review` **FINAL_ARCHIVE_PASS**；94 项 SHA、67 条 evidencePath、113/113 全量 CTest 与矩阵/审计检查均复核通过。
- 边界：仅证明本机合成图像下 Blob 三种阈值模式的软件行为，不证明噪声/照明现场鲁棒性或 G8 整体验收。

### G8-GRABIMAGE-DEMO-FOLDER-SLICE46-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。审核修正了两项运行语义：连续 `run_flow` 重建模块会重置 Folder 游标，Folder 路径须由 Agent 显式配置 `grabSource=Path`（UI `setsParam` 联动不会由工具调用触发）。最终契约以同一模块 runtime 的 `step_once` 验证连续取图，并先读 `effectiveParams` 核实配置。
- 更新：`testAgentLoadsGrabImagePathAndReportsMissingFile` 扩展并更名为 `testAgentLoadsGrabImagePathDemoAndFolderModes`。新增 Demo 640x480 棋盘像素 same-run 断言；Folder 用 `a.png`/`b.png` 与忽略的 `.txt` 输入，检查同 runtime 连续 step 的排序、source 元数据、位置/数量、尺寸和像素；第三步确认 `STEP_FAILED`、同 run 失败实例及耗尽诊断。原 Path 成功/缺文件失败断言保留。
- 验证：全量构建退出 0；新增 QtTest slot 3/3（含 init/cleanup）；focused visual-plugin CTest 1/1；授权本机 loopback 全量 CTest 113/113、0 失败/跳过。Inventory 与 matrix 重生成、`--check`、19 项审计测试、JSON/Python、格式和 `git diff --check` 均通过。矩阵仍 69 插件、126 操作、123 linked/3 not linked、现场 0/69、overall `NOT_READY`。
- 独立审核：契约 **CONTRACT_PASS**；`/root/slice176_final_review` **IMPLEMENTATION_PASS**（含失败 run 输出 `unavailable/run_failed` 边界）、**FINAL_ARCHIVE_PASS**。109 项 SHA、67 条 fixture evidencePath、113/113 全量 CTest 与矩阵/审计检查均复核通过。
- 边界：证明的是本机合成输入下 Agent 可调用的 Demo/Folder 软件路径；不代表真实相机采集或现场验收。

### G8-GRABIMAGE-AGENT-CAMERA-FRAME-SLICE47-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。契约要求经 `RunEngine::runOutputSnapshot` 对 Agent 成功 `captureRunId` 的原始 4x3 图像逐像素断言，不使用缩放预览；oracle 能区分启动时延迟旧帧和软件触发帧。
- 更新：扩展现有 `testAgentAsyncSessionFeedsGrabImage`，Autopilot 确认和异步 fake camera 链路不变；检查 12 个像素都为触发帧 RGB `(20+n,80+n,160+n)`，并排除 prior RGB `(1,2,3)`。GrabImage 审计行保留主 G8 Path/Demo/Folder 测试，增加支持 test ID；inventory generator 逐 ID 校验其 CTest 通过、QtTest 用例存在且源码引用 GrabImage，并将 supporting IDs 独立输出。
- 验证：全量构建退出 0；camera lifecycle 与 visual-plugin focused CTest 2/2；授权本机 loopback 全量 CTest 113/113、0 失败/跳过。Inventory/current matrix 重生成、`--check`、20 项审计测试、JSON/Python、格式和 `git diff --check` 均通过。67 条 fixture evidencePath 均绑定 Slice47 全量日志。矩阵保持 69 插件、126 操作、123 linked/3 not linked、现场 0/69、overall `NOT_READY`。
- 独立审核：契约 **CONTRACT_PASS**；`/root/slice176_final_review` **IMPLEMENTATION_PASS**：12 个原始像素均匹配本次触发帧，延迟旧帧被排除；supporting test ID 解析与输出通过复核。最终归档 **FINAL_ARCHIVE_PASS**；124 项 SHA、67 条 fixture evidencePath、113/113 全量 CTest 与矩阵/审计检查均通过。
- 边界：只证明 Linux 软件 fake camera 捕获行为，不代表实体相机、SDK或现场验证。

### G8-WHILE-PUBLIC-COMPARATORS-SLICE43-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。审核补上 Empty 防假通过断言：CreateString 的同 run 输出变量必须可用且确实为空；flow state 必须包含明确的 image data edge 与四条 control edge。
- 更新：扩展 `testAgentStopWhileExitsWhileBodyAndContinuesAfterFlow`，加入真实 CreateString→While image 数据边；NotEmpty、Empty、Equal、NotEqual、GreaterThan、LessThan 六种比较各有 true oracle。每个 runId 验证 source 值 available 且等于配置值、`while_result=true`、`stop_while_requested=true`，并断言 body/StopWhile/after 各执行一次；Empty 另验 `string_length=0`。旧 false-condition、参数边界/非法枚举无副作用测试保留。
- 验证：全量构建退出码 0；focused visual-plugin CTest 1/1；授权本机 loopback 全量 CTest 113/113、0 失败/跳过（`slice43-full-ctest.log`）。67 项 fixture evidencePath 已绑定本轮日志；inventory/current matrix 重生成及 `--check`、19 项 auditor tests、JSON、Python 编译、格式与 `git diff --check` 通过。矩阵仍为 69 插件、126 操作、123 `EVIDENCE_LINKED`/3 `NOT_LINKED`、现场 0/69、overall `NOT_READY`。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**；最终归档 **FINAL_ARCHIVE_PASS**。68 项 SHA 清单与校验日志经终审全部通过。
- 边界：仅覆盖 While 六种 comparator 的真值路径与 StopWhile 首次 body 退出的软件行为；不声称无界循环安全、取消竞态、现场设备或 G8 整体验收。

### G8-CONDITION-PUBLIC-COMPARATORS-SLICE42-2026-11-03

- 契约：`本地逐切片契约（未纳入提交）`，`/root/defect_detection_g8_review` **CONTRACT_PASS**。9 种 operator 均对照 ConditionPlugin 和真实 CreateString 输入链路核验；`42 > 9` 与 `2 < 10` 可区分数值比较和字典序，空字符串以真实 image 端口传递。
- 更新：扩展 `testAgentEvaluatesConditionFromUpstreamAndReportsMissingVariable`；Agent 对 NotEmpty、Empty、Equal、NotEqual、Contains、GreaterThan、LessThan、StartsWith、EndsWith 分别设置参数、执行流程并按本次 `runId` 读取 Condition 的 `condition_result`。保留 Equal false 分支与空变量失败归因、精确诊断。
- 验证：全量构建退出码 0；定向 QtTest slot 3/3（含 init/cleanup）；受限沙箱下完整 core-plugin CTest 因 Modbus localhost listener 无权限绑定而不作为验收结果；授权本机 loopback 全量 CTest 113/113、0 失败/跳过（`slice42-full-ctest.log`）。67 项 fixture evidencePath 已绑定本次全量日志；inventory/current matrix 生成及 `--check`、19 项 auditor tests、JSON、Python 编译、格式与 `git diff --check` 通过。矩阵仍为 69 插件、126 操作、123 `EVIDENCE_LINKED`/3 `NOT_LINKED`、现场 0/69、overall `NOT_READY`。
- 独立审核：契约 **CONTRACT_PASS**；实现 **IMPLEMENTATION_PASS**；最终归档 **FINAL_ARCHIVE_PASS**。53 项 SHA 清单及校验日志经终审全部通过。
- 边界：只证明 Condition 的九个 comparator 在 Linux/Qt 5.15.3 软件 fixture 中经 Agent 数据流可配置并运行；不证明 malformed numeric、其他插件、现场设备、provider 或 G8 整体验收。
