#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QProcess>
#include <QRectF>
#include <QString>
#include <QStringList>
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
    QString resolvedServerScriptPath() const {
        return resolvedScriptPath();
    }

    void setModelPath(const QString& path) {
        if (m_modelPath == path)
            return;
        stopServerProcess();
        m_modelPath = path;
    }
    QString modelPath() const {
        return m_modelPath;
    }
    QString inferredModelType() const;
    QString unsupportedModelReason() const;

    QString managedEnvironmentPath() const;
    QString managedPythonPath() const;
    QString managedSitePackagesPath() const;
    QStringList requiredPythonModules() const;
    QString requirementsPath() const;
    QString resolvedPythonPath() const;
    bool managedEnvironmentReady() const;
    bool isEnvironmentInitializationRunning() const {
        return m_envProcess != nullptr;
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
    void cancelPendingPrediction();
    void initializeManagedEnvironment();
    void cancelEnvironmentInitialization();

signals:
    void stateChanged(SamBackendClient::State newState);
    void predictionReady(const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle);
    void errorOccurred(const QString& message);
    void embeddingReady(const QString& embeddingId);
    void environmentInitializationStarted();
    void environmentInitializationProgress(const QString& message);
    void environmentInitializationFinished(bool ok, const QString& message);

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
    void startEnvironmentStep();
    void finishEnvironmentInitialization(bool ok, const QString& message);

    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_pendingHealthReply;
    QPointer<QNetworkReply> m_pendingSetImageReply;
    QPointer<QNetworkReply> m_pendingPredictReply;
    qint64 m_predictSeq = 0;     // Fix 4: 请求序号，忽略过期的推理结果
    qint64 m_lastCompletedSeq = 0;
    QPointer<QNetworkReply> m_pendingUnloadReply;

    QProcess* m_process = nullptr;
    QProcess* m_envProcess = nullptr;
    QTimer m_timeoutTimer;

    State m_state = State::NotStarted;
    QString m_serverUrl = QStringLiteral("http://127.0.0.1:8000");
    QString m_scriptPath;
    QString m_embeddingId;
    QString m_modelName;
    QString m_modelPath;

    // 记录最近一次预测参数，用于 invalid_embedding 重试
    QList<QPointF> m_lastPositive;
    QList<QPointF> m_lastNegative;
    QRectF m_lastBox;
    QString m_lastImagePath;
    bool m_imageRetryPending = false;
    int m_healthPollsRemaining = 0;
    int m_envInitStep = 0;
    QString m_envLastOutput;
};

} // namespace DeepLux
