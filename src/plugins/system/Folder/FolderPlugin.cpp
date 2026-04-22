#include "FolderPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>

namespace DeepLux {

FolderPlugin::FolderPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"operation", "Exists"},
        {"folderPath", ""}
    };
    m_params = m_defaultParams;
}

FolderPlugin::~FolderPlugin()
{
}

bool FolderPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "FolderPlugin initialized";
    return true;
}

void FolderPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool FolderPlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input);
    output = input;

    QString folderPath = m_params["folderPath"].toString();
    QString operation = m_params["operation"].toString();

    if (folderPath.isEmpty()) {
        folderPath = input.data("folder_path").toString();
    }

    if (folderPath.isEmpty()) {
        emit errorOccurred(tr("未提供文件夹路径"));
        return false;
    }

    m_operationResult = false;

    if (operation == "Create") {
        QDir dir;
        m_operationResult = dir.mkpath(folderPath);
        Logger::instance().debug(QString("创建文件夹: %1, 结果: %2").arg(folderPath).arg(m_operationResult ? "成功" : "失败"), "Folder");
    }
    else if (operation == "Delete") {
        QDir dir(folderPath);
        m_operationResult = dir.exists() && dir.removeRecursively();
        Logger::instance().debug(QString("删除文件夹: %1, 结果: %2").arg(folderPath).arg(m_operationResult ? "成功" : "失败"), "Folder");
    }
    else if (operation == "Exists") {
        QDir dir(folderPath);
        m_operationResult = dir.exists();
        Logger::instance().debug(QString("检查文件夹存在: %1, 结果: %2").arg(folderPath).arg(m_operationResult ? "存在" : "不存在"), "Folder");
    }
    else if (operation == "List") {
        QDir dir(folderPath);
        if (dir.exists()) {
            QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
            output.setData("folder_entries", entries);
            output.setData("folder_count", entries.size());
            m_operationResult = true;
            Logger::instance().debug(QString("列出文件夹内容: %1, 文件数: %2").arg(folderPath).arg(entries.size()), "Folder");
        } else {
            m_operationResult = false;
        }
    }

    output.setData("operation_result", m_operationResult);
    output.setData("folder_path", folderPath);

    return true;
}

bool FolderPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    QString operation = params["operation"].toString();
    QString folderPath = params["folderPath"].toString();

    if (operation == "Delete" && folderPath.isEmpty()) {
        error = tr("删除操作需要指定文件夹路径");
        return false;
    }

    return true;
}

QWidget* FolderPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("操作类型:")));
    QComboBox* opCombo = new QComboBox();
    opCombo->addItem("检查是否存在", "Exists");
    opCombo->addItem("创建文件夹", "Create");
    opCombo->addItem("删除文件夹", "Delete");
    opCombo->addItem("列出内容", "List");

    int idx = opCombo->findData(m_params["operation"].toString());
    if (idx >= 0) opCombo->setCurrentIndex(idx);
    layout->addWidget(opCombo);

    layout->addWidget(new QLabel(tr("文件夹路径:")));
    QLineEdit* pathEdit = new QLineEdit(m_params["folderPath"].toString());
    layout->addWidget(pathEdit);

    layout->addStretch();

    connect(opCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, opCombo](int) {
        m_params["operation"] = opCombo->currentData().toString();
    });

    connect(pathEdit, &QLineEdit::textChanged, this, [this](const QString& text) {
        m_params["folderPath"] = text;
    });

    return widget;
}

IModule* FolderPlugin::cloneImpl() const
{
    FolderPlugin* clone = new FolderPlugin();
    return clone;
}

} // namespace DeepLux
