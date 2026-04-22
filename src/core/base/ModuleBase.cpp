#include "ModuleBase.h"
#include <QDebug>
#include <QJsonValue>

namespace DeepLux {

// ========== ModuleParam ==========

QJsonObject ModuleParam::toJson() const
{
    QJsonObject json;
    json["name"] = name;
    json["enabled"] = enabled;
    json["posX"] = posX;
    json["posY"] = posY;
    return json;
}

bool ModuleParam::fromJson(const QJsonObject& json)
{
    name = json["name"].toString();
    enabled = json["enabled"].toBool(true);
    posX = json["posX"].toInt(0);
    posY = json["posY"].toInt(0);
    return true;
}

// ========== ModuleBase ==========

ModuleBase::ModuleBase(QObject* parent)
    : IModule(parent)
{
}

ModuleBase::~ModuleBase()
{
}

bool ModuleBase::initialize()
{
    if (m_initialized) {
        return true;
    }
    
    m_state = ModuleState::Idle;
    m_initialized = true;
    emit stateChanged(m_state);
    
    qDebug() << "Module initialized:" << m_name;
    return true;
}

void ModuleBase::shutdown()
{
    if (!m_initialized) {
        return;
    }
    
    m_initialized = false;
    m_state = ModuleState::Idle;
    emit stateChanged(m_state);
    
    qDebug() << "Module shutdown:" << m_name;
}

bool ModuleBase::execute(const ImageData& input, ImageData& output)
{
    if (!m_initialized) {
        emit errorOccurred(tr("Module not initialized"));
        emit executionCompleted(false);
        return false;
    }
    
    if (m_state == ModuleState::Running) {
        emit errorOccurred(tr("Module already running"));
        emit executionCompleted(false);
        return false;
    }
    
    m_state = ModuleState::Running;
    emit stateChanged(m_state);
    
    bool success = false;
    try {
        success = process(input, output);
    } catch (const std::exception& e) {
        emit errorOccurred(QString("Exception: %1").arg(e.what()));
        success = false;
    } catch (...) {
        emit errorOccurred("Unknown exception occurred");
        success = false;
    }
    
    m_state = success ? ModuleState::Idle : ModuleState::Error;
    emit stateChanged(m_state);
    emit executionCompleted(success);
    
    return success;
}

QJsonObject ModuleBase::defaultParams() const
{
    return m_defaultParams;
}

QJsonObject ModuleBase::currentParams() const
{
    QMutexLocker locker(&m_paramsMutex);
    // Create a deep copy of the QJsonObject to avoid COW shared data issues
    QJsonObject copy;
    for (auto it = m_params.begin(); it != m_params.end(); ++it) {
        copy[it.key()] = QJsonValue::fromVariant(it.value().toVariant());
    }
    return copy;
}

void ModuleBase::setParams(const QJsonObject& params)
{
    QString error;
    if (validateParams(params, error)) {
        QMutexLocker locker(&m_paramsMutex);
        m_params = params;
    } else {
        qWarning() << "Invalid params:" << error;
    }
}

void ModuleBase::setParam(const QString& key, const QVariant& value)
{
    QMutexLocker locker(&m_paramsMutex);
    m_params[key] = QJsonValue::fromVariant(value);
}

bool ModuleBase::validateParams(const QJsonObject& params, QString& error) const
{
    return doValidateParams(params, error);
}

bool ModuleBase::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params)
    Q_UNUSED(error)
    return true;
}

QJsonObject ModuleBase::toJson() const
{
    QJsonObject json;
    json["moduleId"] = m_moduleId;
    json["name"] = m_name;
    json["category"] = m_category;
    json["version"] = m_version;
    json["params"] = m_params;
    return json;
}

bool ModuleBase::fromJson(const QJsonObject& json)
{
    m_moduleId = json["moduleId"].toString();
    m_name = json["name"].toString();
    m_category = json["category"].toString();
    m_version = json["version"].toString("1.0.0");
    m_params = json["params"].toObject();
    return true;
}

IModule* ModuleBase::clone() const
{
    // Delegate to the derived class's cloneImpl()
    // If cloneImpl() returns nullptr, we cannot create multiple instances
    return cloneImpl();
}

IModule* ModuleBase::cloneImpl() const
{
    // Default implementation does not support cloning
    // Plugins that need multiple instances must override this
    qWarning() << "Module does not support cloning:" << m_moduleId;
    return nullptr;
}

} // namespace DeepLux
