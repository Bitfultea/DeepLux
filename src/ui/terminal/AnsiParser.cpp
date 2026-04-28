#include "AnsiParser.h"

#include <QDebug>

namespace DeepLux {

AnsiParser::AnsiParser(TerminalScreen* screen, QObject* parent)
    : QObject(parent)
    , m_screen(screen)
{
}

AnsiParser::~AnsiParser() = default;

void AnsiParser::parse(const QByteArray& data)
{
    if (!m_screen) return;

    for (char b : data) {
        processByte(b);
    }

    emit screenUpdated();
}

void AnsiParser::parse(const QString& text)
{
    parse(text.toUtf8());
}

void AnsiParser::writePlainText(const QString& text)
{
    if (!m_screen) return;
    for (QChar c : text) {
        m_screen->putChar(c);
    }
    emit screenUpdated();
}

void AnsiParser::reset()
{
    m_state = State::Ground;
    m_csiParams.clear();
    m_csiPrefix.clear();
    m_oscParam = 0;
    m_oscString.clear();
    m_dcsString.clear();
    m_currentParam = 0;
    m_paramHasValue = false;
    m_utf8Buffer.clear();
}

void AnsiParser::processByte(char b)
{
    uchar ub = static_cast<uchar>(b);

    // 非 Ground 状态或 ASCII 字节：直接处理（先清空可能残留的 UTF-8 缓冲区）
    if (m_state != State::Ground || ub < 0x80) {
        if (!m_utf8Buffer.isEmpty()) {
            // 不完整的 UTF-8 序列，输出替换字符
            processChar(QChar(0xFFFD));
            m_utf8Buffer.clear();
        }
        processChar(QChar(ub));
        return;
    }

    // Ground 状态且字节 >= 0x80：累积 UTF-8 序列
    m_utf8Buffer.append(b);

    // 检查 UTF-8 序列长度
    int expectedLen = 0;
    uchar first = static_cast<uchar>(m_utf8Buffer[0]);
    if ((first & 0xE0) == 0xC0) expectedLen = 2;
    else if ((first & 0xF0) == 0xE0) expectedLen = 3;
    else if ((first & 0xF8) == 0xF0) expectedLen = 4;
    else {
        // 非法 UTF-8 起始字节
        processChar(QChar(0xFFFD));
        m_utf8Buffer.clear();
        return;
    }

    // 等待更多字节
    if (m_utf8Buffer.size() < expectedLen) {
        return;
    }

    // 验证后续字节
    for (int i = 1; i < expectedLen; ++i) {
        if ((static_cast<uchar>(m_utf8Buffer[i]) & 0xC0) != 0x80) {
            processChar(QChar(0xFFFD));
            m_utf8Buffer.clear();
            return;
        }
    }

    // 解码 Unicode code point
    uint codePoint = 0;
    if (expectedLen == 2) {
        codePoint = ((first & 0x1F) << 6) | (m_utf8Buffer[1] & 0x3F);
    } else if (expectedLen == 3) {
        codePoint = ((first & 0x0F) << 12)
                  | ((m_utf8Buffer[1] & 0x3F) << 6)
                  | (m_utf8Buffer[2] & 0x3F);
    } else if (expectedLen == 4) {
        codePoint = ((first & 0x07) << 18)
                  | ((m_utf8Buffer[1] & 0x3F) << 12)
                  | ((m_utf8Buffer[2] & 0x3F) << 6)
                  | (m_utf8Buffer[3] & 0x3F);
    }

    // 转换为 QChar(s)
    if (codePoint < 0x10000) {
        processChar(QChar(codePoint));
    } else {
        // Surrogate pair for non-BMP characters
        codePoint -= 0x10000;
        processChar(QChar(0xD800 + (codePoint >> 10)));
        processChar(QChar(0xDC00 + (codePoint & 0x3FF)));
    }

    m_utf8Buffer.clear();
}

void AnsiParser::processChar(QChar c)
{
    if (!m_screen) return;

    switch (m_state) {
    case State::Ground:
        if (c.unicode() == 0x1B) {  // ESC
            m_state = State::Escape;
        } else if (c == QChar('\x07')) {  // BEL
            emit bell();
        } else if (c == QChar('\x08')) {  // BS (Backspace)
            m_screen->moveCursor(0, -1);
        } else if (c.unicode() < 32) {
            // 其他控制字符直接传给 screen（包括 \n, \r, \t）
            m_screen->putChar(c);
        } else {
            m_screen->putChar(c);
        }
        break;

    case State::Escape:
        if (c == QChar('[')) {
            m_state = State::CSI;
            m_csiParams.clear();
            m_csiPrefix.clear();
            m_currentParam = 0;
            m_paramHasValue = false;
        } else if (c == QChar(']')) {
            m_state = State::OSC;
            m_oscParam = 0;
            m_oscString.clear();
        } else if (c == QChar('P')) {
            m_state = State::DCS;
            m_dcsString.clear();
        } else if (c == QChar('c')) {
            // RIS - Reset to Initial State
            m_screen->clearScreen();
            m_screen->currentAttrs().reset();
            m_screen->setCursorVisible(true);
            m_state = State::Ground;
        } else if (c == QChar('M')) {
            // RI - Reverse Index (scroll down if at top)
            m_screen->moveCursor(-1, 0);
            m_state = State::Ground;
        } else if (c == QChar('7')) {
            // DECSC - Save Cursor
            m_screen->saveCursor();
            m_state = State::Ground;
        } else if (c == QChar('8')) {
            // DECRC - Restore Cursor
            m_screen->restoreCursor();
            m_state = State::Ground;
        } else {
            // 未知转义序列，忽略
            m_state = State::Ground;
        }
        break;

    case State::CSI:
        if (c >= QChar('0') && c <= QChar('9')) {
            m_currentParam = m_currentParam * 10 + (c.unicode() - '0');
            m_paramHasValue = true;
            m_state = State::CSIParam;
        } else if (c == QChar(';')) {
            m_csiParams.append(m_paramHasValue ? m_currentParam : 0);
            m_currentParam = 0;
            m_paramHasValue = false;
        } else if (c == QChar(':')) {
            m_csiParams.append(m_paramHasValue ? m_currentParam : 0);
            m_currentParam = 0;
            m_paramHasValue = false;
        } else if (c == QChar('?') || c == QChar('>') || c == QChar('!')) {
            m_csiPrefix.append(static_cast<char>(c.unicode()));
        } else {
            // 收集最后一个参数
            if (m_paramHasValue || !m_csiParams.isEmpty()) {
                m_csiParams.append(m_paramHasValue ? m_currentParam : 0);
            }
            // 处理 CSI 终结字符
            handleCSIChar(c);
            m_state = State::Ground;
        }
        break;

    case State::CSIParam:
        if (c >= QChar('0') && c <= QChar('9')) {
            m_currentParam = m_currentParam * 10 + (c.unicode() - '0');
            m_paramHasValue = true;
        } else if (c == QChar(';')) {
            m_csiParams.append(m_paramHasValue ? m_currentParam : 0);
            m_currentParam = 0;
            m_paramHasValue = false;
        } else if (c == QChar(':')) {
            m_csiParams.append(m_paramHasValue ? m_currentParam : 0);
            m_currentParam = 0;
            m_paramHasValue = false;
        } else {
            m_csiParams.append(m_paramHasValue ? m_currentParam : 0);
            handleCSIChar(c);
            m_state = State::Ground;
        }
        break;

    case State::OSC:
        if (c >= QChar('0') && c <= QChar('9')) {
            m_oscParam = m_oscParam * 10 + (c.unicode() - '0');
        } else if (c == QChar(';')) {
            m_state = State::OSCString;
        } else if (c == QChar('\x07')) {
            // BEL terminates OSC
            handleOSC();
            m_state = State::Ground;
        } else {
            m_state = State::Ground;
        }
        break;

    case State::OSCString:
        if (c == QChar('\x07')) {
            handleOSC();
            m_state = State::Ground;
        } else if (c.unicode() == 0x1B) {
            // ESC might start ST (ESC \)
            m_state = State::Ignore;
        } else {
            m_oscString.append(c);
        }
        break;

    case State::DCS:
        if (c.unicode() == 0x1B) {
            m_state = State::Ignore;  // Expect ST
        } else if (c == QChar('\x07')) {
            handleDCS();
            m_state = State::Ground;
        } else {
            m_dcsString.append(static_cast<char>(c.unicode()));
        }
        break;

    case State::DCString:
        if (c.unicode() == 0x1B) {
            m_state = State::Ignore;  // Expect ST
        } else {
            m_dcsString.append(static_cast<char>(c.unicode()));
        }
        break;

    case State::Ignore:
        if (c == QChar('\\')) {
            // ST (ESC \) - end of OSC/DCS
            if (!m_oscString.isEmpty()) {
                handleOSC();
            } else if (!m_dcsString.isEmpty()) {
                handleDCS();
            }
            m_state = State::Ground;
        } else if (c.unicode() == 0x1B) {
            // Another ESC, stay in ignore
        } else {
            // Collecting string content
            if (!m_oscString.isEmpty() || m_oscParam > 0) {
                m_oscString.append(c);
            } else {
                m_dcsString.append(static_cast<char>(c.unicode()));
            }
        }
        break;
    }
}

void AnsiParser::handleCSIChar(QChar c)
{
    if (m_csiPrefix == "?") {
        // DEC Private Mode
        bool set = (c == QChar('h'));
        bool reset = (c == QChar('l'));
        if (!set && !reset) {
            return;
        }
        for (int param : m_csiParams) {
            switch (param) {
            case 25:
                m_screen->setCursorVisible(set);
                break;
            case 1049:
                m_screen->useAlternateScreen(set);
                break;
            case 2004:
                // Bracketed paste mode - 由 TerminalWidget 处理
                break;
            }
        }
        return;
    }

    if (c == QChar('m')) {
        applySGR(m_csiParams);
    } else if (c == QChar('H') || c == QChar('f')) {
        // CUP / HVP
        int row = m_csiParams.isEmpty() ? 1 : m_csiParams.value(0, 1);
        int col = m_csiParams.size() > 1 ? m_csiParams.at(1) : 1;
        m_screen->setCursor(row - 1, col - 1);
    } else if (c == QChar('A')) {
        // CUU - Cursor Up
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->moveCursor(-n, 0);
    } else if (c == QChar('B')) {
        // CUD - Cursor Down
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->moveCursor(n, 0);
    } else if (c == QChar('C')) {
        // CUF - Cursor Forward
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->moveCursor(0, n);
    } else if (c == QChar('D')) {
        // CUB - Cursor Back
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->moveCursor(0, -n);
    } else if (c == QChar('E')) {
        // CNL - Cursor Next Line
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->setCursor(m_screen->cursorRow() + n, 0);
    } else if (c == QChar('F')) {
        // CPL - Cursor Previous Line
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->setCursor(m_screen->cursorRow() - n, 0);
    } else if (c == QChar('G')) {
        // CHA - Cursor Horizontal Absolute
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->setCursor(m_screen->cursorRow(), n - 1);
    } else if (c == QChar('J')) {
        // ED - Erase in Display
        int n = m_csiParams.isEmpty() ? 0 : m_csiParams.at(0);
        if (n == 0) {
            m_screen->clearToEndOfScreen();
        } else if (n == 1) {
            m_screen->clearToStartOfScreen();
        } else if (n == 2) {
            m_screen->clearScreen();
        } else if (n == 3) {
            m_screen->clearScreen();
            m_screen->clearScrollback();
        }
    } else if (c == QChar('K')) {
        // EL - Erase in Line
        int n = m_csiParams.isEmpty() ? 0 : m_csiParams.at(0);
        if (n == 0) {
            m_screen->clearToEndOfLine();
        } else if (n == 1) {
            m_screen->clearToStartOfLine();
        } else if (n == 2) {
            m_screen->clearLine();
        }
    } else if (c == QChar('L')) {
        // IL - Insert Line
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->insertLines(n);
    } else if (c == QChar('M')) {
        // DL - Delete Line
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->deleteLines(n);
    } else if (c == QChar('P')) {
        // DCH - Delete Character
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->deleteChars(n);
    } else if (c == QChar('@')) {
        // ICH - Insert Character
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->insertChars(n);
    } else if (c == QChar('r')) {
        // DECSTBM - Set Scroll Region
        int top = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        int bottom = m_csiParams.size() > 1 ? m_csiParams.at(1) : m_screen->rows();
        m_screen->setScrollRegion(top - 1, bottom - 1);
        m_screen->setCursor(0, 0);
    } else if (c == QChar('s')) {
        // SC - Save Cursor
        m_screen->saveCursor();
    } else if (c == QChar('u')) {
        // RC - Restore Cursor
        m_screen->restoreCursor();
    } else if (c == QChar('n')) {
        // DSR - Device Status Report
        // 通常发送 ESC [ row ; col R 作为响应，但这里我们不需要回复
    } else if (c == QChar('S')) {
        // SU - Scroll Up
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->scrollUp(n);
    } else if (c == QChar('T')) {
        // SD - Scroll Down
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        m_screen->scrollDown(n);
    } else if (c == QChar('X')) {
        // ECH - Erase Character
        int n = m_csiParams.isEmpty() ? 1 : m_csiParams.at(0);
        for (int i = 0; i < n && (m_screen->cursorCol() + i) < m_screen->cols(); ++i) {
            m_screen->cellAt(m_screen->cursorRow(), m_screen->cursorCol() + i) = Cell();
        }
    }
}

void AnsiParser::handleOSC()
{
    if (m_oscParam == 0 || m_oscParam == 2) {
        emit titleChanged(m_oscString);
    } else if (m_oscParam == 888) {
        // DeepLux CLI wrapper: OSC 888;deeplux;<command> BEL
        QStringList parts = m_oscString.split(';');
        if (parts.size() >= 2 && parts[0] == QStringLiteral("deeplux")) {
            QString command = parts.mid(1).join(';');
            emit cliCommandReceived(command);
        }
    }
    m_oscString.clear();
    m_oscParam = 0;
}

void AnsiParser::handleDCS()
{
    // DCS 序列通常用于终端特性查询，目前忽略
    m_dcsString.clear();
}

void AnsiParser::applySGR(const QVector<int>& params)
{
    if (params.isEmpty()) {
        m_screen->currentAttrs().reset();
        return;
    }

    int i = 0;
    while (i < params.size()) {
        int param = params.at(i);

        // 处理 256 色和 True Color
        if (param == AnsiConstants::SGR_FG_256 || param == AnsiConstants::SGR_BG_256) {
            if (i + 2 < params.size() && params.at(i + 1) == 5) {
                int colorIndex = params.at(i + 2);
                if (param == AnsiConstants::SGR_FG_256) {
                    m_screen->currentAttrs().fgType = ColorType::Palette256;
                    m_screen->currentAttrs().fgValue = colorIndex;
                } else {
                    m_screen->currentAttrs().bgType = ColorType::Palette256;
                    m_screen->currentAttrs().bgValue = colorIndex;
                }
                i += 3;
                continue;
            } else if (i + 4 < params.size() && params.at(i + 1) == 2) {
                int r = params.at(i + 2);
                int g = params.at(i + 3);
                int b = params.at(i + 4);
                if (param == AnsiConstants::SGR_FG_256) {
                    m_screen->currentAttrs().fgType = ColorType::TrueColor;
                    m_screen->currentAttrs().fgValue = (r << 16) | (g << 8) | b;
                } else {
                    m_screen->currentAttrs().bgType = ColorType::TrueColor;
                    m_screen->currentAttrs().bgValue = (r << 16) | (g << 8) | b;
                }
                i += 5;
                continue;
            }
        }

        switch (param) {
        case AnsiConstants::SGR_RESET:
            m_screen->currentAttrs().reset();
            break;
        case AnsiConstants::SGR_BOLD:
            m_screen->currentAttrs().bold = true;
            break;
        case AnsiConstants::SGR_DIM:
            m_screen->currentAttrs().dim = true;
            break;
        case AnsiConstants::SGR_ITALIC:
            m_screen->currentAttrs().italic = true;
            break;
        case AnsiConstants::SGR_UNDERLINE:
            m_screen->currentAttrs().underline = true;
            break;
        case AnsiConstants::SGR_BLINK:
            break;
        case AnsiConstants::SGR_REVERSE:
            m_screen->currentAttrs().reverse = true;
            break;
        case AnsiConstants::SGR_STRIKETHROUGH:
            m_screen->currentAttrs().strikethrough = true;
            break;
        case AnsiConstants::SGR_NORMAL_INTENSITY:
            m_screen->currentAttrs().bold = false;
            m_screen->currentAttrs().dim = false;
            break;
        case AnsiConstants::SGR_NOT_ITALIC:
            m_screen->currentAttrs().italic = false;
            break;
        case AnsiConstants::SGR_NOT_UNDERLINE:
            m_screen->currentAttrs().underline = false;
            break;
        case AnsiConstants::SGR_NOT_BLINK:
            break;
        case AnsiConstants::SGR_NOT_REVERSE:
            m_screen->currentAttrs().reverse = false;
            break;
        case AnsiConstants::SGR_NOT_STRIKETHROUGH:
            m_screen->currentAttrs().strikethrough = false;
            break;

        // 基本前景色 (30-37)
        case AnsiConstants::SGR_FG_BLACK:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 0;
            break;
        case AnsiConstants::SGR_FG_RED:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 1;
            break;
        case AnsiConstants::SGR_FG_GREEN:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 2;
            break;
        case AnsiConstants::SGR_FG_YELLOW:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 3;
            break;
        case AnsiConstants::SGR_FG_BLUE:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 4;
            break;
        case AnsiConstants::SGR_FG_MAGENTA:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 5;
            break;
        case AnsiConstants::SGR_FG_CYAN:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 6;
            break;
        case AnsiConstants::SGR_FG_WHITE:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 7;
            break;

        // 亮色前景色 (90-97)
        case AnsiConstants::SGR_FG_BRIGHT_BLACK:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 8;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_RED:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 9;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_GREEN:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 10;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_YELLOW:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 11;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_BLUE:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 12;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_MAGENTA:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 13;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_CYAN:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 14;
            break;
        case AnsiConstants::SGR_FG_BRIGHT_WHITE:
            m_screen->currentAttrs().fgType = ColorType::Basic;
            m_screen->currentAttrs().fgValue = 15;
            break;

        // 基本背景色 (40-47)
        case AnsiConstants::SGR_BG_BLACK:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 0;
            break;
        case AnsiConstants::SGR_BG_RED:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 1;
            break;
        case AnsiConstants::SGR_BG_GREEN:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 2;
            break;
        case AnsiConstants::SGR_BG_YELLOW:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 3;
            break;
        case AnsiConstants::SGR_BG_BLUE:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 4;
            break;
        case AnsiConstants::SGR_BG_MAGENTA:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 5;
            break;
        case AnsiConstants::SGR_BG_CYAN:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 6;
            break;
        case AnsiConstants::SGR_BG_WHITE:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 7;
            break;

        // 亮色背景色 (100-107)
        case AnsiConstants::SGR_BG_BRIGHT_BLACK:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 8;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_RED:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 9;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_GREEN:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 10;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_YELLOW:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 11;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_BLUE:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 12;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_MAGENTA:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 13;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_CYAN:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 14;
            break;
        case AnsiConstants::SGR_BG_BRIGHT_WHITE:
            m_screen->currentAttrs().bgType = ColorType::Basic;
            m_screen->currentAttrs().bgValue = 15;
            break;

        default:
            break;
        }
        i++;
    }
}

} // namespace DeepLux
