#include "YoloSegExporter.h"

#include "model/Annotation.h"

#include <QSaveFile>
#include <QTextStream>

namespace DeepLux {

bool YoloSegExporter::exportToFile(const AnnotationSession& session, const QString& outputPath,
                                   const QStringList& classLabels, QString* error) {
    if (session.imageWidth <= 0 || session.imageHeight <= 0) {
        if (error)
            *error = QStringLiteral("Image dimensions are invalid");
        return false;
    }

    // Fail closed: every annotation label must be present in classLabels.
    // Silently mapping an unknown label to class 0 corrupts training data.
    for (const AnnotationObject& obj : session.annotations) {
        if (!classLabels.contains(obj.label)) {
            if (error)
                *error = QStringLiteral("Unknown label '%1' is not in classLabels [%2]")
                             .arg(obj.label, classLabels.join(QStringLiteral(", ")));
            return false;
        }
    }

    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("Cannot create file: %1 (%2)").arg(outputPath, file.errorString());
        return false;
    }

    QTextStream out(&file);
    out << toJsonString(session, classLabels);
    out.flush();
    if (out.status() != QTextStream::Ok) {
        if (error)
            *error = QStringLiteral("Failed writing file: %1").arg(outputPath);
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("Failed to commit file: %1 (%2)").arg(outputPath, file.errorString());
        return false;
    }
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
