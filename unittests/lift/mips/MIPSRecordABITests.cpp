//===- MIPSRecordABITests.cpp - Natural record ABI tests -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// n32/n64 record locations checked against the independent GCC/Clang
/// record-abi-roundtrip.py caller/callee corpus, including both byte orders.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
constexpr Arch Architectures[] = {Arch::MIPS64N32, Arch::MIPS64N32EL,
                                  Arch::MIPS64, Arch::MIPS64EL};

TypeRef record(std::vector<TypeRef> Fields, uint16_t PointerBytes = 8) {
  auto Type = NdType::makeStruct(std::move(Fields), PointerBytes);
  if (Type)
    Type->Passing = NdRecordPassing::ByValue;
  return Type;
}

TypeRef bytes(uint16_t Count) {
  return record({NdType::makeArray(NdType::makeInt(1, false), Count, Count)});
}

TEST(MIPSRecordABI, PartialRecordChunksRetainObjectAndRegisterPositions) {
  for (Arch A : Architectures) {
    const auto &TRI = getTargetRegInfo(A);
    const auto Registers = TRI.integerArgumentLayout(BinaryFormat::ELF);
    for (uint16_t Bytes : {3, 5, 6, 7, 9, 10, 13, 15, 16}) {
      const auto Type = bytes(Bytes);
      for (unsigned Prefix : {0u, 7u}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << ':' << Bytes << ':' << Prefix);
        std::vector<TypeRef> Parameters(Prefix, NdType::makeInt(8, false));
        Parameters.push_back(Type);
        Parameters.push_back(NdType::makeInt(8, false));
        const auto Placement =
            mips::placeRecordCall(A, BinaryFormat::ELF, Type, Parameters);
        ASSERT_TRUE(Placement);
        EXPECT_FALSE(Placement->Arguments.ResultPointer);
        ASSERT_EQ(Placement->Arguments.Parameters.size(), Prefix + 2);
        const auto &Input = Placement->Arguments.Parameters[Prefix];
        const auto &Output = Placement->Result;
        const unsigned Chunks = (Bytes + 7) / 8;
        ASSERT_EQ(Input.size(), Chunks);
        ASSERT_EQ(Output.size(), Chunks);
        for (unsigned I = 0; I < Chunks; ++I) {
          const unsigned Width = I == 0 && Bytes > 8 ? 8 : Bytes - 8 * I;
          EXPECT_EQ(Input[I].Offset, I * 8);
          EXPECT_EQ(Output[I].Offset, I * 8);
          EXPECT_EQ(Input[I].Location.ValueBytes, Width);
          EXPECT_EQ(Output[I].Location.ValueBytes, Width);
          EXPECT_EQ(Output[I].Location.RegisterOffset,
                    I ? TRI.IntReturnReg2 : TRI.IntReturnReg);
          EXPECT_EQ(Output[I].CarrierBytes, 8);
          EXPECT_EQ(Output[I].RegisterBitOffset,
                    archLittleEndian(A) ? 0 : (8 - Width) * 8);
          if (Prefix + I < 8) {
            EXPECT_EQ(Input[I].Location.Kind,
                      SourceABICarrierKind::IntegerRegister);
            EXPECT_EQ(Input[I].Location.RegisterOffset,
                      Registers.Registers[Prefix + I]);
            EXPECT_EQ(Input[I].RegisterBitOffset, Output[I].RegisterBitOffset);
            EXPECT_EQ(Input[I].CarrierBytes, 8);
          } else {
            EXPECT_EQ(Input[I].Location.Kind, SourceABICarrierKind::Stack);
            EXPECT_EQ(Input[I].Location.EntryStackOffset, 0);
            EXPECT_EQ(Input[I].RegisterBitOffset, 0);
            EXPECT_EQ(Input[I].CarrierBytes, Width);
          }
        }
        const auto &Tail = Placement->Arguments.Parameters.back();
        ASSERT_EQ(Tail.size(), 1u);
        if (Prefix == 0) {
          EXPECT_EQ(Tail[0].Location.RegisterOffset,
                    Registers.Registers[Chunks]);
        } else {
          EXPECT_EQ(Tail[0].Location.Kind, SourceABICarrierKind::Stack);
          EXPECT_EQ(Tail[0].Location.EntryStackOffset, (Chunks - 1) * 8);
        }
      }
    }
  }
}

TEST(MIPSRecordABI, DirectFloatingFieldsKeepTheirArgumentAndResultClasses) {
  const auto F = NdType::makeFloat(4), D = NdType::makeFloat(8);
  const auto FD = record({F, D}), DF = record({D, F});
  for (Arch A : Architectures) {
    SCOPED_TRACE(unsigned(A));
    for (const auto &Type : {FD, DF}) {
      const auto Placement =
          mips::placeRecordCall(A, BinaryFormat::ELF, Type, {Type});
      ASSERT_TRUE(Placement);
      const auto &Input = Placement->Arguments.Parameters[0];
      ASSERT_EQ(Input.size(), 2u);
      const unsigned DoubleIndex = Type == FD ? 1 : 0;
      EXPECT_EQ(Input[DoubleIndex].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(Input[DoubleIndex].Location.RegisterOffset,
                mipsreg::w(12 + DoubleIndex));
      EXPECT_EQ(Input[1 - DoubleIndex].Location.Kind,
                SourceABICarrierKind::IntegerRegister);
      ASSERT_EQ(Placement->Result.size(), 2u);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Placement->Result[I].Location.Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Placement->Result[I].Location.RegisterOffset,
                  mipsreg::w(2 * I));
        EXPECT_EQ(Placement->Result[I].Location.ValueBytes,
                  Type->Fields[I]->Size);
        EXPECT_EQ(Placement->Result[I].Offset, I * 8);
        EXPECT_EQ(Placement->Result[I].RegisterBitOffset, 0);
      }
    }
    const auto OneFloat = record({F});
    const auto Single =
        mips::placeRecordCall(A, BinaryFormat::ELF, OneFloat, {OneFloat});
    ASSERT_TRUE(Single);
    EXPECT_EQ(Single->Arguments.Parameters[0][0].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Single->Arguments.Parameters[0][0].RegisterBitOffset,
              archLittleEndian(A) ? 0 : 32);
    EXPECT_EQ(Single->Result[0].Location.Kind,
              SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Single->Result[0].RegisterBitOffset, 0);

    // The object is only eight bytes, but its two direct float fields return
    // independently in f0/f2. Flattening it into one integer chunk or treating
    // the second field as an eight-byte object offset changes the ABI.
    const auto TwoFloats = record({F, F});
    const auto Pair =
        mips::placeRecordCall(A, BinaryFormat::ELF, TwoFloats, {TwoFloats});
    ASSERT_TRUE(Pair);
    ASSERT_EQ(Pair->Arguments.Parameters[0].size(), 1u);
    EXPECT_EQ(Pair->Arguments.Parameters[0][0].Location.Kind,
              SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Pair->Arguments.Parameters[0][0].Location.ValueBytes, 8);
    ASSERT_EQ(Pair->Result.size(), 2u);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Pair->Result[I].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(Pair->Result[I].Location.RegisterOffset, mipsreg::w(2 * I));
      EXPECT_EQ(Pair->Result[I].Location.ValueBytes, 4);
      EXPECT_EQ(Pair->Result[I].Offset, I * 4);
      EXPECT_EQ(Pair->Result[I].CarrierBytes, 8);
      EXPECT_EQ(Pair->Result[I].RegisterBitOffset, 0);
    }

    const auto Nested = record({record({D}), record({D})});
    const auto Array = record({NdType::makeArray(D, 2, 16)});
    for (const auto &Type : {Nested, Array, record({F, F, F})}) {
      const auto Placement =
          mips::placeRecordCall(A, BinaryFormat::ELF, Type, {Type});
      ASSERT_TRUE(Placement);
      for (const auto &Piece : Placement->Arguments.Parameters[0])
        EXPECT_EQ(Piece.Location.Kind, SourceABICarrierKind::IntegerRegister);
      for (const auto &Piece : Placement->Result)
        EXPECT_EQ(Piece.Location.Kind, SourceABICarrierKind::IntegerRegister);
    }
  }
}

TEST(MIPSRecordABI, HiddenResultConsumesFirstArgumentWithoutDefiningV0) {
  for (Arch A : Architectures) {
    const auto Type = bytes(17);
    const auto &TRI = getTargetRegInfo(A);
    const auto Registers = TRI.integerArgumentLayout(BinaryFormat::ELF);
    for (unsigned Prefix : {0u, 7u}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << ':' << Prefix);
      std::vector<TypeRef> Parameters(Prefix, NdType::makeInt(8, false));
      Parameters.push_back(Type);
      Parameters.push_back(NdType::makeInt(8, false));
      const auto Placement =
          mips::placeRecordCall(A, BinaryFormat::ELF, Type, Parameters);
      ASSERT_TRUE(Placement);
      ASSERT_TRUE(Placement->Arguments.ResultPointer);
      EXPECT_EQ(Placement->Arguments.ResultPointer->RegisterOffset,
                Registers.Registers[0]);
      EXPECT_EQ(Placement->Arguments.ResultPointer->ValueBytes,
                TRI.PointerSize);
      EXPECT_TRUE(Placement->Result.empty());
      const auto &Input = Placement->Arguments.Parameters[Prefix];
      ASSERT_EQ(Input.size(), 3u);
      for (unsigned I = 0; I < 3; ++I) {
        EXPECT_EQ(Input[I].Offset, I * 8);
        if (!Prefix) {
          EXPECT_EQ(Input[I].Location.RegisterOffset,
                    Registers.Registers[I + 1]);
        } else {
          EXPECT_EQ(Input[I].Location.Kind, SourceABICarrierKind::Stack);
          EXPECT_EQ(Input[I].Location.EntryStackOffset, I * 8);
          EXPECT_EQ(Input[I].CarrierBytes, I == 2 ? 1 : 8);
        }
      }
      const auto &Tail = Placement->Arguments.Parameters.back()[0];
      if (Prefix)
        EXPECT_EQ(Tail.Location.EntryStackOffset, 24);
      else
        EXPECT_EQ(Tail.Location.RegisterOffset, Registers.Registers[4]);
    }
  }
}

TEST(MIPSRecordABI, TargetPointerLayoutDoesNotExpandOtherSourceABIs) {
  const auto P32 = NdType::makePtr(), P64 = NdType::makePtr();
  P32->Size = 4;
  const auto Byte = NdType::makeInt(1, false);
  EXPECT_FALSE(NdType::makeStruct({P32, Byte}));
  EXPECT_FALSE(NdType::makeStruct({P32, P64}, 4));
  EXPECT_FALSE(NdType::makeStruct({P32, P64}, 8));
  EXPECT_FALSE(NdType::makeStruct({P32}, 0));
  const auto Type = record({P32, Byte}, 4);
  ASSERT_TRUE(Type);
  EXPECT_EQ(Type->Size, 8);
  EXPECT_EQ(Type->Alignment, 4);
  EXPECT_EQ(Type->FieldOffsets, (std::vector<uint16_t>{0, 4}));
  EXPECT_NE(Type->str(), "invalid_record");
  EXPECT_TRUE(sourceAggregateMembers(Type).empty());
  EXPECT_FALSE(equalSourceTypes(Type, Type));
  for (Arch A : Architectures) {
    const auto Placement =
        mips::placeRecordCall(A, BinaryFormat::ELF, Type, {Type});
    EXPECT_EQ(Placement.has_value(), isMIPSN32(A));
    const auto Generic =
        placeSourceParameters(A, BinaryFormat::ELF, NdType::makeVoid(), {Type});
    ASSERT_TRUE(Generic);
    EXPECT_TRUE(Generic->Parameters.empty());
  }
}

TEST(MIPSRecordABI, RefusesPartialLayoutsAndUnqualifiedConventions) {
  const auto Good = record({NdType::makeFloat(8), NdType::makeInt(4)});
  ASSERT_TRUE(Good);
  std::vector<TypeRef> Bad;
  const auto Change = [&]() {
    auto Type = std::make_shared<NdType>(*Good);
    Bad.push_back(Type);
    return Type;
  };
  Change()->Size = 12;
  Change()->Alignment = 4;
  Change()->FieldOffsets[1] = 0;
  Change()->Fields.pop_back();
  Change()->Passing = NdRecordPassing::Unknown;
  Change()->Passing = NdRecordPassing::ByReference;
  Change()->RegisterReturnRecord = true;
  Change()->FieldDisplayBits.push_back({"bit", 0, 1, false});
  Bad.push_back(NdType::makeNamedRecord("missing_definition", 8));
  Bad.push_back(record({NdType::makeComplex(8)}));
  for (Arch A : Architectures) {
    for (const auto &Type : Bad) {
      EXPECT_FALSE(mips::placeRecordCall(A, BinaryFormat::ELF,
                                         NdType::makeVoid(), {Type}));
      EXPECT_FALSE(mips::placeRecordCall(A, BinaryFormat::ELF, Type, {}));
    }
    EXPECT_FALSE(mips::placeRecordCall(A, BinaryFormat::COFF, Good, {Good}));
    EXPECT_FALSE(
        mips::placeRecordCall(A, BinaryFormat::ELF, Good, {Good}, true));
    const auto InvalidVoid = NdType::makeVoid();
    InvalidVoid->Size = 8;
    EXPECT_FALSE(
        mips::placeRecordCall(A, BinaryFormat::ELF, InvalidVoid, {Good}));
  }
  for (Arch A : {Arch::X64, Arch::AArch64})
    EXPECT_FALSE(mips::placeRecordCall(A, BinaryFormat::ELF, Good, {Good}));
}

TEST(MIPSRecordABI, O32ReturnsTheStorageAddressAndSplitsRecordArguments) {
  for (Arch A :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64O32, Arch::MIPS64O32EL}) {
    const auto Tiny = bytes(3);
    const auto Split = bytes(7);
    const auto Word = NdType::makeInt(4, false);
    const auto Double = record({NdType::makeFloat(8)}, 4);
    auto Layout = mips::placeRecordCall(A, BinaryFormat::ELF, Tiny,
                                        {Word, Word, Split, Double});
    ASSERT_TRUE(Layout);
    ASSERT_TRUE(Layout->Arguments.ResultPointer);
    EXPECT_TRUE(Layout->ReturnsResultPointer);
    EXPECT_TRUE(Layout->Result.empty());
    EXPECT_EQ(Layout->Arguments.ResultPointer->ValueBytes, 4u);
    EXPECT_EQ(Layout->Arguments.ResultPointer->RegisterOffset, mipsreg::A0);
    const auto &Pieces = Layout->Arguments.Parameters[2];
    ASSERT_EQ(Pieces.size(), 2u);
    EXPECT_EQ(Pieces[0].Location.RegisterOffset, mipsreg::r(7));
    EXPECT_EQ(Pieces[0].CarrierBytes, 4u);
    EXPECT_EQ(Pieces[1].Location.EntryStackOffset, 16);
    EXPECT_EQ(Pieces[1].Location.ValueBytes, 3u);
    EXPECT_EQ(Layout->Arguments.Parameters[3][0].Location.EntryStackOffset, 24);
    auto Call =
        integerCallArguments(A, BinaryFormat::ELF, Tiny, {Word, Word, Split});
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->ResultPointer, 0u);
    EXPECT_EQ(Call->Parameters[2].Pieces[1].BitOffset,
              archLittleEndian(A) ? 0u : 8u);
    auto Aligned = mips::placeRecordCall(A, BinaryFormat::ELF, Tiny, {Double});
    ASSERT_TRUE(Aligned);
    EXPECT_EQ(Aligned->Arguments.Parameters[0][0].Location.RegisterOffset,
              mipsreg::r(6));
    EXPECT_EQ(Aligned->Arguments.Parameters[0][1].Location.RegisterOffset,
              mipsreg::r(7));
    auto Scalar =
        mips::placeRecordCall(A, BinaryFormat::ELF, NdType::makeInt(8), {Tiny});
    ASSERT_TRUE(Scalar);
    EXPECT_EQ(Scalar->Result.size(), 2u);
    EXPECT_EQ(Scalar->Result[0].Offset, archLittleEndian(A) ? 0u : 4u);
  }
}

TEST(MIPSRecordABI, NarrowIntegerFieldUsesTheSameLaneInArgumentAndResult) {
  for (Arch A : Architectures) {
    SCOPED_TRACE(unsigned(A));
    const auto Value = record({NdType::makeInt(4, false)});
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Signature.ReturnType = Value;
    Signature.Parameters.push_back({"value", Value});
    ASSERT_TRUE(mips::assignRecordSourceABI(Signature, A, BinaryFormat::ELF));
    const auto Parameters = sourceABIParameters(Signature);
    ASSERT_EQ(Parameters.size(), 1u);
    ASSERT_EQ(Signature.ReturnComponents.size(), 1u);
    EXPECT_EQ(Parameters.front().Location.RegisterOffset, mipsreg::r(4));
    EXPECT_EQ(Signature.ReturnComponents.front().RegisterOffset, mipsreg::r(2));
    for (const auto &Location :
         {Parameters.front().Location, Signature.ReturnComponents.front()}) {
      EXPECT_EQ(Location.Kind, SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(Location.ValueBytes, 4u);
      EXPECT_EQ(Location.CarrierBytes, 8u);
      EXPECT_EQ(Location.RegisterByteOffset, archLittleEndian(A) ? 0u : 4u);
    }
  }
}

TEST(MIPSRecordABI, NaturalLayoutAloneDoesNotAuthorizeSourceCalls) {
  for (Arch A : Architectures)
    for (const auto &Type :
         {bytes(3), record({NdType::makeFloat(8)}),
          record({NdType::makeInt(8), NdType::makeInt(8)}), bytes(17)}) {
      ASSERT_TRUE(mips::placeRecordCall(A, BinaryFormat::ELF, Type, {Type}));
      SourceCallTypeHint Hint;
      Hint.CallKind = SourceCallTypeHint::Kind::Analyst;
      Hint.Signature.Architecture = A;
      Hint.Signature.Origin =
          SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Hint.Signature.ReturnType = Type;
      Hint.Signature.Parameters.push_back({"", Type});
      Hint.Signature.HasExplicitABI = true;
      std::string Diagnostic;
      EXPECT_FALSE(validateSourceABI(Hint.Signature, Diagnostic));
      EXPECT_TRUE(sourceABIParameters(Hint.Signature).empty());
      EXPECT_FALSE(sourceABICallResultType(Hint.Signature));
      ASSERT_TRUE(assignAnalystCallABI(Hint, A, BinaryFormat::ELF, Diagnostic));
      EXPECT_TRUE(validateSourceABI(Hint.Signature, Diagnostic));
      EXPECT_TRUE(Hint.Signature.MIPSRecords);
      if (Type->Size > 16) {
        EXPECT_EQ(Hint.Signature.ReturnLocation.Kind,
                  SourceABICarrierKind::IndirectResultPointer);
        EXPECT_TRUE(Hint.Signature.ReturnComponents.empty());
      } else {
        ASSERT_EQ(Hint.Signature.ReturnComponents.size(), Type->Fields.size());
        if (mips::isByteRecord(Type)) {
          const auto &L = Hint.Signature.ReturnComponents.front();
          EXPECT_EQ(L.CarrierBytes, 8);
          EXPECT_EQ(L.RegisterByteOffset, archLittleEndian(A) ? 0 : 5);
        }
      }
      auto Bad = Hint.Signature;
      Bad.Parameters.front().Components.front().RegisterOffset += 8;
      EXPECT_FALSE(validateSourceABI(Bad, Diagnostic));
    }
}

TEST(MIPSRecordABI, IntegerCallsKeepPartialRegisterAndStackWords) {
  for (Arch A : Architectures)
    for (uint16_t Bytes : {3, 5, 6, 7, 9, 10, 13, 15, 16, 17})
      for (unsigned Prefix : {0u, 7u, 8u}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << ':' << Bytes << ':' << Prefix);
        std::vector<TypeRef> Parameters(Prefix, NdType::makeInt(8, false));
        Parameters.push_back(bytes(Bytes));
        Parameters.push_back(NdType::makeInt(8, false));
        const auto Call = integerCallArguments(A, BinaryFormat::ELF,
                                               NdType::makeInt(8), Parameters);
        ASSERT_TRUE(Call);
        ASSERT_EQ(Call->Parameters.size(), Parameters.size());
        const unsigned Words = (Bytes + 7) / 8;
        EXPECT_EQ(Call->Registers, std::min(8u, Prefix + Words + 1));
        EXPECT_EQ(Call->Positions, Prefix + Words + 1);
        const auto &Pieces = Call->Parameters[Prefix].Pieces;
        ASSERT_EQ(Pieces.size(), Words);
        for (unsigned I = 0; I != Words; ++I) {
          const unsigned Count = std::min(8u, unsigned(Bytes) - I * 8);
          EXPECT_EQ(Pieces[I].Argument, Prefix + I);
          EXPECT_EQ(Pieces[I].Offset, I * 8);
          EXPECT_EQ(Pieces[I].Bytes, Count);
          EXPECT_EQ(Pieces[I].CarrierBytes, 8);
          EXPECT_EQ(Pieces[I].BitOffset,
                    archLittleEndian(A) ? 0 : (8 - Count) * 8);
        }
        EXPECT_EQ(Call->Parameters.back().Pieces[0].Argument, Prefix + Words);
        EXPECT_EQ(Call->Parameters.back().Pieces[0].CarrierBytes, 0);
      }
}

TEST(MIPSRecordABI, AByteRecordResultOccupiesTheFirstAddressesOfV0) {
  for (Arch A : Architectures) {
    for (uint16_t Bytes : {3, 5, 6, 7, 8}) {
      const auto Layout = integerCallArguments(A, BinaryFormat::ELF,
                                               bytes(Bytes), {bytes(Bytes)});
      ASSERT_TRUE(Layout && Layout->RecordResult);
      const auto &P = *Layout->RecordResult;
      EXPECT_EQ(P.Bytes, Bytes);
      EXPECT_EQ(P.CarrierBytes, 8);
      EXPECT_EQ(P.BitOffset, archLittleEndian(A) ? 0 : (8 - Bytes) * 8);
    }
  }
}

TEST(MIPSRecordABI, IntegerAdapterDoesNotAdmitAnotherResultOrRegisterBank) {
  for (Arch A : Architectures) {
    for (const auto &Result :
         {bytes(17), NdType::makeFloat(8), NdType::makeInt(16)})
      EXPECT_FALSE(
          integerCallArguments(A, BinaryFormat::ELF, Result, {bytes(3)}));
    for (const auto &Parameter :
         {NdType::makeFloat(8), record({NdType::makeFloat(8)}),
          record({NdType::makeInt(8), NdType::makeFloat(8)})})
      EXPECT_FALSE(integerCallArguments(
          A, BinaryFormat::ELF, NdType::makeInt(8), {bytes(3), Parameter}));
    EXPECT_FALSE(integerCallArguments(A, BinaryFormat::COFF, NdType::makeInt(8),
                                      {bytes(3)}));
  }
}
} // namespace
