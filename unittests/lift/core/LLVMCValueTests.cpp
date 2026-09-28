//===- LLVMCValueTests.cpp - Executable LLVM C value semantics
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/CTypeFormat.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <stdexcept>

namespace {

void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O2") {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-value", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  const llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      Optimization,
      "-Werror=uninitialized",
      "-Werror=return-type",
      SourcePath,
      "-o",
      BinaryPath};
  std::string Error;
  int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                         Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Result, 0) << Error << (Errors ? (*Errors)->getBuffer().str() : "")
                       << '\n'
                       << Source;
  Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                     Redirects, 30, 0, &Error);
  ASSERT_EQ(Result, 0) << Error << '\n' << Source;
}

TEST(LLVMCValues, EscapedByteStringsPreserveAdjacentDigitsWhenExecuted) {
  std::string Bytes;
  for (unsigned Byte = 0; Byte < 256; ++Byte)
    for (char Digit : llvm::StringRef("0123456789abcdefABCDEF")) {
      Bytes += static_cast<char>(Byte);
      Bytes += Digit;
    }
  Bytes += "?"
           "?/"
           "?"
           "?=";
  std::string Source = "static const unsigned char actual[] = \"" +
                       neverd::escapeCString(Bytes) + "\";\n";
  Source += "static const unsigned char expected[] = {";
  for (unsigned char Byte : Bytes)
    Source += std::to_string(Byte) + ",";
  Source += "};\nint main(void) {\n"
            "if (sizeof(actual) != sizeof(expected) + 1) return 1;\n"
            "for (unsigned i = 0; i < sizeof(expected); ++i)\n"
            "  if (actual[i] != expected[i]) return 2;\n"
            "return actual[sizeof(expected)] != 0;\n}\n";
  compileAndRun(Source);
}

TEST(LLVMCValues, WideIntegerConstantsRetainBothHalvesWhenExecuted) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-constants", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context),
      {llvm::Type::getInt1Ty(Context), Pointer, Pointer}, false);
  std::string Main = "int main(void) { __uint128_t actual, copy;\n";
  unsigned Index = 0;
  for (unsigned Width : {65u, 96u, 128u}) {
    for (uint64_t High :
         {uint64_t(0), uint64_t(1), uint64_t(1) << 63, ~uint64_t(0)}) {
      llvm::APInt Value(128, High);
      Value = ((Value << 64) | llvm::APInt(128, 0xfedcba9876543210ULL))
                  .trunc(Width);
      std::string Name = "constant" + std::to_string(Index++);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      // Keep a nonstandard-width local value live without changing the width
      // of a memory access: both observable stores are exactly 128 bits.
      auto *Chosen = Builder.CreateSelect(
          Function->getArg(0), llvm::ConstantInt::get(Context, Value),
          llvm::ConstantInt::get(Context, llvm::APInt(Width, 0)), "chosen");
      for (unsigned Argument : {1u, 2u})
        Builder.CreateStore(
            Builder.CreateZExtOrTrunc(Chosen, Builder.getInt128Ty()),
            Function->getArg(Argument));
      Builder.CreateRetVoid();
      auto Extended = Value.zextOrTrunc(128);
      Main += "actual = copy = 0; " + Name + "(1, &actual, &copy);\n";
      Main += "if ((uint64_t)actual != " +
              std::to_string(Extended.extractBitsAsZExtValue(64, 0)) +
              "ULL || (uint64_t)(actual >> 64) != " +
              std::to_string(Extended.extractBitsAsZExtValue(64, 64)) +
              "ULL || actual != copy) return " + std::to_string(Index) + ";\n";
      Main += Name + "(0, &actual, &copy);\n";
      Main += "if (actual || copy) return 100;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, FoldedIntegerViewsAndArithmeticKeepTheirWidths) {
  llvm::LLVMContext Context;
  llvm::Module Module("folded-integer-widths", Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::PointerType::getUnqual(Context)}, false);
  std::string Main = "int main(void) { __uint128_t actual;\n";
  enum class Case {
    ZeroExtend,
    SignExtend,
    Truncate,
    Add,
    Subtract,
    SignedAdd
  };
  unsigned Index = 0;
  for (unsigned Width : {8u, 16u, 32u, 64u}) {
    for (Case Kind : {Case::ZeroExtend, Case::SignExtend, Case::Truncate,
                      Case::Add, Case::Subtract, Case::SignedAdd}) {
      const std::string Name = "folded" + std::to_string(Index++);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      // Keep the casts and arithmetic as instructions so the C emitter owns
      // their folding; IRBuilder's normal constant folder would hide a bug.
      llvm::IRBuilder<llvm::NoFolder> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Type = Builder.getIntNTy(Width);
      const llvm::APInt Ones = llvm::APInt::getAllOnes(Width);
      auto Constant = [&](const llvm::APInt &Bits) {
        return llvm::ConstantInt::get(Context, Bits);
      };
      llvm::Value *Value = nullptr;
      llvm::APInt Expected(Width, 0);
      bool Signed = false;
      switch (Kind) {
      case Case::ZeroExtend:
        Value = Constant(Ones);
        Expected = Ones;
        break;
      case Case::SignExtend:
        Expected = llvm::APInt::getSignedMinValue(Width);
        Value = Constant(Expected);
        Signed = true;
        break;
      case Case::Truncate: {
        const llvm::APInt Wide(128,
                               {0x1234567812345600ULL, 0xfedcba9876543210ULL});
        Value = Builder.CreateTrunc(Constant(Wide), Type, "narrowed");
        Expected = Wide.trunc(Width);
        break;
      }
      case Case::Add:
        Value = Builder.CreateAdd(
            Constant(Ones), Constant(llvm::APInt(Width, 1)), "wrapped_add");
        break;
      case Case::Subtract:
        Value = Builder.CreateSub(Constant(llvm::APInt(Width, 0)),
                                  Constant(llvm::APInt(Width, 1)),
                                  "wrapped_subtract");
        Expected = Ones;
        break;
      case Case::SignedAdd:
        Value = Builder.CreateAdd(
            Constant(llvm::APInt::getSignedMaxValue(Width)),
            Constant(llvm::APInt(Width, 1)), "signed_boundary");
        Expected = llvm::APInt::getSignedMinValue(Width);
        Signed = true;
        break;
      }
      Value = Signed ? Builder.CreateSExtOrTrunc(Value, Builder.getInt64Ty())
                     : Builder.CreateZExtOrTrunc(Value, Builder.getInt64Ty());
      Value = Signed ? Builder.CreateSExt(Value, Builder.getInt128Ty())
                     : Builder.CreateZExt(Value, Builder.getInt128Ty());
      Builder.CreateStore(Value, Function->getArg(0));
      Builder.CreateRetVoid();
      Expected = Signed ? Expected.sext(128) : Expected.zext(128);
      Main += Name + "(&actual);\n";
      Main += "if ((uint64_t)actual != " +
              std::to_string(Expected.extractBitsAsZExtValue(64, 0)) +
              "ULL || (uint64_t)(actual >> 64) != " +
              std::to_string(Expected.extractBitsAsZExtValue(64, 64)) +
              "ULL) return " + std::to_string(Index) + ";\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, RuntimeIntegerCastChainsKeepTruncationAndSignedness) {
  llvm::LLVMContext Context;
  llvm::Module Module("runtime-integer-widths", Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Wide = llvm::Type::getInt128Ty(Context);
  auto *Signature = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context), {Wide, Pointer, Pointer, Pointer}, false);
  std::string Main = "int main(void) { __uint128_t zero, sign, copy;\n";
  auto Literal = [](const llvm::APInt &Value) {
    return "(((__uint128_t)" +
           std::to_string(Value.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(Value.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  for (unsigned Width :
       {1u, 7u, 8u, 9u, 16u, 31u, 32u, 63u, 64u, 65u, 96u, 127u}) {
    const std::string Name = "cast" + std::to_string(Width);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<llvm::NoFolder> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Narrowed = Builder.CreateTrunc(Function->getArg(0),
                                         Builder.getIntNTy(Width), "narrowed");
    auto *Zero = Builder.CreateZExt(Narrowed, Wide, "zero_extended");
    auto *Sign = Builder.CreateSExt(Narrowed, Wide, "sign_extended");
    Builder.CreateStore(Zero, Function->getArg(1));
    Builder.CreateStore(Sign, Function->getArg(2));
    Builder.CreateStore(Zero, Function->getArg(3));
    Builder.CreateRetVoid();
    for (const llvm::APInt &Input :
         {llvm::APInt(128, 0), llvm::APInt::getAllOnes(128),
          llvm::APInt::getOneBitSet(128, Width - 1),
          llvm::APInt::getOneBitSet(128, Width)}) {
      const llvm::APInt Bits = Input.trunc(Width);
      Main += Name + "(" + Literal(Input) + ", &zero, &sign, &copy);\n";
      Main += "if (zero != " + Literal(Bits.zext(128)) +
              " || sign != " + Literal(Bits.sext(128)) +
              " || copy != zero) return 1;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, SwitchCasesPreserveTheirSelectorBitPattern) {
  llvm::LLVMContext Context;
  llvm::Module Module("switch-bit-patterns", Context);
  auto *Wide = llvm::Type::getInt128Ty(Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), {Wide}, false);
  auto Literal = [](const llvm::APInt &Value) {
    const auto WideValue = Value.zextOrTrunc(128);
    return "(((__uint128_t)" +
           std::to_string(WideValue.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(WideValue.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  std::string Main = "int main(void) {\n";
  for (unsigned Width :
       {1u, 2u, 7u, 8u, 9u, 16u, 31u, 32u, 63u, 64u, 65u, 96u, 127u, 128u}) {
    const std::string Name = "switch_bits" + std::to_string(Width);
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Default = llvm::BasicBlock::Create(Context, "otherwise", Function);
    llvm::IRBuilder<llvm::NoFolder> Builder(Entry);
    auto *Selector = Builder.CreateZExtOrTrunc(
        Function->getArg(0), Builder.getIntNTy(Width), "selector");
    auto *Switch = Builder.CreateSwitch(Selector, Default);
    const auto HighBit = llvm::APInt::getSignedMinValue(Width);
    const auto Ones = llvm::APInt::getAllOnes(Width);
    const auto AddCase = [&](const llvm::APInt &Value, unsigned Result) {
      auto *Block = llvm::BasicBlock::Create(Context, "matched", Function);
      Switch->addCase(llvm::ConstantInt::get(Context, Value), Block);
      Builder.SetInsertPoint(Block);
      Builder.CreateRet(Builder.getInt32(Result));
    };
    AddCase(llvm::APInt(Width, 0), 11);
    AddCase(HighBit, 23);
    if (HighBit != Ones)
      AddCase(Ones, 37);
    Builder.SetInsertPoint(Default);
    Builder.CreateRet(Builder.getInt32(49));

    // Exercise the sign boundary, every i2 value, the default arm, and upper
    // input bits discarded by the selector. Expected tags use APInt equality,
    // independently of the C case-label spelling and integer promotions.
    for (const llvm::APInt &Input :
         {llvm::APInt(128, 0), llvm::APInt(128, 1), llvm::APInt(128, 2),
          llvm::APInt(128, 3), HighBit.zextOrTrunc(128),
          (HighBit - 1).zextOrTrunc(128), (HighBit + 1).zextOrTrunc(128),
          Ones.zextOrTrunc(128), llvm::APInt::getAllOnes(128)}) {
      const auto Bits = Input.zextOrTrunc(Width);
      const unsigned Expected = Bits.isZero()     ? 11
                                : Bits == HighBit ? 23
                                : Bits == Ones    ? 37
                                                  : 49;
      Main += "if (" + Name + "(" + Literal(Input) +
              ") != " + std::to_string(Expected) + ") return 1;\n";
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, EqualityPreservesNarrowBitsAcrossEveryComparisonSurface) {
  llvm::LLVMContext Context;
  llvm::Module Module("comparison-bit-patterns", Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I128 = llvm::Type::getInt128Ty(Context);
  auto *Signature = llvm::FunctionType::get(
      I32, {I128, llvm::PointerType::getUnqual(Context)}, false);
  auto Literal = [](const llvm::APInt &Value) {
    const auto Wide = Value.zextOrTrunc(128);
    return "(((__uint128_t)" +
           std::to_string(Wide.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(Wide.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  std::string Main = "int main(void) { uint32_t observed;\n";
  unsigned Index = 0;
  for (unsigned Width : {1u, 2u, 3u, 8u, 16u, 31u, 33u, 65u, 127u}) {
    const auto High = llvm::APInt::getSignedMinValue(Width);
    for (const auto &Constant : {llvm::APInt(Width, 0), High})
      for (bool Equal : {false, true})
        for (bool Reversed : {false, true})
          for (unsigned Surface = 0; Surface != 3; ++Surface) {
            const std::string Name = "compare_bits" + std::to_string(Index++);
            auto *Function = llvm::Function::Create(
                Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
            llvm::IRBuilder<llvm::NoFolder> Builder(
                llvm::BasicBlock::Create(Context, "entry", Function));
            auto *Type = Builder.getIntNTy(Width);
            auto *Narrow = Builder.CreateTrunc(Function->getArg(0), Type);
            // Keep modular wrap visible to comparison spelling, including
            // the EQ/NE-zero condition shortcuts.
            auto *Wrapped = Builder.CreateAdd(
                Narrow, llvm::ConstantInt::get(Type, 1), "wrapped");
            llvm::Value *LHS = Wrapped;
            llvm::Value *RHS = llvm::ConstantInt::get(Context, Constant);
            if (Reversed)
              std::swap(LHS, RHS);
            auto *Compare = Equal ? Builder.CreateICmpEQ(LHS, RHS)
                                  : Builder.CreateICmpNE(LHS, RHS);
            if (Surface == 2) {
              // Only the false edge has an effect. The structured writer
              // renders an inverted condition before the shared return.
              auto *Done = llvm::BasicBlock::Create(Context, "done", Function);
              auto *Miss = llvm::BasicBlock::Create(Context, "miss", Function);
              Builder.CreateCondBr(Compare, Done, Miss);
              Builder.SetInsertPoint(Miss);
              Builder.CreateStore(Builder.getInt32(13), Function->getArg(1));
              Builder.CreateBr(Done);
              Builder.SetInsertPoint(Done);
              Builder.CreateRet(Builder.getInt32(41));
            } else {
              llvm::Value *Selected = Builder.CreateSelect(
                  Compare, Builder.getInt32(41), Builder.getInt32(13));
              if (Surface == 1)
                // Two uses force an assigned ICmp instead of inline text.
                Selected = Builder.CreateAdd(Selected,
                                             Builder.CreateZExt(Compare, I32));
              Builder.CreateRet(Selected);
            }
            for (const auto &Input :
                 {llvm::APInt(128, 0), llvm::APInt(128, 1), llvm::APInt(128, 2),
                  llvm::APInt(128, 3), llvm::APInt(128, 7), High.zext(128),
                  (High - 1).zext(128), (High + 1).zext(128),
                  llvm::APInt::getAllOnes(128)}) {
              const auto Bits = Input.trunc(Width) + 1;
              const bool Matches = (Bits == Constant) == Equal;
              const unsigned Returned = Surface == 2 ? 41
                                        : Matches    ? (Surface == 1 ? 42 : 41)
                                                     : 13;
              const unsigned Stored = Surface == 2 && !Matches ? 13 : 41;
              Main += "observed = 41; if (" + Name + "(" + Literal(Input) +
                      ", &observed) != " + std::to_string(Returned) +
                      " || observed != " + std::to_string(Stored) +
                      ") return 1;\n";
            }
          }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization.str());
    compileAndRun(Source + Main, Optimization);
  }
}

TEST(LLVMCValues, BooleanExtensionDoesNotPeelArithmeticNormalization) {
  llvm::LLVMContext Context;
  llvm::Module Module("boolean-arithmetic-view", Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *Signature = llvm::FunctionType::get(I32, {I32}, false);
  std::string Main = "int main(void) { for (uint32_t x = 0; x < 16; ++x) {\n";
  for (bool Equal : {false, true}) {
    const std::string Name = Equal ? "wrapped_eq" : "wrapped_ne";
    auto *Function = llvm::Function::Create(
        Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
    llvm::IRBuilder<llvm::NoFolder> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Sum = Builder.CreateAdd(
        Builder.CreateTrunc(Function->getArg(0), Builder.getInt1Ty()),
        Builder.getInt1(true));
    auto *Extended = Builder.CreateZExt(Sum, Builder.getInt8Ty());
    auto *Compare = Equal ? Builder.CreateICmpEQ(Extended, Builder.getInt8(0))
                          : Builder.CreateICmpNE(Extended, Builder.getInt8(0));
    auto *True = llvm::BasicBlock::Create(Context, "true", Function);
    auto *False = llvm::BasicBlock::Create(Context, "false", Function);
    Builder.CreateCondBr(Compare, True, False);
    Builder.SetInsertPoint(True);
    Builder.CreateRet(Builder.getInt32(1));
    Builder.SetInsertPoint(False);
    Builder.CreateRet(Builder.getInt32(0));
    Main += "if (" + Name + "(x) != " + (Equal ? "(x & 1)" : "((x & 1) ^ 1)") +
            ") return 1;\n";
  }
  Main += "} return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, BooleanFlagViewsKeepOneObservableCall) {
  llvm::LLVMContext Context;
  llvm::Module Module("boolean-call-effects", Context);
  auto *I8 = llvm::Type::getInt8Ty(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Next = llvm::Function::Create(llvm::FunctionType::get(I64, {}, false),
                                      llvm::GlobalValue::ExternalLinkage,
                                      "next_value", Module);
  for (bool Invert : {false, true}) {
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(I32, {}, false),
        llvm::GlobalValue::ExternalLinkage,
        Invert ? "inverted_flags" : "direct_flags", Module);
    llvm::IRBuilder<llvm::NoFolder> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Home = Builder.CreateAlloca(I32);
    Builder.CreateStore(Builder.CreateTrunc(Builder.CreateCall(Next), I32),
                        Home);
    auto *Word = Builder.CreateAnd(Builder.CreateLoad(I32, Home),
                                   Builder.CreateLoad(I32, Home));
    auto *Zero = Builder.CreateICmpEQ(Word, Builder.getInt32(0));
    auto *Sign = Builder.CreateICmpSLT(Word, Builder.getInt32(0));
    auto *Flags = Builder.CreateOr(Builder.CreateZExt(Zero, I8),
                                   Builder.CreateZExt(Sign, I8));
    auto *Test = Invert ? Builder.CreateICmpEQ(Flags, Builder.getInt8(0))
                        : Builder.CreateICmpNE(Flags, Builder.getInt8(0));
    auto *True = llvm::BasicBlock::Create(Context, "true", Function);
    auto *False = llvm::BasicBlock::Create(Context, "false", Function);
    Builder.CreateCondBr(Test, True, False);
    Builder.SetInsertPoint(True);
    Builder.CreateRet(Builder.getInt32(1));
    Builder.SetInsertPoint(False);
    Builder.CreateRet(Builder.getInt32(0));
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  const std::string Main = R"(
static uint64_t supplied;
static unsigned calls;
uint64_t next_value(void) { ++calls; return supplied; }
int main(void) {
  const uint64_t values[] = {0, 1, 0x7fffffff, 0x80000000, 0xffffffff,
                            0x100000000ULL, 0x180000000ULL};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    supplied = values[i];
    uint32_t bits = (uint32_t)supplied;
    unsigned nonpositive = bits == 0 || (bits & 0x80000000U) != 0;
    calls = 0;
    if (direct_flags() != nonpositive || calls != 1) return 1;
    calls = 0;
    if (inverted_flags() != !nonpositive || calls != 1) return 2;
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + Main, Optimization);
}

TEST(LLVMCValues, OrderedComparisonsUseTheirLLVMWidthAndSignedness) {
  llvm::LLVMContext Context;
  llvm::Module Module("ordered-comparison-widths", Context);
  auto *I128 = llvm::Type::getInt128Ty(Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), {I128}, false);
  auto Literal = [](const llvm::APInt &Value) {
    const auto Wide = Value.zextOrTrunc(128);
    return "(((__uint128_t)" +
           std::to_string(Wide.extractBitsAsZExtValue(64, 64)) +
           "ULL << 64) | " +
           std::to_string(Wide.extractBitsAsZExtValue(64, 0)) + "ULL)";
  };
  std::string Main = "int main(void) {\n";
  unsigned Index = 0;
  for (unsigned Width :
       {1u, 2u, 3u, 8u, 16u, 31u, 32u, 33u, 64u, 65u, 127u, 128u}) {
    const auto High = llvm::APInt::getSignedMinValue(Width);
    for (auto Predicate : {llvm::CmpInst::ICMP_ULT, llvm::CmpInst::ICMP_UGT,
                           llvm::CmpInst::ICMP_SLT, llvm::CmpInst::ICMP_SGT}) {
      const std::string Name = "ordered_bits" + std::to_string(Index++);
      auto *Function = llvm::Function::Create(
          Signature, llvm::GlobalValue::ExternalLinkage, Name, Module);
      llvm::IRBuilder<llvm::NoFolder> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Value = Builder.CreateZExtOrTrunc(Function->getArg(0),
                                              Builder.getIntNTy(Width));
      Builder.CreateRet(Builder.CreateZExt(
          Builder.CreateICmp(Predicate, Value,
                             llvm::ConstantInt::get(Context, High)),
          Builder.getInt32Ty()));
      for (const auto &Input :
           {llvm::APInt(128, 0), llvm::APInt(128, 1), High.zextOrTrunc(128),
            (High - 1).zextOrTrunc(128), (High + 1).zextOrTrunc(128),
            llvm::APInt::getAllOnes(128)}) {
        const auto Bits = Input.zextOrTrunc(Width);
        const bool Expected =
            Predicate == llvm::CmpInst::ICMP_ULT   ? Bits.ult(High)
            : Predicate == llvm::CmpInst::ICMP_UGT ? Bits.ugt(High)
            : Predicate == llvm::CmpInst::ICMP_SLT ? Bits.slt(High)
                                                   : Bits.sgt(High);
        Main += "if (" + Name + "(" + Literal(Input) +
                ") != " + std::to_string(Expected) + ") return 1;\n";
      }
    }
  }
  Main += "return 0; }\n";
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization.str());
    compileAndRun(Source + Main, Optimization);
  }
}

TEST(LLVMCValues, TruncatedPredicatesTestOnlyTheirLowBits) {
  llvm::LLVMContext Context;
  llvm::Module Module("truncated-predicate", Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::Type::getInt64Ty(Context)}, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "low_bit", Module);
  llvm::IRBuilder<llvm::NoFolder> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Set = llvm::BasicBlock::Create(Context, "set", Function);
  auto *Clear = llvm::BasicBlock::Create(Context, "clear", Function);
  Builder.CreateCondBr(
      Builder.CreateTrunc(Function->getArg(0), Builder.getInt1Ty()), Set,
      Clear);
  Builder.SetInsertPoint(Set);
  Builder.CreateRet(Builder.getInt32(1));
  Builder.SetInsertPoint(Clear);
  Builder.CreateRet(Builder.getInt32(0));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + "int main(void) { return low_bit(0) != 0 || "
                         "low_bit(1) != 1 || low_bit(2) != 0 || "
                         "low_bit(UINT64_MAX) != 1; }\n");
}

TEST(LLVMCValues, ForwardedBitwiseOperandsRetainComparisonGrouping) {
  llvm::LLVMContext Context;
  llvm::Module Module("forwarded-bitwise-comparisons", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(I64, {I64, I64}, false);
  const llvm::Instruction::BinaryOps Operators[] = {
      llvm::Instruction::And, llvm::Instruction::Or, llvm::Instruction::Xor};
  unsigned Index = 0;
  for (auto Operator : Operators) {
    for (bool Equal : {false, true}) {
      for (bool Reversed : {false, true}) {
        auto *Function = llvm::Function::Create(
            Signature, llvm::GlobalValue::ExternalLinkage,
            "grouped" + std::to_string(Index++), Module);
        llvm::IRBuilder<llvm::NoFolder> Builder(
            llvm::BasicBlock::Create(Context, "entry", Function));
        auto *Home = Builder.CreateAlloca(I64);
        Builder.CreateStore(
            Builder.CreateBinOp(Operator, Function->getArg(0),
                                llvm::ConstantInt::get(I64, 0x24)),
            Home);
        llvm::Value *Left = Builder.CreateLoad(I64, Home);
        llvm::Value *Right = Function->getArg(1);
        if (Reversed)
          std::swap(Left, Right);
        auto *Compare = Equal ? Builder.CreateICmpEQ(Left, Right)
                              : Builder.CreateICmpNE(Left, Right);
        Builder.CreateRet(Builder.CreateZExt(Compare, I64));
      }
    }
  }
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  std::string Main = R"(
int main(void) {
  const uint64_t inputs[] = {0, 1, 4, 0x20, 0x24, 0x25, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t x = inputs[i];
    for (unsigned j = 0; j < sizeof(inputs) / sizeof(inputs[0]); ++j) {
      uint64_t y = inputs[j];
)";
  Index = 0;
  for (const char *Operator : {"&", "|", "^"})
    for (const char *Compare : {"!=", "=="})
      for (unsigned Reversed = 0; Reversed != 2; ++Reversed)
        Main += "if (grouped" + std::to_string(Index++) + "(x, y) != ((x " +
                Operator + " 0x24) " + Compare + " y)) return 1;\n";
  Main += "} } return 0; }\n";
  compileAndRun(Source + Main);
}

TEST(LLVMCValues, ForwardedHomesKeepNarrowSignAndWideShiftSemantics) {
  llvm::LLVMContext Context;
  llvm::Module Module("forwarded-home-widths", Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(I64, {I64}, false),
      llvm::GlobalValue::ExternalLinkage, "source_bits", Module);
  llvm::IRBuilder<llvm::NoFolder> CalleeBuilder(
      llvm::BasicBlock::Create(Context, "entry", Callee));
  CalleeBuilder.CreateRet(Callee->getArg(0));

  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {I64, I64, Pointer, Pointer, Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "home_widths", Module);
  llvm::IRBuilder<llvm::NoFolder> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *WideHome = Builder.CreateAlloca(I64, nullptr, "wide_home");
  auto *NarrowHome = Builder.CreateAlloca(I32, nullptr, "narrow_home");
  auto *ReloadHome = Builder.CreateAlloca(I64, nullptr, "reload_home");
  auto *Call = Builder.CreateCall(Callee, {Function->getArg(0)}, "call_bits");
  Builder.CreateStore(Call, WideHome);
  auto *Narrow = Builder.CreateTrunc(Builder.CreateLoad(I64, WideHome), I32);
  Builder.CreateStore(Narrow, NarrowHome);
  auto *Reloaded = Builder.CreateLoad(I32, NarrowHome);
  Builder.CreateStore(Builder.CreateSExt(Reloaded, I64), Function->getArg(2));
  Builder.CreateStore(Builder.CreateZExt(Reloaded, I64), ReloadHome);
  auto *High = Builder.CreateLShr(Builder.CreateLoad(I64, ReloadHome),
                                  llvm::ConstantInt::get(I64, 32));
  Builder.CreateStore(High, Function->getArg(3));
  auto *Other = Builder.CreateTrunc(Function->getArg(1), I32);
  Builder.CreateStore(Builder.CreateAdd(Reloaded, Other), Function->getArg(4));
  Builder.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));

  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t inputs[] = {
      0, 0x7fffffffULL, 0x80000000ULL, 0xffffffffULL,
      0x1234567800000001ULL, 0x1234567880000001ULL, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
    uint64_t sign = 0, high = 1;
    uint32_t sum = 0;
    const uint32_t low = (uint32_t)inputs[i];
    const uint64_t expected_sign =
        low & 0x80000000U ? (uint64_t)low | 0xffffffff00000000ULL : low;
    home_widths(inputs[i], 0xfedcba98ffffffffULL, &sign, &high, &sum);
    if (sign != expected_sign || high != 0 || sum != low - 1U)
      return 1;
  }
  return 0;
}
)");
}

TEST(LLVMCValues, WideIntegerPointerConstantKeepsItsPointerWidth) {
  llvm::LLVMContext Context;
  llvm::Module Module("wide-integer-pointer", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(Pointer, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "wide_pointer", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::APInt Address(128, 1);
  Address = (Address << 96) | llvm::APInt(128, 0x1234);
  Builder.CreateRet(llvm::ConstantExpr::getIntToPtr(
      llvm::ConstantInt::get(Context, Address), Pointer));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(
      Source +
      "int main(void) { return (uintptr_t)wide_pointer() != 0x1234; }\n");
}

TEST(LLVMCValues, ConstantsWiderThanTheCarrierAreRejected) {
  llvm::LLVMContext Context;
  llvm::Module Module("unsupported-constant", Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::PointerType::getUnqual(Context)}, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "wide_store", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  Builder.CreateStore(llvm::ConstantInt::get(Context, llvm::APInt(256, 17)),
                      Function->getArg(0));
  Builder.CreateRetVoid();
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
               std::runtime_error);
}

} // namespace
