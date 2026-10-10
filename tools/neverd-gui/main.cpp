#include "app/DisassemblyView.h"
#include "app/Docking.h"
#include "app/GnomeModalDialogs.h"
#include "app/GnomeWindowEdges.h"
#include "app/InteractionMetrics.h"
#include "app/Language.h"
#include "app/ListingView.h"
#include "app/MainWindow.h"
#include "app/Session.h"
#include "app/SettingsKeys.h"
#include "app/StartupMetrics.h"
#include "app/Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

#ifdef NEVERD_GUI_TEST_PROBES
#include "tests/WidgetsProbe.h"
#endif

using namespace neverd::gui;

namespace {
// The second version: the first remembered windows that GNOME maximized
// because they opened at nearly the size of the screen.
constexpr char GeometryKey[] = "ui/geometry2";
constexpr int CaptureDelayMs = 6000;
constexpr int SmokeDelayMs = 1500;
constexpr int DefaultWidth = 1600;
constexpr int DefaultHeight = 1000;
/// The share of the screen a window opens at without a remembered geometry:
/// below the 80% of its area at which GNOME maximizes a new window, whose
/// edges then no longer resize it.
constexpr int DefaultWidthPercent = 75;
constexpr int DefaultHeightPercent = 80;

void restoreGeometry(MainWindow &window) {
  const auto geometry = QSettings().value(GeometryKey).toByteArray();
  if (!geometry.isEmpty() && window.restoreGeometry(geometry))
    return;
  const QRect available = window.screen()->availableGeometry();
  const QSize size(
      std::min(DefaultWidth, available.width() * DefaultWidthPercent / 100),
      std::min(DefaultHeight, available.height() * DefaultHeightPercent / 100));
  window.resize(size);
  window.move(available.center() - QPoint(size.width() / 2, size.height() / 2));
}
} // namespace

int main(int argc, char **argv) {
  QElapsedTimer startupClock;
  startupClock.start();
  QApplication::setOrganizationName(QStringLiteral("NeverSight"));
  QApplication::setOrganizationDomain(QStringLiteral("neversight.dev"));
  QApplication::setApplicationName(QStringLiteral("NeverD"));
  QApplication::setApplicationVersion(QStringLiteral("3389.0.1"));
  gnome::prepareModalDialogs();
  QApplication app(argc, argv);
  const auto applicationCreatedMs = startupClock.nsecsElapsed() / 1.0e6;
  gnome::detachModalDialogs(app);
  configureDocking();
  const auto dockingFrontendMs = startupClock.nsecsElapsed() / 1.0e6;

  QCommandLineParser parser;
  parser.setApplicationDescription(
      QStringLiteral("NeverD interactive disassembler and decompiler"));
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument(QStringLiteral("binary"),
                               QStringLiteral("Binary file to open"),
                               QStringLiteral("[binary]"));
  parser.addOption({QStringLiteral("worker"),
                    QStringLiteral("Path to the matching analysis worker"),
                    QStringLiteral("path")});
  parser.addOption({QStringLiteral("capture"),
                    QStringLiteral("Save a workbench screenshot after loading"),
                    QStringLiteral("path")});
  parser.addOption({QStringLiteral("capture-delay"),
                    QStringLiteral("Milliseconds before --capture"),
                    QStringLiteral("milliseconds"),
                    QString::number(CaptureDelayMs)});
  parser.addOption(
      {QStringLiteral("smoke-test"),
       QStringLiteral("Exit after loading the UI (for build verification)")});
  parser.addOption({QStringLiteral("fresh-layout"),
                    QStringLiteral("Use a temporary workbench layout")});
  parser.addOption(
      {QStringLiteral("command"),
       QStringLiteral("Run an output-window command line after the binary "
                      "opens, such as \"g main\" or \"graph\"; repeatable"),
       QStringLiteral("line")});
  parser.addOption(
      {QStringLiteral("startup-benchmark"),
       QStringLiteral("Write startup milestones as JSON and exit after a "
                      "useful frame"),
       QStringLiteral("path")});
  parser.addOption(
      {QStringLiteral("startup-benchmark-timeout"),
       QStringLiteral("Startup benchmark deadline from main entry in "
                      "milliseconds"),
       QStringLiteral("milliseconds"), QStringLiteral("30000")});
  parser.addOption(
      {QStringLiteral("interaction-benchmark"),
       QStringLiteral("Scroll, jump, open graphs and pseudocode in the opened "
                      "file, write their latencies as JSON and exit"),
       QStringLiteral("path")});
  parser.addOption(
      {QStringLiteral("interaction-benchmark-timeout"),
       QStringLiteral("Interaction benchmark deadline in milliseconds"),
       QStringLiteral("milliseconds"), QStringLiteral("600000")});
#ifdef NEVERD_GUI_TEST_PROBES
  parser.addOption(
      {QStringLiteral("widgets-test"),
       QStringLiteral("Exercise the workbench on a fixture and exit")});
  parser.addOption({QStringLiteral("analysis-test"),
                    QStringLiteral("Exercise the workbench on a binary with a "
                                   "function named main and exit"),
                    QStringLiteral("binary")});
#endif
  parser.process(app);
  const auto optionsParsedMs = startupClock.nsecsElapsed() / 1.0e6;

  QString worker = parser.value(QStringLiteral("worker"));
  if (worker.isEmpty())
    worker = QDir(QCoreApplication::applicationDirPath())
                 .filePath(
#ifdef Q_OS_WIN
                     QStringLiteral("neverd-worker.exe")
#else
                     QStringLiteral("neverd-worker")
#endif
                 );
  const bool interaction =
      parser.isSet(QStringLiteral("interaction-benchmark"));
  bool automated = parser.isSet(QStringLiteral("startup-benchmark")) ||
                   parser.isSet(QStringLiteral("smoke-test")) || interaction;
#ifdef NEVERD_GUI_TEST_PROBES
  const bool probe = parser.isSet(QStringLiteral("widgets-test")) ||
                     parser.isSet(QStringLiteral("analysis-test"));
  automated = automated || probe;
#endif
  std::unique_ptr<QTemporaryDir> temporarySettings;
  if (automated) {
    temporarySettings = std::make_unique<QTemporaryDir>();
    if (!temporarySettings->isValid()) {
      qCritical("Could not create temporary workbench test settings");
      return 2;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       temporarySettings->path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope,
                       temporarySettings->path());
  }
  applyLanguage(currentLanguage());
  Theme::instance().apply();

  Session session(QFileInfo(worker).absoluteFilePath());
  std::unique_ptr<StartupMetrics> startupMetrics;
  const bool openingFile = !parser.positionalArguments().isEmpty();
  if (parser.isSet(QStringLiteral("startup-benchmark"))) {
    bool validTimeout = false;
    const auto timeout =
        parser.value(QStringLiteral("startup-benchmark-timeout"))
            .toInt(&validTimeout);
    if (!validTimeout || timeout < 100 || timeout > 300000) {
      qCritical("Startup benchmark timeout must be 100-300000 milliseconds");
      return 2;
    }
    startupMetrics = std::make_unique<StartupMetrics>(
        session, startupClock,
        parser.value(QStringLiteral("startup-benchmark")), openingFile, timeout,
        &app);
    startupMetrics->record("application_created_ms", applicationCreatedMs);
    startupMetrics->record("docking_frontend_ms", dockingFrontendMs);
    startupMetrics->record("options_parsed_ms", optionsParsedMs);
    startupMetrics->record("controller_created_ms");
  }
  McpConnectionManager mcp;
  GuiSessionBroker broker;
  if (startupMetrics)
    startupMetrics->record("services_created_ms");

  MainWindow window(session, mcp, broker);
  QTemporaryDir testLayout;
  if (automated || parser.isSet(QStringLiteral("fresh-layout")))
    window.setLayoutPath(testLayout.filePath(QStringLiteral("layout.json")));
  restoreGeometry(window);
  window.initializeLayout();
  if (startupMetrics)
    startupMetrics->observeWindow(&window);
  std::unique_ptr<InteractionMetrics> interactionMetrics;
  if (interaction) {
    bool validTimeout = false;
    const auto timeout =
        parser.value(QStringLiteral("interaction-benchmark-timeout"))
            .toInt(&validTimeout);
    if (!validTimeout || timeout < 1000 || timeout > 3600000 || !openingFile) {
      qCritical("The interaction benchmark needs an input file and a timeout "
                "of 1000-3600000 milliseconds");
      return 2;
    }
    interactionMetrics = std::make_unique<InteractionMetrics>(
        window, session, parser.value(QStringLiteral("interaction-benchmark")),
        timeout, &app);
  }
  window.show();
  gnome::widenResizeEdges(window);
  QObject::connect(&app, &QApplication::aboutToQuit, &window, [&window] {
    QSettings().setValue(GeometryKey, window.saveGeometry());
  });

#ifdef NEVERD_GUI_TEST_PROBES
  if (probe) {
    const QString binary = parser.value(QStringLiteral("analysis-test"));
    startWidgetsProbe(window, session,
                      binary.isEmpty() ? QString()
                                       : QFileInfo(binary).absoluteFilePath());
    return app.exec();
  }
#endif
  window.runAfterOpen(parser.values(QStringLiteral("command")));
  if (openingFile)
    window.openFile(
        QFileInfo(parser.positionalArguments().first()).absoluteFilePath());
  else if (!automated && !parser.isSet(QStringLiteral("capture")) &&
           QSettings().value(settings::QuickStart, true).toBool())
    window.scheduleQuickStart();

  if (parser.isSet(QStringLiteral("capture"))) {
    const int delay = parser.value(QStringLiteral("capture-delay")).toInt();
    QTimer::singleShot(
        delay, &app, [&window, path = parser.value(QStringLiteral("capture"))] {
          if (!window.grab().save(path))
            QCoreApplication::exit(2);
        });
  }
  if (parser.isSet(QStringLiteral("smoke-test")))
    QTimer::singleShot(
        parser.isSet(QStringLiteral("capture"))
            ? parser.value(QStringLiteral("capture-delay")).toInt() + 1000
            : SmokeDelayMs,
        &app, [&window] {
          auto *functions =
              window.findChild<QWidget *>(QStringLiteral("functionsList"));
          if (!functions || !functions->isVisible() ||
              functions->width() <= 0 || functions->height() <= 0) {
            qCritical("The default function window is not visible.");
            QCoreApplication::exit(3);
          } else {
            QCoreApplication::quit();
          }
        });
  return app.exec();
}
