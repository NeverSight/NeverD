//===- PluginServices.h - Session-owned plugin services -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Installation intent, subscriptions and publications that outlive task views.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_PLUGINSERVICES_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_PLUGINSERVICES_H

#include "Address.h"
#include "plugins/PluginJobs.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <functional>
#include <map>
#include <optional>

namespace neverd::gui {
class Session;

/// Saved installation intent is independent of the NPH component lifetime.
struct PluginInstallation {
  QString Id, Name, Version, Path, Diagnostic;
  bool Enabled = true;
  bool Installed = true;
};

class PluginServices final : public QObject {
  Q_OBJECT
public:
  explicit PluginServices(Session &Session);
  ~PluginServices() override;
  PluginJobs &jobs() { return Jobs; }
  void restorePlugins();
  QList<PluginInstallation> pluginInstallations() const;
  bool installPlugin(const QString &Path, QString &Error);
  bool setPluginEnabled(const QString &Id, bool Enabled, QString &Error);
  void uninstallPlugin(const QString &Id);
  /// The workbench's focused selection; the session selection when unset.
  void setSelectionProvider(std::function<QJsonObject()> Provider);
  /// The only publisher of the plugin context. Every refresh builds it from the
  /// same selection source, so host events see only real changes.
  void refreshContext();
  bool pending(const QString &Task) const { return Pending.contains(Task); }
  QString receipt(const QString &Task) const { return Receipts.value(Task); }
  void importImage(const QString &Task);

signals:
  void pluginsChanged();
  void contextChanged();
  void changed(const QString &Task);
  void message(const QString &Text, int Level);
  void navigateRequested(neverd::gui::Address Address);

private:
  void savePlugin(const QString &Id);
  void complete(const QString &Task, const QString &Text, int Level = 0);
  void pruneQueries();
  void recordReceipt(const QString &Task, const QString &State,
                     const QString &Text);
  bool beginPublication(const QString &Task);
  Session &Project;
  PluginJobs Jobs;
  std::function<QJsonObject()> Selection;
  bool PluginsRestored = false;
  QHash<QString, QObject *> QueryOwners;
  QSet<QString> Pending;
  QHash<QString, QString> Receipts;
  struct Publication {
    neverd::plugin::Owner Scope;
    neverd::plugin::Invocation Lease;
  };
  std::map<QString, Publication> Publications;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_PLUGINSERVICES_H
