#include "DockingProbe.h"

#include "Workbench.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <memory>

namespace {
struct Probe {
  QQmlApplicationEngine *engine = nullptr;
  Workbench *workbench = nullptr;
  QQuickWindow *window = nullptr;
  QObject *workspace = nullptr;
  QObject *representation = nullptr;
  PaneController *representationController = nullptr;
  QPointer<QQuickWindow> floating;
  QObject *saver = nullptr;
  QTimer *timer = nullptr;
  QString originalLanguage;
  QString savedLayoutPath;
  QString changedLayoutPath;
  QByteArray savedLayout;
  const QDateTime savedTimestamp = QDateTime::fromSecsSinceEpoch(946684800);
  int stage = 0;
  int stageWaits = 0;

  bool require(bool condition, const char *description) {
    if (condition)
      return true;
    qCritical("Docking probe failed at stage %d: %s", stage, description);
    timer->stop();
    workbench->setLanguage(originalLanguage);
    QCoreApplication::exit(1);
    return false;
  }

  bool awaitStage(bool condition, const char *description) {
    if (condition) {
      stageWaits = 0;
      return true;
    }
    if (++stageWaits < 10)
      --stage;
    else
      require(false, description);
    return false;
  }

  bool representationState(bool open, bool visible, const char *description) {
    return require(representationController &&
                       representationController->open() == open &&
                       representationController->contentVisible() == visible,
                   description);
  }

  bool layoutCommand(const char *command, const QString &path) {
    bool result = false;
    return QMetaObject::invokeMethod(saver, command, Q_RETURN_ARG(bool, result),
                                     Q_ARG(QString, path)) &&
           result;
  }

  void step() {
    switch (stage++) {
    case 0:
      if (!require(workspace && representation && representationController &&
                       saver,
                   "named docking components exist"))
        return;
      if (!require(representation->property("isOpen").toBool(),
                   "representation starts open"))
        return;
      if (!representationState(true, true,
                               "initial controller matches open dock"))
        return;
      if (!require(
              !layoutCommand("restoreFromFile", savedLayoutPath + ".missing"),
              "first launch accepts a missing saved layout"))
        return;
      if (!require(QMetaObject::invokeMethod(workspace, "resetLayout"),
                   "default layout command"))
        return;
      break;
    case 1:
      if (!require(representation->setProperty("isFloating", true),
                   "float command accepted"))
        return;
      break;
    case 2: {
      if (!require(representation->property("isFloating").toBool(),
                   "panel became floating"))
        return;
      for (QWindow *candidate : QGuiApplication::topLevelWindows()) {
        if (candidate != window && candidate->isVisible()) {
          floating = qobject_cast<QQuickWindow *>(candidate);
          if (floating)
            break;
        }
      }
      if (!require(floating != nullptr, "native floating QQuickWindow exists"))
        return;
      if (!representationState(true, true, "floating controller stays visible"))
        return;
      floating->setPosition(100000, 100000);
      workbench->clampWindows();
      bool intersects = false;
      for (auto *screen : QGuiApplication::screens())
        intersects |=
            screen->availableGeometry().intersects(floating->geometry());
      if (!require(intersects,
                   "off-screen floating panel recovers to an available screen"))
        return;
      floating->showMinimized();
      break;
    }
    case 3:
      if (!awaitStage(floating &&
                          floating->visibility() == QWindow::Minimized &&
                          !representationController->contentVisible(),
                      "minimized floating controller stops visible work"))
        return;
      if (!require(representation->property("isOpen").toBool(),
                   "minimized floating dock remains open") ||
          !representationState(true, false,
                               "minimized controller stays open but hidden"))
        return;
      floating->showNormal();
      break;
    case 4:
      if (!awaitStage(floating && floating->isVisible() &&
                          floating->visibility() != QWindow::Minimized &&
                          representationController->contentVisible(),
                      "restored floating controller becomes visible"))
        return;
      if (!representationState(true, true,
                               "normal floating controller resumes visibility"))
        return;
      representation->setProperty("isFloating", false);
      break;
    case 5: {
      if (!require(!representation->property("isFloating").toBool(),
                   "panel redocked"))
        return;
      if (!representationState(true, true,
                               "redocked controller remains visible"))
        return;
      // Separate probe snapshots from the workspace's periodic autosave.
      if (!require(layoutCommand("saveToFile", savedLayoutPath),
                   "layout save succeeded"))
        return;
      QFile file(savedLayoutPath);
      if (!require(file.open(QIODevice::ReadWrite), "layout file created"))
        return;
      savedLayout = file.readAll();
      const auto saved = QJsonDocument::fromJson(savedLayout).object();
      if (!require(!saved.isEmpty(), "saved layout is valid JSON"))
        return;
      if (!require(saved.value("closedDockWidgets").toArray().size() < 7,
                   "active panels are saved before teardown"))
        return;
      if (!require(file.setFileTime(savedTimestamp,
                                    QFileDevice::FileModificationTime),
                   "unchanged-save timestamp precondition"))
        return;
      file.close();
      if (!require(layoutCommand("saveToFile", savedLayoutPath),
                   "unchanged layout save succeeds"))
        return;
      if (!require(QFileInfo(savedLayoutPath).lastModified() == savedTimestamp,
                   "unchanged layout preserves file modification time"))
        return;
      if (!require(QFile::copy(savedLayoutPath, changedLayoutPath),
                   "changed-layout baseline snapshot"))
        return;
      QMetaObject::invokeMethod(representation, "close");
      break;
    }
    case 6: {
      if (!require(!representation->property("isOpen").toBool(),
                   "panel closed"))
        return;
      if (!representationState(false, false, "closed dock retires controller"))
        return;
      if (!require(layoutCommand("saveToFile", changedLayoutPath),
                   "changed layout save succeeds"))
        return;
      QFile changed(changedLayoutPath);
      if (!require(changed.open(QIODevice::ReadOnly),
                   "changed layout is readable"))
        return;
      if (!require(changed.readAll() != savedLayout,
                   "closing a panel updates the saved layout"))
        return;
      if (!require(layoutCommand("restoreFromFile", savedLayoutPath),
                   "layout restored through KDDockWidgets"))
        return;
      break;
    }
    case 7:
      if (!require(representation->property("isOpen").toBool(),
                   "saved panel reopened"))
        return;
      if (!representationState(true, true,
                               "saved open dock restores controller"))
        return;
      if (!require(layoutCommand("restoreFromFile", changedLayoutPath),
                   "changed layout restored through KDDockWidgets"))
        return;
      break;
    case 8:
      if (!require(!representation->property("isOpen").toBool(),
                   "changed snapshot restores the closed panel state"))
        return;
      if (!representationState(false, false,
                               "saved closed dock retires controller"))
        return;
      if (!require(layoutCommand("restoreFromFile", savedLayoutPath),
                   "original layout restored after changed snapshot"))
        return;
      break;
    case 9:
      if (!require(representation->property("isOpen").toBool(),
                   "original snapshot reopens the panel"))
        return;
      if (!representationState(
              true, true, "original snapshot restores controller visibility"))
        return;
      window->resize(900, 950);
      break;
    case 10:
      QMetaObject::invokeMethod(workspace, "resetLayout");
      break;
    case 11: {
      auto *machine = window->findChild<QQuickItem *>("machinePane");
      auto *code = window->findChild<QQuickItem *>("representationPane");
      if (!require(machine && code, "both analysis panes remain present"))
        return;
      if (!require(machine->width() >= 280 && code->width() >= 300,
                   "narrow layout respects reading widths"))
        return;
      if (!require(code->mapToScene(QPointF()).y() >
                       machine->mapToScene(QPointF()).y() + 100,
                   "narrow default stacks the two analysis panes"))
        return;
      workbench->setLanguage("ar");
      break;
    }
    case 12: {
      const auto rootMirror = window->property("interfaceMirrored");
      auto *address = window->findChild<QQuickItem *>("addressField");
      if (!require(rootMirror.isValid() && workbench->language() == "ar" &&
                       rootMirror.toBool(),
                   "Arabic interface mirrors"))
        return;
      if (!require(address && address->property("actualMirrored").isValid() &&
                       !address->property("actualMirrored").toBool(),
                   "technical address field stays left-to-right"))
        return;
      QMetaObject::invokeMethod(workspace, "focusPanel",
                                Q_ARG(QVariant, QVariant("machine")));
      break;
    }
    case 13:
      if (!require(!representation->property("isOpen").toBool(),
                   "focused mode hides other panes"))
        return;
      if (!representationState(true, false,
                               "focused mode hides without closing controller"))
        return;
      workbench->setLanguage(originalLanguage);
      window->resize(1500, 950);
      QMetaObject::invokeMethod(workspace, "resetLayout");
      break;
    case 14:
      if (!require(representation->property("isOpen").toBool(),
                   "reset restores companion dock") ||
          !representationState(
              true, true, "reset restores companion controller visibility"))
        return;
      qInfo("Docking probe passed: native float/redock, off-screen recovery, "
            "unchanged save, changed-layout restore, 900px layout, Arabic/LTR, "
            "focused mode, controller open/visible, floating minimize/restore");
      timer->stop();
      QCoreApplication::exit(0);
      break;
    }
  }
};
} // namespace

void startDockingProbe(QQmlApplicationEngine &engine, Workbench &workbench) {
  auto probe = std::make_shared<Probe>();
  probe->engine = &engine;
  probe->workbench = &workbench;
  probe->representationController =
      workbench.paneRegistry()->defaultRepresentation();
  probe->originalLanguage = workbench.language();
  probe->savedLayoutPath = workbench.dockLayoutPath() + ".probe.json";
  probe->changedLayoutPath = workbench.dockLayoutPath() + ".changed.json";
  probe->window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
  if (!probe->window) {
    QCoreApplication::exit(1);
    return;
  }
  probe->workspace = probe->window->findChild<QObject *>("dockWorkspace");
  probe->representation =
      probe->window->findChild<QObject *>("representationDock");
  probe->saver = probe->window->findChild<QObject *>("dockLayoutSaver");
  probe->timer = new QTimer(&engine);
  probe->timer->setInterval(350);
  QObject::connect(probe->timer, &QTimer::timeout, &engine,
                   [probe] { probe->step(); });
  QTimer::singleShot(900, &engine, [probe] { probe->timer->start(); });
}
