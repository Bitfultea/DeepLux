#include "DelayPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSpinBox>
#include <QThread>

namespace DeepLux {

DelayPlugin::DelayPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"delayMs", 100}
    };
    m_params = m_defaultParams;
}

bool DelayPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DelayPlugin initialized";
    return true;
}

bool DelayPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    m_delayMs = m_params["delayMs"].toInt(100);

    if (m_delayMs <= 0) {
        emit errorOccurred(tr("延时时间必须大于0"));
        return false;
    }

    Logger::instance().debug(QString("Delay: Waiting %1 ms").arg(m_delayMs), "Logic");

    QThread::msleep(m_delayMs);

    return true;
}

bool DelayPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    if (params["delayMs"].toInt() <= 0) {
        error = QString("Delay time must be greater than 0");
        return false;
    }
    return true;
}

QWidget* DelayPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QSpinBox* delaySpin = new QSpinBox();
    delaySpin->setRange(1, 60000);
    delaySpin->setValue(m_params["delayMs"].toInt(100));
    delaySpin->setPrefix(tr("延时(ms): "));

    formLayout->addRow(tr("延时时间:"), delaySpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(delaySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["delayMs"] = value;
    });

    return widget;
}

} // namespace DeepLux
