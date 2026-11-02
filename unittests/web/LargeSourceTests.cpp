//===- LargeSourceTests.cpp - Explicit large source evidence coverage ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Large syntax inventories, unchanged admission and retained real sources.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "RecoveryParser.h"
#include "gtest/gtest.h"

#include "neverd/web/PassiveReport.h"
#include "neverd/web/SourceLimits.h"
#include "neverd/web/SourceModules.h"

#include <array>
#include <cstdlib>
#include <filesystem>

namespace {
using namespace neverd::web;

TEST(WebLargeSource, ProfileIdentityPreservesSyntaxAndInteractiveAdmission) {
  const std::string Text = "const value=1; fetch(`/items/${value}`);";
  const auto Small = inspectJavaScript("a", Text, "module");
  const auto Large = inspectLargeJavaScript("a", Text, "module");
  ASSERT_EQ(Small.ParseStatus, "parsed");
  ASSERT_EQ(Large.ParseStatus, "parsed");
  EXPECT_NE(Small.ID, Large.ID);
  ASSERT_EQ(Small.Nodes.size(), Large.Nodes.size());
  for (size_t I = 0; I < Small.Nodes.size(); ++I) {
    EXPECT_EQ(Small.Nodes[I].Kind, Large.Nodes[I].Kind);
    EXPECT_EQ(Small.Nodes[I].Start, Large.Nodes[I].Start);
    EXPECT_EQ(Small.Nodes[I].End, Large.Nodes[I].End);
    EXPECT_NE(Small.Nodes[I].ID, Large.Nodes[I].ID);
  }
  EXPECT_EQ(Large.ID, inspectLargeJavaScript("a", Text, "module").ID);
  EXPECT_THROW(inspectJavaScript("a", std::string(MaxJavaScriptBytes + 1, ' '),
                                 "module"),
               Error);
  const auto Recovery = inspectRecoveryJavaScript("a", Text, "module");
  EXPECT_THROW(analyzeSourceBindings(Recovery), Error);
  auto Forged = Large;
  Forged.Nodes[0].End = sourceLimits(Forged).Bytes + 1;
  EXPECT_THROW(analyzeSourceBindings(Forged), Error);
}

TEST(WebLargeSource, CompleteLexicalInventoryRetainsLateRequestAndFunction) {
  std::string Text = "/*" + std::string(MaxJavaScriptBytes, ' ') + "*/";
  for (unsigned I = 0; I < 55000; ++I)
    Text += "foreignValue;";
  const auto At = Text.size();
  Text += "function request(id){return fetch(`/items/${id}`)}";
  const auto S = inspectLargeJavaScript("a", Text, "module");
  ASSERT_EQ(S.ParseStatus, "parsed");
  ASSERT_EQ(S.LexemeStatus, "ok");
  EXPECT_GT(S.Nodes.size(), MaxJavaScriptNodes);
  const auto B = analyzeSourceBindings(S);
  ASSERT_EQ(B.Status, "ok");
  const auto N = analyzeSourceNavigation(S, B);
  ASSERT_EQ(N.Status, "ok");
  ASSERT_EQ(N.Functions.size(), 1u);
  EXPECT_EQ(S.Nodes[N.Functions[0].Node].Start, At);
  EXPECT_EQ(B.References.size(), 55002u);
  const auto F = analyzeClientSourceFeatures(S, B, N);
  ASSERT_EQ(F.Status, "partial");
  const auto I = analyzeSourceInterfaces(S, B, analyzeSourceValues(S));
  ASSERT_EQ(I.Status, "partial");
  ASSERT_EQ(I.Records.size(), 1u);
  const auto R = analyzeInterfaceSourceRelations(S, B, I, N);
  ASSERT_EQ(R.Status, "partial");
  const auto P = projectPassiveSource(S, B, N, F, I, R);
  ASSERT_FALSE(P.Records.empty());
  for (const auto &Row : P.Records) {
    EXPECT_EQ(Row.SourceID, S.ID);
    EXPECT_EQ(Row.EvidenceClass, "static_inference");
    EXPECT_LE(Row.Offset + Row.Length, Text.size());
  }
  auto Changed = S;
  Changed.Mode = SourceAnalysisMode::Interactive;
  EXPECT_THROW(analyzeSourceBindings(Changed), Error);
}

TEST(WebLargeSource, ModuleInventoryUsesTheSameExplicitLargeWorkProfile) {
  std::string Text;
  Text.reserve(700128);
  for (unsigned I = 0; I < 350000; ++I)
    Text += "0;";
  Text +=
      "import {original as local} from './dep.js';export {local as exposed};";
  const auto S = inspectLargeJavaScript("large-module", Text, "module");
  ASSERT_EQ(S.ParseStatus, "parsed");
  const auto B = analyzeSourceBindings(S);
  ASSERT_EQ(B.Status, "ok");
  const auto M = analyzeSourceModules(S, B);
  ASSERT_EQ(M.Status, "ok");
  EXPECT_GT(M.Steps, MaxJavaScriptModuleSteps);
  EXPECT_LE(M.Steps, sourceWorkLimit(S, MaxJavaScriptModuleSteps));
  ASSERT_EQ(M.Requests.size(), 1u);
  ASSERT_EQ(M.Imports.size(), 1u);
  ASSERT_EQ(M.Exports.size(), 1u);
  EXPECT_EQ(M.Names[M.Requests[0].Specifier], u"./dep.js");
  EXPECT_EQ(M.Names[M.Exports[0].ExportedName], u"exposed");
  EXPECT_EQ(M.Exports[0].Binding, M.Imports[0].Binding);
  EXPECT_EQ(M.Exports[0].LinkStatus, "lexical_binding");
  const auto Recovery =
      inspectRecoveryJavaScript("recovery", "export{};", "module");
  SourceBindingAnalysis Unadmitted;
  Unadmitted.SourceID = Recovery.ID;
  Unadmitted.Status = "ok";
  const auto Rejected = analyzeSourceModules(Recovery, Unadmitted);
  EXPECT_EQ(Rejected.Status, "unsupported");
  ASSERT_EQ(Rejected.Diagnostics.size(), 1u);
  EXPECT_EQ(Rejected.Diagnostics[0].Code,
            "recovery_source_model_not_analyzable");
  EXPECT_TRUE(Rejected.Requests.empty());
}

TEST(WebLargeSource, MalformedEncodingSizeAndNodeBudgetsKeepRefusalExplicit) {
  EXPECT_THROW(
      inspectLargeJavaScript(
          "a",
          std::string(sourceLimits(SourceAnalysisMode::Large).Bytes + 1, ' '),
          "module"),
      Error);
  const auto Invalid =
      inspectLargeJavaScript("a", "const SECRET_CANARY = ;", "module");
  EXPECT_NE(Invalid.ParseStatus, "parsed");
  EXPECT_TRUE(Invalid.Nodes.empty());
  for (const auto &D : Invalid.Diagnostics)
    EXPECT_EQ(D.Code.find("SECRET_CANARY"), std::string::npos);
  EXPECT_EQ(
      inspectLargeJavaScript("a", std::string("x\0y", 3), "module").ParseStatus,
      "unsupported_encoding");
  std::string Dense;
  Dense.reserve(1600002);
  for (unsigned I = 0; I < 800001; ++I)
    Dense += "0;";
  try {
    const auto Result = inspectLargeJavaScript("a", Dense, "module");
    EXPECT_NE(Result.ParseStatus, "parsed");
    EXPECT_TRUE(Result.Nodes.empty());
  } catch (const Error &E) {
    EXPECT_EQ(std::string_view(E.what()), "parser_budget_exceeded");
  }
}

TEST(WebLargeSource, CapturedClaudePreviouslyRefusedUnitsWhenSupplied) {
  const auto *Base = std::getenv("NEVERD_CLAUDE_CODE_21296_CROSS_DIR");
  if (!Base)
    GTEST_SKIP() << "Optional pinned NeverD-recovered sources absent";
  struct Case {
    const char *File, *Hash;
    uint64_t Bytes, Nodes, Interfaces, Records;
  };
  constexpr std::array Cases{
      Case{"m00141.js",
           "efbc2d987608904a4a3ad7221f39f6e6afe34c13076d2ce3e4be2d49f43e6dd1",
           744610, 180783, 4, 12},
      Case{"m00299.js",
           "2a0e605d3c585ce776af544c82755ec30f932e568cdef58b2d9a5b1f53725eac",
           697926, 185080, 3, 33},
      Case{"m00415.js",
           "4c74137eb23221981404d0ca87b765912f83344851d5c132b21857e5685b03da",
           4762583, 1165367, 3, 141},
      Case{"m01452.js",
           "98f5ad67a24b8e8e412934f13de27fdb500fad75e6e886c437183e9e104e1df0",
           1458514, 286849, 6, 31}};
  for (const auto &Input : Cases) {
    SCOPED_TRACE(Input.File);
    const auto Root =
        std::filesystem::path(Base) / "darwin-arm64" / "recovered";
    const auto Snapshot = capture((Root / Input.File).string(), Limits{});
    ASSERT_EQ(Snapshot.Artifacts.size(), 1u);
    const auto &Raw = Snapshot.Artifacts[0];
    ASSERT_EQ(Raw.BlobHash, Input.Hash);
    ASSERT_EQ(Raw.Content.size(), Input.Bytes);
    const auto Bytes = Raw.Content.read(
        0, Input.Bytes, sourceLimits(SourceAnalysisMode::Large).Bytes);
    const auto S = inspectLargeJavaScript(Raw.ID, Bytes, "module");
    ASSERT_EQ(S.ParseStatus, "parsed");
    ASSERT_EQ(S.LexemeStatus, "ok");
    EXPECT_EQ(S.Nodes.size(), Input.Nodes);
    const auto B = analyzeSourceBindings(S);
    EXPECT_TRUE(B.Status == "ok" || B.Status == "partial");
    const auto N = analyzeSourceNavigation(S, B);
    EXPECT_TRUE(N.Status == "ok" || N.Status == "partial");
    const auto I = analyzeSourceInterfaces(S, B, analyzeSourceValues(S));
    ASSERT_EQ(I.Status, "partial");
    EXPECT_EQ(I.Records.size(), Input.Interfaces);
    const auto F = analyzeClientSourceFeatures(S, B, N);
    ASSERT_EQ(F.Status, "partial");
    const auto Relations = analyzeInterfaceSourceRelations(S, B, I, N);
    ASSERT_EQ(Relations.Status, "partial");
    const auto Source = projectPassiveSource(S, B, N, F, I, Relations);
    const auto Report = composePassiveReport(&Source, &I, nullptr, {}, nullptr);
    EXPECT_EQ(Report.Records.size(), Input.Records);
    for (const auto &Row : Report.Records) {
      EXPECT_EQ(Row.SourceID, S.ID);
      EXPECT_EQ(Row.EvidenceClass, "static_inference");
      EXPECT_LE(Row.Offset + Row.Length, Input.Bytes);
    }
    RecordProperty(std::string(Input.File) + "_binding_steps",
                   std::to_string(B.Steps));
    RecordProperty(std::string(Input.File) + "_navigation_steps",
                   std::to_string(N.Steps));
  }
}
} // namespace
