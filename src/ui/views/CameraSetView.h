#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QComboBox>
#include <QLineEdit>
#include <QSpinBox>

namespace DeepLux {

class CameraSetView : public QDialog
{
    Q_OBJECT

public:
    explicit CameraSetView(QWidget* parent = nullptr);
    ~CameraSetView() override = default;

private slots:
    void onRefreshClicked();
    void onConnectClicked();
    void onDisconnectClicked();
    void onDeleteClicked();
    void onApplyClicked();
    void onCloseClicked();
    void onCameraSelectionChanged();

private:
    void setupUI();
    void loadCameras();
    void loadCamerasInternal();
    void updateButtons();

    QTableWidget* m_cameraTable = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QPushButton* m_disconnectBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;

    // Camera settings
    QLineEdit* m_ipEdit = nullptr;
    QSpinBox* m_portSpin = nullptr;
    QComboBox* m_protocolCombo = nullptr;
};

} // namespace DeepLux