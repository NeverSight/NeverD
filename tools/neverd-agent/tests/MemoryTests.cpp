//===- MemoryTests.cpp - Durable memory lifecycle contracts
//----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "app/AgentController.h"
#include "project/MemoryStore.h"
#include "runtime/MemoryLearner.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

using namespace neverd::agent;
namespace {
QString uid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
QJsonObject sourceFor(const Project &project, bool object = false) {
  QJsonArray evidence;
  if (object)
    evidence.append(QJsonObject{{"id", uid()},
                                {"objectId", project.Objects[0].Id},
                                {"runId", uid()},
                                {"sha256", project.Objects[0].Sha256},
                                {"tool", "decompile"},
                                {"address", "0x140001000"},
                                {"revision", "7"},
                                {"complete", true},
                                {"successful", true}});
  return {{"version", 1},
          {"projectId", project.Id},
          {"objectId", object ? project.Objects[0].Id : ""},
          {"sha256", object ? project.Objects[0].Sha256 : ""},
          {"runId", uid()},
          {"agentId", uid()},
          {"sessionId", ""},
          {"createdAt",
           QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
          {"requestOrigin", object ? "delegated" : "user"},
          {"request", "以后先看导出函数。不要设置模型调用次数上限。"},
          {"answer", "GetMessageW 位于 0x140001000，调用 NtUserGetMessage。"},
          {"evidence", evidence}};
}
QJsonObject preference(const QString &text = "先检查导出函数。") {
  return {{"key", "analysis-entry"},     {"title", "优先检查导出函数"},
          {"category", "preference"},    {"text", text},
          {"basis", "request"},          {"quote", "以后先看导出函数。"},
          {"evidence_ids", QJsonArray{}}};
}
QJsonObject finding(const QJsonObject &source) {
  return {{"key", "GetMessageW-call"},
          {"title", "GetMessageW 调用关系"},
          {"category", "finding"},
          {"text", source["answer"]},
          {"basis", "answer"},
          {"quote", source["answer"]},
          {"evidence_ids",
           QJsonArray{source["evidence"].toArray()[0].toObject()["id"]}}};
}
QJsonObject response(const QString &text) {
  return {
      {"status", "completed"},
      {"output", QJsonArray{QJsonObject{
                     {"type", "message"},
                     {"role", "assistant"},
                     {"content", QJsonArray{QJsonObject{{"type", "output_text"},
                                                        {"text", text}}}}}}},
      {"usage", QJsonObject{{"input_tokens", 12}, {"output_tokens", 7}}}};
}
struct Workspace {
  QTemporaryDir Directory;
  Project Value;
  QString Error;
  Workspace() {
    const auto project =
        ProjectStore::create(Directory.filePath("project"), "research", Error);
    if (!project)
      return;
    Value = *project;
    QFile file(Directory.filePath("object.dll"));
    if (!file.open(QIODevice::WriteOnly)) {
      Error = file.errorString();
      return;
    }
    file.write("unique test binary bytes");
    file.close();
    ProjectStore::addObject(Value, ProjectStore::fingerprint(file.fileName()),
                            Error);
  }
};
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
  ModelProfile profile(ModelProtocol protocol = ModelProtocol::Responses) {
    ModelProfile profile;
    profile.Model = "fixture";
    profile.Protocol = protocol;
    profile.BaseUrl = QString("http://127.0.0.1:%1/v1").arg(Tcp.serverPort());
    return profile;
  }
  void respond(int index, const QJsonObject &body) {
    const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact);
    Sockets[index]->write(
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Connection: close\r\nContent-Length: " +
        QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
    Sockets[index]->disconnectFromHost();
  }
};
} // namespace
class MemoryTests : public QObject {
  Q_OBJECT
private slots:
  void malformedExistingMemoryIsNotReset() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    QString error;
    const auto path = QDir(ProjectStore::stateDirectory(workspace.Value))
                          .filePath("router/memory.json");
    QVERIFY(ProjectStore::writeJson(path, {}, error));
    QJsonArray records;
    QVERIFY(!MemoryStore::read(workspace.Value, {}, records, error));
    QVERIFY(
        !MemoryStore::append(workspace.Value, {}, "Do not overwrite", error));
    QJsonObject unchanged;
    QVERIFY(ProjectStore::readJson(path, unchanged, error));
    QVERIFY(unchanged.isEmpty());
  }
  void projectAndObjectSurviveRestartWithoutMixing() {
    Workspace workspace;
    QVERIFY2(workspace.Error.isEmpty(), qPrintable(workspace.Error));
    auto &project = workspace.Value;
    QString error;
    QVERIFY(MemoryStore::append(project, {}, "项目按 API 链路研究。", error));
    QVERIFY(ProjectStore::appendMemory(project, project.Objects[0],
                                       "Object-only note", error));
    const auto reopened = ProjectStore::open(project.Directory, error);
    QVERIFY(reopened);
    QJsonArray records;
    QVERIFY(MemoryStore::read(*reopened, {}, records, error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].toObject()["text"], "项目按 API 链路研究。");
    QVERIFY(
        MemoryStore::read(*reopened, project.Objects[0].Id, records, error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].toObject()["text"], "Object-only note");
    QVERIFY(!MemoryStore::read(*reopened, uid(), records, error));
  }
  void extractionIsAtomicAndRequiresRealQuotesAndEvidence() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    QString error;
    int added;
    auto source = sourceFor(workspace.Value, true);
    auto invalid = finding(source);
    invalid["evidence_ids"] = QJsonArray{uid()};
    QVERIFY(!MemoryStore::consolidate(
        workspace.Value, source, {finding(source), invalid}, added, error));
    QJsonArray records;
    QVERIFY(MemoryStore::read(workspace.Value, workspace.Value.Objects[0].Id,
                              records, error));
    QVERIFY(records.isEmpty());
    invalid = finding(source);
    invalid["quote"] = "Invented source text";
    QVERIFY(!MemoryStore::consolidate(workspace.Value, source, {invalid}, added,
                                      error));
    error.clear();
    QVERIFY(MemoryStore::consolidate(workspace.Value, source, {finding(source)},
                                     added, error));
    QCOMPARE(added, 1);
    source["agentId"] = uid();
    source["evidence"] = QJsonArray{};
    invalid["quote"] = source["answer"];
    invalid["evidence_ids"] = QJsonArray{};
    QVERIFY(!MemoryStore::consolidate(workspace.Value, source, {invalid}, added,
                                      error));
    source["sha256"] = QString(64, '0');
    QVERIFY(
        !MemoryStore::consolidate(workspace.Value, source, {}, added, error));
  }
  void conflictRequiresConfirmationAndForgetPreventsResurrection() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    auto source = sourceFor(workspace.Value);
    QString error;
    int added;
    QVERIFY(MemoryStore::consolidate(workspace.Value, source, {preference()},
                                     added, error));
    QVERIFY(MemoryStore::consolidate(workspace.Value, source, {preference()},
                                     added, error));
    QCOMPARE(added, 0);
    source["agentId"] = uid();
    QVERIFY(MemoryStore::consolidate(workspace.Value, source,
                                     {preference("导出函数只作为次要入口。")},
                                     added, error));
    QJsonArray records;
    QVERIFY(MemoryStore::read(workspace.Value, {}, records, error));
    QVERIFY(records.isEmpty());
    QVERIFY(MemoryStore::read(workspace.Value, {}, records, error, true));
    QCOMPARE(records.size(), 2);
    for (const auto &record : records)
      QCOMPARE(record.toObject()["state"], "needs_review");
    const auto id = records[0].toObject()["id"].toString();
    QVERIFY(
        MemoryStore::edit(workspace.Value, {}, id, "先检查导出函数。", error));
    QVERIFY(MemoryStore::read(workspace.Value, {}, records, error));
    QCOMPARE(records.size(), 1);
    QVERIFY(MemoryStore::forget(workspace.Value, {}, id, error));
    source["agentId"] = uid();
    QVERIFY(MemoryStore::consolidate(workspace.Value, source, {preference()},
                                     added, error));
    QCOMPARE(added, 0);
    QVERIFY(MemoryStore::read(workspace.Value, {}, records, error));
    QVERIFY(records.isEmpty());
  }
  void automaticRecordsRemainPortableAndBoundToExactBytes() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    QString error;
    const auto source = sourceFor(workspace.Value, true);
    int added;
    QVERIFY(MemoryStore::consolidate(workspace.Value, source, {finding(source)},
                                     added, error));
    const auto path = workspace.Directory.filePath("export.ndmemory.json");
    QVERIFY(ProjectStore::exportMemory(
        workspace.Value, workspace.Value.Objects[0], path, error));
    auto second = ProjectStore::create(workspace.Directory.filePath("second"),
                                       "second", error);
    QVERIFY(second);
    QVERIFY(ProjectStore::addObject(
        *second, ProjectStore::fingerprint(workspace.Value.Objects[0].Path),
        error));
    QVERIFY(
        ProjectStore::importMemory(*second, second->Objects[0], path, error));
    QVERIFY(
        ProjectStore::importMemory(*second, second->Objects[0], path, error));
    QJsonArray records;
    QVERIFY(MemoryStore::read(*second, second->Objects[0].Id, records, error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].toObject()["source"].toObject()["quote"],
             source["answer"]);
    second->Objects[0].Sha256 = QString(64, 'f');
    QVERIFY(
        !ProjectStore::importMemory(*second, second->Objects[0], path, error));
    QFile exported(path);
    QVERIFY(exported.open(QIODevice::ReadOnly));
    QVERIFY(!exported.readAll().contains(workspace.Directory.path().toUtf8()));
  }
  void relevanceIndexIsBoundedAndProjectIsolationIsExplicit() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    QString error;
    const auto id = workspace.Value.Objects[0].Id;
    QVERIFY(MemoryStore::append(workspace.Value, id,
                                "GetMessageW 调用 NtUserGetMessage。", error));
    QVERIFY(MemoryStore::append(workspace.Value, id,
                                "Unrelated graphics render pipeline", error));
    QVERIFY(MemoryStore::append(workspace.Value, {},
                                "研究消息机制时保留调用证据。", error));
    QJsonArray records;
    QVERIFY(MemoryStore::collect(workspace.Value, id, {}, records, error));
    const auto index =
        MemoryStore::recall(records, "GetMessageW 的调用关系", 1800);
    QVERIFY(index.contains("GetMessageW"));
    QVERIFY(!index.contains("graphics"));
    QVERIFY(index.size() < 2200);
    auto foreign = records[0].toObject();
    foreign["id"] = uid();
    foreign["sha256"] = QString(64, '0');
    foreign["text"] = "Different binary note";
    QVERIFY(
        MemoryStore::collect(workspace.Value, id, {foreign}, records, error));
    QVERIFY(!QJsonDocument(records).toJson().contains("Different binary note"));
  }
  void sourcesRedactSecretsAndResumeAfterCancellation() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    Server server;
    auto source = sourceFor(workspace.Value);
    source["request"] = source["request"].toString() +
                        " custom-provider-secret sk-testabcdefghijklmnop";
    QString error;
    QVERIFY(MemoryLearner::enqueue(workspace.Value, source,
                                   "custom-provider-secret", error));
    auto pending = MemoryLearner::pending(workspace.Value, error);
    QVERIFY(!pending.isEmpty());
    QVERIFY(
        !QJsonDocument(pending).toJson().contains("custom-provider-secret"));
    QVERIFY(!QJsonDocument(pending).toJson().contains("sk-test"));
    MemoryLearner learner;
    QSignalSpy finished(&learner, &MemoryLearner::finished);
    QVERIFY(
        learner.start(workspace.Value, pending, server.profile(), {}, error));
    QTRY_COMPARE(server.Requests.size(), 1);
    learner.cancel();
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!MemoryLearner::pending(workspace.Value, error).isEmpty());
    QVERIFY(
        learner.start(workspace.Value, pending, server.profile(), {}, error));
    QTRY_COMPARE(server.Requests.size(), 2);
    server.respond(1, response("{\"memories\":[]}"));
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY2(finished.last()[2].toString().isEmpty(),
             qPrintable(finished.last()[2].toString()));
    QVERIFY(MemoryLearner::pending(workspace.Value, error).isEmpty());
  }
  void projectFindingsKeepTheirObjectAndVersionBindings() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    QString error;
    auto source = sourceFor(workspace.Value, true);
    source["objectId"] = "";
    source["sha256"] = "";
    source["requestOrigin"] = "user";
    int added;
    QVERIFY(MemoryStore::consolidate(workspace.Value, source, {finding(source)},
                                     added, error));
    QFile other(workspace.Directory.filePath("other.dll"));
    QVERIFY(other.open(QIODevice::WriteOnly));
    other.write("a different object");
    other.close();
    QVERIFY(ProjectStore::addObject(
        workspace.Value, ProjectStore::fingerprint(other.fileName()), error));
    QJsonArray records;
    QVERIFY(MemoryStore::collect(workspace.Value, workspace.Value.Objects[0].Id,
                                 {}, records, error));
    QCOMPARE(records.size(), 1);
    QVERIFY(MemoryStore::collect(workspace.Value, workspace.Value.Objects[1].Id,
                                 {}, records, error));
    QVERIFY(records.isEmpty());
    auto &first = workspace.Value.Objects[0];
    first.Sha256 = QString(64, 'a');
    QJsonObject memory;
    const auto path =
        QDir(ProjectStore::objectDirectory(workspace.Value, first))
            .filePath("memory.json");
    QVERIFY(ProjectStore::readJson(path, memory, error));
    memory["sha256"] = first.Sha256;
    QVERIFY(ProjectStore::writeJson(path, memory, error));
    QVERIFY(MemoryStore::collect(workspace.Value, {}, {}, records, error));
    QVERIFY(records.isEmpty());
  }
  void delegatedTasksCannotCreateUserPreferences() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    const auto source = sourceFor(workspace.Value, true);
    QString error;
    int added;
    QVERIFY(!MemoryStore::consolidate(workspace.Value, source, {preference()},
                                      added, error));
    QJsonArray records;
    QVERIFY(MemoryStore::read(workspace.Value, workspace.Value.Objects[0].Id,
                              records, error));
    QVERIFY(records.isEmpty());
  }
  void rejectedExtractionNeedsExplicitRetryAndCompletedSourceIsIdempotent() {
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    Server server;
    MemoryLearner learner;
    auto source = sourceFor(workspace.Value);
    QString error;
    QVERIFY(MemoryLearner::enqueue(workspace.Value, source, {}, error));
    source = MemoryLearner::pending(workspace.Value, error);
    QSignalSpy finished(&learner, &MemoryLearner::finished);
    QVERIFY(
        learner.start(workspace.Value, source, server.profile(), {}, error));
    QTRY_COMPARE(server.Requests.size(), 1);
    server.respond(0, response("{\"memories\":false}"));
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY(!finished.last()[2].toString().isEmpty());
    QVERIFY(MemoryLearner::pending(workspace.Value, error).isEmpty());
    QVERIFY(MemoryLearner::retryFailed(workspace.Value, error));
    source = MemoryLearner::pending(workspace.Value, error);
    QVERIFY(!source.isEmpty());
    QVERIFY(
        learner.start(workspace.Value, source, server.profile(), {}, error));
    QTRY_COMPARE(server.Requests.size(), 2);
    server.respond(1, response("{\"memories\":[]}"));
    QTRY_COMPARE(finished.size(), 2);
    QVERIFY(finished.last()[2].toString().isEmpty());
    // The record commit can precede the separate source-status write.
    // Reopening that pending packet must not issue another model request.
    source["status"] = "pending";
    const auto path = QDir(MemoryStore::sourceDirectory(workspace.Value))
                          .filePath(source["agentId"].toString() + ".json");
    QVERIFY(ProjectStore::writeJson(path, source, error));
    QVERIFY(MemoryLearner::pending(workspace.Value, error).isEmpty());
    QCOMPARE(server.Requests.size(), 2);
  }
  void extractionUsesEveryProvider_data() {
    QTest::addColumn<int>("protocol");
    QTest::newRow("responses") << int(ModelProtocol::Responses);
    QTest::newRow("messages") << int(ModelProtocol::Messages);
    QTest::newRow("chat-completions") << int(ModelProtocol::ChatCompletions);
  }
  void extractionUsesEveryProvider() {
    QFETCH(int, protocol);
    Workspace workspace;
    QVERIFY(workspace.Error.isEmpty());
    Server server;
    MemoryLearner learner;
    QString error;
    auto source = sourceFor(workspace.Value);
    QVERIFY(MemoryLearner::enqueue(workspace.Value, source, {}, error));
    source = MemoryLearner::pending(workspace.Value, error);
    QSignalSpy finished(&learner, &MemoryLearner::finished);
    QVERIFY(learner.start(workspace.Value, source,
                          server.profile(ModelProtocol(protocol)), {}, error));
    QTRY_COMPARE(server.Requests.size(), 1);
    const QJsonObject arguments{{"memories", QJsonArray{preference()}}};
    const auto encoded = QString::fromUtf8(
        QJsonDocument(arguments).toJson(QJsonDocument::Compact));
    QJsonObject body;
    if (ModelProtocol(protocol) == ModelProtocol::Responses)
      body = {{"status", "completed"},
              {"output", QJsonArray{QJsonObject{{"type", "function_call"},
                                                {"call_id", "extract"},
                                                {"name", "record_memories"},
                                                {"arguments", encoded}}}}};
    else if (ModelProtocol(protocol) == ModelProtocol::Messages)
      body = {{"id", "extract"},
              {"role", "assistant"},
              {"type", "message"},
              {"content", QJsonArray{QJsonObject{{"type", "tool_use"},
                                                 {"id", "extract"},
                                                 {"name", "record_memories"},
                                                 {"input", arguments}}}},
              {"stop_reason", "tool_use"}};
    else
      body = {
          {"choices",
           QJsonArray{QJsonObject{
               {"finish_reason", "tool_calls"},
               {"message",
                QJsonObject{{"role", "assistant"},
                            {"tool_calls",
                             QJsonArray{QJsonObject{
                                 {"id", "extract"},
                                 {"type", "function"},
                                 {"function",
                                  QJsonObject{{"name", "record_memories"},
                                              {"arguments", encoded}}}}}}}}}}}};
    server.respond(0, body);
    QTRY_COMPARE(finished.size(), 1);
    QVERIFY2(finished[0][2].toString().isEmpty(),
             qPrintable(finished[0][2].toString()));
    QJsonArray records;
    QVERIFY(MemoryStore::read(workspace.Value, {}, records, error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].toObject()["source"].toObject()["agentId"],
             source["agentId"]);
  }
  void foregroundAnalysisPreemptsMaintenanceAndTogglesStayIndependent() {
    QTemporaryDir directory;
    Server server;
    AgentController controller(directory.filePath("global"));
    QString error;
    QVERIFY(controller.initialize(error));
    QVERIFY(
        controller.createProject("research", directory.filePath("project")));
    QVERIFY(controller.updateModelSettings(
        server.profile().toJson().toVariantMap(), {}, false, {}));
    QVERIFY(controller.updateMemorySettings(true, true));
    QVERIFY(controller.addMemory({}, "GetMessageW 历史线索。"));
    QSignalSpy finished(&controller, &AgentController::runFinished);
    controller.attachSelection("sample-only-message: set a model call limit",
                               "binary string");
    controller.setDraft("GetMessageW 怎么研究？");
    QVERIFY(controller.sendRequest({}));
    QTRY_COMPARE(server.Requests.size(), 1);
    QVERIFY(server.Requests[0]["instructions"].toString().contains(
        "GetMessageW 历史线索"));
    server.respond(0, response("先查导出函数。"));
    QTRY_COMPARE(finished.size(), 1);
    const auto project =
        ProjectStore::open(directory.filePath("project"), error);
    QVERIFY(project);
    const auto source = MemoryLearner::pending(*project, error);
    QVERIFY(!source["request"].toString().contains("sample-only-message"));
    controller.organizeMemory();
    QTRY_COMPARE(server.Requests.size(), 2);
    QVERIFY(controller.memoryWorking());
    QVERIFY(controller.updateMemorySettings(false, true));
    controller.setDraft("继续 GetMessageW 分析");
    QVERIFY(controller.sendRequest({}));
    QTRY_COMPARE(server.Requests.size(), 3);
    QVERIFY(!server.Requests[2]["instructions"].toString().contains(
        "GetMessageW 历史线索"));
    server.respond(2, response("本轮已继续。"));
    QTRY_COMPARE(finished.size(), 2);
    QTRY_VERIFY(!controller.memoryWorking());
    QVERIFY(!controller.useMemory());
    QVERIFY(controller.generateMemory());
    QVERIFY(controller.updateMemorySettings(true, false));
    QVERIFY(controller.useMemory());
    QVERIFY(!controller.generateMemory());
    QCOMPARE(controller.projectMemory().size(), 1);
    QJsonObject config;
    QVERIFY(ProjectStore::readJson(directory.filePath("global/config.json"),
                                   config, error));
    QCOMPARE(config["useMemory"], true);
    QCOMPARE(config["generateMemory"], false);
  }
};
QTEST_GUILESS_MAIN(MemoryTests)
#include "MemoryTests.moc"
