#include "PluginManager.h"

#include "../base/ModuleBase.h"
#include "../common/ModuleIconProvider.h"
#include "../device/CameraManager.h"
#include "../interface/ICamera.h"
#include "../interface/ICameraPlugin.h"
#include "../interface/IModule.h"
#include "../platform/PathUtils.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPixmap>
#include <QPluginLoader>
#include <QThread>
#include <QTimer>

namespace DeepLux {

namespace {
bool hasGuiApplication() {
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}
} // namespace

PluginManager& PluginManager::instance() {
    static PluginManager instance;
    return instance;
}

PluginManager::PluginManager() {
    m_loadTimer = new QTimer(this);
    m_loadTimer->setSingleShot(false);
    m_loadTimer->setInterval(10);
    connect(m_loadTimer, &QTimer::timeout, this, &PluginManager::onLoadTimer);
}

PluginManager::~PluginManager() {
    if (m_loadTimer) {
        m_loadTimer->stop();
    }
    unloadAllPlugins();
}

void PluginManager::addPluginPath(const QString& path) {
    QMutexLocker locker(&m_mutex);
    if (!m_pluginPaths.contains(path)) {
        m_pluginPaths.append(path);
    }
}

QStringList PluginManager::pluginPaths() const {
    QMutexLocker locker(&m_mutex);
    return m_pluginPaths;
}

bool PluginManager::initialize() {
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

void PluginManager::shutdown() {
    unloadAllPlugins();
    m_initialized = false;
}

void PluginManager::scanPlugins() {
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

namespace {

// 解析单个端口数组（ports.inputs / ports.outputs）。返回 false 并填充 error 表示声明非法。
bool parsePortArray(const QJsonArray& arr, QList<PortSpec>& out, QString& error, const QString& pluginName,
                    const QString& section) {
    QSet<QString> seen;
    for (const QJsonValue& v : arr) {
        if (!v.isObject()) {
            error = QString("%1: port entry in %2 is not an object").arg(pluginName, section);
            return false;
        }
        QJsonObject o = v.toObject();
        PortSpec spec;
        spec.id = o["id"].toString();
        spec.displayName = o.value("displayName").toString(o.value("name").toString());
        const QString typeName = o.value("type").toString(QStringLiteral("Any"));

        if (spec.id.isEmpty()) {
            error = QString("%1: port in %2 missing id").arg(pluginName, section);
            return false;
        }
        if (spec.displayName.isEmpty()) {
            error = QString("%1: port %2 in %3 has empty display name").arg(pluginName, spec.id, section);
            return false;
        }
        if (seen.contains(spec.id)) {
            error = QString("%1: duplicate port id %2 in %3").arg(pluginName, spec.id, section);
            return false;
        }
        if (!dataTypeFromString(typeName, spec.type)) {
            error = QString("%1: port %2 has unknown type %3").arg(pluginName, spec.id, typeName);
            return false;
        }
        // 阶段 3：未实现类型（无载荷契约/生产者插件）不得声明为可运行端口，
        // 在加载阶段明确拒绝，避免"连接合法、运行期永远拒绝"的悬空端口。
        if (!isSupportedPortType(spec.type)) {
            error = QString("%1: port %2 declares unimplemented type %3 (rejected at load)")
                        .arg(pluginName, spec.id, typeName);
            return false;
        }
        spec.required = o.value("required").toBool(false);
        spec.multiple = o.value("multiple").toBool(false);
        spec.control = o.value("control").toBool(false);
        if (o.contains("joinPolicy") && (!spec.control || !spec.multiple)) {
            error =
                QString("%1: port %2 uses joinPolicy without control=true and multiple=true").arg(pluginName, spec.id);
            return false;
        }
        if (spec.control && spec.multiple) {
            const QString policy = o.value("joinPolicy").toString("any").toLower();
            if (policy != QLatin1String("any") && policy != QLatin1String("all")) {
                error = QString("%1: port %2 has unknown joinPolicy %3").arg(pluginName, spec.id, policy);
                return false;
            }
            spec.joinPolicy = policy == QLatin1String("all") ? ControlJoinPolicy::All : ControlJoinPolicy::Any;
        }
        seen.insert(spec.id);
        out.append(spec);
    }
    return true;
}

} // namespace

bool PluginManager::loadPluginMetadata(const QString& path, PluginInfo& info) {
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
    info.id = json["id"].toString();
    info.version = json["version"].toString();
    info.category = json["category"].toString();
    info.description = json["description"].toString();
    info.author = json["author"].toString();
    info.icon = json["icon"].toString();
    info.path = path;
    info.loaded = false;
    info.ui = json["ui"].toObject();
    info.requirements = QJsonObject();
    info.requirementMetadataErrors.clear();
    for (const QString& key :
         {QStringLiteral("abi"), QStringLiteral("sdk"), QStringLiteral("model"), QStringLiteral("dependencies")}) {
        if (json.contains(key))
            info.requirements.insert(key, json.value(key));
    }
    if (json.contains(QStringLiteral("platforms"))) {
        const QJsonValue platforms = json.value(QStringLiteral("platforms"));
        bool valid = platforms.isArray();
        if (valid) {
            for (const QJsonValue& platform : platforms.toArray()) {
                if (!platform.isString()) {
                    valid = false;
                    break;
                }
            }
        }
        if (valid) {
            info.requirements.insert(QStringLiteral("platforms"), platforms);
        } else {
            info.requirementMetadataErrors.append(QStringLiteral("platforms"));
        }
    } else if (json.contains(QStringLiteral("platform"))) {
        const QJsonValue platform = json.value(QStringLiteral("platform"));
        if (platform.isString())
            info.requirements.insert(QStringLiteral("platforms"), QJsonArray{platform});
        else
            info.requirementMetadataErrors.append(QStringLiteral("platforms"));
    }

    // ABI v2 模块必须有完整 ports 声明；错误元数据不可进入可加载列表。
    info.inputPorts.clear();
    info.outputPorts.clear();
    info.portsDeclared = false;
    QJsonObject ports = json["ports"].toObject();
    QString portError;
    if (!info.category.toLower().contains(QStringLiteral("camera"))) {
        if (ports.isEmpty() || !ports.contains("inputs") || !ports.contains("outputs")) {
            info.error =
                QStringLiteral("%1: ABI v2 module metadata requires ports.inputs and ports.outputs").arg(info.name);
            qWarning() << info.error;
            return false;
        }
        // 字段必须是 JSON 数组：字符串/对象经 toArray() 会得到空数组，
        // 若放行会把插件当作"零端口"合法加载。
        if (!ports.value("inputs").isArray()) {
            info.error = QStringLiteral("%1: ports.inputs must be a JSON array").arg(info.name);
            qWarning() << info.error;
            return false;
        }
        if (!ports.value("outputs").isArray()) {
            info.error = QStringLiteral("%1: ports.outputs must be a JSON array").arg(info.name);
            qWarning() << info.error;
            return false;
        }
        if (!parsePortArray(ports["inputs"].toArray(), info.inputPorts, portError, info.name,
                            QStringLiteral("inputs")) ||
            !parsePortArray(ports["outputs"].toArray(), info.outputPorts, portError, info.name,
                            QStringLiteral("outputs"))) {
            info.error = portError;
            qWarning() << "Invalid ports:" << portError;
            return false;
        }
        info.portsDeclared = true;
    }

    // 阶段 E: 并行安全标记
    QJsonObject execution = json["execution"].toObject();
    info.threadSafe = execution["threadSafe"].toBool(false);
    info.blocking = execution["blocking"].toBool(false);

    return !info.name.isEmpty();
}

bool PluginManager::validateLoadedPlugin(const QString& name, QPluginLoader* loader, QString* error) const {
    if (!loader) {
        if (error) {
            *error = QString("Plugin loader is null: %1").arg(name);
        }
        return false;
    }

    QObject* plugin = loader->instance();
    if (!plugin) {
        if (error) {
            *error = loader->errorString().isEmpty() ? QString("Plugin instance is null: %1").arg(name)
                                                     : loader->errorString();
        }
        return false;
    }

    // 仅对模块插件执行 IModule ABI 校验；相机插件走 ICameraPlugin 注册路径。
    QMutexLocker locker(&m_mutex);
    const bool isModule = m_modules.contains(name);
    locker.unlock();

    if (isModule) {
        IModule* module = qobject_cast<IModule*>(plugin);
        // ABI v1（或更旧）二进制的 IID 与虚表不匹配：cast 失败或版本号不符，明确拒绝并提示重编。
        if (!module) {
            if (error) {
                *error = QString("Plugin %1 does not implement IModule ABI v%2 (interface cast failed). "
                                 "Please rebuild the plugin against the current headers.")
                             .arg(name)
                             .arg(DEEPLUX_MODULE_INTERFACE_VERSION);
            }
            return false;
        }
        if (module->interfaceVersion() != DEEPLUX_MODULE_INTERFACE_VERSION) {
            if (error) {
                *error =
                    QString("Interface version mismatch: plugin=%1, expected=%2, actual=%3. Please rebuild the plugin.")
                        .arg(name)
                        .arg(DEEPLUX_MODULE_INTERFACE_VERSION)
                        .arg(module->interfaceVersion());
            }
            return false;
        }
    }

    return true;
}

void PluginManager::markPluginLoaded(const QString& name, QPluginLoader* loader) {
    QMutexLocker locker(&m_mutex);
    m_loadedPlugins[name] = loader->instance();
    m_pluginLoaders[name] = loader;

    if (m_modules.contains(name)) {
        PluginInfo info = m_modules.value(name);
        info.loaded = true;
        info.error.clear();
        m_modules[name] = info;
    } else if (m_cameras.contains(name)) {
        PluginInfo info = m_cameras.value(name);
        info.loaded = true;
        info.error.clear();
        m_cameras[name] = info;
    }
}

bool PluginManager::loadPlugin(const QString& name, int timeoutMs) {
    Q_UNUSED(timeoutMs);

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
        emit pluginLoadFailed(name, "Plugin metadata path is empty");
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
        emit pluginLoadFailed(name, "No library file found");
        return false;
    }
    QString libPath = dir.filePath(libs.first());

    QPluginLoader* loader = new QPluginLoader(libPath);
    bool loaded = loader->load();
    if (loaded) {
        QString validationError;
        if (!validateLoadedPlugin(name, loader, &validationError)) {
            qCritical() << validationError;
            loader->unload();
            delete loader;
            emit pluginLoadFailed(name, validationError);
            return false;
        }

        QObject* plugin = loader->instance();
        markPluginLoaded(name, loader);
        qDebug() << "Plugin loaded:" << name;

        emit pluginLoaded(name);

        // 设置模块图标
        if (hasGuiApplication() && !info.icon.isEmpty()) {
            QString iconPath = dir.filePath(info.icon);
            if (QFile::exists(iconPath)) {
                DeepLux::IModule* module = qobject_cast<DeepLux::IModule*>(plugin);
                if (module) {
                    module->setIcon(QIcon(iconPath));
                }
            }
        }

        // 如果是相机插件，注册到 CameraManager
        ICameraPlugin* cameraPlugin = qobject_cast<ICameraPlugin*>(plugin);
        if (cameraPlugin) {
            CameraManager::instance().registerCameraPlugin(cameraPlugin);
            qDebug() << "Camera plugin registered:" << name;
        }

        return true;
    } else {
        QString error = loader->errorString();
        qWarning() << "Plugin load failed:" << name << error;
        delete loader;
        emit pluginLoadFailed(name, error);
        return false;
    }
}

void PluginManager::unloadPlugin(const QString& name) {
    tryUnloadPlugin(name);
}

QString PluginManager::unloadBlockReason(bool cameraPlugin, int liveModuleInstances, bool lifecycleTracked) {
    if (cameraPlugin)
        return QStringLiteral("CAMERA_PLUGIN_LIFECYCLE_UNSUPPORTED");
    if (!lifecycleTracked)
        return QStringLiteral("PLUGIN_LIFECYCLE_UNTRACKED");
    if (liveModuleInstances > 0)
        return QStringLiteral("PLUGIN_IN_USE");
    return {};
}

bool PluginManager::tryUnloadPlugin(const QString& name, QString* error) {
    QMutexLocker locker(&m_mutex);

    if (!m_loadedPlugins.contains(name)) {
        if (error) {
            *error = m_modules.contains(name) || m_cameras.contains(name) ? QStringLiteral("PLUGIN_NOT_LOADED")
                                                                          : QStringLiteral("PLUGIN_NOT_FOUND");
        }
        return false;
    }

    const QString blockReason = unloadBlockReason(m_cameras.contains(name), m_liveModuleInstances.value(name, 0),
                                                  !m_untrackedModulePlugins.contains(name));
    if (!blockReason.isEmpty()) {
        if (error)
            *error = blockReason;
        qWarning() << "Plugin unload rejected:" << name << blockReason;
        return false;
    }

    QPluginLoader* loader = nullptr;
    if (m_pluginLoaders.contains(name)) {
        loader = m_pluginLoaders.value(name);
        if (!loader->unload()) {
            if (error)
                *error = QStringLiteral("PLUGIN_UNLOAD_FAILED");
            qWarning() << "Plugin unload failed:" << name << loader->errorString();
            return false;
        }
        m_pluginLoaders.remove(name);
        m_loadedPlugins.remove(name);
        m_liveModuleInstances.remove(name);
        delete loader;
    } else {
        m_loadedPlugins.remove(name);
    }

    if (m_modules.contains(name)) {
        PluginInfo info = m_modules.value(name);
        info.loaded = false;
        m_modules[name] = info;
    } else if (m_cameras.contains(name)) {
        PluginInfo info = m_cameras.value(name);
        info.loaded = false;
        m_cameras[name] = info;
    }

    if (error)
        error->clear();
    emit pluginUnloaded(name);
    return true;
}

void PluginManager::loadAllPlugins() {
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

void PluginManager::loadAllPluginsAsync() {
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
        loadPlugin(name, 5000);
        m_loadingCurrent++;
        emit pluginLoadProgress(m_loadingCurrent, m_loadingTotal, name);
        QCoreApplication::processEvents();
    }

    emit allPluginsLoaded();
}

// PluginLoadRunnable implementation
PluginManager::PluginLoadRunnable::PluginLoadRunnable(const QString& name, const QString& path)
    : m_name(name), m_path(path) {}

void PluginManager::PluginLoadRunnable::run() {
    QPluginLoader* loader = new QPluginLoader(m_path);
    bool success = loader->load();
    QString error = success ? QString() : loader->errorString();

    // 通过事件通知主线程
    QCoreApplication::postEvent(&PluginManager::instance(), new PluginLoadEvent(m_name, success, error, loader));
}

bool PluginManager::event(QEvent* event) {
    if (event->type() == QEvent::User) {
        auto* loadEvent = static_cast<PluginLoadEvent*>(event);
        m_loadingCurrent++;

        QString validationError;
        if (loadEvent->success && validateLoadedPlugin(loadEvent->name, loadEvent->loader, &validationError)) {
            markPluginLoaded(loadEvent->name, loadEvent->loader);
            emit pluginLoaded(loadEvent->name);

            // 如果是相机插件，注册到 CameraManager
            ICameraPlugin* cameraPlugin = qobject_cast<ICameraPlugin*>(loadEvent->loader->instance());
            if (cameraPlugin) {
                CameraManager::instance().registerCameraPlugin(cameraPlugin);
                qDebug() << "Camera plugin registered:" << loadEvent->name;
            }
        } else {
            QString error = validationError.isEmpty() ? loadEvent->error : validationError;
            emit pluginLoadFailed(loadEvent->name, error);
            if (loadEvent->loader) {
                loadEvent->loader->unload();
            }
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

void PluginManager::onLoadTimer() {
    // 空实现，保持接口兼容
}

void PluginManager::unloadAllPlugins() {
    QStringList names;
    {
        QMutexLocker locker(&m_mutex);
        names = m_loadedPlugins.keys();
    }

    for (const QString& name : names) {
        unloadPlugin(name);
    }
}

QStringList PluginManager::availableModules() const {
    QMutexLocker locker(&m_mutex);
    return m_modules.keys();
}

QStringList PluginManager::availableCameras() const {
    QMutexLocker locker(&m_mutex);
    return m_cameras.keys();
}

QList<PluginInfo> PluginManager::moduleInfos() const {
    QMutexLocker locker(&m_mutex);
    return m_modules.values();
}

QList<PluginInfo> PluginManager::cameraInfos() const {
    QMutexLocker locker(&m_mutex);
    return m_cameras.values();
}

PluginInfo PluginManager::pluginInfo(const QString& name) const {
    QMutexLocker locker(&m_mutex);

    // 1. 精确 name 匹配
    if (m_modules.contains(name)) {
        return m_modules[name];
    }
    if (m_cameras.contains(name)) {
        return m_cameras[name];
    }
    // 2. 精确 id 匹配（如 "com.deeplux.plugin.grabimage"）
    for (const PluginInfo& info : m_modules) {
        if (info.id == name)
            return info;
    }
    for (const PluginInfo& info : m_cameras) {
        if (info.id == name)
            return info;
    }
    // 3. 大小写不敏感 name 回退
    for (auto it = m_modules.constBegin(); it != m_modules.constEnd(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    for (auto it = m_cameras.constBegin(); it != m_cameras.constEnd(); ++it) {
        if (it.key().compare(name, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    // 4. id 前缀匹配（strip "com.deeplux.plugin." → match id）
    // 低: "com.deeplux.plugin." 长度为 20（含末尾点），mid(20) 正确
    // 但用 size() 更安全可读
    QString stripped = name;
    const QString prefix = "com.deeplux.plugin.";
    if (stripped.startsWith(prefix))
        stripped = stripped.mid(prefix.size());
    for (const PluginInfo& info : m_modules) {
        if (info.id.endsWith(stripped, Qt::CaseInsensitive))
            return info;
    }
    return PluginInfo();
}

QJsonObject PluginManager::availabilityItem(const PluginInfo& info, bool loaded) {
    const QDir pluginDir = QFileInfo(info.path).absoluteDir();
    QStringList libraryFilters;
#ifdef Q_OS_WIN
    libraryFilters << QStringLiteral("*.dll");
#else
    libraryFilters << QStringLiteral("*.so");
#endif
    const bool installed = !pluginDir.entryList(libraryFilters, QDir::Files).isEmpty();

    const auto requirementValue = [&info](const QString& key) {
        if (info.requirementMetadataErrors.contains(key))
            return QJsonValue(QStringLiteral("invalid_declaration"));
        return info.requirements.contains(key) ? info.requirements.value(key)
                                               : QJsonValue(QStringLiteral("not_declared"));
    };
    QJsonObject requirements{{QStringLiteral("abi"), requirementValue(QStringLiteral("abi"))},
                             {QStringLiteral("platforms"), requirementValue(QStringLiteral("platforms"))},
                             {QStringLiteral("sdk"), requirementValue(QStringLiteral("sdk"))},
                             {QStringLiteral("model"), requirementValue(QStringLiteral("model"))},
                             {QStringLiteral("dependencies"), requirementValue(QStringLiteral("dependencies"))}};
    QJsonArray reasons;
    if (!installed)
        reasons.append(QStringLiteral("PLUGIN_BINARY_MISSING"));
    if (!info.requirementMetadataErrors.isEmpty())
        reasons.append(QStringLiteral("PLUGIN_REQUIREMENT_METADATA_INVALID"));

    const QString availability =
        loaded ? QStringLiteral("loaded") : (installed ? QStringLiteral("loadable") : QStringLiteral("unavailable"));
    QJsonArray suggestedActions;
    if (!loaded)
        suggestedActions.append(installed ? QStringLiteral("load") : QStringLiteral("install_or_repair"));

    return QJsonObject{{QStringLiteral("id"), info.id.isEmpty() ? info.name : info.id},
                       {QStringLiteral("name"), info.name},
                       {QStringLiteral("category"), info.category},
                       {QStringLiteral("version"), info.version},
                       {QStringLiteral("installed"), installed},
                       {QStringLiteral("loaded"), loaded},
                       {QStringLiteral("availability"), availability},
                       {QStringLiteral("unavailableReason"), reasons},
                       {QStringLiteral("requirements"), requirements},
                       {QStringLiteral("executableActions"), QJsonArray{}},
                       {QStringLiteral("suggestedActions"), suggestedActions}};
}

QString PluginManager::currentPlatformName() {
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#else
    return QStringLiteral("linux");
#endif
}

QJsonObject PluginManager::cameraAvailabilityItem(const PluginInfo& info, bool loaded, const QString& currentPlatform) {
    QJsonObject item = availabilityItem(info, loaded);
    const QJsonValue declaredPlatforms = info.requirements.value(QStringLiteral("platforms"));
    QString compatibility = QStringLiteral("not_declared");
    if (!info.requirementMetadataErrors.contains(QStringLiteral("platforms")) && declaredPlatforms.isArray()) {
        const QJsonArray platforms = declaredPlatforms.toArray();
        if (!platforms.isEmpty()) {
            compatibility = QStringLiteral("unsupported");
            for (const QJsonValue& platform : platforms) {
                if (platform.toString().compare(currentPlatform, Qt::CaseInsensitive) == 0) {
                    compatibility = QStringLiteral("supported");
                    break;
                }
            }
        }
    }
    item.insert(QStringLiteral("platformCompatibility"), compatibility);

    if (!loaded && compatibility == QStringLiteral("unsupported")) {
        item.insert(QStringLiteral("availability"), QStringLiteral("unavailable"));
        QJsonArray reasons = item.value(QStringLiteral("unavailableReason")).toArray();
        reasons.append(QStringLiteral("PLUGIN_PLATFORM_UNSUPPORTED"));
        item.insert(QStringLiteral("unavailableReason"), reasons);
    }
    return item;
}

QString PluginManager::moduleDisplayName(const QString& name) const {
    QMutexLocker locker(&m_mutex);

    if (QObject* plugin = m_loadedPlugins.value(name, nullptr)) {
        if (IModule* module = qobject_cast<IModule*>(plugin)) {
            QString displayName = module->name().trimmed();
            if (!displayName.isEmpty()) {
                return displayName;
            }
        }
    }
    if (m_modules.contains(name) && !m_modules[name].name.trimmed().isEmpty()) {
        return m_modules[name].name.trimmed();
    }
    return name;
}

bool PluginManager::isPluginLoaded(const QString& name) const {
    QMutexLocker locker(&m_mutex);
    return m_loadedPlugins.contains(name);
}

QString PluginManager::unloadBlockReasonForPlugin(const QString& name) const {
    if (QThread::currentThread() != thread())
        return QStringLiteral("PLUGIN_THREAD_AFFINITY");

    QMutexLocker locker(&m_mutex);
    const bool isModule = m_modules.contains(name);
    const bool isCamera = m_cameras.contains(name);
    if (isModule && isCamera)
        return QStringLiteral("PLUGIN_NAME_AMBIGUOUS");
    if (!isModule && !isCamera)
        return QStringLiteral("PLUGIN_NOT_FOUND");
    if (isCamera)
        return QStringLiteral("PLUGIN_LIFECYCLE_UNSUPPORTED");
    if (!m_loadedPlugins.contains(name))
        return QStringLiteral("PLUGIN_NOT_LOADED");

    return unloadBlockReason(false, m_liveModuleInstances.value(name, 0), !m_untrackedModulePlugins.contains(name));
}

IModule* PluginManager::createModule(const QString& name) {
    QMutexLocker locker(&m_mutex);
    if (!m_loadedPlugins.contains(name) || !m_modules.contains(name))
        return nullptr;

    QObject* plugin = m_loadedPlugins.value(name);
    if (!plugin)
        return nullptr;
    IModule* mod = qobject_cast<IModule*>(plugin);
    if (!mod)
        return nullptr;

    // 图标优先级: PNG 文件 > 统一运行时生成 > 空
    if (hasGuiApplication() && mod->icon().isNull() && m_modules.contains(name)) {
        PluginInfo info = m_modules[name];
        // 先尝试加载 PNG 文件
        if (!info.icon.isEmpty()) {
            QDir dir(QFileInfo(info.path).absoluteDir());
            QPixmap pm(dir.filePath(info.icon));
            if (!pm.isNull()) {
                mod->setIcon(QIcon(pm));
            }
        }
        // PNG 不存在或加载失败 → 运行时生成统一风格图标
        if (mod->icon().isNull()) {
            mod->setIcon(ModuleIconProvider::instance().iconFor(info.name, info.category));
        }
    }

    IModule* clone = mod->clone();
    if (clone) {
        ModuleBase* moduleBase = qobject_cast<ModuleBase*>(clone);
        const quint64 instanceId = m_nextModuleInstanceId++;
        m_moduleInstances.insert(clone, ModuleInstanceRecord{name, instanceId, false});
        ++m_liveModuleInstances[name];
        connect(
            clone, &QObject::destroyed, this,
            [this, clone, instanceId]() {
                QMutexLocker instanceLocker(&m_mutex);
                const auto it = m_moduleInstances.find(clone);
                if (it == m_moduleInstances.end() || it->instanceId != instanceId)
                    return;
                if (!it->destroying)
                    m_untrackedModulePlugins.insert(it->pluginName);
                m_moduleInstances.erase(it);
            },
            Qt::DirectConnection);

        clone->setIcon(mod->icon());
        // ABI v2：注入 metadata 声明的端口
        if (ModuleBase* mb = moduleBase) {
            const PluginInfo info = m_modules.value(name);
            mb->setProperty("deeplux.pluginId", name);
            mb->setPorts(info.inputPorts, info.outputPorts);
            mb->setThreadSafe(info.threadSafe);
            mb->setBlocking(info.blocking);
        }
        return clone;
    }

    qWarning() << "Plugin does not provide a cloneable module instance:" << name;
    return nullptr;
}

PluginManager::ModulePtr PluginManager::createManagedModule(const QString& name) {
    return ModulePtr(createModule(name), [](IModule* module) {
        if (module)
            PluginManager::instance().destroyModule(module);
    });
}

bool PluginManager::destroyModule(IModule* module) {
    if (!module)
        return false;

    ModuleInstanceRecord record;
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_moduleInstances.find(module);
        if (it == m_moduleInstances.end() || it->destroying)
            return false;
        it->destroying = true;
        record = it.value();
    }

    delete module;

    {
        QMutexLocker locker(&m_mutex);
        auto it = m_liveModuleInstances.find(record.pluginName);
        if (it != m_liveModuleInstances.end()) {
            if (it.value() <= 1)
                m_liveModuleInstances.erase(it);
            else
                --it.value();
        }
    }
    return true;
}

IModule* PluginManager::createFreshModule(const QString& name) {
    QObject* plugin = m_loadedPlugins.value(name);
    if (!plugin) {
        return nullptr;
    }
    return qobject_cast<IModule*>(plugin);
}

ICamera* PluginManager::createCamera(const QString& name) {
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
