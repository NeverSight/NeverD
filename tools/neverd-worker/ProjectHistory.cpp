#include "ProjectHistory.h"

#include "CodeEdits.h"

#include "neverd/support/ProjectFiles.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace neverd::worker {
namespace fs = std::filesystem;
namespace {
// The history and the recovery journal are the user's own records and stay
// compact; the edit tables are the project's shared state, indented for
// version control (sidecarText), and each file has its own bound.
constexpr std::size_t MaxHistoryBytes = 8 * 1024 * 1024;
constexpr std::size_t MaxJournalBytes = 24 * 1024 * 1024;
constexpr std::size_t MaxStateFileBytes = 16 * 1024 * 1024;
std::string compactText(const Json &value) {
  return value.dump(-1, ' ', false, Json::error_handler_t::replace);
}
fs::path sidecar(const fs::path &binary, project_files::File file) {
  return project_files::path(binary, file);
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
    // A merge git could not finish leaves its markers in the file.
    if (text.starts_with("<<<<<<< ") ||
        text.find("\n<<<<<<< ") != std::string::npos)
      throw Error("merge_conflict",
                  path.filename().string() +
                      " has unresolved merge conflicts; resolve them before "
                      "opening the project");
    throw Error("history_invalid", "User state file is not valid bounded JSON");
  }
}
/// The fields of a row of \p table besides its address.
void validateRow(std::string_view table, const Json &row) {
  if (table == "code_edits") {
    CodeEdits::validateRow(row);
  } else if (table == "annotations") {
    if (!row.contains("text"))
      throw Error("history_invalid", "Annotation text is missing");
    (void)stringField(row, "text", {}, 65536);
  } else if (table == "renames") {
    if (!row.contains("original") || !row.contains("renamed"))
      throw Error("history_invalid", "Rename text is missing");
    (void)stringField(row, "original", {}, 4096);
    (void)stringField(row, "renamed", {}, 4096);
  } else if (table == "functions") {
    // A row edits whether a function starts there, what it is, or both.
    bool edits = false;
    if (row.contains("state")) {
      const auto state = stringField(row, "state", {}, 16);
      if (state != "created" && state != "deleted")
        throw Error("history_invalid", "Function edit state is invalid");
      edits = true;
    }
    const auto isColor = [](const std::string &text) {
      return text.size() == 7 && text[0] == '#' &&
             std::all_of(text.begin() + 1, text.end(), [](char c) {
               return std::isxdigit(static_cast<unsigned char>(c));
             });
    };
#define NEVERD_FUNCTION_FLAG(Member, Key)                                      \
  if (row.contains(Key)) {                                                     \
    if (!row.at(Key).is_boolean())                                             \
      throw Error("history_invalid", "Function attribute is invalid");         \
    edits = true;                                                              \
  }
#define NEVERD_FUNCTION_COLOR(Key)                                             \
  if (row.contains(Key)) {                                                     \
    if (!isColor(stringField(row, Key, {}, 7)))                                \
      throw Error("history_invalid", "Function color is invalid");             \
    edits = true;                                                              \
  }
// The engine reads a declaration against the local types when it loads it.
#define NEVERD_FUNCTION_TYPE(Member, Key)                                      \
  if (row.contains(Key)) {                                                     \
    if (stringField(row, Key, {}, 4096).empty())                               \
      throw Error("history_invalid", "Function declaration is empty");         \
    edits = true;                                                              \
  }
#include "neverd/sdk/FunctionAttributes.def"
    if (!edits)
      throw Error("history_invalid", "Function edit is empty");
  } else if (table == "items") {
    // The engine checks a data item against the image when it loads one.
    (void)stringField(row, "kind", {}, 32);
    if (!row.contains("size") || !row.at("size").is_number_unsigned() ||
        row.at("size").get<std::uint64_t>() == 0)
      throw Error("history_invalid", "Data item size is invalid");
    if (row.contains("encoding"))
      (void)stringField(row, "encoding", {}, 64);
  } else {
    // The engine checks each operand's base when it loads the formats.
    if (!row.contains("operands") || !row.at("operands").is_array() ||
        row.at("operands").size() > 8)
      throw Error("history_invalid", "Operand formats are invalid");
    for (const auto &operand : row.at("operands")) {
      if (!operand.is_object() || !operand.contains("operand") ||
          !operand.at("operand").is_number_unsigned() ||
          operand.at("operand").get<std::uint64_t>() > 7)
        throw Error("history_invalid", "Operand index is invalid");
      (void)stringField(operand, "base", {}, 16);
      for (const char *flag : {"negate", "invert"})
        if (operand.contains(flag) && !operand.at(flag).is_boolean())
          throw Error("history_invalid", "Operand format flag is invalid");
      // A structure offset's type and delta.
      if (operand.contains("type"))
        (void)stringField(operand, "type", {}, 512);
      if (operand.contains("delta") && !operand.at("delta").is_number_integer())
        throw Error("history_invalid", "Operand structure delta is invalid");
    }
  }
}

/// The most rows besides its own one history command may change.
constexpr std::size_t MaxCommandRows = 64;

void validateChange(std::string_view kind, const char *table,
                    const Json &change) {
  parseAddress(stringField(change, "address"));
  if (!change.contains("before") || !change.contains("after"))
    throw Error("history_invalid", "History command is incomplete");
  for (const auto *field : {"before", "after"}) {
    const auto &value = change[field];
    if (kind == "annotation") {
      if (!value.is_string() ||
          value.get_ref<const std::string &>().size() > 65536)
        throw Error("history_invalid", "Annotation history value is invalid");
      (void)stringField(change, field, {}, 65536);
    } else if (!value.is_null()) {
      if (!value.is_object() || !value.contains("addr"))
        throw Error("history_invalid", "History row value is invalid");
      if (parseAddress(stringField(value, "addr")) !=
          parseAddress(change["address"].get<std::string>()))
        throw Error("history_invalid",
                    "History row address does not match its command");
      validateRow(table, value);
    }
  }
}

void validateCommand(const Json &command) {
  const auto kind = stringField(command, "kind", {}, 32);
  const char *table = ProjectHistory::tableOfKind(kind);
  if (!table)
    throw Error("history_invalid", "Unsupported history command kind");
  validateChange(kind, table, command);
  if (!command.contains("rows"))
    return;
  const auto &rows = command["rows"];
  if (kind == "annotation" || !rows.is_array() || rows.size() > MaxCommandRows)
    throw Error("history_invalid", "History command rows are invalid");
  for (const auto &row : rows) {
    if (!row.is_object())
      throw Error("history_invalid", "History command rows are invalid");
    validateChange(kind, table, row);
  }
}
} // namespace

const char *ProjectHistory::tableOfKind(std::string_view kind) {
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (kind == Kind)                                                            \
    return Table;
#include "UserStateTables.def"
  return nullptr;
}

Json ProjectHistory::normalizedState(Json state) {
  if (!state.is_object())
    throw Error("history_invalid", "User state must be an object");
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional) {Table, Optional},
  static constexpr std::pair<const char *, bool> Tables[] = {
#include "UserStateTables.def"
  };
  for (const auto &[name, optional] : Tables) {
    if (optional && !state.contains(name))
      state[name] = Json::array();
    if (!state.contains(name) || !state[name].is_array())
      throw Error("history_invalid", "User state table is missing");
    auto &items = state[name];
    for (auto &item : items) {
      if (!item.is_object())
        throw Error("history_invalid", "User state item must be an object");
      item["addr"] = hexAddress(parseAddress(stringField(item, "addr")));
      validateRow(name, item);
    }
    // By address, as the CLI orders the tables it writes: one order for
    // both writers keeps a project's files from changing with the tool.
    std::sort(items.begin(), items.end(), [](const Json &a, const Json &b) {
      return parseAddress(a.at("addr").get<std::string>()) <
             parseAddress(b.at("addr").get<std::string>());
    });
    for (std::size_t i = 1; i < items.size(); ++i)
      if (items[i - 1].at("addr") == items[i].at("addr"))
        throw Error("history_invalid",
                    "User state contains duplicate addresses");
  }
  return state;
}

bool ProjectHistory::recoveryPending(const fs::path &binary) {
  return fs::exists(sidecar(binary, project_files::File::Journal));
}

ProjectHistory::ProjectHistory(fs::path binary, std::string sourceHash,
                               std::string engineVersion, bool readOnly,
                               AtomicWriter writer)
    : binary_(std::move(binary)),
      historyPath_(sidecar(binary_, project_files::File::History)),
      journalPath_(sidecar(binary_, project_files::File::Journal)),
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
  Json state = Json::object();
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (auto rows =                                                              \
          read(sidecar(binary_, project_files::File::Id), MaxStateFileBytes);  \
      rows.is_null())                                                          \
    state[Table] = Json::array();                                              \
  else                                                                         \
    state[Table] = std::move(rows);
#include "UserStateTables.def"
  return normalizedState(std::move(state));
}

void ProjectHistory::writeSidecars(const Json &state) const {
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  writer_(sidecar(binary_, project_files::File::Id),                           \
          sidecarText(state.at(Table)));
#include "UserStateTables.def"
}

void ProjectHistory::checkSidecarBudgets(const Json &state) {
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (sidecarText(state.at(Table)).size() > MaxStateFileBytes)                 \
    throw Error("history_budget_exceeded", "A user state file exceeds 16 "     \
                                           "MiB");
#include "UserStateTables.def"
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
         compactText(commands_).size() > MaxHistoryBytes / 2) {
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
  if (compactText(nextDocument).size() > MaxHistoryBytes)
    throw Error("history_budget_exceeded",
                "Persisted user history exceeds 8 MiB");
  const Json journal{{"schema_version", 1},
                     {"source_sha256", hash_},
                     {"before", committedState_},
                     {"after", after},
                     {"before_history", previousDocument_},
                     {"after_history", nextDocument}};
  if (compactText(journal).size() > MaxJournalBytes)
    throw Error("history_budget_exceeded", "Recovery journal exceeds 24 MiB");
  checkSidecarBudgets(after);
  if (const auto error = project_files::prepare(binary_))
    throw Error("save_failed",
                "Cannot create the project folder: " + error.message());
  writer_(journalPath_, compactText(journal));
  // From this point any failure leaves a recoverable write-ahead journal.
  writeSidecars(after);
  writer_(historyPath_, compactText(nextDocument));
  std::error_code ec;
  if (!fs::remove(journalPath_, ec) || ec)
    throw Error(
        "recovery_required",
        "User edits were saved but journal cleanup failed; restart to recover");
  committedState_ = after;
  previousDocument_ = nextDocument;
  if (committed_)
    committed_();
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
    for (const auto &[table, rows] : current.items())
      if (rows != before.at(table) && rows != after.at(table))
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
        compactText(history).size() > MaxHistoryBytes)
      throw Error("history_invalid",
                  "Recovery history exceeds its command or byte budget");
    (void)sizeField(history, "cursor", 0, commands.size());
    for (const auto &command : commands)
      validateCommand(command);
    checkSidecarBudgets(after);
    writeSidecars(after);
    writer_(historyPath_, compactText(history));
    std::error_code ec;
    if (!fs::remove(journalPath_, ec) || ec)
      throw Error("recovery_required", "Cannot clean up recovered journal");
    recovered_ = true;
  } catch (const Json::exception &) {
    throw Error("history_invalid", "Recovery journal is incomplete");
  }
}

bool ProjectHistory::current() const {
  return blocked_.empty() && diskState() == committedState_;
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
