#include "TerminalScreen.h"

namespace DeepLux {

TerminalScreen::TerminalScreen(int rows, int cols)
    : m_rows(rows)
    , m_cols(cols)
    , m_scrollTop(0)
    , m_scrollBottom(rows - 1)
{
    initScreen(m_screen, rows, cols);
    initScreen(m_altScreen, rows, cols);
}

void TerminalScreen::initScreen(QVector<QVector<Cell>>& screen, int rows, int cols)
{
    screen.resize(rows);
    for (int r = 0; r < rows; ++r) {
        screen[r].resize(cols);
        for (int c = 0; c < cols; ++c) {
            screen[r][c] = Cell();
        }
    }
}

void TerminalScreen::resize(int rows, int cols)
{
    if (rows <= 0 || cols <= 0) return;

    int oldRows = m_rows;
    int oldCols = m_cols;

    // 保存旧内容
    QVector<QVector<Cell>> oldScreen = m_screen;

    m_rows = rows;
    m_cols = cols;
    m_scrollTop = 0;
    m_scrollBottom = rows - 1;

    initScreen(m_screen, rows, cols);
    initScreen(m_altScreen, rows, cols);

    // 复制旧内容到新屏幕（左上角对齐）
    int copyRows = qMin(oldRows, rows);
    int copyCols = qMin(oldCols, cols);
    for (int r = 0; r < copyRows; ++r) {
        for (int c = 0; c < copyCols; ++c) {
            m_screen[r][c] = oldScreen[r][c];
        }
    }

    // 限制光标位置
    m_cursorRow = qMin(m_cursorRow, rows - 1);
    m_cursorCol = qMin(m_cursorCol, cols - 1);

    for (int r = 0; r < rows; ++r) {
        markDirty(r);
    }
}

void TerminalScreen::putChar(QChar c)
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) return;
    if (m_cursorCol < 0) m_cursorCol = 0;
    // m_cursorCol >= m_cols 时由后面的换行逻辑处理，不要直接丢弃

    if (c == QChar('\n')) {
        // 换行
        m_cursorCol = 0;
        m_cursorRow++;
        if (m_cursorRow > m_scrollBottom) {
            m_cursorRow = m_scrollBottom;
            scrollUp(1);
        }
    } else if (c == QChar('\r')) {
        // 回车
        m_cursorCol = 0;
    } else if (c == QChar('\t')) {
        // Tab
        int spaces = 8 - (m_cursorCol % 8);
        for (int i = 0; i < spaces && m_cursorCol < m_cols; ++i) {
            m_screen[m_cursorRow][m_cursorCol].character = QChar(' ');
            m_screen[m_cursorRow][m_cursorCol].attrs = m_currentAttrs;
            m_cursorCol++;
        }
    } else if (c.unicode() >= 32 || c.unicode() == 0) {
        // 可打印字符
        if (isWideChar(c)) {
            // 宽字符占用 2 列
            if (m_cursorCol >= m_cols - 1) {
                // 宽字符在边缘无法完整显示，换行
                m_cursorCol = 0;
                m_cursorRow++;
                if (m_cursorRow > m_scrollBottom) {
                    m_cursorRow = m_scrollBottom;
                    scrollUp(1);
                }
            }
            m_screen[m_cursorRow][m_cursorCol].character = c;
            m_screen[m_cursorRow][m_cursorCol].attrs = m_currentAttrs;
            m_screen[m_cursorRow][m_cursorCol].isWide = true;
            // 标记下一列为宽字符的右半部分
            if (m_cursorCol + 1 < m_cols) {
                m_screen[m_cursorRow][m_cursorCol + 1].character = QChar(' ');
                m_screen[m_cursorRow][m_cursorCol + 1].attrs = m_currentAttrs;
                m_screen[m_cursorRow][m_cursorCol + 1].isWide = true;
            }
            m_cursorCol += 2;
        } else {
            m_screen[m_cursorRow][m_cursorCol].character = c;
            m_screen[m_cursorRow][m_cursorCol].attrs = m_currentAttrs;
            m_screen[m_cursorRow][m_cursorCol].isWide = false;
            m_cursorCol++;
        }

        if (m_cursorCol >= m_cols) {
            m_cursorCol = 0;
            m_cursorRow++;
            if (m_cursorRow > m_scrollBottom) {
                m_cursorRow = m_scrollBottom;
                scrollUp(1);
            }
        }
    }
    // 其他控制字符忽略
}

void TerminalScreen::putString(const QString& s)
{
    for (QChar c : s) {
        putChar(c);
    }
}

void TerminalScreen::setCursor(int row, int col)
{
    m_cursorRow = qBound(0, row, m_rows - 1);
    m_cursorCol = qBound(0, col, m_cols - 1);
}

void TerminalScreen::moveCursor(int dr, int dc)
{
    setCursor(m_cursorRow + dr, m_cursorCol + dc);
}

void TerminalScreen::saveCursor()
{
    m_savedRow = m_cursorRow;
    m_savedCol = m_cursorCol;
}

void TerminalScreen::restoreCursor()
{
    m_cursorRow = qBound(0, m_savedRow, m_rows - 1);
    m_cursorCol = qBound(0, m_savedCol, m_cols - 1);
}

void TerminalScreen::clearScreen()
{
    for (int r = 0; r < m_rows; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
        markDirty(r);
    }
    m_cursorRow = 0;
    m_cursorCol = 0;
}

void TerminalScreen::clearToEndOfLine()
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) return;
    markDirty(m_cursorRow);
    for (int c = m_cursorCol; c < m_cols; ++c) {
        m_screen[m_cursorRow][c] = Cell();
    }
}

void TerminalScreen::clearToStartOfLine()
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) return;
    markDirty(m_cursorRow);
    for (int c = 0; c <= m_cursorCol; ++c) {
        m_screen[m_cursorRow][c] = Cell();
    }
}

void TerminalScreen::clearLine()
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) return;
    markDirty(m_cursorRow);
    for (int c = 0; c < m_cols; ++c) {
        m_screen[m_cursorRow][c] = Cell();
    }
}

void TerminalScreen::clearToEndOfScreen()
{
    clearToEndOfLine();
    for (int r = m_cursorRow + 1; r < m_rows; ++r) {
        markDirty(r);
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
    }
}

void TerminalScreen::clearToStartOfScreen()
{
    for (int r = 0; r < m_cursorRow; ++r) {
        markDirty(r);
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
    }
    clearToStartOfLine();
}

void TerminalScreen::scrollUp(int lines)
{
    if (lines <= 0) return;
    int top = m_scrollTop;
    int bottom = m_scrollBottom;
    int height = bottom - top + 1;

    lines = qMin(lines, height);

    // 将滚出区域的行加入 scrollback
    for (int i = 0; i < lines; ++i) {
        addLineToScrollback(m_screen[top + i]);
    }

    // 向上移动内容
    for (int r = top; r <= bottom - lines; ++r) {
        m_screen[r] = m_screen[r + lines];
    }

    // 清空底部新露出的行
    for (int r = bottom - lines + 1; r <= bottom; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
    }

    for (int r = top; r <= bottom; ++r) {
        markDirty(r);
    }
}

void TerminalScreen::scrollDown(int lines)
{
    if (lines <= 0) return;
    int top = m_scrollTop;
    int bottom = m_scrollBottom;
    int height = bottom - top + 1;

    lines = qMin(lines, height);

    // 向下移动内容
    for (int r = bottom; r >= top + lines; --r) {
        m_screen[r] = m_screen[r - lines];
    }

    // 清空顶部新露出的行
    for (int r = top; r < top + lines; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
    }

    for (int r = top; r <= bottom; ++r) {
        markDirty(r);
    }
}

void TerminalScreen::setScrollRegion(int top, int bottom)
{
    m_scrollTop = qBound(0, top, m_rows - 1);
    m_scrollBottom = qBound(m_scrollTop, bottom, m_rows - 1);
}

void TerminalScreen::insertChars(int count)
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) return;
    count = qBound(0, count, m_cols - m_cursorCol);
    if (count <= 0) return;

    markDirty(m_cursorRow);
    // 从右向左移动
    for (int c = m_cols - 1; c >= m_cursorCol + count; --c) {
        m_screen[m_cursorRow][c] = m_screen[m_cursorRow][c - count];
    }
    // 清空插入区域
    for (int c = m_cursorCol; c < m_cursorCol + count; ++c) {
        m_screen[m_cursorRow][c] = Cell();
    }
}

void TerminalScreen::deleteChars(int count)
{
    if (m_cursorRow < 0 || m_cursorRow >= m_rows) return;
    count = qBound(0, count, m_cols - m_cursorCol);
    if (count <= 0) return;

    markDirty(m_cursorRow);
    // 向左移动
    for (int c = m_cursorCol; c < m_cols - count; ++c) {
        m_screen[m_cursorRow][c] = m_screen[m_cursorRow][c + count];
    }
    // 清空右侧
    for (int c = m_cols - count; c < m_cols; ++c) {
        m_screen[m_cursorRow][c] = Cell();
    }
}

void TerminalScreen::insertLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) return;
    count = qBound(0, count, m_scrollBottom - m_cursorRow + 1);
    if (count <= 0) return;

    for (int r = m_cursorRow; r <= m_scrollBottom; ++r) {
        markDirty(r);
    }
    // 从底部向上移动
    for (int r = m_scrollBottom; r >= m_cursorRow + count; --r) {
        m_screen[r] = m_screen[r - count];
    }
    // 清空插入区域
    for (int r = m_cursorRow; r < m_cursorRow + count; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
    }
}

void TerminalScreen::deleteLines(int count)
{
    if (m_cursorRow < m_scrollTop || m_cursorRow > m_scrollBottom) return;
    count = qBound(0, count, m_scrollBottom - m_cursorRow + 1);
    if (count <= 0) return;

    for (int r = m_cursorRow; r <= m_scrollBottom; ++r) {
        markDirty(r);
    }
    // 向下移动
    for (int r = m_cursorRow; r <= m_scrollBottom - count; ++r) {
        m_screen[r] = m_screen[r + count];
    }
    // 清空底部
    for (int r = m_scrollBottom - count + 1; r <= m_scrollBottom; ++r) {
        for (int c = 0; c < m_cols; ++c) {
            m_screen[r][c] = Cell();
        }
    }
}

void TerminalScreen::useAlternateScreen(bool alternate)
{
    if (alternate == m_usingAltScreen) return;

    if (alternate) {
        // 切换到交替屏幕：保存当前屏幕到主缓冲区，清空交替屏幕
        m_altScreen = m_screen;
        clearScreen();
        m_usingAltScreen = true;
    } else {
        // 切换回主屏幕
        m_screen = m_altScreen;
        m_usingAltScreen = false;
        for (int r = 0; r < m_rows; ++r) {
            markDirty(r);
        }
    }
}

const Cell& TerminalScreen::cellAt(int row, int col) const
{
    static Cell emptyCell;
    if (row < 0 || row >= m_rows || col < 0 || col >= m_cols) return emptyCell;
    return m_screen[row][col];
}

Cell& TerminalScreen::cellAt(int row, int col)
{
    // 注意：不检查越界，调用方需保证
    return m_screen[row][col];
}

const QVector<Cell>& TerminalScreen::scrollbackLine(int index) const
{
    static QVector<Cell> emptyLine;
    if (index < 0 || index >= m_scrollback.size()) return emptyLine;
    return m_scrollback[index];
}

void TerminalScreen::clearScrollback()
{
    m_scrollback.clear();
}

bool TerminalScreen::isWideChar(QChar c)
{
    ushort u = c.unicode();
    // CJK Unified Ideographs Extension A
    if (u >= 0x3400 && u <= 0x4DBF) return true;
    // CJK Unified Ideographs
    if (u >= 0x4E00 && u <= 0x9FFF) return true;
    // Hangul Syllables
    if (u >= 0xAC00 && u <= 0xD7AF) return true;
    // CJK Compatibility Ideographs
    if (u >= 0xF900 && u <= 0xFAFF) return true;
    // Fullwidth ASCII variants
    if (u >= 0xFF01 && u <= 0xFF5E) return true;
    // Fullwidth brackets and halfwidth Katakana
    if (u >= 0xFF5F && u <= 0xFF60) return true;
    // Halfwidth CJK punctuation / Halfwidth Katakana
    if (u >= 0xFFE0 && u <= 0xFFE6) return true;
    // CJK Symbols and Punctuation
    if (u >= 0x3000 && u <= 0x303F) return true;
    // Hiragana
    if (u >= 0x3040 && u <= 0x309F) return true;
    // Katakana
    if (u >= 0x30A0 && u <= 0x30FF) return true;
    // Hangul Jamo
    if (u >= 0x1100 && u <= 0x11FF) return true;
    // CJK Strokes, Enclosed CJK Letters and Months
    if (u >= 0x3200 && u <= 0x32FF) return true;
    // CJK Compatibility
    if (u >= 0x3300 && u <= 0x33FF) return true;
    // CJK Unified Ideographs Extension B 的一部分（在 BMP 外，QChar 无法直接表示）
    return false;
}

void TerminalScreen::setScrollOffset(int offset)
{
    int maxOffset = qMax(0, m_scrollback.size());
    m_scrollOffset = qBound(0, offset, maxOffset);
}

void TerminalScreen::addLineToScrollback(const QVector<Cell>& line)
{
    m_scrollback.append(line);
    // 限制 scrollback 大小，防止内存无限增长
    const int MAX_SCROLLBACK = 10000;
    if (m_scrollback.size() > MAX_SCROLLBACK) {
        m_scrollback.removeFirst();
    }
}

} // namespace DeepLux
