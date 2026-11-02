//===- InterpreterRecoveryDialog.h - Recovery options ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Per-view interpreter recovery settings, with explicit memory premises.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_INTERPRETERRECOVERYDIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_INTERPRETERRECOVERYDIALOG_H

#include <QDialog>
#include <QJsonObject>

namespace neverd::gui {

class InterpreterRecoveryDialog final : public QDialog {
  Q_OBJECT
public:
  explicit InterpreterRecoveryDialog(const QJsonObject &Options,
                                     QWidget *Parent = nullptr);
  const QJsonObject &options() const { return Options; }
  static QJsonObject defaults();
  static void showReport(const QJsonObject &Report, QWidget *Parent);

private:
  QJsonObject Options;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_INTERPRETERRECOVERYDIALOG_H
