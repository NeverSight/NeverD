//===- ComparisonImpactDialog.h - Observed native call paths ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Asynchronous bounded caller and entry review with original-code witnesses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONIMPACTDIALOG_H
#define NEVERD_GUI_COMPARISONIMPACTDIALOG_H

#include <QDialog>
#include <QJsonObject>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QSpinBox;
class QTabWidget;
class QTableView;
namespace neverd::gui {
class ComparisonModel;
class ComparisonSession;
class ComparisonImpactDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonImpactDialog(ComparisonSession &Session, QWidget *Parent);
  void trace(const QString &Side, const QString &Address);
signals:
  void inspectFunction(const QString &Side, const QString &Address);

private:
  ComparisonSession &Session;
  ComparisonModel *Functions, *Sites;
  QTableView *FunctionTable, *SiteTable;
  QComboBox *Side, *Direction, *FunctionKind, *SiteKind;
  QLineEdit *Seed;
  QSpinBox *Depth;
  QPushButton *Trace, *Cancel, *Inspect;
  QLabel *Summary, *SiteStatus;
  QPlainTextEdit *SiteDetails;
  QTabWidget *SiteTabs;
  QString Revision;
  QJsonObject Query;
  void trace();
  void clear();
  void controls();
  void showFunctions();
  void showSites();
  void showSite();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONIMPACTDIALOG_H
