//===- SourceParameterPlacementTests.cpp - Source parameter placement ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Where each convention passes the parameters of a source signature.  The
/// decompiler matches a recovered parameter to the source parameter by these
/// locations, never by position: System V passes `f(double x, int n)` with n
/// in the first integer register, and a 16-byte record in two registers.
/// Placement stops at a parameter whose place the rules cannot be sure of.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"

#include <iterator>
#include <vector>

using namespace neverd;

namespace {

#define SOURCE_PARAMETER_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "SourceParameterPlacementFixtures.def"

TypeRef intType(uint16_t Size) { return NdType::makeInt(Size, true); }
TypeRef floatType(uint16_t Size) { return NdType::makeFloat(Size); }

/// A record of \p Size bytes laid out as \p Leaves.
TypeRef record(uint16_t Size, NdRecordPassing Passing,
               std::vector<NdScalarLeaf> Leaves, uint16_t Align) {
  auto T = std::make_shared<NdType>();
  T->Kind = NdTypeKind::Struct;
  T->Size = Size;
  T->Passing = Passing;
  T->ScalarLeaves = std::move(Leaves);
  T->Alignment = Align;
  return T;
}

/// struct { long a, b; } as C passes it.
TypeRef pairOfLongs(NdRecordPassing Passing = NdRecordPassing::ByValue) {
  return record(16, Passing, {{0, 8, false}, {8, 8, false}}, 8);
}

SourceParameterPlacement place(Arch A, BinaryFormat F, TypeRef Return,
                               std::vector<TypeRef> Params) {
  auto Placement = placeSourceParameters(A, F, Return, Params);
  EXPECT_TRUE(Placement.has_value());
  return Placement.value_or(SourceParameterPlacement());
}

uint64_t integerRegister(Arch A, BinaryFormat F, size_t Index) {
  return getTargetRegInfo(A).integerArgumentLayout(F).Registers[Index];
}

uint64_t floatingRegister(Arch A, size_t Index) {
  return getTargetRegInfo(A).FPParamRegs[Index];
}

int64_t stackBase(Arch A, BinaryFormat F) {
  return getTargetRegInfo(A).integerArgumentLayout(F).EntryStackBase;
}

void expectRegister(const SourceParameterPiece &Piece,
                    SourceABICarrierKind Kind, uint64_t Register,
                    uint16_t Offset, uint16_t Bytes, bool Indirect = false) {
  EXPECT_EQ(Piece.Location.Kind, Kind);
  EXPECT_EQ(Piece.Location.RegisterOffset, Register);
  EXPECT_EQ(Piece.Offset, Offset);
  EXPECT_EQ(Piece.Location.ValueBytes, Bytes);
  EXPECT_EQ(Piece.Indirect, Indirect);
}

void expectStack(const SourceParameterPiece &Piece, int64_t EntryOffset,
                 uint16_t Bytes, bool Indirect = false) {
  EXPECT_EQ(Piece.Location.Kind, SourceABICarrierKind::Stack);
  EXPECT_EQ(Piece.Location.EntryStackOffset, EntryOffset);
  EXPECT_EQ(Piece.Location.ValueBytes, Bytes);
  EXPECT_EQ(Piece.Indirect, Indirect);
}

constexpr auto IntegerReg = SourceABICarrierKind::IntegerRegister;
constexpr auto FloatingReg = SourceABICarrierKind::FloatingRegister;
constexpr Arch X64 = Arch::X64;
constexpr BinaryFormat ELF = BinaryFormat::ELF;

TEST(SourceParameterPlacement, SysVPassesIntegersAndFloatsInTheirOwnRegisters) {
  const auto P = place(X64, ELF, floatType(8), {floatType(8), intType(4)});
  ASSERT_EQ(P.Parameters.size(), 2u);
  ASSERT_EQ(P.Parameters[0].size(), 1u);
  expectRegister(P.Parameters[0][0], FloatingReg, floatingRegister(X64, 0), 0,
                 8);
  ASSERT_EQ(P.Parameters[1].size(), 1u);
  expectRegister(P.Parameters[1][0], IntegerReg, integerRegister(X64, ELF, 0),
                 0, 4);
  EXPECT_FALSE(P.ResultPointer);
}

TEST(SourceParameterPlacement, SysVSplitsARecordByItsEightbytes) {
  const auto Mixed =
      record(16, NdRecordPassing::ByValue, {{0, 8, true}, {8, 4, false}}, 8);
  const auto P =
      place(X64, ELF, intType(8), {pairOfLongs(), Mixed, intType(8)});
  ASSERT_EQ(P.Parameters.size(), 3u);
  ASSERT_EQ(P.Parameters[0].size(), 2u);
  expectRegister(P.Parameters[0][0], IntegerReg, integerRegister(X64, ELF, 0),
                 0, 8);
  expectRegister(P.Parameters[0][1], IntegerReg, integerRegister(X64, ELF, 1),
                 8, 8);
  ASSERT_EQ(P.Parameters[1].size(), 2u);
  expectRegister(P.Parameters[1][0], FloatingReg, floatingRegister(X64, 0), 0,
                 8);
  expectRegister(P.Parameters[1][1], IntegerReg, integerRegister(X64, ELF, 2),
                 8, 8);
  expectRegister(P.Parameters[2][0], IntegerReg, integerRegister(X64, ELF, 3),
                 0, 8);
}

TEST(SourceParameterPlacement, SysVPassesAComplexNumberInVectorRegisters) {
  const auto Complex =
      record(16, NdRecordPassing::Complex, {{0, 8, true}, {8, 8, true}}, 8);
  const auto P = place(X64, ELF, floatType(8), {Complex, intType(4)});
  ASSERT_EQ(P.Parameters.size(), 2u);
  expectRegister(P.Parameters[0][0], FloatingReg, floatingRegister(X64, 0), 0,
                 8);
  expectRegister(P.Parameters[0][1], FloatingReg, floatingRegister(X64, 1), 8,
                 8);
  expectRegister(P.Parameters[1][0], IntegerReg, integerRegister(X64, ELF, 0),
                 0, 4);

  // A complex long double comes back in x87 registers, not through memory.
  const auto Wide =
      record(32, NdRecordPassing::Complex, {{0, 16, true}, {16, 16, true}}, 16);
  EXPECT_FALSE(place(X64, ELF, Wide, {intType(4)}).ResultPointer);
}

TEST(SourceParameterPlacement, SysVReturnsALargeRecordThroughTheFirstRegister) {
  const auto Large = record(24, NdRecordPassing::ByValue,
                            {{0, 8, false}, {8, 8, false}, {16, 8, false}}, 8);
  const auto P = place(X64, ELF, Large, {intType(8)});
  ASSERT_TRUE(P.ResultPointer);
  EXPECT_EQ(P.ResultPointer->RegisterOffset, integerRegister(X64, ELF, 0));
  ASSERT_EQ(P.Parameters.size(), 1u);
  expectRegister(P.Parameters[0][0], IntegerReg, integerRegister(X64, ELF, 1),
                 0, 8);

  // A class C++ passes by reference is returned through memory too.
  const auto Class = pairOfLongs(NdRecordPassing::ByReference);
  EXPECT_TRUE(place(X64, ELF, Class, {intType(8)}).ResultPointer);
}

TEST(SourceParameterPlacement, SysVPassesMemoryRecordsOnTheStack) {
  const auto Large = record(24, NdRecordPassing::ByValue,
                            {{0, 8, false}, {8, 8, false}, {16, 8, false}}, 8);
  std::vector<TypeRef> Params(6, intType(8));
  Params.push_back(intType(8));
  Params.push_back(Large);
  Params.push_back(intType(4));
  const auto P = place(X64, ELF, intType(8), Params);
  ASSERT_EQ(P.Parameters.size(), 9u);
  const int64_t Base = stackBase(X64, ELF);
  expectStack(P.Parameters[6][0], Base, 8);
  expectStack(P.Parameters[7][0], Base + 8, 24);
  expectStack(P.Parameters[8][0], Base + 32, 4);
}

TEST(SourceParameterPlacement, SysVPassesAClassByReferenceAsAnAddress) {
  const auto P = place(X64, ELF, intType(4),
                       {pairOfLongs(NdRecordPassing::ByReference), intType(4)});
  ASSERT_EQ(P.Parameters.size(), 2u);
  expectRegister(P.Parameters[0][0], IntegerReg, integerRegister(X64, ELF, 0),
                 0, 8, /*Indirect=*/true);
  expectRegister(P.Parameters[1][0], IntegerReg, integerRegister(X64, ELF, 1),
                 0, 4);
}

TEST(SourceParameterPlacement, SysVStopsWhereTheRulesAreUnsure) {
  // A record whose passing the source does not say.
  auto P =
      place(X64, ELF, intType(4),
            {intType(4), pairOfLongs(NdRecordPassing::Unknown), intType(4)});
  EXPECT_EQ(P.Parameters.size(), 1u);
  // A record without its layout.
  P = place(X64, ELF, intType(4),
            {intType(4), record(16, NdRecordPassing::ByValue, {}, 8)});
  EXPECT_EQ(P.Parameters.size(), 1u);
  // 16 integer bytes are __int128 or _BitInt(128); 16 floating bytes an x87
  // long double or a __float128.
  EXPECT_TRUE(place(X64, ELF, intType(4), {intType(16)}).Parameters.empty());
  EXPECT_TRUE(place(X64, ELF, intType(4), {floatType(16)}).Parameters.empty());
  // A memory record without a known alignment has no certain slot.
  std::vector<TypeRef> Params(6, intType(8));
  Params.push_back(record(24, NdRecordPassing::ByValue,
                          {{0, 8, false}, {8, 8, false}, {16, 8, false}}, 0));
  EXPECT_EQ(place(X64, ELF, intType(4), Params).Parameters.size(), 6u);
  // A result whose passing is unknown leaves every register in doubt.
  P = place(X64, ELF, pairOfLongs(NdRecordPassing::Unknown), {intType(4)});
  EXPECT_TRUE(P.Parameters.empty());
  EXPECT_FALSE(P.ResultPointer);
}

TEST(SourceParameterPlacement, Win64PassesEachParameterByPosition) {
  constexpr BinaryFormat COFF = BinaryFormat::COFF;
  const auto Odd = record(12, NdRecordPassing::Microsoft, {}, 0);
  const auto Small = record(8, NdRecordPassing::Microsoft, {}, 0);
  const auto P = place(X64, COFF, intType(4),
                       {floatType(8), intType(4), Odd, Small, intType(8)});
  ASSERT_EQ(P.Parameters.size(), 5u);
  expectRegister(P.Parameters[0][0], FloatingReg, floatingRegister(X64, 0), 0,
                 8);
  expectRegister(P.Parameters[1][0], IntegerReg, integerRegister(X64, COFF, 1),
                 0, 4);
  expectRegister(P.Parameters[2][0], IntegerReg, integerRegister(X64, COFF, 2),
                 0, 8, /*Indirect=*/true);
  // Its bytes or, for a class, an address; the position is the same.
  expectRegister(P.Parameters[3][0], IntegerReg, integerRegister(X64, COFF, 3),
                 0, 8);
  expectStack(P.Parameters[4][0], stackBase(X64, COFF), 8);

  // A 16-byte record result takes the first position.
  const auto R = place(X64, COFF, pairOfLongs(), {intType(4)});
  ASSERT_TRUE(R.ResultPointer);
  EXPECT_EQ(R.ResultPointer->RegisterOffset, integerRegister(X64, COFF, 0));
  expectRegister(R.Parameters[0][0], IntegerReg, integerRegister(X64, COFF, 1),
                 0, 4);
  // A small class may come back through memory whatever its size.
  const auto Doubt = place(X64, COFF, Small, {intType(4)});
  EXPECT_TRUE(Doubt.Parameters.empty());
  EXPECT_FALSE(Doubt.ResultPointer);
}

TEST(SourceParameterPlacement, AAPCS64UsesHomogeneousAggregatesAndPairs) {
  constexpr Arch A64 = Arch::AArch64;
  const auto Floats = record(12, NdRecordPassing::ByValue,
                             {{0, 4, true}, {4, 4, true}, {8, 4, true}}, 4);
  const auto Large = record(24, NdRecordPassing::ByValue,
                            {{0, 8, false}, {8, 8, false}, {16, 8, false}}, 8);
  const auto P =
      place(A64, ELF, intType(4), {intType(4), Floats, intType(16), Large});
  ASSERT_EQ(P.Parameters.size(), 4u);
  expectRegister(P.Parameters[0][0], IntegerReg, integerRegister(A64, ELF, 0),
                 0, 4);
  ASSERT_EQ(P.Parameters[1].size(), 3u);
  for (size_t I = 0; I < 3; ++I)
    expectRegister(P.Parameters[1][I], FloatingReg, floatingRegister(A64, I),
                   static_cast<uint16_t>(4 * I), 4);
  // A 128-bit integer starts at an even register.
  expectRegister(P.Parameters[2][0], IntegerReg, integerRegister(A64, ELF, 2),
                 0, 8);
  expectRegister(P.Parameters[2][1], IntegerReg, integerRegister(A64, ELF, 3),
                 8, 8);
  expectRegister(P.Parameters[3][0], IntegerReg, integerRegister(A64, ELF, 4),
                 0, 8, /*Indirect=*/true);
}

TEST(SourceParameterPlacement, DarwinWideArgumentsStartAtOddRegisters) {
  const auto WideRecord =
      record(kAArch64WideIntegerBytes, NdRecordPassing::ByValue,
             {{0, kAArch64IntegerBytes, false},
              {kAArch64IntegerBytes, kAArch64IntegerBytes, false}},
             kAArch64WideIntegerBytes);
  for (const auto Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF})
    for (const auto Wide : {intType(kAArch64WideIntegerBytes), WideRecord})
      for (unsigned Prefix = 0; Prefix < kAArch64ArgumentRegisters; ++Prefix) {
        std::vector<TypeRef> Params(Prefix, intType(kAArch64IntegerBytes));
        Params.push_back(Wide);
        Params.push_back(intType(kAArch64IntegerBytes));
        const auto P =
            place(Arch::AArch64, Format, intType(kAArch64IntegerBytes), Params);
        const unsigned First = Format == BinaryFormat::MachO
                                   ? Prefix
                                   : (Prefix + 1) /
                                         kAArch64WideIntegerRegisters *
                                         kAArch64WideIntegerRegisters;
        if (First + kAArch64WideIntegerRegisters > kAArch64ArgumentRegisters) {
          ASSERT_EQ(P.Parameters.size(), Params.size());
          ASSERT_EQ(P.Parameters[Prefix].size(), 1u);
          expectStack(P.Parameters[Prefix][0], 0, kAArch64WideIntegerBytes);
          expectStack(P.Parameters.back()[0], kAArch64WideIntegerBytes,
                      kAArch64IntegerBytes);
        } else {
          ASSERT_EQ(P.Parameters.size(), Params.size());
          ASSERT_EQ(P.Parameters[Prefix].size(), kAArch64WideIntegerRegisters);
          for (unsigned I = 0; I < kAArch64WideIntegerRegisters; ++I)
            expectRegister(P.Parameters[Prefix][I], IntegerReg,
                           integerRegister(Arch::AArch64, Format, First + I),
                           I * kAArch64IntegerBytes, kAArch64IntegerBytes);
        }
      }
}

TEST(SourceParameterPlacement, DarwinWideScalarSpillsKeepSixteenByteAlignment) {
  struct Spill {
    uint16_t Bytes;
    int64_t Offset;
  };
  const Spill Spills[] = {
#define SOURCE_PARAMETER_WIDE_SPILL(Bytes, Offset) {Bytes, Offset},
#include "SourceParameterPlacementFixtures.def"
  };
  std::vector<TypeRef> Params(kAArch64ArgumentRegisters,
                              intType(kAArch64IntegerBytes));
  for (const auto &S : Spills)
    Params.push_back(intType(S.Bytes));
  const auto P = place(Arch::AArch64, BinaryFormat::MachO,
                       intType(kAArch64IntegerBytes), Params);
  ASSERT_EQ(P.Parameters.size(), Params.size());
  for (unsigned I = 0; I < std::size(Spills); ++I) {
    ASSERT_EQ(P.Parameters[kAArch64ArgumentRegisters + I].size(), 1u);
    expectStack(P.Parameters[kAArch64ArgumentRegisters + I][0],
                Spills[I].Offset, Spills[I].Bytes);
  }
}

TEST(SourceParameterPlacement, AAPCS64KeepsTheResultPointerApartFromArguments) {
  constexpr Arch A64 = Arch::AArch64;
  const auto Large = record(24, NdRecordPassing::ByValue,
                            {{0, 8, false}, {8, 8, false}, {16, 8, false}}, 8);
  const auto Floating = record(24, NdRecordPassing::ByValue,
                               {{0, 8, true}, {8, 8, true}, {16, 8, true}}, 8);
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    const auto P = place(A64, Format, Large, {intType(8)});
    ASSERT_TRUE(P.ResultPointer);
    EXPECT_EQ(P.ResultPointer->RegisterOffset,
              getTargetRegInfo(A64).indirectResultReg());
    expectRegister(P.Parameters[0][0], IntegerReg,
                   integerRegister(A64, Format, 0), 0, 8);
    EXPECT_FALSE(place(A64, Format, Floating, {}).ResultPointer);
    EXPECT_FALSE(
        place(A64, Format, record(24, NdRecordPassing::Unknown, {}, 8), {})
            .ResultPointer);
    EXPECT_TRUE(
        place(A64, Format, pairOfLongs(NdRecordPassing::ByReference), {})
            .ResultPointer);
  }
}

TEST(SourceParameterPlacement, I386PassesEverythingOnTheStack) {
  constexpr Arch X86 = Arch::X86;
  const int64_t Base = stackBase(X86, ELF);
  auto P = place(X86, ELF, intType(4), {floatType(8), intType(4)});
  ASSERT_EQ(P.Parameters.size(), 2u);
  expectStack(P.Parameters[0][0], Base, 8);
  expectStack(P.Parameters[1][0], Base + 8, 4);

  // Linux returns every record through a pointer pushed first.
  const auto Small =
      record(8, NdRecordPassing::ByValue, {{0, 4, false}, {4, 4, false}}, 4);
  P = place(X86, ELF, Small, {intType(4)});
  ASSERT_TRUE(P.ResultPointer);
  EXPECT_EQ(P.ResultPointer->EntryStackOffset, Base);
  expectStack(P.Parameters[0][0], Base + 4, 4);

  // The Microsoft C++ ABI passes every record as its bytes, so one from a
  // PDB has its slot; a class whose passing is unknown does not.
  constexpr BinaryFormat COFF = BinaryFormat::COFF;
  P = place(X86, COFF, intType(4),
            {record(8, NdRecordPassing::Microsoft, {}, 0), intType(4)});
  ASSERT_EQ(P.Parameters.size(), 2u);
  expectStack(P.Parameters[1][0], stackBase(X86, COFF) + 8, 4);
  EXPECT_TRUE(place(X86, COFF, intType(4),
                    {record(8, NdRecordPassing::Unknown, {}, 0), intType(4)})
                  .Parameters.empty());
  // As a small result it may be a class returned through memory.
  EXPECT_TRUE(place(X86, COFF, record(8, NdRecordPassing::Microsoft, {}, 0),
                    {intType(4)})
                  .Parameters.empty());
}

} // namespace
