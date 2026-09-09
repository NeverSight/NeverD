#include "Workbench.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class WorkbenchTests : public QObject {
  Q_OBJECT
private slots:
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
    controller.filterFunctions("function_599");
    QTRY_COMPARE(functions->count(), 1);
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
};
QTEST_MAIN(WorkbenchTests)
#include "WorkbenchTests.moc"
