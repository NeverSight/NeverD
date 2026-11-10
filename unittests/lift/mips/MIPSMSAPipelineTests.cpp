//===- MIPSMSAPipelineTests.cpp - Scalar source qualification for MSA ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise both source routes and reject unproved vector entry, return and
/// callee-state contracts before ordinary optimizations can erase evidence.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/MIPS/MIPSSourceCompiler.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MIPS/MIPSSourceVectors.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdlib>
#include <vector>

using namespace neverd;
namespace {

mips_test::ELFBytes msaFile(Arch Target, llvm::ArrayRef<uint32_t> Words) {
  mips_test::ELFBytes File(Target);
  File.Bytes.resize(0x200);
  File.put(52 + 16, File.Bytes.size(), 4);
  File.put(52 + 20, File.Bytes.size(), 4);
  File.abiFlags();
  File.Bytes[0xe3] = 5;
  File.Bytes[0xe5] = 3;
  File.Bytes[0xe7] = 6;
  File.put(0xec, 0x200, 4);
  for (unsigned I = 0; I < Words.size(); ++I)
    File.put(0x100 + I * 4, Words[I], 4);
  return File;
}

TEST(MIPSMSAPipeline, InitializedIntegerLeavesEmitBothSourceRoutes) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool LLVM : {false, true}) {
      const uint32_t Words[] = {0x7b02201e, 0x79312819, 0x7840004e,
                                0x78b10899, 0x03e00008, 0};
      auto File = msaFile(Target, Words);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      ASSERT_TRUE(Image->MIPS->HasMSA);
      EXPECT_EQ(Image->MIPS->ISARevision, 5);
      auto Symbol = Symbol::makeFunc(Image->Entry, sizeof(Words));
      Symbol.Name = "msa_leaf";
      Image->Symbols.push_back(std::move(Symbol));
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
      CEmitterOptions Emit;
      Emit.TheArch = Target;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Image;
      std::string Text;
      llvm::raw_string_ostream Stream(Text);
      if (LLVM) {
        ASSERT_NE(Result.LlvmModule, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const auto *Body = Result.LlvmModule->getFunction("msa_leaf");
        ASSERT_NE(Body, nullptr);
        EXPECT_EQ(Body->hasFnAttribute(mips::NoMSAMarker),
                  Target == Arch::MIPS32);
        if (Target == Arch::MIPS32)
          EXPECT_EQ(Body->getFnAttribute("target-features").getValueAsString(),
                    "-msa");
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Stream, Emit,
                                        nullptr, &*Image));
      } else {
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, Emit));
      }
      EXPECT_NE(Text.find("msa_leaf"), std::string::npos) << Text;
      EXPECT_EQ(Text.find(mips::NoMSACBegin) != std::string::npos,
                Target == Arch::MIPS32)
          << Text;
      EXPECT_EQ(Text.find(mips::NoMSACAttribute) != std::string::npos,
                Target == Arch::MIPS32)
          << Text;
    }
}

TEST(MIPSMSAPipeline, MissingImageCannotBypassVectorAndReturnQualification) {
  const std::vector<std::pair<bool, std::vector<uint32_t>>> Cases = {
      {true, {0x7b02201e, 0x78b00099, 0x03e00008, 0}},
      {false, {0x78b00099, 0x03e00008, 0}},             // unknown W0
      {false, {0x7b02251e, 0x78b0a099, 0x03e00008, 0}}, // saved F20 low view
      {false, {0x7b02201e, 0x78b007d9, 0x03e00008, 0}}, // changed RA
      {true, {0x7b02201e, 0x24020007, 0x03e00008, 0x78b007d9}},
      {false, {0x7b02201e, 0x0c008000, 0, 0x03e00008, 0}}};
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (const auto &[Admitted, Words] : Cases) {
      auto File = msaFile(Target, Words);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      Image->Symbols.push_back(
          Symbol::makeFunc(Image->Entry, Words.size() * 4));
      Decoder Decode;
      ASSERT_TRUE(Decode.init(*Image));
      const auto Low =
          CFGBuilder().build(*Image, Decode, Image->Entry, "msa_gate");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      for (bool WithImage : {false, true}) {
        LowToMedConverter Convert;
        if (WithImage)
          Convert.setBinaryImage(&*Image);
        if (Admitted)
          EXPECT_NO_THROW(Convert.convert(Low, Target, BinaryFormat::ELF));
        else
          EXPECT_THROW(Convert.convert(Low, Target, BinaryFormat::ELF),
                       LowToMedConversionError);
      }
    }
}

TEST(MIPSMSAPipeline, StandaloneArchitectureAndFPABIHaveDistinctSaveViews) {
  for (Arch Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Reg : {19u, 20u, 21u, 31u}) {
      LowFunc Function;
      Function.Blocks.emplace_back();
      LowOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = NdVar::reg(mipsreg::w(Reg), 16);
      Write.Inputs[0] = NdVar::cst(0, 16);
      Write.NumInputs = 1;
      Function.Blocks[0].Ops.push_back(Write);
      if (!isMIPS64(Target) && Reg < 20)
        EXPECT_NO_THROW(requireMIPSSourceVectors(Function, Target, nullptr));
      else
        EXPECT_THROW(requireMIPSSourceVectors(Function, Target, nullptr),
                     LowToMedConversionError);
      BinaryImage Image;
      Image.Arch = Target;
      EXPECT_THROW(requireMIPSSourceVectors(Function, Target, &Image),
                   LowToMedConversionError);
      Image.MIPS.emplace();
      Image.MIPS->ISARevision = 5;
      Image.MIPS->HasMSA = true;
      Image.MIPS->FPABI = 6;
      if (!isMIPS64(Target) && Reg != 20)
        EXPECT_NO_THROW(requireMIPSSourceVectors(Function, Target, &Image));
      else
        EXPECT_THROW(requireMIPSSourceVectors(Function, Target, &Image),
                     LowToMedConversionError);
      Image.MIPS->FPABI = 3;
      EXPECT_THROW(requireMIPSSourceVectors(Function, Target, &Image),
                   LowToMedConversionError);
    }
}

TEST(MIPSMSAPipeline, LLVMCWideIntegerCarrier) {
  // Unlike the four static-lane MSA forms, a variable 128-bit funnel shift
  // survives lane projection. The source runner recompiles this exact output
  // and checks it with an independent arbitrary-precision integer oracle.
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    llvm::LLVMContext Context;
    llvm::Module Module("mips-wide-carrier", Context);
    Module.setTargetTriple(llvm::Triple(getTriple(Target, BinaryFormat::ELF)));
    Module.setDataLayout(archLittleEndian(Target) ? "e-p:32:32-i64:64-n32-S64"
                                                  : "E-p:32:32-i64:64-n32-S64");
    auto *I32 = llvm::Type::getInt32Ty(Context);
    auto *I128 = llvm::Type::getInt128Ty(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(I32, {I32, I32, I32}, false),
        llvm::GlobalValue::ExternalLinkage, "msa_i128", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    const auto X = Builder.CreateOr(
        Builder.CreateZExt(Function->getArg(0), I128),
        Builder.CreateShl(Builder.CreateZExt(Function->getArg(1), I128), 65));
    const auto Y = Builder.CreateXor(
        X,
        llvm::ConstantInt::get(
            Context, llvm::APInt(128, "112233445566778899aabbccddeeff00", 16)));
    const auto Shift =
        Builder.CreateZExt(Builder.CreateAnd(Function->getArg(2), 127), I128);
    const auto Result =
        Builder.CreateIntrinsic(llvm::Intrinsic::fshr, {I128}, {X, Y, Shift});
    Builder.CreateRet(Builder.CreateTrunc(Result, I32));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    CEmitterOptions Options;
    Options.TheArch = Target;
    Options.Format = BinaryFormat::ELF;
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options));
    // No image can rule out externally enabled DSP/MSA. The one attribute
    // must carry both constraints, including for compiler-generated helpers.
    EXPECT_NE(Text.find(Target == Arch::MIPS32 ? mips::DSPNoMSACBegin
                                               : mips::DSPSourceCBegin),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("_BitInt(128)"), std::string::npos) << Text;
    EXPECT_NE(Text.find("__uint128_t"), std::string::npos) << Text;
    if (const char *Directory = std::getenv("NEVERD_MIPS_CARRIER_OUTPUT")) {
      llvm::SmallString<256> Path(Directory);
      llvm::sys::path::append(Path, archLittleEndian(Target) ? "mipsel-wide.c"
                                                             : "mips-wide.c");
      std::error_code Error;
      llvm::raw_fd_ostream Output(Path, Error);
      ASSERT_FALSE(Error);
      Output << Text;
    }
  }
}

TEST(MIPSMSAPipeline, LLVMCWideStorageAndDeclarations) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Declaration : {false, true}) {
      llvm::LLVMContext Context;
      llvm::Module Module("mips-wide-declaration", Context);
      Module.setTargetTriple(
          llvm::Triple(getTriple(Target, BinaryFormat::ELF)));
      Module.setDataLayout(archLittleEndian(Target)
                               ? "e-p:32:32-i64:64-n32-S64"
                               : "E-p:32:32-i64:64-n32-S64");
      auto *Pointer = llvm::PointerType::get(Context, 0);
      auto *I32 = llvm::Type::getInt32Ty(Context);
      auto *I128 = llvm::Type::getInt128Ty(Context);
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(
              Declaration ? static_cast<llvm::Type *>(Pointer) : I32, false),
          llvm::GlobalValue::ExternalLinkage, "selected", Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      if (Declaration) {
        auto *Wide = llvm::Function::Create(
            llvm::FunctionType::get(I128, false),
            llvm::GlobalValue::ExternalLinkage, "external_wide", Module);
        Builder.CreateRet(Wide);
      } else {
        auto *Touch = llvm::Function::Create(
            llvm::FunctionType::get(Builder.getVoidTy(), {Pointer}, false),
            llvm::GlobalValue::ExternalLinkage, "touch", Module);
        auto *Storage = Builder.CreateAlloca(I128);
        Builder.CreateCall(Touch, {Storage});
        Builder.CreateRet(Builder.CreateZExt(
            Builder.CreateLoad(Builder.getInt8Ty(), Storage), I32));
      }
      ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
      CEmitterOptions Options;
      Options.TheArch = Target;
      Options.Format = BinaryFormat::ELF;
      std::string Text;
      llvm::raw_string_ostream Stream(Text);
      ASSERT_TRUE(LLVMCEmitter().emit(Module, Stream, Options, nullptr, nullptr,
                                      Function));
      EXPECT_NE(Text.find("_BitInt(128)"), std::string::npos) << Text;
      if (const char *Directory = std::getenv("NEVERD_MIPS_CARRIER_OUTPUT")) {
        llvm::SmallString<256> Path(Directory);
        const std::string Name =
            std::string(getArchName(Target)) +
            (Declaration ? "-declaration.c" : "-storage.c");
        llvm::sys::path::append(Path, Name);
        std::error_code Error;
        llvm::raw_fd_ostream Output(Path, Error);
        ASSERT_FALSE(Error);
        Output << Text;
      }
    }
}

} // namespace
