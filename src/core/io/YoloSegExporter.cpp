#include "YoloSegExporter.h"
#include "model/Annotation.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace DeepLux {

bool YoloSegExporter::exportToFile(const AnnotationSession& session, const QString& outputPath,
                                   const QStringList& classLabels, QString* error) {
    if (session.imageWidth <= 0 || session.imageHeight <= 0) {
        if (error)
            *error = QStringLiteral("Image dimensions are invalid");
        return false;
    }

    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot create file: %1").arg(outputPath);
        return false;
    }

    QTextStream out(&file);
    out << toJsonString(session, classLabels);
    file.close();
    return true;
}

QString YoloSegExporter::toJsonString(const AnnotationSession& session, const QStringList& classLabels) {
    QString text;
    QTextStream out(&text);

    const double w = session.imageWidth;
    const double h = session.imageHeight;

    for (const AnnotationObject& obj : session.annotations) {
        int classId = classLabels.indexOf(obj.label);
        if (classId < 0)
            classId = 0;

        out << classId;
        for (const QPointF& p : obj.polygon) {
            double nx = (w > 0) ? p.x() / w : 0.0;
            double ny = (h > 0) ? p.y() / h : 0.0;
            nx = qBound(0.0, nx, 1.0);
            ny = qBound(0.0, ny, 1.0);
            out << ' ' << nx << ' ' << ny;
        }
        out << '\n';
    }

    return text;
}

} // namespace DeepLux
