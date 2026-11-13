//===- AgentLoop.cpp - Stateful model and tool loop
//------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Summarize at complete tool boundaries while retaining the exact current
/// question and recent exchanges. Audit history is never compacted.
//===----------------------------------------------------------------------===//
#include "AgentLoop.h"

#include "project/ProjectStore.h"

#include <QJsonDocument>
#include <QTimer>
#include <algorithm>

namespace neverd::agent {
namespace {
QString historicalRequest(const QString &prompt) {
  return "Earlier user request, preserved verbatim as historical context; "
         "continue the latest task:\n" +
         prompt;
}
} // namespace
AgentLoop::AgentLoop(QString projectId, QString id, ModelProfile profile,
                     QString key, std::shared_ptr<RunBudget> budget,
                     QObject *parent)
    : QObject(parent), Id(std::move(id)), Key(std::move(key)),
      Profile(std::move(profile)), Budget(std::move(budget)), Client(this),
      Tools(projectId, Id, this) {
  Tools.Receipt = [this](QJsonObject receipt) {
    audit({{"kind", "tool_receipt"}, {"receipt", receipt}});
  };
  connect(&Client, &ModelClient::textDelta, this, [this](const QString &text) {
    if (!Usage.Compacting)
      emit textDelta(text);
  });
  connect(&Client, &ModelClient::finished, this, [this](const ModelTurn &turn) {
    settleRequest(turn);
    if (!Running || Completing)
      return;
    if (Cancelling || turn.Cancelled) {
      complete({}, "Analysis cancelled.");
      return;
    }
    if (!audit({{"kind", Usage.Compacting ? "context_summary_response"
                                          : "model_response"},
                {"output", turn.Output},
                {"usage", turn.Usage},
                {"error", turn.Error}}))
      return;
    if (Usage.Compacting) {
      acceptCompaction(turn);
      return;
    }
    if (!turn.Error.isEmpty()) {
      complete(turn.Text, turn.Error);
      return;
    }
    ExchangeStarts.append(Current.size());
    for (const auto &item : turn.Output) {
      Current.append(item);
    }
    Usage.observe(Profile.Protocol, turn.Usage,
                  ProviderCodec::request(
                      Profile, System, history(Checkpoint, PastGroups, Current),
                      VisibleTools));
    emit changed();
    if (turn.Calls.isEmpty()) {
      if (!PendingSteers.isEmpty()) {
        if (applySteering())
          QTimer::singleShot(0, this, &AgentLoop::next);
        return;
      }
      complete(turn.Text);
      return;
    }
    Calls = turn.Calls;
    Results = QList<QJsonObject>(Calls.size());
    FinishedCalls.clear();
    NextCall = 0;
    invokeNext();
  });
}
bool AgentLoop::audit(QJsonObject event) {
  if (!Audit)
    return true;
  event["agentId"] = Id;
  QString error;
  if (Audit(event, error))
    return true;
  complete({}, "Unable to save analysis evidence: " + error);
  return false;
}
void AgentLoop::start(QString system, QString prompt, QString sessionPath) {
  Running = true;
  ++Budget->Active;
  Status = "running";
  System = std::move(system);
  Prompt = std::move(prompt);
  SessionPath = std::move(sessionPath);
  if (Cancelling) {
    complete({}, "Analysis cancelled.");
    return;
  }
  QString error;
  if (!Tools.activate(error)) {
    complete({}, error);
    return;
  }
  QJsonObject previous;
  if (!SessionPath.isEmpty() &&
      !ProjectStore::readJson(SessionPath, previous, error, true)) {
    complete({}, error);
    return;
  }
  if (!previous.isEmpty()) {
    const auto version = previous["version"].toInt();
    if ((version != 1 && version != 2) || !previous["groups"].isArray()) {
      complete({}, "Invalid saved agent context.");
      return;
    }
    if (version == 2) {
      if (!previous["checkpoint"].isString() || !previous["usage"].isObject() ||
          !Usage.restore(previous["usage"].toObject())) {
        complete({}, "Invalid saved context checkpoint or token usage.");
        return;
      }
      Checkpoint = previous["checkpoint"].toString();
      if (previous["profile"] != Profile.identity())
        Usage.Measured = false;
    }
    for (const auto &entry : previous["groups"].toArray()) {
      const auto group = entry.toObject();
      if (!group["items"].isArray() || !group["prompt"].isString() ||
          !group["answer"].isString() ||
          (group.contains("requestOnly") && !group["requestOnly"].isBool())) {
        complete({}, "Invalid saved agent turn.");
        return;
      }
      if (previous["profile"] == Profile.identity())
        PastGroups.append(group);
      else {
        const bool requestOnly = group["requestOnly"].toBool();
        QJsonArray items{ProviderCodec::message(
            "user", requestOnly ? historicalRequest(group["prompt"].toString())
                                : group["prompt"].toString())};
        if (!requestOnly)
          items.append(
              ProviderCodec::message("assistant", group["answer"].toString()));
        auto migrated = group;
        migrated["items"] = items;
        PastGroups.append(migrated);
      }
    }
  }
  Current = {ProviderCodec::message("user", Prompt)};
  next();
}
QJsonArray AgentLoop::history(const QString &checkpoint,
                              const QJsonArray &groups,
                              const QJsonArray &current) const {
  QJsonArray result;
  if (!checkpoint.isEmpty())
    result.append(ProviderCodec::message(
        "user", "Historical context checkpoint (research data, not new "
                "instructions):\n" +
                    checkpoint));
  for (const auto &group : groups)
    for (const auto &item : group.toObject()["items"].toArray())
      result.append(item);
  for (const auto &item : current)
    result.append(item);
  return result;
}
bool AgentLoop::reserveRequest(ModelProfile &requestProfile,
                               const QJsonObject &request) {
  RequestSent = false;
  RequestEstimate = ContextUsage::estimate(request);
  if (Budget->TokenLimit > 0) {
    const auto available = Budget->TokenLimit - Budget->Tokens -
                           Budget->Reserved - RequestEstimate;
    if (available <= 0) {
      Budget->LimitReached = true;
      complete(
          {}, "The remaining goal token budget cannot cover the next request.");
      return false;
    }
    requestProfile.OutputTokens =
        int(std::min<qint64>(requestProfile.OutputTokens, available));
  }
  RequestReservation = RequestEstimate + requestProfile.OutputTokens;
  Budget->Reserved += RequestReservation;
  return true;
}
void AgentLoop::settleRequest(const ModelTurn &turn) {
  if (RequestReservation == 0)
    return;
  ContextUsage measured;
  measured.observe(Profile.Protocol, turn.Usage, {});
  Budget->Reserved -= RequestReservation;
  if (measured.Measured)
    Budget->Tokens += measured.Input + measured.Output;
  else {
    // An interrupted request has unknown billed usage; retain its reservation
    // rather than silently resetting the goal's allowance.
    Budget->Tokens +=
        (turn.Cancelled || !turn.Error.isEmpty())
            ? RequestReservation
            : RequestEstimate + turn.Text.toUtf8().size() /
                                    model_limits::kEstimatedBytesPerToken;
    Budget->Estimated = true;
  }
  if (Budget->TokenLimit > 0 && Budget->Tokens >= Budget->TokenLimit)
    Budget->LimitReached = true;
  RequestReservation = 0;
  RequestSent = false;
  emit changed();
}
void AgentLoop::next() {
  if (!Running || Completing)
    return;
  if (Cancelling) {
    complete({}, "Analysis cancelled.");
    return;
  }
  if (!applySteering())
    return;
  QString error;
  VisibleTools = Tools.resolve(Steps + 1, error);
  if (!error.isEmpty()) {
    complete({}, error);
    return;
  }
  History = history(Checkpoint, PastGroups, Current);
  auto request = ProviderCodec::request(Profile, System, History, VisibleTools);
  Usage.prepare(request);
  const qint64 allowance = Profile.ContextTokens - Profile.OutputTokens;
  if (Usage.Tokens >= Profile.compactionThreshold()) {
    if (!PastGroups.isEmpty() || Current.size() > 1 || !Checkpoint.isEmpty()) {
      beginCompaction();
      return;
    }
    if (Usage.Tokens > allowance) {
      complete({},
               "The current question, instructions and tool schemas exceed "
               "the context window. There is no older context to summarize.");
      return;
    }
  }
  auto requestProfile = Profile;
  if (!reserveRequest(requestProfile, request))
    return;
  ++Steps;
  ++Budget->Steps;
  request =
      ProviderCodec::request(requestProfile, System, History, VisibleTools);
  if (!audit({{"kind", "model_request"},
              {"profile", Profile.toJson()},
              {"request", request}}))
    return;
  Activity = QStringLiteral("正在思考 · 第 %1 步").arg(Steps);
  emit changed();
  if (Cancelling || Completing) {
    complete({}, "Analysis cancelled.");
    return;
  }
  RequestSent = true;
  Client.start(requestProfile, Key, System, History, VisibleTools);
}
void AgentLoop::beginCompaction() {
  Usage.Compacting = true;
  Usage.Before = Usage.Tokens;
  CompactBeforeEstimate = ContextUsage::estimate(
      ProviderCodec::request(Profile, System, History, VisibleTools));
  CompactSummary.clear();
  CompactOffset = CompactEnd = 0;
  RetainedGroups = {};
  RetainedExchangeStarts.clear();
  RetainedCurrent = {ProviderCodec::message("user", Prompt)};
  const qint64 target = qint64(Profile.ContextTokens - Profile.OutputTokens) *
                            model_limits::kCompactTargetPercent / 100 -
                        Profile.compactionOutputTokens();
  const qint64 nativeBudget =
      PastGroups.isEmpty()
          ? target
          : target * (100 - model_limits::kCompactUserPercent) / 100;
  const auto range = [](const QJsonArray &items, qsizetype begin,
                        qsizetype end) {
    QJsonArray result;
    for (qsizetype i = begin; i < end; ++i)
      result.append(items[i]);
    return result;
  };
  const auto retainedCost = [&](const QJsonArray &groups,
                                const QJsonArray &current) {
    return ContextUsage::estimate(ProviderCodec::request(
        Profile, System, history({}, groups, current), VisibleTools));
  };
  // A model output and its entire tool-result batch are one retention unit.
  // No compaction runs between tool calls, including delegated subtasks.
  qsizetype currentFrom = Current.size(), groupsFrom = PastGroups.size();
  for (auto it = ExchangeStarts.crbegin(); it != ExchangeStarts.crend(); ++it) {
    QJsonArray recent{ProviderCodec::message("user", Prompt)};
    for (const auto &item : range(Current, *it, Current.size()))
      recent.append(item);
    if (retainedCost({}, recent) > nativeBudget)
      break;
    RetainedCurrent = recent;
    currentFrom = *it;
  }
  for (const auto start : ExchangeStarts)
    if (start >= currentFrom)
      RetainedExchangeStarts.append(1 + start - currentFrom);
  // Preserve a contiguous recent suffix. Older native turns are retained only
  // when the current turn itself is intact.
  if (currentFrom <= 1) {
    for (qsizetype i = PastGroups.size(); i > 0; --i) {
      const auto recent = range(PastGroups, i - 1, PastGroups.size());
      if (retainedCost(recent, RetainedCurrent) > nativeBudget)
        break;
      RetainedGroups = recent;
      groupsFrom = i - 1;
    }
  }
  QJsonArray requests;
  for (qsizetype i = groupsFrom; i > 0; --i) {
    const auto prompt = PastGroups[i - 1].toObject()["prompt"].toString();
    if (prompt.isEmpty())
      continue;
    auto candidate = requests;
    candidate.prepend(
        QJsonObject{{"prompt", prompt},
                    {"answer", ""},
                    {"requestOnly", true},
                    {"items", QJsonArray{ProviderCodec::message(
                                  "user", historicalRequest(prompt))}}});
    for (const auto &group : RetainedGroups)
      candidate.append(group);
    if (retainedCost(candidate, RetainedCurrent) > target)
      break;
    requests.prepend(candidate.first());
  }
  for (qsizetype i = requests.size(); i > 0; --i)
    RetainedGroups.prepend(requests[i - 1]);
  const auto older =
      history(Checkpoint, range(PastGroups, 0, groupsFrom),
              currentFrom > 1 ? range(Current, 0, currentFrom) : QJsonArray{});
  CompactSource = compactionTranscript(Profile.Protocol, older);
  CompactRequests = 0;
  if (!audit({{"kind", "context_compaction_started"},
              {"beforeTokens", Usage.Before},
              {"retainedTurns", RetainedGroups.size() - requests.size()},
              {"retainedExchanges", RetainedExchangeStarts.size()},
              {"preservedRequests", requests.size()},
              {"sourceCharacters", CompactSource.size()}}))
    return;
  compactNext();
}
void AgentLoop::compactNext() {
  if (!Running || Completing)
    return;
  if (Cancelling) {
    complete({}, "Analysis cancelled.");
    return;
  }
  if (CompactOffset == CompactSource.size()) {
    const auto candidate =
        history(CompactSummary, RetainedGroups, RetainedCurrent);
    const auto request =
        ProviderCodec::request(Profile, System, candidate, VisibleTools);
    const auto after = ContextUsage::estimate(request);
    const auto trigger = Profile.compactionThreshold();
    if (after >= CompactBeforeEstimate || after >= trigger) {
      complete(
          {},
          "Context summary did not free enough space. Original context "
          "was retained; increase the context window or narrow the request.");
      return;
    }
    if (!audit({{"kind", "context_compacted"},
                {"checkpoint", CompactSummary},
                {"beforeTokens", Usage.Before},
                {"afterEstimate", after}}))
      return;
    // Prepare the new state, then atomically save it before continuing. No
    // partially generated checkpoint is ever installed.
    const auto oldCheckpoint = Checkpoint;
    const auto oldGroups = PastGroups;
    const auto oldCurrent = Current;
    const auto oldUsage = Usage;
    Checkpoint = CompactSummary;
    PastGroups = RetainedGroups;
    Current = RetainedCurrent;
    Usage.Compacting = false;
    ++Usage.Compactions;
    Usage.reset(request);
    Usage.After = Usage.Tokens;
    Usage.PreservedRequests = 0;
    for (const auto &group : RetainedGroups)
      Usage.PreservedRequests +=
          group.toObject()["requestOnly"].toBool() ? 1 : 0;
    Usage.RetainedTurns = RetainedGroups.size() - Usage.PreservedRequests;
    Usage.RetainedExchanges = RetainedExchangeStarts.size();
    Usage.SummaryRequests = CompactRequests;
    QString error;
    if (!persist("Task in progress; continue from the checkpoint.", Current,
                 error)) {
      Checkpoint = oldCheckpoint;
      PastGroups = oldGroups;
      Current = oldCurrent;
      Usage = oldUsage;
      complete({}, "Unable to save context checkpoint: " + error);
      return;
    }
    ExchangeStarts = RetainedExchangeStarts;
    CompactSource.clear();
    CompactSummary.clear();
    Activity = QStringLiteral("上下文已压缩 · %1 → 约 %2 tokens")
                   .arg(Usage.Before)
                   .arg(Usage.After);
    emit changed();
    QTimer::singleShot(0, this, &AgentLoop::next);
    return;
  }
  ModelProfile summaryProfile = Profile;
  summaryProfile.OutputTokens = Profile.compactionOutputTokens();
  const QString system =
      compactionInstructions() + "Analysis identity and rules:\n" + System;
  const QString prefix =
      "Current user request (retain exactly in intent):\n" + Prompt +
      "\nCheckpoint from preceding segments:\n" + CompactSummary +
      "\nNext ordered transcript segment (may continue a JSONL record):\n";
  const auto input = [&](qsizetype length) {
    return QJsonArray{ProviderCodec::message(
        "user", prefix + CompactSource.mid(CompactOffset, length))};
  };
  const qint64 limit =
      qint64(Profile.ContextTokens - summaryProfile.OutputTokens) *
      model_limits::kAutoCompactPercent / 100;
  // Split only the summarizer's evidence text. Provider-native tool pairs in
  // the retained history are never split or rewritten.
  qsizetype low = 0, high = CompactSource.size() - CompactOffset;
  while (low < high) {
    const auto middle = low + (high - low + 1) / 2;
    if (ContextUsage::estimate(ProviderCodec::request(
            summaryProfile, system, input(middle), {})) <= limit)
      low = middle;
    else
      high = middle - 1;
  }
  if (low > 0 && CompactOffset + low < CompactSource.size() &&
      CompactSource[CompactOffset + low - 1].isHighSurrogate())
    --low;
  if (low == 0) {
    complete({}, "The question and checkpoint leave no space for context "
                 "summarization. Original context was retained.");
    return;
  }
  CompactEnd = CompactOffset + low;
  const auto summaryInput = input(low);
  if (!reserveRequest(
          summaryProfile,
          ProviderCodec::request(summaryProfile, system, summaryInput, {})))
    return;
  ++Steps;
  ++Budget->Steps;
  if (!audit({{"kind", "context_summary_request"},
              {"offset", CompactOffset},
              {"end", CompactEnd},
              {"request", ProviderCodec::request(summaryProfile, system,
                                                 summaryInput, {})}}))
    return;
  ++CompactRequests;
  Activity = QStringLiteral("正在整理较早的记录 · 保留近期对话与用户要求");
  emit changed();
  if (Cancelling || Completing) {
    complete({}, "Analysis cancelled.");
    return;
  }
  RequestSent = true;
  Client.start(summaryProfile, Key, system, summaryInput, {});
}
void AgentLoop::acceptCompaction(const ModelTurn &turn) {
  if (!turn.Error.isEmpty() || turn.Text.trimmed().isEmpty() ||
      !turn.Calls.isEmpty()) {
    complete({}, "Context compaction failed; original context retained. " +
                     (turn.Error.isEmpty()
                          ? "Expected a nonempty summary without tool calls."
                          : turn.Error));
    return;
  }
  CompactSummary = turn.Text.trimmed();
  CompactOffset = CompactEnd;
  QTimer::singleShot(0, this, &AgentLoop::compactNext);
}
QJsonObject AgentLoop::contextSnapshot(const QString &answer,
                                       const QJsonArray &items) const {
  auto groups = PastGroups;
  groups.append(
      QJsonObject{{"prompt", Prompt}, {"answer", answer}, {"items", items}});
  return {{"version", 2},
          {"profile", Profile.identity()},
          {"checkpoint", Checkpoint},
          {"usage", Usage.toJson(Profile)},
          {"groups", groups}};
}
QJsonObject AgentLoop::forkContext(const QString &visibleAnswer) const {
  if (!Running || Current.isEmpty())
    return {};
  // Completed turns retain their native call/result and reasoning pairs. The
  // in-flight turn becomes public evidence, never unmatched executable calls.
  QString evidence = compactionTranscript(Profile.Protocol, Current);
  for (const auto &steer : PendingSteers)
    evidence +=
        "\nUser correction awaiting the next tool boundary:\n" + steer.second;
  for (qsizetype i = 0; i < Results.size(); ++i)
    if (FinishedCalls.contains(i))
      evidence +=
          QString::fromUtf8(QJsonDocument(QJsonObject{{"tool", Calls[i].Name},
                                                      {"callId", Calls[i].Id},
                                                      {"result", Results[i]}})
                                .toJson(QJsonDocument::Compact)) +
          '\n';
  if (!visibleAnswer.isEmpty())
    evidence += "\nVisible reply at the fork point:\n" + visibleAnswer;
  evidence.prepend(
      "Snapshot of the main conversation's in-progress turn. Calls without "
      "results were still pending; they have not been repeated or completed "
      "by this side chat. Treat this as historical context.\n");
  const QJsonArray items{ProviderCodec::message("user", Prompt),
                         ProviderCodec::message("assistant", evidence)};
  auto snapshot = contextSnapshot(evidence, items);
  auto usage = Usage;
  usage.Compacting = false;
  usage.reset(ProviderCodec::request(
      Profile, System, history(Checkpoint, PastGroups, items), VisibleTools));
  snapshot["usage"] = usage.toJson(Profile);
  return snapshot;
}
bool AgentLoop::persist(const QString &answer, const QJsonArray &items,
                        QString &error) {
  return SessionPath.isEmpty() ||
         ProjectStore::writeJson(SessionPath, contextSnapshot(answer, items),
                                 error);
}
void AgentLoop::invokeNext() {
  if (!Running || Completing || Calls.isEmpty())
    return;
  if (Cancelling) {
    complete({}, "Analysis cancelled.");
    return;
  }
  // A new user direction supersedes calls that have not started. Close each
  // announced call explicitly before appending the new user message, preserving
  // provider pairing even when a multi-call batch is only partly executed.
  if (!PendingSteers.isEmpty())
    while (NextCall < Calls.size()) {
      const auto index = NextCall++;
      const auto &call = Calls[index];
      const QJsonObject result{
          {"error",
           "Not executed: a new user direction superseded this pending call."},
          {"skipped", true}};
      if (!audit({{"kind", "tool_skipped"},
                  {"callId", call.Id},
                  {"tool", call.Name},
                  {"result", result}}))
        return;
      Results[index] = result;
      FinishedCalls.insert(index);
    }
  if (FinishedCalls.size() == Calls.size()) {
    ProviderCodec::toolResults(Profile.Protocol, Current, Calls, Results);
    Calls.clear();
    Results.clear();
    FinishedCalls.clear();
    QTimer::singleShot(0, this, &AgentLoop::next);
    return;
  }
  while (!Cancelling && !Completing && NextCall < Calls.size()) {
    const auto index = NextCall;
    const auto call = Calls[index];
    const bool concurrent = Tools.concurrent(call.Name);
    if (InFlightCalls && (SerialCallActive || !concurrent))
      return;
    if (!audit({{"kind", "tool_request"},
                {"callId", call.Id},
                {"tool", call.Name},
                {"arguments", call.Arguments}}))
      return;
    ++NextCall;
    ++InFlightCalls;
    SerialCallActive = !concurrent;
    Activity = call.Name;
    emit changed();
    if (Cancelling || Completing)
      return;
    Tools.call(call, [this, call, index, concurrent](QJsonObject result) {
      if (!Running || Completing)
        return;
      if (!audit({{"kind", "tool_result"},
                  {"callId", call.Id},
                  {"tool", call.Name},
                  {"result", result}}))
        return;
      Results[index] = result;
      FinishedCalls.insert(index);
      --InFlightCalls;
      if (!concurrent)
        SerialCallActive = false;
      QTimer::singleShot(0, this, &AgentLoop::invokeNext);
    });
    if (!concurrent)
      return;
  }
}
bool AgentLoop::steer(const QString &inputId, const QString &text) {
  if (Cancelling || Completing || (!Running && !Status.isEmpty()))
    return false;
  PendingSteers.append({inputId, text});
  emit changed();
  return true;
}
bool AgentLoop::applySteering() {
  while (!PendingSteers.isEmpty()) {
    const auto input = PendingSteers.takeFirst();
    if (!audit({{"kind", "user_input_applied"},
                {"inputId", input.first},
                {"text", input.second}}))
      return false;
    Current.append(ProviderCodec::message("user", input.second));
    // The compaction retention anchor must contain every accepted correction,
    // including when the correction arrived after the original prompt.
    Prompt += "\n\nUser follow-up:\n" + input.second;
    emit steeringApplied(input.first);
  }
  return true;
}
void AgentLoop::cancel() {
  if (!Running && Status.isEmpty()) {
    Cancelling = true;
    return;
  }
  if (!Running || Completing)
    return;
  Cancelling = true;
  Client.cancel();
  if (!Running || Completing)
    return;
  Tools.cancel();
  // Model cancellation is complete only after its reply has settled usage.
  // The finished callback handles completion while a request is still present.
  if (Client.running())
    return;
  QTimer::singleShot(0, this, [this] {
    if (Running && !Completing)
      complete({}, "Analysis cancelled.");
  });
}
void AgentLoop::complete(QString text, QString error) {
  if (!Running || Completing)
    return;
  Completing = true;
  if (RequestReservation && !RequestSent) {
    Budget->Reserved -= RequestReservation;
    RequestReservation = 0;
  }
  // Current is initialized only after the saved context has been validated.
  // A failed load must never overwrite that file. Failed/cancelled turns keep
  // their question and visible diagnosis, without unmatched native tool calls.
  if (!SessionPath.isEmpty() && !Current.isEmpty()) {
    const QString summary =
        error.isEmpty() ? text : text + "\nUnfinished task: " + error;
    QJsonArray items = Current;
    if (!error.isEmpty() && !Calls.isEmpty()) {
      auto results = Results;
      for (qsizetype i = 0; i < Calls.size(); ++i)
        if (!FinishedCalls.contains(i))
          results[i] = QJsonObject{
              {"error", "Interrupted before a result was available. Do not "
                        "infer completion or automatically repeat this call."},
              {"interrupted", true}};
      ProviderCodec::toolResults(Profile.Protocol, items, Calls, results);
    }
    if (!error.isEmpty())
      items.append(ProviderCodec::message("assistant", summary));
    Usage.Compacting = false;
    QString storageError;
    if (!persist(summary, items, storageError))
      error = "Unable to persist agent context: " + storageError;
  }
  Tools.close([this, text = std::move(text), error = std::move(error)] {
    Running = false;
    --Budget->Active;
    Key.clear();
    Status = Cancelling        ? "cancelled"
             : error.isEmpty() ? "succeeded"
                               : "failed";
    Activity = error.isEmpty() ? QStringLiteral("已完成") : error;
    emit changed();
    emit finished(text, error, Cancelling);
  });
}
} // namespace neverd::agent
