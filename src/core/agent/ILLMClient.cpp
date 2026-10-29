#include "ILLMClient.h"

namespace DeepLux {

QJsonArray AgentConversation::toOpenAIMessages() const {
    // 第二迭代默认不回传 reasoning（设计 §3.7：agent/reasoningEcho 默认 none）
    return toOpenAIMessages(ReasoningEcho::None);
}

QJsonArray AgentConversation::toOpenAIMessages(ReasoningEcho echo, const QString& imageDetail) const {
    QJsonArray arr;

    if (!systemPrompt.isEmpty()) {
        QJsonObject sys;
        sys["role"] = "system";
        sys["content"] = systemPrompt;
        arr.append(sys);
    }

    // CurrentTurn：只回传"最后一个 user 消息之后"的 assistant reasoning
    int lastUserIndex = -1;
    if (echo == ReasoningEcho::CurrentTurn) {
        for (int i = 0; i < messages.size(); ++i) {
            if (messages.at(i).role == QStringLiteral("user"))
                lastUserIndex = i;
        }
    }

    for (int i = 0; i < messages.size(); ++i) {
        const AgentMessage& msg = messages.at(i);
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
                QJsonObject imageUrl{{"url", QString("data:%1;base64,%2").arg(img.mimeType).arg(base64)}};
                if (!imageDetail.isEmpty())
                    imageUrl["detail"] = imageDetail;
                imageObj["image_url"] = imageUrl;
                contentArray.append(imageObj);
            }
            obj["content"] = contentArray;
        } else {
            obj["content"] = msg.content;
        }

        if (msg.role == "assistant" && !msg.toolCalls.isEmpty()) {
            obj["tool_calls"] = msg.toolCalls; // QJsonArray, direct OpenAI format
        }
        // reasoning_content 回传策略：默认不回传；CurrentTurn 只回传最后一条 user 之后的；
        // Full 保留第一迭代行为（全部 assistant 历史回传）
        const bool echoReasoning =
            msg.role == QStringLiteral("assistant") && !msg.reasoningContent.isNull() &&
            (echo == ReasoningEcho::Full || (echo == ReasoningEcho::CurrentTurn && i > lastUserIndex));
        if (echoReasoning) {
            obj["reasoning_content"] = msg.reasoningContent;
        }
        if (msg.role == "tool") {
            obj["tool_call_id"] = msg.toolCallId;
        }

        arr.append(obj);
    }

    return arr;
}

} // namespace DeepLux
