#pragma once

#include <QFont>
#include <QQuickItem>
#include <QVariantList>

// One scene-graph item for a bounded worker viewport, never one QML item per
// block.
class NativeGraphItem : public QQuickItem {
  Q_OBJECT
  Q_PROPERTY(QVariantList nodes READ nodes WRITE setNodes NOTIFY nodesChanged)
  Q_PROPERTY(QVariantList edges READ edges WRITE setEdges NOTIFY edgesChanged)
  Q_PROPERTY(
      qreal viewportX READ viewportX WRITE setViewportX NOTIFY viewportChanged)
  Q_PROPERTY(
      qreal viewportY READ viewportY WRITE setViewportY NOTIFY viewportChanged)
  Q_PROPERTY(qreal zoom READ zoom WRITE setZoom NOTIFY viewportChanged)
  Q_PROPERTY(QString selectedAddress READ selectedAddress WRITE
                 setSelectedAddress NOTIFY styleChanged)
  Q_PROPERTY(QFont codeFont READ codeFont WRITE setCodeFont NOTIFY styleChanged)
  Q_PROPERTY(QVariantMap graphPalette READ graphPalette WRITE setGraphPalette
                 NOTIFY styleChanged)
  Q_PROPERTY(int boundedNodeCount READ boundedNodeCount NOTIFY nodesChanged)
  Q_PROPERTY(int boundedEdgeCount READ boundedEdgeCount NOTIFY edgesChanged)
public:
  explicit NativeGraphItem(QQuickItem *parent = nullptr);
  QVariantList nodes() const { return sourceNodes_; }
  QVariantList edges() const { return sourceEdges_; }
  qreal viewportX() const { return viewportX_; }
  qreal viewportY() const { return viewportY_; }
  qreal zoom() const { return zoom_; }
  QString selectedAddress() const { return selectedAddress_; }
  QFont codeFont() const { return font_; }
  QVariantMap graphPalette() const { return palette_; }
  int boundedNodeCount() const { return nodes_.size(); }
  int boundedEdgeCount() const { return edges_.size(); }
  void setNodes(const QVariantList &nodes);
  void setEdges(const QVariantList &edges);
  void setViewportX(qreal value);
  void setViewportY(qreal value);
  void setZoom(qreal value);
  void setSelectedAddress(const QString &value);
  void setCodeFont(const QFont &value);
  void setGraphPalette(const QVariantMap &value);
  Q_INVOKABLE QString addressAt(qreal screenX, qreal screenY) const;
signals:
  void nodesChanged();
  void edgesChanged();
  void viewportChanged();
  void styleChanged();

protected:
  QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override;

private:
  struct Block {
    QString id, address, label;
    QStringList lines;
    QRectF rect;
  };
  struct Edge {
    QString type;
    QList<QPointF> points;
  };
  QVariantList sourceNodes_, sourceEdges_;
  QList<Block> nodes_;
  QList<Edge> edges_;
  qreal viewportX_ = 0, viewportY_ = 0, zoom_ = 1;
  QString selectedAddress_;
  QFont font_;
  QVariantMap palette_;
  QSGNode *updateSoftwareNode(QSGNode *oldNode);
};
