#ifndef DEEPLUX_OPENAI_LLM_CLIENT_H
#define DEEPLUX_OPENAI_LLM_CLIENT_H

#include "ILLMClient.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace DeepLux {

class OpenAILLMClient : public ILLMClient
{
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

    void sendRequest(const AgentConversation& ctx,
                     const QList<ToolDefinition>& tools) override;

private slots:
    void onReplyFinished();
    void onNetworkError(QNetworkReply::NetworkError error);

private:
    void parseResponse(const QByteArray& data);

    QString m_apiKey;
    QString m_endpoint = "https://api.openai.com/v1/chat/completions";
    QString m_model = "gpt-4o";
    double m_temperature = 0.3;
    int m_maxTokens = 4096;
    bool m_toolsEnabled = true;

    QNetworkAccessManager* m_networkManager = nullptr;
    QNetworkReply* m_currentReply = nullptr;
};

} // namespace DeepLux

#endif // DEEPLUX_OPENAI_LLM_CLIENT_H
