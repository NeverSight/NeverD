#include "ShortcutProbe.h"

#include "Workbench.h"

#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QJSValue>
#include <QKeySequence>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {
class ShortcutTests final : public QObject {
  Q_OBJECT
public:
  ShortcutTests(QQmlApplicationEngine &engine, Workbench &workbench)
      : workbench_(workbench),
        window_(qobject_cast<QQuickWindow *>(engine.rootObjects().value(0))) {}

private:
  Workbench &workbench_;
  QQuickWindow *window_;
  QObject *workspace_ = nullptr;
  QString focused() const {
    auto *active = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow());
    auto *item = active ? active->activeFocusItem() : nullptr;
    // ListView is a focus scope: any of its current delegates may own
    // active focus, including unnamed Functions and References delegates.
    for (auto *cursor = item; cursor; cursor = cursor->parentItem()) {
      const auto name = cursor->objectName();
      if (name == "disassemblyList" || name == "functionsList" ||
          name == "referencesList")
        return name;
    }
    return item ? item->objectName() : QString{};
  }
  QVariantMap commandTarget(QObject *dialog) const {
    const auto value = dialog->property("commandTarget");
    return value.metaType() == QMetaType::fromType<QJSValue>()
               ? value.value<QJSValue>().toVariant().toMap()
               : value.toMap();
  }
  bool show(const QString &panel) {
    return QMetaObject::invokeMethod(workspace_, "showPanel",
                                     Q_ARG(QVariant, panel));
  }
  void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    auto *active = qobject_cast<QQuickWindow *>(QGuiApplication::focusWindow());
    QTest::keyClick(active ? active : window_, key, modifiers);
  }

private slots:
  void readingShortcutsPreserveEditingAndRestoreLayout() {
    QVERIFY(window_);
    window_->requestActivate();
    QTemporaryDir directory;
    QFile fixture(directory.filePath("shortcuts.bin"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("fixture");
    fixture.close();
    workbench_.openFile(QUrl::fromLocalFile(fixture.fileName()));
    QTRY_VERIFY_WITH_TIMEOUT(!workbench_.representationText().isEmpty(), 7000);
    workspace_ = window_->findChild<QObject *>("dockWorkspace");
    auto *machine = window_->findChild<QQuickItem *>("machinePane");
    auto *machineDock = window_->findChild<QObject *>("machineDock");
    auto *panes = workbench_.paneRegistry();
    auto *rename = window_->findChild<QObject *>("renameDialog");
    auto *comment = window_->findChild<QObject *>("commentDialog");
    auto *settings = window_->findChild<QObject *>("settingsDialog");
    auto *settingsAction = window_->findChild<QObject *>("settingsAction");
    auto *address = window_->findChild<QQuickItem *>("addressField");
    auto *representation = window_->findChild<QObject *>("representationDock");
    QVERIFY(workspace_ && machine && machineDock && panes && rename &&
            comment && settings && settingsAction && address && representation);

    QVERIFY(show("representation"));
    QTRY_COMPARE(focused(), QString("codeText"));
    QVERIFY(!window_->property("textEntryActive").toBool());
    key(Qt::Key_G);
    QTRY_COMPARE(focused(), QString("addressField"));
    key(Qt::Key_N);
    key(Qt::Key_Semicolon);
    QVERIFY(!rename->property("visible").toBool());
    QVERIFY(!comment->property("visible").toBool());
    QVERIFY(address->property("text").toString().contains("n;"));
    key(Qt::Key_Escape);
    QTRY_COMPARE(focused(), QString("disassemblyList"));

    key(Qt::Key_Space);
    QTRY_COMPARE(machine->property("currentIndex").toInt(), 1);
    QTRY_COMPARE(focused(), QString("graphViewport"));
    key(Qt::Key_Space);
    QTRY_COMPARE(machine->property("currentIndex").toInt(), 0);
    QTRY_COMPARE(focused(), QString("disassemblyList"));
    key(Qt::Key_F5);
    QTRY_COMPARE(focused(), QString("codeText"));
    QCOMPARE(workbench_.representation(), QString("c"));
    key(Qt::Key_Tab);
    QTRY_COMPARE(focused(), QString("disassemblyList"));
    key(Qt::Key_Tab);
    QTRY_COMPARE(focused(), QString("codeText"));
    key(Qt::Key_X);
    auto *references = window_->findChild<QQuickItem *>("referencesList");
    QVERIFY(references);
    QTRY_VERIFY(references->hasActiveFocus());
    QTRY_COMPARE(focused(), QString("referencesList"));
    key(Qt::Key_P, Qt::ControlModifier);
    QTRY_COMPARE(focused(), QString("functionSearch"));
    key(Qt::Key_N);
    QVERIFY(!rename->property("visible").toBool());

    QVERIFY(show("representation"));
    QTRY_COMPARE(focused(), QString("codeText"));
    key(Qt::Key_N);
    QTRY_VERIFY(rename->property("visible").toBool());
    QVERIFY(window_->property("modalActive").toBool());
    key(Qt::Key_F5);
    QVERIFY(rename->property("visible").toBool());
    QVERIFY(window_->property("textEntryActive").toBool());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!rename->property("visible").toBool());

    // Both layout changes happen in one event turn. The first change's queued
    // focus must not steal an explicit field focus after the layout is
    // restored.
    QCOMPARE(QGuiApplication::focusWindow(), window_);
    QVERIFY(QMetaObject::invokeMethod(workspace_, "focusPanel",
                                      Q_ARG(QVariant, QString("machine"))));
    QVERIFY(!representation->property("isOpen").toBool());
    QVERIFY(QMetaObject::invokeMethod(workspace_, "focusPanel",
                                      Q_ARG(QVariant, QString("machine"))));
    address->forceActiveFocus(Qt::OtherFocusReason);
    QCOMPARE(focused(), QString("addressField"));
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCOMPARE(focused(), QString("addressField"));
    QTRY_VERIFY(representation->property("isOpen").toBool());
    QVERIFY(workspace_->property("focusedPanel").toString().isEmpty());
    QVERIFY(show("representation"));
    QTRY_COMPARE(focused(), QString("codeText"));

    // The real dock frontend creates a second native QQuickWindow. Commands
    // must use its focus, and main-window dialogs and fields must activate it.
    auto *codePane = window_->findChild<QQuickItem *>("representationPane");
    QVERIFY(codePane);
    QVERIFY(representation->setProperty("isFloating", true));
    QTRY_VERIFY(representation->property("isFloating").toBool());
    QTRY_VERIFY(codePane->window() && codePane->window() != window_);
    auto *floating = codePane->window();
    QCOMPARE(floating->transientParent(), window_);
    QTRY_VERIFY(floating->isVisible());
    floating->requestActivate();
    QTRY_COMPARE(QGuiApplication::focusWindow(), floating);
    QVERIFY(QMetaObject::invokeMethod(codePane, "focusContent"));
    QTRY_COMPARE(focused(), QString("codeText"));
    QTRY_VERIFY(workspace_->property("representationActive").toBool());
    QVERIFY(!window_->property("textEntryActive").toBool());
    const auto preferences =
        QKeySequence::keyBindings(QKeySequence::Preferences);
    if (!preferences.isEmpty())
      QTest::keySequence(floating, preferences.first());
    else
      QVERIFY(QMetaObject::invokeMethod(settingsAction, "trigger"));
    QTRY_VERIFY(settings->property("visible").toBool());
    QTRY_COMPARE(QGuiApplication::focusWindow(), window_);
    QVERIFY(window_->property("modalActive").toBool());
    key(Qt::Key_Escape);
    QTRY_VERIFY(!settings->property("visible").toBool());

    QTRY_VERIFY(floating->isVisible());
    floating->requestActivate();
    QTRY_COMPARE(QGuiApplication::focusWindow(), floating);
    QVERIFY(QMetaObject::invokeMethod(codePane, "focusContent"));
    QTRY_COMPARE(focused(), QString("codeText"));
    QTRY_VERIFY(workspace_->property("representationActive").toBool());
    QVERIFY(!window_->property("textEntryActive").toBool());
    key(Qt::Key_G);
    QTRY_COMPARE(focused(), QString("addressField"));
    QCOMPARE(QGuiApplication::focusWindow(), window_);
    QVERIFY(representation->setProperty("isFloating", false));
    QTRY_VERIFY(!representation->property("isFloating").toBool());

    // History belongs to the focused pane. Establish both entries on the
    // machine pane before exercising the platform and IDA shortcuts.
    QVERIFY(show("machine"));
    QTRY_COMPARE(focused(), QString("disassemblyList"));
    QTRY_COMPARE(panes->activePaneId(), panes->defaultMachine()->id());
    workbench_.navigate("function_20");
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340140"));
    workbench_.navigate("function_21");
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340150"));
    QVERIFY(show("machine"));
    QTRY_COMPARE(focused(), QString("disassemblyList"));
    key(Qt::Key_Escape);
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340140"));
    key(Qt::Key_Return, Qt::ControlModifier);
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340150"));

    // Address submission must reopen the last active default analysis pane
    // before navigation, even when neither analysis dock currently exists on
    // screen. An eventual focus callback cannot admit this request for it.
    QTRY_COMPARE(panes->activePaneId(), panes->defaultMachine()->id());
    QVERIFY(QMetaObject::invokeMethod(representation, "close"));
    QVERIFY(QMetaObject::invokeMethod(machineDock, "close"));
    QTRY_VERIFY(!representation->property("isOpen").toBool());
    QTRY_VERIFY(!machineDock->property("isOpen").toBool());
    QTRY_VERIFY(!panes->defaultRepresentation()->open());
    QTRY_VERIFY(!panes->defaultMachine()->open());
    QVERIFY(!panes->activePane());
    QVERIFY(QMetaObject::invokeMethod(window_, "openRename"));
    QVERIFY(QMetaObject::invokeMethod(window_, "openComment"));
    QVERIFY(!rename->property("visible").toBool());
    QVERIFY(!comment->property("visible").toBool());
    address->forceActiveFocus(Qt::OtherFocusReason);
    QTRY_COMPARE(focused(), QString("addressField"));
    QVERIFY(address->setProperty("text", QString("function_22")));
    key(Qt::Key_Return);
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340160"));
    QTRY_VERIFY(machineDock->property("isOpen").toBool());
    QTRY_VERIFY(panes->defaultMachine()->open());
    QTRY_VERIFY(panes->defaultMachine()->contentVisible());
    QCOMPARE(panes->activePaneId(), panes->defaultMachine()->id());

    // Dialogs snapshot their target before transferring focus. A later
    // navigation must not silently change the annotation destination.
    QVERIFY(QMetaObject::invokeMethod(window_, "openRename"));
    QTRY_VERIFY(rename->property("visible").toBool());
    const auto target = commandTarget(rename);
    QCOMPARE(target.value("address").toString(), QString("0xffff800012340160"));
    QCOMPARE(target.value("function_address").toString(),
             QString("0xffff800012340160"));
    QVERIFY(!target.value("project_id").toString().isEmpty());
    QVERIFY(!target.value("session_epoch").toString().isEmpty());
    workbench_.navigate("function_23");
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340170"));
    QVERIFY(rename->property("visible").toBool());
    QCOMPARE(commandTarget(rename), target);
    QVERIFY(QMetaObject::invokeMethod(rename, "reject"));
    QTRY_VERIFY(!rename->property("visible").toBool());

    // Function activation uses the same open-before-navigation route while
    // retaining the browser's keyboard focus for subsequent selections.
    QVERIFY(QMetaObject::invokeMethod(machineDock, "close"));
    QTRY_VERIFY(!panes->defaultMachine()->open());
    QVERIFY(!panes->defaultRepresentation()->open());
    QVERIFY(QMetaObject::invokeMethod(workspace_, "focusFunctionSearch"));
    QTRY_COMPARE(focused(), QString("functionSearch"));
    key(Qt::Key_Backspace);
    auto *functionsModel =
        qobject_cast<PageModel *>(workbench_.functionsModel());
    QVERIFY(functionsModel);
    QTRY_COMPARE(functionsModel->get(0).value("address").toString(),
                 QString("0xffff800012340000"));
    key(Qt::Key_Down);
    QTRY_COMPARE(focused(), QString("functionsList"));
    key(Qt::Key_Home);
    key(Qt::Key_Return);
    QTRY_COMPARE(workbench_.selectedAddress(), QString("0xffff800012340000"));
    QTRY_VERIFY(machineDock->property("isOpen").toBool());
    QTRY_VERIFY(panes->defaultMachine()->open());
    QTRY_VERIFY(panes->defaultMachine()->contentVisible());
    QTRY_COMPARE(focused(), QString("functionsList"));
    QCOMPARE(panes->activePaneId(), panes->defaultMachine()->id());
  }
};
} // namespace

void startShortcutProbe(QQmlApplicationEngine &engine, Workbench &workbench) {
  QTimer::singleShot(0, &engine, [&engine, &workbench] {
    ShortcutTests tests(engine, workbench);
    const int result =
        QTest::qExec(&tests, QStringList{"neverd-gui-shortcuts"});
    QCoreApplication::exit(result);
  });
}

#include "ShortcutProbe.moc"
