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
#include "neverd/ir/intrinsics/Intrinsics.h"
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
      EXPECT_EQ(Result.MedFuncs[0].Params[0].Size, WordWrite ? 4u : Width);
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
        EXPECT_TRUE(Body->getArg(0)->getType()->isIntegerTy(
            WordWrite ? 32 : Width * 8));
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

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSPipeline,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
