//===- MedPhiPlacementTests.cpp - SSA join placement --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Check complete unique PHI placement at large joins.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <initializer_list>
#include <set>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

void append(LowBlock &B, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Addr = B.StartAddr + B.Ops.size();
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (auto V : Inputs)
    Op.addInput(V);
  B.EndAddr = Op.Addr + 1;
  B.Ops.push_back(std::move(Op));
}

LowFunc makeFunction(unsigned Blocks) {
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "ssa_temporary_flow";
  Low.Blocks.resize(Blocks);
  for (unsigned I = 0; I < Blocks; ++I) {
    Low.Blocks[I].Id = I;
    Low.Blocks[I].StartAddr = Low.Entry + 0x1000 * I;
  }
  return Low;
}

TEST(MedPhiPlacement, LargeJoinsKeepEveryLiveDefinitionExactlyOnce) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    const auto &TRI = getTargetRegInfo(A);
    const auto W = TRI.PointerSize;
    auto Low = makeFunction(4);
    const auto Arguments = TRI.integerParamRegs(BinaryFormat::ELF);
    const auto Input =
        NdVar::reg(Arguments.empty() ? TRI.IntReturnReg : Arguments.front(), W);
    const auto Condition = NdVar::tmp(TmpBase, 1);
    Low.FunctionTemporaries = {{Condition.Offset, 1}};
    Low.Blocks[0].Succs = {1, 2};
    append(Low.Blocks[0], NdOp::INT_LESS, Condition,
           {Input, NdVar::scalar(5, W)});
    append(Low.Blocks[0], NdOp::COND_BR, {},
           {NdVar::codeAddress(Low.Blocks[1].StartAddr, W), Condition});
    std::vector<NdVar> Values;
    for (unsigned I = 0; I != 128; ++I) {
      const auto Value = NdVar::tmp(TmpBase + 16 * (I + 1), W);
      Low.FunctionTemporaries.push_back({Value.Offset, W});
      Values.push_back(Value);
    }
    for (unsigned B : {1u, 2u}) {
      Low.Blocks[B].Preds = {0};
      Low.Blocks[B].Succs = {3};
      for (unsigned I = 0; I != Values.size(); ++I)
        append(Low.Blocks[B], NdOp::INT_ADD, Values[I],
               {Input, NdVar::scalar((I + 1) * B, W)});
      append(Low.Blocks[B], NdOp::BRANCH, {},
             {NdVar::codeAddress(Low.Blocks[3].StartAddr, W)});
    }
    Low.Blocks[3].Preds = {1, 2};
    const auto Result = NdVar::reg(TRI.IntReturnReg, W);
    append(Low.Blocks[3], NdOp::COPY, Result, {Values.front()});
    for (unsigned I = 1; I < Values.size(); ++I)
      append(Low.Blocks[3], NdOp::INT_ADD, Result, {Result, Values[I]});
    append(Low.Blocks[3], NdOp::RETURN, {}, {Result});
    const auto Med = LowToMedConverter().convert(Low, A, BinaryFormat::ELF);
    ASSERT_FALSE(Med.SkippedSSA);
    EXPECT_TRUE(verifyMedFunc(Med, "large live join"));
    std::set<int> Outputs;
    unsigned Count = 0;
    for (const auto &B : Med.Blocks)
      for (const auto &P : B.Phis) {
        if (P.Output.Kind != MedVar::Temp)
          continue;
        EXPECT_TRUE(Outputs.insert(P.Output.Id).second);
        ASSERT_EQ(P.Args.size(), 2u);
        const std::set<int> Incoming{P.Args[0].first, P.Args[1].first};
        EXPECT_EQ(Incoming, (std::set<int>{1, 2}));
        ++Count;
      }
    EXPECT_EQ(Count, 128u);
  }
}
} // namespace
