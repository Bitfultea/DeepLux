#include "CommunicationSetView.h"
#include "core/communication/CommunicationManager.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QGroupBox>
#include <QLabel>
#include <QSerialPortInfo>

namespace DeepLux {

CommunicationSetView::CommunicationSetView(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Communication Settings"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setMinimumSize(750, 550);
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
    loadCommunications();
}

void CommunicationSetView::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Communication list table
    QGroupBox* listGroup = new QGroupBox(tr("Communication List"), this);
    QVBoxLayout* listLayout = new QVBoxLayout(listGroup);

    m_commTable = new QTableWidget(this);
    m_commTable->setObjectName("CommunicationTable");
    m_commTable->setColumnCount(5);
    m_commTable->setHorizontalHeaderLabels({tr("Name"), tr("Type"), tr("Address"), tr("Port"), tr("Status")});
    m_commTable->horizontalHeader()->setStretchLastSection(true);
    m_commTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_commTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_commTable->setAlternatingRowColors(true);
    m_commTable->verticalHeader()->setVisible(false);
    listLayout->addWidget(m_commTable);

    // Buttons
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    m_addBtn = new QPushButton(tr("➕ Add"), this);
    m_deleteBtn = new QPushButton(tr("🗑 Delete"), this);
    m_deleteBtn->setEnabled(false);
    m_connectBtn = new QPushButton(tr("▶ Connect"), this);
    m_disconnectBtn = new QPushButton(tr("■ Disconnect"), this);
    m_disconnectBtn->setEnabled(false);

    btnLayout->addWidget(m_addBtn);
    btnLayout->addWidget(m_deleteBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_connectBtn);
    btnLayout->addWidget(m_disconnectBtn);

    listLayout->addLayout(btnLayout);
    mainLayout->addWidget(listGroup, 1);

    // Settings group
    QGroupBox* settingsGroup = new QGroupBox(tr("Communication Settings"), this);
    QFormLayout* settingsLayout = new QFormLayout(settingsGroup);
    settingsLayout->setSpacing(10);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setObjectName("CommunicationNameEdit");
    m_nameEdit->setPlaceholderText("MyDevice");
    settingsLayout->addRow(tr("Name:"), m_nameEdit);

    m_typeCombo = new QComboBox(this);
    m_typeCombo->setObjectName("CommunicationTypeCombo");
    m_typeCombo->addItem("TCP Client", (int)CommunicationType::TCP_Client);
    m_typeCombo->addItem("TCP Server", (int)CommunicationType::TCP_Server);
    m_typeCombo->addItem("Serial Port", (int)CommunicationType::SerialPort);
    m_typeCombo->addItem("PLC (Modbus TCP)", (int)CommunicationType::PLC);
    settingsLayout->addRow(tr("Type:"), m_typeCombo);

    // TCP Settings
    m_ipEdit = new QLineEdit(this);
    m_ipEdit->setObjectName("CommunicationIpEdit");
    m_ipEdit->setPlaceholderText("192.168.1.100");
    settingsLayout->addRow(tr("IP Address:"), m_ipEdit);

    m_portSpin = new QSpinBox(this);
    m_portSpin->setObjectName("CommunicationPortSpin");
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(5000);
    settingsLayout->addRow(tr("Port:"), m_portSpin);

    // Serial Settings
    m_portNameCombo = new QComboBox(this);
    m_portNameCombo->setObjectName("CommunicationPortNameCombo");
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        m_portNameCombo->addItem(info.portName());
    }
    settingsLayout->addRow(tr("Port Name:"), m_portNameCombo);

    m_baudRateSpin = new QSpinBox(this);
    m_baudRateSpin->setObjectName("CommunicationBaudRateSpin");
    m_baudRateSpin->setRange(9600, 115200);
    m_baudRateSpin->setValue(115200);
    settingsLayout->addRow(tr("Baud Rate:"), m_baudRateSpin);

    m_parityCombo = new QComboBox(this);
    m_parityCombo->setObjectName("CommunicationParityCombo");
    m_parityCombo->addItem("None", 0);
    m_parityCombo->addItem("Even", 1);
    m_parityCombo->addItem("Odd", 2);
    settingsLayout->addRow(tr("Parity:"), m_parityCombo);

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
    connect(m_addBtn, &QPushButton::clicked, this, &CommunicationSetView::onAddClicked);
    connect(m_deleteBtn, &QPushButton::clicked, this, &CommunicationSetView::onDeleteClicked);
    connect(m_connectBtn, &QPushButton::clicked, this, &CommunicationSetView::onConnectClicked);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &CommunicationSetView::onDisconnectClicked);
    connect(m_applyBtn, &QPushButton::clicked, this, &CommunicationSetView::onApplyClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_commTable, &QTableWidget::itemSelectionChanged, this, &CommunicationSetView::onCommSelectionChanged);
    connect(m_typeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CommunicationSetView::onTypeChanged);

    onTypeChanged(m_typeCombo->currentIndex());
}

void CommunicationSetView::loadCommunications()
{
    m_commTable->setRowCount(0);

    const QList<CommunicationConfig>& configs = CommunicationManager::instance().configs();

    for (const CommunicationConfig& config : configs) {
        int row = m_commTable->rowCount();
        m_commTable->insertRow(row);

        QTableWidgetItem* nameItem = new QTableWidgetItem(config.name);
        nameItem->setData(Qt::UserRole, config.id);
        m_commTable->setItem(row, 0, nameItem);

        QString typeStr;
        switch (config.type) {
            case CommunicationType::TCP_Client: typeStr = "TCP Client"; break;
            case CommunicationType::TCP_Server: typeStr = "TCP Server"; break;
            case CommunicationType::SerialPort: typeStr = "Serial"; break;
            case CommunicationType::PLC: typeStr = "PLC"; break;
            default: typeStr = "Unknown";
        }
        m_commTable->setItem(row, 1, new QTableWidgetItem(typeStr));

        m_commTable->setItem(row, 2, new QTableWidgetItem(config.ipAddress));
        m_commTable->setItem(row, 3, new QTableWidgetItem(QString::number(config.port)));

        CommunicationState state = CommunicationManager::instance().state(config.id);
        QString stateStr = (state == CommunicationState::Connected) ? tr("Connected") :
                          (state == CommunicationState::Listening) ? tr("Listening") : tr("Disconnected");
        m_commTable->setItem(row, 4, new QTableWidgetItem(stateStr));
    }

    updateButtons();
}

void CommunicationSetView::updateButtons()
{
    bool hasSelection = m_commTable->currentRow() >= 0;
    m_deleteBtn->setEnabled(hasSelection);

    if (hasSelection) {
        int row = m_commTable->currentRow();
        QString status = m_commTable->item(row, 4)->text();
        bool isConnected = (status == tr("Connected") || status == tr("Listening"));
        m_connectBtn->setEnabled(!isConnected);
        m_disconnectBtn->setEnabled(isConnected);
    } else {
        m_connectBtn->setEnabled(false);
        m_disconnectBtn->setEnabled(false);
    }
}

void CommunicationSetView::onAddClicked()
{
    m_commTable->clearSelection();
    m_nameEdit->setText(tr("New Connection"));
    m_typeCombo->setCurrentIndex(m_typeCombo->findData(static_cast<int>(CommunicationType::TCP_Client)));
    m_ipEdit->setText("192.168.1.100");
    m_portSpin->setValue(5000);
    updateButtons();
}

void CommunicationSetView::onDeleteClicked()
{
    int row = m_commTable->currentRow();
    if (row < 0) return;

    const QString configId = m_commTable->item(row, 0)->data(Qt::UserRole).toString();
    CommunicationManager::instance().removeConfig(configId);
    loadCommunications();
    updateButtons();
}

void CommunicationSetView::onConnectClicked()
{
    int row = m_commTable->currentRow();
    if (row < 0) return;

    QString configId = m_commTable->item(row, 0)->data(Qt::UserRole).toString();
    CommunicationManager::instance().connect(configId);

    loadCommunications();
}

void CommunicationSetView::onDisconnectClicked()
{
    int row = m_commTable->currentRow();
    if (row < 0) return;

    QString configId = m_commTable->item(row, 0)->data(Qt::UserRole).toString();
    CommunicationManager::instance().disconnect(configId);

    loadCommunications();
}

void CommunicationSetView::onApplyClicked()
{
    CommunicationConfig config;
    int row = m_commTable->currentRow();
    if (row >= 0 && m_commTable->item(row, 0)) {
        config.id = m_commTable->item(row, 0)->data(Qt::UserRole).toString();
    }

    config.name = m_nameEdit->text().trimmed();
    if (config.name.isEmpty()) {
        config.name = tr("New Connection");
    }
    if (config.id.isEmpty()) {
        config.id = config.name;
    }

    config.type = static_cast<CommunicationType>(m_typeCombo->currentData().toInt());
    config.ipAddress = m_ipEdit->text().trimmed();
    config.port = m_portSpin->value();
    config.portName = m_portNameCombo->currentText();
    config.baudRate = m_baudRateSpin->value();
    config.parity = m_parityCombo->currentText();

    if (!CommunicationManager::instance().addOrUpdateConfig(config)) {
        Logger::instance().error(QString("Communication settings rejected: %1").arg(config.name), "Comm");
        return;
    }

    Logger::instance().info(QString("Communication settings applied: %1 - %2:%3")
                                .arg(config.name)
                                .arg(config.ipAddress)
                                .arg(config.port),
                            "Comm");
    loadCommunications();
    accept();
}

void CommunicationSetView::onCloseClicked()
{
    accept();
}

void CommunicationSetView::onCommSelectionChanged()
{
    int row = m_commTable->currentRow();
    if (row >= 0 && m_commTable->item(row, 0)) {
        const QString configId = m_commTable->item(row, 0)->data(Qt::UserRole).toString();
        if (CommunicationConfig* config = CommunicationManager::instance().findConfig(configId)) {
            m_nameEdit->setText(config->name);
            int typeIndex = m_typeCombo->findData(static_cast<int>(config->type));
            if (typeIndex >= 0) {
                m_typeCombo->setCurrentIndex(typeIndex);
            }
            m_ipEdit->setText(config->ipAddress);
            if (config->port > 0) {
                m_portSpin->setValue(config->port);
            }
            int portNameIndex = m_portNameCombo->findText(config->portName);
            if (portNameIndex >= 0) {
                m_portNameCombo->setCurrentIndex(portNameIndex);
            }
            m_baudRateSpin->setValue(config->baudRate);
            int parityIndex = m_parityCombo->findText(config->parity);
            if (parityIndex >= 0) {
                m_parityCombo->setCurrentIndex(parityIndex);
            }
        }
    }
    updateButtons();
}

void CommunicationSetView::onTypeChanged(int index)
{
    if (index < 0) {
        return;
    }

    const CommunicationType type = static_cast<CommunicationType>(m_typeCombo->itemData(index).toInt());
    const bool isSerial = (type == CommunicationType::SerialPort);
    const bool isNetwork =
        (type == CommunicationType::TCP_Client || type == CommunicationType::TCP_Server || type == CommunicationType::PLC);

    if (m_ipEdit) {
        m_ipEdit->setEnabled(isNetwork);
    }
    if (m_portSpin) {
        m_portSpin->setEnabled(isNetwork);
    }
    if (m_portNameCombo) {
        m_portNameCombo->setEnabled(isSerial);
    }
    if (m_baudRateSpin) {
        m_baudRateSpin->setEnabled(isSerial);
    }
    if (m_parityCombo) {
        m_parityCombo->setEnabled(isSerial);
    }
}

} // namespace DeepLux
