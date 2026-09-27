//===- MobileAndroidReferences.cpp - Android instruction references -------===//
#include "MobileAndroidQuery.h"

#include <iterator>
#include <limits>

namespace neverd::mobile {
dalvik::DexReferenceKind androidReferenceKind(std::string_view name) {
  if (name == "string")
    return dalvik::DexReferenceKind::String;
  if (name == "type")
    return dalvik::DexReferenceKind::Type;
  if (name == "method")
    return dalvik::DexReferenceKind::Method;
  if (name == "field")
    return dalvik::DexReferenceKind::Field;
  throw Error("--find-refs must be string, type, method, or field");
}

std::string_view androidReferenceKindName(dalvik::DexReferenceKind kind) {
  switch (kind) {
  case dalvik::DexReferenceKind::String:
    return "string";
  case dalvik::DexReferenceKind::Type:
    return "type";
  case dalvik::DexReferenceKind::Method:
    return "method";
  case dalvik::DexReferenceKind::Field:
    return "field";
  }
  throw Error("invalid Android reference kind");
}

namespace {
void requireUTF8(std::string_view value, std::string_view what) {
  if (!llvm::json::isUTF8(llvm::StringRef(value.data(), value.size())))
    throw Error(std::string(what) + " is not representable as UTF-8");
}

void addCount(uint64_t &total, uint64_t count, uint64_t limit,
              std::string_view what) {
  if (count > limit - total)
    throw Error(std::string(what) + " exceeds the query limit");
  total += count;
}

// Bound publication storage before copying result strings into the JSON tree.
// Six bytes covers JSON escaping; the UTF-16 allowance includes pretty-print
// whitespace. The CLI also checks the exact final serialized byte count.
void chargeText(Budget &budget, size_t size, uint64_t expansion = 6) {
  if (size > (budget.limits.max_bytes - budget.output_bytes) / expansion)
    throw Error("Android reference output exceeds the byte limit");
  budget.output(size * expansion);
}
} // namespace

llvm::json::Object findAndroidReferences(const fs::path &input,
                                         const dalvik::DexReferenceQuery &query,
                                         Budget &budget) {
  requireUTF8(query.text, "Android reference query");
  if (query.owner)
    requireUTF8(*query.owner, "Android reference owner");
  const std::string kind(androidReferenceKindName(query.kind));
  llvm::json::Array references;
  std::vector<std::string> classes;
  uint64_t dex_count = 0, class_count = 0, class_bytes = 0;
  uint64_t defined_methods = 0, scanned_methods = 0, scanned_codes = 0;
  uint64_t matching_entries = 0;
  chargeText(budget, query.text.size());
  if (query.owner)
    chargeText(budget, query.owner->size());
  visitAndroidDexes(
      input, budget, [&](std::string_view entry, std::string_view bytes) {
        requireUTF8(entry, "Android DEX entry name");
        auto result = dalvik::findDexReferences(bytes, query, budget);
        if (!result.code_scan_complete)
          throw Error(
              "Android reference scan did not cover every defined body");
        ++dex_count;
        addCount(class_count, result.class_descriptors.size(),
                 budget.limits.max_files, "Android class count");
        for (const auto &name : result.class_descriptors)
          addCount(class_bytes, name.size(), budget.limits.max_bytes,
                   "Android class identity storage");
        classes.insert(
            classes.end(),
            std::make_move_iterator(result.class_descriptors.begin()),
            std::make_move_iterator(result.class_descriptors.end()));
        addCount(defined_methods, result.defined_method_count,
                 budget.limits.max_files, "Android defined method count");
        addCount(scanned_methods, result.scanned_method_count,
                 budget.limits.max_files, "Android scanned method count");
        addCount(scanned_codes, result.scanned_code_item_count,
                 budget.limits.max_files, "Android scanned code item count");
        addCount(matching_entries, result.matching_pool_entries,
                 std::numeric_limits<uint64_t>::max(),
                 "Android matching pool count");
        if (result.references.size() >
            budget.limits.max_files - references.size())
          throw Error("Android reference count exceeds the query limit");
        for (auto &site : result.references) {
          budget.tick();
          auto method = site.method.identity();
          requireUTF8(method, "Android method identity");
          requireUTF8(site.opcode, "Android opcode");
          const bool string_kind =
              query.kind == dalvik::DexReferenceKind::String;
          if (!string_kind)
            requireUTF8(site.target, "Android reference target");
          if (string_kind != site.target_utf16.has_value())
            throw Error(
                "Android reference target has inconsistent string units");
          budget.output(256);
          chargeText(budget, entry.size());
          chargeText(budget, method.size());
          chargeText(budget, site.opcode.size());
          chargeText(budget, site.target.size());
          if (site.target_utf16)
            chargeText(budget, site.target_utf16->size(), 24);
          llvm::json::Object row{{"dex_entry", std::string(entry)},
                                 {"method", std::move(method)},
                                 {"pc_code_units", site.pc_code_units},
                                 {"opcode", std::move(site.opcode)},
                                 {"kind", kind},
                                 {"target_index", site.target_index}};
          if (llvm::json::isUTF8(site.target))
            row["target"] = std::move(site.target);
          else
            row["target"] = nullptr;
          if (site.target_utf16) {
            llvm::json::Array units;
            for (char16_t unit : *site.target_utf16)
              units.push_back(static_cast<uint16_t>(unit));
            row["target_utf16"] = std::move(units);
          }
          references.push_back(std::move(row));
        }
      });
  validateAndroidClassIdentities(classes, budget);
  budget.check();
  llvm::json::Object report{{"schema_version", 1},
                            {"status", "success"},
                            {"operation", "android-code-references"},
                            {"kind", kind},
                            {"query", query.text},
                            {"exact", query.exact},
                            {"validation_scope", "dex-code-references"},
                            {"code_scan_complete", true},
                            {"unselected_zip_payloads_validated", false},
                            {"dex_count", dex_count},
                            {"class_count", class_count},
                            {"defined_method_count", defined_methods},
                            {"scanned_method_count", scanned_methods},
                            {"scanned_code_item_count", scanned_codes},
                            {"matching_pool_entries", matching_entries},
                            {"reference_count", references.size()},
                            {"references", std::move(references)}};
  if (query.owner)
    report["owner"] = *query.owner;
  else
    report["owner"] = nullptr;
  return report;
}
} // namespace neverd::mobile
