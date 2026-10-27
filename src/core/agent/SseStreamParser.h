#ifndef DEEPLUX_SSE_STREAM_PARSER_H
#define DEEPLUX_SSE_STREAM_PARSER_H

#include <QByteArray>
#include <QList>
#include <QString>

namespace DeepLux {

/**
 * @brief Server-Sent Events 增量解析器（纯逻辑、无网络、无 JSON 解析）
 *
 * 设计依据 docs/agent-iteration2-design.md §3.2（P1~P10）：
 * - 任意字节片输入（可含半个字段/半个 UTF-8 字符/多个事件）；
 * - 只按 '\n' 切行并剥离行尾单个 '\r'，不完整的最后一行留在缓冲区；
 * - 空行派发事件，多行 data 以 '\n' 连接；':' 开头为注释（丢弃但算活动）；
 * - data == "[DONE]" 派发 done 事件并在 reset 前丢弃后续输入；
 * - 单行 > 1 MiB 整行丢弃并计数，不中断流；
 * - 只在整行完成后 QString::fromUtf8 解码，避免切裂 UTF-8 多字节字符。
 */
class SseStreamParser {
public:
    struct Event {
        QString name;    // "event:" 字段；缺省空 → 消费方视为 "message"
        QByteArray data; // 多行 data 以 '\n' 连接后的原始负载
        bool done = false;
    };

    /// 喂入任意字节片，返回本次解析出的 0..N 个事件（按到达顺序）
    QList<Event> feed(const QByteArray& bytes);
    /// 重置全部状态（新请求前调用）
    void reset();

    int droppedLines() const {
        return m_droppedLines;
    }
    int eventCount() const {
        return m_eventCount;
    }

    /// 单行长度上限（1 MiB）
    static constexpr int kMaxLineBytes = 1024 * 1024;

private:
    QByteArray m_buffer; // 未完结的行（半包）
    QByteArray m_data;   // 当前事件已累积的 data 行
    QString m_eventName; // 当前事件的 event: 名
    bool m_doneEmitted = false;
    bool m_skippingLine = false; // 正在丢弃超长行，直到下一个 '\n'
    int m_droppedLines = 0;
    int m_eventCount = 0;
};

} // namespace DeepLux

#endif // DEEPLUX_SSE_STREAM_PARSER_H
