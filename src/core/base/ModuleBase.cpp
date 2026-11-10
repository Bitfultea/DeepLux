#include "ModuleBase.h"

#include "../common/CancellationToken.h"

#include <QDebug>
#include <QHash>
#include <QJsonValue>
#include <QMutexLocker>

namespace DeepLux {
namespace {
QHash<const ModuleBase*, CancellationToken*> g_cancellationTokens;
QMutex g_cancellationTokensMutex;
} // namespace

// ========== ModuleParam ==========

QJsonObject ModuleParam::toJson() const {
    QJsonObject json;
    json["name"] = name;
    json["enabled"] = enabled;
    json["posX"] = posX;
    json["posY"] = posY;
    return json;
}

bool ModuleParam::fromJson(const QJsonObject& json) {
    name = json["name"].toString();
    enabled = json["enabled"].toBool(true);
    posX = json["posX"].toInt(0);
    posY = json["posY"].toInt(0);
    return true;
}

// ========== ModuleBase ==========

ModuleBase::ModuleBase(QObject* parent) : IModule(parent) {}

ModuleBase::~ModuleBase() {
    QMutexLocker locker(&g_cancellationTokensMutex);
    g_cancellationTokens.remove(this);
}

bool ModuleBase::initialize() {
    if (m_initialized) {
        return true;
    }

    m_state = ModuleState::Idle;
    m_initialized = true;
    emit stateChanged(m_state);

    qDebug() << "Module initialized:" << m_name;
    return true;
}

void ModuleBase::shutdown() {
    if (!m_initialized) {
        return;
    }

    m_initialized = false;
    m_state = ModuleState::Idle;
    emit stateChanged(m_state);

    qDebug() << "Module shutdown:" << m_name;
}

bool ModuleBase::execute(const ImageData& input, ImageData& output) {
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

    if (isCancellationRequested()) {
        emit errorOccurred(tr("Execution cancelled"));
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

    if (isCancellationRequested()) {
        emit errorOccurred(tr("Execution cancelled"));
        success = false;
    }

    m_state = success ? ModuleState::Idle : ModuleState::Error;
    emit stateChanged(m_state);
    emit executionCompleted(success);

    return success;
}

void ModuleBase::setPorts(const QList<PortSpec>& inputs, const QList<PortSpec>& outputs) {
    m_inputPorts = inputs;
    m_outputPorts = outputs;
}

ExecutionResult ModuleBase::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    // 提取 Image2D 载体：优先 "image" 端口，其次任一可转换值
    ImageData input;
    if (inputs.contains(QStringLiteral("image")) && inputs.value(QStringLiteral("image")).canConvert<ImageData>()) {
        input = inputs.value(QStringLiteral("image")).value<ImageData>();
    } else {
        for (auto it = inputs.constBegin(); it != inputs.constEnd(); ++it) {
            if (it.value().canConvert<ImageData>()) {
                input = it.value().value<ImageData>();
                break;
            }
        }
    }

    // 将载体中的命名数据键补充为端口值（把原隐藏键显式化为端口）
    PortValueMap effectiveInputs = inputs;
    const QMap<QString, QVariant> carrierData = input.allData();
    for (const PortSpec& spec : m_inputPorts) {
        if (!effectiveInputs.contains(spec.id) && carrierData.contains(spec.id)) {
            effectiveInputs.insert(spec.id, carrierData.value(spec.id));
        }
    }

    // 必需输入校验：缺失返回结构化错误（运行前/运行期均可识别）
    for (const PortSpec& spec : m_inputPorts) {
        if (spec.required && !effectiveInputs.contains(spec.id)) {
            const QString msg = tr("缺少必需输入：%1").arg(spec.displayName.isEmpty() ? spec.id : spec.displayName);
            emit errorOccurred(msg);
            return ExecutionResult::fail(ExecError::MissingRequiredInput, msg, QStringLiteral("port=%1").arg(spec.id));
        }
        if (effectiveInputs.contains(spec.id)) {
            const QVariant& val = effectiveInputs.value(spec.id);
            if (spec.multiple && !spec.control) {
                // multiple 数据端口：值为 QVariantList，逐元素校验类型
                if (val.type() == QVariant::List) {
                    const QVariantList list = val.toList();
                    for (int i = 0; i < list.size(); ++i) {
                        if (!portValueMatchesType(list[i], spec.type)) {
                            const QString msg = tr("多输入元素类型不匹配：%1[%2]")
                                                    .arg(spec.displayName.isEmpty() ? spec.id : spec.displayName)
                                                    .arg(i);
                            emit errorOccurred(msg);
                            return ExecutionResult::fail(
                                ExecError::TypeMismatch, msg,
                                QStringLiteral("port=%1 index=%2 expected=%3 actual=%4")
                                    .arg(spec.id)
                                    .arg(i)
                                    .arg(dataTypeName(spec.type), QString::fromLatin1(list[i].typeName())));
                        }
                    }
                } else if (!portValueMatchesType(val, spec.type)) {
                    const QString msg =
                        tr("输入类型不匹配：%1").arg(spec.displayName.isEmpty() ? spec.id : spec.displayName);
                    emit errorOccurred(msg);
                    return ExecutionResult::fail(
                        ExecError::TypeMismatch, msg,
                        QStringLiteral("port=%1 expected=%2 actual=%3")
                            .arg(spec.id, dataTypeName(spec.type), QString::fromLatin1(val.typeName())));
                }
            } else if (!portValueMatchesType(val, spec.type)) {
                const QString msg =
                    tr("输入类型不匹配：%1").arg(spec.displayName.isEmpty() ? spec.id : spec.displayName);
                emit errorOccurred(msg);
                return ExecutionResult::fail(
                    ExecError::TypeMismatch, msg,
                    QStringLiteral("port=%1 expected=%2 actual=%3")
                        .arg(spec.id, dataTypeName(spec.type), QString::fromLatin1(val.typeName())));
            }
        }
    }

    // 让旧 process(ImageData, ImageData) 插件也能读取端口化调用的命名输入。
    for (auto it = effectiveInputs.constBegin(); it != effectiveInputs.constEnd(); ++it) {
        if (it.key() != QLatin1String("image"))
            input.setData(it.key(), it.value());
    }

    if (context.cancellationToken) {
        setCancellationToken(context.cancellationToken);
    }

    // T2.8（2026-11-10 评审裁定）：token 与「本次执行」同生命周期，退出时必须就地摘除。
    //
    // 此前只设不清：全局表在第一次执行后永久留存调用方传入的裸指针，而
    // CancellationToken 自身没有析构清理（ModuleBase 析构只删自己那条记录）。一旦 token
    // 先于模块销毁——例如 RunEngine 析构后模块仍存活——表里就是悬垂指针，而
    // DelayPlugin/FitPlanePlugin/FitEllipsePlugin 等仍会在 isCancellationRequested()
    // 中解引用它。本次整改把「执行期借用」明确限定在本次 execute() 内：
    // 本函数有 7 个提前 return，故用作用域守卫而不是逐出口手工清表，避免漏路径。
    //
    // 只在本次确实安装了 token 时还原：nullptr 表示「本模块此前没有 token」，此时
    // 不应把别人装的 token 顺手抹掉。
    const bool tokenInstalledHere = (context.cancellationToken != nullptr);
    const struct CancellationTokenScopeGuard {
        ModuleBase* self;
        bool armed;
        ~CancellationTokenScopeGuard() {
            if (armed) {
                self->setCancellationToken(nullptr);
            }
        }
    } cancellationScope{this, tokenInstalledHere};
    Q_UNUSED(cancellationScope);

    ImageData output;
    const bool success = execute(input, output);
    if (!success) {
        return ExecutionResult::fail(ExecError::Processing, tr("模块执行失败：%1").arg(name()));
    }

    // 旧插件继续写 ImageData metadata；桥接层只导出 metadata.json 中声明的命名端口。
    outputs.insert(QStringLiteral("image"), QVariant::fromValue(output));
    for (const PortSpec& spec : m_outputPorts) {
        if (spec.id == QLatin1String("image") || !output.hasData(spec.id))
            continue;
        const QVariant value = output.data(spec.id);
        if (!portValueMatchesType(value, spec.type)) {
            const QString msg = tr("输出类型不匹配：%1").arg(spec.displayName.isEmpty() ? spec.id : spec.displayName);
            emit errorOccurred(msg);
            return ExecutionResult::fail(
                ExecError::TypeMismatch, msg,
                QStringLiteral("port=%1 expected=%2 actual=%3")
                    .arg(spec.id, dataTypeName(spec.type), QString::fromLatin1(value.typeName())));
        }
        outputs.insert(spec.id, value);
    }
    return ExecutionResult::ok();
}

void ModuleBase::setCancellationToken(CancellationToken* token) {
    QMutexLocker locker(&g_cancellationTokensMutex);
    if (token) {
        g_cancellationTokens.insert(this, token);
    } else {
        g_cancellationTokens.remove(this);
    }
}

CancellationToken* ModuleBase::cancellationToken() const {
    QMutexLocker locker(&g_cancellationTokensMutex);
    return g_cancellationTokens.value(this, nullptr);
}

void ModuleBase::detachCancellationToken(CancellationToken* token) {
    if (!token) {
        return;
    }
    // T2.8：token 即将销毁，把表中所有指向它的条目摘掉。遍历全部条目而不是只按
    // 当前模块查找——同一 token 可能被多个模块借用过（并行批次、多次执行）。
    QMutexLocker locker(&g_cancellationTokensMutex);
    for (auto it = g_cancellationTokens.begin(); it != g_cancellationTokens.end();) {
        if (it.value() == token) {
            it = g_cancellationTokens.erase(it);
        } else {
            ++it;
        }
    }
}

bool ModuleBase::isCancellationRequested() const {
    CancellationToken* token = cancellationToken();
    return token && token->isCancelledFast();
}

QJsonObject ModuleBase::defaultParams() const {
    return m_defaultParams;
}

QJsonObject ModuleBase::currentParams() const {
    QMutexLocker locker(&m_paramsMutex);
    // Create a deep copy of the QJsonObject to avoid COW shared data issues
    QJsonObject copy;
    for (auto it = m_params.begin(); it != m_params.end(); ++it) {
        copy[it.key()] = QJsonValue::fromVariant(it.value().toVariant());
    }
    return copy;
}

void ModuleBase::setParams(const QJsonObject& params) {
    QJsonObject merged = m_defaultParams;
    for (auto it = params.begin(); it != params.end(); ++it) {
        merged[it.key()] = it.value();
    }

    QString error;
    if (validateParams(merged, error)) {
        QMutexLocker locker(&m_paramsMutex);
        m_params = merged;
    } else {
        qWarning() << "Invalid params:" << error;
    }
}

void ModuleBase::setParam(const QString& key, const QVariant& value) {
    QMutexLocker locker(&m_paramsMutex);
    m_params[key] = QJsonValue::fromVariant(value);
}

bool ModuleBase::validateParams(const QJsonObject& params, QString& error) const {
    return doValidateParams(params, error);
}

bool ModuleBase::doValidateParams(const QJsonObject& params, QString& error) const {
    Q_UNUSED(params)
    Q_UNUSED(error)
    return true;
}

QJsonObject ModuleBase::toJson() const {
    QJsonObject json;
    json["moduleId"] = m_moduleId;
    json["name"] = m_name;
    json["category"] = m_category;
    json["version"] = m_version;
    json["params"] = m_params;
    return json;
}

bool ModuleBase::fromJson(const QJsonObject& json) {
    m_moduleId = json["moduleId"].toString();
    m_name = json["name"].toString();
    m_category = json["category"].toString();
    m_version = json["version"].toString("1.0.0");
    setParams(json["params"].toObject());
    return true;
}

IModule* ModuleBase::clone() const {
    IModule* clonedModule = cloneImpl();
    if (!clonedModule) {
        return nullptr;
    }

    if (clonedModule == this) {
        qWarning() << "Module cloneImpl returned the shared source instance:" << m_moduleId;
        return nullptr;
    }

    ModuleBase* clonedBase = qobject_cast<ModuleBase*>(clonedModule);
    if (!clonedBase) {
        return clonedModule;
    }

    clonedBase->m_moduleId = m_moduleId;
    clonedBase->m_name = m_name;
    clonedBase->m_instanceName = m_instanceName;
    clonedBase->m_category = m_category;
    clonedBase->m_version = m_version;
    clonedBase->m_author = m_author;
    clonedBase->m_description = m_description;
    clonedBase->m_icon = m_icon;

    {
        QMutexLocker sourceLocker(&m_paramsMutex);
        QMutexLocker cloneLocker(&clonedBase->m_paramsMutex);
        clonedBase->m_params = m_params;
        clonedBase->m_defaultParams = m_defaultParams;
    }

    clonedBase->m_initialized = false;
    clonedBase->m_state = ModuleState::Idle;
    return clonedModule;
}

IModule* ModuleBase::cloneImpl() const {
    // Default implementation does not support cloning
    // Plugins that need multiple instances must override this
    qWarning() << "Module does not support cloning:" << m_moduleId;
    return nullptr;
}

} // namespace DeepLux
