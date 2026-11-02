//===- MicrosoftSymbolsTests.cpp - PDB prompt and download tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the Load PDB file question, consent, downloads into the store the
/// worker names, verification and stale completion with an in-memory network
/// reply and the fixture worker. No test contacts a symbol server.
//===----------------------------------------------------------------------===//

#include "MicrosoftSymbols.h"
#include "PdbPromptDialog.h"
#include "ProjectDatabase.h"
#include "Session.h"
#include "SymbolDownload.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cstring>

using namespace neverd::gui;

namespace {
class Reply final : public QNetworkReply {
public:
  Reply(const QNetworkRequest &request, int status, QByteArray bytes,
        bool delayed, int &aborted, QObject *parent)
      : QNetworkReply(parent), bytes_(std::move(bytes)), aborted_(aborted) {
    setRequest(request);
    setUrl(request.url());
    setOperation(QNetworkAccessManager::GetOperation);
    setAttribute(QNetworkRequest::HttpStatusCodeAttribute, status);
    if (status != 200)
      setError(ContentNotFoundError, QStringLiteral("fixture not found"));
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
    if (!delayed)
      QTimer::singleShot(0, this, [this] {
        if (isFinished())
          return;
        emit readyRead();
        emit downloadProgress(bytes_.size(), bytes_.size());
        setFinished(true);
        emit finished();
      });
  }
  void abort() override {
    if (isFinished())
      return;
    ++aborted_;
    setError(OperationCanceledError, QStringLiteral("cancelled"));
    setFinished(true);
    emit finished();
  }
  qint64 bytesAvailable() const override {
    return bytes_.size() - offset_ + QNetworkReply::bytesAvailable();
  }

protected:
  qint64 readData(char *data, qint64 length) override {
    const auto count = std::min(length, qint64(bytes_.size()) - offset_);
    if (!count)
      return -1;
    std::memcpy(data, bytes_.constData() + offset_, size_t(count));
    offset_ += count;
    return count;
  }

private:
  QByteArray bytes_;
  qint64 offset_ = 0;
  int &aborted_;
};

class Network final : public QNetworkAccessManager {
public:
  QList<QUrl> requests;
  QByteArray bytes = "matching fixture PDB";
  int status = 200, aborted = 0;
  bool delayed = false;

protected:
  QNetworkReply *createRequest(Operation, const QNetworkRequest &request,
                               QIODevice *) override {
    requests.append(request.url());
    return new Reply(request, status, bytes, delayed, aborted, this);
  }
};

QString fixture(const QTemporaryDir &directory, const QString &name,
                const QByteArray &bytes = "fixture") {
  const auto path = directory.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
    return {};
  return path;
}

constexpr char MatchingPdb[] = "matching fixture PDB";
constexpr char FixtureKey[] = "123456789ABCDEF0123456789ABCDEF01";

/// Where the fixture worker's symbol store keeps the PDB of \p input.
QString storePdb(const QString &input) {
  return QFileInfo(input).dir().filePath(
      QStringLiteral("symbols/fixture.pdb/%1/fixture.pdb").arg(FixtureKey));
}

/// The worker's pdb_reference for \p input, before it is opened.
QJsonObject reference(Session &session, const QString &input) {
  QJsonObject result;
  bool answered = false;
  // The worker reads once it has said hello.
  QTest::qWaitFor([&] { return session.findsPdbFiles(); }, 10000);
  session.read(QStringLiteral("pdb_reference"), {{"path", input}}, &session,
               [&](const QJsonObject &payload) {
                 result = payload.value("pdb").toObject();
                 answered = true;
               });
  QTest::qWaitFor([&] { return answered; }, 10000);
  return result;
}

/// Whether \p choice loads \p path, however the engine spelled its
/// separators.
bool loads(const QJsonObject &choice, const QString &path) {
  return choice.value("mode").toString() == QLatin1String("verified") &&
         QFileInfo(choice.value("path").toString()) == QFileInfo(path);
}

QPushButton *button(QWidget &dialog, const char *name) {
  return dialog.findChild<QPushButton *>(QLatin1String(name));
}

bool shows(QWidget &dialog, const char *name) {
  auto *found = button(dialog, name);
  return found && found->isVisibleTo(&dialog);
}

PdbPromptDialog *prompt(QWidget &window) {
  return window.findChild<PdbPromptDialog *>(QStringLiteral("pdbPrompt"));
}
} // namespace

class MicrosoftSymbolsTests final : public QObject {
  Q_OBJECT
private slots:
  void downloadAsksOnlyHttpServers() {
    QTemporaryDir directory;
    Network network;
    SymbolDownload download(&network);
    QSignalSpy failed(&download, &SymbolDownload::failed);
    download.start(QJsonArray{QJsonObject{
                       {"url", "file:///etc/passwd"},
                       {"path", directory.filePath("store/x.pdb/1/x.pdb")}}},
                   QStringLiteral("x.pdb"));
    QTRY_COMPARE(failed.count(), 1);
    QVERIFY(!failed.first().at(1).toBool());
    QVERIFY(network.requests.isEmpty());
    QVERIFY(!download.running());
  }

  void promptLoadsAMatchingLocalCopy() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    Network network;
    const auto input = fixture(directory, "symbols-local.bin");
    const auto beside = fixture(directory, "fixture.pdb", MatchingPdb);
    const auto pdb = reference(session, input);
    QCOMPARE(pdb.value("found").toObject().value("location").toString(),
             QStringLiteral("beside"));
    PdbPromptDialog dialog(session, input, pdb, &network);
    QVERIFY(shows(dialog, "yes") && shows(dialog, "no"));
    QVERIFY(!shows(dialog, "browse") && !shows(dialog, "cancel"));
    QVERIFY(button(dialog, "yes")->isDefault());
    QCOMPARE(dialog.findChild<QLabel *>("path")->toolTip(),
             QDir::toNativeSeparators(beside));
    button(dialog, "yes")->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(loads(dialog.choice(), beside));
    QVERIFY(network.requests.isEmpty());
  }

  void promptDeclinesWithNoOrEscape() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    Network network;
    const auto input = fixture(directory, "symbols-missing.bin");
    const auto pdb = reference(session, input);
    QVERIFY(pdb.value("found").isNull());
    PdbPromptDialog answered(session, input, pdb, &network);
    button(answered, "no")->click();
    QCOMPARE(answered.choice(), (QJsonObject{{"mode", "none"}}));
    PdbPromptDialog closed(session, input, pdb, &network);
    closed.open();
    QTest::keyClick(&closed, Qt::Key_Escape);
    QCOMPARE(closed.choice(), (QJsonObject{{"mode", "none"}}));
    QVERIFY(network.requests.isEmpty());
  }

  void promptDownloadsIntoTheStoreItNames() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    Network network;
    network.bytes = MatchingPdb;
    const auto input = fixture(directory, "symbols-missing.bin");
    PdbPromptDialog dialog(session, input, reference(session, input), &network);
    dialog.open();
    QVERIFY(network.requests.isEmpty());
    button(dialog, "yes")->click();
    QTRY_VERIFY(!dialog.choice().isEmpty());
    QCOMPARE(network.requests.size(), 1);
    QCOMPARE(network.requests.first().toString(),
             QStringLiteral("https://msdl.microsoft.com/download/symbols/"
                            "fixture.pdb/%1/fixture.pdb")
                 .arg(FixtureKey));
    QVERIFY(loads(dialog.choice(), storePdb(input)));
    QFile stored(storePdb(input));
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), QByteArray(MatchingPdb));
  }

  void promptOffersBrowsingWhenNoMatchArrives_data() {
    QTest::addColumn<int>("status");
    QTest::addColumn<QByteArray>("bytes");
    QTest::newRow("not-found") << 404 << QByteArray("not found");
    QTest::newRow("another-build") << 200 << QByteArray("wrong PDB identity");
  }
  void promptOffersBrowsingWhenNoMatchArrives() {
    QFETCH(int, status);
    QFETCH(QByteArray, bytes);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    Network network;
    network.status = status;
    network.bytes = bytes;
    const auto input = fixture(directory, "symbols-missing.bin");
    PdbPromptDialog dialog(session, input, reference(session, input), &network);
    dialog.open();
    button(dialog, "yes")->click();
    QTRY_VERIFY(shows(dialog, "browse"));
    QVERIFY(button(dialog, "browse")->isDefault() && shows(dialog, "no"));
    QVERIFY(dialog.choice().isEmpty() && dialog.isVisible());
    // A file of another build leaves the store again.
    QVERIFY(!QFileInfo::exists(storePdb(input)));
    button(dialog, "no")->click();
    QCOMPARE(dialog.choice(), (QJsonObject{{"mode", "none"}}));
  }

  void onlyMissingEligibleSymbolsPrompt_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<bool>("debug");
    QTest::newRow("local-pdb") << QStringLiteral("symbols-local.bin") << true;
    QTest::newRow("no-identity")
        << QStringLiteral("symbols-absent.bin") << true;
    QTest::newRow("disabled") << QStringLiteral("symbols-missing.bin") << false;
  }
  void onlyMissingEligibleSymbolsPrompt() {
    QFETCH(QString, name);
    QFETCH(bool, debug);
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    MicrosoftSymbols symbols(session, &window, &network);
    LoadOptions options;
    options.debugInfo = debug;
    session.open(fixture(directory, name), options);
    QTRY_VERIFY(session.loaded());
    QTest::qWait(200);
    QVERIFY(!session.metadata().value("pdb_download").isObject());
    QVERIFY(!prompt(window));
    QVERIFY(network.requests.isEmpty());
  }

  void declineDoesNotContactServer_data() {
    QTest::addColumn<QString>("answer");
    QTest::newRow("no") << QStringLiteral("no");
    QTest::newRow("escape") << QStringLiteral("escape");
    QTest::newRow("close") << QStringLiteral("close");
  }

  void declineDoesNotContactServer() {
    QFETCH(QString, answer);
    QTemporaryDir directory;
    const auto input = fixture(directory, "symbols-missing.bin");
    {
      Session session(QString::fromLocal8Bit(TEST_WORKER));
      QWidget window;
      Network network;
      MicrosoftSymbols symbols(session, &window, &network);
      session.open(input);
      QTRY_VERIFY(prompt(window));
      QVERIFY(network.requests.isEmpty());
      if (answer == QLatin1String("no"))
        button(*prompt(window), "no")->click();
      else if (answer == QLatin1String("escape"))
        QTest::keyClick(prompt(window), Qt::Key_Escape);
      else
        prompt(window)->close();
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
      QVERIFY(network.requests.isEmpty());
      QVERIFY(session.loaded());
      QCOMPARE(session.metadata().value("pdb").toObject(),
               (QJsonObject{{"mode", "none"}}));
      const auto saved = ProjectDatabase::readProjectFile(
          input, ProjectDatabase::DebugFilesSuffix);
      QVERIFY(saved);
      QCOMPARE(saved->object().value("pdb").toObject(),
               (QJsonObject{{"mode", "none"}}));
      session.reload();
      QTRY_VERIFY(session.loaded());
      QTest::qWait(200);
      QVERIFY(!prompt(window));
      QCOMPARE(session.metadata().value("pdb").toObject(),
               (QJsonObject{{"mode", "none"}}));
      QVERIFY(network.requests.isEmpty());
    }
    // A new session/process restores the answer, while a manual load can
    // still replace it with an explicitly selected PDB.
    Session reopened(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    MicrosoftSymbols symbols(reopened, &window, &network);
    reopened.open(input);
    QTRY_VERIFY(reopened.loaded());
    QTest::qWait(200);
    QVERIFY(!prompt(window));
    QCOMPARE(reopened.metadata().value("pdb").toObject(),
             (QJsonObject{{"mode", "none"}}));
    const auto pdb = fixture(directory, "manual.pdb", MatchingPdb);
    reopened.loadPdb({{"path", pdb}, {"mode", "verified"}});
    QTRY_COMPARE(
        reopened.metadata().value("pdb").toObject().value("mode").toString(),
        QStringLiteral("verified"));
    QVERIFY(network.requests.isEmpty());
  }

  void consentDownloadsVerifiesAndStoresForTheNextLoad() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    network.bytes = MatchingPdb;
    MicrosoftSymbols symbols(session, &window, &network);
    const auto input = fixture(directory, "symbols-missing.bin");
    session.open(input);
    QTRY_VERIFY(prompt(window));
    QVERIFY(network.requests.isEmpty());
    button(*prompt(window), "yes")->click();
    QTRY_VERIFY(session.metadata().value("pdb").isObject());
    QCOMPARE(network.requests.size(), 1);
    const auto selected = session.metadata().value("pdb").toObject();
    QCOMPARE(selected.value("mode").toString(), QStringLiteral("verified"));
    QVERIFY(loads(selected, storePdb(input)));
    QVERIFY(selected.value("stamp").isObject());
    session.closeFile();
    // The store is where the next load finds it, without asking.
    session.open(input);
    QTRY_VERIFY(session.loaded());
    QTest::qWait(200);
    QVERIFY(!session.metadata().value("pdb_download").isObject());
    QVERIFY(!prompt(window));
    QCOMPARE(network.requests.size(), 1);
  }

  void changingFileCancelsPendingDownload() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QWidget window;
    Network network;
    network.delayed = true;
    MicrosoftSymbols symbols(session, &window, &network);
    session.open(fixture(directory, "symbols-missing.bin"));
    QTRY_VERIFY(prompt(window));
    button(*prompt(window), "yes")->click();
    QTRY_COMPARE(network.requests.size(), 1);
    const auto next = fixture(directory, "symbols-absent.bin");
    session.open(next);
    QTRY_VERIFY(session.loaded() && session.filePath() == next);
    QTRY_COMPARE(network.aborted, 1);
    QVERIFY(!session.metadata().value("pdb").isObject());
    QTRY_VERIFY(!prompt(window));
  }
};

QTEST_MAIN(MicrosoftSymbolsTests)
#include "MicrosoftSymbolsTests.moc"
