//===- ProjectHistory.cpp - Durable worker edit history ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Admission, immutable state ownership and journaled publication.
///
//===----------------------------------------------------------------------===//

#include "ProjectHistory.h"

#include "CodeEdits.h"
#include "ResourceLimits.h"

#include "neverd/support/ProjectFiles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::worker {
namespace fs = std::filesystem;
using FileContents = std::map<fs::path, std::shared_ptr<const std::string>>;
struct ProjectHistory::Command {
  Json Value;
  // Commands never change after admission. The owning worker thread can
  // memoize their strict encoding without making history queries encode it.
  mutable std::string Text;
  const std::string &encoded() const;
};
struct ProjectHistory::State {
  // The patches member is a placeholder in Other, preserving object key
  // order without keeping a second copy of the patch DOM.
  std::shared_ptr<const Json> Other;
  PatchRows Patches;
  mutable std::optional<Json> Materialized;

  State() = default;
  explicit State(Json normalized);
  State(std::shared_ptr<const Json> other, PatchRows patches);
  const Json &json() const;
  const Json &at(const std::string &name) const;
  std::size_t size() const { return Other->size(); }
  bool tableEquals(const std::string &name, const Json &rows) const;
  bool tableEquals(const std::string &name, const State &other) const;
  bool equals(const Json &other) const;
  bool equals(const State &other) const;
};
struct ProjectHistory::Snapshot {
  State state;
  // Loaded snapshots need this encoding only if a later save uses them as
  // the journal's before state. Committed snapshots already own the encoding.
  std::string stateText;
  std::shared_ptr<const std::string> document;
  FileContents contents;
  // A loaded document retains validated file bytes (possibly with a BOM).
  // Only serializer-produced text can be embedded directly in a journal.
  bool documentIsEncoded = true;
};
namespace {
// A committed snapshot and its serialized bytes must describe the same value.
// JSON dumping otherwise maps non-finite numbers to null and binary values to
// objects. Strings and keys are checked by the strict serializer below.
void validateJsonValues(const Json &value) {
  if (value.is_binary() || value.is_discarded() ||
      (value.is_number_float() && !std::isfinite(value.get<double>())))
    throw Error("history_invalid", "User history contains a non-JSON value");
  if (value.is_structured())
    for (const auto &child : value)
      validateJsonValues(child);
}
std::string compactText(const Json &value) {
  try {
    return value.dump();
  } catch (const Json::exception &) {
    throw Error("history_invalid", "User history contains invalid JSON text");
  }
}
struct StateEncoding {
  std::string Text;
  std::map<std::string, std::pair<std::size_t, std::size_t>> Tables;

  template <typename State>
  StateEncoding(const State &state, std::size_t previousSize) {
    Text.reserve(previousSize);
    Text += '{';
    for (const auto &[name, value] : state.Other->items()) {
      if (Text.size() > 1)
        Text += ',';
      Text += compactText(name);
      Text += ':';
      const auto offset = Text.size();
      if (name == "patches") {
        Text += '[';
        bool first = true;
        for (const auto &row : state.Patches) {
          if (!first)
            Text += ',';
          first = false;
          Text += row->encoded();
        }
        Text += ']';
      } else
        Text += compactText(value);
      Tables.emplace(name, std::pair{offset, Text.size() - offset});
    }
    Text += '}';
  }
  std::string_view table(const std::string &name) const {
    const auto [offset, size] = Tables.at(name);
    return std::string_view(Text).substr(offset, size);
  }
};
fs::path sidecar(const fs::path &binary, project_files::File file) {
  return project_files::path(binary, file);
}
/// Whether \p table's file holds its rows in order, without addresses
/// (UserStateTables.def).
bool ordered(std::string_view table) {
#define NEVERD_USER_STATE_ORDERED(Table)                                       \
  if (table == Table)                                                          \
    return true;
#include "UserStateTables.def"
  return false;
}
/// \p rows, \p table's file, as the history holds them: an ordered table's
/// rows take their place as their address.
Json rowsFromFile(std::string_view table, Json rows) {
  if (!ordered(table) || !rows.is_array())
    return rows;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (!rows[i].is_object() || rows[i].contains("addr"))
      throw Error("history_invalid", "User state item must be an object");
    rows[i]["addr"] = hexAddress(i);
  }
  return rows;
}
/// \p rows as \p table's file holds them.
std::string sidecarContents(std::string_view table, const Json &rows) {
  if (!ordered(table))
    return sidecarText(rows);
  auto fileRows = rows;
  for (auto &row : fileRows)
    row.erase("addr");
  return sidecarText(fileRows);
}
using SidecarWrites =
    std::vector<std::pair<fs::path, std::shared_ptr<const std::string>>>;
std::string patchSidecarText(const ProjectHistory::PatchRows &rows) {
  std::size_t size = std::string_view("[]\n").size();
  for (const auto &row : rows)
    size += row->sidecarRow().size() + std::string_view(",\n").size();
  std::string text;
  text.reserve(size);
  text += '[';
  bool first = true;
  for (const auto &row : rows) {
    text += first ? "\n" : ",\n";
    first = false;
    text += row->sidecarRow();
  }
  if (!rows.empty())
    text += '\n';
  text += "]\n";
  return text;
}
// Encode once before publishing the journal. Validate every table's write
// budget, including unchanged ones, so recovery can still materialize it.
template <typename State>
SidecarWrites prepareSidecars(const fs::path &binary, const State &state,
                              const State *before = nullptr,
                              const StateEncoding *encoded = nullptr) {
  SidecarWrites writes;
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  {                                                                            \
    auto text = encoded && std::string_view(Table) == "patches"                \
                    ? patchSidecarText(state.Patches)                          \
                : encoded && !ordered(Table)                                   \
                    ? formatSidecarText(encoded->table(Table))                 \
                    : sidecarContents(Table, state.at(Table));                 \
    if (text.size() >                                                          \
        ::neverd::limits::worker::kProjectHistoryMaxStateFileBytes)            \
      throw Error("history_budget_exceeded",                                   \
                  "A user state file exceeds 16 MiB");                         \
    if (!before || !state.tableEquals(Table, *before))                         \
      writes.emplace_back(                                                     \
          sidecar(binary, project_files::File::Id),                            \
          std::make_shared<const std::string>(std::move(text)));               \
  }
#include "UserStateTables.def"
  return writes;
}
std::optional<std::string> readText(const fs::path &path, std::size_t limit) {
  std::error_code ec;
  if (!fs::exists(path, ec))
    return std::nullopt;
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
  return text;
}
Json parseFile(const fs::path &path, const std::optional<std::string> &text,
               std::size_t limit) {
  if (!text)
    return nullptr;
  try {
    return parseJson(*text, limit);
  } catch (const Error &) {
    // A merge git could not finish leaves its markers in the file.
    if (text->starts_with("<<<<<<< ") ||
        text->find("\n<<<<<<< ") != std::string::npos)
      throw Error("merge_conflict",
                  path.filename().string() +
                      " has unresolved merge conflicts; resolve them before "
                      "opening the project");
    throw Error("history_invalid", "User state file is not valid bounded JSON");
  }
}
void remember(FileContents &contents, const fs::path &path,
              std::optional<std::string> text) {
  contents[path] =
      text ? std::make_shared<const std::string>(std::move(*text)) : nullptr;
}
bool sameText(const FileContents &contents, const fs::path &path,
              const std::optional<std::string> &text) {
  const auto found = contents.find(path);
  return found != contents.end() && bool(found->second) == bool(text) &&
         (!text || *found->second == *text);
}
Json read(const fs::path &path,
          std::size_t limit =
              ::neverd::limits::worker::kProjectHistoryMaxHistoryBytes,
          FileContents *contents = nullptr) {
  auto text = readText(path, limit);
  auto value = parseFile(path, text, limit);
  if (contents)
    remember(*contents, path, std::move(text));
  return value;
}
/// The fields of a row of \p table besides its address.
void validateRow(std::string_view table, const Json &row) {
  validateJsonValues(row);
  if (table == "code_edits") {
    CodeEdits::validateRow(row);
  } else if (table == "annotations") {
    if (!row.contains("text"))
      throw Error("history_invalid", "Annotation text is missing");
    (void)stringField(
        row, "text", {},
        ::neverd::limits::worker::kProjectHistoryValidateRowTextStringFieldMax);
  } else if (table == "renames") {
    if (!row.contains("original") || !row.contains("renamed"))
      throw Error("history_invalid", "Rename text is missing");
    (void)stringField(row, "original", {},
                      ::neverd::limits::worker::
                          kProjectHistoryValidateRowOriginalStringFieldMax);
    (void)stringField(row, "renamed", {},
                      ::neverd::limits::worker::
                          kProjectHistoryValidateRowRenamedStringFieldMax);
  } else if (table == "functions") {
    // A row edits whether a function starts there, what it is, or both.
    bool edits = false;
    if (row.contains("state")) {
      const auto state =
          stringField(row, "state", {},
                      ::neverd::limits::worker::
                          kProjectHistoryValidateRowStateStringFieldMax);
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
  } else if (table == "types") {
    // A declaration, or the names of the types a removal takes away; the
    // engine reads each again as it loads the types.
    if (row.contains("forget")) {
      const auto &names = row.at("forget");
      if (!names.is_array() || names.empty() ||
          names.size() > ::neverd::limits::worker::
                             kProjectHistoryValidateRowNamesSizeLimit)
        throw Error("history_invalid", "Type removal is invalid");
      for (const auto &name : names)
        if (!name.is_string() || name.get_ref<const std::string &>().empty() ||
            name.get_ref<const std::string &>().size() >
                ::neverd::limits::worker::
                    kProjectHistoryValidateRowNameGetRefConstStdStringSizeLimit)
          throw Error("history_invalid", "Type removal is invalid");
    } else {
      if (!row.contains("language") || !row.contains("source"))
        throw Error("history_invalid", "Type declaration is incomplete");
      (void)stringField(row, "language", {},
                        ::neverd::limits::worker::
                            kProjectHistoryValidateRowLanguageStringFieldMax);
      (void)stringField(row, "source", {},
                        ::neverd::limits::worker::
                            kProjectHistoryValidateRowSourceStringFieldMax);
    }
  } else if (table == "patches") {
    const auto Offset = parseAddress(stringField(row, "file_off"));
    (void)parseAddress(stringField(row, "va"));
    if (Offset != parseAddress(stringField(row, "addr")))
      throw Error("history_invalid",
                  "Patch history key differs from its file offset");
    const auto Size =
        sizeField(row, "size", 0, ::neverd::limits::patch::kMaxPatchedBytes);
    if (!Size)
      throw Error("history_invalid", "Empty patch row");
    for (const char *Key : {"original", "current"}) {
      const auto Hex = stringField(
          row, Key, {}, ::neverd::limits::patch::kMaxPatchedBytes * 2);
      if (Hex.size() != Size * 2 ||
          !std::all_of(Hex.begin(), Hex.end(),
                       [](unsigned char C) { return std::isxdigit(C); }))
        throw Error("history_invalid", "Invalid patch bytes");
    }
    (void)stringField(row, "kind");
  } else if (table == "items") {
    // The engine checks a data item against the image when it loads one.
    (void)stringField(
        row, "kind", {},
        ::neverd::limits::worker::kProjectHistoryValidateRowKindStringFieldMax);
    if (!row.contains("size") || !row.at("size").is_number_unsigned() ||
        row.at("size").get<std::uint64_t>() == 0)
      throw Error("history_invalid", "Data item size is invalid");
    if (row.contains("encoding"))
      (void)stringField(row, "encoding", {},
                        ::neverd::limits::worker::
                            kProjectHistoryValidateRowEncodingStringFieldMax);
  } else {
    // The engine checks each operand's base when it loads the formats.
    if (!row.contains("operands") || !row.at("operands").is_array() ||
        row.at("operands").size() >
            ::neverd::limits::worker::kMaxOperandHistoryValues)
      throw Error("history_invalid", "Operand formats are invalid");
    for (const auto &operand : row.at("operands")) {
      if (!operand.is_object() || !operand.contains("operand") ||
          !operand.at("operand").is_number_unsigned() ||
          operand.at("operand").get<std::uint64_t>() > 7)
        throw Error("history_invalid", "Operand index is invalid");
      (void)stringField(operand, "base", {},
                        ::neverd::limits::worker::
                            kProjectHistoryValidateRowBaseStringFieldMax);
      for (const char *flag : {"negate", "invert"})
        if (operand.contains(flag) && !operand.at(flag).is_boolean())
          throw Error("history_invalid", "Operand format flag is invalid");
      // A structure offset's type and delta.
      if (operand.contains("type"))
        (void)stringField(operand, "type", {},
                          ::neverd::limits::worker::
                              kProjectHistoryValidateRowTypeTypeStringFieldMax);
      if (operand.contains("delta") && !operand.at("delta").is_number_integer())
        throw Error("history_invalid", "Operand structure delta is invalid");
    }
  }
}

// JSON value equality permits 1 == 1.0; admission also depends on field types.
bool sameTypedValue(const Json &left, const Json &right) {
  if (left.type() != right.type())
    return false;
  if (!left.is_structured())
    return left == right;
  if (left.size() != right.size())
    return false;
  auto a = left.begin(), b = right.begin();
  for (; a != left.end(); ++a, ++b)
    if ((left.is_object() && a.key() != b.key()) || !sameTypedValue(*a, *b))
      return false;
  return true;
}
void normalizeRow(std::string_view name, Json &row) {
  if (!row.is_object())
    throw Error("history_invalid", "User state item must be an object");
  row["addr"] = hexAddress(parseAddress(stringField(row, "addr")));
  validateRow(name, row);
}
void normalizeTable(std::string_view name, Json &items,
                    const Json *validated = nullptr) {
  bool sameAddresses = validated && items.size() == validated->size();
  for (std::size_t index = 0; index < items.size(); ++index) {
    auto &item = items[index];
    // Only an identical row from this history's normalized snapshot carries
    // admission forward. Changed or shifted rows take the original validator.
    if (validated && index < validated->size() &&
        sameTypedValue(item, (*validated)[index]))
      continue;
    normalizeRow(name, item);
    if (sameAddresses && item.at("addr") != (*validated)[index].at("addr"))
      sameAddresses = false;
  }
  // Row contents still pass admission above. Identical canonical addresses
  // retain the snapshot's ordering, uniqueness and ordered-table positions.
  if (sameAddresses)
    return;
  // By address, as the CLI orders the tables it writes: one order for
  // both writers keeps a project's files from changing with the tool.
  const auto byAddress = [](const Json &a, const Json &b) {
    return parseAddress(a.at("addr").get_ref<const std::string &>()) <
           parseAddress(b.at("addr").get_ref<const std::string &>());
  };
  if (!std::is_sorted(items.begin(), items.end(), byAddress))
    std::sort(items.begin(), items.end(), byAddress);
  for (std::size_t i = 1; i < items.size(); ++i)
    if (items[i - 1].at("addr") == items[i].at("addr"))
      throw Error("history_invalid", "User state contains duplicate addresses");
  // An ordered table's rows lie at their places in the list, every one.
  if (ordered(name))
    for (std::size_t i = 0; i < items.size(); ++i)
      if (parseAddress(items[i].at("addr").get<std::string>()) != i)
        throw Error("history_invalid", "User state rows in order leave a gap");
}
void validateChange(std::string_view kind, const char *table,
                    const Json &change) {
  parseAddress(stringField(change, "address"));
  if (!change.contains("before") || !change.contains("after"))
    throw Error("history_invalid", "History command is incomplete");
  for (const auto *field : {"before", "after"}) {
    const auto &value = change[field];
    if (kind == "annotation") {
      if (!value.is_string() ||
          value.get_ref<const std::string &>().size() >
              ::neverd::limits::worker::
                  kProjectHistoryValidateChangeAnnotationValueGetRefConstStdStringSizeLimit)
        throw Error("history_invalid", "Annotation history value is invalid");
      (void)stringField(
          change, field, {},
          ::neverd::limits::worker::
              kProjectHistoryValidateChangeAnnotationFieldStringFieldMax);
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
  validateJsonValues(command);
  const auto kind =
      stringField(command, "kind", {},
                  ::neverd::limits::worker::
                      kProjectHistoryValidateCommandKindKindStringFieldMax);
  const char *table = ProjectHistory::tableOfKind(kind);
  if (!table)
    throw Error("history_invalid", "Unsupported history command kind");
  validateChange(kind, table, command);
  if (!command.contains("rows"))
    return;
  const auto &rows = command["rows"];
  if (kind == "annotation" || !rows.is_array() ||
      rows.size() > ::neverd::limits::worker::kProjectHistoryMaxCommandRows)
    throw Error("history_invalid", "History command rows are invalid");
  for (const auto &row : rows) {
    if (!row.is_object())
      throw Error("history_invalid", "History command rows are invalid");
    validateChange(kind, table, row);
  }
}
Json normalizeState(Json state, const Json *validated = nullptr) {
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
    normalizeTable(name, items, validated ? &validated->at(name) : nullptr);
  }
  for (const auto &[name, value] : state.items())
    if (std::none_of(std::begin(Tables), std::end(Tables),
                     [&](const auto &table) { return name == table.first; }))
      validateJsonValues(value);
  return state;
}

Json readStateFiles(const fs::path &binary, FileContents *contents) {
  Json state = Json::object();
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (auto rows =                                                              \
          read(sidecar(binary, project_files::File::Id),                       \
               ::neverd::limits::worker::kProjectHistoryMaxStateFileBytes,     \
               contents);                                                      \
      rows.is_null())                                                          \
    state[Table] = Json::array();                                              \
  else                                                                         \
    state[Table] = rowsFromFile(Table, std::move(rows));
#include "UserStateTables.def"
  return ProjectHistory::normalizedState(std::move(state));
}

template <typename State>
bool stateMatches(const fs::path &binary, const State &expected,
                  const FileContents &contents,
                  FileContents *observed = nullptr) {
  Json changed = Json::object();
  std::size_t tables = 0;
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  {                                                                            \
    ++tables;                                                                  \
    const auto path = sidecar(binary, project_files::File::Id);                \
    constexpr auto limit =                                                     \
        ::neverd::limits::worker::kProjectHistoryMaxStateFileBytes;            \
    auto text = readText(path, limit);                                         \
    if (!sameText(contents, path, text)) {                                     \
      auto rows = parseFile(path, text, limit);                                \
      changed[Table] = rows.is_null() ? Json::array()                          \
                                      : rowsFromFile(Table, std::move(rows));  \
      if (observed)                                                            \
        remember(*observed, path, std::move(text));                            \
    }                                                                          \
  }
#include "UserStateTables.def"
  bool matches = true;
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (const auto found = changed.find(Table); found != changed.end()) {        \
    if (!found->is_array())                                                    \
      throw Error("history_invalid", "User state table is missing");           \
    normalizeTable(Table, *found);                                             \
    matches &= expected.tableEquals(Table, *found);                            \
  }
#include "UserStateTables.def"
  return matches && expected.size() == tables;
}
bool historyMatches(const fs::path &path, std::string_view expected,
                    const FileContents &contents) {
  constexpr auto limit =
      ::neverd::limits::worker::kProjectHistoryMaxHistoryBytes;
  const auto text = readText(path, limit);
  return sameText(contents, path, text) ||
         parseFile(path, text, limit) == transport::parseBackendJson(expected);
}
} // namespace

ProjectHistory::PatchRow::PatchRow(Json normalized)
    : value_(std::move(normalized)),
      address_(parseAddress(value_.at("addr").get_ref<const std::string &>())) {
}

const std::string &ProjectHistory::PatchRow::encoded() const {
  if (text_.empty())
    text_ = compactText(value_);
  return text_;
}

std::string_view ProjectHistory::PatchRow::sidecarRow() const {
  if (sidecarText_.empty()) {
    // Format a one-row array with the existing layout authority, then expose
    // only the row. This retains nested indentation, scalar spelling and
    // LLVM-compatible control escapes without a second sidecar formatter.
    std::string array;
    array.reserve(encoded().size() + std::string_view("[]").size());
    array += '[';
    array += encoded();
    array += ']';
    sidecarText_ = formatSidecarText(array);
  }
  constexpr std::string_view Prefix = "[\n", Suffix = "\n]\n";
  return std::string_view(sidecarText_)
      .substr(Prefix.size(),
              sidecarText_.size() - Prefix.size() - Suffix.size());
}

std::shared_ptr<const ProjectHistory::PatchRow>
ProjectHistory::patchRow(Json row) {
  normalizeRow("patches", row);
  return std::shared_ptr<const PatchRow>(new PatchRow(std::move(row)));
}

ProjectHistory::State::State(Json normalized) {
  auto &rows = normalized.at("patches");
  Patches.reserve(rows.size());
  // normalizeState already admitted these values. Do not intern logically
  // equal rows here: e.g. unknown fields -0.0 and 0.0 encode differently.
  for (auto &row : rows)
    Patches.emplace_back(new PatchRow(std::move(row)));
  rows = nullptr;
  Other = std::make_shared<const Json>(std::move(normalized));
}

ProjectHistory::State::State(std::shared_ptr<const Json> other,
                             PatchRows patches)
    : Other(std::move(other)), Patches(std::move(patches)) {
  for (const auto &row : Patches)
    if (!row)
      throw Error("history_invalid", "Patch history row is missing");
  const auto less = [](const auto &a, const auto &b) {
    return a->address() < b->address();
  };
  if (!std::is_sorted(Patches.begin(), Patches.end(), less))
    std::sort(Patches.begin(), Patches.end(), less);
  for (std::size_t i = 1; i < Patches.size(); ++i)
    if (Patches[i - 1]->address() == Patches[i]->address())
      throw Error("history_invalid", "User state contains duplicate addresses");
}

const Json &ProjectHistory::State::json() const {
  if (!Materialized) {
    auto state = *Other;
    auto &rows = state["patches"] = Json::array();
    rows.get_ref<Json::array_t &>().reserve(Patches.size());
    for (const auto &row : Patches)
      rows.push_back(row->value());
    Materialized.emplace(std::move(state));
  }
  return *Materialized;
}

const Json &ProjectHistory::State::at(const std::string &name) const {
  return name == "patches" ? json().at(name) : Other->at(name);
}

bool ProjectHistory::State::tableEquals(const std::string &name,
                                        const Json &rows) const {
  if (name != "patches")
    return Other->at(name) == rows;
  return rows.is_array() && rows.size() == Patches.size() &&
         std::equal(
             Patches.begin(), Patches.end(), rows.begin(),
             [](const auto &a, const auto &b) { return a->value() == b; });
}

bool ProjectHistory::State::tableEquals(const std::string &name,
                                        const State &other) const {
  if (name != "patches")
    return Other == other.Other || Other->at(name) == other.Other->at(name);
  return Patches.size() == other.Patches.size() &&
         std::equal(Patches.begin(), Patches.end(), other.Patches.begin(),
                    [](const auto &a, const auto &b) {
                      return a == b || a->value() == b->value();
                    });
}

bool ProjectHistory::State::equals(const Json &other) const {
  if (!other.is_object() || other.size() != size())
    return false;
  for (const auto &[name, rows] : other.items())
    if (!Other->contains(name) || !tableEquals(name, rows))
      return false;
  return true;
}

bool ProjectHistory::State::equals(const State &other) const {
  return (Other == other.Other || *Other == *other.Other) &&
         tableEquals("patches", other);
}

const char *ProjectHistory::tableOfKind(std::string_view kind) {
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (kind == Kind)                                                            \
    return Table;
#include "UserStateTables.def"
  return nullptr;
}

Json ProjectHistory::normalizedState(Json state) {
  return normalizeState(std::move(state));
}

bool ProjectHistory::recoveryPending(const fs::path &binary) {
  return fs::exists(sidecar(binary, project_files::File::Journal));
}

const std::string &ProjectHistory::Command::encoded() const {
  if (Text.empty())
    Text = compactText(Value);
  return Text;
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
  Json previousDocument;
  snapshot_ = readSnapshot(&previousDocument);
  if (previousDocument.is_null())
    return;
  try {
    if (previousDocument.value("schema_version", 0) != 1)
      throw Error("history_invalid", "Unsupported history schema");
    if (previousDocument.value("source_sha256", "") != hash_) {
      blocked_ = "input_changed";
      return;
    }
    // A table the history predates took no edit it recorded: it holds
    // what the file holds.
    Json previous = previousDocument.at("state");
#define NEVERD_USER_STATE_TABLE(Table, Kind, Id, Optional)                     \
  if (Optional && previous.is_object() && !previous.contains(Table))           \
    previous[Table] = snapshot_->state.at(Table);
#include "UserStateTables.def"
    if (!snapshot_->state.equals(normalizedState(std::move(previous)))) {
      blocked_ = "foreign_edits";
      return;
    }
    auto &commands = previousDocument.at("commands");
    if (!commands.is_array() ||
        commands.size() > ::neverd::limits::worker::
                              kProjectHistoryProjectHistoryCommandsSizeLimit)
      throw Error("history_invalid", "History command count is invalid");
    cursor_ = sizeField(previousDocument, "cursor", 0, commands.size());
    commands_.reserve(commands.size());
    for (auto &command : commands) {
      validateCommand(command);
      commands_.push_back(
          std::make_shared<const Command>(Command{std::move(command), {}}));
    }
  } catch (const Json::exception &) {
    throw Error("history_invalid", "History document is incomplete");
  }
}

Json ProjectHistory::diskState() const { return readState(binary_); }

Json ProjectHistory::readState(const fs::path &binary) {
  return readStateFiles(binary, nullptr);
}
std::shared_ptr<const ProjectHistory::Snapshot>
ProjectHistory::readSnapshot(Json *document) const {
  auto snapshot = std::make_shared<Snapshot>();
  snapshot->state = State(readStateFiles(binary_, &snapshot->contents));
  auto previous = read(historyPath_,
                       ::neverd::limits::worker::kProjectHistoryMaxHistoryBytes,
                       &snapshot->contents);
  const auto &raw = snapshot->contents.at(historyPath_);
  snapshot->document = raw ? raw : std::make_shared<const std::string>("null");
  snapshot->documentIsEncoded = !raw;
  if (document)
    *document = std::move(previous);
  return snapshot;
}
const Json &ProjectHistory::committedState() const {
  return snapshot_->state.json();
}
const Json &ProjectHistory::committedTable(const std::string &name) const {
  return snapshot_->state.at(name);
}
const ProjectHistory::PatchRows &ProjectHistory::committedPatches() const {
  return snapshot_->state.Patches;
}

void ProjectHistory::verifyLoadedState(const Json &state) {
  auto loaded = normalizedState(state);
  // The C ABI reconstructs original symbol names independently; only current
  // user rename values are part of the loaded edit-state identity here.
  for (auto &row : loaded["renames"])
    row.erase("original");
  auto renames = snapshot_->state.at("renames");
  for (auto &row : renames)
    row.erase("original");
  bool matches = loaded.size() == snapshot_->state.size();
  for (const auto &[name, rows] : loaded.items())
    matches &= snapshot_->state.Other->contains(name) &&
               (name == "renames" ? rows == renames
                                  : snapshot_->state.tableEquals(name, rows));
  if (!matches && blocked_.empty())
    blocked_ = "foreign_edits";
}

void ProjectHistory::requireUsable() const {
  if (!blocked_.empty())
    throw Error(blocked_, "History does not match the current input or "
                          "sidecars; explicitly reset history before editing");
}

std::uint64_t ProjectHistory::displayAddress(const Json &command) {
  if (command.value("kind", "") == "patch")
    for (const char *side : {"after", "before"})
      if (const auto &row = command.at(side); row.is_object())
        return parseAddress(row.at("va").get<std::string>());
  return parseAddress(command.at("address").get<std::string>());
}

Json ProjectHistory::listing(std::size_t offset, std::size_t limit) const {
  Json items = Json::array();
  for (std::size_t i = std::min(offset, commands_.size());
       i < commands_.size() && items.size() < limit; ++i) {
    auto command = commands_[i]->Value;
    command["address"] = hexAddress(displayAddress(command));
    // The durable command retains every byte. A chooser page must not echo
    // megabytes of old/new data per row; the patches query owns byte pages.
    if (command.value("kind", "") == "patch") {
      command["details_omitted"] = true;
      command["changed_rows"] = 1 + command.value("rows", Json::array()).size();
      command.erase("rows");
      for (const auto *side : {"before", "after"})
        if (command.contains(side) && command[side].is_object()) {
          command[side].erase("original");
          command[side].erase("current");
        }
    }
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
  // Refuse invalid string/key encoding before mutating the undo cursor.
  auto text = compactText(command);
  auto next = std::make_shared<const Command>(
      Command{std::move(command), std::move(text)});
  commands_.erase(
      commands_.begin() +
          static_cast<decltype(commands_)::difference_type>(cursor_),
      commands_.end());
  commands_.push_back(std::move(next));
  ++cursor_;
  auto bytes = commandTextSize();
  while (commands_.size() >
             ::neverd::limits::worker::kProjectHistoryStageCommandsSizeLimit ||
         bytes > ::neverd::limits::worker::kProjectHistoryMaxHistoryBytes / 2) {
    bytes -=
        commands_.front()->encoded().size() + (commands_.size() > 1 ? 1 : 0);
    commands_.erase(commands_.begin());
    --cursor_;
  }
}

Json ProjectHistory::next(bool redo) const {
  requireUsable();
  if (redo ? cursor_ == commands_.size() : cursor_ == 0)
    throw Error("history_empty", redo ? "Nothing to redo" : "Nothing to undo");
  return commands_[redo ? cursor_ : cursor_ - 1]->Value;
}
void ProjectHistory::advance(bool redo) {
  (void)next(redo);
  if (redo)
    ++cursor_;
  else
    --cursor_;
}

std::size_t ProjectHistory::commandTextSize() const {
  std::size_t bytes = 2;
  for (const auto &command : commands_)
    bytes += command->encoded().size();
  if (!commands_.empty())
    bytes += commands_.size() - 1;
  return bytes;
}

std::string ProjectHistory::document(std::string_view encodedState) const {
  const Json header{{"schema_version", 1},
                    {"source_sha256", hash_},
                    {"engine_version", engineVersion_},
                    {"cursor", cursor_}};
  auto headerText = compactText(header);
  headerText.pop_back();
  // Commands sort first in the original object serializer. Keep the same
  // bytes while sharing their immutable values across rollback checkpoints.
  headerText.front() = ',';
  std::string text = "{\"commands\":";
  constexpr std::string_view StateMember = ",\"state\":";
  text.reserve(text.size() + commandTextSize() + headerText.size() +
               StateMember.size() + encodedState.size() + 1);
  text += '[';
  for (std::size_t i = 0; i < commands_.size(); ++i) {
    if (i)
      text += ',';
    text += commands_[i]->encoded();
  }
  text += ']';
  text += headerText;
  text += StateMember;
  text += encodedState;
  text += '}';
  return text;
}

void ProjectHistory::persist(Json state) {
  persist(std::move(state), std::nullopt);
}

void ProjectHistory::persistPatches(PatchRows rows) {
  persist(nullptr, std::move(rows));
}

void ProjectHistory::persist(Json state, std::optional<PatchRows> patches) {
  requireUsable();
  if (readOnly_)
    throw Error("read_only", "History persistence requires the writer lock");
  if (fs::file_size(binary_) != fileSize_ ||
      fs::last_write_time(binary_) != fileTime_)
    throw Error(
        "input_changed",
        "Input changed while the Session was open; reopen it before editing");
  // Preserve the caller's starting tables even if this call must first
  // finish an interrupted transaction that changed the committed snapshot.
  const auto other = snapshot_->state.Other;
  std::optional<State> prepared;
  if (!interruptedWrite_.is_null()) {
    if (!recoveryPending(binary_) ||
        read(journalPath_,
             ::neverd::limits::worker::kProjectHistoryMaxJournalBytes) !=
            interruptedWrite_)
      throw Error(
          "recovery_required",
          "The interrupted transaction changed; reopen to inspect recovery");
    // Complete only our own failed write. recover() verifies every sidecar
    // and history against the journal before replacing anything. Newer staged
    // annotations and commands stay in memory and are saved below afterwards.
    recover();
    auto recovered = std::make_shared<Snapshot>();
    recovered->state = State(normalizedState(interruptedWrite_.at("after")));
    recovered->stateText = StateEncoding(recovered->state, 0).Text;
    recovered->document = std::make_shared<const std::string>(
        compactText(interruptedWrite_.at("after_history")));
    snapshot_ = std::move(recovered);
    interruptedWrite_ = nullptr;
    prepared = patches ? State(other, std::move(*patches))
                       : State(normalizedState(std::move(state)));
    if (prepared->equals(snapshot_->state) &&
        transport::parseBackendJson(document(snapshot_->stateText)) ==
            transport::parseBackendJson(*snapshot_->document)) {
      if (committed_)
        committed_();
      return;
    }
  }
  if (recoveryPending(binary_))
    throw Error("recovery_required",
                "An interrupted edit must be recovered before another save");
  auto contents = snapshot_->contents;
  // Compare actual bounded file contents, never timestamps or file sizes.
  // Equal bytes reuse the validated snapshot; changed bytes retain the full
  // parser/normalizer, including acceptance of equivalent formatting.
  if (!stateMatches(binary_, snapshot_->state, snapshot_->contents,
                    &contents) ||
      !historyMatches(historyPath_, *snapshot_->document, snapshot_->contents))
    throw Error("foreign_edits", "Sidecars or history changed outside this "
                                 "worker; reload and inspect before editing");
  auto after =
      prepared ? std::move(*prepared)
      : patches
          ? State(other, std::move(*patches))
          : State(normalizeState(std::move(state), &snapshot_->state.json()));
  // Preserve the compact state spelling and retain each table's byte range.
  // Sidecars can add layout without walking and encoding the same rows again.
  auto encoded = StateEncoding(after, snapshot_->stateText.size());
  auto &afterText = encoded.Text;
  const auto historyText =
      std::make_shared<const std::string>(document(afterText));
  if (historyText->size() >
      ::neverd::limits::worker::kProjectHistoryMaxHistoryBytes)
    throw Error("history_budget_exceeded",
                "Persisted user history exceeds 8 MiB");
  Json journal{{"schema_version", 1}, {"source_sha256", hash_}};
  // Compose only serializer-produced JSON. The history is already encoded;
  // reuse it without copying whole states into a second tree. Keep the owning
  // states until publication succeeds, or retain the full journal on failure.
  auto journalText = compactText(journal);
  journalText.pop_back();
  {
    std::string beforeStateText, beforeHistoryText;
    std::string_view beforeState = snapshot_->stateText;
    if (beforeState.empty()) {
      beforeStateText = StateEncoding(snapshot_->state, 0).Text;
      beforeState = beforeStateText;
    }
    std::string_view beforeHistory = *snapshot_->document;
    if (!snapshot_->documentIsEncoded) {
      beforeHistoryText =
          compactText(transport::parseBackendJson(*snapshot_->document));
      beforeHistory = beforeHistoryText;
    }
    const std::string_view parts[] = {
        ",\"before\":",        beforeState,   ",\"before_history\":",
        beforeHistory,         ",\"after\":", afterText,
        ",\"after_history\":", *historyText,  "}"};
    auto journalSize = journalText.size();
    for (const auto part : parts)
      journalSize += part.size();
    if (journalSize > ::neverd::limits::worker::kProjectHistoryMaxJournalBytes)
      throw Error("history_budget_exceeded", "Recovery journal exceeds 24 MiB");
    journalText.reserve(journalSize);
    for (const auto part : parts)
      journalText += part;
  }
  const auto writes =
      prepareSidecars(binary_, after, &snapshot_->state, &encoded);
  if (const auto error = project_files::prepare(binary_))
    throw Error("save_failed",
                "Cannot create the project folder: " + error.message());
  for (const auto &[path, text] : writes)
    contents[path] = text;
  contents[historyPath_] = historyText;
  // Allocate the next snapshot before publishing any transaction file. The
  // final in-memory commit is just a noexcept shared-pointer move.
  auto next = std::make_shared<Snapshot>(
      Snapshot{std::move(after), std::move(afterText), historyText,
               std::move(contents)});
  writer_(journalPath_, journalText);
  // From this point any failure leaves a recoverable write-ahead journal.
  try {
    for (const auto &[path, text] : writes)
      writer_(path, *text);
    writer_(historyPath_, *historyText);
    std::error_code ec;
    if (!fs::remove(journalPath_, ec) || ec)
      throw Error("recovery_required", "User edits were saved but journal "
                                       "cleanup failed; retry Save to recover");
  } catch (...) {
    journal["before"] = snapshot_->state.json();
    journal["before_history"] =
        transport::parseBackendJson(*snapshot_->document);
    journal["after"] = next->state.json();
    journal["after_history"] = transport::parseBackendJson(*next->document);
    interruptedWrite_ = std::move(journal);
    throw;
  }
  snapshot_ = std::move(next);
  if (committed_)
    committed_();
}

void ProjectHistory::recover() {
  if (readOnly_)
    throw Error("recovery_required", "A pending user-edit transaction requires "
                                     "a writable worker to recover");
  const auto journal = read(
      journalPath_, ::neverd::limits::worker::kProjectHistoryMaxJournalBytes);
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
    if (!commands.is_array() ||
        commands.size() >
            ::neverd::limits::worker::kProjectHistoryRecoverCommandsSizeLimit ||
        compactText(history).size() >
            ::neverd::limits::worker::kProjectHistoryMaxHistoryBytes)
      throw Error("history_invalid",
                  "Recovery history exceeds its command or byte budget");
    (void)sizeField(history, "cursor", 0, commands.size());
    for (const auto &command : commands)
      validateCommand(command);
    const auto writes = prepareSidecars(binary_, State(after));
    for (const auto &[path, text] : writes)
      writer_(path, *text);
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
  return blocked_.empty() &&
         stateMatches(binary_, snapshot_->state, snapshot_->contents);
}

void ProjectHistory::reset() {
  if (readOnly_)
    throw Error("read_only", "History reset requires the writer lock");
  if (recoveryPending(binary_))
    throw Error("recovery_required",
                "Recover the pending transaction before resetting history");
  const auto checkpoint = *this;
  try {
    snapshot_ = readSnapshot();
    commands_.clear();
    cursor_ = 0;
    blocked_.clear();
    persist(snapshot_->state.json());
  } catch (...) {
    *this = checkpoint;
    throw;
  }
}
} // namespace neverd::worker
