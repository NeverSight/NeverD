//===- MedPositionalCallArgumentsTests.cpp - Shared ABI slots ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Verify scalar register positions shared by integer and floating arguments.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"

#include <map>
#include <memory>
#include <vector>

using namespace neverd;
namespace {
MedVar reg(int Id, uint64_t Offset, uint16_t Bytes) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.SSAVer = 1;
  V.RegOff = Offset;
  V.Size = Bytes;
  V.TheArch = Arch::X64;
  return V;
}
MedOp copy(MedVar Output, uint64_t Bits) {
  MedOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = Output;
  Op.addInput(MedVar::makeConst(Bits, Output.Size));
  return Op;
}
MedOp call(bool Indirect) {
  MedOp Op;
  Op.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
  Op.Addr = 0x1008;
  Op.CallSiteId = 1;
  Op.Output = reg(100, getTargetRegInfo(Arch::X64).IntReturnReg, 8);
  if (Indirect)
    Op.addInput(reg(101, getTargetRegInfo(Arch::X64).FramePointer, 8));
  else
    Op.addInput(MedVar::makeConst(0x2000, 8));
  return Op;
}
MedFunc function() {
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "positional_arguments";
  F.CC = CallingConv::Win64;
  F.Blocks.resize(1);
  F.Blocks[0].Id = 0;
  return F;
}
BinaryImage image(BinaryFormat Format) {
  BinaryImage I;
  I.Arch = Arch::X64;
  I.Format = Format;
  return I;
}

TEST(MedPositionalCallArguments, IntegerSlotsDoNotAcquireAnotherVectorBank) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto GPR = TRI.integerArgumentLayout(BinaryFormat::COFF).Registers;
  const auto FP = TRI.floatingParamRegs(BinaryFormat::COFF);
  const auto Image = image(BinaryFormat::COFF);
  for (bool Indirect : {false, true})
    for (unsigned Count : {1u, 4u}) {
      SCOPED_TRACE(Indirect);
      SCOPED_TRACE(Count);
      auto F = function();
      std::vector<MedVar> Expected;
      for (unsigned K = 0; K < Count; ++K) {
        const auto V = reg(K, GPR[K], 8);
        F.Blocks[0].Ops.push_back(copy(V, 0x100 + K));
        Expected.push_back(MedVar::makeConst(0x100 + K, 8));
      }
      // SIMD scratch, even when every register is explicitly written, does
      // not add another four arguments after the ordinary positional run.
      for (unsigned K = 0; K < Count; ++K)
        F.Blocks[0].Ops.push_back(copy(reg(10 + K, FP[K], 16), 0xABC + K));
      F.Blocks[0].Ops.push_back(call(Indirect));
      const auto Before = F.Blocks[0].Ops;
      recoverCallAbi(F, Arch::X64, {}, &Image);
      ASSERT_EQ(F.CallInfos.size(), 1u);
      EXPECT_EQ(F.CallInfos[0].Args, Expected);
      EXPECT_EQ(F.Blocks[0].Ops.size(), Before.size());
      for (size_t K = 0; K < Before.size(); ++K) {
        EXPECT_EQ(F.Blocks[0].Ops[K].Opcode, Before[K].Opcode);
        EXPECT_EQ(F.Blocks[0].Ops[K].Output, Before[K].Output);
      }
    }
}

TEST(MedPositionalCallArguments, MixedArgumentsKeepTheirPhysicalPositions) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto GPR = TRI.integerArgumentLayout(BinaryFormat::COFF).Registers;
  const auto FP = TRI.floatingParamRegs(BinaryFormat::COFF);
  const auto Image = image(BinaryFormat::COFF);
  for (bool Indirect : {false, true})
    for (unsigned Mask = 1; Mask != 16; ++Mask) {
      SCOPED_TRACE(Indirect);
      SCOPED_TRACE(Mask);
      auto F = function();
      std::vector<MedVar> Expected;
      for (unsigned K = 0; K != 4; ++K) {
        const bool Floating = (Mask >> K) & 1;
        const auto V = reg(K, Floating ? FP[K] : GPR[K], 8);
        F.Blocks[0].Ops.push_back(copy(V, 0x100 + K));
        Expected.push_back(Floating ? V : MedVar::makeConst(0x100 + K, 8));
      }
      F.Blocks[0].Ops.push_back(call(Indirect));
      recoverCallAbi(F, Arch::X64, {}, &Image);
      ASSERT_EQ(F.CallInfos.size(), 1u);
      EXPECT_EQ(F.CallInfos[0].Args, Expected);
    }
}

TEST(MedPositionalCallArguments, SysVRetainsIndependentRegisterBanks) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto GPR = TRI.integerArgumentLayout(BinaryFormat::ELF).Registers;
  const auto FP = TRI.floatingParamRegs(BinaryFormat::ELF);
  const auto Image = image(BinaryFormat::ELF);
  for (bool Indirect : {false, true}) {
    auto F = function();
    F.CC = CallingConv::SysV_AMD64;
    std::vector<MedVar> Expected;
    for (unsigned K = 0; K != 4; ++K) {
      const auto V = reg(K, GPR[K], 8);
      F.Blocks[0].Ops.push_back(copy(V, 0x100 + K));
      Expected.push_back(MedVar::makeConst(0x100 + K, 8));
    }
    const auto V = reg(10, FP[0], 8);
    F.Blocks[0].Ops.push_back(copy(V, 0x200));
    Expected.push_back(V);
    F.Blocks[0].Ops.push_back(call(Indirect));
    recoverCallAbi(F, Arch::X64, {}, &Image);
    ASSERT_EQ(F.CallInfos.size(), 1u);
    EXPECT_EQ(F.CallInfos[0].Args, Expected);
    EXPECT_EQ(F.CallInfos[0].IntegerRegisterArgCount, 4);
    EXPECT_EQ(F.CallInfos[0].FloatingRegisterArgCount, 1);
  }
}

TEST(MedPositionalCallArguments, StackArgumentsFollowFourSharedPositions) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto GPR = TRI.integerArgumentLayout(BinaryFormat::COFF).Registers;
  const auto FP = TRI.floatingParamRegs(BinaryFormat::COFF);
  const auto Image = image(BinaryFormat::COFF);
  for (bool Indirect : {false, true}) {
    auto F = function();
    F.FrameSize = 0x60;
    auto &B = F.Blocks[0];
    auto EntrySP = reg(90, TRI.StackPointer, 8);
    EntrySP.SSAVer = 0;
    const auto SP = reg(90, TRI.StackPointer, 8);
    MedOp Adjust;
    Adjust.Opcode = NdOp::INT_SUB;
    Adjust.Output = SP;
    Adjust.addInput(EntrySP);
    Adjust.addInput(MedVar::makeConst(0x60, 8));
    B.Ops.push_back(Adjust);
    std::vector<MedVar> Expected;
    for (unsigned K = 0; K != 4; ++K) {
      B.Ops.push_back(copy(reg(K, GPR[K], 8), 0x100 + K));
      B.Ops.push_back(copy(reg(10 + K, FP[K], 16), 0xABC + K));
      Expected.push_back(MedVar::makeConst(0x100 + K, 8));
    }
    for (unsigned K = 0; K != 3; ++K) {
      auto Address = reg(200 + K, 0, 8);
      Address.Kind = MedVar::Temp;
      MedOp Add;
      Add.Opcode = NdOp::INT_ADD;
      Add.Output = Address;
      Add.addInput(SP);
      Add.addInput(MedVar::makeConst(0x20 + 8 * K, 8));
      B.Ops.push_back(Add);
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(Address);
      Store.addInput(MedVar::makeConst(0x200 + K, 8));
      B.Ops.push_back(Store);
      Expected.push_back(Store.Inputs[1]);
    }
    auto C = call(Indirect);
    C.CalleeStackArgs = 7;
    B.Ops.push_back(C);
    recoverCallAbi(F, Arch::X64, {}, &Image);
    ASSERT_EQ(F.CallInfos.size(), 1u);
    EXPECT_EQ(F.CallInfos[0].Args, Expected);
  }
}

TEST(MedPositionalCallArguments, KnownFloatingRegistersUseAbsolutePositions) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto GPR = TRI.integerArgumentLayout(BinaryFormat::COFF).Registers;
  const auto FP = TRI.floatingParamRegs(BinaryFormat::COFF);
  const auto Image = image(BinaryFormat::COFF);
  auto F = function();
  std::vector<MedVar> Expected;
  for (unsigned K = 0; K != 4; ++K) {
    const bool Floating = K % 2;
    const auto V = reg(K, Floating ? FP[K] : GPR[K], 8);
    F.Blocks[0].Ops.push_back(copy(V, 0x100 + K));
    Expected.push_back(Floating ? V : MedVar::makeConst(0x100 + K, 8));
  }
  F.Blocks[0].Ops.push_back(call(false));
  const std::map<va_t, int> FloatingCount{{0x2000, 2}};
  const std::map<va_t, std::vector<uint64_t>> FloatingRegisters{
      {0x2000, {FP[1], FP[3]}}};
  recoverCallAbi(F, Arch::X64, {}, &Image, nullptr, nullptr, &FloatingCount,
                 nullptr, &FloatingRegisters);
  ASSERT_EQ(F.CallInfos.size(), 1u);
  EXPECT_EQ(F.CallInfos[0].Args, Expected);
}

TEST(MedPositionalCallArguments, ExplicitSourceOperandsRetainTheirContract) {
  const auto Image = image(BinaryFormat::COFF);
  auto F = function();
  auto C = call(false);
  auto Hint = std::make_shared<SourceCallTypeHint>();
  Hint->Signature.ReturnType = NdType::makeInt(8);
  Hint->Signature.Parameters.resize(5);
  std::vector<MedVar> Expected;
  for (unsigned K = 0; K != 5; ++K) {
    const auto V = MedVar::makeConst(0x100 + K, 8);
    Hint->Signature.Parameters[K].Type = NdType::makeInt(8);
    C.addInput(V);
    Expected.push_back(V);
  }
  C.SourceCallHint = Hint;
  F.Blocks[0].Ops.push_back(C);
  recoverCallAbi(F, Arch::X64, {}, &Image);
  ASSERT_EQ(F.CallInfos.size(), 1u);
  EXPECT_EQ(F.CallInfos[0].Args, Expected);
  EXPECT_EQ(F.CallInfos[0].SourceCallHint, Hint);
}

TEST(MedPositionalCallArguments, NonstandardCalleeOrderKeepsItsBankContract) {
  const auto &TRI = getTargetRegInfo(Arch::X64);
  const auto GPR = TRI.integerArgumentLayout(BinaryFormat::COFF).Registers;
  const auto FP = TRI.floatingParamRegs(BinaryFormat::COFF);
  const auto Image = image(BinaryFormat::COFF);
  auto F = function();
  F.Blocks[0].Ops.push_back(copy(reg(0, GPR[0], 8), 0x100));
  F.Blocks[0].Ops.push_back(copy(reg(1, GPR[1], 8), 0x101));
  const auto Vector = reg(10, FP[0], 16);
  F.Blocks[0].Ops.push_back(copy(Vector, 0xABC));
  F.Blocks[0].Ops.push_back(call(false));
  const std::map<va_t, std::vector<uint64_t>> Registers{
      {0x2000, {GPR[1], GPR[0]}}};
  recoverCallAbi(F, Arch::X64, {}, &Image, nullptr, nullptr, nullptr, nullptr,
                 nullptr, nullptr, nullptr, nullptr, nullptr, &Registers);
  ASSERT_EQ(F.CallInfos.size(), 1u);
  const std::vector<MedVar> Expected{MedVar::makeConst(0x101, 8),
                                     MedVar::makeConst(0x100, 8), Vector};
  EXPECT_EQ(F.CallInfos[0].Args, Expected);
  EXPECT_TRUE(F.CallInfos[0].ArgumentsInCalleeRegisterOrder);
}
} // namespace
