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
#include "../../../lib/pipeline/MIPS/MIPS16EntryAdapter.h"
#include "../../../lib/pipeline/MIPS/MIPS16ReturnAdapter.h"
#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/MIPS/MIPSInstructionModeSource.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/MIPS/MIPS16ReturnTransport.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MIPS/MIPSReturnTargets.h"
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

TEST_P(MIPS16Pipeline, ReturnHelpersRequireExactByteTransport) {
  enum class Change {
    None,
    WrongSource,
    WrongDestination,
    WrongLink,
    Memory,
    FCSR,
    MissingDelay,
    Padding,
    CompressedMode
  };
  for (bool FP64 : {false, true})
    for (unsigned Bytes : {4u, 8u})
      for (unsigned Elements : {1u, 2u})
        for (Change C :
             {Change::None, Change::WrongSource, Change::WrongDestination,
              Change::WrongLink, Change::Memory, Change::FCSR,
              Change::MissingDelay, Change::Padding, Change::CompressedMode}) {
          SCOPED_TRACE(FP64);
          SCOPED_TRACE(Bytes);
          SCOPED_TRACE(Elements);
          SCOPED_TRACE(int(C));
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
          const bool Little = GetParam() == Arch::MIPS32EL;
          std::vector<uint32_t> Words;
          const auto Move = [&](unsigned GPR, unsigned FPR, bool High = false) {
            Words.push_back((High ? 0x44e00000 : 0x44800000) | (GPR << 16) |
                            (FPR << 11));
          };
          // GNU libgcc's scalar/complex return contracts, with the final
          // transfer in jr's actual delay slot. No symbol grants this proof.
          for (unsigned E = 0; E != Elements; ++E) {
            if (Bytes == 4) {
              Move(2 + E, 2 * E);
            } else if (FP64) {
              Move(2 + 2 * E + !Little, 2 * E);
              Move(2 + 2 * E + Little, 2 * E, true);
            } else {
              Move(2 + 2 * E, 2 * E + !Little);
              Move(3 + 2 * E, 2 * E + Little);
            }
          }
          Words.insert(Words.end() - 1, 0x03e00008);
          switch (C) {
          case Change::WrongSource:
            Words.back() ^= 1u << 16;
            break;
          case Change::WrongDestination:
            Words.back() ^= 4u << 11;
            break;
          case Change::WrongLink:
            Words[Words.size() - 2] = 0x03200008;
            break;
          case Change::Memory:
            Words.back() = 0xafa20000;
            break;
          case Change::FCSR:
            Words.back() = 0x44c2f800;
            break;
          case Change::MissingDelay:
            Words.pop_back();
            break;
          case Change::Padding:
            Words.insert(Words.begin(), 0);
            break;
          default:
            break;
          }
          for (size_t I = 0; I < Words.size(); ++I)
            File.put(0x100 + I * 4, Words[I], 4);
          ELFLoader Loader;
          auto Image = Loader.loadBuffer(File.buffer());
          ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
          auto Symbol = Symbol::makeFunc(0x10100, Words.size() * 4);
          Symbol.Name =
              C == Change::None ? "anonymous_transfer" : "__mips16_ret_df";
          Image->Symbols.push_back(std::move(Symbol));
          const auto Mode = C == Change::CompressedMode
                                ? InstructionMode::MIPS16
                                : InstructionMode::MIPSClassic;
          Image->MIPS->CodeModeEntries.emplace(0x10100, Mode);
          Image->MIPS->CodeRegions.push_back(
              {0x10100, 0x10100 + Words.size() * 4, Mode});
          const auto Proof = proveMIPS16ReturnTransport(*Image, 0x10100);
          ASSERT_EQ(bool(Proof), C == Change::None);
          if (!Proof)
            continue;
          EXPECT_EQ(Proof->ElementBytes, Bytes);
          EXPECT_EQ(Proof->Elements, Elements);
          EXPECT_FALSE(Proof->Effects.empty());
          for (const auto &Op : Proof->Effects) {
            EXPECT_NE(Op.Opcode, NdOp::RETURN);
            if (Op.Output.isReg())
              EXPECT_TRUE(mipsreg::isVector(Op.Output.Offset));
          }
        }
}

TEST_P(MIPS16Pipeline, RuntimeFloatReturnPreservesOriginalCallAndSourceResult) {
  for (bool FP64 : {false, true})
    for (bool BadHelper : {false, true}) {
      SCOPED_TRACE(FP64);
      SCOPED_TRACE(BadHelper);
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
      // GCC 12.4 o32 MIPS16 float return, including SAVE/RESTORE and JALX.
      // The classic caller observes f0; the compressed body retains v0.
      const uint32_t Caller[] = {0x27bdffe8, 0xafb00010, 0xafbf0014, 0x74004050,
                                 0x00a08025, 0x8fbf0014, 0x44020000, 0x02021026,
                                 0x8fb00010, 0x03e00008, 0x27bd0018};
      const uint16_t Callee[] = {0x9c40, 0x64c1, 0x1c00, 0x4058,
                                 0x6500, 0x6441, 0xe8a0, 0x6500};
      for (unsigned I = 0; I < std::size(Caller); ++I)
        File.put(0x100 + 4 * I, Caller[I], 4);
      for (unsigned I = 0; I < std::size(Callee); ++I)
        File.put(0x140 + 2 * I, Callee[I], 2);
      File.put(0x160, 0x03e00008, 4);
      File.put(0x164, BadHelper ? 0xafa20000 : 0x44820000, 4);
      ELFLoader Loader;
      auto Image = Loader.loadBuffer(File.buffer());
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      for (const auto &[Address, Size, Name, Mode] :
           {std::tuple{0x10100u, 44u, "kernel", InstructionMode::MIPSClassic},
            std::tuple{0x10140u, 16u, "fp_callee", InstructionMode::MIPS16},
            std::tuple{0x10160u, 8u, "transfer_leaf",
                       InstructionMode::MIPSClassic}}) {
        auto Symbol = Symbol::makeFunc(Address, Size);
        Symbol.Name = Name;
        Image->Symbols.push_back(std::move(Symbol));
        Image->MIPS->CodeModeEntries.emplace(Address, Mode);
        Image->MIPS->CodeRegions.push_back({Address, Address + Size, Mode});
      }
      class ReturnDebug final : public NullDebugContext {
      public:
        std::optional<FunctionSym>
        resolveFunction(va_t Address) const override {
          if (Address != 0x10140)
            return std::nullopt;
          FunctionSym F;
          F.Addr = Address;
          F.Name = "fp_callee";
          F.ReturnType = NdType::makeFloat(4);
          auto Pointer = NdType::makePtr(NdType::makeInt(4, false));
          Pointer->Size = Pointer->Alignment = 4;
          F.Params = {{"input", std::move(Pointer)}};
          F.PrototypeLibrary = UserPrototypeLibrary.str();
          return F;
        }
        bool hasInfo() const override { return true; }
      } Debug;
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.OnlyFunctionEntries = {0x10100, 0x10140};
        Options.LiftMode = Options.SourceProjection = LLVM;
        Options.EmitDumpOutput = false;
        auto Result = Pipeline().run(*Image, Context, Options, &Debug);
        if (BadHelper) {
          EXPECT_FALSE(Result.Success);
          continue;
        }
        ASSERT_TRUE(Result.Success) << Result.Error;
        EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
        const auto Low = llvm::find_if(
            Result.LowFuncs, [](const auto &F) { return F.Entry == 0x10140; });
        ASSERT_NE(Low, Result.LowFuncs.end());
        bool OriginalCall = false;
        for (const auto &Block : Low->Blocks)
          for (const auto &Op : Block.Ops)
            OriginalCall |= Op.Opcode == NdOp::CALL && Op.NumInputs &&
                            Op.Inputs[0].isConst() &&
                            Op.Inputs[0].Offset == 0x10160;
        EXPECT_TRUE(OriginalCall);
        const auto Transports =
            pipeline_detail::mips16ReturnTransports(*Image, Result.LowFuncs);
        const auto Projected =
            pipeline_detail::projectMIPS16ReturnCalls(*Image, *Low, Transports);
        ASSERT_TRUE(Projected);
        auto BoundaryError = validateLowInstructionBoundaries(
            *Projected, LowInstructionBoundaryRequirement::Required);
        EXPECT_FALSE(bool(BoundaryError))
            << llvm::toString(std::move(BoundaryError));
        for (const auto &Function : Result.MedFuncs)
          for (const auto &Call : Function.CallInfos)
            EXPECT_NE(Call.TargetAddr, 0x10160u);
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
        EXPECT_NE(Text.find("float fp_callee("), std::string::npos) << Text;
        EXPECT_EQ(Text.find("transfer_leaf("), std::string::npos) << Text;
      }
    }
}

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
          std::tuple{0x10160u, 16u, "__fn_stub_fp_callee",
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
        // Address-bound user types need not repeat the ELF symbol's name.
        // Keep the body unnamed here, as the local type context does without
        // DWARF, so source projection must use the image's naming owner.
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
        Options.LiftMode = Options.SourceProjection = LLVM;
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
        bool OriginalTarget = false, SourceTarget = false;
        for (const auto &Function : Result.LowFuncs)
          if (Function.Entry == 0x10100)
            for (const auto &Block : Function.Blocks)
              for (const auto &Op : Block.Ops)
                OriginalTarget |= Op.Opcode == NdOp::CALL && Op.NumInputs &&
                                  Op.Inputs[0].isConst() &&
                                  Op.Inputs[0].Offset == 0x10160;
        for (const auto &Function : Result.MedFuncs)
          if (Function.Entry == 0x10100)
            for (const auto &Call : Function.CallInfos)
              SourceTarget |=
                  Call.TargetAddr == 0x10140 && Call.TargetName == "fp_callee";
        EXPECT_TRUE(OriginalTarget);
        EXPECT_TRUE(SourceTarget);
        if (Together) {
          const auto Low = llvm::find_if(Result.LowFuncs, [](const auto &F) {
            return F.Entry == 0x10160;
          });
          const auto Med = llvm::find_if(Result.MedFuncs, [](const auto &F) {
            return F.Entry == 0x10160;
          });
          ASSERT_NE(Low, Result.LowFuncs.end());
          ASSERT_NE(Med, Result.MedFuncs.end());
          EXPECT_TRUE(hasMIPSSourceReturnTargets(*Low, *Med, GetParam(),
                                                 BinaryFormat::ELF));
          for (uint64_t Register :
               {mipsreg::RA, mipsreg::SP, mipsreg::RA + 1}) {
            auto Changed = *Low;
            Changed.Blocks.front().Ops.front().Output = NdVar::reg(Register, 4);
            EXPECT_FALSE(hasMIPSSourceReturnTargets(Changed, *Med, GetParam(),
                                                    BinaryFormat::ELF));
          }
          auto Unbound = *Med;
          for (auto &Block : Unbound.Blocks)
            for (auto &Op : Block.Ops)
              if (Op.IsTailCall)
                Op.ScalarCallSignature.reset();
          EXPECT_FALSE(hasMIPSSourceReturnTargets(*Low, Unbound, GetParam(),
                                                  BinaryFormat::ELF));
        }
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
        EXPECT_NE(Text.find("fp_callee("), std::string::npos) << Text;
        EXPECT_EQ(Text.find("__fn_stub_fp_callee("), std::string::npos) << Text;
        if (Together)
          EXPECT_NE(Text.find("neverd_mips16_entry_fp_callee("),
                    std::string::npos)
              << Text;
      }
  }
}

TEST_P(MIPS16Pipeline, EntryAdapterProofChecksEveryParameterByteAndEffect) {
  enum class Change {
    None,
    WrongFloatRegister,
    WrongHighWord,
    WriteStackPointer,
    WriteReturnAddress,
    Store,
    Load,
    Call,
    MissingSlot,
    MissingModeTag,
    MissingDeclaration,
    UnauthenticatedDeclaration,
    Variadic,
    FloatingResult,
    ExistingEntryDeclaration
  };
  for (bool Double : {false, true})
    for (bool FP64 : {false, true})
      for (Change C :
           {Change::None, Change::WrongFloatRegister, Change::WrongHighWord,
            Change::WriteStackPointer, Change::WriteReturnAddress,
            Change::Store, Change::Load, Change::Call, Change::MissingSlot,
            Change::MissingModeTag, Change::MissingDeclaration,
            Change::UnauthenticatedDeclaration, Change::Variadic,
            Change::FloatingResult, Change::ExistingEntryDeclaration}) {
        if (!Double && C == Change::WrongHighWord)
          continue;
        SCOPED_TRACE(Double);
        SCOPED_TRACE(FP64);
        SCOPED_TRACE(int(C));
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
        // Independently compiled GCC12.4 adapter words. A double's low word
        // occupies a0 on little endian and a1 on big endian. FR=1 must read
        // the high half of f12; FR=0 reads the low half of f13 instead.
        const bool Little = GetParam() == Arch::MIPS32EL;
        const unsigned Low = Little ? 4 : 5, High = Little ? 5 : 4;
        const uint32_t First = 0x44006000 | ((Double ? Low : 4) << 16);
        const uint32_t Second = (FP64 ? 0x44606000 : 0x44006800) | (High << 16);
        std::vector<uint32_t> Words = {0x3c190001, 0x27390141};
        if (Double)
          Words.push_back(First);
        Words.push_back(0x03200008);
        Words.push_back(Double ? Second : First);
        switch (C) {
        case Change::WrongFloatRegister:
          Words[Double ? 2 : 3] += 2 << 11;
          break;
        case Change::WrongHighWord:
          Words.back() = 0x44006000 | (High << 16);
          break;
        case Change::WriteStackPointer:
          Words.insert(Words.end() - 2, 0x27bdfff8);
          break;
        case Change::WriteReturnAddress:
          Words.insert(Words.end() - 2, 0x27ff0004);
          break;
        case Change::Store:
          Words.insert(Words.end() - 2, 0xafa40000);
          break;
        case Change::Load:
          Words.insert(Words.end() - 2, 0x8fa60000);
          break;
        case Change::Call:
          Words[Words.size() - 2] = 0x0320f809;
          break;
        case Change::MissingSlot:
          Words.pop_back();
          break;
        case Change::MissingModeTag:
          --Words[1];
          break;
        default:
          break;
        }
        for (size_t I = 0; I < Words.size(); ++I)
          File.put(0x100 + I * 4, Words[I], 4);
        ELFLoader Loader;
        auto Image = Loader.loadBuffer(File.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        Image->Symbols.push_back(Symbol::makeFunc(0x10100, Words.size() * 4));
        Image->Symbols.push_back(Symbol::makeFunc(0x10140, 6));
        Image->MIPS->CodeModeEntries.emplace(0x10100,
                                             InstructionMode::MIPSClassic);
        Image->MIPS->CodeModeEntries.emplace(0x10140, InstructionMode::MIPS16);
        Image->MIPS->CodeRegions.push_back({0x10100, 0x10100 + Words.size() * 4,
                                            InstructionMode::MIPSClassic});
        Image->MIPS->CodeRegions.push_back(
            {0x10140, 0x10146, InstructionMode::MIPS16});
        class BodyDebug final : public NullDebugContext {
        public:
          bool Double;
          Change C;
          BodyDebug(bool D, Change X) : Double(D), C(X) {}
          std::optional<FunctionSym>
          resolveFunction(va_t Address) const override {
            if (C == Change::MissingDeclaration ||
                (Address != 0x10140 &&
                 !(C == Change::ExistingEntryDeclaration &&
                   Address == 0x10100)))
              return std::nullopt;
            FunctionSym F;
            F.Name = "declared_body";
            F.Addr = Address;
            F.ReturnType = C == Change::FloatingResult
                               ? NdType::makeFloat(4)
                               : NdType::makeInt(4, false);
            F.Params = {{"value", NdType::makeFloat(Double ? 8 : 4)},
                        {"mask", NdType::makeInt(4, false)}};
            F.IsVariadic = C == Change::Variadic;
            if (C != Change::UnauthenticatedDeclaration)
              F.PrototypeLibrary = UserPrototypeLibrary.str();
            return F;
          }
        } Debug(Double, C);
        const auto Proof =
            pipeline_detail::proveMIPS16EntryAdapter(*Image, 0x10100, Debug);
        EXPECT_EQ(bool(Proof), C == Change::None);
        if (Proof) {
          EXPECT_EQ(Proof->Body, 0x10140u);
          EXPECT_FALSE(Proof->EntryArguments.MIPS16Parameters);
          EXPECT_TRUE(Proof->BodyArguments.MIPS16Parameters);
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
