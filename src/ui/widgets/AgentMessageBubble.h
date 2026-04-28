#ifndef DEEPLUX_AGENT_MESSAGE_BUBBLE_H
#define DEEPLUX_AGENT_MESSAGE_BUBBLE_H

#include <QWidget>
#include <QLabel>

namespace DeepLux {

/**
 * @brief 单条聊天消息气泡
 */
class AgentMessageBubble : public QWidget
{
    Q_OBJECT

public:
    enum class Sender { User, Agent, System };

    explicit AgentMessageBubble(Sender sender, const QString& text, QWidget* parent = nullptr);

    void setText(const QString& text);
    QString text() const;

private:
    void setupUi();

    Sender m_sender;
    QLabel* m_label = nullptr;
    QString m_text;
};

} // namespace DeepLux

#endif // DEEPLUX_AGENT_MESSAGE_BUBBLE_H
