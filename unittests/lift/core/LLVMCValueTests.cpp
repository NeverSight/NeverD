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
