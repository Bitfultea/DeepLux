#include "DataCheckPlugin.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QDoubleSpinBox>

namespace DeepLux {

DataCheckPlugin::DataCheckPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"checkType", "Range"},
        {"minValue", 0.0},
        {"maxValue", 100.0},
        {"minLength", 0},
        {"maxLength", 100}
    };
    m_params = m_defaultParams;
}

DataCheckPlugin::~DataCheckPlugin()
{
}

bool DataCheckPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "DataCheckPlugin initialized";
    return true;
}

void DataCheckPlugin::shutdown()
{
    ModuleBase::shutdown();
}

bool DataCheckPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    // 获取要校验的数据
    QVariant dataVar = input.data("check_data");
    if (!dataVar.isValid()) {
        emit errorOccurred(tr("未提供校验数据"));
        return false;
    }

    // 获取校验类型
    QString checkTypeStr = m_params["checkType"].toString();

    m_checkResult = false;
    m_errorMessage.clear();

    if (checkTypeStr == "Range") {
        double minVal = m_params["minValue"].toDouble();
        double maxVal = m_params["maxValue"].toDouble();

        if (dataVar.canConvert<QVariantList>()) {
            // 检查列表中的所有值
            QVariantList list = dataVar.toList();
            for (const QVariant& v : list) {
                if (!checkRange(v, minVal, maxVal)) {
                    m_errorMessage = QString("值 %1 超出范围 [%2, %3]").arg(v.toDouble()).arg(minVal).arg(maxVal);
                    break;
                }
            }
            m_checkResult = m_errorMessage.isEmpty();
        } else {
            m_checkResult = checkRange(dataVar, minVal, maxVal);
            if (!m_checkResult) {
                m_errorMessage = QString("值 %1 超出范围 [%2, %3]").arg(dataVar.toDouble()).arg(minVal).arg(maxVal);
            }
        }
    }
    else if (checkTypeStr == "Length") {
        QString str = dataVar.toString();
        int minLen = m_params["minLength"].toInt();
        int maxLen = m_params["maxLength"].toInt();

        m_checkResult = checkLength(str, minLen, maxLen);
        if (!m_checkResult) {
            m_errorMessage = QString("字符串长度 %1 超出范围 [%2, %3]").arg(str.length()).arg(minLen).arg(maxLen);
        }
    }
    else if (checkTypeStr == "Null") {
        m_checkResult = !dataVar.isNull() && dataVar.isValid();
        if (!m_checkResult) {
            m_errorMessage = "数据为空";
        }
    }

    // 设置输出数据
    output.setData("check_passed", m_checkResult);
    output.setData("check_error", m_errorMessage);

    if (!m_checkResult) {
        emit errorOccurred(m_errorMessage);
    }

    Logger::instance().debug(QString("数据校验: %1, 结果: %2").arg(checkTypeStr).arg(m_checkResult ? "通过" : "失败"), "DataCheck");

    return true;
}

bool DataCheckPlugin::checkRange(const QVariant& value, double minVal, double maxVal)
{
    bool ok;
    double val = value.toDouble(&ok);
    if (!ok) return false;
    return val >= minVal && val <= maxVal;
}

bool DataCheckPlugin::checkLength(const QString& value, int minLen, int maxLen)
{
    int len = value.length();
    return len >= minLen && len <= maxLen;
}

bool DataCheckPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    double minVal = params["minValue"].toDouble();
    double maxVal = params["maxValue"].toDouble();

    if (maxVal < minVal) {
        error = tr("最大值必须大于最小值");
        return false;
    }
    return true;
}

QWidget* DataCheckPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    layout->addWidget(new QLabel(tr("校验类型:")));
    QComboBox* typeCombo = new QComboBox();
    typeCombo->addItem("范围校验", "Range");
    typeCombo->addItem("长度校验", "Length");
    typeCombo->addItem("非空校验", "Null");

    int idx = typeCombo->findData(m_params["checkType"].toString());
    if (idx >= 0) typeCombo->setCurrentIndex(idx);
    layout->addWidget(typeCombo);

    layout->addWidget(new QLabel(tr("最小值:")));
    QDoubleSpinBox* minSpin = new QDoubleSpinBox();
    minSpin->setRange(-1000000, 1000000);
    minSpin->setValue(m_params["minValue"].toDouble());
    layout->addWidget(minSpin);

    layout->addWidget(new QLabel(tr("最大值:")));
    QDoubleSpinBox* maxSpin = new QDoubleSpinBox();
    maxSpin->setRange(-1000000, 1000000);
    maxSpin->setValue(m_params["maxValue"].toDouble());
    layout->addWidget(maxSpin);

    layout->addStretch();

    connect(typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, typeCombo](int) {
        m_params["checkType"] = typeCombo->currentData().toString();
    });

    connect(minSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_params["minValue"] = value;
    });

    connect(maxSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double value) {
        m_params["maxValue"] = value;
    });

    return widget;
}

IModule* DataCheckPlugin::cloneImpl() const
{
    DataCheckPlugin* clone = new DataCheckPlugin();
    return clone;
}

} // namespace DeepLux
