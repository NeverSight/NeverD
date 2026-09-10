#include "StartupMetrics.h"

#include "Workbench.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSaveFile>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

namespace {
QString graphicsApiName(QSGRendererInterface::GraphicsApi api) {
  switch (api) {
  case QSGRendererInterface::Software:
    return "software";
  case QSGRendererInterface::OpenGL:
    return "OpenGL";
  case QSGRendererInterface::Direct3D11:
    return "Direct3D11";
  case QSGRendererInterface::Vulkan:
    return "Vulkan";
  case QSGRendererInterface::Metal:
    return "Metal";
  case QSGRendererInterface::Null:
    return "null";
  case QSGRendererInterface::Direct3D12:
    return "Direct3D12";
  default:
    return "unknown";
  }
}
} // namespace

StartupMetrics::StartupMetrics(Workbench &workbench, const QElapsedTimer &clock,
                               const QString &output, bool openingFile,
                               int timeoutMs, QObject *parent)
    : QObject(parent), workbench_(workbench), clock_(clock), output_(output),
      openingFile_(openingFile), timeoutMs_(timeoutMs) {
  connect(&workbench_, &Workbench::ready, this,
          [this] { record("worker_ready_ms"); });
  connect(&workbench_, &Workbench::changed, this, [this] {
    if (workbench_.loaded())
      record("metadata_ms");
    const auto *instructions =
        qobject_cast<PageModel *>(workbench_.instructionsModel());
    const bool hasInstructions = instructions && instructions->count() > 0;
    const bool hasRepresentation = !workbench_.representationText().isEmpty();
    if (hasInstructions)
      record("instructions_ms");
    if (hasRepresentation)
      record("representation_ms");
    // Milestones retain the first arrival, but the next frame must contain
    // current data even if navigation clears a pane before synchronization.
    contentReady_.store(workbench_.loaded() && hasInstructions &&
                        hasRepresentation);
    if (!workbench_.error().isEmpty())
      finish(false, "workbench_error");
  });
}

void StartupMetrics::record(const char *name) {
  record(name, clock_.nsecsElapsed() / 1.0e6);
}

void StartupMetrics::record(const char *name, double elapsedMs) {
  if (!milestones_.contains(QLatin1String(name)))
    milestones_[QLatin1String(name)] = elapsedMs;
}

void StartupMetrics::observeWindow(QQuickWindow *window) {
  window_ = window;
  record("qml_created_ms");
  if (!window) {
    finish(false, "missing_quick_window");
    return;
  }
  // Qt blocks the GUI thread while synchronizing the scene graph. Readiness
  // is attached to that synchronized frame, not to later GUI event delivery.
  connect(
      window, &QQuickWindow::afterSynchronizing, this,
      [this] {
        synchronizedContent_ = !openingFile_ || contentReady_.load();
        synchronizedMs_ = clock_.nsecsElapsed() / 1.0e6;
      },
      Qt::DirectConnection);
  connect(
      window, &QQuickWindow::frameSwapped, this,
      [this] {
        const auto swappedMs = clock_.nsecsElapsed() / 1.0e6;
        const auto synchronizedMs = synchronizedMs_;
        const bool containsContent = synchronizedContent_;
        const bool onGuiThread = QThread::currentThread() == thread();
        QMetaObject::invokeMethod(
            this,
            [this, swappedMs, synchronizedMs, containsContent, onGuiThread] {
              if (finished_)
                return;
              record("first_frame_ms", swappedMs);
              if (swappedMs > timeoutMs_) {
                finish(false, "timeout");
                return;
              }
              if (containsContent) {
                record("useful_frame_sync_ms", synchronizedMs);
                record("useful_frame_ms", swappedMs);
                milestones_["useful_frame_delivery_ms"] =
                    clock_.nsecsElapsed() / 1.0e6;
                renderingOnGui_ = onGuiThread;
                finish(true);
              }
            },
            Qt::QueuedConnection);
      },
      Qt::DirectConnection);
  // Request a frame after the QML bindings have consumed newly arrived data.
  connect(
      &workbench_, &Workbench::changed, window, [window] { window->update(); },
      Qt::QueuedConnection);
  const auto remaining = qMax(qint64(0), timeoutMs_ - clock_.elapsed());
  QTimer::singleShot(remaining, this, [this] { finish(false, "timeout"); });
}

void StartupMetrics::finish(bool success, const QString &reason) {
  if (finished_)
    return;
  finished_ = true;
  QJsonObject report{
      {"schema_version", 2},
      {"success", success},
      {"failure_reason", reason},
      {"timeout_ms", timeoutMs_},
      {"qt_version", qVersion()},
      {"platform_plugin", QGuiApplication::platformName()},
      {"os", QSysInfo::prettyProductName()},
      {"architecture", QSysInfo::currentCpuArchitecture()},
      {"milestones", milestones_},
      {"error", workbench_.error()},
      {"clock_origin", "main entry, after dynamic loading"},
      {"preferences",
       "temporary defaults; user settings are not read or written"},
      {"measurement", "Qt frameSwapped emission after data synchronization; "
                      "not hardware presentation"}};
  if (window_) {
    report["device_pixel_ratio"] = window_->devicePixelRatio();
    report["width"] = window_->width();
    report["height"] = window_->height();
    report["graphics_api"] =
        graphicsApiName(window_->rendererInterface()->graphicsApi());
  }
  if (success)
    report["rendering_thread_is_gui"] = renderingOnGui_;
  QSaveFile file(output_);
  const auto bytes = QJsonDocument(report).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    qCritical("Could not write startup benchmark report");
    QTimer::singleShot(0, this, [] { QCoreApplication::exit(2); });
    return;
  }
  // openFile() can fail before main enters app.exec(). An immediate exit()
  // there is ignored by Qt, so every terminal result exits on the event loop.
  QTimer::singleShot(0, this,
                     [success] { QCoreApplication::exit(success ? 0 : 1); });
}
