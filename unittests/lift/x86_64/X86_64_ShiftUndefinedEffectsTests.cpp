//===- X86_64_ShiftUndefinedEffectsTests.cpp - Count-dependent flags -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

// All fixtures are independently assembled scalar operations. Forms are
// accumulator, count/destination alias, high byte, memory, immediate,
// implicit 1.
void checkShiftOrRotate(Arch Target, unsigned Width, unsigned Group,
                        unsigned Form) {
  SCOPED_TRACE(::testing::Message() << unsigned(Target) << '/' << Width << '/'
                                    << Group << '/' << Form);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  const bool Rotate = Group < 2;
  const unsigned Bits = Width * 8;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  const bool Memory = Form == 3, Immediate = Form == 4, One = Form == 5;
  const bool HighByte = Form == 2 || Form >= 7;
  const unsigned Reg = Form == 1   ? x86reg::RCX
                       : Form == 6 ? x86reg::R8
                       : Form >= 7 ? (Form - 6) * 8
                                   : x86reg::RAX;
  const unsigned ByteOffset = HighByte ? 1 : 0;
  const unsigned RawRM = Memory ? 3 : HighByte ? Reg / 8 + 4 : Reg / 8 % 8;
  std::vector<uint8_t> Bytes;
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Width == 8 || Form == 6)
    Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Form == 6 ? 1 : 0));
  Bytes.push_back((Immediate ? 0xc0 : One ? 0xd0 : 0xd2) + (Width != 1));
  Bytes.push_back((Memory ? 0 : 0xc0) | (Group << 3) | RawRM);
  if (Immediate)
    Bytes.push_back(0);
  std::vector<LowOp> Instrumented;
  LowInstructionUndefinedEffects Effects;
  for (unsigned RawCount = 0; RawCount < 256; ++RawCount) {
    if (One && RawCount != 1)
      continue;
    SCOPED_TRACE(RawCount);
    if (RawCount == 0 || Immediate || One) {
      if (Immediate)
        Bytes.back() = RawCount;
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                static_cast<int>(Bytes.size()));
      std::vector<LowOp> Ops, Plain;
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects));
      ASSERT_NO_THROW(Dec.liftToLow(Insn, Plain));
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
          << Effects.Diagnostic;
      ASSERT_EQ(Effects.OpCount, Ops.size());
      ASSERT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
      ASSERT_EQ(lowUndefinedOperationDigest(Plain), Effects.OperationDigest);
      Instrumented.clear();
      for (size_t I = 0; I < Ops.size(); ++I) {
        Instrumented.push_back(Ops[I]);
        for (size_t J = 0; J < Effects.Effects.size(); ++J) {
          const auto &E = Effects.Effects[J];
          ASSERT_GT(E.AfterOp, 0u);
          ASSERT_LE(E.AfterOp, Ops.size());
          ASSERT_EQ(E.BitOffset, 0u);
          ASSERT_EQ(E.BitCount, 1u);
          ASSERT_EQ(E.Output.Size, 1u);
          ASSERT_TRUE(E.Output.isReg());
          if (E.AfterOp != I + 1)
            continue;
          LowOp Observe;
          Observe.Opcode = NdOp::COPY;
          Observe.Output = NdVar::reg(0x10000000 + J * 8, 1);
          Observe.addInput(E.When.value_or(NdVar::scalar(1, 1)));
          Instrumented.push_back(Observe);
        }
      }
      if (Memory) {
        LowOp Reload;
        Reload.Opcode = NdOp::LOAD;
        Reload.Output = NdVar::reg(x86reg::RDX, Width);
        Reload.addInput(NdVar::scalar(0x4000, Target == Arch::X64 ? 8 : 4));
        Instrumented.push_back(Reload);
      }
    }
    const unsigned Count = RawCount % (Width == 8 ? 64 : 32);
    for (uint64_t Value : {UINT64_C(0), UINT64_C(1), Sign - 1, Sign, Mask,
                           UINT64_C(0xa55a18e17c81fe04) & Mask}) {
      // Alias fixtures necessarily take their count from the original operand.
      if (Form == 1)
        Value = (Value & ~UINT64_C(255)) | RawCount;
      Value &= Mask;
      for (unsigned Old = 0; Old < 128; ++Old) {
        if (Count && Old != 0 && Old != 127)
          continue;
        SCOPED_TRACE(::testing::Message() << Value << '/' << Old);
        SymContext Ctx;
        SymState State(Ctx);
        SymExec Exec(Ctx, State);
        auto Set = [&](uint64_t Offset, uint64_t V, unsigned Bytes = 8) {
          State.write(SymSpace::Register, Offset, Ctx.mkConst(Bytes * 8, V));
        };
        auto Get = [&](uint64_t Offset, unsigned Bytes = 8) {
          const auto Ref = State.read(SymSpace::Register, Offset, Bytes);
          const auto Constant = Ctx.asConst(Ref);
          EXPECT_TRUE(Constant.has_value()) << Ctx.toString(Ref);
          return Constant ? Constant->getZExtValue() : UINT64_MAX;
        };
        if (Memory) {
          ASSERT_TRUE(
              State.store(Ctx.mkConst(Target == Arch::X64 ? 64 : 32, 0x4000),
                          Ctx.mkConst(Bits, Value)));
          Set(x86reg::RDX, 0);
        }
        const uint64_t WriteMask = Mask << (ByteOffset * 8);
        uint64_t Initial = (UINT64_C(0xfedcba9876543210) & ~WriteMask) |
                           (Value << (ByteOffset * 8));
        if (Reg == x86reg::RCX && HighByte)
          Initial = (Initial & ~UINT64_C(255)) | RawCount;
        if (Target == Arch::X86)
          Initial &= UINT32_MAX;
        Set(x86reg::RCX, RawCount | UINT64_C(0x9af00000));
        Set(Reg, Initial);
        if (Memory)
          Set(x86reg::RBX, 0x4000);
        for (unsigned I = 0; I < Flags.size(); ++I)
          Set(Flags[I], (Old >> I) & 1, 1);
        ASSERT_EQ(Exec.run(Instrumented), Instrumented.size());
        ASSERT_EQ(Exec.unmodelledCount(), 0u);
        // Repeated single-bit unsigned arithmetic is independent of the
        // lifter's variable shift and carry-index formulas, including SAR.
        uint64_t Expected = Value;
        unsigned Carry = Old & 1;
        for (unsigned I = 0; I < Count; ++I) {
          Carry = Group == 0 || Group == 4 || Group == 6 ? (Expected >= Sign)
                                                         : (Expected & 1);
          if (Group == 0)
            Expected = (Expected * 2 + Carry) & Mask;
          else if (Group == 1)
            Expected = Expected / 2 + (Carry ? Sign : 0);
          else if (Group == 4 || Group == 6)
            Expected = (Expected * 2) & Mask;
          else
            Expected = Expected / 2 + (Group == 7 ? Expected & Sign : 0);
        }
        uint64_t Whole =
            (Initial & ~WriteMask) | (Expected << (ByteOffset * 8));
        if (Width == 4 && Target == Arch::X64)
          Whole = Expected; // Also required when the masked count is zero.
        if (Memory)
          EXPECT_EQ(Get(x86reg::RDX) & Mask, Expected);
        else
          EXPECT_EQ(Get(Reg), Whole);
        std::array<bool, 7> Undefined = {!Rotate && Group != 7 && Count >= Bits,
                                         false,
                                         !Rotate && Count != 0,
                                         false,
                                         false,
                                         false,
                                         Count > 1};
        const std::array<unsigned, 7> Defined = {
            Carry,
            Rotate ? (Old >> 1) & 1
                   : unsigned((std::popcount(uint8_t(Expected)) & 1) == 0),
            Rotate ? (Old >> 2) & 1 : 0,
            Rotate ? (Old >> 3) & 1 : unsigned(Expected == 0),
            Rotate ? (Old >> 4) & 1 : unsigned(Expected >= Sign),
            (Old >> 5) & 1,
            Group == 1
                ? unsigned((Expected >= Sign) != bool(Expected & (Sign / 2)))
            : Group == 7 ? 0
            : Group == 5 ? unsigned(Value >= Sign)
                         : unsigned((Expected >= Sign) != Carry)};
        std::array<unsigned, 7> Active{};
        for (size_t J = 0; J < Effects.Effects.size(); ++J) {
          const auto &E = Effects.Effects[J];
          const auto When = Get(0x10000000 + J * 8, 1);
          ASSERT_LE(When, 1u);
          const auto It =
              std::find(Flags.begin(), Flags.end(), E.Output.Offset);
          ASSERT_NE(It, Flags.end());
          Active[It - Flags.begin()] += When;
        }
        for (unsigned I = 0; I < Flags.size(); ++I) {
          EXPECT_EQ(Active[I], unsigned(Undefined[I]));
          if (!Undefined[I])
            EXPECT_EQ(Get(Flags[I], 1), Count ? Defined[I] : (Old >> I) & 1)
                << "flag " << Flags[I];
        }
      }
    }
  }
}

TEST(X86ShiftUndefinedEffects,
     DefinedResultsAndPredicatesMatchArithmeticOracle) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (unsigned Group : {4u, 5u, 6u, 7u})
        for (unsigned Form = 0; Form < 10; ++Form) {
          if (((Form == 2 || Form >= 7) && Width != 1) ||
              (Form == 6 && Target == Arch::X86))
            continue;
          checkShiftOrRotate(Target, Width, Group, Form);
          if (testing::Test::HasFatalFailure())
            return;
        }
    }
}

TEST(X86RotateUndefinedEffects,
     DefinedResultsAndMaskedCountPredicatesMatchArithmeticOracle) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (unsigned Group : {0u, 1u})
        for (unsigned Form = 0; Form < 10; ++Form) {
          if (((Form == 2 || Form >= 7) && Width != 1) ||
              (Form == 6 && Target == Arch::X86))
            continue;
          checkShiftOrRotate(Target, Width, Group, Form);
          if (testing::Test::HasFatalFailure())
            return;
        }
    }
}
} // namespace
