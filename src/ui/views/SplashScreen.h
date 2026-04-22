#pragma once

#include <QWidget>
#include <QLabel>
#include <QProgressBar>
#include <QVBoxLayout>
#include <QTimer>
#include <QDesktopWidget>
#include <QStringList>
#include <QTextEdit>

namespace DeepLux {

class SplashScreen : public QWidget
{
    Q_OBJECT

public:
    explicit SplashScreen(QWidget* parent = nullptr);
    ~SplashScreen();

    void setProgress(int value, const QString& status = QString());
    void setStatus(const QString& status);
    void appendLog(const QString& text);
    void showFailedPlugins(const QStringList& failedPlugins);

signals:
    void loadingFinished();

private:
    void initUI();

    QLabel* m_logoLabel;
    QLabel* m_titleLabel;
    QLabel* m_statusLabel;
    QTextEdit* m_logEdit;
    QProgressBar* m_progressBar;
    QLabel* m_versionLabel;
};

} // namespace DeepLux
