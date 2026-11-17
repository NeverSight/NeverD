//===- CallArgumentRecoveryTests.cpp - Physical call argument slots -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Indirect calls, tail transfers and open variadic argument lists share
/// the target's physical argument layout, independently of pointer width.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedABIPass.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Entry = 0x1000;
constexpr va_t Callee = 0x2000;
constexpr va_t Slot = 0x3000;
constexpr Arch Architectures[] = {
    Arch::X86,         Arch::X64,      Arch::AArch64,
    Arch::MIPS32,      Arch::MIPS32EL, Arch::MIPS64N32,
    Arch::MIPS64N32EL, Arch::MIPS64,   Arch::MIPS64EL};

MedVar reg(int Id, uint64_t Register, uint16_t Bytes, Arch A) {
  MedVar Value;
  Value.Kind = MedVar::Reg;
  Value.Id = Id;
  Value.SSAVer = 1;
  Value.RegOff = Register;
  Value.Size = Bytes;
  Value.TheArch = A;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Entry;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

BinaryImage image(Arch A) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Format = BinaryFormat::ELF;
  Image.Bits =
      getTargetRegInfo(A).PointerSize == 4 ? Bitness::Bits32 : Bitness::Bits64;
  return Image;
}

unsigned registerCount(Arch A) {
  return A == Arch::X86 ? 0
                        : getTargetRegInfo(A)
                              .integerArgumentLayout(BinaryFormat::ELF)
                              .Registers.size();
}

MedFunc callWithSetup(Arch A, unsigned StackArguments, bool Indirect,
                      bool Tail) {
  const auto &TRI = getTargetRegInfo(A);
  const auto Layout = TRI.integerArgumentLayout(BinaryFormat::ELF);
  const unsigned Registers = registerCount(A);
  MedFunc Function;
  Function.Entry = Entry;
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.back();
  Block.Id = 0;
  Block.StartAddr = Entry;
  for (unsigned I = 0; I < Registers; ++I)
    Block.Ops.push_back(
        operation(NdOp::COPY, reg(I, Layout.Registers[I], Layout.SlotBytes, A),
                  {MedVar::makeConst(41 + I, Layout.SlotBytes)}));
  auto SP = reg(100, TRI.StackPointer, TRI.addressRegisterBytes(), A);
  SP.SSAVer = 0;
  for (unsigned I = 0; I < StackArguments; ++I) {
    auto Address = reg(101 + I, 0, TRI.addressRegisterBytes(), A);
    Address.Kind = MedVar::Temp;
    Block.Ops.push_back(
        operation(NdOp::INT_ADD, Address,
                  {SP, MedVar::makeConst((Tail ? Layout.EntryStackBase
                                               : Layout.CallStackBase) +
                                             I * Layout.SlotBytes,
                                         SP.Size)}));
    Block.Ops.push_back(operation(
        NdOp::STORE, {},
        {Address, MedVar::makeConst(41 + Registers + I, Layout.SlotBytes)}));
  }
  auto Call =
      operation(Indirect ? NdOp::INDIR_CALL : NdOp::CALL, {},
                {MedVar::makeConst(Indirect ? Slot : Callee, TRI.PointerSize)});
  Call.IsTailCall = Tail;
  Block.Ops.push_back(Call);
  if (Tail)
    Block.Ops.push_back(operation(NdOp::RETURN, {}, {}));
  return Function;
}

TEST(CallArgumentRecovery, IndirectCallsRetainEveryPhysicalSlot) {
  for (const Arch A : Architectures)
    for (bool Tail : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(Tail);
      auto Image = image(A);
      auto Function = callWithSetup(A, 2, true, Tail);
      recoverCallAbi(Function, A, {}, &Image);
      ASSERT_EQ(Function.CallInfos.size(), 1u);
      const auto &Call = Function.CallInfos.front();
      const auto Layout =
          getTargetRegInfo(A).integerArgumentLayout(Image.Format);
      EXPECT_EQ(Call.Args.size(), registerCount(A) + 2);
      if (Call.Args.size() != registerCount(A) + 2)
        continue;
      for (unsigned I = 0; I < Call.Args.size(); ++I) {
        ASSERT_TRUE(Call.Args[I].isConst());
        EXPECT_EQ(Call.Args[I].ConstVal, 41 + I);
        EXPECT_EQ(Call.Args[I].Size, Layout.SlotBytes);
      }
      if (!Call.StackArgumentOffsets.empty())
        EXPECT_EQ(Call.StackArgumentOffsets,
                  (std::vector<int64_t>{0, Layout.SlotBytes}));
    }
}

TEST(CallArgumentRecovery, TailForwardersKeepTheWholeStackCarrier) {
  for (const Arch A : Architectures) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    auto Image = image(A);
    auto Function = callWithSetup(A, 0, false, true);
    std::map<va_t, int> Registers{{Callee, registerCount(A)}};
    std::map<va_t, int> Total{{Callee, registerCount(A) + 2}};
    recoverCallAbi(Function, A, {}, &Image, &Registers, &Total);
    ASSERT_EQ(Function.CallInfos.size(), 1u);
    const auto &Call = Function.CallInfos.front();
    EXPECT_EQ(Call.Args.size(), registerCount(A) + 2);
    if (Call.Args.size() != registerCount(A) + 2)
      continue;
    const auto Layout = getTargetRegInfo(A).integerArgumentLayout(Image.Format);
    for (unsigned I = registerCount(A); I < Call.Args.size(); ++I) {
      EXPECT_EQ(Call.Args[I].Kind, MedVar::Param);
      EXPECT_EQ(Call.Args[I].Size, Layout.SlotBytes);
    }
    ASSERT_EQ(Function.Params.size(), 2u);
    for (const auto &Parameter : Function.Params)
      EXPECT_EQ(Parameter.Size, Layout.SlotBytes);
  }
}

TEST(CallArgumentRecovery, WideIndirectStackValuesSkipOnlyTheirCoveredSlots) {
  for (const Arch A : Architectures) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    auto Image = image(A);
    auto Function = callWithSetup(A, 3, true, false);
    const auto Layout = getTargetRegInfo(A).integerArgumentLayout(Image.Format);
    unsigned Store = 0;
    for (auto &Op : Function.Blocks.front().Ops)
      if (Op.Opcode == NdOp::STORE) {
        if (Store == 0)
          Op.Inputs[1].Size = 2 * Layout.SlotBytes;
        else if (Store == 1) {
          // Leave this half of the wide store without a separate overwrite.
          Op.Opcode = NdOp::COPY;
          Op.Output = Op.Inputs[0];
          Op.NumInputs = 1;
        }
        ++Store;
      }
    recoverCallAbi(Function, A, {}, &Image);
    ASSERT_EQ(Function.CallInfos.size(), 1u);
    const auto &Args = Function.CallInfos.front().Args;
    ASSERT_EQ(Args.size(), registerCount(A) + 2);
    EXPECT_EQ(Args[registerCount(A)].Size, 2 * Layout.SlotBytes);
    EXPECT_EQ(Args.back().ConstVal, 41 + registerCount(A) + 2);
    EXPECT_EQ(Args.back().Size, Layout.SlotBytes);
  }
}

TEST(CallArgumentRecovery, FixedPointerContractsStillRequireUnwrittenInputs) {
  for (const Arch A : Architectures)
    for (bool Tail : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A));
      SCOPED_TRACE(Tail);
      auto Image = image(A);
      auto Function = callWithSetup(A, 0, true, Tail);
      std::map<va_t, int> Registers{{Callee, registerCount(A)}};
      std::map<va_t, int> Total{{Callee, registerCount(A) + 2}};
      std::map<va_t, bool> Variadic{{Callee, false}};
      const std::map<va_t, PointerVariableCallee> Pointers{
          {Slot, {"", Callee}}};
      recoverCallAbi(Function, A, {}, &Image, &Registers, &Total, nullptr,
                     nullptr, nullptr, nullptr, &Variadic, nullptr, nullptr,
                     nullptr, nullptr, &Pointers);
      ASSERT_EQ(Function.CallInfos.size(), 1u);
      const auto &Call = Function.CallInfos.front();
      ASSERT_EQ(Call.Args.size(), registerCount(A) + 2);
      EXPECT_TRUE(Call.IsIndirect);
      EXPECT_EQ(Call.VarArgFixedCount, -1);
      for (unsigned I = registerCount(A); I < Call.Args.size(); ++I) {
        EXPECT_EQ(Call.Args[I].Kind,
                  Tail ? MedVar::Param : MedVar::Unspecified);
        EXPECT_EQ(
            Call.Args[I].Size,
            getTargetRegInfo(A).integerArgumentLayout(Image.Format).SlotBytes);
      }
    }
}

} // namespace
