//===- EnginePatch.cpp - Journaled interactive patch operations -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Worker adapter; the SDK owns byte admission and instruction encoding.
//===----------------------------------------------------------------------===//

#include "Engine.h"
#include "EngineSymbols.h"
#include "Listing.h"
#include "ProjectHistory.h"

#include "neverd/sdk/NeverDCAPIBytePatch.h"
#include "neverd/sdk/NeverDCAPIPatch.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace neverd::worker {
namespace {
struct PatchFunctions {
  decltype(&neverd_byte_patch_preview) preview;
  decltype(&neverd_byte_patch_apply) apply;
  decltype(&neverd_byte_patch_revert) revert;
  decltype(&neverd_byte_patches_json) rows;
  decltype(&neverd_byte_patches_load) load;
  decltype(&neverd_byte_patches_export) exportFile;
  decltype(&neverd_byte_patch_context_json) context;
  // Older SDKs still provide complete rows; retain their patch capability.
  decltype(&neverd_byte_patch_ranges_json) ranges;
  decltype(&neverd_byte_patches_snapshot_v1) snapshot;
  decltype(&neverd_byte_patch_snapshot_free_v1) freeSnapshot;
  decltype(&neverd_byte_patch_ranges_snapshot_v1) rangeSnapshot;
  decltype(&neverd_byte_patch_range_snapshot_free_v1) freeRangeSnapshot;
  explicit operator bool() const {
    return preview && apply && revert && rows && load && exportFile && context;
  }
};
const PatchFunctions &patches() {
  static const PatchFunctions F{
      engineSymbol<decltype(&neverd_byte_patch_preview)>(
          "neverd_byte_patch_preview"),
      engineSymbol<decltype(&neverd_byte_patch_apply)>(
          "neverd_byte_patch_apply"),
      engineSymbol<decltype(&neverd_byte_patch_revert)>(
          "neverd_byte_patch_revert"),
      engineSymbol<decltype(&neverd_byte_patches_json)>(
          "neverd_byte_patches_json"),
      engineSymbol<decltype(&neverd_byte_patches_load)>(
          "neverd_byte_patches_load"),
      engineSymbol<decltype(&neverd_byte_patches_export)>(
          "neverd_byte_patches_export"),
      engineSymbol<decltype(&neverd_byte_patch_context_json)>(
          "neverd_byte_patch_context_json"),
      engineSymbol<decltype(&neverd_byte_patch_ranges_json)>(
          "neverd_byte_patch_ranges_json"),
      engineSymbol<decltype(&neverd_byte_patches_snapshot_v1)>(
          "neverd_byte_patches_snapshot_v1"),
      engineSymbol<decltype(&neverd_byte_patch_snapshot_free_v1)>(
          "neverd_byte_patch_snapshot_free_v1"),
      engineSymbol<decltype(&neverd_byte_patch_ranges_snapshot_v1)>(
          "neverd_byte_patch_ranges_snapshot_v1"),
      engineSymbol<decltype(&neverd_byte_patch_range_snapshot_free_v1)>(
          "neverd_byte_patch_range_snapshot_free_v1")};
  return F;
}

std::string patchBytesHex(const uint8_t *Bytes, size_t Size) {
  constexpr char Digits[] = "0123456789abcdef";
  std::string Text(Size * 2, '\0');
  for (size_t I = 0; I < Size; ++I) {
    Text[I * 2] = Digits[Bytes[I] >> 4];
    Text[I * 2 + 1] = Digits[Bytes[I] & 15];
  }
  return Text;
}

Json patchRowJson(const neverd_byte_patch_row_v1 &Row) {
  const auto Offset = hexAddress(Row.file_offset);
  Json::object_t Item;
  Item.emplace("addr", Offset);
  Item.emplace("current", patchBytesHex(Row.current, Row.size));
  Item.emplace("file_off", Offset);
  Item.emplace("kind", Row.kind);
  Item.emplace("original", patchBytesHex(Row.original, Row.size));
  Item.emplace("size", Row.size);
  Item.emplace("va", hexAddress(Row.address));
  return Json(std::move(Item));
}

bool matchesPatchBytes(const Json &Value, const uint8_t *Bytes, size_t Size) {
  if (!Value.is_string())
    return false;
  const auto &Text = Value.get_ref<const std::string &>();
  if (Text.size() != Size * 2)
    return false;
  constexpr char Digits[] = "0123456789abcdef";
  for (size_t I = 0; I < Size; ++I)
    if (Text[I * 2] != Digits[Bytes[I] >> 4] ||
        Text[I * 2 + 1] != Digits[Bytes[I] & 15])
      return false;
  return true;
}

// Match the adapter's complete representation, not just byte semantics.
// Unknown fields, uppercase hex and alternative numeric types must not
// survive a canonical SDK snapshot conversion through handle reuse.
bool matchesPatchRow(const Json &Value, const neverd_byte_patch_row_v1 &Row) {
  const auto Offset = hexAddress(Row.file_offset);
  const auto Kind = Value.find("kind");
  // History admission owns these string types. Comparing their storage avoids
  // constructing temporary JSON string values for every unchanged row.
  return Value.size() == 7 && Kind != Value.end() &&
         Kind->get_ref<const std::string &>() == Row.kind &&
         Value.at("size").is_number_unsigned() &&
         Value.at("size").get<Json::number_unsigned_t>() == Row.size &&
         Value.at("addr").get_ref<const std::string &>() == Offset &&
         Value.at("file_off").get_ref<const std::string &>() == Offset &&
         Value.at("va").get_ref<const std::string &>() ==
             hexAddress(Row.address) &&
         matchesPatchBytes(Value.at("original"), Row.original, Row.size) &&
         matchesPatchBytes(Value.at("current"), Row.current, Row.size);
}
} // namespace
bool Engine::keepsBytePatches() { return bool(patches()); }
bool Engine::keepsSourcePatches() {
  return engineSymbol<decltype(&neverd_source_patch_context_json)>(
             "neverd_source_patch_context_json") &&
         engineSymbol<decltype(&neverd_patch_source)>("neverd_patch_source") &&
         engineSymbol<decltype(&neverd_patch_source_report)>(
             "neverd_patch_source_report");
}
bool Engine::provesSourcePatches() {
  return engineSymbol<decltype(&neverd_byte_patch_source_target_json)>(
             "neverd_byte_patch_source_target_json") &&
         engineSymbol<decltype(&neverd_byte_patch_source_preview_json)>(
             "neverd_byte_patch_source_preview_json");
}

Json Engine::bytePatchRows() const {
  const auto &F = patches();
  if (!F)
    return Json::array();
  if (!F.snapshot || !F.freeSnapshot)
    return backendJson(F.rows(session_));

  std::unique_ptr<const neverd_byte_patch_snapshot_v1, decltype(F.freeSnapshot)>
      Snapshot(F.snapshot(session_), F.freeSnapshot);
  if (!Snapshot)
    throw Error("engine_error", error());
  Json::array_t Rows;
  Rows.reserve(Snapshot->row_count);
  for (size_t I = 0; I < Snapshot->row_count; ++I)
    Rows.emplace_back(patchRowJson(Snapshot->rows[I]));
  return Json(std::move(Rows));
}
bool Engine::reloadBytePatches(bool knownChanged) {
  if (!patches())
    return true;
  const auto Before = knownChanged ? Json() : bytePatchRows();
  if (patches().load(session_) != 0)
    return false;
  if (knownChanged || Before != bytePatchRows())
    functionsChanged();
  return true;
}
Json Engine::patchOperation(const std::string &Operation, const Json &P,
                            EncodedPayload *Encoded) {
  requireLoaded();
  if (Operation == "source_patch" || Operation == "source_patch_context") {
    const auto Context =
        engineSymbol<decltype(&neverd_source_patch_context_json)>(
            "neverd_source_patch_context_json");
    const auto Compile =
        engineSymbol<decltype(&neverd_patch_source)>("neverd_patch_source");
    const auto Report = engineSymbol<decltype(&neverd_patch_source_report)>(
        "neverd_patch_source_report");
    if (!Context || !Compile || !Report)
      throw Error("unsupported", "Engine does not support source replacement");
    const auto At = parseAddress(stringField(P, "address"));
    if (Operation == "source_patch_context") {
      const auto *Raw = Context(session_, At);
      if (!Raw)
        throw Error("source_patch_refused", error());
      return backendJson(Raw);
    }
    if (!bytePatchRows().empty())
      throw Error("source_patch_refused", "Export and open the byte-patched "
                                          "binary before rewriting a function");
    const auto Source =
        stringField(P, "source", {}, ::neverd::sdk::kMaxSourcePatchInputBytes);
    const auto Output = stringField(P, "path");
    const auto Compiler = stringField(P, "compiler", "clang");
    const auto CompilerPath = stringField(P, "compiler_path");
    const auto Symbol = stringField(P, "symbol");
    neverd_source_patch_options_v1 O{};
    O.struct_size = sizeof(O);
    O.compiler = Compiler == "clang"    ? NEVERD_SOURCE_PATCH_CLANG
                 : Compiler == "neverc" ? NEVERD_SOURCE_PATCH_NEVERC
                                        : NEVERD_SOURCE_PATCH_GCC;
    if (Compiler != "clang" && Compiler != "neverc" && Compiler != "gcc")
      throw Error("invalid_request", "Unknown compiler");
    O.artifact = Compiler == "gcc" ? NEVERD_SOURCE_PATCH_ARTIFACT_OBJECT
                                   : NEVERD_SOURCE_PATCH_ARTIFACT_IR;
    O.compiler_path = CompilerPath.c_str();
    O.source_name = "replacement.c";
    O.timeout_seconds = sizeField(
        P, "timeout_seconds", ::neverd::sdk::kDefaultSourcePatchTimeoutSeconds,
        ::neverd::sdk::kMaxSourcePatchTimeoutSeconds);
    const neverd_source_patch_mapping_v1 Mapping{Symbol.c_str(), At};
    O.mappings = &Mapping;
    O.mapping_count = 1;
    const auto Rows = P.value("bindings", Json::array());
    if (!Rows.is_array() ||
        Rows.size() > ::neverd::sdk::kMaxSourcePatchBindings)
      throw Error("invalid_request", "Invalid source bindings");
    std::vector<std::string> Names;
    Names.reserve(Rows.size());
    std::vector<neverd_source_patch_binding_v1> Bindings;
    Bindings.reserve(Rows.size());
    for (const auto &Row : Rows) {
      Names.push_back(stringField(Row, "symbol"));
      const auto Kind = stringField(Row, "kind");
      const uint32_t K = Kind == "function" ? NEVERD_SOURCE_BIND_FUNCTION
                         : Kind == "const"  ? NEVERD_SOURCE_BIND_READ_ONLY_DATA
                         : Kind == "data"
                             ? NEVERD_SOURCE_BIND_WRITABLE_DATA
                             : NEVERD_SOURCE_BIND_ORIGINAL_FUNCTION;
      if (Kind != "function" && Kind != "const" && Kind != "data" &&
          Kind != "original")
        throw Error("invalid_request", "Unknown source binding kind");
      Bindings.push_back(
          {Names.back().c_str(), K, parseAddress(stringField(Row, "address")),
           sizeField(Row, "size", 0, std::numeric_limits<size_t>::max())});
    }
    O.bindings = Bindings.data();
    O.binding_count = Bindings.size();
    if (!Compile(session_, Source.data(), Source.size(), &O, Output.c_str()))
      throw Error("source_patch_failed", error());
    auto Result = backendJson(Report(session_));
    Result["path"] = Output;
    return Result;
  }
  if (Operation == "patch_source_target") {
    const auto Prove =
        engineSymbol<decltype(&neverd_byte_patch_source_target_json)>(
            "neverd_byte_patch_source_target_json");
    if (!Prove)
      throw Error("unsupported", "Engine has no source patch proof");
    const auto Representation = stringField(P, "representation", "c");
    const auto *Raw =
        Prove(session_, parseAddress(stringField(P, "function")),
              Representation.c_str(),
              sizeField(P, "line", 0, ::neverd::sdk::kMaxSourcePatchInputBytes),
              P.value("literal", int64_t(-1)));
    if (!Raw)
      throw Error("patch_refused", error());
    return backendJson(Raw);
  }
  if (!patches())
    throw Error("unsupported", "This engine does not support byte patches");
  if (Operation == "patches") {
    const auto &F = patches();
    if (P.value("ranges_only", false)) {
      if (F.rangeSnapshot && F.freeRangeSnapshot) {
        std::unique_ptr<const neverd_byte_patch_range_snapshot_v1,
                        decltype(F.freeRangeSnapshot)>
            Snapshot(F.rangeSnapshot(session_), F.freeRangeSnapshot);
        if (!Snapshot)
          throw Error("engine_error", error());
        if (Encoded &&
            Snapshot->range_count >=
                ::neverd::limits::worker::kPatchMinEncodedRangeRows) {
          // Keep the SDK owner alive while one reusable row is encoded. The
          // envelope still supplies current request metadata and frame limits.
          Json Item = {{"address", ""}, {"size", 0u}};
          auto &Address = Item.at("address").get_ref<std::string &>();
          auto &Size = Item.at("size");
          *Encoded = std::make_shared<EncodedJson>(
              Json::object_t{{"complete", true},
                             {"total", Snapshot->range_count}},
              "items", Snapshot->range_count,
              [&](std::size_t I) -> const Json & {
                const auto &Range = Snapshot->ranges[I];
                Address = hexAddress(Range.address);
                Size = Range.size;
                return Item;
              });
          return Json(nullptr);
        }
        Json::array_t Ranges;
        Ranges.reserve(Snapshot->range_count);
        for (size_t I = 0; I < Snapshot->range_count; ++I) {
          const auto &Range = Snapshot->ranges[I];
          Json::object_t Item;
          Item.emplace("address", hexAddress(Range.address));
          Item.emplace("size", Range.size);
          Ranges.emplace_back(std::move(Item));
        }
        return {{"items", std::move(Ranges)},
                {"total", Snapshot->range_count},
                {"complete", true}};
      }
      if (F.ranges) {
        auto Ranges = backendJson(F.ranges(session_));
        const auto Total = Ranges.size();
        return {
            {"items", std::move(Ranges)}, {"total", Total}, {"complete", true}};
      }
      Json Ranges = Json::array();
      for (const auto &Row : bytePatchRows())
        Ranges.push_back({{"address", Row.at("va")}, {"size", Row.at("size")}});
      const auto Total = Ranges.size();
      return {
          {"items", std::move(Ranges)}, {"total", Total}, {"complete", true}};
    }
    const auto Offset =
        sizeField(P, "offset", 0, ::neverd::limits::patch::kMaxPatchRows);
    const auto Limit = sizeField(
        P, "limit", ::neverd::limits::worker::kEnginePageLimitSizeFieldDefault,
        ::neverd::limits::worker::kEnginePageLimitSizeFieldMax);
    Json::array_t Page;
    std::size_t Total;
    const auto Append = [&](Json Row) {
      Row["address"] = Row.at("va");
      Page.push_back(std::move(Row));
    };
    if (F.snapshot && F.freeSnapshot) {
      std::unique_ptr<const neverd_byte_patch_snapshot_v1,
                      decltype(F.freeSnapshot)>
          Snapshot(F.snapshot(session_), F.freeSnapshot);
      if (!Snapshot)
        throw Error("engine_error", error());
      Total = Snapshot->row_count;
      const auto Begin = std::min(Offset, Total);
      const auto Count = std::min(Limit, Total - Begin);
      Page.reserve(Count);
      // The owning SDK inventory is complete; only the requested rows need
      // JSON objects and presentation addresses. Empty pages build no DOM.
      for (std::size_t I = Begin; I < Begin + Count; ++I)
        Append(patchRowJson(Snapshot->rows[I]));
    } else {
      auto Rows = bytePatchRows();
      Total = Rows.size();
      const auto Begin = std::min(Offset, Total);
      const auto Count = std::min(Limit, Total - Begin);
      Page.reserve(Count);
      for (std::size_t I = Begin; I < Begin + Count; ++I)
        Append(std::move(Rows[I]));
    }
    const bool Complete = Offset + Page.size() >= Total;
    return {{"items", std::move(Page)},
            {"total", Total},
            {"offset", Offset},
            {"complete", Complete}};
  }
  if (Operation == "patch_export") {
    const auto Output = stringField(P, "path");
    if (patches().exportFile(session_, Output.c_str(),
                             P.value("allow_unsigned", false)) != 0)
      throw Error("export_failed", error());
    return {{"path", Output}, {"saved", true}};
  }
  auto Address = parseAddress(stringField(P, "address"));
  if (P.contains("function")) {
    auto Proof = patchOperation("patch_source_target", P);
    if (Proof.at("source_anchor") != P.at("source_anchor"))
      throw Error("stale_preview",
                  "Source text changed; reopen the patch menu");
    const auto Kind = stringField(P, "kind");
    bool Allowed = false;
    for (const auto &K : Proof.at("actions"))
      Allowed |= K == Kind;
    if (!Allowed)
      throw Error("patch_refused",
                  "Requested edit is not proven by the source target");
    Address = parseAddress(Proof.at("address").get<std::string>());
  }
  if (Operation == "patch_context") {
    const auto *Raw = patches().context(session_, Address);
    if (!Raw)
      throw Error("patch_refused", error());
    return backendJson(Raw);
  }
  const bool Preview =
      Operation == "patch_preview" || Operation == "patch_source_preview";
  if (!Preview) {
    requireWriter();
    if (dirty_)
      throw Error("unsaved_changes",
                  "Save or reload staged annotations before patching");
  }
  neverd_byte_patch_options_v1 Options{};
  Options.struct_size = sizeof(Options);
  Options.address = Address;
  const auto Kind = stringField(P, "kind", "bytes");
  const auto Bytes =
      stringField(P, "bytes", {}, ::neverd::limits::worker::kPatchHexBytes);
  const auto Text =
      stringField(P, "text", {}, ::neverd::limits::worker::kPatchTextBytes);
  const auto Expected =
      stringField(P, "expected", {}, ::neverd::limits::worker::kPatchHexBytes);
  Options.kind = Kind.c_str();
  Options.bytes = Bytes.c_str();
  Options.text = Text.c_str();
  auto Bindings = P.value("symbols", Json::object());
  if (!Bindings.is_object())
    throw Error("invalid_request", "Assembly symbols must be an object");
  if (Kind == "assemble") {
    const auto Frame = listing_->assemblySymbols(Address);
    for (const auto &[Name, Offset] : Frame.items())
      if (!Bindings.contains(Name) && Text.find(Name) != std::string::npos)
        Bindings[Name] = Offset;
  }
  const auto Symbols = Bindings.dump();
  if (Symbols.size() > ::neverd::limits::patch::kMaxAssemblyTextBytes)
    throw Error("invalid_request", "Assembly symbols exceed the text budget");
  Options.symbols = Symbols.empty() ? nullptr : Symbols.c_str();
  Options.expected = P.contains("expected") ? Expected.c_str() : nullptr;
  Options.size =
      sizeField(P, "size", 0, ::neverd::limits::patch::kMaxApplyBytes);
  Options.value =
      P.contains("value") ? parseAddress(stringField(P, "value")) : 0;
  Options.fill_nops = P.value("fill_nops", true);
  Options.allow_overlap = P.value("allow_overlap", false);
  const auto Dialect = stringField(P, "dialect", "intel");
  if (Dialect != "intel" && Dialect != "att")
    throw Error("invalid_request", "Unknown assembler dialect");
  Options.dialect = Dialect == "att";
  if (Operation == "patch_source_preview") {
    const auto PreviewSource =
        engineSymbol<decltype(&neverd_byte_patch_source_preview_json)>(
            "neverd_byte_patch_source_preview_json");
    if (!PreviewSource)
      throw Error("unsupported", "Engine has no source preview");
    const auto Representation = stringField(P, "representation", "c");
    const auto *Raw =
        PreviewSource(session_, parseAddress(stringField(P, "function")),
                      Representation.c_str(), &Options);
    if (!Raw)
      throw Error("patch_refused", error());
    return backendJson(Raw);
  }
  if (Preview) {
    const char *Raw = patches().preview(session_, &Options);
    if (!Raw)
      throw Error("patch_refused", error());
    return backendJson(Raw);
  }
  auto &Store = history();
  Json Result;
  if (Operation == "patch_revert") {
    Json Ranges =
        P.value("ranges", Json::array({{{"address", hexAddress(Address)},
                                        {"size", Options.size}}}));
    if (!Ranges.is_array() || Ranges.empty() ||
        Ranges.size() > ::neverd::limits::patch::kMaxPatchRows)
      throw Error("invalid_request", "Invalid patch selection");
    std::vector<std::pair<uint64_t, uint64_t>> Selected;
    for (const auto &Range : Ranges)
      Selected.emplace_back(
          parseAddress(stringField(Range, "address")),
          sizeField(Range, "size", 0,
                    ::neverd::limits::patch::kMaxPatchedBytes));
    for (const auto &[At, Size] : Selected)
      if (patches().revert(session_, At, Size) != 0) {
        const auto Reason = error();
        (void)patches().load(session_);
        functionsChanged();
        throw Error("patch_refused", Reason);
      }
    Result = {{"address", hexAddress(Address)}};
  } else {
    const char *Raw = patches().apply(session_, &Options);
    if (!Raw)
      throw Error("patch_refused", error());
    Result = backendJson(Raw);
  }
  const auto &OldRows = Store.committedPatches();
  ProjectHistory::PatchRows NewRows;
  try {
    const auto &F = patches();
    if (F.snapshot && F.freeSnapshot) {
      std::unique_ptr<const neverd_byte_patch_snapshot_v1,
                      decltype(F.freeSnapshot)>
          Snapshot(F.snapshot(session_), F.freeSnapshot);
      if (!Snapshot)
        throw Error("engine_error", error());
      NewRows.reserve(Snapshot->row_count);
      auto Old = OldRows.begin();
      for (size_t I = 0; I < Snapshot->row_count; ++I) {
        const auto &Row = Snapshot->rows[I];
        while (Old != OldRows.end() && (*Old)->address() < Row.file_offset)
          ++Old;
        if (Old != OldRows.end() && (*Old)->address() == Row.file_offset &&
            matchesPatchRow((*Old)->value(), Row))
          NewRows.push_back(*Old);
        else
          NewRows.push_back(ProjectHistory::patchRow(patchRowJson(Row)));
      }
    } else {
      // The older JSON ABI can include extra fields and escaped strings.
      auto Rows = bytePatchRows();
      NewRows.reserve(Rows.size());
      for (auto &Row : Rows)
        NewRows.push_back(ProjectHistory::patchRow(std::move(Row)));
    }
    std::size_t PatchStateBytes = 2;
    for (const auto &Row : NewRows)
      PatchStateBytes += Row->encoded().size();
    if (!NewRows.empty())
      PatchStateBytes += NewRows.size() - 1;
    if (PatchStateBytes > ::neverd::limits::worker::kPatchStateBytes)
      throw Error("patch_limit", "Patch history exceeds the worker transport "
                                 "budget; use the SDK for larger overlays");
  } catch (...) {
    (void)patches().load(session_);
    functionsChanged();
    throw;
  }
  // Only changed owning rows enter the command. Reused handles also avoid
  // comparing all seven JSON fields a second time.
  auto Old = OldRows.cbegin(), New = NewRows.cbegin();
  Json Command;
  const Json Missing;
  while (Old != OldRows.end() || New != NewRows.end()) {
    const auto OldAt = Old != OldRows.end() ? (*Old)->address() : 0;
    const auto NewAt = New != NewRows.end() ? (*New)->address() : 0;
    const bool TakeOld =
        Old != OldRows.end() && (New == NewRows.end() || OldAt <= NewAt);
    const bool TakeNew =
        New != NewRows.end() && (Old == OldRows.end() || NewAt <= OldAt);
    const auto At = TakeOld ? OldAt : NewAt;
    const auto BeforeRow = TakeOld ? *Old++ : nullptr;
    const auto AfterRow = TakeNew ? *New++ : nullptr;
    if (BeforeRow == AfterRow)
      continue;
    const auto &Before = BeforeRow ? BeforeRow->value() : Missing;
    const auto &After = AfterRow ? AfterRow->value() : Missing;
    if (Before == After)
      continue;
    Json Change{
        {"address", hexAddress(At)}, {"before", Before}, {"after", After}};
    if (Command.is_null()) {
      Command = std::move(Change);
      Command["kind"] = "patch";
    } else
      Command["rows"].push_back(std::move(Change));
  }
  if (Command.is_null()) {
    Result["saved"] = false;
    return Result;
  }
  const auto Checkpoint = Store;
  try {
    Store.stage(std::move(Command));
    Store.persistPatches(std::move(NewRows));
  } catch (...) {
    Store = Checkpoint;
    (void)patches().load(session_);
    functionsChanged();
    throw;
  }
  // This edit always invalidates analysis below; no before/after JSON
  // comparison is needed just to acknowledge its persisted SDK overlay.
  if (!reloadBytePatches(true))
    throw Error("reload_failed", error());
  ++revision_;
  Result["saved"] = true;
  Result["revision"] = revision();
  return Result;
}
} // namespace neverd::worker
