#include "Engine.h"

#include "BrowseCache.h"
#include "CodeEdits.h"
#include "ComparisonService.h"
#include "Contributions.h"
#include "EngineSymbols.h"
#include "GraphSnapshot.h"
#include "LineageService.h"
#include "Listing.h"
#include "OperandFormat.h"
#include "ProjectHistory.h"
#include "ResourceLimits.h"
#include "TablePage.h"
#include "TextFold.h"
#include "UnwindQuery.h"

#include "neverd/sdk/Limits.h"
#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIPersist.h"
#include "neverd/sdk/NeverDCAPIQuery.h"
#include "neverd/sdk/NeverDCAPISigs.h"
#include "neverd/support/FileStamp.h"
#include "neverd/support/ProjectFiles.h"
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
#include <thread>
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
using AddAnalysisSection = int (*)(neverd_session_t, const char *,
                                   const unsigned char *, size_t);
using RemoveAnalysisSection = int (*)(neverd_session_t, const char *);
using AnalysisSections = const char *(*)(neverd_session_t, int);
using ImportAnalysisSections = int (*)(neverd_session_t, const char *);
constexpr std::size_t MaxAnalysisSectionBytes =
    neverd::sdk::kMaxAnalysisSectionBytes;
constexpr std::size_t MaxAnalysisSectionState =
    neverd::sdk::kMaxAnalysisSectionStateBytes;

std::vector<unsigned char> sectionBytes(const std::string &hex) {
  if (hex.empty() || hex.size() % 2 || hex.size() / 2 > MaxAnalysisSectionBytes)
    throw Error("invalid_request", "Invalid analysis section byte count");
  const auto digit = [](char value) -> unsigned {
    if (value >= '0' && value <= '9')
      return unsigned(value - '0');
    if (value >= 'a' && value <= 'f')
      return unsigned(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
      return unsigned(value - 'A' + 10);
    throw Error("invalid_request", "Analysis bytes must be hexadecimal pairs");
  };
  std::vector<unsigned char> bytes;
  bytes.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2)
    bytes.push_back(
        static_cast<unsigned char>((digit(hex[i]) << 4) | digit(hex[i + 1])));
  return bytes;
}

fs::path analysisSectionPath(fs::path path) {
  return project_files::path(path, project_files::File::AnalysisSections);
}

fs::path analysisSectionReadPath(const fs::path &path) {
  auto current = analysisSectionPath(path);
  if (fs::exists(current))
    return current;
  auto legacy = path;
  legacy +=
      project_files::describe(project_files::File::AnalysisSections).Legacy;
  return fs::exists(legacy) ? legacy : current;
}
// A signature source is external to the image. Never silently replay a
// different file/tree into a replica of an already loaded project.
Json signatureStamp(const fs::path &path) {
  const auto stamp = fileStamp(path);
  if (!stamp)
    return nullptr;
  Json files = Json::object();
  for (const auto &[name, identity] : *stamp)
    files[name] = {identity.first, identity.second};
  return files;
}

// A completed document never mutates. Monotone bounds let later pages seek
// directly to their rows/names without allocating a second navigation index.
// Both ends must be ordered: nested spans can have ordered starts alone.
bool orderedViewItems(const Json &view, const char *field, const char *beginKey,
                      const char *endKey) {
  const auto items = view.find(field);
  if (items == view.end() || !items->is_array())
    return false;
  std::size_t previousBegin = 0, previousEnd = 0;
  for (const auto &item : *items) {
    const auto begin = item.value(beginKey, std::size_t{0});
    const auto end = item.value(endKey, std::size_t{0});
    if (begin < previousBegin || end < previousEnd)
      return false;
    previousBegin = begin;
    previousEnd = end;
  }
  return true;
}

// Unordered or nested metadata retains its producer order. Cache only its
// numeric bounds so each later page avoids rereading JSON fields for every
// item.
using ViewBounds = std::vector<std::pair<std::size_t, std::size_t>>;
ViewBounds viewBounds(const Json &view, const char *field, const char *beginKey,
                      const char *endKey, bool ordered) {
  ViewBounds result;
  const auto items = view.find(field);
  if (ordered || items == view.end() || !items->is_array())
    return result;
  result.reserve(items->size());
  for (const auto &item : *items)
    result.emplace_back(item.value(beginKey, std::size_t{0}),
                        item.value(endKey, std::size_t{0}));
  return result;
}

// Point rows use an inclusive lower bound; byte spans use an exclusive end.
Json viewItemsOnPage(const Json &items, bool ordered, const char *beginKey,
                     const char *endKey, bool points, std::size_t begin,
                     std::size_t end, const ViewBounds *bounds = nullptr) {
  if (bounds && !bounds->empty()) {
    Json selected = Json::array();
    for (std::size_t i = 0; i < bounds->size(); ++i) {
      const auto [start, last] = (*bounds)[i];
      if (start < end && (points ? last >= begin : last > begin))
        selected.push_back(items[i]);
    }
    return selected;
  }
  auto first = items.begin();
  if (ordered)
    first = std::partition_point(first, items.end(), [&](const auto &item) {
      const auto last = item.value(endKey, std::size_t{0});
      return points ? last < begin : last <= begin;
    });
  Json selected = Json::array();
  for (auto it = first; it != items.end(); ++it) {
    const auto start = it->value(beginKey, std::size_t{0});
    if (ordered && start >= end)
      break;
    const auto last = points ? start : it->value(endKey, std::size_t{0});
    if (start < end && (points ? last >= begin : last > begin))
      selected.push_back(*it);
  }
  return selected;
}

using IRViewFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                       const char *, std::size_t, std::size_t);
using StringEncodingsFunction = const char *(*)();
using DecodeTextFunction = const char *(*)(const unsigned char *, int,
                                           const char *);
using X87CapabilitiesFunction = const char *(*)(neverd_session_t);
X87CapabilitiesFunction x87CapabilitiesFunction() {
  return engineSymbol<X87CapabilitiesFunction>("neverd_x87_capabilities_json");
}
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
// Additive C ABI capability: what the user states of a function, as IDA's
// Edit function does.
using FunctionStateFunction = int (*)(neverd_session_t, neverd_va_t,
                                      const char *);
using FunctionAttributesFunction = const char *(*)(neverd_session_t,
                                                   neverd_va_t);
struct FunctionAttributeFunctions {
  FunctionStateFunction edit;
  FunctionAttributesFunction attributes;
  explicit operator bool() const { return edit && attributes; }
};
const FunctionAttributeFunctions &functionAttributes() {
  static const FunctionAttributeFunctions functions{
      engineSymbol<FunctionStateFunction>("neverd_func_edit"),
      engineSymbol<FunctionAttributesFunction>("neverd_func_attributes_json")};
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
/// The PDB a PE names and where it is, before or after the PE is open.
using PdbReferenceFunction = const char *(*)(const char *, const char *);
// Additive C ABI capability: the program's local types.
using LocalTypesFunction = const char *(*)(neverd_session_t, const char *);
// neverd_local_types_forget: the session and a JSON list of names.
using LocalTypeNamesFunction = const char *(*)(neverd_session_t, const char *);
// The user's type declarations beside the input, as the engine reads them.
using LocalTypeFunction = const char *(*)(neverd_session_t, const char *,
                                          const char *);
/// Whether the next load searches for the PDB a PE names.
using PdbSearchFunction = void (*)(neverd_session_t, int);
/// The DWARF of an ELF or Mach-O image and where it is, before or after the
/// image is open.
using DwarfReferenceFunction = const char *(*)(const char *);
/// Whether the next load reads the DWARF of an ELF or Mach-O image.
using DwarfSearchFunction = void (*)(neverd_session_t, int);
/// An image's static symbol table and the first names it holds, before or
/// after the image is open.
using SymbolTableReferenceFunction = const char *(*)(const char *);
/// Whether the next load reads the image's static symbol table.
using SymbolTableFunction = void (*)(neverd_session_t, int);
/// The linker MAP beside an input and how it holds against the image, before
/// the image is open.
using MapReferenceFunction = const char *(*)(const char *);
/// Whether the next load reads the MAP beside the image.
using MapSearchFunction = void (*)(neverd_session_t, int);
/// The MAP a user chose, read against the open image.
using MapInspectFunction = const char *(*)(neverd_session_t, const char *);
/// Load a MAP into the open image, unchecked or not.
using MapLoadFunction = int (*)(neverd_session_t, const char *, int);
/// Load a DWARF file into the open image.
using DwarfLoadFunction = int (*)(neverd_session_t, const char *);

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
using IRViewExFunction = const char *(*)(neverd_session_t, neverd_va_t,
                                         const char *, size_t, size_t,
                                         const char *);
IRViewExFunction irViewExFunction() {
  static const auto function =
      engineSymbol<IRViewExFunction>("neverd_ir_view_ex_json");
  return function;
}
SessionJsonFunction headersFunction() {
  static const auto function =
      engineSymbol<SessionJsonFunction>("neverd_headers_json");
  return function;
}
/// neverd_call_paths_json and neverd_call_tree_json, which an older engine
/// lacks.
using GraphQueryFunction = const char *(*)(neverd_session_t, const char *);
GraphQueryFunction callPathsFunction() {
  static const auto function =
      engineSymbol<GraphQueryFunction>("neverd_call_paths_json");
  return function;
}
GraphQueryFunction callTreeFunction() {
  static const auto function =
      engineSymbol<GraphQueryFunction>("neverd_call_tree_json");
  return function;
}
/// Every "0x..." string in \p value spelled as the protocol spells addresses.
void lowercaseAddresses(Json &value) {
  if (value.is_string()) {
    auto text = value.get<std::string>();
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
      value = hexAddress(parseAddress(text));
  } else if (value.is_array() || value.is_object()) {
    for (auto &item : value)
      lowercaseAddresses(item);
  }
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
          if (address.is_string()) {
            auto &text = address.get_ref<std::string &>();
            text = hexAddress(parseAddress(text));
          }
      } else if ((key == "addr" || key == "va" || key == "address" ||
                  key == "start" || key == "end" || key == "entry" ||
                  key == "iat_addr" || key == "from" || key == "to" ||
                  unwind_query::isAddressField(key)) &&
                 it->is_string()) {
        auto &text = it->get_ref<std::string &>();
        if (text.starts_with("0x") || text.starts_with("0X"))
          text = hexAddress(parseAddress(text));
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

/// Conservative retained-size accounting: include container capacity, string
/// capacity and tree-node overhead, not just the serialized text's size. A
/// value beyond the cache budget remains readable but is not retained.
std::size_t namedViewJsonBytes(const Json &value, unsigned depth = 0) {
  constexpr auto OverBudget =
      ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes + 1;
  if (depth > ::neverd::limits::worker::kEngineNamedViewJsonBytesDepthLimit)
    return OverBudget;
  std::size_t bytes = sizeof(Json);
  const auto add = [&](std::size_t count) {
    bytes =
        count > ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes - bytes
            ? OverBudget
            : bytes + count;
    return bytes <= ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes;
  };
  if (value.is_string()) {
    if (!add(sizeof(Json::string_t)) ||
        !add(value.get_ref<const Json::string_t &>().capacity() + 1))
      return OverBudget;
  } else if (value.is_array()) {
    const auto &array = value.get_ref<const Json::array_t &>();
    if (!add(sizeof(Json::array_t)) ||
        array.capacity() >
            ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes /
                sizeof(Json) ||
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

enum class PageAddressMatch { Exact, SegmentRange, RegionRange };

/// A page of \p items; a filter keeps the rows where one of \p searchFields
/// contains it, ignoring the case of ASCII letters.
Json page(const Json &items, const Json &payload,
          std::initializer_list<const char *> searchFields = {},
          PageAddressMatch addressMatch = PageAddressMatch::Exact) {
  const auto limit =
      sizeField(payload, "limit",
                ::neverd::limits::worker::kEnginePageLimitSizeFieldDefault,
                ::neverd::limits::worker::kEnginePageLimitSizeFieldMax);
  if (!limit)
    throw Error("invalid_request", "limit must be at least 1");
  const auto filter = folded(stringField(payload, "filter"));
  Json selected = Json::array();
  std::vector<std::size_t> order;
  const bool locating = payload.contains("seek_address");
  const auto requestedOffset =
      sizeField(payload, "offset", 0, std::numeric_limits<std::size_t>::max());
  std::size_t total = 0;
  for (std::size_t index = 0; index < items.size(); ++index) {
    const auto &item = items[index];
    if (!filter.empty() &&
        std::none_of(searchFields.begin(), searchFields.end(),
                     [&](const char *field) {
                       const auto it = item.find(field);
                       return it != item.end() && it->is_string() &&
                              folded(it->get<std::string>()).find(filter) !=
                                  std::string::npos;
                     }))
      continue;
    if (locating)
      order.push_back(index);
    else if (total >= requestedOffset && selected.size() < limit)
      selected.push_back(item);
    ++total;
  }
  const auto position = tablePagePosition(
      payload, total, limit,
      [&](std::size_t index) -> std::optional<TableRowRange> {
        const auto &item = items[order[index]];
        const char *key =
            addressMatch == PageAddressMatch::RegionRange ? "start" : "address";
        if (!item.contains(key) || !item[key].is_string())
          return std::nullopt;
        TableRowRange row{parseAddress(item[key].get<std::string>())};
        if (addressMatch == PageAddressMatch::RegionRange) {
          const auto end = parseAddress(item.at("end").get<std::string>());
          if (end < row.start)
            throw Error("invalid_image", "A region has an invalid end");
          row.size = end - row.start;
        } else if (addressMatch == PageAddressMatch::SegmentRange) {
          const auto &size = item.at("size");
          if (size.is_number_unsigned() ||
              (size.is_number_integer() && size.get<std::int64_t>() >= 0))
            row.size = size.get<std::uint64_t>();
          else if (size.is_string())
            row.size = parseAddress(size.get<std::string>());
          else
            throw Error("invalid_image", "A segment has an invalid size");
        }
        return row;
      });
  const auto offset = position.offset;
  if (locating)
    for (auto index = std::min(offset, total);
         index < total && selected.size() < limit; ++index)
      selected.push_back(items[order[index]]);
  const bool complete = offset >= total || selected.size() >= total - offset;
  Json result{{"items", std::move(selected)},
              {"total", total},
              {"offset", offset},
              {"next_offset", complete ? Json(nullptr) : Json(offset + limit)},
              {"complete", complete}};
  position.annotate(result);
  return result;
}

/// Sort table rows by {"sort": field, "descending": bool}.  Hex address
/// strings compare numerically; other strings case-insensitively.
void sortItems(Json &items, const Json &payload) {
  const auto field = stringField(
      payload, "sort", {},
      ::neverd::limits::worker::kEngineSortItemsFieldSortStringFieldMax);
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

// Replace one sidecar atomically with \p body. Each sidecar is a separate
// durable file; this is intentionally not advertised as a transaction
// spanning both files.
void atomicWriteControlled(const fs::path &path, const std::string &body,
                           const std::function<void()> &beforePublish) {
  static std::atomic<std::uint64_t> sequence{0};
  // A project file's folder, which its first write creates.
  std::error_code folder;
  fs::create_directories(path.parent_path(), folder);
  fs::path temporary = path;
  temporary +=
      ".tmp-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()) +
      "-" + std::to_string(sequence++);
  const auto preparePublication = [&] {
    try {
      if (beforePublish)
        beforePublish();
    } catch (...) {
      std::error_code ignored;
      fs::remove(temporary, ignored);
      throw;
    }
  };
#ifdef _WIN32
  const auto fail = [&](const char *operation, DWORD code) {
    DeleteFileW(temporary.c_str());
    const auto utf8 = path.generic_u8string();
    throw Error("save_failed", std::string(operation) + " for " +
                                   std::string(utf8.begin(), utf8.end()) +
                                   " (Win32 error " + std::to_string(code) +
                                   ")");
  };
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    fail("Cannot create sidecar temporary file", GetLastError());
  DWORD written = 0;
  const bool success =
      WriteFile(file, body.data(), static_cast<DWORD>(body.size()), &written,
                nullptr) &&
      written == body.size() && FlushFileBuffers(file);
  const auto writeError = success ? ERROR_SUCCESS : GetLastError();
  CloseHandle(file);
  if (!success)
    fail("Cannot write sidecar temporary file",
         writeError == ERROR_SUCCESS ? ERROR_WRITE_FAULT : writeError);
  // A snapshot reader or filesystem scanner may briefly deny deletion of the
  // old file. Retain the flushed temporary and retry only sharing-related
  // failures, with a bound; never fall back to deleting the committed file.
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(
          ::neverd::limits::worker::kEngineAtomicWriteDeadlineSeconds);
  for (;;) {
    preparePublication();
    if (MoveFileExW(temporary.c_str(), path.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      break;
    const auto code = GetLastError();
    if ((code != ERROR_SHARING_VIOLATION && code != ERROR_LOCK_VIOLATION &&
         code != ERROR_ACCESS_DENIED) ||
        std::chrono::steady_clock::now() >= deadline)
      fail("Cannot atomically replace sidecar", code);
    std::this_thread::sleep_for(std::chrono::milliseconds(
        ::neverd::limits::worker::kEngineAtomicWriteMilliseconds));
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
  if (success)
    preparePublication();
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
void atomicWrite(const fs::path &path, const std::string &body) {
  atomicWriteControlled(path, body, {});
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
Engine::~Engine() {
  listing_.reset();
  neverd_session_destroy(session_);
}
std::string Engine::version() { return ownedString(neverd_version_number()); }
bool Engine::reportsUnwindFrames() {
  return unwind_query::function() != nullptr;
}
bool Engine::reportsX87Capabilities() {
  return x87CapabilitiesFunction() != nullptr;
}
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
bool Engine::statesFunctionAttributes() {
  return keepsFunctionEdits() && static_cast<bool>(functionAttributes());
}
bool Engine::keepsDataItems() { return static_cast<bool>(dataItems()); }
bool Engine::identifiesFiles() { return identifyFunction() != nullptr; }
bool Engine::findsCallPaths() {
  return callPathsFunction() && callTreeFunction();
}
bool Engine::loadsPdbFiles() {
  using Inspect = const char *(*)(neverd_session_t, const char *);
  using Load = int (*)(neverd_session_t, const char *);
  return engineSymbol<Inspect>("neverd_session_inspect_pdb_json") &&
         engineSymbol<Load>("neverd_session_load_pdb");
}
bool Engine::listsLocalTypes() {
  return engineSymbol<LocalTypesFunction>("neverd_local_types_json") &&
         engineSymbol<LocalTypeFunction>("neverd_local_type_json") &&
         engineSymbol<LocalTypesFunction>("neverd_local_types_source");
}
bool Engine::typesFunctions() {
  using Declaration =
      const char *(*)(neverd_session_t, neverd_va_t, const char *);
  return statesFunctionAttributes() &&
         engineSymbol<Declaration>("neverd_func_type_json");
}
bool Engine::fitsLocalTypes() {
  using Member = const char *(*)(neverd_session_t, const char *, long long,
                                 unsigned long long, const char *);
  return listsLocalTypes() &&
         engineSymbol<LocalTypesFunction>("neverd_local_types_fit_json") &&
         engineSymbol<Member>("neverd_local_type_member_json");
}
bool Engine::declaresLocalTypes() {
  return listsLocalTypes() &&
         engineSymbol<LocalTypeFunction>("neverd_local_types_declare") &&
         engineSymbol<LocalTypeNamesFunction>("neverd_local_types_forget") &&
         engineSymbol<SessionJsonFunction>("neverd_type_declarations_json") &&
         engineSymbol<SessionLoadFunction>("neverd_type_declarations_load");
}
bool Engine::choosesTypeLibraries() {
  return engineSymbol<SessionJsonFunction>("neverd_type_libraries_json") &&
         engineSymbol<LocalTypesFunction>("neverd_type_libraries_set") &&
         engineSymbol<SessionLoadFunction>("neverd_type_libraries_load");
}
bool Engine::findsPdbFiles() {
  return engineSymbol<PdbReferenceFunction>("neverd_pdb_reference_json") &&
         engineSymbol<PdbSearchFunction>("neverd_session_set_pdb_search");
}
bool Engine::findsDwarf() {
  return engineSymbol<DwarfReferenceFunction>("neverd_dwarf_reference_json") &&
         engineSymbol<DwarfSearchFunction>("neverd_session_set_dwarf_search");
}
bool Engine::findsSymbolTables() {
  return engineSymbol<SymbolTableReferenceFunction>(
             "neverd_symbol_table_reference_json") &&
         engineSymbol<SymbolTableFunction>("neverd_session_set_symbol_table");
}
bool Engine::findsMapFiles() {
  return engineSymbol<MapReferenceFunction>("neverd_map_reference_json") &&
         engineSymbol<MapSearchFunction>("neverd_session_set_map_search");
}
bool Engine::loadsMapFiles() {
  return engineSymbol<MapInspectFunction>("neverd_session_inspect_map_json") &&
         engineSymbol<MapLoadFunction>("neverd_session_load_map");
}
bool Engine::loadsDwarfFiles() {
  return static_cast<bool>(
      engineSymbol<DwarfLoadFunction>("neverd_session_load_dwarf"));
}
bool Engine::keepsOperandFormats() {
  return static_cast<bool>(operandFormats());
}
bool Engine::reloadUserState(bool bytePatchesChanged) {
  reloadCodeEdits();
  const bool annotations = neverd_annotations_load(session_) == 0;
  const bool renames = neverd_renames_load(session_) == 0;
  const bool functions = reloadFunctionEdits();
  const bool items = reloadDataItems();
  const bool operands = reloadOperandFormats();
  const bool types = reloadTypeDeclarations();
  const bool patches = reloadBytePatches(bytePatchesChanged);
  return annotations && renames && functions && items && operands && types &&
         patches;
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
int Engine::functionIndex(std::uint64_t address) {
  // Explicit entries keep their identity. Otherwise use the same bounded
  // recovered ownership as navigation before preparing a function view.
  const int index = neverd_func_find_by_addr(session_, address);
  if (index >= 0)
    return index;
  if (const auto owner = listing().chunkOwner(address))
    return neverd_func_find_by_addr(session_, *owner);
  return -1;
}
std::uint64_t Engine::functionEntry(std::uint64_t address) {
  const int index = functionIndex(address);
  return index >= 0 ? neverd_func_entry(session_, index) : address;
}
Json Engine::functionGraph(std::uint64_t address) {
  // A graph reads the function's LowIR alone; a source or IR view of the
  // same function then continues its analysis.
  prepareFunction(address, "cfg");
  return backendJson(neverd_cfg_json(session_, address), true);
}
void Engine::commentsChanged() {
  forgetNamedViews();
  graphs_.clear();
  // Cached pages contain comments; a page rendered now must not be stored or
  // found under the identity read before this edit.
  if (listing_)
    listing_->commentsChanged();
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
Json Engine::typeDeclarationRows() const {
  if (!declaresLocalTypes())
    return Json::array();
  static const auto rows =
      engineSymbol<SessionJsonFunction>("neverd_type_declarations_json");
  Json declared = backendJson(rows(session_));
  if (!declared.is_array())
    throw Error("engine_error", "Engine returned malformed type declarations");
  for (std::size_t i = 0; i < declared.size(); ++i)
    declared[i]["addr"] = hexAddress(i);
  return declared;
}
bool Engine::reloadTypeDeclarations() {
  if (!declaresLocalTypes() || !history_ ||
      typeDeclarationRows() == history_->committedTable("types"))
    return true;
  static const auto load =
      engineSymbol<SessionLoadFunction>("neverd_type_declarations_load");
  if (load(session_) != 0)
    return false;
  analyzed_ = false;
  preparedFunction_.reset();
  operandFormatsChanged();
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
  if (listing_) {
    bool frames =
        std::any_of(codeEdits_.begin(), codeEdits_.end(), [](const Json &row) {
          return row.contains("stack_frame");
        });
    listing_->setStackFrames(codeEdits_, frames ? inputHash() : std::string());
    listing_->invalidate();
  }
  textKey_.clear();
  textCache_.clear();
  textLines_.clear();
  functionOrderKey_.clear();
  functionOrder_.clear();
}
void Engine::forgetNamedViews() {
  namedViews_.clear();
  namedViewBytes_ = 0;
  encodedPageBytes_ = 0;
}

void Engine::localNamesChanged() {
  // Local display names do not change instructions, functions or the prepared
  // pipeline. Keep original documents for checked, lazy redecoration; encoded
  // pages and already decorated documents still belong to their old revision.
  ++revision_;
  namedViewsRevision_ = revision_;
  forgetEncodedPages();
}

void Engine::forgetEncodedPages() {
  for (const auto &document : namedViews_) {
    document->pages.clear();
    document->pageBytes = 0;
  }
  encodedPageBytes_ = 0;
}

std::shared_ptr<const Engine::NamedView>
Engine::namedView(std::uint64_t address, const std::string &representation,
                  const Json &options, const Json *variableEdits,
                  const Json *callTypes, const Json *stackFrame) {
  const auto view = irViewFunction();
  const auto extended = irViewExFunction();
  if (!options.is_object() ||
      options.dump().size() >
          ::neverd::limits::worker::kEngineNamedViewOptionsDumpSizeLimit)
    throw Error("invalid_request", "source_options must be a bounded object");
  if (!options.empty() && !extended)
    throw Error("unsupported", "This engine does not support source options");
  if (!view)
    return nullptr;
  const bool sourceView = representation == "c" || representation == "llvmc" ||
                          representation == "source" ||
                          representation == "cpp" || representation == "rust" ||
                          representation == "go";
  if (!sourceView && !options.empty())
    throw Error("invalid_request", "Source options require a source view");
  Json engineOptions = options;
  Json staleVariableTypes = Json::array();
  std::optional<std::string> typeImage;
  auto currentTypeImage = [&](const Json &type) {
    if (!typeImage)
      typeImage = inputHash();
    return type.value("input_sha256", std::string()) == *typeImage;
  };
  if (sourceView) {
    const auto row = codeEditRow(address);
    const auto route = representation == "source" ? "c" : representation;
    const auto edits =
        row.is_object()
            ? row.value("views", Json::object()).value(route, Json::object())
            : Json::object();
    if (stackFrame || (row.is_object() && row.contains("stack_frame"))) {
      const auto &Frame = stackFrame ? *stackFrame : row.at("stack_frame");
      if (currentTypeImage(Frame))
        engineOptions["stack_frame"] = Frame;
      else
        staleVariableTypes.push_back(
            "A saved stack correction belongs to a different input image; "
            "reset or reapply it");
    }
    const auto calls = callTypes ? *callTypes
                       : row.is_object()
                           ? row.value("call_types", Json::object())
                           : Json::object();
    Json bindings = Json::array();
    for (const auto &[id, type] : calls.items()) {
      if (!currentTypeImage(type)) {
        staleVariableTypes.push_back(
            "A saved call-site declaration belongs to a different input image; "
            "reset or reapply it");
        continue;
      }
      Json binding = type;
      binding["id"] = id;
      bindings.push_back(std::move(binding));
    }
    if (!bindings.empty())
      engineOptions["call_types"] = std::move(bindings);
    const auto associations =
        variableEdits ? *variableEdits
                      : edits.value("variable_edits", Json::array());
    bool Foreign = false;
    for (const auto &Association : associations)
      Foreign |= !currentTypeImage(Association);
    if (Foreign)
      staleVariableTypes.push_back(
          "Saved variable associations belong to a different input image; "
          "reset or reapply them");
    else if (!associations.empty())
      engineOptions["variable_edits"] = associations;
    Json types = Json::array();
    const Json variableTypes = edits.value("variable_types", Json::object());
    for (const auto &[id, type] : variableTypes.items()) {
      if (!currentTypeImage(type)) {
        staleVariableTypes.push_back(
            id + ": the saved type belongs to a different input image");
        continue;
      }
      Json binding = type;
      binding["id"] = id;
      binding["function"] = hexAddress(address);
      types.push_back(std::move(binding));
    }
    for (const auto &row : codeEdits_)
      if (row.contains("object_type")) {
        if (!currentTypeImage(row.at("object_type"))) {
          if (staleVariableTypes.empty())
            staleVariableTypes.push_back(
                "Saved object types belong to a different input image");
          continue;
        }
        Json binding = row.at("object_type");
        binding["function"] = hexAddress(address);
        binding["address"] = row.at("addr");
        binding["id"] =
            "global:" +
            std::to_string(parseAddress(row.at("addr").get<std::string>()));
        binding["snapshot"] = "";
        types.push_back(std::move(binding));
      }
    if (!types.empty())
      engineOptions["variable_types"] = std::move(types);
  }
  const auto optionText = engineOptions.dump();
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
           std::to_string(revision_) + ":" + std::to_string(generation) + ":" +
           optionText;
  };
  const auto lookupKey = cacheKey();
  for (auto it = namedViews_.begin(); it != namedViews_.end(); ++it)
    if ((*it)->key == lookupKey) {
      namedViews_.splice(namedViews_.begin(), namedViews_, it);
      return namedViews_.front();
    }
  const auto sourceKey = hexAddress(address) + ":" + representation + ":" +
                         std::to_string(listing().generation()) + ":" +
                         optionText;
  Json original;
  for (const auto &cached : namedViews_)
    if (cached->sourceKey == sourceKey && !cached->original.empty()) {
      original = Json::parse(cached->original);
      break;
    }
  // A hit answers from its finished document without pretending the mutable
  // Session still holds this function. Graph/IR requests prepare it normally.
  if (original.is_null())
    prepareFunction(address, representation);
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
    Json full = std::move(original);
    auto backendRepresentation = representation;
    for (std::size_t next = 0;
         full.is_null() || !full.value("complete", false);) {
      auto page = backendJson(
          sourceView && extended && !engineOptions.empty()
              ? extended(session_, address, backendRepresentation.c_str(), next,
                         ::neverd::limits::worker::kEngineEnginePageLines,
                         optionText.c_str())
              : view(session_, address, backendRepresentation.c_str(), next,
                     ::neverd::limits::worker::kEngineEnginePageLines),
          true);
      // Older page APIs offer HighC but not the automatic source dialect.
      // Keep the Pseudocode action useful, with the actual dialect reported.
      if (next == 0 && options.empty() && representation == "source" &&
          page.is_object() && !page.contains("text") &&
          page.value("mapping_status", std::string()) ==
              "unsupported_representation") {
        backendRepresentation = "c";
        page =
            backendJson(view(session_, address, "c", next,
                             ::neverd::limits::worker::kEngineEnginePageLines),
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
        // A spelled page names the source names on it alone, and an LLVM IR
        // page the globals on its lines.
        for (const char *field : {"source_names", "references"})
          if (auto names = page.find(field);
              names != page.end() && names->is_array() &&
              full.contains(field) && full[field].is_array())
            for (auto &name : *names)
              full[field].push_back(std::move(name));
      }
      if (complete)
        break;
      if (!following.is_number_unsigned() ||
          following.get<std::size_t>() <= next ||
          full["text"].get_ref<const std::string &>().size() >
              ::neverd::limits::worker::kEngineMaxNamedViewBytes)
        return nullptr;
      next = following.get<std::size_t>();
    }
    full["complete"] = true;
    full["next_offset"] = nullptr;
    full["representation"] = representation;
    full["source_options"] = options;
    if (representation == "source" && backendRepresentation == "c")
      full["dialect"] = "c";
    if (sourceView)
      document->original = full.dump();
    for (const auto &reason : staleVariableTypes)
      full["stale_variable_types"].push_back(reason);
    if (sourceView)
      CodeEdits::spellIdentifierSymbols(full);
    // The resolver below runs other operations, so the decoration names
    // functions from a copy of the listing's display names.
    auto displayNames = CodeEdits::FunctionAliases(listing().functionAliases());
    if (sourceView && !pdbDisplayNames_.empty()) {
      const auto &source = full.at("text").get_ref<const std::string &>();
      for (const auto &name : full.value("source_names", Json::array())) {
        if (!name.contains("address") || !name.at("address").is_string())
          continue;
        const auto begin = name.value("begin_byte", std::size_t{0});
        const auto end = name.value("end_byte", std::size_t{0});
        if (end <= begin || end > source.size())
          continue;
        if (const auto alias = listing().pdbDataName(
                parseAddress(name.at("address").get<std::string>())))
          displayNames.emplace(source.substr(begin, end - begin), *alias);
      }
    }
    if (sourceView)
      CodeEdits::decorate(
          full, codeEditRow(address), address, representation,
          backendJson(neverd_renames_json(session_)),
          backendJson(neverd_annotations_json(session_)), displayNames,
          [this](const std::string &name) -> std::optional<std::uint64_t> {
            // The address the resolve operation answers, without describing
            // the function there; a query it refuses names nothing.
            if (name.empty() ||
                name.size() >
                    ::neverd::limits::worker::kEngineNamedViewNameSizeLimit ||
                name.find('\0') != std::string::npos)
              return std::nullopt;
            try {
              requireLoaded();
              analyzeBeforeFunctionQueries();
              if (name.starts_with("0x") || name.starts_with("0X"))
                return parseAddress(name);
              if (const auto named = resolvedName(name))
                return named->second;
              return std::nullopt;
            } catch (const Error &) {
              return std::nullopt;
            }
          });
    std::vector<std::pair<std::size_t, std::ptrdiff_t>> shift;
    if (!sourceView) {
      full["text"] = renameIdentifiers(full["text"].get<std::string>(),
                                       listing().functionAliases(), shift);
      // A global's name reads as the renamed text spells it.
      if (auto references = full.find("references");
          references != full.end() && references->is_array())
        for (auto &reference : *references)
          if (reference.contains("name") && reference["name"].is_string()) {
            std::vector<std::pair<std::size_t, std::ptrdiff_t>> unused;
            reference["name"] =
                renameIdentifiers(reference["name"].get<std::string>(),
                                  listing().functionAliases(), unused);
          }
    }
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
    // Renames before the definition move where it begins, not its line; so
    // too where the lines after it begin.
    for (const auto &[region, field] : {std::pair{"prelude", "end_byte"},
                                        std::pair{"epilogue", "begin_byte"}})
      if (auto bounds = full.find(region);
          bounds != full.end() && bounds->is_object() &&
          bounds->contains(field) && (*bounds)[field].is_number_unsigned() &&
          (*bounds)[field].get<std::size_t>() >= base)
        (*bounds)[field] =
            base +
            shiftedOffset((*bounds)[field].get<std::size_t>() - base, shift);
    const auto &text = full["text"].get_ref<const std::string &>();
    document->lines.push_back(0);
    for (std::size_t i = 0; i < text.size(); ++i)
      if (text[i] == '\n' && i + 1 < text.size())
        document->lines.push_back(i + 1);
    document->orderedRows = orderedViewItems(full, "rows", "line", "line");
    document->orderedReferences =
        orderedViewItems(full, "references", "line", "line");
    document->orderedSourceNames =
        orderedViewItems(full, "source_names", "begin_byte", "end_byte");
    document->orderedCodeNames =
        orderedViewItems(full, "code_names", "begin_byte", "end_byte");
    document->rowBounds =
        viewBounds(full, "rows", "line", "line", document->orderedRows);
    document->referenceBounds = viewBounds(full, "references", "line", "line",
                                           document->orderedReferences);
    document->sourceNameBounds =
        viewBounds(full, "source_names", "begin_byte", "end_byte",
                   document->orderedSourceNames);
    document->codeNameBounds =
        viewBounds(full, "code_names", "begin_byte", "end_byte",
                   document->orderedCodeNames);
    document->value = std::move(full);
    document->key = key;
    document->sourceKey = sourceKey;
    // If resolving source names changed the listing while decorating this
    // document, it remains usable for this response but is not reusable yet.
    if (cacheKey() != key)
      return document;
    const auto jsonBytes = namedViewJsonBytes(document->value);
    auto overhead =
        sizeof(NamedView) + 8 * sizeof(void *) + document->key.capacity() + 1 +
        document->sourceKey.capacity() + 1 + document->original.capacity() + 1;
    for (const auto *bounds :
         {&document->rowBounds, &document->referenceBounds,
          &document->sourceNameBounds, &document->codeNameBounds}) {
      if (overhead > ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes ||
          bounds->capacity() >
              (::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
               overhead) /
                  sizeof(ViewBounds::value_type))
        return document;
      overhead += bounds->capacity() * sizeof(ViewBounds::value_type);
    }
    if (jsonBytes <= ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes &&
        overhead <= ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
                        jsonBytes &&
        document->lines.capacity() <=
            (::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
             jsonBytes - overhead) /
                sizeof(std::size_t)) {
      document->bytes = jsonBytes + overhead +
                        document->lines.capacity() * sizeof(std::size_t);
      for (auto it = namedViews_.begin(); it != namedViews_.end();) {
        if ((*it)->sourceKey == sourceKey) {
          namedViewBytes_ -= (*it)->bytes;
          encodedPageBytes_ -= (*it)->pageBytes;
          it = namedViews_.erase(it);
        } else
          ++it;
      }
      while (!namedViews_.empty() &&
             (namedViews_.size() >=
                  ::neverd::limits::worker::kEngineMaxNamedViews ||
              document->bytes >
                  ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
                      namedViewBytes_)) {
        namedViewBytes_ -= namedViews_.back()->bytes;
        encodedPageBytes_ -= namedViews_.back()->pageBytes;
        namedViews_.pop_back();
      }
      // Keep completed documents ahead of optional encoded pages. Loading a
      // document may retire pages, but cannot exceed the shared byte limit.
      if (encodedPageBytes_ >
          ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
              namedViewBytes_ - document->bytes)
        forgetEncodedPages();
      namedViews_.push_front(document);
      namedViewBytes_ += document->bytes;
    }
  }
  return document;
}

std::optional<Json> Engine::namedViewPage(std::uint64_t address,
                                          const std::string &representation,
                                          std::size_t offset, std::size_t limit,
                                          const Json &options,
                                          EncodedPayload *encoded) {
  const auto document = namedView(address, representation, options);
  if (!document)
    return std::nullopt;
  const auto &namedView_ = document->value;
  const auto &namedViewLines_ = document->lines;
  const auto &text = namedView_["text"].get_ref<const std::string &>();
  const std::size_t total = text.empty() ? 0 : namedViewLines_.size();
  const auto start = std::min(offset, total);
  auto end = start + std::min(limit, total - start);
  const auto byteAt = [&](std::size_t line) {
    return line < namedViewLines_.size() ? namedViewLines_[line] : text.size();
  };
  while (end > start && byteAt(end) - byteAt(start) >
                            ::neverd::limits::worker::kEngineMaxCodePageBytes)
    --end;
  if (end == start && start < total)
    throw Error("budget_exceeded", "A code line exceeds the page byte budget");
  if (encoded)
    for (auto it = document->pages.begin(); it != document->pages.end(); ++it)
      if (it->start == start && it->end == end) {
        *encoded = it->value;
        document->pages.splice(document->pages.begin(), document->pages, it);
        return Json(nullptr);
      }
  const auto startByte = start < namedViewLines_.size() && !text.empty()
                             ? namedViewLines_[start]
                             : text.size();
  const auto endByte =
      end < namedViewLines_.size() ? namedViewLines_[end] : text.size();
  Json page = Json::object();
  for (const auto &[field, value] : namedView_.items())
    if (field != "text" && field != "rows" &&
        !((field == "source_names" || field == "code_names" ||
           field == "references") &&
          value.is_array()))
      page[field] = value;
  page["text"] = text.substr(startByte, endByte - startByte);
  page["rows"] =
      viewItemsOnPage(namedView_.at("rows"), document->orderedRows, "line",
                      "line", true, start, end, &document->rowBounds);
  if (const auto references = namedView_.find("references");
      references != namedView_.end() && references->is_array()) {
    page["references"] =
        viewItemsOnPage(*references, document->orderedReferences, "line",
                        "line", true, start, end, &document->referenceBounds);
  }
  const std::size_t base = namedView_.value("byte_offset", std::size_t{0});
  for (const char *field : {"source_names", "code_names"}) {
    if (!namedView_.contains(field) || !namedView_[field].is_array())
      continue;
    const bool ordered = std::string_view(field) == "source_names"
                             ? document->orderedSourceNames
                             : document->orderedCodeNames;
    page[field] = viewItemsOnPage(
        namedView_[field], ordered, "begin_byte", "end_byte", false,
        base + startByte, base + endByte,
        std::string_view(field) == "source_names" ? &document->sourceNameBounds
                                                  : &document->codeNameBounds);
  }
  page["offset"] = start;
  page["byte_offset"] = base + startByte;
  page["total_lines"] = total;
  page["complete"] = end == total;
  page["next_offset"] = end == total ? Json(nullptr) : Json(end);
  page["project_id"] = projectId_;
  page["revision"] = revision();
  if (encoded) {
    // A page owns only already-serialized payload data. The response envelope
    // still supplies the current request id, project and cancellation state.
    *encoded = std::make_shared<EncodedJson>(page);
    const auto bytes = (*encoded)->storageBytes() +
                       sizeof(NamedView::EncodedPage) + 6 * sizeof(void *);
    if (!namedViews_.empty() && namedViews_.front() == document &&
        bytes <= ::neverd::limits::worker::kEngineMaxEncodedBytes &&
        bytes <= ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
                     namedViewBytes_) {
      if (encodedPageBytes_ >
              ::neverd::limits::worker::kEngineMaxEncodedBytes - bytes ||
          encodedPageBytes_ >
              ::neverd::limits::worker::kEngineMaxNamedViewCacheBytes -
                  namedViewBytes_ - bytes)
        forgetEncodedPages();
      if (document->pages.size() >=
          ::neverd::limits::worker::kEngineMaxEncodedPages) {
        const auto removed = document->pages.back().bytes;
        document->pages.pop_back();
        document->pageBytes -= removed;
        encodedPageBytes_ -= removed;
      }
      document->pages.push_front({start, end, *encoded, bytes});
      document->pageBytes += bytes;
      encodedPageBytes_ += bytes;
    }
    return Json(nullptr);
  }
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
void Engine::prepareFunction(std::uint64_t address,
                             std::string_view representation) {
  if (analyzed_ || preparedFunction_ == address)
    return;
  // VM images analyze the whole program as one unit.
  const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
  if (arch == "evm" || arch == "sbf") {
    analyze();
    return;
  }
  // A failed switch may have already discarded the preceding pipeline.
  // Never leave its old address marked prepared after the call fails.
  preparedFunction_.reset();
  textKey_.clear();
  // Preparation shares the decompiler's entry/handler checks but emits no
  // discarded PlainC text before the requested source, IR or CFG view.
  using PrepareFunction = int (*)(neverd_session_t, neverd_va_t);
  static const auto prepare =
      engineSymbol<PrepareFunction>("neverd_prepare_function");
  using PrepareView = int (*)(neverd_session_t, neverd_va_t, const char *);
  static const auto prepareView =
      engineSymbol<PrepareView>("neverd_prepare_function_view");
  if (prepare || prepareView) {
    const std::string view =
        representation.empty() ? "source" : std::string(representation);
    bool ready = prepareView ? prepareView(session_, address, view.c_str())
                             : prepare(session_, address);
    // An engine that predates the graph's own view prepares the analysis
    // the IR views share.
    if (!ready && prepareView && view == "cfg" &&
        error() == "invalid function view")
      ready = prepareView(session_, address, "low");
    if (!ready) {
      const auto message = error();
      throw Error("analysis_failed",
                  message.empty() ? "Function analysis failed" : message);
    }
  } else {
    // Older engines expose preparation through their decompiler alone.
    // Preserve its behavior: an unavailable source need not mean missing IR.
    (void)ownedString(neverd_decompile(session_, address));
  }
  preparedFunction_ = address;
}
Listing &Engine::newListing() {
  listing_ = std::make_unique<Listing>(session_);
  listing_->setSteppedDiscovery(steppedDiscovery_);
  listing_->setStringOptions(stringOptions_);
  listing_->setPdbDisplayNames(pdbDisplayNames_);
  const bool frames =
      std::any_of(codeEdits_.begin(), codeEdits_.end(),
                  [](const Json &row) { return row.contains("stack_frame"); });
  listing_->setStackFrames(codeEdits_, frames ? inputHash() : std::string());
  const auto project = projectJson();
  if (browseCacheEnabled_ && project.is_object() && project.contains("folder"))
    listing_->configureCache(
        utf8Path(project.at("folder").get<std::string>()),
        [this] { return browseCacheIdentity(); }, !readOnly_, atomicWrite);
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
  if (!listing_ || !neverd_session_is_loaded(session_))
    return false;
  return listing_->cachePending() ||
         (backgroundAnalysis_ &&
          (listing_->hasIdleWork() || warmState_ == WarmState::Pending ||
           warmState_ == WarmState::Running));
}
bool Engine::idleStep() {
  if (!hasIdleWork())
    return false;
  if (listing_->cachePending()) {
    listing_->flushCacheOne();
    return true;
  }
  // The listing and its reference index come first; preparing functions
  // only shortens later decompiles.
  if (listing_->hasIdleWork())
    return listing_->idleStep();
  warmStep();
  return true;
}
void Engine::restartWarming() {
  warmState_ = warmFunctions_ ? WarmState::Pending : WarmState::Off;
  warmCursor_ = 0;
  warmPrepared_ = 0;
  warmFull_ = false;
  warmError_.clear();
}
void Engine::warmStep() {
  using WarmFunction = const char *(*)(neverd_session_t, neverd_va_t, int);
  static const auto warm =
      engineSymbol<WarmFunction>("neverd_warm_functions_json");
  if (!warm) {
    warmState_ = WarmState::Unavailable;
    return;
  }
  warmState_ = WarmState::Running;
  // A step holds the session until its builds finish, so it takes only as
  // many functions as the engine builds at once.
  constexpr int WarmStepFunctions = 4;
  const char *raw = warm(session_, warmCursor_, WarmStepFunctions);
  if (!raw) {
    warmState_ = WarmState::Unavailable;
    warmError_ = error();
    return;
  }
  const auto page = backendJson(raw);
  if (!page.value("available", false)) {
    warmState_ = WarmState::Unavailable;
    return;
  }
  warmPrepared_ += page.value("prepared", std::size_t{0});
  warmFull_ = warmFull_ || page.value("full", false);
  const auto next = page.value("next_entry", Json());
  if (next.is_string())
    warmCursor_ = parseAddress(next.get<std::string>());
  else
    warmState_ = warmFull_ ? WarmState::Full : WarmState::Complete;
}
Json Engine::warmJson() const {
  static constexpr const char *Names[] = {"off",      "pending", "running",
                                          "complete", "full",    "unavailable"};
  Json state = {{"state", Names[static_cast<int>(warmState_)]},
                {"prepared", warmPrepared_}};
  if (!warmError_.empty())
    state["error"] = warmError_;
  return state;
}
Json Engine::backgroundState() const {
  if (!listing_)
    return nullptr;
  auto state = listing_->indexState();
  state["warm"] = warmJson();
  return state;
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
    auto value = transport::parseBackendJson(text);
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
    history_->onCommit([this] { recordProject(); });
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
         {"patches", bytePatchRows()},
         {"operands", operandFormatRows()},
         {"code_edits", codeEdits_},
         {"types", declaresLocalTypes() ? typeDeclarationRows()
                                        : history_->committedTable("types")}});
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
  const auto debug = headers.value("debug_info", Json::object());
  const auto symbol =
      headers.value("dynamic", Json::object()).value("pdb_symbol", Json());
  const bool mayDownload = debug.value("enabled", false) &&
                           debug.value("kind", "none") != "pdb" &&
                           pdbInput_.is_null() && loadsPdbFiles();
  return {{"path", ownedString(neverd_session_file_path(session_))},
          {"architecture", arch},
          {"format", ownedString(neverd_session_format_name(session_))},
          {"bitness", neverd_session_bitness(session_)},
          {"file_size", std::to_string(neverd_session_file_size(session_))},
          // The input's identity, which links name it by.
          {"sha256", inputHash()},
          // The project folder beside the input, and which build its files
          // describe: a project made for another build may not match.
          {"project", projectJson()},
          {"base_address", hexAddress(neverd_session_base_addr(session_))},
          {"entry_address", hexAddress(neverd_session_entry_addr(session_))},
          {"loader_diagnostics", std::move(diagnostics)},
          {"pdb", pdbInput_},
          {"pdb_download", mayDownload ? symbol : Json()},
          {"dwarf", dwarfInput_},
          {"map", mapInput_},
          {"symbols", symbolsInput_},
          // What the load read: {"enabled","kind","path","files"}, kind
          // "dwarf", "pdb", "map" or "none", and every file read, the PDB or
          // DWARF before a MAP.
          {"debug_info", debug},
          {"function_count",
           deferred ? Json(nullptr) : Json(neverd_func_count(session_))},
          {"segment_count", neverd_session_segment_count(session_)},
          {"section_count", neverd_session_section_count(session_)},
          {"import_count", neverd_session_import_count(session_)},
          {"export_count", neverd_session_export_count(session_)},
          {"symbol_count", neverd_session_symbol_count(session_)},
          {"language", headers.value("language", Json::object())},
          {"analyzed", analyzed_},
          {"analysis_sections_supported",
           engineSymbol<AddAnalysisSection>("neverd_analysis_section_add") !=
               nullptr},
          {"analysis_state", analyzed_ ? "complete" : "not_analyzed"},
          {"read_only", readOnly_},
          {"browse_cache", listing_ ? listing_->cacheStats() : Json()},
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
          {"patches", bytePatchRows()},
          {"operands", operandFormatRows()},
          {"code_edits", codeEdits_}};
}

void Engine::reloadCodeEdits() {
  codeEdits_ = readCodeEdits(ownedString(neverd_session_file_path(session_)));
}

void Engine::recordProject() {
  // The engine records which build of the input the project's files describe;
  // an older engine keeps no record, and a record that fails fails no edit.
  static const auto record =
      engineSymbol<SessionLoadFunction>("neverd_project_record");
  if (record)
    (void)record(session_);
}

Json Engine::projectJson() const {
  static const auto project =
      engineSymbol<SessionJsonFunction>("neverd_project_json");
  if (!project)
    return nullptr;
  const auto text = ownedString(project(session_));
  if (text.empty())
    return nullptr;
  try {
    return Json::parse(text);
  } catch (const Json::exception &) {
    return nullptr;
  }
}

bool Engine::localTypesChanged() {
  if (!declaresLocalTypes())
    return false;
  static const auto rows =
      engineSymbol<SessionJsonFunction>("neverd_type_declarations_json");
  const fs::path path = project_files::path(
      utf8Path(ownedString(neverd_session_file_path(session_))),
      project_files::File::Types);
  Json onDisk = Json::array();
  if (fs::exists(path)) {
    if (fs::file_size(path) >
        ::neverd::limits::worker::kEngineMaxDeclarationBytes)
      throw Error("resource_limit", "Local types sidecar exceeds 4 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input)
      throw Error("load_failed", "Cannot read the local types sidecar");
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    try {
      onDisk =
          parseJson(text, ::neverd::limits::worker::kEngineMaxDeclarationBytes);
    } catch (const Error &) {
      // A file that does not parse differs; loading it reports why.
      return true;
    }
  }
  return onDisk != backendJson(rows(session_));
}

fs::path Engine::typeLibrariesPath() const {
  return project_files::path(
      utf8Path(ownedString(neverd_session_file_path(session_))),
      project_files::File::TypeLibraries);
}

std::optional<Json> Engine::typeLibrariesOnDisk() {
  const fs::path path = typeLibrariesPath();
  Json onDisk = {{"added", Json::array()}, {"removed", Json::array()}};
  if (!fs::exists(path))
    return onDisk;
  if (fs::file_size(path) >
      ::neverd::limits::worker::kEngineMaxDeclarationBytes)
    throw Error("resource_limit", "The type libraries file exceeds 4 MiB");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw Error("load_failed", "Cannot read the type libraries file");
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  try {
    const Json read =
        parseJson(text, ::neverd::limits::worker::kEngineMaxDeclarationBytes);
    if (!read.is_object())
      return std::nullopt;
    for (const char *key : {"added", "removed"})
      if (read.contains(key))
        onDisk[key] = read.at(key);
  } catch (const Error &) {
    return std::nullopt;
  }
  return onDisk;
}

bool Engine::typeLibrariesChanged() {
  if (!choosesTypeLibraries())
    return false;
  // A file that does not parse differs; loading it reports why.
  const std::optional<Json> onDisk = typeLibrariesOnDisk();
  return !onDisk || *onDisk != typeLibraryChoice_;
}

Json Engine::readCodeEdits(const std::string &binary) {
  const auto path =
      project_files::path(utf8Path(binary), project_files::File::Code);
  if (!fs::exists(path))
    return Json::array();
  if (fs::file_size(path) >
      ::neverd::limits::worker::kEngineReadCodeEditsFsFileSizePathLimit)
    throw Error("resource_limit", "Code edit sidecar exceeds 16 MiB");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw Error("load_failed", "Cannot read code edit sidecar");
  const std::string text((std::istreambuf_iterator<char>(input)), {});
  auto rows = parseJson(text, 16 * 1024 * 1024);
  if (!rows.is_array() ||
      rows.size() > ::neverd::limits::worker::kEngineReadCodeEditsRowsSizeLimit)
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
  // A declined PDB names no file to have changed.
  if (pdbInput_.contains("path") &&
      signatureStamp(utf8Path(pdbInput_.at("path").get<std::string>())) !=
          pdbInput_.at("stamp"))
    throw Error("input_changed", "Loaded PDB changed; load it again");
  for (const auto &input : signatureInputs_)
    if (signatureStamp(utf8Path(input.at("path").get<std::string>())) !=
        input.at("stamp"))
      throw Error("input_changed", "Loaded signatures changed; reload them");
  for (const Json *input : {&dwarfInput_, &mapInput_})
    if (input->contains("path") && !input->value("stamp", Json()).is_null() &&
        signatureStamp(utf8Path(input->at("path").get<std::string>())) !=
            input->at("stamp"))
      throw Error("input_changed",
                  "A loaded debug file changed; load it again");
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
  options["warm"] = false;
  options["read_only"] = true;
  Json analysisSections = nullptr;
  if (const auto query =
          engineSymbol<AnalysisSections>("neverd_analysis_sections_json")) {
    // The replica takes the additions' bytes from this reply, two hexadecimal
    // digits a byte: refuse what one frame cannot carry before encoding it.
    std::uint64_t bytes = 0;
    const auto sectionInfo = backendJson(query(session_, 0), true);
    for (const auto &section : sectionInfo.at("sections"))
      bytes += section.at("size").get<std::uint64_t>();
    if (bytes > MaxFrameBytes / 2)
      throw Error("budget_exceeded", "Background analysis cannot carry " +
                                         std::to_string(bytes) +
                                         " bytes of analysis sections in one " +
                                         std::to_string(MaxFrameBytes >> 20) +
                                         " MiB worker frame");
    analysisSections = backendJson(query(session_, 1), true);
  }
  return {{"schema", 1},
          {"open", options},
          {"input_sha256", inputHash()},
          {"state", userState()},
          {"analysis_sections", std::move(analysisSections)},
          {"signatures", signatureInputs_},
          {"pdb", pdbInput_},
          {"dwarf", dwarfInput_},
          {"map", mapInput_},
          {"symbols", symbolsInput_},
          {"string_options", stringOptions_.empty()
                                 ? Json::object()
                                 : Json::parse(stringOptions_)}};
}

Json Engine::browseCacheIdentity() {
  auto identity = analysisSnapshot();
  // Execution/presentation choices do not change the loaded analysis input.
  // A writer's completed facts are also usable by a read-only consumer.
  identity["open"] = loadOptions() ? backendJson(loadOptions().json(session_))
                                   : Json::object();
  // Whole-program analysis changes function rows and frames without always
  // changing the function count those documents are checked against. Every
  // change of this flag invalidates the listing, which reads it again.
  identity["analyzed"] = analyzed_;
  Json shared = Json::object();
  const auto project = projectJson();
  const auto folder = utf8Path(project.at("folder").get<std::string>());
  for (const auto &file : project_files::Files) {
    if (!file.Shared || file.Id == project_files::File::Manifest ||
        file.Id == project_files::File::Bookmarks)
      continue;
    const auto path = folder / std::string(file.Path);
    if (!fs::exists(path))
      continue;
    std::ifstream input(path, std::ios::binary);
    std::string bytes(static_cast<std::size_t>(fs::file_size(path)), '\0');
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
      throw Error("cache_unavailable", "Cannot read project cache identity");
    shared[std::string(file.Path)] = BrowseCache::digest(bytes);
  }
  identity["shared"] = std::move(shared);
  if (headersFunction()) {
    const auto debug = backendJson(headersFunction()(session_))
                           .value("debug_info", Json::object());
    identity["debug"] = debug;
    for (const auto &file : debug.value("files", Json::array())) {
      const auto path = file.is_string() ? file.get<std::string>()
                                         : file.value("path", std::string());
      if (!path.empty())
        identity["debug_stamps"][path] = signatureStamp(utf8Path(path));
    }
  }
  return identity;
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
  // The owner left the PDB it declined unloaded; so does its replica.
  const auto pdb = snapshot.value("pdb", Json());
  const bool pdbDeclined =
      pdb.is_object() && pdb.value("mode", Json("")) == "none";
  if (pdbDeclined)
    options["pdb"] = pdb;
  // And the DWARF, the MAP and the symbol table it declined; the files it
  // chose load below, once the project's edits are checked.
  for (const char *input : {"dwarf", "map", "symbols"})
    if (const auto answer = snapshot.value(input, Json());
        answer.is_object() && answer.value("mode", Json("")) == "none")
      options[input] = answer;
  execute("open", options);
  if (inputHash() != snapshot.at("input_sha256").get<std::string>())
    throw Error("input_changed",
                "The analysis input differs from the open project");
  if (const auto sections = snapshot.value("analysis_sections", Json());
      !sections.is_null()) {
    const auto import = engineSymbol<ImportAnalysisSections>(
        "neverd_analysis_sections_import_json");
    const auto query =
        engineSymbol<AnalysisSections>("neverd_analysis_sections_json");
    if (!import || !query)
      throw Error("invalid_analysis_section",
                  "Cannot restore the owner's analysis image additions");
    // An owner without additions, over a replica its open gave none, leaves
    // nothing to import: importing copies the image and retires the
    // replica's analysis, which every background start would pay.
    const auto carried = sections.find("sections");
    const bool none =
        carried != sections.end() && carried->is_array() && carried->empty() &&
        backendJson(query(session_, 0), true).at("sections").empty();
    if (!none) {
      if (!import(session_, sections.dump().c_str()))
        throw Error("invalid_analysis_section",
                    "Cannot restore the owner's analysis image additions");
      newListing();
      invalidate();
    }
  }
  auto actual = userState();
  auto expected = snapshot.at("state");
  // A rename's original spelling is derived from the names the image has, and
  // the owner's PDB and signatures are applied below, after this check. As in
  // ProjectHistory::verifyLoadedState, only the user's values identify edits.
  for (auto *state : {&actual, &expected})
    if (auto renames = state->find("renames");
        renames != state->end() && renames->is_array())
      for (auto &row : *renames)
        if (row.is_object())
          row.erase("original");
  // Committed edits have one writer. Loading them is safe only when they
  // still match the version exported by that writer. A racing edit causes
  // rejection; the GUI retires this replica on the new owner revision.
  for (const char *table :
       {"renames", "functions", "items", "operands", "code_edits", "patches"})
    if (actual.at(table) != ((std::string_view(table) == "code_edits" ||
                              std::string_view(table) == "patches")
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
  if (!pdb.is_null() && !pdbDeclined)
    execute("pdb_load", pdb);
  // The DWARF and MAP files the owner loaded by choice load here the same
  // way, after its PDB, which outranks a MAP.
  for (const char *input : {"dwarf", "map"})
    if (const auto chosen = snapshot.value(input, Json());
        chosen.is_object() && chosen.contains("path"))
      execute(std::string(input) + "_load", chosen);
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
  // Automatic companions are also inputs, even when the owner did not choose
  // them through pdb_load/dwarf_load. Keep their loaded-file stamps too.
  restoredDebugInputs_ = Json::array();
  if (headersFunction())
    for (const auto &file : backendJson(headersFunction()(session_))
                                .value("debug_info", Json::object())
                                .value("files", Json::array()))
      if (file.contains("path") && file.at("path").is_string() &&
          !file.at("path").get_ref<const std::string &>().empty()) {
        const auto path = file.at("path").get<std::string>();
        restoredDebugInputs_.push_back(
            {{"path", path}, {"stamp", signatureStamp(utf8Path(path))}});
      }
  restoredSnapshot_ = snapshot;
  return metadata();
}

bool Engine::comparesBinaries() { return ComparisonService::supported(); }

Json Engine::updateAnalysisNames(const Json &snapshot) {
  requireLoaded();
  if (!readOnly_ || restoredSnapshot_.is_null())
    throw Error("invalid_request",
                "Only a restored analysis replica can update names");
  auto before = restoredSnapshot_;
  auto after = snapshot;
  for (auto *value : {&before, &after}) {
    auto &state = value->at("state");
    state["code_edits"] =
        CodeEdits::analysisState(state.value("code_edits", Json::array()));
  }
  if (before != after)
    throw Error("stale_snapshot", "Analysis changed; restore a fresh replica");
  // Check the retained inputs as well as the new writer-owned sidecar before
  // changing anything. This operation never writes owner files or history.
  (void)analysisSnapshot();
  for (const auto &input : restoredDebugInputs_)
    if (signatureStamp(utf8Path(input.at("path").get<std::string>())) !=
        input.at("stamp"))
      throw Error("input_changed",
                  "A loaded debug file changed; restore analysis again");
  const auto path = ownedString(neverd_session_file_path(session_));
  if (fs::file_size(utf8Path(path)) != loadedSize_ ||
      fs::last_write_time(utf8Path(path)) != loadedTime_)
    throw Error("input_changed", "The analysis input changed; reopen it");
  if (ProjectHistory::recoveryPending(utf8Path(path)))
    throw Error("stale_snapshot", "Project edits require transaction recovery");
  auto committed = ProjectHistory::readState(utf8Path(path));
  if (committed.at("code_edits").size() >
      ::neverd::limits::worker::kEngineReadCodeEditsRowsSizeLimit)
    throw Error("invalid_request", "Invalid code edit table");
  auto expected = ProjectHistory::normalizedState(snapshot.at("state"));
  // Local declarations are loaded from disk when a replica opens. They must
  // still match that analysis even though the snapshot has no types table.
  expected["types"] =
      declaresLocalTypes() ? typeDeclarationRows() : committed.at("types");
  for (auto *state : {&committed, &expected})
    for (auto &rename : state->at("renames"))
      rename.erase("original");
  if (committed != expected || typeLibrariesChanged())
    throw Error("stale_snapshot",
                "Project edits changed while updating analysis");
  codeEdits_ = snapshot.at("state").value("code_edits", Json::array());
  restoredSnapshot_ = snapshot;
  localNamesChanged();
  return {{"updated", true}};
}

void Engine::loadAnalysisSections(neverd_session_t session,
                                  const fs::path &path) {
  const auto sidecar = analysisSectionReadPath(path);
  if (!fs::exists(sidecar))
    return;
  const auto add =
      engineSymbol<AddAnalysisSection>("neverd_analysis_section_add");
  if (!add)
    throw Error("unsupported",
                "The engine cannot load saved analysis sections");
  const auto size = fs::file_size(sidecar);
  if (size > MaxAnalysisSectionState)
    throw Error("budget_exceeded",
                "Analysis section sidecar exceeds its byte limit");
  std::ifstream input(sidecar, std::ios::binary);
  if (!input)
    throw Error("load_failed", "Cannot read analysis section sidecar");
  std::string text(static_cast<std::size_t>(size), '\0');
  input.read(text.data(), static_cast<std::streamsize>(text.size()));
  if (!input || input.peek() != std::char_traits<char>::eof())
    throw Error("load_failed",
                "Analysis section sidecar changed while reading");
  if (const auto import = engineSymbol<ImportAnalysisSections>(
          "neverd_analysis_sections_import_json")) {
    if (text.find('\0') != std::string::npos)
      throw Error("load_failed", "Embedded NUL in analysis section sidecar");
    if (!import(session, text.c_str()))
      throw Error("load_failed", ownedString(neverd_last_error(session)));
    return;
  }
  // Compatibility with the first additive section API: import only into the
  // unpublished next session. New engines own state validation in the SDK.
  const auto document = parseJson(text, MaxAnalysisSectionState);
  const auto dashboard =
      Json::parse(ownedString(neverd_dashboard_json(session)));
  if (document.value("schema", "") != "neverd.analysis-sections.v1" ||
      document.value("sample_sha256", "") !=
          dashboard.at("hashes").at("sha256").get<std::string>() ||
      !document.contains("sections") || !document["sections"].is_array() ||
      document["sections"].size() > neverd::sdk::kMaxAnalysisSections)
    throw Error("load_failed",
                "Saved analysis sections do not match the loaded input");
  for (auto section : document["sections"]) {
    const auto bytes = sectionBytes(
        stringField(section, "data_hex", {}, MaxAnalysisSectionState));
    if (!section.contains("size") || section["size"] != bytes.size())
      throw Error("load_failed", "Saved analysis section byte count differs");
    section.erase("size");
    section.erase("data_hex");
    if (!add(session, section.dump().c_str(), bytes.data(), bytes.size()))
      throw Error("load_failed", ownedString(neverd_last_error(session)));
  }
}

void Engine::saveAnalysisSections() {
  const auto query =
      engineSymbol<AnalysisSections>("neverd_analysis_sections_json");
  if (!query)
    return;
  const auto text = ownedString(query(session_, 1));
  if (text.empty())
    throw Error("save_failed", error());
  auto document = parseJson(text, MaxAnalysisSectionState);
  const auto path = analysisSectionPath(
      utf8Path(ownedString(neverd_session_file_path(session_))));
  if (document.at("sections").empty() && !fs::exists(path))
    return;
  if (!document.contains("sample_sha256"))
    document["sample_sha256"] =
        backendJson(neverd_dashboard_json(session_)).at("hashes").at("sha256");
  if (document.dump().size() > MaxAnalysisSectionState)
    throw Error("budget_exceeded",
                "Analysis section sidecar exceeds its byte limit");
  if (const auto ec = project_files::prepare(
          utf8Path(ownedString(neverd_session_file_path(session_)))))
    throw Error("save_failed", ec.message());
  atomicWrite(path, sidecarText(document));
}

void Engine::changedAnalysisSections() {
  analyzed_ = false;
  preparedFunction_.reset();
  dirty_ = true;
  sectionsDirty_ = true;
  ++revision_;
  newListing();
  invalidate();
}

Json Engine::execute(const std::string &operation, const Json &p) {
  return executeImpl(operation, p, nullptr);
}

Engine::Reply Engine::executeResponse(const std::string &operation,
                                      const Json &p) {
  // A change to the project can drop the engine's prepared functions.
  const auto before = revision_;
  struct RestartOnChange {
    Engine &engine;
    std::uint64_t before;
    ~RestartOnChange() {
      if (engine.revision_ != before)
        engine.restartWarming();
    }
  } restart{*this, before};
  Reply reply;
  reply.value = executeImpl(operation, p, &reply.encoded);
  return reply;
}

void Engine::analyzeBeforeFunctionQueries() {
  const auto arch = folded(ownedString(neverd_session_arch_name(session_)));
  // Those C ABI paths trigger the entire pipeline for VM architectures.
  // Publish its completion and revision explicitly rather than silently
  // mutating the Session under a revision advertised as not analyzed.
  if (arch == "evm" || arch == "sbf")
    analyze();
}

std::optional<std::pair<int, std::uint64_t>>
Engine::resolvedName(const std::string &name) {
  if (const int index = neverd_func_find_by_name(session_, name.c_str());
      index >= 0)
    return std::pair{index, neverd_func_entry(session_, index)};
  if (const auto named = listing().resolveName(name))
    return std::pair{neverd_func_find_by_addr(session_, *named), *named};
  return std::nullopt;
}

Json Engine::executeImpl(const std::string &operation, const Json &p,
                         EncodedPayload *encoded) {
  if (encoded)
    encoded->reset();
  if (operation == "x87_capabilities") {
    if (!p.is_object() || !p.empty())
      throw Error("invalid_request", "x87_capabilities accepts no parameters");
    const auto query = x87CapabilitiesFunction();
    if (!query)
      throw Error("unsupported",
                  "This engine does not report x87 capabilities");
    return backendJson(query(session_), true);
  }
  if (operation.starts_with("comparison_")) {
    if (!comparison_)
      comparison_ = std::make_unique<ComparisonService>(atomicWrite);
    return comparison_->execute(operation, p, loadProgress_, cancelled_);
  }
  if (operation.starts_with("lineage_")) {
    if (!lineage_)
      lineage_ = std::make_unique<LineageService>(atomicWriteControlled);
    return lineage_->execute(operation, p, loadProgress_, cancelled_);
  }
  if (operation == "analysis_snapshot")
    return analysisSnapshot();
  if (operation == "analysis_restore")
    return restoreAnalysis(p);
  if (operation == "analysis_update_names")
    return updateAnalysisNames(p);
  if (operation == "identify") {
    // How each loader would read a file, before it is opened.
    const auto identify = identifyFunction();
    if (!identify)
      throw Error("unsupported", "This engine cannot identify files");
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplIdentifyPathPathStringFieldMax);
    auto result = backendJson(identify(path.c_str()));
    if (result.contains("error"))
      throw Error("invalid_request", result.value("error", std::string()));
    return result;
  }
  if (operation == "pdb_reference") {
    // The PDB a PE names, where a copy of it is, and where to download it,
    // before the PE is opened as after.
    const auto reference =
        engineSymbol<PdbReferenceFunction>("neverd_pdb_reference_json");
    if (!reference)
      throw Error("unsupported", "This engine cannot find PDB files");
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplPdbReferencePathPathStringFieldMax);
    auto result = backendJson(reference(path.c_str(), nullptr));
    if (result.contains("error"))
      throw Error("invalid_request", result.value("error", std::string()));
    return result;
  }
  if (operation == "dwarf_reference") {
    // The DWARF of an ELF or Mach-O image and where it is, before the image
    // is opened as after.
    const auto reference =
        engineSymbol<DwarfReferenceFunction>("neverd_dwarf_reference_json");
    if (!reference)
      throw Error("unsupported", "This engine cannot find DWARF");
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplDwarfReferencePathPathStringFieldMax);
    auto result = backendJson(reference(path.c_str()));
    if (result.contains("error"))
      throw Error("invalid_request", result.value("error", std::string()));
    return result;
  }
  if (operation == "map_reference") {
    // The linker MAP beside an input and how it holds against the image,
    // before the image is opened.
    const auto reference =
        engineSymbol<MapReferenceFunction>("neverd_map_reference_json");
    if (!reference)
      throw Error("unsupported", "This engine cannot find MAP files");
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplMapReferencePathPathStringFieldMax);
    auto result = backendJson(reference(path.c_str()));
    if (result.contains("error"))
      throw Error("invalid_request", result.value("error", std::string()));
    return result;
  }
  if (operation == "symbol_table_reference") {
    // The image's static symbol table and the first names it holds, before
    // the image is opened as after.
    const auto reference = engineSymbol<SymbolTableReferenceFunction>(
        "neverd_symbol_table_reference_json");
    if (!reference)
      throw Error("unsupported", "This engine cannot find symbol tables");
    const auto path = stringField(
        p, "path", {},
        ::neverd::limits::worker::
            kEngineExecuteImplSymbolTableReferencePathPathStringFieldMax);
    auto result = backendJson(reference(path.c_str()));
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
        if (!names.is_array() || names.empty() ||
            names.size() > ::neverd::limits::worker::
                               kEngineExecuteImplStringOptionsNamesSizeLimit)
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
            length.get<std::int64_t>() >
                ::neverd::limits::worker::kEngineMaxStringMinLength)
          throw Error(
              "invalid_request",
              "min_length must be an integer from 1 to " +
                  std::to_string(
                      ::neverd::limits::worker::kEngineMaxStringMinLength));
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
      current["min_length"] =
          ::neverd::limits::worker::kEngineDefaultStringMinLength;
    if (!current.contains("preferred"))
      current["preferred"] = nullptr;
    return current;
  }
  if (operation == "contributions")
    return contributions_->listing();
  if (operation == "contribution_register")
    return contributions_->registerFile(stringField(
        p, "path", {},
        ::neverd::limits::worker::
            kEngineExecuteImplContributionRegisterPathStringFieldMax));
  if (operation == "contribution_unregister")
    return contributions_->remove(stringField(
        p, "namespace", {},
        ::neverd::limits::worker::
            kEngineExecuteImplContributionUnregisterNamespaceStringFieldMax));
  if (operation == "contribution_execute") {
    requireLoaded();
    for (auto it = p.begin(); it != p.end(); ++it)
      if (it.key() != "id" && it.key() != "address")
        throw Error("invalid_request",
                    "Contribution execution accepts only id and address");
    const auto id = stringField(
        p, "id", {},
        ::neverd::limits::worker::
            kEngineExecuteImplContributionExecuteIdIdStringFieldMax);
    const auto address = stringField(
        p, "address", hexAddress(neverd_session_entry_addr(session_)),
        ::neverd::limits::worker::
            kEngineExecuteImplContributionExecuteAddressStringFieldMax);
    (void)parseAddress(address);
    const auto query = contributions_->query(id, address);
    return {{"contribution_id", id},
            {"operation", query.at("operation")},
            {"result", execute(query.at("operation").get<std::string>(),
                               query.at("payload"))}};
  }
  if (operation == "open") {
    const auto pathText =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplOpenPathTextPathStringFieldMax);
    if (pathText.empty())
      throw Error("invalid_request", "path is required");
    std::error_code ec;
    const auto path = fs::canonical(utf8Path(pathText), ec);
    if (ec || !fs::is_regular_file(path, ec))
      throw Error("load_failed", "Input is not an accessible regular file");
    for (const char *flag : {"read_only", "debug_info", "analysis", "warm",
                             "step_discovery", "browse_cache"})
      if (p.contains(flag) && !p[flag].is_boolean())
        throw Error("invalid_request", std::string(flag) + " must be boolean");
    if (p.contains("pdb") && !p.at("pdb").is_object())
      throw Error("invalid_request", "PDB selection must be an object");
    // The user's answer for the PDB the input names: none, which leaves the
    // image without one, or the file to load once the image is.
    const bool pdbDeclined =
        p.contains("pdb") && p.at("pdb").value("mode", Json("")) == "none";
    // The user's answer for the DWARF of an ELF or Mach-O image: none, which
    // leaves the image without it, or the file to read it from once the
    // image is open.
    const auto declines = [&](const char *input) {
      return p.contains(input) && p.at(input).value("mode", Json("")) == "none";
    };
    const auto chooses = [&](const char *input) {
      return p.contains(input) && !declines(input);
    };
    if (p.contains("dwarf") &&
        (!p.at("dwarf").is_object() ||
         (!declines("dwarf") && !p.at("dwarf").contains("path"))))
      throw Error("invalid_request",
                  "DWARF selection is {\"mode\":\"none\"} or a file");
    const bool dwarfDeclined = declines("dwarf");
    const bool dwarfChosen = chooses("dwarf");
    // And for the linker MAP beside the input: none, or the file to load once
    // the image is, checked against it or not.
    if (p.contains("map") &&
        (!p.at("map").is_object() ||
         (!declines("map") &&
          (!p.at("map").contains("path") ||
           (p.at("map").value("mode", Json("checked")) != "checked" &&
            p.at("map").value("mode", Json("checked")) != "unchecked")))))
      throw Error("invalid_request",
                  "MAP selection is {\"mode\":\"none\"} or a file");
    const bool mapDeclined = declines("map");
    const bool mapChosen = chooses("map");
    // And for the image's static symbol table: none, which loads the image
    // as if stripped.
    if (p.contains("symbols") &&
        (!p.at("symbols").is_object() ||
         p.at("symbols").value("mode", Json("")) != "none"))
      throw Error("invalid_request",
                  "Symbol table selection is {\"mode\":\"none\"}");
    const bool symbolsDeclined = p.contains("symbols");
    const bool readOnly = p.value("read_only", false);
    // The loader the load dialog chose when the file names none itself: EVM
    // bytecode, or a binary file with its processor and placement, as the
    // engine's load options spell them.
    std::string chosenLoader;
    if (p.contains("loader")) {
      const std::string loader = stringField(
          p, "loader", {},
          ::neverd::limits::worker::kEngineExecuteImplOpenLoaderStringFieldMax);
      if (loader != "binary" && loader != "evm")
        throw Error("invalid_request",
                    "loader is \"binary\", \"evm\" or absent");
      if (!loadOptions())
        throw Error("unsupported", "This engine cannot choose a file's loader");
      Json options{{"loader", loader}};
      if (loader == "binary") {
        options["processor"] = stringField(
            p, "processor", {},
            ::neverd::limits::worker::
                kEngineExecuteImplOpenOptionsProcessorStringFieldMax);
        // The platform whose conventions the code follows; the engine reads
        // it from the code when absent or "auto".
        if (p.contains("platform"))
          options["platform"] = stringField(
              p, "platform", {},
              ::neverd::limits::worker::
                  kEngineExecuteImplOpenOptionsPlatformStringFieldMax);
        for (const char *field : {"base", "offset", "size", "entry"})
          if (p.contains(field))
            options[field] = hexAddress(parseAddress(stringField(
                p, field, {},
                ::neverd::limits::worker::
                    kEngineExecuteImplOpenOptionsFieldStringFieldMax)));
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
    // Searching would load the PDB the user declined, or one the file they
    // chose replaces.
    if (p.contains("pdb")) {
      if (const auto search =
              engineSymbol<PdbSearchFunction>("neverd_session_set_pdb_search"))
        search(next.get(), 0);
      else if (pdbDeclined)
        throw Error("unsupported",
                    "This engine cannot open a PE without its PDB");
    }
    // Reading would load the DWARF the user declined, or one the file they
    // chose replaces.
    if (dwarfDeclined || dwarfChosen) {
      const auto search =
          engineSymbol<DwarfSearchFunction>("neverd_session_set_dwarf_search");
      if (!search)
        throw Error("unsupported",
                    "This engine cannot open an image without its DWARF");
      search(next.get(), 0);
    }
    // The MAP beside the input the user declined or replaced with a file
    // stays unread, and so does one a chosen PDB or DWARF file outranks: the
    // file loads once the image is, and the MAP's names would come first.
    if (const auto search =
            engineSymbol<MapSearchFunction>("neverd_session_set_map_search")) {
      if (mapDeclined || mapChosen || dwarfChosen ||
          (p.contains("pdb") && !pdbDeclined))
        search(next.get(), 0);
    } else if (mapDeclined) {
      throw Error("unsupported",
                  "This engine cannot open an image without its MAP file");
    }
    // Reading would load the symbol table the user declined.
    if (symbolsDeclined) {
      const auto read =
          engineSymbol<SymbolTableFunction>("neverd_session_set_symbol_table");
      if (!read)
        throw Error("unsupported",
                    "This engine cannot open an image without its symbol "
                    "table");
      read(next.get(), 0);
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
    loadAnalysisSections(next.get(), path);
    auto nextCodeEdits =
        readCodeEdits(ownedString(neverd_session_file_path(next.get())));
    neverd_session_destroy(session_);
    session_ = next.release();
    codeEdits_ = std::move(nextCodeEdits);
    // A file too large or unreadable is the next project_sync's to report.
    try {
      typeLibraryChoice_ = typeLibrariesOnDisk().value_or(nullptr);
    } catch (const Error &) {
      typeLibraryChoice_ = nullptr;
    }
    if (!reuseLock)
      lock_ = std::move(nextLock);
    readOnly_ = readOnly;
    browseCacheEnabled_ = p.value("browse_cache", true);
    openOptions_ = p;
    for (const char *input : {"pdb", "dwarf", "map", "symbols"})
      openOptions_.erase(input);
    signatureInputs_ = Json::array();
    pdbInput_ = nullptr;
    dwarfInput_ = dwarfDeclined ? Json{{"mode", "none"}} : Json(nullptr);
    mapInput_ = mapDeclined ? Json{{"mode", "none"}} : Json(nullptr);
    symbolsInput_ = symbolsDeclined ? Json{{"mode", "none"}} : Json(nullptr);
    pdbDisplayNames_ = Json::array();
    backgroundAnalysis_ = p.value("analysis", true);
    steppedDiscovery_ = backgroundAnalysis_ && p.value("step_discovery", false);
    // Preparing functions is the last step of idle-time analysis.
    warmFunctions_ = backgroundAnalysis_ && p.value("warm", false);
    restartWarming();
    dirty_ = false;
    sectionsDirty_ = false;
    analyzed_ = false;
    loadedSize_ = fileSize;
    loadedTime_ = fileTime;
    history_.reset();
    preparedFunction_.reset();
    newListing();
    invalidate();
    ++revision_;
    projectId_ = "project-" + std::to_string(revision_);
    restoredSnapshot_ = nullptr;
    restoredDebugInputs_ = Json::array();
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
        atomicWrite(project_files::path(path, project_files::File::Load),
                    sidecarText(backendJson(loadOptions().json(session_))));
        recordProject();
      } catch (const Error &failure) {
        warnings.push_back(std::string("Load options could not be saved: ") +
                           failure.what());
      }
    }
    if (pdbDeclined) {
      pdbInput_ = {{"mode", "none"}};
    } else if (p.contains("pdb")) {
      // A saved selection names a file outside the project: one that no
      // longer loads leaves the project open without it, and says why.  A
      // matching PDB proves itself again by its GUID and age; only a PDB the
      // user confirmed without that identity stays bound to its file stamp.
      auto selection = p.at("pdb");
      if (selection.value("mode", Json("verified")) == "verified")
        selection.erase("stamp");
      try {
        execute("pdb_load", selection);
      } catch (const std::exception &failure) {
        warnings.push_back(std::string("The project's PDB was not loaded: ") +
                           failure.what());
      }
    }
    // So do the DWARF and MAP files chosen for it, after its PDB.  A file the
    // engine checks against the image proves itself again; only a MAP the
    // user took unchecked stays bound to its file stamp.
    const std::pair<const char *, const char *> chosen[] = {
        {"dwarf", "The project's DWARF file was not loaded: "},
        {"map", "The project's MAP file was not loaded: "}};
    for (const auto &[input, failed] : chosen) {
      if (!(std::string_view(input) == "dwarf" ? dwarfChosen : mapChosen))
        continue;
      auto selection = p.at(input);
      if (selection.value("mode", Json("checked")) != "unchecked")
        selection.erase("stamp");
      try {
        execute(std::string(input) + "_load", selection);
      } catch (const std::exception &failure) {
        warnings.push_back(std::string(failed) + failure.what());
      }
    }
    auto result = metadata();
    for (const auto &diagnostic : result.at("loader_diagnostics"))
      warnings.push_back(diagnostic.value("message", std::string()));
    result["warnings"] = warnings;
    return result;
  }
  requireLoaded();
  if (operation == "analysis_sections") {
    const auto query =
        engineSymbol<AnalysisSections>("neverd_analysis_sections_json");
    if (!query)
      throw Error("unsupported",
                  "The engine does not support analysis sections");
    return backendJson(query(session_, 0), true);
  }
  if (operation == "analysis_section_add") {
    requireWriter();
    const auto add =
        engineSymbol<AddAnalysisSection>("neverd_analysis_section_add");
    if (!add)
      throw Error("unsupported",
                  "The engine does not support analysis sections");
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplAnalysisSectionAddPathStringFieldMax);
    std::vector<unsigned char> bytes;
    if (!path.empty()) {
      if (p.contains("data_hex"))
        throw Error("invalid_request",
                    "Choose a file or inline bytes, not both");
      const auto file = utf8Path(path);
      const auto size = fs::file_size(file);
      if (!size || size > MaxAnalysisSectionBytes)
        throw Error("budget_exceeded",
                    "Analysis section file must contain 1 byte to " +
                        std::to_string(MaxAnalysisSectionBytes >> 20) + " MiB");
      std::ifstream input(file, std::ios::binary);
      bytes.resize(static_cast<std::size_t>(size));
      if (!input.read(reinterpret_cast<char *>(bytes.data()),
                      static_cast<std::streamsize>(size)) ||
          input.peek() != std::char_traits<char>::eof())
        throw Error("load_failed",
                    "Analysis section file changed or could not be read");
    } else {
      bytes = sectionBytes(stringField(p, "data_hex", {}, MaxFrameBytes));
    }
    auto description = p;
    description.erase("path");
    description.erase("data_hex");
    if (!add(session_, description.dump().c_str(), bytes.data(), bytes.size()))
      throw Error("invalid_analysis_section", error());
    changedAnalysisSections();
    return metadata();
  }
  if (operation == "analysis_section_remove") {
    requireWriter();
    const auto remove =
        engineSymbol<RemoveAnalysisSection>("neverd_analysis_section_remove");
    if (!remove)
      throw Error("unsupported",
                  "The engine does not support analysis sections");
    const auto name =
        stringField(p, "name", {}, ::neverd::sdk::kMaxAnalysisSectionNameBytes);
    if (!remove(session_, name.c_str()))
      throw Error("invalid_analysis_section", error());
    changedAnalysisSections();
    return metadata();
  }
  if (operation == unwind_query::kOperation) {
    const auto Query = unwind_query::function();
    if (!Query)
      throw Error(unwind_query::kUnsupported, unwind_query::kUnavailable);
    if (!p.is_object() || p.size() != unwind_query::kRequestFieldCount ||
        !p.contains(unwind_query::kAddress))
      throw Error(unwind_query::kInvalidRequest, unwind_query::kInvalidPayload);
    const auto Address = parseAddress(stringField(p, unwind_query::kAddress));
    // Preserve the exact query PC and both source scopes. The shared JSON
    // adapter only canonicalizes hexadecimal spelling for the wire protocol.
    return backendJson(Query(session_, Address), false);
  }
  if (operation == "pdb_inspect" || operation == "pdb_load") {
    if (operation == "pdb_load" && p.contains("download")) {
      if (!p.at("download").is_object() ||
          p.value("mode", "verified") != "verified")
        throw Error("invalid_request",
                    "Downloaded PDBs require strict verification");
      // Another client may have loaded a PDB while the HTTP request was in
      // flight. Do not replace its selection with the downloaded file.
      if (metadata().at("pdb_download") != p.at("download"))
        throw Error(
            "input_changed",
            "This symbol download no longer applies to the loaded file");
    }
    using Inspect = const char *(*)(neverd_session_t, const char *);
    using Load = int (*)(neverd_session_t, const char *);
    using LoadEx = int (*)(neverd_session_t, const char *, int);
    const auto inspect =
        engineSymbol<Inspect>("neverd_session_inspect_pdb_json");
    const auto load = engineSymbol<Load>("neverd_session_load_pdb");
    const auto loadEx = engineSymbol<LoadEx>("neverd_session_load_pdb_ex");
    if (!inspect || !load)
      throw Error("unsupported", "This engine cannot load a selected PDB");
    const auto path = stringField(
        p, "path", {},
        ::neverd::limits::worker::
            kEngineExecuteImplPdbLoadPdbInspectPathPathStringFieldMax);
    if (path.empty())
      throw Error("invalid_request", "Select a PDB file first");
    if (!fs::is_regular_file(utf8Path(path)))
      throw Error("pdb_load_failed", "The selected PDB is not a regular file");
    const auto stamp = signatureStamp(utf8Path(path));
    if (p.contains("stamp") && p.at("stamp") != stamp)
      throw Error("input_changed", "The selected PDB changed; select it again");
    const auto text = ownedString(inspect(session_, path.c_str()));
    if (text.empty())
      throw Error("pdb_load_failed", error());
    auto symbols = Json::parse(text, nullptr, false);
    if (!symbols.is_object() || !symbols.contains("names"))
      throw Error("pdb_load_failed", "Invalid PDB inspection result");
    const bool verified = symbols.value("verified", false);
    const auto count = symbols.value("name_count", symbols.at("names").size());
    if (signatureStamp(utf8Path(path)) != stamp)
      throw Error("input_changed", "The selected PDB changed while reading it");
    Json result{{"path", path},
                {"verified", verified},
                {"supports_unverified_analysis", loadEx != nullptr},
                {"name_count", count},
                {"stamp", stamp}};
    if (operation == "pdb_inspect")
      return result;
    const auto mode = stringField(
        p, "mode", "verified",
        ::neverd::limits::worker::
            kEngineExecuteImplPdbLoadPdbInspectModeModeStringFieldMax);
    if (mode != "verified" && mode != "analysis" && mode != "display")
      throw Error("invalid_request",
                  "PDB mode is verified, analysis or display");
    if (!verified && mode == "verified")
      throw Error(
          "pdb_identity_missing",
          "The PE has no PDB identity; confirm using this PDB for analysis");
    const bool forAnalysis = verified || mode == "analysis";
    if (forAnalysis) {
      if (!verified && !loadEx)
        throw Error("unsupported",
                    "This engine cannot use an unverified PDB for analysis");
      const bool loaded = verified ? load(session_, path.c_str()) != 0
                                   : loadEx(session_, path.c_str(), 1) != 0;
      if (!loaded)
        throw Error("pdb_load_failed", error());
      pdbDisplayNames_ = Json::array();
      analyzed_ = false;
      preparedFunction_.reset();
      listing_.reset();
    } else {
      pdbDisplayNames_ = std::move(symbols.at("names"));
      if (listing_)
        listing_->setPdbDisplayNames(pdbDisplayNames_);
    }
    pdbInput_ = {{"path", path},
                 {"mode", verified ? "verified" : mode},
                 {"verified", verified},
                 {"name_count", count},
                 {"stamp", stamp}};
    invalidate();
    ++revision_;
    result["function_count"] = neverd_func_count(session_);
    result["mode"] = pdbInput_.at("mode");
    result["analysis"] = forAnalysis;
    result["loaded"] = true;
    return result;
  }
  if (operation == "map_inspect" || operation == "map_load") {
    const auto inspect =
        engineSymbol<MapInspectFunction>("neverd_session_inspect_map_json");
    const auto load = engineSymbol<MapLoadFunction>("neverd_session_load_map");
    if (!inspect || !load)
      throw Error("unsupported", "This engine cannot load a selected MAP file");
    const auto path = stringField(
        p, "path", {},
        ::neverd::limits::worker::
            kEngineExecuteImplMapLoadMapInspectPathPathStringFieldMax);
    if (path.empty())
      throw Error("invalid_request", "Select a MAP file first");
    if (!fs::is_regular_file(utf8Path(path)))
      throw Error("map_load_failed",
                  "The selected MAP file is not a regular file");
    const auto stamp = signatureStamp(utf8Path(path));
    if (p.contains("stamp") && p.at("stamp") != stamp)
      throw Error("input_changed",
                  "The selected MAP file changed; select it again");
    const auto text = ownedString(inspect(session_, path.c_str()));
    if (text.empty())
      throw Error("map_load_failed", error());
    auto result = Json::parse(text, nullptr, false);
    if (!result.is_object() || !result.contains("match"))
      throw Error("map_load_failed", "Invalid MAP inspection result");
    result["stamp"] = stamp;
    if (operation == "map_inspect")
      return result;
    const auto mode = stringField(
        p, "mode", "checked",
        ::neverd::limits::worker::
            kEngineExecuteImplMapLoadMapInspectModeModeStringFieldMax);
    if (mode != "checked" && mode != "unchecked")
      throw Error("invalid_request", "MAP mode is checked or unchecked");
    if (mode == "checked" && result.value("match", "") == "mismatch")
      throw Error("map_mismatch", "The MAP file describes another build; "
                                  "confirm loading it unchecked");
    if (!load(session_, path.c_str(), mode == "unchecked" ? 1 : 0))
      throw Error("map_load_failed", error());
    if (signatureStamp(utf8Path(path)) != stamp)
      throw Error("input_changed",
                  "The selected MAP file changed while reading it");
    mapInput_ = {{"path", path}, {"mode", mode}, {"stamp", stamp}};
    analyzed_ = false;
    preparedFunction_.reset();
    listing_.reset();
    invalidate();
    ++revision_;
    result["mode"] = mode;
    result["loaded"] = true;
    result["function_count"] = neverd_func_count(session_);
    result["debug_info"] = metadata().at("debug_info");
    return result;
  }
  if (operation == "dwarf_load") {
    const auto load =
        engineSymbol<DwarfLoadFunction>("neverd_session_load_dwarf");
    if (!load)
      throw Error("unsupported",
                  "This engine cannot load a selected DWARF file");
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplDwarfLoadPathPathStringFieldMax);
    if (path.empty())
      throw Error("invalid_request", "Select a DWARF file first");
    // A .dSYM bundle is a directory; its DWARF file proves itself by UUID.
    std::error_code ec;
    const bool bundle = fs::is_directory(utf8Path(path), ec);
    if (!bundle && !fs::is_regular_file(utf8Path(path), ec))
      throw Error("dwarf_load_failed",
                  "The selected DWARF file is not a regular file");
    const auto stamp = bundle ? Json() : signatureStamp(utf8Path(path));
    if (p.contains("stamp") && p.at("stamp") != stamp)
      throw Error("input_changed",
                  "The selected DWARF file changed; select it again");
    if (!load(session_, path.c_str()))
      throw Error("dwarf_load_failed", error());
    dwarfInput_ = {{"path", path}, {"stamp", stamp}};
    analyzed_ = false;
    preparedFunction_.reset();
    listing_.reset();
    invalidate();
    ++revision_;
    return {{"path", path},
            {"stamp", stamp},
            {"loaded", true},
            {"function_count", neverd_func_count(session_)},
            {"debug_info", metadata().at("debug_info")}};
  }
  if (operation == "signatures_load") {
    const auto path =
        stringField(p, "path", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplSignaturesLoadPathPathStringFieldMax);
    const auto mode =
        stringField(p, "mode", "file",
                    ::neverd::limits::worker::
                        kEngineExecuteImplSignaturesLoadModeModeStringFieldMax);
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
    const auto limit = sizeField(
        p, "limit",
        ::neverd::limits::worker::
            kEngineExecuteImplHistoryLimitSizeFieldDefault,
        ::neverd::limits::worker::kEngineExecuteImplHistoryLimitSizeFieldMax);
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
  if (operation == "project_sync") {
    requireWriter();
    // The project's files changed outside this worker, as git pull, another
    // tool or a person changes them: the Session takes them as they are now
    // and its undo history starts there.  Comments staged here stay staged
    // on top of them.  Files this worker wrote itself are no change.
    auto &store = history();
    const bool tables = !store.current();
    const bool types = localTypesChanged();
    const bool libraries = typeLibrariesChanged();
    std::size_t kept = 0;
    if (tables) {
      std::map<std::uint64_t, std::string> committed, staged;
      if (dirty_) {
        for (const auto &row : store.committedState().at("annotations"))
          committed[parseAddress(row.at("addr").get<std::string>())] =
              row.at("text").get<std::string>();
        std::set<std::uint64_t> current;
        for (const auto &row : backendJson(neverd_annotations_json(session_))) {
          const auto address = parseAddress(row.at("addr").get<std::string>());
          const auto text = row.at("text").get<std::string>();
          current.insert(address);
          if (const auto it = committed.find(address);
              it == committed.end() || it->second != text)
            staged[address] = text;
        }
        for (const auto &[address, text] : committed)
          if (!current.contains(address))
            staged[address] = std::string();
      }
      store.reset();
      dirty_ = false;
      if (!reloadUserState())
        throw Error("reload_failed",
                    "The project's files changed but could not be loaded",
                    {{"state_changed", true}});
      for (const auto &[address, text] : staged) {
        // A comment the files now hold as staged here needs no save.
        auto before = ownedString(neverd_annotation_get(session_, address));
        if (before == text)
          continue;
        store.stage({{"kind", "annotation"},
                     {"address", hexAddress(address)},
                     {"before", std::move(before)},
                     {"after", text}});
        neverd_annotation_set(session_, address, text.c_str());
        ++kept;
      }
      dirty_ = kept != 0 || sectionsDirty_;
    }
    // The history holds the declarations too: a change of them is a change
    // of its tables, which reloadUserState read.
    if (types && !tables) {
      static const auto load =
          engineSymbol<SessionLoadFunction>("neverd_type_declarations_load");
      if (load(session_) != 0)
        throw Error("load_failed", error(), {{"state_changed", tables}});
    }
    // The type libraries are no history's: the engine reads them again.
    if (libraries) {
      static const auto load =
          engineSymbol<SessionLoadFunction>("neverd_type_libraries_load");
      if (load(session_) != 0)
        throw Error("load_failed", error(), {{"state_changed", tables}});
      typeLibraryChoice_ = typeLibrariesOnDisk().value_or(nullptr);
    }
    if (types || libraries) {
      analyzed_ = false;
      preparedFunction_.reset();
    }
    if (tables || types || libraries) {
      invalidate();
      ++revision_;
    }
    return {{"changed", tables || types || libraries},
            {"tables", tables},
            {"local_types", types},
            {"type_libraries", libraries},
            {"comments_kept", kept},
            {"dirty", dirty_}};
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
      // Replay only the changed rows. Patch handles retain immutable values
      // across checkpoints; other edits keep their existing JSON path.
      Json changes = command.value("rows", Json::array());
      changes.push_back(command);
      const auto replay = [&](auto &rows, auto key, auto own) {
        for (const auto &change : changes) {
          const auto at = parseAddress(change.at("address").get<std::string>());
          const auto position = std::lower_bound(
              rows.begin(), rows.end(), at, [&](const auto &row, auto address) {
                return key(row) < address;
              });
          const bool present = position != rows.end() && key(*position) == at;
          const auto &side = change.at(redo ? "after" : "before");
          if (side.is_null()) {
            if (present)
              rows.erase(position);
          } else if (present)
            *position = own(side);
          else
            rows.insert(position, own(side));
        }
      };
      const bool patch = command.at("kind") == "patch";
      Json state;
      ProjectHistory::PatchRows patches;
      if (patch) {
        patches = store.committedPatches();
        replay(
            patches, [](const auto &row) { return row->address(); },
            ProjectHistory::patchRow);
      } else {
        state = store.committedState();
        auto &rows = state[ProjectHistory::tableOfKind(
            command.at("kind").get<std::string>())];
        replay(
            rows,
            [](const Json &row) {
              return parseAddress(
                  row.at("addr").get_ref<const std::string &>());
            },
            [](const Json &row) { return row; });
      }
      const auto checkpoint = store;
      store.advance(redo);
      try {
        if (patch)
          store.persistPatches(std::move(patches));
        else
          store.persist(std::move(state));
      } catch (...) {
        store = checkpoint;
        throw;
      }
      if (!reloadUserState(command.at("kind") == "patch"))
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
    result["address"] = hexAddress(ProjectHistory::displayAddress(command));
    return result;
  }
  if (operation == "analyze") {
    analyze();
    return metadata();
  }
  if (operation == "functions" || operation == "resolve" ||
      operation == "disasm")
    analyzeBeforeFunctionQueries();
  if (operation == "functions") {
    const auto limit = sizeField(
        p, "limit",
        ::neverd::limits::worker::
            kEngineExecuteImplFunctionsLimitSizeFieldDefault,
        ::neverd::limits::worker::kEngineExecuteImplFunctionsLimitSizeFieldMax);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    const auto filter = folded(stringField(p, "filter"));
    const auto sort =
        stringField(p, "sort", p.contains("from") ? "address" : "",
                    ::neverd::limits::worker::
                        kEngineExecuteImplFunctionsSortSortStringFieldMax);
    const bool descending = p.value("descending", false);
    if (p.contains("from") && sort != "address")
      throw Error("invalid_request",
                  "Function navigation requires address order");
    auto &listingView = listing();
    const auto &rows = listingView.functionRows();
    if (rows.size() > ::neverd::limits::worker::kEngineMaxFunctionRows)
      throw Error("budget_exceeded",
                  "The function index exceeds one million entries");
    std::string key = filter;
    key += '\0';
    key += sort;
    key += '\0';
    key += descending ? 'd' : 'a';
    key += '\0';
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
    auto first = functionOrder_.begin();
    if (p.contains("from")) {
      const auto from = parseAddress(stringField(p, "from"));
      first = std::upper_bound(
          first, functionOrder_.end(), from,
          [&](std::uint64_t address, std::size_t index) {
            const auto candidate =
                parseAddress(rows[index]["address"].get<std::string>());
            return descending ? address > candidate : address < candidate;
          });
    }
    const auto start = std::size_t(first - functionOrder_.begin());
    const auto total = functionOrder_.size() - start;
    const auto position = tablePagePosition(
        p, total, limit,
        [&](std::size_t index) -> std::optional<TableRowRange> {
          return TableRowRange{
              parseAddress(rows[functionOrder_[start + index]]["address"]
                               .get<std::string>())};
        });
    const auto offset = position.offset;
    Json items = Json::array();
    for (auto i = std::min(offset, total); i < total && items.size() < limit;
         ++i) {
      const auto &row = rows[functionOrder_[start + i]];
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
    Json result{{"items", items},
                {"offset", offset},
                {"total", total},
                {"complete", complete},
                {"next_offset",
                 complete ? Json(nullptr) : Json(offset + items.size())}};
    position.annotate(result);
    return result;
  }
  if (operation == "resolve") {
    const auto query = stringField(p, "query");
    if (query.empty())
      throw Error("invalid_request", "query is required");
    int index = -1;
    std::uint64_t address = 0;
    if (query.starts_with("0x") || query.starts_with("0X")) {
      address = parseAddress(query);
      index = functionIndex(address);
    } else if (const auto named = resolvedName(query)) {
      std::tie(index, address) = *named;
    } else {
      throw Error("not_found", "No function or name matches that symbol");
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
  if (operation == "local_types_declare" || operation == "local_types_forget") {
    requireWriter();
    if (!declaresLocalTypes())
      throw Error("unsupported", "This engine cannot declare local types");
    // A declaration commits at once, as a rename does, over saved comments,
    // into a history that holds the declarations as they stand before it.
    if (dirty_)
      throw Error("unsaved_changes",
                  "Save or reload staged annotations before declaring types");
    history().requireUsable();
    static const auto declare =
        engineSymbol<LocalTypeFunction>("neverd_local_types_declare");
    static const auto forget =
        engineSymbol<LocalTypeNamesFunction>("neverd_local_types_forget");
    static const auto load =
        engineSymbol<SessionLoadFunction>("neverd_type_declarations_load");
    std::string reply;
    if (operation == "local_types_declare") {
      const auto source =
          stringField(p, "source", {},
                      ::neverd::limits::worker::kEngineMaxDeclarationBytes);
      const Json options{
          {"language",
           stringField(
               p, "language", "c",
               ::neverd::limits::worker::
                   kEngineExecuteImplLocalTypesForgetLocalTypesDeclareOptionsLanguageStringFieldMax)}};
      reply = ownedString(
          declare(session_, source.c_str(), options.dump().c_str()));
    } else {
      const auto names = p.value("names", Json());
      if (!names.is_array() || names.empty() ||
          names.size() >
              ::neverd::limits::worker::
                  kEngineExecuteImplLocalTypesForgetLocalTypesDeclareNamesSizeLimit)
        throw Error("invalid_request", "Name the types to remove in a list");
      reply = ownedString(forget(session_, names.dump().c_str()));
    }
    if (reply.empty())
      throw Error("local_types_failed", error());
    Json result;
    try {
      result = Json::parse(reply);
    } catch (const Json::exception &) {
      throw Error("engine_error", "Engine returned malformed JSON");
    }
    // The project keeps what the user declared at once, as a rename, and
    // undo takes it away again: the declaration after the last.  The
    // sidecar unchanged, so are the types.
    if (result.value("problems", Json::array()).empty()) {
      auto &store = *history_;
      const auto checkpoint = store;
      try {
        auto state = store.committedState();
        Json declared = typeDeclarationRows();
        if (declared.empty())
          throw Error("engine_error", "Engine kept no declaration");
        store.stage({{"kind", "types"},
                     {"address", hexAddress(declared.size() - 1)},
                     {"before", nullptr},
                     {"after", declared.back()}});
        state["types"] = std::move(declared);
        store.persist(state);
      } catch (...) {
        store = checkpoint;
        (void)load(session_);
        throw;
      }
    }
    // Structure offsets name the members of the types as they are now.
    if (result.value("problems", Json::array()).empty()) {
      operandFormatsChanged();
      namesChanged();
      if (!result.value("decompilation", false))
        ++revision_;
    }
    // A prototype the decompiler reads, or a type it names, changed: the
    // engine let its analysis go, as loading signatures does, and every view
    // reads anew.
    if (result.value("decompilation", false)) {
      analyzed_ = false;
      preparedFunction_.reset();
      invalidate();
      ++revision_;
    }
    return result;
  }
  if (operation == "local_types_fit") {
    if (!fitsLocalTypes())
      throw Error("unsupported", "This engine cannot fit local types to code");
    // The request the C API reads: the search, and what the code does
    // through the pointer, the instruction's at "address" in "function"
    // among them.
    Json request = Json::object();
    for (const char *key : {"text", "spelling"})
      if (p.contains(key))
        request[key] = stringField(
            p, key, {},
            ::neverd::limits::worker::
                kEngineExecuteImplLocalTypesFitRequestKeyKeyStringFieldMax);
    for (const char *key : {"records", "shifts"})
      if (p.contains(key)) {
        if (!p.at(key).is_boolean())
          throw Error("invalid_request",
                      std::string(key) + " must be true or false");
        request[key] = p.at(key);
      }
    if (p.contains("limit"))
      request["limit"] = sizeField(
          p, "limit",
          ::neverd::limits::worker::
              kEngineExecuteImplLocalTypesFitRequestLimitSizeFieldDefault,
          ::neverd::limits::worker::
              kEngineExecuteImplLocalTypesFitRequestLimitSizeFieldMax);
    if (p.contains("accesses")) {
      const auto &accesses = p.at("accesses");
      if (!accesses.is_array() ||
          accesses.size() >
              ::neverd::limits::worker::
                  kEngineExecuteImplLocalTypesFitAccessesSizeLimit)
        throw Error("invalid_request",
                    "accesses must be a list of at most 4096 accesses");
      request["accesses"] = accesses;
    }
    // The operand a structure offset would change: a displacement is a
    // pointer's, whose accesses the engine reads in the function; a number
    // is an offset by itself.
    std::optional<std::size_t> operand;
    std::optional<Json> own;
    std::string base;
    if (p.contains("address") && p.contains("parameter"))
      throw Error("invalid_request",
                  "A fit follows an instruction's pointer or a parameter, "
                  "not both");
    if (p.contains("parameter")) {
      request["function"] = hexAddress(parseAddress(stringField(
          p, "function", {},
          ::neverd::limits::worker::
              kEngineExecuteImplLocalTypesFitRequestFunctionStringFieldMax)));
      request["target"] = {{"parameter", sizeField(p, "parameter", 0, 255)}};
    }
    if (p.contains("address")) {
      const auto address = parseAddress(stringField(p, "address"));
      std::optional<std::int64_t> number;
      operand = listing().structOperand(
          address,
          p.contains("operand")
              ? std::optional<std::size_t>(sizeField(
                    p, "operand", 0,
                    ::neverd::limits::worker::
                        kEngineExecuteImplLocalTypesFitOperandSizeFieldMax))
              : std::nullopt,
          &number);
      if (!operand)
        throw Error("invalid_request",
                    "The instruction at " + hexAddress(address) +
                        " has no number or displacement from a register to "
                        "show as a structure offset");
      if (number) {
        if (!request.contains("accesses"))
          request["accesses"] = Json::array();
        request["accesses"].push_back(
            {{"offset", *number}, {"size", 0}, {"use", "address"}});
      } else if (const auto access = listing().ownAccess(address, *operand)) {
        // The engine follows no SBF pointer: the instruction's own access is
        // the one a member must hold.
        own = Json{{"offset", access->offset},
                   {"size", access->bytes},
                   {"use", access->write ? "write" : "read"}};
        if (!request.contains("accesses"))
          request["accesses"] = Json::array();
        request["accesses"].push_back(*own);
        (*own)["address"] = hexAddress(address);
        base = access->base;
      } else {
        request["function"] = hexAddress(parseAddress(stringField(
            p, "function", {},
            ::neverd::limits::worker::
                kEngineExecuteImplLocalTypesFitRequestFunctionStringFieldMax)));
        request["target"] = {{"address", hexAddress(address)}};
      }
    }
    const auto fit =
        engineSymbol<LocalTypesFunction>("neverd_local_types_fit_json");
    const auto reply = ownedString(fit(session_, request.dump().c_str()));
    if (reply.empty())
      throw Error("local_types_failed", error());
    Json result;
    try {
      result = Json::parse(reply);
    } catch (const Json::exception &) {
      throw Error("engine_error", "Engine returned malformed JSON");
    }
    if (operand)
      result["operand"] = *operand;
    if (own)
      result["target"] = {{"base", base}, {"accesses", Json::array({*own})}};
    // How the listing writes the operand the fit would change.
    if (const auto syntax = listing().operandSyntax(); !syntax.empty())
      result["syntax"] = std::string(syntax);
    return result;
  }
  if (operation == "type_libraries" || operation == "type_libraries_set") {
    if (!choosesTypeLibraries())
      throw Error("unsupported", "This engine chooses no type libraries");
    const auto reply = [this](const std::string &text) {
      if (text.empty())
        throw Error("type_libraries_failed", error());
      try {
        return Json::parse(text);
      } catch (const Json::exception &) {
        throw Error("engine_error", "Engine returned malformed JSON");
      }
    };
    if (operation == "type_libraries") {
      static const auto list =
          engineSymbol<SessionJsonFunction>("neverd_type_libraries_json");
      return reply(ownedString(list(session_)));
    }
    requireWriter();
    // The libraries joined beyond the image's own and those left out, by
    // name: the engine keeps them in libraries.json, outside the history.
    Json choice = {{"added", Json::array()}, {"removed", Json::array()}};
    for (const char *key : {"added", "removed"})
      if (p.contains(key)) {
        if (!p.at(key).is_array() ||
            p.at(key).size() >
                ::neverd::limits::worker::
                    kEngineExecuteImplTypeLibrariesSetTypeLibrariesPAtKeySizeLimit)
          throw Error("invalid_request",
                      std::string(key) + " must be an array of names");
        for (const auto &name : p.at(key))
          choice[key].push_back(stringField(
              Json{{"name", name}}, "name", {},
              ::neverd::limits::worker::
                  kEngineExecuteImplTypeLibrariesSetTypeLibrariesNameStringFieldMax));
      }
    static const auto set =
        engineSymbol<LocalTypesFunction>("neverd_type_libraries_set");
    Json result = reply(ownedString(set(session_, choice.dump().c_str())));
    analyzed_ = false;
    preparedFunction_.reset();
    invalidate();
    ++revision_;
    // The writer lock this session holds covers the write, which the
    // engine's own save would take again.
    const Json kept = {{"added", result.value("added", Json::array())},
                       {"removed", result.value("removed", Json::array())}};
    try {
      atomicWrite(typeLibrariesPath(), sidecarText(kept));
    } catch (const Error &) {
      // The engine joins again what the file still says.
      static const auto load =
          engineSymbol<SessionLoadFunction>("neverd_type_libraries_load");
      (void)load(session_);
      throw;
    }
    recordProject();
    typeLibraryChoice_ = kept;
    return result;
  }
  if (operation == "local_types" || operation == "local_type" ||
      operation == "local_types_source") {
    if (!listsLocalTypes())
      throw Error("unsupported", "This engine has no local types");
    // Type names and sizes are no addresses: the reply stays as it is.
    const auto localTypesJson = [this](const std::string &reply) {
      if (reply.empty())
        throw Error("local_types_failed", error());
      try {
        return Json::parse(reply);
      } catch (const Json::exception &) {
        throw Error("engine_error", "Engine returned malformed JSON");
      }
    };
    // The options the C API reads, as the request states them.
    Json options = Json::object();
    for (const char *key : {"filter", "language", "spelling", "origin"})
      if (p.contains(key))
        options[key] = stringField(
            p, key, {},
            ::neverd::limits::worker::
                kEngineExecuteImplLocalTypesSourceLocalTypesOptionsKeyKeyStringFieldMax);
    for (const char *key :
         {"offsets", "hexOffsets", "asserts", "dependencies", "prototypes"})
      if (p.contains(key)) {
        if (!p.at(key).is_boolean())
          throw Error("invalid_request",
                      std::string(key) + " must be true or false");
        options[key] = p.at(key);
      }
    if (p.contains("types")) {
      if (!p.at("types").is_array())
        throw Error("invalid_request", "types must be an array of names");
      for (const auto &name : p.at("types"))
        if (!name.is_string())
          throw Error("invalid_request", "types must be an array of names");
      options["types"] = p.at("types");
    }
    const std::string text = options.dump();
    if (operation == "local_types") {
      const auto list =
          engineSymbol<LocalTypesFunction>("neverd_local_types_json");
      return localTypesJson(ownedString(list(session_, text.c_str())));
    }
    if (operation == "local_type") {
      const auto one =
          engineSymbol<LocalTypeFunction>("neverd_local_type_json");
      const auto name = stringField(
          p, "name", {},
          ::neverd::limits::worker::
              kEngineExecuteImplLocalTypesSourceLocalTypesLocalTypeNameNameStringFieldMax);
      return localTypesJson(
          ownedString(one(session_, name.c_str(), text.c_str())));
    }
    const auto source =
        engineSymbol<LocalTypesFunction>("neverd_local_types_source");
    const auto reply = ownedString(source(session_, text.c_str()));
    if (reply.empty())
      throw Error("local_types_failed", error());
    return Json{{"language", options.value("language", "c")},
                {"source", reply}};
  }
  if (operation == "strings")
    return listing().strings(p);
  if (operation == "segments") {
    auto items = backendJson(neverd_segments_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("va");
      item.erase("va");
    }
    return page(items, p, {"name"}, PageAddressMatch::SegmentRange);
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
    return page(items, p, {"name"}, PageAddressMatch::RegionRange);
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
    const auto kind =
        stringField(p, "kind", "text",
                    ::neverd::limits::worker::
                        kEngineExecuteImplSearchKindKindStringFieldMax);
    const auto pattern =
        stringField(p, "pattern", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplSearchPatternStringFieldMax);
    const auto limit = sizeField(
        p, "limit",
        ::neverd::limits::worker::kEngineExecuteImplSearchLimitSizeFieldDefault,
        ::neverd::limits::worker::kEngineExecuteImplSearchLimitSizeFieldMax);
    if (pattern.empty() || !limit)
      throw Error("invalid_request", "A search pattern and limit are required");
    if (kind != "text" && kind != "bytes" && kind != "immediate")
      throw Error("invalid_request",
                  "Search kind must be bytes, text or immediate");
    using SearchPage = const char *(*)(neverd_session_t, const char *);
    const auto search = engineSymbol<SearchPage>("neverd_search_page");
    if (!search)
      throw Error("unsupported",
                  "This engine does not support cursor-based search");
    Json options{{"kind", kind}, {"pattern", pattern}, {"limit", limit}};
    for (const char *field : {"from", "backward", "case_sensitive"})
      if (p.contains(field))
        options[field] = p[field];
    const auto *reply = search(session_, options.dump().c_str());
    if (!reply)
      throw Error("invalid_request", error());
    auto result = backendJson(reply);
    for (auto &hit : result["items"]) {
      hit["address"] = hit.at("addr");
      hit.erase("addr");
    }
    return result;
  }
  if (operation == "annotations") {
    auto items = backendJson(neverd_annotations_json(session_));
    for (auto &item : items) {
      item["address"] = item.at("addr");
      item.erase("addr");
    }
    return page(items, p, {"text"});
  }
  if (operation == "register_flow") {
    using Query = const char *(*)(neverd_session_t, neverd_va_t, neverd_va_t,
                                  const char *);
    const auto query = engineSymbol<Query>("neverd_register_flow_json");
    if (!query)
      throw Error("unsupported", "This engine cannot query register flow");
    const auto entry = parseAddress(stringField(p, "address"));
    const auto pc = parseAddress(stringField(p, "instruction"));
    Json options{
        {"kind",
         stringField(
             p, "kind", "registers",
             ::neverd::limits::worker::
                 kEngineExecuteImplRegisterFlowOptionsKindKindStringFieldMax)},
        {"register",
         stringField(
             p, "register", {},
             ::neverd::limits::worker::
                 kEngineExecuteImplRegisterFlowOptionsRegisterStringFieldMax)}};
    const auto *raw = query(session_, entry, pc, options.dump().c_str());
    if (!raw)
      throw Error("register_flow_failed", error());
    return backendJson(raw);
  }
  if (operation == "stack_frame") {
    const auto address = parseAddress(stringField(p, "address"));
    using Inspect =
        const char *(*)(neverd_session_t, neverd_va_t, const char *);
    const auto inspect = engineSymbol<Inspect>("neverd_stack_frame_json");
    if (!inspect)
      throw Error("unsupported", "This engine cannot inspect stack frames");
    const auto row = codeEditRow(address);
    const auto policy = row.is_object()
                            ? row.value("stack_frame", Json::object())
                            : Json::object();
    std::string stale;
    const char *raw = nullptr;
    if (policy.empty() ||
        policy.value("input_sha256", std::string()) == inputHash())
      raw = inspect(session_, address, policy.dump().c_str());
    if (!raw) {
      stale = policy.value("input_sha256", std::string()) != inputHash()
                  ? "The saved stack policy belongs to a different input image"
                  : error();
      raw = inspect(session_, address, "{}");
    }
    if (!raw)
      throw Error("stack_frame_failed", error());
    auto result = backendJson(raw);
    result["policy"] = policy;
    result["stale"] = stale;
    return result;
  }
  if (operation == "call_sites") {
    const auto address = parseAddress(stringField(p, "address"));
    const auto representation = stringField(p, "representation");
    if (representation != "c" && representation != "source" &&
        representation != "llvmc")
      throw Error("unsupported",
                  "Call-site declarations require Pseudocode or LLVM C");
    const auto document = namedView(address, representation,
                                    p.value("source_options", Json::object()));
    if (!document)
      throw Error("unsupported",
                  "The engine does not expose original call occurrences");
    const auto row = codeEditRow(address);
    return {{"calls", document->value.value("source_calls", Json::array())},
            {"snapshot", document->value.value("call_snapshot", std::string())},
            {"types", row.is_object() ? row.value("call_types", Json::object())
                                      : Json::object()}};
  }
  if (operation == "source_variables") {
    const auto address = parseAddress(stringField(p, "address"));
    const auto representation = stringField(p, "representation");
    if (representation != "c" && representation != "source" &&
        representation != "llvmc")
      throw Error("unsupported",
                  "Variable associations require Pseudocode or LLVM C");
    const auto document = namedView(address, representation,
                                    p.value("source_options", Json::object()));
    if (!document)
      throw Error("unsupported", "The engine does not expose source variables");
    Json result{
        {"variables", document->value.value("source_variables", Json::array())},
        {"snapshot", document->value.value("variable_snapshot", std::string())},
        {"results",
         document->value.value("variable_edit_results", Json::array())}};
    const auto row = codeEditRow(address);
    const auto route = representation == "source" ? "c" : representation;
    result["associations"] = row.is_object()
                                 ? row.value("views", Json::object())
                                       .value(route, Json::object())
                                       .value("variable_edits", Json::array())
                                 : Json::array();
    return result;
  }
  if (operation == "source_type") {
    const auto address = parseAddress(stringField(p, "address"));
    const auto representation = stringField(p, "representation");
    const auto document = namedView(address, representation,
                                    p.value("source_options", Json::object()));
    if (!document)
      throw Error("unsupported", "The engine does not expose source variables");
    Json request = p;
    request["kind"] = "type";
    CodeEdits::validateChange(document->value, request);
    const auto id =
        stringField(p, "variable_id", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplSourceTypeIdVariableIdStringFieldMax);
    Json variable;
    for (const auto &item : document->value.at("source_variables"))
      if (item.at("id") == id)
        variable = item;
    const auto row = codeEditRow(
        variable.contains("address")
            ? parseAddress(variable.at("address").get<std::string>())
            : address);
    const auto route = representation == "source" ? "c" : representation;
    const auto edits = row.is_object()
                           ? row.value("views", Json::object())
                                 .value(route, Json::object())
                                 .value("variable_types", Json::object())
                           : Json::object();
    const auto stated = variable.contains("address") && row.is_object()
                            ? row.value("object_type", Json::object())
                            : edits.value(id, Json::object());
    Json result{
        {"input_sha256", inputHash()},
        {"stated", !stated.empty()},
        {"name", variable.at("name")},
        {"type", stated.value("type", variable.value("type", std::string()))},
        {"bytes", variable.at("bytes")},
        {"floating", variable.at("floating")}};
    if (variable.contains("address"))
      result["object_address"] = variable.at("address");
    if (p.contains("type")) {
      using Check = const char *(*)(neverd_session_t, const char *);
      const auto check = engineSymbol<Check>("neverd_source_type_json");
      if (!check)
        throw Error("unsupported",
                    "The engine cannot check source variable types");
      const Json proposal{
          {"type",
           stringField(
               p, "type", {},
               ::neverd::limits::worker::
                   kEngineExecuteImplSourceTypeTypeProposalTypeStringFieldMax)},
          {"bytes", variable.at("bytes")},
          {"floating", variable.at("floating")}};
      const auto *reply = check(session_, proposal.dump().c_str());
      if (!reply)
        throw Error("invalid_type", error());
      result["type"] = backendJson(reply).at("type");
    }
    return result;
  }
  if (operation == "code_edit_prepare") {
    requireLoaded();
    if (p.value("kind", std::string()) != "name")
      throw Error("invalid_request", "Only a local name can be prepared");
    const auto document = namedView(parseAddress(stringField(p, "address")),
                                    stringField(p, "representation"),
                                    p.value("source_options", Json::object()));
    if (!document)
      throw Error("unsupported",
                  "The engine does not expose editable source targets");
    CodeEdits::validateChange(document->value, p);
    return {{"ready", true}};
  }
  if (operation == "code_edit") {
    requireWriter();
    if (dirty_)
      throw Error("unsaved_changes", "Save staged comments before a code edit");
    const auto address = parseAddress(stringField(p, "address"));
    auto &store = history();
    Json before = codeEditRow(address);
    const bool frameEdit = p.value("kind", std::string()) == "stack_frame";
    const auto document =
        frameEdit
            ? decltype(namedView(address, std::string(), Json::object()))()
            : namedView(address, stringField(p, "representation"),
                        p.value("source_options", Json::object()));
    if (!frameEdit && !document)
      throw Error("unsupported",
                  "The engine does not expose editable source targets");
    const bool associations = p.value("kind", std::string()) == "variables";
    if (!associations && !frameEdit)
      CodeEdits::validateChange(document->value, p);
    // The names keep while the code keeps its locals (code_shape).
    Json request = p;
    std::uint64_t owner = address;
    if (frameEdit) {
      request["representation"] = "c";
      if (!p.value("reset", false)) {
        auto frame = p.value("stack_frame", Json::object());
        using Inspect =
            const char *(*)(neverd_session_t, neverd_va_t, const char *);
        const auto inspect = engineSymbol<Inspect>("neverd_stack_frame_json");
        if (!inspect)
          throw Error("unsupported", "This engine cannot correct stack frames");
        const auto *reply = inspect(session_, address, frame.dump().c_str());
        if (!reply)
          throw Error("stack_frame_refused", error());
        (void)backendJson(reply);
        frame["input_sha256"] = inputHash();
        if (!frame.value("variables", Json::array()).empty())
          for (const auto *route : {"c", "llvmc"}) {
            const auto preview = namedView(address, route, Json::object(),
                                           nullptr, nullptr, &frame);
            if (!preview)
              throw Error("stack_frame_refused",
                          "This frame cannot be named in both C views");
          }
        request["stack_frame"] = std::move(frame);
      }
    }
    if (p.value("kind", std::string()) == "call_type") {
      const bool clear = p.value("action", std::string()) == "clear";
      const auto id =
          clear
              ? std::string()
              : stringField(
                    p, "call_id", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplCodeEditCallTypeKindIdCallIdStringFieldMax);
      Json calls = before.is_object()
                       ? before.value("call_types", Json::object())
                       : Json::object();
      if (clear)
        calls = Json::object();
      else if (p.value("reset", false))
        calls.erase(id);
      else {
        Json actuals = p.value("variadic_types", Json::array());
        if (!actuals.is_array() ||
            actuals.size() >
                ::neverd::limits::worker::
                    kEngineExecuteImplCodeEditCallTypeKindActualsSizeLimit)
          throw Error("invalid_request", "Invalid actual variadic types");
        for (const auto &type : actuals)
          if (!type.is_string() ||
              type.get_ref<const std::string &>().empty() ||
              type.get_ref<const std::string &>().size() >
                  ::neverd::limits::worker::
                      kEngineExecuteImplCodeEditCallTypeKindTypeGetRefConstStdStringSizeLimit)
            throw Error("invalid_request",
                        "Each actual variadic argument needs a bounded type");
        calls[id] = {
            {"input_sha256", inputHash()},
            {"snapshot", p.at("snapshot")},
            {"declaration",
             stringField(
                 p, "declaration", {},
                 ::neverd::limits::worker::
                     kEngineExecuteImplCodeEditCallTypeKindCallsIdDeclarationStringFieldMax)},
            {"variadic_types", actuals}};
        if (calls.size() >
            ::neverd::limits::worker::
                kEngineExecuteImplCodeEditCallTypeKindCallsSizeLimit)
          throw Error(
              "edit_budget",
              "At most 32 call-site declarations are supported per function");
        const auto preview = namedView(
            address, stringField(p, "representation"),
            p.value("source_options", Json::object()), nullptr, &calls);
        bool applied = false;
        std::string reason =
            "The call-site declaration could not be bound to complete operands";
        if (preview)
          for (const auto &status :
               preview->value.value("call_type_results", Json::array()))
            if (status.value("id", std::string()) == id) {
              applied = status.value("applied", false);
              reason = status.value("refusal", reason);
            }
        if (!applied)
          throw Error("call_type_refused", reason);
      }
      request["call_types"] = std::move(calls);
      if (request.value("representation", std::string()) == "source")
        request["representation"] = "c";
    }
    if (associations) {
      const auto representation = stringField(p, "representation");
      if (representation != "c" && representation != "source" &&
          representation != "llvmc")
        throw Error("unsupported",
                    "Variable associations require Pseudocode or LLVM C");
      if (stringField(p, "snapshot", {},
                      ::neverd::limits::worker::
                          kEngineExecuteImplCodeEditSnapshotStringFieldMax) !=
          document->value.value("variable_snapshot", std::string()))
        throw Error("stale_target",
                    "The source analysis changed; select the variable again");
      const auto route = representation == "source" ? "c" : representation;
      Json edits = before.is_object()
                       ? before.value("views", Json::object())
                             .value(route, Json::object())
                             .value("variable_edits", Json::array())
                       : Json::array();
      const auto action =
          stringField(p, "action", {},
                      ::neverd::limits::worker::
                          kEngineExecuteImplCodeEditActionStringFieldMax);
      if (action == "clear")
        edits = Json::array();
      else if (action == "unmap" || action == "unsplit") {
        const auto index = sizeField(
            p, "association", edits.size(),
            ::neverd::limits::worker::
                kEngineExecuteImplCodeEditClearUnsplitUnmapIndexAssociationSizeFieldMax);
        if (index >= edits.size() ||
            edits[index].at("kind") != (action == "unmap" ? "map" : "split"))
          throw Error("stale_target",
                      "This variable association no longer exists");
        edits.erase(index);
      } else if (action == "map" || action == "split") {
        if (edits.size() >=
            ::neverd::limits::worker::
                kEngineExecuteImplCodeEditClearUnsplitUnmapSplitMapEditsSizeLimit)
          throw Error("edit_budget",
                      "Reset an association before adding more than 64 edits");
        const auto from = stringField(
            p, "variable_id", {},
            ::neverd::limits::worker::
                kEngineExecuteImplCodeEditClearUnsplitUnmapSplitMapFromVariableIdStringFieldMax);
        const auto to =
            action == "map"
                ? stringField(
                      p, "to", {},
                      ::neverd::limits::worker::
                          kEngineExecuteImplCodeEditClearUnsplitUnmapSplitMapToToStringFieldMax)
                : std::string();
        auto available = [&](const std::string &id) {
          for (const auto &v :
               document->value.value("source_variables", Json::array()))
            if (v.value("id", std::string()) == id &&
                !v.value("ambiguous", true) && !v.contains("address"))
              return true;
          return false;
        };
        if (!available(from) || (action == "map" && !available(to)))
          throw Error(
              "stale_target",
              "Select an unambiguous local variable in the current source");
        edits.push_back({{"kind", action},
                         {"from", from},
                         {"to", to},
                         {"snapshot", p.at("snapshot")},
                         {"input_sha256", inputHash()}});
        const auto preview =
            namedView(address, representation,
                      p.value("source_options", Json::object()), &edits);
        const auto results =
            preview
                ? preview->value.value("variable_edit_results", Json::array())
                : Json::array();
        if (results.size() != edits.size() ||
            !results.back().value("applied", false))
          throw Error("variable_edit_refused",
                      results.empty()
                          ? "The engine cannot prove this variable association"
                          : results.back().value(
                                "refusal", "Variable association was refused"));
      } else
        throw Error("invalid_request", "Unknown variable association action");
      request["representation"] = route;
      request["variable_edits"] = std::move(edits);
    }
    if (p.value("kind", std::string()) == "type") {
      const auto checked = execute("source_type", p);
      request["bytes"] = checked.at("bytes");
      request["input_sha256"] = checked.at("input_sha256");
      request["floating"] = checked.at("floating");
      if (checked.contains("object_address")) {
        owner = parseAddress(checked.at("object_address").get<std::string>());
        before = codeEditRow(owner);
        request["address"] = hexAddress(owner);
        request["kind"] = "global_type";
      }
      if (request.value("representation", std::string()) == "source")
        request["representation"] = "c";
    }
    if (document)
      if (const auto shape = document->value.find("code_shape");
          shape != document->value.end() && shape->is_string())
        request["shape"] = *shape;
    const auto after = CodeEdits::change(before, request);
    if (before == after)
      return {{"saved", false}, {"address", hexAddress(address)}};
    const auto checkpoint = store;
    auto state = store.committedState();
    auto &rows = state["code_edits"];
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](const Json &row) {
                                return parseAddress(
                                           row.at("addr").get<std::string>()) ==
                                       owner;
                              }),
               rows.end());
    rows.push_back(after);
    try {
      store.stage({{"kind", "code"},
                   {"address", hexAddress(owner)},
                   {"before", before},
                   {"after", after}});
      store.persist(state);
    } catch (...) {
      store = checkpoint;
      throw;
    }
    codeEdits_ = std::move(rows);
    const bool localName = p.value("kind", std::string()) == "name";
    if (localName)
      localNamesChanged();
    else {
      invalidate();
      ++revision_;
    }
    return {{"saved", true},
            {"address", hexAddress(address)},
            {"local_names_only", localName}};
  }
  if (operation == "save") {
    requireWriter();
    auto &store = history();
    auto state = store.committedState();
    state["annotations"] = backendJson(neverd_annotations_json(session_));
    saveAnalysisSections();
    sectionsDirty_ = false;
    store.persist(std::move(state));
    dirty_ = false;
    return {{"saved", true}, {"dirty", false}};
  }
  if (operation == "reload") {
    const auto sections =
        engineSymbol<AnalysisSections>("neverd_analysis_sections_json");
    if (sections &&
        (!backendJson(sections(session_, 0), true).at("sections").empty() ||
         fs::exists(analysisSectionReadPath(
             utf8Path(ownedString(neverd_session_file_path(session_))))))) {
      // Reload all saved image additions transactionally too. Reusing the
      // current image would retain unsaved section edits after "Discard".
      // Open the input as it is open now: with its options, and the debug
      // files and signatures chosen for it, which load over the saved
      // sections again.
      auto options = openOptions_;
      options["path"] = ownedString(neverd_session_file_path(session_));
      options["read_only"] = readOnly_;
      const std::pair<const char *, const Json *> inputs[] = {
          {"pdb", &pdbInput_},
          {"dwarf", &dwarfInput_},
          {"map", &mapInput_},
          {"symbols", &symbolsInput_}};
      for (const auto &[input, chosen] : inputs)
        if (!chosen->is_null())
          options[input] = *chosen;
      const auto signatures = signatureInputs_;
      const auto identity = projectId_;
      auto result = execute("open", options);
      projectId_ = identity;
      if (signatures.empty())
        return result;
      auto warnings = result.value("warnings", Json::array());
      for (const auto &input : signatures) {
        const auto path = input.at("path").get<std::string>();
        // A file that changed since it loaded is the user's to load again.
        if (signatureStamp(utf8Path(path)) != input.at("stamp")) {
          warnings.push_back("Signatures changed and were not loaded again: " +
                             path);
          continue;
        }
        try {
          execute("signatures_load", input);
        } catch (const std::exception &failure) {
          warnings.push_back("Signatures were not loaded again: " + path +
                             ": " + failure.what());
        }
      }
      result = metadata();
      result["warnings"] = std::move(warnings);
      return result;
    }
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
    sectionsDirty_ = false;
    return metadata();
  }
  if (operation == "call_paths" || operation == "call_tree") {
    const auto query =
        operation == "call_paths" ? callPathsFunction() : callTreeFunction();
    if (!query)
      throw Error("unsupported", "This engine does not publish call paths");
    // A name the listing shows, such as a synthesized main, is an address
    // to the engine; the engine resolves its own names and imports' names.
    auto options = p.is_object() ? p : Json::object();
    const auto resolveNode = [&](Json &node) {
      if (!node.is_string())
        return;
      const auto text = node.get<std::string>();
      if (text.starts_with("0x") || text.starts_with("0X"))
        return;
      if (const auto address = listing().resolveName(text))
        node = hexAddress(*address);
    };
    for (const char *key : {"waypoints", "exclude"})
      if (auto it = options.find(key); it != options.end() && it->is_array())
        for (auto &node : *it)
          resolveNode(node);
    if (auto it = options.find("root"); it != options.end())
      resolveNode(*it);
    const char *raw = query(session_, options.dump().c_str());
    if (!raw)
      throw Error("invalid_request", error());
    auto result = backendJson(raw, false);
    lowercaseAddresses(result);
    // Functions under the names the workbench shows them by.
    const auto &aliases = listing().functionAliases();
    if (auto it = result.find("nodes"); it != result.end() && it->is_array())
      for (auto &node : *it)
        if (node.value("kind", "") == "function")
          if (const auto alias = aliases.find(node.value("name", ""));
              alias != aliases.end())
            node["name"] = alias->second;
    return result;
  }
  if (operation == "source_patch" || operation == "source_patch_context" ||
      operation == "patch_source_target" ||
      operation == "patch_source_preview" || operation == "patch_context" ||
      operation == "patch_preview" || operation == "patch_apply" ||
      operation == "patch_revert" || operation == "patches" ||
      operation == "patch_export")
    return patchOperation(operation, p, encoded);
  if (operation != "disasm" && operation != "bytes" &&
      operation != "annotation_set" && operation != "rename" &&
      operation != "function_create" && operation != "function_delete" &&
      operation != "function_edit" && operation != "function_attributes" &&
      operation != "function_type" && operation != "item_define" &&
      operation != "operand_format" && operation != "decompile" &&
      operation != "cfg" && operation != "cfg_summary" &&
      operation != "cfg_viewport" && operation != "xrefs")
    throw Error("unsupported", "Unknown worker operation: " + operation);
  const auto address = parseAddress(stringField(p, "address"));
  if (operation == "disasm") {
    const auto limit = sizeField(
        p, "limit",
        ::neverd::limits::worker::kEngineExecuteImplDisasmLimitSizeFieldDefault,
        ::neverd::limits::worker::kEngineExecuteImplDisasmLimitSizeFieldMax);
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
    auto size = sizeField(
        p, "size",
        ::neverd::limits::worker::
            kEngineExecuteImplBytesSizeSizeSizeFieldDefault,
        ::neverd::limits::worker::kEngineExecuteImplBytesSizeSizeSizeFieldMax);
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
      const auto encoding = stringField(
          p, "text_encoding", {},
          ::neverd::limits::worker::
              kEngineExecuteImplBytesEncodingTextEncodingStringFieldMax);
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
    auto text =
        stringField(p, "text", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplAnnotationSetTextTextStringFieldMax);
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
    const auto name =
        stringField(p, "name", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplRenameNameNameStringFieldMax);
    if (name.empty() || std::any_of(name.begin(), name.end(), [](char c) {
          return static_cast<unsigned char>(c) <= ' ' || c == 0x7f;
        }))
      throw Error(
          "invalid_request",
          "A name is not empty and has no spaces or control characters");
    // The engine's naming rules, which the command line's rename keeps too;
    // an engine without them leaves the automatic forms to this check.
    using NameCheck = int (*)(neverd_session_t, neverd_va_t, const char *);
    static const auto nameCheck = engineSymbol<NameCheck>("neverd_name_check");
    if (nameCheck ? nameCheck(session_, address, name.c_str()) != 0
                  : parseDummyName(name).has_value())
      throw Error("invalid_request",
                  nameCheck ? error()
                            : name + " is an automatic name; choose another");
    // A name leads to one address, the names only the listing gives
    // included.
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
  if (operation == "function_attributes") {
    const auto &attributes = functionAttributes();
    if (!attributes)
      throw Error("unsupported", "This engine keeps no function attributes");
    const char *row = attributes.attributes(session_, address);
    if (!row)
      throw Error("invalid_request", error());
    return backendJson(row);
  }
  if (operation == "function_type") {
    if (!typesFunctions())
      throw Error("unsupported", "This engine keeps no function declarations");
    // The declaration the user edits, one checked before it is stated, or
    // the one a parameter's new type makes.
    Json options = Json::object();
    if (p.contains("declaration"))
      options["declaration"] = stringField(
          p, "declaration", {},
          ::neverd::limits::worker::
              kEngineExecuteImplFunctionTypeOptionsDeclarationStringFieldMax);
    if (p.contains("parameter")) {
      options["parameter"] = sizeField(p, "parameter", 0, 255);
      options["type"] = stringField(
          p, "type", {},
          ::neverd::limits::worker::
              kEngineExecuteImplFunctionTypeOptionsTypeTypeStringFieldMax);
    }
    using Declaration =
        const char *(*)(neverd_session_t, neverd_va_t, const char *);
    const auto declaration = engineSymbol<Declaration>("neverd_func_type_json");
    const auto reply = ownedString(declaration(
        session_, address, options.empty() ? nullptr : options.dump().c_str()));
    if (reply.empty())
      throw Error("invalid_request", error());
    try {
      return Json::parse(reply);
    } catch (const Json::exception &) {
      throw Error("engine_error", "Engine returned malformed JSON");
    }
  }
  if (operation == "function_create" || operation == "function_delete" ||
      operation == "function_edit") {
    requireWriter();
    const auto &edits = functionEdits();
    const auto &attributes = functionAttributes();
    const bool create = operation == "function_create";
    const bool edit = operation == "function_edit";
    if (!edits || (edit && !attributes))
      throw Error("unsupported", "This engine keeps no function edits");
    if (dirty_)
      throw Error("unsaved_changes", "Save or reload staged annotations "
                                     "before editing a function");
    // What the user states of the function: each key of "attributes" sets
    // one of them, and null hands it back to analysis.
    std::string stated;
    if (edit) {
      if (!p.contains("attributes") || !p.at("attributes").is_object() ||
          p.at("attributes").empty() ||
          p.at("attributes").size() >
              ::neverd::limits::worker::kMaxItemAttributes)
        throw Error("invalid_request",
                    "A function edit states one attribute or more");
      stated = p.at("attributes").dump();
    }
    auto &store = history();
    // The engine checks the edit; the rows it then holds are the new table.
    if ((edit     ? attributes.edit(session_, address, stated.c_str())
         : create ? edits.create(session_, address)
                  : edits.remove(session_, address)) != 0)
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
    Json result{{"address", hexAddress(address)}, {"saved", true}};
    if (edit)
      result["row"] = after;
    else
      result["created"] = create;
    return result;
  }
  if (operation == "item_define") {
    requireWriter();
    const auto &items = dataItems();
    if (!items)
      throw Error("unsupported", "This engine keeps no data items");
    if (dirty_)
      throw Error("unsaved_changes", "Save or reload staged annotations "
                                     "before defining data");
    const auto action =
        stringField(p, "action", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplItemDefineActionStringFieldMax);
    const Json at = listing().item(address);
    if (at.is_null())
      throw Error("invalid_request", "No image byte at " + hexAddress(address));
    const auto kind = at.at("kind").get<std::string>();
    if (kind == "instruction" && action == "code")
      return {{"address", at.at("start")},
              {"kind", CodeItemKind},
              {"size", at.at("size")},
              {"saved", false}};
    if (kind == "instruction" && action != "undefine" &&
        at.value("user", std::string()) != CodeItemKind)
      throw Error("invalid_request",
                  "Code belongs to its function; delete the function to "
                  "define data in its bytes");
    // The item the cursor is in is the one the action changes.
    const auto start =
        action == "code"
            ? address
            : parseAddress(at.value("definition_start", at.at("start"))
                               .get<std::string>());
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
      auto cursor = start;
      for (;;) {
        if (!code.empty()) {
          const auto next = listing().item(cursor);
          if (next.is_null() || next.at("kind") == "instruction")
            break;
        }
        if (code.size() == ::neverd::limits::worker::kEngineMaxInstructions)
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
    } else if (action == "array") {
      const auto width = sizeField(
          p, "element_size",
          ::neverd::limits::worker::
              kEngineExecuteImplItemDefineCodeArrayWidthElementSizeSizeFieldDefault,
          ::neverd::limits::worker::
              kEngineExecuteImplItemDefineCodeArrayWidthElementSizeSizeFieldMax);
      const auto count =
          sizeField(p, "count", 0,
                    ::neverd::limits::worker::
                        kEngineExecuteImplItemDefineCodeArrayCountSizeFieldMax);
      if (!width || !count ||
          width * count > ::neverd::sdk::kMaxDataItemArrayBytes)
        throw Error("invalid_request", "Array extent exceeds 64 MiB");
      size = width * count;
      row = {
          {"kind", "array"},
          {"element_size", width},
          {"count", count},
          {"size", size},
          {"type",
           stringField(
               p, "type", {},
               ::neverd::limits::worker::
                   kEngineExecuteImplItemDefineCodeArrayRowTypeStringFieldMax)}};
    } else if (action == "alignment") {
      const auto alignment = sizeField(
          p, "alignment",
          ::neverd::limits::worker::
              kEngineExecuteImplItemDefineCodeArrayAlignmentSizeFieldDefault,
          ::neverd::limits::worker::
              kEngineExecuteImplItemDefineCodeArrayAlignmentSizeFieldMax);
      if (alignment < 2 || (alignment & (alignment - 1)))
        throw Error("invalid_request", "Alignment must be a power of two");
      size = alignment - start % alignment;
      row = {{"kind", "alignment"}, {"alignment", alignment}, {"size", size}};
    } else if (action == "data") {
      // The data carousel: byte, word, dword, qword, then byte again.
      static constexpr std::array<std::uint64_t, 4> Carousel = {1, 2, 4, 8};
      size = sizeField(
          p, "size", 0,
          ::neverd::limits::worker::
              kEngineExecuteImplItemDefineCodeArrayAlignmentDataSizeSizeSizeFieldMax);
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
          kind == "uninitialized" || (kind == "align" && user != "alignment"))
        return {{"address", hexAddress(start)},
                {"kind", UndefinedItemKind},
                {"size", at.at("size")},
                {"saved", false}};
      size = at.value("definition_size", at.at("size")).get<std::uint64_t>();
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
      throw Error("unsupported", "Operand types are shown for x86, ARM, "
                                 "AArch64, SBF and EVM instructions only");
    // Formats belong to the instruction that starts at the address, and
    // change its operands that are numbers: the one asked for if it is one,
    // else the last.  A structure offset also changes a displacement from a
    // register, which it takes first.
    const auto action =
        stringField(p, "action", {},
                    ::neverd::limits::worker::
                        kEngineExecuteImplOperandFormatActionStringFieldMax);
    const bool structure = action == numberBaseName(NumberBase::Struct);
    if (structure && !fitsLocalTypes())
      throw Error("unsupported",
                  "This engine shows no structure offsets of local types");
    const auto numbers = listing().numberOperands(address);
    const auto structs = listing().structOperands(address);
    if (!numbers || !structs)
      throw Error("invalid_request",
                  "No instruction starts at " + hexAddress(address));
    // The listing's own number again also clears a structure offset.
    const bool reset = action == numberBaseName(NumberBase::Number);
    const auto &candidates = structure || reset ? *structs : *numbers;
    const std::optional<std::size_t> asked =
        p.contains("operand")
            ? std::optional<std::size_t>(sizeField(
                  p, "operand", 0,
                  ::neverd::limits::worker::
                      kEngineExecuteImplOperandFormatAskedOperandSizeFieldMax))
            : std::nullopt;
    std::optional<std::size_t> chosen;
    if (structure)
      chosen = listing().structOperand(address, asked);
    if (!structure && asked && *asked < candidates.size() && candidates[*asked])
      chosen = asked;
    for (std::size_t i = candidates.size(); !structure && !chosen && i-- > 0;)
      if (candidates[i])
        chosen = i;
    if (!chosen)
      throw Error("invalid_request",
                  "The instruction at " + hexAddress(address) +
                      (structure ? " has no number or displacement from a "
                                   "register to show as a structure offset"
                                 : " has no number to format"));
    const auto operand = *chosen;
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
    if (action == "enum") {
      next = {
          {"base", "enum"},
          {"negate", false},
          {"invert", false},
          {"type",
           stringField(
               p, "type", {},
               ::neverd::limits::worker::
                   kEngineExecuteImplOperandFormatEnumNextTypeStringFieldMax)},
          {"flags", p.value("flags", false)}};
    } else if (structure) {
      // The member a structure offset names has no sign of its own.
      next = {
          {"base", action},
          {"negate", false},
          {"invert", false},
          {"type",
           stringField(
               p, "type", {},
               ::neverd::limits::worker::
                   kEngineExecuteImplOperandFormatEnumNextTypeStringFieldMax)},
          {"delta", p.value("delta", Json(0))}};
      if (!next.at("delta").is_number_integer())
        throw Error("invalid_request", "delta must be an integer");
    } else if ((action == "negate" || action == "invert") &&
               (next.at("base") ==
                    std::string(numberBaseName(NumberBase::Struct)) ||
                next.at("base") == "enum")) {
      throw Error(
          "invalid_request",
          "A structure offset has no sign or bits of its own to change");
    } else if (action == "negate")
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
    // A structure offset may go to other instructions too, as every access
    // through the same pointer does: the displacement each one adds to a
    // register, all of them one step of the history.
    std::vector<std::pair<std::uint64_t, std::size_t>> others;
    if (p.contains("also")) {
      const auto &also = p.at("also");
      if (!structure)
        throw Error("invalid_request",
                    "Only a structure offset goes to other instructions");
      if (!also.is_array() ||
          also.size() > ::neverd::limits::worker::kEngineMaxOperandFormatRows)
        throw Error(
            "invalid_request",
            "also lists at most " +
                std::to_string(
                    ::neverd::limits::worker::kEngineMaxOperandFormatRows) +
                " instructions");
      for (const auto &value : also) {
        if (!value.is_string())
          throw Error("invalid_request", "also lists instruction addresses");
        const auto at = parseAddress(value.get<std::string>());
        if (at == address ||
            std::any_of(others.begin(), others.end(),
                        [&](const auto &other) { return other.first == at; }))
          continue;
        if (const auto chosen = listing().structOperand(at, std::nullopt))
          others.emplace_back(at, *chosen);
      }
    }
    try {
      if (formats.set(session_, address, static_cast<int>(operand),
                      next.dump().c_str()) != 0)
        throw Error("invalid_request", error());
      for (const auto &[at, index] : others)
        if (formats.set(session_, at, static_cast<int>(index),
                        next.dump().c_str()) != 0)
          throw Error("invalid_request", error());
    } catch (...) {
      // The sidecar still holds the formats before these.
      (void)formats.load(session_);
      throw;
    }
    // One history step holds every row the change made, the instruction's
    // first.
    std::map<std::uint64_t, std::pair<Json, Json>> changed;
    for (const auto &row : state.at("operands"))
      changed[parseAddress(row.at("addr").get<std::string>())].first = row;
    state["operands"] = operandFormatRows();
    for (const auto &row : state.at("operands"))
      changed[parseAddress(row.at("addr").get<std::string>())].second = row;
    const Json after = changed[address].second;
    Json command{{"kind", "operand"},
                 {"address", hexAddress(address)},
                 {"before", before},
                 {"after", after}};
    for (const auto &[at, sides] : changed)
      if (at != address && sides.first != sides.second)
        command["rows"].push_back({{"address", hexAddress(at)},
                                   {"before", sides.first},
                                   {"after", sides.second}});
    if (before == after && !command.contains("rows"))
      return {{"address", hexAddress(address)},
              {"operand", operand},
              {"format", next},
              {"also", 0},
              {"saved", false}};
    const auto checkpoint = store;
    try {
      store.stage(std::move(command));
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
            {"also", others.size()},
            {"saved", true}};
  }
  if (operation == "decompile") {
    const auto representation = stringField(
        p, "representation", "c",
        ::neverd::limits::worker::
            kEngineExecuteImplDecompileRepresentationStringFieldMax);
    // `source` reads in the function's own language; the named dialects spell
    // the HighC source in one, through the engine's view alone.
    const bool spelled = representation == "source" ||
                         representation == "cpp" || representation == "rust" ||
                         representation == "go";
    if (representation != "c" && representation != "llvmc" &&
        representation != "low" && representation != "med" &&
        representation != "high" && representation != "llvm" && !spelled)
      throw Error("unsupported", "Unknown code representation");
    const auto options = p.value("source_options", Json::object());
    if (!options.is_object() ||
        options.dump().size() >
            ::neverd::limits::worker::
                kEngineExecuteImplDecompileOptionsDumpSizeLimit ||
        (!options.empty() && representation != "c" &&
         representation != "llvmc" && !spelled))
      throw Error("invalid_request",
                  "Invalid source_options for this representation");
    const auto offset =
        sizeField(p, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = sizeField(
        p, "limit",
        ::neverd::limits::worker::
            kEngineExecuteImplDecompileLimitSizeFieldDefault,
        ::neverd::limits::worker::kEngineExecuteImplDecompileLimitSizeFieldMax);
    if (!limit)
      throw Error("invalid_request", "limit must be at least 1");
    const auto entry = functionEntry(address);
    std::string mappingStatus = "unsupported_representation";
    // Why the engine offers no page, such as Rust asked of a C program.
    std::string reason;
    // An engine that pages no LLVM IR answers without text; its whole text
    // follows below.
    if (representation == "low" || representation == "med" ||
        representation == "high" || representation == "llvm" ||
        representation == "c" || representation == "llvmc" || spelled) {
      if (auto named = namedViewPage(entry, representation, offset, limit,
                                     options, encoded))
        return std::move(*named);
      if (!options.empty())
        throw Error("unavailable",
                    error().empty() ? "Source transformation failed" : error());
      prepareFunction(entry, representation);
      if (const auto view = irViewFunction()) {
        auto result = backendJson(
            view(session_, entry, representation.c_str(), offset, limit), true);
        mappingStatus =
            result.value("mapping_status", std::string("unavailable"));
        if (result.contains("reason") && result["reason"].is_string())
          reason = result["reason"].get<std::string>();
        if (result.contains("text")) {
          if (!result["text"].is_string() ||
              result["text"].get_ref<const std::string &>().size() >
                  ::neverd::limits::worker::kEngineMaxCodePageBytes ||
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
    prepareFunction(entry, representation);
    const auto key = hexAddress(entry) + ":" + representation;
    if (textKey_ != key) {
      textKey_.clear();
      const char *result =
          representation == "c"       ? neverd_decompile(session_, entry)
          : representation == "llvmc" ? neverd_decompile_llvm(session_, entry)
          : representation == "low"   ? neverd_ir_low(session_, entry)
          : representation == "med"   ? neverd_ir_med(session_, entry)
          : representation == "high"  ? neverd_ir_high(session_, entry)
                                      : neverd_ir_llvm(session_, entry);
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
    const auto byteAt = [this](std::size_t line) {
      return line < textLines_.size() ? textLines_[line] : textCache_.size();
    };
    const auto start = std::min(offset, textLines_.size());
    auto end = start + std::min(limit, textLines_.size() - start);
    const auto startByte = byteAt(start);
    // The page ends before the line that would pass its budget; the next
    // page starts there.
    std::size_t fits = start;
    while (fits < end && byteAt(fits + 1) - startByte <=
                             ::neverd::limits::worker::kEngineMaxCodePageBytes)
      ++fits;
    if (fits == start && end > start)
      throw Error("budget_exceeded",
                  "A code line exceeds the 2 MiB page budget");
    end = fits;
    const auto endByte = byteAt(end);
    return {
        {"address", hexAddress(entry)},
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
    // Client text metrics size each node to its formatted listing rows.
    const auto metrics = GraphMetrics::fromRequest(p);
    const auto metricsKey = metrics.key();
    const auto entry = functionEntry(address);
    const auto key = hexAddress(entry);
    for (auto it = graphs_.begin(); it != graphs_.end(); ++it)
      if (it->address == key && it->metrics == metricsKey) {
        graphs_.splice(graphs_.begin(), graphs_, it);
        return graphs_.front().snapshot->summary();
      }
    const auto layout =
        projectId_ + ":" + revision() + ":" + key + ":" + metricsKey;
    auto snapshot = std::make_unique<GraphSnapshot>(
        functionGraph(entry), key, layout, metrics,
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
    const auto key = hexAddress(functionEntry(address));
    const auto layout = stringField(
        p, "layout_revision", {},
        ::neverd::limits::worker::
            kEngineExecuteImplCfgViewportLayoutRevisionStringFieldMax);
    for (const auto &graph : graphs_)
      if (graph.address == key && (layout.empty() || graph.revision == layout))
        return graph.snapshot->viewport(p);
    throw Error("stale_layout",
                "No matching graph snapshot; request cfg_summary first");
  }
  if (operation == "cfg") {
    auto result = functionGraph(functionEntry(address));
    if (result.value("nodes", Json::array()).size() >
            ::neverd::limits::worker::
                kEngineExecuteImplCfgResultValueNodesJsonArraySizeLimit ||
        result.value("edges", Json::array()).size() >
            ::neverd::limits::worker::
                kEngineExecuteImplCfgResultValueEdgesJsonArraySizeLimit)
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
    const auto direction =
        stringField(p, "direction", "to",
                    ::neverd::limits::worker::
                        kEngineExecuteImplXrefsDirectionStringFieldMax);
    if (direction != "to" && direction != "from")
      throw Error("invalid_request", "direction must be to or from");
    if (stringField(p, "source", "direct",
                    ::neverd::limits::worker::
                        kEngineExecuteImplXrefsSourceStringFieldMax) != "ir")
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
