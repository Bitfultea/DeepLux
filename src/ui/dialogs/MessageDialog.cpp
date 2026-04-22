#include "MessageDialog.h"
#include "common/Logger.h"
#include <QApplication>
#include <QMessageBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QDialogButtonBox>

namespace DeepLux {

MessageDialog* MessageDialog::s_instance = nullptr;

MessageDialog& MessageDialog::instance()
{
    if (!s_instance) {
        s_instance = new MessageDialog();
    }
    return *s_instance;
}

MessageDialog::MessageDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setWindowModality(Qt::WindowModal);
}

void MessageDialog::messageBoxShow(const QString& message, MsgType type)
{
    messageBoxShow(message, type, QMessageBox::Ok, true);
}

MsgType MessageDialog::messageBoxShow(
    const QString& message,
    MsgType type,
    QMessageBox::StandardButtons buttons,
    bool modal)
{
    if (modal) {
        setWindowModality(Qt::ApplicationModal);
    } else {
        setWindowModality(Qt::NonModal);
    }

    QMessageBox msgBox(this);
    msgBox.setIcon(getQMessageBoxIcon(type));
    msgBox.setText(message);
    msgBox.setStandardButtons(buttons);

    int ret = msgBox.exec();

    if (ret == QMessageBox::Yes || ret == QMessageBox::Ok) {
        return MsgType::Info;
    } else if (ret == QMessageBox::No) {
        return MsgType::Question;
    }

    return type;
}

int MessageDialog::information(const QString& message, bool modal)
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);

    if (!modal) {
        msgBox.setWindowModality(Qt::NonModal);
    }

    Logger::instance().info(message, "Dialog");
    return msgBox.exec();
}

int MessageDialog::warning(const QString& message, bool modal)
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Warning);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);

    if (!modal) {
        msgBox.setWindowModality(Qt::NonModal);
    }

    Logger::instance().warning(message, "Dialog");
    return msgBox.exec();
}

int MessageDialog::error(const QString& message, bool modal)
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Critical);
    msgBox.setText(message);
    msgBox.setStandardButtons(QMessageBox::Ok);

    if (!modal) {
        msgBox.setWindowModality(Qt::NonModal);
    }

    Logger::instance().error(message, "Dialog");
    return msgBox.exec();
}

int MessageDialog::question(const QString& message, QMessageBox::StandardButtons buttons)
{
    QMessageBox msgBox;
    msgBox.setIcon(QMessageBox::Question);
    msgBox.setText(message);
    msgBox.setStandardButtons(buttons);

    Logger::instance().info(message, "Dialog");
    return msgBox.exec();
}

QString MessageDialog::getMsgTypeIcon(MsgType type)
{
    switch (type) {
        case MsgType::Info: return "ℹ️";
        case MsgType::Warning: return "⚠️";
        case MsgType::Error: return "❌";
        case MsgType::Question: return "❓";
        default: return "";
    }
}

QMessageBox::Icon MessageDialog::getQMessageBoxIcon(MsgType type)
{
    switch (type) {
        case MsgType::Info: return QMessageBox::Information;
        case MsgType::Warning: return QMessageBox::Warning;
        case MsgType::Error: return QMessageBox::Critical;
        case MsgType::Question: return QMessageBox::Question;
        default: return QMessageBox::NoIcon;
    }
}

} // namespace DeepLux