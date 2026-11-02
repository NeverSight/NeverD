//===- ComparisonChangesDialog.h - Local change review ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded review leads with native attribution and independently sliced SSA.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONCHANGESDIALOG_H
#define NEVERD_GUI_COMPARISONCHANGESDIALOG_H

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableView;
class QTabWidget;
namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonChangesDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonChangesDialog(ComparisonSession &Session, QWidget *Parent);
  void review(const QString &Side, const QString &Address);
signals:
  void inspectFunction(const QString &Side, const QString &Address);
  void traceCalls(const QString &Side, const QString &Address);

private:
  ComparisonSession &Session;
  ComparisonModel *Changes, *Slice;
  QTableView *ChangeTable, *SliceTable;
  QComboBox *Context, *SliceSide;
  QPushButton *Refresh, *Cancel, *Inspect, *Calls, *Backward, *Forward;
  QLabel *Summary, *SliceStatus;
  QPlainTextEdit *Evidence;
  QTabWidget *Details;
  QString Revision, PrimaryAddress, SecondaryAddress;
  QJsonObject Query;
  void reload(bool Fresh);
  void selected();
  void controls();
  void slice(const QString &Direction);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONCHANGESDIALOG_H
