#include "StopWhilePlugin.h"
#include "core/engine/RunEngine.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QCheckBox>

namespace DeepLux {

StopWhilePlugin::StopWhilePlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"stopAlways", true}
    };
    m_params = m_defaultParams;
    m_name = "停止循环";
    m_moduleId = "StopWhilePlugin";
    m_category = "logic";
    m_description = "放在循环内用于提前退出";
}

bool StopWhilePlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "StopWhilePlugin initialized";
    return true;
}

bool StopWhilePlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error = QString();
    return true;
}

bool StopWhilePlugin::process(const ImageData& input, ImageData& output)
{
    Q_UNUSED(input);
    output = input;

    output.setData("stop_while_requested", true);

    Logger::instance().info("StopWhile: Loop stop requested", "Logic");

    return true;
}

QWidget* StopWhilePlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QCheckBox* stopCheck = new QCheckBox(tr("总是停止循环"));
    stopCheck->setChecked(m_params["stopAlways"].toBool(true));

    formLayout->addRow(tr(""), stopCheck);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(stopCheck, &QCheckBox::toggled, this, [=](bool checked) {
        m_params["stopAlways"] = checked;
    });

    return widget;
}

} // namespace DeepLux
