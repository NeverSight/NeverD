#include "PageModel.h"

#include <QTimer>

PageModel::PageModel(const QList<QByteArray> &roles, QObject *parent)
    : QAbstractListModel(parent) {
  for (int i = 0; i < roles.size(); ++i)
    roles_[Qt::UserRole + 1 + i] = roles[i];
}
int PageModel::rowCount(const QModelIndex &parent) const {
  return parent.isValid() ? 0 : count();
}
QVariant PageModel::data(const QModelIndex &index, int role) const {
  if (!index.isValid() || index.row() < 0 || index.row() >= count())
    return {};
  if (paged_) {
    const int page = index.row() / PageSize;
    if (const auto *rows = pages_.object(page))
      return rows->at(index.row() % PageSize)
          .toObject()
          .value(QString::fromUtf8(roles_.value(role)))
          .toVariant();
    if (!requestedPages_.contains(page)) {
      auto *self = const_cast<PageModel *>(this);
      self->schedulePageRequest(page, 0);
    }
    return roles_.value(role) == "size" ? QVariant(0) : QVariant(QString{});
  }
  return rows_[index.row()]
      .value(QString::fromUtf8(roles_.value(role)))
      .toVariant();
}
QVariantMap PageModel::get(int row) const {
  if (paged_) {
    if (row < 0 || row >= total_)
      return {};
    const auto *page = pages_.object(row / PageSize);
    return page ? page->at(row % PageSize).toObject().toVariantMap()
                : QVariantMap{};
  }
  return row >= 0 && row < rows_.size() ? rows_[row].toVariantMap()
                                        : QVariantMap{};
}
void PageModel::replace(const QJsonArray &rows) {
  beginResetModel();
  paged_ = false;
  pages_.clear();
  clearPageRequests();
  total_ = 0;
  rows_.clear();
  for (const auto &row : rows) {
    if (rows_.size() == MaximumRows)
      break;
    rows_.append(row.toObject());
  }
  endResetModel();
  emit countChanged();
}
void PageModel::resetPages() {
  beginResetModel();
  paged_ = true;
  total_ = 0;
  rows_.clear();
  pages_.clear();
  clearPageRequests();
  endResetModel();
  emit countChanged();
}
bool PageModel::setPage(int offset, int total, const QJsonArray &rows) {
  if (offset < 0 || offset % PageSize != 0 || total < 0 ||
      rows.size() > PageSize || offset > total)
    return false;
  if (rows.size() != qMin(PageSize, total - offset))
    return false;
  if (!paged_ || total_ != total) {
    beginResetModel();
    paged_ = true;
    total_ = total;
    rows_.clear();
    pages_.clear();
    clearPageRequests();
    pages_.insert(offset / PageSize, new QJsonArray(rows));
    endResetModel();
    emit countChanged();
  } else {
    pages_.insert(offset / PageSize, new QJsonArray(rows));
    requestedPages_.remove(offset / PageSize);
    inFlightPages_.remove(offset / PageSize);
    pageAttempts_.remove(offset / PageSize);
    if (!rows.isEmpty())
      emit dataChanged(index(offset, 0), index(offset + rows.size() - 1, 0));
  }
  return true;
}
void PageModel::clearPageRequests() {
  ++requestGeneration_;
  requestedPages_.clear();
  inFlightPages_.clear();
  pageAttempts_.clear();
}
void PageModel::schedulePageRequest(int page, int delay) {
  requestedPages_.insert(page);
  const auto generation = requestGeneration_;
  QTimer::singleShot(delay, this, [this, page, generation] {
    if (generation == requestGeneration_ && paged_ &&
        requestedPages_.contains(page) && !inFlightPages_.contains(page) &&
        !pages_.contains(page))
      emit pageRequested(page * PageSize);
  });
}
bool PageModel::beginPageRequest(int offset) {
  if (!paged_ || offset < 0 || offset % PageSize != 0 ||
      (total_ > 0 && offset >= total_))
    return false;
  const int page = offset / PageSize;
  if (inFlightPages_.contains(page))
    return false;
  requestedPages_.insert(page);
  inFlightPages_.insert(page);
  ++pageAttempts_[page];
  return true;
}
void PageModel::pageRequestFailed(int offset) {
  if (offset < 0 || offset % PageSize != 0)
    return;
  const int page = offset / PageSize;
  if (!inFlightPages_.remove(page))
    return;
  requestedPages_.remove(page);
  const auto attempts = pageAttempts_.value(page);
  // Recover transient queue pressure without letting an unavailable worker
  // spin. After three attempts, a later visible-row access or explicit request
  // can retry.
  if (attempts < 3)
    schedulePageRequest(page, 150 * (1 << (attempts - 1)));
  else
    pageAttempts_.remove(page);
}
void PageModel::append(const QJsonArray &rows) {
  if (rows.isEmpty())
    return;
  const auto count = qMin(qsizetype(MaximumRows), rows.size());
  const auto remove = qMax(qsizetype(0), rows_.size() + count - MaximumRows);
  if (remove) {
    beginRemoveRows({}, 0, remove - 1);
    rows_.erase(rows_.begin(), rows_.begin() + remove);
    endRemoveRows();
  }
  beginInsertRows({}, rows_.size(), rows_.size() + count - 1);
  for (qsizetype i = 0; i < count; ++i)
    rows_.append(rows[i].toObject());
  endInsertRows();
  emit countChanged();
}
