#ifndef DEEPLUX_ILLM_CLIENT_H
#define DEEPLUX_ILLM_CLIENT_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>

namespace DeepLux {

struct ToolDefinition;

/**
 * @brief 图像附件
 */
struct AgentImageAttachment
{
    QByteArray data;    // PNG/JPEG raw data
    QString mimeType;   // "image/png", "image/jpeg"
    QString description; // optional description for non-vision models
};

/**
 * @brief 对话消息
 */
struct AgentMessage
{
    QString role;       // "system", "user", "assistant", "tool"
    QString content;
    QJsonObject toolCalls;   // for assistant messages with tool_calls
    QString toolCallId;      // for tool role messages
    QList<AgentImageAttachment> images; // for multimodal user messages
};

/**
 * @brief 对话上下文
 */
struct AgentConversation
{
    QList<AgentMessage> messages;
    QString systemPrompt;

    QJsonArray toOpenAIMessages() const;
};

/**
 * @brief LLM 响应
 */
struct AgentResponse
{
    bool success = false;
    QString errorMessage;
    QString content;            // 文本回复
    QJsonArray toolCalls;       // tool_call 列表
    int promptTokens = 0;
    int completionTokens = 0;
};

/**
 * @brief LLM 客户端抽象接口
 *
 * 设计约束：
 * - 内部使用 QThread + QNetworkAccessManager 做异步 HTTP
 * - 结果通过信号槽回传到 GUI 线程
 */
class ILLMClient : public QObject
{
    Q_OBJECT

public:
    explicit ILLMClient(QObject* parent = nullptr) : QObject(parent) {}
    virtual ~ILLMClient() = default;

    // 配置
    virtual void setApiKey(const QString& key) = 0;
    virtual void setEndpoint(const QString& url) = 0;
    virtual void setModel(const QString& model) = 0;
    virtual void setTemperature(double temp) = 0;
    virtual void setMaxTokens(int tokens) = 0;

    // 发送请求（异步）
    virtual void sendRequest(const AgentConversation& ctx,
                             const QList<ToolDefinition>& tools) = 0;

signals:
    void responseReceived(const AgentResponse& resp);
    void errorOccurred(const QString& error);
    void streamChunkReceived(const QString& chunk);
};

} // namespace DeepLux

#endif // DEEPLUX_ILLM_CLIENT_H
