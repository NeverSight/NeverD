#include "Config.h"
#include "ResourceLimits.h"
#include "app/DeepLink.h"
#include "app/DialogReturnKey.h"
#include "app/DisassemblyView.h"
#include "app/Docking.h"
#include "app/GnomeModalDialogs.h"
#include "app/InteractionMetrics.h"
#include "app/Language.h"
#include "app/LinkDispatch.h"
#include "app/LinkOpener.h"
#include "app/ListingView.h"
#include "app/MainWindow.h"
#include "app/Session.h"
#include "app/SettingsKeys.h"
#include "app/StartupMetrics.h"
#include "app/Theme.h"
#include "app/WindowFrame.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
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

void restoreGeometry(MainWindow &window) {
  const auto geometry = QSettings().value(GeometryKey).toByteArray();
  if (!geometry.isEmpty() && window.restoreWindowedGeometry(geometry))
    return;
  const QRect available = window.screen()->availableGeometry();
  const QSize size(
      std::min(::neverd::limits::gui_config::kMainDefaultWidth,
               available.width() *
                   ::neverd::limits::gui_config::kMainDefaultWidthPercent /
                   100),
      std::min(::neverd::limits::gui_config::kMainDefaultHeight,
               available.height() *
                   ::neverd::limits::gui_config::kMainDefaultHeightPercent /
                   100));
  window.resize(size);
  window.move(available.center() - QPoint(size.width() / 2, size.height() / 2));
}

void configureOptions(QCommandLineParser &parser) {
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
       QStringLiteral("milliseconds"),
       QString::number(::neverd::limits::gui::
                           kDefaultStartupBenchmarkTimeoutMilliseconds)});
  parser.addOption(
      {QStringLiteral("interaction-benchmark"),
       QStringLiteral("Scroll, jump, follow links, open graphs and pseudocode "
                      "in the opened file, write their latencies as JSON "
                      "and exit"),
       QStringLiteral("path")});
  parser.addOption(
      {QStringLiteral("interaction-benchmark-timeout"),
       QStringLiteral("Interaction benchmark deadline in milliseconds"),
       QStringLiteral("milliseconds"),
       QString::number(::neverd::limits::gui::
                           kDefaultInteractionBenchmarkTimeoutMilliseconds)});
  parser.addOption(
      {QStringLiteral("link"),
       QStringLiteral("Open a neverd:// or ida:// link in the running "
                      "workbench that has its program, else open the program "
                      "and the link here; a link in place of the binary does "
                      "the same"),
       QStringLiteral("url")});
  parser.addOption(
      {QStringLiteral("no-launch"),
       QStringLiteral("With a link, only send it to a running workbench")});
  parser.addOption({QStringLiteral("list-instances"),
                    QStringLiteral("List the running workbenches and the "
                                   "program each has open, then exit")});
  parser.addOption({QStringLiteral("register-link-handler"),
                    QStringLiteral("Open neverd:// links clicked anywhere in "
                                   "this workbench, then exit")});
  parser.addOption({QStringLiteral("unregister-link-handler"),
                    QStringLiteral("Stop opening neverd:// links in this "
                                   "workbench, then exit")});
#ifdef NEVERD_GUI_TEST_PROBES
  parser.addOption(
      {QStringLiteral("widgets-test"),
       QStringLiteral("Exercise the workbench on a fixture and exit")});
  parser.addOption({QStringLiteral("analysis-test"),
                    QStringLiteral("Exercise the workbench on a binary with a "
                                   "function named main and exit"),
                    QStringLiteral("binary")});
#endif
}

} // namespace

int main(int argc, char **argv) {
  QElapsedTimer startupClock;
  startupClock.start();
  QApplication::setOrganizationName(QStringLiteral("NeverSight"));
  QApplication::setOrganizationDomain(QStringLiteral("neversight.dev"));
  QApplication::setApplicationName(QStringLiteral("NeverD"));
  QApplication::setApplicationVersion(QStringLiteral("3389.0.1"));
  QCommandLineParser parser;
  configureOptions(parser);
  QStringList arguments;
  for (int index = 0; index < argc; ++index)
    arguments << QString::fromLocal8Bit(argv[index]);
  const bool parsed = parser.parse(arguments);
  const auto positional = parser.positionalArguments();
  const bool positionalLink =
      std::any_of(positional.begin(), positional.end(), looksLikeDeepLink);
  const bool headless =
      parsed && (parser.isSet(QStringLiteral("list-instances")) ||
                 parser.isSet(QStringLiteral("register-link-handler")) ||
                 parser.isSet(QStringLiteral("unregister-link-handler")) ||
                 parser.isSet(QStringLiteral("help")) ||
                 parser.isSet(QStringLiteral("version")) ||
                 (parser.isSet(QStringLiteral("no-launch")) &&
                  (parser.isSet(QStringLiteral("link")) || positionalLink)));
  std::unique_ptr<QCoreApplication> application;
  if (headless) {
    application = std::make_unique<QCoreApplication>(argc, argv);
  } else {
    gnome::prepareModalDialogs();
    WindowFrame::prepareNativeWindows();
    application = std::make_unique<QApplication>(argc, argv);
  }
  QCoreApplication &app = *application;
  const auto applicationCreatedMs = startupClock.nsecsElapsed() / 1.0e6;
  if (auto *gui = qobject_cast<QApplication *>(&app)) {
    gnome::detachModalDialogs(*gui);
    WindowFrame::frameDialogs(*gui);
    acceptDialogsOnReturn(*gui);
    configureDocking();
  }
  const auto dockingFrontendMs = startupClock.nsecsElapsed() / 1.0e6;

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

  // Links: the running workbench that has a link's program takes it, as
  // IDA's hcli forwards an ida:// link, and only then does this one start.
  QTextStream out(stdout), errors(stderr);
  if (parser.isSet(QStringLiteral("list-instances"))) {
    out << describeInstances() << '\n';
    return 0;
  }
  if (parser.isSet(QStringLiteral("register-link-handler")) ||
      parser.isSet(QStringLiteral("unregister-link-handler"))) {
    QString message;
    const bool done = registerLinkHandler(
        parser.isSet(QStringLiteral("register-link-handler")), message);
    (done ? out : errors) << message << '\n';
    return done ? 0 : 1;
  }
  QStringList files = parser.positionalArguments();
  QString linkText = parser.value(QStringLiteral("link"));
  if (parser.isSet(QStringLiteral("link")) && linkText.isEmpty()) {
    errors << parseDeepLink(linkText).error << '\n';
    return 2;
  }
  for (qsizetype index = 0; linkText.isEmpty() && index < files.size(); ++index)
    if (looksLikeDeepLink(files[index]))
      linkText = files.takeAt(index);
  std::optional<DeepLink> link;
  if (!linkText.isEmpty()) {
    const ParsedLink parsed = parseDeepLink(linkText);
    if (!parsed.link) {
      errors << parsed.error << '\n';
      return 2;
    }
    link = parsed.link;
    const Dispatched dispatched = dispatchLink(linkText, *link);
    if (dispatched.sent) {
      (dispatched.opened ? out : errors) << dispatched.message << '\n';
      return dispatched.opened ? 0 : 1;
    }
    if (parser.isSet(QStringLiteral("no-launch"))) {
      errors << QCoreApplication::translate(
                    "LinkDispatch",
                    "No running NeverD has the link's program open.")
             << '\n';
      return 1;
    }
    if (files.isEmpty()) {
      if (const auto file = recentFileFor(*link)) {
        files << *file;
      } else {
        errors << QCoreApplication::translate(
                      "LinkDispatch",
                      "No running NeverD has the link's program open, and "
                      "no recently opened file is it; name the file too: "
                      "neverd-gui FILE LINK")
               << '\n';
        return 1;
      }
    }
  }
  Theme::instance().apply();

  Session session(QFileInfo(worker).absoluteFilePath());
  std::unique_ptr<StartupMetrics> startupMetrics;
  const bool openingFile = !files.isEmpty();
  if (parser.isSet(QStringLiteral("startup-benchmark"))) {
    bool validTimeout = false;
    const auto timeout =
        parser.value(QStringLiteral("startup-benchmark-timeout"))
            .toInt(&validTimeout);
    if (!validTimeout ||
        timeout <
            ::neverd::limits::gui::kStartupMinBenchmarkTimeoutMilliseconds ||
        timeout >
            ::neverd::limits::gui::kStartupMaxBenchmarkTimeoutMilliseconds) {
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
  // Nobody answers the questions of a run that captures or measures.
  const bool unattended = automated || parser.isSet(QStringLiteral("capture"));
  if (unattended)
    window.setAsksForDebugInfo(false);
  // Without a file the quick start greets alone; the workbench lays out its
  // desktop and shows once the user chooses how to start.  A file named here
  // asks what it asks before its load alone too, and shows the workbench as
  // it starts to load.
  const bool greet = !openingFile && !unattended &&
                     QSettings().value(settings::QuickStart, true).toBool();
  const bool asksFirst = openingFile && !unattended;
  if (!greet && !asksFirst)
    window.initializeLayout();
  if (startupMetrics)
    startupMetrics->observeWindow(&window);
  std::unique_ptr<InteractionMetrics> interactionMetrics;
  if (interaction) {
    bool validTimeout = false;
    const auto timeout =
        parser.value(QStringLiteral("interaction-benchmark-timeout"))
            .toInt(&validTimeout);
    if (!validTimeout ||
        timeout < ::neverd::limits::gui::
                      kInteractionMinBenchmarkTimeoutMilliseconds ||
        timeout > ::neverd::limits::gui::
                      kInteractionMaxBenchmarkTimeoutMilliseconds ||
        !openingFile) {
      qCritical("The interaction benchmark needs an input file and a timeout "
                "of 1000-3600000 milliseconds");
      return 2;
    }
    interactionMetrics = std::make_unique<InteractionMetrics>(
        window, session, parser.value(QStringLiteral("interaction-benchmark")),
        timeout, &app);
  }
  if (!greet && !asksFirst)
    window.showWorkbench();
  QObject::connect(&app, &QApplication::aboutToQuit, &window, [&window] {
    QSettings().setValue(GeometryKey, window.saveWindowedGeometry());
  });
  // Cocoa can close the window while already handling an application quit.
  // Do not reenter native termination from inside that close event.
  QObject::connect(&window, &MainWindow::shutdownAccepted, &app,
                   &QCoreApplication::quit, Qt::QueuedConnection);

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
  if (link)
    window.openLinkAfterOpen(*link);
  LinkOpener linkOpener(window, session);
  app.installEventFilter(&linkOpener);
  if (openingFile)
    window.openFile(QFileInfo(files.first()).absoluteFilePath());
  else if (greet)
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
