#ifndef DEEPLUX_OPENAI_LLM_CLIENT_H
#define DEEPLUX_OPENAI_LLM_CLIENT_H

#include "ILLMClient.h"
#include "SseStreamParser.h"

#include <QByteArray>
#include <QMap>
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
    /// L3：配置缺省时的模型名视觉能力启发式（纯函数；其它模型保守返回 true）
    static bool heuristicSupportsVision(const QString& model);
    void setReasoningEffort(const QString& effort);
    void setThinkingEnabled(bool enabled);

    // ---- 第二迭代：流式 / reasoning 回传 / 看门狗（设计 §3/§4/§6）----
    void setStreamIncludeUsage(bool include) {
        m_streamIncludeUsage = include;
    }
    void setReasoningEcho(ReasoningEcho echo) {
        m_reasoningEcho = echo;
    }
    ReasoningEcho reasoningEcho() const {
        return m_reasoningEcho;
    }
    void setStreamIdleTimeoutMs(int ms);
    void setStreamMaxDurationMs(int ms);

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
    void onStreamReadyRead();
    void onStreamIdleTimeout();
    void onStreamDeadlineTimeout();

    DEEPLUX_TEST_VISIBILITY : QJsonObject buildRequestBody(const AgentConversation& ctx,
                                                           const QList<ToolDefinition>& tools) const;
    void parseResponse(const QByteArray& data);

    void postRequestBody(bool streaming);
    void handleFailure(const LLMErrorInfo& info);

    // ---- 流式内部实现 ----
    struct ToolCallAcc {
        QString id;
        QString type = QStringLiteral("function");
        QString name;
        QString arguments;
    };

    void resetStreamStateForRequest();
    void resetStreamAccumulators();
    void processStreamBytes(const QByteArray& bytes);
    void handleStreamEvent(const SseStreamParser::Event& event);
    void handleStreamJson(const QJsonObject& obj);
    void accumulateToolCalls(const QJsonArray& toolCalls);
    QJsonArray buildStreamedToolCalls();
    void finalizeStream();
    void failStream(const LLMErrorInfo& info);
    void recordFailedRequestIfNeeded(const LLMErrorInfo& info);
    bool tryStreamFallback(const QString& reason);
    void releaseCurrentReply();
    void startStreamTimers();
    void stopStreamTimers();
    void refreshStreamIdleTimer();
    bool isStreamUnsupportedResponse(int httpStatus, const QByteArray& body) const;
    bool looksLikeJsonBody(const QByteArray& body) const;

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
    QList<AgentMessage> m_lastRequestMessages; // 失败报文脱敏落盘用（设计 §4.5；只加不改）
    QList<ToolDefinition> m_lastTools;
    int m_retryAttempt = 0; // 已安排的重试次数
    LLMErrorCategory m_retryCategory = LLMErrorCategory::None;
    bool m_aborted = false; // 用户主动中止：不分类、不重试

    // ---- 流式状态（设计 §3/§4/§6）----
    bool m_streamIncludeUsage = false;
    ReasoningEcho m_reasoningEcho = ReasoningEcho::None;
    int m_streamIdleTimeoutMs = 60000;
    int m_streamMaxDurationMs = 600000;

    bool m_requestStreaming = false;   // 本请求以 SSE 模式发送
    bool m_streamFallbackUsed = false; // 本次 sendRequest 已回退过（只允许一次）
    bool m_streamFinalized = false;    // 终态幂等（readyRead/finished 双触发）
    bool m_streamStartedEmitted = false;
    bool m_sawDelta = false; // 已收到任意有效 delta（决定重试/回退边界）
    int m_streamParseErrors = 0;
    int m_streamEventCount = 0;
    QString m_streamContent;
    QString m_streamReasoning;
    QString m_finishReason;
    int m_streamPromptTokens = 0;
    int m_streamCompletionTokens = 0;
    QMap<int, ToolCallAcc> m_toolCallAcc;
    int m_droppedToolCalls = 0;
    SseStreamParser m_sseParser;

    bool m_contentTypeKnown = false;
    bool m_contentTypeIsSse = false;
    QByteArray m_pendingStreamBytes; // Content-Type 判定前的缓存
    QByteArray m_jsonBody;           // F2：JSON 整包累积

    QTimer* m_streamIdleTimer = nullptr;
    QTimer* m_streamDeadlineTimer = nullptr;
};

} // namespace DeepLux

#endif // DEEPLUX_OPENAI_LLM_CLIENT_H
