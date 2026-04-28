#ifndef DEEPLUX_TERMINAL_SCREEN_H
#define DEEPLUX_TERMINAL_SCREEN_H

#include "AnsiConstants.h"

#include <QVector>
#include <QChar>
#include <QString>
#include <QSet>

namespace DeepLux {

/**
 * @brief 终端单元格
 */
struct Cell {
    QChar character = QChar(' ');
    CellAttributes attrs;
    bool isWide = false;
};

/**
 * @brief 终端屏幕缓冲区管理
 *
 * 维护二维字符网格，支持：
 * - 主屏幕缓冲区（当前可见）
 * - 交替屏幕缓冲区（vim/htop 等全屏应用）
 * - 滚动历史缓冲区
 * - 光标控制、清屏、滚动区域
 */
class TerminalScreen
{
public:
    explicit TerminalScreen(int rows = 24, int cols = 80);

    // 尺寸
    int rows() const { return m_rows; }
    int cols() const { return m_cols; }
    void resize(int rows, int cols);

    // 写入字符（光标处写入，自动推进光标）
    void putChar(QChar c);
    void putString(const QString& s);

    // 光标控制
    void setCursor(int row, int col);
    void moveCursor(int dr, int dc);
    void saveCursor();
    void restoreCursor();
    int cursorRow() const { return m_cursorRow; }
    int cursorCol() const { return m_cursorCol; }

    // 清除
    void clearScreen();
    void clearToEndOfLine();
    void clearToStartOfLine();
    void clearLine();
    void clearToEndOfScreen();
    void clearToStartOfScreen();

    // 滚动
    void scrollUp(int lines = 1);
    void scrollDown(int lines = 1);
    void setScrollRegion(int top, int bottom);
    int scrollTop() const { return m_scrollTop; }
    int scrollBottom() const { return m_scrollBottom; }

    // 插入/删除
    void insertChars(int count);
    void deleteChars(int count);
    void insertLines(int count);
    void deleteLines(int count);

    // 屏幕缓冲区切换
    void useAlternateScreen(bool alternate);
    bool isAlternateScreen() const { return m_usingAltScreen; }

    // 单元格访问
    const Cell& cellAt(int row, int col) const;
    Cell& cellAt(int row, int col);
    const QVector<QVector<Cell>>& screen() const { return m_screen; }

    // 滚动历史
    int scrollbackSize() const { return m_scrollback.size(); }
    const QVector<Cell>& scrollbackLine(int index) const;
    void clearScrollback();

    // 当前 SGR 属性
    CellAttributes& currentAttrs() { return m_currentAttrs; }
    const CellAttributes& currentAttrs() const { return m_currentAttrs; }

    // 光标可见性
    bool cursorVisible() const { return m_cursorVisible; }
    void setCursorVisible(bool visible) { m_cursorVisible = visible; }

    // 滚动偏移（用于查看 scrollback 历史）
    int scrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(int offset);

    // 宽字符检测（East Asian Width）
    static bool isWideChar(QChar c);

    // 脏行跟踪（用于优化重绘）
    const QSet<int>& dirtyRows() const { return m_dirtyRows; }
    void clearDirtyRows() { m_dirtyRows.clear(); }
    bool hasDirtyRows() const { return !m_dirtyRows.isEmpty(); }
    void markDirty(int row) { if (row >= 0 && row < m_rows) m_dirtyRows.insert(row); }

private:
    void initScreen(QVector<QVector<Cell>>& screen, int rows, int cols);
    void addLineToScrollback(const QVector<Cell>& line);
    void ensureScreenSize();

    QVector<QVector<Cell>> m_screen;       // 当前可见屏幕
    QVector<QVector<Cell>> m_scrollback;   // 滚动历史
    QVector<QVector<Cell>> m_altScreen;    // 交替屏幕缓冲区
    bool m_usingAltScreen = false;

    int m_rows = 0;
    int m_cols = 0;
    int m_cursorRow = 0;
    int m_cursorCol = 0;
    int m_savedRow = 0;
    int m_savedCol = 0;

    CellAttributes m_currentAttrs;

    int m_scrollTop = 0;
    int m_scrollBottom = 0;
    int m_scrollOffset = 0;

    bool m_cursorVisible = true;

    QSet<int> m_dirtyRows;
};

} // namespace DeepLux

#endif // DEEPLUX_TERMINAL_SCREEN_H
