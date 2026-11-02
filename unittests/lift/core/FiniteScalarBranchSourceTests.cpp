//===- FiniteScalarBranchSourceTests.cpp - Arithmetic dispatch C ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute both C routes through every arithmetic dispatch destination.

#include "FiniteScalarBranchTestImage.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <string>

namespace {
using namespace neverd;

TEST(FiniteScalarBranchSource, AllCasesExecuteInBothRoutesAndFormats) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (unsigned Form = 0; Form != 3; ++Form) {
      const bool Reverse = Form == 1, Joined = Form == 2;
      if (Reverse && Architecture == Arch::AArch64)
        continue;
      for (auto Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(Architecture) << ':' << unsigned(Format)
                         << ':' << LLVM << ':' << Form << ':' << NoOpt);
            auto Image = Joined ? finite_scalar_branch_test::joinedImage(
                                      Architecture, Format)
                                : finite_scalar_branch_test::image(
                                      Architecture, Format, Reverse);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.SourceProjection = true;
            Options.LiftMode = LLVM;
            Options.NoOpt = NoOpt;
            Options.OnlyFunctionEntries = {Image.Entry};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            std::string Source = "#include <stdint.h>\n";
            llvm::raw_string_ostream Output(Source);
            CEmitterOptions COptions;
            COptions.EmitIncludes = false;
            COptions.TheArch = Architecture;
            COptions.Format = Format;
            COptions.Image = &Image;
            if (LLVM) {
              ASSERT_TRUE(Result.LlvmModule);
              ASSERT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
            } else
              ASSERT_TRUE(
                  HighCEmitter().emit(Result.HighFuncs, Output, COptions));
            ASSERT_NE(Source.find("finite_dispatch_probe("), std::string::npos)
                << Source;
            // Unknown switch values may trap, but no raw address is executed.
            ASSERT_EQ(Source.find("(*)"), std::string::npos) << Source;
            const std::string Main =
                std::string("\n#define REVERSE ") + (Reverse ? "1\n" : "0\n") +
                "#define JOINED " + (Joined ? "1\n" : "0\n") + R"C(
int main(void) {
    const uint32_t expected[] = {17, 29, 43, 61};
    for (uint32_t i = 0; i != 256; ++i) {
        const uint32_t values[] = {i, 0x80000000u + i, 0xffffffffu - i};
        for (unsigned j = 0; j != 3; ++j) {
            const unsigned index = values[j] & 3;
            if ((uint32_t)finite_dispatch_probe(values[j]) !=
                expected[JOINED ? (values[j] == 0) : REVERSE ? 3 - index : index])
                return 1;
        }
    }
    return 0;
}
)C";
            for (const char *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source + Main, {Optimization, "-fsanitize=undefined",
                                  "-fno-sanitize-recover=all"});
          }
    }
}

TEST(FiniteScalarBranchSource, ObservableCodeAddressesKeepTheirOwnIdentity) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << unsigned(Format));
      auto Image = finite_scalar_branch_test::joinedImage(Architecture, Format);
      auto &Bytes = Image.Segments.front().Data;
      if (Architecture == Arch::AArch64) {
        writeLE<uint32_t>(Bytes.data() + 0x20, 0xf9000030); // str x16,[x1]
        writeLE<uint32_t>(Bytes.data() + 0x24, 0xd61f0200); // br x16
      } else {
        const uint8_t ModRM = Format == BinaryFormat::COFF ? 0x1a : 0x1e;
        const uint8_t Code[] = {0x4c, 0x89, ModRM, 0x41, 0xff, 0xe3};
        std::copy(std::begin(Code), std::end(Code), Bytes.begin() + 0x20);
      }
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.SourceProjection = true;
      Options.LiftMode = true;
      Options.NoOpt = true;
      Options.OnlyFunctionEntries = {Image.Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      const auto &Function = Result.MedFuncs.front();
      ASSERT_EQ(Function.SwitchSelectorPlans.size(), 1u);
      const auto &Selector =
          Function.SwitchSelectorPlans.begin()->second.Selector;
      unsigned NumericTargets = 0, AddressTargets = 0;
      for (const auto &Block : Function.Blocks) {
        for (const auto &Phi : Block.Phis)
          for (const auto &[Pred, Value] : Phi.Args)
            if (Value.isConst() &&
                (Value.ConstVal == 0x1080 || Value.ConstVal == 0x1088)) {
              if (Value.Provenance == ConstantAddressProvenance::Scalar)
                ++NumericTargets;
              else if (isExactAddressProvenance(Value.Provenance))
                ++AddressTargets;
            }
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2)
            EXPECT_NE(Op.Inputs[1], Selector);
      }
      EXPECT_EQ(NumericTargets, 2u);
      EXPECT_EQ(AddressTargets, 2u);
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      std::string IR;
      llvm::raw_string_ostream Output(IR);
      Result.LlvmModule->print(Output, nullptr);
      EXPECT_NE(IR.find("blockaddress("), std::string::npos) << IR;
    }
}

TEST(FiniteScalarBranchSource, SkippedSSACannotPublishANumericSelectorView) {
  auto Image =
      finite_scalar_branch_test::joinedImage(Arch::X64, BinaryFormat::ELF);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Image.Entry};
  auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  LowFunc Low = Result.LowFuncs.front();
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  Low.DecodedInstructionCount = limits::kMaxSSAFunctionOps + 1;
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  const auto Function = Converter.convert(Low, Image.Arch, Image.Format);
  EXPECT_TRUE(Function.SkippedSSA);
  EXPECT_TRUE(Function.SwitchSelectorPlans.empty());
}
} // namespace
