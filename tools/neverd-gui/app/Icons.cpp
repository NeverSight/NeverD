#include "Icons.h"

#include <QFile>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPixmapCache>
#include <QSvgRenderer>
#include <memory>

namespace neverd::gui {
namespace {
constexpr char IconPrefix[] = ":/neverd/icons/";
constexpr qreal DisabledOpacity = 0.35;

class SvgIconEngine final : public QIconEngine {
public:
  SvgIconEngine(QString name, std::shared_ptr<QSvgRenderer> renderer)
      : name_(std::move(name)), renderer_(std::move(renderer)) {}
  SvgIconEngine(QString name, QString resourcePath)
      : name_(std::move(name)), resourcePath_(std::move(resourcePath)) {}

  void paint(QPainter *painter, const QRect &rect, QIcon::Mode mode,
             QIcon::State) override {
    if (!renderer_)
      renderer_ = std::make_shared<QSvgRenderer>(resourcePath_);
    painter->save();
    if (mode == QIcon::Disabled)
      painter->setOpacity(painter->opacity() * DisabledOpacity);
    renderer_->render(painter, QRectF(rect));
    painter->restore();
  }

  QPixmap pixmap(const QSize &size, QIcon::Mode mode,
                 QIcon::State state) override {
    return scaledPixmap(size, mode, state, 1.0);
  }

  QPixmap scaledPixmap(const QSize &size, QIcon::Mode mode, QIcon::State state,
                       qreal scale) override {
    const QSize device = size * scale;
    const QString key = QStringLiteral("neverd-icon:%1:%2x%3:%4")
                            .arg(name_)
                            .arg(device.width())
                            .arg(device.height())
                            .arg(int(mode));
    QPixmap result;
    if (!QPixmapCache::find(key, &result)) {
      result = QPixmap(device);
      result.fill(Qt::transparent);
      QPainter painter(&result);
      paint(&painter, QRect(QPoint(), device), mode, state);
      painter.end();
      QPixmapCache::insert(key, result);
    }
    result.setDevicePixelRatio(scale);
    return result;
  }

  QSize actualSize(const QSize &size, QIcon::Mode, QIcon::State) override {
    return size;
  }
  QIconEngine *clone() const override {
    return renderer_ ? new SvgIconEngine(name_, renderer_)
                     : new SvgIconEngine(name_, resourcePath_);
  }
  QString key() const override { return QStringLiteral("neverd-svg"); }

private:
  QString name_;
  QString resourcePath_;
  std::shared_ptr<QSvgRenderer> renderer_;
};
} // namespace

namespace {
class WorkbenchFileIcons final : public QFileIconProvider {
public:
  QIcon icon(IconType type) const override {
    switch (type) {
    case Computer:
    case Desktop:
      return gui::icon(QStringLiteral("computer"));
    case Drive:
      return gui::icon(QStringLiteral("drive"));
    case Folder:
    case Network:
      return gui::icon(QStringLiteral("folder"));
    case File:
      return gui::icon(QStringLiteral("file"));
    case Trashcan:
      return gui::icon(QStringLiteral("local_types_delete"));
    }
    return {};
  }
  QIcon icon(const QFileInfo &info) const override {
    if (info.isRoot())
      return icon(Drive);
    if (info.isDir())
      return icon(Folder);
    return icon(File);
  }
};
} // namespace

QAbstractFileIconProvider *fileIcons() {
  static WorkbenchFileIcons provider;
  return &provider;
}

QIcon icon(const QString &name) {
  static QHash<QString, QIcon> cache;
  if (auto it = cache.constFind(name); it != cache.cend())
    return *it;
  QIcon result;
  const QString path = IconPrefix + name + QStringLiteral(".svg");
  // Bundled resources are validated with the UI assets. Parse a menu's SVG
  // only when it is painted, rather than decoding every hidden command while
  // constructing the workbench. Imported files still validate at their
  // boundary.
  if (QFile::exists(path))
    result = QIcon(new SvgIconEngine(name, path));
  cache.insert(name, result);
  return result;
}

} // namespace neverd::gui
