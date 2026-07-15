#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QProcess>
#include <QRectF>
#include <QString>
#include <QTimer>

class QNetworkReply;

namespace DeepLux {

/**
 * @brief SAM 后端客户端
 *
 * 通过 QNetworkAccessManager 调用 FastAPI（Python sam_server），完成：
 * - healthCheck()
 * - setImage(path)
 * - predict(positive, negative, box)
 * - unloadImage()
 *
 * 状态机：NotStarted → Starting → LoadingModel → Ready → Busy → (Ready|Error)
 *
 * 行为：
 * - 启动时根据 SAM_SERVER_PYTHON 环境变量启动 Python 脚本（可选）
 * - 30s 超时：将状态置为 Error 并发出 errorOccurred
 * - 收到 invalid_embedding 时自动重新 setImage 并重试一次预测
 */
class SamBackendClient : public QObject {
    Q_OBJECT

public:
    enum class State { NotStarted, Starting, LoadingModel, Ready, Busy, Error };
    Q_ENUM(State)

    explicit SamBackendClient(QObject* parent = nullptr);
    ~SamBackendClient() override;

    // 配置
    void setServerUrl(const QString& url) {
        m_serverUrl = url;
    }
    QString serverUrl() const {
        return m_serverUrl;
    }

    void setServerScriptPath(const QString& path) {
        m_scriptPath = path;
    }
    QString serverScriptPath() const {
        return m_scriptPath;
    }

    State state() const {
        return m_state;
    }
    QString currentEmbeddingId() const {
        return m_embeddingId;
    }
    QString modelName() const {
        return m_modelName;
    }

    // API
    void healthCheck();
    void setImage(const QString& imagePath);
    void predict(const QList<QPointF>& positive, const QList<QPointF>& negative, const QRectF& box);
    void unloadImage();

    // 启动/停止 Python 进程（可选，便于测试时跳过）
    void startServerProcess();
    void stopServerProcess();

signals:
    void stateChanged(SamBackendClient::State newState);
    void predictionReady(const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle);
    void errorOccurred(const QString& message);
    void embeddingReady(const QString& embeddingId);

private slots:
    void onHealthReply();
    void onSetImageReply();
    void onPredictReply();
    void onUnloadReply();
    void onTimeout();

private:
    void setState(State s);
    void startTimeout(int ms = 30000);
    void stopTimeout();
    QString pyPath() const;
    QString resolvedScriptPath() const;

    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_pendingHealthReply;
    QPointer<QNetworkReply> m_pendingSetImageReply;
    QPointer<QNetworkReply> m_pendingPredictReply;
    QPointer<QNetworkReply> m_pendingUnloadReply;

    QProcess* m_process = nullptr;
    QTimer m_timeoutTimer;

    State m_state = State::NotStarted;
    QString m_serverUrl = QStringLiteral("http://127.0.0.1:8000");
    QString m_scriptPath;
    QString m_embeddingId;
    QString m_modelName;

    // 记录最近一次预测参数，用于 invalid_embedding 重试
    QList<QPointF> m_lastPositive;
    QList<QPointF> m_lastNegative;
    QRectF m_lastBox;
    QString m_lastImagePath;
    bool m_imageRetryPending = false;
    int m_healthPollsRemaining = 0;
};

} // namespace DeepLux
