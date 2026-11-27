//===- JVMRecoveryDialog.h - Class and JAR navigation ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Release, class and method selection for the shared JVM worker.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_GUI_JVMRECOVERYDIALOG_H
#define NEVERD_GUI_JVMRECOVERYDIALOG_H
#include "../EngineClient.h"

#include <QBitArray>
#include <QDialog>
#include <QJsonArray>
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
namespace neverd::gui {
class JVMRecoveryDialog final : public QDialog {
  Q_OBJECT
public:
  explicit JVMRecoveryDialog(const QString &Worker, QWidget *Parent = nullptr);
  bool exportView(const QString &Path, bool FullReport);

private:
  EngineClient Client;
  QString Active;
  QStringList Dependencies;
  bool Ready = false, Inventory = false;
  QLineEdit *Path;
  QComboBox *Release, *Classes, *Methods, *Results;
  QCheckBox *Preview;
  QLabel *Status;
  QPushButton *Browse, *Deps, *Clear, *Inspect, *Analyze, *Cancel, *Copy, *Save,
      *Export;
  QTabWidget *Tabs;
  QPlainTextEdit *Java, *IR, *Bytecode, *Evidence;
  QListWidget *Mappings;
  QJsonObject Report, Index;
  QBitArray RenderedTabs;
  void request(bool InventoryOnly);
  void receive(const QJsonObject &Message);
  void updateActions();
  void updateViewActions();
  void classes();
  void methods();
  QJsonObject currentFunction() const;
  void select(bool ChooseTab = false);
  void showCurrentTab();
  void resetIndex();
  QByteArray selectedText() const;
};
} // namespace neverd::gui
#endif // NEVERD_GUI_JVMRECOVERYDIALOG_H
