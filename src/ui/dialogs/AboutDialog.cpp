#include "AboutDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QFrame>
#include <QPainter>

namespace DeepLux {

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About DeepLux"));
    setWindowFlags(Qt::Dialog | Qt::WindowCloseButtonHint);
    setFixedSize(420, 320);
    setStyleSheet(R"(
        QDialog {
            background-color: #2b2b2b;
            color: #ffffff;
        }
        QLabel {
            color: #ffffff;
            background-color: transparent;
        }
        QPushButton {
            padding: 10px 30px;
            border: none;
            border-radius: 4px;
            background-color: #0078d4;
            color: #ffffff;
            font-size: 14px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #1084d8;
        }
    )");

    setupUI();
}

void AboutDialog::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(15);
    mainLayout->setContentsMargins(40, 30, 40, 30);

    // Logo area
    QFrame* logoFrame = new QFrame(this);
    logoFrame->setFixedSize(80, 80);
    logoFrame->setStyleSheet(R"(
        QFrame {
            background-color: #0078d4;
            border-radius: 10px;
        }
    )");

    QVBoxLayout* logoLayout = new QVBoxLayout(logoFrame);
    logoLayout->setContentsMargins(0, 0, 0, 0);

    QLabel* logoIcon = new QLabel("DL", logoFrame);
    logoIcon->setStyleSheet(R"(
        QLabel {
            color: #ffffff;
            font-size: 32px;
            font-weight: bold;
            background-color: transparent;
        }
    )");
    logoIcon->setAlignment(Qt::AlignCenter);
    logoLayout->addWidget(logoIcon);

    mainLayout->addWidget(logoFrame, 0, Qt::AlignHCenter);

    // App name
    m_nameLabel = new QLabel("DeepLux", this);
    m_nameLabel->setStyleSheet(R"(
        QLabel {
            font-size: 28px;
            font-weight: bold;
            color: #0078d4;
            background-color: transparent;
        }
    )");
    m_nameLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_nameLabel);

    // Version
    m_versionLabel = new QLabel(tr("Version 1.0.0"), this);
    m_versionLabel->setStyleSheet(R"(
        QLabel {
            font-size: 14px;
            color: #888;
            background-color: transparent;
        }
    )");
    m_versionLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_versionLabel);

    // Description
    m_descriptionLabel = new QLabel(
        tr("A powerful machine vision software platform\n")
        + tr("for industrial automation and quality inspection.\n\n")
        + tr("Copyright © 2024 DeepLux Team"),
        this
    );
    m_descriptionLabel->setStyleSheet(R"(
        QLabel {
            font-size: 13px;
            color: #aaa;
            background-color: transparent;
            line-height: 1.6;
        }
    )");
    m_descriptionLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_descriptionLabel);

    mainLayout->addStretch();

    // Close button
    QPushButton* closeBtn = new QPushButton(tr("Close"), this);
    closeBtn->setFixedWidth(100);
    mainLayout->addWidget(closeBtn, 0, Qt::AlignHCenter);

    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
}

} // namespace DeepLux