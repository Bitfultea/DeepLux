#include "PropertyPanel.h"

#include "core/interface/IModule.h"
#include "core/manager/PluginManager.h"

#include <QDebug>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>
#include <QSet>
#include <QToolButton>

namespace DeepLux {

namespace {

bool isChoiceInfoKey(const QString& key) {
    return key.endsWith("_options");
}

// 内部字段过滤：以 _ 开头的字段不显示在参数面板
bool isInternalParamKey(const QString& key) {
    return key.startsWith('_');
}

QJsonObject choiceInfoFromValue(const QJsonValue& value) {
    if (value.isObject()) {
        return value.toObject();
    }

    QJsonObject info;
    if (value.isArray()) {
        info["options"] = value.toArray();
    }
    return info;
}

QString defaultLabelForKey(const QString& key) {
    // 简单的默认标签：用 key 本身
    return key;
}

} // namespace

PropertyPanel::PropertyPanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(220);
    setupUi();
}

PropertyPanel::~PropertyPanel() {}

void PropertyPanel::setupUi() {
    // 参数页随父容器扩展，并禁止水平滚动条以避免视觉抖动
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    m_scrollArea = new QScrollArea();
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_contentWidget = new QWidget();
    m_contentWidget->setObjectName("PropertyPanelContent");
    m_contentWidget->setAutoFillBackground(true);
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

void PropertyPanel::setModule(IModule* module, const QString& instanceId) {
    clear();

    if (!module) {
        m_noSelectionLabel->setVisible(true);
        return;
    }

    m_currentModule = module;
    m_currentModuleId = instanceId.isEmpty() ? module->moduleId() : instanceId;

    m_noSelectionLabel->setVisible(false);

    loadParams();
}

void PropertyPanel::setPluginInfo(const PluginInfo& info) {
    m_uiParameters = info.ui.value("parameters").toObject();
    // 如果模块已加载，重新渲染参数
    if (m_currentModule) {
        setModule(m_currentModule, m_currentModuleId);
    }
}

void PropertyPanel::refreshFromModule() {
    if (!m_currentModule) {
        return;
    }
    // 重新从模块读取参数并刷新控件，用于撤销/重做后同步显示
    setModule(m_currentModule, m_currentModuleId);
}

void PropertyPanel::clear() {
    m_currentModule = nullptr;
    m_currentModuleId.clear();
    // 不清除 m_uiParameters — 由 setPluginInfo 管理，
    // 这样 setModule 调用 clear() 后仍能使用 PluginInfo 中的元数据。

    while (QLayoutItem* item = m_contentLayout->takeAt(0)) {
        QWidget* widget = item->widget();
        if (widget && widget != m_noSelectionLabel) {
            delete widget;
        }
        delete item;
    }
    m_paramWidgets.clear();
    m_paramLabels.clear();

    if (m_noSelectionLabel) {
        m_noSelectionLabel->setVisible(true);
        m_contentLayout->addWidget(m_noSelectionLabel);
    }
    m_contentLayout->addStretch();
}

QStringList PropertyPanel::sortedParamKeys(const QJsonObject& params) const {
    // 收集非 _options 键且非内部字段
    QStringList keys;
    for (auto it = params.begin(); it != params.end(); ++it) {
        const QString& key = it.key();
        if (isChoiceInfoKey(key) || isInternalParamKey(key)) {
            continue;
        }
        // 当存在 ui.parameters 元数据时，只显示元数据中描述的参数；
        // 没有元数据时显示所有非内部参数（用 key 作为标签）。
        if (!m_uiParameters.isEmpty() && !m_uiParameters.contains(key)) {
            continue;
        }
        keys.append(key);
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
              [](const QPair<int, QString>& a, const QPair<int, QString>& b) { return a.first < b.first; });

    QStringList result;
    for (const auto& p : ordered) {
        result.append(p.second);
    }
    result.append(unordered);
    return result;
}

void PropertyPanel::loadParams() {
    if (!m_currentModule)
        return;

    QJsonObject params = m_currentModule->currentParams();

    // 参数组
    if (!params.isEmpty()) {
        QGroupBox* paramsGroup = new QGroupBox(tr("参数"));
        QFormLayout* paramsLayout = new QFormLayout(paramsGroup);
        paramsLayout->setRowWrapPolicy(QFormLayout::DontWrapRows);
        paramsLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        paramsLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        paramsLayout->setHorizontalSpacing(8);
        paramsLayout->setVerticalSpacing(6);

        const QStringList sortedKeys = sortedParamKeys(params);

        for (const QString& key : sortedKeys) {
            QJsonValue value = params.value(key);

            // 合并 ui.parameters 中的元数据和 _options 信息
            QJsonObject meta = m_uiParameters.value(key).toObject();
            // P2: 也检查 meta 内部的 _options 字段（metadata.json 格式）
            const QJsonValue choiceInfo = params.value(key + "_options");
            const QJsonValue innerOptions = meta.value("_options");
            if (innerOptions.isArray()) {
                meta["options"] = innerOptions.toArray();
            } else if (choiceInfo.isArray() || choiceInfo.isObject()) {
                QJsonObject merged = choiceInfoFromValue(choiceInfo);
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
                constexpr int labelWidth = 104;
                label->setText(label->fontMetrics().elidedText(labelText, Qt::ElideRight, labelWidth));
                label->setToolTip(labelText);
                label->setFixedWidth(labelWidth);
                label->setWordWrap(false);
                label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
                paramsLayout->addRow(label, widget);
                m_paramWidgets[key] = widget;
            }
        }

        m_contentLayout->insertWidget(0, paramsGroup);
    }

    auto* infoContainer = new QWidget();
    infoContainer->setObjectName("ModuleInfoContainer");
    auto* infoContainerLayout = new QVBoxLayout(infoContainer);
    infoContainerLayout->setContentsMargins(0, 0, 0, 0);
    infoContainerLayout->setSpacing(0);

    auto* infoToggle = new QToolButton(infoContainer);
    infoToggle->setObjectName("ModuleInfoToggle");
    infoToggle->setText(tr("模块信息"));
    infoToggle->setCheckable(true);
    infoToggle->setArrowType(Qt::RightArrow);
    infoToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    infoContainerLayout->addWidget(infoToggle);

    auto* infoContent = new QWidget(infoContainer);
    infoContent->setObjectName("ModuleInfoContent");
    auto* infoLayout = new QFormLayout(infoContent);
    auto addInfoRow = [infoLayout](const QString& label, const QString& value) {
        auto* valueLabel = new QLabel(value);
        valueLabel->setWordWrap(true);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valueLabel->setToolTip(value);
        infoLayout->addRow(label, valueLabel);
    };
    addInfoRow(tr("ID:"), m_currentModule->moduleId());
    addInfoRow(tr("类型:"), m_currentModule->category());
    addInfoRow(tr("版本:"), m_currentModule->version());
    infoContent->setVisible(false);
    infoContainerLayout->addWidget(infoContent);

    connect(infoToggle, &QToolButton::toggled, infoContent, [infoToggle, infoContent](bool expanded) {
        infoToggle->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        infoContent->setVisible(expanded);
    });
    m_contentLayout->insertWidget(params.isEmpty() ? 0 : 1, infoContainer);
}

bool PropertyPanel::commitParam(const QString& key, const QVariant& value) {
    if (!m_currentModule)
        return false;

    // 构造候选参数 JSON
    QJsonObject candidate = m_currentModule->currentParams();
    // QVariant -> QJsonValue
    // 注意：canConvert<double>() 对字符串也返回 true（结果为 0），
    // 因此必须用 userType() 严格判断类型，避免把字符串误转为数值。
    const int typeId = value.userType();
    if (typeId == QMetaType::Bool) {
        candidate[key] = value.toBool();
    } else if (typeId == QMetaType::Double || typeId == QMetaType::Int || typeId == QMetaType::LongLong ||
               typeId == QMetaType::UInt || typeId == QMetaType::ULongLong || typeId == QMetaType::Float) {
        candidate[key] = value.toDouble();
    } else if (typeId == QMetaType::QString) {
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
            if (idx >= 0)
                combo->setCurrentIndex(idx);
        }
        return false;
    }

    // 验证通过：清除错误标记
    QWidget* w = m_paramWidgets.value(key, nullptr);
    if (w) {
        clearWidgetError(w);
    }

    // 只发信号，不直接修改运行时模块
    // 由 MainWindow 的 pushParamCommand 统一处理修改和撤销
    emit paramsChanged(m_currentModuleId, key, value);
    return true;
}

void PropertyPanel::markWidgetError(QWidget* widget, const QString& hint) {
    widget->setStyleSheet("border: 1px solid #EF4444;");
    widget->setToolTip(hint);
}

void PropertyPanel::clearWidgetError(QWidget* widget) {
    widget->setStyleSheet(QString());
    widget->setToolTip(QString());
}

QWidget* PropertyPanel::createTextWidget(const QString& key, const QJsonObject& info) {
    Q_UNUSED(info)

    QLineEdit* edit = new QLineEdit();

    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        edit->setText(params[key].toString());
    }

    // 文本参数在 editingFinished 时提交
    connect(edit, &QLineEdit::editingFinished, this, [this, key, edit]() { commitParam(key, edit->text()); });

    return edit;
}

QWidget* PropertyPanel::createNumberWidget(const QString& key, const QJsonObject& info) {
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
    connect(spin, &QDoubleSpinBox::editingFinished, this, [this, key, spin]() { commitParam(key, spin->value()); });

    return spin;
}

QWidget* PropertyPanel::createBoolWidget(const QString& key, const QJsonObject& info) {
    Q_UNUSED(info)

    QCheckBox* check = new QCheckBox();

    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        check->setChecked(params[key].toBool());
    }

    // 复选框在 toggled 时提交
    connect(check, &QCheckBox::toggled, this, [this, key, check](bool checked) { commitParam(key, checked); });

    return check;
}

QWidget* PropertyPanel::createChoiceWidget(const QString& key, const QJsonObject& info) {
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
    // P1: 实际应用主题，不再忽略 isDark
    const QString bg = isDark ? "#252525" : "#ffffff";
    const QString fg = isDark ? "#ffffff" : "#212121";
    const QString border = isDark ? "#3a3a3a" : "#dddddd";

    setStyleSheet(QString("PropertyPanel { background-color: %1; }"
                          "QScrollArea, QScrollArea > QWidget > QWidget { background-color: %1; border: none; }"
                          "QWidget#PropertyPanelContent { background-color: %1; }"
                          "QLabel { color: %2; background-color: transparent; }"
                          "QGroupBox { color: %2; border: 1px solid %3; border-radius: 4px; "
                          "  margin-top: 8px; padding-top: 8px; background-color: %1; }"
                          "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 4px; }"
                          "QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { "
                          "  background-color: %1; color: %2; border: 1px solid %3; padding: 4px; border-radius: 2px; }"
                          "QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { "
                          "  border: 1px solid #0078d7; }"
                          "QCheckBox { color: %2; }"
                          "QWidget#ModuleInfoContainer { background-color: %1; border-top: 1px solid %3; }"
                          "QToolButton#ModuleInfoToggle { background-color: %1; color: %2; border: none; "
                          "  padding: 4px 0; text-align: left; }"
                          "QToolButton#ModuleInfoToggle:hover { color: #0078d7; }"
                          "QWidget#ModuleInfoContent { background-color: %1; border-top: 1px solid %3; }")
                      .arg(bg, fg, border));

    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(bg));
    setPalette(pal);
}

} // namespace DeepLux
