#include "core/io/YoloSegExporter.h"
#include "core/model/Annotation.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestYoloSegExporter : public QObject {
    Q_OBJECT

private slots:
    void exportSingleObject();
    void exportMultipleObjectsWithClassIdMapping();
    void emptySessionProducesEmptyFile();
    void unknownLabelFallsBackToClassZero();
};

AnnotationObject makeObject(const QString& label, const QList<QPointF>& polygon, const QRectF& bbox,
                            int imageWidth, int imageHeight) {
    AnnotationObject obj;
    obj.id = QStringLiteral("test-id");
    obj.label = label;
    obj.polygon = polygon;
    obj.bbox = bbox;
    obj.score = 0.9;
    return obj;
}

void TestYoloSegExporter::exportSingleObject() {
    AnnotationSession session;
    session.imagePath = QStringLiteral("/tmp/test.png");
    session.imageWidth = 100;
    session.imageHeight = 100;
    AnnotationObject obj = makeObject(QStringLiteral("缺陷"),
                                      {QPointF(10, 10), QPointF(20, 10), QPointF(20, 20), QPointF(10, 20)},
                                      QRectF(10, 10, 10, 10), 100, 100);
    session.annotations.append(obj);

    const QStringList labels = {QStringLiteral("缺陷"), QStringLiteral("划痕")};
    const QString content = YoloSegExporter::toJsonString(session, labels);

    const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 1);
    const QStringList parts = lines.at(0).split(QLatin1Char(' '));
    QCOMPARE(parts.size(), 9);
    QCOMPARE(parts.at(0).toInt(), 0);
    QVERIFY(qAbs(parts.at(1).toDouble() - 0.1) < 1e-6);
    QVERIFY(qAbs(parts.at(2).toDouble() - 0.1) < 1e-6);
}

void TestYoloSegExporter::exportMultipleObjectsWithClassIdMapping() {
    AnnotationSession session;
    session.imageWidth = 200;
    session.imageHeight = 100;
    session.annotations.append(
        makeObject(QStringLiteral("划痕"), {QPointF(0, 0), QPointF(10, 0), QPointF(10, 10), QPointF(0, 10)},
                   QRectF(0, 0, 10, 10), 200, 100));
    session.annotations.append(
        makeObject(QStringLiteral("缺陷"), {QPointF(20, 20), QPointF(30, 20), QPointF(30, 30), QPointF(20, 30)},
                   QRectF(20, 20, 10, 10), 200, 100));

    const QStringList labels = {QStringLiteral("缺陷"), QStringLiteral("划痕")};
    const QString content = YoloSegExporter::toJsonString(session, labels);
    const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0).split(QLatin1Char(' ')).at(0).toInt(), 1);  // 划痕 -> index 1
    QCOMPARE(lines.at(1).split(QLatin1Char(' ')).at(0).toInt(), 0);  // 缺陷 -> index 0
}

void TestYoloSegExporter::emptySessionProducesEmptyFile() {
    AnnotationSession session;
    session.imageWidth = 50;
    session.imageHeight = 50;

    QTemporaryFile tmp("yolo_empty_XXXXXX.txt");
    QVERIFY(tmp.open());
    tmp.close();

    QString err;
    QVERIFY(YoloSegExporter::exportToFile(session, tmp.fileName(), {QStringLiteral("缺陷")}, &err));

    QFile f(tmp.fileName());
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.size(), 0);
}

void TestYoloSegExporter::unknownLabelFallsBackToClassZero() {
    AnnotationSession session;
    session.imageWidth = 100;
    session.imageHeight = 100;
    session.annotations.append(
        makeObject(QStringLiteral("unknown"), {QPointF(10, 10), QPointF(20, 10), QPointF(20, 20), QPointF(10, 20)},
                   QRectF(10, 10, 10, 10), 100, 100));

    const QString content = YoloSegExporter::toJsonString(session, {QStringLiteral("缺陷")});
    const QStringList lines = content.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(lines.size(), 1);
    QCOMPARE(lines.at(0).split(QLatin1Char(' ')).at(0).toInt(), 0);
}

QTEST_MAIN(TestYoloSegExporter)
#include "test_yoloseg_exporter.moc"
