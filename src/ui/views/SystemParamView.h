#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>

namespace DeepLux {

class SystemParamView : public QDialog
{
    Q_OBJECT

public:
    explicit SystemParamView(QWidget* parent = nullptr);
    ~SystemParamView() override = default;

private slots:
    void onApplyClicked();
    void onResetClicked();
    void onCloseClicked();
    void onSaveClicked();

private:
    void setupUI();
    void loadSettings();
    void saveSettings();

    // General settings
    QGroupBox* m_generalGroup = nullptr;
    QLineEdit* m_projectPathEdit = nullptr;
    QCheckBox* m_autoLoadCheck = nullptr;
    QCheckBox* m_autoSaveCheck = nullptr;
    QSpinBox* m_autoSaveIntervalSpin = nullptr;

    // Display settings
    QGroupBox* m_displayGroup = nullptr;
    QComboBox* m_languageCombo = nullptr;
    QComboBox* mThemeCombo = nullptr;
    QCheckBox* m_showToolbarCheck = nullptr;
    QCheckBox* m_showStatusbarCheck = nullptr;

    // Run settings
    QGroupBox* m_runGroup = nullptr;
    QSpinBox* m_cycleIntervalSpin = nullptr;
    QSpinBox* m_maxLoopCountSpin = nullptr;
    QCheckBox* m_stopOnErrorCheck = nullptr;

    // Log settings
    QGroupBox* m_logGroup = nullptr;
    QSpinBox* m_logLevelCombo = nullptr;
    QSpinBox* m_logMaxSizeSpin = nullptr;
    QCheckBox* m_enableFileLogCheck = nullptr;

    // Buttons
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_resetBtn = nullptr;
    QPushButton* m_saveBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
};

} // namespace DeepLux