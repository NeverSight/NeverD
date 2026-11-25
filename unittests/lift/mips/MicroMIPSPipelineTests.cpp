//===- MicroMIPSPipelineTests.cpp - Compressed source contracts ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise ELF-to-source mode retention and the architectural callable tag.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/MIPS/MIPSInstructionModeSource.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/raw_ostream.h"

#include <stdexcept>

using namespace neverd;

namespace {

class MicroMIPSPipeline : public ::testing::TestWithParam<Arch> {};

TEST(MicroMIPSPipelineSource, LLVMCModePreludeFollowsEmittedSignatures) {
  llvm::LLVMContext Context;
  llvm::Module Module("mixed", Context);
  llvm::IRBuilder<> IR(Context);
  auto *Type = llvm::FunctionType::get(IR.getInt32Ty(), false);
  auto *Micro = llvm::Function::Create(Type, llvm::Function::ExternalLinkage,
                                       "micro_target", Module);
  Micro->addFnAttr("micromips");
  IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Micro));
  IR.CreateRet(IR.getInt32(7));
  CEmitterOptions Options;
  Options.TheArch = Arch::MIPS64;
  Options.Format = BinaryFormat::ELF;
  for (bool CallsMicro : {false, true}) {
    auto *Classic =
        llvm::Function::Create(Type, llvm::Function::ExternalLinkage,
                               CallsMicro ? "calls_micro" : "leaf", Module);
    IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Classic));
    if (CallsMicro)
      IR.CreateRet(IR.CreateCall(Micro));
    else
      IR.CreateRet(IR.getInt32(42));
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(
        LLVMCEmitter().emit(Module, Out, Options, nullptr, nullptr, Classic));
    EXPECT_EQ(Text.find("#define NEVERD_MICROMIPS") != std::string::npos,
              CallsMicro)
        << Text;
    EXPECT_EQ(Text.find("#error \"this compiler cannot emit microMIPS"),
              std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("NEVERD_MICROMIPS_REFERENCE ") != std::string::npos,
              CallsMicro)
        << Text;
  }
  std::string Text;
  llvm::raw_string_ostream Out(Text);
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_NE(Text.find("#error \"this compiler cannot emit microMIPS"),
            std::string::npos)
      << Text;
}

TEST(MicroMIPSPipelineSource, N32RetainsNativeWideRecordTypes) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-call-result", Context);
  llvm::IRBuilder<> IR(Context);
  auto *Type =
      llvm::FunctionType::get(IR.getInt128Ty(), {IR.getInt128Ty()}, false);
  auto *Function = llvm::Function::Create(Type, llvm::Function::ExternalLinkage,
                                          "wide_identity", Module);
  IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  IR.CreateRet(Function->getArg(0));
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64,
                      Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    CEmitterOptions Options;
    Options.TheArch = Target;
    Options.Format = BinaryFormat::ELF;
    Options.PreserveLLVMFunctionTypes = true;
    Options.RecordDefinitions = [](const std::vector<TypeRef> &) {
      CRecordDefinitions Definitions;
      Definitions.Text = "struct WideResult { unsigned __int128 value; };\n";
      Definitions.Records = {{"WideResult", false}};
      Definitions.Tags = {{"WideResult", false}};
      return Definitions;
    };
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
    EXPECT_NE(Text.find("struct WideResult { unsigned __int128 value; };"),
              std::string::npos)
        << Text;
    EXPECT_EQ(Text.find("#define __int128 _BitInt(128)") != std::string::npos,
              !isMIPS64(Target))
        << Text;
  }
}

TEST(MicroMIPSPipelineSource, GCCBitCastPreludeFollowsTheSelectedBody) {
  llvm::LLVMContext Context;
  llvm::Module Module("selected-bit-copy", Context);
  llvm::IRBuilder<> IR(Context);
  auto *PlainType =
      llvm::FunctionType::get(IR.getInt32Ty(), {IR.getInt32Ty()}, false);
  auto *Plain = llvm::Function::Create(
      PlainType, llvm::Function::ExternalLinkage, "integer_identity", Module);
  IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Plain));
  IR.CreateRet(Plain->getArg(0));
  auto *BitsType = llvm::FunctionType::get(IR.getFloatTy(), false);
  auto *Bits = llvm::Function::Create(BitsType, llvm::Function::ExternalLinkage,
                                      "nan_payload", Module);
  IR.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Bits));
  // Ordinary LLVM bitcast instructions use memcpy. A nonfinite constant
  // needs the compiler bit-cast expression to retain its exact payload.
  IR.CreateRet(llvm::ConstantFP::get(
      Context,
      llvm::APFloat(llvm::APFloat::IEEEsingle(), llvm::APInt(32, 0x7fc12345))));
  for (Arch Target : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64,
                      Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    CEmitterOptions Options;
    Options.TheArch = Target;
    Options.Format = BinaryFormat::ELF;
    Options.PreserveLLVMFunctionTypes = true;
    for (bool Micro : {false, true}) {
      SCOPED_TRACE(Micro);
      if (Micro) {
        Plain->addFnAttr("micromips");
        Bits->addFnAttr("micromips");
      } else {
        Plain->removeFnAttr("micromips");
        Bits->removeFnAttr("micromips");
      }
      for (const llvm::Function *Selected : {Plain, Bits}) {
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options, nullptr, nullptr,
                                        Selected));
        EXPECT_EQ(Text.find("#define __builtin_bit_cast") != std::string::npos,
                  Selected == Bits)
            << Text;
        EXPECT_EQ(Text.find("__builtin_bit_cast(float,") != std::string::npos,
                  Selected == Bits)
            << Text;
      }
    }
  }
}

void checkHalfwordAlignedALUSources(Arch Target, unsigned Revision) {
  mips_test::ELFBytes File(Target);
  const bool R6 = Revision == 6;
  if (File.Is64) {
    constexpr va_t Base = 0x100010000;
    File.put(48, R6 ? 0xa2000000u : 0x82000000u, 4);
    File.put(64 + 16, Base, 8);
    File.put(64 + 24, Base, 8);
    File.put(24, Base + 0x101, 8);
  } else {
    File.put(36, R6 ? 0x92001000u : 0x72001000u, 4);
    File.put(24, 0x10101, 4);
  }
  // MOVE16 v0,a0; [D]ADDIU v0,v0,1 at a halfword boundary; JRC[16] ra.
  File.put(0x100, 0x0c44, 2);
  File.put(0x102, File.Is64 ? 0x5c42 : 0x3042, 2);
  File.put(0x104, 1, 2);
  File.put(0x106, R6 ? 0x47e3 : 0x45bf, 2);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Symbol = Symbol::makeFunc(Image->Entry, 8);
  Symbol.Name = "micro_increment";
  Image->Symbols.push_back(std::move(Symbol));
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
    EXPECT_EQ(Result.MedFuncs.front().Params.size(), 1u);
    EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
    CEmitterOptions Emit;
    Emit.TheArch = Target;
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      const auto *Body = Result.LlvmModule->getFunction("micro_increment");
      ASSERT_NE(Body, nullptr);
      EXPECT_TRUE(Body->hasFnAttribute("micromips"));
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, Emit));
    } else {
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Emit));
    }
    EXPECT_NE(Text.find("__attribute__((micromips,nomips16))"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("__attribute__((micromips))"), std::string::npos);
    const auto PreludeEnd = Text.find("#endif\n\n");
    ASSERT_NE(PreludeEnd, std::string::npos);
    EXPECT_NE(Text.find("NEVERD_MICROMIPS ", PreludeEnd), std::string::npos);
  }
}

TEST_P(MicroMIPSPipeline, HalfwordAlignedALUReachesBothSourcesWithMode) {
  checkHalfwordAlignedALUSources(GetParam(), 2);
  checkHalfwordAlignedALUSources(GetParam(), 6);
}

TEST(MicroMIPS64Pipeline, HighELFEntryReachesBothSources) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Revision : {2, 6}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Target) << " r" << Revision);
      checkHalfwordAlignedALUSources(Target, Revision);
    }
}

BinaryImage mixedImage(Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Mode = InstructionMode::MixedMIPS;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = getTargetRegInfo(Target).PointerSize == 8 ? Bitness::Bits64
                                                         : Bitness::Bits32;
  Image.MIPS.emplace();
  Image.MIPS->ABI = isMIPSN32(Target)  ? MIPSABI::N32
                    : isMIPS64(Target) ? MIPSABI::N64
                                       : MIPSABI::O32;
  Image.MIPS->HasMicroMIPS = true;
  Image.MIPS->CodeModeEntries = {{0x1000, InstructionMode::MIPSClassic},
                                 {0x1100, InstructionMode::MicroMIPS}};
  Segment Text;
  Text.VA = 0x1000;
  Text.Data.resize(0x200);
  Text.Size = Text.FileSz = Text.Data.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Text));
  auto Symbol = Symbol::makeFunc(0x1100, 4);
  Symbol.Name = "micro_target";
  Image.Symbols.push_back(std::move(Symbol));
  return Image;
}

TEST(MicroMIPSPipelineSource, ExternalMicroModeDoesNotRequireBodyCodegen) {
  for (Arch Architecture :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL,
        Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Image = mixedImage(Architecture);
    HighFunc Caller;
    Caller.Entry = 0x1000;
    Caller.Name = "calls_micro";
    Caller.ReturnType = NdType::makeInt(4, true);
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeCall("micro_target", 0x1100, {});
    Return.RetVal->Type = Caller.ReturnType;
    Caller.Body.push_back(Return);
    HighFunc Target = Caller;
    Target.Entry = 0x1100;
    Target.Name = "micro_target";
    Target.Body.front().RetVal = HighExpr::makeConst(7, 4);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.Format = BinaryFormat::ELF;
    Options.Image = &Image;
    for (bool HasBody : {false, true}) {
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      std::vector<HighFunc> Functions{Caller};
      if (HasBody)
        Functions.push_back(Target);
      ASSERT_TRUE(HighCEmitter().emit(Functions, Out, Options));
      EXPECT_EQ(Text.find("#error \"this compiler cannot emit microMIPS") !=
                    std::string::npos,
                HasBody)
          << Text;
      EXPECT_NE(Text.find("micro_target("), std::string::npos) << Text;
      if (!HasBody)
        EXPECT_NE(Text.find("NEVERD_MICROMIPS_REFERENCE extern int"),
                  std::string::npos)
            << Text;
    }
  }
}

TEST(MicroMIPSPipelineSource, TypedCallsRetainExternalInstructionModes) {
  for (Arch Architecture :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL,
        Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    const auto Word = TRI.integerReturnWordBytes();
    for (auto Mode : {InstructionMode::MIPSClassic, InstructionMode::MicroMIPS,
                      InstructionMode::MIPS16}) {
      if (Mode == InstructionMode::MIPS16 && isMIPS64(Architecture))
        continue;
      for (bool Analyst : {false, true}) {
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(static_cast<unsigned>(Mode));
        SCOPED_TRACE(Analyst);
        auto Image = mixedImage(Architecture);
        Image.MIPS->HasMIPS16 = Mode == InstructionMode::MIPS16;
        Image.MIPS->HasMicroMIPS = !Image.MIPS->HasMIPS16;
        Image.MIPS->CodeModeEntries[0x1100] = Mode;
        ASSERT_EQ(Image.instructionModeAt(0x1100), Mode);
        MedFunc Function;
        Function.Entry = 0x1000;
        Function.Name = "typed_caller";
        Function.ReturnType = NdType::makeInt(Word, false);
        MedBlock Block;
        Block.Id = 0;
        Block.StartAddr = Function.Entry;
        MedOp Call;
        Call.Opcode = NdOp::CALL;
        Call.Addr = Function.Entry;
        Call.OriginSeq = 0;
        Call.Output.Kind = MedVar::Reg;
        Call.Output.RegOff = TRI.IntReturnReg;
        Call.Output.Size = Word;
        Call.Output.TheArch = Architecture;
        Call.Output.Id = 1;
        Call.Output.SSAVer = 1;
        Call.addInput(MedVar::makeConst(0x1100, TRI.PointerSize));
        Call.addInput(MedVar::makeConst(0x12345678, Word));
        MedCallInfo Info;
        Info.BlockId = Block.Id;
        Info.OpIdx = 0;
        Info.TargetAddr = 0x1100;
        Info.TargetName = "micro_target";
        Info.Args = {Call.Inputs[1]};
        if (Analyst) {
          auto Hint = std::make_shared<SourceCallTypeHint>();
          Hint->CallKind = SourceCallTypeHint::Kind::Analyst;
          Hint->AnalystSite = SourceCallOccurrenceKey{
              Call.Addr, Call.OriginSeq, NdOp::CALL, Info.TargetAddr};
          Hint->Signature.ReturnType = Function.ReturnType;
          Hint->Signature.Parameters = {{"value", Function.ReturnType}};
          std::string Error;
          ASSERT_TRUE(assignAnalystCallABI(*Hint, Architecture,
                                           BinaryFormat::ELF, Error, Mode))
              << Error;
          Call.SourceCallHint = Hint;
          Info.SourceCallHint = std::move(Hint);
        }
        Block.Ops.push_back(Call);
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.addInput(Call.Output);
        Block.Ops.push_back(Return);
        Function.Blocks.push_back(std::move(Block));
        Function.CallInfos.push_back(std::move(Info));
        llvm::LLVMContext Context;
        MedLLVMEmitter Emitter;
        Emitter.setAnalystSourceProjection(Analyst);
        auto Module = Emitter.emit({Function}, Context, "typed_call_mode",
                                   Architecture, {}, &Image, BinaryFormat::ELF);
        ASSERT_TRUE(Module);
        EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
        const auto *Callee = Module->getFunction("micro_target");
        ASSERT_TRUE(Callee);
        EXPECT_EQ(mips::instructionModeFromLLVM(*Callee), Mode);
        CEmitterOptions Options;
        Options.TheArch = Architecture;
        Options.Format = BinaryFormat::ELF;
        Options.Image = &Image;
        std::string Text;
        llvm::raw_string_ostream Output(Text);
        ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
        const std::string Attribute =
            mips::instructionModeCAttribute(Mode, /*HasBody=*/false);
        const unsigned DeclarationBits =
            Callee->getReturnType()->getIntegerBitWidth();
        EXPECT_NE(Text.find(Attribute + "uint" +
                            std::to_string(DeclarationBits) +
                            "_t micro_target("),
                  std::string::npos)
            << Text;
      }
    }
  }
}

void checkNumericHighCIdentities(Arch Architecture) {
  auto Image = mixedImage(Architecture);
  const auto PointerBytes = getTargetRegInfo(Architecture).PointerSize;
  using P = ConstantAddressProvenance;
  for (P Provenance : {P::CodeAddress, P::Address, P::DataAddress, P::Scalar})
    for (va_t Value : {0x1100u, 0x1101u}) {
      SCOPED_TRACE(int(Provenance));
      SCOPED_TRACE(Value);
      HighFunc Function;
      Function.Entry = 0x1000;
      Function.Name = "numeric_value";
      Function.ReturnType = NdType::makeInt(PointerBytes, false);
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeConst(Value, PointerBytes, Provenance);
      Function.Body.push_back(std::move(Return));
      CEmitterOptions Emit;
      Emit.TheArch = Architecture;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      if (Provenance == P::CodeAddress && Value == 0x1100) {
        // An exact callable occurrence must already carry the compressed
        // mode bit. Its untagged code-byte address is not interchangeable.
        EXPECT_THROW(HighCEmitter().emit({Function}, Output, Emit),
                     std::invalid_argument);
      } else if (Provenance == P::CodeAddress ||
                 (Provenance == P::Address && Value == 0x1101)) {
        ASSERT_TRUE(HighCEmitter().emit({Function}, Output, Emit));
        // An image symbol alone supplies no C prototype. Its numeric identity
        // uses the linker alias without inventing a callable declaration.
        EXPECT_NE(Text.find("extern unsigned char neverd_code_micro_target[] "
                            "__asm__(\"micro_target\");"),
                  std::string::npos)
            << Text;
        EXPECT_NE(Text.find("(uintptr_t)neverd_code_micro_target"),
                  std::string::npos)
            << Text;
        EXPECT_EQ(Text.find("NEVERD_MICROMIPS"), std::string::npos) << Text;

        // With an actual definition, both the mode and the typed callable
        // identity must survive. Keep this separate from the symbol-only case.
        HighFunc Target;
        Target.Entry = 0x1100;
        Target.Name = "micro_target";
        Target.ReturnType = NdType::makeInt(4, false);
        HighStmt TargetReturn;
        TargetReturn.Kind = StmtKind::Return;
        TargetReturn.RetVal = HighExpr::makeConst(42, 4);
        Target.Body.push_back(std::move(TargetReturn));
        std::string DefinedText;
        llvm::raw_string_ostream DefinedOutput(DefinedText);
        ASSERT_TRUE(
            HighCEmitter().emit({Function, Target}, DefinedOutput, Emit));
        EXPECT_NE(DefinedText.find("NEVERD_MICROMIPS uint32_t micro_target("),
                  std::string::npos)
            << DefinedText;
        EXPECT_NE(DefinedText.find("return (uint" +
                                   std::to_string(PointerBytes * 8) +
                                   "_t)(uintptr_t)micro_target;"),
                  std::string::npos)
            << DefinedText;
        EXPECT_EQ(DefinedText.find("neverd_code_micro_target"),
                  std::string::npos)
            << DefinedText;
      } else {
        EXPECT_TRUE(HighCEmitter().emit({Function}, Output, Emit)) << Text;
        EXPECT_EQ(Text.find("micro_target"), std::string::npos) << Text;
      }
      // A data/code-byte relation may require its own original-image backing;
      // it must never silently acquire the callable's mode-tagged identity.
    }
}

void checkNumericLLVMIdentities(Arch Architecture) {
  auto Image = mixedImage(Architecture);
  const auto PointerBytes = getTargetRegInfo(Architecture).PointerSize;
  using P = ConstantAddressProvenance;
  for (P Provenance : {P::CodeAddress, P::Address}) {
    MedFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "numeric_value";
    Function.ReturnType = NdType::makeInt(PointerBytes, false);
    Function.Blocks.emplace_back();
    auto &Block = Function.Blocks.back();
    Block.Id = 0;
    Block.StartAddr = 0x1000;
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output.Kind = MedVar::Reg;
    Copy.Output.RegOff = mipsreg::V0;
    Copy.Output.Size = PointerBytes;
    Copy.Output.TheArch = Architecture;
    Copy.Output.Id = 1;
    Copy.Output.SSAVer = 1;
    auto Value = MedVar::makeConst(0x1101, PointerBytes);
    Value.Provenance = Provenance;
    Copy.addInput(Value);
    Block.Ops.push_back(Copy);
    MedOp Return;
    Return.Opcode = NdOp::RETURN;
    Block.Ops.push_back(Return);
    // A selected target has a definition; an authenticated function outside
    // the selection has a declaration. Mode evidence alone supplies neither.
    MedFunc TargetFunction = Function;
    TargetFunction.Entry = 0x1100;
    TargetFunction.Name = "micro_target";
    TargetFunction.Blocks.front().StartAddr = 0x1100;
    TargetFunction.Blocks.front().Ops.front().Inputs[0] =
        MedVar::makeConst(42, PointerBytes);
    llvm::LLVMContext Context;
    MedLLVMEmitter Emitter;
    if (Provenance == P::CodeAddress) {
      auto Declared =
          MedLLVMEmitter().emit({Function}, Context, "unselected_target",
                                Architecture, {}, &Image, BinaryFormat::ELF);
      ASSERT_TRUE(Declared);
      EXPECT_FALSE(llvm::verifyModule(*Declared, &llvm::errs()));
      const auto *Entry = Declared->getFunction("micro_target");
      ASSERT_TRUE(Entry);
      EXPECT_TRUE(Entry->isDeclaration());
      EXPECT_FALSE(Entry->use_empty());
      EXPECT_EQ(mips::instructionModeFromLLVM(*Entry),
                InstructionMode::MicroMIPS);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = BinaryFormat::ELF;
      Options.Image = &Image;
      std::string Text;
      llvm::raw_string_ostream Output(Text);
      ASSERT_TRUE(LLVMCEmitter().emit(*Declared, Output, Options));
      EXPECT_EQ(Text.find("#error"), std::string::npos) << Text;
      EXPECT_NE(Text.find("NEVERD_MICROMIPS_REFERENCE "), std::string::npos)
          << Text;
      auto Missing = mixedImage(Architecture);
      Missing.Symbols.clear();
      EXPECT_FALSE(MedLLVMEmitter().emit({Function}, Context, "missing_target",
                                         Architecture, {}, &Missing,
                                         BinaryFormat::ELF));
    }
    auto Module =
        Emitter.emit({Function, TargetFunction}, Context, "micro_pointer",
                     Architecture, {}, &Image, BinaryFormat::ELF);
    ASSERT_TRUE(Module);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto *Target = Module->getFunction("micro_target");
    ASSERT_NE(Target, nullptr);
    EXPECT_EQ(mips::instructionModeFromLLVM(*Target),
              InstructionMode::MicroMIPS);
    // Code alignment must not turn the callable tag into a known zero, even
    // before native code generation or when the pointer is a constant.
    Target->setAlignment(llvm::Align(4));
    auto *Address = llvm::ConstantExpr::getPtrToInt(
        Target, llvm::Type::getIntNTy(Context, 8 * PointerBytes));
    EXPECT_FALSE(
        llvm::computeKnownBits(Address, Module->getDataLayout()).Zero[0]);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.Format = BinaryFormat::ELF;
    Options.Image = &Image;
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    ASSERT_TRUE(LLVMCEmitter().emit(*Module, Output, Options));
    EXPECT_NE(Text.find("micro_target"), std::string::npos) << Text;
    EXPECT_NE(Text.find("NEVERD_MICROMIPS"), std::string::npos) << Text;
    EXPECT_NE(Text.find("#error \"this compiler cannot emit microMIPS"),
              std::string::npos)
        << Text;
  }
}

TEST_P(MicroMIPSPipeline, HighCDoesNotConfuseCodeBytesAndCallableValues) {
  checkNumericHighCIdentities(GetParam());
}

TEST_P(MicroMIPSPipeline, LLVMRetainsNumericMicroIdentitiesBeforeOptimization) {
  checkNumericLLVMIdentities(GetParam());
}

TEST(MicroMIPS64Pipeline, BothSourcesPreserveNumericCodeIdentityAndWidth) {
  for (Arch Target :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    checkNumericHighCIdentities(Target);
    checkNumericLLVMIdentities(Target);
  }
}

TEST_P(MicroMIPSPipeline, NumericModeBoundaryRejectsUnknownAndMismatchedABIs) {
  auto Image = mixedImage(GetParam());
  EXPECT_NO_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100));
  EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, InvalidVA),
               std::invalid_argument);
  Image.Mode = InstructionMode::MicroMIPS;
  EXPECT_NO_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100));
  EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, InvalidVA),
               std::invalid_argument);
  Image.MIPS->HasMicroMIPS = false;
  EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
               std::invalid_argument);
  Image.MIPS->HasMicroMIPS = true;
  for (Arch Wide :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    Image.Arch = Wide;
    EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
                 std::invalid_argument);
  }
}

TEST(MicroMIPSPipelineSource, NumericModeBoundaryAdmitsQualifiedWideProfiles) {
  for (Arch Target :
       {Arch::MIPS64, Arch::MIPS64EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(Target));
    const bool N32 = Target == Arch::MIPS64N32 || Target == Arch::MIPS64N32EL;
    auto Image = mixedImage(Target);
    Image.Bits = N32 ? Bitness::Bits32 : Bitness::Bits64;
    Image.MIPS->ABI = N32 ? MIPSABI::N32 : MIPSABI::N64;
    EXPECT_NO_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100));
    EXPECT_NO_THROW(mips::requireNumericCodePointerSource(&Image, 0x1000));
    EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, InvalidVA),
                 std::invalid_argument);
    Image.MIPS->ABI = N32 ? MIPSABI::N64 : MIPSABI::N32;
    EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
                 std::invalid_argument);
    Image.MIPS->ABI = N32 ? MIPSABI::N32 : MIPSABI::N64;
    for (unsigned Revision : {3u, 5u, 6u}) {
      Image.MIPS->ISARevision = Revision;
      EXPECT_NO_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100));
    }
    for (unsigned Revision : {1u, 4u}) {
      Image.MIPS->ISARevision = Revision;
      EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
                   std::invalid_argument);
    }
    Image.MIPS->ISARevision = 2;
    Image.MIPS->HistoricalISA = 3;
    EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
                 std::invalid_argument);
    Image.MIPS->HistoricalISA = 0;
    Image.MIPS->HasMicroMIPS = false;
    EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
                 std::invalid_argument);
    Image.MIPS->HasMIPS16 = true;
    Image.Mode = InstructionMode::MIPS16;
    Image.MIPS->CodeModeEntries.clear();
    EXPECT_THROW(mips::requireNumericCodePointerSource(&Image, 0x1100),
                 std::invalid_argument);
  }
}

TEST_P(MicroMIPSPipeline, HighCRejectsWholeAndPartialUnboundImportValues) {
  auto Image = mixedImage(GetParam());
  Segment GOT;
  GOT.VA = 0x2000;
  GOT.Data.resize(16);
  GOT.Size = GOT.FileSz = GOT.Data.size();
  GOT.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(GOT));
  ASSERT_TRUE(Image.recordImportStorageSlot(0x2004, "external_function", 0,
                                            ImportStorageEvidence::LoaderBind));
  Import Imported;
  Imported.Name = "external_function";
  Imported.IATAddr = 0x2004;
  Image.Imports.push_back(std::move(Imported));
  const auto Check = [&](ExprPtr Value, bool Reject, bool Indexed = false) {
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "read_import_bits";
    Function.ReturnType = Value->Type;
    if (Indexed)
      Function.Params.push_back(
          {"index", NdType::makeInt(4, false), mipsreg::A0, 0});
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Value);
    Function.Body.push_back(std::move(Return));
    CEmitterOptions Emit;
    Emit.TheArch = GetParam();
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &Image;
    std::string Text;
    llvm::raw_string_ostream Output(Text);
    if (Reject)
      EXPECT_THROW(HighCEmitter().emit({Function}, Output, Emit),
                   std::invalid_argument);
    else
      EXPECT_TRUE(HighCEmitter().emit({Function}, Output, Emit));
  };
  const auto Address = [](unsigned Offset) {
    return HighExpr::makeConst(0x2000 + Offset, 4,
                               ConstantAddressProvenance::DataAddress);
  };
  for (auto [Offset, Size] :
       {std::pair{4, 4}, {4, 1}, {7, 1}, {3, 2}, {0, 4}, {8, 4}}) {
    SCOPED_TRACE(Offset);
    SCOPED_TRACE(Size);
    Check(HighExpr::makeLoad(Address(Offset), NdType::makeInt(Size, false)),
          Offset != 0 && Offset != 8);
  }
  // The merge's actual span, not just its effective address, owns admission.
  // Both full and partial reads can observe an unbound callable's tag.
  for (Intrinsic Id : {Intrinsic::MIPSLwl, Intrinsic::MIPSLwr}) {
    SCOPED_TRACE(int(Id));
    for (unsigned Offset : {4u, 5u, 6u, 7u}) {
      SCOPED_TRACE(Offset);
      auto Merge = HighExpr::makeCall(
          "", 0, {Address(Offset), HighExpr::makeConst(0, 4)});
      Merge->IntrinsicId = Id;
      Merge->Type = NdType::makeInt(4, false);
      Check(std::move(Merge), true);
    }
  }
  MedVar Index;
  Index.Kind = MedVar::Param;
  Index.TheArch = GetParam();
  Index.Size = 4;
  auto Variable = HighExpr::makeVar(Index, NdType::makeInt(4, false));
  auto Indexed = HighExpr::makeBinop(
      NdOp::INT_ADD, Address(0),
      HighExpr::makeBinop(NdOp::INT_MULT, Variable, HighExpr::makeConst(4, 4)));
  Check(HighExpr::makeLoad(Indexed, NdType::makeInt(4, false)), true, true);
  // An external parameter supplies runtime storage, not a file-byte backing.
  Check(HighExpr::makeLoad(Variable, NdType::makeInt(4, false)), false, true);
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MicroMIPSPipeline,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
