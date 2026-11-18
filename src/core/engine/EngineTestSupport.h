#pragma once

#include "FlowExecutionService.h"
#include "RunEngine.h"

#include <mutex>

namespace DeepLux {

#ifdef DEEPLUX_BUILD_TESTS
DEEPLUX_API void resetParallelPoolStartCountForTest();
DEEPLUX_API int parallelPoolStartCountForTest();
#endif

class RunEngineTestAccess {
public:
    explicit RunEngineTestAccess(RunEngine& engine) : m_engine(engine) {}

    void setNodePortOutputsForTest(const QString& moduleName, const PortValueMap& outputs);
    QStringList executionOrderForTest() const;
    QList<ModuleConnection> controlEdgesForTest() const;
    QVariantMap loopMetadataForTest() const;
    void setModuleExecutionAffinityForTest(const QString& moduleName, ExecutionAffinity affinity);
    bool isParallelEligibleForTest(const QString& moduleName) const;
    void setFinalizeHookForTest(std::function<void()> hook);
    void clearFinalizeHookForTest();
    void setBeforeFinalDisplayFrameHookForTest(std::function<void()> hook);
    void clearBeforeFinalDisplayFrameHookForTest();
    void setExecutionFaultInjectionForTest(std::function<void()> hook);
    void clearExecutionFaultInjectionForTest();
    bool isExternalJobActiveForTest() const;
    void setBeforeJobStartHookForTest(std::function<void()> hook);
    void clearBeforeJobStartHookForTest();
    void setGuiDispatchTimeoutForTest(int msecs);
    QString lastGuiDispatchErrorCodeForTest() const;
    int guiDispatchLateCallbackDropCountForTest() const;
    int guiDispatchTimeoutHitCountForTest() const;
    int guiDispatchRunningWaitEnteredCountForTest() const;
    void setBeforeCompletedSnapshotClearHookForTest(std::function<void()> hook);
    void clearBeforeCompletedSnapshotClearHookForTest();
    std::unique_lock<QMutex> lockRunObservationForTest();
    bool hasCompletedOutputSnapshotForTest() const;
    void resetParallelPoolStartCountForTest();
    int parallelPoolStartCountForTest() const;

private:
    RunEngine& m_engine;
};

class FlowExecutionServiceTestAccess {
public:
    explicit FlowExecutionServiceTestAccess(FlowExecutionService& service) : m_service(service) {}

    void setAfterStartSettledHookForTest(std::function<void()> hook);
    void clearAfterStartSettledHookForTest();
    void setStartHandshakeHookForTest(std::function<void()> hook);
    void clearStartHandshakeHookForTest();
    void setShutdownDeadlineForTest(int msecs);
    int shutdownDeadlineHitCountForTest() const;
    void setWorkerExitBarrierForTest(std::function<void()> hook);
    void clearWorkerExitBarrierForTest();
    void setJobTeardownHookForTest(std::function<void()> hook);
    void clearJobTeardownHookForTest();
    void setWorkerLoopFaultHookForTest(std::function<void()> hook);
    void clearWorkerLoopFaultHookForTest();
    void setReclaimWaitHookForTest(std::function<void()> hook);
    void clearReclaimWaitHookForTest();
    int activeReclaimBorrowCountForTest() const;
    void setInCriticalSectionHookForTest(std::function<void()> hook);
    void clearInCriticalSectionHookForTest();
    int inflightWithoutBorrowCountForTest() const;
    int liveReclaimGuardCountForTest() const;

private:
    FlowExecutionService& m_service;
};

} // namespace DeepLux
