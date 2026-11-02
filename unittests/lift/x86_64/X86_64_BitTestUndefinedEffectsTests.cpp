//===- X86_64_BitTestUndefinedEffectsTests.cpp - Bit-test audit -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

void checkBitTest(Arch Target, unsigned Width, unsigned Kind, unsigned Base,
                  unsigned Index, bool Immediate) {
  SCOPED_TRACE(::testing::Message()
               << unsigned(Target) << '/' << Width << '/' << Kind << '/' << Base
               << '/' << Index << '/' << Immediate);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  const unsigned Bits = Width * 8;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  std::vector<uint8_t> Bytes;
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Width == 8 || Base >= 8 || (!Immediate && Index >= 8))
    Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Base >= 8 ? 1 : 0) |
                    (!Immediate && Index >= 8 ? 4 : 0));
  Bytes.push_back(0x0f);
  Bytes.push_back(Immediate ? 0xba : 0xa3 + Kind * 8);
  Bytes.push_back(0xc0 | ((Immediate ? Kind + 4 : Index % 8) << 3) | Base % 8);
  if (Immediate)
    Bytes.push_back(0);
  for (uint64_t RawIndex :
       {UINT64_C(0), UINT64_C(1), UINT64_C(15), UINT64_C(16), UINT64_C(31),
        UINT64_C(32), UINT64_C(63), UINT64_C(64), UINT64_C(127), UINT64_C(128),
        UINT64_C(255), UINT64_C(1) << 32, UINT64_MAX - 1, UINT64_MAX}) {
    if (Immediate)
      Bytes.back() = RawIndex;
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              int(Bytes.size()));
    LowInstructionUndefinedEffects Effects;
    std::vector<LowOp> Ops, Plain;
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
    Dec.liftToLow(Insn, Plain);
    ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
        << Effects.Diagnostic;
    ASSERT_EQ(Effects.OpCount, Ops.size());
    ASSERT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
    ASSERT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
    std::array<unsigned, 7> Active{};
    for (const auto &E : Effects.Effects) {
      ASSERT_EQ(E.BitCount, 1u);
      ASSERT_EQ(E.BitOffset, 0u);
      ASSERT_FALSE(E.When);
      ASSERT_GT(E.AfterOp, 0u);
      ASSERT_LE(E.AfterOp, Ops.size());
      bool Found = false;
      for (size_t I = 0; I != Flags.size(); ++I)
        if (E.Output == NdVar::reg(Flags[I], 1)) {
          ++Active[I];
          Found = true;
        }
      ASSERT_TRUE(Found);
    }
    EXPECT_EQ(Active, (std::array<unsigned, 7>{0, 1, 1, 0, 1, 0, 1}));
    for (uint64_t Value : {UINT64_C(0), UINT64_C(1), Sign - 1, Sign, Mask,
                           UINT64_C(0xa5195ac3fe817204) & Mask}) {
      if (!Immediate && Base == Index)
        Value = RawIndex & Mask;
      for (unsigned Old = 0; Old != 128; ++Old) {
        SymContext Ctx;
        SymState State(Ctx);
        SymExec Exec(Ctx, State);
        const auto Set = [&](uint64_t Offset, uint64_t V, unsigned Size = 8) {
          State.write(SymSpace::Register, Offset, Ctx.mkConst(Size * 8, V));
        };
        const uint64_t Initial = (UINT64_C(0xaabbccddeeff1234) & ~Mask) | Value;
        if (!Immediate)
          Set(Index * 8, RawIndex);
        Set(Base * 8, Target == Arch::X86 ? Initial & UINT32_MAX : Initial);
        for (unsigned I = 0; I != Flags.size(); ++I)
          Set(Flags[I], (Old >> I) & 1, 1);
        ASSERT_EQ(Exec.run(Ops), Ops.size());
        ASSERT_EQ(Exec.unmodelledCount(), 0u);
        const unsigned Bit = RawIndex & (Bits - 1);
        // Unsigned division observes the old bit before a set/reset/toggle.
        const uint64_t Power = UINT64_C(1) << Bit;
        const unsigned Carry = (Value / Power) % 2;
        const uint64_t Updated = Kind == 0   ? Value
                                 : Kind == 1 ? Value | Power
                                 : Kind == 2 ? Value & ~Power
                                             : Value ^ Power;
        uint64_t Expected = (Initial & ~Mask) | Updated;
        if (Target == Arch::X86 || (Width == 4 && Kind != 0))
          Expected &= UINT32_MAX;
        const auto Result =
            Ctx.asConst(State.read(SymSpace::Register, Base * 8, 8));
        ASSERT_TRUE(Result);
        EXPECT_EQ(Result->getZExtValue(), Expected);
        for (unsigned I : {0u, 3u, 5u}) {
          const auto Flag =
              Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
          ASSERT_TRUE(Flag);
          EXPECT_EQ(Flag->getZExtValue(), I == 0 ? Carry : (Old >> I) & 1);
        }
        if (!Immediate && Base != Index) {
          const auto Unchanged =
              Ctx.asConst(State.read(SymSpace::Register, Index * 8, 8));
          ASSERT_TRUE(Unchanged);
          EXPECT_EQ(Unchanged->getZExtValue(), RawIndex);
        }
      }
    }
  }
}

TEST(X86BitTestUndefinedEffects, ResultsFlagsAliasesAndUpperRegisters) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (unsigned Kind = 0; Kind != 4; ++Kind)
        for (const auto [Base, Index] : {std::pair{0u, 1u},
                                         {1u, 1u},
                                         {3u, 0u},
                                         {9u, 10u},
                                         {8u, 8u},
                                         {0u, 9u},
                                         {9u, 0u}}) {
          if (Target == Arch::X86 && (Base >= 8 || Index >= 8))
            continue;
          for (bool Immediate : {false, true}) {
            checkBitTest(Target, Width, Kind, Base, Index, Immediate);
            if (testing::Test::HasFatalFailure())
              return;
          }
        }
    }
}

} // namespace
