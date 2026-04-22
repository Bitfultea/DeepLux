#include "GlobalVarView.h"
#include "core/manager/GlobalVarManager.h"
#include "core/common/VarModel.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QGroupBox>
#include <QLabel>

namespace DeepLux {

GlobalVarView::GlobalVarView(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Global Variables"));
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
        QComboBox {
            padding: 8px;
            border: 1px solid #555;
            border-radius: 4px;
            background-color: #3a3a3a;
            color: #ffffff;
        }
        QLineEdit {
            padding: 8px;
            border: 1px solid #555;
            border-radius: 4px;
            background-color: #3a3a3a;
            color: #ffffff;
        }
        QLineEdit:focus {
            border: 1px solid #0078d4;
        }
    )");

    setupUI();
    loadVariables();
}

void GlobalVarView::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Variable list table
    QGroupBox* listGroup = new QGroupBox(tr("Variable List"), this);
    QVBoxLayout* listLayout = new QVBoxLayout(listGroup);

    m_varTable = new QTableWidget(this);
    m_varTable->setColumnCount(5);
    m_varTable->setHorizontalHeaderLabels({tr("Index"), tr("Name"), tr("Type"), tr("Value"), tr("Note")});
    m_varTable->horizontalHeader()->setStretchLastSection(true);
    m_varTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_varTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_varTable->setAlternatingRowColors(true);
    m_varTable->verticalHeader()->setVisible(false);
    listLayout->addWidget(m_varTable);

    // Buttons
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    m_addBtn = new QPushButton(tr("➕ Add"), this);
    m_deleteBtn = new QPushButton(tr("🗑 Delete"), this);
    m_deleteBtn->setEnabled(false);
    m_moveUpBtn = new QPushButton(tr("⬆ Up"), this);
    m_moveDownBtn = new QPushButton(tr("⬇ Down"), this);
    m_executeBtn = new QPushButton(tr("▶ Execute"), this);
    m_executeBtn->setEnabled(false);

    btnLayout->addWidget(m_addBtn);
    btnLayout->addWidget(m_deleteBtn);
    btnLayout->addSpacing(20);
    btnLayout->addWidget(m_moveUpBtn);
    btnLayout->addWidget(m_moveDownBtn);
    btnLayout->addSpacing(20);
    btnLayout->addWidget(m_executeBtn);
    btnLayout->addStretch();

    listLayout->addLayout(btnLayout);
    mainLayout->addWidget(listGroup, 1);

    // Add variable section
    QGroupBox* addGroup = new QGroupBox(tr("Add Variable"), this);
    QHBoxLayout* addLayout = new QHBoxLayout(addGroup);

    m_addTypeCombo = new QComboBox(this);
    m_addTypeCombo->addItem("int", QVariant::fromValue((int)VarDataType::Int));
    m_addTypeCombo->addItem("double", QVariant::fromValue((int)VarDataType::Double));
    m_addTypeCombo->addItem("string", QVariant::fromValue((int)VarDataType::String));
    m_addTypeCombo->addItem("bool", QVariant::fromValue((int)VarDataType::Bool));
    m_addTypeCombo->addItem("int[]", QVariant::fromValue((int)VarDataType::IntArray));
    m_addTypeCombo->addItem("double[]", QVariant::fromValue((int)VarDataType::DoubleArray));
    m_addTypeCombo->addItem("string[]", QVariant::fromValue((int)VarDataType::StringArray));
    m_addTypeCombo->addItem("bool[]", QVariant::fromValue((int)VarDataType::BoolArray));

    QPushButton* addIntBtn = new QPushButton(tr("Add Int"), this);
    QPushButton* addDoubleBtn = new QPushButton(tr("Add Double"), this);
    QPushButton* addStringBtn = new QPushButton(tr("Add String"), this);
    QPushButton* addBoolBtn = new QPushButton(tr("Add Bool"), this);

    addLayout->addWidget(new QLabel(tr("Type:"), this));
    addLayout->addWidget(m_addTypeCombo);
    addLayout->addWidget(addIntBtn);
    addLayout->addWidget(addDoubleBtn);
    addLayout->addWidget(addStringBtn);
    addLayout->addWidget(addBoolBtn);
    addLayout->addStretch();

    mainLayout->addWidget(addGroup);

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
    connect(addIntBtn, &QPushButton::clicked, [this]() { onAddClicked(); });
    connect(addDoubleBtn, &QPushButton::clicked, [this]() { onAddClicked(); });
    connect(addStringBtn, &QPushButton::clicked, [this]() { onAddClicked(); });
    connect(addBoolBtn, &QPushButton::clicked, [this]() { onAddClicked(); });
    connect(m_deleteBtn, &QPushButton::clicked, this, &GlobalVarView::onDeleteClicked);
    connect(m_moveUpBtn, &QPushButton::clicked, this, &GlobalVarView::onMoveUpClicked);
    connect(m_moveDownBtn, &QPushButton::clicked, this, &GlobalVarView::onMoveDownClicked);
    connect(m_executeBtn, &QPushButton::clicked, this, &GlobalVarView::onExecuteClicked);
    connect(m_applyBtn, &QPushButton::clicked, this, &GlobalVarView::onApplyClicked);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_varTable, &QTableWidget::itemSelectionChanged, this, &GlobalVarView::onSelectionChanged);
}

void GlobalVarView::loadVariables()
{
    m_varTable->setRowCount(0);

    QList<VarModel*> vars = GlobalVarManager::instance().getAllVariables();

    for (VarModel* var : vars) {
        int row = m_varTable->rowCount();
        m_varTable->insertRow(row);

        m_varTable->setItem(row, 0, new QTableWidgetItem(QString::number(var->index())));
        m_varTable->setItem(row, 1, new QTableWidgetItem(var->name()));
        m_varTable->setItem(row, 2, new QTableWidgetItem(VarModel::dataTypeToString(var->dataType())));
        m_varTable->setItem(row, 3, new QTableWidgetItem(var->value().toString()));
        m_varTable->setItem(row, 4, new QTableWidgetItem(var->note()));
    }

    updateButtons();
}

void GlobalVarView::updateButtons()
{
    bool hasSelection = m_varTable->currentRow() >= 0;
    m_deleteBtn->setEnabled(hasSelection);
    m_executeBtn->setEnabled(hasSelection);
    m_moveUpBtn->setEnabled(hasSelection && m_varTable->currentRow() > 0);
    m_moveDownBtn->setEnabled(hasSelection && m_varTable->currentRow() < m_varTable->rowCount() - 1);
}

void GlobalVarView::onAddClicked()
{
    int typeIndex = m_addTypeCombo->currentData().toInt();
    VarDataType type = (VarDataType)typeIndex;

    VarModel* var = new VarModel();
    var->setName(QString("newVar%1").arg(GlobalVarManager::instance().count() + 1));
    var->setDataType(type);
    var->setValue(QVariant(0));

    GlobalVarManager::instance().addVariable(var);
    loadVariables();

    Logger::instance().info(QString("Variable added: %1").arg(var->name()), "Var");
}

void GlobalVarView::onDeleteClicked()
{
    int row = m_varTable->currentRow();
    if (row < 0) return;

    QString name = m_varTable->item(row, 1)->text();
    GlobalVarManager::instance().removeVariable(name);
    loadVariables();

    Logger::instance().info(QString("Variable deleted: %1").arg(name), "Var");
}

void GlobalVarView::onMoveUpClicked()
{
    int row = m_varTable->currentRow();
    if (row <= 0) return;

    // Swap rows
    for (int col = 0; col < m_varTable->columnCount(); ++col) {
        QTableWidgetItem* current = m_varTable->takeItem(row, col);
        QTableWidgetItem* above = m_varTable->takeItem(row - 1, col);
        m_varTable->setItem(row - 1, col, current);
        m_varTable->setItem(row, col, above);
    }

    m_varTable->selectRow(row - 1);
}

void GlobalVarView::onMoveDownClicked()
{
    int row = m_varTable->currentRow();
    if (row < 0 || row >= m_varTable->rowCount() - 1) return;

    // Swap rows
    for (int col = 0; col < m_varTable->columnCount(); ++col) {
        QTableWidgetItem* current = m_varTable->takeItem(row, col);
        QTableWidgetItem* below = m_varTable->takeItem(row + 1, col);
        m_varTable->setItem(row + 1, col, current);
        m_varTable->setItem(row, col, below);
    }

    m_varTable->selectRow(row + 1);
}

void GlobalVarView::onExecuteClicked()
{
    int row = m_varTable->currentRow();
    if (row < 0) return;

    QString name = m_varTable->item(row, 1)->text();
    GlobalVarManager::instance().evaluateVariable(name);
    loadVariables();
}

void GlobalVarView::onApplyClicked()
{
    Logger::instance().info("Global variables applied", "Var");
    accept();
}

void GlobalVarView::onCloseClicked()
{
    accept();
}

void GlobalVarView::onSelectionChanged()
{
    updateButtons();
}

void GlobalVarView::onAddVarTypeChanged(const QString& type)
{
    Q_UNUSED(type);
}

} // namespace DeepLux