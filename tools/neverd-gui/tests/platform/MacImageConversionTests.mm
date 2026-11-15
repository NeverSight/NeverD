//===- MacImageConversionTests.mm - Native image and cursor ownership -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the Qt/CoreGraphics boundary used by Cocoa toolbar cursors.
/// Native images must retain their color space and pixels after conversion.
//===----------------------------------------------------------------------===//

#include <CoreGraphics/CoreGraphics.h>
#include <QApplication>
#include <QByteArray>
#include <QColorSpace>
#include <QCursor>
#include <QImage>
#include <QPixmap>
#include <QScopeGuard>
#include <QTest>
#include <QWidget>

class MacImageConversionTests final : public QObject {
  Q_OBJECT

private slots:
  void colorSpaceAndPixelsSurviveConversion_data() {
    QTest::addColumn<QImage::Format>("format");
    QTest::addColumn<QColorSpace>("colorSpace");
    for (auto Format : {QImage::Format_ARGB32, QImage::Format_RGBA8888}) {
      const auto Prefix = QByteArray::number(Format) + '-';
      QTest::newRow((Prefix + "default").constData())
          << Format << QColorSpace();
      QTest::newRow((Prefix + "srgb").constData())
          << Format << QColorSpace(QColorSpace::SRgb);
      QTest::newRow((Prefix + "display-p3").constData())
          << Format << QColorSpace(QColorSpace::DisplayP3);
      QTest::newRow((Prefix + "custom-gamma").constData())
          << Format
          << QColorSpace(QColorSpace::Primaries::SRgb,
                         QColorSpace::TransferFunction::Gamma, 1.8f);
    }
  }

  void colorSpaceAndPixelsSurviveConversion() {
    QFETCH(QImage::Format, format);
    QFETCH(QColorSpace, colorSpace);
    QImage Image(16, 16, format);
    Image.setColorSpace(colorSpace);
    for (int Y = 0; Y != Image.height(); ++Y)
      for (int X = 0; X != Image.width(); ++X)
        Image.setPixel(X, Y, qRgba(X * 15, Y * 15, X ^ Y, 255));
    const QByteArray Expected(reinterpret_cast<const char *>(Image.constBits()),
                              Image.sizeInBytes());
    CGImageRef Native = Image.toCGImage();
    QVERIFY(Native);
    const auto ReleaseImage = qScopeGuard([&] { CGImageRelease(Native); });
    Image = {};

    QCOMPARE(CGImageGetWidth(Native), size_t(16));
    QCOMPARE(CGImageGetHeight(Native), size_t(16));
    CGColorSpaceRef Space = CGImageGetColorSpace(Native);
    QVERIFY(Space);
    QCOMPARE(CFGetTypeID(Space), CGColorSpaceGetTypeID());
    QCOMPARE(CGColorSpaceGetModel(Space), kCGColorSpaceModelRGB);
    CFDataRef Pixels = CGDataProviderCopyData(CGImageGetDataProvider(Native));
    QVERIFY(Pixels);
    const auto ReleasePixels = qScopeGuard([&] { CFRelease(Pixels); });
    QCOMPARE(
        QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(Pixels)),
                   CFDataGetLength(Pixels)),
        Expected);
  }

  void nativeToolbarAndBitmapCursors() {
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
      QSKIP("Native cursor conversion requires Cocoa");
    QWidget Window;
    Window.resize(160, 120);
    Window.show();
    if (!QTest::qWaitForWindowExposed(&Window))
      QSKIP("Native cursor conversion requires an exposed desktop window");
    for (auto Shape : {Qt::SizeAllCursor, Qt::UpArrowCursor, Qt::WaitCursor,
                       Qt::BlankCursor, Qt::ArrowCursor}) {
      QApplication::setOverrideCursor(QCursor(Shape));
      QCoreApplication::processEvents();
      QApplication::restoreOverrideCursor();
    }
    QImage Image(16, 16, QImage::Format_ARGB32);
    Image.fill(Qt::blue);
    Image.setColorSpace(QColorSpace(QColorSpace::Primaries::SRgb,
                                    QColorSpace::TransferFunction::Gamma,
                                    1.8f));
    QApplication::setOverrideCursor(QCursor(QPixmap::fromImage(Image)));
    QCoreApplication::processEvents();
    QApplication::restoreOverrideCursor();
  }
};

QTEST_MAIN(MacImageConversionTests)
#include "MacImageConversionTests.moc"
