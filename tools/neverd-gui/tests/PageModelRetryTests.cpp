#include "PageModel.h"

#include <QSignalSpy>
#include <QTest>

class PageModelRetryTests final : public QObject {
  Q_OBJECT
  static QJsonArray page() {
    QJsonArray result;
    for (int i = 0; i < 256; ++i)
      result.append(QJsonObject{{"name", QString::number(i)}});
    return result;
  }
private slots:
  void failedVisiblePageRetriesAndPublishesData() {
    PageModel model({"name"});
    QVERIFY(model.setPage(0, 512, page()));
    QSignalSpy requests(&model, &PageModel::pageRequested);
    connect(&model, &PageModel::pageRequested, &model, [&](int offset) {
      QVERIFY(model.beginPageRequest(offset));
      if (requests.size() == 1)
        model.pageRequestFailed(offset);
      else
        QVERIFY(model.setPage(offset, 512, page()));
    });
    model.data(model.index(300), Qt::UserRole + 1);
    model.data(model.index(301), Qt::UserRole + 1);
    QTRY_COMPARE(requests.size(), 2);
    QCOMPARE(model.data(model.index(300), Qt::UserRole + 1).toString(), "44");
  }
  void emptyFirstPageCanRetryWithoutVisibleDelegates() {
    PageModel model({"name"});
    model.resetPages();
    QVERIFY(model.beginPageRequest(0));
    QVERIFY(!model.beginPageRequest(0));
    QSignalSpy requests(&model, &PageModel::pageRequested);
    model.pageRequestFailed(0);
    QTRY_COMPARE(requests.size(), 1);
    QCOMPARE(requests.first().first().toInt(), 0);
    QVERIFY(model.beginPageRequest(0));
    QVERIFY(model.setPage(0, 256, page()));
    QCOMPARE(model.count(), 256);
  }
  void resetDropsScheduledRequestsAndRetries() {
    PageModel model({"name"});
    QVERIFY(model.setPage(0, 512, page()));
    QSignalSpy requests(&model, &PageModel::pageRequested);
    model.data(model.index(300), Qt::UserRole + 1);
    const auto oldGeneration = model.requestGeneration();
    model.resetPages();
    QVERIFY(model.requestGeneration() != oldGeneration);
    QTest::qWait(30);
    QCOMPARE(requests.size(), 0);
    QVERIFY(model.beginPageRequest(0));
    model.pageRequestFailed(0);
    model.replace({});
    QTest::qWait(200);
    QCOMPARE(requests.size(), 0);
  }
  void retriesAreBoundedAndLaterRequestsRemainPossible() {
    PageModel model({"name"});
    QVERIFY(model.setPage(0, 512, page()));
    QSignalSpy requests(&model, &PageModel::pageRequested);
    const auto connection =
        connect(&model, &PageModel::pageRequested, &model, [&](int offset) {
          QVERIFY(model.beginPageRequest(offset));
          model.pageRequestFailed(offset);
        });
    model.data(model.index(300), Qt::UserRole + 1);
    QTRY_COMPARE(requests.size(), 3);
    QTest::qWait(500);
    QCOMPARE(requests.size(), 3);
    disconnect(connection);
    connect(&model, &PageModel::pageRequested, &model, [&](int offset) {
      QVERIFY(model.beginPageRequest(offset));
      QVERIFY(model.setPage(offset, 512, page()));
    });
    model.data(model.index(300), Qt::UserRole + 1);
    QTRY_COMPARE(requests.size(), 4);
    QCOMPARE(model.data(model.index(300), Qt::UserRole + 1).toString(), "44");
  }
  void malformedPageDoesNotFinishRequest() {
    PageModel model({"name"});
    model.resetPages();
    QVERIFY(model.beginPageRequest(0));
    QVERIFY(!model.setPage(0, 256, {}));
    QSignalSpy requests(&model, &PageModel::pageRequested);
    model.pageRequestFailed(0);
    QTRY_COMPARE(requests.size(), 1);
  }
};
QTEST_GUILESS_MAIN(PageModelRetryTests)
#include "PageModelRetryTests.moc"
