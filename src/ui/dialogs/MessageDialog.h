#pragma once

#include <QDialog>
#include <QMessageBox>
#include "../../core/platform/Platform.h"

namespace DeepLux {

enum class MsgType {
    Info,
    Warning,
    Error,
    Question
};

class MessageDialog : public QDialog
{
    Q_OBJECT

public:
    static MessageDialog& instance();

    void messageBoxShow(const QString& message, MsgType type = MsgType::Info);
    MsgType messageBoxShow(
        const QString& message,
        MsgType type,
        QMessageBox::StandardButtons buttons,
        bool modal = true
    );

    Q_INVOKABLE int information(const QString& message, bool modal = true);
    Q_INVOKABLE int warning(const QString& message, bool modal = true);
    Q_INVOKABLE int error(const QString& message, bool modal = true);
    Q_INVOKABLE int question(const QString& message, QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No);

private:
    explicit MessageDialog(QWidget* parent = nullptr);
    DISABLE_COPY(MessageDialog)

    QString getMsgTypeIcon(MsgType type);
    QMessageBox::Icon getQMessageBoxIcon(MsgType type);

    static MessageDialog* s_instance;
};

} // namespace DeepLux