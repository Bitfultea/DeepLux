#ifndef DEEPLUX_TERMINAL_RENDERER_H
#define DEEPLUX_TERMINAL_RENDERER_H

#include "TerminalScreen.h"

#include <QWidget>
#include <QFont>
#include <QFontMetrics>
#include <QPainter>

namespace DeepLux {

/**
 * @brief 终端渲染器 - 使用 QPainter 绘制字符网格
 *
 * 职责：
 * - 计算等宽字体单元格大小
 * - 绘制每个单元格的背景和字符
 * - 绘制光标（方块/下划线/竖线）
 * - 处理鼠标选中
 * - 窗口大小变化时计算新的行列数
 */
class TerminalRenderer : public QWidget
{
    Q_OBJECT

public:
    explicit TerminalRenderer(TerminalScreen* screen, QWidget* parent = nullptr);

    void setScreen(TerminalScreen* screen);
    void setThemeColors(const QColor& fg, const QColor& bg, const QColor& selectionBg);

    int cellWidth() const { return m_cellWidth; }
    int cellHeight() const { return m_cellHeight; }

    int scrollOffset() const { return m_scrollOffset; }
    void setScrollOffset(int offset) { m_scrollOffset = offset; update(); }

    // 像素坐标 -> 单元格坐标
    QPoint pixelToCell(const QPoint& pos) const;
    QPoint cellToPixel(int row, int col) const;

    // 选中区域
    bool hasSelection() const { return m_selection.active; }
    QString selectedText() const;
    void clearSelection();

signals:
    void sizeChanged(int cols, int rows);
    void selectionChanged();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    void updateCellSize();
    void drawCell(QPainter& p, int row, int col, const Cell& cell);
    void drawCursor(QPainter& p, int row, int col);
    void drawSelection(QPainter& p);

    QColor resolveForeground(const CellAttributes& attrs) const;
    QColor resolveBackground(const CellAttributes& attrs) const;

    TerminalScreen* m_screen = nullptr;

    QFont m_font;
    int m_cellWidth = 8;
    int m_cellHeight = 16;
    int m_baseline = 13;

    // 默认颜色
    QColor m_defaultFg = QColor("#d4d4d4");
    QColor m_defaultBg = QColor("#000000");
    QColor m_selectionBg = QColor("#264f78");

    // 光标样式
    enum class CursorStyle { Block, Underline, Bar };
    CursorStyle m_cursorStyle = CursorStyle::Block;

    // 选中区域（单元格坐标）
    struct Selection {
        int startRow = 0, startCol = 0;
        int endRow = 0, endCol = 0;
        bool active = false;
        bool selecting = false;
    } m_selection;

    int m_scrollOffset = 0;
    bool m_focused = false;
};

} // namespace DeepLux

#endif // DEEPLUX_TERMINAL_RENDERER_H
