#ifndef DEEPLUX_OPENAI_LLM_CLIENT_H
#define DEEPLUX_OPENAI_LLM_CLIENT_H

#include "ILLMClient.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>

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
        return m_visionEnabled;
    }
    void setVisionEnabled(bool enabled);
    void setReasoningEffort(const QString& effort);
    void setThinkingEnabled(bool enabled);

    static QString normalizeEndpoint(const QString& endpoint);

    void sendRequest(const AgentConversation& ctx, const QList<ToolDefinition>& tools) override;

DEEPLUX_TEST_VISIBILITY slots : void onReplyFinished();
    void onNetworkError(QNetworkReply::NetworkError error);

    DEEPLUX_TEST_VISIBILITY : QJsonObject buildRequestBody(const AgentConversation& ctx,
                                                           const QList<ToolDefinition>& tools) const;
    void parseResponse(const QByteArray& data);

    QString m_apiKey;
    QString m_endpoint = "https://api.openai.com/v1/chat/completions";
    QString m_model = "gpt-4o";
    double m_temperature = 0.3;
    int m_maxTokens = 4096;
    bool m_toolsEnabled = true;
    bool m_visionEnabled = true;
    QString m_reasoningEffort; // "high" or "max" (DeepSeek thinking mode)
    bool m_thinkingEnabled = false;

    QNetworkAccessManager* m_networkManager = nullptr;
    QNetworkReply* m_currentReply = nullptr;
};

} // namespace DeepLux

#endif // DEEPLUX_OPENAI_LLM_CLIENT_H
