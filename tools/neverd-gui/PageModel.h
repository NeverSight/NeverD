#pragma once

#include <QAbstractListModel>
#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

// A bounded page window, never a copy of the full program in JavaScript.
class PageModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
  explicit PageModel(const QList<QByteArray> &roles, QObject *parent = nullptr);
  int rowCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QHash<int, QByteArray> roleNames() const override { return roles_; }
  int count() const { return paged_ ? total_ : rows_.size(); }
  void replace(const QJsonArray &rows);
  void append(const QJsonArray &rows);
  bool setPage(int offset, int total, const QJsonArray &rows);
  void resetPages();
  bool beginPageRequest(int offset);
  void pageRequestFailed(int offset);
  quint64 requestGeneration() const { return requestGeneration_; }
  Q_INVOKABLE QVariantMap get(int row) const;
signals:
  void countChanged();
  void pageRequested(int offset);

private:
  QHash<int, QByteArray> roles_;
  QList<QJsonObject> rows_;
  mutable QCache<int, QJsonArray> pages_{16};
  mutable QSet<int> requestedPages_;
  QSet<int> inFlightPages_;
  QHash<int, int> pageAttempts_;
  quint64 requestGeneration_ = 0;
  bool paged_ = false;
  int total_ = 0;
  static constexpr int PageSize = 256;
  static constexpr int MaximumRows = 4096;
  void clearPageRequests();
  void schedulePageRequest(int page, int delay);
};
