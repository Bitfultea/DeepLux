#include "AgentToolPreviewCard.h"
#include "AgentMessageBubble.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QJsonDocument>

namespace DeepLux {

AgentToolPreviewCard::AgentToolPreviewCard(const QList<ToolItem>& tools, bool isDark, QWidget* parent)
    : QWidget(parent)
    , m_tools(tools)
    , m_isDark(isDark)
{
    setupUi();
}

void AgentToolPreviewCard::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 4, 8, 4);
    mainLayout->setSpacing(4);

    ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();

    // 标题行
    auto* titleLayout = new QHBoxLayout();
    QLabel* title = new QLabel("🔧 Agent wants to execute:", this);
    title->setStyleSheet(QString("font-weight: bold; font-size: 11px; color: %1;").arg(theme.toolFg.name()));
    titleLayout->addWidget(title);
    titleLayout->addStretch();
    mainLayout->addLayout(titleLayout);

    for (int i = 0; i < m_tools.size(); ++i) {
        const ToolItem& t = m_tools[i];

        QLabel* nameLabel = new QLabel(QString("%1. %2").arg(i + 1).arg(t.name), this);
        nameLabel->setStyleSheet(QString("font-weight: bold; font-size: 12px; color: %1;").arg(theme.toolFg.name()));
        mainLayout->addWidget(nameLabel);

        QString paramsStr = QString(QJsonDocument(t.params).toJson(QJsonDocument::Indented));
        QLabel* paramsLabel = new QLabel(paramsStr, this);
        paramsLabel->setTextFormat(Qt::PlainText);
        paramsLabel->setWordWrap(true);
        paramsLabel->setStyleSheet(QString(
            "background-color: %1;"
            "color: %2;"
            "padding: 4px 6px;"
            "border-radius: 3px;"
            "font-family: Consolas, Monaco, 'Courier New', monospace;"
            "font-size: 11px;"
        ).arg(theme.codeBlockBg.name()).arg(theme.codeBlockFg.name()));

        m_paramWidgets.append(paramsLabel);
        mainLayout->addWidget(paramsLabel);
    }

    // Claude Code 终端风格提示行 — 无按钮，纯键盘交互
    QLabel* hint = new QLabel("▸ <b>Enter</b> to confirm  ·  <b>Esc</b> to cancel", this);
    hint->setStyleSheet(QString("font-size: 11px; color: %1; padding-top: 4px;").arg(theme.statusColor.name()));
    mainLayout->addWidget(hint);

    applyTheme(m_isDark);
}

void AgentToolPreviewCard::applyTheme(bool isDark)
{
    m_isDark = isDark;
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();

    setStyleSheet(QString(
        "AgentToolPreviewCard { border-left: 2px solid #f59e0b; }"
    ));

    for (QWidget* w : m_paramWidgets) {
        if (auto* lbl = qobject_cast<QLabel*>(w)) {
            lbl->setStyleSheet(QString(
                "background-color: %1;"
                "color: %2;"
                "padding: 4px 6px;"
                "border-radius: 3px;"
                "font-family: Consolas, Monaco, 'Courier New', monospace;"
                "font-size: 11px;"
            ).arg(theme.codeBlockBg.name()).arg(theme.codeBlockFg.name()));
        }
    }
}

} // namespace DeepLux
