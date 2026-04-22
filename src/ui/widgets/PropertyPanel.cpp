#include "PropertyPanel.h"
#include "core/interface/IModule.h"
#include <QDebug>
#include <QFormLayout>

namespace DeepLux {

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

void PropertyPanel::setModule(IModule* module)
{
    clear();
    
    if (!module) {
        m_noSelectionLabel->setVisible(true);
        m_titleLabel->setText(tr("属性"));
        return;
    }
    
    m_currentModule = module;
    m_currentModuleId = module->moduleId();
    
    m_titleLabel->setText(tr("属性 - %1").arg(module->name()));
    m_noSelectionLabel->setVisible(false);
    
    loadParams();
}

void PropertyPanel::clear()
{
    m_currentModule = nullptr;
    m_currentModuleId.clear();
    
    // 删除所有参数控件
    for (auto* widget : m_paramWidgets) {
        delete widget;
    }
    m_paramWidgets.clear();
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
            QJsonValue value = it.value();
            
            QWidget* widget = nullptr;
            
            if (value.isString()) {
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
        emit paramsChanged(m_currentModuleId, key, checked);
    });
    
    return check;
}

QWidget* PropertyPanel::createChoiceWidget(const QString& key, const QJsonObject& info)
{
    Q_UNUSED(key)
    Q_UNUSED(info)
    
    QComboBox* combo = new QComboBox();
    // TODO: 从 info 中加载选项
    return combo;
}

} // namespace DeepLux
