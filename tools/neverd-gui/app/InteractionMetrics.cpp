//===- InteractionMetrics.cpp - Painted interaction measurements ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Measure production views only after the requested content has arrived.
//===----------------------------------------------------------------------===//

#include "InteractionMetrics.h"

#include "../QueryService.h"
#include "../ResourceLimits.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "Session.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <memory>

namespace neverd::gui {
namespace {
constexpr int ReportSchema = 2;
/// Scroll ticks in one pass, one per 60 Hz frame.
constexpr int ScrollTicks = 300;
constexpr int FrameMs = 16;

/// Nearest-rank percentiles of \p ms.
QJsonObject summary(QVector<double> ms, int missed) {
  std::sort(ms.begin(), ms.end());
  const auto rank = [&](double q) {
    if (ms.isEmpty())
      return 0.0;
    const auto index = qsizetype(std::ceil(q * double(ms.size()))) - 1;
    return ms[std::clamp<qsizetype>(index, 0, ms.size() - 1)];
  };
  double total = 0;
  for (const double value : ms)
    total += value;
  return {{"samples", int(ms.size())},
          {"missed", missed},
          {"p50_ms", rank(0.50)},
          {"p95_ms", rank(0.95)},
          {"p99_ms", rank(0.99)},
          {"max_ms", ms.isEmpty() ? 0.0 : ms.last()},
          {"mean_ms", ms.isEmpty() ? 0.0 : total / double(ms.size())}};
}
} // namespace

InteractionMetrics::InteractionMetrics(MainWindow &window, Session &session,
                                       const QString &output, int timeoutMs,
                                       QObject *parent)
    : QObject(parent), window_(window), session_(session), output_(output),
      timeoutMs_(timeoutMs) {
  clock_.start();
  connect(
      &window_, &MainWindow::firstContentPainted, this,
      [this] {
        contentPainted_ = true;
        begin();
      },
      Qt::QueuedConnection);
  connect(&session_, &Session::stateChanged, this, &InteractionMetrics::begin,
          Qt::QueuedConnection);
  connect(&session_.queries(), &QueryService::analysisCompleted, this,
          &InteractionMetrics::begin, Qt::QueuedConnection);
  QTimer::singleShot(timeoutMs_, this,
                     [this] { finish(false, QStringLiteral("timeout")); });
}

void InteractionMetrics::begin() {
  if (finished_ || started_ || !contentPainted_ || !session_.analysisComplete())
    return;
  started_ = true;
  // The first frame may precede discovery. Its loader guesses and count are
  // not the completed function catalog, even after a later scroll warms it.
  session_.read(
      QStringLiteral("functions"), {{"limit", 1}, {"sort", "address"}}, this,
      [this](const QJsonObject &payload) {
        sampleFunctions(payload.value("total").toInt());
      },
      [this](const QString &code, const QString &) {
        finish(false, QStringLiteral("functions: ") + code);
      });
}

void InteractionMetrics::sampleFunctions(int count) {
  if (finished_)
    return;
  functionCount_ = count;
  if (count <= 0) {
    finish(false, QStringLiteral("no functions"));
    return;
  }
  // One page per sample, spread over the address-ordered list; its first
  // function that is not a thunk is the sample.
  const int wanted =
      std::min(count, ::neverd::limits::gui::kInteractionMetricsMaxSamples);
  auto found = std::make_shared<QVector<Address>>();
  auto pending = std::make_shared<int>(wanted);
  for (int i = 0; i < wanted; ++i) {
    session_.read(
        QStringLiteral("functions"),
        {{"offset", int(qint64(i) * count / wanted)},
         {"limit", 8},
         {"sort", "address"}},
        this,
        [this, found, pending](const QJsonObject &payload) {
          for (const auto &value : payload.value("items").toArray()) {
            const auto item = value.toObject();
            if (item.value("thunk").toBool())
              continue;
            if (const auto address = addressValue(item.value("address"))) {
              found->append(*address);
              break;
            }
          }
          if (--*pending)
            return;
          std::sort(found->begin(), found->end());
          found->erase(std::unique(found->begin(), found->end()), found->end());
          samples_ = *found;
          if (samples_.isEmpty()) {
            finish(false, QStringLiteral("no sampled functions"));
            return;
          }
          // Graphs come before pseudocode: an open pseudocode window would
          // follow every graph's function.
          runSteps({scrollStep("scroll_cold", 1), scrollStep("scroll_warm", -1),
                    jumpStep("jump_cold"), jumpStep("jump_warm"),
                    jumpStep("link_addresses", LinkResource::Addresses),
                    jumpStep("link_functions", LinkResource::Functions),
                    graphStep("graph_cold"), graphStep("graph_warm"),
                    pseudocodeStep("pseudocode_cold"),
                    pseudocodeStep("pseudocode_warm")});
        },
        [this](const QString &code, const QString &) {
          finish(false, QStringLiteral("functions: ") + code);
        });
  }
}

void InteractionMetrics::runSteps(QVector<Step> steps) {
  if (steps.isEmpty()) {
    finish(true);
    return;
  }
  const Step step = steps.takeFirst();
  step([this, steps] {
    QTimer::singleShot(0, this, [this, steps] { runSteps(steps); });
  });
}

InteractionMetrics::Step InteractionMetrics::scrollStep(const char *name,
                                                        int direction) {
  return [this, name, direction](Done done) {
    auto *disassembly = window_.disassembly();
    disassembly->setGraphMode(false);
    auto *listing = disassembly->listing();
    auto series = std::make_shared<Series>();
    auto tick = std::make_shared<int>(0);
    // The send time of the tick whose frame has not been flushed yet.
    auto waiting = std::make_shared<std::optional<qint64>>();
    const auto first = listing->topItem();
    auto *timer = new QTimer(this);
    timer->setTimerType(Qt::PreciseTimer);
    timer->setInterval(FrameMs);
    connect(timer, &QTimer::timeout, this, [=, this] {
      if (*waiting) {
        ++series->missed;
        waiting->reset();
      }
      if (*tick == ScrollTicks) {
        timer->stop();
        timer->deleteLater();
        record(name, *series);
        auto phase = phases_.value(QLatin1String(name)).toObject();
        phase["first_item"] = first ? hexAddress(*first) : QString();
        const auto last = listing->topItem();
        phase["last_item"] = last ? hexAddress(*last) : QString();
        phases_.insert(QLatin1String(name), phase);
        done();
        return;
      }
      ++*tick;
      const qint64 sent = clock_.nsecsElapsed();
      *waiting = sent;
      const QPointF inside(10, 10);
      QWheelEvent wheel(inside, listing->viewport()->mapToGlobal(inside),
                        QPoint(), QPoint(0, -120 * direction), Qt::NoButton,
                        Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(listing->viewport(), &wheel);
      afterPaint(listing->viewport(), [=, this] {
        if (*waiting && **waiting == sent) {
          series->ms.append((clock_.nsecsElapsed() - sent) / 1.0e6);
          waiting->reset();
        }
      });
    });
    timer->start();
  };
}

InteractionMetrics::Step
InteractionMetrics::jumpStep(const char *name,
                             std::optional<LinkResource> resource) {
  return [this, name, resource](Done done) {
    auto *disassembly = window_.disassembly();
    disassembly->setGraphMode(false);
    auto *listing = disassembly->listing();
    auto series = std::make_shared<Series>();
    const int count =
        int(samples_.size()) *
        (resource ? ::neverd::limits::gui::kInteractionMetricsLinkPasses : 1);
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [=, this](int index) {
      if (index == count) {
        record(name, *series);
        done();
        return;
      }
      const Address address = samples_[index % samples_.size()];
      QString uri;
      if (resource) {
        DeepLink link;
        link.scheme = QStringLiteral("ida");
        link.input = session_.fileName();
        link.resource = *resource;
        link.ea = address;
        link.view = LinkView::Disasm;
        uri = formatDeepLink(link);
      }
      const qint64 start = clock_.nsecsElapsed();
      auto over = std::make_shared<bool>(false);
      auto settled = std::make_shared<QMetaObject::Connection>();
      auto *limit = new QTimer(this);
      limit->setSingleShot(true);
      const auto complete = [=, this](const QString &failure) {
        if (*over)
          return;
        *over = true;
        disconnect(*settled);
        limit->stop();
        limit->deleteLater();
        QJsonObject sample{{"address", hexAddress(address)}};
        if (!uri.isEmpty())
          sample["uri"] = uri;
        if (failure.isEmpty()) {
          const double ms = (clock_.nsecsElapsed() - start) / 1.0e6;
          series->ms.append(ms);
          sample["ms"] = ms;
        } else {
          ++series->missed;
          sample["missed"] = failure;
        }
        series->detail.append(sample);
        QTimer::singleShot(0, this, [next, index] { (*next)(index + 1); });
      };
      *settled = connect(listing, &ListingView::jumpSettled, this, [=, this] {
        disconnect(*settled);
        afterPaint(listing->viewport(), [=] {
          complete(!listing->jumpPending() && listing->currentItem() == address
                       ? QString()
                       : QStringLiteral("listing did not reach the target"));
        });
      });
      connect(limit, &QTimer::timeout, this,
              [complete] { complete(QStringLiteral("timeout")); });
      limit->start(::neverd::limits::gui::kInteractionMetricsSampleTimeoutMs);
      // What a jump does: navigate the disassembly, which the other views
      // follow.
      if (resource) {
        const auto parsed = parseDeepLink(uri);
        if (!parsed.link) {
          complete(parsed.error);
          return;
        }
        window_.openLink(*parsed.link, [complete](const QString &error) {
          if (!error.isEmpty())
            complete(error);
        });
      } else {
        disassembly->navigate(address);
      }
    };
    (*next)(0);
  };
}

InteractionMetrics::Step InteractionMetrics::graphStep(const char *name) {
  return [this, name](Done done) {
    auto *disassembly = window_.disassembly();
    disassembly->setGraphMode(true);
    auto *graph = disassembly->graph();
    auto series = std::make_shared<Series>();
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [=, this](int index) {
      if (index == samples_.size()) {
        disassembly->setGraphMode(false);
        record(name, *series);
        done();
        return;
      }
      const qint64 start = clock_.nsecsElapsed();
      auto over = std::make_shared<bool>(false);
      auto status = std::make_shared<QMetaObject::Connection>();
      auto *limit = new QTimer(this);
      limit->setSingleShot(true);
      const auto complete = [=, this](const QString &failure) {
        if (*over)
          return;
        *over = true;
        disconnect(*status);
        limit->stop();
        limit->deleteLater();
        QJsonObject sample{{"address", hexAddress(samples_[index])}};
        if (failure.isEmpty()) {
          const double ms = (clock_.nsecsElapsed() - start) / 1.0e6;
          series->ms.append(ms);
          sample["ms"] = ms;
        } else {
          ++series->missed;
          sample["missed"] = failure;
        }
        series->detail.append(sample);
        QTimer::singleShot(0, this, [next, index] { (*next)(index + 1); });
      };
      // The view reports a complete layout with an empty status and an
      // error with its message; it announces the request itself first.
      const QString laying = GraphView::tr("Laying out graph…");
      *status = connect(graph, &GraphView::statusChanged, this,
                        [=, this](const QString &text) {
                          if (text == laying)
                            return;
                          disconnect(*status);
                          if (!text.isEmpty()) {
                            complete(text);
                            return;
                          }
                          afterPaint(graph, [complete] { complete({}); });
                        });
      connect(limit, &QTimer::timeout, this,
              [complete] { complete(QStringLiteral("timeout")); });
      limit->start(::neverd::limits::gui::kInteractionMetricsSampleTimeoutMs);
      graph->showFunction(samples_[index]);
    };
    (*next)(0);
  };
}

InteractionMetrics::Step InteractionMetrics::pseudocodeStep(const char *name) {
  return [this, name](Done done) {
    const auto open = [this, name, done] {
      // F5 opens the pseudocode window on the current function.
      if (auto *action =
              window_.findChild<QAction *>(QStringLiteral("ViewPseudocode")))
        action->trigger();
      QTimer::singleShot(0, this,
                         [this, name, done] { measurePseudocode(name, done); });
    };
    // Leaving the graph starts a listing jump. F5 itself defers until that
    // jump lands, so its window cannot be measured on the next timer yet.
    auto *listing = window_.disassembly()->listing();
    if (listing->jumpPending())
      connect(listing, &ListingView::jumpSettled, this, open,
              Qt::SingleShotConnection);
    else
      open();
  };
}

void InteractionMetrics::measurePseudocode(const char *name, Done done) {
  {
    CodeView *code = nullptr;
    for (auto *view : window_.findChildren<CodeView *>())
      if (view->isVisible() && CodeView::isSource(view->representation())) {
        code = view;
        break;
      }
    if (!code) {
      phases_.insert(QLatin1String(name),
                     QJsonObject{{"error", "no pseudocode window"}});
      done();
      return;
    }
    auto *text = code->text();
    auto series = std::make_shared<Series>();
    auto next = std::make_shared<std::function<void(int)>>();
    *next = [=, this](int index) {
      if (index == samples_.size()) {
        record(name, *series);
        done();
        return;
      }
      const qint64 start = clock_.nsecsElapsed();
      auto over = std::make_shared<bool>(false);
      auto status = std::make_shared<QMetaObject::Connection>();
      auto *limit = new QTimer(this);
      limit->setSingleShot(true);
      const auto complete = [=, this](const QString &failure) {
        if (*over)
          return;
        *over = true;
        disconnect(*status);
        limit->stop();
        limit->deleteLater();
        QJsonObject sample{{"address", hexAddress(samples_[index])}};
        if (failure.isEmpty()) {
          const double ms = (clock_.nsecsElapsed() - start) / 1.0e6;
          series->ms.append(ms);
          sample["ms"] = ms;
        } else {
          ++series->missed;
          sample["missed"] = failure;
        }
        series->detail.append(sample);
        QTimer::singleShot(0, this, [next, index] { (*next)(index + 1); });
      };
      auto issued = std::make_shared<bool>(false);
      const auto observe = [=, this] {
        // load() first cancels the preceding document, which emits a settled
        // status before the new request exists. Only its terminal state counts.
        if (!*issued || text->loading() || text->function() != samples_[index])
          return;
        disconnect(*status);
        if (!text->complete()) {
          complete(text->status().isEmpty()
                       ? QStringLiteral("pseudocode did not complete")
                       : text->status());
          return;
        }
        const quint64 version = text->documentVersion();
        afterPaint(text->viewport(), [=, this] {
          complete(text->function() == samples_[index] && text->complete() &&
                           text->documentVersion() == version
                       ? QString()
                       : QStringLiteral("pseudocode changed before painting"));
        });
      };
      *status = connect(text, &CodeText::statusChanged, this, observe);
      connect(limit, &QTimer::timeout, this,
              [complete] { complete(QStringLiteral("timeout")); });
      limit->start(::neverd::limits::gui::kInteractionMetricsSampleTimeoutMs);
      code->showFunction(samples_[index]);
      *issued = true;
      observe();
    };
    (*next)(0);
  }
}

void InteractionMetrics::afterPaint(QWidget *view, Done then) {
  if (paintView_)
    paintView_->removeEventFilter(this);
  paintView_ = view;
  paintThen_ = std::move(then);
  view->installEventFilter(this);
  view->update();
}

bool InteractionMetrics::eventFilter(QObject *object, QEvent *event) {
  if (object == paintView_ && event->type() == QEvent::Paint && paintThen_) {
    paintView_->removeEventFilter(this);
    paintView_.clear();
    // The view paints during this event; the backing store is flushed
    // before the event loop runs the next timer.
    QTimer::singleShot(0, this, [then = std::move(paintThen_)] { then(); });
    paintThen_ = nullptr;
  }
  return QObject::eventFilter(object, event);
}

void InteractionMetrics::record(const char *name, const Series &series) {
  auto phase = summary(series.ms, series.missed);
  phase["analysis_complete"] = session_.analysisComplete();
  const auto *viewport = window_.disassembly()->listing()->viewport();
  phase["listing_viewport"] = QJsonArray{viewport->width(), viewport->height()};
  if (!series.detail.isEmpty())
    phase["detail"] = series.detail;
  phases_.insert(QLatin1String(name), phase);
}

void InteractionMetrics::finish(bool success, const QString &reason) {
  if (finished_)
    return;
  finished_ = true;
  QJsonArray samples;
  for (const Address address : samples_)
    samples.append(hexAddress(address));
  QJsonObject report{
      {"schema_version", ReportSchema},
      {"success", success},
      {"failure_reason", reason},
      {"timeout_ms", timeoutMs_},
      {"sample_timeout_ms",
       ::neverd::limits::gui::kInteractionMetricsSampleTimeoutMs},
      {"qt_version", qVersion()},
      {"platform_plugin", QApplication::platformName()},
      {"os", QSysInfo::prettyProductName()},
      {"architecture", QSysInfo::currentCpuArchitecture()},
      {"file", session_.metadata().value("path")},
      {"input_sha256", session_.metadata().value("sha256")},
      {"function_count", functionCount_},
      {"samples", samples},
      {"phases", phases_},
      {"error", session_.lastError()},
      {"measurement",
       "From the input or request to the backing-store flush after the view "
       "painted the content; Qt Widgets paint, not hardware presentation. "
       "Scroll ticks are wheel steps of three lines every 16 ms; a tick "
       "without a frame before the next one is missed."}};
  QSaveFile file(output_);
  const auto bytes = QJsonDocument(report).toJson();
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
      !file.commit()) {
    qCritical("Could not write interaction benchmark report");
    QTimer::singleShot(0, this, [] { QCoreApplication::exit(2); });
    return;
  }
  QTimer::singleShot(0, this,
                     [success] { QCoreApplication::exit(success ? 0 : 1); });
}

} // namespace neverd::gui
