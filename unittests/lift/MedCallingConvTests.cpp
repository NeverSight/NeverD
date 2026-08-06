//===- MedCallingConvTests.cpp - MedIR calling convention tests ----------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/lift/X86Regs.h"

namespace {

using namespace neverd;
using med_calling_conv_detail::computeForwardValueClosure;
using med_calling_conv_detail::containsValue;
using med_calling_conv_detail::findFirstUseSize;

MedVar reg(int Id, int SSAVer, uint16_t Size, uint64_t RegOff, Arch TheArch) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.SSAVer = SSAVer;
  V.Size = Size;
  V.RegOff = RegOff;
  V.TheArch = TheArch;
  return V;
}

MedVar temp(int Id, int SSAVer, uint16_t Size, Arch TheArch) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.SSAVer = SSAVer;
  V.Size = Size;
  V.TheArch = TheArch;
  return V;
}

MedOp unary(NdOp Opcode, MedVar Output, MedVar Input) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.addInput(Input);
  return Op;
}

MedOp binary(NdOp Opcode, MedVar Output, MedVar Left, MedVar Right) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.addInput(Left);
  Op.addInput(Right);
  return Op;
}

void addLiveIn(MedBlock &Entry, const MedVar &LiveIn) {
  Entry.Ops.push_back(unary(NdOp::COPY, LiveIn, LiveIn));
}

TEST(MedCallingConvValueClosure, ReachesReverseBlockOrderChain) {
  MedVar Seed = temp(1, 0, 4, Arch::X86);
  MedVar Mid = temp(2, 0, 4, Arch::X86);
  MedVar End = temp(3, 0, 4, Arch::X86);

  MedFunc Func;
  Func.Blocks.resize(2);
  Func.Blocks[0].Ops.push_back(unary(NdOp::COPY, End, Mid));
  Func.Blocks[1].Ops.push_back(unary(NdOp::COPY, Mid, Seed));

  auto Values =
      computeForwardValueClosure(Func, Seed, [](const MedOp &Op, unsigned I) {
        return Op.Opcode == NdOp::COPY && I == 0;
      });

  EXPECT_TRUE(containsValue(Values, Mid));
  EXPECT_TRUE(containsValue(Values, End));
}

TEST(MedCallingConvValueClosure, TerminatesOnPhiCycle) {
  MedVar Seed = temp(1, 0, 4, Arch::X86);
  MedVar Merged = temp(2, 0, 4, Arch::X86);
  MedVar Backedge = temp(3, 0, 4, Arch::X86);

  MedFunc Func;
  Func.Blocks.resize(2);
  Func.Blocks[0].Phis.push_back({Merged, {{0, Seed}, {1, Backedge}}});
  Func.Blocks[1].Ops.push_back(unary(NdOp::COPY, Backedge, Merged));

  auto Values =
      computeForwardValueClosure(Func, Seed, [](const MedOp &Op, unsigned I) {
        return Op.Opcode == NdOp::COPY && I == 0;
      });

  EXPECT_TRUE(containsValue(Values, Merged));
  EXPECT_TRUE(containsValue(Values, Backedge));
}

TEST(MedCallingConvValueClosure, StopsAtNonForwardingOperation) {
  MedVar Seed = temp(1, 0, 4, Arch::X86);
  MedVar Product = temp(2, 0, 4, Arch::X86);

  MedFunc Func;
  Func.Blocks.resize(1);
  Func.Blocks[0].Ops.push_back(
      binary(NdOp::INT_MULT, Product, Seed, MedVar::makeConst(2, 4)));

  auto Values =
      computeForwardValueClosure(Func, Seed, [](const MedOp &Op, unsigned I) {
        return Op.Opcode == NdOp::COPY && I == 0;
      });

  EXPECT_FALSE(containsValue(Values, Product));
}

TEST(MedCallingConvValueFlow, FindsNarrowUseThroughBranchPhi) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  uint64_t ParamReg = TRI.IntParamRegs.front();
  MedVar LiveIn = reg(1, 0, 4, ParamReg, TheArch);
  MedVar BranchCopy = reg(1, 1, 4, ParamReg, TheArch);
  MedVar Merged = reg(1, 2, 4, ParamReg, TheArch);

  MedFunc Func;
  Func.Blocks.resize(3);
  addLiveIn(Func.Blocks[0], LiveIn);
  Func.Blocks[1].Ops.push_back(unary(NdOp::COPY, BranchCopy, LiveIn));
  Func.Blocks[2].Phis.push_back({Merged, {{0, LiveIn}, {1, BranchCopy}}});
  Func.Blocks[2].Ops.push_back(binary(NdOp::INT_ADD,
                                      reg(1, 3, 4, ParamReg, TheArch), Merged,
                                      MedVar::makeConst(1, 4)));

  EXPECT_EQ(findFirstUseSize(Func, ParamReg, TRI), 4);
}

TEST(MedCallingConvValueFlow, BoundsLoopPhiThroughWidthViews) {
  constexpr Arch TheArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  uint64_t ParamReg = TRI.IntParamRegs.front();
  MedVar LiveIn = reg(1, 0, 8, ParamReg, TheArch);
  MedVar LowWord = temp(10, 0, 4, TheArch);
  MedVar Extended = temp(11, 0, 8, TheArch);
  MedVar Merged = temp(12, 0, 8, TheArch);
  MedVar Backedge = temp(13, 0, 8, TheArch);

  MedFunc Func;
  Func.Blocks.resize(2);
  addLiveIn(Func.Blocks[0], LiveIn);
  Func.Blocks[0].Ops.push_back(
      binary(NdOp::SUBBYTES, LowWord, LiveIn, MedVar::makeConst(0, 8)));
  Func.Blocks[0].Ops.push_back(unary(NdOp::INT_SEXT, Extended, LowWord));
  Func.Blocks[1].Phis.push_back({Merged, {{0, Extended}, {1, Backedge}}});
  Func.Blocks[1].Ops.push_back(unary(NdOp::COPY, Backedge, Merged));
  Func.Blocks[1].Ops.push_back(binary(NdOp::INT_ADD, temp(14, 0, 8, TheArch),
                                      Merged, MedVar::makeConst(1, 8)));

  EXPECT_EQ(findFirstUseSize(Func, ParamReg, TRI), 4);
}

TEST(MedCallingConvValueFlow, StopsAtRealTransformation) {
  constexpr Arch TheArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  uint64_t ParamReg = TRI.IntParamRegs.front();
  MedVar LiveIn = reg(1, 0, 8, ParamReg, TheArch);
  MedVar NarrowInput = LiveIn;
  NarrowInput.Size = 4;
  MedVar Transformed = reg(1, 1, 8, ParamReg, TheArch);

  MedFunc Func;
  Func.Blocks.resize(1);
  addLiveIn(Func.Blocks[0], LiveIn);
  Func.Blocks[0].Ops.push_back(
      binary(NdOp::INT_ADD, Transformed, NarrowInput, MedVar::makeConst(1, 4)));
  Func.Blocks[0].Ops.push_back(binary(NdOp::INT_MULT, temp(10, 0, 8, TheArch),
                                      Transformed, MedVar::makeConst(2, 8)));

  EXPECT_EQ(findFirstUseSize(Func, ParamReg, TRI), 4);
}

TEST(MedCallingConvValueFlow, LoadAddressUsesPointerWidth) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  uint64_t ParamReg = TRI.IntParamRegs.front();
  MedVar LiveIn = reg(1, 0, 8, ParamReg, TheArch);

  MedFunc Func;
  Func.Blocks.resize(1);
  addLiveIn(Func.Blocks[0], LiveIn);
  Func.Blocks[0].Ops.push_back(
      unary(NdOp::LOAD, reg(2, 0, 4, ParamReg, TheArch), LiveIn));

  EXPECT_EQ(findFirstUseSize(Func, ParamReg, TRI), TRI.PointerSize);
}

TEST(TargetRegInfo, AArch64PartiallyPreservesV8ThroughV15) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::AArch64);

  EXPECT_TRUE(TRI.isCallPreserved(TRI.VecRegBase + 9 * TRI.VecRegStride, 8));
  EXPECT_FALSE(TRI.isCallPreserved(TRI.VecRegBase + 9 * TRI.VecRegStride, 16));
  EXPECT_FALSE(TRI.isCallPreserved(TRI.VecRegBase, 8));
  EXPECT_EQ(
      TRI.callPreservedPrefixSize(TRI.VecRegBase + 9 * TRI.VecRegStride, 4), 4);
  EXPECT_EQ(
      TRI.callPreservedPrefixSize(TRI.VecRegBase + 9 * TRI.VecRegStride, 16),
      8);
  EXPECT_EQ(TRI.callPreservedPrefixSize(TRI.VecRegBase, 16), 0);
}

TEST(TargetRegInfo, ARM32PreservesD8ThroughD15Views) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::ARM);

  EXPECT_TRUE(TRI.isCallPreserved(TRI.VecRegBase + 8 * TRI.VecRegStride, 8));
  EXPECT_TRUE(TRI.isCallPreserved(TRI.VecRegBase + 8 * TRI.VecRegStride, 16));
  EXPECT_FALSE(TRI.isCallPreserved(TRI.VecRegBase + 7 * TRI.VecRegStride, 8));
  EXPECT_FALSE(TRI.isCallPreserved(TRI.VecRegBase + 6 * TRI.VecRegStride, 16));
  EXPECT_FALSE(TRI.isCallPreserved(TRI.VecRegBase + 15 * TRI.VecRegStride, 16));
}

TEST(TargetRegInfo, X86UsesSysVCalleeSavedRegisters) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);

  EXPECT_TRUE(TRI.isCallPreserved(x86reg::RBX, 4));
  EXPECT_TRUE(TRI.isCallPreserved(x86reg::RBP, 4));
  EXPECT_TRUE(TRI.isCallPreserved(x86reg::RSI, 4));
  EXPECT_TRUE(TRI.isCallPreserved(x86reg::RDI, 4));
  EXPECT_FALSE(TRI.isCallPreserved(x86reg::RAX, 4));
  EXPECT_FALSE(TRI.isCallPreserved(x86reg::RCX, 4));
  EXPECT_FALSE(TRI.isCallPreserved(x86reg::RDX, 4));
}

TEST(MedVerifier, AcceptsImplicitCallClobberDefinition) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.CallSiteId = 1;
  Call.Output = reg(1, 1, 8, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(0x1000, TRI.PointerSize));
  Func.Blocks[0].Ops.push_back(Call);
  MedVar Clobber = reg(2, 1, 8, TRI.IntParamRegs[1], TheArch);
  Func.CallClobbers.push_back({Clobber, 1});
  Func.Blocks[0].Ops.push_back(
      unary(NdOp::COPY, temp(3, 1, 8, TheArch), Clobber));

  EXPECT_TRUE(verifyMedFunc(Func, "test-call-clobber"));
}

TEST(MedVerifier, AcceptsPartiallyPreservedCallClobber) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  uint64_t Q9 = TRI.VecRegBase + 9 * TRI.VecRegStride;
  MedVar Preserved = reg(2, 0, 16, Q9, TheArch);
  MedVar Clobber = reg(2, 1, 16, Q9, TheArch);

  MedFunc Func;
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;
  Func.Blocks[0].Ops.push_back(unary(NdOp::COPY, Preserved, Preserved));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.CallSiteId = 1;
  Call.Output = reg(1, 1, 8, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(0x1000, TRI.PointerSize));
  Func.Blocks[0].Ops.push_back(Call);
  Func.CallClobbers.push_back({Clobber, 1, Preserved, 8});
  Func.Blocks[0].Ops.push_back(
      unary(NdOp::COPY, temp(3, 1, 16, TheArch), Clobber));

  EXPECT_TRUE(verifyMedFunc(Func, "test-partial-call-clobber"));
}

#ifndef NDEBUG
TEST(MedVerifier, RejectsCallClobberExplicitDefinitionCollision) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  MedVar Clobber = reg(2, 1, 8, TRI.IntParamRegs[1], TheArch);

  MedFunc Func;
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.CallSiteId = 1;
  Call.Output = reg(1, 1, 8, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(0x1000, TRI.PointerSize));
  Func.Blocks[0].Ops.push_back(Call);
  Func.Blocks[0].Ops.push_back(
      unary(NdOp::COPY, Clobber, MedVar::makeConst(0, 8)));
  Func.CallClobbers.push_back({Clobber, 1});

  EXPECT_FALSE(verifyMedFunc(Func, "test-call-clobber-collision"));
}
#endif

} // namespace
