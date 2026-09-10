#include "PaneRegistry.h"

#include "PaneController.h"
#include "QueryService.h"

#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>
#include <optional>
#include <utility>

namespace {
bool validId(const QString &id) {
  static const QRegularExpression pattern("^[a-zA-Z0-9_-]{1,64}$");
  return pattern.match(id).hasMatch();
}
bool validHash(const QString &hash) {
  static const QRegularExpression pattern("^[a-fA-F0-9]{64}$");
  return pattern.match(hash).hasMatch();
}
bool validAddress(const QString &address) {
  static const QRegularExpression pattern("^0[xX][a-fA-F0-9]{1,16}$");
  return pattern.match(address).hasMatch();
}
QVariantMap saveLocation(const PaneLocation &location) {
  // Names and comments are mutable. Resolve them from the active session.
  return {{"address", location.address},
          {"function_address", location.functionAddress}};
}
PaneLocation readLocation(const QVariantMap &data) {
  return {
      data["address"].toString(), data["function_address"].toString(), {}, {}};
}
bool validLocation(const QVariantMap &data) {
  const auto address = data["address"].toString(),
             function = data["function_address"].toString();
  return (address.isEmpty() || validAddress(address)) &&
         (function.isEmpty() || validAddress(function));
}
} // namespace

struct PaneRegistry::State {
  struct Group {
    QString id, name;
    quint64 incarnation = 0, sequence = 0, committed = 0;
    bool pending = false;
    QPointer<PaneController> origin;
    std::optional<PaneLocation> canonical;
  };
  struct PaneStamp {
    QPointer<PaneController> pane;
    quint64 epoch = 0, navigation = 0, membership = 0;
  };
  PaneRegistry *q;
  QueryService *queries;
  QList<QPointer<PaneController>> panes;
  QList<Group> groups;
  QPointer<PaneController> machine, representation, active;
  QString error, binaryHash;
  quint64 nextIncarnation = 0, epoch = 0, navigationClock = 0, restoreClock = 0;
  int nextOrdinal = 1;
  bool loaded = false;
  QVariantMap pendingRestore;
  QHash<QString, quint64> restoreMemberships;

  State(PaneRegistry *registry, QueryService *service)
      : q(registry), queries(service), epoch(service->sessionEpoch()) {}

  Group *group(const QString &id) {
    for (auto &entry : groups)
      if (entry.id == id)
        return &entry;
    return nullptr;
  }
  void reject(const QString &message) {
    error = message;
    emit q->changed();
  }
  PaneController *add(const QString &id, const QString &kind, int ordinal) {
    auto *pane = new PaneController(id, kind, ordinal, queries, q, q);
    pane->loaded_ = loaded;
    panes.append(pane);
    QObject::connect(pane, &PaneController::changed, q, &PaneRegistry::changed);
    QObject::connect(pane, &PaneController::selectionChanged, q, [this, pane] {
      if (active == pane)
        emit q->selectionChanged();
    });
    QObject::connect(pane, &PaneController::errorOccurred, q,
                     [this, pane](const QString &message) {
                       emit q->paneError(pane->id(), message);
                     });
    return pane;
  }
  bool eligible(const PaneController *pane, const QString &groupId) const {
    return pane && pane->loaded_ && pane->open_ && !pane->removing_ &&
           !pane->pinned_ && pane->groupId_ == groupId;
  }
  PaneStamp operationStamp(PaneController *pane, bool retire = false) const {
    return {pane, queries->sessionEpoch(),
            pane->navigationSequence_ + (retire ? 1 : 0),
            pane->membershipEpoch_};
  }
  bool currentStructure(const PaneStamp &stamp,
                        quint64 membershipAdvance = 0) const {
    return stamp.pane && q->findPane(stamp.pane->id()) == stamp.pane &&
           queries->sessionEpoch() == stamp.epoch &&
           stamp.pane->membershipEpoch_ == stamp.membership + membershipAdvance;
  }
  bool currentOperation(const PaneStamp &stamp,
                        quint64 membershipAdvance = 0) const {
    return currentStructure(stamp, membershipAdvance) &&
           stamp.pane->navigationSequence_ == stamp.navigation;
  }
  void adopt(PaneController *pane) {
    const QPointer<PaneRegistry> registryGuard(q);
    auto *entry = group(pane->groupId_);
    if (!entry || !eligible(pane, entry->id))
      return;
    if (entry->canonical) {
      auto location = *entry->canonical;
      // Restored canonicals intentionally omit mutable metadata. Keep already
      // resolved details for the same identity until the current read returns.
      if (location.functionName.isEmpty() &&
          location.functionAddress == pane->location_.functionAddress) {
        location.functionName = pane->location_.functionName;
        if (location.address == pane->location_.address)
          location.comment = pane->location_.comment;
      }
      const QPointer<PaneController> guard(pane);
      const auto membership = pane->membershipEpoch_;
      const auto navigation = pane->navigationSequence_;
      const auto epoch = queries->sessionEpoch();
      pane->applyLocation(location, PaneNavigation::Function, -1, false);
      if (registryGuard && guard && guard->membershipEpoch_ == membership &&
          guard->navigationSequence_ == navigation &&
          queries->sessionEpoch() == epoch)
        guard->loadComment();
    } else if (!entry->pending && !pane->location_.address.isEmpty()) {
      entry->canonical = pane->location_;
      entry->committed = ++entry->sequence;
      const auto id = entry->id;
      const auto incarnation = entry->incarnation, sequence = entry->sequence;
      const auto location = *entry->canonical;
      const QPointer<PaneController> origin(pane);
      const auto navigation = pane->navigationSequence_;
      const auto membership = pane->membershipEpoch_;
      const auto recipients = panes;
      for (const auto &recipient : recipients) {
        entry = group(id);
        if (!origin || origin->navigationSequence_ != navigation ||
            origin->membershipEpoch_ != membership || !entry ||
            entry->incarnation != incarnation || entry->sequence != sequence)
          break;
        if (recipient != pane && eligible(recipient, id)) {
          recipient->applyLocation(location, PaneNavigation::Function, -1,
                                   false);
          if (!registryGuard)
            return;
        }
      }
    }
  }
  void chooseActive() {
    if (active && active->open_ && !active->removing_)
      return;
    active.clear();
    for (const auto &pane : panes)
      if (pane && pane->open_ && !pane->removing_) {
        active = pane;
        break;
      }
    emit q->selectionChanged();
  }
};

PaneRegistry::PaneRegistry(QueryService *queries, QObject *parent)
    : QObject(parent), state_(std::make_unique<State>(this, queries)) {
  auto &s = *state_;
  s.groups.append({"default", {}, ++s.nextIncarnation});
  s.machine = s.add("machine", "machine", s.nextOrdinal++);
  s.representation = s.add("representation", "representation", s.nextOrdinal++);
  s.machine->groupId_ = s.representation->groupId_ = "default";
  // The existing workspace shows both defaults. Extra docks opt in through
  // real guest visibility when their UI is created.
  s.machine->contentVisible_ = s.representation->contentVisible_ = true;
  s.active = s.machine;
  connect(queries, &QueryService::contextChanged, this, [this] {
    const QPointer<PaneRegistry> guard(this);
    auto &s = *state_;
    if (s.epoch != s.queries->sessionEpoch()) {
      s.epoch = s.queries->sessionEpoch();
      s.loaded = false;
      s.binaryHash.clear();
      for (auto &group : s.groups) {
        group.incarnation = ++s.nextIncarnation;
        group.sequence = group.committed = 0;
        group.pending = false;
        group.origin.clear();
        group.canonical.reset();
      }
      const auto panes = s.panes;
      for (const auto &pane : panes) {
        if (!pane)
          continue;
        retireNavigation(pane);
        if (!guard)
          return;
        if (!pane)
          continue;
        pane->loaded_ = false;
        pane->pinned_ = false;
        pane->location_ = {};
        pane->history_.clear();
        pane->historyIndex_ = -1;
        pane->error_.clear();
        pane->clearResults();
        if (!guard)
          return;
        if (pane)
          emit pane->changed();
        if (!guard)
          return;
      }
    } else {
      // Lazy analysis can publish a revision during this pane's own request.
      // Hide obsolete anchors/layouts without cancelling that valid response.
      for (const auto &pane : QList<QPointer<PaneController>>(s.panes))
        if (pane)
          emit pane->changed();
    }
    emit changed();
    if (guard)
      emit selectionChanged();
  });
}

PaneRegistry::~PaneRegistry() {
  // Pane destructors detach subscriptions while both State and the borrowed
  // service still exist. QObject's later child teardown then has no panes.
  const auto panes = state_->panes;
  for (const auto &pane : panes)
    delete pane.data();
}

PaneController *PaneRegistry::findPane(const QString &id) const {
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && pane->id() == id)
      return pane;
  return nullptr;
}
QObject *PaneRegistry::pane(const QString &id) const { return findPane(id); }
PaneController *PaneRegistry::activePane() const { return state_->active; }
PaneController *PaneRegistry::defaultMachine() const { return state_->machine; }
PaneController *PaneRegistry::defaultRepresentation() const {
  return state_->representation;
}
QObject *PaneRegistry::activePaneObject() const { return activePane(); }
QObject *PaneRegistry::defaultMachineObject() const { return defaultMachine(); }
QObject *PaneRegistry::defaultRepresentationObject() const {
  return defaultRepresentation();
}
QString PaneRegistry::activePaneId() const {
  return state_->active ? state_->active->id() : QString{};
}
QString PaneRegistry::error() const { return state_->error; }
quint64 PaneRegistry::navigationRevision() const {
  return state_->navigationClock;
}

QVariantList PaneRegistry::items() const {
  QVariantList result;
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && !pane->removing_)
      result.append(
          QVariantMap{{"id", pane->id()},
                      {"dockId", pane->dockId()},
                      {"kind", pane->kind()},
                      {"ordinal", pane->ordinal()},
                      {"open", pane->open()},
                      {"groupId", pane->groupId()},
                      {"pinned", pane->pinned()},
                      {"controller", QVariant::fromValue<QObject *>(pane)}});
  return result;
}
QVariantList PaneRegistry::groups() const {
  QVariantList result;
  for (const auto &group : std::as_const(state_->groups))
    result.append(QVariantMap{
        {"id", group.id},
        {"name", group.name.isEmpty() && group.id == "default" ? tr("Default")
                                                               : group.name}});
  return result;
}
QString PaneRegistry::createPane(const QString &kind, const QString &sourceId) {
  auto &s = *state_;
  if ((kind != "machine" && kind != "representation") || s.panes.size() >= 16) {
    s.reject(tr("Cannot create pane: invalid kind or pane limit reached."));
    return {};
  }
  auto *source = sourceId.isEmpty() ? activePane() : findPane(sourceId);
  if ((!sourceId.isEmpty() && !source) || (source && source->removing_)) {
    s.reject(tr("The source pane is unavailable."));
    return {};
  }
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto *created = s.add(id, kind, s.nextOrdinal++);
  if (source) {
    created->location_ = source->location_;
    if (source->kind() == kind) {
      created->representation_ = source->representation_;
      created->centralView_ = source->centralView_;
    }
  }
  s.error.clear();
  emit changed();
  return id;
}
void PaneRegistry::setActivePane(const QString &id) {
  auto *pane = findPane(id);
  if (!pane || !pane->open_ || pane->removing_ || state_->active == pane)
    return;
  state_->active = pane;
  emit changed();
  emit selectionChanged();
}
void PaneRegistry::setPaneOpen(const QString &id, bool open) {
  const QPointer<PaneRegistry> guard(this);
  QPointer<PaneController> pane = findPane(id);
  if (!pane || pane->removing_ || pane->open_ == open)
    return;
  const auto operation = state_->operationStamp(pane, true);
  retireNavigation(pane);
  if (!guard || !state_->currentStructure(operation))
    return;
  ++pane->membershipEpoch_;
  pane->open_ = open;
  if (!open) {
    pane->contentVisible_ = false;
    if (state_->currentOperation(operation, 1))
      pane->clearResults();
  } else if (state_->currentOperation(operation, 1)) {
    state_->adopt(pane);
  }
  if (!guard)
    return;
  state_->chooseActive();
  if (!guard)
    return;
  if (pane)
    emit pane->changed();
  if (guard)
    emit changed();
}
void PaneRegistry::setPaneVisible(const QString &id, bool visible) {
  const QPointer<PaneRegistry> guard(this);
  QPointer<PaneController> pane = findPane(id);
  if (!pane || pane->removing_ || pane->contentVisible_ == visible ||
      (visible && !pane->open_))
    return;
  const auto operation = state_->operationStamp(pane, !visible);
  pane->contentVisible_ = visible;
  if (!visible) {
    retireNavigation(pane);
    if (!guard || !state_->currentOperation(operation))
      return;
    pane->clearResults();
  } else {
    pane->refreshVisible();
    if (!guard || !state_->currentOperation(operation))
      return;
    pane->loadComment();
  }
  if (guard && pane)
    emit pane->changed();
}
void PaneRegistry::setPaneGroup(const QString &id, const QString &groupId) {
  const QPointer<PaneRegistry> guard(this);
  QPointer<PaneController> pane = findPane(id);
  if (!pane || pane->removing_ || pane->groupId_ == groupId ||
      (!groupId.isEmpty() && !state_->group(groupId)))
    return;
  const auto operation = state_->operationStamp(pane, true);
  retireNavigation(pane);
  if (!guard || !state_->currentStructure(operation) ||
      (!groupId.isEmpty() && !state_->group(groupId)))
    return;
  ++pane->membershipEpoch_;
  pane->groupId_ = groupId;
  if (state_->currentOperation(operation, 1))
    state_->adopt(pane);
  if (!guard)
    return;
  if (pane)
    emit pane->changed();
  if (guard && pane == activePane())
    emit selectionChanged();
}
void PaneRegistry::setPanePinned(const QString &id, bool pinned) {
  const QPointer<PaneRegistry> guard(this);
  QPointer<PaneController> pane = findPane(id);
  if (!pane || pane->removing_ || pane->pinned_ == pinned)
    return;
  const auto operation = state_->operationStamp(pane, true);
  retireNavigation(pane);
  if (!guard || !state_->currentStructure(operation))
    return;
  ++pane->membershipEpoch_;
  pane->pinned_ = pinned;
  if (!pinned && state_->currentOperation(operation, 1))
    state_->adopt(pane);
  if (guard && pane)
    emit pane->changed();
}
QString PaneRegistry::createGroup(const QString &name) {
  if (state_->groups.size() >= 16 || name.trimmed().isEmpty() ||
      name.toUtf8().size() > 256) {
    state_->reject(
        tr("Cannot create group: invalid name or group limit reached."));
    return {};
  }
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  state_->groups.append({id, name, ++state_->nextIncarnation});
  emit changed();
  return id;
}
void PaneRegistry::renameGroup(const QString &id, const QString &name) {
  auto *group = state_->group(id);
  if (!group || name.trimmed().isEmpty() || name.toUtf8().size() > 256)
    return;
  group->name = name;
  emit changed();
}
void PaneRegistry::removeGroup(const QString &id) {
  if (id == "default" || !state_->group(id))
    return;
  const auto panes = state_->panes;
  // Invalidate the incarnation before any reentrant membership notification.
  for (qsizetype i = 0; i < state_->groups.size(); ++i)
    if (state_->groups[i].id == id) {
      state_->groups.removeAt(i);
      break;
    }
  for (const auto &pane : panes)
    if (pane && pane->groupId_ == id)
      setPaneGroup(pane->id(), {});
  emit changed();
}
bool PaneRegistry::beginRemovePane(const QString &id) {
  const QPointer<PaneRegistry> guard(this);
  QPointer<PaneController> pane = findPane(id);
  if (!pane || pane->removing_ || pane == defaultMachine() ||
      pane == defaultRepresentation())
    return false;
  const auto operation = state_->operationStamp(pane, true);
  pane->removing_ = true;
  pane->open_ = pane->contentVisible_ = false;
  retireNavigation(pane);
  if (!guard || !state_->currentStructure(operation))
    return true;
  ++pane->membershipEpoch_;
  if (state_->currentOperation(operation, 1))
    pane->clearResults();
  if (!guard)
    return true;
  state_->chooseActive();
  if (!guard)
    return true;
  if (pane)
    emit pane->changed();
  if (guard)
    emit changed();
  return true;
}
void PaneRegistry::finishRemovePane(const QString &id) {
  auto *pane = findPane(id);
  if (!pane || !pane->removing_)
    return;
  state_->panes.removeAll(pane);
  delete pane;
  emit changed();
}

PaneNavigationTicket PaneRegistry::beginNavigation(PaneController *pane,
                                                   PaneNavigation kind,
                                                   int historyTarget) {
  if (!pane || findPane(pane->id()) != pane || !pane->loaded_ || !pane->open_ ||
      pane->removing_)
    return {};
  const QPointer<PaneRegistry> guard(this);
  const auto operation = state_->operationStamp(pane, true);
  const auto groupId = pane->pinned_ ? QString{} : pane->groupId_;
  const auto *originalGroup = state_->group(groupId);
  const auto incarnation = originalGroup ? originalGroup->incarnation : 0;
  const auto sequence = originalGroup ? originalGroup->sequence : 0;
  const auto current = [guard, operation, groupId, incarnation, sequence] {
    if (!guard || !guard->state_->currentOperation(operation))
      return false;
    if (groupId.isEmpty())
      return true;
    const auto *group = guard->state_->group(groupId);
    return group && group->incarnation == incarnation &&
           group->sequence == sequence;
  };
  retireNavigation(pane);
  if (!current())
    return {};
  if (auto *group = state_->group(groupId)) {
    const auto previous = group->origin;
    if (group->pending && previous && previous != pane) {
      retireNavigation(previous);
      if (!current())
        return {};
    }
  }
  // Install the replacement only after both retirement notification boundaries
  // have preserved its original stamp. A nested Cancel cannot become its base.
  ++state_->navigationClock;
  pane->navigationPending_ = true;
  PaneNavigationTicket ticket{pane,
                              operation.epoch,
                              operation.navigation,
                              operation.membership,
                              {},
                              0,
                              0,
                              kind,
                              historyTarget};
  if (!groupId.isEmpty()) {
    if (auto *group = state_->group(groupId)) {
      ticket.groupId = group->id;
      ticket.groupIncarnation = group->incarnation;
      ticket.groupSequence = ++group->sequence;
      group->origin = pane;
      group->pending = true;
    }
  }
  return ticket;
}
bool PaneRegistry::isCurrent(const PaneNavigationTicket &ticket) const {
  auto *pane = ticket.origin.data();
  if (!pane || !pane->navigationPending_ || findPane(pane->id()) != pane ||
      !pane->loaded_ || !pane->open_ || pane->removing_ ||
      ticket.sessionEpoch != state_->queries->sessionEpoch() ||
      ticket.paneSequence != pane->navigationSequence_ ||
      ticket.membershipEpoch != pane->membershipEpoch_)
    return false;
  if (ticket.groupId.isEmpty())
    return pane->pinned_ || pane->groupId_.isEmpty();
  const auto *group = state_->group(ticket.groupId);
  return group && !pane->pinned_ && pane->groupId_ == ticket.groupId &&
         group->pending && group->origin == pane &&
         group->incarnation == ticket.groupIncarnation &&
         group->sequence == ticket.groupSequence;
}
bool PaneRegistry::commitNavigation(const PaneNavigationTicket &ticket,
                                    const PaneLocation &location) {
  if (!isCurrent(ticket) || !validAddress(location.address) ||
      (!location.functionAddress.isEmpty() &&
       !validAddress(location.functionAddress)))
    return false;
  const auto origin = ticket.origin;
  const QPointer<PaneRegistry> guard(this);
  origin->navigationPending_ = false;
  if (ticket.groupId.isEmpty()) {
    origin->applyLocation(location, ticket.kind, ticket.historyTarget);
    return true;
  }
  auto *group = state_->group(ticket.groupId);
  group->pending = false;
  group->origin.clear();
  group->canonical = location;
  group->committed = ticket.groupSequence;
  const auto recipients = state_->panes;
  for (const auto &pane : recipients) {
    group = state_->group(ticket.groupId);
    if (!origin || origin->navigationSequence_ != ticket.paneSequence ||
        origin->membershipEpoch_ != ticket.membershipEpoch ||
        ticket.sessionEpoch != state_->queries->sessionEpoch() || !group ||
        group->incarnation != ticket.groupIncarnation ||
        group->sequence != ticket.groupSequence)
      break;
    if (state_->eligible(pane, ticket.groupId)) {
      pane->applyLocation(location, ticket.kind,
                          pane == origin ? ticket.historyTarget : -1);
      if (!guard)
        return true;
    }
  }
  emit changed();
  return true;
}
void PaneRegistry::finishNavigation(const PaneNavigationTicket &ticket) {
  if (!isCurrent(ticket))
    return;
  ticket.origin->navigationPending_ = false;
  if (auto *group = state_->group(ticket.groupId)) {
    group->pending = false;
    group->origin.clear();
  }
  emit ticket.origin->changed();
}
void PaneRegistry::retireNavigation(PaneController *pane) {
  if (!pane)
    return;
  if (auto *group = state_->group(pane->groupId_))
    if (group->pending && group->origin == pane) {
      group->pending = false;
      group->origin.clear();
    }
  pane->navigationPending_ = false;
  ++pane->navigationSequence_;
  const QPointer<PaneController> guard(pane);
  pane->cancelChannel(PaneController::Navigation);
  // Notify after the caller has installed its replacement ticket/lifecycle.
  // A synchronous changed handler here could supersede a half-built intent.
  if (guard)
    QTimer::singleShot(0, guard.data(), [guard] { emit guard->changed(); });
}

void PaneRegistry::setLoaded(bool loaded) {
  const QPointer<PaneRegistry> guard(this);
  state_->loaded = loaded;
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes)) {
    if (!pane)
      continue;
    pane->loaded_ = loaded;
    if (!loaded) {
      retireNavigation(pane);
      if (!guard)
        return;
      if (!pane)
        continue;
      pane->clearResults();
      if (!guard)
        return;
    }
    if (pane)
      emit pane->changed();
    if (!guard)
      return;
  }
  emit changed();
}
void PaneRegistry::cancelReads() {
  const QPointer<PaneRegistry> guard(this);
  // Cancel is also intent against a deferred startup entry fallback. Advance
  // before any unsubscribe/changed observer can enter the same stack again.
  ++state_->navigationClock;
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane) {
      retireNavigation(pane);
      if (!guard)
        return;
      if (!pane)
        continue;
      pane->cancelRequests();
      if (!guard)
        return;
      if (!pane)
        continue;
      pane->textStatus_ =
          QT_TRANSLATE_NOOP("Workbench", "Cancellation requested");
      emit pane->changed();
      if (!guard)
        return;
    }
}
void PaneRegistry::refreshAnnotations() {
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && pane->canFetch()) {
      pane->loadComment();
      if (pane->kind_ == "machine" && pane->centralView_ == "cfg")
        pane->requestView("cfg");
    }
}
void PaneRegistry::renamed(const QString &address, const QString &name) {
  for (auto &group : state_->groups)
    if (group.canonical && group.canonical->functionAddress == address)
      group.canonical->functionName = name;
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && pane->location_.functionAddress == address) {
      pane->location_.functionName = name;
      if (pane->kind_ == "representation")
        pane->reloadRepresentation();
      emit pane->changed();
    }
  emit selectionChanged();
}

void PaneRegistry::commented(const QString &address, const QString &comment) {
  for (auto &group : state_->groups)
    if (group.canonical && group.canonical->address == address)
      group.canonical->comment = comment;
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && pane->location_.address == address) {
      pane->location_.comment = comment;
      emit pane->changed();
    }
}

QVariantMap PaneRegistry::serializeMetadata() const {
  QVariantList paneData, groupData;
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && !pane->removing_)
      paneData.append(QVariantMap{{"id", pane->id()},
                                  {"kind", pane->kind()},
                                  {"ordinal", pane->ordinal()},
                                  {"group_id", pane->groupId_},
                                  {"open", pane->open_},
                                  {"pinned", pane->pinned_},
                                  {"view", pane->centralView_},
                                  {"representation", pane->representation_},
                                  {"location", saveLocation(pane->location_)}});
  for (const auto &group : std::as_const(state_->groups))
    groupData.append(QVariantMap{
        {"id", group.id},
        {"name", group.name},
        {"location",
         group.canonical ? saveLocation(*group.canonical) : QVariantMap{}}});
  return {{"version", 1},
          {"binary_sha256", state_->binaryHash},
          {"active_pane", activePaneId()},
          {"panes", paneData},
          {"groups", groupData}};
}

bool PaneRegistry::restoreMetadata(const QVariantMap &metadata) {
  const QPointer<PaneRegistry> guard(this);
  const auto panes = metadata["panes"].toList(),
             groups = metadata["groups"].toList();
  const auto hash = metadata["binary_sha256"].toString();
  const auto invalid = [this] {
    state_->reject(tr("The saved pane catalog is invalid."));
    return false;
  };
  if (metadata["version"].toInt() != 1 || panes.size() < 2 ||
      panes.size() > 16 || groups.isEmpty() || groups.size() > 16 ||
      (!hash.isEmpty() && !validHash(hash)) ||
      QJsonDocument::fromVariant(metadata)
              .toJson(QJsonDocument::Compact)
              .size() > 128 * 1024)
    return invalid();
  QSet<QString> paneIds, groupIds;
  QSet<int> ordinals;
  for (const auto &value : groups) {
    const auto group = value.toMap();
    const auto id = group["id"].toString(), name = group["name"].toString();
    if (!validId(id) || groupIds.contains(id) || name.toUtf8().size() > 256 ||
        (id != "default" && name.trimmed().isEmpty()) ||
        !validLocation(group["location"].toMap()))
      return invalid();
    groupIds.insert(id);
  }
  if (!groupIds.contains("default"))
    return invalid();
  for (const auto &value : panes) {
    const auto pane = value.toMap();
    const auto id = pane["id"].toString(), kind = pane["kind"].toString();
    const auto groupId = pane["group_id"].toString();
    const int ordinal = pane["ordinal"].toInt();
    if (!validId(id) || paneIds.contains(id) ||
        (kind != "machine" && kind != "representation") ||
        ((id == "machine" || id == "representation") && kind != id) ||
        ordinal < 1 || ordinal > 1000000 || ordinals.contains(ordinal) ||
        (!groupId.isEmpty() && !groupIds.contains(groupId)) ||
        !QStringList{"disasm", "cfg", "hex"}.contains(
            pane["view"].toString()) ||
        !QStringList{"c", "low", "med", "high", "llvm"}.contains(
            pane["representation"].toString()) ||
        !validLocation(pane["location"].toMap()))
      return invalid();
    if (auto *existing = findPane(id))
      if (existing->removing_ || existing->kind() != kind ||
          existing->ordinal() != ordinal)
        return invalid();
    paneIds.insert(id);
    ordinals.insert(ordinal);
  }
  if (!paneIds.contains("machine") || !paneIds.contains("representation") ||
      (!metadata["active_pane"].toString().isEmpty() &&
       !paneIds.contains(metadata["active_pane"].toString())))
    return invalid();
  // Removing a live dock requires the explicit two-phase API. A catalog may
  // create/reuse controllers, but cannot silently delete an existing guest.
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane && !paneIds.contains(pane->id()))
      return invalid();

  // This restore deliberately retires earlier work once. Any further Cancel
  // or navigation during that retirement invalidates this original operation.
  const auto operationEpoch = state_->queries->sessionEpoch();
  const auto operationClock = state_->navigationClock + 1;
  const auto current = [guard, operationEpoch, operationClock] {
    return guard && guard->state_->queries->sessionEpoch() == operationEpoch &&
           guard->state_->navigationClock == operationClock;
  };
  cancelReads();
  if (!current())
    return false;
  state_->groups.clear();
  for (const auto &value : groups) {
    const auto group = value.toMap();
    state_->groups.append({group["id"].toString(), group["name"].toString(),
                           ++state_->nextIncarnation});
  }
  for (const auto &value : panes) {
    const auto data = value.toMap();
    auto *pane = findPane(data["id"].toString());
    if (!pane)
      pane = state_->add(data["id"].toString(), data["kind"].toString(),
                         data["ordinal"].toInt());
    state_->nextOrdinal = qMax(state_->nextOrdinal, pane->ordinal() + 1);
    ++pane->membershipEpoch_;
    pane->groupId_ = data["group_id"].toString();
    pane->open_ = data["open"].toBool();
    pane->contentVisible_ = false;
    pane->pinned_ = false;
    pane->centralView_ = data["view"].toString();
    pane->representation_ = data["representation"].toString();
    pane->location_ = {};
    pane->history_.clear();
    pane->historyIndex_ = -1;
  }
  // Finish the whole local catalog before clearing a model can notify Cancel.
  // Every pane must already reference a surviving group and a valid active
  // pane must be selected even when the first clear aborts further work.
  state_->active = findPane(metadata["active_pane"].toString());
  if (!state_->active || !state_->active->open_ || state_->active->removing_) {
    state_->active.clear();
    for (const auto &pane : state_->panes)
      if (pane && pane->open_ && !pane->removing_) {
        state_->active = pane;
        break;
      }
  }
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes)) {
    if (!pane || !pane->clearResults() || !current())
      return false;
  }
  state_->pendingRestore = metadata;
  state_->restoreClock = operationClock;
  state_->restoreMemberships.clear();
  for (const auto &pane : QList<QPointer<PaneController>>(state_->panes))
    if (pane)
      state_->restoreMemberships.insert(pane->id(), pane->membershipEpoch_);
  state_->error.clear();
  if (!state_->binaryHash.isEmpty())
    setBinaryIdentity(state_->binaryHash);
  if (!current())
    return false;
  emit changed();
  if (current())
    emit selectionChanged();
  return current();
}

void PaneRegistry::setBinaryIdentity(const QString &hash) {
  const QPointer<PaneRegistry> guard(this);
  if (!validHash(hash))
    return;
  state_->binaryHash = hash.toLower();
  if (state_->pendingRestore.isEmpty())
    return;
  const auto metadata = std::exchange(state_->pendingRestore, {});
  if (metadata["binary_sha256"].toString().toLower() != state_->binaryHash ||
      state_->restoreClock != state_->navigationClock)
    return;
  const auto restoreEpoch = state_->queries->sessionEpoch();
  const auto restoreClock = state_->restoreClock;
  const auto current = [guard, restoreEpoch, restoreClock] {
    return guard && guard->state_->queries->sessionEpoch() == restoreEpoch &&
           guard->state_->restoreClock == restoreClock &&
           guard->state_->navigationClock == restoreClock;
  };
  for (const auto &value : metadata["groups"].toList()) {
    const auto data = value.toMap();
    if (auto *group = state_->group(data["id"].toString())) {
      const auto location = readLocation(data["location"].toMap());
      if (!location.address.isEmpty()) {
        group->canonical = location;
        group->committed = ++group->sequence;
      }
    }
  }
  for (const auto &value : metadata["panes"].toList()) {
    if (!current())
      break;
    const auto data = value.toMap();
    if (QPointer<PaneController> pane = findPane(data["id"].toString())) {
      const auto membership = pane->membershipEpoch_;
      const auto epoch = state_->queries->sessionEpoch();
      if (state_->restoreMemberships.value(pane->id(), membership + 1) !=
          membership)
        continue;
      pane->pinned_ = data["pinned"].toBool();
      pane->applyLocation(readLocation(data["location"].toMap()),
                          PaneNavigation::Function, -1, false);
      if (!current())
        return;
      if (!pane || pane->membershipEpoch_ != membership ||
          state_->queries->sessionEpoch() != epoch)
        continue;
      state_->adopt(pane);
      if (!current())
        return;
      if (pane && pane->membershipEpoch_ == membership &&
          state_->queries->sessionEpoch() == epoch)
        pane->loadComment();
      if (!current())
        return;
    }
  }
  emit changed();
  if (current())
    emit selectionChanged();
}
