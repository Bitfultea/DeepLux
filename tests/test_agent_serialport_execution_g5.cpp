#include "core/agent/AgentActor.h"
#include "core/agent/AgentController.h"
#include "core/common/UserManager.h"
#include "core/agent/GuiEvent.h"
#include "core/engine/RunEngine.h"
#include "core/manager/PluginManager.h"
#include "core/manager/ProjectManager.h"
#include "core/model/Project.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWaitCondition>
#include <QtTest/QtTest>
#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif

using namespace DeepLux;

namespace {

#ifdef Q_OS_UNIX
class PtyEndpoint {
public:
    ~PtyEndpoint() {
        if (m_masterFd >= 0)
            ::close(m_masterFd);
    }

    bool open(QString* error) {
        m_masterFd = ::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (m_masterFd < 0)
            return fail(error, QStringLiteral("posix_openpt"));
        if (::grantpt(m_masterFd) != 0 || ::unlockpt(m_masterFd) != 0)
            return fail(error, QStringLiteral("grantpt/unlockpt"));
        const char* slave = ::ptsname(m_masterFd);
        if (!slave)
            return fail(error, QStringLiteral("ptsname"));
        m_portName = QString::fromLocal8Bit(slave);
        const QByteArray slaveBytes = m_portName.toLocal8Bit();
        const int slaveFd = ::open(slaveBytes.constData(), O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (slaveFd < 0)
            return fail(error, QStringLiteral("open PTY slave"));
        termios settings{};
        const bool configured = ::tcgetattr(slaveFd, &settings) == 0;
        if (configured) {
            ::cfmakeraw(&settings);
            ::tcsetattr(slaveFd, TCSANOW, &settings);
        }
        ::close(slaveFd);
        if (!configured)
            return fail(error, QStringLiteral("tcgetattr"));
        return true;
    }

    int masterFd() const {
        return m_masterFd;
    }

    QString portName() const {
        return m_portName;
    }

private:
    bool fail(QString* error, const QString& operation) const {
        if (error)
            *error = QStringLiteral("%1 failed: %2").arg(operation, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    int m_masterFd = -1;
    QString m_portName;
};

class PtyEchoPeer final : public QObject {
public:
    PtyEchoPeer(int masterFd, QByteArray request, QByteArray response, QObject* parent = nullptr)
        : QObject(parent), m_masterFd(masterFd), m_expectedRequest(std::move(request)),
          m_response(std::move(response)) {
        m_pollTimer.setInterval(2);
        connect(&m_pollTimer, &QTimer::timeout, this, [this] { pollMaster(); });
    }

    void start() {
        m_pollTimer.start();
    }

    QByteArray received() const {
        return m_received;
    }

    int replyCount() const {
        return m_replyCount;
    }

private:
    void pollMaster() {
        char buffer[512];
        while (true) {
            const ssize_t count = ::read(m_masterFd, buffer, sizeof(buffer));
            if (count > 0) {
                m_received.append(buffer, static_cast<int>(count));
                continue;
            }
            if (count < 0 && errno == EINTR)
                continue;
            break;
        }
        if (m_replyCount == 0 && m_received.size() >= m_expectedRequest.size()) {
            if (m_received != m_expectedRequest)
                return;
            const ssize_t written = ::write(m_masterFd, m_response.constData(), static_cast<size_t>(m_response.size()));
            if (!m_response.isEmpty() && written == m_response.size())
                ++m_replyCount;
        }
    }

    int m_masterFd;
    QByteArray m_expectedRequest;
    QByteArray m_response;
    QByteArray m_received;
    int m_replyCount = 0;
    QTimer m_pollTimer;
};

class ThreadedPtyEchoPeer final {
public:
    ThreadedPtyEchoPeer(int masterFd, QByteArray request, QByteArray response)
        : m_masterFd(masterFd), m_expectedRequest(std::move(request)), m_response(std::move(response)) {}

    ~ThreadedPtyEchoPeer() {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.isRunning()) {
            m_thread.quit();
            if (!m_thread.wait(5000))
                qFatal("PTY peer worker did not stop within 5000 ms");
        }
        m_worker = nullptr;
    }

    void start() {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
        QMetaObject::invokeMethod(
            m_worker,
            [this] {
                {
                    QMutexLocker locker(&m_mutex);
                    m_armed = true;
                    m_condition.wakeAll();
                }
                QElapsedTimer deadline;
                deadline.start();
                while (!m_stop.load(std::memory_order_acquire) && deadline.elapsed() < 3000) {
                    pollfd descriptor{m_masterFd, POLLIN, 0};
                    const int ready = ::poll(&descriptor, 1, 50);
                    if (ready <= 0 || !(descriptor.revents & POLLIN))
                        continue;
                    char buffer[512];
                    const ssize_t count = ::read(m_masterFd, buffer, sizeof(buffer));
                    if (count < 0 && (errno == EAGAIN || errno == EINTR))
                        continue;
                    if (count <= 0)
                        break;
                    bool requestComplete = false;
                    {
                        QMutexLocker locker(&m_mutex);
                        m_received.append(buffer, static_cast<int>(count));
                        requestComplete = m_received.size() >= m_expectedRequest.size();
                        m_condition.wakeAll();
                    }
                    if (!requestComplete)
                        continue;

                    if (m_response.isEmpty())
                        continue;
                    if (received() != m_expectedRequest)
                        break;
                    int offset = 0;
                    while (offset < m_response.size() && !m_stop.load(std::memory_order_acquire) &&
                           deadline.elapsed() < 3000) {
                        const ssize_t written = ::write(m_masterFd, m_response.constData() + offset,
                                                        static_cast<size_t>(m_response.size() - offset));
                        if (written > 0) {
                            offset += static_cast<int>(written);
                            continue;
                        }
                        if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
                            pollfd writable{m_masterFd, POLLOUT, 0};
                            ::poll(&writable, 1, 50);
                            continue;
                        }
                        break;
                    }
                    QMutexLocker locker(&m_mutex);
                    if (offset == m_response.size())
                        m_replyCount = 1;
                    break;
                }
                {
                    QMutexLocker locker(&m_mutex);
                    m_finished = true;
                    m_condition.wakeAll();
                }
                m_thread.quit();
            },
            Qt::QueuedConnection);
    }

    bool waitUntilArmed(int timeoutMs) {
        QMutexLocker locker(&m_mutex);
        if (m_armed)
            return true;
        return m_condition.wait(&m_mutex, timeoutMs) && m_armed;
    }

    bool waitForReply(int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&m_mutex);
        while (!m_finished && timer.elapsed() < timeoutMs)
            m_condition.wait(&m_mutex, qMax(1, timeoutMs - static_cast<int>(timer.elapsed())));
        const bool complete = m_replyCount == 1;
        const bool finished = m_finished;
        locker.unlock();
        if (finished && !m_thread.wait(5000))
            qFatal("PTY echo worker did not join within 5000 ms");
        return complete;
    }

    bool waitForRequestAndNoReply(int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&m_mutex);
        while (!m_finished && m_received.size() < m_expectedRequest.size() && timer.elapsed() < timeoutMs)
            m_condition.wait(&m_mutex, qMax(1, timeoutMs - static_cast<int>(timer.elapsed())));
        if (m_received.size() < m_expectedRequest.size() || m_finished) {
            locker.unlock();
            return false;
        }

        // Keep draining after the first complete request through the module timeout and
        // a bounded quiet window. A second write must remain observable before we stop.
        QElapsedTimer quiet;
        quiet.start();
        qsizetype observedSize = m_received.size();
        while (!m_finished && quiet.elapsed() < 250 && timer.elapsed() < timeoutMs) {
            const int quietRemaining = 250 - static_cast<int>(quiet.elapsed());
            const int overallRemaining = timeoutMs - static_cast<int>(timer.elapsed());
            m_condition.wait(&m_mutex, qMax(1, qMin(quietRemaining, overallRemaining)));
            if (m_received.size() != observedSize) {
                observedSize = m_received.size();
                quiet.restart();
            }
        }
        const bool withinDeadline = quiet.elapsed() >= 250;
        m_stop.store(true, std::memory_order_release);
        const bool requestReceived = m_received == m_expectedRequest;
        const bool noReply = m_replyCount == 0;
        locker.unlock();
        if (!m_thread.wait(qMax(1, timeoutMs - static_cast<int>(timer.elapsed()))))
            return false;
        QMutexLocker finishedLocker(&m_mutex);
        return withinDeadline && m_finished && requestReceived && noReply;
    }

    QByteArray received() const {
        QMutexLocker locker(&m_mutex);
        return m_received;
    }

    int replyCount() const {
        QMutexLocker locker(&m_mutex);
        return m_replyCount;
    }

private:
    int m_masterFd;
    QByteArray m_expectedRequest;
    QByteArray m_response;
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    QThread m_thread;
    QObject* m_worker = nullptr;
    QByteArray m_received;
    int m_replyCount = 0;
    bool m_armed = false;
    bool m_finished = false;
    std::atomic_bool m_stop{false};
};

class ThreadedPtyCyclePeer final {
public:
    ThreadedPtyCyclePeer(int masterFd, QByteArray request, QByteArray response)
        : m_masterFd(masterFd), m_expectedRequest(std::move(request)), m_response(std::move(response)) {}

    ~ThreadedPtyCyclePeer() {
        m_stop.store(true, std::memory_order_release);
        if (m_thread.isRunning() && !m_thread.wait(5000))
            qFatal("PTY cycle peer worker did not stop within 5000 ms; PTY fd remains owned by endpoint");
        m_worker = nullptr;
    }

    void start() {
        m_worker = new QObject;
        m_worker->moveToThread(&m_thread);
        QObject::connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
        m_thread.start();
        QMetaObject::invokeMethod(
            m_worker,
            [this] {
                {
                    QMutexLocker locker(&m_mutex);
                    m_armed = true;
                    m_condition.wakeAll();
                }
                while (!m_stop.load(std::memory_order_acquire)) {
                    pollfd descriptor{m_masterFd, POLLIN, 0};
                    const int ready = ::poll(&descriptor, 1, 50);
                    if (ready < 0 && errno == EINTR)
                        continue;
                    if (ready <= 0)
                        continue;
                    if (!(descriptor.revents & POLLIN)) {
                        if (descriptor.revents & (POLLHUP | POLLERR | POLLNVAL))
                            std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }

                    char buffer[512];
                    const ssize_t count = ::read(m_masterFd, buffer, sizeof(buffer));
                    if (count < 0 && (errno == EAGAIN || errno == EINTR))
                        continue;
                    if (count < 0 && errno == EIO) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                        continue;
                    }
                    if (count <= 0)
                        continue;

                    QList<QByteArray> frames;
                    {
                        QMutexLocker locker(&m_mutex);
                        const QByteArray bytes(buffer, static_cast<int>(count));
                        m_receivedByteCount += static_cast<quint64>(bytes.size());
                        m_stream.append(bytes);
                        m_pending.append(bytes);
                        while (true) {
                            const qsizetype delimiter = m_pending.indexOf('\n');
                            if (delimiter < 0)
                                break;
                            const QByteArray frame = m_pending.left(delimiter + 1);
                            m_pending.remove(0, delimiter + 1);
                            if (frame != m_expectedRequest)
                                m_invalidFrame = true;
                            else {
                                m_requests.append(frame);
                                frames.append(frame);
                            }
                        }
                        m_condition.wakeAll();
                    }

                    for (qsizetype i = 0; i < frames.size(); ++i) {
                        int offset = 0;
                        QElapsedTimer deadline;
                        deadline.start();
                        while (offset < m_response.size() && !m_stop.load(std::memory_order_acquire) &&
                               deadline.elapsed() < 1000) {
                            const ssize_t written = ::write(m_masterFd, m_response.constData() + offset,
                                                            static_cast<size_t>(m_response.size() - offset));
                            if (written > 0) {
                                offset += static_cast<int>(written);
                                continue;
                            }
                            if (written < 0 && (errno == EAGAIN || errno == EINTR)) {
                                pollfd writable{m_masterFd, POLLOUT, 0};
                                ::poll(&writable, 1, 50);
                                continue;
                            }
                            break;
                        }
                        QMutexLocker locker(&m_mutex);
                        if (offset == m_response.size())
                            m_responses.append(m_response);
                        else
                            m_invalidFrame = true;
                        m_condition.wakeAll();
                    }
                }
                QMutexLocker locker(&m_mutex);
                m_condition.wakeAll();
                m_thread.quit();
            },
            Qt::QueuedConnection);
    }

    bool waitUntilArmed(int timeoutMs) {
        QMutexLocker locker(&m_mutex);
        if (m_armed)
            return true;
        return m_condition.wait(&m_mutex, timeoutMs) && m_armed;
    }

    bool waitForCounts(int requests, int responses, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&m_mutex);
        while ((m_requests.size() < requests || m_responses.size() < responses) && timer.elapsed() < timeoutMs)
            m_condition.wait(&m_mutex, qMax(1, timeoutMs - static_cast<int>(timer.elapsed())));
        return m_requests.size() == requests && m_responses.size() == responses && !m_invalidFrame;
    }

    bool waitForQuiet(int requestCount, int responseCount, quint64 byteCount, int quietMs, int timeoutMs) {
        QElapsedTimer timer;
        timer.start();
        QElapsedTimer quiet;
        while (timer.elapsed() < timeoutMs) {
            {
                QMutexLocker locker(&m_mutex);
                if (m_requests.size() == requestCount && m_responses.size() == responseCount &&
                    m_receivedByteCount == byteCount && !m_invalidFrame) {
                    if (!quiet.isValid())
                        quiet.start();
                    if (quiet.elapsed() >= quietMs)
                        return true;
                } else {
                    quiet.invalidate();
                }
            }
            QTest::qWait(5);
        }
        return false;
    }

    QList<QByteArray> requests() const {
        QMutexLocker locker(&m_mutex);
        return m_requests;
    }
    QList<QByteArray> responses() const {
        QMutexLocker locker(&m_mutex);
        return m_responses;
    }
    QByteArray stream() const {
        QMutexLocker locker(&m_mutex);
        return m_stream;
    }
    quint64 receivedByteCount() const {
        QMutexLocker locker(&m_mutex);
        return m_receivedByteCount;
    }
    bool hasInvalidFrame() const {
        QMutexLocker locker(&m_mutex);
        return m_invalidFrame;
    }

private:
    int m_masterFd;
    QByteArray m_expectedRequest;
    QByteArray m_response;
    mutable QMutex m_mutex;
    QWaitCondition m_condition;
    QThread m_thread;
    QObject* m_worker = nullptr;
    QByteArray m_pending;
    QByteArray m_stream;
    QList<QByteArray> m_requests;
    QList<QByteArray> m_responses;
    quint64 m_receivedByteCount = 0;
    bool m_armed = false;
    bool m_invalidFrame = false;
    std::atomic_bool m_stop{false};
};
#endif

} // namespace

class TestAgentSerialPortExecutionG5 : public QObject {
    Q_OBJECT

private:
    struct EnvironmentRestore {
        QByteArray appDataDir = qgetenv("DEEPLUX_APP_DATA_DIR");
        bool hadAppDataDir = qEnvironmentVariableIsSet("DEEPLUX_APP_DATA_DIR");

        ~EnvironmentRestore() {
            if (hadAppDataDir)
                qputenv("DEEPLUX_APP_DATA_DIR", appDataDir);
            else
                qunsetenv("DEEPLUX_APP_DATA_DIR");
        }
    };

    EnvironmentRestore m_environmentRestore;
    QTemporaryDir m_appDataDir;

    static bool installSerialPortPlugin(const QString& pluginRoot) {
        const QDir pluginDir(QDir(pluginRoot).filePath(QStringLiteral("SerialPort")));
        if (!QDir().mkpath(pluginDir.absolutePath()))
            return false;
        return QFile::copy(QStringLiteral(AGENT_SERIALPORT_G5_METADATA),
                           pluginDir.filePath(QStringLiteral("metadata.json"))) &&
               QFile::copy(QStringLiteral(AGENT_SERIALPORT_G5_LIBRARY),
                           pluginDir.filePath(QFileInfo(QStringLiteral(AGENT_SERIALPORT_G5_LIBRARY)).fileName()));
    }

    static int moduleSignalCount(const QSignalSpy& spy, const QString& instanceId) {
        int count = 0;
        for (const QList<QVariant>& args : spy)
            if (!args.isEmpty() && args.first().toString() == instanceId)
                ++count;
        return count;
    }

    static ModuleInstance serialModule(const QString& instanceId, const QString& portName,
                                       const QString& operation = QStringLiteral("WriteRead"),
                                       const QString& writeData = QStringLiteral("CMD-7"), int timeout = 500) {
        ModuleInstance module;
        module.id = instanceId;
        module.moduleId = QStringLiteral("串口通信");
        module.name = QStringLiteral("Slice 52 PTY SerialPort");
        module.params = QJsonObject{{QStringLiteral("portName"), portName},
                                    {QStringLiteral("baudRate"), 115200},
                                    {QStringLiteral("dataBits"), 8},
                                    {QStringLiteral("parity"), QStringLiteral("None")},
                                    {QStringLiteral("stopBits"), QStringLiteral("One")},
                                    {QStringLiteral("flowControl"), QStringLiteral("None")},
                                    {QStringLiteral("timeout"), timeout},
                                    {QStringLiteral("writeData"), writeData},
                                    {QStringLiteral("readVariable"), QStringLiteral("agent_serial_reply")},
                                    {QStringLiteral("operation"), operation}};
        return module;
    }

private slots:
    void initTestCase() {
        UserManager::instance().login(QStringLiteral("developer"), QString());
        QVERIFY(m_appDataDir.isValid());
        qputenv("DEEPLUX_APP_DATA_DIR", m_appDataDir.path().toLocal8Bit());
        const QString pluginRoot = QDir(m_appDataDir.path()).filePath(QStringLiteral("plugins"));
        QVERIFY(installSerialPortPlugin(pluginRoot));

        PluginManager& plugins = PluginManager::instance();
        QVERIFY(plugins.initialize());
        QVERIFY(plugins.pluginPaths().contains(QDir(pluginRoot).absolutePath()));
        const QList<PluginInfo> scanned = plugins.moduleInfos();
        QCOMPARE(scanned.size(), 1);
        QCOMPARE(scanned.first().id, QStringLiteral("com.deeplux.plugin.serialport"));
        QVERIFY(plugins.cameraInfos().isEmpty());
        QVERIFY(plugins.loadPlugin(QStringLiteral("串口通信")));
        QVERIFY(AgentController::instance().initialize());
    }

    void cleanupTestCase() {
        RunEngine& engine = RunEngine::instance();
        engine.stop();
        QElapsedTimer wait;
        wait.start();
        while (engine.isBusy() && wait.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QTest::qWait(10);
        }
        if (engine.isBusy()) {
            m_appDataDir.setAutoRemove(false);
            qCritical() << "SerialPort execution did not become idle; preserving plugin directory"
                        << m_appDataDir.path();
            QFAIL("RunEngine remained busy; temporary SerialPort plugin tree was preserved");
            return;
        }
        engine.clearModules();
        ProjectManager::instance().closeProject();
        AgentController::instance().shutdown();
        PluginManager::instance().shutdown();
    }

    void testRunFlowRejectsAndExpiresSerialTargetThenReadsPTYReply() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        QString ptyError;
        PtyEndpoint oldPty;
        PtyEndpoint newPty;
        QVERIFY2(oldPty.open(&ptyError), qPrintable(ptyError));
        QVERIFY2(newPty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("CMD-7");
        const QByteArray reply = QStringLiteral("OK-✓").toUtf8();
        ThreadedPtyEchoPeer oldPeer(oldPty.masterFd(), request, reply);
        ThreadedPtyEchoPeer newPeer(newPty.masterFd(), request, reply);
        oldPeer.start();
        newPeer.start();
        QVERIFY(oldPeer.waitUntilArmed(1000));
        QVERIFY(newPeer.waitUntilArmed(1000));

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice52_serial_target");
        project->addModule(serialModule(instanceId, oldPty.portName()));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        std::atomic<quintptr> executionThreadId{0};
        const QMetaObject::Connection threadConnection = connect(
            &engine, &RunEngine::moduleStarted, &engine,
            [&executionThreadId, instanceId](const QString& startedId) {
                if (startedId == instanceId)
                    executionThreadId.store(reinterpret_cast<quintptr>(QThread::currentThreadId()),
                                            std::memory_order_relaxed);
            },
            Qt::DirectConnection);
        const quintptr mainThreadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
        auto startPending = [&controller] {
            return controller.handleToolCall(QStringLiteral("run_flow"),
                                             QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        };
        const auto expectedEffects = [instanceId](const QString& portName) {
            return QJsonArray{QStringLiteral("execution:may_touch_files_devices_network(流程内模块)"),
                              QStringLiteral("serial_port:%1@%2 baud=115200 data=8 parity=None stop=One flow=None "
                                             "operation=WriteRead timeout=500ms")
                                  .arg(instanceId, portName)};
        };

        QJsonObject pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray oldEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
        QCOMPARE(oldEffects, expectedEffects(oldPty.portName()));
        QVERIFY(!QString::fromUtf8(QJsonDocument(oldEffects).toJson()).contains(QStringLiteral("CMD-7")));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(oldPeer.received(), QByteArray());
        QCOMPARE(newPeer.received(), QByteArray());

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("portName"), newPty.portName()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(oldPeer.received(), QByteArray());
        QCOMPARE(newPeer.received(), QByteArray());

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QJsonArray newEffects = controller.pendingConfirmationPayload()
                                          .value(QStringLiteral("context"))
                                          .toObject()
                                          .value(QStringLiteral("sideEffects"))
                                          .toArray();
        QCOMPARE(newEffects, expectedEffects(newPty.portName()));
        QVERIFY(!QString::fromUtf8(QJsonDocument(newEffects).toJson()).contains(QStringLiteral("CMD-7")));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(started.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QVERIFY(newPeer.waitForReply(5000));
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());
        QCOMPARE(executionThreadId.load(std::memory_order_relaxed) == mainThreadId, false);
        QCOMPARE(oldPeer.received(), QByteArray());
        QCOMPARE(newPeer.received(), request);
        QCOMPARE(newPeer.replyCount(), 1);

        const QJsonObject received =
            controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                            QJsonObject{{QStringLiteral("runId"), runId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply")}});
        QCOMPARE(received.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(received.value(QStringLiteral("value")).toString(), QString::fromUtf8(reply));
        const QJsonObject receivedBytes = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply_bytes")}});
        QCOMPARE(receivedBytes.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(receivedBytes.value(QStringLiteral("value")).toInt(), reply.size());

        disconnect(threadConnection);
        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testWriteReadTimeoutIsUncertainAndPollingDoesNotRetry() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        QString ptyError;
        PtyEndpoint pty;
        QVERIFY2(pty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("CMD-53");
        PtyEchoPeer peer(pty.masterFd(), request, {});
        peer.start();

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice53_write_read_timeout");
        project->addModule(
            serialModule(instanceId, pty.portName(), QStringLiteral("WriteRead"), QString::fromUtf8(request), 100));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject started = toolExecuted.last().at(1).toJsonObject();
        const QString runId = started.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);

        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QVERIFY(!failed.success);
        QCOMPARE(failed.runId, runId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(peer.received(), request);
        QCOMPARE(peer.replyCount(), 0);

        const QJsonObject results = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                    QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(results.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(results.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!results.value(QStringLiteral("retryable")).toBool());
        for (const QString& key : {QStringLiteral("agent_serial_reply"), QStringLiteral("agent_serial_reply_bytes")}) {
            const QJsonObject output = controller.actor()->executeTool(
                QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                                 {QStringLiteral("instanceId"), instanceId},
                                                                 {QStringLiteral("key"), key}});
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!output.contains(QStringLiteral("value")));
        }
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(moduleStarted.count(), 1);
        QCOMPARE(peer.received(), request);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testStepOnceWriteTimeoutIsUncertainAndDoesNotRetry() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        QString ptyError;
        PtyEndpoint pty;
        QVERIFY2(pty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("slice80-step-uncertain");
        ThreadedPtyEchoPeer peer(pty.masterFd(), request, {});
        peer.start();
        QVERIFY(peer.waitUntilArmed(1000));

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice80_serial_step_uncertain");
        project->addModule(
            serialModule(instanceId, pty.portName(), QStringLiteral("WriteRead"), QString::fromUtf8(request), 100));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        const QString targetEffect =
            QStringLiteral("serial_port:%1@%2 baud=115200 data=8 parity=None stop=One flow=None "
                           "operation=WriteRead timeout=100ms")
                .arg(instanceId, pty.portName());

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QVERIFY(!engine.isBusy());
        QVERIFY(!engine.isPaused());
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(peer.replyCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        const QString runId = failed.runId;
        QVERIFY(!runId.isEmpty());
        QVERIFY(peer.waitForRequestAndNoReply(3000));
        QVERIFY(!engine.isBusy());
        QCOMPARE(failed.runId, runId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(!failed.diagnostics.isEmpty());
        QVERIFY(failed.diagnostics.contains(QStringLiteral("超时")));
        QCOMPARE(peer.received(), request);
        QCOMPARE(peer.replyCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject uncertain = controller.actor()->executeTool(QStringLiteral("get_run_results"),
                                                                      QJsonObject{{QStringLiteral("runId"), runId}});
        QCOMPARE(uncertain.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(uncertain.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!uncertain.value(QStringLiteral("retryable")).toBool());
        for (const QString& key : {QStringLiteral("agent_serial_reply"), QStringLiteral("agent_serial_reply_bytes")}) {
            const QJsonObject output = controller.actor()->executeTool(
                QStringLiteral("get_module_output"), QJsonObject{{QStringLiteral("runId"), runId},
                                                                 {QStringLiteral("instanceId"), instanceId},
                                                                 {QStringLiteral("key"), key}});
            QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
            QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
            QVERIFY(!output.contains(QStringLiteral("value")));
        }
        QCOMPARE(controller.actor()
                     ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), runId}})
                     .value(QStringLiteral("status"))
                     .toString(),
                 QStringLiteral("uncertain"));
        QCOMPARE(peer.received(), request);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testResumeFlowWriteTimeoutIsUncertainAndDoesNotRetry() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        QString ptyError;
        PtyEndpoint pty;
        QVERIFY2(pty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("slice80-resume-uncertain");
        ThreadedPtyEchoPeer peer(pty.masterFd(), request, {});
        peer.start();
        QVERIFY(peer.waitUntilArmed(1000));

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice80_serial_resume_uncertain");
        ModuleInstance serial =
            serialModule(instanceId, pty.portName(), QStringLiteral("WriteRead"), QString::fromUtf8(request), 100);
        serial.breakpoint = true;
        project->addModule(serial);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        const QString targetEffect =
            QStringLiteral("serial_port:%1@%2 baud=115200 data=8 parity=None stop=One flow=None "
                           "operation=WriteRead timeout=100ms")
                .arg(instanceId, pty.portName());

        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString pausedRunId = engine.runObservation().runId;
        QVERIFY(!pausedRunId.isEmpty());
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        auto startPending = [&controller] { return controller.handleToolCall(QStringLiteral("resume_flow"), {}); };
        QJsonObject pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, pausedRunId);
        QCOMPARE(peer.received(), QByteArray());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = startPending();
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("resumed"));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QVERIFY(peer.waitForRequestAndNoReply(3000));
        QTRY_COMPARE_WITH_TIMEOUT(engine.state(), RunState::Idle, 5000);
        QVERIFY(!engine.isBusy());
        const RunResult failed = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(failed.runId, pausedRunId);
        QCOMPARE(failed.errorCode, ExecError::SideEffectUncertain);
        QCOMPARE(failed.failedModule, instanceId);
        QVERIFY(!failed.diagnostics.isEmpty());
        QVERIFY(failed.diagnostics.contains(QStringLiteral("超时")));
        QCOMPARE(peer.received(), request);
        QCOMPARE(peer.replyCount(), 0);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(!moduleFinished.last().at(1).toBool());

        const QJsonObject uncertain = controller.actor()->executeTool(
            QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), pausedRunId}});
        QCOMPARE(uncertain.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QCOMPARE(uncertain.value(QStringLiteral("sideEffectStatus")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!uncertain.value(QStringLiteral("retryable")).toBool());
        const QJsonObject output =
            controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                            QJsonObject{{QStringLiteral("runId"), pausedRunId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply")}});
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("uncertain"));
        QVERIFY(!output.value(QStringLiteral("retryable")).toBool());
        QVERIFY(!output.contains(QStringLiteral("value")));
        QCOMPARE(
            controller.actor()
                ->executeTool(QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), pausedRunId}})
                .value(QStringLiteral("status"))
                .toString(),
            QStringLiteral("uncertain"));
        QCOMPARE(peer.received(), request);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testOneWayWriteSucceedsWithoutResponseAndReadTimeoutIsDefiniteFailure() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();

        QString ptyError;
        PtyEndpoint writePty;
        QVERIFY2(writePty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("ONE-WAY");
        PtyEchoPeer writePeer(writePty.masterFd(), request, {});
        writePeer.start();

        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString writeInstanceId = QStringLiteral("slice53_one_way_write");
        project->addModule(serialModule(writeInstanceId, writePty.portName(), QStringLiteral("Write"),
                                        QString::fromUtf8(request), 100));
        QSignalSpy writeFinished(&engine, &RunEngine::runFinished);
        QSignalSpy writeExecuted(controller.actor(), &AgentActor::toolExecuted);
        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                                        QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(writeExecuted.count(), 1, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(writeFinished.count(), 1, 5000);
        const RunResult writeResult = qvariant_cast<RunResult>(writeFinished.last().at(0));
        QVERIFY2(writeResult.success, qPrintable(writeResult.errorMessage));
        QTRY_COMPARE_WITH_TIMEOUT(writePeer.received(), request, 1000);
        QCOMPARE(writePeer.replyCount(), 0);

        engine.clearModules();
        ProjectManager::instance().closeProject();
        QString readPtyError;
        PtyEndpoint readPty;
        QVERIFY2(readPty.open(&readPtyError), qPrintable(readPtyError));
        PtyEchoPeer readPeer(readPty.masterFd(), QByteArrayLiteral("NO-WRITE"), {});
        readPeer.start();
        project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString readInstanceId = QStringLiteral("slice53_read_timeout");
        project->addModule(serialModule(readInstanceId, readPty.portName(), QStringLiteral("Read"), QString(), 50));
        QSignalSpy readFinished(&engine, &RunEngine::runFinished);
        QSignalSpy readExecuted(controller.actor(), &AgentActor::toolExecuted);
        pending = controller.handleToolCall(QStringLiteral("run_flow"),
                                            QJsonObject{{QStringLiteral("mode"), QStringLiteral("once")}, {QStringLiteral("wait"), false}});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.confirmPendingTools(controller.pendingConfirmationId()));
        QTRY_COMPARE_WITH_TIMEOUT(readExecuted.count(), 1, 5000);
        const QString readRunId = readExecuted.last().at(1).toJsonObject().value(QStringLiteral("runId")).toString();
        QVERIFY(!readRunId.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(readFinished.count(), 1, 5000);
        const RunResult readResult = qvariant_cast<RunResult>(readFinished.last().at(0));
        QVERIFY(!readResult.success);
        QCOMPARE(readResult.errorCode, ExecError::Processing);
        QCOMPARE(readPeer.received(), QByteArray());
        const QJsonObject readStatus = controller.actor()->executeTool(
            QStringLiteral("get_run_results"), QJsonObject{{QStringLiteral("runId"), readRunId}});
        QCOMPARE(readStatus.value(QStringLiteral("status")).toString(), QStringLiteral("failed"));
        QVERIFY(!readStatus.contains(QStringLiteral("retryable")) ||
                !readStatus.value(QStringLiteral("retryable")).toBool());
        QCOMPARE(readPeer.received(), QByteArray());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testStepOnceConfirmsSerialPortSingleExchange() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        QString ptyError;
        PtyEndpoint pty;
        QVERIFY2(pty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("slice75-step-request");
        const QByteArray reply = QStringLiteral("step-reply-✓").toUtf8();

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice75_serial_step");
        project->addModule(
            serialModule(instanceId, pty.portName(), QStringLiteral("WriteRead"), QString::fromUtf8(request), 1000));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        const QString targetEffect =
            QStringLiteral("serial_port:%1@%2 baud=115200 data=8 parity=None stop=One flow=None "
                           "operation=WriteRead timeout=1000ms")
                .arg(instanceId, pty.portName());

        QJsonObject pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("step_once"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        ThreadedPtyEchoPeer peer(pty.masterFd(), request, reply);
        peer.start();
        QVERIFY(peer.waitUntilArmed(1000));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(toolExecuted.count(), 1, 5000);
        QVERIFY(peer.waitForReply(3000));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        const QJsonObject stepResult = toolExecuted.last().at(1).toJsonObject();
        QCOMPARE(stepResult.value(QStringLiteral("status")).toString(), QStringLiteral("started"));
        const QString runId = stepResult.value(QStringLiteral("runId")).toString();
        QVERIFY(!runId.isEmpty());
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(peer.received(), request);
        QCOMPARE(peer.replyCount(), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());

        const QJsonObject received =
            controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                            QJsonObject{{QStringLiteral("runId"), runId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply")}});
        QCOMPARE(received.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(received.value(QStringLiteral("value")).toString(), QString::fromUtf8(reply));
        const QJsonObject receivedBytes = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply_bytes")}});
        QCOMPARE(receivedBytes.value(QStringLiteral("value")).toInt(), reply.size());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("step_once"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testResumeFlowConfirmsSerialPortSingleExchange() {
#ifndef Q_OS_UNIX
        QSKIP("POSIX PTY loopback is unavailable on this platform");
#else
        QString ptyError;
        PtyEndpoint pty;
        QVERIFY2(pty.open(&ptyError), qPrintable(ptyError));
        const QByteArray request = QByteArrayLiteral("slice75-resume-request");
        const QByteArray reply = QStringLiteral("resume-reply-✓").toUtf8();

        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);
        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice75_serial_resume");
        ModuleInstance serial =
            serialModule(instanceId, pty.portName(), QStringLiteral("WriteRead"), QString::fromUtf8(request), 1000);
        serial.breakpoint = true;
        project->addModule(serial);
        QVERIFY(engine.loadProject(project));

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        engine.runOnce();
        QCOMPARE(engine.state(), RunState::Paused);
        const QString runId = engine.runObservation().runId;
        QVERIFY(!runId.isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);

        const QString targetEffect =
            QStringLiteral("serial_port:%1@%2 baud=115200 data=8 parity=None stop=One flow=None "
                           "operation=WriteRead timeout=1000ms")
                .arg(instanceId, pty.portName());
        QJsonObject pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        const QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(engine.state(), RunState::Paused);
        QCOMPARE(engine.runObservation().runId, runId);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        pending = controller.handleToolCall(QStringLiteral("resume_flow"), {});
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QVERIFY(controller.pendingConfirmationPayload()
                    .value(QStringLiteral("context"))
                    .toObject()
                    .value(QStringLiteral("sideEffects"))
                    .toArray()
                    .contains(targetEffect));
        ThreadedPtyEchoPeer peer(pty.masterFd(), request, reply);
        peer.start();
        QVERIFY(peer.waitUntilArmed(1000));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != rejectedId);
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(runFinished.count(), 1, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!engine.isBusy(), 5000);
        const RunResult result = qvariant_cast<RunResult>(runFinished.last().at(0));
        QCOMPARE(result.runId, runId);
        QVERIFY2(result.success,
                 qPrintable(QStringLiteral("%1; peer received=%2 bytes replies=%3")
                                .arg(result.errorMessage)
                                .arg(peer.received().size())
                                .arg(peer.replyCount())));
        QVERIFY(peer.waitForReply(3000));
        QCOMPARE(peer.received(), request);
        QCOMPARE(peer.replyCount(), 1);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 1);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 1);
        QVERIFY(moduleFinished.last().at(1).toBool());

        const QJsonObject received =
            controller.actor()->executeTool(QStringLiteral("get_module_output"),
                                            QJsonObject{{QStringLiteral("runId"), runId},
                                                        {QStringLiteral("instanceId"), instanceId},
                                                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply")}});
        QCOMPARE(received.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(received.value(QStringLiteral("value")).toString(), QString::fromUtf8(reply));
        const QJsonObject receivedBytes = controller.actor()->executeTool(
            QStringLiteral("get_module_output"),
            QJsonObject{{QStringLiteral("runId"), runId},
                        {QStringLiteral("instanceId"), instanceId},
                        {QStringLiteral("key"), QStringLiteral("agent_serial_reply_bytes")}});
        QCOMPARE(receivedBytes.value(QStringLiteral("value")).toInt(), reply.size());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().action, QStringLiteral("resume_flow"));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));
        QVERIFY(!audit.last().at(0).value<AgentActionLogEntry>().undoable);

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }

    void testBoundedCycleWriteReadConfirmsDriftAndCompletesExactCount() {
#ifndef Q_OS_UNIX
        QSKIP("Slice97 requires a POSIX pseudo-terminal");
#else
        AgentController& controller = AgentController::instance();
        RunEngine& engine = RunEngine::instance();
        ProjectManager::instance().closeProject();
        engine.stop();
        engine.clearModules();
        controller.clearConversation();
        controller.setPermissionLevel(AgentController::PermissionLevel::Autopilot);

        constexpr int maxCycles = 3;
        constexpr int timeoutMs = 500;
        const QByteArray request = QByteArrayLiteral("SLICE97-CYCLE\n");
        const QByteArray responseBytes = QByteArrayLiteral("ACK-SLICE97\n");
        PtyEndpoint stalePty;
        PtyEndpoint approvedPty;
        QString ptyError;
        QVERIFY2(stalePty.open(&ptyError), qPrintable(ptyError));
        QVERIFY2(approvedPty.open(&ptyError), qPrintable(ptyError));
        ThreadedPtyCyclePeer stalePeer(stalePty.masterFd(), request, responseBytes);
        ThreadedPtyCyclePeer approvedPeer(approvedPty.masterFd(), request, responseBytes);
        stalePeer.start();
        approvedPeer.start();
        QVERIFY(stalePeer.waitUntilArmed(1000));
        QVERIFY(approvedPeer.waitUntilArmed(1000));

        Project* project = ProjectManager::instance().newProject();
        QVERIFY(project);
        const QString instanceId = QStringLiteral("slice97_serial_cycle");
        ModuleInstance serial = serialModule(instanceId, stalePty.portName(), QStringLiteral("WriteRead"),
                                             QString::fromUtf8(request), timeoutMs);
        serial.name = QStringLiteral("Slice 97 bounded SerialPort WriteRead");
        project->addModule(serial);

        QSignalSpy moduleStarted(&engine, &RunEngine::moduleStarted);
        QSignalSpy moduleFinished(&engine, &RunEngine::moduleFinished);
        QSignalSpy runFinished(&engine, &RunEngine::runFinished);
        QSignalSpy cycleStopped(&engine, &RunEngine::cycleStopped);
        QSignalSpy toolExecuted(controller.actor(), &AgentActor::toolExecuted);
        QSignalSpy audit(&controller, &AgentController::actionLogEntryAdded);
        QString sessionId;
        QObject resultContext;
        connect(controller.actor(), &AgentActor::toolExecuted, &resultContext,
                [&sessionId](const QString& toolName, const QJsonObject& result) {
                    if (toolName == QLatin1String("run_flow"))
                        sessionId = result.value(QStringLiteral("sessionId")).toString();
                });
        const QJsonObject cycleParams{{QStringLiteral("mode"), QStringLiteral("cycle")},
                                      {QStringLiteral("maxCycles"), maxCycles}};
        const QString label = QStringLiteral("不可撤销：Agent Undo 不会回滚外部副作用");
        const auto serialEffect = [instanceId, timeoutMs](const QString& portName) {
            return QStringLiteral("serial_port:%1@%2 baud=115200 data=8 parity=None stop=One flow=None "
                                  "operation=WriteRead timeout=%3ms")
                .arg(instanceId, portName)
                .arg(timeoutMs);
        };

        QJsonObject pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        QString rejectedId = controller.pendingConfirmationId();
        QVERIFY(!rejectedId.isEmpty());
        QJsonObject context =
            pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("context")).toObject();
        QCOMPARE(context.value(QStringLiteral("reversibilityLabel")).toString(), label);
        QJsonArray effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(QStringLiteral("cycle_session:maxCycles=3 interval=100ms")));
        QVERIFY(effects.contains(serialEffect(stalePty.portName())));
        QVERIFY(stalePeer.stream().isEmpty());
        QVERIFY(approvedPeer.stream().isEmpty());
        QVERIFY(controller.rejectPendingTools(rejectedId));
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("rejected"));
        QVERIFY(stalePeer.stream().isEmpty());
        QVERIFY(approvedPeer.stream().isEmpty());
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString staleId = controller.pendingConfirmationId();
        QVERIFY(!staleId.isEmpty());
        QVERIFY(staleId != rejectedId);
        QVERIFY(project->setModuleParam(instanceId, QStringLiteral("portName"), approvedPty.portName()));
        const int auditCountBeforeStaleConfirmation = audit.count();
        QVERIFY(controller.confirmPendingTools(staleId));
        QTRY_VERIFY_WITH_TIMEOUT(audit.count() > auditCountBeforeStaleConfirmation, 5000);
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("expired"));
        QVERIFY(stalePeer.stream().isEmpty());
        QVERIFY(approvedPeer.stream().isEmpty());
        QCOMPARE(stalePeer.receivedByteCount(), quint64(0));
        QCOMPARE(approvedPeer.receivedByteCount(), quint64(0));
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), 0);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), 0);
        QCOMPARE(runFinished.count(), 0);
        QCOMPARE(cycleStopped.count(), 0);
        QCOMPARE(toolExecuted.count(), 0);

        pending = controller.handleToolCall(QStringLiteral("run_flow"), cycleParams);
        QCOMPARE(pending.value(QStringLiteral("status")).toString(), QStringLiteral("pending_confirmation"));
        const QString approvedId = controller.pendingConfirmationId();
        QVERIFY(!approvedId.isEmpty());
        QVERIFY(approvedId != staleId);
        context = pending.value(QStringLiteral("confirmation")).toObject().value(QStringLiteral("context")).toObject();
        effects = context.value(QStringLiteral("sideEffects")).toArray();
        QVERIFY(effects.contains(serialEffect(approvedPty.portName())));
        QVERIFY(controller.confirmPendingTools(approvedId));
        QTRY_COMPARE_WITH_TIMEOUT(cycleStopped.count(), 1, 10000);
        QCOMPARE(toolExecuted.count(), 1);
        QCOMPARE(toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("status")).toString(),
                 QStringLiteral("started"));
        sessionId = toolExecuted.last().at(1).toJsonObject().value(QStringLiteral("sessionId")).toString();
        QVERIFY(!sessionId.isEmpty());
        QVERIFY(approvedPeer.waitForCounts(maxCycles, maxCycles, 3000));
        QCOMPARE(stalePeer.stream(), QByteArray());
        QCOMPARE(approvedPeer.requests(), (QList<QByteArray>{request, request, request}));
        QCOMPARE(approvedPeer.responses(), (QList<QByteArray>{responseBytes, responseBytes, responseBytes}));
        QCOMPARE(approvedPeer.stream(), request + request + request);
        QVERIFY(!approvedPeer.hasInvalidFrame());
        QCOMPARE(runFinished.count(), maxCycles);
        QCOMPARE(moduleSignalCount(moduleStarted, instanceId), maxCycles);
        QCOMPARE(moduleSignalCount(moduleFinished, instanceId), maxCycles);
        for (const QList<QVariant>& args : runFinished) {
            const RunResult run = qvariant_cast<RunResult>(args.at(0));
            QVERIFY2(run.success, qPrintable(run.errorMessage));
        }

        QJsonObject session = controller.handleToolCall(QStringLiteral("get_run_results"),
                                                        QJsonObject{{QStringLiteral("runId"), sessionId}});
        QCOMPARE(session.value(QStringLiteral("status")).toString(), QStringLiteral("completed"));
        QVERIFY(!session.value(QStringLiteral("isBusy")).toBool());
        QCOMPARE(session.value(QStringLiteral("maxCycles")).toInt(), maxCycles);
        QCOMPARE(session.value(QStringLiteral("completedCycles")).toInt(), maxCycles);
        const QString lastRunId = qvariant_cast<RunResult>(runFinished.last().at(0)).runId;
        QCOMPARE(session.value(QStringLiteral("lastSuccessfulRunId")).toString(), lastRunId);
        QCOMPARE(session.value(QStringLiteral("lastCompletedRunId")).toString(), lastRunId);
        const auto queryOutput = [&controller, &lastRunId, &instanceId](const QString& key) {
            return controller.handleToolCall(QStringLiteral("get_module_output"),
                                             QJsonObject{{QStringLiteral("runId"), lastRunId},
                                                         {QStringLiteral("instanceId"), instanceId},
                                                         {QStringLiteral("key"), key}});
        };
        const QJsonObject output = queryOutput(QStringLiteral("agent_serial_reply"));
        QCOMPARE(output.value(QStringLiteral("status")).toString(), QStringLiteral("available"));
        QCOMPARE(output.value(QStringLiteral("value")).toString(), QString::fromUtf8(responseBytes));
        const QJsonObject outputBytes = queryOutput(QStringLiteral("agent_serial_reply_bytes"));
        QCOMPARE(outputBytes.value(QStringLiteral("value")).toInt(), responseBytes.size());
        QCOMPARE(audit.last().at(0).value<AgentActionLogEntry>().result, QStringLiteral("success"));

        const quint64 expectedBytes = static_cast<quint64>(maxCycles * request.size());
        QVERIFY(approvedPeer.waitForQuiet(maxCycles, maxCycles, expectedBytes, 550, 2000));
        QCOMPARE(approvedPeer.receivedByteCount(), expectedBytes);
        QCOMPARE(queryOutput(QStringLiteral("agent_serial_reply")).value(QStringLiteral("value")).toString(),
                 QString::fromUtf8(responseBytes));
        QCOMPARE(approvedPeer.receivedByteCount(), expectedBytes);
        QCOMPARE(approvedPeer.requests().size(), maxCycles);
        QCOMPARE(approvedPeer.responses().size(), maxCycles);
        QCOMPARE(runFinished.count(), maxCycles);
        QCOMPARE(cycleStopped.count(), 1);
        QVERIFY(!engine.isBusy());

        controller.setPermissionLevel(AgentController::PermissionLevel::Advisor);
        engine.clearModules();
        ProjectManager::instance().closeProject();
#endif
    }
};

QTEST_MAIN(TestAgentSerialPortExecutionG5)
#include "test_agent_serialport_execution_g5.moc"
