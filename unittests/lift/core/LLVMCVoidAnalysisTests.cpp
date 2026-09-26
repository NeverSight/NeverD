//===- LLVMCVoidAnalysisTests.cpp - LLVM C return inference tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
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
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-freeze", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-freeze", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-freeze", "err",
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

llvm::CallInst *emitReturnAddress(llvm::IRBuilder<> &Builder,
                                  bool IsSemanticProducer) {
  llvm::LLVMContext &Context = Builder.getContext();
  auto *PtrTy = llvm::PointerType::getUnqual(Context);
  llvm::CallInst *Call = Builder.CreateIntrinsic(
      llvm::Intrinsic::returnaddress, {PtrTy},
      {llvm::ConstantInt::get(llvm::Type::getInt32Ty(Context), 0)});
  if (IsSemanticProducer)
    neverd::llvm_value_provenance::markSemanticProducer(*Call);
  return Call;
}

llvm::Function *defineReturnedProducer(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *Type = llvm::FunctionType::get(I32Ty, false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "returned_producer", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  llvm::Value *Address =
      Builder.CreatePtrToInt(emitReturnAddress(Builder, true), I32Ty);
  Builder.CreateRet(Address);
  return Function;
}

llvm::Function *defineDiscardedProducer(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *Type = llvm::FunctionType::get(I32Ty, false);
  llvm::Function *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "discarded_producer", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  emitReturnAddress(Builder, true);
  Builder.CreateRet(llvm::ConstantInt::get(I32Ty, 0));
  return Function;
}

llvm::Function *defineUnmarkedCallResidual(llvm::Module &Module) {
  llvm::LLVMContext &Context = Module.getContext();
  auto *I32Ty = llvm::Type::getInt32Ty(Context);
  auto *Type = llvm::FunctionType::get(I32Ty, false);
  llvm::Function *Function =
      llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                             "unmarked_call_residual", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto Callee = Module.getOrInsertFunction("ordinary_call", Type);
  Builder.CreateRet(Builder.CreateCall(Callee));
  return Function;
}

TEST(LLVMCVoidAnalysis, DistinguishesReturnedProducerFromDiscardedCall) {
  llvm::LLVMContext Context;
  llvm::Module Module("value-producer-void-analysis", Context);
  llvm::Function *Returned = defineReturnedProducer(Module);
  llvm::Function *Discarded = defineDiscardedProducer(Module);
  llvm::Function *Unmarked = defineUnmarkedCallResidual(Module);
  neverd::LLVMCAnalysisState State;

  EXPECT_FALSE(neverd::analyzeVoidReturn(State, *Returned));
  EXPECT_TRUE(neverd::analyzeVoidReturn(State, *Discarded));
  EXPECT_TRUE(neverd::analyzeVoidReturn(State, *Unmarked));
}

TEST(LLVMCVoidAnalysis, FreezeMaterializesOneDefinedChoiceForEveryUseCount) {
  llvm::LLVMContext Context;
  llvm::Module Module("freeze-values", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Type = llvm::FunctionType::get(I64, {I64, Pointer}, false);
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    const char *Names[] = {"freeze_single", "freeze_multiple", "freeze_undef",
                           "freeze_poison"};
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, Names[Variant], Module);
    Function->getArg(0)->addAttr(llvm::Attribute::NoUndef);
    Function->getArg(1)->addAttr(llvm::Attribute::NoUndef);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    llvm::Value *Input = Function->getArg(0);
    if (Variant == 2)
      Input = llvm::UndefValue::get(I64);
    if (Variant == 3)
      Input = llvm::PoisonValue::get(I64);
    auto *Frozen = Builder.CreateFreeze(Input, "chosen");
    if (Variant != 0)
      Builder.CreateStore(Frozen, Function->getArg(1));
    Builder.CreateRet(Frozen);
  }
  neverd::CEmitterOptions Options;
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_EQ(Source.find("unhandled:"), std::string::npos) << Source;
  EXPECT_NE(Source.find("chosen0 = arg0;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("chosen0 = 0;"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  uint64_t stored = UINT64_C(17);
  uint64_t value = UINT64_C(0xfedcba9876543210);
  if (freeze_single(value, &stored) != value || stored != 17) return 1;
  if (freeze_multiple(value, &stored) != value || stored != value) return 2;
  stored = 17;
  if (freeze_undef(value, &stored) != 0 || stored != 0) return 3;
  stored = 17;
  if (freeze_poison(value, &stored) != 0 || stored != 0) return 4;
  return 0;
}
)");
}

TEST(LLVMCVoidAnalysis, FreezeRejectsUnprovedPossiblyPoisonArithmetic) {
  for (bool SingleUse : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("unsafe-freeze", Context);
    auto *I64 = llvm::Type::getInt64Ty(Context);
    auto *Type = llvm::FunctionType::get(I64, {I64, I64}, false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "unsafe_freeze", Module);
    for (auto &Argument : Function->args())
      Argument.addAttr(llvm::Attribute::NoUndef);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Frozen = Builder.CreateFreeze(
        Builder.CreateShl(Function->getArg(0), Function->getArg(1)));
    Builder.CreateRet(SingleUse ? Frozen : Builder.CreateAdd(Frozen, Frozen));
    neverd::CEmitterOptions Options;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, Options),
                 std::runtime_error);
  }
}

TEST(LLVMCVoidAnalysis, FreezeThroughComparisonKeepsDefinedChoiceAndRejectsPoison) {
  for (bool MayBePoison : {false, true}) {
    SCOPED_TRACE(MayBePoison);
    llvm::LLVMContext Context;
    llvm::Module Module("freeze-comparison", Context);
    auto *I64 = llvm::Type::getInt64Ty(Context);
    auto *Type = llvm::FunctionType::get(llvm::Type::getInt1Ty(Context),
                                        {I64, I64}, false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "freeze_compare", Module);
    for (auto &Argument : Function->args())
      Argument.addAttr(llvm::Attribute::NoUndef);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    llvm::Value *Input = Function->getArg(0);
    if (MayBePoison)
      Input = Builder.CreateShl(Input, Function->getArg(1));
    auto *Frozen = Builder.CreateFreeze(Input, "chosen");
    Builder.CreateRet(Builder.CreateICmpEQ(Frozen, Builder.getInt64(0)));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (MayBePoison) {
      EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                   std::runtime_error);
    } else {
      ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
      compileAndRun(Source + R"(
int main(void) {
  if (!freeze_compare(0, 0)) return 1;
  if (freeze_compare(UINT64_C(0xfedcba9876543210), 0)) return 2;
  return 0;
}
)");
    }
  }
}

} // namespace
