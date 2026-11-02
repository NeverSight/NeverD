//===- X86_64_XaddAuditTests.cpp - XADD arithmetic audit -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

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

void checkXadd(Arch Target, unsigned Width, unsigned Destination,
               unsigned Source, unsigned ExtraRex = 0,
               bool ExhaustBytes = false) {
  SCOPED_TRACE(::testing::Message()
               << unsigned(Target) << '/' << Width << '/' << Destination << '/'
               << Source << '/' << ExtraRex);
  const unsigned Bits = Width * 8;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  const uint8_t Rex = Width == 8 || Destination >= 8 || Source >= 8 || ExtraRex
                          ? 0x40 | (Width == 8 ? 8 : 0) |
                                (Destination >= 8 ? 1 : 0) |
                                (Source >= 8 ? 4 : 0) | ExtraRex
                          : 0;
  std::vector<uint8_t> Bytes;
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Rex)
    Bytes.push_back(Rex);
  Bytes.insert(Bytes.end(),
               {0x0f, uint8_t(Width == 1 ? 0xc0 : 0xc1),
                uint8_t(0xc0 | ((Source % 8) << 3) | Destination % 8)});
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  Dec.liftToLow(Insn, Plain);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  const auto Offset = [&](unsigned Number) {
    return Width == 1 && !Rex && Number >= 4 ? (Number - 4) * 8 + 1
                                             : Number * 8;
  };
  const unsigned D = Offset(Destination), S = Offset(Source);
  std::vector<uint64_t> Values = {
      0,        1,        15,
      16,       Sign - 1, Sign,
      Mask - 1, Mask,     UINT64_C(0xa5c319087fed204b) & Mask};
  if (ExhaustBytes) {
    Values.clear();
    for (unsigned I = 0; I != 256; ++I)
      Values.push_back(I);
  }
  for (uint64_t DV : Values)
    for (uint64_t SV : Values)
      for (unsigned Old : {0u, 127u}) {
        if (ExhaustBytes && Old != 0)
          continue;
        const unsigned OldFlags = ExhaustBytes ? (DV + SV) % 128 : Old;
        SCOPED_TRACE(::testing::Message()
                     << DV << '/' << SV << '/' << OldFlags);
        std::array<uint64_t, 16> Registers;
        for (unsigned I = 0; I != Registers.size(); ++I) {
          Registers[I] = UINT64_C(0xfedcba9876543210) + I;
          if (Target == Arch::X86)
            Registers[I] &= UINT32_MAX;
        }
        const auto Write = [&](unsigned O, uint64_t V, bool Commit) {
          const unsigned Shift = (O % 8) * 8;
          const uint64_t Field = Mask << Shift;
          auto &R = Registers[O / 8];
          R = (R & ~Field) | ((V & Mask) << Shift);
          if (Commit && Width == 4 && Target == Arch::X64)
            R &= UINT32_MAX;
        };
        Write(D, DV, false);
        Write(S, SV, false);
        // Read both original register slices after setting overlapping inputs.
        const uint64_t A = (Registers[D / 8] >> ((D % 8) * 8)) & Mask;
        const uint64_t B = (Registers[S / 8] >> ((S % 8) * 8)) & Mask;
        SymContext Ctx;
        SymState State(Ctx);
        SymExec Exec(Ctx, State);
        for (unsigned I = 0; I != Registers.size(); ++I)
          State.write(SymSpace::Register, I * 8, Ctx.mkConst(64, Registers[I]));
        for (unsigned I = 0; I != Flags.size(); ++I)
          State.write(SymSpace::Register, Flags[I],
                      Ctx.mkConst(8, (OldFlags >> I) & 1));
        const uint64_t Sum = (A + B) & Mask;
        // Range comparisons avoid the lifter's XOR-based flag formulas and
        // avoid host signed overflow, including the 64-bit carry boundary.
        const bool Overflow =
            (A < Sign) == (B < Sign) && (Sum < Sign) != (A < Sign);
        const std::array<unsigned, 7> ExpectedFlags = {
            unsigned(B > Mask - A),
            unsigned((std::popcount(uint8_t(Sum)) & 1) == 0),
            unsigned((A % 16) + (B % 16) >= 16),
            unsigned(Sum == 0),
            unsigned(Sum >= Sign),
            (OldFlags >> 5) & 1,
            unsigned(Overflow)};
        Write(S, A, true);
        Write(D, Sum, true);
        ASSERT_EQ(Exec.run(Ops), Ops.size());
        ASSERT_EQ(Exec.unmodelledCount(), 0u);
        for (unsigned I = 0; I != Registers.size(); ++I) {
          const auto Actual =
              Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->getZExtValue(), Registers[I]) << "register " << I;
        }
        for (unsigned I = 0; I != Flags.size(); ++I) {
          const auto Actual =
              Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->getZExtValue(), ExpectedFlags[I])
              << "flag " << Flags[I];
        }
      }
}

TEST(X86XaddAudit, ArithmeticFlagsAndBothRegisterWritesAcrossAliases) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Target == Arch::X86 && Width == 8)
        continue;
      for (const auto [D, S] : {std::pair{0u, 1u},
                                {0u, 0u},
                                {0u, 4u},
                                {4u, 0u},
                                {4u, 4u},
                                {5u, 7u},
                                {7u, 5u},
                                {9u, 10u},
                                {8u, 8u},
                                {0u, 9u},
                                {9u, 0u},
                                {15u, 15u}}) {
        if (Target == Arch::X86 && (D >= 8 || S >= 8))
          continue;
        for (unsigned Rex : {0u, 0x40u, 0x48u}) {
          if (Rex && (Target == Arch::X86 || Width != 1))
            continue;
          checkXadd(Target, Width, D, S, Rex);
          if (testing::Test::HasFatalFailure())
            return;
        }
      }
    }
}

TEST(X86XaddAudit, EveryByteOperandPairMatchesArithmeticOracle) {
  checkXadd(Arch::X64, 1, 0, 1, 0, true);
}

struct MemoryCase {
  Arch Target;
  // ModRM (with an empty source field), SIB and displacement bytes. Expected
  // address components are specified independently of the decoder/lifter.
  std::vector<uint8_t> Address;
  int Base;
  int Index;
  unsigned Scale;
  int64_t Displacement;
  uint8_t Rex = 0;
  bool AddressPrefix = false;
};

void checkMemoryXadd(const MemoryCase &C, unsigned Width, unsigned Source,
                     bool ExhaustBytes = false) {
  SCOPED_TRACE(::testing::Message()
               << unsigned(C.Target) << '/' << Width << '/' << Source << '/'
               << C.Base << '/' << C.Index << '/' << C.Scale << '/'
               << C.Displacement << '/' << C.AddressPrefix);
  const bool Long = C.Target == Arch::X64;
  const unsigned AddressBytes =
      Long ? (C.AddressPrefix ? 4 : 8) : (C.AddressPrefix ? 2 : 4);
  const unsigned Bits = Width * 8;
  const uint64_t Mask = Width == 8 ? UINT64_MAX : (UINT64_C(1) << Bits) - 1;
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  uint8_t Rex = C.Rex | (Width == 8 ? 8 : 0) | (Source >= 8 ? 4 : 0);
  if (Rex)
    Rex |= 0x40;
  std::vector<uint8_t> Bytes;
  if (C.AddressPrefix)
    Bytes.push_back(0x67);
  if (Width == 2)
    Bytes.push_back(0x66);
  if (Rex)
    Bytes.push_back(Rex);
  Bytes.insert(Bytes.end(), {0x0f, uint8_t(Width == 1 ? 0xc0 : 0xc1)});
  const size_t ModRM = Bytes.size();
  Bytes.insert(Bytes.end(), C.Address.begin(), C.Address.end());
  Bytes[ModRM] |= (Source % 8) << 3;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(C.Target));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
  Dec.liftToLow(Insn, Plain);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  const unsigned S =
      Width == 1 && !Rex && Source >= 4 ? (Source - 4) * 8 + 1 : Source * 8;
  std::vector<uint64_t> Values = {
      0,        1,        15,
      16,       Sign - 1, Sign,
      Mask - 1, Mask,     UINT64_C(0xa5c319087fed204b) & Mask};
  if (ExhaustBytes) {
    Values.clear();
    for (unsigned I = 0; I != 256; ++I)
      Values.push_back(I);
  }
  for (uint64_t A : Values)
    for (uint64_t B : Values)
      for (unsigned OldFlags : {0u, 127u}) {
        if (ExhaustBytes && OldFlags)
          continue;
        SCOPED_TRACE(::testing::Message() << A << '/' << B << '/' << OldFlags);
        std::array<uint64_t, 16> Registers;
        for (unsigned I = 0; I != Registers.size(); ++I) {
          Registers[I] = UINT64_C(0xaabbccdd00004021) + I * 16;
          if (!Long)
            Registers[I] &= UINT32_MAX;
        }
        const unsigned Shift = (S % 8) * 8;
        const uint64_t Field = Mask << Shift;
        Registers[S / 8] = (Registers[S / 8] & ~Field) | (B << Shift);
        // Address/source overlap uses the complete entry register value.
        uint64_t Address = C.Displacement;
        if (C.Base == -2)
          Address += 0x1000 + Bytes.size();
        else if (C.Base >= 0)
          Address += Registers[C.Base];
        if (C.Index >= 0)
          Address += Registers[C.Index] * C.Scale;
        if (AddressBytes < 8)
          Address &= (UINT64_C(1) << (AddressBytes * 8)) - 1;
        // Keep the sentinel storage nonwrapping. This checks lifted operations,
        // not native access permissions or exception delivery.
        if (Address == 0 || Address > UINT64_MAX - Width - 1)
          continue;
        SymContext Ctx;
        SymState State(Ctx);
        SymExec Exec(Ctx, State);
        for (unsigned I = 0; I != Registers.size(); ++I)
          State.write(SymSpace::Register, I * 8, Ctx.mkConst(64, Registers[I]));
        for (unsigned I = 0; I != Flags.size(); ++I)
          State.write(SymSpace::Register, Flags[I],
                      Ctx.mkConst(8, (OldFlags >> I) & 1));
        State.store(Ctx.mkConst(64, Address - 1), Ctx.mkConst(8, 0x39));
        State.store(Ctx.mkConst(64, Address), Ctx.mkConst(Bits, A));
        State.store(Ctx.mkConst(64, Address + Width), Ctx.mkConst(8, 0xc5));
        const uint64_t Sum = (A + B) & Mask;
        const std::array<unsigned, 7> ExpectedFlags = {
            unsigned(B > Mask - A),
            unsigned((std::popcount(uint8_t(Sum)) & 1) == 0),
            unsigned((A % 16) + (B % 16) >= 16),
            unsigned(Sum == 0),
            unsigned(Sum >= Sign),
            (OldFlags >> 5) & 1,
            unsigned((A < Sign) == (B < Sign) && (Sum < Sign) != (A < Sign))};
        Registers[S / 8] = (Registers[S / 8] & ~Field) | (A << Shift);
        if (Width == 4 && Long)
          Registers[S / 8] &= UINT32_MAX;
        ASSERT_EQ(Exec.run(Ops), Ops.size());
        ASSERT_EQ(Exec.unmodelledCount(), 0U);
        for (unsigned I = 0; I != Registers.size(); ++I) {
          const auto Actual =
              Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->getZExtValue(), Registers[I]) << "register " << I;
        }
        for (unsigned I = 0; I != Flags.size(); ++I) {
          const auto Actual =
              Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
          ASSERT_TRUE(Actual);
          EXPECT_EQ(Actual->getZExtValue(), ExpectedFlags[I]) << "flag " << I;
        }
        for (unsigned I = 0; I != Width + 2; ++I) {
          const auto Actual =
              Ctx.asConst(State.load(Ctx.mkConst(64, Address - 1 + I), 1));
          ASSERT_TRUE(Actual);
          const uint8_t Expected = I == 0           ? 0x39
                                   : I == Width + 1 ? 0xc5
                                                    : Sum >> ((I - 1) * 8);
          EXPECT_EQ(Actual->getZExtValue(), Expected) << "memory byte " << I;
        }
      }
}

TEST(X86XaddAudit, MemoryFlagsAndWritebacksUseEntryAddressAcrossWidths) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const MemoryCase &C : {
             MemoryCase{Target, {0x00}, 0, -1, 1, 0},
             {Target, {0x46, 0xfd}, 6, -1, 1, -3},
             {Target, {0x84, 0x94, 0x99, 0xba, 0xff, 0xff}, 4, 2, 4, -17767},
             {Target, {0x04, 0xe4}, 4, -1, 1, 0},
             {Target, {0x04, 0x4d, 0, 0x20, 0, 0}, -1, 1, 2, 0x2000},
         })
      for (unsigned Width : {1u, 2u, 4u, 8u}) {
        if (Width == 8 && Target == Arch::X86)
          continue;
        for (unsigned Source : {0u, 2u, 4u, 6u}) {
          checkMemoryXadd(C, Width, Source);
          if (testing::Test::HasFatalFailure())
            return;
        }
      }
}

TEST(X86XaddAudit, MemoryAddressOverridesExtensionsAndRelativeAddresses) {
  for (const MemoryCase &C : {
           MemoryCase{Arch::X64,
                      {0x84, 0xd5, 0x63, 0xff, 0xff, 0xff},
                      13,
                      10,
                      8,
                      -157,
                      3},
           {Arch::X64,
            {0x84, 0xd5, 0x63, 0xff, 0xff, 0xff},
            13,
            10,
            8,
            -157,
            3,
            true},
           {Arch::X64, {0x05, 0xff, 0x1f, 0, 0}, -2, -1, 1, 8191},
           {Arch::X64,
            {0x05, 0xf1, 0xee, 0xff, 0xff},
            -2,
            -1,
            1,
            -4367,
            0,
            true},
           {Arch::X86, {0x05, 0xff, 0xff, 0xff, 0x7f}, -1, -1, 1, INT32_MAX},
           {Arch::X86, {0x80, 0x63, 0xff}, 3, 6, 1, -157, 0, true},
           {Arch::X86, {0x43, 0xfd}, 5, 7, 1, -3, 0, true},
           {Arch::X86, {0x06, 0xff, 0x7f}, -1, -1, 1, 32767, 0, true},
       })
    for (unsigned Width : {1u, 2u, 4u, 8u}) {
      if (Width == 8 && C.Target == Arch::X86)
        continue;
      for (unsigned Source : {0u, 4u, 6u, 10u, 13u}) {
        if (Source >= 8 && C.Target == Arch::X86)
          continue;
        checkMemoryXadd(C, Width, Source);
        if (testing::Test::HasFatalFailure())
          return;
      }
    }
}

TEST(X86XaddAudit, EveryMemoryBytePairMatchesArithmeticOracle) {
  checkMemoryXadd({Arch::X64, {0x07}, 7, -1, 1, 0}, 1, 0, true);
}

TEST(X86XaddAudit, EveryI386Address16BaseAndDisplacementForm) {
  constexpr int Bases[] = {3, 3, 5, 5, 6, 7, 5, 3};
  constexpr int Indices[] = {6, 7, 6, 7, -1, -1, -1, -1};
  for (unsigned Mod = 0; Mod != 3; ++Mod)
    for (unsigned RM = 0; RM != 8; ++RM) {
      MemoryCase C{
          Arch::X86, {uint8_t(Mod * 64 + RM)}, Bases[RM], Indices[RM], 1, 0, 0,
          true};
      if (Mod == 0 && RM == 6) {
        C.Base = -1;
        C.Displacement = -1;
        C.Address.insert(C.Address.end(), {0xff, 0xff});
      } else if (Mod == 1) {
        C.Displacement = -128;
        C.Address.push_back(0x80);
      } else if (Mod == 2) {
        C.Displacement = -32768;
        C.Address.insert(C.Address.end(), {0, 0x80});
      }
      checkMemoryXadd(C, 2, 3);
      if (testing::Test::HasFatalFailure())
        return;
    }
}

} // namespace
