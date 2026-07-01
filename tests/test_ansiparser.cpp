#include <QtTest/QtTest>
#include <ui/terminal/TerminalScreen.h>
#include <ui/terminal/AnsiParser.h>

using namespace DeepLux;

class TestAnsiParser : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testPlainText();
    void testBasicColors();
    void testCursorMovement();
    void testClearScreen();
    void testClearLine();
    void testBoldItalic();
    void test256Color();
    void testTrueColor();
    void testCursorSaveRestore();
    void testScrollRegion();
    void testAlternateScreen();
    void testCombinedAttributes();
    void testBackspace();
    void testTab();
    void testCliCommand();
    void testUtf8Chinese();
};

void TestAnsiParser::initTestCase()
{
    qDebug() << "=== TestAnsiParser Start ===";
}

void TestAnsiParser::cleanupTestCase()
{
    qDebug() << "=== TestAnsiParser End ===";
}

void TestAnsiParser::testPlainText()
{
    TerminalScreen screen(5, 20);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("Hello World"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar('H'));
    QCOMPARE(screen.cellAt(0, 10).character, QChar('d'));
    QCOMPARE(screen.cursorCol(), 11);
}

void TestAnsiParser::testBasicColors()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    // Red text: ESC[31m
    parser.parse(QByteArray("\x1b[31mRed\x1b[0m"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar('R'));
    QCOMPARE(screen.cellAt(0, 0).attrs.fgValue, 1); // red
    QCOMPARE(screen.cellAt(0, 3).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 3).attrs.fgValue, 7); // reset to default white
}

void TestAnsiParser::testCursorMovement()
{
    TerminalScreen screen(5, 10);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("AB\x1b[2;3HXY"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar('B'));
    QCOMPARE(screen.cellAt(1, 2).character, QChar('X'));
    QCOMPARE(screen.cellAt(1, 3).character, QChar('Y'));
}

void TestAnsiParser::testClearScreen()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("HelloWorld"));
    parser.parse(QByteArray("\x1b[2J"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar(' '));
    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorCol(), 0);
}

void TestAnsiParser::testClearLine()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("HelloWorld"));
    parser.parse(QByteArray("\x1b[1;1H"));  // move cursor to row 0
    parser.parse(QByteArray("\x1b[2K"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 9).character, QChar(' '));
}

void TestAnsiParser::testBoldItalic()
{
    TerminalScreen screen(3, 20);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("\x1b[1mBold\x1b[0m \x1b[3mItalic\x1b[0m"));

    QCOMPARE(screen.cellAt(0, 0).attrs.bold, true);
    QCOMPARE(screen.cellAt(0, 5).attrs.bold, false);
    QCOMPARE(screen.cellAt(0, 6).attrs.italic, true);
}

void TestAnsiParser::test256Color()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    // 256-color foreground: ESC[38;5;196m
    parser.parse(QByteArray("\x1b[38;5;196mRed256\x1b[0m"));

    QCOMPARE(screen.cellAt(0, 0).attrs.fgType, ColorType::Palette256);
    QCOMPARE(screen.cellAt(0, 0).attrs.fgValue, 196);
    QCOMPARE(screen.cellAt(0, 7).attrs.fgType, ColorType::Basic);
}

void TestAnsiParser::testTrueColor()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    // True color: ESC[38;2;255;128;0m
    parser.parse(QByteArray("\x1b[38;2;255;128;0mOrange\x1b[0m"));

    QCOMPARE(screen.cellAt(0, 0).attrs.fgType, ColorType::TrueColor);
    QCOMPARE(screen.cellAt(0, 0).attrs.fgValue, (255 << 16) | (128 << 8) | 0);
}

void TestAnsiParser::testCursorSaveRestore()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("AB\x1b[3;4H"));
    parser.parse(QByteArray("\x1b") + "7");  // ESC 7 = save cursor
    parser.parse(QByteArray("\x1b[1;1HXY"));
    parser.parse(QByteArray("\x1b") + "8");  // ESC 8 = restore cursor
    parser.parse(QByteArray("Z"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar('X'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar('Y'));
    QCOMPARE(screen.cellAt(2, 3).character, QChar('Z'));
}

void TestAnsiParser::testScrollRegion()
{
    TerminalScreen screen(5, 10);
    AnsiParser parser(&screen);
    // Set scroll region to rows 1-3 (0-indexed: 0-2)
    parser.parse(QByteArray("\x1b[1;3r"));
    QCOMPARE(screen.scrollTop(), 0);
    QCOMPARE(screen.scrollBottom(), 2);
}

void TestAnsiParser::testAlternateScreen()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("Main"));
    parser.parse(QByteArray("\x1b[?1049h")); // enter alternate screen
    parser.parse(QByteArray("Alt"));
    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    parser.parse(QByteArray("\x1b[?1049l")); // exit alternate screen
    QCOMPARE(screen.cellAt(0, 0).character, QChar('M'));
}

void TestAnsiParser::testCombinedAttributes()
{
    TerminalScreen screen(3, 20);
    AnsiParser parser(&screen);
    // Bold + Italic + Underline + Red foreground + Blue background
    parser.parse(QByteArray("\x1b[1;3;4;31;44mCombo\x1b[0m"));

    QCOMPARE(screen.cellAt(0, 0).attrs.bold, true);
    QCOMPARE(screen.cellAt(0, 0).attrs.italic, true);
    QCOMPARE(screen.cellAt(0, 0).attrs.underline, true);
    QCOMPARE(screen.cellAt(0, 0).attrs.fgValue, 1); // red
    QCOMPARE(screen.cellAt(0, 0).attrs.bgValue, 4); // blue
}

void TestAnsiParser::testBackspace()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);
    QByteArray bsData;
    bsData.append("ABC");
    bsData.append(char(0x08));
    bsData.append("D");
    parser.parse(bsData);

    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar('B'));
    QCOMPARE(screen.cellAt(0, 2).character, QChar('D'));
}

void TestAnsiParser::testTab()
{
    TerminalScreen screen(3, 20);
    AnsiParser parser(&screen);
    parser.parse(QByteArray("A\tB"));

    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 8).character, QChar('B'));
}

void TestAnsiParser::testCliCommand()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);

    QString capturedCommand;
    connect(&parser, &AnsiParser::cliCommandReceived, this, [&](const QString& cmd) {
        capturedCommand = cmd;
    });

    // OSC 888: ESC ] 888 ; deeplux ; run BEL
    QByteArray oscData;
    oscData.append(char(0x1B));
    oscData.append(']');
    oscData.append("888;deeplux;run");
    oscData.append(char(0x07));

    parser.parse(oscData);

    QCOMPARE(capturedCommand, QString("run"));
}

void TestAnsiParser::testUtf8Chinese()
{
    TerminalScreen screen(3, 10);
    AnsiParser parser(&screen);

    // "你好" in UTF-8: 你=E4 BD A0, 好=E5 A5 BD
    QByteArray utf8Data;
    utf8Data.append("\xE4\xBD\xA0"); // 你
    utf8Data.append("\xE5\xA5\xBD"); // 好

    parser.parse(utf8Data);

    // 中文字符应占 2 列
    QCOMPARE(screen.cellAt(0, 0).character, QChar(QString::fromUtf8("\xE4\xBD\xA0").at(0)));
    QCOMPARE(screen.cellAt(0, 0).isWide, true);

    QCOMPARE(screen.cellAt(0, 1).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 1).isWide, true);

    QCOMPARE(screen.cellAt(0, 2).character, QChar(QString::fromUtf8("\xE5\xA5\xBD").at(0)));
    QCOMPARE(screen.cellAt(0, 2).isWide, true);

    QCOMPARE(screen.cellAt(0, 3).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 3).isWide, true);
}

QTEST_MAIN(TestAnsiParser)
#include "test_ansiparser.moc"
