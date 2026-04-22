#include "TerminalWidget.h"

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

namespace DeepLux {

TerminalWidget::TerminalWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUI();
    setupStyle();
}

TerminalWidget::~TerminalWidget() = default;

void TerminalWidget::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(0);

    // 输出区域 - 使用 QTextEdit 以支持富文本
    m_outputArea = new QTextEdit(this);
    m_outputArea->setObjectName("TerminalOutputArea");
    m_outputArea->setReadOnly(true);
    m_outputArea->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    m_outputArea->setBackgroundRole(QPalette::Window);
    mainLayout->addWidget(m_outputArea);

    // 输入区域
    QWidget* inputWidget = new QWidget(this);
    inputWidget->setObjectName("TerminalInputWidget");
    QHBoxLayout* inputLayout = new QHBoxLayout(inputWidget);
    inputLayout->setContentsMargins(0, 3, 0, 0);
    inputLayout->setSpacing(5);

    m_promptLabel = new QLabel(">", inputWidget);
    m_promptLabel->setObjectName("TerminalPrompt");
    m_promptLabel->setAlignment(Qt::AlignCenter);
    m_promptLabel->setFixedWidth(20);

    m_inputLine = new QLineEdit(inputWidget);
    m_inputLine->setObjectName("TerminalInputLine");
    m_inputLine->setPlaceholderText(tr("输入命令... (help 获取帮助)"));
    m_inputLine->setFocus();
    connect(m_inputLine, &QLineEdit::returnPressed, this, &TerminalWidget::onReturnPressed);

    inputLayout->addWidget(m_promptLabel);
    inputLayout->addWidget(m_inputLine);

    mainLayout->addWidget(inputWidget);

    // 连接选中文本信号
    connect(m_outputArea, &QTextEdit::selectionChanged, this, &TerminalWidget::onTextSelected);

    // 安装事件过滤器
    m_inputLine->installEventFilter(this);
}

void TerminalWidget::setupStyle()
{
    // 深色主题样式
    QString styleSheet = R"(
        #TerminalOutputArea {
            background-color: #1e1e1e;
            color: #d4d4d4;
            border: 1px solid #3c3c3c;
            border-radius: 4px;
            padding: 5px;
            font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
            font-size: 12px;
        }
        #TerminalInputWidget {
            background-color: transparent;
        }
        #TerminalPrompt {
            color: #27ae60;
            font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
            font-size: 12px;
            font-weight: bold;
        }
        #TerminalInputLine {
            background-color: #2d2d2d;
            color: #ffffff;
            border: 1px solid #3c3c3c;
            border-radius: 4px;
            padding: 5px 10px;
            font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
            font-size: 12px;
        }
        #TerminalInputLine:focus {
            border: 1px solid #27ae60;
        }
        #TerminalInputLine:disabled {
            background-color: #1e1e1e;
            color: #808080;
        }
    )";
    setStyleSheet(styleSheet);
}

void TerminalWidget::connectToBashProcess(BashProcess* bashProcess)
{
    if (!bashProcess) return;

    // 连接 BashProcess 信号到 TerminalWidget 槽
    connect(bashProcess, &BashProcess::outputReady,
            this, &TerminalWidget::onBashOutput,
            Qt::QueuedConnection);

    connect(bashProcess, &BashProcess::errorReady,
            this, &TerminalWidget::onBashError,
            Qt::QueuedConnection);

    // 当用户输入命令时，通过 emit commandEntered 信号
    // TerminalBridge 会将命令写入 BashProcess
}

void TerminalWidget::printCommand(const QString& cmd)
{
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    QString coloredCmd = QString("<span style='color: #27ae60;'>></span> <span style='color: #ffffff;'>%1</span>")
                             .arg(escapeHtml(cmd));

    QString line = QString("[<span style='color: #808080;'>%1</span>] %2")
                       .arg(timestamp)
                       .arg(coloredCmd);

    m_outputArea->append(line);
    scrollToBottom();
}

void TerminalWidget::printOutput(const QString& text)
{
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    QString line = QString("[<span style='color: #808080;'>%1</span>] <span style='color: #d4d4d4;'>%2</span>")
                       .arg(timestamp)
                       .arg(ansiToHtml(text));
    m_outputArea->append(line);
    scrollToBottom();
}

void TerminalWidget::printError(const QString& text)
{
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    QString line = QString("[<span style='color: #808080;'>%1</span>] <span style='color: #e94560;'>%2</span>")
                       .arg(timestamp)
                       .arg(escapeHtml(text));
    m_outputArea->append(line);
    scrollToBottom();
}

void TerminalWidget::printInfo(const QString& text)
{
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    QString line = QString("[<span style='color: #808080;'>%1</span>] <span style='color: #3498db;'>%2</span>")
                       .arg(timestamp)
                       .arg(escapeHtml(text));
    m_outputArea->append(line);
    scrollToBottom();
}

void TerminalWidget::printSuccess(const QString& text)
{
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    QString line = QString("[<span style='color: #808080;'>%1</span>] <span style='color: #27ae60;'>%2</span>")
                       .arg(timestamp)
                       .arg(escapeHtml(text));
    m_outputArea->append(line);
    scrollToBottom();
}

void TerminalWidget::printWarning(const QString& text)
{
    QString timestamp = QTime::currentTime().toString("hh:mm:ss");
    QString line = QString("[<span style='color: #808080;'>%1</span>] <span style='color: #f39c12;'>%2</span>")
                       .arg(timestamp)
                       .arg(escapeHtml(text));
    m_outputArea->append(line);
    scrollToBottom();
}

void TerminalWidget::printRaw(const QString& text)
{
    m_outputArea->append(ansiToHtml(text));
    scrollToBottom();
}

void TerminalWidget::clear()
{
    m_outputArea->clear();
}

void TerminalWidget::setCommandHistory(const QStringList& history)
{
    m_commandHistory = history;
    m_historyIndex = -1;
}

void TerminalWidget::setAvailableCommands(const QStringList& commands)
{
    m_availableCommands = commands;

    if (m_completerModel) {
        delete m_completerModel;
    }

    m_completerModel = new QStringListModel(commands, this);
    m_completer = new QCompleter(commands, this);
    m_completer->setModel(m_completerModel);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_inputLine->setCompleter(m_completer);
}

void TerminalWidget::onReturnPressed()
{
    QString cmd = m_inputLine->text().trimmed();
    if (!cmd.isEmpty()) {
        addToHistory(cmd);
        printCommand(cmd);
        emit commandEntered(cmd);
    }
    m_inputLine->clear();
    m_historyIndex = -1;
}

void TerminalWidget::onHistoryUp()
{
    if (m_commandHistory.isEmpty()) return;

    if (m_historyIndex < 0) {
        m_currentInput = m_inputLine->text();
        m_historyIndex = m_commandHistory.size() - 1;
    } else if (m_historyIndex > 0) {
        m_historyIndex--;
    }

    m_inputLine->setText(m_commandHistory[m_historyIndex]);
}

void TerminalWidget::onHistoryDown()
{
    if (m_historyIndex < 0) return;

    if (m_historyIndex < m_commandHistory.size() - 1) {
        m_historyIndex++;
        m_inputLine->setText(m_commandHistory[m_historyIndex]);
    } else {
        m_historyIndex = -1;
        m_inputLine->setText(m_currentInput);
    }
}

void TerminalWidget::onTextSelected()
{
    // 当用户选中文本时，可以复制
}

void TerminalWidget::onBashOutput(const QString& text)
{
    printRaw(text);
}

void TerminalWidget::onBashError(const QString& text)
{
    printError(text);
}

void TerminalWidget::onLogMessage(const QString& message, int level)
{
    switch (level) {
    case 0: printOutput(message); break;   // Debug
    case 1: printInfo(message); break;     // Info
    case 2: printWarning(message); break;  // Warning
    case 3: printError(message); break;    // Error
    case 4: printSuccess(message); break;  // Success
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

void TerminalWidget::addToHistory(const QString& command)
{
    // 避免重复
    if (!m_commandHistory.isEmpty() && m_commandHistory.last() == command) {
        return;
    }
    m_commandHistory.append(command);

    // 限制历史长度
    if (m_commandHistory.size() > 100) {
        m_commandHistory.removeFirst();
    }
}

QString TerminalWidget::escapeHtml(const QString& text) const
{
    QString result = text;
    result.replace("&", "&amp;");
    result.replace("<", "&lt;");
    result.replace(">", "&gt;");
    result.replace("\"", "&quot;");
    result.replace("'", "&#39;");
    return result;
}

QString TerminalWidget::ansiToHtml(const QString& text) const
{
    QString result = escapeHtml(text);

    // 简单的 ANSI 颜色替换
    result = result.replace("\x1b[31m", "<span style='color: #e94560;'>");
    result = result.replace("\x1b[32m", "<span style='color: #27ae60;'>");
    result = result.replace("\x1b[33m", "<span style='color: #f39c12;'>");
    result = result.replace("\x1b[34m", "<span style='color: #3498db;'>");
    result = result.replace("\x1b[35m", "<span style='color: #9b59b6;'>");
    result = result.replace("\x1b[36m", "<span style='color: #1abc9c;'>");
    result = result.replace("\x1b[37m", "<span style='color: #ffffff;'>");
    result = result.replace("\x1b[90m", "<span style='color: #808080;'>");
    result = result.replace("\x1b[91m", "<span style='color: #e94560;'>");
    result = result.replace("\x1b[92m", "<span style='color: #27ae60;'>");
    result = result.replace("\x1b[93m", "<span style='color: #f1c40f;'>");
    result = result.replace("\x1b[94m", "<span style='color: #3498db;'>");
    result = result.replace("\x1b[95m", "<span style='color: #9b59b6;'>");
    result = result.replace("\x1b[96m", "<span style='color: #1abc9c;'>");
    result = result.replace("\x1b[97m", "<span style='color: #ffffff;'>");
    result = result.replace("\x1b[1m", "<span style='font-weight: bold;'>");
    result = result.replace("\x1b[0m", "</span>");
    result = result.replace("\x1b[m", "</span>");
    result = result.replace("\x1b[K", "");  // 清除到行尾

    return result;
}

void TerminalWidget::scrollToBottom()
{
    QTimer::singleShot(0, this, [this]() {
        QTextCursor cursor = m_outputArea->textCursor();
        cursor.movePosition(QTextCursor::End);
        m_outputArea->setTextCursor(cursor);
    });

    // 限制输出行数
    QTextDocument* doc = m_outputArea->document();
    while (doc->blockCount() > m_maxOutputLines) {
        QTextCursor cursor(doc->begin());
        cursor.select(QTextCursor::LineUnderCursor);
        cursor.removeSelectedText();
        cursor.deleteChar();
    }
}

bool TerminalWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_inputLine && event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);

        if (keyEvent->key() == Qt::Key_Up) {
            onHistoryUp();
            return true;
        }

        if (keyEvent->key() == Qt::Key_Down) {
            onHistoryDown();
            return true;
        }

        if (keyEvent->key() == Qt::Key_Tab) {
            // 自动补全
            QString current = m_inputLine->text();
            for (const QString& cmd : m_availableCommands) {
                if (cmd.startsWith(current, Qt::CaseInsensitive) && cmd != current) {
                    m_inputLine->setText(cmd);
                    break;
                }
            }
            return true;
        }
    }

    return QWidget::eventFilter(watched, event);
}

} // namespace DeepLux
