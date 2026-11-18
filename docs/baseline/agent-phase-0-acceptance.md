# 阶段 0 验收记录：基线、能力矩阵与复现样例（整改轮 3）

> 历史验收快照：本文状态与结果对应其记录日期和基线；当前项目状态见 [docs/STATUS.md](../STATUS.md)。

依据 `docs/agent-capability-improvement-plan.md`(v1.0, 2026-09-28)§5/§14.2 编制。
**状态:PASS**(第 3 次送审通过,2026-09-29;审核结论由独立审核者给出并归档于下方模板头。轮 1 FAIL→整改见 §11,轮 2 FAIL(R6/R7/R8)→整改见 §12。PASS 仅表示 G0 基线完整可复现,**不表示 F01–F13 已修复**)。

```text
阶段：0（基线、能力矩阵与复现样例）——整改轮 3
实现负责人：Claude Code（外部实现 Agent）
审核负责人：待指派（独立审核 Agent / 维护者；轮 1 提出 5 项整改,轮 2 确认其中 4 项并提出 3 项新整改）
开始 revision / 工作区摘要：b516f54da28b63623ef6b293b71a4afa3b84fad0 @ agent/development；
  脏树（他人在先修改，全部保留未动）：README.md、docs/plugins.md、src/core/agent/AgentActor.cpp、
  src/core/agent/agent_resources.qrc、src/core/agent/knowledge/DEEPLUX.md、tests/test_agentactor.cpp；
  未跟踪：docs/user-manual.md、docs/agent-capability-improvement-plan.md、.claude/（按 §3.1.2 不纳入提交）
结束 revision / 工作区摘要：仍为 b516f54（阶段 0 未提交，等待提交授权；§3.1.5 交付限定文件清单见 §2）；
  阶段 0 全部交付为审计与测试资产，src/ 产品代码零改动；整改轮工作区快照与摘要见
  evidence/workspace-baseline-r2.txt
覆盖 F 编号与操作编号：F01–F13 全部建立复现（自动化 16 用例 + 文档化 2 项）；
  GUI 操作 109 项全部编号并指派责任阶段
构建命令 / 退出码：cmake --build build-codex-agent -j2 → 0（build-baseline.log）
测试命令 / 实际用例数 / 退出码：env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent
  --output-on-failure → 78/78 套件通过（0 失败 0 跳过），CTEST_EXIT=0；
  复现套件单跑 16/16 用例通过，REPRO_EXIT=0
原始证据位置与 SHA-256：build-codex-agent/agent-phase0-evidence/（清单 sha256sums.txt，逐项见 §4）
现场依赖与未验证项：真实相机/PLC/串口/SAM 权重/Windows 平台/真实 LLM 均未验证（属阶段 5/8）；
  全量 CTest 需允许本机回环监听的环境（4 个监听类套件，见 §9）
回退方式：删除 §2 所列阶段 0 新增文件并还原 tests/CMakeLists.txt 的注册 diff 即可；
  不影响产品代码、他人在先修改与既有测试；原始日志（*.v1.log）与整改后日志并存，均未销毁
审核结论：PASS（独立审核者第 3 轮结论，由实现 Agent 于 2026-09-29 归档原文要点）
审核依据、日期和后续阶段放行范围：
  依据（审核者原文要点）：R6 两项映射已改“未支持”，109 个操作 ID 唯一，矩阵统计直接工具 13/未支持 68；
  R7 的 15/15 项证据哈希、70/70 项二进制哈希校验通过，483 个矩阵证据引用有效；R8 已将受限环境失败项
  更正为 test_agentbridge 并明确 test_communicationmanager 通过；定向复跑 F02、F06 控制器路径、F08、
  F13 控制器路径退出码 0；本轮未重跑全量 CTest（轮 2 已独立确认允许监听环境下 78/78、退出码 0，
  本轮产品代码与测试断言未变）。审核者未修改文件、未提交、未推送。
  日期：2026-09-29。
  放行范围：G0 基线达到可复现、可追溯要求，可按计划进入阶段 1；F01–F13 均未修复；
  阶段 0 资产的提交与推送仍需维护者单独授权。
```

## 1. 基线环境

| 项 | 值 | 核验方式 |
| --- | --- | --- |
| 工作区 | `/home/charles/Data/Repo/DeepLux_codex`(未触碰同级 `DeepLux`) | `pwd`;§3.1.1 |
| 构建目录归属 | `CMAKE_HOME_DIRECTORY=/home/charles/Data/Repo/DeepLux_codex` | `build-codex-agent/CMakeCache.txt`;§14.1 |
| HEAD / 分支 | `b516f54…` @ `agent/development` | `git rev-parse HEAD` |
| 构建类型 / 编译器 | Release / g++ 11.4.0 (Ubuntu 22.04) | CMakeCache / `c++ --version` |
| Qt(构建期) | 系统 Qt5 **5.15.3**(dpkg `qtbase5-dev 5.15.3+dfsg-2ubuntu0.2`;`Qt5_DIR=/usr/lib/x86_64-linux-gnu/cmake/Qt5`;`USE_QT6=ON` 但 `Qt6_DIR-NOTFOUND`,回落 Qt5) | CMakeCache + dpkg-query |
| Qt(运行期,实现方) | **5.15.2** —— 实现方 shell 带 `LD_LIBRARY_PATH=/home/charles/Data/Software/Qt5.15/5.15.2/gcc_64/lib`,测试横幅与 `ldd` 均解析到该自装 Qt | `evidence/qt-environment.txt`(ldd/dpkg/结论全文) |
| Qt(运行期,审核方复跑) | **5.15.3** —— 审核环境无上述 LD_LIBRARY_PATH,加载系统运行库 | 审核意见记载;`qt-environment.txt` 已归因 |
| 两环境测试结果 | 均 78/78 通过(实现方 Qt 5.15.2 本机;审核方 Qt 5.15.3 允许回环监听环境) | ctest-baseline.log / 审核记录 |
| 构建二进制摘要 | **binary-manifest.txt**:DeepLux、deeplux_core、deeplux_ui、65 个模块 `.so`、V4L2Camera `.so` 共 69 项 SHA-256 | `evidence/binary-manifest.txt`(整改 R3) |

> 复核命令:`ldd build-codex-agent/bin/test_agentphase0_repro | grep Qt5Core`;`dpkg-query -W qtbase5-dev libqt5core5a libqt5test5`;测试二进制首行 Config 横幅。

## 2. 阶段 0 交付文件(限定范围;未提交,等待授权)

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `scripts/audit_agent_capability_baseline.py` | 新增 | 矩阵生成器:扫描元数据/CMake/二进制/运行时探针,合并操作清单;**引用自动校验(整改 R1)**;**探针目录拒绝复用(整改 R4)** |
| `docs/baseline/agent-capability-matrix.json` | 新增(生成) | **唯一数据源**;69 插件 × 7 维度(483 格,全格 evidencePath 非空)+ 109 操作 |
| `docs/baseline/agent-capability-matrix.md` | 新增(生成) | 由 JSON 渲染,供审核;禁止独立维护 |
| `docs/baseline/agent-gui-operations.json` | 新增 | GUI 操作清单输入;整改 R2/轮 3 后 109 项(拆分复合项、修正过度声称与不等价映射) |
| `tests/test_agentphase0_repro.cpp` | 新增 | F01–F13 基线特征化复现(**16 用例**,含整改 R5 两个控制器端到端用例) |
| `tests/CMakeLists.txt` | 修改 | 仅两处:TEST_SOURCES 注册 + 插件注入/TIMEOUT 块 |
| `docs/baseline/agent-phase-0-acceptance.md` | 新增 | 本记录 |

产品代码(`src/`)**零改动**;他人在先修改(模板头所列)原样保留。

## 3. 插件清单与运行时核对(G0 第 1、2 条)

- 源码元数据:**69** 份 = 模块 **66** + 相机 **3**(脚本实时扫描,未硬编码 69;增删插件自动反映)。
- 已构建二进制:**66**(65 模块 `.so` 于 `build-codex-agent/lib/` + `libV4L2CameraPlugin.so` 于 `build-codex-agent/plugins/camera/`——该插件 `LIBRARY_OUTPUT_DIRECTORY` 指向 `plugins/camera`,不在 `lib/`;全部二进制哈希见 binary-manifest.txt)。
- 运行时探针(隔离目录,`DEEPLUX_APP_DATA_DIR` 重定向,未触碰 `~/.deeplux`;探针脚本对已存在目录**拒绝执行**,整改 R4):`DeepLux list-plugins` 报告 **65/65 模块 [OK]**;日志报告 **V4L2 相机发现并注册成功**;PROBE_EXIT=0。
- **“65 个加载模块与其余 4 项”的差异原因**(逐项,禁止静默跳过):
  1. `DefectDetection`(hymson3d):`ENABLE_HYMSON3D=OFF` 时 CMake 提前 `return()`,注释宣称的 demo 模式构建**实际不可达**——未构建;
  2. `DirectShowCamera`:`camera/CMakeLists.txt` WIN32 分支,Linux **平台不适用**——未构建;
  3. `HikvisionCamera`:`ENABLE_CAMERA_HIKVISION=OFF`(需海康 MVS SDK)——未构建;
  4. `V4L2Camera`:**已构建且运行时加载成功**,但按 `category=camera` 归入 `cameraInfos` 而非模块列表;`test_pluginparametercontracts` 报“0 相机”是因其只按 `lib/` 目录匹配 `.so`(`installAllPlugins` 对缺失二进制**静默 `continue`**,即 F12),未覆盖 `plugins/camera/` 输出目录。
- 以上均记入矩阵 `buildEnabled/buildDisabledReason/runtime/binaryPath` 字段,无一项被写成“通过”。

## 4. 构建与测试基线(G0 第 4 条;原始证据 + SHA-256)

证据目录:`build-codex-agent/agent-phase0-evidence/`(git 忽略的构建目录内;§14.2)。完整哈希以 **`sha256sums.txt`** 为准(下表短哈希供对照;本记录自身不入清单,避免自引用)。首轮原始日志以 `*.v1.log` 保留,未销毁。**整改轮 3(复审 R7):清单已扩展覆盖本阶段全部测试资产**——审计脚本、复现测试源码、`tests/CMakeLists.txt`(源码摘要)与复现测试可执行文件(并入 binary-manifest.txt,70 项)。

| 证据/资产 | SHA-256(前 8 位) | 内容 |
| --- | --- | --- |
| `build-baseline.log` | `9a2cd2bb` | `cmake --build build-codex-agent -j2` 全量输出;`BUILD_EXIT_CODE=0` |
| `ctest-baseline.log` | `934d088b` | 整改后全量 CTest:**78/78 通过**,`CTEST_EXIT=0`(sh -c 包装原子写入) |
| `ctest-baseline.v1.log` | `7301b9ab` | 首轮全量 CTest 原始日志(78/78,退出码行当时因 shell 展开缺陷为空,已由 v2 取代;内容保留) |
| `repro-test-run.log` | `e19f7dd1` | 复现套件(16 用例):**16 passed, 0 failed**,`REPRO_EXIT=0`;首行含 Qt 5.15.2 横幅 |
| `repro-test-run.v1.log` | `8ba18fab` | 首轮复现套件(14 用例)原始日志 |
| `runtime-list-plugins.log` | `6613e0fb` | 运行时探针 stdout+stderr(65 模块 OK/V4L2 相机注册);`PROBE_EXIT=0` |
| `workspace-baseline.txt` | `40f29e49` | 首轮:HEAD 与审计对象文件 SHA-256 |
| `workspace-baseline-r2.txt` | `2213daee` | 整改轮 2:工作区状态与文件摘要 |
| `binary-manifest.txt` | `a8072faf` | **70 项构建二进制 SHA-256**(DeepLux/core/ui/65 模块 .so/V4L2 .so + **test_agentphase0_repro 可执行文件**) |
| `qt-environment.txt` | `ab4096cb` | Qt 5.15.2/5.15.3 差异归因(LD_LIBRARY_PATH/ldd/dpkg/CMakeCache 原始输出) |
| `agent-capability-matrix.json` | `bb9a0683` | 矩阵唯一数据源(整改轮 3 重生成) |
| `agent-gui-operations.json` | `c9e15bc1` | 操作清单(整改轮 3 修正 2 项映射) |
| `audit_agent_capability_baseline.py` | 见清单 | 审计/生成脚本源码摘要 |
| `test_agentphase0_repro.cpp` | 见清单 | 复现测试源码摘要 |
| `tests/CMakeLists.txt` | 见清单 | 测试注册(阶段 0 唯一既有文件修改)摘要 |
| `sha256sums.txt` | — | 上述 15 项的完整哈希清单(复审 R7 扩展后) |

复现命令(审核者可直接执行;探针需全新目录,脚本对已存在目录拒绝执行):

```bash
cmake --build build-codex-agent -j2
env QT_QPA_PLATFORM=offscreen ctest --test-dir build-codex-agent --output-on-failure
env QT_QPA_PLATFORM=offscreen ./build-codex-agent/bin/test_agentphase0_repro
python3 scripts/audit_agent_capability_baseline.py prepare-probe --probe-dir /tmp/dlx-phase0-probe-$$
DEEPLUX_APP_DATA_DIR=/tmp/dlx-phase0-probe-$$/appdata QT_QPA_PLATFORM=offscreen \
    ./build-codex-agent/bin/DeepLux list-plugins
python3 scripts/audit_agent_capability_baseline.py generate \
    --runtime-log build-codex-agent/agent-phase0-evidence/runtime-list-plugins.log
sha256sum -c build-codex-agent/agent-phase0-evidence/sha256sums.txt
git diff --check   # 通过,无输出
```

`generate` 内置引用校验(整改 R1):任一 testId 不存在于复现测试源码/TEST_SOURCES、或任一 evidencePath 不存在,即**拒绝生成矩阵**;本次输出 `reference validation OK: 483 cells, all testId/evidencePath resolved`。

## 5. F01–F13 最小复现(G0 第 5 条:F02/F06/F08 必可按记录重现)

自动化载体:`tests/test_agentphase0_repro.cpp`(特征化测试——断言**当前缺陷行为**并绑定 F 编号;对应收口阶段修复时必须翻转断言,禁止删除用例掩盖问题)。运行:
`env QT_QPA_PLATFORM=offscreen ./build-codex-agent/bin/test_agentphase0_repro`(**16/16 通过**,证据 `repro-test-run.log`)。

| F | 复现方式 | 复现位置/代码入口 | 观察结果(基线) |
| --- | --- | --- | --- |
| F01 | 自动:`testF01GetRunResultsCumulativeOnly` | `AgentActor::getRunResults` | 仅 `totalRuns/successRuns/failedRuns/lastElapsedMs/isRunning`;无 `runId/outputs/moduleResults/lastError` |
| F02★ | 自动:`testF02SetParamCannotExpressArray` | `ToolSchema` set_param `value:"string"`;`AgentActor::setParam` 仅数字/布尔转换;`MeasurementInputPlugin.cpp:55` point1 默认数组 | 数组参数 `[10,20]` 无法经声明接口表达:或被校验拒绝,或以字符串存储,均非数值数组 |
| F03 | 自动:`testF03SchemaAndFlowStateMissingFields` | `AgentActor::getModuleParamsSchema/getFlowState` | Schema 仅 name/description/defaultParams/currentParams,无范围/枚举/单位/数组结构/端口;连线仅 from/to,丢失 fromPort/toPort/edgeType |
| F04 | 自动:`testF04CalibrationKeysNotSettable` | `NPointCalibrationPlugin.cpp:180-184` 读取 `pointCount/imgX_i/…`;键不在默认参数与元数据 | `set_param(pointCount)` 返回 `Unknown parameter`;业务必需参数对 Agent 不可达 |
| F05 | 自动:`testF05PluginAndCameraCatalogWithoutLifecycleTools`（用例名随 Slice 16 更新） | `ToolSchema::registerDefaultTools`(阶段0为19工具) | 本行记录阶段0原始发现：无相机/插件生命周期工具；`get_available_plugins` 无 cameras/loadStatus 字段 |
| F06★ | 自动:`testF06SaveFailedHasNoErrorField` + **端到端** `testF06ControllerLogsFailedSaveAsSuccess`(整改 R5) | `AgentActor::saveProject` 返回 `{status:failed}`;`AgentController.cpp:427/498/626` 以 `contains("error")` 判定 | Actor 层:失败不含 error 字段。**控制器层(端到端)**:Autopilot 确认链路执行失败保存后,审计日志 `actionLogEntryAdded` 记录 `result=="success"` 且 `undoable==true`,工程实际未保存(filePath 为空)——对话日志假成功得证 |
| F07★ | 自动:`testF07OpenCorruptProjectDestroysCurrent` | `ProjectManager::openProject` 先 `closeProject()` 后加载候选 | 打开损坏工程报错正确,但**原工程已被销毁**,get_flow_state 返回 No project |
| F08★ | 自动:`testF08ObserverChatVsBridgeReadOnly` | 聊天路径 `AgentController::onLLMResponse`(Observer 分支直接回 Idle);Bridge 路径 `handleToolCall`(Observer 允许只读) | 同一 Observer 权限:聊天路径**不执行**只读工具 read_documentation(actionSpy=0);Bridge 路径执行并返回 content——策略不一致 |
| F09 | 自动:`testF09WhitelistMissingUiOperations` | `ToolSchema` 白名单 | step/breakpoint/variable/rename/move/enable/validate_flow 均不存在;白名单规模锁定 19 |
| F10 | 自动:`testF10NoOpToolsReturnSuccess` | `AgentActor::removeModule/pauseFlow/resumeFlow` | 未知实例 remove_module 返回 `{status:removed}` 且压入空撤销命令;空闲态 pause/resume 无条件返回成功 |
| F11 | 自动:`testF11AsyncRunHasNoRunIdentity` + 文档化 | `run_flow` 返回 `{status:started,async:true}` 无 runId;`test_agente2e.cpp:231` 以 QTRY 等待 Idle 后才查询 | 立即查询与完成后查询字段集完全一致,结果无法按运行归属;控制器无有界等待/重试/取消闭环 |
| F12 | 文档化(代码+计数证据) | `tests/test_pluginparametercontracts.cpp:146-152`:仅按 `lib/` 匹配 `.so`,缺失即 `continue`(静默跳过) | 69 份元数据 vs 契约测试安装 65 份;V4L2(输出于 `plugins/camera/`)、DefectDetection/DirectShow/Hikvision(未构建)被跳过且无报告;且该测试不走 Agent 业务链路 |
| F13★ | 自动:`testF13RunFlowFolderDeleteNoConfirmation` + **端到端** `testF13AutopilotDeletesViaRunFlowWithoutConfirmation`(整改 R5) | `ToolSchema` run_flow 未标 dangerous(对照 sam_annotate 已标);`FolderPlugin.cpp:55-57` Delete→`removeRecursively()` | Actor 层:run_flow 触发递归删除测试临时目录,无确认门槛。**控制器层(端到端)**:Autopilot 对话路径由模拟 LLM 发起 run_flow,`toolsPendingConfirmation` 信号 0 次、无 Confirming 状态,目录被删除,审计日志记 run_flow success——“未确认即删除”得证。副作用仅使用测试自建临时目录 |

★ = 计划 §5.5 “特别保留”的 6 个必留复现,全部自动化;其中 F06/F13 已按审核整改 R5 补足**控制器路径端到端证据**。

## 6. GUI 操作清单(G0 第 3 条;整改 R2 后)

`docs/baseline/agent-gui-operations.json`(经生成器并入矩阵):**109 项**(105 → 拆分复合项后 109;ID 全局唯一,已程序校验),覆盖计划要求的全部页面——主菜单(文件/参数/视图/工具/帮助)、主工具栏、工具箱、流程树、流程画布、检查器、数据源面板、主视口工具栏、快速测量(6 种)、日志/终端面板、Agent 对话与日志、SAM 标注(11 项)、知识库(6 项)、Agent 设置(5 项)、相机/通讯/全局变量/系统设置、登录、状态栏。

整改 R2 的拆分与降级(逐项核对实际入口后):

- `SAM_CONFIRM_CANCEL` → `SAM_CONFIRM_OBJECT`(直接工具 sam_annotate)+ `SAM_CANCEL_PROMPT`(未支持,GUI 编辑态无对应物);
- `SAM_SESSION_OPEN_SAVE` → `SAM_SESSION_SAVE`(直接工具 sam_annotate(sessionPath))+ `SAM_SESSION_OPEN`(未支持,无会话读取工具);
- `KB_FILTER_PREVIEW` → `KB_SEARCH`(直接工具 read_documentation)+ `KB_LIST_DOCS`(未支持)+ `KB_PREVIEW_DOC`(未支持,分节检索≠按文档预览);
- `PARAM_UNDO/PARAM_REDO`:直接工具 → **未支持**(白名单无 undo/redo 工具,F09 用例锁定 19 项清单);
- `TB_QUICK_ANNOTATE`:直接工具 → **未支持**(打开对话框为纯 GUI 动作;SAM 能力由 SAM_* 项承载);
- `CHAT_SEND_TEXT/CHAT_ATTACH_IMAGE`:直接工具 → **人机协作**(Agent 对话入口本身,非可映射工具操作);
- `TOOLBOX_SEARCH`:直接工具 → **通用工具组合**(仅全量枚举,无过滤/分类/加载状态);
- `TOOL_CAMERA_SETTINGS/TOOL_COMM_SETTINGS/TOOL_KNOWLEDGE_BASE` 收窄为“打开设置页”,页内原子操作由 CAM_*/COMM_*/KB_* 各项承载。

整改轮 3 的再更正(复审第 2 轮发现,详见 §12):

- `SAM_SESSION_SAVE`:直接工具 → **未支持**——GUI 保存的是现有内存会话;`sam_annotate` 要求新提示并重跑分割后创建/追加文件,非等价入口;既有会话持久化工具属阶段 6;
- `STATUSBAR_INFO`:直接工具 → **未支持**——`get_flow_state` 仅返回模块与连线;运行状态仅可经 `get_run_results.isRunning` 间接获得(权威运行状态属阶段 3,F01),用户/时间显示无工具入口。

映射统计(整改轮 3 重生成后):直接工具 **13**(逐项与 19 工具白名单及工具实际返回核对:FILE_NEW_SOLUTION/FILE_OPEN/FILE_SAVE/TB_RUN_ONCE/TB_STOP/TOOLBOX_DRAG_ADD/PTREE_DELETE/CANVAS_CONNECT_PORTS/INSP_TAB_PARAMS/SAM_PROMPT_POINTS_BOX/SAM_CANDIDATE_NAV/SAM_CONFIRM_OBJECT/KB_SEARCH)/ 通用工具组合 **3** / 人机协作 **25** / 未支持 **68**;阶段分布见矩阵 MD。凭据/登录/SDK 安装/终端均映射为人机协作,不提供 Agent 直通(§1.2 边界)。

## 7. 能力矩阵摘要(全量见 JSON/MD;整改 R1 后)

七维度按分母报告(§5 要求,不给单一总百分比);阶段 0 的矩阵记录**基线现状**,PASS=0 是预期结果(§3.2:阶段 0 PASS 指基线完整可重复,不表示缺陷已修复)。**483/483 单元格 evidencePath 非空且经生成器自动校验**(testId 全部解析到真实用例/套件):

| 维度 | PASS | FAIL | BLOCKED | N/A | NOT_TESTED | 分母 |
| --- | --- | --- | --- | --- | --- | --- |
| 发现 | 0 | 69 | 0 | 0 | 0 | 69 |
| 配置 | 0 | 1 | 0 | 0 | 68 | 69 |
| 输入 | 0 | 0 | 4 | 0 | 65 | 69 |
| 执行 | 0 | 0 | 4 | 0 | 65 | 69 |
| 输出 | 0 | 66 | 3 | 0 | 0 | 69 |
| 权限 | 0 | 15 | 0 | 54 | 0 | 69 |
| 撤销/持久化 | 0 | 4 | 0 | 8 | 57 | 69 |

关键口径:发现=FAIL(F05,全员);输出=FAIL(F01,66 模块,testId 已修正为 `testF01GetRunResultsCumulativeOnly`)/BLOCKED(3 相机);配置 FAIL 仅 NPointCalibration(F04),其余 NOT_TESTED(契约测试按 F12 判定不构成 Agent 链路证据);权限 FAIL=15 个有副作用插件(F13,Folder 含端到端用例),纯计算 54 项 NOT_APPLICABLE(理由已记录,不计入通过分母);撤销维度文件副作用 4 插件 FAIL(save_project undoable 误标已有端到端证据)。每格含 `status/reason/phase/testId/evidencePath`。

历史台账(`docs/baseline/capability-matrix.md`,67 插件时代)的 可用/部分/实验性/依赖硬件 分类已作为 `legacyStatus` 字段并入 JSON 并标注来源;`JiErHanDefectsDet` 保持**实验性**标记、`DefectDetection` 保持 demo 模式不可达标记——不以新增 Agent 接口升级其能力定级(§1)。

## 8. G0 验收标准自检(判定权在审核者)

- [x] 源码清单与 69 份元数据一致;计数由脚本实时扫描,插件增删自动反映(未硬编码 69)——§3
- [x] 65 个加载模块与其余 4 项差异原因逐项记录,无一写成“通过”——§3
- [x] GUI 操作清单覆盖计划所列全部页面,109 项均有稳定编号与责任阶段——§6
- [x] 基线原始命令、stdout/stderr、退出码、环境、**源码与二进制摘要**已保存:binary-manifest.txt 70 项(含复现测试可执行文件)+ sha256sums.txt 15 项(含审计脚本/复现测试源码/tests/CMakeLists.txt,整改轮 3 扩展);复现命令可直接执行——§1/§4
- [x] F02、F06、F08 可由审核者按 §5 记录单命令重现(`test_agentphase0_repro`,16/16)——§5
- 已知失败与问题编号绑定:基线 CTest 无失败;全部缺陷以 F01–F13 编号绑定复现用例,未删测、未放宽断言、未省略插件。

## 9. 现场依赖与环境注意事项(不以模拟冒充)

| 项 | 状态 | 归属 |
| --- | --- | --- |
| **全量 CTest 需允许本机回环监听** | 受限沙箱(禁止 bind/listen)中 4 个监听类套件失败:`test_sam_region_flow`、`test_agent_sam_tools`、`test_sambackendclient`、**`test_agentbridge`**(本地 socket 监听;测试内自建 HTTP/TCP 服务)。审核方定向复跑证实 `test_communicationmanager` 在受限环境**通过**(轮 2 记录曾误列,已按实际证据更正,见 §12);允许监听的环境全部 78/78 通过。**属环境限制,非产品回归失败** | 环境要求 |
| 真实相机(V4L2 设备/海康+SDK/DirectShow+Windows) | BLOCKED_EXTERNAL(无设备/平台) | 阶段 5(模拟)/8(真机) |
| 真实 PLC/串口/TCP 现场设备 | BLOCKED_EXTERNAL | 阶段 5(本地模拟)/8(现场) |
| SAM 真实 GPU 权重与推理质量 | BLOCKED_EXTERNAL | 阶段 8 |
| 真实 LLM Function Calling | 未验证(基线仅模拟 LLM;模拟仅产出响应,不触碰 Project/Engine) | 阶段 7(协议)/8(真实) |
| Windows 平台构建/驱动 | 未在本机验证(CI 有产品构建门禁) | 阶段 8 |

## 10. 遗留事项与责任人

1. 阶段 0 交付文件的**提交授权**待维护者决定(§3.1.5;当前为限定文件补丁+本摘要,未提交;审核建议不提交/不推送,已遵从)。
2. `test_pluginparametercontracts` 的静默跳过(F12)与 `plugins/camera/` 输出目录差异——收口属阶段 0→8 的覆盖检查改造,本阶段仅记录,未改产品/既有测试行为。
3. 另一会话的知识库扩展脏文件为本审计对象的一部分,已按原样保留并记入摘要(workspace-baseline-r2.txt);其提交归属由维护者裁定。
4. 矩阵中 66 项“未支持”操作为阶段 2–6 的分母,任何阶段收口须回写矩阵 JSON 并重新渲染 MD(生成器引用校验会拦截失效引用)。

## 11. 整改轮变更记录(首轮审核 FAIL → 本轮送审)

| 审核发现 | 整改 | 验证 |
| --- | --- | --- |
| R1:483/483 单元格 evidencePath 为 null;输出维度 66 处引用不存在的 `testF01GetRunResultsHasNoModuleOutputs` | 生成器为每格注入证据路径(repro/ctest/探针日志/验收记录);testId 修正为 `testF01GetRunResultsCumulativeOnly`;新增 `validate_references`——testId 必须解析到复现测试源码用例或 TEST_SOURCES 套件、evidencePath 必须存在,否则拒绝生成 | 生成输出 `reference validation OK: 483 cells`;JSON 抽查 null evidencePath=0 |
| R2:复合操作被整体标为直接工具(SAM 确认/取消、会话打开/保存、知识库过滤/预览),21 项直接工具统计不可靠 | 拆分为原子操作(105→109);逐项与 19 工具白名单核对;PARAM_UNDO/REDO、TB_QUICK_ANNOTATE、CHAT_* 等过度声称全部降级(详见 §6) | 直接工具 21→**15**,每项 currentTool 均在白名单内;ID 唯一性程序校验通过 |
| R3:声称保存源码/二进制摘要但缺构建二进制哈希;Qt 5.15.2/5.15.3 差异未归因 | 新增 `binary-manifest.txt`(69 项二进制 SHA-256)与 `qt-environment.txt`(LD_LIBRARY_PATH→自装 Qt 5.15.2 vs 系统 5.15.3,ldd/dpkg/CMakeCache 原始输出) | §1 环境表双运行期口径;`sha256sum -c` 全 OK |
| R4:探针脚本会递归删除 --probe-dir 下已有 appdata/plugins | 移除 rmtree;目标已存在时**拒绝执行**并提示换全新目录 | 脚本 180-188 行;复现命令改用 `/tmp/dlx-phase0-probe-$$` |
| R5:F06 仅复算分类表达式、F13 绕过控制器,缺端到端证明 | 新增 `testF06ControllerLogsFailedSaveAsSuccess`(Autopilot 确认链路→审计日志 result=success/undoable=true→实际未保存)与 `testF13AutopilotDeletesViaRunFlowWithoutConfirmation`(模拟 LLM 发起 run_flow→0 确认信号→目录被删→日志记 success) | 复现套件 14→**16 用例**,16/16 通过(repro-test-run.log) |
| 附:首轮日志退出码行因 shell `$?` 展开缺陷为空 | 以 `sh -c '…; echo EXIT=$?'` 包装重跑 repro 与全量 ctest,退出码原子写入;首轮日志以 `*.v1.log` 原样保留 | ctest-baseline.log `CTEST_EXIT=0`;repro `REPRO_EXIT=0` |

## 12. 整改轮 3 变更记录(复审第 2 轮 FAIL → 本轮送审)

轮 2 复审已独立确认:R1/R3/R4/R5 整改核实、复现套件 16/16、全量 CTest 78/78(允许监听环境)、12 项证据哈希与 69 项二进制哈希校验通过、483 单元格证据可解析且失效引用被校验器拒绝。本轮针对其余 3 项发现:

| 复审发现 | 整改 | 验证 |
| --- | --- | --- |
| R6:`SAM_SESSION_SAVE` 仍标 sam_annotate 直接支持(GUI 保存内存会话 ≠ 工具要求新提示重跑分割);`STATUSBAR_INFO` 标 get_flow_state 直接支持(该工具仅返回模块与连线) | 两项均改**未支持**并在 notes 记录不等价原因与收口阶段(会话持久化→阶段 6;权威运行状态→阶段 3/F01);其余 13 项直接工具逐条对照工具实际请求/返回复核 | 矩阵重生成:直接工具 15→**13**、未支持 66→**68**(109 项总数不变);引用校验 `OK: 483 cells` |
| R7:摘要清单与二进制清单未覆盖审计脚本、复现测试源码、tests/CMakeLists.txt、复现测试可执行文件 | `binary-manifest.txt` 增加 `build-codex-agent/bin/test_agentphase0_repro`(69→**70 项**);`sha256sums.txt` 增加 `scripts/audit_agent_capability_baseline.py`、`tests/test_agentphase0_repro.cpp`、`tests/CMakeLists.txt`(12→**15 项**),并同步刷新矩阵/操作清单/二进制清单的新哈希 | `sha256sum -c build-codex-agent/agent-phase0-evidence/sha256sums.txt` 15/15 OK |
| R8:§9 将受限环境失败项误记为 `test_communicationmanager`;实际为 `test_agentbridge` 的本地 socket 监听 | §9 按审核方定向复跑证据更正:受限沙箱 4 项 = sam_region_flow / agent_sam_tools / sambackendclient / **agentbridge**;明确 `test_communicationmanager` 受限环境通过;维持“环境限制、非产品回归失败”定性 | §9 表首行;审核方复跑记录为准 |

本轮未改动任何产品代码与测试断言(16 用例、78/78 结果与轮 2 完全一致,无需重跑);变更仅限操作清单 2 条映射、矩阵重生成、两份哈希清单扩展与本记录。原始证据(含 v1 日志)继续保留。
