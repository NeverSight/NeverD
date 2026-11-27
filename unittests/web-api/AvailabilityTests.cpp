//===- AvailabilityTests.cpp - Offline analysis availability -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compiled and omitted offline analysis API contracts.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIWeb.h"

#include "llvm/Support/JSON.h"

#include <algorithm>

namespace {
llvm::json::Value take(const char *Owned) {
  EXPECT_NE(Owned, nullptr);
  if (!Owned)
    return nullptr;
  auto Result = llvm::json::parse(Owned);
  neverd_free_string(Owned);
  EXPECT_TRUE(bool(Result));
  if (!Result) {
    llvm::consumeError(Result.takeError());
    return nullptr;
  }
  return std::move(*Result);
}

TEST(WebAvailability, CapabilitiesReflectCompiledBackendAndParser) {
  const auto Result = take(neverd_web_capabilities_json());
  const auto *Object = Result.getAsObject();
  ASSERT_NE(Object, nullptr);
#if NEVERD_TEST_WEB
  EXPECT_EQ(Object->getString("status"), "ok");
  const auto *Operations = Object->getArray("operations");
  ASSERT_NE(Operations, nullptr);
  const auto Has = [&](llvm::StringRef Name) {
    return std::any_of(
        Operations->begin(), Operations->end(),
        [&](const auto &Op) { return Op.getAsString() == Name; });
  };
  EXPECT_TRUE(Has("import_preview"));
  EXPECT_TRUE(Has("source_map_analyze"));
  EXPECT_TRUE(Has("bun_extract"));
  EXPECT_TRUE(Has("sea_extract"));
  EXPECT_TRUE(Has("sea_records"));
  EXPECT_TRUE(Has("desktop_manifest_analyze"));
  EXPECT_TRUE(Has("bun_records"));
  EXPECT_TRUE(Has("bun_export"));
  EXPECT_TRUE(Has("packages_analyze"));
  EXPECT_TRUE(Has("package_records"));
  EXPECT_TRUE(Has("packages_compare"));
  EXPECT_TRUE(Has("package_diff_records"));
  EXPECT_TRUE(Has("package_archive_extract"));
  EXPECT_TRUE(Has("package_archive_records"));
  EXPECT_TRUE(Has("package_integrity_verify"));
  for (const auto *Name :
       {"package_advisory_import", "package_advisory_records",
        "package_advisories_match", "package_advisories_match_records",
        "package_attestation_import", "package_attestation_records",
        "package_attestation_digest_match", "package_attestation_digest_result",
        "package_assessment_create", "package_assessment_findings",
        "package_review_append", "package_review_decisions",
        "package_review_restore", "package_assessment_report"})
    EXPECT_TRUE(Has(Name));
  for (const auto Op : {"har_preview", "har_commit", "har_records",
                        "stream_preview", "stream_commit", "stream_records",
                        "passive_report_create", "passive_report_records"})
    EXPECT_TRUE(Has(Op));
  for (const auto Op :
       {"interfaces_analyze", "interface_records", "interfaces_compare",
        "interface_correlation_records", "interface_relations_analyze",
        "interface_relation_records"})
    EXPECT_EQ(Has(Op), bool(NEVERD_TEST_WEB_JS));
  bool ASARAvailable = false;
  const auto *Analysis = Object->getArray("analysis");
  ASSERT_NE(Analysis, nullptr);
  for (const auto &A : *Analysis)
    if (const auto *Entry = A.getAsObject())
      if (Entry->getString("kind") == "asar_extraction")
        ASARAvailable = Entry->getBoolean("available").value_or(false);
  bool SourceInterfacesAvailable = false;
  EXPECT_EQ(Has("nexe_extract"), ASARAvailable);
  EXPECT_EQ(Has("nexe_records"), ASARAvailable);
  EXPECT_EQ(Has("pkg_extract"), ASARAvailable);
  EXPECT_EQ(Has("pkg_records"), ASARAvailable);
  EXPECT_TRUE(Has("datapack_extract"));
  EXPECT_TRUE(Has("datapack_records"));
  EXPECT_EQ(Has("wails_extract"), ASARAvailable);
  EXPECT_EQ(Has("wails_records"), ASARAvailable);
  EXPECT_EQ(Has("wails_export"), ASARAvailable);
  EXPECT_EQ(Has("source_readable_export"), bool(NEVERD_TEST_WEB_JS));
  for (const auto &A : *Analysis) {
    const auto *Entry = A.getAsObject();
    if (!Entry || Entry->getString("kind") != "source_interfaces")
      continue;
    SourceInterfacesAvailable = true;
    EXPECT_EQ(Entry->getString("profile"),
              "fetch-websocket-xhr-wrappers-syntax-v4");
    EXPECT_FALSE(Entry->getBoolean("xhr_correlation").value_or(true));
    const auto *Kinds = Entry->getArray("candidate_kinds");
    ASSERT_NE(Kinds, nullptr);
    EXPECT_EQ(Kinds->size(), 8u);
    EXPECT_TRUE(std::any_of(Kinds->begin(), Kinds->end(), [](const auto &Kind) {
      return Kind.getAsString() == "fetch_member_call";
    }));
    EXPECT_EQ(Entry->getString("fetch_member_identity"),
              "property_spelling_unverified");
    EXPECT_FALSE(Entry->getBoolean("fetch_member_correlation").value_or(true));
    EXPECT_EQ(Entry->getString("wrapper_profile"),
              "direct-lexical-fetch-forwarding-v1");
    EXPECT_FALSE(Entry->getBoolean("wrapper_correlation").value_or(true));
    EXPECT_TRUE(std::any_of(Kinds->begin(), Kinds->end(), [](const auto &Kind) {
      return Kind.getAsString() == "fetch_wrapper_call";
    }));
  }
  EXPECT_EQ(SourceInterfacesAvailable, bool(NEVERD_TEST_WEB_JS));
  unsigned RelationProfiles = 0;
  for (const auto &A : *Analysis) {
    const auto *Entry = A.getAsObject();
    if (!Entry || Entry->getString("kind") != "source_interface_relations")
      continue;
    ++RelationProfiles;
    EXPECT_EQ(Entry->getString("profile"), "request-source-relations-v2");
    EXPECT_EQ(Entry->getString("response_api_identity"),
              "member_spelling_unverified");
    EXPECT_EQ(Entry->getString("callback_parameter_roles"), "not_assigned");
    EXPECT_FALSE(Entry->getBoolean("har_correlation").value_or(true));
  }
  EXPECT_EQ(RelationProfiles, NEVERD_TEST_WEB_JS ? 1u : 0u);
  unsigned PassiveProfiles = 0;
  for (const auto &A : *Analysis) {
    const auto *Entry = A.getAsObject();
    if (!Entry || Entry->getString("kind") != "passive_report")
      continue;
    ++PassiveProfiles;
    EXPECT_EQ(Entry->getString("profile"), "passive-evidence-shape-report-v2");
    EXPECT_EQ(Entry->getInteger("max_shape_private_bytes"), 2 * 1024 * 1024);
    EXPECT_EQ(Entry->getString("all_input_values"), "withheld");
    EXPECT_EQ(Entry->getBoolean("executes_input"), false);
    EXPECT_EQ(Entry->getBoolean("network_access"), false);
  }
  EXPECT_EQ(PassiveProfiles, 1u);
  unsigned RenameProfiles = 0;
  for (const auto &A : *Analysis) {
    const auto *Entry = A.getAsObject();
    if (!Entry || Entry->getString("kind") != "javascript_scope_rename")
      continue;
    ++RenameProfiles;
    EXPECT_EQ(Entry->getString("profile"), "javascript-scope-rename-v1");
    const auto *Passes = Entry->getArray("passes");
    ASSERT_NE(Passes, nullptr);
    ASSERT_EQ(Passes->size(), 1U);
    EXPECT_EQ((*Passes)[0].getAsString(), "scope_renaming");
    EXPECT_EQ(Entry->getInteger("max_cached"), 2);
    EXPECT_EQ(Entry->getString("cache_pool"), "source_rewrites");
    EXPECT_EQ(Entry->getBoolean("original_name_recovery"), false);
    EXPECT_EQ(Entry->getBoolean("whole_program_equivalence"), false);
    EXPECT_EQ(Entry->getBoolean("executes_input"), false);
  }
  EXPECT_EQ(RenameProfiles, NEVERD_TEST_WEB_JS ? 1U : 0U);
  unsigned BranchProfiles = 0;
  for (const auto &A : *Analysis) {
    const auto *Entry = A.getAsObject();
    if (!Entry || Entry->getString("kind") != "javascript_boolean_branches")
      continue;
    ++BranchProfiles;
    EXPECT_EQ(Entry->getString("profile"), "javascript-boolean-branches-v1");
    const auto *Passes = Entry->getArray("passes");
    ASSERT_NE(Passes, nullptr);
    ASSERT_EQ(Passes->size(), 1U);
    EXPECT_EQ((*Passes)[0].getAsString(), "boolean_branches");
    EXPECT_EQ(Entry->getInteger("max_cached"), 2);
    EXPECT_EQ(Entry->getString("cache_pool"), "source_rewrites");
    EXPECT_EQ(Entry->getBoolean("statements_rewritten"), false);
    EXPECT_EQ(Entry->getBoolean("whole_program_equivalence"), false);
    EXPECT_EQ(Entry->getBoolean("executes_input"), false);
  }
  EXPECT_EQ(BranchProfiles, NEVERD_TEST_WEB_JS ? 1U : 0U);
  unsigned NavigationProfiles = 0;
  for (const auto &A : *Analysis) {
    const auto *Entry = A.getAsObject();
    if (!Entry || Entry->getString("kind") != "javascript_source_navigation")
      continue;
    ++NavigationProfiles;
    EXPECT_EQ(Entry->getString("configuration_files_profile"),
              "javascript-configuration-file-navigation-v1");
    EXPECT_EQ(Entry->getInteger("max_configuration_file_records"), 1024);
    EXPECT_EQ(Entry->getInteger("max_configuration_file_terms"), 4096);
    EXPECT_EQ(Entry->getBoolean("resolves_runtime_values"), false);
    const auto *Kinds = Entry->getArray("record_kinds");
    ASSERT_NE(Kinds, nullptr);
    EXPECT_NE(std::find_if(Kinds->begin(), Kinds->end(),
                           [](const auto &Value) {
                             return Value.getAsString() == "config_files";
                           }),
              Kinds->end());
  }
  EXPECT_EQ(NavigationProfiles, NEVERD_TEST_WEB_JS ? 1U : 0U);
  EXPECT_EQ(Has("asar_extract"), ASARAvailable);
  EXPECT_EQ(Has("asar_records"), ASARAvailable);
  EXPECT_TRUE(Has("native_open"));
  EXPECT_TRUE(Has("electron_manifest_analyze"));
  EXPECT_TRUE(Has("html_analyze"));
  EXPECT_TRUE(Has("html_records"));
  EXPECT_EQ(Has("electron_source_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("electron_source_records"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("electron_ipc_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("electron_ipc_records"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_TRUE(Has("native_metadata"));
  EXPECT_TRUE(Has("native_analyze"));
  EXPECT_EQ(Has("source_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_analyze_large"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_map_lookup"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_bindings_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_binding_records"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_semantics_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_semantic_records"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_rewrite_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_rewrite_records"), bool(NEVERD_TEST_WEB_JS));
  for (const auto Op :
       {"node_behavior_analyze", "node_behavior_records",
        "package_behavior_analyze", "package_behavior_records",
        "package_install_analyze", "package_install_records",
        "package_behavior_diff_analyze", "package_behavior_diff_records"})
    EXPECT_EQ(Has(Op), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_rewrite_export"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_modules_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_module_records"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_bundles_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_bundle_records"), bool(NEVERD_TEST_WEB_JS));
  for (const auto Op : {"source_navigation_analyze",
                        "source_navigation_records", "source_anchor"})
    EXPECT_EQ(Has(Op), bool(NEVERD_TEST_WEB_JS));
  for (const auto Op : {"source_view_preview", "source_view_commit",
                        "source_view_records", "source_view_chunk"})
    EXPECT_EQ(Has(Op), bool(NEVERD_TEST_WEB_JS));
  const auto Session = neverd_web_session_create();
  ASSERT_NE(Session, nullptr);
#if !NEVERD_TEST_WEB_JS
  for (const auto *Owned :
       {neverd_web_source_rewrite_analyze_json(Session, "1", 1, "source", 6,
                                               "constant_folding", 16),
        neverd_web_source_rewrite_analyze_json(Session, "1", 1, "source", 6,
                                               "scope_renaming", 14),
        neverd_web_source_rewrite_analyze_json(Session, "1", 1, "source", 6,
                                               "boolean_branches", 16),
        neverd_web_source_rewrite_records_json(Session, "1", 1, "rewrite", 7, 0,
                                               1),
        neverd_web_source_rewrite_export_json(Session, "1", 1, "rewrite", 7,
                                              "receipt", 7, "output", 6),
        neverd_web_node_behavior_analyze_json(Session, "1", 1, "source", 6),
        neverd_web_node_behavior_records_json(Session, "1", 1, "behavior", 8, 0,
                                              1),
        neverd_web_package_behavior_analyze_json(Session, "1", 1, "package", 7,
                                                 "{}", 2),
        neverd_web_package_behavior_records_json(Session, "1", 1, "behavior", 8,
                                                 "sources", 7, 0, 1),
        neverd_web_package_install_analyze_json(Session, "1", 1, "package", 7,
                                                "{}", 2),
        neverd_web_package_behavior_diff_analyze_json(Session, "1", 1, "diff",
                                                      4, "{}", 2),
        neverd_web_package_behavior_diff_records_json(Session, "1", 1, "diff",
                                                      4, "pairs", 5, 0, 1),
        neverd_web_package_install_records_json(Session, "1", 1, "install", 7,
                                                "scripts", 7, 0, 1),
        neverd_web_interfaces_analyze_json(Session, "1", 1, "source", 6),
        neverd_web_interface_records_json(Session, "1", 1, "analysis", 8, 0, 1),
        neverd_web_interface_relations_analyze_json(Session, "1", 1, "analysis",
                                                    8),
        neverd_web_interface_relation_records_json(Session, "1", 1, "relations",
                                                   9, 0, 1),
        neverd_web_interfaces_compare_json(Session, "1", 1, "analysis", 8,
                                           "capture", 7),
        neverd_web_interface_correlation_records_json(
            Session, "1", 1, "correlation", 11, 0, 1)}) {
    const auto R = take(Owned);
    ASSERT_NE(R.getAsObject(), nullptr);
    ASSERT_NE(R.getAsObject()->getObject("error"), nullptr);
    EXPECT_EQ(R.getAsObject()->getObject("error")->getString("code"),
              "capability_unavailable");
  }
#endif
  neverd_web_session_destroy(Session);
#else
  EXPECT_EQ(Object->getString("status"), "error");
  ASSERT_NE(Object->getObject("error"), nullptr);
  EXPECT_EQ(Object->getObject("error")->getString("code"),
            "capability_unavailable");
  EXPECT_EQ(neverd_web_session_create(), nullptr);
#endif
  neverd_web_session_destroy(nullptr);
}

TEST(WebAvailability, AllPublicEntryPointsRemainAvailableWhenBackendIsOmitted) {
#if NEVERD_TEST_WEB
  GTEST_SKIP()
      << "Unavailable C ABI checks require NEVERD_ENABLE_WEB_ANALYSIS=OFF";
#else
  neverd_session_t NativeOutput = reinterpret_cast<void *>(uintptr_t(1));
  for (const auto *Owned :
       {neverd_web_metadata_json(nullptr),
        neverd_web_desktop_manifest_analyze_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0),
        neverd_web_sea_extract_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                    0),
        neverd_web_sea_records_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_nexe_extract_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                     0),
        neverd_web_nexe_records_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                     0, 0, 1),
        neverd_web_pkg_extract_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                    0),
        neverd_web_datapack_extract_json(nullptr, nullptr, 0, nullptr, 0,
                                         nullptr, 0),
        neverd_web_datapack_records_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_datapack_decode_json(nullptr, nullptr, 0, nullptr, 0,
                                        nullptr, 0),
        neverd_web_datapack_export_json(nullptr, nullptr, 0, nullptr, 0,
                                        nullptr, 0),
        neverd_web_pkg_records_json(nullptr, nullptr, 0, nullptr, 0, nullptr, 0,
                                    0, 1),
        neverd_web_wails_extract_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                      0),
        neverd_web_wails_records_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                      0, 0, 1),
        neverd_web_wails_export_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                     0),
        neverd_web_source_readable_export_json(nullptr, nullptr, 0, nullptr, 0,
                                               nullptr, 0, nullptr, 0),
        neverd_web_stream_preview_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                       0),
        neverd_web_stream_commit_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_stream_records_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_har_preview_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_har_commit_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_har_records_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_interfaces_analyze_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_interface_records_json(nullptr, nullptr, 0, nullptr, 0, 0,
                                          1),
        neverd_web_interface_relations_analyze_json(nullptr, nullptr, 0,
                                                    nullptr, 0),
        neverd_web_interface_relation_records_json(nullptr, nullptr, 0, nullptr,
                                                   0, 0, 1),
        neverd_web_interfaces_compare_json(nullptr, nullptr, 0, nullptr, 0,
                                           nullptr, 0),
        neverd_web_interface_correlation_records_json(nullptr, nullptr, 0,
                                                      nullptr, 0, 0, 1),
        neverd_web_package_advisory_import_json(nullptr, nullptr, 0, nullptr, 0,
                                                nullptr, 0),
        neverd_web_package_advisory_records_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0, 0, 128),
        neverd_web_package_advisories_match_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0),
        neverd_web_package_advisories_match_records_json(nullptr, nullptr, 0,
                                                         nullptr, 0, 0, 128),
        neverd_web_package_attestation_import_json(nullptr, nullptr, 0, nullptr,
                                                   0, nullptr, 0, nullptr, 0),
        neverd_web_package_attestation_records_json(
            nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, 128),
        neverd_web_package_attestation_digest_match_json(
            nullptr, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0),
        neverd_web_package_attestation_digest_result_json(nullptr, nullptr, 0,
                                                          nullptr, 0),
        neverd_web_package_assessment_create_json(nullptr, nullptr, 0, nullptr,
                                                  0, nullptr, 0),
        neverd_web_package_assessment_findings_json(
            nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_package_review_append_json(nullptr, nullptr, 0, nullptr, 0,
                                              nullptr, 0, nullptr, 0, nullptr,
                                              0),
        neverd_web_package_review_decisions_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0, 0, 1),
        neverd_web_package_review_restore_json(nullptr, nullptr, 0, nullptr, 0,
                                               nullptr, 0, nullptr, 0),
        neverd_web_package_assessment_report_json(nullptr, nullptr, 0, nullptr,
                                                  0, nullptr, 0, 0, 1),
        neverd_web_package_integrity_verify_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0, nullptr, 0),
        neverd_web_package_archive_extract_json(nullptr, nullptr, 0, nullptr, 0,
                                                nullptr, 0),
        neverd_web_package_archive_records_json(nullptr, nullptr, 0, nullptr, 0,
                                                0, 1),
        neverd_web_packages_analyze_json(nullptr, nullptr, 0, nullptr, 0,
                                         nullptr, 0),
        neverd_web_package_records_json(nullptr, nullptr, 0, nullptr, 0,
                                        nullptr, 0, 0, 1),
        neverd_web_packages_compare_json(nullptr, nullptr, 0, nullptr, 0,
                                         nullptr, 0),
        neverd_web_package_diff_records_json(nullptr, nullptr, 0, nullptr, 0, 0,
                                             1),
        neverd_web_node_behavior_analyze_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_node_behavior_records_json(nullptr, nullptr, 0, nullptr, 0,
                                              0, 1),
        neverd_web_package_behavior_analyze_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0),
        neverd_web_package_behavior_records_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0, 0, 1),
        neverd_web_package_install_analyze_json(nullptr, nullptr, 0, nullptr, 0,
                                                nullptr, 0),
        neverd_web_package_behavior_diff_analyze_json(nullptr, nullptr, 0,
                                                      nullptr, 0, nullptr, 0),
        neverd_web_package_behavior_diff_records_json(
            nullptr, nullptr, 0, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_package_install_records_json(nullptr, nullptr, 0, nullptr, 0,
                                                nullptr, 0, 0, 1),
        neverd_web_electron_manifest_analyze_json(nullptr, nullptr, 0, nullptr,
                                                  0),
        neverd_web_electron_source_analyze_json(nullptr, nullptr, 0, nullptr,
                                                0),
        neverd_web_electron_source_records_json(nullptr, nullptr, 0, nullptr, 0,
                                                0, 1),
        neverd_web_electron_ipc_analyze_json(nullptr, nullptr, 0, nullptr, 0,
                                             nullptr, 0),
        neverd_web_electron_ipc_records_json(nullptr, nullptr, 0, nullptr, 0,
                                             nullptr, 0, 0, 1),
        neverd_web_electron_entries_analyze_json(nullptr, nullptr, 0, nullptr,
                                                 0, nullptr, 0),
        neverd_web_electron_entry_records_json(nullptr, nullptr, 0, nullptr, 0,
                                               nullptr, 0, 0, 1),
        neverd_web_html_analyze_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_html_records_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                     0, 0, 1),
        neverd_web_asar_extract_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                     0),
        neverd_web_asar_records_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_native_open_json(nullptr, nullptr, 0, nullptr, 0,
                                    &NativeOutput),
        neverd_web_native_metadata_json(nullptr),
        neverd_web_native_analyze_json(nullptr),
        neverd_web_source_navigation_analyze_json(nullptr, nullptr, 0, nullptr,
                                                  0),
        neverd_web_source_navigation_records_json(nullptr, nullptr, 0, nullptr,
                                                  0, nullptr, 0, 0, 1),
        neverd_web_source_anchor_json(nullptr, nullptr, 0, nullptr, 0, 0, 1,
                                      nullptr, 0),
        neverd_web_source_view_preview_json(nullptr, nullptr, 0, nullptr, 0,
                                            nullptr, 0),
        neverd_web_source_view_commit_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_source_view_records_json(nullptr, nullptr, 0, nullptr, 0, 0,
                                            1),
        neverd_web_source_view_chunk_json(nullptr, nullptr, 0, nullptr, 0, 0,
                                          1),
        neverd_web_bun_extract_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_bun_export_json(nullptr, nullptr, 0, nullptr, 0, nullptr, 0),
        neverd_web_bun_records_json(nullptr, nullptr, 0, nullptr, 0, nullptr, 0,
                                    0, 1),
        neverd_web_import_preview_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_import_commit_json(nullptr, nullptr, 0),
        neverd_web_artifacts_json(nullptr, nullptr, 0, 0, 1),
        neverd_web_source_analyze_json(nullptr, nullptr, 0, nullptr, 0, nullptr,
                                       0),
        neverd_web_source_analyze_large_json(nullptr, nullptr, 0, nullptr, 0,
                                             nullptr, 0),
        neverd_web_source_nodes_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_source_bindings_analyze_json(nullptr, nullptr, 0, nullptr,
                                                0),
        neverd_web_source_binding_records_json(nullptr, nullptr, 0, nullptr, 0,
                                               nullptr, 0, 0, 1),
        neverd_web_source_semantics_analyze_json(nullptr, nullptr, 0, nullptr,
                                                 0),
        neverd_web_source_semantic_records_json(nullptr, nullptr, 0, nullptr, 0,
                                                0, 1),
        neverd_web_source_rewrite_analyze_json(nullptr, nullptr, 0, nullptr, 0,
                                               nullptr, 0),
        neverd_web_source_rewrite_analyze_json(nullptr, nullptr, 0, nullptr, 0,
                                               "scope_renaming", 14),
        neverd_web_source_rewrite_analyze_json(nullptr, nullptr, 0, nullptr, 0,
                                               "boolean_branches", 16),
        neverd_web_source_rewrite_records_json(nullptr, nullptr, 0, nullptr, 0,
                                               0, 1),
        neverd_web_source_rewrite_export_json(nullptr, nullptr, 0, nullptr, 0,
                                              nullptr, 0, nullptr, 0),
        neverd_web_source_modules_analyze_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_source_bundles_analyze_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_source_bundle_records_json(nullptr, nullptr, 0, nullptr, 0,
                                              nullptr, 0, 0, 1),
        neverd_web_source_module_records_json(nullptr, nullptr, 0, nullptr, 0,
                                              nullptr, 0, 0, 1),
        neverd_web_source_map_analyze_json(nullptr, nullptr, 0, nullptr, 0),
        neverd_web_source_map_sources_json(nullptr, nullptr, 0, nullptr, 0, 0,
                                           1),
        neverd_web_source_map_segments_json(nullptr, nullptr, 0, nullptr, 0, 0,
                                            1),
        neverd_web_source_map_lookup_json(nullptr, nullptr, 0, nullptr, 0,
                                          nullptr, 0, 0)}) {
    const auto Result = take(Owned);
    const auto *Object = Result.getAsObject();
    ASSERT_NE(Object, nullptr);
    EXPECT_EQ(Object->getString("status"), "error");
    ASSERT_NE(Object->getObject("error"), nullptr);
    EXPECT_EQ(Object->getObject("error")->getString("code"),
              "capability_unavailable");
  }
  EXPECT_EQ(NativeOutput, nullptr);
#endif
}
} // namespace
