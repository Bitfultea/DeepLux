#include "GlobalVarManager.h"

#include "common/Logger.h"

#include <QJsonArray>
#include <QMetaType>
#include <cmath>

namespace DeepLux {

namespace {

QJsonValue variantToJson(const QVariant& value) {
    if (value.userType() == QMetaType::QVariantList) {
        QJsonArray array;
        const QVariantList values = value.toList();
        for (const QVariant& item : values)
            array.append(variantToJson(item));
        return array;
    }
    if (!value.isValid() || value.isNull())
        return QJsonValue();
    return QJsonValue::fromVariant(value);
}

QJsonObject snapshotToJson(const GlobalVariableSnapshot& snapshot) {
    QJsonObject json{{QStringLiteral("exists"), snapshot.exists}, {QStringLiteral("name"), snapshot.name}};
    if (!snapshot.exists)
        return json;
    json.insert(QStringLiteral("identity"), QString::number(snapshot.identity));
    json.insert(QStringLiteral("type"), VarModel::dataTypeToString(snapshot.type));
    json.insert(QStringLiteral("expression"), snapshot.expression);
    json.insert(QStringLiteral("isLinked"), snapshot.isLinked);
    if (snapshot.type == VarDataType::Int || snapshot.type == VarDataType::Double ||
        snapshot.type == VarDataType::String || snapshot.type == VarDataType::Bool ||
        snapshot.type == VarDataType::IntArray || snapshot.type == VarDataType::DoubleArray ||
        snapshot.type == VarDataType::StringArray || snapshot.type == VarDataType::BoolArray)
        json.insert(QStringLiteral("value"), variantToJson(snapshot.value));
    return json;
}

bool snapshotMatches(const GlobalVariableSnapshot& current, const QJsonObject& expected) {
    if (expected.isEmpty())
        return true;
    if (current.exists != expected.value(QStringLiteral("exists")).toBool() ||
        current.name != expected.value(QStringLiteral("name")).toString())
        return false;
    if (!current.exists)
        return true;
    const QJsonObject currentJson = snapshotToJson(current);
    return currentJson.value(QStringLiteral("identity")) == expected.value(QStringLiteral("identity")) &&
           currentJson.value(QStringLiteral("type")) == expected.value(QStringLiteral("type")) &&
           currentJson.value(QStringLiteral("value")) == expected.value(QStringLiteral("value")) &&
           currentJson.value(QStringLiteral("expression")) == expected.value(QStringLiteral("expression")) &&
           currentJson.value(QStringLiteral("isLinked")) == expected.value(QStringLiteral("isLinked"));
}

bool isUserVariableTypeSupported(VarDataType type) {
    return type == VarDataType::Int || type == VarDataType::Double || type == VarDataType::String ||
           type == VarDataType::Bool || type == VarDataType::IntArray || type == VarDataType::DoubleArray ||
           type == VarDataType::StringArray || type == VarDataType::BoolArray;
}

} // namespace

GlobalVarManager* GlobalVarManager::s_instance = nullptr;

GlobalVarManager& GlobalVarManager::instance() {
    if (!s_instance) {
        s_instance = new GlobalVarManager();
    }
    return *s_instance;
}

GlobalVarManager::GlobalVarManager(QObject* parent) : QObject(parent) {
    Logger::instance().info("Global variable manager initialized", "Var");
}

GlobalVarManager::~GlobalVarManager() {
    clearAllVariables();
}

void GlobalVarManager::addVariable(VarModel* var) {
    if (!var)
        return;

    QString name = var->name();
    const QString typeName = VarModel::dataTypeToString(var->dataType());
    {
        QMutexLocker locker(&m_mutex);
        if (m_variables.contains(name)) {
            Logger::instance().warning(QString("Variable already exists: %1").arg(name), "Var");
            delete var;
            return;
        }

        m_variables[name] = var;
        m_variableIdentities.insert(name, m_nextVariableIdentity++);
    }

    emit variableAdded(name);
    Logger::instance().debug(QString("Variable added: %1 (%2)").arg(name, typeName), "Var");
}

void GlobalVarManager::removeVariable(const QString& name) {
    bool removed = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_variables.contains(name)) {
            delete m_variables.take(name);
            m_variableIdentities.remove(name);
            removed = true;
        }
    }
    if (removed) {
        emit variableRemoved(name);
        Logger::instance().debug(QString("Variable removed: %1").arg(name), "Var");
    }
}

VarModel* GlobalVarManager::getVariable(const QString& name) const {
    QMutexLocker locker(&m_mutex);
    return m_variables.value(name, nullptr);
}

QList<VarModel*> GlobalVarManager::getAllVariables() const {
    QMutexLocker locker(&m_mutex);
    return m_variables.values();
}

void GlobalVarManager::clearAllVariables() {
    {
        QMutexLocker locker(&m_mutex);
        for (auto* var : m_variables)
            delete var;
        m_variables.clear();
        m_variableIdentities.clear();
    }
    emit allVariablesCleared();
    Logger::instance().debug("All variables cleared", "Var");
}

bool GlobalVarManager::hasVariable(const QString& name) const {
    QMutexLocker locker(&m_mutex);
    return m_variables.contains(name);
}

bool GlobalVarManager::isEmpty() const {
    QMutexLocker locker(&m_mutex);
    return m_variables.isEmpty();
}

int GlobalVarManager::count() const {
    QMutexLocker locker(&m_mutex);
    return m_variables.size();
}

QStringList GlobalVarManager::getVariableNames(VarDataType type) const {
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

QStringList GlobalVarManager::getVariableNamesByType(const QString& typeName) const {
    VarDataType type = VarModel::stringToDataType(typeName);
    return getVariableNames(type);
}

QList<GlobalVariableSnapshot> GlobalVarManager::userVariableSnapshots(const QString& typeName) const {
    QMutexLocker locker(&m_mutex);
    QList<GlobalVariableSnapshot> snapshots;
    const VarDataType filter = typeName.isEmpty() ? VarDataType::None : VarModel::stringToDataType(typeName);
    if (!typeName.isEmpty() && filter == VarDataType::None)
        return snapshots;
    for (auto it = m_variables.cbegin(); it != m_variables.cend(); ++it) {
        if (filter != VarDataType::None && it.value()->dataType() != filter)
            continue;
        snapshots.append(snapshotLocked(it.key()));
    }
    return snapshots;
}

GlobalVariableSnapshot GlobalVarManager::snapshotLocked(const QString& name) const {
    GlobalVariableSnapshot snapshot;
    snapshot.name = name;
    VarModel* variable = m_variables.value(name, nullptr);
    if (!variable)
        return snapshot;
    snapshot.exists = true;
    snapshot.type = variable->dataType();
    snapshot.value = variable->value();
    snapshot.expression = variable->expression();
    snapshot.note = variable->note();
    snapshot.isLinked = variable->isLinked();
    snapshot.index = variable->index();
    snapshot.identity = m_variableIdentities.value(name);
    return snapshot;
}

GlobalVariableSnapshot GlobalVarManager::userVariableSnapshot(const QString& name) const {
    QMutexLocker locker(&m_mutex);
    return snapshotLocked(name);
}

QJsonObject GlobalVarManager::userVariableSnapshotJson(const QString& name) const {
    QMutexLocker locker(&m_mutex);
    return snapshotToJson(snapshotLocked(name));
}

bool GlobalVarManager::userVariableSnapshotMatches(const QString& name, const QJsonObject& expected) const {
    QMutexLocker locker(&m_mutex);
    return snapshotMatches(snapshotLocked(name), expected);
}

bool GlobalVarManager::validateUserValue(VarDataType type, const QVariant& value) const {
    auto isArrayOf = [&value](int metaType) {
        if (value.userType() != QMetaType::QVariantList)
            return false;
        for (const QVariant& item : value.toList())
            if (item.userType() != metaType)
                return false;
        return true;
    };
    switch (type) {
    case VarDataType::Int:
        return value.userType() == QMetaType::Int;
    case VarDataType::Double:
        return value.userType() == QMetaType::Double && std::isfinite(value.toDouble());
    case VarDataType::String:
        return value.userType() == QMetaType::QString;
    case VarDataType::Bool:
        return value.userType() == QMetaType::Bool;
    case VarDataType::IntArray:
        return isArrayOf(QMetaType::Int);
    case VarDataType::DoubleArray:
        if (value.userType() != QMetaType::QVariantList)
            return false;
        for (const QVariant& item : value.toList())
            if (item.userType() != QMetaType::Double || !std::isfinite(item.toDouble()))
                return false;
        return true;
    case VarDataType::StringArray:
        return isArrayOf(QMetaType::QString);
    case VarDataType::BoolArray:
        return isArrayOf(QMetaType::Bool);
    default:
        return false;
    }
}

bool GlobalVarManager::beginUserMutationLease() {
    QMutexLocker locker(&m_activityMutex);
    while (m_userMutationLeaseHeld)
        m_activityChanged.wait(&m_activityMutex);
    if (m_executionLeaseHeld || m_executionLeaseWaiting)
        return false;
    m_userMutationLeaseHeld = true;
    return true;
}

void GlobalVarManager::endUserMutationLease() {
    QMutexLocker locker(&m_activityMutex);
    m_userMutationLeaseHeld = false;
    m_activityChanged.wakeAll();
}

bool GlobalVarManager::beginExecutionLease() {
    QMutexLocker locker(&m_activityMutex);
    m_executionLeaseWaiting = true;
    while (m_userMutationLeaseHeld)
        m_activityChanged.wait(&m_activityMutex);
    m_executionLeaseWaiting = false;
    if (m_executionLeaseHeld) {
        m_activityChanged.wakeAll();
        return false;
    }
    m_executionLeaseHeld = true;
    return true;
}

void GlobalVarManager::endExecutionLease() {
    QMutexLocker locker(&m_activityMutex);
    m_executionLeaseHeld = false;
    m_activityChanged.wakeAll();
}

bool GlobalVarManager::tryCreateUserVariable(const QString& rawName, VarDataType type, const QVariant& value,
                                             const QJsonObject& expected, GlobalVariableSnapshot* created,
                                             QString* errorCode) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    const QString name = rawName.trimmed();
    if (name.isEmpty())
        return fail(QStringLiteral("INVALID_ARGUMENT"));
    if (!isUserVariableTypeSupported(type))
        return fail(QStringLiteral("UNSUPPORTED_TYPE"));
    if (!validateUserValue(type, value))
        return fail(QStringLiteral("VARIABLE_TYPE_MISMATCH"));
    if (!beginUserMutationLease())
        return fail(QStringLiteral("ENGINE_BUSY"));
    struct LeaseGuard {
        GlobalVarManager* manager;
        ~LeaseGuard() {
            manager->endUserMutationLease();
        }
    } lease{this};
    QMutexLocker locker(&m_mutex);
    const GlobalVariableSnapshot current = snapshotLocked(name);
    if (!expected.isEmpty() && !snapshotMatches(current, expected))
        return fail(QStringLiteral("CONFIRMATION_EXPIRED"));
    if (current.exists)
        return fail(QStringLiteral("VARIABLE_EXISTS"));
    auto* variable = new VarModel(name, type, value);
    m_variables.insert(name, variable);
    const quint64 identity = m_nextVariableIdentity++;
    m_variableIdentities.insert(name, identity);
    if (created)
        *created = snapshotLocked(name);
    locker.unlock();
    emit variableAdded(name);
    emit variableValueChanged(name, value);
    return true;
}

bool GlobalVarManager::trySetUserVariable(const QString& rawName, const QVariant& value, const QJsonObject& expected,
                                          GlobalVariableSnapshot* updated, QString* errorCode) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    const QString name = rawName.trimmed();
    if (name.isEmpty())
        return fail(QStringLiteral("INVALID_ARGUMENT"));
    if (!beginUserMutationLease())
        return fail(QStringLiteral("ENGINE_BUSY"));
    struct LeaseGuard {
        GlobalVarManager* manager;
        ~LeaseGuard() {
            manager->endUserMutationLease();
        }
    } lease{this};
    QMutexLocker locker(&m_mutex);
    const GlobalVariableSnapshot current = snapshotLocked(name);
    if (!current.exists)
        return fail(QStringLiteral("VARIABLE_NOT_FOUND"));
    if (!isUserVariableTypeSupported(current.type))
        return fail(QStringLiteral("UNSUPPORTED_TYPE"));
    if (!validateUserValue(current.type, value))
        return fail(QStringLiteral("VARIABLE_TYPE_MISMATCH"));
    if (!expected.isEmpty() && !snapshotMatches(current, expected))
        return fail(QStringLiteral("CONFIRMATION_EXPIRED"));
    VarModel* variable = m_variables.value(name);
    variable->setValue(value);
    const GlobalVariableSnapshot result = snapshotLocked(name);
    if (updated)
        *updated = result;
    locker.unlock();
    emit variableValueChanged(name, value);
    return true;
}

bool GlobalVarManager::tryRemoveUserVariable(const QString& rawName, const QJsonObject& expected, QString* errorCode) {
    auto fail = [errorCode](const QString& code) {
        if (errorCode)
            *errorCode = code;
        return false;
    };
    const QString name = rawName.trimmed();
    if (name.isEmpty())
        return fail(QStringLiteral("INVALID_ARGUMENT"));
    if (!beginUserMutationLease())
        return fail(QStringLiteral("ENGINE_BUSY"));
    struct LeaseGuard {
        GlobalVarManager* manager;
        ~LeaseGuard() {
            manager->endUserMutationLease();
        }
    } lease{this};
    QMutexLocker locker(&m_mutex);
    const GlobalVariableSnapshot current = snapshotLocked(name);
    if (!current.exists)
        return fail(QStringLiteral("VARIABLE_NOT_FOUND"));
    if (!expected.isEmpty() && !snapshotMatches(current, expected))
        return fail(QStringLiteral("CONFIRMATION_EXPIRED"));
    delete m_variables.take(name);
    m_variableIdentities.remove(name);
    locker.unlock();
    emit variableRemoved(name);
    return true;
}

bool GlobalVarManager::setVariableValue(const QString& name, const QVariant& value) {
    {
        QMutexLocker locker(&m_mutex);
        VarModel* var = m_variables.value(name, nullptr);
        if (!var) {
            Logger::instance().warning(QString("Variable not found: %1").arg(name), "Var");
            return false;
        }
        var->setValue(value);
    }
    emit variableValueChanged(name, value);
    return true;
}

QVariant GlobalVarManager::getVariableValue(const QString& name) const {
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        return QVariant();
    }
    return var->value();
}

bool GlobalVarManager::compileVariable(const QString& name, const QString& projectId, const QString& moduleName) {
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        return false;
    }
    return var->compileExpression(projectId, moduleName);
}

bool GlobalVarManager::evaluateVariable(const QString& name) {
    QMutexLocker locker(&m_mutex);

    VarModel* var = m_variables.value(name, nullptr);
    if (!var) {
        return false;
    }

    QVariant result = var->evaluateExpression();
    var->setValue(result);
    return true;
}

void GlobalVarManager::enqueue(const QString& queueName, const QVariant& value) {
    QMutexLocker locker(&m_mutex);
    m_queues[queueName].append(value);
}

QVariant GlobalVarManager::dequeue(const QString& queueName) {
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName) && !m_queues[queueName].isEmpty()) {
        return m_queues[queueName].takeFirst();
    }
    return QVariant();
}

QVariant GlobalVarManager::peekQueue(const QString& queueName) const {
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName) && !m_queues[queueName].isEmpty()) {
        return m_queues[queueName].first();
    }
    return QVariant();
}

int GlobalVarManager::queueSize(const QString& queueName) const {
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName)) {
        return m_queues[queueName].size();
    }
    return 0;
}

void GlobalVarManager::clearQueue(const QString& queueName) {
    QMutexLocker locker(&m_mutex);
    if (m_queues.contains(queueName)) {
        m_queues[queueName].clear();
    }
}

QStringList GlobalVarManager::getQueueNames() const {
    QMutexLocker locker(&m_mutex);
    return m_queues.keys();
}

} // namespace DeepLux
