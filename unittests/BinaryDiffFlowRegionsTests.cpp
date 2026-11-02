//===- BinaryDiffFlowRegionsTests.cpp - SSA region evidence invariants ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe split/merge/clone alternatives without double-counting overlapping
/// neighborhoods or silently admitting weak, stopped or incompatible evidence.
//===----------------------------------------------------------------------===//

#include "neverd/diff/ComparisonJSON.h"
#include "neverd/diff/FlowRegionsJSON.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <gtest/gtest.h>
#include <set>

using namespace neverd::diff;
namespace {
DataFlowGraph chains(uint64_t Address, std::initializer_list<uint64_t> Salts) {
  DataFlowGraph G;
  G.Revision = "fixture-revision";
  G.ABI = "fixture-abi";
  G.Function = Address;
  G.EntryBlock = 0;
  G.Blocks.push_back({Address, Address + 0x100});
  for (auto Salt : Salts) {
    const auto Start = uint32_t(G.Nodes.size());
    for (uint32_t I = 0; I < 5; ++I) {
      FlowNode N;
      N.Opcode = I % 2 ? "INT_MULT" : "INT_ADD";
      N.ResultBytes = 8;
      N.Block = 0;
      N.Address = Address + G.Nodes.size();
      N.OriginSequence = G.Nodes.size();
      FlowOperand Input;
      Input.Type = I ? FlowOperand::Kind::Node : FlowOperand::Kind::Parameter;
      Input.Node = I ? Start + I - 1 : NoIndex;
      Input.Bytes = 8;
      N.Inputs.push_back(Input);
      Input.Type = FlowOperand::Kind::Constant;
      Input.Node = NoIndex;
      Input.Value = Salt + I;
      N.Inputs.push_back(Input);
      G.Nodes.push_back(std::move(N));
    }
  }
  return G;
}
Program program(std::initializer_list<DataFlowGraph> Graphs) {
  ProgramIdentity Id;
  Id.Architecture = "x86_64";
  Id.ABI = "fixture-abi";
  Id.Revision = "fixture-revision";
  Id.SHA256 = std::string(64, 'a');
  Id.Bits = 64;
  Id.Endian = "little";
  ProgramBuilder B(Id);
  auto Evidence = std::make_shared<DataFlowEvidence>();
  Evidence->Recipe = "fixture-ssa";
  Evidence->EligibleFunctions = Graphs.size();
  for (const auto &G : Graphs) {
    B.beginFunction(G.Function, 0x100, "", NameSource::Generated, 0);
    B.addBlock(G.Function);
    std::set<uint64_t> Addresses;
    for (const auto &N : G.Nodes)
      N.forEachOrigin(
          [&](const FlowOrigin &O) { Addresses.insert(O.Address); });
    for (auto A : Addresses)
      B.addInstruction(A, {0x90}, "fixture", A, A, InstructionFlow::None);
    Evidence->Entries.push_back(
        {G.Function, FlowAvailability::Available, {}, G});
  }
  auto P = B.finish();
  P.DataFlow = std::move(Evidence);
  return P;
}
DataFlowSettings evidenceSettings() {
  DataFlowSettings S;
  S.Enabled = true;
  return S;
}
FlowRegionSettings regionSettings() {
  FlowRegionSettings S;
  S.Enabled = true;
  return S;
}
} // namespace

TEST(BinaryDiffFlowRegions, SplitMergeAndInlineAreAlternativeTopologyClaims) {
  auto A = program({chains(0x1000, {11, 37})});
  auto B = program({chains(0x2000, {11}), chains(0x3000, {37})});
  B.Instructions[0].Flow = InstructionFlow::Call;
  B.Instructions[0].Callee = 1;
  A.Functions[0].Module = "old-module";
  B.Functions[0].Module = B.Functions[1].Module = "new-module";
  const auto R = findFlowRegions(A, B, evidenceSettings(), regionSettings());
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  const auto &H = R.Hypotheses[0];
  EXPECT_EQ(H.PrimaryFunctions, (std::vector<uint64_t>{0x1000}));
  EXPECT_EQ(H.SecondaryFunctions, (std::vector<uint64_t>{0x2000, 0x3000}));
  EXPECT_EQ(H.Explanations,
            (std::vector<RegionKind>{RegionKind::Split, RegionKind::Outline,
                                     RegionKind::ModuleMove}));
  EXPECT_EQ(H.PrimaryNodes, 10u);
  EXPECT_EQ(H.SecondaryNodes, 10u);
  EXPECT_DOUBLE_EQ(H.PrimaryCoverage, 1);
  EXPECT_DOUBLE_EQ(H.SecondaryCoverage, 1);
  EXPECT_TRUE(H.OpenControlFrontier);
  EXPECT_TRUE(H.Ambiguous);
  EXPECT_TRUE(R.PrimaryScopeComplete);
  EXPECT_TRUE(R.SecondaryScopeComplete);
  const auto Again =
      findFlowRegions(A, B, evidenceSettings(), regionSettings());
  EXPECT_EQ(R.Anchors, Again.Anchors);
  EXPECT_EQ(R.Hypotheses, Again.Hypotheses);
  const auto Reverse =
      findFlowRegions(B, A, evidenceSettings(), regionSettings());
  ASSERT_EQ(Reverse.Hypotheses.size(), 1u);
  EXPECT_EQ(Reverse.Hypotheses[0].Explanations,
            (std::vector<RegionKind>{RegionKind::Merge, RegionKind::Inline,
                                     RegionKind::ModuleMove}));
  // The native adapter also admits tail transfers and resolved import slots
  // through Callee. Their spelling must not change the region explanation.
  for (auto Flow : {InstructionFlow::Jump, InstructionFlow::IndirectCall}) {
    B.Instructions[0].Flow = Flow;
    const auto Tail =
        findFlowRegions(A, B, evidenceSettings(), regionSettings());
    ASSERT_EQ(Tail.Hypotheses.size(), 1u);
    EXPECT_EQ(Tail.Hypotheses[0].Explanations, H.Explanations);
  }
}

TEST(BinaryDiffFlowRegions,
     NestedNeighborhoodsAreNotCopiesButMultipleTargetsAre) {
  const auto A = program({chains(0x1000, {11})});
  const auto B = program({chains(0x2000, {11})});
  const auto Shared =
      findFlowRegions(A, B, evidenceSettings(), regionSettings());
  ASSERT_EQ(Shared.Hypotheses.size(), 1u);
  ASSERT_GT(Shared.Anchors.size(), 1u);
  EXPECT_EQ(Shared.Hypotheses[0].Explanations,
            (std::vector<RegionKind>{RegionKind::Shared}));
  EXPECT_FALSE(Shared.Hypotheses[0].Ambiguous);
  EXPECT_EQ(Shared.Hypotheses[0].PrimaryNodes, 5u);
  for (const auto &A : Shared.Anchors) {
    EXPECT_GE(A.PrimaryOrigins, 4u);
    EXPECT_TRUE(A.UniqueSeed);
    EXPECT_TRUE(A.OpenControlFrontier);
  }
  const auto Copies = program({chains(0x2000, {11}), chains(0x3000, {11})});
  const auto R =
      findFlowRegions(A, Copies, evidenceSettings(), regionSettings());
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_EQ(R.Hypotheses[0].Explanations,
            (std::vector<RegionKind>{RegionKind::Split, RegionKind::Clone}));
  EXPECT_EQ(R.Hypotheses[0].PrimaryNodes, 5u);
  EXPECT_EQ(R.Hypotheses[0].SecondaryNodes, 10u);
  for (const auto &A : R.Anchors)
    EXPECT_FALSE(A.UniqueSeed);
}

TEST(BinaryDiffFlowRegions, ConstantsOriginsFrequencyAndCoverageRemainVisible) {
  const auto A = program({chains(0x1000, {11})});
  auto G = chains(0x2000, {12});
  auto R =
      findFlowRegions(A, program({G}), evidenceSettings(), regionSettings());
  EXPECT_TRUE(R.Anchors.empty());
  G = chains(0x2000, {11});
  for (auto &N : G.Nodes)
    N.Address = G.Function;
  R = findFlowRegions(A, program({G}), evidenceSettings(), regionSettings());
  EXPECT_TRUE(R.Anchors.empty());
  EXPECT_GT(R.WeakCandidates, 0u);
  auto S = regionSettings();
  S.MaxOccurrences = 1;
  auto B = program({chains(0x2000, {11}), chains(0x3000, {11})});
  R = findFlowRegions(A, B, evidenceSettings(), S);
  EXPECT_TRUE(R.Anchors.empty());
  EXPECT_GT(R.FrequentSeedGroups, 0u);
  auto Partial = std::make_shared<DataFlowEvidence>(*B.DataFlow);
  Partial->Entries.resize(1);
  Partial->State = ComparisonState::ResourceLimited;
  B.DataFlow = Partial;
  R = findFlowRegions(A, B, evidenceSettings(), regionSettings());
  EXPECT_FALSE(R.SecondaryScopeComplete);
  EXPECT_FALSE(R.Anchors.empty());
  G = chains(0x2000, {11});
  G.Gaps = FlowMemoryAlias;
  auto X = chains(0x1000, {11});
  X.Gaps = FlowMemoryAlias;
  R = findFlowRegions(program({X}), program({G}), evidenceSettings(),
                      regionSettings());
  ASSERT_FALSE(R.Hypotheses.empty());
  EXPECT_NE(R.Hypotheses[0].Gaps & FlowMemoryAlias, 0u);
  EXPECT_TRUE(R.Hypotheses[0].OpenDataFrontier);
}

TEST(BinaryDiffFlowRegions,
     StopsKeepOnlyCompletedEvidenceAndRejectWrongProfiles) {
  const auto A = program({chains(0x1000, {11})});
  const auto B = program({chains(0x2000, {11})});
  for (unsigned Limit = 0; Limit < 4; ++Limit) {
    auto S = regionSettings();
    if (Limit == 0)
      S.MaxSeeds = 1;
    else if (Limit == 1)
      S.MaxWork = 2;
    else if (Limit == 2)
      S.MaxNodePairs = 1;
    else
      S.MaxAnchors = 1;
    const auto R = findFlowRegions(A, B, evidenceSettings(), S);
    EXPECT_EQ(R.State, ComparisonState::ResourceLimited);
    EXPECT_LE(R.Work, S.MaxWork);
    EXPECT_EQ(R.Anchors.size(), Limit == 3 ? 1u : 0u);
    EXPECT_TRUE(R.Hypotheses.empty());
    EXPECT_FALSE(R.StoppedPhase.empty());
  }
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "flow-regions:groups")
      Cancel = true;
  };
  const auto R =
      findFlowRegions(A, B, evidenceSettings(), regionSettings(), Control);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_FALSE(R.Anchors.empty());
  EXPECT_TRUE(R.Hypotheses.empty());
  EXPECT_THROW(findFlowRegions(A, B, {}, regionSettings()),
               std::invalid_argument);
  auto Bad = B;
  Bad.Identity.ABI = "other-abi";
  EXPECT_THROW(findFlowRegions(A, Bad, evidenceSettings(), regionSettings()),
               std::invalid_argument);
}

TEST(BinaryDiffFlowRegions,
     SavedCorrespondencesRebindAndForgedFactsAreRefused) {
  const auto A = program({chains(0x1000, {11})});
  const auto B = program({chains(0x2000, {11})});
  ComparisonSettings S;
  S.DataFlow = evidenceSettings();
  S.FlowRegions = regionSettings();
  const auto R = compare(A, B, S);
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_FALSE(R.FlowRegions.Anchors.empty());
  std::string Text, Error;
  llvm::raw_string_ostream OS(Text);
  writeComparison(OS, R, A, B);
  const auto Saved = readComparison(Text, Error);
  ASSERT_TRUE(Saved) << Error;
  ComparisonResult Bound;
  EXPECT_FALSE(bindComparison(*Saved, A, B, Bound));
  EXPECT_EQ(Bound.FlowRegions, R.FlowRegions);
  auto OtherWork = *Saved;
  ++OtherWork.FlowRegions.Work;
  EXPECT_FALSE(bindComparison(OtherWork, A, B, Bound));
  EXPECT_EQ(Bound.FlowRegions.Work, OtherWork.FlowRegions.Work);
  OtherWork.FlowRegions.Work = S.FlowRegions.MaxWork + 1;
  EXPECT_TRUE(bindComparison(OtherWork, A, B, Bound));
  EXPECT_FALSE(bindComparison(*Saved, A, B, Bound));
  auto Forged = *Saved;
  ++Forged.FlowRegions.Anchors[0].Nodes[0].second;
  BindFailure Why;
  EXPECT_TRUE(bindComparison(Forged, A, B, Bound, &Why));
  EXPECT_EQ(Why, BindFailure::InvalidEvidence);
  EXPECT_EQ(Bound.FlowRegions, R.FlowRegions);
  Forged = *Saved;
  Forged.FlowRegions.Hypotheses[0].PrimaryCoverage = 0.25;
  EXPECT_TRUE(bindComparison(Forged, A, B, Bound));
  Forged = *Saved;
  Forged.FlowRegions = {};
  EXPECT_TRUE(bindComparison(Forged, A, B, Bound));
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "flow-regions:groups")
      Cancel = true;
  };
  auto Partial = *Saved;
  Partial.State = ComparisonState::Cancelled;
  Partial.FlowRegions =
      findFlowRegions(A, B, S.DataFlow, S.FlowRegions, Control);
  Partial.StoppedPhase = Partial.FlowRegions.StoppedPhase;
  EXPECT_FALSE(bindComparison(Partial, A, B, Bound));
  EXPECT_TRUE(Bound.FlowRegions.Hypotheses.empty());
  ++Partial.FlowRegions.Anchors[0].SecondaryRoot;
  EXPECT_TRUE(bindComparison(Partial, A, B, Bound));
  auto JSON = llvm::json::parse(Text);
  ASSERT_TRUE(bool(JSON));
  (*JSON->getAsObject()->getObject("flow_regions"))["semantic_validation"] =
      "equivalent";
  Text.clear();
  OS << *JSON;
  EXPECT_FALSE(readComparison(Text, Error));
  EXPECT_EQ(readFlowRegionSettings(flowRegionSettingsJSON(S.FlowRegions)),
            S.FlowRegions);
  EXPECT_THROW(
      readFlowRegionSettings(llvm::json::Object{{"max_node_pairs", 0}}),
      std::invalid_argument);
}

TEST(BinaryDiffFlowRegions, ManyRootsReuseGraphAdmissionWithinFiniteWork) {
  // Independent source expressions make hundreds of distinct candidate roots
  // in one function. Charging/validating its whole graph per root used to
  // exhaust this allowance before most of the known correspondences survived.
  auto G = chains(0x1000, {});
  for (unsigned C = 0; C < 100; ++C) {
    auto Part = chains(G.Function, {100u * C + 1});
    const auto Begin = uint32_t(G.Nodes.size());
    for (auto N : Part.Nodes) {
      N.Address += Begin;
      N.OriginSequence += Begin;
      for (auto &I : N.Inputs)
        if (I.Type == FlowOperand::Kind::Node)
          I.Node += Begin;
      G.Nodes.push_back(std::move(N));
    }
  }
  G.Blocks[0].End = G.Function + G.Nodes.size();
  auto H = G;
  H.Function += 0x7000;
  H.Blocks[0].Begin += 0x7000;
  H.Blocks[0].End += 0x7000;
  for (auto &N : H.Nodes)
    N.Address += 0x7000;
  auto Settings = regionSettings();
  Settings.MaxWork = 250000;
  const auto R =
      findFlowRegions(program({G}), program({H}), evidenceSettings(), Settings);
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_EQ(R.PrimarySeeds, 500u);
  EXPECT_EQ(R.Anchors.size(), 200u);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_DOUBLE_EQ(R.Hypotheses[0].PrimaryCoverage, 1);
  EXPECT_DOUBLE_EQ(R.Hypotheses[0].SecondaryCoverage, 1);
  EXPECT_LE(R.Work, Settings.MaxWork);
}

TEST(BinaryDiffFlowRegions,
     OptimizedOriginGateCountsInstructionsNotOperations) {
  const auto Convert = [](DataFlowGraph G) {
    G.Profile = LLVMDataFlowProfile.str();
    for (auto &N : G.Nodes) {
      N.Origins = {{G.Function, N.OriginSequence}};
      N.Address = 0;
      N.OriginSequence = -1;
      N.Semantics = "owned-typed-fixture";
      for (auto &I : N.Inputs)
        I.Semantics = "i64";
    }
    return G;
  };
  auto A = Convert(chains(0x1000, {17}));
  auto B = Convert(chains(0x8000, {17}));
  auto Evidence = evidenceSettings();
  Evidence.Profile = LLVMDataFlowProfile.str();
  FlowRegionSettings Options;
  Options.Enabled = true;
  const auto Weak =
      findFlowRegions(program({A}), program({B}), Evidence, Options);
  EXPECT_TRUE(Weak.Anchors.empty());
  EXPECT_GT(Weak.WeakCandidates, 0u);
  EXPECT_EQ(Weak.Profile, LLVMFlowRegionProfile);
  A.Nodes.back().Origins.push_back({0x1004, 0});
  B.Nodes.back().Origins.push_back({0x8004, 0});
  auto PA = program({A}), PB = program({B});
  auto Qualified = findFlowRegions(PA, PB, Evidence, Options);
  ASSERT_FALSE(Qualified.Anchors.empty());
  EXPECT_EQ(Qualified.Anchors.back().PrimaryOrigins, 2u);
  Qualified.Profile = FlowRegionProfile.str();
  EXPECT_TRUE(validateFlowRegions(Qualified, PA, PB, Evidence, Options));
  FlowRegionAnalysis Empty;
  EXPECT_TRUE(validateFlowRegions(Empty, PA, PB, Evidence, Options));
  Empty.Profile = LLVMFlowRegionProfile.str();
  EXPECT_FALSE(validateFlowRegions(Empty, PA, PB, Evidence, Options));
}

TEST(BinaryDiffFlowRegions, OptimizedPluralAndUnstartedProfilesRoundTrip) {
  auto A = chains(0x1000, {17}), B = chains(0x8000, {17});
  for (auto *G : {&A, &B}) {
    G->Profile = LLVMDataFlowProfile.str();
    for (uint32_t I = 0; I < G->Nodes.size(); ++I) {
      auto &N = G->Nodes[I];
      N.Origins = {{G->Function + 2 * I, 0}, {G->Function + 2 * I + 1, 1}};
      N.Address = 0;
      N.OriginSequence = -1;
      N.Semantics = "owned-typed-fixture";
      for (auto &Input : N.Inputs)
        Input.Semantics = "i64";
    }
  }
  auto Evidence = evidenceSettings();
  Evidence.Profile = LLVMDataFlowProfile.str();
  FlowRegionSettings Options;
  Options.Enabled = true;
  const auto R = findFlowRegions(program({A}), program({B}), Evidence, Options);
  ASSERT_FALSE(R.Anchors.empty());
  EXPECT_GT(R.Anchors.front().PrimaryOrigins, R.Anchors.front().Nodes.size());
  FlowRegionAnalysis Empty;
  Empty.Profile = LLVMFlowRegionProfile.str();
  for (const auto &Input : {R, Empty}) {
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    llvm::json::OStream JSON(OS);
    JSON.object([&] { writeFlowRegionAnalysisFields(JSON, Input, false); });
    auto Value = llvm::json::parse(Text);
    ASSERT_TRUE(bool(Value));
    EXPECT_EQ(readFlowRegionAnalysis(*Value, Options), Input);
  }
}

TEST(BinaryDiffFlowRegions, FragmentsKeepChangedProducersOutsideCoverage) {
  auto X = chains(0x1000, {17}), Y = chains(0x8000, {17});
  Y.Nodes[0].Opcode = "INT_SUB";
  auto A = program({X}), B = program({Y});
  auto Options = regionSettings();
  EXPECT_TRUE(
      findFlowRegions(A, B, evidenceSettings(), Options).Anchors.empty());
  Options.Fragments = true;
  const auto R = findFlowRegions(A, B, evidenceSettings(), Options);
  ASSERT_EQ(R.State, ComparisonState::Complete);
  ASSERT_EQ(R.Anchors.size(), 1u);
  EXPECT_EQ(R.Profile, FlowFragmentProfile);
  const auto &F = R.Anchors.front();
  EXPECT_EQ(F.Depth, 4u);
  EXPECT_EQ(F.Nodes.size(), 4u);
  EXPECT_EQ(F.Frontier, (std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}}));
  EXPECT_EQ(F.PrimaryOrigins, 4u);
  ASSERT_EQ(R.Hypotheses.size(), 1u);
  EXPECT_EQ(R.Hypotheses.front().PrimaryNodes, 4u);
  EXPECT_DOUBLE_EQ(R.Hypotheses.front().PrimaryCoverage, 0.8);
  EXPECT_TRUE(R.Hypotheses.front().OpenDataFrontier);
  Y.Nodes[1].Inputs[1].Value ^= 1;
  EXPECT_TRUE(findFlowRegions(A, program({Y}), evidenceSettings(), Options)
                  .Anchors.empty());
  Options.MaxNodePairs = 4;
  const auto Limited = findFlowRegions(A, B, evidenceSettings(), Options);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_TRUE(Limited.Anchors.empty());
  EXPECT_FALSE(validateFlowRegions(Limited, A, B, evidenceSettings(), Options));
  Options.MaxNodePairs = 5;
  EXPECT_EQ(findFlowRegions(A, B, evidenceSettings(), Options).Anchors.size(),
            1u);
  Options.MaxSeeds = 1;
  EXPECT_EQ(findFlowRegions(A, B, evidenceSettings(), Options).State,
            ComparisonState::ResourceLimited);
}

TEST(BinaryDiffFlowRegions, FragmentBoundariesAndRecipesRebindAfterSaving) {
  for (bool Optimized : {false, true}) {
    auto X = chains(0x1000, {17}), Y = chains(0x8000, {17});
    Y.Nodes[0].Opcode = "INT_SUB";
    auto Evidence = evidenceSettings();
    if (Optimized) {
      Evidence.Profile = LLVMDataFlowProfile.str();
      for (auto *G : {&X, &Y}) {
        G->Profile = Evidence.Profile;
        for (uint32_t I = 0; I < G->Nodes.size(); ++I) {
          auto &N = G->Nodes[I];
          N.Semantics = "owned-exact-contract";
          N.Origins = {{G->Function + 2 * I, 0}, {G->Function + 2 * I + 1, 1}};
          N.Address = 0;
          N.OriginSequence = -1;
          for (auto &V : N.Inputs)
            V.Semantics = "i64";
        }
      }
    }
    const auto A = program({X}), B = program({Y});
    ComparisonSettings Settings;
    Settings.DataFlow = Evidence;
    Settings.FlowRegions = regionSettings();
    Settings.FlowRegions.Fragments = true;
    const auto R = compare(A, B, Settings);
    ASSERT_FALSE(R.FlowRegions.Anchors.empty());
    std::string Text, Error;
    llvm::raw_string_ostream OS(Text);
    writeComparison(OS, R, A, B);
    auto Saved = readComparison(Text, Error);
    ASSERT_TRUE(Saved) << Error;
    ComparisonResult Bound;
    EXPECT_FALSE(bindComparison(*Saved, A, B, Bound));
    EXPECT_EQ(Bound.FlowRegions, R.FlowRegions);
    for (unsigned Case = 0; Case < 4; ++Case) {
      auto Bad = *Saved;
      switch (Case) {
      case 0:
        ++Bad.FlowRegions.Anchors[0].Depth;
        break;
      case 1:
        Bad.FlowRegions.Anchors[0].Frontier.clear();
        break;
      case 2:
        ++Bad.FlowRegions.Anchors[0].Frontier[0].second;
        break;
      case 3:
        Bad.Settings.FlowRegions.Fragments = false;
        break;
      }
      EXPECT_TRUE(bindComparison(Bad, A, B, Bound)) << Case;
    }
    auto JSON = llvm::json::parse(Text);
    ASSERT_TRUE(bool(JSON));
    auto *FR = JSON->getAsObject()->getObject("flow_regions");
    (*FR)["profile"] = Optimized ? LLVMFlowRegionProfile : FlowRegionProfile;
    (*JSON->getAsObject()
          ->getObject("settings")
          ->getObject("flow_regions"))["fragments"] = false;
    Text.clear();
    OS << *JSON;
    EXPECT_FALSE(readComparison(Text, Error));
  }
}

TEST(BinaryDiffFlowRegions, FragmentPublicationKeepsStoppedPrefixesStable) {
  auto A = program({chains(0x1000, {17, 73})});
  auto B = program({chains(0x8000, {17, 73})});
  auto Options = regionSettings();
  Options.Fragments = true;
  const auto Full = findFlowRegions(A, B, evidenceSettings(), Options);
  ASSERT_FALSE(Full.Anchors.empty());
  std::atomic<bool> Cancel{false};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  Control.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "flow-regions:groups")
      Cancel = true;
  };
  const auto Stopped =
      findFlowRegions(A, B, evidenceSettings(), Options, Control);
  EXPECT_EQ(Stopped.State, ComparisonState::Cancelled);
  EXPECT_EQ(Stopped.Anchors, Full.Anchors);
  EXPECT_TRUE(Stopped.Hypotheses.empty());
  EXPECT_FALSE(validateFlowRegions(Stopped, A, B, evidenceSettings(), Options));
  const auto Early =
      findFlowRegions(A, B, evidenceSettings(), Options, Control);
  EXPECT_EQ(Early.Profile, FlowFragmentProfile);
  EXPECT_TRUE(Early.Anchors.empty());
  EXPECT_FALSE(validateFlowRegions(Early, A, B, evidenceSettings(), Options));
  EXPECT_EQ(readFlowRegionSettings(flowRegionSettingsJSON(Options)), Options);
  EXPECT_THROW(readFlowRegionSettings(llvm::json::Object{{"fragments", 1}}),
               std::invalid_argument);
}

TEST(BinaryDiffFlowRegions, CrossAnchorBoundaryConflictsOnlyMarkAmbiguity) {
  for (bool Internal : {false, true}) {
    auto X = chains(0x1000, {17, 73}), Y = chains(0x8000, {17, 73});
    X.Nodes[6].Inputs[0].Node = 0;
    if (Internal) {
      Y.Nodes[0].Opcode = "INT_SUB";
      const auto Address = Y.Nodes[5].Address;
      Y.Nodes[5] = X.Nodes[0];
      Y.Nodes[5].Address = Address;
    } else {
      for (auto *G : {&X, &Y})
        for (auto I : {0u, 5u})
          G->Nodes[I].Gaps = FlowMemoryAlias;
    }
    auto Options = regionSettings();
    Options.Fragments = true;
    const auto R = findFlowRegions(program({X}), program({Y}),
                                   evidenceSettings(), Options);
    ASSERT_EQ(R.State, ComparisonState::Complete);
    ASSERT_EQ(R.Hypotheses.size(), 1u);
    EXPECT_TRUE(R.Hypotheses[0].Ambiguous) << Internal;
    EXPECT_FALSE(
        llvm::is_contained(R.Hypotheses[0].Explanations, RegionKind::Clone));
    std::set<uint32_t> Matched;
    for (const auto &A : R.Anchors)
      for (const auto &[P, S] : A.Nodes) {
        (void)S;
        Matched.insert(P);
      }
    EXPECT_EQ(R.Hypotheses[0].PrimaryNodes, Matched.size());
    EXPECT_EQ(Matched.count(0), unsigned(Internal));
  }
}

TEST(BinaryDiffFlowRegions, LegacyStoppedAndCompleteRecipesKeepTheirEncoding) {
  for (bool Optimized : {false, true}) {
    auto X = chains(0x1000, {17}), Y = chains(0x8000, {17});
    auto Evidence = evidenceSettings();
    if (Optimized) {
      Evidence.Profile = LLVMDataFlowProfile.str();
      for (auto *G : {&X, &Y}) {
        G->Profile = Evidence.Profile;
        for (auto &N : G->Nodes) {
          N.Semantics = "owned-exact-contract";
          N.Origins = {{N.Address, N.OriginSequence}};
          N.Address = 0;
          N.OriginSequence = -1;
          for (auto &V : N.Inputs)
            V.Semantics = "i64";
        }
      }
    }
    auto A = program({X}), B = program({Y});
    for (unsigned Case = 0; Case < 3; ++Case) {
      auto Options = regionSettings();
      if (Case == 1)
        Options.MaxAnchors = 1;
      std::atomic<bool> Cancel{Case == 2};
      ComparisonControl Control;
      Control.Cancel = &Cancel;
      const auto R = findFlowRegions(A, B, Evidence, Options, Control);
      EXPECT_FALSE(validateFlowRegions(R, A, B, Evidence, Options));
      std::string Text;
      llvm::raw_string_ostream OS(Text);
      llvm::json::OStream JSON(OS);
      JSON.object([&] { writeFlowRegionAnalysisFields(JSON, R, false); });
      auto Value = llvm::json::parse(Text);
      ASSERT_TRUE(bool(Value));
      EXPECT_EQ(readFlowRegionAnalysis(*Value, Options), R);
      EXPECT_EQ(Text.find("\"depth\""), std::string::npos);
      EXPECT_EQ(Text.find("\"frontier\""), std::string::npos);
      EXPECT_FALSE(flowRegionSettingsJSON(Options).get("fragments"));
    }
  }
}
