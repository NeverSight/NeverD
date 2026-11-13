//===- ModelTests.cpp - Model protocol and execution contracts
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise fragmented protocols, real HTTP, NPH dispatch and project
/// isolation.
//===----------------------------------------------------------------------===//
#include "app/AgentController.h"
#include "models/Credentials.h"
#include "models/ProviderCodec.h"
#include "runtime/AgentLoop.h"
#include "runtime/ResearchRuntime.h"

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace neverd::agent;
namespace {
QByteArray sseEvent(QJsonObject object) {
  return "data: " + QJsonDocument(object).toJson(QJsonDocument::Compact) +
         "\r\n\r\n";
}
QJsonObject response(QJsonArray output) {
  return {{"status", "completed"},
          {"output", output},
          {"usage", QJsonObject{{"input_tokens", 12}, {"output_tokens", 7}}}};
}
QJsonArray answer(QString text) {
  return {QJsonObject{
      {"type", "message"},
      {"role", "assistant"},
      {"content",
       QJsonArray{QJsonObject{{"type", "output_text"}, {"text", text}}}}}};
}
QJsonObject toolCall(const QString &name, const QString &id,
                     const QJsonObject &arguments = {}) {
  return {{"type", "function_call"},
          {"call_id", id},
          {"name", name},
          {"arguments", QString::fromUtf8(QJsonDocument(arguments).toJson(
                            QJsonDocument::Compact))}};
}
class Server : public QObject {
public:
  QTcpServer Tcp;
  QList<QJsonObject> Requests;
  QList<QPointer<QTcpSocket>> Sockets;
  Server() {
    Tcp.listen(QHostAddress::LocalHost);
    connect(&Tcp, &QTcpServer::newConnection, this, [this] {
      auto *socket = Tcp.nextPendingConnection();
      auto bytes = std::make_shared<QByteArray>();
      connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
        *bytes += socket->readAll();
        auto end = bytes->indexOf("\r\n\r\n");
        if (end < 0)
          return;
        qsizetype length = 0;
        for (auto line : bytes->left(end).split('\n'))
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
  QString url() const {
    return QString("http://127.0.0.1:%1/responses").arg(Tcp.serverPort());
  }
  void respond(int i, QJsonObject object) {
    auto *socket = Sockets[i].data();
    const auto bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                  "application/json\r\nConnection: close\r\nContent-Length: " +
                  QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
    socket->disconnectFromHost();
  }
};
} // namespace
class ModelTests : public QObject {
  Q_OBJECT
private slots:
  void sideChatInheritsContextThenContinuesIndependently() {
    Server server;
    QTemporaryDir directory;
    QString firstSide, secondSide;
    {
      AgentController controller(directory.filePath("global"));
      QString error;
      QVERIFY(controller.initialize(error));
      QVERIFY(
          controller.createProject("project", directory.filePath("project")));
      ModelProfile profile;
      profile.Model = "mock";
      profile.BaseUrl = server.url();
      QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(),
                                             "", false, ""));
      const QJsonArray historicalItems{
          ProviderCodec::message("user", "Earlier analysis"),
          QJsonObject{{"type", "reasoning"},
                      {"encrypted_content", "opaque-history"}},
          QJsonObject{{"type", "function_call"},
                      {"call_id", "historical-call"},
                      {"name", "list_objects"},
                      {"arguments", "{}"}},
          QJsonObject{{"type", "function_call_output"},
                      {"call_id", "historical-call"},
                      {"output", "verified earlier evidence"}},
          ProviderCodec::message("assistant", "Earlier answer")};
      QVERIFY(ProjectStore::writeJson(
          directory.filePath("project/.nd-agent/router/context.json"),
          {{"version", 2},
           {"profile", profile.identity()},
           {"checkpoint", "Earlier checkpoint: field offset 0x48"},
           {"usage", ContextUsage{}.toJson(profile)},
           {"groups", QJsonArray{QJsonObject{{"prompt", "Earlier analysis"},
                                             {"answer", "Earlier answer"},
                                             {"items", historicalItems}}}}},
          error));
      controller.setDraft("main question");
      QVERIFY(controller.sendRequest(""));
      QTRY_COMPARE(server.Requests.size(), 1);
      QVERIFY(controller.createSideChat());
      auto *side = controller.sideChat();
      firstSide = side->id();
      side->setDraft("independent side question");
      QVERIFY(controller.sendSideRequest());
      QTRY_COMPARE(server.Requests.size(), 2);
      QVERIFY(
          QJsonDocument(server.Requests[1]).toJson().contains("main question"));
      const auto inherited = QJsonDocument(server.Requests[1]).toJson();
      QVERIFY(inherited.contains("field offset 0x48"));
      QVERIFY(inherited.contains("verified earlier evidence"));
      QVERIFY(inherited.contains("opaque-history"));
      QCOMPARE(side->inheritedMessages().size(), 1);
      QVERIFY(controller.running());
      QVERIFY(side->running());
      server.respond(1, response(answer("side answer")));
      QTRY_VERIFY(!side->running());
      QVERIFY(controller.running());
      QVERIFY(
          controller.messages().last().toMap()["text"].toString().isEmpty());
      controller.setDraft("main draft stays here");
      QCOMPARE(side->draft(), "");
      controller.quoteSideAnswer(
          side->messages().last().toMap()["id"].toString());
      QVERIFY(controller.draft().contains("main draft stays here"));
      QVERIFY(controller.draft().contains("> side answer"));
      QCOMPARE(server.Requests.size(), 2);
      server.respond(0, response(answer("main answer")));
      QTRY_VERIFY(!controller.running());
      QVERIFY(controller.draft().contains("> side answer"));
      QJsonObject mainContext, sideContext;
      QVERIFY(ProjectStore::readJson(
          directory.filePath("project/.nd-agent/router/context.json"),
          mainContext, error));
      QVERIFY(ProjectStore::readJson(
          directory.filePath("project/.nd-agent/sessions/" + firstSide +
                             "/router/context.json"),
          sideContext, error));
      QVERIFY(!QJsonDocument(mainContext).toJson().contains("side answer"));
      QVERIFY(!QJsonDocument(sideContext).toJson().contains("main answer"));
      QVERIFY(controller.createSideChat());
      secondSide = controller.sideChat()->id();
      QVERIFY(ProjectStore::readJson(
          directory.filePath("project/.nd-agent/sessions/" + secondSide +
                             "/router/context.json"),
          sideContext, error));
      QVERIFY(QJsonDocument(sideContext).toJson().contains("main answer"));
      QVERIFY(!QJsonDocument(sideContext).toJson().contains("side answer"));
      controller.sideChat()->setDraft("second side draft");
      QVERIFY(controller.flush(error));
    }
    AgentController reopened(directory.filePath("global"));
    QString error;
    QVERIFY(reopened.initialize(error));
    QCOMPARE(reopened.sideChats().size(), 2);
    QCOMPARE(reopened.sideChat()->id(), secondSide);
    QCOMPARE(reopened.sideChat()->draft(), "second side draft");
    QVERIFY(reopened.selectSideChat(firstSide));
    QCOMPARE(reopened.sideChat()->inheritedMessages().size(), 1);
    QCOMPARE(reopened.sideChat()->messages().last().toMap()["text"].toString(),
             "side answer");
  }
  void sideChatSnapshotDoesNotReplayPendingTools() {
    Server server;
    QTemporaryDir directory;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    auto budget = std::make_shared<RunBudget>();
    AgentLoop loop("project", "main", profile, "", budget);
    QString error;
    std::shared_ptr<neverd::plugin::ActionCall> pending;
    QVERIFY(loop.tools().add(
        "fast", "Return evidence", {},
        [](auto call) {
          NphTools::succeed(call, {{"evidence", "verified partial batch"}});
        },
        error));
    QVERIFY(loop.tools().add(
        "wait", "Wait for an external result", {},
        [&](auto call) {
          pending = call;
          std::weak_ptr<neverd::plugin::ActionCall> weak = call;
          call->onCancel([weak] {
            if (auto active = weak.lock())
              active->fail({"cancelled", "Cancelled by test"}, true);
          });
        },
        error));
    loop.start("Test", "Main question", directory.filePath("context.json"));
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(
        0, response({QJsonObject{{"type", "reasoning"},
                                 {"encrypted_content", "opaque-pending"}},
                     QJsonObject{{"type", "function_call"},
                                 {"call_id", "fast-call"},
                                 {"name", "fast"},
                                 {"arguments", "{}"}},
                     QJsonObject{{"type", "function_call"},
                                 {"call_id", "wait-call"},
                                 {"name", "wait"},
                                 {"arguments", "{}"}}}));
    QTRY_VERIFY(pending != nullptr);
    const auto context = loop.forkContext("Visible partial answer");
    const auto group = context["groups"].toArray().last().toObject();
    const auto items = group["items"].toArray();
    QCOMPARE(items.size(), 2);
    QCOMPARE(items[0].toObject()["role"].toString(), "user");
    QCOMPARE(items[1].toObject()["role"].toString(), "assistant");
    const auto evidence = items[1].toObject()["content"].toString();
    QVERIFY(evidence.contains("verified partial batch"));
    QVERIFY(evidence.contains("wait-call"));
    QVERIFY(evidence.contains("still pending"));
    QVERIFY(evidence.contains("Visible partial answer"));
    QVERIFY(!evidence.contains("opaque-pending"));
    loop.cancel();
    QTRY_VERIFY(!loop.running());
    QJsonObject stored;
    QVERIFY(ProjectStore::readJson(directory.filePath("context.json"), stored,
                                   error));
    const auto saved =
        stored["groups"].toArray().last().toObject()["items"].toArray();
    QStringList callIds, resultIds;
    for (const auto &value : saved) {
      const auto item = value.toObject();
      if (item["type"] == "function_call")
        callIds.append(item["call_id"].toString());
      if (item["type"] == "function_call_output")
        resultIds.append(item["call_id"].toString());
    }
    QCOMPARE(callIds, QStringList({"fast-call", "wait-call"}));
    QCOMPARE(resultIds, callIds);
    QVERIFY(QJsonDocument(saved).toJson().contains("verified partial batch"));
  }
  void modelCallsContinueBeyondObsoleteLimits() {
    Server server;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    auto legacy = profile.toJson();
    legacy["maxSteps"] = 1;
    QString error;
    const auto restored = ModelProfile::fromJson(legacy, error);
    QVERIFY2(restored.has_value(), qPrintable(error));
    QVERIFY(!restored->toJson().contains("maxSteps"));
    auto budget = std::make_shared<RunBudget>();
    budget->Steps = 50; // Work in other contexts cannot starve this one.
    AgentLoop loop("project", "session", *restored, "", budget);
    int calls = 0;
    QVERIFY(loop.tools().add(
        "inspect", "inspect", {},
        [&](auto call) {
          ++calls;
          NphTools::succeed(call, {{"evidence", "verified finding"}});
        },
        error));
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "question", "");
    for (int i = 0; i < 40; ++i) {
      QTRY_COMPARE(server.Requests.size(), i + 1);
      QVERIFY(!server.Requests[i]["tools"].toArray().isEmpty());
      server.respond(i, response({toolCall("inspect", QString::number(i))}));
    }
    QTRY_COMPARE(server.Requests.size(), 41);
    QCOMPARE(calls, 40);
    QVERIFY(!server.Requests[40]["tools"].toArray().isEmpty());
    QCOMPARE(server.Requests[40]["instructions"].toString(), "system");
    server.respond(40, response(answer("All requested work completed")));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toString(), "All requested work completed");
    QVERIFY(done[0][1].toString().isEmpty());
    QCOMPARE(loop.steps(), 41);
    QCOMPARE(budget->Steps, 91);
    QCOMPARE(budget->Active, 0);
  }
  void concurrentToolsKeepResultOrderAndSerialBarriers() {
    Server server;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    AgentLoop loop("project", "session", profile, "",
                   std::make_shared<RunBudget>());
    QList<std::shared_ptr<neverd::plugin::ActionCall>> pending;
    int serial = 0;
    QString error;
    QVERIFY(loop.tools().add(
        "serial", "Run in order", {},
        [&](auto call) {
          ++serial;
          NphTools::succeed(call, {{"serial", serial}});
        },
        error));
    QVERIFY(loop.tools().add(
        "parallel", "Independent work", {},
        [&](auto call) { pending.append(call); }, error,
        neverd::plugin::ActionEffect::Read, true));
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "question", "");
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(
        0, response({toolCall("serial", "before"), toolCall("parallel", "one"),
                     toolCall("parallel", "two"), toolCall("parallel", "three"),
                     toolCall("serial", "after")}));
    QTRY_COMPARE(pending.size(), 3);
    QCOMPARE(serial, 1);
    NphTools::succeed(pending[2], {{"answer", "third"}});
    NphTools::succeed(pending[1], {{"answer", "second"}});
    QTRY_VERIFY(loop.forkContext("")
                    .value("groups")
                    .toArray()
                    .last()
                    .toObject()["answer"]
                    .toString()
                    .contains("third"));
    QCOMPARE(serial, 1);
    QCOMPARE(server.Requests.size(), 1);
    NphTools::succeed(pending[0], {{"answer", "first"}});
    QTRY_COMPARE(server.Requests.size(), 2);
    QCOMPARE(serial, 2);
    QStringList ids, answers;
    for (const auto &value : server.Requests[1]["input"].toArray()) {
      const auto item = value.toObject();
      if (item["type"] == "function_call_output") {
        ids.append(item["call_id"].toString());
        answers.append(
            QJsonDocument::fromJson(item["output"].toString().toUtf8())
                .object()["answer"]
                .toString());
      }
    }
    QCOMPARE(ids, QStringList({"before", "one", "two", "three", "after"}));
    QCOMPARE(answers, QStringList({"", "first", "second", "third", ""}));
    server.respond(1, response(answer("done")));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done[0][1].toString().isEmpty());
  }
  void objectDelegationsQueueWithoutStarvingTheCoordinator_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("five-objects-and-summary") << false;
    QTest::newRow("cancel-with-queued-objects") << true;
  }
  void auditFailureStopsTheConcurrentBatch() {
    Server server;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    AgentLoop loop("p", "s", profile, "", std::make_shared<RunBudget>());
    QString error;
    int requests = 0, effects = 0;
    QVERIFY(loop.tools().add(
        "parallel", "Independent work", {},
        [&](auto call) {
          ++effects;
          NphTools::succeed(call, {{"done", true}});
        },
        error, neverd::plugin::ActionEffect::Read, true));
    loop.Audit = [&](const QJsonObject &event, QString &issue) {
      requests += event["kind"] == "tool_request";
      if (event["kind"] == "tool_result") {
        issue = "Injected audit failure";
        return false;
      }
      return true;
    };
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "question", "");
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(0, response({toolCall("parallel", "invalid", {{"extra", 1}}),
                                toolCall("parallel", "unstarted")}));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done[0][1].toString().contains("Injected audit failure"));
    QCOMPARE(requests, 1);
    QCOMPARE(effects, 0);
  }
  void objectDelegationsQueueWithoutStarvingTheCoordinator() {
    QFETCH(bool, cancel);
    Server server;
    QTemporaryDir directory;
    QString error;
    auto project =
        ProjectStore::create(directory.filePath("project"), "Test", error);
    QVERIFY2(project.has_value(), qPrintable(error));
    for (int i = 0; i < 5; ++i) {
      QFile file(directory.filePath(QString("object-%1.bin").arg(i)));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QByteArray::number(i));
      file.close();
      QVERIFY(ProjectStore::addObject(
          *project, ProjectStore::fingerprint(file.fileName()), error));
    }
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    profile.MaxAgents = 3;
    auto budget = std::make_shared<RunBudget>();
    ResearchRunOptions options;
    options.Budget = budget;
    ResearchRuntime runtime;
    QSignalSpy done(&runtime, &ResearchRuntime::finished);
    QVERIFY(runtime.start(*project, profile, "", "", "Investigate", "", {},
                          error, options));
    QTRY_COMPARE(server.Requests.size(), 1);
    QJsonArray calls;
    for (int i = 0; i < 5; ++i)
      calls.append(toolCall("delegate_object", QString("object-%1").arg(i),
                            {{"object_id", project->Objects[i].Id},
                             {"task", QString("task-%1").arg(i)}}));
    server.respond(0, response(calls));
    QTRY_COMPARE(server.Requests.size(), 3);    // Coordinator + two analysts.
    QTRY_COMPARE(runtime.progress().size(), 6); // Three objects remain queued.
    QCOMPARE(budget->Active, 3);
    if (cancel) {
      runtime.cancel();
      QTRY_COMPARE(done.size(), 1);
      QCOMPARE(done[0][2].toBool(), true);
      QCOMPARE(budget->Active, 0);
      QCOMPARE(server.Requests.size(), 3);
      return;
    }
    // Finish the second analyst first, then keep filling its released slot.
    for (int i = 2; i < 6; ++i) {
      QTRY_COMPARE(server.Requests.size(), i + 1);
      const auto task = server.Requests[i]["input"]
                            .toArray()
                            .first()
                            .toObject()["content"]
                            .toString();
      server.respond(i, response(answer("verified " + task)));
    }
    QTRY_COMPARE(budget->Active,
                 2); // The first analyst still holds its result.
    QCOMPARE(server.Requests.size(), 6);
    const auto firstTask = server.Requests[1]["input"]
                               .toArray()
                               .first()
                               .toObject()["content"]
                               .toString();
    server.respond(1, response(answer("verified " + firstTask)));
    QTRY_COMPARE(server.Requests.size(), 7);
    QStringList ids;
    for (const auto &value : server.Requests[6]["input"].toArray()) {
      const auto item = value.toObject();
      if (item["type"] != "function_call_output")
        continue;
      ids.append(item["call_id"].toString());
      const int index = ids.size() - 1;
      const auto result =
          QJsonDocument::fromJson(item["output"].toString().toUtf8()).object();
      QCOMPARE(result["answer"].toString(),
               QString("verified task-%1").arg(index));
      QCOMPARE(result["objectId"].toString(), project->Objects[index].Id);
    }
    QCOMPARE(ids, QStringList({"object-0", "object-1", "object-2", "object-3",
                               "object-4"}));
    server.respond(6, response(answer("Conclusions from all five objects")));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY2(done[0][1].toString().isEmpty(),
             qPrintable(done[0][1].toString()));
    QCOMPARE(budget->Steps, 7);
    QCOMPARE(budget->Active, 0);
  }
  void cancellationBeforeDeferredStartPreventsNetwork() {
    Server server;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    auto budget = std::make_shared<RunBudget>();
    AgentLoop loop("p", "s", profile, "", budget);
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.cancel();
    loop.start("system", "question", "");
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(server.Requests.size(), 0);
    QCOMPARE(budget->Active, 0);
    QVERIFY(done[0][2].toBool());
  }
  void persistentGoalContinuesAndCompletes() {
    Server server;
    QTemporaryDir directory;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("project", directory.filePath("project")));
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(), "",
                                           false, ""));
    controller.setDraft("unrelated draft");
    QVERIFY(controller.startGoal("Verify both steps", 100000, 4));
    QTRY_VERIFY(!server.Requests.isEmpty() || !controller.running());
    QVERIFY2(controller.running(),
             qPrintable(controller.goal()["error"].toString()));
    for (int round = 0; round < 2; ++round) {
      const int request = round * 2;
      QTRY_COMPARE(server.Requests.size(), request + 1);
      const auto parameters = QJsonObject{
          {"status", round ? "completed" : "continue"},
          {"summary", round ? "Both steps verified" : "First step verified"},
          {"next_step", round ? "" : "Verify the second step"},
          {"evidence", "Observed test results"}};
      server.respond(
          request,
          response({QJsonObject{
              {"type", "function_call"},
              {"call_id", QString("goal-%1").arg(round)},
              {"name", "report_goal_progress"},
              {"arguments",
               QString::fromUtf8(QJsonDocument(parameters)
                                     .toJson(QJsonDocument::Compact))}}}));
      QTRY_COMPARE(server.Requests.size(), request + 2);
      server.respond(request + 1,
                     response(answer(round ? "Completed with evidence"
                                           : "First step done")));
    }
    QTRY_COMPARE(controller.goal()["status"].toString(), "completed");
    QCOMPARE(controller.goal()["rounds"].toInt(), 2);
    QCOMPARE(controller.goal()["usedTokens"].toLongLong(), 76);
    QCOMPARE(controller.goal()["pendingTokens"].toLongLong(), 0);
    QCOMPARE(controller.draft(), "unrelated draft");
    QCOMPARE(controller.messages().size(), 3);
    QJsonObject saved;
    QVERIFY(ProjectStore::readJson(
        directory.filePath("project/.nd-agent/router/goal.json"), saved,
        error));
    QCOMPARE(saved["status"].toString(), "completed");
    QVERIFY(saved["evidence"].toString().contains("Observed"));
  }
  void goalPausePreservesUsageAndStopsContinuation() {
    Server server;
    QTemporaryDir directory;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("project", directory.filePath("project")));
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(), "",
                                           false, ""));
    QVERIFY(controller.startGoal("Test pause", 100000, 4));
    QTRY_VERIFY(!server.Requests.isEmpty() || !controller.running());
    QVERIFY2(controller.running(),
             qPrintable(controller.goal()["error"].toString()));
    QTRY_COMPARE(server.Requests.size(), 1);
    controller.pauseGoal();
    QTRY_VERIFY(!controller.running());
    QCOMPARE(controller.goal()["status"].toString(), "paused");
    const auto spent = controller.goal()["usedTokens"].toLongLong();
    QVERIFY(spent > 0);
    QCOMPARE(controller.goal()["pendingTokens"].toLongLong(), 0);
    QCOMPARE(server.Requests.size(), 1);
    QVERIFY(controller.updateGoal("Test pause", 120000, 6));
    QCOMPARE(controller.goal()["usedTokens"].toLongLong(), spent);
    QVERIFY(controller.resumeGoal());
    QTRY_COMPARE(server.Requests.size(), 2);
    server.respond(1, response(answer("No structured progress")));
    QTRY_COMPARE(controller.goal()["status"].toString(), "blocked");
    QCOMPARE(controller.goal()["rounds"].toInt(), 2);
    QCOMPARE(controller.goal()["usedTokens"].toLongLong(), spent + 19);
    QCOMPARE(server.Requests.size(), 2);
  }
  void goalBudgetStopsBeforeNetwork() {
    Server server;
    QTemporaryDir directory;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("project", directory.filePath("project")));
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(), "",
                                           false, ""));
    QVERIFY(controller.startGoal("Insufficient budget", 1, 2));
    QTRY_COMPARE(controller.goal()["status"].toString(), "budget_exhausted");
    QCOMPARE(server.Requests.size(), 0);
    QCOMPARE(controller.goal()["usedTokens"].toLongLong(), 0);
  }
  void goalRoundLimitStopsAutomaticContinuation() {
    Server server;
    QTemporaryDir directory;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("project", directory.filePath("project")));
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(), "",
                                           false, ""));
    QVERIFY(controller.startGoal("Bounded exploration", 100000, 1));
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(
        0,
        response({QJsonObject{
            {"type", "function_call"},
            {"call_id", "progress"},
            {"name", "report_goal_progress"},
            {"arguments",
             R"({"status":"continue","summary":"First step done","next_step":"Second step","evidence":"First observation"})"}}}));
    QTRY_COMPARE(server.Requests.size(), 2);
    server.respond(1, response(answer("More work remains")));
    QTRY_COMPARE(controller.goal()["status"].toString(), "round_limit");
    QVERIFY(!controller.running());
    QVERIFY(!controller.resumeGoal());
    QCoreApplication::processEvents();
    QCOMPARE(server.Requests.size(), 2);
    QCOMPARE(controller.goal()["rounds"].toInt(), 1);
  }
  void interruptedGoalNeedsExplicitResume() {
    QTemporaryDir directory;
    QString error;
    const auto project =
        ProjectStore::create(directory.filePath("project"), "p", error);
    QVERIFY(project.has_value());
    GoalState original;
    original.Id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    original.Objective = "Continue after restart";
    original.Status = "active";
    original.TokenBudget = 10000;
    original.UsedTokens = 100;
    original.PendingTokens = 700;
    original.Rounds = 1;
    original.RoundLimit = 5;
    original.CreatedAt =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QVERIFY(original.save(*project, error));
    GoalState resumed;
    QVERIFY(GoalState::load(*project, resumed, error));
    QCOMPARE(resumed.Status, "interrupted");
    QCOMPARE(resumed.UsedTokens, 800);
    QCOMPARE(resumed.PendingTokens, 0);
    QVERIFY(resumed.Estimated);
    QJsonObject malformed = resumed.toJson();
    malformed["tokenBudget"] = -1;
    const auto path = directory.filePath("project/.nd-agent/router/goal.json");
    QVERIFY(ProjectStore::writeJson(path, malformed, error));
    QVERIFY(!GoalState::load(*project, resumed, error));
    QJsonObject untouched;
    QVERIFY(ProjectStore::readJson(path, untouched, error));
    QCOMPARE(untouched, malformed);
  }
  void contextAccountingUsesOneRequestAndIncludesCache() {
    ModelProfile profile;
    ContextUsage usage;
    const QJsonObject request{{"input", "test"}};
    usage.observe(
        ModelProtocol::Responses,
        {{"input_tokens", 700},
         {"output_tokens", 50},
         {"input_tokens_details", QJsonObject{{"cached_tokens", 600}}}},
        request);
    QCOMPARE(usage.Tokens, 750);
    QCOMPARE(usage.Cached, 600);
    QVERIFY(!usage.Estimated);
    usage.prepare({{"input", QString(303, 'a')}});
    QVERIFY(usage.Tokens > 750);
    QVERIFY(usage.Estimated);
    usage.observe(ModelProtocol::Messages,
                  {{"input_tokens", 10},
                   {"output_tokens", 20},
                   {"cache_read_input_tokens", 100},
                   {"cache_creation_input_tokens", 30}},
                  request);
    QCOMPARE(usage.Tokens, 160);
    QCOMPARE(usage.Input, 140);
    usage.observe(
        ModelProtocol::ChatCompletions,
        {{"prompt_tokens", 200},
         {"completion_tokens", 10},
         {"prompt_tokens_details", QJsonObject{{"cached_tokens", 150}}}},
        request);
    QCOMPARE(usage.Tokens, 210); // Not 160 + 210, and cache not counted twice.
    ContextUsage resumed;
    QVERIFY(resumed.restore(usage.toJson(profile)));
    QCOMPARE(resumed.Tokens, usage.Tokens);
    auto malformed = usage.toJson(profile);
    malformed["tokens"] = -1;
    QVERIFY(!resumed.restore(malformed));
    usage.observe(ModelProtocol::Responses, {}, request);
    QVERIFY(usage.Estimated);
  }
  void semanticCompactionAndResume() {
    Server server;
    QTemporaryDir directory;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    profile.ContextTokens = 12000;
    profile.OutputTokens = 1000;
    const auto path = directory.filePath("context.json");
    const auto old = QJsonObject{
        {"prompt", "Analyze parser. Preserve unwind handlers exactly."},
        {"answer", "Findings"},
        {"items", QJsonArray{ProviderCodec::message("user", "Analyze parser"),
                             ProviderCodec::message("assistant",
                                                    "Address 0x180001030. " +
                                                        QString(33000, 'x'))}}};
    const QJsonObject recent{
        {"prompt", "recent lookup"},
        {"answer", "recent evidence"},
        {"items",
         QJsonArray{ProviderCodec::message("user", "recent lookup"),
                    QJsonObject{{"type", "function_call"},
                                {"call_id", "recent-call"},
                                {"name", "inspect"},
                                {"arguments", "{}"}},
                    QJsonObject{{"type", "function_call_output"},
                                {"call_id", "recent-call"},
                                {"output", "recent evidence"}},
                    ProviderCodec::message("assistant", "recent evidence")}}};
    const QJsonObject recentSecond{
        {"prompt", "Check the pointer width"},
        {"answer", "RAW_RECENT_POINTER_64"},
        {"items",
         QJsonArray{
             ProviderCodec::message("user", "Check the pointer width"),
             ProviderCodec::message("assistant", "RAW_RECENT_POINTER_64")}}};
    QString error;
    QVERIFY(ProjectStore::writeJson(
        path,
        {{"version", 1},
         {"profile", profile.identity()},
         {"groups", QJsonArray{old, recent, recentSecond}}},
        error));
    AgentLoop loop("p", "s", profile, "", std::make_shared<RunBudget>());
    QSignalSpy done(&loop, &AgentLoop::finished);
    QJsonArray audit;
    loop.Audit = [&](auto event, auto &) {
      audit.append(event);
      return true;
    };
    loop.start("Object sample.dll, SHA256 exact-hash", "Continue: verify -5",
               path);
    int i = 0, summaries = 0;
    for (; i < 6; ++i) {
      QTRY_VERIFY(server.Requests.size() > i);
      const auto request = server.Requests[i];
      if (request["instructions"].toString().startsWith(
              compactionInstructions())) {
        ++summaries;
        QVERIFY(!request.contains("tools"));
        QVERIFY(!QJsonDocument(request).toJson().contains("recent evidence"));
        QVERIFY(
            !QJsonDocument(request).toJson().contains("RAW_RECENT_POINTER_64"));
        QVERIFY(ContextUsage::estimate(request) <
                profile.ContextTokens - profile.OutputTokens);
        server.respond(i,
                       response(answer(
                           "目标：验证 -5。sample.dll SHA256 exact-hash。"
                           "已确认 0x180001030 是解析入口；待核对校验分支。")));
      } else {
        const auto input = request["input"].toArray();
        QVERIFY(input.first().toObject()["content"].toString().contains(
            "0x180001030"));
        QCOMPARE(input.last().toObject()["content"].toString(),
                 "Continue: verify -5");
        QVERIFY(QJsonDocument(input).toJson().contains(
            "Preserve unwind handlers exactly"));
        QVERIFY(QJsonDocument(input).toJson().contains("recent evidence"));
        QVERIFY(
            QJsonDocument(input).toJson().contains("RAW_RECENT_POINTER_64"));
        server.respond(i, response(answer("Verified -5")));
        break;
      }
    }
    QVERIFY(summaries >=
            2); // Oversized saved history is summarized in bounded segments.
    QTRY_COMPARE(done.size(), 1);
    QVERIFY2(done[0][1].toString().isEmpty(),
             qPrintable(done[0][1].toString()));
    QCOMPARE(loop.contextUsage()["compactions"].toInt(), 1);
    QJsonObject saved;
    QVERIFY(ProjectStore::readJson(path, saved, error));
    QCOMPARE(saved["version"].toInt(), 2);
    QVERIFY(saved["checkpoint"].toString().contains("0x180001030"));
    QCOMPARE(saved["groups"].toArray().size(), 4);
    QVERIFY(saved["groups"].toArray()[0].toObject()["requestOnly"].toBool());
    QCOMPARE(saved["groups"].toArray()[1].toObject(), recent);
    QCOMPARE(saved["groups"].toArray()[2].toObject(), recentSecond);
    QCOMPARE(saved["usage"].toObject()["retainedTurns"].toInteger(), 2);
    QCOMPARE(saved["usage"].toObject()["preservedRequests"].toInteger(), 1);
    QVERIFY(!audit.isEmpty());
    AgentLoop resumed("p", "next", profile, "", std::make_shared<RunBudget>());
    QSignalSpy resumedDone(&resumed, &AgentLoop::finished);
    resumed.start("Object sample.dll", "What remains?", path);
    QTRY_COMPARE(server.Requests.size(), i + 2);
    const auto input = server.Requests.last()["input"].toArray();
    QVERIFY(
        input.first().toObject()["content"].toString().contains("0x180001030"));
    server.respond(i + 1, response(answer("Nothing")));
    QTRY_COMPARE(resumedDone.size(), 1);
    QVERIFY(resumedDone[0][1].toString().isEmpty());
  }
  void preservedRequestsMigrateWithoutEmptyAssistantMessages() {
    Server server;
    QTemporaryDir directory;
    QString error;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    const auto path = directory.filePath("context.json");
    const QJsonObject request{
        {"prompt", "Keep exception handlers"},
        {"answer", ""},
        {"requestOnly", true},
        {"items", QJsonArray{ProviderCodec::message(
                      "user", "Keep exception handlers")}}};
    QVERIFY(ProjectStore::writeJson(path,
                                    {{"version", 2},
                                     {"profile", profile.identity()},
                                     {"checkpoint", "Earlier checkpoint"},
                                     {"usage", ContextUsage{}.toJson(profile)},
                                     {"groups", QJsonArray{request}}},
                                    error));
    profile.Protocol = ModelProtocol::Messages;
    AgentLoop loop("project", "analyst", profile, "",
                   std::make_shared<RunBudget>());
    QSignalSpy finished(&loop, &AgentLoop::finished);
    loop.start("system", "Continue with the caller", path);
    QTRY_COMPARE(server.Requests.size(), 1);
    const auto messages = server.Requests[0]["messages"].toArray();
    QVERIFY(
        QJsonDocument(messages).toJson().contains("Keep exception handlers"));
    for (const auto &message : messages)
      QCOMPARE(message.toObject()["role"].toString(), "user");
    server.respond(
        0,
        {{"id", "m"},
         {"type", "message"},
         {"role", "assistant"},
         {"stop_reason", "end_turn"},
         {"content",
          QJsonArray{QJsonObject{{"type", "text"}, {"text", "Continued"}}}},
         {"usage", QJsonObject{{"input_tokens", 20}, {"output_tokens", 5}}}});
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY2(finished[0][1].toString().isEmpty(),
             qPrintable(finished[0][1].toString()));
  }
  void compactionRetainsMultipleActiveToolExchanges() {
    Server server;
    QTemporaryDir directory;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    profile.ContextTokens = 12000;
    profile.OutputTokens = 1000;
    AgentLoop loop("project", "analyst", profile, "",
                   std::make_shared<RunBudget>());
    QString error;
    int calls = 0;
    QVERIFY(loop.tools().add(
        "inspect", "Inspect", {},
        [&](auto call) {
          ++calls;
          NphTools::succeed(
              call,
              {{"evidence", calls == 1 ? "OLDER_PROOF" + QString(19000, 'x')
                                       : QString("EXACT_RECENT_%1").arg(calls) +
                                             QString(3000, 'y')}});
        },
        error));
    QSignalSpy finished(&loop, &AgentLoop::finished);
    loop.start("Analysis rules", "Keep exact addresses",
               directory.filePath("context.json"));
    for (int i = 0; i < 3; ++i) {
      QTRY_COMPARE(server.Requests.size(), i + 1);
      QVERIFY(!server.Requests[i]["instructions"].toString().startsWith(
          compactionInstructions()));
      auto reply =
          response({QJsonObject{{"type", "function_call"},
                                {"name", "inspect"},
                                {"call_id", QString("call-%1").arg(i + 1)},
                                {"arguments", "{}"}}});
      reply["usage"] = QJsonObject{{"input_tokens", i == 0   ? 20
                                                    : i == 1 ? 6500
                                                             : 8000},
                                   {"output_tokens", 10}};
      server.respond(i, reply);
    }
    QTRY_COMPARE(server.Requests.size(), 4);
    QVERIFY(server.Requests[3]["instructions"].toString().startsWith(
        compactionInstructions()));
    const auto summarized = QJsonDocument(server.Requests[3]).toJson();
    QVERIFY(summarized.contains("OLDER_PROOF"));
    QVERIFY(!summarized.contains("EXACT_RECENT_2"));
    QVERIFY(!summarized.contains("EXACT_RECENT_3"));
    server.respond(
        3, response(
               answer("Earlier proof preserved. Continue the current task.")));
    QTRY_COMPARE(server.Requests.size(), 5);
    const auto continued = QJsonDocument(server.Requests[4]).toJson();
    QVERIFY(continued.contains("call-2") &&
            continued.contains("EXACT_RECENT_2"));
    QVERIFY(continued.contains("call-3") &&
            continued.contains("EXACT_RECENT_3"));
    QVERIFY(!continued.contains("call-1"));
    QCOMPARE(loop.contextUsage()["retainedExchanges"].toInt(), 2);
    QCOMPARE(calls, 3);
    server.respond(4, response(answer("Done")));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY2(finished[0][1].toString().isEmpty(),
             qPrintable(finished[0][1].toString()));
  }
  void compactionWaitsForEntireToolBatch() {
    Server server;
    QTemporaryDir directory;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    profile.ContextTokens = 12000;
    profile.OutputTokens = 1000;
    AgentLoop loop("p", "s", profile, "", std::make_shared<RunBudget>());
    int calls = 0;
    QString error;
    QVERIFY(loop.tools().add(
        "inspect", "inspect", {},
        [&](auto call) {
          ++calls;
          NphTools::succeed(call, {{"evidence", calls == 1 ? QString(31000, 'x')
                                                           : "second-result"}});
        },
        error));
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "original question",
               directory.filePath("context.json"));
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(0, response({QJsonObject{{"type", "function_call"},
                                            {"name", "inspect"},
                                            {"call_id", "one"},
                                            {"arguments", "{}"}},
                                QJsonObject{{"type", "function_call"},
                                            {"name", "inspect"},
                                            {"call_id", "two"},
                                            {"arguments", "{}"}}}));
    int i = 1;
    QString evidence;
    for (; i < 6; ++i) {
      QTRY_VERIFY(server.Requests.size() > i);
      QCOMPARE(calls, 2);
      const auto request = server.Requests[i];
      if (request["instructions"].toString().startsWith(
              compactionInstructions())) {
        evidence +=
            request["input"].toArray().first().toObject()["content"].toString();
        server.respond(
            i, response(answer("Both inspect calls completed. Preserve "
                               "second-result; original question remains.")));
      } else {
        QVERIFY(evidence.contains("second-result"));
        const auto input = request["input"].toArray();
        QCOMPARE(input.last().toObject()["content"].toString(),
                 "original question");
        // The oversized batch was summarized as a whole, no orphaned
        // call/result.
        for (const auto &item : input)
          QVERIFY(item.toObject()["type"] != "function_call_output");
        server.respond(i, response(answer("done")));
        break;
      }
    }
    QTRY_COMPARE(done.size(), 1);
    QVERIFY2(done[0][1].toString().isEmpty(),
             qPrintable(done[0][1].toString()));
    QCOMPARE(calls, 2);
  }
  void failedCompactionPreservesHistory_data() {
    QTest::addColumn<bool>("cancel");
    QTest::newRow("empty-summary") << false;
    QTest::newRow("cancel") << true;
  }
  void failedCompactionPreservesHistory() {
    QFETCH(bool, cancel);
    Server server;
    QTemporaryDir directory;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    profile.ContextTokens = 12000;
    profile.OutputTokens = 1000;
    const auto path = directory.filePath("context.json");
    const QJsonArray groups{QJsonObject{
        {"prompt", "original"},
        {"answer", "evidence"},
        {"items", QJsonArray{ProviderCodec::message("user", "original"),
                             ProviderCodec::message("assistant",
                                                    QString(33000, 'x'))}}}};
    QString error;
    QVERIFY(ProjectStore::writeJson(
        path,
        {{"version", 1}, {"profile", profile.identity()}, {"groups", groups}},
        error));
    AgentLoop loop("p", "s", profile, "", std::make_shared<RunBudget>());
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "continue", path);
    QTRY_COMPARE(server.Requests.size(), 1);
    if (cancel)
      loop.cancel();
    else
      server.respond(0, response(answer("")));
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(!done[0][1].toString().isEmpty());
    QJsonObject saved;
    QVERIFY(ProjectStore::readJson(path, saved, error));
    QVERIFY(saved["checkpoint"].toString().isEmpty());
    QCOMPARE(saved["groups"].toArray().first(), groups.first());
    QCOMPARE(saved["usage"].toObject()["compactions"].toInt(), 0);
    QCOMPARE(server.Requests.size(), 1);
  }
  void profilesAndCredentialValidation() {
    ModelProfile profile;
    profile.Model = "test";
    QCOMPARE(profile.endpoint().path(), "/v1/responses");
    profile.BaseUrl = "https://api.deepseek.com/responses";
    QCOMPARE(profile.endpoint().path(), "/responses");
    QString error;
    profile.Protocol = ModelProtocol::Messages;
    QVERIFY(!profile.validate(error));
    profile.BaseUrl = "https://api.deepseek.com/anthropic";
    QCOMPARE(profile.endpoint().path(), "/anthropic/v1/messages");
    QVERIFY(profile.validate(error));
    QVERIFY(!Credentials::validateKey("bad\r\nheader", error));
  }
  void nativeCredentialsRoundTrip() {
#ifndef Q_OS_WIN
    if (!qEnvironmentVariableIsSet("NEVERD_AGENT_KEYRING_TESTS"))
      QSKIP(
          "Set NEVERD_AGENT_KEYRING_TESTS=1 with an unlocked native keyring.");
#endif
    QTemporaryDir directory;
    Credentials credentials(directory.path());
    QString id, key, error;
    QVERIFY(credentials.store("test-secret", id, error));
    const auto cleanup = qScopeGuard([&] { credentials.remove(id); });
    QVERIFY(credentials.read(id, key, error));
    QCOMPARE(key, "test-secret");
    QFile file(directory.filePath("credentials/" + id + ".key"));
#ifdef Q_OS_WIN
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(!file.readAll().contains("test-secret"));
#else
    QVERIFY(!file.exists());
    // Import the previous Unix format under the same reference, then delete
    // its plaintext copy only after the keyring write succeeds.
    QVERIFY(QDir().mkpath(directory.filePath("credentials")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("legacy-test-secret");
    file.close();
    QVERIFY2(credentials.read(id, key, error), qPrintable(error));
    QCOMPARE(key, "legacy-test-secret");
    QVERIFY(!file.exists());
    QVERIFY(credentials.read(id, key, error));
    QCOMPARE(key, "legacy-test-secret");
#endif
    file.close();
    QVERIFY(!credentials.read("../config", key, error));
    QVERIFY(credentials.remove(id));
    QVERIFY(!credentials.read(id, key, error));
  }
  void modelSettingsWaitForCredentialCommitAndReload() {
#ifndef Q_OS_WIN
    if (!qEnvironmentVariableIsSet("NEVERD_AGENT_KEYRING_TESTS"))
      QSKIP(
          "Set NEVERD_AGENT_KEYRING_TESTS=1 with an unlocked native keyring.");
#endif
    QTemporaryDir directory;
    QString error;
    AgentController controller(directory.path());
    QVERIFY(controller.initialize(error));
    ModelProfile profile;
    profile.Model = "credential-test";
    QSignalSpy saved(&controller, &AgentController::modelSettingsSaved);
    QSignalSpy failed(&controller, &AgentController::operationError);
    QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(),
                                           "test-secret", false, ""));
    QVERIFY(controller.credentialBusy());
    QCOMPARE(saved.size(), 0);
    QTRY_VERIFY(!controller.credentialBusy());
    QCOMPARE(saved.size(), 1);
    QVERIFY(failed.isEmpty());
    QJsonObject config;
    QVERIFY(ProjectStore::readJson(directory.filePath("config.json"), config,
                                   error));
    const auto id = config["model"].toObject()["credentialId"].toString();
    const auto cleanup =
        qScopeGuard([&] { Credentials(directory.path()).remove(id); });
    QVERIFY(!id.isEmpty());
    AgentController reopened(directory.path());
    QVERIFY(reopened.initialize(error));
    QTRY_VERIFY(!reopened.credentialBusy());
    QVERIFY(reopened.credentialStatus().isEmpty());
    QCOMPARE(reopened.modelSettings()["hasKey"].toBool(), true);
    QVERIFY(QFile::rename(directory.filePath("config.json"),
                          directory.filePath("saved.json")));
    QVERIFY(QDir().mkpath(directory.filePath("config.json")));
    QVERIFY(controller.updateModelSettings(
        profile.toJson().toVariantMap(), "replacement-test-secret", false, ""));
    QTRY_VERIFY(!controller.credentialBusy());
    QCOMPARE(saved.size(), 1);
    QVERIFY(!failed.isEmpty());
    QString key;
    QVERIFY(Credentials(directory.path()).read(id, key, error));
    QCOMPARE(key, "test-secret");
#ifdef Q_OS_WIN
    QCOMPARE(QDir(directory.filePath("credentials"))
                 .entryList({"*.key"}, QDir::Files)
                 .size(),
             1);
#endif
  }
  void responsesPreserveOpaqueHistoryAndCallId() {
    ProviderCodec codec(ModelProtocol::Responses);
    QString text;
    const QJsonArray output{
        QJsonObject{{"type", "reasoning"}, {"encrypted_content", "opaque"}},
        QJsonObject{{"type", "function_call"},
                    {"id", "item-identity"},
                    {"call_id", "call-identity"},
                    {"name", "inspect"},
                    {"arguments", "{\"x\":2}"}}};
    QByteArray bytes = sseEvent({{"type", "response.output_text.delta"},
                                 {"delta", QStringLiteral("中文")}}) +
                       sseEvent({{"type", "response.completed"},
                                 {"response", response(output)}});
    for (char byte : bytes)
      QVERIFY(codec.feed(QByteArray(1, byte), [&](auto t) { text += t; }));
    const auto result = codec.finish(true, {});
    QVERIFY2(result.Error.isEmpty(), qPrintable(result.Error));
    QCOMPARE(text, QStringLiteral("中文"));
    QCOMPARE(result.Output, output);
    QCOMPARE(result.Calls[0].Id, "call-identity");
    QJsonArray history = result.Output;
    ProviderCodec::toolResults(ModelProtocol::Responses, history, result.Calls,
                               {QJsonObject{{"ok", true}}});
    QCOMPARE(history.last().toObject()["call_id"].toString(), "call-identity");
  }
  void chatThinkingAndArguments() {
    ProviderCodec codec(ModelProtocol::ChatCompletions);
    auto chunk = [](QJsonObject delta, QJsonValue stop = QJsonValue::Null) {
      return sseEvent(
          {{"choices", QJsonArray{QJsonObject{{"delta", delta},
                                              {"finish_reason", stop}}}}});
    };
    const QJsonObject firstCall{
        {"index", 0},
        {"id", "c1"},
        {"type", "function"},
        {"function",
         QJsonObject{{"name", "inspect"}, {"arguments", "{\"x\":"}}}};
    const QJsonObject secondCall{
        {"index", 0}, {"function", QJsonObject{{"arguments", "7}"}}}};
    QVERIFY(codec.feed(
        chunk({{"reasoning_content", "private provider continuation"},
               {"tool_calls", QJsonArray{firstCall}}}),
        [](auto) {}));
    QVERIFY(codec.feed(
        chunk({{"tool_calls", QJsonArray{secondCall}}}, "tool_calls") +
            "data: [DONE]\n\n",
        [](auto) {}));
    const auto turn = codec.finish(true, {});
    QVERIFY2(turn.Error.isEmpty(), qPrintable(turn.Error));
    QCOMPARE(turn.Calls[0].Arguments["x"].toInt(), 7);
    QVERIFY(turn.Output[0].toObject().contains("reasoning_content"));
  }
  void anthropicThinkingSignatureAndResults() {
    ProviderCodec codec(ModelProtocol::Messages);
    const QList<QJsonObject> events{
        {{"type", "content_block_start"},
         {"index", 0},
         {"content_block",
          QJsonObject{{"type", "thinking"}, {"thinking", ""}}}},
        {{"type", "content_block_delta"},
         {"index", 0},
         {"delta", QJsonObject{{"type", "thinking_delta"},
                               {"thinking", "opaque continuation"}}}},
        {{"type", "content_block_delta"},
         {"index", 0},
         {"delta",
          QJsonObject{{"type", "signature_delta"}, {"signature", "sig"}}}},
        {{"type", "content_block_start"},
         {"index", 1},
         {"content_block", QJsonObject{{"type", "tool_use"},
                                       {"id", "c2"},
                                       {"name", "inspect"},
                                       {"input", QJsonObject{}}}}},
        {{"type", "content_block_delta"},
         {"index", 1},
         {"delta", QJsonObject{{"type", "input_json_delta"},
                               {"partial_json", "{\"path\":\"one\"}"}}}},
        {{"type", "message_delta"},
         {"delta", QJsonObject{{"stop_reason", "tool_use"}}}},
        {{"type", "message_stop"}}};
    for (const auto &e : events)
      QVERIFY(codec.feed(sseEvent(e), [](auto) {}));
    auto turn = codec.finish(true, {});
    QVERIFY2(turn.Error.isEmpty(), qPrintable(turn.Error));
    QCOMPARE(turn.Output[0]
                 .toObject()["content"]
                 .toArray()[0]
                 .toObject()["signature"]
                 .toString(),
             "sig");
    ProviderCodec::toolResults(ModelProtocol::Messages, turn.Output, turn.Calls,
                               {QJsonObject{{"ok", true}}});
    QCOMPARE(turn.Output.last()
                 .toObject()["content"]
                 .toArray()[0]
                 .toObject()["tool_use_id"]
                 .toString(),
             "c2");
  }
  void incompleteStreamsNeverDispatch() {
    ProviderCodec codec(ModelProtocol::Responses);
    QVERIFY(codec.feed(sseEvent({{"type", "response.output_text.delta"},
                                 {"delta", "partial"}}),
                       [](auto) {}));
    QVERIFY(!codec.finish(true, {}).Error.isEmpty());
    ProviderCodec malformed(ModelProtocol::Responses);
    const auto body =
        QJsonDocument(response({QJsonObject{{"type", "function_call"},
                                            {"call_id", "id"},
                                            {"name", "inspect"},
                                            {"arguments", "{bad"}}}))
            .toJson();
    const auto result = malformed.finish(false, body);
    QVERIFY(!result.Error.isEmpty());
    QVERIFY(result.Calls.isEmpty());
    ProviderCodec truncated(ModelProtocol::Responses);
    QVERIFY(!truncated.feed(
        sseEvent(
            {{"type", "response.incomplete"},
             {"response",
              QJsonObject{{"status", "incomplete"},
                          {"output", QJsonArray{}},
                          {"incomplete_details",
                           QJsonObject{{"reason", "max_output_tokens"}}},
                          {"usage", QJsonObject{{"output_tokens", 8192}}}}}}),
        [](auto) {}));
    const auto partial = truncated.finish(true, {});
    QVERIFY(partial.Error.contains("max_output_tokens"));
    QCOMPARE(partial.Usage["output_tokens"].toInt(), 8192);
    QVERIFY(partial.Calls.isEmpty());
  }
  void reasoningControlsAreProtocolSpecific() {
    ModelProfile profile;
    profile.Model = "test";
    profile.ReasoningEffort = "low";
    const auto responses = ProviderCodec::request(profile, "", {}, {});
    QCOMPARE(responses["reasoning"].toObject()["effort"].toString(), "low");
    profile.Protocol = ModelProtocol::Messages;
    const auto messages = ProviderCodec::request(profile, "", {}, {});
    QCOMPARE(messages["output_config"].toObject()["effort"].toString(), "low");
    profile.Protocol = ModelProtocol::ChatCompletions;
    const auto chat = ProviderCodec::request(profile, "", {}, {});
    QCOMPARE(chat["reasoning_effort"].toString(), "low");
  }
  void realHttpNphLoopAndPersistence() {
    Server server;
    QTemporaryDir directory;
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    auto budget = std::make_shared<RunBudget>();
    AgentLoop loop("project", "session", profile, "", budget);
    QString error;
    int calls = 0;
    QVERIFY(loop.tools().add(
        "inspect", "Inspect input", {NphTools::stringParameter("path", "Path")},
        [&](auto call) {
          ++calls;
          NphTools::succeed(
              call, {{"result", QString::fromStdString(
                                    call->request()
                                        .Parameters["path"]
                                        .template get<std::string>())}});
        },
        error));
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "test", directory.filePath("session.json"));
    QTRY_COMPARE(server.Requests.size(), 1);
    QVERIFY(!server.Requests[0]["tools"].toArray().isEmpty());
    server.respond(
        0, response({QJsonObject{{"type", "function_call"},
                                 {"call_id", "call"},
                                 {"name", "inspect"},
                                 {"arguments", "{\"path\":\"file\"}"}}}));
    QTRY_COMPARE(server.Requests.size(), 2);
    QCOMPARE(calls, 1);
    QVERIFY(server.Requests[1]["input"]
                .toArray()
                .last()
                .toObject()["output"]
                .toString()
                .contains("file"));
    server.respond(1, response(answer("verified")));
    QTRY_COMPARE(done.size(), 1);
    QCOMPARE(done[0][0].toString(), "verified");
    QCOMPARE(done[0][1].toString(), "");
    QJsonObject stored;
    QVERIFY(ProjectStore::readJson(directory.filePath("session.json"), stored,
                                   error));
    QCOMPARE(stored["groups"].toArray().size(), 1);
    QCOMPARE(budget->Active, 0);
  }
  void crossProjectStreamingDoesNotOverwriteDraft() {
    Server server;
    QTemporaryDir directory;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("A", directory.filePath("a")));
    const auto first = controller.activeId();
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    QVERIFY(controller.updateModelSettings(profile.toJson().toVariantMap(), "",
                                           false, ""));
    controller.setDraft("question");
    QVERIFY(controller.sendRequest(""));
    QTRY_COMPARE(server.Requests.size(), 1);
    QVERIFY(controller.createProject("B", directory.filePath("b")));
    controller.setDraft("B draft");
    server.respond(0, response(answer("A answer")));
    QSignalSpy finished(&controller, &AgentController::runFinished);
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(controller.draft(), "B draft");
    QVERIFY(controller.messages().isEmpty());
    QVERIFY(controller.selectProject(first));
    QCOMPARE(controller.messages().last().toMap()["text"].toString(),
             "A answer");
  }
  void cancellationCompletesWithoutRetry() {
    Server server;
    QTemporaryDir directory;
    const auto context = directory.filePath("context.json");
    ModelProfile profile;
    profile.Model = "mock";
    profile.BaseUrl = server.url();
    auto budget = std::make_shared<RunBudget>();
    AgentLoop loop("p", "s", profile, "", budget);
    QSignalSpy done(&loop, &AgentLoop::finished);
    loop.start("system", "question", context);
    QTRY_COMPARE(server.Requests.size(), 1);
    loop.cancel();
    QTRY_COMPARE(done.size(), 1);
    QVERIFY(done[0][2].toBool());
    QCOMPARE(server.Requests.size(), 1);
    QCOMPARE(budget->Active, 0);
    QJsonObject saved;
    QString error;
    QVERIFY(ProjectStore::readJson(context, saved, error));
    const auto items =
        saved["groups"].toArray().last().toObject()["items"].toArray();
    QCOMPARE(items.size(), 2);
    QCOMPARE(items.first().toObject()["content"].toString(), "question");
    QVERIFY(items.last().toObject()["content"].toString().contains(
        "Unfinished task"));
    AgentLoop resumed("p", "resumed", profile, "",
                      std::make_shared<RunBudget>());
    QSignalSpy resumedDone(&resumed, &AgentLoop::finished);
    resumed.start("system", "continue", context);
    QTRY_COMPARE(server.Requests.size(), 2);
    QCOMPARE(server.Requests[1]["input"]
                 .toArray()
                 .first()
                 .toObject()["content"]
                 .toString(),
             "question");
    server.respond(1, response(answer("continued")));
    QTRY_COMPARE(resumedDone.size(), 1);
    QVERIFY(resumedDone[0][1].toString().isEmpty());
  }
};
QTEST_GUILESS_MAIN(ModelTests)
#include "ModelTests.moc"
