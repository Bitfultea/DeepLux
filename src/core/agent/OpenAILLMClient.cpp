#include "OpenAILLMClient.h"

#include "FailedRequestLog.h"
#include "ToolSchema.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUrl>

namespace DeepLux {

namespace {
constexpr int kMaxErrorDetailChars = 2048;

QString truncateDetail(const QString& detail) {
    return detail.size() > kMaxErrorDetailChars ? detail.left(kMaxErrorDetailChars) : detail;
}

// 审计用原始错误串：状态码 + Qt errorString + 响应体（截断 2KB）
QString describeDetail(int httpStatus, const QString& errorString, const QByteArray& body) {
    QString detail = httpStatus > 0 ? QStringLiteral("HTTP %1").arg(httpStatus) : QStringLiteral("network");
    if (!errorString.isEmpty())
        detail += QStringLiteral(": ") + errorString;
    if (!body.isEmpty())
        detail += QStringLiteral(" | body: ") + QString::fromUtf8(body);
    return truncateDetail(detail);
}

bool looksLikeContextLimit(const QString& bodyText) {
    static const QRegularExpression re(
        QStringLiteral("context[ _-]?length|too many tokens|maximum context|reduce the length|token limit"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(bodyText).hasMatch();
}

bool looksLikeVisionRejection(const QString& bodyText) {
    static const QRegularExpression re(
        QStringLiteral("image_url|content.*type|vision|multimodal|does not support image"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(bodyText).hasMatch();
}
} // namespace

OpenAILLMClient::OpenAILLMClient(QObject* parent)
    : ILLMClient(parent), m_networkManager(new QNetworkAccessManager(this)), m_retryTimer(new QTimer(this)),
      m_streamIdleTimer(new QTimer(this)), m_streamDeadlineTimer(new QTimer(this)) {
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        if (m_aborted)
            return;
        qDebug() << "[OpenAILLMClient] retrying request, attempt" << (m_retryAttempt + 1);
        postRequestBody(m_requestStreaming);
    });
    // 流式看门狗（设计 §3.6）
    m_streamIdleTimer->setSingleShot(true);
    m_streamDeadlineTimer->setSingleShot(true);
    connect(m_streamIdleTimer, &QTimer::timeout, this, &OpenAILLMClient::onStreamIdleTimeout);
    connect(m_streamDeadlineTimer, &QTimer::timeout, this, &OpenAILLMClient::onStreamDeadlineTimeout);
}

OpenAILLMClient::~OpenAILLMClient() {
    m_aborted = true;
    if (m_retryTimer)
        m_retryTimer->stop();
    stopStreamTimers();
    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
}

void OpenAILLMClient::setStreamIdleTimeoutMs(int ms) {
    m_streamIdleTimeoutMs = qBound(5000, ms, 600000);
}

void OpenAILLMClient::setStreamMaxDurationMs(int ms) {
    m_streamMaxDurationMs = qBound(60000, ms, 3600000);
}

void OpenAILLMClient::setApiKey(const QString& key) {
    m_apiKey = key;
}

void OpenAILLMClient::setEndpoint(const QString& url) {
    m_endpoint = normalizeEndpoint(url);
}

QString OpenAILLMClient::normalizeEndpoint(const QString& endpoint) {
    QString normalized = endpoint.trimmed();
    if (!normalized.startsWith("http://") && !normalized.startsWith("https://"))
        normalized.prepend("https://");

    QUrl parsed(normalized);
    QString path = parsed.path();
    if (path.isEmpty() || path == "/") {
        parsed.setPath("/v1/chat/completions");
        normalized = parsed.toString();
    }
    return normalized;
}

void OpenAILLMClient::setModel(const QString& model) {
    m_model = model;
}
void OpenAILLMClient::setTemperature(double temp) {
    m_temperature = temp;
}
void OpenAILLMClient::setMaxTokens(int tokens) {
    m_maxTokens = tokens;
}
void OpenAILLMClient::setToolsEnabled(bool enabled) {
    m_toolsEnabled = enabled;
}
void OpenAILLMClient::setVisionEnabled(bool enabled) {
    m_visionEnabled = enabled;
    if (enabled) {
        // 用户显式重新开启视觉能力时清除会话级降级标记
        m_visionDowngraded = false;
    }
}

bool OpenAILLMClient::heuristicSupportsVision(const QString& model) {
    // 设计 §7.3：显式配置优先；本启发式只在配置缺省时使用
    static const QRegularExpression visionPattern(
        QStringLiteral("gpt-4o|gpt-4\\.1|claude-3|claude-4|gemini|qwen-vl|qwen2\\.5-vl|glm-4v|internvl|llava|"
                       "pixtral"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression textOnlyPattern(QStringLiteral("deepseek-(chat|reasoner)|o1-mini|text-davinci"),
                                                    QRegularExpression::CaseInsensitiveOption);
    const QString name = model.trimmed();
    if (visionPattern.match(name).hasMatch())
        return true;
    if (textOnlyPattern.match(name).hasMatch())
        return false;
    return true; // 保守开放：未知模型先按支持处理，靠 400 自动降级兜底
}
void OpenAILLMClient::setReasoningEffort(const QString& effort) {
    m_reasoningEffort = effort;
}
void OpenAILLMClient::setThinkingEnabled(bool enabled) {
    m_thinkingEnabled = enabled;
}

// ========== 错误分类与重试策略（唯一实现，§7.2/§7.3） ==========

LLMErrorInfo OpenAILLMClient::classifyError(int httpStatus, QNetworkReply::NetworkError networkError,
                                            const QString& errorString, const QByteArray& body) {
    LLMErrorInfo info;
    info.httpStatus = httpStatus;
    info.detail = describeDetail(httpStatus, errorString, body);
    const QString bodyText = QString::fromUtf8(body);

    // 1) HTTP 状态优先：4xx/5xx 的语义比 Qt 的 ContentAccessDenied 等更精确
    if (httpStatus == 401 || httpStatus == 403) {
        info.category = LLMErrorCategory::Auth;
        info.retryable = false;
        info.message =
            QStringLiteral("Authentication failed (%1). Check the API key and its permissions.").arg(httpStatus);
        return info;
    }
    if (httpStatus == 429) {
        info.category = LLMErrorCategory::RateLimit;
        info.retryable = true;
        info.message = QStringLiteral("Rate limit or quota exceeded (429). Retrying…");
        return info;
    }
    if (httpStatus >= 500 && httpStatus <= 599) {
        info.category = LLMErrorCategory::Server;
        info.retryable = true;
        info.message = QStringLiteral("The model service returned a server error (%1). Retrying…").arg(httpStatus);
        return info;
    }
    if (httpStatus == 400 || httpStatus == 413 || httpStatus == 422) {
        info.category = LLMErrorCategory::BadRequest;
        info.retryable = false;
        info.contextLimitSuspected = looksLikeContextLimit(bodyText);
        info.visionDowngradeHint = looksLikeVisionRejection(bodyText);
        info.message = QStringLiteral("The request was rejected (%1). ").arg(httpStatus);
        if (info.contextLimitSuspected) {
            info.message += QStringLiteral(
                "The conversation may exceed the model's context window; clear the conversation or shorten the "
                "request.");
        } else if (info.visionDowngradeHint) {
            info.message += QStringLiteral("The model may not support image input; retry without images.");
        } else {
            info.message += QStringLiteral("Check the request parameters.");
        }
        return info;
    }
    if (httpStatus >= 400) {
        info.category = LLMErrorCategory::BadRequest;
        info.retryable = false;
        info.message = QStringLiteral("The request was rejected (%1). Check the request parameters.").arg(httpStatus);
        return info;
    }

    // 2) 网络层错误
    if (networkError != QNetworkReply::NoError) {
        const bool timedOut = networkError == QNetworkReply::TimeoutError ||
                              networkError == QNetworkReply::OperationCanceledError ||
                              errorString.contains(QStringLiteral("timed out"), Qt::CaseInsensitive);
        if (timedOut) {
            info.category = LLMErrorCategory::Timeout;
            info.message = QStringLiteral("The model service did not respond in time.");
        } else {
            info.category = LLMErrorCategory::Network;
            info.message =
                QStringLiteral("Network error while contacting the model service. Check connectivity/proxy.");
        }
        info.retryable = true;
        return info;
    }

    // 3) 兜底：HTTP 200 但响应无法解析（JSON 非法 / 缺 choices / 空响应体）
    info.category = LLMErrorCategory::Malformed;
    info.retryable = false;
    info.message = QStringLiteral("The model service returned an invalid response. This is usually a temporary "
                                  "gateway issue.");
    return info;
}

int OpenAILLMClient::maxRetriesFor(LLMErrorCategory category) {
    switch (category) {
    case LLMErrorCategory::RateLimit:
    case LLMErrorCategory::Server:
        return 3;
    case LLMErrorCategory::Network:
        return 2;
    case LLMErrorCategory::Timeout:
        return 1;
    case LLMErrorCategory::None:
    case LLMErrorCategory::Auth:
    case LLMErrorCategory::BadRequest:
    case LLMErrorCategory::Malformed:
        break;
    }
    return 0;
}

int OpenAILLMClient::retryDelayMs(LLMErrorCategory category, int retryIndex) {
    if (retryIndex <= 0)
        return 0;
    switch (category) {
    case LLMErrorCategory::RateLimit: {
        static const int delays[] = {500, 1000, 2000};
        return delays[qBound(1, retryIndex, 3) - 1];
    }
    case LLMErrorCategory::Server: {
        static const int delays[] = {1000, 2000, 4000};
        return delays[qBound(1, retryIndex, 3) - 1];
    }
    case LLMErrorCategory::Network: {
        static const int delays[] = {1000, 3000};
        return delays[qBound(1, retryIndex, 2) - 1];
    }
    case LLMErrorCategory::Timeout:
        return 2000;
    case LLMErrorCategory::None:
    case LLMErrorCategory::Auth:
    case LLMErrorCategory::BadRequest:
    case LLMErrorCategory::Malformed:
        break;
    }
    return 0;
}

// ========== 请求发送 ==========

QJsonObject OpenAILLMClient::buildRequestBody(const AgentConversation& ctx, const QList<ToolDefinition>& tools) const {
    QJsonObject body;
    body["model"] = m_model;
    body["temperature"] = m_temperature;
    body["max_tokens"] = m_maxTokens;
    body["messages"] = ctx.toOpenAIMessages(m_reasoningEcho);

    if (m_toolsEnabled && !tools.isEmpty()) {
        QJsonArray toolsArray;
        for (const ToolDefinition& t : tools) {
            toolsArray.append(t.toOpenAIFunction());
        }
        body["tools"] = toolsArray;
        body["tool_choice"] = "auto";
    }

    // 流式请求形状（设计 §3.1）：回退过或关闭流式时绝不带 stream 键
    if (m_streamEnabled && !m_streamFallbackUsed) {
        body["stream"] = true;
        if (m_streamIncludeUsage)
            body["stream_options"] = QJsonObject{{"include_usage", true}};
    }

    // DeepSeek thinking mode
    if (!m_reasoningEffort.isEmpty()) {
        body["reasoning_effort"] = m_reasoningEffort;
    }
    if (m_thinkingEnabled) {
        body["thinking"] = QJsonObject{{"type", "enabled"}};
    }

    return body;
}

void OpenAILLMClient::sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) {
    // 中止上一个请求，防止信号串联（Agent 闭环中连续发请求时）
    releaseCurrentReply();
    // 新请求取消一切待触发的重试并重置重试状态
    m_retryTimer->stop();
    m_aborted = false;
    m_retryAttempt = 0;
    m_retryCategory = LLMErrorCategory::None;
    resetStreamStateForRequest();
    m_requestStreaming = m_streamEnabled;

    QJsonDocument doc(buildRequestBody(ctx, tools));
    m_lastBody = doc.toJson();
    m_lastRequestMessages = ctx.messages;
    m_lastTools = tools;
    postRequestBody(m_requestStreaming);
}

void OpenAILLMClient::postRequestBody(bool streaming) {
    m_requestStreaming = streaming;
    if (streaming)
        resetStreamAccumulators();

    QUrl url(m_endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Connection", "keep-alive");
    if (streaming) {
        request.setRawHeader("Accept", "text/event-stream");
        request.setTransferTimeout(0); // 流式：整段超时交给看门狗（§3.6）
    } else {
        request.setTransferTimeout(30000); // 非流式保持第一迭代行为
    }

    qDebug() << "OpenAILLMClient: Sending request to"
             << url.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment) << "model=" << m_model
             << "bytes=" << m_lastBody.size() << "tools=" << m_lastTools.size() << "stream=" << streaming;

    m_currentReply = m_networkManager->post(request, m_lastBody);
    connect(m_currentReply, &QNetworkReply::finished, this, &OpenAILLMClient::onReplyFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred, this, &OpenAILLMClient::onNetworkError);
    if (streaming) {
        connect(m_currentReply, &QNetworkReply::readyRead, this, &OpenAILLMClient::onStreamReadyRead);
        startStreamTimers();
    }
}

void OpenAILLMClient::abortRequest() {
    m_aborted = true;
    // 主动中止：禁止迟到回调再发终态信号
    m_streamFinalized = true;
    if (m_retryTimer)
        m_retryTimer->stop();
    stopStreamTimers();
    releaseCurrentReply();
    m_retryAttempt = 0;
    m_retryCategory = LLMErrorCategory::None;
    m_sseParser.reset();
    m_pendingStreamBytes.clear();
    m_jsonBody.clear();
    m_contentTypeKnown = false;
    m_contentTypeIsSse = false;
}

void OpenAILLMClient::releaseCurrentReply() {
    if (!m_currentReply)
        return;
    disconnect(m_currentReply, nullptr, this, nullptr);
    m_currentReply->abort();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
}

// ========== 响应处理 ==========

void OpenAILLMClient::onReplyFinished() {
    // 主动中止的请求不产生任何错误/重试（§7.3 第 5 条）
    if (m_aborted || !m_currentReply)
        return;

    QNetworkReply* reply = m_currentReply;
    m_currentReply = nullptr;

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QNetworkReply::NetworkError networkError = reply->error();
    const QString errorString = reply->errorString();
    QByteArray data = reply->readAll();
    reply->deleteLater();

    if (m_requestStreaming) {
        // Content-Type 判定前缓存的字节必须并入尾包，避免丢事件
        if (!m_pendingStreamBytes.isEmpty()) {
            data = m_pendingStreamBytes + data;
            m_pendingStreamBytes.clear();
        }
        if (networkError != QNetworkReply::NoError || httpStatus >= 400) {
            if (data.isEmpty())
                data = m_jsonBody;
            if (!m_sawDelta && isStreamUnsupportedResponse(httpStatus, data) &&
                tryStreamFallback(QStringLiteral("server does not support streaming"))) {
                return;
            }
            stopStreamTimers();
            handleFailure(classifyError(httpStatus, networkError, errorString, data));
            return;
        }

        if (!m_contentTypeKnown) {
            const QByteArray sniff = (!data.isEmpty() ? data : m_jsonBody).trimmed();
            m_contentTypeKnown = true;
            m_contentTypeIsSse = !looksLikeJsonBody(sniff);
        }

        if (!m_contentTypeIsSse) {
            // F2：网关忽略 stream，返回 application/json 整包 —— 不重发
            m_jsonBody += data;
            if (!m_sawDelta && !m_streamFallbackUsed) {
                m_streamFallbackUsed = true;
                emit streamFallback(QStringLiteral("server returned non-stream body"));
            }
            m_streamFinalized = true;
            stopStreamTimers();
            parseResponse(m_jsonBody);
            return;
        }

        if (!data.isEmpty())
            processStreamBytes(data);
        if (m_streamFinalized)
            return;
        if (m_streamEventCount > 0 || m_sawDelta) {
            finalizeStream();
            return;
        }
        if (tryStreamFallback(QStringLiteral("empty stream")))
            return;
        stopStreamTimers();
        handleFailure(classifyError(0, QNetworkReply::NoError, QStringLiteral("Empty response from LLM"), {}));
        return;
    }

    // ---- 非流式路径（第一迭代行为不变）----
    if (networkError != QNetworkReply::NoError || httpStatus >= 400) {
        handleFailure(classifyError(httpStatus, networkError, errorString, data));
        return;
    }

    if (data.isEmpty()) {
        handleFailure(classifyError(0, QNetworkReply::NoError, QStringLiteral("Empty response from LLM"), data));
        return;
    }

    parseResponse(data);
}

void OpenAILLMClient::onNetworkError(QNetworkReply::NetworkError error) {
    if (m_aborted || !m_currentReply)
        return;

    // finished() 一般随后到达，这里先消费掉 reply 并通过 m_currentReply=nullptr
    // 让 onReplyFinished 直接返回，避免同一次失败被处理两次。
    QNetworkReply* reply = m_currentReply;
    m_currentReply = nullptr;
    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString errorString = reply->errorString();
    QByteArray body = reply->readAll();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();

    if (m_requestStreaming) {
        // readyRead 可能已消费错误体；优先用累积缓冲做分类/回退判定
        if (body.isEmpty())
            body = m_jsonBody;
        else
            m_jsonBody = body;
        if (!m_sawDelta) {
            if (isStreamUnsupportedResponse(httpStatus, body) &&
                tryStreamFallback(QStringLiteral("server does not support streaming"))) {
                return;
            }
            // F4：首事件前连接被关闭（无任何 SSE 事件）→ 回退一次性请求
            if (httpStatus == 200 && m_streamEventCount == 0 &&
                tryStreamFallback(QStringLiteral("stream closed before first event"))) {
                return;
            }
        }
        stopStreamTimers();
    }

    handleFailure(classifyError(httpStatus, error, errorString, body));
}

void OpenAILLMClient::handleFailure(const LLMErrorInfo& info) {
    if (m_aborted)
        return;

    // R5（设计 §6.6）：服务端明确拒绝图片输入后，本客户端会话内不再声明视觉能力，
    // 避免每张图片都先被拒再走一次"去图重发"。
    if (info.visionDowngradeHint && !m_visionDowngraded) {
        m_visionDowngraded = true;
        qWarning() << "[OpenAILLMClient] model rejected image input; vision disabled for this client session";
    }

    // §6.2：已收到任意 delta 后失败 —— 不重试、不回退，带分类错误收口
    if (m_requestStreaming && m_sawDelta) {
        failStream(info);
        return;
    }
    // F4：首 delta 前连续 3 次解析失败 → 回退一次性请求（只允许一次）
    if (m_requestStreaming && !m_sawDelta && info.category == LLMErrorCategory::Malformed && m_streamParseErrors >= 3) {
        if (tryStreamFallback(QStringLiteral("invalid stream data")))
            return;
    }

    LLMErrorInfo failure = info;
    failure.attempts = m_retryAttempt + 1; // 含首次
    const int maxRetries = info.retryable ? maxRetriesFor(info.category) : 0;

    if (failure.retryable && m_retryAttempt < maxRetries) {
        ++m_retryAttempt;
        m_retryCategory = info.category;
        const int delayMs = retryDelayMs(info.category, m_retryAttempt);
        qWarning() << "[OpenAILLMClient] transient failure" << llmErrorCategoryName(info.category) << "retry"
                   << m_retryAttempt << "/" << maxRetries << "in" << delayMs << "ms";
        emit retryScheduled(failure, m_retryAttempt, delayMs);
        m_retryTimer->start(delayMs);
        return;
    }

    qWarning() << "[OpenAILLMClient] request failed" << llmErrorCategoryName(info.category)
               << "httpStatus=" << info.httpStatus << "attempts=" << failure.attempts;
    // R5：流式请求在首 delta 前的最终失败必须终止流，避免后续 readyRead/finished
    // 再产出 responseReceived（首个事件即顶层 error 的场景）。
    if (m_requestStreaming) {
        m_streamFinalized = true;
        stopStreamTimers();
        releaseCurrentReply();
    }
    recordFailedRequestIfNeeded(failure);
    emit errorOccurredClassified(failure);
    emit errorOccurred(failure.message);
}

void OpenAILLMClient::recordFailedRequestIfNeeded(const LLMErrorInfo& info) {
    // 设计 §4.1：只记录需要复现的持久性失败；Auth/瞬时/中止一律不落盘
    if (info.category != LLMErrorCategory::BadRequest && info.category != LLMErrorCategory::Malformed)
        return;
    FailedRequestLog::instance().record(info, m_lastBody, m_endpoint, m_model, m_lastRequestMessages);
}

// ========== 流式实装（设计 §3/§4/§6） ==========

void OpenAILLMClient::resetStreamStateForRequest() {
    m_streamFallbackUsed = false;
    resetStreamAccumulators();
}

void OpenAILLMClient::resetStreamAccumulators() {
    m_streamFinalized = false;
    m_streamStartedEmitted = false;
    m_sawDelta = false;
    m_streamParseErrors = 0;
    m_streamEventCount = 0;
    m_streamContent.clear();
    m_streamReasoning.clear();
    m_finishReason.clear();
    m_streamPromptTokens = 0;
    m_streamCompletionTokens = 0;
    m_toolCallAcc.clear();
    m_droppedToolCalls = 0;
    m_sseParser.reset();
    m_contentTypeKnown = false;
    m_contentTypeIsSse = false;
    m_pendingStreamBytes.clear();
    m_jsonBody.clear();
}

void OpenAILLMClient::onStreamReadyRead() {
    if (m_aborted || !m_currentReply || m_streamFinalized)
        return;
    const QByteArray chunk = m_currentReply->readAll();
    if (chunk.isEmpty())
        return;
    refreshStreamIdleTimer();

    if (!m_contentTypeKnown) {
        const QString contentType =
            m_currentReply->header(QNetworkRequest::ContentTypeHeader).toString().trimmed().toLower();
        if (contentType.startsWith(QStringLiteral("text/event-stream"))) {
            m_contentTypeKnown = true;
            m_contentTypeIsSse = true;
        } else if (contentType.startsWith(QStringLiteral("application/json"))) {
            m_contentTypeKnown = true;
            m_contentTypeIsSse = false;
        } else {
            // 头未就绪/未知类型：先缓存，按首字节嗅探（不得把 JSON 喂给 SSE 解析器）
            m_pendingStreamBytes += chunk;
            const QByteArray sniff = m_pendingStreamBytes.trimmed().left(16);
            if (sniff.startsWith("data:") || sniff.startsWith(':') || sniff.startsWith("event:")) {
                m_contentTypeKnown = true;
                m_contentTypeIsSse = true;
            } else if (sniff.startsWith('{')) {
                m_contentTypeKnown = true;
                m_contentTypeIsSse = false;
            } else {
                return; // 等更多字节/头部
            }
        }
    }

    QByteArray bytes = m_pendingStreamBytes;
    bytes += chunk;
    m_pendingStreamBytes.clear();
    if (m_contentTypeIsSse)
        processStreamBytes(bytes);
    else
        m_jsonBody += bytes;
}

void OpenAILLMClient::processStreamBytes(const QByteArray& bytes) {
    const QList<SseStreamParser::Event> events = m_sseParser.feed(bytes);
    refreshStreamIdleTimer();
    for (const SseStreamParser::Event& event : events) {
        // R4：回退/结束/中止后，同批剩余事件不得再被消费（回退会切换 m_requestStreaming=false）
        if (!m_requestStreaming || m_streamFinalized || m_aborted)
            return;
        handleStreamEvent(event);
        if (!m_requestStreaming || m_streamFinalized || m_aborted)
            return;
    }
}

void OpenAILLMClient::handleStreamEvent(const SseStreamParser::Event& event) {
    ++m_streamEventCount;
    if (event.done) {
        finalizeStream();
        return;
    }
    const QByteArray data = event.data.trimmed();
    if (data.isEmpty())
        return;

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (!doc.isObject()) {
        if (!m_sawDelta) {
            ++m_streamParseErrors;
            qWarning() << "[OpenAILLMClient] malformed stream event (no delta yet), count=" << m_streamParseErrors;
            if (m_streamParseErrors >= 3)
                tryStreamFallback(QStringLiteral("invalid stream data"));
        } else {
            qWarning() << "[OpenAILLMClient] ignoring malformed stream event after delta:" << parseError.errorString();
        }
        return;
    }
    handleStreamJson(doc.object());
}

void OpenAILLMClient::handleStreamJson(const QJsonObject& obj) {
    if (obj.contains(QStringLiteral("error"))) {
        QString errMsg = obj.value(QStringLiteral("error")).toObject().value(QStringLiteral("message")).toString();
        if (errMsg.isEmpty())
            errMsg = obj.value(QStringLiteral("error")).toString();
        LLMErrorInfo info =
            classifyError(0, QNetworkReply::NoError, errMsg, QJsonDocument(obj).toJson(QJsonDocument::Compact));
        info.message = QStringLiteral("The model service returned an invalid response: %1").arg(errMsg);
        handleFailure(info);
        return;
    }

    // usage 可能出现在独立块（choices 为空）
    const QJsonObject usage = obj.value(QStringLiteral("usage")).toObject();
    if (!usage.isEmpty()) {
        m_streamPromptTokens = usage.value(QStringLiteral("prompt_tokens")).toInt();
        m_streamCompletionTokens = usage.value(QStringLiteral("completion_tokens")).toInt();
    }

    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return; // usage-only 块

    const QJsonObject choice = choices.at(0).toObject();
    const QJsonObject delta = choice.value(QStringLiteral("delta")).toObject();

    // 首个有效事件（含仅 role 的首块）→ streamStarted
    if (!m_streamStartedEmitted) {
        m_streamStartedEmitted = true;
        emit streamStarted();
    }

    const QString content = delta.value(QStringLiteral("content")).toString();
    if (!content.isEmpty()) {
        m_sawDelta = true;
        m_streamContent += content;
        emit streamContentDelta(content);
    }

    QString reasoning;
    const QStringList reasoningKeys = {QStringLiteral("reasoning_content"), QStringLiteral("reasoning"),
                                       QStringLiteral("reasoning_text")};
    for (const QString& key : reasoningKeys) {
        const QString value = delta.value(key).toString();
        if (!value.isEmpty()) {
            reasoning = value;
            break;
        }
    }
    if (!reasoning.isEmpty()) {
        m_sawDelta = true;
        m_streamReasoning += reasoning;
        emit streamReasoningDelta(reasoning);
    }

    if (delta.contains(QStringLiteral("tool_calls"))) {
        m_sawDelta = true;
        accumulateToolCalls(delta.value(QStringLiteral("tool_calls")).toArray());
        emit streamToolCallsUpdated(m_toolCallAcc.size());
    }

    const QJsonValue finishReason = choice.value(QStringLiteral("finish_reason"));
    if (!finishReason.isNull() && !finishReason.isUndefined())
        m_finishReason = finishReason.toString();
}

void OpenAILLMClient::accumulateToolCalls(const QJsonArray& toolCalls) {
    for (const QJsonValue& value : toolCalls) {
        const QJsonObject tc = value.toObject();
        int index = -1;
        if (tc.contains(QStringLiteral("index")) && !tc.value(QStringLiteral("index")).isNull())
            index = tc.value(QStringLiteral("index")).toInt(-1);
        if (index < 0) {
            if (m_toolCallAcc.size() == 1) {
                index = m_toolCallAcc.firstKey();
            } else {
                index = 0;
                qWarning() << "[OpenAILLMClient] tool_call fragment without index; assuming index 0";
            }
        }
        ToolCallAcc& acc = m_toolCallAcc[index];
        const QString id = tc.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            acc.id = id;
        const QString type = tc.value(QStringLiteral("type")).toString();
        if (!type.isEmpty())
            acc.type = type;
        const QJsonObject function = tc.value(QStringLiteral("function")).toObject();
        const QString name = function.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            acc.name = name;
        const QJsonValue arguments = function.value(QStringLiteral("arguments"));
        if (arguments.isString()) {
            acc.arguments += arguments.toString();
        } else if (arguments.isObject()) {
            acc.arguments += QString::fromUtf8(QJsonDocument(arguments.toObject()).toJson(QJsonDocument::Compact));
        }
    }
}

QJsonArray OpenAILLMClient::buildStreamedToolCalls() {
    QJsonArray toolCalls;
    QSet<QString> usedIds;
    for (auto it = m_toolCallAcc.cbegin(); it != m_toolCallAcc.cend(); ++it) {
        const ToolCallAcc& acc = it.value();
        if (acc.name.isEmpty()) {
            ++m_droppedToolCalls;
            continue;
        }
        QString id = acc.id;
        if (id.isEmpty())
            id = QStringLiteral("call_%1").arg(it.key());
        if (usedIds.contains(id)) {
            const QString base = id;
            int suffix = 2;
            while (usedIds.contains(id = QStringLiteral("%1_%2").arg(base).arg(suffix)))
                ++suffix;
        }
        usedIds.insert(id);
        toolCalls.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("type"), acc.type.isEmpty() ? QStringLiteral("function") : acc.type},
            {QStringLiteral("function"),
             QJsonObject{{QStringLiteral("name"), acc.name}, {QStringLiteral("arguments"), acc.arguments}}},
        });
    }
    return toolCalls;
}

void OpenAILLMClient::finalizeStream() {
    if (m_aborted || m_streamFinalized)
        return;

    if (m_finishReason == QStringLiteral("content_filter")) {
        stopStreamTimers();
        LLMErrorInfo info;
        info.category = LLMErrorCategory::BadRequest;
        info.retryable = false;
        info.message = QStringLiteral("The response was blocked by the content filter.");
        info.detail = QStringLiteral("finish_reason=content_filter");
        failStream(info);
        return;
    }

    m_streamFinalized = true;
    stopStreamTimers();

    AgentResponse resp;
    resp.success = true;
    resp.errorCategory = LLMErrorCategory::None;
    resp.content = m_streamContent;
    resp.reasoningContent = m_streamReasoning;
    resp.toolCalls = buildStreamedToolCalls();
    resp.promptTokens = m_streamPromptTokens;
    resp.completionTokens = m_streamCompletionTokens;
    resp.truncatedByMaxTokens = m_finishReason == QStringLiteral("length");

    emit streamFinished();
    emit responseReceived(resp);
}

void OpenAILLMClient::failStream(const LLMErrorInfo& info) {
    if (m_aborted || m_streamFinalized)
        return;
    m_streamFinalized = true;
    stopStreamTimers();

    if (info.visionDowngradeHint && !m_visionDowngraded) {
        m_visionDowngraded = true;
        qWarning() << "[OpenAILLMClient] model rejected image input; vision disabled for this client session";
    }

    LLMErrorInfo failure = info;
    failure.attempts = m_retryAttempt + 1;
    qWarning() << "[OpenAILLMClient] stream failed after delta:" << llmErrorCategoryName(info.category)
               << "httpStatus=" << info.httpStatus;
    recordFailedRequestIfNeeded(failure);
    emit streamFailed();
    emit errorOccurredClassified(failure);
    emit errorOccurred(failure.message);
}

bool OpenAILLMClient::tryStreamFallback(const QString& reason) {
    if (m_streamFallbackUsed || m_sawDelta || m_streamFinalized || m_aborted)
        return false;
    m_streamFallbackUsed = true;

    // 回退重发沿用同一对话/工具列表，只删 stream 相关键（§4.3）
    QJsonObject body = QJsonDocument::fromJson(m_lastBody).object();
    body.remove(QStringLiteral("stream"));
    body.remove(QStringLiteral("stream_options"));
    m_lastBody = QJsonDocument(body).toJson(QJsonDocument::Compact);

    stopStreamTimers();
    resetStreamAccumulators();
    releaseCurrentReply();

    qWarning() << "[OpenAILLMClient] falling back to non-stream request:" << reason;
    emit streamFallback(reason);
    m_requestStreaming = false;
    postRequestBody(false);
    return true;
}

void OpenAILLMClient::startStreamTimers() {
    m_streamIdleTimer->start(m_streamIdleTimeoutMs);
    m_streamDeadlineTimer->start(m_streamMaxDurationMs);
}

void OpenAILLMClient::stopStreamTimers() {
    if (m_streamIdleTimer)
        m_streamIdleTimer->stop();
    if (m_streamDeadlineTimer)
        m_streamDeadlineTimer->stop();
}

void OpenAILLMClient::refreshStreamIdleTimer() {
    if (m_requestStreaming && !m_streamFinalized && !m_aborted)
        m_streamIdleTimer->start(m_streamIdleTimeoutMs);
}

void OpenAILLMClient::onStreamIdleTimeout() {
    if (m_aborted || m_streamFinalized || !m_requestStreaming)
        return;
    qWarning() << "[OpenAILLMClient] stream idle timeout after" << m_streamIdleTimeoutMs << "ms";
    releaseCurrentReply();
    LLMErrorInfo info;
    info.category = LLMErrorCategory::Timeout;
    info.retryable = true;
    info.message = QStringLiteral("The model service did not respond in time.");
    info.detail = QStringLiteral("stream idle timeout");
    if (m_sawDelta)
        failStream(info);
    else
        handleFailure(info);
}

void OpenAILLMClient::onStreamDeadlineTimeout() {
    if (m_aborted || m_streamFinalized || !m_requestStreaming)
        return;
    qWarning() << "[OpenAILLMClient] stream exceeded maximum duration" << m_streamMaxDurationMs << "ms";
    releaseCurrentReply();
    LLMErrorInfo info;
    info.category = LLMErrorCategory::Timeout;
    info.retryable = true;
    info.message = QStringLiteral("The model service did not respond in time.");
    info.detail = QStringLiteral("stream max duration exceeded");
    if (m_sawDelta)
        failStream(info);
    else
        handleFailure(info);
}

bool OpenAILLMClient::isStreamUnsupportedResponse(int httpStatus, const QByteArray& body) const {
    if (httpStatus != 400 && httpStatus != 404 && httpStatus != 405 && httpStatus != 422)
        return false;
    static const QRegularExpression re(
        QStringLiteral("stream|event-stream|not support.*stream|streaming.*not.*support"),
        QRegularExpression::CaseInsensitiveOption);
    return re.match(QString::fromUtf8(body)).hasMatch();
}

bool OpenAILLMClient::looksLikeJsonBody(const QByteArray& body) const {
    const QByteArray trimmed = body.trimmed();
    return !trimmed.isEmpty() && trimmed.startsWith('{');
}

void OpenAILLMClient::parseResponse(const QByteArray& data) {
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (!doc.isObject()) {
        handleFailure(classifyError(0, QNetworkReply::NoError, parseError.errorString(), data));
        return;
    }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        QString errMsg = obj["error"].toObject()["message"].toString();
        if (errMsg.isEmpty())
            errMsg = obj["error"].toString();
        if (errMsg.isEmpty())
            errMsg = QStringLiteral("Unknown error");
        LLMErrorInfo info = classifyError(0, QNetworkReply::NoError, errMsg, data);
        info.message = QStringLiteral("The model service returned an invalid response: %1").arg(errMsg);
        handleFailure(info);
        return;
    }

    QJsonArray choices = obj["choices"].toArray();
    if (choices.isEmpty()) {
        handleFailure(classifyError(0, QNetworkReply::NoError, QStringLiteral("No choices in LLM response"), data));
        return;
    }

    QJsonObject message = choices[0].toObject()["message"].toObject();
    AgentResponse resp;
    resp.success = true;
    resp.errorCategory = LLMErrorCategory::None;
    resp.content = message["content"].toString();
    resp.reasoningContent = message["reasoning_content"].toString();

    QJsonArray toolCalls = message["tool_calls"].toArray();
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject();
        QJsonObject func = tc["function"].toObject();
        QJsonValue argsVal = func["arguments"];
        if (argsVal.isObject()) {
            func["arguments"] = QString(QJsonDocument(argsVal.toObject()).toJson(QJsonDocument::Compact));
            tc["function"] = func;
        }
        resp.toolCalls.append(tc);
    }

    QJsonObject usage = obj["usage"].toObject();
    resp.promptTokens = usage["prompt_tokens"].toInt();
    resp.completionTokens = usage["completion_tokens"].toInt();

    emit responseReceived(resp);
}

} // namespace DeepLux
