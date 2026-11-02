//===- ComparisonGraph.cpp - Viewport-based paired CFG review -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared deterministic worker geometry with bounded local paint data. Pan,
/// zoom, resize and counterpart navigation discard obsolete viewport replies.
//===----------------------------------------------------------------------===//

#include "ComparisonGraph.h"

#include "../ResourceLimits.h"
#include "ComparisonSession.h"
#include "Theme.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QShowEvent>
#include <QWheelEvent>
#include <cmath>

namespace neverd::gui {
namespace {
namespace limits = ::neverd::limits::gui;
QRectF jsonRect(const QJsonObject &O) {
  return {O["x"].toDouble(), O["y"].toDouble(), O["width"].toDouble(),
          O["height"].toDouble()};
}
} // namespace
ComparisonGraph::ComparisonGraph(ComparisonSession &S, QString Which,
                                 QWidget *Parent)
    : QAbstractScrollArea(Parent), Session(S), Side(std::move(Which)) {
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(tr("%1 control-flow graph").arg(Side));
  Update.setSingleShot(true);
  Update.setInterval(limits::kComparisonGraphDebounceMs);
  connect(&Update, &QTimer::timeout, this, &ComparisonGraph::requestView);
  for (auto *Bar : {horizontalScrollBar(), verticalScrollBar()})
    connect(Bar, &QScrollBar::valueChanged, this, [this] {
      viewport()->update();
      Update.start();
    });
  connect(&Theme::instance(), &Theme::changed, this,
          [this] { viewport()->update(); });
}
void ComparisonGraph::showFunction(const QString &Function) {
  ++Generation;
  ++ViewGeneration;
  Nodes = {};
  Edges = {};
  Bounds = {};
  FitView = true;
  Address = Function;
  Selected.clear();
  LayoutRevision.clear();
  Message =
      Address.isEmpty() ? tr("No counterpart") : tr("Laying out control flow…");
  viewport()->update();
  if (Address.isEmpty())
    return;
  const auto Serial = Generation;
  const QFontMetricsF Font(Theme::instance().codeFont());
  Session.request(
      "comparison_cfg_summary",
      {{"side", Side},
       {"address", Address},
       {"metrics", QJsonObject{{"char_width", Font.horizontalAdvance('M')},
                               {"line_height", Font.lineSpacing()},
                               {"padding", 6},
                               {"title_height", Font.lineSpacing()}}}},
      this, [this, Serial](const auto &Reply) {
        if (Serial != Generation)
          return;
        if (Reply["status"] != QJsonValue("ok")) {
          Message = Reply["error"].toObject()["message"].toString();
          emit statusChanged(Message);
          viewport()->update();
          return;
        }
        const auto Result = Reply["payload"].toObject();
        Bounds = jsonRect(Result["bounds"].toObject());
        LayoutRevision = Result["layout_revision"].toString();
        Message.clear();
        emit statusChanged(Result["analysis_complete"].toBool()
                               ? tr("Control-flow graph")
                               : tr("Incomplete control flow"));
        fit();
      });
}
void ComparisonGraph::scrollRanges() {
  horizontalScrollBar()->setPageStep(viewport()->width());
  verticalScrollBar()->setPageStep(viewport()->height());
  horizontalScrollBar()->setRange(
      0, qMax(0, int(std::ceil(Bounds.right() * Scale)) - viewport()->width()));
  verticalScrollBar()->setRange(
      0,
      qMax(0, int(std::ceil(Bounds.bottom() * Scale)) - viewport()->height()));
}
void ComparisonGraph::fit() {
  FitView = true;
  if (Bounds.isEmpty())
    return;
  Scale = std::clamp(qMin(viewport()->width() / Bounds.width(),
                          viewport()->height() / Bounds.height()),
                     limits::kComparisonMinimumZoom, 1.0);
  scrollRanges();
  horizontalScrollBar()->setValue(0);
  verticalScrollBar()->setValue(0);
  requestView();
}
void ComparisonGraph::requestView() {
  if (LayoutRevision.isEmpty() || Session.busy())
    return;
  const auto Serial = ++ViewGeneration;
  Nodes = {};
  Edges = {};
  fetchView({{"side", Side},
             {"address", Address},
             {"layout_revision", LayoutRevision},
             {"x", horizontalScrollBar()->value() / Scale},
             {"y", verticalScrollBar()->value() / Scale},
             {"width", viewport()->width() / Scale},
             {"height", viewport()->height() / Scale},
             {"scale", Scale}},
            Serial);
}
void ComparisonGraph::fetchView(QJsonObject Query, quint64 Serial) {
  Session.request(
      "comparison_cfg_viewport", Query, this,
      [this, Query, Serial](const auto &Reply) mutable {
        if (Serial != ViewGeneration)
          return;
        if (Reply["status"] != QJsonValue("ok")) {
          Message = Reply["error"].toObject()["message"].toString();
          viewport()->update();
          return;
        }
        const auto Result = Reply["payload"].toObject();
        for (const auto &N : Result["nodes"].toArray())
          Nodes.push_back(N);
        for (const auto &E : Result["edges"].toArray())
          Edges.push_back(E);
        viewport()->update();
        if (!Result["complete"].toBool()) {
          if (Nodes.size() >= limits::kComparisonGraphVisibleNodes ||
              Edges.size() >= limits::kComparisonGraphVisibleEdges) {
            emit statusChanged(tr("Viewport limit reached — zoom in to inspect "
                                  "remaining blocks"));
            return;
          }
          Query["node_offset"] = Result["next_node_offset"].isNull()
                                     ? Result["visible_node_count"]
                                     : Result["next_node_offset"];
          Query["edge_offset"] = Result["next_edge_offset"].isNull()
                                     ? Result["visible_edge_count"]
                                     : Result["next_edge_offset"];
          fetchView(Query, Serial);
        }
      });
}
void ComparisonGraph::focusBlock(const QString &Block) {
  if (Address.isEmpty() || Block.isEmpty())
    return;
  const auto Serial = Generation;
  Session.request(
      "comparison_cfg_focus",
      {{"side", Side}, {"address", Address}, {"block", Block}}, this,
      [this, Block, Serial](const auto &Reply) {
        if (Serial != Generation || Reply["status"] != QJsonValue("ok"))
          return;
        const auto Box = jsonRect(Reply["payload"].toObject());
        Selected = Block;
        FitView = false;
        Scale = qMax(Scale, 0.75);
        scrollRanges();
        horizontalScrollBar()->setValue(
            int(Box.center().x() * Scale - viewport()->width() / 2));
        verticalScrollBar()->setValue(
            int(Box.center().y() * Scale - viewport()->height() / 2));
        requestView();
      });
}
void ComparisonGraph::paintEvent(QPaintEvent *) {
  QPainter P(viewport());
  const auto &Theme = Theme::instance();
  P.fillRect(viewport()->rect(), Theme.chrome("Base"));
  if (!Message.isEmpty()) {
    P.setPen(Theme.chrome("Text"));
    P.drawText(viewport()->rect().adjusted(12, 12, -12, -12),
               Qt::AlignCenter | Qt::TextWordWrap, Message);
    return;
  }
  P.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
  P.scale(Scale, Scale);
  P.setRenderHint(QPainter::Antialiasing);
  for (const auto &Value : Edges) {
    const auto Edge = Value.toObject();
    const auto Points = Edge["points"].toArray();
    if (Points.isEmpty())
      continue;
    const auto Point = [](const QJsonValue &V) {
      const auto O = V.toObject();
      return QPointF(O["x"].toDouble(), O["y"].toDouble());
    };
    QPainterPath Path(Point(Points.first()));
    for (qsizetype I = 1; I < Points.size(); ++I)
      Path.lineTo(Point(Points[I]));
    P.setPen(QPen(Theme.chrome("Link"), 1 / Scale));
    P.drawPath(Path);
  }
  P.setFont(Theme.codeFont());
  for (const auto &Value : Nodes) {
    const auto Node = Value.toObject();
    const auto Box = jsonRect(Node);
    const auto Evidence = Node["comparison"].toObject();
    const auto State = Evidence["state"].toString();
    P.setBrush(Theme.color(ColorRole::GraphNodeBody));
    const auto Border =
        Selected == Node["address"].toString()
            ? Theme.color(ColorRole::GraphNodeBorderSelected)
        : State == "changed" ? Theme.color(ColorRole::DiffChanged)
        : State == "unmatched"
            ? Theme.color(Side == "primary" ? ColorRole::DiffRemoved
                                            : ColorRole::DiffAdded)
            : Theme.color(ColorRole::GraphNodeBorder);
    P.setPen(QPen(Border, 1 / Scale));
    P.drawRect(Box);
    if (Scale < 0.25)
      continue;
    P.setPen(Theme.chrome("Text"));
    QString Text = Node["address"].toString();
    for (const auto &Line : Node["lines"].toArray())
      Text += '\n' + Line.toString();
    P.save();
    P.setClipRect(Box);
    P.drawText(Box.adjusted(6, 4, -6, -4), Qt::AlignLeft | Qt::AlignTop, Text);
    P.restore();
  }
}
void ComparisonGraph::resizeEvent(QResizeEvent *Event) {
  QAbstractScrollArea::resizeEvent(Event);
  if (FitView && isVisible())
    fit();
  else {
    scrollRanges();
    Update.start();
  }
}
void ComparisonGraph::showEvent(QShowEvent *Event) {
  QAbstractScrollArea::showEvent(Event);
  if (FitView)
    fit();
  else
    Update.start();
}
void ComparisonGraph::wheelEvent(QWheelEvent *Event) {
  if (Event->modifiers().testFlag(Qt::ControlModifier)) {
    FitView = false;
    const auto Old = Scale;
    Scale = std::clamp(Scale * std::pow(limits::kComparisonZoomStep,
                                        Event->angleDelta().y() / 120.0),
                       limits::kComparisonMinimumZoom,
                       limits::kComparisonMaximumZoom);
    const auto Point = Event->position();
    const auto X = (horizontalScrollBar()->value() + Point.x()) / Old;
    const auto Y = (verticalScrollBar()->value() + Point.y()) / Old;
    scrollRanges();
    horizontalScrollBar()->setValue(int(X * Scale - Point.x()));
    verticalScrollBar()->setValue(int(Y * Scale - Point.y()));
    Update.start();
    viewport()->update();
    Event->accept();
  } else
    QAbstractScrollArea::wheelEvent(Event);
}
void ComparisonGraph::mousePressEvent(QMouseEvent *Event) {
  setFocus();
  const QPointF Point =
      (Event->position() +
       QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value())) /
      Scale;
  for (const auto &V : Nodes) {
    const auto N = V.toObject();
    if (!jsonRect(N).contains(Point))
      continue;
    Selected = N["address"].toString();
    viewport()->update();
    emit blockSelected(Selected,
                       N["comparison"].toObject()["counterpart"].toString());
    break;
  }
  QAbstractScrollArea::mousePressEvent(Event);
}
} // namespace neverd::gui
