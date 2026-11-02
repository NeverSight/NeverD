//===- X87CapabilitiesModel.h - Shared x87 inventory view ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Presents the worker's factorized capability report without deciding which
/// instruction semantics or execution profiles are supported.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_X87CAPABILITIESMODEL_H
#define NEVERD_GUI_APP_X87CAPABILITIESMODEL_H

#include <QAbstractTableModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace neverd::gui {
class X87CapabilitiesModel final : public QAbstractTableModel {
  Q_OBJECT
public:
  explicit X87CapabilitiesModel(QObject *Parent = nullptr);
  bool setReport(const QJsonObject &Report, QString &Error);
  void clear();
  void setDimensions(const QJsonObject &Dimensions);
  QStringList values(const QString &Dimension) const;
  QJsonObject form(int Row) const;
  QJsonObject capability(int Row) const;
  QJsonObject sourcePath() const;
  const QJsonObject &report() const { return Report; }
  int rowCount(const QModelIndex &Parent = {}) const override;
  int columnCount(const QModelIndex &Parent = {}) const override;
  QVariant data(const QModelIndex &Index,
                int Role = Qt::DisplayRole) const override;
  QVariant headerData(int Section, Qt::Orientation Orientation,
                      int Role = Qt::DisplayRole) const override;

private:
  void selectCells();
  QJsonObject Report, Dimensions;
  QJsonArray Forms;
  QHash<QString, QJsonObject> Cells;
};
} // namespace neverd::gui

#endif // NEVERD_GUI_APP_X87CAPABILITIESMODEL_H
