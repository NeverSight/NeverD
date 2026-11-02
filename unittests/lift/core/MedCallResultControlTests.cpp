//===- MedCallResultControlTests.cpp - Live primary result carriers -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks exact call-result projections when integer results feed control.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace neverd;
namespace {
LowFunc resultControl(bool Indirect, bool CrossBlock, bool BoundFP,
                      uint16_t IntegerBytes, bool ControlUsesResult,
                      bool LaterCall) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  LowFunc F;
  F.Entry = 0x1000;
  F.Name = "integer_control_and_vector_state";
  F.Blocks.resize(CrossBlock ? 4 : 3);
  for (unsigned I = 0; I < F.Blocks.size(); ++I) {
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = F.Entry + 0x100 * I;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 0xFF;
  }
  auto Add = [&](unsigned B, NdOp Opcode, NdVar Output,
                 std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Addr = F.Blocks[B].StartAddr + F.Blocks[B].Ops.size();
    Op.Output = Output;
    for (auto V : Inputs)
      Op.addInput(V);
    F.Blocks[B].Ops.push_back(Op);
  };
  const auto Integer = NdVar::reg(TRI.IntReturnReg, 8);
  const auto Vector = NdVar::reg(TRI.FPReturnReg, 16);
  Add(0, Indirect ? NdOp::INDIR_CALL : NdOp::CALL, BoundFP ? Vector : Integer,
      {Indirect ? NdVar::reg(TRI.FramePointer, 8) : NdVar::cst(0x2000, 8)});
  // The vector observation is live and observable. Changing the primary
  // projection must retain this exact post-call value, not the incoming XMM0.
  Add(0, NdOp::STORE, {}, {NdVar::cst(0x6000, 8), Vector});
  if (LaterCall)
    Add(0, NdOp::CALL, Integer, {NdVar::cst(0x3000, 8)});
  const unsigned Test = CrossBlock ? 1 : 0;
  const unsigned Yes = Test + 1, No = Test + 2;
  if (CrossBlock) {
    F.Blocks[0].Succs = {1};
    F.Blocks[1].Preds = {0};
    Add(0, NdOp::BRANCH, {}, {NdVar::cst(F.Blocks[1].StartAddr, 8)});
  }
  const auto Input =
      ControlUsesResult
          ? NdVar::reg(TRI.IntReturnReg, IntegerBytes)
          : NdVar::reg(TRI.integerParamRegs(BinaryFormat::COFF)[0],
                       IntegerBytes);
  Add(Test, NdOp::COPY, NdVar::tmp(50, IntegerBytes), {Input});
  Add(Test, NdOp::INT_ADD, NdVar::tmp(51, IntegerBytes),
      {NdVar::tmp(50, IntegerBytes), NdVar::cst(1, IntegerBytes)});
  Add(Test, NdOp::INT_EQUAL, NdVar::tmp(52, 1),
      {NdVar::tmp(51, IntegerBytes), NdVar::cst(0, IntegerBytes)});
  Add(Test, NdOp::COND_BR, {},
      {NdVar::cst(F.Blocks[Yes].StartAddr, 8), NdVar::tmp(52, 1)});
  F.Blocks[Test].Succs = {static_cast<int>(Yes), static_cast<int>(No)};
  for (unsigned B : {Yes, No})
    F.Blocks[B].Preds = {static_cast<int>(Test)};
  Add(Yes, NdOp::RETURN, {}, {NdVar::cst(1, 8)});
  Add(No, NdOp::RETURN, {}, {NdVar::cst(0, 8)});
  return F;
}
std::vector<const MedOp *> calls(const MedFunc &F) {
  std::vector<const MedOp *> Result;
  for (const auto &B : F.Blocks)
    for (const auto &O : B.Ops)
      if (O.Opcode == NdOp::CALL || O.Opcode == NdOp::INDIR_CALL)
        Result.push_back(&O);
  return Result;
}

TEST(MedCallResultControl, Win64KeepsTheIntegerProjectionAndVectorObservation) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  for (bool Indirect : {false, true})
    for (bool CrossBlock : {false, true})
      for (uint16_t Bytes : {4, 8}) {
        SCOPED_TRACE(Indirect);
        SCOPED_TRACE(CrossBlock);
        SCOPED_TRACE(Bytes);
        const auto M = LowToMedConverter().convert(
            resultControl(Indirect, CrossBlock, false, Bytes, true, false),
            Arch::X64, BinaryFormat::COFF);
        const auto Calls = calls(M);
        ASSERT_EQ(Calls.size(), 1u);
        EXPECT_EQ(Calls[0]->Output.RegOff, TRI.IntReturnReg);
        const auto Clobber = std::find_if(
            M.CallClobbers.begin(), M.CallClobbers.end(), [&](const auto &C) {
              return C.CallSiteId == Calls[0]->CallSiteId &&
                     C.Value.Kind == MedVar::Reg &&
                     C.Value.RegOff == TRI.FPReturnReg;
            });
        ASSERT_NE(Clobber, M.CallClobbers.end());
        bool Stored = false;
        for (const auto &B : M.Blocks)
          for (const auto &O : B.Ops)
            if (!O.Dead && O.Opcode == NdOp::STORE && O.NumInputs == 2 &&
                O.Inputs[0].isConst() && O.Inputs[0].ConstVal == 0x6000) {
              EXPECT_EQ(O.Inputs[1], Clobber->Value);
              Stored = true;
            }
        EXPECT_TRUE(Stored);
        EXPECT_TRUE(verifyMedFunc(M, "integer-primary-and-observed-vector"));
      }
}

TEST(MedCallResultControl, BoundFloatingOutputRetainsItsExistingContract) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  for (bool CrossBlock : {false, true}) {
    const auto M = LowToMedConverter().convert(
        resultControl(false, CrossBlock, true, 8, true, false), Arch::X64,
        BinaryFormat::COFF);
    const auto Calls = calls(M);
    ASSERT_EQ(Calls.size(), 1u);
    EXPECT_EQ(Calls[0]->Output.RegOff, TRI.FPReturnReg);
    EXPECT_TRUE(std::any_of(M.CallClobbers.begin(), M.CallClobbers.end(),
                            [&](const auto &C) {
                              return C.CallSiteId == Calls[0]->CallSiteId &&
                                     C.Value.RegOff == TRI.IntReturnReg;
                            }));
    EXPECT_TRUE(verifyMedFunc(M, "bound-fp-output-and-integer-control"));
  }
}

TEST(MedCallResultControl, UnrelatedOrLaterIntegerValuesDoNotClaimTheResult) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  for (bool LaterCall : {false, true})
    for (bool CrossBlock : {false, true}) {
      const auto M = LowToMedConverter().convert(
          resultControl(false, CrossBlock, false, 8, LaterCall, LaterCall),
          Arch::X64, BinaryFormat::COFF);
      const auto Calls = calls(M);
      ASSERT_EQ(Calls.size(), LaterCall ? 2u : 1u);
      EXPECT_EQ(Calls[0]->Output.RegOff, TRI.FPReturnReg);
      if (LaterCall)
        EXPECT_EQ(Calls[1]->Output.RegOff, TRI.IntReturnReg);
      EXPECT_TRUE(verifyMedFunc(M, "independent-or-clobbered-integer-control"));
    }
}
} // namespace
