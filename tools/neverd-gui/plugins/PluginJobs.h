//===- PluginJobs.h - External plugin process jobs --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External plugin process jobs.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_PLUGINS_PLUGINJOBS_H
#define NEVERD_TOOLS_NEVERD_GUI_PLUGINS_PLUGINJOBS_H

#include "PluginManifest.h"

#include "neverd/plugin/Executor.h"
#include "neverd/plugin/PluginCatalog.h"

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>

namespace neverd::gui {

struct PluginJob {
  QString id, plugin, action, title, state, diagnostic, directory;
  QString pluginName, actionTitle;
  QString stderrText;
  quint64 registration = 0;
  QJsonObject context, parameters, progress, result;
  QElapsedTimer clock;
  qint64 elapsedMs = 0;
  bool terminal() const;
};

/// Bounded, serial process jobs. The host owns process exit and publication;
/// a provider's result message alone never means that a job succeeded.
class PluginJobs final : public QObject {
  Q_OBJECT
public:
  explicit PluginJobs(QString outputRoot, QObject *parent = nullptr);
  ~PluginJobs() override;
  bool install(const QString &manifest, QString &error);
  void uninstall(const QString &id);
  QList<GuiPlugin> plugins() const;
  std::optional<GuiPlugin> plugin(const QString &id) const;
  QList<PluginJob> jobs() const;
  std::optional<PluginJob> job(const QString &id) const;
  void setContext(QJsonObject context);
  QJsonObject context() const { return context_; }
  void setGuiActivator(neverd::plugin::PluginCatalog::ActivateGui activate) {
    activateGui_ = std::move(activate);
  }
  void drainClosures() { control_->drain(); }
  QString
  start(const QString &plugin, const QString &action,
        const QJsonObject &parameters, QString &error,
        std::optional<neverd::plugin::Registration> binding = std::nullopt);
  std::optional<neverd::plugin::Registration>
  actionBinding(const QString &plugin, const QString &action) const;
  void cancel(const QString &id);
  void stopAll();
  void answerQuery(const QString &task, const QString &id,
                   const QJsonObject &reply);
  /// Recheck identity and copy into a user-chosen new file.
  bool exportArtifact(const QString &task, int index,
                      const QString &destination, QString &error) const;
  /// Prepare a user-requested image addition from a completed current task.
  /// Bytes are read and hashed together; no provider-supplied filesystem path
  /// is forwarded to the worker.
  bool analysisSection(const QString &task, QJsonObject &description,
                       QString &error) const;
  neverd::plugin::PluginHost &host() { return host_; }
  neverd::plugin::PluginCatalog &catalog() { return *catalog_; }
  const neverd::plugin::PluginCatalog &catalog() const { return *catalog_; }
  neverd::plugin::HostServices &capabilities() { return *capabilities_; }
  std::optional<neverd::plugin::Owner> componentOwner(const QString &id) const;
  bool retiring(const QString &id) const {
    return catalog_->retiring(id.toStdString());
  }
  void shutdown();

signals:
  void installationRecordChanged(const QString &id);
  void pluginsChanged();
  void componentBound(const QString &id);
  void contextChanged(const QJsonObject &context);
  void jobsChanged();
  void logMessage(const QString &task, const QString &message, int level);
  void queryRequested(const QString &task, const QString &id,
                      const QString &operation, const QJsonObject &payload);

private:
  void updateProcess(const neverd::plugin::ProcessActionSnapshot &snapshot);
  void updateAction(const QString &id,
                    const neverd::plugin::ActionSnapshot &snapshot);
  void createProjectOwner();
  void retireOwner(neverd::plugin::Owner owner);
  bool current(const PluginJob &job) const;
  QList<PluginJob> jobs_;
  QJsonObject context_;
  QString outputRoot_;
  bool stopping_ = false;
  std::shared_ptr<neverd::plugin::Executor> control_;
  neverd::plugin::PluginHost host_;
  neverd::plugin::Actions actions_;
  std::unique_ptr<neverd::plugin::HostServices> capabilities_;
  std::unique_ptr<neverd::plugin::ProcessActions> processes_;
  std::unique_ptr<neverd::plugin::PluginCatalog> catalog_;
  neverd::plugin::PluginCatalog::ActivateGui activateGui_;
  neverd::plugin::Owner projectOwner_;
  QHash<QString, std::shared_ptr<neverd::plugin::ActionCall>> calls_;
  QHash<QString, QHash<QString, std::function<void(neverd::plugin::Json)>>>
      queries_;
  QTimer poll_;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_PLUGINS_PLUGINJOBS_H
