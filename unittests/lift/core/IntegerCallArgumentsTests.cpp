//===- IntegerCallArgumentsTests.cpp - Source-call operand placement -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Register and stack operand identities for integer-class C records.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/FloatingCallArguments.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/libc/LibCVariadicArguments.h"

using namespace neverd;

namespace {

TEST(FloatingCallArguments, HFAPlacementKeepsCompleteRecordsAndResultFields) {
  for (uint16_t Bytes : {4u, 8u})
    for (unsigned Count : {1u, 2u, 3u, 4u}) {
      const auto Scalar = NdType::makeFloat(Bytes);
      const auto Record =
          NdType::makeStruct(std::vector<TypeRef>(Count, Scalar));
      ASSERT_TRUE(Record);
      Record->Passing = NdRecordPassing::ByValue;
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
        const auto Layout = floatingCallArguments(Arch::AArch64, Format, Record,
                                                  {Record, Record});
        ASSERT_TRUE(Layout);
        EXPECT_EQ(Layout->ReturnBytes, Bytes * Count);
        EXPECT_EQ(Layout->ReturnElements, Count);
        EXPECT_EQ(Layout->Scalars.size(), 2 * Count);
        EXPECT_EQ(Layout->ParameterElements,
                  (std::vector<uint16_t>{uint16_t(Count), uint16_t(Count)}));
        const auto Mixed =
            floatingCallArguments(Arch::AArch64, Format, Scalar,
                                  {Record, NdType::makeInt(4), Record});
        ASSERT_TRUE(Mixed);
        EXPECT_EQ(Mixed->Scalars.size(), 2 * Count + 1);
        EXPECT_FALSE(Mixed->Scalars[Count].Floating);
        std::vector<TypeRef> Overflow(9 - Count, Scalar);
        Overflow.push_back(Record);
        const auto Spilled =
            floatingCallArguments(Arch::AArch64, Format, Scalar, Overflow);
        if (Format == BinaryFormat::MachO) {
          EXPECT_FALSE(Spilled);
        } else {
          ASSERT_TRUE(Spilled);
          ASSERT_EQ(Spilled->Scalars.size(), 9u);
          for (unsigned I = 0; I < Count; ++I) {
            const auto &Member = Spilled->Scalars[9 - Count + I];
            EXPECT_EQ(Member.Kind, SourceABICarrierKind::Stack);
            EXPECT_EQ(Member.StackOffset, I * Bytes);
            EXPECT_EQ(Member.Bytes, Bytes);
          }
        }
      }
      Record->Size += Bytes;
      EXPECT_FALSE(floatingCallArguments(Arch::AArch64, BinaryFormat::COFF,
                                         Scalar, {Record}));
      Record->Size -= Bytes;
      Record->Passing = NdRecordPassing::Unknown;
      EXPECT_FALSE(floatingCallArguments(Arch::AArch64, BinaryFormat::COFF,
                                         Scalar, {Record}));
    }
}
TEST(FloatingCallArguments, SpilledHFAsKeepDenseMembersAndIndependentBanks) {
  for (auto Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (uint16_t Bytes : {4u, 8u})
      for (unsigned Prefix : {7u, 8u, 9u}) {
        SCOPED_TRACE(Prefix);
        const auto Scalar = NdType::makeFloat(Bytes);
        auto Pair = NdType::makeStruct({Scalar, Scalar});
        Pair->Passing = NdRecordPassing::ByValue;
        std::vector<TypeRef> Parameters(Prefix, Scalar);
        Parameters.insert(Parameters.end(),
                          {Pair, Scalar, NdType::makeInt(4), Pair});
        const auto Layout =
            floatingCallArguments(Arch::AArch64, Format, Pair, Parameters);
        ASSERT_TRUE(Layout);
        const int64_t Base = Prefix > 8 ? 8 : 0;
        const auto &First = Layout->Scalars[Prefix];
        EXPECT_EQ(First.Kind, SourceABICarrierKind::Stack);
        EXPECT_EQ(First.StackOffset, Base);
        EXPECT_EQ(Layout->Scalars[Prefix + 1].StackOffset, Base + Bytes);
        EXPECT_EQ(Layout->Scalars[Prefix + 2].StackOffset, Base + 2 * Bytes);
        const auto &Integer = Layout->Scalars[Prefix + 3];
        EXPECT_EQ(Integer.Kind, SourceABICarrierKind::IntegerRegister);
        EXPECT_EQ(Integer.Register,
                  getTargetRegInfo(Arch::AArch64).integerParamRegs(Format)[0]);
        EXPECT_EQ(Layout->Scalars[Prefix + 4].StackOffset,
                  Base + 2 * Bytes + 8);
        EXPECT_EQ(Layout->Scalars[Prefix + 5].StackOffset,
                  Base + 3 * Bytes + 8);
        Pair->Alignment *= 2;
        EXPECT_FALSE(
            floatingCallArguments(Arch::AArch64, Format, Pair, Parameters));
      }
}

TEST(FloatingCallArguments, CompleteScalarRegistersAndConservativeBoundaries) {
  for (Arch A : {Arch::X64, Arch::AArch64}) {
    for (BinaryFormat F :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      for (uint16_t Bytes : {4u, 8u}) {
        const auto T = NdType::makeFloat(Bytes);
        const auto Layout = floatingCallArguments(A, F, T, {T, T});
        ASSERT_TRUE(Layout);
        EXPECT_EQ(Layout->ReturnBytes, Bytes);
        EXPECT_EQ(Layout->ParameterBytes,
                  (std::vector<uint16_t>{Bytes, Bytes}));
        EXPECT_TRUE(floatingCallArguments(A, F, T, {}));
        const auto Mixed =
            floatingCallArguments(A, F, T, {T, NdType::makePtr()});
        ASSERT_TRUE(Mixed);
        ASSERT_EQ(Mixed->Scalars.size(), 2u);
        EXPECT_TRUE(Mixed->Scalars[0].Floating);
        EXPECT_TRUE(Mixed->Scalars[1].Pointer);
        EXPECT_EQ(Mixed->Scalars[0].Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Mixed->Scalars[1].Kind,
                  SourceABICarrierKind::IntegerRegister);
        EXPECT_FALSE(floatingCallArguments(A, F, NdType::makeInt(8), {T}));
        EXPECT_FALSE(floatingCallArguments(A, F, T, {NdType::makeFloat(10)}));
        EXPECT_FALSE(
            floatingCallArguments(A, F, T, std::vector<TypeRef>(9, T)));
        if (A == Arch::X64 && F == BinaryFormat::COFF)
          EXPECT_FALSE(
              floatingCallArguments(A, F, T, std::vector<TypeRef>(5, T)));
      }
    }
  }
  const auto T = NdType::makeFloat(8);
  for (BinaryFormat F :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    const auto Stack = floatingCallArguments(Arch::X86, F, T, {T, T});
    ASSERT_TRUE(Stack);
    ASSERT_EQ(Stack->Scalars.size(), 2u);
    EXPECT_EQ(Stack->Scalars[0].StackOffset, 4);
    EXPECT_EQ(Stack->Scalars[1].StackOffset, 12);
    EXPECT_EQ(Stack->Scalars[0].Kind, SourceABICarrierKind::Stack);
    EXPECT_TRUE(Stack->Scalars[0].Floating);
    EXPECT_TRUE(floatingCallArguments(Arch::X86, F, T, {}));
    EXPECT_FALSE(floatingCallArguments(Arch::X86, F, T, {NdType::makePtr()}));
  }
  FunctionSym Function;
  Function.ReturnType = T;
  Function.Params = {{"x", T}};
  EXPECT_FALSE(
      libc::floatingCallArguments(Function, Arch::X64, BinaryFormat::COFF));
  Function.PrototypeFromHeader = true;
  EXPECT_TRUE(
      libc::floatingCallArguments(Function, Arch::X64, BinaryFormat::COFF));
  Function.PlatformConvention = false;
  EXPECT_FALSE(
      libc::floatingCallArguments(Function, Arch::X64, BinaryFormat::COFF));
  Function.PlatformConvention = true;
  Function.CallConv = DebugCallConv::Fastcall;
  EXPECT_FALSE(
      libc::floatingCallArguments(Function, Arch::X64, BinaryFormat::COFF));
}

TEST(VariadicCallArguments, HeaderPrefixIsNotAnExactOrFloatingSignature) {
  for (Arch A : {Arch::X86, Arch::X64, Arch::AArch64}) {
    FunctionSym Function;
    Function.IsVariadic = true;
    Function.PrototypeFromHeader = true;
    Function.ReturnType = NdType::makeInt(4, true);
    auto Pointer = NdType::makePtr();
    Pointer->Size = getTargetRegInfo(A).PointerSize;
    Function.Params = {{"name", Pointer}, {"flags", NdType::makeInt(4)}};
    const auto Prefix =
        libc::variadicCallArguments(Function, A, BinaryFormat::COFF);
    ASSERT_TRUE(Prefix);
    EXPECT_EQ(*Prefix, (std::vector<uint16_t>{Pointer->Size, 4}));
    EXPECT_FALSE(libc::variadicCallArguments(Function, A, BinaryFormat::ELF));
    Function.IsVariadic = false;
    EXPECT_FALSE(libc::variadicCallArguments(Function, A, BinaryFormat::COFF));
    Function.IsVariadic = true;
    Function.CallConv = DebugCallConv::Stdcall;
    EXPECT_FALSE(libc::variadicCallArguments(Function, A, BinaryFormat::COFF));
    Function.CallConv = DebugCallConv::Cdecl;
    Function.PrototypeFromHeader = false;
    EXPECT_FALSE(libc::variadicCallArguments(Function, A, BinaryFormat::COFF));
    Function.PrototypeFromHeader = true;
    Function.Params[0].second = NdType::makeFloat(8);
    EXPECT_FALSE(libc::variadicCallArguments(Function, A, BinaryFormat::COFF));
    Function.Params[1].second = NdType::makeFloat(8);
    Function.ReturnType = NdType::makeFloat(8);
    EXPECT_FALSE(libc::floatingCallArguments(Function, A, BinaryFormat::COFF));
    Function.Params.clear();
    EXPECT_FALSE(libc::variadicCallArguments(Function, A, BinaryFormat::COFF));
  }
}

TypeRef pointer(uint16_t Bytes = 8) {
  auto Type = NdType::makePtr();
  Type->Size = Bytes;
  return Type;
}

TypeRef record(uint16_t Bytes, uint16_t Word) {
  auto Type = std::make_shared<NdType>();
  Type->Kind = NdTypeKind::Struct;
  Type->Size = Bytes;
  Type->Alignment = Word;
  Type->Passing = NdRecordPassing::ByValue;
  for (uint16_t Offset = 0; Offset < Bytes; Offset += Word)
    Type->ScalarLeaves.push_back({Offset, Word, false});
  return Type;
}

TEST(IntegerCallArguments, SysVMemoryRecordLeavesUnusedRegistersBeforeStack) {
  const auto Layout = integerCallArguments(
      Arch::X64, BinaryFormat::ELF, NdType::makePtr(),
      {NdType::makePtr(), NdType::makePtr(), record(32, 8)});
  ASSERT_TRUE(Layout);
  EXPECT_EQ(Layout->Registers, 2u);
  EXPECT_EQ(Layout->Positions, 10u);
  ASSERT_EQ(Layout->Parameters.size(), 3u);
  const auto &Record = Layout->Parameters[2];
  EXPECT_FALSE(Record.Indirect);
  ASSERT_EQ(Record.Pieces.size(), 4u);
  for (unsigned I = 0; I < 4; ++I) {
    EXPECT_EQ(Record.Pieces[I].Argument, 6u + I);
    EXPECT_EQ(Record.Pieces[I].Offset, 8u * I);
    EXPECT_EQ(Record.Pieces[I].Bytes, 8u);
  }
}

TEST(IntegerCallArguments,
     SysVEntryConsumesTwoRegistersBeforeActionAndPointers) {
  const auto Layout = integerCallArguments(
      Arch::X64, BinaryFormat::ELF, NdType::makeInt(4, true),
      {record(16, 8), NdType::makeInt(4, true), NdType::makePtr(),
       NdType::makePtr()});
  ASSERT_TRUE(Layout);
  EXPECT_EQ(Layout->Registers, 5u);
  EXPECT_EQ(Layout->Positions, 5u);
  ASSERT_EQ(Layout->Parameters[0].Pieces.size(), 2u);
  EXPECT_EQ(Layout->Parameters[0].Pieces[0].Argument, 0u);
  EXPECT_EQ(Layout->Parameters[0].Pieces[1].Argument, 1u);
  EXPECT_EQ(Layout->Parameters[1].Pieces[0].Argument, 2u);
  EXPECT_EQ(Layout->Parameters[2].Pieces[0].Argument, 3u);
  EXPECT_EQ(Layout->Parameters[3].Pieces[0].Argument, 4u);
}

TEST(IntegerCallArguments, SysVSpilledRecordDoesNotConsumeRemainingRegister) {
  std::vector<TypeRef> Parameters(5, NdType::makeInt(8, false));
  Parameters.push_back(record(16, 8));
  Parameters.push_back(NdType::makeInt(8, false));
  const auto Layout = integerCallArguments(Arch::X64, BinaryFormat::ELF,
                                           NdType::makeVoid(), Parameters);
  ASSERT_TRUE(Layout);
  EXPECT_EQ(Layout->Registers, 6u);
  EXPECT_EQ(Layout->Positions, 8u);
  EXPECT_EQ(Layout->Parameters[5].Pieces[0].Argument, 6u);
  EXPECT_EQ(Layout->Parameters[5].Pieces[1].Argument, 7u);
  EXPECT_EQ(Layout->Parameters[6].Pieces[0].Argument, 5u);
}

TEST(IntegerCallArguments, I386CountsEveryRecordWordBeforeFollowingParameters) {
  const auto Layout = integerCallArguments(
      Arch::X86, BinaryFormat::ELF, NdType::makeInt(4, true),
      {record(8, 4), NdType::makeInt(4, true), pointer(4), pointer(4)});
  ASSERT_TRUE(Layout);
  EXPECT_EQ(Layout->Registers, 0u);
  EXPECT_EQ(Layout->Positions, 5u);
  ASSERT_EQ(Layout->Parameters[0].Pieces.size(), 2u);
  for (unsigned I = 1; I < 4; ++I)
    EXPECT_EQ(Layout->Parameters[I].Pieces[0].Argument, I + 1);
}

TEST(IntegerCallArguments, LargeRecordsUseThePlatformsIndirectCopyCarrier) {
  for (const auto &[Architecture, Format] :
       {std::pair{Arch::AArch64, BinaryFormat::ELF},
        std::pair{Arch::X64, BinaryFormat::COFF}}) {
    const auto Layout = integerCallArguments(
        Architecture, Format, NdType::makePtr(),
        {NdType::makePtr(), NdType::makePtr(), record(32, 8)});
    ASSERT_TRUE(Layout);
    EXPECT_EQ(Layout->Registers, 3u);
    EXPECT_EQ(Layout->Positions, 3u);
    ASSERT_EQ(Layout->Parameters.size(), 3u);
    const auto &Record = Layout->Parameters[2];
    EXPECT_TRUE(Record.Indirect);
    ASSERT_EQ(Record.Pieces.size(), 1u);
    EXPECT_EQ(Record.Pieces[0].Argument, 2u);
    EXPECT_EQ(Record.Pieces[0].Bytes, 8u);
  }
}

TEST(IntegerCallArguments, RefusesIncompleteOrDifferentCarrierContracts) {
  EXPECT_FALSE(integerCallArguments(Arch::X64, BinaryFormat::ELF,
                                    NdType::makeVoid(),
                                    {record(16, 8), NdType::makeFloat(8)}));
  EXPECT_FALSE(integerCallArguments(Arch::X64, BinaryFormat::ELF, record(32, 8),
                                    {NdType::makePtr()}));
  auto Unknown = record(16, 8);
  Unknown->Passing = NdRecordPassing::Unknown;
  EXPECT_FALSE(integerCallArguments(Arch::X64, BinaryFormat::ELF,
                                    NdType::makeVoid(), {Unknown}));
  EXPECT_FALSE(integerCallArguments(Arch::ARM, BinaryFormat::ELF,
                                    NdType::makeVoid(), {record(8, 4)}));
  EXPECT_FALSE(integerCallArguments(Arch::X64, BinaryFormat::ELF,
                                    NdType::makeVoid(), {record(256, 8)}));
  auto OverAligned = record(32, 8);
  OverAligned->Alignment = 16;
  EXPECT_FALSE(integerCallArguments(Arch::X64, BinaryFormat::ELF,
                                    NdType::makeVoid(), {OverAligned}));
  EXPECT_FALSE(integerCallArguments(Arch::X64, BinaryFormat::ELF, {},
                                    {NdType::makePtr()}));
}

TEST(IntegerCallArguments, WindowsHiddenResultPrecedesRecordParameters) {
  for (const Arch Architecture : {Arch::X64, Arch::X86}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto Layout =
        integerCallArguments(Architecture, BinaryFormat::COFF, record(16, 8),
                             {record(16, 8), record(16, 8)});
    ASSERT_TRUE(Layout);
    ASSERT_TRUE(Layout->ResultPointer);
    EXPECT_EQ(*Layout->ResultPointer, 0u);
    EXPECT_EQ(Layout->Positions, Architecture == Arch::X64 ? 3u : 9u);
    EXPECT_EQ(Layout->Registers, Architecture == Arch::X64 ? 3u : 0u);
    ASSERT_EQ(Layout->Parameters.size(), 2u);
    EXPECT_EQ(Layout->Parameters[0].Pieces[0].Argument, 1u);
    EXPECT_EQ(Layout->Parameters[1].Pieces[0].Argument,
              Architecture == Arch::X64 ? 2u : 5u);
    EXPECT_EQ(Layout->Parameters[0].Indirect, Architecture == Arch::X64);
  }
}
} // namespace
