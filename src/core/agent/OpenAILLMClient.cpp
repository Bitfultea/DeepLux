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
    QJsonObject body;
    body["model"] = m_model;
    body["temperature"] = m_temperature;
    body["max_tokens"] = m_maxTokens;
    body["messages"] = ctx.toOpenAIMessages();
    body["stream"] = true;  // SSE streaming

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

    // 重置流式状态
    m_streamContent.clear();
    m_streamToolCallId.clear();
    m_streamToolName.clear();
    m_streamToolArgs.clear();

    m_currentReply = m_networkManager->post(request, data);
    connect(m_currentReply, &QNetworkReply::readyRead,
            this, &OpenAILLMClient::onReplyReadyRead);
    connect(m_currentReply, &QNetworkReply::finished,
            this, &OpenAILLMClient::onReplyFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred,
            this, &OpenAILLMClient::onNetworkError);
}

void OpenAILLMClient::onReplyReadyRead()
{
    if (!m_currentReply) return;
    // 防止 finished 已触发后的延迟 readyRead
    if (m_currentReply->property("_finished").toBool()) return;

    QByteArray chunk = m_currentReply->readAll();
    QString text = QString::fromUtf8(chunk);
    QStringList lines = text.split('\n', Qt::SkipEmptyParts);

    for (const QString& line : lines) {
        if (!line.startsWith("data: ")) continue;
        QString jsonStr = line.mid(6).trimmed();
        if (jsonStr == "[DONE]") continue;

        QJsonDocument chunkDoc = QJsonDocument::fromJson(jsonStr.toUtf8());
        if (!chunkDoc.isObject()) continue;

        QJsonObject chunkObj = chunkDoc.object();
        QJsonArray choices = chunkObj["choices"].toArray();
        if (choices.isEmpty()) continue;

        QJsonObject delta = choices[0].toObject()["delta"].toObject();

        // 文本 delta
        QString contentDelta = delta["content"].toString();
        if (!contentDelta.isEmpty()) {
            m_streamContent += contentDelta;
            emit streamChunkReceived(contentDelta);
        }

        // 工具调用 delta
        QJsonArray toolDeltas = delta["tool_calls"].toArray();
        for (const QJsonValue& v : toolDeltas) {
            QJsonObject tc = v.toObject();
            int idx = tc["index"].toInt(-1);
            if (idx != 0) continue;  // 单工具调用场景

            QJsonObject func = tc["function"].toObject();
            QString nameDelta = func["name"].toString();
            QString argsDelta = func["arguments"].toString();

            if (!nameDelta.isEmpty()) m_streamToolName += nameDelta;
            if (!argsDelta.isEmpty()) m_streamToolArgs += argsDelta;
            if (!tc["id"].toString().isEmpty()) m_streamToolCallId = tc["id"].toString();
        }
    }
}

void OpenAILLMClient::onReplyFinished()
{
    if (!m_currentReply) return;

    // 标记 finished，防止延迟的 readyRead 再处理
    m_currentReply->setProperty("_finished", true);

    // 非流式 fallback：如果 stream accums 为空，用完整响应
    if (m_streamContent.isEmpty() && m_streamToolName.isEmpty()) {
        QByteArray data = m_currentReply->readAll();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
        if (!data.isEmpty()) {
            parseResponse(data);
        } else {
            emit errorOccurred("Empty response from LLM");
        }
        return;
    }

    m_currentReply->deleteLater();
    m_currentReply = nullptr;

    // 从 stream accumulators 构建响应
    AgentResponse resp;
    resp.success = true;
    resp.content = m_streamContent;

    if (!m_streamToolName.isEmpty()) {
        QJsonObject toolCall;
        toolCall["id"] = m_streamToolCallId;
        toolCall["type"] = "function";
        toolCall["name"] = m_streamToolName;
        toolCall["arguments"] = QJsonDocument::fromJson(m_streamToolArgs.toUtf8()).object();
        resp.toolCalls.append(toolCall);
    }

    emit responseReceived(resp);
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
