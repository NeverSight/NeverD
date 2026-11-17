//===- NativeUiTests.cpp - Workbench UI plugin qualification -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ActionRegistry.h"
#include "AnalysisPlugins.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "GuiPluginUiHost.h"
#include "LoadFileDialog.h"
#include "MainWindow.h"
#include "OutputWindow.h"
#include "PluginManagerDialog.h"
#include "PluginServices.h"
#include "Session.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QEnterEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontInfo>
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
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <memory>
#include <numbers>

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

/// Whether \p view stands where a new conversation opens in \p window: at
/// the golden section of the free room across, on the side a line starts
/// from, and in the middle down.
bool restsIn(const QWidget &window, const QWidget &view) {
  const double share = view.layoutDirection() == Qt::RightToLeft
                           ? std::numbers::phi - 1
                           : 2 - std::numbers::phi;
  return view.pos() == QPoint(qRound(share * (window.width() - view.width())),
                              (window.height() - view.height()) / 2);
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
    // The host took each view along after what its module kept below the
    // activation's lifetime, which may watch the view until it goes itself.
    QVERIFY(!qApp->property("nativeSelectionViewWentFirst").toBool());
    QVERIFY(jobs.catalog().setEnabled("fixture.one", false, diagnostic));
    QTRY_COMPARE(labels().size(), 1);
    QVERIFY(!qApp->property("nativeSelectionViewWentFirst").toBool());
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
      // Without a binary, the composer says to open one where the context
      // chip would be, and Send waits for it.
      auto *chip =
          panel->findChild<QAbstractButton *>("NeverDAgentIncludeContext");
      auto *noBinary = panel->findChild<QLabel *>("NeverDAgentNoBinary");
      auto *send = panel->findChild<QPushButton *>("NeverDAgentSend");
      QVERIFY(chip && noBinary && send);
      QVERIFY(noBinary->isVisible() && !chip->isVisible());
      prompt->setPlainText("A question");
      QVERIFY(!send->isEnabled() && noBinary->isVisible());
      prompt->clear();
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
      QVERIFY(panel->isVisible() && ball->isChecked());
      QTRY_COMPARE(QApplication::focusWidget(), prompt);
      window.setFocus();
      QTRY_COMPARE(QApplication::focusWidget(), &window);
      open->trigger();
      QVERIFY(panel->isVisible());
      QTRY_COMPARE(QApplication::focusWidget(), prompt);
      open->trigger();
      QVERIFY(panel->isHidden() && !ball->isChecked());
      QTRY_COMPARE(QApplication::focusWidget(), &window);
      // A click on the button leaves the keyboard in the view, so Escape
      // gives it back there rather than to the button.
      QTest::mouseClick(ball, Qt::LeftButton);
      QVERIFY(panel->isVisible());
      QTRY_COMPARE(QApplication::focusWidget(), prompt);
      QTest::keyClick(prompt, Qt::Key_Escape);
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
      QVERIFY(ball->isVisible() && panel->isHidden() && !ball->isChecked());
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

    // Wherever the button is, the conversation opens at the golden section
    // of the workbench and stays there while the button moves.
    QTest::mouseClick(ball, Qt::LeftButton);
    QCOMPARE(clicks.count(), 1);
    QTRY_VERIFY(panel->isVisible() && restsIn(window, *panel));

    drag({45, 314});
    QTRY_COMPARE(ball->pos(), QPoint(45, 314));
    QVERIFY(panel->isVisible() && restsIn(window, *panel));
    QCOMPARE(clicks.count(), 1);

    // Let go at the side, the button docks there as its mark alone.
    drag({1400, 1100});
    QTRY_VERIFY(window.rect().contains(ball->geometry()) &&
                ball->geometry().right() > 900);
    QTRY_COMPARE(ball->width(), ball->height());
    // A small workbench keeps both inside; the conversation may then cover
    // the button, and closes by its own button or Escape.
    window.resize(500, 380);
    QTRY_VERIFY(window.rect().contains(ball->geometry()) &&
                window.rect().contains(panel->geometry()) &&
                panel->geometry().center().x() <= window.rect().center().x());
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
    // Press on the title, move once by delta and let go.
    const auto dragTitle = [](QWidget &title, QPoint delta) {
      const QPoint grip(title.width() / 2, title.height() / 2);
      QTest::mousePress(&title, Qt::LeftButton, Qt::NoModifier, grip);
      const QPoint target = title.mapToGlobal(grip) + delta;
      QMouseEvent move(QEvent::MouseMove, title.mapFromGlobal(target), target,
                       Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(&title, &move);
      QCoreApplication::processEvents();
      QTest::mouseRelease(&title, Qt::LeftButton, Qt::NoModifier,
                          title.mapFromGlobal(target));
    };
    QRect moved;
    {
      Session session(QStringLiteral(TEST_WORKER));
      QWidget window;
      GuiPluginUiHost host(
          window, session, [&] { return window.rect(); }, [](const auto &) {});
      auto *panel = open(session, window, host);
      QVERIFY(panel);
      // First at the golden section across the workbench, left of its
      // middle: 199 of the 520 free pixels before it, and the middle down.
      QTRY_VERIFY(panel->isVisible() && restsIn(window, *panel));
      QCOMPARE(panel->pos(), QPoint(199, 70));
      auto *title = panel->findChild<QWidget *>("NeverDAgentTitleBar");
      QVERIFY(title);
      const QRect before = panel->geometry();
      // The title moves the window.
      dragTitle(*title, QPoint(220, -40));
      QTRY_COMPARE(panel->geometry().topLeft(),
                   before.topLeft() + QPoint(220, -40));
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
      QCOMPARE(panel->geometry().right(), before.right() + 220);
      moved = panel->geometry();
      // It stays inside the workbench when the workbench shrinks, and keeps
      // its share of the room when it grows again.
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
      // A double click on its title puts it back where it opens at its
      // first size and forgets the place.
      auto *title = panel->findChild<QWidget *>("NeverDAgentTitleBar");
      QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier,
                         QPoint(title->width() / 2, title->height() / 2));
      QTRY_VERIFY(panel->size() == QSize(480, 560) && restsIn(window, *panel));
      QVERIFY(!QSettings().contains("guiPlugins/neverd.agent-ui/chatPlace"));
      QVERIFY(!QSettings().contains("guiPlugins/neverd.agent-ui/chatSize"));
      // Away from there it stands where it is let go; near it, it snaps
      // back and keeps that place while the workbench resizes.
      const QPoint rest = panel->pos();
      dragTitle(*title, QPoint(150, 40));
      QTRY_COMPARE(panel->pos(), rest + QPoint(150, 40));
      dragTitle(*title, QPoint(-140, -35));
      QTRY_COMPARE(panel->pos(), rest);
      window.resize(900, 640);
      QTRY_VERIFY(panel->pos() != rest && restsIn(window, *panel));
    }
  }

  void conversationOpensAtTheGoldenSection_data() {
    QTest::addColumn<bool>("mirrored");
    QTest::newRow("left-to-right") << false;
    QTest::newRow("right-to-left") << true;
  }

  // The conversation opens left of the workbench's middle, or right of it
  // where lines start at the right.
  void conversationOpensAtTheGoldenSection() {
    QFETCH(bool, mirrored);
    const ScopedTestSettings settings("AgentChatRest");
    QVERIFY(settings.isValid());
    const auto direction = QGuiApplication::layoutDirection();
    const auto restore = qScopeGuard(
        [direction] { QGuiApplication::setLayoutDirection(direction); });
    QGuiApplication::setLayoutDirection(mirrored ? Qt::RightToLeft
                                                 : Qt::LeftToRight);
    Session session(QStringLiteral(TEST_WORKER));
    QWidget window;
    GuiPluginUiHost host(
        window, session, [&] { return window.rect(); }, [](const auto &) {});
    window.resize(1000, 700);
    window.show();
    QString error;
    QVERIFY2(session.pluginServices().jobs().install(
                 QStringLiteral(TEST_AGENT_UI), error),
             qPrintable(error));
    neverd::plugin::Diagnostic diagnostic;
    QVERIFY(session.pluginServices().jobs().catalog().setEnabled(
        "neverd.agent-ui", true, diagnostic));
    auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
    QVERIFY(ball && panel);
    QTRY_VERIFY(ball->isVisible());
    ball->click();
    // 520 pixels of room across: the smaller golden part before the
    // conversation, on the side its lines start from.
    QTRY_VERIFY(panel->isVisible());
    QTRY_COMPARE(panel->pos(), QPoint(mirrored ? 321 : 199, 70));
  }

  // Focus mode lends a window of its own to the views; they go there where
  // they were, shown or hidden, its window takes the plugins' shortcuts, and
  // they come back when it is returned or destroyed.
  void viewsGoWhereTheHostLendsThem() {
    const ScopedTestSettings settings("AgentLentViews");
    QVERIFY(settings.isValid());
    Session session(QStringLiteral(TEST_WORKER));
    QWidget window;
    GuiPluginUiHost host(
        window, session, [&] { return window.rect(); }, [](const auto &) {});
    ActionRegistry registry;
    AnalysisPlugins plugins(session, registry, nullptr,
                            QStringLiteral(TEST_AGENT_UI), &host);
    window.resize(1000, 700);
    window.show();
    auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
    auto *prompt = window.findChild<QPlainTextEdit *>("NeverDAgentPrompt");
    QVERIFY(ball && panel && prompt);
    QTRY_VERIFY(ball->isVisible() && panel->isHidden());

    auto workspace = std::make_unique<QWidget>();
    workspace->setFocusPolicy(Qt::StrongFocus);
    workspace->resize(1200, 800);
    workspace->show();
    // Below a caption 40 pixels high, as a floating window's.
    const QRect area(0, 40, 1200, 760);
    host.lendViews(*workspace, [&] { return area; });
    QCOMPARE(ball->window(), workspace.get());
    QCOMPARE(panel->window(), workspace.get());
    QTRY_VERIFY(ball->isVisible() && panel->isHidden());
    QTRY_VERIFY(area.contains(ball->geometry()) &&
                ball->geometry().right() > 1100 &&
                ball->geometry().bottom() > 700);
    // The shortcut opens the conversation in the workspace, at its golden
    // section (720 pixels free across, 240 down), and closes it again.
    workspace->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(workspace.get()));
    workspace->setFocus();
    QTest::keyClick(workspace.get(), Qt::Key_I,
                    Qt::ControlModifier | Qt::AltModifier);
    QTRY_VERIFY(panel->isVisible() && ball->isChecked());
    QTRY_COMPARE(panel->pos(), QPoint(275, 120));
    QTRY_COMPARE(QApplication::focusWidget(), prompt);
    QTest::keyClick(prompt, Qt::Key_I, Qt::ControlModifier | Qt::AltModifier);
    QTRY_VERIFY(panel->isHidden() && !ball->isChecked());

    ball->click();
    QVERIFY(panel->isVisible());
    host.returnViews();
    QCOMPARE(ball->window(), &window);
    QCOMPARE(panel->window(), &window);
    QTRY_VERIFY(ball->isVisible() && panel->isVisible() &&
                restsIn(window, *panel));
    QVERIFY(window.rect().contains(ball->geometry()));

    // A workspace destroyed while it holds the views gives them back first.
    host.lendViews(*workspace, [&] { return area; });
    QCOMPARE(panel->window(), workspace.get());
    const QPointer<QWidget> keptBall(ball), keptPanel(panel);
    workspace.reset();
    QVERIFY(keptBall && keptPanel);
    QCOMPARE(ball->window(), &window);
    QCOMPARE(panel->window(), &window);
    QTRY_VERIFY(ball->isVisible() && panel->isVisible());
    ball->click();
    QVERIFY(panel->isHidden());
  }

  // In focus mode, F pressed three times, the Ask Agent button and the
  // conversation are in the workspace that covers the workbench, clear of
  // its command line, and they come back with the workbench.
  void askAgentFollowsFocusMode() {
    const ScopedTestSettings settings("AgentFocusMode");
    QVERIFY(settings.isValid());
    QSettings().setValue(settings::OpenInNewInstance, false);
    static const bool docking = (configureDocking(), true);
    Q_UNUSED(docking);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto binary = directory.filePath("focus.bin");
    QVERIFY(write(binary, QByteArray("fixture")));
    Session session(QStringLiteral(TEST_WORKER));
    McpConnectionManager mcp;
    GuiSessionBroker broker;
    MainWindow window(session, mcp, broker, QStringLiteral(TEST_AGENT_UI));
    window.setLayoutPath(directory.filePath("layout.json"));
    window.resize(1400, 900);
    window.initializeLayout();
    window.show();
    QTimer loads;
    QObject::connect(&loads, &QTimer::timeout, [] {
      if (auto *dialog =
              qobject_cast<LoadFileDialog *>(QApplication::activeModalWidget()))
        dialog->accept();
    });
    loads.start(10);
    window.openFile(binary);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), 7000);
    auto *ball = window.findChild<QAbstractButton *>("NeverDAgentLauncher");
    auto *panel = window.findChild<QWidget *>("NeverDAgentChat");
    auto *prompt = window.findChild<QPlainTextEdit *>("NeverDAgentPrompt");
    QVERIFY(ball && panel && prompt);
    QTRY_VERIFY(ball->isVisible() && panel->isHidden());

    auto *mode = window.actions().action(ActionId::ViewFocusMode);
    QTRY_VERIFY(mode->isEnabled());
    mode->trigger();
    QVERIFY(mode->isChecked());
    auto *workspace = window.disassembly()->window();
    QVERIFY(workspace != &window);
    QCOMPARE(ball->window(), workspace);
    QCOMPARE(panel->window(), workspace);
    QTRY_VERIFY(workspace->isMaximized() && ball->isVisible());
    auto *output = workspace->findChild<OutputWindow *>();
    QVERIFY(output && output->commandLine());
    const auto *line = output->commandLine();
    QTRY_VERIFY(line->isVisible());
    const QRect row(line->mapTo(workspace, QPoint()), line->size());
    QTRY_VERIFY(workspace->rect().contains(ball->geometry()) &&
                ball->geometry().center().x() > workspace->width() / 2 &&
                ball->geometry().bottom() < row.top() &&
                ball->geometry().bottom() > row.top() - 3 * ball->height());
    // The plugins' shortcut works in the workspace's window.
    workspace->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(workspace));
    QTest::keyClick(window.disassembly(), Qt::Key_I,
                    Qt::ControlModifier | Qt::AltModifier);
    QTRY_VERIFY(panel->isVisible() && ball->isChecked());
    QTRY_COMPARE(QApplication::focusWidget(), prompt);
    QVERIFY(workspace->rect().contains(panel->geometry()));

    // Leaving focus mode brings both back, the conversation still open.
    mode->trigger();
    QVERIFY(!mode->isChecked());
    QCOMPARE(ball->window(), &window);
    QCOMPARE(panel->window(), &window);
    QTRY_VERIFY(ball->isVisible() && panel->isVisible());
    QTRY_VERIFY(window.rect().contains(panel->geometry()));
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
    // The button is a toggle that follows the conversation.
    QVERIFY(ball->isCheckable() && !ball->isChecked());
    ball->click();
    QTRY_VERIFY(panel->isVisible());
    QVERIFY(ball->isChecked());
    auto *status = panel->findChild<QLabel *>("NeverDAgentStatus");
    QVERIFY(status);
    QTRY_VERIFY(status->text().isEmpty());
    QVERIFY(!QFileInfo::exists(dir.filePath("workbench")));
    QCOMPARE(model.Requests.size(), 0);
    QVERIFY(window.rect().contains(panel->geometry()));
    QVERIFY(!panel->geometry().intersects(ball->geometry()));
    window.resize(520, 410);
    QTRY_VERIFY(window.rect().contains(panel->geometry()));
    QVERIFY(panel->height() > 0);
    window.resize(1000, 700);

    auto *prompt = panel->findChild<QPlainTextEdit *>("NeverDAgentPrompt");
    auto *send = panel->findChild<QPushButton *>("NeverDAgentSend");
    QVERIFY(prompt && send);
    // With a binary, the composer names what a question takes along.
    auto *chip =
        panel->findChild<QAbstractButton *>("NeverDAgentIncludeContext");
    QVERIFY(chip && chip->isCheckable() && chip->isChecked());
    QTRY_VERIFY(
        chip->isVisible() &&
        !panel->findChild<QLabel *>("NeverDAgentNoBinary")->isVisible());
    // A question is prose: the prompt types in the interface font, though the
    // workbench types its text fields in the code font.
    {
      const QString sheet = qApp->styleSheet();
      const auto restoreSheet =
          qScopeGuard([&] { qApp->setStyleSheet(sheet); });
      qApp->setStyleSheet(QStringLiteral(
          "QPlainTextEdit[readOnly=\"false\"] { font: 23px \"Courier\"; }"));
      QTRY_COMPARE(prompt->font().family(), QApplication::font().family());
      QVERIFY(QFontInfo(prompt->font()).pixelSize() != 23);
    }
    // It grows with its lines and shows every one of them without scrolling,
    // up to a limit; past it, it scrolls.
    const int least = prompt->height();
    prompt->setPlainText("one\ntwo");
    QTRY_COMPARE(prompt->verticalScrollBar()->maximum(), 0);
    const int two = prompt->height();
    prompt->setPlainText("one\ntwo\nthree");
    QTRY_VERIFY(prompt->height() > two);
    QTRY_COMPARE(prompt->verticalScrollBar()->maximum(), 0);
    prompt->setPlainText(QStringLiteral("line\n").repeated(12));
    QTRY_VERIFY(prompt->verticalScrollBar()->maximum() > 0);
    const int most = prompt->height();
    prompt->setPlainText(QStringLiteral("line\n").repeated(20));
    QTRY_VERIFY(prompt->verticalScrollBar()->maximum() > 6);
    QCOMPARE(prompt->height(), most);
    prompt->clear();
    QTRY_COMPARE(prompt->height(), least);
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
    QVERIFY(!ball->isChecked());
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
