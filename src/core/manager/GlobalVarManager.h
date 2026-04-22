#pragma once

#include "../common/VarModel.h"
#include "../platform/Platform.h"
#include <QObject>
#include <QMutex>
#include <QWaitCondition>

namespace DeepLux {

class GlobalVarManager : public QObject
{
    Q_OBJECT

public:
    static GlobalVarManager& instance();

    Q_INVOKABLE void addVariable(VarModel* var);
    Q_INVOKABLE void removeVariable(const QString& name);
    Q_INVOKABLE VarModel* getVariable(const QString& name) const;
    Q_INVOKABLE QList<VarModel*> getAllVariables() const;
    Q_INVOKABLE void clearAllVariables();
    Q_INVOKABLE bool hasVariable(const QString& name) const;

    Q_INVOKABLE QStringList getVariableNames(VarDataType type = VarDataType::None) const;
    Q_INVOKABLE QStringList getVariableNamesByType(const QString& typeName) const;

    bool setVariableValue(const QString& name, const QVariant& value);
    QVariant getVariableValue(const QString& name) const;

    bool compileVariable(const QString& name, const QString& projectId, const QString& moduleName);
    bool evaluateVariable(const QString& name);

    // Queue operations
    Q_INVOKABLE void enqueue(const QString& queueName, const QVariant& value);
    Q_INVOKABLE QVariant dequeue(const QString& queueName);
    Q_INVOKABLE QVariant peekQueue(const QString& queueName) const;
    Q_INVOKABLE int queueSize(const QString& queueName) const;
    Q_INVOKABLE void clearQueue(const QString& queueName);
    Q_INVOKABLE QStringList getQueueNames() const;

    bool isEmpty() const { return m_variables.isEmpty(); }
    int count() const { return m_variables.size(); }

    void lock() { m_mutex.lock(); }
    void unlock() { m_mutex.unlock(); }

signals:
    void variableAdded(const QString& name);
    void variableRemoved(const QString& name);
    void variableValueChanged(const QString& name, const QVariant& value);
    void allVariablesCleared();

private:
    explicit GlobalVarManager(QObject* parent = nullptr);
    ~GlobalVarManager();
    DISABLE_COPY(GlobalVarManager)

    static GlobalVarManager* s_instance;
    QMap<QString, VarModel*> m_variables;
    QMap<QString, QList<QVariant>> m_queues;
    mutable QMutex m_mutex;
};

} // namespace DeepLux