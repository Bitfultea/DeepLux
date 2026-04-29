#ifndef DEEPLUX_AGENT_SETTINGS_DIALOG_H
#define DEEPLUX_AGENT_SETTINGS_DIALOG_H

#include <QDialog>
#include "core/agent/AgentController.h"

class QLineEdit;
class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QTextEdit;
class QCheckBox;

namespace DeepLux {

/**
 * @brief Agent 设置对话框
 *
 * 配置模型端点、API Key、温度、权限级别、系统提示词。
 */
class AgentSettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AgentSettingsDialog(QWidget* parent = nullptr);

    QString endpoint() const;
    QString apiKey() const;
    QString model() const;
    double temperature() const;
    int maxTokens() const;
    AgentController::PermissionLevel permissionLevel() const;
    QString systemPrompt() const;
    bool toolsEnabled() const;

private:
    void setupUi();
    void loadSettings();
    void saveSettings();

    QLineEdit* m_endpointEdit = nullptr;
    QLineEdit* m_apiKeyEdit = nullptr;
    QLineEdit* m_modelEdit = nullptr;
    QDoubleSpinBox* m_tempSpin = nullptr;
    QSpinBox* m_maxTokensSpin = nullptr;
    QComboBox* m_permissionCombo = nullptr;
    QTextEdit* m_systemPromptEdit = nullptr;
    QCheckBox* m_toolsCheck = nullptr;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_SETTINGS_DIALOG_H
