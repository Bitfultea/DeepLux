#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>

namespace DeepLux {

class LoginDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LoginDialog(QWidget* parent = nullptr);
    ~LoginDialog() override = default;

    QString userName() const { return m_userName; }
    QString passWord() const { return m_passWord; }

signals:
    void loginRequested(const QString& userName, const QString& passWord);

private slots:
    void onLoginClicked();
    void onCancelClicked();

private:
    void setupUI();

    QLineEdit* m_userNameEdit = nullptr;
    QLineEdit* m_passWordEdit = nullptr;
    QComboBox* m_userTypeCombo = nullptr;
    QPushButton* m_loginBtn = nullptr;
    QPushButton* m_cancelBtn = nullptr;

    QString m_userName;
    QString m_passWord;
};

} // namespace DeepLux