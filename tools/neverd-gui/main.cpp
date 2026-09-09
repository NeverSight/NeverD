#include "NativeCodeHighlighter.h"
#include "NativeGraphItem.h"
#include "Workbench.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <kddockwidgets/Config.h>
#include <kddockwidgets/qtquick/Platform.h>
#include <kddockwidgets/qtquick/ViewFactory.h>
#ifdef NEVERD_GUI_TEST_PROBES
#include "tests/AnalysisProbe.h"
#include "tests/DockingProbe.h"
#endif

class NeverDDockViewFactory final : public KDDockWidgets::QtQuick::ViewFactory {
public:
  QUrl titleBarFilename() const override {
    return QUrl("qrc:/qt/qml/NeverD/Workbench/qml/DockTitleBar.qml");
  }
  QUrl tabbarFilename() const override {
    return QUrl("qrc:/qt/qml/NeverD/Workbench/qml/DockTabBar.qml");
  }
  QUrl groupFilename() const override {
    return QUrl("qrc:/qt/qml/NeverD/Workbench/qml/DockGroup.qml");
  }
  QUrl separatorFilename() const override {
    return QUrl("qrc:/qt/qml/NeverD/Workbench/qml/DockSeparator.qml");
  }
};

// Installed after the docking frontend: preserve the live layout and resolve
// unsaved edits before KDDockWidgets begins closing child panels.
class WorkbenchCloseGuard final : public QObject {
public:
  explicit WorkbenchCloseGuard(Workbench &workbench) : workbench_(workbench) {}
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() != QEvent::Close)
      return false;
    if (!object->property("closeApproved").toBool() &&
        !workbench_.requestClose()) {
      event->ignore();
      return true;
    }
    if (auto *workspace = object->findChild<QObject *>("dockWorkspace"))
      QMetaObject::invokeMethod(workspace, "saveLayout");
    return false;
  }

private:
  Workbench &workbench_;
};

int main(int argc, char **argv) {
  QGuiApplication::setOrganizationName("NeverSight");
  QGuiApplication::setOrganizationDomain("neversight.dev");
  QGuiApplication::setApplicationName("NeverD");
  QGuiApplication::setApplicationVersion("3389.0.1");
  QQuickStyle::setStyle("Basic");
  qmlRegisterType<NativeCodeHighlighter>("NeverD.Native", 1, 0,
                                         "NativeCodeHighlighter");
  qmlRegisterType<NativeGraphItem>("NeverD.Native", 1, 0, "NativeGraphItem");
  QGuiApplication app(argc, argv);
  KDDockWidgets::initFrontend(KDDockWidgets::FrontendType::QtQuick);
  KDDockWidgets::Config::self().setViewFactory(new NeverDDockViewFactory);
  QCommandLineParser parser;
  parser.setApplicationDescription("NeverD interactive analysis workbench");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("binary", "Binary file to open", "[binary]");
  parser.addOption({"worker", "Path to the matching analysis worker", "path"});
  parser.addOption(
      {"capture", "Save a workbench screenshot after loading", "path"});
  parser.addOption(
      {"smoke-test", "Exit after loading the UI (for build verification)"});
  parser.addOption({"fresh-layout", "Use a temporary workbench layout"});
#ifdef NEVERD_GUI_TEST_PROBES
  parser.addOption({"docking-test", "Exercise the docking workbench and exit"});
  parser.addOption(
      {"analysis-test", "Exercise the live analysis views and exit"});
#endif
  parser.process(app);
  QString worker = parser.value("worker");
  if (worker.isEmpty())
    worker = QDir(QCoreApplication::applicationDirPath())
                 .filePath(
#ifdef Q_OS_WIN
                     "neverd-worker.exe"
#else
                     "neverd-worker"
#endif
                 );
  Workbench workbench(QFileInfo(worker).absoluteFilePath());
  QTemporaryDir testLayout;
  if (parser.isSet("smoke-test") || parser.isSet("fresh-layout"))
    workbench.setDockLayoutPath(testLayout.filePath("layout.json"));
#ifdef NEVERD_GUI_TEST_PROBES
  if (parser.isSet("docking-test") || parser.isSet("analysis-test"))
    workbench.setDockLayoutPath(testLayout.filePath("layout.json"));
#endif
  QObject::connect(
      &app, &QGuiApplication::screenRemoved, &workbench, [&workbench] {
        QTimer::singleShot(0, &workbench, &Workbench::clampWindows);
      });
  McpConnectionManager mcp;
  GuiSessionBroker broker;
  QObject::connect(&workbench, &Workbench::languageChanged, &mcp,
                   &McpConnectionManager::retranslate);
  QObject::connect(&workbench, &Workbench::languageChanged, &broker,
                   &GuiSessionBroker::retranslate);
  QObject::connect(&broker, &GuiSessionBroker::queryRequested, &workbench,
                   &Workbench::externalQuery);
  QObject::connect(&workbench, &Workbench::externalResponse, &broker,
                   &GuiSessionBroker::reply);
  QObject::connect(&workbench, &Workbench::selectionChanged, &broker,
                   &GuiSessionBroker::setSelection);
  QObject::connect(&broker, &GuiSessionBroker::navigationRequested, &workbench,
                   [&workbench](const QString &address, bool highlight) {
                     if (highlight)
                       workbench.selectInstruction(address);
                     else
                       workbench.navigate(address);
                   });
  QQmlApplicationEngine engine;
  KDDockWidgets::QtQuick::Platform::instance()->setQmlEngine(&engine);
  workbench.setQmlEngine(&engine);
  engine.rootContext()->setContextProperty("workbench", &workbench);
  engine.rootContext()->setContextProperty("mcp", &mcp);
  engine.rootContext()->setContextProperty("sessionBroker", &broker);
  engine.rootContext()->setContextProperty(
      "systemMonoFamily",
      QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
      [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
  engine.loadFromModule("NeverD.Workbench", "Main");
  if (engine.rootObjects().isEmpty())
    return 1;
  WorkbenchCloseGuard closeGuard(workbench);
  engine.rootObjects().first()->installEventFilter(&closeGuard);
#ifdef NEVERD_GUI_TEST_PROBES
  if (parser.isSet("docking-test"))
    startDockingProbe(engine, workbench);
  if (parser.isSet("analysis-test"))
    startAnalysisProbe(engine, workbench);
#endif
  if (!parser.positionalArguments().isEmpty())
    workbench.openFile(QUrl::fromLocalFile(
        QFileInfo(parser.positionalArguments().first()).absoluteFilePath()));
  if (parser.isSet("capture")) {
    auto capture = [&engine, path = parser.value("capture")] {
      auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
      if (!window || !window->grabWindow().save(path))
        QCoreApplication::exit(2);
    };
    QTimer::singleShot(6000, &app, capture);
  }
  if (parser.isSet("smoke-test"))
    QTimer::singleShot(parser.isSet("capture") ? 7000 : 1500, &app, [&engine] {
      auto *list = engine.rootObjects().first()->findChild<QQuickItem *>(
          "functionsList");
      if (!list || !list->isVisible() || list->width() <= 0 ||
          list->height() <= 0) {
        qCritical("The default function pane is not visible.");
        QCoreApplication::exit(3);
      } else
        QCoreApplication::quit();
    });
  return app.exec();
}
