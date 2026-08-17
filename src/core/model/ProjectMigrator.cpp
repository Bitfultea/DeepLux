#include "ProjectMigrator.h"

#include "core/manager/PluginManager.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace DeepLux {

QPair<QString, QString> ProjectMigrator::defaultPortLookup(const QString& moduleId) {
    const PluginInfo info = PluginManager::instance().pluginInfo(moduleId);
    QString outPort = QStringLiteral("image");
    QString inPort = QStringLiteral("image");
    if (!info.outputPorts.isEmpty())
        outPort = info.outputPorts.first().id;
    if (!info.inputPorts.isEmpty())
        inPort = info.inputPorts.first().id;
    return {outPort, inPort};
}

MigrationReport ProjectMigrator::migrate(Project& project, const PortLookup& lookup) {
    MigrationReport report;
    const QString originalVersion = project.formatVersion();

    // 3.0 工程已经持久化稳定端口，重复打开不应改写流程、时间戳或脏状态。
    if (originalVersion == QStringLiteral("3.0")) {
        report.succeeded << QStringLiteral("工程已为 3.0，无需迁移");
        return report;
    }

    // 1. 所有节点放入 main 流程
    ProjectFlow main;
    main.id = QStringLiteral("main");
    main.name = QStringLiteral("主流程");
    for (const ModuleInstance& m : project.modules())
        main.nodeIds.append(m.id);
    QList<ProjectFlow> flows;
    flows.append(main);
    project.setFlows(flows);
    project.setFormatVersion(QStringLiteral("3.0"));

    // 建立 moduleId 索引
    auto moduleTypeOf = [&](const QString& id) {
        for (const ModuleInstance& m : project.modules())
            if (m.id == id)
                return m.moduleId;
        return QString();
    };

    // 2. 连接端口映射
    QList<ModuleConnection> conns = project.connections();
    for (ModuleConnection& conn : conns) {
        const QString fromType = moduleTypeOf(conn.fromModuleId);
        const QString toType = moduleTypeOf(conn.toModuleId);
        const auto fromPorts = lookup(fromType);
        const auto toPorts = lookup(toType);

        // 下游若声明必需命名输入（非 image）而上游未声明对应输出，视为歧义，禁止猜测
        const PluginInfo downInfo = PluginManager::instance().pluginInfo(toType);
        bool ambiguous = false;
        for (const PortSpec& in : downInfo.inputPorts) {
            if (!in.required || in.id == QLatin1String("image"))
                continue;
            const PluginInfo upInfo = PluginManager::instance().pluginInfo(fromType);
            bool provided = false;
            for (const PortSpec& out : upInfo.outputPorts)
                if (out.id == in.id)
                    provided = true;
            if (!provided) {
                ambiguous = true;
                report.blockers << QStringLiteral("连接 %1->%2 的必需输入 %3 无法由上游确定（待修复）")
                                       .arg(conn.fromModuleId, conn.toModuleId, in.id);
            }
        }

        if (ambiguous) {
            conn.edgeType = QStringLiteral("pending");
            continue;
        }

        if (conn.fromPort.isEmpty() || conn.toPort.isEmpty()) {
            conn.fromPort = fromPorts.first;
            conn.toPort = toPorts.second;
            conn.edgeType = QStringLiteral("data");
            report.autoFixed << QStringLiteral("连接 %1->%2 端口映射为 %3->%4")
                                    .arg(conn.fromModuleId, conn.toModuleId, conn.fromPort, conn.toPort);
        } else {
            report.succeeded << QStringLiteral("连接 %1->%2 已具端口").arg(conn.fromModuleId, conn.toModuleId);
        }
    }
    // 写回带字符串端口的连接
    project.setConnections(conns);

    // 3. 迁移记录
    MigrationRecord rec;
    rec.originalVersion = originalVersion;
    rec.migratedAt = QDateTime::currentDateTime();
    rec.warnings = report.warnings + report.autoFixed + report.blockers;
    project.setMigration(rec);

    project.setModified(true);
    return report;
}

bool ProjectMigrator::saveWithBackup(Project& project, const QString& path) {
    QFile existing(path);
    if (existing.exists()) {
        if (existing.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(existing.readAll());
            existing.close();
            const QString oldVersion = doc.object()["version"].toString(QStringLiteral("2.0"));
            if (oldVersion != QStringLiteral("3.0")) {
                QFile::remove(path + QStringLiteral(".v2.bak"));
                QFile::copy(path, path + QStringLiteral(".v2.bak"));
            }
        }
    }
    return project.save(path);
}

} // namespace DeepLux
