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
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

using namespace neverd;

namespace {

class MIPS16Pipeline : public ::testing::TestWithParam<Arch> {};

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

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16Pipeline,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
