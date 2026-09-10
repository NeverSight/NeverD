#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <atomic>

class QQuickWindow;
class Workbench;

// Opt-in measurement of the production workbench, not a synthetic viewport.
class StartupMetrics final : public QObject {
public:
  StartupMetrics(Workbench &workbench, const QElapsedTimer &clock,
                 const QString &output, bool openingFile, int timeoutMs,
                 QObject *parent);
  void observeWindow(QQuickWindow *window);
  // Absolute elapsed time from the same main-entry clock, for phase profiling.
  void record(const char *name);
  void record(const char *name, double elapsedMs);

private:
  void finish(bool success, const QString &reason = {});

  Workbench &workbench_;
  QElapsedTimer clock_;
  QString output_;
  QJsonObject milestones_;
  QPointer<QQuickWindow> window_;
  bool openingFile_;
  int timeoutMs_;
  bool finished_ = false;
  bool renderingOnGui_ = false;
  std::atomic<bool> contentReady_{false};
  // Accessed only in direct scene-graph callbacks, on the rendering thread.
  bool synchronizedContent_ = false;
  double synchronizedMs_ = 0;
};
