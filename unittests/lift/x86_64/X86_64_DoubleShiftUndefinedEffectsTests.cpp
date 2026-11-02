//===- X86_64_DoubleShiftUndefinedEffectsTests.cpp - SHLD/SHRD audit -----===//
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

void checkDoubleShift(Arch Target, unsigned Width, bool Right,
                      unsigned Destination, unsigned Source, bool Immediate) {
  SCOPED_TRACE(::testing::Message()
               << unsigned(Target) << '/' << Width << '/' << Right << '/'
               << Destination << '/' << Source << '/' << Immediate);
  const unsigned Bits = Width * 8;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  std::vector<uint8_t> Bytes;
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Width == 8 || Destination >= 8 || Source >= 8)
    Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) | (Destination >= 8 ? 1 : 0) |
                    (Source >= 8 ? 4 : 0));
  Bytes.insert(Bytes.end(),
               {0x0f, uint8_t((Right ? 0xac : 0xa4) + !Immediate),
                uint8_t(0xc0 | (Source % 8) * 8 | Destination % 8)});
  if (Immediate)
    Bytes.push_back(0);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  LowInstructionUndefinedEffects Effects;
  std::vector<LowOp> Instrumented;
  for (unsigned RawCount = 0; RawCount != 256; ++RawCount) {
    SCOPED_TRACE(RawCount);
    if (RawCount == 0 || Immediate) {
      if (Immediate)
        Bytes.back() = RawCount;
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                int(Bytes.size()));
      std::vector<LowOp> Ops, Plain;
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
      Dec.liftToLow(Insn, Plain);
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
          << Effects.Diagnostic;
      EXPECT_EQ(Effects.OpCount, Ops.size());
      EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
      EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
      Instrumented.clear();
      for (size_t I = 0; I != Ops.size(); ++I) {
        Instrumented.push_back(Ops[I]);
        for (size_t J = 0; J != Effects.Effects.size(); ++J) {
          const auto &E = Effects.Effects[J];
          ASSERT_GT(E.AfterOp, 0U);
          ASSERT_LE(E.AfterOp, Ops.size());
          ASSERT_EQ(E.BitOffset, 0U);
          ASSERT_TRUE(E.Output.isReg());
          const bool Result = E.Output.Offset == Destination * 8;
          EXPECT_EQ(E.BitCount, Result ? 16U : 1U);
          EXPECT_EQ(E.Output.Size, Result ? 2U : 1U);
          if (E.AfterOp != I + 1)
            continue;
          LowOp Observe;
          Observe.Opcode = NdOp::COPY;
          Observe.Output = NdVar::reg(0x10000000 + J * 8, 1);
          Observe.addInput(E.When.value_or(NdVar::scalar(1, 1)));
          Instrumented.push_back(Observe);
        }
      }
    }
    const unsigned Count = RawCount % (Width == 8 ? 64 : 32);
    for (uint64_t AV : {UINT64_C(0), UINT64_C(1), Sign - 1, Sign, Mask,
                        UINT64_C(0x319da68eb27540cf) & Mask})
      for (uint64_t BV :
           {UINT64_C(0), Sign, Mask, UINT64_C(0xc672a9035b84de1f) & Mask})
        for (unsigned OldFlags : {0u, 127u}) {
          SCOPED_TRACE(::testing::Message()
                       << AV << '/' << BV << '/' << OldFlags);
          std::array<uint64_t, 16> Registers;
          for (unsigned I = 0; I != Registers.size(); ++I) {
            Registers[I] = UINT64_C(0xabcd987665431200) + I;
            if (Target == Arch::X86)
              Registers[I] &= UINT32_MAX;
          }
          Registers[Destination] = (Registers[Destination] & ~Mask) | AV;
          Registers[Source] = (Registers[Source] & ~Mask) | BV;
          if (!Immediate)
            Registers[1] = (Registers[1] & ~UINT64_C(255)) | RawCount;
          // Read the actual entry values after all overlapping assignments.
          const uint64_t A = Registers[Destination] & Mask;
          const uint64_t B = Registers[Source] & Mask;
          uint64_t Result = A, Incoming = B;
          unsigned Carry = OldFlags & 1;
          if (Count <= Bits)
            for (unsigned I = 0; I != Count; ++I) {
              // Independent repeated single-bit transfers avoid the lifter's
              // variable shift/complement formulas and host shift overflow.
              if (Right) {
                Carry = Result & 1;
                Result = Result / 2 + (Incoming & 1 ? Sign : 0);
                Incoming /= 2;
              } else {
                Carry = Result >= Sign;
                Result = (Result * 2 + (Incoming >= Sign)) & Mask;
                Incoming = (Incoming * 2) & Mask;
              }
            }
          SymContext Ctx;
          SymState State(Ctx);
          SymExec Exec(Ctx, State);
          for (unsigned I = 0; I != Registers.size(); ++I)
            State.write(SymSpace::Register, I * 8,
                        Ctx.mkConst(64, Registers[I]));
          for (unsigned I = 0; I != Flags.size(); ++I)
            State.write(SymSpace::Register, Flags[I],
                        Ctx.mkConst(8, (OldFlags >> I) & 1));
          ASSERT_EQ(Exec.run(Instrumented), Instrumented.size());
          ASSERT_EQ(Exec.unmodelledCount(), 0U);
          const auto Get = [&](uint64_t Offset, unsigned Size) {
            const auto V =
                Ctx.asConst(State.read(SymSpace::Register, Offset, Size));
            EXPECT_TRUE(V);
            return V ? V->getZExtValue() : UINT64_MAX;
          };
          const bool UndefinedResult = Count > Bits;
          Registers[Destination] = (Registers[Destination] & ~Mask) | Result;
          if (Width == 4 && Target == Arch::X64)
            Registers[Destination] &= UINT32_MAX;
          for (unsigned I = 0; I != Registers.size(); ++I) {
            const uint64_t Observed =
                UndefinedResult && I == Destination ? ~Mask : UINT64_MAX;
            EXPECT_EQ(Get(I * 8, 8) & Observed, Registers[I] & Observed)
                << "register " << I;
          }
          const std::array<bool, 7> Undefined = {
              UndefinedResult, UndefinedResult, Count != 0, UndefinedResult,
              UndefinedResult, false,           Count > 1};
          const std::array<unsigned, 7> Defined = {
              Carry,
              unsigned((std::popcount(uint8_t(Result)) & 1) == 0),
              0,
              unsigned(Result == 0),
              unsigned(Result >= Sign),
              (OldFlags >> 5) & 1,
              unsigned((Result >= Sign) != (A >= Sign))};
          std::array<unsigned, 7> Active{};
          unsigned ActiveResult = 0;
          for (size_t J = 0; J != Effects.Effects.size(); ++J) {
            const auto &E = Effects.Effects[J];
            const uint64_t When = Get(0x10000000 + J * 8, 1);
            ASSERT_LE(When, 1U);
            if (E.Output.Offset == Destination * 8) {
              ActiveResult += When;
            } else {
              const auto It =
                  std::find(Flags.begin(), Flags.end(), E.Output.Offset);
              ASSERT_NE(It, Flags.end());
              Active[It - Flags.begin()] += When;
            }
          }
          EXPECT_EQ(ActiveResult, unsigned(UndefinedResult));
          for (unsigned I = 0; I != Flags.size(); ++I) {
            EXPECT_EQ(Active[I], unsigned(Undefined[I])) << "flag " << I;
            if (!Undefined[I])
              EXPECT_EQ(Get(Flags[I], 1),
                        Count ? Defined[I] : (OldFlags >> I) & 1)
                  << "flag " << I;
          }
        }
  }
}

TEST(X86DoubleShiftUndefinedEffects, AllCountsAndWidthsMatchBitTransferOracle) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (bool Right : {false, true})
        for (bool Immediate : {false, true}) {
          checkDoubleShift(Target, Width, Right, 0, 2, Immediate);
          if (testing::Test::HasFatalFailure())
            return;
        }
    }
}

TEST(X86DoubleShiftUndefinedEffects,
     SourceDestinationAndCountAliasesRemainExact) {
  for (unsigned Width : {2u, 4u, 8u})
    for (bool Right : {false, true})
      for (const auto [D, S] : {std::pair{0u, 0u},
                                {1u, 2u},
                                {0u, 1u},
                                {1u, 1u},
                                {8u, 9u},
                                {9u, 1u}}) {
        checkDoubleShift(Arch::X64, Width, Right, D, S, false);
        if (testing::Test::HasFatalFailure())
          return;
      }
}

} // namespace
