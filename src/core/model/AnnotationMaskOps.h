#pragma once

#include <QImage>
#include <QList>
#include <QPointF>
#include <QRectF>
#include <QString>

namespace DeepLux {

/**
 * @brief 标注掩膜编辑算法（SAM 批2）——纯函数、无 UI 依赖、可独立测试
 *
 * 掩膜口径（全模块统一）：
 * - 二值掩膜：Format_Grayscale8，像素 0（背景）/255（前景），尺寸=整图尺寸
 *   （与 AnnotationOverlayWidget "mask 是整图大小" 的绘制约定一致）。
 * - 显示掩膜：Format_ARGB32，前景 qRgba(6,182,212,90)（与 sam_server
 *   mask_png_base64 配色一致），背景全透明。
 * - RLE：与 tools/sam_server/sam_server.py `_encode_rle` 语义兼容的 JSON 字符串
 *   `{"size":[h,w],"counts":[...]}`：列主序扫描（x 外层、y 内层），counts[0]
 *   为 0 的游程（可为 0 长度），0/1 游程交替；JSON 字段顺序不约定（语义等价，
 *   编解码两侧均按键名解析）。
 *
 * 失败关闭：非法输入（空图/尺寸非法/RLE 损坏或尺寸越界）返回空值或原样副本，
 * 不猜测、不伪造前景。
 */
namespace MaskOps {

/// 显示掩膜（RGBA，alpha>0 为前景）→ 二值掩膜
QImage toBinaryMask(const QImage& displayMask);
/// 二值掩膜 → 显示掩膜（sam_server 配色）
QImage toDisplayMask(const QImage& binaryMask);

/// 二值掩膜 → RLE 字符串（与 sam_server _encode_rle 语义一致）；空图返回空串
QString encodeRle(const QImage& binaryMask);
/// RLE 字符串 → 二值掩膜；损坏/尺寸非法/游程总和不等于 w*h 时返回空图并置 ok=false
QImage decodeRle(const QString& rle, bool* ok = nullptr);

/**
 * 形态学膨胀/腐蚀：方形结构元，可分离滑动窗口实现（O(w·h)，与半径无关）。
 * 越界像素按 0（背景）处理：膨胀=窗口内任一前景；腐蚀=完整窗口 (2r+1)² 全前景，
 * 图像边界优先被腐蚀（与 OpenCV 默认 border=0 语义一致）。
 * radius<=0 或空输入返回原样副本。
 */
QImage dilate(const QImage& binaryMask, int radius);
QImage erode(const QImage& binaryMask, int radius);

/// 8-连通分量：按首像素 raster 顺序返回，每个分量为同尺寸独立二值掩膜；空掩膜返回空列表
QList<QImage> connectedComponents(const QImage& binaryMask);

/// 前景像素计数
qint64 foregroundCount(const QImage& binaryMask);

/**
 * 外轮廓追踪（Moore 邻域，Jacob 停机准则 + 步数上限防死循环）：
 * 追踪 raster 序最上最左连通分量的外边界，返回像素中心整数坐标多边形
 * （与 cv2.findContours 外轮廓口径一致；面积较像素数少约半个周长，孔洞
 * 不体现在单多边形中，像素级信息由掩膜/RLE 保留）。空掩膜返回空列表。
 */
QList<QPointF> traceContour(const QImage& binaryMask);

/// 前景像素包围矩形（x=minX, y=minY, w=maxX-minX+1, h=maxY-minY+1，像素中心口径）；空掩膜返回无效 QRectF()
QRectF foregroundBoundingBox(const QImage& binaryMask);

/**
 * 橡皮擦笔画：沿原图坐标折线以步长 ≤ max(0.5, radius/2) 采样，逐点擦除
 * 半径 radiusPx 的圆盘（越界裁剪）；单点笔画=擦除一个圆盘。返回新掩膜，
 * 不修改入参。空笔画/非法半径返回原样副本。
 */
QImage eraseStroke(const QImage& binaryMask, const QList<QPointF>& strokeImagePoints, double radiusPx);

/**
 * 多边形栅格化（无抗锯齿硬边缘，确定性输出；<3 点返回全背景）：
 * 用于无像素掩膜（缓存/RLE 均不可用）时从 polygon 重建可编辑掩膜。
 */
QImage rasterizePolygon(const QList<QPointF>& polygon, int width, int height);

} // namespace MaskOps

} // namespace DeepLux
