#include "PathPolicy.h"
#include "RecoveryParser.h"
#include "SessionInternal.h"

#include "neverd/web/Source.h"

namespace neverd::web {

namespace {
constexpr uint64_t MaxCachedSources = 16;
constexpr uint64_t MaxCachedNodes = 200000;

std::string sourceSummary(const SourceAnalysis &Source, uint64_t Revision,
                          const SourceBindingAnalysis *Bindings = nullptr) {
  llvm::json::Array Diagnostics;
  for (const auto &D : Source.Diagnostics)
    Diagnostics.emplace_back(llvm::json::Object{
        {"code", D.Code},
        {"byte_offset",
         D.ByteOffset < 0 ? llvm::json::Value(nullptr)
                          : llvm::json::Value(std::to_string(D.ByteOffset))}});
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(Revision)},
      {"source_id", Source.ID},
      {"artifact_id", Source.ArtifactID},
      {"blob_sha256", Source.BlobHash},
      {"source_type", Source.SourceType},
      {"parse_status", Source.ParseStatus},
      {"parser_profile", std::string(JavaScriptParserProfile)},
      {"node_count", Source.Nodes.size()},
      {"comment_count", Source.CommentCount},
      {"lexeme_status", Source.LexemeStatus},
      {"lexeme_count", Source.Lexemes.size()},
      {"diagnostics", std::move(Diagnostics)},
      {"validation", "parser_checks"},
      {"semantic_analysis", Bindings ? "partial" : "not_analyzed"},
      {"lexical_binding_status", Bindings ? Bindings->Status : "not_analyzed"},
      {"redaction_policy", "metadata-only-v1"}});
}
} // namespace

Session::Session() : State(std::make_unique<Impl>()) {}
Session::~Session() = default;

std::string Session::capabilities() {
  llvm::json::Array Operations{
      "import_preview",      "import_commit",      "metadata",
      "artifacts",           "source_map_analyze", "source_map_sources",
      "source_map_segments", "bun_extract",        "bun_records",
      "bun_export",          "native_open",        "native_metadata",
      "native_analyze"};
  llvm::json::Array Analyses{
      llvm::json::Object{{"kind", "source_map_metadata"},
                         {"profile", std::string(SourceMapProfile)},
                         {"max_map_bytes", std::to_string(MaxSourceMapBytes)},
                         {"max_map_segments", MaxSourceMapSegments},
                         {"max_cached_maps", 4},
                         {"max_cached_map_segments", 200000},
                         {"resolves_external_references", false}}};
  Operations.emplace_back("electron_manifest_analyze");
  Operations.emplace_back("html_analyze");
  Operations.emplace_back("html_records");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "html_script_candidates"},
      {"profile", std::string(HTMLProfile)},
      {"max_bytes", std::to_string(MaxHTMLBytes)},
      {"max_records", MaxHTMLRecords},
      {"max_steps", MaxHTMLSteps},
      {"max_link_steps", MaxHTMLLinkSteps},
      {"max_cached", 4},
      {"record_kinds", llvm::json::Array{"scripts", "bases", "import_maps"}},
      {"tree_construction_verified", false},
      {"runtime_entries_verified", false},
      {"executes_input", false},
      {"resolves_external_references", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "html_import_map_candidates"},
      {"profile", std::string(ImportMapProfile)},
      {"url_parser", "ada-4.0.0-b12a893a45809da8103bb4f1e2f6f5ee13f9100b"},
      {"max_declarations", MaxHTMLImportMaps},
      {"max_total_body_bytes", MaxImportMapBytes},
      {"max_records", MaxImportMapRecords},
      {"max_url_bytes", MaxImportMapURLBytes},
      {"max_steps", MaxImportMapSteps},
      {"activation_verified", false},
      {"integrity_verified", false},
      {"executes_input", false},
      {"resolves_external_references", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_manifest"},
      {"profile", std::string(ElectronManifestProfile)},
      {"max_bytes", std::to_string(MaxElectronManifestBytes)},
      {"max_cached", 16},
      {"framework_verified", false},
      {"runtime_entry_verified", false}});
  if (asarAvailable()) {
    Operations.emplace_back("asar_extract");
    Operations.emplace_back("asar_records");
  }
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "asar_extraction"},
      {"profile", std::string(AsarProfile)},
      {"available", asarAvailable()},
      {"path_policy", std::string(ArchivePathProfile)},
      {"max_header_bytes", std::to_string(MaxAsarHeaderBytes)},
      {"max_members", MaxAsarMembers},
      {"max_depth", MaxAsarDepth},
      {"max_path_bytes", std::to_string(MaxAsarPathBytes)},
      {"max_payload_bytes", std::to_string(MaxAsarPayloadBytes)},
      {"max_cached_extractions", 4},
      {"follows_links", false},
      {"host_companion_discovery", false},
      {"authenticates_producer", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "native_handoff"},
      {"profile", std::string(NativeHandoffProfile)},
      {"max_input_bytes", std::to_string(MaxNativeInputBytes)},
      {"loads_automatically", false},
      {"executes_input", false},
      {"companion_file_discovery", false},
      {"resource_domain", "native_loader_and_pipeline"}});
  llvm::json::Array BunProfiles;
  for (const auto &P : bunProfiles())
    BunProfiles.emplace_back(P);
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "bun_standalone_extraction"},
      {"profile", std::string(BunProfile)},
      {"profiles", std::move(BunProfiles)},
      {"max_modules", MaxBunModules},
      {"max_builtins", MaxBunBuiltins},
      {"max_private_name_bytes", std::to_string(MaxBunNameBytes)},
      {"max_cached_extractions", 4},
      {"record_kinds", llvm::json::Array{"modules", "regions"}},
      {"executes_input", false},
      {"authenticates_producer_version", false},
      {"source_map_decoding",
       bunSourceMapAvailable() ? "available_on_request" : "zstd_unavailable"},
      {"bytecode_decoding", "opaque"}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "bun_local_export"},
      {"profile", "bun-local-evidence-export-v1"},
      {"destination", "new_private_directory"},
      {"filename_policy", "generated_ordinals"},
      {"raw_local_disclosure", true},
      {"executes_input", false},
      {"max_output_bytes", "1073741824"},
      {"max_files", 40000},
      {"max_decoded_source_bytes", std::to_string(MaxBunDecodedSourceBytes)},
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
      {"readable_source_available", true},
      {"readable_source_profile", std::string(JavaScriptRecoveryProfile)},
      {"max_recovery_parse_bytes", std::to_string(MaxRecoverySourceBytes)},
#else
      {"readable_source_available", false},
      {"readable_source_profile", nullptr},
      {"max_recovery_parse_bytes", nullptr},
#endif
      {"verification", "all_exported_files_reread_sha256_matches"}});
  if (bunSourceMapAvailable())
    Analyses.emplace_back(llvm::json::Object{
        {"kind", "bun_serialized_source_map"},
        {"profile", std::string(BunSourceMapProfile)},
        {"max_map_bytes", std::to_string(MaxSourceMapBytes)},
        {"max_sources", MaxBunMapSources},
        {"max_source_bytes", std::to_string(MaxBunMapSourceBytes)},
        {"max_decoded_bytes", std::to_string(MaxBunMapDecodedBytes)},
        {"max_segments", MaxSourceMapSegments},
        {"mapping_coverage", "retained_mapped_anchors_only"},
        {"resolves_external_references", false}});
#ifdef NEVERD_ENABLE_WEB_JAVASCRIPT
  Operations.emplace_back("source_analyze");
  Operations.emplace_back("source_nodes");
  Operations.emplace_back("source_map_lookup");
  Operations.emplace_back("source_bindings_analyze");
  Operations.emplace_back("source_binding_records");
  Operations.emplace_back("source_semantics_analyze");
  Operations.emplace_back("source_semantic_records");
  Operations.emplace_back("source_modules_analyze");
  Operations.emplace_back("source_module_records");
  Operations.emplace_back("source_bundles_analyze");
  Operations.emplace_back("source_bundle_records");
  Operations.emplace_back("source_view_preview");
  Operations.emplace_back("source_view_commit");
  Operations.emplace_back("source_view_records");
  Operations.emplace_back("source_view_chunk");
  Operations.emplace_back("source_navigation_analyze");
  Operations.emplace_back("electron_source_analyze");
  Operations.emplace_back("electron_source_records");
  Operations.emplace_back("electron_ipc_analyze");
  Operations.emplace_back("electron_ipc_records");
  Operations.emplace_back("electron_entries_analyze");
  Operations.emplace_back("electron_entry_records");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_entry_candidates"},
      {"profile", std::string(ElectronEntryProfile)},
      {"path_profile", std::string(CapturedPathProfile)},
      {"max_sources", MaxElectronIPCSources},
      {"max_records", MaxElectronRecords},
      {"max_steps", MaxElectronEntrySteps},
      {"max_path_units", MaxCapturedPathUnits},
      {"max_cached", 4},
      {"record_kinds", llvm::json::Array{"sources", "entries"}},
      {"runtime_entries_verified", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_scoped_channels"},
      {"profile", std::string(ElectronIPCProfile)},
      {"max_sources", MaxElectronIPCSources},
      {"max_records", MaxElectronIPCEndpoints},
      {"max_steps", MaxElectronIPCSteps},
      {"max_channel_units", MaxElectronIPCUnits},
      {"max_cached", 4},
      {"record_kinds", llvm::json::Array{"sources", "channels", "endpoints"}},
      {"runtime_routing_verified", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "electron_source_boundaries"},
      {"profile", std::string(ElectronSourceProfile)},
      {"origin_profile", std::string(JavaScriptOriginProfile)},
      {"max_records", MaxElectronRecords},
      {"max_steps", MaxElectronSteps},
      {"runtime_targets_verified", false}});
  Operations.emplace_back("source_navigation_records");
  Operations.emplace_back("source_anchor");
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_source_navigation"},
      {"profile", std::string(JavaScriptNavigationProfile)},
      {"anchor_profile", std::string(SourceAnchorProfile)},
      {"max_steps", MaxJavaScriptNavigationSteps},
      {"record_kinds", llvm::json::Array{"functions", "calls", "references"}},
      {"runtime_call_graph", false},
      {"resolves_runtime_values", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_source_view"},
      {"profile", std::string(SourceViewProfile)},
      {"lexeme_profile", std::string(JavaScriptLexemeProfile)},
      {"max_view_bytes", std::to_string(MaxSourceViewBytes)},
      {"max_view_segments", MaxSourceViewSegments},
      {"max_chunk_bytes", MaxSourceViewChunk},
      {"max_published_views", MaxPublishedSourceViews},
      {"max_pending_views", 1},
      {"max_reviewed_ranges", 64},
      {"default_policy", "structural-with-reviewed-ranges-v1"},
      {"semantic_rewrite", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_syntax_inventory"},
      {"profile", std::string(JavaScriptParserProfile)},
      {"source_types", llvm::json::Array{"script", "module", "commonjs"}},
      {"max_source_bytes", std::to_string(MaxJavaScriptBytes)},
      {"max_source_nodes", MaxJavaScriptNodes},
      {"max_source_lexemes", MaxJavaScriptLexemes},
      {"max_cached_lexemes", 400000},
      {"max_decoded_string_units", MaxJavaScriptStringUnits},
      {"max_cached_sources", MaxCachedSources},
      {"max_cached_nodes", MaxCachedNodes},
      {"executes_input", false},
      {"semantic_validation_complete", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_module_evidence"},
      {"profile", std::string(JavaScriptModuleProfile)},
      {"link_profile", std::string(JavaScriptModuleLinkProfile)},
      {"html_link_profile", std::string(HTMLModuleLinkProfile)},
      {"max_html_link_steps", MaxHTMLModuleLinkSteps},
      {"max_html_module_requests", MaxHTMLModuleRequests},
      {"max_steps", MaxJavaScriptModuleSteps},
      {"max_link_steps", MaxJavaScriptModuleSteps},
      {"max_private_name_units", MaxJavaScriptModuleNameUnits},
      {"max_relative_path_units", MaxJavaScriptModulePathUnits},
      {"max_retained_diagnostics", 32},
      {"record_kinds",
       llvm::json::Array{"requests", "imports", "exports", "attributes"}},
      {"resolves_runtime_modules", false},
      {"executes_input", false},
      {"semantic_validation_complete", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_bundle_partitions"},
      {"profile", std::string(JavaScriptBundleProfile)},
      {"max_steps", MaxJavaScriptBundleSteps},
      {"max_bundles", MaxJavaScriptBundles},
      {"max_modules", MaxJavaScriptBundleModules},
      {"max_dependencies", MaxJavaScriptBundleDependencies},
      {"max_private_key_units", MaxJavaScriptBundleNameUnits},
      {"record_kinds",
       llvm::json::Array{"bundles", "modules", "dependencies", "regions"}},
      {"executes_input", false},
      {"authenticates_producer", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_lexical_bindings"},
      {"profile", std::string(JavaScriptBindingProfile)},
      {"max_steps", MaxJavaScriptBindingSteps},
      {"record_kinds",
       llvm::json::Array{"scopes", "bindings", "declarations", "references"}},
      {"resolves_runtime_values", false},
      {"semantic_validation_complete", false}});
  Analyses.emplace_back(llvm::json::Object{
      {"kind", "javascript_primitive_values_and_effects"},
      {"value_profile", std::string(JavaScriptValueProfile)},
      {"effect_profile", std::string(JavaScriptEffectProfile)},
      {"max_value_steps", MaxJavaScriptValueSteps},
      {"max_effect_steps", MaxJavaScriptEffectSteps},
      {"max_value_string_units", MaxJavaScriptValueStringUnits},
      {"max_value_allocated_string_units", MaxJavaScriptValueAllocatedUnits},
      {"max_bigint_magnitude_bits", MaxJavaScriptBigIntBits},
      {"executes_input", false},
      {"resolves_runtime_bindings", false},
      {"authorizes_source_rewrites", false},
      {"semantic_validation_complete", false}});
#endif
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"identity_profile", "web-identity-v1"},
      {"redaction_policy", "metadata-only-v1"},
#ifdef _WIN32
      {"input_reader", "unavailable"},
      {"artifact_storage", "unavailable"},
#else
      {"input_reader", "posix-descriptor-v1"},
      {"artifact_storage", "posix-unlinked-spool-v1"},
#endif
      {"operations", std::move(Operations)},
      {"analysis", std::move(Analyses)},
      {"limits",
       llvm::json::Object{
           {"max_input_bytes", std::to_string(Limits::HardInputBytes)},
           {"max_member_bytes", std::to_string(Limits::HardMemberBytes)},
           {"max_entries", Limits::HardEntries},
           {"max_depth", Limits::HardDepth},
           {"max_blob_read_bytes", std::to_string(MaxBlobReadBytes)},
           {"capture_buffer_bytes", std::to_string(BlobTransferBytes)},
           {"max_session_spool_bytes",
            std::to_string(2 * Limits::HardInputBytes)},
           {"max_page_entries", 512}}}});
}

std::string Session::preview(std::string_view Path, std::string_view Options) {
  std::lock_guard Lock(State->Mutex);
  // A failed replacement invalidates the older approval token, but leaves
  // the published snapshot intact.
  State->Token.clear();
  State->CandidateID.clear();
  auto Budget = parseLimits(Options);
  auto Candidate = capture(Path, Budget);
  const auto Sequence = std::to_string(++State->PreviewSequence);
  const auto Config = limitsIdentity(Budget);
  const auto Token =
      identity("import-preview", {Candidate.ID, Config, Sequence});
  auto Reply = json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"preview_token", Token},
      {"snapshot_id", Candidate.ID},
      {"artifact_count", Candidate.Artifacts.size()},
      {"input_bytes", std::to_string(Candidate.InputBytes)},
      {"redaction_policy", "metadata-only-v1"},
      {"exposed_fields", llvm::json::Array{"identities", "hashes", "sizes",
                                           "kinds", "parent_id"}},
      {"source_text_exposed", false},
      {"input_names_exposed", false}});
  State->Budget = Budget;
  State->Path = Path;
  // Preview retains the digest only. Commit captures again and compares it;
  // retaining a second set of bytes here would spend disk without evidence.
  State->CandidateID = Candidate.ID;
  State->Token = Token;
  return Reply;
}

std::string Session::commit(std::string_view Token) {
  std::lock_guard Lock(State->Mutex);
  if (State->Token.empty() || State->Token != Token)
    throw Error("stale_preview");
  // Consume on any attempted commit, including an input change or read error.
  State->Token.clear();
  const auto CandidateID = std::move(State->CandidateID);
  State->CandidateID.clear();
  Snapshot Current = capture(State->Path, State->Budget);
  if (Current.ID != CandidateID) {
    throw Error("input_changed");
  }
  const auto Revision = State->Revision + 1;
  auto Reply = json(
      llvm::json::Object{{"schema_version", 1},
                         {"status", "ok"},
                         {"project_id", Current.ID},
                         {"revision", std::to_string(Revision)},
                         {"artifact_count", Current.Artifacts.size()},
                         {"input_bytes", std::to_string(Current.InputBytes)},
                         {"analysis_status", "not_analyzed"},
                         {"redaction_policy", "metadata-only-v1"}});
  State->Published = std::move(Current);
  State->Sources.clear();
  State->Bindings.clear();
  State->Navigation.clear();
  State->Semantics.clear();
  State->Modules.clear();
  State->Bundles.clear();
  State->BunExtractions.clear();
  State->AsarExtractions.clear();
  State->ElectronManifests.clear();
  State->ElectronSources.clear();
  State->ElectronIPCs.clear();
  State->ElectronEntryAnalyses.clear();
  State->HTMLDocuments.clear();
  State->CachedNodes = 0;
  State->CachedLexemes = 0;
  State->Maps.clear();
  State->CachedMapSegments = 0;
  State->SourceViews.clear();
  State->PendingSourceView.reset();
  State->SourceViewToken.clear();
  State->Revision = Revision;
  return Reply;
}

std::string Session::metadata() const {
  std::lock_guard Lock(State->Mutex);
  if (!State->Revision)
    throw Error("no_project");
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"project_id", State->Published.ID},
      {"revision", std::to_string(State->Revision)},
      {"artifact_count", State->Published.Artifacts.size()},
      {"input_bytes", std::to_string(State->Published.InputBytes)},
      {"inventory_complete", true},
      {"analysis_status", State->analysisStatus()},
      {"source_inventory_count", State->Sources.size()},
      {"source_binding_analysis_count", State->Bindings.size()},
      {"source_navigation_count", State->Navigation.size()},
      {"source_semantic_analysis_count", State->Semantics.size()},
      {"source_module_analysis_count", State->Modules.size()},
      {"source_bundle_analysis_count", State->Bundles.size()},
      {"bun_extraction_count", State->BunExtractions.size()},
      {"asar_extraction_count", State->AsarExtractions.size()},
      {"electron_manifest_count", State->ElectronManifests.size()},
      {"electron_source_count", State->ElectronSources.size()},
      {"electron_ipc_count", State->ElectronIPCs.size()},
      {"electron_entries_count", State->ElectronEntryAnalyses.size()},
      {"source_map_count", State->Maps.size()},
      {"source_view_count", State->SourceViews.size()},
      {"redaction_policy", "metadata-only-v1"}});
}

std::string Session::artifacts(std::string_view ExpectedRevision,
                               uint64_t Offset, uint64_t Limit) const {
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  if (!Limit || Limit > 512 || Offset > State->Published.Artifacts.size())
    throw Error("invalid_page");
  const auto End =
      std::min<uint64_t>(State->Published.Artifacts.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &A = State->Published.Artifacts[I];
    Items.emplace_back(llvm::json::Object{
        {"artifact_id", A.ID},
        {"parent_id", A.ParentID},
        {"blob_sha256", A.Directory ? llvm::json::Value(nullptr)
                                    : llvm::json::Value(A.BlobHash)},
        {"size", std::to_string(A.Content.size())},
        {"kind", A.Kind},
        {"name_redacted", true}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"project_id", State->Published.ID},
      {"revision", std::to_string(State->Revision)},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == State->Published.Artifacts.size()},
      {"next_offset", End == State->Published.Artifacts.size()
                          ? llvm::json::Value(nullptr)
                          : llvm::json::Value(End)},
      {"inventory_complete", true},
      {"analysis_status", State->analysisStatus()},
      {"redaction_policy", "metadata-only-v1"}});
}

std::string Session::analyzeSource(std::string_view ExpectedRevision,
                                   std::string_view ArtifactID,
                                   std::string_view SourceType) {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  if (SourceType != "script" && SourceType != "module" &&
      SourceType != "commonjs")
    throw Error("unsupported_source_type");
  for (const auto &[ID, Source] : State->Sources)
    if (Source.ArtifactID == ArtifactID && Source.SourceType == SourceType) {
      const auto Bindings = State->Bindings.find(ID);
      return sourceSummary(
          Source, State->Revision,
          Bindings == State->Bindings.end() ? nullptr : &Bindings->second);
    }
  const auto Bytes = State->sourceBytes(ArtifactID, MaxJavaScriptBytes,
                                        "source_byte_budget_exceeded");
  // Reserve room for a complete maximum-size result before calling a parser.
  // Concurrent jobs and repeated queries cannot escape the session budget.
  if (State->Sources.size() >= MaxCachedSources ||
      State->CachedNodes > MaxCachedNodes - MaxJavaScriptNodes ||
      State->CachedLexemes > 400000 - MaxJavaScriptLexemes)
    throw Error("source_cache_budget_exceeded");
  auto Source = inspectJavaScript(ArtifactID, Bytes, SourceType);
  auto Reply = sourceSummary(Source, State->Revision);
  const auto Count = Source.Nodes.size();
  const auto Lexemes = Source.Lexemes.size();
  const auto ID = Source.ID;
  State->Sources.emplace(ID, std::move(Source));
  State->CachedNodes += Count;
  State->CachedLexemes += Lexemes;
  return Reply;
#endif
}

std::string Session::sourceNodes(std::string_view ExpectedRevision,
                                 std::string_view SourceID, uint64_t Offset,
                                 uint64_t Limit) const {
#ifndef NEVERD_ENABLE_WEB_JAVASCRIPT
  throw Error("capability_unavailable");
#else
  std::lock_guard Lock(State->Mutex);
  State->requireRevision(ExpectedRevision);
  const auto Found = State->Sources.find(std::string(SourceID));
  if (Found == State->Sources.end())
    throw Error("unknown_source");
  const auto &Source = Found->second;
  if (!Limit || Limit > 512 || Offset > Source.Nodes.size())
    throw Error("invalid_page");
  const auto End = std::min<uint64_t>(Source.Nodes.size(), Offset + Limit);
  llvm::json::Array Items;
  for (auto I = Offset; I < End; ++I) {
    const auto &Node = Source.Nodes[I];
    Items.emplace_back(
        llvm::json::Object{{"node_id", Node.ID},
                           {"parent_id", Node.ParentID},
                           {"kind", Node.Kind},
                           {"start_byte", std::to_string(Node.Start)},
                           {"end_byte", std::to_string(Node.End)}});
  }
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "ok"},
      {"revision", std::to_string(State->Revision)},
      {"source_id", Source.ID},
      {"items", std::move(Items)},
      {"offset", Offset},
      {"page_complete", End == Source.Nodes.size()},
      {"next_offset", End == Source.Nodes.size() ? llvm::json::Value(nullptr)
                                                 : llvm::json::Value(End)},
      {"parse_status", Source.ParseStatus},
      {"semantic_analysis",
       State->Bindings.count(Source.ID) ? "partial" : "not_analyzed"},
      {"redaction_policy", "metadata-only-v1"}});
#endif
}

} // namespace neverd::web
