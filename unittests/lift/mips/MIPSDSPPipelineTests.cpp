//===- MIPSDSPPipelineTests.cpp - DSP state in source backends -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// DSPControl effects survive SSA, DCE and both source projections without
/// inventing a register parameter or a local initial state.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/MIPS/MIPSDSPSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <vector>

using namespace neverd;

namespace {

TEST(MIPSDSPPipeline, DiscardedPackedResultStillPublishesMachineState) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool LLVM : {false, true})
      for (bool Initialize : {false, true}) {
        SCOPED_TRACE(int(Target));
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Initialize);
        mips_test::ELFBytes File(Target);
        File.abiFlags();
        File.put(0xec, 1, 4);
        // Optional full initialization from a0; add bytes a0,a1 to zero;
        // return a scalar constant without ever reading the updated state.
        const uint32_t Words[] = {Initialize ? 0x7c81fcf8u : 0u, 0x7c850010,
                                  0x03e00008, 0x24020007};
        for (unsigned I = 0; I != std::size(Words); ++I)
          File.put(0x100 + I * 4, Words[I], 4);
        ELFLoader Loader;
        auto Image = Loader.loadBuffer(File.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        auto FunctionSymbol = Symbol::makeFunc(Image->Entry, sizeof(Words));
        FunctionSymbol.Name = "dsp_state_only";
        Image->Symbols.push_back(std::move(FunctionSymbol));
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {Image->Entry};
        Options.LiftMode = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_EQ(Result.MedFuncs.size(), 1u);
        EXPECT_EQ(Result.MedFuncs[0].Params.size(), 2u);
        EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
        ASSERT_EQ(Result.FunctionAudits.size(), 1u);
        EXPECT_TRUE(Result.FunctionAudits[0].UnsupportedInstructions.empty());
        EXPECT_TRUE(Result.FunctionAudits[0].DecodeFailures.empty());
        unsigned Reads = 0, Writes = 0;
        for (const auto &Block : Result.MedFuncs[0].Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
                Op.Inputs[0].isConst()) {
              Reads += Op.Inputs[0].ConstVal ==
                       unsigned(Intrinsic::MIPSReadDSPControl);
              Writes += Op.Inputs[0].ConstVal ==
                        unsigned(Intrinsic::MIPSWriteDSPControl);
            }
        EXPECT_EQ(Reads, 1u);
        EXPECT_EQ(Writes, Initialize ? 2u : 1u);
        CEmitterOptions Emit;
        Emit.TheArch = Target;
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Output(Text);
        if (LLVM) {
          ASSERT_NE(Result.LlvmModule, nullptr);
          EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          const auto *Body = Result.LlvmModule->getFunction("dsp_state_only");
          ASSERT_NE(Body, nullptr);
          EXPECT_EQ(Body->getFnAttribute("target-features").getValueAsString(),
                    "-dsp,-dspr2");
          unsigned AsmReads = 0, AsmWrites = 0;
          for (const auto &Function : *Result.LlvmModule)
            for (const auto &Block : Function)
              for (const auto &Instruction : Block)
                if (const auto *Call =
                        llvm::dyn_cast<llvm::CallBase>(&Instruction))
                  if (const auto Write = mips::dspControlSourceWrite(*Call)) {
                    AsmWrites += *Write;
                    AsmReads += !*Write;
                  }
          EXPECT_EQ(AsmReads, 1u);
          EXPECT_EQ(AsmWrites, Initialize ? 2u : 1u);
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, Emit,
                                          nullptr, &*Image));
        } else {
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
        }
        EXPECT_NE(Text.find("rddsp"), std::string::npos) << Text;
        EXPECT_NE(Text.find("wrdsp"), std::string::npos) << Text;
        EXPECT_NE(Text.find("__asm__ volatile"), std::string::npos) << Text;
        EXPECT_NE(Text.find("target(\"no-dsp,no-dspr2\")"), std::string::npos)
            << Text;
      }
}

TEST(MIPSDSPPipeline, SourceReturnRequiresCapturedOrRestoredEntryLink) {
  const std::vector<std::pair<bool, std::vector<uint32_t>>> Cases = {
      {false, {0x7c3ffcb8, 0x03e00008, 0x24020007}}, // rddsp ra before jr
      {false, {0x7c00fcb8, 0x03e00008, 0x24020007}}, // rddsp ra,0
      {false, {0x7c85f810, 0x03e00008, 0x24020007}}, // addu.qb ra,a0,a1
      {true, {0x24020007, 0x03e00008, 0x7c3ffcb8}},  // captured before slot
      {true,
       {0x27bdfff0, 0xafbf000c, 0x7c81fcf8, 0x7c3ffcb8, 0x8fbf000c, 0x24020007,
        0x03e00008, 0x27bd0010}}};
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (const auto &[Admitted, Words] : Cases) {
      mips_test::ELFBytes File(Target);
      File.abiFlags();
      File.put(0xec, 1, 4);
      for (unsigned I = 0; I < Words.size(); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      Decoder Decode;
      ASSERT_TRUE(Decode.init(*Image));
      Image->Symbols.push_back(
          Symbol::makeFunc(Image->Entry, Words.size() * 4));
      CFGBuilder Builder;
      const auto Low = Builder.build(*Image, Decode, Image->Entry, "dsp_link");
      ASSERT_TRUE(Low.hasCompleteLiftCoverage());
      for (bool WithImage : {false, true}) {
        SCOPED_TRACE(WithImage);
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

TEST(MIPSDSPPipeline,
     PlainFunctionsInDSPImagesAlsoDisableImplicitStateChanges) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool LLVM : {false, true}) {
      mips_test::ELFBytes File(Target);
      File.abiFlags();
      File.put(0xec, 1, 4);
      File.put(0x100, 0x00851021, 4); // ordinary addu v0,a0,a1
      File.put(0x104, 0x03e00008, 4);
      File.put(0x108, 0, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      auto FunctionSymbol = Symbol::makeFunc(Image->Entry, 12);
      FunctionSymbol.Name = "ordinary_sum";
      Image->Symbols.push_back(std::move(FunctionSymbol));
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries = {Image->Entry};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions Emit;
      Emit.TheArch = Target;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        const auto *Body = Result.LlvmModule->getFunction("ordinary_sum");
        ASSERT_NE(Body, nullptr);
        EXPECT_EQ(Body->getFnAttribute("target-features").getValueAsString(),
                  "-dsp,-dspr2");
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, Emit,
                                        nullptr, &*Image));
      } else {
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
      }
      EXPECT_NE(Text.find("target(\"no-dsp,no-dspr2\")"), std::string::npos)
          << Text;
    }
}

TEST(MIPSDSPPipeline, StandaloneModuleAndShardsProtectOrdinarySiblings) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (bool Reverse : {false, true})
      for (bool MaskStateBody : {false, true}) {
        std::vector<MedFunc> Functions(2);
        for (unsigned I = 0; I < Functions.size(); ++I) {
          auto &Function = Functions[I];
          Function.Name = I ? "ordinary_sibling" : "read_state";
          Function.Entry = 0x1000 + I * 0x100;
          Function.ReturnType = NdType::makeInt(4, false);
          auto &Block = Function.Blocks.emplace_back();
          Block.Id = 0;
          Block.StartAddr = Function.Entry;
          MedOp Value;
          Value.Opcode = I ? NdOp::COPY : NdOp::INTRINSIC;
          Value.Addr = Function.Entry;
          Value.Output.Kind = MedVar::Reg;
          Value.Output.RegOff = mipsreg::V0;
          Value.Output.Size = 4;
          Value.Output.TheArch = Target;
          Value.Output.Id = 1;
          Value.Output.SSAVer = 1;
          Value.addInput(MedVar::makeConst(
              I ? 7 : uint64_t(Intrinsic::MIPSReadDSPControl), I ? 4 : 2));
          Block.Ops.push_back(Value);
          MedOp Return;
          Return.Opcode = NdOp::RETURN;
          Return.Addr = Function.Entry + 4;
          Block.Ops.push_back(Return);
        }
        std::vector<char> Mask{char(!MaskStateBody), 1};
        if (Reverse) {
          std::reverse(Functions.begin(), Functions.end());
          std::reverse(Mask.begin(), Mask.end());
        }
        llvm::LLVMContext Context;
        MedLLVMEmitter Emitter;
        auto Module =
            Emitter.emit(Functions, Context, "dsp_siblings", Target, {},
                         nullptr, BinaryFormat::ELF, false, &Mask);
        ASSERT_NE(Module, nullptr);
        EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        for (const auto &Function : Functions) {
          const auto *Body = Module->getFunction(Function.Name);
          ASSERT_NE(Body, nullptr);
          EXPECT_TRUE(Body->hasFnAttribute(mips::DSPSourceMarker));
          EXPECT_EQ(Body->getFnAttribute("target-features").getValueAsString(),
                    "-dsp,-dspr2");
        }
        CEmitterOptions Options;
        Options.TheArch = Target;
        Options.Format = BinaryFormat::ELF;
        std::string Text;
        llvm::raw_string_ostream Stream(Text);
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, Options));
        EXPECT_NE(Text.find(mips::DSPSourceCBegin), std::string::npos) << Text;
        EXPECT_NE(Text.find(mips::DSPSourceCEnd), std::string::npos) << Text;
        // A declaration-only shard cannot hide a malformed state contract.
        auto &State = Functions[Reverse ? 1 : 0].Blocks[0].Ops[0];
        State.Output.Size = 8;
        EXPECT_THROW(Emitter.emit(Functions, Context, "bad_dsp", Target, {},
                                  nullptr, BinaryFormat::ELF, false, &Mask),
                     std::invalid_argument);
      }
}

TEST(MIPSDSPPipeline, DynamicStackHelperIsInsideTheDSPCompilerContract) {
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL}) {
    BinaryImage Image;
    Image.Arch = Target;
    Image.MIPS.emplace();
    Image.MIPS->HasDSP = true;
    HighFunc Function;
    Function.Name = "dsp_stack";
    Function.Entry = 0x1000;
    Function.ReturnType = NdType::makeVoid();
    Function.Params = {{"size", NdType::makeInt(4, false)}};
    MedVar Size;
    Size.Kind = MedVar::Param;
    Size.Id = 0;
    Size.Size = 4;
    Size.TheArch = Target;
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr =
        HighExpr::makeCall("__builtin_alloca", 0,
                           {HighExpr::makeVar(Size, Function.Params[0].Type)});
    Call.CallExpr->Type = NdType::makePtr(NdType::makeInt(1, false));
    Call.CallExpr->Type->Size = 4;
    Function.Body.push_back(std::move(Call));
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Function.Body.push_back(Return);
    CEmitterOptions Options;
    Options.TheArch = Target;
    Options.Format = BinaryFormat::ELF;
    Options.Image = &Image;
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, Stream, Options));
    const size_t Helper = Text.find("static inline uint64_t");
    ASSERT_NE(Helper, std::string::npos) << Text;
    EXPECT_LT(Text.find(mips::DSPSourceCBegin), Helper) << Text;
    EXPECT_GT(Text.find(mips::DSPSourceCEnd), Helper) << Text;
    EXPECT_NE(Text.find("return size + 16"), std::string::npos) << Text;
  }
}

} // namespace
