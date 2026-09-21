#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace DeepLux {

struct AnnotationSession;

/**
 * @brief COCO 格式导出器（SAM 批4）
 *
 * 将标注会话导出为 COCO detection/segmentation JSON：
 * - images：单图（会话绑定图像），id=1，file_name 取路径文件名，宽高来自会话
 * - annotations：id 从 1 顺序编号（会话顺序）；segmentation 为展平多边形
 *   [[x1,y1,x2,y2,...]]（polygon<3 点时为空数组，仅 bbox）；bbox=[x,y,w,h]
 *   （无效时由 polygon 推导，两者皆无则失败关闭）；area 口径链：
 *   maskRle 可解码且尺寸匹配 → 前景像素数（精确）；否则 polygon shoelace；
 *   否则 bbox 面积；iscrowd=0
 * - categories：categories 参数顺序优先（id 从 1 起），标注中出现但参数未
 *   含的 label 按出现顺序追加
 *
 * 失败关闭：空标注、会话缺正数宽高、对象缺 polygon/bbox、写盘失败均报错，
 * 不产出半合法文件（QSaveFile 原子写）。
 */
class CocoExporter {
public:
    static bool exportToFile(const AnnotationSession& session, const QString& outputPath, const QStringList& categories,
                             QString* error = nullptr);
    /// 生成 COCO JSON（校验失败返回空对象并置 error）
    static QJsonObject toJson(const AnnotationSession& session, const QStringList& categories,
                              QString* error = nullptr);
};

} // namespace DeepLux
