#include "SystemParamView.h"

#include "../widgets/AppIconProvider.h"
#include "common/Logger.h"
#include "core/config/SystemConfig.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace DeepLux {

SystemParamView::SystemParamView(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("System Parameters"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setMinimumSize(600, 550);

    setupUI();
    loadSettings();
}

void SystemParamView::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // General settings
    m_generalGroup = new QGroupBox(tr("General Settings"), this);
    QFormLayout* generalLayout = new QFormLayout(m_generalGroup);
    generalLayout->setSpacing(10);

    m_autoLoadCheck = new QCheckBox(tr("Auto-load last project"), this);
    generalLayout->addRow(QString(), m_autoLoadCheck);

    m_autoSaveCheck = new QCheckBox(tr("Auto-save enabled"), this);
    generalLayout->addRow(QString(), m_autoSaveCheck);

    m_autoSaveIntervalSpin = new QSpinBox(this);
    m_autoSaveIntervalSpin->setRange(30, 3600);
    m_autoSaveIntervalSpin->setSuffix(" s");
    generalLayout->addRow(tr("Auto-save Interval:"), m_autoSaveIntervalSpin);

    mainLayout->addWidget(m_generalGroup);

    // Display settings
    m_displayGroup = new QGroupBox(tr("Display Settings"), this);
    QFormLayout* displayLayout = new QFormLayout(m_displayGroup);
    displayLayout->setSpacing(10);

    m_languageCombo = new QComboBox(this);
    m_languageCombo->addItem("English", "en");
    m_languageCombo->addItem("简体中文", "zh_CN");
    m_languageCombo->addItem("繁體中文", "zh_TW");
    displayLayout->addRow(tr("Language:"), m_languageCombo);

    mThemeCombo = new QComboBox(this);
    mThemeCombo->addItem(tr("Dark Theme"), "dark");
    mThemeCombo->addItem(tr("Light Theme"), "light");
    displayLayout->addRow(tr("Theme:"), mThemeCombo);

    mainLayout->addWidget(m_displayGroup);

    // Run settings
    m_runGroup = new QGroupBox(tr("Run Settings"), this);
    QFormLayout* runLayout = new QFormLayout(m_runGroup);
    runLayout->setSpacing(10);

    m_cycleIntervalSpin = new QSpinBox(this);
    m_cycleIntervalSpin->setRange(10, 10000);
    m_cycleIntervalSpin->setSuffix(" ms");
    runLayout->addRow(tr("Cycle Interval:"), m_cycleIntervalSpin);

    mainLayout->addWidget(m_runGroup);

    // Log settings
    m_logGroup = new QGroupBox(tr("Log Settings"), this);
    QFormLayout* logLayout = new QFormLayout(m_logGroup);
    logLayout->setSpacing(10);

    m_logLevelCombo = new QSpinBox(this);
    m_logLevelCombo->setRange(0, 5);
    logLayout->addRow(tr("Log Level:"), m_logLevelCombo);

    m_enableFileLogCheck = new QCheckBox(tr("Enable file logging"), this);
    m_enableFileLogCheck->setChecked(true);
    logLayout->addRow(QString(), m_enableFileLogCheck);

    mainLayout->addWidget(m_logGroup);

    // Buttons
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    m_resetBtn = new QPushButton(tr("Reset"), this);
    m_resetBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Undo, 16, QColor("#D97706")));
    m_saveBtn = new QPushButton(tr("Save"), this);
    m_saveBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Save, 16, QColor("#2563EB")));
    m_saveBtn->setProperty("default", true);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 16, QColor("#4B5563")));

    btnLayout->addWidget(m_resetBtn);
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_closeBtn);

    mainLayout->addLayout(btnLayout);

    // Connections
    connect(m_resetBtn, &QPushButton::clicked, this, &SystemParamView::onResetClicked);
    connect(m_saveBtn, &QPushButton::clicked, this, &SystemParamView::onSaveClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &SystemParamView::onCloseClicked);
}

void SystemParamView::loadSettings() {
    SystemConfig& config = SystemConfig::instance();

    m_autoLoadCheck->setChecked(config.autoLoadSolution());
    m_autoSaveCheck->setChecked(config.autoSave());
    m_autoSaveIntervalSpin->setValue(config.autoSaveInterval());

    int langIndex = m_languageCombo->findData(config.language());
    if (langIndex >= 0)
        m_languageCombo->setCurrentIndex(langIndex);

    int themeIndex = mThemeCombo->findData(config.theme());
    if (themeIndex >= 0)
        mThemeCombo->setCurrentIndex(themeIndex);

    m_cycleIntervalSpin->setValue(config.cycleInterval());
    m_logLevelCombo->setValue(config.logLevel());
    m_enableFileLogCheck->setChecked(config.enableFileLog());
}

void SystemParamView::saveSettings() {
    SystemConfig& config = SystemConfig::instance();

    config.setAutoLoadSolution(m_autoLoadCheck->isChecked());
    config.setAutoSave(m_autoSaveCheck->isChecked());
    config.setAutoSaveInterval(m_autoSaveIntervalSpin->value());

    config.setLanguage(m_languageCombo->currentData().toString());
    config.setTheme(mThemeCombo->currentData().toString());

    config.setCycleInterval(m_cycleIntervalSpin->value());
    config.setLogLevel(m_logLevelCombo->value());
    config.setEnableFileLog(m_enableFileLogCheck->isChecked());

    config.save();
}

void SystemParamView::onResetClicked() {
    SystemConfig::instance().reset();
    loadSettings();
    Logger::instance().info("System parameters reset to defaults", "Config");
}

void SystemParamView::onSaveClicked() {
    saveSettings();
    Logger::instance().info("System parameters saved", "Config");
}

void SystemParamView::onCloseClicked() {
    accept();
}

} // namespace DeepLux
