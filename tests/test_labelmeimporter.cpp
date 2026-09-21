#include "core/io/LabelMeImporter.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

using namespace DeepLux;

/**
 * SAM 批4：LabelMe 导入器——严格解析失败关闭（对象/shapes 数组/label 非空/
 * points 有限数值对/shape_type 类型与缺省推导），rectangleBoxes 归一化与
 * 非矩形/退化跳过计数。
 */
class TestLabelMeImporter : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString write(const QString& name, const QByteArray& content) {
        const QString path = QDir(m_dir.path()).filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write(content);
        f.close();
        return path;
    }

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
    }

    void loadsValidShapes() {
        const QString path = write(QStringLiteral("ok.labelme.json"),
                                   R"({
            "version": "5.0.0",
            "shapes": [
                {"label": "defect", "shape_type": "rectangle", "points": [[10, 20], [30, 50]]},
                {"label": "scratch", "shape_type": "polygon", "points": [[1, 2], [3, 4], [5, 6]]},
                {"label": "legacy", "points": [[7, 8], [9, 10]]}
            ],
            "imagePath": "pic.png", "imageWidth": 100, "imageHeight": 80
        })");
        QVERIFY(!path.isEmpty());
        QList<LabelMeShape> shapes;
        QString err;
        QVERIFY2(LabelMeImporter::loadShapes(path, shapes, &err), qPrintable(err));
        QCOMPARE(shapes.size(), 3);
        QCOMPARE(shapes[0].label, QStringLiteral("defect"));
        QCOMPARE(shapes[0].shapeType, QStringLiteral("rectangle"));
        QCOMPARE(shapes[0].points.size(), 2);
        QCOMPARE(shapes[0].points[1], QPointF(30, 50));
        QCOMPARE(shapes[1].shapeType, QStringLiteral("polygon"));
        // shape_type 缺省 → 按点数推导（2 → rectangle）
        QCOMPARE(shapes[2].shapeType, QStringLiteral("rectangle"));
    }

    void rejectsMalformedFiles() {
        QList<LabelMeShape> shapes;
        QString err;
        // 文件不存在
        QVERIFY(!LabelMeImporter::loadShapes(QStringLiteral("/nonexistent_lm/x.json"), shapes, &err));
        QVERIFY(!err.isEmpty());
        // 非 JSON 对象
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(write(QStringLiteral("arr.json"), R"([1,2])"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("JSON 对象")));
        // 缺 shapes / shapes 非数组
        err.clear();
        QVERIFY(
            !LabelMeImporter::loadShapes(write(QStringLiteral("noshapes.json"), R"({"imagePath":"p"})"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("shapes")));
        err.clear();
        QVERIFY(
            !LabelMeImporter::loadShapes(write(QStringLiteral("badshapes.json"), R"({"shapes":{}})"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("shapes")));
        // 元素非对象 / label 缺失或空 / points 缺失、短、非有限 / shape_type 非字符串
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(write(QStringLiteral("el.json"), R"({"shapes":[42]})"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("shapes[0]")));
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(
            write(QStringLiteral("nolabel.json"), R"({"shapes":[{"points":[[1,2],[3,4]]}]})"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("label")));
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(
            write(QStringLiteral("emptylabel.json"), R"({"shapes":[{"label":"","points":[[1,2],[3,4]]}]})"), shapes,
            &err));
        QVERIFY(err.contains(QStringLiteral("label")));
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(write(QStringLiteral("nopoints.json"), R"({"shapes":[{"label":"a"}]})"),
                                             shapes, &err));
        QVERIFY(err.contains(QStringLiteral("points")));
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(
            write(QStringLiteral("onepoint.json"), R"({"shapes":[{"label":"a","points":[[1,2]]}]})"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("points")));
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(
            write(QStringLiteral("strpt.json"), R"({"shapes":[{"label":"a","points":[["x","y"],[1,2]]}]})"), shapes,
            &err));
        QVERIFY(err.contains(QStringLiteral("points[0]")));
        err.clear();
        QVERIFY(!LabelMeImporter::loadShapes(
            write(QStringLiteral("tri.json"), R"({"shapes":[{"label":"a","points":[[1,2,3],[4,5]]}]})"), shapes, &err));
        QVERIFY(err.contains(QStringLiteral("points[0]")));
        err.clear();
        QVERIFY(
            !LabelMeImporter::loadShapes(write(QStringLiteral("badtype.json"),
                                               R"({"shapes":[{"label":"a","shape_type":7,"points":[[1,2],[3,4]]}]})"),
                                         shapes, &err));
        QVERIFY(err.contains(QStringLiteral("shape_type")));
        // 失败关闭：出错时不残留部分结果
        QVERIFY(shapes.isEmpty());
    }

    void rectangleBoxesNormalizeAndSkip() {
        QList<LabelMeShape> shapes;
        QString err;
        const QString path = write(QStringLiteral("boxes.labelme.json"),
                                   R"({
            "shapes": [
                {"label": "a", "shape_type": "rectangle", "points": [[30, 50], [10, 20]]},
                {"label": "poly", "shape_type": "polygon", "points": [[1, 2], [3, 4], [5, 6]]},
                {"label": "degen", "shape_type": "rectangle", "points": [[5, 5], [5, 9]]},
                {"label": "b", "shape_type": "rectangle", "points": [[0, 0], [4, 3]]}
            ]
        })");
        QVERIFY(!path.isEmpty());
        QVERIFY(LabelMeImporter::loadShapes(path, shapes, &err));
        int skipped = -1;
        const QList<LabelMeBox> boxes = LabelMeImporter::rectangleBoxes(shapes, &skipped);
        QCOMPARE(boxes.size(), 2);
        QCOMPARE(skipped, 2); // polygon + 零宽退化矩形
        // 反角归一化
        QCOMPARE(boxes[0].label, QStringLiteral("a"));
        QCOMPARE(boxes[0].box, QRectF(10, 20, 20, 30));
        QCOMPARE(boxes[1].label, QStringLiteral("b"));
        QCOMPARE(boxes[1].box, QRectF(0, 0, 4, 3));
        // 全非矩形 → 空列表 + 计数
        QList<LabelMeShape> onlyPolygon;
        onlyPolygon.append(shapes[1]);
        int skipped2 = 0;
        QVERIFY(LabelMeImporter::rectangleBoxes(onlyPolygon, &skipped2).isEmpty());
        QCOMPARE(skipped2, 1);
    }
};

QTEST_MAIN(TestLabelMeImporter)
#include "test_labelmeimporter.moc"
