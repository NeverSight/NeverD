#include "Workbench.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QScreen>
#include <QSet>
#include <QStandardPaths>
#include <QWindow>
#include <utility>

Workbench::Workbench(QString workerPath, QObject *parent)
    : QObject(parent), queries_(&client_), panes_(&queries_),
      workerPath_(std::move(workerPath)),
      sessionEpoch_(queries_.sessionEpoch()) {
  status_ = QT_TR_NOOP("Open a binary to begin");
  filterTimer_.setSingleShot(true);
  filterTimer_.setInterval(180);
  connect(&filterTimer_, &QTimer::timeout, this, [this] {
    nextFunction_ = 0;
    loadFunctions(false);
  });
  connect(&functions_, &PageModel::pageRequested, this,
          &Workbench::loadFunctionPage);
  connect(&panes_, &PaneRegistry::changed, this, &Workbench::changed);
  connect(&panes_, &PaneRegistry::selectionChanged, this,
          [this] { emit selectionChanged(selection()); });
  connect(&panes_, &PaneRegistry::paneError, this,
          [this](const QString &id, const QString &message) {
            if (id == panes_.activePaneId() || id == representationPane()->id())
              setError(message);
            else
              log(message);
          });
  connect(&queries_, &QueryService::contextChanged, this, [this] {
    if (sessionEpoch_ != queries_.sessionEpoch()) {
      sessionEpoch_ = queries_.sessionEpoch();
      analysisPublishedEpoch_ = 0;
      loaded_ = false;
      dirty_ = false;
      clearViews();
    }
    revision_ = queries_.revision();
    projectId_ = queries_.projectId();
    emit changed();
    emit selectionChanged(selection());
  });
  connect(&queries_, &QueryService::analysisCompleted, this,
          [this] { publishAnalysisCompletion(queries_.sessionEpoch()); });
  connect(&queries_, &QueryService::pendingChanged, this, [this] {
    finishTransition();
    if (!busy() && loaded_ && error_.isEmpty())
      status_ = QT_TR_NOOP("Ready");
    emit changed();
  });
  connect(&client_, &EngineClient::message, this, &Workbench::receive);
  connect(&client_, &EngineClient::diagnostic, this, &Workbench::log);
  connect(&client_, &EngineClient::failure, this, &Workbench::setError);
  connect(&client_, &EngineClient::stopped, this, [this] {
    connected_ = false;
    opening_ = false;
    resetSessionRequests();
    emit changed();
    emit selectionChanged(selection());
  });
  language_ = QSettings().value("ui/language", "en").toString();
  if (!languages().contains(language_))
    language_ = "en";
  queries_.setCacheBudgetMiB(
      QSettings().value("analysis/cacheMiB", 256).toInt());
}

Workbench::~Workbench() {
  disconnect(&queries_, nullptr, this, nullptr);
  disconnect(&panes_, nullptr, this, nullptr);
  disconnect(&client_, nullptr, this, nullptr);
}

void Workbench::setQmlEngine(QQmlEngine *engine) {
  qmlEngine_ = engine;
  setLanguage(language_);
}
QString Workbench::fileName() const { return QFileInfo(filePath_).fileName(); }
QString Workbench::dockLayoutPath() const {
  if (!dockLayoutOverride_.isEmpty())
    return dockLayoutOverride_;
  const auto directory =
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  QDir().mkpath(directory);
  return directory + "/layout.json";
}
void Workbench::copyText(const QString &text) {
  QGuiApplication::clipboard()->setText(text);
}
void Workbench::clampWindows() {
  for (auto *window : QGuiApplication::topLevelWindows()) {
    if (!window->isVisible())
      continue;
    bool onScreen = false;
    for (auto *screen : QGuiApplication::screens())
      if (screen->availableGeometry().intersects(window->frameGeometry())) {
        onScreen = true;
        break;
      }
    if (!onScreen && QGuiApplication::primaryScreen()) {
      const auto available =
          QGuiApplication::primaryScreen()->availableGeometry();
      window->resize(qMin(window->width(), available.width()),
                     qMin(window->height(), available.height()));
      window->setPosition(available.topLeft());
    }
  }
}
QStringList Workbench::languages() const {
  return {"en", "zh-CN", "zh-TW", "ja", "ko", "fr",
          "de", "es",    "it",    "ru", "ar"};
}
void Workbench::setLanguage(const QString &language) {
  if (!languages().contains(language))
    return;
  QCoreApplication::removeTranslator(&translator_);
  auto resourceLocale = language;
  resourceLocale.replace('-', '_');
  if (language != "en" &&
      translator_.load(":/i18n/neverd_" + resourceLocale + ".qm"))
    QCoreApplication::installTranslator(&translator_);
  language_ = language;
  QSettings().setValue("ui/language", language);
  if (qmlEngine_)
    qmlEngine_->retranslate();
  emit languageChanged();
  emit changed();
}
void Workbench::log(const QString &message) {
  if (message.isEmpty())
    return;
  log_ += message;
  if (!log_.endsWith('\n'))
    log_ += '\n';
  if (log_.size() > 128 * 1024)
    log_ = log_.right(128 * 1024);
  emit changed();
}
void Workbench::setError(const QString &message) {
  error_ = message;
  status_ = QT_TR_NOOP("Action failed");
  log(message);
  emit changed();
}

PaneController *Workbench::selectedPane() const {
  return panes_.activePane() ? panes_.activePane() : panes_.defaultMachine();
}
PaneController *Workbench::machinePane() const {
  auto *active = panes_.activePane();
  return active && active->kind() == "machine" ? active
                                               : panes_.defaultMachine();
}
PaneController *Workbench::representationPane() const {
  auto *active = panes_.activePane();
  return active && active->kind() == "representation"
             ? active
             : panes_.defaultRepresentation();
}
QJsonObject Workbench::selection() const {
  auto selection = selectedPane()->selection();
  selection["project_id"] = projectId_;
  selection["revision"] = revision_;
  selection["representation"] = representation();
  selection["loaded"] = loaded_ && connected_;
  return selection;
}

QString Workbench::send(const QString &operation, const QJsonObject &payload,
                        Callback callback, bool mutation,
                        FailureCallback failed) {
  static const QSet<QString> commands{"open",
                                      "analyze",
                                      "rename",
                                      "annotation_set",
                                      "save",
                                      "reload",
                                      "undo",
                                      "redo",
                                      "history_reset",
                                      "contribution_register",
                                      "contribution_unregister"};
  if (opening_ && mutation) {
    const auto detail = tr("Finish opening the binary before editing.");
    setError(detail);
    if (failed)
      QTimer::singleShot(0, this, [failed, detail] {
        failed({{"status", "error"},
                {"error", QJsonObject{{"code", "session_changing"},
                                      {"message", detail}}}});
      });
    return {};
  }
  const auto epoch = queries_.sessionEpoch();
  const auto counted = std::make_shared<bool>(mutation);
  if (mutation)
    ++pendingWrites_;
  auto complete = [this, epoch, operation, counted,
                   callback = std::move(callback),
                   failed = std::move(failed)](const QJsonObject &response) {
    const bool openingSuccess = operation == "open" &&
                                response["status"] == "ok" &&
                                queries_.sessionEpoch() == epoch + 1;
    if (epoch == queries_.sessionEpoch() || openingSuccess) {
      const auto payload = response["payload"].toObject();
      if (response["status"] == "ok") {
        if (payload.contains("dirty"))
          dirty_ = payload["dirty"].toBool();
        if (callback)
          callback(payload, response);
      } else {
        const auto code = response["error"].toObject()["code"].toString();
        if (response["status"] != "cancelled" && code != "worker_stopped" &&
            code != "session_changed")
          setError(response["error"].toObject()["message"].toString(
              response["status"].toString()));
        if (failed)
          failed(response);
        if (*counted && operation == "save") {
          transitionReady_ = false;
          transition_.clear();
        }
      }
    }
    if (*counted)
      --pendingWrites_;
    emit changed();
    // QueryService emits pendingChanged after completing this delivery. The
    // transition then observes both the callback's result and retired write.
  };
  const auto id =
      commands.contains(operation)
          ? queries_.enqueueCommand(operation, payload, this,
                                    std::move(complete))
          : queries_.subscribe({operation, payload}, &workspaceReads_,
                               std::move(complete));
  if (!id && *counted) {
    *counted = false;
    --pendingWrites_;
  }
  return id ? QString::number(id) : QString{};
}
void Workbench::resetSessionRequests() {
  queries_.setAvailable(false);
  panes_.setLoaded(false);
  functionRequest_ = false;
  ++filterGeneration_;
}
void Workbench::receive(const QJsonObject &message) {
  const auto type = message["type"].toString();
  if (type == "hello") {
    if (message["protocol_major"].toInt() != 1) {
      setError(tr("Incompatible analysis worker protocol."));
      client_.stop();
      return;
    }
    connected_ = true;
    queries_.setAvailable(true);
    log(tr("Connected to %1").arg(message["engine_version"].toString()));
    emit changed();
    emit ready();
    openPending();
  } else if (type == "fatal") {
    setError(message["error"].toObject()["message"].toString());
    client_.stop();
  } else if (type == "heartbeat") {
    emit changed();
  }
}
void Workbench::clearViews() {
  filterTimer_.stop();
  ++filterGeneration_;
  functions_.resetPages();
  historyState_ = {};
  contributions_.clear();
  contributionResult_.clear();
  functionCount_ = nextFunction_ = 0;
  functionRequest_ = false;
}
void Workbench::openFile(const QUrl &url) {
  const auto path = url.isLocalFile() ? url.toLocalFile() : url.toString();
  const QFileInfo file(path);
  if (!file.isFile()) {
    setError(tr("Select an existing binary file."));
    return;
  }
  pendingFile_ = file.absoluteFilePath();
  if (unsavedChanges()) {
    requestTransition("open");
    return;
  }
  if (!connected_) {
    status_ = QT_TR_NOOP("Starting analysis worker…");
    client_.start(workerPath_);
    emit changed();
  } else {
    openPending();
  }
}
void Workbench::openPending() {
  if (pendingFile_.isEmpty() || opening_ || !connected_)
    return;
  // A Cancel/new navigation during loaded notifications must invalidate the
  // deferred startup entry fallback, including before history is subscribed.
  const auto navigation = panes_.navigationRevision();
  const auto path = std::exchange(pendingFile_, {});
  opening_ = true;
  error_.clear();
  status_ = QT_TR_NOOP("Opening binary…");
  send(
      "open", {{"path", path}},
      [this, path, navigation](const auto &payload, const auto &) {
        opening_ = false;
        dirty_ = false;
        metadata_ = payload;
        filePath_ = path;
        loaded_ = true;
        functionCount_ = payload["function_count"].toInt();
        panes_.setLoaded(true);
        status_ = QT_TR_NOOP("Binary loaded");
        QSettings().setValue("files/last", filePath_);
        // A later open gesture is enqueued only after this replacement
        // established its new epoch. Never replay it as an old-project queued
        // command.
        if (!pendingFile_.isEmpty()) {
          openPending();
          return;
        }
        loadFunctions(false);
        const auto entry = payload["entry_address"].toString();
        const auto epoch = queries_.sessionEpoch();
        send(
            "history", {{"limit", 1}},
            [this, entry, epoch, navigation](const auto &history,
                                             const auto &) {
              if (epoch != queries_.sessionEpoch())
                return;
              historyState_ = history;
              panes_.setBinaryIdentity(history["source_sha256"].toString());
              if (navigation == panes_.navigationRevision() &&
                  selectedAddress().isEmpty() && !entry.isEmpty())
                navigate(entry);
            },
            false,
            [this, entry, epoch, navigation](const auto &) {
              if (epoch == queries_.sessionEpoch() &&
                  navigation == panes_.navigationRevision() &&
                  selectedAddress().isEmpty() && !entry.isEmpty())
                navigate(entry);
            });
        send("contributions", {}, [this](const auto &result, const auto &) {
          contributions_ = result["items"].toArray().toVariantList();
        });
      },
      false,
      [this](const auto &) {
        opening_ = false;
        if (!pendingFile_.isEmpty())
          openPending();
      });
  emit changed();
}

void Workbench::filterFunctions(const QString &filter) {
  filter_ = filter;
  ++filterGeneration_;
  // Retire selectable rows immediately; only the worker query is debounced.
  functions_.resetPages();
  nextFunction_ = 0;
  functionCount_ = 0;
  functionRequest_ = false;
  filterTimer_.start();
  emit changed();
}
void Workbench::publishAnalysisCompletion(quint64 epoch) {
  if (!loaded_ || epoch != queries_.sessionEpoch() ||
      !queries_.analysisComplete() || analysisPublishedEpoch_ == epoch)
    return;
  // Mark before changing the model: its observers may synchronously reenter.
  analysisPublishedEpoch_ = epoch;
  const QPointer<Workbench> guard(this);
  loadFunctions(false);
  if (guard && loaded_ && queries_.sessionEpoch() == epoch)
    panes_.repairAnalysisLocations();
}

void Workbench::loadFunctions(bool append) {
  if (!loaded_ || (append && (nextFunction_ < 0 || functionRequest_)))
    return;
  if (!append)
    functions_.resetPages();
  loadFunctionPage(append ? nextFunction_ : 0);
}
void Workbench::loadFunctionPage(int offset) {
  if (!loaded_ || offset < 0 || !functions_.beginPageRequest(offset))
    return;
  const auto filterGeneration = filterGeneration_;
  const auto pageGeneration = functions_.requestGeneration();
  const auto project = projectId_;
  functionRequest_ = true;
  send(
      "functions", {{"offset", offset}, {"limit", 256}, {"filter", filter_}},
      [this, offset, filterGeneration, pageGeneration,
       project](const auto &payload, const auto &) {
        if (filterGeneration != filterGeneration_ ||
            pageGeneration != functions_.requestGeneration() ||
            project != projectId_)
          return;
        functionRequest_ = false;
        if (!functions_.setPage(offset, payload["total"].toInt(-1),
                                payload["items"].toArray())) {
          functions_.pageRequestFailed(offset);
          return;
        }
        nextFunction_ = payload["next_offset"].isNull()
                            ? -1
                            : payload["next_offset"].toInt(-1);
        functionCount_ = payload["total"].toInt();
      },
      false,
      [this, offset, filterGeneration, pageGeneration, project](const auto &) {
        if (filterGeneration != filterGeneration_ ||
            pageGeneration != functions_.requestGeneration() ||
            project != projectId_)
          return;
        functionRequest_ = false;
        functions_.pageRequestFailed(offset);
      });
}
void Workbench::loadMoreFunctions() { loadFunctions(true); }

void Workbench::selectFunction(const QString &address) { navigate(address); }
void Workbench::navigate(const QString &query) {
  selectedPane()->navigate(query);
}
void Workbench::selectInstruction(const QString &address) {
  selectedPane()->selectInstruction(address);
}
void Workbench::selectTextLine(int line) {
  representationPane()->selectTextLine(line);
}
void Workbench::goBack() { selectedPane()->goBack(); }
void Workbench::goForward() { selectedPane()->goForward(); }
void Workbench::loadMoreInstructions() {
  machinePane()->loadMoreInstructions();
}
void Workbench::loadMoreText() { representationPane()->loadMoreText(); }
void Workbench::reloadRepresentation() {
  representationPane()->reloadRepresentation();
}
void Workbench::setRepresentation(const QString &representation) {
  representationPane()->setRepresentation(representation);
}
void Workbench::toggleRepresentationPin() {
  representationPane()->toggleRepresentationPin();
}
void Workbench::requestView(const QString &view) {
  machinePane()->requestView(view);
}
void Workbench::requestGraphViewport(double x, double y, double width,
                                     double height, double scale) {
  machinePane()->requestGraphViewport(x, y, width, height, scale);
}
void Workbench::analyze() {
  if (!loaded_)
    return;
  const auto epoch = queries_.sessionEpoch();
  const bool previouslyPublished = analysisPublishedEpoch_ == epoch;
  const QPointer<Workbench> guard(this);
  panes_.resumeAnalysisReads();
  status_ = QT_TR_NOOP("Analyzing…");
  send("analyze", {},
       [this, epoch, previouslyPublished](const auto &payload, const auto &) {
         metadata_ = payload;
         status_ = QT_TR_NOOP("Ready");
         // First analysis publishes after the command's complete delivery. A
         // later explicit Analyze (or an older worker) retains its explicit
         // list refresh.
         if (previouslyPublished || !queries_.analysisComplete()) {
           const QPointer<Workbench> guard(this);
           loadFunctions(false);
           if (guard && loaded_ && queries_.sessionEpoch() == epoch)
             panes_.repairAnalysisLocations();
         }
       });
  // Queue visible reads behind Analyze now. A subsequent Cancel can retire
  // them; no delayed command callback may resume a cancelled pane.
  if (guard && loaded_ && queries_.sessionEpoch() == epoch)
    panes_.refreshAnalysisViews();
  if (guard)
    emit changed();
}
void Workbench::cancel() {
  panes_.cancelReads();
  // The function browser, annotation state and external callers remain useful
  // while a pane's analysis is cancelled; they have independent ownership.
  status_ = QT_TR_NOOP(
      "Cancellation requested; restart stops the worker immediately.");
  emit changed();
}
void Workbench::requestTransition(const QString &action) {
  transition_ = action;
  transitionReady_ = false;
  emit confirmSessionChange();
}
bool Workbench::requestClose() {
  if (!unsavedChanges())
    return true;
  requestTransition("close");
  return false;
}
void Workbench::resolveSessionChange(const QString &choice) {
  if (transition_.isEmpty())
    return;
  if (choice == "cancel") {
    transition_.clear();
    transitionReady_ = false;
    pendingFile_.clear();
  } else if (choice == "save") {
    send(
        "save", {},
        [this](const auto &, const auto &) {
          dirty_ = false;
          transitionReady_ = true;
        },
        true);
  } else if (choice == "discard") {
    transitionReady_ = true;
    finishTransition();
  }
}
void Workbench::finishTransition() {
  if (!transitionReady_ || pendingWrites_ > 0)
    return;
  const auto action = std::exchange(transition_, {});
  transitionReady_ = false;
  if (action == "close") {
    emit closeReady();
  } else if (action == "restart") {
    performRestart();
  } else if (action == "open") {
    openPending();
  } else if (action == "reload") {
    send(
        "reload", {},
        [this](const auto &, const auto &) {
          dirty_ = false;
          refreshHistory();
          panes_.refreshAnnotations();
          loadFunctions(false);
          status_ = QT_TR_NOOP("Annotations reloaded");
        },
        true);
  }
}
void Workbench::restartWorker() {
  if (unsavedChanges()) {
    requestTransition("restart");
    return;
  }
  performRestart();
}
void Workbench::performRestart() {
  pendingFile_ = filePath_;
  // Retire the physical transport before the local epoch can dispatch work.
  client_.stop();
  resetSessionRequests();
  loaded_ = connected_ = opening_ = false;
  dirty_ = false;
  clearViews();
  status_ = QT_TR_NOOP("Restarting analysis worker…");
  client_.start(workerPath_);
  emit changed();
  emit selectionChanged(selection());
}
QVariantMap Workbench::captureCommandTarget() const {
  return {{"session_epoch", QString::number(queries_.sessionEpoch())},
          {"project_id", projectId_},
          {"address", selectedAddress()},
          {"function_address", selectedFunctionAddress()}};
}
bool Workbench::validCommandTarget(const QVariantMap &target) const {
  return loaded_ && connected_ && !opening_ && transition_.isEmpty() &&
         target["project_id"].toString() == projectId_ &&
         target["session_epoch"].toString() ==
             QString::number(queries_.sessionEpoch());
}
void Workbench::renameFunction(const QString &name) {
  renameFunctionAt(captureCommandTarget(), name);
}
void Workbench::renameFunctionAt(const QVariantMap &target,
                                 const QString &name) {
  const auto address = target["function_address"].toString();
  if (!validCommandTarget(target) || address.isEmpty() ||
      name.trimmed().isEmpty())
    return;
  send(
      "rename", {{"address", address}, {"name", name}},
      [this, name, address](const auto &, const auto &) {
        panes_.renamed(address, name);
        loadFunctions(false);
        refreshHistory();
        status_ = QT_TR_NOOP("Rename saved");
      },
      true);
}
void Workbench::setComment(const QString &comment) {
  setCommentAt(captureCommandTarget(), comment);
}
void Workbench::setCommentAt(const QVariantMap &target,
                             const QString &comment) {
  const auto address = target["address"].toString();
  if (!validCommandTarget(target) || address.isEmpty())
    return;
  send(
      "annotation_set", {{"address", address}, {"text", comment}},
      [this, comment, address](const auto &, const auto &) {
        dirty_ = true;
        panes_.commented(address, comment);
        panes_.refreshAnnotations();
        refreshHistory();
        status_ = QT_TR_NOOP("Comment changed; save annotations to keep it.");
      },
      true);
}
void Workbench::saveAnnotations() {
  send(
      "save", {},
      [this](const auto &, const auto &) {
        dirty_ = false;
        refreshHistory();
        status_ = QT_TR_NOOP("Annotations saved");
      },
      true);
}
void Workbench::loadAnnotations() {
  if (unsavedChanges()) {
    requestTransition("reload");
    return;
  }
  send(
      "reload", {},
      [this](const auto &, const auto &) {
        refreshHistory();
        panes_.refreshAnnotations();
        loadFunctions(false);
        status_ = QT_TR_NOOP("Annotations reloaded");
      },
      true);
}
void Workbench::refreshHistory() {
  if (!loaded_)
    return;
  send("history", {{"limit", 1}}, [this](const auto &payload, const auto &) {
    historyState_ = payload;
    panes_.setBinaryIdentity(payload["source_sha256"].toString());
  });
}
void Workbench::applyHistory(const QString &operation) {
  const QPointer<PaneController> target(selectedPane());
  send(
      operation, {},
      [this, target](const auto &payload, const auto &) {
        historyState_ = payload;
        loadFunctions(false);
        panes_.refreshAnnotations();
        const auto address = payload["address"].toString();
        if (target && !address.isEmpty())
          target->navigate(address);
      },
      true);
}
void Workbench::undo() {
  if (canUndo() && !opening_ && transition_.isEmpty())
    applyHistory("undo");
}
void Workbench::redo() {
  if (canRedo() && !opening_ && transition_.isEmpty())
    applyHistory("redo");
}
void Workbench::importContributions(const QUrl &url) {
  if (!url.isLocalFile())
    return;
  send("contribution_register", {{"path", url.toLocalFile()}},
       [this](const auto &payload, const auto &) {
         contributions_ = payload["items"].toArray().toVariantList();
       });
}
void Workbench::unloadContributions(const QString &nameSpace) {
  send("contribution_unregister", {{"namespace", nameSpace}},
       [this](const auto &payload, const auto &) {
         contributions_ = payload["items"].toArray().toVariantList();
       });
}
void Workbench::runContribution(const QString &id) {
  QJsonObject payload{{"id", id}};
  if (!selectedAddress().isEmpty())
    payload["address"] = selectedAddress();
  send("contribution_execute", payload,
       [this](const auto &result, const auto &) {
         contributionResult_ = QString::fromUtf8(
             QJsonDocument(result).toJson(QJsonDocument::Indented));
       });
}
void Workbench::externalQuery(const QString &id, const QString &operation,
                              const QJsonObject &payload,
                              const QString &revision) {
  if (!connected_ || !loaded_) {
    emit externalResponse(
        id, {{"status", "error"},
             {"error",
              QJsonObject{{"code", "unavailable"},
                          {"message", "No active project or queue is full"}}}});
    return;
  }
  QueryService::QuerySpec spec{operation, payload};
  if (!revision.isEmpty()) {
    spec.policy = QueryService::QuerySpec::Exact;
    spec.expectedRevision = revision;
  }
  // Capture caller identity even for rejected (zero-ID) subscriptions. Session
  // retirement must still deliver exactly one terminal response to this caller.
  queries_.subscribe(std::move(spec), this, [this, id](const auto &response) {
    emit externalResponse(id, response);
  });
}
