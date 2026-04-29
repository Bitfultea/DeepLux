#include "AgentSettingsDialog.h"
#include "core/manager/ConfigManager.h"
#include "core/agent/AgentController.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QMessageBox>
#include <QCheckBox>

namespace DeepLux {

AgentSettingsDialog::AgentSettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setupUi();
    loadSettings();
}

void AgentSettingsDialog::setupUi()
{
    setWindowTitle("Agent Settings");
    setMinimumWidth(500);

    auto* mainLayout = new QVBoxLayout(this);
    auto* formLayout = new QFormLayout();

    m_endpointEdit = new QLineEdit(this);
    m_endpointEdit->setPlaceholderText("https://api.openai.com/v1/chat/completions");
    formLayout->addRow("Endpoint:", m_endpointEdit);

    m_apiKeyEdit = new QLineEdit(this);
    m_apiKeyEdit->setEchoMode(QLineEdit::Password);
    m_apiKeyEdit->setPlaceholderText("sk-...");
    formLayout->addRow("API Key:", m_apiKeyEdit);

    m_modelEdit = new QLineEdit(this);
    m_modelEdit->setPlaceholderText("e.g. gpt-4o, deepseek-chat, claude-3-5-sonnet");
    formLayout->addRow("Model:", m_modelEdit);

    m_tempSpin = new QDoubleSpinBox(this);
    m_tempSpin->setRange(0.0, 2.0);
    m_tempSpin->setSingleStep(0.1);
    m_tempSpin->setValue(0.3);
    formLayout->addRow("Temperature:", m_tempSpin);

    m_maxTokensSpin = new QSpinBox(this);
    m_maxTokensSpin->setRange(256, 16384);
    m_maxTokensSpin->setSingleStep(256);
    m_maxTokensSpin->setValue(4096);
    formLayout->addRow("Max Tokens:", m_maxTokensSpin);

    m_permissionCombo = new QComboBox(this);
    m_permissionCombo->addItem("Observer (read-only)", static_cast<int>(AgentController::PermissionLevel::Observer));
    m_permissionCombo->addItem("Advisor (confirm required)", static_cast<int>(AgentController::PermissionLevel::Advisor));
    m_permissionCombo->addItem("Autopilot (auto-execute)", static_cast<int>(AgentController::PermissionLevel::Autopilot));
    m_permissionCombo->setCurrentIndex(1);
    formLayout->addRow("Permission Level:", m_permissionCombo);

    m_systemPromptEdit = new QTextEdit(this);
    m_systemPromptEdit->setMaximumHeight(120);
    m_systemPromptEdit->setPlaceholderText("You are an AI assistant for DeepLux Vision...");
    formLayout->addRow("System Prompt:", m_systemPromptEdit);

    m_toolsCheck = new QCheckBox("Enable Function Calling (tools)", this);
    m_toolsCheck->setChecked(true);
    m_toolsCheck->setToolTip("Disable if your API provider does not support function calling (e.g. some DeepSeek models)");
    formLayout->addRow(m_toolsCheck);

    mainLayout->addLayout(formLayout);

    auto* btnLayout = new QHBoxLayout();
    QPushButton* okBtn = new QPushButton("OK", this);
    QPushButton* cancelBtn = new QPushButton("Cancel", this);
    QPushButton* testBtn = new QPushButton("Test Connection", this);
    btnLayout->addWidget(testBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    connect(okBtn, &QPushButton::clicked, this, [this]() {
        saveSettings();
        accept();
    });
    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(testBtn, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, "Test", "Connection test not yet implemented.");
    });
}

void AgentSettingsDialog::loadSettings()
{
    ConfigManager& cfg = ConfigManager::instance();
    m_endpointEdit->setText(cfg.groupString("agent", "endpoint", "https://api.openai.com/v1/chat/completions"));
    m_apiKeyEdit->setText(cfg.groupString("agent", "apiKey", ""));
    m_modelEdit->setText(cfg.groupString("agent", "model", "gpt-4o"));
    m_toolsCheck->setChecked(cfg.groupBool("agent", "toolsEnabled", true));
    m_tempSpin->setValue(cfg.groupDouble("agent", "temperature", 0.3));
    m_maxTokensSpin->setValue(cfg.groupInt("agent", "maxTokens", 4096));
    m_permissionCombo->setCurrentIndex(cfg.groupInt("agent", "permissionLevel", 1));
    m_systemPromptEdit->setPlainText(cfg.groupString("agent", "systemPrompt",
        "You are an AI assistant for DeepLux Vision, an industrial vision software. "
        "You can help users create and manage vision inspection workflows. "
        "You have access to tools for project management, module configuration, and flow execution. "
        "Always be concise and professional."));
}

void AgentSettingsDialog::saveSettings()
{
    ConfigManager& cfg = ConfigManager::instance();
    cfg.setGroupValue("agent", "endpoint", m_endpointEdit->text());
    cfg.setGroupValue("agent", "apiKey", m_apiKeyEdit->text());
    cfg.setGroupValue("agent", "model", m_modelEdit->text());
    cfg.setGroupValue("agent", "temperature", m_tempSpin->value());
    cfg.setGroupValue("agent", "maxTokens", m_maxTokensSpin->value());
    cfg.setGroupValue("agent", "permissionLevel", m_permissionCombo->currentIndex());
    cfg.setGroupValue("agent", "systemPrompt", m_systemPromptEdit->toPlainText());
    cfg.setGroupValue("agent", "toolsEnabled", m_toolsCheck->isChecked());
    cfg.save();
}

QString AgentSettingsDialog::endpoint() const
{
    return m_endpointEdit->text();
}

QString AgentSettingsDialog::apiKey() const
{
    return m_apiKeyEdit->text();
}

QString AgentSettingsDialog::model() const
{
    return m_modelEdit->text();
}

double AgentSettingsDialog::temperature() const
{
    return m_tempSpin->value();
}

int AgentSettingsDialog::maxTokens() const
{
    return m_maxTokensSpin->value();
}

AgentController::PermissionLevel AgentSettingsDialog::permissionLevel() const
{
    return static_cast<AgentController::PermissionLevel>(m_permissionCombo->currentData().toInt());
}

QString AgentSettingsDialog::systemPrompt() const
{
    return m_systemPromptEdit->toPlainText();
}

bool AgentSettingsDialog::toolsEnabled() const
{
    return m_toolsCheck->isChecked();
}

} // namespace DeepLux
