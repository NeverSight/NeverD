#include <QAbstractListModel>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSGSimpleRectNode>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <vector>
#ifdef Q_OS_MACOS
#include <mach/mach.h>
#elif defined(Q_OS_WIN)
#include <psapi.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
qint64 now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
qint64 residentBytes() {
#ifdef Q_OS_MACOS
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t size = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                reinterpret_cast<task_info_t>(&info), &size) == KERN_SUCCESS)
    return static_cast<qint64>(info.resident_size);
#elif defined(Q_OS_WIN)
  PROCESS_MEMORY_COUNTERS info{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info)))
    return static_cast<qint64>(info.WorkingSetSize);
#else
  std::ifstream file("/proc/self/statm");
  qint64 total = 0, resident = 0;
  if (file >> total >> resident)
    return resident * sysconf(_SC_PAGESIZE);
#endif
  return -1;
}
QJsonObject distribution(std::vector<double> values) {
  if (values.empty())
    return {};
  std::sort(values.begin(), values.end());
  auto percentile = [&](double p) {
    return values.at(std::max<std::size_t>(1, std::ceil(values.size() * p)) -
                     1);
  };
  return {{"samples", int(values.size())}, {"p50_ms", percentile(.50)},
          {"p95_ms", percentile(.95)},     {"p99_ms", percentile(.99)},
          {"min_ms", values.front()},      {"max_ms", values.back()}};
}
QString backendName(QSGRendererInterface::GraphicsApi api) {
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
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  case QSGRendererInterface::Direct3D12:
    return "Direct3D12";
#endif
  default:
    return "unknown";
  }
}
} // namespace

// Functions expose the full sparse row count; text uses a fixed local window
// over a 64-bit logical source. Addresses are always formatted in C++.
class WindowModel final : public QAbstractListModel {
  Q_OBJECT
public:
  enum { RowText = Qt::UserRole + 1 };
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : text_ ? 512 : logicalCount_;
  }
  QHash<int, QByteArray> roleNames() const override {
    return {{RowText, "rowText"}};
  }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || role != RowText)
      return {};
    ++requests;
    const quint64 row = start_ + static_cast<unsigned>(index.row());
    const auto address = QStringLiteral("0x%1").arg(
        0xffff800000000000ULL + row * 16, 16, 16, QLatin1Char('0'));
    return text_ ? QStringLiteral(
                       "%1   mov   rax, [rbx + 0x10]   ; logical line %2")
                       .arg(address)
                       .arg(row)
                 : QStringLiteral("%1   function_%2                      0x40")
                       .arg(address)
                       .arg(row, 8, 10, QLatin1Char('0'));
  }
  void move(quint64 start, bool text, int count) {
    if (text_ != text || logicalCount_ != count) {
      beginResetModel();
      text_ = text;
      logicalCount_ = count;
      start_ = text ? start : 0;
      endResetModel();
    } else if (text_) {
      start_ = start;
      emit dataChanged(index(0), index(511), {RowText});
    }
  }
  mutable quint64 requests = 0;

private:
  quint64 start_ = 0;
  bool text_ = false;
  int logicalCount_ = 100000;
};

class GridGraph : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(
      int logicalNodes READ logicalNodes WRITE setLogicalNodes NOTIFY changed)
  Q_PROPERTY(int panX READ panX WRITE setPanX NOTIFY changed)
  Q_PROPERTY(int panY READ panY WRITE setPanY NOTIFY changed)
public:
  GridGraph() { setFlag(ItemHasContents); }
  int logicalNodes() const { return count_; }
  int panX() const { return x_; }
  int panY() const { return y_; }
  void setLogicalNodes(int value) {
    count_ = value;
    emit changed();
    update();
  }
  void setPanX(int value) {
    x_ = value;
    emit changed();
    update();
  }
  void setPanY(int value) {
    y_ = value;
    emit changed();
    update();
  }
  static inline std::atomic<int> peakVisibleNodes = 0, peakSceneNodes = 0;
signals:
  void changed();

protected:
  QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override {
    delete old;
    auto *root = new QSGNode;
    const int columns = static_cast<int>(std::ceil(std::sqrt(double(count_))));
    int visible = 0, sceneNodes = 0;
    // Analytical grid index; work scales with the viewport, not total nodes.
    const int firstColumn = std::max(0, x_ / 180 - 1);
    const int firstRow = std::max(0, y_ / 96 - 1);
    const int lastColumn = std::min(columns - 1, int((x_ + width()) / 180) + 1);
    const int lastRow = int((y_ + height()) / 96) + 1;
    for (int row = firstRow; row <= lastRow; ++row) {
      for (int column = firstColumn; column <= lastColumn; ++column) {
        if (row * columns + column >= count_)
          continue;
        const QRectF rect(column * 180 - x_, row * 96 - y_, 140, 56);
        if (!rect.intersects(boundingRect()))
          continue;
        ++visible;
        root->appendChildNode(new QSGSimpleRectNode(rect, QColor("#264f78")));
        ++sceneNodes;
        // Grid edges are overview geometry, not fully rendered CFG text.
        if (column + 1 < columns && row * columns + column + 1 < count_) {
          root->appendChildNode(new QSGSimpleRectNode(
              QRectF(rect.right(), rect.center().y(), 40, 1),
              QColor("#569cd6")));
          ++sceneNodes;
        }
        if ((row + 1) * columns + column < count_) {
          root->appendChildNode(new QSGSimpleRectNode(
              QRectF(rect.center().x(), rect.bottom(), 1, 40),
              QColor("#6a9955")));
          ++sceneNodes;
        }
      }
    }
    peakVisibleNodes.store(std::max(peakVisibleNodes.load(), visible));
    peakSceneNodes.store(std::max(peakSceneNodes.load(), sceneNodes));
    return root;
  }

private:
  int count_ = 1000, x_ = 0, y_ = 0;
};

class Benchmark final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool graphMode READ graphMode NOTIFY changed)
  Q_PROPERTY(int logicalCount READ logicalCount NOTIFY changed)
  Q_PROPERTY(int scrollRow READ scrollRow NOTIFY changed)
  Q_PROPERTY(int panX READ panX NOTIFY changed)
  Q_PROPERTY(int panY READ panY NOTIFY changed)
public:
  Benchmark(WindowModel &model, int samples, QString output)
      : model_(model), samples_(samples), output_(std::move(output)) {}
  bool graphMode() const { return case_ >= 3; }
  int logicalCount() const { return counts_[case_]; }
  int scrollRow() const { return scroll_; }
  int panX() const { return panX_; }
  int panY() const { return panY_; }
  Q_INVOKABLE void delegateCreated() {
    ++live_;
    peakLive_ = std::max(peakLive_, live_);
  }
  Q_INVOKABLE void delegateDestroyed() { --live_; }
  void attach(QQuickWindow *window) {
    window_ = window;
    report_ = {{"schema", 1},
               {"kind", "synthetic-qt-viewport"},
               {"qt_version", QT_VERSION_STR},
               {"platform", QSysInfo::prettyProductName()},
               {"architecture", QSysInfo::currentCpuArchitecture()},
               {"platform_plugin", QGuiApplication::platformName()},
               {"window_width", window->width()},
               {"window_height", window->height()},
               {"device_pixel_ratio", window->devicePixelRatio()},
               {"build_type", BENCH_BUILD_TYPE},
               {"seed", 3389},
               {"text_window_rows", 512},
               {"font_family",
                QFontDatabase::systemFont(QFontDatabase::FixedFont).family()},
               {"font_pixel_size", 14},
               {"requested_render_loop",
                qEnvironmentVariable("QSG_RENDER_LOOP", "Qt default")},
               {"rss_start_bytes", residentBytes()}};
    connect(
        window, &QQuickWindow::beforeRendering, this,
        [this] { renderStart_ = now(); }, Qt::DirectConnection);
    connect(
        window, &QQuickWindow::afterRendering, this,
        [this] { renderCost_ = now() - renderStart_.load(); },
        Qt::DirectConnection);
    connect(window, &QQuickWindow::frameSwapped, this, &Benchmark::frameDone,
            Qt::QueuedConnection);
    QTimer::singleShot(100, this, &Benchmark::advance);
    QTimer::singleShot(120000, this, [] {
      qCritical("benchmark timed out: no rendering progress");
      QCoreApplication::exit(2);
    });
  }
signals:
  void changed();

private:
  void advance() {
    requestAt_ = now();
    const quint64 seed = quint64(frame_ + 1) * 2654435761ULL + 3389;
    const quint64 trace = seed % (logicalCount() - 512);
    const qint64 begin = now();
    if (!graphMode()) {
      model_.move(trace, case_ == 2, logicalCount());
      scroll_ = case_ == 2 ? (frame_ * 7) % 192 : int(trace);
    } else {
      const int columns = int(std::ceil(std::sqrt(double(logicalCount()))));
      panX_ = int(seed % unsigned(std::max(1, columns * 180 - 1100)));
      panY_ = int((seed / 97) % unsigned(std::max(1, columns * 96 - 700)));
    }
    emit changed();
    if (frame_ >= Warmup)
      modelTimes_.push_back((now() - begin) / 1e6);
    const auto posted = now();
    QMetaObject::invokeMethod(
        this,
        [this, posted] {
          if (frame_ >= Warmup)
            queueTimes_.push_back((now() - posted) / 1e6);
        },
        Qt::QueuedConnection);
    pending_ = true;
    window_->update();
  }
  void frameDone() {
    if (!pending_)
      return;
    pending_ = false;
    const auto timestamp = now();
    if (frame_ >= Warmup) {
      if (lastFrame_)
        frameIntervals_.push_back((timestamp - lastFrame_) / 1e6);
      requestTimes_.push_back((timestamp - requestAt_) / 1e6);
      renderTimes_.push_back(renderCost_.load() / 1e6);
    }
    lastFrame_ = timestamp;
    ++frame_;
    if (frame_ == Warmup)
      peakLive_ = live_;
    if (frame_ < Warmup + samples_) {
      QTimer::singleShot(0, this, &Benchmark::advance);
      return;
    }
    QJsonObject result{
        {"scenario", names_[case_]},
        {"logical_count", logicalCount()},
        {"warmup_frames", Warmup},
        {"frames", samples_},
        {"qt_model_rows", graphMode() ? 0 : model_.rowCount()},
        {"qt_frame_swapped_interval", distribution(frameIntervals_)},
        {"request_to_frame_swapped", distribution(requestTimes_)},
        {"model_update", distribution(modelTimes_)},
        {"queued_dispatch", distribution(queueTimes_)},
        {"before_to_after_rendering", distribution(renderTimes_)},
        {"peak_live_qml_row_delegates", peakLive_},
        {"peak_visible_graph_nodes", GridGraph::peakVisibleNodes.load()},
        {"peak_graph_scene_nodes", GridGraph::peakSceneNodes.load()},
        {"model_data_requests", double(model_.requests)},
        {"rss_end_bytes", residentBytes()}};
    result["bounded_objects_observed"] =
        peakLive_ <= 96 && GridGraph::peakSceneNodes <= 256;
    cases_.append(result);
    if (++case_ == 5) {
      finish();
      return;
    }
    frame_ = 0;
    lastFrame_ = 0;
    frameIntervals_.clear();
    requestTimes_.clear();
    modelTimes_.clear();
    queueTimes_.clear();
    renderTimes_.clear();
    peakLive_ = live_;
    model_.requests = 0;
    GridGraph::peakVisibleNodes = 0;
    GridGraph::peakSceneNodes = 0;
    emit changed();
    QTimer::singleShot(50, this, &Benchmark::advance);
  }
  void finish() {
    report_["graphics_api"] =
        backendName(window_->rendererInterface()->graphicsApi());
    report_["rendering_thread_is_gui"] = renderOnGui_.load();
    report_["cases"] = cases_;
    report_["caveats"] = QJsonArray{
        "Synthetic data/windowing harness, not the production workbench and "
        "not a real recovered million-function image.",
        "frameSwapped is a Qt signal, not independently measured hardware "
        "presentation; offscreen runs are not display-frame acceptance.",
        "queued_dispatch is synthetic event-loop delivery, not "
        "keyboard/IME/input-to-display latency.",
        "Graph workload is analytical grid culling and overview "
        "rectangles/edges; it excludes graph layout and full CFG text.",
        "RSS covers this Qt process only, not a simultaneous worker, plugins, "
        "private/PSS memory or GPU allocations.",
        "No P0 acceptance decision; compare same hardware, Qt version, "
        "backend, viewport, font and build configuration."};
    QFile output(output_);
    if (!output.open(QIODevice::WriteOnly)) {
      qCritical("Cannot write report");
      QCoreApplication::exit(2);
      return;
    }
    output.write(QJsonDocument(report_).toJson());
    output.close();
    qInfo().noquote() << output_;
    QCoreApplication::quit();
  }

public:
  void setRenderThread(bool gui) { renderOnGui_ = gui; }

private:
  static constexpr int Warmup = 20;
  static constexpr int counts_[]{100000, 1000000, 10000000, 1000, 10000};
  const QString names_[5]{
      "100k-functions-sparse-model", "1m-functions-sparse-model",
      "10m-logical-text-window", "1k-graph-overview", "10k-graph-overview"};
  WindowModel &model_;
  QQuickWindow *window_ = nullptr;
  int samples_, case_ = 0, frame_ = 0, scroll_ = 0, panX_ = 0, panY_ = 0,
                live_ = 0, peakLive_ = 0;
  QString output_;
  bool pending_ = false;
  qint64 requestAt_ = 0, lastFrame_ = 0;
  std::atomic<qint64> renderStart_{0}, renderCost_{0};
  std::atomic<bool> renderOnGui_{false};
  std::vector<double> frameIntervals_, requestTimes_, modelTimes_, queueTimes_,
      renderTimes_;
  QJsonObject report_;
  QJsonArray cases_;
};

int main(int argc, char **argv) {
  QGuiApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addHelpOption();
  parser.addOption(
      {"output", "JSON report output", "path", "viewport-report.json"});
  parser.addOption({"samples",
                    "Measured frames per case (20 warmup frames additional)",
                    "count", "120"});
  parser.process(app);
  bool valid = false;
  const int samples = parser.value("samples").toInt(&valid);
  if (!valid || samples < 20 || samples > 10000) {
    qCritical("samples must be 20–10000");
    return 2;
  }
  qmlRegisterType<GridGraph>("NeverDBenchmark", 1, 0, "GridGraph");
  WindowModel rows;
  Benchmark benchmark(rows, samples, parser.value("output"));
  QQmlEngine engine;
  engine.rootContext()->setContextProperty("bench", &benchmark);
  engine.rootContext()->setContextProperty("rows", &rows);
  engine.rootContext()->setContextProperty(
      "codeFont", QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  QQmlComponent component(&engine);
  component.setData(R"QML(
import QtQuick
import QtQuick.Window
import NeverDBenchmark 1.0
Window {
    width: 1100; height: 700; visible: true; color: "#1e1e1e"
    title: "NeverD synthetic viewport benchmark"
    Loader { anchors.fill: parent; sourceComponent: bench.graphMode ? graph : table }
    Component { id: table
        ListView {
            clip: true; model: rows; cacheBuffer: 0; reuseItems: true
            contentY: bench.scrollRow * 22
            delegate: Rectangle {
                required property string rowText
                width: ListView.view.width; height: 22; color: "#1e1e1e"
                Component.onCompleted: bench.delegateCreated()
                Component.onDestruction: bench.delegateDestroyed()
                Text { anchors.verticalCenter: parent.verticalCenter; x: 12; text: rowText
                       color: "#d4d4d4"; font.family: codeFont; font.pixelSize: 14 }
            }
        }
    }
    Component { id: graph
        GridGraph { clip: true; logicalNodes: bench.logicalCount; panX: bench.panX; panY: bench.panY }
    }
}
)QML",
                    QUrl("qrc:/NeverDBenchmark.qml"));
  std::unique_ptr<QObject> root(component.create());
  if (!root) {
    qCritical().noquote() << component.errorString();
    return 2;
  }
  auto *window = qobject_cast<QQuickWindow *>(root.get());
  if (!window)
    return 2;
  QObject::connect(
      window, &QQuickWindow::beforeRendering, &benchmark,
      [&] {
        benchmark.setRenderThread(QThread::currentThread() == app.thread());
      },
      Qt::DirectConnection);
  benchmark.attach(window);
  return app.exec();
}

#include "viewport_bench.moc"
