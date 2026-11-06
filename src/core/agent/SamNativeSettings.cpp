#include "SamNativeSettings.h"

#include "core/manager/ConfigManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QThread>

namespace DeepLux {
namespace {
QMutex g_mutex;
QString g_packageDir;
QString g_device = QStringLiteral("auto");
bool g_initialized = false;

QString bundledPackagePath() {
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates = {appDir.absoluteFilePath(QStringLiteral("../models/sam1_vit_b_onnx")),
                                    appDir.absoluteFilePath(QStringLiteral("models/sam1_vit_b_onnx")),
                                    QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                                        .absoluteFilePath(QStringLiteral("models/sam1_vit_b_onnx"))};
    for (const QString& candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.isDir())
            return info.canonicalFilePath();
    }
    return {};
}
} // namespace

void SamNativeSettings::initialize() {
    ConfigManager::instance().initialize();
    const QString packageDir =
        ConfigManager::instance().groupString(QStringLiteral("sam"), QStringLiteral("nativePackagePath"));
    QString device = ConfigManager::instance()
                         .groupString(QStringLiteral("sam"), QStringLiteral("nativeDevice"), QStringLiteral("auto"))
                         .trimmed()
                         .toLower();
    if (device != QLatin1String("auto") && device != QLatin1String("cpu") && device != QLatin1String("cuda"))
        device = QStringLiteral("auto");
    QMutexLocker lock(&g_mutex);
    g_packageDir = packageDir;
    g_device = device;
    g_initialized = true;
}

SamNativeConfig SamNativeSettings::snapshot() {
    SamNativeConfig config;
    {
        QMutexLocker lock(&g_mutex);
        if (g_initialized) {
            config.packageDir = g_packageDir;
            config.device = g_device;
        }
    }

    if (qEnvironmentVariableIsSet("DEEPLUX_SAM_PACKAGE")) {
        config.packageDir = qEnvironmentVariable("DEEPLUX_SAM_PACKAGE").trimmed();
        config.packageFromEnvironment = true;
    } else if (config.packageDir.isEmpty()) {
        config.packageDir = bundledPackagePath();
    }
    if (qEnvironmentVariableIsSet("DEEPLUX_SAM_DEVICE")) {
        config.device = qEnvironmentVariable("DEEPLUX_SAM_DEVICE").trimmed().toLower();
        config.deviceFromEnvironment = true;
    }

    if (config.packageDir.isEmpty())
        config.error = QStringLiteral("尚未配置 SAM ONNX 模型包");
    else if (!QFileInfo(config.packageDir).isDir())
        config.error = QStringLiteral("SAM ONNX 模型包目录不存在：%1").arg(config.packageDir);
    else if (config.device != QLatin1String("auto") && config.device != QLatin1String("cpu") &&
             config.device != QLatin1String("cuda"))
        config.error = QStringLiteral("DEEPLUX_SAM_DEVICE 仅支持 auto、cpu 或 cuda：%1").arg(config.device);
    return config;
}

bool SamNativeSettings::saveUserSettings(const QString& packageDir, const QString& device, QString* error) {
    const QString normalizedDevice = device.trimmed().toLower();
    if (!QFileInfo(packageDir).isDir()) {
        if (error)
            *error = QStringLiteral("模型包目录不存在");
        return false;
    }
    if (normalizedDevice != QLatin1String("auto") && normalizedDevice != QLatin1String("cpu") &&
        normalizedDevice != QLatin1String("cuda")) {
        if (error)
            *error = QStringLiteral("推理设备必须为 auto、cpu 或 cuda");
        return false;
    }
    if (QCoreApplication::instance() && QThread::currentThread() != QCoreApplication::instance()->thread()) {
        if (error)
            *error = QStringLiteral("SAM 设置只能从应用主线程保存");
        return false;
    }

    ConfigManager& config = ConfigManager::instance();
    config.initialize();
    const QString oldPath = config.groupString(QStringLiteral("sam"), QStringLiteral("nativePackagePath"));
    const QString oldDevice =
        config.groupString(QStringLiteral("sam"), QStringLiteral("nativeDevice"), QStringLiteral("auto"));
    const QString canonicalPath = QFileInfo(packageDir).canonicalFilePath();
    config.setGroupValue(QStringLiteral("sam"), QStringLiteral("nativePackagePath"), canonicalPath);
    config.setGroupValue(QStringLiteral("sam"), QStringLiteral("nativeDevice"), normalizedDevice);
    if (!config.save()) {
        config.setGroupValue(QStringLiteral("sam"), QStringLiteral("nativePackagePath"), oldPath);
        config.setGroupValue(QStringLiteral("sam"), QStringLiteral("nativeDevice"), oldDevice);
        config.save();
        if (error)
            *error = QStringLiteral("无法保存 SAM 设置");
        return false;
    }

    QMutexLocker lock(&g_mutex);
    g_packageDir = canonicalPath;
    g_device = normalizedDevice;
    g_initialized = true;
    return true;
}

} // namespace DeepLux
