#include "WorkbenchFocus.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QKeySequence>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QSignalSpy>
#include <QTest>
#include <memory>

class WorkbenchFocusTests final : public QObject {
  Q_OBJECT
private slots:
  void textShortcutsPreserveEditingAndSelection_data() {
    QTest::addColumn<bool>("readOnly");
    QTest::addColumn<bool>("floating");
    QTest::newRow("read-only-main") << true << false;
    QTest::newRow("read-only-floating") << true << true;
    QTest::newRow("editable-main") << false << false;
    QTest::newRow("editable-floating") << false << true;
  }

  void textShortcutsPreserveEditingAndSelection() {
    QFETCH(bool, readOnly);
    QFETCH(bool, floating);
    WorkbenchFocus focus;
    QQmlEngine engine;
    QQuickWindow main;
    QQuickWindow auxiliary;
    auxiliary.setTransientParent(&main);
    main.resize(320, 200);
    auxiliary.resize(320, 200);
    QQmlComponent component(&engine);
    component.setData(R"(
      import QtQuick
      import QtQuick.Controls.Basic
      Item {
        id: root
        width: 320; height: 200
        property bool readOnly: true
        property bool overrideG: false
        property int hits: 0
        Shortcut {
          sequence: "G"
          onActivated: root.hits += 1
        }
        TextArea {
          id: editor
          objectName: "shortcutEditor"
          anchors.fill: parent
          text: "alpha beta"
          readOnly: root.readOnly
          selectByKeyboard: true
          Keys.onShortcutOverride: event => {
            if (root.overrideG && event.key === Qt.Key_G)
              event.accepted = true
          }
        }
      }
    )",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object != nullptr, qPrintable(component.errorString()));
    auto *item = qobject_cast<QQuickItem *>(object.get());
    auto *editor = object->findChild<QQuickItem *>("shortcutEditor");
    QVERIFY(item && editor);
    QVERIFY(object->setProperty("readOnly", readOnly));
    auto *window = floating ? &auxiliary : &main;
    item->setParentItem(window->contentItem());
    main.show();
    QVERIFY(QTest::qWaitForWindowExposed(&main));
    if (floating) {
      auxiliary.show();
      QVERIFY(QTest::qWaitForWindowExposed(&auxiliary));
    }
    window->requestActivate();
    editor->forceActiveFocus();
    QTRY_COMPARE(focus.focusWindow(), window);
    QTRY_COMPARE(focus.focusedItem(), editor);
    QTest::keyClick(window, Qt::Key_G);
    QCOMPARE(object->property("hits").toInt(), readOnly ? 1 : 0);
    if (readOnly)
      QCOMPARE(editor->property("text").toString(), QString("alpha beta"));
    else {
      QCOMPARE(editor->property("text").toString().size(), 11);
      QVERIFY(editor->property("text").toString().contains('g'));
    }

    if (readOnly) {
      // Explicit QML overrides still win over the application shortcut.
      QVERIFY(object->setProperty("overrideG", true));
      QTest::keyClick(window, Qt::Key_G);
      QCOMPARE(object->property("hits").toInt(), 1);
      QCOMPARE(editor->property("text").toString(), QString("alpha beta"));
    }

    QTest::keySequence(window, QKeySequence(QKeySequence::SelectAll));
    QCOMPARE(editor->property("selectedText").toString(),
             editor->property("text").toString());
    // Preserve all clipboard formats, including when an assertion returns.
    struct RestoreClipboard {
      QClipboard *clipboard;
      std::unique_ptr<QMimeData> saved = std::make_unique<QMimeData>();
      explicit RestoreClipboard(QClipboard *value) : clipboard(value) {
        if (const auto *original = clipboard->mimeData())
          for (const auto &format : original->formats())
            saved->setData(format, original->data(format));
      }
      ~RestoreClipboard() { clipboard->setMimeData(saved.release()); }
    } clipboard(QGuiApplication::clipboard());
    QTest::keySequence(window, QKeySequence(QKeySequence::Copy));
    QCOMPARE(clipboard.clipboard->text(), editor->property("text").toString());
    QCOMPARE(focus.focusedItem(), editor);
  }

  void followsActiveContentAcrossIndependentWindows() {
    WorkbenchFocus focus;
    QQuickWindow main;
    QQuickWindow floating;
    main.resize(320, 200);
    floating.resize(240, 160);
    QQuickItem mainContent(main.contentItem());
    QQuickItem floatingContent(floating.contentItem());
    QQuickItem floatingField(floating.contentItem());
    main.show();
    floating.show();
    QVERIFY(QTest::qWaitForWindowExposed(&main));
    QVERIFY(QTest::qWaitForWindowExposed(&floating));

    main.requestActivate();
    mainContent.forceActiveFocus();
    QTRY_COMPARE(focus.focusWindow(), &main);
    QTRY_COMPARE(focus.focusedItem(), &mainContent);

    floating.requestActivate();
    floatingContent.forceActiveFocus();
    QTRY_COMPARE(focus.focusWindow(), &floating);
    QTRY_COMPARE(focus.focusedItem(), &floatingContent);
    QVERIFY(main.activeFocusItem() != focus.focusedItem());

    floatingField.forceActiveFocus();
    QTRY_COMPARE(focus.focusedItem(), &floatingField);
    // Focus changes in an inactive window cannot override active pane routing.
    mainContent.forceActiveFocus();
    QCOMPARE(focus.focusWindow(), &floating);
    QCOMPARE(focus.focusedItem(), &floatingField);

    main.requestActivate();
    QTRY_COMPARE(focus.focusWindow(), &main);
    QTRY_COMPARE(focus.focusedItem(), &mainContent);
  }

  void retiringItemsAndWindowsNeverLeaveDanglingFocus() {
    WorkbenchFocus focus;
    QSignalSpy changed(&focus, &WorkbenchFocus::focusedItemChanged);
    auto window = std::make_unique<QQuickWindow>();
    window->resize(320, 200);
    auto item = std::make_unique<QQuickItem>(window->contentItem());
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.get()));
    window->requestActivate();
    item->forceActiveFocus();
    QTRY_COMPARE(focus.focusedItem(), item.get());
    changed.clear();
    item.reset();
    QVERIFY(!changed.isEmpty());
    QCOMPARE(focus.focusedItem(), window->activeFocusItem());
    window.reset();
    QTRY_COMPARE(focus.focusWindow(), nullptr);
    QTRY_COMPARE(focus.focusedItem(), nullptr);
  }

  void nativeWindowsClearQuickContentRouting() {
    WorkbenchFocus focus;
    QQuickWindow quick;
    QWindow native;
    quick.resize(320, 200);
    native.resize(240, 160);
    QQuickItem content(quick.contentItem());
    quick.show();
    native.show();
    QVERIFY(QTest::qWaitForWindowExposed(&quick));
    QVERIFY(QTest::qWaitForWindowExposed(&native));
    quick.requestActivate();
    content.forceActiveFocus();
    QTRY_COMPARE(focus.focusedItem(), &content);
    native.requestActivate();
    QTRY_COMPARE(QGuiApplication::focusWindow(), &native);
    QCOMPARE(focus.focusWindow(), nullptr);
    QCOMPARE(focus.focusedItem(), nullptr);
  }
};

QTEST_MAIN(WorkbenchFocusTests)
#include "WorkbenchFocusTests.moc"
