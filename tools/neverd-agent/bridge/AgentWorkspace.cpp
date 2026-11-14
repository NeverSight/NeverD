//===- AgentWorkspace.cpp - CLI project workspace -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dispatch the workspace actions and publish changed state to the frontend.

#include "AgentWorkspace.h"

#include "client/AgentProtocol.h"

#include <QCoreApplication>
#include <QDir>
#include <QVariant>
#include <limits>
#include <tuple>
#include <type_traits>
#include <utility>

namespace neverd::agent {
namespace {
struct Text {
  Q_DECLARE_TR_FUNCTIONS(neverd::agent::AgentWorkspace)
};

template <typename T>
std::optional<std::remove_cvref_t<T>> decodeArgument(const QJsonValue &value) {
  using Type = std::remove_cvref_t<T>;
  if constexpr (std::is_same_v<Type, QString>) {
    if (value.isString())
      return value.toString();
  } else if constexpr (std::is_same_v<Type, QVariantMap>) {
    if (value.isObject())
      return value.toObject().toVariantMap();
  } else if constexpr (std::is_same_v<Type, bool>) {
    if (value.isBool())
      return value.toBool();
  } else if constexpr (std::is_integral_v<Type>) {
    const auto integer = value.toInteger();
    if (value.isDouble() && QJsonValue(integer) == value &&
        integer >= std::numeric_limits<Type>::min() &&
        integer <= std::numeric_limits<Type>::max())
      return static_cast<Type>(integer);
  } else {
    static_assert(!sizeof(T), "Unsupported workspace argument");
  }
  return std::nullopt;
}

template <typename Owner, typename Return, typename... Args>
std::optional<QJsonValue> invokeAction(Owner &owner,
                                       Return (Owner::*method)(Args...),
                                       const QJsonArray &args) {
  if (args.size() != sizeof...(Args))
    return std::nullopt;
  return [&]<size_t... Index>(
             std::index_sequence<Index...>) -> std::optional<QJsonValue> {
    const auto decoded = std::tuple{decodeArgument<Args>(args.at(Index))...};
    if (!(std::get<Index>(decoded).has_value() && ...))
      return std::nullopt;
    if constexpr (std::is_void_v<Return>) {
      (owner.*method)(*std::get<Index>(decoded)...);
      return QJsonValue{};
    } else {
      return QJsonValue::fromVariant(
          QVariant::fromValue((owner.*method)(*std::get<Index>(decoded)...)));
    }
  }(std::index_sequence_for<Args...>{});
}

void capture(QJsonObject &previous, QJsonObject &patch, QJsonObject &tail,
             const QString &name, const QVariant &value) {
  const auto current = QJsonValue::fromVariant(value);
  const auto prior = previous.value(name);
  if (prior == current)
    return;
  if (name == "messages")
    tail = wire::changedTail(prior.toArray(), current.toArray());
  else
    patch[name] = current;
  previous[name] = current;
}
} // namespace

bool initializeWorkspace(AgentController &controller, QLockFile &lock,
                         const QString &project, QString &error) {
  const auto directory = controller.dataDirectory();
  if (!QDir().mkpath(directory)) {
    error = Text::tr("Cannot create the application data directory: %1")
                .arg(directory);
    return false;
  }
  if (!lock.tryLock()) {
    error = Text::tr("Agent is already running with this data directory, or "
                     "the directory cannot be written: %1")
                .arg(directory);
    return false;
  }
  if (!controller.initialize(error))
    return false;
  if (project.isEmpty())
    return true;
  const auto connection = QObject::connect(
      &controller, &AgentController::operationError, &controller,
      [&error](const QString &message) { error = message; },
      Qt::SingleShotConnection);
  const bool opened = controller.openProject(project);
  QObject::disconnect(connection);
  if (!opened && error.isEmpty())
    error = Text::tr("Cannot open the Agent project: %1").arg(project);
  return opened;
}

AgentWorkspace::AgentWorkspace(const QString &directory,
                               std::function<void(const QJsonObject &)> publish)
    : Lock(QDir(directory).filePath("agent.lock")), Controller(directory, this),
      Publish(std::move(publish)) {
  Flush.setSingleShot(true);
  connect(&Flush, &QTimer::timeout, this, &AgentWorkspace::publishPending);
#define AGENT_STATE_SIGNAL(Name)                                               \
  connect(&Controller, &AgentController::Name, this,                           \
          [this] { markChanged(#Name); });
#include "client/WorkspaceProtocol.def"
#undef AGENT_STATE_SIGNAL
  connect(&Controller, &AgentController::operationError, this,
          [this](const QString &text) {
            postEvent({{"type", "operation_error"}, {"text", text}});
          });
  connect(&Controller, &AgentController::notice, this,
          [this](const auto &text) {
            postEvent({{"type", "notice"}, {"text", text}});
          });
  connect(&Controller, &AgentController::modelSettingsSaved, this,
          [this] { postEvent({{"type", "model_settings_saved"}}); });
  connect(&Controller, &AgentController::evidenceOpened, this,
          [this](const auto &id, const auto &title) {
            postEvent(
                {{"type", "evidence_opened"}, {"id", id}, {"title", title}});
          });
  connect(&Controller, &AgentController::sideChatOpened, this,
          [this] { postEvent({{"type", "side_chat_opened"}}); });
  connect(&Controller, &AgentController::desktopUrlRequested, this,
          [this](const auto &url, const auto &error) {
            postEvent({{"type", "desktop_url"}, {"url", url}, {"text", error}});
          });
}

bool AgentWorkspace::initialize(const QString &project,
                                bool interactiveObjectApprovals,
                                QString &error) {
  if (interactiveObjectApprovals)
    Controller.enableInteractiveObjectApprovals();
  if (!initializeWorkspace(Controller, Lock, project, error)) {
    Flush.stop();
    return false;
  }
#define AGENT_STATE_SIGNAL(Name) Dirty.insert(#Name);
#include "client/WorkspaceProtocol.def"
#undef AGENT_STATE_SIGNAL
  publishPending();
  Publish({{"type", "ready"}, {"protocol", wire::kVersion}});
  return true;
}

ShutdownStatus AgentWorkspace::prepareShutdown() {
  return Controller.prepareShutdown();
}

void AgentWorkspace::markChanged(const QByteArray &signal) {
  Dirty.insert(signal);
  Flush.start();
}

void AgentWorkspace::postEvent(const QJsonObject &event) {
  Events.append(event);
  Flush.start();
}

void AgentWorkspace::publishSideChat() {
  auto *current = Controller.sideChat();
  if (Side != current) {
    for (const auto &connection : SideConnections)
      disconnect(connection);
    SideConnections.clear();
    PublishedSide = {};
    Side = current;
    if (Side) {
      const auto dirty = [this] {
        SideDirty = true;
        Flush.start();
      };
      SideConnections = {connect(Side, &SideChat::draftChanged, this, dirty),
                         connect(Side, &SideChat::messagesChanged, this, dirty),
                         connect(Side, &SideChat::runChanged, this, dirty),
                         connect(Side, &SideChat::titleChanged, this, dirty)};
    }
    SideDirty = true;
  }
  if (!std::exchange(SideDirty, false))
    return;
  QJsonObject patch, tail;
  if (Side) {
    if (PublishedSide.isEmpty()) {
      patch["forkedAt"] = Side->forkedAt();
      patch["inheritedMessages"] =
          QJsonArray::fromVariantList(Side->inheritedMessages());
    }
#define AGENT_SIDE_PROPERTY(Type, Name, Signal)                                \
  capture(PublishedSide, patch, tail, #Name, Side->Name());
#include "client/WorkspaceProtocol.def"
#undef AGENT_SIDE_PROPERTY
  }
  QJsonObject message{{"type", "side_chat"},
                      {"id", Side ? Side->id() : QString{}},
                      {"values", patch}};
  if (!tail.isEmpty())
    message["tail"] = tail;
  Publish(message);
}

void AgentWorkspace::publishPending() {
  Flush.stop();
  const auto dirty = std::exchange(Dirty, {});
  QJsonObject patch, tail;
#define AGENT_PROPERTY(Type, Name, Signal)                                     \
  if (dirty.contains(#Signal))                                                 \
    capture(Published, patch, tail, #Name, Controller.Name());
#include "client/WorkspaceProtocol.def"
#undef AGENT_PROPERTY
  if (!tail.isEmpty()) {
    tail["type"] = "workspace_messages";
    Publish(tail);
  }
  publishSideChat();
  if (!dirty.isEmpty()) {
    QJsonArray changed;
    for (const auto &signal : dirty)
      changed.append(QString::fromLatin1(signal));
    Publish(
        {{"type", "workspace_state"}, {"values", patch}, {"changed", changed}});
  }
  const auto events = std::exchange(Events, {});
  for (const auto &event : events)
    Publish(event);
}

std::optional<QJsonValue> AgentWorkspace::invoke(const QString &method,
                                                 const QJsonArray &args,
                                                 const QString &sideId) {
  if (method.startsWith("side.")) {
    auto *side = Controller.sideChat();
    if (!side || side->id() != sideId)
      return std::nullopt;
    if (method == "side.setDraft")
      return invokeAction(*side, &SideChat::setDraft, args);
    if (method == "side.cancel")
      return invokeAction(*side, &SideChat::cancel, args);
    if (method == "side.resumeQueue")
      return invokeAction(*side, &SideChat::resumeQueue, args);
    if (method == "side.discardQueuedInput")
      return invokeAction(*side, &SideChat::discardQueuedInput, args);
    return std::nullopt;
  }
  if (method == "prepareClose")
    return invokeAction(Controller, &AgentController::prepareClose, args);
#define AGENT_ACTION(Return, Name, Signature, Arguments)                       \
  if (method == #Name)                                                         \
    return invokeAction(Controller, &AgentController::Name, args);
#include "client/WorkspaceProtocol.def"
#undef AGENT_ACTION
  return std::nullopt;
}

std::optional<QJsonValue> AgentWorkspace::dispatch(const QString &method,
                                                   const QJsonArray &args,
                                                   const QString &sideId) {
  const auto result = invoke(method, args, sideId);
  if (method == "setDraft" && result)
    markChanged("conversationChanged");
  publishPending();
  return result;
}
} // namespace neverd::agent
