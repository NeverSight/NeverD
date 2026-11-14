//===- main.cpp - Standalone NeverD Agent desktop -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native application entry point. Project storage and preview do not require
/// LLVM, a model provider, an ND analysis worker, or the workbench to be
/// running.
//===----------------------------------------------------------------------===//

#include "Config.h"
#include "app/MarkdownDocument.h"
#include "client/WorkspaceClient.h"
#include "platform/AgentAppearance.h"
#include "platform/AgentWindowFrame.h"
#include "platform/AppPaths.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTextStream>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {
QString LogPath;
QMutex LogMutex;
void logMessage(QtMsgType type, const QMessageLogContext &,
                const QString &message) {
  QMutexLocker lock(&LogMutex);
  QFile file(LogPath);
  if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
    QTextStream stream(&file);
    stream << message << '\n';
  }
  const auto bytes = message.toUtf8();
  std::fprintf(stderr, "%s\n", bytes.constData());
  if (type == QtFatalMsg)
    std::abort();
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
  QQuickWindow::setDefaultAlphaBuffer(true);
  QQuickStyle::setStyle("Basic");
  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName("neverd-agent");
  QCoreApplication::setApplicationVersion("0.1.0");
  QCoreApplication::setOrganizationName("NeverSight");
  app.setWindowIcon(QIcon(":/agent/icons/brand.svg"));
  for (const char *name :
       {"NotoSans.ttf", "NotoSansCJK.ttc", "JetBrainsMonoNL-Regular.ttf"})
    if (QFontDatabase::addApplicationFont(QStringLiteral(":/agent/fonts/") +
                                          QLatin1String(name)) < 0)
      qFatal("Unable to load the bundled Agent font %s", name);
  // The workbench's type: the bundled Noto Sans and Noto Sans CJK on every
  // platform, at the desktop's interface size.
  QFont uiFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  uiFont.setFamilies({"Noto Sans", "Noto Sans CJK SC", "JetBrains Mono NL"});
  uiFont.setHintingPreference(QFont::PreferVerticalHinting);
  app.setFont(uiFont);

  QCommandLineParser parser;
  parser.setApplicationDescription("NeverD Agent project workspace");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addOption({"show-settings", "Open model and analysis settings."});
  parser.addOption({"data-dir",
                    "Agent global data directory (default: ~/.nd_agent).",
                    "directory"});
  parser.addOption(
      {"project", "Open an existing Agent project directory.", "directory"});
  parser.addOption({"show-panel",
                    "Open files, progress, memory, chat, evidence, review or "
                    "research in the right sidebar.",
                    "view"});
  parser.addOption(
      {"capture",
       "Save a native Qt window capture and exit (for UI verification).",
       "png"});
  parser.addOption({"capture-size",
                    "Native capture size in logical pixels, WIDTHxHEIGHT.",
                    "size", "1360x860"});
  parser.process(app);

  const QString directory = neverd::agent::agentHome(parser.value("data-dir"));
  QString startupError;
  LogPath = QDir(directory).filePath("agent.log");
  qInstallMessageHandler(logMessage);
  // The workbench's interface language and theme, before any text exists;
  // the CLI starts in the same language.
  neverd::agent::AgentAppearance appearance;
  appearance.refresh();
  neverd::agent::WorkspaceClient controller(directory);
  controller.initialize(parser.value("project"), startupError);

  const QString panel = parser.value("show-panel");
  if (!panel.isEmpty() && panel != "files" && panel != "progress" &&
      panel != "memory" && panel != "chat" && panel != "evidence" &&
      panel != "review" && panel != "research")
    startupError = "--show-panel must be files, progress, memory, chat, "
                   "evidence, review or research.";
  neverd::agent::AgentWindowFrame frame;
  qmlRegisterType<neverd::agent::MarkdownDocument>("NeverD.Agent", 1, 0,
                                                   "MarkdownDocument");
  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("controller", &controller);
  engine.rootContext()->setContextProperty("windowFrame", &frame);
  engine.rootContext()->setContextProperty("appearance", &appearance);
  engine.rootContext()->setContextProperty("appFont", uiFont);
  engine.rootContext()->setContextProperty("startupError", startupError);
  engine.rootContext()->setContextProperty("initialSidePanel", panel);
  engine.loadFromModule("NeverD.Agent", "Main");
  if (engine.rootObjects().isEmpty())
    return 1;
  auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().front());
  if (!window)
    return 1;
  frame.attach(*window);
  // Right-to-left languages mirror every item, dialogs and menus included,
  // from the window's root, as the workbench mirrors its widgets.
  const auto mirror = [window, &appearance] {
    auto *context = qmlContext(window);
    QQmlProperty(window->contentItem(), "LayoutMirroring.enabled", context)
        .write(appearance.rightToLeft());
    QQmlProperty(window->contentItem(), "LayoutMirroring.childrenInherit",
                 context)
        .write(true);
  };
  mirror();
  // A language or theme chosen in the workbench shows here the next time
  // this window becomes active.
  QObject::connect(window, &QWindow::activeChanged, &appearance,
                   [window, &appearance] {
                     if (window->isActive())
                       appearance.refresh();
                   });
  QObject::connect(&appearance,
                   &neverd::agent::AgentAppearance::languageChanged, &engine,
                   [&engine, &controller, &appearance, mirror] {
                     engine.retranslate();
                     mirror();
                     controller.setLanguage(appearance.language());
                   });
  window->show();
  if (parser.isSet("show-settings"))
    QMetaObject::invokeMethod(window, "showModelSettings");

  if (parser.isSet("capture")) {
    if (!startupError.isEmpty())
      return 1;
    const auto size = parser.value("capture-size").split('x');
    bool widthOK = false, heightOK = false;
    const int width = size.value(0).toInt(&widthOK);
    const int height = size.value(1).toInt(&heightOK);
    if (size.size() != 2 || !widthOK || !heightOK ||
        width < window->minimumWidth() || height < window->minimumHeight()) {
      qCritical("--capture-size must be WIDTHxHEIGHT and at least the window "
                "minimum size.");
      return 1;
    }
    window->resize(width, height);
    // Allow text layouts, follow-tail positioning and effect textures to settle
    // after the first frame. This delay is only used for diagnostic captures.
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(
        window, &QQuickWindow::frameSwapped, &app,
        [window, &app, path = parser.value("capture"), connection] {
          QObject::disconnect(*connection);
          QTimer::singleShot(
              neverd::agent::kCaptureSettleMs, &app, [window, &app, path] {
                const bool saved = window->grabWindow().save(path);
                if (!saved)
                  qCritical("Unable to save the native window capture.");
                app.exit(saved ? 0 : 1);
              });
        },
        Qt::QueuedConnection);
    window->update();
  }
  return app.exec();
}
