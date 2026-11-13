//===- SideChat.cpp - Independent project side conversation --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reuse the same analysis runtime and compaction with a separate session ID.
//===----------------------------------------------------------------------===//
#include "SideChat.h"

#include "Config.h"
#include "project/EvidenceStore.h"

#include <QDateTime>
#include <QDir>
#include <QUuid>

namespace neverd::agent {
SideChat::SideChat(Project project, QString id, QObject *parent)
    : QObject(parent), ProjectValue(std::move(project)), Id(std::move(id)),
      Title(tr("新聊天")), Runtime(this) {
  StreamTimer.setSingleShot(true);
  StreamTimer.setInterval(model_limits::kStreamUiUpdateMs);
  connect(&StreamTimer, &QTimer::timeout, this, &SideChat::messagesChanged);
  SaveTimer.setSingleShot(true);
  SaveTimer.setInterval(kDraftSaveDelayMs);
  connect(&SaveTimer, &QTimer::timeout, this, [this] {
    QString error;
    if (!flush(error)) {
      Runtime.cancel();
      emit operationError(error);
    }
  });
  connect(&Runtime, &ResearchRuntime::changed, this, &SideChat::runChanged);
  connect(&Runtime, &ResearchRuntime::memorySource, this,
          &SideChat::memorySource);
  connect(&Runtime, &ResearchRuntime::artifactsChanged, this,
          &SideChat::artifactsChanged);
  connect(&Runtime, &ResearchRuntime::textDelta, this,
          [this](const QString &text) { updateAnswer(text, "running", true); });
  connect(&Runtime, &ResearchRuntime::steeringApplied, this,
          [this](const QString &id) {
            auto messages = Conversation["messages"].toArray();
            for (qsizetype i = 0; i < messages.size(); ++i) {
              auto message = messages[i].toObject();
              if (message["id"] == id) {
                message["status"] = "saved";
                messages[i] = message;
              }
            }
            Conversation["messages"] = messages;
            Dirty = true;
            SaveTimer.start();
            emit messagesChanged();
          });
  connect(&Runtime, &ResearchRuntime::finished, this,
          [this](QString text, const QString &error, bool cancelled) {
            auto messages = Conversation["messages"].toArray();
            for (qsizetype i = 0; i < messages.size(); ++i) {
              auto message = messages[i].toObject();
              if (message["status"] == "pending") {
                message["status"] = "interrupted";
                messages[i] = message;
              }
            }
            Conversation["messages"] = messages;
            if (cancelled || !error.isEmpty())
              Conversation["queuePaused"] = true;
            if (!error.isEmpty()) {
              if (text.isEmpty())
                text = answer(MessageId);
              text += "\n\n" + error;
            }
            updateAnswer(text,
                         cancelled         ? "cancelled"
                         : error.isEmpty() ? "succeeded"
                                           : "failed",
                         false);
            QString storageError;
            if (!flush(storageError))
              emit operationError(storageError);
            emit messagesChanged();
            emit runChanged();
            emit finished();
            emit draftChanged();
            if (error.isEmpty() && !cancelled && !queuePaused())
              QTimer::singleShot(0, this, &SideChat::nextInputRequested);
          });
}
bool SideChat::load(QString &error) {
  if (!ProjectStore::readConversation(ProjectValue, Conversation, error, Id))
    return false;
  auto messages = Conversation["messages"].toArray();
  bool titleFound = false;
  for (qsizetype i = 0; i < messages.size(); ++i) {
    auto message = messages[i].toObject();
    if (!titleFound && message["role"] == "user" &&
        !message["inherited"].toBool()) {
      Title = message["text"].toString().simplified().left(
          kSideChatTitleCharacters);
      titleFound = true;
    }
    if (message["status"] == "running" || message["status"] == "pending") {
      message["status"] = "interrupted";
      messages[i] = message;
      Dirty = true;
    }
  }
  Conversation["messages"] = messages;
  if (!Conversation["pendingInputs"].toArray().isEmpty()) {
    Conversation["queuePaused"] = true;
    Dirty = true;
  }
  return flush(error);
}
bool SideChat::inherit(const QJsonObject &conversation,
                       const QJsonObject &context, QString &error) {
  QJsonArray messages;
  for (const auto &entry : conversation["messages"].toArray()) {
    auto message = entry.toObject();
    if (message["text"].toString().isEmpty())
      continue;
    message["inherited"] = true;
    if (message["status"] == "running")
      message["status"] = "snapshot";
    messages.append(message);
  }
  Conversation["messages"] = messages;
  Conversation["forkedAt"] =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  Dirty = true;
  if (!context.isEmpty() &&
      !ProjectStore::writeJson(
          QDir(ProjectStore::sessionDirectory(ProjectValue, Id))
              .filePath("router/context.json"),
          context, error))
    return false;
  return flush(error);
}
QVariantList SideChat::messages() const {
  QVariantList result;
  for (const auto &entry : Conversation["messages"].toArray())
    if (!entry.toObject()["inherited"].toBool())
      result.append(entry.toVariant());
  return result;
}
QVariantList SideChat::inheritedMessages() const {
  QVariantList result;
  for (const auto &entry : Conversation["messages"].toArray())
    if (entry.toObject()["inherited"].toBool())
      result.append(entry.toVariant());
  return result;
}
bool SideChat::flush(QString &error) {
  SaveTimer.stop();
  if (!Dirty)
    return true;
  if (!ProjectStore::saveConversation(ProjectValue, Conversation, error, Id))
    return false;
  Dirty = false;
  return true;
}
void SideChat::setDraft(const QString &text) {
  if (draft() == text)
    return;
  Conversation["draft"] = text;
  Dirty = true;
  SaveTimer.start();
  emit draftChanged();
}
bool SideChat::setContext(const QJsonArray &context, QString &error) {
  const auto previous = Conversation;
  Conversation["context"] = context;
  Dirty = true;
  if (!flush(error)) {
    Conversation = previous;
    return false;
  }
  emit draftChanged();
  return true;
}
QVariantMap SideChat::contextUsage() const {
  if (!Runtime.runId().isEmpty())
    return Runtime.contextUsage();
  QJsonObject saved;
  QString error;
  if (ProjectStore::readJson(
          QDir(ProjectStore::sessionDirectory(ProjectValue, Id))
              .filePath("router/context.json"),
          saved, error, true))
    return saved["usage"].toObject().toVariantMap();
  return {};
}
QString SideChat::answer(const QString &messageId) const {
  for (const auto &entry : Conversation["messages"].toArray()) {
    const auto message = entry.toObject();
    if (message["id"] == messageId && message["role"] == "assistant")
      return message["text"].toString();
  }
  return {};
}
bool SideChat::send(const ModelProfile &profile, QString key, QString mcp,
                    QJsonArray sharedMemory, QString &error,
                    const QString &mode) {
  if (draft().trimmed().isEmpty() || (mode != "queue" && mode != "steer"))
    return false;
  const auto input = ConversationInput::capture(
      draft(), {}, Conversation["context"].toArray());
  const auto previous = Conversation;
  Conversation["draft"] = "";
  Conversation["context"] = QJsonArray{};
  if (mode == "steer" && running()) {
    auto messages = Conversation["messages"].toArray();
    auto message = input.message("pending");
    message["inputKind"] = "steer";
    messages.append(message);
    Conversation["messages"] = messages;
  } else {
    auto pending = Conversation["pendingInputs"].toArray();
    pending.append(input.toJson());
    Conversation["pendingInputs"] = pending;
    Conversation["queuePaused"] = false;
  }
  Dirty = true;
  if (!flush(error)) {
    Conversation = previous;
    return false;
  }
  emit draftChanged();
  emit messagesChanged();
  if (running())
    return mode != "steer" ||
           Runtime.steer(input.Id, input.prompt(), error, input.Text);
  return startNext(profile, std::move(key), std::move(mcp),
                   std::move(sharedMemory), error);
}
bool SideChat::startNext(const ModelProfile &profile, QString key, QString mcp,
                         QJsonArray sharedMemory, QString &error) {
  auto pending = Conversation["pendingInputs"].toArray();
  if (running() || queuePaused() || pending.isEmpty())
    return false;
  const auto input =
      ConversationInput::fromJson(pending.first().toObject(), error);
  if (!input ||
      !EvidenceStore::validateBindings(ProjectValue, input->Context, error))
    return false;
  const auto prompt = input->prompt();
  const auto previous = Conversation;
  const auto timestamp =
      QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
  MessageId = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto messages = Conversation["messages"].toArray();
  messages.append(input->message());
  messages.append(QJsonObject{{"id", MessageId},
                              {"role", "assistant"},
                              {"text", ""},
                              {"status", "running"},
                              {"objectId", ""},
                              {"createdAt", timestamp}});
  Conversation["messages"] = messages;
  pending.removeFirst();
  Conversation["pendingInputs"] = pending;
  Dirty = true;
  if (!flush(error)) {
    Conversation = previous;
    return false;
  }
  if (this->messages().size() == 2) {
    Title = input->Text.simplified().left(kSideChatTitleCharacters);
    emit titleChanged();
  }
  emit draftChanged();
  emit messagesChanged();
  ResearchRunOptions options;
  options.UserPrompt = input->Text;
  options.UseMemory = UseMemory;
  options.GenerateMemory = GenerateMemory;
  options.WorkerPath = WorkerPath;
  options.ImportObject = ImportObject;
  options.SessionId = Id;
  if (!Runtime.start(ProjectValue, profile, std::move(key), std::move(mcp),
                     prompt, {}, std::move(sharedMemory), error, options)) {
    updateAnswer(error, "failed", false);
    QString storageError;
    if (!flush(storageError))
      error += "\n" + storageError;
    return false;
  }
  messages = Conversation["messages"].toArray();
  auto reply = messages.last().toObject();
  reply["runId"] = Runtime.runId();
  messages[messages.size() - 1] = reply;
  Conversation["messages"] = messages;
  Dirty = true;
  if (!flush(error)) {
    Runtime.cancel();
    return false;
  }
  emit runChanged();
  return true;
}
void SideChat::cancel() {
  Conversation["queuePaused"] = true;
  Dirty = true;
  QString error;
  if (!flush(error))
    emit operationError(error);
  emit draftChanged();
  Runtime.cancel();
}
void SideChat::resumeQueue() {
  if (running())
    return;
  Conversation["queuePaused"] = false;
  Dirty = true;
  QString error;
  if (!flush(error)) {
    emit operationError(error);
    return;
  }
  emit draftChanged();
  emit nextInputRequested();
}
void SideChat::discardQueuedInput(const QString &id) {
  auto pending = Conversation["pendingInputs"].toArray();
  for (qsizetype i = 0; i < pending.size(); ++i)
    if (pending[i].toObject()["id"] == id) {
      pending.removeAt(i);
      Conversation["pendingInputs"] = pending;
      Dirty = true;
      QString error;
      if (!flush(error))
        emit operationError(error);
      emit draftChanged();
      return;
    }
}
void SideChat::updateAnswer(const QString &text, const QString &status,
                            bool append) {
  auto messages = Conversation["messages"].toArray();
  for (qsizetype i = 0; i < messages.size(); ++i) {
    auto message = messages[i].toObject();
    if (message["id"] != MessageId)
      continue;
    message["text"] = append ? message["text"].toString() + text : text;
    message["status"] = status;
    messages[i] = message;
    break;
  }
  Conversation["messages"] = messages;
  Dirty = true;
  if (!StreamTimer.isActive())
    StreamTimer.start();
  if (!SaveTimer.isActive())
    SaveTimer.start(model_limits::kRunAutosaveMs);
}
} // namespace neverd::agent
