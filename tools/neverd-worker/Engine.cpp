#include "Engine.h"

#include "Contributions.h"
#include "GraphSnapshot.h"
#include "ProjectHistory.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/support/ProjectWriteLock.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#ifdef _WIN32
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
using IRViewFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                       const char *, std::size_t, std::size_t);
IRViewFunction irViewFunction() {
  // Additive C ABI capability: an older matching engine can still run the GUI.
  // Resolve only from the already linked engine, never an arbitrary user path.
  static const auto function = []() -> IRViewFunction {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&neverd_session_create),
                            &module))
      return nullptr;
    return reinterpret_cast<IRViewFunction>(
        GetProcAddress(module, "neverd_ir_view_json"));
#else
    return reinterpret_cast<IRViewFunction>(
        dlsym(RTLD_DEFAULT, "neverd_ir_view_json"));
#endif
  }();
  return function;
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
std::string folded(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}
Json page(const Json &items, const Json &payload,
          const char *searchField = nullptr) {
  const auto offset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  const auto limit = sizeField(payload, "limit", 128, 512);
  if (!limit)
    throw Error("invalid_request", "limit must be at least 1");
  const auto filter = folded(stringField(payload, "filter"));
  Json selected = Json::array();
  std::size_t total = 0;
  for (const auto &item : items) {
    if (searchField && !filter.empty() &&
        folded(item.value(searchField, std::string())).find(filter) ==
            std::string::npos)
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
void Engine::invalidate() {
  graph_.reset();
  stringsCache_ = nullptr;
  textKey_.clear();
  textCache_.clear();
  textLines_.clear();
  haveFilter_ = false;
  filteredFunctions_.clear();
}
void Engine::analyze() {
  if (analyzed_)
    return;
  if (!neverd_session_analyze(session_))
    throw Error("analysis_failed", error());
  analyzed_ = true;
  invalidate();
  ++revision_;
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
    // Hashing lives behind the C ABI; the worker gains no LLVM linkage.
    const auto dashboard = backendJson(neverd_dashboard_json(session_));
    const auto hash = dashboard.value("hashes", Json::object())
                          .value("sha256", std::string());
    history_ = std::make_unique<ProjectHistory>(
        utf8Path(ownedString(neverd_session_file_path(session_))), hash,
        version(), readOnly_, atomicWrite);
    if (history_->recovered()) {
      if (neverd_annotations_load(session_) != 0 ||
          neverd_renames_load(session_) != 0)
        throw Error("recovery_required",
                    "Recovered files could not be reloaded into the Session");
      dirty_ = false;
      invalidate();
      ++revision_;
    }
    history_->verifyLoadedState(
        {{"annotations", backendJson(neverd_annotations_json(session_))},
         {"renames", backendJson(neverd_renames_json(session_))}});
  }
  return *history_;
}
Json Engine::metadata() const {
  requireLoaded();
  const auto arch = ownedString(neverd_session_arch_name(session_));
  const bool deferred =
      !analyzed_ && (folded(arch) == "evm" || folded(arch) == "sbf");
  return {{"path", ownedString(neverd_session_file_path(session_))},
          {"architecture", arch},
          {"format", ownedString(neverd_session_format_name(session_))},
          {"bitness", neverd_session_bitness(session_)},
          {"file_size", std::to_string(neverd_session_file_size(session_))},
          {"base_address", hexAddress(neverd_session_base_addr(session_))},
          {"entry_address", hexAddress(neverd_session_entry_addr(session_))},
          {"function_count",
           deferred ? Json(nullptr) : Json(neverd_func_count(session_))},
          {"segment_count", neverd_session_segment_count(session_)},
          {"section_count", neverd_session_section_count(session_)},
          {"import_count", neverd_session_import_count(session_)},
          {"export_count", neverd_session_export_count(session_)},
          {"symbol_count", neverd_session_symbol_count(session_)},
          {"analyzed", analyzed_},
          {"analysis_state", analyzed_ ? "complete" : "not_analyzed"},
          {"read_only", readOnly_},
          {"dirty", dirty_}};
}

Json Engine::execute(const std::string &operation, const Json &p) {
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
    if (p.contains("read_only") && !p["read_only"].is_boolean())
      throw Error("invalid_request", "read_only must be boolean");
    const bool readOnly = p.value("read_only", false);
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
    if (!neverd_session_load(next.get(), loadPath.c_str()))
      throw Error("load_failed", ownedString(neverd_last_error(next.get())));
    if (fs::file_size(path) != fileSize ||
        fs::last_write_time(path) != fileTime)
      throw Error(
          "input_changed",
          "Input changed while loading; retry after the writer finishes");
    neverd_session_destroy(session_);
    session_ = next.release();
    if (!reuseLock)
      lock_ = std::move(nextLock);
    readOnly_ = readOnly;
    dirty_ = false;
    analyzed_ = false;
    loadedSize_ = fileSize;
    loadedTime_ = fileTime;
    history_.reset();
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
    auto result = metadata();
    result["warnings"] = warnings;
    return result;
  }
  requireLoaded();
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
    if (neverd_annotations_load(session_) != 0 ||
        neverd_renames_load(session_) != 0)
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
        throw Error(
            "unsaved_changes",
            "Save or reload staged annotations before undoing a rename");
      auto state = store.committedState();
      auto &renames = state["renames"];
      renames.erase(
          std::remove_if(renames.begin(), renames.end(),
                         [&](const Json &row) {
                           return parseAddress(
                                      row.at("addr").get<std::string>()) ==
                                  address;
                         }),
          renames.end());
      if (!value.is_null())
        renames.push_back(value);
      const auto checkpoint = store;
      store.advance(redo);
      try {
        store.persist(state);
      } catch (...) {
        store = checkpoint;
        throw;
      }
      if (neverd_renames_load(session_) != 0)
        throw Error("reload_failed",
                    "History operation was saved but cannot be reloaded",
                    {{"saved", true}});
    }
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
    const int count = neverd_func_count(session_);
    if (!filter.empty() && (!haveFilter_ || filterKey_ != filter)) {
      if (count > 1000000)
        throw Error("budget_exceeded",
                    "Filtered function index exceeds one million entries");
      filteredFunctions_.clear();
      for (int i = 0; i < count; ++i) {
        if (folded(ownedString(neverd_func_name(session_, i))).find(filter) !=
                std::string::npos ||
            hexAddress(neverd_func_entry(session_, i)).find(filter) !=
                std::string::npos)
          filteredFunctions_.push_back(i);
      }
      filterKey_ = filter;
      haveFilter_ = true;
    }
    const auto total = filter.empty()
                           ? static_cast<std::size_t>(std::max(count, 0))
                           : filteredFunctions_.size();
    Json items = Json::array();
    for (auto i = std::min(offset, total); i < total && items.size() < limit;
         ++i) {
      const int index =
          filter.empty() ? static_cast<int>(i) : filteredFunctions_[i];
      items.push_back(
          {{"name", ownedString(neverd_func_name(session_, index))},
           {"address", hexAddress(neverd_func_entry(session_, index))},
           {"size", neverd_func_size(session_, index)}});
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
      if (index < 0)
        throw Error("not_found", "No function matches that symbol");
      address = neverd_func_entry(session_, index);
    }
    return {{"address", hexAddress(address)},
            {"function_address",
             index < 0 ? Json(nullptr)
                       : Json(hexAddress(neverd_func_entry(session_, index)))},
            {"name", index < 0
                         ? std::string()
                         : ownedString(neverd_func_name(session_, index))},
            {"comment", ownedString(neverd_annotation_get(session_, address))}};
  }
  if (operation == "strings") {
    if (stringsCache_.is_null()) {
      stringsCache_ = backendJson(neverd_strings_json(session_, 4));
      for (auto &item : stringsCache_) {
        item["address"] = item.at("addr");
        item["text"] = item.at("value");
        item.erase("addr");
      }
    }
    return page(stringsCache_, p, "text");
  }
  if (operation == "segments") {
    auto items = backendJson(neverd_segments_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("va");
      item.erase("va");
    }
    return page(items, p, "name");
  }
  if (operation == "annotations") {
    auto items = backendJson(neverd_annotations_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("addr");
      item.erase("addr");
    }
    return page(items, p, "text");
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
    const bool annotationsLoaded = neverd_annotations_load(session_) == 0;
    const bool renamesLoaded = neverd_renames_load(session_) == 0;
    // Each existing API can change its own table independently. Publish a new
    // revision even on partial failure so clients cannot retain stale pages.
    invalidate();
    ++revision_;
    history_.reset();
    if (!annotationsLoaded || !renamesLoaded)
      throw Error("load_failed",
                  "Could not reload annotations or renames sidecar",
                  {{"annotations_loaded", annotationsLoaded},
                   {"renames_loaded", renamesLoaded},
                   {"state_changed", true}});
    dirty_ = false;
    return metadata();
  }
  if (operation != "disasm" && operation != "bytes" &&
      operation != "annotation_set" && operation != "rename" &&
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
    return {
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
    invalidate();
    ++revision_;
    return {{"address", hexAddress(address)},
            {"text", text},
            {"dirty", true},
            {"saved", false}};
  }
  if (operation == "rename") {
    requireWriter();
    if (dirty_)
      throw Error(
          "unsaved_changes",
          "Save or reload staged annotations before renaming a function");
    const auto name = stringField(p, "name", {}, 4096);
    if (name.empty())
      throw Error("invalid_request", "Function name cannot be empty");
    const int index = neverd_func_find_by_addr(session_, address);
    if (index < 0)
      throw Error("not_found", "No function begins at that address");
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
      renames.push_back(
          {{"addr", hexAddress(address)},
           {"original", ownedString(neverd_func_name(session_, index))},
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
    invalidate();
    ++revision_;
    return {{"address", hexAddress(address)}, {"name", name}, {"saved", true}};
  }
  if (operation == "decompile") {
    const auto representation = stringField(p, "representation", "c", 16);
    if (representation != "c" && representation != "low" &&
        representation != "med" && representation != "high" &&
        representation != "llvm")
      throw Error("unsupported", "Unknown code representation");
    const auto offset =
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = sizeField(p, "limit", 512, 2048);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    analyze();
    std::string mappingStatus = "unsupported_representation";
    if (representation == "low" || representation == "med") {
      if (const auto view = irViewFunction()) {
        auto result = backendJson(
            view(session_, address, representation.c_str(), offset, limit),
            true);
        mappingStatus =
            result.value("mapping_status", std::string("unavailable"));
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
    const auto key = hexAddress(address) + ":" + representation;
    if (textKey_ != key) {
      textKey_.clear();
      const char *result =
          representation == "c"      ? neverd_decompile(session_, address)
          : representation == "low"  ? neverd_ir_low(session_, address)
          : representation == "med"  ? neverd_ir_med(session_, address)
          : representation == "high" ? neverd_ir_high(session_, address)
                                     : neverd_ir_llvm(session_, address);
      textCache_ = ownedString(result);
      if (textCache_.empty())
        throw Error("unavailable", error().empty()
                                       ? "This representation is unavailable"
                                       : error());
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
    analyze();
    const auto key = hexAddress(address);
    if (!graph_ || graph_->address() != key) {
      auto snapshot = std::make_unique<GraphSnapshot>(
          backendJson(neverd_cfg_json(session_, address), true), key,
          projectId_ + ":" + revision() + ":" + key);
      graph_ = std::move(snapshot);
    }
    return graph_->summary();
  }
  if (operation == "cfg_viewport") {
    if (!graph_ || graph_->address() != hexAddress(address))
      throw Error("stale_layout",
                  "No matching graph snapshot; request cfg_summary first");
    return graph_->viewport(p);
  }
  if (operation == "cfg") {
    analyze();
    auto result = backendJson(neverd_cfg_json(session_, address), true);
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
