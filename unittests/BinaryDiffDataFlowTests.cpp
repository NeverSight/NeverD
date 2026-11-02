//===- BinaryDiffDataFlowTests.cpp - SSA evidence and slice invariants
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Dependency correctness under SSA renaming, scheduling, PHI edge changes,
/// incomplete effects and finite work. No graph hash is an equivalence proof.
//===----------------------------------------------------------------------===//

#include "neverd/diff/DataFlowJSON.h"
#include "neverd/diff/FlowMatch.h"
#include "neverd/diff/MedDataFlow.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/raw_ostream.h"

#include <atomic>
#include <gtest/gtest.h>

using namespace neverd;
using namespace neverd::diff;
namespace {
MedVar variable(int Id, uint16_t Bytes = 8) {
  MedVar V;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = Bytes;
  return V;
}
MedOp operation(NdOp Code, MedVar Output, std::initializer_list<MedVar> Inputs,
                uint64_t Address) {
  MedOp O;
  O.Opcode = Code;
  O.Output = Output;
  O.Addr = Address;
  O.OriginSeq = int(Address & 0xff);
  for (auto I : Inputs)
    O.addInput(I);
  return O;
}
MedFunc diamond() {
  MedFunc F;
  F.Entry = 0x1000;
  auto P = variable(0);
  P.Kind = MedVar::Param;
  F.Params.push_back(P);
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 0x10;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 0x10;
  }
  auto &Entry = F.Blocks[0];
  Entry.Succs = {1, 2};
  Entry.Ops = {operation(NdOp::INT_LESS, variable(1, 1),
                         {P, MedVar::makeConst(16, 8)}, 0x1000),
               operation(NdOp::COND_BR, {}, {variable(1, 1)}, 0x1004)};
  F.Blocks[1].Succs = F.Blocks[2].Succs = {3};
  F.Blocks[1].Ops = {operation(NdOp::INT_MULT, variable(2),
                               {P, MedVar::makeConst(2, 8)}, 0x1010),
                     operation(NdOp::BRANCH, {}, {}, 0x1014)};
  F.Blocks[2].Ops = {operation(NdOp::INT_ADD, variable(3),
                               {P, MedVar::makeConst(7, 8)}, 0x1020),
                     operation(NdOp::BRANCH, {}, {}, 0x1024)};
  PhiNode Phi;
  Phi.Output = variable(4);
  Phi.Args = {{1, variable(2)}, {2, variable(3)}};
  F.Blocks[3].Phis.push_back(Phi);
  F.Blocks[3].Ops = {operation(NdOp::RETURN, {}, {variable(4)}, 0x1030)};
  return F;
}
DataFlowGraph graph(const MedFunc &F) {
  return buildMedDataFlow(F, "snapshot-revision", "test-abi", true);
}
} // namespace

TEST(BinaryDiffDataFlow, BackwardAndForwardSlicesRetainControlAndPhiOrigins) {
  const auto G = graph(diamond());
  ASSERT_FALSE(G.validate({}));
  ASSERT_EQ(G.Nodes.size(), 8u);
  EXPECT_EQ(G.Nodes[2].Address, 0x1010u);
  EXPECT_EQ(G.Nodes[2].OriginSequence, 0x10);
  EXPECT_EQ(G.Nodes[6].OriginSequence, -1);
  EXPECT_EQ(G.Nodes[6].Address, 0u);
  EXPECT_EQ(G.Nodes[6].Inputs[0].Predecessor, 1u);
  const auto Data = sliceDataFlow(G, {2}, SliceDirection::Backward, false);
  EXPECT_EQ(Data.Nodes, (std::vector<uint32_t>{2}));
  const auto Backward = sliceDataFlow(G, {2}, SliceDirection::Backward, true);
  EXPECT_EQ(Backward.State, ComparisonState::Complete);
  EXPECT_TRUE(Backward.ControlDependencies);
  EXPECT_EQ(Backward.Gaps, 0u);
  EXPECT_EQ(Backward.Nodes, (std::vector<uint32_t>{0, 1, 2}));
  const auto Forward = sliceDataFlow(G, {0}, SliceDirection::Forward, true);
  EXPECT_EQ(Forward.Nodes, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7}));
  const auto Return = sliceDataFlow(G, {7}, SliceDirection::Backward, true);
  EXPECT_EQ(Return.Nodes, (std::vector<uint32_t>{0, 1, 2, 4, 6, 7}));
}

TEST(BinaryDiffDataFlow, KeysIgnoreSSANumbersButPreserveConstantsAndPhiEdges) {
  const auto F = diamond();
  const auto Original = fingerprintDataFlow(graph(F));
  ASSERT_EQ(Original.State, ComparisonState::Complete);
  auto Renamed = F;
  const auto Rename = [](MedVar &V) {
    if (!V.isConst() && V.Size) {
      V.Id += 100;
      V.SSAVer += 20;
    }
  };
  for (auto &P : Renamed.Params)
    Rename(P);
  for (auto &B : Renamed.Blocks) {
    for (auto &P : B.Phis) {
      Rename(P.Output);
      std::reverse(P.Args.begin(), P.Args.end());
      for (auto &[Predecessor, V] : P.Args) {
        (void)Predecessor;
        Rename(V);
      }
    }
    for (auto &O : B.Ops) {
      Rename(O.Output);
      for (unsigned I = 0; I < O.NumInputs; ++I)
        Rename(O.Inputs[I]);
      O.Addr += 0x8000;
      if (O.Opcode == NdOp::INT_MULT || O.Opcode == NdOp::INT_ADD)
        std::swap(O.Inputs[0], O.Inputs[1]);
    }
  }
  EXPECT_EQ(fingerprintDataFlow(graph(Renamed)).Keys, Original.Keys);
  auto Changed = F;
  Changed.Blocks[1].Ops[0].Inputs[1].ConstVal = 3;
  EXPECT_NE(fingerprintDataFlow(graph(Changed)).Keys.back(),
            Original.Keys.back());
  Changed = F;
  std::swap(Changed.Blocks[3].Phis[0].Args[0].second,
            Changed.Blocks[3].Phis[0].Args[1].second);
  EXPECT_NE(fingerprintDataFlow(graph(Changed)).Keys.back(),
            Original.Keys.back());
  Changed = F;
  Changed.Blocks[1].Ops[0].Inputs[1].Provenance =
      ConstantAddressProvenance::DataAddress;
  EXPECT_NE(fingerprintDataFlow(graph(Changed)).Keys[2], Original.Keys[2]);
}

TEST(BinaryDiffDataFlow, MemoryCallsAndUnresolvedValuesAreVisibleBoundaries) {
  auto F = diamond();
  auto &O = F.Blocks[1].Ops[0];
  O.Opcode = NdOp::LOAD;
  O.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  O.NumInputs = 1;
  O.Inputs[0] = variable(99);
  const auto G = graph(F);
  const auto R = sliceDataFlow(G, {2}, SliceDirection::Backward, false);
  EXPECT_NE(R.Gaps & FlowUnknownInput, 0u);
  EXPECT_NE(R.Gaps & FlowMemoryAlias, 0u);
  EXPECT_EQ(R.Nodes, (std::vector<uint32_t>{2}));
  O.Opcode = NdOp::CALL;
  O.MemoryAddressSpace = NdMemoryAddressSpace::Default;
  O.CallSiteId = 1;
  MedCallClobber Clobber;
  Clobber.CallSiteId = 1;
  Clobber.Value = variable(50);
  F.CallClobbers.push_back(Clobber);
  F.Blocks[3].Ops[0].Inputs[0] = Clobber.Value;
  const auto Call =
      sliceDataFlow(graph(F), {7}, SliceDirection::Backward, false);
  EXPECT_EQ(Call.Nodes, (std::vector<uint32_t>{2, 7}));
  EXPECT_NE(Call.Gaps & FlowCallEffects, 0u);
}

TEST(BinaryDiffDataFlow, NoExitCancellationAndWorkExhaustionStayExplicit) {
  auto F = diamond();
  F.Blocks[2].Succs = {2};
  F.Blocks[3].Phis[0].Args.resize(1);
  auto G = graph(F);
  EXPECT_NE(sliceDataFlow(G, {2}, SliceDirection::Backward, true).Gaps &
                FlowNoExit,
            0u);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  auto Fingerprints = fingerprintDataFlow(G, {}, Control);
  EXPECT_EQ(Fingerprints.State, ComparisonState::Cancelled);
  EXPECT_TRUE(Fingerprints.Keys.empty());
  EXPECT_EQ(
      sliceDataFlow(G, {2}, SliceDirection::Backward, true, {}, Control).State,
      ComparisonState::Cancelled);
  DataFlowOptions Small;
  Small.MaxWork = 20;
  EXPECT_EQ(fingerprintDataFlow(G, Small).State,
            ComparisonState::ResourceLimited);
  EXPECT_EQ(sliceDataFlow(G, {2}, SliceDirection::Backward, true, Small).State,
            ComparisonState::ResourceLimited);
  EXPECT_THROW(buildMedDataFlow(F, "r", "abi", true, {}, Control),
               std::runtime_error);
}

TEST(BinaryDiffDataFlow, NonSSAAndMalformedGraphsAreRefused) {
  auto F = diamond();
  F.SkippedSSA = true;
  EXPECT_THROW(graph(F), std::invalid_argument);
  F = diamond();
  F.Blocks[2].Ops[0].Output = F.Blocks[1].Ops[0].Output;
  EXPECT_THROW(graph(F), std::invalid_argument);
  auto G = graph(diamond());
  G.Nodes[2].Inputs[0].Type = FlowOperand::Kind::Node;
  G.Nodes[2].Inputs[0].Node = 10000;
  EXPECT_TRUE(G.validate({}));
  EXPECT_THROW(fingerprintDataFlow(G), std::invalid_argument);
  G = graph(diamond());
  G.Nodes[6].Inputs[0].Predecessor = 0;
  EXPECT_TRUE(G.validate({}));
}

TEST(BinaryDiffDataFlow, ParametersUsePhysicalIdentityAndNeverShadowClobbers) {
  MedFunc F;
  F.Entry = 0x1000;
  auto A = variable(-1), B = variable(-1);
  A.Kind = B.Kind = MedVar::Param;
  A.SSAVer = B.SSAVer = 0;
  A.RegOff = 8;
  B.RegOff = 16;
  F.Params = {A, B};
  F.Blocks.resize(1);
  auto &Block = F.Blocks[0];
  Block.Id = 42;
  Block.StartAddr = 0x1000;
  Block.EndAddr = 0x1010;
  auto Root = A;
  Root.Kind = MedVar::Reg;
  Root.Id = 7;
  Block.Ops = {operation(NdOp::COPY, Root, {Root}, 0x1000),
               operation(NdOp::RETURN, {}, {Root}, 0x1008)};
  const auto Entry = graph(F);
  EXPECT_EQ(Entry.EntryBlock, 0u);
  ASSERT_EQ(Entry.Nodes[0].Inputs.size(), 1u);
  EXPECT_EQ(Entry.Nodes[0].Inputs[0].Type, FlowOperand::Kind::Parameter);
  EXPECT_EQ(Entry.Nodes[0].Inputs[0].Value, 0u);
  EXPECT_EQ(Entry.Nodes[1].Inputs[0].Type, FlowOperand::Kind::Parameter);
  Block.Ops = {operation(NdOp::CALL, {}, {A}, 0x1000),
               operation(NdOp::RETURN, {}, {Root, B}, 0x1008)};
  Block.Ops[0].CallSiteId = 1;
  MedCallClobber Clobber;
  Clobber.CallSiteId = 1;
  Clobber.Value = Root;
  F.CallClobbers.push_back(Clobber);
  const auto Call = graph(F);
  EXPECT_EQ(Call.Nodes[0].Inputs[0].Type, FlowOperand::Kind::Parameter);
  EXPECT_EQ(Call.Nodes[0].Inputs[0].Value, 0u);
  EXPECT_EQ(Call.Nodes[1].Inputs[0].Type, FlowOperand::Kind::Node);
  EXPECT_EQ(Call.Nodes[1].Inputs[0].Node, 0u);
  EXPECT_EQ(Call.Nodes[1].Inputs[1].Type, FlowOperand::Kind::Parameter);
  EXPECT_EQ(Call.Nodes[1].Inputs[1].Value, 1u);
}

TEST(BinaryDiffDataFlow, SavedGraphsPreserveBitsOriginsAndBoundedPages) {
  auto F = diamond();
  F.Blocks[1].Ops[0].Inputs[1].ConstVal = UINT64_MAX;
  auto G = graph(F);
  G.Nodes[2].Gaps |= FlowMemoryAlias;
  std::string Saved;
  llvm::raw_string_ostream Stream(Saved);
  llvm::json::OStream Out(Stream);
  writeDataFlowGraph(Out, G);
  auto Parsed = llvm::json::parse(Saved);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_EQ(readDataFlowGraph(*Parsed), G);
  EXPECT_EQ(readDataFlowOptions(dataFlowOptionsJSON({})), DataFlowOptions{});
  auto Page = dataFlowPageJSON(G, false, 2, 1);
  EXPECT_EQ(Page.getInteger("total"), 8);
  ASSERT_EQ(Page.getArray("rows")->size(), 1u);
  const auto *Row = Page.getArray("rows")->front().getAsObject();
  EXPECT_EQ(Row->getInteger("id"), 2);
  EXPECT_EQ(Row->getString("address"), "0x1010");
  EXPECT_EQ(Row->getArray("gaps")->front().getAsString(), "memory_alias");
  EXPECT_EQ(dataFlowPageJSON(G, false, UINT64_MAX, 1).getArray("rows")->size(),
            0u);
  EXPECT_THROW(dataFlowPageJSON(G, true, 0, 0), std::invalid_argument);
  const auto Slice = sliceDataFlow(G, {7}, SliceDirection::Backward, true);
  const auto SlicePage = dataFlowSliceJSON(G, Slice, 2, 2);
  EXPECT_EQ(SlicePage.getString("semantic_validation"), "not_performed");
  EXPECT_EQ(SlicePage.getInteger("total"), Slice.Nodes.size());
  EXPECT_EQ(SlicePage.getArray("rows")->front().getAsObject()->getInteger("id"),
            Slice.Nodes[2]);
  EXPECT_EQ(dataFlowPageJSON(G, true, 1, 1)
                .getArray("rows")
                ->front()
                .getAsObject()
                ->getString("begin"),
            "0x1010");

  const auto Reject = [&](auto Mutate) {
    auto Copy = llvm::json::parse(Saved);
    ASSERT_TRUE(bool(Copy));
    Mutate(*Copy->getAsObject());
    EXPECT_THROW(readDataFlowGraph(*Copy), std::invalid_argument);
  };
  Reject([](auto &O) { O["schema_version"] = 99; });
  Reject([](auto &O) { O["node_count"] = 1; });
  Reject([](auto &O) { O["entry_block"] = -1; });
  Reject([](auto &O) { O["gap_mask"] = UINT32_MAX; });
  Reject([](auto &O) { O.erase("abi"); });
  Reject([](auto &O) {
    (*O.getArray("nodes"))[2].getAsObject()->operator[]("phi") = 1;
  });
  Reject([](auto &O) {
    (*O.getArray("nodes"))[2].getAsObject()->operator[]("origin_sequence") = -2;
  });
  Reject([](auto &O) {
    auto *Inputs = (*O.getArray("nodes"))[6].getAsObject()->getArray("inputs");
    (*(*Inputs)[0].getAsArray())[6] = 0; // Not a PHI predecessor of block 3.
  });
  Reject([](auto &O) {
    auto *Inputs = (*O.getArray("nodes"))[7].getAsObject()->getArray("inputs");
    (*(*Inputs)[0].getAsArray())[1] = 999; // Missing SSA definition.
  });
  DataFlowOptions Small;
  Small.MaxNodes = 1;
  EXPECT_THROW(readDataFlowGraph(*Parsed, Small), std::invalid_argument);
  Small = {};
  Small.MaxWork = 1;
  EXPECT_THROW(readDataFlowGraph(*Parsed, Small), std::invalid_argument);
  EXPECT_THROW(readDataFlowOptions(llvm::json::Object{{"rounds", 0}}),
               std::invalid_argument);
  EXPECT_THROW(readDataFlowOptions(llvm::json::Object{{"typo", 1}}),
               std::invalid_argument);
}

TEST(BinaryDiffDataFlow,
     RecoveredCallArgumentsKeepDependenciesAndExactOwnership) {
  MedFunc F;
  F.Entry = 0x1000;
  auto P = variable(-1), Q = variable(-1);
  P.Kind = Q.Kind = MedVar::Param;
  P.SSAVer = Q.SSAVer = 0;
  P.RegOff = 16;
  Q.RegOff = 24;
  F.Params = {P, Q};
  auto A = variable(20), B = variable(21), Return = variable(22);
  A.Kind = MedVar::Reg;
  A.RegOff = 0;
  F.Blocks.resize(1);
  auto &Block = F.Blocks[0];
  Block.Id = 7;
  Block.StartAddr = 0x1000;
  Block.EndAddr = 0x1010;
  Block.Ops = {
      operation(NdOp::INT_ADD, A, {P, MedVar::makeConst(3, 8)}, 0x1000),
      operation(NdOp::INT_MULT, B, {Q, MedVar::makeConst(7, 8)}, 0x1004),
      operation(NdOp::CALL, Return, {MedVar::makeConst(0x9000, 8), A}, 0x1008),
      operation(NdOp::RETURN, {}, {Return}, 0x100c)};
  Block.Ops[2].CallSiteId = 1;
  MedCallInfo CI;
  CI.BlockId = 7;
  CI.OpIdx = 2;
  CI.Args = {A, B};
  F.CallInfos.push_back(CI);
  MedCallClobber Clobber;
  Clobber.CallSiteId = 1;
  Clobber.Value = A;
  Clobber.Value.SSAVer = 2;
  Clobber.PreservedInput = A;
  Clobber.PreservedPrefixSize = 4;
  F.CallClobbers.push_back(Clobber);
  const auto G = graph(F);
  const auto &Call = G.Nodes[2];
  EXPECT_EQ(Call.CallArgumentBegin, 2u);
  EXPECT_EQ(Call.CallArgumentCount, 2u);
  ASSERT_EQ(Call.Inputs.size(), 5u);
  EXPECT_EQ(Call.Inputs[2].Node, 0u);
  EXPECT_EQ(Call.Inputs[3].Node, 1u);
  EXPECT_EQ(Call.Inputs[4].Node, 0u);
  const auto Backward = sliceDataFlow(G, {2}, SliceDirection::Backward, false);
  EXPECT_EQ(Backward.Nodes, (std::vector<uint32_t>{0, 1, 2}));
  EXPECT_TRUE(Backward.Gaps & FlowCallEffects);
  EXPECT_TRUE(Backward.Gaps & FlowMemoryAlias);
  EXPECT_EQ(sliceDataFlow(G, {1}, SliceDirection::Forward, false).Nodes,
            (std::vector<uint32_t>{1, 2, 3}));
  const auto Keys = fingerprintDataFlow(G);
  auto Changed = G;
  Changed.Nodes[2].CallArgumentBegin = 1;
  EXPECT_NE(fingerprintDataFlow(Changed).Keys[2], Keys.Keys[2]);
  EXPECT_EQ(matchDataFlowNeighborhood(G, 2, Keys, Changed, 2, Keys).Outcome,
            FlowMatchOutcome::Different);
  Changed.Nodes[2].CallArgumentBegin = NoIndex;
  EXPECT_TRUE(Changed.validate({}).has_value());
  Changed = G;
  Changed.Nodes[2].Opcode = "INT_ADD";
  EXPECT_TRUE(Changed.validate({}).has_value());
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  llvm::json::OStream JSON(OS);
  writeDataFlowGraph(JSON, G);
  auto Parsed = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Parsed));
  EXPECT_EQ(readDataFlowGraph(*Parsed), G);
  EXPECT_EQ(dataFlowNodeJSON(G, 2).getInteger("call_argument_count"), 2);
  (*Parsed->getAsObject())["schema_version"] = 1;
  EXPECT_THROW(readDataFlowGraph(*Parsed), std::invalid_argument);
  F.CallInfos.push_back(CI);
  EXPECT_THROW(graph(F), std::invalid_argument);
  F.CallInfos.resize(1);
  F.CallInfos[0].OpIdx = 3;
  EXPECT_THROW(graph(F), std::invalid_argument);
  F.CallInfos[0].OpIdx = 2;
  F.CallInfos[0].BlockId = -1;
  EXPECT_THROW(graph(F), std::invalid_argument);
  F.CallInfos[0] = CI;
  F.CallInfos[0].Args.clear();
  const auto Empty = graph(F);
  EXPECT_EQ(Empty.Nodes[2].CallArgumentBegin, 2u);
  EXPECT_EQ(Empty.Nodes[2].CallArgumentCount, 0u);
  F.CallInfos.clear();
  const auto Unknown = graph(F);
  EXPECT_EQ(Unknown.Nodes[2].CallArgumentBegin, NoIndex);
  EXPECT_TRUE(Unknown.Nodes[2].Gaps & FlowUnknownInput);
}

TEST(BinaryDiffDataFlow,
     NeighborhoodChecksDefsPhiArmsAndScalarFactsBeyondHashes) {
  auto A = graph(diamond());
  auto F = diamond();
  std::reverse(F.Blocks[3].Phis[0].Args.begin(),
               F.Blocks[3].Phis[0].Args.end());
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin(),
                         operation(NdOp::INT_ADD, variable(99),
                                   {F.Params[0], MedVar::makeConst(21, 8)},
                                   0x1000));
  auto B = graph(F);
  const auto AK = fingerprintDataFlow(A), BK = fingerprintDataFlow(B);
  const auto R = matchDataFlowNeighborhood(A, 7, AK, B, 8, BK);
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Matched) << R.Reason;
  EXPECT_TRUE(llvm::is_contained(R.Nodes, std::pair{7u, 8u}));
  EXPECT_TRUE(R.OpenControlFrontier);
  EXPECT_FALSE(R.PhiBlocks.empty());
  // Even intentionally colliding candidate keys cannot establish a changed
  // scalar or swapped branch-arm dependency as an exact correspondence.
  B = A;
  B.Nodes[2].Inputs[1].Value = 3;
  EXPECT_EQ(matchDataFlowNeighborhood(A, 2, AK, B, 2, AK).Outcome,
            FlowMatchOutcome::Different);
  F = diamond();
  std::swap(F.Blocks[3].Phis[0].Args[0].second,
            F.Blocks[3].Phis[0].Args[1].second);
  B = graph(F);
  EXPECT_EQ(matchDataFlowNeighborhood(A, 7, AK, B, 7, AK).Outcome,
            FlowMatchOutcome::Different);
  B = A;
  B.ABI = "other-abi";
  EXPECT_EQ(matchDataFlowNeighborhood(A, 7, AK, B, 7, AK).Outcome,
            FlowMatchOutcome::Unsupported);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const auto Stopped =
      matchDataFlowNeighborhood(A, 7, AK, A, 7, AK, {}, Control);
  EXPECT_EQ(Stopped.State, ComparisonState::Cancelled);
  EXPECT_TRUE(Stopped.Nodes.empty());
  DataFlowOptions Small;
  Small.MaxWork = 20;
  const auto Limited = matchDataFlowNeighborhood(A, 7, AK, A, 7, AK, Small);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  EXPECT_TRUE(Limited.Nodes.empty());
}

TEST(BinaryDiffDataFlow, NeighborhoodAbstainsOnSymmetricDistinctDefinitions) {
  auto F = diamond();
  auto &B = F.Blocks[1];
  auto Clone = B.Ops[0];
  Clone.Output = variable(8);
  B.Ops.insert(B.Ops.begin() + 1, Clone);
  B.Ops.insert(B.Ops.begin() + 2,
               operation(NdOp::INT_ADD, variable(9), {variable(2), variable(8)},
                         0x1012));
  const auto G = graph(F);
  const auto K = fingerprintDataFlow(G);
  const auto R = matchDataFlowNeighborhood(G, 4, K, G, 4, K);
  EXPECT_EQ(R.State, ComparisonState::Complete);
  EXPECT_EQ(R.Outcome, FlowMatchOutcome::Ambiguous);
  EXPECT_TRUE(R.Nodes.empty());
}
