//===- WheelInputTests.cpp - Wheel and trackpad behavior ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replay precision input through the production text views.
//===----------------------------------------------------------------------===//

#include "AddressSpace.h"
#include "CodeView.h"
#include "HexView.h"
#include "ListingView.h"
#include "Session.h"
#include "Theme.h"
#include "WindowFrame.h"

#include <QApplication>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>
#include <memory>
#include <utility>

namespace neverd::gui {
// Seed a loaded viewport without requiring a worker or a binary fixture.
struct ListingViewTestAccess {
  static void seedScrolling(ListingView &view) {
    view.lines_.clear();
    for (int row = 0; row < 300; ++row) {
      ListingView::Line line;
      line.item = line.address = row;
      line.styled.text = QStringLiteral("mov eax, ebx");
      view.lines_.push_back(std::move(line));
    }
    view.top_ = view.cursorLine_ = 100;
  }
};
} // namespace neverd::gui

using namespace neverd::gui;

namespace {
void wheel(QAbstractScrollArea &view, QPoint pixels, QPoint angles,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           Qt::ScrollPhase phase = Qt::ScrollUpdate) {
  QWheelEvent event(QPointF(20, 20), QPointF(20, 20), pixels, angles,
                    Qt::NoButton, modifiers, phase, false);
  QApplication::sendEvent(view.viewport(), &event);
}
} // namespace

class WheelInputTests final : public QObject {
  Q_OBJECT
public:
  static void initMain() { WindowFrame::prepareNativeWindows(); }

private slots:
  void initTestCase() {
    QVERIFY(settings_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WheelInputTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    for (const auto scope : {QSettings::UserScope, QSettings::SystemScope})
      QSettings::setPath(QSettings::IniFormat, scope, settings_.path());
    Theme::instance().apply();
  }

  void init() {
    QFont font = Theme::defaultCodeFont();
    font.setPointSize(10);
    Theme::instance().setCodeFont(font);
  }

  void smallScrollsKeepTheirTotal_data() {
    QTest::addColumn<QPoint>("pixels");
    QTest::addColumn<QPoint>("angles");
    QTest::newRow("precision-wheel") << QPoint() << QPoint(0, -5);
    QTest::newRow("pixel-only-trackpad") << QPoint(0, -2) << QPoint();
    QTest::newRow("trackpad-with-angles") << QPoint(0, -2) << QPoint(0, -5);
  }

  void smallScrollsKeepTheirTotal() {
    QFETCH(QPoint, pixels);
    QFETCH(QPoint, angles);
    Session session(QStringLiteral("unused-worker"));
    AddressSpace space;
    ListingView listing(session, space);
    listing.resize(800, 300);
    ListingViewTestAccess::seedScrolling(listing);
    for (int event = 0; event < 24; ++event)
      wheel(listing, pixels, angles);
    const auto afterSmall = listing.topItem();
    QVERIFY(afterSmall && *afterSmall > 100);
    ListingViewTestAccess::seedScrolling(listing);
    wheel(listing, pixels * 24, angles * 24, Qt::NoModifier, Qt::ScrollBegin);
    QCOMPARE(listing.topItem(), afterSmall);

    CodeText source(session);
    source.verticalScrollBar()->setRange(0, 500);
    source.verticalScrollBar()->setValue(100);
    for (int event = 0; event < 24; ++event)
      wheel(source, pixels, angles);
    const int sourceAfterSmall = source.verticalScrollBar()->value();
    QVERIFY(sourceAfterSmall > 100);
    source.verticalScrollBar()->setValue(100);
    wheel(source, pixels * 24, angles * 24, Qt::NoModifier, Qt::ScrollBegin);
    QCOMPARE(source.verticalScrollBar()->value(), sourceAfterSmall);
  }

  void sourceScrollsHorizontally_data() {
    QTest::addColumn<QPoint>("pixels");
    QTest::addColumn<QPoint>("angles");
    QTest::newRow("trackpad") << QPoint(-40, 0) << QPoint(-120, 0);
    QTest::newRow("tilt-wheel") << QPoint() << QPoint(-120, 0);
  }

  void sourceScrollsHorizontally() {
    QFETCH(QPoint, pixels);
    QFETCH(QPoint, angles);
    Session session(QStringLiteral("unused-worker"));
    CodeText source(session);
    source.horizontalScrollBar()->setRange(0, 500);
    source.horizontalScrollBar()->setValue(100);
    wheel(source, pixels, angles);
    QVERIFY(source.horizontalScrollBar()->value() > 100);
  }

  void zoomUsesMovement_data() {
    QTest::addColumn<QString>("view");
    for (const char *view : {"listing", "source", "hex"})
      QTest::newRow(view) << QString::fromLatin1(view);
  }

  void zoomUsesMovement() {
    QFETCH(QString, view);
    Session session(QStringLiteral("unused-worker"));
    AddressSpace space;
    std::unique_ptr<QAbstractScrollArea> area;
    if (view == QLatin1String("listing"))
      area = std::make_unique<ListingView>(session, space);
    else if (view == QLatin1String("source"))
      area = std::make_unique<CodeText>(session);
    else
      area = std::make_unique<HexView>(session, space);
    const int original = Theme::instance().codeFont().pointSize();
    for (const auto phase : {Qt::ScrollBegin, Qt::ScrollEnd})
      wheel(*area, {}, {}, Qt::ControlModifier, phase);
    QCOMPARE(Theme::instance().codeFont().pointSize(), original);
    wheel(*area, QPoint(20, 0), QPoint(120, 0), Qt::ControlModifier);
    QCOMPARE(Theme::instance().codeFont().pointSize(), original);
    for (int event = 0; event < 24; ++event)
      wheel(*area, QPoint(0, 2), QPoint(0, 5), Qt::ControlModifier);
    QCOMPARE(Theme::instance().codeFont().pointSize(), original + 1);
    wheel(*area, {}, {}, Qt::ControlModifier, Qt::ScrollEnd);
    QCOMPARE(Theme::instance().codeFont().pointSize(), original + 1);
    wheel(*area, {}, QPoint(0, -120), Qt::ControlModifier);
    QCOMPARE(Theme::instance().codeFont().pointSize(), original);
    // Devices with pixel movement alone still zoom in the supplied direction.
    wheel(*area, QPoint(0, 64), {}, Qt::ControlModifier, Qt::ScrollBegin);
    QVERIFY(Theme::instance().codeFont().pointSize() > original);
  }

private:
  QTemporaryDir settings_;
};

QTEST_MAIN(WheelInputTests)
#include "WheelInputTests.moc"
