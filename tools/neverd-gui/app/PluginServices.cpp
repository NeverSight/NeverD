//===- PluginServices.cpp - Session-owned plugin services -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stable query and publication ownership independent of QWidget lifetime.
///
//===----------------------------------------------------------------------===//

#include "PluginServices.h"

#include "../ResourceLimits.h"
#include "Session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <algorithm>

namespace neverd::gui {
namespace {
constexpr auto InstalledPluginsKey = "guiPlugins/installed";
constexpr auto EnabledPluginsKey = "guiPlugins/enabled";
constexpr auto AgentPluginId = "neverd.agent-ui";

QString pluginKey(const char *Group, const QString &Id) {
  return QLatin1String(Group) + QLatin1Char('/') + Id;
}

std::filesystem::path manifestPath(const QString &Path) {
  const auto Utf8 = Path.toUtf8();
  return std::filesystem::u8path(Utf8.constBegin(), Utf8.constEnd());
}
} // namespace

PluginServices::PluginServices(Session &Session)
    : Project(Session), Jobs(QDir(QStandardPaths::writableLocation(
                                      QStandardPaths::AppLocalDataLocation))
                                 .filePath(QStringLiteral("plugin-jobs")),
                             this) {
  connect(&Jobs, &PluginJobs::installationRecordChanged, this,
          [this](const QString &Id) {
            if (PluginsRestored)
              savePlugin(Id);
          });
  connect(&Jobs, &PluginJobs::pluginsChanged, this,
          &PluginServices::pluginsChanged);
  connect(&Jobs, &PluginJobs::queryRequested, this,
          [this](const QString &Task, const QString &Id,
                 const QString &Operation, const QJsonObject &Payload) {
            const auto Job = Jobs.job(Task);
            if (!Job || Job->context.value("epoch").toString() !=
                            QString::number(Project.epoch())) {
              Jobs.answerQuery(Task, Id,
                               {{"status", "error"},
                                {"code", "context_changed"},
                                {"message", "The plugin project changed"}});
              return;
            }
            auto *Owner = QueryOwners.value(Task);
            if (!Owner) {
              Owner = new QObject(this);
              QueryOwners.insert(Task, Owner);
            }
            QueryService::QuerySpec Query{
                Operation, Payload, QueryService::QuerySpec::Exact,
                Job->context.value("revision").toString()};
            Project.queries().subscribe(Query, Owner,
                                        [this, Task, Id](const auto &Reply) {
                                          Jobs.answerQuery(Task, Id, Reply);
                                        });
          });
  connect(&Jobs, &PluginJobs::jobsChanged, this, &PluginServices::pruneQueries);
  connect(&Project, &Session::stateChanged, this,
          &PluginServices::refreshContext);
  connect(&Project, &Session::selectionChanged, this,
          &PluginServices::refreshContext);
  connect(&Project, &Session::revisionChanged, this,
          &PluginServices::refreshContext);
  refreshContext();
}

QList<PluginInstallation> PluginServices::pluginInstallations() const {
  QList<PluginInstallation> Result;
  for (const auto &State : Jobs.catalog().snapshots()) {
    PluginInstallation Item;
    Item.Id = QString::fromStdString(State.Record.Id);
    const auto Path = State.Record.Path.u8string();
    Item.Path = QDir::fromNativeSeparators(QString::fromUtf8(
        reinterpret_cast<const char *>(Path.data()), Path.size()));
    Item.Enabled = State.Record.Enabled;
    Item.Installed = State.Installed;
    Item.Bundled = State.Bundled;
    Item.Name = State.Declaration
                    ? QString::fromStdString(State.Declaration->Name)
                    : Item.Id;
    if (State.Declaration)
      Item.Version = QString::fromStdString(State.Declaration->Version);
    QStringList Errors;
    for (const auto *Error : {&State.DiscoveryError, &State.ActivationError})
      if (!Error->Code.empty())
        Errors.append(
            QString::fromStdString(Error->Code + ": " + Error->Message));
    for (const auto &Error : State.CleanupErrors)
      Errors.append(QString::fromStdString(Error.Code + ": " + Error.Message));
    Item.Diagnostic = Errors.join(QLatin1Char('\n'));
    Result.append(std::move(Item));
  }
  std::sort(Result.begin(), Result.end(),
            [](const auto &Left, const auto &Right) {
              const auto Order =
                  QString::compare(Left.Name, Right.Name, Qt::CaseInsensitive);
              return Order ? Order < 0 : Left.Id < Right.Id;
            });
  return Result;
}

void PluginServices::savePlugin(const QString &Id) {
  QSettings Settings;
  const auto State = Jobs.catalog().snapshot(Id.toStdString());
  const auto InstalledKey = pluginKey(InstalledPluginsKey, Id);
  const auto EnabledKey = pluginKey(EnabledPluginsKey, Id);
  if (State && State->Bundled) {
    Settings.setValue(EnabledKey, State->Record.Enabled);
  } else if (!State || !State->Installed) {
    Settings.remove(InstalledKey);
    Settings.remove(EnabledKey);
  } else {
    const auto Path = State->Record.Path.u8string();
    Settings.setValue(
        InstalledKey,
        QDir::fromNativeSeparators(QString::fromUtf8(
            reinterpret_cast<const char *>(Path.data()), Path.size())));
    Settings.setValue(EnabledKey, State->Record.Enabled);
  }
  Settings.sync();
  if (Settings.status() != QSettings::NoError)
    emit message(tr("Plugin state changed, but its startup preference could "
                    "not be saved."),
                 2);
}

void PluginServices::restorePlugins(const QString &BundledAgentManifest) {
  if (PluginsRestored)
    return;
  QSettings Settings;
  Settings.beginGroup(QLatin1String(InstalledPluginsKey));
  const auto Ids = Settings.childKeys();
  Settings.endGroup();
  std::vector<neverd::plugin::InstallationRecord> Records, Bundled;
  const auto AgentId = QString::fromLatin1(AgentPluginId);
  const auto enabled = [&](const QString &Id) {
    return Settings.value(pluginKey(EnabledPluginsKey, Id), true).toBool();
  };
  if (!BundledAgentManifest.isEmpty())
    Bundled.push_back(
        {AgentPluginId, manifestPath(BundledAgentManifest), enabled(AgentId)});
  for (const auto &Id : Ids) {
    if (!BundledAgentManifest.isEmpty() && Id == AgentId)
      continue;
    Records.push_back(
        {Id.toStdString(),
         manifestPath(
             Settings.value(pluginKey(InstalledPluginsKey, Id)).toString()),
         enabled(Id)});
  }
  neverd::plugin::Diagnostic Error;
  const bool Restored = Jobs.catalog().restore(Records, Error, Bundled);
  PluginsRestored = true;
  QStringList Warnings;
  if (!Restored)
    Warnings.append(QString::fromStdString(Error.Code + ": " + Error.Message));
  for (const auto &Installation : pluginInstallations())
    if (!Installation.Diagnostic.isEmpty())
      Warnings.append(tr("Could not restore plugin %1 from %2: %3")
                          .arg(Installation.Id, Installation.Path,
                               Installation.Diagnostic));
  for (const auto &Text : Warnings)
    QTimer::singleShot(0, this, [this, Text] { emit message(Text, 1); });
}

bool PluginServices::installPlugin(const QString &Path, QString &Error) {
  return Jobs.install(Path, Error);
}

bool PluginServices::setPluginEnabled(const QString &Id, bool Enabled,
                                      QString &Error) {
  neverd::plugin::Diagnostic Diagnostic;
  const bool Active =
      Jobs.catalog().setEnabled(Id.toStdString(), Enabled, Diagnostic);
  Error = QString::fromStdString(Diagnostic.Message);
  return Active;
}

void PluginServices::uninstallPlugin(const QString &Id) { Jobs.uninstall(Id); }

PluginServices::~PluginServices() {
  // The Session is going away with its worker. Do not report an unreceived
  // command as accepted, and do not submit new cleanup commands during
  // teardown.
  for (const auto &[Task, Publication] : Publications)
    recordReceipt(Task, QStringLiteral("unknown"),
                  tr("Session closed before the publication receipt arrived."));
  Publications.clear();
  Jobs.shutdown();
  for (auto *Owner : QueryOwners)
    Project.queries().unsubscribeOwner(Owner);
}

bool PluginServices::beginPublication(const QString &Task) {
  const auto Job = Jobs.job(Task);
  const auto Parent = Job ? Jobs.componentOwner(Job->plugin) : std::nullopt;
  neverd::plugin::Diagnostic Error;
  auto &Host = Jobs.host();
  const auto Scope =
      Parent ? Host.createOwner(*Parent, neverd::plugin::OwnerKind::Task,
                                "publication:" + Task.toStdString(), Error)
             : std::nullopt;
  if (!Scope) {
    complete(Task, tr("The plugin instance is no longer active."), 2);
    return false;
  }
  const auto Registration = Host.registerResource(
      *Scope, {"publication", Task.toStdString(), "sdk.command.v1"}, {}, {},
      Error);
  if (!Registration || !Host.activate(*Scope, Error)) {
    Host.failActivation(*Scope, Error.Message);
    complete(Task, QString::fromStdString(Error.Message), 2);
    return false;
  }
  auto Lease = Host.invoke(*Registration, Error);
  Publications.emplace(Task, Publication{*Scope, std::move(Lease)});
  Pending.insert(Task);
  recordReceipt(Task, QStringLiteral("submitted"),
                tr("Waiting for project receipt."));
  return true;
}

void PluginServices::recordReceipt(const QString &Task, const QString &State,
                                   const QString &Text) {
  Receipts.insert(Task, Text);
  const auto Job = Jobs.job(Task);
  if (!Job)
    return;
  const auto Path =
      QDir(Job->directory).filePath(QStringLiteral("receipt.json"));
  QFile Source(Path);
  if (!Source.open(QIODevice::ReadOnly)) {
    emit message(tr("Cannot read publication receipt: %1").arg(Path), 2);
    return;
  }
  auto Record = QJsonDocument::fromJson(Source.readAll()).object();
  Source.close();
  Record.insert("publication",
                QJsonObject{{"state", State}, {"message", Text}});
  QSaveFile Destination(Path);
  const auto Bytes = QJsonDocument(Record).toJson();
  if (!Destination.open(QIODevice::WriteOnly) ||
      Destination.write(Bytes) != Bytes.size() || !Destination.commit())
    emit message(tr("Project operation completed, but its receipt could not be "
                    "saved: %1")
                     .arg(Path),
                 2);
}

void PluginServices::setSelectionProvider(
    std::function<QJsonObject()> Provider) {
  Selection = std::move(Provider);
  refreshContext();
}

QJsonObject PluginServices::currentContext() const {
  auto Context = Selection ? Selection() : Project.selection();
  Context.insert("loaded", Project.loaded());
  Context.insert("epoch", QString::number(Project.epoch()));
  Context.insert("project_id", Project.queries().projectId());
  Context.insert("revision", Project.revision());
  Context.insert("input_path", Project.filePath());
  Context.insert("project_path", Project.projectPath());
  Context.insert("architecture", Project.architecture());
  Context.insert("format", Project.format());
  return Context;
}

void PluginServices::refreshContext() {
  Jobs.setContext(currentContext());
  emit contextChanged();
}

void PluginServices::pruneQueries() {
  for (const auto &Task : QueryOwners.keys()) {
    auto Job = Jobs.job(Task);
    if (!Job || Job->terminal()) {
      auto *Owner = QueryOwners.take(Task);
      Project.queries().unsubscribeOwner(Owner);
      Owner->deleteLater();
    }
  }
}

void PluginServices::complete(const QString &Task, const QString &Text,
                              int Level) {
  if (auto It = Publications.find(Task); It != Publications.end()) {
    const auto Scope = It->second.Scope;
    Publications.erase(It);
    QPointer<PluginServices> Guard(this);
    Jobs.host().close(Scope, [Guard, Scope](const auto &) {
      if (Guard)
        Guard->Jobs.host().forget(Scope);
    });
  }
  Pending.remove(Task);
  recordReceipt(
      Task, Level == 2 ? QStringLiteral("failed") : QStringLiteral("succeeded"),
      Text);
  emit message(Text, Level);
  emit changed(Task);
}

void PluginServices::importImage(const QString &Task) {
  if (Pending.contains(Task))
    return;
  refreshContext();
  QJsonObject Description;
  QString Error;
  if (!Jobs.analysisSection(Task, Description, Error)) {
    complete(Task, Error, 2);
    return;
  }
  const auto Job = Jobs.job(Task);
  const auto Epoch = Project.epoch();
  if (!beginPublication(Task))
    return;
  emit changed(Task);
  Project.addAnalysisSection(
      Description,
      [this, Task, Epoch, Description](const auto &) {
        complete(
            Task,
            tr("Imported analysis section %1. Save the project to keep it.")
                .arg(Description.value("name").toString()));
        if (Epoch != Project.epoch())
          return;
        const auto Base = addressValue(Description.value("address").toString());
        const auto Functions = Description.value("functions").toArray();
        if (Base && !Functions.isEmpty()) {
          const auto Offset = addressValue(
              Functions.first().toObject().value("offset").toString());
          if (Offset)
            emit navigateRequested(*Base + *Offset);
        }
        refreshContext();
      },
      [this, Task](const auto &, const QString &Text) {
        complete(Task, Text, 2);
      },
      Job->context.value("revision").toString());
}

} // namespace neverd::gui
