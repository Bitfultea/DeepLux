#pragma once

#include <QObject>
#include <QStringList>
#include <QMap>
#include <QJsonObject>
#include <QMutex>
#include <QWaitCondition>
#include <QSet>
#include <QPluginLoader>
#include <QTimer>
#include <QThread>
#include <QEvent>
#include <QCoreApplication>
#include <QThreadPool>
#include <QRunnable>
#include <memory>

namespace DeepLux {

class IModule;
class ICamera;


/**
 * @brief 插件信息
 */
struct PluginInfo {
    QString name;           // 插件名称
    QString version;        // 版本
    QString category;       // 分类
    QString path;           // 路径
    QString description;    // 描述
    QString author;         // 作者
    bool loaded = false;    // 是否已加载
    QString error;          // 错误信息
};

/**
 * @brief 插件管理器
 *
 * 负责扫描、加载、管理插件
 */
class PluginManager : public QObject
{
    Q_OBJECT

public:
    static PluginManager& instance();

    // 初始化
    bool initialize();
    void shutdown();
    bool isInitialized() const { return m_initialized; }

    // 插件路径管理
    void addPluginPath(const QString& path);
    QStringList pluginPaths() const;

    // 扫描插件
    void scanPlugins();

    // 加载/卸载
    bool loadPlugin(const QString& name, int timeoutMs = 10000);
    void unloadPlugin(const QString& name);
    void loadAllPlugins();
    void loadAllPluginsAsync();  // 异步加载所有插件，不阻塞主线程
    void unloadAllPlugins();

    // 查询
    QStringList availableModules() const;
    QStringList availableCameras() const;
    QList<PluginInfo> moduleInfos() const;
    QList<PluginInfo> cameraInfos() const;
    PluginInfo pluginInfo(const QString& name) const;
    bool isPluginLoaded(const QString& name) const;

    // 创建实例
    IModule* createModule(const QString& name);
    ICamera* createCamera(const QString& name);

signals:
    void pluginLoaded(const QString& name);
    void pluginUnloaded(const QString& name);
    void scanCompleted();
    void allPluginsLoaded();  // 所有插件异步加载完成
    void pluginLoadProgress(int current, int total, const QString& name);  // 加载进度
    void pluginLoadFailed(const QString& name, const QString& error);  // 加载失败
    void errorOccurred(const QString& error);

private slots:
    void onLoadTimer();

private:
    PluginManager();
    ~PluginManager();

    bool loadPluginMetadata(const QString& path, PluginInfo& info);
    void loadNextPluginAsync();

    // 自定义事件，用于线程安全地传递插件加载结果
    class PluginLoadEvent : public QEvent {
    public:
        QString name;
        bool success;
        QString error;
        QPluginLoader* loader;
        PluginLoadEvent(const QString& n, bool s, const QString& e, QPluginLoader* l)
            : QEvent(QEvent::User), name(n), success(s), error(e), loader(l) {}
    };
    bool event(QEvent* event) override;

    // 插件加载任务
    class PluginLoadRunnable : public QRunnable {
    public:
        PluginLoadRunnable(const QString& name, const QString& path);
        void run() override;
    private:
        QString m_name;
        QString m_path;
    };

    mutable QMutex m_mutex{QMutex::Recursive};
    QStringList m_pluginPaths;
    QMap<QString, PluginInfo> m_modules;
    QMap<QString, PluginInfo> m_cameras;
    QMap<QString, QObject*> m_loadedPlugins;
    QMap<QString, QPluginLoader*> m_pluginLoaders;
    // 正在加载的插件集合，用于避免重复加载
    QSet<QString> m_loadingPlugins;
    // 等待加载完成的条件变量
    QWaitCondition m_loadingComplete;
    // Template instances for cloning (each plugin provides one template)
    QMap<QString, IModule*> m_moduleTemplates;
    // Create a fresh module instance without cloning (internal use)
    IModule* createFreshModule(const QString& name);
    bool m_initialized = false;
    // 异步加载相关
    QTimer* m_loadTimer = nullptr;
    QStringList m_pendingPlugins;
    int m_loadingTotal = 0;
    int m_loadingCurrent = 0;
    QThreadPool* m_threadPool = nullptr;
};

} // namespace DeepLux
