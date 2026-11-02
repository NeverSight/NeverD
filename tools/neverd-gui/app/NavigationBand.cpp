#include "NavigationBand.h"

#include "../Config.h"
#include "../ResourceLimits.h"
#include "AddressSpace.h"
#include "Session.h"
#include "Theme.h"
#include "WorkerCodes.h"

#include <QCoreApplication>
#include <QHelpEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <cmath>

namespace neverd::gui {
namespace {

struct LegendEntry {
  AddressClass addressClass;
  const char *text;
};
constexpr LegendEntry Legend[] = {
    {AddressClass::LibraryFunction,
     QT_TRANSLATE_NOOP("NavigationBand", "Library function")},
    {AddressClass::RegularFunction,
     QT_TRANSLATE_NOOP("NavigationBand", "Regular function")},
    {AddressClass::Instruction,
     QT_TRANSLATE_NOOP("NavigationBand", "Instruction")},
    {AddressClass::Data, QT_TRANSLATE_NOOP("NavigationBand", "Data")},
    {AddressClass::Unexplored,
     QT_TRANSLATE_NOOP("NavigationBand", "Unexplored")},
    {AddressClass::External,
     QT_TRANSLATE_NOOP("NavigationBand", "External symbol")},
};
} // namespace

NavigationBand::NavigationBand(Session &session, AddressSpace &space,
                               QWidget *parent)
    : QWidget(parent), session_(session), space_(space) {
  setMouseTracking(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  refresh_.setSingleShot(true);
  refresh_.setInterval(::neverd::limits::gui::kNavigationBandRefreshDebounceMs);
  connect(&refresh_, &QTimer::timeout, this, &NavigationBand::requestOverview);
  connect(&session_, &Session::opened, this, [this] {
    // The initial listing needs this address map before it can navigate.
    refresh_.stop();
    requestOverview();
  });
  connect(&session_, &Session::generationChanged, this,
          [this] { refresh_.start(); });
  connect(&session_, &Session::revisionChanged, this,
          [this] { refresh_.start(); });
  connect(&session_, &Session::unloaded, this, [this] {
    classes_.clear();
    space_.clear();
    current_.reset();
    emit spaceChanged();
    update();
  });
  connect(&Theme::instance(), &Theme::changed, this,
          qOverload<>(&QWidget::update));
}

QSize NavigationBand::sizeHint() const {
  return {800, ::neverd::limits::gui_config::kNavigationBandBandHeight +
                   ::neverd::limits::gui_config::kNavigationBandLegendHeight +
                   ::neverd::limits::gui_config::kNavigationBandMargin * 2};
}
QSize NavigationBand::minimumSizeHint() const {
  return {120, ::neverd::limits::gui_config::kNavigationBandBandHeight +
                   ::neverd::limits::gui_config::kNavigationBandLegendHeight +
                   ::neverd::limits::gui_config::kNavigationBandMargin * 2};
}

QRect NavigationBand::bandRect() const {
  return QRect(
      ::neverd::limits::gui_config::kNavigationBandMargin,
      ::neverd::limits::gui_config::kNavigationBandMargin,
      std::max(1, width() -
                      2 * ::neverd::limits::gui_config::kNavigationBandMargin),
      ::neverd::limits::gui_config::kNavigationBandBandHeight);
}

void NavigationBand::requestOverview() {
  if (!session_.loaded())
    return;
  const quint64 serial = ++serial_;
  session_.read(
      QStringLiteral("overview"), {{"buckets", bandRect().width()}}, this,
      [this, serial](const QJsonObject &payload) {
        if (serial != serial_)
          return;
        classes_ = payload.value("buckets").toString().toLatin1();
        space_.reset(payload.value("regions").toArray());
        emit spaceChanged();
        update();
      },
      {}, QueryService::QuerySpec::Visible);
}

void NavigationBand::setCurrent(std::optional<Address> address) {
  if (address == current_)
    return;
  current_ = address;
  update();
}

std::optional<Address> NavigationBand::addressAtX(int x) const {
  const QRect band = bandRect();
  if (space_.empty() || band.width() <= 0)
    return std::nullopt;
  const long double fraction = std::clamp<long double>(
      (x - band.left()) / (long double)band.width(), 0, 1);
  return space_.addressAt(static_cast<Address>(
      fraction * std::max<Address>(1, space_.total() - 1)));
}

void NavigationBand::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  refresh_.start();
}

void NavigationBand::paintEvent(QPaintEvent *) {
  // The band lies on its toolbar's background.
  QPainter painter(this);
  const auto &theme = Theme::instance();
  const QRect band = bandRect();
  painter.fillRect(band, theme.navigationColor(0));
  if (!classes_.isEmpty()) {
    const qreal step = band.width() / qreal(classes_.size());
    // Merge runs of one class into single rectangles.
    int start = 0;
    for (int i = 1; i <= classes_.size(); ++i) {
      if (i < classes_.size() && classes_[i] == classes_[start])
        continue;
      const int cls = classes_[start] - '0';
      painter.fillRect(QRectF(band.left() + start * step, band.top(),
                              (i - start) * step, band.height()),
                       theme.navigationColor(cls));
      start = i;
    }
  }
  if (current_ && !space_.empty()) {
    const long double fraction =
        static_cast<long double>(space_.linearOf(*current_)) /
        std::max<Address>(1, space_.total());
    // On a whole pixel, so the two-pixel line keeps its full color instead
    // of smearing across three pixels.
    const qreal x = std::round(band.left() + double(fraction) * band.width());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(theme.color(ColorRole::NavCursorOutline), 1));
    painter.setBrush(theme.color(ColorRole::NavCursor));
    const QPointF arrow[] = {{x, qreal(band.top()) + 6},
                             {x - 5, qreal(band.top()) - 2},
                             {x + 5, qreal(band.top()) - 2}};
    painter.drawPolygon(arrow, 3);
    painter.fillRect(QRectF(x - 1, band.top(), 2, band.height()),
                     theme.color(ColorRole::NavCursor));
  }
  // Legend, in secondary text so the key stays quieter than the band.
  painter.setRenderHint(QPainter::Antialiasing);
  const QFontMetrics metrics(font());
  const QColor legendText = theme.chrome(QStringLiteral("SecondaryText"));
  int x = ::neverd::limits::gui_config::kNavigationBandMargin;
  const int y =
      band.bottom() + ::neverd::limits::gui_config::kNavigationBandMargin + 2;
  for (const auto &entry : Legend) {
    const QString text =
        QCoreApplication::translate("NavigationBand", entry.text);
    const QRectF swatch(
        x,
        y + (::neverd::limits::gui_config::kNavigationBandLegendHeight -
             ::neverd::limits::gui_config::kNavigationBandSwatchSize) /
                2,
        ::neverd::limits::gui_config::kNavigationBandSwatchSize,
        ::neverd::limits::gui_config::kNavigationBandSwatchSize);
    painter.setPen(Qt::NoPen);
    painter.setBrush(
        theme.navigationColor(static_cast<int>(entry.addressClass)));
    painter.drawRoundedRect(
        swatch, ::neverd::limits::gui_config::kNavigationBandSwatchRadius,
        ::neverd::limits::gui_config::kNavigationBandSwatchRadius);
    const int textLeft =
        x + ::neverd::limits::gui_config::kNavigationBandSwatchSize +
        ::neverd::limits::gui_config::kNavigationBandSwatchGap;
    painter.setPen(legendText);
    painter.drawText(
        QRect(textLeft, y, metrics.horizontalAdvance(text) + 2,
              ::neverd::limits::gui_config::kNavigationBandLegendHeight),
        Qt::AlignVCenter | Qt::AlignLeft, text);
    x = textLeft + metrics.horizontalAdvance(text) +
        ::neverd::limits::gui_config::kNavigationBandLegendEntryGap;
    if (x > width())
      break;
  }
}

void NavigationBand::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton &&
      bandRect().adjusted(0, -4, 0, 4).contains(event->pos()))
    if (const auto address = addressAtX(event->pos().x()))
      emit navigateRequested(*address);
}

void NavigationBand::mouseMoveEvent(QMouseEvent *event) {
  if ((event->buttons() & Qt::LeftButton) &&
      bandRect().adjusted(0, -4, 0, 4).contains(event->pos()))
    if (const auto address = addressAtX(event->pos().x()))
      emit navigateRequested(*address);
}

bool NavigationBand::event(QEvent *event) {
  if (event->type() == QEvent::ToolTip) {
    auto *help = static_cast<QHelpEvent *>(event);
    if (bandRect().contains(help->pos()))
      if (const auto address = addressAtX(help->pos().x())) {
        const auto *region = space_.regionOf(*address);
        QToolTip::showText(
            help->globalPos(),
            (region ? region->name + QLatin1Char(':') : QString()) +
                displayAddress(*address, session_.bitness() == 64 ? 16 : 8),
            this);
        return true;
      }
    QToolTip::hideText();
    return true;
  }
  return QWidget::event(event);
}

} // namespace neverd::gui
