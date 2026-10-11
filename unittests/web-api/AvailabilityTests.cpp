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
  for (const auto Op : {"har_preview", "har_commit", "har_records",
                        "stream_preview", "stream_commit", "stream_records"})
    EXPECT_TRUE(Has(Op));
  for (const auto Op : {"interfaces_analyze", "interface_records",
                        "interfaces_compare", "interface_correlation_records"})
    EXPECT_EQ(Has(Op), bool(NEVERD_TEST_WEB_JS));
  bool ASARAvailable = false;
  const auto *Analysis = Object->getArray("analysis");
  ASSERT_NE(Analysis, nullptr);
  for (const auto &A : *Analysis)
    if (const auto *Entry = A.getAsObject())
      if (Entry->getString("kind") == "asar_extraction")
        ASARAvailable = Entry->getBoolean("available").value_or(false);
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
  EXPECT_EQ(Has("source_map_lookup"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_bindings_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_binding_records"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_semantics_analyze"), bool(NEVERD_TEST_WEB_JS));
  EXPECT_EQ(Has("source_semantic_records"), bool(NEVERD_TEST_WEB_JS));
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
       {neverd_web_interfaces_analyze_json(Session, "1", 1, "source", 6),
        neverd_web_interface_records_json(Session, "1", 1, "analysis", 8, 0, 1),
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
        neverd_web_interfaces_compare_json(nullptr, nullptr, 0, nullptr, 0,
                                           nullptr, 0),
        neverd_web_interface_correlation_records_json(nullptr, nullptr, 0,
                                                      nullptr, 0, 0, 1),
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
        neverd_web_source_nodes_json(nullptr, nullptr, 0, nullptr, 0, 0, 1),
        neverd_web_source_bindings_analyze_json(nullptr, nullptr, 0, nullptr,
                                                0),
        neverd_web_source_binding_records_json(nullptr, nullptr, 0, nullptr, 0,
                                               nullptr, 0, 0, 1),
        neverd_web_source_semantics_analyze_json(nullptr, nullptr, 0, nullptr,
                                                 0),
        neverd_web_source_semantic_records_json(nullptr, nullptr, 0, nullptr, 0,
                                                0, 1),
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
