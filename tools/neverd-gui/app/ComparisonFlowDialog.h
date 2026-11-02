//===- ComparisonFlowDialog.h - Dependency evidence and slices --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded asynchronous SSA observation review with original-code navigation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONFLOWDIALOG_H
#define NEVERD_GUI_COMPARISONFLOWDIALOG_H

#include <QDialog>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QTableView;
namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonFlowDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonFlowDialog(ComparisonSession &Session, QWidget *Parent);
signals:
  void inspectFunction(const QString &Side, const QString &Address);

private:
  ComparisonSession &Session;
  ComparisonModel *Functions, *Nodes, *Slice, *Accesses;
  QTableView *FunctionTable, *NodeTable, *SliceTable;
  QComboBox *Side;
  QCheckBox *Control;
  QPushButton *Backward, *Forward, *ArgumentAccesses, *Cancel, *Inspect;
  QLabel *Summary, *FunctionStatus, *LoopStatus, *SliceStatus;
  QString Revision, Address;
  bool Requested = false, AccessesStopped = false;
  void refresh();
  void selectSide();
  void controls();
  void slice(const QString &Direction);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONFLOWDIALOG_H
