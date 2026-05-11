#include "OpenAILLMClient.h"
#include "ToolSchema.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QFile>
#include <QIODevice>

namespace DeepLux {

OpenAILLMClient::OpenAILLMClient(QObject* parent)
    : ILLMClient(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
}

OpenAILLMClient::~OpenAILLMClient()
{
    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
    }
}

void OpenAILLMClient::setApiKey(const QString& key)   { m_apiKey = key; }

void OpenAILLMClient::setEndpoint(const QString& url)
{
    m_endpoint = url;
    QUrl parsed(url);
    QString path = parsed.path();
    if (path.isEmpty() || path == "/") {
        parsed.setPath("/v1/chat/completions");
        m_endpoint = parsed.toString();
        qDebug() << "OpenAILLMClient: Auto-completed endpoint to" << m_endpoint;
    }
}

void OpenAILLMClient::setModel(const QString& model)       { m_model = model; }
void OpenAILLMClient::setTemperature(double temp)          { m_temperature = temp; }
void OpenAILLMClient::setMaxTokens(int tokens)              { m_maxTokens = tokens; }
void OpenAILLMClient::setToolsEnabled(bool enabled)         { m_toolsEnabled = enabled; }
void OpenAILLMClient::setReasoningEffort(const QString& effort) { m_reasoningEffort = effort; }
void OpenAILLMClient::setThinkingEnabled(bool enabled)      { m_thinkingEnabled = enabled; }

static void diagLLM(const QString& msg) {
    QFile f("/tmp/deeplux_agent_diag.log");
    f.open(QIODevice::WriteOnly | QIODevice::Append);
    f.write(QString("[DIAG-LLM] %1\n").arg(msg).toUtf8());
    f.close();
    qDebug() << "[DIAG-LLM]" << msg;
}

void OpenAILLMClient::sendRequest(const AgentConversation& ctx,
                                  const QList<ToolDefinition>& tools)
{
    // 中止上一个请求，防止信号串联（Agent 闭环中连续发请求时）
    if (m_currentReply) {
        diagLLM("sendRequest: aborting previous reply");
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    diagLLM(QString("sendRequest: endpoint=%1 model=%2 msgs=%3").arg(m_endpoint).arg(m_model).arg(ctx.messages.size()));
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

    QJsonDocument doc(body);
    QByteArray data = doc.toJson();

    QUrl url(m_endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());
    request.setRawHeader("Connection", "keep-alive");
    request.setTransferTimeout(30000);  // 30s 超时，防止无限等待

    qDebug() << "OpenAILLMClient: Sending request to" << m_endpoint
             << "model=" << m_model << "msgs=" << ctx.messages.size();

    m_currentReply = m_networkManager->post(request, data);
    diagLLM("sendRequest: post() called, connecting signals");
    connect(m_currentReply, &QNetworkReply::finished,
            this, &OpenAILLMClient::onReplyFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &OpenAILLMClient::onNetworkError);
}

void OpenAILLMClient::onReplyFinished()
{
    diagLLM("onReplyFinished: called");
    if (!m_currentReply) {
        diagLLM("onReplyFinished: m_currentReply is null, returning");
        return;
    }

    QByteArray data = m_currentReply->readAll();
    diagLLM(QString("onReplyFinished: read %1 bytes").arg(data.size()));
    m_currentReply->deleteLater();
    m_currentReply = nullptr;

    if (data.isEmpty()) {
        diagLLM("onReplyFinished: empty data, emitting error");
        emit errorOccurred("Empty response from LLM");
        return;
    }

    diagLLM("onReplyFinished: calling parseResponse");
    parseResponse(data);
}

void OpenAILLMClient::parseResponse(const QByteArray& data)
{
    diagLLM("parseResponse: start");
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        diagLLM("parseResponse: invalid JSON");
        emit errorOccurred("Invalid JSON response");
        return;
    }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        QString errMsg = obj["error"].toObject()["message"].toString("Unknown error");
        diagLLM(QString("parseResponse: LLM error: %1").arg(errMsg));
        emit errorOccurred(errMsg);
        return;
    }

    QJsonArray choices = obj["choices"].toArray();
    if (choices.isEmpty()) {
        diagLLM("parseResponse: no choices");
        emit errorOccurred("No choices in LLM response");
        return;
    }
    diagLLM(QString("parseResponse: choices=%1").arg(choices.size()));

    QJsonObject message = choices[0].toObject()["message"].toObject();
    AgentResponse resp;
    resp.success = true;
    resp.content = message["content"].toString();
    resp.reasoningContent = message["reasoning_content"].toString();
    diagLLM(QString("parseResponse: content len=%1 reasoning len=%2")
              .arg(resp.content.length()).arg(resp.reasoningContent.length()));

    QJsonArray toolCalls = message["tool_calls"].toArray();
    diagLLM(QString("parseResponse: toolCalls=%1").arg(toolCalls.size()));
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject();
        QJsonObject func = tc["function"].toObject();
        QJsonValue argsVal = func["arguments"];
        if (argsVal.isObject()) {
            func["arguments"] = QString(QJsonDocument(argsVal.toObject())
                .toJson(QJsonDocument::Compact));
            tc["function"] = func;
        }
        resp.toolCalls.append(tc);
    }

    QJsonObject usage = obj["usage"].toObject();
    resp.promptTokens = usage["prompt_tokens"].toInt();
    resp.completionTokens = usage["completion_tokens"].toInt();

    diagLLM("parseResponse: emitting responseReceived");
    emit responseReceived(resp);
}

void OpenAILLMClient::onNetworkError(QNetworkReply::NetworkError error)
{
    Q_UNUSED(error);
    diagLLM(QString("onNetworkError: error=%1").arg(static_cast<int>(error)));
    if (!m_currentReply) {
        diagLLM("onNetworkError: m_currentReply is null, returning");
        return;
    }
    QString msg = m_currentReply->errorString();
    QByteArray body = m_currentReply->readAll();
    if (!body.isEmpty()) {
        msg += " | Response: " + QString::fromUtf8(body);
    }
    diagLLM(QString("onNetworkError: %1").arg(msg));
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    emit errorOccurred(msg);
}

} // namespace DeepLux
