#include "ShortcutProbe.h"

#include "Workbench.h"

#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QEvent>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJSValue>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
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
  void emptyWorkbenchUsesDarkSurfaces() {
    QVERIFY(window_);
    QVERIFY(!workbench_.loaded());
    workspace_ = window_->findChild<QObject *>("dockWorkspace");
    auto *dock = window_->findChild<QObject *>("connectionsDock");
    auto *manager = window_->findChild<QObject *>("connectionManager");
    auto *settings = window_->findChild<QObject *>("settingsDialog");
    auto *settingsAction = window_->findChild<QObject *>("settingsAction");
    auto *language = window_->findChild<QQuickItem *>("languageSelector");
    auto *connections =
        manager ? qobject_cast<QQuickItem *>(manager->parent()) : nullptr;
    QVERIFY(workspace_ && dock && manager && settings && settingsAction &&
            language && connections);
    QVERIFY(connections->property("client").isValid());
    QPointer<QObject> transportPopup, languagePopup;
    QList<QPointer<QQuickWindow>> pointerWindows;
    const auto originalLanguage = workbench_.language();
    bool restored = false;
    auto restore = [&] {
      if (restored)
        return;
      restored = true;
      for (const auto &host : pointerWindows) {
        if (!host)
          continue;
        QEvent leave(QEvent::Leave);
        QCoreApplication::sendEvent(host.data(), &leave);
      }
      for (auto *popup :
           {transportPopup.data(), languagePopup.data(), manager, settings})
        if (popup)
          QMetaObject::invokeMethod(popup, "close");
      dock->setProperty("isFloating", false);
      QMetaObject::invokeMethod(workspace_, "resetLayout");
      show("references");
      workbench_.setLanguage(originalLanguage);
    };
    const auto cleanup = qScopeGuard(restore);
    workbench_.setLanguage("en");
    auto findControl = [](QObject *owner, const char *type,
                          const char *property, const QString &text) {
      for (auto *item : owner->findChildren<QQuickItem *>())
        if (item->inherits(type) && item->property(property).toString() == text)
          return item;
      return static_cast<QQuickItem *>(nullptr);
    };
    auto objectProperty = [](QObject *owner, const char *name) {
      return owner->property(name).value<QObject *>();
    };
    QStringList failures;
    QImage shot;
    QQuickWindow *shotWindow = nullptr;
    const auto captureDirectory =
        qEnvironmentVariable("NEVERD_GUI_THEME_CAPTURE_DIR");
    auto capture = [&](QQuickItem *item, const QString &name) {
      auto *host = item ? item->window() : nullptr;
      if (!host || !host->isVisible() || !item->isVisible() ||
          item->width() < 8 || item->height() < 8)
        return false;
      QSignalSpy frame(host, &QQuickWindow::frameSwapped);
      host->update();
      if (frame.isEmpty() && !frame.wait(2000))
        return false;
      shot = host->grabWindow();
      shotWindow = host;
      if (shot.isNull())
        return false;
      if (!captureDirectory.isEmpty()) {
        const auto path = QDir(captureDirectory).filePath(name + ".png");
        if (!QDir().mkpath(captureDirectory) || QFileInfo::exists(path) ||
            !shot.save(path))
          return false;
      }
      return true;
    };
    auto pixels = [&](QQuickItem *item, const QRectF &local,
                      const QString &name, bool neutral,
                      QColor *average = nullptr) {
      if (!item || item->window() != shotWindow ||
          !item->boundingRect().contains(local))
        return false;
      const auto scene = item->mapRectToScene(local);
      const double sx = double(shot.width()) / shotWindow->width();
      const double sy = double(shot.height()) / shotWindow->height();
      const auto roi = QRectF(scene.x() * sx, scene.y() * sy,
                              scene.width() * sx, scene.height() * sy)
                           .toAlignedRect();
      if (roi.width() < 2 || roi.height() < 2 || !shot.rect().contains(roi))
        return false;
      qint64 red = 0, green = 0, blue = 0, white = 0;
      for (int y = roi.top(); y <= roi.bottom(); ++y)
        for (int x = roi.left(); x <= roi.right(); ++x) {
          const auto color = shot.pixelColor(x, y);
          red += color.red();
          green += color.green();
          blue += color.blue();
          white +=
              color.red() > 205 && color.green() > 205 && color.blue() > 205;
        }
      const auto count = qint64(roi.width()) * roi.height();
      const QColor mean(int(red / count), int(green / count),
                        int(blue / count));
      if (average)
        *average = mean;
      const int high = qMax(mean.red(), qMax(mean.green(), mean.blue()));
      const int low = qMin(mean.red(), qMin(mean.green(), mean.blue()));
      const auto detail = QString("%1: mean=%2 white=%3/%4 ROI=%5,%6 %7x%8")
                              .arg(name, mean.name())
                              .arg(white)
                              .arg(count)
                              .arg(roi.x())
                              .arg(roi.y())
                              .arg(roi.width())
                              .arg(roi.height());
      qInfo().noquote() << detail;
      if (high > 115 || white * 10 > count || (neutral && high - low > 20))
        failures << detail;
      return true;
    };
    auto panePixels = [&](const QString &name) {
      return pixels(
          connections,
          QRectF(12, connections->height() - 14, connections->width() - 24, 6),
          name, true);
    };
    auto popupPixels = [&](QObject *popup, const QString &name) {
      auto *background =
          qobject_cast<QQuickItem *>(objectProperty(popup, "background"));
      if (!capture(background, name) ||
          !pixels(background, QRectF(4, 12, 3, background->height() - 24), name,
                  false))
        return false;
      auto *footer =
          qobject_cast<QQuickItem *>(objectProperty(popup, "footer"));
      return !footer || !footer->isVisible() ||
             pixels(footer, QRectF(4, 3, footer->width() - 8, 3),
                    name + "-footer", false);
    };

    QVERIFY(show("connections"));
    QTRY_VERIFY(connections->isVisible() && connections->window() == window_);
    QQuickItem *open = nullptr;
    QTRY_VERIFY(
        (open = findControl(window_, "QQuickButton", "text", "Open Binary")));
    QSignalSpy hoverChanges(open, SIGNAL(hoveredChanged()));
    QVERIFY(hoverChanges.isValid());
    bool isolateExternalPointer = false;
    class HoverTrace final : public QObject {
    public:
      HoverTrace(QQuickItem *button, const QSignalSpy &changes,
                 const bool &isolateExternal)
          : button_(button), changes_(changes),
            isolateExternal_(isolateExternal) {}
      bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
            event->type() == QEvent::MouseMove)
          qInfo() << "theme before delivery" << event->type() << "hovered"
                  << button_->property("hovered") << "changes"
                  << changes_.size() << "cursor" << QCursor::pos()
                  << "spontaneous" << event->spontaneous();
        if (isolateExternal_ && event->spontaneous() &&
            (event->type() == QEvent::MouseMove ||
             event->type() == QEvent::Enter ||
             event->type() == QEvent::Leave)) {
          qInfo() << "theme isolated external pointer event" << event->type();
          return true;
        }
        return false;
      }

    private:
      QQuickItem *button_;
      const QSignalSpy &changes_;
      const bool &isolateExternal_;
    } hoverTrace(open, hoverChanges, isolateExternalPointer);
    window_->installEventFilter(&hoverTrace);
    auto traceCursor = [&](const char *stage, const QPoint &local) {
      qInfo() << "theme cursor" << stage << "local" << local << "global"
              << window_->mapToGlobal(local) << "actual" << QCursor::pos()
              << "active" << window_->isActive() << "exposed"
              << window_->isExposed() << "application"
              << QGuiApplication::applicationState() << "focusWindow"
              << QGuiApplication::focusWindow() << "buttonScene"
              << open->mapRectToScene(open->boundingRect())
              << "buttonGlobalCenter"
              << open->mapToGlobal(open->boundingRect().center())
              << "containsCursor"
              << open->contains(open->mapFromGlobal(QCursor::pos()))
              << "visible" << open->isVisible() << "enabled"
              << open->isEnabled() << "hoverEnabled"
              << open->property("hoverEnabled") << "hovered"
              << open->property("hovered") << "changes" << hoverChanges.size();
    };
    window_->requestActivate();
    QVERIFY(QTest::qWaitForWindowExposed(window_));
    if (QGuiApplication::platformName() == "cocoa") {
      window_->raise();
      const bool active = QTest::qWaitForWindowActive(window_);
      traceCursor("activation", QPoint());
      QVERIFY(active);
    }
    QTRY_VERIFY(!workspace_->property("layoutSettling").toBool());
    QSignalSpy readyFrame(window_, &QQuickWindow::frameSwapped);
    window_->update();
    QVERIFY(!readyFrame.isEmpty() || readyFrame.wait(2000));
    // Exercise the window's Quick hit-testing and hover chain without moving
    // the physical cursor. No control receives input directly, and the hover
    // state must still change through event delivery.
    auto sendMouse = [&](QQuickWindow *host, QEvent::Type type,
                         const QPoint &local, Qt::MouseButton button,
                         Qt::MouseButtons buttons) {
      if (!pointerWindows.contains(QPointer<QQuickWindow>(host)))
        pointerWindows.append(host);
      const auto global = host->mapToGlobal(local);
      QMouseEvent event(type, QPointF(local), QPointF(local), QPointF(global),
                        button, buttons, Qt::NoModifier);
      const bool delivered = QCoreApplication::sendEvent(host, &event);
      qInfo() << "theme window mouse" << type << "host" << host << "delivered"
              << delivered << "accepted" << event.isAccepted() << "local"
              << event.position() << "global" << event.globalPosition();
      return delivered;
    };
    auto moveMouse = [&](const QPoint &local, const char *stage) {
      const bool delivered = sendMouse(window_, QEvent::MouseMove, local,
                                       Qt::NoButton, Qt::NoButton);
      traceCursor(stage, local);
      return delivered;
    };
    const auto pointerIsolation =
        qScopeGuard([&] { isolateExternalPointer = false; });
    isolateExternalPointer = true;
    QVERIFY(moveMouse(QPoint(window_->width() - 10, 100), "idle"));
    QTRY_VERIFY(!open->property("hovered").toBool());
    QVERIFY(capture(connections, "01-empty-connections"));
    QVERIFY(!open->property("hovered").toBool());
    QVERIFY(panePixels("connections-docked"));
    QColor idle, hovered;
    const QRectF buttonRoi(3, 4, 5, open->height() - 8);
    QVERIFY(pixels(open, buttonRoi, "open-idle", true, &idle));
    auto *status =
        qobject_cast<QQuickItem *>(objectProperty(window_, "footer"));
    QVERIFY(status);
    QVERIFY(pixels(status, QRectF(8, 3, status->width() - 16, 3), "status-bar",
                   true));
    const auto hoverPoint =
        open->mapToScene(open->boundingRect().center()).toPoint();
    QVERIFY(moveMouse(hoverPoint, "hover requested"));
    QTRY_VERIFY(open->property("hovered").toBool());
    traceCursor("hover delivered", hoverPoint);
    QVERIFY(capture(open, "01-open-hover"));
    QVERIFY(open->property("hovered").toBool());
    QVERIFY(pixels(open, buttonRoi, "open-hover", true, &hovered));
    if (qAbs(idle.red() - hovered.red()) +
            qAbs(idle.green() - hovered.green()) +
            qAbs(idle.blue() - hovered.blue()) <
        9)
      failures << "Open Binary hover has no visible background change";
    isolateExternalPointer = false;

    auto *manage =
        findControl(connections, "QQuickButton", "text", "Manage Connections…");
    QVERIFY(manage);
    auto clickManage = [&] {
      auto *host = manage->window();
      if (!host || !host->isVisible() || !manage->isVisible() ||
          !manage->isEnabled())
        return false;
      const auto local =
          manage->mapToScene(manage->boundingRect().center()).toPoint();
      if (!QRect(QPoint(), host->size()).contains(local) ||
          !manage->contains(manage->mapFromScene(local)))
        return false;
      QSignalSpy clicked(manage, SIGNAL(clicked()));
      if (!clicked.isValid())
        return false;
      const bool moved =
          sendMouse(host, QEvent::MouseMove, local, Qt::NoButton, Qt::NoButton);
      const bool pressed = sendMouse(host, QEvent::MouseButtonPress, local,
                                     Qt::LeftButton, Qt::LeftButton);
      const bool released = sendMouse(host, QEvent::MouseButtonRelease, local,
                                      Qt::LeftButton, Qt::NoButton);
      qInfo() << "theme Manage clicked" << clicked.size() << "host" << host
              << "hitPoint" << local << "windowSize" << host->size();
      return moved && pressed && released && clicked.size() == 1;
    };
    QVERIFY(dock->setProperty("isFloating", true));
    QTRY_VERIFY(connections->window() && connections->window() != window_ &&
                connections->window()->isVisible());
    connections->window()->resize(460, 220);
    QTRY_VERIFY(connections->height() > 40);
    connections->window()->requestActivate();
    QVERIFY(QTest::qWaitForWindowExposed(connections->window()));
    if (QGuiApplication::platformName() == "cocoa")
      QVERIFY(QTest::qWaitForWindowActive(connections->window()));
    QVERIFY(capture(connections, "02-floating-connections"));
    QVERIFY(panePixels("connections-floating"));
    auto *floatingSurface = connections->window()->contentItem();
    QVERIFY(pixels(floatingSurface,
                   QRectF(2, 2, floatingSurface->width() - 4, 2),
                   "floating-window-frame", true));
    QVERIFY(clickManage());
    QTRY_VERIFY(manager->property("opened").toBool());
    auto *managerBackground =
        qobject_cast<QQuickItem *>(objectProperty(manager, "background"));
    auto *transport =
        findControl(manager, "QQuickComboBox", "currentText", "stdio");
    auto *endpoint = findControl(manager, "QQuickTextField", "placeholderText",
                                 "Absolute path to server executable");
    QVERIFY(managerBackground && transport && endpoint);
    auto managerContract = [&](const QString &phase) {
      const bool focused = QTest::qWaitFor(
          [&] { return window_->isActive() && endpoint->hasActiveFocus(); },
          1000);
      auto *host = managerBackground->window();
      qInfo() << phase << "managerHost" << host << "hostName"
              << (host ? host->objectName() : QString()) << "hostTitle"
              << (host ? host->title() : QString()) << "expectedMain" << window_
              << "mainActive" << window_->isActive() << "endpointFocus"
              << endpoint->hasActiveFocus() << "managerSize"
              << managerBackground->size() << "transportRect"
              << transport->mapRectToScene(transport->boundingRect());
      if (host != window_)
        failures << phase + ": manager is not owned by the main window";
      if (!focused)
        failures << phase +
                        ": main window and endpoint do not have active focus";
      auto usable = [&](QQuickItem *item) {
        return item->window() == window_ && item->isVisible() &&
               item->isEnabled() && item->width() > 24 && item->height() > 16 &&
               QRectF(QPointF(), QSizeF(window_->size()))
                   .contains(item->mapRectToScene(item->boundingRect()));
      };
      if (managerBackground->width() < 600 ||
          managerBackground->height() < 400 || !usable(managerBackground) ||
          !usable(endpoint) || !usable(transport))
        failures << phase +
                        ": manager form or transport geometry is not usable";
    };
    QVERIFY(popupPixels(manager, "03-main-manager-from-float"));
    managerContract("manager from floating Connections");
    transportPopup = objectProperty(transport, "popup");
    QVERIFY(transportPopup);
    QVERIFY(QMetaObject::invokeMethod(transportPopup, "open"));
    QTRY_VERIFY(transportPopup->property("opened").toBool());
    QVERIFY(popupPixels(transportPopup, "03-transport-menu"));
    auto *transportBackground = qobject_cast<QQuickItem *>(
        objectProperty(transportPopup, "background"));
    QVERIFY(transportBackground);
    if (transportBackground->window() != window_ ||
        transportBackground->width() < 100 ||
        transportBackground->height() < 40 ||
        !QRectF(QPointF(), QSizeF(window_->size()))
             .contains(transportBackground->mapRectToScene(
                 transportBackground->boundingRect())))
      failures << "transport menu is not usable within the main window";
    QVERIFY(QMetaObject::invokeMethod(transportPopup, "close"));
    QTRY_VERIFY(!transportPopup->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(manager, "close"));
    QTRY_VERIFY(!manager->property("visible").toBool());
    QVERIFY(dock->setProperty("isFloating", false));
    QTRY_VERIFY(connections->window() == window_);
    QTRY_VERIFY(!workspace_->property("layoutSettling").toBool());
    QSignalSpy redockedFrame(window_, &QQuickWindow::frameSwapped);
    window_->update();
    QVERIFY(!redockedFrame.isEmpty() || redockedFrame.wait(2000));
    QVERIFY(clickManage());
    QTRY_VERIFY(manager->property("opened").toBool());
    QVERIFY(popupPixels(manager, "03-main-manager-after-redock"));
    managerContract("manager from redocked Connections");
    QVERIFY(QMetaObject::invokeMethod(manager, "close"));
    QTRY_VERIFY(!manager->property("visible").toBool());
    QVERIFY(show("references"));

    QVERIFY(QMetaObject::invokeMethod(settingsAction, "trigger"));
    QTRY_VERIFY(settings->property("opened").toBool());
    QVERIFY(popupPixels(settings, "04-settings"));
    languagePopup = objectProperty(language, "popup");
    QVERIFY(languagePopup);
    QVERIFY(QMetaObject::invokeMethod(languagePopup, "open"));
    QTRY_VERIFY(languagePopup->property("opened").toBool());
    QVERIFY(popupPixels(languagePopup, "04-language-menu"));
    restore();
    QTRY_VERIFY(!settings->property("visible").toBool() &&
                !manager->property("visible").toBool());
    QTRY_VERIFY(!dock->property("isFloating").toBool() &&
                !workspace_->property("layoutSettling").toBool());
    QVERIFY(!workbench_.loaded());
    QVERIFY(!workbench_.workerConnected());
    QVERIFY2(failures.isEmpty(), qPrintable(failures.join("\n")));
  }

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
