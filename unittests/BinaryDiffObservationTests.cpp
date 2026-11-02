//===- BinaryDiffObservationTests.cpp - Patch review evidence contracts ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authored immutable observations test attribution, abstention and scope.
/// They are not compiler or native-execution qualification.
//===----------------------------------------------------------------------===//

#include "neverd/diff/ChangeObservations.h"
#include "neverd/ir/low/LowIR.h"

#include "llvm/ADT/STLExtras.h"

#include <atomic>
#include <gtest/gtest.h>

using namespace neverd::diff;
namespace {
FlowOperand constant(uint64_t Value, uint16_t Bytes = 4) {
  FlowOperand V;
  V.Type = FlowOperand::Kind::Constant;
  V.Value = Value;
  V.Bytes = Bytes;
  return V;
}
FlowOperand node(uint32_t N, uint16_t Bytes = 1) {
  auto V = constant(0, Bytes);
  V.Type = FlowOperand::Kind::Node;
  V.Node = N;
  return V;
}
DataFlowGraph guard(uint64_t Base, uint64_t Limit = 17) {
  DataFlowGraph G;
  G.Revision = "owned-observation-fixture";
  G.ABI = "fixture-abi";
  G.Function = Base;
  G.EntryBlock = 0;
  G.Blocks = {{Base, Base + 8, 1, {1, 2}},
              {Base + 8, Base + 12, 2},
              {Base + 12, Base + 16, 3}};
  FlowNode N;
  N.Opcode = "INT_LESS";
  N.ResultBytes = 1;
  N.Block = 0;
  N.Address = Base;
  N.OriginSequence = 0;
  auto Parameter = constant(0);
  Parameter.Type = FlowOperand::Kind::Parameter;
  N.Inputs = {Parameter, constant(Limit)};
  G.Nodes.push_back(N);
  N.Opcode = "COND_BR";
  N.ResultBytes = 0;
  N.Address = Base + 4;
  N.OriginSequence = 1;
  N.Inputs = {node(0)};
  G.Nodes.push_back(N);
  for (uint32_t I = 0; I < 2; ++I) {
    N.Opcode = "RETURN";
    N.Block = I + 1;
    N.Address = Base + 8 + 4 * I;
    N.OriginSequence = I + 2;
    N.Inputs = {constant(I ? uint32_t(-1) : 0)};
    G.Nodes.push_back(N);
  }
  return G;
}
Program program(const DataFlowGraph &G) {
  ProgramIdentity Id;
  Id.Architecture = "x86_64";
  Id.ABI = G.ABI;
  Id.Revision = G.Revision;
  Id.SHA256 = std::string(64, 'a');
  Id.Bits = 64;
  Id.Endian = "little";
  ProgramBuilder B(Id);
  B.beginFunction(G.Function, 16, "fixture", NameSource::Symbol, 0);
  for (uint32_t Block = 0; Block < G.Blocks.size(); ++Block) {
    B.addBlock(G.Blocks[Block].Begin);
    for (uint32_t I = 0; I < 4; ++I) {
      const auto &N = G.Nodes[I];
      if (N.Block != Block)
        continue;
      const auto Value = N.Inputs.back().Value;
      const auto Width = N.Inputs.back().Bytes;
      const bool Signed = N.Opcode == "INT_SLESS";
      const uint64_t Shape =
          Value + (uint64_t(Width) << 32) + (uint64_t(Signed) << 48) + I;
      B.addInstruction(
          N.Address,
          {uint8_t(I), uint8_t(Value), uint8_t(Width), uint8_t(Signed)},
          I == 0   ? "compare"
          : I == 1 ? "branch"
                   : "return",
          Shape, Shape - Value,
          I == 1  ? InstructionFlow::Branch
          : I > 1 ? InstructionFlow::Return
                  : InstructionFlow::None);
    }
  }
  B.addEdge(0, 1, EdgeKind::Taken);
  B.addEdge(0, 2, EdgeKind::NotTaken);
  B.setEntryBlock(0);
  return B.finish();
}
FunctionChangeObservations observe(const DataFlowGraph &A,
                                   const DataFlowGraph &B,
                                   const ChangeObservationOptions &Options = {},
                                   const ComparisonControl &Control = {}) {
  const auto P = program(A), S = program(B);
  const auto D = compareFunctions(P, 0, S, 0, {});
  return observeFunctionChanges(P, 0, S, 0, D, &A, &B, Options, Control);
}
} // namespace

TEST(BinaryDiffObservations, PredicateConstantsKeepOriginalNodeAndRowContext) {
  const auto A = guard(0x1000), B = guard(0x8000, 9);
  const auto R = observe(A, B);
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_TRUE(R.NativeAnalysisComplete);
  EXPECT_TRUE(R.PrimarySSA && R.SecondarySSA);
  EXPECT_EQ(R.VisitedRows, R.TotalRows);
  ASSERT_EQ(R.Observations.size(), 1u);
  const auto &O = R.Observations[0];
  EXPECT_EQ(O.Row, 0u);
  EXPECT_EQ(O.PrimaryInstruction, 0u);
  EXPECT_EQ(O.SecondaryInstruction, 0u);
  EXPECT_EQ(O.NativeKind, RowKind::Constant);
  EXPECT_EQ(O.PrimaryNode, 0u);
  EXPECT_EQ(O.SecondaryNode, 0u);
  EXPECT_EQ(O.Kinds, ObserveConstant);
  EXPECT_EQ(O.PrimaryContext, ContextPredicate);
  EXPECT_EQ(O.SecondaryContext, ContextPredicate);
  EXPECT_EQ(O.Priority, neverd::limits::diff::kObservationPredicatePriority);
  EXPECT_TRUE(observe(A, guard(0x8000)).Observations.empty());
}

TEST(BinaryDiffObservations, WidthAndSignednessRemainSeparateFromLiteralBits) {
  auto A = guard(0x1000), B = guard(0x8000);
  B.Nodes[0].Opcode = "INT_SLESS";
  for (auto &Input : B.Nodes[0].Inputs)
    Input.Bytes = 8;
  const auto R = observe(A, B);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].Kinds,
            ObserveOperation | ObserveWidth | ObserveSignedness);
  EXPECT_EQ(R.Observations[0].PrimaryContext, ContextPredicate);
  EXPECT_EQ(observationKindNames(R.Observations[0].Kinds).size(), 3u);
}

TEST(BinaryDiffObservations, SyntheticPredicateUsesUnchangedNativeAnchor) {
  auto A = guard(0x1000), B = guard(0x8000);
  for (auto *G : {&A, &B}) {
    auto Expression = G->Nodes[0];
    Expression.OriginSequence = -1;
    // Synthetic addresses are hints, not original-instruction attribution.
    Expression.Address = 0;
    G->Nodes.push_back(Expression);
    G->Nodes[1].Inputs = {constant(G->Function + 8, 8), node(4)};
  }
  B.Nodes[4].Inputs.back().Value = 9;
  const auto R = observe(A, B);
  ASSERT_EQ(R.Observations.size(), 1u);
  const auto &O = R.Observations[0];
  EXPECT_EQ(O.PrimaryInstruction, 1u);
  EXPECT_EQ(O.NativeKind, RowKind::Identical);
  EXPECT_EQ(O.PrimaryAnchorNode, 1u);
  EXPECT_EQ(O.PrimaryNode, 4u);
  EXPECT_EQ(O.PrimaryContext, ContextPredicate);
  EXPECT_EQ(O.Kinds, ObserveConstant);
  B.Nodes[4].Inputs.back().Value = 17;
  EXPECT_TRUE(observe(A, B).Observations.empty());
  // No inferred transitive pairing or native attribution for a deeper rewrite.
  for (auto *G : {&A, &B}) {
    G->Nodes.push_back(G->Nodes[4]);
    G->Nodes[4].Inputs = {node(5), constant(0)};
  }
  B.Nodes[5].Inputs.back().Value = 9;
  EXPECT_TRUE(observe(A, B).Observations.empty());
}

TEST(BinaryDiffObservations, ReturnConstantsAndCallArityKeepSeparateFacts) {
  auto A = guard(0x1000), B = guard(0x8000);
  const auto P = program(A), S = program(B);
  const auto D = compareFunctions(P, 0, S, 0, {});
  B.Nodes[2].Inputs[0].Value = 7;
  auto R = observeFunctionChanges(P, 0, S, 0, D, &A, &B);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].PrimaryContext, ContextReturn);
  EXPECT_EQ(R.Observations[0].NativeKind, RowKind::Identical);
  for (auto *G : {&A, &B}) {
    auto &N = G->Nodes[0];
    N.Opcode = "CALL";
    N.CallArgumentBegin = 1;
    N.CallArgumentCount = 1;
  }
  B.Nodes[2] = A.Nodes[2];
  B.Nodes[2].Address = B.Function + 8;
  B.Nodes[0].Inputs.push_back(constant(42, 8));
  B.Nodes[0].CallArgumentCount = 2;
  R = observeFunctionChanges(P, 0, S, 0, D, &A, &B);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].Kinds, ObserveCallArguments);
  EXPECT_TRUE(R.Observations[0].PrimaryContext & ContextCallArgument);
}

TEST(BinaryDiffObservations, AmbiguousAndSyntheticOriginsNeverInventPairs) {
  auto A = guard(0x1000), B = guard(0x8000, 9);
  A.Nodes.push_back(A.Nodes[0]);
  auto R = observe(A, B);
  EXPECT_EQ(R.AmbiguousOrigins, 1u);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].PrimaryNode, NoIndex);
  EXPECT_TRUE(R.Observations[0].Kinds & ObserveNativeInstruction);
  A.Nodes.back().OriginSequence = -1;
  R = observe(A, B);
  EXPECT_EQ(R.AmbiguousOrigins, 0u);
  EXPECT_EQ(R.Observations[0].PrimaryNode, 0u);
}

TEST(BinaryDiffObservations, CallArgumentConstantsExcludeRebasedTargets) {
  auto A = guard(0x1000), B = guard(0x8000, 9);
  for (auto *G : {&A, &B}) {
    auto &N = G->Nodes[0];
    N.Opcode = "CALL";
    N.ResultBytes = 1;
    N.Inputs[0] = constant(G->Function + 0x100);
    N.CallArgumentBegin = 1;
    N.CallArgumentCount = 1;
    N.Gaps = FlowCallEffects | FlowMemoryAlias;
  }
  auto R = observe(A, B);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].Kinds, ObserveConstant);
  EXPECT_TRUE(R.Observations[0].PrimaryContext & ContextCallArgument);
  EXPECT_TRUE(R.PrimaryGaps & FlowCallEffects);
  EXPECT_TRUE(R.SecondaryGaps & FlowMemoryAlias);
  B.Nodes[0].Inputs[1].Value = A.Nodes[0].Inputs[1].Value;
  EXPECT_TRUE(observe(A, B).Observations.empty());
}

TEST(BinaryDiffObservations, DataContextsDoNotAssertMemoryOrCalleeEffects) {
  auto A = guard(0x1000), B = guard(0x8000, 9);
  for (auto *G : {&A, &B}) {
    auto &Use = G->Nodes[1];
    Use.Opcode = "STORE";
    Use.Inputs = {constant(G->Function + 0x100, 8), node(0)};
    Use.Inputs[0].Provenance =
        uint8_t(neverd::ConstantAddressProvenance::DataAddress);
    Use.Gaps = FlowMemoryAlias;
  }
  auto R = observe(A, B);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].PrimaryContext, ContextMemoryWrite);
  EXPECT_TRUE(R.PrimaryGaps & FlowMemoryAlias);
  for (auto *G : {&A, &B}) {
    auto &Use = G->Nodes[1];
    Use.Opcode = "CALL";
    Use.CallArgumentBegin = 1;
    Use.CallArgumentCount = 1;
    Use.Gaps = FlowCallEffects;
  }
  R = observe(A, B);
  ASSERT_EQ(R.Observations.size(), 1u);
  EXPECT_EQ(R.Observations[0].PrimaryContext, ContextCallArgument);
  EXPECT_TRUE(R.PrimaryGaps & FlowCallEffects);
}

TEST(BinaryDiffObservations, StopsAndMissingSSAKeepScopeExplicit) {
  const auto A = guard(0x1000), B = guard(0x8000, 9);
  const auto P = program(A), S = program(B);
  const auto D = compareFunctions(P, 0, S, 0, {});
  const auto Native = observeFunctionChanges(P, 0, S, 0, D);
  EXPECT_FALSE(Native.PrimarySSA || Native.SecondarySSA);
  ASSERT_EQ(Native.Observations.size(), 1u);
  EXPECT_TRUE(Native.Observations[0].Kinds & ObserveNativeInstruction);
  ChangeObservationOptions Options;
  Options.MaxRows = 1;
  const auto Partial = observe(A, B, Options);
  EXPECT_EQ(Partial.State, ComparisonState::ResourceLimited);
  EXPECT_EQ(Partial.VisitedRows, 1u);
  EXPECT_EQ(Partial.Observations.size(), 1u);
  Options.MaxWork = 1;
  EXPECT_EQ(observe(A, B, Options).State, ComparisonState::ResourceLimited);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  EXPECT_EQ(observe(A, B, {}, Control).State, ComparisonState::Cancelled);
  Control.Cancel = nullptr;
  Control.Deadline = std::chrono::steady_clock::now();
  EXPECT_EQ(observe(A, B, {}, Control).State, ComparisonState::TimedOut);
  auto Stopped = D;
  Stopped.State = ComparisonState::Cancelled;
  Stopped.ComputationComplete = false;
  const auto R = observeFunctionChanges(P, 0, S, 0, Stopped, &A, &B);
  EXPECT_EQ(R.State, ComparisonState::Cancelled);
  EXPECT_TRUE(R.Observations.empty());
}

TEST(BinaryDiffObservations, ForeignBindingsAndRepeatedRowsAreRefused) {
  const auto A = guard(0x1000), B = guard(0x8000, 9);
  const auto P = program(A), S = program(B);
  auto D = compareFunctions(P, 0, S, 0, {});
  auto Wrong = B;
  Wrong.Revision += "-stale";
  EXPECT_THROW(observeFunctionChanges(P, 0, S, 0, D, &A, &Wrong),
               std::invalid_argument);
  Wrong = B;
  Wrong.Nodes[0].Address += 1;
  EXPECT_THROW(observeFunctionChanges(P, 0, S, 0, D, &A, &Wrong),
               std::invalid_argument);
  D.Rows.push_back(D.Rows[0]);
  EXPECT_THROW(observeFunctionChanges(P, 0, S, 0, D, &A, &B),
               std::invalid_argument);
}
