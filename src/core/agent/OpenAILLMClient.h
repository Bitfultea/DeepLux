#ifndef DEEPLUX_OPENAI_LLM_CLIENT_H
#define DEEPLUX_OPENAI_LLM_CLIENT_H

#include "ILLMClient.h"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class QTimer;

namespace DeepLux {

class DEEPLUX_API OpenAILLMClient : public ILLMClient {
    Q_OBJECT

public:
    explicit OpenAILLMClient(QObject* parent = nullptr);
    ~OpenAILLMClient() override;

    void setApiKey(const QString& key) override;
    void setEndpoint(const QString& url) override;
    void setModel(const QString& model) override;
    void setTemperature(double temp) override;
    void setMaxTokens(int tokens) override;
    void setToolsEnabled(bool enabled) override;
    bool supportsImageInput() const override {
        // R5：一旦服务端以"不支持图片"拒绝过，本客户端会话内不再附图
        return m_visionEnabled && !m_visionDowngraded;
    }
    void setVisionEnabled(bool enabled);
    void setReasoningEffort(const QString& effort);
    void setThinkingEnabled(bool enabled);

    static QString normalizeEndpoint(const QString& endpoint);

    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) override;
    void abortRequest() override;

    /**
     * @brief 唯一错误分类实现（HTTP 状态优先，其次网络错误，最后兜底 Malformed）
     *
     * 纯函数、无网络依赖，便于契约测试直接断言分类结果。
     * @param httpStatus HTTP 状态码；0 表示没有 HTTP 响应
     * @param networkError QNetworkReply::error()；无错误传 NoError
     * @param errorString QNetworkReply::errorString()
     * @param body 响应体（解析失败时传原始响应）
     */
    static LLMErrorInfo classifyError(int httpStatus, QNetworkReply::NetworkError networkError,
                                      const QString& errorString, const QByteArray& body);

    /// 首次之外允许的重试次数（RateLimit/Server=3，Network=2，Timeout=1，其余=0）
    static int maxRetriesFor(LLMErrorCategory category);
    /// 第 retryIndex 次重试（从 1 开始）的退避毫秒数；无策略返回 0
    static int retryDelayMs(LLMErrorCategory category, int retryIndex);

DEEPLUX_TEST_VISIBILITY slots : void onReplyFinished();
    void onNetworkError(QNetworkReply::NetworkError error);

    DEEPLUX_TEST_VISIBILITY : QJsonObject buildRequestBody(const AgentConversation& ctx,
                                                           const QList<ToolDefinition>& tools) const;
    void parseResponse(const QByteArray& data);

    void postRequestBody();
    void handleFailure(const LLMErrorInfo& info);

    QString m_apiKey;
    QString m_endpoint = "https://api.openai.com/v1/chat/completions";
    QString m_model = "gpt-4o";
    double m_temperature = 0.3;
    int m_maxTokens = 4096;
    bool m_toolsEnabled = true;
    bool m_visionEnabled = true;
    bool m_visionDowngraded = false; // 服务端拒绝过图片输入（本客户端会话内不再附图）
    QString m_reasoningEffort;       // "high" or "max" (DeepSeek thinking mode)
    bool m_thinkingEnabled = false;

    QNetworkAccessManager* m_networkManager = nullptr;
    QNetworkReply* m_currentReply = nullptr;

    // 重试状态：只重发上一次请求体，不重放对话副作用（§7.3 第 8 条）
    QTimer* m_retryTimer = nullptr;
    QByteArray m_lastBody;
    QList<ToolDefinition> m_lastTools;
    int m_retryAttempt = 0; // 已安排的重试次数
    LLMErrorCategory m_retryCategory = LLMErrorCategory::None;
    bool m_aborted = false; // 用户主动中止：不分类、不重试
};

} // namespace DeepLux

#endif // DEEPLUX_OPENAI_LLM_CLIENT_H
