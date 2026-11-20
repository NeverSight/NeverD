//===- MIPSRecordEntryTests.cpp - C entry carrier reconstruction ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/MIPS/CRecordEntry.h"
#include "../../../lib/backend/llvm/MIPS/LLVMRecordABI.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/llvm/LLVMSourceCallValue.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/MIPS/SourceRecordABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Instructions.h"
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
TEST(MIPSRecordABI, TwoFloatsShareOneArgumentWordAndReturnInTwoFPRs) {
  llvm::LLVMContext Context;
  auto Float = NdType::makeFloat(4);
  auto Pair = NdType::makeStruct({Float, Float});
  Pair->Passing = NdRecordPassing::ByValue;
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    for (unsigned Prefix : {0u, 7u, 8u}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << '/' << Prefix);
      SourceFunctionTypeHint Sig;
      Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
      Sig.ReturnType = Pair;
      for (unsigned I = 0; I < Prefix; ++I)
        Sig.Parameters.push_back({"word", NdType::makeInt(8, false)});
      Sig.Parameters.push_back({"pair", Pair});
      ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
      std::string Diagnostic;
      ASSERT_TRUE(validateSourceABI(Sig, Diagnostic)) << Diagnostic;
      const auto Args = sourceABIParameters(Sig);
      ASSERT_EQ(Args.size(), Prefix + 1);
      const auto &Last = Args.back();
      EXPECT_EQ(Last.ParameterIndex, Prefix);
      EXPECT_EQ(Last.ByteOffset, 0);
      EXPECT_TRUE(llvmMIPSRecordPieceType(Context, Last, Sig)->isIntegerTy(64));
      EXPECT_EQ(Last.Location.Kind,
                Prefix == 8 ? SourceABICarrierKind::Stack
                            : SourceABICarrierKind::IntegerRegister);
      ASSERT_EQ(Sig.ReturnComponents.size(), 2u);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Sig.ReturnComponents[I].RegisterOffset, mipsreg::w(I * 2));
        EXPECT_EQ(Sig.ReturnComponents[I].ValueBytes, 4);
      }
      auto Bad = Sig;
      Bad.Parameters.back().Components.front().ValueBytes = 4;
      EXPECT_FALSE(validateSourceABI(Bad, Diagnostic));
    }
  }
}

TEST(MIPSRecordABI, MixedFloatingAndIntegerFieldsKeepIndependentCallClasses) {
  llvm::LLVMContext Context;
  auto D = NdType::makeFloat(8);
  auto U64 = NdType::makeInt(8, false);
  auto Mixed = NdType::makeStruct({D, U64});
  Mixed->Passing = NdRecordPassing::ByValue;
  EXPECT_TRUE(sourceAggregateMembers(Mixed).empty());
  const auto Members = sourceValueMembers(Mixed);
  ASSERT_EQ(Members.size(), 2u);
  EXPECT_EQ(Members[0].ByteOffset, 0);
  EXPECT_EQ(Members[1].ByteOffset, 8);
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SourceFunctionTypeHint Sig;
    Sig.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    Sig.ReturnType = Mixed;
    Sig.Parameters.push_back({"value", Mixed});
    ASSERT_TRUE(mips::assignRecordSourceABI(Sig, A, BinaryFormat::ELF));
    const auto Pieces = sourceABIParameters(Sig);
    ASSERT_EQ(Pieces.size(), 2u);
    EXPECT_TRUE(llvmMIPSRecordPieceType(Context, Pieces[0], Sig)->isDoubleTy());
    EXPECT_TRUE(
        llvmMIPSRecordPieceType(Context, Pieces[1], Sig)->isIntegerTy(64));
    EXPECT_EQ(Pieces[0].Location.RegisterOffset, mipsreg::w(12));
    EXPECT_EQ(Pieces[1].Location.RegisterOffset, mipsreg::r(5));
    const auto *Result =
        llvm::cast<llvm::StructType>(llvmSourceCallResultType(Context, Sig));
    ASSERT_EQ(Result->getNumElements(), 2u);
    EXPECT_TRUE(Result->getElementType(0)->isIntegerTy(64));
    EXPECT_TRUE(Result->getElementType(1)->isIntegerTy(64));
    auto Bad = Sig;
    Bad.ReturnComponents[0] = Pieces[0].Location;
    std::string Diagnostic;
    EXPECT_FALSE(validateSourceABI(Bad, Diagnostic));
  }
}

TEST(MIPSRecordABI, ByteRecordsBindEachPartialRegisterOrExactStackPiece) {
  llvm::LLVMContext Context;
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Count : {3u, 9u, 15u})
      for (unsigned Prefix : {0u, 7u, 8u}) {
        auto ByteArray =
            NdType::makeArray(NdType::makeInt(1, false), Count, Count);
        auto Record = NdType::makeStruct({ByteArray});
        Record->Passing = NdRecordPassing::ByValue;
        SourceFunctionTypeHint S;
        S.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
        S.ReturnType = Record;
        for (unsigned I = 0; I < Prefix; ++I)
          S.Parameters.push_back({"word", NdType::makeInt(8, false)});
        S.Parameters.push_back({"bytes", Record});
        ASSERT_TRUE(mips::assignRecordSourceABI(S, A, BinaryFormat::ELF));
        auto Parameters = sourceABIParameters(S);
        const unsigned Chunks = (Count + 7) / 8;
        ASSERT_EQ(Parameters.size(), Prefix + Chunks);
        ASSERT_EQ(S.ReturnComponents.size(), Chunks);
        auto *RT =
            llvm::cast<llvm::StructType>(llvmSourceCallResultType(Context, S));
        ASSERT_EQ(RT->getNumElements(), Chunks);
        for (unsigned J = 0; J < Chunks; ++J) {
          const auto &P = Parameters[Prefix + J];
          const unsigned Bytes = std::min(8u, Count - 8 * J);
          EXPECT_EQ(P.ByteOffset, 8 * J);
          EXPECT_EQ(P.Location.ValueBytes, Bytes);
          EXPECT_EQ(P.Location.CarrierBytes,
                    Prefix + J < 8 && Bytes < 8 ? 8 : 0);
          EXPECT_EQ(P.Location.RegisterByteOffset,
                    Prefix + J < 8 && !archLittleEndian(A) ? 8 - Bytes : 0);
          EXPECT_TRUE(
              llvmMIPSRecordPieceType(Context, P, S)->isIntegerTy(Bytes * 8));
          EXPECT_TRUE(RT->getElementType(J)->isIntegerTy(Bytes * 8));
        }
        auto Bad = S;
        Bad.ReturnComponents.back().RegisterByteOffset ^= 1;
        std::string Diagnostic;
        EXPECT_FALSE(validateSourceABI(Bad, Diagnostic));
      }
}

TEST(MIPSRecordABI, HiddenResultPreservesStorageWidthAndLLVMArgumentPositions) {
  llvm::LLVMContext Context;
  auto Bytes = NdType::makeArray(NdType::makeInt(1, false), 17, 17);
  auto Record = NdType::makeStruct({Bytes});
  Record->Passing = NdRecordPassing::ByValue;
  for (const auto A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                       Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    const auto &TRI = getTargetRegInfo(A);
    SourceFunctionTypeHint S;
    S.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    S.ReturnType = Record;
    S.Parameters.push_back({"object", Record});
    ASSERT_TRUE(mips::assignRecordSourceABI(S, A, BinaryFormat::ELF));
    EXPECT_EQ(S.ReturnLocation.Kind,
              SourceABICarrierKind::IndirectResultPointer);
    EXPECT_EQ(S.ReturnLocation.RegisterOffset, TRI.IntParamRegs.front());
    EXPECT_EQ(S.ReturnLocation.ValueBytes, TRI.PointerSize);
    EXPECT_TRUE(S.ReturnComponents.empty());
    EXPECT_TRUE(llvmSourceCallResultType(Context, S)->isVoidTy());
    const auto Pieces = sourceABIParameters(S);
    ASSERT_FALSE(Pieces.empty());
    EXPECT_EQ(Pieces.front().Location.RegisterOffset, TRI.IntParamRegs[1]);
    auto Attrs = mips::recordCallAttributes(Context, {}, S, Pieces);
    auto *Storage =
        llvm::dyn_cast_or_null<llvm::ArrayType>(Attrs.getParamStructRetType(0));
    ASSERT_TRUE(Storage);
    EXPECT_EQ(Storage->getNumElements(), 17u);
    EXPECT_FALSE(Attrs.hasParamAttr(0, llvm::Attribute::InReg));
    EXPECT_FALSE(Attrs.hasRetAttr(llvm::Attribute::InReg));
    for (unsigned I = 0; I < Pieces.size(); ++I)
      EXPECT_TRUE(Attrs.hasParamAttr(I + 1, llvm::Attribute::InReg));
    std::string Error;
    S.ReturnLocation.RegisterOffset = TRI.IntReturnReg;
    EXPECT_FALSE(validateSourceABI(S, Error));
  }
}

TEST(MIPSRecordABI, O32ScalarWordPairsAreOneCAndLLVMArgument) {
  llvm::LLVMContext Context;
  auto Bytes = NdType::makeArray(NdType::makeInt(1, false), 5, 5);
  auto Record = NdType::makeStruct({Bytes});
  Record->Passing = NdRecordPassing::ByValue;
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Stack : {false, true})
      for (bool Floating : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << ' ' << Stack << ' ' << Floating);
        SourceFunctionTypeHint S;
        S.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
        S.ReturnType = Record;
        const auto Scalar =
            Floating ? NdType::makeFloat(8) : NdType::makeInt(8, false);
        S.Parameters =
            Stack ? std::vector<SourceParameterTypeHint>{{"bytes", Record},
                                                         {"number", Scalar}}
                  : std::vector<SourceParameterTypeHint>{{"number", Scalar},
                                                         {"bytes", Record}};
        ASSERT_TRUE(mips::assignRecordSourceABI(S, A, BinaryFormat::ELF));
        const auto Parts = sourceABIParameters(S);
        const auto Args = llvmMIPSRecordArguments(Context, S, Parts);
        ASSERT_EQ(Args.size(), 3u);
        const unsigned I = Stack ? 2 : 0;
        const auto &Number = Args[I];
        ASSERT_EQ(Number.PieceCount, 2u);
        EXPECT_EQ(Number.Type, Floating ? llvm::Type::getDoubleTy(Context)
                                        : llvm::Type::getInt64Ty(Context));
        EXPECT_FALSE(Number.InReg);
        for (unsigned J = 0; J < 2; ++J) {
          const auto &P = Parts[Number.FirstPiece + J];
          EXPECT_EQ(P.ByteOffset, (archLittleEndian(A) ? J : 1 - J) * 4);
          EXPECT_EQ(P.Location.ValueBytes, 4u);
          if (Stack) {
            EXPECT_EQ(P.Location.Kind, SourceABICarrierKind::Stack);
            EXPECT_EQ(P.Location.EntryStackOffset, 16u + J * 4u);
          } else {
            EXPECT_EQ(P.Location.RegisterOffset,
                      getTargetRegInfo(A).IntParamRegs[2 + J]);
          }
        }
        const auto Attrs = mips::recordCallAttributes(Context, {}, S, Parts);
        EXPECT_FALSE(Attrs.hasParamAttr(I + 1, llvm::Attribute::InReg));
        EXPECT_EQ(Attrs.hasParamAttr(I + 1, llvm::Attribute::ZExt), !Floating);
        auto Changed = S;
        std::swap(Changed.Parameters[Stack ? 1 : 0].Components[0],
                  Changed.Parameters[Stack ? 1 : 0].Components[1]);
        std::string Diagnostic;
        EXPECT_FALSE(validateSourceABI(Changed, Diagnostic));
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
      Unknown.Output.Size = 8;
      Unknown.addInput(MedVar::makeConst(unsigned(Intrinsic::MIPSUnpredictable),
                                         sizeof(Intrinsic)));
      const auto Register = Unknown.Output;
      auto &Upper = Ops.emplace_back();
      Upper.Opcode = NdOp::SUBBYTES;
      Upper.Output = Word;
      Upper.Output.Id = 4;
      Upper.addInput(Register);
      Upper.addInput(MedVar::makeConst(0, 4));
      const auto UpperWord = Upper.Output;
      auto &Join = Ops.emplace_back();
      Join.Opcode = NdOp::CONCAT;
      Join.Output = Word;
      Join.Output.Id = 2;
      Join.Output.Size = 8;
      Join.addInput(UpperWord);
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

TEST(MIPSRecordABI, LLVMReturnsNestedDoubleFieldsThroughIntegerRegisters) {
  llvm::LLVMContext Context;
  for (Arch A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    const auto D = NdType::makeFloat(8);
    auto Nested = NdType::makeStruct({D});
    Nested->Passing = NdRecordPassing::ByValue;
    auto Pair = NdType::makeStruct({Nested, Nested});
    Pair->Passing = NdRecordPassing::ByValue;
    SourceFunctionTypeHint S;
    S.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
    S.ReturnType = Pair;
    S.Parameters.push_back({"input", Pair});
    ASSERT_TRUE(mips::assignRecordSourceABI(S, A, BinaryFormat::ELF));
    auto *Type =
        llvm::cast<llvm::StructType>(llvmSourceCallResultType(Context, S));
    ASSERT_EQ(Type->getNumElements(), 2u);
    EXPECT_TRUE(Type->getElementType(0)->isIntegerTy(64));
    EXPECT_TRUE(Type->getElementType(1)->isIntegerTy(64));
  }
}

TEST(MIPSRecordSourceEntry, AlignedTailMayLeaveADeadIncomingStackSlot) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    RecordFixture F(A, 13);
    F.Function.Params.emplace_back("tail", NdType::makeInt(8, false));
    for (int I = 5; I < 8; ++I)
      F.Inputs.push_back({{kNoParamReg, I, 4}, NdTypeKind::Int, "uint32_t"});
    EXPECT_FALSE(F.project());
    F.Inputs[5].Unused = true;
    ASSERT_TRUE(F.project());
    F.Inputs[5].Site.MedIndex = -1;
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordSourceEntry, StackTailAlwaysOccupiesTheFirstAddressedBytes) {
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL}) {
    RecordFixture F(A, 15);
    const auto Entry = F.project();
    ASSERT_TRUE(Entry);
    EXPECT_NE(Entry->Definition.find(
                  "__builtin_memcpy((unsigned char *)&__neverd_word4, (const "
                  "unsigned char *)&__neverd_arg0 + 12, 3)"),
              std::string::npos);
    MedVar First;
    First.Kind = MedVar::Param;
    First.TheArch = A;
    First.Id = 4;
    First.RegOff = kNoParamReg;
    MedOp Read;
    int NextId = 42;
    std::vector<MedOp> Prefix;
    med_calling_conv_detail::composeStackSlots(Read, First, 4, 10, NextId,
                                               Prefix);
    ASSERT_EQ(Prefix.size(), 2u);
    const bool Little = archLittleEndian(A);
    EXPECT_EQ(Prefix[0].Inputs[0].Id, Little ? 5 : 4);
    EXPECT_EQ(Prefix[0].Inputs[1].Id, Little ? 4 : 5);
    EXPECT_EQ(Prefix[1].Opcode, NdOp::SUBBYTES);
    EXPECT_EQ(Prefix[1].Inputs[0].Id, 6);
    EXPECT_EQ(Prefix[1].Inputs[1].ConstVal, Little ? 0u : 2u);
    EXPECT_EQ(Read.Inputs[0].Id, Little ? 43 : 42);
    EXPECT_EQ(Read.Inputs[1].Id, Little ? 42 : 43);
  }
}

TEST(MIPSRecordSourceEntry, N32StackReadsUseEightByteSlotsInMemoryOrder) {
  for (Arch A : {Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    for (bool Wide : {false, true}) {
      SCOPED_TRACE(::testing::Message() << unsigned(A) << '/' << Wide);
      auto U64 = NdType::makeInt(8, false);
      MedFunc F;
      F.Entry = 0x1000;
      F.Name = "read_stack";
      F.ReturnType = U64;
      for (int I = 0; I < 2; ++I) {
        MedVar Slot;
        Slot.Kind = MedVar::Param;
        Slot.TheArch = A;
        Slot.RegOff = kNoParamReg;
        Slot.Size = 8;
        Slot.Id = I;
        F.Params.push_back(Slot);
        F.TypedParams.push_back({I ? "second" : "first", U64});
      }
      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = F.Entry;
      MedOp Read;
      Read.Opcode = Wide ? NdOp::SUBBYTES : NdOp::COPY;
      auto Bytes = F.Params.front();
      Bytes.Size = Wide ? 16 : 8;
      Read.addInput(Bytes);
      if (Wide)
        Read.addInput(MedVar::makeConst(8, 4));
      Read.Output.Kind = MedVar::Reg;
      Read.Output.TheArch = A;
      Read.Output.RegOff = getTargetRegInfo(A).IntReturnReg;
      Read.Output.Id = 10;
      Read.Output.SSAVer = 1;
      Read.Output.Size = 8;
      Block.Ops.push_back(Read);
      MedOp Ret;
      Ret.Opcode = NdOp::RETURN;
      Ret.addInput(Read.Output);
      Block.Ops.push_back(Ret);
      F.Blocks.push_back(std::move(Block));
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit({F}, Context, "stack", A, {}, nullptr,
                                          BinaryFormat::ELF);
      ASSERT_TRUE(Module);
      Pipeline::optimizeModule(*Module);
      auto *Fn = Module->getFunction(F.Name);
      ASSERT_TRUE(Fn);
      ASSERT_EQ(Fn->arg_size(), 2u);
      auto *Return =
          llvm::dyn_cast<llvm::ReturnInst>(Fn->back().getTerminator());
      ASSERT_TRUE(Return);
      EXPECT_EQ(Return->getReturnValue(),
                Fn->getArg(Wide && archLittleEndian(A) ? 1 : 0));
    }
  }
}

} // namespace
