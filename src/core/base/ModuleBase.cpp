#include "ModuleBase.h"

#include "../common/CancellationToken.h"

#include <QDebug>
#include <QHash>
#include <QJsonValue>
#include <QMutexLocker>

namespace DeepLux {
namespace {
struct CancellationTokenRegistry {
    QHash<const ModuleBase*, CancellationToken*> tokens;
    QMutex mutex;
};

CancellationTokenRegistry& cancellationTokenRegistry() {
    // Dynamically loaded plugins may destroy modules after core static teardown.
    static auto* registry = new CancellationTokenRegistry;
    return *registry;
}
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
    auto& registry = cancellationTokenRegistry();
    QMutexLocker locker(&registry.mutex);
    registry.tokens.remove(this);
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
    // T2.8（2026-11-10 裁定；2026-11-11 复核 P2 整改）：token 与「本次执行」同生命周期，
    // 必须在**函数入口**就建立守卫，退出时无条件摘除。
    //
    // 复核指出的缺陷：上一版把守卫放在输入校验**之后**，而 token 的安装并没有提前——
    // 于是「缺少必需输入（1001）」「类型不匹配（1002）」这两类早退路径绕过了守卫：
    // 条目已写入借用表却永不摘除，执行结束后仍留存。更严重的是 RunEngine 会在调用
    // execute() **之前**先 setCancellationToken()，那些早退路径连"安装"都发生在守卫之外。
    //
    // 现在的做法：安装与守卫都放在最前面，覆盖函数**全部**出口（含所有校验早退）。
    // 守卫只在本次确实安装了 token 时还原，避免抹掉别人装的 token。
    if (context.cancellationToken) {
        setCancellationToken(context.cancellationToken);
    }
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
    auto& registry = cancellationTokenRegistry();
    QMutexLocker locker(&registry.mutex);
    if (token) {
        registry.tokens.insert(this, token);
    } else {
        registry.tokens.remove(this);
    }
}

CancellationToken* ModuleBase::cancellationToken() const {
    auto& registry = cancellationTokenRegistry();
    QMutexLocker locker(&registry.mutex);
    return registry.tokens.value(this, nullptr);
}

void ModuleBase::detachCancellationToken(CancellationToken* token) {
    if (!token) {
        return;
    }
    // T2.8：token 即将销毁，把表中所有指向它的条目摘掉。遍历全部条目而不是只按
    // 当前模块查找——同一 token 可能被多个模块借用过（并行批次、多次执行）。
    auto& registry = cancellationTokenRegistry();
    QMutexLocker locker(&registry.mutex);
    for (auto it = registry.tokens.begin(); it != registry.tokens.end();) {
        if (it.value() == token) {
            it = registry.tokens.erase(it);
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
