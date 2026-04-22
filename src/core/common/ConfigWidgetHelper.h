#pragma once

/**
 * @brief 配置控件样式助手 - 头文件-only 实现，插件可直接 include
 *
 * 用法:
 *   #include "core/common/ConfigWidgetHelper.h"
 *
 *   auto factory = ConfigWidgetHelper();  // 自动使用当前主题
 *   // 或
 *   auto factory = ConfigWidgetHelper(true);  // 强制深色主题
 *
 *   QLineEdit* edit = factory.createLineEdit("default", parent);
 *   QSpinBox* spin = factory.createSpinBox(0, 100, 50, parent);
 *
 * 主题切换时更新所有控件样式:
 *   ConfigWidgetHelper::updateAllWidgetsStyle(widget, isDark);
 *
 * 主题切换前设置全局主题状态:
 *   ConfigWidgetHelper::setGlobalDarkTheme(isDark);
 */

#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QWidget>
#include <QSizePolicy>

class ConfigWidgetHelper
{
public:
    // 静态方法：设置全局主题状态（主题切换前调用）
    static void setGlobalDarkTheme(bool isDark) { s_globalDarkTheme = isDark; }
    static bool globalDarkTheme() { return s_globalDarkTheme; }

    // 构造函数：默认使用全局主题状态，也可指定特定主题
    ConfigWidgetHelper(bool darkTheme = true) : m_darkTheme(darkTheme) {}

    // 使用全局主题状态初始化
    static ConfigWidgetHelper createWithGlobalTheme() {
        return ConfigWidgetHelper(s_globalDarkTheme);
    }

    void setDarkTheme(bool dark) { m_darkTheme = dark; }
    bool isDarkTheme() const { return m_darkTheme; }

    // 创建标签（不会水平伸展）
    QLabel* createLabel(const QString& text, QWidget* parent = nullptr)
    {
        QLabel* label = new QLabel(text, parent);
        // 设置为优先使用最小尺寸，避免在 QVBoxLayout 中水平伸展
        label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        if (m_darkTheme) {
            label->setStyleSheet("color: #e8f4f8; background-color: transparent; border: none;");
        } else {
            label->setStyleSheet("color: #1a1a1a; background-color: transparent; border: none;");
        }
        return label;
    }

    // 创建单行输入框
    QLineEdit* createLineEdit(const QString& text = QString(), QWidget* parent = nullptr)
    {
        QLineEdit* edit = new QLineEdit(text, parent);
        applyInputStyle(edit);
        return edit;
    }

    // 创建整数输入框
    QSpinBox* createSpinBox(int min = 0, int max = 999999, int value = 0, QWidget* parent = nullptr)
    {
        QSpinBox* spin = new QSpinBox(parent);
        spin->setRange(min, max);
        spin->setValue(value);
        applyInputStyle(spin);
        return spin;
    }

    // 创建浮点数输入框
    QDoubleSpinBox* createDoubleSpinBox(double min = 0.0, double max = 999999.0, double value = 0.0, int decimals = 3, QWidget* parent = nullptr)
    {
        QDoubleSpinBox* spin = new QDoubleSpinBox(parent);
        spin->setRange(min, max);
        spin->setValue(value);
        spin->setDecimals(decimals);
        applyInputStyle(spin);
        return spin;
    }

    // 创建下拉框
    QComboBox* createComboBox(QWidget* parent = nullptr)
    {
        QComboBox* combo = new QComboBox(parent);
        applyInputStyle(combo);
        return combo;
    }

    // 创建复选框
    QCheckBox* createCheckBox(const QString& text = QString(), bool checked = false, QWidget* parent = nullptr)
    {
        QCheckBox* check = new QCheckBox(text, parent);
        check->setChecked(checked);
        if (m_darkTheme) {
            check->setStyleSheet("color: #e8f4f8; background-color: transparent; border: none;");
        } else {
            check->setStyleSheet("color: #1a1a1a; background-color: transparent; border: none;");
        }
        return check;
    }

    // 应用输入控件样式
    void applyInputStyle(QWidget* widget)
    {
        if (m_darkTheme) {
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

    // 应用容器样式（用于 QWidget 作为配置页面）
    void applyContainerStyle(QWidget* widget)
    {
        if (m_darkTheme) {
            widget->setStyleSheet(R"(
                background-color: #1a2332;
                color: #e8f4f8;
            )");
        } else {
            widget->setStyleSheet(R"(
                background-color: #f9fafb;
                color: #1a1a1a;
            )");
        }
    }

    // 静态方法：递归更新所有子控件的样式（用于主题切换）
    // skipObjectNames: 跳过具有这些对象名的控件（如日志过滤器等使用 MainWindow 样式的控件）
    static void updateAllWidgetsStyle(QWidget* widget, bool isDark,
        const QSet<QString>& skipObjectNames = {"LogFilterCombo", "LogPanel"})
    {
        if (!widget) return;

        // 跳过指定对象名的控件（保持原有样式）
        if (skipObjectNames.contains(widget->objectName())) {
            return;
        }

        // 更新自身
        updateWidgetStyle(widget, isDark);

        // 递归更新所有子控件
        const QObjectList& children = widget->children();
        for (QObject* child : children) {
            QWidget* childWidget = qobject_cast<QWidget*>(child);
            if (childWidget) {
                updateAllWidgetsStyle(childWidget, isDark, skipObjectNames);
            }
        }
    }

    // 静态方法：更新单个控件样式
    static void updateWidgetStyle(QWidget* widget, bool isDark)
    {
        if (!widget) return;

        QString inputStyle = isDark ? R"(
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
        )" : R"(
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
        )";

        QString labelStyle = isDark
            ? "color: #e8f4f8; background-color: transparent; border: none;"
            : "color: #1a1a1a; background-color: transparent; border: none;";

        QString checkStyle = isDark
            ? "color: #e8f4f8; background-color: transparent; border: none;"
            : "color: #1a1a1a; background-color: transparent; border: none;";

        QString containerStyle = isDark
            ? "background-color: #1a2332; color: #e8f4f8;"
            : "background-color: #f9fafb; color: #1a1a1a;";

        // 根据控件类型设置样式
        if (qobject_cast<QLineEdit*>(widget) ||
            qobject_cast<QSpinBox*>(widget) ||
            qobject_cast<QDoubleSpinBox*>(widget) ||
            qobject_cast<QComboBox*>(widget)) {
            widget->setStyleSheet(inputStyle);
        } else if (qobject_cast<QLabel*>(widget)) {
            widget->setStyleSheet(labelStyle);
        } else if (qobject_cast<QCheckBox*>(widget)) {
            widget->setStyleSheet(checkStyle);
        } else if (qobject_cast<QGroupBox*>(widget)) {
            // GroupBox 需要特殊处理
            QString groupStyle = isDark ? R"(
                QGroupBox {
                    font-weight: bold;
                    color: #e8f4f8;
                    border: 1px solid #2d3748;
                    border-radius: 6px;
                    margin-top: 12px;
                    padding-top: 12px;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 12px;
                    padding: 0 4px;
                }
            )" : R"(
                QGroupBox {
                    font-weight: bold;
                    color: #1a1a1a;
                    border: 1px solid #d1d5db;
                    border-radius: 6px;
                    margin-top: 12px;
                    padding-top: 12px;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 12px;
                    padding: 0 4px;
                }
            )";
            widget->setStyleSheet(groupStyle);
        }
    }

private:
    bool m_darkTheme;
    static bool s_globalDarkTheme;  // 默认深色主题
};

// 静态成员定义
inline bool ConfigWidgetHelper::s_globalDarkTheme = true;
