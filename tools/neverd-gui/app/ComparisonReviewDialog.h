//===- ComparisonReviewDialog.h - Analyst case history ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit manual assessments with paged history and input-bound drafts.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONREVIEWDIALOG_H
#define NEVERD_GUI_COMPARISONREVIEWDIALOG_H

#include <QDialog>
#include <QJsonObject>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableView;
namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonReviewDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonReviewDialog(ComparisonSession &Session, QWidget *Parent);
  void review(const QString &Side, const QString &Address);
signals:
  void saveComparison();

private:
  ComparisonSession &Session;
  ComparisonModel *History;
  QTableView *Table;
  QLineEdit *CaseFilter, *Case, *Author, *Reference;
  QComboBox *Assessment;
  QCheckBox *Latest;
  QPlainTextEdit *Note, *Record;
  QLabel *Summary, *Target;
  QPushButton *Append, *NewDraft, *Refresh, *Save;
  QString Revision, Head, PrimaryAddress, SecondaryAddress;
  QString DraftBinding, DraftPrimary, DraftSecondary;
  bool PairActive = false, Recording = false;
  QJsonObject Query;
  QString binding() const;
  void reload();
  void newDraft();
  void controls();
  void append();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONREVIEWDIALOG_H
