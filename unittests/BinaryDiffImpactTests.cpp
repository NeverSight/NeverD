//===- BinaryDiffImpactTests.cpp - Native call impact evidence ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check observed witnesses, recursion, partial scope and unknown targets
/// independently of function pairing and intraprocedural SSA availability.
//===----------------------------------------------------------------------===//

#include "neverd/diff/CallImpact.h"

#include "llvm/ADT/STLExtras.h"

#include <atomic>
#include <gtest/gtest.h>
#include <set>
#include <stdexcept>

using namespace neverd::diff;
namespace {
Program fixture() {
  ProgramIdentity Id;
  Id.Revision = "impact-fixture";
  Id.DiscoveryComplete = true;
  ProgramBuilder B(Id);
  // Entry -> export -> changed; entry also calls changed directly. Preserve
  // duplicate sites and a recursive cycle rather than counting paths forever.
  B.beginFunction(0x1000, 16, "entry", NameSource::Symbol, TraitEntryPoint);
  B.addBlock(0x1000);
  B.addInstruction(0x1000, {0xe8}, "call", 1, 1, InstructionFlow::Call, 0x2000);
  B.addInstruction(0x1001, {0xe8}, "call", 1, 1, InstructionFlow::Call, 0x3000);
  B.addInstruction(0x1002, {0xe8}, "call", 1, 1, InstructionFlow::Call, 0x3000);
  B.beginFunction(0x2000, 16, "export", NameSource::Export, TraitExported);
  B.addBlock(0x2000);
  B.addInstruction(0x2000, {0xe9}, "jmp", 2, 2, InstructionFlow::Jump, 0x3000);
  B.beginFunction(0x3000, 16, "changed", NameSource::Symbol, 0);
  B.addBlock(0x3000);
  B.addInstruction(0x3000, {0xe8}, "call", 1, 1, InstructionFlow::Call, 0x2000);
  B.addInstruction(0x3001, {0xff}, "call", 3, 3, InstructionFlow::IndirectCall,
                   0x5000);
  // No path connects this caller until its indirect target is resolved.
  B.beginFunction(0x4000, 16, "unknown", NameSource::Symbol, TraitIncomplete);
  B.addBlock(0x4000);
  B.addInstruction(0x4000, {0xff}, "call", 3, 3, InstructionFlow::IndirectCall);
  B.addInstruction(0x4001, {0xff}, "jmp", 3, 3, InstructionFlow::IndirectJump);
  B.beginFunction(0x5000, 0, "import", NameSource::Import, TraitImport);
  return B.finish();
}
std::set<uint32_t> functions(const CallImpact &R) {
  std::set<uint32_t> Out;
  for (auto F : R.Functions)
    Out.insert(F.Function);
  return Out;
}
void checkWitnesses(const CallImpact &R, ImpactDirection Direction) {
  for (uint32_t I = 0; I < R.Functions.size(); ++I) {
    const auto &F = R.Functions[I];
    if (F.Predecessor == NoIndex) {
      EXPECT_EQ(F.Depth, 0u);
      EXPECT_EQ(F.WitnessSite, NoIndex);
      continue;
    }
    ASSERT_LT(F.Predecessor, I);
    ASSERT_LT(F.WitnessSite, R.Sites.size());
    const auto &S = R.Sites[F.WitnessSite];
    const auto &Prev = R.Functions[F.Predecessor];
    EXPECT_EQ(F.Depth, Prev.Depth + 1);
    EXPECT_EQ(F.Function,
              Direction == ImpactDirection::Callers ? S.Caller : S.Callee);
    EXPECT_EQ(Prev.Function,
              Direction == ImpactDirection::Callers ? S.Callee : S.Caller);
    EXPECT_EQ(S.Gaps, 0u);
  }
}
} // namespace

TEST(BinaryDiffImpact, CallersKeepShortestWitnessesTailsCyclesAndUnknownScope) {
  auto P = fixture();
  const auto R = analyzeCallImpact(P, {2}, ImpactDirection::Callers);
  ASSERT_EQ(R.State, ComparisonState::Complete);
  EXPECT_TRUE(R.IndexComplete);
  EXPECT_TRUE(R.TraversalComplete);
  EXPECT_EQ(R.IndexedFunctions, 5u);
  EXPECT_EQ(R.IndexedInstructions, 8u);
  EXPECT_EQ(functions(R), (std::set<uint32_t>{0, 1, 2}));
  EXPECT_EQ(R.TraversedSites.size(), 5u);
  EXPECT_EQ(R.BoundarySites.size(), 4u);
  EXPECT_TRUE(R.Gaps & ImpactIndirectCall);
  EXPECT_TRUE(R.Gaps & ImpactIndirectTransfer);
  EXPECT_TRUE(R.Gaps & ImpactExternalBody);
  checkWitnesses(R, ImpactDirection::Callers);
  for (const auto &F : R.Functions) {
    EXPECT_LE(F.Depth, 1u);
    EXPECT_TRUE(F.ScanComplete);
  }
  EXPECT_EQ(R, analyzeCallImpact(P, {2, 2}, ImpactDirection::Callers));
  // Callee is the authority, even for a resolved indirect or synthetic tail.
  P.Instructions[3].Flow = InstructionFlow::IndirectJump;
  EXPECT_EQ(R, analyzeCallImpact(P, {2}, ImpactDirection::Callers));
}

TEST(BinaryDiffImpact, FilteredTransfersKeepTheirUnknownBoundary) {
  for (InstructionFlow Flow :
       {InstructionFlow::Jump, InstructionFlow::Branch}) {
    ProgramBuilder Builder({});
    Builder.beginFunction(0x1000, 2, "caller", NameSource::Symbol, 0);
    Builder.addBlock(0x1000);
    Builder.addInstruction(0x1000, {0x74, 0x10}, "transfer", 1, 1, Flow,
                           0x2000);
    const Program P = Builder.finish();
    ASSERT_TRUE(P.Instructions[0].UnresolvedCallee);
    const auto R = analyzeCallImpact(P, {0}, ImpactDirection::Callees);
    ASSERT_EQ(R.State, ComparisonState::Complete);
    EXPECT_EQ(functions(R), (std::set<uint32_t>{0}));
    ASSERT_EQ(R.BoundarySites.size(), 1U);
    const auto &Site = R.Sites[R.BoundarySites.front()];
    EXPECT_EQ(Site.Callee, NoIndex);
    EXPECT_EQ(Site.Instruction, 0U);
    EXPECT_TRUE(Site.Gaps & ImpactUnresolvedCall);
  }
}

TEST(BinaryDiffImpact, CalleesKeepImportsAndMultipleSeedsWithoutInventedEdges) {
  auto P = fixture();
  const auto R = analyzeCallImpact(P, {0}, ImpactDirection::Callees);
  EXPECT_EQ(functions(R), (std::set<uint32_t>{0, 1, 2, 4}));
  EXPECT_EQ(R.TraversedSites.size(), 6u);
  checkWitnesses(R, ImpactDirection::Callees);
  const auto Both = analyzeCallImpact(P, {3, 0}, ImpactDirection::Callees);
  EXPECT_EQ(functions(Both), (std::set<uint32_t>{0, 1, 2, 3, 4}));
  EXPECT_EQ(Both.Functions[0].Function, 0u);
  EXPECT_EQ(Both.Functions[1].Function, 3u);
  P.Identity.DiscoveryComplete = false;
  P.Edges.push_back({0, 0, EdgeKind::Exception});
  P.Functions[0].EdgeCount = 1;
  const auto Partial = analyzeCallImpact(P, {0}, ImpactDirection::Callees);
  EXPECT_TRUE(Partial.Gaps & ImpactDiscovery);
  EXPECT_TRUE(Partial.Gaps & ImpactExceptionalControl);
  EXPECT_TRUE(Partial.Functions[0].Gaps & ImpactExceptionalControl);
}

TEST(BinaryDiffImpact, DepthAndStorageStopsRetainReviewableFrontiers) {
  const auto P = fixture();
  ImpactOptions O;
  O.MaxDepth = 0;
  auto R = analyzeCallImpact(P, {2}, ImpactDirection::Callers, O);
  EXPECT_EQ(R.State, ComparisonState::ResourceLimited);
  EXPECT_TRUE(R.IndexComplete);
  EXPECT_FALSE(R.TraversalComplete);
  EXPECT_EQ(functions(R), (std::set<uint32_t>{2}));
  EXPECT_EQ(R.FrontierSites.size(), 3u);
  O.MaxDepth = 1;
  R = analyzeCallImpact(P, {0}, ImpactDirection::Callees, O);
  EXPECT_EQ(functions(R), (std::set<uint32_t>{0, 1, 2}));
  EXPECT_EQ(R.FrontierSites.size(), 1u);
  O.MaxFunctions = 2;
  R = analyzeCallImpact(P, {0}, ImpactDirection::Callees, O);
  EXPECT_EQ(R.Functions.size(), 2u);
  EXPECT_FALSE(R.FrontierSites.empty());
  O = {};
  O.MaxSites = 2;
  R = analyzeCallImpact(P, {2}, ImpactDirection::Callers, O);
  EXPECT_EQ(R.IndexState, ComparisonState::ResourceLimited);
  EXPECT_FALSE(R.IndexComplete);
  EXPECT_EQ(R.Sites.size(), 2u);
  EXPECT_EQ(functions(R), (std::set<uint32_t>{0, 2}));
  EXPECT_FALSE(R.Functions.back().ScanComplete);
  O = {};
  O.MaxWork = 1;
  R = analyzeCallImpact(P, {2}, ImpactDirection::Callers, O);
  EXPECT_EQ(R.State, ComparisonState::ResourceLimited);
  EXPECT_TRUE(R.Sites.empty());
  EXPECT_LE(R.Work, O.MaxWork);
}

TEST(BinaryDiffImpact, CancellationDeadlinesAndMalformedInputsStayExplicit) {
  auto P = fixture();
  std::atomic<bool> Cancel{true};
  ComparisonControl C;
  C.Cancel = &Cancel;
  auto R = analyzeCallImpact(P, {2}, ImpactDirection::Callers, {}, C);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_TRUE(R.Sites.empty());
  Cancel = false;
  C.Progress = [&](llvm::StringRef Phase, uint64_t, uint64_t) {
    if (Phase == "impact:traverse")
      Cancel = true;
  };
  R = analyzeCallImpact(P, {2}, ImpactDirection::Callers, {}, C);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_TRUE(R.IndexComplete);
  EXPECT_FALSE(R.Sites.empty());
  EXPECT_TRUE(R.Functions.empty());
  C = {};
  C.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(analyzeCallImpact(P, {2}, ImpactDirection::Callers, {}, C).State,
            ComparisonState::TimedOut);
  EXPECT_THROW(analyzeCallImpact(P, {}, ImpactDirection::Callers),
               std::invalid_argument);
  EXPECT_THROW(analyzeCallImpact(P, {5}, ImpactDirection::Callers),
               std::invalid_argument);
  ImpactOptions Wide;
  Wide.MaxFunctions = 8192;
  std::vector<uint32_t> TooManySeeds(4097, 0);
  EXPECT_THROW(
      analyzeCallImpact(P, TooManySeeds, ImpactDirection::Callers, Wide),
      std::invalid_argument);
  P.Instructions[0].Callee = 7;
  EXPECT_THROW(analyzeCallImpact(P, {0}, ImpactDirection::Callers),
               std::invalid_argument);
}
