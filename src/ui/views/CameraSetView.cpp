#include "CameraSetView.h"
#include "core/device/CameraManager.h"
#include "core/manager/PluginManager.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>

namespace DeepLux {

CameraSetView::CameraSetView(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Camera Settings"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setMinimumSize(700, 500);
    setStyleSheet(R"(
        QDialog {
            background-color: #2b2b2b;
            color: #ffffff;
        }
        QLabel {
            color: #ffffff;
            background-color: transparent;
        }
        QGroupBox {
            border: 1px solid #555;
            border-radius: 6px;
            margin-top: 10px;
            padding-top: 10px;
            color: #ffffff;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            padding: 0 5px;
        }
        QTableWidget {
            background-color: #3a3a3a;
            alternate-background-color: #333;
            color: #ffffff;
            border: 1px solid #555;
            gridline-color: #444;
        }
        QTableWidget::item {
            padding: 5px;
        }
        QTableWidget::item:selected {
            background-color: #0078d4;
        }
        QHeaderView::section {
            background-color: #3a3a3a;
            color: #ffffff;
            padding: 8px;
            border: none;
            border-right: 1px solid #555;
            border-bottom: 1px solid #555;
        }
        QPushButton {
            padding: 8px 16px;
            border: none;
            border-radius: 4px;
            background-color: #3a3a3a;
            color: #ffffff;
            font-size: 13px;
        }
        QPushButton:hover {
            background-color: #444;
        }
        QPushButton:disabled {
            background-color: #2a2a2a;
            color: #666;
        }
        QPushButton[default="true"] {
            background-color: #0078d4;
        }
        QPushButton[default="true"]:hover {
            background-color: #1084d8;
        }
        QLineEdit, QSpinBox, QComboBox {
            padding: 8px;
            border: 1px solid #555;
            border-radius: 4px;
            background-color: #3a3a3a;
            color: #ffffff;
        }
        QLineEdit:focus, QSpinBox:focus, QComboBox:focus {
            border: 1px solid #0078d4;
        }
    )");

    setupUI();
    loadCameras();
}

void CameraSetView::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Camera list table
    QGroupBox* listGroup = new QGroupBox(tr("Camera List"), this);
    QVBoxLayout* listLayout = new QVBoxLayout(listGroup);

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

    m_refreshBtn = new QPushButton(tr("🔄 Refresh"), this);
    m_connectBtn = new QPushButton(tr("▶ Connect"), this);
    m_disconnectBtn = new QPushButton(tr("■ Disconnect"), this);
    m_disconnectBtn->setEnabled(false);
    m_deleteBtn = new QPushButton(tr("🗑 Delete"), this);
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
    m_ipEdit->setPlaceholderText("192.168.1.100");
    settingsLayout->addRow(tr("IP Address:"), m_ipEdit);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(8000);
    settingsLayout->addRow(tr("Port:"), m_portSpin);

    m_protocolCombo = new QComboBox(this);
    m_protocolCombo->addItem("GigE Vision", 0);
    m_protocolCombo->addItem("USB3 Vision", 1);
    m_protocolCombo->addItem("CoaXPress", 2);
    settingsLayout->addRow(tr("Protocol:"), m_protocolCombo);

    mainLayout->addWidget(settingsGroup);

    // Bottom buttons
    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->setSpacing(10);

    m_applyBtn = new QPushButton(tr("Apply"), this);
    m_applyBtn->setProperty("default", true);
    m_closeBtn = new QPushButton(tr("Close"), this);

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
}

void CameraSetView::loadCameras()
{
    m_cameraTable->setRowCount(0);

    // 等待插件加载完成后再发现相机
    connect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, [this]() {
        disconnect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, nullptr);
        loadCamerasInternal();
    }, Qt::UniqueConnection);

    // 如果插件已经加载完成，立即加载
    if (PluginManager::instance().isInitialized()) {
        disconnect(&PluginManager::instance(), &PluginManager::allPluginsLoaded, this, nullptr);
        loadCamerasInternal();
    }
}

void CameraSetView::loadCamerasInternal()
{
    QList<CameraStatus> cameras = CameraManager::instance().discoverCameras();

    for (const CameraStatus& cam : cameras) {
        int row = m_cameraTable->rowCount();
        m_cameraTable->insertRow(row);

        m_cameraTable->setItem(row, 0, new QTableWidgetItem(cam.name));
        m_cameraTable->setItem(row, 1, new QTableWidgetItem(cam.deviceId));
        m_cameraTable->setItem(row, 2, new QTableWidgetItem(QString()));  // IP not available in status
        m_cameraTable->setItem(row, 3, new QTableWidgetItem(cam.state == CameraState::Connected ? tr("Connected") : tr("Disconnected")));
        m_cameraTable->setItem(row, 4, new QTableWidgetItem(QString()));  // Serial not available in status
    }

    updateButtons();
}

void CameraSetView::updateButtons()
{
    bool hasSelection = m_cameraTable->currentRow() >= 0;
    m_deleteBtn->setEnabled(hasSelection);

    if (hasSelection) {
        int row = m_cameraTable->currentRow();
        QString status = m_cameraTable->item(row, 3)->text();
        bool isConnected = status == tr("Connected");
        m_connectBtn->setEnabled(!isConnected);
        m_disconnectBtn->setEnabled(isConnected);
    } else {
        m_connectBtn->setEnabled(false);
        m_disconnectBtn->setEnabled(false);
    }
}

void CameraSetView::onRefreshClicked()
{
    loadCameras();
    Logger::instance().info("Camera list refreshed", "Camera");
}

void CameraSetView::onConnectClicked()
{
    int row = m_cameraTable->currentRow();
    if (row < 0) return;

    QString cameraId = m_cameraTable->item(row, 1)->text();  // deviceId is in column 1
    bool ok = CameraManager::instance().connectCamera(cameraId);

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

void CameraSetView::onDisconnectClicked()
{
    int row = m_cameraTable->currentRow();
    if (row < 0) return;

    QString cameraId = m_cameraTable->item(row, 1)->text();  // deviceId is in column 1
    CameraManager::instance().disconnectCamera(cameraId);

    // 直接更新当前行状态，避免重新枚举导致 SDK 崩溃
    m_cameraTable->item(row, 3)->setText(tr("Disconnected"));
    updateButtons();
}

void CameraSetView::onDeleteClicked()
{
    int row = m_cameraTable->currentRow();
    if (row < 0) return;

    m_cameraTable->removeRow(row);
    updateButtons();
}

void CameraSetView::onApplyClicked()
{
    // Apply camera settings
    QString ip = m_ipEdit->text();
    int port = m_portSpin->value();

    Logger::instance().info(QString("Camera settings applied: %1:%2").arg(ip).arg(port), "Camera");
    accept();
}

void CameraSetView::onCloseClicked()
{
    accept();
}

void CameraSetView::onCameraSelectionChanged()
{
    updateButtons();

    int row = m_cameraTable->currentRow();
    if (row >= 0) {
        QString ip = m_cameraTable->item(row, 2)->text();
        m_ipEdit->setText(ip);
    }
}

} // namespace DeepLux