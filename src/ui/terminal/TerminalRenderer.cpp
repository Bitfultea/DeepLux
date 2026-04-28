#include "TerminalRenderer.h"

#include <QPaintEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QClipboard>
#include <QDebug>

namespace DeepLux {

TerminalRenderer::TerminalRenderer(TerminalScreen* screen, QWidget* parent)
    : QWidget(parent)
    , m_screen(screen)
{
    setAttribute(Qt::WA_OpaquePaintEvent);

    m_font = QFont("DejaVu Sans Mono", 11);
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    updateCellSize();

    setCursor(Qt::IBeamCursor);
}

void TerminalRenderer::setScreen(TerminalScreen* screen)
{
    m_screen = screen;
    update();
}

void TerminalRenderer::setThemeColors(const QColor& fg, const QColor& bg, const QColor& selectionBg)
{
    m_defaultFg = fg;
    m_defaultBg = bg;
    m_selectionBg = selectionBg;
    update();
}

void TerminalRenderer::updateCellSize()
{
    QFontMetrics fm(m_font);
    m_cellWidth = fm.horizontalAdvance('W');
    m_cellHeight = fm.height();
    m_baseline = fm.ascent();
}

QPoint TerminalRenderer::pixelToCell(const QPoint& pos) const
{
    int col = pos.x() / m_cellWidth;
    int row = pos.y() / m_cellHeight;
    if (m_screen) {
        col = qBound(0, col, m_screen->cols() - 1);
        row = qBound(0, row, m_screen->rows() - 1);
    }
    return QPoint(col, row);
}

QPoint TerminalRenderer::cellToPixel(int row, int col) const
{
    return QPoint(col * m_cellWidth, row * m_cellHeight);
}

QString TerminalRenderer::selectedText() const
{
    if (!m_selection.active || !m_screen) return QString();

    QString result;
    int startRow = qMin(m_selection.startRow, m_selection.endRow);
    int endRow = qMax(m_selection.startRow, m_selection.endRow);
    int startCol = (startRow == m_selection.startRow) ? m_selection.startCol : m_selection.endCol;
    int endCol = (endRow == m_selection.endRow) ? m_selection.endCol : m_selection.startCol;

    if (m_selection.startRow > m_selection.endRow) {
        qSwap(startCol, endCol);
    }

    for (int r = startRow; r <= endRow; ++r) {
        int sc = (r == startRow) ? startCol : 0;
        int ec = (r == endRow) ? endCol : m_screen->cols() - 1;

        for (int c = sc; c <= ec; ++c) {
            result.append(m_screen->cellAt(r, c).character);
        }
        if (r < endRow) {
            result.append('\n');
        }
    }

    return result;
}

void TerminalRenderer::clearSelection()
{
    m_selection.active = false;
    m_selection.selecting = false;
    update();
}

void TerminalRenderer::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event)
    if (!m_screen) return;

    QPainter p(this);
    p.setFont(m_font);

    int rows = m_screen->rows();
    int cols = m_screen->cols();

    // 绘制背景
    p.fillRect(rect(), m_defaultBg);

    // 绘制选中区域背景
    if (m_selection.active) {
        drawSelection(p);
    }

    // 绘制每个单元格
    int scrollbackSize = m_screen->scrollbackSize();
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            Cell cell;
            if (m_scrollOffset > 0 && scrollbackSize > 0) {
                // 从 scrollback 中取行
                int sbIndex = scrollbackSize - m_scrollOffset + r;
                if (sbIndex >= 0 && sbIndex < scrollbackSize) {
                    const QVector<Cell>& line = m_screen->scrollbackLine(sbIndex);
                    if (c < line.size()) {
                        cell = line[c];
                    }
                } else if (sbIndex >= scrollbackSize) {
                    // 超出 scrollback 的部分显示当前 screen
                    int screenRow = sbIndex - scrollbackSize;
                    if (screenRow >= 0 && screenRow < rows) {
                        cell = m_screen->cellAt(screenRow, c);
                    }
                }
            } else {
                cell = m_screen->cellAt(r, c);
            }
            drawCell(p, r, c, cell);
        }
    }

    // 绘制光标（仅在未滚动时）
    if (m_scrollOffset == 0 && m_screen->cursorVisible() && hasFocus()) {
        drawCursor(p, m_screen->cursorRow(), m_screen->cursorCol());
    }
}

void TerminalRenderer::drawCell(QPainter& p, int row, int col, const Cell& cell)
{
    int x = col * m_cellWidth;
    int y = row * m_cellHeight;

    // 如果是宽字符的右半部分，跳过（由左半部分绘制）
    if (cell.isWide && cell.character == QChar(' ')) {
        if (col > 0) {
            const Cell& leftCell = m_screen->cellAt(row, col - 1);
            if (leftCell.isWide && leftCell.character != QChar(' ')) {
                return;
            }
        }
    }

    // 确定绘制宽度（宽字符占 2 列）
    int drawWidth = m_cellWidth;
    if (cell.isWide && cell.character != QChar(' ')) {
        drawWidth = qMin(m_cellWidth * 2, (m_screen->cols() - col) * m_cellWidth);
    }

    // 计算是否在选中区域内
    bool inSelection = false;
    if (m_selection.active) {
        int sr = qMin(m_selection.startRow, m_selection.endRow);
        int er = qMax(m_selection.startRow, m_selection.endRow);
        int sc = (m_selection.startRow <= m_selection.endRow) ? m_selection.startCol : m_selection.endCol;
        int ec = (m_selection.startRow <= m_selection.endRow) ? m_selection.endCol : m_selection.startCol;

        if (m_selection.startRow > m_selection.endRow) {
            qSwap(sc, ec);
        }

        if (row > sr && row < er) {
            inSelection = true;
        } else if (row == sr && row == er) {
            inSelection = (col >= sc && col <= ec);
        } else if (row == sr) {
            inSelection = (col >= sc);
        } else if (row == er) {
            inSelection = (col <= ec);
        }
    }

    // 确定颜色
    QColor bg = resolveBackground(cell.attrs);
    QColor fg = resolveForeground(cell.attrs);

    if (inSelection) {
        bg = m_selectionBg;
    }

    // 绘制背景（仅在非默认背景时绘制，paintEvent 开头已绘制默认背景）
    if (bg != m_defaultBg) {
        p.fillRect(x, y, drawWidth, m_cellHeight, bg);
    }

    // 如果宽字符主单元格在选中区域内，也绘制 spacer 的选中背景
    if (inSelection && cell.isWide && cell.character != QChar(' ') && col + 1 < m_screen->cols()) {
        const Cell& rightCell = m_screen->cellAt(row, col + 1);
        if (rightCell.isWide && rightCell.character == QChar(' ')) {
            p.fillRect(x + m_cellWidth, y, m_cellWidth, m_cellHeight, m_selectionBg);
        }
    }

    // 绘制字符（使用 clip 防止抗锯齿溢出到相邻单元格）
    if (cell.character != QChar(' ') && cell.character != QChar(0)) {
        p.save();
        p.setClipRect(x, y, drawWidth, m_cellHeight);
        p.setPen(fg);
        p.drawText(x, y, drawWidth, m_cellHeight, Qt::AlignLeft | Qt::AlignVCenter, QString(cell.character));
        p.restore();
    }
}

void TerminalRenderer::drawCursor(QPainter& p, int row, int col)
{
    if (row < 0 || col < 0) return;
    int x = col * m_cellWidth;
    int y = row * m_cellHeight;

    QColor cursorColor = QColor("#ffffff");
    QColor fg = resolveForeground(m_screen->cellAt(row, col).attrs);

    switch (m_cursorStyle) {
    case CursorStyle::Block:
        p.fillRect(x, y, m_cellWidth, m_cellHeight, cursorColor);
        // 反色绘制字符
        if (m_screen->cellAt(row, col).character != QChar(' ')) {
            p.setPen(m_defaultBg);
            p.drawText(x, y + m_baseline, QString(m_screen->cellAt(row, col).character));
        }
        break;
    case CursorStyle::Underline:
        p.setPen(QPen(cursorColor, 2));
        p.drawLine(x, y + m_cellHeight - 1, x + m_cellWidth, y + m_cellHeight - 1);
        break;
    case CursorStyle::Bar:
        p.fillRect(x, y, 2, m_cellHeight, cursorColor);
        break;
    }
}

void TerminalRenderer::drawSelection(QPainter& p)
{
    // 选中背景在 drawCell 中逐个处理，这里不需要额外绘制
    Q_UNUSED(p)
}

QColor TerminalRenderer::resolveForeground(const CellAttributes& attrs) const
{
    if (attrs.reverse) {
        return resolveBackground(attrs);
    }

    if (attrs.fgType == ColorType::Basic) {
        return AnsiConstants::basicPalette().value(attrs.fgValue, m_defaultFg);
    } else if (attrs.fgType == ColorType::Palette256) {
        if (attrs.fgValue >= 0 && attrs.fgValue < 256) {
            return AnsiConstants::palette256().at(attrs.fgValue);
        }
    } else if (attrs.fgType == ColorType::TrueColor) {
        return QColor(attrs.fgValue);
    }
    return m_defaultFg;
}

QColor TerminalRenderer::resolveBackground(const CellAttributes& attrs) const
{
    if (attrs.reverse) {
        // 反色：交换前景和背景
        CellAttributes rev = attrs;
        rev.reverse = false;
        QColor fg = resolveForeground(rev);
        // 返回原前景色作为背景
        if (attrs.fgType == ColorType::Basic) {
            return AnsiConstants::basicPalette().value(attrs.fgValue, m_defaultBg);
        } else if (attrs.fgType == ColorType::Palette256) {
            if (attrs.fgValue >= 0 && attrs.fgValue < 256) {
                return AnsiConstants::palette256().at(attrs.fgValue);
            }
        } else if (attrs.fgType == ColorType::TrueColor) {
            return QColor(attrs.fgValue);
        }
        return m_defaultBg;
    }

    if (attrs.bgType == ColorType::Basic) {
        return AnsiConstants::basicPalette().value(attrs.bgValue, m_defaultBg);
    } else if (attrs.bgType == ColorType::Palette256) {
        if (attrs.bgValue >= 0 && attrs.bgValue < 256) {
            return AnsiConstants::palette256().at(attrs.bgValue);
        }
    } else if (attrs.bgType == ColorType::TrueColor) {
        return QColor(attrs.bgValue);
    }
    return m_defaultBg;
}

void TerminalRenderer::resizeEvent(QResizeEvent* event)
{
    Q_UNUSED(event)
    if (!m_screen) return;

    int newCols = width() / m_cellWidth;
    int newRows = height() / m_cellHeight;

    newCols = qMax(1, newCols);
    newRows = qMax(1, newRows);

    if (newCols != m_screen->cols() || newRows != m_screen->rows()) {
        m_screen->resize(newRows, newCols);
        emit sizeChanged(newCols, newRows);
    }
}

void TerminalRenderer::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        clearSelection();
        QPoint cell = pixelToCell(event->pos());
        m_selection.startRow = cell.y();
        m_selection.startCol = cell.x();
        m_selection.endRow = cell.y();
        m_selection.endCol = cell.x();
        m_selection.selecting = true;
        m_selection.active = true;
        if (parentWidget()) parentWidget()->setFocus();
        update();
    }
}

void TerminalRenderer::mouseMoveEvent(QMouseEvent* event)
{
    if (m_selection.selecting && (event->buttons() & Qt::LeftButton)) {
        QPoint cell = pixelToCell(event->pos());
        m_selection.endRow = cell.y();
        m_selection.endCol = cell.x();
        update();
    }
}

void TerminalRenderer::mouseReleaseEvent(QMouseEvent* event)
{
    Q_UNUSED(event)
    if (m_selection.selecting) {
        m_selection.selecting = false;
        emit selectionChanged();
        update();
    }
}

void TerminalRenderer::mouseDoubleClickEvent(QMouseEvent* event)
{
    // 双击选中单词
    QPoint cell = pixelToCell(event->pos());
    int row = cell.y();
    int col = cell.x();

    if (!m_screen || row >= m_screen->rows() || col >= m_screen->cols()) return;

    // 找到单词边界
    int startCol = col;
    int endCol = col;

    while (startCol > 0 && m_screen->cellAt(row, startCol - 1).character.isLetterOrNumber()) {
        startCol--;
    }
    while (endCol < m_screen->cols() - 1 && m_screen->cellAt(row, endCol + 1).character.isLetterOrNumber()) {
        endCol++;
    }

    m_selection.startRow = row;
    m_selection.startCol = startCol;
    m_selection.endRow = row;
    m_selection.endCol = endCol;
    m_selection.active = true;
    m_selection.selecting = false;
    emit selectionChanged();
    update();
}

} // namespace DeepLux
