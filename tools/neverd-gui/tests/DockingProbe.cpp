#include "DockingProbe.h"

#include "Workbench.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
  QObject *saver = nullptr;
  QTimer *timer = nullptr;
  QString originalLanguage;
  int stage = 0;

  bool require(bool condition, const char *description) {
    if (condition)
      return true;
    qCritical("Docking probe failed at stage %d: %s", stage, description);
    timer->stop();
    workbench->setLanguage(originalLanguage);
    QCoreApplication::exit(1);
    return false;
  }

  void step() {
    switch (stage++) {
    case 0:
      if (!require(workspace && representation && saver,
                   "named docking components exist"))
        return;
      if (!require(representation->property("isOpen").toBool(),
                   "representation starts open"))
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
      QQuickWindow *floating = nullptr;
      for (QWindow *candidate : QGuiApplication::topLevelWindows()) {
        if (candidate != window && candidate->isVisible()) {
          floating = qobject_cast<QQuickWindow *>(candidate);
          if (floating)
            break;
        }
      }
      if (!require(floating != nullptr, "native floating QQuickWindow exists"))
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
      representation->setProperty("isFloating", false);
      break;
    }
    case 3: {
      if (!require(!representation->property("isFloating").toBool(),
                   "panel redocked"))
        return;
      QMetaObject::invokeMethod(workspace, "saveLayout");
      QFile file(workbench->dockLayoutPath());
      if (!require(file.open(QIODevice::ReadOnly), "layout file created"))
        return;
      const auto saved = QJsonDocument::fromJson(file.readAll()).object();
      if (!require(!saved.isEmpty(), "saved layout is valid JSON"))
        return;
      if (!require(saved.value("closedDockWidgets").toArray().size() < 7,
                   "active panels are saved before teardown"))
        return;
      QMetaObject::invokeMethod(representation, "close");
      break;
    }
    case 4: {
      if (!require(!representation->property("isOpen").toBool(),
                   "panel closed"))
        return;
      bool restored = false;
      QMetaObject::invokeMethod(saver, "restoreFromFile",
                                Q_RETURN_ARG(bool, restored),
                                Q_ARG(QString, workbench->dockLayoutPath()));
      if (!require(restored, "layout restored through KDDockWidgets"))
        return;
      break;
    }
    case 5:
      if (!require(representation->property("isOpen").toBool(),
                   "saved panel reopened"))
        return;
      window->resize(900, 950);
      break;
    case 6:
      QMetaObject::invokeMethod(workspace, "resetLayout");
      break;
    case 7: {
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
    case 8: {
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
    case 9:
      if (!require(!representation->property("isOpen").toBool(),
                   "focused mode hides other panes"))
        return;
      workbench->setLanguage(originalLanguage);
      window->resize(1500, 950);
      QMetaObject::invokeMethod(workspace, "resetLayout");
      break;
    case 10:
      qInfo("Docking probe passed: native float/redock, off-screen recovery, "
            "save/restore, 900px layout, Arabic/LTR, focused mode");
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
  probe->originalLanguage = workbench.language();
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
