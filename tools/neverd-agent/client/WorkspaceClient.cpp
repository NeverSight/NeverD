//===- WorkspaceClient.cpp - QML frontend to the Agent CLI ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Apply CLI state updates before notifying QML bindings.

#include "WorkspaceClient.h"

#include "platform/AppPaths.h"

#include <QDeadlineTimer>
#include <QDesktopServices>
#include <QKeySequence>
#include <QUrl>
#include <utility>

namespace neverd::agent {
namespace {
constexpr int kRequestTimeoutMs = 30000;

void merge(QJsonObject &state, const QJsonObject &changes) {
  for (auto it = changes.constBegin(); it != changes.constEnd(); ++it)
    state.insert(it.key(), it.value());
}

bool updateMessages(QJsonObject &state, const QJsonObject &change) {
  auto messages = state.value("messages").toArray();
  if (!wire::replaceTail(messages, change))
    return false;
  state["messages"] = messages;
  return true;
}
} // namespace

SideChatClient::SideChatClient(WorkspaceClient &workspace, const QString &id)
    : QObject(&workspace), Workspace(workspace), Id(id) {}

void SideChatClient::setDraft(const QString &text) {
  if (text != draft())
    Workspace.request("side.setDraft", wire::arguments(text), Id);
}
void SideChatClient::cancel() { Workspace.request("side.cancel", {}, Id); }
void SideChatClient::resumeQueue() {
  Workspace.request("side.resumeQueue", {}, Id);
}
void SideChatClient::discardQueuedInput(const QString &id) {
  Workspace.request("side.discardQueuedInput", wire::arguments(id), Id);
}

bool SideChatClient::update(const QJsonObject &message) {
  const auto fields = message.value("values").toObject();
  merge(State, fields);
#define AGENT_SIDE_PROPERTY(Type, Name, Signal)                                \
  if (fields.contains(#Name))                                                  \
    Changed.insert(#Signal);
#include "WorkspaceProtocol.def"
#undef AGENT_SIDE_PROPERTY
  if (message.contains("tail")) {
    if (!updateMessages(State, message.value("tail").toObject()))
      return false;
    Changed.insert("messagesChanged");
  }
  return true;
}

void SideChatClient::notify() {
  const auto changed = std::exchange(Changed, {});
  if (changed.contains("draftChanged"))
    emit draftChanged();
  if (changed.contains("messagesChanged"))
    emit messagesChanged();
  if (changed.contains("titleChanged"))
    emit titleChanged();
  if (changed.contains("runChanged"))
    emit runChanged();
}

WorkspaceClient::WorkspaceClient(const QString &directory, QObject *parent)
    : QObject(parent), Directory(directory) {
  NotifyTimer.setSingleShot(true);
  connect(&NotifyTimer, &QTimer::timeout, this, &WorkspaceClient::notify);
  connect(&Process, &AgentProcess::received, this, &WorkspaceClient::receive);
  connect(&Process, &AgentProcess::failed, this, &WorkspaceClient::fail);
}

bool WorkspaceClient::initialize(const QString &project, QString &error) {
  if (!Process.start(Directory, error))
    return false;
  auto request = wire::initialize("workspace");
  request["project"] = project;
  request["interactiveObjectApprovals"] = true;
  if (!Process.send(request))
    fail(tr("Could not initialize NeverD Agent CLI."));
  if (!waitFor(Ready)) {
    error = TransportError;
    Process.close();
    return false;
  }
  Initialized = true;
  return true;
}

bool WorkspaceClient::prepareClose() {
  return !Ready || call<bool>("prepareClose", {});
}

QString WorkspaceClient::pathFromUrl(const QString &url) const {
  return localPath(url);
}

QString WorkspaceClient::shortcutText(const QString &sequence) const {
  return QKeySequence::fromString(sequence, QKeySequence::PortableText)
      .toString(QKeySequence::NativeText);
}

#define AGENT_ACTION(Return, Name, Signature, Arguments)                       \
  Return WorkspaceClient::Name Signature {                                     \
    return call<Return>(#Name, wire::arguments Arguments);                     \
  }
#include "WorkspaceProtocol.def"
#undef AGENT_ACTION

bool WorkspaceClient::waitFor(const bool &completed) {
  QDeadlineTimer deadline(kRequestTimeoutMs);
  while (!completed && TransportError.isEmpty()) {
    if (deadline.hasExpired() ||
        !Process.waitForOutput(static_cast<int>(deadline.remainingTime()))) {
      if (!completed && TransportError.isEmpty())
        fail(tr("NeverD Agent CLI did not answer the request."));
      break;
    }
  }
  return completed && TransportError.isEmpty();
}

QJsonValue WorkspaceClient::request(const QString &name, const QJsonArray &args,
                                    const QString &sideId) {
  if (!Ready || (name == "setDraft" && args.at(0) == State.value("draft")))
    return {};
  // Read the reply and update the cache now. QML notifications are deferred
  // until the calling action returns, so handlers can issue their own requests.
  ReplyReceived = false;
  auto message = wire::invoke(++RequestId, name, args);
  if (!sideId.isEmpty())
    message["sideChat"] = sideId;
  if (!Process.send(message)) {
    fail(tr("Could not send a request to NeverD Agent CLI."));
    return {};
  }
  return waitFor(ReplyReceived) ? Reply : QJsonValue{};
}

void WorkspaceClient::fail(const QString &error) {
  if (!TransportError.isEmpty())
    return;
  Ready = false;
  TransportError = error;
  if (Initialized) {
    Events.append({{"type", "operation_error"}, {"text", error}});
    NotifyTimer.start();
  }
}

void WorkspaceClient::receive(const QJsonObject &message) {
  const auto type = message.value("type").toString();
  if (type == "ready") {
    if (message.value("protocol").toInt() == wire::kVersion)
      Ready = true;
    else
      fail(tr("Incompatible NeverD Agent CLI protocol."));
    return;
  }
  if (type == "error" && message.value("fatal").toBool()) {
    fail(message.value("text").toString());
    return;
  }
  if (type == "response") {
    if (message.value("id").toInteger() != RequestId || ReplyReceived)
      return;
    Reply = message.value("result");
    ReplyReceived = true;
    if (message.contains("error"))
      Events.append(
          {{"type", "operation_error"}, {"text", message.value("error")}});
  } else if (type == "workspace_state") {
    merge(State, message.value("values").toObject());
    for (const auto &signal : message.value("changed").toArray())
      Changed.insert(signal.toString().toLatin1());
  } else if (type == "workspace_messages") {
    if (!updateMessages(State, message))
      fail(tr("Invalid workspace transcript update from NeverD Agent CLI."));
  } else if (type == "side_chat") {
    const auto id = message.value("id").toString();
    if ((Side ? Side->id() : QString{}) != id) {
      if (Side)
        Side->deleteLater();
      Side = id.isEmpty() ? nullptr : new SideChatClient(*this, id);
      Changed.insert("sideChatChanged");
    }
    if (Side && !Side->update(message))
      fail(tr("Invalid side chat transcript update from NeverD Agent CLI."));
  } else {
    Events.append(message);
  }
  NotifyTimer.start();
}

void WorkspaceClient::notify() {
  const auto changed = std::exchange(Changed, {});
  const auto events = std::exchange(Events, {});
#define AGENT_STATE_SIGNAL(Name)                                               \
  if (changed.contains(#Name))                                                 \
    emit Name();
#include "WorkspaceProtocol.def"
#undef AGENT_STATE_SIGNAL
  if (Side)
    Side->notify();
  for (const auto &event : events) {
    const auto type = event.value("type").toString();
    const auto text = event.value("text").toString();
    if (type == "operation_error")
      emit operationError(text);
    else if (type == "notice")
      emit notice(text);
    else if (type == "model_settings_saved")
      emit modelSettingsSaved();
    else if (type == "evidence_opened")
      emit evidenceOpened(event.value("id").toString(),
                          event.value("title").toString());
    else if (type == "side_chat_opened")
      emit sideChatOpened();
    else if (type == "desktop_url" &&
             !QDesktopServices::openUrl(QUrl(event.value("url").toString())))
      emit operationError(text);
  }
}
} // namespace neverd::agent
