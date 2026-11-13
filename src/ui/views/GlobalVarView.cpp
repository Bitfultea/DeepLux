#include "GlobalVarView.h"

#include "../widgets/AppIconProvider.h"
#include "common/Logger.h"
#include "core/common/UserManager.h"
#include "core/common/VarModel.h"
#include "core/manager/GlobalVarManager.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QSet>
#include <QVBoxLayout>
#include <limits>

namespace DeepLux {

GlobalVarView::GlobalVarView(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Global Variables"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setMinimumSize(700, 500);
    // 样式跟随主窗口主题（ThemeManager 统一管理），不再硬编码

    setupUI();
    connect(&UserManager::instance(), &UserManager::permissionChanged, this, &GlobalVarView::updatePermissionState);
    auto& variables = GlobalVarManager::instance();
    connect(&variables, &GlobalVarManager::variableAdded, this, [this](const QString&) { loadVariables(); });
    connect(&variables, &GlobalVarManager::variableRemoved, this, [this](const QString&) { loadVariables(); });
    connect(&variables, &GlobalVarManager::variableValueChanged, this,
            [this](const QString&, const QVariant&) { loadVariables(); });
    connect(&variables, &GlobalVarManager::allVariablesCleared, this, [this]() { loadVariables(); });
    updatePermissionState();
}

void GlobalVarView::setupUI() {
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(20, 20, 20, 20);

    // Variable list table
    QGroupBox* listGroup = new QGroupBox(tr("Variable List"), this);
    QVBoxLayout* listLayout = new QVBoxLayout(listGroup);

    m_varTable = new QTableWidget(this);
    m_varTable->setObjectName(QStringLiteral("GlobalVarTable"));
    m_varTable->setColumnCount(5);
    m_varTable->setHorizontalHeaderLabels({tr("Index"), tr("Name"), tr("Type"), tr("Value"), tr("Note")});
    m_varTable->horizontalHeader()->setStretchLastSection(true);
    m_varTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_varTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_varTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_varTable->setAlternatingRowColors(true);
    m_varTable->verticalHeader()->setVisible(false);
    listLayout->addWidget(m_varTable);

    // Buttons
    QHBoxLayout* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(10);

    m_addBtn = new QPushButton(tr("Add"), this);
    m_addBtn->setObjectName(QStringLiteral("GlobalVarAddButton"));
    m_addBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));
    m_deleteBtn = new QPushButton(tr("Delete"), this);
    m_deleteBtn->setObjectName(QStringLiteral("GlobalVarDeleteButton"));
    m_deleteBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Delete, 16, QColor("#DC2626")));
    m_deleteBtn->setEnabled(false);
    m_moveUpBtn = new QPushButton(tr("Up"), this);
    m_moveUpBtn->setObjectName(QStringLiteral("GlobalVarMoveUpButton"));
    m_moveUpBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::MoveUp, 16, QColor("#4B5563")));
    m_moveDownBtn = new QPushButton(tr("Down"), this);
    m_moveDownBtn->setObjectName(QStringLiteral("GlobalVarMoveDownButton"));
    m_moveDownBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::MoveDown, 16, QColor("#4B5563")));
    m_executeBtn = new QPushButton(tr("Execute"), this);
    m_executeBtn->setObjectName(QStringLiteral("GlobalVarExecuteButton"));
    m_executeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Execute, 16, QColor("#16A34A")));
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
    m_addGroup = new QGroupBox(tr("Add Variable"), this);
    QHBoxLayout* addLayout = new QHBoxLayout(m_addGroup);

    m_addTypeCombo = new QComboBox(this);
    m_addTypeCombo->setObjectName(QStringLiteral("GlobalVarAddTypeCombo"));
    m_addTypeCombo->addItem("int", QVariant::fromValue((int) VarDataType::Int));
    m_addTypeCombo->addItem("double", QVariant::fromValue((int) VarDataType::Double));
    m_addTypeCombo->addItem("string", QVariant::fromValue((int) VarDataType::String));
    m_addTypeCombo->addItem("bool", QVariant::fromValue((int) VarDataType::Bool));
    m_addTypeCombo->addItem("int[]", QVariant::fromValue((int) VarDataType::IntArray));
    m_addTypeCombo->addItem("double[]", QVariant::fromValue((int) VarDataType::DoubleArray));
    m_addTypeCombo->addItem("string[]", QVariant::fromValue((int) VarDataType::StringArray));
    m_addTypeCombo->addItem("bool[]", QVariant::fromValue((int) VarDataType::BoolArray));

    QPushButton* addIntBtn = new QPushButton(tr("Add Int"), this);
    addIntBtn->setObjectName(QStringLiteral("GlobalVarAddIntButton"));
    addIntBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));
    QPushButton* addDoubleBtn = new QPushButton(tr("Add Double"), this);
    addDoubleBtn->setObjectName(QStringLiteral("GlobalVarAddDoubleButton"));
    addDoubleBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));
    QPushButton* addStringBtn = new QPushButton(tr("Add String"), this);
    addStringBtn->setObjectName(QStringLiteral("GlobalVarAddStringButton"));
    addStringBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));
    QPushButton* addBoolBtn = new QPushButton(tr("Add Bool"), this);
    addBoolBtn->setObjectName(QStringLiteral("GlobalVarAddBoolButton"));
    addBoolBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Add, 16, QColor("#2563EB")));

    addLayout->addWidget(new QLabel(tr("Type:"), this));
    addLayout->addWidget(m_addTypeCombo);
    addLayout->addWidget(addIntBtn);
    addLayout->addWidget(addDoubleBtn);
    addLayout->addWidget(addStringBtn);
    addLayout->addWidget(addBoolBtn);
    addLayout->addStretch();

    mainLayout->addWidget(m_addGroup);

    // Bottom buttons
    QHBoxLayout* bottomLayout = new QHBoxLayout();
    bottomLayout->setSpacing(10);

    m_applyBtn = new QPushButton(tr("Apply"), this);
    m_applyBtn->setObjectName(QStringLiteral("GlobalVarApplyButton"));
    m_applyBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Confirm, 16, QColor("#16A34A")));
    m_applyBtn->setProperty("default", true);
    m_closeBtn = new QPushButton(tr("Close"), this);
    m_closeBtn->setObjectName(QStringLiteral("GlobalVarCloseButton"));
    m_closeBtn->setIcon(AppIconProvider::icon(AppIconProvider::Icon::Close, 16, QColor("#4B5563")));

    bottomLayout->addStretch();
    bottomLayout->addWidget(m_applyBtn);
    bottomLayout->addWidget(m_closeBtn);

    mainLayout->addLayout(bottomLayout);

    // Connections
    connect(m_addBtn, &QPushButton::clicked, this, &GlobalVarView::onAddClicked);
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

void GlobalVarView::loadVariables() {
    if (!hasGlobalVarPermission())
        return;
    m_varTable->setRowCount(0);

    const QList<GlobalVariableSnapshot> vars = GlobalVarManager::instance().userVariableSnapshots();

    for (const GlobalVariableSnapshot& var : vars) {
        int row = m_varTable->rowCount();
        m_varTable->insertRow(row);

        m_varTable->setItem(row, 0, new QTableWidgetItem(QString::number(var.index)));
        m_varTable->setItem(row, 1, new QTableWidgetItem(var.name));
        m_varTable->setItem(row, 2, new QTableWidgetItem(VarModel::dataTypeToString(var.type)));
        m_varTable->setItem(row, 3, new QTableWidgetItem(var.value.toString()));
        m_varTable->setItem(row, 4, new QTableWidgetItem(var.note));
    }

    updateButtons();
}

bool GlobalVarView::hasGlobalVarPermission() const {
    const UserManager& users = UserManager::instance();
    return users.isLoggedIn() && users.currentUser().type != UserType::Guest &&
           users.hasPermission(QStringLiteral("GlobalVar"));
}

void GlobalVarView::updatePermissionState() {
    const bool allowed = hasGlobalVarPermission();
    if (m_permissionInitialized && allowed == m_permissionGranted)
        return;
    const bool initialState = !m_permissionInitialized;
    m_permissionInitialized = true;
    m_permissionGranted = allowed;

    m_varTable->setEnabled(allowed);
    m_addBtn->setEnabled(allowed);
    m_addGroup->setEnabled(allowed);
    m_addTypeCombo->setEnabled(allowed);
    m_applyBtn->setEnabled(allowed);
    if (!allowed) {
        m_varTable->setRowCount(0);
        m_addTypeCombo->setCurrentIndex(-1);
    } else if (m_addTypeCombo->currentIndex() < 0) {
        m_addTypeCombo->setCurrentIndex(0);
    }
    updateButtons();
    if (allowed && initialState)
        loadVariables();
}

bool GlobalVarView::refreshVariablesIfAuthorized() {
    if (!hasGlobalVarPermission())
        return false;
    if (m_addTypeCombo->currentIndex() < 0)
        m_addTypeCombo->setCurrentIndex(0);
    loadVariables();
    return true;
}

void GlobalVarView::updateButtons() {
    if (!m_permissionGranted) {
        m_deleteBtn->setEnabled(false);
        m_moveUpBtn->setEnabled(false);
        m_moveDownBtn->setEnabled(false);
        m_executeBtn->setEnabled(false);
        return;
    }
    bool hasSelection = m_varTable->currentRow() >= 0;
    m_deleteBtn->setEnabled(hasSelection);
    m_executeBtn->setEnabled(hasSelection);
    m_moveUpBtn->setEnabled(hasSelection && m_varTable->currentRow() > 0);
    m_moveDownBtn->setEnabled(hasSelection && m_varTable->currentRow() < m_varTable->rowCount() - 1);
}

void GlobalVarView::onAddClicked() {
    if (!hasGlobalVarPermission())
        return;
    int typeIndex = m_addTypeCombo->currentData().toInt();
    VarDataType type = (VarDataType) typeIndex;
    QSet<QString> existingNames;
    for (const GlobalVariableSnapshot& variable : GlobalVarManager::instance().userVariableSnapshots())
        existingNames.insert(variable.name);
    QString name;
    for (int suffix = 1; suffix < std::numeric_limits<int>::max(); ++suffix) {
        const QString candidate = QStringLiteral("newVar%1").arg(suffix);
        if (!existingNames.contains(candidate)) {
            name = candidate;
            break;
        }
    }
    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Variable"), tr("No available automatic variable name was found."));
        return;
    }
    QVariant value;
    switch (type) {
    case VarDataType::Int:
        value = 0;
        break;
    case VarDataType::Double:
        value = 0.0;
        break;
    case VarDataType::String:
        value = QString();
        break;
    case VarDataType::Bool:
        value = false;
        break;
    case VarDataType::IntArray:
    case VarDataType::DoubleArray:
    case VarDataType::StringArray:
    case VarDataType::BoolArray:
        value = QVariantList();
        break;
    default:
        QMessageBox::warning(this, tr("Variable"), tr("The selected variable type is not supported."));
        return;
    }
    QString errorCode;
    GlobalVariableSnapshot created;
    if (!GlobalVarManager::instance().tryCreateUserVariable(name, type, value, {}, &created, &errorCode)) {
        QMessageBox::warning(this, tr("Variable"), tr("Variable creation failed: %1").arg(errorCode));
        return;
    }
    Logger::instance().info(QString("Variable added: %1").arg(created.name), "Var");
}

void GlobalVarView::onDeleteClicked() {
    if (!hasGlobalVarPermission())
        return;
    int row = m_varTable->currentRow();
    if (row < 0)
        return;

    QString name = m_varTable->item(row, 1)->text();
    QString errorCode;
    if (!GlobalVarManager::instance().tryRemoveUserVariable(name, {}, &errorCode)) {
        QMessageBox::warning(this, tr("Variable"), tr("Variable removal failed: %1").arg(errorCode));
        return;
    }

    Logger::instance().info(QString("Variable deleted: %1").arg(name), "Var");
}

void GlobalVarView::onMoveUpClicked() {
    if (!hasGlobalVarPermission())
        return;
    int row = m_varTable->currentRow();
    if (row <= 0)
        return;

    // Swap rows
    for (int col = 0; col < m_varTable->columnCount(); ++col) {
        QTableWidgetItem* current = m_varTable->takeItem(row, col);
        QTableWidgetItem* above = m_varTable->takeItem(row - 1, col);
        m_varTable->setItem(row - 1, col, current);
        m_varTable->setItem(row, col, above);
    }

    m_varTable->selectRow(row - 1);
}

void GlobalVarView::onMoveDownClicked() {
    if (!hasGlobalVarPermission())
        return;
    int row = m_varTable->currentRow();
    if (row < 0 || row >= m_varTable->rowCount() - 1)
        return;

    // Swap rows
    for (int col = 0; col < m_varTable->columnCount(); ++col) {
        QTableWidgetItem* current = m_varTable->takeItem(row, col);
        QTableWidgetItem* below = m_varTable->takeItem(row + 1, col);
        m_varTable->setItem(row + 1, col, current);
        m_varTable->setItem(row, col, below);
    }

    m_varTable->selectRow(row + 1);
}

void GlobalVarView::onExecuteClicked() {
    if (!hasGlobalVarPermission())
        return;
    int row = m_varTable->currentRow();
    if (row < 0)
        return;

    QString name = m_varTable->item(row, 1)->text();
    GlobalVarManager::instance().evaluateVariable(name);
    loadVariables();
}

void GlobalVarView::onApplyClicked() {
    if (!hasGlobalVarPermission())
        return;
    Logger::instance().info("Global variables applied", "Var");
    accept();
}

void GlobalVarView::onCloseClicked() {
    accept();
}

void GlobalVarView::onSelectionChanged() {
    updateButtons();
}

void GlobalVarView::onAddVarTypeChanged(const QString& type) {
    Q_UNUSED(type);
}

} // namespace DeepLux
