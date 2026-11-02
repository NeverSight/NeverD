//===- ComparisonDialog.h - Binary comparison review window ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Paired inputs, inventories, disassembly, CFGs and analyst mapping edits.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONDIALOG_H
#define NEVERD_GUI_COMPARISONDIALOG_H

#include "ComparisonSession.h"

#include <QDialog>
#include <QPointer>
#include <QTimer>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableView;

namespace neverd::gui {
class ComparisonGraph;
class ComparisonSourceView;
class ComparisonModel;
class ComparisonRegionsDialog;
class ComparisonFlowDialog;
class ComparisonImpactDialog;
class ComparisonChangesDialog;
class ComparisonReviewDialog;
class ComparisonValidationDialog;
class ComparisonDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonDialog(const QString &Worker, const QString &PrimaryPath = {},
                   QWidget *Parent = nullptr);
  ComparisonSession &session() { return Session; }

protected:
  void closeEvent(QCloseEvent *Event) override;

private:
  ComparisonSession Session;
  ComparisonModel *Functions, *Instructions;
  ComparisonGraph *PrimaryGraph, *SecondaryGraph;
  QLineEdit *Paths[2], *Bases[2], *Search;
  QComboBox *Loaders[2], *Side, *Kind, *FlowProfile;
  QCheckBox *Names, *AnalyzeRegions, *RegionCalls, *AnalyzeDataFlow,
      *AnalyzeFlowRegions, *FlowFragments;
  QDoubleSpinBox *Threshold;
  QSpinBox *Timeout, *FlowFunctions;
  QTableView *FunctionTable, *InstructionTable;
  QLabel *Summary, *Status, *FunctionTitle;
  QPlainTextEdit *Evidence;
  ComparisonSourceView *Source;
  QPushButton *Start, *Cancel, *Save, *Reopen, *Undo, *Redo, *Confirm, *Unmatch,
      *Remap, *Counterpart, *Regions, *DataFlow, *Impact, *RetryDetail,
      *Changes, *Reviews, *Validate;
  QPointer<ComparisonRegionsDialog> RegionsView, FlowRegionsView;
  QPointer<ComparisonFlowDialog> FlowView;
  QPointer<ComparisonImpactDialog> ImpactView;
  QPointer<ComparisonChangesDialog> ChangesView;
  QPointer<ComparisonReviewDialog> ReviewView;
  QPointer<ComparisonValidationDialog> ValidationView;
  void showImpact(const QString &Side, const QString &Address);
  QProgressBar *Progress;
  QTimer Filter;
  QString SavedPath, PrimaryAddress, SecondaryAddress, SelectedSide = "primary";
  bool RestoreSettings = false;
  bool DetailStopped = false;
  void run(const QString &Restore = {});
  void update();
  void inventory();
  void selectFunction(QString Which, QString Address);
  void showPage(const QJsonObject &Page);
  void edit(const QString &Kind);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONDIALOG_H
