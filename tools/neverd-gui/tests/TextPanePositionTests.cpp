#include "NativeCodeHighlighter.h"
#include "Workbench.h"

#include <QFile>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <cmath>
#include <memory>

namespace {
const QString AddressA = QStringLiteral("0xffff800012340168");
const QString AddressB = QStringLiteral("0xffff800012340169");

QString lines(const QString &prefix, int count, int first = 0) {
  QString result;
  for (int line = first; line < first + count; ++line)
    result += QStringLiteral("%1 operation %2\n").arg(prefix).arg(line);
  return result;
}

QVariantMap anchor(int line, const QString &address) {
  return {{"line", line},
          {"object_id", QStringLiteral("test:op:%1").arg(line)},
          {"mapping_status", "instruction_anchor"},
          {"addresses", QVariantList{address}}};
}

// Both paths instantiate the production component from its source URL. Only
// the controller-binding wrapper is test QML; no TextPane or highlighter logic
// is copied or substituted here.
struct TextFixture {
  QQmlEngine engine;
  QQuickWindow window;
  std::unique_ptr<QObject> object;
  QQuickItem *pane = nullptr;
  QQuickItem *code = nullptr;
  QString error;

  bool load(Workbench *controller = nullptr) {
    QQmlComponent component(&engine);
    const QString directory = QString::fromUtf8(TEST_QML_DIR);
    if (controller) {
      engine.rootContext()->setContextProperty("controller", controller);
      component.setData(
          R"(
        import QtQuick
        import "."
        TextPane {
          text: controller.representationText
          mappings: controller.textMappings
          selectedAddress: controller.selectedAddress
        }
      )",
          QUrl::fromLocalFile(directory + "/_TextPositionHarness.qml"));
    } else {
      component.loadUrl(QUrl::fromLocalFile(directory + "/TextPane.qml"));
    }
    object.reset(component.createWithInitialProperties(
        {{"width", 640}, {"height", 220}, {"syntaxHighlight", true}}));
    error = component.errorString();
    pane = qobject_cast<QQuickItem *>(object.get());
    if (!pane)
      return false;
    code = pane->findChild<QQuickItem *>("codeText");
    if (!code) {
      error = "Production TextPane did not create codeText";
      return false;
    }
    window.resize(640, 220);
    pane->setParentItem(window.contentItem());
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
      error = "TextPane test window was not exposed";
      return false;
    }
    return true;
  }

  QTextDocument *document() const {
    auto *quick = code->property("textDocument").value<QQuickTextDocument *>();
    return quick ? quick->textDocument() : nullptr;
  }
  int cursorLine() const {
    auto *text = document();
    return text ? text->findBlock(code->property("cursorPosition").toInt())
                      .blockNumber()
                : -1;
  }
  bool cursorVisible() const {
    auto *view = viewport();
    if (!view || !pane->isVisible() || !code->isVisible() ||
        view->width() <= 0 || view->height() <= 0)
      return false;
    const auto rectangle = code->property("cursorRectangle").toRectF();
    const auto mapped = code->mapRectToItem(view, rectangle);
    return rectangle.height() > 0 && QRectF(0, 0, view->width(), view->height())
                                         .adjusted(-1, -1, 1, 1)
                                         .contains(mapped);
  }
  // Simulate the user already reading this LowIR location. This is only a
  // precondition; no setup scrolling is used for the final MedIR assertion.
  bool scrollCursorForSetup() {
    auto *view = viewport();
    if (!view || view->height() <= 0)
      return false;
    const auto rectangle = code->property("cursorRectangle").toRectF();
    const auto cursor = code->mapRectToItem(view, rectangle);
    const auto maximum =
        qMax(0.0, view->property("contentHeight").toDouble() - view->height());
    const auto target =
        view->property("contentY").toDouble() + cursor.y() - view->height() / 2;
    return view->setProperty("contentY", qBound(0.0, target, maximum));
  }
  bool setCursorLine(int line) {
    auto *text = document();
    if (!text)
      return false;
    const auto block = text->findBlockByNumber(line);
    return block.isValid() &&
           code->setProperty("cursorPosition", block.position());
  }
  QQuickItem *viewport() const {
    for (auto *item = code->parentItem(); item; item = item->parentItem())
      if (item->metaObject()->indexOfProperty("contentY") >= 0 &&
          item->metaObject()->indexOfProperty("contentHeight") >= 0)
        return item;
    return nullptr;
  }
  bool frame() {
    QSignalSpy rendered(&window, &QQuickWindow::frameSwapped);
    window.update();
    return rendered.wait(2000);
  }
};

#ifdef TEST_WORKER
QString mappingIdentity(const Workbench &controller) {
  const auto rows = controller.textMappings();
  return rows.isEmpty() ? QString{}
                        : rows.first().toMap()["object_id"].toString();
}
#endif
} // namespace

class TextPanePositionTests final : public QObject {
  Q_OBJECT
  QTemporaryDir settingsDirectory_;

private slots:
  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName("NeverDTests");
    QCoreApplication::setApplicationName("TextPanePositionTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
    qmlRegisterType<NativeCodeHighlighter>("NeverD.Native", 1, 0,
                                           "NativeCodeHighlighter");
  }

  void lateMappingRevealsTheSelectedInstruction() {
    TextFixture fixture;
    QVERIFY2(fixture.load(), qPrintable(fixture.error));
    fixture.pane->setProperty("selectedAddress", AddressA);
    fixture.pane->setProperty("text", lines("LowIR", 500));
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 0);

    // Address and text do not change when this later mapping arrives.
    fixture.pane->setProperty("mappings", QVariantList{anchor(420, AddressA)});
    QTRY_COMPARE(fixture.cursorLine(), 420);
    QTRY_VERIFY(fixture.cursorVisible());
  }

  void replacingRepresentationRestoresTheSameAddressAtItsNewLine() {
    TextFixture fixture;
    QVERIFY2(fixture.load(), qPrintable(fixture.error));
    fixture.pane->setProperty("text", lines("LowIR", 500));
    fixture.pane->setProperty("mappings", QVariantList{anchor(320, AddressA)});
    fixture.pane->setProperty("selectedAddress", AddressA);
    QTRY_COMPARE(fixture.cursorLine(), 320);
    QVERIFY(fixture.scrollCursorForSetup());
    QVERIFY(fixture.frame());
    QTRY_VERIFY(fixture.cursorVisible());

    fixture.pane->setProperty("mappings", QVariantList{});
    fixture.pane->setProperty("text", "");
    QVERIFY(fixture.frame());
    fixture.pane->setProperty("text", lines("MedIR", 500));
    QVERIFY(fixture.frame());
    fixture.pane->setProperty("mappings", QVariantList{anchor(410, AddressA)});
    QCOMPARE(fixture.pane->property("selectedAddress").toString(), AddressA);
    QTRY_COMPARE(fixture.cursorLine(), 410);
    QTRY_VERIFY(fixture.cursorVisible());
  }

  void queuedRestoreUsesTheLatestSelectionAndDocument() {
    TextFixture fixture;
    QVERIFY2(fixture.load(), qPrintable(fixture.error));
    fixture.pane->setProperty("text", lines("old", 500));
    fixture.pane->setProperty("mappings", QVariantList{anchor(440, AddressA)});
    fixture.pane->setProperty("selectedAddress", AddressA);
    // Retire A before its deferred position callback can execute.
    fixture.pane->setProperty("mappings", QVariantList{});
    fixture.pane->setProperty("text", "");
    fixture.pane->setProperty("selectedAddress", AddressB);
    fixture.pane->setProperty("text", lines("new", 500));
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 0);
    fixture.pane->setProperty("mappings", QVariantList{anchor(315, AddressB)});
    QTRY_COMPARE(fixture.cursorLine(), 315);
    QTRY_VERIFY(fixture.cursorVisible());
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 315);
  }

  void invalidatedAndMissingMappingsDoNotInventAJump() {
    TextFixture fixture;
    QVERIFY2(fixture.load(), qPrintable(fixture.error));
    fixture.pane->setProperty("text", lines("LowIR", 500));
    fixture.pane->setProperty("mappings", QVariantList{anchor(420, AddressA)});
    QVERIFY(fixture.frame());
    QVERIFY(fixture.setCursorLine(25));
    fixture.pane->setProperty("selectedAddress", AddressA);
    fixture.pane->setProperty("mappings", QVariantList{});
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 25);

    fixture.pane->setProperty("text", lines("unsupported", 500));
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 0);
    QVERIFY(fixture.setCursorLine(30));
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 30);
    QCOMPARE(fixture.pane->property("selectedAddress").toString(), AddressA);
  }

  void appendingKeepsTheUsersCursorAndScrollPosition() {
    TextFixture fixture;
    QVERIFY2(fixture.load(), qPrintable(fixture.error));
    const auto first = lines("LowIR", 500);
    fixture.pane->setProperty("text", first);
    fixture.pane->setProperty("mappings", QVariantList{anchor(40, AddressA)});
    fixture.pane->setProperty("selectedAddress", AddressA);
    QTRY_COMPARE(fixture.cursorLine(), 40);
    QVERIFY(fixture.setCursorLine(300));
    QVERIFY(fixture.frame());
    auto *viewport = fixture.viewport();
    QVERIFY(viewport);
    QVERIFY(viewport->setProperty("contentY", 1200.0));
    QVERIFY(fixture.frame());
    const auto savedY = viewport->property("contentY").toDouble();
    const auto savedCursor = fixture.code->property("cursorPosition").toInt();

    fixture.pane->setProperty("text", first + lines("LowIR", 100, 500));
    fixture.pane->setProperty(
        "mappings", QVariantList{anchor(40, AddressA), anchor(550, AddressB)});
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.code->property("cursorPosition").toInt(), savedCursor);
    QVERIFY(std::abs(viewport->property("contentY").toDouble() - savedY) < 1);
  }

  void appendedPageCanFulfillAnUnmappedPendingSelection() {
    TextFixture fixture;
    QVERIFY2(fixture.load(), qPrintable(fixture.error));
    const auto first = lines("LowIR", 500);
    fixture.pane->setProperty("text", first);
    fixture.pane->setProperty("mappings", QVariantList{anchor(20, AddressB)});
    fixture.pane->setProperty("selectedAddress", AddressA);
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 0);
    fixture.pane->setProperty("text", first + lines("LowIR", 150, 500));
    fixture.pane->setProperty(
        "mappings", QVariantList{anchor(20, AddressB), anchor(580, AddressA)});
    QTRY_COMPARE(fixture.cursorLine(), 580);
    QTRY_VERIFY(fixture.cursorVisible());
  }

#ifdef TEST_WORKER
  void workerAndCachedRepliesOnlyPositionTheCurrentRepresentation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.bin");
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    TextFixture fixture;
    QVERIFY2(fixture.load(&controller), qPrintable(fixture.error));
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(controller.selectedAddress(),
                              QString("0xffff800012340000"), 5000);
    // The existing mock engine deliberately holds the initial pipeline for
    // 1800 ms. Queue competing representations while that real worker runs.
    controller.selectInstruction(AddressA);
    controller.setRepresentation("low");
    controller.setRepresentation("med");
    QSignalSpy replies(&controller, &Workbench::externalResponse);
    controller.externalQuery("position-barrier", "metadata", {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 7000);
    QTRY_VERIFY(mappingIdentity(controller).startsWith("med:"));
    QCOMPARE(controller.selectedAddress(), AddressA);
    QTRY_COMPARE(fixture.cursorLine(), 360);
    QTRY_VERIFY(fixture.cursorVisible());

    controller.setRepresentation("low");
    QTRY_VERIFY(mappingIdentity(controller).startsWith("low:"));
    QTRY_COMPARE(fixture.cursorLine(), 360);
    controller.setRepresentation("med"); // Queues an already cached callback.
    controller.setRepresentation("c");   // Supersedes it before event delivery.
    replies.clear();
    controller.externalQuery("cached-position-barrier", "metadata", {}, {});
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 5000);
    QTRY_VERIFY(!controller.representationText().isEmpty());
    QCOMPARE(controller.representation(), QString("c"));
    QVERIFY(controller.textMappings().isEmpty());
    QVERIFY(controller.mappingStatus().contains("unavailable"));
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 0);
  }

  void aNewRevisionDisablesOldAnchorsUntilCurrentMappingsArrive() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.bin");
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    TextFixture fixture;
    QVERIFY2(fixture.load(&controller), qPrintable(fixture.error));
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    controller.setRepresentation("low");
    QTRY_VERIFY(!controller.textMappings().isEmpty());
    controller.selectInstruction(AddressA);
    QTRY_COMPARE(fixture.cursorLine(), 360);
    QTRY_VERIFY(fixture.cursorVisible());
    controller.setComment("invalidate representation mappings");
    QTRY_COMPARE(controller.selectedComment(),
                 QString("invalidate representation mappings"));
    QTRY_VERIFY(controller.textMappings().isEmpty());
    QVERIFY(controller.mappingStatus().contains("earlier revision"));
    controller.selectInstruction(AddressB);
    QVERIFY(fixture.frame());
    QCOMPARE(fixture.cursorLine(), 360);

    controller.setRepresentation("med");
    QTRY_VERIFY(mappingIdentity(controller).startsWith("med:"));
    QCOMPARE(controller.selectedAddress(), AddressB);
    QTRY_COMPARE(fixture.cursorLine(), 361);
    QTRY_VERIFY(fixture.cursorVisible());
  }
#endif
};
QTEST_MAIN(TextPanePositionTests)
#include "TextPanePositionTests.moc"
