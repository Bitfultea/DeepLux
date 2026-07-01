#include <QCoreApplication>
#include <QDebug>
#include "core/io/PlyLoader.h"
#include "core/io/TiffLoader.h"

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        qDebug() << "Usage: test_3dload <file.ply|file.tiff>";
        return 1;
    }

    QString filePath = argv[1];
    DeepLux::PointCloudData pc;
    QString error;
    bool ok = false;

    if (filePath.endsWith(".ply", Qt::CaseInsensitive)) {
        ok = DeepLux::PlyLoader::load(filePath, pc, error);
    } else {
        DeepLux::TiffLoader::Config cfg;
        cfg.step = 2;
        ok = DeepLux::TiffLoader::load(filePath, pc, error, cfg);
    }

    if (!ok) {
        qDebug() << "FAILED:" << error;
        return 1;
    }

    qDebug() << "OK:" << pc.size() << "points";
    qDebug() << "  hasColors:" << pc.hasColors() << "hasNormals:" << pc.hasNormals();
    if (pc.size() > 0) {
        qDebug() << "  first point:" << pc.points[0].x() << pc.points[0].y() << pc.points[0].z();
    }
    return 0;
}
