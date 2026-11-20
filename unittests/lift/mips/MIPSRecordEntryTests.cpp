//===- MIPSRecordEntryTests.cpp - C entry carrier reconstruction ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/MIPS/CRecordEntry.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/llvm/LLVMSourceCallValue.h"
#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/LLVMContext.h"

using namespace neverd;

namespace {
struct RecordFixture {
  BinaryImage Image;
  NullDebugContext Debug;
  CEmitterOptions Options;
  FunctionSym Function;
  std::vector<mips::CEntryCarrier> Inputs;
  mips::CEntryCarrier Result;

  RecordFixture(Arch Target, uint16_t Count)
      : Result{{kNoParamReg, -1, 8}, NdTypeKind::Int, "uint64_t"} {
    Image.Arch = Target;
    Image.Format = BinaryFormat::ELF;
    Image.MIPS.emplace();
    Options.Image = &Image;
    Options.TheArch = Target;
    Options.Format = BinaryFormat::ELF;
    Options.RecordDefinitions = [](const auto &) {
      CRecordDefinitions D;
      D.Tags.emplace("Payload", false);
      return D;
    };
    auto Record = NdType::makeStruct(
        {NdType::makeArray(NdType::makeInt(1, false), Count, Count)});
    Record->Passing = NdRecordPassing::ByValue;
    Record->SourceName = "Payload";
    Function.Addr = 0x1000;
    Function.ExplicitEntryDeclaration = true;
    Function.Params.emplace_back("value", Record);
    Function.ReturnType = Record;
    const auto Layout =
        getTargetRegInfo(Target).integerArgumentLayout(Options.Format);
    const unsigned Hidden = isMIPSO32(Target) || Count > 16;
    for (unsigned I = 0;
         I < Hidden + (Count + Layout.SlotBytes - 1) / Layout.SlotBytes; ++I) {
      const auto Reg =
          I < Layout.Registers.size() ? Layout.Registers[I] : kNoParamReg;
      Inputs.push_back({{Reg, static_cast<int>(I), Layout.SlotBytes},
                        NdTypeKind::Int,
                        Layout.SlotBytes == 4 ? "uint32_t" : "uint64_t"});
    }
  }

  auto project() const {
    return mips::cRecordEntry(Options, &Debug, Function, Function.Addr, "entry",
                              Inputs, Result);
  }
};

TEST(MIPSRecordSourceEntry, PartialWordUsesTheSourceObjectRepresentation) {
  for (const auto A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 3);
    const auto Entry = F.project();
    ASSERT_TRUE(Entry);
    EXPECT_EQ(Entry->Declaration,
              "struct Payload entry(struct Payload __neverd_arg0)");
    // A short record occupies the lowest addressed bytes on both orders.
    // The significant bits of its carrier differ between them.
    EXPECT_NE(Entry->Definition.find(
                  "__builtin_memcpy((unsigned char *)&__neverd_word0, (const "
                  "unsigned char *)&__neverd_arg0, 3)"),
              std::string::npos);
    EXPECT_NE(Entry->Definition.find(
                  "__builtin_memcpy((unsigned char *)&__neverd_result, (const "
                  "unsigned char *)&__neverd_return_word, 3)"),
              std::string::npos);
    F.Inputs.front().Site.RegOff = getTargetRegInfo(A).IntReturnReg;
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordSourceEntry, HiddenStorageIsTakenFromTheEntryArgument) {
  for (const auto A :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64O32, Arch::MIPS64O32EL,
        Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 17);
    const auto Entry = F.project();
    ASSERT_TRUE(Entry) << static_cast<unsigned>(A);
    EXPECT_NE(Entry->Definition.find("(uintptr_t)&__neverd_result"),
              std::string::npos);
    EXPECT_EQ(Entry->Definition.find("__neverd_return_word"),
              std::string::npos);
    F.Inputs.erase(F.Inputs.begin());
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordSourceEntry,
     IncompleteOrUnboundDeclarationsRetainMachineCarriers) {
  RecordFixture F(Arch::MIPS64N32EL, 3);
  F.Function.ExplicitEntryDeclaration = false;
  EXPECT_FALSE(F.project());
  F.Function.ExplicitEntryDeclaration = true;
  F.Function.ReturnType->Passing = NdRecordPassing::Unknown;
  EXPECT_FALSE(F.project());
}
TEST(MIPSRecordSourceEntry,
     FloatingRecordCallDefinesItsTwoReturnRegistersOnly) {
  for (auto A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 8);
    F.Image.MIPS->FPABI = 1;
    auto Pair =
        NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8)});
    Pair->Passing = NdRecordPassing::ByValue;
    SourceFunctionTypeHint Signature;
    Signature.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Signature.ReturnType = Pair;
    Signature.Parameters.push_back({"input", Pair});
    ASSERT_TRUE(mips::assignRecordSourceABI(Signature, A, BinaryFormat::ELF));
    std::map<va_t, SourceFunctionTypeHint> Callees{{0x2000, Signature}};
    LowFunc Body;
    Body.Entry = F.Function.Addr;
    auto &Ops = Body.Blocks.emplace_back().Ops;
    auto &Call = Ops.emplace_back();
    Call.Addr = Body.Entry;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(0x2000, 8));
    auto &Use = Ops.emplace_back();
    Use.Addr = Body.Entry + 8;
    Use.Opcode = NdOp::COPY;
    Use.Output = NdVar::reg(mipsreg::V0, 8);
    Use.addInput(NdVar::reg(mipsreg::w(2), 8));
    EXPECT_NO_THROW(requireMIPSSourceVectors(Body, A, &F.Image, nullptr,
                                             nullptr, nullptr, &Callees));
    Use.Inputs[0] = NdVar::reg(mipsreg::w(4), 8);
    EXPECT_THROW(requireMIPSSourceVectors(Body, A, &F.Image, nullptr, nullptr,
                                          nullptr, &Callees),
                 LowToMedConversionError);
  }
}
TEST(MIPSRecordSourceEntry, TwoRegisterResultKeepsThePartialSecondWord) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 9);
    EXPECT_FALSE(F.project());
    F.Result = {{kNoParamReg, -1, 16},
                NdTypeKind::Struct,
                "word_pair",
                {{NdTypeKind::Int, 0, 8}, {NdTypeKind::Int, 8, 8}}};
    const auto Entry = F.project();
    ASSERT_TRUE(Entry);
    EXPECT_NE(Entry->Definition.find("&__neverd_result + 8, (const unsigned "
                                     "char *)&__neverd_return_word + 8, 1)"),
              std::string::npos);
    F.Result.Fields.back().Kind = NdTypeKind::Float;
    EXPECT_FALSE(F.project());
    F.Result.Fields = {{NdTypeKind::Int, 0, 8}, {NdTypeKind::Int, 0, 8}};
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordABI, LLVMRecordArgumentsUseEachPhysicalScalar) {
  llvm::LLVMContext Context;
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    auto Double = NdType::makeFloat(8);
    auto Pair = NdType::makeStruct({Double, Double});
    Pair->Passing = NdRecordPassing::ByValue;
    for (unsigned Prefix : {0u, 7u, 8u}) {
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Pair;
      for (unsigned I = 0; I < Prefix; ++I)
        Sig.Parameters.push_back({"word", NdType::makeInt(8, false)});
      Sig.Parameters.push_back({"pair", Pair});
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      std::string Diagnostic;
      ASSERT_TRUE(validateSourceABI(Sig, Diagnostic)) << Diagnostic;
      const auto Operands = sourceABIParameters(Sig);
      ASSERT_EQ(Operands.size(), Prefix + 2);
      for (unsigned I = 0; I < Operands.size(); ++I) {
        const auto *Type = llvmMIPSRecordPieceType(Context, Operands[I], Sig);
        if (I < Prefix) {
          EXPECT_TRUE(Type->isIntegerTy(64));
        } else {
          EXPECT_TRUE(Type->isDoubleTy());
          EXPECT_EQ(Operands[I].ParameterIndex, Prefix);
          EXPECT_EQ(Operands[I].ByteOffset, (I - Prefix) * 8);
        }
      }
    }
    auto Pointer = NdType::makePtr(NdType::makeInt(1));
    Pointer->Size = getTargetRegInfo(A).PointerSize;
    if (Pointer->Size == 8) {
      auto PointerPair = NdType::makeStruct({Pointer, Pointer});
      PointerPair->Passing = NdRecordPassing::ByValue;
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = NdType::makeVoid();
      Sig.Parameters.push_back({"pointer", Pointer});
      Sig.Parameters.push_back({"record", PointerPair});
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      auto Pieces = sourceABIParameters(Sig);
      ASSERT_EQ(Pieces.size(), 3u);
      EXPECT_TRUE(
          llvmMIPSRecordPieceType(Context, Pieces[0], Sig)->isPointerTy());
      EXPECT_TRUE(
          llvmMIPSRecordPieceType(Context, Pieces[1], Sig)->isIntegerTy(64));
      EXPECT_TRUE(
          llvmMIPSRecordPieceType(Context, Pieces[2], Sig)->isIntegerTy(64));
    }
  }
}
TEST(MIPSRecordSourceEntry, AlignmentGapRequiresAnUnreadIntegerSlot) {
  for (const auto A : {Arch::MIPS32, Arch::MIPS32EL}) {
    RecordFixture F(A, 8);
    auto Record = NdType::makeStruct({NdType::makeFloat(8)});
    Record->Passing = NdRecordPassing::ByValue;
    Record->SourceName = "Payload";
    F.Function.Params.front().second = Record;
    F.Function.ReturnType = Record;
    F.Inputs.push_back({{mipsreg::A3, 3, 4}, NdTypeKind::Int, "uint32_t"});
    ASSERT_EQ(F.Inputs[1].Site.RegOff, mipsreg::A1);
    EXPECT_FALSE(F.project());
    F.Inputs[1].Unused = true;
    const auto Entry = F.project();
    ASSERT_TRUE(Entry);
    EXPECT_FALSE(Entry->BodyReturnsVoid);
    EXPECT_NE(Entry->Definition.find(", 0, __neverd_word2"), std::string::npos);
    F.Inputs[1].Site.RegOff = mipsreg::RA;
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordSourceEntry, LargeNewABIResultHasNoScalarBodyReturn) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 17);
    const auto Entry = F.project();
    ASSERT_TRUE(Entry);
    EXPECT_TRUE(Entry->BodyReturnsVoid);
  }
}

TEST(MIPSRecordSourceEntry, OnlyUnobservedFPReturnBytesCanBeFilled) {
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 8);
    F.Image.MIPS->FPABI = 1;
    for (bool ReadUpper : {false, true}) {
      MedFunc Body;
      Body.Entry = F.Function.Addr;
      Body.ExplicitMIPSRecordResult = true;
      Body.ReturnType =
          NdType::makeStruct({NdType::makeFloat(4), NdType::makeFloat(4)});
      Body.MultiReturn = {{mipsreg::w(0), 4, true}, {mipsreg::w(2), 4, true}};
      auto &Ops = Body.Blocks.emplace_back().Ops;
      MedVar Word;
      Word.Kind = MedVar::Temp;
      Word.TheArch = A;
      Word.Size = 4;
      Word.Id = 1;
      auto &Unknown = Ops.emplace_back();
      Unknown.Opcode = NdOp::INTRINSIC;
      Unknown.Output = Word;
      Unknown.addInput(MedVar::makeConst(unsigned(Intrinsic::MIPSUnpredictable),
                                         sizeof(Intrinsic)));
      auto &Join = Ops.emplace_back();
      Join.Opcode = NdOp::CONCAT;
      Join.Output = Word;
      Join.Output.Id = 2;
      Join.Output.Size = 8;
      Join.addInput(Word);
      Join.addInput(MedVar::makeConst(0, 4));
      const auto Joined = Join.Output;
      auto &Part = Ops.emplace_back();
      Part.Opcode = NdOp::SUBBYTES;
      Part.Output = Word;
      Part.Output.Id = 3;
      Part.addInput(Joined);
      Part.addInput(MedVar::makeConst(ReadUpper ? 4 : 0, 4));
      const auto Used = Part.Output;
      auto &Ret = Ops.emplace_back();
      Ret.Opcode = NdOp::RETURN;
      Ret.addInput(Used);
      Ret.addInput(MedVar::makeConst(0, 4));
      discardUnobservedMIPSFPUpperBits(Body, F.Image, {}, nullptr);
      EXPECT_EQ(Ops.front().Opcode, ReadUpper ? NdOp::INTRINSIC : NdOp::COPY);
    }
  }
}

} // namespace
