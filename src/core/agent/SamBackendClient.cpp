#include "SamBackendClient.h"
#include "core/common/Logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace DeepLux {

SamBackendClient::SamBackendClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
{
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &SamBackendClient::onTimeout);
}

SamBackendClient::~SamBackendClient() {
    stopServerProcess();
}

QString SamBackendClient::pyPath() const {
    // 优先环境变量，回退系统 PATH
    QByteArray env = qgetenv("SAM_SERVER_PYTHON");
    if (!env.isEmpty()) return QString::fromLocal8Bit(env);
    return QStringLiteral("python3");
}

void SamBackendClient::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged(s);
}

void SamBackendClient::startTimeout(int ms) {
    stopTimeout();
    m_timeoutTimer.setInterval(ms);
    m_timeoutTimer.start();
}

void SamBackendClient::stopTimeout() {
    if (m_timeoutTimer.isActive()) m_timeoutTimer.stop();
}

void SamBackendClient::startServerProcess() {
    if (m_process) return;
    m_process = new QProcess(this);
    QString script = m_scriptPath.isEmpty()
                         ? QStringLiteral("tools/sam_server/sam_server.py")
                         : m_scriptPath;

    connect(m_process, &QProcess::errorOccurred,
            this, [this](QProcess::ProcessError) {
        setState(State::Error);
        emit errorOccurred(tr("SAM server 进程错误"));
    });

    setState(State::Starting);
    m_process->start(pyPath(), QStringList{script});
    if (!m_process->waitForStarted(5000)) {
        setState(State::Error);
        emit errorOccurred(tr("无法启动 SAM server 进程"));
        return;
    }
    // 给 server 一点时间启动
    setState(State::LoadingModel);
}

void SamBackendClient::stopServerProcess() {
    if (m_process) {
        m_process->kill();
        m_process->waitForFinished(2000);
        delete m_process;
        m_process = nullptr;
    }
    stopTimeout();
}

void SamBackendClient::healthCheck() {
    setState(State::LoadingModel);
    QUrl url(m_serverUrl + "/health");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    m_pendingHealthReply = m_nam->get(req);

    connect(m_pendingHealthReply, &QNetworkReply::finished,
            this, &SamBackendClient::onHealthReply);
    startTimeout();
}

void SamBackendClient::setImage(const QString& imagePath) {
    if (m_state == State::NotStarted || m_state == State::Error) {
        setState(State::LoadingModel);
    }
    m_lastImagePath = imagePath;

    QUrl url(m_serverUrl + "/set_image");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["image_path"] = imagePath;
    m_pendingSetImageReply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));

    connect(m_pendingSetImageReply, &QNetworkReply::finished,
            this, &SamBackendClient::onSetImageReply);
    startTimeout();
}

void SamBackendClient::predict(const QList<QPointF>& positive,
                                const QList<QPointF>& negative,
                                const QRectF& box) {
    if (m_embeddingId.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("尚无 embedding，无法预测"));
        return;
    }

    m_lastPositive = positive;
    m_lastNegative = negative;
    m_lastBox = box;

    setState(State::Busy);
    QUrl url(m_serverUrl + "/predict");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["embedding_id"] = m_embeddingId;

    QJsonArray posArr;
    for (const QPointF& p : positive) {
        posArr.append(QJsonArray{p.x(), p.y()});
    }
    body["points_pos"] = posArr;

    QJsonArray negArr;
    for (const QPointF& p : negative) {
        negArr.append(QJsonArray{p.x(), p.y()});
    }
    body["points_neg"] = negArr;

    if (box.isValid()) {
        body["box"] = QJsonArray{box.x(), box.y(), box.width(), box.height()};
    }

    m_pendingPredictReply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_pendingPredictReply, &QNetworkReply::finished,
            this, &SamBackendClient::onPredictReply);
    startTimeout();
}

void SamBackendClient::unloadImage() {
    if (m_embeddingId.isEmpty()) return;
    QUrl url(m_serverUrl + "/unload_image");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["embedding_id"] = m_embeddingId;
    m_pendingUnloadReply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_pendingUnloadReply, &QNetworkReply::finished,
            this, &SamBackendClient::onUnloadReply);
    startTimeout();
}

static QJsonObject parseReply(QNetworkReply* reply, QString* err) {
    QJsonObject result;
    if (!reply) {
        if (err) *err = QStringLiteral("空回复");
        return result;
    }
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        if (err) *err = reply->errorString();
        return result;
    }
    QByteArray data = reply->readAll();
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        if (err) *err = parseErr.errorString();
        return result;
    }
    result = doc.object();
    return result;
}

void SamBackendClient::onHealthReply() {
    QString err;
    QJsonObject obj = parseReply(m_pendingHealthReply, &err);
    if (!err.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("健康检查失败：%1").arg(err));
        stopTimeout();
        return;
    }
    if (obj.value("status").toString() == "ok") {
        setState(State::Ready);
    } else {
        setState(State::LoadingModel);
    }
    stopTimeout();
}

void SamBackendClient::onSetImageReply() {
    QString err;
    QJsonObject obj = parseReply(m_pendingSetImageReply, &err);
    if (!err.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("setImage 失败：%1").arg(err));
        stopTimeout();
        return;
    }

    QString id = obj.value("embedding_id").toString();
    if (id.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("setImage 响应缺少 embedding_id"));
        stopTimeout();
        return;
    }

    m_embeddingId = id;
    m_imageRetryPending = false;
    setState(State::Ready);
    emit embeddingReady(id);
    stopTimeout();
}

void SamBackendClient::onPredictReply() {
    QString err;
    QJsonObject obj = parseReply(m_pendingPredictReply, &err);
    if (!err.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("预测失败：%1").arg(err));
        stopTimeout();
        return;
    }

    // 检查 invalid_embedding：自动重新 setImage 并重试一次
    QString status = obj.value("status").toString();
    if (status == "invalid_embedding" && !m_imageRetryPending) {
        m_imageRetryPending = true;
        Logger::instance().info(tr("收到 invalid_embedding，自动重新 setImage 并重试"), "SamBackend");
        // 重设 embedding 后再次预测
        setState(State::LoadingModel);
        setImage(m_lastImagePath);
        // setImage 成功后通过 onSetImageReply 触发重试
        // 注意：setImage 完成时 m_imageRetryPending 为 true，需要再次发起 predict
        // 这里通过临时连接处理
        connect(this, &SamBackendClient::embeddingReady, this, [this](const QString&) {
            if (m_imageRetryPending) {
                m_imageRetryPending = false;
                predict(m_lastPositive, m_lastNegative, m_lastBox);
            }
        }, Qt::UniqueConnection);
        stopTimeout();
        return;
    }

    QList<QPointF> polygon;
    QJsonArray polyArr = obj.value("polygon").toArray();
    for (const QJsonValue& v : polyArr) {
        QJsonArray pt = v.toArray();
        if (pt.size() >= 2) {
            polygon.append(QPointF(pt.at(0).toDouble(), pt.at(1).toDouble()));
        }
    }

    QJsonArray bboxArr = obj.value("bbox").toArray();
    QRectF bbox;
    if (bboxArr.size() >= 4) {
        bbox = QRectF(bboxArr.at(0).toDouble(), bboxArr.at(1).toDouble(),
                      bboxArr.at(2).toDouble(), bboxArr.at(3).toDouble());
    }

    double score = obj.value("score").toDouble(0.0);
    QString maskRle = obj.value("mask_rle").toString();

    setState(State::Ready);
    emit predictionReady(polygon, bbox, score, maskRle);
    stopTimeout();
}

void SamBackendClient::onUnloadReply() {
    QString err;
    QJsonObject obj = parseReply(m_pendingUnloadReply, &err);
    Q_UNUSED(obj)
    if (!err.isEmpty()) {
        emit errorOccurred(tr("unloadImage 失败：%1").arg(err));
    }
    m_embeddingId.clear();
    setState(State::NotStarted);
    stopTimeout();
}

void SamBackendClient::onTimeout() {
    if (m_pendingHealthReply) m_pendingHealthReply->abort();
    if (m_pendingSetImageReply) m_pendingSetImageReply->abort();
    if (m_pendingPredictReply) m_pendingPredictReply->abort();
    if (m_pendingUnloadReply) m_pendingUnloadReply->abort();
    setState(State::Error);
    emit errorOccurred(tr("SAM 请求超时（30s）"));
}

} // namespace DeepLux
