//===- MedTempLivenessTests.cpp - Temporary SSA liveness --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Check temporary values crossing branches and loops.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <initializer_list>
#include <utility>

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

LowFunc loop(Arch A) {
  const auto &TRI = getTargetRegInfo(A);
  const auto W = TRI.PointerSize;
  auto Low = makeFunction(3);
  const auto T = NdVar::tmp(TmpBase, W);
  const auto C = NdVar::tmp(TmpBase + 16, 1);
  Low.FunctionTemporaries = {{T.Offset, W}, {C.Offset, 1}};
  const auto Limit =
      NdVar::reg(TRI.integerParamRegs(BinaryFormat::ELF).front(), W);
  Low.Blocks[0].Succs = {1};
  append(Low.Blocks[0], NdOp::COPY, T, {NdVar::scalar(0, W)});
  append(Low.Blocks[0], NdOp::BRANCH, {},
         {NdVar::codeAddress(Low.Blocks[1].StartAddr, W)});
  Low.Blocks[1].Preds = {0, 1};
  Low.Blocks[1].Succs = {1, 2};
  append(Low.Blocks[1], NdOp::INT_ADD, T, {T, NdVar::scalar(1, W)});
  append(Low.Blocks[1], NdOp::INT_LESS, C, {T, Limit});
  append(Low.Blocks[1], NdOp::COND_BR, {},
         {NdVar::codeAddress(Low.Blocks[1].StartAddr, W), C});
  Low.Blocks[2].Preds = {1};
  const auto R = NdVar::reg(TRI.IntReturnReg, W);
  append(Low.Blocks[2], NdOp::COPY, R, {T});
  append(Low.Blocks[2], NdOp::RETURN, {}, {R});
  return Low;
}

TEST(MedTempLiveness, LoopCarriedTemporaryRetainsItsPhi) {
  for (auto A : {Arch::X64, Arch::ARM, Arch::AArch64}) {
    auto Med = LowToMedConverter().convert(loop(A), A, BinaryFormat::ELF);
    ASSERT_FALSE(Med.SkippedSSA);
    EXPECT_TRUE(verifyMedFunc(Med, "loop-carried temporary"));
    bool Found = false;
    for (const auto &B : Med.Blocks)
      for (const auto &P : B.Phis)
        Found |= P.Output.Kind == MedVar::Temp && P.Args.size() == 2;
    EXPECT_TRUE(Found) << static_cast<unsigned>(A);
  }
}

TEST(MedTempLiveness, LocalTemporariesDoNotHideCrossBlockUses) {
  for (bool UseAfterJoin : {false, true}) {
    const auto &TRI = getTargetRegInfo(Arch::X64);
    auto Low = makeFunction(4);
    const auto Args = TRI.integerParamRegs(BinaryFormat::ELF);
    const auto A = NdVar::reg(Args[0], 8);
    const auto Address = NdVar::reg(Args[1], 8);
    const auto T = NdVar::tmp(TmpBase, 8);
    const auto C = NdVar::tmp(TmpBase + 16, 1);
    Low.FunctionTemporaries = {{T.Offset, 8}, {C.Offset, 1}};
    Low.Blocks[0].Succs = {1, 2};
    append(Low.Blocks[0], NdOp::INT_LESS, C, {A, NdVar::scalar(10, 8)});
    append(Low.Blocks[0], NdOp::COND_BR, {},
           {NdVar::codeAddress(Low.Blocks[1].StartAddr, 8), C});
    for (unsigned I : {1u, 2u}) {
      auto &B = Low.Blocks[I];
      B.Preds = {0};
      B.Succs = {3};
      append(B, NdOp::INT_ADD, T, {A, NdVar::scalar(I, 8)});
      append(B, NdOp::STORE, {}, {Address, T});
      append(B, NdOp::BRANCH, {},
             {NdVar::codeAddress(Low.Blocks[3].StartAddr, 8)});
    }
    Low.Blocks[3].Preds = {1, 2};
    const auto R = NdVar::reg(TRI.IntReturnReg, 8);
    append(Low.Blocks[3], NdOp::COPY, R, {UseAfterJoin ? T : A});
    append(Low.Blocks[3], NdOp::RETURN, {}, {R});
    auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::ELF);
    ASSERT_FALSE(Med.SkippedSSA);
    EXPECT_TRUE(verifyMedFunc(Med, "temporary diamond"));
    unsigned Phis = 0, Stores = 0;
    for (const auto &B : Med.Blocks) {
      for (const auto &P : B.Phis)
        Phis += P.Output.Kind == MedVar::Temp;
      for (const auto &O : B.Ops)
        Stores += O.Opcode == NdOp::STORE && !O.Dead;
    }
    EXPECT_EQ(Stores, 2u);
    EXPECT_EQ(Phis != 0, UseAfterJoin);
  }
}
} // namespace
