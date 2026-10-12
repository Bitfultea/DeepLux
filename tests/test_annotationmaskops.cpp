#include "core/model/AnnotationMaskOps.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <cmath>

using namespace DeepLux;
using namespace DeepLux::MaskOps;

namespace {

// 构造二值掩膜（Grayscale8，0/255），rects 为闭区间像素矩形（QRect 语义）
QImage makeBinary(int w, int h, const QList<QRect>& rects) {
    QImage mask(w, h, QImage::Format_Grayscale8);
    mask.fill(0);
    for (const QRect& r : rects) {
        for (int y = qMax(0, r.top()); y <= qMin(h - 1, r.bottom()); ++y) {
            uchar* row = mask.scanLine(y);
            for (int x = qMax(0, r.left()); x <= qMin(w - 1, r.right()); ++x)
                row[x] = 255;
        }
    }
    return mask;
}

double polygonArea(const QList<QPointF>& polygon) {
    double area2 = 0.0;
    const int n = polygon.size();
    for (int i = 0; i < n; ++i) {
        const QPointF& p = polygon[i];
        const QPointF& q = polygon[(i + 1) % n];
        area2 += p.x() * q.y() - q.x() * p.y();
    }
    return std::fabs(area2) / 2.0;
}

} // namespace

/**
 * SAM 批2：AnnotationMaskOps 掩膜编辑算法测试——RLE 与 sam_server 编码语义
 * 一致（已知向量+往返）、形态学数值断言、连通分量计数与顺序、轮廓追踪、
 * 包围矩形、橡皮擦笔画、多边形栅格化，以及损坏输入失败关闭。
 */
class TestAnnotationMaskOps : public QObject {
    Q_OBJECT

private slots:
    // RLE 已知向量：与 sam_server _encode_rle 语义一致（列主序、counts[0] 为 0 游程）
    void rleKnownVectorMatchesSamServerSemantics() {
        // 2x2 对角掩膜 [[1,0],[0,1]]：列主序像素 1,0,0,1 → counts [0,1,2,1]
        const QImage diagonal = makeBinary(2, 2, {QRect(0, 0, 1, 1), QRect(1, 1, 1, 1)});
        const QString encoded = encodeRle(diagonal);
        QVERIFY(!encoded.isEmpty());
        const QJsonObject obj = QJsonDocument::fromJson(encoded.toUtf8()).object();
        QCOMPARE(obj.value("size").toArray(), QJsonArray({2, 2}));
        QCOMPARE(obj.value("counts").toArray(), QJsonArray({0, 1, 2, 1}));

        // 全前景 2x2 → counts [0,4]；全背景 2x2 → counts [4]
        const QImage full = makeBinary(2, 2, {QRect(0, 0, 2, 2)});
        QCOMPARE(QJsonDocument::fromJson(encodeRle(full).toUtf8()).object().value("counts").toArray(),
                 QJsonArray({0, 4}));
        const QImage empty = makeBinary(2, 2, {});
        QCOMPARE(QJsonDocument::fromJson(encodeRle(empty).toUtf8()).object().value("counts").toArray(),
                 QJsonArray({4}));

        // 空图（null）→ 空串
        QCOMPARE(encodeRle(QImage()), QString());
    }

    // 批2 复核二轮（P3）：期望尺寸不匹配在展开像素前快速失败
    void rleDecodeHonorsExpectedSize() {
        const QImage mask = makeBinary(2, 2, {QRect(0, 0, 1, 1)});
        const QString rle = encodeRle(mask);
        bool ok = true;
        QCOMPARE(decodeRle(rle, QSize(2, 2), &ok), mask);
        QVERIFY(ok);
        QVERIFY(decodeRle(rle, QSize(3, 2), &ok).isNull() && !ok);
        QVERIFY(decodeRle(rle, QSize(2, 3), &ok).isNull() && !ok);
        QVERIFY(decodeRle(rle, QSize(), &ok) == mask && ok); // 无效期望 = 不检查（旧行为）
    }

    // 批3 复核二轮（P0）：游程累计超出像素总数必须在写入前拒绝——
    // 旧实现先写像素后比较，{"size":[1,4],"counts":[0,4,0,1]} 会越界写 x=4
    void rleDecodeRejectsOverflowBeforeWrite() {
        bool ok = true;
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[1,4],"counts":[0,4,0,1]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[1,4],"counts":[0,5]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[2,2],"counts":[0,4,1]})"), &ok).isNull() && !ok);
        // 数据不足（总和 < w*h）仍拒绝
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[1,4],"counts":[0,3]})"), &ok).isNull() && !ok);
        // 超大尺寸必须在 int 转换前拒绝（int(1e300) 为未定义行为）
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[1e300,1],"counts":[0]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[1e6,1e6],"counts":[0]})"), &ok).isNull() && !ok); // 1e12 > 64M
        // 合法编码（含末尾补零游程）→ 接受
        QVERIFY(!decodeRle(QStringLiteral(R"({"size":[1,4],"counts":[0,4,0]})"), &ok).isNull() && ok);
        QVERIFY(!decodeRle(QStringLiteral(R"({"size":[1,4],"counts":[1,1,0,2]})"), &ok).isNull() && ok);
        // 写入位置正确性：size=[2,2] counts=[0,1,2,1] 列主序（x 外层）→
        // 0 个背景、1 个前景 (x0,y0)、2 个背景 (x0,y1)(x1,y0)、1 个前景 (x1,y1)
        const QImage decoded = decodeRle(QStringLiteral(R"({"size":[2,2],"counts":[0,1,2,1]})"), &ok);
        QVERIFY(ok);
        QCOMPARE(int(decoded.constScanLine(0)[0]), 255); // (x0,y0) 前景
        QCOMPARE(int(decoded.constScanLine(1)[0]), 0);   // (x0,y1) 背景
        QCOMPARE(int(decoded.constScanLine(0)[1]), 0);   // (x1,y0) 背景
        QCOMPARE(int(decoded.constScanLine(1)[1]), 255); // (x1,y1) 前景
    }

    // 批3 复核二轮（P1）：只计数路径早退，不构造分量图像（碎片掩膜不打爆内存）
    void componentCountEarlyExit() {
        QList<QRect> dots;
        for (int i = 0; i < 10; ++i)
            for (int j = 0; j < 10; ++j)
                dots.append(QRect(2 + i * 6, 2 + j * 6, 1, 1)); // 100 个孤立像素
        const QImage fragmented = makeBinary(64, 64, dots);
        QCOMPARE(connectedComponentCount(fragmented), 100);
        QCOMPARE(connectedComponentCount(fragmented, 1), 2); // 数到第 2 个即早退
        QCOMPARE(connectedComponentCount(fragmented, 200), 100);
        QCOMPARE(connectedComponentCount(makeBinary(8, 8, {})), 0);
        QCOMPARE(connectedComponentCount(makeBinary(8, 8, {QRect(1, 1, 3, 3)})), 1);
        // 与 connectedComponents 计数一致
        QCOMPARE(connectedComponentCount(makeBinary(40, 40, {QRect(2, 2, 5, 5), QRect(20, 20, 5, 5)})), 2);
        QCOMPARE(connectedComponentCount(QImage()), 0);
    }

    void rleRoundTrip() {
        const QList<QImage> masks = {
            makeBinary(1, 1, {QRect(0, 0, 1, 1)}),
            makeBinary(7, 5, {QRect(2, 1, 3, 3)}),
            makeBinary(8, 8, {QRect(0, 0, 1, 8), QRect(7, 0, 1, 8)}), // 首尾列
            makeBinary(6, 4, {}),                                     // 全背景
            makeBinary(6, 4, {QRect(0, 0, 6, 4)}),                    // 全前景
        };
        for (const QImage& mask : masks) {
            bool ok = false;
            const QImage decoded = decodeRle(encodeRle(mask), &ok);
            QVERIFY2(ok, qPrintable(QStringLiteral("decode failed for %1x%2").arg(mask.width()).arg(mask.height())));
            QCOMPARE(decoded.size(), mask.size());
            QCOMPARE(decoded, mask); // 逐像素一致
        }
    }

    void rleDecodeRejectsCorrupt() {
        bool ok = true;
        QVERIFY(decodeRle(QStringLiteral("not json"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral("{}"), &ok).isNull() && !ok);
        // size 缺失/非法
        QVERIFY(decodeRle(QStringLiteral(R"({"counts":[4]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[0,2],"counts":[0]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[2.5,2],"counts":[5]})"), &ok).isNull() && !ok);
        // 游程总和 != w*h
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[2,2],"counts":[1,1]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[2,2],"counts":[0,4,1]})"), &ok).isNull() && !ok);
        // 负游程/非数值
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[2,2],"counts":[0,-4]})"), &ok).isNull() && !ok);
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[2,2],"counts":["0",4]})"), &ok).isNull() && !ok);
        // 尺寸上限（防损坏会话文件撑爆内存）
        QVERIFY(decodeRle(QStringLiteral(R"({"size":[100000,100000],"counts":[0]})"), &ok).isNull() && !ok);
        // 合法输入 ok=true
        QVERIFY(!decodeRle(QStringLiteral(R"({"size":[2,2],"counts":[0,4]})"), &ok).isNull() && ok);
    }

    void displayBinaryRoundTrip() {
        const QImage binary = makeBinary(9, 4, {QRect(1, 1, 3, 2)});
        const QImage display = toDisplayMask(binary);
        QCOMPARE(display.format(), QImage::Format_ARGB32);
        QCOMPARE(display.pixelColor(1, 1), QColor(6, 182, 212, 90)); // sam_server 配色
        QCOMPARE(display.pixelColor(0, 0), QColor(0, 0, 0, 0));
        QCOMPARE(toBinaryMask(display), binary);
        // 空图失败关闭
        QVERIFY(toDisplayMask(QImage()).isNull());
        QVERIFY(toBinaryMask(QImage()).isNull());
    }

    void dilateAndErodeNumerics() {
        // 40x40 中央 10x10 方块（15..24）
        const QImage square = makeBinary(40, 40, {QRect(15, 15, 10, 10)});
        QCOMPARE(foregroundCount(square), qint64(100));

        const QImage grown = dilate(square, 2);
        QCOMPARE(foregroundCount(grown), qint64(196)); // 14x14
        QCOMPARE(foregroundBoundingBox(grown), QRectF(13, 13, 14, 14));

        const QImage shrunk = erode(square, 2);
        QCOMPARE(foregroundCount(shrunk), qint64(36)); // 6x6
        QCOMPARE(foregroundBoundingBox(shrunk), QRectF(17, 17, 6, 6));

        // radius<=0 原样；空掩膜不变
        QCOMPARE(dilate(square, 0), square);
        QCOMPARE(erode(square, 0), square);
        QCOMPARE(foregroundCount(dilate(makeBinary(5, 5, {}), 3)), qint64(0));
        // 腐蚀半径达到半边长 → 全空（失败关闭由调用方处理）
        QCOMPARE(foregroundCount(erode(square, 5)), qint64(0));

        // 越界按 0：贴边方块腐蚀从边界优先
        const QImage corner = makeBinary(20, 20, {QRect(0, 0, 10, 10)});
        const QImage cornerEroded = erode(corner, 1);
        QCOMPARE(foregroundCount(cornerEroded), qint64(64)); // 8x8（三条边被削）
        QCOMPARE(foregroundBoundingBox(cornerEroded), QRectF(1, 1, 8, 8));
    }

    void connectedComponentsCountAndOrder() {
        // 两个分离方块 → 2 个分量，raster 序（左上先）
        const QImage two = makeBinary(40, 40, {QRect(20, 20, 5, 5), QRect(2, 2, 5, 5)});
        const QList<QImage> comps = connectedComponents(two);
        QCOMPARE(comps.size(), 2);
        QCOMPARE(foregroundCount(comps[0]), qint64(25));
        QCOMPARE(foregroundBoundingBox(comps[0]), QRectF(2, 2, 5, 5)); // raster 首像素所在分量在前
        QCOMPARE(foregroundBoundingBox(comps[1]), QRectF(20, 20, 5, 5));

        // 对角接触 = 8-连通 → 1 个分量
        const QImage diagonalTouch = makeBinary(20, 20, {QRect(0, 0, 5, 5), QRect(5, 5, 5, 5)});
        QCOMPARE(connectedComponents(diagonalTouch).size(), 1);

        // 空掩膜 → 0 分量；单像素 → 1
        QCOMPARE(connectedComponents(makeBinary(4, 4, {})).size(), 0);
        QCOMPARE(connectedComponents(makeBinary(4, 4, {QRect(1, 1, 1, 1)})).size(), 1);
        QVERIFY(connectedComponents(QImage()).isEmpty());
    }

    void traceContourSquareAndDegenerate() {
        // 10x10 方块（10..19）：外轮廓为边界像素中心，shoelace 面积 = 9x9 = 81
        const QImage square = makeBinary(40, 40, {QRect(10, 10, 10, 10)});
        const QList<QPointF> contour = traceContour(square);
        QVERIFY2(contour.size() >= 4, qPrintable(QStringLiteral("contour size=%1").arg(contour.size())));
        QVERIFY(std::abs(polygonArea(contour) - 81.0) < 1e-9);
        // 轮廓点全部落在边界像素中心集合上
        for (const QPointF& p : contour) {
            QCOMPARE(p.x(), double(int(p.x()))); // 整数像素坐标
            const bool onBorder = (int(p.x()) == 10 || int(p.x()) == 19 || int(p.y()) == 10 || int(p.y()) == 19);
            QVERIFY2(onBorder, qPrintable(QStringLiteral("contour point off border: %1,%2").arg(p.x()).arg(p.y())));
            QVERIFY(p.x() >= 10 && p.x() <= 19 && p.y() >= 10 && p.y() <= 19);
        }
        // 闭合（首尾不重复，由调用方 closeSubpath）
        QVERIFY(contour.first() != contour.last());

        // 单像素 → 单点轮廓
        const QList<QPointF> single = traceContour(makeBinary(5, 5, {QRect(2, 2, 1, 1)}));
        QCOMPARE(single.size(), 1);
        QCOMPARE(single.first(), QPointF(2, 2));

        // 空掩膜 → 空列表（不伪造）
        QVERIFY(traceContour(makeBinary(5, 5, {})).isEmpty());
        QVERIFY(traceContour(QImage()).isEmpty());

        // 多分量输入：追踪 raster 序第一个分量（左上 3x3）
        const QImage two = makeBinary(30, 30, {QRect(20, 20, 5, 5), QRect(2, 2, 3, 3)});
        const QList<QPointF> first = traceContour(two);
        for (const QPointF& p : first)
            QVERIFY(p.x() <= 4 && p.y() <= 4);
    }

    void traceLargestContourSelectsMainComponent() {
        const QImage mask = makeBinary(80, 80, {QRect(2, 2, 3, 3), QRect(30, 25, 30, 20)});
        const QList<QPointF> polygon = traceLargestContour(mask);
        QVERIFY(polygon.size() >= 4);
        for (const QPointF& point : polygon) {
            QVERIFY(point.x() >= 30 && point.x() <= 59);
            QVERIFY(point.y() >= 25 && point.y() <= 44);
        }
        QVERIFY(traceLargestContour(makeBinary(8, 8, {})).isEmpty());
    }

    void foregroundBoundingBoxContract() {
        QCOMPARE(foregroundBoundingBox(makeBinary(30, 30, {QRect(5, 7, 4, 3)})), QRectF(5, 7, 4, 3));
        QVERIFY(!foregroundBoundingBox(makeBinary(30, 30, {})).isValid());
        QVERIFY(!foregroundBoundingBox(QImage()).isValid());
    }

    void eraseStrokeCutsAndStamps() {
        // 全前景 40x40，水平笔画 (5,20)→(35,20) 半径 2 → 中部条带被擦除，角部保留
        const QImage full = makeBinary(40, 40, {QRect(0, 0, 40, 40)});
        const QImage erased = eraseStroke(full, {QPointF(5, 20), QPointF(35, 20)}, 2.0);
        QCOMPARE(erased.size(), full.size());
        const qint64 before = foregroundCount(full);
        const qint64 after = foregroundCount(erased);
        QVERIFY2(after < before, "stroke must remove pixels");
        // r=2 圆盘沿 x=5..35 采样并集 ≈ 163 px（行带 35+33·2+31·2），给采样取整留容差
        QVERIFY2(before - after >= 150 && before - after <= 180,
                 qPrintable(QStringLiteral("removed=%1 out of expected band area").arg(before - after)));
        // 笔画覆盖处（x∈[8,32], y∈[19,21] 距轴 ≤1 < 2）被擦除
        for (int x = 8; x <= 32; ++x) {
            QCOMPARE(int(erased.constScanLine(20)[x]), 0);
            QCOMPARE(int(erased.constScanLine(19)[x]), 0);
        }
        // 远处角部保留
        QCOMPARE(int(erased.constScanLine(0)[0]), 255);
        QCOMPARE(int(erased.constScanLine(39)[39]), 255);

        // 单点笔画 = 圆盘擦除（dx²+dy² ≤ r²）：r=2 → 中心与 (0,±2) 擦除、(3,0)/(0,±3) 保留
        const QImage dot = eraseStroke(full, {QPointF(20, 20)}, 2.0);
        QCOMPARE(int(dot.constScanLine(20)[20]), 0);   // 中心
        QCOMPARE(int(dot.constScanLine(18)[20]), 0);   // dy=-2: 4≤4 → 擦除
        QCOMPARE(int(dot.constScanLine(20)[22]), 0);   // dx=+2: 4≤4 → 擦除
        QCOMPARE(int(dot.constScanLine(20)[23]), 255); // dx=3: 9>4 → 保留
        QCOMPARE(int(dot.constScanLine(17)[20]), 255); // dy=-3 → 保留

        // 空笔画/非法半径 → 原样副本（失败关闭不擦除）
        QCOMPARE(eraseStroke(full, {}, 2.0), full);
        QCOMPARE(eraseStroke(full, {QPointF(5, 5)}, 0.0), full);
        QCOMPARE(eraseStroke(full, {QPointF(5, 5)}, -1.0), full);
        QVERIFY(eraseStroke(QImage(), {QPointF(5, 5)}, 2.0).isNull());
    }

    void rasterizePolygonDeterministic() {
        const QList<QPointF> square = {QPointF(10, 10), QPointF(20, 10), QPointF(20, 20), QPointF(10, 20)};
        const QImage mask = rasterizePolygon(square, 40, 30);
        QCOMPARE(mask.size(), QSize(40, 30));
        const qint64 area = foregroundCount(mask);
        // 无抗锯齿硬边缘：整数坐标方形面积 ~100（边界像素归属允许 ±边长容差）
        QVERIFY2(area >= 90 && area <= 130, qPrintable(QStringLiteral("area=%1").arg(area)));
        const QRectF box = foregroundBoundingBox(mask);
        QCOMPARE(box.x(), 10.0);
        QCOMPARE(box.y(), 10.0);
        QVERIFY(box.width() >= 10 && box.width() <= 11);

        // <3 点 / 非法尺寸 → 全背景或空图（不伪造前景）
        QCOMPARE(foregroundCount(rasterizePolygon({QPointF(1, 1), QPointF(5, 5)}, 10, 10)), qint64(0));
        QVERIFY(rasterizePolygon(square, 0, 10).isNull());
        QVERIFY(rasterizePolygon(square, 10, -1).isNull());
    }
};

QTEST_MAIN(TestAnnotationMaskOps)
#include "test_annotationmaskops.moc"
