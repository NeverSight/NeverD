//===- ComparisonValidationDialog.h - Conditional scalar proof --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit assumptions, bounded retry and original input attribution for one
/// function pair. No proof result survives a comparison revision change.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_COMPARISONVALIDATIONDIALOG_H
#define NEVERD_GUI_COMPARISONVALIDATIONDIALOG_H

#include <QDialog>
#include <QJsonObject>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
namespace neverd::gui {
class ComparisonSession;
class ComparisonValidationDialog final : public QDialog {
  Q_OBJECT
public:
  ComparisonValidationDialog(ComparisonSession &Session, QWidget *Parent);
  void review(const QString &Side, const QString &Address);

private:
  ComparisonSession &Session;
  QLabel *Status, *Target;
  QPlainTextEdit *Evidence, *Report;
  QSpinBox *Timeout, *Instructions, *Work;
  QPushButton *Run, *Cancel;
  QJsonObject Query;
  QString Revision;
  quint64 Generation = 0;
  bool Pending = false;
  void run();
  void controls();
  void clear();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_COMPARISONVALIDATIONDIALOG_H
