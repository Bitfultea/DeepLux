# Agent 集成更新收口验收记录

- 日期：2026-10-29
- 基线：`9cf9aa7`（第一迭代 `af56b5b`、第二迭代 `0fd1aa2`、集成修正）
- 分支：`agent/integration-hardening`
- 计划：[agent-integration-hardening-plan.md](../agent-integration-hardening-plan.md)

## 1. 交付提交

| 批次 | 提交 | 内容 |
|---|---|---|
| B0 | `3fe85a0` | 三份设计文档与路线图契约统一；基线冻结与有意行为变化清单 |
| B1 | `0b30827` | 单源成功判定、失败后停止本批并补齐跳过结果、仅 `run_failed` 续跑、审计区分 skipped/rejected |
| B2 | `0240448` | Observer 混合批次逐项判定、确认绑定工程会话/版本、逐项门禁、create/open 后终止旧批次 |
| B3 | `c99d833` | 真实用户/自动回灌来源标记、保护集合裁剪、统一发送门禁与 `context_budget_exceeded` 失败关闭 |
| B4 | `3de03a6` | MCP 异步可取消输入、有界分帧/队列、子进程黑盒测试 |
| B5 | `2f12592` | 隐私开关；失败请求正文默认关闭且尊重显式值 |
| B6 | 本记录提交 | 闭环验收、空宏修复、验收记录与进度收口 |

## 2. 有意行为变化

见计划 §12 的 C1~C10。实现与文档已一致，重点：

- 工具结果成功判定以布尔 `success` 优先；`success:false` 不再被任何入口当成成功。
- 批量失败后剩余调用生成 `skipped_after_failure`，每个 tool_call 恰有一条结果。
- 仅 `run_failed`（及 `create/open` 成功后的 `project_switched`）允许下一轮重新规划。
- 确认绑定工程会话身份 + 内部递增版本；切换/关闭/删除/外部改参使旧确认失效。
- 上下文保护当前真实请求、在途组、最近完整组；保护集合超限时不发请求。
- MCP 取消只抑制本地迟到响应；输入帧、待处理队列有界。
- 失败请求正文未显式配置时默认关闭；关闭不删除已有文件。

另外修复 B6 复查发现的空撤销宏问题：批量中只有可撤销工具才开启宏，纯读/运行批次不再向撤销栈压入空宏。

## 3. 自动门禁结果

环境：Linux 6.8 / Qt 5.15.2 / Debug；`build/CMakeCache.txt` 的
`CMAKE_HOME_DIRECTORY=/home/charles/Data/Repo/DeepLux`。

```bash
cmake --build build -j4                      # 通过
TEST_HOME=$(mktemp -d /tmp/deeplux-agent-hardening.XXXXXX)
env HOME="$TEST_HOME" bash scripts/sync-plugins.sh build
# [DONE] prepared=67 copied=67 skipped=0 cleaned=0

env -u DEEPLUX_APP_DATA_DIR HOME="$TEST_HOME" QT_QPA_PLATFORM=offscreen \
  ctest --test-dir build --output-on-failure -j4
# 100% tests passed, 0 tests failed out of 86 (53.9s)

# B1~B4 + 撤销边界定向重复 20 次
# repeat_passed=20/20
```

`git diff --check` 通过；改动 C++ 文件已执行 `clang-format -i`。

新增/加强的主要用例：

- `test_agentactor`：成功判定单源、预校验全量跳过、批量门禁、失败后剩余调用零执行。
- `test_agentcontroller`：Observer 纯只读续跑/混合批次、确认过期（切换工程、入队后切换、目标删除、外部改参）、`create_project` 终止旧批次、确认只执行一次、上下文保护与预算失败关闭、非法配对拒绝发送。
- `test_agentclosedloop`：`run_failed` 后跳过后续写操作并重新规划；连续失败受轮数上限约束；非 `run_failed` 结束本轮；B6 完整闭环（失败→诊断跳过→改参→重跑成功→审计→撤销）。
- `test_agentaudit`：失败正文默认关闭/显式开/显式关/持久化，以及关闭后不写盘。
- `test_agentstreamui`：隐私开关默认值、显式值加载、保存绑定、最小宽度可见性。
- `test_mcpbridge`：异步取消抑制迟到响应、取消不影响下一请求、超长/错误帧恢复、EOF 半帧、`id:null` 与通知区分、队列上限、真实 `deeplux-mcp-bridge` 子进程黑盒。

## 4. 未验证项（不得写成已通过）

| 项 | 状态 | 原因 |
|---|---|---|
| Windows 构建 + CTest + stdin/取消/退出 | 未验证 | 当前环境为 Linux；Windows 命名管道黑盒测试以 `QSKIP` 跳过 |
| 真实模型服务闭环/失败修正/用户拒绝/中途取消各 3 次 | 未验证 | 无可用真实供应商凭据与计费环境；本轮仅使用脚本化/假 HTTP |
| 真实视觉模型图像进入请求、纯文本模型降级 | 未验证 | 同上（离线用例已覆盖回灌与文本降级路径） |
| 实际 MCP 客户端（Claude Desktop/Cursor 等） | 未验证 | 无客户端环境；子进程测试使用真实 bridge 进程但假 GUI 桥 |
| 真实 GUI 手工“确认前切工程”提示 | 未验证 | 仅 headless QtTest 覆盖状态与参数不变 |
| TSan / 长期并发 | 未运行 | 计划明确不以重复测试替代 TSan |
| CUDA 推理 | 未改动 | 本轮不涉及 CUDA，既有证据只能标注沿用 |

## 5. 风险与说明

- MCP 取消采用“断开本地连接 + 抑制迟到响应”，不声称中止 GUI 内已开始的操作；GUI 侧仍按单请求在途处理。
- `context_budget_exceeded` 为失败关闭：本轮不自动摘要，用户可清理对话或提高预算。
- 失败请求正文默认关闭是行为变化；已显式配置 `agent/failedRequestLogEnabled=true` 的现场继续按原设置记录。
- 审计与失败正文的脱敏规则不能保证识别任意机密；敏感机台仍需控制目录访问权限。
