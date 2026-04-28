#ifndef DEEPLUX_ANSI_PARSER_H
#define DEEPLUX_ANSI_PARSER_H

#include "AnsiConstants.h"
#include "TerminalScreen.h"

#include <QObject>
#include <QString>
#include <QVector>

namespace DeepLux {

/**
 * @brief ANSI 转义序列解析器
 *
 * 使用状态机解析 ANSI 转义序列，直接操作 TerminalScreen。
 *
 * 支持：
 * - 完整 SGR (Select Graphic Rendition) 参数
 * - 256 色模式 (38;5;N)
 * - True Color 模式 (38;2;R;G;B)
 * - 光标位置控制 (CUP, HVP, CUU, CUD, CUF, CUB)
 * - 清屏/清行 (ED, EL)
 * - 滚动区域
 * - 交替屏幕缓冲区 (?1049)
 * - OSC 0/2 标题设置
 */
class AnsiParser : public QObject
{
    Q_OBJECT

public:
    explicit AnsiParser(TerminalScreen* screen, QObject* parent = nullptr);
    ~AnsiParser() override;

    /**
     * @brief 解析数据并直接操作 TerminalScreen
     */
    void parse(const QByteArray& data);
    void parse(const QString& text);

    /**
     * @brief 将纯文本（无 ANSI 序列）写入 screen
     */
    void writePlainText(const QString& text);

    /**
     * @brief 重置解析器状态
     */
    void reset();

signals:
    void screenUpdated();
    void titleChanged(const QString& title);
    void bell();
    void cliCommandReceived(const QString& command);

private:
    // 解析状态
    enum class State {
        Ground,          // 正常状态
        Escape,         // 收到 ESC
        CSI,            // 收到 ESC [
        CSIParam,       // CSI 参数收集中
        OSC,            // 收到 ESC ]
        OSCString,      // OSC 字符串收集中
        DCS,            // 收到 ESC P
        DCString,       // DCS 字符串收集中
        Ignore          // 忽略到 ST (ESC \ 或 BEL)
    };

    void processByte(char b);
    void processChar(QChar c);

    void handleCSIChar(QChar c);
    void handleOSC();
    void handleDCS();

    void applySGR(const QVector<int>& params);

    TerminalScreen* m_screen = nullptr;
    State m_state = State::Ground;

    // CSI 参数
    QVector<int> m_csiParams;
    QByteArray m_csiPrefix;  // 用于私有参数如 ? > !

    // OSC 参数
    int m_oscParam = 0;
    QString m_oscString;

    // DCS 参数
    QByteArray m_dcsString;

    // 解析数字参数时的临时缓冲区
    int m_currentParam = 0;
    bool m_paramHasValue = false;

    // UTF-8 多字节解码缓冲区
    QByteArray m_utf8Buffer;
};

} // namespace DeepLux

#endif // DEEPLUX_ANSI_PARSER_H
