//===- ScriptRecoveryDialog.h - Script artifact browser -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shows immutable source and bytecode mappings returned by a dedicated worker.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_SCRIPTRECOVERYDIALOG_H
#define NEVERD_GUI_APP_SCRIPTRECOVERYDIALOG_H

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
enum class ScriptLanguage { Lua, Python };
class ScriptRecoveryDialog final : public QDialog {
  Q_OBJECT
public:
  explicit ScriptRecoveryDialog(const QString &Worker,
                                QWidget *Parent = nullptr,
                                ScriptLanguage Language = ScriptLanguage::Lua);
  bool exportFile(const QString &Path, bool Manifest);

  bool openNative(const QString &NewFile);
signals:
  void nativeReady(const QString &File);

private:
  int selectedComponent() const;
  void resetComponents();
  bool Python;
  QString Operation, NativeDestination;
  QComboBox *Components = nullptr;
  QPushButton *Directory = nullptr, *Native = nullptr;
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
#endif // NEVERD_GUI_APP_SCRIPTRECOVERYDIALOG_H
