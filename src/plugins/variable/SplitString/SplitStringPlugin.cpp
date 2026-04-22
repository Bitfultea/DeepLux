#include "SplitStringPlugin.h"
#include "core/common/Logger.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QSpinBox>
#include <QFormLayout>

namespace DeepLux {

SplitStringPlugin::SplitStringPlugin(QObject* parent)
    : ModuleBase(parent)
{
    m_defaultParams = QJsonObject{
        {"inputString", ""},
        {"separator", ","},
        {"useRegex", false},
        {"outputPrefix", "part"},
        {"maxSplits", 0}
    };
    m_params = m_defaultParams;
}

bool SplitStringPlugin::initialize()
{
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SplitStringPlugin initialized";
    return true;
}

bool SplitStringPlugin::process(const ImageData& input, ImageData& output)
{
    output = input;

    QString inputString = m_params["inputString"].toString();
    if (inputString.isEmpty()) {
        inputString = input.data("input_string").toString();
    }
    if (inputString.isEmpty()) {
        inputString = input.data("barcode").toString();
    }

    QString separator = m_params["separator"].toString(",");
    bool useRegex = m_params["useRegex"].toBool(false);
    QString outputPrefix = m_params["outputPrefix"].toString("part");
    int maxSplits = m_params["maxSplits"].toInt(0);

    if (inputString.isEmpty()) {
        emit errorOccurred(tr("输入字符串为空"));
        return false;
    }

    QStringList parts;
    if (useRegex) {
        parts = inputString.split(QRegExp(separator), QString::SkipEmptyParts);
    } else {
        if (maxSplits > 0) {
            parts = inputString.split(separator, QString::SkipEmptyParts, Qt::CaseSensitive);
            while (parts.size() > maxSplits) {
                parts.removeLast();
            }
        } else {
            parts = inputString.split(separator, QString::SkipEmptyParts, Qt::CaseSensitive);
        }
    }

    output.setData("split_count", parts.size());
    output.setData("split_result", parts.join("|"));

    for (int i = 0; i < parts.size(); ++i) {
        output.setData(QString("%1_%2").arg(outputPrefix).arg(i), parts[i]);
    }

    output.setData(QString("%1_total").arg(outputPrefix), parts.size());

    Logger::instance().info(QString("SplitString: Split '%1' into %2 parts")
        .arg(inputString.left(20)).arg(parts.size()), "Variable");

    return true;
}

bool SplitStringPlugin::doValidateParams(const QJsonObject& params, QString& error) const
{
    Q_UNUSED(params);
    error.clear();
    return true;
}

QWidget* SplitStringPlugin::createConfigWidget()
{
    QWidget* widget = new QWidget();
    QVBoxLayout* layout = new QVBoxLayout(widget);

    QFormLayout* formLayout = new QFormLayout();

    QLineEdit* inputEdit = new QLineEdit(m_params["inputString"].toString());
    QLineEdit* sepEdit = new QLineEdit(m_params["separator"].toString(","));
    QCheckBox* regexCheck = new QCheckBox(tr("使用正则表达式"));
    regexCheck->setChecked(m_params["useRegex"].toBool(false));
    QLineEdit* prefixEdit = new QLineEdit(m_params["outputPrefix"].toString("part"));
    QSpinBox* maxSplitsSpin = new QSpinBox();
    maxSplitsSpin->setRange(0, 100);
    maxSplitsSpin->setValue(m_params["maxSplits"].toInt(0));
    maxSplitsSpin->setPrefix(tr("最大分割数: "));

    formLayout->addRow(tr("输入字符串:"), inputEdit);
    formLayout->addRow(tr("分隔符:"), sepEdit);
    formLayout->addRow(tr(""), regexCheck);
    formLayout->addRow(tr("输出前缀:"), prefixEdit);
    formLayout->addRow(maxSplitsSpin);

    layout->addLayout(formLayout);
    layout->addStretch();

    connect(inputEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["inputString"] = text;
    });

    connect(sepEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["separator"] = text;
    });

    connect(regexCheck, &QCheckBox::toggled, this, [=](bool checked) {
        m_params["useRegex"] = checked;
    });

    connect(prefixEdit, &QLineEdit::textChanged, this, [=](const QString& text) {
        m_params["outputPrefix"] = text;
    });

    connect(maxSplitsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [=](int value) {
        m_params["maxSplits"] = value;
    });

    return widget;
}

} // namespace DeepLux
