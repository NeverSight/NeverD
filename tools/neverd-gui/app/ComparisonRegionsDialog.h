//===- ComparisonRegionsDialog.h - Overlapping region review ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Virtualized transformation hypotheses, shared spans and original paired
/// instructions. This view does not edit the accepted function mapping.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONREGIONSDIALOG_H
#define NEVERD_GUI_COMPARISONREGIONSDIALOG_H

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QPushButton;
class QTableView;
namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonRegionsDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonRegionsDialog(ComparisonSession &Session, QWidget *Parent,
                          bool SSA = false);
signals:
  void inspectFunction(const QString &Side, const QString &Address);
  void inspectCallContext(const QString &Side, const QString &Address);

private:
  ComparisonSession &Session;
  bool Flow;
  ComparisonModel *Hypotheses, *Anchors, *Instructions;
  QTableView *HypothesisTable, *AnchorTable, *InstructionTable;
  QComboBox *Kind;
  QComboBox *Coordinates = nullptr;
  QLabel *FragmentScope = nullptr;
  QLabel *Summary, *Evidence;
  QPushButton *CallContext;
  QString Revision, RegionId;
  QJsonObject InstructionQuery;
  QString anchorFunction(const QString &Side) const;
  void refresh();
  void filter();
  void selectHypothesis();
  void selectAnchor();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONREGIONSDIALOG_H
