//===- LineagePageView.h - Bounded offline evidence table -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Accessible table navigation over engine-owned pages. Failed queries retain
/// the visible page and stale replies never replace a newer selection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_LINEAGEPAGEVIEW_H
#define NEVERD_GUI_LINEAGEPAGEVIEW_H

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QWidget>

class QLabel;
class QSpinBox;
class QToolButton;
class QStandardItemModel;
class QTableView;

namespace neverd::gui {
class LineageSession;
class LineagePageView final : public QWidget {
  Q_OBJECT
public:
  LineagePageView(LineageSession &Session, const QStringList &Headers,
                  const QStringList &Keys, QWidget *Parent = nullptr);
  void setQuery(QJsonObject Query, const QString &SelectedID = {},
                qint64 First = 0);
  qint64 offset() const { return Offset; }
  void clear();
  void refresh();
  QJsonObject selected() const;
  QTableView *table() const { return Table; }
  const QJsonArray &rows() const { return Rows; }

protected:
  void changeEvent(QEvent *Event) override;

signals:
  void selectionChanged(const QJsonObject &Row);
  void activated(const QJsonObject &Row);
  void pageChanged(const QJsonArray &Rows);

private:
  LineageSession &Session;
  QTableView *Table;
  QStandardItemModel *Model;
  QLabel *Status;
  QSpinBox *RowsPerPage;
  QToolButton *Previous, *Next, *Retry;
  QStringList Keys;
  QString PreferredSelection;
  QJsonObject Query;
  QJsonArray Rows;
  qint64 Offset = 0, NextOffset = -1;
  quint64 Generation = 0;
  bool Pending = false;
  void page(qint64 First);
  void buttons();
  void updateTableMinimumHeight();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_LINEAGEPAGEVIEW_H
