//===- LineageGraph.cpp - Paged comparison graph rendering ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Draws original report endpoints and distinct parallel evidence. Native
/// matching and function lineage remain wholly in the engine.
//===----------------------------------------------------------------------===//

#include "LineageGraph.h"

#include "../ResourceLimits.h"
#include "SingleLineText.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsTextItem>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainterPath>
#include <QResizeEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace neverd::gui {
LineageGraph::LineageGraph(QWidget *Parent) : QGraphicsView(Parent) {
  setScene(new QGraphicsScene(this));
  setRenderHint(QPainter::Antialiasing);
  setDragMode(ScrollHandDrag);
  setTransformationAnchor(AnchorUnderMouse);
  setAccessibleName(
      tr("Version comparison diagram for the current report page"));
  connect(&Theme::instance(), &Theme::changed, this, &LineageGraph::rebuild);
}
void LineageGraph::setReports(QJsonArray Rows) {
  Reports = std::move(Rows);
  rebuild();
  fit();
}
void LineageGraph::setLabels(const QMap<QString, QString> &Names) {
  Labels = Names;
  rebuild();
}
void LineageGraph::fit() {
  AutoFit = true;
  if (scene()->items().isEmpty())
    return;
  fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
  if (transform().m11() > 1)
    resetTransform();
}
void LineageGraph::zoom(bool In) {
  const auto Step = ::neverd::limits::gui::kComparisonZoomStep;
  const auto Current = transform().m11();
  const auto Target = std::clamp(Current * (In ? Step : 1 / Step),
                                 ::neverd::limits::gui::kComparisonMinimumZoom,
                                 ::neverd::limits::gui::kComparisonMaximumZoom);
  // Fitting a large page can go below the manual zoom range. Zooming in must
  // reenter that range; zooming out must never enlarge the fitted diagram.
  if ((In && Target <= Current) || (!In && Target >= Current))
    return;
  AutoFit = false;
  scale(Target / Current, Target / Current);
}
void LineageGraph::rebuild() {
  scene()->clear();
  QMap<QString, QPointF> Positions;
  QMap<QString, QString> Names = Labels;
  for (const auto &Value : Reports) {
    const auto R = Value.toObject();
    Positions[R["primary_version"].toString()] = {};
    Positions[R["secondary_version"].toString()] = {};
    for (const auto &Side :
         {QStringLiteral("primary"), QStringLiteral("secondary")})
      if (!R[Side + "_label"].toString().isEmpty())
        Names[R[Side + "_version"].toString()] = R[Side + "_label"].toString();
  }
  Positions.remove(QString());
  const QFontMetricsF Metrics(font());
  const auto Height = Metrics.height() * 3;
  const auto Width = Metrics.horizontalAdvance(QString(24, 'M'));
  const auto Radius =
      std::max(Width / 2, Positions.size() * Width / (2 * std::numbers::pi));
  int N = 0;
  for (auto It = Positions.begin(); It != Positions.end(); ++It, ++N) {
    const auto Angle = 2 * std::numbers::pi * N / Positions.size();
    It.value() = Positions.size() == 1 ? QPointF()
                                       : QPointF(Radius * std::cos(Angle),
                                                 Radius * std::sin(Angle));
  }
  const auto &Theme = Theme::instance();
  setBackgroundBrush(Theme.chrome("Base"));
  QMap<QString, int> Parallel;
  for (const auto &Value : Reports) {
    const auto R = Value.toObject();
    const auto A = R["primary_version"].toString(),
               B = R["secondary_version"].toString();
    if (!Positions.contains(A) || !Positions.contains(B))
      continue;
    const auto Key = std::min(A, B) + ':' + std::max(A, B);
    const int Order = Parallel[Key]++;
    const auto Start = Positions[A], End = Positions[B];
    QPainterPath Path(Start);
    if (A == B)
      Path.cubicTo(Start + QPointF(Width, -Height * (Order + 2)),
                   Start + QPointF(-Width, -Height * (Order + 2)), Start);
    else {
      const auto Delta = End - Start;
      const auto Length = std::hypot(Delta.x(), Delta.y());
      const auto Bend = ((Order + 1) / 2) * Height * (Order % 2 ? 1 : -1);
      Path.quadTo((Start + End) / 2 +
                      QPointF(-Delta.y(), Delta.x()) * (Bend / Length),
                  End);
    }
    auto *Line = scene()->addPath(Path, QPen(Theme.chrome("Accent"), 1));
    Line->setToolTip(tr("Source report: %1\nComparison direction: %2 → %3")
                         .arg(R["id"].toString(), A, B));
    Line->setData(0, "report");
    Line->setData(1, R["id"].toString());
  }
  for (auto It = Positions.begin(); It != Positions.end(); ++It) {
    auto *Node = scene()->addRect(
        QRectF(-Width / 2, -Height / 2, Width, Height),
        QPen(Theme.chrome("Border")), QBrush(Theme.chrome("Base")));
    Node->setPos(It.value());
    Node->setData(0, "version");
    Node->setData(1, It.key());
    Node->setToolTip(Names.value(It.key(), It.key()) + '\n' + It.key());
    auto *Text = new QGraphicsTextItem(Node);
    Text->setFont(font());
    Text->setDefaultTextColor(Theme.chrome("Text"));
    const auto Label = Metrics.elidedText(
        singleLineDisplayText(Names.value(It.key(), It.key())), Qt::ElideMiddle,
        Width - Metrics.height());
    Text->setPlainText(Label + '\n' + It.key().left(16));
    Text->setPos(-Width / 2 + Metrics.height() / 3, -Height / 2);
  }
  scene()->setSceneRect(
      scene()->itemsBoundingRect().adjusted(-Height, -Height, Height, Height));
}
void LineageGraph::mouseDoubleClickEvent(QMouseEvent *Event) {
  auto *Item = itemAt(Event->pos());
  while (Item && Item->data(0).isNull())
    Item = Item->parentItem();
  if (Item) {
    if (Item->data(0) == "version")
      emit versionActivated(Item->data(1).toString());
    else if (Item->data(0) == "report")
      emit reportActivated(Item->data(1).toString());
    Event->accept();
    return;
  }
  QGraphicsView::mouseDoubleClickEvent(Event);
}
void LineageGraph::changeEvent(QEvent *Event) {
  QGraphicsView::changeEvent(Event);
  if (Event->type() == QEvent::FontChange) {
    rebuild();
    if (AutoFit)
      fit();
  }
}
void LineageGraph::resizeEvent(QResizeEvent *Event) {
  QGraphicsView::resizeEvent(Event);
  if (AutoFit)
    fit();
}
void LineageGraph::wheelEvent(QWheelEvent *Event) {
  if (Event->modifiers().testFlag(Qt::ControlModifier)) {
    zoom(Event->angleDelta().y() > 0);
    Event->accept();
    return;
  }
  QGraphicsView::wheelEvent(Event);
}
} // namespace neverd::gui
