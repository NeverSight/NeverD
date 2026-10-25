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

#include <map>

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
          "neverd_byte_patch_ranges_json")};
  return F;
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
  return patches() ? backendJson(patches().rows(session_)) : Json::array();
}
bool Engine::reloadBytePatches() {
  if (!patches())
    return true;
  const auto Before = bytePatchRows();
  if (patches().load(session_) != 0)
    return false;
  if (Before != bytePatchRows())
    functionsChanged();
  return true;
}
Json Engine::patchOperation(const std::string &Operation, const Json &P) {
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
    if (P.value("ranges_only", false) && patches().ranges) {
      auto Ranges = backendJson(patches().ranges(session_));
      return {{"items", Ranges}, {"total", Ranges.size()}, {"complete", true}};
    }
    auto Rows = bytePatchRows();
    for (auto &Row : Rows)
      Row["address"] = Row.at("va");
    if (P.value("ranges_only", false)) {
      Json Ranges = Json::array();
      for (const auto &Row : Rows)
        Ranges.push_back({{"address", Row.at("va")}, {"size", Row.at("size")}});
      return {{"items", Ranges}, {"total", Ranges.size()}, {"complete", true}};
    }
    const auto Offset =
        sizeField(P, "offset", 0, ::neverd::limits::patch::kMaxPatchRows);
    const auto Limit = sizeField(
        P, "limit", ::neverd::limits::worker::kEnginePageLimitSizeFieldDefault,
        ::neverd::limits::worker::kEnginePageLimitSizeFieldMax);
    Json Page = Json::array();
    for (size_t I = std::min(Offset, Rows.size());
         I < Rows.size() && Page.size() < Limit; ++I)
      Page.push_back(Rows[I]);
    return {{"items", Page},
            {"total", Rows.size()},
            {"offset", Offset},
            {"complete", Offset + Page.size() >= Rows.size()}};
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
  auto State = Store.committedState();
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
  std::map<uint64_t, std::pair<Json, Json>> Changes;
  for (const auto &Row : State.at("patches"))
    Changes[parseAddress(Row.at("addr").get<std::string>())].first = Row;
  State["patches"] = bytePatchRows();
  if (State["patches"].dump().size() >
      ::neverd::limits::worker::kPatchStateBytes) {
    (void)patches().load(session_);
    functionsChanged();
    throw Error("patch_limit", "Patch history exceeds the worker transport "
                               "budget; use the SDK for larger overlays");
  }
  for (const auto &Row : State.at("patches"))
    Changes[parseAddress(Row.at("addr").get<std::string>())].second = Row;
  Json Command;
  for (const auto &[At, Sides] : Changes) {
    if (Sides.first == Sides.second)
      continue;
    Json Change{{"address", hexAddress(At)},
                {"before", Sides.first},
                {"after", Sides.second}};
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
    Store.persist(State);
  } catch (...) {
    Store = Checkpoint;
    (void)patches().load(session_);
    functionsChanged();
    throw;
  }
  // This edit always invalidates analysis below; no before/after JSON
  // comparison is needed just to acknowledge its persisted SDK overlay.
  if (patches().load(session_) != 0)
    throw Error("reload_failed", error());
  functionsChanged();
  ++revision_;
  Result["saved"] = true;
  Result["revision"] = revision();
  return Result;
}
} // namespace neverd::worker
