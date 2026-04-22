#include "GlobalVarManager.h"
#include "common/Logger.h"

namespace DeepLux {

GlobalVarManager* GlobalVarManager::s_instance = nullptr;

GlobalVarManager& GlobalVarManager::instance()
{
    if (!s_instance) {
        s_instance = new GlobalVarManager();
    }
    return *s_instance;
}

GlobalVarManager::GlobalVarManager(QObject* parent)
    : QObject(parent)
{
    Logger::instance().info("Global variable manager initialized", "Var");
}

GlobalVarManager::~GlobalVarManager()
{
    clearAllVariables();
}

void GlobalVarManager::addVariable(VarModel* var)
{
    if (!var) return;

    QMutexLocker locker(&m_mutex);

    QString name = var->name();
    if (m_variables.contains(name)) {
        Logger::instance().warning(QString("Variable already exists: %1").arg(name), "Var");
        delete var;
        return;
    }

    m_variables[name] = var;
    emit variableAdded(name);
    Logger::instance().debug(QString("Variable added: %1 (%2)").arg(name).arg(VarModel::dataTypeToString(var->dataType())), "Var");
}

void GlobalVarManager::removeVariable(const QString& name)
{
    QMutexLocker locker(&m_mutex);

    if (m_variables.contains(name)) {
        delete m_variables[name];
        m_variables.remove(name);
        emit variableRemoved(name);
        Logger::instance().debug(QString("Variable removed: %1").arg(name), "Var");
    }
}

VarModel* GlobalVarManager::getVariable(const QString& name) const
{
    QMutexLocker locker(&m_mutex);
    return m_variables.value(name, nullptr);
}

QList<VarModel*> GlobalVarManager::getAllVariables() const
{
    QMutexLocker locker(&m_mutex);
    return m_variables.values();
}

void GlobalVarManager::clearAllVariables()
{
    QMutexLocker locker(&m_mutex);

    for (auto* var : m_variables) {
        delete var;
    }
    m_variables.clear();
    emit allVariablesCleared();
    Logger::instance().debug("All variables cleared", "Var");
}

bool GlobalVarManager::hasVariable(const QString& name) const
{
    QMutexLocker locker(&m_mutex);
    return m_variables.contains(name);
}

QStringList GlobalVarManager::getVariableNames(VarDataType type) const
{
    QMutexLocker locker(&m_mutex);
    QStringList names;

    if (type == VarDataType::None) {
        return m_variables.keys();
    }

    for (auto it = m_variables.constBegin(); it != m_variables.constEnd(); ++it) {
        if (it.value()->dataType() == type) {
            names.append(it.key());
        }
    }
    return names;
}

QStringList GlobalVarManager::getVariableNamesByType(const QString& typeName) const
{
    VarDataType type = VarModel::stringToDataType(typeName);
    return getVariableNames(type);
}

bool GlobalVarManager::setVariableValue(const QString& name, const QVariant& value)
{
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        Logger::instance().warning(QString("Variable not found: %1").arg(name), "Var");
        return false;
    }

    var->setValue(value);
    emit variableValueChanged(name, value);
    return true;
}

QVariant GlobalVarManager::getVariableValue(const QString& name) const
{
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        return QVariant();
    }
    return var->value();
}

bool GlobalVarManager::compileVariable(const QString& name, const QString& projectId, const QString& moduleName)
{
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        return false;
    }
    return var->compileExpression(projectId, moduleName);
}

bool GlobalVarManager::evaluateVariable(const QString& name)
{
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        return false;
    }

    QVariant result = var->evaluateExpression();
    var->setValue(result);
    return true;
}

void GlobalVarManager::enqueue(const QString& queueName, const QVariant& value)
{
    QMutexLocker locker(&m_mutex);
    m_queues[queueName].append(value);
}

QVariant GlobalVarManager::dequeue(const QString& queueName)
{
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName) && !m_queues[queueName].isEmpty()) {
        return m_queues[queueName].takeFirst();
    }
    return QVariant();
}

QVariant GlobalVarManager::peekQueue(const QString& queueName) const
{
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName) && !m_queues[queueName].isEmpty()) {
        return m_queues[queueName].first();
    }
    return QVariant();
}

int GlobalVarManager::queueSize(const QString& queueName) const
{
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName)) {
        return m_queues[queueName].size();
    }
    return 0;
}

void GlobalVarManager::clearQueue(const QString& queueName)
{
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName)) {
        m_queues[queueName].clear();
    }
}

QStringList GlobalVarManager::getQueueNames() const
{
    QMutexLocker locker(&m_mutex);
    return m_queues.keys();
}

} // namespace DeepLux