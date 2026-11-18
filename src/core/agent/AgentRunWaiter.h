#ifndef DEEPLUX_AGENT_RUN_WAITER_H
#define DEEPLUX_AGENT_RUN_WAITER_H

#include "../engine/RunEngine.h"
#include "../platform/Platform.h"

#include <QObject>
#include <QString>
#include <functional>

class QEventLoop;

namespace DeepLux {

/**
 * @brief 工具侧的运行等待器：连接信号 → 启动运行 → 嵌套事件循环等待
 *
 * 只负责"等待"，不拥有运行、不修改 Agent 状态：
 * - 先连接 runFinished/moduleFinished/breakpointHit，再调用 start()，消除"运行在连接建立前
 *   就结束"的丢失信号竞态；
 * - 后台执行（设计 §8.2）：同时监听 FlowExecutionService 的 jobFinished/jobFailed 与
 *   pauseHit(User)，保证 job 启动失败或用户暂停时等待不会挂到超时；
 * - 超时/中止/断点/用户暂停均以事件循环退出返回，绝不在内部调用 stop()（由调用方决定）。
 */
class DEEPLUX_API AgentRunWaiter : public QObject {
    Q_OBJECT

public:
    struct Outcome {
        bool finished = false;          // runFinished 到达（或 Service jobFinished 收尾）
        bool timedOut = false;          // 等待超时
        bool aborted = false;           // 被 abort() 打断
        bool reentrantRejected = false; // 同线程重入 / 非 GUI 线程
        bool rejectedStart = false;     // start() 返回 false（服务/引擎忙）
        bool pausedAtBreakpoint = false;
        bool pausedByUser = false; // 用户暂停（Service pause_flow）
        bool startFailed = false;  // Service jobFailed（引擎拒绝启动）
        QString startError;
        RunResult result{};
        QString failedModule; // 等待期间首个 moduleFinished(success=false)
        int moduleFailures = 0;
    };

    explicit AgentRunWaiter(QObject* parent = nullptr);
    ~AgentRunWaiter() override;

    /**
     * @brief 启动一次运行并阻塞当前线程的嵌套事件循环，直到 runFinished / 超时 / abort() / 断点命中。
     * @param engine    运行引擎
     * @param timeoutMs 等待超时，clamp 到 [1000, 300000]
     * @param start     启动回调；返回 false 表示引擎拒绝启动（忙/停止中）
     */
    Outcome waitForRun(RunEngine& engine, int timeoutMs, const std::function<bool()>& start);

    /// 幂等；由 AgentActor::abortRunningTool 调用
    void abort();
    bool isActive() const {
        return m_active;
    }
    /// "finished" | "timeout" | "aborted" | "reentrant" | "breakpoint" | "rejected"
    QString lastReason() const {
        return m_lastReason;
    }

    // 测试注入：让下一次 waitForRun 直接返回指定 Outcome（生产为空，永不生效）
#ifdef DEEPLUX_BUILD_TESTS
    DEEPLUX_TEST_VISIBILITY : static void setNextOutcomeForTest(const Outcome& outcome);
    static void clearOutcomeForTest();
#endif

public:
    static int clampTimeoutMs(int timeoutMs);

private:
    void quitLoop();

    bool m_active = false;
    bool m_aborted = false;
    bool m_timedOut = false;
    bool m_finished = false;
    bool m_pausedAtBreakpoint = false;
    bool m_pausedByUser = false;
    bool m_startFailed = false;
    QString m_startError;
    RunResult m_result{};
    QString m_failedModule;
    int m_moduleFailures = 0;
    QString m_lastReason;
    QEventLoop* m_loop = nullptr;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_RUN_WAITER_H
