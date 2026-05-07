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
    mainLayout->setSpacing(2);

    ChatTheme theme = m_isDark ? ChatTheme::dark() : ChatTheme::light();

    // 标题 + 工具列表（紧凑内联）
    QStringList lines;
    lines.append(QString("<span style=\"font-weight:bold;color:%1;\">🔧 Agent wants to execute:</span>")
                 .arg(theme.toolFg.name()));

    for (int i = 0; i < m_tools.size(); ++i) {
        const ToolItem& t = m_tools[i];
        QString line = QString("<span style=\"color:%1;\">  %2. %3</span>")
            .arg(theme.toolFg.name())
            .arg(i + 1)
            .arg(t.name);

        // 内联参数: key: "value" 格式, 紧凑单行
        if (!t.params.isEmpty()) {
            QStringList paramParts;
            for (auto it = t.params.begin(); it != t.params.end(); ++it) {
                QJsonValue val = it.value();
                QString valStr;
                if (val.isString()) valStr = QString("\"%1\"").arg(val.toString());
                else valStr = QString(QJsonDocument(QJsonObject{{it.key(), val}})
                    .toJson(QJsonDocument::Compact)).mid(1 + it.key().length() + 3).chopped(1);
                paramParts.append(QString("%1: %2").arg(it.key()).arg(valStr));
            }
            line = QString("<span style=\"color:%1;\">  %2. %3(</span>"
                           "<span style=\"color:%4;\">%5</span>"
                           "<span style=\"color:%1;\">)</span>")
                .arg(theme.toolFg.name())
                .arg(i + 1)
                .arg(t.name)
                .arg(theme.codeBlockFg.name())
                .arg(paramParts.join(", "));
        }
        lines.append(line);
    }

    QLabel* content = new QLabel(lines.join("<br>"), this);
    content->setTextFormat(Qt::RichText);
    content->setWordWrap(true);
    m_contentLabel = content;
    mainLayout->addWidget(content);

    // 确认/取消按钮
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(4);
    btnLayout->addStretch();

    m_cancelBtn = new QPushButton("Cancel", this);
    m_cancelBtn->setFixedHeight(22);
    m_confirmBtn = new QPushButton("Confirm", this);
    m_confirmBtn->setFixedHeight(22);

    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_confirmBtn);
    mainLayout->addLayout(btnLayout);

    connect(m_confirmBtn, &QPushButton::clicked, this, [this]() {
        m_confirmBtn->setEnabled(false);
        m_cancelBtn->setEnabled(false);
        emit confirmed();
    });
    connect(m_cancelBtn, &QPushButton::clicked, this, [this]() {
        m_confirmBtn->setEnabled(false);
        m_cancelBtn->setEnabled(false);
        emit cancelled();
    });

    applyTheme(m_isDark);
}

void AgentToolPreviewCard::applyTheme(bool isDark)
{
    m_isDark = isDark;
    ChatTheme theme = isDark ? ChatTheme::dark() : ChatTheme::light();

    setStyleSheet(QString("AgentToolPreviewCard { border-left: 2px solid #f59e0b; }"));

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
