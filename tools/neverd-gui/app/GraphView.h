#pragma once

#include "Address.h"
#include "StyledText.h"

#include <QJsonObject>
#include <QVector>
#include <QWidget>
#include <optional>

class QPainter;

namespace neverd::gui {

class Session;

/// The function graph: basic blocks as boxes of listing rows, laid out in
/// layers by the worker, with conditional (taken/not taken) and
/// unconditional edges.  The whole graph is fetched once; panning and zooming
/// are local and draw only what is visible, with text dropped at low zoom.
class GraphView final : public QWidget {
  Q_OBJECT
public:
  struct Row {
    Address address = 0;
    QString kind;
    StyledLine styled;
  };
  struct Node {
    QString id;
    Address start = 0, end = 0;
    QRectF box;
    QVector<Row> rows;
  };
  struct Edge {
    QString type;
    int from = -1, to = -1;
    QVector<QPointF> points;
    QRectF box;
  };

  explicit GraphView(Session &session, QWidget *parent = nullptr);

  /// Show the graph of \p function with the cursor on \p cursor.
  void showFunction(Address function, std::optional<Address> cursor = {});
  /// Move the cursor to \p address within the current graph, if present.
  bool setCursorAddress(Address address);
  std::optional<Address> function() const { return function_; }
  std::optional<Address> currentAddress() const;
  std::optional<Address> operandTarget() const;
  QString currentToken() const;
  QString currentRowText() const;
  bool loaded() const { return !nodes_.isEmpty(); }
  /// A layout is on its way and nothing is drawn yet.
  bool waiting() const { return waiting_ && nodes_.isEmpty(); }
  /// Say, in \p area, that a layout is on its way.
  void drawWaiting(QPainter &painter, const QRect &area) const;

  // Overview support.
  const QVector<Node> &nodes() const { return nodes_; }
  const QVector<Edge> &edges() const { return edges_; }
  QRectF sceneBounds() const { return bounds_; }
  QRectF visibleScene() const;
  void centerOn(QPointF scene);
  void zoomToFit();
  void setZoom(qreal scale, std::optional<QPointF> anchor = {});
  qreal zoom() const { return scale_; }

signals:
  void locationChanged(neverd::gui::Address address);
  void navigateRequested(neverd::gui::Address target);
  void contextMenuRequested(const QPoint &globalPosition);
  void viewChanged();
  void statusChanged(const QString &status);

protected:
  void paintEvent(QPaintEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

private:
  void request(int nodeOffset, int edgeOffset, const QString &layout,
               quint64 serial);
  void accept(const QJsonObject &payload, quint64 serial);
  QJsonObject metrics() const;
  QPointF toScene(QPointF widget) const;
  QPointF toWidget(QPointF scene) const;
  /// Node and row under a widget position; row -1 for the title strip.
  std::pair<int, int> hit(QPointF widget) const;
  int columnAt(const Row &row, const Node &node, QPointF scene) const;
  void ensureCursorVisible();
  void moveCursor(int node, int row);
  void paintNode(QPainter &painter, int index, bool text) const;

  Session &session_;
  QVector<Node> nodes_;
  QVector<Edge> edges_;
  QRectF bounds_;
  std::optional<Address> function_;
  // Latest requested or selected address, including while a layout is loading.
  std::optional<Address> pendingCursor_;
  QString layoutRevision_;
  quint64 serial_ = 0;
  qreal scale_ = 1;
  QPointF origin_; // Scene point at the widget's top-left.
  int cursorNode_ = -1, cursorRow_ = 0, cursorColumn_ = 0;
  QString highlight_;
  QPointF dragStart_, dragOrigin_;
  bool dragging_ = false;
  qreal charWidth_ = 8, lineHeight_ = 16, ascent_ = 12;
  quint64 styleStamp_ = 1;
  bool fitPending_ = false;
  bool waiting_ = false;
};

/// The graph overview: the whole graph in miniature with the visible area;
/// clicking or dragging moves the main graph.
class GraphOverview final : public QWidget {
  Q_OBJECT
public:
  explicit GraphOverview(QWidget *parent = nullptr);
  void setGraph(GraphView *graph);

protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;

private:
  QTransform transform() const;
  GraphView *graph_ = nullptr;
};

} // namespace neverd::gui
