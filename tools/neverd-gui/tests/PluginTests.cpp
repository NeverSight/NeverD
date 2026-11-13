//===- PluginTests.cpp - External plugin qualification --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External plugin qualification.
///
//===----------------------------------------------------------------------===//

#include "ActionRegistry.h"
#include "AnalysisPlugins.h"
#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "Icons.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "PluginManagerDialog.h"
#include "PluginServices.h"
#include "ResourceLimits.h"
#include "Session.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"
#include "plugins/PluginJobs.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsView>
#include <QImage>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace neverd::gui;

namespace {
QString prepare(const QString &directory, bool developer = true) {
  QDir().mkpath(directory);
  QFile::copy(QStringLiteral(TEST_GUI_SDK "/neverd_gui_plugin.py"),
              directory + QStringLiteral("/neverd_gui_plugin.py"));
  QFile::copy(
      QStringLiteral(TEST_GUI_SDK "/examples/workbench-explorer/explorer.py"),
      directory + QStringLiteral("/explorer.py"));
  QDir().mkpath(directory + QStringLiteral("/icons"));
  QFile::copy(
      QStringLiteral(TEST_GUI_SDK
                     "/examples/workbench-explorer/icons/function-report.svg"),
      directory + QStringLiteral("/icons/function-report.svg"));
  QFile source(QStringLiteral(TEST_GUI_SDK "/examples/workbench-explorer/") +
               (developer ? QStringLiteral("plugin-developer.json")
                          : QStringLiteral("plugin.json")));
  if (!source.open(QIODevice::ReadOnly))
    return {};
  auto manifest = QJsonDocument::fromJson(source.readAll()).object();
  manifest.insert("program", QStringLiteral(TEST_PYTHON));
  const auto path = directory + QStringLiteral("/plugin.json");
  QFile output(path);
  if (!output.open(QIODevice::WriteOnly))
    return {};
  output.write(QJsonDocument(manifest).toJson());
  return path;
}
QJsonObject defaults(const PluginJobs &jobs, const QString &action) {
  QJsonObject parameters;
  const auto plugin = jobs.plugin(QStringLiteral("workbench-explorer"));
  for (const auto &value : plugin->actions)
    if (value.toObject().value("id").toString() == action)
      for (const auto &fieldValue :
           value.toObject().value("parameters").toArray()) {
        const auto field = fieldValue.toObject();
        parameters.insert(field.value("id").toString(), field.value("default"));
      }
  return parameters;
}
QJsonObject context(const QString &epoch = QStringLiteral("1")) {
  return {{"epoch", epoch},
          {"project_id", "fixture"},
          {"revision", "1"},
          {"loaded", true},
          {"address", "0xffff800012340000"},
          {"function_address", "0xffff800012340000"}};
}
} // namespace

class PluginTests : public QObject {
  Q_OBJECT
  QTemporaryDir settings_;
private slots:
  void emptyTaskDockStaysHiddenAcrossDesktopRestore() {
    QTemporaryDir directory;
    const auto layout = directory.filePath("layout.json");
    const auto manifest = prepare(directory.filePath("plugin"));
    {
      Session session(QStringLiteral(TEST_WORKER));
      McpConnectionManager mcp;
      GuiSessionBroker broker;
      MainWindow window(session, mcp, broker);
      window.setLayoutPath(layout);
      window.initializeLayout();
      window.show();
      QCoreApplication::processEvents();
      auto *pane = window.analysisPlugins();
      QVERIFY(!pane->isVisible());
      QString error;
      QVERIFY2(pane->installPlugin(manifest, error), qPrintable(error));
      QVERIFY(!pane->isVisible());
      auto *show = pane->findChild<QAction *>("pluginShowTasks");
      QVERIFY(show);
      show->trigger();
      QTRY_VERIFY(pane->isVisible());
      window.actions().action(ActionId::WindowsSaveDesktop)->trigger();
      QVERIFY(QFileInfo::exists(layout));
      QVERIFY(window.close());
    }
    {
      Session session(QStringLiteral(TEST_WORKER));
      McpConnectionManager mcp;
      GuiSessionBroker broker;
      MainWindow window(session, mcp, broker);
      window.setLayoutPath(layout);
      window.initializeLayout();
      window.show();
      QCoreApplication::processEvents();
      auto *pane = window.analysisPlugins();
      QVERIFY(!pane->isVisible());
      window.actions().action(ActionId::WindowsLoadDesktop)->trigger();
      QCoreApplication::processEvents();
      QVERIFY(!pane->isVisible());
      auto *run =
          pane->findChild<QAction *>("plugin.workbench-explorer:context");
      QVERIFY(run && run->isEnabled());
      run->trigger();
      QTRY_VERIFY(pane->isVisible());
      QTRY_VERIFY(!pane->jobs().jobs().isEmpty());
      const auto id = pane->jobs().jobs().last().id;
      QTRY_VERIFY_WITH_TIMEOUT(pane->jobs().job(id)->terminal(), 7000);
      QCOMPARE(pane->jobs().job(id)->state, QStringLiteral("succeeded"));
    }
  }

  void taskActionsStayWithTheirPluginAndSelectedResult() {
    QTemporaryDir directory;
    QFile helper(QStringLiteral(TEST_GUI_SDK "/neverd_gui_plugin.py"));
    QVERIFY(helper.copy(directory.filePath("neverd_gui_plugin.py")));
    QFile provider(directory.filePath("provider.py"));
    QVERIFY(provider.open(QIODevice::WriteOnly));
    provider.write(R"PY(import sys
from neverd_gui_plugin import run
mode = sys.argv[1]
def handler(task):
    task.log(mode + ' log: only this task')
    (task.output_dir / 'result.json').write_text('{}', encoding='utf-8')
    result = dict(views=[dict(kind='text', title='Result', text=mode + ' result')],
                  artifacts=[task.artifact('result.json', 'Result document')])
    if mode == 'image':
        result['image_addition'] = dict(artifact=0, sample_sha256='0' * 64, description=dict(schema='neverd.analysis-section.v1', name='.example', address='0x180000000', permissions='rx', functions=[]))
    return result
raise SystemExit(run(handler))
)PY");
    provider.close();
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    pane.setWindowTitle(QStringLiteral("Plugin tasks"));
    pane.resize(1000, 720);
    pane.show();
    QString error;
    for (const auto &mode :
         {QStringLiteral("image"), QStringLiteral("report")}) {
      // The same display name is deliberate: stable plugin IDs must still
      // disambiguate history, commands and logs.
      QJsonObject manifest{
          {"schema", "neverd.gui-plugin.v1"},
          {"id", mode + QStringLiteral("-tools")},
          {"name", "Analysis tools"},
          {"version", "1"},
          {"program", TEST_PYTHON},
          {"arguments", QJsonArray{"provider.py", mode}},
          {"queries", QJsonArray{}},
          {"actions", QJsonArray{QJsonObject{
                          {"id", "run"},
                          {"title", mode == "image" ? "Build recovered image"
                                                    : "Inspect function"},
                          {"context", "none"},
                          {"parameters", QJsonArray{}}}}}};
      QFile file(directory.filePath(mode + QStringLiteral(".json")));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QJsonDocument(manifest).toJson());
      file.close();
      QVERIFY2(pane.jobs().install(file.fileName(), error), qPrintable(error));
    }
    auto *management =
        pane.findChild<QWidget *>(QStringLiteral("pluginManagement"));
    auto *actions =
        pane.findChild<QWidget *>(QStringLiteral("pluginTaskActions"));
    auto *tree = pane.findChild<QTreeWidget *>(QStringLiteral("pluginTasks"));
    auto *title = pane.findChild<QLabel *>(QStringLiteral("pluginTaskTitle"));
    auto *owner = pane.findChild<QLabel *>(QStringLiteral("pluginTaskOwner"));
    auto *image =
        pane.findChild<QPushButton *>(QStringLiteral("pluginImportImage"));
    auto *showLog =
        pane.findChild<QPushButton *>(QStringLiteral("pluginShowLog"));
    auto *log =
        pane.findChild<QPlainTextEdit *>(QStringLiteral("pluginTaskLog"));
    QVERIFY(management && actions && tree && title && owner && image &&
            showLog && log);
    QVERIFY(!management->isAncestorOf(image));
    QVERIFY(actions->isAncestorOf(image));
    QVERIFY(actions->isHidden());
    const auto imageTask = pane.jobs().start("image-tools", "run", {}, error);
    const auto reportTask = pane.jobs().start("report-tools", "run", {}, error);
    const auto secondImageTask =
        pane.jobs().start("image-tools", "run", {}, error);
    QVERIFY2(!imageTask.isEmpty() && !reportTask.isEmpty() &&
                 !secondImageTask.isEmpty(),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(pane.jobs().job(secondImageTask)->terminal(),
                             10000);
    for (const auto &id : {imageTask, reportTask, secondImageTask})
      QVERIFY2(pane.jobs().job(id)->state == "succeeded",
               qPrintable(pane.jobs().job(id)->diagnostic));
    QCOMPARE(tree->topLevelItemCount(), 2);
    const auto find = [&](const QString &id) -> QTreeWidgetItem * {
      for (int row = 0; row < tree->topLevelItemCount(); ++row)
        for (int child = 0; child < tree->topLevelItem(row)->childCount();
             ++child)
          if (auto *item = tree->topLevelItem(row)->child(child);
              item->data(0, Qt::UserRole).toString() == id)
            return item;
      return nullptr;
    };
    auto *imageItem = find(imageTask);
    auto *reportItem = find(reportTask);
    QVERIFY(imageItem && reportItem);
    QCOMPARE(imageItem->parent()->childCount(), 2);
    QCOMPARE(reportItem->parent()->childCount(), 1);
    QVERIFY(imageItem->parent()->text(0).contains("image-tools"));
    QVERIFY(reportItem->parent()->text(0).contains("report-tools"));
    const auto screenshotDirectory =
        qEnvironmentVariable("NEVERD_PLUGIN_GROUP_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty())
      QVERIFY(QDir().mkpath(screenshotDirectory));
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      Theme::instance().setMode(mode);
      Theme::instance().apply();
      const auto suffix = mode == Theme::Mode::Dark ? QStringLiteral("dark")
                                                    : QStringLiteral("light");
      pane.resize(1000, 720);
      tree->setCurrentItem(imageItem);
      QCOMPARE(title->text(), QStringLiteral("Build recovered image"));
      QVERIFY(owner->text().contains("image-tools") &&
              owner->text().contains(imageTask.left(8)));
      QVERIFY(!image->isHidden());
      showLog->setChecked(true);
      QVERIFY(log->toPlainText().contains("image log"));
      QVERIFY(!log->toPlainText().contains("report log"));
      showLog->setChecked(false);
      QCoreApplication::processEvents();
      if (!screenshotDirectory.isEmpty())
        QVERIFY(pane.grab().save(screenshotDirectory + "/image-" + suffix +
                                 ".png"));
      tree->setCurrentItem(reportItem);
      QCOMPARE(title->text(), QStringLiteral("Inspect function"));
      QVERIFY(owner->text().contains("report-tools"));
      QVERIFY(image->isHidden());
      showLog->setChecked(true);
      QVERIFY(log->toPlainText().contains("report log"));
      QVERIFY(!log->toPlainText().contains("image log"));
      showLog->setChecked(false);
      QCoreApplication::processEvents();
      if (!screenshotDirectory.isEmpty())
        QVERIFY(pane.grab().save(screenshotDirectory + "/report-" + suffix +
                                 ".png"));
      tree->setCurrentItem(reportItem->parent());
      QVERIFY(actions->isHidden());
      tree->setCurrentItem(imageItem);
      imageItem->parent()->setExpanded(false);
      QTest::qWait(::neverd::limits::gui::plugin::kRefreshIntervalMs + 100);
      QVERIFY(!imageItem->parent()->isExpanded());
      imageItem->parent()->setExpanded(true);
      pane.resize(340, 900);
      QCoreApplication::processEvents();
      QVERIFY(tree->columnWidth(0) >=
              ::neverd::limits::gui::plugin::kTaskTreeNameInitialWidth);
      for (auto *button : actions->findChildren<QPushButton *>())
        if (!button->isHidden())
          QVERIFY(actions->rect().contains(button->geometry()));
      if (!screenshotDirectory.isEmpty())
        QVERIFY(pane.grab().save(screenshotDirectory + "/narrow-" + suffix +
                                 ".png"));
    }
  }

  void initTestCase() {
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDPluginTests"));
    QCoreApplication::setApplicationName(QStringLiteral("GuiPlugins"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_.path());
  }
  void cleanup() {
    QSettings().remove("guiPlugins/installed");
    QSettings().remove("guiPlugins/enabled");
  }

  void disabledInstallationsSurviveSessionRestart() {
    QTemporaryDir directory;
    const auto manifest = prepare(directory.filePath("disabled"), false);
    QString error;
    {
      Session session(QStringLiteral(TEST_WORKER));
      ActionRegistry registry;
      AnalysisPlugins pane(session, registry);
      QVERIFY2(pane.installPlugin(manifest, error), qPrintable(error));
      auto &services = session.pluginServices();
      QVERIFY2(services.setPluginEnabled("workbench-explorer", false, error),
               qPrintable(error));
      QTRY_VERIFY(!pane.jobs().retiring("workbench-explorer"));
      QCOMPARE(services.pluginInstallations().size(), 1);
      QVERIFY(!services.pluginInstallations().first().Enabled);
      QVERIFY(pane.contextActions().isEmpty());
    }
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    auto &services = session.pluginServices();
    QVERIFY(pane.jobs().plugins().isEmpty());
    QCOMPARE(services.pluginInstallations().size(), 1);
    QVERIFY(!services.pluginInstallations().first().Enabled);
    QVERIFY2(services.setPluginEnabled("workbench-explorer", true, error),
             qPrintable(error));
    QCOMPARE(pane.jobs().plugins().size(), 1);
    QVERIFY(!pane.contextActions().isEmpty());
  }

  void returnInTheManagersFilterOpensNoChooser() {
    // "Import plugin…" was the manager's default by Qt's choice of its first
    // push button, so Return in the filter opened the file chooser.
    // Were the chooser to open, it is Qt's own, which this test can see and
    // dismiss; a native one would hold the test until it timed out.
    const bool native =
        QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    const auto restore = qScopeGuard([native] {
      QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, native);
    });
    Session session(QStringLiteral(TEST_WORKER));
    PluginManagerDialog manager(session.pluginServices());
    manager.show();
    QVERIFY(QTest::qWaitForWindowExposed(&manager));
    bool opened = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, &manager, [&opened] {
      if (auto *chooser =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        opened = true;
        chooser->reject();
      }
    });
    watch.start(10);
    auto *filter = manager.findChild<QLineEdit *>("pluginManagerFilter");
    QVERIFY(filter);
    filter->setFocus();
    QTest::keyClick(filter, Qt::Key_Return);
    QTest::qWait(100);
    QVERIFY(!opened);
    QVERIFY(manager.isVisible());
    // No button is the default, and importing, which the manager's line of
    // help asks for, is the one that wears the accent.
    QStringList accented;
    for (const auto *button : manager.findChildren<QPushButton *>()) {
      QVERIFY2(!button->isDefault() && !button->autoDefault(),
               qPrintable(button->text()));
      if (button->property("mainAction").toBool())
        accented.append(button->objectName());
    }
    QCOMPARE(accented, QStringList{QStringLiteral("pluginManagerImport")});
  }

  void managerProjectsTheSharedCatalog() {
    QTemporaryDir directory;
    const auto manifest = prepare(directory.filePath("manager"), false);
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    QString error;
    QVERIFY2(pane.installPlugin(manifest, error), qPrintable(error));
    PluginManagerDialog manager(session.pluginServices());
    auto *list = manager.findChild<QTreeWidget *>("pluginManagerList");
    QVERIFY(list);
    QCOMPARE(list->topLevelItemCount(), 1);
    const auto old = pane.jobs().actionBinding("workbench-explorer", "report");
    neverd::plugin::Diagnostic diagnostic;
    QVERIFY(pane.jobs().catalog().setEnabled("workbench-explorer", false,
                                             diagnostic));
    QTRY_VERIFY(!pane.jobs().retiring("workbench-explorer"));
    QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("No"));
    QCOMPARE(list->topLevelItem(0)->text(3), QStringLiteral("Inactive"));
    QVERIFY(!QSettings()
                 .value("guiPlugins/enabled/workbench-explorer", true)
                 .toBool());
    auto *toggle = manager.findChild<QPushButton *>("pluginManagerToggle");
    QVERIFY(toggle && toggle->isEnabled());
    toggle->click();
    QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("Yes"));
    const auto replacement =
        pane.jobs().actionBinding("workbench-explorer", "report");
    QVERIFY(replacement && old && replacement->id() != old->id());
    QVERIFY(!pane.jobs().host().describe(*old));
  }

  void runtimeChangesDoNotRewriteAnotherSessionsSavedIntent() {
    QTemporaryDir directory;
    const auto manifest = prepare(directory.filePath("shared"), false);
    QString error;
    Session first(QStringLiteral(TEST_WORKER));
    ActionRegistry firstActions;
    AnalysisPlugins firstPane(first, firstActions);
    QVERIFY2(firstPane.installPlugin(manifest, error), qPrintable(error));
    Session second(QStringLiteral(TEST_WORKER));
    ActionRegistry secondActions;
    AnalysisPlugins secondPane(second, secondActions);
    QVERIFY(secondPane.jobs().plugin("workbench-explorer"));
    QVERIFY(first.pluginServices().setPluginEnabled("workbench-explorer", false,
                                                    error));
    QTRY_VERIFY(!firstPane.jobs().retiring("workbench-explorer"));
    secondPane.jobs().setContext(context("next"));
    QVERIFY(!QSettings()
                 .value("guiPlugins/enabled/workbench-explorer", true)
                 .toBool());
    const auto other = prepare(directory.filePath("other"), false);
    QFile file(other);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto description = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    description.insert("id", "other-plugin");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(description).toJson());
    file.close();
    QVERIFY2(secondPane.installPlugin(other, error), qPrintable(error));
    QVERIFY(!QSettings()
                 .value("guiPlugins/enabled/workbench-explorer", true)
                 .toBool());
    QVERIFY(QSettings().contains("guiPlugins/installed/other-plugin"));
  }

  void registrationsSurviveRestartAndUnload() {
    QTemporaryDir directory;
    const auto path = prepare(directory.filePath("remembered"), false);
    Session session(QStringLiteral(TEST_WORKER));
    QString error;
    {
      ActionRegistry registry;
      AnalysisPlugins pane(session, registry);
      QVERIFY2(pane.installPlugin(path, error), qPrintable(error));
      QCOMPARE(pane.jobs().plugins().size(), 1);
      QVERIFY(pane.jobs().jobs().isEmpty());
    }
    {
      ActionRegistry registry;
      AnalysisPlugins pane(session, registry);
      QCOMPARE(pane.jobs().plugins().size(), 1);
      QCOMPARE(pane.contextActions().size(), 3);
      QVERIFY2(pane.installPlugin(path, error), qPrintable(error));
      QCOMPARE(pane.jobs().plugins().size(), 1);
      QVERIFY(pane.jobs().jobs().isEmpty());
      pane.uninstallPlugin("workbench-explorer");
      QVERIFY(pane.jobs().plugins().isEmpty());
    }
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    QVERIFY(pane.jobs().plugins().isEmpty());
    QVERIFY(pane.contextActions().isEmpty());
  }

  void unavailableRememberedManifestDoesNotBlockStartup() {
    QTemporaryDir directory;
    const auto path = prepare(directory.filePath("remembered"), false);
    QString error;
    {
      Session session(QStringLiteral(TEST_WORKER));
      ActionRegistry registry;
      AnalysisPlugins pane(session, registry);
      QVERIFY2(pane.installPlugin(path, error), qPrintable(error));
    }
    QVERIFY(QFile::remove(path));
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    QSignalSpy messages(&pane, &AnalysisPlugins::message);
    QVERIFY(pane.jobs().plugins().isEmpty());
    QVERIFY(pane.jobs().jobs().isEmpty());
    QTRY_COMPARE(messages.size(), 1);
    QVERIFY(messages.first().first().toString().contains(path));
    QVERIFY(QSettings().contains("guiPlugins/installed/workbench-explorer"));
    const auto repaired = prepare(directory.filePath("relocated"), false);
    QVERIFY2(pane.installPlugin(repaired, error), qPrintable(error));
    QCOMPARE(pane.contextActions().size(), 3);
    pane.uninstallPlugin("workbench-explorer");
  }

  void queryAndResultSurvivePaneDestruction() {
    QTemporaryDir directory;
    QFile input(directory.filePath("sample.bin"));
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Session session(QStringLiteral(TEST_WORKER));
    session.open(input.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 7000);
    const auto path = prepare(directory.filePath("plugin"));
    QFile script(directory.filePath("plugin/explorer.py"));
    QVERIFY(script.open(QIODevice::WriteOnly | QIODevice::Truncate));
    script.write(R"PY(from neverd_gui_plugin import run
import time
def handler(task):
    (task.output_dir / 'ready').write_text('ready')
    while not (task.output_dir / 'continue').exists():
        time.sleep(0.01)
    metadata = task.query('metadata')
    return dict(views=[dict(kind='text', title='Query after close', text=str(metadata))])
raise SystemExit(run(handler))
)PY");
    script.close();
    ActionRegistry registry;
    auto pane = std::make_unique<AnalysisPlugins>(session, registry);
    QString error;
    QVERIFY2(pane->jobs().install(path, error), qPrintable(error));
    const auto id =
        pane->jobs().start("workbench-explorer", "context", {}, error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    auto &jobs = session.pluginServices().jobs();
    const auto output = jobs.job(id)->directory;
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(output + "/ready"), 7000);
    pane.reset();
    QVERIFY(!jobs.job(id)->terminal());
    QFile proceed(output + "/continue");
    QVERIFY(proceed.open(QIODevice::WriteOnly));
    proceed.close();
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("succeeded"));
    pane = std::make_unique<AnalysisPlugins>(session, registry);
    QCOMPARE(pane->jobs().job(id)->result.value("views").toArray().size(), 1);
    QVERIFY(pane->findChild<QAction *>("plugin.workbench-explorer:context"));
  }

  void actualContextMenusUseTheirInvokingFunction_data() {
    QTest::addColumn<QString>("surface");
    for (const auto &name : {"listing", "graph", "code", "functions"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }

  void actualContextMenusUseTheirInvokingFunction() {
    QFETCH(QString, surface);
    QTemporaryDir directory;
    QFile fixture(directory.filePath("sample.bin"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("fixture");
    fixture.close();
    Session session(QStringLiteral(TEST_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setAsksForDebugInfo(false);
    window.setLayoutPath(directory.filePath("layout.json"));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    auto *pane = window.analysisPlugins();
    QString error;
    QVERIFY2(pane->jobs().install(prepare(directory.filePath("plugin"), false),
                                  error),
             qPrintable(error));
    window.openFile(fixture.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 7000);
    auto *table = window.findChild<QTreeView *>("functionsList");
    QVERIFY(table);
    auto *functions = qobject_cast<ChooserView *>(table->parentWidget());
    QVERIFY(functions);
    QTRY_VERIFY(functions->model().rowCount() > 20);
    std::optional<Address> original, other;
    QTRY_VERIFY((original = functions->model().addressAt(0)).has_value());
    QTRY_VERIFY((other = functions->model().addressAt(20)).has_value());
    QVERIFY(original != other);
    auto *assembly = window.disassembly();
    assembly->navigate(*original);
    QTRY_COMPARE(assembly->currentFunction(), original);
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));

    std::optional<Address> expected = original;
    std::function<void()> openMenu;
    if (surface == "functions") {
      expected = other;
      const auto row = functions->model().index(20, 0);
      table->scrollTo(row);
      table->setFocus();
      openMenu = [table, row] {
        emit table->customContextMenuRequested(table->visualRect(row).center());
      };
    } else if (surface == "code") {
      expected = other;
      window.actions().action(ActionId::ViewPseudocode)->trigger();
      auto *code = window.findChild<CodeView *>();
      QVERIFY(code);
      code->showFunction(*expected);
      QTRY_VERIFY(!code->text()->loading());
      QTRY_COMPARE(code->text()->function(), expected);
      code->text()->setFocus();
      QTRY_VERIFY(code->text()->hasFocus());
      openMenu = [code] {
        emit code->text()->contextMenuRequested(
            code->text()->viewport()->mapToGlobal(QPoint(40, 40)));
      };
    } else if (surface == "graph") {
      window.actions().action(ActionId::ViewToggleGraph)->trigger();
      QTRY_VERIFY(!assembly->graph()->nodes().isEmpty());
      assembly->focusContent();
      openMenu = [assembly] {
        emit assembly->graph()->contextMenuRequested(
            assembly->graph()->mapToGlobal(QPoint(40, 40)));
      };
    } else {
      assembly->focusContent();
      openMenu = [assembly] {
        emit assembly->listing()->contextMenuRequested(
            assembly->listing()->mapToGlobal(QPoint(40, 40)));
      };
    }
    bool inspected = false, offered = false, enabled = false;
    QTimer probe;
    connect(&probe, &QTimer::timeout, this, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      probe.stop();
      inspected = true;
      auto *group =
          menu->findChild<QMenu *>("pluginContextGroup.workbench-explorer");
      auto *report =
          pane->findChild<QAction *>("plugin.workbench-explorer:report");
      offered = group && report && group->actions().contains(report);
      enabled = offered && report->isEnabled();
      if (enabled)
        report->trigger();
      menu->close();
    });
    probe.start(5);
    openMenu();
    QVERIFY(inspected);
    QVERIFY(offered);
    QVERIFY(enabled);
    QTRY_COMPARE(pane->jobs().jobs().size(), 1);
    const auto job = pane->jobs().jobs().first();
    QCOMPARE(job.context.value("function_address").toString(),
             hexAddress(*expected));
    if (surface == "functions")
      QCOMPARE(job.context.value("address").toString(), hexAddress(*expected));
    QTRY_VERIFY_WITH_TIMEOUT(pane->jobs().job(job.id)->terminal(), 10000);
    QVERIFY2(pane->jobs().job(job.id)->state == "succeeded",
             qPrintable(pane->jobs().job(job.id)->diagnostic));
  }
  void serviceEventsKeepCapturedRevision() {
    QTemporaryDir directory;
    const auto manifestPath = prepare(directory.filePath("provider"));
    QFile manifest(manifestPath);
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    auto declaration = QJsonDocument::fromJson(manifest.readAll()).object();
    manifest.close();
    declaration.insert(
        "services",
        QJsonArray{QJsonObject{{"id", "project.context"}, {"version", 1}},
                   QJsonObject{{"id", "analysis.metadata"}, {"version", 1}}});
    declaration.insert(
        "events", QJsonArray{QJsonObject{{"id", "project.revision_changed"},
                                         {"version", 1}}});
    QVERIFY(manifest.open(QIODevice::WriteOnly));
    manifest.write(QJsonDocument(declaration).toJson());
    manifest.close();
    QFile script(directory.filePath("provider/explorer.py"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(from neverd_gui_plugin import run, QueryError
def handler(task):
    subscription = task.subscribe('project.revision_changed')
    assert task.service('analysis.metadata')['value'] == 42
    event = task.next_event()['event']
    assert event['context']['revision'] == '2'
    assert task.context['revision'] == '1'
    assert task.service('project.context')['revision'] == '2'
    try: task.service('analysis.metadata')
    except QueryError as error: assert error.code == 'stale_revision'
    else: raise AssertionError('stale input accepted')
    task.unsubscribe(subscription)
    return dict(views=[dict(kind='text', title='Event', text=event['id'])])
raise SystemExit(run(handler))
)PY");
    script.close();
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY2(jobs.install(manifestPath, error), qPrintable(error));
    auto captured = context();
    captured.insert("revision", "1");
    jobs.setContext(captured);
    int queries = 0;
    connect(&jobs, &PluginJobs::queryRequested, &jobs,
            [&](const QString &task, const QString &id,
                const QString &operation, const QJsonObject &) {
              QCOMPARE(operation, QStringLiteral("metadata"));
              ++queries;
              auto next = captured;
              next.insert("revision", "2");
              jobs.setContext(next);
              jobs.answerQuery(
                  task, id,
                  {{"status", "ok"}, {"payload", QJsonObject{{"value", 42}}}});
            });
    const auto id = jobs.start("workbench-explorer", "context", {}, error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("succeeded"));
    QCOMPARE(queries, 1);
  }
  void standaloneExampleAndArtifact() {
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY2(jobs.install(prepare(directory.filePath(
                              QStringLiteral("external plugin 中文"))),
                          error),
             qPrintable(error));
    jobs.setContext(context());
    auto parameters = defaults(jobs, "background");
    parameters.insert("steps", 3);
    parameters.insert("delay_ms", 0);
    QSignalSpy progress(&jobs, &PluginJobs::jobsChanged);
    const auto id =
        jobs.start("workbench-explorer", "background", parameters, error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("succeeded"));
    QVERIFY(progress.size() >= 4);
    QCOMPARE(jobs.job(id)->progress.value("done").toInt(), 3);
    const auto exported = directory.filePath("export.json");
    QVERIFY2(jobs.exportArtifact(id, 0, exported, error), qPrintable(error));
    QVERIFY(QFileInfo::exists(exported));
    QVERIFY(!jobs.exportArtifact(id, 0, exported, error));
    QFile changed(jobs.job(id)->directory + QStringLiteral("/report.json"));
    QVERIFY(changed.open(QIODevice::WriteOnly));
    changed.write("changed");
    changed.close();
    QVERIFY(
        !jobs.exportArtifact(id, 0, directory.filePath("second.json"), error));
    QVERIFY(QFileInfo::exists(jobs.job(id)->directory +
                              QStringLiteral("/receipt.json")));
  }
  void imageAdditionRechecksProjectInputAndArtifact() {
    QTemporaryDir directory;
    const auto manifest = prepare(directory.filePath("provider"));
    QFile script(directory.filePath("provider/explorer.py"));
    QVERIFY(script.open(QIODevice::WriteOnly));
    script.write(R"PY(from pathlib import Path
import hashlib
from neverd_gui_plugin import run
def handler(task):
    (task.output_dir / 'code.bin').write_bytes(bytes.fromhex('b82a000000c3'))
    return dict(views=[], artifacts=[task.artifact('code.bin', 'Machine code')],
        image_addition=dict(artifact=0, sample_sha256=hashlib.sha256(Path(task.context['input_path']).read_bytes()).hexdigest(),
          description=dict(schema='neverd.analysis-section.v1', name='.test', address='0x500000', permissions='rx',
            functions=[dict(name='returns42', offset='0x0', size=6)])))
raise SystemExit(run(handler))
)PY");
    script.close();
    const auto input = directory.filePath("sample.bin");
    QFile source(input);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("original");
    source.close();
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY2(jobs.install(manifest, error), qPrintable(error));
    auto captured = context();
    captured.insert("input_path", input);
    jobs.setContext(captured);
    const auto id = jobs.start("workbench-explorer", "report",
                               defaults(jobs, "report"), error);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("succeeded"));
    QJsonObject description;
    QVERIFY2(jobs.analysisSection(id, description, error), qPrintable(error));
    QCOMPARE(description.value("data_hex").toString(), "b82a000000c3");
    auto changed = captured;
    changed.insert("revision", "2");
    jobs.setContext(changed);
    QVERIFY(!jobs.analysisSection(id, description, error));
    changed = captured;
    changed.insert("epoch", "2");
    jobs.setContext(changed);
    QVERIFY(!jobs.analysisSection(id, description, error));
    jobs.setContext(captured);
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("changed");
    source.close();
    QVERIFY(!jobs.analysisSection(id, description, error));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("original");
    source.close();
    QFile code(jobs.job(id)->directory + "/code.bin");
    QVERIFY(code.open(QIODevice::WriteOnly));
    code.write("tampered");
    code.close();
    QVERIFY(!jobs.analysisSection(id, description, error));
  }
  void failureModes_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("state");
    QTest::newRow("reported")
        << QStringLiteral("reported") << QStringLiteral("failed");
    QTest::newRow("crash") << QStringLiteral("crash")
                           << QStringLiteral("failed");
    QTest::newRow("malformed-result")
        << QStringLiteral("malformed") << QStringLiteral("failed");
    QTest::newRow("timeout")
        << QStringLiteral("timeout") << QStringLiteral("timed_out");
  }
  void failureModes() {
    QFETCH(QString, mode);
    QFETCH(QString, state);
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY(jobs.install(prepare(directory.filePath("plugin")), error));
    jobs.setContext(context());
    const auto id =
        jobs.start("workbench-explorer", "failure", {{"mode", mode}}, error);
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, state);
    QVERIFY(jobs.job(id)->result.isEmpty());
    QVERIFY(!jobs.job(id)->diagnostic.isEmpty());
  }
  void cancellationQueueAndProjectChange() {
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY(jobs.install(prepare(directory.filePath("plugin")), error));
    jobs.setContext(context());
    auto parameters = defaults(jobs, "background");
    parameters.insert("steps", 100);
    const auto first =
        jobs.start("workbench-explorer", "background", parameters, error);
    const auto queued = jobs.start("workbench-explorer", "context", {}, error);
    QVERIFY(!first.isEmpty() && !queued.isEmpty());
    jobs.cancel(queued);
    QCOMPARE(jobs.job(queued)->state, QStringLiteral("cancelled"));
    QTRY_COMPARE_WITH_TIMEOUT(jobs.job(first)->state, QStringLiteral("running"),
                              5000);
    jobs.cancel(first);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.job(first)->state,
                              QStringLiteral("cancelled"), 5000);
    const auto stale =
        jobs.start("workbench-explorer", "background", parameters, error);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.job(stale)->state, QStringLiteral("running"),
                              5000);
    jobs.setContext(context("2"));
    QTRY_COMPARE_WITH_TIMEOUT(jobs.job(stale)->state, QStringLiteral("stale"),
                              5000);
    QVERIFY(jobs.job(stale)->result.isEmpty());
    const auto unload =
        jobs.start("workbench-explorer", "background", parameters, error);
    QTRY_COMPARE_WITH_TIMEOUT(jobs.job(unload)->state,
                              QStringLiteral("running"), 5000);
    jobs.uninstall("workbench-explorer");
    QTRY_COMPARE_WITH_TIMEOUT(jobs.job(unload)->state,
                              QStringLiteral("cancelled"), 5000);
    QVERIFY(jobs.plugins().isEmpty());
  }
  void validatesBeforeRegistrationOrExecution() {
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    const auto path = prepare(directory.filePath("plugin"));
    QVERIFY(jobs.install(path, error));
    // Reimporting the same active declaration is idempotent in every consumer.
    QVERIFY(jobs.install(path, error));
    QCOMPARE(jobs.plugins().size(), 1);
    jobs.setContext({{"epoch", "1"}, {"loaded", false}});
    QVERIFY(jobs.start("workbench-explorer", "overview", {}, error).isEmpty());
    QVERIFY(
        jobs.start("workbench-explorer", "background", {{"steps", -1}}, error)
            .isEmpty());
    QVERIFY(jobs.jobs().isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    root.insert("id", "another");
    root.insert("queries", QJsonArray{"rename"});
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();
    QVERIFY(!jobs.install(path, error));
    QCOMPARE(jobs.plugins().size(), 1);
    QVERIFY(!validatePluginResult(
        {{"views",
          QJsonArray{QJsonObject{
              {"kind", "graph"},
              {"title", "Bad graph"},
              {"nodes", QJsonArray{}},
              {"edges", QJsonArray{QJsonObject{{"from", "missing"},
                                               {"to", "missing"}}}}}}}},
        error));
  }
  void imageAdditionRejectsPathsInlineDataAndInvalidIndices() {
    QString error;
    QJsonObject description{{"schema", "neverd.analysis-section.v1"},
                            {"name", ".test"},
                            {"address", "0x500000"},
                            {"permissions", "rx"},
                            {"functions", QJsonArray{}}};
    QJsonObject addition{{"artifact", 0},
                         {"sample_sha256", QString(64, 'a')},
                         {"description", description}};
    QJsonObject result{
        {"views", QJsonArray{}},
        {"artifacts", QJsonArray{QJsonObject{{"title", "Code"},
                                             {"path", "code.bin"},
                                             {"sha256", QString(64, 'b')}}}},
        {"image_addition", addition}};
    QVERIFY2(validatePluginResult(result, error), qPrintable(error));
    for (const auto &index :
         {QJsonValue(-1), QJsonValue(1), QJsonValue(0.5), QJsonValue("0")}) {
      auto invalid = addition;
      invalid.insert("artifact", index);
      result.insert("image_addition", invalid);
      QVERIFY(!validatePluginResult(result, error));
    }
    for (const auto *field : {"path", "data_hex"}) {
      auto invalid = description;
      invalid.insert(field, "untrusted");
      addition.insert("description", invalid);
      result.insert("image_addition", addition);
      QVERIFY(!validatePluginResult(result, error));
    }
  }
  void manifestQueryDiagnostics_data() {
    QTest::addColumn<QJsonArray>("queries");
    QTest::addColumn<QString>("diagnostic");
    QTest::newRow("unsupported")
        << QJsonArray{"rename"}
        << QStringLiteral("Unsupported plugin query 'rename'");
    QTest::newRow("repeated")
        << QJsonArray{"metadata", "metadata"}
        << QStringLiteral("Repeated plugin query 'metadata'");
    QTest::newRow("not-string")
        << QJsonArray{42}
        << QStringLiteral("Plugin queries must contain strings");
  }
  void manifestQueryDiagnostics() {
    QFETCH(QJsonArray, queries);
    QFETCH(QString, diagnostic);
    QTemporaryDir directory;
    const auto path = prepare(directory.filePath("plugin"), false);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    manifest.insert("queries", queries);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(manifest).toJson());
    file.close();
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY(!jobs.install(path, error));
    QVERIFY2(error.startsWith(diagnostic), qPrintable(error));
    QVERIFY(jobs.plugins().isEmpty());
    QVERIFY(jobs.jobs().isEmpty());
    if (diagnostic.startsWith("Unsupported"))
      QVERIFY(error.contains("imports"));
  }
  void unloadWhileStarting() {
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    QVERIFY(jobs.install(prepare(directory.filePath("plugin")), error));
    jobs.setContext(context());
    bool unloaded = false;
    connect(&jobs, &PluginJobs::jobsChanged, &jobs, [&] {
      if (!unloaded && !jobs.jobs().isEmpty() &&
          jobs.jobs().first().state == QLatin1String("starting")) {
        unloaded = true;
        jobs.uninstall("workbench-explorer");
      }
    });
    const auto id = jobs.start("workbench-explorer", "context", {}, error);
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QVERIFY(unloaded);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("cancelled"));
  }
  void resolvedActionCannotCallReplacement() {
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    const auto manifest = prepare(directory.filePath("plugin"));
    QVERIFY(jobs.install(manifest, error));
    const auto old = jobs.actionBinding("workbench-explorer", "context");
    QVERIFY(old);
    jobs.uninstall("workbench-explorer");
    QTRY_VERIFY(!jobs.retiring("workbench-explorer"));
    QVERIFY2(jobs.install(manifest, error), qPrintable(error));
    QVERIFY(
        jobs.start("workbench-explorer", "context", {}, error, old).isEmpty());
    QVERIFY(error.contains("registration"));
    QVERIFY(jobs.jobs().isEmpty());
    const auto current = jobs.actionBinding("workbench-explorer", "context");
    QVERIFY(current && current->id() != old->id());
    const auto id =
        jobs.start("workbench-explorer", "context", {}, error, current);
    QVERIFY2(!id.isEmpty(), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("succeeded"));
  }
  void protocolRefusals_data() {
    QTest::addColumn<QString>("mode");
    for (const auto *mode :
         {"startup-error", "oversized", "invalid-json", "wrong-task",
          "unauthorized-query", "missing-result", "bad-hash",
          "escaping-artifact", "invalid-progress", "partial-frame",
          "result-then-crash"})
      QTest::newRow(mode) << QString::fromLatin1(mode);
  }
  void protocolRefusals() {
    QFETCH(QString, mode);
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    const auto path = prepare(directory.filePath("plugin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QFile::copy(QStringLiteral(TEST_PROVIDER_FIXTURE),
                directory.filePath("plugin/fixture.py"));
    root.insert("arguments", QJsonArray{"fixture.py", mode});
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();
    QVERIFY(jobs.install(path, error));
    jobs.setContext(context());
    const auto id = jobs.start("workbench-explorer", "context", {}, error);
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("failed"));
    QVERIFY(jobs.job(id)->result.isEmpty());
    if (mode == QLatin1String("startup-error")) {
      QVERIFY(jobs.job(id)->diagnostic.contains(
          QStringLiteral("missing_helper_fixture")));
      QFile receipt(jobs.job(id)->directory + QStringLiteral("/receipt.json"));
      QVERIFY(receipt.open(QIODevice::ReadOnly));
      QVERIFY(QJsonDocument::fromJson(receipt.readAll())
                  .object()
                  .value("stderr")
                  .toString()
                  .contains(QStringLiteral("missing_helper_fixture")));
    }
  }
  void descendantsAreStoppedOnProviderExit() {
    QTemporaryDir directory;
    PluginJobs jobs(directory.filePath("jobs"));
    QString error;
    const auto path = prepare(directory.filePath("plugin"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QFile::copy(QStringLiteral(TEST_PROVIDER_FIXTURE),
                directory.filePath("plugin/fixture.py"));
    root.insert("arguments", QJsonArray{"fixture.py", "descendant"});
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QJsonDocument(root).toJson());
    file.close();
    QVERIFY(jobs.install(path, error));
    jobs.setContext(context());
    const auto id = jobs.start("workbench-explorer", "context", {}, error);
    QVERIFY(!id.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(jobs.job(id)->terminal(), 7000);
    QCOMPARE(jobs.job(id)->state, QStringLiteral("succeeded"));
    QTest::qWait(1200);
    QVERIFY(!QFileInfo::exists(jobs.job(id)->directory +
                               QStringLiteral("/descendant-survived.txt")));
  }
  void guiParameterDialogAndSettings() {
    QTemporaryDir directory;
    Session session(QStringLiteral(TEST_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setAsksForDebugInfo(false);
    window.setLayoutPath(directory.filePath("layout.json"));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    auto *pane = window.analysisPlugins();
    QString error;
    QVERIFY(pane->jobs().install(prepare(directory.filePath("plugin")), error));
    auto *action = pane->findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:background"));
    QVERIFY(action);
    QTRY_VERIFY(action->isEnabled());
    bool edited = false;
    QTimer::singleShot(0, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      auto *steps = dialog->findChild<QSpinBox *>(
          QStringLiteral("pluginParameter.steps"));
      auto *delay = dialog->findChild<QSpinBox *>(
          QStringLiteral("pluginParameter.delay_ms"));
      auto *message = dialog->findChild<QLineEdit *>(
          QStringLiteral("pluginParameter.message"));
      auto *choice = dialog->findChild<QComboBox *>(
          QStringLiteral("pluginParameter.letter_case"));
      auto *report = dialog->findChild<QCheckBox *>(
          QStringLiteral("pluginParameter.write_report"));
      if (steps && delay && message && choice && report) {
        steps->setValue(1);
        delay->setValue(0);
        message->setText(QStringLiteral("<b>plain text</b> 中文"));
        choice->setCurrentText("upper");
        report->setChecked(false);
        dialog->findChild<QCheckBox *>("pluginRememberParameters")
            ->setChecked(true);
        edited = true;
      }
      dialog->accept();
    });
    action->trigger();
    QVERIFY(edited);
    QVERIFY(!pane->jobs().jobs().isEmpty());
    const auto id = pane->jobs().jobs().last().id;
    QTRY_VERIFY_WITH_TIMEOUT(pane->jobs().job(id)->terminal(), 7000);
    QCOMPARE(pane->jobs().job(id)->state, QStringLiteral("succeeded"));
    QVERIFY(
        pane->jobs().job(id)->result.value("artifacts").toArray().isEmpty());
    QCOMPARE(QSettings()
                 .value("guiPlugins/workbench-explorer/background/steps")
                 .toInt(),
             1);
    QVERIFY(pane->jobs()
                .job(id)
                ->result.value("views")
                .toArray()
                .first()
                .toObject()
                .value("text")
                .toString()
                .contains(QStringLiteral("<B>PLAIN TEXT</B>")));
    const auto screenshot = qEnvironmentVariable("NEVERD_PLUGIN_SCREENSHOT");
    if (!screenshot.isEmpty())
      QVERIFY(window.grab().save(screenshot));
  }
  void rememberedParametersSurviveTextSettings() {
    QTemporaryDir directory;
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    QString error;
    QVERIFY2(pane.jobs().install(prepare(directory.filePath("plugin")), error),
             qPrintable(error));
    // INI files and the registry return remembered values as text after a
    // restart, unlike the typed values this process wrote.
    const QString prefix("guiPlugins/workbench-explorer/background/");
    QSettings settings;
    settings.setValue(prefix + "steps", QStringLiteral("2"));
    settings.setValue(prefix + "write_report", QStringLiteral("false"));
    settings.setValue(prefix + "letter_case", QStringLiteral("upper"));
    // Above the declared maximum: the dialog offers the default instead.
    settings.setValue(prefix + "delay_ms", QStringLiteral("5000"));
    auto *action = pane.findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:background"));
    QVERIFY(action);
    QTRY_VERIFY(action->isEnabled());
    bool restored = false;
    QTimer::singleShot(0, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      auto *steps = dialog->findChild<QSpinBox *>(
          QStringLiteral("pluginParameter.steps"));
      auto *delay = dialog->findChild<QSpinBox *>(
          QStringLiteral("pluginParameter.delay_ms"));
      auto *letters = dialog->findChild<QComboBox *>(
          QStringLiteral("pluginParameter.letter_case"));
      auto *report = dialog->findChild<QCheckBox *>(
          QStringLiteral("pluginParameter.write_report"));
      restored = steps && steps->value() == 2 && delay &&
                 delay->value() == 100 && letters &&
                 letters->currentText() == QLatin1String("upper") && report &&
                 !report->isChecked();
      dialog->reject();
    });
    action->trigger();
    for (const auto *key : {"steps", "write_report", "letter_case", "delay_ms"})
      settings.remove(prefix + key);
    QVERIFY(restored);
  }
  void guiMenusQueriesViewsAndUnload() {
    QTemporaryDir directory;
    const auto realInput = qEnvironmentVariable("NEVERD_PLUGIN_REAL_INPUT");
    Session session(qEnvironmentVariable("NEVERD_PLUGIN_REAL_WORKER",
                                         QStringLiteral(TEST_WORKER)));
    if (!realInput.isEmpty())
      connect(&session, &Session::message, this,
              [](const QString &text, int level) { qInfo() << level << text; });
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker);
    window.setAsksForDebugInfo(false);
    window.setLayoutPath(directory.filePath("layout.json"));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    auto *pane = window.analysisPlugins();
    QVERIFY(pane);
    QString error;
    QVERIFY(pane->jobs().install(prepare(directory.filePath("plugin"), false),
                                 error));
    auto *showLog =
        pane->findChild<QPushButton *>(QStringLiteral("pluginShowLog"));
    auto *log =
        pane->findChild<QPlainTextEdit *>(QStringLiteral("pluginTaskLog"));
    QVERIFY(showLog && log);
    QVERIFY(log->isHidden());
    QVERIFY(showLog->parentWidget()->isHidden());
    QCOMPARE(pane->contextActions().size(), 3);
    QVERIFY(!pane->findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:failure")));
    auto *overview = pane->findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:overview"));
    QVERIFY(overview);
    QVERIFY(!overview->isEnabled());
    QFile fixture(directory.filePath("sample.bin"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("fixture");
    fixture.close();
    const QPointer<QAction> previousOverview = overview;
    window.openFile(realInput.isEmpty() ? fixture.fileName() : realInput);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded() || !session.lastError().isEmpty(),
                             7000);
    QVERIFY2(session.loaded(), qPrintable(session.lastError()));
    QVERIFY(previousOverview.isNull());
    overview = pane->findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:overview"));
    QVERIFY(overview);
    QTRY_VERIFY(overview->isEnabled());
    const auto registered = window.actions().allActions();
    QVERIFY(registered.contains(overview));
    overview->trigger();
    QTRY_VERIFY(!pane->jobs().jobs().isEmpty());
    const auto id = pane->jobs().jobs().last().id;
    QTRY_VERIFY_WITH_TIMEOUT(pane->jobs().job(id)->terminal(), 7000);
    QVERIFY2(pane->jobs().job(id)->state == QLatin1String("succeeded"),
             qPrintable(pane->jobs().job(id)->diagnostic));
    auto *tabs =
        pane->findChild<QTabWidget *>(QStringLiteral("pluginResultViews"));
    QVERIFY(tabs);
    QTRY_COMPARE(tabs->count(), 2);
    showLog->click();
    QVERIFY(!log->isHidden());
    showLog->click();
    QVERIFY(log->isHidden());
    auto *table = qobject_cast<QTableWidget *>(tabs->widget(0));
    QVERIFY(table);
    QVERIFY(table->rowCount() > 0);
    QSignalSpy navigation(pane, &AnalysisPlugins::navigateRequested);
    QMetaObject::invokeMethod(table, "itemActivated",
                              Q_ARG(QTableWidgetItem *, table->item(0, 0)));
    QCOMPARE(navigation.size(), 1);
    window.disassembly()->navigate(realInput.isEmpty() ? 0xffff800012340140ULL
                                                       : 0x180001654ULL);
    auto *reportAction = pane->findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:report"));
    QVERIFY(reportAction);
    QTRY_VERIFY(reportAction->isEnabled());
    reportAction->trigger();
    const auto reportTask = pane->jobs().jobs().last().id;
    QTRY_VERIFY_WITH_TIMEOUT(pane->jobs().job(reportTask)->terminal(), 7000);
    QVERIFY2(pane->jobs().job(reportTask)->state == QLatin1String("succeeded"),
             qPrintable(pane->jobs().job(reportTask)->diagnostic));
    QTRY_COMPARE(tabs->count(), 4);
    QCOMPARE(pane->jobs()
                 .job(reportTask)
                 ->result.value("artifacts")
                 .toArray()
                 .size(),
             2);
    const auto screenshot =
        qEnvironmentVariable("NEVERD_PLUGIN_REPORT_SCREENSHOT");
    if (!screenshot.isEmpty())
      QVERIFY(window.grab().save(screenshot));
    auto *graphAction = pane->findChild<QAction *>(
        QStringLiteral("plugin.workbench-explorer:graph"));
    QVERIFY(graphAction);
    QTRY_VERIFY(graphAction->isEnabled());
    graphAction->trigger();
    const auto graphTask = pane->jobs().jobs().last().id;
    QTRY_VERIFY_WITH_TIMEOUT(pane->jobs().job(graphTask)->terminal(), 7000);
    QVERIFY2(pane->jobs().job(graphTask)->state == QLatin1String("succeeded"),
             qPrintable(pane->jobs().job(graphTask)->diagnostic));
    QTRY_COMPARE(tabs->count(), 5);
    QVERIFY(qobject_cast<QGraphicsView *>(tabs->widget(4)));
    const auto actionCount = window.actions().allActions().size();
    pane->jobs().uninstall("workbench-explorer");
    QCOMPARE(window.actions().allActions().size(), actionCount - 4);
    QVERIFY(pane->contextActions().isEmpty());
  }

  void sessionChangesPublishOneContext() {
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    auto focused = context();
    focused.insert("selection_text", QStringLiteral("focused view"));
    pane.setSelectionProvider([&] { return focused; });
    // Every context published for a session change comes from the focused
    // view; a second publisher would alternate it and its host events.
    QList<QJsonObject> published;
    connect(&pane.jobs(), &PluginJobs::jobsChanged, this,
            [&] { published.append(pane.jobs().context()); });
    emit session.stateChanged();
    emit session.selectionChanged(QJsonObject{{"address", "0x10"}});
    emit session.revisionChanged();
    QVERIFY(!published.isEmpty());
    for (const auto &value : published)
      QCOMPARE(value.value("selection_text").toString(),
               QStringLiteral("focused view"));
  }

  void contextMenuGroupsOverridesAndPersistence() {
    Theme::instance().apply();
    QTemporaryDir directory;
    Session session(QStringLiteral(TEST_WORKER));
    ActionRegistry registry;
    AnalysisPlugins pane(session, registry);
    pane.setSelectionProvider([] { return context(); });
    const auto install = [&](const QString &id, const QString &name) {
      const auto path = prepare(directory.filePath(id), false);
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly))
        return QString{};
      auto manifest = QJsonDocument::fromJson(file.readAll()).object();
      file.close();
      manifest.insert("id", id);
      manifest.insert("name", name);
      if (!file.open(QIODevice::WriteOnly))
        return QString{};
      file.write(QJsonDocument(manifest).toJson());
      return path;
    };
    const auto firstPath = install("menu-alpha", "Alpha tools");
    const auto secondPath = install("menu-beta", "Beta tools");
    QString error;
    QVERIFY2(pane.jobs().install(firstPath, error), qPrintable(error));
    QVERIFY2(pane.jobs().install(secondPath, error), qPrintable(error));
    QMenu grouped;
    pane.populateContextMenu(grouped);
    auto *root =
        grouped.findChild<QMenu *>(QStringLiteral("pluginContextRoot"));
    QVERIFY(root);
    QCOMPARE(root->actions().size(), 2);
    auto *alpha = root->findChild<QMenu *>(
        QStringLiteral("pluginContextGroup.menu-alpha"));
    auto *beta = root->findChild<QMenu *>(
        QStringLiteral("pluginContextGroup.menu-beta"));
    QVERIFY(alpha && beta);
    QCOMPARE(alpha->title(), QStringLiteral("Alpha tools"));
    QCOMPARE(alpha->actions().size(), 3);
    QCOMPARE(beta->actions().size(), 3);
    auto *report =
        pane.findChild<QAction *>(QStringLiteral("plugin.menu-alpha:report"));
    QVERIFY(report && !report->icon().isNull());
    QVERIFY(!report->isEnabled());
    const auto highDpi = report->icon().pixmap(QSize(16, 16), 2.0);
    QVERIFY(!highDpi.isNull());
    QCOMPARE(highDpi.size(), QSize(32, 32));

    const auto imagePath = directory.filePath(QStringLiteral("用户图标.png"));
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(Qt::red);
    QVERIFY(image.save(imagePath));
    auto *settingsButton =
        pane.findChild<QPushButton *>(QStringLiteral("pluginMenuSettings"));
    QVERIFY(settingsButton);
    bool edited = false;
    bool invalidRejected = false;
    QTimer::singleShot(0, &pane, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      auto *position = dialog->findChild<QComboBox *>(
          QStringLiteral("pluginMenuPlacement.menu-alpha:report"));
      auto *imageField = dialog->findChild<QLineEdit *>(
          QStringLiteral("pluginMenuIcon.menu-alpha:report"));
      if (position && imageField) {
        position->setCurrentIndex(position->findData(QStringLiteral("inline")));
        imageField->setText(directory.filePath("missing.png"));
        if (auto *buttons = dialog->findChild<QDialogButtonBox *>()) {
          buttons->button(QDialogButtonBox::Ok)->click();
          const auto *diagnostic = dialog->findChild<QLabel *>(
              QStringLiteral("pluginMenuSettingsError"));
          invalidRejected = dialog->isVisible() && diagnostic &&
                            !diagnostic->text().isEmpty() &&
                            !QSettings().contains(
                                "guiPlugins/menu-alpha/menus/report/placement");
        }
        imageField->setText(imagePath);
        edited = true;
      }
      const auto screenshot =
          qEnvironmentVariable("NEVERD_PLUGIN_MENU_SETTINGS_SCREENSHOT");
      if (!screenshot.isEmpty())
        dialog->grab().save(screenshot);
      if (auto *buttons = dialog->findChild<QDialogButtonBox *>())
        buttons->button(QDialogButtonBox::Ok)->click();
      else
        dialog->reject();
    });
    settingsButton->click();
    QVERIFY(edited);
    QVERIFY(invalidRejected);
    QCOMPARE(
        pane.findChild<QAction *>(QStringLiteral("plugin.menu-alpha:report")),
        report);
    QCOMPARE(QSettings()
                 .value("guiPlugins/menu-alpha/menus/report/placement")
                 .toString(),
             QStringLiteral("inline"));
    QCOMPARE(report->icon().pixmap(16, 16).toImage().pixelColor(8, 8),
             QColor(Qt::red));
    QMenu mixed;
    pane.populateContextMenu(mixed);
    QVERIFY(mixed.actions().contains(report));
    alpha = mixed.findChild<QMenu *>(
        QStringLiteral("pluginContextGroup.menu-alpha"));
    beta = mixed.findChild<QMenu *>(
        QStringLiteral("pluginContextGroup.menu-beta"));
    QVERIFY(alpha && beta);
    QCOMPARE(alpha->actions().size(), 2);
    QCOMPARE(beta->actions().size(), 3);

    QFile fixture(directory.filePath("sample.bin"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("fixture");
    fixture.close();
    const QPointer<QAction> previousReport = report;
    session.open(fixture.fileName());
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 7000);
    QVERIFY(previousReport.isNull());
    report =
        pane.findChild<QAction *>(QStringLiteral("plugin.menu-alpha:report"));
    QVERIFY(report);
    QTRY_VERIFY(report->isEnabled());
    const auto screenshotDirectory =
        qEnvironmentVariable("NEVERD_PLUGIN_CONTEXT_SCREENSHOT_DIR");
    if (!screenshotDirectory.isEmpty()) {
      QMenu preview;
      pane.populateContextMenu(preview);
      preview.addAction(registry.action(ActionId::EditRename));
      preview.addAction(registry.action(ActionId::EditRepeatableComment));
      preview.popup(QPoint(40, 40));
      QCoreApplication::processEvents();
      QVERIFY(preview.grab().save(screenshotDirectory +
                                  QStringLiteral("/mixed-menu.png")));
      auto *groups =
          preview.findChild<QMenu *>(QStringLiteral("pluginContextRoot"));
      QVERIFY(groups);
      groups->popup(QPoint(400, 40));
      QCoreApplication::processEvents();
      QVERIFY(groups->grab().save(screenshotDirectory +
                                  QStringLiteral("/plugin-groups.png")));
      auto *commands = groups->findChild<QMenu *>(
          QStringLiteral("pluginContextGroup.menu-beta"));
      QVERIFY(commands);
      commands->popup(QPoint(620, 40));
      QCoreApplication::processEvents();
      QVERIFY(commands->grab().save(screenshotDirectory +
                                    QStringLiteral("/group-commands.png")));
      commands->hide();
      groups->hide();
      preview.hide();
    }
    report->trigger();
    QCOMPARE(pane.jobs().jobs().size(), 1);
    const auto task = pane.jobs().jobs().first().id;
    QTRY_VERIFY_WITH_TIMEOUT(pane.jobs().job(task)->terminal(), 7000);
    QVERIFY2(pane.jobs().job(task)->state == QLatin1String("succeeded"),
             qPrintable(pane.jobs().job(task)->diagnostic));

    pane.jobs().uninstall("menu-alpha");
    QMenu afterUnload;
    pane.populateContextMenu(afterUnload);
    QVERIFY(!afterUnload.findChild<QMenu *>(
        QStringLiteral("pluginContextGroup.menu-alpha")));
    QVERIFY(afterUnload.findChild<QMenu *>(
        QStringLiteral("pluginContextGroup.menu-beta")));
    QTRY_VERIFY(!pane.jobs().retiring("menu-alpha"));
    QVERIFY2(pane.jobs().install(firstPath, error), qPrintable(error));
    report =
        pane.findChild<QAction *>(QStringLiteral("plugin.menu-alpha:report"));
    QMenu reimported;
    pane.populateContextMenu(reimported);
    QVERIFY(reimported.actions().contains(report));
    QCOMPARE(report->icon().pixmap(16, 16).toImage().pixelColor(8, 8),
             QColor(Qt::red));

    // Cancel does not change the saved placement or icon.
    QTimer::singleShot(0, &pane, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      if (auto *position = dialog->findChild<QComboBox *>(
              QStringLiteral("pluginMenuPlacement.menu-alpha:report")))
        position->setCurrentIndex(0);
      dialog->reject();
    });
    settingsButton->click();
    QCOMPARE(QSettings()
                 .value("guiPlugins/menu-alpha/menus/report/placement")
                 .toString(),
             QStringLiteral("inline"));
    // Defaults only become effective after accepting the dialog.
    QTimer::singleShot(0, &pane, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      if (auto *buttons = dialog->findChild<QDialogButtonBox *>()) {
        buttons->button(QDialogButtonBox::RestoreDefaults)->click();
        buttons->button(QDialogButtonBox::Ok)->click();
      } else
        dialog->reject();
    });
    settingsButton->click();
    QMenu restored;
    pane.populateContextMenu(restored);
    QVERIFY(!restored.actions().contains(report));
    QCOMPARE(
        restored
            .findChild<QMenu *>(QStringLiteral("pluginContextGroup.menu-alpha"))
            ->actions()
            .size(),
        3);
    QVERIFY(!QSettings().contains("guiPlugins/menu-alpha/menus/report/icon"));
    for (auto *action : pane.contextActions())
      QSettings().setValue(QStringLiteral("guiPlugins/") +
                               action->property("pluginId").toString() +
                               QStringLiteral("/menus/") +
                               action->property("pluginActionId").toString() +
                               QStringLiteral("/placement"),
                           QStringLiteral("inline"));
    QMenu allInline;
    pane.populateContextMenu(allInline);
    QVERIFY(!allInline.findChild<QMenu *>(QStringLiteral("pluginContextRoot")));
    for (auto *action : pane.contextActions())
      QVERIFY(allInline.actions().contains(action));
    pane.jobs().uninstall("menu-alpha");
    pane.jobs().uninstall("menu-beta");
    QMenu empty;
    pane.populateContextMenu(empty);
    QVERIFY(empty.isEmpty());
    QSettings().remove("guiPlugins/menu-alpha");
    QSettings().remove("guiPlugins/menu-beta");
  }

  void customIconsAndManifestTypes() {
    QTemporaryDir directory;
    const auto path = prepare(directory.filePath("plugin"), false);
    QString error;
    QVERIFY(readGuiPlugin(path, error));
    QVERIFY(
        !iconFromFile(directory.filePath("plugin/icons/function-report.svg"))
             .isNull());
    const auto broken = directory.filePath("broken.svg");
    QFile file(broken);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("not an SVG");
    file.close();
    QVERIFY(iconFromFile(broken).isNull());
    QVERIFY(iconFromFile(directory.filePath("missing.png")).isNull());
    QFile manifestFile(path);
    QVERIFY(manifestFile.open(QIODevice::ReadOnly));
    auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    manifestFile.close();
    manifest.insert("icon", 42);
    QVERIFY(manifestFile.open(QIODevice::WriteOnly));
    manifestFile.write(QJsonDocument(manifest).toJson());
    manifestFile.close();
    QVERIFY(!readGuiPlugin(path, error));
  }
};
QTEST_MAIN(PluginTests)
#include "PluginTests.moc"
