//===- BunProject.cpp - Derived Bun reconstruction projects ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Export source candidates and loader evidence without invoking a runtime.
///
//===----------------------------------------------------------------------===//

#include "BunProject.h"

#include "BunProjectRequire.h"
#include "BunProjectSource.h"
#include "ExportDirectory.h"

#include "neverd/web/SourceRecovery.h"

#include <algorithm>
#include <set>

namespace neverd::web {
namespace {
struct MetadataBudget {
  BunProjectLimits Limits;
  uint64_t Records = 0, UTF16Units = 0;

  explicit MetadataBudget(BunProjectLimits Value) : Limits(Value) {
    if (Limits.Records > BunProjectLimits::HardRecords ||
        Limits.UTF16Units > BunProjectLimits::HardUTF16Units)
      throw Error("bun_project_invalid_limits");
  }
  void records(uint64_t Count = 1) {
    if (Count > Limits.Records - Records)
      throw Error("bun_project_metadata_budget_exceeded");
    Records += Count;
  }
  void units(uint64_t Count) {
    if (Count > Limits.UTF16Units - UTF16Units)
      throw Error("bun_project_metadata_budget_exceeded");
    UTF16Units += Count;
  }
};

std::string file(size_t Index, std::string_view Extension) {
  auto Number = std::to_string(Index);
  Number.insert(0, 5 - std::min<size_t>(5, Number.size()), '0');
  return "m" + Number + std::string(Extension);
}
llvm::json::Array units(std::u16string_view Text, MetadataBudget &Budget) {
  Budget.units(Text.size());
  llvm::json::Array Result;
  for (auto Unit : Text)
    Result.emplace_back(uint64_t(Unit));
  return Result;
}

llvm::json::Array diagnostics(std::span<const SourceDiagnostic> Input,
                              MetadataBudget &Budget, uint64_t &Count) {
  Budget.records(Input.size());
  Count += Input.size();
  llvm::json::Array Result;
  for (const auto &D : Input)
    Result.emplace_back(
        llvm::json::Object{{"code", D.Code}, {"byte_offset", D.ByteOffset}});
  return Result;
}
std::string html(std::string_view Text) {
  std::string Result;
  for (const char C : Text) {
    switch (C) {
    case '&':
      Result += "&amp;";
      break;
    case '<':
      Result += "&lt;";
      break;
    case '>':
      Result += "&gt;";
      break;
    case '"':
      Result += "&quot;";
      break;
    default:
      Result += C;
      break;
    }
  }
  return Result;
}
} // namespace

std::string exportBunProject(const BunExtraction &E,
                             std::string_view OutputDirectory,
                             BunProjectLimits Limits) {
  MetadataBudget Budget(Limits);
  uint64_t Diagnostics = 0;
  if (E.Modules.empty() || E.Modules.size() > MaxBunModules ||
      E.EntryPoint >= E.Modules.size() || E.StartupCount > E.Modules.size())
    throw Error("bun_project_invalid_extraction");
  std::vector<BunProjectSourceTarget> Targets;
  std::set<std::string> Names;
  for (size_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    if (M.Name >= E.Regions.size() || M.Contents >= E.Regions.size())
      throw Error("bun_project_invalid_extraction");
    const auto &R = E.Regions[M.Name].Content;
    const auto Name = R.read(0, R.size(), MaxBunNameBytes);
    if (!validUtf8(Name) || !Names.insert(Name).second)
      throw Error("bun_project_ambiguous_virtual_name");
    const auto Extension = !M.SourceArtifactID.empty() ? ".js"
                           : M.Loader == 13            ? ".txt"
                           : M.Loader == 10            ? ".node"
                                                       : ".bin";
    Targets.push_back({Name, file(I, Extension)});
  }
  if (E.Modules[E.EntryPoint].SourceArtifactID.empty())
    throw Error("bun_project_entry_source_unavailable");
  std::vector<BunProjectRequireProvider> Providers;
  llvm::json::Array ProviderInventory;
  for (size_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    if (M.SourceArtifactID.empty())
      continue;
    const auto Text = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
    // A candidate filter only; lexical/module analysis owns the proof.
    if (Text.find("import.meta.require") == std::string::npos)
      continue;
    const auto Found = collectBunProjectRequireProviders(
        M.SourceArtifactID, Text, M.Format == 1 ? "module" : "commonjs",
        Targets[I].VirtualName);
    Budget.records();
    ProviderInventory.emplace_back(llvm::json::Object{
        {"module_index", I},
        {"status", Found.Status},
        {"provider_count", Found.Providers.size()},
        {"diagnostics", diagnostics(Found.Diagnostics, Budget, Diagnostics)}});
    if (Found.Providers.size() > MaxBunProjectRequireRecords - Providers.size())
      throw Error("bun_project_require_provider_budget_exceeded");
    Budget.records(Found.Providers.size());
    for (const auto &Provider : Found.Providers) {
      Budget.units(Provider.data().VirtualName.size());
      Budget.units(Provider.data().ExportedName.size());
    }
    Providers.insert(Providers.end(), Found.Providers.begin(),
                     Found.Providers.end());
  }
  std::set<std::u16string_view> ProviderNames;
  for (const auto &Provider : Providers)
    ProviderNames.insert(Provider.data().VirtualName);
  ExportDirectory Output(OutputDirectory);
  llvm::json::Array Modules;
  std::string IndexRows;
  uint64_t SourceCount = 0, AssetCount = 0, Relocations = 0;
  uint64_t RuntimeReferences = 0, FailedSources = 0;
  uint64_t ExternalReferences = 0;
  uint64_t RequireAliases = 0;
  uint64_t ReadableSources = 0, UnavailableReadable = 0;
  for (size_t I = 0; I < E.Modules.size(); ++I) {
    const auto &M = E.Modules[I];
    const auto &R = E.Regions[M.Contents];
    Budget.records();
    llvm::json::Object Row{{"module_index", I},
                           {"module_id", M.ID},
                           {"virtual_name", Targets[I].VirtualName},
                           {"file", Targets[I].OutputFile},
                           {"loader_id", M.Loader},
                           {"encoding_id", M.Encoding},
                           {"format_id", M.Format},
                           {"side_id", M.Side},
                           {"storage_sha256", R.BlobHash},
                           {"storage_bytes", std::to_string(R.Content.size())}};
    if (!M.SourceArtifactID.empty()) {
      const auto Text = bunSourceBytes(E, M, MaxBunDecodedSourceBytes);
      const auto OriginalFile = file(I, ".original.js");
      Output.write(OriginalFile, Text);
      Row["original_source_file"] = OriginalFile;
      Row["original_source_sha256"] = sha256(Text);
      auto Plan =
          planBunProjectSource(M.SourceArtifactID, Text,
                               M.Format == 1 ? "module" : "commonjs", Targets);
      bool ImportsProvider = false;
      for (const auto &Edit : Plan.Edits)
        if (Edit.Kind == "import" && ProviderNames.contains(Edit.OldTarget)) {
          ImportsProvider = true;
          break;
        }
      if (Plan.Status == "verified_literal_relocations" && ImportsProvider) {
        auto Calls = proveBunProjectRequireCalls(
            M.SourceArtifactID, Text, M.Format == 1 ? "module" : "commonjs",
            Providers, Targets);
        Row["require_alias_status"] = Calls.Status;
        llvm::json::Array Proofs;
        Row["require_alias_diagnostics"] =
            diagnostics(Calls.Diagnostics, Budget, Diagnostics);
        if (!Calls.Calls.empty()) {
          Plan = planBunProjectSource(M.SourceArtifactID, Text,
                                      M.Format == 1 ? "module" : "commonjs",
                                      Targets, Calls.Calls);
          for (const auto &Call : Calls.Calls) {
            Budget.records();
            const auto &D = Call.data();
            Proofs.emplace_back(llvm::json::Object{
                {"proof_id", D.ID},
                {"provider_id", D.ProviderID},
                {"provider_source_sha256", D.ProviderSourceHash},
                {"provider_binding_id", D.ProviderBindingID},
                {"provider_export_id", D.ProviderExportID},
                {"consumer_binding_id", D.ConsumerBindingID},
                {"consumer_import_id", D.ConsumerImportID},
                {"start", std::to_string(D.OriginalStart)},
                {"end", std::to_string(D.OriginalEnd)}});
          }
          if (Plan.Status == "verified_literal_relocations")
            RequireAliases += Calls.Calls.size();
        }
        if (Calls.Status == "ok") {
          for (auto &Reference : Calls.ExternalReferences)
            Plan.ExternalModuleReferences.push_back(std::move(Reference));
          std::sort(
              Plan.ExternalModuleReferences.begin(),
              Plan.ExternalModuleReferences.end(),
              [](const auto &A, const auto &B) { return A.Start < B.Start; });
        }
        Row["require_alias_proofs"] = std::move(Proofs);
      }
      Row["relocation_status"] = Plan.Status;
      Row["parse_status"] = Plan.ParseStatus;
      llvm::json::Array Edits;
      Budget.records(Plan.Edits.size());
      for (const auto &Edit : Plan.Edits)
        Edits.emplace_back(llvm::json::Object{
            {"start", std::to_string(Edit.Start)},
            {"end", std::to_string(Edit.End)},
            {"kind", Edit.Kind},
            {"old_target_utf16", units(Edit.OldTarget, Budget)},
            {"new_target_utf16", units(Edit.NewTarget, Budget)}});
      Relocations += Plan.Edits.size();
      RuntimeReferences += Plan.UnresolvedRuntimeReferences.size();
      ExternalReferences += Plan.ExternalModuleReferences.size();
      llvm::json::Array Unresolved;
      Budget.records(Plan.UnresolvedRuntimeReferences.size());
      for (const auto &D : Plan.UnresolvedRuntimeReferences)
        Unresolved.emplace_back(
            llvm::json::Object{{"start", std::to_string(D.Start)},
                               {"end", std::to_string(D.End)},
                               {"target_utf16", units(D.Target, Budget)}});
      Row["relocations"] = std::move(Edits);
      Row["diagnostics"] = diagnostics(Plan.Diagnostics, Budget, Diagnostics);
      Row["unresolved_runtime_references"] = std::move(Unresolved);
      llvm::json::Array External;
      Budget.records(Plan.ExternalModuleReferences.size());
      for (const auto &D : Plan.ExternalModuleReferences)
        External.emplace_back(
            llvm::json::Object{{"start", std::to_string(D.Start)},
                               {"end", std::to_string(D.End)},
                               {"target_utf16", units(D.Target, Budget)}});
      Row["external_module_references"] = std::move(External);
      // A failed plan never substitutes empty or partially changed source.
      if (Plan.Status != "verified_literal_relocations") {
        ++FailedSources;
        ++UnavailableReadable;
        Row["file"] = nullptr;
        Row["readable_status"] = "not_available_relocation_failed";
        Row["readable_diagnostics"] = llvm::json::Array{};
      } else {
        Output.write(Targets[I].OutputFile, Plan.Text);
        Row["sha256"] = sha256(Plan.Text);
        const auto Readable =
            recoverReadableJavaScript(M.SourceArtifactID, Plan.Text,
                                      M.Format == 1 ? "module" : "commonjs");
        Row["readable_status"] = Readable.Status;
        auto ReadableDiagnostics = Readable.Diagnostics;
        if (Readable.Status != "verified_same_parser_tree" &&
            ReadableDiagnostics.empty())
          ReadableDiagnostics.push_back({Readable.Status, -1});
        Row["readable_diagnostics"] =
            diagnostics(ReadableDiagnostics, Budget, Diagnostics);
        if (Readable.Status == "verified_same_parser_tree") {
          ++ReadableSources;
          const auto Name = file(I, ".readable.js");
          Output.write(Name, Readable.Text);
          Row["readable_file"] = Name;
          Row["readable_sha256"] = sha256(Readable.Text);
        } else
          ++UnavailableReadable;
      }
      ++SourceCount;
    } else {
      const auto Raw = file(I, ".storage.bin");
      Output.write(Raw, R.Content, R.BlobHash);
      Row["original_storage_file"] = Raw;
      if (M.Loader == 13) {
        const auto Text = bunTextBytes(E, M, MaxBunDecodedSourceBytes);
        Output.write(Targets[I].OutputFile, Text);
        Row["sha256"] = sha256(Text);
        Row["projection"] = "decoded_text_utf8";
      } else {
        Output.write(Targets[I].OutputFile, R.Content, R.BlobHash);
        Row["sha256"] = R.BlobHash;
        Row["projection"] = "exact_stored_bytes";
      }
      Row["runtime_loader_verified"] = false;
      if (M.Loader != 5 && M.Loader != 10 && M.Loader != 13) {
        Budget.records();
        Row["loader_status"] = "unsupported_reconstruction_loader";
        ++Diagnostics;
      }
      ++AssetCount;
    }
    IndexRows += "<tr id=\"m" + std::to_string(I) + "\"><td>" +
                 std::to_string(I) + (I == E.EntryPoint ? " (entry)" : "") +
                 "</td><td><code>" + html(Targets[I].VirtualName) +
                 "</code></td><td>";
    for (const auto Key : {"readable_file", "file", "original_source_file",
                           "original_storage_file"})
      if (const auto Name = Row.getString(Key))
        IndexRows += "<a href=\"" + Name->str() + "\">" + Key + "</a> ";
    IndexRows += "</td><td>" + std::to_string(M.Loader) + "</td></tr>\n";
    Modules.emplace_back(std::move(Row));
  }
  std::string Entry;
  for (uint32_t I = 0; I < E.StartupCount; ++I) {
    if (E.Modules[I].SourceArtifactID.empty())
      throw Error("bun_project_startup_asset_unsupported");
    Entry += "import \"./" + Targets[I].OutputFile + "\";\n";
  }
  if (E.EntryPoint >= E.StartupCount)
    Entry += "import \"./" + Targets[E.EntryPoint].OutputFile + "\";\n";
  Output.write("entry.js", Entry);
  Output.write("package.json", "{\"private\":true,\"type\":\"module\"}\n");
  Output.write("README.txt",
               "Derived Bun reconstruction candidate.\n"
               "Run the separately pinned Bun compiler on build.mjs only in "
               "the qualified isolated environment.\n"
               "manifest.json records every source/asset and unresolved "
               "virtual path literal and external module literal. Computed "
               "runtime dependencies remain unknown. Build/runtime behavior "
               "have not "
               "been verified by this exporter.\n"
               "*.original.js retain decoded source; *.js contain verified "
               "literal relocation; *.readable.js add verified whitespace.\n"
               "*.storage.bin retain asset bytes; .txt files are UTF-8 "
               "projections of Bun text-loader strings.\n"
               "Native addons remain original binary dependencies. Original "
               "TypeScript, deleted names/types and runtime equivalence are "
               "not claimed.\n");
  // This recipe is development scaffolding, not an engine-side subprocess.
  // The operator runs it separately in the tested isolated environment.
  Output.write(
      "build.mjs",
      R"JS(// Generated rebuild candidate; see manifest.json for unresolved semantics.
import {fileURLToPath} from 'node:url';
const result = await Bun.build({
  entrypoints: [fileURLToPath(new URL('./entry.js', import.meta.url))],
  target: 'bun',
  env: 'disable',
  macros: false,
  compile: {
    outfile: fileURLToPath(new URL('./client', import.meta.url)),
    executablePath: process.execPath,
  },
  minify: false,
});
if (!result.success) {
  for (const log of result.logs) console.error(log);
  process.exit(1);
}
)JS");
  llvm::json::Object Summary{
      {"schema_version", 1},
      {"status", "ok"},
      {"profile", "bun-reconstruction-candidate-v1"},
      {"extraction_id", E.ID},
      {"layout_profile", E.Profile},
      {"platform", E.Platform},
      {"architecture", E.Architecture},
      {"entry_point_index", E.EntryPoint},
      {"startup_count", E.StartupCount},
      {"flags", E.Flags},
      {"source_count", SourceCount},
      {"readable_source_count", ReadableSources},
      {"unavailable_readable_count", UnavailableReadable},
      {"asset_count", AssetCount},
      {"relocation_count", Relocations},
      {"require_alias_relocation_count", RequireAliases},
      {"diagnostic_count", Diagnostics},
      {"unresolved_runtime_reference_count", RuntimeReferences},
      {"external_module_reference_count", ExternalReferences},
      {"reference_inventory", "static_literals_and_nonliteral_diagnostics"},
      {"computed_dependency_closure", "unknown"},
      {"failed_source_count", FailedSources},
      {"build_status", "not_run"},
      {"runtime_equivalence", "not_verified"},
      {"runtime_options_applied", false},
      {"executes_input", false},
      {"includes_original_executable", false},
      {"includes_cached_bytecode", false},
      {"virtual_names_are_host_paths", false}};
  auto Manifest = Summary;
  Manifest["require_provider_inventory"] = std::move(ProviderInventory);
  const auto Index =
      "<!doctype html><meta charset=\"utf-8\"><meta "
      "http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; "
      "style-src 'unsafe-inline'\"><title>Bun reconstruction source "
      "index</title>"
      "<style>body{font:15px system-ui;margin:32px}td,th{padding:8px;"
      "text-align:left;border-bottom:1px solid "
      "#ddd}table{border-collapse:collapse}"
      "code{font-size:12px}</style><h1>Reconstruction source index</h1>"
      "<p>Captured distribution module boundaries; original TypeScript file "
      "structure and erased types remain unknown. Derived source paths and "
      "runtime behavior require separate qualification.</p>"
      "<p><a href=\"manifest.json\">Evidence and unresolved references</a></p>"
      "<table><tr><th>Module</th><th>Captured name</th><th>Source and "
      "asset files</th><th>Bun loader</th></tr>" +
      IndexRows + "</table>";
  Output.write("index.html", Index);
  Manifest["index_file"] = "index.html";
  Manifest["index_sha256"] = sha256(Index);
  llvm::json::Array Prelinked, Options;
  Budget.records(E.PrelinkedModuleFiles.size());
  for (const auto Index : E.PrelinkedModuleFiles)
    Prelinked.emplace_back(Index);
  if (E.RuntimeOptions) {
    Budget.records(E.RuntimeOptions->size());
    for (const auto Byte : *E.RuntimeOptions)
      Options.emplace_back(Byte);
  }
  Manifest["prelinked_module_files"] = std::move(Prelinked);
  Manifest["runtime_options_bytes"] = std::move(Options);
  if (E.CompileArguments != NoBunIndex) {
    if (E.CompileArguments >= E.Regions.size())
      throw Error("bun_project_invalid_extraction");
    const auto &R = E.Regions[E.CompileArguments];
    Output.write("compile-arguments.bin", R.Content, R.BlobHash);
    Manifest["compile_arguments_file"] = "compile-arguments.bin";
    Manifest["compile_arguments_sha256"] = R.BlobHash;
  }
  Manifest["modules"] = std::move(Modules);
  Manifest["metadata_record_count"] = Budget.Records;
  Manifest["metadata_utf16_unit_count"] = Budget.UTF16Units;
  Summary["metadata_record_count"] = Budget.Records;
  Summary["metadata_utf16_unit_count"] = Budget.UTF16Units;
  const auto Text = json(std::move(Manifest));
  Output.complete(Text);
  Summary["manifest_sha256"] = sha256(Text);
  Summary["file_count"] = Output.files();
  Summary["bytes_written"] = std::to_string(Output.bytes());
  return json(std::move(Summary));
}
} // namespace neverd::web
