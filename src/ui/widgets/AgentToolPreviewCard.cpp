#include "AgentToolPreviewCard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QJsonDocument>
#include <QScrollArea>

namespace DeepLux {

AgentToolPreviewCard::AgentToolPreviewCard(const QList<ToolItem>& tools, QWidget* parent)
    : QWidget(parent)
    , m_tools(tools)
{
    setupUi();
}

void AgentToolPreviewCard::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(12, 12, 12, 12);
    mainLayout->setSpacing(8);

    QLabel* title = new QLabel("Agent wants to execute:", this);
    title->setStyleSheet("font-weight: bold; font-size: 14px;");
    mainLayout->addWidget(title);

    for (int i = 0; i < m_tools.size(); ++i) {
        const ToolItem& t = m_tools[i];
        QWidget* itemWidget = new QWidget(this);
        auto* itemLayout = new QHBoxLayout(itemWidget);
        itemLayout->setContentsMargins(8, 6, 8, 6);

        QLabel* nameLabel = new QLabel(QString("%1. %2").arg(i + 1).arg(t.name), this);
        nameLabel->setStyleSheet("font-weight: bold;");
        itemLayout->addWidget(nameLabel);

        QString paramsStr = QString(QJsonDocument(t.params).toJson(QJsonDocument::Compact));
        QLabel* paramsLabel = new QLabel(paramsStr, this);
        paramsLabel->setStyleSheet("color: #666; font-size: 11px;");
        paramsLabel->setWordWrap(true);
        itemLayout->addWidget(paramsLabel, 1);

        itemWidget->setStyleSheet("background-color: #f5f5f5; border-radius: 4px;");
        mainLayout->addWidget(itemWidget);
    }

    auto* btnLayout = new QHBoxLayout();
    QPushButton* confirmBtn = new QPushButton("Confirm", this);
    QPushButton* cancelBtn = new QPushButton("Cancel", this);
    confirmBtn->setStyleSheet("background-color: #27ae60; color: white;");
    cancelBtn->setStyleSheet("background-color: #e74c3c; color: white;");

    btnLayout->addStretch();
    btnLayout->addWidget(confirmBtn);
    btnLayout->addWidget(cancelBtn);
    mainLayout->addLayout(btnLayout);

    connect(confirmBtn, &QPushButton::clicked, this, &AgentToolPreviewCard::confirmed);
    connect(cancelBtn, &QPushButton::clicked, this, &AgentToolPreviewCard::cancelled);

    setStyleSheet("background-color: #fff3cd; border: 1px solid #ffc107; border-radius: 8px;");
}

} // namespace DeepLux
