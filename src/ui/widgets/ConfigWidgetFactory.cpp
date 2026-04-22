#include "ConfigWidgetFactory.h"

#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>

namespace DeepLux {

ConfigWidgetFactory& ConfigWidgetFactory::instance()
{
    static ConfigWidgetFactory factory;
    return factory;
}

ConfigWidgetFactory::ConfigWidgetFactory()
{
}

void ConfigWidgetFactory::setDarkTheme(bool dark)
{
    if (m_darkTheme != dark) {
        m_darkTheme = dark;
    }
}

QLabel* ConfigWidgetFactory::createLabel(const QString& text, QWidget* parent)
{
    QLabel* label = new QLabel(text, parent);
    if (m_darkTheme) {
        label->setStyleSheet("color: #e8f4f8; background-color: transparent;");
    } else {
        label->setStyleSheet("color: #1a1a1a; background-color: transparent;");
    }
    return label;
}

QLineEdit* ConfigWidgetFactory::createLineEdit(const QString& text, QWidget* parent)
{
    QLineEdit* edit = new QLineEdit(text, parent);
    applyStyle(edit);
    return edit;
}

QSpinBox* ConfigWidgetFactory::createSpinBox(int min, int max, int value, QWidget* parent)
{
    QSpinBox* spin = new QSpinBox(parent);
    spin->setRange(min, max);
    spin->setValue(value);
    applyStyle(spin);
    return spin;
}

QDoubleSpinBox* ConfigWidgetFactory::createDoubleSpinBox(double min, double max, double value, int decimals, QWidget* parent)
{
    QDoubleSpinBox* spin = new QDoubleSpinBox(parent);
    spin->setRange(min, max);
    spin->setValue(value);
    spin->setDecimals(decimals);
    applyStyle(spin);
    return spin;
}

QComboBox* ConfigWidgetFactory::createComboBox(QWidget* parent)
{
    QComboBox* combo = new QComboBox(parent);
    applyStyle(combo);
    return combo;
}

QCheckBox* ConfigWidgetFactory::createCheckBox(const QString& text, bool checked, QWidget* parent)
{
    QCheckBox* check = new QCheckBox(text, parent);
    check->setChecked(checked);
    if (m_darkTheme) {
        check->setStyleSheet("color: #e8f4f8; background-color: transparent;");
    } else {
        check->setStyleSheet("color: #1a1a1a; background-color: transparent;");
    }
    return check;
}

void ConfigWidgetFactory::applyStyle(QWidget* widget)
{
    if (m_darkTheme) {
        // 深色主题样式
        widget->setStyleSheet(R"(
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                background-color: #1a2332;
                color: #e8f4f8;
                border: 1px solid #2d3748;
                border-radius: 4px;
                padding: 6px 10px;
                min-height: 28px;
            }
            QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
                border: 1px solid #00d4aa;
            }
            QSpinBox::up-button, QSpinBox::down-button,
            QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
                background-color: #2d3748;
                border-radius: 2px;
            }
            QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {
                background-color: #3d4758;
            }
            QComboBox {
                padding-right: 20px;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-top: 5px solid #64748b;
            }
        )");
    } else {
        // 浅色主题样式
        widget->setStyleSheet(R"(
            QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
                background-color: #ffffff;
                color: #1a1a1a;
                border: 1px solid #d1d5db;
                border-radius: 4px;
                padding: 6px 10px;
                min-height: 28px;
            }
            QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus {
                border: 1px solid #00d4aa;
            }
            QSpinBox::up-button, QSpinBox::down-button,
            QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
                background-color: #e5e7eb;
                border-radius: 2px;
            }
            QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover {
                background-color: #d1d5db;
            }
            QComboBox {
                padding-right: 20px;
            }
            QComboBox::drop-down {
                border: none;
                width: 20px;
            }
            QComboBox::down-arrow {
                image: none;
                border-left: 5px solid transparent;
                border-right: 5px solid transparent;
                border-top: 5px solid #6b7280;
            }
        )");
    }
}

} // namespace DeepLux
