//===- AnalysisProgress.h - Non-blocking analysis progress ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Opening and background analysis, shown without blocking the workbench.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_ANALYSISPROGRESS_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_ANALYSISPROGRESS_H

#include <QElapsedTimer>
#include <QFrame>
#include <QJsonObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QWidget>

class QLabel;

namespace neverd::gui {

/// What the workbench does in the background: opening a file, finding its
/// functions, then indexing its references, as the session and its worker
/// report them.  Queries refresh the session often, so this reports a change
/// only when what it shows changes.
class AnalysisProgress final : public QObject {
  Q_OBJECT
public:
  enum class Stage { Idle, Loading, Discovering, Indexing, Finished };
  Q_ENUM(Stage)

  explicit AnalysisProgress(QObject *parent = nullptr);

  /// The session's state: whether \p file is opening, whether a file is
  /// loaded and analyzed in idle time, and the worker's background object.
  /// Without idle-time analysis the worker indexes references only when a
  /// query needs them.
  void update(bool opening, bool loaded, bool analysis, const QString &file,
              const QJsonObject &background);
  /// A step of the load: the engine's phase token, the step's completed
  /// fraction or a negative value while unknown, and the engine's detail.
  void loadProgress(const QString &phase, double fraction,
                    const QString &detail);
  /// Report the texts again in the current language.
  void retranslate();

  Stage stage() const { return stage_; }
  /// The completed fraction of the current step; negative while unknown.
  double fraction() const { return fraction_; }
  /// While a file opens into an empty workbench.
  bool firstLoad() const { return stage_ == Stage::Loading && !loaded_; }
  /// The file opening, by name.
  QString fileName() const;
  /// The status bar's text, after IDA's AU indicator.
  QString statusText() const;
  /// The status bar's text for \p stage at a whole \p percent, or without
  /// one when it is negative.
  static QString statusText(Stage stage, int percent);
  /// The load step in words, and the engine's own words for it.
  QString stepText() const;
  QString detail() const;
  /// Time since the file began to open.
  qint64 elapsedMs() const;

signals:
  void changed();
  /// The initial analysis of the opened file finished, with \p text for the
  /// output window.
  void finished(const QString &text);

private:
  /// Whether this reported a change.
  bool setStage(Stage stage, double fraction);

  Stage stage_ = Stage::Idle;
  double fraction_ = -1;
  bool loaded_ = false;
  bool opening_ = false;
  /// Whether this file's initial analysis is still to be reported.
  bool reporting_ = false;
  QString file_, phase_, detail_;
  /// Since the file began to open, and since it loaded.
  QElapsedTimer clock_, loadedClock_;
  QTimer finishedTimer_;
};

/// A slim bar, as Visual Studio Code draws progress: a known fraction fills
/// it, and an unknown one sweeps a segment across it.  It animates only
/// while the fraction is unknown and the bar is on screen.
class ProgressStrip final : public QWidget {
  Q_OBJECT
public:
  explicit ProgressStrip(QWidget *parent = nullptr);

  /// The fraction to fill, or a negative value to sweep.
  void setFraction(double fraction);
  /// The time the segment takes to cross the bar.
  void setSweepTime(int milliseconds);
  bool animating() const { return frames_.isActive(); }
  QSize sizeHint() const override;

protected:
  void paintEvent(QPaintEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

private:
  void updateAnimation();

  double fraction_ = 0;
  int sweepTime_ = 0;
  QTimer frames_;
  QElapsedTimer sweep_;
};

/// The status bar's analysis indicator: the bar beside the AU text.
class AnalysisIndicator final : public QWidget {
  Q_OBJECT
public:
  explicit AnalysisIndicator(AnalysisProgress &progress,
                             QWidget *parent = nullptr);

  /// The tooltip while no file is opening.
  void setIdleToolTip(const QString &text);
  QString text() const;
  ProgressStrip *strip() const { return strip_; }
  /// Busy, room for the widest text; a narrow status bar may still shrink it
  /// to the text it shows.
  QSize sizeHint() const override;

protected:
  void changeEvent(QEvent *event) override;

private:
  void refresh();
  /// The room of the widest text it shows while busy.
  int busyWidth();

  AnalysisProgress &progress_;
  ProgressStrip *strip_ = nullptr;
  QLabel *label_ = nullptr;
  /// Hidden and styled as the text, it measures the texts the text may show.
  QLabel *measure_ = nullptr;
  /// Negative until measured.
  int busyWidth_ = -1;
  /// The room the text asks for: the busy width while busy, none idle.
  int reserved_ = 0;
  QString idleToolTip_;
};

/// While a file opens into an empty workbench, a card over its main view
/// names the file and the step, with a bar and the time spent.  It takes no
/// input and leaves every window usable, unlike a modal wait box; a quick
/// load finishes before it appears.
class LoadProgressCard final : public QFrame {
  Q_OBJECT
public:
  LoadProgressCard(AnalysisProgress &progress, QWidget &window);

  /// The view to center on while it is visible in the window; otherwise the
  /// card centers on the window.
  void setAnchor(QWidget *anchor);
  QString title() const;
  QString step() const;

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void refresh();
  void place();
  void tick();

  AnalysisProgress &progress_;
  QWidget &window_;
  QPointer<QWidget> anchor_;
  QLabel *title_ = nullptr;
  QLabel *step_ = nullptr;
  QLabel *elapsed_ = nullptr;
  ProgressStrip *strip_ = nullptr;
  QTimer delay_;
  QTimer clock_;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_ANALYSISPROGRESS_H
