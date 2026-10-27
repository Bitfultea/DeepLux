#ifndef DEEPLUX_ILLM_CLIENT_H
#define DEEPLUX_ILLM_CLIENT_H

#include "../platform/Platform.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>

namespace DeepLux {

struct ToolDefinition;

/**
 * @brief 图像附件
 */
struct AgentImageAttachment {
    QByteArray data;     // PNG/JPEG raw data
    QString mimeType;    // "image/png", "image/jpeg"
    QString description; // optional description for non-vision models
};

/**
 * @brief 对话消息
 */
struct AgentMessage {
    QString role; // "system", "user", "assistant", "tool"
    QString content;
    QJsonArray toolCalls;               // for assistant messages with tool_calls (OpenAI array format)
    QString toolCallId;                 // for tool role messages
    QList<AgentImageAttachment> images; // for multimodal user messages
    QString reasoningContent;           // DeepSeek thinking mode: 必须原样传回
};

/**
 * @brief 对话上下文
 */
struct AgentConversation {
    QList<AgentMessage> messages;
    QString systemPrompt;

    QJsonArray toOpenAIMessages() const;
};

/**
 * @brief LLM 错误分类
 *
 * 唯一分类来源是 OpenAILLMClient::classifyError()；该枚举只描述"哪一类失败"，
 * 不包含重试策略（策略见 OpenAILLMClient::maxRetriesFor/retryDelayMs）。
 */
enum class LLMErrorCategory { None, Auth, RateLimit, Server, Network, BadRequest, Timeout, Malformed };

/**
 * @brief 分类后的 LLM 错误信息
 *
 * detail 供审计/排障（截断 2KB），message 面向用户（英文短句）。
 */
struct LLMErrorInfo {
    LLMErrorCategory category = LLMErrorCategory::None;
    int httpStatus = 0; // 0 = 无 HTTP 状态（网络层）
    QString message;    // 面向用户的英文短句
    QString detail;     // 原始错误体/错误串（审计用，截断 2KB）
    bool retryable = false;
    bool contextLimitSuspected = false; // BadRequest 且疑似上下文超限
    bool visionDowngradeHint = false;   // BadRequest 且疑似不支持图片
    int attempts = 0;                   // 已尝试次数（含首次）
};

/// 类别名（"None"/"Auth"/...），用于审计字段与 UI 状态条。
/// 头文件内联：本迭代 ILLMClient.cpp 不在 T2 的写入范围内（文件所有权 §8），
/// 且该函数是纯字符串映射，无跨 TU 状态，内联不引入 ABI 问题。
inline QString llmErrorCategoryName(LLMErrorCategory category) {
    switch (category) {
    case LLMErrorCategory::None:
        return QStringLiteral("None");
    case LLMErrorCategory::Auth:
        return QStringLiteral("Auth");
    case LLMErrorCategory::RateLimit:
        return QStringLiteral("RateLimit");
    case LLMErrorCategory::Server:
        return QStringLiteral("Server");
    case LLMErrorCategory::Network:
        return QStringLiteral("Network");
    case LLMErrorCategory::BadRequest:
        return QStringLiteral("BadRequest");
    case LLMErrorCategory::Timeout:
        return QStringLiteral("Timeout");
    case LLMErrorCategory::Malformed:
        return QStringLiteral("Malformed");
    }
    return QStringLiteral("None");
}

/**
 * @brief LLM 响应
 */
struct AgentResponse {
    bool success = false;
    QString errorMessage;
    LLMErrorCategory errorCategory = LLMErrorCategory::None; // 成功时为 None
    QString content;                                         // 文本回复
    QJsonArray toolCalls;                                    // tool_call 列表
    QString reasoningContent;                                // DeepSeek thinking mode 推理内容
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
class DEEPLUX_API ILLMClient : public QObject {
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
    virtual void setToolsEnabled(bool enabled) = 0;
    virtual bool supportsImageInput() const {
        return false;
    }

    // 发送请求（异步）
    virtual void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) = 0;

    // 中止当前请求并取消待触发的重试（非纯虚：既有 Mock/Scripted 客户端无需改动）
    virtual void abortRequest() {}

signals:
    void responseReceived(const AgentResponse& resp);
    void errorOccurred(const QString& error);
    // 分类错误（最终失败时发出；与 errorOccurred 并行保留既有链路）
    void errorOccurredClassified(const LLMErrorInfo& info);
    // 已安排一次退避重试：attempt 从 1 开始，delayMs 为本次退避时长
    void retryScheduled(const LLMErrorInfo& info, int attempt, int delayMs);
};

} // namespace DeepLux

Q_DECLARE_METATYPE(DeepLux::AgentResponse)
Q_DECLARE_METATYPE(DeepLux::LLMErrorInfo)

#endif // DEEPLUX_ILLM_CLIENT_H
