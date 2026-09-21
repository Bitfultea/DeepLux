#include "core/io/CocoExporter.h"
#include "core/model/Annotation.h"
#include "core/model/AnnotationMaskOps.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

using namespace DeepLux;

/**
 * SAM 批4：COCO 导出——结构完整性、类别映射（参数序优先+未知追加）、
 * segmentation 展平、bbox 推导、area 口径链（RLE 像素数 → shoelace → bbox）、
 * 失败关闭（空会话/缺尺寸/退化对象/空 label）与 QSaveFile 原子写。
 */
class TestCocoExporter : public QObject {
    Q_OBJECT

private:
    // 200x150 会话：obj1 带精确 RLE（20x20 方块=400px），obj2 无 bbox 靠 polygon 推导
    static AnnotationSession makeSession() {
        AnnotationSession session;
        session.imagePath = QStringLiteral("/data/pic.png");
        session.imageWidth = 200;
        session.imageHeight = 150;

        QImage binary(200, 150, QImage::Format_Grayscale8);
        binary.fill(0);
        for (int y = 40; y < 60; ++y) {
            uchar* row = binary.scanLine(y);
            for (int x = 50; x < 70; ++x)
                row[x] = 255;
        }

        AnnotationObject obj1;
        obj1.id = QStringLiteral("a1");
        obj1.label = QStringLiteral("defect");
        obj1.bbox = QRectF(50, 40, 20, 20);
        obj1.polygon = {QPointF(50, 40), QPointF(70, 40), QPointF(70, 60), QPointF(50, 60)};
        obj1.maskRle = MaskOps::encodeRle(binary);
        obj1.score = 0.9;
        session.annotations.append(obj1);

        AnnotationObject obj2;
        obj2.id = QStringLiteral("a2");
        obj2.label = QStringLiteral("scratch");
        // bbox 缺省（无效）→ 导出时由 polygon 推导
        obj2.polygon = {QPointF(0, 0), QPointF(10, 0), QPointF(0, 10)};
        obj2.score = 0.5;
        session.annotations.append(obj2);
        return session;
    }

private slots:
    void exportProducesValidCoco() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("out.coco.json"));
        QString err;
        QVERIFY(CocoExporter::exportToFile(makeSession(), path, {QStringLiteral("defect"), QStringLiteral("scratch")},
                                           &err));
        QVERIFY2(err.isEmpty(), qPrintable(err));

        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        QVERIFY(root.contains("info"));
        QVERIFY(root.contains("licenses"));

        // images：单图，file_name 取文件名，宽高来自会话
        const QJsonArray images = root.value("images").toArray();
        QCOMPARE(images.size(), 1);
        QCOMPARE(images[0].toObject().value("id").toInt(), 1);
        QCOMPARE(images[0].toObject().value("file_name").toString(), QStringLiteral("pic.png"));
        QCOMPARE(images[0].toObject().value("width").toInt(), 200);
        QCOMPARE(images[0].toObject().value("height").toInt(), 150);

        // categories：参数顺序优先，id 从 1 起
        const QJsonArray categories = root.value("categories").toArray();
        QCOMPARE(categories.size(), 2);
        QCOMPARE(categories[0].toObject().value("id").toInt(), 1);
        QCOMPARE(categories[0].toObject().value("name").toString(), QStringLiteral("defect"));
        QCOMPARE(categories[1].toObject().value("name").toString(), QStringLiteral("scratch"));

        // annotations
        const QJsonArray annotations = root.value("annotations").toArray();
        QCOMPARE(annotations.size(), 2);
        const QJsonObject ann1 = annotations[0].toObject();
        QCOMPARE(ann1.value("id").toInt(), 1);
        QCOMPARE(ann1.value("image_id").toInt(), 1);
        QCOMPARE(ann1.value("category_id").toInt(), 1);
        QCOMPARE(ann1.value("iscrowd").toInt(), 0);
        QCOMPARE(ann1.value("bbox").toArray(), QJsonArray({50.0, 40.0, 20.0, 20.0}));
        // area：RLE 可解码且尺寸匹配 → 精确像素数
        QCOMPARE(ann1.value("area").toDouble(), 400.0);
        // segmentation：多边形展平 [x1,y1,...]
        const QJsonArray seg1 = ann1.value("segmentation").toArray();
        QCOMPARE(seg1.size(), 1);
        QCOMPARE(seg1[0].toArray().size(), 8);
        QCOMPARE(seg1[0].toArray()[0].toDouble(), 50.0);
        QCOMPARE(seg1[0].toArray()[1].toDouble(), 40.0);

        const QJsonObject ann2 = annotations[1].toObject();
        QCOMPARE(ann2.value("id").toInt(), 2);
        QCOMPARE(ann2.value("category_id").toInt(), 2);
        // bbox 由 polygon 推导（像素中心口径 w=maxX-minX）
        QCOMPARE(ann2.value("bbox").toArray(), QJsonArray({0.0, 0.0, 10.0, 10.0}));
        // area：无 RLE → shoelace（直角三角形 10x10/2 = 50）
        QCOMPARE(ann2.value("area").toDouble(), 50.0);
        QCOMPARE(ann2.value("segmentation").toArray()[0].toArray().size(), 6);
    }

    void categoryMappingFollowsParamOrderAndAppendsUnknown() {
        QString err;
        // 参数序翻转 → id 映射翻转
        const QJsonObject root =
            CocoExporter::toJson(makeSession(), {QStringLiteral("scratch"), QStringLiteral("defect")}, &err);
        QVERIFY(err.isEmpty());
        const QJsonArray annotations = root.value("annotations").toArray();
        QCOMPARE(annotations[0].toObject().value("category_id").toInt(), 2); // defect → id 2
        QCOMPARE(annotations[1].toObject().value("category_id").toInt(), 1); // scratch → id 1
        // 未知 label 按出现顺序追加
        const QJsonObject root2 = CocoExporter::toJson(makeSession(), {QStringLiteral("other")}, &err);
        QVERIFY(err.isEmpty());
        const QJsonArray cats = root2.value("categories").toArray();
        QCOMPARE(cats.size(), 3);
        QCOMPARE(cats[0].toObject().value("name").toString(), QStringLiteral("other"));
        QCOMPARE(cats[1].toObject().value("name").toString(), QStringLiteral("defect"));
        QCOMPARE(cats[2].toObject().value("name").toString(), QStringLiteral("scratch"));
    }

    void areaFallbackChain() {
        QString err;
        AnnotationSession session = makeSession();
        // obj1：RLE 尺寸与会话不符 → 解码失败 → 回落 shoelace（20x20 方块=400 恰好同值，
        // 改用三角形 polygon 区分：重设 polygon 为 10x10 直角三角形 → shoelace=50）
        QImage small(7, 9, QImage::Format_Grayscale8);
        small.fill(0);
        session.annotations[0].maskRle = MaskOps::encodeRle(small); // 尺寸 [9,7] ≠ 会话 200x150
        session.annotations[0].polygon = {QPointF(50, 40), QPointF(60, 40), QPointF(50, 50)};
        QJsonObject root = CocoExporter::toJson(session, {}, &err);
        QVERIFY(err.isEmpty());
        QCOMPARE(root.value("annotations").toArray()[0].toObject().value("area").toDouble(), 50.0);

        // 无 RLE 无 polygon（<3 点）但 bbox 有效 → bbox 面积；segmentation 为空数组
        AnnotationSession bboxOnly = makeSession();
        bboxOnly.annotations[1].polygon.clear();
        bboxOnly.annotations[1].bbox = QRectF(10, 10, 5, 4);
        root = CocoExporter::toJson(bboxOnly, {}, &err);
        QVERIFY(err.isEmpty());
        const QJsonObject ann2 = root.value("annotations").toArray()[1].toObject();
        QCOMPARE(ann2.value("area").toDouble(), 20.0);
        QCOMPARE(ann2.value("segmentation").toArray().size(), 0);
    }

    void degenerateAndEmptyLabelRejected() {
        QString err;
        // 无 polygon 且 bbox 无效 → 拒绝并指明下标
        AnnotationSession session = makeSession();
        session.annotations[1].polygon.clear();
        QVERIFY(CocoExporter::toJson(session, {}, &err).isEmpty());
        QVERIFY2(err.contains(QStringLiteral("annotations[1]")), qPrintable(err));

        // 空 label → 拒绝（COCO category_id 从 1 起，不伪造类别）
        err.clear();
        AnnotationSession noLabel = makeSession();
        noLabel.annotations[0].label = QString();
        QVERIFY(CocoExporter::toJson(noLabel, {}, &err).isEmpty());
        QVERIFY2(err.contains(QStringLiteral("label")), qPrintable(err));
    }

    void guardsEmptySessionAndDims() {
        QString err;
        AnnotationSession empty;
        empty.imageWidth = 10;
        empty.imageHeight = 10;
        QVERIFY(CocoExporter::toJson(empty, {}, &err).isEmpty());
        QVERIFY(err.contains(QStringLiteral("无标注对象")));

        AnnotationSession noDims = makeSession();
        noDims.imageWidth = 0;
        err.clear();
        QVERIFY(CocoExporter::toJson(noDims, {}, &err).isEmpty());
        QVERIFY(err.contains(QStringLiteral("宽高")));
    }

    void atomicWriteFailureReported() {
        QString err;
        // 不可创建路径 → false + error（QSaveFile 不损坏既有文件）
        QVERIFY(!CocoExporter::exportToFile(makeSession(), QStringLiteral("/nonexistent_dir_coco/out.json"),
                                            {QStringLiteral("defect")}, &err));
        QVERIFY(!err.isEmpty());

        // 正常导出：目录中仅目标文件，无 QSaveFile 临时残留
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("ok.coco.json"));
        err.clear();
        QVERIFY(CocoExporter::exportToFile(makeSession(), path, {QStringLiteral("defect")}, &err));
        QVERIFY2(err.isEmpty(), qPrintable(err));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), 1);
    }
};

QTEST_MAIN(TestCocoExporter)
#include "test_cocoexporter.moc"
