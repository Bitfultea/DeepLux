# Agent Async Loop 设计 Review

## Current Decision

当前实现不再额外引入 `m_agentBusy`。`AgentController::state() != Idle` 已经承担 busy gate：

- 用户新消息在非 Idle 状态会被拒绝；
- 外部 `tool_call` 在非 Idle 状态会返回 busy error；
- pending confirmation 只在 `Confirming` 状态处理；
- LLM loop 在 `Executing`/`Thinking` 间流转。

这比再维护一个并行布尔锁更简单，也减少状态不同步的风险。

## Implemented Safety Measures

- `confirmPendingTools()` 先检查状态和空 pending 数组。
- 确认前复制 `m_pendingToolCalls`，再清空 pending。
- `rejectPendingTools()` 清空 pending 并回到 Idle。
- `extendAgentLoop()` 限制单轮最多 10 个 tool calls。
- 自动执行最多 20 轮，避免 LLM 无限工具循环。
- `AgentActor::executeTools()` 先校验 required/enum 参数，执行中遇到业务错误即停止。
- tool result 作为独立 `tool` role 消息写回 conversation history。
- `OpenAILLMClient` 设置 30 秒 transfer timeout。

## Current Flow

```text
User message
  -> AgentController::sendUserMessage()
  -> state: Idle -> Thinking
  -> LLM request
  -> onLLMResponse()

If no tool calls:
  -> append assistant message
  -> state: Idle
  -> show response

If tool calls:
  Observer:
    -> show response/tool calls only
    -> state: Idle

  Advisor:
    -> write tools require confirmation
    -> state: Confirming
    -> show Tool Preview card

  Autopilot:
    -> dangerous tools require confirmation
    -> safe tools execute directly
    -> state: Executing
    -> AgentActor::executeTools()
    -> append tool results
    -> state: Thinking
    -> next LLM request
```

Direct external `AgentBridge tool_call` follows the same permission policy. If confirmation is required, it returns `pending_confirmation` and waits for GUI confirmation; it does not continue the LLM loop after confirmation.

## QueuedConnection Decision

The original recommendation was “all loop steps via QueuedConnection”. Current implementation uses direct synchronous execution after confirmation because `AgentActor` tools are local and lightweight.

This is acceptable while tool execution stays short. Do not add async machinery preemptively.

Upgrade trigger:

- a tool performs long image processing;
- `run_flow` becomes blocking in practice;
- UI screenshots or manual testing show visible freeze during confirmation.

Minimal future change when needed:

- keep public `confirmPendingTools()` unchanged;
- copy calls immediately;
- use `QMetaObject::invokeMethod(this, [this, calls] { doConfirmPendingTools(calls); }, Qt::QueuedConnection)`;
- keep state checks in `doConfirmPendingTools()`.

## Remaining Improvements

1. Add protocol-level tests around `AgentBridge` busy responses and pending confirmation.
2. Add UI tab status/highlight when state becomes `Confirming`.
3. Rename the Agent log undo button to “撤销最近操作” so stack-based undo is clear.
4. Revisit queued execution only after a measured or visible UI freeze.

## Verification

```bash
ctest --test-dir build -R "test_agent(controller|permissions|actor|undo)" --output-on-failure
```
