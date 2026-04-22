# DeepLux Bash 终端 + LLM Agent 集成设计方案

## Context

用户最初需求是让 GUI 和 CLI 双向同步操作，支持 LLM Agent 辅助操作。

**之前错误的设计**：当前实现的 TerminalWidget 只调用 CLIHandler 中注册的应用级命令，不能执行真正的 bash 命令，无法满足 LLM Agent 需求。

**正确设计**：使用真正的 bash 终端 (跨平台 PTY)，支持任意 shell 命令，并通过 AgentBridge 与 LLM Agent 通信。

**设计原则**：
- Linux 和 Windows 平台逻辑高度抽象分离
- 每层组件职责单一，通过接口通信
- 信号槽跨线程安全，显式指定连接类型
- 分阶段交付，每阶段可独立验证

## 需求总结

| 需求 | 解决方案 |
|------|---------|
| 真 bash 终端（跨平台） | BashProcess: Linux POSIX PTY, Windows ConPTY/winpty |
| GUI → 终端同步 | TerminalBridge 监听 GUI 信号，格式化后显示 |
| 终端 → GUI 反馈 | BashProcess 执行结果通过信号通知 GUI |
| LLM Agent 操作软件 | AgentBridge (Unix Domain Socket / Named Pipe) 接收 Agent 命令并转发到 bash |
| Agent 读取状态 | AgentBridge 提供 query 接口查询系统状态 |
| Agent 保活 | 心跳机制 (10s 间隔, 30s 超时断开 + 自动重连) |

## 架构设计

```
┌─────────────────────────────────────────────────────────────────┐
│                         MainWindow                               │
│  ┌──────────────────────────────────────────────────────────┐     │
│  │  QTabWidget: [📄 日志] [🖥️ 终端]                      │     │
│  └──────────────────────────────────────────────────────────┘     │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     TerminalWidget (QWidget)                     │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  QTextEdit - ANSI 彩色输出显示                           │   │
│  └──────────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  QLineEdit - 命令输入                                    │   │
│  └──────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       BashProcess (单例)                        │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  PtyImpl 抽象层 (平台差异封装)                           │   │
│  │  ├── LinuxPtyImpl (POSIX PTY)                          │   │
│  │  └── WindowsPtyImpl (ConPTY)                            │   │
│  └──────────────────────────────────────────────────────────┘   │
│  - 命令历史管理 (QReadWriteLock)                               │
│  - 输出节流 (50ms QTimer)                                     │
│  - bash 可用性检测                                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                    AgentBridge (单例)                           │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  AgentConnection (Unix socket / Named Pipe 封装)         │   │
│  └──────────────────────────────────────────────────────────┘   │
│  - JSON 协议 v1.0 + 心跳机制                                 │
│  - 注册式 query handler (支持扩展)                            │
│  - 断开重连流程                                               │
└─────────────────────────────────────────────────────────────────┘
```

## 核心组件

### 1. BashProcess (新增，跨平台)

**文件**: `src/ui/process/BashProcess.h`, `.cpp`

**平台抽象设计**：
```cpp
// 内部实现类，屏蔽平台差异
class PtyImpl {
public:
    virtual ~PtyImpl() = default;
    virtual bool start(const QString& shell, const QStringList& args) = 0;
    virtual void write(const QByteArray& data) = 0;
    virtual QByteArray read() = 0;
    virtual void resize(int cols, int rows) = 0;
    virtual void kill() = 0;
    virtual bool isRunning() const = 0;
signals:
    void outputReady(const QByteArray& data);
    void errorReady(const QByteArray& data);
    void finished(int exitCode);
    void errorOccurred(const QString& error);
};

class BashProcess : public QObject {
    Q_OBJECT
public:
    static BashProcess& instance();

    enum State { NotStarted, Starting, Running, Failed };
    enum Error { None, ShellNotFound, PtyOpenFailed, ForkFailed, ShellCrashed, Timeout };

    State state() const { return m_state; }
    Error lastError() const { return m_lastError; }

    Q_INVOKABLE void writeCommand(const QString& command);
    Q_INVOKABLE void writeRaw(const QString& data);

    QStringList history() const;
    void addToHistory(const QString& cmd);
    void clearHistory();

    void resize(int cols, int rows);

    static QString findAvailableShell();
    static bool isShellAvailable(const QString& shellPath);

signals:
    void stateChanged(State state) const;
    void outputReady(const QString& data) const;   // 已解码
    void errorReady(const QString& data) const;
    void commandFinished(int exitCode) const;
    void errorOccurred(Error error, const QString& details) const;

private:
    BashProcess(QObject* parent = nullptr);
    ~BashProcess() override;

    PtyImpl* m_impl = nullptr;
    State m_state = NotStarted;
    Error m_lastError = None;
    QString m_shellPath;
    QStringList m_commandHistory;
    QReadWriteLock m_historyLock;

    QTimer* m_outputThrottle = nullptr;
    QString m_pendingOutput;
};
```

**关键实现细节**：

1. **Linux POSIX PTY**:
```cpp
bool LinuxPtyImpl::start(const QString& shell, const QStringList& args) {
    m_masterFd = ::open("/dev/ptmx", O_RDWR | O_NOCTTY);
    grantpt(m_masterFd);
    unlockpt(m_masterFd);

    pid_t pid = fork();
    if (pid == 0) {
        int slaveFd = ::open(ptsname(m_masterFd), O_RDWR);
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);
        ::dup2(slaveFd, STDIN_FILENO);
        ::dup2(slaveFd, STDOUT_FILENO);
        ::dup2(slaveFd, STDERR_FILENO);
        execl(shell.toUtf8(), "bash", "--login", "-i", nullptr);
        _exit(1);
    }
    m_notifier = new QSocketNotifier(m_masterFd, QSocketNotifier::Read, this);
    return true;
}
```

2. **Windows ConPTY** (Win10 1809+):
```cpp
bool WindowsConPtyImpl::start(const QString& shell, const QStringList& args) {
    HRESULT hr = CreatePseudoConsole(..., &hPty);

    STARTUPINFOEX siex = {};
    siex.StartupInfo.cb = sizeof(STARTUPINFOEX);
    InitializeProcThreadAttributeList(nullptr, 1, 0, &pBuffer);
    UpdateProcThreadAttribute(&pBuffer, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                              hPty, sizeof(hPty), nullptr, nullptr);

    CreateProcessW(shellPath, ..., &siex.StartupInfo, ...);
}
```

3. **bash 可用性检测**:
```cpp
QString BashProcess::detectShell() {
#if defined(_WIN32)
    QStringList candidates = {"bash", "cmd.exe", "powershell.exe"};
#else
    QStringList candidates = {"/bin/bash", "/usr/bin/bash", "/bin/sh"};
#endif
    for (const QString& shell : candidates) {
        if (QFileInfo(shell).exists() && QFileInfo(shell).isExecutable()) {
            return shell;
        }
    }
    return QString();
}
```

### 2. TerminalWidget (重写)

**文件**: `src/ui/widgets/TerminalWidget.h`, `.cpp`

**关键功能**：
- 接收 BashProcess 信号 (`Qt::QueuedConnection`)
- ANSI X3.64 → HTML 着色
- 命令历史（上下键）
- Tab 补全
- 输出节流 (50ms)

### 3. TerminalBridge (增强)

**文件**: `src/ui/bridge/TerminalBridge.h`, `.cpp`

**GUI Action 命令映射**：

| GUI 操作 | bash 命令 |
|---------|----------|
| 新建方案 | `deeplux create-project %1` |
| 打开工程 | `deeplux open %1` |
| 保存工程 | `deeplux save` |
| 运行流程 | `deeplux run` |
| 停止流程 | `deeplux stop` |

### 4. AgentBridge (新增)

**文件**: `src/core/agent/AgentBridge.h`, `.cpp`

**平台抽象**：`QLocalServer` 自动适配 Unix Domain Socket (Linux) / Named Pipe (Windows)

**通信协议** (JSON v1.0):

```json
// 通用头部
{"version": "1.0", "type": "...", "id": "req-001"}

// Agent → DeepLux: 执行命令
{"type": "execute", "id": "req-001", "payload": {"command": "ls -la", "timeout": 30000}}

// Agent → DeepLux: 查询
{"type": "query", "id": "req-002", "payload": {"target": "project"}}

// Agent → DeepLux: 心跳
{"type": "ping"}

// DeepLux → Agent: 结果
{"type": "result", "id": "req-001", "payload": {"exitCode": 0, "stdout": "..."}}

// DeepLux → Agent: 主动事件
{"type": "event", "event": "run_started", "payload": {...}}

// DeepLux → Agent: 心跳响应
{"type": "pong"}
```

**心跳机制**：
- 10 秒发送一次 `{"type": "ping"}`
- 30 秒无响应 (3次 missed) 自动断开
- 断开后 5 秒自动尝试重连
- GUI 发出 `agentConnectionLost` 信号

**query 接口**（注册式）：
```cpp
AgentBridge::instance().registerQueryHandler("project", [](const QJsonObject& params) {
    Project* proj = ProjectManager::instance().currentProject();
    return QJsonObject{{"name", proj->name()}};
});
```

## 文件结构

```
src/
├── ui/
│   ├── process/                    (新增)
│   │   ├── BashProcess.h/cpp
│   │   ├── PtyImpl.h              (抽象基类)
│   │   ├── LinuxPtyImpl.cpp       (Linux 实现)
│   │   └── WindowsPtyImpl.cpp     (Windows 实现)
│   ├── widgets/
│   │   └── TerminalWidget.h/cpp   (重写)
│   ├── bridge/
│   │   └── TerminalBridge.h/cpp   (增强)
│   └── CMakeLists.txt
└── core/
    ├── agent/                      (新增)
    │   ├── AgentBridge.h/cpp
    │   └── AgentConnection.cpp
    └── CMakeLists.txt
```

## 实现步骤

### Phase 1: BashProcess + TerminalWidget + TerminalBridge (核心终端 + GUI同步)

1. **BashProcess** - PtyImpl 抽象 + Linux/Windows 实现
2. **TerminalWidget** - ANSI 着色 + 命令历史 + Tab 补全
3. **TerminalBridge** - GUI 同步 + deeplux wrapper

### Phase 2: AgentBridge (基础版)

1. AgentConnection 连接类
2. Unix socket / Named Pipe server
3. JSON 协议解析 + 心跳

### Phase 3: AgentBridge (完整版)

1. 注册式 query handler
2. 事件订阅 + 主动推送
3. 断开重连流程

### Phase 4: deeplux CLI 工具

1. 命令行入口
2. CLI 命令处理
3. wrapper 安装

## 设计改进（基于审查）

### 高优先级
1. **跨平台 PTY 抽象**: PtyImpl 基类 + Linux/Windows 实现分离
2. **Windows ConPTY 细化**: 明确 API 调用，降级处理
3. **Socket/Pipe 抽象**: AgentConnection 封装差异
4. **线程安全**: Qt::QueuedConnection + 50ms 节流

### 中优先级
5. **Phase 1+2 合并**: 避免 TerminalWidget 重写两次
6. **bash 检测**: detectShell() + 明确错误提示
7. **心跳恢复**: 断开 → 5s 重连 → agentConnectionLost 信号

### 平台差异

| 功能 | Linux | Windows |
|------|-------|---------|
| PTY 打开 | `open("/dev/ptmx")` | `CreatePseudoConsole()` |
| 进程创建 | `fork()` + `setsid()` | `CreateProcessW()` + `EXTENDED_STARTUPINFO_PRESENT` |
| Socket/Pipe | `QLocalServer` (Unix socket) | `QLocalServer` (Named Pipe) |
