//===- Catalog.cpp - Typed offline MCP tools ------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Transport schemas do not infer evidence or replace C API semantic checks.
///
//===----------------------------------------------------------------------===//

#include "Catalog.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <set>

namespace neverd::mcp {
namespace {
Field text(const char *Name, std::size_t Maximum = 64, bool Required = true,
           bool Empty = false) {
  return {Name, Field::Kind::String, Required, Maximum, Empty ? 0U : 1U};
}
Field decimal(const char *Name) {
  return {Name, Field::Kind::Decimal, true, 20, 1};
}
Field number(const char *Name, std::size_t Maximum, bool Required = false,
             std::size_t Minimum = 0) {
  return {Name, Field::Kind::Unsigned, Required, Maximum, Minimum};
}
Field choice(const char *Name, std::initializer_list<const char *> Choices) {
  auto F = text(Name);
  for (const auto *C : Choices)
    F.Choices.push_back(C);
  return F;
}
bool decimalValue(std::string_view Text, uint64_t &Value) {
  const auto Result =
      std::from_chars(Text.data(), Text.data() + Text.size(), Value);
  return !Text.empty() && Result.ec == std::errc{} &&
         Result.ptr == Text.data() + Text.size() &&
         std::to_string(Value) == Text;
}
bool boundedString(const Json &Value, std::size_t Minimum,
                   std::size_t Maximum) {
  if (!Value.is_string())
    return false;
  const auto &S = Value.get_ref<const std::string &>();
  return S.size() >= Minimum && S.size() <= Maximum &&
         S.find('\0') == std::string::npos;
}
void validate(const Field &F, const Json &Value) {
  bool Valid = false;
  switch (F.Type) {
  case Field::Kind::String:
  case Field::Kind::Decimal: {
    Valid = boundedString(Value, F.Minimum, F.Maximum);
    if (Valid && F.Type == Field::Kind::Decimal) {
      uint64_t Number;
      Valid = decimalValue(Value.get_ref<const std::string &>(), Number);
    }
    break;
  }
  case Field::Kind::Unsigned:
    Valid = Value.is_number_integer() &&
            (Value.is_number_unsigned() || Value.get<int64_t>() >= 0) &&
            Value.get<uint64_t>() >= F.Minimum &&
            Value.get<uint64_t>() <= F.Maximum;
    break;
  case Field::Kind::StringArray:
    Valid = Value.is_array() && Value.size() >= F.Minimum &&
            Value.size() <= F.Maximum;
    if (Valid)
      for (const auto &Item : Value)
        Valid &= boundedString(Item, 1, 64);
    break;
  }
  if (!Valid ||
      (!F.Choices.empty() &&
       std::find(F.Choices.begin(), F.Choices.end(), Value) == F.Choices.end()))
    throw RpcError{-32602, "Invalid tool arguments"};
}
} // namespace

Json Tool::definition() const {
  Json Properties = Json::object(), Required = Json::array();
  for (const auto &F : Fields) {
    Json Schema;
    if (F.Type == Field::Kind::Unsigned) {
      Schema = {
          {"type", "integer"}, {"minimum", F.Minimum}, {"maximum", F.Maximum}};
    } else if (F.Type == Field::Kind::StringArray) {
      Schema = {
          {"type", "array"},
          {"minItems", F.Minimum},
          {"maxItems", F.Maximum},
          {"items", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}}};
    } else {
      Schema = {{"type", "string"},
                {"minLength", F.Minimum},
                {"maxLength", F.Maximum},
                {"description",
                 "Bound also applies to UTF-8 bytes; NUL is not accepted."}};
      if (F.Type == Field::Kind::Decimal)
        Schema["pattern"] = "^(0|[1-9][0-9]*)$";
    }
    if (!F.Choices.empty())
      Schema["enum"] = F.Choices;
    Properties[F.Name] = std::move(Schema);
    if (F.Required)
      Required.push_back(F.Name);
  }
  Properties["schema_version"] = {
      {"type", "integer"}, {"const", 1}, {"default", 1}};
  return {{"name", Name},
          {"description", Description},
          {"inputSchema",
           {{"type", "object"},
            {"properties", std::move(Properties)},
            {"required", std::move(Required)},
            {"additionalProperties", false}}},
          {"annotations",
           {{"readOnlyHint", ReadOnly},
            {"destructiveHint", false},
            {"idempotentHint", ReadOnly},
            {"openWorldHint", false}}}};
}

Json Tool::payload(const Json &Arguments) const {
  if (!Arguments.is_object())
    throw RpcError{-32602, "Invalid tool arguments"};
  for (const auto &[Key, Value] : Arguments.items()) {
    if (Key == "schema_version") {
      if (!Value.is_number_integer() || Value != 1)
        throw RpcError{-32602, "Invalid tool arguments"};
      continue;
    }
    const auto It = std::find_if(Fields.begin(), Fields.end(),
                                 [&](const auto &F) { return F.Name == Key; });
    if (It == Fields.end())
      throw RpcError{-32602, "Invalid tool arguments"};
    validate(*It, Value);
  }
  for (const auto &F : Fields)
    if (F.Required && !Arguments.contains(F.Name))
      throw RpcError{-32602, "Invalid tool arguments"};
  auto Payload = Arguments;
  Payload["schema_version"] = 1;
  return Payload;
}

Catalog::Catalog(const Json &Capabilities, std::size_t InputCount) {
  std::set<std::string> Available;
  if (Capabilities.is_object() && Capabilities.value("status", "") == "ok" &&
      Capabilities.contains("operations") &&
      Capabilities["operations"].is_array())
    for (const auto &Operation : Capabilities["operations"])
      if (Operation.is_string())
        Available.insert("web_" + Operation.get<std::string>());
  auto Add = [&](const char *Operation, const char *Description,
                 std::vector<Field> Fields = {}, bool ReadOnly = false,
                 Tool::Adapter Adapter = Tool::Adapter::Backend) {
    if (Adapter != Tool::Adapter::Capabilities &&
        !Available.contains(Operation))
      return;
    Tools.push_back({"neverd_" + std::string(Operation), Operation, Description,
                     std::move(Fields), Adapter, ReadOnly});
  };
  auto Selected = [&](const char *Operation, const char *ID,
                      const char *Description) {
    Add(Operation, Description, {decimal("revision"), text(ID)});
  };
  auto Page = [&](const char *Operation, const char *ID, bool Kind = false,
                  std::size_t Limit = 512, std::size_t KindBytes = 16) {
    std::vector<Field> Fields{decimal("revision")};
    if (ID)
      Fields.push_back(text(ID));
    if (Kind)
      Fields.push_back(text("record_kind", KindBytes));
    Fields.push_back(number("offset", 2147483647));
    Fields.push_back(number("limit", Limit, false, 1));
    Add(Operation,
        "Return a bounded page of evidence metadata; no raw asset content.",
        std::move(Fields), true);
  };
  Add("web_capabilities",
      "Report backend capabilities, MCP restrictions and configured input "
      "count.",
      {}, true, Tool::Adapter::Capabilities);
  if (InputCount)
    Add("web_import_preview",
        "Preview capture of a launch-configured input; this does not publish "
        "it.",
        {number("input_index", InputCount - 1, true)}, false,
        Tool::Adapter::InputPreview);
  Add("web_import_commit",
      "Publish the exact preview token and revoke the previous revision.",
      {text("preview_token")});
  Add("web_metadata",
      "Read authoritative project, revision and analysis metadata.", {}, true);
  Page("web_artifacts", nullptr);
  Selected(
      "web_bun_extract", "artifact_id",
      "Extract explicitly supported Bun source and asset regions offline.");
  Page("web_bun_records", "extraction_id", true);
  Add("web_sea_extract",
      "Extract a Node SEA resource under an explicit supported layout profile.",
      {decimal("revision"), text("artifact_id"), text("profile")});
  Page("web_sea_records", "extraction_id", false, 128);
  Add("web_asar_extract",
      "Extract admitted ASAR members with an optional captured unpacked "
      "directory.",
      {decimal("revision"), text("artifact_id"),
       text("unpacked_directory_id", 64, false, true)});
  Page("web_asar_records", "extraction_id");
  Add("web_package_archive_extract",
      "Extract bounded tar or tgz package members without executing scripts.",
      {decimal("revision"), text("artifact_id"),
       choice("format", {"tar", "tgz"})});
  Page("web_package_archive_records", "archive_id");
  Add("web_packages_analyze",
      "Analyze a selected package document using its explicit input kind.",
      {decimal("revision"), text("artifact_id"), text("input_kind", 32)});
  Page("web_package_records", "analysis_id", true, 512, 32);
  Add("web_packages_compare",
      "Compare two retained package analyses in the same revision.",
      {decimal("revision"), text("before_id"), text("after_id")});
  Page("web_package_diff_records", "diff_id");
  Add("web_package_integrity_verify",
      "Compare captured bytes to selected integrity declarations; not a "
      "benignness verdict.",
      {decimal("revision"), text("artifact_id"), text("declaration_id"),
       text("package_id", 64, false)});
  Selected(
      "web_har_preview", "artifact_id",
      "Preview metadata-only import and redaction of a captured HAR artifact.");
  Selected("web_har_commit", "preview_token",
           "Commit the selected HAR preview without releasing private values.");
  Page("web_har_records", "capture_id", false, 128);
  Add("web_stream_preview",
      "Preview passive stream/log evidence under an explicit supported framing "
      "profile.",
      {decimal("revision"), text("artifact_id"), text("profile")});
  Selected(
      "web_stream_commit", "preview_token",
      "Commit the selected stream preview without releasing private values.");
  Page("web_stream_records", "capture_id", false, 128);
  Selected("web_interfaces_analyze", "source_id",
           "Find supported source-inferred network interface candidates "
           "without network access.");
  Page("web_interface_records", "analysis_id", false, 128);
  Add("web_interfaces_compare",
      "Correlate explicit source candidates with imported HAR observations; "
      "evidence classes remain separate.",
      {decimal("revision"), text("analysis_id"), text("capture_id")});
  Page("web_interface_correlation_records", "correlation_id", false, 128);
  Selected("web_electron_manifest_analyze", "artifact_id",
           "Analyze captured Electron manifest entry metadata.");
  Selected(
      "web_electron_source_analyze", "source_id",
      "Find source-visible Electron boundaries; no application execution.");
  Page("web_electron_source_records", "source_id");
  for (const auto *Operation :
       {"web_electron_ipc_analyze", "web_electron_entries_analyze"})
    Add(Operation,
        "Compare explicitly selected Electron source evidence with a captured "
        "manifest.",
        {decimal("revision"),
         text("manifest_artifact_id"),
         {"source_ids", Field::Kind::StringArray, true, 16, 1}});
  Page("web_electron_ipc_records", "electron_ipc_id", true);
  Page("web_electron_entry_records", "electron_entries_id", true);
  Selected("web_html_analyze", "artifact_id",
           "Inspect captured HTML scripts and import-map candidates without "
           "fetching references.");
  Page("web_html_records", "html_id", true);
  Add("web_source_analyze",
      "Parse admitted source with the embedded parser; do not execute it.",
      {decimal("revision"), text("artifact_id"),
       choice("source_type", {"script", "module", "commonjs"})});
  Page("web_source_nodes", "source_id");
  for (const auto *Operation :
       {"web_source_bindings_analyze", "web_source_semantics_analyze",
        "web_source_modules_analyze", "web_source_bundles_analyze",
        "web_source_navigation_analyze"})
    Selected(Operation, "source_id",
             "Analyze supported source relationships and explicit unresolved "
             "boundaries offline.");
  for (const auto *Operation :
       {"web_source_binding_records", "web_source_module_records",
        "web_source_bundle_records", "web_source_navigation_records"})
    Page(Operation, "source_id", true);
  Page("web_source_semantic_records", "source_id");
  Add("web_source_anchor",
      "Resolve a source byte span to original immutable storage and optional "
      "structural view coordinates.",
      {decimal("revision"), text("source_id"), decimal("byte_offset"),
       decimal("byte_length"), text("view_id", 64, false, true)},
      true);
  Selected("web_source_view_preview", "source_id",
           "Preview a structural source view; MCP cannot mark target ranges as "
           "reviewed.");
  Selected("web_source_view_commit", "preview_token",
           "Commit the structural view preview; no reviewed target ranges are "
           "enabled.");
  Page("web_source_view_records", "view_id");
  Add("web_source_view_chunk",
      "Read a bounded structural view chunk with private source text replaced.",
      {decimal("revision"), text("view_id"), decimal("byte_offset"),
       number("byte_limit", 65536, false, 1)},
      true);
  Selected("web_source_map_analyze", "artifact_id",
           "Validate a selected local source map; never fetch URLs or "
           "authenticate its claims.");
  Page("web_source_map_sources", "map_id");
  Page("web_source_map_segments", "map_id");
  Add("web_source_map_lookup",
      "Look up a generated source coordinate under an explicitly selected map.",
      {decimal("revision"), text("map_id"), text("generated_source_id"),
       decimal("byte_offset")},
      true);
  Selected("web_native_open", "selection_id",
           "Open selected immutable native bytes for loader metadata only; no "
           "target execution.");
  Add("web_native_metadata",
      "Read metadata for the selected native handoff; deep native analysis is "
      "outside this MCP profile.",
      {decimal("revision"), text("handoff_id")}, true);
  std::sort(Tools.begin(), Tools.end(),
            [](const auto &A, const auto &B) { return A.Name < B.Name; });
}

const Tool &Catalog::find(std::string_view Name) const {
  const auto It =
      std::lower_bound(Tools.begin(), Tools.end(), Name,
                       [](const auto &T, auto N) { return T.Name < N; });
  if (It == Tools.end() || It->Name != Name)
    throw RpcError{-32602, "Unknown or unavailable tool"};
  return *It;
}

Json Catalog::list(const Json &Parameters) const {
  uint64_t Offset = 0;
  constexpr std::size_t PageSize = 16;
  if (Parameters.contains("cursor")) {
    const auto Cursor = Parameters["cursor"].get<std::string>();
    constexpr std::string_view Prefix = "web-tools-v1:";
    if (!Cursor.starts_with(Prefix) ||
        !decimalValue(std::string_view(Cursor).substr(Prefix.size()), Offset) ||
        Offset == 0 || Offset % PageSize != 0 || Offset >= Tools.size())
      throw RpcError{-32602, "Invalid tool cursor"};
  }
  Json Items = Json::array();
  const auto End = std::min<std::size_t>(Tools.size(), Offset + PageSize);
  for (auto I = Offset; I < End; ++I)
    Items.push_back(Tools[I].definition());
  Json Result{{"tools", std::move(Items)}};
  if (End < Tools.size())
    Result["nextCursor"] = "web-tools-v1:" + std::to_string(End);
  return Result;
}

Json Catalog::names() const {
  Json Result = Json::array();
  for (const auto &T : Tools)
    Result.push_back(T.Name);
  return Result;
}
} // namespace neverd::mcp
