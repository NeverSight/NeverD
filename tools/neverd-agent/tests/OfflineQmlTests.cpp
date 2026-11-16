//===- OfflineQmlTests.cpp - The window fetches nothing
//--------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A label whose text names an image makes Qt Quick fetch it. Check both
/// guards against that with a listening socket: plain text names nothing, and
/// the window's engine answers a request without a connection.
//===----------------------------------------------------------------------===//

#include "platform/OfflineQml.h"

#include <QNetworkReply>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QTcpServer>
#include <QtTest>
#include <memory>

using namespace neverd::agent;

class OfflineQmlTests : public QObject {
  Q_OBJECT
private:
  // A label showing markup that names an image on \p server, as text a
  // project file or a model could carry.
  static std::unique_ptr<QObject> label(QQmlEngine &engine,
                                        const QTcpServer &server,
                                        const char *format, QString &error) {
    QQmlComponent component(&engine);
    component.setData(QByteArray("import QtQuick\nText { ") + format +
                          "text: '<img src=\"http://127.0.0.1:" +
                          QByteArray::number(server.serverPort()) +
                          "/probe.png\"> name' }",
                      QUrl());
    std::unique_ptr<QObject> object(component.create());
    if (!object)
      error = component.errorString();
    return object;
  }

private slots:
  void aRequestIsAnsweredWithoutAConnection() {
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    OfflineAccessManager manager;
    std::unique_ptr<QNetworkReply> reply(manager.get(QNetworkRequest(QUrl(
        QString("http://127.0.0.1:%1/probe.png").arg(server.serverPort())))));
    QTRY_VERIFY(reply->isFinished());
    QVERIFY(reply->error() != QNetworkReply::NoError);
    QTest::qWait(200);
    QVERIFY(!server.hasPendingConnections());
  }

  // Each label has a server of its own: Qt asks again when a connection is
  // closed on it, and that second call must not be counted against the next
  // label.
  void aLabelReachesNoMachine() {
    QString error;
    {
      // What is guarded against: left to Qt, the markup is laid out and its
      // image asked for.
      QTcpServer server;
      QVERIFY(server.listen(QHostAddress::LocalHost));
      QQmlEngine engine;
      const auto markup = label(engine, server, "", error);
      if (!markup)
        QSKIP(qPrintable("Qt Quick is not available here: " + error));
      // Without that there is nothing here to tell the guards from luck.
      if (!QTest::qWaitFor([&] { return server.hasPendingConnections(); },
                           10000))
        QSKIP("This Qt did not fetch an image named in a label's text.");
    }
    {
      // The rule for every label in the window: plain text names nothing.
      QTcpServer server;
      QVERIFY(server.listen(QHostAddress::LocalHost));
      QQmlEngine engine;
      const auto plain =
          label(engine, server, "textFormat: Text.PlainText; ", error);
      QVERIFY2(plain, qPrintable(error));
      QTest::qWait(1000);
      QVERIFY(!server.hasPendingConnections());
    }
    // The window's engine: even markup that is laid out reaches nothing.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    OfflineAccessFactory offline;
    QQmlEngine engine;
    engine.setNetworkAccessManagerFactory(&offline);
    const auto markup = label(engine, server, "", error);
    QVERIFY2(markup, qPrintable(error));
    QTest::qWait(1000);
    QVERIFY(!server.hasPendingConnections());
  }
};

QTEST_MAIN(OfflineQmlTests)
#include "OfflineQmlTests.moc"
