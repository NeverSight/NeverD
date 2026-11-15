//===- MIPS16PipelineTests.cpp - MIPS16 source mode contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check ELF-to-source scalar and control paths, including explicit exclusion
/// of the alternate compressed ISA on definitions and declarations.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/MIPS/MIPSInstructionModeSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

using namespace neverd;

namespace {

class MIPS16Pipeline : public ::testing::TestWithParam<Arch> {};

TEST(MIPS16PipelineSource, SelectedClassicCallerRetainsCompressedDeclaration) {
  llvm::LLVMContext Context;
  llvm::Module Module("mixed", Context);
  llvm::IRBuilder<> IR(Context);
  auto *Type = llvm::FunctionType::get(IR.getInt32Ty(), false);
  auto *Compressed = llvm::Function::Create(
      Type, llvm::Function::ExternalLinkage, "compressed_target", Module);
  mips::applyInstructionModeLLVMAttributes(*Compressed,
                                           InstructionMode::MIPS16);
  IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Compressed));
  IR.CreateRet(IR.getInt32(7));
  CEmitterOptions Options;
  Options.TheArch = Arch::MIPS32;
  Options.Format = BinaryFormat::ELF;
  for (bool CallsCompressed : {false, true}) {
    auto *Classic = llvm::Function::Create(
        Type, llvm::Function::ExternalLinkage,
        CallsCompressed ? "calls_compressed" : "leaf", Module);
    IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Classic));
    IR.CreateRet(CallsCompressed ? IR.CreateCall(Compressed)
                                 : static_cast<llvm::Value *>(IR.getInt32(42)));
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(
        LLVMCEmitter().emit(Module, Out, Options, nullptr, nullptr, Classic));
    EXPECT_EQ(Text.find(mips::MIPS16ModeCPrelude) != std::string::npos,
              CallsCompressed)
        << Text;
  }
}

TEST_P(MIPS16Pipeline, DeclaredGPRFloatKeepsItsIntegerBitUse) {
  for (bool Compressed : {false, true}) {
    SCOPED_TRACE(Compressed);
    mips_test::ELFBytes File(GetParam());
    File.abiFlags();
    File.Bytes[0xe5] = 1; // FR=0 hard float.
    File.Bytes[0xe7] = 1;
    if (Compressed) {
      File.put(36, 0x74001000, 4);
      File.put(24, 0x10101, 4);
      File.put(0xec, 0x400, 4); // AFL_ASE_MIPS16.
      // xor a0,a1; move v0,a0; jrc ra (Clang 22 MIPS16).
      const uint16_t Words[] = {0xecae, 0x6744, 0xe8a0};
      for (unsigned I = 0; I < std::size(Words); ++I)
        File.put(0x100 + I * 2, Words[I], 2);
    } else {
      // xor v0,a0,a1; jr ra; nop.
      const uint32_t Words[] = {0x00851026, 0x03e00008, 0};
      for (unsigned I = 0; I < std::size(Words); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
    }
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Symbol = Symbol::makeFunc(Image->Entry, Compressed ? 6 : 12);
    Symbol.Name = "after_int";
    Image->Symbols.push_back(std::move(Symbol));
    class FloatDebug final : public NullDebugContext {
    public:
      va_t Entry = 0;
      std::optional<FunctionSym> resolveFunction(va_t Address) const override {
        if (Address != Entry)
          return std::nullopt;
        FunctionSym F;
        F.Name = "after_int";
        F.Addr = Address;
        F.ReturnType = NdType::makeInt(4, false);
        // o32 passes floating arguments after an integer through GPRs.
        F.Params = {{"mask", NdType::makeInt(4, false)},
                    {"value", NdType::makeFloat(4)}};
        return F;
      }
      bool hasInfo() const override { return true; }
    } Debug;
    Debug.Entry = Image->Entry;
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.HighFuncs.size(), 1u);
    CEmitterOptions Emit;
    Emit.TheArch = GetParam();
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit, &Debug));
    EXPECT_NE(Text.find("uint32_t mask, float value"), std::string::npos)
        << Text;
    EXPECT_NE(Text.find("__builtin_bit_cast(int32_t, value)"),
              std::string::npos)
        << Text;
  }
}

TEST_P(MIPS16Pipeline, ExtendAndScalarLeavesReachBothSourcesWithExactMode) {
  for (bool Extended : {false, true}) {
    mips_test::ELFBytes File(GetParam());
    File.put(36, 0x74001000, 4);
    File.put(24, 0x10103, 4);
    const auto Words =
        Extended ? std::vector<uint16_t>{0xf010, 0x6a00, 0xe289, 0xe8a0}
                 : std::vector<uint16_t>{0x6544, 0x4a01, 0xe8a0};
    for (unsigned I = 0; I < Words.size(); ++I)
      File.put(0x102 + I * 2, Words[I], 2);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Symbol = Symbol::makeFunc(Image->Entry, Words.size() * 2);
    Symbol.Name = "mips16_leaf";
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
      EXPECT_EQ(Result.MedFuncs.front().Params.size(), 1u);
      EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const auto *Body = Result.LlvmModule->getFunction("mips16_leaf");
        ASSERT_NE(Body, nullptr);
        EXPECT_TRUE(Body->hasFnAttribute("mips16"));
        EXPECT_TRUE(Body->hasFnAttribute("nomicromips"));
        EXPECT_EQ(mips::instructionModeFromLLVM(*Body),
                  InstructionMode::MIPS16);
      } else {
        CEmitterOptions Emit;
        Emit.TheArch = GetParam();
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Output(Text);
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
        EXPECT_NE(Text.find("__attribute__((mips16,nomicromips))"),
                  std::string::npos)
            << Text;
      }
    }
    EXPECT_THROW(mips::requireNumericCodePointerSource(&*Image, Image->Entry),
                 std::invalid_argument);
  }
}

TEST_P(MIPS16Pipeline, FrameDivisionAndDeadResultsReachQualifiedSources) {
  for (unsigned Case = 0; Case < 3; ++Case) {
    mips_test::ELFBytes File(GetParam());
    File.put(36, 0x74001000, 4);
    File.put(24, 0x10103, 4);
    // Case 1 has a fully dead divide-by-zero result. Case 2 checks zero after
    // DIV; source qualification still applies to its retained executed divide.
    const std::vector<uint16_t> Words =
        Case == 0
            ? std::vector<uint16_t>{0x64c4, 0xecba, 0xea12, 0x6444, 0xe8a0}
        : Case == 1 ? std::vector<uint16_t>{0x6d00, 0xecba, 0x6a07, 0xe8a0}
                    : std::vector<uint16_t>{0xecba, 0x2502, 0xea12,
                                            0xe8a0, 0x6a2a, 0xe8a0};
    for (unsigned I = 0; I < Words.size(); ++I)
      File.put(0x102 + I * 2, Words[I], 2);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Sym = Symbol::makeFunc(Image->Entry, Words.size() * 2);
    Sym.Name = "mips16_division";
    Image->Symbols.push_back(std::move(Sym));
    for (bool LLVM : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        bool Divide = false, Trap = false;
        for (const auto &F : *Result.LlvmModule)
          for (const auto &B : F)
            for (const auto &I : B) {
              Divide |= I.getOpcode() == llvm::Instruction::UDiv ||
                        I.getOpcode() == llvm::Instruction::URem;
              if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I))
                if (const auto *Callee = Call->getCalledFunction())
                  Trap |= Callee->getName() == "llvm.trap";
            }
        EXPECT_EQ(Divide, Case != 1);
        EXPECT_EQ(Trap, Case != 1);
      } else {
        CEmitterOptions Emit;
        Emit.TheArch = GetParam();
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit));
        EXPECT_EQ(Text.find("neverd_mips_div_word") != std::string::npos,
                  Case != 1)
            << Text;
        EXPECT_EQ(Text.find("requires a nonzero divisor") != std::string::npos,
                  Case != 1)
            << Text;
        if (Case != 1)
          EXPECT_NE(Text.find(std::string(mips::instructionModeCAttribute(
                                  InstructionMode::MIPSClassic)) +
                              "uint64_t neverd_mips_div_word"),
                    std::string::npos)
              << Text;
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16Pipeline,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
