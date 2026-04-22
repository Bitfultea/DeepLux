#include "SystemConfig.h"
#include "common/Logger.h"
#include "platform/PathUtils.h"
#include <QFile>
#include <QJsonDocument>

namespace DeepLux {

SystemConfig& SystemConfig::instance()
{
    static SystemConfig instance;
    return instance;
}

SystemConfig::SystemConfig()
{
    setDefaults();
    load();
}

SystemConfig::~SystemConfig()
{
    save();
}

void SystemConfig::setDefaults()
{
    m_autoLoadSolution = false;
    m_solutionPath = PathUtils::projectPath();
    m_autoRun = false;
    m_theme = "dark";
    m_language = "zh_CN";
    m_maxCameras = 4;
    m_logLevel = 1;
    m_maxLogFiles = 30;
    m_autoLoadLayout = true;
    m_layoutFile = PathUtils::configPath() + "/layout.xml";
    m_projectPath = PathUtils::projectPath();
    m_autoSave = false;
    m_autoSaveInterval = 300;
}

bool SystemConfig::load()
{
    QString configPath = PathUtils::configPath() + "/system.json";
    QFile file(configPath);

    if (!file.exists()) {
        Logger::instance().info("System config not found, using defaults", "Config");
        return false;
    }

    if (!file.open(QIODevice::ReadOnly)) {
        Logger::instance().error(QString("Failed to open config file: %1").arg(configPath), "Config");
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);

    if (error.error != QJsonParseError::NoError) {
        Logger::instance().error(QString("Failed to parse config: %1").arg(error.errorString()), "Config");
        return false;
    }

    loadFromJson(doc.object());
    Logger::instance().info("System config loaded", "Config");
    return true;
}

bool SystemConfig::save()
{
    QString configPath = PathUtils::configPath() + "/system.json";
    QFile file(configPath);

    if (!file.open(QIODevice::WriteOnly)) {
        Logger::instance().error(QString("Failed to open config file for writing: %1").arg(configPath), "Config");
        return false;
    }

    QJsonDocument doc(saveToJson());
    file.write(doc.toJson());
    file.close();

    Logger::instance().debug("System config saved", "Config");
    return true;
}

void SystemConfig::loadFromJson(const QJsonObject& json)
{
    // 自动加载
    m_autoLoadSolution = json["autoLoadSolution"].toBool(m_autoLoadSolution);
    m_solutionPath = json["solutionPath"].toString(m_solutionPath);
    m_autoRun = json["autoRun"].toBool(m_autoRun);

    // 外观
    m_theme = json["theme"].toString(m_theme);
    m_language = json["language"].toString(m_language);

    // 硬件
    m_maxCameras = json["maxCameras"].toInt(m_maxCameras);

    // 日志
    m_logLevel = json["logLevel"].toInt(m_logLevel);
    m_maxLogFiles = json["maxLogFiles"].toInt(m_maxLogFiles);

    // 布局
    m_autoLoadLayout = json["autoLoadLayout"].toBool(m_autoLoadLayout);
    m_layoutFile = json["layoutFile"].toString(m_layoutFile);

    // 项目
    m_projectPath = json["projectPath"].toString(m_projectPath);
    m_autoSave = json["autoSave"].toBool(m_autoSave);
    m_autoSaveInterval = json["autoSaveInterval"].toInt(m_autoSaveInterval);

    // 文件日志
    m_enableFileLog = json["enableFileLog"].toBool(m_enableFileLog);

    // 运行
    m_cycleInterval = json["cycleInterval"].toInt(m_cycleInterval);
}

QJsonObject SystemConfig::saveToJson() const
{
    QJsonObject json;

    // 自动加载
    json["autoLoadSolution"] = m_autoLoadSolution;
    json["solutionPath"] = m_solutionPath;
    json["autoRun"] = m_autoRun;

    // 外观
    json["theme"] = m_theme;
    json["language"] = m_language;

    // 硬件
    json["maxCameras"] = m_maxCameras;

    // 日志
    json["logLevel"] = m_logLevel;
    json["maxLogFiles"] = m_maxLogFiles;

    // 布局
    json["autoLoadLayout"] = m_autoLoadLayout;
    json["layoutFile"] = m_layoutFile;

    // 项目
    json["projectPath"] = m_projectPath;
    json["autoSave"] = m_autoSave;
    json["autoSaveInterval"] = m_autoSaveInterval;

    // 文件日志
    json["enableFileLog"] = m_enableFileLog;

    // 运行
    json["cycleInterval"] = m_cycleInterval;

    return json;
}

void SystemConfig::setAutoLoadSolution(bool enabled)
{
    if (m_autoLoadSolution != enabled) {
        m_autoLoadSolution = enabled;
        emit autoLoadSolutionChanged();
        emit configChanged();
    }
}

void SystemConfig::setSolutionPath(const QString& path)
{
    if (m_solutionPath != path) {
        m_solutionPath = path;
        emit solutionPathChanged();
        emit configChanged();
    }
}

void SystemConfig::setAutoRun(bool enabled)
{
    if (m_autoRun != enabled) {
        m_autoRun = enabled;
        emit autoRunChanged();
        emit configChanged();
    }
}

void SystemConfig::setTheme(const QString& theme)
{
    if (m_theme != theme) {
        m_theme = theme;
        emit themeChanged();
        emit configChanged();
    }
}

void SystemConfig::setLanguage(const QString& lang)
{
    if (m_language != lang) {
        m_language = lang;
        emit languageChanged();
        emit configChanged();
    }
}

void SystemConfig::setMaxCameras(int max)
{
    if (m_maxCameras != max) {
        m_maxCameras = max;
        emit maxCamerasChanged();
        emit configChanged();
    }
}

void SystemConfig::setLogLevel(int level)
{
    if (m_logLevel != level) {
        m_logLevel = level;
        emit logLevelChanged();
        emit configChanged();
    }
}

void SystemConfig::setMaxLogFiles(int max)
{
    if (m_maxLogFiles != max) {
        m_maxLogFiles = max;
        emit maxLogFilesChanged();
        emit configChanged();
    }
}

void SystemConfig::setAutoLoadLayout(bool enabled)
{
    if (m_autoLoadLayout != enabled) {
        m_autoLoadLayout = enabled;
        emit autoLoadLayoutChanged();
        emit configChanged();
    }
}

void SystemConfig::setLayoutFile(const QString& path)
{
    if (m_layoutFile != path) {
        m_layoutFile = path;
        emit layoutFileChanged();
        emit configChanged();
    }
}

void SystemConfig::setProjectPath(const QString& path)
{
    if (m_projectPath != path) {
        m_projectPath = path;
        emit projectPathChanged();
        emit configChanged();
    }
}

void SystemConfig::setAutoSave(bool enabled)
{
    if (m_autoSave != enabled) {
        m_autoSave = enabled;
        emit autoSaveChanged();
        emit configChanged();
    }
}

void SystemConfig::setAutoSaveInterval(int seconds)
{
    if (m_autoSaveInterval != seconds) {
        m_autoSaveInterval = seconds;
        emit autoSaveIntervalChanged();
        emit configChanged();
    }
}

void SystemConfig::setEnableFileLog(bool enabled)
{
    if (m_enableFileLog != enabled) {
        m_enableFileLog = enabled;
        emit enableFileLogChanged();
        emit configChanged();
    }
}

void SystemConfig::setCycleInterval(int ms)
{
    if (m_cycleInterval != ms) {
        m_cycleInterval = ms;
        emit cycleIntervalChanged();
        emit configChanged();
    }
}

void SystemConfig::reset()
{
    setDefaults();
    emit configChanged();
}

} // namespace DeepLux