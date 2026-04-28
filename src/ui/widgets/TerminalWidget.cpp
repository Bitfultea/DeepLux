#include "TerminalWidget.h"

#include "terminal/TerminalScreen.h"
#include "terminal/TerminalRenderer.h"
#include "terminal/AnsiParser.h"
#include "process/BashProcess.h"

#include <QApplication>
#include <QScrollBar>
#include <QTimer>
#include <QTime>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QDebug>
#include <QRegularExpression>
#include <QClipboard>

namespace DeepLux {

TerminalWidget::TerminalWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
}

TerminalWidget::~TerminalWidget() = default;

void TerminalWidget::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 终端屏幕
    m_screen = new TerminalScreen(24, 80);
    m_parser = new AnsiParser(m_screen, this);
    connect(m_parser, &AnsiParser::screenUpdated, this, &TerminalWidget::onScreenUpdated);
    connect(m_parser, &AnsiParser::cliCommandReceived, this, [this](const QString& cmd) {
        emit commandEntered(cmd);
    });

    // 渲染器
    m_renderer = new TerminalRenderer(m_screen, this);
    connect(m_renderer, &TerminalRenderer::sizeChanged, this, &TerminalWidget::onRendererSizeChanged);
    mainLayout->addWidget(m_renderer, 1);

    // 滚动条
    m_scrollBar = new QScrollBar(Qt::Vertical, this);
    m_scrollBar->setRange(0, 0);
    m_scrollBar->setValue(0);
    connect(m_scrollBar, &QScrollBar::valueChanged, this, &TerminalWidget::onScrollBarValueChanged);

    // 水平布局：渲染器 + 滚动条
    QHBoxLayout* hLayout = new QHBoxLayout();
    hLayout->setContentsMargins(0, 0, 0, 0);
    hLayout->setSpacing(0);
    hLayout->addWidget(m_renderer, 1);
    hLayout->addWidget(m_scrollBar);
    mainLayout->addLayout(hLayout, 1);

    setFocusPolicy(Qt::StrongFocus);
    setFocus();
}

void TerminalWidget::setThemeColors(const QColor& fg, const QColor& bg, const QColor& selectionBg)
{
    if (m_renderer) {
        m_renderer->setThemeColors(fg, bg, selectionBg);
    }
}

void TerminalWidget::connectToBashProcess(BashProcess* bashProcess)
{
    if (!bashProcess) return;
    m_bashProcess = bashProcess;

    connect(bashProcess, &BashProcess::outputReady,
            this, &TerminalWidget::onBashOutput,
            Qt::QueuedConnection);

    connect(bashProcess, &BashProcess::errorReady,
            this, &TerminalWidget::onBashError,
            Qt::QueuedConnection);
}

void TerminalWidget::onBashOutput(const QByteArray& data)
{
    if (m_parser) {
        m_parser->parse(data);
    }
}

void TerminalWidget::onBashError(const QByteArray& data)
{
    if (m_parser) {
        // 错误输出也作为 ANSI 文本解析
        m_parser->parse(data);
    }
}

void TerminalWidget::onScreenUpdated()
{
    if (!m_renderer || !m_screen) return;

    // 使用精确重绘：只更新脏行，而不是整个屏幕
    if (m_screen->hasDirtyRows()) {
        int cellH = m_renderer->cellHeight();
        for (int row : m_screen->dirtyRows()) {
            QRect dirtyRect(0, row * cellH, m_renderer->width(), cellH);
            m_renderer->update(dirtyRect);
        }
        m_screen->clearDirtyRows();
    } else {
        m_renderer->update();
    }

    // 光标位置变化也需要重绘（旧位置和新位置）
    // 这里由 TerminalRenderer 的 paintEvent 处理光标绘制

    // 更新滚动条范围（基于 scrollback 大小）
    if (m_scrollBar) {
        int scrollbackSize = m_screen->scrollbackSize();
        m_scrollBar->setRange(0, scrollbackSize);
        // 如果用户没有手动滚动，自动滚到最底部
        if (m_screen->scrollOffset() == 0 || m_screen->scrollOffset() == scrollbackSize) {
            m_scrollBar->setValue(scrollbackSize);
            m_screen->setScrollOffset(0);
            m_renderer->setScrollOffset(0);
        }
    }
}

void TerminalWidget::onRendererSizeChanged(int cols, int rows)
{
    if (m_bashProcess) {
        m_bashProcess->resize(cols, rows);
    }
}

void TerminalWidget::onScrollBarValueChanged(int value)
{
    if (m_screen) {
        m_screen->setScrollOffset(value);
    }
    if (m_renderer) {
        m_renderer->setScrollOffset(value);
    }
}

void TerminalWidget::printCommand(const QString& cmd)
{
    if (!m_screen) return;
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    m_parser->writePlainText(QString("[%1] > %2\n").arg(timestamp, cmd));
}

void TerminalWidget::printOutput(const QString& text)
{
    if (!m_screen) return;
    m_parser->writePlainText(text + "\n");
}

void TerminalWidget::printError(const QString& text)
{
    if (!m_screen || !m_parser) return;
    // 使用红色输出
    m_screen->currentAttrs().fgType = ColorType::Basic;
    m_screen->currentAttrs().fgValue = 1; // red
    m_parser->writePlainText(text + "\n");
    m_screen->currentAttrs().reset();
}

void TerminalWidget::printInfo(const QString& text)
{
    if (!m_screen || !m_parser) return;
    m_screen->currentAttrs().fgType = ColorType::Basic;
    m_screen->currentAttrs().fgValue = 4; // blue
    m_parser->writePlainText(text + "\n");
    m_screen->currentAttrs().reset();
}

void TerminalWidget::printSuccess(const QString& text)
{
    if (!m_screen || !m_parser) return;
    m_screen->currentAttrs().fgType = ColorType::Basic;
    m_screen->currentAttrs().fgValue = 2; // green
    m_parser->writePlainText(text + "\n");
    m_screen->currentAttrs().reset();
}

void TerminalWidget::printWarning(const QString& text)
{
    if (!m_screen || !m_parser) return;
    m_screen->currentAttrs().fgType = ColorType::Basic;
    m_screen->currentAttrs().fgValue = 3; // yellow
    m_parser->writePlainText(text + "\n");
    m_screen->currentAttrs().reset();
}

void TerminalWidget::printRaw(const QString& text)
{
    if (m_parser) {
        m_parser->writePlainText(text);
    }
}

void TerminalWidget::clear()
{
    if (m_screen) {
        m_screen->clearScreen();
    }
    if (m_renderer) {
        m_renderer->update();
    }
}

void TerminalWidget::setAvailableCommands(const QStringList& commands)
{
    m_availableCommands = commands;
}

void TerminalWidget::onLogMessage(const QString& message, int level)
{
    switch (level) {
    case 0: printOutput(message); break;
    case 1: printInfo(message); break;
    case 2: printWarning(message); break;
    case 3: printError(message); break;
    case 4: printSuccess(message); break;
    default: printOutput(message); break;
    }
}

void TerminalWidget::onCommandExecuted(const QString& command, int exitCode)
{
    if (exitCode == 0) {
        printSuccess(QString("命令 '%1' 执行成功").arg(command));
    } else {
        printError(QString("命令 '%1' 执行失败 (退出码: %2)").arg(command).arg(exitCode));
    }
}

void TerminalWidget::onModuleStarted(const QString& moduleName)
{
    printInfo(QString("模块 '%1' 开始执行").arg(moduleName));
}

void TerminalWidget::onModuleFinished(const QString& moduleName, bool success)
{
    if (success) {
        printSuccess(QString("模块 '%1' 执行完成").arg(moduleName));
    } else {
        printError(QString("模块 '%1' 执行失败").arg(moduleName));
    }
}

void TerminalWidget::keyPressEvent(QKeyEvent* event)
{
    // 复制/粘贴快捷键（兼容 VSCode 终端行为）
    if (event->modifiers() & Qt::ControlModifier) {
        if (event->key() == Qt::Key_C) {
            if (event->modifiers() & Qt::ShiftModifier) {
                // Ctrl+Shift+C: 复制选中
                if (m_renderer && m_renderer->hasSelection()) {
                    QApplication::clipboard()->setText(m_renderer->selectedText());
                }
                return;
            }
            // Ctrl+C: 有选中则复制，否则发送 SIGINT
            if (m_renderer && m_renderer->hasSelection()) {
                QApplication::clipboard()->setText(m_renderer->selectedText());
                m_renderer->clearSelection();
                return;
            }
            // 无选中：走 sendKeyToPty 发送 \x03
        }
        if (event->key() == Qt::Key_V) {
            if (event->modifiers() & Qt::ShiftModifier) {
                // Ctrl+Shift+V: 粘贴
                QString text = QApplication::clipboard()->text();
                if (!text.isEmpty() && m_bashProcess) {
                    m_bashProcess->writeRaw(text);
                }
                return;
            }
            // Ctrl+V: 粘贴（VSCode 终端行为）
            QString text = QApplication::clipboard()->text();
            if (!text.isEmpty() && m_bashProcess) {
                m_bashProcess->writeRaw(text);
            }
            return;
        }
    }

    // 所有键直接发送到 PTY，让 bash/readline/vim 自行处理
    sendKeyToPty(event);
    event->accept();
}

bool TerminalWidget::focusNextPrevChild(bool next)
{
    Q_UNUSED(next)
    // 阻止 Qt 用 Tab/Shift+Tab 移动焦点，让 Tab 键发送到 PTY
    return false;
}

void TerminalWidget::sendKeyToPty(QKeyEvent* event)
{
    if (!m_bashProcess) return;

    Qt::KeyboardModifiers mods = event->modifiers();
    bool ctrl = mods & Qt::ControlModifier;
    bool shift = mods & Qt::ShiftModifier;
    bool alt = mods & Qt::AltModifier;

    // 处理特殊键
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        m_bashProcess->writeRaw("\r");
        return;
    case Qt::Key_Backspace:
        m_bashProcess->writeRaw("\x7f");
        return;
    case Qt::Key_Tab:
        m_bashProcess->writeRaw(shift ? "\x1b[Z" : "\t");
        return;
    case Qt::Key_Escape:
        m_bashProcess->writeRaw("\x1b");
        return;
    case Qt::Key_Left:
        m_bashProcess->writeRaw(alt ? "\x1b\x1b[D" : "\x1b[D");
        return;
    case Qt::Key_Right:
        m_bashProcess->writeRaw(alt ? "\x1b\x1b[C" : "\x1b[C");
        return;
    case Qt::Key_Up:
        m_bashProcess->writeRaw(alt ? "\x1b\x1b[A" : "\x1b[A");
        return;
    case Qt::Key_Down:
        m_bashProcess->writeRaw(alt ? "\x1b\x1b[B" : "\x1b[B");
        return;
    case Qt::Key_Home:
        m_bashProcess->writeRaw(ctrl ? "\x1b[1;5H" : "\x1b[H");
        return;
    case Qt::Key_End:
        m_bashProcess->writeRaw(ctrl ? "\x1b[1;5F" : "\x1b[F");
        return;
    case Qt::Key_PageUp:
        m_bashProcess->writeRaw("\x1b[5~");
        return;
    case Qt::Key_PageDown:
        m_bashProcess->writeRaw("\x1b[6~");
        return;
    case Qt::Key_Delete:
        m_bashProcess->writeRaw("\x1b[3~");
        return;
    case Qt::Key_Insert:
        m_bashProcess->writeRaw("\x1b[2~");
        return;
    case Qt::Key_F1:
        m_bashProcess->writeRaw("\x1bOP");
        return;
    case Qt::Key_F2:
        m_bashProcess->writeRaw("\x1bOQ");
        return;
    case Qt::Key_F3:
        m_bashProcess->writeRaw("\x1bOR");
        return;
    case Qt::Key_F4:
        m_bashProcess->writeRaw("\x1bOS");
        return;
    }

    // 处理 Ctrl+字母
    if (ctrl && !alt && !shift) {
        int key = event->key();
        if (key >= Qt::Key_A && key <= Qt::Key_Z) {
            char c = static_cast<char>(key - Qt::Key_A + 1);
            m_bashProcess->writeRaw(QString(QChar(c)));
            return;
        }
        if (key == Qt::Key_Space) {
            m_bashProcess->writeRaw("\x00");
            return;
        }
        if (key == Qt::Key_6) {
            m_bashProcess->writeRaw("\x1e");  // Ctrl+^
            return;
        }
        if (key == Qt::Key_Minus || key == Qt::Key_Underscore) {
            m_bashProcess->writeRaw("\x1f");  // Ctrl+_
            return;
        }
        if (key == Qt::Key_BracketLeft) {
            m_bashProcess->writeRaw("\x1b");  // Ctrl+[
            return;
        }
        if (key == Qt::Key_Backslash) {
            m_bashProcess->writeRaw("\x1c");  // Ctrl+\
            return;
        }
        if (key == Qt::Key_BracketRight) {
            m_bashProcess->writeRaw("\x1d");  // Ctrl+]
            return;
        }
    }

    // 普通字符输入
    QString text = event->text();
    if (!text.isEmpty()) {
        m_bashProcess->writeRaw(text);
    }
}

void TerminalWidget::sendTextToPty(const QString& text)
{
    if (m_bashProcess) {
        m_bashProcess->writeRaw(text);
    }
}

} // namespace DeepLux
