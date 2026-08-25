#include "SplitStringPlugin.h"

#include "core/common/Logger.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegExp>
#include <QSpinBox>
#include <QVBoxLayout>

namespace DeepLux {

SplitStringPlugin::SplitStringPlugin(QObject* parent) : ModuleBase(parent) {
    m_defaultParams = QJsonObject{
        {"inputString", ""}, {"separator", ","}, {"useRegex", false}, {"outputPrefix", "part"}, {"maxSplits", 0}};
    m_params = m_defaultParams;
}

bool SplitStringPlugin::initialize() {
    if (!ModuleBase::initialize()) {
        return false;
    }
    qDebug() << "SplitStringPlugin initialized";
    return true;
}

bool SplitStringPlugin::process(const ImageData& input, ImageData& output) {
    output = input;

    QJsonObject params = currentParams();
    QString inputString = params["inputString"].toString();
    if (inputString.isEmpty()) {
        inputString = input.data("input_string").toString();
    }
    if (inputString.isEmpty()) {
        inputString = input.data("barcode").toString();
    }

    QString separator = params["separator"].toString(",");
    bool useRegex = params["useRegex"].toBool(false);
    QString outputPrefix = params["outputPrefix"].toString("part");
    int maxSplits = params["maxSplits"].toInt(0);

    if (inputString.isEmpty()) {
        emit errorOccurred(tr("输入字符串为空"));
        return false;
    }

    QStringList parts;
    if (useRegex) {
        parts = inputString.split(QRegExp(separator), Qt::SkipEmptyParts);
    } else {
        if (maxSplits > 0) {
            parts = inputString.split(separator, Qt::SkipEmptyParts, Qt::CaseSensitive);
            while (parts.size() > maxSplits) {
                parts.removeLast();
            }
        } else {
            parts = inputString.split(separator, Qt::SkipEmptyParts, Qt::CaseSensitive);
        }
    }

    output.setData("split_count", parts.size());
    output.setData("split_result", parts.join("|"));

    for (int i = 0; i < parts.size(); ++i) {
        output.setData(QString("%1_%2").arg(outputPrefix).arg(i), parts[i]);
    }

    output.setData(QString("%1_total").arg(outputPrefix), parts.size());

    Logger::instance().info(
        QString("SplitString: Split '%1' into %2 parts").arg(inputString.left(20)).arg(parts.size()), "Variable");

    return true;
}

bool SplitStringPlugin::doValidateParams(const QJsonObject& params, QString& error) const {
    error.clear();

    const QString separator = params["separator"].toString(",");
    if (separator.isEmpty()) {
        error = tr("分隔符不能为空");
        return false;
    }

    if (params["outputPrefix"].toString("part").trimmed().isEmpty()) {
        error = tr("输出前缀不能为空");
        return false;
    }

    if (params["maxSplits"].toInt(0) < 0) {
        error = tr("最大分割数不能小于0");
        return false;
    }

    if (params["useRegex"].toBool(false)) {
        QRegExp regex(separator);
        if (!regex.isValid()) {
            error = tr("正则表达式无效");
            return false;
        }
    }

    return true;
}

QWidget* SplitStringPlugin::createConfigWidget() {
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

    connect(inputEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("inputString", text); });

    connect(sepEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("separator", text); });

    connect(regexCheck, &QCheckBox::toggled, this, [=](bool checked) { setParam("useRegex", checked); });

    connect(prefixEdit, &QLineEdit::textChanged, this, [=](const QString& text) { setParam("outputPrefix", text); });

    connect(maxSplitsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            [=](int value) { setParam("maxSplits", value); });

    return widget;
}

IModule* SplitStringPlugin::cloneImpl() const {
    SplitStringPlugin* clone = new SplitStringPlugin();
    clone->setParams(currentParams());
    return clone;
}

} // namespace DeepLux
