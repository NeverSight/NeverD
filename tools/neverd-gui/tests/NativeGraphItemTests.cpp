#include "NativeGraphItem.h"

#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <limits>

class NativeGraphItemTests final : public QObject {
  Q_OBJECT
  static QVariantMap block(int i) {
    return {{"id", QString::number(i)},
            {"address", "0xffff800012340000"},
            {"label", "entry"},
            {"x", i * 380.0},
            {"y", 250.0},
            {"width", 300.0},
            {"height", 180.0},
            {"lines", QVariantList{"ret"}}};
  }
private slots:
  void viewportHitTestingPreservesWideAddresses() {
    NativeGraphItem item;
    item.setNodes({block(10)});
    item.setViewportX(3800);
    item.setViewportY(200);
    item.setZoom(2);
    QCOMPARE(item.addressAt(20, 120), "0xffff800012340000");
    QVERIFY(item.addressAt(610, 120).isEmpty());
    item.setViewportX(8000);
    QVERIFY(item.addressAt(20, 120).isEmpty());
  }
  void rendererOnlyRetainsBoundedWindow() {
    NativeGraphItem item;
    QVariantList huge;
    for (int i = 0; i < 10000; ++i)
      huge.append(block(i));
    item.setNodes(huge);
    QCOMPARE(item.boundedNodeCount(), 256);
    QCOMPARE(item.nodes().size(), 256);
    QCOMPARE(item.childItems().size(), 0);
    QVariantList edges;
    const QVariantList points{QVariantMap{{"x", 10}, {"y", 10}},
                              QVariantMap{{"x", 200}, {"y", 200}}};
    for (int i = 0; i < 10000; ++i)
      edges.append(QVariantMap{{"points", points}});
    item.setEdges(edges);
    QCOMPARE(item.boundedEdgeCount(), 512);
    QCOMPARE(item.edges().size(), 512);
  }
  void invalidGeometryIsIgnored() {
    NativeGraphItem item;
    auto negative = block(1);
    negative["width"] = -2;
    auto nonfinite = block(2);
    nonfinite["x"] = std::numeric_limits<double>::quiet_NaN();
    item.setNodes({negative, nonfinite, block(3), block(3)});
    QCOMPARE(item.boundedNodeCount(), 1);
    item.setEdges({QVariantMap{
        {"points", QVariantList{QVariantMap{{"x", 1}, {"y", 2}}}}}});
    QCOMPARE(item.boundedEdgeCount(), 0);
  }
  void replacingRenderedLabelsKeepsTextureOwnership() {
    QQuickWindow window;
    window.resize(400, 300);
    auto *item = new NativeGraphItem(window.contentItem());
    item->setSize(QSizeF(400, 300));
    item->setViewportY(200);
    item->setNodes({block(0)});
    QSignalSpy frames(&window, &QQuickWindow::frameSwapped);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QTRY_VERIFY(frames.size() > 0);
    auto frame = frames.size();
    auto font = item->codeFont();
    font.setPointSizeF(12);
    item->setCodeFont(font);
    QTRY_VERIFY(frames.size() > frame);
    frame = frames.size();
    auto changed = block(0);
    changed["label"] = "replacement texture";
    item->setNodes({changed});
    QTRY_VERIFY(frames.size() > frame);
    frame = frames.size();
    item->setZoom(
        0.5); // Remove and destroy all label textures at overview scale.
    QTRY_VERIFY(frames.size() > frame);
    frame = frames.size();
    item->setZoom(1);
    QTRY_VERIFY(frames.size() > frame);
    window.hide();
  }
};
QTEST_MAIN(NativeGraphItemTests)
#include "NativeGraphItemTests.moc"
