#include "core/model/Annotation.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

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
    // 批3 复核二轮（P1-5/P1-4）：严格加载、原子保存、图像摘要
    void loadRejectsMalformedFiles();
    void saveReportsFailureAndKeepsOriginal();
    void imageDigestDeterministicAndSerialized();
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
    obj.maskRle = ""; // 空 RLE

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

// 批3 复核二轮（P1-5）：合法 JSON 但非对象/字段类型错误必须失败关闭，
// 不得静默当空会话载入（否则追加保存会覆盖损坏原文件）
void TestAnnotationSession::loadRejectsMalformedFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    auto write = [&](const QString& name, const QByteArray& content) -> QString {
        const QString path = dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(content);
        f.close();
        return path;
    };

    QString err;
    // 合法 JSON 数组 → 拒绝（旧实现 doc.object() 静默得到空对象）
    const QString arrayPath = write(QStringLiteral("array.json"), R"([{"id":"a"}])");
    QVERIFY(!arrayPath.isEmpty());
    AnnotationSession s = AnnotationSession::load(arrayPath, &err);
    QVERIFY(!err.isEmpty());
    QCOMPARE(s.annotations.size(), 0);
    QVERIFY(err.contains(QStringLiteral("JSON object")));

    // annotations 非数组 → 拒绝
    err.clear();
    const QString badAnn = write(QStringLiteral("badann.json"), R"({"imagePath":"x","annotations":{"a":1}})");
    s = AnnotationSession::load(badAnn, &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("annotations")));

    // imageWidth 非数值 → 拒绝
    err.clear();
    const QString badW = write(QStringLiteral("badw.json"), R"({"imageWidth":"640","annotations":[]})");
    s = AnnotationSession::load(badW, &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageWidth")));

    // imagePath 非字符串 → 拒绝
    err.clear();
    const QString badP = write(QStringLiteral("badp.json"), R"({"imagePath":42,"annotations":[]})");
    s = AnnotationSession::load(badP, &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imagePath")));

    // 合法对象（无可选字段）→ 接受
    err.clear();
    const QString good = write(QStringLiteral("good.json"), R"({"annotations":[]})");
    s = AnnotationSession::load(good, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));

    // === 批3 复核三轮（P1-2）：元素级/字段级严格校验 ===
    // 缺少 annotations 字段 → 拒绝
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("noann.json"), R"({"imagePath":"x"})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("annotations")));
    // 元素非对象 → 拒绝（旧实现 toObject() 把 42 静默变空标注）
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("el42.json"), R"({"annotations":[42]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("annotations[0]")));
    QCOMPARE(s.annotations.size(), 0);
    // 元素缺 id / id 非字符串 → 拒绝
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("noid.json"), R"({"annotations":[{"label":"a"}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("id")));
    // score 越界 / 非数值 → 拒绝
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("badscore.json"), R"({"annotations":[{"id":"x","score":1.5}]})"),
                                &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("score")));
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("strscore.json"), R"({"annotations":[{"id":"x","score":"9"}]})"),
                                &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("score")));
    // bbox 非数组 / 元素非数值 → 拒绝
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("badbbox.json"), R"({"annotations":[{"id":"x","bbox":"b"}]})"),
                                &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("bbox")));
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("nanbbox.json"), R"({"annotations":[{"id":"x","bbox":[1,2,3,"w"]}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("bbox")));
    // polygon 元素畸形 → 拒绝
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("badpoly.json"), R"({"annotations":[{"id":"x","polygon":[["a","b"]]}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("polygon")));
    // 尺寸负数/小数 → 拒绝
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("negw.json"), R"({"imageWidth":-1,"annotations":[]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageWidth")));
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("fracw.json"), R"({"imageWidth":10.5,"annotations":[]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageWidth")));
    // 摘要/模型名类型错误 → 拒绝
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("bigdigest.json"), R"({"imageDigest":42,"annotations":[]})"),
                                &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageDigest")));
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("badmodel.json"), R"({"modelName":7,"annotations":[]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("modelName")));
    // prompts 畸形 → 拒绝
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("badprompt.json"), R"({"annotations":[{"id":"x","prompts":{"points_pos":[[1,"a"]]}}]})"),
        &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("prompts")));

    // === 批3 复核四轮 ===
    // P1-2：重复 id → 拒绝（findById/removeById/掩膜缓存以 id 为键）
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("dupid.json"), R"({"annotations":[{"id":"same","score":0.1},{"id":"same","score":0.2}]})"),
        &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("duplicated")));
    QCOMPARE(s.annotations.size(), 0);
    // P2-3：尺寸超 int 上限 → 拒绝（旧实现 toInt() 静默溢出为 0）
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("bigw.json"), R"({"imageWidth":2147483648,"annotations":[]})"),
                                &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageWidth")));
    // P2-3：摘要必须为空或 64 位十六进制
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("shortdigest.json"), R"({"imageDigest":"ab","annotations":[]})"),
                                &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageDigest")));
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("baddigest.json"),
              R"({"imageDigest":"gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg","annotations":[]})"),
        &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("imageDigest")));

    // === 批3 复核五轮（P2-3）：超长数组拒绝——旧 size()<N 口径放行
    // [x,y,extra]/五元素 bbox，反序列化与再次保存时静默丢弃多余字段 ===
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("extrapt.json"), R"({"annotations":[{"id":"x","polygon":[[1,2,3]]}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("polygon")));
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("extrabbox.json"), R"({"annotations":[{"id":"x","bbox":[1,2,3,4,5]}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("bbox")));
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("extrabox.json"), R"({"annotations":[{"id":"x","prompts":{"box":[1,2,3,4,5]}}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("box")));
    // 短数组同样拒绝（回归护栏）
    err.clear();
    s = AnnotationSession::load(
        write(QStringLiteral("shortpt.json"), R"({"annotations":[{"id":"x","polygon":[[1]]}]})"), &err);
    QVERIFY(!err.isEmpty() && err.contains(QStringLiteral("polygon")));

    // 完整合法对象（64 位十六进制摘要）→ 接受
    err.clear();
    s = AnnotationSession::load(write(QStringLiteral("fullok.json"),
                                      R"({"imagePath":"p","imageWidth":8,"imageHeight":6,
                   "imageDigest":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                   "modelName":"sam",
                   "annotations":[{"id":"a1","label":"l","bbox":[1,2,3,4],"polygon":[[1,2],[3,4],[5,6]],
                                   "mask_rle":"","score":0.5,"model_name":"sam",
                                   "prompts":{"points_pos":[[1,2]],"points_neg":[],"box":[0,0,2,2]}}]})"),
                                &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(s.annotations.size(), 1);
    QCOMPARE(s.imageDigest, QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

// QSaveFile 原子写：失败路径显式报错；成功覆写不残留临时文件
void TestAnnotationSession::saveReportsFailureAndKeepsOriginal() {
    AnnotationSession session;
    session.imagePath = QStringLiteral("/data/x.png");

    // 不可创建路径 → false + error（旧实现忽略 write 结果假成功）
    QString err;
    QVERIFY(!session.save(QStringLiteral("/nonexistent_dir_deeplux_anno/fail.json"), &err));
    QVERIFY(!err.isEmpty());

    // 正常保存 → 文件可读回，且目录中不残留 QSaveFile 临时文件
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("ok.deeplux-anno.json"));
    err.clear();
    QVERIFY(session.save(path, &err));
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QStringList entries = QDir(dir.path()).entryList(QDir::Files);
    QCOMPARE(entries.size(), 1); // 仅目标文件，无 .xxxx 临时残留
    QString loadErr;
    const AnnotationSession restored = AnnotationSession::load(path, &loadErr);
    QVERIFY(loadErr.isEmpty());
    QCOMPARE(restored.imagePath, QStringLiteral("/data/x.png"));
}

// 批3 复核二轮（P1-4）：图像摘要确定性、判别性与序列化
void TestAnnotationSession::imageDigestDeterministicAndSerialized() {
    QImage a(8, 6, QImage::Format_RGB32);
    a.fill(Qt::gray);
    QImage aCopy = a.copy();
    QImage b(8, 6, QImage::Format_RGB32);
    b.fill(Qt::gray);
    b.setPixel(3, 3, qRgb(255, 0, 0)); // 单像素差异

    const QString digestA = AnnotationSession::computeImageDigest(a);
    QVERIFY(!digestA.isEmpty());
    QCOMPARE(AnnotationSession::computeImageDigest(aCopy), digestA);      // 确定性
    QVERIFY(AnnotationSession::computeImageDigest(b) != digestA);         // 单像素差异可判别
    QCOMPARE(AnnotationSession::computeImageDigest(QImage()), QString()); // 空图 → 空摘要
    // 同像素不同格式（规范化 ARGB32）摘要一致
    const QImage aArgb = a.convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(AnnotationSession::computeImageDigest(aArgb), digestA);

    // 摘要随会话序列化往返
    AnnotationSession session;
    session.imageDigest = digestA;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("digest.deeplux-anno.json"));
    QString err;
    QVERIFY(session.save(path, &err));
    const AnnotationSession restored = AnnotationSession::load(path, &err);
    QVERIFY(err.isEmpty());
    QCOMPARE(restored.imageDigest, digestA);
    // 旧文件（无 imageDigest 字段）加载 → 空摘要（兼容）
    QFile f(dir.filePath(QStringLiteral("legacy.json")));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(R"({"imagePath":"p","imageWidth":1,"imageHeight":1,"annotations":[]})");
    f.close();
    const AnnotationSession legacy = AnnotationSession::load(dir.filePath(QStringLiteral("legacy.json")), &err);
    QVERIFY(err.isEmpty());
    QVERIFY(legacy.imageDigest.isEmpty());
}

QTEST_MAIN(TestAnnotationSession)
#include "test_annotation_session.moc"
