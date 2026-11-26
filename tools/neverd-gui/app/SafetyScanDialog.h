//===- SafetyScanDialog.h - Native memory-safety scan ------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Run a static scan in the open project's worker and inspect its evidence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_SAFETYSCANDIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_SAFETYSCANDIALOG_H

#include "QueryService.h"

#include <QDialog>
#include <QJsonArray>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace neverd::gui {
class SafetyScanDialog final : public QDialog {
  Q_OBJECT
public:
  SafetyScanDialog(QueryService &Service, QWidget *Parent);
  ~SafetyScanDialog() override;

private:
  void start();
  void showReport(const QJsonObject &Report);
  QueryService &Queries;
  QueryService::SubscriptionId Request = 0;
  QComboBox *Analyzers;
  QPushButton *Run;
  QPushButton *Cancel;
  QLabel *Status;
  QTreeWidget *Findings;
  QPlainTextEdit *Details;
  QJsonArray Rows;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_SAFETYSCANDIALOG_H
