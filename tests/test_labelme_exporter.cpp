#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "core/io/LabelMeExporter.h"
#include "core/model/Annotation.h"

using namespace DeepLux;

class TestLabelMeExporter : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();

    void exportSingleObject();
    void exportMultipleObjects();
    void exportToFileIO();
    void emptySession();
    void largeImageExport();
};

void TestLabelMeExporter::initTestCase() {}

void TestLabelMeExporter::exportSingleObject() {
    AnnotationSession session;
    session.imagePath = "/data/test.png";
    session.imageWidth = 640;
    session.imageHeight = 480;
    session.modelName = "sam_vit_b";

    AnnotationObject obj;
    obj.id = "ann_001";
    obj.label = "defect";
    obj.polygon = {QPointF(10, 20), QPointF(40, 20), QPointF(40, 60), QPointF(10, 60)};
    session.annotations.append(obj);

    QString json = LabelMeExporter::toJsonString(session);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QVERIFY(!doc.isNull());

    QJsonObject root = doc.object();
    QCOMPARE(root["version"].toString(), QStringLiteral("5.0.0"));
    QCOMPARE(root["imagePath"].toString(), QStringLiteral("test.png"));
    QCOMPARE(root["imageWidth"].toInt(), 640);
    QCOMPARE(root["imageHeight"].toInt(), 480);

    QJsonArray shapes = root["shapes"].toArray();
    QCOMPARE(shapes.size(), 1);

    QJsonObject shape = shapes[0].toObject();
    QCOMPARE(shape["label"].toString(), QStringLiteral("defect"));
    QCOMPARE(shape["shape_type"].toString(), QStringLiteral("polygon"));

    QJsonArray points = shape["points"].toArray();
    QCOMPARE(points.size(), 4);
    QCOMPARE(points[0].toArray()[0].toDouble(), 10.0);
    QCOMPARE(points[0].toArray()[1].toDouble(), 20.0);
    QCOMPARE(points[2].toArray()[0].toDouble(), 40.0);
    QCOMPARE(points[2].toArray()[1].toDouble(), 60.0);
}

void TestLabelMeExporter::exportMultipleObjects() {
    AnnotationSession session;
    session.imagePath = "/data/test.png";
    session.imageWidth = 640;
    session.imageHeight = 480;

    AnnotationObject obj1;
    obj1.label = "scratch";
    obj1.polygon = {QPointF(0, 0), QPointF(10, 0), QPointF(10, 10)};
    session.annotations.append(obj1);

    AnnotationObject obj2;
    obj2.label = "dent";
    obj2.polygon = {QPointF(20, 20), QPointF(30, 20), QPointF(30, 30), QPointF(20, 30)};
    session.annotations.append(obj2);

    QString json = LabelMeExporter::toJsonString(session);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject root = doc.object();

    QJsonArray shapes = root["shapes"].toArray();
    QCOMPARE(shapes.size(), 2);
    QCOMPARE(shapes[0].toObject()["label"].toString(), QStringLiteral("scratch"));
    QCOMPARE(shapes[1].toObject()["label"].toString(), QStringLiteral("dent"));
}

void TestLabelMeExporter::exportToFileIO() {
    AnnotationSession session;
    session.imagePath = "/data/NG2.tiff";
    session.imageWidth = 18200;
    session.imageHeight = 501;

    AnnotationObject obj;
    obj.label = "defect";
    obj.polygon = {QPointF(1000, 100), QPointF(1200, 100), QPointF(1200, 150), QPointF(1000, 150)};
    session.annotations.append(obj);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString outPath = dir.filePath("export.json");

    QString error;
    QVERIFY(LabelMeExporter::exportToFile(session, outPath, &error));
    QVERIFY(error.isEmpty());

    QFile f(outPath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();

    QVERIFY(!doc.isNull());
    QJsonObject root = doc.object();
    QCOMPARE(root["imageWidth"].toInt(), 18200);
    QCOMPARE(root["imageHeight"].toInt(), 501);
    QCOMPARE(root["imagePath"].toString(), QStringLiteral("NG2.tiff"));
}

void TestLabelMeExporter::emptySession() {
    AnnotationSession session;
    session.imagePath = "/data/empty.png";
    session.imageWidth = 100;
    session.imageHeight = 100;

    QString json = LabelMeExporter::toJsonString(session);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject root = doc.object();

    QJsonArray shapes = root["shapes"].toArray();
    QCOMPARE(shapes.size(), 0);
    QCOMPARE(root["imageWidth"].toInt(), 100);
}

void TestLabelMeExporter::largeImageExport() {
    AnnotationSession session;
    session.imagePath = "/data/NG2.tiff";
    session.imageWidth = 18200;
    session.imageHeight = 501;

    AnnotationObject obj;
    obj.label = "defect";
    obj.polygon = {QPointF(18000, 400), QPointF(18100, 400), QPointF(18100, 450), QPointF(18000, 450)};
    session.annotations.append(obj);

    QString json = LabelMeExporter::toJsonString(session);
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject root = doc.object();
    QJsonArray points = root["shapes"].toArray()[0].toObject()["points"].toArray();

    // 大坐标完整保留
    QCOMPARE(points[0].toArray()[0].toDouble(), 18000.0);
    QCOMPARE(points[0].toArray()[1].toDouble(), 400.0);
    QCOMPARE(points[2].toArray()[0].toDouble(), 18100.0);
    QCOMPARE(points[2].toArray()[1].toDouble(), 450.0);
}

QTEST_MAIN(TestLabelMeExporter)
#include "test_labelme_exporter.moc"
