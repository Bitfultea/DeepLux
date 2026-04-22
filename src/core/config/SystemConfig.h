#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>

namespace DeepLux {

// Forward declaration - LogLevel is defined in Logger.h

/**
 * @brief 系统配置管理器
 *
 * 管理软件的系统配置，包括：
 * - 自动加载
 * - 快捷键配置
 * - 显示配置
 * - 保存路径等
 */
class SystemConfig : public QObject
{
    Q_OBJECT

public:
    static SystemConfig& instance();

    // 加载/保存
    bool load();
    bool save();
    void reset();

    // 自动加载配置
    Q_PROPERTY(bool autoLoadSolution READ autoLoadSolution WRITE setAutoLoadSolution NOTIFY autoLoadSolutionChanged)
    bool autoLoadSolution() const { return m_autoLoadSolution; }
    void setAutoLoadSolution(bool enabled);

    Q_PROPERTY(QString solutionPath READ solutionPath WRITE setSolutionPath NOTIFY solutionPathChanged)
    QString solutionPath() const { return m_solutionPath; }
    void setSolutionPath(const QString& path);

    Q_PROPERTY(bool autoRun READ autoRun WRITE setAutoRun NOTIFY autoRunChanged)
    bool autoRun() const { return m_autoRun; }
    void setAutoRun(bool enabled);

    // 主题配置
    Q_PROPERTY(QString theme READ theme WRITE setTheme NOTIFY themeChanged)
    QString theme() const { return m_theme; }
    void setTheme(const QString& theme);

    // 语言配置
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)
    QString language() const { return m_language; }
    void setLanguage(const QString& lang);

    // 相机配置
    Q_PROPERTY(int maxCameras READ maxCameras WRITE setMaxCameras NOTIFY maxCamerasChanged)
    int maxCameras() const { return m_maxCameras; }
    void setMaxCameras(int max);

    // 日志配置
    Q_PROPERTY(int logLevel READ logLevel WRITE setLogLevel NOTIFY logLevelChanged)
    int logLevel() const { return m_logLevel; }
    void setLogLevel(int level);

    Q_PROPERTY(int maxLogFiles READ maxLogFiles WRITE setMaxLogFiles NOTIFY maxLogFilesChanged)
    int maxLogFiles() const { return m_maxLogFiles; }
    void setMaxLogFiles(int max);

    // 布局配置
    Q_PROPERTY(bool autoLoadLayout READ autoLoadLayout WRITE setAutoLoadLayout NOTIFY autoLoadLayoutChanged)
    bool autoLoadLayout() const { return m_autoLoadLayout; }
    void setAutoLoadLayout(bool enabled);

    Q_PROPERTY(QString layoutFile READ layoutFile WRITE setLayoutFile NOTIFY layoutFileChanged)
    QString layoutFile() const { return m_layoutFile; }
    void setLayoutFile(const QString& path);

    // 项目配置
    Q_PROPERTY(QString projectPath READ projectPath WRITE setProjectPath NOTIFY projectPathChanged)
    QString projectPath() const { return m_projectPath; }
    void setProjectPath(const QString& path);

    Q_PROPERTY(bool autoSave READ autoSave WRITE setAutoSave NOTIFY autoSaveChanged)
    bool autoSave() const { return m_autoSave; }
    void setAutoSave(bool enabled);

    Q_PROPERTY(int autoSaveInterval READ autoSaveInterval WRITE setAutoSaveInterval NOTIFY autoSaveIntervalChanged)
    int autoSaveInterval() const { return m_autoSaveInterval; }
    void setAutoSaveInterval(int seconds);

    Q_PROPERTY(bool enableFileLog READ enableFileLog WRITE setEnableFileLog NOTIFY enableFileLogChanged)
    bool enableFileLog() const { return m_enableFileLog; }
    void setEnableFileLog(bool enabled);

    Q_PROPERTY(int cycleInterval READ cycleInterval WRITE setCycleInterval NOTIFY cycleIntervalChanged)
    int cycleInterval() const { return m_cycleInterval; }
    void setCycleInterval(int ms);

signals:
    void configChanged();
    void autoLoadSolutionChanged();
    void solutionPathChanged();
    void autoRunChanged();
    void themeChanged();
    void languageChanged();
    void maxCamerasChanged();
    void logLevelChanged();
    void maxLogFilesChanged();
    void autoLoadLayoutChanged();
    void layoutFileChanged();
    void projectPathChanged();
    void autoSaveChanged();
    void autoSaveIntervalChanged();
    void enableFileLogChanged();
    void cycleIntervalChanged();

private:
    SystemConfig();
    ~SystemConfig();

    void setDefaults();
    void loadFromJson(const QJsonObject& json);
    QJsonObject saveToJson() const;

    // 自动加载
    bool m_autoLoadSolution = false;
    QString m_solutionPath;
    bool m_autoRun = false;

    // 外观
    QString m_theme = "dark";
    QString m_language = "zh_CN";

    // 硬件
    int m_maxCameras = 4;

    // 日志
    int m_logLevel = 2; // 0=Debug, 1=Info, 2=Warn, 3=Error
    int m_maxLogFiles = 30;

    // 布局
    bool m_autoLoadLayout = true;
    QString m_layoutFile;

    // 项目
    QString m_projectPath;
    bool m_autoSave = false;
    int m_autoSaveInterval = 300; // 5分钟

    // 文件日志
    bool m_enableFileLog = true;

    // 运行
    int m_cycleInterval = 100; // 100ms
};

} // namespace DeepLux