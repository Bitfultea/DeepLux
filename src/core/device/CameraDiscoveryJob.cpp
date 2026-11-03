#include "CameraDiscoveryJob.h"

#include <QThread>

namespace DeepLux {

bool CameraDiscoveryJob::completeOnce(quint64 requestId, const QByteArray& utf8Json) {
    if (QThread::currentThread() != thread() || m_terminal)
        return false;
    m_terminal = true;
    emit completed(requestId, utf8Json);
    return true;
}

} // namespace DeepLux
