#include "Engine.h"

#include "CodeEdits.h"
#include "Contributions.h"
#include "EngineSymbols.h"
#include "GraphSnapshot.h"
#include "Listing.h"
#include "OperandFormat.h"
#include "ProjectHistory.h"
#include "TextFold.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISigs.h"
#include "neverd/support/ProjectWriteLock.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#ifdef _WIN32
// std::min, std::max and numeric_limits<>::max() must not meet the macros.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace neverd::worker {
namespace fs = std::filesystem;
namespace {
// A signature source is external to the image. Never silently replay a
// different file/tree into a replica of an already loaded project.
Json signatureStamp(const fs::path &path) {
  if (!fs::exists(path))
    return nullptr;
  std::map<std::string, Json> files;
  const auto add = [&](const fs::path &file) {
    const auto name = file.generic_u8string();
    files[std::string(name.begin(), name.end())] = {
        fs::file_size(file),
        fs::last_write_time(file).time_since_epoch().count()};
  };
  if (fs::is_directory(path)) {
    for (const auto &entry : fs::recursive_directory_iterator(path))
      if (entry.is_regular_file())
        add(entry.path());
  } else {
    add(path);
  }
  return files;
}

using IRViewFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                       const char *, std::size_t, std::size_t);
using StringEncodingsFunction = const char *(*)();
using DecodeTextFunction = const char *(*)(const unsigned char *, int,
                                           const char *);
// Additive C ABI capabilities for strings in more than ASCII.
/// The engine's string encodings, or an empty array from an older engine.
const Json &stringEncodings() {
  static const Json encodings = [] {
    const auto function =
        engineSymbol<StringEncodingsFunction>("neverd_string_encodings_json");
    if (!function)
      return Json::array();
    const char *raw = function();
    Json parsed = Json::parse(raw ? raw : "[]", nullptr, false);
    neverd_free_string(raw);
    return parsed.is_array() ? parsed : Json::array();
  }();
  return encodings;
}
// Additive C ABI capability: the user's function edits.
using FunctionEditFunction = int (*)(neverd_session_t, neverd_va_t);
using SessionJsonFunction = const char *(*)(neverd_session_t);
using SessionLoadFunction = int (*)(neverd_session_t);
struct FunctionEditFunctions {
  FunctionEditFunction create, remove;
  SessionJsonFunction rows;
  SessionLoadFunction load;
  explicit operator bool() const { return create && remove && rows && load; }
};
const FunctionEditFunctions &functionEdits() {
  static const FunctionEditFunctions functions{
      engineSymbol<FunctionEditFunction>("neverd_func_create"),
      engineSymbol<FunctionEditFunction>("neverd_func_delete"),
      engineSymbol<SessionJsonFunction>("neverd_functions_json"),
      engineSymbol<SessionLoadFunction>("neverd_functions_load")};
  return functions;
}
// Additive C ABI capability: the user's data items.
using ItemSetFunction = int (*)(neverd_session_t, neverd_va_t, const char *);
using StringAtFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                         const char *);
struct DataItemFunctions {
  ItemSetFunction set;
  SessionJsonFunction rows;
  SessionLoadFunction load;
  StringAtFunction stringAt;
  explicit operator bool() const { return set && rows && load && stringAt; }
};
const DataItemFunctions &dataItems() {
  static const DataItemFunctions functions{
      engineSymbol<ItemSetFunction>("neverd_item_set"),
      engineSymbol<SessionJsonFunction>("neverd_items_json"),
      engineSymbol<SessionLoadFunction>("neverd_items_load"),
      engineSymbol<StringAtFunction>("neverd_string_at")};
  return functions;
}
// Additive C ABI capability: how the user shows operands' numbers.
using OperandFormatSetFunction = int (*)(neverd_session_t, neverd_va_t, int,
                                         const char *);
struct OperandFormatFunctions {
  OperandFormatSetFunction set;
  SessionJsonFunction rows;
  SessionLoadFunction load;
  explicit operator bool() const { return set && rows && load; }
};
const OperandFormatFunctions &operandFormats() {
  static const OperandFormatFunctions functions{
      engineSymbol<OperandFormatSetFunction>("neverd_operand_format_set"),
      engineSymbol<SessionJsonFunction>("neverd_operand_formats_json"),
      engineSymbol<SessionLoadFunction>("neverd_operand_formats_load")};
  return functions;
}
// Additive C ABI capability: the loader the user chose for a file, which the
// engine reads again from the input's load options sidecar.
using LoadOptionsSetFunction = int (*)(neverd_session_t, const char *);
struct LoadOptionsFunctions {
  LoadOptionsSetFunction set;
  SessionJsonFunction json;
  explicit operator bool() const { return set && json; }
};
const LoadOptionsFunctions &loadOptions() {
  static const LoadOptionsFunctions functions{
      engineSymbol<LoadOptionsSetFunction>("neverd_session_set_load_options"),
      engineSymbol<SessionJsonFunction>("neverd_session_load_options_json")};
  return functions;
}
constexpr std::string_view LoadOptionsSuffix = ".neverd-load.json";
using IdentifyFunction = const char *(*)(const char *);
IdentifyFunction identifyFunction() {
  static const auto function =
      engineSymbol<IdentifyFunction>("neverd_identify_json");
  return function;
}
IRViewFunction irViewFunction() {
  // Additive C ABI capability: an older matching engine can still run the GUI.
  static const auto function =
      engineSymbol<IRViewFunction>("neverd_ir_view_json");
  return function;
}
SessionJsonFunction headersFunction() {
  static const auto function =
      engineSymbol<SessionJsonFunction>("neverd_headers_json");
  return function;
}
/// Engine code text under workbench function names: every identifier token
/// that is an engine name with a display alias, outside string and character
/// literals.  \p shift receives (old byte offset, delta) for each edit.
std::string
renameIdentifiers(const std::string &text,
                  const std::unordered_map<std::string, std::string> &aliases,
                  std::vector<std::pair<std::size_t, std::ptrdiff_t>> &shift) {
  const auto identifierStart = [](unsigned char c) {
    return std::isalpha(c) || c == '_';
  };
  const auto identifierPart = [](unsigned char c) {
    return std::isalnum(c) || c == '_';
  };
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c == '"' || c == '\'') {
      std::size_t j = i + 1;
      while (j < text.size() && text[j] != static_cast<char>(c) &&
             text[j] != '\n')
        j += text[j] == '\\' ? 2 : 1;
      j = std::min(text.size(), j + 1);
      out.append(text, i, j - i);
      i = j;
      continue;
    }
    if (identifierStart(c) &&
        (i == 0 || !identifierPart(static_cast<unsigned char>(text[i - 1])))) {
      std::size_t j = i + 1;
      while (j < text.size() &&
             identifierPart(static_cast<unsigned char>(text[j])))
        ++j;
      const std::string_view word(text.data() + i, j - i);
      if (const auto it = aliases.find(std::string(word));
          it != aliases.end()) {
        out += it->second;
        shift.emplace_back(i, static_cast<std::ptrdiff_t>(it->second.size()) -
                                  static_cast<std::ptrdiff_t>(word.size()));
      } else {
        out.append(word);
      }
      i = j;
      continue;
    }
    out += static_cast<char>(c);
    ++i;
  }
  return out;
}

/// A byte offset of the original text in the renamed text.
std::size_t shiftedOffset(
    std::size_t offset,
    const std::vector<std::pair<std::size_t, std::ptrdiff_t>> &shift) {
  std::ptrdiff_t delta = 0;
  for (const auto &[at, change] : shift) {
    if (at >= offset)
      break;
    delta += change;
  }
  return static_cast<std::size_t>(static_cast<std::ptrdiff_t>(offset) + delta);
}

fs::path utf8Path(const std::string &text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()),
                                text.size()));
}
void canonicalizeAddresses(Json &value) {
  if (value.is_array()) {
    for (auto &item : value)
      canonicalizeAddresses(item);
  } else if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      const auto &key = it.key();
      if (key == "addresses" && it->is_array()) {
        for (auto &address : *it)
          if (address.is_string())
            address = hexAddress(parseAddress(address.get<std::string>()));
      } else if ((key == "addr" || key == "va" || key == "address" ||
                  key == "start" || key == "end" || key == "entry" ||
                  key == "iat_addr" || key == "from" || key == "to") &&
                 it->is_string()) {
        const auto &text = it->get_ref<const std::string &>();
        if (text.starts_with("0x") || text.starts_with("0X"))
          *it = hexAddress(parseAddress(text));
      } else
        canonicalizeAddresses(*it);
    }
  }
}
std::string ownedString(const char *value) {
  std::unique_ptr<const char, decltype(&neverd_free_string)> owned(
      value, neverd_free_string);
  if (!value)
    return {};
  const auto size = strnlen(value, MaxBackendBytes + 1);
  if (size > MaxBackendBytes)
    throw Error("budget_exceeded",
                "Engine result exceeds the 32 MiB adapter budget");
  return std::string(value, size);
}
/// \p text folded for comparisons that ignore case, in any script.
std::string folded(const std::string &text) { return foldText(text); }
constexpr std::size_t MaxFunctionRows = 1000000;
// Strings need this many display columns unless string_options says
// otherwise; a minimum is at most MaxStringMinLength
// (neverd::strings::MaxMinLength).
constexpr int DefaultStringMinLength = 4;
constexpr std::int64_t MaxStringMinLength = 1024;
/// Lines of one engine code page.
constexpr std::size_t EnginePageLines = 2048;
/// Text a renamed whole-function view may hold: the engine's source budget.
constexpr std::size_t MaxNamedViewBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxNamedViewCacheBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxNamedViews = 8;

/// Conservative retained-size accounting: include container capacity, string
/// capacity and tree-node overhead, not just the serialized text's size. A
/// value beyond the cache budget remains readable but is not retained.
std::size_t namedViewJsonBytes(const Json &value, unsigned depth = 0) {
  constexpr auto OverBudget = MaxNamedViewCacheBytes + 1;
  if (depth > 64)
    return OverBudget;
  std::size_t bytes = sizeof(Json);
  const auto add = [&](std::size_t count) {
    bytes = count > MaxNamedViewCacheBytes - bytes ? OverBudget : bytes + count;
    return bytes <= MaxNamedViewCacheBytes;
  };
  if (value.is_string()) {
    if (!add(sizeof(Json::string_t)) ||
        !add(value.get_ref<const Json::string_t &>().capacity() + 1))
      return OverBudget;
  } else if (value.is_array()) {
    const auto &array = value.get_ref<const Json::array_t &>();
    if (!add(sizeof(Json::array_t)) ||
        array.capacity() > MaxNamedViewCacheBytes / sizeof(Json) ||
        !add(array.capacity() * sizeof(Json)))
      return OverBudget;
    for (const auto &item : array)
      if (!add(namedViewJsonBytes(item, depth + 1)))
        return OverBudget;
  } else if (value.is_object()) {
    if (!add(sizeof(Json::object_t)))
      return OverBudget;
    for (const auto &[key, item] : value.items())
      if (!add(sizeof(Json::object_t::value_type) + 4 * sizeof(void *)) ||
          !add(key.capacity() + 1) || !add(namedViewJsonBytes(item, depth + 1)))
        return OverBudget;
  }
  return bytes;
}

/// A page of \p items; a filter keeps the rows where one of \p searchFields
/// contains it, ignoring the case of ASCII letters.
Json page(const Json &items, const Json &payload,
          std::initializer_list<const char *> searchFields = {}) {
  const auto offset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  const auto limit = sizeField(payload, "limit", 128, 512);
  if (!limit)
    throw Error("invalid_request", "limit must be at least 1");
  const auto filter = folded(stringField(payload, "filter"));
  Json selected = Json::array();
  std::size_t total = 0;
  for (const auto &item : items) {
    if (!filter.empty() &&
        std::none_of(searchFields.begin(), searchFields.end(),
                     [&](const char *field) {
                       const auto it = item.find(field);
                       return it != item.end() && it->is_string() &&
                              folded(it->get<std::string>()).find(filter) !=
                                  std::string::npos;
                     }))
      continue;
    if (total >= offset && selected.size() < limit)
      selected.push_back(item);
    ++total;
  }
  const bool complete = offset >= total || selected.size() >= total - offset;
  return {{"items", std::move(selected)},
          {"total", total},
          {"offset", offset},
          {"next_offset", complete ? Json(nullptr) : Json(offset + limit)},
          {"complete", complete}};
}

/// Sort table rows by {"sort": field, "descending": bool}.  Hex address
/// strings compare numerically; other strings case-insensitively.
void sortItems(Json &items, const Json &payload) {
  const auto field = stringField(payload, "sort", {}, 64);
  if (field.empty() || !items.is_array())
    return;
  const bool descending = payload.value("descending", false);
  const auto key =
      [&](const Json &item) -> std::pair<std::uint64_t, std::string> {
    const auto it = item.find(field);
    if (it == item.end())
      return {0, {}};
    if (it->is_number_unsigned())
      return {it->get<std::uint64_t>(), {}};
    if (it->is_number_integer())
      return {static_cast<std::uint64_t>(it->get<std::int64_t>()), {}};
    if (it->is_string()) {
      const auto &text = it->get_ref<const std::string &>();
      if (text.size() > 2 && text[0] == '0' &&
          (text[1] == 'x' || text[1] == 'X'))
        try {
          return {parseAddress(text), {}};
        } catch (const Error &) {
        }
      return {0, folded(text)};
    }
    return {0, {}};
  };
  std::vector<std::pair<std::pair<std::uint64_t, std::string>, Json>> keyed;
  keyed.reserve(items.size());
  for (auto &item : items)
    keyed.emplace_back(key(item), std::move(item));
  std::stable_sort(keyed.begin(), keyed.end(),
                   [&](const auto &a, const auto &b) {
                     return descending ? b.first < a.first : a.first < b.first;
                   });
  items = Json::array();
  for (auto &entry : keyed)
    items.push_back(std::move(entry.second));
}

// Replace one sidecar atomically. Each sidecar is a separate durable file;
// this is intentionally not advertised as a transaction spanning both files.
void atomicWrite(const fs::path &path, const Json &value) {
  static std::atomic<std::uint64_t> sequence{0};
  fs::path temporary = path;
  temporary +=
      ".tmp-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(sequence++);
  // Persistence validates compact serialized byte budgets. Write that same
  // encoding, otherwise indentation can produce an unreadable saved history.
  const auto body = value.dump(-1, ' ', false, Json::error_handler_t::replace);
#ifdef _WIN32
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    throw Error("save_failed", "Cannot create sidecar temporary file");
  DWORD written = 0;
  const bool success =
      WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written,
                nullptr) &&
      written == body.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (!success ||
      !MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(temporary.c_str());
    throw Error("save_failed", "Cannot atomically replace sidecar");
  }
#else
  int descriptor =
      ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (descriptor < 0)
    throw Error("save_failed", "Cannot create sidecar temporary file");
  std::size_t offset = 0;
  bool success = true;
  while (offset < body.size()) {
    const auto count =
        ::write(descriptor, body.data() + offset, body.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      success = false;
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  if (::fsync(descriptor) != 0)
    success = false;
  if (::close(descriptor) != 0)
    success = false;
  if (!success || ::rename(temporary.c_str(), path.c_str()) != 0) {
    ::unlink(temporary.c_str());
    throw Error("save_failed", "Cannot atomically replace sidecar");
  }
  int directory = ::open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (directory >= 0) {
    (void)::fsync(directory);
    ::close(directory);
  }
#endif
}
} // namespace

class ProjectLock {
public:
  explicit ProjectLock(const fs::path &binary) : guard_(binary) {
    if (!guard_)
      throw Error("project_locked",
                  "Cannot acquire the sidecar writer lock: " + guard_.error());
  }

private:
  neverd::ProjectWriteLock guard_;
};

Engine::Engine()
    : session_(neverd_session_create()),
      contributions_(std::make_unique<Contributions>()) {
  if (!session_)
    throw Error("engine_unavailable", "Cannot create NeverD session");
}
Engine::~Engine() { neverd_session_destroy(session_); }
std::string Engine::version() { return ownedString(neverd_version_number()); }
std::string Engine::error() const {
  return ownedString(neverd_last_error(session_));
}
void Engine::requireLoaded() const {
  if (!neverd_session_is_loaded(session_))
    throw Error("not_loaded", "Open a binary first");
}
void Engine::requireWriter() const {
  requireLoaded();
  if (readOnly_ || !lock_)
    throw Error("read_only", "This session is read-only");
}
bool Engine::keepsFunctionEdits() { return static_cast<bool>(functionEdits()); }
bool Engine::keepsDataItems() { return static_cast<bool>(dataItems()); }
bool Engine::identifiesFiles() { return identifyFunction() != nullptr; }
bool Engine::keepsOperandFormats() {
  return static_cast<bool>(operandFormats());
}
bool Engine::reloadUserState() {
  reloadCodeEdits();
  const bool annotations = neverd_annotations_load(session_) == 0;
  const bool renames = neverd_renames_load(session_) == 0;
  const bool functions = reloadFunctionEdits();
  const bool items = reloadDataItems();
  const bool operands = reloadOperandFormats();
  return annotations && renames && functions && items && operands;
}
bool Engine::reloadOperandFormats() {
  const auto &formats = operandFormats();
  if (!formats)
    return true;
  const auto before = operandFormatRows();
  if (formats.load(session_) != 0)
    return false;
  if (operandFormatRows() != before)
    operandFormatsChanged();
  return true;
}
void Engine::namesChanged() {
  forgetNamedViews();
  graphs_.clear();
  if (listing_)
    listing_->namesChanged();
  textKey_.clear();
  textCache_.clear();
  textLines_.clear();
  functionOrderKey_.clear();
  functionOrder_.clear();
}
Json Engine::functionGraph(std::uint64_t address) {
  prepareFunction(address);
  return backendJson(neverd_cfg_json(session_, address), true);
}
void Engine::commentsChanged() {
  forgetNamedViews();
  graphs_.clear();
  textKey_.clear();
  textCache_.clear();
  textLines_.clear();
}
void Engine::operandFormatsChanged() {
  // Graph nodes hold formatted lines; pseudocode does not show the formats.
  graphs_.clear();
  if (listing_)
    listing_->reloadNumberFormats();
}
Json Engine::operandFormatRows() const {
  const auto &formats = operandFormats();
  return formats ? backendJson(formats.rows(session_)) : Json::array();
}
bool Engine::reloadDataItems() {
  const auto &items = dataItems();
  if (!items)
    return true;
  const auto before = dataItemRows();
  if (items.load(session_) != 0)
    return false;
  if (dataItemRows() != before)
    namesChanged();
  return true;
}
Json Engine::dataItemRows() const {
  const auto &items = dataItems();
  return items ? backendJson(items.rows(session_)) : Json::array();
}
bool Engine::reloadFunctionEdits() {
  const auto &edits = functionEdits();
  if (!edits)
    return true;
  const auto before = functionEditRows();
  if (edits.load(session_) != 0)
    return false;
  if (functionEditRows() != before)
    functionsChanged();
  return true;
}
Json Engine::functionEditRows() const {
  const auto &edits = functionEdits();
  return edits ? backendJson(edits.rows(session_)) : Json::array();
}
void Engine::functionsChanged() {
  analyzed_ = false;
  preparedFunction_.reset();
  if (listing_)
    listing_->reindex();
  invalidate();
}
void Engine::invalidate() {
  forgetNamedViews();
  graphs_.clear();
  if (listing_)
    listing_->invalidate();
  textKey_.clear();
  textCache_.clear();
  textLines_.clear();
  functionOrderKey_.clear();
  functionOrder_.clear();
}
void Engine::forgetNamedViews() {
  namedViews_.clear();
  namedViewBytes_ = 0;
}

std::shared_ptr<const Engine::NamedView>
Engine::namedView(std::uint64_t address, const std::string &representation) {
  const auto view = irViewFunction();
  if (!view)
    return nullptr;
  const bool sourceView = representation == "c" || representation == "llvmc" ||
                          representation == "source" ||
                          representation == "cpp" || representation == "rust" ||
                          representation == "go";
  (void)listing().functionAliases();
  const auto cacheKey = [&] {
    const auto generation = listing().generation();
    if (namedViewsRevision_ != revision_ ||
        namedViewsListingGeneration_ != generation) {
      forgetNamedViews();
      namedViewsRevision_ = revision_;
      namedViewsListingGeneration_ = generation;
    }
    return hexAddress(address) + ":" + representation + ":" +
           std::to_string(revision_) + ":" + std::to_string(generation);
  };
  const auto lookupKey = cacheKey();
  for (auto it = namedViews_.begin(); it != namedViews_.end(); ++it)
    if ((*it)->key == lookupKey) {
      namedViews_.splice(namedViews_.begin(), namedViews_, it);
      return namedViews_.front();
    }
  // A hit answers from its finished document without pretending the mutable
  // Session still holds this function. Graph/IR requests prepare it normally.
  prepareFunction(address);
  // Whole-program VM preparation can analyze(), invalidating the listing and
  // advancing the revision. Rebuild aliases and stamp the prepared context;
  // never keep an alias reference across that invalidation.
  // Preparation may also discover this function and create its display
  // alias. Decide whether an IR view needs renaming only after that step,
  // so the first page and subsequent pages use the same presentation.
  if (listing().functionAliases().empty() && !sourceView)
    return nullptr;
  const auto key = cacheKey();
  auto document = std::make_shared<NamedView>();
  {
    // The whole function, from as many engine pages as it fills: the engine
    // emits it once and pages it from there.
    Json full;
    auto backendRepresentation = representation;
    for (std::size_t next = 0;;) {
      auto page =
          backendJson(view(session_, address, backendRepresentation.c_str(),
                           next, EnginePageLines),
                      true);
      // Older page APIs offer HighC but not the automatic source dialect.
      // Keep the Pseudocode action useful, with the actual dialect reported.
      if (next == 0 && representation == "source" && page.is_object() &&
          !page.contains("text") &&
          page.value("mapping_status", std::string()) ==
              "unsupported_representation") {
        backendRepresentation = "c";
        page = backendJson(view(session_, address, "c", next, EnginePageLines),
                           true);
      }
      if (!page.is_object() || !page.contains("text") ||
          !page["text"].is_string() || !page.contains("rows") ||
          !page["rows"].is_array() ||
          page.value("offset", std::size_t{0}) != next)
        return nullptr;
      const bool complete = page.value("complete", false);
      const Json following = page.value("next_offset", Json());
      if (full.is_null()) {
        full = std::move(page);
      } else {
        full["text"].get_ref<std::string &>() +=
            page["text"].get_ref<const std::string &>();
        for (auto &row : page["rows"])
          full["rows"].push_back(std::move(row));
        // A spelled page names the source names on it alone.
        if (auto names = page.find("source_names");
            names != page.end() && names->is_array() &&
            full.contains("source_names") && full["source_names"].is_array())
          for (auto &name : *names)
            full["source_names"].push_back(std::move(name));
      }
      if (complete)
        break;
      if (!following.is_number_unsigned() ||
          following.get<std::size_t>() <= next ||
          full["text"].get_ref<const std::string &>().size() >
              MaxNamedViewBytes)
        return nullptr;
      next = following.get<std::size_t>();
    }
    full["complete"] = true;
    full["next_offset"] = nullptr;
    full["representation"] = representation;
    if (representation == "source" && backendRepresentation == "c")
      full["dialect"] = "c";
    if (sourceView)
      CodeEdits::decorate(
          full, codeEditRow(address), address, representation,
          backendJson(neverd_renames_json(session_)),
          backendJson(neverd_annotations_json(session_)),
          [this](const std::string &name) -> std::optional<std::uint64_t> {
            Json target;
            try {
              target = execute("resolve", {{"query", name}});
            } catch (const Error &) {
              return std::nullopt;
            }
            if (target.is_object() && target.contains("address") &&
                target["address"].is_string())
              return parseAddress(target["address"].get<std::string>());
            return std::nullopt;
          });
    std::vector<std::pair<std::size_t, std::ptrdiff_t>> shift;
    if (!sourceView)
      full["text"] = renameIdentifiers(full["text"].get<std::string>(),
                                       listing().functionAliases(), shift);
    const std::size_t base = full.value("byte_offset", std::size_t{0});
    if (auto regions = full.find("library_regions");
        regions != full.end() && regions->is_array())
      for (auto &region : *regions)
        if (auto spans = region.find("spans");
            spans != region.end() && spans->is_array())
          for (auto &span : *spans)
            for (const char *field : {"begin_byte", "end_byte"})
              if (span.contains(field) && span[field].is_number_unsigned() &&
                  span[field].get<std::size_t>() >= base)
                span[field] =
                    base +
                    shiftedOffset(span[field].get<std::size_t>() - base, shift);
    if (auto names = full.find("source_names");
        names != full.end() && names->is_array())
      for (auto &name : *names)
        for (const char *field : {"begin_byte", "end_byte"})
          if (name.contains(field) && name[field].is_number_unsigned() &&
              name[field].get<std::size_t>() >= base)
            name[field] =
                base +
                shiftedOffset(name[field].get<std::size_t>() - base, shift);
    // Renames before the definition move where it begins, not its line.
    if (auto prelude = full.find("prelude");
        prelude != full.end() && prelude->is_object() &&
        prelude->contains("end_byte") &&
        (*prelude)["end_byte"].is_number_unsigned() &&
        (*prelude)["end_byte"].get<std::size_t>() >= base)
      (*prelude)["end_byte"] =
          base + shiftedOffset((*prelude)["end_byte"].get<std::size_t>() - base,
                               shift);
    const auto &text = full["text"].get_ref<const std::string &>();
    document->lines.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n' && i + 1 < text.size())
        document->lines.push_back(i + 1);
    document->value = std::move(full);
    document->key = key;
    // If resolving source names changed the listing while decorating this
    // document, it remains usable for this response but is not reusable yet.
    if (cacheKey() != key)
      return document;
    const auto jsonBytes = namedViewJsonBytes(document->value);
    const auto overhead =
        sizeof(NamedView) + 8 * sizeof(void *) + document->key.capacity() + 1;
    if (jsonBytes <= MaxNamedViewCacheBytes &&
        overhead <= MaxNamedViewCacheBytes - jsonBytes &&
        document->lines.capacity() <=
            (MaxNamedViewCacheBytes - jsonBytes - overhead) /
                sizeof(std::size_t)) {
      document->bytes = jsonBytes + overhead +
                        document->lines.capacity() * sizeof(std::size_t);
      while (!namedViews_.empty() &&
             (namedViews_.size() >= MaxNamedViews ||
              document->bytes > MaxNamedViewCacheBytes - namedViewBytes_)) {
        namedViewBytes_ -= namedViews_.back()->bytes;
        namedViews_.pop_back();
      }
      namedViews_.push_front(document);
      namedViewBytes_ += document->bytes;
    }
  }
  return document;
}

std::optional<Json> Engine::namedViewPage(std::uint64_t address,
                                          const std::string &representation,
                                          std::size_t offset,
                                          std::size_t limit) {
  const auto document = namedView(address, representation);
  if (!document)
    return std::nullopt;
  const auto &namedView_ = document->value;
  const auto &namedViewLines_ = document->lines;
  const auto &text = namedView_["text"].get_ref<const std::string &>();
  const std::size_t total = text.empty() ? 0 : namedViewLines_.size();
  const auto start = std::min(offset, total);
  const auto end = start + std::min(limit, total - start);
  const auto startByte = start < namedViewLines_.size() && !text.empty()
                             ? namedViewLines_[start]
                             : text.size();
  const auto endByte =
      end < namedViewLines_.size() ? namedViewLines_[end] : text.size();
  Json page = Json::object();
  for (const auto &[field, value] : namedView_.items())
    if (field != "text" && field != "rows" &&
        !((field == "source_names" || field == "code_names") && value.is_array()))
      page[field] = value;
  page["text"] = text.substr(startByte, endByte - startByte);
  Json rows = Json::array();
  for (const auto &row : namedView_.at("rows")) {
    const auto line = row.value("line", std::size_t{0});
    if (line >= start && line < end)
      rows.push_back(row);
  }
  page["rows"] = std::move(rows);
  const std::size_t base = namedView_.value("byte_offset", std::size_t{0});
  for (const char *field : {"source_names", "code_names"}) {
    if (!namedView_.contains(field) || !namedView_[field].is_array())
      continue;
    const auto &names = namedView_[field];
    Json onPage = Json::array();
    for (const auto &name : names) {
      const auto begin = name.value("begin_byte", std::size_t{0});
      const auto nameEnd = name.value("end_byte", std::size_t{0});
      if (nameEnd > base + startByte && begin < base + endByte)
        onPage.push_back(name);
    }
    page[field] = std::move(onPage);
  }
  page["offset"] = start;
  page["byte_offset"] = base + startByte;
  page["total_lines"] = total;
  page["complete"] = end == total;
  page["next_offset"] = end == total ? Json(nullptr) : Json(end);
  page["project_id"] = projectId_;
  page["revision"] = revision();
  return page;
}

void Engine::analyze() {
  if (analyzed_)
    return;
  // A restricted single-function pipeline must not stand in for the image.
  neverd_session_restrict_function(session_, 0);
  preparedFunction_.reset();
  if (!neverd_session_analyze(session_))
    throw Error("analysis_failed", error());
  analyzed_ = true;
  invalidate();
  if (listing_)
    listing_->loadSwitches();
  ++revision_;
}
void Engine::prepareFunction(std::uint64_t address) {
  if (analyzed_ || preparedFunction_ == address)
    return;
  // VM images analyze the whole program as one unit.
  const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
  if (arch == "evm" || arch == "sbf") {
    analyze();
    return;
  }
  // neverd_decompile() restricts the session pipeline to this entry and
  // replaces a previous restriction; IR, CFG and LLVM views then read it.
  (void)ownedString(neverd_decompile(session_, address));
  preparedFunction_ = address;
  textKey_.clear();
}
Listing &Engine::newListing() {
  listing_ = std::make_unique<Listing>(session_);
  listing_->setStringOptions(stringOptions_);
  if (analyzed_)
    listing_->loadSwitches();
  return *listing_;
}

Listing &Engine::listing() {
  requireLoaded();
  if (!listing_)
    newListing();
  return *listing_;
}
bool Engine::hasIdleWork() const {
  return backgroundAnalysis_ && listing_ &&
         neverd_session_is_loaded(session_) && listing_->hasIdleWork();
}
void Engine::idleStep() {
  if (hasIdleWork())
    listing_->idleStep();
}
Json Engine::backgroundState() const {
  if (!listing_)
    return nullptr;
  return listing_->indexState();
}
Json Engine::backendJson(const char *owned, bool checkError) const {
  const auto text = ownedString(owned);
  if (checkError) {
    auto diagnostic = error();
    if (!diagnostic.empty())
      throw Error(diagnostic.find("budget") != std::string::npos
                      ? "budget_exceeded"
                      : "engine_error",
                  diagnostic);
  }
  if (text.empty())
    throw Error("engine_error",
                error().empty() ? "Engine returned no result" : error());
  try {
    auto value = Json::parse(text);
    canonicalizeAddresses(value);
    return value;
  } catch (const Json::exception &) {
    throw Error("engine_error", "Engine returned malformed JSON");
  }
}
ProjectHistory &Engine::history() {
  const auto path = utf8Path(ownedString(neverd_session_file_path(session_)));
  if (fs::file_size(path) != loadedSize_ ||
      fs::last_write_time(path) != loadedTime_)
    throw Error(
        "input_changed",
        "Input changed while the Session was open; reopen it before editing");
  if (!history_) {
    const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
    if (arch == "evm" || arch == "sbf")
      analyze();
    // Hashing lives behind the C ABI; the worker gains no LLVM linkage.  The
    // loader's own hash costs nothing, where an older engine's dashboard
    // hashes the whole input again.
    static const auto inputHash =
        engineSymbol<SessionJsonFunction>("neverd_session_input_sha256");
    const auto hash = inputHash ? ownedString(inputHash(session_))
                                : backendJson(neverd_dashboard_json(session_))
                                      .value("hashes", Json::object())
                                      .value("sha256", std::string());
    history_ = std::make_unique<ProjectHistory>(
        utf8Path(ownedString(neverd_session_file_path(session_))), hash,
        version(), readOnly_, atomicWrite);
    if (history_->recovered()) {
      if (!reloadUserState())
        throw Error("recovery_required",
                    "Recovered files could not be reloaded into the Session");
      dirty_ = false;
      invalidate();
      ++revision_;
    }
    history_->verifyLoadedState(
        {{"annotations", backendJson(neverd_annotations_json(session_))},
         {"renames", backendJson(neverd_renames_json(session_))},
         {"functions", functionEditRows()},
         {"items", dataItemRows()},
         {"operands", operandFormatRows()},
         {"code_edits", codeEdits_}});
  }
  return *history_;
}
Json Engine::metadata() const {
  requireLoaded();
  const auto arch = ownedString(neverd_session_arch_name(session_));
  const bool deferred =
      !analyzed_ && (folded(arch) == "evm" || folded(arch) == "sbf");
  // The language whose runtime built the image, as the engine read it.
  const auto headers = headersFunction()
                           ? backendJson(headersFunction()(session_))
                           : Json::object();
  using DiagnosticsFunction = const char *(*)(neverd_session_t);
  static const auto diagnosticsFunction =
      engineSymbol<DiagnosticsFunction>("neverd_session_load_diagnostics_json");
  Json diagnostics = Json::array();
  if (diagnosticsFunction) {
    const auto text = ownedString(diagnosticsFunction(session_));
    const auto parsed = Json::parse(text, nullptr, false);
    if (parsed.is_array())
      diagnostics = parsed;
  }
  return {{"path", ownedString(neverd_session_file_path(session_))},
          {"architecture", arch},
          {"format", ownedString(neverd_session_format_name(session_))},
          {"bitness", neverd_session_bitness(session_)},
          {"file_size", std::to_string(neverd_session_file_size(session_))},
          {"base_address", hexAddress(neverd_session_base_addr(session_))},
          {"entry_address", hexAddress(neverd_session_entry_addr(session_))},
          {"loader_diagnostics", std::move(diagnostics)},
          {"function_count",
           deferred ? Json(nullptr) : Json(neverd_func_count(session_))},
          {"segment_count", neverd_session_segment_count(session_)},
          {"section_count", neverd_session_section_count(session_)},
          {"import_count", neverd_session_import_count(session_)},
          {"export_count", neverd_session_export_count(session_)},
          {"symbol_count", neverd_session_symbol_count(session_)},
          {"language", headers.value("language", Json::object())},
          {"analyzed", analyzed_},
          {"analysis_state", analyzed_ ? "complete" : "not_analyzed"},
          {"read_only", readOnly_},
          {"dirty", dirty_},
          // How the file was read: for a binary file, its processor,
          // placement and platform, and whether detection chose the platform.
          {"load_options", loadOptions()
                               ? backendJson(loadOptions().json(session_))
                               : Json::object()}};
}

std::string Engine::inputHash() const {
  static const auto hash =
      engineSymbol<SessionJsonFunction>("neverd_session_input_sha256");
  return hash ? ownedString(hash(session_))
              : backendJson(neverd_dashboard_json(session_))
                    .value("hashes", Json::object())
                    .value("sha256", std::string());
}

Json Engine::userState() const {
  return {{"annotations", backendJson(neverd_annotations_json(session_))},
          {"renames", backendJson(neverd_renames_json(session_))},
          {"functions", functionEditRows()},
          {"items", dataItemRows()},
          {"operands", operandFormatRows()},
          {"code_edits", codeEdits_}};
}

void Engine::reloadCodeEdits() {
  codeEdits_ = readCodeEdits(ownedString(neverd_session_file_path(session_)));
}

Json Engine::readCodeEdits(const std::string &binary) {
  const auto path = utf8Path(binary + ".neverd-code.json");
  if (!fs::exists(path))
    return Json::array();
  if (fs::file_size(path) > 16 * 1024 * 1024)
    throw Error("resource_limit", "Code edit sidecar exceeds 16 MiB");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw Error("load_failed", "Cannot read code edit sidecar");
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  auto rows = parseJson(text, 16 * 1024 * 1024);
  if (!rows.is_array() || rows.size() > 16384)
    throw Error("invalid_request", "Invalid code edit table");
  for (const auto &row : rows)
    CodeEdits::validateRow(row);
  std::set<std::uint64_t> addresses;
  for (const auto &row : rows)
    if (!addresses.insert(parseAddress(row.at("addr").get<std::string>()))
             .second)
      throw Error("invalid_request", "Duplicate function in code edit table");
  return rows;
}

Json Engine::codeEditRow(std::uint64_t address) const {
  for (const auto &row : codeEdits_)
    if (parseAddress(row.at("addr").get<std::string>()) == address)
      return row;
  return nullptr;
}

Json Engine::analysisSnapshot() {
  requireLoaded();
  for (const auto &input : signatureInputs_)
    if (signatureStamp(utf8Path(input.at("path").get<std::string>())) !=
        input.at("stamp"))
      throw Error("input_changed", "Loaded signatures changed; reload them");
  auto options = openOptions_;
  if (loadOptions()) {
    const auto actual = backendJson(loadOptions().json(session_));
    const auto loader = actual.value("loader", std::string());
    if (loader == "binary" || loader == "evm")
      for (const char *key : {"loader", "processor", "base", "offset", "size",
                              "entry", "platform"})
        if (actual.contains(key))
          options[key] = actual.at(key);
  }
  options["path"] = ownedString(neverd_session_file_path(session_));
  options["analysis"] = false;
  options["read_only"] = true;
  return {{"schema", 1},
          {"open", options},
          {"input_sha256", inputHash()},
          {"state", userState()},
          {"signatures", signatureInputs_},
          {"string_options", stringOptions_.empty()
                                 ? Json::object()
                                 : Json::parse(stringOptions_)}};
}

Json Engine::restoreAnalysis(const Json &snapshot) {
  // This operation only initializes a fresh process. It cannot replace the
  // writable owner's project or commit anything to its sidecars/history.
  if (!projectId_.empty() || snapshot.value("schema", 0) != 1 ||
      !snapshot.contains("open") || !snapshot.contains("state"))
    throw Error("invalid_request",
                "Analysis restore needs a fresh worker and a snapshot");
  auto options = snapshot.at("open");
  options["read_only"] = true;
  options["analysis"] = false;
  execute("open", options);
  if (inputHash() != snapshot.at("input_sha256").get<std::string>())
    throw Error("input_changed",
                "The analysis input differs from the open project");
  const auto actual = userState();
  const auto &expected = snapshot.at("state");
  // Committed edits have one writer. Loading them is safe only when they
  // still match the version exported by that writer. A racing edit causes
  // rejection; the GUI retires this replica on the new owner revision.
  for (const char *table :
       {"renames", "functions", "items", "operands", "code_edits"})
    if (actual.at(table) != (std::string_view(table) == "code_edits"
                                 ? expected.value(table, Json::array())
                                 : expected.at(table)))
      throw Error("stale_snapshot",
                  "Project edits changed while opening analysis");
  for (const auto &row : actual.at("annotations"))
    neverd_annotation_set(session_,
                          parseAddress(row.at("addr").get<std::string>()), "");
  for (const auto &row : expected.at("annotations"))
    neverd_annotation_set(
        session_, parseAddress(row.at("addr").get<std::string>()),
        row.at("text").get_ref<const std::string &>().c_str());
  for (const auto &input : snapshot.at("signatures")) {
    const auto path = utf8Path(input.at("path").get<std::string>());
    if (signatureStamp(path) != input.at("stamp"))
      throw Error("input_changed", "Loaded signatures changed; reload them");
    execute("signatures_load", input);
    if (signatureStamp(path) != input.at("stamp"))
      throw Error("input_changed",
                  "Signatures changed during analysis restore");
  }
  execute("string_options", snapshot.at("string_options"));
  return metadata();
}

Json Engine::execute(const std::string &operation, const Json &p) {
  if (operation == "analysis_snapshot")
    return analysisSnapshot();
  if (operation == "analysis_restore")
    return restoreAnalysis(p);
  if (operation == "identify") {
    // How each loader would read a file, before it is opened.
    const auto identify = identifyFunction();
    if (!identify)
      throw Error("unsupported", "This engine cannot identify files");
    const auto path = stringField(p, "path", {}, 32768);
    auto result = backendJson(identify(path.c_str()));
    if (result.contains("error"))
      throw Error("invalid_request", result.value("error", std::string()));
    return result;
  }
  if (operation == "string_encodings") {
    if (stringEncodings().empty())
      throw Error("unsupported", "The engine finds only ASCII strings");
    return {{"items", stringEncodings()}};
  }
  if (operation == "string_options") {
    if (p.contains("encodings") || p.contains("min_length") ||
        p.contains("preferred")) {
      Json options = Json::object();
      const auto knownEncoding = [](const Json &name) {
        const auto known =
            name.is_string()
                ? std::find_if(stringEncodings().begin(),
                               stringEncodings().end(),
                               [&](const Json &encoding) {
                                 return encoding.value("name", std::string()) ==
                                        name.get<std::string>();
                               })
                : stringEncodings().end();
        if (known == stringEncodings().end())
          throw Error(
              "unsupported_encoding",
              "Unknown string encoding: " +
                  (name.is_string() ? name.get<std::string>() : name.dump()));
        return *known;
      };
      if (p.contains("encodings")) {
        const auto &names = p["encodings"];
        if (!names.is_array() || names.empty() || names.size() > 64)
          throw Error("invalid_request",
                      "encodings must be a non-empty array of names");
        for (const auto &name : names)
          knownEncoding(name);
        options["encodings"] = names;
      }
      // The code page that reads a C string first; the engine searches it
      // whether or not the encodings name it.
      if (p.contains("preferred") && !p["preferred"].is_null()) {
        if (!knownEncoding(p["preferred"]).value("legacy", false))
          throw Error("invalid_request",
                      "preferred must name a legacy code page");
        options["preferred"] = p["preferred"];
      }
      if (p.contains("min_length")) {
        const auto &length = p["min_length"];
        if (!length.is_number_integer() || length.get<std::int64_t>() < 1 ||
            length.get<std::int64_t>() > MaxStringMinLength)
          throw Error("invalid_request",
                      "min_length must be an integer from 1 to " +
                          std::to_string(MaxStringMinLength));
        options["min_length"] = length;
      }
      stringOptions_ = options.dump();
      if (listing_)
        listing_->setStringOptions(stringOptions_);
      ++revision_;
    }
    Json current =
        stringOptions_.empty() ? Json::object() : Json::parse(stringOptions_);
    if (!current.contains("encodings")) {
      current["encodings"] = Json::array();
      for (const auto &encoding : stringEncodings())
        if (encoding.value("default", false))
          current["encodings"].push_back(encoding.value("name", std::string()));
    }
    if (!current.contains("min_length"))
      current["min_length"] = DefaultStringMinLength;
    if (!current.contains("preferred"))
      current["preferred"] = nullptr;
    return current;
  }
  if (operation == "contributions")
    return contributions_->listing();
  if (operation == "contribution_register")
    return contributions_->registerFile(stringField(p, "path", {}, 32768));
  if (operation == "contribution_unregister")
    return contributions_->remove(stringField(p, "namespace", {}, 64));
  if (operation == "contribution_execute") {
    requireLoaded();
    for (auto it = p.begin(); it != p.end(); ++it)
      if (it.key() != "id" && it.key() != "address")
        throw Error("invalid_request",
                    "Contribution execution accepts only id and address");
    const auto id = stringField(p, "id", {}, 129);
    const auto address = stringField(
        p, "address", hexAddress(neverd_session_entry_addr(session_)), 18);
    (void)parseAddress(address);
    const auto query = contributions_->query(id, address);
    return {{"contribution_id", id},
            {"operation", query.at("operation")},
            {"result", execute(query.at("operation").get<std::string>(),
                               query.at("payload"))}};
  }
  if (operation == "open") {
    const auto pathText = stringField(p, "path", {}, 32768);
    if (pathText.empty())
      throw Error("invalid_request", "path is required");
    std::error_code ec;
    const auto path = fs::canonical(utf8Path(pathText), ec);
    if (ec || !fs::is_regular_file(path, ec))
      throw Error("load_failed", "Input is not an accessible regular file");
    for (const char *flag : {"read_only", "debug_info", "analysis"})
      if (p.contains(flag) && !p[flag].is_boolean())
        throw Error("invalid_request", std::string(flag) + " must be boolean");
    const bool readOnly = p.value("read_only", false);
    // The loader the load dialog chose when the file names none itself: EVM
    // bytecode, or a binary file with its processor and placement, as the
    // engine's load options spell them.
    std::string chosenLoader;
    if (p.contains("loader")) {
      const std::string loader = stringField(p, "loader", {}, 16);
      if (loader != "binary" && loader != "evm")
        throw Error("invalid_request",
                    "loader is \"binary\", \"evm\" or absent");
      if (!loadOptions())
        throw Error("unsupported", "This engine cannot choose a file's loader");
      Json options{{"loader", loader}};
      if (loader == "binary") {
        options["processor"] = stringField(p, "processor", {}, 32);
        // The platform whose conventions the code follows; the engine reads
        // it from the code when absent or "auto".
        if (p.contains("platform"))
          options["platform"] = stringField(p, "platform", {}, 16);
        for (const char *field : {"base", "offset", "size", "entry"})
          if (p.contains(field))
            options[field] =
                hexAddress(parseAddress(stringField(p, field, {}, 32)));
      }
      chosenLoader = options.dump();
    }
    auto currentPath =
        utf8Path(ownedString(neverd_session_file_path(session_)));
    const bool reuseLock = !readOnly && lock_ && currentPath == path;
    std::unique_ptr<ProjectLock> nextLock;
    if (!readOnly && !reuseLock)
      nextLock = std::make_unique<ProjectLock>(path);
    std::unique_ptr<void, decltype(&neverd_session_destroy)> next(
        neverd_session_create(), neverd_session_destroy);
    if (!next)
      throw Error("engine_unavailable", "Cannot create NeverD session");
    const auto utf8 = path.u8string();
    const std::string loadPath(utf8.begin(), utf8.end());
    const auto fileSize = fs::file_size(path);
    const auto fileTime = fs::last_write_time(path);
    if (loadProgress_)
      neverd_session_set_load_progress(
          next.get(),
          [](void *User, const char *Phase, unsigned long long Done,
             unsigned long long Total, const char *Detail) {
            auto *Sink = static_cast<LoadProgressSink *>(User);
            if (Sink && *Sink)
              (*Sink)(Phase, Done, Total, Detail);
          },
          &loadProgress_);
    // The load dialog's choices: the debug information beside the input, and
    // idle-time analysis.
    if (!p.value("debug_info", true)) {
      using DebugInfoFunction = void (*)(neverd_session_t, int);
      const auto set = engineSymbol<DebugInfoFunction>(
          "neverd_session_set_debug_info_enabled");
      if (!set)
        throw Error("unsupported",
                    "This engine cannot disable debug information");
      set(next.get(), 0);
    }
    if (!chosenLoader.empty() &&
        loadOptions().set(next.get(), chosenLoader.c_str()) != 0)
      throw Error("invalid_request",
                  ownedString(neverd_last_error(next.get())));
    if (!neverd_session_load(next.get(), loadPath.c_str()))
      throw Error("load_failed", ownedString(neverd_last_error(next.get())));
    if (fs::file_size(path) != fileSize ||
        fs::last_write_time(path) != fileTime)
      throw Error(
          "input_changed",
          "Input changed while loading; retry after the writer finishes");
    auto nextCodeEdits =
        readCodeEdits(ownedString(neverd_session_file_path(next.get())));
    neverd_session_destroy(session_);
    session_ = next.release();
    codeEdits_ = std::move(nextCodeEdits);
    if (!reuseLock)
      lock_ = std::move(nextLock);
    readOnly_ = readOnly;
    openOptions_ = p;
    signatureInputs_ = Json::array();
    backgroundAnalysis_ = p.value("analysis", true);
    dirty_ = false;
    analyzed_ = false;
    loadedSize_ = fileSize;
    loadedTime_ = fileTime;
    history_.reset();
    preparedFunction_.reset();
    newListing();
    invalidate();
    ++revision_;
    projectId_ = "project-" + std::to_string(revision_);
    Json warnings = Json::array();
    if (ProjectHistory::recoveryPending(path)) {
      try {
        (void)history();
      } catch (const Error &error) {
        warnings.push_back(std::string(error.what()));
      }
    }
    if (neverd_annotations_load(session_) != 0)
      warnings.push_back("Annotations sidecar could not be loaded");
    if (neverd_renames_load(session_) != 0)
      warnings.push_back("Renames sidecar could not be loaded");
    if (!reloadFunctionEdits())
      warnings.push_back("Function edits sidecar could not be loaded");
    if (!reloadDataItems())
      warnings.push_back("Data items sidecar could not be loaded");
    if (!reloadOperandFormats())
      warnings.push_back("Operand formats sidecar could not be loaded");
    // The chosen loader outlives the session, as the project's other
    // sidecars do, so reopening the file reads it the same way.  The writer
    // lock this session holds covers the write.
    if (!chosenLoader.empty() && !readOnly) {
      try {
        fs::path sidecar = path;
        sidecar += std::string(LoadOptionsSuffix);
        atomicWrite(sidecar, backendJson(loadOptions().json(session_)));
      } catch (const Error &failure) {
        warnings.push_back(std::string("Load options could not be saved: ") +
                           failure.what());
      }
    }
    auto result = metadata();
    for (const auto &diagnostic : result.at("loader_diagnostics"))
      warnings.push_back(diagnostic.value("message", std::string()));
    result["warnings"] = warnings;
    return result;
  }
  requireLoaded();
  if (operation == "signatures_load") {
    const auto path = stringField(p, "path", {}, 32768);
    const auto mode = stringField(p, "mode", "file", 16);
    if (path.empty() || (mode != "file" && mode != "auto"))
      throw Error("invalid_request",
                  "A signature path and file/auto mode are required");
    const auto stamp = signatureStamp(utf8Path(path));
    const int matches =
        mode == "auto" ? neverd_auto_apply_signatures(session_, path.c_str())
                       : neverd_apply_signature_file(session_, path.c_str());
    if (matches < 0)
      throw Error("signature_load_failed", error());
    signatureInputs_.push_back(
        {{"path", path}, {"mode", mode}, {"stamp", stamp}});
    // This changes analysis evidence only. It is also available on a read-only
    // image and never stages a sidecar edit or modifies the binary.
    analyzed_ = false;
    invalidate();
    ++revision_;
    return {{"loaded", true}, {"byte_matches", matches}};
  }
  if (operation == "metadata")
    return metadata();
  if (operation == "history") {
    const auto limit = sizeField(p, "limit", 128, 512);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    return history().listing(
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max()),
        limit);
  }
  if (operation == "history_reset") {
    requireWriter();
    if (dirty_)
      throw Error("unsaved_changes",
                  "Save or reload staged annotations before resetting history");
    history().reset();
    if (!reloadUserState())
      throw Error("reload_failed",
                  "History was reset but user state could not be reloaded",
                  {{"saved", true}});
    invalidate();
    ++revision_;
    return history().listing(0, 128);
  }
  if (operation == "undo" || operation == "redo") {
    requireWriter();
    auto &store = history();
    const bool redo = operation == "redo";
    const auto command = store.next(redo);
    const auto value = command.at(redo ? "after" : "before");
    const auto address = parseAddress(command.at("address").get<std::string>());
    if (command.at("kind") == "annotation") {
      store.advance(redo);
      neverd_annotation_set(session_, address,
                            value.get_ref<const std::string &>().c_str());
      dirty_ = true;
    } else {
      if (dirty_)
        throw Error("unsaved_changes",
                    "Save or reload staged annotations before undoing a "
                    "rename or a function edit");
      // The rows the command changed go back to their values on this side.
      auto state = store.committedState();
      auto &rows = state[ProjectHistory::tableOfKind(
          command.at("kind").get<std::string>())];
      Json changes = command.value("rows", Json::array());
      changes.push_back(command);
      for (const auto &change : changes) {
        const auto at = parseAddress(change.at("address").get<std::string>());
        rows.erase(std::remove_if(
                       rows.begin(), rows.end(),
                       [&](const Json &row) {
                         return parseAddress(
                                    row.at("addr").get<std::string>()) == at;
                       }),
                   rows.end());
        if (const auto &side = change.at(redo ? "after" : "before");
            !side.is_null())
          rows.push_back(side);
      }
      const auto checkpoint = store;
      store.advance(redo);
      try {
        store.persist(state);
      } catch (...) {
        store = checkpoint;
        throw;
      }
      if (!reloadUserState())
        throw Error("reload_failed",
                    "History operation was saved but cannot be reloaded",
                    {{"saved", true}});
    }
    // Reloading operand formats shows them already, and names and data items
    // leave the string scan as it is; a function edit changes what the
    // listing lays out.
    if (const auto kind = command.at("kind");
        kind == "rename" || kind == "item")
      namesChanged();
    else if (kind == "annotation")
      commentsChanged();
    else if (kind != "operand")
      invalidate();
    ++revision_;
    auto result = store.listing(0, 128);
    result["dirty"] = dirty_;
    result["saved"] = !dirty_;
    result["address"] = hexAddress(address);
    return result;
  }
  if (operation == "analyze") {
    analyze();
    return metadata();
  }
  if (operation == "functions" || operation == "resolve" ||
      operation == "disasm") {
    const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
    // Those C ABI paths trigger the entire pipeline for VM architectures.
    // Publish its completion and revision explicitly rather than silently
    // mutating the Session under a revision advertised as not analyzed.
    if (arch == "evm" || arch == "sbf")
      analyze();
  }
  if (operation == "functions") {
    const auto offset =
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = sizeField(p, "limit", 128, 512);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    const auto filter = folded(stringField(p, "filter"));
    const auto sort = stringField(p, "sort", {}, 64);
    const bool descending = p.value("descending", false);
    auto &listingView = listing();
    const auto &rows = listingView.functionRows();
    if (rows.size() > MaxFunctionRows)
      throw Error("budget_exceeded",
                  "The function index exceeds one million entries");
    std::string key = filter;
    key += '\0';
    key += sort;
    key += descending ? "\0d\0" : "\0a\0";
    key += std::to_string(listingView.generation());
    if (key != functionOrderKey_) {
      functionOrder_.clear();
      for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto &row = rows[i];
        // The filter matches the name, the engine's name, the name as it
        // reads demangled or the address.
        const auto matches = [&](const char *field) {
          return folded(row.value(field, std::string())).find(filter) !=
                 std::string::npos;
        };
        if (filter.empty() || matches("name") || matches("engine_name") ||
            matches("demangled_name") ||
            row.value("address", std::string()).find(filter) !=
                std::string::npos)
          functionOrder_.push_back(i);
      }
      if (!sort.empty()) {
        Json keyed = Json::array();
        for (const auto i : functionOrder_) {
          Json item = rows[i];
          item["\x01index"] = i;
          keyed.push_back(std::move(item));
        }
        sortItems(keyed, p);
        functionOrder_.clear();
        for (const auto &item : keyed)
          functionOrder_.push_back(item["\x01index"].get<std::size_t>());
      }
      functionOrderKey_ = std::move(key);
    }
    const auto total = functionOrder_.size();
    Json items = Json::array();
    for (auto i = std::min(offset, total); i < total && items.size() < limit;
         ++i) {
      const auto &row = rows[functionOrder_[i]];
      const auto address = parseAddress(row["address"].get<std::string>());
      // The engine's identity (linkage and demangled names) under the
      // workbench's display name.
      auto item = backendJson(neverd_resolve_addr(session_, address), false);
      if (!item.is_object())
        item = Json::object();
      const bool renamed = row.contains("engine_name");
      for (const auto &[field, value] : row.items())
        if (field != "demangled_name")
          item[field] = value;
      // A name the workbench gave reads demangled, as the engine's do.
      if (renamed || !item.contains("display_name"))
        item["display_name"] = row.value("demangled_name", row["name"]);
      items.push_back(std::move(item));
    }
    const bool complete = offset >= total || items.size() >= total - offset;
    return {{"items", items},
            {"offset", offset},
            {"total", total},
            {"complete", complete},
            {"next_offset",
             complete ? Json(nullptr) : Json(offset + items.size())}};
  }
  if (operation == "resolve") {
    const auto query = stringField(p, "query");
    if (query.empty())
      throw Error("invalid_request", "query is required");
    int index = -1;
    std::uint64_t address = 0;
    if (query.starts_with("0x") || query.starts_with("0X")) {
      address = parseAddress(query);
      index = neverd_func_find_by_addr(session_, address);
      if (index < 0) {
        const auto count = neverd_func_count(session_);
        for (int i = 0; i < count; ++i) {
          const auto entry = neverd_func_entry(session_, i);
          const int size = neverd_func_size(session_, i);
          if (entry <= address && size > 0 &&
              address - entry < static_cast<std::uint64_t>(size)) {
            index = i;
            break;
          }
        }
      }
    } else {
      index = neverd_func_find_by_name(session_, query.c_str());
      if (index >= 0) {
        address = neverd_func_entry(session_, index);
      } else if (auto named = listing().resolveName(query)) {
        address = *named;
        index = neverd_func_find_by_addr(session_, address);
      } else {
        throw Error("not_found", "No function or name matches that symbol");
      }
    }
    auto result =
        index < 0
            ? Json::object()
            : backendJson(neverd_resolve_addr(
                              session_, neverd_func_entry(session_, index)),
                          false);
    result["address"] = hexAddress(address);
    result["function_address"] =
        index < 0 ? Json(nullptr)
                  : Json(hexAddress(neverd_func_entry(session_, index)));
    std::string name = index < 0
                           ? std::string()
                           : ownedString(neverd_func_name(session_, index));
    // The workbench name of a function the engine leaves generic.
    if (const auto &aliases = listing().functionAliases();
        aliases.contains(name))
      name = aliases.at(name);
    result["name"] = std::move(name);
    // The name of the address itself, which a rename starts from.
    result["address_name"] = listing().nameAt(address);
    result["comment"] = ownedString(neverd_annotation_get(session_, address));
    result["import"] = listing().isImport(address);
    return result;
  }
  if (operation == "strings")
    return listing().strings(p);
  if (operation == "segments") {
    auto items = backendJson(neverd_segments_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("va");
      item.erase("va");
    }
    return page(items, p, {"name"});
  }
  if (operation == "listing")
    return listing().page(p);
  if (operation == "string_references")
    return listing().stringReferences(p);
  if (operation == "overview")
    return listing().overview(p);
  if (operation == "names") {
    auto items = listing().names();
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "regions") {
    auto items = listing().regions();
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "imports") {
    Json items = Json::array();
    for (const auto &row : backendJson(neverd_imports_json(session_)))
      if (row.contains("iat_addr") &&
          parseAddress(row["iat_addr"].get<std::string>()))
        items.push_back({{"address", row["iat_addr"]},
                         {"name", row.value("name", std::string())},
                         {"module", row.value("module", std::string())},
                         {"ordinal", row.value("ordinal", 0)}});
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "exports") {
    Json items = Json::array();
    for (const auto &row : backendJson(neverd_exports_json(session_)))
      items.push_back({{"address", row.value("addr", std::string("0x0"))},
                       {"name", row.value("name", std::string())},
                       {"ordinal", row.value("ordinal", 0)},
                       {"kind", "export"}});
    for (const auto &row : backendJson(neverd_entrypoints_json(session_)))
      items.push_back({{"address", row.value("addr", std::string("0x0"))},
                       {"name", row.value("name", std::string())},
                       {"ordinal", nullptr},
                       {"kind", row.value("type", std::string("entry"))}});
    sortItems(items, p);
    return page(items, p, {"name"});
  }
  if (operation == "search") {
    const auto kind = stringField(p, "kind", "text", 16);
    const auto pattern = stringField(p, "pattern", {}, 4096);
    const auto limit = sizeField(p, "limit", 256, 4096);
    if (pattern.empty() || !limit)
      throw Error("invalid_request", "A search pattern and limit are required");
    Json hits;
    if (kind == "bytes") {
      std::vector<unsigned char> bytes;
      std::string digits;
      for (char c : pattern)
        if (std::isxdigit(static_cast<unsigned char>(c)))
          digits += c;
        else if (c != ' ' && c != '\t')
          throw Error("invalid_request",
                      "Byte patterns contain hexadecimal pairs");
      if (digits.empty() || digits.size() % 2)
        throw Error("invalid_request",
                    "Byte patterns contain hexadecimal pairs");
      for (std::size_t i = 0; i < digits.size(); i += 2)
        bytes.push_back(static_cast<unsigned char>(
            std::stoul(digits.substr(i, 2), nullptr, 16)));
      hits = backendJson(neverd_search_bytes(session_, bytes.data(),
                                             static_cast<int>(bytes.size()),
                                             static_cast<int>(limit)));
    } else if (kind == "text") {
      hits = backendJson(neverd_search_string(
          session_, pattern.c_str(), p.value("case_sensitive", false) ? 1 : 0,
          static_cast<int>(limit)));
    } else {
      throw Error("invalid_request", "Search kind must be bytes or text");
    }
    Json items = Json::array();
    for (auto &hit : hits) {
      hit["address"] = hit.value("addr", std::string("0x0"));
      hit.erase("addr");
      items.push_back(std::move(hit));
    }
    return {{"items", std::move(items)}, {"complete", true}};
  }
  if (operation == "annotations") {
    auto items = backendJson(neverd_annotations_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("addr");
      item.erase("addr");
    }
    return page(items, p, {"text"});
  }
  if (operation == "code_edit") {
    requireWriter();
    if (dirty_)
      throw Error("unsaved_changes", "Save staged comments before a code edit");
    const auto address = parseAddress(stringField(p, "address"));
    auto &store = history();
    const auto before = codeEditRow(address);
    const auto after = CodeEdits::change(before, p);
    const auto document = namedView(address, stringField(p, "representation"));
    if (!document)
      throw Error("unsupported",
                  "The engine does not expose editable source targets");
    CodeEdits::validateChange(document->value, p);
    if (before == after)
      return {{"saved", false}, {"address", hexAddress(address)}};
    const auto checkpoint = store;
    auto state = store.committedState();
    auto &rows = state["code_edits"];
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const Json &row) {
                                return parseAddress(
                                           row.at("addr").get<std::string>()) ==
                                       address;
                              }),
               rows.end());
    rows.push_back(after);
    try {
      store.stage({{"kind", "code"},
                   {"address", hexAddress(address)},
                   {"before", before},
                   {"after", after}});
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      throw;
    }
    codeEdits_ = std::move(rows);
    invalidate();
    ++revision_;
    return {{"saved", true}, {"address", hexAddress(address)}};
  }
  if (operation == "save") {
    requireWriter();
    auto &store = history();
    auto state = store.committedState();
    state["annotations"] = backendJson(neverd_annotations_json(session_));
    store.persist(state);
    dirty_ = false;
    return {{"saved", true}, {"dirty", false}};
  }
  if (operation == "reload") {
    reloadCodeEdits();
    const bool annotationsLoaded = neverd_annotations_load(session_) == 0;
    const bool renamesLoaded = neverd_renames_load(session_) == 0;
    const bool functionsLoaded = reloadFunctionEdits();
    const bool itemsLoaded = reloadDataItems();
    const bool operandsLoaded = reloadOperandFormats();
    // Each existing API can change its own table independently. Publish a new
    // revision even on partial failure so clients cannot retain stale pages.
    invalidate();
    ++revision_;
    history_.reset();
    if (!annotationsLoaded || !renamesLoaded || !functionsLoaded ||
        !itemsLoaded || !operandsLoaded)
      throw Error("load_failed",
                  "Could not reload the annotations, renames, function edits, "
                  "data items or operand formats sidecar",
                  {{"annotations_loaded", annotationsLoaded},
                   {"renames_loaded", renamesLoaded},
                   {"functions_loaded", functionsLoaded},
                   {"items_loaded", itemsLoaded},
                   {"operands_loaded", operandsLoaded},
                   {"state_changed", true}});
    dirty_ = false;
    return metadata();
  }
  if (operation != "disasm" && operation != "bytes" &&
      operation != "annotation_set" && operation != "rename" &&
      operation != "function_create" && operation != "function_delete" &&
      operation != "item_define" && operation != "operand_format" &&
      operation != "decompile" && operation != "cfg" &&
      operation != "cfg_summary" && operation != "cfg_viewport" &&
      operation != "xrefs")
    throw Error("unsupported", "Unknown worker operation: " + operation);
  const auto address = parseAddress(stringField(p, "address"));
  if (operation == "disasm") {
    const auto limit = sizeField(p, "limit", 128, 512);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    auto items = backendJson(
        neverd_disasm_json(session_, address, static_cast<int>(limit)), true);
    for (auto &item : items) {
      item["address"] = item.at("addr");
      item["operands"] = item.value("op_str", "");
      item["comment"] = ownedString(neverd_annotation_get(
          session_, parseAddress(item.at("addr").get<std::string>())));
      item.erase("addr");
      item.erase("op_str");
    }
    Json next = nullptr;
    if (!items.empty()) {
      const auto last =
          parseAddress(items.back().at("address").get<std::string>());
      const auto size = items.back().at("size").get<std::uint64_t>();
      if (size && size <= std::numeric_limits<std::uint64_t>::max() - last &&
          items.size() == limit)
        next = hexAddress(last + size);
    }
    return {{"items", items},
            {"address", hexAddress(address)},
            {"next_address", next},
            {"complete", next.is_null()}};
  }
  if (operation == "bytes") {
    auto size = sizeField(p, "size", 256, 65536);
    if (!size)
      throw Error("invalid_request", "size must be at least 1");
    if (size - 1 > std::numeric_limits<std::uint64_t>::max() - address)
      throw Error("invalid_address", "Byte range overflows the address space");
    std::vector<unsigned char> buffer(size);
    const int count = neverd_read_bytes(session_, address, buffer.data(),
                                        static_cast<int>(size));
    std::string data;
    constexpr char digits[] = "0123456789abcdef";
    for (int i = 0; i < count; ++i) {
      data += digits[buffer[i] >> 4];
      data += digits[buffer[i] & 15];
    }
    // The bytes as text in an encoding, one cell per byte, for a hex view.
    Json cells;
    if (p.contains("text_encoding")) {
      const auto encoding = stringField(p, "text_encoding", {}, 64);
      static const auto decode =
          engineSymbol<DecodeTextFunction>("neverd_decode_text_json");
      if (!decode)
        throw Error("unsupported", "The engine cannot decode text");
      const char *raw = decode(buffer.data(), count, encoding.c_str());
      if (!raw)
        throw Error("unsupported_encoding",
                    "Unknown text encoding: " + encoding);
      auto decoded = backendJson(raw);
      if (!decoded.is_object() || !decoded["cells"].is_array() ||
          decoded["cells"].size() != static_cast<std::size_t>(count))
        throw Error("engine_error", "Engine returned malformed text cells");
      cells = std::move(decoded["cells"]);
    }
    Json result = {
        {"address", hexAddress(address)},
        {"encoding", "hex"},
        {"data", data},
        {"bytes_read", count},
        {"requested_size", size},
        {"mapping_status", count == 0 ? "unmapped_or_unmaterialized"
                           : count < static_cast<int>(size) ? "partial"
                                                            : "mapped"},
        {"next_address",
         count > 0 && static_cast<std::uint64_t>(count) <=
                          std::numeric_limits<std::uint64_t>::max() - address
             ? Json(hexAddress(address + count))
             : Json(nullptr)}};
    if (!cells.is_null())
      result["cells"] = std::move(cells);
    return result;
  }
  if (operation == "annotation_set") {
    requireWriter();
    auto text = stringField(p, "text", {}, 65536);
    auto &store = history();
    store.stage(
        {{"kind", "annotation"},
         {"address", hexAddress(address)},
         {"before", ownedString(neverd_annotation_get(session_, address))},
         {"after", text}});
    neverd_annotation_set(session_, address, text.c_str());
    dirty_ = true;
    commentsChanged();
    ++revision_;
    return {{"address", hexAddress(address)},
            {"text", text},
            {"dirty", true},
            {"saved", false}};
  }
  if (operation == "rename") {
    requireWriter();
    if (dirty_)
      throw Error("unsaved_changes",
                  "Save or reload staged annotations before renaming");
    const auto name = stringField(p, "name", {}, 4096);
    if (name.empty() || std::any_of(name.begin(), name.end(), [](char c) {
          return static_cast<unsigned char>(c) <= ' ' || c == 0x7f;
        }))
      throw Error(
          "invalid_request",
          "A name is not empty and has no spaces or control characters");
    // A name in an automatic form reads as an address, and a name leads to
    // one address.
    if (parseDummyName(name))
      throw Error("invalid_request",
                  name + " is an automatic name; choose another");
    if (const auto other = listing().resolveName(name);
        other && *other != address)
      throw Error("invalid_request",
                  "The name " + name + " is used at " + hexAddress(*other));
    const int index = neverd_func_find_by_addr(session_, address);
    auto &store = history();
    auto state = store.committedState();
    auto &renames = state["renames"];
    Json before = nullptr;
    bool found = false;
    for (auto &item : renames)
      if (parseAddress(item.at("addr").get<std::string>()) == address) {
        before = item;
        item["renamed"] = name;
        found = true;
      }
    if (!found)
      renames.push_back({{"addr", hexAddress(address)},
                         {"original", index >= 0 ? ownedString(neverd_func_name(
                                                       session_, index))
                                                 : listing().nameAt(address)},
                         {"renamed", name}});
    Json after;
    for (const auto &item : renames)
      if (parseAddress(item.at("addr").get<std::string>()) == address)
        after = item;
    const auto checkpoint = store;
    store.stage({{"kind", "rename"},
                 {"address", hexAddress(address)},
                 {"before", before},
                 {"after", after}});
    try {
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      throw;
    }
    if (neverd_renames_load(session_) != 0)
      throw Error("reload_failed", "Rename was saved but could not be reloaded",
                  {{"saved", true}});
    namesChanged();
    ++revision_;
    return {{"address", hexAddress(address)}, {"name", name}, {"saved", true}};
  }
  if (operation == "function_create" || operation == "function_delete") {
    requireWriter();
    const auto &edits = functionEdits();
    if (!edits)
      throw Error("unsupported", "This engine keeps no function edits");
    if (dirty_)
      throw Error("unsaved_changes", "Save or reload staged annotations "
                                     "before creating or deleting a function");
    auto &store = history();
    const bool create = operation == "function_create";
    // The engine checks the edit; the rows it then holds are the new table.
    if ((create ? edits.create : edits.remove)(session_, address) != 0)
      throw Error("invalid_request", error());
    const auto rowAt = [&](const Json &rows) -> Json {
      for (const auto &row : rows)
        if (parseAddress(row.at("addr").get<std::string>()) == address)
          return row;
      return nullptr;
    };
    auto state = store.committedState();
    const Json before = rowAt(state.at("functions"));
    state["functions"] = functionEditRows();
    const Json after = rowAt(state.at("functions"));
    const auto checkpoint = store;
    store.stage({{"kind", "function"},
                 {"address", hexAddress(address)},
                 {"before", before},
                 {"after", after}});
    try {
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      // The sidecar still holds the edits before this one.
      (void)edits.load(session_);
      functionsChanged();
      throw;
    }
    functionsChanged();
    ++revision_;
    return {
        {"address", hexAddress(address)}, {"created", create}, {"saved", true}};
  }
  if (operation == "item_define") {
    requireWriter();
    const auto &items = dataItems();
    if (!items)
      throw Error("unsupported", "This engine keeps no data items");
    if (dirty_)
      throw Error("unsaved_changes", "Save or reload staged annotations "
                                     "before defining data");
    const auto action = stringField(p, "action", {}, 16);
    const Json at = listing().item(address);
    if (at.is_null())
      throw Error("invalid_request", "No image byte at " + hexAddress(address));
    const auto kind = at.at("kind").get<std::string>();
    if (kind == "instruction" && action == "code")
      return {{"address", at.at("start")},
              {"kind", CodeItemKind},
              {"size", at.at("size")},
              {"saved", false}};
    if (kind == "instruction" &&
        at.value("user", std::string()) != CodeItemKind)
      throw Error("invalid_request",
                  "Code belongs to its function; delete the function to "
                  "define data in its bytes");
    // The item the cursor is in is the one the action changes.
    const auto start = action == "code"
                           ? address
                           : parseAddress(at.at("start").get<std::string>());
    const auto user = at.value("user", std::string());
    Json row;
    std::vector<std::pair<std::uint64_t, Json>> code;
    std::uint64_t size = 0;
    if (action == "code") {
      using DisasmEx =
          const char *(*)(neverd_session_t, neverd_va_t, int, unsigned);
      const auto decode = engineSymbol<DisasmEx>("neverd_disasm_json_ex");
      if (!decode)
        throw Error("unsupported", "This engine cannot define native code");
      // Decode one basic block without following its successors or inventing
      // a function. Known instructions are a boundary, and unmodelled control
      // flow never invites guessing what bytes follow it.
      constexpr std::size_t MaxInstructions = 256;
      auto cursor = start;
      for (;;) {
        if (!code.empty()) {
          const auto next = listing().item(cursor);
          if (next.is_null() || next.at("kind") == "instruction")
            break;
        }
        if (code.size() == MaxInstructions)
          throw Error("resource_limit",
                      "Code definition exceeds 256 instructions");
        const auto decoded =
            backendJson(decode(session_, cursor, 1, NEVERD_DISASM_FLOW), true);
        if (!decoded.is_array() || decoded.size() != 1 ||
            parseAddress(decoded[0].at("addr").get<std::string>()) != cursor)
          throw Error("invalid_request",
                      "Invalid or truncated instruction at " +
                          hexAddress(cursor));
        const auto &instruction = decoded[0];
        const auto bytes = instruction.at("size").get<std::uint64_t>();
        if (!bytes || bytes > 16 ||
            cursor > std::numeric_limits<std::uint64_t>::max() - bytes)
          throw Error("invalid_request", "Invalid instruction size");
        code.push_back({cursor, {{"kind", CodeItemKind}, {"size", bytes}}});
        size += bytes;
        cursor += bytes;
        const auto flow = instruction.value("flow", std::string());
        if (!flow.empty() && flow != "call" && flow != "icall")
          break;
      }
    } else if (action == "data") {
      // The data carousel: byte, word, dword, qword, then byte again.
      static constexpr std::array<std::uint64_t, 4> Carousel = {1, 2, 4, 8};
      size = sizeField(p, "size", 0, 8);
      if (size &&
          std::find(Carousel.begin(), Carousel.end(), size) == Carousel.end())
        throw Error("invalid_request", "A value is 1, 2, 4 or 8 bytes");
      if (!size) {
        const auto current =
            kind == "data" ? at.at("size").get<std::uint64_t>() : 0;
        const auto next = std::find(Carousel.begin(), Carousel.end(), current);
        size = next == Carousel.end() || next + 1 == Carousel.end()
                   ? Carousel.front()
                   : *(next + 1);
      }
      row = {{"kind", std::string(sizeKeywordOf(size))}};
    } else if (action == "string") {
      // The string the scan reads there, from its first character.
      Json options = listing().stringOptions().empty()
                         ? Json::object()
                         : parseJson(listing().stringOptions());
      options.erase("min_length");
      const char *raw = items.stringAt(
          session_, start, options.empty() ? nullptr : options.dump().c_str());
      if (!raw)
        throw Error("invalid_request", "No string starts at " +
                                           hexAddress(start) + ": " + error());
      const auto string = backendJson(raw);
      size = string.at("length").get<std::uint64_t>() +
             string.at("unit").get<std::uint64_t>();
      row = {{"kind", StringItemKind},
             {"encoding", string.at("encoding")},
             {"size", size}};
    } else if (action == "undefine") {
      // Bytes the listing already shows one by one stay as they are.
      if (user == UndefinedItemKind || kind == "byte" ||
          kind == "uninitialized" || kind == "align")
        return {{"address", hexAddress(start)},
                {"kind", UndefinedItemKind},
                {"size", at.at("size")},
                {"saved", false}};
      size = at.at("size").get<std::uint64_t>();
      row = {{"kind", UndefinedItemKind}, {"size", size}};
    } else {
      throw Error("invalid_request",
                  "action is \"code\", \"data\", \"string\" or \"undefine\"");
    }
    auto &store = history();
    auto state = store.committedState();
    // The engine checks the item against the image and the other items;
    // undefined bytes it covers stay undefined around it.
    if (code.empty())
      code.push_back({start, std::move(row)});
    try {
      for (const auto &[entry, definition] : code)
        if (items.set(session_, entry, definition.dump().c_str()) != 0)
          throw Error("invalid_request", error());
    } catch (...) {
      (void)items.load(session_);
      namesChanged();
      throw;
    }
    // One history step holds every row the edit changed, the item's first.
    std::map<std::uint64_t, std::pair<Json, Json>> changed;
    for (const auto &item : state.at("items"))
      changed[parseAddress(item.at("addr").get<std::string>())].first = item;
    state["items"] = dataItemRows();
    for (const auto &item : state.at("items"))
      changed[parseAddress(item.at("addr").get<std::string>())].second = item;
    const Json after = changed[start].second;
    Json command{{"kind", "item"},
                 {"address", hexAddress(start)},
                 {"before", changed[start].first},
                 {"after", after}};
    for (const auto &[itemStart, sides] : changed)
      if (itemStart != start && sides.first != sides.second)
        command["rows"].push_back({{"address", hexAddress(itemStart)},
                                   {"before", sides.first},
                                   {"after", sides.second}});
    const auto checkpoint = store;
    try {
      store.stage(std::move(command));
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      // The sidecar still holds the items before this one.
      (void)items.load(session_);
      namesChanged();
      throw;
    }
    namesChanged();
    ++revision_;
    return {{"address", hexAddress(start)},
            {"kind", after.at("kind")},
            {"size", action == "code" ? Json(size) : after.at("size")},
            {"saved", true}};
  }
  if (operation == "operand_format") {
    requireWriter();
    const auto &formats = operandFormats();
    if (!formats)
      throw Error("unsupported", "This engine keeps no operand formats");
    if (dirty_)
      throw Error("unsaved_changes", "Save or reload staged annotations "
                                     "before formatting an operand");
    if (!listing().showsNumberFormats())
      throw Error("unsupported",
                  "Operand types are shown for x86 instructions only");
    // Formats belong to the instruction that starts at the address, and
    // change its operands that are numbers: the one asked for if it is one,
    // else the last.
    const auto numbers = listing().numberOperands(address);
    if (!numbers)
      throw Error("invalid_request",
                  "No instruction starts at " + hexAddress(address));
    std::optional<std::size_t> chosen;
    if (p.contains("operand"))
      if (const auto asked = sizeField(p, "operand", 0, 7);
          asked < numbers->size() && (*numbers)[asked])
        chosen = asked;
    for (std::size_t i = numbers->size(); !chosen && i-- > 0;)
      if ((*numbers)[i])
        chosen = i;
    if (!chosen)
      throw Error("invalid_request", "The instruction at " +
                                         hexAddress(address) +
                                         " has no number to format");
    const auto operand = *chosen;
    const auto action = stringField(p, "action", {}, 16);
    auto &store = history();
    auto state = store.committedState();
    const auto rowAt = [&](const Json &rows) -> Json {
      for (const auto &row : rows)
        if (parseAddress(row.at("addr").get<std::string>()) == address)
          return row;
      return nullptr;
    };
    const Json before = rowAt(state.at("operands"));
    // The operand's format so far, then the action's change to it: a base,
    // the sign, the bits, or the listing's own number again.
    const std::string number(numberBaseName(NumberBase::Number));
    Json next = {{"base", number}, {"negate", false}, {"invert", false}};
    if (!before.is_null())
      for (const auto &entry : before.at("operands"))
        if (entry.at("operand").get<std::size_t>() == operand)
          next = {{"base", entry.at("base")},
                  {"negate", entry.value("negate", false)},
                  {"invert", entry.value("invert", false)}};
    if (action == "negate")
      next["negate"] = !next.at("negate").get<bool>();
    else if (action == "invert")
      next["invert"] = !next.at("invert").get<bool>();
    else if (action == number)
      next = {{"base", number}, {"negate", false}, {"invert", false}};
    else if (parseNumberBase(action))
      next["base"] = action;
    else
      throw Error("invalid_request", "action is a base of OperandFormats.def, "
                                     "\"negate\" or \"invert\"");
    if (formats.set(session_, address, static_cast<int>(operand),
                    next.dump().c_str()) != 0)
      throw Error("invalid_request", error());
    state["operands"] = operandFormatRows();
    const Json after = rowAt(state.at("operands"));
    if (before == after)
      return {{"address", hexAddress(address)},
              {"operand", operand},
              {"format", next},
              {"saved", false}};
    const auto checkpoint = store;
    try {
      store.stage({{"kind", "operand"},
                   {"address", hexAddress(address)},
                   {"before", before},
                   {"after", after}});
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      // The sidecar still holds the formats before this one.
      (void)formats.load(session_);
      operandFormatsChanged();
      throw;
    }
    operandFormatsChanged();
    ++revision_;
    return {{"address", hexAddress(address)},
            {"operand", operand},
            {"format", next},
            {"saved", true}};
  }
  if (operation == "decompile") {
    const auto representation = stringField(p, "representation", "c", 16);
    // `source` reads in the function's own language; the named dialects spell
    // the HighC source in one, through the engine's view alone.
    const bool spelled = representation == "source" ||
                         representation == "cpp" || representation == "rust" ||
                         representation == "go";
    if (representation != "c" && representation != "llvmc" &&
        representation != "low" && representation != "med" &&
        representation != "high" && representation != "llvm" && !spelled)
      throw Error("unsupported", "Unknown code representation");
    const auto offset =
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = sizeField(p, "limit", 512, 2048);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    std::string mappingStatus = "unsupported_representation";
    // Why the engine offers no page, such as Rust asked of a C program.
    std::string reason;
    if (representation == "low" || representation == "med" ||
        representation == "c" || representation == "llvmc" || spelled) {
      if (auto named = namedViewPage(address, representation, offset, limit))
        return *named;
      prepareFunction(address);
      if (const auto view = irViewFunction()) {
        auto result = backendJson(
            view(session_, address, representation.c_str(), offset, limit),
            true);
        mappingStatus =
            result.value("mapping_status", std::string("unavailable"));
        if (result.contains("reason") && result["reason"].is_string())
          reason = result["reason"].get<std::string>();
        if (result.contains("text")) {
          if (!result["text"].is_string() ||
              result["text"].get_ref<const std::string &>().size() >
                  2 * 1024 * 1024 ||
              !result.contains("rows") || !result["rows"].is_array() ||
              result["rows"].size() > limit)
            throw Error("invalid_engine_result",
                        "Invalid bounded IR view page");
          result["project_id"] = projectId_;
          result["revision"] = revision();
          return result;
        }
      } else
        mappingStatus = "unavailable_engine_api";
    }
    if (spelled)
      throw Error("unavailable", !reason.empty() ? reason
                                 : error().empty()
                                     ? "This representation is unavailable"
                                     : error());
    prepareFunction(address);
    const auto key = hexAddress(address) + ":" + representation;
    if (textKey_ != key) {
      textKey_.clear();
      const char *result =
          representation == "c"       ? neverd_decompile(session_, address)
          : representation == "llvmc" ? neverd_decompile_llvm(session_, address)
          : representation == "low"   ? neverd_ir_low(session_, address)
          : representation == "med"   ? neverd_ir_med(session_, address)
          : representation == "high"  ? neverd_ir_high(session_, address)
                                      : neverd_ir_llvm(session_, address);
      textCache_ = ownedString(result);
      if (textCache_.empty())
        throw Error("unavailable", error().empty()
                                       ? "This representation is unavailable"
                                       : error());
      if (const auto &aliases = listing().functionAliases(); !aliases.empty()) {
        std::vector<std::pair<std::size_t, std::ptrdiff_t>> shift;
        textCache_ = renameIdentifiers(textCache_, aliases, shift);
      }
      textLines_.clear();
      textLines_.push_back(0);
      for (std::size_t i = 0; i < textCache_.size(); ++i)
        if (textCache_[i] == '\n' && i + 1 < textCache_.size())
          textLines_.push_back(i + 1);
      textKey_ = key;
    }
    const auto start = std::min(offset, textLines_.size());
    const auto end = start + std::min(limit, textLines_.size() - start);
    const auto startByte =
        start < textLines_.size() ? textLines_[start] : textCache_.size();
    const auto endByte =
        end < textLines_.size() ? textLines_[end] : textCache_.size();
    if (endByte - startByte > 2 * 1024 * 1024)
      throw Error("budget_exceeded",
                  "Code page exceeds 2 MiB; request fewer lines");
    return {
        {"address", hexAddress(address)},
        {"representation", representation},
        {"text", textCache_.substr(startByte, endByte - startByte)},
        {"offset", offset},
        {"total_lines", textLines_.size()},
        {"complete", end == textLines_.size()},
        {"next_offset", end == textLines_.size() ? Json(nullptr) : Json(end)},
        {"mapping_status", mappingStatus},
        {"provenance_complete", false},
        {"rows", Json::array()},
        {"project_id", projectId_},
        {"revision", revision()}};
  }
  if (operation == "cfg_summary") {
    const auto key = hexAddress(address);
    // Client text metrics size each node to its formatted listing rows.
    GraphMetrics metrics;
    if (const auto it = p.find("metrics"); it != p.end() && it->is_object()) {
      const auto number = [&](const char *field) {
        const auto value = it->find(field);
        if (value == it->end())
          return 0.0;
        if (!value->is_number() || !std::isfinite(value->get<double>()) ||
            value->get<double>() < 0 || value->get<double>() > 1000)
          throw Error("invalid_request",
                      std::string("metrics.") + field + " is out of range");
        return value->get<double>();
      };
      metrics.charWidth = number("char_width");
      metrics.lineHeight = number("line_height");
      metrics.padding = number("padding");
      metrics.titleHeight = number("title_height");
    }
    const auto metricsKey = std::to_string(metrics.charWidth) + "/" +
                            std::to_string(metrics.lineHeight) + "/" +
                            std::to_string(metrics.padding) + "/" +
                            std::to_string(metrics.titleHeight);
    for (auto it = graphs_.begin(); it != graphs_.end(); ++it)
      if (it->address == key && it->metrics == metricsKey) {
        graphs_.splice(graphs_.begin(), graphs_, it);
        return graphs_.front().snapshot->summary();
      }
    const auto layout =
        projectId_ + ":" + revision() + ":" + key + ":" + metricsKey;
    auto snapshot = std::make_unique<GraphSnapshot>(
        functionGraph(address), key, layout, metrics,
        metrics.valid()
            ? GraphRows([this](std::uint64_t start, std::uint64_t end) {
                return listing().blockLines(start, end);
              })
            : GraphRows());
    graphs_.push_front({key, metricsKey, layout, std::move(snapshot)});
    if (graphs_.size() > MaxGraphs)
      graphs_.pop_back();
    return graphs_.front().snapshot->summary();
  }
  if (operation == "cfg_viewport") {
    // The layout the client names, else the function's newest.
    const auto key = hexAddress(address);
    const auto layout = stringField(p, "layout_revision", {}, 256);
    for (const auto &graph : graphs_)
      if (graph.address == key && (layout.empty() || graph.revision == layout))
        return graph.snapshot->viewport(p);
    throw Error("stale_layout",
                "No matching graph snapshot; request cfg_summary first");
  }
  if (operation == "cfg") {
    auto result = functionGraph(address);
    if (result.value("nodes", Json::array()).size() > 500 ||
        result.value("edges", Json::array()).size() > 2000)
      throw Error("budget_exceeded",
                  "Graph exceeds the preview budget of 500 nodes / 2000 edges; "
                  "no partial graph was published");
    for (auto &node : result["nodes"]) {
      if (!node["id"].is_string())
        node["id"] = node["id"].dump();
      node["address"] = node.at("start");
      node["label"] = node.at("start");
      node["lines"] = node.at("disasm");
    }
    for (auto &edge : result["edges"]) {
      if (!edge["from"].is_string())
        edge["from"] = edge["from"].dump();
      if (!edge["to"].is_null() && !edge["to"].is_string())
        edge["to"] = edge["to"].dump();
    }
    result["complete"] = true;
    return result;
  }
  if (operation == "xrefs") {
    const auto direction = stringField(p, "direction", "to", 8);
    if (direction != "to" && direction != "from")
      throw Error("invalid_request", "direction must be to or from");
    if (stringField(p, "source", "direct", 16) != "ir")
      return listing().references(address, p);
    analyze();
    const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
    if (arch == "evm" || arch == "sbf")
      throw Error("unsupported",
                  "The existing xref API does not expose architecture-specific "
                  "references for this image");
    auto items = backendJson(direction == "to"
                                 ? neverd_xrefs_to_json(session_, address)
                                 : neverd_xrefs_from_json(session_, address),
                             true);
    for (auto &item : items) {
      item["address"] = item.at(direction == "to" ? "from" : "to");
      item["function"] = item.value("func", "");
      item["kind"] = "IR constant reference";
    }
    return page(items, p);
  }
  throw Error("unsupported", "Unknown worker operation: " + operation);
}
} // namespace neverd::worker
