//===- PluginJobs.cpp - Qt presentation of shared plugin actions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adapt NPH actions, process events and installation snapshots for the GUI.
//===----------------------------------------------------------------------===//

#include "PluginJobs.h"

#include "../ResourceLimits.h"
#include "../app/Address.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QUuid>
#include <algorithm>
#include <stdexcept>

namespace neverd::gui {
using namespace ::neverd::limits::gui::plugin;
using namespace ::neverd::plugin::limits;
namespace {
neverd::plugin::Json native(const QJsonObject &Value) {
  return neverd::plugin::Json::parse(
      QJsonDocument(Value).toJson(QJsonDocument::Compact).toStdString());
}
QJsonObject gui(const neverd::plugin::Json &Value) {
  return QJsonDocument::fromJson(QByteArray::fromStdString(Value.dump()))
      .object();
}
QString artifactPath(const PluginJob &job, const QJsonObject &artifact) {
  const auto relative = artifact.value("path").toString();
  if (QFileInfo(relative).isAbsolute() || relative.contains(QLatin1Char(':')) ||
      QFileInfo(relative).fileName().compare(QStringLiteral("receipt.json"),
                                             Qt::CaseInsensitive) == 0 ||
      relative.split(QRegularExpression(QStringLiteral("[/\\\\]")))
          .contains(QStringLiteral("..")))
    return {};
  const auto root = QFileInfo(job.directory).canonicalFilePath();
  const auto path =
      QFileInfo(QDir(root).filePath(relative)).canonicalFilePath();
  return !root.isEmpty() &&
                 path.startsWith(root + QLatin1Char('/'), Qt::CaseSensitive)
             ? path
             : QString();
}
bool digestMatches(const QString &path, const QString &sha) {
  QFile file(path);
  if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) ||
      file.size() > kMaxArtifactBytes)
    return false;
  QCryptographicHash digest(QCryptographicHash::Sha256);
  return digest.addData(&file) &&
         QString::fromLatin1(digest.result().toHex()) == sha;
}
} // namespace

bool PluginJob::terminal() const {
  return state != QLatin1String("queued") &&
         state != QLatin1String("starting") &&
         state != QLatin1String("running") &&
         state != QLatin1String("cancelling");
}
PluginJobs::PluginJobs(QString outputRoot, QObject *parent)
    : QObject(parent), outputRoot_(std::move(outputRoot)),
      control_(std::make_shared<neverd::plugin::Executor>()),
      host_([control = control_](auto work) {
        control->post(std::move(work));
        QTimer::singleShot(0, QCoreApplication::instance(),
                           [control] { control->drain(); });
      }),
      actions_(host_) {
  createProjectOwner();
  neverd::plugin::ProcessActionEvents events;
  events.Changed = [this](const auto &snapshot) { updateProcess(snapshot); };
  events.Log = [this](const auto &task, const auto &text, bool diagnostic) {
    emit logMessage(QString::fromStdString(task), QString::fromStdString(text),
                    diagnostic ? 1 : 0);
  };
  neverd::plugin::Services::Handler query =
      [this](const neverd::plugin::ProcessServiceRequest &request, auto done) {
        const auto task = QString::fromStdString(request.Task);
        const auto record = job(task);
        if (!record || !current(*record)) {
          processes_->stop(request.Task, neverd::plugin::TaskPhase::Stale,
                           "Project context changed");
          done({{"status", "error"},
                {"code", "context_changed"},
                {"message", "The plugin project changed"}});
          return;
        }
        const auto id = QString::fromStdString(request.Id);
        queries_[task].insert(id, std::move(done));
        emit queryRequested(task, id, QString::fromStdString(request.Operation),
                            gui(request.Payload));
      };
  capabilities_ = std::make_unique<neverd::plugin::HostServices>(
      host_,
      [control = control_](auto work) {
        control->post(std::move(work));
        QTimer::singleShot(0, QCoreApplication::instance(),
                           [control] { control->drain(); });
      },
      query);
  neverd::plugin::Diagnostic error;
  if (!capabilities_->bind(projectOwner_, native(context_), true, error))
    throw std::logic_error(error.Message);
  processes_ = std::make_unique<neverd::plugin::ProcessActions>(
      host_, actions_, *control_,
      std::filesystem::u8path(outputRoot_.toUtf8().toStdString()),
      std::move(query), neverd::plugin::ProcessActions::Service{},
      std::move(events), capabilities_.get());
  catalog_ = std::make_unique<neverd::plugin::PluginCatalog>(
      host_, *processes_, projectOwner_,
      [this](const auto &change) {
        const auto state = catalog_->snapshot(change.Plugin);
        capabilities_->publish(
            projectOwner_, "plugin.installation_changed",
            {{"kind", change.Kind == neverd::plugin::CatalogChangeKind::Record
                          ? "record"
                          : "runtime"},
             {"plugin", change.Plugin},
             {"installation", state
                                  ? neverd::plugin::describeInstallation(*state)
                                  : neverd::plugin::Json()}});
        if (change.Kind == neverd::plugin::CatalogChangeKind::Record)
          emit installationRecordChanged(QString::fromStdString(change.Plugin));
        emit pluginsChanged();
      },
      [this](const auto &id) {
        emit componentBound(QString::fromStdString(id));
      });
  poll_.setInterval(kRuntimePollMilliseconds);
  connect(&poll_, &QTimer::timeout, this, [this] {
    control_->drain();
    processes_->poll();
    if (processes_->idle())
      poll_.stop();
  });
}
PluginJobs::~PluginJobs() { shutdown(); }
void PluginJobs::shutdown() {
  if (stopping_)
    return;
  stopping_ = true;
  poll_.stop();
  stopAll();
  processes_->shutdown();
  processes_.reset();
  control_->drain();
  host_.close(host_.root());
  control_->drain();
  queries_.clear();
}
void PluginJobs::createProjectOwner() {
  neverd::plugin::Diagnostic error;
  projectOwner_ = *host_.createOwner(
      host_.root(), neverd::plugin::OwnerKind::Project,
      context_.value("project_id").toString().toStdString(), error);
  host_.activate(projectOwner_, error);
}
void PluginJobs::retireOwner(neverd::plugin::Owner owner) {
  QPointer<PluginJobs> guard(this);
  host_.close(owner, [guard, owner](const auto &) {
    if (guard)
      guard->host_.forget(owner);
  });
}
std::optional<neverd::plugin::Owner>
PluginJobs::componentOwner(const QString &id) const {
  return catalog_->component(id.toStdString());
}
QList<GuiPlugin> PluginJobs::plugins() const {
  QList<GuiPlugin> result;
  for (const auto &state : catalog_->snapshots())
    if (state.Installed && state.Declaration && state.Component &&
        state.Component->Phase == neverd::plugin::OwnerPhase::Active)
      result.append(guiPlugin(*state.Declaration));
  return result;
}
std::optional<GuiPlugin> PluginJobs::plugin(const QString &id) const {
  const auto state = catalog_->snapshot(id.toStdString());
  if (!state || !state->Installed || !state->Declaration || !componentOwner(id))
    return std::nullopt;
  return guiPlugin(*state->Declaration);
}
QList<PluginJob> PluginJobs::jobs() const { return jobs_; }
std::optional<PluginJob> PluginJobs::job(const QString &id) const {
  for (const auto &record : jobs_)
    if (record.id == id)
      return record;
  return std::nullopt;
}
bool PluginJobs::install(const QString &path, QString &error) {
  neverd::plugin::Diagnostic diagnostic;
  const bool installed = catalog_->install(
      std::filesystem::u8path(path.toUtf8().toStdString()), diagnostic);
  error = QString::fromStdString(diagnostic.Message);
  return installed;
}
void PluginJobs::uninstall(const QString &id) {
  catalog_->remove(id.toStdString());
}
bool PluginJobs::current(const PluginJob &record) const {
  return record.context.value("epoch") == context_.value("epoch") &&
         record.context.value("project_id") == context_.value("project_id");
}
void PluginJobs::setContext(QJsonObject context) {
  if (stopping_)
    return;
  const bool changed =
      context.value("epoch") != context_.value("epoch") ||
      context.value("project_id") != context_.value("project_id");
  // The host holds the context it was given last, and takes one equal to it
  // as no change (HostServices::setContext): it is not written out for it.
  const bool same = context == context_;
  context_ = std::move(context);
  for (const auto &record : jobs_)
    if (!record.terminal() && !current(record))
      processes_->stop(record.id.toStdString(),
                       neverd::plugin::TaskPhase::Stale,
                       "Project context changed");
  if (changed) {
    const auto previous = projectOwner_;
    createProjectOwner();
    neverd::plugin::Diagnostic error;
    if (!capabilities_->bind(projectOwner_, native(context_), true, error))
      throw std::logic_error(error.Message);
    catalog_->setProject(projectOwner_);
    retireOwner(previous);
  } else if (!same)
    capabilities_->setContext(projectOwner_, native(context_));
  emit jobsChanged();
}
QString PluginJobs::start(const QString &pluginId, const QString &actionId,
                          const QJsonObject &parameters, QString &error,
                          std::optional<neverd::plugin::Registration> binding) {
  const auto currentBinding = actionBinding(pluginId, actionId);
  if (!currentBinding || (binding && *binding != *currentBinding)) {
    error =
        QStringLiteral("The selected action registration is no longer active");
    return {};
  }
  const auto descriptor = plugin(pluginId);
  const auto definition = actions_.definition(*currentBinding);
  const auto effective = resolvePluginParameters(
      QJsonDocument::fromJson(
          QByteArray::fromStdString(definition->Parameters.dump()))
          .array(),
      {}, {}, parameters, error);
  if (!effective)
    return {};
  int pending = 0;
  for (const auto &record : jobs_)
    pending += !record.terminal();
  if (pending >= kMaxQueuedJobs) {
    error = QStringLiteral("Plugin job queue is full (8 jobs)");
    return {};
  }
  if (!QDir().mkpath(outputRoot_)) {
    error = QStringLiteral("Cannot create plugin output directory");
    return {};
  }
  if (jobs_.size() >= kMaxHistoryRows) {
    const auto old =
        std::find_if(jobs_.begin(), jobs_.end(),
                     [](const auto &record) { return record.terminal(); });
    if (old != jobs_.end())
      jobs_.erase(old);
  }
  PluginJob record;
  record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  record.registration = currentBinding->id();
  record.plugin = pluginId;
  record.action = actionId;
  record.pluginName = descriptor->name;
  record.actionTitle = QString::fromStdString(definition->Title);
  record.title = descriptor->name + QStringLiteral(" / ") + record.actionTitle;
  record.state = QStringLiteral("queued");
  record.context = context_;
  record.parameters = *effective;
  record.directory = QDir(outputRoot_).absoluteFilePath(record.id);
  const auto id = record.id;
  jobs_.append(std::move(record));
  QPointer<PluginJobs> guard(this);
  neverd::plugin::Diagnostic diagnostic;
  auto call = actions_.invoke(
      *currentBinding,
      {id.toStdString(), native(context_), native(*effective), {}},
      [guard, id](const auto &snapshot) {
        if (guard)
          guard->updateAction(id, snapshot);
      },
      diagnostic);
  if (!call) {
    jobs_.removeLast();
    error = QString::fromStdString(diagnostic.Message);
    return {};
  }
  if (!neverd::plugin::terminal(call->snapshot().Phase))
    calls_.insert(id, std::move(call));
  poll_.start();
  emit jobsChanged();
  return id;
}
std::optional<neverd::plugin::Registration>
PluginJobs::actionBinding(const QString &plugin, const QString &action) const {
  return catalog_->action(plugin.toStdString(), action.toStdString());
}
void PluginJobs::updateProcess(
    const neverd::plugin::ProcessActionSnapshot &snapshot) {
  const auto id = QString::fromStdString(snapshot.Task.Id);
  for (auto &record : jobs_) {
    if (record.id != id)
      continue;
    record.state =
        QString::fromLatin1(neverd::plugin::taskPhaseName(snapshot.Task.Phase));
    record.diagnostic = QString::fromStdString(snapshot.Task.Diagnostic);
    record.stderrText = QString::fromStdString(snapshot.Stderr);
    record.elapsedMs = snapshot.ElapsedMs;
    record.result = gui(snapshot.Result);
    if (!record.clock.isValid() &&
        (snapshot.Task.Phase == neverd::plugin::TaskPhase::Starting ||
         snapshot.Task.Phase == neverd::plugin::TaskPhase::Running))
      record.clock.start();
    if (record.terminal())
      queries_.remove(id);
    break;
  }
  emit jobsChanged();
}
void PluginJobs::updateAction(const QString &id,
                              const neverd::plugin::ActionSnapshot &snapshot) {
  const bool terminal = neverd::plugin::terminal(snapshot.Phase);
  for (auto &record : jobs_) {
    if (record.id != id)
      continue;
    record.progress = gui(snapshot.Progress);
    // Transport completion has a more precise task outcome (stale/timeout).
    // Direct admission/handler failures have no process snapshot to project.
    if (terminal && !record.terminal()) {
      record.state =
          QString::fromLatin1(neverd::plugin::actionPhaseName(snapshot.Phase));
      record.diagnostic = QString::fromStdString(snapshot.Error.Message);
      if (snapshot.Phase != neverd::plugin::ActionPhase::Succeeded)
        record.result = {};
    } else if (snapshot.Phase == neverd::plugin::ActionPhase::Cancelling)
      record.state = QStringLiteral("cancelling");
    break;
  }
  if (terminal) {
    calls_.remove(id);
    queries_.remove(id);
  }
  emit jobsChanged();
}
void PluginJobs::answerQuery(const QString &task, const QString &id,
                             const QJsonObject &reply) {
  const auto found = queries_.find(task);
  if (found == queries_.end())
    return;
  auto done = found->take(id);
  if (found->isEmpty())
    queries_.erase(found);
  if (done)
    done(native(reply));
}
void PluginJobs::cancel(const QString &id) {
  const auto call = calls_.value(id);
  if (call) {
    call->cancel();
    processes_->poll();
  }
}
void PluginJobs::stopAll() {
  const auto calls = calls_.values();
  for (const auto &call : calls)
    call->cancel();
  processes_->poll();
}
bool PluginJobs::analysisSection(const QString &task, QJsonObject &description,
                                 QString &error) const {
  const auto record = job(task);
  if (!record || record->state != QLatin1String("succeeded") ||
      !current(*record) || !context_.value("loaded").toBool() ||
      record->context.value("revision") != context_.value("revision")) {
    error = QStringLiteral(
        "The project changed; run this task again before importing");
    return false;
  }
  const auto addition = record->result.value("image_addition").toObject();
  if (addition.isEmpty()) {
    error = QStringLiteral("This task has no analysis image addition");
    return false;
  }
  const auto artifact = record->result.value("artifacts")
                            .toArray()
                            .at(addition.value("artifact").toInt())
                            .toObject();
  QFile source(context_.value("input_path").toString());
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!source.open(QIODevice::ReadOnly) || !hash.addData(&source) ||
      QString::fromLatin1(hash.result().toHex()) !=
          addition.value("sample_sha256").toString()) {
    error = QStringLiteral(
        "The analysis image addition belongs to a different input");
    return false;
  }
  QFile file(artifactPath(*record, artifact));
  // Inline hexadecimal must fit the worker's 8 MiB frame, with room for
  // function metadata. Larger artifacts remain available for export.
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 ||
      file.size() > kMaxImageImportBytes) {
    error = QStringLiteral(
        "Image addition is missing or exceeds the 3 MiB import limit");
    return false;
  }
  const auto bytes = file.readAll();
  if (bytes.size() != file.size() || file.error() != QFile::NoError ||
      QString::fromLatin1(
          QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)
              .toHex()) != artifact.value("sha256").toString()) {
    error = QStringLiteral("Image addition artifact identity changed");
    return false;
  }
  description = addition.value("description").toObject();
  description.insert("data_hex", QString::fromLatin1(bytes.toHex()));
  error.clear();
  return true;
}

bool PluginJobs::exportArtifact(const QString &task, int index,
                                const QString &destination,
                                QString &error) const {
  const auto record = job(task);
  if (!record || record->state != QLatin1String("succeeded") || index < 0 ||
      index >= record->result.value("artifacts").toArray().size()) {
    error = QStringLiteral("Artifact is unavailable");
    return false;
  }
  const auto artifact =
      record->result.value("artifacts").toArray().at(index).toObject();
  const auto source = artifactPath(*record, artifact);
  if (source.isEmpty() ||
      !digestMatches(source, artifact.value("sha256").toString())) {
    error = QStringLiteral("Artifact identity changed");
    return false;
  }
  if (!QFile::copy(source, destination)) {
    error = QStringLiteral("Cannot copy artifact to a new destination");
    return false;
  }
  if (!digestMatches(destination, artifact.value("sha256").toString())) {
    error = QStringLiteral("Copied artifact identity does not match");
    return false;
  }
  return true;
}
} // namespace neverd::gui
