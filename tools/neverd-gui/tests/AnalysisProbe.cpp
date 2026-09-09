#include "AnalysisProbe.h"

#include "NativeGraphItem.h"
#include "Workbench.h"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>
#include <memory>

namespace {
struct AnalysisProbe {
  Workbench *workbench = nullptr;
  QQuickWindow *window = nullptr;
  QObject *machine = nullptr;
  NativeGraphItem *graph = nullptr;
  QObject *unsavedDialog = nullptr;
  QObject *functionsDock = nullptr;
  QObject *representationDock = nullptr;
  QTimer *timer = nullptr;
  QElapsedTimer deadline;
  QElapsedTimer settling;
  QString captureDirectory;
  QString mappedAddress;
  QString functionAddress;
  int stage = 0;

  void fail(const QString &description) {
    qCritical().noquote() << "Analysis probe failed at stage" << stage
                          << description;
    if (!captureDirectory.isEmpty()) {
      QDir().mkpath(captureDirectory);
      window->grabWindow().save(
          QDir(captureDirectory).filePath("neverd-analysis-failure.png"));
    }
    timer->stop();
    QCoreApplication::exit(1);
  }
  bool capture(const QString &name) {
    if (captureDirectory.isEmpty())
      return true;
    if (!QDir().mkpath(captureDirectory) ||
        !window->grabWindow().save(
            QDir(captureDirectory).filePath(name + ".png"))) {
      fail("Could not capture the actual workbench window");
      return false;
    }
    return true;
  }
  void next() {
    ++stage;
    settling.restart();
  }
  bool switchCenter(int index) {
    if (!QMetaObject::invokeMethod(window, "switchCenter",
                                   Q_ARG(QVariant, QVariant(index)))) {
      fail("Main.switchCenter is not available");
      return false;
    }
    return true;
  }
  void step() {
    if (deadline.elapsed() > 15000) {
      fail("Timed out waiting for real engine output: " + workbench->error());
      return;
    }
    switch (stage) {
    case 0:
      if (!workbench->loaded() || workbench->representationText().isEmpty() ||
          workbench->busy())
        return;
      if (workbench->representation() != "c") {
        workbench->setRepresentation("c");
        return;
      }
      functionAddress = workbench->selectedFunctionAddress();
      next();
      break;
    case 1:
      if (settling.elapsed() < 250)
        return;
      if (!capture("neverd-analysis-c"))
        return;
      workbench->setRepresentation("low");
      next();
      break;
    case 2: {
      if (workbench->representationText().isEmpty() || workbench->busy())
        return;
      const auto mappings = workbench->textMappings();
      int mappedLine = -1;
      for (const auto &value : mappings) {
        const auto row = value.toMap();
        const auto addresses = row.value("addresses").toList();
        if (addresses.isEmpty())
          continue;
        mappedAddress = addresses.first().toString();
        mappedLine = row.contains("line") ? row.value("line").toInt() : -1;
        if (mappedAddress != workbench->selectedAddress())
          break;
      }
      if (mappedLine < 0 || mappedAddress.isEmpty()) {
        fail("LowIR has no exact instruction anchor from the matching engine");
        return;
      }
      workbench->selectTextLine(mappedLine);
      if (workbench->selectedAddress() != mappedAddress) {
        fail("Selecting a mapped LowIR row did not select its exact "
             "instruction address");
        return;
      }
      next();
      break;
    }
    case 3:
      if (settling.elapsed() < 250)
        return;
      if (!capture("neverd-analysis-low"))
        return;
      workbench->setRepresentation("med");
      next();
      break;
    case 4:
      if (workbench->representationText().isEmpty() || workbench->busy())
        return;
      if (workbench->representation() != "med" ||
          workbench->selectedAddress() != mappedAddress ||
          workbench->selectedFunctionAddress() != functionAddress) {
        fail("Changing representation changed the selected instruction or "
             "function");
        return;
      }
      if (!switchCenter(1))
        return;
      next();
      break;
    case 5:
      if (workbench->graphNodes().isEmpty() ||
          workbench->graphSummary().isEmpty())
        return;
      // KDDW reparents panel content during its deferred initial layout.
      // Resolve rendered objects after activation, not during engine.load().
      graph = window->findChild<NativeGraphItem *>("nativeGraph");
      machine = window->findChild<QObject *>("machinePane");
      if (!graph || !graph->isVisible() || graph->width() <= 0 ||
          graph->height() <= 0 || graph->boundedNodeCount() <= 0 ||
          graph->boundedNodeCount() > 256 || graph->boundedEdgeCount() > 512) {
        fail(QString("Actual native graph pane did not render a bounded worker "
                     "viewport: found=%1 visible=%2 width=%3 height=%4 "
                     "nodes=%5 edges=%6 workerNodes=%7")
                 .arg(graph != nullptr)
                 .arg(graph && graph->isVisible())
                 .arg(graph ? graph->width() : -1)
                 .arg(graph ? graph->height() : -1)
                 .arg(graph ? graph->boundedNodeCount() : -1)
                 .arg(graph ? graph->boundedEdgeCount() : -1)
                 .arg(workbench->graphNodes().size()));
        return;
      }
      if (!machine || machine->property("currentIndex").toInt() != 1) {
        fail("Machine pane did not switch to CFG");
        return;
      }
      next();
      break;
    case 6:
      if (settling.elapsed() < 250)
        return;
      if (!capture("neverd-analysis-cfg"))
        return;
      if (!switchCenter(2))
        return;
      next();
      break;
    case 7:
      if (workbench->hexText().isEmpty())
        return;
      if (!machine || machine->property("currentIndex").toInt() != 2 ||
          workbench->selectedAddress() != mappedAddress) {
        fail("Hex navigation lost the selected instruction");
        return;
      }
      // This probe runs against its provided fixture. The edit stays in the
      // worker's staging state and is discarded when the probe exits.
      workbench->setComment("NeverD GUI close-cancel probe");
      next();
      break;
    case 8:
      if (workbench->selectedComment() != "NeverD GUI close-cancel probe" ||
          !workbench->unsavedChanges())
        return;
      window->close();
      next();
      break;
    case 9:
      if (settling.elapsed() < 200)
        return;
      unsavedDialog = window->findChild<QObject *>("unsavedAnnotationsDialog");
      if (!window->isVisible() || !unsavedDialog ||
          !unsavedDialog->property("visible").toBool()) {
        fail("Closing with a staged comment did not keep the window open for "
             "confirmation");
        return;
      }
      if (!QMetaObject::invokeMethod(unsavedDialog, "reject")) {
        fail("Unsaved dialog cannot cancel closing");
        return;
      }
      next();
      break;
    case 10:
      if (settling.elapsed() < 200)
        return;
      functionsDock = window->findChild<QObject *>("functionsDock");
      representationDock = window->findChild<QObject *>("representationDock");
      if (!window->isVisible() || unsavedDialog->property("visible").toBool() ||
          !functionsDock || !functionsDock->property("isOpen").toBool() ||
          !representationDock ||
          !representationDock->property("isOpen").toBool()) {
        fail("Cancelling close lost the main window or docking panels");
        return;
      }
      qInfo("Analysis probe passed: real C, exact LowIR address selection, "
            "MedIR context, bounded native CFG, Hex, dirty-close cancellation");
      timer->stop();
      QCoreApplication::exit(0);
      break;
    }
  }
};
} // namespace

void startAnalysisProbe(QQmlApplicationEngine &engine, Workbench &workbench) {
  auto probe = std::make_shared<AnalysisProbe>();
  probe->workbench = &workbench;
  probe->window = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
  if (!probe->window) {
    QCoreApplication::exit(1);
    return;
  }
  probe->machine = probe->window->findChild<QObject *>("machinePane");
  probe->graph = probe->window->findChild<NativeGraphItem *>("nativeGraph");
  probe->unsavedDialog =
      probe->window->findChild<QObject *>("unsavedAnnotationsDialog");
  probe->functionsDock = probe->window->findChild<QObject *>("functionsDock");
  probe->representationDock =
      probe->window->findChild<QObject *>("representationDock");
  probe->captureDirectory = qEnvironmentVariable("NEVERD_GUI_CAPTURE_DIR");
  probe->timer = new QTimer(&engine);
  probe->timer->setInterval(100);
  probe->deadline.start();
  probe->settling.start();
  QObject::connect(probe->timer, &QTimer::timeout, &engine,
                   [probe] { probe->step(); });
  probe->timer->start();
}
