//===- history_tests.cpp - Durable history publication -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Sidecar preservation and recovery after interrupted transaction writes.
///
//===----------------------------------------------------------------------===//

#include "ProjectHistory.h"
#include "ResourceLimits.h"

#include "neverd/support/ProjectFiles.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace neverd::worker;
namespace fs = std::filesystem;
namespace files = neverd::project_files;

namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
std::string text(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  check(bool(input), "Cannot read test sidecar");
  return {std::istreambuf_iterator<char>(input), {}};
}
void writeFile(const fs::path &path, const std::string &value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << value;
  output.close();
  check(bool(output), "Cannot write test sidecar");
}
struct Writer {
  std::vector<fs::path> calls;
  std::size_t failAt = std::numeric_limits<std::size_t>::max();
  bool failAfter = false;
  static inline Writer *active = nullptr;
  static void write(const fs::path &path, const std::string &value) {
    auto &self = *active;
    const bool fail = self.calls.size() == self.failAt;
    self.calls.push_back(path);
    if (fail && !self.failAfter)
      throw Error("injected_failure", "Before complete file replacement");
    writeFile(path, value);
    if (fail)
      throw Error("injected_failure", "After complete file replacement");
  }
  void clear() {
    calls.clear();
    failAt = std::numeric_limits<std::size_t>::max();
  }
};
struct Fixture {
  fs::path directory, binary;
  Writer writer;
  Fixture() {
    static unsigned sequence = 0;
    directory =
        fs::temp_directory_path() /
        ("neverd-history-unit-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(sequence++));
    binary = directory / "input.bin";
    writeFile(binary, "owned history fixture");
    Writer::active = &writer;
  }
  ~Fixture() {
    std::error_code ec;
    fs::remove_all(directory, ec);
    Writer::active = nullptr;
  }
  fs::path path(files::File id) const { return files::path(binary, id); }
  ProjectHistory open() const {
    return {binary, std::string(64, '0'), "fixture", false, Writer::write};
  }
};
Json annotation(const char *value) {
  return {{"addr", "0x1000"}, {"text", value}};
}
Json rename(const char *value) {
  return {{"addr", "0x1000"}, {"original", "function"}, {"renamed", value}};
}
Json initial(ProjectHistory &store) {
  auto state = store.committedState();
  state["annotations"] = Json::array({annotation("before")});
  state["renames"] = Json::array({rename("before")});
  store.persist(state);
  return state;
}
Json change(ProjectHistory &store, const char *value) {
  auto state = store.committedState();
  const auto beforeNote = state.at("annotations")[0].at("text");
  const auto beforeName = state.at("renames")[0];
  state["annotations"] = Json::array({annotation(value)});
  state["renames"] = Json::array({rename(value)});
  store.stage({{"kind", "annotation"},
               {"address", "0x1000"},
               {"before", beforeNote},
               {"after", value}});
  store.stage({{"kind", "rename"},
               {"address", "0x1000"},
               {"before", beforeName},
               {"after", state["renames"][0]}});
  return state;
}
template <typename F> void rejects(F &&action, const char *code) {
  try {
    action();
  } catch (const Error &error) {
    check(error.code == code, "Wrong transaction refusal");
    return;
  }
  check(false, "Invalid transaction accepted");
}
void unchangedFiles() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = initial(store);
  const auto annotations = fixture.path(files::File::Annotations);
  const auto original = text(annotations);
  const auto timestamp = fs::last_write_time(annotations);
  fixture.writer.clear();
  state["renames"][0]["renamed"] = "after";
  store.persist(state);
  check(text(annotations) == original &&
            fs::last_write_time(annotations) == timestamp,
        "Unchanged sidecar was replaced");
  check(fixture.writer.calls ==
            std::vector<fs::path>{fixture.path(files::File::Journal),
                                  fixture.path(files::File::Renames),
                                  fixture.path(files::File::History)},
        "Commit wrote files outside its changed table and journal/history");
  check(text(fixture.path(files::File::Renames)) ==
            sidecarText(state["renames"]),
        "Changed sidecar lost canonical formatting");
  state["renames"] = Json::array();
  store.persist(state);
  check(Json::parse(text(fixture.path(files::File::Renames))).empty(),
        "Removing the final row did not persist an empty table");
  auto reopened = fixture.open();
  check(reopened.committedState() == state && reopened.current(),
        "Reopening changed the committed tables");

  // A foreign edit to an otherwise unchanged table must still block saving.
  writeFile(annotations, sidecarText(Json::array({annotation("foreign")})));
  fixture.writer.clear();
  rejects([&] { store.persist(state); }, "foreign_edits");
  check(fixture.writer.calls.empty() &&
            !ProjectHistory::recoveryPending(fixture.binary),
        "Conflict published transaction files");
}
void failureBoundaries() {
  // Two changed tables, plus the write-ahead journal and history document.
  constexpr std::size_t Writes = 4;
  for (std::size_t fail = 0; fail < Writes; ++fail)
    for (bool afterWrite : {false, true}) {
      Fixture fixture;
      auto store = fixture.open();
      const auto before = initial(store);
      const auto after = change(store, "after");
      fixture.writer.clear();
      fixture.writer.failAt = fail;
      fixture.writer.failAfter = afterWrite;
      rejects([&] { store.persist(after); }, "injected_failure");
      fixture.writer.clear();
      const bool published = fail != 0 || afterWrite;
      check(ProjectHistory::recoveryPending(fixture.binary) == published,
            "Write failure lost its recovery boundary");
      auto reopened = fixture.open();
      check(reopened.committedState() == (published ? after : before),
            "Recovery produced a mixed transaction state");
      check(!ProjectHistory::recoveryPending(fixture.binary) &&
                reopened.current(),
            "Recovered state still needs repair");
    }
}
void retryAndConflict() {
  for (bool foreignHistory : {false, true}) {
    Fixture fixture;
    auto store = fixture.open();
    initial(store);
    const auto after = change(store, "after");
    fixture.writer.clear();
    fixture.writer.failAt = 2;
    rejects([&] { store.persist(after); }, "injected_failure");
    fixture.writer.clear();
    const auto path = fixture.path(foreignHistory ? files::File::History
                                                  : files::File::Renames);
    const auto saved = text(path);
    writeFile(path, foreignHistory
                        ? "{}"
                        : sidecarText(Json::array({rename("foreign")})));
    const auto foreign = text(path);
    rejects([&] { fixture.open(); }, "foreign_edits");
    rejects([&] { store.persist(after); }, "foreign_edits");
    check(text(path) == foreign && fixture.writer.calls.empty(),
          "Recovery overwrote a foreign table or history");
    writeFile(path, saved);
    auto newer = after;
    newer["annotations"][0]["text"] = "newer";
    store.stage({{"kind", "annotation"},
                 {"address", "0x1000"},
                 {"before", "after"},
                 {"after", "newer"}});
    store.persist(newer);
    auto reopened = fixture.open();
    check(reopened.committedState() == newer &&
              !ProjectHistory::recoveryPending(fixture.binary),
          "Retry discarded newer staged state");
    check(reopened.next(false).at("after") == "newer",
          "Retry discarded newer history");
  }
}
void journalContents() {
  for (bool firstCommit : {true, false}) {
    Fixture fixture;
    auto store = fixture.open();
    if (!firstCommit)
      initial(store);
    const auto before = store.committedState();
    const auto historyPath = fixture.path(files::File::History);
    const auto beforeHistory =
        firstCommit ? Json() : Json::parse(text(historyPath));
    auto after = before;
    // These bytes must remain JSON string data when encoded subdocuments are
    // composed, including text that looks like a journal member boundary.
    const std::string note =
        "quote\" slash\\ tab\t backspace\b formfeed\f newline\n"
        "\xe4\xb8\xad\xe6\x96\x87 },\"after\":null";
    after["annotations"] = Json::array({{{"addr", "0x1000"}, {"text", note}}});
    const Json command{{"kind", "annotation"},
                       {"address", "0x1000"},
                       {"before", firstCommit ? "" : "before"},
                       {"after", note}};
    store.stage(command);
    fixture.writer.clear();
    fixture.writer.failAt = 1;
    rejects([&] { store.persist(after); }, "injected_failure");
    const auto journal = Json::parse(text(fixture.path(files::File::Journal)));
    const Json history{{"schema_version", 1},
                       {"source_sha256", std::string(64, '0')},
                       {"engine_version", "fixture"},
                       {"cursor", 1},
                       {"commands", Json::array({command})},
                       {"state", after}};
    const Json expected{
        {"schema_version", 1}, {"source_sha256", std::string(64, '0')},
        {"before", before},    {"before_history", beforeHistory},
        {"after", after},      {"after_history", history}};
    check(journal == expected, "Recovery journal changed its transaction");
    fixture.writer.clear();
    store.persist(after);
    check(Json::parse(text(historyPath)) == history &&
              !ProjectHistory::recoveryPending(fixture.binary),
          "Retry disagrees with the published journal");
    auto reopened = fixture.open();
    check(reopened.committedState() == after && reopened.next(false) == command,
          "Reopening lost escaped state or history");
  }
}
void loadedDocumentJournal() {
  for (bool bom : {false, true}) {
    Fixture fixture;
    auto original = fixture.open();
    const auto before = initial(original);
    const auto historyPath = fixture.path(files::File::History);
    const auto beforeDocument = Json::parse(text(historyPath));
    writeFile(historyPath,
              (bom ? "\xef\xbb\xbf" : "") + beforeDocument.dump(4) + "\n");
    auto loaded = fixture.open();
    const auto after = change(loaded, "after reopening");
    fixture.writer.clear();
    fixture.writer.failAt = 1;
    rejects([&] { loaded.persist(after); }, "injected_failure");
    // Valid top-level file bytes (notably a BOM) are not necessarily valid
    // nested JSON. Lazy encoding must publish a complete, parseable journal.
    const auto journal = parseJson(text(fixture.path(files::File::Journal)));
    check(journal.at("before_history") == beforeDocument &&
              journal.at("before") == before && journal.at("after") == after,
          "Loaded history text changed or corrupted the recovery journal");
    fixture.writer.clear();
    loaded.persist(after);
    check(!ProjectHistory::recoveryPending(fixture.binary) &&
              fixture.open().committedState() == after,
          "Retry lost the state of a reformatted loaded history");
  }
}
void independentCheckpoints() {
  Fixture fixture;
  auto store = fixture.open();
  const auto before = initial(store);
  auto checkpoint = store;
  const auto &retained = checkpoint.committedState();
  const auto after = change(store, "after");
  rejects([&] { checkpoint.next(false); }, "history_empty");
  store.persist(after);
  check(retained == before && checkpoint.committedState() == before &&
            !checkpoint.current(),
        "Commit mutated an independent rollback checkpoint");
  fixture.writer.clear();
  rejects([&] { checkpoint.persist(before); }, "foreign_edits");
  check(fixture.writer.calls.empty(), "Stale checkpoint overwrote newer files");
  checkpoint.reset();
  check(checkpoint.committedState() == after && checkpoint.current() &&
            store.next(false).at("after") == after["renames"][0],
        "Reset shared mutable state or commands with another history");
}
void equivalentAndForeignContents() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = initial(store);
  state["annotations"].push_back({{"addr", "0x20"}, {"text", "second"}});
  store.persist(state);
  state = store.committedState();
  const auto notes = fixture.path(files::File::Annotations);
  const auto history = fixture.path(files::File::History);
  const std::string formatted =
      R"([{"text":"before","addr":"0X01000"},
           {"addr":"0x0020","text":"second"}])";
  writeFile(notes, formatted);
  writeFile(history, Json::parse(text(history)).dump(4));
  check(store.current(), "Equivalent row order/address spelling was refused");
  state["renames"][0]["renamed"] = "after";
  store.persist(state);
  check(text(notes) == formatted && store.current(),
        "Equivalent unchanged file was rewritten or lost its identity");
  // File size and timestamp alone do not establish content identity.
  const auto timestamp = fs::last_write_time(notes);
  auto foreign = formatted;
  foreign.replace(foreign.find("before"), 6, "forged");
  writeFile(notes, foreign);
  fs::last_write_time(notes, timestamp);
  check(!store.current(), "Same-size timestamp-preserving edit was missed");
  fixture.writer.clear();
  rejects([&] { store.persist(state); }, "foreign_edits");
  check(fixture.writer.calls.empty() && text(notes) == foreign,
        "Foreign file was replaced through a validated snapshot");
  writeFile(notes, formatted);
  const auto savedHistory = text(history);
  const auto historyTime = fs::last_write_time(history);
  auto changedHistory = savedHistory;
  const auto schema = changedHistory.find("\"schema_version\":1");
  check(schema != std::string::npos, "History schema fixture changed");
  changedHistory[schema + std::string("\"schema_version\":").size()] = '2';
  writeFile(history, changedHistory);
  fs::last_write_time(history, historyTime);
  rejects([&] { store.persist(state); }, "foreign_edits");
  check(fixture.writer.calls.empty(), "Changed history bypassed comparison");
  writeFile(history, savedHistory);
  for (
      const auto *malformed :
      {R"([{"addr":"0x1000","addr":"0x1000","text":"before"}])",
       R"([{"addr":"0x1000","text":"before"},{"addr":"0X01000","text":"before"}])",
       "{broken"}) {
    writeFile(notes, malformed);
    rejects([&] { store.persist(state); }, "history_invalid");
    check(fixture.writer.calls.empty(), "Malformed file published a journal");
  }
  writeFile(notes, "<<<<<<< HEAD\n[]\n=======\n[]\n>>>>>>> incoming\n");
  rejects([&] { store.persist(state); }, "merge_conflict");
}
void missingAndEmptyContents() {
  Fixture fixture;
  auto store = fixture.open();
  const auto state = store.committedState();
  const auto items = fixture.path(files::File::Items);
  writeFile(items, "null");
  writeFile(fixture.path(files::File::Annotations), "[]");
  check(store.current(), "Missing and empty tables stopped being equivalent");
  store.persist(state);
  fs::remove(items);
  check(store.current(), "Removing an empty file changed its logical table");
  store.persist(state);
  auto unknown = state;
  unknown["future_table"] = Json::array();
  store.persist(unknown);
  check(!store.current(), "Disk comparison ignored an extra state table");
}
void changedRowsStillRequireAdmission() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = initial(store);
  state["patches"] = Json::array({{{"addr", "0x0"},
                                   {"file_off", "0x0"},
                                   {"va", "0x1000"},
                                   {"size", std::uint64_t(1)},
                                   {"original", "00"},
                                   {"current", "01"},
                                   {"kind", "bytes"}}});
  state["items"] = Json::array(
      {{{"addr", "0x1000"}, {"size", std::uint64_t(1)}, {"kind", "byte"}}});
  store.persist(state);
  fixture.writer.clear();
  auto changed = state;
  changed["patches"][0]["size"] = 1.0;
  check(changed == state, "Floating-type equality control changed");
  rejects([&] { store.persist(changed); }, "invalid_request");
  changed = state;
  changed["items"][0]["size"] = std::int64_t(1);
  check(changed == state, "Signed-type equality control changed");
  rejects([&] { store.persist(changed); }, "history_invalid");
  changed = state;
  changed["patches"][0]["current"] = "gg";
  rejects([&] { store.persist(changed); }, "history_invalid");
  changed = state;
  changed["annotations"][0]["text"] = std::string("bad\0text", 8);
  rejects([&] { store.persist(changed); }, "invalid_request");
  check(fixture.writer.calls.empty() && store.committedState() == state &&
            store.current(),
        "Changed row reused admission or published transaction files");
}
void lossyValuesCannotPublishSnapshots() {
  Fixture fixture;
  auto store = fixture.open();
  const auto before = initial(store);
  fixture.writer.clear();
  auto after = before;
  after["annotations"][0]["text"] = std::string(1, '\xff');
  rejects([&] { store.persist(after); }, "history_invalid");
  rejects(
      [&] {
        store.stage({{"kind", "annotation"},
                     {"address", "0x1000"},
                     {"before", "before"},
                     {"after", std::string(1, '\xff')}});
      },
      "history_invalid");
  rejects([&] { store.next(false); }, "history_empty");
  for (const auto &value :
       {Json(std::numeric_limits<double>::infinity()), Json::binary({1, 2})}) {
    after = before;
    after["annotations"][0]["extra"] = value;
    rejects([&] { store.persist(after); }, "history_invalid");
    after = before;
    after["extra"] = value;
    rejects([&] { store.persist(after); }, "history_invalid");
    rejects(
        [&] {
          store.stage({{"kind", "annotation"},
                       {"address", "0x1000"},
                       {"before", "before"},
                       {"after", "after"},
                       {"extra", value}});
        },
        "history_invalid");
  }
  check(fixture.writer.calls.empty() && store.committedState() == before &&
            store.current(),
        "Lossy JSON created a snapshot that disagrees with its saved bytes");
}
void normalization() {
  auto state = ProjectHistory::normalizedState(
      {{"annotations", Json::array()}, {"renames", Json::array()}});
  state["annotations"] = Json::array({{{"addr", "0x20"}, {"text", "second"}},
                                      {{"addr", "0xA"}, {"text", "first"}}});
  auto sorted = ProjectHistory::normalizedState(state);
  check(sorted["annotations"][0]["addr"] == "0xa" &&
            sorted["annotations"][1]["addr"] == "0x20",
        "Unsorted address spellings did not normalize numerically");
  check(ProjectHistory::normalizedState(sorted) == sorted,
        "Already ordered state changed");
  state["annotations"].push_back({{"addr", "0xa"}, {"text", "duplicate"}});
  rejects([&] { ProjectHistory::normalizedState(state); }, "history_invalid");
}
void commandEncodingAndBranches() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = initial(store);
  Json commands = Json::array();
  auto stage = [&](std::string value) {
    Json command{{"kind", "annotation"},
                 {"address", "0x1000"},
                 {"before", state["annotations"][0]["text"]},
                 {"after", value},
                 {"extra", {{"number", -0.0}, {"quoted\"key", "\b\f中文"}}}};
    store.stage(command);
    commands.push_back(command);
    command["extra"] = "caller mutation";
    state["annotations"][0]["text"] = std::move(value);
  };
  auto verify = [&] {
    store.persist(state);
    Json header{{"schema_version", 1},
                {"source_sha256", std::string(64, '0')},
                {"engine_version", "fixture"},
                {"cursor", commands.size()},
                {"commands", commands}};
    auto expected = header.dump();
    expected.pop_back();
    expected += ",\"state\":" + store.committedState().dump() + "}";
    check(text(fixture.path(files::File::History)) == expected,
          "Command reuse changed serialized history bytes");
  };
  stage("quoted\"\\\n中文");
  stage("second\tcommand");
  verify();
  // Reopened commands encode lazily and must retain unknown fields and floats.
  store = fixture.open();
  const auto checkpoint = store;
  store.advance(false);
  state["annotations"][0]["text"] = commands.back()["before"];
  commands.erase(commands.end() - 1);
  stage("replacement redo branch");
  verify();
  check(checkpoint.next(false).at("after") == "second\tcommand" &&
            store.next(false).at("after") == "replacement redo branch",
        "Staging mutated a checkpoint's shared command");
  auto returned = store.next(false);
  returned["after"] = "returned value mutation";
  check(store.next(false).at("after") == "replacement redo branch",
        "Returned command was a mutable alias");
}
void stateEncodingAndSidecars() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = store.committedState();
  state["annotations"] = Json::array({annotation("quoted\"\\\n中文")});
  state["annotations"][0]["extra"] = {
      {"array",
       Json::array({nullptr, true, -0.0, Json::object(), Json::array()})},
      {"quoted\"key", "\\\\\"{}[,]"}};
  state["types"] = Json::array({{{"addr", "0x0"},
                                 {"language", "c"},
                                 {"source", "typedef int Item;"},
                                 {"extra", {{"empty", Json::array()}}}}});
  const auto verify = [&] {
    store.persist(state);
    Json expected{{"commands", Json::array()},
                  {"cursor", 0},
                  {"engine_version", "fixture"},
                  {"schema_version", 1},
                  {"source_sha256", std::string(64, '0')},
                  {"state", state}};
    check(text(fixture.path(files::File::History)) == expected.dump(),
          "Per-table serialization changed compact state bytes");
    check(text(fixture.path(files::File::Annotations)) ==
              state["annotations"].dump(2) + '\n',
          "Encoded table span changed annotation file bytes");
    auto types = state["types"];
    for (auto &row : types)
      row.erase("addr");
    check(text(fixture.path(files::File::Types)) == types.dump(2) + '\n',
          "Ordered table encoding retained synthetic addresses");
  };
  verify();
  store = fixture.open();
  state["annotations"][0]["text"] = "second";
  verify();
  // Unknown state fields remain in history, including escaped member names;
  // they do not become sidecar paths or change another table's byte span.
  state["unknown\"key"] = {{"value", "nested\n[]"}, {"float", -0.0}};
  verify();
}
void changedAddressSequence() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = store.committedState();
  state["annotations"] = Json::array({{{"addr", "0xa"}, {"text", "first"}},
                                      {{"addr", "0x20"}, {"text", "second"}}});
  state["types"] = Json::array(
      {{{"addr", "0x0"}, {"language", "c"}, {"source", "typedef int Item;"}}});
  store.persist(state);
  fixture.writer.clear();
  auto duplicate = state;
  duplicate["annotations"][1]["addr"] = "0xA";
  rejects([&] { store.persist(duplicate); }, "history_invalid");
  auto gap = state;
  gap["types"][0]["addr"] = "0x1";
  rejects([&] { store.persist(gap); }, "history_invalid");
  check(fixture.writer.calls.empty() && store.committedState() == state,
        "Changed address sequence bypassed pre-publication validation");
  state["annotations"][0]["addr"] = "0xA";
  state["annotations"][0]["text"] = "same position";
  store.persist(state);
  check(store.committedState()["annotations"][0]["addr"] == "0xa",
        "Equivalent address spelling was not normalized");
  state["annotations"][0]["addr"] = "0x30";
  store.persist(state);
  check(store.committedState()["annotations"][0]["addr"] == "0x20" &&
            store.committedState()["annotations"][1]["text"] == "same position",
        "Moved addresses retained stale order");
  state["annotations"].push_back({{"addr", "0x20"}, {"text", "duplicate"}});
  fixture.writer.clear();
  rejects([&] { store.persist(state); }, "history_invalid");
  check(fixture.writer.calls.empty(),
        "Changed row count bypassed duplicate-address validation");
}
void commandBudgetCountsEncodedBytes() {
  Fixture fixture;
  auto store = fixture.open();
  Json command{{"kind", "annotation"},
               {"address", "0x1000"},
               {"before", "before"},
               {"after", "after"},
               {"extra", ""}};
  const auto limit = neverd::limits::worker::kProjectHistoryMaxHistoryBytes / 2;
  command["extra"] = std::string(limit - 2 - command.dump().size(), 'x');
  check(Json::array({command}).dump().size() == limit,
        "Command byte-boundary fixture is not exact");
  store.stage(command);
  check(store.next(false) == command, "Exact-budget command was discarded");
  const auto checkpoint = store;
  command["extra"].get_ref<std::string &>() += 'x';
  store.stage(command);
  rejects([&] { store.next(false); }, "history_empty");
  check(
      checkpoint.next(false).at("extra").get_ref<const std::string &>().size() +
              1 ==
          command.at("extra").get_ref<const std::string &>().size(),
      "Budget eviction mutated another checkpoint");
  command["extra"] = "escaped\"\\\n";
  for (std::size_t i = 0;
       i < neverd::limits::worker::kProjectHistoryStageCommandsSizeLimit + 2;
       ++i) {
    command["sequence"] = i;
    store.stage(command);
  }
  store.persist(store.committedState());
  const auto document = Json::parse(text(fixture.path(files::File::History)));
  check(document.at("commands").size() ==
                neverd::limits::worker::kProjectHistoryStageCommandsSizeLimit &&
            document.at("commands").front().at("sequence") == 2,
        "Cached command byte accounting changed count-budget eviction");
}
void budgetBeforePublication() {
  Fixture fixture;
  auto store = fixture.open();
  const auto before = initial(store);
  const auto history = text(fixture.path(files::File::History));
  auto after = before;
  auto &rows = after["annotations"];
  rows = Json::array();
  constexpr std::size_t TextBytes = 1024;
  const auto count =
      neverd::limits::worker::kProjectHistoryMaxHistoryBytes / TextBytes + 1;
  for (std::size_t index = 0; index < count; ++index)
    rows.push_back(
        {{"addr", hexAddress(index)}, {"text", std::string(TextBytes, 'x')}});
  fixture.writer.clear();
  rejects([&] { store.persist(after); }, "history_budget_exceeded");
  check(fixture.writer.calls.empty() && store.committedState() == before &&
            text(fixture.path(files::File::History)) == history &&
            !ProjectHistory::recoveryPending(fixture.binary),
        "Oversized state published a partial transaction");
}
void journalBudgetBeforePublication() {
  Fixture fixture;
  auto store = fixture.open();
  auto before = store.committedState();
  constexpr std::size_t TextBytes = 1024;
  // Each history fits by itself, but the journal's four state occurrences
  // exceed its own budget. JSON row overhead puts this above the boundary.
  const auto count =
      neverd::limits::worker::kProjectHistoryMaxJournalBytes / (4 * TextBytes);
  for (std::size_t index = 0; index < count; ++index)
    before["annotations"].push_back(
        {{"addr", hexAddress(index)}, {"text", std::string(TextBytes, 'b')}});
  store.persist(before);
  const auto history = text(fixture.path(files::File::History));
  auto after = before;
  for (auto &row : after["annotations"])
    row["text"] = std::string(TextBytes, 'a');
  fixture.writer.clear();
  rejects([&] { store.persist(after); }, "history_budget_exceeded");
  check(fixture.writer.calls.empty() && store.committedState() == before &&
            text(fixture.path(files::File::History)) == history &&
            !ProjectHistory::recoveryPending(fixture.binary),
        "Oversized journal published a partial transaction");
  check(fixture.open().committedState() == before,
        "Oversized journal changed the persisted sidecars");
}

Json patch(std::uint64_t offset, const char *bytes = "91") {
  return {{"addr", hexAddress(offset)},
          {"file_off", hexAddress(offset)},
          {"va", hexAddress(0x1000 + offset)},
          {"size", std::uint64_t(1)},
          {"original", "90"},
          {"current", bytes},
          {"kind", "bytes"}};
}
Json historyDocument(const Json &state, const Json &commands = Json::array()) {
  return {
      {"schema_version", 1},         {"source_sha256", std::string(64, '0')},
      {"engine_version", "fixture"}, {"cursor", commands.size()},
      {"commands", commands},        {"state", state}};
}
void sharedPatchOwnership() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = initial(store);
  auto supplied = patch(10);
  supplied["addr"] = "0xA";
  auto first = ProjectHistory::patchRow(supplied);
  auto second = ProjectHistory::patchRow(patch(32));
  supplied["current"] = "ff";
  state["patches"] = Json::array({patch(10), patch(32)});
  store.persistPatches({second, first});
  check(store.committedState() == state &&
            store.committedPatches()[0] == first &&
            store.committedPatches()[1] == second,
        "Shared rows lost ordering, admission or independent ownership");
  check(text(fixture.path(files::File::History)) ==
                historyDocument(state).dump() &&
            text(fixture.path(files::File::Patches)) ==
                state["patches"].dump(2) + '\n',
        "Shared row encoding differs from the full JSON oracle");
  {
    const auto checkpoint = store;
    const auto &retained = checkpoint.committedState();
    const auto *types = &checkpoint.committedTable("types");
    store.persistPatches({ProjectHistory::patchRow(patch(10, "92")), second});
    check(retained == state && checkpoint.committedPatches()[0] == first &&
              &store.committedTable("types") == types,
          "Patch commit mutated a checkpoint or copied unchanged tables");
    fixture.writer.clear();
    rejects(
        [&] {
          auto stale = checkpoint;
          stale.persistPatches({first});
        },
        "foreign_edits");
    check(fixture.writer.calls.empty(),
          "Stale handles overwrote a newer state");
  }
  // Retaining another unchanged row must not keep the obsolete whole table
  // or snapshot alive. Each row owns only itself.
  std::weak_ptr<const ProjectHistory::PatchRow> retired = first;
  first.reset();
  check(retired.expired(), "An unchanged row retained an obsolete generation");
  state["patches"][0] = patch(10, "92");
  auto generic = state;
  generic["annotations"][0]["text"] = "generic edit";
  store.persist(generic);
  auto rows = store.committedPatches();
  rows.erase(rows.begin());
  rows.push_back(ProjectHistory::patchRow(patch(0)));
  store.persistPatches(rows);
  generic["patches"] = Json::array({patch(0), patch(32)});
  check(store.committedState() == generic &&
            fixture.open().committedState() == generic,
        "Generic and patch-only edits lost each other's state");
  fixture.writer.clear();
  rejects([&] { store.persistPatches({second, second}); }, "history_invalid");
  rejects([&] { store.persistPatches({nullptr}); }, "history_invalid");
  check(fixture.writer.calls.empty(),
        "Invalid handle sequence published files");
}
void sharedPatchRepresentation() {
  Fixture fixture;
  auto store = fixture.open();
  auto state = store.committedState();
  auto row = patch(0);
  row["extra"] = {{"zero", -0.0}, {"escaped", "quote\"\\\n"}};
  state["patches"] = Json::array({row});
  store.persistPatches({ProjectHistory::patchRow(row)});
  const auto original = text(fixture.path(files::File::Patches));
  row["extra"]["zero"] = 0.0;
  state["patches"][0] = row;
  fixture.writer.clear();
  // Value equality intentionally avoids a sidecar replacement, while the
  // caller's new representation must reach the history document unchanged.
  store.persist(state);
  check(text(fixture.path(files::File::History)) ==
                historyDocument(state).dump() &&
            text(fixture.path(files::File::Patches)) == original,
        "Logical equality reused an incorrectly encoded JSON representation");
  check(fixture.writer.calls ==
            std::vector<fs::path>{fixture.path(files::File::Journal),
                                  fixture.path(files::File::History)},
        "Equal pointer-different patch rows rewrote their sidecar");
  store.persistPatches({ProjectHistory::patchRow(row)});
  check(text(fixture.path(files::File::History)) ==
            historyDocument(state).dump(),
        "Patch-only encoding lost unknown members");
  fixture.writer.clear();
  for (auto invalid : {Json(1.0), Json(-1)}) {
    auto changed = row;
    changed["size"] = invalid;
    rejects([&] { ProjectHistory::patchRow(changed); }, "invalid_request");
  }
  auto invalid = row;
  invalid["current"] = "gg";
  rejects([&] { ProjectHistory::patchRow(invalid); }, "history_invalid");
  invalid = row;
  invalid["extra"] = std::numeric_limits<double>::infinity();
  rejects([&] { ProjectHistory::patchRow(invalid); }, "history_invalid");
  invalid["extra"] = std::string(1, '\xff');
  rejects([&] { store.persistPatches({ProjectHistory::patchRow(invalid)}); },
          "history_invalid");
  check(fixture.writer.calls.empty() && store.current(),
        "Invalid shared row reached publication");
}
void sharedPatchSidecarLayout() {
  Fixture fixture;
  auto store = fixture.open();
  std::string controls;
  for (char c = 0; c < 32; ++c)
    controls += c;
  ProjectHistory::PatchRows rows;
  auto expected = Json::array();
  for (std::uint64_t offset : {0, 10, 32}) {
    auto row = patch(offset);
    row["extra"] = {
        {"quote\"\\\b\f", controls},
        {"nested",
         Json::array({Json::object(), Json::array(), nullptr, false, -0.0, 1.5,
                      1.0e30, Json{{"text", "literal \\b \\f /\t\n雪"}}})}};
    if (offset == 0)
      row.erase("kind");
    rows.push_back(ProjectHistory::patchRow(row));
    expected.push_back(row);
  }
  const auto verify = [&] {
    check(text(fixture.path(files::File::Patches)) == sidecarText(expected),
          "Shared patch formatting differs from the complete sidecar oracle");
    check(fixture.open().committedState()["patches"] == expected,
          "Formatted patch rows did not survive reopening");
  };
  store.persistPatches(rows);
  verify();
  const auto retained = rows.front()->sidecarRow();
  const std::string retainedText(retained);
  expected[1]["current"] = "92";
  rows[1] = ProjectHistory::patchRow(expected[1]);
  store.persistPatches(rows);
  verify();
  check(retained == retainedText &&
            rows.front()->sidecarRow().data() == retained.data(),
        "Another row's edit invalidated an immutable sidecar view");
  rows.erase(rows.begin());
  expected.erase(expected.begin());
  store.persistPatches(rows);
  verify();
  rows.clear();
  expected.clear();
  store.persistPatches(rows);
  verify();
}
void sharedPatchFailureBoundaries() {
  // Patch-only commits have journal, changed patch sidecar, and history writes.
  for (std::size_t fail = 0; fail < 3; ++fail)
    for (bool afterWrite : {false, true})
      for (bool retry : {false, true}) {
        Fixture fixture;
        auto store = fixture.open();
        auto before = initial(store);
        const auto beforeHistory =
            Json::parse(text(fixture.path(files::File::History)));
        const auto row = patch(10);
        auto after = before;
        after["patches"] = Json::array({row});
        const Json command{{"kind", "patch"},
                           {"address", "0xa"},
                           {"before", nullptr},
                           {"after", row}};
        store.stage(command);
        fixture.writer.clear();
        fixture.writer.failAt = fail;
        fixture.writer.failAfter = afterWrite;
        const ProjectHistory::PatchRows rows{ProjectHistory::patchRow(row)};
        rejects([&] { store.persistPatches(rows); }, "injected_failure");
        check(store.committedState() == before,
              "Failed patch commit published its in-memory snapshot");
        const bool published = fail != 0 || afterWrite;
        if (published) {
          const auto journal =
              Json::parse(text(fixture.path(files::File::Journal)));
          check(journal ==
                    Json({{"schema_version", 1},
                          {"source_sha256", std::string(64, '0')},
                          {"before", before},
                          {"before_history", beforeHistory},
                          {"after", after},
                          {"after_history",
                           historyDocument(after, Json::array({command}))}}),
                "Patch-only journal differs from complete JSON oracle");
        }
        fixture.writer.clear();
        if (retry && !(fail == 0 && afterWrite)) {
          store.persistPatches(rows);
          check(store.committedState() == after && store.current(),
                "Same-session patch retry failed");
        } else if (retry) {
          // A throwing journal writer did not return ownership to this store.
          rejects([&] { store.persistPatches(rows); }, "recovery_required");
        }
        auto reopened = fixture.open();
        check(reopened.committedState() ==
                      (published || retry ? after : before) &&
                  reopened.current() &&
                  !ProjectHistory::recoveryPending(fixture.binary),
              "Reopened patch recovery produced mixed or pending state");
      }
}
void sharedPatchRetryAndForeignState() {
  Fixture fixture;
  auto store = fixture.open();
  auto before = initial(store);
  auto after = change(store, "interrupted generic");
  fixture.writer.clear();
  fixture.writer.failAt = 2;
  rejects([&] { store.persist(after); }, "injected_failure");
  fixture.writer.clear();
  const auto sidecar = fixture.path(files::File::Patches);
  writeFile(sidecar, Json::array({patch(7)}).dump());
  rejects([&] { store.persistPatches({ProjectHistory::patchRow(patch(0))}); },
          "foreign_edits");
  check(fixture.writer.calls.empty(),
        "Patch retry overwrote a foreign sidecar");
  fs::remove(sidecar);
  // A new request based on the old snapshot must keep its original other
  // tables after recovering the failed generic write, just as persist(Json).
  store.persistPatches({ProjectHistory::patchRow(patch(0))});
  before["patches"] = Json::array({patch(0)});
  check(store.committedState() == before &&
            fixture.open().committedState() == before,
        "Recovery silently substituted the requested non-patch tables");
  const auto timestamp = fs::last_write_time(sidecar);
  auto reformatted = Json::array({patch(0)}).dump(4) + '\n';
  writeFile(sidecar, reformatted);
  store.persistPatches(store.committedPatches());
  check(text(sidecar) == reformatted,
        "Equivalent external formatting was overwritten");
  auto foreign = reformatted;
  const auto at = foreign.find("91");
  check(at != std::string::npos, "Foreign patch fixture missing bytes");
  foreign.replace(at, 2, "92");
  writeFile(sidecar, foreign);
  fs::last_write_time(sidecar, timestamp);
  fixture.writer.clear();
  rejects([&] { store.persistPatches(store.committedPatches()); },
          "foreign_edits");
  check(fixture.writer.calls.empty() && text(sidecar) == foreign,
        "Patch state check trusted file size or timestamp");
}
void sharedPatchBudgets() {
  Fixture fixture;
  auto store = fixture.open();
  auto before = initial(store);
  auto row = patch(0);
  row["extra"] =
      std::string(neverd::limits::worker::kProjectHistoryMaxHistoryBytes, 'x');
  fixture.writer.clear();
  rejects([&] { store.persistPatches({ProjectHistory::patchRow(row)}); },
          "history_budget_exceeded");
  check(fixture.writer.calls.empty() && store.committedState() == before,
        "Oversized shared rows published files");
  row["extra"] = std::string(
      neverd::limits::worker::kProjectHistoryMaxJournalBytes / 4, 'a');
  store.persistPatches({ProjectHistory::patchRow(row)});
  const auto history = text(fixture.path(files::File::History));
  fixture.writer.clear();
  row["extra"].get_ref<std::string &>()[0] = 'b';
  rejects([&] { store.persistPatches({ProjectHistory::patchRow(row)}); },
          "history_budget_exceeded");
  check(fixture.writer.calls.empty() &&
            text(fixture.path(files::File::History)) == history &&
            !ProjectHistory::recoveryPending(fixture.binary),
        "Shared row journal budget published a partial transaction");
}
} // namespace
int main() {
  try {
    sharedPatchOwnership();
    sharedPatchRepresentation();
    sharedPatchSidecarLayout();
    sharedPatchFailureBoundaries();
    sharedPatchRetryAndForeignState();
    sharedPatchBudgets();
    unchangedFiles();
    failureBoundaries();
    retryAndConflict();
    journalContents();
    loadedDocumentJournal();
    independentCheckpoints();
    equivalentAndForeignContents();
    missingAndEmptyContents();
    changedRowsStillRequireAdmission();
    lossyValuesCannotPublishSnapshots();
    normalization();
    commandEncodingAndBranches();
    stateEncodingAndSidecars();
    changedAddressSequence();
    commandBudgetCountsEncodedBytes();
    budgetBeforePublication();
    journalBudgetBeforePublication();
    std::cout << "Unchanged sidecars, every write failure boundary, retries, "
                 "journal contents, budgets, foreign edits and normalization "
                 "passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
