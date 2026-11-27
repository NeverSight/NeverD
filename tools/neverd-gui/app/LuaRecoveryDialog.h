//===- LuaRecoveryDialog.h - Lua artifact browser -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shows immutable source and bytecode mappings returned by a dedicated worker.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_LUARECOVERYDIALOG_H
#define NEVERD_GUI_APP_LUARECOVERYDIALOG_H

#include "EngineClient.h"

#include <QDialog>
#include <QJsonArray>
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
namespace neverd::gui {
class LuaRecoveryDialog final : public QDialog {
  Q_OBJECT
public:
  explicit LuaRecoveryDialog(const QString &Worker, QWidget *Parent = nullptr);
  bool exportFile(const QString &Path, bool Manifest);

private:
  void receive(const QJsonObject &Message);
  void start();
  void clearReport();
  void select(int Index);
  void updateActions();
  EngineClient Client;
  bool Ready = false;
  QString Active;
  QJsonObject Report;
  QLineEdit *Path;
  QComboBox *Profile, *Prototypes;
  QLabel *Status;
  QPlainTextEdit *Source, *Evidence;
  QListWidget *Mappings;
  QPushButton *Browse, *Recover, *Cancel, *Copy, *Save, *SaveReport;
};
} // namespace neverd::gui
#endif // NEVERD_GUI_APP_LUARECOVERYDIALOG_H
