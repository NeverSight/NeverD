//===- MIPS16PipelineTests.cpp - MIPS16 source mode contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check ELF-to-source scalar and control paths, including explicit exclusion
/// of the alternate compressed ISA on definitions and declarations.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/DebugParamBinding.h"
#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/MIPS/MIPSInstructionModeSource.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <tuple>
#include <vector>

using namespace neverd;

namespace {

class MIPS16Pipeline : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPS16Pipeline, DoubleParameterNamesDistinguishWholeFromPhysicalWords) {
  FunctionSym Signature;
  Signature.ReturnType = NdType::makeInt(4, false);
  Signature.Params = {{"value", NdType::makeFloat(8)}};
  const bool Little = GetParam() == Arch::MIPS32EL;
  const auto Bind = [&](const std::vector<RecoveredParamSite> &Sites) {
    return bindDebugParams(GetParam(), BinaryFormat::ELF, Signature, Sites,
                           nullptr, InstructionMode::MIPS16);
  };
  const auto Whole = Bind({{mipsreg::r(4), 0, 8}});
  ASSERT_EQ(Whole.Params.size(), 1u);
  EXPECT_TRUE(Whole.Params[0].Whole);
  EXPECT_EQ(debugParamName(Signature, Whole.Params[0]), "value");
  const auto Split = Bind({{mipsreg::r(4), 0, 4}, {mipsreg::r(5), 1, 4}});
  ASSERT_EQ(Split.Params.size(), 2u);
  EXPECT_FALSE(Split.Params[0].Whole);
  EXPECT_FALSE(Split.Params[1].Whole);
  EXPECT_EQ(debugParamName(Signature, Split.Params[0]),
            Little ? "value" : "value_4");
  EXPECT_EQ(debugParamName(Signature, Split.Params[1]),
            Little ? "value_4" : "value");
  const auto Overlapping = Bind({{mipsreg::r(4), 0, 8}, {mipsreg::r(5), 1, 4}});
  EXPECT_FALSE(Overlapping.Params[0].Whole);
  const auto LaterWord = Bind({{mipsreg::r(5), 0, 8}});
  EXPECT_FALSE(LaterWord.Params[0].Whole);
}

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

TEST_P(MIPS16Pipeline, DeclaredLeadingFloatUsesItsActualEntryMode) {
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
      // mfc1 a0,f12; xor v0,a0,a1; jr ra; nop.
      const uint32_t Words[] = {0x44046000, 0x00851026, 0x03e00008, 0};
      for (unsigned I = 0; I < std::size(Words); ++I)
        File.put(0x100 + I * 4, Words[I], 4);
    }
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    auto Symbol = Symbol::makeFunc(Image->Entry, Compressed ? 6 : 16);
    Symbol.Name = "leading_float";
    Image->Symbols.push_back(std::move(Symbol));
    class FloatDebug final : public NullDebugContext {
    public:
      va_t Entry = 0;
      std::optional<FunctionSym> resolveFunction(va_t Address) const override {
        if (Address != Entry)
          return std::nullopt;
        FunctionSym F;
        F.Name = "leading_float";
        F.Addr = Address;
        F.ReturnType = NdType::makeInt(4, false);
        // Only the compressed body takes the leading float in a0.
        F.Params = {{"value", NdType::makeFloat(4)},
                    {"mask", NdType::makeInt(4, false)}};
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
    EXPECT_NE(Text.find("leading_float(float value, uint32_t mask) {"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("__builtin_bit_cast("), std::string::npos) << Text;
  }
}

TEST_P(MIPS16Pipeline, SplitDoubleEntryKeepsBothPhysicalWords) {
  mips_test::ELFBytes File(GetParam());
  File.abiFlags();
  File.Bytes[0xe5] = 1;
  File.Bytes[0xe7] = 1;
  File.put(36, 0x74001000, 4);
  File.put(24, 0x10101, 4);
  File.put(0xec, 0x400, 4);
  // Clang 22 + GNU as 2.47 for uint32_t f(double value, uint32_t mask):
  // low(value) ^ rotl(high(value), 7) ^ mask. The two words have deliberately
  // different uses, so an endian-dependent exchange cannot pass the replay.
  const bool Little = GetParam() == Arch::MIPS32EL;
  const uint16_t Words[] = {uint16_t(Little ? 0xecce : 0xedce),
                            0xf640,
                            uint16_t(Little ? 0x33a2 : 0x3382),
                            uint16_t(Little ? 0x32bc : 0x329c),
                            0xea6d,
                            uint16_t(Little ? 0xea8e : 0xeaae),
                            0xe8a0};
  for (unsigned I = 0; I < std::size(Words); ++I)
    File.put(0x100 + I * 2, Words[I], 2);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Symbol = Symbol::makeFunc(Image->Entry, sizeof(Words));
  Symbol.Name = "split_double";
  Image->Symbols.push_back(std::move(Symbol));
  class DoubleDebug final : public NullDebugContext {
  public:
    va_t Entry = 0;
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != Entry)
        return std::nullopt;
      FunctionSym F;
      F.Name = "split_double";
      F.Addr = Address;
      F.ReturnType = NdType::makeInt(4, false);
      F.Params = {{"value", NdType::makeFloat(8)},
                  {"mask", NdType::makeInt(4, false)}};
      F.PrototypeLibrary = UserPrototypeLibrary.str();
      return F;
    }
    bool hasInfo() const override { return true; }
  } Debug;
  Debug.Entry = Image->Entry;
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {Image->Entry};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedIRVerifierFailures, 0u);
    for (const auto &Function : Result.MedFuncs)
      ASSERT_TRUE(verifyMedFunc(Function, "MIPS16 split-double source entry"));
    CEmitterOptions Emit;
    Emit.TheArch = GetParam();
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      const auto *Body = Result.LlvmModule->getFunction("split_double");
      ASSERT_NE(Body, nullptr);
      ASSERT_EQ(Body->arg_size(), 2u);
      EXPECT_TRUE(Body->getArg(0)->getType()->isDoubleTy());
      EXPECT_TRUE(Body->getArg(1)->getType()->isIntegerTy(32));
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emit, &Debug));
    } else {
      ASSERT_EQ(Result.HighFuncs.size(), 1u);
      ASSERT_EQ(Result.HighFuncs[0].Params.size(), 2u);
      EXPECT_EQ(Result.HighFuncs[0].Params[0].Type->Kind, NdTypeKind::Float);
      EXPECT_EQ(Result.HighFuncs[0].Params[0].Type->Size, 8u);
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit, &Debug));
    }
    EXPECT_NE(Text.find("double value, uint32_t mask"), std::string::npos)
        << Text;
    EXPECT_NE(Text.find("NEVERD_MIPS16 "), std::string::npos) << Text;
  }
}

TEST_P(MIPS16Pipeline, TypedDirectCallReadsGPRFloatInEitherSelection) {
  mips_test::ELFBytes File(GetParam());
  File.Bytes.resize(0x140);
  File.put(52 + 16, File.Bytes.size(), 4);
  File.put(52 + 20, File.Bytes.size(), 4);
  File.abiFlags();
  File.Bytes[0xe5] = 1;
  File.Bytes[0xe7] = 1;
  File.put(36, 0x74001000, 4);
  File.put(24, 0x10101, 4);
  File.put(0xec, 0x400, 4);
  // Clang 22 + GNU as 2.47: save ra; reorder integer payloads;
  // jal fp_callee with the second argument in its physical delay slot;
  // restore ra; jrc ra. The callee's first argument is a float in a0.
  const uint16_t Caller[] = {0x64c3, 0x6744, 0x6785, 0x1800, 0x4048,
                             0x67a2, 0x6500, 0x6443, 0xe8a0};
  const uint16_t Callee[] = {0xecae, 0x6744, 0xe8a0};
  for (unsigned I = 0; I < std::size(Caller); ++I)
    File.put(0x100 + I * 2, Caller[I], 2);
  for (unsigned I = 0; I < std::size(Callee); ++I)
    File.put(0x120 + I * 2, Callee[I], 2);
  ELFLoader Loader;
  auto Image = Loader.loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (const auto &[Address, Size, Name] :
       {std::tuple{0x10100u, 18u, "kernel"},
        std::tuple{0x10120u, 6u, "fp_callee"}}) {
    auto S = Symbol::makeFunc(Address, Size);
    S.Name = Name;
    Image->Symbols.push_back(std::move(S));
    Image->Exports.push_back({Name, 0, Address});
    Image->MIPS->CodeModeEntries.emplace(Address, InstructionMode::MIPS16);
    Image->MIPS->CodeRegions.push_back(
        {Address, Address + Size, InstructionMode::MIPS16});
  }
  class CallDebug final : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x10120)
        return std::nullopt;
      FunctionSym F;
      F.Name = "fp_callee";
      F.Addr = Address;
      F.ReturnType = NdType::makeInt(4, false);
      F.Params = {{"value", NdType::makeFloat(4)},
                  {"mask", NdType::makeInt(4, false)}};
      F.PrototypeLibrary = UserPrototypeLibrary.str();
      return F;
    }
    bool hasInfo() const override { return true; }
  } Debug;
  for (bool Together : {false, true})
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Together);
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.OnlyFunctionEntries =
          Together ? std::set<va_t>{0x10100, 0x10120} : std::set<va_t>{0x10100};
      Options.LiftMode = LLVM;
      Options.EmitDumpOutput = false;
      auto Result = Pipeline().run(*Image, Context, Options, &Debug);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedIRVerifierFailures, 0u);
      CEmitterOptions Emit;
      Emit.TheArch = GetParam();
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &*Image;
      std::string Text;
      llvm::raw_string_ostream Out(Text);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        const llvm::Function *Target = nullptr;
        for (const auto &Function : *Result.LlvmModule) {
          auto Address = rewrite_source::getOriginalVA(Function);
          ASSERT_TRUE(bool(Address)) << llvm::toString(Address.takeError());
          if (*Address == 0x10120)
            Target = &Function;
        }
        ASSERT_NE(Target, nullptr);
        EXPECT_TRUE(Target->hasFnAttribute("mips16"));
        EXPECT_TRUE(Target->hasFnAttribute("nomicromips"));
        ASSERT_EQ(Target->arg_size(), 2u);
        EXPECT_TRUE(Target->getArg(0)->getType()->isFloatTy());
        EXPECT_TRUE(Target->getReturnType()->isIntegerTy(32));
        EXPECT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emit, &Debug));
      } else {
        EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit, &Debug));
        EXPECT_NE(Text.find("fp_callee("), std::string::npos) << Text;
      }
      EXPECT_NE(Text.find("NEVERD_MIPS16 "), std::string::npos) << Text;
    }
}

TEST_P(MIPS16Pipeline, ClassicEntryStubUsesItsDeclaredCompressedBody) {
  for (bool FP64 : {false, true}) {
    SCOPED_TRACE(FP64);
    mips_test::ELFBytes File(GetParam());
    File.Bytes.resize(0x180);
    File.put(52 + 16, File.Bytes.size(), 4);
    File.put(52 + 20, File.Bytes.size(), 4);
    File.abiFlags();
    File.Bytes[0xe5] = FP64 ? 2 : 1;
    File.Bytes[0xe7] = FP64 ? 6 : 1;
    File.put(36, FP64 ? 0x74001200 : 0x74001000, 4);
    File.put(24, 0x10100, 4);
    File.put(0xec, 0x400, 4);
    // GCC 12.4 + GNU as 2.47. Both the caller's integer reorder and the
    // stub's mfc1 execute in physical delay slots. Relocate the three entries
    // together; the jr destination keeps its MIPS16 tag.
    const uint32_t Caller[] = {0x44856000, 0x27bdffe8, 0xafbf0014, 0x0c004058,
                               0x00802825, 0x8fbf0014, 0x03e00008, 0x27bd0018};
    const uint16_t Callee[] = {0x6745, 0xe820, 0xea8e};
    const uint32_t Adapter[] = {0x3c190001, 0x27390141, 0x03200008, 0x44046000};
    for (unsigned I = 0; I < std::size(Caller); ++I)
      File.put(0x100 + I * 4, Caller[I], 4);
    for (unsigned I = 0; I < std::size(Callee); ++I)
      File.put(0x140 + I * 2, Callee[I], 2);
    for (unsigned I = 0; I < std::size(Adapter); ++I)
      File.put(0x160 + I * 4, Adapter[I], 4);
    ELFLoader Loader;
    auto Image = Loader.loadBuffer(File.buffer());
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    for (const auto &[Address, Size, Name, Mode] :
         {std::tuple{0x10100u, 32u, "kernel", InstructionMode::MIPSClassic},
          std::tuple{0x10140u, 6u, "fp_callee", InstructionMode::MIPS16},
          std::tuple{0x10160u, 16u, "entry_adapter",
                     InstructionMode::MIPSClassic}}) {
      auto S = Symbol::makeFunc(Address, Size);
      S.Name = Name;
      Image->Symbols.push_back(std::move(S));
      // The adapter's local symbol and spelling confer no prototype.
      if (Address != 0x10160)
        Image->Exports.push_back({Name, 0, Address});
      Image->MIPS->CodeModeEntries.emplace(Address, Mode);
      Image->MIPS->CodeRegions.push_back({Address, Address + Size, Mode});
    }
    class BodyDebug final : public NullDebugContext {
    public:
      std::optional<FunctionSym> resolveFunction(va_t Address) const override {
        if (Address != 0x10140)
          return std::nullopt;
        FunctionSym F;
        F.Name = "fp_callee";
        F.Addr = Address;
        F.ReturnType = NdType::makeInt(4, false);
        F.Params = {{"value", NdType::makeFloat(4)},
                    {"mask", NdType::makeInt(4, false)}};
        F.PrototypeLibrary = UserPrototypeLibrary.str();
        return F;
      }
      bool hasInfo() const override { return true; }
    } Debug;
    for (bool Together : {false, true})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Together);
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries =
            Together ? std::set<va_t>{0x10100, 0x10140, 0x10160}
                     : std::set<va_t>{0x10100};
        Options.LiftMode = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*Image, Context, Options, &Debug);
        std::string Diagnostic;
        if (!Result.Success) {
          Decoder Dec;
          ASSERT_TRUE(Dec.init(*Image));
          CFGBuilder CFG;
          const std::set<va_t> Entries{0x10100, 0x10140, 0x10160};
          CFG.setKnownFuncEntries(&Entries);
          llvm::raw_string_ostream Out(Diagnostic);
          Pipeline::dumpLowIR(
              {CFG.build(*Image, Dec, 0x10160, "entry_adapter")}, Out);
        }
        ASSERT_TRUE(Result.Success) << Result.Error << '\n' << Diagnostic;
        ASSERT_EQ(Result.MedIRVerifierFailures, 0u);
        const auto Signature = Result.CallEntryFloatingArguments.find(0x10160);
        ASSERT_NE(Signature, Result.CallEntryFloatingArguments.end());
        EXPECT_FALSE(Signature->second.MIPS16Parameters);
        ASSERT_EQ(Signature->second.Scalars.size(), 2u);
        EXPECT_EQ(Signature->second.Scalars[0].Kind,
                  SourceABICarrierKind::FloatingRegister);
        EXPECT_EQ(Signature->second.Scalars[0].Register, mipsreg::w(12));
        CEmitterOptions Emit;
        Emit.TheArch = GetParam();
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &*Image;
        std::string Text;
        llvm::raw_string_ostream Out(Text);
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Out, Emit, &Debug));
        } else {
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit, &Debug));
        }
      }
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
