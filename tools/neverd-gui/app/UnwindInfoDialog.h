//===- UnwindInfoDialog.h - Exact-PC unwind inspection ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read-only view of one fixed PC, retaining both source scopes and absence.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_UNWINDINFODIALOG_H
#define NEVERD_GUI_APP_UNWINDINFODIALOG_H

#include "Address.h"

#include <QDialog>
#include <QPointer>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QueryService;

namespace neverd::gui {
class UnwindInfoDialog final : public QDialog {
  Q_OBJECT
public:
  UnwindInfoDialog(QueryService &Service, Address PC,
                   QWidget *Parent = nullptr);
  ~UnwindInfoDialog() override;
  void setProviderAvailable(bool Connected, bool Supported);

private:
  void refresh();
  void contextChanged();
  void clear();
  bool sameProgram() const;
  void present(const QJsonValue &Value);

  QPointer<QueryService> Queries;
  const Address QueryPC;
  const quint64 Epoch;
  const QString Project;
  QString Revision;
  quint64 Generation = 0;
  bool Connected = false;
  bool Supported = false;
  QLabel *Status;
  QPlainTextEdit *Dwarf;
  QPlainTextEdit *Compact;
  QPushButton *Reload;
};
} // namespace neverd::gui

#endif // NEVERD_GUI_APP_UNWINDINFODIALOG_H
