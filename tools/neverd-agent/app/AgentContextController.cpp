//===- AgentContextController.cpp - Explicit captured context references --===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "AgentController.h"

#include <QDir>

namespace neverd::agent {
bool AgentController::appendContext(const QJsonObject &reference, bool side) {
  QString error;
  if (!EvidenceStore::validateReference(reference, error)) {
    emit operationError(error);
    return false;
  }
  auto *chat = side ? sideChat() : nullptr;
  if (side && !chat)
    return false;
  auto context = side ? QJsonArray::fromVariantList(chat->draftContext())
                      : Conversation["context"].toArray();
  context.append(reference);
  if (side) {
    if (!chat->setContext(context, error)) {
      emit operationError(error);
      return false;
    }
  } else {
    auto value = Conversation;
    value["context"] = context;
    if (!commitConversation(ActiveId, value))
      return false;
  }
  return true;
}
void AgentController::attachObjectReference(const QString &objectId,
                                            const QString &query, bool side) {
  const auto *project = findProject(ActiveId);
  const auto *object = project ? findObject(*project, objectId) : nullptr;
  if (!object)
    return;
  appendContext({{"kind", query.trimmed().isEmpty() ? "object" : "address"},
                 {"title", object->Name + (query.trimmed().isEmpty()
                                               ? ""
                                               : " · " + query.trimmed())},
                 {"objectId", object->Id},
                 {"sha256", object->Sha256},
                 {"address", query.trimmed()},
                 {"text", ""}},
                side);
}
void AgentController::attachSelection(const QString &text, const QString &title,
                                      bool side) {
  if (!findProject(ActiveId) || text.trimmed().isEmpty())
    return;
  appendContext({{"kind", "selection"},
                 {"title", title.isEmpty() ? tr("Selected text") : title},
                 {"text", text}},
                side);
}
void AgentController::attachEvidence(const QString &id, bool side) {
  const auto *project = findProject(ActiveId);
  if (!project)
    return;
  QJsonObject evidence;
  QString error;
  if (!EvidenceStore::read(*project, id, evidence, error)) {
    emit operationError(error);
    return;
  }
  auto reference = EvidenceStore::reference(evidence);
  // An evidence attachment carries its exact identity; the model reads the
  // saved page only when needed, rather than injecting a multi-megabyte result.
  reference["text"] =
      "Read this captured evidence with read_evidence when needed.";
  if (appendContext(reference, side))
    emit notice(tr("Evidence reference added. Sending keeps the identity of "
                   "this snapshot."));
}
void AgentController::attachPreview(bool side) {
  const auto *project = findProject(ActiveId);
  if (!project || PreviewPath.isEmpty())
    return;
  appendContext(
      {{"kind", "file"},
       {"title", PreviewTitle},
       {"path", QDir(project->Directory).relativeFilePath(PreviewPath)},
       {"text", PreviewText}},
      side);
}
void AgentController::removeContext(int index, bool side) {
  auto *chat = side ? sideChat() : nullptr;
  if (side && !chat)
    return;
  auto context = side ? QJsonArray::fromVariantList(chat->draftContext())
                      : Conversation["context"].toArray();
  if (index < 0 || index >= context.size())
    return;
  context.removeAt(index);
  if (side) {
    QString error;
    if (!chat->setContext(context, error))
      emit operationError(error);
  } else {
    auto value = Conversation;
    value["context"] = context;
    commitConversation(ActiveId, value);
  }
}
void AgentController::createSideChatFromSelection(const QString &text) {
  if (text.trimmed().isEmpty() || !createSideChat())
    return;
  sideChat()->setDraft(
      tr("Explain this passage in the context of the main conversation:\n\n%1")
          .arg(text));
  emit sideChatOpened();
}
} // namespace neverd::agent
