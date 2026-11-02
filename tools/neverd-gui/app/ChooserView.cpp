#include "ChooserView.h"

#include "../ResourceLimits.h"
#include "AddressSpace.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QFontInfo>
#include <QFontMetrics>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QTreeView>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace neverd::gui {
namespace {

struct ChooserSpec {
  const char *operation;
  const char *title;
  const char *icon;
  bool filterable;
  bool requested;
};
constexpr ChooserSpec Specs[] = {
#define NEVERD_CHOOSER(Id, Operation, Title, Icon, Filterable, Requested)      \
  {Operation, Title, Icon, Filterable, Requested},
#include "Choosers.def"
};

/// The digit widths a column of \p format starts at (ChooserFormats.def).
/// Addresses and hex values print in the code font, at the list's size.
bool codeFormat(ChooserModel::Format format) {
  return format == ChooserModel::Format::Address ||
         format == ChooserModel::Format::Hex ||
         format == ChooserModel::Format::SegmentAddress;
}

QFont codeColumnFont() {
  QFont font = Theme::instance().codeFont();
  font.setPointSizeF(QFontInfo(QApplication::font()).pointSizeF());
  return font;
}

int columnChars(ChooserModel::Format format) {
  switch (format) {
#define NEVERD_CHOOSER_FORMAT(Id, Columns)                                     \
  case ChooserModel::Format::Id:                                               \
    return Columns;
#include "ChooserFormats.def"
  }
  return 1;
}

struct ColumnSpec {
  ChooserKind chooser;
  const char *field;
  const char *title;
  ChooserModel::Format format;
  const char *sortField;
};
constexpr ColumnSpec ColumnSpecs[] = {
#define NEVERD_CHOOSER_COLUMN(Chooser, Field, Title, Kind, SortField)          \
  {ChooserKind::Chooser, Field, Title, ChooserModel::Format::Kind, SortField},
#include "Choosers.def"
};

QVector<ChooserModel::Column> columnsOf(ChooserKind kind) {
  QVector<ChooserModel::Column> columns;
  for (const auto &spec : ColumnSpecs)
    if (spec.chooser == kind)
      columns.append({QString::fromLatin1(spec.field), spec.title, spec.format,
                      QString::fromLatin1(spec.sortField)});
  return columns;
}

bool isLibrary(const QJsonObject &function) {
  return function.value("library").toBool() ||
         function.value("display_origin").toString() ==
             QLatin1String("recognition");
}

QString flagLetter(const QString &flags, int index, QChar letter) {
  return index < flags.size() && flags.at(index) == letter
             ? QString(letter)
             : QStringLiteral(".");
}
} // namespace

ChooserModel::ChooserModel(Session &session, const AddressSpace &space,
                           ChooserKind kind, QObject *parent)
    : QAbstractTableModel(parent), session_(session), space_(space),
      kind_(kind),
      operation_(QString::fromLatin1(Specs[static_cast<int>(kind)].operation)),
      columns_(columnsOf(kind)) {
  connect(&session_, &Session::revisionChanged, this,
          [this, epoch = session_.epoch()]() mutable {
            const bool sameProject = epoch == session_.epoch();
            epoch = session_.epoch();
            // A local display name changes neither rows nor their order.
            if (session_.queries().localNamesOnly())
              return;
            // Edits invalidate engine answers, not the visible inventory.
            // Stage fresh pages and publish only actual changes, just as for
            // background discovery. A different project discards old rows.
            if (sameProject && (kind_ == ChooserKind::Functions ||
                                kind_ == ChooserKind::Names))
              refreshFunctions();
            else
              reload();
          });
  connect(&session_, &Session::unloaded, this, &ChooserModel::reload);
  if (kind_ == ChooserKind::Functions || kind_ == ChooserKind::Names)
    connect(&session_, &Session::functionsChanged, this,
            &ChooserModel::refreshFunctions);
}

QString ChooserModel::title() const {
  return QCoreApplication::translate("Choosers",
                                     Specs[static_cast<int>(kind_)].title);
}
QString ChooserModel::iconName() const {
  return QString::fromLatin1(Specs[static_cast<int>(kind_)].icon);
}
bool ChooserModel::filterable() const {
  return Specs[static_cast<int>(kind_)].filterable;
}

void ChooserModel::setFilter(const QString &filter) {
  if (filter == filter_)
    return;
  filter_ = filter;
  reload();
}

void ChooserModel::setRequest(const QJsonObject &payload) {
  request_ = payload;
  reload();
}

void ChooserModel::setLocalRows(const QJsonArray &rows) {
  beginResetModel();
  localRows_ = true;
  local_ = rows;
  total_ = int(rows.size());
  endResetModel();
  emit totalChanged(total_);
}

void ChooserModel::addressSpaceChanged() {
  const int rows = rowCount();
  for (int column = 0; column < columnCount(); ++column)
    if (rows && (columns_.at(column).format == Format::SegmentOf ||
                 columns_.at(column).format == Format::SegmentAddress))
      emit dataChanged(index(0, column), index(rows - 1, column),
                       {Qt::DisplayRole});
}

void ChooserModel::reload() {
  refresh_.reset();
  ++locationSerial_;
  finishLocation(std::nullopt,
                 tr("The table changed before the address was selected."));
  if (localRows_) {
    // Local query results belong to one open project, just like worker pages.
    if (!session_.loaded())
      setLocalRows({});
    return;
  }
  ++serial_;
  // Old filters, sorts and revisions no longer need their queued pages.
  session_.queries().unsubscribeOwner(this);
  beginResetModel();
  pages_.clear();
  pageOrder_.clear();
  pageRequests_.clear();
  requestOrder_.clear();
  total_ = 0;
  endResetModel();
  emit totalChanged(total_);
  // A list of search results or references waits for its request.
  if (session_.loaded() && !operation_.isEmpty() &&
      (!Specs[static_cast<int>(kind_)].requested || !request_.isEmpty()))
    requestPage(0);
}

void ChooserModel::refreshFunctions() {
  if (!session_.loaded() || localRows_ || pages_.isEmpty()) {
    reload();
    return;
  }
  ++serial_;
  ++locationSerial_;
  finishLocation(std::nullopt,
                 tr("The table changed before the address was selected."));
  session_.queries().unsubscribeOwner(this);
  refresh_.emplace();
  // Include the viewport's outstanding pages first. Keep the staging set
  // within the same page allowance as the ordinary cache.
  for (const int page : requestOrder_)
    refresh_->pending.append(page);
  for (const int page : pageOrder_)
    if (!refresh_->pending.contains(page) &&
        refresh_->pending.size() <
            ::neverd::limits::gui::kChooserViewMaxCachedPages)
      refresh_->pending.append(page);
  pageRequests_.clear();
  requestOrder_.clear();
  refreshNextPage(serial_);
}

void ChooserModel::refreshNextPage(quint64 serial) {
  if (serial != serial_ || !refresh_)
    return;
  if (refresh_->pending.isEmpty()) {
    publishFunctionRefresh(serial);
    return;
  }
  const int page = refresh_->pending.takeFirst();
  session_.read(
      operation_, pagePayload(page), this,
      [this, serial, page](const QJsonObject &payload) {
        if (serial != serial_ || !refresh_)
          return;
        const int total = payload.value("total").toInt(-1);
        if (total < 0 || (refresh_->total >= 0 && refresh_->total != total)) {
          // A later discovery notification will retry. Retain the old pages
          // rather than combine different inventories or loop on cached reads.
          refresh_.reset();
          emit failed(tr("Functions changed while refreshing the table."));
          return;
        }
        refresh_->total = total;
        refresh_->pages.insert(page, payload.value("items").toArray());
        refreshNextPage(serial);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial != serial_)
          return;
        refresh_.reset();
        emit failed(message);
      });
}

void ChooserModel::publishFunctionRefresh(quint64 serial) {
  const int total = refresh_->total;
  if (total == total_ && refresh_->pages == pages_) {
    refresh_.reset();
    return;
  }
  QHash<Address, int> positions;
  for (auto it = refresh_->pages.cbegin(); it != refresh_->pages.cend(); ++it)
    for (int i = 0; i < it->size(); ++i)
      if (const auto address = itemAddress(it->at(i).toObject()))
        positions.insert(
            *address,
            it.key() * ::neverd::limits::gui::kChooserViewPageSize + i);
  // Selection models materialize their persistent indexes on this signal.
  // Balance the unchanged layout before doing any asynchronous address lookup;
  // filters, navigation and user selection remain available during that read.
  emit layoutAboutToBeChanged();
  if (serial != serial_ || !refresh_) {
    emit layoutChanged();
    return;
  }
  QSet<Address> selected;
  std::optional<Address> unresolved;
  for (const auto &index : persistentIndexList())
    if (const auto address = addressAt(index.row())) {
      selected.insert(*address);
      if (!positions.contains(*address) &&
          !refresh_->missing.contains(*address))
        unresolved = address;
    }
  emit layoutChanged();
  if (serial != serial_ || !refresh_)
    return;
  if (unresolved && total >= total_) {
    seekRefreshAddress(serial, *unresolved, selected);
    return;
  }
  auto replacement = std::move(*refresh_);
  refresh_.reset();
  if (total < total_) {
    beginResetModel();
    total_ = total;
    pages_ = std::move(replacement.pages);
    endResetModel();
  } else {
    if (total > total_) {
      beginInsertRows({}, total_, total - 1);
      total_ = total;
      endInsertRows();
    }
    emit layoutAboutToBeChanged();
    if (serial != serial_) {
      emit layoutChanged();
      return;
    }
    const auto before = persistentIndexList();
    QModelIndexList after;
    for (const auto &old : before) {
      const auto address = addressAt(old.row());
      const auto at =
          address ? positions.constFind(*address) : positions.cend();
      after.append(at != positions.cend() ? index(*at, old.column())
                                          : QModelIndex());
    }
    pages_ = std::move(replacement.pages);
    changePersistentIndexList(before, after);
    emit layoutChanged();
  }
  pageOrder_.clear();
  for (auto it = pages_.cbegin(); it != pages_.cend(); ++it)
    pageOrder_.push_front(it.key());
  emit totalChanged(total_);
}

void ChooserModel::seekRefreshAddress(quint64 serial, Address address,
                                      const QSet<Address> &selected) {
  if (refresh_->lookups++ >=
      ::neverd::limits::gui::kChooserViewMaxRefreshLookups) {
    refresh_.reset();
    emit failed(tr("Too many selection changes while refreshing the table."));
    return;
  }
  auto payload = pagePayload(0);
  payload["seek_address"] = hexAddress(address);
  session_.read(
      operation_, payload, this,
      [this, serial, address, selected](const QJsonObject &result) {
        if (serial != serial_ || !refresh_)
          return;
        const int offset = result.value("offset").toInt(-1);
        const int row = result.value("selected_index").toInt(-1);
        const auto items = result.value("items").toArray();
        if (result.value("total").toInt(-1) != refresh_->total || offset < 0 ||
            offset % ::neverd::limits::gui::kChooserViewPageSize != 0 ||
            row < offset || row >= refresh_->total ||
            row - offset >= items.size() ||
            addressValue(result.value("selected_address")) != address ||
            itemAddress(items[row - offset].toObject()) != address) {
          refresh_.reset();
          emit failed(tr("Functions changed while refreshing the table."));
          return;
        }
        const int page = offset / ::neverd::limits::gui::kChooserViewPageSize;
        if (!refresh_->pages.contains(page) &&
            refresh_->pages.size() >=
                ::neverd::limits::gui::kChooserViewMaxCachedPages) {
          // Keep selected destinations within the same cache allowance. An
          // unselected old page can be fetched normally if it becomes visible.
          auto discard = refresh_->pages.end();
          for (auto it = pageOrder_.crbegin(); it != pageOrder_.crend(); ++it) {
            auto candidate = refresh_->pages.find(*it);
            if (candidate != refresh_->pages.end() &&
                std::none_of(candidate->cbegin(), candidate->cend(),
                             [&](const auto &item) {
                               const auto at = itemAddress(item.toObject());
                               return at && selected.contains(*at);
                             })) {
              discard = candidate;
              break;
            }
          }
          if (discard == refresh_->pages.end()) {
            refresh_.reset();
            emit failed(tr("Selected functions exceed the table page budget."));
            return;
          }
          refresh_->pages.erase(discard);
        }
        refresh_->pages.insert(page, items);
        publishFunctionRefresh(serial);
      },
      [this, serial, address](const QString &code, const QString &message) {
        if (serial != serial_ || !refresh_)
          return;
        if (code == QLatin1String("not_found")) {
          refresh_->missing.insert(address);
          publishFunctionRefresh(serial);
        } else {
          refresh_.reset();
          emit failed(message);
        }
      });
}

void ChooserModel::requestPage(int page) const {
  if (refresh_ || pageRequests_.contains(page) || !session_.loaded() ||
      operation_.isEmpty())
    return;
  // Fast scrolling replaces older viewport requests instead of filling the
  // shared dispatcher. The latest pages leave room for other views and edits.
  while (int(requestOrder_.size()) >=
         ::neverd::limits::gui::kChooserViewMaxPendingPages) {
    const int oldPage = requestOrder_.back();
    requestOrder_.pop_back();
    session_.queries().unsubscribe(pageRequests_.take(oldPage));
  }
  const QJsonObject payload = pagePayload(page);
  const quint64 serial = serial_;
  auto *self = const_cast<ChooserModel *>(this);
  const auto subscription = session_.read(
      operation_, payload, self,
      [self, page, serial](const QJsonObject &result) {
        self->accept(page, serial, result);
      },
      [self, page, serial](const QString &code, const QString &message) {
        if (serial != self->serial_)
          return;
        self->pageRequests_.remove(page);
        self->requestOrder_.remove(page);
        emit self->failed(code == QLatin1String("analysis_pending")
                              ? tr("References are still being indexed…")
                              : message);
      });
  pageRequests_.insert(page, subscription);
  requestOrder_.push_front(page);
}

QJsonObject ChooserModel::pagePayload(int page) const {
  QJsonObject payload = request_;
  payload["offset"] = page * ::neverd::limits::gui::kChooserViewPageSize;
  payload["limit"] = ::neverd::limits::gui::kChooserViewPageSize;
  // Length order can start with multi-megabyte literals. The table needs a
  // preview; filtering and sorting still use the complete text in the worker.
  if (kind_ == ChooserKind::Strings)
    payload["text_limit"] =
        ::neverd::limits::gui::kChooserViewChooserModelRequestPageTextLimit;
  if (!filter_.isEmpty())
    payload["filter"] = filter_;
  if (!sortField_.isEmpty()) {
    payload["sort"] = sortField_;
    payload["descending"] = descending_;
  }
  return payload;
}

void ChooserModel::finishLocation(std::optional<int> row,
                                  const QString &error) {
  if (auto done = std::exchange(locationDone_, {}))
    done(row, error);
}

void ChooserModel::locateAddress(Address address, Located done) {
  finishLocation(std::nullopt,
                 tr("Another address selection took this one's place."));
  const auto request = ++locationSerial_;
  locationDone_ = std::move(done);
  if (localRows_) {
    for (int row = 0; row < local_.size(); ++row)
      if (addressAt(row) == address)
        return finishLocation(row);
    return finishLocation(std::nullopt);
  }
  for (auto page = pages_.cbegin(); page != pages_.cend(); ++page)
    for (int index = 0; index < page->size(); ++index) {
      const int row =
          page.key() * ::neverd::limits::gui::kChooserViewPageSize + index;
      if (addressAt(row) == address)
        return finishLocation(row);
    }
  QJsonObject payload = pagePayload(0);
  payload.insert(QStringLiteral("seek_address"), hexAddress(address));
  const quint64 serial = serial_;
  session_.read(
      operation_, payload, this,
      [this, serial, request, address](const QJsonObject &result) {
        if (serial != serial_ || request != locationSerial_)
          return;
        const auto selected = result.value("selected_index");
        const int offset = result.value("offset").toInt(-1);
        const int row = selected.toInt(-1);
        const auto items = result.value("items").toArray();
        const auto located = addressValue(result.value("selected_address"));
        if (!selected.isDouble() || row < offset || offset < 0 ||
            offset % ::neverd::limits::gui::kChooserViewPageSize != 0 ||
            row >= result.value("total").toInt() ||
            row - offset >= items.size() || !located ||
            itemAddress(items[row - offset].toObject()) != located ||
            (kind_ != ChooserKind::Segments && located != address)) {
          finishLocation(
              std::nullopt,
              tr("The worker did not locate the requested address."));
          return;
        }
        accept(offset / ::neverd::limits::gui::kChooserViewPageSize, serial,
               result);
        finishLocation(row);
      },
      [this, serial, request](const QString &, const QString &error) {
        if (serial == serial_ && request == locationSerial_)
          finishLocation(std::nullopt, error);
      });
}

void ChooserModel::accept(int page, quint64 serial,
                          const QJsonObject &payload) {
  if (serial != serial_)
    return;
  pageRequests_.remove(page);
  requestOrder_.remove(page);
  const auto items = payload.value("items").toArray();
  const int total =
      payload.contains("total")
          ? payload.value("total").toInt()
          : int(items.size()) +
                page * ::neverd::limits::gui::kChooserViewPageSize;
  pages_.insert(page, items);
  pageOrder_.remove(page);
  pageOrder_.push_front(page);
  while (int(pageOrder_.size()) >
         ::neverd::limits::gui::kChooserViewMaxCachedPages) {
    pages_.remove(pageOrder_.back());
    pageOrder_.pop_back();
  }
  if (total != total_) {
    if (total > total_) {
      beginInsertRows({}, total_, total - 1);
      total_ = total;
      endInsertRows();
    } else {
      beginResetModel();
      total_ = total;
      endResetModel();
    }
    emit totalChanged(total_);
  }
  const int first = page * ::neverd::limits::gui::kChooserViewPageSize;
  const int last =
      std::min(total_, first + ::neverd::limits::gui::kChooserViewPageSize) - 1;
  if (last >= first)
    emit dataChanged(index(first, 0), index(last, columnCount() - 1));
}

QJsonObject ChooserModel::rowObject(int row) const {
  if (localRows_)
    return row >= 0 && row < local_.size() ? local_.at(row).toObject()
                                           : QJsonObject();
  const int page = row / ::neverd::limits::gui::kChooserViewPageSize;
  auto it = pages_.constFind(page);
  if (it == pages_.cend())
    return {};
  const int offset = row % ::neverd::limits::gui::kChooserViewPageSize;
  return offset < it->size() ? it->at(offset).toObject() : QJsonObject();
}

std::optional<Address> ChooserModel::addressAt(int row) const {
  return itemAddress(rowObject(row));
}

std::optional<Address>
ChooserModel::itemAddress(const QJsonObject &object) const {
  if (kind_ == ChooserKind::Segments)
    return addressValue(object.value("start"));
  return addressValue(object.value("address"));
}

std::optional<Address> ChooserModel::referenceAddressAt(int row) const {
  if (kind_ == ChooserKind::StringReferences)
    return addressValue(rowObject(row).value("string_address"));
  return addressAt(row);
}

int ChooserModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : total_;
}

int ChooserModel::columnCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : int(columns_.size());
}

QString ChooserModel::display(const QJsonObject &row,
                              const Column &column) const {
  const QJsonValue value = row.value(column.field);
  const int digits = session_.bitness() == 64 ? 16 : 8;
  switch (column.format) {
  case Format::Text:
  case Format::Name:
    if (value.isDouble())
      return QString::number(value.toDouble(), 'f', 0);
    return value.toString();
  case Format::Literal: {
    QString text;
    for (const QChar c : value.toString()) {
      if (c == QLatin1Char('\n'))
        text += QStringLiteral("\\n");
      else if (c == QLatin1Char('\t'))
        text += QStringLiteral("\\t");
      else if (c == QLatin1Char('\r'))
        text += QStringLiteral("\\r");
      else if (c.unicode() < 0x20 || c.unicode() == 0x7f)
        text += QStringLiteral("\\x%1").arg(static_cast<uint>(c.unicode()), 2,
                                            16, QLatin1Char('0'));
      else
        text += c;
    }
    return text;
  }
  case Format::Address: {
    const auto address = addressValue(value);
    return address ? displayAddress(*address, digits) : QString();
  }
  case Format::SegmentAddress: {
    const auto address = addressValue(value);
    if (!address)
      return {};
    const auto *region = space_.regionOf(*address);
    return (region ? region->name : QStringLiteral("?")) + QLatin1Char(':') +
           displayAddress(*address, digits);
  }
  case Format::SegmentOf: {
    const auto address = addressValue(value);
    const auto *region = address ? space_.regionOf(*address) : nullptr;
    return region ? region->name : QString();
  }
  case Format::Hex: {
    if (value.isString())
      if (const auto parsed = addressValue(value))
        return displayAddress(*parsed, 8);
    return value.isDouble()
               ? displayAddress(static_cast<Address>(value.toDouble()), 8)
               : QString();
  }
  case Format::Decimal:
    return value.isDouble() ? QString::number(qint64(value.toDouble()))
                            : value.toString();
  case Format::FlagRead:
    return flagLetter(value.toString(), 0, QLatin1Char('R'));
  case Format::FlagWrite:
    return flagLetter(value.toString(), 1, QLatin1Char('W'));
  case Format::FlagExecute:
    return flagLetter(value.toString(), 2, QLatin1Char('X'));
  case Format::Direction: {
    const auto from = addressValue(row.value("from"));
    const auto to = addressValue(row.value("to"));
    if (!from || !to)
      return {};
    return *from < *to ? tr("Up") : *from > *to ? tr("Down") : QString();
  }
  }
  return {};
}

QVariant ChooserModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.column() >= columns_.size())
    return {};
  const int row = index.row();
  if (!localRows_ &&
      !pages_.contains(row / ::neverd::limits::gui::kChooserViewPageSize)) {
    if (role == Qt::DisplayRole) {
      // Fetch on demand; the visible rows decide which pages load.
      requestPage(row / ::neverd::limits::gui::kChooserViewPageSize);
      return index.column() == 0 ? QStringLiteral("…") : QString();
    }
    return {};
  }
  const auto object = rowObject(row);
  const auto &column = columns_[index.column()];
  switch (role) {
  case Qt::DisplayRole:
    return display(object, column);
  case Qt::ToolTipRole:
    if (kind_ == ChooserKind::Functions) {
      const auto linkage = object.value("linkage_name").toString();
      if (!linkage.isEmpty() &&
          linkage != object.value("display_name").toString())
        return linkage;
    }
    return {};
  case Qt::DecorationRole:
    if (index.column() == 0 && kind_ == ChooserKind::Functions)
      return icon(isLibrary(object) ? QStringLiteral("function_library")
                  : object.value("thunk").toBool()
                      ? QStringLiteral("imports")
                      : QStringLiteral("functions"));
    return {};
  case Qt::ForegroundRole:
    if (kind_ == ChooserKind::Functions && isLibrary(object))
      return Theme::instance().color(ColorRole::ListingLibraryName);
    return {};
  case Qt::BackgroundRole:
    // Import thunks and library code stand out, as in classic function lists.
    if (kind_ == ChooserKind::Functions) {
      if (object.value("thunk").toBool())
        return Theme::instance().color(ColorRole::ChooserThunkRow);
      if (isLibrary(object))
        return Theme::instance().color(ColorRole::ChooserLibraryRow);
    }
    return {};
  case Qt::FontRole:
    if (codeFormat(column.format))
      return codeColumnFont();
    return {};
  case Qt::TextAlignmentRole:
    if (column.format == Format::Hex || column.format == Format::Decimal)
      return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
  default:
    return {};
  }
}

QVariant ChooserModel::headerData(int section, Qt::Orientation orientation,
                                  int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole ||
      section >= columns_.size())
    return {};
  return QCoreApplication::translate("Choosers", columns_[section].title);
}

void ChooserModel::sort(int column, Qt::SortOrder order) {
  if (column < 0 || column >= columns_.size() ||
      columns_[column].sortField.isEmpty() || localRows_)
    return;
  sortField_ = columns_[column].sortField;
  descending_ = order == Qt::DescendingOrder;
  reload();
}

//===----------------------------------------------------------------------===//
// ChooserView
//===----------------------------------------------------------------------===//

ChooserView::ChooserView(Session &session, const AddressSpace &space,
                         ChooserKind kind, QWidget *parent)
    : QWidget(parent), model_(new ChooserModel(session, space, kind, this)),
      table_(new QTreeView(this)), filter_(new QLineEdit(this)),
      status_(new QLabel(this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  table_->setModel(model_);
  activateOnReturn(*table_);
  table_->setRootIsDecorated(false);
  table_->setUniformRowHeights(true);
  table_->setAllColumnsShowFocus(true);
  table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setSortingEnabled(false);
  table_->header()->setSectionsClickable(true);
  // Rows start in address order with no sort column, as the worker sends them.
  table_->header()->setSortIndicatorShown(true);
  table_->header()->setSortIndicator(-1, Qt::AscendingOrder);
  table_->header()->setStretchLastSection(true);
  table_->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  table_->setIconSize(QSize(16, 16));
  table_->installEventFilter(this);
  layout->addWidget(table_, 1);
  filter_->setObjectName(QStringLiteral("chooserFilter"));
  filter_->setPlaceholderText(tr("Quick filter"));
  filter_->setClearButtonEnabled(true);
  filter_->setVisible(false);
  filter_->installEventFilter(this);
  layout->addWidget(filter_);
  makeStatusLine(*status_);
  layout->addWidget(status_);
  filterTimer_.setSingleShot(true);
  filterTimer_.setInterval(::neverd::limits::gui::kChooserViewFilterDebounceMs);
  connect(&filterTimer_, &QTimer::timeout, this,
          [this] { model_->setFilter(filter_->text().trimmed()); });
  connect(filter_, &QLineEdit::textChanged, this,
          [this] { filterTimer_.start(); });
  connect(table_->header(), &QHeaderView::sectionClicked, this,
          [this](int section) {
            const auto order = table_->header()->sortIndicatorOrder();
            table_->header()->setSortIndicator(section, order);
            model_->sort(section, order);
          });
  connect(table_, &QTreeView::activated, this,
          [this](const QModelIndex &index) {
            if (const auto address = model_->addressAt(index.row()))
              emit activated(*address);
          });
  connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, [this](const QModelIndex &current) {
            updateStatus();
            if (const auto address = model_->addressAt(current.row()))
              emit currentChanged(*address);
          });
  connect(model_, &ChooserModel::totalChanged, this,
          [this] { updateStatus(); });
  connect(model_, &ChooserModel::failed, this,
          [this](const QString &message) { status_->setText(message); });
  // Each column but the last, which stretches, starts as wide as its
  // format needs in the font it prints in.
  const int textDigit =
      table_->fontMetrics().horizontalAdvance(QLatin1Char('0'));
  const int codeDigit =
      QFontMetrics(codeColumnFont()).horizontalAdvance(QLatin1Char('0'));
  auto *header = table_->header();
  // Titles measure with the style sheet's padding.
  header->ensurePolished();
  for (int i = 0; i + 1 < model_->columnCount(); ++i) {
    const auto format = model_->column(i).format;
    // A column is never narrower than its title.
    header->resizeSection(
        i, std::max(header->sectionSizeHint(i),
                    columnChars(format) *
                        (codeFormat(format) ? codeDigit : textDigit)));
  }
  updateStatus();
}

std::optional<Address> ChooserView::currentAddress() const {
  const auto index = table_->currentIndex();
  return index.isValid() ? model_->addressAt(index.row()) : std::nullopt;
}

std::optional<Address> ChooserView::referenceTarget() const {
  const auto index = table_->currentIndex();
  return index.isValid() ? model_->referenceAddressAt(index.row())
                         : std::nullopt;
}

QString ChooserView::selectedText() const {
  auto rows = table_->selectionModel()->selectedRows();
  if (rows.isEmpty() && table_->currentIndex().isValid())
    rows.append(table_->currentIndex());
  std::sort(rows.begin(), rows.end(),
            [](const QModelIndex &a, const QModelIndex &b) {
              return a.row() < b.row();
            });
  // Columns in the order the header shows them.
  const QHeaderView *header = table_->header();
  QStringList lines;
  for (const auto &row : rows) {
    QStringList cells;
    for (int visual = 0; visual < header->count(); ++visual)
      if (const int column = header->logicalIndex(visual);
          !header->isSectionHidden(column))
        cells.append(
            model_->index(row.row(), column).data(Qt::DisplayRole).toString());
    lines.append(cells.join(QLatin1Char('\t')));
  }
  return lines.join(QLatin1Char('\n'));
}

void ChooserView::focusFilter() {
  filter_->setVisible(true);
  filter_->setFocus(Qt::ShortcutFocusReason);
  filter_->selectAll();
}

void ChooserView::setFilterText(const QString &text) {
  filter_->setVisible(true);
  filter_->setText(text);
}

void ChooserView::revealAddress(Address address) {
  locateAddress(address, [this](std::optional<int> row, const QString &error) {
    if (!error.isEmpty())
      return;
    if (!row)
      return;
    const auto index = model_->index(*row, 0);
    table_->setCurrentIndex(index);
    table_->scrollTo(index, QAbstractItemView::PositionAtCenter);
  });
}

void ChooserView::locateAddress(Address address, ChooserModel::Located done) {
  filter_->clear();
  filterTimer_.stop();
  model_->setFilter({});
  model_->locateAddress(address, std::move(done));
}

void ChooserView::updateStatus() {
  const int total = model_->total();
  const auto current = table_->currentIndex();
  if (current.isValid())
    status_->setText(tr("Line %1 of %2").arg(current.row() + 1).arg(total));
  else
    status_->setText(total == 1 ? tr("1 item") : tr("%n items", "", total));
}

void ChooserView::enableCommand(ChooserCommand command, bool enabled) {
  const unsigned bit = 1u << unsigned(command);
  commands_ = enabled ? commands_ | bit : commands_ & ~bit;
}

QKeySequence ChooserView::commandKey(ChooserCommand command) {
#define NEVERD_IDA_CHOOSER_COMMAND(IdaAction, Shortcut, QtShortcut, Command)   \
  if (command == ChooserCommand::Command)                                      \
    return QKeySequence(QStringLiteral(QtShortcut), QKeySequence::PortableText);
#include "IdaActions.def"
  return {};
}

std::optional<ChooserCommand>
ChooserView::commandFor(const QKeySequence &key) const {
#define NEVERD_IDA_CHOOSER_COMMAND(IdaAction, Shortcut, QtShortcut, Command)   \
  if ((commands_ & (1u << unsigned(ChooserCommand::Command))) &&               \
      key == commandKey(ChooserCommand::Command))                              \
    return ChooserCommand::Command;
#include "IdaActions.def"
  return std::nullopt;
}

bool ChooserView::eventFilter(QObject *object, QEvent *event) {
  // A list window's own command takes its key before any window-wide one.
  if (object == table_ && (event->type() == QEvent::ShortcutOverride ||
                           event->type() == QEvent::KeyPress)) {
    const auto *pressed = static_cast<QKeyEvent *>(event);
    if (const auto command =
            commandFor(QKeySequence(pressed->keyCombination()))) {
      event->accept();
      if (event->type() == QEvent::KeyPress)
        if (const auto address = currentAddress())
          emit commandRequested(*command, *address);
      return true;
    }
  }
  if (event->type() != QEvent::KeyPress)
    return QWidget::eventFilter(object, event);
  auto *key = static_cast<QKeyEvent *>(event);
  if (object == filter_ && key->key() == Qt::Key_Escape) {
    filter_->clear();
    filter_->setVisible(false);
    table_->setFocus();
    return true;
  }
  if (object == filter_ &&
      (key->key() == Qt::Key_Down || key->key() == Qt::Key_Return)) {
    table_->setFocus();
    if (!table_->currentIndex().isValid() && model_->total())
      table_->setCurrentIndex(model_->index(0, 0));
    return key->key() == Qt::Key_Down;
  }
  if (object == table_) {
    if (key->matches(QKeySequence::Find)) {
      focusFilter();
      return true;
    }
    if (key->matches(QKeySequence::Copy)) {
      QApplication::clipboard()->setText(selectedText());
      return true;
    }
    // Typing starts a quick filter, as in the classic list windows.
    const QString text = key->text();
    if (model_->filterable() && !text.isEmpty() && text.at(0).isPrint() &&
        !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
      focusFilter();
      filter_->setText(filter_->text() + text);
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}

} // namespace neverd::gui
