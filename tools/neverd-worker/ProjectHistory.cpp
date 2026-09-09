#include "ProjectHistory.h"

#include <algorithm>
#include <fstream>

namespace neverd::worker {
namespace fs = std::filesystem;
namespace {
constexpr std::size_t MaxHistoryBytes = 8 * 1024 * 1024;
constexpr std::size_t MaxJournalBytes = 24 * 1024 * 1024;
fs::path sidecar(fs::path binary, const char *suffix) {
  binary += suffix;
  return binary;
}
Json read(const fs::path &path, std::size_t limit = MaxHistoryBytes) {
  std::error_code ec;
  if (!fs::exists(path, ec))
    return nullptr;
  const auto size = fs::file_size(path, ec);
  if (ec || size > limit)
    throw Error("history_budget_exceeded",
                "User state file exceeds its read budget");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw Error("history_read_failed", "Cannot open user state file");
  std::string text(static_cast<std::size_t>(size) + 1, '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  text.resize(static_cast<std::size_t>(input.gcount()));
  if (text.size() > size)
    throw Error("foreign_edits", "User state changed while it was being read");
  try {
    return parseJson(text, limit);
  } catch (const Error &) {
    throw Error("history_invalid", "User state file is not valid bounded JSON");
  }
}
void validateCommand(const Json &command) {
  const auto kind = stringField(command, "kind", {}, 32);
  if (kind != "annotation" && kind != "rename")
    throw Error("history_invalid", "Unsupported history command kind");
  parseAddress(stringField(command, "address"));
  if (!command.contains("before") || !command.contains("after"))
    throw Error("history_invalid", "History command is incomplete");
  for (const auto *field : {"before", "after"}) {
    const auto &value = command[field];
    if (kind == "annotation") {
      if (!value.is_string() ||
          value.get_ref<const std::string &>().size() > 65536)
        throw Error("history_invalid", "Annotation history value is invalid");
      (void)stringField(command, field, {}, 65536);
    } else if (!value.is_null()) {
      if (!value.is_object() || !value.contains("addr") ||
          !value.contains("renamed") || !value.contains("original"))
        throw Error("history_invalid", "Rename history value is invalid");
      if (parseAddress(stringField(value, "addr")) !=
          parseAddress(command["address"].get<std::string>()))
        throw Error("history_invalid",
                    "Rename history address does not match its command");
      (void)stringField(value, "renamed", {}, 4096);
      (void)stringField(value, "original", {}, 4096);
    }
  }
}
} // namespace

Json ProjectHistory::normalizedState(Json state) {
  if (!state.is_object())
    throw Error("history_invalid", "User state must be an object");
  for (const auto *name : {"annotations", "renames"}) {
    if (!state.contains(name) || !state[name].is_array())
      throw Error("history_invalid", "User state table is missing");
    auto &items = state[name];
    for (auto &item : items) {
      if (!item.is_object())
        throw Error("history_invalid", "User state item must be an object");
      item["addr"] = hexAddress(parseAddress(stringField(item, "addr")));
      if (std::string_view(name) == "annotations") {
        if (!item.contains("text"))
          throw Error("history_invalid", "Annotation text is missing");
        (void)stringField(item, "text", {}, 65536);
      } else {
        if (!item.contains("original") || !item.contains("renamed"))
          throw Error("history_invalid", "Rename text is missing");
        (void)stringField(item, "original", {}, 4096);
        (void)stringField(item, "renamed", {}, 4096);
      }
    }
    std::sort(items.begin(), items.end(), [](const Json &a, const Json &b) {
      return a.at("addr").get<std::string>() < b.at("addr").get<std::string>();
    });
    for (std::size_t i = 1; i < items.size(); ++i)
      if (items[i - 1].at("addr") == items[i].at("addr"))
        throw Error("history_invalid",
                    "User state contains duplicate addresses");
  }
  return state;
}

bool ProjectHistory::recoveryPending(const fs::path &binary) {
  return fs::exists(sidecar(binary, ".neverd-journal.json"));
}

ProjectHistory::ProjectHistory(fs::path binary, std::string sourceHash,
                               std::string engineVersion, bool readOnly,
                               AtomicWriter writer)
    : binary_(std::move(binary)),
      historyPath_(sidecar(binary_, ".neverd-history.json")),
      journalPath_(sidecar(binary_, ".neverd-journal.json")),
      hash_(std::move(sourceHash)), engineVersion_(std::move(engineVersion)),
      readOnly_(readOnly), writer_(writer) {
  fileSize_ = fs::file_size(binary_);
  fileTime_ = fs::last_write_time(binary_);
  if (hash_.size() != 64 ||
      hash_.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw Error("identity_unavailable",
                "Engine did not provide a valid source SHA-256");
  if (recoveryPending(binary_))
    recover();
  committedState_ = diskState();
  previousDocument_ = read(historyPath_);
  if (previousDocument_.is_null())
    return;
  try {
    if (previousDocument_.value("schema_version", 0) != 1)
      throw Error("history_invalid", "Unsupported history schema");
    if (previousDocument_.value("source_sha256", "") != hash_) {
      blocked_ = "input_changed";
      return;
    }
    if (normalizedState(previousDocument_.at("state")) != committedState_) {
      blocked_ = "foreign_edits";
      return;
    }
    commands_ = previousDocument_.at("commands");
    if (!commands_.is_array() || commands_.size() > 128)
      throw Error("history_invalid", "History command count is invalid");
    cursor_ = sizeField(previousDocument_, "cursor", 0, commands_.size());
    for (const auto &command : commands_)
      validateCommand(command);
  } catch (const Json::exception &) {
    throw Error("history_invalid", "History document is incomplete");
  }
}

Json ProjectHistory::diskState() const {
  auto annotations = read(sidecar(binary_, ".neverd-annotations.json"));
  auto renames = read(sidecar(binary_, ".neverd-renames.json"));
  return normalizedState(
      {{"annotations", annotations.is_null() ? Json::array() : annotations},
       {"renames", renames.is_null() ? Json::array() : renames}});
}

void ProjectHistory::verifyLoadedState(const Json &state) {
  auto loaded = normalizedState(state);
  auto committed = committedState_;
  // The C ABI reconstructs original symbol names independently; only current
  // user rename values are part of the loaded edit-state identity here.
  for (auto &row : loaded["renames"])
    row.erase("original");
  for (auto &row : committed["renames"])
    row.erase("original");
  if (loaded != committed && blocked_.empty())
    blocked_ = "foreign_edits";
}

void ProjectHistory::requireUsable() const {
  if (!blocked_.empty())
    throw Error(blocked_, "History does not match the current input or "
                          "sidecars; explicitly reset history before editing");
}

Json ProjectHistory::listing(std::size_t offset, std::size_t limit) const {
  Json items = Json::array();
  for (std::size_t i = std::min(offset, commands_.size());
       i < commands_.size() && items.size() < limit; ++i) {
    auto command = commands_[i];
    command["index"] = i;
    command["applied"] = i < cursor_;
    items.push_back(std::move(command));
  }
  const bool complete =
      offset >= commands_.size() || items.size() >= commands_.size() - offset;
  return {
      {"schema_version", 1},
      {"items", items},
      {"total", commands_.size()},
      {"cursor", cursor_},
      {"offset", offset},
      {"next_offset", complete ? Json(nullptr) : Json(offset + items.size())},
      {"complete", complete},
      {"can_undo", blocked_.empty() && cursor_ > 0},
      {"can_redo", blocked_.empty() && cursor_ < commands_.size()},
      {"available", blocked_.empty()},
      {"blocked_reason", blocked_},
      {"source_sha256", hash_}};
}

void ProjectHistory::stage(Json command) {
  requireUsable();
  validateCommand(command);
  commands_.erase(commands_.begin() +
                      static_cast<Json::difference_type>(cursor_),
                  commands_.end());
  commands_.push_back(std::move(command));
  ++cursor_;
  while (commands_.size() > 128 ||
         commands_.dump().size() > MaxHistoryBytes / 2) {
    commands_.erase(commands_.begin());
    --cursor_;
  }
}

Json ProjectHistory::next(bool redo) const {
  requireUsable();
  if (redo ? cursor_ == commands_.size() : cursor_ == 0)
    throw Error("history_empty", redo ? "Nothing to redo" : "Nothing to undo");
  return commands_[redo ? cursor_ : cursor_ - 1];
}
void ProjectHistory::advance(bool redo) {
  (void)next(redo);
  if (redo)
    ++cursor_;
  else
    --cursor_;
}

Json ProjectHistory::document(const Json &state) const {
  return {{"schema_version", 1},
          {"source_sha256", hash_},
          {"engine_version", engineVersion_},
          {"cursor", cursor_},
          {"commands", commands_},
          {"state", state}};
}

void ProjectHistory::persist(const Json &state) {
  requireUsable();
  if (readOnly_)
    throw Error("read_only", "History persistence requires the writer lock");
  if (fs::file_size(binary_) != fileSize_ ||
      fs::last_write_time(binary_) != fileTime_)
    throw Error(
        "input_changed",
        "Input changed while the Session was open; reopen it before editing");
  if (recoveryPending(binary_))
    throw Error("recovery_required",
                "An interrupted edit must be recovered before another save");
  if (diskState() != committedState_ || read(historyPath_) != previousDocument_)
    throw Error("foreign_edits", "Sidecars or history changed outside this "
                                 "worker; reload and inspect before editing");
  const auto after = normalizedState(state);
  const auto nextDocument = document(after);
  if (nextDocument.dump().size() > MaxHistoryBytes)
    throw Error("history_budget_exceeded",
                "Persisted user history exceeds 8 MiB");
  const Json journal{{"schema_version", 1},
                     {"source_sha256", hash_},
                     {"before", committedState_},
                     {"after", after},
                     {"before_history", previousDocument_},
                     {"after_history", nextDocument}};
  if (journal.dump().size() > MaxJournalBytes)
    throw Error("history_budget_exceeded", "Recovery journal exceeds 24 MiB");
  writer_(journalPath_, journal);
  // From this point any failure leaves a recoverable write-ahead journal.
  writer_(sidecar(binary_, ".neverd-annotations.json"),
          after.at("annotations"));
  writer_(sidecar(binary_, ".neverd-renames.json"), after.at("renames"));
  writer_(historyPath_, nextDocument);
  std::error_code ec;
  if (!fs::remove(journalPath_, ec) || ec)
    throw Error(
        "recovery_required",
        "User edits were saved but journal cleanup failed; restart to recover");
  committedState_ = after;
  previousDocument_ = nextDocument;
}

void ProjectHistory::recover() {
  if (readOnly_)
    throw Error("recovery_required", "A pending user-edit transaction requires "
                                     "a writable worker to recover");
  const auto journal = read(journalPath_, MaxJournalBytes);
  if (!journal.is_object() || journal.value("schema_version", 0) != 1 ||
      journal.value("source_sha256", "") != hash_)
    throw Error("input_changed", "Recovery journal does not match this input; "
                                 "no sidecars were overwritten");
  try {
    const auto before = normalizedState(journal.at("before"));
    const auto after = normalizedState(journal.at("after"));
    const auto current = diskState();
    for (const auto *table : {"annotations", "renames"})
      if (current.at(table) != before.at(table) &&
          current.at(table) != after.at(table))
        throw Error("foreign_edits",
                    "User sidecars changed after the interrupted transaction; "
                    "automatic recovery refused");
    const auto currentHistory = read(historyPath_);
    if (currentHistory != journal.at("before_history") &&
        currentHistory != journal.at("after_history"))
      throw Error("foreign_edits", "History changed after the interrupted "
                                   "transaction; automatic recovery refused");
    const auto &history = journal.at("after_history");
    if (history.value("schema_version", 0) != 1 ||
        history.value("source_sha256", "") != hash_ ||
        normalizedState(history.at("state")) != after)
      throw Error("history_invalid",
                  "Recovery history does not describe its target state");
    const auto &commands = history.at("commands");
    if (!commands.is_array() || commands.size() > 128 ||
        history.dump().size() > MaxHistoryBytes)
      throw Error("history_invalid",
                  "Recovery history exceeds its command or byte budget");
    (void)sizeField(history, "cursor", 0, commands.size());
    for (const auto &command : commands)
      validateCommand(command);
    writer_(sidecar(binary_, ".neverd-annotations.json"),
            after.at("annotations"));
    writer_(sidecar(binary_, ".neverd-renames.json"), after.at("renames"));
    writer_(historyPath_, history);
    std::error_code ec;
    if (!fs::remove(journalPath_, ec) || ec)
      throw Error("recovery_required", "Cannot clean up recovered journal");
    recovered_ = true;
  } catch (const Json::exception &) {
    throw Error("history_invalid", "Recovery journal is incomplete");
  }
}

void ProjectHistory::reset() {
  if (readOnly_)
    throw Error("read_only", "History reset requires the writer lock");
  if (recoveryPending(binary_))
    throw Error("recovery_required",
                "Recover the pending transaction before resetting history");
  const auto checkpoint = *this;
  try {
    committedState_ = diskState();
    previousDocument_ = read(historyPath_);
    commands_ = Json::array();
    cursor_ = 0;
    blocked_.clear();
    persist(committedState_);
  } catch (...) {
    *this = checkpoint;
    throw;
  }
}
} // namespace neverd::worker
