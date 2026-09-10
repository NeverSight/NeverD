#include "Workbench.h"

#include <QDataStream>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#ifdef TEST_REAL_WORKER
namespace {
// A non-executed ELF containing either one return-value body or a direct call
// to a second body. The optional MAP names only the entry, so analysis must
// publish the additional function discovered through the call.
QByteArray nativeFixture(bool named) {
  const auto code = QByteArray::fromHex(named ? "e803000000c30000b807000000c3"
                                              : "b807000000c3");
  QByteArray data;
  QDataStream out(&data, QIODevice::WriteOnly);
  out.setByteOrder(QDataStream::LittleEndian);
  auto ident = QByteArray::fromHex("7f454c46020101");
  ident.resize(16, '\0');
  out.writeRawData(ident.constData(), ident.size());
  out << quint16(2) << quint16(62) << quint32(1) << quint64(0x400078)
      << quint64(64) << quint64(0) << quint32(0) << quint16(64) << quint16(56)
      << quint16(1) << quint16(64) << quint16(0) << quint16(0);
  out << quint32(1) << quint32(5) << quint64(0) << quint64(0x400000)
      << quint64(0x400000) << quint64(120 + code.size())
      << quint64(120 + code.size()) << quint64(4096);
  data += code;
  return data;
}
} // namespace
#endif

class WorkbenchTests : public QObject {
  Q_OBJECT
  QTemporaryDir settingsDirectory_;
private slots:
#ifdef TEST_REAL_WORKER
  void nativeAnalysisRefreshesNavigation_data() {
    QTest::addColumn<bool>("named");
    QTest::newRow("stripped-entry") << false;
    QTest::newRow("named-entry-with-recovered-callee") << true;
  }
  void nativeAnalysisRefreshesNavigation() {
    QFETCH(bool, named);
    QTemporaryDir directory;
    const auto path = directory.filePath("native.elf");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes = nativeFixture(named);
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    if (named) {
      QFile map(directory.filePath("native.map"));
      QVERIFY(map.open(QIODevice::WriteOnly));
      map.write("VMA LMA Size Align Out In Symbol\n"
                "00400078 00400078 0000000e 1 .text\n"
                "00400078 00400078 00000006 1 named_main\n");
    }
    Workbench controller(QString::fromLocal8Bit(TEST_REAL_WORKER));
    int initialCount = -1;
    connect(&controller, &Workbench::changed, this, [&] {
      if (controller.loaded() && initialCount < 0)
        initialCount = controller.functionCount();
    });
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(controller.loaded(), 5000);
    QCOMPARE(initialCount, named ? 1 : 0);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.functionCount(), named ? 2 : 1, 5000);
    QCOMPARE(controller.selectedFunctionAddress(), QString("0x400078"));
    QVERIFY(!controller.selectedFunctionName().isEmpty());
    auto *functions = qobject_cast<PageModel *>(controller.functionsModel());
    QVERIFY(functions);
    QCOMPARE(functions->count(), named ? 2 : 1);
    QSignalSpy resets(functions, &QAbstractItemModel::modelReset);
    controller.setComment("ordinary revision after analysis");
    QTRY_COMPARE(controller.selectedComment(),
                 QString("ordinary revision after analysis"));
    QCOMPARE(resets.size(), 0);
    QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
  }
  void externalAnalysisRefreshesFunctionList() {
    QTemporaryDir directory;
    const auto path = directory.filePath("native.elf");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    const auto bytes = nativeFixture(false);
    QCOMPARE(file.write(bytes), bytes.size());
    file.close();
    Workbench controller(QString::fromLocal8Bit(TEST_REAL_WORKER));
    QSignalSpy replies(&controller, &Workbench::externalResponse);
    bool submitted = false;
    connect(&controller, &Workbench::changed, this, [&] {
      if (!controller.loaded() || submitted)
        return;
      submitted = true;
      // Retire startup view requests so the external request is the first
      // caller to complete analysis in this shared worker session.
      controller.cancel();
      controller.externalQuery(
          "external-analysis", "decompile",
          {{"address", "0x400078"}, {"representation", "c"}}, {});
    });
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 5000);
    const auto response = replies.first().at(1).toJsonObject();
    QCOMPARE(response["status"].toString(), QString("ok"));
    QCOMPARE(response["analysis_state"].toString(), QString("complete"));
    QTRY_COMPARE_WITH_TIMEOUT(controller.functionCount(), 1, 5000);
    auto *functions = qobject_cast<PageModel *>(controller.functionsModel());
    QVERIFY(functions);
    QCOMPARE(functions->count(), 1);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.selectedAddress().isEmpty());
    QVERIFY(controller.representationText().isEmpty());
  }
#endif
  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName("NeverDTests");
    QCoreApplication::setApplicationName("WorkbenchTests");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
  }
  void pageModelBoundaries() {
    PageModel model({"address", "name"});
    QJsonArray rows;
    for (int i = 0; i < 5000; ++i)
      rows.append(QJsonObject{{"address", QString::number(i)}, {"name", "f"}});
    model.replace(rows);
    QCOMPARE(model.count(), 4096);
    QCOMPARE(model.get(0)["address"].toString(), "0");
    model.append(QJsonArray{QJsonObject{{"address", "next"}, {"name", "g"}}});
    QCOMPARE(model.count(), 4096);
    QCOMPARE(model.get(0)["address"].toString(), "1");
    QCOMPARE(model.get(4095)["address"].toString(), "next");
    QVERIFY(model.get(-1).isEmpty());
    QCOMPARE(model.rowCount(model.index(0, 0)), 0);
  }
  void millionRowModelRequestsOnlyVisiblePages() {
    PageModel model({"address", "name"});
    QJsonArray page;
    for (int i = 0; i < 256; ++i)
      page.append(QJsonObject{{"address", QString::number(i)}, {"name", "f"}});
    model.setPage(0, 1000000, page);
    QCOMPARE(model.rowCount(), 1000000);
    QSignalSpy requests(&model, &PageModel::pageRequested);
    QVERIFY(
        model.data(model.index(900000), Qt::UserRole + 1).toString().isEmpty());
    model.data(model.index(900001), Qt::UserRole + 2);
    QTRY_COMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toInt(), 899840);
    model.setPage(899840, 1000000, page);
    QCOMPARE(model.data(model.index(900000), Qt::UserRole + 1).toString(),
             "160");
    QVERIFY(model.get(1000000).isEmpty());
  }
  void editsAreSerializedAndSessionChangesAreExplicit() {
    QTemporaryDir directory;
    const auto first = directory.filePath("first.bin");
    const auto second = directory.filePath("second.bin");
    for (const auto &path : {first, second}) {
      QFile file(path);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("fixture");
    }
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    controller.openFile(QUrl::fromLocalFile(first));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    controller.setComment("first revision");
    controller.setComment("second revision");
    controller.saveAnnotations();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(first + ".neverd-annotations.json"),
                             5000);
    QTRY_VERIFY(!controller.unsavedChanges());
    QFile saved(first + ".neverd-annotations.json");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    QVERIFY(saved.readAll().contains("second revision"));
    saved.close();
    QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));

    controller.setComment("comment on A");
    controller.navigate("function_20");
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340140"));
    QTRY_VERIFY(controller.unsavedChanges());
    // A delayed edit completion must not label the newly selected instruction.
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.selectedComment() != "comment on A");
    QSignalSpy confirmation(&controller, &Workbench::confirmSessionChange);
    controller.openFile(QUrl::fromLocalFile(second));
    QCOMPARE(confirmation.size(), 1);
    QCOMPARE(controller.filePath(), first);
    controller.resolveSessionChange("cancel");
    QCOMPARE(controller.filePath(), first);
    controller.openFile(QUrl::fromLocalFile(second));
    controller.resolveSessionChange("save");
    QTRY_COMPARE_WITH_TIMEOUT(controller.filePath(), second, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    QVERIFY(!controller.unsavedChanges());

    // A deliberately interrupted read gets a terminal broker response.
    QSignalSpy replies(&controller, &Workbench::externalResponse);
    controller.externalQuery("external-restart", "metadata", {}, {});
    controller.restartWorker();
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first().at(0).toString(), QString("external-restart"));
    QCOMPARE(replies.first()
                 .at(1)
                 .toJsonObject()["error"]
                 .toObject()["code"]
                 .toString(),
             QString("worker_stopped"));
    QTRY_VERIFY_WITH_TIMEOUT(controller.loaded(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    controller.setComment("keep before close");
    QVERIFY(!controller.requestClose());
    QSignalSpy closeReady(&controller, &Workbench::closeReady);
    controller.resolveSessionChange("save");
    QTRY_COMPARE_WITH_TIMEOUT(closeReady.size(), 1, 5000);
    QVERIFY(!controller.unsavedChanges());
  }
  void realControllerTransport() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.bin");
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("fixture");
    file.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(controller.loaded(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(controller.selectedAddress(),
                              QString("0xffff800012340000"), 5000);
    auto *functions = qobject_cast<PageModel *>(controller.functionsModel());
    auto *instructions =
        qobject_cast<PageModel *>(controller.instructionsModel());
    QVERIFY(functions);
    QVERIFY(instructions);
    QTRY_COMPARE_WITH_TIMEOUT(functions->count(), 600, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(instructions->count(), 256, 5000);
    QCOMPARE(functions->get(0)["address"].toString(), "0xffff800012340000");
    QCOMPARE(instructions->get(0)["address"].toString(), "0xffff800012340000");
    // A timer continues firing during the deliberately synchronous worker
    // pipeline.
    int ticks = 0;
    QTimer timer;
    timer.setInterval(10);
    connect(&timer, &QTimer::timeout, this, [&] { ++ticks; });
    timer.start();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    QVERIFY(ticks > 20);
    QVERIFY(controller.representationText().contains("code line 511"));
    controller.toggleRepresentationPin();
    QTRY_VERIFY(controller.hasMoreText());
    controller.navigate("function_1");
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340010"));
    QVERIFY(controller.hasMoreText());
    controller.loadMoreText();
    QTRY_VERIFY(controller.representationText().contains("code line 699"));
    QVERIFY(!controller.hasMoreText());
    controller.toggleRepresentationPin();
    controller.navigate("0xffff800012340000");
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340000"));
    controller.setRepresentation("low");
    QTRY_VERIFY(!controller.textMappings().isEmpty());
    controller.selectTextLine(3);
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340003"));
    controller.loadMoreFunctions();
    QTRY_COMPARE(functions->get(256)["name"].toString(),
                 QString("function_256"));
    const auto beforeFilter = functions->requestGeneration();
    const auto selectedBeforeFilter = controller.selectedAddress();
    controller.filterFunctions("function_59");
    // Filtering must retire old selectable rows before either debounce or IPC.
    QCOMPARE(functions->count(), 0);
    QCOMPARE(controller.functionCount(), 0);
    QVERIFY(functions->get(0).isEmpty());
    QVERIFY(functions->requestGeneration() != beforeFilter);
    controller.filterFunctions("function_599");
    QCOMPARE(functions->count(), 0);
    QTRY_COMPARE(functions->count(), 1);
    QCOMPARE(functions->get(0)["name"].toString(), "function_599");
    QCOMPARE(functions->get(0)["address"].toString(),
             QString("0xffff800012342570"));
    QCOMPARE(controller.selectedAddress(), selectedBeforeFilter);
    // A previous debounce must not later replace the final one-row result.
    QTest::qWait(250);
    QCOMPARE(functions->count(), 1);
    QCOMPARE(functions->get(0)["name"].toString(), "function_599");
    controller.navigate("function_20");
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340140"));
    QTRY_VERIFY(!controller.representationText().isEmpty());
    controller.requestView("hex");
    QTRY_VERIFY(!controller.hexText().isEmpty());
    QVERIFY(controller.hexText().startsWith("ffff800012340140"));
    controller.requestView("cfg");
    QTRY_COMPARE(controller.graphNodes().size(), 2);
    controller.setComment(QString::fromUtf8("中文 تعليق"));
    QTRY_COMPARE(controller.selectedComment(), QString::fromUtf8("中文 تعليق"));
    controller.saveAnnotations();
    QTRY_VERIFY(QFile::exists(path + ".neverd-annotations.json"));
    controller.renameFunction("renamed");
    QTRY_COMPARE(controller.selectedFunctionName(), QString("renamed"));
    controller.goBack();
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340000"));
    QVERIFY(controller.canGoForward());
    controller.goForward();
    QTRY_COMPARE(controller.selectedAddress(), QString("0xffff800012340140"));
    controller.restartWorker();
    QTRY_VERIFY_WITH_TIMEOUT(controller.loaded(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    QVERIFY(controller.error().isEmpty());
  }
  void capturedEditTargetsSurviveFocusAndSourcePaneRemoval() {
    QTemporaryDir directory;
    const auto path = directory.filePath("targets.bin");
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    auto *panes = controller.paneRegistry();
    const auto sourceId = panes->createPane("machine");
    auto *source = panes->findPane(sourceId);
    panes->setPaneVisible(sourceId, true);
    panes->setActivePane(sourceId);
    source->navigate("function_20");
    QTRY_COMPARE(source->selectedAddress(), QString("0xffff800012340140"));
    const auto target = controller.captureCommandTarget();
    panes->setActivePane("machine");
    QCOMPARE(controller.selectedAddress(), QString("0xffff800012340000"));
    controller.setCommentAt(target, "comment on captured extra pane");
    QVERIFY(panes->beginRemovePane(sourceId));
    panes->finishRemovePane(sourceId);
    QTRY_VERIFY(!controller.busy());
    QVERIFY(controller.unsavedChanges());
    QVERIFY(controller.selectedComment() != "comment on captured extra pane");
    controller.saveAnnotations();
    QTRY_VERIFY(QFile::exists(path + ".neverd-annotations.json"));
    QTRY_VERIFY(!controller.unsavedChanges());
    QFile saved(path + ".neverd-annotations.json");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto bytes = saved.readAll();
    QCOMPARE(saved.error(), QFileDevice::NoError);
    // Rename also replaces this sidecar; an open QFile blocks that on Windows.
    saved.close();
    QVERIFY(bytes.contains("0xffff800012340140"));
    QVERIFY(bytes.contains("comment on captured extra pane"));
    controller.navigate("function_20");
    QTRY_COMPARE(controller.selectedComment(),
                 QString("comment on captured extra pane"));
    controller.renameFunctionAt(target, "captured_function");
    QTRY_VERIFY2(
        controller.selectedFunctionName() == QString("captured_function"),
        qPrintable(
            QString("Expected captured_function; actual=%1; error=%2")
                .arg(controller.selectedFunctionName(), controller.error())));
    QFile renames(path + ".neverd-renames.json");
    QVERIFY(renames.open(QIODevice::ReadOnly));
    const auto renameBytes = renames.readAll();
    QCOMPARE(renames.error(), QFileDevice::NoError);
    renames.close();
    const auto entries = QJsonDocument::fromJson(renameBytes).array();
    QCOMPARE(entries.size(), 1);
    const auto entry = entries.first().toObject();
    QCOMPARE(entry.value("addr").toString(), QString("0xffff800012340140"));
    QCOMPARE(entry.value("renamed").toString(), QString("captured_function"));
  }
  void queuedOpenIntentsUseTheNewSessionAndOldDialogTargetsExpire() {
    QTemporaryDir directory;
    const auto first = directory.filePath("first.bin");
    const auto second = directory.filePath("second.bin");
    const auto third = directory.filePath("third.bin");
    for (const auto &path : {first, second, third}) {
      QFile input(path);
      QVERIFY(input.open(QIODevice::WriteOnly));
      input.write("fixture");
    }
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    controller.openFile(QUrl::fromLocalFile(first));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    const auto oldTarget = controller.captureCommandTarget();
    QSignalSpy confirmation(&controller, &Workbench::confirmSessionChange);
    controller.openFile(QUrl::fromLocalFile(second));
    controller.openFile(QUrl::fromLocalFile(third));
    QTRY_COMPARE_WITH_TIMEOUT(controller.filePath(), third, 7000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    QCOMPARE(confirmation.size(), 0);
    controller.setCommentAt(oldTarget, "must not enter new project");
    QVERIFY(!controller.unsavedChanges());
    QVERIFY(controller.selectedComment().isEmpty());
    QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
  }
  void cancellingPaneReadsStillCompletesExternalQueries() {
    QTemporaryDir directory;
    const auto path = directory.filePath("external.bin");
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    auto *functions = qobject_cast<PageModel *>(controller.functionsModel());
    QVERIFY(functions);
    // The first completed analysis refreshes the browser after publishing text.
    // Establish a populated browser before checking synchronous Cancel effects.
    QTRY_VERIFY_WITH_TIMEOUT(
        !controller.busy() && controller.functionCount() == 600 &&
            functions->count() == 600 && !functions->get(0).isEmpty(),
        7000);
    QSignalSpy replies(&controller, &Workbench::externalResponse);
    controller.setRepresentation("low");
    controller.externalQuery("external-survives-cancel", "metadata", {}, {});
    const auto count = controller.functionCount();
    controller.cancel();
    QCOMPARE(qobject_cast<PageModel *>(controller.functionsModel())->count(),
             count);
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 5000);
    QCOMPARE(replies.first().first().toString(), "external-survives-cancel");
    QCOMPARE(replies.first()[1].toJsonObject()["status"].toString(), "ok");
    replies.clear();
    controller.externalQuery("exact-stale", "metadata", {}, "invalid-revision");
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first()[1]
                 .toJsonObject()["error"]
                 .toObject()["code"]
                 .toString(),
             "stale_revision");
  }
  void cancelAtFirstLoadedNotificationPreventsLateEntryNavigation() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("startup-cancel.bin");
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    bool cancelled = false;
    QObject observer;
    connect(
        &controller, &Workbench::changed, &observer,
        [&] {
          if (cancelled || !controller.loaded())
            return;
          cancelled = true;
          controller.cancel();
        },
        Qt::DirectConnection);

    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(cancelled, 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!controller.busy(), 7000);
    // The workspace's startup reads finish, but their history callback must
    // respect the Cancel observed during the first loaded notification.
    QCOMPARE(controller.functionCount(), 600);
    QVERIFY(controller.selectedAddress().isEmpty());
    QVERIFY(controller.representationText().isEmpty());
    QVERIFY2(controller.error().isEmpty(), qPrintable(controller.error()));
  }
  void saveBeforeCloseDoesNotAcceptAnUncoveredLateEdit() {
    QTemporaryDir directory;
    const auto path = directory.filePath("transition.bin");
    QFile input(path);
    QVERIFY(input.open(QIODevice::WriteOnly));
    input.write("fixture");
    input.close();
    Workbench controller(QString::fromLocal8Bit(TEST_WORKER));
    controller.openFile(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.representationText().isEmpty(), 7000);
    controller.setComment("accepted before close");
    const auto target = controller.captureCommandTarget();
    QVERIFY(!controller.requestClose());
    QSignalSpy ready(&controller, &Workbench::closeReady);
    controller.resolveSessionChange("save");
    controller.setCommentAt(target, "late edit after save intent");
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 5000);
    QVERIFY(!controller.unsavedChanges());
    QFile saved(path + ".neverd-annotations.json");
    QVERIFY(saved.open(QIODevice::ReadOnly));
    const auto bytes = saved.readAll();
    QVERIFY(bytes.contains("accepted before close"));
    QVERIFY(!bytes.contains("late edit after save intent"));
  }
};
QTEST_MAIN(WorkbenchTests)
#include "WorkbenchTests.moc"
