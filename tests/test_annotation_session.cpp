#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include "core/model/Annotation.h"

using namespace DeepLux;

class TestAnnotationSession : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void promptRoundTrip();
    void objectRoundTrip();
    void sessionRoundTrip();
    void sessionFileIO();
    void findByIdRemoveById();
    void emptyMaskRle();
    void largeCoordinates();
};

void TestAnnotationSession::initTestCase() {}

void TestAnnotationSession::cleanup() {}

void TestAnnotationSession::promptRoundTrip() {
    AnnotationPrompt original;
    original.pointsPos = {QPointF(10.5, 20.3), QPointF(30.0, 40.0)};
    original.pointsNeg = {QPointF(50.0, 60.0)};
    original.box = QRectF(0, 0, 100, 200);

    QJsonObject json = original.toJson();
    AnnotationPrompt restored = AnnotationPrompt::fromJson(json);

    QCOMPARE(restored.pointsPos.size(), 2);
    QCOMPARE(restored.pointsPos[0].x(), 10.5);
    QCOMPARE(restored.pointsPos[0].y(), 20.3);
    QCOMPARE(restored.pointsPos[1].x(), 30.0);
    QCOMPARE(restored.pointsNeg.size(), 1);
    QCOMPARE(restored.pointsNeg[0].x(), 50.0);
    QVERIFY(restored.box.has_value());
    QCOMPARE(restored.box->width(), 100.0);
    QCOMPARE(restored.box->height(), 200.0);
}

void TestAnnotationSession::objectRoundTrip() {
    AnnotationObject original;
    original.id = "ann_001";
    original.label = "defect";
    original.bbox = QRectF(100, 200, 50, 80);
    original.polygon = {QPointF(100, 200), QPointF(150, 200), QPointF(150, 280), QPointF(100, 280)};
    original.maskRle = "abc123";
    original.prompts.pointsPos = {QPointF(125, 240)};
    original.prompts.box = QRectF(100, 200, 50, 80);
    original.score = 0.95;
    original.modelName = "sam_vit_b";

    QJsonObject json = original.toJson();
    AnnotationObject restored = AnnotationObject::fromJson(json);

    QCOMPARE(restored.id, QStringLiteral("ann_001"));
    QCOMPARE(restored.label, QStringLiteral("defect"));
    QCOMPARE(restored.bbox.x(), 100.0);
    QCOMPARE(restored.bbox.width(), 50.0);
    QCOMPARE(restored.polygon.size(), 4);
    QCOMPARE(restored.polygon[2].y(), 280.0);
    QCOMPARE(restored.maskRle, QStringLiteral("abc123"));
    QCOMPARE(restored.score, 0.95);
    QCOMPARE(restored.modelName, QStringLiteral("sam_vit_b"));
    QCOMPARE(restored.prompts.pointsPos.size(), 1);
    QVERIFY(restored.prompts.box.has_value());
}

void TestAnnotationSession::sessionRoundTrip() {
    AnnotationSession original;
    original.imagePath = "/data/NG2.tiff";
    original.imageWidth = 18200;
    original.imageHeight = 501;
    original.modelName = "mobile_sam";

    AnnotationObject obj1;
    obj1.id = "ann_001";
    obj1.label = "scratch";
    obj1.bbox = QRectF(1000, 100, 200, 50);
    obj1.polygon = {QPointF(1000, 100), QPointF(1200, 100), QPointF(1200, 150)};
    obj1.maskRle = "rle_data_1";
    obj1.score = 0.92;
    obj1.modelName = "mobile_sam";
    original.annotations.append(obj1);

    AnnotationObject obj2;
    obj2.id = "ann_002";
    obj2.label = "dent";
    obj2.bbox = QRectF(5000, 200, 300, 100);
    obj2.polygon = {QPointF(5000, 200), QPointF(5300, 200), QPointF(5300, 300), QPointF(5000, 300)};
    obj2.maskRle = "rle_data_2";
    obj2.score = 0.88;
    obj2.modelName = "mobile_sam";
    original.annotations.append(obj2);

    QJsonObject json = original.toJson();
    AnnotationSession restored = AnnotationSession::fromJson(json);

    QCOMPARE(restored.imagePath, QStringLiteral("/data/NG2.tiff"));
    QCOMPARE(restored.imageWidth, 18200);
    QCOMPARE(restored.imageHeight, 501);
    QCOMPARE(restored.modelName, QStringLiteral("mobile_sam"));
    QCOMPARE(restored.annotations.size(), 2);
    QCOMPARE(restored.annotations[0].label, QStringLiteral("scratch"));
    QCOMPARE(restored.annotations[1].bbox.x(), 5000.0);
    QCOMPARE(restored.annotations[1].polygon.size(), 4);
}

void TestAnnotationSession::sessionFileIO() {
    AnnotationSession original;
    original.imagePath = "/data/test.png";
    original.imageWidth = 640;
    original.imageHeight = 480;
    original.modelName = "sam_vit_b";

    AnnotationObject obj;
    obj.id = "ann_001";
    obj.label = "defect";
    obj.bbox = QRectF(10, 20, 30, 40);
    obj.polygon = {QPointF(10, 20), QPointF(40, 20), QPointF(40, 60), QPointF(10, 60)};
    obj.maskRle = "rle_test";
    obj.score = 0.95;
    obj.modelName = "sam_vit_b";
    original.annotations.append(obj);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString filePath = dir.filePath("test.deeplux-anno.json");

    QString saveError;
    QVERIFY(original.save(filePath, &saveError));
    QVERIFY(saveError.isEmpty());

    QString loadError;
    AnnotationSession restored = AnnotationSession::load(filePath, &loadError);
    QVERIFY(loadError.isEmpty());

    QCOMPARE(restored.imagePath, QStringLiteral("/data/test.png"));
    QCOMPARE(restored.imageWidth, 640);
    QCOMPARE(restored.imageHeight, 480);
    QCOMPARE(restored.annotations.size(), 1);
    QCOMPARE(restored.annotations[0].label, QStringLiteral("defect"));
    QCOMPARE(restored.annotations[0].bbox.width(), 30.0);
    QCOMPARE(restored.annotations[0].maskRle, QStringLiteral("rle_test"));
}

void TestAnnotationSession::findByIdRemoveById() {
    AnnotationSession session;
    AnnotationObject obj1;
    obj1.id = "ann_001";
    obj1.label = "a";
    AnnotationObject obj2;
    obj2.id = "ann_002";
    obj2.label = "b";
    session.annotations = {obj1, obj2};

    QCOMPARE(session.count(), 2);

    AnnotationObject* found = session.findById("ann_002");
    QVERIFY(found != nullptr);
    QCOMPARE(found->label, QStringLiteral("b"));

    QVERIFY(session.findById("nonexistent") == nullptr);

    session.removeById("ann_001");
    QCOMPARE(session.count(), 1);
    QCOMPARE(session.annotations[0].id, QStringLiteral("ann_002"));
}

void TestAnnotationSession::emptyMaskRle() {
    AnnotationObject obj;
    obj.id = "ann_001";
    obj.label = "test";
    obj.maskRle = "";  // 空 RLE

    QJsonObject json = obj.toJson();
    AnnotationObject restored = AnnotationObject::fromJson(json);

    QCOMPARE(restored.maskRle, QString());
    QVERIFY(restored.maskRle.isEmpty());
}

void TestAnnotationSession::largeCoordinates() {
    // 模拟 18200x501 长图的大坐标
    AnnotationObject obj;
    obj.id = "ann_001";
    obj.label = "defect";
    obj.bbox = QRectF(18000, 400, 100, 50);
    obj.polygon = {QPointF(18000, 400), QPointF(18100, 400), QPointF(18100, 450), QPointF(18000, 450)};
    obj.maskRle = "rle_large";
    obj.score = 0.91;
    obj.modelName = "mobile_sam";

    QJsonObject json = obj.toJson();
    AnnotationObject restored = AnnotationObject::fromJson(json);

    QCOMPARE(restored.bbox.x(), 18000.0);
    QCOMPARE(restored.bbox.y(), 400.0);
    QCOMPARE(restored.polygon[0].x(), 18000.0);
    QCOMPARE(restored.polygon[2].y(), 450.0);
}

QTEST_MAIN(TestAnnotationSession)
#include "test_annotation_session.moc"
