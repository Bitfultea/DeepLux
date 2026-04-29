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

void OpenAILLMClient::setApiKey(const QString& key)
{
    m_apiKey = key;
}

void OpenAILLMClient::setEndpoint(const QString& url)
{
    m_endpoint = url;
    // 自动补全 OpenAI-compatible API 路径
    QUrl parsed(url);
    QString path = parsed.path();
    if (path.isEmpty() || path == "/") {
        parsed.setPath("/v1/chat/completions");
        m_endpoint = parsed.toString();
        qDebug() << "OpenAILLMClient: Auto-completed endpoint to" << m_endpoint;
    }
}

void OpenAILLMClient::setModel(const QString& model)
{
    m_model = model;
}

void OpenAILLMClient::setTemperature(double temp)
{
    m_temperature = temp;
}

void OpenAILLMClient::setMaxTokens(int tokens)
{
    m_maxTokens = tokens;
}

void OpenAILLMClient::sendRequest(const AgentConversation& ctx,
                                  const QList<ToolDefinition>& tools)
{
    QJsonObject body;
    body["model"] = m_model;
    body["temperature"] = m_temperature;
    body["max_tokens"] = m_maxTokens;
    body["messages"] = ctx.toOpenAIMessages();

    if (!tools.isEmpty()) {
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

    qDebug() << "OpenAILLMClient: Sending request to" << m_endpoint;

    m_currentReply = m_networkManager->post(request, data);
    connect(m_currentReply, &QNetworkReply::finished,
            this, &OpenAILLMClient::onReplyFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &OpenAILLMClient::onNetworkError);
}

void OpenAILLMClient::onReplyFinished()
{
    if (!m_currentReply) return;

    QByteArray responseData = m_currentReply->readAll();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;

    QJsonDocument doc = QJsonDocument::fromJson(responseData);
    if (!doc.isObject()) {
        emit errorOccurred("Invalid JSON response from LLM");
        return;
    }

    QJsonObject obj = doc.object();
    if (obj.contains("error")) {
        QJsonObject err = obj["error"].toObject();
        emit errorOccurred(err.value("message").toString("Unknown error"));
        return;
    }

    QJsonArray choices = obj["choices"].toArray();
    if (choices.isEmpty()) {
        emit errorOccurred("No choices in LLM response");
        return;
    }

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
        QString argsStr = func["arguments"].toString();
        parsed["arguments"] = QJsonDocument::fromJson(argsStr.toUtf8()).object();
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
    m_currentReply->deleteLater();
    m_currentReply = nullptr;
    emit errorOccurred(msg);
}

} // namespace DeepLux
