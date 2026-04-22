#include "SplashScreen.h"
#include <QApplication>
#include <QDesktopWidget>

namespace DeepLux {

SplashScreen::SplashScreen(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_ShowWithoutActivating);
    initUI();

    QDesktopWidget* desktop = QApplication::desktop();
    QRect screenGeometry = desktop->screenGeometry();
    int x = (screenGeometry.width() - width()) / 2;
    int y = (screenGeometry.height() - height()) / 2;
    move(x, y);
}

SplashScreen::~SplashScreen()
{
}

void SplashScreen::initUI()
{
    setFixedSize(420, 320);
    setStyleSheet("background-color: #1a1a2e; border-radius: 10px;");

    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(30, 25, 30, 25);
    mainLayout->setSpacing(0);

    // Logo
    m_logoLabel = new QLabel(tr("◆"), this);
    m_logoLabel->setAlignment(Qt::AlignCenter);
    QFont logoFont = m_logoLabel->font();
    logoFont.setPointSize(32);
    m_logoLabel->setFont(logoFont);
    m_logoLabel->setStyleSheet("color: #e94560;");

    // 标题
    m_titleLabel = new QLabel(tr("DeepLux"), this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    m_titleLabel->setStyleSheet("color: #eaeaea;");

    // 副标题
    QLabel* subTitle = new QLabel(tr("Vision System"), this);
    subTitle->setAlignment(Qt::AlignCenter);
    QFont subFont = subTitle->font();
    subFont.setPointSize(10);
    subTitle->setFont(subFont);
    subTitle->setStyleSheet("color: #7f8c8d;");

    // 进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setFixedHeight(4);
    m_progressBar->setTextVisible(false);
    m_progressBar->setStyleSheet(R"(
        QProgressBar {
            background-color: #16213e;
            border: none;
            border-radius: 2px;
        }
        QProgressBar::chunk {
            background-color: #e94560;
            border-radius: 2px;
        }
    )");

    // 状态
    m_statusLabel = new QLabel(tr("Initializing..."), this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    QFont statusFont = m_statusLabel->font();
    statusFont.setPointSize(11);
    m_statusLabel->setFont(statusFont);
    m_statusLabel->setStyleSheet("color: #a0a0a0;");

    // 日志窗口
    m_logEdit = new QTextEdit(this);
    m_logEdit->setReadOnly(true);
    m_logEdit->setMaximumHeight(100);
    m_logEdit->setStyleSheet(R"(
        QTextEdit {
            background-color: transparent;
            border: none;
            color: #7f8c8d;
            font-size: 10px;
            padding: 5px;
        }
    )");
    m_logEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_logEdit->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // 版本
    m_versionLabel = new QLabel(tr("v1.0"), this);
    m_versionLabel->setAlignment(Qt::AlignCenter);
    m_versionLabel->setStyleSheet("color: #606060; font-size: 10px;");

    mainLayout->addStretch();
    mainLayout->addWidget(m_logoLabel);
    mainLayout->addWidget(m_titleLabel);
    mainLayout->addWidget(subTitle);
    mainLayout->addSpacing(8);
    mainLayout->addWidget(m_progressBar);
    mainLayout->addSpacing(4);
    mainLayout->addWidget(m_statusLabel);
    mainLayout->addSpacing(8);
    mainLayout->addWidget(m_logEdit);
    mainLayout->addStretch();
    mainLayout->addWidget(m_versionLabel);
}

void SplashScreen::setProgress(int value, const QString& status)
{
    m_progressBar->setValue(value);
    if (!status.isEmpty()) {
        m_statusLabel->setText(status);
    }
}

void SplashScreen::setStatus(const QString& status)
{
    m_statusLabel->setText(status);
}

void SplashScreen::appendLog(const QString& text)
{
    m_logEdit->append(text);
    m_logEdit->ensureCursorVisible();
}

void SplashScreen::showFailedPlugins(const QStringList& failedPlugins)
{
    if (!failedPlugins.isEmpty()) {
        m_logEdit->append("");
        m_logEdit->append(QString("<span style='color: #e94560;'>加载失败:</span>"));
        for (const QString& name : failedPlugins) {
            m_logEdit->append(QString("  - %1").arg(name));
        }
    }
}

} // namespace DeepLux
