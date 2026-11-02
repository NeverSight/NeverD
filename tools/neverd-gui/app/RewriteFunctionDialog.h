//===- RewriteFunctionDialog.h - Compile a replacement ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Whole-function replacement uses the existing source-patch publisher.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_REWRITEFUNCTIONDIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_REWRITEFUNCTIONDIALOG_H
#include "Address.h"

#include <QDialog>
#include <QJsonArray>
class QComboBox;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
namespace neverd::gui {
class Session;
class RewriteFunctionDialog final : public QDialog {
  Q_OBJECT
public:
  RewriteFunctionDialog(Session &session, Address function, QWidget *parent);
signals:
  void openProduced(const QString &path);
  void compareProduced(const QString &path);

private:
  void compile();
  Session &session_;
  Address function_;
  quint64 epoch_;
  QString revision_, produced_;
  QJsonArray bindings_;
  QPlainTextEdit *source_, *receipt_;
  QComboBox *compiler_;
  QLineEdit *compilerPath_, *symbol_, *original_;
  QLabel *status_;
  QPushButton *compile_, *open_, *compare_;
};
} // namespace neverd::gui
#endif // NEVERD_TOOLS_NEVERD_GUI_APP_REWRITEFUNCTIONDIALOG_H
