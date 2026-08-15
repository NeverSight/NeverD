//===- MedCallingConvTests.cpp - MedIR calling convention tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/lift/ARMRegs.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

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

TEST(TargetRegInfo, EnumeratesPartialVectorCallPreservation) {
  auto HasRange = [](llvm::ArrayRef<TargetRegisterRange> Ranges,
                     uint64_t Offset, uint16_t Bytes) {
    return llvm::any_of(Ranges, [&](const TargetRegisterRange &Range) {
      return Range.Offset == Offset && Range.Bytes == Bytes;
    });
  };

  const TargetRegInfo &A64 = getTargetRegInfo(Arch::AArch64);
  std::vector<TargetRegisterRange> A64Ranges =
      A64.callPreservedRanges(BinaryFormat::ELF);
  EXPECT_TRUE(HasRange(A64Ranges, A64.VecRegBase + 8 * A64.VecRegStride, 8));
  EXPECT_TRUE(HasRange(A64Ranges, A64.VecRegBase + 15 * A64.VecRegStride, 8));
  EXPECT_FALSE(HasRange(A64Ranges, A64.VecRegBase + 7 * A64.VecRegStride, 8));

  const TargetRegInfo &ARM = getTargetRegInfo(Arch::ARM);
  std::vector<TargetRegisterRange> ARMRanges =
      ARM.callPreservedRanges(BinaryFormat::ELF);
  EXPECT_TRUE(HasRange(ARMRanges, ARM.VecRegBase + 8 * ARM.VecRegStride, 8));
  EXPECT_TRUE(HasRange(ARMRanges, ARM.VecRegBase + 15 * ARM.VecRegStride, 8));
}

TEST(TargetRegInfo, EnumeratesWin64CallPreservation) {
  auto HasRange = [](llvm::ArrayRef<TargetRegisterRange> Ranges,
                     uint64_t Offset, uint16_t Bytes) {
    return llvm::any_of(Ranges, [&](const TargetRegisterRange &Range) {
      return Range.Offset == Offset && Range.Bytes == Bytes;
    });
  };

  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  std::vector<TargetRegisterRange> Ranges =
      TRI.callPreservedRanges(BinaryFormat::COFF);
  EXPECT_TRUE(HasRange(Ranges, x86reg::RSI, 8));
  EXPECT_TRUE(HasRange(Ranges, x86reg::RDI, 8));
  EXPECT_TRUE(HasRange(Ranges, x86reg::XMM6, 16));
  EXPECT_TRUE(HasRange(Ranges, x86reg::XMM15, 16));
  EXPECT_FALSE(HasRange(Ranges, x86reg::XMM5, 16));
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

TEST(MedABIPass, ForwardedLiveInKeepsParameterProvenance) {
  constexpr Arch TheArch = Arch::ARM;
  constexpr va_t Callee = 0x2000;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "forwarded_live_in";
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = reg(1, 0, TRI.PointerSize, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(Callee, TRI.PointerSize));
  Func.Blocks[0].Ops.push_back(Call);

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.addInput(Call.Output);
  Func.Blocks[0].Ops.push_back(Return);

  const std::map<va_t, std::string> Names{{Callee, "callee"}};
  const std::map<va_t, int> RegArity{{Callee, 2}};
  const std::map<va_t, int> TotalArity{{Callee, 2}};
  recoverCallAbi(Func, TheArch, Names, nullptr, &RegArity, &TotalArity);

  ASSERT_EQ(Func.Params.size(), 2u);
  EXPECT_EQ(Func.Params[0].Kind, MedVar::Param);
  EXPECT_EQ(Func.Params[0].RegOff, TRI.IntParamRegs[0]);
  EXPECT_EQ(Func.Params[1].Kind, MedVar::Param);
  EXPECT_EQ(Func.Params[1].RegOff, TRI.IntParamRegs[1]);
  ASSERT_EQ(Func.CallInfos.size(), 1u);
  ASSERT_EQ(Func.CallInfos[0].Args.size(), 2u);
  EXPECT_EQ(Func.CallInfos[0].Args[0].Kind, MedVar::Param);
  EXPECT_EQ(Func.CallInfos[0].Args[0].RegOff, TRI.IntParamRegs[0]);
  EXPECT_EQ(Func.CallInfos[0].Args[1].Kind, MedVar::Param);
  EXPECT_EQ(Func.CallInfos[0].Args[1].RegOff, TRI.IntParamRegs[1]);
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
      SawHandlerFrameLiveIn |= Block.Id == 1 && Op.Opcode == NdOp::COPY &&
                               Op.Output.Kind == MedVar::Reg &&
                               Op.Output.RegOff == TRI.FramePointer &&
                               Op.NumInputs == 1 &&
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
        SawFlowingFramePointer |= Block.Id == 1 &&
                                  Op.Inputs[I].Kind == MedVar::Reg &&
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

MedFunc predicatedControlFunction(llvm::StringRef Name, NdOp EffectOpcode,
                                  va_t EntryAddress = 0x1000) {
  const va_t ContinueAddress = EntryAddress + 4;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::ARM);

  MedFunc Func;
  Func.Entry = EntryAddress;
  Func.Name = Name.str();
  Func.CC = CallingConv::ARM_AAPCS;
  Func.ReturnType = NdType::makeVoid();

  MedVar GuardValue = reg(1, 0, 1, TRI.IntParamRegs[0], Arch::ARM);
  Func.Params.push_back(GuardValue);
  MedVar IndirectTarget;
  if (EffectOpcode == NdOp::INDIR_CALL || EffectOpcode == NdOp::INDIR_BR) {
    IndirectTarget = reg(2, 0, TRI.PointerSize, TRI.IntParamRegs[1], Arch::ARM);
    Func.Params.push_back(IndirectTarget);
  }
  MedVar MemoryAddress;
  MedVar StoredValue;
  if (EffectOpcode == NdOp::LOAD || EffectOpcode == NdOp::STORE) {
    MemoryAddress = reg(2, 0, TRI.PointerSize, TRI.IntParamRegs[1], Arch::ARM);
    Func.Params.push_back(MemoryAddress);
    if (EffectOpcode == NdOp::STORE) {
      StoredValue = reg(3, 0, 4, TRI.IntParamRegs[2], Arch::ARM);
      Func.Params.push_back(StoredValue);
    }
  }

  MedBlock Guarded;
  Guarded.Id = 0;
  Guarded.StartAddr = EntryAddress;
  Guarded.EndAddr = ContinueAddress;
  Guarded.Succs = {1};

  MedOp Guard;
  Guard.Opcode = NdOp::COND_BR;
  Guard.Addr = EntryAddress;
  Guard.addInput(MedVar::makeConst(ContinueAddress, TRI.PointerSize));
  Guard.addInput(GuardValue);
  Guarded.Ops.push_back(Guard);

  MedOp Effect;
  Effect.Opcode = EffectOpcode;
  Effect.Addr = EntryAddress;
  if (EffectOpcode == NdOp::CALL)
    Effect.addInput(MedVar::makeConst(0x2000, TRI.PointerSize));
  else if (EffectOpcode == NdOp::INDIR_CALL || EffectOpcode == NdOp::INDIR_BR)
    Effect.addInput(IndirectTarget);
  else if (EffectOpcode == NdOp::LOAD) {
    Effect.Output = temp(4, 0, 4, Arch::ARM);
    Effect.addInput(MemoryAddress);
  } else if (EffectOpcode == NdOp::STORE) {
    Effect.addInput(MemoryAddress);
    Effect.addInput(StoredValue);
  }
  Guarded.Ops.push_back(Effect);

  MedBlock Continued;
  Continued.Id = 1;
  Continued.StartAddr = ContinueAddress;
  Continued.EndAddr = ContinueAddress + 4;
  Continued.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ContinueAddress;
  Continued.Ops.push_back(Return);

  Func.Blocks.push_back(std::move(Guarded));
  Func.Blocks.push_back(std::move(Continued));
  return Func;
}

llvm::Function *verifiedFunction(llvm::Module &Module, llvm::StringRef Name) {
  std::string Verification;
  llvm::raw_string_ostream VerificationOS(Verification);
  EXPECT_FALSE(llvm::verifyModule(Module, &VerificationOS)) << Verification;
  llvm::Function *Function = Module.getFunction(Name);
  EXPECT_NE(Function, nullptr);
  return Function;
}

TEST(MedLLVMEmitterPredicatedControl,
     DirectAndIndirectCallsExecuteOnlyOnTheFalseGuardEdge) {
  for (NdOp Opcode : {NdOp::CALL, NdOp::INDIR_CALL}) {
    SCOPED_TRACE(ndOpName(Opcode));
    const std::string Name =
        Opcode == NdOp::CALL ? "predicated_bl" : "predicated_blx";
    MedFunc Func = predicatedControlFunction(Name, Opcode);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit(
        {Func}, Context, Name, Arch::ARM,
        Opcode == NdOp::CALL
            ? std::vector<std::pair<va_t, std::string>>{{0x2000, "callee"}}
            : std::vector<std::pair<va_t, std::string>>{});
    ASSERT_NE(Module, nullptr);
    llvm::Function *Function = verifiedFunction(*Module, Name);
    ASSERT_NE(Function, nullptr);

    auto *Guard = llvm::dyn_cast<llvm::CondBrInst>(
        Function->getEntryBlock().getTerminator());
    ASSERT_NE(Guard, nullptr);
    EXPECT_EQ(Guard->getSuccessor(0)->getName(), "bb_1")
        << "a true ARM skip guard reaches the next guest instruction";
    llvm::BasicBlock *EffectBlock = Guard->getSuccessor(1);
    EXPECT_TRUE(EffectBlock->getName().starts_with("predeffect_"));

    llvm::CallInst *Call = nullptr;
    for (llvm::Instruction &Instruction : *EffectBlock)
      if (auto *Candidate = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        Call = Candidate;
    ASSERT_NE(Call, nullptr) << "the call must survive lowering";
    EXPECT_EQ(Call->getCalledFunction() != nullptr, Opcode == NdOp::CALL);
    auto *Rejoin =
        llvm::dyn_cast<llvm::UncondBrInst>(EffectBlock->getTerminator());
    ASSERT_NE(Rejoin, nullptr);
    EXPECT_EQ(Rejoin->getSuccessor(0), Guard->getSuccessor(0));
  }
}

TEST(MedLLVMEmitterPredicatedControl,
     ConditionalReturnKeepsTheContinuationAndReturnEdgesDistinct) {
  MedFunc Func = predicatedControlFunction("predicated_bx_lr", NdOp::RETURN);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::ARM);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  auto *Guard = llvm::dyn_cast<llvm::CondBrInst>(
      Function->getEntryBlock().getTerminator());
  ASSERT_NE(Guard, nullptr);
  EXPECT_EQ(Guard->getSuccessor(0)->getName(), "bb_1");
  EXPECT_TRUE(Guard->getSuccessor(1)->getName().starts_with("predeffect_"));
  EXPECT_TRUE(
      llvm::isa<llvm::ReturnInst>(Guard->getSuccessor(1)->getTerminator()));
}

TEST(MedLLVMEmitterPredicatedControl,
     ZeroGuestAddressDoesNotEraseInstructionProvenance) {
  MedFunc Func =
      predicatedControlFunction("predicated_zero_address", NdOp::RETURN, 0);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::ARM);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  auto *Guard = llvm::dyn_cast<llvm::CondBrInst>(
      Function->getEntryBlock().getTerminator());
  ASSERT_NE(Guard, nullptr);
  EXPECT_EQ(Guard->getSuccessor(0)->getName(), "bb_1");
  EXPECT_TRUE(Guard->getSuccessor(1)->getName().starts_with("predeffect_"));
  EXPECT_TRUE(
      llvm::isa<llvm::ReturnInst>(Guard->getSuccessor(1)->getTerminator()));
}

TEST(MedLLVMEmitterPredicatedControl,
     UnresolvedConditionalIndirectBranchFailsLoudlyOnlyWhenSelected) {
  MedFunc Func =
      predicatedControlFunction("predicated_bx_unresolved", NdOp::INDIR_BR);
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::ARM);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  auto *Guard = llvm::dyn_cast<llvm::CondBrInst>(
      Function->getEntryBlock().getTerminator());
  ASSERT_NE(Guard, nullptr);
  EXPECT_EQ(Guard->getSuccessor(0)->getName(), "bb_1");
  llvm::BasicBlock *Failure = Guard->getSuccessor(1);
  EXPECT_TRUE(llvm::isa<llvm::UnreachableInst>(Failure->getTerminator()));
  bool HasTrap = false;
  for (llvm::Instruction &Instruction : *Failure)
    if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction)) {
      llvm::Function *Callee = Call->getCalledFunction();
      HasTrap |= Callee && Callee->getIntrinsicID() == llvm::Intrinsic::trap;
    }
  EXPECT_TRUE(HasTrap);
}

TEST(MedLLVMEmitterPredicatedControl,
     UntakenMemoryEffectsDoNotAccessAnySubstituteAddress) {
  for (NdOp Opcode : {NdOp::LOAD, NdOp::STORE}) {
    SCOPED_TRACE(ndOpName(Opcode));
    const std::string Name =
        Opcode == NdOp::LOAD ? "predicated_load" : "predicated_store";
    MedFunc Func = predicatedControlFunction(Name, Opcode);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({Func}, Context, Name, Arch::ARM);
    ASSERT_NE(Module, nullptr);
    llvm::Function *Function = verifiedFunction(*Module, Name);
    ASSERT_NE(Function, nullptr);

    auto *Guard = llvm::dyn_cast<llvm::CondBrInst>(
        Function->getEntryBlock().getTerminator());
    ASSERT_NE(Guard, nullptr);
    llvm::BasicBlock *SkipBlock = Guard->getSuccessor(0);
    llvm::BasicBlock *EffectBlock = Guard->getSuccessor(1);
    EXPECT_EQ(SkipBlock->getName(), "bb_1");
    EXPECT_TRUE(EffectBlock->getName().starts_with("predeffect_"));

    bool SawGuestLoad = false;
    bool SawGuestStore = false;
    for (llvm::Instruction &Instruction : *EffectBlock) {
      SawGuestLoad |= llvm::isa<llvm::LoadInst>(Instruction);
      SawGuestStore |= llvm::isa<llvm::StoreInst>(Instruction);
    }
    EXPECT_EQ(SawGuestLoad, Opcode == NdOp::LOAD);
    EXPECT_TRUE(SawGuestStore)
        << "a load result is materialized and a store performs its effect";

    for (llvm::Instruction &Instruction : *SkipBlock)
      EXPECT_FALSE((llvm::isa<llvm::LoadInst, llvm::StoreInst>(Instruction)))
          << "the untaken instruction must not touch fallback memory";
  }
}

TEST(MedLLVMEmitterPredicatedControl,
     ResolvedConditionalIndirectBranchKeepsAllTargetsAndItsSkipEdge) {
  constexpr va_t EntryAddress = 0x1000;
  constexpr va_t ContinueAddress = EntryAddress + 4;
  MedFunc Func =
      predicatedControlFunction("predicated_bx_resolved", NdOp::INDIR_BR);
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::ARM);
  MedVar Index = Func.Params[1];
  Func.Blocks[0].Succs = {1, 2, 3, 4};

  for (int Id = 2; Id <= 4; ++Id) {
    MedBlock Target;
    Target.Id = Id;
    Target.StartAddr = EntryAddress + va_t(Id) * 0x10;
    Target.EndAddr = Target.StartAddr + 4;
    Target.Preds = {0};
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Target.StartAddr;
    Target.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Target));
  }

  JumpTable Table;
  Table.InsnAddr = EntryAddress;
  Table.EntrySize = TRI.PointerSize;
  Table.IndexRegOff = static_cast<int>(Index.RegOff);
  Table.Targets = {Func.Blocks[2].StartAddr, Func.Blocks[3].StartAddr,
                   Func.Blocks[4].StartAddr};
  Table.CaseLabels = {0, 1, 2};
  Func.JumpTables.push_back(std::move(Table));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::ARM);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  auto *Guard = llvm::dyn_cast<llvm::CondBrInst>(
      Function->getEntryBlock().getTerminator());
  ASSERT_NE(Guard, nullptr);
  EXPECT_EQ(Guard->getSuccessor(0)->getName(), "bb_1")
      << "the skip edge is not a jump-table case";
  auto *Switch =
      llvm::dyn_cast<llvm::SwitchInst>(Guard->getSuccessor(1)->getTerminator());
  ASSERT_NE(Switch, nullptr);
  EXPECT_EQ(Switch->getNumCases(), 3u);
  for (const auto &Case : Switch->cases())
    EXPECT_NE(Case.getCaseSuccessor(), Guard->getSuccessor(0));
}

} // namespace
