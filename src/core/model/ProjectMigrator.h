#pragma once

#include "Project.h"

#include <QPair>
#include <QString>
#include <QStringList>
#include <functional>

namespace DeepLux {

/**
 * @brief 迁移报告（阶段 2.2）
 */
struct MigrationReport {
    QStringList succeeded; // 成功项
    QStringList autoFixed; // 自动修复项
    QStringList warnings;  // 警告
    QStringList blockers;  // 阻塞项（待修复连接，禁止静默猜测）

    bool hasBlockers() const {
        return !blockers.isEmpty();
    }
};

/**
 * @brief 2.0 -> 3.0 工程迁移器
 *
 * - 所有节点放入 main 流程。
 * - fromOutput=0/toInput=0 映射到插件声明的默认输出/输入端口；线性图像流程 image->image。
 * - 无法确定的测量/变量/控制连接标记为"待修复"（blockers），禁止静默猜测。
 * - 幂等：同一工程重复迁移结果一致。
 * - 首次覆盖保存生成 .v2.bak（见 saveWithBackup）。
 */
class ProjectMigrator {
public:
    /// 返回 (默认输出端口ID, 默认输入端口ID)。默认实现基于 PluginManager 元数据。
    using PortLookup = std::function<QPair<QString, QString>(const QString& moduleId)>;

    static QPair<QString, QString> defaultPortLookup(const QString& moduleId);

    /// 原地迁移到 3.0，返回报告。不写盘。
    static MigrationReport migrate(Project& project, const PortLookup& lookup = defaultPortLookup);

    /// 保存 3.0 工程；若目标文件已存在且为旧版本，先生成 .v2.bak 备份。
    static bool saveWithBackup(Project& project, const QString& path);
};

} // namespace DeepLux
