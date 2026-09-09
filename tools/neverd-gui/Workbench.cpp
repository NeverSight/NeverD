#include "Workbench.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QQmlEngine>
#include <QScreen>
#include <QStandardPaths>
#include <QWindow>
#include <cmath>

Workbench::Workbench(QString workerPath, QObject *parent)
    : QObject(parent), workerPath_(std::move(workerPath)) {
  status_ = QT_TR_NOOP("Open a binary to begin");
  filterTimer_.setSingleShot(true);
  filterTimer_.setInterval(180);
  connect(&filterTimer_, &QTimer::timeout, this, [this] {
    nextFunction_ = 0;
    loadFunctions(false);
  });
  connect(&functions_, &PageModel::pageRequested, this,
          &Workbench::loadFunctionPage);
  connect(&client_, &EngineClient::message, this, &Workbench::receive);
  connect(&client_, &EngineClient::diagnostic, this, &Workbench::log);
  connect(&client_, &EngineClient::failure, this, &Workbench::setError);
  connect(&client_, &EngineClient::stopped, this, [this] {
    connected_ = false;
    busy_ = false;
    resetSessionRequests();
    emit changed();
    emit selectionChanged(selection());
  });
  language_ = QSettings().value("ui/language", "en").toString();
  if (!languages().contains(language_))
    language_ = "en";
  cache_.setMaxCost(
      qBound(16, QSettings().value("analysis/cacheMiB", 256).toInt(), 1024) *
      1024);
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
QJsonObject Workbench::selection() const {
  return {
      {"project_id", projectId_},       {"revision", revision_},
      {"address", selectedAddress_},    {"function_address", functionAddress_},
      {"function_name", functionName_}, {"representation", representation_},
      {"loaded", loaded_ && connected_}};
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
QString Workbench::send(const QString &operation, const QJsonObject &payload,
                        Callback callback, bool contextual, bool mutation,
                        FailureCallback failed) {
  if (!connected_ ||
      pending_.size() + external_.size() + mutations_.size() >= 48) {
    const auto detail = tr("Worker unavailable or request queue full.");
    setError(detail);
    if (operation == "decompile") {
      textRequest_ = false;
      textStatus_ = detail;
      busy_ = false;
    }
    if (operation == "disasm")
      instructionRequest_ = false;
    if (operation == "open")
      busy_ = false;
    if (failed)
      QTimer::singleShot(0, this, [failed, detail] {
        failed({{"status", "error"},
                {"error",
                 QJsonObject{{"code", "queue_full"}, {"message", detail}}}});
      });
    return {};
  }
  if (mutation) {
    mutations_.enqueue(
        {operation, payload, std::move(callback), std::move(failed)});
    dispatchMutation();
    emit changed();
    return "queued";
  }
  const auto cacheKey =
      contextual ? operation + ':' +
                       QString::fromUtf8(QJsonDocument(payload).toJson(
                           QJsonDocument::Compact))
                 : QString{};
  if (!cacheKey.isEmpty()) {
    if (const auto *cached = cache_.object(cacheKey)) {
      const auto response = *cached;
      const auto generation = generation_;
      const auto revision = revision_, project = projectId_;
      QTimer::singleShot(
          0, this,
          [this, generation, revision, project, response, operation, payload,
           callback, failed] {
            if (generation != generation_ || project != projectId_)
              return;
            if (revision != revision_) {
              send(operation, payload, callback, true, false, failed);
              return;
            }
            if (callback)
              callback(response["payload"].toObject(), response);
            emit changed();
          });
      return "cached";
    }
  }
  const auto id = client_.request(operation, payload);
  pending_.insert(id, {operation, generation_, contextual, std::move(callback),
                       cacheKey, std::move(failed), false});
  return id;
}
void Workbench::dispatchMutation() {
  // A revision is stamped only after previously dispatched work has completed.
  // Pipeline reads may themselves publish a revision, so they are a barrier
  // too.
  if (!connected_ || mutationActive_ || mutations_.isEmpty() ||
      !pending_.isEmpty() || !external_.isEmpty())
    return;
  auto mutation = mutations_.dequeue();
  mutationActive_ = true;
  const auto id =
      client_.request(mutation.operation, mutation.payload, revision_);
  pending_.insert(id, {mutation.operation,
                       generation_,
                       false,
                       std::move(mutation.callback),
                       {},
                       std::move(mutation.failed),
                       true});
}
void Workbench::resetSessionRequests() {
  for (auto it = external_.begin(); it != external_.end(); ++it)
    emit externalResponse(
        it.value(), {{"status", "error"},
                     {"error", QJsonObject{{"code", "worker_stopped"},
                                           {"message", "Worker stopped"}}}});
  external_.clear();
  pending_.clear();
  mutations_.clear();
  mutationActive_ = false;
  functionRequest_ = instructionRequest_ = textRequest_ = false;
  cache_.clear();
  ++generation_;
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
    log(tr("Connected to %1").arg(message["engine_version"].toString()));
    emit changed();
    emit ready();
    openPending();
    return;
  }
  if (type == "fatal") {
    setError(message["error"].toObject()["message"].toString());
    client_.stop();
    return;
  }
  if (type == "heartbeat") {
    activeJob_ = message["active_request_id"].toString();
    busy_ = !activeJob_.isEmpty();
    emit changed();
    return;
  }
  if (type != "response")
    return;
  const auto id = message["request_id"].toString();
  // Administrative cancellation and old process messages never own project
  // state.
  if (!pending_.contains(id) && !external_.contains(id))
    return;
  if (message.contains("revision")) {
    const auto nextRevision = message["revision"].toString();
    if (revision_ != nextRevision)
      cache_.clear();
    revision_ = nextRevision;
  }
  if (message.contains("project_id"))
    projectId_ = message["project_id"].toString();
  emit selectionChanged(selection());
  if (external_.contains(id)) {
    emit externalResponse(external_.take(id), message);
    dispatchMutation();
    finishTransition();
    return;
  }
  auto pending = pending_.take(id);
  if (pending.mutation)
    mutationActive_ = false;
  if (pending.contextual && pending.generation != generation_) {
    dispatchMutation();
    finishTransition();
    return;
  }
  const auto payload = message["payload"].toObject();
  const auto state = message["status"].toString();
  if (state != "ok") {
    const auto detail = message["error"].toObject()["message"].toString(state);
    if (state != "cancelled")
      setError(detail);
    if (pending.operation == "decompile" && !pending.failed) {
      textRequest_ = false;
      textStatus_ = detail;
      busy_ = false;
    }
    if (pending.operation == "functions")
      functionRequest_ = false;
    if (pending.operation == "disasm")
      instructionRequest_ = false;
    if (pending.operation == "open")
      busy_ = false;
    if (pending.failed)
      pending.failed(message);
    if (pending.mutation) {
      mutations_.clear();
      transitionReady_ = false;
      transition_.clear();
    }
    dispatchMutation();
    finishTransition();
    emit changed();
    return;
  }
  if (payload.contains("dirty"))
    dirty_ = payload["dirty"].toBool();
  if (!pending.cacheKey.isEmpty()) {
    const auto bytes =
        QJsonDocument(message).toJson(QJsonDocument::Compact).size();
    cache_.insert(pending.cacheKey, new QJsonObject(message),
                  qMax(1, int((bytes * 4 + 1023) / 1024)));
  }
  if (pending.callback)
    pending.callback(payload, message);
  if (pending.mutation && centralView_ == "cfg" && !graphSummary_.isEmpty())
    requestView("cfg");
  dispatchMutation();
  finishTransition();
  emit changed();
}

void Workbench::clearViews() {
  cache_.clear();
  ++textGeneration_;
  representationPinned_ = false;
  pinnedAddress_.clear();
  pinnedName_.clear();
  historyState_ = {};
  functions_.replace({});
  instructions_.replace({});
  xrefs_.replace({});
  graphSummary_ = {};
  graphViewport_ = {};
  ++graphGeneration_;
  nodes_.clear();
  edges_.clear();
  text_.clear();
  textStatus_.clear();
  hexText_.clear();
  nextInstruction_.clear();
  nextText_ = 0;
  textMappings_.clear();
  mappingRevision_.clear();
  mappingState_.clear();
  functionRequest_ = instructionRequest_ = textRequest_ = false;
}
void Workbench::openFile(const QUrl &url) {
  auto path = url.isLocalFile() ? url.toLocalFile() : url.toString();
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
    busy_ = true;
    status_ = QT_TR_NOOP("Starting analysis worker…");
    client_.start(workerPath_);
    emit changed();
  } else
    openPending();
}
void Workbench::openPending() {
  if (pendingFile_.isEmpty())
    return;
  const auto path = pendingFile_;
  pendingFile_.clear();
  ++generation_;
  busy_ = true;
  error_.clear();
  status_ = QT_TR_NOOP("Opening binary…");
  send("open", {{"path", path}},
       [this, path](const auto &payload, const auto &) {
         clearViews();
         dirty_ = false;
         metadata_ = payload;
         filePath_ = path;
         loaded_ = true;
         busy_ = false;
         history_.clear();
         historyIndex_ = -1;
         selectedAddress_.clear();
         functionAddress_.clear();
         functionCount_ = payload["function_count"].toInt();
         nextFunction_ = 0;
         status_ = QT_TR_NOOP("Binary loaded");
         QSettings().setValue("files/last", filePath_);
         loadFunctions(false);
         const auto entry = payload["entry_address"].toString();
         if (!entry.isEmpty())
           navigate(entry);
         refreshHistory();
         send("contributions", {}, [this](const auto &payload, const auto &) {
           contributions_ = payload["items"].toArray().toVariantList();
         });
       });
  emit changed();
}
void Workbench::filterFunctions(const QString &filter) {
  filter_ = filter;
  ++filterGeneration_;
  filterTimer_.start();
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
      false, false,
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
  if (!loaded_ || query.trimmed().isEmpty())
    return;
  for (auto it = pending_.cbegin(); it != pending_.cend(); ++it)
    if (it->contextual)
      client_.request("cancel", {{"request_id", it.key()}});
  ++generation_;
  instructionRequest_ = false;
  if (!representationPinned_) {
    textRequest_ = false;
    ++textGeneration_;
  }
  const bool record = !replayHistory_;
  replayHistory_ = false;
  send(
      "resolve", {{"query", query.trimmed()}},
      [this, record](const auto &payload, const auto &) {
        functionAddress_ = payload["function_address"].toString();
        selectedComment_ = payload["comment"].toString();
        moveTo(payload["address"].toString(), payload["name"].toString(),
               record);
      },
      true);
}
void Workbench::moveTo(const QString &address, const QString &name,
                       bool recordHistory) {
  selectedAddress_ = address;
  functionName_ = name;
  error_.clear();
  if (recordHistory &&
      (historyIndex_ < 0 || history_.value(historyIndex_) != address)) {
    while (history_.size() > historyIndex_ + 1)
      history_.removeLast();
    history_.append(address);
    if (history_.size() > 256)
      history_.removeFirst();
    historyIndex_ = history_.size() - 1;
  }
  nextInstruction_ = address;
  if (!representationPinned_)
    nextText_ = 0;
  graphSummary_ = {};
  graphViewport_ = {};
  ++graphGeneration_;
  nodes_.clear();
  edges_.clear();
  hexText_.clear();
  xrefs_.replace({});
  if (!representationPinned_) {
    text_.clear();
    textMappings_.clear();
    mappingState_.clear();
  }
  status_ = QT_TR_NOOP("Reading analysis views…");
  loadInstructions(false);
  if (centralView_ != "disasm")
    requestView(centralView_);
  if (!representationPinned_)
    loadText();
  loadXrefs();
  emit selectionChanged(selection());
  emit changed();
}
void Workbench::goBack() {
  if (canGoBack()) {
    --historyIndex_;
    replayHistory_ = true;
    navigate(history_[historyIndex_]);
  }
}
void Workbench::goForward() {
  if (canGoForward()) {
    ++historyIndex_;
    replayHistory_ = true;
    navigate(history_[historyIndex_]);
  }
}
void Workbench::loadInstructions(bool append) {
  if (nextInstruction_.isEmpty() || instructionRequest_)
    return;
  instructionRequest_ = true;
  send(
      "disasm", {{"address", nextInstruction_}, {"limit", 256}},
      [this, append](const auto &payload, const auto &) {
        instructionRequest_ = false;
        if (append)
          instructions_.append(payload["items"].toArray());
        else
          instructions_.replace(payload["items"].toArray());
        nextInstruction_ = payload["next_address"].toString();
        status_ = QT_TR_NOOP("Ready");
      },
      true);
}
void Workbench::loadMoreInstructions() { loadInstructions(true); }
void Workbench::selectInstruction(const QString &address) {
  if (address.isEmpty() || address == selectedAddress_)
    return;
  selectedAddress_ = address;
  selectedComment_.clear();
  loadComment();
  loadXrefs();
  emit changed();
  emit selectionChanged(selection());
}
void Workbench::loadComment() {
  const auto address = selectedAddress_;
  send(
      "resolve", {{"query", address}},
      [this, address](const auto &payload, const auto &) {
        if (address == selectedAddress_)
          selectedComment_ = payload["comment"].toString();
      },
      true);
}
void Workbench::loadText(bool append) {
  const auto address =
      representationPinned_ ? pinnedAddress_ : functionAddress_;
  if (address.isEmpty()) {
    textStatus_ = QT_TR_NOOP("No function at this address");
    return;
  }
  if (textRequest_ || (append && nextText_ < 0))
    return;
  textRequest_ = true;
  busy_ = true;
  textStatus_ = QT_TR_NOOP("Analyzing…");
  const auto representation = representation_;
  const auto textGeneration = ++textGeneration_;
  send(
      "decompile",
      {{"address", address},
       {"representation", representation},
       {"offset", append ? nextText_ : 0},
       {"limit", 512}},
      [this, append, representation, address,
       textGeneration](const auto &payload, const auto &response) {
        if (textGeneration != textGeneration_)
          return;
        if (representation != representation_ ||
            address !=
                (representationPinned_ ? pinnedAddress_ : functionAddress_))
          return;
        textRequest_ = false;
        busy_ = false;
        const auto page = payload["text"].toString();
        // Keep the reader bounded even for huge generated functions.
        if (append && text_.size() + page.size() <= 2 * 1024 * 1024 &&
            mappingRevision_ == response["revision"].toString()) {
          text_ += page;
        } else {
          text_ = page;
          textMappings_.clear();
          textStartLine_ = payload["offset"].toInt();
        }
        mappingRevision_ = response["revision"].toString();
        mappingState_ = payload["mapping_status"].toString();
        for (const auto &value : payload["rows"].toArray()) {
          auto row = value.toObject();
          row["line"] = row["line"].toInt() - textStartLine_;
          textMappings_.append(row.toVariantMap());
        }
        nextText_ = payload["next_offset"].isNull()
                        ? -1
                        : payload["next_offset"].toInt(-1);
        textStatus_ = nextText_ < 0 ? QT_TR_NOOP("Complete")
                                    : QT_TR_NOOP("More lines available");
        status_ = QT_TR_NOOP("Ready");
      },
      !representationPinned_, false,
      [this, textGeneration](const auto &response) {
        if (textGeneration != textGeneration_)
          return;
        textRequest_ = false;
        busy_ = false;
        textStatus_ = response["error"].toObject()["message"].toString(
            response["status"].toString());
      });
}
QString Workbench::mappingStatus() const {
  if (text_.isEmpty())
    return {};
  if (mappingRevision_ != revision_)
    return tr(
        "Mapping belongs to an earlier revision; reload the representation");
  if (mappingState_ == "instruction_anchors")
    return tr("Linked instruction addresses; synthetic rows may be unmapped");
  return tr("Instruction mapping unavailable for this representation");
}
void Workbench::selectTextLine(int line) {
  if (mappingRevision_ != revision_ ||
      (representationPinned_ && pinnedAddress_ != functionAddress_))
    return;
  for (const auto &value : textMappings_) {
    const auto row = value.toMap();
    if (row["line"].toInt() != line)
      continue;
    const auto addresses = row["addresses"].toList();
    if (!addresses.isEmpty())
      selectInstruction(addresses.first().toString());
    return;
  }
}
void Workbench::loadMoreText() { loadText(true); }
void Workbench::reloadRepresentation() {
  textRequest_ = false;
  nextText_ = 0;
  loadText();
}
void Workbench::toggleRepresentationPin() {
  if (!loaded_ || functionAddress_.isEmpty())
    return;
  representationPinned_ = !representationPinned_;
  pinnedAddress_ = representationPinned_ ? functionAddress_ : QString{};
  pinnedName_ = representationPinned_ ? functionName_ : QString{};
  textRequest_ = false;
  nextText_ = 0;
  loadText();
  emit changed();
}
void Workbench::setRepresentation(const QString &representation) {
  if (!QStringList{"c", "low", "med", "high", "llvm"}.contains(
          representation) ||
      representation_ == representation)
    return;
  representation_ = representation;
  textRequest_ = false;
  nextText_ = 0;
  text_.clear();
  textMappings_.clear();
  mappingState_.clear();
  loadText();
  emit changed();
  emit selectionChanged(selection());
}
void Workbench::requestView(const QString &view) {
  centralView_ = view;
  if (!loaded_ || selectedAddress_.isEmpty())
    return;
  if (view == "hex") {
    send(
        "bytes", {{"address", selectedAddress_}, {"size", 1024}},
        [this](const auto &payload, const auto &) {
          const auto data =
              QByteArray::fromHex(payload["data"].toString().toLatin1());
          bool ok;
          const auto start = payload["address"].toString().toULongLong(&ok, 16);
          if (!ok)
            return;
          QStringList lines;
          for (int i = 0; i < data.size(); i += 16) {
            const auto row = data.mid(i, 16);
            QString printable;
            for (const auto c : row)
              printable += c >= 32 && c < 127 ? QChar(c) : QChar('.');
            lines << QString("%1  %2  %3")
                         .arg(start + quint64(i), 16, 16, QChar('0'))
                         .arg(QString::fromLatin1(row.toHex(' '))
                                  .leftJustified(47),
                              printable);
          }
          hexText_ = lines.join('\n');
        },
        true);
  } else if (view == "cfg" && !functionAddress_.isEmpty()) {
    send(
        "cfg_summary", {{"address", functionAddress_}},
        [this](const auto &payload, const auto &) {
          graphSummary_ = payload;
          graphViewport_ = {};
          nodes_.clear();
          edges_.clear();
          const auto bounds = payload["bounds"].toObject();
          requestGraphViewport(bounds["x"].toDouble(), bounds["y"].toDouble(),
                               1000, 700, 1);
        },
        true);
  }
}
QString Workbench::graphViewportStatus() const {
  if (graphViewport_.isEmpty())
    return tr("Loading graph viewport…");
  if (graphViewport_["nodes_truncated"].toBool() ||
      graphViewport_["edges_truncated"].toBool())
    return tr("Viewport limit reached; zoom in for details");
  return tr("%1 visible blocks · %2 edges")
      .arg(nodes_.size())
      .arg(edges_.size());
}
void Workbench::requestGraphViewport(double x, double y, double width,
                                     double height, double scale) {
  const auto layout = graphSummary_["layout_revision"].toString();
  if (layout.isEmpty() || !loaded_ || functionAddress_.isEmpty() ||
      !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(width) ||
      !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
      height <= 0 || scale <= 0)
    return;
  const auto graphGeneration = ++graphGeneration_;
  send(
      "cfg_viewport",
      {{"address", functionAddress_},
       {"layout_revision", layout},
       {"x", x},
       {"y", y},
       {"width", width},
       {"height", height},
       {"scale", scale}},
      [this, layout, graphGeneration](const auto &payload, const auto &) {
        if (graphGeneration != graphGeneration_ ||
            layout != graphSummary_["layout_revision"].toString())
          return;
        graphViewport_ = payload;
        nodes_ = payload["nodes"].toArray().toVariantList();
        edges_ = payload["edges"].toArray().toVariantList();
      },
      true);
}
void Workbench::loadXrefs() {
  const auto address = selectedAddress_;
  send(
      "xrefs", {{"address", address}, {"direction", "to"}, {"limit", 256}},
      [this, address](const auto &payload, const auto &) {
        if (address == selectedAddress_)
          xrefs_.replace(payload["items"].toArray());
      },
      true);
}
void Workbench::analyze() {
  if (!loaded_)
    return;
  busy_ = true;
  status_ = QT_TR_NOOP("Analyzing…");
  send("analyze", {}, [this](const auto &payload, const auto &) {
    metadata_ = payload;
    busy_ = false;
    loadFunctions(false);
    loadText();
    status_ = QT_TR_NOOP("Ready");
  });
  emit changed();
}
void Workbench::cancel() {
  for (auto it = pending_.cbegin(); it != pending_.cend(); ++it)
    if (!it->mutation)
      client_.request("cancel", {{"request_id", it.key()}});
  ++generation_;
  ++textGeneration_;
  instructionRequest_ = textRequest_ = functionRequest_ = false;
  status_ = QT_TR_NOOP(
      "Cancellation requested; restart stops the worker immediately.");
  textStatus_ = QT_TR_NOOP("Cancellation requested");
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
    return;
  }
  if (choice == "save") {
    send(
        "save", {},
        [this](const auto &, const auto &) {
          dirty_ = false;
          transitionReady_ = true;
        },
        false, true);
  } else if (choice == "discard") {
    // An acknowledged edit can be discarded; an edit still executing must
    // finish first.
    transitionReady_ = true;
    finishTransition();
  }
}
void Workbench::finishTransition() {
  if (!transitionReady_ || mutationActive_ || !mutations_.isEmpty())
    return;
  // Restart can interrupt read-only work, but must never interrupt a write.
  const auto action = transition_;
  transition_.clear();
  transitionReady_ = false;
  if (action == "close") {
    emit closeReady();
    return;
  }
  if (action == "restart")
    performRestart();
  else if (action == "open")
    openPending();
  else if (action == "reload")
    send(
        "reload", {},
        [this](const auto &, const auto &) {
          dirty_ = false;
          refreshHistory();
          loadComment();
          loadFunctions(false);
          status_ = QT_TR_NOOP("Annotations reloaded");
        },
        false, true);
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
  resetSessionRequests();
  loaded_ = connected_ = busy_ = false;
  dirty_ = false;
  revision_.clear();
  projectId_.clear();
  clearViews();
  status_ = QT_TR_NOOP("Restarting analysis worker…");
  client_.start(workerPath_);
  emit changed();
  emit selectionChanged(selection());
}
void Workbench::renameFunction(const QString &name) {
  if (functionAddress_.isEmpty() || name.trimmed().isEmpty())
    return;
  const auto address = functionAddress_;
  send(
      "rename", {{"address", address}, {"name", name}},
      [this, name, address](const auto &, const auto &) {
        if (functionAddress_ == address) {
          functionName_ = name;
          textRequest_ = false;
          loadText();
        }
        loadFunctions(false);
        refreshHistory();
        status_ = QT_TR_NOOP("Rename saved");
      },
      false, true);
}
void Workbench::setComment(const QString &comment) {
  if (selectedAddress_.isEmpty())
    return;
  const auto address = selectedAddress_;
  send(
      "annotation_set", {{"address", address}, {"text", comment}},
      [this, comment, address](const auto &, const auto &) {
        dirty_ = true;
        if (selectedAddress_ == address)
          selectedComment_ = comment;
        refreshHistory();
        status_ = QT_TR_NOOP("Comment changed; save annotations to keep it.");
      },
      false, true);
}
void Workbench::saveAnnotations() {
  send(
      "save", {},
      [this](const auto &, const auto &) {
        dirty_ = false;
        refreshHistory();
        status_ = QT_TR_NOOP("Annotations saved");
      },
      false, true);
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
        loadComment();
        loadFunctions(false);
        status_ = QT_TR_NOOP("Annotations reloaded");
      },
      false, true);
}
void Workbench::refreshHistory() {
  if (!loaded_)
    return;
  send("history", {{"limit", 1}},
       [this](const auto &payload, const auto &) { historyState_ = payload; });
}
void Workbench::applyHistory(const QString &operation) {
  send(
      operation, {},
      [this](const auto &payload, const auto &) {
        historyState_ = payload;
        loadFunctions(false);
        loadComment();
        const auto address = payload["address"].toString();
        if (!address.isEmpty())
          navigate(address);
      },
      false, true);
}
void Workbench::undo() {
  if (canUndo())
    applyHistory("undo");
}
void Workbench::redo() {
  if (canRedo())
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
  if (!selectedAddress_.isEmpty())
    payload["address"] = selectedAddress_;
  send("contribution_execute", payload,
       [this](const auto &result, const auto &) {
         contributionResult_ = QString::fromUtf8(
             QJsonDocument(result).toJson(QJsonDocument::Indented));
       });
}
void Workbench::externalQuery(const QString &id, const QString &operation,
                              const QJsonObject &payload,
                              const QString &revision) {
  if (!connected_ || !loaded_ || pending_.size() + external_.size() >= 48) {
    emit externalResponse(
        id, {{"status", "error"},
             {"error",
              QJsonObject{{"code", "unavailable"},
                          {"message", "No active project or queue is full"}}}});
    return;
  }
  external_.insert(client_.request(operation, payload, revision), id);
}
