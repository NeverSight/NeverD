//===- ComparisonCounterpartDialog.h - Searchable pairing -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Select a counterpart from revision-bound, paged function inventories.
/// Candidate browsing never changes a mapping; only the explicit Pair action
/// requests a replacement through the comparison session.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONCOUNTERPARTDIALOG_H
#define NEVERD_GUI_COMPARISONCOUNTERPARTDIALOG_H

#include <QDialog>
#include <QString>
#include <QTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonCounterpartDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonCounterpartDialog(ComparisonSession &Session,
                              const QString &SourceSide,
                              const QString &SourceAddress,
                              const QString &CurrentCounterpart,
                              QWidget *Parent = nullptr);

private:
  ComparisonSession &Session;
  const QString SourceSide, SourceAddress, CurrentCounterpart, Revision;
  ComparisonModel *Candidates;
  QTableView *Table;
  QComboBox *Scope, *Sort;
  QLineEdit *Search;
  QLabel *Source, *Status, *Selection;
  QPushButton *Pair;
  QTimer Filter;
  bool Invalid = false;
  void reload();
  void updateSelection();
  void invalidate(const QString &Message);
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONCOUNTERPARTDIALOG_H
