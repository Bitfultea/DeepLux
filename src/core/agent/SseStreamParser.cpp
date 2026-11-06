#include "SseStreamParser.h"

namespace DeepLux {

namespace {

// P4：field: value —— 冒号后仅剥离一个空格
void parseField(const QByteArray& line, QByteArray* field, QByteArray* value) {
    const int colon = line.indexOf(':');
    if (colon < 0) {
        *field = line;
        value->clear();
        return;
    }
    *field = line.left(colon);
    *value = line.mid(colon + 1);
    if (value->startsWith(' '))
        value->remove(0, 1);
}

} // namespace

QList<SseStreamParser::Event> SseStreamParser::feed(const QByteArray& bytes) {
    QList<Event> events;
    if (m_doneEmitted)
        return events; // P6：reset() 前所有输入丢弃

    m_buffer += bytes;

    forever {
        // P7：正在丢弃超长行的剩余部分，直到行尾
        if (m_skippingLine) {
            const int newline = m_buffer.indexOf('\n');
            if (newline < 0) {
                m_buffer.clear();
                break;
            }
            m_buffer.remove(0, newline + 1);
            m_skippingLine = false;
            continue;
        }

        // P1：按 '\n' 切行；不完整的最后一行留在 m_buffer
        const int newline = m_buffer.indexOf('\n');
        if (newline < 0) {
            // 无行尾且缓冲已超上限：整行丢弃（后续剩余字节由 m_skippingLine 丢弃）
            if (m_buffer.size() > kMaxLineBytes) {
                ++m_droppedLines;
                m_buffer.clear();
                m_skippingLine = true;
            }
            break;
        }

        QByteArray line = m_buffer.left(newline);
        m_buffer.remove(0, newline + 1);
        if (line.size() > kMaxLineBytes) {
            ++m_droppedLines;
            continue;
        }
        if (line.endsWith('\r'))
            line.chop(1); // CRLF 兼容

        // P2：空行 = 事件边界
        if (line.isEmpty()) {
            if (m_data.isEmpty() && m_eventName.isEmpty())
                continue; // 连续空行/保活
            Event event;
            event.name = m_eventName;
            event.data = m_data;
            if (m_data == "[DONE]") {
                event.done = true;
                m_doneEmitted = true;
            }
            ++m_eventCount;
            events.append(event);
            m_data.clear();
            m_eventName.clear();
            if (event.done)
                break; // 后续输入丢弃
            continue;
        }

        // P3：':' 开头为注释（含心跳），丢弃但算作活动（消费方按 feed 调用刷新看门狗）
        if (line.startsWith(':'))
            continue;

        QByteArray field;
        QByteArray value;
        parseField(line, &field, &value);

        // P5：data 累积（多行 join '\n'）、event 覆盖、id/retry/未知字段忽略
        if (field == "data") {
            if (!m_data.isEmpty())
                m_data += '\n';
            m_data += value;
        } else if (field == "event") {
            m_eventName = QString::fromUtf8(value);
        }
    }

    return events;
}

void SseStreamParser::reset() {
    m_buffer.clear();
    m_data.clear();
    m_eventName.clear();
    m_doneEmitted = false;
    m_skippingLine = false;
    m_droppedLines = 0;
    m_eventCount = 0;
}

} // namespace DeepLux
