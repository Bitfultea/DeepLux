#include "OpenAILLMClient.h"

#include "ToolSchema.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

namespace DeepLux {

OpenAILLMClient::OpenAILLMClient(QObject* parent)
    : ILLMClient(parent), m_networkManager(new QNetworkAccessManager(this)) {}

OpenAILLMClient::~OpenAILLMClient() {
    cancelRequest();
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
}
void OpenAILLMClient::setReasoningEffort(const QString& effort) {
    m_reasoningEffort = effort;
}
void OpenAILLMClient::setThinkingEnabled(bool enabled) {
    m_thinkingEnabled = enabled;
}

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
    m_activeRequestId = ctx.requestId;

    QJsonDocument doc(buildRequestBody(ctx, tools));
    QByteArray data = doc.toJson();

    QUrl url(m_endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Connection", "keep-alive");
    request.setTransferTimeout(30000); // 30s 超时，防止无限等待

    qDebug() << "OpenAILLMClient: Sending request to" << m_endpoint << "model=" << m_model
             << "msgs=" << ctx.messages.size();

    m_currentReply = m_networkManager->post(request, data);
    const QPointer<QNetworkReply> reply = m_currentReply;
    const QString requestId = m_activeRequestId;
    connect(m_currentReply, &QNetworkReply::finished, this, [this, reply, requestId]() {
        if (reply)
            onReplyFinished(reply, requestId);
    });
    connect(m_currentReply, &QNetworkReply::errorOccurred, this,
            [this, reply, requestId](QNetworkReply::NetworkError error) {
                if (reply)
                    onNetworkError(reply, requestId, error);
            });
}

void OpenAILLMClient::cancelRequest() {
    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }
    m_activeRequestId.clear();
}

void OpenAILLMClient::onReplyFinished(QNetworkReply* reply, const QString& requestId) {
    if (!reply || reply != m_currentReply) {
        return;
    }

    QByteArray data = reply->readAll();
    m_activeRequestId.clear();
    reply->deleteLater();
    m_currentReply = nullptr;

    if (data.isEmpty()) {
        emit requestFailed(requestId, "Empty response from LLM");
        emit errorOccurred("Empty response from LLM");
        return;
    }

    parseResponse(data, requestId);
}

void OpenAILLMClient::parseResponse(const QByteArray& data, const QString& requestId) {
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        emit requestFailed(requestId, "Invalid JSON response");
        emit errorOccurred("Invalid JSON response");
        return;
    }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        QString errMsg = obj["error"].toObject()["message"].toString("Unknown error");
        emit requestFailed(requestId, errMsg);
        emit errorOccurred(errMsg);
        return;
    }

    QJsonArray choices = obj["choices"].toArray();
    if (choices.isEmpty()) {
        emit requestFailed(requestId, "No choices in LLM response");
        emit errorOccurred("No choices in LLM response");
        return;
    }

    QJsonObject message = choices[0].toObject()["message"].toObject();
    AgentResponse resp;
    resp.requestId = requestId;
    resp.success = true;
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

void OpenAILLMClient::onNetworkError(QNetworkReply* reply, const QString& requestId,
                                     QNetworkReply::NetworkError error) {
    Q_UNUSED(error);
    if (!reply || reply != m_currentReply) {
        return;
    }
    QString msg = reply->errorString();
    QByteArray body = reply->readAll();
    if (!body.isEmpty()) {
        msg += " | Response: " + QString::fromUtf8(body);
    }
    disconnect(reply, nullptr, this, nullptr);
    reply->abort();
    reply->deleteLater();
    m_currentReply = nullptr;
    m_activeRequestId.clear();
    emit requestFailed(requestId, msg);
    emit errorOccurred(msg);
}

} // namespace DeepLux
