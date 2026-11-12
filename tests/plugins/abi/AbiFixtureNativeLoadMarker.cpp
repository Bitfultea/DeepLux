#include <QByteArray>
#include <QFile>

namespace {
struct NativeLoadMarker {
    NativeLoadMarker() {
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
