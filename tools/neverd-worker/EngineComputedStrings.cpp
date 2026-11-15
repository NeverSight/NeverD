//===- EngineComputedStrings.cpp - Computed string queries ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Page and filter computed string observations through the engine C ABI.
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "EngineSymbols.h"
#include "Listing.h"
#include "TablePage.h"
#include "TextFold.h"

#include "neverd/sdk/NeverDCAPIDisasm.h"
#include "neverd/sdk/NeverDCAPIQuery.h"

#include <algorithm>
#include <chrono>

namespace neverd::worker {
namespace {
using ObserveFunction = const char *(*)(neverd_session_t, neverd_va_t);
ObserveFunction observeFunction() {
  static const auto observe =
      engineSymbol<ObserveFunction>("neverd_computed_strings_json");
  return observe;
}
} // namespace

bool Engine::reportsComputedStrings() { return observeFunction() != nullptr; }

Json Engine::computedStringsStatus() const {
  const auto &catalogue = *computedStrings_;
  return {{"generation", catalogue.generation},
          {"done", catalogue.next},
          {"last_function",
           catalogue.next
               ? Json(hexAddress(catalogue.pending[catalogue.next - 1]))
               : Json(nullptr)},
          {"total", catalogue.pending.size()},
          {"rows", catalogue.rows.size()},
          {"failures", catalogue.failures},
          {"complete", catalogue.complete},
          {"discovery_error", catalogue.discoveryError}};
}

Json Engine::computedStringsStep() {
  const auto observe = observeFunction();
  if (!observe)
    throw Error("unsupported", "This engine does not report computed strings");
  const auto arch = backendText(neverd_session_arch_name(session_), false);
  if (arch != "x86_64" && arch != "x64" && arch != "aarch64")
    throw Error("unsupported", "Computed strings require x64 or AArch64");
  if (!computedStrings_) {
    computedStrings_.emplace();
    // Identifies a disposable catalogue, including across replica restarts.
    computedStrings_->generation = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }
  auto &catalogue = *computedStrings_;
  if (catalogue.complete)
    return computedStringsStatus();
  if (!catalogue.discovered) {
    using DiscoverFunction = int (*)(neverd_session_t);
    static const auto discover =
        engineSymbol<DiscoverFunction>("neverd_session_discover_functions");
    if (discover && discover(session_) < 0)
      catalogue.discoveryError = error();
    catalogue.discovered = true;
  }
  // Inventory is read once per completed pass, not once per function. A
  // preparation can discover additional handlers/entries; attempt those too
  // until the catalogue reaches a fixed point.
  const auto appendInventory = [&] {
    const int count = neverd_func_count(session_);
    for (int index = 0; index < count; ++index) {
      const auto entry = neverd_func_entry(session_, index);
      if (catalogue.scheduled.insert(entry).second)
        catalogue.pending.push_back(entry);
    }
  };
  if (catalogue.next == catalogue.pending.size())
    appendInventory();
  if (catalogue.next != catalogue.pending.size()) {
    const auto entry = catalogue.pending[catalogue.next];
    const auto name = listing().nameAt(entry);
    const auto diagnostic = [&](const std::string &status,
                                const std::string &message) {
      ++catalogue.failures;
      catalogue.rows.push_back({{"address", hexAddress(entry)},
                                {"function_address", hexAddress(entry)},
                                {"function", name},
                                {"status", status},
                                {"text", message}});
    };
    try {
      prepareFunction(entry, "med");
      auto result = backendJson(observe(session_, entry), true);
      for (auto &row : result.at("strings")) {
        row["string_address"] = row.at("address");
        row["address"] = row.at("instruction");
        row["function_address"] = hexAddress(entry);
        row["function"] = name;
        row["status"] = "conditional";
        catalogue.rows.push_back(std::move(row));
      }
      if (!result.at("complete").get<bool>())
        diagnostic("incomplete",
                   "The supported observation analysis did not complete");
    } catch (const std::exception &failure) {
      // One refused function does not hide the rest of the image. The row
      // keeps its actual diagnostic; it is never an empty successful result.
      diagnostic("failed", failure.what());
    }
    ++catalogue.next;
    catalogue.ordered = false;
  }
  if (catalogue.next == catalogue.pending.size()) {
    appendInventory();
    catalogue.complete = catalogue.next == catalogue.pending.size();
  }
  return computedStringsStatus();
}

Json Engine::computedStringsPage(const Json &payload) {
  if (!computedStrings_ ||
      stringField(payload, "generation") != computedStrings_->generation)
    throw Error("catalogue_changed",
                "Computed string analysis was reset; scan again");
  auto &catalogue = *computedStrings_;
  const auto limit =
      sizeField(payload, "limit",
                ::neverd::limits::worker::kEnginePageLimitSizeFieldDefault,
                ::neverd::limits::worker::kEnginePageLimitSizeFieldMax);
  if (!limit)
    throw Error("invalid_request", "limit must be at least 1");
  const auto filter = foldText(stringField(payload, "filter"));
  const auto sort = stringField(payload, "sort", "address");
  if (sort != "address" && sort != "string_address" && sort != "function" &&
      sort != "text" && sort != "status")
    throw Error("invalid_request", "Unknown computed string sort field");
  const bool descending = payload.value("descending", false);
  const std::string key = filter + '\0' + sort + (descending ? "d" : "a");
  if (!catalogue.ordered || key != catalogue.orderKey) {
    catalogue.order.clear();
    std::vector<std::string> keys(catalogue.rows.size());
    const bool addressOrder = sort == "address" || sort == "string_address";
    std::vector<std::uint64_t> addresses(addressOrder ? catalogue.rows.size()
                                                      : 0);
    for (std::size_t i = 0; i < catalogue.rows.size(); ++i) {
      auto &row = catalogue.rows[i];
      // Display names are mutable independently of the evidence's revision.
      row["function"] =
          listing().nameAt(parseAddress(stringField(row, "function_address")));
      bool match = filter.empty();
      for (const char *field :
           {"text", "function", "address", "string_address", "status"})
        if (!match && foldText(row.value(field, std::string())).find(filter) !=
                          std::string::npos)
          match = true;
      if (!match)
        continue;
      catalogue.order.push_back(i);
      if (addressOrder)
        addresses[i] = row.contains(sort)
                           ? parseAddress(stringField(row, sort.c_str()))
                           : 0;
      else
        keys[i] = foldText(row.value(sort, std::string()));
    }
    std::stable_sort(catalogue.order.begin(), catalogue.order.end(),
                     [&](std::size_t a, std::size_t b) {
                       return addressOrder ? addresses[a] < addresses[b]
                                           : keys[a] < keys[b];
                     });
    if (descending)
      std::reverse(catalogue.order.begin(), catalogue.order.end());
    catalogue.orderKey = key;
    catalogue.ordered = true;
  }
  const auto position = tablePagePosition(
      payload, catalogue.order.size(), limit, [&](std::size_t index) {
        return std::optional<TableRowRange>{
            {parseAddress(stringField(catalogue.rows[catalogue.order[index]],
                                      "address")),
             1}};
      });
  Json items = Json::array();
  const auto total = catalogue.order.size();
  for (std::size_t i = std::min(position.offset, total);
       i < total && items.size() < limit; ++i)
    items.push_back(catalogue.rows[catalogue.order[i]]);
  const bool complete =
      position.offset >= total || items.size() >= total - position.offset;
  Json result{{"items", std::move(items)},
              {"total", total},
              {"offset", position.offset},
              {"complete", complete},
              {"scan", computedStringsStatus()}};
  position.annotate(result);
  return result;
}
} // namespace neverd::worker
