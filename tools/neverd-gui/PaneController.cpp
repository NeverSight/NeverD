#include "PaneController.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
QString translated(const char *text) {
  return QCoreApplication::translate("Workbench", text);
}

bool validAddress(const QString &address) {
  if (!address.startsWith("0x", Qt::CaseInsensitive) || address.size() < 3 ||
      address.size() > 18)
    return false;
  for (const auto c : QStringView(address).mid(2))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return false;
  return true;
}
bool sameAddress(const QString &left, const QString &right) {
  return validAddress(left) && validAddress(right) &&
         left.mid(2).toULongLong(nullptr, 16) ==
             right.mid(2).toULongLong(nullptr, 16);
}
} // namespace

PaneController::PaneController(QString id, QString kind, int ordinal,
                               QueryService *queries, PaneRegistry *registry,
                               QObject *parent)
    : QObject(parent), queries_(queries), registry_(registry),
      id_(std::move(id)), kind_(std::move(kind)), ordinal_(ordinal) {
  selectionTimer_.setSingleShot(true);
  selectionTimer_.setInterval(40);
  connect(&selectionTimer_, &QTimer::timeout, this,
          &PaneController::loadSelectionDetail);
}

PaneController::~PaneController() {
  resetSelectionDetails();
  queries_->unsubscribeOwner(this);
}

QString PaneController::dockId() const {
  return id_ == "machine" || id_ == "representation" ? "neverd." + id_
                                                     : "neverd.pane." + id_;
}

bool PaneController::busy() const {
  return navigationPending_ || detailActive_ || queuedDetails_ ||
         std::any_of(pending_.begin(), pending_.end(),
                     [](auto id) { return id != 0; });
}

QJsonObject PaneController::selection() const {
  return {{"project_id", queries_->projectId()},
          {"revision", queries_->revision()},
          {"address", location_.address},
          {"function_address", location_.functionAddress},
          {"function_name", location_.functionName},
          {"representation", representation_},
          {"loaded", loaded_ && queries_->available()},
          {"pane_id", id_},
          {"group_id", groupId_},
          {"view", kind_ == "machine" ? centralView_ : representation_}};
}

void PaneController::cancelChannel(Channel channel) {
  ++generations_[channel];
  const auto id = std::exchange(pending_[channel], 0);
  if (id)
    queries_->unsubscribe(id);
}

void PaneController::cancelRequests() {
  resetSelectionDetails();
  const QPointer<PaneController> guard(this);
  for (int channel = 0; channel < ChannelCount; ++channel) {
    cancelChannel(Channel(channel));
    if (!guard)
      return;
  }
}

bool PaneController::request(Channel channel, QueryService::QuerySpec spec,
                             Completion complete, bool graph) {
  const QPointer<PaneController> guard(this);
  const auto generation = generations_[channel] + 1;
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  const auto current = [guard, channel, generation, navigation, membership,
                        epoch] {
    return guard && guard->generations_[channel] == generation &&
           guard->navigationSequence_ == navigation &&
           guard->membershipEpoch_ == membership &&
           guard->queries_->sessionEpoch() == epoch;
  };
  cancelChannel(channel);
  if (!current())
    return false;
  // Admission and cache completions are deferred, including zero-ID errors.
  const auto id = std::make_shared<QueryService::SubscriptionId>(0);
  auto callback = [guard, channel, generation, epoch, id,
                   complete =
                       std::move(complete)](const QJsonObject &response) {
    if (!guard || guard->queries_->sessionEpoch() != epoch ||
        guard->generations_[channel] != generation ||
        guard->pending_[channel] != *id)
      return;
    guard->pending_[channel] = 0;
    complete(response);
    if (guard)
      emit guard->changed();
  };
  *id = graph ? queries_->graphViewport(std::move(spec.payload), this,
                                        std::move(callback))
              : queries_->subscribe(std::move(spec), this, std::move(callback));
  if (!guard)
    return false;
  if (!current()) {
    // Admission may synchronously notify Cancel or a newer request before its
    // ID is returned. Retire this subscription only; preserve any newer slot.
    if (*id)
      queries_->unsubscribe(*id);
    return false;
  }
  pending_[channel] = *id;
  return true;
}

bool PaneController::clearResults() {
  resetSelectionDetails();
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  const auto current = [guard, navigation, membership, epoch] {
    return guard && guard->navigationSequence_ == navigation &&
           guard->membershipEpoch_ == membership &&
           guard->queries_->sessionEpoch() == epoch;
  };
  // Navigation intent is retired separately by the registry. Applying an
  // already validated group location must not create or consume another one.
  for (int channel = Instructions; channel < ChannelCount; ++channel) {
    cancelChannel(Channel(channel));
    if (!current())
      return false;
  }
  instructions_.replace({});
  if (!current())
    return false;
  xrefs_.replace({});
  if (!current())
    return false;
  nextInstruction_.clear();
  text_.clear();
  textStatus_.clear();
  nextText_ = textStartLine_ = 0;
  textMappings_.clear();
  mappingRevision_.clear();
  mappingState_.clear();
  hexText_.clear();
  graphSummary_ = {};
  graphViewport_ = {};
  graphRevision_.clear();
  nodes_.clear();
  edges_.clear();
  return true;
}

void PaneController::navigate(const QString &query) { navigateTo(query, -1); }

void PaneController::navigateTo(const QString &query, int historyTarget) {
  const auto input = query.trimmed();
  if (!loaded_ || !open_ || removing_ || input.isEmpty() ||
      input.size() > 4096 ||
      (input.startsWith("0x", Qt::CaseInsensitive) && !validAddress(input)))
    return;
  const auto ticket =
      registry_->beginNavigation(this, PaneNavigation::Function, historyTarget);
  if (!ticket.origin)
    return;
  request(Navigation, {"resolve", {{"query", input}}},
          [this, ticket](const auto &response) {
            if (!registry_->isCurrent(ticket))
              return;
            if (response["status"] != "ok") {
              registry_->finishNavigation(ticket);
              fail(response);
              return;
            }
            const auto payload = response["payload"].toObject();
            PaneLocation location{payload["address"].toString(),
                                  payload["function_address"].toString(),
                                  payload["name"].toString(),
                                  payload["comment"].toString(),
                                  payload["comment"].isString()};
            if (!validAddress(location.address) ||
                (!location.functionAddress.isEmpty() &&
                 !validAddress(location.functionAddress))) {
              registry_->finishNavigation(ticket);
              fail({{"status", "error"},
                    {"error",
                     QJsonObject{{"message", "Invalid navigation result"}}}});
              return;
            }
            registry_->commitNavigation(ticket, location);
          });
  emit changed();
}

void PaneController::selectInstruction(const QString &address) {
  if (!loaded_ || !open_ || removing_ || !validAddress(address))
    return;
  // Even choosing the current instruction supersedes an older pending intent.
  const auto ticket =
      registry_->beginNavigation(this, PaneNavigation::Instruction);
  auto location = location_;
  location.address = address;
  if (address != location_.address) {
    location.comment.clear();
    location.commentKnown = false;
  }
  registry_->commitNavigation(ticket, location);
}

void PaneController::applyLocation(const PaneLocation &location,
                                   PaneNavigation kind, int historyTarget,
                                   bool recordHistory) {
  // clearResults can synchronously emit pending/model signals. Capture before
  // entering it so a Cancel during clearing cannot become the new baseline.
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  const auto current = [guard, navigation, membership, epoch] {
    return guard && guard->navigationSequence_ == navigation &&
           guard->membershipEpoch_ == membership &&
           guard->queries_->sessionEpoch() == epoch;
  };
  const bool documentChanged =
      location.functionAddress != location_.functionAddress ||
      (kind == PaneNavigation::Function &&
       location.address != location_.address);
  const bool addressChanged = location.address != location_.address;
  ++selectionGeneration_;
  location_ = location;
  error_.clear();
  if (kind == PaneNavigation::Function) {
    if (historyTarget >= 0 && historyTarget < history_.size() &&
        history_[historyTarget] == location.address) {
      historyIndex_ = historyTarget;
    } else if (recordHistory &&
               (historyIndex_ < 0 ||
                history_.value(historyIndex_) != location.address)) {
      while (history_.size() > historyIndex_ + 1)
        history_.removeLast();
      history_.append(location.address);
      if (history_.size() > 256)
        history_.removeFirst();
      historyIndex_ = int(history_.size()) - 1;
    }
  }
  if (documentChanged) {
    if (!clearResults() || !current())
      return;
    nextInstruction_ = location_.address;
  } else if (addressChanged) {
    // Keep an admitted detail read until its terminal callback. Replacing its
    // subscriber on every cursor move would send cancellation/request storms.
    // Its context guard prevents publication after this immediate selection.
    xrefs_.replace({});
    if (!current())
      return;
  }
  emit selectionChanged();
  if (!current())
    return;
  if (canFetch()) {
    if (documentChanged)
      refreshVisible();
    else
      loadXrefs();
    if (!current())
      return;
    // Missing metadata is unknown, including after a function navigation.
    // One ordinary detail read can fill it without treating absence as empty.
    if (kind == PaneNavigation::Instruction || !location_.commentKnown)
      loadComment();
  }
  if (current())
    emit changed();
}

void PaneController::goBack() {
  if (canGoBack())
    navigateTo(history_[historyIndex_ - 1], historyIndex_ - 1);
}
void PaneController::goForward() {
  if (canGoForward())
    navigateTo(history_[historyIndex_ + 1], historyIndex_ + 1);
}

void PaneController::loadInstructions(bool append) {
  if (!canFetch() || kind_ != "machine" || pending_[Instructions] ||
      (append && nextInstruction_.isEmpty()))
    return;
  const auto address = append ? nextInstruction_ : location_.address;
  if (address.isEmpty())
    return;
  request(Instructions, {"disasm", {{"address", address}, {"limit", 256}}},
          [this, append](const auto &response) {
            if (response["status"] != "ok") {
              fail(response);
              return;
            }
            const auto payload = response["payload"].toObject();
            if (append)
              instructions_.append(payload["items"].toArray());
            else
              instructions_.replace(payload["items"].toArray());
            nextInstruction_ = payload["next_address"].toString();
          });
}
void PaneController::loadMoreInstructions() { loadInstructions(true); }

void PaneController::loadText(bool append) {
  if (!canFetch() || kind_ != "representation")
    return;
  if (location_.functionAddress.isEmpty()) {
    textStatus_ = QT_TRANSLATE_NOOP("Workbench", "No function at this address");
    return;
  }
  if (pending_[Text] || (append && nextText_ < 0))
    return;
  textStatus_ = QT_TRANSLATE_NOOP("Workbench", "Analyzing…");
  request(Text,
          {"decompile",
           {{"address", location_.functionAddress},
            {"representation", representation_},
            {"offset", append ? nextText_ : 0},
            {"limit", 512}}},
          [this, append](const auto &response) {
            if (response["status"] != "ok") {
              textStatus_ = response["error"].toObject()["message"].toString(
                  response["status"].toString());
              fail(response);
              return;
            }
            const auto payload = response["payload"].toObject();
            const auto page = payload["text"].toString();
            if (append && text_.size() + page.size() <= 2 * 1024 * 1024 &&
                mappingRevision_ == response["revision"].toString()) {
              text_ += page;
            } else {
              text_ = page.left(2 * 1024 * 1024);
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
            textStatus_ =
                nextText_ < 0
                    ? QT_TRANSLATE_NOOP("Workbench", "Complete")
                    : QT_TRANSLATE_NOOP("Workbench", "More lines available");
          });
}

QVariantList PaneController::textMappings() const {
  return mappingRevision_ == queries_->revision() ? textMappings_
                                                  : QVariantList{};
}
QString PaneController::mappingStatus() const {
  if (text_.isEmpty())
    return {};
  if (mappingRevision_ != queries_->revision())
    return translated(QT_TRANSLATE_NOOP(
        "Workbench",
        "Mapping belongs to an earlier revision; reload the representation"));
  if (mappingState_ == "instruction_anchors")
    return translated(QT_TRANSLATE_NOOP(
        "Workbench",
        "Linked instruction addresses; synthetic rows may be unmapped"));
  return translated(QT_TRANSLATE_NOOP(
      "Workbench", "Instruction mapping unavailable for this representation"));
}
QString PaneController::representationStatus() const {
  return translated(textStatus_.toUtf8().constData());
}
void PaneController::selectTextLine(int line) {
  for (const auto &value : textMappings()) {
    const auto row = value.toMap();
    if (row["line"].toInt() != line)
      continue;
    const auto addresses = row["addresses"].toList();
    if (!addresses.isEmpty())
      selectInstruction(addresses.first().toString());
    return;
  }
}
void PaneController::loadMoreText() { loadText(true); }
void PaneController::reloadRepresentation() {
  registry_->resumeAnalysisReads(this);
  if (location_.functionAddress.isEmpty() && queries_->analysisComplete()) {
    registry_->repairAnalysisLocations(this);
    return;
  }
  reloadRepresentationImpl();
}
void PaneController::reloadRepresentationImpl() {
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  const auto generation = generations_[Text] + 1;
  cancelChannel(Text);
  if (!guard || navigationSequence_ != navigation ||
      membershipEpoch_ != membership || queries_->sessionEpoch() != epoch ||
      generations_[Text] != generation)
    return;
  nextText_ = 0;
  loadText();
  if (guard)
    emit changed();
}
void PaneController::setRepresentation(const QString &representation) {
  if (kind_ != "representation" || representation_ == representation ||
      !QStringList{"c", "low", "med", "high", "llvm"}.contains(representation))
    return;
  registry_->resumeAnalysisReads(this);
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  const auto generation = generations_[Text] + 1;
  representation_ = representation;
  text_.clear();
  textMappings_.clear();
  mappingState_.clear();
  mappingRevision_.clear();
  nextText_ = 0;
  cancelChannel(Text);
  if (!guard || navigationSequence_ != navigation ||
      membershipEpoch_ != membership || queries_->sessionEpoch() != epoch ||
      generations_[Text] != generation)
    return;
  if (location_.functionAddress.isEmpty() && queries_->analysisComplete())
    registry_->repairAnalysisLocations(this);
  else
    loadText();
  if (guard)
    emit changed();
  if (guard)
    emit selectionChanged();
}
void PaneController::toggleRepresentationPin() {
  if (loaded_ && !location_.functionAddress.isEmpty())
    registry_->setPanePinned(id_, !pinned_);
}

void PaneController::requestView(const QString &view) {
  if (kind_ != "machine" || !QStringList{"disasm", "hex", "cfg"}.contains(view))
    return;
  registry_->resumeAnalysisReads(this);
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  requestViewImpl(view);
  if (guard && navigationSequence_ == navigation &&
      membershipEpoch_ == membership && queries_->sessionEpoch() == epoch &&
      centralView_ == view && location_.functionAddress.isEmpty() &&
      queries_->analysisComplete())
    registry_->repairAnalysisLocations(this);
}
void PaneController::requestViewImpl(const QString &view) {
  if (kind_ != "machine" || !QStringList{"disasm", "hex", "cfg"}.contains(view))
    return;
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  const auto current = [guard, navigation, membership, epoch, view] {
    return guard && guard->navigationSequence_ == navigation &&
           guard->membershipEpoch_ == membership &&
           guard->queries_->sessionEpoch() == epoch &&
           guard->centralView_ == view;
  };
  const bool switched = centralView_ != view;
  centralView_ = view;
  if (switched) {
    cancelChannel(Instructions);
    if (!current())
      return;
    cancelChannel(Hex);
    if (!current())
      return;
    cancelChannel(Graph);
    if (!current())
      return;
  }
  if (!canFetch() || location_.address.isEmpty())
    return;
  if (view == "disasm") {
    if (instructions_.count() == 0)
      loadInstructions(false);
  } else if (view == "hex") {
    request(Hex, {"bytes", {{"address", location_.address}, {"size", 1024}}},
            [this](const auto &response) {
              if (response["status"] != "ok") {
                fail(response);
                return;
              }
              const auto payload = response["payload"].toObject();
              const auto data =
                  QByteArray::fromHex(payload["data"].toString().toLatin1());
              bool ok = false;
              const auto start =
                  payload["address"].toString().toULongLong(&ok, 16);
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
            });
  } else if (!location_.functionAddress.isEmpty()) {
    const auto bounds = graphSummary_["bounds"].toObject();
    requestGraphViewport(bounds["x"].toDouble(), bounds["y"].toDouble(), 1000,
                         700, 1);
  }
  if (current())
    emit changed();
}

void PaneController::requestGraphViewport(double x, double y, double width,
                                          double height, double scale) {
  if (!canFetch() || kind_ != "machine" || centralView_ != "cfg" ||
      location_.functionAddress.isEmpty() || !std::isfinite(x) ||
      !std::isfinite(y) || !std::isfinite(width) || !std::isfinite(height) ||
      !std::isfinite(scale) || width <= 0 || height <= 0 || scale <= 0)
    return;
  request(
      Graph,
      {"cfg_viewport",
       {{"address", location_.functionAddress},
        {"x", x},
        {"y", y},
        {"width", width},
        {"height", height},
        {"scale", scale}}},
      [this](const auto &response) {
        if (response["status"] != "ok") {
          fail(response);
          return;
        }
        const auto payload = response["payload"].toObject();
        graphSummary_ = payload["summary"].toObject();
        graphViewport_ = payload["viewport"].toObject();
        graphRevision_ = response["revision"].toString();
        nodes_ = graphViewport_["nodes"].toArray().toVariantList();
        edges_ = graphViewport_["edges"].toArray().toVariantList();
      },
      true);
}
QVariantMap PaneController::graphSummary() const {
  return graphRevision_ == queries_->revision() ? graphSummary_.toVariantMap()
                                                : QVariantMap{};
}
QVariantList PaneController::graphNodes() const {
  return graphRevision_ == queries_->revision() ? nodes_ : QVariantList{};
}
QVariantList PaneController::graphEdges() const {
  return graphRevision_ == queries_->revision() ? edges_ : QVariantList{};
}
QString PaneController::graphViewportStatus() const {
  if (graphViewport_.isEmpty() || graphRevision_ != queries_->revision())
    return translated(
        QT_TRANSLATE_NOOP("Workbench", "Loading graph viewport…"));
  if (graphViewport_["nodes_truncated"].toBool() ||
      graphViewport_["edges_truncated"].toBool())
    return translated(QT_TRANSLATE_NOOP(
        "Workbench", "Viewport limit reached; zoom in for details"));
  return translated(
             QT_TRANSLATE_NOOP("Workbench", "%1 visible blocks · %2 edges"))
      .arg(nodes_.size())
      .arg(edges_.size());
}

bool PaneController::currentDetails(const DetailContext &context) const {
  return canFetch() && queries_->available() && !analysisRefreshSuppressed_ &&
         !navigationPending_ && context.epoch == queries_->sessionEpoch() &&
         context.navigation == navigationSequence_ &&
         context.membership == membershipEpoch_ &&
         context.selection == selectionGeneration_ &&
         !context.address.isEmpty() && context.address == location_.address;
}

void PaneController::queueDetail(Channel channel) {
  const DetailContext context{queries_->sessionEpoch(), navigationSequence_,
                              membershipEpoch_, selectionGeneration_,
                              location_.address};
  if (!currentDetails(context))
    return;
  if (!currentDetails(queuedDetailContext_))
    queuedDetails_ = 0;
  queuedDetailContext_ = context;
  queuedDetails_ |= 1u << channel;
  selectionTimer_.start();
}

void PaneController::resetSelectionDetails() {
  selectionTimer_.stop();
  queuedDetails_ = 0;
  detailActive_ = false;
  ++selectionGeneration_;
  ++detailRequestGeneration_;
}

void PaneController::finishSelectionDetail(quint64 generation) {
  if (generation != detailRequestGeneration_)
    return;
  detailActive_ = false;
  if (queuedDetails_ && !selectionTimer_.isActive()) {
    // Return through the dispatcher's delivery barrier before admitting the
    // next read; queued edits can run before the remaining derived detail.
    QTimer::singleShot(0, this, [this, generation] {
      if (generation == detailRequestGeneration_)
        loadSelectionDetail();
    });
  }
}

void PaneController::loadSelectionDetail() {
  if (!queuedDetails_ || detailActive_ || selectionTimer_.isActive())
    return;
  const auto context = queuedDetailContext_;
  if (!currentDetails(context)) {
    queuedDetails_ = 0;
    emit changed();
    return;
  }
  // One admitted detail subscription per pane, plus the latest location's
  // pending flags. Cursor movement replaces flags without detaching that read.
  const Channel channel =
      queuedDetails_ & (1u << References) ? References : Comment;
  queuedDetails_ &= ~(1u << channel);
  detailActive_ = true;
  const auto generation = detailRequestGeneration_;
  const QPointer<PaneController> guard(this);
  QueryService::QuerySpec spec;
  if (channel == References) {
    spec = {
        "xrefs",
        {{"address", context.address}, {"direction", "to"}, {"limit", 256}}};
  } else {
    spec = {"resolve", {{"query", context.address}}};
  }
  const bool admitted = request(
      channel, std::move(spec),
      [guard, context, channel, generation](const QJsonObject &response) {
        if (!guard || generation != guard->detailRequestGeneration_)
          return;
        if (guard->currentDetails(context)) {
          if (response["status"] != "ok") {
            guard->fail(response);
          } else if (channel == References) {
            guard->xrefs_.replace(
                response["payload"].toObject()["items"].toArray());
          } else {
            const auto payload = response["payload"].toObject();
            if (sameAddress(payload["address"].toString(), context.address)) {
              if (payload["comment"].isString()) {
                guard->location_.comment = payload["comment"].toString();
                guard->location_.commentKnown = true;
              }
              if (guard->location_.functionAddress ==
                  payload["function_address"].toString())
                guard->location_.functionName = payload["name"].toString();
            }
          }
        }
        if (guard)
          guard->finishSelectionDetail(generation);
      });
  // A reentrant navigation/Cancel during admission can retire its callback.
  // Release only this attempt; cancellation may already have started a new one.
  if (guard && !admitted)
    guard->finishSelectionDetail(generation);
}

void PaneController::loadXrefs() { queueDetail(References); }
void PaneController::loadComment() { queueDetail(Comment); }
void PaneController::refreshVisible() {
  if (!canFetch() || location_.address.isEmpty())
    return;
  const QPointer<PaneController> guard(this);
  const auto navigation = navigationSequence_, membership = membershipEpoch_;
  const auto epoch = queries_->sessionEpoch();
  if (kind_ == "machine")
    requestViewImpl(centralView_);
  else
    loadText();
  if (guard && navigationSequence_ == navigation &&
      membershipEpoch_ == membership && queries_->sessionEpoch() == epoch)
    loadXrefs();
}
void PaneController::fail(const QJsonObject &response) {
  if (response["status"] == "cancelled")
    return;
  error_ = response["error"].toObject()["message"].toString(
      response["status"].toString());
  emit errorOccurred(error_);
}
void PaneController::copyText(const QString &text) {
  QGuiApplication::clipboard()->setText(text);
}
