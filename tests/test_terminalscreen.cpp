#include <QtTest/QtTest>
#include <ui/terminal/TerminalScreen.h>

using namespace DeepLux;

class TestTerminalScreen : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testBasicWrite();
    void testCursorMovement();
    void testClearOperations();
    void testScroll();
    void testInsertDelete();
    void testAlternateScreen();
    void testScrollback();
    void testResize();
    void testTabExpansion();
    void testCjkWideChar();
    void testLineWrapping();
};

void TestTerminalScreen::initTestCase()
{
    qDebug() << "=== TestTerminalScreen Start ===";
}

void TestTerminalScreen::cleanupTestCase()
{
    qDebug() << "=== TestTerminalScreen End ===";
}

void TestTerminalScreen::testBasicWrite()
{
    TerminalScreen screen(5, 10);
    screen.putString("Hello");

    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorCol(), 5);
    QCOMPARE(screen.cellAt(0, 0).character, QChar('H'));
    QCOMPARE(screen.cellAt(0, 4).character, QChar('o'));
    QCOMPARE(screen.cellAt(0, 5).character, QChar(' '));
}

void TestTerminalScreen::testCursorMovement()
{
    TerminalScreen screen(5, 10);
    screen.putString("AB");
    screen.setCursor(2, 3);
    QCOMPARE(screen.cursorRow(), 2);
    QCOMPARE(screen.cursorCol(), 3);

    screen.moveCursor(1, 2);
    QCOMPARE(screen.cursorRow(), 3);
    QCOMPARE(screen.cursorCol(), 5);

    screen.saveCursor();
    screen.setCursor(0, 0);
    screen.restoreCursor();
    QCOMPARE(screen.cursorRow(), 3);
    QCOMPARE(screen.cursorCol(), 5);
}

void TestTerminalScreen::testClearOperations()
{
    TerminalScreen screen(5, 10);
    screen.putString("HelloWorld");
    screen.setCursor(0, 2);
    screen.clearToEndOfLine();

    QCOMPARE(screen.cellAt(0, 0).character, QChar('H'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar('e'));
    QCOMPARE(screen.cellAt(0, 2).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 9).character, QChar(' '));

    screen.putString("TestData!!");
    screen.setCursor(0, 0);
    screen.clearLine();
    for (int c = 0; c < 10; ++c) {
        QCOMPARE(screen.cellAt(0, c).character, QChar(' '));
    }

    screen.putString("Row0");
    screen.setCursor(1, 0);
    screen.putString("Row1");
    screen.setCursor(2, 0);
    screen.putString("Row2");
    screen.setCursor(1, 0);
    screen.clearScreen();
    for (int r = 0; r < 5; ++r) {
        for (int c = 0; c < 10; ++c) {
            QCOMPARE(screen.cellAt(r, c).character, QChar(' '));
        }
    }
    QCOMPARE(screen.cursorRow(), 0);
    QCOMPARE(screen.cursorCol(), 0);
}

void TestTerminalScreen::testScroll()
{
    TerminalScreen screen(5, 10);
    screen.setScrollRegion(0, 4);

    for (int i = 0; i < 6; ++i) {
        screen.putString(QString("Line%1").arg(i));
        screen.putChar('\n');
    }

    // After 6 lines with screen height 5, two lines should be in scrollback
    QCOMPARE(screen.scrollbackSize(), 2);
    QCOMPARE(screen.cellAt(0, 0).character, QChar('L'));
    QCOMPARE(screen.cellAt(0, 4).character, QChar('2'));
}

void TestTerminalScreen::testInsertDelete()
{
    TerminalScreen screen(5, 10);
    screen.putString("ABCDE");
    screen.setCursor(0, 1);
    screen.insertChars(2);

    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 2).character, QChar(' '));
    QCOMPARE(screen.cellAt(0, 3).character, QChar('B'));

    screen.clearScreen();
    screen.putString("ABCDE");
    screen.setCursor(0, 1);
    screen.deleteChars(2);

    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar('D'));
    QCOMPARE(screen.cellAt(0, 2).character, QChar('E'));
}

void TestTerminalScreen::testAlternateScreen()
{
    TerminalScreen screen(3, 5);
    screen.putString("Main");

    screen.useAlternateScreen(true);
    screen.putString("Alt");
    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));

    screen.useAlternateScreen(false);
    QCOMPARE(screen.cellAt(0, 0).character, QChar('M'));
}

void TestTerminalScreen::testScrollback()
{
    TerminalScreen screen(3, 5);
    screen.setScrollRegion(0, 2);

    for (int i = 0; i < 5; ++i) {
        screen.putString(QString::number(i));
        screen.putChar('\n');
    }

    QCOMPARE(screen.scrollbackSize(), 3);
    QCOMPARE(screen.scrollbackLine(0)[0].character, QChar('0'));
    QCOMPARE(screen.scrollbackLine(1)[0].character, QChar('1'));
    QCOMPARE(screen.scrollbackLine(2)[0].character, QChar('2'));
}

void TestTerminalScreen::testResize()
{
    TerminalScreen screen(3, 5);
    screen.putString("Hello");
    screen.resize(4, 8);

    QCOMPARE(screen.rows(), 4);
    QCOMPARE(screen.cols(), 8);
    QCOMPARE(screen.cellAt(0, 0).character, QChar('H'));
    QCOMPARE(screen.cellAt(0, 4).character, QChar('o'));
}

void TestTerminalScreen::testTabExpansion()
{
    TerminalScreen screen(3, 16);
    screen.putString("A");
    screen.putChar('\t');
    QCOMPARE(screen.cursorCol(), 8);
    screen.putString("B");
    screen.putChar('\t');
    QCOMPARE(screen.cursorCol(), 16);
}

void TestTerminalScreen::testCjkWideChar()
{
    TerminalScreen screen(3, 10);
    screen.putString("A");
    screen.putChar(QChar(0x4E2D)); // 中
    screen.putString("B");

    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 1).character, QChar(0x4E2D)); // 中
    QCOMPARE(screen.cellAt(0, 1).isWide, true);
    QCOMPARE(screen.cellAt(0, 2).character, QChar(' ')); // 右半部分占位
    QCOMPARE(screen.cellAt(0, 2).isWide, true);
    QCOMPARE(screen.cellAt(0, 3).character, QChar('B'));
    QCOMPARE(screen.cursorCol(), 4);
}

void TestTerminalScreen::testLineWrapping()
{
    TerminalScreen screen(3, 5);
    screen.putString("ABCDEFGHIJ");

    QCOMPARE(screen.cursorRow(), 2);
    QCOMPARE(screen.cursorCol(), 0);
    QCOMPARE(screen.cellAt(0, 0).character, QChar('A'));
    QCOMPARE(screen.cellAt(0, 4).character, QChar('E'));
    QCOMPARE(screen.cellAt(1, 0).character, QChar('F'));
    QCOMPARE(screen.cellAt(1, 4).character, QChar('J'));
}

QTEST_MAIN(TestTerminalScreen)
#include "test_terminalscreen.moc"
