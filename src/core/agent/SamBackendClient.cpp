#include "SamBackendClient.h"

#include "core/common/Logger.h"
#include "core/platform/PathUtils.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

namespace DeepLux {

namespace {
QString findSamServerScriptFrom(QDir dir) {
    for (int i = 0; i < 6; ++i) {
        const QString candidate = dir.absoluteFilePath(QStringLiteral("tools/sam_server/sam_server.py"));
        if (QFileInfo::exists(candidate))
            return QFileInfo(candidate).absoluteFilePath();
        if (!dir.cdUp())
            break;
    }
    return QString();
}
} // namespace

SamBackendClient::SamBackendClient(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &SamBackendClient::onTimeout);
}

SamBackendClient::~SamBackendClient() {
    stopServerProcess();
    finishEnvironmentInitialization(false, QString());
}

QString SamBackendClient::inferredModelType() const {
    const QString fileName = QFileInfo(m_modelPath).fileName().toLower();
    if (fileName.contains(QStringLiteral("vit_h")))
        return QStringLiteral("vit_h");
    if (fileName.contains(QStringLiteral("vit_l")))
        return QStringLiteral("vit_l");
    return QStringLiteral("vit_b");
}

QString SamBackendClient::unsupportedModelReason() const {
    const QString fileName = QFileInfo(m_modelPath).fileName().toLower();
    if (fileName.contains(QStringLiteral("sam3")) || fileName.contains(QStringLiteral("sam_3")) ||
        fileName.contains(QStringLiteral("sam-3"))) {
        return tr("当前后端使用原版 segment-anything，暂不支持 SAM3 权重，请使用原版 SAM 权重 sam_vit_b/l/h.pth");
    }
    if (fileName.contains(QStringLiteral("sam2")) || fileName.contains(QStringLiteral("sam_2")) ||
        fileName.contains(QStringLiteral("sam-2"))) {
        return tr("当前后端使用原版 segment-anything，暂不支持 SAM2 权重，请使用原版 SAM 权重 sam_vit_b/l/h.pth");
    }
    if (fileName.contains(QStringLiteral("sam_hq")) || fileName.contains(QStringLiteral("sam-hq")) ||
        fileName.contains(QStringLiteral("hq_sam"))) {
        return tr("当前后端使用原版 segment-anything，暂不支持 SAM-HQ 权重，请使用原版 SAM 权重 sam_vit_b/l/h.pth");
    }
    return QString();
}

QString SamBackendClient::managedEnvironmentPath() const {
    return QDir(PathUtils::appDataPath()).filePath(QStringLiteral("sam_env"));
}

QString SamBackendClient::managedPythonPath() const {
#ifdef DEEPLUX_PLATFORM_WINDOWS
    return QDir(managedEnvironmentPath()).filePath(QStringLiteral("Scripts/python.exe"));
#else
    return QDir(managedEnvironmentPath()).filePath(QStringLiteral("bin/python"));
#endif
}

QString SamBackendClient::managedSitePackagesPath() const {
#ifdef DEEPLUX_PLATFORM_WINDOWS
    return QDir(managedEnvironmentPath()).filePath(QStringLiteral("Lib/site-packages"));
#else
    QString version = QStringLiteral("3.10");
    QFile cfg(QDir(managedEnvironmentPath()).filePath(QStringLiteral("pyvenv.cfg")));
    if (cfg.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromLocal8Bit(cfg.readAll()).split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            if (!line.startsWith(QStringLiteral("version")))
                continue;
            const QStringList parts = line.section('=', 1).trimmed().split('.');
            if (parts.size() >= 2)
                version = parts.at(0) + QStringLiteral(".") + parts.at(1);
            break;
        }
    }
    return QDir(managedEnvironmentPath()).filePath(QStringLiteral("lib/python%1/site-packages").arg(version));
#endif
}

QStringList SamBackendClient::requiredPythonModules() const {
    // Base modules always required (matches uncommented lines in requirements.txt)
    QStringList base = {QStringLiteral("fastapi"), QStringLiteral("uvicorn"),
                        QStringLiteral("numpy"),  QStringLiteral("PIL")};
    // SAM model modules only required when a real model path is set (not stub mode)
    if (!m_modelPath.isEmpty()) {
        base << QStringLiteral("torch") << QStringLiteral("segment_anything") << QStringLiteral("cv2");
    }
    return base;
}

QString SamBackendClient::requirementsPath() const {
    return QFileInfo(resolvedScriptPath()).dir().filePath(QStringLiteral("requirements.txt"));
}

bool SamBackendClient::managedEnvironmentReady() const {
    if (!QFileInfo::exists(managedPythonPath()))
        return false;
    const QDir sitePackages(managedSitePackagesPath());
    for (const QString& module : requiredPythonModules()) {
        if (!QFileInfo::exists(sitePackages.filePath(module)))
            return false;
    }
    return true;
}

QString SamBackendClient::resolvedPythonPath() const {
    QByteArray env = qgetenv("SAM_SERVER_PYTHON");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    if (managedEnvironmentReady())
        return managedPythonPath();
#ifdef DEEPLUX_PLATFORM_WINDOWS
    // Windows 上 python3 通常不存在，使用 python 或 py
    return QStringLiteral("python");
#else
    return QStringLiteral("python3");
#endif
}

QString SamBackendClient::pyPath() const {
    return resolvedPythonPath();
}

QString SamBackendClient::resolvedScriptPath() const {
    if (!m_scriptPath.isEmpty())
        return m_scriptPath;

    QByteArray env = qgetenv("DEEPLUX_SAM_SERVER_SCRIPT");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);

    const QString appRelative = findSamServerScriptFrom(QDir(QCoreApplication::applicationDirPath()));
    if (!appRelative.isEmpty())
        return appRelative;

    const QString cwdRelative = findSamServerScriptFrom(QDir::current());
    if (!cwdRelative.isEmpty())
        return cwdRelative;

    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../tools/sam_server/sam_server.py"));
}

void SamBackendClient::setState(State s) {
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged(s);
}

void SamBackendClient::startTimeout(int ms) {
    stopTimeout();
    m_timeoutTimer.setInterval(ms);
    m_timeoutTimer.start();
}

void SamBackendClient::stopTimeout() {
    if (m_timeoutTimer.isActive())
        m_timeoutTimer.stop();
}

void SamBackendClient::startServerProcess() {
    if (m_process)
        return;

    const QString script = resolvedScriptPath();
    if (script.isEmpty() || !QFileInfo::exists(script)) {
        setState(State::Error);
        emit errorOccurred(tr("SAM server 脚本不存在：%1").arg(script));
        return;
    }

    const QString modelError = unsupportedModelReason();
    if (!modelError.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(modelError);
        return;
    }

    m_process = new QProcess(this);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        setState(State::Error);
        emit errorOccurred(tr("SAM server 进程错误：%1").arg(m_process ? m_process->errorString() : QString()));
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                const QString stderrText =
                    m_process ? QString::fromLocal8Bit(m_process->readAllStandardError()).trimmed() : QString();
                setState(State::Error);
                emit errorOccurred(
                    tr("SAM server 已退出 code=%1 status=%2 %3")
                        .arg(code)
                        .arg(status == QProcess::NormalExit ? QStringLiteral("normal") : QStringLiteral("crash"),
                             stderrText));
            });
    // Fix 8: 解析 stdout 获取实际端口，同时转发到 Logger
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_process)
            return;
        while (m_process->canReadLine()) {
            const QString line = QString::fromLocal8Bit(m_process->readLine()).trimmed();
            if (line.isEmpty())
                continue;
            if (line.startsWith(QStringLiteral("SAM_SERVER_PORT="))) {
                const int port = line.mid(16).toInt();
                if (port > 0) {
                    m_serverUrl = QStringLiteral("http://127.0.0.1:%1").arg(port);
                    Logger::instance().info(tr("SAM server 端口: %1").arg(port), "SamBackend");
                }
                continue;
            }
            Logger::instance().info(line, "SamBackend");
        }
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        if (!m_process)
            return;
        const QByteArray data = m_process->readAllStandardError();
        const QString text = QString::fromLocal8Bit(data).trimmed();
        if (text.isEmpty())
            return;
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString& line : lines)
            Logger::instance().warning(line.trimmed(), "SamBackend");
    });

    setState(State::Starting);
    m_healthPollsRemaining = 60;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!m_modelPath.isEmpty()) {
        env.insert(QStringLiteral("DEEPLUX_SAM_MODEL"), m_modelPath);
        if (!env.contains(QStringLiteral("DEEPLUX_SAM_MODEL_TYPE")))
            env.insert(QStringLiteral("DEEPLUX_SAM_MODEL_TYPE"), inferredModelType());
    }
    m_process->setProcessEnvironment(env);
    m_process->start(pyPath(), QStringList{script});
    if (!m_process->waitForStarted(1000)) {
        setState(State::Error);
        emit errorOccurred(tr("无法启动 SAM server 进程：%1").arg(m_process->errorString()));
        m_process->deleteLater();
        m_process = nullptr;
        return;
    }

    setState(State::LoadingModel);
    QTimer::singleShot(300, this, &SamBackendClient::healthCheck);
}

void SamBackendClient::stopServerProcess() {
    auto cancelReply = [this](QPointer<QNetworkReply>& reply) {
        if (!reply)
            return;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    };
    cancelReply(m_pendingHealthReply);
    cancelReply(m_pendingSetImageReply);
    cancelReply(m_pendingPredictReply);
    cancelReply(m_pendingUnloadReply);

    if (m_process) {
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
        m_process->waitForFinished(2000);
        delete m_process;
        m_process = nullptr;
    }
    m_embeddingId.clear();
    m_imageRetryPending = false;
    m_healthPollsRemaining = 0;
    stopTimeout();
    setState(State::NotStarted);
}

void SamBackendClient::initializeManagedEnvironment() {
    if (m_envProcess)
        return;

    const QString requirements = requirementsPath();
    if (!QFileInfo::exists(requirements)) {
        emit environmentInitializationFinished(false, tr("requirements.txt 不存在：%1").arg(requirements));
        return;
    }

    if (!PathUtils::ensureDirExists(QFileInfo(managedEnvironmentPath()).absolutePath())) {
        emit environmentInitializationFinished(false, tr("无法创建 SAM 环境目录"));
        return;
    }

    m_envProcess = new QProcess(this);
    m_envInitStep = 0;
    m_envLastOutput.clear();
    connect(m_envProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        const QString message = m_envProcess ? m_envProcess->errorString() : QString();
        finishEnvironmentInitialization(false, tr("初始化环境失败：%1").arg(message));
    });
    auto forwardOutput = [this](const QByteArray& data) {
        QString text = QString::fromLocal8Bit(data).trimmed();
        if (text.isEmpty())
            return;
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        if (!lines.isEmpty())
            text = lines.last().trimmed();
        if (text.size() > 180)
            text = text.right(180);
        m_envLastOutput = text;
        emit environmentInitializationProgress(tr("SAM 环境：%1").arg(text));
    };
    connect(m_envProcess, &QProcess::readyReadStandardOutput, this,
            [this, forwardOutput]() { forwardOutput(m_envProcess->readAllStandardOutput()); });
    connect(m_envProcess, &QProcess::readyReadStandardError, this,
            [this, forwardOutput]() { forwardOutput(m_envProcess->readAllStandardError()); });
    connect(m_envProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                if (!m_envProcess)
                    return;
                if (status != QProcess::NormalExit || code != 0) {
                    QString err = QString::fromLocal8Bit(m_envProcess->readAllStandardError()).trimmed();
                    if (err.isEmpty())
                        err = m_envLastOutput;
                    finishEnvironmentInitialization(false, err.isEmpty() ? tr("初始化环境失败") : err);
                    return;
                }
                if (m_envInitStep == 0) {
                    m_envInitStep = 1;
                    startEnvironmentStep();
                    return;
                }
                finishEnvironmentInitialization(true, tr("SAM 环境已就绪"));
            });

    emit environmentInitializationStarted();
    startEnvironmentStep();
}

void SamBackendClient::cancelEnvironmentInitialization() {
    finishEnvironmentInitialization(false, tr("SAM 环境初始化已取消"));
}

void SamBackendClient::startEnvironmentStep() {
    if (!m_envProcess)
        return;
    if (m_envInitStep == 0) {
        if (QFileInfo::exists(managedPythonPath())) {
            m_envInitStep = 1;
            startEnvironmentStep();
            return;
        }
        m_envProcess->start(resolvedPythonPath(),
                            QStringList{QStringLiteral("-m"), QStringLiteral("venv"), QStringLiteral("--without-pip"),
                                        managedEnvironmentPath()});
        return;
    }

#ifdef DEEPLUX_PLATFORM_WINDOWS
    const QString pipName = QStringLiteral("pip.exe");
#else
    const QString pipName = QStringLiteral("pip");
#endif
    const QString venvPip = QDir(QFileInfo(managedPythonPath()).absolutePath()).filePath(pipName);
    if (QFileInfo::exists(venvPip)) {
        m_envProcess->start(managedPythonPath(),
                            QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                                        QStringLiteral("-r"), requirementsPath()});
        return;
    }

    const QString pip3 = QStandardPaths::findExecutable(QStringLiteral("pip3"));
    if (pip3.isEmpty()) {
        finishEnvironmentInitialization(false, tr("系统缺少 pip3，无法安装 SAM 依赖"));
        return;
    }
    PathUtils::ensureDirExists(managedSitePackagesPath());
    emit environmentInitializationProgress(tr("SAM 环境：正在安装依赖，首次安装可能需要几分钟"));
    m_envProcess->start(pip3, QStringList{QStringLiteral("install"), QStringLiteral("--target"),
                                          managedSitePackagesPath(), QStringLiteral("-r"), requirementsPath()});
}

void SamBackendClient::finishEnvironmentInitialization(bool ok, const QString& message) {
    if (m_envProcess) {
        disconnect(m_envProcess, nullptr, this, nullptr);
        if (m_envProcess->state() != QProcess::NotRunning) {
            m_envProcess->kill();
            m_envProcess->waitForFinished(2000);
        }
        m_envProcess->deleteLater();
        m_envProcess = nullptr;
    }
    m_envInitStep = 0;
    if (!message.isEmpty())
        emit environmentInitializationFinished(ok, message);
}

void SamBackendClient::healthCheck() {
    setState(State::LoadingModel);
    QUrl url(m_serverUrl + "/health");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    m_pendingHealthReply = m_nam->get(req);

    connect(m_pendingHealthReply, &QNetworkReply::finished, this, &SamBackendClient::onHealthReply);
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

    connect(m_pendingSetImageReply, &QNetworkReply::finished, this, &SamBackendClient::onSetImageReply);
    startTimeout();
}

void SamBackendClient::predict(const QList<QPointF>& positive, const QList<QPointF>& negative, const QRectF& box) {
    if (m_embeddingId.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("尚无 embedding，无法预测"));
        return;
    }

    cancelPendingPrediction();

    m_lastPositive = positive;
    m_lastNegative = negative;
    m_lastBox = box;

    // Fix 4: 分配递增序号，用于在回调中忽略过期结果
    const qint64 thisSeq = ++m_predictSeq;

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
    m_pendingPredictReply->setProperty("predictSeq", thisSeq);
    connect(m_pendingPredictReply, &QNetworkReply::finished, this, &SamBackendClient::onPredictReply);
    startTimeout();
}

void SamBackendClient::cancelPendingPrediction() {
    if (!m_pendingPredictReply)
        return;
    disconnect(m_pendingPredictReply, nullptr, this, nullptr);
    m_pendingPredictReply->abort();
    m_pendingPredictReply->deleteLater();
    m_pendingPredictReply = nullptr;
    stopTimeout();
    setState(m_embeddingId.isEmpty() ? State::NotStarted : State::Ready);
}

void SamBackendClient::unloadImage() {
    if (m_embeddingId.isEmpty())
        return;
    // Fix P0-3: 取消任何挂起的 predict，防止 unload 和 predict 竞争
    cancelPendingPrediction();
    QUrl url(m_serverUrl + "/unload_image");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["embedding_id"] = m_embeddingId;
    m_pendingUnloadReply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    // 记录 unload 的 embedding ID，回调时检查是否已被新 set_image 替换
    m_pendingUnloadReply->setProperty("unloadEmbeddingId", m_embeddingId);
    connect(m_pendingUnloadReply, &QNetworkReply::finished, this, &SamBackendClient::onUnloadReply);
    startTimeout();
}

static QJsonObject parseReply(QNetworkReply* reply, QString* err) {
    QJsonObject result;
    if (!reply) {
        if (err)
            *err = QStringLiteral("空回复");
        return result;
    }
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        if (err)
            *err = reply->errorString();
        return result;
    }
    QByteArray data = reply->readAll();
    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        if (err)
            *err = parseErr.errorString();
        return result;
    }
    result = doc.object();
    return result;
}

void SamBackendClient::onHealthReply() {
    QString err;
    QJsonObject obj = parseReply(m_pendingHealthReply, &err);
    if (!err.isEmpty()) {
        if (m_process && m_healthPollsRemaining-- > 0) {
            stopTimeout();
            QTimer::singleShot(500, this, &SamBackendClient::healthCheck);
            return;
        }
        setState(State::Error);
        emit errorOccurred(tr("健康检查失败：%1").arg(err));
        stopTimeout();
        return;
    }

    const QString status = obj.value("status").toString();
    const QString model = obj.value("model_name").toString();
    if (!model.isEmpty())
        m_modelName = model;

    if (status == "ok" || status == "ready") {
        m_healthPollsRemaining = 0;
        setState(State::Ready);
    } else if (status == "error") {
        setState(State::Error);
        emit errorOccurred(obj.value("error").toString(tr("SAM server 未就绪")));
    } else if (m_process && m_healthPollsRemaining-- > 0) {
        setState(State::LoadingModel);
        QTimer::singleShot(500, this, &SamBackendClient::healthCheck);
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

    if (obj.value("status").toString() == "error") {
        setState(State::Error);
        emit errorOccurred(obj.value("error").toString(tr("setImage 失败")));
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

    const bool retryPrediction = m_imageRetryPending;
    m_embeddingId = id;
    const QString model = obj.value("model_name").toString();
    if (!model.isEmpty())
        m_modelName = model;
    m_imageRetryPending = false;
    setState(State::Ready);
    emit embeddingReady(id);
    stopTimeout();

    if (retryPrediction) {
        predict(m_lastPositive, m_lastNegative, m_lastBox);
    }
}

void SamBackendClient::onPredictReply() {
    // Fix 4: 检查请求序号，忽略过期的推理结果
    const qint64 replySeq = m_pendingPredictReply ? m_pendingPredictReply->property("predictSeq").toLongLong() : 0;
    if (replySeq > 0 && replySeq <= m_lastCompletedSeq) {
        stopTimeout();
        return;  // 过期结果，忽略
    }
    m_lastCompletedSeq = replySeq;

    QString err;
    QJsonObject obj = parseReply(m_pendingPredictReply, &err);
    if (!err.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("预测失败：%1").arg(err));
        stopTimeout();
        return;
    }

    const QString status = obj.value("status").toString();
    if (status == "error") {
        setState(State::Error);
        emit errorOccurred(obj.value("error").toString(tr("预测失败")));
        stopTimeout();
        return;
    }
    if (status == "invalid_embedding" && !m_imageRetryPending && !m_lastImagePath.isEmpty()) {
        m_imageRetryPending = true;
        Logger::instance().info(tr("收到 invalid_embedding，自动重新 setImage 并重试"), "SamBackend");
        setState(State::LoadingModel);
        stopTimeout();
        setImage(m_lastImagePath);
        return;
    }
    if (status == "invalid_embedding") {
        setState(State::Error);
        emit errorOccurred(tr("embedding 无效，重试失败"));
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
        bbox = QRectF(bboxArr.at(0).toDouble(), bboxArr.at(1).toDouble(), bboxArr.at(2).toDouble(),
                      bboxArr.at(3).toDouble());
    }

    double score = obj.value("score").toDouble(0.0);
    QString maskRle = obj.value("mask_rle").toString();
    const QString model = obj.value("model_name").toString();
    if (!model.isEmpty())
        m_modelName = model;

    // Decode optional mask_png_base64 into a QImage so the UI can render the actual mask.
    QImage maskImage;
    const QString maskPngB64 = obj.value("mask_png_base64").toString();
    if (!maskPngB64.isEmpty()) {
        const QByteArray pngData = QByteArray::fromBase64(maskPngB64.toLatin1());
        if (maskImage.loadFromData(pngData, "PNG"))
            maskImage = maskImage.convertToFormat(QImage::Format_ARGB32);
        else
            maskImage = QImage();
    }

    setState(State::Ready);
    emit predictionReady(polygon, bbox, score, maskRle, maskImage);
    stopTimeout();
}

void SamBackendClient::onUnloadReply() {
    // Fix P0-3: 保存要 unload 的 embedding ID，防止新 set_image 设置了新 ID 后被旧 unload 清除
    const QString unloadedId = m_pendingUnloadReply ? m_pendingUnloadReply->property("unloadEmbeddingId").toString() : QString();
    QString err;
    QJsonObject obj = parseReply(m_pendingUnloadReply, &err);
    Q_UNUSED(obj)
    if (!err.isEmpty()) {
        emit errorOccurred(tr("unloadImage 失败：%1").arg(err));
    }
    // 只在当前 embedding 仍然是 unload 的那个时才清除
    if (m_embeddingId == unloadedId) {
        m_embeddingId.clear();
        setState(State::NotStarted);
    }
    stopTimeout();
}

void SamBackendClient::onTimeout() {
    if (m_pendingHealthReply)
        m_pendingHealthReply->abort();
    if (m_pendingSetImageReply)
        m_pendingSetImageReply->abort();
    if (m_pendingPredictReply)
        m_pendingPredictReply->abort();
    if (m_pendingUnloadReply)
        m_pendingUnloadReply->abort();
    setState(State::Error);
    emit errorOccurred(tr("SAM 请求超时（30s）"));
}

} // namespace DeepLux
