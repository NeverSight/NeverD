//===- LLVMCValueTests.cpp - Executable LLVM C value semantics
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

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

void compileAndRun(const std::string &Source) {
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
      "-O2",
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
  auto *Signature = llvm::FunctionType::get(
      llvm::Type::getVoidTy(Context),
      {llvm::PointerType::getUnqual(Context)}, false);
  std::string Main = "int main(void) { __uint128_t actual;\n";
  enum class Case { ZeroExtend, SignExtend, Truncate, Add, Subtract, SignedAdd };
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
        const llvm::APInt Wide(128, {0x1234567812345600ULL,
                                     0xfedcba9876543210ULL});
        Value = Builder.CreateTrunc(Constant(Wide), Type, "narrowed");
        Expected = Wide.trunc(Width);
        break;
      }
      case Case::Add:
        Value = Builder.CreateAdd(Constant(Ones), Constant(llvm::APInt(Width, 1)),
                                    "wrapped_add");
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
  compileAndRun(Source +
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
