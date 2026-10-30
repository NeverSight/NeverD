//===- ComparisonModel.h - On-demand comparison rows -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded page caches for inventories and synchronized instruction rows.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONMODEL_H
#define NEVERD_GUI_COMPARISONMODEL_H

#include <QAbstractTableModel>
#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

namespace neverd::gui {
class ComparisonSession;
class ComparisonModel final : public QAbstractTableModel {
  Q_OBJECT
public:
  enum class Kind {
    Functions,
    Candidates,
    Instructions,
    Regions,
    RegionAnchors,
    FlowFunctions,
    FlowNodes,
    FlowSlice,
    FlowAccesses,
    FlowRegions,
    FlowRegionAnchors,
    FlowRegionNodes,
    ImpactFunctions,
    ImpactSites,
    Changes,
    Reviews
  };
  ComparisonModel(ComparisonSession &Session, Kind Type,
                  QObject *Parent = nullptr);
  int rowCount(const QModelIndex &Parent = {}) const override;
  int columnCount(const QModelIndex &Parent = {}) const override;
  QVariant data(const QModelIndex &Index, int Role) const override;
  QVariant headerData(int Section, Qt::Orientation Direction,
                      int Role) const override;
  void sort(int Column, Qt::SortOrder Order) override;
  void setQuery(const QJsonObject &Query);
  void clear();
  void reload();
  QJsonObject row(int Index) const;
  static QString functionNamesText(const QJsonObject &Function);
  static QString callEffectText(const QJsonObject &Effect);
  static QString changeStateText(const QString &State);
  qsizetype cachedPages() const { return Pages.size(); }

signals:
  void pageReady(const QJsonObject &Page);
  void failed(const QString &Message);

private:
  static QString parameterViewText(const QJsonObject &View);
  static QString parameterViewsText(const QJsonObject &Node);
  ComparisonSession &Session;
  Kind Type;
  QJsonObject Query;
  bool QueryActive = false;
  mutable QCache<int, QJsonArray> Pages;
  mutable QSet<int> Pending;
  quint64 Generation = 0;
  int Total = 0;
  void fetch(int Page) const;
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONMODEL_H
