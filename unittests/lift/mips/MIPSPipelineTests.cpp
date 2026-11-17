//===- MIPSPipelineTests.cpp - MIPS ELF to source and LLVM ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate admitted ELF profiles through the production pipeline, including
/// ABI recovery, source emission and the target's LLVM data layout.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSSourceAccumulators.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"

#include <stdexcept>

using namespace neverd;

namespace {

class MIPSPipeline : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSPipeline, HistoricalTimingCannotDisappearIntoUnboundSource) {
  const unsigned Level = isMIPS64(GetParam()) ? 3 : 1;
  mips_test::ELFBytes File(GetParam());
  File.put(File.Is64 ? 48 : 36,
           ((Level - 1) << 28) | (File.Is64 ? 0u : 0x1000u), 4);
  // Even a dead load result retains its physical instruction period. The
  // lifted trace needs caller-bound incoming state; C cannot assume it empty.
  const uint32_t Words[] = {0x8c820000, 0x24020007, 0x03e00008, 0};
  for (unsigned I = 0; I != 4; ++I)
    File.put(0x100 + 4 * I, Words[I], 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (bool LLVM : {false, true}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    if (LLVM) {
      EXPECT_FALSE(Result.Success);
      EXPECT_NE(Result.Error.find(mips::kStatefulSourceUnsupported),
                std::string::npos)
          << Result.Error;
      continue;
    }
    ASSERT_TRUE(Result.Success) << Result.Error;
    EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
    unsigned Begins = 0, Retires = 0;
    for (const auto &Function : Result.MedFuncs)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::INTRINSIC) {
            Begins += Op.Inputs[0].ConstVal ==
                      unsigned(Intrinsic::MIPSHistoricalBegin);
            Retires += Op.Inputs[0].ConstVal ==
                       unsigned(Intrinsic::MIPSHistoricalRetire);
          }
    EXPECT_EQ(Begins, 4u);
    EXPECT_EQ(Retires, 4u);
    CEmitterOptions Emit;
    Emit.TheArch = GetParam();
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    try {
      (void)HighCEmitter().emit(Result.HighFuncs, Out, Emit);
      FAIL() << "historical scheduling emitted without a bound processor";
    } catch (const std::runtime_error &Error) {
      EXPECT_NE(
          std::string(Error.what()).find(mips::kStatefulSourceUnsupported),
          std::string::npos);
    }
  }
}

TEST(MIPSFloatingResult, ComplexResultsKeepTheirClassAndBothRegisters) {
  for (const auto Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    for (const uint16_t Bytes : {4, 8}) {
      for (const auto Mode :
           {InstructionMode::MIPSClassic, InstructionMode::MIPS16}) {
        SCOPED_TRACE(testing::Message() << getArchName(Target) << ':' << Bytes
                                        << ':' << static_cast<unsigned>(Mode));
        auto Pointer = NdType::makePtr();
        Pointer->Size = 4;
        const auto Result = NdType::makeComplex(Bytes);
        const auto Signature = floatingCallArguments(
            Target, BinaryFormat::ELF, Result, {Pointer}, std::nullopt, Mode);
        ASSERT_TRUE(Signature);
        EXPECT_TRUE(Signature->ReturnComplex);
        EXPECT_EQ(Signature->ReturnElements, 2u);
        const auto Types =
            floatingCallTypes(Target, BinaryFormat::ELF, *Signature);
        ASSERT_TRUE(Types);
        EXPECT_TRUE(equalSourceTypes(Result, Types->Result));
        EXPECT_EQ(Types->Parameters.size(), 1u);
        auto Corrupt = *Signature;
        Corrupt.ReturnComplex = false;
        EXPECT_FALSE(floatingCallTypes(Target, BinaryFormat::ELF, Corrupt));
        Corrupt = *Signature;
        Corrupt.ReturnElements = 1;
        EXPECT_FALSE(floatingCallTypes(Target, BinaryFormat::ELF, Corrupt));
        SourceCallTypeHint Call;
        Call.Signature.ReturnType = Result;
        Call.Signature.Parameters = {{"input", Pointer}};
        std::string Error;
        ASSERT_TRUE(
            assignAnalystCallABI(Call, Target, BinaryFormat::ELF, Error, Mode))
            << Error;
        ASSERT_EQ(Call.Signature.ReturnComponents.size(), 2u);
        for (unsigned I = 0; I != 2; ++I) {
          const auto &Part = Call.Signature.ReturnComponents[I];
          EXPECT_EQ(Part.Kind, SourceABICarrierKind::FloatingRegister);
          EXPECT_EQ(Part.RegisterOffset, mipsreg::w(2 * I));
          EXPECT_EQ(Part.ValueBytes, Bytes);
        }
        Call.Signature.ReturnComponents[1].RegisterOffset = mipsreg::w(1);
        EXPECT_FALSE(validateSourceABI(Call.Signature, Error));
        auto Record = NdType::makeStruct(
            {NdType::makeFloat(Bytes), NdType::makeFloat(Bytes)});
        Record->Passing = NdRecordPassing::ByValue;
        EXPECT_FALSE(floatingCallArguments(Target, BinaryFormat::ELF, Record,
                                           {Pointer}));
        EXPECT_FALSE(
            floatingCallArguments(Target, BinaryFormat::ELF, Result, {Result}));
      }
    }
  }
  for (const auto Target :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    EXPECT_FALSE(floatingCallArguments(Target, BinaryFormat::ELF,
                                       NdType::makeComplex(8), {}));
}

TEST(MIPSFloatingResult, IntegerExtensionRequiresAnEntryDeclarationAndFP64) {
  for (const auto Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                            Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(getArchName(Target));
    BinaryImage Image;
    Image.Arch = Target;
    Image.MIPS.emplace();
    Image.MIPS->ISARevision = 6;
    const auto Signature =
        floatingCallArguments(Target, BinaryFormat::ELF,
                              NdType::makeInt(1, true), {NdType::makeFloat(8)});
    ASSERT_TRUE(Signature);
    MedFunc Function;
    Function.Entry = 0x10000;
    const auto Inferred = NdType::makeInt(4, true);
    Function.ReturnType = Inferred;
    const std::map<va_t, FloatingCallArguments> Declared{
        {Function.Entry, *Signature}};
    discardUnobservedMIPSFPUpperBits(Function, Image, Declared, nullptr);
    EXPECT_FALSE(Function.HasDeclaredMIPSIntegerResult);
    EXPECT_EQ(Function.ReturnType, Inferred);
    Image.MIPS->FPABI = isMIPS64(Target) ? 1 : 6;
    discardUnobservedMIPSFPUpperBits(Function, Image, {}, nullptr);
    EXPECT_FALSE(Function.HasDeclaredMIPSIntegerResult);
    EXPECT_EQ(Function.ReturnType, Inferred);
    discardUnobservedMIPSFPUpperBits(Function, Image, Declared, nullptr);
    EXPECT_TRUE(Function.HasDeclaredMIPSIntegerResult);
    ASSERT_TRUE(Function.ReturnType);
    EXPECT_EQ(Function.ReturnType->Size, 1u);
    EXPECT_TRUE(Function.ReturnType->IsSigned);
  }
}

TEST_P(MIPSPipeline, ScalarFPBitReturnRetainsObservedUnknownUpperWords) {
  for (unsigned Revision : {2u, 6u})
    for (bool HighWord : {false, true}) {
      SCOPED_TRACE(Revision);
      SCOPED_TRACE(HighWord);
      mips_test::ELFBytes File(GetParam());
      File.abiFlags();
      if (Revision == 6)
        File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000 : 0x90001000, 4);
      File.Bytes[0xe3] = Revision;
      File.Bytes[0xe5] = 2;
      File.Bytes[0xe7] = File.Is64 ? 1 : 6;
      // mtc1 a0,f0; mtc1 a1,f2; add.s f4,f0,f2; mfc1/mfhc1 v0,f4; jr; nop
      const uint32_t Words[] = {0x44840000,
                                0x44851000,
                                0x46020100,
                                HighWord ? 0x44622000u : 0x44022000u,
                                Revision == 6 ? 0x03e00009u : 0x03e00008u,
                                0};
      for (unsigned I = 0; I < std::size(Words); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Symbol = Symbol::makeFunc(Image->Entry, sizeof(Words));
      Symbol.Name = "fp_bits";
      Image->Symbols.push_back(std::move(Symbol));
      for (bool LLVM : {false, true}) {
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Image->Entry};
        Options.LiftMode = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*Image, Context, Options);
        if (LLVM) {
          EXPECT_EQ(Result.Success, !HighWord) << Result.Error;
          if (Result.Success)
            EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        } else {
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions C;
          C.TheArch = GetParam();
          C.Format = BinaryFormat::ELF;
          C.Image = &*Image;
          std::string Text;
          llvm::raw_string_ostream Output(Text);
          if (HighWord)
            EXPECT_THROW(HighCEmitter().emit(Result.HighFuncs, Output, C),
                         std::runtime_error);
          else
            EXPECT_NO_THROW(HighCEmitter().emit(Result.HighFuncs, Output, C));
        }
      }
    }
}

TEST_P(MIPSPipeline, DoubleResultDiscardsOnlyOverwrittenUnknownFPRWords) {
  for (unsigned Revision : {2u, 6u})
    for (bool HighWord : {false, true}) {
      SCOPED_TRACE(Revision);
      SCOPED_TRACE(HighWord);
      mips_test::ELFBytes File(GetParam());
      File.abiFlags();
      if (Revision == 6)
        File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000 : 0x90001000, 4);
      File.Bytes[0xe3] = Revision;
      File.Bytes[0xe5] = 2;
      File.Bytes[0xe7] = File.Is64 ? 1 : 6;
      // MTC1 leaves f0's upper word unpredictable. CVT.D.W reads only the
      // defined low word, then replaces all of f0; ADD.D observes both words.
      const uint32_t Words[] = {0x44840000,
                                HighWord ? 0x46200000u : 0x46800021u,
                                Revision == 6 ? 0x03e00009u : 0x03e00008u, 0};
      for (unsigned I = 0; I < std::size(Words); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Symbol = Symbol::makeFunc(Image->Entry, sizeof(Words));
      Symbol.Name = "fp_double";
      Image->Symbols.push_back(std::move(Symbol));
      for (bool LLVM : {false, true}) {
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Image->Entry};
        Options.LiftMode = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*Image, Context, Options);
        if (LLVM) {
          EXPECT_EQ(Result.Success, !HighWord) << Result.Error;
          if (Result.Success)
            EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        } else {
          ASSERT_TRUE(Result.Success) << Result.Error;
          CEmitterOptions C;
          C.TheArch = GetParam();
          C.Format = BinaryFormat::ELF;
          C.Image = &*Image;
          std::string Text;
          llvm::raw_string_ostream Output(Text);
          if (HighWord)
            EXPECT_THROW(HighCEmitter().emit(Result.HighFuncs, Output, C),
                         std::runtime_error);
          else
            EXPECT_NO_THROW(HighCEmitter().emit(Result.HighFuncs, Output, C));
        }
      }
    }
}

TEST_P(MIPSPipeline, MixedFloatingEntryKeepsSourceParameterOrder) {
  mips_test::ELFBytes File(GetParam());
  File.abiFlags();
  File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000 : 0x90001000, 4);
  File.Bytes[0xe3] = 6;
  File.Bytes[0xe5] = 2;
  File.Bytes[0xe7] = File.Is64 ? 1 : 6;
  // double kernel(double value, int bias): value + (double)bias.
  // The double consumes one n64 position, or two o32 word positions.
  const uint32_t Words[] = {File.Is64 ? 0x44851000u : 0x44861000u, 0x468010a1,
                            0x46226000, 0x03e00009, 0};
  for (size_t I = 0; I != std::size(Words); ++I)
    File.put(0x100 + I * 4, Words[I], 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Symbol = Symbol::makeFunc(Image->Entry, sizeof(Words));
  Symbol.Name = "fp_mixed_entry";
  Image->Symbols.push_back(std::move(Symbol));
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.OnlyFunctionEntries = {Image->Entry};
  Options.LiftMode = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  const auto &Function = Result.MedFuncs.front();
  ASSERT_EQ(Function.Params.size(), 2u);
  EXPECT_EQ(Function.Params[0].RegOff, mipsreg::w(12));
  EXPECT_EQ(Function.Params[0].Size, 8);
  EXPECT_EQ(Function.Params[1].RegOff, mipsreg::r(File.Is64 ? 5 : 6));
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  const auto *Body = Result.LlvmModule->getFunction("fp_mixed_entry");
  ASSERT_NE(Body, nullptr);
  ASSERT_EQ(Body->arg_size(), 2u);
  EXPECT_TRUE(Body->getArg(0)->getType()->isDoubleTy());
  EXPECT_TRUE(Body->getArg(1)->getType()->isIntegerTy());
}

TEST_P(MIPSPipeline, ScalarFloatingEntryAndResultFollowHardFloatABI) {
  for (unsigned Bytes : {4u, 8u}) {
    SCOPED_TRACE(Bytes);
    mips_test::ELFBytes File(GetParam());
    File.abiFlags();
    File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000 : 0x90001000, 4);
    File.Bytes[0xe3] = 6;
    File.Bytes[0xe5] = 2;
    File.Bytes[0xe7] = File.Is64 ? 1 : 6;
    // ADD.S/D f0,f12,f13 (n64), or f14 (o32); JR ra; NOP.
    File.put(
        0x100,
        (File.Is64 ? 0x460d6000 : 0x460e6000) | (Bytes == 8 ? 0x200000 : 0), 4);
    File.put(0x104, 0x03e00009, 4);
    File.put(0x108, 0, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Symbol = Symbol::makeFunc(Image->Entry, 12);
    Symbol.Name = "fp_entry";
    Image->Symbols.push_back(std::move(Symbol));
    for (bool LLVM : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      const auto &Function = Result.MedFuncs.front();
      ASSERT_EQ(Function.Params.size(), 2u);
      EXPECT_EQ(Function.Params[0].RegOff, mipsreg::w(12));
      EXPECT_EQ(Function.Params[1].RegOff, mipsreg::w(File.Is64 ? 13 : 14));
      ASSERT_NE(Function.ReturnType, nullptr);
      EXPECT_EQ(Function.ReturnType->Kind, NdTypeKind::Float);
      EXPECT_EQ(Function.ReturnType->Size, Bytes);
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const auto *Body = Result.LlvmModule->getFunction("fp_entry");
        ASSERT_NE(Body, nullptr);
        EXPECT_TRUE(Bytes == 8 ? Body->getReturnType()->isDoubleTy()
                               : Body->getReturnType()->isFloatTy());
        ASSERT_EQ(Body->arg_size(), 2u);
        for (const auto &Argument : Body->args())
          EXPECT_TRUE(Bytes == 8 ? Argument.getType()->isDoubleTy()
                                 : Argument.getType()->isFloatTy());
      }
    }
  }
}

TEST_P(MIPSPipeline, PairedFR0EntryAndResultFollowO32ABI) {
  if (isMIPS64(GetParam()))
    return;
  for (unsigned Bytes : {4u, 8u}) {
    SCOPED_TRACE(Bytes);
    mips_test::ELFBytes File(GetParam());
    File.abiFlags();
    File.Bytes[0xe3] = 2;
    File.Bytes[0xe5] = 1;
    File.Bytes[0xe7] = 1;
    // ADD.S/D f0,f12,f13 (n64), or f14 (o32); JR ra; NOP.
    File.put(
        0x100,
        (File.Is64 ? 0x460d6000 : 0x460e6000) | (Bytes == 8 ? 0x200000 : 0), 4);
    File.put(0x104, 0x03e00008, 4);
    File.put(0x108, 0, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Symbol = Symbol::makeFunc(Image->Entry, 12);
    Symbol.Name = "fp_entry";
    Image->Symbols.push_back(std::move(Symbol));
    for (bool LLVM : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      const auto &Function = Result.MedFuncs.front();
      ASSERT_EQ(Function.Params.size(), 2u);
      EXPECT_EQ(Function.Params[0].RegOff, mipsreg::w(12));
      EXPECT_EQ(Function.Params[1].RegOff, mipsreg::w(File.Is64 ? 13 : 14));
      ASSERT_NE(Function.ReturnType, nullptr);
      EXPECT_EQ(Function.ReturnType->Kind, NdTypeKind::Float);
      EXPECT_EQ(Function.ReturnType->Size, Bytes);
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const auto *Body = Result.LlvmModule->getFunction("fp_entry");
        ASSERT_NE(Body, nullptr);
        EXPECT_TRUE(Bytes == 8 ? Body->getReturnType()->isDoubleTy()
                               : Body->getReturnType()->isFloatTy());
        ASSERT_EQ(Body->arg_size(), 2u);
        for (const auto &Argument : Body->args())
          EXPECT_TRUE(Bytes == 8 ? Argument.getType()->isDoubleTy()
                                 : Argument.getType()->isFloatTy());
      }
    }
  }
}

TEST_P(MIPSPipeline, FR0ScratchPairDoesNotConsumeAnIncomingArgument) {
  if (isMIPS64(GetParam()))
    return;
  for (const auto &Words : std::vector<std::vector<uint32_t>>{
           // Linear scratch pair, then use both physical words.
           {0x46206386, 0x44861000, 0x468010a1, 0x46227000, 0x03e00008, 0},
           // Both sides of a diamond define f14/f15 from the incoming f12/f13.
           {0x44861000, 0x468010a1, 0x04c10004, 0, 0x46206386, 0x10000002, 0,
            0x46206386, 0x46227000, 0x03e00008, 0}}) {
    SCOPED_TRACE(Words.size());
    mips_test::ELFBytes File(GetParam());
    File.Bytes.resize(0x140);
    File.put(52 + 16, File.Bytes.size(), 4);
    File.put(52 + 20, File.Bytes.size(), 4);
    File.abiFlags();
    File.Bytes[0xe3] = 2;
    File.Bytes[0xe5] = 1;
    File.Bytes[0xe7] = 1;
    for (unsigned I = 0; I < std::size(Words); ++I)
      File.put(0x100 + I * 4, Words[I], 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Image->Symbols.push_back(Symbol::makeFunc(Image->Entry, Words.size() * 4));
    for (bool LLVM : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      const auto &Function = Result.MedFuncs.front();
      ASSERT_EQ(Function.Params.size(), 2u);
      EXPECT_EQ(Function.Params[0].RegOff, mipsreg::w(12));
      EXPECT_EQ(Function.Params[1].RegOff, mipsreg::r(6));
      EXPECT_EQ(Function.FPParamScalarBytes.at(mipsreg::w(12)), 8u);
      ASSERT_NE(Function.ReturnType, nullptr);
      EXPECT_EQ(Function.ReturnType->Kind, NdTypeKind::Float);
      EXPECT_EQ(Function.ReturnType->Size, 8u);
      if (LLVM)
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    }
  }
}

TEST_P(MIPSPipeline, SinglePrecisionUpperWordCannotEscapeToMemory) {
  mips_test::ELFBytes File(GetParam());
  File.abiFlags();
  File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000 : 0x90001000, 4);
  File.Bytes[0xe3] = 6;
  File.Bytes[0xe5] = 2;
  File.Bytes[0xe7] = File.Is64 ? 1 : 6;
  // ADD.S f0,f12,f12; MFHC1 $8,f0; SW $8,0(a2); JR ra; NOP.
  // Only the low word is returned, but memory observes the unknown high word.
  const uint32_t Words[] = {0x460c6000, 0x44680000, 0xacc80000, 0x03e00009, 0};
  for (size_t I = 0; I < std::size(Words); ++I)
    File.put(0x100 + I * 4, Words[I], 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Image->Symbols.push_back(Symbol::makeFunc(Image->Entry, sizeof(Words)));
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.OnlyFunctionEntries = {Image->Entry};
  Options.LiftMode = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  EXPECT_FALSE(Result.Success);
  EXPECT_NE(Result.Error.find("architecturally unpredictable"),
            std::string::npos)
      << Result.Error;
}

TEST(MIPSScalarCalls, IntegerCallsDoNotRequireFloatingPointProfiles) {
  for (const Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                            Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(getArchName(Target));
    const bool Wide = isMIPS64(Target);
    const bool NarrowPointer =
        Target == Arch::MIPS64N32 || Target == Arch::MIPS64N32EL;
    mips_test::ELFBytes File(
        NarrowPointer
            ? (archLittleEndian(Target) ? Arch::MIPS32EL : Arch::MIPS32)
            : Target);
    File.abiFlags();
    if (NarrowPointer) {
      File.put(36, 0x80000020, 4);
      File.Bytes[0xe2] = 64;
      File.Bytes[0xe4] = 2;
    }
    const uint32_t Words[] = {
        Wide ? 0x67bdffe0u : 0x27bdffe0u, // allocate 32 bytes
        Wide ? 0xffbf0018u : 0xafbf001cu, // save the incoming link
        0x0c004080u,
        0,           // call 0x10200; nop
        0x24020007u, // return a constant after the call
        Wide ? 0xdfbf0018u : 0x8fbf001cu,
        0x03e00008u,
        Wide ? 0x67bd0020u : 0x27bd0020u};
    for (unsigned I = 0; I < std::size(Words); ++I)
      File.put(0x100 + I * 4, Words[I], 4);
    auto Pointer = NdType::makePtr();
    Pointer->Size = getTargetRegInfo(Target).PointerSize;
    struct SignatureCase {
      TypeRef Result;
      std::vector<TypeRef> Parameters;
      bool NeedsFloating;
    };
    const SignatureCase Signatures[] = {
        {NdType::makeVoid(), {}, false},
        {NdType::makeInt(4), {NdType::makeInt(4)}, false},
        {Pointer, {Pointer}, false},
        {NdType::makeVoid(), {NdType::makeFloat(4)}, true},
        {NdType::makeFloat(8), {}, true}};
    // Unspecified, soft-float and o32 FPXX do not establish the scalar FPR
    // profile. None is needed by a complete integer/pointer/void call.
    for (const uint8_t Profile : {0, 3, 5}) {
      SCOPED_TRACE(Profile);
      File.Bytes[0xe7] = Profile;
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      Decoder Decode;
      ASSERT_TRUE(Decode.init(*Image));
      const auto Low =
          CFGBuilder().build(*Image, Decode, Image->Entry, "scalar_call");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*Image);
      for (const auto &Case : Signatures) {
        SCOPED_TRACE(Case.NeedsFloating);
        SCOPED_TRACE(static_cast<int>(Case.Result->Kind));
        const auto Signature = floatingCallArguments(
            Target, BinaryFormat::ELF, Case.Result, Case.Parameters);
        ASSERT_TRUE(Signature);
        const std::map<va_t, FloatingCallArguments> Calls{
            {0x10200, *Signature}};
        Convert.setCallEntryFloatingArguments(&Calls);
        if (Case.NeedsFloating)
          EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                       LowToMedConversionError);
        else {
          EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
          auto Invalid = Calls;
          Invalid.begin()->second.ReturnBytes = 3;
          EXPECT_THROW(
              requireMIPSSourceVectors(Low, Target, &*Image, &Invalid, nullptr),
              LowToMedConversionError);
        }
      }
    }
  }
}

TEST(MIPSN32Calls, PointerSpillsPreserveTheIncomingLinkAcrossCalls) {
  for (const bool Little : {false, true})
    for (const bool WordAdjustment : {false, true}) {
      const Arch Target = Little ? Arch::MIPS64N32EL : Arch::MIPS64N32;
      SCOPED_TRACE(getArchName(Target));
      SCOPED_TRACE(WordAdjustment);
      mips_test::ELFBytes File(Little ? Arch::MIPS32EL : Arch::MIPS32);
      File.abiFlags();
      File.put(36, 0x80000020, 4); // MIPS64r2, ABI2, ELFCLASS32.
      File.Bytes[0xe2] = 64;
      File.Bytes[0xe4] = File.Bytes[0xe5] = 2;
      File.Bytes[0xe7] = 1;
      // Clang n32: save the eight-byte link and four-byte pointer, call a
      // declared void callee, reload the pointer and return its second word.
      const uint32_t Words[] = {WordAdjustment ? 0x27bdfff0u : 0x67bdfff0u,
                                0xffbf0008u,
                                0x0c004080u,
                                0xafa40004u,
                                0x8fa10004u,
                                0x8c220004u,
                                0xdfbf0008u,
                                0x03e00008u,
                                WordAdjustment ? 0x27bd0010u : 0x67bd0010u};
      File.Bytes.resize(0x208);
      File.put(52 + 16, File.Bytes.size(), 4);
      File.put(52 + 20, File.Bytes.size(), 4);
      for (unsigned I = 0; I < std::size(Words); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
      File.put(0x200, 0x03e00008, 4);
      File.put(0x204, 0, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Caller = Symbol::makeFunc(Image->Entry, sizeof(Words));
      Caller.Name = "pointer_spill";
      Image->Symbols.push_back(Caller);
      Image->Symbols.push_back(Symbol::makeFunc(0x10200, 8));
      Decoder Decode;
      ASSERT_TRUE(Decode.init(*Image));
      auto Low = CFGBuilder().build(*Image, Decode, Image->Entry, Caller.Name);
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      const auto Signature = floatingCallArguments(Target, BinaryFormat::ELF,
                                                   NdType::makeVoid(), {});
      ASSERT_TRUE(Signature);
      const std::map<va_t, FloatingCallArguments> Calls{{0x10200, *Signature}};
      LowToMedConverter Convert;
      Convert.setBinaryImage(&*Image);
      // Return identity is independent of the callee's source declaration.
      EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
      Convert.setCallEntryFloatingArguments(&Calls);
      EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
      // The declaration does not excuse an overlapping write to the saved
      // link. Move the pointer spill and reload into the eight-byte RA slot.
      File.put(0x10c, 0xafa40008, 4);
      File.put(0x110, 0x8fa10008, 4);
      auto Overwritten = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Overwritten)) << llvm::toString(Overwritten.takeError());
      ASSERT_TRUE(Decode.init(*Overwritten));
      const auto InvalidLow = CFGBuilder().build(
          *Overwritten, Decode, Overwritten->Entry, Caller.Name);
      ASSERT_TRUE(InvalidLow.hasCompleteLiftCoverage());
      Convert.setBinaryImage(&*Overwritten);
      try {
        (void)Convert.convert(InvalidLow, Target, BinaryFormat::ELF);
        FAIL() << "An overwritten link must not qualify as a source return";
      } catch (const LowToMedConversionError &Error) {
        EXPECT_NE(std::string(Error.what()).find("MIPS source return target"),
                  std::string::npos)
            << Error.what();
      }
      class NativeDebug final : public NullDebugContext {
      public:
        FunctionSym Function;
        bool Authenticated = true;
        AuthenticatedReturnValueState ReturnState{
            AuthenticatedReturnKind::NoValue, 0};
        NativeDebug() {
          Function.Addr = 0x10200;
          Function.Name = "touch";
          Function.ReturnType = NdType::makeVoid();
        }
        std::optional<FunctionSym>
        resolveFunction(va_t Address) const override {
          return Address == 0x10200 ? std::optional(Function) : std::nullopt;
        }
        bool hasAuthenticatedFunctionSignatures() const override {
          return Authenticated;
        }
        AuthenticatedReturnValueState
        resolveAuthenticatedReturnValueState(va_t Address) const override {
          return Address == Function.Addr ? ReturnState
                                          : AuthenticatedReturnValueState{};
        }
      };
      Export Callee;
      Callee.Name = "touch";
      Callee.Addr = 0x10200;
      for (const bool Exported : {false, true}) {
        SCOPED_TRACE(Exported);
        Image->Exports.clear();
        if (Exported)
          Image->Exports.push_back(Callee);
        for (unsigned Variant = 0; Variant < 10; ++Variant) {
          SCOPED_TRACE(Variant);
          NativeDebug Debug;
          switch (Variant) {
          case 1:
            Debug.Authenticated = false;
            break;
          case 2:
            Debug.ReturnState = {};
            break;
          case 3:
            Debug.Function.Addr -= 4;
            break;
          case 4:
            Debug.Function.IsVariadic = true;
            break;
          case 5:
            Debug.Function.PlatformConvention = false;
            break;
          case 6:
            Debug.Function.CallConv = DebugCallConv::Fastcall;
            break;
          case 7:
            Debug.Function.ParamEntryLocations.emplace_back();
            break;
          case 8:
            Debug.Function.Params = {{"unknown", TypeRef()}};
            break;
          case 9:
            Debug.Function.ReturnType = {};
            break;
          }
          for (bool LLVM : {false, true}) {
            SCOPED_TRACE(LLVM);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.OnlyFunctionEntries = {Image->Entry};
            Options.LiftMode = LLVM;
            Options.EmitDumpOutput = false;
            auto Result = Pipeline().run(*Image, Context, Options, &Debug);
            // Unauthenticated or incomplete types cannot grant a signature,
            // but do not invalidate the independently proven private RA slot.
            ASSERT_TRUE(Result.Success) << Result.Error;
            if (Variant != 0)
              EXPECT_FALSE(Result.CallEntryFloatingArguments.count(0x10200));
            if (LLVM) {
              ASSERT_NE(Result.LlvmModule, nullptr);
              EXPECT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            }
          }
        }
      }
    }
}

TEST(MIPSSourceReturnTargets, PrivateFrameSpillsDoNotPublishTheFrame) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32,
                      Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Variant = 0; Variant != 11; ++Variant) {
      SCOPED_TRACE(getArchName(Target));
      SCOPED_TRACE(Variant);
      const bool Wide = isMIPS64(Target);
      const bool N32 = Target == Arch::MIPS64N32 || Target == Arch::MIPS64N32EL;
      const unsigned Frame = Wide ? 64 : 32, Link = Wide ? 56 : 28;
      const uint16_t SavedFP = Variant == 2   ? (Wide ? uint16_t(-8) : 8)
                               : Variant == 3 ? Frame
                               : Variant == 7 ? Frame / 2
                                              : (Wide ? 48 : 24);
      // The normal prologue saves the incoming FP in its private frame.
      // 1 also spills its current SP privately; 2 saves FP in callee-owned
      // storage; 3 writes above the new frame; 4 publishes SP externally;
      // 5 passes SP to the call; 6 loses call SP; 7 exposes the FP slot to a
      // second call's frame; 8 has two calls with the same private area;
      // 9 reloads a private SP spill into a call argument; 10 publishes that
      // reloaded value through an external store.
      std::vector<uint32_t> Words{Wide ? 0x67bdffc0u : 0x27bdffe0u,
                                  (Wide ? 0xffbf0000u : 0xafbf0000u) | Link,
                                  (Wide ? 0xffbe0000u : 0xafbe0000u) | SavedFP,
                                  Wide ? 0x03a0f02du
                                       : 0x03a0f021u}; // daddu/addu fp,sp,zero
      if (Variant == 1 || Variant >= 9) {
        Words.push_back((Wide ? 0xffbd0000u : 0xafbd0000u) | Frame / 2);
        if (Variant == 9)
          Words.push_back((Wide ? 0xdfa40000u : 0x8fa40000u) | Frame / 2);
        else if (Variant == 10) {
          Words.push_back((Wide ? 0xdfa80000u : 0x8fa80000u) | Frame / 2);
          Words.push_back(Wide ? 0xfc880000u : 0xac880000u); // sd/sw t0,0(a0)
        }
      } else if (Variant == 4)
        Words.push_back(Wide ? 0xfc9d0000u : 0xac9d0000u); // sd/sw sp,0(a0)
      else if (Variant == 5)
        Words.push_back(Wide ? 0x03a0202du : 0x03a02021u); // a0 = sp
      else if (Variant == 6)
        Words.push_back(Wide ? 0x03a5e82fu : 0x03a5e823u); // sp -= a1
      Words.insert(Words.end(), {0x0c004080, 0});          // jal 0x10200; nop
      if (Variant == 7 || Variant == 8) {
        if (Variant == 7)
          Words.push_back(Wide ? 0x67bd0030u : 0x27bd0008u);
        Words.insert(Words.end(), {0x0c004080, 0});
      }
      Words.insert(Words.end(),
                   {Wide ? 0x03c0e82du : 0x03c0e821u, // sp = fp
                    (Wide ? 0xdfbe0000u : 0x8fbe0000u) | SavedFP,
                    (Wide ? 0xdfbf0000u : 0x8fbf0000u) | Link,
                    Wide ? 0x67bd0040u : 0x27bd0020u, 0x03e00008, 0});
      mips_test::ELFBytes File(
          N32 ? (archLittleEndian(Target) ? Arch::MIPS32EL : Arch::MIPS32)
              : Target);
      File.abiFlags();
      if (N32) {
        File.put(36, 0x80000020, 4);
        File.Bytes[0xe2] = 64;
        File.Bytes[0xe4] = 2;
      }
      File.Bytes[0xe5] = Wide ? 2 : 1;
      File.Bytes[0xe7] = 1;
      File.Bytes.resize(0x208);
      const size_t Program = File.Is64 ? 64 : 52;
      File.put(Program + (File.Is64 ? 32 : 16), File.Bytes.size(),
               File.Is64 ? 8 : 4);
      File.put(Program + (File.Is64 ? 40 : 20), File.Bytes.size(),
               File.Is64 ? 8 : 4);
      for (unsigned I = 0; I != Words.size(); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
      File.put(0x200, 0x03e00008, 4);
      File.put(0x204, 0, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      Image->Symbols.push_back(
          Symbol::makeFunc(Image->Entry, Words.size() * 4));
      Image->Symbols.push_back(Symbol::makeFunc(0x10200, 8));
      Decoder Decode;
      ASSERT_TRUE(Decode.init(*Image));
      const auto Low =
          CFGBuilder().build(*Image, Decode, Image->Entry, "saved_frame");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      auto Pointer = NdType::makePtr();
      Pointer->Size = getTargetRegInfo(Target).PointerSize;
      const auto Signature = floatingCallArguments(
          Target, BinaryFormat::ELF, NdType::makeVoid(), {Pointer});
      ASSERT_TRUE(Signature);
      const std::map<va_t, FloatingCallArguments> Calls{{0x10200, *Signature}};
      for (bool Declared : {false, true}) {
        SCOPED_TRACE(Declared);
        LowToMedConverter Convert;
        Convert.setBinaryImage(&*Image);
        if (Declared)
          Convert.setCallEntryFloatingArguments(&Calls);
        if (Variant == 0 || Variant == 1 || Variant == 8)
          EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
        else
          EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                       LowToMedConversionError);
      }
    }
}

TEST(MIPSSourceReturnTargets, LogicalZeroMovesRetainExactFrameIdentity) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Function : {37u, 38u})
      for (bool ZeroFirst : {false, true})
        for (bool UnknownRegister : {false, true}) {
          SCOPED_TRACE(getArchName(Target));
          SCOPED_TRACE(Function);
          SCOPED_TRACE(ZeroFirst);
          SCOPED_TRACE(UnknownRegister);
          const bool Wide = isMIPS64(Target);
          const auto Move = [&](unsigned To, unsigned From) {
            const unsigned Other = UnknownRegister ? 8 : 0;
            return ((ZeroFirst ? Other : From) << 21) |
                   ((ZeroFirst ? From : Other) << 16) | (To << 11) | Function;
          };
          mips_test::ELFBytes File(Target);
          const uint32_t Words[] = {Wide ? 0x67bdfff0u : 0x27bdfff8u,
                                    Wide ? 0xffbf0008u : 0xafbf0004u,
                                    Wide ? 0xffbe0000u : 0xafbe0000u,
                                    Move(30, 29),
                                    0x24020007,
                                    Move(29, 30),
                                    Wide ? 0xdfbe0000u : 0x8fbe0000u,
                                    Wide ? 0xdfbf0008u : 0x8fbf0004u,
                                    Wide ? 0x67bd0010u : 0x27bd0008u,
                                    0x03e00008,
                                    0};
          File.Bytes.resize(0x100 + sizeof(Words));
          const size_t Program = Wide ? 64 : 52;
          File.put(Program + (Wide ? 32 : 16), File.Bytes.size(), Wide ? 8 : 4);
          File.put(Program + (Wide ? 40 : 20), File.Bytes.size(), Wide ? 8 : 4);
          for (size_t I = 0; I != std::size(Words); ++I)
            File.put(0x100 + I * 4, Words[I], 4);
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(File.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          Image->MIPS->FPABI = 1;
          auto Symbol = Symbol::makeFunc(Image->Entry, 44);
          Symbol.Name = "logical_frame";
          Image->Symbols.push_back(std::move(Symbol));
          for (bool LLVM : {false, true}) {
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.OnlyFunctionEntries = {Image->Entry};
            Options.LiftMode = LLVM;
            Options.EmitDumpOutput = false;
            auto Result = Pipeline().run(*Image, Context, Options);
            EXPECT_EQ(Result.Success, !UnknownRegister) << Result.Error;
            if (!UnknownRegister && LLVM && Result.Success) {
              ASSERT_TRUE(Result.LlvmModule);
              EXPECT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            }
            if (UnknownRegister)
              EXPECT_NE(Result.Error.find("MIPS source return target"),
                        std::string::npos)
                  << Result.Error;
          }
        }
}

TEST(MIPSSourceReturnTargets, SplitFloatingSpillsKeepEveryReachingByte) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool FP64 : {false, true})
      for (unsigned Flow = 0; Flow != 3; ++Flow)
        for (unsigned Variant = 0; Variant != 7; ++Variant) {
          SCOPED_TRACE(getArchName(Target));
          SCOPED_TRACE(FP64);
          SCOPED_TRACE(Flow);
          SCOPED_TRACE(Variant);
          mips_test::ELFBytes File(Target);
          File.abiFlags();
          File.put(36, FP64 ? 0x70001200 : 0x70001000, 4);
          File.Bytes[0xe5] = FP64 ? 2 : 1;
          File.Bytes[0xe7] = FP64 ? 6 : 1;
          // GCC stages a double argument with two word stores before LDC1.
          // A whole double spill is the control. Neither a missing word nor
          // frame-derived bits may establish that the call preserves RA.
          uint32_t FirstStore = Variant == 1   ? 0xf7ac0010u // sdc1 f12,16(sp)
                                : Variant == 2 ? 0xafbd0010u // sw sp,16(sp)
                                               : 0xafa40010u;
          uint32_t SecondStore = Variant == 1 || Variant == 3 ? 0u
                                 : Variant == 4 ? 0xafa5001cu // overwrite RA
                                                : 0xafa50014u;
          if (Variant >= 5) {
            // A float call reads only the low word of the double load. The
            // uninitialized high word must not poison that proof, and swapping
            // the initialized word must fail in both byte orders.
            const bool FirstWord = archLittleEndian(Target) == (Variant == 5);
            FirstStore = FirstWord ? 0xafa40010u : 0u;
            SecondStore = FirstWord ? 0u : 0xafa50014u;
          }
          std::vector<uint32_t> Words{0x27bdffe0, 0xafbf001c, FirstStore};
          if (Flow)
            // Both predecessors must initialize every byte used by the call.
            Words.insert(Words.end(), {0x10c00004, 0, SecondStore, 0x10000002,
                                       0, Flow == 2 ? 0u : SecondStore});
          else
            Words.push_back(SecondStore);
          Words.insert(Words.end(),
                       {0xd7ac0010, 0x0c008000, 0, // ldc1; jal; nop
                        0x8fbf001c, 0x24020007, 0x03e00008, 0x27bd0020});
          File.Bytes.resize(0x100 + Words.size() * 4);
          File.put(52 + 16, File.Bytes.size(), 4);
          File.put(52 + 20, File.Bytes.size(), 4);
          for (unsigned I = 0; I != std::size(Words); ++I)
            File.put(0x100 + I * 4, Words[I], 4);
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(File.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          Decoder Decode;
          ASSERT_TRUE(Decode.init(*Image));
          const auto Low = CFGBuilder().build(*Image, Decode, Image->Entry,
                                              "split_fp_argument");
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          const auto Signature = floatingCallArguments(
              Target, BinaryFormat::ELF, NdType::makeFloat(8),
              {NdType::makeFloat(Variant >= 5 ? 4 : 8)});
          ASSERT_TRUE(Signature);
          const std::map<va_t, FloatingCallArguments> Calls{
              {0x20000, *Signature}};
          LowToMedConverter Convert;
          Convert.setBinaryImage(&*Image);
          Convert.setCallEntryFloatingArguments(&Calls);
          const bool Expected =
              Variant == 1 ||
              ((Variant == 0 || Variant == 5) &&
               (Flow != 2 || (Variant == 5 && archLittleEndian(Target))));
          if (Expected)
            EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
          else
            EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                         LowToMedConversionError);
        }
}

TEST(MIPSSourceReturnTargets, FloatingResultSpillsDoNotPoisonArgumentProofs) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool FP64 : {false, true})
      for (unsigned ResultKind = 0; ResultKind != 3; ++ResultKind)
        for (unsigned Variant = 0; Variant != 4; ++Variant) {
          SCOPED_TRACE(getArchName(Target));
          SCOPED_TRACE(FP64);
          SCOPED_TRACE(ResultKind);
          SCOPED_TRACE(Variant);
          mips_test::ELFBytes File(Target);
          File.abiFlags();
          File.put(36, FP64 ? 0x70001200 : 0x70001000, 4);
          File.Bytes[0xe5] = FP64 ? 2 : 1;
          File.Bytes[0xe7] = FP64 ? 6 : 1;
          // The incoming double is assembled from two word stores. Querying
          // its reaching values also visits the later result spill, whose
          // numerical provenance depends on this same argument proof.
          // A failed recursive query must not become a permanent negative.
          const uint32_t Spill =
              ResultKind == 1 ? (Variant == 2 ? 0xe7a00024u : 0xe7a00018u)
                              : (Variant == 2 ? 0xf7a00020u : 0xf7a00018u);
          const uint32_t Words[] = {
              0x27bdffd8, 0xafbf0024, Variant == 1 ? 0xafbd0010u : 0xafa40010u,
              0xafa50014, 0xd7ac0010, 0x0c008000,
              0,          Spill,      0x8fbf0024,
              0x24020007, 0x03e00008, 0x27bd0028};
          File.Bytes.resize(0x100 + sizeof(Words));
          File.put(52 + 16, File.Bytes.size(), 4);
          File.put(52 + 20, File.Bytes.size(), 4);
          for (unsigned I = 0; I != std::size(Words); ++I)
            File.put(0x100 + I * 4, Words[I], 4);
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(File.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          Decoder Decode;
          ASSERT_TRUE(Decode.init(*Image));
          const auto Low = CFGBuilder().build(*Image, Decode, Image->Entry,
                                              "fp_result_spill");
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          const auto Result =
              ResultKind == 0 ? NdType::makeFloat(8)
                              : NdType::makeComplex(ResultKind == 1 ? 4 : 8);
          const auto Signature = floatingCallArguments(
              Target, BinaryFormat::ELF, Result, {NdType::makeFloat(8)});
          ASSERT_TRUE(Signature);
          const std::map<va_t, FloatingCallArguments> Calls{
              {0x20000, *Signature}};
          LowToMedConverter Convert;
          Convert.setBinaryImage(&*Image);
          if (Variant != 3)
            Convert.setCallEntryFloatingArguments(&Calls);
          if (Variant == 0)
            EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
          else
            EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                         LowToMedConversionError);
        }
}

TEST_P(MIPSPipeline, FloatingCallPreservesOnlyPrivateLinkSlots) {
  for (unsigned Variant = 0; Variant < 6; ++Variant) {
    mips_test::ELFBytes File(GetParam());
    File.abiFlags();
    File.put(File.Is64 ? 48 : 36, File.Is64 ? 0xa0000000 : 0x90001000, 4);
    File.Bytes[0xe3] = 6;
    File.Bytes[0xe5] = 2;
    File.Bytes[0xe7] = File.Is64 ? 1 : 6;
    // A fixed double call with an explicitly saved RA and restored SP.
    // Variant 1 spills into callee-owned storage; 2 observes volatile F12;
    // 3 passes this frame's address to the callee; 4 has no parameters;
    // 5 forwards the result without any explicit COP1 instruction.
    const uint16_t Slot =
        Variant == 1 ? (File.Is64 ? 0xfff8 : 8) : (File.Is64 ? 24 : 28);
    const std::vector<uint32_t> Words{
        File.Is64 ? 0x67bdffe0u : 0x27bdffe0u,
        (File.Is64 ? 0xffbf0000u : 0xafbf0000u) | Slot,
        Variant == 3 ? 0x03a02025u : 0u,                       // or a0,sp,zero
        0xe8000000u | uint32_t((0x20000 - (0x1010c + 4)) / 4), // balc
        Variant == 5   ? 0u
        : Variant == 2 ? 0x462c6000u
                       : 0x46200000u, // add.d
        (File.Is64 ? 0xdfbf0000u : 0x8fbf0000u) | Slot,
        File.Is64 ? 0x67bd0020u : 0x27bd0020u,
        0x03e00009,
        0};
    File.Bytes.resize(0x100 + Words.size() * 4);
    const auto P = File.Is64 ? 64 : 52;
    File.put(P + (File.Is64 ? 32 : 16), File.Bytes.size(), File.Is64 ? 8 : 4);
    File.put(P + (File.Is64 ? 40 : 20), File.Bytes.size(), File.Is64 ? 8 : 4);
    for (size_t I = 0; I < Words.size(); ++I)
      File.put(0x100 + I * 4, Words[I], 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    Image->Symbols.push_back(Symbol::makeFunc(Image->Entry, Words.size() * 4));
    Decoder Decode;
    ASSERT_TRUE(Decode.init(*Image));
    CFGBuilder Builder;
    auto Low = Builder.build(*Image, Decode, Image->Entry, "fp_call");
    ASSERT_TRUE(Low.hasCompleteLiftCoverage());
    auto Pointer = NdType::makePtr(NdType::makeVoid());
    Pointer->Size = File.Is64 ? 8 : 4;
    std::vector<TypeRef> Parameters;
    if (Variant != 4)
      Parameters.push_back(Variant == 3 ? Pointer : NdType::makeFloat(8));
    const auto Signature = floatingCallArguments(
        GetParam(), BinaryFormat::ELF, NdType::makeFloat(8), Parameters);
    ASSERT_TRUE(Signature);
    const std::map<va_t, FloatingCallArguments> Calls{{0x20000, *Signature}};
    LowToMedConverter Convert;
    Convert.setBinaryImage(&*Image);
    if (Variant == 0) {
      // This caller reads f0 immediately after the call. A floating argument
      // does not define f0 when the declared result is in v0 or is void.
      for (const auto &Result :
           {NdType::makeVoid(), NdType::makeInt(Pointer->Size), Pointer}) {
        const auto OtherSignature = floatingCallArguments(
            GetParam(), BinaryFormat::ELF, Result, Parameters);
        ASSERT_TRUE(OtherSignature);
        const std::map<va_t, FloatingCallArguments> Other{
            {0x20000, *OtherSignature}};
        Convert.setCallEntryFloatingArguments(&Other);
        EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                     LowToMedConversionError);
      }
    }
    Convert.setCallEntryFloatingArguments(&Calls);
    if (Variant > 0 && Variant < 4)
      EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                   LowToMedConversionError);
    else {
      auto Med = Convert.convert(Low, GetParam(), BinaryFormat::ELF);
      inferMedTypes(Med, GetParam());
      ASSERT_TRUE(Med.ReturnType);
      EXPECT_EQ(Med.ReturnType->Kind, NdTypeKind::Float);
      EXPECT_EQ(Med.ReturnType->Size, 8);
      if (Variant == 5) {
        auto WrongProfile = *Image;
        WrongProfile.MIPS->FPABI = 0;
        Convert.setBinaryImage(&WrongProfile);
        EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                     LowToMedConversionError);
        Convert.setBinaryImage(&*Image);
      }
      class FloatingDebug final : public NullDebugContext {
      public:
        bool NoParameters = false;
        std::optional<FunctionSym>
        resolveFunction(va_t Address) const override {
          if (Address != 0x20000)
            return std::nullopt;
          FunctionSym F;
          F.Addr = Address;
          F.Name = "fp_callee";
          F.ReturnType = NdType::makeFloat(8);
          if (!NoParameters)
            F.Params = {{"value", NdType::makeFloat(8)}};
          F.PlatformConvention = true;
          F.PrototypeFromHeader = true;
          return F;
        }
        std::optional<FunctionSym>
        resolvePrototypeABI(llvm::StringRef Name) const override {
          return Name == "fp_callee" ? resolveFunction(0x20000) : std::nullopt;
        }
        bool hasInfo() const override { return true; }
      } Debug;
      Debug.NoParameters = Variant == 4;
      for (bool Imported : {false, true}) {
        Image->Exports.clear();
        Image->Imports.clear();
        if (Imported) {
          Import I;
          I.Name = "fp_callee";
          I.IATAddr = 0x20000;
          Image->Imports.push_back(std::move(I));
        } else {
          Export E;
          E.Name = "fp_callee";
          E.Addr = 0x20000;
          Image->Exports.push_back(std::move(E));
        }
        for (bool LLVM : {false, true}) {
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.OnlyFunctionEntries = {Image->Entry};
          Options.LiftMode = LLVM;
          Options.EmitDumpOutput = false;
          auto Result = Pipeline().run(*Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.MedFuncs.size(), 1u);
          EXPECT_EQ(Result.MedFuncs.front().ReturnType->Kind,
                    NdTypeKind::Float);
          ASSERT_EQ(Result.MedFuncs.front().CallInfos.size(), 1u);
          EXPECT_EQ(Result.MedFuncs.front().CallInfos.front().Args.size(),
                    Parameters.size());
          if (LLVM) {
            ASSERT_NE(Result.LlvmModule, nullptr);
            EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          }
        }
      }
    }
  }
}

TEST_P(MIPSPipeline, NarrowReadsRetainTheIncomingIntegerCarrier) {
  // SEB/SEH on n64 also require a sign-extended word. SB/SH read only the
  // narrow value and impose no input extension, so they exercise raw carriers.
  for (uint32_t Word : {0x7c041420u, 0x7c041620u, 0xa0a40000u, 0xa4a40000u}) {
    const bool Store = Word >> 26 >= 40;
    mips_test::ELFBytes File(GetParam());
    File.put(0x100, Word, 4);
    File.put(0x104, 0x03e00008, 4);
    File.put(0x108, 0, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Symbol = Symbol::makeFunc(Image->Entry, 12);
    Symbol.Name = "mips_narrow_read";
    Image->Symbols.push_back(std::move(Symbol));
    for (bool LLVM : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      ASSERT_EQ(Result.MedFuncs.front().Params.size(), Store ? 2u : 1u);
      const unsigned Bytes = isMIPS64(GetParam()) ? 8 : 4;
      EXPECT_EQ(Result.MedFuncs.front().Params.front().Size, Bytes);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        const auto *Body = Result.LlvmModule->getFunction("mips_narrow_read");
        ASSERT_NE(Body, nullptr);
        ASSERT_EQ(Body->arg_size(), Store ? 2u : 1u);
        EXPECT_TRUE(Body->getArg(0)->getType()->isIntegerTy(Bytes * 8));
      }
    }
  }
}

TEST_P(MIPSPipeline, AccumulatorSourceRequiresDefinedInputsAcrossEncodings) {
  for (bool Compressed : {false, true}) {
    if (Compressed && isMIPS64(GetParam()))
      continue;
    for (bool High : {false, true})
      for (bool Defined : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Compressed << "/" << High << "/" << Defined);
        mips_test::ELFBytes File(GetParam());
        std::vector<uint32_t> Words;
        if (Compressed) {
          File.put(36, 0x74001000, 4);
          File.put(24, 0x10101, 4);
          if (Defined)
            Words.push_back(0xecb8); // mult a0,a1
          Words.push_back(High ? 0xea10 : 0xea12);
          Words.push_back(0xe8a0); // jrc ra
        } else {
          if (Defined)
            Words.push_back(High ? 0x00800011 : 0x00800013); // mt[hi/lo] a0
          Words.push_back(High ? 0x00001010 : 0x00001012);
          Words.push_back(0x03e00008); // jr ra
          Words.push_back(0);
        }
        const unsigned Width = Compressed ? 2 : 4;
        for (unsigned I = 0; I != Words.size(); ++I)
          File.put(0x100 + I * Width, Words[I], Width);
        ELFLoader Loader;
        auto Image = Loader.loadBuffer(File.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        auto Symbol = Symbol::makeFunc(Image->Entry, Words.size() * Width);
        Symbol.Name = "accumulator_read";
        Image->Symbols.push_back(std::move(Symbol));
        Decoder Decode;
        ASSERT_TRUE(Decode.init(*Image));
        const auto Low = CFGBuilder().build(*Image, Decode, Image->Entry,
                                            "accumulator_read");
        ASSERT_TRUE(Low.hasCompleteLiftCoverage());
        for (bool WithImage : {false, true}) {
          LowToMedConverter Convert;
          if (WithImage)
            Convert.setBinaryImage(&*Image);
          if (Defined)
            EXPECT_NO_THROW(
                Convert.convert(Low, GetParam(), BinaryFormat::ELF));
          else
            EXPECT_THROW(Convert.convert(Low, GetParam(), BinaryFormat::ELF),
                         LowToMedConversionError);
        }
        for (bool LLVM : {false, true}) {
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.OnlyFunctionEntries = {Image->Entry};
          Options.LiftMode = LLVM;
          Options.EmitDumpOutput = false;
          const auto Result = Pipeline().run(*Image, Context, Options);
          EXPECT_EQ(Result.Success, Defined) << Result.Error;
          if (!Defined)
            EXPECT_NE(Result.Error.find("HI/LO"), std::string::npos)
                << Result.Error;
        }
      }
  }
}

TEST_P(MIPSPipeline, AccumulatorJoinsAndCallsCannotInventSourceState) {
  for (unsigned Bank = 0; Bank != 4; ++Bank)
    for (bool High : {false, true})
      for (bool Defined : {false, true})
        for (bool Call : {false, true}) {
          const uint32_t Write =
              (High ? 0x00800011 : 0x00800013) | (Bank << 11);
          const uint32_t Read = (High ? 0x00001010 : 0x00001012) | (Bank << 21);
          // The branch's taken path skips the write. A dominating write makes
          // both arms defined. Across a call only a fresh write is sufficient.
          const bool Wide = isMIPS64(GetParam());
          std::vector<uint32_t> Words =
              Call ? std::vector<uint32_t>{Wide ? 0x67bdfff0u : 0x27bdffe8u,
                                           Wide ? 0xffbf0008u : 0xafbf0014u,
                                           Write,
                                           0x0c00404c,
                                           0,
                                           Defined ? Write : 0,
                                           Read,
                                           Wide ? 0xdfbf0008u : 0x8fbf0014u,
                                           0x03e00008,
                                           Wide ? 0x67bd0010u : 0x27bd0018u}
                   : std::vector<uint32_t>{
                         Defined ? Write : 0, 0x10a00003, 0, Write,
                         0x10000002,          0,          0, Read,
                         0x03e00008,          0};
          if (Bank && Call)
            // DSP source requires an authenticated return link. Carry RA in
            // callee-saved s0; cross-call stack-link recovery is not admitted.
            Words = {Wide ? 0x67bdfff0u : 0x27bdffe8u,
                     Wide ? 0xffb00008u : 0xafb00014u,
                     0x03e08025,
                     Write,
                     0x0c00404c,
                     0,
                     Defined ? Write : 0,
                     Read,
                     0x0200f825,
                     Wide ? 0xdfb00008u : 0x8fb00014u,
                     0x03e00008,
                     Wide ? 0x67bd0010u : 0x27bd0018u};
          SCOPED_TRACE(::testing::Message()
                       << Bank << "/" << High << "/" << Defined << "/" << Call);
          mips_test::ELFBytes File(GetParam());
          if (Bank) {
            File.abiFlags();
            File.put(0xec, 1, 4);
          }
          File.Bytes.resize(0x140);
          const unsigned Program = File.Is64 ? 64 : 52;
          File.put(Program + (File.Is64 ? 32 : 16), File.Bytes.size(),
                   File.Is64 ? 8 : 4);
          File.put(Program + (File.Is64 ? 40 : 20), File.Bytes.size(),
                   File.Is64 ? 8 : 4);
          for (unsigned I = 0; I != Words.size(); ++I)
            File.put(0x100 + I * 4, Words[I], 4);
          File.put(0x130, 0x03e00008, 4);
          File.put(0x134, 0, 4);
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(File.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          Image->Symbols.push_back(
              Symbol::makeFunc(Image->Entry, Words.size() * 4));
          Image->Symbols.push_back(Symbol::makeFunc(0x10130, 8));
          Decoder Decode;
          ASSERT_TRUE(Decode.init(*Image));
          const auto Low = CFGBuilder().build(*Image, Decode, Image->Entry,
                                              "accumulator_paths");
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          for (bool WithImage : {false, true}) {
            LowToMedConverter Convert;
            if (WithImage)
              Convert.setBinaryImage(&*Image);
            const bool MissingMode = Call && !WithImage;
            if (Defined && !MissingMode)
              EXPECT_NO_THROW(
                  Convert.convert(Low, GetParam(), BinaryFormat::ELF));
            else {
              try {
                Convert.convert(Low, GetParam(), BinaryFormat::ELF);
                FAIL() << "unqualified accumulator/call source was accepted";
              } catch (const LowToMedConversionError &Error) {
                EXPECT_NE(std::string(Error.what())
                              .find(MissingMode ? "instruction mode" : "HI/LO"),
                          std::string::npos)
                    << Error.what();
              }
            }
          }
        }
}

TEST_P(MIPSPipeline, AccumulatorPhiChecksOnlyReadJoinsAndRejectsAbsentEdges) {
  for (uint64_t Register :
       {mipsreg::HI, mipsreg::LO, mipsreg::AC1HI, mipsreg::AC1LO,
        mipsreg::AC2HI, mipsreg::AC2LO, mipsreg::AC3HI, mipsreg::AC3LO}) {
    MedFunc Function;
    Function.Blocks.emplace_back();
    auto &Block = Function.Blocks.back();
    PhiNode Phi;
    Phi.Output.Kind = MedVar::Reg;
    Phi.Output.Id = 1;
    Phi.Output.SSAVer = 0;
    Phi.Output.RegOff = Register;
    Phi.Output.Size = isMIPS64(GetParam()) ? 8 : 4;
    Phi.Output.TheArch = GetParam();
    Phi.Args = {{0, MedVar::makeUnspecified(Phi.Output.Size, GetParam())},
                {1, MedVar::makeConst(7, Phi.Output.Size)}};
    Block.Phis.push_back(Phi);
    EXPECT_NO_THROW(requireMIPSSourceAccumulators(Function));
    MedOp Read;
    Read.Opcode = NdOp::COPY;
    Read.Output = Phi.Output;
    Read.Output.Id = 2;
    Read.Output.RegOff = mipsreg::V0;
    Read.addInput(Phi.Output);
    Block.Ops.push_back(Read);
    EXPECT_THROW(requireMIPSSourceAccumulators(Function),
                 LowToMedConversionError);
    Block.Phis[0].Args[0].second = MedVar::makeConst(9, Phi.Output.Size);
    EXPECT_NO_THROW(requireMIPSSourceAccumulators(Function));
  }
}

TEST_P(MIPSPipeline, ReturnSlotComputationReachesCAndLLVMWithCorrectABI) {
  mips_test::ELFBytes File(GetParam());
  File.put(0x100, 0x03e00008, 4);                          // jr ra
  File.put(0x104, File.Is64 ? 0x0085102d : 0x00851021, 4); // [d]addu v0,a0,a1
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Function = Symbol::makeFunc(Image->Entry, 8);
  Function.Name = "mips_add";
  Image->Symbols.push_back(std::move(Function));
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs[0].Params.size(), 2u);
    ASSERT_EQ(Result.FunctionAudits.size(), 1u);
    EXPECT_TRUE(Result.FunctionAudits[0].UnsupportedInstructions.empty());
    EXPECT_TRUE(Result.FunctionAudits[0].DecodeFailures.empty());
    EXPECT_TRUE(Result.FunctionAudits[0].TruncatedPaths.empty());
    EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
    if (LLVM) {
      ASSERT_NE(Result.LlvmModule, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      const auto &Layout = Result.LlvmModule->getDataLayout();
      EXPECT_EQ(Layout.isLittleEndian(), File.Little);
      EXPECT_EQ(Layout.getPointerSize(), File.Is64 ? 8u : 4u);
      const auto *Body = Result.LlvmModule->getFunction("mips_add");
      ASSERT_NE(Body, nullptr);
      EXPECT_FALSE(Body->isDeclaration());
      EXPECT_EQ(Body->arg_size(), 2u);
    } else {
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      CEmitterOptions Emit;
      Emit.TheArch = GetParam();
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
      EXPECT_NE(Text.find("mips_add"), std::string::npos) << Text;
      EXPECT_NE(Text.find("return"), std::string::npos) << Text;
    }
  }
}

TEST_P(MIPSPipeline, O32WideReturnUsesTargetWordOrderInLLVM) {
  if (isMIPS64(GetParam()))
    return;
  MedFunc Function;
  Function.Name = "mips_wide_constant";
  Function.Entry = 0x1000;
  Function.ReturnType = NdType::makeInt(8, false);
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.back();
  Block.Id = 0;
  Block.StartAddr = 0x1000;
  for (unsigned I = 0; I < 2; ++I) {
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = 0x1000 + I * 4;
    Copy.Output.Kind = MedVar::Reg;
    Copy.Output.RegOff = I ? mipsreg::V1 : mipsreg::V0;
    Copy.Output.Size = 4;
    Copy.Output.TheArch = GetParam();
    Copy.Output.Id = I + 1;
    Copy.Output.SSAVer = 1;
    Copy.addInput(MedVar::makeConst(I ? 0x55667788u : 0x11223344u, 4));
    Block.Ops.push_back(Copy);
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1008;
  Block.Ops.push_back(Return);
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  const auto Module = Emitter.emit({Function}, Context, "wide", GetParam());
  ASSERT_NE(Module, nullptr);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  auto *Body = Module->getFunction("mips_wide_constant");
  ASSERT_NE(Body, nullptr);
  // The emitter deliberately materializes mutable register slots. Promote
  // them and fold the arithmetic before comparing the observable result.
  llvm::LoopAnalysisManager Loops;
  llvm::FunctionAnalysisManager Analyses;
  llvm::CGSCCAnalysisManager SCCs;
  llvm::ModuleAnalysisManager Modules;
  llvm::PassBuilder Passes;
  Passes.registerModuleAnalyses(Modules);
  Passes.registerCGSCCAnalyses(SCCs);
  Passes.registerFunctionAnalyses(Analyses);
  Passes.registerLoopAnalyses(Loops);
  Passes.crossRegisterProxies(Loops, Analyses, SCCs, Modules);
  llvm::FunctionPassManager Optimize;
  Optimize.addPass(llvm::PromotePass());
  Optimize.addPass(llvm::InstCombinePass());
  Optimize.run(*Body, Analyses);
  const auto *Ret =
      llvm::dyn_cast<llvm::ReturnInst>(Body->back().getTerminator());
  ASSERT_NE(Ret, nullptr);
  const auto *Value = llvm::dyn_cast<llvm::ConstantInt>(Ret->getReturnValue());
  std::string IR;
  llvm::raw_string_ostream IRStream(IR);
  Module->print(IRStream, nullptr);
  ASSERT_NE(Value, nullptr) << IR;
  EXPECT_EQ(Value->getZExtValue(), archLittleEndian(GetParam())
                                       ? 0x5566778811223344ull
                                       : 0x1122334455667788ull);
}

TEST_P(MIPSPipeline, CapturedIndirectCalleeRetainsItsIncomingArguments) {
  for (bool WordWrite : {false, true}) {
    SCOPED_TRACE(WordWrite);
    mips_test::ELFBytes File(GetParam());
    File.Bytes.resize(0x148);
    const unsigned Width = File.Is64 ? 8 : 4;
    const size_t Load = File.Is64 ? 64 : 52;
    File.put(Load + (File.Is64 ? 32 : 16), File.Bytes.size(), Width);
    File.put(Load + (File.Is64 ? 40 : 20), File.Bytes.size(), Width);
    const uint32_t Words[] = {
        File.Is64 ? 0x67bdffe0u : 0x27bdffe8u, // [d]addiu sp,sp,-32/-24
        File.Is64 ? 0xffbf0018u : 0xafbf0014u, // s[d/w] ra,24/20(sp)
        0x3c190001,
        0x37390140, // t9 = 0x10140
        WordWrite ? 0x00042000u
                  : 0u, // sll a0,a0,0 truncates/sign-extends; NOP does not
        0x0320f809,
        0xafa40000, // jalr t9; slot stores a0 to frame
        File.Is64 ? 0xdfbf0018u : 0x8fbf0014u, // l[d/w] ra,24/20(sp)
        0x03e00008,                            // jr ra
        File.Is64 ? 0x67bd0020u : 0x27bd0018u};
    for (unsigned I = 0; I < std::size(Words); ++I)
      File.put(0x100 + I * 4, Words[I], 4);
    File.put(0x140, 0x03e00008, 4);
    File.put(0x144, File.Is64 ? 0x0085102d : 0x00851021, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Caller = Symbol::makeFunc(0x10100, sizeof(Words));
    Caller.Name = "captured_call";
    Image->Symbols.push_back(std::move(Caller));
    auto Callee = Symbol::makeFunc(0x10140, 8);
    Callee.Name = "mips_add";
    Image->Symbols.push_back(std::move(Callee));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      ASSERT_EQ(Result.MedFuncs[0].Params.size(), 2u);
      // The truncating write is part of the body, not an incoming extension
      // promise: inferred source parameters keep their complete carriers.
      EXPECT_EQ(Result.MedFuncs[0].Params[0].Size, Width);
      EXPECT_EQ(Result.MedFuncs[0].Params[1].Size, Width);
      ASSERT_EQ(Result.MedFuncs[0].CallInfos.size(), 1u);
      EXPECT_EQ(Result.MedFuncs[0].CallInfos[0].TargetAddr, 0x10140u);
      ASSERT_EQ(Result.MedFuncs[0].CallInfos[0].Args.size(), 2u);
      EXPECT_EQ(Result.MedFuncs[0].CallInfos[0].Args[0].Size, Width);
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const auto *Body = Result.LlvmModule->getFunction("captured_call");
        ASSERT_NE(Body, nullptr);
        EXPECT_EQ(Body->arg_size(), 2u);
        EXPECT_TRUE(Body->getArg(0)->getType()->isIntegerTy(Width * 8));
      }
    }
  }
}

TEST_P(MIPSPipeline, DynamicCopyObjectIsAnExternalDependencyInC) {
  for (unsigned Access : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(Access);
    mips_test::ELFBytes File(GetParam());
    File.runtimeRelocation(126, true); // R_MIPS_COPY
    File.put(0x100, 0x3c040001, 4);
    File.put(0x104, 0x348403c0, 4);
    File.put(0x108, 0x8c820000, 4);
    if (Access == 0) {
      File.put(0x10c, 0x03e00008, 4);
      File.put(0x110, 0, 4);
    } else if (Access == 3) {
      File.put(0x100, 0x3c050001, 4); // lui a1,1
      File.put(0x104, 0x34a503c0, 4); // ori a1,a1,0x3c0
      File.put(0x108, File.Is64 ? 0x0085202d : 0x00852021, 4);
      File.put(0x10c, 0x03e00008, 4);
      File.put(0x110, 0x8c820000, 4); // indexed lw v0,0(a0)
    } else {
      File.put(0x108, Access == 1 ? 0x8c820000 : 0x24020000, 4);
      File.put(0x10c, 0x90830001, 4); // lbu v1,1(a0)
      File.put(0x110, 0x03e00008, 4);
      File.put(0x114, 0x00431021, 4); // add v0,v0,v1
    }
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Function =
        Symbol::makeFunc(Image->Entry, Access == 1 || Access == 2 ? 24 : 20);
    Function.Name = "read_copied_object";
    Image->Symbols.push_back(std::move(Function));
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    if (Access == 3) {
      // Exercise the emitter's indexed-object contract with the COPY symbol
      // as an established address. Bare LUI/ORI immediates alone do not prove
      // address provenance for the full pipeline.
      unsigned Addresses = 0;
      auto MarkAddress = [&](auto &&Self, const ExprPtr &Expr) -> void {
        if (!Expr)
          return;
        if (Expr->Kind == ExprKind::Const && Expr->ConstVal == 0x103c0) {
          Expr->ConstProvenance = ConstantAddressProvenance::Address;
          ++Addresses;
        }
        Expr->forEachChildExpr(
            [&](const ExprPtr &Child) { Self(Self, Child); });
      };
      for (auto &High : Result.HighFuncs)
        walkStmts(High.Body, [&](HighStmt &Stmt) {
          forEachExpr(Stmt, [&](const ExprPtr &Expr) {
            MarkAddress(MarkAddress, Expr);
          });
        });
      ASSERT_GT(Addresses, 0u);
    }
    CEmitterOptions Emit;
    Emit.TheArch = GetParam();
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    if (Access) {
      EXPECT_THROW(HighCEmitter().emit(Result.HighFuncs, Output, Emit),
                   std::invalid_argument);
      continue;
    }
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
    EXPECT_NE(Text.find("extern int32_t fixture_import;"), std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("fixture_import ="), std::string::npos) << Text;
  }
}

TEST(MIPSPipeline, O32CopyPointerUsesItsExternalHeaderDeclaration) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    for (const std::string Name : {"optarg", "stderr"}) {
      SCOPED_TRACE(Name);
      mips_test::ELFBytes File(Target);
      File.runtimeRelocation(126, true);
      std::fill(File.Bytes.begin() + 0x341, File.Bytes.begin() + 0x350, 0);
      std::copy(Name.begin(), Name.end(), File.Bytes.begin() + 0x341);
      File.put(0x100, 0x3c040001, 4);
      File.put(0x104, 0x348403c0, 4);
      File.put(0x108, 0x8c820000, 4);
      File.put(0x10c, 0x03e00008, 4);
      File.put(0x110, 0, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Function = Symbol::makeFunc(Image->Entry, 20);
      Function.Name = "read_header_object";
      Image->Symbols.push_back(std::move(Function));
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions Emit;
      Emit.TheArch = Target;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
      EXPECT_NE(Text.find(Name), std::string::npos) << Text;
      EXPECT_NE(Text.find(Name == "stderr" ? "#include <stdio.h>"
                                           : "#include <unistd.h>"),
                std::string::npos)
          << Text;
      EXPECT_EQ(Text.find(Name + " ="), std::string::npos) << Text;
    }
  }
}

TEST_P(MIPSPipeline, ReadonlyAndWritableScalarsUseTheImageByteOrder) {
  for (bool Writable : {false, true}) {
    SCOPED_TRACE(Writable);
    mips_test::ELFBytes File(GetParam());
    File.Bytes.resize(0x308);
    const unsigned Width = File.Is64 ? 8 : 4;
    const size_t Head = File.Is64 ? 64 : 52;
    const size_t Data = File.Is64 ? 120 : 84;
    File.put(File.Is64 ? 56 : 44, 2, 2);
    File.put(Head + (File.Is64 ? 4 : 24), 5, 4); // RX code
    File.put(Data, 1, 4);
    File.put(Data + (File.Is64 ? 4 : 24), Writable ? 6 : 4, 4);
    File.put(Data + (File.Is64 ? 8 : 4), 0x300, Width);
    File.put(Data + (File.Is64 ? 16 : 8), 0x10300, Width);
    File.put(Data + (File.Is64 ? 32 : 16), 8, Width);
    File.put(Data + (File.Is64 ? 40 : 20), 8, Width);
    File.put(Data + (File.Is64 ? 48 : 28), 0x1000, Width);
    File.put(0x300, 0x12345678, 4);
    File.put(0x100, 0x3c040001, 4);                // lui a0,1
    File.put(0x104, 0x34840300, 4);                // ori a0,a0,0x300
    File.put(0x108, 0x8c820000, 4);                // lw v0,0(a0)
    File.put(0x10c, 0x03e00008, 4);                // jr ra
    File.put(0x110, Writable ? 0xac800000 : 0, 4); // sw zero,0(a0) / nop
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Function = Symbol::makeFunc(Image->Entry, 20);
    Function.Name = "read_image_scalar";
    Image->Symbols.push_back(std::move(Function));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        EXPECT_EQ(Result.LlvmModule->getDataLayout().isLittleEndian(),
                  File.Little);
      } else {
        CEmitterOptions Emit;
        Emit.TheArch = GetParam();
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Output(Text);
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
        EXPECT_TRUE(Text.find("305419896") != std::string::npos ||
                    Text.find("12345678") != std::string::npos)
            << Text;
        EXPECT_EQ(Text.find("2018915346"), std::string::npos) << Text;
      }
    }
  }
}

TEST_P(MIPSPipeline, MergeOnlyImageReadsBindTheOriginalByteSpan) {
  for (bool Writable : {false, true}) {
    SCOPED_TRACE(Writable);
    mips_test::ELFBytes File(GetParam());
    File.Bytes.resize(0x308);
    const unsigned Width = File.Is64 ? 8 : 4;
    const size_t Head = File.Is64 ? 64 : 52;
    const size_t Data = File.Is64 ? 120 : 84;
    File.put(File.Is64 ? 56 : 44, 2, 2);
    File.put(Head + (File.Is64 ? 4 : 24), 5, 4); // RX code
    File.put(Data, 1, 4);
    File.put(Data + (File.Is64 ? 4 : 24), Writable ? 6 : 4, 4);
    File.put(Data + (File.Is64 ? 8 : 4), 0x300, Width);
    File.put(Data + (File.Is64 ? 16 : 8), 0x10300, Width);
    File.put(Data + (File.Is64 ? 32 : 16), 8, Width);
    File.put(Data + (File.Is64 ? 40 : 20), 8, Width);
    File.put(Data + (File.Is64 ? 48 : 28), 0x1000, Width);
    File.put(0x300, 0x12345678, 4);
    File.put(0x100, 0x3c040001, 4); // lui a0,1
    File.put(0x104, 0x34840301, 4); // ori a0,a0,0x301
    File.put(0x108, 0x00001025, 4); // or v0,zero,zero
    File.put(0x10c, 0x88820000 | (File.Little ? 3 : 0), 4);
    File.put(0x110, 0x98820000 | (File.Little ? 0 : 3), 4);
    File.put(0x114, 0x03e00008, 4);
    File.put(0x118, 0, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Function = Symbol::makeFunc(Image->Entry, 28);
    Function.Name = "read_image_merge";
    Image->Symbols.push_back(std::move(Function));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        EXPECT_EQ(Result.LlvmModule->getDataLayout().isLittleEndian(),
                  File.Little);
        unsigned Bytes = 0;
        for (const auto &Function : *Result.LlvmModule)
          for (const auto &Block : Function)
            for (const auto &Instruction : Block)
              if (const auto *Read =
                      llvm::dyn_cast<llvm::LoadInst>(&Instruction))
                if (Read->isVolatile()) {
                  ++Bytes;
                  EXPECT_TRUE(llvm::isa<llvm::GlobalVariable>(
                      llvm::getUnderlyingObject(Read->getPointerOperand())));
                }
        EXPECT_GE(Bytes, 2u);
      } else {
        unsigned ScalarAddresses = 0;
        for (auto &Function : Result.HighFuncs)
          walkStmts(Function.Body, [&](HighStmt &Statement) {
            forEachExpr(Statement, [&](const ExprPtr &Root) {
              std::vector<ExprPtr> Pending{Root};
              while (!Pending.empty()) {
                const auto E = Pending.back();
                Pending.pop_back();
                if (!E)
                  continue;
                if ((E->IntrinsicId == Intrinsic::MIPSLwl ||
                     E->IntrinsicId == Intrinsic::MIPSLwr) &&
                    E->Operands[0]->Kind == ExprKind::Const) {
                  E->Operands[0]->ConstProvenance =
                      ConstantAddressProvenance::Scalar;
                  ++ScalarAddresses;
                }
                Pending.insert(Pending.end(), E->Operands.begin(),
                               E->Operands.end());
              }
            });
          });
        ASSERT_GE(ScalarAddresses, 2u);
        CEmitterOptions Emit;
        Emit.TheArch = GetParam();
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Output(Text);
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
        EXPECT_NE(Text.find("const volatile uint8_t"), std::string::npos)
            << Text;
        // Numeric value occurrences keep their spelling; actual memory
        // arguments must be projected through the collected backing.
        EXPECT_EQ(Text.find(")(66817)"), std::string::npos) << Text;
        EXPECT_EQ(Text.find(")(0x10301)"), std::string::npos) << Text;
        EXPECT_EQ(Text.find(")(66820)"), std::string::npos) << Text;
        EXPECT_EQ(Text.find(")(0x10304)"), std::string::npos) << Text;
      }
    }
  }
}

TEST_P(MIPSPipeline, MulDropsDeadAccumulatorsBeforeSourceABIRecovery) {
  mips_test::ELFBytes File(GetParam());
  File.put(0x100, 0x70851002, 4); // mul v0,a0,a1
  File.put(0x104, 0x03e00008, 4);
  File.put(0x108, 0, 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto FunctionSymbol = Symbol::makeFunc(Image->Entry, 12);
  FunctionSymbol.Name = "mips_mul";
  Image->Symbols.push_back(std::move(FunctionSymbol));
  for (bool LLVM : {false, true}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    EXPECT_EQ(Result.MedFuncs[0].Params.size(), 2u);
    for (const auto &Block : Result.MedFuncs[0].Blocks)
      for (const auto &Op : Block.Ops)
        if (!Op.Dead && Op.Opcode == NdOp::INTRINSIC)
          EXPECT_NE(Op.Inputs[0].ConstVal,
                    uint64_t(Intrinsic::MIPSUnpredictable));
    if (LLVM) {
      ASSERT_NE(Result.LlvmModule, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
    } else {
      CEmitterOptions Emit;
      Emit.TheArch = GetParam();
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
      EXPECT_EQ(Text.find("unpredictable"), std::string::npos) << Text;
    }
  }
}

TEST_P(MIPSPipeline,
       MergePairsDiscardUnobservedImplementationDependentUpperWord) {
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    mips_test::ELFBytes File(GetParam());
    File.put(0x100, 0x00a01025, 4); // or v0,a1,zero: arbitrary incoming bits
    const uint32_t Left = 0x88820000 | (File.Little ? 3 : 0);
    const uint32_t Right = 0x98820000 | (File.Little ? 0 : 3);
    File.put(0x104, Reverse ? Right : Left, 4);
    File.put(0x108, Reverse ? Left : Right, 4);
    File.put(0x10c, 0x03e00008, 4);
    File.put(0x110, 0, 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto FunctionSymbol = Symbol::makeFunc(Image->Entry, 20);
    FunctionSymbol.Name = "mips_merge_pair";
    Image->Symbols.push_back(std::move(FunctionSymbol));
    for (bool LLVM : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      EXPECT_EQ(Result.MedFuncs[0].Params.size(), 1u);
      EXPECT_FALSE(Result.MedFuncs[0].ReturnsNoValue);
      unsigned Loads = 0;
      for (const auto &Block : Result.MedFuncs[0].Blocks)
        for (const auto &Op : Block.Ops)
          if (!Op.Dead && Op.Opcode == NdOp::INTRINSIC) {
            EXPECT_NE(Op.Inputs[0].ConstVal, uint64_t(Intrinsic::MIPSLwrUpper));
            Loads += Op.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSLwl) ||
                     Op.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSLwr);
          }
      EXPECT_EQ(Loads, 2u);
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        unsigned VolatileBytes = 0;
        for (const auto &Function : *Result.LlvmModule)
          for (const auto &Block : Function)
            for (const auto &Instruction : Block)
              if (const auto *Read =
                      llvm::dyn_cast<llvm::LoadInst>(&Instruction))
                VolatileBytes +=
                    Read->isVolatile() && Read->getType()->isIntegerTy(8);
        EXPECT_GE(VolatileBytes, 2u);
      } else {
        CEmitterOptions Emit;
        Emit.TheArch = GetParam();
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Output(Text);
        EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
        EXPECT_NE(Text.find("const volatile uint8_t"), std::string::npos)
            << Text;
        EXPECT_EQ(Text.find("lwr_upper"), std::string::npos) << Text;
      }
    }
  }
}

TEST_P(MIPSPipeline, CompleteMergePairsDoNotReadAnUninitializedDestination) {
  for (unsigned Bytes : {4u, 8u}) {
    if (Bytes == 8 && !isMIPS64(GetParam()))
      continue;
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Reverse);
      mips_test::ELFBytes File(GetParam());
      const uint32_t Left = ((Bytes == 8 ? 26u : 34u) << 26) | (4u << 21) |
                            (2u << 16) | (File.Little ? Bytes - 1 : 0);
      const uint32_t Right = ((Bytes == 8 ? 27u : 38u) << 26) | (4u << 21) |
                             (2u << 16) | (File.Little ? 0 : Bytes - 1);
      File.put(0x100, Reverse ? Right : Left, 4);
      File.put(0x104, Reverse ? Left : Right, 4);
      File.put(0x108, 0x03e00008, 4);
      File.put(0x10c, 0, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto Symbol = Symbol::makeFunc(Image->Entry, 16);
      Symbol.Name = "mips_complete_pair";
      Image->Symbols.push_back(std::move(Symbol));
      for (bool LLVM : {false, true}) {
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Image->Entry};
        Options.LiftMode = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_EQ(Result.MedFuncs.size(), 1u);
        EXPECT_EQ(Result.MedFuncs[0].Params.size(), 1u);
        unsigned Loads = 0;
        for (const auto &Block : Result.MedFuncs[0].Blocks)
          for (const auto &Op : Block.Ops)
            if (!Op.Dead && Op.Opcode == NdOp::INTRINSIC &&
                mips::mergeLoad(
                    static_cast<Intrinsic>(Op.Inputs[0].ConstVal))) {
              if (!Loads)
                EXPECT_TRUE(Op.Inputs[2].isConst());
              ++Loads;
            }
        EXPECT_EQ(Loads, 2u);
        if (LLVM) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          EXPECT_EQ(
              Result.LlvmModule->getFunction("mips_complete_pair")->arg_size(),
              1u);
        } else {
          CEmitterOptions Emit;
          Emit.TheArch = GetParam();
          Emit.Format = BinaryFormat::ELF;
          Emit.Image = &*Image;
          std::string Text;
          llvm::raw_string_ostream Output(Text);
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
          EXPECT_EQ(Text.find("unknown_register"), std::string::npos) << Text;
          EXPECT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
        }
      }
    }
  }
}

TEST_P(MIPSPipeline,
       MergePairsKeepOldBitsWhenTheAddressConsumesThePartialValue) {
  mips_test::ELFBytes File(GetParam());
  File.put(0x100, 0x00801025, 4);                         // or v0,a0,zero
  File.put(0x104, 0x88420000 | (File.Little ? 3 : 0), 4); // lwl v0,off(v0)
  File.put(0x108, 0x98420000 | (File.Little ? 0 : 3), 4); // lwr v0,off(v0)
  File.put(0x10c, 0x03e00008, 4);
  File.put(0x110, 0, 4);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto FunctionSymbol = Symbol::makeFunc(Image->Entry, 20);
  FunctionSymbol.Name = "mips_merge_address_alias";
  Image->Symbols.push_back(std::move(FunctionSymbol));
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.OnlyFunctionEntries = {Image->Entry};
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(*Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  unsigned FirstLoads = 0;
  for (const auto &Block : Result.MedFuncs[0].Blocks)
    for (const auto &Op : Block.Ops)
      if (!Op.Dead && Op.Opcode == NdOp::INTRINSIC &&
          Op.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSLwl)) {
        EXPECT_FALSE(Op.Inputs[2].isConst());
        ++FirstLoads;
      }
  EXPECT_EQ(FirstLoads, 1u);
}

TEST_P(MIPSPipeline, ObservableMulAccumulatorsCannotBecomeStaleSourceValues) {
  for (bool Overwrite : {false, true}) {
    for (unsigned Read : {16u, 18u}) {
      mips_test::ELFBytes File(GetParam());
      File.put(0x100, 0x70851002, 4);                 // mul v0,a0,a1
      File.put(0x104, Overwrite ? 0x00850018 : 0, 4); // mult a0,a1
      File.put(0x108, 0x03e00008, 4);
      File.put(0x10c, 0x00001000 | Read, 4); // mfhi/mflo v0
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto FunctionSymbol = Symbol::makeFunc(Image->Entry, 16);
      FunctionSymbol.Name = "mul_accumulator";
      Image->Symbols.push_back(std::move(FunctionSymbol));
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = true;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      EXPECT_EQ(Result.Success, Overwrite) << Result.Error;
      if (!Overwrite)
        EXPECT_NE(Result.Error.find("unpredictable"), std::string::npos)
            << Result.Error;
    }
  }
}

TEST_P(MIPSPipeline,
       PartialAccumulatorHistoryMustBeProvenBeforeSourceEmission) {
  for (bool High : {false, true})
    for (unsigned Case = 0; Case != 4; ++Case) {
      // Case 0 observes an unread MULT result after a partial write; Case 1
      // consumes it via MFHI $zero; Case 2 overwrites both halves; Case 3 joins
      // the consumed/unconsumed histories and must conservatively refuse.
      std::vector<uint32_t> Words = {0x00850018};
      if (Case == 3)
        Words.insert(Words.end(), {0x10c00002, 0});
      if (Case == 1 || Case == 3)
        Words.push_back(0x00000010);
      Words.push_back(High ? 0x00800011 : 0x00800013);
      if (Case == 2)
        Words.push_back(High ? 0x00a00013 : 0x00a00011);
      Words.insert(Words.end(),
                   {High ? 0x00001012u : 0x00001010u, 0x03e00008, 0});
      mips_test::ELFBytes F(GetParam());
      for (unsigned N = 0; N < Words.size(); ++N)
        F.put(0x100 + N * 4, Words[N], 4);
      ELFLoader L;
      auto I = L.loadBuffer(F.buffer());
      ASSERT_TRUE(bool(I)) << llvm::toString(I.takeError());
      for (bool LLVM : {false, true}) {
        llvm::LLVMContext C;
        PipelineOptions O;
        O.OnlyFunctionEntries = {I->Entry};
        O.LiftMode = LLVM;
        O.EmitDumpOutput = false;
        auto R = Pipeline().run(*I, C, O);
        const bool Defined = Case == 1 || Case == 2;
        if (Defined)
          EXPECT_TRUE(R.Success) << R.Error;
        else if (LLVM || !R.Success)
          EXPECT_FALSE(R.Success);
        else {
          CEmitterOptions E;
          E.TheArch = GetParam();
          E.Format = BinaryFormat::ELF;
          E.Image = &*I;
          std::string Text;
          llvm::raw_string_ostream Out(Text);
          EXPECT_THROW(HighCEmitter().emit(R.HighFuncs, Out, E),
                       std::runtime_error);
        }
      }
    }
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSPipeline,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
