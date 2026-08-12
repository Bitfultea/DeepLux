#include "plugins/image_processing/GrabImage/GrabImagePlugin.h"

#include <QImage>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace DeepLux;

class TestGrabImage : public QObject {
    Q_OBJECT

private slots:
    void readsFolderInNameOrder();
};

void TestGrabImage::readsFolderInNameOrder() {
#ifndef DEEPLUX_HAS_OPENCV
    QSKIP("GrabImage file loading requires OpenCV");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QImage image(8, 6, QImage::Format_Mono);
    image.setColor(0, qRgb(255, 0, 0));
    image.fill(0);
    QVERIFY(image.save(directory.filePath(QStringLiteral("02.png"))));
    image.setColor(0, qRgb(0, 255, 0));
    QVERIFY(image.save(directory.filePath(QStringLiteral("01.png"))));

    GrabImagePlugin plugin;
    QVERIFY(plugin.initialize());
    plugin.setParam(QStringLiteral("filePath"), directory.path());
    plugin.setParam(QStringLiteral("folderLoop"), false);
    QCOMPARE(plugin.currentParams().value(QStringLiteral("grabSource")).toString(), QStringLiteral("Path"));

    ImageData input;
    ImageData output;
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data(QStringLiteral("source_file_name")).toString(), QStringLiteral("01.png"));
    QCOMPARE(output.data(QStringLiteral("folder_position")).toInt(), 1);
    QCOMPARE(output.data(QStringLiteral("folder_count")).toInt(), 2);
    QVERIFY(output.toQImage().pixelColor(0, 0).green() > 200);

    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data(QStringLiteral("source_file_name")).toString(), QStringLiteral("02.png"));
    QCOMPARE(output.data(QStringLiteral("folder_position")).toInt(), 2);
    QVERIFY(output.toQImage().pixelColor(0, 0).red() > 200);
    QVERIFY(!plugin.execute(input, output));

    plugin.setParam(QStringLiteral("folderLoop"), true);
    QVERIFY(plugin.execute(input, output));
    QCOMPARE(output.data(QStringLiteral("source_file_name")).toString(), QStringLiteral("01.png"));

    GrabImagePlugin legacyPlugin;
    legacyPlugin.setParams(
        {{QStringLiteral("grabSource"), QStringLiteral("Folder")}, {QStringLiteral("folderPath"), directory.path()}});
    QCOMPARE(legacyPlugin.currentParams().value(QStringLiteral("filePath")).toString(), directory.path());
    QCOMPARE(legacyPlugin.currentParams().value(QStringLiteral("grabSource")).toString(), QStringLiteral("Path"));
#endif
}

QTEST_MAIN(TestGrabImage)
#include "test_grabimage.moc"
