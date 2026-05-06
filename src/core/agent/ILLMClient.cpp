#include "ILLMClient.h"

namespace DeepLux {

QJsonArray AgentConversation::toOpenAIMessages() const
{
    QJsonArray arr;

    if (!systemPrompt.isEmpty()) {
        QJsonObject sys;
        sys["role"] = "system";
        sys["content"] = systemPrompt;
        arr.append(sys);
    }

    for (const AgentMessage& msg : messages) {
        QJsonObject obj;
        obj["role"] = msg.role;

        if (!msg.images.isEmpty()) {
            // Multimodal: OpenAI vision format with content array
            QJsonArray contentArray;
            if (!msg.content.isEmpty()) {
                QJsonObject textObj;
                textObj["type"] = "text";
                textObj["text"] = msg.content;
                contentArray.append(textObj);
            }
            for (const AgentImageAttachment& img : msg.images) {
                QJsonObject imageObj;
                imageObj["type"] = "image_url";
                QString base64 = img.data.toBase64();
                imageObj["image_url"] = QJsonObject{
                    {"url", QString("data:%1;base64,%2").arg(img.mimeType).arg(base64)}
                };
                contentArray.append(imageObj);
            }
            obj["content"] = contentArray;
        } else {
            obj["content"] = msg.content;
        }

        if (msg.role == "assistant" && !msg.toolCalls.isEmpty()) {
            obj["tool_calls"] = msg.toolCalls;  // QJsonArray, direct OpenAI format
        }
        if (msg.role == "tool") {
            obj["tool_call_id"] = msg.toolCallId;
        }

        arr.append(obj);
    }

    return arr;
}

} // namespace DeepLux
