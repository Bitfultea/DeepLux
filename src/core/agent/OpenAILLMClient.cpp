#include "OpenAILLMClient.h"
#include "ToolSchema.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

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

void OpenAILLMClient::sendRequest(const AgentConversation& ctx,
                                  const QList<ToolDefinition>& tools)
{
    // 中止上一个请求，防止信号串联（Agent 闭环中连续发请求时）
    if (m_currentReply) {
        disconnect(m_currentReply, nullptr, this, nullptr);
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

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

    QJsonDocument doc(body);
    QByteArray data = doc.toJson();

    QUrl url(m_endpoint);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());

    qDebug() << "OpenAILLMClient: Sending request to" << m_endpoint
             << "model=" << m_model << "msgs=" << ctx.messages.size();

    m_currentReply = m_networkManager->post(request, data);
    connect(m_currentReply, &QNetworkReply::finished,
            this, &OpenAILLMClient::onReplyFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &OpenAILLMClient::onNetworkError);
}

void OpenAILLMClient::onReplyFinished()
{
    if (!m_currentReply) return;

    QByteArray data = m_currentReply->readAll();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;

    if (data.isEmpty()) {
        emit errorOccurred("Empty response from LLM");
        return;
    }

    parseResponse(data);
}

void OpenAILLMClient::parseResponse(const QByteArray& data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) { emit errorOccurred("Invalid JSON response"); return; }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        emit errorOccurred(obj["error"].toObject()["message"].toString("Unknown error"));
        return;
    }

    QJsonArray choices = obj["choices"].toArray();
    if (choices.isEmpty()) { emit errorOccurred("No choices in LLM response"); return; }

    QJsonObject message = choices[0].toObject()["message"].toObject();
    AgentResponse resp;
    resp.success = true;
    resp.content = message["content"].toString();

    QJsonArray toolCalls = message["tool_calls"].toArray();
    for (const QJsonValue& v : toolCalls) {
        QJsonObject tc = v.toObject();
        QJsonObject parsed;
        parsed["id"] = tc["id"];
        parsed["type"] = tc["type"];
        QJsonObject func = tc["function"].toObject();
        parsed["name"] = func["name"];
        parsed["arguments"] = QJsonDocument::fromJson(
            func["arguments"].toString().toUtf8()).object();
        resp.toolCalls.append(parsed);
    }

    QJsonObject usage = obj["usage"].toObject();
    resp.promptTokens = usage["prompt_tokens"].toInt();
    resp.completionTokens = usage["completion_tokens"].toInt();

    emit responseReceived(resp);
}

void OpenAILLMClient::onNetworkError(QNetworkReply::NetworkError error)
{
    Q_UNUSED(error);
    if (!m_currentReply) return;
    QString msg = m_currentReply->errorString();
    QByteArray body = m_currentReply->readAll();
    if (!body.isEmpty()) {
        msg += " | Response: " + QString::fromUtf8(body);
    }
    qDebug() << "OpenAILLMClient: Network error:" << msg;
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    emit errorOccurred(msg);
}

} // namespace DeepLux
