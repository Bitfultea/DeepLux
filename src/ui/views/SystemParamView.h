#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>

namespace DeepLux {

class SystemParamView : public QDialog {
    Q_OBJECT

public:
    explicit SystemParamView(QWidget* parent = nullptr,
                             const QString& requiredPermission = QStringLiteral("SystemParam"));
    ~SystemParamView() override = default;

private slots:
    void onResetClicked();
    void onCloseClicked();
    void onSaveClicked();

private:
    void setupUI();
    bool hasRequiredPermission() const;
    bool ensureRequiredPermission();
    void updatePermissionState();
    void clearSettings();
    void loadSettings();
    void saveSettings();

    QString m_requiredPermission;
    bool m_permissionGranted = false;
    QLabel* m_permissionMessage = nullptr;

    // General settings
    QGroupBox* m_generalGroup = nullptr;
    QCheckBox* m_autoLoadCheck = nullptr;
    QCheckBox* m_autoSaveCheck = nullptr;
    QSpinBox* m_autoSaveIntervalSpin = nullptr;

    // Display settings
    QGroupBox* m_displayGroup = nullptr;
    QComboBox* m_languageCombo = nullptr;
    QComboBox* mThemeCombo = nullptr;

    // Run settings
    QGroupBox* m_runGroup = nullptr;
    QSpinBox* m_cycleIntervalSpin = nullptr;

    // Log settings
    QGroupBox* m_logGroup = nullptr;
    QSpinBox* m_logLevelCombo = nullptr;
    QCheckBox* m_enableFileLogCheck = nullptr;

    // Buttons
    QPushButton* m_resetBtn = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
};

} // namespace DeepLux
