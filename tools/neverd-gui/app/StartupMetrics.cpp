//===- StartupMetrics.cpp - GUI startup observations ---------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Record startup milestones and the listing delivered by the workbench.
//===----------------------------------------------------------------------===//

#include "StartupMetrics.h"

#include "Address.h"
#include "DisassemblyView.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "Session.h"

#include <QApplication>
#include <QEvent>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>

namespace neverd::gui {
namespace {
constexpr int ReportSchema = 4;
} // namespace

StartupMetrics::StartupMetrics(Session &session, const QElapsedTimer &clock,
                               const QString &output, bool openingFile,
                               int timeoutMs, QObject *parent)
    : QObject(parent), session_(session), clock_(clock), output_(output),
      openingFile_(openingFile), timeoutMs_(timeoutMs) {
  connect(&session_, &Session::stateChanged, this, [this] {
    if (session_.connected() && !milestones_.contains("worker_ready_ms"))
      record("worker_ready_ms");
    if (session_.loaded() && !milestones_.contains("metadata_ms"))
      record("metadata_ms");
    if (!session_.lastError().isEmpty())
      finish(false, QStringLiteral("workbench_error"));
  });
}

void StartupMetrics::record(const char *name) {
  record(name, clock_.nsecsElapsed() / 1.0e6);
}

void StartupMetrics::record(const char *name, double elapsedMs) {
  if (!milestones_.contains(QLatin1String(name)))
    milestones_.insert(QLatin1String(name), elapsedMs);
}

void StartupMetrics::observeWindow(MainWindow *window) {
  window_ = window;
  record("window_created_ms");
  window->installEventFilter(this);
  connect(window, &MainWindow::firstContentPainted, this, [this] {
    record("useful_frame_ms");
    // The paint reaches the window surface when this event-loop pass flushes
    // the backing store.
    QTimer::singleShot(0, this, [this] {
      record("useful_frame_flushed_ms");
      finish(true);
    });
  });
  if (!openingFile_)
    QTimer::singleShot(0, this, [this] {
      if (milestones_.contains("first_frame_ms"))
        finish(true);
    });
  const auto remaining = qMax(qint64(0), timeoutMs_ - clock_.elapsed());
  QTimer::singleShot(remaining, this,
                     [this] { finish(false, QStringLiteral("timeout")); });
}

bool StartupMetrics::eventFilter(QObject *object, QEvent *event) {
  if (object == window_ && event->type() == QEvent::Paint &&
      !milestones_.contains("first_frame_ms")) {
    record("first_frame_ms");
    if (!openingFile_)
      QTimer::singleShot(0, this, [this] { finish(true); });
  }
  return QObject::eventFilter(object, event);
}

void StartupMetrics::finish(bool success, const QString &reason) {
  if (finished_)
    return;
  finished_ = true;
  QJsonObject report{
      {"schema_version", ReportSchema},
      {"success", success},
      {"failure_reason", reason},
      {"timeout_ms", timeoutMs_},
      {"qt_version", qVersion()},
      {"platform_plugin", QApplication::platformName()},
      {"os", QSysInfo::prettyProductName()},
      {"architecture", QSysInfo::currentCpuArchitecture()},
      {"milestones", milestones_},
      {"error", session_.lastError()},
      {"clock_origin", "main entry, after dynamic loading"},
      // The origin on the system's monotonic clock, so that a runner can
      // measure from the launch it timed.
      {"clock_origin_monotonic_ms", clock_.msecsSinceReference()},
      {"preferences",
       "temporary defaults; user settings are not read or written"},
      {"measurement",
       "Qt Widgets paint of the disassembly listing and the "
       "following backing-store flush; not hardware presentation"}};
  if (window_) {
    report["device_pixel_ratio"] = window_->devicePixelRatioF();
    report["width"] = window_->width();
    report["height"] = window_->height();
    if (openingFile_ && success) {
      const auto *listing = window_->disassembly()->listing();
      const auto current = listing->currentAddress();
      const auto top = listing->topItem();
      // Collect evidence after recording the paint/flush milestones.
      report["input_path"] = session_.filePath();
      report["listing"] = QJsonObject{
          {"current_address", current ? hexAddress(*current) : QString()},
          {"top_address", top ? hexAddress(*top) : QString()},
          {"rows_current", listing->searchReady()},
          {"jump_pending", listing->jumpPending()},
          {"current_text", listing->currentLineText()},
          {"loaded_text", listing->searchText()},
          {"viewport_width", listing->viewport()->width()},
          {"viewport_height", listing->viewport()->height()}};
    }
  }
  QSaveFile file(output_);
  const auto bytes = QJsonDocument(report).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    qCritical("Could not write startup benchmark report");
    QTimer::singleShot(0, this, [] { QCoreApplication::exit(2); });
    return;
  }
  // Opening can fail before main enters exec(); exit on the event loop.
  QTimer::singleShot(0, this,
                     [success] { QCoreApplication::exit(success ? 0 : 1); });
}

} // namespace neverd::gui
