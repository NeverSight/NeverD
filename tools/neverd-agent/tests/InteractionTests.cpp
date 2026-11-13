//===- InteractionTests.cpp - Investigation interaction contracts --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "Config.h"
#include "app/AgentController.h"
#include "client/WorkspaceClient.h"
#include "platform/AppPaths.h"
#include "project/ResearchPlan.h"
#include "runtime/ChangeReview.h"
#include "runtime/EvidenceCapture.h"

#include <QDir>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace neverd::agent;
namespace {
class ModelServer final : public QObject {
public:
  QTcpServer Listener;
  QList<QJsonObject> Requests;
  QList<QPointer<QTcpSocket>> Sockets;
  ModelServer() {
    Listener.listen(QHostAddress::LocalHost);
    connect(&Listener, &QTcpServer::newConnection, this, [this] {
      auto *socket = Listener.nextPendingConnection();
      auto bytes = std::make_shared<QByteArray>();
      connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
        *bytes += socket->readAll();
        const auto end = bytes->indexOf("\r\n\r\n");
        if (end < 0)
          return;
        qsizetype length = 0;
        for (const auto &line : bytes->left(end).split('\n'))
          if (line.toLower().startsWith("content-length:"))
            length = line.mid(15).trimmed().toLongLong();
        if (bytes->size() < end + 4 + length)
          return;
        Requests.append(
            QJsonDocument::fromJson(bytes->mid(end + 4, length)).object());
        Sockets.append(socket);
        socket->disconnect(this);
      });
    });
  }
  ModelProfile
  profile(ModelProtocol protocol = ModelProtocol::Responses) const {
    ModelProfile profile;
    profile.Protocol = protocol;
    profile.Model = "mock";
    profile.BaseUrl =
        QString("http://127.0.0.1:%1/%2")
            .arg(Listener.serverPort())
            .arg(protocol == ModelProtocol::Responses  ? "responses"
                 : protocol == ModelProtocol::Messages ? "messages"
                                                       : "chat/completions");
    return profile;
  }
  void respond(int index, ModelProtocol protocol, const QString &text,
               const QStringList &calls = {}, const QJsonObject &arguments = {},
               const QString &idSuffix = "-id") {
    QJsonObject value;
    QJsonArray content, tools;
    const auto encodedArguments = QString::fromUtf8(
        QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    if (protocol == ModelProtocol::Responses) {
      if (!text.isEmpty())
        content.append(QJsonObject{
            {"type", "message"},
            {"role", "assistant"},
            {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                               {"text", text}}}}});
      for (const auto &name : calls)
        content.append(QJsonObject{{"type", "function_call"},
                                   {"call_id", name + idSuffix},
                                   {"name", name},
                                   {"arguments", encodedArguments}});
      value = {
          {"status", "completed"},
          {"output", content},
          {"usage", QJsonObject{{"input_tokens", 20}, {"output_tokens", 10}}}};
    } else if (protocol == ModelProtocol::Messages) {
      if (!text.isEmpty())
        content.append(QJsonObject{{"type", "text"}, {"text", text}});
      for (const auto &name : calls)
        content.append(QJsonObject{{"type", "tool_use"},
                                   {"id", name + idSuffix},
                                   {"name", name},
                                   {"input", arguments}});
      value = {
          {"id", "turn"},
          {"type", "message"},
          {"role", "assistant"},
          {"content", content},
          {"stop_reason", calls.isEmpty() ? "end_turn" : "tool_use"},
          {"usage", QJsonObject{{"input_tokens", 20}, {"output_tokens", 10}}}};
    } else {
      for (const auto &name : calls)
        tools.append(QJsonObject{
            {"id", name + idSuffix},
            {"type", "function"},
            {"function",
             QJsonObject{{"name", name}, {"arguments", encodedArguments}}}});
      QJsonObject message{{"role", "assistant"}, {"content", text}};
      if (!tools.isEmpty())
        message["tool_calls"] = tools;
      value = {{"choices", QJsonArray{QJsonObject{
                               {"message", message},
                               {"finish_reason",
                                calls.isEmpty() ? "stop" : "tool_calls"}}}},
               {"usage",
                QJsonObject{{"prompt_tokens", 20}, {"completion_tokens", 10}}}};
    }
    const auto data = QJsonDocument(value).toJson(QJsonDocument::Compact);
    auto *socket = Sockets[index].data();
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                  "application/json\r\nConnection: close\r\nContent-Length: " +
                  QByteArray::number(data.size()) + "\r\n\r\n" + data);
    socket->disconnectFromHost();
  }
};
} // namespace
class InteractionTests final : public QObject {
  Q_OBJECT
private slots:
  void dependencyImportsRespectWorkspaceApprovals() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QVERIFY(QDir(directory.path()).mkdir("bin"));
    ModelServer server;
    QVERIFY(server.Listener.isListening());
    const auto home = directory.filePath("agent");
    const auto first = directory.filePath("first");
    const auto second = directory.filePath("second");
    const auto binary = [&](const QString &name) {
      QFile file(directory.filePath("bin/" + name));
      return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7
                 ? file.fileName()
                 : QString{};
    };
    const auto source = binary("program.exe");
    const auto dependency = binary("child.dll");
    const auto denied = binary("denied.dll");
    const auto cancelled = binary("cancelled.dll");
    const auto automatic = binary("auto.dll");
    QVERIFY(!source.isEmpty() && !dependency.isEmpty() && !denied.isEmpty() &&
            !cancelled.isEmpty() && !automatic.isEmpty());

    QString error;
    WorkspaceClient workspace(home);
    QVERIFY2(workspace.initialize({}, error), qPrintable(error));
    QVERIFY(!workspace.autoAddAnalysisObjects());
    QVERIFY(workspace.createProject("First", first));
    const auto firstId = workspace.activeId();
    workspace.addObject(source);
    QTRY_VERIFY(!workspace.selectedObjectId().isEmpty());
    const auto sourceId = workspace.selectedObjectId();
    QVERIFY(workspace.createProject("Second", second));
    const auto secondId = workspace.activeId();
    QVERIFY(workspace.selectProject(firstId));
    QVERIFY(workspace.updateModelSettings(
        server.profile().toJson().toVariantMap(), "", false, ""));

    const auto propose = [&](int index, const QString &path) {
      server.respond(index, ModelProtocol::Responses, {},
                     {"add_analysis_object"},
                     {{"path", path},
                      {"source_object_id", sourceId},
                      {"reason", "Imported by program.exe"}},
                     QString::number(index));
    };
    const auto output = [&](int index, const QString &tool) {
      for (const auto &entry : server.Requests.at(index)["input"].toArray()) {
        const auto item = entry.toObject();
        if (item["type"] == "function_call_output" &&
            item["call_id"] == tool + QString::number(index - 1))
          return QJsonDocument::fromJson(item["output"].toString().toUtf8())
              .object();
      }
      return QJsonObject{};
    };
    const auto pending = [&] {
      return workspace.analysisObjectApprovals().value(0).toMap();
    };
    const auto objectCount = [&](const QString &path) {
      QString issue;
      const auto project = ProjectStore::open(path, issue);
      return project ? project->Objects.size() : qsizetype{-1};
    };

    workspace.setDraft("Trace the dependency");
    QVERIFY(workspace.sendRequest(sourceId));
    QTRY_COMPARE(server.Requests.size(), 1);
    QVERIFY(QJsonDocument(server.Requests[0]["tools"].toArray())
                .toJson()
                .contains("add_analysis_object"));
    propose(0, "CHILD.DLL");
    QTRY_COMPARE(workspace.analysisObjectApprovals().size(), 1);
    QCOMPARE(pending()["project"].toString(), "First");
    QCOMPARE(pending()["projectId"].toString(), firstId);
    QVERIFY(pending().contains("sideChatId"));
    QVERIFY(pending()["sideChatId"].toString().isEmpty());
    QCOMPARE(pending()["source"].toString(), "program.exe");
    QVERIFY(QFileInfo(pending()["path"].toString()) == QFileInfo(dependency));
    QCOMPARE(server.Requests.size(), 1);
    QCOMPARE(objectCount(first), 1);
    QVERIFY(workspace.selectProject(secondId));
    const auto approval = pending()["id"].toString();
    QVERIFY(!workspace.resolveAnalysisObjectApproval("wrong-id", true));
    QVERIFY(workspace.resolveAnalysisObjectApproval(approval, true));
    QVERIFY(!workspace.resolveAnalysisObjectApproval(approval, true));
    QTRY_COMPARE(server.Requests.size(), 2);
    const auto added = output(1, "add_analysis_object");
    const auto dependencyId = added["objectId"].toString();
    QVERIFY(!dependencyId.isEmpty());
    QCOMPARE(added["decision"].toString(), "approved");
    QCOMPARE(objectCount(first), 2);
    QCOMPARE(objectCount(second), 0);
    QCOMPARE(workspace.activeId(), secondId);

    server.respond(
        1, ModelProtocol::Responses, {}, {"delegate_object"},
        {{"object_id", dependencyId}, {"task", "Analyze child.dll"}});
    QTRY_COMPARE(server.Requests.size(), 3);
    QVERIFY(QJsonDocument(server.Requests[2])
                .toJson()
                .contains(dependencyId.toUtf8()));
    propose(2, "denied.dll");
    QTRY_COMPARE(workspace.analysisObjectApprovals().size(), 1);
    QCOMPARE(pending()["source"].toString().toLower(), "child.dll");
    QVERIFY(workspace.resolveAnalysisObjectApproval(pending()["id"].toString(),
                                                    false));
    QTRY_COMPARE(server.Requests.size(), 4);
    QVERIFY(output(3, "add_analysis_object")["error"].toString().contains(
        "declined"));
    QCOMPARE(objectCount(first), 2);
    server.respond(3, ModelProtocol::Responses, "Dependency inspected");
    QTRY_COMPARE(server.Requests.size(), 5);
    QVERIFY(workspace.selectProject(firstId));
    server.respond(4, ModelProtocol::Responses, "Dependency inspected");
    QTRY_VERIFY(!workspace.running());

    workspace.setDraft("Cancel an unanswered approval");
    QVERIFY(workspace.sendRequest(sourceId));
    QTRY_COMPARE(server.Requests.size(), 6);
    propose(5, "cancelled.dll");
    QTRY_COMPARE(workspace.analysisObjectApprovals().size(), 1);
    const auto cancelledId = pending()["id"].toString();
    workspace.cancelRun();
    QTRY_VERIFY(!workspace.running());
    QVERIFY(workspace.analysisObjectApprovals().isEmpty());
    QVERIFY(!workspace.resolveAnalysisObjectApproval(cancelledId, true));
    QCOMPARE(objectCount(first), 2);

    QVERIFY(workspace.createSideChat());
    const auto sideId = workspace.sideChat()->id();
    workspace.sideChat()->setDraft("Review the dependency in a side chat");
    QVERIFY(workspace.sendSideRequest());
    QTRY_COMPARE(server.Requests.size(), 7);
    propose(6, "denied.dll");
    QTRY_COMPARE(workspace.analysisObjectApprovals().size(), 1);
    QCOMPARE(pending()["projectId"].toString(), firstId);
    QCOMPARE(pending()["sideChatId"].toString(), sideId);
    QVERIFY(workspace.resolveAnalysisObjectApproval(pending()["id"].toString(),
                                                    false));
    QTRY_COMPARE(server.Requests.size(), 8);
    server.respond(7, ModelProtocol::Responses, "Side request declined");
    QTRY_VERIFY(!workspace.sideChat()->running());
    QCOMPARE(objectCount(first), 2);

    QVERIFY(workspace.updateWorkspaceSettings(false, false, "", true));
    workspace.setDraft("Continue with automatic approval");
    QVERIFY(workspace.sendRequest(sourceId));
    QTRY_COMPARE(server.Requests.size(), 9);
    propose(8, "auto.dll");
    QTRY_COMPARE(server.Requests.size(), 10);
    QCOMPARE(output(9, "add_analysis_object")["decision"].toString(),
             "automatic");
    QVERIFY(workspace.analysisObjectApprovals().isEmpty());
    QCOMPARE(objectCount(first), 3);
    server.respond(9, ModelProtocol::Responses,
                   "Automatic dependency inspected");
    QTRY_VERIFY(!workspace.running());
    QVERIFY(workspace.prepareClose());
    QJsonObject config;
    QVERIFY(ProjectStore::readJson(QDir(home).filePath("config.json"), config,
                                   error));
    QVERIFY(config["autoAddAnalysisObjects"].toBool());
  }

  void workspaceAndTerminalUseTheCli() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ModelServer server;
    QVERIFY(server.Listener.isListening());
    const auto home = directory.filePath("agent");
    const auto first = directory.filePath("project");
    QString firstId, error;
    {
      WorkspaceClient workspace(home);
      QVERIFY2(workspace.initialize({}, error), qPrintable(error));
      QVERIFY(workspace.createProject("First", first));
      firstId = workspace.activeId();
      QVERIFY(!firstId.isEmpty());
      QVERIFY(workspace.createProject("Second", directory.filePath("second")));
      QVERIFY(workspace.activeId() != firstId);
      QCOMPARE(workspace.projects().size(), 2);
      QVERIFY(workspace.selectProject(firstId));
      QCOMPARE(workspace.activeId(), firstId);

      QFile sample(directory.filePath("object.bin"));
      QVERIFY(sample.open(QIODevice::WriteOnly));
      QCOMPARE(sample.write("fixture"), 7);
      sample.close();
      workspace.addObject(sample.fileName());
      QTRY_VERIFY(!workspace.selectedObjectId().isEmpty());
      const auto objectId = workspace.selectedObjectId();
      QVERIFY(workspace.addMemory(objectId, "Object memory"));
      QCOMPARE(workspace.memory().size(), 1);
      QVERIFY(workspace.updateModelSettings(
          server.profile().toJson().toVariantMap(), "", false, ""));
      QVERIFY(workspace.modelConfigured());
      workspace.setDraft("Workspace question");
      QCOMPARE(workspace.draft(), "Workspace question");
      QVERIFY(workspace.sendRequest(""));
      QTRY_COMPARE(server.Requests.size(), 1);
      server.respond(0, ModelProtocol::Responses, {}, {"list_objects"});
      QTRY_COMPARE(server.Requests.size(), 2);
      QVERIFY(
          QJsonDocument(server.Requests[1]).toJson().contains("object.bin"));
      server.respond(1, ModelProtocol::Responses, "Workspace answer");
      QTRY_VERIFY(!workspace.running());
      const auto messages = workspace.messages();
      QCOMPARE(messages.size(), 2);
      const auto answer = messages.last().toMap();
      QCOMPARE(answer.value("text").toString(), "Workspace answer");
      QVERIFY(!workspace.execution(answer.value("runId").toString()).isEmpty());
      QVERIFY(workspace.createSideChat());
      QVERIFY(workspace.sideChat());
      const auto inherited = workspace.sideChat()->inheritedMessages();
      QCOMPARE(inherited.size(), messages.size());
      QCOMPARE(inherited.last().toMap().value("text").toString(),
               "Workspace answer");
      workspace.sideChat()->setDraft("Side chat draft");
      QCOMPARE(workspace.sideChat()->draft(), "Side chat draft");
      QVERIFY(workspace.prepareClose());
    }
    {
      WorkspaceClient reopened(home);
      QVERIFY2(reopened.initialize(first, error), qPrintable(error));
      QCOMPARE(reopened.activeId(), firstId);
      QCOMPARE(reopened.messages().last().toMap().value("text").toString(),
               "Workspace answer");
      QVERIFY(reopened.sideChat());
      QCOMPARE(reopened.sideChat()->draft(), "Side chat draft");
      QVERIFY(reopened.prepareClose());
    }
    QProcess terminal;
    terminal.start(findAgentCliExecutable(),
                   {"run", "--data-dir", home, "--project", first, "--prompt",
                    "Terminal question"});
    QTRY_COMPARE(server.Requests.size(), 3);
    server.respond(2, ModelProtocol::Responses, "Terminal answer");
    QTRY_COMPARE(terminal.state(), QProcess::NotRunning);
    QCOMPARE(terminal.exitCode(), 0);
    QCOMPARE(terminal.readAllStandardOutput().trimmed(), "Terminal answer");
  }

  void uncertainWritesAreNotAutomaticallyRetried_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("state");
    QTest::newRow("lost-acknowledgement")
        << QString("lost_write") << QString("uncertain");
    QTest::newRow("saved-without-history-receipt")
        << QString("history_error") << QString("applied_unverified");
  }
  void uncertainWritesAreNotAutomaticallyRetried() {
    QFETCH(QString, mode);
    QFETCH(QString, state);
    QTemporaryDir directory;
    QString error;
    auto project =
        ProjectStore::create(directory.filePath("project"), "Review", error);
    QVERIFY(project.has_value());
    QFile fixture(directory.filePath("object.bin"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write(mode.toUtf8());
    fixture.close();
    QVERIFY(ProjectStore::addObject(
        *project, ProjectStore::fingerprint(fixture.fileName()), error));
    const auto executable = QDir(QCoreApplication::applicationDirPath())
                                .filePath(
#ifdef Q_OS_WIN
                                    "NeverDAgentFixtureMcp.exe"
#else
                                    "NeverDAgentFixtureMcp"
#endif
                                );
    QJsonObject result;
    bool done = false;
    ChangeReview::propose(*project,
                          {{"object_id", project->Objects[0].Id},
                           {"kind", "comment"},
                           {"address", "0x1000"},
                           {"after", "new"},
                           {"rationale", "fixture"},
                           {"evidence_ids", QJsonArray{}}},
                          executable, 5, this, [&](auto value) {
                            result = value;
                            done = true;
                          });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QVERIFY2(!result.contains("error"),
             QJsonDocument(result).toJson().constData());
    const auto id = result["proposal"].toObject()["id"].toString();
    done = false;
    ChangeReview::apply(*project, id, false, executable, 5, this,
                        [&](auto value) {
                          result = value;
                          done = true;
                        });
    QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
    QCOMPARE(result["status"].toString(), state);
    QFile marker(fixture.fileName() + ".accepted");
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QCOMPARE(marker.readAll(), QByteArray("accepted\n"));
    marker.close();
    done = false;
    ChangeReview::apply(*project, id, false, executable, 5, this,
                        [&](auto value) {
                          result = value;
                          done = true;
                        });
    QVERIFY(done);
    QVERIFY(result.contains("error"));
    QVERIFY(marker.open(QIODevice::ReadOnly));
    QCOMPARE(marker.readAll(), QByteArray("accepted\n"));
  }
  void sideQueueKeepsCapturedContextAndDraft() {
    ModelServer server;
    QTemporaryDir directory;
    QString error;
    auto project =
        ProjectStore::create(directory.filePath("project"), "Side", error);
    QVERIFY(project.has_value());
    SideChat chat(*project, QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(chat.load(error));
    chat.setDraft("first");
    QVERIFY(chat.send(server.profile(), "", "", {}, error));
    QTRY_COMPARE(server.Requests.size(), 1);
    QVERIFY(chat.setContext(
        QJsonArray{QJsonObject{{"kind", "selection"},
                               {"title", "Captured source"},
                               {"text", "immutable selected expression"}}},
        error));
    chat.setDraft("second");
    QVERIFY(chat.send(server.profile(), "", "", {}, error));
    chat.setDraft("unsent");
    QVERIFY(chat.setContext({}, error));
    server.respond(0, ModelProtocol::Responses, "one");
    QTRY_VERIFY(!chat.running());
    QVERIFY(chat.startNext(server.profile(), "", "", {}, error));
    QTRY_COMPARE(server.Requests.size(), 2);
    const auto wire = QJsonDocument(server.Requests[1]).toJson();
    QVERIFY(wire.contains("immutable selected expression"));
    QVERIFY(!wire.contains("unsent"));
    QCOMPARE(chat.draft(), "unsent");
    server.respond(1, ModelProtocol::Responses, "two");
    QTRY_VERIFY(!chat.running());
    SideChat restored(*project, chat.id());
    QVERIFY(restored.load(error));
    QCOMPARE(restored.draft(), "unsent");
    QCOMPARE(restored.messages().size(), 4);
  }
  void evidenceAndPlanSurviveRestartAndRejectWrongObject() {
    QTemporaryDir directory;
    QString error;
    auto project =
        ProjectStore::create(directory.filePath("project"), "Evidence", error);
    QVERIFY(project.has_value());
    AnalysisObject object{QUuid::createUuid().toString(QUuid::WithoutBraces),
                          "sample", "fixture", QString(64, 'a')};
    project->Objects.append(object);
    const QString text =
        QString(kMaxPreviewBytes - 1, 'a') + QString::fromUtf8("中🙂末");
    auto evidence = EvidenceStore::descriptor(
        *project, object, QUuid::createUuid().toString(QUuid::WithoutBraces),
        "read_disassembly", {{"address", "0x140001000"}}, {});
    const auto id = evidence["id"].toString();
    QVERIFY(EvidenceStore::save(*project, evidence, text, error));
    QVERIFY(EvidenceStore::read(*project, id, evidence, error));
    QString joined = evidence["text"].toString();
    const auto offset = evidence["nextOffset"].toInteger();
    QVERIFY(offset > kMaxPreviewBytes);
    QVERIFY(EvidenceStore::read(*project, id, evidence, error, offset));
    joined += evidence["text"].toString();
    QCOMPARE(joined, text);
    const QJsonObject plan{
        {"steps", QJsonArray{QJsonObject{{"id", "one"},
                                         {"title", "Validate"},
                                         {"status", "completed"},
                                         {"summary", "Checked return"},
                                         {"evidence_ids", QJsonArray{id}}}}},
        {"questions", QJsonArray{"Caller contract?"}}};
    QVERIFY(ResearchPlan::save(*project, "", plan, "run", error));
    QJsonObject restored;
    QVERIFY(ResearchPlan::read(*project, "", restored, error));
    QCOMPARE(restored["steps"], plan["steps"]);
    QVERIFY(ResearchPlan::read(
        *project, QUuid::createUuid().toString(QUuid::WithoutBraces), restored,
        error));
    QVERIFY(restored["steps"].toArray().isEmpty());
    const auto reference = EvidenceStore::reference(evidence);
    project->Objects[0].Sha256 = QString(64, 'b');
    QVERIFY(!EvidenceStore::read(*project, id, evidence, error));
    QVERIFY(!EvidenceStore::validateBindings(*project, {reference}, error));
    QVERIFY(!ResearchPlan::save(*project, "", plan, "run", error));
  }
  void snapshotEvidenceOutlivesItsMcpProcess() {
    QTemporaryDir directory;
    QString error;
    auto project =
        ProjectStore::create(directory.filePath("project"), "Snapshot", error);
    QVERIFY(project.has_value());
    QFile fixture(directory.filePath("object.bin"));
    QVERIFY(fixture.open(QIODevice::WriteOnly));
    fixture.write("local fixture");
    fixture.close();
    QVERIFY(ProjectStore::addObject(
        *project, ProjectStore::fingerprint(fixture.fileName()), error));
    const auto object = project->Objects[0];
    const auto executable = QDir(QCoreApplication::applicationDirPath())
                                .filePath(
#ifdef Q_OS_WIN
                                    "NeverDAgentFixtureMcp.exe"
#else
                                    "NeverDAgentFixtureMcp"
#endif
                                );
    QJsonObject result;
    QString evidenceId;
    {
      McpSession engine;
      engine.start(executable, object, 5, [&](auto reply) { result = reply; });
      QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 10000);
      QVERIFY2(engine.ready(), QJsonDocument(result).toJson().constData());
      result = {};
      engine.call("read_cfg", {}, [&](auto reply) { result = reply; });
      QTRY_VERIFY(!result.isEmpty());
      bool done = false;
      EvidenceCapture::captureAndSave(
          &engine, *project, object,
          QUuid::createUuid().toString(QUuid::WithoutBraces), "read_cfg", {},
          result, [&](auto captured) {
            result = captured;
            done = true;
          });
      QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
      QVERIFY2(result["evidence_complete"].toBool(),
               QJsonDocument(result).toJson().constData());
      evidenceId = result["evidence_id"].toString();
    }
    McpSession replacement;
    result = {};
    replacement.start(executable, object, 5,
                      [&](auto reply) { result = reply; });
    QTRY_VERIFY(!result.isEmpty());
    QVERIFY(replacement.ready());
    result = {};
    replacement.readResource("neverd://result/1",
                             [&](auto reply) { result = reply; });
    QTRY_VERIFY(!result.isEmpty());
    QVERIFY(result.contains("error"));
    QString content;
    qint64 offset = 0;
    do {
      QJsonObject page;
      QVERIFY2(EvidenceStore::read(*project, evidenceId, page, error, offset),
               qPrintable(error));
      content += page["text"].toString();
      offset = page["nextOffset"].toInteger();
    } while (offset >= 0);
    QCOMPARE(content, QString(600000, QChar(0x7ed3)) + " snapshot end");
  }
  void executionTracePreservesObjectAndSkippedCalls() {
    ExecutionTrace trace;
    trace.observe(
        {{"kind", "tool_request"},
         {"agentId", "analyst"},
         {"callId", "one"},
         {"tool", "nd_query"},
         {"elapsedMs", 10},
         {"node", QJsonObject{{"label", "sample.dll"},
                              {"objectId", "object"},
                              {"depth", 1}}},
         {"arguments",
          QJsonObject{{"tool", "read_cfg"},
                      {"arguments", "{\"address\":\"0x140001000\"}"}}}});
    trace.observe({{"kind", "tool_result"},
                   {"agentId", "analyst"},
                   {"callId", "one"},
                   {"elapsedMs", 55},
                   {"result", QJsonObject{{"evidence_id", "saved"}}}});
    const auto first = trace.rows()[0].toMap();
    QCOMPARE(first["address"].toString(), "0x140001000");
    QCOMPARE(first["objectId"].toString(), "object");
    QCOMPARE(first["elapsedMs"].toLongLong(), 45);
    trace.observe({{"kind", "tool_skipped"},
                   {"agentId", "analyst"},
                   {"callId", "two"},
                   {"tool", "read_cfg"},
                   {"result", QJsonObject{{"skipped", true}}}});
    QCOMPARE(trace.rows()[1].toMap()["state"].toString(), "skipped");
  }
  void contextDistanceUsesCurrentOccupation() {
    ModelProfile profile;
    profile.ContextTokens = 10000;
    profile.OutputTokens = 1000;
    QCOMPARE(profile.compactionThreshold(), 7200);
    ContextUsage usage;
    usage.observe(profile.Protocol,
                  {{"input_tokens", 6000}, {"output_tokens", 200}}, {});
    QCOMPARE(usage.toJson(profile)["remainingToCompaction"].toInteger(), 1000);
    usage.observe(profile.Protocol,
                  {{"input_tokens", 6300}, {"output_tokens", 100}}, {});
    QCOMPARE(usage.Tokens, 6400);
    QCOMPARE(usage.toJson(profile)["remainingToCompaction"].toInteger(), 800);
    usage.reset({{"input", "small checkpoint"}});
    QVERIFY(usage.toJson(profile)["remainingToCompaction"].toInteger() > 6000);
  }
  void queuedInputsKeepOrderAndDraft() {
    ModelServer server;
    QTemporaryDir directory;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("queue", directory.filePath("project")));
    QVERIFY(controller.updateModelSettings(
        server.profile().toJson().toVariantMap(), "", false, ""));
    controller.setDraft("first");
    QVERIFY(controller.sendRequest(""));
    QTRY_COMPARE(server.Requests.size(), 1);
    controller.setDraft("second");
    QVERIFY(controller.sendRequest(""));
    controller.setDraft("third");
    QVERIFY(controller.sendRequest(""));
    QCOMPARE(controller.pendingInputs().size(), 2);
    controller.setDraft("unsent draft");
    server.respond(0, ModelProtocol::Responses, "first answer");
    QTRY_COMPARE(server.Requests.size(), 2);
    QVERIFY(QJsonDocument(server.Requests[1]).toJson().contains("second"));
    QVERIFY(!QJsonDocument(server.Requests[1]).toJson().contains("third"));
    server.respond(1, ModelProtocol::Responses, "second answer");
    QTRY_COMPARE(server.Requests.size(), 3);
    server.respond(2, ModelProtocol::Responses, "third answer");
    QTRY_VERIFY(!controller.running());
    QCOMPARE(controller.pendingInputs().size(), 0);
    QCOMPARE(controller.draft(), "unsent draft");
    QCOMPARE(controller.messages().size(), 6);
  }
  void stopAndRestartRequireQueueResume() {
    ModelServer server;
    QTemporaryDir directory;
    {
      AgentController controller(directory.filePath("global"));
      QString error;
      QVERIFY(controller.initialize(error));
      QVERIFY(controller.createProject("queue", directory.filePath("project")));
      QVERIFY(controller.updateModelSettings(
          server.profile().toJson().toVariantMap(), "", false, ""));
      controller.setDraft("running");
      QVERIFY(controller.sendRequest(""));
      QTRY_COMPARE(server.Requests.size(), 1);
      controller.setDraft("saved queued request");
      QVERIFY(controller.sendRequest(""));
      controller.cancelRun();
      QTRY_VERIFY(!controller.running());
      QVERIFY(controller.queuePaused());
      QCOMPARE(controller.pendingInputs().size(), 1);
    }
    AgentController reopened(directory.filePath("global"));
    QString error;
    QVERIFY(reopened.initialize(error));
    QVERIFY(reopened.queuePaused());
    QCOMPARE(reopened.pendingInputs().size(), 1);
    QCOMPARE(server.Requests.size(), 1);
    QVERIFY(reopened.resumeInputQueue());
    QTRY_COMPARE(server.Requests.size(), 2);
    server.respond(1, ModelProtocol::Responses, "resumed");
    QTRY_VERIFY(!reopened.running());
  }
  void steeringSettlesPendingCalls_data() {
    QTest::addColumn<int>("protocol");
    QTest::newRow("responses") << int(ModelProtocol::Responses);
    QTest::newRow("messages") << int(ModelProtocol::Messages);
    QTest::newRow("chat") << int(ModelProtocol::ChatCompletions);
  }
  void steeringSettlesPendingCalls() {
    QFETCH(int, protocol);
    const auto codec = ModelProtocol(protocol);
    ModelServer server;
    QTemporaryDir directory;
    auto budget = std::make_shared<RunBudget>();
    AgentLoop loop("project", "root", server.profile(codec), "", budget);
    QString error;
    std::shared_ptr<neverd::plugin::ActionCall> first;
    int secondCalls = 0;
    QVERIFY(loop.tools().add(
        "first", "First read", {}, [&](auto call) { first = call; }, error));
    QVERIFY(loop.tools().add(
        "second", "Second read", {},
        [&](auto call) {
          ++secondCalls;
          NphTools::succeed(call, {{"unexpected", true}});
        },
        error));
    loop.start("Test only", "Original request",
               directory.filePath("context.json"));
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(0, codec, "", {"first", "second"});
    QTRY_VERIFY(first != nullptr);
    QVERIFY(loop.steer("correction",
                       "Only identify parameters. Do not enter the callee."));
    NphTools::succeed(first, {{"evidence", "completed first read"}});
    QTRY_COMPARE(server.Requests.size(), 2);
    QCOMPARE(secondCalls, 0);
    const auto wire = QJsonDocument(server.Requests[1]).toJson();
    QVERIFY(wire.contains("completed first read"));
    QVERIFY(wire.contains("Not executed"));
    QVERIFY(wire.contains("Only identify parameters"));
    QVERIFY(wire.indexOf("Not executed") <
            wire.indexOf("Only identify parameters"));
    server.respond(1, codec, "parameters identified");
    QTRY_VERIFY(!loop.running());
    QJsonObject saved;
    QVERIFY(ProjectStore::readJson(directory.filePath("context.json"), saved,
                                   error));
    QVERIFY(saved["groups"]
                .toArray()
                .last()
                .toObject()["prompt"]
                .toString()
                .contains("Only identify parameters"));
  }
  void steeringDuringFinalAnswerKeepsTaskOpen() {
    ModelServer server;
    auto budget = std::make_shared<RunBudget>();
    AgentLoop loop("project", "root", server.profile(), "", budget);
    loop.start("Test", "Initial question", "");
    QTRY_COMPARE(server.Requests.size(), 1);
    QVERIFY(loop.steer("new-input", "Also verify the return value."));
    server.respond(0, ModelProtocol::Responses, "initial answer");
    QTRY_COMPARE(server.Requests.size(), 2);
    QVERIFY(loop.running());
    QVERIFY(QJsonDocument(server.Requests[1])
                .toJson()
                .contains("Also verify the return value"));
    server.respond(1, ModelProtocol::Responses, "verified");
    QTRY_VERIFY(!loop.running());
  }
};
QTEST_GUILESS_MAIN(InteractionTests)
#include "InteractionTests.moc"
