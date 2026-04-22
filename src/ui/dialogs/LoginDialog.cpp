#include "LoginDialog.h"
#include "core/common/UserManager.h"
#include "common/Logger.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QDialogButtonBox>

namespace DeepLux {

LoginDialog::LoginDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Login"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setFixedSize(360, 220);
    setStyleSheet(R"(
        QDialog {
            background-color: #2b2b2b;
            color: #ffffff;
        }
        QLabel {
            color: #ffffff;
            font-size: 14px;
        }
        QLineEdit {
            padding: 8px;
            border: 1px solid #555;
            border-radius: 4px;
            background-color: #3a3a3a;
            color: #ffffff;
            font-size: 14px;
        }
        QLineEdit:focus {
            border: 1px solid #0078d4;
        }
        QComboBox {
            padding: 8px;
            border: 1px solid #555;
            border-radius: 4px;
            background-color: #3a3a3a;
            color: #ffffff;
            font-size: 14px;
        }
        QPushButton {
            padding: 10px 20px;
            border: none;
            border-radius: 4px;
            font-size: 14px;
            font-weight: bold;
        }
        QPushButton[default="true"] {
            background-color: #0078d4;
            color: #ffffff;
        }
        QPushButton[default="true"]:hover {
            background-color: #1084d8;
        }
        QPushButton[default="false"] {
            background-color: #555;
            color: #ffffff;
        }
        QPushButton[default="false"]:hover {
            background-color: #666;
        }
    )");

    setupUI();
}

void LoginDialog::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(20);
    mainLayout->setContentsMargins(30, 30, 30, 20);

    // Title
    QLabel* titleLabel = new QLabel(tr("User Login"), this);
    titleLabel->setStyleSheet("font-size: 20px; font-weight: bold; color: #0078d4;");
    titleLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(titleLabel);

    // Form
    QFormLayout* formLayout = new QFormLayout();
    formLayout->setSpacing(15);

    m_userNameEdit = new QLineEdit(this);
    m_userNameEdit->setPlaceholderText(tr("Enter username"));
    m_userNameEdit->setText("Operator");
    formLayout->addRow(tr("Username:"), m_userNameEdit);

    m_passWordEdit = new QLineEdit(this);
    m_passWordEdit->setPlaceholderText(tr("Enter password"));
    m_passWordEdit->setEchoMode(QLineEdit::Password);
    formLayout->addRow(tr("Password:"), m_passWordEdit);

    m_userTypeCombo = new QComboBox(this);
    m_userTypeCombo->addItem(tr("Operator"), QVariant::fromValue((int)UserType::Operator));
    m_userTypeCombo->addItem(tr("Administrator"), QVariant::fromValue((int)UserType::Administrator));
    m_userTypeCombo->addItem(tr("Developer"), QVariant::fromValue((int)UserType::Developer));
    m_userTypeCombo->addItem(tr("Guest"), QVariant::fromValue((int)UserType::Guest));
    formLayout->addRow(tr("User Type:"), m_userTypeCombo);

    mainLayout->addLayout(formLayout);

    // Buttons
    QDialogButtonBox* buttonBox = new QDialogButtonBox(this);
    buttonBox->setStyleSheet("QDialogButtonBox { spacing: 10px; }");

    m_loginBtn = new QPushButton(tr("Login"), this);
    m_loginBtn->setProperty("default", true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    m_cancelBtn->setProperty("default", false);

    buttonBox->addButton(m_loginBtn, QDialogButtonBox::AcceptRole);
    buttonBox->addButton(m_cancelBtn, QDialogButtonBox::RejectRole);

    mainLayout->addWidget(buttonBox);

    connect(m_loginBtn, &QPushButton::clicked, this, &LoginDialog::onLoginClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &LoginDialog::onCancelClicked);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    m_userNameEdit->setFocus();
}

void LoginDialog::onLoginClicked()
{
    m_userName = m_userNameEdit->text().trimmed();
    m_passWord = m_passWordEdit->text();

    if (m_userName.isEmpty()) {
        m_userNameEdit->setFocus();
        return;
    }

    // Emit login request
    emit loginRequested(m_userName, m_passWord);

    // Perform login via UserManager
    UserManager::instance().login(m_userName, m_passWord);

    Logger::instance().info(QString("User logged in: %1").arg(m_userName), "Auth");
    accept();
}

void LoginDialog::onCancelClicked()
{
    reject();
}

} // namespace DeepLux