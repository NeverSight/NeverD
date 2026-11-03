//===- PluginManagerDialog.h - Installed plugin management -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Present saved installation intent and the Session-owned NPH runtime.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_PLUGINMANAGERDIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_PLUGINMANAGERDIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

namespace neverd::gui {
class PluginServices;

class PluginManagerDialog final : public QDialog {
  Q_OBJECT
public:
  explicit PluginManagerDialog(PluginServices &Services,
                               QWidget *Parent = nullptr);

private:
  void refresh();
  void selectPlugin();
  void filterPlugins();
  void changeEnabled(bool Enabled);
  QString selectedId() const;

  PluginServices &Services;
  QLineEdit *Filter;
  QTreeWidget *Plugins;
  QPlainTextEdit *Details;
  QLabel *Status;
  QPushButton *Toggle, *Retry, *Uninstall, *Location;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_PLUGINMANAGERDIALOG_H
