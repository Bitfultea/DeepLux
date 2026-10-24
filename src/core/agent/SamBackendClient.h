#pragma once

#include "../platform/Platform.h"

#include <QImage>
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
#ifdef DEEPLUX_HAS_ONNXRUNTIME
#include <memory>
#endif

class QNetworkReply;

namespace DeepLux {

/**
 * @brief 多掩膜候选（SAM 批4）——/predict 响应可选 candidates 数组的元素
 *
 * sam_server multimask_output 产生至多 3 个候选（按 score 降序），顶层
 * polygon/bbox/score 字段仍为最佳候选（向后兼容）。候选经客户端严格解析
 * （类型/有限值/score∈[0,1]），消费方（SamPredictor 的 candidateIndex 选择、
 * 标注对话框的候选循环）通过 lastCandidates() 读取本次 predict 的候选。
 */
struct SamCandidate {
    QList<QPointF> polygon;
    QRectF bbox;
    double score = 0.0;
    QString maskRle;
    QImage maskImage;
};

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
class DEEPLUX_API SamBackendClient : public QObject {
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
    // 请求超时（默认 30s）。测试可调小以便在合理时长内断言超时路径。
    void setTimeoutMs(int ms) {
        if (ms > 0)
            m_timeoutMs = ms;
    }
    int timeoutMs() const {
        return m_timeoutMs;
    }
    QString currentEmbeddingId() const {
        return m_embeddingId;
    }
    QString modelName() const {
        return m_modelName;
    }
    QString nativeDevice() const {
        return m_nativeDevice;
    }
    // SAM 批4：最近一次成功 predict 的多掩膜候选（按 score 降序；无候选为空）。
    // 在 predictionReady 发出前就绪，回调内读取即为本次结果。
    const QList<SamCandidate>& lastCandidates() const {
        return m_lastCandidates;
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
    void predictionReady(const QList<QPointF>& polygon, const QRectF& bbox, double score, const QString& maskRle,
                         const QImage& maskImage);
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
    void startTimeout(int ms = -1); // ms<=0 表示使用 setTimeoutMs 配置的超时
    void stopTimeout();
    // 某个请求回调结束时调用：先清除该请求的挂起指针，仅当再无任何挂起请求时
    // 才停止超时保护，避免并发请求（如 unload+setImage）互相取消对方的超时。
    void settleReply(QPointer<QNetworkReply>& reply);
    // latest-wins：发起同类型新请求前取消仍挂起的旧请求，避免旧回调误解析新请求。
    void abortPendingReply(QPointer<QNetworkReply>& reply);
    QString pyPath() const;
    QString resolvedScriptPath() const;
    void startEnvironmentStep();
    void finishEnvironmentInitialization(bool ok, const QString& message);

    QNetworkAccessManager* m_nam = nullptr;
    QPointer<QNetworkReply> m_pendingHealthReply;
    QPointer<QNetworkReply> m_pendingSetImageReply;
    QPointer<QNetworkReply> m_pendingPredictReply;
    QPointer<QNetworkReply> m_pendingUnloadReply;
    // 请求序号：同类型请求重叠时，回调据序号判断触发者是否仍为最新请求。
    qint64 m_healthSeq = 0;
    qint64 m_setImageSeq = 0;
    qint64 m_unloadSeq = 0;
    qint64 m_predictSeq = 0; // Fix 4: 请求序号，忽略过期的推理结果
    qint64 m_lastCompletedSeq = 0;

    QProcess* m_process = nullptr;
    QProcess* m_envProcess = nullptr;
    QTimer m_timeoutTimer;
    int m_timeoutMs = 30000;
#ifdef DEEPLUX_HAS_ONNXRUNTIME
    struct NativeRequest;
    std::unique_ptr<NativeRequest> m_nativeRequest;
    QTimer m_nativePollTimer;
#endif

    State m_state = State::NotStarted;
    QString m_serverUrl = QStringLiteral("http://127.0.0.1:0"); // 0 = auto-assign
    QString m_scriptPath;
    QString m_embeddingId;
    QString m_modelName;
    QString m_nativeDevice;
    QString m_modelPath;

    // 记录最近一次预测参数，用于 invalid_embedding 重试
    QList<QPointF> m_lastPositive;
    QList<QPointF> m_lastNegative;
    QRectF m_lastBox;
    QString m_lastImagePath;
    QList<SamCandidate> m_lastCandidates; // SAM 批4：本次 predict 的候选（发起时清空）
    bool m_imageRetryPending = false;
    int m_healthPollsRemaining = 0;
    int m_envInitStep = 0;
    QString m_envLastOutput;
};

} // namespace DeepLux
