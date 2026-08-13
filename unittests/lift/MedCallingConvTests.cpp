//===- MedCallingConvTests.cpp - MedIR calling convention tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

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

TEST(LowToMedSSA, VerifiesDisconnectedControlFlowComponents) {
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "disconnected_cfg";
  Low.Blocks.resize(2);

  Low.Blocks[0].Id = 0;
  Low.Blocks[0].StartAddr = 0x1000;
  Low.Blocks[0].EndAddr = 0x1002;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1000;
  Call.Output = NdVar::reg(x86reg::RAX, 8);
  Call.addInput(NdVar::cst(0x2000, 8));
  Low.Blocks[0].Ops.push_back(Call);
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1001;
  Return.addInput(NdVar::reg(x86reg::RAX, 8));
  Low.Blocks[0].Ops.push_back(Return);

  Low.Blocks[1].Id = 1;
  Low.Blocks[1].StartAddr = 0x1100;
  Low.Blocks[1].EndAddr = 0x1102;
  LowOp DefineCallerSaved;
  DefineCallerSaved.Opcode = NdOp::COPY;
  DefineCallerSaved.Addr = 0x1100;
  DefineCallerSaved.Output = NdVar::reg(x86reg::RCX, 8);
  DefineCallerSaved.addInput(NdVar::cst(7, 8));
  Low.Blocks[1].Ops.push_back(DefineCallerSaved);
  LowOp DetachedReturn;
  DetachedReturn.Opcode = NdOp::RETURN;
  DetachedReturn.Addr = 0x1101;
  DetachedReturn.addInput(NdVar::reg(x86reg::RCX, 8));
  Low.Blocks[1].Ops.push_back(DetachedReturn);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  EXPECT_TRUE(verifyMedFunc(Med, "test-disconnected-cfg"));
}

TEST(LowToMedSSA, ModelsItaniumLandingPadRegistersAsExceptionalLiveIns) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "itanium_landing_pad_live_ins";
  Low.ExceptionMetadata.emplace();
  Low.ExceptionMetadata->Itanium.emplace();
  Low.ExceptionMetadata->Itanium->IsCallSiteAddressForm = true;
  Low.Blocks.resize(3);
  ASSERT_GT(TRI.IntReturnRegs.size(), 1u);

  Low.Blocks[0].Id = 0;
  Low.Blocks[0].StartAddr = 0x1000;
  Low.Blocks[0].EndAddr = 0x1001;
  ExceptionalEdge ToHandler;
  ToHandler.BlockId = 2;
  ToHandler.TargetVA = 0x1100;
  ToHandler.Kind = ExceptionalEdgeKind::ItaniumCatchPad;
  Low.Blocks[0].ExceptionalSuccs.push_back(ToHandler);
  LowOp SetFrame;
  SetFrame.Opcode = NdOp::INT_ADD;
  SetFrame.Addr = 0x1000;
  SetFrame.Output = NdVar::reg(TRI.FramePointer, TRI.PointerSize);
  SetFrame.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  SetFrame.addInput(NdVar::cst(16, TRI.PointerSize));
  Low.Blocks[0].Ops.push_back(SetFrame);
  LowOp NormalReturn;
  NormalReturn.Opcode = NdOp::RETURN;
  NormalReturn.Addr = 0x1000;
  Low.Blocks[0].Ops.push_back(NormalReturn);

  Low.Blocks[1].Id = 1;
  Low.Blocks[1].StartAddr = 0x1080;
  Low.Blocks[1].EndAddr = 0x1080;

  Low.Blocks[2].Id = 2;
  Low.Blocks[2].StartAddr = 0x1100;
  Low.Blocks[2].EndAddr = 0x1103;
  ExceptionalEdge FromCall;
  FromCall.BlockId = 0;
  FromCall.TargetVA = 0x1100;
  FromCall.Kind = ExceptionalEdgeKind::ItaniumCatchPad;
  Low.Blocks[2].ExceptionalPreds.push_back(FromCall);
  NdVar NarrowException = NdVar::tmp(0x7FF0, 4);
  LowOp ReadNarrowException;
  ReadNarrowException.Opcode = NdOp::COPY;
  ReadNarrowException.Addr = 0x1100;
  ReadNarrowException.Output = NarrowException;
  ReadNarrowException.addInput(NdVar::reg(TRI.IntReturnReg, 4));
  Low.Blocks[2].Ops.push_back(ReadNarrowException);
  NdVar Pair = NdVar::tmp(0x8000, TRI.PointerSize);
  LowOp Combine;
  Combine.Opcode = NdOp::INT_ADD;
  Combine.Addr = 0x1100;
  Combine.Output = Pair;
  Combine.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Combine.addInput(NdVar::reg(TRI.IntReturnRegs[1], TRI.PointerSize));
  Low.Blocks[2].Ops.push_back(Combine);
  NdVar WithFrame = NdVar::tmp(0x8010, TRI.PointerSize);
  LowOp UseFrame;
  UseFrame.Opcode = NdOp::INT_ADD;
  UseFrame.Addr = 0x1101;
  UseFrame.Output = WithFrame;
  UseFrame.addInput(Pair);
  UseFrame.addInput(NdVar::reg(TRI.FramePointer, TRI.PointerSize));
  Low.Blocks[2].Ops.push_back(UseFrame);
  LowOp HandlerReturn;
  HandlerReturn.Opcode = NdOp::RETURN;
  HandlerReturn.Addr = 0x1102;
  HandlerReturn.addInput(WithFrame);
  Low.Blocks[2].Ops.push_back(HandlerReturn);

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  ASSERT_EQ(Med.Blocks.size(), 2u);
  ASSERT_EQ(Med.Blocks[0].ExceptionalSuccs.size(), 1u);
  EXPECT_EQ(Med.Blocks[0].ExceptionalSuccs[0].BlockId, 1);
  ASSERT_EQ(Med.Blocks[1].ExceptionalPreds.size(), 1u);
  EXPECT_EQ(Med.Blocks[1].ExceptionalPreds[0].BlockId, 0);
  bool SawException = false;
  bool SawSelector = false;
  bool SawFlowingFramePointer = false;
  bool SawHandlerFrameLiveIn = false;
  for (const MedBlock &Block : Med.Blocks) {
    for (const MedOp &Op : Block.Ops) {
      SawHandlerFrameLiveIn |=
          Block.Id == 1 && Op.Opcode == NdOp::COPY &&
          Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.FramePointer && Op.NumInputs == 1 &&
          Op.Inputs[0].Kind == MedVar::Reg &&
          Op.Inputs[0].RegOff == TRI.FramePointer;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (Op.Inputs[I].Kind == MedVar::EHException) {
          SawException = true;
          EXPECT_EQ(Op.Inputs[I].Size, TRI.PointerSize);
        }
        if (Op.Inputs[I].Kind == MedVar::EHSelector) {
          SawSelector = true;
          EXPECT_EQ(Op.Inputs[I].Size, 4u);
        }
        SawFlowingFramePointer |=
            Block.Id == 1 && Op.Inputs[I].Kind == MedVar::Reg &&
            Op.Inputs[I].RegOff == TRI.FramePointer;
      }
    }
  }
  EXPECT_TRUE(SawException);
  EXPECT_TRUE(SawSelector);
  EXPECT_TRUE(SawFlowingFramePointer);
  EXPECT_FALSE(SawHandlerFrameLiveIn);
  EXPECT_TRUE(verifyMedFunc(Med, "test-itanium-landing-pad-live-ins"));
}

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

} // namespace
