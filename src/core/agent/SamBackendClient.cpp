#include "SamBackendClient.h"

#include "core/common/Logger.h"
#include "core/platform/PathUtils.h"
#ifdef DEEPLUX_HAS_ONNXRUNTIME
#include "SamNativeSettings.h"
#include "SamPredictor.h"
#include "core/common/CancellationToken.h"

#include <QThreadPool>
#include <chrono>
#include <future>
#include <memory>
#endif

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
#include <cmath>

namespace DeepLux {

namespace {
constexpr qint64 kMaxSamResponseBytes = 64LL * 1024 * 1024;
constexpr const char* kSamResponseTooLarge = "samResponseTooLarge";

void guardSamReplySize(QNetworkReply* reply) {
    auto abortIfTooLarge = [reply](qint64 received, qint64 total) {
        if (reply->property(kSamResponseTooLarge).toBool())
            return;
        if (received > kMaxSamResponseBytes || total > kMaxSamResponseBytes ||
            reply->bytesAvailable() > kMaxSamResponseBytes) {
            reply->setProperty(kSamResponseTooLarge, true);
            reply->abort();
        }
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, abortIfTooLarge]() {
        abortIfTooLarge(reply->bytesAvailable(), reply->header(QNetworkRequest::ContentLengthHeader).toLongLong());
    });
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, abortIfTooLarge);
    QObject::connect(reply, &QIODevice::readyRead, reply,
                     [reply, abortIfTooLarge]() { abortIfTooLarge(reply->bytesAvailable(), -1); });
}

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

#ifdef DEEPLUX_HAS_ONNXRUNTIME
struct SamBackendClient::NativeRequest {
    std::future<SamPredictOutcome> future;
    std::shared_ptr<CancellationToken> cancel;
};
#endif

SamBackendClient::SamBackendClient(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {
    m_timeoutTimer.setSingleShot(true);
    connect(&m_timeoutTimer, &QTimer::timeout, this, &SamBackendClient::onTimeout);
#ifdef DEEPLUX_HAS_ONNXRUNTIME
    m_nativePollTimer.setInterval(10);
    connect(&m_nativePollTimer, &QTimer::timeout, this, [this]() {
        if (!m_nativeRequest ||
            m_nativeRequest->future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return;
        SamPredictOutcome result;
        try {
            result = m_nativeRequest->future.get();
        } catch (const std::exception& e) {
            result.error = tr("原生 SAM 工作线程失败：%1").arg(e.what());
        }
        m_nativePollTimer.stop();
        m_nativeRequest.reset();
        if (!result.ok || result.previewCandidates.size() != 1 || result.previewCandidates.first().isEmpty()) {
            setState(State::Error);
            emit errorOccurred(result.error.isEmpty() ? tr("原生 SAM 未返回有效候选") : result.error);
            return;
        }
        m_lastCandidates = result.previewCandidates.first();
        m_modelName = result.modelName;
        m_nativeDevice = result.nativeDevice;
        setState(State::Ready);
        const SamCandidate& best = m_lastCandidates.first();
        emit predictionReady(best.polygon, best.bbox, best.score, best.maskRle, best.maskImage);
    });
#endif
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
    QStringList base = {QStringLiteral("fastapi"), QStringLiteral("uvicorn"), QStringLiteral("numpy"),
                        QStringLiteral("PIL")};
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
    m_timeoutTimer.setInterval(ms > 0 ? ms : m_timeoutMs);
    m_timeoutTimer.start();
}

void SamBackendClient::stopTimeout() {
    if (m_timeoutTimer.isActive())
        m_timeoutTimer.stop();
}

void SamBackendClient::settleReply(QPointer<QNetworkReply>& reply) {
    reply = nullptr;
    // 仍有其他请求在途（如 unload 之后紧跟的 setImage）时保持超时保护，
    // 防止先到的响应取消超时后，另一个请求永久挂起。
    if (m_pendingHealthReply || m_pendingSetImageReply || m_pendingPredictReply || m_pendingUnloadReply)
        return;
    stopTimeout();
}

void SamBackendClient::abortPendingReply(QPointer<QNetworkReply>& reply) {
    if (!reply)
        return;
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    reply = nullptr;
}

void SamBackendClient::startServerProcess() {
    if (m_serverUrl == QLatin1String("native")) {
#ifdef DEEPLUX_HAS_ONNXRUNTIME
        const SamNativeConfig config = SamNativeSettings::snapshot();
        if (!config.error.isEmpty()) {
            setState(State::Error);
            emit errorOccurred(config.error);
            return;
        }
        m_modelName = QStringLiteral("sam1_vit_b");
        setState(State::Ready);
#else
        setState(State::Error);
        emit errorOccurred(tr("此构建未启用原生 SAM 推理"));
#endif
        return;
    }
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
#ifdef DEEPLUX_HAS_ONNXRUNTIME
    if (m_nativeRequest)
        m_nativeRequest->cancel->cancel();
    m_nativePollTimer.stop();
    m_nativeRequest.reset();
#endif
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
    m_lastCandidates.clear();
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
#ifdef DEEPLUX_PLATFORM_WINDOWS
        const QStringList venvArgs{QStringLiteral("-m"), QStringLiteral("venv"), managedEnvironmentPath()};
#else
        // Linux/macOS 沿用无 pip 的 venv，随后由系统 pip3 安装依赖。
        const QStringList venvArgs{QStringLiteral("-m"), QStringLiteral("venv"), QStringLiteral("--without-pip"),
                                   managedEnvironmentPath()};
#endif
        m_envProcess->start(resolvedPythonPath(), venvArgs);
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
#ifdef DEEPLUX_PLATFORM_WINDOWS
        // Windows 通常没有 pip3，也没有独立的 pip 可执行文件；退回 `python -m pip`。
        const QString python = resolvedPythonPath();
        if (python.isEmpty() || QStandardPaths::findExecutable(python).isEmpty()) {
            finishEnvironmentInitialization(false, tr("系统缺少 Python/pip，无法安装 SAM 依赖"));
            return;
        }
        PathUtils::ensureDirExists(managedSitePackagesPath());
        emit environmentInitializationProgress(tr("SAM 环境：正在安装依赖，首次安装可能需要几分钟"));
        m_envProcess->start(python, QStringList{QStringLiteral("-m"), QStringLiteral("pip"), QStringLiteral("install"),
                                                QStringLiteral("--target"), managedSitePackagesPath(),
                                                QStringLiteral("-r"), requirementsPath()});
#else
        finishEnvironmentInitialization(false, tr("系统缺少 pip3，无法安装 SAM 依赖"));
#endif
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
    if (m_serverUrl == QLatin1String("native")) {
        startServerProcess();
        return;
    }
    // latest-wins：同类型请求重叠时取消旧请求，回调按序号只认最新请求
    abortPendingReply(m_pendingHealthReply);
    const qint64 thisSeq = ++m_healthSeq;
    setState(State::LoadingModel);
    QUrl url(m_serverUrl + "/health");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = m_nam->get(req);
    guardSamReplySize(reply);
    m_pendingHealthReply = reply;
    reply->setProperty("healthSeq", thisSeq);
    connect(reply, &QNetworkReply::finished, this, &SamBackendClient::onHealthReply);
    startTimeout();
}

void SamBackendClient::setImage(const QString& imagePath) {
    if (m_serverUrl == QLatin1String("native")) {
        cancelPendingPrediction();
        m_embeddingId.clear();
        m_lastCandidates.clear();
        m_lastImagePath = imagePath;
        if (!QFileInfo(imagePath).isFile()) {
            setState(State::Error);
            emit errorOccurred(tr("原生 SAM 图像不存在：%1").arg(imagePath));
            return;
        }
        m_embeddingId = imagePath;
        setState(State::Ready);
        emit embeddingReady(m_embeddingId);
        return;
    }
    // latest-wins：快速连续切换图像时取消上一个未完成的 set_image，
    // 避免旧响应覆盖新图的 embedding 或误删新请求。
    abortPendingReply(m_pendingSetImageReply);
    const qint64 thisSeq = ++m_setImageSeq;
    if (m_state == State::NotStarted || m_state == State::Error) {
        setState(State::LoadingModel);
    }
    m_lastImagePath = imagePath;

    QUrl url(m_serverUrl + "/set_image");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["image_path"] = imagePath;
    QNetworkReply* reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    guardSamReplySize(reply);
    m_pendingSetImageReply = reply;
    reply->setProperty("setImageSeq", thisSeq);
    connect(reply, &QNetworkReply::finished, this, &SamBackendClient::onSetImageReply);
    startTimeout();
}

void SamBackendClient::predict(const QList<QPointF>& positive, const QList<QPointF>& negative, const QRectF& box) {
    if (m_embeddingId.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("尚无 embedding，无法预测"));
        return;
    }

    if (m_serverUrl == QLatin1String("native")) {
#ifdef DEEPLUX_HAS_ONNXRUNTIME
        cancelPendingPrediction();
        m_lastCandidates.clear();
        auto request = std::make_unique<NativeRequest>();
        request->cancel = std::make_shared<CancellationToken>();
        SamPromptItem item;
        item.positivePoints = positive;
        item.negativePoints = negative;
        item.box = box;
        const QString imagePath = m_lastImagePath;
        const int timeoutMs = m_timeoutMs;
        const SamNativeConfig nativeConfig = SamNativeSettings::snapshot();
        const auto token = request->cancel;
        auto work = std::make_shared<std::packaged_task<SamPredictOutcome()>>(
            [imagePath, item, timeoutMs, token, nativeConfig] {
                return runSamPredictBatch(QStringLiteral("native"), imagePath, {item}, timeoutMs, token.get(), true,
                                          nativeConfig);
            });
        request->future = work->get_future();
        m_nativeRequest = std::move(request);
        QThreadPool* pool = samNativeThreadPool();
        if (!pool) {
            m_nativeRequest.reset();
            setState(State::Error);
            emit errorOccurred(tr("原生 SAM 只能从应用主线程启动"));
            return;
        }
        pool->start([work] { (*work)(); });
        setState(State::Busy);
        m_nativePollTimer.start();
#else
        setState(State::Error);
        emit errorOccurred(tr("此构建未启用原生 SAM 推理"));
#endif
        return;
    }

    cancelPendingPrediction();
    m_lastCandidates.clear(); // 批4（C）：候选与本次 predict 绑定，发起即清空

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
    guardSamReplySize(m_pendingPredictReply);
    m_pendingPredictReply->setProperty("predictSeq", thisSeq);
    connect(m_pendingPredictReply, &QNetworkReply::finished, this, &SamBackendClient::onPredictReply);
    startTimeout();
}

void SamBackendClient::cancelPendingPrediction() {
    if (m_serverUrl == QLatin1String("native")) {
#ifdef DEEPLUX_HAS_ONNXRUNTIME
        if (!m_nativeRequest)
            return;
        m_nativeRequest->cancel->cancel();
        m_nativePollTimer.stop();
        m_nativeRequest.reset();
        m_lastCandidates.clear();
        setState(m_embeddingId.isEmpty() ? State::NotStarted : State::Ready);
#endif
        return;
    }
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
    if (m_serverUrl == QLatin1String("native")) {
        cancelPendingPrediction();
        m_embeddingId.clear();
        m_lastImagePath.clear();
        m_lastCandidates.clear();
        setState(State::NotStarted);
        return;
    }
    if (m_embeddingId.isEmpty())
        return;
    // Fix P0-3: 取消任何挂起的 predict，防止 unload 和 predict 竞争
    cancelPendingPrediction();
    // latest-wins：取消同类型旧请求，回调按序号只认最新请求
    abortPendingReply(m_pendingUnloadReply);
    const qint64 thisSeq = ++m_unloadSeq;
    QUrl url(m_serverUrl + "/unload_image");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QJsonObject body;
    body["embedding_id"] = m_embeddingId;
    QNetworkReply* reply = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    guardSamReplySize(reply);
    m_pendingUnloadReply = reply;
    reply->setProperty("unloadSeq", thisSeq);
    // 记录 unload 的 embedding ID，回调时检查是否已被新 set_image 替换
    reply->setProperty("unloadEmbeddingId", m_embeddingId);
    connect(reply, &QNetworkReply::finished, this, &SamBackendClient::onUnloadReply);
    startTimeout();
}

// ===== SAM 批4（C）：candidates 字段严格解析（与顶层同口径：类型/有限值/
// score∈[0,1]；成功返回空串，失败返回错误信息，不静默转零）=====
namespace {

QString parseCandidatePolygon(const QJsonValue& value, QList<QPointF>& out) {
    if (!value.isArray())
        return QStringLiteral("polygon 必须为数组");
    const QJsonArray arr = value.toArray();
    for (const QJsonValue& v : arr) {
        if (!v.isArray() || v.toArray().size() != 2 || !v.toArray().at(0).isDouble() || !v.toArray().at(1).isDouble())
            return QStringLiteral("polygon 元素必须为 [x,y] 数值对");
        const QPointF point(v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble());
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
            return QStringLiteral("polygon 坐标必须为有限数值");
        out.append(point);
    }
    return QString();
}

QString parseCandidateBbox(const QJsonValue& value, QRectF& out) {
    out = QRectF();
    if (value.isUndefined() || value.isNull())
        return QString();
    if (!value.isArray() || value.toArray().size() != 4)
        return QStringLiteral("bbox 必须为 [x,y,w,h] 4 数值或缺省");
    const QJsonArray arr = value.toArray();
    double vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!arr.at(i).isDouble() || !std::isfinite(arr.at(i).toDouble()))
            return QStringLiteral("bbox 必须为有限数值");
        vals[i] = arr.at(i).toDouble();
    }
    out = QRectF(vals[0], vals[1], vals[2], vals[3]);
    return QString();
}

QString parseCandidateScore(const QJsonValue& value, double& out) {
    if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0.0 || value.toDouble() > 1.0)
        return QStringLiteral("score 必须为 [0,1] 有限数值");
    out = value.toDouble();
    return QString();
}

QString parseCandidateString(const QJsonValue& value, const QString& name, QString& out) {
    out.clear();
    if (value.isUndefined() || value.isNull())
        return QString();
    if (!value.isString())
        return QStringLiteral("%1 必须为字符串或缺省").arg(name);
    out = value.toString();
    return QString();
}

QString parseCandidateMaskPng(const QJsonValue& value, QImage& out) {
    out = QImage();
    if (value.isUndefined() || value.isNull())
        return QString();
    if (!value.isString())
        return QStringLiteral("mask_png_base64 必须为字符串或缺省");
    const QString b64 = value.toString();
    if (!b64.isEmpty()) {
        const QByteArray pngData = QByteArray::fromBase64(b64.toLatin1());
        if (out.loadFromData(pngData, "PNG"))
            out = out.convertToFormat(QImage::Format_ARGB32);
        else
            out = QImage(); // 解码失败降级为无掩膜像素（仅显示辅助，不参与契约）
    }
    return QString();
}

} // namespace

static QJsonObject parseReply(QNetworkReply* reply, QString* err) {
    QJsonObject result;
    if (!reply) {
        if (err)
            *err = QStringLiteral("空回复");
        return result;
    }
    reply->deleteLater();
    if (reply->property(kSamResponseTooLarge).toBool()) {
        if (err)
            *err = QStringLiteral("SAM 响应超过尺寸上限（64MB）");
        return result;
    }
    if (reply->error() != QNetworkReply::NoError) {
        if (err)
            *err = reply->errorString();
        return result;
    }
    // 阶7 Agent 评审修复（P3-11）：响应尺寸上限，防止故障/恶意后端以超大响应耗尽内存
    if (reply->size() > kMaxSamResponseBytes) {
        if (err)
            *err = QStringLiteral("SAM 响应超过尺寸上限（64MB）");
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
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    // latest-wins：仅处理当前最新请求的响应，忽略被新请求取代/中止的过期回调
    if (reply != m_pendingHealthReply || reply->property("healthSeq").toLongLong() != m_healthSeq) {
        reply->deleteLater();
        return;
    }
    QString err;
    QJsonObject obj = parseReply(reply, &err);
    if (!err.isEmpty()) {
        if (m_process && m_healthPollsRemaining-- > 0) {
            settleReply(m_pendingHealthReply);
            QTimer::singleShot(500, this, &SamBackendClient::healthCheck);
            return;
        }
        setState(State::Error);
        emit errorOccurred(tr("健康检查失败：%1").arg(err));
        settleReply(m_pendingHealthReply);
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
    settleReply(m_pendingHealthReply);
}

void SamBackendClient::onSetImageReply() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    // latest-wins：仅处理当前最新 set_image 的响应，忽略过期/中止的回调
    if (reply != m_pendingSetImageReply || reply->property("setImageSeq").toLongLong() != m_setImageSeq) {
        reply->deleteLater();
        return;
    }
    QString err;
    QJsonObject obj = parseReply(reply, &err);
    if (!err.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("setImage 失败：%1").arg(err));
        settleReply(m_pendingSetImageReply);
        return;
    }

    if (obj.value("status").toString() == "error") {
        setState(State::Error);
        emit errorOccurred(obj.value("error").toString(tr("setImage 失败")));
        settleReply(m_pendingSetImageReply);
        return;
    }

    QString id = obj.value("embedding_id").toString();
    if (id.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("setImage 响应缺少 embedding_id"));
        settleReply(m_pendingSetImageReply);
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
    settleReply(m_pendingSetImageReply);

    if (retryPrediction) {
        predict(m_lastPositive, m_lastNegative, m_lastBox);
    }
}

void SamBackendClient::onPredictReply() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    // Fix 4 + latest-wins：只处理当前最新 predict 的响应；过期的推理结果忽略
    const qint64 replySeq = reply->property("predictSeq").toLongLong();
    if (reply != m_pendingPredictReply || (replySeq > 0 && replySeq <= m_lastCompletedSeq)) {
        // 过期回复到达（已有更新的 predict 在途）：只清理本条过期回复，
        // 不停止在途请求的超时保护。
        reply->deleteLater();
        return;
    }
    m_lastCompletedSeq = replySeq;

    QString err;
    QJsonObject obj = parseReply(reply, &err);
    if (!err.isEmpty()) {
        setState(State::Error);
        emit errorOccurred(tr("预测失败：%1").arg(err));
        settleReply(m_pendingPredictReply);
        return;
    }

    // 批3 复核二轮（P1-3）：响应严格失败关闭。真实 sam_server 成功响应不带
    // status 字段——"缺失 status"仅进入下方数据字段严格校验；显式失败标记
    // （status=error / ok=false / 非空 error 字段）与未知 status 均按错误处理，
    // 字段类型/长度/有限值/score 范围逐项校验，杜绝 toArray()/toDouble() 把
    // 字符串、布尔值或缺失字段静默转零后"三个错误坐标成为有效多边形"。
    const auto reject = [this](const QString& message) {
        setState(State::Error);
        emit errorOccurred(message);
        settleReply(m_pendingPredictReply);
    };

    // 批3 复核三轮（P2-4）+ 四轮：status/ok/error 字段存在即强制类型——
    // status:true 经 toString() 得空串会被当"字段缺失"，带合法 polygon
    // 即误发 predictionReady；显式 null 同样不是缺省（只有字段不存在才是），
    // 错误类型的显式失败标记不得被静默忽略
    const QJsonValue statusValue = obj.value("status");
    if (!statusValue.isUndefined() && !statusValue.isString()) {
        reject(tr("预测失败：status 必须为字符串或不存在"));
        return;
    }
    const QString status = statusValue.toString();
    if (status == QLatin1String("error")) {
        reject(obj.value("error").toString(tr("预测失败")));
        return;
    }
    if (status == QLatin1String("invalid_embedding") && !m_imageRetryPending && !m_lastImagePath.isEmpty()) {
        m_imageRetryPending = true;
        Logger::instance().info(tr("收到 invalid_embedding，自动重新 setImage 并重试"), "SamBackend");
        setState(State::LoadingModel);
        settleReply(m_pendingPredictReply);
        setImage(m_lastImagePath);
        return;
    }
    if (status == QLatin1String("invalid_embedding")) {
        reject(tr("embedding 无效，重试失败"));
        return;
    }
    if (!status.isEmpty() && status != QLatin1String("ok")) {
        reject(tr("预测失败：未知状态 %1").arg(status));
        return;
    }
    const QJsonValue okValue = obj.value("ok");
    if (!okValue.isUndefined() && !okValue.isBool()) {
        reject(tr("预测失败：ok 必须为布尔或不存在"));
        return;
    }
    if (okValue.isBool() && !okValue.toBool()) {
        reject(obj.value("error").toString(tr("预测失败：后端返回 ok=false")));
        return;
    }
    const QJsonValue errorValue = obj.value("error");
    if (!errorValue.isUndefined() && !errorValue.isString()) {
        reject(tr("预测失败：error 字段必须为字符串或不存在"));
        return;
    }
    if (errorValue.isString() && !errorValue.toString().isEmpty()) {
        reject(tr("预测失败：%1").arg(errorValue.toString()));
        return;
    }

    const QJsonValue polygonValue = obj.value("polygon");
    if (!polygonValue.isArray()) {
        reject(tr("预测失败：polygon 必须为数组"));
        return;
    }
    QList<QPointF> polygon;
    const QJsonArray polyArr = polygonValue.toArray();
    for (const QJsonValue& v : polyArr) {
        if (!v.isArray() || v.toArray().size() != 2 || !v.toArray().at(0).isDouble() || !v.toArray().at(1).isDouble()) {
            reject(tr("预测失败：polygon 元素必须为 [x,y] 数值对"));
            return;
        }
        const QPointF point(v.toArray().at(0).toDouble(), v.toArray().at(1).toDouble());
        if (!std::isfinite(point.x()) || !std::isfinite(point.y())) {
            reject(tr("预测失败：polygon 坐标必须为有限数值"));
            return;
        }
        polygon.append(point);
    }

    QRectF bbox;
    const QJsonValue bboxValue = obj.value("bbox");
    if (!bboxValue.isUndefined() && !bboxValue.isNull()) {
        if (!bboxValue.isArray() || bboxValue.toArray().size() != 4) {
            reject(tr("预测失败：bbox 必须为 [x,y,w,h] 4 数值或缺省"));
            return;
        }
        const QJsonArray bboxArr = bboxValue.toArray();
        double vals[4];
        for (int i = 0; i < 4; ++i) {
            if (!bboxArr.at(i).isDouble() || !std::isfinite(bboxArr.at(i).toDouble())) {
                reject(tr("预测失败：bbox 必须为有限数值"));
                return;
            }
            vals[i] = bboxArr.at(i).toDouble();
        }
        bbox = QRectF(vals[0], vals[1], vals[2], vals[3]);
    }

    const QJsonValue scoreValue = obj.value("score");
    if (!scoreValue.isDouble() || !std::isfinite(scoreValue.toDouble()) || scoreValue.toDouble() < 0.0 ||
        scoreValue.toDouble() > 1.0) {
        reject(tr("预测失败：score 必须为 [0,1] 有限数值"));
        return;
    }
    const double score = scoreValue.toDouble();

    const QJsonValue rleValue = obj.value("mask_rle");
    if (!rleValue.isUndefined() && !rleValue.isNull() && !rleValue.isString()) {
        reject(tr("预测失败：mask_rle 必须为字符串或缺省"));
        return;
    }
    const QString maskRle = rleValue.toString();

    const QJsonValue modelValue = obj.value("model_name");
    if (!modelValue.isUndefined() && !modelValue.isNull() && !modelValue.isString()) {
        reject(tr("预测失败：model_name 必须为字符串或缺省"));
        return;
    }
    const QString model = modelValue.toString();
    if (!model.isEmpty())
        m_modelName = model;

    // mask_png_base64：类型严格校验；base64/PNG 解码失败降级为无掩膜像素
    // （仅显示辅助，不参与区域契约，不值得失败关闭整个预测）
    QImage maskImage;
    const QJsonValue pngValue = obj.value("mask_png_base64");
    if (!pngValue.isUndefined() && !pngValue.isNull()) {
        if (!pngValue.isString()) {
            reject(tr("预测失败：mask_png_base64 必须为字符串或缺省"));
            return;
        }
        const QString maskPngB64 = pngValue.toString();
        if (!maskPngB64.isEmpty()) {
            const QByteArray pngData = QByteArray::fromBase64(maskPngB64.toLatin1());
            if (maskImage.loadFromData(pngData, "PNG"))
                maskImage = maskImage.convertToFormat(QImage::Format_ARGB32);
            else
                maskImage = QImage();
        }
    }

    // 批4（C）：candidates 可选数组（multimask 候选，score 降序）——逐元素
    // 严格解析（与顶层同口径），上限 8 防滥用；在 predictionReady 发出前
    // 存入 m_lastCandidates，回调内经 lastCandidates() 读取即为本次结果
    m_lastCandidates.clear();
    const QJsonValue candValue = obj.value("candidates");
    if (!candValue.isUndefined() && !candValue.isNull()) {
        if (!candValue.isArray()) {
            reject(tr("预测失败：candidates 必须为数组或缺省"));
            return;
        }
        const QJsonArray candArr = candValue.toArray();
        if (candArr.size() > 8) {
            reject(tr("预测失败：candidates 数量超过上限 8"));
            return;
        }
        for (int i = 0; i < candArr.size(); ++i) {
            if (!candArr[i].isObject()) {
                reject(tr("预测失败：candidates[%1] 必须为对象").arg(i));
                return;
            }
            const QJsonObject cobj = candArr[i].toObject();
            SamCandidate candidate;
            QString candidateError = parseCandidatePolygon(cobj.value("polygon"), candidate.polygon);
            if (candidateError.isEmpty())
                candidateError = parseCandidateBbox(cobj.value("bbox"), candidate.bbox);
            if (candidateError.isEmpty())
                candidateError = parseCandidateScore(cobj.value("score"), candidate.score);
            if (candidateError.isEmpty())
                candidateError =
                    parseCandidateString(cobj.value("mask_rle"), QStringLiteral("mask_rle"), candidate.maskRle);
            if (candidateError.isEmpty())
                candidateError = parseCandidateMaskPng(cobj.value("mask_png_base64"), candidate.maskImage);
            if (!candidateError.isEmpty()) {
                reject(tr("预测失败：candidates[%1] %2").arg(i).arg(candidateError));
                return;
            }
            m_lastCandidates.append(candidate);
        }
    }

    setState(State::Ready);
    emit predictionReady(polygon, bbox, score, maskRle, maskImage);
    settleReply(m_pendingPredictReply);
}

void SamBackendClient::onUnloadReply() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    // latest-wins：只处理当前最新 unload 的响应，忽略过期/中止的回调
    if (reply != m_pendingUnloadReply || reply->property("unloadSeq").toLongLong() != m_unloadSeq) {
        reply->deleteLater();
        return;
    }
    // Fix P0-3: 保存要 unload 的 embedding ID，防止新 set_image 设置了新 ID 后被旧 unload 清除
    const QString unloadedId = reply->property("unloadEmbeddingId").toString();
    QString err;
    QJsonObject obj = parseReply(reply, &err);
    Q_UNUSED(obj)
    if (!err.isEmpty()) {
        emit errorOccurred(tr("unloadImage 失败：%1").arg(err));
    }
    // 只在当前 embedding 仍然是 unload 的那个时才清除
    if (m_embeddingId == unloadedId) {
        m_embeddingId.clear();
        setState(State::NotStarted);
    }
    settleReply(m_pendingUnloadReply);
}

void SamBackendClient::onTimeout() {
    // 先断开连接再中止：避免被中止回复的回调在超时之后再次触发错误信号，
    // 保证一次超时只报告一条错误。
    const auto abortPending = [this](QPointer<QNetworkReply>& reply) {
        if (!reply)
            return;
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        reply = nullptr;
    };
    abortPending(m_pendingHealthReply);
    abortPending(m_pendingSetImageReply);
    abortPending(m_pendingPredictReply);
    abortPending(m_pendingUnloadReply);
    setState(State::Error);
    emit errorOccurred(tr("SAM 请求超时（%1ms）").arg(m_timeoutMs));
}

} // namespace DeepLux
