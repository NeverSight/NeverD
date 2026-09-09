#include "NativeGraphItem.h"

#include <QColor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGVertexColorMaterial>
#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {
constexpr int NodeLimit = 256;
constexpr int EdgeLimit = 512;
bool coordinate(qreal number) {
  return std::isfinite(number) && std::abs(number) <= 1e12;
}
struct LabelNode {
  QSGSimpleTextureNode *node = nullptr;
  QString signature;
};
struct GraphNode final : QSGNode {
  QSGGeometryNode *shapes = new QSGGeometryNode;
  QHash<QString, LabelNode> labels;
  GraphNode() {
    auto *geometry =
        new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    shapes->setGeometry(geometry);
    shapes->setFlag(QSGNode::OwnsGeometry);
    shapes->setMaterial(new QSGVertexColorMaterial);
    shapes->setFlag(QSGNode::OwnsMaterial);
    appendChildNode(shapes);
  }
};
using Vertex = QSGGeometry::ColoredPoint2D;
void point(QList<Vertex> &vertices, const QPointF &point, const QColor &color) {
  Vertex vertex;
  vertex.set(float(point.x()), float(point.y()), color.red(), color.green(),
             color.blue(), color.alpha());
  vertices.append(vertex);
}
void triangle(QList<Vertex> &vertices, const QPointF &a, const QPointF &b,
              const QPointF &c, const QColor &color) {
  point(vertices, a, color);
  point(vertices, b, color);
  point(vertices, c, color);
}
void rectangle(QList<Vertex> &vertices, const QRectF &rect,
               const QColor &color) {
  triangle(vertices, rect.topLeft(), rect.topRight(), rect.bottomRight(),
           color);
  triangle(vertices, rect.topLeft(), rect.bottomRight(), rect.bottomLeft(),
           color);
}
void line(QList<Vertex> &vertices, const QPointF &a, const QPointF &b,
          qreal width, const QColor &color) {
  const auto delta = b - a;
  const auto length = std::hypot(delta.x(), delta.y());
  if (length < 0.01)
    return;
  const QPointF normal(-delta.y() * width / length / 2,
                       delta.x() * width / length / 2);
  triangle(vertices, a + normal, b + normal, b - normal, color);
  triangle(vertices, a + normal, b - normal, a - normal, color);
}
void paintLabel(QPainter &painter, const QSizeF &size, const QString &label,
                const QStringList &lines, const QFont &font,
                const QColor &foreground, const QColor &labelColor,
                const QColor &border) {
  painter.setRenderHint(QPainter::TextAntialiasing);
  painter.setFont(font);
  painter.setLayoutDirection(Qt::LeftToRight);
  const QFontMetricsF metrics(font);
  const qreal textWidth = size.width() - 24;
  painter.setPen(labelColor);
  painter.drawText(QRectF(12, 7, textWidth, 24),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   metrics.elidedText(label, Qt::ElideRight, textWidth));
  painter.setPen(border);
  painter.drawLine(QPointF(12, 36), QPointF(size.width() - 12, 36));
  painter.setPen(foreground);
  const qreal lineHeight = qMax(qreal(17), metrics.height() * 1.15);
  for (int i = 0; i < lines.size() && 43 + (i + 1) * lineHeight < size.height();
       ++i)
    painter.drawText(QRectF(12, 43 + i * lineHeight, textWidth, lineHeight),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     metrics.elidedText(lines[i], Qt::ElideRight, textWidth));
}
} // namespace

NativeGraphItem::NativeGraphItem(QQuickItem *parent) : QQuickItem(parent) {
  setFlag(ItemHasContents);
  setClip(true);
  font_ = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  font_.setPointSizeF(10);
}
void NativeGraphItem::setNodes(const QVariantList &values) {
  sourceNodes_ = values.mid(0, NodeLimit);
  nodes_.clear();
  QSet<QString> ids;
  for (const auto &value : sourceNodes_) {
    const auto map = value.toMap();
    Block block;
    block.id = map.value("id").toString();
    if (block.id.isEmpty() || ids.contains(block.id))
      continue;
    const auto x = map.value("x").toDouble(), y = map.value("y").toDouble();
    const auto w = map.value("width").toDouble(),
               h = map.value("height").toDouble();
    if (!coordinate(x) || !coordinate(y) || !std::isfinite(w) ||
        !std::isfinite(h) || w <= 0 || h <= 0 || w > 4096 || h > 4096)
      continue;
    block.rect = QRectF(x, y, w, h);
    block.address = map.value("address").toString();
    block.label = map.value("label", block.address).toString().left(512);
    for (const auto &text : map.value("lines").toList().mid(0, 8))
      block.lines.append(text.toString().left(512));
    ids.insert(block.id);
    nodes_.append(std::move(block));
  }
  emit nodesChanged();
  update();
}
void NativeGraphItem::setEdges(const QVariantList &values) {
  sourceEdges_ = values.mid(0, EdgeLimit);
  edges_.clear();
  for (const auto &value : sourceEdges_) {
    const auto map = value.toMap();
    Edge edge;
    edge.type = map.value("type").toString();
    bool valid = true;
    const auto points = map.value("points").toList();
    if (points.size() < 2 || points.size() > 32)
      continue;
    for (const auto &value : points) {
      const auto item = value.toMap();
      const auto x = item.value("x").toDouble(), y = item.value("y").toDouble();
      if (!coordinate(x) || !coordinate(y)) {
        valid = false;
        break;
      }
      edge.points.append(QPointF(x, y));
    }
    if (valid)
      edges_.append(std::move(edge));
  }
  emit edgesChanged();
  update();
}
void NativeGraphItem::setViewportX(qreal value) {
  if (!coordinate(value) || value == viewportX_)
    return;
  viewportX_ = value;
  emit viewportChanged();
  update();
}
void NativeGraphItem::setViewportY(qreal value) {
  if (!coordinate(value) || value == viewportY_)
    return;
  viewportY_ = value;
  emit viewportChanged();
  update();
}
void NativeGraphItem::setZoom(qreal value) {
  if (!std::isfinite(value))
    return;
  value = qBound(qreal(0.001), value, qreal(4));
  if (value == zoom_)
    return;
  zoom_ = value;
  emit viewportChanged();
  update();
}
void NativeGraphItem::setSelectedAddress(const QString &value) {
  if (value == selectedAddress_)
    return;
  selectedAddress_ = value;
  emit styleChanged();
  update();
}
void NativeGraphItem::setCodeFont(const QFont &value) {
  if (value == font_)
    return;
  font_ = value;
  emit styleChanged();
  update();
}
void NativeGraphItem::setGraphPalette(const QVariantMap &value) {
  if (value == palette_)
    return;
  palette_ = value;
  emit styleChanged();
  update();
}
QString NativeGraphItem::addressAt(qreal x, qreal y) const {
  const QPointF position(viewportX_ + x / zoom_, viewportY_ + y / zoom_);
  for (auto it = nodes_.crbegin(); it != nodes_.crend(); ++it)
    if (it->rect.contains(position))
      return it->address;
  return {};
}

QSGNode *NativeGraphItem::updatePaintNode(QSGNode *oldNode,
                                          UpdatePaintNodeData *) {
  if (window() && window()->rendererInterface()->graphicsApi() ==
                      QSGRendererInterface::Software)
    return updateSoftwareNode(oldNode);
  auto *root = static_cast<GraphNode *>(oldNode);
  if (!root)
    root = new GraphNode;
  if (!window())
    return root;
  const auto color = [this](const char *name, const char *fallback) {
    const auto result =
        palette_.value(QString::fromLatin1(name), QString::fromLatin1(fallback))
            .value<QColor>();
    return result.isValid() ? result : QColor(QString::fromLatin1(fallback));
  };
  const auto border = color("border", "#3c3c3c"),
             body = color("body", "#252526");
  const auto focus = color("focus", "#007fd4"),
             foreground = color("foreground", "#d4d4d4");
  const auto labelColor = color("label", "#9cdcfe"),
             edgeColor = color("edge", "#569cd6");
  const auto branchColor = color("branch", "#c586c0");
  const auto project = [this](QPointF p) {
    return QPointF((p.x() - viewportX_) * zoom_, (p.y() - viewportY_) * zoom_);
  };
  const QRectF viewport(0, 0, width(), height());
  QList<Vertex> vertices;
  vertices.reserve(edges_.size() * 30 + nodes_.size() * 12);
  for (const auto &edge : edges_) {
    const auto ink = edge.type.contains("conditional") || edge.type == "true" ||
                             edge.type == "false"
                         ? branchColor
                         : edgeColor;
    for (int i = 1; i < edge.points.size(); ++i)
      line(vertices, project(edge.points[i - 1]), project(edge.points[i]), 1.4,
           ink);
    const auto tip = project(edge.points.last()),
               previous = project(edge.points[edge.points.size() - 2]);
    const auto delta = tip - previous;
    const auto length = std::hypot(delta.x(), delta.y());
    if (length > 0.01) {
      const auto direction = delta / length;
      const QPointF normal(-direction.y(), direction.x());
      triangle(vertices, tip, tip - direction * 7 + normal * 3.5,
               tip - direction * 7 - normal * 3.5, ink);
    }
  }
  QList<const Block *> visible;
  for (const auto &block : nodes_) {
    const QRectF rect(project(block.rect.topLeft()), block.rect.size() * zoom_);
    if (!rect.intersects(viewport))
      continue;
    rectangle(vertices, rect,
              block.address == selectedAddress_ ? focus : border);
    rectangle(vertices, rect.adjusted(1, 1, -1, -1), body);
    visible.append(&block);
  }
  auto *geometry = root->shapes->geometry();
  geometry->allocate(vertices.size());
  std::copy(vertices.cbegin(), vertices.cend(),
            geometry->vertexDataAsColoredPoint2D());
  root->shapes->markDirty(QSGNode::DirtyGeometry);
  QSet<QString> activeLabels;
  // Keep label textures below roughly 56 MiB even at the maximum viewport size.
  const qreal ratio =
      visible.size() <= 64
          ? qMin(qreal(2), window()->effectiveDevicePixelRatio())
          : 1;
  if (zoom_ >= 0.6)
    for (const auto *block : visible) {
      activeLabels.insert(block->id);
      auto &cached = root->labels[block->id];
      const auto signature = block->label + QChar(0x1f) +
                             block->lines.join(QChar(0x1f)) + font_.toString() +
                             foreground.name() + labelColor.name() +
                             border.name() + QString::number(ratio) +
                             QString::number(block->rect.width()) + ":" +
                             QString::number(block->rect.height());
      if (!cached.node) {
        cached.node = new QSGSimpleTextureNode;
        cached.node->setOwnsTexture(true);
        cached.node->setFiltering(QSGTexture::Linear);
        root->appendChildNode(cached.node);
      }
      if (cached.signature != signature) {
        // Worker nodes are 300x180; clamp malformed oversized nodes before
        // allocating.
        const QSize logical(qMin(300, int(std::ceil(block->rect.width()))),
                            qMin(180, int(std::ceil(block->rect.height()))));
        QImage image(QSize(qCeil(logical.width() * ratio),
                           qCeil(logical.height() * ratio)),
                     QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paintLabel(painter, logical, block->label, block->lines, font_,
                   foreground, labelColor, border);
        painter.end();
        auto *texture = window()->createTextureFromImage(image);
        // setTexture deletes the previous texture when ownsTexture is true.
        cached.node->setTexture(texture);
        cached.signature = signature;
      }
      cached.node->setRect(
          QRectF(project(block->rect.topLeft()), block->rect.size() * zoom_));
    }
  for (auto it = root->labels.begin(); it != root->labels.end();) {
    if (activeLabels.contains(it.key())) {
      ++it;
      continue;
    }
    root->removeChildNode(it->node);
    delete it->node;
    it = root->labels.erase(it);
  }
  return root;
}

QSGNode *NativeGraphItem::updateSoftwareNode(QSGNode *oldNode) {
  // Qt's raster adaptation cannot render a custom vertex-color material.
  // Paint only the viewport into one texture, with an eight-megapixel ceiling.
  if (width() <= 0 || height() <= 0) {
    delete oldNode;
    return nullptr;
  }
  auto *root = static_cast<QSGSimpleTextureNode *>(oldNode);
  if (!root) {
    root = new QSGSimpleTextureNode;
    root->setOwnsTexture(true);
    root->setFiltering(QSGTexture::Linear);
  }
  const auto color = [this](const char *name, const char *fallback) {
    const auto result =
        palette_.value(QString::fromLatin1(name), QString::fromLatin1(fallback))
            .value<QColor>();
    return result.isValid() ? result : QColor(QString::fromLatin1(fallback));
  };
  const auto border = color("border", "#3c3c3c"),
             body = color("body", "#252526");
  const auto focus = color("focus", "#007fd4"),
             foreground = color("foreground", "#d4d4d4");
  const auto labelColor = color("label", "#9cdcfe"),
             edgeColor = color("edge", "#569cd6");
  const auto branchColor = color("branch", "#c586c0");
  const auto project = [this](QPointF p) {
    return QPointF((p.x() - viewportX_) * zoom_, (p.y() - viewportY_) * zoom_);
  };
  const QRectF viewport(0, 0, width(), height());
  const auto ratio = qMin(window()->effectiveDevicePixelRatio(),
                          std::sqrt(8.0 * 1024 * 1024 / (width() * height())));
  QImage image(
      QSize(qMax(1, qCeil(width() * ratio)), qMax(1, qCeil(height() * ratio))),
      QImage::Format_ARGB32_Premultiplied);
  image.setDevicePixelRatio(ratio);
  image.fill(Qt::transparent);
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  for (const auto &edge : edges_) {
    const auto ink = edge.type.contains("conditional") || edge.type == "true" ||
                             edge.type == "false"
                         ? branchColor
                         : edgeColor;
    painter.setPen(QPen(ink, 1.4));
    for (int i = 1; i < edge.points.size(); ++i)
      painter.drawLine(project(edge.points[i - 1]), project(edge.points[i]));
    const auto tip = project(edge.points.last()),
               previous = project(edge.points[edge.points.size() - 2]);
    const auto delta = tip - previous;
    const auto length = std::hypot(delta.x(), delta.y());
    if (length > 0.01) {
      const auto direction = delta / length;
      const QPointF normal(-direction.y(), direction.x());
      painter.setPen(Qt::NoPen);
      painter.setBrush(ink);
      painter.drawPolygon(QPolygonF{tip, tip - direction * 7 + normal * 3.5,
                                    tip - direction * 7 - normal * 3.5});
    }
  }
  for (const auto &block : nodes_) {
    const QRectF rect(project(block.rect.topLeft()), block.rect.size() * zoom_);
    if (!rect.intersects(viewport))
      continue;
    painter.fillRect(rect, block.address == selectedAddress_ ? focus : border);
    painter.fillRect(rect.adjusted(1, 1, -1, -1), body);
    if (zoom_ >= 0.6) {
      painter.save();
      painter.translate(rect.topLeft());
      painter.scale(zoom_, zoom_);
      painter.setClipRect(QRectF(QPointF{}, block.rect.size()));
      paintLabel(painter, block.rect.size(), block.label, block.lines, font_,
                 foreground, labelColor, border);
      painter.restore();
    }
  }
  painter.end();
  root->setTexture(window()->createTextureFromImage(image));
  root->setRect(viewport);
  return root;
}
