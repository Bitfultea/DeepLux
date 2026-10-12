#include "AnnotationMaskOps.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <opencv2/imgproc.hpp>
#include <vector>

namespace DeepLux {

namespace {

QImage ensureGray8(const QImage& src) {
    if (src.isNull())
        return QImage();
    if (src.format() == QImage::Format_Grayscale8)
        return src;
    return src.convertToFormat(QImage::Format_Grayscale8);
}

inline bool pixelSet(const QImage& gray, int x, int y) {
    return gray.constScanLine(y)[x] != 0;
}

// 可分离滑动窗口形态学单趟（H 或 V）。erode 要求完整窗口 (2r+1) 全前景，
// 越界按 0 计（边界优先腐蚀）；dilate 窗口内任一前景即置位。
QImage morphologyPassH(const QImage& gray, int radius, bool dilating) {
    const int w = gray.width();
    const int h = gray.height();
    QImage out(w, h, QImage::Format_Grayscale8);
    const int full = 2 * radius + 1;
    for (int y = 0; y < h; ++y) {
        const uchar* srow = gray.constScanLine(y);
        uchar* orow = out.scanLine(y);
        int sum = 0;
        const int initEnd = qMin(radius, w - 1);
        for (int x = 0; x <= initEnd; ++x)
            sum += srow[x] ? 1 : 0;
        for (int x = 0; x < w; ++x) {
            if (x > 0) {
                const int add = x + radius;
                const int rem = x - radius - 1;
                if (add < w)
                    sum += srow[add] ? 1 : 0;
                if (rem >= 0)
                    sum -= srow[rem] ? 1 : 0;
            }
            const bool set = dilating ? (sum > 0) : (sum == full);
            orow[x] = set ? 255 : 0;
        }
    }
    return out;
}

QImage morphologyPassV(const QImage& gray, int radius, bool dilating) {
    const int w = gray.width();
    const int h = gray.height();
    QVector<const uchar*> rows(h);
    for (int y = 0; y < h; ++y)
        rows[y] = gray.constScanLine(y);
    QImage out(w, h, QImage::Format_Grayscale8);
    const int full = 2 * radius + 1;
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        const int initEnd = qMin(radius, h - 1);
        for (int y = 0; y <= initEnd; ++y)
            sum += rows[y][x] ? 1 : 0;
        for (int y = 0; y < h; ++y) {
            if (y > 0) {
                const int add = y + radius;
                const int rem = y - radius - 1;
                if (add < h)
                    sum += rows[add][x] ? 1 : 0;
                if (rem >= 0)
                    sum -= rows[rem][x] ? 1 : 0;
            }
            uchar* orow = out.scanLine(y);
            const bool set = dilating ? (sum > 0) : (sum == full);
            orow[x] = set ? 255 : 0;
        }
    }
    return out;
}

} // namespace

namespace MaskOps {

QImage toBinaryMask(const QImage& displayMask) {
    if (displayMask.isNull())
        return QImage();
    const bool useAlpha = displayMask.hasAlphaChannel();
    const QImage src = (displayMask.format() == QImage::Format_ARGB32 || displayMask.format() == QImage::Format_RGB32)
                           ? displayMask
                           : displayMask.convertToFormat(QImage::Format_ARGB32);
    QImage out(src.width(), src.height(), QImage::Format_Grayscale8);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb* srow = reinterpret_cast<const QRgb*>(src.constScanLine(y));
        uchar* drow = out.scanLine(y);
        for (int x = 0; x < src.width(); ++x)
            drow[x] = (useAlpha ? qAlpha(srow[x]) > 0 : qGray(srow[x]) > 0) ? 255 : 0;
    }
    return out;
}

QImage toDisplayMask(const QImage& binaryMask) {
    const QImage gray = ensureGray8(binaryMask);
    if (gray.isNull())
        return QImage();
    QImage out(gray.width(), gray.height(), QImage::Format_ARGB32);
    const QRgb foreground = qRgba(6, 182, 212, 90); // sam_server mask_png_base64 配色
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* srow = gray.constScanLine(y);
        QRgb* drow = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < gray.width(); ++x)
            drow[x] = srow[x] ? foreground : 0;
    }
    return out;
}

QString encodeRle(const QImage& binaryMask) {
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return QString();
    const int w = mask.width();
    const int h = mask.height();
    QVector<const uchar*> rows(h);
    for (int y = 0; y < h; ++y)
        rows[y] = mask.constScanLine(y);

    // 与 sam_server _encode_rle 一致：列主序（x 外层、y 内层），counts[0] 为 0 的游程
    QJsonArray counts;
    int last = 0;
    int run = 0;
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
            const int value = rows[y][x] ? 1 : 0;
            if (value == last) {
                ++run;
            } else {
                counts.append(run);
                run = 1;
                last = value;
            }
        }
    }
    counts.append(run);
    const QJsonObject obj{{QStringLiteral("size"), QJsonArray{h, w}}, {QStringLiteral("counts"), counts}};
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

QImage decodeRle(const QString& rle, bool* ok) {
    return decodeRle(rle, QSize(), ok);
}

QImage decodeRle(const QString& rle, const QSize& expectedSize, bool* ok) {
    auto fail = [ok]() {
        if (ok)
            *ok = false;
        return QImage();
    };
    if (ok)
        *ok = false;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(rle.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return fail();
    const QJsonObject obj = doc.object();
    const QJsonArray size = obj.value(QStringLiteral("size")).toArray();
    const QJsonArray counts = obj.value(QStringLiteral("counts")).toArray();
    if (size.size() != 2 || !size[0].isDouble() || !size[1].isDouble())
        return fail();
    const double hd = size[0].toDouble();
    const double wd = size[1].toDouble();
    if (!(hd >= 1.0) || !(wd >= 1.0) || hd != std::floor(hd) || wd != std::floor(wd))
        return fail();                                // 非法/非整数尺寸（NaN 比较为假，一并拒绝）
    constexpr qint64 kMaxPixels = 64LL * 1024 * 1024; // 与 SAM 响应上限同量级，防损坏会话文件撑爆内存
    // 批3 复核二轮（P0）：上限判定在 double 阶段先行——超大尺寸（如 1e300）
    // 先转 int 是未定义行为，不得先转换再检查（乘积溢出为 inf 时同样被拒）
    if (hd * wd > double(kMaxPixels))
        return fail();
    const int h = int(hd);
    const int w = int(wd);
    // 批2 复核二轮（P3）：期望尺寸不匹配立即失败，不展开整幅像素
    if (expectedSize.isValid() && (expectedSize.width() != w || expectedSize.height() != h))
        return fail();
    const qint64 total = qint64(h) * w;

    QImage mask(w, h, QImage::Format_Grayscale8);
    mask.fill(0);
    qint64 consumed = 0;
    int value = 0; // counts[0] 为 0 的游程
    for (const QJsonValue& cv : counts) {
        if (!cv.isDouble())
            return fail();
        const double cd = cv.toDouble();
        if (!(cd >= 0.0) || cd != std::floor(cd) || cd > double(total))
            return fail();
        const qint64 runLength = qint64(cd);
        // 批3 复核二轮（P0）：写入前校验剩余容量——旧实现先写像素后累计比较，
        // {"size":[1,4],"counts":[0,4,0,1]} 会在 x=4 越界写之后才发现溢出
        if (runLength > total - consumed)
            return fail();
        if (value != 0) {
            // 列主序：线性索引 k → (x=k/h, y=k%h)，不维护可漂移的光标状态
            for (qint64 k = consumed; k < consumed + runLength; ++k)
                mask.scanLine(int(k % h))[int(k / h)] = 255;
        }
        consumed += runLength;
        value = 1 - value;
    }
    if (consumed != total)
        return fail();
    if (ok)
        *ok = true;
    return mask;
}

QImage dilate(const QImage& binaryMask, int radius) {
    const QImage gray = ensureGray8(binaryMask);
    if (gray.isNull() || radius <= 0)
        return gray;
    return morphologyPassV(morphologyPassH(gray, radius, true), radius, true);
}

QImage erode(const QImage& binaryMask, int radius) {
    const QImage gray = ensureGray8(binaryMask);
    if (gray.isNull() || radius <= 0)
        return gray;
    return morphologyPassV(morphologyPassH(gray, radius, false), radius, false);
}

QList<QImage> connectedComponents(const QImage& binaryMask) {
    QList<QImage> components;
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return components;
    const int w = mask.width();
    const int h = mask.height();
    QVector<const uchar*> rows(h);
    for (int y = 0; y < h; ++y)
        rows[y] = mask.constScanLine(y);

    QVector<quint8> visited(qint64(w) * h, 0);
    QVector<QPoint> stack;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const qint64 index = qint64(y) * w + x;
            if (!rows[y][x] || visited[index])
                continue;
            QImage component(w, h, QImage::Format_Grayscale8);
            component.fill(0);
            stack.clear();
            stack.append(QPoint(x, y));
            visited[index] = 1;
            while (!stack.isEmpty()) {
                const QPoint p = stack.takeLast();
                component.scanLine(p.y())[p.x()] = 255;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = p.y() + dy;
                    if (ny < 0 || ny >= h)
                        continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = p.x() + dx;
                        if (nx < 0 || nx >= w)
                            continue;
                        const qint64 nindex = qint64(ny) * w + nx;
                        if (visited[nindex] || !rows[ny][nx])
                            continue;
                        visited[nindex] = 1;
                        stack.append(QPoint(nx, ny));
                    }
                }
            }
            components.append(component);
        }
    }
    return components;
}

int connectedComponentCount(const QImage& binaryMask, int stopAfter) {
    // 批3 复核二轮（P1）：只计数路径不构造任何分量图像——碎片化掩膜（如成千
    // 上万个孤立像素）经 connectedComponents 会按分量数 × 整图尺寸分配内存直至
    // 耗尽；stopAfter>0 时数到第 stopAfter+1 个分量种子即提前返回
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return 0;
    const int w = mask.width();
    const int h = mask.height();
    QVector<const uchar*> rows(h);
    for (int y = 0; y < h; ++y)
        rows[y] = mask.constScanLine(y);

    QVector<quint8> visited(qint64(w) * h, 0);
    QVector<QPoint> stack;
    int count = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const qint64 index = qint64(y) * w + x;
            if (!rows[y][x] || visited[index])
                continue;
            ++count;
            if (stopAfter > 0 && count > stopAfter)
                return count;
            stack.clear();
            stack.append(QPoint(x, y));
            visited[index] = 1;
            while (!stack.isEmpty()) {
                const QPoint p = stack.takeLast();
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = p.y() + dy;
                    if (ny < 0 || ny >= h)
                        continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = p.x() + dx;
                        if (nx < 0 || nx >= w)
                            continue;
                        const qint64 nindex = qint64(ny) * w + nx;
                        if (visited[nindex] || !rows[ny][nx])
                            continue;
                        visited[nindex] = 1;
                        stack.append(QPoint(nx, ny));
                    }
                }
            }
        }
    }
    return count;
}

qint64 foregroundCount(const QImage& binaryMask) {
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return 0;
    qint64 count = 0;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar* row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x)
            count += row[x] ? 1 : 0;
    }
    return count;
}

QList<QPointF> traceContour(const QImage& binaryMask) {
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return {};
    const int w = mask.width();
    const int h = mask.height();
    auto isSet = [&](int x, int y) { return x >= 0 && x < w && y >= 0 && y < h && mask.constScanLine(y)[x] != 0; };

    // 起点：raster 序第一个前景像素（其上方必为非前景/越界）
    QPoint start(-1, -1);
    for (int y = 0; y < h && start.x() < 0; ++y) {
        for (int x = 0; x < w; ++x) {
            if (isSet(x, y)) {
                start = QPoint(x, y);
                break;
            }
        }
    }
    if (start.x() < 0)
        return {};

    // Moore 邻域追踪：顺时针方向表 E,SE,S,SW,W,NW,N,NE
    static const int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    static const int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    auto dirIndex = [](const QPoint& delta) -> int {
        for (int d = 0; d < 8; ++d) {
            if (kDx[d] == delta.x() && kDy[d] == delta.y())
                return d;
        }
        return 0;
    };

    QList<QPoint> contour;
    QPoint current = start;
    QPoint backtrack(start.x(), start.y() - 1);
    QPoint secondPixel(0, 0);
    bool haveSecond = false;
    const qint64 stepCap = 8LL * w * h + 16; // 防死循环兜底（Jacob 准则正常先停）
    for (qint64 step = 0; step < stepCap; ++step) {
        contour.append(current);
        const int backtrackDir = dirIndex(backtrack - current);
        QPoint next(0, 0);
        bool found = false;
        for (int k = 1; k <= 8; ++k) {
            const int d = (backtrackDir + k) % 8;
            const QPoint candidate(current.x() + kDx[d], current.y() + kDy[d]);
            if (isSet(candidate.x(), candidate.y())) {
                next = candidate;
                found = true;
                break;
            }
        }
        if (!found)
            break; // 孤立单点：轮廓即起点
        if (!haveSecond) {
            secondPixel = next;
            haveSecond = true;
        } else if (current == start && next == secondPixel) {
            break; // Jacob 停机：回到起点且下一步与首次一致
        }
        backtrack = current;
        current = next;
    }

    // 去重（相邻重复点 + 闭合重复的首点）并转像素中心坐标
    QList<QPointF> polygon;
    for (const QPoint& p : contour) {
        const QPointF point(p.x(), p.y());
        if (!polygon.isEmpty() && polygon.last() == point)
            continue;
        polygon.append(point);
    }
    if (polygon.size() >= 2 && polygon.first() == polygon.last())
        polygon.removeLast();
    return polygon;
}

QList<QPointF> traceLargestContour(const QImage& binaryMask) {
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return {};
    const cv::Mat view(mask.height(), mask.width(), CV_8UC1, const_cast<uchar*>(mask.constBits()), mask.bytesPerLine());
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(view.clone(), contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty())
        return {};
    const auto largest = std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
        return cv::contourArea(a) < cv::contourArea(b);
    });
    std::vector<cv::Point> simplified;
    cv::approxPolyDP(*largest, simplified, std::max(1.0, 0.002 * cv::arcLength(*largest, true)), true);
    QList<QPointF> polygon;
    for (const cv::Point& point : simplified)
        polygon.append(QPointF(point.x, point.y));
    return polygon;
}

QRectF foregroundBoundingBox(const QImage& binaryMask) {
    const QImage mask = ensureGray8(binaryMask);
    if (mask.isNull())
        return QRectF();
    int minX = -1;
    int minY = -1;
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < mask.height(); ++y) {
        const uchar* row = mask.constScanLine(y);
        for (int x = 0; x < mask.width(); ++x) {
            if (!row[x])
                continue;
            if (minX < 0) {
                minX = maxX = x;
                minY = maxY = y;
            } else {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
                minY = qMin(minY, y);
                maxY = qMax(maxY, y);
            }
        }
    }
    if (minX < 0)
        return QRectF(); // 空掩膜
    return QRectF(minX, minY, maxX - minX + 1, maxY - minY + 1);
}

QImage eraseStroke(const QImage& binaryMask, const QList<QPointF>& strokeImagePoints, double radiusPx) {
    QImage out = ensureGray8(binaryMask);
    if (out.isNull() || strokeImagePoints.isEmpty() || !std::isfinite(radiusPx) || radiusPx <= 0.0)
        return out;
    const int w = out.width();
    const int h = out.height();
    const double step = qMax(0.5, radiusPx / 2.0);
    const double radiusSquared = radiusPx * radiusPx;
    const int extent = int(std::ceil(radiusPx));

    auto stamp = [&](const QPointF& center) {
        const int cx = int(std::lround(center.x()));
        const int cy = int(std::lround(center.y()));
        for (int dy = -extent; dy <= extent; ++dy) {
            const int y = cy + dy;
            if (y < 0 || y >= h)
                continue;
            uchar* row = out.scanLine(y);
            for (int dx = -extent; dx <= extent; ++dx) {
                if (double(dx * dx + dy * dy) > radiusSquared)
                    continue;
                const int x = cx + dx;
                if (x >= 0 && x < w)
                    row[x] = 0;
            }
        }
    };

    stamp(strokeImagePoints.first());
    for (int i = 1; i < strokeImagePoints.size(); ++i) {
        const QPointF a = strokeImagePoints[i - 1];
        const QPointF b = strokeImagePoints[i];
        const double distance = QLineF(a, b).length();
        const int segments = qMax(1, int(std::ceil(distance / step)));
        for (int s = 1; s <= segments; ++s) {
            const double t = double(s) / segments;
            stamp(QPointF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t));
        }
    }
    return out;
}

QImage rasterizePolygon(const QList<QPointF>& polygon, int width, int height) {
    if (width <= 0 || height <= 0)
        return QImage();
    QImage canvas(width, height, QImage::Format_ARGB32);
    canvas.fill(0);
    if (polygon.size() >= 3) {
        QPainterPath path;
        path.moveTo(polygon.first());
        for (int i = 1; i < polygon.size(); ++i)
            path.lineTo(polygon[i]);
        path.closeSubpath();
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, false); // 硬边缘，确定性输出
        painter.fillPath(path, Qt::white);
        painter.end();
    }
    return toBinaryMask(canvas);
}

} // namespace MaskOps

} // namespace DeepLux
