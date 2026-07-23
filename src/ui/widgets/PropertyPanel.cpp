#include "PropertyPanel.h"
#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"
#include <QDebug>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>

namespace DeepLux {

namespace {

bool isChoiceInfoKey(const QString& key)
{
    return key.endsWith("_options");
}

QJsonObject choiceInfoFromValue(const QJsonValue& value)
{
    if (value.isObject()) {
        return value.toObject();
    }

    QJsonObject info;
    if (value.isArray()) {
        info["options"] = value.toArray();
    }
    return info;
}

QString defaultLabelForKey(const QString& key)
{
    // 简单的默认标签：用 key 本身
    return key;
}

} // namespace

PropertyPanel::PropertyPanel(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

PropertyPanel::~PropertyPanel()
{
}

void PropertyPanel::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_titleLabel = new QLabel(tr("属性"));
    m_titleLabel->setObjectName("PropertyPanelTitle");
    mainLayout->addWidget(m_titleLabel);

    m_scrollArea = new QScrollArea();
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);

    m_contentWidget = new QWidget();
    m_contentLayout = new QVBoxLayout(m_contentWidget);
    m_contentLayout->setContentsMargins(8, 8, 8, 8);
    m_contentLayout->setSpacing(8);
    m_contentLayout->addStretch();

    m_scrollArea->setWidget(m_contentWidget);
    mainLayout->addWidget(m_scrollArea);

    m_noSelectionLabel = new QLabel(tr("请选择一个模块"));
    m_noSelectionLabel->setAlignment(Qt::AlignCenter);
    m_contentLayout->insertWidget(0, m_noSelectionLabel);
}

void PropertyPanel::setModule(IModule* module, const QString& instanceId)
{
    clear();

    if (!module) {
        m_noSelectionLabel->setVisible(true);
        m_titleLabel->setText(tr("属性"));
        return;
    }

    m_currentModule = module;
    m_currentModuleId = instanceId.isEmpty() ? module->moduleId() : instanceId;

    m_titleLabel->setText(tr("属性 - %1").arg(module->name()));
    m_noSelectionLabel->setVisible(false);

    loadParams();
}

void PropertyPanel::setPluginInfo(const PluginInfo& info)
{
    m_uiParameters = info.ui.value("parameters").toObject();
    // 如果模块已加载，重新渲染参数
    if (m_currentModule) {
        setModule(m_currentModule, m_currentModuleId);
    }
}

void PropertyPanel::clear()
{
    m_currentModule = nullptr;
    m_currentModuleId.clear();
    m_uiParameters = QJsonObject();

    while (QLayoutItem* item = m_contentLayout->takeAt(0)) {
        QWidget* widget = item->widget();
        if (widget && widget != m_noSelectionLabel) {
            delete widget;
        }
        delete item;
    }
    m_paramWidgets.clear();
    m_paramLabels.clear();

    if (m_titleLabel) {
        m_titleLabel->setText(tr("属性"));
    }
    if (m_noSelectionLabel) {
        m_noSelectionLabel->setVisible(true);
        m_contentLayout->addWidget(m_noSelectionLabel);
    }
    m_contentLayout->addStretch();
}

QStringList PropertyPanel::sortedParamKeys(const QJsonObject& params) const
{
    // 收集非 _options 键
    QStringList keys;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!isChoiceInfoKey(it.key())) {
            keys.append(it.key());
        }
    }

    // 根据 ui.parameters 中的 order 字段排序
    // 有 order 的优先；无 order 的按原始顺序追加
    QList<QPair<int, QString>> ordered;
    QStringList unordered;
    for (const QString& key : keys) {
        QJsonObject meta = m_uiParameters.value(key).toObject();
        if (meta.contains("order")) {
            ordered.append({meta.value("order").toInt(), key});
        } else {
            unordered.append(key);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) {
                  return a.first < b.first;
              });

    QStringList result;
    for (const auto& p : ordered) {
        result.append(p.second);
    }
    result.append(unordered);
    return result;
}

void PropertyPanel::loadParams()
{
    if (!m_currentModule) return;

    QJsonObject params = m_currentModule->currentParams();

    // 模块信息组
    QGroupBox* infoGroup = new QGroupBox(tr("模块信息"));
    QFormLayout* infoLayout = new QFormLayout(infoGroup);

    infoLayout->addRow(tr("ID:"), new QLabel(m_currentModule->moduleId()));
    infoLayout->addRow(tr("类型:"), new QLabel(m_currentModule->category()));
    infoLayout->addRow(tr("版本:"), new QLabel(m_currentModule->version()));

    m_contentLayout->insertWidget(0, infoGroup);

    // 参数组
    if (!params.isEmpty()) {
        QGroupBox* paramsGroup = new QGroupBox(tr("参数"));
        QFormLayout* paramsLayout = new QFormLayout(paramsGroup);

        const QStringList sortedKeys = sortedParamKeys(params);

        for (const QString& key : sortedKeys) {
            QJsonValue value = params.value(key);

            // 合并 ui.parameters 中的元数据和 _options 信息
            QJsonObject meta = m_uiParameters.value(key).toObject();
            const QJsonValue choiceInfo = params.value(key + "_options");
            if (choiceInfo.isArray() || choiceInfo.isObject()) {
                QJsonObject merged = choiceInfoFromValue(choiceInfo);
                // 合并 options 到 meta
                if (merged.contains("options") && !meta.contains("options")) {
                    meta["options"] = merged["options"];
                }
            }

            QWidget* widget = nullptr;

            if (value.isString() && (meta.contains("options") || choiceInfo.isArray() || choiceInfo.isObject())) {
                widget = createChoiceWidget(key, meta);
            } else if (value.isString()) {
                widget = createTextWidget(key, meta);
            } else if (value.isDouble()) {
                widget = createNumberWidget(key, meta);
            } else if (value.isBool()) {
                widget = createBoolWidget(key, meta);
            }

            if (widget) {
                QString labelText = meta.value("label").toString(defaultLabelForKey(key));
                QString unit = meta.value("unit").toString();
                if (!unit.isEmpty()) {
                    labelText = QString("%1 (%2)").arg(labelText, unit);
                }
                QLabel* label = new QLabel(labelText);
                label->setMinimumWidth(80);
                paramsLayout->addRow(label, widget);
                m_paramWidgets[key] = widget;
            }
        }

        m_contentLayout->insertWidget(1, paramsGroup);
    }
}

bool PropertyPanel::commitParam(const QString& key, const QVariant& value)
{
    if (!m_currentModule) return false;

    // 构造候选参数 JSON
    QJsonObject candidate = m_currentModule->currentParams();
    // QVariant -> QJsonValue
    if (value.type() == QVariant::Bool) {
        candidate[key] = value.toBool();
    } else if (value.canConvert<double>()) {
        candidate[key] = value.toDouble();
    } else if (value.canConvert<QString>()) {
        candidate[key] = value.toString();
    } else {
        // 复杂类型尝试通过 QJsonDocument 转换
        candidate[key] = QJsonValue::fromVariant(value);
    }

    // 验证
    QString error;
    if (!m_currentModule->validateParams(candidate, error)) {
        // 验证失败：保持旧值，标记错误
        QWidget* w = m_paramWidgets.value(key, nullptr);
        if (w) {
            markWidgetError(w, error);
        }
        // 恢复控件旧值
        QJsonObject oldParams = m_currentModule->currentParams();
        QJsonValue oldVal = oldParams.value(key);
        if (auto* edit = qobject_cast<QLineEdit*>(w)) {
            edit->setText(oldVal.toString());
        } else if (auto* spin = qobject_cast<QDoubleSpinBox*>(w)) {
            spin->setValue(oldVal.toDouble());
        } else if (auto* check = qobject_cast<QCheckBox*>(w)) {
            check->setChecked(oldVal.toBool());
        } else if (auto* combo = qobject_cast<QComboBox*>(w)) {
            int idx = combo->findData(oldVal.toString());
            if (idx >= 0) combo->setCurrentIndex(idx);
        }
        return false;
    }

    // 验证通过：清除错误标记
    QWidget* w = m_paramWidgets.value(key, nullptr);
    if (w) {
        clearWidgetError(w);
    }

    // 同步运行时模块
    m_currentModule->setParam(key, value);
    // 发送信号（MainWindow 中会同步 Project::setModuleParam）
    emit paramsChanged(m_currentModuleId, key, value);
    return true;
}

void PropertyPanel::markWidgetError(QWidget* widget, const QString& hint)
{
    widget->setStyleSheet("border: 1px solid #EF4444;");
    widget->setToolTip(hint);
}

void PropertyPanel::clearWidgetError(QWidget* widget)
{
    widget->setStyleSheet(QString());
    widget->setToolTip(QString());
}

QWidget* PropertyPanel::createTextWidget(const QString& key, const QJsonObject& info)
{
    Q_UNUSED(info)

    QLineEdit* edit = new QLineEdit();

    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        edit->setText(params[key].toString());
    }

    // 文本参数在 editingFinished 时提交
    connect(edit, &QLineEdit::editingFinished, this, [this, key, edit]() {
        commitParam(key, edit->text());
    });

    return edit;
}

QWidget* PropertyPanel::createNumberWidget(const QString& key, const QJsonObject& info)
{
    QDoubleSpinBox* spin = new QDoubleSpinBox();
    spin->setRange(-999999, 999999);
    spin->setDecimals(2);

    // 使用 ui.parameters 中的 min/max/step
    if (info.contains("min")) {
        spin->setMinimum(info.value("min").toDouble());
    }
    if (info.contains("max")) {
        spin->setMaximum(info.value("max").toDouble());
    }
    if (info.contains("step")) {
        double step = info.value("step").toDouble();
        if (step > 0) {
            spin->setSingleStep(step);
        }
    }

    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        spin->setValue(params[key].toDouble());
    }

    // 数值参数在 editingFinished 时提交
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this, key, spin]() {
        commitParam(key, spin->value());
    });

    return spin;
}

QWidget* PropertyPanel::createBoolWidget(const QString& key, const QJsonObject& info)
{
    Q_UNUSED(info)

    QCheckBox* check = new QCheckBox();

    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        check->setChecked(params[key].toBool());
    }

    // 复选框在 toggled 时提交
    connect(check, &QCheckBox::toggled, this, [this, key, check](bool checked) {
        commitParam(key, checked);
    });

    return check;
}

QWidget* PropertyPanel::createChoiceWidget(const QString& key, const QJsonObject& info)
{
    QComboBox* combo = new QComboBox();
    const QJsonArray options = info["options"].toArray();

    for (const QJsonValue& optionValue : options) {
        if (optionValue.isObject()) {
            QJsonObject option = optionValue.toObject();
            const QString value = option["value"].toString();
            const QString label = option["label"].toString(value);
            if (!value.isEmpty()) {
                combo->addItem(label, value);
            }
        } else {
            const QString value = optionValue.toString();
            if (!value.isEmpty()) {
                combo->addItem(value, value);
            }
        }
    }

    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        const QString currentValue = params[key].toString();
        int index = combo->findData(currentValue);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        }
    }

    // 组合框在 currentIndexChanged（明确选择）时提交
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, key, combo](int index) {
        if (index < 0) {
            return;
        }
        const QString value = combo->itemData(index).toString();
        commitParam(key, value);
    });

    return combo;
}

void PropertyPanel::applyTheme(bool isDark) {
    Q_UNUSED(isDark)
    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QString("font-weight: bold; font-size: 14px; padding: 8px;"));
    }
}

} // namespace DeepLux
