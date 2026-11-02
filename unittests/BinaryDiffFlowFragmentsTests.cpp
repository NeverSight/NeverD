//===- BinaryDiffFlowFragmentsTests.cpp - SSA boundaries --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact local facts survive changed external producers without counting those
/// producers as shared code or losing definition, ambiguity and stop
/// boundaries.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowFragments.h"
#include "neverd/diff/FlowMatch.h"

#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>

using namespace neverd::diff;
namespace {
FlowOperand ref(uint32_t Node) {
  FlowOperand I;
  I.Type = FlowOperand::Kind::Node;
  I.Node = Node;
  I.Bytes = 8;
  return I;
}
DataFlowGraph chain(unsigned Count = 5) {
  DataFlowGraph G;
  G.Revision = "owned";
  G.ABI = "owned-abi";
  G.Function = 0x1000;
  G.EntryBlock = 0;
  G.Blocks.push_back({0x1000, 0x1100});
  for (unsigned I = 0; I < Count; ++I) {
    FlowNode N;
    N.Opcode = I % 2 ? "INT_MULT" : "INT_ADD";
    N.ResultBytes = 8;
    N.Block = 0;
    N.Address = 0x1000 + I;
    N.OriginSequence = 0;
    auto V = ref(I ? I - 1 : NoIndex);
    if (!I)
      V.Type = FlowOperand::Kind::Parameter;
    N.Inputs.push_back(V);
    V.Type = FlowOperand::Kind::Constant;
    V.Node = NoIndex;
    V.Value = 11 + I;
    N.Inputs.push_back(V);
    G.Nodes.push_back(std::move(N));
  }
  return G;
}
FlowNeighborhoodMatch
match(const DataFlowGraph &A, unsigned AR, const DataFlowGraph &B, unsigned BR,
      unsigned Depth, uint64_t Work = neverd::limits::diff::kDefaultFlowMaxWork,
      const ComparisonControl &Control = {}) {
  const auto AK = fingerprintFlowFragments(A), BK = fingerprintFlowFragments(B);
  return matchFlowFragment(FlowFragmentInput(A, AK), AR,
                           FlowFragmentInput(B, BK), BR, Depth, Work, Control);
}
} // namespace

TEST(BinaryDiffFlowFragments, ChangedProducersStayOutsideTheExactFragment) {
  auto A = chain(), B = A;
  B.Nodes[0].Opcode = "INT_SUB";
  const auto R = match(A, 4, B, 4, 4);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Matched) << R.Reason;
  EXPECT_EQ(R.Nodes, (std::vector<std::pair<uint32_t, uint32_t>>{
                         {1, 1}, {2, 2}, {3, 3}, {4, 4}}));
  EXPECT_EQ(R.Frontier, (std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}}));
  EXPECT_TRUE(R.OpenDataFrontier);
  EXPECT_TRUE(R.OpenControlFrontier);
  EXPECT_TRUE(R.PhiBlocks.empty());
  B.Nodes[1].Inputs[1].Value ^= 1;
  EXPECT_EQ(match(A, 4, B, 4, 4).Outcome, FlowMatchOutcome::Different);
  B = A;
  B.Nodes[1].Inputs[0].Bytes = 4;
  EXPECT_EQ(match(A, 4, B, 4, 4).Outcome, FlowMatchOutcome::Different);
  B = A;
  B.Nodes[1].Inputs[1].Provenance = 1;
  EXPECT_EQ(match(A, 4, B, 4, 4).Outcome, FlowMatchOutcome::Different);
}

TEST(BinaryDiffFlowFragments, TypedFlagsAndFunctionContractsAreNotDiscarded) {
  auto A = chain();
  A.Profile = LLVMDataFlowProfile.str();
  for (auto &N : A.Nodes) {
    N.Semantics = "function-contract;result=i64;flags=0";
    N.Origins = {{N.Address, 0}};
    N.Address = 0;
    N.OriginSequence = -1;
    for (auto &I : N.Inputs)
      I.Semantics = "i64";
  }
  auto B = A;
  B.Nodes[1].Semantics = "function-contract;result=i64;flags=2";
  EXPECT_EQ(match(A, 4, B, 4, 4).Outcome, FlowMatchOutcome::Different);
  B = A;
  B.Nodes[1].Inputs[0].Semantics = "i63";
  EXPECT_EQ(match(A, 4, B, 4, 4).Outcome, FlowMatchOutcome::Different);
  B = A;
  for (auto &N : B.Nodes)
    N.Semantics = "different-function-contract;result=i64;flags=0";
  EXPECT_EQ(match(A, 4, B, 4, 4).Outcome, FlowMatchOutcome::Different);
}

TEST(BinaryDiffFlowFragments, ExternalDefinitionsShareTheInternalBijection) {
  auto A = chain(), B = A;
  A.Nodes[2].Inputs = {ref(0), ref(0)};
  B = A;
  B.Nodes[2].Inputs[1] = ref(1);
  EXPECT_EQ(match(A, 2, B, 2, 1).Outcome, FlowMatchOutcome::Different);
  A.Nodes[2].Inputs = {ref(0)};
  A.Nodes[3].Inputs = {ref(2), ref(0)};
  B = A;
  B.Nodes[3].Inputs[1] = ref(1);
  EXPECT_EQ(match(A, 3, B, 3, 2).Outcome, FlowMatchOutcome::Different);
  EXPECT_EQ(match(A, 3, A, 3, 2).Outcome, FlowMatchOutcome::Matched);
}

TEST(BinaryDiffFlowFragments, DepthLocalCommutativeKeysExposeAmbiguity) {
  auto A = chain(), B = A;
  A.Nodes[2].Commutative = true;
  A.Nodes[2].Inputs = {ref(0), ref(1)};
  B = A;
  std::reverse(B.Nodes[2].Inputs.begin(), B.Nodes[2].Inputs.end());
  // Different producers outside depth one cannot break the tie.
  EXPECT_EQ(match(A, 2, B, 2, 1).Outcome, FlowMatchOutcome::Ambiguous);
  EXPECT_EQ(match(A, 2, B, 2, 2).Outcome, FlowMatchOutcome::Matched);
  A.Nodes[2].Inputs = {ref(0), ref(0)};
  EXPECT_EQ(match(A, 2, A, 2, 1).Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(match(A, 2, A, 2, 1).Frontier.size(), 1u);
}

TEST(BinaryDiffFlowFragments, ReconvergentDepthsDoNotDuplicateCoveredInputs) {
  auto A = chain();
  A.Nodes[2].Inputs = {ref(0)};
  A.Nodes[3].Inputs = {ref(2)};
  A.Nodes[4].Inputs = {ref(3), ref(0)};
  const auto R = match(A, 4, A, 4, 3);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(R.Nodes.size(), 4u);
  EXPECT_TRUE(R.Frontier.empty());
  EXPECT_FALSE(R.OpenDataFrontier);
  A.Nodes[4].Inputs = {ref(3), ref(2)};
  const auto DifferentDepths = match(A, 4, A, 4, 3);
  EXPECT_EQ(DifferentDepths.Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(DifferentDepths.Nodes.size(), 4u);
  EXPECT_TRUE(DifferentDepths.Frontier.empty());
}

TEST(BinaryDiffFlowFragments, CyclesRemainOpenWithoutUnvisitedDefinitions) {
  auto A = chain(2);
  A.Nodes[0].Inputs = {ref(0)};
  auto R = match(A, 0, A, 0, 4);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(R.Nodes.size(), 1u);
  EXPECT_TRUE(R.Frontier.empty());
  EXPECT_TRUE(R.OpenDataFrontier);
  A.Nodes[0].Inputs = {ref(1)};
  A.Nodes[1].Inputs = {ref(0)};
  R = match(A, 0, A, 0, 4);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(R.Nodes.size(), 2u);
  EXPECT_TRUE(R.Frontier.empty());
  EXPECT_TRUE(R.OpenDataFrontier);
}

TEST(BinaryDiffFlowFragments, ExcludedNodesAreOnlyAdmittedOutsideTheDepth) {
  for (unsigned Case = 0; Case < 7; ++Case) {
    auto A = chain();
    switch (Case) {
    case 0:
      A.Nodes[0].Phi = true;
      break;
    case 1:
      A.Nodes[0].Gaps = FlowCallEffects;
      break;
    case 2:
      A.Nodes[0].Opcode = "COND_BR";
      break;
    case 3:
      A.Nodes[0].Inputs[0].Type = FlowOperand::Kind::Unknown;
      break;
    case 4:
      A.Nodes[0].Opcode = "LOAD";
      break;
    case 5:
      A.Nodes[0].MemoryOrdering = 1;
      break;
    case 6:
      // Detached graphs still need control exclusion even if malformed
      // metadata gives a native branch a scalar result width.
      A.Nodes[0].Opcode = "BRANCH";
      break;
    }
    EXPECT_EQ(match(A, 4, A, 4, 4).Outcome, FlowMatchOutcome::Matched) << Case;
    EXPECT_EQ(match(A, 3, A, 3, 4).Outcome, FlowMatchOutcome::Different)
        << Case;
    const auto K = fingerprintFlowFragments(A);
    EXPECT_TRUE(K.Layers[3].Admitted[4]);
    EXPECT_FALSE(K.Layers[3].Admitted[3]);
  }
}

TEST(BinaryDiffFlowFragments, RenumberingAndOriginsDoNotNominateByIdentity) {
  auto A = chain(), B = A;
  std::reverse(B.Nodes.begin(), B.Nodes.end());
  for (auto &N : B.Nodes) {
    N.Address += 0x5000;
    for (auto &I : N.Inputs)
      if (I.Type == FlowOperand::Kind::Node)
        I.Node = B.Nodes.size() - 1 - I.Node;
  }
  const auto R = match(A, 4, B, 0, 4);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Matched);
  EXPECT_EQ(R.Frontier, (std::vector<std::pair<uint32_t, uint32_t>>{{0, 4}}));
  const auto AK = fingerprintFlowFragments(A), BK = fingerprintFlowFragments(B);
  EXPECT_EQ(AK.Layers[3].Keys[4], BK.Layers[3].Keys[0]);
  B.ABI = "another-abi";
  EXPECT_EQ(match(A, 4, B, 0, 4).Outcome, FlowMatchOutcome::Unsupported);
}

TEST(BinaryDiffFlowFragments, CollidingKeysCannotHideChangedLocalFacts) {
  const auto A = chain();
  auto B = A;
  B.Nodes[1].Inputs[1].Value ^= 1;
  const auto K = fingerprintFlowFragments(A);
  const FlowFragmentInput X(A, K), Y(B, K);
  const auto R = matchFlowFragment(X, 4, Y, 4, 4);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Different);
  EXPECT_TRUE(R.Nodes.empty());
  EXPECT_TRUE(R.Frontier.empty());
}

TEST(BinaryDiffFlowFragments, IndexAndCandidateStopsPublishNoPartialEvidence) {
  const auto A = chain();
  const auto Keys = fingerprintFlowFragments(A);
  ASSERT_EQ(Keys.State, ComparisonState::Complete);
  DataFlowOptions Small;
  Small.MaxWork = Keys.Work - 1;
  const auto LimitedKeys = fingerprintFlowFragments(A, Small);
  EXPECT_EQ(LimitedKeys.State, ComparisonState::ResourceLimited);
  EXPECT_TRUE(LimitedKeys.Layers.empty());
  const auto Complete = match(A, 4, A, 4, 4);
  ASSERT_EQ(Complete.Outcome, FlowMatchOutcome::Matched);
  const auto Limited = match(A, 4, A, 4, 4, Complete.Work - 1);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_TRUE(Limited.Nodes.empty());
  EXPECT_TRUE(Limited.Frontier.empty());
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_EQ(fingerprintFlowFragments(A, {}, Control).State,
            ComparisonState::Cancelled);
  const auto Stopped = match(A, 4, A, 4, 4, Small.MaxWork, Control);
  EXPECT_EQ(Stopped.State, ComparisonState::Cancelled);
  EXPECT_TRUE(Stopped.Nodes.empty());
  EXPECT_TRUE(Stopped.Frontier.empty());
  EXPECT_THROW(match(A, 4, A, 4, 0), std::invalid_argument);
}

TEST(BinaryDiffFlowFragments,
     PreparedInputsAndFingerprintValidationObserveRestoreControl) {
  const auto G = chain();
  const auto Keys = fingerprintDataFlow(G);
  const auto Fragments = fingerprintFlowFragments(G);
  std::atomic<bool> Cancel = true;
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_THROW(FlowMatchInput(G, Keys, {}, Control), ComparisonInterrupted);
  EXPECT_THROW(FlowFragmentInput(G, Fragments, {}, Control),
               ComparisonInterrupted);
  const auto StoppedKeys = fingerprintDataFlow(G, {}, Control);
  EXPECT_EQ(StoppedKeys.State, ComparisonState::Cancelled);
  EXPECT_EQ(StoppedKeys.CompletedRounds, 0u);
  const auto StoppedFragments = fingerprintFlowFragments(G, {}, Control);
  EXPECT_EQ(StoppedFragments.State, ComparisonState::Cancelled);
  EXPECT_TRUE(StoppedFragments.Layers.empty());
  Cancel = false;
  Control.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(fingerprintDataFlow(G, {}, Control).State,
            ComparisonState::TimedOut);
  EXPECT_EQ(fingerprintFlowFragments(G, {}, Control).State,
            ComparisonState::TimedOut);
}
