#include "PageModel.h"

#include <QEvent>
#include <QEventLoop>
#include <QMetaEnum>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <memory>

namespace {
QString address(int row) {
  return QStringLiteral("0xffff80001234%1").arg(row, 4, 16, QLatin1Char('0'));
}

void drainQueuedCallbacks() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
  QCoreApplication::processEvents(QEventLoop::AllEvents);
}
} // namespace

class PaneNavigationController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QObject *functionsModel READ functionsModel CONSTANT)
  Q_PROPERTY(QObject *instructionsModel READ instructionsModel CONSTANT)
  Q_PROPERTY(QObject *xrefsModel READ xrefsModel CONSTANT)
  Q_PROPERTY(QString selectedAddress READ selectedAddress NOTIFY changed)
  Q_PROPERTY(
      QString selectedFunctionAddress READ selectedAddress NOTIFY changed)
  Q_PROPERTY(
      QString selectedFunctionName READ selectedFunctionName NOTIFY changed)
  Q_PROPERTY(int functionCount READ functionCount CONSTANT)
  Q_PROPERTY(bool loaded READ loaded CONSTANT)
public:
  PageModel functions{{"name", "address", "size"}};
  PageModel instructions{
      {"address", "bytes", "mnemonic", "operands", "comment"}};
  PageModel references{{"from", "to", "kind"}};
  QString selection = address(0);
  QString navigated;
  QString copied;
  QString filter;
  int navigationCount = 0;

  PaneNavigationController() {
    QJsonArray functionRows, instructionRows, referenceRows;
    for (int row = 0; row < 600; ++row) {
      functionRows.append(
          QJsonObject{{"name", QStringLiteral("function_%1").arg(row)},
                      {"address", address(row)},
                      {"size", 16}});
      instructionRows.append(QJsonObject{{"address", address(row)},
                                         {"bytes", "90"},
                                         {"mnemonic", "mov"},
                                         {"operands", "rax, rbx"},
                                         {"comment", "annotation"}});
      referenceRows.append(QJsonObject{
          {"from", address(row)}, {"to", address(row + 1)}, {"kind", "call"}});
    }
    functions.replace(functionRows);
    instructions.replace(instructionRows);
    references.replace(referenceRows);
  }
  QObject *functionsModel() { return &functions; }
  QObject *instructionsModel() { return &instructions; }
  QObject *xrefsModel() { return &references; }
  QString selectedAddress() const { return selection; }
  QString selectedFunctionName() const { return "function_0"; }
  int functionCount() const { return functions.count(); }
  bool loaded() const { return true; }
  Q_INVOKABLE void selectInstruction(const QString &value) {
    if (selection == value)
      return;
    selection = value;
    emit changed();
  }
  Q_INVOKABLE void selectFunction(const QString &value) { navigate(value); }
  Q_INVOKABLE void navigate(const QString &value) {
    navigated = value;
    ++navigationCount;
    selectInstruction(value);
  }
  Q_INVOKABLE void copyText(const QString &value) { copied = value; }
  Q_INVOKABLE void filterFunctions(const QString &value) { filter = value; }
  Q_INVOKABLE void loadMoreInstructions() {}
signals:
  void changed();
};

namespace {
struct PaneFixture {
  QQmlEngine engine;
  PaneNavigationController controller;
  QQuickWindow window;
  std::unique_ptr<QObject> object;
  QQuickItem *pane = nullptr;
  QQuickItem *list = nullptr;
  QString error;

  bool load(const QString &componentName, const QString &listName,
            int width = 720, int height = 260, int fontSize = 11) {
    QQmlComponent component(
        &engine, QUrl::fromLocalFile(QString::fromUtf8(TEST_QML_DIR) + "/" +
                                     componentName + ".qml"));
    QVariantMap properties{{"controller", QVariant::fromValue(&controller)},
                           {"width", width},
                           {"height", height}};
    if (componentName == "DisassemblyPane")
      properties.insert("codePointSize", fontSize);
    object.reset(component.createWithInitialProperties(properties));
    error = component.errorString();
    pane = qobject_cast<QQuickItem *>(object.get());
    if (!pane)
      return false;
    list = pane->findChild<QQuickItem *>(listName);
    if (!list) {
      error = "Production component did not create its list";
      return false;
    }
    window.resize(width, height);
    pane->setParentItem(window.contentItem());
    window.show();
    if (!QTest::qWaitForWindowExposed(&window)) {
      error = "Test window was not exposed";
      return false;
    }
    return true;
  }
  QQuickItem *current() const {
    return list->property("currentItem").value<QQuickItem *>();
  }
  int index() const { return list->property("currentIndex").toInt(); }
  bool currentRowVisible() const {
    const auto *item = current();
    if (!item)
      return false;
    const auto viewport = list->boundingRect().adjusted(-1, -1, 1, 1);
    const auto row = item->mapRectToItem(list, item->boundingRect());
    // Disassembly rows can be wider than a dock and scroll horizontally.
    // Keyboard navigation must reveal the complete row vertically.
    return row.width() > 0 && row.height() > 0 && viewport.intersects(row) &&
           row.top() >= viewport.top() && row.bottom() <= viewport.bottom();
  }
  QString rowVisibilityDetails() const {
    const auto *item = current();
    const auto row =
        item ? item->mapRectToItem(list, item->boundingRect()) : QRectF{};
    return QStringLiteral("index=%1 viewport=%2x%3 row=(%4,%5 %6x%7) "
                          "content=(%8,%9)")
        .arg(index())
        .arg(list->width())
        .arg(list->height())
        .arg(row.x())
        .arg(row.y())
        .arg(row.width())
        .arg(row.height())
        .arg(list->property("contentX").toDouble())
        .arg(list->property("contentY").toDouble());
  }
  void focusList() { list->forceActiveFocus(Qt::TabFocusReason); }
};

QQuickItem *textItem(QQuickItem *parent, const QString &text) {
  for (auto *child : parent->findChildren<QQuickItem *>())
    if (child->property("text").toString() == text &&
        child->metaObject()->indexOfProperty("contentWidth") >= 0)
      return child;
  return nullptr;
}
} // namespace

class PaneNavigationTests final : public QObject {
  Q_OBJECT
private slots:
  void disassemblyKeyboardSelectsActivatesAndCopies() {
    PaneFixture fixture;
    QVERIFY2(fixture.load("DisassemblyPane", "disassemblyList"),
             qPrintable(fixture.error));
    fixture.focusList();
    QTest::keyClick(&fixture.window, Qt::Key_Down);
    QTRY_COMPARE(fixture.controller.selection, address(1));
    QVERIFY(fixture.current()->property("highlighted").toBool());
    QTest::keyClick(&fixture.window, Qt::Key_PageDown);
    QVERIFY(fixture.index() > 1);
    QCOMPARE(fixture.controller.selection, address(fixture.index()));
    QTest::keyClick(&fixture.window, Qt::Key_End);
    QTRY_COMPARE(fixture.index(), 599);
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));
    QTest::keyClick(&fixture.window, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(fixture.controller.copied,
             address(599) + "  mov rax, rbx ; annotation");
    QTest::keyClick(&fixture.window, Qt::Key_Return);
    QCOMPARE(fixture.controller.navigated, address(599));
    QCOMPARE(fixture.controller.navigationCount, 1);
    QTest::keyClick(&fixture.window, Qt::Key_Home);
    QTRY_COMPARE(fixture.controller.selection, address(0));
  }

  void appendedInstructionsPreserveTheReadingPosition() {
    PaneFixture fixture;
    QVERIFY2(fixture.load("DisassemblyPane", "disassemblyList"),
             qPrintable(fixture.error));
    fixture.focusList();
    QTest::keyClick(&fixture.window, Qt::Key_Home);
    drainQueuedCallbacks();
    QCOMPARE(fixture.index(), 0);
    QCOMPARE(fixture.controller.selection, address(0));
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));

    const auto positionMode = fixture.list->metaObject()->enumerator(
        fixture.list->metaObject()->indexOfEnumerator("PositionMode"));
    QVERIFY(positionMode.isValid());
    QVERIFY(QMetaObject::invokeMethod(
        fixture.list, "positionViewAtIndex", Q_ARG(int, 599),
        Q_ARG(int, positionMode.keyToValue("End"))));
    drainQueuedCallbacks();
    const auto readingY = fixture.list->property("contentY").toDouble();
    QVERIFY(readingY > fixture.list->height());
    QVERIFY(!fixture.currentRowVisible());

    QJsonArray nextPage;
    for (int row = 600; row < 640; ++row)
      nextPage.append(QJsonObject{{"address", address(row)},
                                  {"bytes", "90"},
                                  {"mnemonic", "nop"},
                                  {"operands", ""},
                                  {"comment", ""}});
    fixture.controller.instructions.append(nextPage);
    drainQueuedCallbacks();
    QVERIFY(QMetaObject::invokeMethod(fixture.list, "forceLayout"));
    QCOMPARE(fixture.list->property("contentY").toDouble(), readingY);
    QCOMPARE(fixture.index(), 0);
    QCOMPARE(fixture.controller.selection, address(0));
  }

  void externalInstructionSelectionIsRevealedAfterAppendAndReplacement() {
    PaneFixture fixture;
    QJsonArray firstPage, nextPage;
    for (int row = 0; row < 512; ++row) {
      const auto instruction =
          QJsonObject::fromVariantMap(fixture.controller.instructions.get(row));
      (row < 256 ? firstPage : nextPage).append(instruction);
    }
    fixture.controller.instructions.replace(firstPage);
    QVERIFY2(fixture.load("DisassemblyPane", "disassemblyList"),
             qPrintable(fixture.error));
    fixture.controller.selectInstruction(address(500));
    drainQueuedCallbacks();
    QCOMPARE(fixture.index(), 0);
    fixture.controller.instructions.append(nextPage);
    QTRY_COMPARE(fixture.index(), 500);
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));
    QJsonArray replacement;
    for (int row = 480; row < 520; ++row)
      replacement.append(QJsonObject{{"address", address(row)},
                                     {"bytes", "90"},
                                     {"mnemonic", "nop"},
                                     {"operands", ""},
                                     {"comment", ""}});
    fixture.controller.instructions.replace(replacement);
    QTRY_COMPARE(fixture.index(), 20);
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));
    QCOMPARE(fixture.controller.selection, address(500));
  }

  void functionSearchHandsFocusToVisibleKeyboardSelection() {
    PaneFixture fixture;
    QVERIFY2(fixture.load("FunctionsPane", "functionsList", 300, 500),
             qPrintable(fixture.error));
    QSignalSpy navigation(fixture.pane, SIGNAL(navigationRequested(QString)));
    QVERIFY(navigation.isValid());
    auto *search = fixture.pane->findChild<QQuickItem *>("functionSearch");
    QVERIFY(search);
    search->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(&fixture.window, Qt::Key_Down);
    QTRY_VERIFY(fixture.list->hasActiveFocus());
    QTest::keyClick(&fixture.window, Qt::Key_Down);
    QTRY_COMPARE(fixture.index(), 1);
    QVERIFY(fixture.current()->property("highlighted").toBool());
    QTest::keyClick(&fixture.window, Qt::Key_Return);
    QCOMPARE(navigation.count(), 1);
    QCOMPARE(navigation.first().at(0).toString(), address(1));
  }

  void referencesActivateCurrentRowWithKeyboard() {
    PaneFixture fixture;
    QVERIFY2(fixture.load("ReferencesPane", "referencesList"),
             qPrintable(fixture.error));
    fixture.focusList();
    QTest::keyClick(&fixture.window, Qt::Key_Down);
    QTRY_COMPARE(fixture.index(), 1);
    QVERIFY(fixture.current()->property("highlighted").toBool());
    QTest::keyClick(&fixture.window, Qt::Key_Return);
    QCOMPARE(fixture.controller.navigated, address(1));
    QCOMPARE(fixture.controller.navigationCount, 1);
  }

  void functionPageArrivalPreservesKeyboardCurrentRow() {
    PaneFixture fixture;
    QJsonArray firstPage, nextPage;
    for (int row = 0; row < 256; ++row) {
      firstPage.append(
          QJsonObject::fromVariantMap(fixture.controller.functions.get(row)));
      nextPage.append(QJsonObject::fromVariantMap(
          fixture.controller.functions.get(row + 256)));
    }
    fixture.controller.functions.resetPages();
    QVERIFY(fixture.controller.functions.setPage(0, 1000000, firstPage));
    QVERIFY2(fixture.load("FunctionsPane", "functionsList", 300, 500),
             qPrintable(fixture.error));
    fixture.focusList();
    QTest::keyClick(&fixture.window, Qt::Key_Down);
    QCOMPARE(fixture.index(), 1);
    QVERIFY(fixture.controller.functions.setPage(256, 1000000, nextPage));
    // Let the model's queued selection synchronization run before checking.
    QCoreApplication::processEvents();
    QCOMPARE(fixture.index(), 1);
    QCOMPARE(fixture.controller.selection, address(0));
  }

  void largeDisassemblyFontPreservesColumnBoundaries() {
    PaneFixture fixture;
    QVERIFY2(fixture.load("DisassemblyPane", "disassemblyList", 300, 240, 20),
             qPrintable(fixture.error));
    QTRY_VERIFY(fixture.current());
    auto *row = fixture.current();
    auto *location = textItem(row, address(0).mid(2));
    auto *mnemonic = textItem(row, "mov");
    auto *operands = textItem(row, "rax, rbx");
    QVERIFY(location && mnemonic && operands);
    QVERIFY2(location->width() >= location->property("contentWidth").toDouble(),
             "The complete 64-bit address must fit its column at 20pt");
    QVERIFY(location->mapToItem(row, QPointF(location->width(), 0)).x() <=
            mnemonic->mapToItem(row, QPointF()).x());
    QVERIFY(mnemonic->mapToItem(row, QPointF(mnemonic->width(), 0)).x() <=
            operands->mapToItem(row, QPointF()).x());
    for (auto *item : {location, mnemonic, operands})
      QVERIFY(row->boundingRect().contains(
          item->mapRectToItem(row, item->boundingRect())));
    QVERIFY(fixture.list->property("contentWidth").toDouble() >
            fixture.list->width());
    QVERIFY(row->width() > fixture.list->width());
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));
    fixture.focusList();
    QTest::keyClick(&fixture.window, Qt::Key_End);
    QTRY_COMPARE(fixture.index(), 599);
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));
    QTest::keyClick(&fixture.window, Qt::Key_Home);
    QTRY_COMPARE(fixture.index(), 0);
    QTRY_VERIFY2(fixture.currentRowVisible(),
                 qPrintable(fixture.rowVisibilityDetails()));
  }

  void narrowReferencesKeepBothAddressesInsideRow() {
    PaneFixture fixture;
    QVERIFY2(fixture.load("ReferencesPane", "referencesList", 280, 240),
             qPrintable(fixture.error));
    QTRY_VERIFY(fixture.current());
    auto *row = fixture.current();
    auto *from = textItem(row, address(0));
    auto *to = textItem(row, address(1));
    QVERIFY(from && to);
    for (auto *item : {from, to}) {
      QVERIFY2(row->boundingRect().contains(
                   item->mapRectToItem(row, item->boundingRect())),
               "Reference addresses must remain inside a 280px dock");
      QVERIFY(item->width() >= item->property("contentWidth").toDouble());
    }
    QVERIFY(to->mapToItem(row, QPointF()).y() >
            from->mapToItem(row, QPointF()).y());
  }
};

QTEST_MAIN(PaneNavigationTests)
#include "PaneNavigationTests.moc"
