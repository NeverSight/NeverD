#include "GuiSessionBroker.h"
#include "McpConnectionManager.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>
#include <QtEndian>

namespace {
void sendFrame(QLocalSocket &socket, QJsonObject object) {
  const auto body = QJsonDocument(object).toJson(QJsonDocument::Compact);
  QByteArray frame(4, '\0');
  qToBigEndian(quint32(body.size()), reinterpret_cast<uchar *>(frame.data()));
  socket.write(frame + body);
}
QJsonObject readFrame(QLocalSocket &socket) {
  const auto data = socket.readAll();
  if (data.size() < 4)
    return {};
  return QJsonDocument::fromJson(data.mid(4)).object();
}
} // namespace

class McpTests final : public QObject {
  Q_OBJECT
private slots:
  void brokerIsExplicitAndRoutesSameSession() {
    GuiSessionBroker broker;
    QVERIFY(!broker.enabled());
    broker.setSelection({{"project_id", "project-1"},
                         {"revision", "71"},
                         {"address", "0xffffffffffffffff"}});
    QVERIFY(broker.start());
    QFile credentialFile(broker.credentialFile());
    QVERIFY(credentialFile.open(QIODevice::ReadOnly));
    const auto credentials =
        QJsonDocument::fromJson(credentialFile.readAll()).object();
    credentialFile.close();
    const auto fileName = broker.credentialFile();
    QLocalSocket socket;
    socket.connectToServer(credentials.value("endpoint").toString());
    QVERIFY(socket.waitForConnected(1000));
    sendFrame(socket, {{"protocol_major", 1},
                       {"request_id", "auth"},
                       {"operation", "authenticate"},
                       {"payload",
                        QJsonObject{{"token", credentials.value("token")}}}});
    QTRY_VERIFY(socket.bytesAvailable() > 4);
    QCOMPARE(readFrame(socket).value("type").toString(), "hello");
    sendFrame(socket, {{"protocol_major", 1},
                       {"request_id", "blocked"},
                       {"operation", "open"},
                       {"payload", QJsonObject{{"path", "/another/input"}}}});
    QTRY_VERIFY(socket.bytesAvailable() > 4);
    QCOMPARE(readFrame(socket).value("status").toString(), "error");
    QSignalSpy query(&broker, &GuiSessionBroker::queryRequested);
    sendFrame(socket,
              {{"protocol_major", 1},
               {"request_id", "caller-id"},
               {"operation", "functions"},
               {"expected_revision", "71"},
               {"payload", QJsonObject{{"offset", 20}, {"limit", 10}}}});
    QTRY_COMPARE(query.size(), 1);
    const auto arguments = query.first();
    QCOMPARE(arguments[1].toString(), "functions");
    QCOMPARE(arguments[3].toString(), "71");
    broker.reply(arguments[0].toString(),
                 {{"type", "response"},
                  {"request_id", "internal-id"},
                  {"status", "ok"},
                  {"revision", "71"},
                  {"payload", QJsonObject{{"items", QJsonArray{}}}}});
    QTRY_VERIFY(socket.bytesAvailable() > 4);
    const auto response = readFrame(socket);
    QCOMPARE(response.value("request_id").toString(), "caller-id");
    QCOMPARE(response.value("revision").toString(), "71");
    sendFrame(socket, {{"protocol_major", 1},
                       {"request_id", "selection"},
                       {"operation", "selection"},
                       {"expected_revision", "70"},
                       {"payload", QJsonObject{}}});
    QTRY_VERIFY(socket.bytesAvailable() > 4);
    QCOMPARE(
        readFrame(socket).value("error").toObject().value("code").toString(),
        "stale_revision");
    broker.stop();
    QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
    QVERIFY(!QFile::exists(fileName));
  }

  void brokerRejectsBadCredentials() {
    GuiSessionBroker broker;
    QVERIFY(broker.start());
    QFile file(broker.credentialFile());
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto credentials = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    QLocalSocket socket;
    socket.connectToServer(credentials.value("endpoint").toString());
    QVERIFY(socket.waitForConnected(1000));
    sendFrame(socket, {{"protocol_major", 1},
                       {"request_id", "auth"},
                       {"operation", "authenticate"},
                       {"payload", QJsonObject{{"token", "incorrect"}}}});
    QTRY_COMPARE(socket.state(), QLocalSocket::UnconnectedState);
  }

  void brokerRevokesCredentialsWhenProjectChanges() {
    GuiSessionBroker broker;
    broker.setSelection({{"project_id", "first-project"}, {"revision", "1"}});
    QVERIFY(broker.start());
    const auto credentials = broker.credentialFile();
    broker.setSelection({{"project_id", "first-project"}, {"revision", "2"}});
    QVERIFY(broker.enabled());
    broker.setSelection({{"project_id", "second-project"}, {"revision", "3"}});
    QVERIFY(!broker.enabled());
    QVERIFY(!QFile::exists(credentials));
  }

  void stdioHandshakeAndExplicitToolCall() {
    McpConnectionManager manager;
    QVERIFY(!manager.connected());
    manager.connectStdio(TEST_PYTHON, {TEST_SERVER});
    QTRY_VERIFY_WITH_TIMEOUT(manager.connected(), 5000);
    QTRY_COMPARE(manager.tools().size(), 1);
    QTRY_COMPARE(manager.resources().size(), 1);
    QVERIFY(manager.lastResult().isEmpty());
    QVERIFY(manager.callHistory().isEmpty());
    manager.callTool("echo", R"({"address":"0xffffffffffffffff"})");
    QTRY_VERIFY(manager.lastResult().contains("0xffffffffffffffff"));
    QCOMPARE(manager.callHistory().first().toMap().value("status").toString(),
             "completed");
    manager.readResource("fixture://data");
    QTRY_VERIFY(manager.lastResult().contains("fixture resource"));
    manager.disconnectServer();
    QVERIFY(!manager.connected());
  }

  void callCancellationAndBoundedHistory() {
    McpConnectionManager manager;
    manager.connectStdio(TEST_PYTHON, {TEST_SERVER});
    QTRY_VERIFY(manager.connected());
    manager.callTool("hold", "{}");
    QCOMPARE(manager.callHistory().size(), 1);
    const auto heldId =
        manager.callHistory().first().toMap().value("id").toString();
    for (int index = 0; index < 55; ++index) {
      manager.callTool("echo", QString("{\"index\":%1}").arg(index));
      QTRY_COMPARE(
          manager.callHistory().first().toMap().value("status").toString(),
          "completed");
    }
    QCOMPARE(manager.callHistory().size(), 50);
    bool retainedPending = false;
    for (const auto &entry : manager.callHistory())
      retainedPending |= entry.toMap().value("id").toString() == heldId;
    QVERIFY(retainedPending);
    manager.cancelCall(heldId);
    manager.inspectCall(heldId);
    QVERIFY(manager.lastResult().contains("cancelled"));
    QTest::qWait(
        50); // Fixture intentionally sends a late response after cancellation.
    QVERIFY(!manager.lastResult().contains("late response"));
  }

  void malformedAndOversizedServersFailClosed() {
    for (const auto &mode : {QString("malformed"), QString("oversized")}) {
      McpConnectionManager manager;
      manager.connectStdio(TEST_PYTHON, {TEST_SERVER, mode});
      QTRY_VERIFY(manager.status().contains(
          mode == "malformed" ? "Invalid MCP" : "exceeds 8 MiB"));
      QVERIFY(!manager.connected());
      QVERIFY(manager.tools().isEmpty());
      QVERIFY(manager.callHistory().isEmpty());
    }
  }

  void rejectedHttpSettingsCanDisconnectAndSwitchToStdio() {
    McpConnectionManager manager;
    manager.disconnectServer();
    manager.connectHttp("http://example.invalid/mcp", "");
    QVERIFY(manager.status().contains("requires HTTPS"));
    manager.connectHttp("https://localhost/mcp", "invalid\r\ntoken");
    QVERIFY(manager.status().contains("Invalid authentication token"));
    manager.connectHttp("https://localhost/mcp", "",
                        ":/neverd-test-missing-certificate.pem");
    QVERIFY(manager.status().contains("Cannot load CA certificate"));
    QVERIFY(!manager.connected());
    manager.disconnectServer();
    manager.disconnectServer();
    manager.connectStdio(TEST_PYTHON, {TEST_SERVER});
    QTRY_VERIFY(manager.connected());
    manager.callTool("echo", R"({"transport":"stdio-after-rejection"})");
    QTRY_VERIFY(manager.lastResult().contains("stdio-after-rejection"));
  }

  void httpSessionHeadersAndSse() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    bool sawSession = false;
    bool sawAuth = false;
    int initializations = 0;
    connect(&server, &QTcpServer::newConnection, this, [&] {
      auto *socket = server.nextPendingConnection();
      auto buffer = QSharedPointer<QByteArray>::create();
      connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
        *buffer += socket->readAll();
        const auto end = buffer->indexOf("\r\n\r\n");
        if (end < 0)
          return;
        const auto headers = buffer->left(end);
        int length = 0;
        for (auto line : headers.split('\n'))
          if (line.trimmed().toLower().startsWith("content-length:"))
            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        if (buffer->size() < end + 4 + length)
          return;
        const auto request =
            QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
        sawAuth |=
            headers.toLower().contains("authorization: bearer fixture-token");
        sawSession |=
            headers.toLower().contains("mcp-session-id: fixture-session") &&
            headers.toLower().contains("mcp-protocol-version: 2025-11-25");
        const auto method = request.value("method").toString();
        QJsonObject result;
        if (method == "initialize") {
          ++initializations;
          result = {
              {"protocolVersion", "2025-11-25"},
              {"capabilities", QJsonObject{{"tools", QJsonObject{}},
                                           {"resources", QJsonObject{}}}}};
        } else if (method == "tools/list")
          result = {{"tools", QJsonArray{QJsonObject{{"name", "sse-tool"}}}}};
        else if (method == "resources/list")
          result = {{"resources", QJsonArray{}}};
        const auto body = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                                    {"id", request.value("id")},
                                                    {"result", result}})
                              .toJson(QJsonDocument::Compact);
        if (method == "tools/list") {
          const auto event = "id: event-1\r\ndata: " + body + "\r\n\r\n";
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                        "text/event-stream\r\nConnection: close\r\n\r\n" +
                        event.left(12));
          QTimer::singleShot(10, socket, [socket, event] {
            socket->write(event.mid(12));
            socket->disconnectFromHost();
          });
        } else if (!request.contains("id")) {
          socket->write("HTTP/1.1 202 Accepted\r\nContent-Length: "
                        "0\r\nConnection: close\r\n\r\n");
          socket->disconnectFromHost();
        } else {
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                        "application/json\r\nMCP-Session-Id: "
                        "fixture-session\r\nContent-Length: " +
                        QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body);
          socket->disconnectFromHost();
        }
        buffer->clear();
      });
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    McpConnectionManager manager;
    manager.connectHttp(
        QString("http://127.0.0.1:%1/mcp").arg(server.serverPort()),
        "fixture-token");
    QTRY_VERIFY(manager.connected());
    QTRY_COMPARE(manager.tools().size(), 1);
    QCOMPARE(manager.tools()[0].toMap().value("name").toString(), "sse-tool");
    QVERIFY(sawSession);
    QVERIFY(sawAuth);
    manager.disconnectServer();
    manager.connectHttp(
        QString("http://127.0.0.1:%1/mcp").arg(server.serverPort()),
        "fixture-token");
    QTRY_VERIFY(manager.connected());
    QTRY_COMPARE(initializations, 2);
    QTRY_COMPARE(manager.tools().size(), 1);
    QCOMPARE(manager.tools()[0].toMap().value("name").toString(), "sse-tool");
    manager.disconnectServer();
    // Reconnecting through stdio must stop routing calls to the previous HTTP
    // transport, regardless of whether TLS settings have been initialized.
    manager.connectStdio(TEST_PYTHON, {TEST_SERVER});
    QTRY_VERIFY(manager.connected());
    QTRY_COMPARE(manager.tools().size(), 1);
    QCOMPARE(manager.tools()[0].toMap().value("name").toString(), "echo");
    manager.callTool("echo", R"({"transport":"stdio-after-http"})");
    QTRY_VERIFY(manager.lastResult().contains("stdio-after-http"));
    manager.disconnectServer();
  }

  void httpSseResumesAfterLastCompleteEvent() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QString requestId;
    bool correctResumeId = false;
    connect(&server, &QTcpServer::newConnection, this, [&] {
      auto *socket = server.nextPendingConnection();
      auto buffer = QSharedPointer<QByteArray>::create();
      connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer] {
        *buffer += socket->readAll();
        const auto end = buffer->indexOf("\r\n\r\n");
        if (end < 0)
          return;
        const auto headers = buffer->left(end);
        if (headers.startsWith("GET ")) {
          correctResumeId =
              headers.toLower().contains("last-event-id: delivered");
          const auto body =
              QJsonDocument(
                  QJsonObject{
                      {"jsonrpc", "2.0"},
                      {"id", requestId},
                      {"result",
                       QJsonObject{{"tools", QJsonArray{QJsonObject{
                                                 {"name", "resumed"}}}}}}})
                  .toJson(QJsonDocument::Compact);
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                        "text/event-stream\r\nConnection: close\r\n\r\nid: "
                        "complete\ndata: " +
                        body + "\n\n");
          socket->disconnectFromHost();
          buffer->clear();
          return;
        }
        int length = 0;
        for (const auto &line : headers.split('\n'))
          if (line.trimmed().toLower().startsWith("content-length:"))
            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
        if (buffer->size() < end + 4 + length)
          return;
        const auto request =
            QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
        const auto method = request.value("method").toString();
        if (method == "tools/list") {
          requestId = request.value("id").toString();
          socket->write(
              "HTTP/1.1 200 OK\r\nContent-Type: "
              "text/event-stream\r\nConnection: close\r\n\r\nid: "
              "delivered\ndata:\nretry: 1\n\nid: partial\ndata: {\"jsonrpc\":");
        } else if (method == "initialize") {
          const auto body =
              QJsonDocument(
                  QJsonObject{
                      {"jsonrpc", "2.0"},
                      {"id", request.value("id")},
                      {"result",
                       QJsonObject{{"protocolVersion", "2025-11-25"},
                                   {"capabilities",
                                    QJsonObject{{"tools", QJsonObject{}}}}}}})
                  .toJson(QJsonDocument::Compact);
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                        "application/json\r\nContent-Length: " +
                        QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body);
        } else {
          socket->write("HTTP/1.1 202 Accepted\r\nContent-Length: "
                        "0\r\nConnection: close\r\n\r\n");
        }
        socket->disconnectFromHost();
        buffer->clear();
      });
      connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    McpConnectionManager manager;
    manager.connectHttp(
        QString("http://127.0.0.1:%1/mcp").arg(server.serverPort()), "");
    QTRY_COMPARE(manager.tools().size(), 1);
    QVERIFY(correctResumeId);
  }
};

QTEST_GUILESS_MAIN(McpTests)
#include "test_mcp.moc"
