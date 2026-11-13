#include "EngineTestSupport.h"

#include "model/Project.h"

namespace DeepLux {

void RunEngineTestAccess::setNodePortOutputsForTest(const QString& moduleName, const PortValueMap& outputs) {
    m_engine.setNodePortOutputsForTest(moduleName, outputs);
}

QStringList RunEngineTestAccess::executionOrderForTest() const {
    return m_engine.executionOrderForTest();
}

QList<ModuleConnection> RunEngineTestAccess::controlEdgesForTest() const {
    return m_engine.controlEdgesForTest();
}

QVariantMap RunEngineTestAccess::loopMetadataForTest() const {
    return m_engine.loopMetadataForTest();
}

void RunEngineTestAccess::setModuleExecutionAffinityForTest(const QString& moduleName, ExecutionAffinity affinity) {
    m_engine.setModuleExecutionAffinityForTest(moduleName, affinity);
}

bool RunEngineTestAccess::isParallelEligibleForTest(const QString& moduleName) const {
    return m_engine.isParallelEligibleForTest(moduleName);
}

void RunEngineTestAccess::setFinalizeHookForTest(std::function<void()> hook) {
    m_engine.setFinalizeHookForTest(std::move(hook));
}

void RunEngineTestAccess::clearFinalizeHookForTest() {
    m_engine.clearFinalizeHookForTest();
}

void RunEngineTestAccess::setBeforeFinalDisplayFrameHookForTest(std::function<void()> hook) {
    m_engine.setBeforeFinalDisplayFrameHookForTest(std::move(hook));
}

void RunEngineTestAccess::clearBeforeFinalDisplayFrameHookForTest() {
    m_engine.clearBeforeFinalDisplayFrameHookForTest();
}

void RunEngineTestAccess::setExecutionFaultInjectionForTest(std::function<void()> hook) {
    m_engine.setExecutionFaultInjectionForTest(std::move(hook));
}

void RunEngineTestAccess::clearExecutionFaultInjectionForTest() {
    m_engine.clearExecutionFaultInjectionForTest();
}

bool RunEngineTestAccess::isExternalJobActiveForTest() const {
    return m_engine.isExternalJobActiveForTest();
}

void RunEngineTestAccess::setBeforeJobStartHookForTest(std::function<void()> hook) {
    m_engine.setBeforeJobStartHookForTest(std::move(hook));
}

void RunEngineTestAccess::clearBeforeJobStartHookForTest() {
    m_engine.clearBeforeJobStartHookForTest();
}

void RunEngineTestAccess::setGuiDispatchTimeoutForTest(int msecs) {
    m_engine.setGuiDispatchTimeoutForTest(msecs);
}

QString RunEngineTestAccess::lastGuiDispatchErrorCodeForTest() const {
    return m_engine.lastGuiDispatchErrorCodeForTest();
}

int RunEngineTestAccess::guiDispatchLateCallbackDropCountForTest() const {
    return m_engine.guiDispatchLateCallbackDropCountForTest();
}

int RunEngineTestAccess::guiDispatchTimeoutHitCountForTest() const {
    return m_engine.guiDispatchTimeoutHitCountForTest();
}

int RunEngineTestAccess::guiDispatchRunningWaitEnteredCountForTest() const {
    return m_engine.guiDispatchRunningWaitEnteredCountForTest();
}

void RunEngineTestAccess::setBeforeCompletedSnapshotClearHookForTest(std::function<void()> hook) {
    m_engine.m_testBeforeCompletedSnapshotClearHook = std::move(hook);
}

void RunEngineTestAccess::clearBeforeCompletedSnapshotClearHookForTest() {
    m_engine.m_testBeforeCompletedSnapshotClearHook = nullptr;
}

std::unique_lock<QMutex> RunEngineTestAccess::lockRunObservationForTest() {
    return std::unique_lock<QMutex>(m_engine.m_runObservationMutex);
}

bool RunEngineTestAccess::hasCompletedOutputSnapshotForTest() const {
    QMutexLocker valueLocker(&m_engine.m_outputMutex);
    QMutexLocker imageLocker(&m_engine.m_lastOutputMutex);
    return !m_engine.m_lastFinishedValues.isEmpty() || !m_engine.m_lastFinishedImages.isEmpty() ||
           !m_engine.m_lastFinishedPortOutputs.isEmpty();
}

void RunEngineTestAccess::resetParallelPoolStartCountForTest() {
    DeepLux::resetParallelPoolStartCountForTest();
}

int RunEngineTestAccess::parallelPoolStartCountForTest() const {
    return DeepLux::parallelPoolStartCountForTest();
}

void FlowExecutionServiceTestAccess::setAfterStartSettledHookForTest(std::function<void()> hook) {
    m_service.setAfterStartSettledHookForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearAfterStartSettledHookForTest() {
    m_service.clearAfterStartSettledHookForTest();
}

void FlowExecutionServiceTestAccess::setStartHandshakeHookForTest(std::function<void()> hook) {
    m_service.setStartHandshakeHookForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearStartHandshakeHookForTest() {
    m_service.clearStartHandshakeHookForTest();
}

void FlowExecutionServiceTestAccess::setShutdownDeadlineForTest(int msecs) {
    m_service.setShutdownDeadlineForTest(msecs);
}

int FlowExecutionServiceTestAccess::shutdownDeadlineHitCountForTest() const {
    return m_service.shutdownDeadlineHitCountForTest();
}

void FlowExecutionServiceTestAccess::setWorkerExitBarrierForTest(std::function<void()> hook) {
    m_service.setWorkerExitBarrierForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearWorkerExitBarrierForTest() {
    m_service.clearWorkerExitBarrierForTest();
}

void FlowExecutionServiceTestAccess::setJobTeardownHookForTest(std::function<void()> hook) {
    m_service.setJobTeardownHookForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearJobTeardownHookForTest() {
    m_service.clearJobTeardownHookForTest();
}

void FlowExecutionServiceTestAccess::setWorkerLoopFaultHookForTest(std::function<void()> hook) {
    m_service.setWorkerLoopFaultHookForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearWorkerLoopFaultHookForTest() {
    m_service.clearWorkerLoopFaultHookForTest();
}

void FlowExecutionServiceTestAccess::setReclaimWaitHookForTest(std::function<void()> hook) {
    m_service.setReclaimWaitHookForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearReclaimWaitHookForTest() {
    m_service.clearReclaimWaitHookForTest();
}

int FlowExecutionServiceTestAccess::activeReclaimBorrowCountForTest() const {
    return m_service.activeReclaimBorrowCountForTest();
}

void FlowExecutionServiceTestAccess::setInCriticalSectionHookForTest(std::function<void()> hook) {
    m_service.setInCriticalSectionHookForTest(std::move(hook));
}

void FlowExecutionServiceTestAccess::clearInCriticalSectionHookForTest() {
    m_service.clearInCriticalSectionHookForTest();
}

int FlowExecutionServiceTestAccess::inflightWithoutBorrowCountForTest() const {
    return m_service.inflightWithoutBorrowCountForTest();
}

int FlowExecutionServiceTestAccess::liveReclaimGuardCountForTest() const {
    return m_service.liveReclaimGuardCountForTest();
}

} // namespace DeepLux
