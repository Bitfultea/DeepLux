#include <QtTest/QtTest>
#include <core/model/ImageData.h>

using namespace DeepLux;

class TestImageData : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testDefaultConstructor();
    void testQImageConstructor();
    void testCopyConstructor();
    void testMoveConstructor();
    void testCopyAssignment();
    void testMoveAssignment();
    void testImageDimensions();
    void testMetadata();
    void testTimestamp();
    void testSerialization();
    void testToQImage();
    void testIsValid();
    void testIsEmpty();

#ifdef DEEPLUX_HAS_OPENCV
    void testMatConstructor();
    void testToMat();
    void testHasMat();
#endif

private:
};

void TestImageData::initTestCase()
{
    qDebug() << "=== TestImageData Start ===";
}

void TestImageData::cleanupTestCase()
{
    qDebug() << "=== TestImageData End ===";
}

void TestImageData::testDefaultConstructor()
{
    ImageData img;
    QVERIFY(!img.isValid());
    QVERIFY(img.isEmpty());
    QVERIFY(img.width() == 0);
    QVERIFY(img.height() == 0);
}

void TestImageData::testQImageConstructor()
{
    QImage img(100, 200, QImage::Format_Grayscale8);
    img.fill(Qt::black);

    ImageData imageData(img);

    QVERIFY(imageData.isValid());
    QVERIFY(!imageData.isEmpty());
    QVERIFY(imageData.width() == 100);
    QVERIFY(imageData.height() == 200);
}

void TestImageData::testCopyConstructor()
{
    QImage img(50, 50, QImage::Format_Grayscale8);
    img.fill(Qt::white);

    ImageData original(img);
    original.setData("key", "value");
    original.setTimestamp(12345);

    ImageData copy(original);

    QVERIFY(copy.isValid());
    QVERIFY(copy.width() == original.width());
    QVERIFY(copy.height() == original.height());
    QVERIFY(copy.data("key").toString() == "value");
    QVERIFY(copy.timestamp() == 12345);
}

void TestImageData::testMoveConstructor()
{
    QImage img(50, 50, QImage::Format_Grayscale8);
    img.fill(Qt::red);

    ImageData original(img);
    original.setData("key", "value");

    ImageData moved(std::move(original));

    QVERIFY(moved.isValid());
    QVERIFY(moved.width() == 50);
    QVERIFY(moved.data("key").toString() == "value");
}

void TestImageData::testCopyAssignment()
{
    QImage img(75, 75, QImage::Format_Grayscale8);
    img.fill(Qt::blue);

    ImageData original(img);
    original.setData("testKey", "testValue");

    ImageData copy;
    copy = original;

    QVERIFY(copy.isValid());
    QVERIFY(copy.width() == 75);
    QVERIFY(copy.height() == 75);
    QVERIFY(copy.data("testKey").toString() == "testValue");
}

void TestImageData::testMoveAssignment()
{
    QImage img(80, 80, QImage::Format_Grayscale8);
    img.fill(Qt::green);

    ImageData original(img);
    original.setData("moveKey", "moveValue");

    ImageData moved;
    moved = std::move(original);

    QVERIFY(moved.isValid());
    QVERIFY(moved.width() == 80);
    QVERIFY(moved.data("moveKey").toString() == "moveValue");
}

void TestImageData::testImageDimensions()
{
    QImage img(320, 240, QImage::Format_RGB32);
    ImageData imageData(img);

    QVERIFY(imageData.width() == 320);
    QVERIFY(imageData.height() == 240);
}

void TestImageData::testMetadata()
{
    ImageData img;

    // Test setData and data
    img.setData("stringKey", "stringValue");
    img.setData("intKey", 42);
    img.setData("doubleKey", 3.14);

    QVERIFY(img.hasData("stringKey"));
    QVERIFY(img.hasData("intKey"));
    QVERIFY(img.hasData("doubleKey"));

    QCOMPARE(img.data("stringKey").toString(), QString("stringValue"));
    QCOMPARE(img.data("intKey").toInt(), 42);
    QVERIFY(qAbs(img.data("doubleKey").toDouble() - 3.14) < 0.001);

    // Test removeData
    img.removeData("stringKey");
    QVERIFY(!img.hasData("stringKey"));

    // Test allData
    QMap<QString, QVariant> allData = img.allData();
    QVERIFY(allData.contains("intKey"));
    QVERIFY(allData.contains("doubleKey"));

    // Test clearData
    img.clearData();
    QVERIFY(img.allData().isEmpty());
}

void TestImageData::testTimestamp()
{
    ImageData img;

    qint64 testTimestamp = 1234567890;
    img.setTimestamp(testTimestamp);

    QVERIFY(img.timestamp() == testTimestamp);
}

void TestImageData::testSerialization()
{
    QImage img(100, 100, QImage::Format_Grayscale8);
    img.fill(Qt::white);

    ImageData original(img);
    original.setData("meta", "test");

    QByteArray serialized = original.toByteArray();
    QVERIFY(!serialized.isEmpty());

    ImageData restored = ImageData::fromByteArray(serialized);
    QVERIFY(restored.isValid());
    QVERIFY(restored.width() == original.width());
    QVERIFY(restored.height() == original.height());
}

void TestImageData::testToQImage()
{
    QImage img(150, 150, QImage::Format_Grayscale8);
    img.fill(Qt::gray);

    ImageData imageData(img);
    QImage converted = imageData.toQImage();

    QVERIFY(!converted.isNull());
    QVERIFY(converted.width() == 150);
    QVERIFY(converted.height() == 150);
}

void TestImageData::testIsValid()
{
    ImageData emptyImg;
    QVERIFY(!emptyImg.isValid());

    QImage validImg(10, 10, QImage::Format_Grayscale8);
    ImageData validImageData(validImg);
    QVERIFY(validImageData.isValid());
}

void TestImageData::testIsEmpty()
{
    ImageData emptyImg;
    QVERIFY(emptyImg.isEmpty());

    QImage validImg(10, 10, QImage::Format_Grayscale8);
    ImageData validImageData(validImg);
    QVERIFY(!validImageData.isEmpty());
}

#ifdef DEEPLUX_HAS_OPENCV
void TestImageData::testMatConstructor()
{
    cv::Mat mat(100, 200, CV_8UC1, cv::Scalar(128));

    ImageData imageData(mat);

    QVERIFY(imageData.isValid());
    QVERIFY(imageData.width() == 200);
    QVERIFY(imageData.height() == 100);
}

void TestImageData::testToMat()
{
    QImage img(100, 100, QImage::Format_Grayscale8);
    img.fill(Qt::white);

    ImageData imageData(img);
    cv::Mat mat = imageData.toMat();

    QVERIFY(!mat.empty());
    QVERIFY(mat.cols == 100);
    QVERIFY(mat.rows == 100);
}

void TestImageData::testHasMat()
{
    ImageData emptyImg;
    QVERIFY(!emptyImg.hasMat());

    QImage img(50, 50, QImage::Format_Grayscale8);
    ImageData imageData(img);
    QVERIFY(imageData.hasMat());
}
#endif

QTEST_MAIN(TestImageData)
#include "test_imagedata.moc"
