//===- AnalysisPlugins.h - External plugin workbench bridge --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External plugin workbench bridge.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_ANALYSISPLUGINS_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_ANALYSISPLUGINS_H

#include "Address.h"
#include "plugins/PluginJobs.h"

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

class QAction;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;

namespace neverd::gui {
class ActionRegistry;
class Session;
class PluginServices;

/// Generic process-plugin bridge and workbench pane. No provider-specific
/// analysis, parameter names or result formats belong in this class.
class AnalysisPlugins final : public QWidget {
  Q_OBJECT
public:
  AnalysisPlugins(Session &session, ActionRegistry &actions,
                  QWidget *parent = nullptr);
  PluginJobs &jobs() { return jobs_; }
  /// Register a manifest and remember it for subsequent workbench launches.
  bool installPlugin(const QString &path, QString &error);
  /// Remove the installation from this Session and future launches.
  void uninstallPlugin(const QString &id);
  QMenu *menu() const { return menu_; }
  QList<QAction *> contextActions() const;
  void populateContextMenu(QMenu &menu);
  void setSelectionProvider(std::function<QJsonObject()> provider);
  void importPlugin();

signals:
  void showRequested();
  void navigateRequested(neverd::gui::Address address);
  void message(const QString &text, int level);

private:
  void restorePlugins();
  void refreshContext();
  void rebuildActions();
  void configureMenus();
  void applyPresentation();
  void refreshJobs();
  QTreeWidgetItem *taskItem(const QString &id) const;
  void showResult();
  void invoke(const QString &plugin, const QJsonObject &action);
  void activateAddress(const QString &task, const QString &address);
  void importImageAddition();
  Session &session_;
  ActionRegistry &actions_;
  PluginServices &services_;
  PluginJobs &jobs_;
  QMenu *menu_;
  QTreeWidget *tasks_;
  QTabWidget *views_;
  QPlainTextEdit *log_;
  QLabel *status_, *resultTitle_, *resultOwner_;
  QPushButton *cancel_, *export_, *importImage_, *showLog_;
  QWidget *resultActions_;
  QHash<QString, QStringList> taskLogs_;
  QProgressBar *progress_;
  QList<QAction *> registered_;
  std::function<QJsonObject()> selection_;
  QString displayed_;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_ANALYSISPLUGINS_H
