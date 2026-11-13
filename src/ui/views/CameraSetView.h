#pragma once

#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>

namespace DeepLux {

struct CameraStatus;
struct CameraOperationSnapshot;

class CameraSetView : public QDialog {
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
    bool hasCameraPermissions() const;
    void updatePermissionState();
    void loadCameras();
    void loadCamerasInternal();
    void renderCameraCache();
    void updateDeviceRow(const QString& deviceId);
    int rowForDeviceId(const QString& deviceId) const;
    QString displayState(const CameraStatus& status, const CameraOperationSnapshot* pending) const;
    void showCameraOperationError(const QString& deviceId, const QString& errorCode);
    void updateDiscoveryStatus();
    void updateButtons();

    QTableWidget* m_cameraTable = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_connectBtn = nullptr;
    QPushButton* m_disconnectBtn = nullptr;
    QPushButton* m_deleteBtn = nullptr;
    QPushButton* m_applyBtn = nullptr;
    QPushButton* m_closeBtn = nullptr;
    QLabel* m_discoveryStatus = nullptr;

    // Camera settings
    QLineEdit* m_ipEdit = nullptr;
    QSpinBox* m_portSpin = nullptr;
    QComboBox* m_protocolCombo = nullptr;
    bool m_permissionGranted = false;
    bool m_permissionInitialized = false;
};

} // namespace DeepLux
