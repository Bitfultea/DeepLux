# Agent Async Loop 设计 Review

## 设计方案：全程 QueuedConnection

```cpp
// 所有入口用 QueuedConnection / QMetaObject::invokeMethod 入队
Confirmed → invoke("confirmPendingTools", QueuedConnection)
  → promptPendingTools → invoke("extendAgentLoop", QueuedConnection)
    → executeTools → invoke("sendRequest", QueuedConnection)
      → post() → 事件循环 → finished → invoke("onLLMResponse", QueuedConnection)
        → [再来一轮] invoke("extendAgentLoop", QueuedConnection) ...
```

每步返回事件循环，UI 不阻塞。

---

## 风险点

### 🔴 风险 1：并发 Agent Loop（最严重）

队列中已有待执行的 `extendAgentLoop`，但用户在前端又点了 Confirm 或发了新消息，触发第二个 `invoke`。两个 loop 同时操作 `m_conversationHistory` 和 `m_agentTurnCount`。

```
时间线:
  t0: 用户点 Confirm → invoke(extendAgentLoop) 入队
  t1: 事件循环处理 invoke，sendRequest 发完 → 返回事件循环
  t2: LLM 响应未到，用户又发了新消息 → invoke(sendUserMessage) 入队
  t3: 事件循环处理 invoke，sendUserMessage 追加 history + 重置 turnCount = 0
  t4: LLM 响应到达 → onLLMResponse 追加 assistant 消息
  t5: 新一轮 extendAgentLoop 的 turnCount 已被重置为 0
      同时 history 中有两条独立的 user 消息
```

**建议**：加 `m_agentBusy` 锁，正在执行时拒绝新的用户输入和 Confirm。

```cpp
void AgentController::sendUserMessage(const QString& message) {
    if (m_agentBusy) {
        emit llmResponseReceived("Agent is busy, please wait...", {});
        return;
    }
    m_agentBusy = true;
    // ...
}

void AgentController::onLLMResponse(const AgentResponse& resp) {
    if (!resp.toolCalls.isEmpty()) {
        // 继续 loop
    } else {
        m_agentBusy = false;  // 终态解锁
    }
}
```

### 🟠 风险 2：cancel/confirm 竞态

用户点了 Confirm，`invoke(confirmPendingTools)` 入队。但在它执行前，用户又点了 Cancel（或同一张卡点了两次 Confirm）。

```
t0: Confirm → invoke(confirmPendingTools) 入队
t1: Cancel → invoke(rejectPendingTools) 入队
t2: confirmPendingTools 执行 → extendAgentLoop
t3: rejectPendingTools 执行 → m_pendingToolCalls 已是空数组
```

**建议**：在 `confirmPendingTools` 和 `rejectPendingTools` 入口检查 `m_pendingToolCalls.isEmpty()`，空数组直接返回。

### 🟠 风险 3：m_conversationHistory 并发写

`sendUserMessage` 追加 user 消息到 history，同时 `extendAgentLoop` 追加 tool 结果到 history。如果两者通过 QueuedConnection 交替执行，history 会交错错乱。

```
t0: sendUserMessage → history = [user1]
t1: extendAgentLoop → history = [user1, assistant1]
t2: sendUserMessage → history = [user1, assistant1, user2] ← 插入在中间
t3: extendAgentLoop → history = [user1, assistant1, user2, tool2] ← 对不上了
```

**建议**：`m_agentBusy` 锁可解决，因为 busy 时拒绝 sendUserMessage。

### 🟡 风险 4：QMetaObject::invokeMethod 必须是 Q_INVOKABLE 或 slot

不是所有方法都是 Qt slot。需要声明：
```cpp
// AgentController.h
public slots:
    void confirmPendingTools();
    void rejectPendingTools();
private slots:
    void extendAgentLoopImpl(const QJsonArray& toolCalls);  // 需要改签名
```

或者用 lambda: `QMetaObject::invokeMethod(this, []{...}, Qt::QueuedConnection);` (Qt 5.10+)

### 🟡 风险 5：m_pendingToolCalls 需要在 invoke 前拷贝

必须**先拷贝再入队**，避免入队到执行之间被修改：
```cpp
void AgentChatPanel emit handler() {
    // ❌ 错：直接传引用
    QMetaObject::invokeMethod(&AgentController::instance(),
        "confirmPendingTools", Qt::QueuedConnection);
    // → confirmPendingTools 执行时 m_pendingToolCalls 可能已被其他人清空
}
```

正确做法：
```cpp
QJsonArray calls = AgentController::instance().pendingToolCalls();  // 拷贝!
QMetaObject::invokeMethod(&AgentController::instance(), [calls] {
    instance().extendAgentLoop(calls);
}, Qt::QueuedConnection);
```

当前代码已经做了拷贝（`QJsonArray calls = m_pendingToolCalls; m_pendingToolCalls.clear()`），没问题。

### 🟢 风险 6：Tool Preview 重复显示

`onLLMResponse` 如果是直接连接（从 `QNetworkReply::finished`），在 Advisor 模式会 emit `toolsPendingConfirmation` → `showToolPreview` → 创建新 card。如果 LLM 在 Autopilot 模式（没有 confirm 步骤），loop 自动继续，不会积攒 card。

Advisor 模式每次都等用户确认，不会有两个 card 同时存在。低风险。

---

## 推荐最终方案

```
                                用户点击 Confirm
                                      │
                    DirectConnection  │ queued 的 Confirmed handler
                                      ▼
                          QMetaObject::invokeMethod(
                              &AgentController::confirmPendingTools,
                              Qt::QueuedConnection)
                                      │
                              ┌───────┘
                              ▼ (事件循环)
                      confirmPendingTools()
                      if (m_agentBusy) return;  ← 防并发
                      m_agentBusy = true;
                      extendAgentLoop(calls);
                        ├─ executeTools (同步, <1ms)
                        ├─ 追加 history
                        ├─ emit agentLoopIterating
                        └─ sendRequest (异步网络)
                              │
                              ▼ (事件循环)
                      finished → onLLMResponse
                        ├─ 有 tool_calls?
                        │   ├─ Advisor: emit toolsPendingConfirmation → show Tool Card
                        │   │     (返回事件循环, 等用户 Confirm → 回到顶部)
                        │   └─ Autopilot: QMetaObject::invokeMethod(extendAgentLoop)
                        └─ 无 tool_calls (终态):
                            m_agentBusy = false;
                            emit llmResponseReceived;
```

关键安全措施：
1. `m_agentBusy` 门锁 — 正在 loop 时拒收新消息
2. 入队前拷贝数据 — 所有传参都是值拷贝
3. 全程 `QueuedConnection` — 每步返回事件循环
4. `setTransferTimeout(30000)` — 网络超时不无限等
5. `keep-alive` — 复用 SSL 连接避免重复握手
