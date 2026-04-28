#include "AgentMessageBubble.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextBrowser>
#include <QPainter>

namespace DeepLux {

AgentMessageBubble::AgentMessageBubble(Sender sender, const QString& text, QWidget* parent)
    : QWidget(parent)
    , m_sender(sender)
    , m_text(text)
{
    setupUi();
}

void AgentMessageBubble::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);

    m_label = new QLabel(this);
    m_label->setWordWrap(true);
    m_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_label->setText(m_text);

    QString bg;
    QString fg = "#000000";
    switch (m_sender) {
    case Sender::User:
        bg = "#DCF8C6"; // green-ish
        layout->addStretch();
        layout->addWidget(m_label);
        break;
    case Sender::Agent:
        bg = "#FFFFFF";
        layout->addWidget(m_label);
        layout->addStretch();
        break;
    case Sender::System:
        bg = "#E8E8E8";
        fg = "#666666";
        layout->addStretch();
        layout->addWidget(m_label);
        layout->addStretch();
        break;
    }

    m_label->setStyleSheet(QString("background-color: %1; color: %2; padding: 8px; border-radius: 8px;").arg(bg).arg(fg));
    m_label->setMaximumWidth(600);
}

void AgentMessageBubble::setText(const QString& text)
{
    m_text = text;
    if (m_label) m_label->setText(text);
}

QString AgentMessageBubble::text() const
{
    return m_text;
}

} // namespace DeepLux
