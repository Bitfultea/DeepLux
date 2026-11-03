#pragma once

#include "../platform/Platform.h"

#include <QByteArray>
#include <QObject>

namespace DeepLux {

class DEEPLUX_API CameraDiscoveryJob : public QObject {
    Q_OBJECT
public:
    explicit CameraDiscoveryJob(QObject* parent = nullptr) : QObject(parent) {}
    ~CameraDiscoveryJob() override = default;

    bool isTerminal() const {
        return m_terminal;
    }

    // Called on this object's thread after the worker has released its resources.
    bool completeOnce(quint64 requestId, const QByteArray& utf8Json);

signals:
    void completed(quint64 requestId, const QByteArray& utf8Json);

private:
    bool m_terminal = false;
};

} // namespace DeepLux
