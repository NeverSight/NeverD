//===- ModelScanDialog.h - Local model scan and evidence browser ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Local model scan and evidence browser.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_MODELSCANDIALOG_H
#define NEVERD_GUI_APP_MODELSCANDIALOG_H
#include "EngineClient.h"

#include <QDialog>
class QLineEdit;
class QSpinBox;
class QPushButton;
class QTreeWidget;
class QPlainTextEdit;
class QLabel;
namespace neverd::gui {
class ModelScanDialog final : public QDialog {
  Q_OBJECT
public:
  ModelScanDialog(const QString &Worker, QWidget *Parent);

private:
  void start();
  void receive(const QJsonObject &Message);
  void showReport();
  void updateActions();
  EngineClient Client;
  QString Active;
  bool Ready = false;
  int Offset = 0;
  QJsonObject Report;
  QLineEdit *Input, *Baseline, *OptionsJSON;
  QSpinBox *ScaleKiB;
  QPushButton *Run, *Cancel, *Previous, *Next, *Save;
  QLabel *Status;
  QTreeWidget *Evidence;
  QPlainTextEdit *Detail;
};
} // namespace neverd::gui
#endif // NEVERD_GUI_APP_MODELSCANDIALOG_H
