//===- X87CapabilitiesDialog.h - Inspect x87 support -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read-only inspection of the engine's shared x87 capability report.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_X87CAPABILITIESDIALOG_H
#define NEVERD_GUI_APP_X87CAPABILITIESDIALOG_H

#include <QDialog>
#include <QHash>
#include <QString>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
class QueryService;

namespace neverd::gui {
class X87CapabilitiesModel;
class X87CapabilitiesDialog final : public QDialog {
  Q_OBJECT
public:
  X87CapabilitiesDialog(QueryService &Queries, QWidget *Parent = nullptr);
  ~X87CapabilitiesDialog() override;
  /// Availability comes from the worker hello, never from a loaded binary.
  void setProviderAvailable(bool Connected, bool Supported);

private:
  void refresh();
  void selectDimensions();
  void showDetails();
  QueryService &Queries;
  X87CapabilitiesModel *Model;
  QSortFilterProxyModel *Filter;
  QTableView *Table;
  QPlainTextEdit *Details;
  QLabel *Status;
  QPushButton *Reload;
  QHash<QString, QComboBox *> Selectors;
  bool ProviderKnown = false, Connected = false, Supported = false;
  quint64 Generation = 0;
};
} // namespace neverd::gui

#endif // NEVERD_GUI_APP_X87CAPABILITIESDIALOG_H
