//===- ComparisonGraph.h - Viewport-based paired CFG review -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paints only the current worker-layout viewport; block addresses link the
/// two graphs without assuming that their geometry or ordering is identical.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONGRAPH_H
#define NEVERD_GUI_COMPARISONGRAPH_H

#include <QAbstractScrollArea>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

namespace neverd::gui {
class ComparisonSession;
class ComparisonGraph final : public QAbstractScrollArea {
  Q_OBJECT
public:
  ComparisonGraph(ComparisonSession &Session, QString Side,
                  QWidget *Parent = nullptr);
  void showFunction(const QString &Address);
  void focusBlock(const QString &Address);
  void fit();
  qreal zoom() const { return Scale; }

signals:
  void blockSelected(const QString &Address, const QString &Counterpart);
  void statusChanged(const QString &Message);

protected:
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void mousePressEvent(QMouseEvent *) override;

private:
  ComparisonSession &Session;
  QString Side, Address, LayoutRevision, Selected, Message;
  QJsonArray Nodes, Edges;
  QRectF Bounds;
  QTimer Update;
  qreal Scale = 1;
  bool FitView = true;
  quint64 Generation = 0, ViewGeneration = 0;
  void requestView();
  void fetchView(QJsonObject Query, quint64 Serial);
  void scrollRanges();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONGRAPH_H
