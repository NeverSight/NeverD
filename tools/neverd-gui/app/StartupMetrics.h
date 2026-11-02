//===- StartupMetrics.h - GUI startup observations -----------------------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe production workbench startup without changing its workload.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_GUI_APP_STARTUPMETRICS_H
#define NEVERD_TOOLS_GUI_APP_STARTUPMETRICS_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>

namespace neverd::gui {

class MainWindow;
class Session;

/// Opt-in measurement of the production workbench: milestones from main
/// entry to the first painted disassembly of the opened file.
class StartupMetrics final : public QObject {
public:
  StartupMetrics(Session &session, const QElapsedTimer &clock,
                 const QString &output, bool openingFile, int timeoutMs,
                 QObject *parent);
  void observeWindow(MainWindow *window);
  /// Absolute elapsed time from the same main-entry clock.
  void record(const char *name);
  void record(const char *name, double elapsedMs);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void finish(bool success, const QString &reason = {});

  Session &session_;
  QElapsedTimer clock_;
  QString output_;
  QJsonObject milestones_;
  QPointer<MainWindow> window_;
  bool openingFile_;
  int timeoutMs_;
  bool finished_ = false;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_GUI_APP_STARTUPMETRICS_H
