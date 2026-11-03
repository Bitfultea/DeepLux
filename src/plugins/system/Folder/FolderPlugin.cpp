#include "FolderPlugin.h"

#include "common/Logger.h"
#include "platform/PathUtils.h"

#include <QComboBox>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace DeepLux {

FolderPlugin::FolderPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{{"operation", "Exists"}, {"folderPath", ""}};
    m_params = m_defaultParams;
}

FolderPlugin::~FolderPlugin() {}

bool FolderPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FolderPlugin initialized";
    return true;
}

void FolderPlugin::shutdown() {
    ModuleBase::shutdown();
}

ExecutionResult FolderPlugin::execute(const PortValueMap& inputs, PortValueMap& outputs, ExecutionContext& context) {
    m_deleteAttempted = false;
    m_operationResult = false;
    const QJsonObject params = currentParams();
    const QJsonObject authorization = property(AgentFolderDeleteAuthorizationProperty).toJsonObject();
    const QString operation = params.value(QStringLiteral("operation")).toString();
    if (authorization.value(QStringLiteral("active")).toBool()) {
        const bool authorized = authorization.value(QStringLiteral("authorized")).toBool() &&
                                authorization.value(QStringLiteral("authorizedModuleId")).toString() == instanceName();
        const QString approvedOperation = authorization.value(QStringLiteral("approvedOperation")).toString();
        const QString folderPath = params.value(QStringLiteral("folderPath")).toString();
        bool targetChanged = false;
        if ((operation == QLatin1String("Create") || operation == QLatin1String("Delete")) &&
            (!authorized || operation != approvedOperation))
            targetChanged = true;
        if (authorized && operation != approvedOperation)
            targetChanged = true;
        if (authorized && folderPath != authorization.value(QStringLiteral("approvedConfiguredPath")).toString())
            targetChanged = true;

        if (operation == QLatin1String("Delete") && authorized && approvedOperation == QLatin1String("Delete")) {
            const QString approvedTarget = authorization.value(QStringLiteral("resolvedTarget")).toString();
            const QFileInfo currentInfo(folderPath);
            const bool targetExists = currentInfo.isDir();
            const QString currentTarget = QDir::cleanPath(currentInfo.canonicalFilePath());
            const bool approvedTargetExists = authorization.value(QStringLiteral("targetExistsAtApproval")).toBool();
            targetChanged = targetChanged || targetExists != approvedTargetExists;
            if (folderPath.trimmed().isEmpty())
                targetChanged = true;
            if (targetExists && (!approvedTargetExists || currentTarget != approvedTarget))
                targetChanged = true;
        }
        if (operation == QLatin1String("Create") && authorized && approvedOperation == QLatin1String("Create")) {
            const QString approvedTarget = authorization.value(QStringLiteral("resolvedTarget")).toString();
            const QString currentTarget = PathUtils::resolveFileTargetPath(folderPath);
            if (folderPath.trimmed().isEmpty() || approvedTarget.isEmpty() || currentTarget.isEmpty() ||
                currentTarget != approvedTarget)
                targetChanged = true;
        }
        if (targetChanged) {
            return ExecutionResult::fail(
                ExecError::ConfirmedTargetChanged, tr("已批准的文件夹操作或目标已变化，未执行操作"),
                QStringLiteral("codeName=CONFIRMED_TARGET_CHANGED instanceId=%1 authorizationId=%2")
                    .arg(instanceName(), authorization.value(QStringLiteral("executionAuthorizationId")).toString()));
        }
    }

    const ExecutionResult result = ModuleBase::execute(inputs, outputs, context);
    if (m_deleteAttempted && !m_operationResult) {
        return ExecutionResult::fail(ExecError::SideEffectUncertain, tr("文件夹递归删除未能确认完成"),
                                     QStringLiteral("operation=Delete result=unconfirmed"));
    }
    return result;
}

bool FolderPlugin::process(const ImageData& input, ImageData& output) {
    Q_UNUSED(input);
    output = input;

    QJsonObject params = currentParams();
    QString folderPath = params["folderPath"].toString();
    QString operation = params["operation"].toString();

    if (folderPath.isEmpty()) {
        folderPath = input.data("folder_path").toString();
    }

    if (folderPath.isEmpty()) {
        emit errorOccurred(tr("未提供文件夹路径"));
        return false;
    }

    m_operationResult = false;
    m_deleteAttempted = false;

    if (operation == "Create") {
        QDir dir;
        m_operationResult = dir.mkpath(folderPath);
        Logger::instance().debug(
            QString("创建文件夹: %1, 结果: %2").arg(folderPath).arg(m_operationResult ? "成功" : "失败"), "Folder");
    } else if (operation == "Delete") {
        QDir dir(folderPath);
        m_deleteAttempted = dir.exists();
        m_operationResult = m_deleteAttempted && dir.removeRecursively();
        Logger::instance().debug(
            QString("删除文件夹: %1, 结果: %2").arg(folderPath).arg(m_operationResult ? "成功" : "失败"), "Folder");
    } else if (operation == "Exists") {
        QDir dir(folderPath);
        m_operationResult = dir.exists();
        Logger::instance().debug(
            QString("检查文件夹存在: %1, 结果: %2").arg(folderPath).arg(m_operationResult ? "存在" : "不存在"),
            "Folder");
    } else if (operation == "List") {
        QDir dir(folderPath);
        if (dir.exists()) {
            QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
            output.setData("folder_entries", entries);
            output.setData("folder_count", entries.size());
            m_operationResult = true;
            Logger::instance().debug(QString("列出文件夹内容: %1, 文件数: %2").arg(folderPath).arg(entries.size()),
                                     "Folder");
        } else {
            m_operationResult = false;
        }
    }

    output.setData("operation_result", m_operationResult);
    output.setData("folder_path", folderPath);

    return true;
}

bool FolderPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    QString operation = params["operation"].toString();
    QString folderPath = params["folderPath"].toString();

    if (operation == "Delete" && folderPath.isEmpty()) {
        error = tr("删除操作需要指定文件夹路径");
        return false;
    }

    return true;
}

QWidget* FolderPlugin::createConfigWidget() {
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("操作类型:")));
    QComboBox* opCombo = new QComboBox();
    opCombo->addItem("检查是否存在", "Exists");
    opCombo->addItem("创建文件夹", "Create");
    opCombo->addItem("删除文件夹", "Delete");
    opCombo->addItem("列出内容", "List");

    int idx = opCombo->findData(m_params["operation"].toString());
    if (idx >= 0)
        opCombo->setCurrentIndex(idx);
    layout->addWidget(opCombo);

    layout->addWidget(new QLabel(tr("文件夹路径:")));
    QLineEdit* pathEdit = new QLineEdit(m_params["folderPath"].toString());
    layout->addWidget(pathEdit);

    layout->addStretch();

    connect(opCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, opCombo](int) { setParam("operation", opCombo->currentData().toString()); });

    connect(pathEdit, &QLineEdit::textChanged, this, [this](const QString& text) { setParam("folderPath", text); });

    return widget;
}

IModule* FolderPlugin::cloneImpl() const {
    FolderPlugin* clone = new FolderPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
