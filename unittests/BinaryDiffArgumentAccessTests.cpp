//===- BinaryDiffArgumentAccessTests.cpp - Local addresses ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Frame layout, SSA identity, effect boundaries and bounded observation tests.
//===----------------------------------------------------------------------===//

#include "neverd/diff/FlowArgumentAccess.h"
#include "neverd/ir/low/LowIR.h"

#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include <stdexcept>
#include <tuple>
#include <vector>

using namespace neverd;
using namespace neverd::diff;
namespace {
FlowOperand node(uint32_t N, uint16_t Bytes = 8) {
  FlowOperand I;
  I.Type = FlowOperand::Kind::Node;
  I.Node = N;
  I.Bytes = Bytes;
  return I;
}
FlowOperand parameter(uint64_t P = 0, uint16_t Bytes = 8) {
  FlowOperand I;
  I.Type = FlowOperand::Kind::Parameter;
  I.Value = P;
  I.Bytes = Bytes;
  return I;
}
FlowOperand scalar(uint64_t Value, uint16_t Bytes = 8) {
  auto I = parameter(Value, Bytes);
  I.Type = FlowOperand::Kind::Constant;
  I.Provenance = uint8_t(ConstantAddressProvenance::Scalar);
  I.AddressOwner = UINT64_MAX;
  return I;
}
FlowNode op(const char *Code, uint16_t Bytes,
            std::initializer_list<FlowOperand> Inputs) {
  FlowNode N;
  N.Opcode = Code;
  N.ResultBytes = Bytes;
  N.Block = 0;
  N.Inputs = Inputs;
  return N;
}
DataFlowGraph wrapper(uint64_t First = 8, uint64_t Second = 16,
                      bool ReturnSecond = false) {
  DataFlowGraph G;
  G.Function = 0x1000;
  G.EntryBlock = 0;
  G.Revision = "owned-test";
  G.ABI = "0:x86-64";
  G.Blocks.push_back({0x1000, 0x1100, 7, {}, 0});
  FlowOperand Unknown;
  Unknown.Bytes = 8;
  G.Nodes = {op("INT_SUB", 8, {Unknown, scalar(40)}),
             op("INT_ADD", 8, {node(0), scalar(First)}),
             op("INT_ADD", 8, {scalar(Second), node(0)}),
             op("INDIR_CALL", 8, {parameter(1), parameter(), node(1), node(2)}),
             op("INT_ADD", 8, {node(0), scalar(ReturnSecond ? Second : First)}),
             op("LOAD", 8, {node(4)}),
             op("COPY", 8, {node(5)}),
             op("RETURN", 0, {node(6)})};
  G.Nodes[0].Gaps = FlowUnknownInput;
  G.Nodes[3].CallArgumentBegin = 1;
  G.Nodes[3].CallArgumentCount = 3;
  G.Nodes[3].Gaps = FlowCallEffects | FlowMemoryAlias;
  G.Nodes[5].Gaps = FlowMemoryAlias;
  for (uint32_t I = 0; I < G.Nodes.size(); ++I) {
    G.Nodes[I].Address = G.Function + I;
    G.Nodes[I].OriginSequence = 0;
  }
  return G;
}
std::vector<std::tuple<uint64_t, uint32_t, uint64_t, bool>>
observations(const DataFlowGraph &G) {
  const auto R = observeFlowArgumentAccesses(G);
  EXPECT_TRUE(R.Complete);
  std::vector<std::tuple<uint64_t, uint32_t, uint64_t, bool>> Out;
  for (const auto &A : R.Records)
    Out.emplace_back(G.Nodes[A.Call].Address, A.Argument,
                     G.Nodes[A.Access].Address, A.ReturnOperandPath);
  std::sort(Out.begin(), Out.end());
  return Out;
}
} // namespace

TEST(BinaryDiffArgumentAccess, FrameLayoutKeepsArgumentRoleAndSiblingDistinct) {
  for (const auto &[First, Second] :
       {std::pair{8ull, 16ull}, std::pair{16ull, 8ull},
        std::pair{0xfffffff8ull, 0xffffffd0ull}}) {
    const auto G = wrapper(First, Second);
    const auto Copy = G;
    const auto R = observeFlowArgumentAccesses(G);
    ASSERT_TRUE(R.Complete);
    EXPECT_EQ(R.State, ComparisonState::Complete);
    EXPECT_EQ(R.Records,
              (std::vector<FlowArgumentAccess>{{3, 1, 5, 8, false, true}}));
    EXPECT_EQ(R.Gaps, FlowUnknownInput | FlowMemoryAlias | FlowCallEffects);
    EXPECT_EQ(G, Copy);
    const auto Sibling =
        observeFlowArgumentAccesses(wrapper(First, Second, true));
    ASSERT_EQ(Sibling.Records.size(), 1u);
    EXPECT_EQ(Sibling.Records[0].Argument, 2u);
  }
}
TEST(BinaryDiffArgumentAccess, CopiesAndModularOffsetsKeepExactWidth) {
  auto G = wrapper();
  G.Nodes[1] = op("INT_SUB", 8, {node(0), scalar(UINT64_MAX - 7)});
  G.Nodes[4] = op("COPY", 8, {node(1)});
  EXPECT_EQ(observeFlowArgumentAccesses(G).Records.size(), 1u);
  G.Nodes[4].ResultBytes = 4;
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
  G = wrapper();
  for (auto &N : G.Nodes) {
    if (N.ResultBytes)
      N.ResultBytes = 4;
    for (auto &I : N.Inputs)
      I.Bytes = 4;
  }
  G.Nodes[1].Inputs[1] = scalar(0xfffffff8, 4);
  G.Nodes[4] = op("INT_SUB", 4, {node(0, 4), scalar(8, 4)});
  const auto R = observeFlowArgumentAccesses(G);
  ASSERT_EQ(R.Records.size(), 1u);
  EXPECT_EQ(R.Records[0].Bytes, 4u);
}
TEST(BinaryDiffArgumentAccess,
     DuplicateArgumentPositionsAndStoresRemainVisible) {
  auto G = wrapper(8, 8);
  G.Nodes.push_back(op("STORE", 0, {node(4), parameter(2, 4)}));
  const auto R = observeFlowArgumentAccesses(G);
  EXPECT_EQ(R.Records,
            (std::vector<FlowArgumentAccess>{{3, 1, 5, 8, false, true},
                                             {3, 1, 8, 4, true, false},
                                             {3, 2, 5, 8, false, true},
                                             {3, 2, 8, 4, true, false}}));
  FlowArgumentAccessOptions O;
  O.MaxRecords = 1;
  const auto Limited = observeFlowArgumentAccesses(G, O);
  EXPECT_FALSE(Limited.Complete);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  ASSERT_EQ(Limited.Records.size(), 1u);
  EXPECT_EQ(Limited.Records[0], R.Records[0]);
}
TEST(BinaryDiffArgumentAccess, RawUnknownsAndCallClobbersNeverUnifyRoots) {
  auto G = wrapper();
  G.Nodes[4] = G.Nodes[0];
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
  G = wrapper();
  G.Nodes[1].Inputs[0] = node(3);
  G.Nodes[4].Inputs[0] = node(3);
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
  G = wrapper();
  G.Nodes[0] = op("INTRINSIC", 8, {parameter()});
  G.Nodes[0].Gaps = FlowCallEffects | FlowMemoryAlias;
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
}
TEST(BinaryDiffArgumentAccess,
     SeparatePointerLoadsDoNotEstablishAddressEquality) {
  auto G = wrapper();
  G.Nodes[1] = op("LOAD", 8, {parameter(5)});
  G.Nodes[4] = op("LOAD", 8, {parameter(5)});
  G.Nodes[1].Gaps = G.Nodes[4].Gaps = FlowMemoryAlias;
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
  G.Nodes[4] = op("COPY", 8, {node(1)});
  const auto R = observeFlowArgumentAccesses(G);
  ASSERT_EQ(R.Records.size(), 1u);
  EXPECT_EQ(R.Records[0].Argument, 1u);
}
TEST(BinaryDiffArgumentAccess,
     AddressSpaceOrderingAndLiteralRootsStayOutsideScope) {
  for (bool Segment : {false, true}) {
    auto G = wrapper();
    if (Segment)
      G.Nodes[5].MemoryAddressSpace = 1;
    else
      G.Nodes[5].MemoryOrdering = 1;
    EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
  }
  auto G = wrapper();
  G.Nodes[1] = op("COPY", 8, {scalar(0x1234)});
  G.Nodes[4] = op("COPY", 8, {scalar(0x1234)});
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
  G = wrapper();
  G.Nodes[1].Inputs[1].Provenance =
      uint8_t(ConstantAddressProvenance::AddressFragment);
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
}
TEST(BinaryDiffArgumentAccess,
     ReturnOperandPathsPreservePhiInputsWithoutProvingExecution) {
  auto G = wrapper();
  G.Blocks[0].Successors = {1};
  G.Blocks[0].Terminator = NoIndex;
  G.Blocks.push_back({0x1100, 0x1200, 7, {}, 0});
  G.Nodes[6].Opcode = "PHI";
  G.Nodes[6].Phi = true;
  G.Nodes[6].Block = G.Nodes[7].Block = 1;
  G.Nodes[6].Inputs[0].Predecessor = 0;
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records[0].ReturnOperandPath);
  G = wrapper();
  G.Nodes[6] = op("INT_ADD", 8, {node(5), scalar(1)});
  EXPECT_FALSE(observeFlowArgumentAccesses(G).Records[0].ReturnOperandPath);
}
TEST(BinaryDiffArgumentAccess, NodeSchedulingDoesNotChooseRelationships) {
  auto G = wrapper();
  const auto Expected = observations(G);
  const auto Count = G.Nodes.size();
  std::reverse(G.Nodes.begin(), G.Nodes.end());
  for (auto &N : G.Nodes)
    for (auto &I : N.Inputs)
      if (I.Type == FlowOperand::Kind::Node)
        I.Node = Count - 1 - I.Node;
  G.Blocks[0].Terminator = Count - 1 - G.Blocks[0].Terminator;
  EXPECT_EQ(observations(G), Expected);
}
TEST(BinaryDiffArgumentAccess,
     CyclesBudgetsAndCancellationRetainExplicitState) {
  auto G = wrapper();
  G.Nodes[1] = op("COPY", 8, {node(4)});
  G.Nodes[4] = op("COPY", 8, {node(1)});
  const auto Cycle = observeFlowArgumentAccesses(G);
  EXPECT_TRUE(Cycle.Complete);
  EXPECT_TRUE(Cycle.Records.empty());
  FlowArgumentAccessOptions O;
  O.MaxWork = 1;
  const auto Limited = observeFlowArgumentAccesses(wrapper(), O);
  EXPECT_FALSE(Limited.Complete);
  EXPECT_EQ(Limited.State, ComparisonState::ResourceLimited);
  std::atomic<bool> Cancel{true};
  ComparisonControl Control;
  Control.Cancel = &Cancel;
  const auto Stopped = observeFlowArgumentAccesses(wrapper(), {}, {}, Control);
  EXPECT_FALSE(Stopped.Complete);
  EXPECT_EQ(Stopped.State, ComparisonState::Cancelled);
  G = wrapper();
  G.Nodes[3].CallArgumentCount = 100;
  EXPECT_THROW(observeFlowArgumentAccesses(G), std::invalid_argument);
  G = wrapper();
  G.Profile = LLVMDataFlowProfile.str();
  EXPECT_THROW(observeFlowArgumentAccesses(G), std::invalid_argument);
}
TEST(BinaryDiffArgumentAccess, MissingCallArgumentsDoNotGuessRegisterRoles) {
  auto G = wrapper();
  G.Nodes[3].CallArgumentBegin = NoIndex;
  G.Nodes[3].CallArgumentCount = 0;
  G.Nodes[3].Gaps |= FlowUnknownInput;
  const auto R = observeFlowArgumentAccesses(G);
  EXPECT_TRUE(R.Complete);
  EXPECT_TRUE(R.Records.empty());
  EXPECT_TRUE(R.Gaps & FlowUnknownInput);
}

TEST(BinaryDiffArgumentAccess,
     LongForwardDefinitionChainsDoNotNeedHostRecursion) {
  auto G = wrapper();
  constexpr uint32_t Copies = 10000;
  G.Nodes[4] = op("COPY", 8, {node(8)});
  for (uint32_t I = 0; I < Copies; ++I)
    G.Nodes.push_back(op("COPY", 8, {node(I + 1 == Copies ? 1 : I + 9)}));
  const auto R = observeFlowArgumentAccesses(G);
  EXPECT_TRUE(R.Complete);
  EXPECT_EQ(R.Records,
            (std::vector<FlowArgumentAccess>{{3, 1, 5, 8, false, true}}));
}

TEST(BinaryDiffArgumentAccess, UnknownAccessWidthDoesNotProduceARecord) {
  auto G = wrapper();
  G.Nodes[5].ResultBytes = 0;
  EXPECT_TRUE(observeFlowArgumentAccesses(G).Records.empty());
}
