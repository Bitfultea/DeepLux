# DeepLux 终端与 LLM Agent 集成设计

## Context

DeepLux 需要同时支持三类交互：

- 人类用户在 GUI 中使用真实终端。
- GUI 操作、CLI 命令和运行日志相互可见。
- LLM Agent 辅助操作软件，但必须可审计、可确认、可撤销。

旧设计把 AgentBridge 的 `execute` 作为任意 shell 命令入口。这个方向不再采用。LLM Agent 不应拥有任意 shell 执行权；当前更安全的设计是：终端服务人类用户，Agent 只通过白名单 `tool_call` 调用 DeepLux 内部工具。

## 设计原则

- 真实终端和 Agent 执行面分离。
- Agent 操作必须结构化、权限受控、可审计。
- 写操作优先通过 `AgentActor` 和 `QUndoStack`，而不是 shell 命令。
- GUI 主入口保持在底部 `Agent 对话` tab，审计入口保持在 `Agent 日志` tab。
- 分阶段交付，每阶段可由 Qt Test 或截图验证。

## 需求总结

| 需求 | 当前方案 |
|------|---------|
| 真 bash 终端（跨平台） | `BashProcess`: Linux POSIX PTY, Windows ConPTY |
| GUI -> 终端同步 | `TerminalBridge` 监听 GUI/日志/运行信号并格式化输出 |
| 终端 -> GUI 反馈 | CLI wrapper 将 `deeplux ...` 命令回传到 `CLIHandler` |
| LLM Agent 读取状态 | `AgentBridge` 提供注册式 `query` |
| LLM Agent 操作软件 | `AgentBridge` 只接受白名单 `tool_call`，路由到 `AgentController`/`AgentActor` |
| Agent 保活 | server 发送 `ping`，超时发出 `agentConnectionLost`；外部 Agent 客户端负责重新连接 |
| GUI Agent 入口 | 底部 `Agent 对话` tab |
| Agent 审计/撤销 | 底部 `Agent 日志` tab + `AgentActor` undo stack |

## Architecture

```text
MainWindow
  bottom tabs:
    - 日志
    - 终端            -> TerminalWidget -> BashProcess -> PTY shell
    - Agent 对话      -> AgentChatPanel -> AgentController -> LLM client
    - Agent 日志      -> AgentActionLogWidget -> AgentController undo

TerminalBridge
  - mirrors GUI/log/run events into TerminalWidget
  - starts AgentBridge IPC server
  - keeps terminal/CLI integration separate from Agent tool execution

AgentBridge
  - QLocalServer IPC: Unix domain socket on Linux, named pipe on Windows
  - JSON protocol v1.0
  - accepts: tool_call, query, subscribe, ping
  - rejects: execute

AgentController
  - manages conversation, permission level, pending confirmations and loop limits
  - Observer: read-only tools only
  - Advisor: write tools require confirmation
  - Autopilot: safe write tools execute directly, dangerous tools require confirmation

AgentActor
  - executes schema-defined DeepLux tools
  - validates required/enum params
  - wraps write operations in undo commands
```

## Core Components

### BashProcess

`BashProcess` owns the real terminal process. It provides:

- platform-specific PTY implementation through `PtyImpl`;
- shell detection;
- command history protected by `QReadWriteLock`;
- 50 ms output throttling;
- raw byte output to `TerminalWidget`, where ANSI parsing happens.

It is for human terminal use and CLI wrapper integration, not for Agent arbitrary execution.

### TerminalWidget

`TerminalWidget` renders a character-grid terminal backed by `TerminalScreen`, `TerminalRenderer`, and `AnsiParser`.

Key behavior:

- receives `BashProcess::outputReady` and `errorReady` via `Qt::QueuedConnection`;
- sends keyboard input directly to PTY;
- supports terminal copy/paste conventions;
- receives formatted GUI event output from `TerminalBridge`.

### TerminalBridge

`TerminalBridge` bridges GUI state to terminal output and routes internal CLI commands. It also starts/stops `AgentBridge`.

Responsibilities:

- print project/run/plugin/log events to the terminal;
- run known CLI commands through `CLIHandler`;
- forward unknown terminal input to bash;
- send selected GUI events to AgentBridge subscribers.

It must not route Agent requests to bash.

### AgentBridge

`AgentBridge` is the IPC server for external or embedded Agent integrations.

Accepted protocol messages:

```json
{"version": "1.0", "type": "tool_call", "id": "req-001", "payload": {"tool": "add_module", "params": {"plugin": "GrabImage"}}}
{"version": "1.0", "type": "query", "id": "req-002", "payload": {"target": "project"}}
{"version": "1.0", "type": "subscribe", "id": "req-003", "payload": {"event": "run_finished"}}
{"version": "1.0", "type": "ping", "id": "req-004"}
```

Rejected message:

```json
{"version": "1.0", "type": "execute", "id": "req-005", "payload": {"command": "rm -rf /tmp/x"}}
```

`execute` remains rejected with a deprecation error. This is intentional: unrestricted shell access is not an Agent capability.

Responses:

```json
{"version": "1.0", "type": "result", "id": "req-001", "payload": {"status": "ok"}}
{"version": "1.0", "type": "error", "id": "req-005", "payload": {"message": "Message type 'execute' is deprecated. Use 'tool_call' instead."}}
{"version": "1.0", "type": "event", "event": "run_finished", "payload": {"success": true}}
```

Heartbeat behavior:

- server sends `ping` every 10 seconds to connected clients;
- after 3 missed heartbeats, server emits `agentConnectionLost`;
- client is responsible for reconnecting to the server socket/pipe.

### AgentController and AgentActor

The Agent execution surface is intentionally narrow:

- tools are declared in `ToolSchema`;
- `AgentActor` validates inputs before execution;
- write actions go through undoable commands where practical;
- `AgentController` owns permission policy, confirmation state and LLM loop limits.

Current safety defaults:

- `Observer`: read-only only;
- `Advisor`: any write tool requires user confirmation;
- `Autopilot`: dangerous tools require confirmation;
- max 20 automatic tool turns;
- max 10 tool calls per turn.

## GUI Entry Points

The GUI entry point remains the bottom `Agent 对话` tab. This is deliberate:

- it keeps Agent interaction near logs and terminal output;
- it does not steal space from the main vision workflow canvas;
- confirmation cards stay close to the conversation that triggered them.

The bottom `Agent 日志` tab is the audit surface. Its undo behavior is stack-based: it reverts the most recent undoable Agent action, not an arbitrary selected historical row.

## Current Status

Implemented:

- real PTY terminal;
- terminal event mirroring;
- AgentBridge `tool_call`, `query`, `subscribe`, `ping`;
- deprecated `execute`;
- Agent permission levels;
- pending confirmation card;
- dangerous tool marking;
- undo stack for Agent writes;
- Agent loop turn/tool-count limits;
- Agent chat/status/log tabs.

Known gaps:

- AgentBridge protocol branches need fuller IPC-level tests;
- tab status/highlight for “thinking / waiting confirmation / error” is still minimal;
- `Agent 日志` undo button wording should say “撤销最近操作”;
- Tool Preview is displayed as a card, but not yet also recorded as a `Tool` message in the chat flow;
- failed Agent messages do not have a retry action.

## Implementation Plan

### Phase 1: Documentation Sync

1. Keep this document as the source of truth for terminal/Agent boundaries.
2. Keep `execute` documented as deprecated and intentionally rejected.
3. Document bottom `Agent 对话` as the primary GUI entry.
4. Document client-owned reconnection.

### Phase 2: Small UI Polish

1. Rename Agent log undo button to “撤销最近操作”.
2. Highlight or switch to `Agent 对话` when a pending confirmation appears.
3. Add tab tooltip/status for thinking, waiting confirmation and error.
4. Add a short `Tool` message when showing a Tool Preview card.

### Phase 3: Protocol Coverage

1. Add AgentBridge tests for rejected `execute`.
2. Add AgentBridge tests for `tool_call`, `query`, `subscribe`, and `ping`.
3. Keep bash terminal tests separate from AgentBridge tests.

### Phase 4: Optional Chat UX

1. Add retry for failed user messages.
2. Revisit asynchronous tool execution only if real tools become slow enough to block UI.

## Verification

For documentation consistency:

```bash
rg -n "转发到 bash|execute.*bash|自动尝试重连" docs/Terminal_Agent_Design.md | rg -v "rg -n"
```

Expected: no statement claims that Agent requests are forwarded to bash or that DeepLux actively reconnects clients.

For related behavior:

```bash
ctest --test-dir build -R "test_(agent|terminal|mainwindow)" --output-on-failure
```
