#include "AgentRunWaiter.h"

#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <optional>

namespace DeepLux {

namespace {
QMutex g_waiterMutex;
QSet<const QThread*> g_waitingThreads;

// 测试注入：仅测试构建可写；生产构建该符号不可达（DEEPLUX_TEST_VISIBILITY=private）
std::optional<AgentRunWaiter::Outcome> g_nextOutcomeForTest;

/// waitForRun 退出时（含异常路径）把当前线程移出等待表
class WaitingThreadGuard {
public:
    WaitingThreadGuard() {
        QMutexLocker locker(&g_waiterMutex);
        g_waitingThreads.insert(QThread::currentThread());
    }
    ~WaitingThreadGuard() {
        QMutexLocker locker(&g_waiterMutex);
        g_waitingThreads.remove(QThread::currentThread());
    }
    WaitingThreadGuard(const WaitingThreadGuard&) = delete;
    WaitingThreadGuard& operator=(const WaitingThreadGuard&) = delete;
};
} // namespace

void AgentRunWaiter::setNextOutcomeForTest(const Outcome& outcome) {
    g_nextOutcomeForTest = outcome;
}

void AgentRunWaiter::clearOutcomeForTest() {
    g_nextOutcomeForTest.reset();
}

AgentRunWaiter::AgentRunWaiter(QObject* parent) : QObject(parent) {}

AgentRunWaiter::~AgentRunWaiter() {
    // 防御性：栈对象正常不会在等待中析构
    if (m_loop)
        m_loop->quit();
}

int AgentRunWaiter::clampTimeoutMs(int timeoutMs) {
    return qBound(1000, timeoutMs, 300000);
}

void AgentRunWaiter::abort() {
    m_aborted = true;
    if (m_loop)
        m_loop->quit();
}

void AgentRunWaiter::quitLoop() {
    if (m_loop)
        m_loop->quit();
}

AgentRunWaiter::Outcome AgentRunWaiter::waitForRun(RunEngine& engine, int timeoutMs,
                                                   const std::function<bool()>& start) {
    Outcome outcome;
    // L1 测试注入：直接返回调用方指定的 OutCome，不启动/等待运行
    if (g_nextOutcomeForTest) {
        outcome = *g_nextOutcomeForTest;
        g_nextOutcomeForTest.reset();
        if (outcome.timedOut)
            m_lastReason = QStringLiteral("timeout");
        else if (outcome.aborted)
            m_lastReason = QStringLiteral("aborted");
        else if (outcome.reentrantRejected)
            m_lastReason = QStringLiteral("reentrant");
        else if (outcome.pausedAtBreakpoint)
            m_lastReason = QStringLiteral("breakpoint");
        else if (outcome.rejectedStart)
            m_lastReason = QStringLiteral("rejected");
        else if (outcome.finished)
            m_lastReason = QStringLiteral("finished");
        return outcome;
    }
    if (!start) {
        outcome.rejectedStart = true;
        m_lastReason = QStringLiteral("rejected");
        return outcome;
    }

    QCoreApplication* app = QCoreApplication::instance();
    if (!app || app->thread() != QThread::currentThread()) {
        qWarning() << "[AgentRunWaiter] waitForRun must be called from the GUI thread";
        outcome.reentrantRejected = true;
        m_lastReason = QStringLiteral("reentrant");
        return outcome;
    }

    {
        QMutexLocker locker(&g_waiterMutex);
        if (g_waitingThreads.contains(QThread::currentThread())) {
            qWarning() << "[AgentRunWaiter] reentrant wait rejected on this thread";
            outcome.reentrantRejected = true;
            m_lastReason = QStringLiteral("reentrant");
            return outcome;
        }
    }

    // 进入等待前已被请求中止（abort() 在等待未激活时只置标志）
    if (m_aborted) {
        m_aborted = false;
        outcome.aborted = true;
        m_lastReason = QStringLiteral("aborted");
        return outcome;
    }

    WaitingThreadGuard guard;
    m_active = true;
    m_aborted = false;
    m_timedOut = false;
    m_finished = false;
    m_pausedAtBreakpoint = false;
    m_result = RunResult{};
    m_failedModule.clear();
    m_moduleFailures = 0;
    m_lastReason.clear();

    const QMetaObject::Connection finishedConn =
        connect(&engine, &RunEngine::runFinished, this, [this](const RunResult& result) {
            m_result = result;
            m_finished = true;
            quitLoop();
        });
    const QMetaObject::Connection moduleConn = connect(&engine, &RunEngine::moduleFinished, this,
                                                       [this](const QString& moduleId, bool success, int elapsedMs) {
                                                           Q_UNUSED(elapsedMs)
                                                           if (!success) {
                                                               if (m_failedModule.isEmpty())
                                                                   m_failedModule = moduleId;
                                                               ++m_moduleFailures;
                                                           }
                                                       });
    const QMetaObject::Connection breakpointConn =
        connect(&engine, &RunEngine::breakpointHit, this, [this](const QString& moduleId) {
            Q_UNUSED(moduleId)
            m_pausedAtBreakpoint = true;
            quitLoop();
        });

    // 先连接、先装事件循环与超时，再启动运行：
    // 若 runFinished 在 start() 内/紧随其后同步到达，m_finished 已置位，exec() 前会跳过等待，
    // 不会出现"quit 已发生但事件循环还没启动 → 永久挂到超时"的丢失信号竞态。
    QEventLoop loop;
    m_loop = &loop;
    QTimer::singleShot(clampTimeoutMs(timeoutMs), &loop, [this]() {
        m_timedOut = true;
        quitLoop();
    });

    if (!start()) {
        m_loop = nullptr;
        disconnect(finishedConn);
        disconnect(moduleConn);
        disconnect(breakpointConn);
        m_active = false;
        outcome.rejectedStart = true;
        m_lastReason = QStringLiteral("rejected");
        return outcome;
    }

    if (!m_finished && !m_aborted && !m_pausedAtBreakpoint && !m_timedOut)
        loop.exec();
    m_loop = nullptr;

    disconnect(finishedConn);
    disconnect(moduleConn);
    disconnect(breakpointConn);

    m_active = false;
    outcome.finished = m_finished;
    outcome.timedOut = m_timedOut;
    outcome.aborted = m_aborted;
    outcome.pausedAtBreakpoint = m_pausedAtBreakpoint;
    outcome.result = m_result;
    outcome.failedModule = m_failedModule;
    outcome.moduleFailures = m_moduleFailures;

    if (outcome.timedOut)
        m_lastReason = QStringLiteral("timeout");
    else if (outcome.aborted)
        m_lastReason = QStringLiteral("aborted");
    else if (outcome.pausedAtBreakpoint)
        m_lastReason = QStringLiteral("breakpoint");
    else if (outcome.finished)
        m_lastReason = QStringLiteral("finished");
    return outcome;
}

} // namespace DeepLux
