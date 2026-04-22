#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>

namespace DeepLux {

class CommunicationSetView : public QDialog
{
    Q_OBJECT

public:
    explicit CommunicationSetView(QWidget* parent = nullptr);
    ~CommunicationSetView() override = default;

private slots:
    void onAddClicked();
    void onDeleteClicked();
    void onConnectClicked();
    void onDisconnectClicked();
    void onApplyClicked();
    void onCloseClicked();
    void onCommSelectionChanged();
    void onTypeChanged(int index);

private:
    void setupUI();
    void loadCommunications();
    void updateButtons();

    QTableWidget* m_commTable = nullptr;
    QPushButton* m_addBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QPushButton* m_disconnectBtn = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;

    // Communication settings
    QComboBox* m_typeCombo = nullptr;
    QLineEdit* m_nameEdit = nullptr;
    QLineEdit* m_ipEdit = nullptr;
    QSpinBox* m_portSpin = nullptr;
    QComboBox* m_portNameCombo = nullptr;
    QSpinBox* m_baudRateSpin = nullptr;
    QComboBox* m_parityCombo = nullptr;

    // Settings widget
    QWidget* m_tcpSettingsWidget = nullptr;
    QWidget* m_serialSettingsWidget = nullptr;
};

} // namespace DeepLux