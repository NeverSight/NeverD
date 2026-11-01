//===- AnalysisProgress.cpp - Non-blocking analysis progress --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Follow opening and background analysis in the status bar and over an
/// empty workbench, from the state the worker already reports.
//===----------------------------------------------------------------------===//

#include "AnalysisProgress.h"

#include "../Config.h"
#include "Theme.h"

#include <QEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {
namespace config = ::neverd::limits::gui_config;

/// The engine's phases that read, hash or copy the input as a whole; their
/// details name the file, which the card already shows.
bool readsTheFile(const QString &phase) {
  return phase == QLatin1String("image") ||
         phase == QLatin1String("identify") || phase == QLatin1String("read") ||
         phase == QLatin1String("hash") || phase == QLatin1String("copy");
}
} // namespace

//===----------------------------------------------------------------------===//
// AnalysisProgress
//===----------------------------------------------------------------------===//

AnalysisProgress::AnalysisProgress(QObject *parent) : QObject(parent) {
  finishedTimer_.setSingleShot(true);
  finishedTimer_.setInterval(config::kAnalysisProgressFinishedMs);
  connect(&finishedTimer_, &QTimer::timeout, this, [this] {
    if (stage_ == Stage::Finished)
      setStage(Stage::Idle, -1);
  });
}

void AnalysisProgress::update(bool opening, bool loaded, bool analysis,
                              const QString &file,
                              const QJsonObject &background) {
  bool shown = false;
  if (opening && !opening_) {
    // Each open has its own clock, steps and report.
    clock_.start();
    phase_.clear();
    detail_.clear();
    reporting_ = true;
    finishedTimer_.stop();
    shown = true;
  }
  if (opening && file != file_) {
    file_ = file;
    shown = true;
  }
  if (loaded != loaded_) {
    loaded_ = loaded;
    if (loaded)
      loadedClock_.start();
    shown = true;
  }
  opening_ = opening;
  Stage stage = Stage::Idle;
  double fraction = -1;
  if (opening) {
    stage = Stage::Loading;
    fraction = stage_ == Stage::Loading ? fraction_ : -1;
  } else if (loaded) {
    const auto state = background.value(QLatin1String("state")).toString();
    if (state == QLatin1String("building")) {
      // A cross-reference query builds the index even without idle-time
      // analysis.
      stage = Stage::Indexing;
      const double total = background.value(QLatin1String("total")).toDouble();
      if (total > 0)
        fraction = std::clamp(
            background.value(QLatin1String("done")).toDouble() / total, 0.0,
            1.0);
    } else if (analysis && (state == QLatin1String("pending") ||
                            (state.isEmpty() &&
                             loadedClock_.elapsed() <
                                 config::kAnalysisProgressReportWaitMs))) {
      // The worker reports its background state after the open; it finds
      // functions before it indexes their references.
      stage = background.value(QLatin1String("discovery_complete")).toBool()
                  ? Stage::Indexing
                  : Stage::Discovering;
    } else if (state == QLatin1String("ready") &&
               (std::exchange(reporting_, false) ||
                finishedTimer_.isActive())) {
      stage = Stage::Finished;
      fraction = 1;
    }
    if (!analysis || state == QLatin1String("unavailable"))
      reporting_ = false;
  } else {
    reporting_ = false;
  }
  const bool finishing = stage == Stage::Finished && stage_ != stage;
  if (!setStage(stage, fraction) && shown)
    emit changed();
  if (finishing) {
    finishedTimer_.start();
    emit finished(tr("Analysis finished in %1 s")
                      .arg(QLocale().toString(elapsedMs() / 1000.0, 'f', 1)));
  }
}

void AnalysisProgress::loadProgress(const QString &phase, double fraction,
                                    const QString &detail) {
  // Progress of a request other than the open the workbench waits for.
  if (!opening_)
    return;
  const bool shown = phase != phase_ || detail != detail_;
  phase_ = phase;
  detail_ = detail;
  if (!setStage(Stage::Loading, fraction) && shown)
    emit changed();
}

void AnalysisProgress::retranslate() { emit changed(); }

bool AnalysisProgress::setStage(Stage stage, double fraction) {
  // The bar and the text show whole percents.
  const auto percent = [](double value) {
    return value < 0 ? -1 : static_cast<int>(value * 100);
  };
  const bool same = stage == stage_ && percent(fraction) == percent(fraction_);
  stage_ = stage;
  fraction_ = fraction;
  if (same)
    return false;
  emit changed();
  return true;
}

QString AnalysisProgress::fileName() const {
  return QFileInfo(file_).fileName();
}

QString AnalysisProgress::statusText() const {
  return statusText(stage_,
                    fraction_ < 0 ? -1 : static_cast<int>(fraction_ * 100));
}

QString AnalysisProgress::statusText(Stage stage, int percent) {
  switch (stage) {
  case Stage::Idle:
    break;
  case Stage::Loading:
    return percent < 0 ? tr("AU: loading") : tr("AU: loading %1%").arg(percent);
  case Stage::Discovering:
    return tr("AU: finding functions");
  case Stage::Indexing:
    return percent < 0 ? tr("AU: indexing references")
                       : tr("AU: indexing references %1%").arg(percent);
  case Stage::Finished:
    return tr("AU: analysis finished");
  }
  return tr("AU: idle");
}

QString AnalysisProgress::stepText() const {
  if (readsTheFile(phase_))
    return tr("Reading the file");
  if (phase_ == QLatin1String("parse") || phase_ == QLatin1String("metadata") ||
      phase_ == QLatin1String("platform"))
    return tr("Parsing the format");
  if (phase_ == QLatin1String("load-discovery"))
    return tr("Finding code");
  if (phase_ == QLatin1String("debug") ||
      phase_ == QLatin1String("debug-apply"))
    return tr("Loading debug information");
  if (phase_.startsWith(QLatin1String("project-")))
    return tr("Opening the project");
  return tr("Loading");
}

QString AnalysisProgress::detail() const {
  return readsTheFile(phase_) ? QString() : detail_;
}

qint64 AnalysisProgress::elapsedMs() const {
  return clock_.isValid() ? clock_.elapsed() : 0;
}

//===----------------------------------------------------------------------===//
// ProgressStrip
//===----------------------------------------------------------------------===//

ProgressStrip::ProgressStrip(QWidget *parent)
    : QWidget(parent), sweepTime_(config::kAnalysisProgressSweepMs) {
  setFixedHeight(config::kAnalysisProgressStripHeight);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  frames_.setInterval(config::kAnalysisProgressFrameMs);
  connect(&frames_, &QTimer::timeout, this, [this] {
    // A minimized window shows nothing: keep the sweep's clock, skip the
    // frame.
    if (!window()->isMinimized())
      update();
  });
  // While the user works in another application the bar rests where it is;
  // the step and the seconds still change.
  connect(qApp, &QGuiApplication::applicationStateChanged, this,
          &ProgressStrip::updateAnimation);
  connect(&Theme::instance(), &Theme::changed, this,
          qOverload<>(&QWidget::update));
}

void ProgressStrip::setFraction(double fraction) {
  const int width = this->width();
  const auto filled = [width](double value) {
    return value < 0 ? -1 : static_cast<int>(std::min(value, 1.0) * width);
  };
  const bool redraw = filled(fraction) != filled(fraction_);
  fraction_ = fraction;
  updateAnimation();
  if (redraw)
    update();
}

void ProgressStrip::setSweepTime(int milliseconds) {
  sweepTime_ = milliseconds;
}

QSize ProgressStrip::sizeHint() const {
  return {config::kAnalysisProgressStripWidth,
          config::kAnalysisProgressStripHeight};
}

void ProgressStrip::paintEvent(QPaintEvent *) {
  const auto &theme = Theme::instance();
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen);
  const QRectF track = rect();
  const qreal radius = track.height() / 2;
  painter.setBrush(theme.chrome(QStringLiteral("Border")));
  painter.drawRoundedRect(track, radius, radius);
  QRectF fill = track;
  if (fraction_ >= 0) {
    fill.setWidth(track.width() * std::min(fraction_, 1.0));
  } else {
    // A segment enters at the left and leaves at the right.
    const qreal segment =
        track.width() * config::kAnalysisProgressSegmentPercent / 100.0;
    const qreal phase =
        (sweep_.elapsed() % sweepTime_) / static_cast<qreal>(sweepTime_);
    fill = QRectF(phase * (track.width() + segment) - segment, 0, segment,
                  track.height())
               .intersected(track);
  }
  if (fill.width() <= 0)
    return;
  // A right-to-left layout fills and sweeps from the right.
  if (isRightToLeft())
    fill.moveLeft(track.left() + track.right() - fill.right());
  painter.setBrush(theme.chrome(QStringLiteral("Accent")));
  painter.drawRoundedRect(fill, radius, radius);
}

void ProgressStrip::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  updateAnimation();
}

void ProgressStrip::hideEvent(QHideEvent *event) {
  QWidget::hideEvent(event);
  updateAnimation();
}

void ProgressStrip::updateAnimation() {
  const bool sweeping =
      fraction_ < 0 && isVisible() &&
      QGuiApplication::applicationState() == Qt::ApplicationActive;
  if (sweeping && !frames_.isActive()) {
    if (!sweep_.isValid())
      sweep_.start();
    frames_.start();
  } else if (!sweeping) {
    frames_.stop();
  }
}

//===----------------------------------------------------------------------===//
// AnalysisIndicator
//===----------------------------------------------------------------------===//

AnalysisIndicator::AnalysisIndicator(AnalysisProgress &progress,
                                     QWidget *parent)
    : QWidget(parent), progress_(progress) {
  setObjectName(QStringLiteral("analysisIndicator"));
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins({});
  layout->setSpacing(0);
  // The bar keeps its place while the text after it changes, and leaves the
  // status bar's free room to the texts beside it; in a narrow window it
  // gives up its own room before any text is cut.
  strip_ = new ProgressStrip(this);
  strip_->setMaximumWidth(config::kAnalysisProgressStripWidth);
  strip_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  label_ = new QLabel(this);
  measure_ = new QLabel(this);
  measure_->hide();
  layout->addWidget(strip_, 0, Qt::AlignVCenter);
  layout->addWidget(label_);
  connect(&progress_, &AnalysisProgress::changed, this,
          &AnalysisIndicator::refresh);
  refresh();
}

void AnalysisIndicator::setIdleToolTip(const QString &text) {
  idleToolTip_ = text;
  refresh();
}

QString AnalysisIndicator::text() const { return label_->text(); }

QSize AnalysisIndicator::sizeHint() const {
  auto size = QWidget::sizeHint();
  size.rwidth() += std::max(0, reserved_ - label_->sizeHint().width());
  return size;
}

void AnalysisIndicator::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  // The language, the font and the style change the texts' widths; they reach
  // the texts one widget at a time, so measure once all have.
  if (event->type() == QEvent::LanguageChange ||
      event->type() == QEvent::FontChange ||
      event->type() == QEvent::StyleChange)
    QMetaObject::invokeMethod(
        this,
        [this] {
          busyWidth_ = -1;
          refresh();
        },
        Qt::QueuedConnection);
}

int AnalysisIndicator::busyWidth() {
  using Stage = AnalysisProgress::Stage;
  if (busyWidth_ < 0) {
    busyWidth_ = 0;
    for (const auto stage :
         {Stage::Loading, Stage::Discovering, Stage::Indexing, Stage::Finished})
      for (const int percent : {-1, 100}) {
        measure_->setText(AnalysisProgress::statusText(stage, percent));
        busyWidth_ = std::max(busyWidth_, measure_->sizeHint().width());
      }
  }
  return busyWidth_;
}

void AnalysisIndicator::refresh() {
  const auto stage = progress_.stage();
  const bool busy = stage != AnalysisProgress::Stage::Idle;
  // Idle, the text keeps the place it has beside the other status texts.
  layout()->setContentsMargins(busy ? config::kAnalysisProgressStripMargin : 0,
                               0, 0, 0);
  strip_->setVisible(busy);
  strip_->setFraction(progress_.fraction());
  label_->setText(progress_.statusText());
  // Busy, the text asks for the room of its widest stage, so the status texts
  // after it keep their place while the stage and the percent change.
  const int reserved = busy ? busyWidth() : 0;
  if (reserved != reserved_) {
    reserved_ = reserved;
    updateGeometry();
  }
  if (stage == AnalysisProgress::Stage::Loading) {
    const auto detail = progress_.detail();
    setToolTip(detail.isEmpty() ? progress_.stepText()
                                : QStringLiteral("%1 · %2").arg(
                                      progress_.stepText(), detail));
  } else {
    setToolTip(idleToolTip_);
  }
}

//===----------------------------------------------------------------------===//
// LoadProgressCard
//===----------------------------------------------------------------------===//

LoadProgressCard::LoadProgressCard(AnalysisProgress &progress, QWidget &window)
    : QFrame(&window), progress_(progress), window_(window) {
  setObjectName(QStringLiteral("loadProgressCard"));
  // It reports; the workbench under it stays usable.
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setFixedWidth(config::kLoadProgressCardWidth);
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(
      config::kLoadProgressCardMargin, config::kLoadProgressCardMargin,
      config::kLoadProgressCardMargin, config::kLoadProgressCardMargin);
  layout->setSpacing(config::kLoadProgressCardSpacing);
  // Long names and details are cut short with an ellipsis rather than
  // widening the card.
  title_ = new QLabel(this);
  title_->setObjectName(QStringLiteral("loadProgressTitle"));
  title_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto *row = new QHBoxLayout;
  row->setSpacing(config::kLoadProgressCardSpacing);
  step_ = new QLabel(this);
  step_->setObjectName(QStringLiteral("loadProgressStep"));
  step_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  elapsed_ = new QLabel(this);
  elapsed_->setObjectName(QStringLiteral("loadProgressElapsed"));
  row->addWidget(step_, 1);
  row->addWidget(elapsed_);
  strip_ = new ProgressStrip(this);
  strip_->setSweepTime(config::kLoadProgressCardSweepMs);
  layout->addWidget(title_);
  layout->addLayout(row);
  layout->addWidget(strip_);
  hide();
  delay_.setSingleShot(true);
  delay_.setInterval(config::kLoadProgressCardDelayMs);
  connect(&delay_, &QTimer::timeout, this, [this] {
    if (!progress_.firstLoad())
      return;
    window_.installEventFilter(this);
    if (anchor_)
      anchor_->installEventFilter(this);
    show();
    raise();
    refresh();
    clock_.start();
  });
  clock_.setInterval(1000);
  connect(&clock_, &QTimer::timeout, this, &LoadProgressCard::tick);
  connect(&progress_, &AnalysisProgress::changed, this,
          &LoadProgressCard::refresh);
}

void LoadProgressCard::setAnchor(QWidget *anchor) {
  if (anchor_ && !isHidden())
    anchor_->removeEventFilter(this);
  anchor_ = anchor;
  if (anchor_ && !isHidden())
    anchor_->installEventFilter(this);
}

QString LoadProgressCard::title() const { return title_->text(); }

QString LoadProgressCard::step() const { return step_->text(); }

void LoadProgressCard::refresh() {
  // The card's own state, not its window's: an open can begin while the
  // workbench waits behind a question, and the card then shows with it.
  if (!progress_.firstLoad()) {
    delay_.stop();
    clock_.stop();
    if (!isHidden()) {
      window_.removeEventFilter(this);
      if (anchor_)
        anchor_->removeEventFilter(this);
      hide();
    }
    return;
  }
  if (isHidden()) {
    if (!delay_.isActive())
      delay_.start();
    return;
  }
  // A window narrower than the card narrows it, keeping a margin on each
  // side.
  const int cardWidth = std::min(config::kLoadProgressCardWidth,
                                 window_.contentsRect().width() -
                                     2 * config::kLoadProgressCardMargin);
  setFixedWidth(cardWidth);
  // The card's width less its margins; the labels' own widths are settled
  // only once the card has laid them out.
  const int width = cardWidth - 2 * config::kLoadProgressCardMargin;
  // A long name loses its middle and keeps its extension.
  const auto title = tr("Opening %1");
  const auto titleMetrics = title_->fontMetrics();
  const int around = titleMetrics.horizontalAdvance(
      QString(title).remove(QLatin1String("%1")));
  title_->setText(title.arg(titleMetrics.elidedText(
      progress_.fileName(), Qt::ElideMiddle, std::max(0, width - around))));
  const auto detail = progress_.detail();
  const QString step =
      detail.isEmpty()
          ? progress_.stepText()
          : QStringLiteral("%1 · %2").arg(progress_.stepText(), detail);
  tick();
  // The step shares its line with the time.
  const int room =
      width - config::kLoadProgressCardSpacing - elapsed_->sizeHint().width();
  step_->setText(step_->fontMetrics().elidedText(step, Qt::ElideRight, room));
  strip_->setFraction(progress_.fraction());
  place();
}

void LoadProgressCard::place() {
  adjustSize();
  QRect area = window_.rect();
  if (anchor_ && anchor_->isVisible() && anchor_->window() == &window_)
    area = QRect(anchor_->mapTo(&window_, QPoint()), anchor_->size());
  // Centered on the view, and wholly inside the window.
  const auto margin = config::kLoadProgressCardMargin;
  const QRect bounds =
      window_.contentsRect().adjusted(margin, margin, -margin, -margin);
  const QPoint corner = area.center() - QPoint(width() / 2, height() / 2);
  move(std::clamp(corner.x(), bounds.left(),
                  std::max(bounds.left(), bounds.right() + 1 - width())),
       std::clamp(corner.y(), bounds.top(),
                  std::max(bounds.top(), bounds.bottom() + 1 - height())));
}

void LoadProgressCard::tick() {
  elapsed_->setText(
      tr("%1 s").arg(QLocale().toString(progress_.elapsedMs() / 1000)));
}

bool LoadProgressCard::eventFilter(QObject *object, QEvent *event) {
  // The window's width sets the card's, and with it where the texts are cut.
  if (object == &window_ && event->type() == QEvent::Resize)
    refresh();
  else if ((object == &window_ || object == anchor_) &&
           (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
            event->type() == QEvent::Show || event->type() == QEvent::Hide))
    place();
  return QFrame::eventFilter(object, event);
}

} // namespace neverd::gui
