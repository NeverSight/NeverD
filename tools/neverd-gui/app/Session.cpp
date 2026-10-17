#include "Session.h"

#include "ProjectDatabase.h"
#include "SettingsKeys.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

namespace neverd::gui {
namespace {
constexpr int CacheMiB = 256;
constexpr char StringEncodingsKey[] = "strings/encodings";
constexpr char StringPreferredKey[] = "strings/preferred";
constexpr char StringMinLengthKey[] = "strings/minLength";
constexpr int MaxRecentFiles = 10;
/// The project's record of the user's answers about its debug information:
/// the PDB, DWARF or MAP file to load, or {"mode":"none"} for one declined,
/// and {"mode":"none"} for a declined symbol table.
constexpr char PdbInputState[] = "pdb-input.json";
constexpr char DwarfInputState[] = "dwarf-input.json";
constexpr char MapInputState[] = "map-input.json";
constexpr char SymbolsInputState[] = "symbols-input.json";

/// Keep \p options' answers about debug information with the project.
void insertChoices(QHash<QString, QByteArray> &state,
                   const LoadOptions &options) {
  if (!options.pdb.isEmpty())
    state.insert(QLatin1String(PdbInputState),
                 QJsonDocument(options.pdb).toJson(QJsonDocument::Compact));
  if (!options.dwarf.isEmpty())
    state.insert(QLatin1String(DwarfInputState),
                 QJsonDocument(options.dwarf).toJson(QJsonDocument::Compact));
  if (!options.map.isEmpty())
    state.insert(QLatin1String(MapInputState),
                 QJsonDocument(options.map).toJson(QJsonDocument::Compact));
  if (!options.symbols.isEmpty())
    state.insert(QLatin1String(SymbolsInputState),
                 QJsonDocument(options.symbols).toJson(QJsonDocument::Compact));
}

/// A debug file choice's key in the project's debug files and in a
/// database's workbench state.
struct DebugChoice {
  const char *Key, *State;
};
constexpr DebugChoice DebugChoices[] = {{"pdb", PdbInputState},
                                        {"dwarf", DwarfInputState},
                                        {"map", MapInputState},
                                        {"symbols", SymbolsInputState}};

/// \p choice with a path inside \p folder relative to it, so the project's
/// debug files name a file the project keeps wherever it is checked out.
QJsonObject withRelativePath(QJsonObject choice, const QDir &folder) {
  const QString path = choice.value(QStringLiteral("path")).toString();
  if (path.isEmpty() || QDir::isRelativePath(path))
    return choice;
  const QString relative = folder.relativeFilePath(path);
  if (QDir::isRelativePath(relative) &&
      !relative.startsWith(QLatin1String("..")))
    choice.insert(QStringLiteral("path"), relative);
  return choice;
}

QJsonObject withAbsolutePath(QJsonObject choice, const QDir &folder) {
  const QString path = choice.value(QStringLiteral("path")).toString();
  if (!path.isEmpty() && QDir::isRelativePath(path))
    choice.insert(QStringLiteral("path"),
                  QDir::cleanPath(folder.absoluteFilePath(path)));
  return choice;
}

/// Read the project's own files beside \p input over what a database
/// recorded: the bookmarks and debug files shared with the project, and the
/// user's place and desktop in it.
void overlayProjectFiles(const QString &input,
                         QHash<QString, QByteArray> &state) {
  const auto compact = [](const QJsonDocument &document) {
    return document.toJson(QJsonDocument::Compact);
  };
  if (const auto bookmarks = ProjectDatabase::readProjectFile(
          input, ProjectDatabase::BookmarksSuffix);
      bookmarks && bookmarks->isArray())
    state.insert(QLatin1String(ProjectDatabase::BookmarksState),
                 compact(QJsonDocument(
                     ProjectDatabase::bookmarkRows(bookmarks->array()))));
  if (const auto debug = ProjectDatabase::readProjectFile(
          input, ProjectDatabase::DebugFilesSuffix);
      debug && debug->isObject()) {
    const QDir folder = QFileInfo(input).dir();
    for (const auto &choice : DebugChoices)
      if (const auto value =
              debug->object().value(QLatin1String(choice.Key)).toObject();
          !value.isEmpty())
        state.insert(QLatin1String(choice.State),
                     compact(QJsonDocument(withAbsolutePath(value, folder))));
  }
  if (const auto workbench = ProjectDatabase::readProjectFile(
          input, ProjectDatabase::WorkbenchSuffix);
      workbench && workbench->isObject())
    for (const char *key :
         {ProjectDatabase::LocationState, ProjectDatabase::DesktopState}) {
      const auto value = workbench->object().value(QLatin1String(key));
      if (value.isObject())
        state.insert(QLatin1String(key),
                     compact(QJsonDocument(value.toObject())));
      else if (value.isArray())
        state.insert(QLatin1String(key),
                     compact(QJsonDocument(value.toArray())));
    }
}

bool isEdit(const QString &operation) {
  static const QSet<QString> edits{QStringLiteral("annotation_set"),
                                   QStringLiteral("rename"),
                                   QStringLiteral("code_edit"),
                                   QStringLiteral("function_create"),
                                   QStringLiteral("function_delete"),
                                   QStringLiteral("function_edit"),
                                   QStringLiteral("item_define"),
                                   QStringLiteral("operand_format"),
                                   QStringLiteral("local_types_declare"),
                                   QStringLiteral("local_types_forget"),
                                   QStringLiteral("undo"),
                                   QStringLiteral("redo"),
                                   QStringLiteral("save")};
  return edits.contains(operation);
}

bool silentFailure(const QString &status, const QString &code) {
  return status == QLatin1String("cancelled") ||
         code == QLatin1String("worker_stopped") ||
         code == QLatin1String("session_changed");
}
} // namespace

Session::Session(QString workerPath, QObject *parent)
    : QObject(parent), queries_(&client_), analysis_(queries_, workerPath),
      workerPath_(std::move(workerPath)),
      sessionEpoch_(queries_.sessionEpoch()) {
  queries_.setCacheBudgetMiB(CacheMiB);
  connect(&analysis_, &AnalysisPool::diagnostic, this,
          [this](const QString &text) { emit message(text, 0); });
  connect(&client_, &EngineClient::message, this, &Session::receive);
  connect(&client_, &EngineClient::diagnostic, this,
          [this](const QString &text) {
            // Engine stderr is diagnostics, never protocol data.
            for (const auto &line :
                 text.split(QLatin1Char('\n'), Qt::SkipEmptyParts))
              emit message(line, 0);
          });
  connect(&client_, &EngineClient::failure, this, &Session::setError);
  connect(&client_, &EngineClient::stopped, this, [this] {
    connected_ = false;
    starting_ = false;
    opening_ = false;
    queries_.setAvailable(false);
    if (loaded_) {
      loaded_ = false;
      emit unloaded();
    }
    emit stateChanged();
    if (restartPending_) {
      restartPending_ = false;
      startWorker();
    }
  });
  connect(&queries_, &QueryService::contextChanged, this, [this] {
    if (sessionEpoch_ != queries_.sessionEpoch())
      sessionEpoch_ = queries_.sessionEpoch();
    emit revisionChanged();
    emit stateChanged();
  });
  connect(&queries_, &QueryService::pendingChanged, this, [this] {
    finishTransition();
    emit stateChanged();
  });
  connect(&queries_, &QueryService::loadProgress, this,
          [this](const QJsonObject &payload) {
            const double total = payload.value("total").toDouble();
            const double done = payload.value("done").toDouble();
            emit loadProgress(payload.value("phase").toString(),
                              total > 0 ? std::clamp(done / total, 0.0, 1.0)
                                        : -1.0);
          });
  // A change on disk settles before the project takes it: git and editors
  // write several files, some by replacing them.
  projectSync_.setSingleShot(true);
  projectSync_.setInterval(300);
  connect(&projectSync_, &QTimer::timeout, this, &Session::syncProject);
  connect(&projectWatcher_, &QFileSystemWatcher::directoryChanged,
          &projectSync_, qOverload<>(&QTimer::start));
  connect(&projectWatcher_, &QFileSystemWatcher::fileChanged, &projectSync_,
          qOverload<>(&QTimer::start));
  startWorker();
}

Session::~Session() {
  disconnect(&client_, nullptr, this, nullptr);
  disconnect(&queries_, nullptr, this, nullptr);
}

QString Session::fileName() const { return QFileInfo(filePath_).fileName(); }

Address Session::entryAddress() const {
  return addressValue(metadata_.value("entry_address")).value_or(0);
}

bool Session::indexReady() const {
  return background_.value("state").toString() == QLatin1String("ready");
}

bool Session::busy() const {
  const auto state = background_.value("state").toString();
  return opening_ || queries_.hasPending() ||
         state == QLatin1String("building") ||
         state == QLatin1String("pending");
}

void Session::setError(const QString &text) {
  error_ = text;
  emit message(text, 2);
  emit stateChanged();
}

void Session::startWorker() {
  starting_ = true;
  emit message(tr("Starting analysis worker…"), 0);
  client_.start(workerPath_);
}

void Session::receive(const QJsonObject &incoming) {
  const auto type = incoming.value("type").toString();
  if (type == QLatin1String("hello")) {
    if (incoming.value("protocol_major").toInt() != 1) {
      setError(tr("Incompatible analysis worker protocol."));
      client_.stop();
      return;
    }
    connected_ = true;
    starting_ = false;
    capabilities_.clear();
    for (const auto &value : incoming.value("capabilities").toArray())
      capabilities_.insert(value.toString());
    queries_.setAvailable(true);
    emit message(tr("Analysis engine %1 ready")
                     .arg(incoming.value("engine_version").toString()),
                 0);
    emit stateChanged();
    // Before any file opens, so its listing starts with them.
    if (stringOptionsNeedMigration()) {
      const auto proceed = [this] {
        applyStringOptions(false);
        openPending();
      };
      read(
          QStringLiteral("string_encodings"), {}, this,
          [this, proceed](const QJsonObject &payload) {
            migrateStringOptions(payload.value("items").toArray());
            proceed();
          },
          [proceed](const QString &, const QString &) { proceed(); });
    } else {
      applyStringOptions(false);
      openPending();
    }
  } else if (type == QLatin1String("heartbeat")) {
    const auto background = incoming.value("background").toObject();
    if (background != background_) {
      background_ = background;
      const auto generation = background.value("generation").toString();
      const bool newGeneration = generation != generation_;
      generation_ = generation;
      const auto functions =
          background.value("functions").toInteger(functionCount_);
      const bool moreFunctions =
          functionCount_ >= 0 && functions != functionCount_;
      functionCount_ = functions;
      if (newGeneration || moreFunctions)
        queries_.listingChanged();
      emit backgroundChanged();
      if (newGeneration)
        emit generationChanged();
      if (moreFunctions)
        emit functionsChanged();
      emit stateChanged();
    }
  } else if (type == QLatin1String("fatal")) {
    setError(incoming.value("error").toObject().value("message").toString());
    client_.stop();
  }
}

QString Session::platformName() const {
  const auto platform =
      metadata_.value("load_options").toObject().value("platform").toString();
#define NEVERD_PLATFORM(ShortName, Name)                                       \
  if (!platform.isEmpty() && platform == QLatin1String(ShortName))             \
    return QCoreApplication::translate("Platforms", Name);
#include "Processors.def"
  return {};
}

QueryService::SubscriptionId Session::read(const QString &operation,
                                           const QJsonObject &payload,
                                           QObject *owner, Reply done,
                                           Failure failed) {
  auto &service = AnalysisService::handles(operation)
                      ? analysis_.queries(payload, owner ? owner : &reads_)
                      : queries_;
  return service.subscribe(
      {operation, payload}, owner ? owner : &reads_,
      [this, done = std::move(done),
       failed = std::move(failed)](const QJsonObject &response) {
        const auto status = response.value("status").toString();
        if (status == QLatin1String("ok")) {
          if (done)
            done(response.value("payload").toObject());
          return;
        }
        const auto error = response.value("error").toObject();
        const auto code = error.value("code").toString(status);
        if (silentFailure(status, code))
          return;
        const auto text = error.value("message").toString(code);
        if (failed)
          failed(code, text);
        else
          emit message(text, 1);
      });
}

void Session::command(const QString &operation, const QJsonObject &payload,
                      Callback done, Failure failed) {
  // Only edits and their saving make the project dirty while in flight; an
  // open, reload or analysis does not.
  const bool edit = isEdit(operation);
  if (edit)
    ++pendingWrites_;
  const auto epoch = queries_.sessionEpoch();
  // Rejections complete asynchronously too, so the completion always runs.
  queries_.enqueueCommand(
      operation, payload, this,
      [this, operation, epoch, edit, done = std::move(done),
       failed = std::move(failed)](const QJsonObject &response) {
        if (edit)
          --pendingWrites_;
        const auto status = response.value("status").toString();
        const bool opening = operation == QLatin1String("open");
        if (epoch == queries_.sessionEpoch() || (opening && status == "ok")) {
          const auto payload = response.value("payload").toObject();
          if (status == QLatin1String("ok")) {
            if (payload.contains("dirty"))
              dirty_ = payload.value("dirty").toBool();
            // An opened database holds the project until it is written back.
            if (edit && openedFromDatabase_)
              databaseDirty_ = true;
            if (done)
              done(payload);
          } else {
            const auto error = response.value("error").toObject();
            const auto code = error.value("code").toString(status);
            // The project's files changed on disk before the workbench
            // noticed: it takes them now, and the edit can be made again.
            if (code == QLatin1String("foreign_edits"))
              projectSync_.start();
            if (!silentFailure(status, code))
              setError(error.value("message").toString(code));
            if (failed)
              failed(code, error.value("message").toString(code));
            if (operation == QLatin1String("save")) {
              transition_.clear();
              transitionReady_ = false;
            }
          }
        }
        emit stateChanged();
      });
}

void Session::resetState() {
  if (!projectWatcher_.directories().isEmpty())
    projectWatcher_.removePaths(projectWatcher_.directories());
  if (!projectWatcher_.files().isEmpty())
    projectWatcher_.removePaths(projectWatcher_.files());
  projectSync_.stop();
  projectFiles_.clear();
  loaded_ = false;
  dirty_ = false;
  openedFromDatabase_ = false;
  databaseDirty_ = false;
  databaseState_.clear();
  metadata_ = {};
  history_ = {};
  background_ = {};
  generation_.clear();
  functionCount_ = -1;
}

void Session::open(const QString &path, LoadOptions options) {
  const QFileInfo file(path);
  if (!file.isFile()) {
    setError(tr("Select an existing binary file."));
    return;
  }
  pendingFile_ = file.absoluteFilePath();
  pendingOptions_ = options;
  if (dirty()) {
    requestTransition(QStringLiteral("open"));
    return;
  }
  if (!connected_) {
    if (!starting_ && !restartPending_)
      startWorker();
    emit stateChanged();
    return;
  }
  openPending();
}

namespace {
/// What opening a path needs once databases are unpacked or consulted.
struct PreparedOpen {
  QString input, database, error;
  QStringList notes, warnings;
  QHash<QString, QByteArray> state;
};

/// Runs off the GUI thread: unpack a database, or bring back a binary's
/// sidecars from its database when they are missing.
PreparedOpen unpackOrRestore(const QString &requested) {
  PreparedOpen prepared;
  if (ProjectDatabase::isDatabase(requested)) {
    prepared.database = requested;
    prepared.input = ProjectDatabase::unpack(
        requested, ProjectDatabase::workingDirectory(requested),
        &prepared.error);
    if (prepared.error.isEmpty())
      if (const auto contents =
              ProjectDatabase::read(requested, &prepared.error))
        prepared.state = contents->state;
    return prepared;
  }
  prepared.input = requested;
  if (!ProjectDatabase::canKeepBeside(requested)) {
    // A file in a folder this user cannot write to, such as a system
    // directory or a read-only mount, keeps its database, sidecars and lock
    // in the user's data directory, beside a copy of it.  IDA asks for
    // another place for its database instead.
    prepared.input = ProjectDatabase::workingCopy(requested, &prepared.error);
    if (!prepared.error.isEmpty())
      return prepared;
    prepared.notes.append(
        QCoreApplication::translate(
            "neverd::gui::Session",
            "%1 is in a folder you cannot write to; its database is kept in "
            "%2")
            .arg(QDir::toNativeSeparators(requested),
                 QDir::toNativeSeparators(
                     QFileInfo(prepared.input).absolutePath())));
  }
  // A project an older NeverD kept as files beside the input moves into its
  // folder before anything reads it.
  ProjectDatabase::migrate(prepared.input);
  prepared.database = ProjectDatabase::pathFor(prepared.input);
  if (!QFileInfo::exists(prepared.database))
    return prepared;
  QString error;
  const auto contents = ProjectDatabase::read(prepared.database, &error);
  if (!contents) {
    prepared.warnings.append(error);
    return prepared;
  }
  QFile input(prepared.input);
  QCryptographicHash hash(QCryptographicHash::Sha256);
  if (!input.open(QIODevice::ReadOnly) || !hash.addData(&input) ||
      QString::fromLatin1(hash.result().toHex()) != contents->inputSha256) {
    prepared.warnings.append(
        QCoreApplication::translate(
            "neverd::gui::Session",
            "%1 describes a different version of this file and was not used; "
            "saving replaces it.")
            .arg(prepared.database));
    return prepared;
  }
  prepared.state = contents->state;
  bool sidecarsPresent = false;
  for (const auto &suffix : ProjectDatabase::sidecarSuffixes())
    sidecarsPresent =
        sidecarsPresent || QFileInfo::exists(ProjectDatabase::projectFilePath(
                               prepared.input, suffix));
  if (!sidecarsPresent)
    for (auto it = contents->sidecars.cbegin(); it != contents->sidecars.cend();
         ++it) {
      if (const auto error = ProjectDatabase::prepareFolder(prepared.input);
          !error.isEmpty()) {
        prepared.warnings.append(error);
        break;
      }
      QSaveFile sidecar(
          ProjectDatabase::projectFilePath(prepared.input, it.key()));
      if (!sidecar.open(QIODevice::WriteOnly) ||
          sidecar.write(it.value()) != it.value().size() || !sidecar.commit())
        prepared.warnings.append(QCoreApplication::translate(
                                     "neverd::gui::Session",
                                     "Could not restore %1 from the database.")
                                     .arg(sidecar.fileName()));
    }
  return prepared;
}

/// Runs off the GUI thread: the files of the project \p requested opens,
/// and the state it keeps.
PreparedOpen prepareOpen(const QString &requested) {
  PreparedOpen prepared = unpackOrRestore(requested);
  if (prepared.error.isEmpty() && !prepared.input.isEmpty())
    overlayProjectFiles(prepared.input, prepared.state);
  return prepared;
}
} // namespace

void Session::openPending() {
  if (pendingFile_.isEmpty() || opening_ || !connected_)
    return;
  const auto requested = std::exchange(pendingFile_, {});
  const auto options = std::exchange(pendingOptions_, {});
  opening_ = true;
  error_.clear();
  emit message(tr("Loading %1…").arg(QFileInfo(requested).fileName()), 0);
  emit stateChanged();
  auto *watcher = new QFutureWatcher<PreparedOpen>(this);
  connect(watcher, &QFutureWatcherBase::finished, this,
          [this, watcher, requested, options] {
            const PreparedOpen prepared = watcher->result();
            watcher->deleteLater();
            for (const auto &note : prepared.notes)
              emit message(note, 0);
            for (const auto &warning : prepared.warnings)
              emit message(warning, 1);
            if (!prepared.error.isEmpty()) {
              opening_ = false;
              setError(prepared.error);
              emit stateChanged();
              if (!pendingFile_.isEmpty())
                openPending();
              return;
            }
            sendOpen(requested, prepared.input, prepared.database,
                     prepared.state, options);
          });
  watcher->setFuture(
      QtConcurrent::run([requested] { return prepareOpen(requested); }));
}

void Session::sendOpen(const QString &requested, const QString &path,
                       const QString &database,
                       const QHash<QString, QByteArray> &state,
                       LoadOptions options) {
  if (options.pdb.isEmpty())
    options.pdb =
        QJsonDocument::fromJson(state.value(QLatin1String(PdbInputState)))
            .object();
  if (options.dwarf.isEmpty())
    options.dwarf =
        QJsonDocument::fromJson(state.value(QLatin1String(DwarfInputState)))
            .object();
  if (options.map.isEmpty())
    options.map =
        QJsonDocument::fromJson(state.value(QLatin1String(MapInputState)))
            .object();
  if (options.symbols.isEmpty())
    options.symbols =
        QJsonDocument::fromJson(state.value(QLatin1String(SymbolsInputState)))
            .object();
  QJsonObject payload{{"path", path},
                      {"debug_info", options.debugInfo},
                      {"analysis", options.analysis}};
  if (!options.pdb.isEmpty())
    payload.insert(QStringLiteral("pdb"), options.pdb);
  if (!options.dwarf.isEmpty())
    payload.insert(QStringLiteral("dwarf"), options.dwarf);
  if (!options.map.isEmpty())
    payload.insert(QStringLiteral("map"), options.map);
  if (!options.symbols.isEmpty())
    payload.insert(QStringLiteral("symbols"), options.symbols);
  if (!options.loader.isEmpty())
    payload.insert(QStringLiteral("loader"), options.loader);
  if (options.loader == QLatin1String("binary")) {
    payload.insert(QStringLiteral("processor"), options.processor);
    payload.insert(QStringLiteral("base"), hexAddress(options.base));
    payload.insert(QStringLiteral("offset"), hexAddress(options.offset));
    payload.insert(QStringLiteral("size"), hexAddress(options.size));
    if (options.entry)
      payload.insert(QStringLiteral("entry"), hexAddress(*options.entry));
    if (!options.platform.isEmpty())
      payload.insert(QStringLiteral("platform"), options.platform);
  }
  command(
      QStringLiteral("open"), payload,
      [this, requested, path, database, state,
       options](const QJsonObject &payload) {
        opening_ = false;
        resetState();
        loadOptions_ = options;
        metadata_ = payload;
        // The functions the opened file lists; idle-time discovery can add
        // to them before the first heartbeat reports a count.
        functionCount_ = payload.value("function_count").toInteger(-1);
        filePath_ = path;
        projectPath_ = requested;
        databasePath_ = database;
        databaseState_ = state;
        loaded_ = true;
        openedFromDatabase_ = ProjectDatabase::isDatabase(requested);
        recordDebugChoices(false);
        // A project made for another build of the input, as after a pull
        // brought a new binary but not its project, may not match it.
        if (const auto project = payload.value("project").toObject();
            project.value("recorded").isObject() &&
            !project.value("matches").toBool(true))
          emit message(
              tr("The project's files describe another build of %1 (SHA-256 "
                 "%2…); their addresses may not match this one. The next "
                 "edit records this build.")
                  .arg(QFileInfo(projectPath_).fileName(),
                       project.value("recorded")
                           .toObject()
                           .value("sha256")
                           .toString()
                           .left(12)),
              1);
        // Files a pull or another tool changed since the last session leave
        // the undo history behind them; the project takes them as they are,
        // and watches them from now on.
        syncProject();
        QSettings store;
        auto recent = store.value(settings::RecentFiles).toStringList();
        recent.removeAll(requested);
        recent.prepend(requested);
        while (recent.size() > MaxRecentFiles)
          recent.removeLast();
        store.setValue(settings::RecentFiles, recent);
        // When each was last opened, for the quick start.
        QVariantMap openedAt;
        const QVariantMap known = store.value(settings::RecentOpened).toMap();
        for (const QString &file : recent)
          if (known.contains(file))
            openedAt.insert(file, known.value(file));
        openedAt.insert(requested, QDateTime::currentDateTime());
        store.setValue(settings::RecentOpened, openedAt);
        for (const auto &warning : payload.value("warnings").toArray())
          emit message(warning.toString(), 1);
        emit message(
            tr("%1: %2 %3, %4 functions")
                .arg(QFileInfo(requested).fileName(), format(), architecture())
                .arg(payload.value("function_count").toInt()),
            0);
        // A binary file's platform, and what detection read it from.
        if (const auto platform = platformName(); !platform.isEmpty()) {
          const auto load = payload.value("load_options").toObject();
          emit message(
              load.value("platform_source").toString() ==
                      QLatin1String("detected")
                  ? tr("Platform: %1, read from the code: %2")
                        .arg(platform,
                             load.value("platform_evidence").toString())
                  : tr("Platform: %1, as chosen").arg(platform),
              0);
        }
        if (const auto pdb = payload.value("pdb").toObject();
            pdb.value("mode").toString() == QLatin1String("none"))
          emit message(tr("Opened without the PDB the input names, as "
                          "chosen"),
                       0);
        else if (!pdb.isEmpty())
          reportPdb(pdb);
        if (payload.value("symbols").toObject().value("mode").toString() ==
            QLatin1String("none"))
          emit message(tr("Opened without its symbol table, as chosen"), 0);
        if (payload.value("dwarf").toObject().value("mode").toString() ==
            QLatin1String("none"))
          emit message(tr("Opened without its DWARF debug information, as "
                          "chosen"),
                       0);
        else if (const auto debug = payload.value("debug_info").toObject();
                 debug.value("kind").toString() == QLatin1String("dwarf"))
          emit message(tr("DWARF loaded: %1")
                           .arg(QDir::toNativeSeparators(
                               debug.value("path").toString())),
                       0);
        if (payload.value("map").toObject().value("mode").toString() ==
            QLatin1String("none"))
          emit message(tr("Opened without the MAP file beside the input, as "
                          "chosen"),
                       0);
        reportMaps(payload.value("debug_info").toObject());
        // A binary file's MAP is held against the image only as it loads.
        if (!options.acceptedMap.isEmpty() &&
            payload.value("debug_info").toObject().value("kind").toString() !=
                QLatin1String("map"))
          emit message(tr("MAP not loaded: %1 does not fit the image as loaded")
                           .arg(QDir::toNativeSeparators(options.acceptedMap)),
                       1);
        if (std::exchange(reloading_, false))
          emit message(tr("Reloaded the input file"), 0);
        emit opened();
        emit stateChanged();
        refreshHistory();
        refreshContributions();
        if (!pendingFile_.isEmpty())
          openPending();
      },
      [this](const QString &, const QString &) {
        opening_ = false;
        reloading_ = false;
        emit stateChanged();
        if (!pendingFile_.isEmpty())
          openPending();
      });
}

void Session::closeFile() {
  if (!loaded_)
    return;
  if (dirty()) {
    requestTransition(QStringLiteral("close"));
    return;
  }
  // The worker has no unload operation: restart it without a file.
  pendingFile_.clear();
  resetState();
  filePath_.clear();
  loadOptions_ = {};
  emit unloaded();
  restartPending_ = true;
  client_.stop();
}

void Session::reload(std::optional<LoadOptions> options) {
  if (!loaded_)
    return;
  reloadOptions_ = std::move(options);
  if (dirty()) {
    requestTransition(QStringLiteral("reload"));
    return;
  }
  // A fresh worker reads the file's bytes as they are now; the annotations
  // come from what was saved, and the user returns to the same place.  The
  // file opened is the one the user named: a working copy is refreshed from
  // it.
  saveDatabaseState();
  pendingFile_ = projectPath_.isEmpty() ? filePath_ : projectPath_;
  pendingOptions_ = std::exchange(reloadOptions_, {}).value_or(loadOptions_);
  reloading_ = true;
  resetState();
  emit unloaded();
  restartPending_ = true;
  client_.stop();
}

void Session::restart() {
  if (dirty()) {
    requestTransition(QStringLiteral("restart"));
    return;
  }
  // The project the user opened, so a database stays the one saving
  // writes back, and the place the user was.
  saveDatabaseState();
  pendingFile_ = projectPath_.isEmpty() ? filePath_ : projectPath_;
  pendingOptions_ = loadOptions_;
  resetState();
  emit unloaded();
  restartPending_ = true;
  client_.stop();
}

bool Session::requestQuit() {
  if (!dirty())
    return true;
  requestTransition(QStringLiteral("quit"));
  return false;
}

void Session::requestTransition(const QString &action) {
  transition_ = action;
  transitionReady_ = false;
  emit transitionRequested(action);
}

void Session::resolveTransition(const QString &choice) {
  if (transition_.isEmpty())
    return;
  if (choice == QLatin1String("cancel")) {
    transition_.clear();
    transitionReady_ = false;
    pendingFile_.clear();
    reloadOptions_.reset();
    return;
  }
  if (choice == QLatin1String("save")) {
    command(QStringLiteral("save"), {}, [this](const QJsonObject &) {
      dirty_ = false;
      if (openedFromDatabase_)
        writeDatabase(databasePath_);
      transitionReady_ = true;
      finishTransition();
    });
    return;
  }
  // Discard: staged annotations disappear with the session, and a database
  // the user opened keeps what it held.
  dirty_ = false;
  databaseDirty_ = false;
  transitionReady_ = true;
  finishTransition();
}

void Session::finishTransition() {
  if (!transitionReady_ || pendingWrites_ > 0 || transition_.isEmpty())
    return;
  const auto action = std::exchange(transition_, {});
  transitionReady_ = false;
  dirty_ = false;
  databaseDirty_ = false;
  if (action == QLatin1String("quit")) {
    emit quitApproved();
  } else if (action == QLatin1String("open")) {
    // Reopen through a fresh worker so discarded edits cannot linger.
    const auto path = pendingFile_;
    resetState();
    emit unloaded();
    pendingFile_ = path;
    restartPending_ = true;
    client_.stop();
  } else if (action == QLatin1String("close")) {
    closeFile();
  } else if (action == QLatin1String("reload")) {
    reload(std::exchange(reloadOptions_, {}));
  } else if (action == QLatin1String("restart")) {
    restart();
  }
}

void Session::refreshHistory() {
  if (!loaded_)
    return;
  read(
      QStringLiteral("history"), {{"limit", 1}}, &reads_,
      [this](const QJsonObject &payload) {
        history_ = payload;
        emit historyChanged();
        emit stateChanged();
      },
      [](const QString &, const QString &) {});
}

void Session::save() {
  if (!loaded_)
    return;
  // A binary's project keeps its edits beside it as they are made; a
  // database the user opened is written back.
  command(QStringLiteral("save"), {}, [this](const QJsonObject &) {
    dirty_ = false;
    refreshHistory();
    emit message(tr("Comments and history saved"), 0);
    if (openedFromDatabase_)
      writeDatabase(databasePath_);
  });
}

void Session::packDatabase(const QString &path) {
  if (!loaded_ || path.isEmpty())
    return;
  // What is staged or still on its way to the worker belongs in the
  // database too; the worker commits it in order before this save.
  if (!dirty()) {
    writeDatabase(path);
    return;
  }
  command(QStringLiteral("save"), {}, [this, path](const QJsonObject &) {
    dirty_ = false;
    refreshHistory();
    writeDatabase(path);
  });
}

void Session::writeDatabase(const QString &path) {
  if (path.isEmpty())
    return;
  auto state = stateProvider_ ? stateProvider_() : QHash<QString, QByteArray>{};
  insertChoices(state, loadOptions_);
  // Quitting waits for the database like it waits for an edit.
  ++pendingWrites_;
  emit stateChanged();
  auto *watcher = new QFutureWatcher<QString>(this);
  connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, path] {
    const QString error = watcher->result();
    watcher->deleteLater();
    --pendingWrites_;
    if (error.isEmpty()) {
      if (path == databasePath_)
        databaseDirty_ = false;
      emit message(tr("Database saved: %1").arg(path), 0);
    } else {
      setError(tr("Could not save the database: %1").arg(error));
    }
    emit databaseSaved(error.isEmpty());
    finishTransition();
    emit stateChanged();
  });
  watcher->setFuture(QtConcurrent::run([path, input = filePath_, state] {
    return ProjectDatabase::save(path, input, state);
  }));
}

void Session::saveDatabaseState() {
  if (!loaded_ || readOnly() || !stateProvider_)
    return;
  auto state = stateProvider_();
  if (openedFromDatabase_) {
    insertChoices(state, loadOptions_);
    if (QFileInfo::exists(databasePath_))
      if (const auto error = ProjectDatabase::saveState(databasePath_, state);
          !error.isEmpty())
        emit message(tr("Could not update the database: %1").arg(error), 1);
    return;
  }
  // A binary's project keeps its user's place and desktop beside it, in a
  // file that is the user's own rather than the project's.
  QJsonObject workbench;
  for (const char *key :
       {ProjectDatabase::LocationState, ProjectDatabase::DesktopState}) {
    const auto value = QJsonDocument::fromJson(state.value(QLatin1String(key)));
    if (value.isObject())
      workbench.insert(QLatin1String(key), value.object());
    else if (value.isArray())
      workbench.insert(QLatin1String(key), value.array());
  }
  if (const auto error = ProjectDatabase::writeProjectFile(
          filePath_, ProjectDatabase::WorkbenchSuffix,
          QJsonDocument(workbench));
      !error.isEmpty())
    emit message(error, 1);
}

void Session::saveBookmarks(const QJsonArray &rows) {
  if (!loaded_ || readOnly())
    return;
  if (const auto error = ProjectDatabase::writeProjectFile(
          filePath_, ProjectDatabase::BookmarksSuffix,
          QJsonDocument(ProjectDatabase::bookmarkFile(rows)));
      !error.isEmpty()) {
    setError(error);
    return;
  }
  if (openedFromDatabase_) {
    databaseDirty_ = true;
    emit stateChanged();
  }
}

void Session::watchProject() {
  // The input's folder shows the project folder appear, as a pull or a
  // first edit makes it; the project folder shows its files replaced.
  projectWatcher_.addPath(QFileInfo(filePath_).absolutePath());
  if (const QString folder = ProjectDatabase::projectFolder(filePath_);
      QFileInfo(folder).isDir() &&
      !projectWatcher_.directories().contains(folder))
    projectWatcher_.addPath(folder);
  const auto current = projectWatcher_.files();
  for (const auto &suffix : ProjectDatabase::sidecarSuffixes()) {
    const QString path = ProjectDatabase::projectFilePath(filePath_, suffix);
    QFile file(path);
    projectFiles_.insert(suffix, file.open(QIODevice::ReadOnly) ? file.readAll()
                                                                : QByteArray());
    // A file replaced as a whole is a new file to watch.
    if (file.exists() && !current.contains(path))
      projectWatcher_.addPath(path);
  }
}

void Session::syncProject() {
  if (!loaded_ || opening_ || !transition_.isEmpty())
    return;
  const auto before = std::exchange(projectFiles_, {});
  watchProject();
  if (projectFiles_ == before && !before.isEmpty())
    return;
  if (!before.isEmpty() &&
      projectFiles_.value(QLatin1String(ProjectDatabase::BookmarksSuffix)) !=
          before.value(QLatin1String(ProjectDatabase::BookmarksSuffix)))
    emit projectFileChanged(QLatin1String(ProjectDatabase::BookmarksSuffix));
  // A read-only project follows the writer's files as they are.
  if (readOnly()) {
    if (!before.isEmpty())
      command(QStringLiteral("reload"), {}, {});
    return;
  }
  if (!capabilities_.contains(QStringLiteral("project_sync")))
    return;
  command(
      QStringLiteral("project_sync"), {},
      [this](const QJsonObject &result) {
        if (!result.value("changed").toBool())
          return;
        refreshHistory();
        emit functionsChanged();
        const qint64 kept = result.value("comments_kept").toInteger();
        emit message(
            kept ? tr("The project's files changed on disk and were loaded; "
                      "undo starts here. Unsaved comments kept on top: %1")
                       .arg(kept)
                 : tr("The project's files changed on disk and were loaded; "
                      "undo starts here."),
            0);
      },
      [this](const QString &code, const QString &) {
        if (code == QLatin1String("input_changed"))
          emit message(tr("The input file changed on disk; reload it to "
                          "continue editing."),
                       1);
      });
}

void Session::recordDebugChoices(bool loadedNow) {
  if (!loaded_ || readOnly())
    return;
  const QDir folder = QFileInfo(filePath_).dir();
  const QJsonObject chosen[] = {loadOptions_.pdb, loadOptions_.dwarf,
                                loadOptions_.map, loadOptions_.symbols};
  QJsonObject choices;
  for (std::size_t i = 0; i < std::size(DebugChoices); ++i)
    if (!chosen[i].isEmpty())
      choices.insert(QLatin1String(DebugChoices[i].Key),
                     withRelativePath(chosen[i], folder));
  if (choices.isEmpty())
    return;
  // The same choices leave the file, and an opened database, as they are.
  if (const auto saved = ProjectDatabase::readProjectFile(
          filePath_, ProjectDatabase::DebugFilesSuffix);
      saved && saved->object() == choices)
    return;
  if (const auto error = ProjectDatabase::writeProjectFile(
          filePath_, ProjectDatabase::DebugFilesSuffix, QJsonDocument(choices));
      !error.isEmpty()) {
    emit message(error, 1);
    return;
  }
  // A database written before projects kept their debug files still names
  // the choices it opened with; only a newly loaded file makes it out of
  // date.
  if (openedFromDatabase_ && loadedNow) {
    databaseDirty_ = true;
    emit stateChanged();
  }
}

bool Session::acceptsEdit(std::optional<quint64> epoch) {
  if (!loaded_)
    return false;
  if (epoch && *epoch != queries_.sessionEpoch()) {
    emit message(tr("The edit was prepared for a session that is no longer "
                    "open and was not applied."),
                 1);
    return false;
  }
  if (!transition_.isEmpty()) {
    emit message(tr("Edits are paused while the pending save, open or close "
                    "completes."),
                 1);
    return false;
  }
  return true;
}

void Session::defineItem(Address address, const QString &action,
                         std::optional<quint64> epoch) {
  if (!acceptsEdit(epoch))
    return;
  // Like a rename, a data item commits at once over saved comments.
  if (dirty_)
    save();
  command(
      QStringLiteral("item_define"),
      {{"address", hexAddress(address)}, {"action", action}},
      [this, address, action](const QJsonObject &result) {
        refreshHistory();
        // The item the cursor was in starts at the address answered.
        const auto at = displayAddress(
            addressValue(result.value("address")).value_or(address));
        if (!result.value("saved").toBool())
          emit message((action == QLatin1String("code")
                            ? tr("An instruction is already defined at %1")
                            : tr("The bytes at %1 are already undefined"))
                           .arg(at),
                       0);
        else if (action == QLatin1String("undefine"))
          emit message(tr("Undefined the item at %1").arg(at), 0);
        else
          emit message(
              tr("Defined %1 at %2").arg(result.value("kind").toString(), at),
              0);
      });
}

void Session::formatOperand(Address address, std::optional<int> operand,
                            const QString &action,
                            std::optional<quint64> epoch) {
  if (!acceptsEdit(epoch))
    return;
  // Like a rename, an operand format commits at once over saved comments.
  if (dirty_)
    save();
  QJsonObject payload{{"address", hexAddress(address)}, {"action", action}};
  if (operand)
    payload.insert(QStringLiteral("operand"), *operand);
  command(QStringLiteral("operand_format"), payload,
          [this](const QJsonObject &) { refreshHistory(); });
}

void Session::formatOperandStruct(Address address, std::optional<int> operand,
                                  const QString &type, qint64 delta,
                                  const QStringList &others,
                                  std::optional<quint64> epoch) {
  if (!acceptsEdit(epoch))
    return;
  if (dirty_)
    save();
  // The worker takes 63 other instructions in a step; more go on in steps
  // of their own.
  constexpr int Step = 63;
  QJsonObject payload{{"address", hexAddress(address)},
                      {"action", QStringLiteral("struct")},
                      {"type", type},
                      {"delta", delta}};
  if (operand)
    payload.insert(QStringLiteral("operand"), *operand);
  if (!others.isEmpty())
    payload.insert(QStringLiteral("also"),
                   QJsonArray::fromStringList(others.mid(0, Step)));
  const QStringList rest = others.mid(Step);
  command(QStringLiteral("operand_format"), payload,
          [this, type, delta, rest,
           count = int(others.size()) + 1](const QJsonObject &) {
            refreshHistory();
            emit message(
                tr("Showed %n operands as members of %1", "", count).arg(type),
                0);
            // The rest go on in steps of their own, each with its first
            // instruction leading.
            for (qsizetype at = 0; at < rest.size(); at += Step + 1)
              if (const auto next = addressValue(rest.at(at))) {
                QJsonObject more{{"address", hexAddress(*next)},
                                 {"action", QStringLiteral("struct")},
                                 {"type", type},
                                 {"delta", delta}};
                const QStringList also = rest.mid(at + 1, Step);
                if (!also.isEmpty())
                  more.insert(QStringLiteral("also"),
                              QJsonArray::fromStringList(also));
                command(QStringLiteral("operand_format"), more,
                        [this](const QJsonObject &) { refreshHistory(); });
              }
          });
}

void Session::createFunction(Address address, std::optional<quint64> epoch) {
  if (!acceptsEdit(epoch))
    return;
  // Like a rename, a function edit commits at once over saved comments.
  if (dirty_)
    save();
  command(QStringLiteral("function_create"), {{"address", hexAddress(address)}},
          [this, address](const QJsonObject &) {
            refreshHistory();
            emit message(
                tr("Created a function at %1").arg(displayAddress(address)), 0);
          });
}

void Session::deleteFunction(Address entry, std::optional<quint64> epoch) {
  if (!acceptsEdit(epoch))
    return;
  if (dirty_)
    save();
  command(QStringLiteral("function_delete"), {{"address", hexAddress(entry)}},
          [this, entry](const QJsonObject &) {
            refreshHistory();
            emit message(
                tr("Deleted the function at %1").arg(displayAddress(entry)), 0);
          });
}

void Session::editFunction(Address entry, const QJsonObject &attributes,
                           std::optional<quint64> epoch) {
  if (attributes.isEmpty() || !acceptsEdit(epoch))
    return;
  if (dirty_)
    save();
  command(QStringLiteral("function_edit"),
          {{"address", hexAddress(entry)}, {"attributes", attributes}},
          [this, entry](const QJsonObject &) {
            refreshHistory();
            emit message(
                tr("Edited the function at %1").arg(displayAddress(entry)), 0);
          });
}

void Session::rename(Address function, const QString &name,
                     std::optional<quint64> epoch, std::function<void()> then) {
  if (name.trimmed().isEmpty() || !acceptsEdit(epoch))
    return;
  // A rename commits at once, and the worker refuses it over staged comments;
  // save them first so the rename never discards or blocks on them.
  if (dirty_)
    save();
  command(QStringLiteral("rename"),
          {{"address", hexAddress(function)}, {"name", name.trimmed()}},
          [this, function, name, then = std::move(then)](const QJsonObject &) {
            refreshHistory();
            emit renamed(function, name.trimmed());
            emit message(tr("Renamed %1 to %2")
                             .arg(displayAddress(function), name.trimmed()),
                         0);
            if (then)
              then();
          });
}

void Session::editCode(const QJsonObject &edit, quint64 epoch) {
  if (!acceptsEdit(epoch))
    return;
  if (dirty_)
    save();
  command(QStringLiteral("code_edit"), edit,
          [this](const QJsonObject &) { refreshHistory(); });
}

void Session::setComment(Address address, const QString &text,
                         std::optional<quint64> epoch) {
  if (!acceptsEdit(epoch))
    return;
  command(QStringLiteral("annotation_set"),
          {{"address", hexAddress(address)}, {"text", text}},
          [this, address, text](const QJsonObject &) {
            dirty_ = true;
            refreshHistory();
            emit commented(address, text);
          });
}

void Session::undo() {
  if (!canUndo())
    return;
  command(QStringLiteral("undo"), {}, [this](const QJsonObject &payload) {
    history_ = payload;
    emit historyChanged();
  });
}

void Session::redo() {
  if (!canRedo())
    return;
  command(QStringLiteral("redo"), {}, [this](const QJsonObject &payload) {
    history_ = payload;
    emit historyChanged();
  });
}

void Session::loadSignatures(const QString &path, bool tree) {
  if (!loaded_)
    return;
  command(QStringLiteral("signatures_load"),
          {{"path", path}, {"mode", tree ? "auto" : "file"}},
          [this](const QJsonObject &payload) {
            emit message(tr("Signature pack applied: %1 byte matches")
                             .arg(payload.value("byte_matches").toInt()),
                         0);
          });
}

void Session::loadPdb(const QJsonObject &selection) {
  if (!loaded_ || !loadsPdbFiles())
    return;
  command(QStringLiteral("pdb_load"), selection,
          [this](const QJsonObject &result) {
            loadOptions_.pdb = {{"path", result.value("path")},
                                {"mode", result.value("mode")},
                                {"stamp", result.value("stamp")}};
            functionCount_ =
                result.value("function_count").toInteger(functionCount_);
            metadata_.insert(QStringLiteral("debug_info"), result);
            metadata_.insert(QStringLiteral("pdb"), loadOptions_.pdb);
            metadata_.remove(QStringLiteral("pdb_download"));
            recordDebugChoices(true);
            emit functionsChanged();
            emit stateChanged();
            reportPdb(result);
          });
}

void Session::loadMap(const QJsonObject &selection) {
  if (!loaded_ || !loadsMapFiles())
    return;
  command(QStringLiteral("map_load"), selection,
          [this](const QJsonObject &result) {
            loadOptions_.map = {{"path", result.value("path")},
                                {"mode", result.value("mode")},
                                {"stamp", result.value("stamp")}};
            functionCount_ =
                result.value("function_count").toInteger(functionCount_);
            metadata_.insert(QStringLiteral("debug_info"),
                             result.value("debug_info"));
            metadata_.insert(QStringLiteral("map"), loadOptions_.map);
            recordDebugChoices(true);
            emit functionsChanged();
            emit stateChanged();
            reportMaps(result.value("debug_info").toObject());
          });
}

void Session::loadDwarf(const QJsonObject &selection) {
  if (!loaded_ || !loadsDwarfFiles())
    return;
  command(QStringLiteral("dwarf_load"), selection,
          [this](const QJsonObject &result) {
            loadOptions_.dwarf = {{"path", result.value("path")},
                                  {"stamp", result.value("stamp")}};
            functionCount_ =
                result.value("function_count").toInteger(functionCount_);
            metadata_.insert(QStringLiteral("debug_info"),
                             result.value("debug_info"));
            metadata_.insert(QStringLiteral("dwarf"), loadOptions_.dwarf);
            recordDebugChoices(true);
            emit functionsChanged();
            emit stateChanged();
            emit message(tr("DWARF loaded: %1")
                             .arg(QDir::toNativeSeparators(
                                 result.value("path").toString())),
                         0);
          });
}

void Session::reportMaps(const QJsonObject &debug) {
  for (const auto &value : debug.value("files").toArray()) {
    const auto file = value.toObject();
    if (file.value("kind").toString() != QLatin1String("map"))
      continue;
    const QString path =
        QDir::toNativeSeparators(file.value("path").toString());
    const qint64 names = file.value("names").toInteger();
    if (file.value("match").toString() == QLatin1String("mismatch"))
      emit message(
          tr("MAP loaded unchecked: %1 (%2 names added, of another build)")
              .arg(path)
              .arg(names),
          1);
    else
      emit message(tr("MAP loaded: %1 (%2 names added)").arg(path).arg(names),
                   0);
  }
}

void Session::reportPdb(const QJsonObject &result) {
  emit message(
      (result.value("verified").toBool()
           ? tr("PDB loaded: %1 (%2 names, identity verified)")
       : result.value("mode").toString() == QLatin1String("analysis")
           ? tr("PDB loaded for analysis: %1 (%2 names, user confirmed, "
                "identity unverified)")
           : tr("PDB display names imported: %1 (%2 names, unverified)"))
          .arg(QDir::toNativeSeparators(result.value("path").toString()))
          .arg(result.value("name_count").toInteger()),
      result.value("verified").toBool() ? 0 : 1);
}

void Session::declareTypes(const QString &language, const QString &source,
                           Reply done, Failure failed) {
  command(QStringLiteral("local_types_declare"),
          {{"language", language}, {"source", source}}, std::move(done),
          std::move(failed));
}

void Session::forgetTypes(const QStringList &names, Reply done,
                          Failure failed) {
  command(QStringLiteral("local_types_forget"),
          {{"names", QJsonArray::fromStringList(names)}}, std::move(done),
          std::move(failed));
}

void Session::setStringOptions(const QStringList &encodings,
                               const QString &preferred, int minLength) {
  QSettings settings;
  settings.setValue(StringEncodingsKey, encodings);
  settings.setValue(StringPreferredKey, preferred);
  settings.setValue(StringMinLengthKey, minLength);
  applyStringOptions(true);
}

bool Session::stringOptionsNeedMigration() const {
  const QSettings settings;
  return settings.contains(StringEncodingsKey) &&
         !settings.contains(StringPreferredKey);
}

void Session::migrateStringOptions(const QJsonArray &encodings) {
  QSettings settings;
  const auto saved = settings.value(StringEncodingsKey).toStringList();
  QStringList names;
  QString preferred;
  for (const auto &value : encodings) {
    const auto encoding = value.toObject();
    const auto name = encoding.value("name").toString();
    const bool legacy = encoding.value("legacy").toBool();
    if (legacy && saved.contains(name))
      preferred = name;
    if ((!legacy && saved.contains(name)) || encoding.value("default").toBool())
      names.append(name);
  }
  if (names.isEmpty())
    return;
  settings.setValue(StringEncodingsKey, names);
  settings.setValue(StringPreferredKey, preferred);
}

void Session::applyStringOptions(bool announce) {
  const QSettings settings;
  if (!settings.contains(StringEncodingsKey))
    return;
  const auto encodings = settings.value(StringEncodingsKey).toStringList();
  const auto preferred = settings.value(StringPreferredKey).toString();
  const int minLength = settings.value(StringMinLengthKey).toInt();
  QJsonObject payload{{"encodings", QJsonArray::fromStringList(encodings)}};
  if (!preferred.isEmpty())
    payload.insert("preferred", preferred);
  if (minLength > 0)
    payload.insert("min_length", minLength);
  command(QStringLiteral("string_options"), payload,
          [this, announce](const QJsonObject &options) {
            if (!announce)
              return;
            QStringList names;
            for (const auto &name : options.value("encodings").toArray())
              names.append(name.toString());
            const auto first = options.value("preferred").toString();
            const int columns = options.value("min_length").toInt();
            emit message(first.isEmpty()
                             ? tr("Strings: %1, at least %2 columns")
                                   .arg(names.join(QStringLiteral(", ")))
                                   .arg(columns)
                             : tr("Strings: %1, %2 first, at least %3 columns")
                                   .arg(names.join(QStringLiteral(", ")), first)
                                   .arg(columns),
                         0);
            emit stringOptionsChanged();
          });
}

void Session::analyzeWholeProgram() {
  if (!loaded_)
    return;
  emit message(tr("Whole-program analysis started; views stay available."), 0);
  command(QStringLiteral("analyze"), {}, [this](const QJsonObject &payload) {
    metadata_ = payload;
    emit message(tr("Whole-program analysis finished"), 0);
  });
}

void Session::cancelReads() {
  // Session bookkeeping and external (MCP) callers keep their replies.
  QSet<QObject *> keep{&reads_, &external_};
  analysis_.cancelReads(keep);
  // Retained analysis requests may still need a queued project snapshot.
  // A cancelled replica already removes its own snapshot subscription.
  keep.unite(analysis_.snapshotOwners());
  queries_.cancelReads(keep);
  emit message(tr("Queued requests cancelled; a running engine call finishes "
                  "unless the worker is restarted."),
               1);
}

void Session::externalQuery(const QString &id, const QString &operation,
                            const QJsonObject &payload,
                            const QString &revision) {
  if (!connected_ || !loaded_) {
    emit externalResponse(
        id, {{"status", "error"},
             {"error", QJsonObject{{"code", "unavailable"},
                                   {"message", "No active project"}}}});
    return;
  }
  QueryService::QuerySpec spec{operation, payload};
  if (!revision.isEmpty()) {
    spec.policy = QueryService::QuerySpec::Exact;
    spec.expectedRevision = revision;
  }
  auto &service = AnalysisService::handles(operation)
                      ? analysis_.externalQueries()
                      : queries_;
  service.subscribe(std::move(spec), &external_,
                    [this, id](const QJsonObject &response) {
                      emit externalResponse(id, response);
                    });
}

void Session::publishSelection(Address address, std::optional<Address> function,
                               const QString &view) {
  QJsonObject next{{"address", hexAddress(address)},
                   {"view", view},
                   {"project_id", queries_.projectId()},
                   {"revision", queries_.revision()},
                   {"loaded", loaded_}};
  if (function)
    next.insert("function_address", hexAddress(*function));
  if (next == selection_)
    return;
  selection_ = next;
  emit selectionChanged(selection_);
}

void Session::refreshContributions() {
  read(
      QStringLiteral("contributions"), {}, &reads_,
      [this](const QJsonObject &payload) {
        contributions_ = payload.value("items").toArray();
        emit contributionsChanged();
      },
      [](const QString &, const QString &) {});
}

void Session::registerContributions(const QString &manifestPath) {
  command(QStringLiteral("contribution_register"), {{"path", manifestPath}},
          [this](const QJsonObject &payload) {
            contributions_ = payload.value("items").toArray();
            emit contributionsChanged();
          });
}

void Session::unregisterContributions(const QString &nameSpace) {
  command(QStringLiteral("contribution_unregister"), {{"namespace", nameSpace}},
          [this](const QJsonObject &payload) {
            contributions_ = payload.value("items").toArray();
            emit contributionsChanged();
          });
}

void Session::executeContribution(const QString &id,
                                  std::optional<Address> address) {
  QJsonObject payload{{"id", id}};
  if (address)
    payload.insert("address", hexAddress(*address));
  // Contributions are declarative reads; they never mutate the project.
  read(QStringLiteral("contribution_execute"), payload, &reads_,
       [this](const QJsonObject &result) { emit contributionResult(result); });
}

} // namespace neverd::gui
