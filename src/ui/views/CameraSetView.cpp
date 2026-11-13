#include "CameraSetView.h"

#include "../widgets/AppIconProvider.h"
#include "common/Logger.h"
#include "core/common/UserManager.h"
#include "core/device/CameraManager.h"
#include "core/manager/PluginManager.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>
#include <algorithm>

namespace DeepLux {

CameraSetView::CameraSetView(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Camera Settings"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setMinimumSize(700, 500);
    // 样式跟随主窗口主题（ThemeManager 统一管理），不再硬编码

    setupUI();
    connect(&UserManager::instance(), &UserManager::permissionChanged, this, &CameraSetView::updatePermissionState);
    updatePermissionState();
}

void CameraSetView::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Camera list table
    QGroupBox* listGroup = new QGroupBox(tr("Camera List"), this);
    QVBoxLayout* listLayout = new QVBoxLayout(listGroup);

    m_discoveryStatus = new QLabel(this);
    m_discoveryStatus->setObjectName(QStringLiteral("cameraDiscoveryStatus"));
    m_discoveryStatus->setVisible(false);
    listLayout->addWidget(m_discoveryStatus);

    m_cameraTable = new QTableWidget(this);
    m_cameraTable->setColumnCount(5);
    m_cameraTable->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("IP"), tr("Status"), tr("Serial")});
    m_cameraTable->horizontalHeader()->setStretchLastSection(true);
    m_cameraTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_cameraTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_cameraTable->setAlternatingRowColors(true);
    m_cameraTable->verticalHeader()->setVisible(false);
    listLayout->addWidget(m_cameraTable);

    // Buttons
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    m_refreshBtn = new QPushButton(tr("Refresh"), this);
    m_refreshBtn->setObjectName(QStringLiteral("cameraRefreshButton"));
    m_refreshBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Refresh, 16, QColor("#2563EB")));
    m_connectBtn = new QPushButton(tr("Connect"), this);
    m_connectBtn->setObjectName(QStringLiteral("cameraConnectButton"));
    m_connectBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Connect, 16, QColor("#16A34A")));
    m_disconnectBtn = new QPushButton(tr("Disconnect"), this);
    m_disconnectBtn->setObjectName(QStringLiteral("cameraDisconnectButton"));
    m_disconnectBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Disconnect, 16, QColor("#DC2626")));
    m_disconnectBtn->setEnabled(false);
    m_deleteBtn = new QPushButton(tr("Delete"), this);
    m_deleteBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Delete, 16, QColor("#DC2626")));
    m_deleteBtn->setEnabled(false);

    btnLayout->addWidget(m_refreshBtn);
    btnLayout->addWidget(m_connectBtn);
    btnLayout->addWidget(m_disconnectBtn);
    btnLayout->addWidget(m_deleteBtn);
    btnLayout->addStretch();

    listLayout->addLayout(btnLayout);
    mainLayout->addWidget(listGroup, 1);

    // Settings group
    QGroupBox* settingsGroup = new QGroupBox(tr("Camera Settings"), this);
    QFormLayout* settingsLayout = new QFormLayout(settingsGroup);
    settingsLayout->setSpacing(10);

    m_ipEdit = new QLineEdit(this);
    m_ipEdit->setObjectName(QStringLiteral("cameraIpEdit"));
    m_ipEdit->setPlaceholderText("192.168.1.100");
    settingsLayout->addRow(tr("IP Address:"), m_ipEdit);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setObjectName(QStringLiteral("cameraPortSpin"));
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(8000);
    settingsLayout->addRow(tr("Port:"), m_portSpin);

    m_protocolCombo = new QComboBox(this);
    m_protocolCombo->setObjectName(QStringLiteral("cameraProtocolCombo"));
    m_protocolCombo->addItem("GigE Vision", 0);
    m_protocolCombo->addItem("USB3 Vision", 1);
    m_protocolCombo->addItem("CoaXPress", 2);
    settingsLayout->addRow(tr("Protocol:"), m_protocolCombo);

    mainLayout->addWidget(settingsGroup);

    // Bottom buttons
    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->setSpacing(10);

    m_applyBtn = new QPushButton(tr("Apply"), this);
    m_applyBtn->setObjectName(QStringLiteral("cameraApplyButton"));
    m_applyBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Confirm, 16, QColor("#16A34A")));
    m_applyBtn->setProperty("default", true);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 16, QColor("#4B5563")));

    bottomLayout->addStretch();
    bottomLayout->addWidget(m_applyBtn);
    bottomLayout->addWidget(m_closeBtn);

    mainLayout->addLayout(bottomLayout);

    // Connections
    connect(m_refreshBtn, &QPushButton::clicked, this, &CameraSetView::onRefreshClicked);
    connect(m_connectBtn, &QPushButton::clicked, this, &CameraSetView::onConnectClicked);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &CameraSetView::onDisconnectClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &CameraSetView::onDeleteClicked);
    connect(m_applyBtn, &QPushButton::clicked, this, &CameraSetView::onApplyClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cameraTable, &QTableWidget::itemSelectionChanged, this, &CameraSetView::onCameraSelectionChanged);
    connect(&CameraManager::instance(), &CameraManager::cameraListChanged, this, &CameraSetView::renderCameraCache);
    connect(&CameraManager::instance(), &CameraManager::cameraDiscoverySettled, this,
            [this](const QString&, quint64, const QString&, const QString&) { updateDiscoveryStatus(); });
    connect(&CameraManager::instance(), &CameraManager::cameraOperationStarted, this,
            [this](const QString& deviceId, quint64, const QString&) { updateDeviceRow(deviceId); });
    connect(&CameraManager::instance(), &CameraManager::cameraOperationSettled, this,
            [this](const QString& deviceId, quint64, const QString&, const QString&, const QString&) {
                updateDeviceRow(deviceId);
            });
    connect(&CameraManager::instance(), &CameraManager::cameraStateChanged, this,
            [this](const QString& deviceId, CameraState) { updateDeviceRow(deviceId); });
}

void CameraSetView::loadCameras() {
    if (!hasCameraPermissions())
        return;
    m_cameraTable->setRowCount(0);

    // 等待插件加载完成后再发现相机（lambda connect 不能用 UniqueConnection，
    // 先显式断开再连接以保持单次连接）
    disconnect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, nullptr);
    connect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, [this]() {
        disconnect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, nullptr);
        if (hasCameraPermissions())
            loadCamerasInternal();
    });

    // 如果插件已经加载完成，立即加载
    if (PluginManager::instance().isInitialized()) {
        disconnect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, nullptr);
        loadCamerasInternal();
    }
}

void CameraSetView::loadCamerasInternal() {
    if (!hasCameraPermissions())
        return;
    CameraManager::instance().discoverCameras();
    renderCameraCache();
    updateDiscoveryStatus();
}

void CameraSetView::renderCameraCache() {
    if (!hasCameraPermissions())
        return;
    const QList<CameraStatus> cameras = CameraManager::instance().knownCameras();
    m_cameraTable->setRowCount(0);

    for (const CameraStatus& cam : cameras) {
        int row = m_cameraTable->rowCount();
        m_cameraTable->insertRow(row);

        m_cameraTable->setItem(row, 0, new QTableWidgetItem(cam.name));
        m_cameraTable->setItem(row, 1, new QTableWidgetItem(cam.deviceId));
        m_cameraTable->setItem(row, 2, new QTableWidgetItem(QString())); // IP not available in status
        m_cameraTable->setItem(row, 3, new QTableWidgetItem(displayState(cam, nullptr)));
        m_cameraTable->setItem(row, 4, new QTableWidgetItem(QString())); // Serial not available in status
        m_cameraTable->item(row, 3)->setToolTip(cam.lastError);
        CameraOperationSnapshot pending;
        if (CameraManager::instance().pendingCameraOperation(cam.deviceId, &pending))
            m_cameraTable->item(row, 3)->setText(displayState(cam, &pending));
    }

    updateButtons();
}

int CameraSetView::rowForDeviceId(const QString& deviceId) const {
    for (int row = 0; row < m_cameraTable->rowCount(); ++row) {
        const QTableWidgetItem* item = m_cameraTable->item(row, 1);
        if (item && item->text() == deviceId)
            return row;
    }
    return -1;
}

QString CameraSetView::displayState(const CameraStatus& status, const CameraOperationSnapshot* pending) const {
    if (pending) {
        if (pending->operation == QLatin1String("connect"))
            return tr("Connecting…");
        if (pending->operation == QLatin1String("disconnect"))
            return tr("Disconnecting…");
        if (pending->operation == QLatin1String("get_details"))
            return tr("Reading details…");
        if (pending->operation == QLatin1String("set_parameter"))
            return tr("Updating parameter…");
    }
    if (status.state == CameraState::Connected || status.state == CameraState::Grabbing)
        return tr("Connected");
    if (status.lastError == QLatin1String("CAMERA_OPERATION_CANCELLED"))
        return tr("Cancelled");
    if (status.state == CameraState::Error || !status.lastError.isEmpty())
        return tr("Error");
    return tr("Disconnected");
}

void CameraSetView::updateDeviceRow(const QString& deviceId) {
    if (!hasCameraPermissions())
        return;
    const int row = rowForDeviceId(deviceId);
    if (row < 0)
        return;
    const QList<CameraStatus> statuses = CameraManager::instance().knownCameras();
    const auto status = std::find_if(statuses.cbegin(), statuses.cend(),
                                     [&deviceId](const CameraStatus& value) { return value.deviceId == deviceId; });
    if (status == statuses.cend())
        return;
    CameraOperationSnapshot pending;
    const bool isPending = CameraManager::instance().pendingCameraOperation(deviceId, &pending);
    QTableWidgetItem* stateItem = m_cameraTable->item(row, 3);
    if (!stateItem)
        return;
    stateItem->setText(displayState(*status, isPending ? &pending : nullptr));
    stateItem->setToolTip(status->lastError);
    updateButtons();
}

void CameraSetView::showCameraOperationError(const QString& deviceId, const QString& errorCode) {
    const int row = rowForDeviceId(deviceId);
    if (row < 0)
        return;
    CameraOperationSnapshot pending;
    if (CameraManager::instance().pendingCameraOperation(deviceId, &pending)) {
        updateDeviceRow(deviceId);
        return;
    }
    if (QTableWidgetItem* stateItem = m_cameraTable->item(row, 3)) {
        stateItem->setText(tr("Error"));
        stateItem->setToolTip(errorCode);
    }
    updateButtons();
}

void CameraSetView::updateDiscoveryStatus() {
    if (!hasCameraPermissions()) {
        m_discoveryStatus->clear();
        m_discoveryStatus->hide();
        return;
    }
    const bool pending = CameraManager::instance().pendingDiscoveryCount() > 0;
    m_discoveryStatus->setText(pending ? tr("Discovering cameras…") : QString());
    m_discoveryStatus->setVisible(pending);
}

void CameraSetView::updateButtons() {
    if (!m_permissionGranted) {
        m_connectBtn->setEnabled(false);
        m_disconnectBtn->setEnabled(false);
        m_deleteBtn->setEnabled(false);
        return;
    }

    bool hasSelection = m_cameraTable->currentRow() >= 0;
    m_deleteBtn->setEnabled(hasSelection);

    if (hasSelection) {
        int row = m_cameraTable->currentRow();
        const QString deviceId = m_cameraTable->item(row, 1)->text();
        CameraOperationSnapshot pending;
        if (CameraManager::instance().pendingCameraOperation(deviceId, &pending)) {
            m_connectBtn->setEnabled(false);
            m_disconnectBtn->setEnabled(false);
            return;
        }
        const QList<CameraStatus> statuses = CameraManager::instance().knownCameras();
        const auto status = std::find_if(statuses.cbegin(), statuses.cend(),
                                         [&deviceId](const CameraStatus& value) { return value.deviceId == deviceId; });
        const CameraState state = status == statuses.cend() ? CameraState::Disconnected : status->state;
        if (state == CameraState::Error) {
            m_connectBtn->setEnabled(false);
            m_disconnectBtn->setEnabled(true);
        } else if (state == CameraState::Connected || state == CameraState::Grabbing) {
            m_connectBtn->setEnabled(false);
            m_disconnectBtn->setEnabled(true);
        } else {
            m_connectBtn->setEnabled(true);
            m_disconnectBtn->setEnabled(false);
        }
    } else {
        m_connectBtn->setEnabled(false);
        m_disconnectBtn->setEnabled(false);
    }
}

bool CameraSetView::hasCameraPermissions() const {
    const UserManager& users = UserManager::instance();
    return users.isLoggedIn() && users.currentUser().type != UserType::Guest &&
           users.hasPermission(QStringLiteral("Camera")) && users.hasPermission(QStringLiteral("CameraSet"));
}

void CameraSetView::updatePermissionState() {
    const bool allowed = hasCameraPermissions();
    if (m_permissionInitialized && allowed == m_permissionGranted)
        return;
    const bool initialState = !m_permissionInitialized;
    m_permissionInitialized = true;
    m_permissionGranted = allowed;

    m_cameraTable->setEnabled(allowed);
    m_refreshBtn->setEnabled(allowed);
    m_applyBtn->setEnabled(allowed);
    m_ipEdit->setEnabled(allowed);
    m_portSpin->setEnabled(allowed);
    m_protocolCombo->setEnabled(allowed);
    m_discoveryStatus->clear();
    m_discoveryStatus->hide();
    if (!allowed) {
        m_cameraTable->setRowCount(0);
        m_ipEdit->clear();
        m_portSpin->setValue(m_portSpin->minimum());
        m_protocolCombo->setCurrentIndex(-1);
    }
    updateButtons();

    if (allowed && initialState)
        loadCameras();
}

void CameraSetView::onRefreshClicked() {
    if (!hasCameraPermissions())
        return;
    CameraManager::instance().refreshCameras();
    renderCameraCache();
    updateDiscoveryStatus();
    Logger::instance().info("Camera list refreshed", "Camera");
}

void CameraSetView::onConnectClicked() {
    if (!hasCameraPermissions())
        return;
    int row = m_cameraTable->currentRow();
    if (row < 0)
        return;

    QString cameraId = m_cameraTable->item(row, 1)->text(); // deviceId is in column 1
    CameraManager& manager = CameraManager::instance();
    CameraOperationSnapshot pending;
    if (manager.pendingCameraOperation(cameraId, &pending))
        return;
    if (manager.supportsAsyncOperations(cameraId) && !manager.getCamera(cameraId)) {
        quint64 requestId = 0;
        QString errorCode;
        if (!manager.requestConnectCameraAsync(cameraId, &requestId, &errorCode))
            showCameraOperationError(cameraId, errorCode);
        return;
    }
    bool ok = manager.connectCamera(cameraId);

    if (!ok) {
        QString error = CameraManager::instance().lastError(cameraId);
        if (error.isEmpty()) {
            error = tr("连接相机失败，请检查日志了解详细原因");
        }
        Logger::instance().error(QString("Camera connect failed: %1 - %2").arg(cameraId).arg(error), "Camera");
        QMessageBox::warning(this, tr("连接失败"), error);
        return;
    }

    // 直接更新当前行状态，避免重新枚举导致 SDK 崩溃
    m_cameraTable->item(row, 3)->setText(tr("Connected"));
    updateButtons();
}

void CameraSetView::onDisconnectClicked() {
    if (!hasCameraPermissions())
        return;
    int row = m_cameraTable->currentRow();
    if (row < 0)
        return;

    QString cameraId = m_cameraTable->item(row, 1)->text(); // deviceId is in column 1
    CameraManager& manager = CameraManager::instance();
    CameraOperationSnapshot pending;
    if (manager.pendingCameraOperation(cameraId, &pending))
        return;
    if (manager.supportsAsyncOperations(cameraId) && !manager.getCamera(cameraId)) {
        quint64 requestId = 0;
        QString errorCode;
        if (!manager.requestDisconnectCameraAsync(cameraId, &requestId, &errorCode))
            showCameraOperationError(cameraId, errorCode);
        return;
    }
    manager.disconnectCamera(cameraId);

    // 直接更新当前行状态，避免重新枚举导致 SDK 崩溃
    m_cameraTable->item(row, 3)->setText(CameraManager::instance().isConnected(cameraId) ? tr("Connected")
                                                                                         : tr("Disconnected"));
    updateButtons();
}

void CameraSetView::onDeleteClicked() {
    if (!hasCameraPermissions())
        return;
    int row = m_cameraTable->currentRow();
    if (row < 0)
        return;

    m_cameraTable->removeRow(row);
    updateButtons();
}

void CameraSetView::onApplyClicked() {
    if (!hasCameraPermissions())
        return;
    // Apply camera settings
    QString ip = m_ipEdit->text();
    int port = m_portSpin->value();

    Logger::instance().info(QString("Camera settings applied: %1:%2").arg(ip).arg(port), "Camera");
    accept();
}

void CameraSetView::onCloseClicked() {
    accept();
}

void CameraSetView::onCameraSelectionChanged() {
    if (!hasCameraPermissions())
        return;
    updateButtons();

    int row = m_cameraTable->currentRow();
    if (row >= 0) {
        QString ip = m_cameraTable->item(row, 2)->text();
        m_ipEdit->setText(ip);
    }
}

} // namespace DeepLux
