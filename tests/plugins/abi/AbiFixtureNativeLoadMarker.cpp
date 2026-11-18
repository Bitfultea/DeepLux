#include <QByteArray>
#include <QFile>
#include <QThread>

namespace {
struct NativeLoadMarker {
    NativeLoadMarker() {
        bool delayOk = false;
        const int delayMs = qEnvironmentVariableIntValue("DEEPLUX_ABI_FIXTURE_NATIVE_LOAD_DELAY_MS", &delayOk);
        if (delayOk && delayMs > 0)
            QThread::msleep(static_cast<unsigned long>(delayMs));
        const QByteArray path = qgetenv("DEEPLUX_ABI_FIXTURE_NATIVE_LOAD_MARKER");
        if (!path.isEmpty()) {
            QFile marker(QString::fromLocal8Bit(path));
            if (marker.open(QIODevice::WriteOnly | QIODevice::Append)) {
                marker.write("load\n");
            }
        }
    }
};

NativeLoadMarker marker;
} // namespace
