#include "GraphView.h"

#include "Session.h"
#include "Theme.h"

#include <QHash>
#include <QJsonArray>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTextLine>
#include <algorithm>
#include <cmath>

namespace neverd::gui {
namespace {
constexpr qreal MaxOverviewScale = 0.5;
constexpr double NodePadding = 6;
constexpr double TitleRatio = 0.8;
constexpr double WholeGraphOrigin = -1e6;
constexpr double WholeGraphExtent = 9e8;
constexpr qreal MinimumZoom = 0.02;
constexpr qreal MaximumZoom = 4.0;
constexpr qreal WheelZoomStep = 1.15;
constexpr qreal TextPixelThreshold = 5.0;
constexpr qreal ArrowLength = 9.0;
constexpr qreal ArrowHalfWidth = 4.5;
constexpr int PanStep = 40;

QColor edgeColor(const QString &type) {
  const auto &theme = Theme::instance();
  if (type == QLatin1String("true"))
    return theme.color(ColorRole::GraphEdgeTrue);
  if (type == QLatin1String("false"))
    return theme.color(ColorRole::GraphEdgeFalse);
  return theme.color(ColorRole::GraphEdgeNormal);
}
} // namespace

GraphView::GraphView(Session &session, QWidget *parent)
    : QWidget(parent), session_(session) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setMouseTracking(true);
  const auto updateMetrics = [this] {
    const QFontMetricsF metrics(Theme::instance().codeFont());
    charWidth_ = metrics.horizontalAdvance(QLatin1Char('M'));
    lineHeight_ = std::ceil(metrics.lineSpacing());
    ascent_ = std::ceil(metrics.ascent());
    ++styleStamp_;
  };
  updateMetrics();
  connect(&Theme::instance(), &Theme::changed, this, [this, updateMetrics] {
    const qreal before = lineHeight_;
    updateMetrics();
    // Node sizes depend on the font: lay the graph out again.
    if (before != lineHeight_ && function_)
      showFunction(*function_, pendingCursor_);
    update();
  });
  connect(&session_, &Session::revisionChanged, this, [this] {
    if (function_)
      showFunction(*function_, pendingCursor_);
  });
  connect(&session_, &Session::generationChanged, this, [this] {
    if (function_)
      showFunction(*function_, pendingCursor_);
  });
  connect(&session_, &Session::unloaded, this, [this] {
    nodes_.clear();
    edges_.clear();
    function_.reset();
    pendingCursor_.reset();
    waiting_ = false;
    update();
    emit viewChanged();
  });
}

QJsonObject GraphView::metrics() const {
  return {{"char_width", charWidth_},
          {"line_height", lineHeight_},
          {"padding", NodePadding},
          {"title_height", std::round(lineHeight_ * TitleRatio)}};
}

void GraphView::showFunction(Address function, std::optional<Address> cursor) {
  session_.cancelAnalysisReads(this);
  const bool sameFunction = function_ == function && !nodes_.isEmpty();
  function_ = function;
  pendingCursor_ = cursor ? cursor : std::optional<Address>(function);
  if (!sameFunction) {
    // These nodes belong to the previous function. Retaining them while the
    // new layout loads lets a later jump appear satisfied by the wrong graph,
    // leaving the outstanding request to overwrite that navigation.
    nodes_.clear();
    edges_.clear();
    bounds_ = {};
    cursorNode_ = -1;
    layoutRevision_.clear();
    fitPending_ = true;
  }
  const quint64 serial = ++serial_;
  waiting_ = true;
  emit statusChanged(tr("Laying out graph…"));
  update();
  emit viewChanged();
  request(0, 0, {}, serial);
}

void GraphView::request(int nodeOffset, int edgeOffset, const QString &layout,
                        quint64 serial) {
  if (!function_)
    return;
  QJsonObject payload{{"address", hexAddress(*function_)},
                      {"metrics", metrics()},
                      {"x", WholeGraphOrigin},
                      {"y", WholeGraphOrigin},
                      {"width", WholeGraphExtent},
                      {"height", WholeGraphExtent},
                      {"scale", 1},
                      {"node_offset", nodeOffset},
                      {"edge_offset", edgeOffset}};
  if (!layout.isEmpty())
    payload["layout_revision"] = layout;
  auto &queries = session_.analysisQueries(payload, this);
  queries.graphViewport(
      payload, this, [this, serial](const QJsonObject &response) {
        if (serial != serial_)
          return;
        if (response.value("status").toString() != QLatin1String("ok")) {
          const auto error = response.value("error").toObject();
          const auto code = error.value("code").toString();
          // No layout follows these, so the view stops waiting for one.
          if (code == QLatin1String("session_changed") ||
              code == QLatin1String("worker_stopped") ||
              response.value("status").toString() ==
                  QLatin1String("cancelled")) {
            waiting_ = false;
            update();
            emit viewChanged();
            return;
          }
          nodes_.clear();
          edges_.clear();
          waiting_ = false;
          update();
          emit viewChanged();
          emit statusChanged(error.value("message").toString(code));
          return;
        }
        accept(response.value("payload").toObject(), serial);
      });
}

void GraphView::accept(const QJsonObject &payload, quint64 serial) {
  const auto summary = payload.value("summary").toObject();
  const auto viewport = payload.value("viewport").toObject();
  const QString layout = summary.value("layout_revision").toString();
  const bool firstPage = viewport.value("node_offset").toInt() == 0 &&
                         viewport.value("edge_offset").toInt() == 0;
  if (firstPage) {
    nodes_.clear();
    edges_.clear();
    waiting_ = false;
    layoutRevision_ = layout;
    const auto bounds = summary.value("bounds").toObject();
    bounds_ = QRectF(bounds.value("x").toDouble(), bounds.value("y").toDouble(),
                     bounds.value("width").toDouble(),
                     bounds.value("height").toDouble());
  } else if (layout != layoutRevision_) {
    return;
  }
  QHash<QString, int> byId;
  for (int i = 0; i < nodes_.size(); ++i)
    byId.insert(nodes_[i].id, i);
  for (const auto &value : viewport.value("nodes").toArray()) {
    const auto object = value.toObject();
    Node node;
    node.id = object.value("id").toString();
    node.start = addressValue(object.value("start")).value_or(0);
    node.end = addressValue(object.value("end")).value_or(node.start);
    node.box = QRectF(
        object.value("x").toDouble(), object.value("y").toDouble(),
        object.value("width").toDouble(), object.value("height").toDouble());
    for (const auto &rowValue : object.value("rows").toArray()) {
      const auto rowObject = rowValue.toObject();
      Row row;
      row.address =
          addressValue(rowObject.value("address")).value_or(node.start);
      row.kind = rowObject.value("kind").toString();
      row.styled.setFromWorker(rowObject.value("text").toString(),
                               rowObject.value("spans").toArray());
      node.rows.append(std::move(row));
    }
    byId.insert(node.id, int(nodes_.size()));
    nodes_.append(std::move(node));
  }
  for (const auto &value : viewport.value("edges").toArray()) {
    const auto object = value.toObject();
    Edge edge;
    edge.type = object.value("type").toString();
    edge.from = byId.value(object.value("from").toString(), -1);
    edge.to = byId.value(object.value("to").toString(), -1);
    for (const auto &point : object.value("points").toArray()) {
      const auto p = point.toObject();
      edge.points.append(
          QPointF(p.value("x").toDouble(), p.value("y").toDouble()));
    }
    if (edge.points.size() < 2)
      continue;
    QPolygonF polygon(edge.points);
    edge.box = polygon.boundingRect().adjusted(-ArrowLength, -ArrowLength,
                                               ArrowLength, ArrowLength);
    edges_.append(std::move(edge));
  }
  if (!viewport.value("complete").toBool(true)) {
    const auto nextNodes = viewport.value("next_node_offset");
    const auto nextEdges = viewport.value("next_edge_offset");
    request(nextNodes.isNull() ? viewport.value("visible_node_count").toInt()
                               : nextNodes.toInt(),
            nextEdges.isNull() ? viewport.value("visible_edge_count").toInt()
                               : nextEdges.toInt(),
            layout, serial);
    return;
  }
  // Edges whose endpoints arrived on a later page.
  for (auto &edge : edges_)
    if (edge.from < 0 || edge.to < 0)
      edge.type = edge.type; // Kept for drawing; endpoints are geometric.
  cursorNode_ = -1;
  if (pendingCursor_)
    setCursorAddress(*pendingCursor_);
  if (fitPending_) {
    fitPending_ = false;
    // Start at 100% on the entry block like a classic graph view; very large
    // graphs that cannot show their entry legibly still start there.
    scale_ = 1;
    int entry = -1;
    for (int i = 0; i < nodes_.size(); ++i)
      if (function_ && nodes_[i].start == *function_)
        entry = i;
    if (entry >= 0) {
      const auto &box = nodes_[entry].box;
      origin_ = QPointF(box.center().x() - width() / 2.0 / scale_,
                        box.top() - 40 / scale_);
    }
  }
  ensureCursorVisible();
  emit statusChanged({});
  emit viewChanged();
  update();
}

std::optional<Address> GraphView::currentAddress() const {
  if (cursorNode_ < 0 || cursorNode_ >= nodes_.size())
    return std::nullopt;
  const auto &node = nodes_[cursorNode_];
  if (cursorRow_ >= 0 && cursorRow_ < node.rows.size())
    return node.rows[cursorRow_].address;
  return node.start;
}

QString GraphView::currentToken() const {
  if (cursorNode_ < 0 || cursorNode_ >= nodes_.size())
    return {};
  const auto &node = nodes_[cursorNode_];
  if (cursorRow_ < 0 || cursorRow_ >= node.rows.size())
    return {};
  return node.rows[cursorRow_].styled.tokenAt(cursorColumn_);
}

QString GraphView::currentRowText() const {
  if (cursorNode_ < 0 || cursorNode_ >= nodes_.size())
    return {};
  const auto &node = nodes_[cursorNode_];
  if (cursorRow_ < 0 || cursorRow_ >= node.rows.size())
    return {};
  return node.rows[cursorRow_].styled.text;
}

std::optional<Address> GraphView::operandTarget() const {
  if (cursorNode_ < 0 || cursorNode_ >= nodes_.size())
    return std::nullopt;
  const auto &node = nodes_[cursorNode_];
  if (cursorRow_ < 0 || cursorRow_ >= node.rows.size())
    return std::nullopt;
  const auto &styled = node.rows[cursorRow_].styled;
  for (int column : {cursorColumn_, cursorColumn_ - 1})
    if (const auto *span = styled.spanAt(column); span && span->address)
      return span->address;
  for (const auto &span : styled.spans)
    if (span.address && *span.address != node.rows[cursorRow_].address)
      return span.address;
  return std::nullopt;
}

bool GraphView::setCursorAddress(Address address) {
  for (int n = 0; n < nodes_.size(); ++n) {
    const auto &node = nodes_[n];
    if (address < node.start || address >= std::max(node.end, node.start + 1))
      continue;
    int best = 0;
    for (int r = 0; r < node.rows.size(); ++r)
      if (node.rows[r].address <= address &&
          node.rows[r].kind == QLatin1String("insn"))
        best = r;
    cursorNode_ = n;
    cursorRow_ = best;
    cursorColumn_ = 0;
    pendingCursor_ = address;
    ensureCursorVisible();
    update();
    return true;
  }
  return false;
}

QPointF GraphView::toScene(QPointF widget) const {
  return origin_ + widget / scale_;
}
QPointF GraphView::toWidget(QPointF scene) const {
  return (scene - origin_) * scale_;
}
QRectF GraphView::visibleScene() const {
  return QRectF(origin_, QSizeF(width() / scale_, height() / scale_));
}

void GraphView::centerOn(QPointF scene) {
  origin_ = scene - QPointF(width() / 2.0 / scale_, height() / 2.0 / scale_);
  update();
  emit viewChanged();
}

void GraphView::setZoom(qreal scale, std::optional<QPointF> anchor) {
  scale = std::clamp(scale, MinimumZoom, MaximumZoom);
  const QPointF fixed =
      anchor ? *anchor : QPointF(width() / 2.0, height() / 2.0);
  const QPointF scene = toScene(fixed);
  scale_ = scale;
  origin_ = scene - fixed / scale_;
  update();
  emit viewChanged();
}

void GraphView::zoomToFit() {
  if (bounds_.isEmpty())
    return;
  const qreal fit =
      std::min(width() / bounds_.width(), height() / bounds_.height());
  scale_ = std::clamp(fit * 0.95, MinimumZoom, 1.0);
  centerOn(bounds_.center());
}

void GraphView::ensureCursorVisible() {
  if (cursorNode_ < 0 || cursorNode_ >= nodes_.size())
    return;
  const auto &node = nodes_[cursorNode_];
  const qreal rowTop = node.box.top() + NodePadding +
                       std::round(lineHeight_ * TitleRatio) +
                       std::max(0, cursorRow_) * lineHeight_;
  const QRectF row(node.box.left(), rowTop, node.box.width(), lineHeight_);
  const QRectF view = visibleScene();
  if (view.contains(row))
    return;
  QPointF target = origin_;
  if (row.left() < view.left() || row.right() > view.right())
    target.setX(row.center().x() - view.width() / 2);
  if (row.top() < view.top() || row.bottom() > view.bottom())
    target.setY(row.center().y() - view.height() / 3);
  origin_ = target;
  emit viewChanged();
}

void GraphView::moveCursor(int node, int row) {
  if (node < 0 || node >= nodes_.size())
    return;
  cursorNode_ = node;
  cursorRow_ =
      std::clamp(row, 0, std::max(0, int(nodes_[node].rows.size()) - 1));
  ensureCursorVisible();
  if (const auto address = currentAddress()) {
    pendingCursor_ = address;
    emit locationChanged(*address);
  }
  update();
}

std::pair<int, int> GraphView::hit(QPointF widget) const {
  const QPointF scene = toScene(widget);
  for (int n = int(nodes_.size()) - 1; n >= 0; --n) {
    const auto &node = nodes_[n];
    if (!node.box.contains(scene))
      continue;
    const qreal top =
        node.box.top() + NodePadding + std::round(lineHeight_ * TitleRatio);
    if (scene.y() < top)
      return {n, -1};
    const int row = int((scene.y() - top) / lineHeight_);
    return {n, std::clamp(row, 0, std::max(0, int(node.rows.size()) - 1))};
  }
  return {-1, -1};
}

int GraphView::columnAt(const Row &row, const Node &node, QPointF scene) const {
  auto &layout = row.styled.layout(
      Theme::instance().codeFont(), styleStamp_,
      [](int role) { return Theme::instance().listingRole(role); });
  if (!layout.lineCount())
    return 0;
  return layout.lineAt(0).xToCursor(scene.x() - node.box.left() - NodePadding);
}

void GraphView::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  emit viewChanged();
}

void GraphView::paintNode(QPainter &painter, int index, bool text) const {
  const auto &theme = Theme::instance();
  const auto &node = nodes_[index];
  const QRectF box = node.box;
  painter.fillRect(box.translated(3, 3),
                   theme.color(ColorRole::GraphNodeShadow));
  painter.fillRect(box, theme.color(ColorRole::GraphNodeBody));
  const qreal title = std::round(lineHeight_ * TitleRatio);
  const bool current = index == cursorNode_;
  painter.fillRect(QRectF(box.left(), box.top(), box.width(), title),
                   current ? theme.color(ColorRole::GraphNodeTitleCurrent)
                           : theme.color(ColorRole::GraphNodeTitle));
  if (!text) {
    // Low zoom: a bar per row stands in for its text.
    const qreal top = box.top() + NodePadding + title;
    QColor bar = theme.color(ColorRole::ListingPlain);
    bar.setAlphaF(0.35);
    for (int r = 0; r < node.rows.size(); ++r) {
      const qreal width =
          std::min(box.width() - 2 * NodePadding,
                   node.rows[r].styled.text.trimmed().size() * charWidth_);
      painter.fillRect(QRectF(box.left() + NodePadding,
                              top + r * lineHeight_ + lineHeight_ * 0.25, width,
                              lineHeight_ * 0.5),
                       bar);
    }
  } else {
    const QFont font = Theme::instance().codeFont();
    const auto colorOf = [&theme](int role) { return theme.listingRole(role); };
    const qreal top = box.top() + NodePadding + title;
    for (int r = 0; r < node.rows.size(); ++r) {
      const auto &row = node.rows[r];
      const qreal y = top + r * lineHeight_;
      if (current && r == cursorRow_)
        painter.fillRect(
            QRectF(box.left() + 1, y, box.width() - 2, lineHeight_),
            theme.color(ColorRole::ListingCurrentLine));
      auto &layout = row.styled.layout(font, styleStamp_, colorOf);
      const qreal x = box.left() + NodePadding;
      if (!highlight_.isEmpty() && layout.lineCount()) {
        const QTextLine line = layout.lineAt(0);
        for (const int at : tokenOccurrences(row.styled.text, highlight_)) {
          const qreal x0 = line.cursorToX(at);
          const qreal x1 = line.cursorToX(at + int(highlight_.size()));
          painter.fillRect(QRectF(x + x0, y, x1 - x0, lineHeight_),
                           theme.color(ColorRole::ListingHighlight));
        }
      }
      layout.draw(&painter, QPointF(x, y));
    }
  }
  QPen border(current ? theme.color(ColorRole::GraphNodeBorderSelected)
                      : theme.color(ColorRole::GraphNodeBorder));
  border.setCosmetic(true);
  border.setWidthF(current ? 2.0 : 1.0);
  painter.setPen(border);
  painter.setBrush(Qt::NoBrush);
  painter.drawRect(box);
}

void GraphView::drawWaiting(QPainter &painter, const QRect &area) const {
  painter.save();
  painter.setFont(font());
  painter.setPen(Theme::instance().chrome(QStringLiteral("PlaceholderText")));
  painter.drawText(area, Qt::AlignCenter, tr("Laying out graph…"));
  painter.restore();
}

void GraphView::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  const auto &theme = Theme::instance();
  painter.fillRect(rect(), theme.color(ColorRole::GraphBackground));
  if (nodes_.isEmpty()) {
    if (waiting_)
      drawWaiting(painter, rect());
    return;
  }
  const QRectF view = visibleScene();
  painter.save();
  painter.scale(scale_, scale_);
  painter.translate(-origin_);
  painter.setRenderHint(QPainter::Antialiasing, scale_ < 1.0);
  // Edges first, nodes on top.  Edges touching the current node draw last.
  const auto drawEdge = [&](const Edge &edge, bool highlighted) {
    if (!edge.box.intersects(view))
      return;
    QPen pen(highlighted ? theme.color(ColorRole::GraphEdgeHighlight)
                         : edgeColor(edge.type));
    pen.setCosmetic(true);
    pen.setWidthF(highlighted ? 2.0 : 1.2);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(edge.points.constData(), int(edge.points.size()));
    const QPointF tip = edge.points.back();
    const QPointF before = edge.points[edge.points.size() - 2];
    QPointF direction = tip - before;
    const qreal length = std::hypot(direction.x(), direction.y());
    if (length <= 0)
      return;
    direction /= length;
    const QPointF normal(-direction.y(), direction.x());
    const qreal arrow = ArrowLength / std::max<qreal>(scale_, 0.5);
    const qreal half = ArrowHalfWidth / std::max<qreal>(scale_, 0.5);
    const QPointF head[] = {tip, tip - direction * arrow + normal * half,
                            tip - direction * arrow - normal * half};
    painter.setPen(Qt::NoPen);
    painter.setBrush(pen.color());
    painter.drawPolygon(head, 3);
  };
  for (const auto &edge : edges_)
    if (edge.from != cursorNode_ && edge.to != cursorNode_)
      drawEdge(edge, false);
  for (const auto &edge : edges_)
    if (edge.from == cursorNode_ || edge.to == cursorNode_)
      drawEdge(edge, true);
  const bool text = scale_ * lineHeight_ >= TextPixelThreshold;
  for (int i = 0; i < nodes_.size(); ++i)
    if (nodes_[i].box.adjusted(-4, -4, 4, 4).intersects(view))
      paintNode(painter, i, text);
  painter.restore();
  if (cursorNode_ >= 0 && hasFocus()) {
    // Caret in widget coordinates so it stays crisp at any zoom.
    const auto &node = nodes_[cursorNode_];
    if (cursorRow_ >= 0 && cursorRow_ < node.rows.size() && text) {
      auto &layout = node.rows[cursorRow_].styled.layout(
          theme.codeFont(), styleStamp_,
          [&theme](int role) { return theme.listingRole(role); });
      if (layout.lineCount()) {
        const qreal x = node.box.left() + NodePadding +
                        layout.lineAt(0).cursorToX(cursorColumn_);
        const qreal y = node.box.top() + NodePadding +
                        std::round(lineHeight_ * TitleRatio) +
                        cursorRow_ * lineHeight_;
        const QPointF a = toWidget(QPointF(x, y));
        const QPointF b = toWidget(QPointF(x, y + lineHeight_));
        painter.fillRect(QRectF(a.x(), a.y() + 1, 2, b.y() - a.y() - 2),
                         theme.color(ColorRole::ListingCursor));
      }
    }
  }
}

void GraphView::wheelEvent(QWheelEvent *event) {
  const QPoint angle = event->angleDelta();
  if (event->modifiers() & Qt::ControlModifier) {
    const qreal factor = std::pow(WheelZoomStep, angle.y() / 120.0);
    setZoom(scale_ * factor, event->position());
    return;
  }
  const bool horizontal = event->modifiers() & Qt::ShiftModifier;
  const QPointF pixels = !event->pixelDelta().isNull()
                             ? QPointF(event->pixelDelta())
                             : QPointF(angle) / 120.0 * PanStep * 3;
  origin_ -= (horizontal ? QPointF(pixels.y(), pixels.x()) : pixels) / scale_;
  update();
  emit viewChanged();
}

void GraphView::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  const auto [node, row] = hit(event->position());
  if (event->button() == Qt::MiddleButton ||
      (event->button() == Qt::LeftButton && node < 0)) {
    dragging_ = true;
    dragStart_ = event->position();
    dragOrigin_ = origin_;
    setCursor(Qt::ClosedHandCursor);
    return;
  }
  if (node >= 0 && (event->button() == Qt::LeftButton ||
                    event->button() == Qt::RightButton)) {
    cursorNode_ = node;
    cursorRow_ = std::max(0, row);
    if (row >= 0 && row < nodes_[node].rows.size()) {
      cursorColumn_ = columnAt(nodes_[node].rows[row], nodes_[node],
                               toScene(event->position()));
      if (event->button() == Qt::LeftButton)
        highlight_ = nodes_[node].rows[row].styled.tokenAt(cursorColumn_);
    }
    if (const auto address = currentAddress()) {
      pendingCursor_ = address;
      emit locationChanged(*address);
    }
    update();
  }
}

void GraphView::mouseMoveEvent(QMouseEvent *event) {
  if (dragging_) {
    origin_ = dragOrigin_ - (event->position() - dragStart_) / scale_;
    update();
    emit viewChanged();
  }
}

void GraphView::mouseReleaseEvent(QMouseEvent *) {
  if (dragging_) {
    dragging_ = false;
    unsetCursor();
  }
}

void GraphView::mouseDoubleClickEvent(QMouseEvent *event) {
  const auto [node, row] = hit(event->position());
  if (node < 0)
    return;
  if (const auto target = operandTarget())
    emit navigateRequested(*target);
}

void GraphView::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
  case Qt::Key_Up:
    if (cursorNode_ >= 0) {
      if (cursorRow_ > 0)
        moveCursor(cursorNode_, cursorRow_ - 1);
      else if (cursorNode_ > 0)
        moveCursor(cursorNode_ - 1,
                   int(nodes_[cursorNode_ - 1].rows.size()) - 1);
    }
    return;
  case Qt::Key_Down:
    if (cursorNode_ >= 0) {
      if (cursorRow_ + 1 < nodes_[cursorNode_].rows.size())
        moveCursor(cursorNode_, cursorRow_ + 1);
      else if (cursorNode_ + 1 < nodes_.size())
        moveCursor(cursorNode_ + 1, 0);
    }
    return;
  case Qt::Key_Left:
    cursorColumn_ = std::max(0, cursorColumn_ - 1);
    update();
    return;
  case Qt::Key_Right:
    ++cursorColumn_;
    update();
    return;
  case Qt::Key_PageUp:
    origin_.ry() -= height() * 0.9 / scale_;
    update();
    emit viewChanged();
    return;
  case Qt::Key_PageDown:
    origin_.ry() += height() * 0.9 / scale_;
    update();
    emit viewChanged();
    return;
  case Qt::Key_1:
    setZoom(1.0);
    ensureCursorVisible();
    return;
  case Qt::Key_W:
    zoomToFit();
    return;
  case Qt::Key_Plus:
  case Qt::Key_Equal:
    setZoom(scale_ * WheelZoomStep);
    return;
  case Qt::Key_Minus:
    setZoom(scale_ / WheelZoomStep);
    return;
  case Qt::Key_Home:
    for (int i = 0; i < nodes_.size(); ++i)
      if (function_ && nodes_[i].start == *function_) {
        moveCursor(i, 0);
        break;
      }
    return;
  default:
    QWidget::keyPressEvent(event);
  }
}

void GraphView::contextMenuEvent(QContextMenuEvent *event) {
  emit contextMenuRequested(event->globalPos());
}

//===----------------------------------------------------------------------===//
// GraphOverview
//===----------------------------------------------------------------------===//

GraphOverview::GraphOverview(QWidget *parent) : QWidget(parent) {
  setMinimumSize(80, 60);
  setAttribute(Qt::WA_OpaquePaintEvent);
}

void GraphOverview::setGraph(GraphView *graph) {
  if (graph_)
    disconnect(graph_, nullptr, this, nullptr);
  graph_ = graph;
  if (graph_)
    connect(graph_, &GraphView::viewChanged, this,
            qOverload<>(&QWidget::update));
  update();
}

QTransform GraphOverview::transform() const {
  QTransform result;
  if (!graph_ || graph_->sceneBounds().isEmpty())
    return result;
  const QRectF bounds = graph_->sceneBounds();
  // An overview never magnifies: a small graph stays a miniature.
  const qreal scale =
      std::min({(width() - 8) / bounds.width(),
                (height() - 8) / bounds.height(), MaxOverviewScale});
  const qreal x = (width() - bounds.width() * scale) / 2;
  const qreal y = (height() - bounds.height() * scale) / 2;
  result.translate(x, y);
  result.scale(scale, scale);
  result.translate(-bounds.left(), -bounds.top());
  return result;
}

void GraphOverview::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  const auto &theme = Theme::instance();
  painter.fillRect(rect(), theme.color(ColorRole::GraphBackground));
  if (graph_ && graph_->waiting())
    graph_->drawWaiting(painter, rect());
  if (!graph_ || !graph_->loaded())
    return;
  painter.setTransform(transform());
  QPen pen(theme.color(ColorRole::GraphEdgeNormal));
  pen.setCosmetic(true);
  painter.setPen(pen);
  for (const auto &edge : graph_->edges())
    painter.drawPolyline(edge.points.constData(), int(edge.points.size()));
  // Blocks stand out as solid shapes; the visible area is an outline over
  // them, lightly tinted, so a graph that fits the view stays readable.
  painter.setPen(Qt::NoPen);
  painter.setBrush(theme.color(ColorRole::GraphOverviewNode));
  for (const auto &node : graph_->nodes())
    painter.drawRect(node.box);
  QPen frame(theme.color(ColorRole::GraphOverviewViewport));
  frame.setCosmetic(true);
  frame.setWidthF(1.5);
  painter.setPen(frame);
  QColor fill = theme.color(ColorRole::GraphOverviewViewport);
  fill.setAlphaF(0.06);
  painter.setBrush(fill);
  painter.drawRect(graph_->visibleScene().intersected(graph_->sceneBounds()));
}

void GraphOverview::mousePressEvent(QMouseEvent *event) {
  if (graph_ && graph_->loaded())
    graph_->centerOn(transform().inverted().map(event->position()));
}

void GraphOverview::mouseMoveEvent(QMouseEvent *event) {
  if ((event->buttons() & Qt::LeftButton) && graph_ && graph_->loaded())
    graph_->centerOn(transform().inverted().map(event->position()));
}

} // namespace neverd::gui
