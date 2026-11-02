//===- X86MpxContextTests.cpp - MPX state and table regressions ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent enable, bounds, table-address and fault-state expectations.
/// These tests do not execute MPX on the host or authorize strict lifting.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/X86/X86MpxContext.h"

#include <array>
#include <cstdint>
#include <optional>

using namespace neverd;

namespace {

using Mode = X86MpxMode;
using Result = X86MpxResult;
using Check = X86MpxCheck;
using Branch = X86MpxBranch;

X86MpxConfiguration configuration(unsigned CPL = 3) {
  X86MpxConfiguration C;
  C.Supported = true;
  C.OSXSAVE = true;
  C.XCR0 = 0x1f;
  C.PrivilegeLevel = CPL;
  C.UserConfig = 0x12345001;
  C.SupervisorConfig = 0x98765001;
  C.UserAddressWidthAdjust = 0;
  C.InTransaction = false;
  return C;
}

X86MpxBounds known(uint64_t Lower, uint64_t Upper) {
  return {{Lower, UINT64_MAX}, {Upper, UINT64_MAX}};
}

std::array<X86MpxBounds, 4> snapshot(const X86MpxContext &C) {
  return {*C.bounds(0), *C.bounds(1), *C.bounds(2), *C.bounds(3)};
}

X86MpxContext seeded() {
  X86MpxContext C;
  EXPECT_TRUE(C.configure(configuration()));
  for (unsigned I = 0; I != 4; ++I)
    EXPECT_TRUE(C.setBounds(
        I, known(0x1234567800000100ull + I, 0x8765432100000200ull + I)));
  C.setStatus(0x76543212);
  return C;
}

TEST(X86MpxContext, EnableRequiresEvidenceAndSelectsTheCurrentPrivilegeBank) {
  // Each prerequisite independently ranges over unknown, disabled, enabled.
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (unsigned Feature = 0; Feature != 3; ++Feature)
      for (unsigned OS = 0; OS != 3; ++OS)
        for (unsigned Xcr = 0; Xcr != 3; ++Xcr)
          for (unsigned Config = 0; Config != 3; ++Config) {
            X86MpxConfiguration S;
            S.PrivilegeLevel = CPL;
            if (Feature)
              S.Supported = Feature == 2;
            if (OS)
              S.OSXSAVE = OS == 2;
            if (Xcr)
              S.XCR0 = Xcr == 2 ? 0x18 : 0x08;
            auto &Active = CPL == 3 ? S.UserConfig : S.SupervisorConfig;
            auto &Inactive = CPL == 3 ? S.SupervisorConfig : S.UserConfig;
            Inactive = Config == 2 ? 0 : 1;
            if (Config)
              Active = Config == 2 ? 1 : 0;
            X86MpxContext C;
            ASSERT_TRUE(C.configure(S));
            const bool Disabled =
                Feature == 1 || OS == 1 || Xcr == 1 || Config == 1;
            const bool Enabled =
                Feature == 2 && OS == 2 && Xcr == 2 && Config == 2;
            EXPECT_EQ(C.enabled(), Disabled  ? std::optional<bool>(false)
                                   : Enabled ? std::optional<bool>(true)
                                             : std::nullopt);
          }
  for (uint64_t Xcr : std::array<uint64_t, 5>{0, 8, 16, 24, UINT64_MAX}) {
    auto S = configuration();
    S.XCR0 = Xcr;
    X86MpxContext C;
    ASSERT_TRUE(C.configure(S));
    EXPECT_EQ(C.enabled(), (Xcr & 24) == 24);
  }
}

TEST(X86MpxContext, InvalidSnapshotsLeaveThePriorContextAndBankIntact) {
  auto C = seeded();
  const auto Before = snapshot(C);
  for (unsigned Bit = 2; Bit != 12; ++Bit)
    for (bool User : {false, true}) {
      auto S = configuration();
      (User ? S.UserConfig : S.SupervisorConfig) = (1ull << Bit) | 1;
      EXPECT_FALSE(C.configure(S));
      EXPECT_EQ(C.enabled(), true);
      EXPECT_EQ(snapshot(C), Before);
      EXPECT_EQ(C.status(), 0x76543212u);
    }
  for (unsigned CPL = 4; CPL != 256; ++CPL) {
    auto S = configuration();
    S.PrivilegeLevel = CPL;
    EXPECT_FALSE(C.configure(S));
  }
  for (unsigned Adjust = 17; Adjust != 256; ++Adjust) {
    auto S = configuration();
    S.UserAddressWidthAdjust = Adjust;
    EXPECT_FALSE(C.configure(S));
  }
  EXPECT_EQ(C.directoryAddress(Mode::Bits64, 0).Address, 0x12345000u);
}

TEST(X86MpxContext, UnknownAndDisabledContextsNeverInventStateOrOperands) {
  for (bool Disabled : {false, true}) {
    auto C = seeded();
    X86MpxConfiguration S;
    if (Disabled)
      S.Supported = false;
    ASSERT_TRUE(C.configure(S));
    const auto Before = snapshot(C);
    const auto Expected = Disabled ? Result::Disabled : Result::UnknownContext;
    for (auto M : {Mode::Bits32, Mode::Bits64}) {
      EXPECT_EQ(C.operationStatus(M, 0), Expected);
      EXPECT_EQ(C.move(M, 0, 1), Expected);
      EXPECT_EQ(C.make(M, 0, {}, {}), Expected);
      EXPECT_EQ(C.check(M, Check::Lower, 0, {}), Expected);
      EXPECT_EQ(C.completeLoad(M, 0, 0, 0), Expected);
      EXPECT_EQ(C.storeValue(M, 0).Result, Expected);
      EXPECT_EQ(C.directoryAddress(M, {}).Result, Expected);
      EXPECT_EQ(C.tableAddress(M, 0, 0).Result, Expected);
      EXPECT_EQ(C.completeTableLoad(M, 0, 1, 2, 3, 4), Expected);
      EXPECT_EQ(C.tableReadFault(M, 0), Expected);
      EXPECT_EQ(C.completeBranch(Branch::Initialize), Expected);
      EXPECT_EQ(C.completeLegacyBoundsFault(), Expected);
      EXPECT_EQ(snapshot(C), Before);
      EXPECT_EQ(C.status(), 0x76543212u);
    }
    // An architectural preserve has no dependency on enable/context knownness.
    EXPECT_EQ(C.completeBranch(Branch::Preserve), Result::Completed);
    EXPECT_EQ(snapshot(C), Before);
  }
}

TEST(X86MpxContext,
     TransactionsRequireTheOriginalCheckpointBeforeStateChanges) {
  for (bool Known : {false, true}) {
    auto C = seeded();
    auto S = configuration();
    S.InTransaction = Known ? std::optional<bool>(true) : std::nullopt;
    ASSERT_TRUE(C.configure(S));
    EXPECT_EQ(C.enabled(), true);
    const auto Before = snapshot(C);
    const auto Expected =
        Known ? Result::UnsupportedTransaction : Result::UnknownContext;
    EXPECT_EQ(C.operationStatus(Mode::Bits64, 0), Expected);
    EXPECT_EQ(C.move(Mode::Bits64, 0, 1), Expected);
    EXPECT_EQ(C.make(Mode::Bits64, 0, 0, 0), Expected);
    EXPECT_EQ(C.check(Mode::Bits64, Check::Lower, 0, 0), Expected);
    EXPECT_EQ(C.completeLoad(Mode::Bits64, 0, 0, 0), Expected);
    EXPECT_EQ(C.storeValue(Mode::Bits64, 0).Result, Expected);
    EXPECT_EQ(C.directoryAddress(Mode::Bits64, 0).Result, Expected);
    EXPECT_EQ(C.tableAddress(Mode::Bits64, 0, 0).Result, Expected);
    EXPECT_EQ(C.completeTableLoad(Mode::Bits64, 0, 1, 2, 3, 4), Expected);
    EXPECT_EQ(C.tableReadFault(Mode::Bits64, 0), Expected);
    EXPECT_EQ(C.completeBranch(Branch::Initialize), Expected);
    EXPECT_EQ(C.completeLegacyBoundsFault(), Expected);
    EXPECT_EQ(snapshot(C), Before);
    EXPECT_EQ(C.status(), 0x76543212u);
    EXPECT_EQ(C.completeBranch(Branch::Preserve), Result::Completed);
  }
}

TEST(X86MpxContext, BoundsCopiesPreserveKnownnessIncludingAliasedBanks) {
  for (auto M : {Mode::Bits32, Mode::Bits64})
    for (unsigned From = 0; From != 4; ++From)
      for (unsigned To = 0; To != 4; ++To) {
        auto C = seeded();
        const auto Before = snapshot(C);
        EXPECT_EQ(C.move(M, To, From), Result::Completed);
        auto Expected = Before[From];
        if (M == Mode::Bits32) {
          Expected.Lower.KnownMask = UINT32_MAX;
          Expected.Upper.KnownMask = UINT32_MAX;
        }
        EXPECT_EQ(C.bounds(To), Expected);
        for (unsigned I = 0; I != 4; ++I)
          if (I != To)
            EXPECT_EQ(C.bounds(I), Before[I]);
      }
  auto C = seeded();
  ASSERT_TRUE(C.setBounds(2, {{1, 1}, {0, 0}}));
  EXPECT_EQ(C.move(Mode::Bits64, 3, 2), Result::Completed);
  EXPECT_EQ(C.bounds(3), C.bounds(2));
  EXPECT_EQ(C.check(Mode::Bits64, Check::Lower, 3, 42), Result::UnknownOperand);
  EXPECT_EQ(C.status(), 0x76543212u);
}

TEST(X86MpxContext, MakeUsesUnsegmentedFullWidthInputsAndInclusiveBounds) {
  auto C = seeded();
  const auto Before = snapshot(C);
  EXPECT_EQ(C.make(Mode::Bits64, 2, {}, 0), Result::UnknownOperand);
  EXPECT_EQ(C.make(Mode::Bits64, 2, 0, {}), Result::UnknownOperand);
  EXPECT_EQ(snapshot(C), Before);
  EXPECT_EQ(
      C.make(Mode::Bits64, 2, 0x123456789abcdef0ull, 0xfedcba9876543210ull),
      Result::Completed);
  EXPECT_EQ(C.bounds(2), known(0x123456789abcdef0ull, 0x0123456789abcdefull));
  EXPECT_EQ(
      C.make(Mode::Bits32, 2, 0x123456789abcdef0ull, 0xfedcba9876543210ull),
      Result::Completed);
  EXPECT_EQ(C.bounds(2),
            (X86MpxBounds{{0x9abcdef0, UINT32_MAX}, {0x89abcdef, UINT32_MAX}}));
  EXPECT_EQ(C.check(Mode::Bits32, Check::Lower, 2, 0x9abcdef0),
            Result::Completed);
  // Crossing modes cannot convert an unqualified high half into zero.
  EXPECT_EQ(C.check(Mode::Bits64, Check::Lower, 2, 0x9abcdef0),
            Result::UnknownOperand);
  EXPECT_EQ(C.status(), 0x76543212u);
}

TEST(X86MpxContext, CompletedMemoryLoadsPublishBothFieldsAndNoOtherBank) {
  for (auto M : {Mode::Bits32, Mode::Bits64})
    for (unsigned Destination = 0; Destination != 4; ++Destination) {
      auto C = seeded();
      const auto Before = snapshot(C);
      EXPECT_EQ(C.completeLoad(M, Destination, 0x8877665544332211ull,
                               0x0123456789abcdefull),
                Result::Completed);
      const auto Bound = *C.bounds(Destination);
      EXPECT_EQ(Bound.Lower.Value,
                M == Mode::Bits64 ? 0x8877665544332211ull : 0x44332211ull);
      EXPECT_EQ(Bound.Upper.Value,
                M == Mode::Bits64 ? 0x0123456789abcdefull : 0x89abcdefull);
      EXPECT_EQ(Bound.Lower.KnownMask,
                M == Mode::Bits64 ? UINT64_MAX : UINT32_MAX);
      EXPECT_EQ(Bound.Upper.KnownMask, Bound.Lower.KnownMask);
      EXPECT_EQ(C.status(), 0x76543212u);
      for (unsigned I = 0; I != 4; ++I)
        if (I != Destination)
          EXPECT_EQ(C.bounds(I), Before[I]);
    }
}

TEST(X86MpxContext, StoreValuesRequireOnlyTheKnownBitsAtTheSelectedWidth) {
  for (auto M : {Mode::Bits32, Mode::Bits64})
    for (unsigned Register = 0; Register != 4; ++Register) {
      auto C = seeded();
      const auto Before = snapshot(C);
      const auto V = C.storeValue(M, Register);
      EXPECT_EQ(V.Result, Result::Completed);
      EXPECT_EQ(V.Lower, M == Mode::Bits64
                             ? Before[Register].Lower.Value
                             : uint32_t(Before[Register].Lower.Value));
      EXPECT_EQ(V.Upper, M == Mode::Bits64
                             ? Before[Register].Upper.Value
                             : uint32_t(Before[Register].Upper.Value));
      EXPECT_EQ(V.WordBytes, M == Mode::Bits64 ? 8u : 4u);
      EXPECT_EQ(snapshot(C), Before);
      EXPECT_EQ(C.status(), 0x76543212u);
    }
  auto C = seeded();
  ASSERT_TRUE(C.setBounds(0, {{0x1234, UINT32_MAX}, {0x5678, UINT32_MAX}}));
  EXPECT_EQ(C.storeValue(Mode::Bits32, 0).Result, Result::Completed);
  EXPECT_EQ(C.storeValue(Mode::Bits64, 0).Result, Result::UnknownOperand);
  for (bool Lower : {false, true}) {
    auto B = known(1, 2);
    (Lower ? B.Lower : B.Upper).KnownMask ^= 1;
    ASSERT_TRUE(C.setBounds(0, B));
    EXPECT_EQ(C.storeValue(Mode::Bits32, 0).Result, Result::UnknownOperand);
  }
  EXPECT_EQ(C.storeValue(Mode::Bits64, 4).Result, Result::InvalidInput);
}

TEST(X86MpxContext,
     ChecksDistinguishRawAndComplementedUpperWithoutClearingStatus) {
  constexpr std::array<uint64_t, 12> Values = {0,
                                               1,
                                               2,
                                               0x7fffffff,
                                               0x80000000,
                                               0xfffffffe,
                                               0xffffffff,
                                               0x100000000,
                                               0x7fffffffffffffff,
                                               0x8000000000000000,
                                               0xfffffffffffffffe,
                                               UINT64_MAX};
  for (auto M : {Mode::Bits32, Mode::Bits64})
    for (auto K : {Check::Lower, Check::UpperComplement, Check::UpperRaw})
      for (uint64_t Bound : Values)
        for (uint64_t Address : Values) {
          auto C = seeded();
          ASSERT_TRUE(C.setBounds(1, known(Bound, Bound)));
          const auto Before = snapshot(C);
          const uint64_t Limit = M == Mode::Bits32 ? uint32_t(Bound) : Bound;
          const uint64_t Value =
              M == Mode::Bits32 ? uint32_t(Address) : Address;
          const uint64_t Top = M == Mode::Bits32 ? UINT32_MAX : UINT64_MAX;
          bool Failed = false;
          switch (K) {
          case Check::Lower:
            Failed = Value < Limit;
            break;
          case Check::UpperComplement:
            Failed = Value > Top - Limit;
            break;
          case Check::UpperRaw:
            Failed = Value > Limit;
            break;
          }
          EXPECT_EQ(C.check(M, K, 1, Address),
                    Failed ? Result::BoundsFault : Result::Completed);
          EXPECT_EQ(snapshot(C), Before);
          EXPECT_EQ(C.status(), Failed ? 1u : 0x76543212u);
        }
}

TEST(X86MpxContext, DirectoryUsesNonzeroBasesPrivilegeAndEveryMAWABit) {
  auto C = seeded();
  // Independent hand-computed addresses; a historical emulator bug shifted
  // BNDCFG by 20 instead of 12, which a zero-base fixture could not expose.
  auto A = C.directoryAddress(Mode::Bits64, 0x123456789abcull);
  EXPECT_EQ(A.Result, Result::Completed);
  EXPECT_EQ(A.Address, 0x1b4e7b38u);
  EXPECT_EQ(A.WordBytes, 8u);
  A = C.directoryAddress(Mode::Bits32, 0x89abcdef);
  EXPECT_EQ(A.Address, 0x1256baf0u);
  EXPECT_EQ(A.WordBytes, 4u);
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (unsigned Adjust = 0; Adjust <= 16; ++Adjust) {
      auto S = configuration(CPL);
      S.UserAddressWidthAdjust = Adjust;
      ASSERT_TRUE(C.configure(S));
      for (unsigned Bit = 0; Bit != 64; ++Bit) {
        const uint64_t Base = CPL == 3 ? 0x12345000 : 0x98765000;
        const unsigned Last = CPL == 3 ? 47 + Adjust : 47;
        const uint64_t Offset =
            Bit >= 20 && Bit <= Last ? uint64_t(1) << (Bit - 17) : 0;
        A = C.directoryAddress(Mode::Bits64, uint64_t(1) << Bit);
        EXPECT_EQ(A.Result, Result::Completed);
        EXPECT_EQ(A.Address, Base + Offset);
      }
    }
  auto S = configuration(0);
  S.UserAddressWidthAdjust.reset();
  ASSERT_TRUE(C.configure(S));
  EXPECT_EQ(C.directoryAddress(Mode::Bits64, 0).Result, Result::Completed);
  S.PrivilegeLevel = 3;
  ASSERT_TRUE(C.configure(S));
  EXPECT_EQ(C.directoryAddress(Mode::Bits64, 0).Result, Result::UnknownContext);
  EXPECT_EQ(C.directoryAddress(Mode::Bits32, 0).Result, Result::Completed);
}

TEST(X86MpxContext,
     TableAddressesIgnoreReservedEntryBitsAndWrapAtTheModeWidth) {
  auto C = seeded();
  for (uint64_t LowBits : {1u, 3u, 5u, 7u}) {
    const auto A = C.tableAddress(Mode::Bits64, 0x123456789abcull,
                                  0x0000800000000000ull | LowBits);
    EXPECT_EQ(A.Result, Result::Completed);
    EXPECT_EQ(A.Address, 0x0000800000226ae0ull);
    EXPECT_EQ(A.WordBytes, 8u);
  }
  // A noncanonical input is an indexing operand, not a memory reference.
  EXPECT_EQ(C.directoryAddress(Mode::Bits64, 0x8000123456789abcull).Address,
            C.directoryAddress(Mode::Bits64, 0x123456789abcull).Address);
  for (uint64_t LowBits : {1u, 3u}) {
    auto A = C.tableAddress(Mode::Bits32, 0x89abcdef, 0xfffffff0 | LowBits);
    EXPECT_EQ(A.Result, Result::Completed);
    EXPECT_EQ(A.Address, 0x37a0u);
  }
  auto S = configuration();
  S.UserConfig = 0xfffff001;
  ASSERT_TRUE(C.configure(S));
  EXPECT_EQ(C.directoryAddress(Mode::Bits32, 0xfffff000).Address, 0x003feffcu);
  S.UserConfig = 0xfffffffffffff001ull;
  ASSERT_TRUE(C.configure(S));
  EXPECT_EQ(C.directoryAddress(Mode::Bits64, 0x200000000ull).Address, 0xf000u);
  EXPECT_EQ(C.tableAddress(Mode::Bits64, 8, 0xfffffffffffffff9ull).Address,
            0x18u);
  EXPECT_EQ(C.status(), 0x76543212u);
}

TEST(X86MpxContext,
     DirectoryFailureAndPartialTableFaultHaveDifferentStateEffects) {
  for (auto M : {Mode::Bits32, Mode::Bits64})
    for (unsigned Destination = 0; Destination != 4; ++Destination) {
      auto C = seeded();
      const auto Before = snapshot(C);
      const uint64_t ExpectedStatus =
          M == Mode::Bits64 ? 0x1b4e7b3a : 0x1249ee26;
      const auto A = C.tableAddress(M, 0x123456789abcull, 0xabcdef00);
      EXPECT_EQ(A.Result, Result::BoundsFault);
      EXPECT_EQ(C.status(), ExpectedStatus);
      EXPECT_EQ(snapshot(C), Before);
      EXPECT_EQ(C.tableReadFault(M, Destination), Result::PartialStateUnknown);
      EXPECT_EQ(C.bounds(Destination), X86MpxBounds{});
      EXPECT_EQ(C.status(), ExpectedStatus);
      for (unsigned I = 0; I != 4; ++I)
        if (I != Destination)
          EXPECT_EQ(C.bounds(I), Before[I]);
      EXPECT_EQ(C.check(M, Check::Lower, Destination, 0),
                Result::UnknownOperand);
    }
}

TEST(X86MpxContext,
     TablePointerComparisonIsUnscaledAndUsesTheCurrentModeWidth) {
  for (auto M : {Mode::Bits32, Mode::Bits64})
    for (unsigned Destination = 0; Destination != 4; ++Destination)
      for (bool Match : {false, true}) {
        auto C = seeded();
        const auto Before = snapshot(C);
        const uint64_t Pointer = 0x5555555512345678ull;
        const uint64_t Stored = Match ? 0xaaaaaaaa12345678ull : Pointer * 2;
        EXPECT_EQ(C.completeTableLoad(M, Destination, 0x987654321, 0x123456789,
                                      Stored, Pointer),
                  Result::Completed);
        const auto B = *C.bounds(Destination);
        const bool Equal = M == Mode::Bits32 && Match;
        EXPECT_EQ(B.Lower.Value, Equal ? 0x87654321u : 0);
        EXPECT_EQ(B.Upper.Value, Equal ? 0x23456789u : 0);
        EXPECT_EQ(B.Lower.KnownMask,
                  M == Mode::Bits32 ? UINT32_MAX : UINT64_MAX);
        EXPECT_EQ(B.Upper.KnownMask, B.Lower.KnownMask);
        EXPECT_EQ(C.status(), 0x76543212u);
        for (unsigned I = 0; I != 4; ++I)
          if (I != Destination)
            EXPECT_EQ(C.bounds(I), Before[I]);
      }
  auto C = seeded();
  EXPECT_EQ(C.completeTableLoad(Mode::Bits64, 0, 0x123456789abcdef0ull,
                                0xfedcba9876543210ull, 0, 0),
            Result::Completed);
  EXPECT_EQ(C.bounds(0), known(0x123456789abcdef0ull, 0xfedcba9876543210ull));
}

TEST(X86MpxContext, OriginalBranchInitializationUsesTheActivePreserveBit) {
  for (unsigned CPL = 0; CPL != 4; ++CPL)
    for (bool Preserve : {false, true})
      for (auto Effect : {Branch::Preserve, Branch::Initialize}) {
        auto C = seeded();
        auto S = configuration(CPL);
        S.UserConfig = CPL == 3 ? (Preserve ? 3 : 1) : (Preserve ? 1 : 3);
        S.SupervisorConfig = CPL == 3 ? (Preserve ? 1 : 3) : (Preserve ? 3 : 1);
        ASSERT_TRUE(C.configure(S));
        const auto Before = snapshot(C);
        EXPECT_EQ(C.completeBranch(Effect), Result::Completed);
        for (unsigned I = 0; I != 4; ++I)
          EXPECT_EQ(C.bounds(I), !Preserve && Effect == Branch::Initialize
                                     ? known(0, 0)
                                     : Before[I]);
        EXPECT_EQ(C.status(), 0x76543212u);
      }
  auto C = seeded();
  EXPECT_EQ(C.completeLegacyBoundsFault(), Result::Completed);
  EXPECT_EQ(C.status(), 0u);
}

TEST(X86MpxContext, InvalidArgumentsAndUnknownMemoryNeverPublishAResult) {
  auto C = seeded();
  const auto Before = snapshot(C);
  EXPECT_FALSE(C.setBounds(4, known(0, 0)));
  EXPECT_FALSE(C.bounds(4));
  EXPECT_EQ(C.move(Mode::Bits64, 0, 4), Result::InvalidInput);
  EXPECT_EQ(C.move(Mode::Bits64, 4, 0), Result::InvalidInput);
  EXPECT_EQ(C.check(Mode::Bits64, static_cast<Check>(255), 0, 0),
            Result::InvalidInput);
  EXPECT_EQ(C.completeBranch(static_cast<Branch>(255)), Result::InvalidInput);
  EXPECT_EQ(C.completeLoad(static_cast<Mode>(255), 0, 0, 0),
            Result::InvalidInput);
  EXPECT_EQ(C.directoryAddress(static_cast<Mode>(255), 0).Result,
            Result::InvalidInput);
  EXPECT_EQ(C.directoryAddress(Mode::Bits64, {}).Result,
            Result::UnknownOperand);
  EXPECT_EQ(C.tableAddress(Mode::Bits64, {}, 0).Result, Result::UnknownOperand);
  EXPECT_EQ(snapshot(C), Before);
  EXPECT_EQ(C.status(), 0x76543212u);
}

} // namespace
