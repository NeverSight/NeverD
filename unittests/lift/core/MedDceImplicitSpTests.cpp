//===- MedDceImplicitSpTests.cpp - Implicit call-frame liveness ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"

namespace {
using namespace neverd;

MedVar reg(int Id, int Version, uint16_t Size, uint64_t Offset) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.TheArch = Arch::X86;
  Value.Id = Id;
  Value.SSAVer = Version;
  Value.Size = Size;
  Value.RegOff = Offset;
  return Value;
}

TEST(MedDceImplicitSp, KeepsPredecessorStackPointerAtCall) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  MedFunc Func;
  Func.Blocks.resize(2);
  MedBlock &Setup = Func.Blocks[0];
  Setup.Id = 0;
  Setup.Succs = {1};
  const MedVar EntrySP = reg(1, 0, 4, TRI.StackPointer);
  const MedVar CallSP = reg(1, 1, 4, TRI.StackPointer);
  MedOp Adjust;
  Adjust.Opcode = NdOp::INT_SUB;
  Adjust.Output = CallSP;
  Adjust.addInput(EntrySP);
  Adjust.addInput(MedVar::makeConst(4, 4));
  Setup.Ops.push_back(Adjust);

  MedBlock &CallBlock = Func.Blocks[1];
  CallBlock.Id = 1;
  CallBlock.Preds = {0};
  MedOp DeadCopy;
  DeadCopy.Opcode = NdOp::COPY;
  DeadCopy.Output.Kind = MedVar::Temp;
  DeadCopy.Output.Id = 2;
  DeadCopy.Output.SSAVer = 1;
  DeadCopy.Output.Size = 4;
  DeadCopy.addInput(MedVar::makeConst(7, 4));
  CallBlock.Ops.push_back(DeadCopy);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output.Kind = MedVar::Temp;
  Call.Output.Id = 3;
  Call.Output.SSAVer = 1;
  Call.Output.Size = 4;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  CallBlock.Ops.push_back(Call);

  LowToMedConverter().runRegisterDce(Func, Arch::X86);
  ASSERT_EQ(Setup.Ops.size(), 1u);
  EXPECT_EQ(Setup.Ops.front().Output, CallSP);
  ASSERT_EQ(CallBlock.Ops.size(), 1u);
  EXPECT_EQ(CallBlock.Ops.front().Opcode, NdOp::CALL);
}
} // namespace
