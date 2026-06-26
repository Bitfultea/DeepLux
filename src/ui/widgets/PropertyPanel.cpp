#include "PropertyPanel.h"
#include "core/interface/IModule.h"
#include <QDebug>
#include <QFormLayout>
#include <QJsonArray>

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
    
    // 标题
    m_titleLabel = new QLabel(tr("属性"));
    m_titleLabel->setStyleSheet(
        "QLabel {"
        "  background-color: #3c3c3c;"
        "  color: white;"
        "  padding: 8px;"
        "  font-weight: bold;"
        "}"
    );
    mainLayout->addWidget(m_titleLabel);
    
    // 滚动区域
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
    
    // 无选择提示
    m_noSelectionLabel = new QLabel(tr("请选择一个模块"));
    m_noSelectionLabel->setAlignment(Qt::AlignCenter);
    m_noSelectionLabel->setStyleSheet("color: #808080;");
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

void PropertyPanel::clear()
{
    m_currentModule = nullptr;
    m_currentModuleId.clear();

    while (QLayoutItem* item = m_contentLayout->takeAt(0)) {
        QWidget* widget = item->widget();
        if (widget && widget != m_noSelectionLabel) {
            delete widget;
        }
        delete item;
    }
    m_paramWidgets.clear();

    if (m_titleLabel) {
        m_titleLabel->setText(tr("属性"));
    }
    if (m_noSelectionLabel) {
        m_noSelectionLabel->setVisible(true);
        m_contentLayout->addWidget(m_noSelectionLabel);
    }
    m_contentLayout->addStretch();
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
        
        for (auto it = params.begin(); it != params.end(); ++it) {
            QString key = it.key();
            if (isChoiceInfoKey(key)) {
                continue;
            }

            QJsonValue value = it.value();
            
            QWidget* widget = nullptr;
            
            const QJsonValue choiceInfo = params.value(key + "_options");
            if (value.isString() && (choiceInfo.isArray() || choiceInfo.isObject())) {
                widget = createChoiceWidget(key, choiceInfoFromValue(choiceInfo));
            } else if (value.isString()) {
                widget = createTextWidget(key, QJsonObject());
            } else if (value.isDouble()) {
                widget = createNumberWidget(key, QJsonObject());
            } else if (value.isBool()) {
                widget = createBoolWidget(key, QJsonObject());
            }
            
            if (widget) {
                QLabel* label = new QLabel(key);
                label->setMinimumWidth(80);
                paramsLayout->addRow(label, widget);
                m_paramWidgets[key] = widget;
            }
        }
        
        m_contentLayout->insertWidget(1, paramsGroup);
    }
}

QWidget* PropertyPanel::createTextWidget(const QString& key, const QJsonObject& info)
{
    Q_UNUSED(info)
    
    QLineEdit* edit = new QLineEdit();
    
    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        edit->setText(params[key].toString());
    }
    
    connect(edit, &QLineEdit::textChanged, this, [this, key](const QString& text) {
        if (m_currentModule) {
            m_currentModule->setParam(key, text);
        }
        emit paramsChanged(m_currentModuleId, key, text);
    });
    
    return edit;
}

QWidget* PropertyPanel::createNumberWidget(const QString& key, const QJsonObject& info)
{
    Q_UNUSED(info)
    
    QDoubleSpinBox* spin = new QDoubleSpinBox();
    spin->setRange(-999999, 999999);
    spin->setDecimals(2);
    
    if (m_currentModule) {
        QJsonObject params = m_currentModule->currentParams();
        spin->setValue(params[key].toDouble());
    }
    
    connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), 
            this, [this, key](double value) {
        if (m_currentModule) {
            m_currentModule->setParam(key, value);
        }
        emit paramsChanged(m_currentModuleId, key, value);
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
    
    connect(check, &QCheckBox::toggled, this, [this, key](bool checked) {
        if (m_currentModule) {
            m_currentModule->setParam(key, checked);
        }
        emit paramsChanged(m_currentModuleId, key, checked);
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

    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, key, combo](int index) {
        if (index < 0) {
            return;
        }
        const QString value = combo->itemData(index).toString();
        if (m_currentModule) {
            m_currentModule->setParam(key, value);
        }
        emit paramsChanged(m_currentModuleId, key, value);
    });

    return combo;
}

void PropertyPanel::applyTheme(bool isDark) {
    QString bg = isDark ? "#252525" : "#ffffff";
    QString fg = isDark ? "#ffffff" : "#212121";
    setStyleSheet(QString("background-color: %1; color: %2;").arg(bg).arg(fg));
    if (m_contentWidget) {
        m_contentWidget->setStyleSheet(QString("background-color: %1;").arg(bg));
    }
    if (m_scrollArea) {
        m_scrollArea->setStyleSheet(QString("background-color: %1; border: none;").arg(bg));
    }
    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 14px; padding: 8px;").arg(fg));
    }
    if (m_noSelectionLabel) {
        m_noSelectionLabel->setStyleSheet(QString("color: %1; padding: 20px;").arg(isDark ? "#888" : "#666"));
    }
}

} // namespace DeepLux
