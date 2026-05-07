#include "PluginManager.h"
#include "../interface/IModule.h"
#include "../interface/ICamera.h"
#include "../interface/ICameraPlugin.h"
#include "../device/CameraManager.h"
#include "../platform/PathUtils.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPluginLoader>
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QCoreApplication>

namespace DeepLux {

PluginManager& PluginManager::instance()
{
    static PluginManager instance;
    return instance;
}

PluginManager::PluginManager()
{
    m_loadTimer = new QTimer(this);
    m_loadTimer->setSingleShot(false);
    m_loadTimer->setInterval(10);
    connect(m_loadTimer, &QTimer::timeout, this, &PluginManager::onLoadTimer);
}

PluginManager::~PluginManager()
{
    if (m_loadTimer) {
        m_loadTimer->stop();
    }
    unloadAllPlugins();
}

void PluginManager::addPluginPath(const QString& path)
{
    QMutexLocker locker(&m_mutex);
    if (!m_pluginPaths.contains(path)) {
        m_pluginPaths.append(path);
    }
}

QStringList PluginManager::pluginPaths() const
{
    QMutexLocker locker(&m_mutex);
    return m_pluginPaths;
}

bool PluginManager::initialize()
{
    if (m_initialized) {
        return true;
    }

    qDebug() << "Initializing plugin manager...";

    // 扫描所有插件路径
    scanPlugins();

    m_initialized = true;
    qDebug() << "Plugin manager initialized";
    emit scanCompleted();
    return true;
}

void PluginManager::shutdown()
{
    unloadAllPlugins();
    m_initialized = false;
}

void PluginManager::scanPlugins()
{
    QMutexLocker locker(&m_mutex);

    QStringList searchPaths = m_pluginPaths;
    QString defaultPath = PathUtils::pluginPath();
    if (!searchPaths.contains(defaultPath)) {
        searchPaths.append(defaultPath);
    }
    qDebug() << "Plugin search paths:" << searchPaths;

    // 将默认路径持久化到 m_pluginPaths，确保 pluginPaths() 能正确返回
    if (!searchPaths.isEmpty() && m_pluginPaths.isEmpty()) {
        m_pluginPaths = searchPaths;
    }

    for (const QString& basePath : searchPaths) {
        QDir dir(basePath);
        if (!dir.exists()) {
            continue;
        }

        QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo& subDir : subDirs) {
            QString pluginPath = subDir.absoluteFilePath();
            QString metadataPath = pluginPath + "/metadata.json";

            if (!QFile::exists(metadataPath)) {
                continue;
            }

            PluginInfo info;
            if (loadPluginMetadata(metadataPath, info)) {
                if (info.category.toLower().contains("camera")) {
                    m_cameras[info.name] = info;
                } else {
                    m_modules[info.name] = info;
                }
                qDebug() << "Found plugin:" << info.name << "at" << pluginPath;
            }
        }
    }

    qDebug() << "Modules found:" << m_modules.size();
    qDebug() << "Cameras found:" << m_cameras.size();
}

bool PluginManager::loadPluginMetadata(const QString& path, PluginInfo& info)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError) {
        return false;
    }

    QJsonObject json = doc.object();

    info.name = json["name"].toString();
    info.version = json["version"].toString();
    info.category = json["category"].toString();
    info.description = json["description"].toString();
    info.author = json["author"].toString();
    info.path = path;
    info.loaded = false;

    return !info.name.isEmpty();
}

bool PluginManager::loadPlugin(const QString& name, int timeoutMs)
{
    Q_UNUSED(timeoutMs);
    fprintf(stderr, "DEBUG: loadPlugin called for: %s\n", qPrintable(name));

    {
        QMutexLocker locker(&m_mutex);
        if (m_loadedPlugins.contains(name)) {
            qDebug() << "Plugin already loaded:" << name;
            return true;
        }
    }

    PluginInfo info;

    {
        QMutexLocker locker(&m_mutex);
        if (m_modules.contains(name)) {
            info = m_modules[name];
        } else if (m_cameras.contains(name)) {
            info = m_cameras[name];
        } else {
            qWarning() << "Plugin not found:" << name;
            return false;
        }
    }

    if (info.path.isEmpty()) {
        fprintf(stderr, "DEBUG: info.path is empty for %s, returning false\n", qPrintable(name));
        return false;
    }

    // info.path is the metadata.json path, get the directory containing it
    QDir dir(QFileInfo(info.path).absoluteDir());
    QStringList filters;
#ifdef Q_OS_WIN
    filters << "*.dll";
#else
    filters << "*.so";
#endif
    QStringList libs = dir.entryList(filters, QDir::Files);
    if (libs.isEmpty()) {
        fprintf(stderr, "DEBUG: No .so files found in %s for %s\n", qPrintable(info.path), qPrintable(name));
        return false;
    }
    QString libPath = dir.filePath(libs.first());

        QPluginLoader* loader = new QPluginLoader(libPath);
    fprintf(stderr, "DEBUG: Loading plugin from: %s\n", qPrintable(libPath));
    bool loaded = loader->load();
    fprintf(stderr, "DEBUG: load() returned: %d\n", loaded);
    if (loaded) {
        QMutexLocker locker(&m_mutex);
        m_loadedPlugins[name] = loader->instance();
        m_pluginLoaders[name] = loader;

        // 更新 info.loaded 并写回字典
        info.loaded = true;
        if (m_modules.contains(name)) {
            m_modules[name] = info;
            fprintf(stderr, "DEBUG: Updated m_modules[%s] with loaded=true\n", qPrintable(name));
        } else if (m_cameras.contains(name)) {
            m_cameras[name] = info;
            fprintf(stderr, "DEBUG: Updated m_cameras[%s] with loaded=true\n", qPrintable(name));
        } else {
            fprintf(stderr, "DEBUG: WARNING: Plugin %s not found in m_modules or m_cameras!\n", qPrintable(name));
        }

        qDebug() << "Plugin loaded:" << name;
        emit pluginLoaded(name);

        // 如果是相机插件，注册到 CameraManager
        QObject* plugin = loader->instance();
        ICameraPlugin* cameraPlugin = qobject_cast<ICameraPlugin*>(plugin);
        if (cameraPlugin) {
            CameraManager::instance().registerCameraPlugin(cameraPlugin);
            qDebug() << "Camera plugin registered:" << name;
        }

        return true;
    } else {
        QString error = loader->errorString();
        fprintf(stderr, "DEBUG: Plugin load failed: %s - %s\n", qPrintable(name), qPrintable(error));
        qWarning() << "Plugin load failed:" << name << error;
        delete loader;
        return false;
    }
}

void PluginManager::unloadPlugin(const QString& name)
{
    QMutexLocker locker(&m_mutex);

    if (!m_loadedPlugins.contains(name)) {
        return;
    }

    m_loadedPlugins.remove(name);

    if (m_pluginLoaders.contains(name)) {
        QPluginLoader* loader = m_pluginLoaders.take(name);
        loader->unload();
        delete loader;
    }

    emit pluginUnloaded(name);
}

void PluginManager::loadAllPlugins()
{
    qDebug() << "Loading all plugins synchronously...";

    QStringList moduleNames;
    QStringList cameraNames;
    {
        QMutexLocker locker(&m_mutex);
        moduleNames = m_modules.keys();
        cameraNames = m_cameras.keys();
    }

    for (const QString& name : moduleNames) {
        loadPlugin(name, 5000);
    }

    for (const QString& name : cameraNames) {
        loadPlugin(name, 5000);
    }

    qDebug() << "All plugins loaded";
}

void PluginManager::loadAllPluginsAsync()
{
    QStringList moduleNames;
    QStringList cameraNames;
    {
        QMutexLocker locker(&m_mutex);
        moduleNames = m_modules.keys();
        cameraNames = m_cameras.keys();
    }

    m_pendingPlugins.clear();
    for (const QString& name : moduleNames) {
        if (!m_loadedPlugins.contains(name)) {
            m_pendingPlugins.append(name);
        }
    }
    for (const QString& name : cameraNames) {
        if (!m_loadedPlugins.contains(name)) {
            m_pendingPlugins.append(name);
        }
    }

    m_loadingTotal = m_pendingPlugins.size();
    m_loadingCurrent = 0;

    if (m_loadingTotal == 0) {
        emit allPluginsLoaded();
        return;
    }

    // 串行加载每个插件，每加载一个后立即处理事件以更新UI
    for (const QString& name : m_pendingPlugins) {
        QString pluginDir;
        {
            QMutexLocker locker(&m_mutex);
            if (m_modules.contains(name)) {
                // info.path 是 metadata.json 的路径，需要提取目录
                pluginDir = QFileInfo(m_modules[name].path).absolutePath();
            } else if (m_cameras.contains(name)) {
                pluginDir = QFileInfo(m_cameras[name].path).absolutePath();
            }
        }

        if (!pluginDir.isEmpty()) {
            QDir dir(pluginDir);
            QStringList filters;
#ifdef Q_OS_WIN
            filters << "*.dll";
#else
            filters << "*.so";
#endif
            QStringList libs = dir.entryList(filters, QDir::Files);
            if (!libs.isEmpty()) {
                QString fullPath = dir.filePath(libs.first());
                QPluginLoader* loader = new QPluginLoader(fullPath);
                bool success = loader->load();
                QString error = success ? QString() : loader->errorString();

                m_loadingCurrent++;
                if (success) {
                    QMutexLocker locker(&m_mutex);
                    m_loadedPlugins[name] = loader->instance();
                    m_pluginLoaders[name] = loader;
                    emit pluginLoaded(name);

                    // 如果是相机插件，注册到 CameraManager
                    ICameraPlugin* cameraPlugin = qobject_cast<ICameraPlugin*>(loader->instance());
                    if (cameraPlugin) {
                        CameraManager::instance().registerCameraPlugin(cameraPlugin);
                        qDebug() << "Camera plugin registered:" << name;
                    }
                } else {
                    emit pluginLoadFailed(name, error);
                    delete loader;
                }
                emit pluginLoadProgress(m_loadingCurrent, m_loadingTotal, name);

                // 立即处理事件以更新UI
                QCoreApplication::processEvents();
            } else {
                m_loadingCurrent++;
                emit pluginLoadFailed(name, "No library file found");
                emit pluginLoadProgress(m_loadingCurrent, m_loadingTotal, name);
                QCoreApplication::processEvents();
            }
        } else {
            m_loadingCurrent++;
            emit pluginLoadFailed(name, "Plugin path not found");
            emit pluginLoadProgress(m_loadingCurrent, m_loadingTotal, name);
            QCoreApplication::processEvents();
        }
    }

    emit allPluginsLoaded();
}

// PluginLoadRunnable implementation
PluginManager::PluginLoadRunnable::PluginLoadRunnable(const QString& name, const QString& path)
    : m_name(name), m_path(path)
{
}

void PluginManager::PluginLoadRunnable::run()
{
    QPluginLoader* loader = new QPluginLoader(m_path);
    bool success = loader->load();
    QString error = success ? QString() : loader->errorString();

    // 通过事件通知主线程
    QCoreApplication::postEvent(&PluginManager::instance(),
                                new PluginLoadEvent(m_name, success, error, loader));
}

bool PluginManager::event(QEvent* event)
{
    if (event->type() == QEvent::User) {
        auto* loadEvent = static_cast<PluginLoadEvent*>(event);
        m_loadingCurrent++;

        if (loadEvent->success) {
            QMutexLocker locker(&m_mutex);
            m_loadedPlugins[loadEvent->name] = loadEvent->loader->instance();
            m_pluginLoaders[loadEvent->name] = loadEvent->loader;
            emit pluginLoaded(loadEvent->name);

            // 如果是相机插件，注册到 CameraManager
            ICameraPlugin* cameraPlugin = qobject_cast<ICameraPlugin*>(loadEvent->loader->instance());
            if (cameraPlugin) {
                CameraManager::instance().registerCameraPlugin(cameraPlugin);
                qDebug() << "Camera plugin registered:" << loadEvent->name;
            }
        } else {
            emit pluginLoadFailed(loadEvent->name, loadEvent->error);
            delete loadEvent->loader;
        }

        emit pluginLoadProgress(m_loadingCurrent, m_loadingTotal, loadEvent->name);

        // 检查是否全部完成
        if (m_loadingCurrent >= m_loadingTotal) {
            emit allPluginsLoaded();
        }
        return true;
    }
    return QObject::event(event);
}

void PluginManager::onLoadTimer()
{
    // 空实现，保持接口兼容
}

void PluginManager::unloadAllPlugins()
{
    QStringList names;
    {
        QMutexLocker locker(&m_mutex);
        names = m_loadedPlugins.keys();
    }

    for (const QString& name : names) {
        unloadPlugin(name);
    }
}

QStringList PluginManager::availableModules() const
{
    QMutexLocker locker(&m_mutex);
    return m_modules.keys();
}

QStringList PluginManager::availableCameras() const
{
    QMutexLocker locker(&m_mutex);
    return m_cameras.keys();
}

QList<PluginInfo> PluginManager::moduleInfos() const
{
    QMutexLocker locker(&m_mutex);
    return m_modules.values();
}

QList<PluginInfo> PluginManager::cameraInfos() const
{
    QMutexLocker locker(&m_mutex);
    return m_cameras.values();
}

PluginInfo PluginManager::pluginInfo(const QString& name) const
{
    QMutexLocker locker(&m_mutex);

    if (m_modules.contains(name)) {
        return m_modules[name];
    }
    if (m_cameras.contains(name)) {
        return m_cameras[name];
    }
    return PluginInfo();
}

bool PluginManager::isPluginLoaded(const QString& name) const
{
    QMutexLocker locker(&m_mutex);
    return m_loadedPlugins.contains(name);
}

IModule* PluginManager::createModule(const QString& name)
{
    auto diag = [](const QString& msg) {
        QFile f("/tmp/deeplux_agent_diag.log");
        f.open(QIODevice::WriteOnly | QIODevice::Append);
        f.write(QString("[DIAG-PM] %1\n").arg(msg).toUtf8());
        f.close();
        qDebug() << "[DIAG-PM]" << msg;
    };

    diag(QString("createModule start: %1").arg(name));

    if (!isPluginLoaded(name)) {
        diag(QString("createModule: %1 not loaded, skipping (loadAllPluginsAsync already attempted)").arg(name));
        qWarning() << "Plugin not loaded and will not be reloaded:" << name;
        return nullptr;
    }
    diag(QString("createModule: %1 already loaded").arg(name));

    diag(QString("createModule: acquiring mutex for %1").arg(name));
    QMutexLocker locker(&m_mutex);
    diag(QString("createModule: mutex acquired for %1").arg(name));

    IModule* templateModule = m_moduleTemplates.value(name);
    if (!templateModule) {
        diag(QString("createModule: templateModule not cached, looking in m_loadedPlugins").arg(name));
        QObject* plugin = m_loadedPlugins.value(name);
        if (!plugin) {
            diag(QString("createModule: plugin not found in m_loadedPlugins for %1").arg(name));
            return nullptr;
        }
        diag(QString("createModule: qobject_cast for %1").arg(name));
        templateModule = qobject_cast<IModule*>(plugin);
        if (!templateModule) {
            qWarning() << "Plugin is not a valid module:" << name;
            diag(QString("createModule: qobject_cast failed for %1").arg(name));
            return nullptr;
        }
        m_moduleTemplates[name] = templateModule;
        diag(QString("createModule: templateModule cached for %1").arg(name));
    } else {
        diag(QString("createModule: templateModule cached for %1").arg(name));
    }

    diag(QString("createModule: calling clone for %1").arg(name));
    IModule* newInstance = templateModule->clone();
    diag(QString("createModule: clone returned %1 for %2").arg(reinterpret_cast<quintptr>(newInstance)).arg(name));
    if (newInstance) return newInstance;

    // clone 返回 null（插件未实现 cloneImpl）→ 用 createFreshModule 回退
    diag(QString("createModule: clone failed, falling back to createFreshModule for %1").arg(name));
    return createFreshModule(name);
}

IModule* PluginManager::createFreshModule(const QString& name)
{
    QObject* plugin = m_loadedPlugins.value(name);
    if (!plugin) {
        return nullptr;
    }
    return qobject_cast<IModule*>(plugin);
}

ICamera* PluginManager::createCamera(const QString& name)
{
    if (!isPluginLoaded(name)) {
        if (!loadPlugin(name, 5000)) {
            return nullptr;
        }
    }

    QMutexLocker locker(&m_mutex);
    QObject* plugin = m_loadedPlugins.value(name);
    if (!plugin) {
        return nullptr;
    }
    return qobject_cast<ICamera*>(plugin);
}

} // namespace DeepLux
