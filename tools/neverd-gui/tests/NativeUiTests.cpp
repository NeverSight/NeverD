//===- NativeUiTests.cpp - Workbench UI plugin qualification -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ActionRegistry.h"
#include "AnalysisPlugins.h"
#include "GuiPluginUiHost.h"
#include "PluginManagerDialog.h"
#include "PluginServices.h"
#include "Session.h"
#include "Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBrowser>
#include <QTextCursor>
#include <QToolButton>
#include <QTreeWidget>

using namespace neverd::gui;

namespace {
class ScopedTestSettings {
public:
  explicit ScopedTestSettings(const QString &name)
      : Organization(QCoreApplication::organizationName()),
        Application(QCoreApplication::applicationName()),
        Format(QSettings::defaultFormat()) {
    QCoreApplication::setOrganizationName("NeverDNativeUiTests");
    QCoreApplication::setApplicationName(name);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       Directory.path());
  }
  ~ScopedTestSettings() {
    QCoreApplication::setOrganizationName(Organization);
    QCoreApplication::setApplicationName(Application);
    QSettings::setDefaultFormat(Format);
  }
  bool isValid() const { return Directory.isValid(); }
  QString filePath(const QString &name) const {
    return Directory.filePath(name);
  }

private:
  QTemporaryDir Directory;
  QString Organization, Application;
  QSettings::Format Format;
};

bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool write(const QString &path, const QJsonObject &object) {
  return write(path, QJsonDocument(object).toJson());
}

QString prepareInspector(const QString &dir, const QString &id) {
  const QFileInfo module(QStringLiteral(TEST_NATIVE_UI));
  if (!QDir().mkpath(dir) ||
      !QFile::copy(module.filePath(), QDir(dir).filePath(module.fileName())))
    return {};
  const auto path = QDir(dir).filePath("plugin.json");
  const QJsonObject component{
      {"id", "inspector"},
      {"kind", "gui-native"},
      {"interface", "neverd.gui-ui.v1"},
      {"entry", QJsonObject{{"library", module.fileName()}}}};
  if (!write(path, QJsonObject{{"schema", "neverd.plugin-package.v2"},
                               {"id", id},
                               {"name", "Selection Inspector"},
                               {"version", "1.0.0"},
                               {"components", QJsonArray{component}}}))
    return {};
  return path;
}

#ifdef TEST_AGENT_UI
class ModelServer final : public QObject {
public:
  ModelServer() {
    connect(&Tcp, &QTcpServer::newConnection, this, [this] {
      auto *socket = Tcp.nextPendingConnection();
      connect(
          socket, &QTcpSocket::readyRead, this,
          [this, socket, bytes = QByteArray{}]() mutable {
            bytes += socket->readAll();
            const auto end = bytes.indexOf("\r\n\r\n");
            if (end < 0)
              return;
            qsizetype length = 0;
            for (const auto &line : bytes.left(end).split('\n'))
              if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toLongLong();
            if (bytes.size() < end + 4 + length)
              return;
            Requests.append(
                QJsonDocument::fromJson(bytes.mid(end + 4, length)).object());
            Sockets.append(socket);
            socket->disconnect(this);
          });
    });
  }

  void respond(qsizetype index, const QJsonObject &output) {
    auto *socket = Sockets.at(index);
    const QJsonObject body{
        {"status", "completed"},
        {"output", QJsonArray{output}},
        {"usage", QJsonObject{{"input_tokens", 12}, {"output_tokens", 7}}}};
    const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                  "Connection: close\r\nContent-Length: " +
                  QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
    socket->disconnectFromHost();
  }

  QTcpServer Tcp;
  QList<QJsonObject> Requests;
  QList<QTcpSocket *> Sockets;
};

QJsonObject call(const QString &name, const QString &id,
                 const QJsonObject &arguments = {}) {
  return {{"type", "function_call"},
          {"name", name},
          {"call_id", id},
          {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(
                            QJsonDocument::Compact))}};
}

QJsonObject answer(const QString &text) {
  return {{"type", "message"},
          {"role", "assistant"},
          {"content",
           QJsonArray{QJsonObject{{"type", "output_text"}, {"text", text}}}}};
}

QJsonObject outputFor(const QJsonObject &request, const QString &id) {
  for (const auto &value : request.value("input").toArray()) {
    const auto item = value.toObject();
    if (item.value("type") == "function_call_output" &&
        item.value("call_id") == id)
      return QJsonDocument::fromJson(item.value("output").toString().toUtf8())
          .object();
  }
  return {};
}
#endif
} // namespace

class NativeUiTests final : public QObject {
  Q_OBJECT

private slots:
  void nativeComponentsOwnTheirViewsAndFollowProjectEpoch() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto first = prepareInspector(dir.filePath("one"), "fixture.one");
    const auto second = prepareInspector(dir.filePath("two"), "fixture.two");
    QVERIFY(!first.isEmpty() && !second.isEmpty());

    Session session(QStringLiteral(TEST_WORKER));
    auto &jobs = session.pluginServices().jobs();
    QWidget window;
    GuiPluginUiHost host(
        window, session, [&] { return window.rect(); }, [](const auto &) {});
    window.resize(640, 480);
    window.show();
    QString error;
    QVERIFY2(jobs.install(first, error), qPrintable(error));
    QVERIFY2(jobs.install(second, error), qPrintable(error));
    QVERIFY(!jobs.catalog().snapshot("fixture.one")->Record.Enabled);
    const auto labels = [&] {
      return window.findChildren<QLabel *>("NativeSelectionInspector");
    };
    QVERIFY(labels().isEmpty());
    const auto input = dir.filePath("one.bin");
    const auto next = dir.filePath("two.bin");
    QVERIFY(write(input, QByteArray("fixture")));
    QVERIFY(write(next, QByteArray("fixture")));
    session.open(input);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 7000);
    session.publishSelection(0x1010, {}, "disassembly");
    neverd::plugin::Diagnostic diagnostic;
    QVERIFY(jobs.catalog().setEnabled("fixture.one", true, diagnostic));
    QVERIFY(jobs.catalog().setEnabled("fixture.two", true, diagnostic));
    QTRY_COMPARE(labels().size(), 2);
    auto *a = labels().front();
    auto *b = labels().back();
    QTRY_VERIFY(!a->geometry().intersects(b->geometry()));
    QVERIFY(window.rect().contains(a->geometry()));
    QVERIFY(window.rect().contains(b->geometry()));
    QCOMPARE(a->text(), QStringLiteral("0x1010"));
    session.publishSelection(0x1020, {}, "disassembly");
    QTRY_COMPARE(a->text(), QStringLiteral("0x1020"));

    QPointer<QLabel> oldA(a), oldB(b);
    session.open(next);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded() && session.filePath() == next,
                             7000);
    session.publishSelection(0x2010, {}, "disassembly");
    QTRY_VERIFY(oldA.isNull() && oldB.isNull());
    QTRY_COMPARE(labels().size(), 2);
    QCOMPARE(labels().front()->text(), QStringLiteral("0x2010"));
    QVERIFY(jobs.catalog().setEnabled("fixture.one", false, diagnostic));
    QTRY_COMPARE(labels().size(), 1);
    QVERIFY(labels().front()->close());
    QTRY_VERIFY(labels().isEmpty());
    QVERIFY(jobs.catalog().component("fixture.two"));
  }

#ifdef TEST_AGENT_UI
  void bundledAgentDefaultsOnAndPreservesSavedDisablement() {
    const ScopedTestSettings settingsDirectory("BundledAgent");
    QVERIFY(settingsDirectory.isValid());
    constexpr auto id = "neverd.agent-ui";
    constexpr auto pathKey = "guiPlugins/installed/neverd.agent-ui";
    constexpr auto enabledKey = "guiPlugins/enabled/neverd.agent-ui";
    const auto manifest = QStringLiteral(TEST_AGENT_UI);
    const auto oldManifest = settingsDirectory.filePath("old/plugin.json");

    const auto check = [&](bool expectedEnabled, bool toggleEnabled) {
      Session session(QStringLiteral(TEST_WORKER));
      auto &services = session.pluginServices();
      QWidget window;
      GuiPluginUiHost host(
          window, session, [&] { return window.rect(); }, [](const auto &) {});
      services.restorePlugins(manifest);
      const auto installations = services.pluginInstallations();
      QCOMPARE(installations.size(), 1);
      const auto &agent = installations.first();
      QCOMPARE(agent.Id, QString::fromLatin1(id));
      QCOMPARE(agent.Path, manifest);
      QVERIFY(agent.Bundled);
      QCOMPARE(agent.Enabled, expectedEnabled);
      QCOMPARE(bool(window.findChild<QAbstractButton *>("NeverDAgentLauncher")),
               expectedEnabled);

      PluginManagerDialog manager(services);
      auto *uninstall =
          manager.findChild<QPushButton *>("pluginManagerUninstall");
      QVERIFY(uninstall && !uninstall->isEnabled());
      services.uninstallPlugin(id);
      QCOMPARE(services.pluginInstallations().size(), 1);
      QString error;
      QVERIFY2(services.installPlugin(manifest, error), qPrintable(error));
      if (toggleEnabled) {
        auto *toggle = manager.findChild<QPushButton *>("pluginManagerToggle");
        QVERIFY(toggle && toggle->isEnabled());
        toggle->click();
        QTRY_COMPARE(
            bool(window.findChild<QAbstractButton *>("NeverDAgentLauncher")),
            !expectedEnabled);
        QCOMPARE(QSettings().value(enabledKey, true).toBool(),
                 !expectedEnabled);
      }
    };

    check(true, false);
    QVERIFY(!QSettings().contains(pathKey));
    QSettings().setValue(pathKey, oldManifest);
    QSettings().setValue(enabledKey, true);
    check(true, true);
    QCOMPARE(QSettings().value(pathKey).toString(), oldManifest);
    check(false, true);
    check(true, false);
  }

  void agentVisibilityIsExplicitAndRestorable() {
    const ScopedTestSettings settings("AgentVisibility");
    QVERIFY(settings.isValid());
    constexpr auto id = "neverd.agent-ui";
    constexpr auto hintId = "NeverDAgentLauncherHint";
    const auto manifest = QStringLiteral(TEST_AGENT_UI);
    const auto action = [](const QList<QAction *> &actions, const char *name) {
      for (auto *entry : actions)
        if (entry->objectName() == QLatin1String(name))
          return entry;
      return static_cast<QAction *>(nullptr);
    };

    {
      Session session(QStringLiteral(TEST_WORKER));
      QWidget window;
      GuiPluginUiHost host(
          window, session, [&] { return window.rect(); }, [](const auto &) {});
      ActionRegistry registry;
      AnalysisPlugins plugins(session, registry, nullptr, manifest, &host);
      window.resize(1000, 700);
      window.show();
      auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
      auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
      auto *prompt = window.findChild<QPlainTextEdit *>("NeverDAgentPrompt");
      QVERIFY(ball && panel && prompt);
      QVERIFY(ball->isVisible() && panel->isHidden());
      QVERIFY(!window.findChild<QToolButton *>("NeverDAgentPin"));

      const auto menuActions = [&]() -> QList<QAction *> {
        for (auto *entry : plugins.menu()->actions())
          if (auto *menu = entry->menu();
              menu && menu->title() == "NeverD Agent")
            return menu->actions();
        return {};
      };
      auto *toggle = action(menuActions(), "NeverDAgentLauncherVisibility");
      auto *open = action(menuActions(), "NeverDAgentOpenChat");
      QVERIFY(toggle && toggle->isChecked() && open);
      QCOMPARE(open->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+I")));

      // Hovering previews nothing; a click opens the conversation, and it
      // stays open when the pointer and the keyboard go elsewhere.
      QEnterEvent enter(QPointF(10, 10), QPointF(10, 10),
                        ball->mapToGlobal(QPoint(10, 10)));
      QCoreApplication::sendEvent(ball, &enter);
      QTest::qWait(350);
      QVERIFY(panel->isHidden());
      ball->click();
      QVERIFY(panel->isVisible());
      QEvent leave(QEvent::Leave);
      QCoreApplication::sendEvent(panel, &leave);
      QTest::qWait(350);
      QVERIFY(panel->isVisible());
      ball->click();
      QVERIFY(panel->isHidden());
      // The shortcut opens and focuses the conversation, takes the keyboard
      // back to it from elsewhere, and closes it from inside.
      window.setFocusPolicy(Qt::StrongFocus);
      window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      open->trigger();
      QVERIFY(panel->isVisible());
      QTRY_COMPARE(QApplication::focusWidget(), prompt);
      window.setFocus();
      QTRY_COMPARE(QApplication::focusWidget(), &window);
      open->trigger();
      QVERIFY(panel->isVisible());
      QTRY_COMPARE(QApplication::focusWidget(), prompt);
      open->trigger();
      QVERIFY(panel->isHidden());
      QTRY_COMPARE(QApplication::focusWidget(), &window);

      const QPoint at = ball->rect().center();
      QContextMenuEvent context(QContextMenuEvent::Mouse, at,
                                ball->mapToGlobal(at));
      QCoreApplication::sendEvent(ball, &context);
      auto *contextMenu = ball->findChild<QMenu *>();
      QVERIFY(contextMenu);
      const auto entries = contextMenu->actions();
      QVERIFY(!entries.isEmpty());
      entries.last()->trigger();
      contextMenu->close();
      QVERIFY(ball->isHidden() && panel->isHidden());
      QVERIFY(!toggle->isChecked());
      auto *hint = window.findChild<QMessageBox *>(hintId);
      QVERIFY(hint && hint->text().contains("Show Ask Agent button"));
      hint->accept();
      QTRY_VERIFY(!window.findChild<QMessageBox *>(hintId));
      window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      // The shortcut still opens the conversation without the button.
      open->trigger();
      QVERIFY(panel->isVisible() && ball->isHidden());
      QTRY_COMPARE(QApplication::focusWidget(), prompt);
      open->trigger();
      QVERIFY(panel->isHidden());
      toggle->trigger();
      QVERIFY(ball->isVisible() && panel->isHidden());
      toggle->trigger();
      QVERIFY(ball->isHidden());
      QVERIFY(!window.findChild<QMessageBox *>(hintId));

      QString error;
      QPointer<QAction> oldToggle(toggle);
      QVERIFY2(session.pluginServices().setPluginEnabled(id, false, error),
               qPrintable(error));
      QTRY_VERIFY(oldToggle.isNull());
      QVERIFY(menuActions().isEmpty());
    }

    Session session(QStringLiteral(TEST_WORKER));
    QWidget window;
    GuiPluginUiHost host(
        window, session, [&] { return window.rect(); }, [](const auto &) {});
    QSettings().setValue("guiPlugins/enabled/neverd.agent-ui", true);
    session.pluginServices().restorePlugins(manifest);
    auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
    QVERIFY(ball && ball->isHidden() && panel && panel->isHidden());
    const auto actions = host.menuActions(id);
    QCOMPARE(actions.size(), 2);
    action(actions, "NeverDAgentLauncherVisibility")->trigger();
    QVERIFY(!ball->isHidden() && panel->isHidden());
  }

  void launcherDragsWithoutClickingAndKeepsPanelInBounds() {
    const ScopedTestSettings settings("AgentLauncherDrag");
    QVERIFY(settings.isValid());
    Session session(QStringLiteral(TEST_WORKER));
    auto &jobs = session.pluginServices().jobs();
    QWidget window;
    GuiPluginUiHost host(
        window, session, [&] { return window.rect(); }, [](const auto &) {});
    window.resize(1000, 700);
    window.show();
    QString error;
    QVERIFY2(jobs.install(QStringLiteral(TEST_AGENT_UI), error),
             qPrintable(error));
    neverd::plugin::Diagnostic diagnostic;
    QVERIFY(jobs.catalog().setEnabled("neverd.agent-ui", true, diagnostic));
    auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
    QVERIFY(ball && panel);
    QTRY_VERIFY(ball->geometry().right() > 900 && ball->isVisible());
    // The labelled button: wider than tall.
    QVERIFY(ball->width() > ball->height());

    const auto drag = [&](QPoint target) {
      const auto center = ball->rect().center();
      QTest::mousePress(ball, Qt::LeftButton, Qt::NoModifier, center);
      const auto global = window.mapToGlobal(target + center);
      QMouseEvent move(QEvent::MouseMove, ball->mapFromGlobal(global), global,
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(ball, &move);
      QCoreApplication::processEvents();
      QTest::mouseRelease(ball, Qt::LeftButton, Qt::NoModifier, center);
    };
    QSignalSpy clicks(ball, &QAbstractButton::clicked);
    drag({45, 24});
    QTRY_COMPARE(ball->pos(), QPoint(45, 24));
    QCOMPARE(clicks.count(), 0);
    QVERIFY(!panel->isVisible());

    QTest::mouseClick(ball, Qt::LeftButton);
    QCOMPARE(clicks.count(), 1);
    QTRY_VERIFY(panel->isVisible() &&
                panel->geometry().top() > ball->geometry().bottom() &&
                window.rect().contains(panel->geometry()));

    drag({45, 314});
    QTRY_COMPARE(ball->pos(), QPoint(45, 314));
    QTRY_VERIFY(panel->geometry().left() > ball->geometry().right());
    QVERIFY(panel->isVisible());
    QCOMPARE(clicks.count(), 1);

    // Let go at the side, the button docks there as its mark alone.
    drag({1400, 1100});
    QTRY_VERIFY(window.rect().contains(ball->geometry()) &&
                ball->geometry().right() > 900);
    QTRY_COMPARE(ball->width(), ball->height());
    window.resize(500, 380);
    QTRY_VERIFY(window.rect().contains(ball->geometry()) &&
                window.rect().contains(panel->geometry()) &&
                !panel->geometry().intersects(ball->geometry()));
    QTest::mouseClick(ball, Qt::LeftButton);
    QCOMPARE(clicks.count(), 2);
    QVERIFY(!panel->isVisible());
    // Dragged away from the side, it is the labelled button again.
    window.resize(1000, 700);
    drag({300, 300});
    QTRY_VERIFY(ball->width() > ball->height());
  }

  void conversationMovesResizesAndRemembersItsPlace() {
    const ScopedTestSettings settings("AgentChatPlace");
    QVERIFY(settings.isValid());
    const auto open = [&](Session &session, QWidget &window,
                          GuiPluginUiHost &) -> QWidget * {
      auto &jobs = session.pluginServices().jobs();
      window.resize(1000, 700);
      window.show();
      QString error;
      jobs.install(QStringLiteral(TEST_AGENT_UI), error);
      neverd::plugin::Diagnostic diagnostic;
      jobs.catalog().setEnabled("neverd.agent-ui", true, diagnostic);
      auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
      if (!ball)
        return nullptr;
      ball->click();
      return window.findChild<QWidget *>("NeverDAgentChat");
    };
    QRect moved;
    {
      Session session(QStringLiteral(TEST_WORKER));
      QWidget window;
      GuiPluginUiHost host(
          window, session, [&] { return window.rect(); }, [](const auto &) {});
      auto *panel = open(session, window, host);
      QVERIFY(panel);
      auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
      // First beside the button, above it.
      QTRY_VERIFY(panel->isVisible() &&
                  panel->geometry().bottom() < ball->geometry().top());
      auto *title = panel->findChild<QWidget *>("NeverDAgentTitleBar");
      QVERIFY(title);
      const QRect before = panel->geometry();
      // The title moves the window.
      const QPoint grip(title->width() / 2, title->height() / 2);
      QTest::mousePress(title, Qt::LeftButton, Qt::NoModifier, grip);
      const QPoint target = title->mapToGlobal(grip) + QPoint(-260, -60);
      QMouseEvent move(QEvent::MouseMove, title->mapFromGlobal(target), target,
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(title, &move);
      QCoreApplication::processEvents();
      QTest::mouseRelease(title, Qt::LeftButton, Qt::NoModifier,
                          title->mapFromGlobal(target));
      QTRY_COMPARE(panel->geometry().topLeft(),
                   before.topLeft() + QPoint(-260, -60));
      QCOMPARE(panel->size(), before.size());
      // Its left edge resizes it.
      const QPoint edge(2, panel->height() / 2);
      QTest::mousePress(panel, Qt::LeftButton, Qt::NoModifier, edge);
      const QPoint wider = panel->mapToGlobal(edge) + QPoint(-80, 0);
      QMouseEvent stretch(QEvent::MouseMove, panel->mapFromGlobal(wider), wider,
                          Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(panel, &stretch);
      QCoreApplication::processEvents();
      QTest::mouseRelease(panel, Qt::LeftButton, Qt::NoModifier,
                          panel->mapFromGlobal(wider));
      QTRY_COMPARE(panel->width(), before.width() + 80);
      QCOMPARE(panel->geometry().right(), before.right() - 260);
      moved = panel->geometry();
      // It stays inside the workbench when the workbench shrinks.
      window.resize(700, 500);
      QTRY_VERIFY(window.rect().contains(panel->geometry()));
      window.resize(1000, 700);
      QTRY_COMPARE(panel->geometry(), moved);
    }
    {
      // The next workbench opens it where it was left.
      Session session(QStringLiteral(TEST_WORKER));
      QWidget window;
      GuiPluginUiHost host(
          window, session, [&] { return window.rect(); }, [](const auto &) {});
      auto *panel = open(session, window, host);
      QVERIFY(panel);
      QTRY_COMPARE(panel->geometry(), moved);
      // A double click on its title puts it back beside the button.
      auto *title = panel->findChild<QWidget *>("NeverDAgentTitleBar");
      QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier,
                         QPoint(title->width() / 2, title->height() / 2));
      auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
      QTRY_VERIFY(panel->geometry() != moved &&
                  panel->geometry().bottom() < ball->geometry().top());
      QVERIFY(!QSettings().contains("guiPlugins/neverd.agent-ui/chatOffset"));
    }
  }

  void agentChatUsesTheLiveWorkbenchAndReopensAfterHiding() {
    ModelServer model;
    QVERIFY(model.Tcp.listen(QHostAddress::LocalHost));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto priorHome = qgetenv("NEVERD_AGENT_HOME");
    qputenv("NEVERD_AGENT_HOME", dir.path().toUtf8());
    const auto restoreHome = qScopeGuard([&] {
      if (priorHome.isNull())
        qunsetenv("NEVERD_AGENT_HOME");
      else
        qputenv("NEVERD_AGENT_HOME", priorHome);
    });
    const QJsonObject profile{
        {"protocol", "responses"},
        {"model", "mock"},
        {"url",
         QString("http://127.0.0.1:%1/responses").arg(model.Tcp.serverPort())},
        {"timeoutSeconds", 8}};
    const auto configPath = dir.filePath("config.json");
    const QJsonObject config{{"version", 1},
                             {"projects", QJsonArray{}},
                             {"sharing", false},
                             {"lightTheme", false},
                             {"guiPath", ""},
                             {"generateMemory", false},
                             {"mcpPath", QStringLiteral(TEST_MCP)},
                             {"model", profile}};
    QVERIFY(write(configPath, config));
    const auto binary = dir.filePath("sample.bin");
    QVERIFY(write(binary, QByteArray("fixture")));

    Session session(QStringLiteral(TEST_WORKER));
    auto &jobs = session.pluginServices().jobs();
    QWidget window;
    QString navigated;
    GuiPluginUiHost host(
        window, session, [&] { return window.rect(); },
        [&](const auto &address) { navigated = address; });
    window.resize(1000, 700);
    window.show();
    session.open(binary);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 7000);
    const QString address = "0xffff800012340000";
    session.pluginServices().setSelectionProvider([address] {
      return QJsonObject{{"address", address},
                         {"function_address", address},
                         {"selection_text", "Live workbench selection"}};
    });
    session.pluginServices().refreshContext();
    QString error;
    QVERIFY2(jobs.install(QStringLiteral(TEST_AGENT_UI), error),
             qPrintable(error));
    neverd::plugin::Diagnostic diagnostic;
    QVERIFY(jobs.catalog().setEnabled("neverd.agent-ui", true, diagnostic));
    QTRY_VERIFY(window.findChild<QAbstractButton *>("NeverDAgentLauncher"));
    auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
    QVERIFY(panel);
    QVERIFY(!panel->isVisible());
    ball->click();
    QTRY_VERIFY(panel->isVisible());
    auto *status = panel->findChild<QLabel *>("NeverDAgentStatus");
    QVERIFY(status);
    QTRY_VERIFY(status->text().isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath("workbench")));
    QCOMPARE(model.Requests.size(), 0);
    QVERIFY(window.rect().contains(panel->geometry()));
    QVERIFY(!panel->geometry().intersects(ball->geometry()));
    window.resize(520, 410);
    QTRY_VERIFY(window.rect().contains(panel->geometry()) &&
                !panel->geometry().intersects(ball->geometry()));
    QVERIFY(panel->height() > 0);
    window.resize(1000, 700);

    auto *prompt = panel->findChild<QPlainTextEdit *>("NeverDAgentPrompt");
    auto *send = panel->findChild<QPushButton *>("NeverDAgentSend");
    QVERIFY(prompt && send);
    prompt->setPlainText("Inspect the current instruction");
    prompt->moveCursor(QTextCursor::End);
    QTest::keyClick(prompt, Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(prompt->toPlainText(), "Inspect the current instruction\n");
    QTest::keyClicks(prompt, "Include the listing.");
    QCOMPARE(model.Requests.size(), 0);
    QVERIFY(send->isEnabled());
    QTest::keyClick(prompt, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(model.Requests.size(), 1, 12000);
    const auto request = QJsonDocument(model.Requests.front()).toJson();
    QVERIFY(request.contains("Live workbench selection"));
    QVERIFY(request.contains(
        "Inspect the current instruction\\nInclude the listing."));
    prompt->setPlainText("Draft a follow-up");
    QTest::keyClick(prompt, Qt::Key_Return);
    QCOMPARE(prompt->toPlainText(), "Draft a follow-up");
    ball->click();
    QVERIFY(!panel->isVisible());
    model.respond(0, call("list_objects", "objects"));
    QTRY_COMPARE(model.Requests.size(), 2);
    auto *latest = panel->findChild<QLabel *>("NeverDAgentExecutionLatest");
    auto *execution =
        panel->findChild<QToolButton *>("NeverDAgentExecutionToggle");
    auto *rows = panel->findChild<QTreeWidget *>("NeverDAgentExecutionRows");
    QVERIFY(latest && execution && rows);
    QTRY_VERIFY(latest->text().contains("list_objects"));
    ball->click();
    QVERIFY(latest->isVisible());
    execution->click();
    QTRY_VERIFY(rows->isVisible() && rows->topLevelItemCount() == 1);
    QVERIFY(rows->topLevelItem(0)->text(1).startsWith("Done"));
    ball->click();
    QVERIFY(!panel->isVisible());
    const auto objects =
        outputFor(model.Requests.at(1), "objects").value("objects").toArray();
    QCOMPARE(objects.size(), 1);
    const auto id = objects.first().toObject().value("id").toString();
    QVERIFY(!id.isEmpty());
    model.respond(1, call("delegate_object", "delegate",
                          {{"object_id", id}, {"task", "Read the listing"}}));
    QTRY_COMPARE(model.Requests.size(), 3);
    QTRY_COMPARE(rows->topLevelItemCount(), 2);
    QCOMPARE(rows->topLevelItem(1)->text(1), QStringLiteral("Running"));
    model.respond(2, call("nd_query", "listing",
                          {{"tool", "read_disassembly"},
                           {"arguments",
                            QJsonObject{{"address", address}, {"limit", 2}}}}));
    QTRY_COMPARE_WITH_TIMEOUT(model.Requests.size(), 4, 10000);
    const auto listing = outputFor(model.Requests.at(3), "listing");
    const auto link = listing.value("evidence_url").toString();
    QVERIFY2(!link.isEmpty(), QJsonDocument(listing).toJson().constData());
    QTRY_COMPARE(rows->topLevelItemCount(), 3);
    QTRY_VERIFY(rows->topLevelItem(2)->text(1).startsWith("Done"));
    QVERIFY(rows->topLevelItem(2)->text(0).contains("read_disassembly"));
    QVERIFY(rows->topLevelItem(2)->text(0).contains(address));
    model.respond(3, answer(QString("Read the [listing](%1).").arg(link)));
    QTRY_COMPARE(model.Requests.size(), 5);
    const auto result =
        QString("Verified the [listing](%1).").arg(link) +
        QStringLiteral("\n\nMore listing details.").repeated(24);
    model.respond(4, answer(result));
    const auto replyText = [&] {
      auto *reply = panel->findChild<QWidget *>("NeverDAgentReply");
      return reply ? reply->findChild<QTextBrowser *>("NeverDAgentMessageText")
                   : nullptr;
    };
    QTRY_VERIFY(replyText() &&
                replyText()->toPlainText().contains("Verified the listing."));
    QVERIFY(!panel->isVisible());
    ball->click();
    QVERIFY(panel->isVisible());
    const auto *user = panel->findChild<QWidget *>("NeverDAgentUserMessage");
    QVERIFY(user && user->geometry().left() >
                        replyText()->parentWidget()->geometry().left());
    auto *history = panel->findChild<QScrollArea *>("NeverDAgentTranscript");
    QVERIFY(history);
    auto *scroll = history->verticalScrollBar();
    QTRY_VERIFY(scroll->maximum() > 0);
    QTRY_COMPARE(scroll->value(), scroll->maximum());
    QVERIFY(rows->isVisible());
    emit rows->itemActivated(rows->topLevelItem(2), 0);
    QTRY_COMPARE(navigated, address);
    navigated.clear();
    emit replyText() -> anchorClicked(QUrl(link));
    QTRY_COMPARE(navigated, address);

    QPointer<QWidget> oldPanel(panel);
    QVERIFY(jobs.catalog().setEnabled("neverd.agent-ui", false, diagnostic));
    QTRY_VERIFY(oldPanel.isNull());
    QVERIFY(!window.findChild<QAbstractButton *>("NeverDAgentLauncher"));

    // Opening a saved conversation does not read the model configuration.
    QVERIFY(write(configPath, QByteArray("invalid model configuration")));
    QTRY_VERIFY(!jobs.catalog().retiring("neverd.agent-ui"));
    QVERIFY(jobs.catalog().setEnabled("neverd.agent-ui", true, diagnostic));
    auto *reopenedBall =
        window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *reopened = window.findChild<QWidget *>("NeverDAgentChat");
    QVERIFY(reopenedBall && reopened);
    reopenedBall->click();
    QTRY_VERIFY(reopened->isVisible());
    QTRY_VERIFY(reopened->findChild<QWidget *>("NeverDAgentReply"));
    auto *previousReply = reopened->findChild<QWidget *>("NeverDAgentReply");
    auto *savedText =
        previousReply->findChild<QTextBrowser *>("NeverDAgentMessageText");
    QVERIFY(savedText &&
            savedText->toPlainText().contains("Verified the listing."));
    QCOMPARE(model.Requests.size(), 5);
    QVERIFY(reopened->isVisible());
    auto *savedExecution =
        previousReply->findChild<QToolButton *>("NeverDAgentExecutionToggle");
    auto *savedRows =
        previousReply->findChild<QTreeWidget *>("NeverDAgentExecutionRows");
    QVERIFY(savedExecution && savedRows);
    QCOMPARE(savedRows->topLevelItemCount(), 0);
    savedExecution->click();
    QTRY_COMPARE(savedRows->topLevelItemCount(), 3);
    QVERIFY(savedRows->topLevelItem(2)->text(0).contains("read_disassembly"));
    navigated.clear();
    emit savedRows->itemActivated(savedRows->topLevelItem(2), 0);
    QTRY_COMPARE(navigated, address);
    QCOMPARE(model.Requests.size(), 5);

    QVERIFY(write(configPath, config));
    auto *reopenedPrompt =
        reopened->findChild<QPlainTextEdit *>("NeverDAgentPrompt");
    auto *reopenedSend = reopened->findChild<QPushButton *>("NeverDAgentSend");
    QVERIFY(reopenedPrompt && reopenedSend);
    reopenedPrompt->setPlainText("Continue");
    QTest::keyClick(reopenedPrompt, Qt::Key_Enter, Qt::KeypadModifier);
    QTRY_COMPARE_WITH_TIMEOUT(model.Requests.size(), 6, 12000);
    model.respond(5, answer("Continued."));
    QTRY_COMPARE(reopenedSend->text(), QStringLiteral("Send"));
  }
#endif
};

QTEST_MAIN(NativeUiTests)
#include "NativeUiTests.moc"
