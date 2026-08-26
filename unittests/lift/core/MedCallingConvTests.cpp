//===- MedCallingConvTests.cpp - MedIR calling convention tests ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedSwitchNorm.h"
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

MedFunc recoverDirectCallWithArgumentPhis(Arch TheArch, int ArgIdx,
                                          std::vector<MedVar> EntryValues,
                                          std::vector<PhiNode> Phis) {
  constexpr va_t Callee = 0x2000;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "argument_phi_caller";
  Func.Blocks.resize(2);
  Func.Blocks[0].Id = 0;
  Func.Blocks[0].Succs = {1};
  for (const MedVar &Value : EntryValues)
    addLiveIn(Func.Blocks[0], Value);

  MedBlock &CallBlock = Func.Blocks[1];
  CallBlock.Id = 1;
  CallBlock.Preds = {0, 1};
  CallBlock.Succs = {1};
  CallBlock.Phis = std::move(Phis);
  for (int K = 0; K < ArgIdx; ++K)
    CallBlock.Ops.push_back(
        unary(NdOp::COPY,
              reg(100 + K, 0, TRI.PointerSize, TRI.IntParamRegs[K], TheArch),
              MedVar::makeConst(K + 1, TRI.PointerSize)));

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = reg(300, 0, TRI.PointerSize, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(Callee, TRI.PointerSize));
  CallBlock.Ops.push_back(Call);

  const int Arity = ArgIdx + 1;
  const std::map<va_t, std::string> Names{{Callee, "callee"}};
  std::map<va_t, int> RegArity{{Callee, Arity}};
  std::map<va_t, int> TotalArity{{Callee, Arity}};
  recoverCallAbi(Func, TheArch, Names, nullptr, &RegArity, &TotalArity);
  return Func;
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

TEST(TargetRegInfo, SelectsIntegerArgumentRegistersFromImageFormat) {
  const TargetRegInfo &X64 = getTargetRegInfo(Arch::X64);
  const llvm::ArrayRef<uint64_t> Win64Args =
      X64.integerParamRegs(BinaryFormat::COFF);
  EXPECT_EQ(Win64Args, X64.Win64ParamRegs);
  ASSERT_GE(Win64Args.size(), 3u);
  EXPECT_EQ(Win64Args[0], x86reg::RCX);
  EXPECT_EQ(Win64Args[1], x86reg::RDX);
  EXPECT_EQ(Win64Args[2], x86reg::R8);
  EXPECT_EQ(X64.integerParamRegs(BinaryFormat::ELF), X64.IntParamRegs);
  EXPECT_EQ(X64.integerParamRegs(BinaryFormat::MachO), X64.IntParamRegs);

  const TargetRegInfo &X86 = getTargetRegInfo(Arch::X86);
  EXPECT_EQ(X86.integerParamRegs(BinaryFormat::COFF), X86.IntParamRegs);
  const TargetRegInfo &A64 = getTargetRegInfo(Arch::AArch64);
  EXPECT_EQ(A64.integerParamRegs(BinaryFormat::COFF), A64.IntParamRegs);
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

TEST(TargetRegInfo, X86DoesNotInventX64GPRContainers) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X86);

  EXPECT_EQ(TRI.FullRegWidth, 4u);
  EXPECT_FALSE(TRI.writeZeroExtends(x86reg::RAX, 4));
  EXPECT_FALSE(TRI.writeZeroExtends(x86reg::RSP, 4));
  EXPECT_EQ(TRI.findWideReg(x86reg::RAX, 4),
            std::make_pair(x86reg::RAX, uint16_t{4}));
  EXPECT_EQ(TRI.findWideReg(x86reg::RSP, 4),
            std::make_pair(x86reg::RSP, uint16_t{4}));

  // SIMD registers retain their architecture-independent XMM/YMM container
  // hierarchy even though an i386 integer register is only four bytes wide.
  EXPECT_EQ(TRI.findWideReg(x86reg::XMM0, 16),
            std::make_pair(x86reg::XMM0, uint16_t{32}));
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
  std::map<va_t, int> RegArity{{Callee, 2}};
  std::map<va_t, int> TotalArity{{Callee, 2}};
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

TEST(MedABIPass, PromotedRegisterParamsRebaseMutableStackHomes) {
  constexpr Arch TheArch = Arch::X86;
  constexpr va_t Callee = 0x2000;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "mixed_regparm_mutable_stack_forwarder";
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;
  for (int I = 0; I < 3; ++I) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.Id = I;
    Param.RegOff = kNoParamReg;
    Param.Size = TRI.PointerSize;
    Param.TheArch = TheArch;
    Func.Params.push_back(Param);
  }
  Func.MutableStackParamHomes = {{0, 4}, {2, 12}};

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = reg(1, 0, TRI.PointerSize, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(Callee, TRI.PointerSize));
  Func.Blocks[0].Ops.push_back(Call);

  const std::map<va_t, std::string> Names{{Callee, "regparm_callee"}};
  std::map<va_t, int> RegArity{{Callee, 2}};
  std::map<va_t, int> TotalArity{{Callee, 5}};
  recoverCallAbi(Func, TheArch, Names, nullptr, &RegArity, &TotalArity);

  ASSERT_EQ(Func.Params.size(), 5u);
  EXPECT_EQ(Func.Params[0].RegOff, TRI.IntParamRegs[0]);
  EXPECT_EQ(Func.Params[1].RegOff, TRI.IntParamRegs[1]);
  EXPECT_EQ(Func.MutableStackParamHomes,
            (std::vector<std::pair<int, int64_t>>{{2, 4}, {4, 12}}));
}

TEST(MedABIPass, DirectCallUsesWidestEquallySeededArgumentPhi) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  const uint64_t Arg2 = TRI.IntParamRegs[2];

  MedVar NarrowSeed = reg(20, 0, 4, Arg2, TheArch);
  MedVar NarrowBackedge = reg(20, 1, 4, Arg2, TheArch);
  MedVar NarrowPhi = reg(20, 2, 4, Arg2, TheArch);
  MedVar WideSeed = reg(21, 0, TRI.PointerSize, Arg2, TheArch);
  MedVar WideBackedge = reg(21, 1, TRI.PointerSize, Arg2, TheArch);
  MedVar WidePhi = reg(21, 2, TRI.PointerSize, Arg2, TheArch);

  // Keep the narrow alias first: argument recovery must be semantic, not
  // dependent on the PHI insertion order.
  MedFunc Func = recoverDirectCallWithArgumentPhis(
      TheArch, 2, {NarrowSeed, WideSeed},
      {{NarrowPhi, {{0, NarrowSeed}, {1, NarrowBackedge}}},
       {WidePhi, {{0, WideSeed}, {1, WideBackedge}}}});

  ASSERT_EQ(Func.CallInfos.size(), 1u);
  ASSERT_EQ(Func.CallInfos[0].Args.size(), 3u);
  EXPECT_EQ(Func.CallInfos[0].Args[2], WidePhi);
  EXPECT_EQ(Func.CallInfos[0].Args[2].Size, TRI.PointerSize);
}

TEST(MedABIPass, DirectCallPhiAliasSelectionIsSharedAcrossRegisterAbis) {
  for (Arch TheArch : {Arch::AArch64, Arch::ARM, Arch::X64, Arch::X86}) {
    SCOPED_TRACE(static_cast<int>(TheArch));
    const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
    ASSERT_GE(TRI.IntParamRegs.size(), 2u);
    const uint64_t Arg1 = TRI.IntParamRegs[1];
    const uint16_t NarrowSize = TRI.PointerSize / 2;

    MedVar NarrowSeed = reg(20, 0, NarrowSize, Arg1, TheArch);
    MedVar NarrowBackedge = reg(20, 1, NarrowSize, Arg1, TheArch);
    MedVar NarrowPhi = reg(20, 2, NarrowSize, Arg1, TheArch);
    MedVar WideSeed = reg(21, 0, TRI.PointerSize, Arg1, TheArch);
    MedVar WideBackedge = reg(21, 1, TRI.PointerSize, Arg1, TheArch);
    MedVar WidePhi = reg(21, 2, TRI.PointerSize, Arg1, TheArch);

    MedFunc Func = recoverDirectCallWithArgumentPhis(
        TheArch, 1, {NarrowSeed, WideSeed},
        {{NarrowPhi, {{0, NarrowSeed}, {1, NarrowBackedge}}},
         {WidePhi, {{0, WideSeed}, {1, WideBackedge}}}});

    ASSERT_EQ(Func.CallInfos.size(), 1u);
    ASSERT_EQ(Func.CallInfos[0].Args.size(), 2u);
    EXPECT_EQ(Func.CallInfos[0].Args[1], WidePhi);
    EXPECT_EQ(Func.CallInfos[0].Args[1].Size, TRI.PointerSize);
  }
}

TEST(MedABIPass, DirectCallPrefersSeededArgumentPhiOverWiderUndefPhi) {
  constexpr Arch TheArch = Arch::AArch64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  const uint64_t Arg2 = TRI.IntParamRegs[2];

  MedVar NarrowSeed = reg(20, 0, 4, Arg2, TheArch);
  MedVar NarrowBackedge = reg(20, 1, 4, Arg2, TheArch);
  MedVar NarrowPhi = reg(20, 2, 4, Arg2, TheArch);
  MedVar WideUndef = reg(21, 0, TRI.PointerSize, Arg2, TheArch);
  MedVar WideBackedge = reg(21, 1, TRI.PointerSize, Arg2, TheArch);
  MedVar WidePhi = reg(21, 2, TRI.PointerSize, Arg2, TheArch);

  // Keep the unsafe wide alias first to prove that an entry-defined narrow
  // value wins over a wider view that is undef on the first iteration.
  MedFunc Func = recoverDirectCallWithArgumentPhis(
      TheArch, 2, {NarrowSeed},
      {{WidePhi, {{0, WideUndef}, {1, WideBackedge}}},
       {NarrowPhi, {{0, NarrowSeed}, {1, NarrowBackedge}}}});

  ASSERT_EQ(Func.CallInfos.size(), 1u);
  ASSERT_EQ(Func.CallInfos[0].Args.size(), 3u);
  EXPECT_EQ(Func.CallInfos[0].Args[2], NarrowPhi);
  EXPECT_EQ(Func.CallInfos[0].Args[2].Size, 4u);
}

TEST(LowToMedCallReturnFP, DoesNotCrossCallClobber) {
  constexpr Arch TheArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "fp_return_call_barrier";

  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  Block.EndAddr = 0x1018;

  const NdVar WideFP = NdVar::reg(x86reg::XMM0, 16);
  LowOp Seed;
  Seed.Opcode = NdOp::COPY;
  Seed.Addr = 0x1000;
  Seed.Output = WideFP;
  Seed.addInput(WideFP);
  Block.Ops.push_back(Seed);

  auto makeCall = [](va_t Addr, va_t Target) {
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = Addr;
    Call.Output = NdVar::reg(x86reg::RAX, 8);
    Call.addInput(NdVar::cst(Target, 8));
    return Call;
  };
  Block.Ops.push_back(makeCall(0x1004, 0x2000));
  Block.Ops.push_back(makeCall(0x1008, 0x3000));

  LowOp ReadSecondResult;
  ReadSecondResult.Opcode = NdOp::COPY;
  ReadSecondResult.Addr = 0x100c;
  ReadSecondResult.Output = NdVar::tmp(0x4000, 8);
  ReadSecondResult.addInput(NdVar::reg(x86reg::XMM0, 8));
  Block.Ops.push_back(ReadSecondResult);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1010;
  Return.addInput(ReadSecondResult.Output);
  Block.Ops.push_back(Return);
  Low.Blocks.push_back(std::move(Block));

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  std::vector<const MedOp *> Calls;
  for (const MedOp &Op : Med.Blocks.front().Ops)
    if (Op.Opcode == NdOp::CALL)
      Calls.push_back(&Op);
  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(Calls[0]->Output.RegOff, TRI.IntReturnReg);
  EXPECT_EQ(Calls[1]->Output.RegOff, TRI.FPReturnReg);
}

TEST(MedCallAbi, ExactVectorReturnFeedsFollowingFPCall) {
  constexpr Arch TheArch = Arch::X64;
  constexpr va_t Callee = 0x2000;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);
  ASSERT_GE(TRI.FPParamRegs.size(), 2u);

  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "fp_return_chain";
  Func.Blocks.resize(1);
  MedBlock &Block = Func.Blocks[0];
  Block.Id = 0;

  const MedVar InitialFP0 = reg(10, 0, 16, TRI.FPParamRegs[0], TheArch);
  const MedVar InitialFP1 = reg(11, 0, 16, TRI.FPParamRegs[1], TheArch);
  addLiveIn(Block, InitialFP0);
  addLiveIn(Block, InitialFP1);

  MedOp FirstCall;
  FirstCall.Opcode = NdOp::CALL;
  FirstCall.Addr = 0x1004;
  FirstCall.CallSiteId = 1;
  FirstCall.Output = reg(20, 1, 8, TRI.IntReturnReg, TheArch);
  FirstCall.addInput(MedVar::makeConst(Callee, TRI.PointerSize));
  Block.Ops.push_back(FirstCall);
  const MedVar FirstFPResult = reg(10, 1, 16, TRI.FPReturnReg, TheArch);
  const MedVar FirstTooNarrow = reg(12, 1, 4, TRI.FPReturnReg, TheArch);
  const MedVar FirstTooWide = reg(13, 1, 32, TRI.FPReturnReg, TheArch);
  Func.CallClobbers.push_back({FirstTooNarrow, FirstCall.CallSiteId});
  Func.CallClobbers.push_back({FirstTooWide, FirstCall.CallSiteId});
  Func.CallClobbers.push_back({FirstFPResult, FirstCall.CallSiteId});

  const MedVar SecondFP1 = reg(11, 1, 16, TRI.FPParamRegs[1], TheArch);
  Block.Ops.push_back(unary(NdOp::COPY, SecondFP1, InitialFP1));

  MedOp SecondCall;
  SecondCall.Opcode = NdOp::CALL;
  SecondCall.Addr = 0x1008;
  SecondCall.CallSiteId = 2;
  SecondCall.Output = reg(20, 2, 8, TRI.IntReturnReg, TheArch);
  SecondCall.addInput(MedVar::makeConst(Callee, TRI.PointerSize));
  Block.Ops.push_back(SecondCall);
  const MedVar SecondFPResult = reg(10, 2, 16, TRI.FPReturnReg, TheArch);
  Func.CallClobbers.push_back({SecondFPResult, SecondCall.CallSiteId});

  const std::map<va_t, std::string> Names{{Callee, "fp_callee"}};
  const std::map<va_t, int> FPArity{{Callee, 2}};
  const std::map<va_t, uint16_t> FPReturnSize{{Callee, 8}};
  const std::map<va_t, std::vector<uint64_t>> FPRegs{
      {Callee, {TRI.FPParamRegs[0], TRI.FPParamRegs[1]}}};
  recoverCallAbi(Func, TheArch, Names, nullptr, nullptr, nullptr, &FPArity,
                 &FPReturnSize, &FPRegs);

  ASSERT_EQ(Func.CallInfos.size(), 2u);
  ASSERT_EQ(Func.CallInfos[1].Args.size(), 2u);
  EXPECT_EQ(Func.CallInfos[1].Args[0], FirstFPResult);
  EXPECT_EQ(Func.Blocks[0].Ops[2].Output, FirstFPResult);
  EXPECT_EQ(Func.Blocks[0].Ops[4].Output, SecondFPResult);
  ASSERT_EQ(Func.CallClobbers.size(), 2u);
  EXPECT_NE(std::find_if(Func.CallClobbers.begin(), Func.CallClobbers.end(),
                         [&](const MedCallClobber &C) {
                           return C.Value == FirstTooNarrow;
                         }),
            Func.CallClobbers.end());
  EXPECT_NE(std::find_if(Func.CallClobbers.begin(), Func.CallClobbers.end(),
                         [&](const MedCallClobber &C) {
                           return C.Value == FirstTooWide;
                         }),
            Func.CallClobbers.end());
  EXPECT_TRUE(verifyMedFunc(Func, "test-exact-vector-return-chain"));
}

TEST(MedCallAbi, IntegerReturnKeepsFPCallClobber) {
  constexpr Arch TheArch = Arch::X64;
  constexpr va_t Callee = 0x3000;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "integer_return_with_fp_clobber";
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;

  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1000;
  Call.CallSiteId = 1;
  Call.Output = reg(20, 1, 8, TRI.IntReturnReg, TheArch);
  Call.addInput(MedVar::makeConst(Callee, TRI.PointerSize));
  Func.Blocks[0].Ops.push_back(Call);
  const MedVar FPClobber = reg(10, 1, 16, TRI.FPReturnReg, TheArch);
  Func.CallClobbers.push_back({FPClobber, Call.CallSiteId});

  const std::map<va_t, std::string> Names{{Callee, "integer_callee"}};
  const std::map<va_t, uint16_t> FPReturnSize{{Callee, 0}};
  recoverCallAbi(Func, TheArch, Names, nullptr, nullptr, nullptr, nullptr,
                 &FPReturnSize);

  EXPECT_EQ(Func.Blocks[0].Ops[0].Output.RegOff, TRI.IntReturnReg);
  ASSERT_EQ(Func.CallClobbers.size(), 1u);
  EXPECT_EQ(Func.CallClobbers[0].Value, FPClobber);
  EXPECT_TRUE(verifyMedFunc(Func, "test-integer-return-fp-clobber"));
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

TEST(LowToMedX86CallingConv,
     StackAddressExtensionDoesNotCreateMutableParameterHome) {
  constexpr Arch TheArch = Arch::X86;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "extended_stack_parameter_address";
  Low.Blocks.resize(1);
  LowBlock &Block = Low.Blocks[0];
  Block.Id = 0;
  Block.StartAddr = 0x1000;
  Block.EndAddr = 0x1004;

  NdVar Address32 = NdVar::tmp(1, TRI.PointerSize);
  LowOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Addr = 0x1000;
  Add.Output = Address32;
  Add.addInput(NdVar::reg(TRI.StackPointer, TRI.PointerSize));
  Add.addInput(NdVar::cst(4, TRI.PointerSize));
  Block.Ops.push_back(Add);

  NdVar Address64 = NdVar::tmp(2, 8);
  LowOp Extend;
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.Addr = 0x1001;
  Extend.Output = Address64;
  Extend.addInput(Address32);
  Block.Ops.push_back(Extend);

  NdVar Value = NdVar::tmp(3, 4);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = 0x1002;
  Load.Output = Value;
  Load.addInput(Address64);
  Block.Ops.push_back(Load);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1003;
  Return.addInput(Value);
  Block.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  ASSERT_EQ(Med.Params.size(), 1u);
  EXPECT_EQ(Med.Params[0].Kind, MedVar::Param);
  EXPECT_EQ(Med.Params[0].Id, 0);
  EXPECT_EQ(Med.Params[0].RegOff, kNoParamReg);
  EXPECT_TRUE(Med.MutableStackParamHomes.empty());

  bool SawParameterCopy = false;
  for (const MedBlock &MedBlock : Med.Blocks)
    for (const MedOp &Op : MedBlock.Ops) {
      SawParameterCopy |= Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                          Op.Inputs[0].Kind == MedVar::Param &&
                          Op.Inputs[0].Id == 0;
      EXPECT_NE(Op.Opcode, NdOp::LOAD);
    }
  EXPECT_TRUE(SawParameterCopy);
  EXPECT_TRUE(verifyMedFunc(Med, "test-extended-stack-parameter-address"));
}

MedFunc convertTwoCallFPFlow(NdOp SecondOpcode, bool ReadInSuccessor,
                             bool PreserveSecondCall) {
  constexpr Arch TheArch = Arch::X64;
  constexpr va_t SecondTarget = 0x3000;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "two_call_fp_result";
  Low.Blocks.resize(ReadInSuccessor ? 2 : 1);
  Low.Blocks[0].Id = 0;
  Low.Blocks[0].StartAddr = 0x1000;
  Low.Blocks[0].EndAddr = 0x1003;
  if (ReadInSuccessor) {
    Low.Blocks[0].Succs = {1};
    Low.Blocks[1].Id = 1;
    Low.Blocks[1].StartAddr = 0x1010;
    Low.Blocks[1].EndAddr = 0x1013;
    Low.Blocks[1].Preds = {0};
  }

  LowOp FirstCall;
  FirstCall.Opcode = NdOp::CALL;
  FirstCall.Addr = 0x1000;
  FirstCall.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  FirstCall.addInput(NdVar::cst(0x2000, TRI.PointerSize));
  Low.Blocks[0].Ops.push_back(FirstCall);

  NdVar FirstResult = NdVar::tmp(100, TRI.PointerSize);
  if (!PreserveSecondCall) {
    LowOp SaveFirstResult;
    SaveFirstResult.Opcode = NdOp::COPY;
    SaveFirstResult.Addr = 0x1001;
    SaveFirstResult.Output = FirstResult;
    SaveFirstResult.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
    Low.Blocks[0].Ops.push_back(SaveFirstResult);
  }

  LowOp SecondCall;
  SecondCall.Opcode = SecondOpcode;
  SecondCall.Addr = 0x1002;
  SecondCall.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  SecondCall.addInput(NdVar::cst(SecondTarget, TRI.PointerSize));
  Low.Blocks[0].Ops.push_back(SecondCall);

  NdVar FPBits = NdVar::tmp(101, TRI.PointerSize);
  LowOp ExtractFPBits;
  ExtractFPBits.Opcode = NdOp::SUBBYTES;
  ExtractFPBits.Addr = ReadInSuccessor ? 0x1010 : 0x1003;
  ExtractFPBits.Output = FPBits;
  ExtractFPBits.addInput(NdVar::reg(TRI.FPReturnReg, 16));
  ExtractFPBits.addInput(NdVar::cst(0, TRI.PointerSize));
  LowBlock &UseBlock = Low.Blocks[ReadInSuccessor ? 1 : 0];
  UseBlock.Ops.push_back(ExtractFPBits);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  if (PreserveSecondCall) {
    Return.addInput(FPBits);
  } else {
    NdVar Combined = NdVar::tmp(102, TRI.PointerSize);
    LowOp Combine;
    Combine.Opcode = NdOp::INT_ADD;
    Combine.Addr = ReadInSuccessor ? 0x1011 : 0x1004;
    Combine.Output = Combined;
    Combine.addInput(FirstResult);
    Combine.addInput(FPBits);
    UseBlock.Ops.push_back(Combine);
    Return.addInput(Combined);
  }
  Return.Addr = ReadInSuccessor ? 0x1012 : 0x1005;
  UseBlock.Ops.push_back(Return);

  const std::set<va_t> PreservingTargets =
      PreserveSecondCall ? std::set<va_t>{SecondTarget} : std::set<va_t>{};
  LowToMedConverter Converter;
  Converter.setStackProbeSlots(&PreservingTargets);
  return Converter.convert(Low, TheArch);
}

std::vector<const MedOp *> callOps(const MedFunc &Med) {
  std::vector<const MedOp *> Calls;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL)
        Calls.push_back(&Op);
  return Calls;
}

TEST(LowToMedCallReturnFP, LaterCallOwnsTheSubsequentFPResult) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  MedFunc Med = convertTwoCallFPFlow(NdOp::CALL, false, false);
  std::vector<const MedOp *> Calls = callOps(Med);

  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(Calls[0]->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Calls[0]->Output.RegOff, TRI.IntReturnReg);
  EXPECT_EQ(Calls[1]->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Calls[1]->Output.RegOff, TRI.FPReturnReg);
  EXPECT_TRUE(verifyMedFunc(Med, "test-two-call-fp-result"));
}

TEST(LowToMedCallReturnFP,
     LaterIndirectCallOwnsFPResultReadInDominatedSuccessor) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  MedFunc Med = convertTwoCallFPFlow(NdOp::INDIR_CALL, true, false);
  std::vector<const MedOp *> Calls = callOps(Med);

  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(Calls[0]->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Calls[0]->Output.RegOff, TRI.IntReturnReg);
  EXPECT_EQ(Calls[1]->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Calls[1]->Output.RegOff, TRI.FPReturnReg);
  EXPECT_TRUE(verifyMedFunc(Med, "test-cross-block-two-call-fp-result"));
}

TEST(LowToMedCallReturnFP, CallerSavedPreservingCallKeepsPriorFPResult) {
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);
  MedFunc Med = convertTwoCallFPFlow(NdOp::CALL, false, true);
  std::vector<const MedOp *> Calls = callOps(Med);

  ASSERT_EQ(Calls.size(), 2u);
  EXPECT_EQ(Calls[0]->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Calls[0]->Output.RegOff, TRI.FPReturnReg);
  EXPECT_TRUE(Calls[1]->PreservesCallerSaved);
  EXPECT_EQ(Calls[1]->Output.Size, 0u);
  EXPECT_FALSE(std::any_of(
      Med.CallClobbers.begin(), Med.CallClobbers.end(),
      [&](const MedCallClobber &Clobber) {
        return Clobber.CallSiteId == Calls[1]->CallSiteId &&
               Clobber.Value.Kind == MedVar::Reg &&
               Clobber.Value.RegOff == TRI.FPReturnReg;
      }));
  EXPECT_TRUE(verifyMedFunc(Med, "test-preserved-fp-result"));
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

TEST(MedLLVMReturn, UsesLatestAliasedDefinitionBeforeCoercion) {
  constexpr Arch TheArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x5000;
  Func.Name = "latest_return_alias";
  Func.ReturnType = NdType::makeInt(8, false);
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.EndAddr = Func.Entry + 12;

  MedOp Older;
  Older.Opcode = NdOp::COPY;
  Older.Output = reg(1, 1, 8, TRI.IntReturnReg, TheArch);
  Older.addInput(MedVar::makeConst(0x1122334455667788ULL, 8));
  Block.Ops.push_back(Older);

  MedOp Newer;
  Newer.Opcode = NdOp::COPY;
  Newer.Output = reg(2, 1, 4, TRI.IntReturnReg, TheArch);
  Newer.addInput(MedVar::makeConst(0x55667788, 4));
  Block.Ops.push_back(Newer);

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry + 8;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, TheArch);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  const llvm::ReturnInst *ReturnInst = nullptr;
  bool HasZExt = false;
  for (const llvm::BasicBlock &BB : *Function)
    for (const llvm::Instruction &Instruction : BB) {
      if (auto *Candidate = llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
        ReturnInst = Candidate;
      HasZExt |= llvm::isa<llvm::ZExtInst>(&Instruction);
    }
  ASSERT_NE(ReturnInst, nullptr);
  EXPECT_TRUE(HasZExt);
}

TEST(MedLLVMReturn, UsesWidestAuthoritativeIntegerReturnPhi) {
  constexpr Arch TheArch = Arch::X64;
  constexpr va_t EntryAddress = 0x5080;
  constexpr va_t LeftAddress = EntryAddress + 0x10;
  constexpr va_t RightAddress = EntryAddress + 0x20;
  constexpr va_t ReturnAddress = EntryAddress + 0x30;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = EntryAddress;
  Func.Name = "widest_integer_return_phi";
  Func.ReturnType = NdType::makeInt(8, false);
  MedVar Condition = reg(1, 0, 8, x86reg::RDI, TheArch);
  Func.Params.push_back(Condition);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = EntryAddress;
  Entry.EndAddr = EntryAddress + 1;
  Entry.Succs = {1, 2};
  MedOp Split;
  Split.Opcode = NdOp::COND_BR;
  Split.addInput(MedVar::makeConst(LeftAddress, 8));
  Split.addInput(Condition);
  Entry.Ops.push_back(std::move(Split));

  auto makePredecessor = [&](int Id, va_t Address) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 1;
    Block.Preds = {0};
    Block.Succs = {3};
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.addInput(MedVar::makeConst(ReturnAddress, 8));
    Block.Ops.push_back(std::move(Branch));
    return Block;
  };

  MedBlock ReturnBlock;
  ReturnBlock.Id = 3;
  ReturnBlock.StartAddr = ReturnAddress;
  ReturnBlock.EndAddr = ReturnAddress + 1;
  ReturnBlock.Preds = {1, 2};

  PhiNode Narrow;
  Narrow.Output = reg(2, 1, 4, TRI.IntReturnReg, TheArch);
  Narrow.Args = {{1, MedVar::makeConst(3, 4)}, {2, MedVar::makeConst(4, 4)}};
  ReturnBlock.Phis.push_back(std::move(Narrow));

  PhiNode Wide;
  Wide.Output = reg(3, 1, 8, TRI.IntReturnReg, TheArch);
  Wide.Args = {{1, MedVar::makeConst(UINT64_C(0x100000003), 8)},
               {2, MedVar::makeConst(UINT64_C(0x200000004), 8)}};
  ReturnBlock.Phis.push_back(std::move(Wide));

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ReturnAddress;
  ReturnBlock.Ops.push_back(Return);

  Func.Blocks = {std::move(Entry), makePredecessor(1, LeftAddress),
                 makePredecessor(2, RightAddress), std::move(ReturnBlock)};

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, TheArch);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  const llvm::ReturnInst *ReturnInst = nullptr;
  for (const llvm::BasicBlock &BB : *Function)
    for (const llvm::Instruction &Instruction : BB)
      if (auto *Candidate = llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
        ReturnInst = Candidate;
  ASSERT_NE(ReturnInst, nullptr);
  ASSERT_NE(ReturnInst->getReturnValue(), nullptr);
  EXPECT_FALSE(llvm::isa<llvm::ZExtInst>(ReturnInst->getReturnValue()))
      << "a preceding EAX phi must not hide the loop-carried RAX value";
}

TEST(MedLLVMReturn, UsesLatestFloatDefinitionBeforeWidthPreference) {
  constexpr Arch TheArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x5100;
  Func.Name = "latest_float_return_alias";
  Func.ReturnType = NdType::makeFloat(4);
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.EndAddr = Func.Entry + 12;

  MedOp Older;
  Older.Opcode = NdOp::COPY;
  Older.Output = reg(1, 1, 16, TRI.fpReturnModelReg(), TheArch);
  Older.addInput(MedVar::makeConst(0x1122334455667788ULL, 16));
  Block.Ops.push_back(Older);

  MedOp Newer;
  Newer.Opcode = NdOp::COPY;
  Newer.Output = reg(2, 1, 4, TRI.fpReturnModelReg(), TheArch);
  Newer.addInput(MedVar::makeConst(0x3f800000ULL, 4));
  Block.Ops.push_back(Newer);

  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry + 8;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, TheArch);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  const llvm::ReturnInst *ReturnInst = nullptr;
  for (const llvm::BasicBlock &BB : *Function)
    for (const llvm::Instruction &Instruction : BB)
      if (auto *Candidate = llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
        ReturnInst = Candidate;
  ASSERT_NE(ReturnInst, nullptr);

  auto *ReturnBitCast =
      llvm::dyn_cast<llvm::BitCastInst>(ReturnInst->getReturnValue());
  ASSERT_NE(ReturnBitCast, nullptr);
  auto *Widened = llvm::dyn_cast<llvm::ZExtInst>(ReturnBitCast->getOperand(0));
  ASSERT_NE(Widened, nullptr);
  EXPECT_EQ(Widened->getSrcTy()->getIntegerBitWidth(), 32u);
}

TEST(MedLLVMReturn, UsesLatestFloatPredecessorDefinitionBeforeWidthPreference) {
  constexpr Arch TheArch = Arch::X64;
  const TargetRegInfo &TRI = getTargetRegInfo(TheArch);

  MedFunc Func;
  Func.Entry = 0x5200;
  Func.Name = "latest_float_predecessor_return_alias";
  Func.ReturnType = NdType::makeFloat(4);

  MedBlock Producer;
  Producer.Id = 0;
  Producer.StartAddr = Func.Entry;
  Producer.EndAddr = Func.Entry + 8;
  Producer.Succs = {1};
  MedOp Older;
  Older.Opcode = NdOp::COPY;
  Older.Output = reg(1, 1, 16, TRI.fpReturnModelReg(), TheArch);
  Older.addInput(MedVar::makeConst(0x1122334455667788ULL, 16));
  Producer.Ops.push_back(Older);
  MedOp Newer;
  Newer.Opcode = NdOp::COPY;
  Newer.Output = reg(2, 1, 4, TRI.fpReturnModelReg(), TheArch);
  Newer.addInput(MedVar::makeConst(0x3f800000ULL, 4));
  Producer.Ops.push_back(Newer);

  MedBlock ReturnBlock;
  ReturnBlock.Id = 1;
  ReturnBlock.StartAddr = Func.Entry + 8;
  ReturnBlock.EndAddr = Func.Entry + 12;
  ReturnBlock.Preds = {0};
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry + 8;
  ReturnBlock.Ops.push_back(Return);
  Func.Blocks = {std::move(Producer), std::move(ReturnBlock)};

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, TheArch);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  const llvm::ReturnInst *ReturnInst = nullptr;
  for (const llvm::BasicBlock &BB : *Function)
    for (const llvm::Instruction &Instruction : BB)
      if (auto *Candidate = llvm::dyn_cast<llvm::ReturnInst>(&Instruction))
        ReturnInst = Candidate;
  ASSERT_NE(ReturnInst, nullptr);

  auto *ReturnBitCast =
      llvm::dyn_cast<llvm::BitCastInst>(ReturnInst->getReturnValue());
  ASSERT_NE(ReturnBitCast, nullptr);
  auto *Widened = llvm::dyn_cast<llvm::ZExtInst>(ReturnBitCast->getOperand(0));
  ASSERT_NE(Widened, nullptr);
  EXPECT_EQ(Widened->getSrcTy()->getIntegerBitWidth(), 32u);
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

TEST(MedLLVMJumpTableSelector,
     DuplicateCaseBitPatternsAfterSelectorTruncationFailClosed) {
  constexpr va_t DispatchAddress = 0x3000;
  MedFunc Func;
  Func.Entry = DispatchAddress;
  Func.Name = "duplicate_narrow_jump_table_cases";

  MedVar Index = reg(1, 0, 1, /*RegOff=*/0x40, Arch::X64);
  Func.Params.push_back(Index);

  MedBlock Dispatch;
  Dispatch.Id = 0;
  Dispatch.StartAddr = DispatchAddress;
  Dispatch.EndAddr = DispatchAddress + 1;
  Dispatch.Succs = {1, 2};
  MedOp Branch;
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Addr = DispatchAddress;
  Branch.addInput(Index);
  Dispatch.Ops.push_back(Branch);

  auto makeReturnBlock = [](int Id, va_t Addr, uint64_t Value) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Addr;
    Block.EndAddr = Addr + 1;
    Block.Preds = {0};
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Addr;
    Return.addInput(MedVar::makeConst(Value, 4));
    Block.Ops.push_back(Return);
    return Block;
  };
  Func.Blocks = {std::move(Dispatch), makeReturnBlock(1, 0x3100, 1),
                 makeReturnBlock(2, 0x3200, 2)};

  JumpTable Table;
  Table.InsnAddr = DispatchAddress;
  Table.EntrySize = 1;
  Table.PreScaledIndex = true;
  Table.IndexRegOff = static_cast<int>(Index.RegOff);
  Table.Targets = {0x3100, 0x3200};
  Table.CaseLabels = {0, 256};
  Func.JumpTables.push_back(std::move(Table));
  MedSwitchSelectorPlan Plan;
  Plan.Selector = Index;
  Plan.ResultSize = Index.Size;
  Func.SwitchSelectorPlans.emplace(DispatchAddress, std::move(Plan));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X64);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  bool SawSwitch = false;
  bool SawTrap = false;
  for (const llvm::BasicBlock &Block : *Function)
    for (const llvm::Instruction &Instruction : Block) {
      SawSwitch |= llvm::isa<llvm::SwitchInst>(Instruction);
      if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&Instruction))
        SawTrap |= Call->getIntrinsicID() == llvm::Intrinsic::trap;
    }
  EXPECT_FALSE(SawSwitch);
  EXPECT_TRUE(SawTrap);
}

TEST(MedSwitchNorm, AffineConstantsFollowEmitterWidthCoercion) {
  auto peelDelta = [](NdOp Opcode, uint64_t Constant, uint16_t ConstantSize) {
    MedFunc Func;
    MedVar Source = reg(1, 0, 8, x86reg::RDI, Arch::X64);
    MedVar Index = temp(2, 0, 8, Arch::X64);
    MedBlock Block;
    Block.Id = 0;
    Block.Ops.push_back(binary(Opcode, Index, Source,
                               MedVar::makeConst(Constant, ConstantSize)));
    Func.Blocks.push_back(std::move(Block));

    uint64_t Delta = 0;
    MedVar Peeled = peelAffineSwitchVar(Func, Index, Delta);
    EXPECT_EQ(Peeled.Kind, Source.Kind);
    EXPECT_EQ(Peeled.Id, Source.Id);
    EXPECT_EQ(Peeled.SSAVer, Source.SSAVer);
    EXPECT_EQ(Peeled.Size, Source.Size);
    return Delta;
  };

  // Arithmetic first zero-extends/truncates the constant to the output width.
  // An i8 0xf0 is therefore +240 in an i64 ADD/SUB, not signed -16.
  EXPECT_EQ(peelDelta(NdOp::INT_ADD, 0xf0, 1), UINT64_C(0xffffffffffffff10));
  EXPECT_EQ(peelDelta(NdOp::INT_SUB, 0xf0, 1), UINT64_C(0x00000000000000f0));

  // A full-width bit-pattern keeps its modular two's-complement meaning.
  EXPECT_EQ(peelDelta(NdOp::INT_ADD, UINT64_C(0xfffffffffffffff0), 8),
            UINT64_C(0x10));
  EXPECT_EQ(peelDelta(NdOp::INT_SUB, UINT64_C(0xfffffffffffffff0), 8),
            UINT64_C(0xfffffffffffffff0));
  EXPECT_EQ(peelDelta(NdOp::INT_ADD, UINT64_C(0x8000000000000000), 8),
            UINT64_C(0x8000000000000000));
}

TEST(MedSwitchNorm, StopsAtWidthChangingExtension) {
  MedFunc Func;
  MedVar Narrow = reg(1, 0, 1, x86reg::RAX, Arch::X64);
  MedVar Widened = temp(2, 0, 8, Arch::X64);
  MedVar Index = temp(3, 0, 8, Arch::X64);
  MedBlock Block;
  Block.Id = 0;
  Block.Ops.push_back(unary(NdOp::INT_ZEXT, Widened, Narrow));
  Block.Ops.push_back(
      binary(NdOp::INT_ADD, Index, Widened, MedVar::makeConst(1, 1)));
  Func.Blocks.push_back(std::move(Block));

  uint64_t Delta = 0;
  MedVar Peeled = peelAffineSwitchVar(Func, Index, Delta);
  EXPECT_EQ(Peeled.Kind, Widened.Kind);
  EXPECT_EQ(Peeled.Id, Widened.Id);
  EXPECT_EQ(Peeled.SSAVer, Widened.SSAVer);
  EXPECT_EQ(Peeled.Size, 8u);
  EXPECT_EQ(Delta, UINT64_MAX);
}

TEST(MedSwitchNorm, WidenedSelectorKeepsAllDistinctCaseBitPatterns) {
  constexpr va_t DispatchAddress = 0x6000;
  constexpr size_t CaseCount = 257;
  MedFunc Func;
  Func.Entry = DispatchAddress;
  Func.Name = "widened_jump_table_selector";

  MedVar Narrow = reg(1, 0, 1, x86reg::RDI, Arch::X64);
  MedVar Widened = temp(2, 0, 8, Arch::X64);
  MedVar Index = temp(3, 0, 8, Arch::X64);
  Func.Params.push_back(Narrow);

  MedBlock Dispatch;
  Dispatch.Id = 0;
  Dispatch.StartAddr = DispatchAddress;
  Dispatch.EndAddr = DispatchAddress + 1;
  Dispatch.Ops.push_back(unary(NdOp::INT_ZEXT, Widened, Narrow));
  Dispatch.Ops.push_back(
      binary(NdOp::INT_ADD, Index, Widened, MedVar::makeConst(1, 1)));
  MedOp Branch;
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Addr = DispatchAddress;
  Branch.addInput(Index);
  Dispatch.Ops.push_back(Branch);

  JumpTable Table;
  Table.InsnAddr = DispatchAddress;
  Table.EntrySize = 8;
  Table.IndexRegOff = static_cast<int>(Narrow.RegOff);
  JumpTableSelectorUseRef Ref;
  Ref.Addr = DispatchAddress;
  Ref.Seq = 0;
  Ref.ExpectedOpcode = NdOp::INT_ADD;
  Ref.Role = JumpTableSelectorUseRef::ValueRole::Output;
  Ref.ExpectedSize = Index.Size;
  Table.SelectorUseRefs.push_back(Ref);

  for (size_t K = 0; K < CaseCount; ++K) {
    const va_t TargetAddress = 0x7000 + static_cast<va_t>(K) * 0x10;
    const int BlockId = static_cast<int>(K + 1);
    Dispatch.Succs.push_back(BlockId);
    Table.Targets.push_back(TargetAddress);

    MedBlock Target;
    Target.Id = BlockId;
    Target.StartAddr = TargetAddress;
    Target.EndAddr = TargetAddress + 1;
    Target.Preds = {0};
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = TargetAddress;
    Return.addInput(MedVar::makeConst(K, 8));
    Target.Ops.push_back(Return);
    Func.Blocks.push_back(std::move(Target));
  }
  Func.Blocks.insert(Func.Blocks.begin(), std::move(Dispatch));
  Func.JumpTables.push_back(Table);
  MedSwitchSelectorPlan Plan;
  Plan.Selector = Index;
  Plan.ResultSize = Index.Size;
  Func.SwitchSelectorPlans.emplace(DispatchAddress, Plan);

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X64);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);
  const llvm::SwitchInst *LLVMCaseSwitch = nullptr;
  for (const llvm::BasicBlock &Block : *Function)
    if (const auto *Candidate =
            llvm::dyn_cast<llvm::SwitchInst>(Block.getTerminator())) {
      LLVMCaseSwitch = Candidate;
      break;
    }
  ASSERT_NE(LLVMCaseSwitch, nullptr);
  EXPECT_EQ(LLVMCaseSwitch->getCondition()->getType()->getIntegerBitWidth(),
            64u);
  EXPECT_EQ(LLVMCaseSwitch->getNumCases(), CaseCount);

  MedToHighConverter Converter;
  Converter.setJumpTables({Table});
  HighFunc High = Converter.convert(Func, Arch::X64);
  auto HighSwitch = std::find_if(
      High.Body.begin(), High.Body.end(),
      [](const HighStmt &Stmt) { return Stmt.Kind == StmtKind::Switch; });
  ASSERT_NE(HighSwitch, High.Body.end());
  ASSERT_EQ(HighSwitch->Cases.size(), CaseCount);
  std::set<uint64_t> HighCases;
  for (const SwitchCase &Case : HighSwitch->Cases)
    HighCases.insert(Case.Value);
  EXPECT_EQ(HighCases.size(), CaseCount);
  EXPECT_TRUE(HighCases.count(UINT64_MAX));
  EXPECT_TRUE(HighCases.count(255));
}

TEST(MedSwitchNorm, DefinitionIdentityIncludesVariableKind) {
  MedFunc Func;
  MedVar Source = reg(1, 0, 8, x86reg::RDI, Arch::X64);
  MedVar CollidingTemp = temp(6, 0, 8, Arch::X64);
  MedBlock Block;
  Block.Id = 0;
  Block.Ops.push_back(
      binary(NdOp::INT_ADD, CollidingTemp, Source, MedVar::makeConst(3, 8)));
  Func.Blocks.push_back(std::move(Block));

  MedVar Param;
  Param.Kind = MedVar::Param;
  Param.Id = CollidingTemp.Id;
  Param.SSAVer = CollidingTemp.SSAVer;
  Param.Size = CollidingTemp.Size;
  Param.TheArch = Arch::X64;

  uint64_t Delta = 0;
  MedVar Unchanged = peelAffineSwitchVar(Func, Param, Delta);
  EXPECT_EQ(Unchanged.Kind, MedVar::Param);
  EXPECT_EQ(Unchanged.Id, Param.Id);
  EXPECT_EQ(Delta, 0);

  MedVar Peeled = peelAffineSwitchVar(Func, CollidingTemp, Delta);
  EXPECT_EQ(Peeled.Kind, Source.Kind);
  EXPECT_EQ(Peeled.Id, Source.Id);
  EXPECT_EQ(Delta, UINT64_C(0xfffffffffffffffd));
}

TEST(JumpTableHighSelector,
     DuplicateCaseBitPatternsAtSelectorWidthRemainUnresolved) {
  constexpr va_t DispatchAddress = 0x3800;
  MedFunc Func;
  Func.Entry = DispatchAddress;
  Func.Name = "high_duplicate_narrow_jump_table_cases";

  MedVar Index = reg(1, 0, 1, x86reg::RAX, Arch::X64);
  Func.Params.push_back(Index);

  MedBlock Dispatch;
  Dispatch.Id = 0;
  Dispatch.StartAddr = DispatchAddress;
  Dispatch.EndAddr = DispatchAddress + 1;
  Dispatch.Succs = {1, 2};
  MedOp Branch;
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Addr = DispatchAddress;
  Branch.addInput(Index);
  Dispatch.Ops.push_back(Branch);

  auto makeReturnBlock = [](int Id, va_t Addr, uint64_t Value) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Addr;
    Block.EndAddr = Addr + 1;
    Block.Preds = {0};
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = Addr;
    Return.addInput(MedVar::makeConst(Value, 4));
    Block.Ops.push_back(Return);
    return Block;
  };
  Func.Blocks = {std::move(Dispatch), makeReturnBlock(1, 0x3810, 1),
                 makeReturnBlock(2, 0x3820, 2)};

  JumpTable Table;
  Table.InsnAddr = DispatchAddress;
  Table.EntrySize = 1;
  Table.PreScaledIndex = true;
  Table.IndexRegOff = static_cast<int>(Index.RegOff);
  Table.Targets = {0x3810, 0x3820};
  Table.CaseLabels = {0, 256};
  MedSwitchSelectorPlan Plan;
  Plan.Selector = Index;
  Plan.ResultSize = Index.Size;
  Func.SwitchSelectorPlans.emplace(DispatchAddress, Plan);

  MedToHighConverter Converter;
  Converter.setJumpTables({Table});
  HighFunc High = Converter.convert(Func, Arch::X64);
  EXPECT_TRUE(std::none_of(
      High.Body.begin(), High.Body.end(),
      [](const HighStmt &Stmt) { return Stmt.Kind == StmtKind::Switch; }));
  EXPECT_TRUE(
      std::any_of(High.Body.begin(), High.Body.end(), [](const HighStmt &Stmt) {
        return Stmt.Kind == StmtKind::Goto && Stmt.GotoTarget == InvalidVA;
      }));
}

static LowFunc scalarAddressModelLowFunc(bool DuplicateOccurrence = false) {
  constexpr va_t AddAddress = 0x3a00;

  LowFunc Low;
  Low.Entry = AddAddress;
  Low.Name =
      DuplicateOccurrence ? "ambiguous_scalar_model" : "exact_scalar_model";

  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = AddAddress;
  Block.EndAddr = AddAddress + 2;

  LowOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Addr = AddAddress;
  Add.Seq = 7;
  Add.Output = NdVar::reg(x86reg::RAX, 4);
  Add.addInput(NdVar::reg(x86reg::RBX, 4));
  Add.addInput(NdVar::reg(x86reg::RCX, 4));
  Block.Ops.push_back(Add);

  if (DuplicateOccurrence) {
    LowOp Duplicate = Add;
    Duplicate.Inputs[0] = Add.Output;
    Block.Ops.push_back(Duplicate);
  }

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = AddAddress + 1;
  Return.Seq = 0;
  Return.addInput(Add.Output);
  Block.Ops.push_back(Return);
  Low.Blocks.push_back(std::move(Block));

  RelocatedInstructionScalarModelOccurrence Model;
  Model.FieldVA = AddAddress + 1;
  Model.InstructionAddr = AddAddress;
  Model.OpSeq = Add.Seq;
  Model.Width = Add.Output.Size;
  Model.OutputOpcode = Add.Opcode;
  Model.OutputWitness = Add.Output;
  Low.RelocatedInstructionScalarModelOccurrences.push_back(Model);
  return Low;
}

TEST(LowToMedScalarAddressModel,
     ExactSourceOccurrenceBindsTheSurvivingFullOutputLane) {
  MedFunc Med =
      LowToMedConverter().convert(scalarAddressModelLowFunc(), Arch::X86);

  ASSERT_EQ(Med.ScalarAddressModels.size(), 1u);
  const MedVar &Value = Med.ScalarAddressModels.front().Value;
  EXPECT_EQ(Value.Kind, MedVar::Reg);
  EXPECT_EQ(Value.TheArch, Arch::X86);
  EXPECT_EQ(Value.RegOff, x86reg::RAX);
  EXPECT_EQ(Value.Size, 4u);
}

TEST(LowToMedScalarAddressModel,
     SourceIdentityOrFullOutputLaneMismatchDoesNotBind) {
  auto expectUnbound = [](auto Mutate) {
    LowFunc Low = scalarAddressModelLowFunc();
    Mutate(Low.RelocatedInstructionScalarModelOccurrences.front());
    MedFunc Med = LowToMedConverter().convert(Low, Arch::X86);
    EXPECT_TRUE(Med.ScalarAddressModels.empty());
  };

  expectUnbound([](auto &Model) { Model.InstructionAddr += 1; });
  expectUnbound([](auto &Model) { Model.OpSeq += 1; });
  expectUnbound([](auto &Model) { Model.OutputOpcode = NdOp::INT_SUB; });
  expectUnbound([](auto &Model) { Model.Width = 2; });
  expectUnbound(
      [](auto &Model) { Model.OutputWitness = NdVar::reg(x86reg::RDX, 4); });
  expectUnbound(
      [](auto &Model) { Model.OutputWitness = NdVar::tmp(0xdead, 4); });
}

TEST(LowToMedScalarAddressModel,
     DuplicateExactSourceOccurrenceIsAmbiguousAndDoesNotBind) {
  MedFunc Med =
      LowToMedConverter().convert(scalarAddressModelLowFunc(true), Arch::X86);
  EXPECT_TRUE(Med.ScalarAddressModels.empty());
}

TEST(MedLLVMScalarAddressModel,
     ExactSSAEmitsModelZeroWithoutChangingSameRegisterSuccessor) {
  MedFunc Func;
  Func.Entry = 0x3b00;
  Func.Name = "exact_scalar_model_llvm";

  MedVar Certified = reg(1, 1, 4, x86reg::RAX, Arch::X86);
  MedVar Ordinary = reg(1, 2, 4, x86reg::RAX, Arch::X86);
  MedVar Sum = temp(2, 0, 4, Arch::X86);

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.EndAddr = Func.Entry + 3;

  MedOp CertifiedAdd =
      binary(NdOp::INT_ADD, Certified, MedVar::makeConst(10, 4),
             MedVar::makeConst(20, 4));
  CertifiedAdd.Addr = Func.Entry;
  CertifiedAdd.OriginSeq = 7;
  Block.Ops.push_back(CertifiedAdd);

  MedOp OrdinaryAdd = binary(NdOp::INT_ADD, Ordinary, MedVar::makeConst(10, 4),
                             MedVar::makeConst(20, 4));
  OrdinaryAdd.Addr = Func.Entry + 1;
  OrdinaryAdd.OriginSeq = 0;
  Block.Ops.push_back(OrdinaryAdd);

  Block.Ops.push_back(binary(NdOp::INT_ADD, Sum, Certified, Ordinary));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry + 2;
  Return.addInput(Sum);
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));
  Func.ScalarAddressModels.push_back(
      {RelocatedInstructionScalarModelOccurrence::ModelKind::I386ELFGOTBaseZero,
       Certified});

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X86);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  bool CertifiedStoredZero = false;
  bool CertifiedStoredThirty = false;
  bool OrdinaryStoredThirty = false;
  for (const llvm::BasicBlock &LLVMBlock : *Function)
    for (const llvm::Instruction &Instruction : LLVMBlock) {
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
      if (!Store)
        continue;
      const auto *Stored =
          llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand());
      const llvm::Value *Storage =
          Store->getPointerOperand()->stripPointerCasts();
      if (!Stored || !Storage)
        continue;
      if (Storage->getName() == "EAX.1") {
        CertifiedStoredZero |= Stored->isZero();
        CertifiedStoredThirty |= Stored->getZExtValue() == 30;
      }
      if (Storage->getName() == "EAX.2")
        OrdinaryStoredThirty |= Stored->getZExtValue() == 30;
    }
  EXPECT_TRUE(CertifiedStoredZero);
  EXPECT_FALSE(CertifiedStoredThirty);
  EXPECT_TRUE(OrdinaryStoredThirty)
      << "the certified add is model zero, while an equal ordinary add at a "
         "new SSA version retains 10 + 20 semantics";
}

TEST(MedLLVMEmitterConstants, NarrowSignExtendedScalarKeepsItsLowBitPattern) {
  MedFunc Func;
  Func.Entry = 0x3c00;
  Func.Name = "narrow_sign_extended_scalar";

  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.EndAddr = Func.Entry + 2;

  const uint64_t SignExtendedMinusTwenty =
      static_cast<uint64_t>(static_cast<int64_t>(-20));
  Block.Ops.push_back(
      binary(NdOp::INT_ADD, temp(1, 0, 4, Arch::X86),
             MedVar::makeConst(SignExtendedMinusTwenty, 4,
                               ConstantAddressProvenance::Scalar),
             MedVar::makeConst(1, 4, ConstantAddressProvenance::Scalar)));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Func.Entry + 1;
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(std::move(Block));

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X86);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  bool StoredExpectedLowBits = false;
  for (const llvm::BasicBlock &LLVMBlock : *Function)
    for (const llvm::Instruction &Instruction : LLVMBlock) {
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
      const auto *Stored =
          Store ? llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand())
                : nullptr;
      StoredExpectedLowBits |= Stored && Stored->getBitWidth() == 32 &&
                               Stored->getZExtValue() == UINT64_C(0xffffffed);
    }
  EXPECT_TRUE(StoredExpectedLowBits);
}

TEST(MedLLVMEmitterConstants, NarrowUnsignedPhiConstantKeepsItsLowBitPattern) {
  constexpr va_t EntryAddress = 0x3d00;
  constexpr va_t LeftAddress = EntryAddress + 0x10;
  constexpr va_t RightAddress = EntryAddress + 0x20;
  constexpr va_t MergeAddress = EntryAddress + 0x30;

  MedFunc Func;
  Func.Entry = EntryAddress;
  Func.Name = "narrow_unsigned_phi_constant";
  MedVar Condition = reg(1, 0, 8, x86reg::RDI, Arch::X64);
  Func.Params.push_back(Condition);

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = EntryAddress;
  Entry.EndAddr = EntryAddress + 1;
  Entry.Succs = {1, 2};
  MedOp Split;
  Split.Opcode = NdOp::COND_BR;
  Split.addInput(MedVar::makeConst(LeftAddress, 8));
  Split.addInput(Condition);
  Entry.Ops.push_back(std::move(Split));

  auto makePredecessor = [&](int Id, va_t Address) {
    MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 1;
    Block.Preds = {0};
    Block.Succs = {3};
    MedOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.addInput(MedVar::makeConst(MergeAddress, 8));
    Block.Ops.push_back(std::move(Branch));
    return Block;
  };

  MedBlock Merge;
  Merge.Id = 3;
  Merge.StartAddr = MergeAddress;
  Merge.EndAddr = MergeAddress + 1;
  Merge.Preds = {1, 2};
  MedVar Merged = temp(2, 0, 4, Arch::X64);
  PhiNode Phi;
  Phi.Output = Merged;
  Phi.Args = {{1, MedVar::makeConst(UINT64_C(0x9e3779b9), 4,
                                    ConstantAddressProvenance::Scalar)},
              {2, MedVar::makeConst(0, 4, ConstantAddressProvenance::Scalar)}};
  Merge.Phis.push_back(std::move(Phi));
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = MergeAddress;
  Merge.Ops.push_back(Return);

  Func.Blocks = {std::move(Entry), makePredecessor(1, LeftAddress),
                 makePredecessor(2, RightAddress), std::move(Merge)};

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Func}, Context, Func.Name, Arch::X64);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Func.Name);
  ASSERT_NE(Function, nullptr);

  bool StoredExpectedLowBits = false;
  for (const llvm::BasicBlock &LLVMBlock : *Function)
    for (const llvm::Instruction &Instruction : LLVMBlock) {
      const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction);
      const auto *Stored =
          Store ? llvm::dyn_cast<llvm::ConstantInt>(Store->getValueOperand())
                : nullptr;
      StoredExpectedLowBits |= Stored && Stored->getBitWidth() == 32 &&
                               Stored->getZExtValue() == UINT64_C(0x9e3779b9);
    }
  EXPECT_TRUE(StoredExpectedLowBits);
}

TEST(MedLLVMEmitterReturn,
     ProjectsCrossBlockYmmParentToTheDeclaredXmmAbiWidth) {
  constexpr Arch TheArch = Arch::X64;
  constexpr va_t EntryAddress = 0x4100;
  constexpr va_t ConsumerAddress = 0x4110;

  LowFunc Low;
  Low.Entry = EntryAddress;
  Low.Name = "cross_block_scalar_xmm_return_with_ymm_consumer";
  Low.Blocks.resize(2);

  LowBlock &Writer = Low.Blocks[0];
  Writer.Id = 0;
  Writer.StartAddr = EntryAddress;
  Writer.EndAddr = EntryAddress + 4;
  Writer.Succs = {1};

  // A legacy/scalar XMM write preserves the upper YMM half.  The Low-to-Med
  // alias repair must therefore keep a 32-byte parent for the real YMM use in
  // the successor; the function return below must not expose that physical
  // container when the recovered ABI declares only XMM0's low 16 bytes.
  LowOp ScalarWrite;
  ScalarWrite.Opcode = NdOp::COPY;
  ScalarWrite.Addr = EntryAddress;
  ScalarWrite.Output = NdVar::reg(x86reg::XMM0, 4);
  ScalarWrite.addInput(NdVar::scalar(UINT64_C(0x3f800000), 4));
  Writer.Ops.push_back(ScalarWrite);

  LowBlock &Consumer = Low.Blocks[1];
  Consumer.Id = 1;
  Consumer.StartAddr = ConsumerAddress;
  Consumer.EndAddr = ConsumerAddress + 4;
  Consumer.Preds = {0};

  LowOp WideStore;
  WideStore.Opcode = NdOp::STORE;
  WideStore.Addr = ConsumerAddress;
  WideStore.addInput(NdVar::reg(x86reg::RDI, 8));
  WideStore.addInput(NdVar::reg(x86reg::XMM0, 32));
  Consumer.Ops.push_back(WideStore);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = ConsumerAddress + 1;
  Consumer.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, TheArch);
  Med.ReturnType = NdType::makeFloat(8);

  bool SawWideParent = false;
  bool SawWideConsumer = false;
  for (const MedBlock &Block : Med.Blocks)
    for (const MedOp &Op : Block.Ops) {
      SawWideParent |= Op.Output.Kind == MedVar::Reg &&
                       Op.Output.RegOff == x86reg::XMM0 && Op.Output.Size == 32;
      if (Op.Opcode == NdOp::STORE)
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          SawWideConsumer |= Op.Inputs[I].Size == 32;
    }
  ASSERT_TRUE(SawWideParent)
      << "the preserving scalar write must still merge into its YMM parent";
  ASSERT_TRUE(SawWideConsumer)
      << "the successor's true YMM consumer must retain all 32 bytes";

  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({Med}, Context, Med.Name, TheArch);
  ASSERT_NE(Module, nullptr);
  llvm::Function *Function = verifiedFunction(*Module, Med.Name);
  ASSERT_NE(Function, nullptr);
  ASSERT_TRUE(Function->getReturnType()->isVectorTy());
  EXPECT_EQ(Function->getReturnType()->getPrimitiveSizeInBits(), 128u);

  bool SawProjectedReturn = false;
  bool SawI256Store = false;
  for (const llvm::BasicBlock &Block : *Function)
    for (const llvm::Instruction &Instruction : Block) {
      if (const auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&Instruction)) {
        ASSERT_NE(Ret->getReturnValue(), nullptr);
        SawProjectedReturn |=
            Ret->getReturnValue()->getType() == Function->getReturnType();
      }
      if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
        SawI256Store |= Store->getValueOperand()->getType()->isIntegerTy(256);
    }
  EXPECT_TRUE(SawProjectedReturn);
  EXPECT_TRUE(SawI256Store)
      << "the ABI projection must not narrow a real YMM consumer";
}

TEST(LowToMedSelectorOccurrence,
     ChangedSourceOpcodeDoesNotBindAStaleOperandRole) {
  auto makeLow = [](bool OpcodeMismatch) {
    LowFunc Low;
    Low.Entry = 0x4000;
    Low.Name = OpcodeMismatch ? "changed_selector_occurrence"
                              : "surviving_selector_occurrence";

    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = 0x4000;
    Block.EndAddr = 0x4002;
    LowOp Selector;
    Selector.Opcode = OpcodeMismatch ? NdOp::COPY : NdOp::INT_ADD;
    Selector.Addr = 0x4000;
    Selector.Seq = 7;
    Selector.Output = NdVar::tmp(0x1000, 8);
    Selector.addInput(NdVar::reg(x86reg::RDI, 8));
    if (!OpcodeMismatch)
      Selector.addInput(NdVar::scalar(2, 8));
    Block.Ops.push_back(Selector);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = 0x4001;
    Return.Seq = 0;
    Return.addInput(Selector.Output);
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(std::move(Block));

    JumpTable Table;
    Table.InsnAddr = 0x4010;
    JumpTableSelectorUseRef Ref;
    Ref.Addr = Selector.Addr;
    Ref.Seq = Selector.Seq;
    Ref.ExpectedOpcode = NdOp::INT_ADD;
    Ref.Role = JumpTableSelectorUseRef::ValueRole::Input;
    Ref.InputNo = 0;
    Ref.ExpectedSize = Selector.Inputs[0].Size;
    Table.SelectorUseRefs.push_back(Ref);
    Low.JumpTables.push_back(std::move(Table));
    return Low;
  };

  MedFunc Changed = LowToMedConverter().convert(makeLow(true), Arch::X64);
  EXPECT_TRUE(Changed.SwitchSelectorPlans.empty())
      << "COPY at the same Addr/Seq must not satisfy an INT_ADD use-ref";

  MedFunc Surviving = LowToMedConverter().convert(makeLow(false), Arch::X64);
  auto It = Surviving.SwitchSelectorPlans.find(0x4010);
  ASSERT_NE(It, Surviving.SwitchSelectorPlans.end());
  EXPECT_EQ(It->second.PlanKind, MedSwitchSelectorPlan::Kind::Direct);
  EXPECT_EQ(It->second.Selector.Size, 8u);
}

} // namespace
