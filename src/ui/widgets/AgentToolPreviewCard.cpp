#include "AgentToolPreviewCard.h"
#include "AgentMessageBubble.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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
    mainLayout->setContentsMargins(8, 2, 8, 2);
    mainLayout->setSpacing(3);

    ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();

    m_titleLabel = new QLabel("🔧 Agent wants to execute:", this);
    m_titleLabel->setStyleSheet(QString("font-weight: bold; font-size: 11px; color: %1;").arg(theme.toolFg.name()));
    mainLayout->addWidget(m_titleLabel);

    for (int i = 0; i < m_tools.size(); ++i) {
        const ToolItem& t = m_tools[i];
        QLabel* nameLabel = new QLabel(QString("%1. %2").arg(i + 1).arg(t.name), this);
        nameLabel->setStyleSheet(QString("font-weight: bold; font-size: 12px; color: %1;").arg(theme.toolFg.name()));
        m_nameLabels.append(nameLabel);
        mainLayout->addWidget(nameLabel);

        QString paramsStr = QString(QJsonDocument(t.params).toJson(QJsonDocument::Indented));
        QLabel* paramsLabel = new QLabel(paramsStr, this);
        paramsLabel->setTextFormat(Qt::PlainText);
        paramsLabel->setWordWrap(true);
        paramsLabel->setStyleSheet(QString(
            "background-color: %1; color: %2; padding: 3px 6px; border-radius: 3px;"
            "font-family: Consolas, Monaco, 'Courier New', monospace; font-size: 11px;"
        ).arg(theme.codeBlockBg.name()).arg(theme.codeBlockFg.name()));
        m_paramWidgets.append(paramsLabel);
        mainLayout->addWidget(paramsLabel);
    }

    // 简洁确认/取消 — 扁平文字按钮
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(4);
    btnLayout->addStretch();

    m_cancelBtn = new QPushButton("Cancel", this);
    m_cancelBtn->setFixedHeight(24);
    m_cancelBtn->setStyleSheet(QString(
        "QPushButton { background: transparent; color: %1; border: 1px solid %2; "
        "border-radius: 3px; padding: 0 8px; font-size: 11px; }"
        "QPushButton:hover { background: %3; }"
    ).arg(theme.errorColor.name()).arg(theme.errorColor.name())
     .arg(theme.inputBorder.name()));

    m_confirmBtn = new QPushButton("Confirm", this);
    m_confirmBtn->setFixedHeight(24);
    m_confirmBtn->setStyleSheet(QString(
        "QPushButton { background: transparent; color: %1; border: 1px solid %1; "
        "border-radius: 3px; padding: 0 8px; font-size: 11px; }"
        "QPushButton:hover { background: %2; }"
    ).arg(theme.agentName.name()).arg(theme.inputBorder.name()));

    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_confirmBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_confirmBtn, &QPushButton::clicked, this, &AgentToolPreviewCard::confirmed);
    connect(m_cancelBtn, &QPushButton::clicked, this, &AgentToolPreviewCard::cancelled);

    applyTheme(m_isDark);
}

void AgentToolPreviewCard::applyTheme(bool isDark)
{
    m_isDark = isDark;
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();

    setStyleSheet(QString(
        "AgentToolPreviewCard { border-left: 2px solid #f59e0b; }"
    ));

    if (m_titleLabel) {
        m_titleLabel->setStyleSheet(QString("font-weight: bold; font-size: 11px; color: %1;").arg(theme.toolFg.name()));
    }
    for (QLabel* lbl : m_nameLabels) {
        if (lbl) lbl->setStyleSheet(QString("font-weight: bold; font-size: 12px; color: %1;").arg(theme.toolFg.name()));
    }
    for (QWidget* w : m_paramWidgets) {
        if (auto* lbl = qobject_cast<QLabel*>(w)) {
            lbl->setStyleSheet(QString(
                "background-color: %1; color: %2; padding: 3px 6px; border-radius: 3px;"
                "font-family: Consolas, Monaco, 'Courier New', monospace; font-size: 11px;"
            ).arg(theme.codeBlockBg.name()).arg(theme.codeBlockFg.name()));
        }
    }
    if (m_cancelBtn) {
        m_cancelBtn->setStyleSheet(QString(
            "QPushButton { background: transparent; color: %1; border: 1px solid %2; "
            "border-radius: 3px; padding: 0 8px; font-size: 11px; }"
            "QPushButton:hover { background: %3; }"
        ).arg(theme.errorColor.name()).arg(theme.errorColor.name())
         .arg(theme.inputBorder.name()));
    }
    if (m_confirmBtn) {
        m_confirmBtn->setStyleSheet(QString(
            "QPushButton { background: transparent; color: %1; border: 1px solid %1; "
            "border-radius: 3px; padding: 0 8px; font-size: 11px; }"
            "QPushButton:hover { background: %2; }"
        ).arg(theme.agentName.name()).arg(theme.inputBorder.name()));
    }
}

} // namespace DeepLux
