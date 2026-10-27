#include "OpenAILLMClient.h"

#include "ToolSchema.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
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
    : ILLMClient(parent), m_networkManager(new QNetworkAccessManager(this)), m_retryTimer(new QTimer(this)) {
    m_retryTimer->setSingleShot(true);
    connect(m_retryTimer, &QTimer::timeout, this, [this]() {
        if (m_aborted)
            return;
        qDebug() << "[OpenAILLMClient] retrying request, attempt" << (m_retryAttempt + 1);
        postRequestBody();
    });
}

OpenAILLMClient::~OpenAILLMClient() {
    m_aborted = true;
    if (m_retryTimer)
        m_retryTimer->stop();
    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
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
    body["messages"] = ctx.toOpenAIMessages();
    // 暂不使用 stream（DeepSeek 兼容性；流式回调已保留以备未来启用）

    if (m_toolsEnabled && !tools.isEmpty()) {
        QJsonArray toolsArray;
        for (const ToolDefinition& t : tools) {
            toolsArray.append(t.toOpenAIFunction());
        }
        body["tools"] = toolsArray;
        body["tool_choice"] = "auto";
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
    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    // 新请求取消一切待触发的重试并重置重试状态
    m_retryTimer->stop();
    m_aborted = false;
    m_retryAttempt = 0;
    m_retryCategory = LLMErrorCategory::None;

    QJsonDocument doc(buildRequestBody(ctx, tools));
    m_lastBody = doc.toJson();
    m_lastTools = tools;
    postRequestBody();
}

void OpenAILLMClient::postRequestBody() {
    QUrl url(m_endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Connection", "keep-alive");
    request.setTransferTimeout(30000); // 30s 超时，防止无限等待（→ Timeout 类别）

    qDebug() << "OpenAILLMClient: Sending request to" << m_endpoint << "model=" << m_model
             << "bytes=" << m_lastBody.size() << "tools=" << m_lastTools.size();

    m_currentReply = m_networkManager->post(request, m_lastBody);
    connect(m_currentReply, &QNetworkReply::finished, this, &OpenAILLMClient::onReplyFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred, this, &OpenAILLMClient::onNetworkError);
}

void OpenAILLMClient::abortRequest() {
    m_aborted = true;
    if (m_retryTimer)
        m_retryTimer->stop();
    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    m_retryAttempt = 0;
    m_retryCategory = LLMErrorCategory::None;
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
    const QByteArray data = reply->readAll();
    reply->deleteLater();

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
    const QByteArray body = reply->readAll();
    disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();

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
               << "httpStatus=" << info.httpStatus << "attempts=" << failure.attempts << info.detail;
    emit errorOccurredClassified(failure);
    emit errorOccurred(failure.message);
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
