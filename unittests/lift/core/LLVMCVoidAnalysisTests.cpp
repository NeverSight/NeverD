//===- LLVMCVoidAnalysisTests.cpp - LLVM C return inference tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"
#include "neverd/backend/llvm/LLVMX86FPStateAsm.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/ADT/ArrayRef.h"
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

void compileAndRun(const std::string &Source,
                   llvm::ArrayRef<llvm::StringRef> TargetFlags = {}) {
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
  llvm::SmallVector<llvm::StringRef, 12> Arguments{Compiler,
                                                   "-std=c11",
                                                   "-O2",
                                                   "-Werror=uninitialized",
                                                   "-Werror=return-type",
                                                   SourcePath,
                                                   "-o",
                                                   BinaryPath};
  Arguments.append(TargetFlags.begin(), TargetFlags.end());
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

TEST(LLVMCVoidAnalysis, ReturnsNoValueOnlyWhereMedIRSettledIt) {
  // The IR folds the undefined register a function hands back to the same
  // constant as a deliberate zero, so whether a function returns a value is
  // settled on MedIR (settleReturnContracts) and read from its mark: a
  // returned call result or zero is a value unless the function is marked.
  llvm::LLVMContext Context;
  llvm::Module Module("value-producer-void-analysis", Context);
  llvm::Function *Returned = defineReturnedProducer(Module);
  llvm::Function *Discarded = defineDiscardedProducer(Module);
  llvm::Function *Unmarked = defineUnmarkedCallResidual(Module);
  neverd::LLVMCAnalysisState State;

  EXPECT_FALSE(neverd::analyzeVoidReturn(State, *Returned));
  EXPECT_FALSE(neverd::analyzeVoidReturn(State, *Discarded));
  EXPECT_FALSE(neverd::analyzeVoidReturn(State, *Unmarked));
  neverd::llvm_value_provenance::markReturnsNoValue(*Discarded);
  EXPECT_TRUE(neverd::analyzeVoidReturn(State, *Discarded));
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

TEST(LLVMCVoidAnalysis, FreezeOfLoadedDigitRecurrenceHasTotalBitvectorC) {
  llvm::LLVMContext Context;
  llvm::Module Module("freeze-digit-loop", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I8 = llvm::Type::getInt8Ty(Context);
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I32, {Pointer, I64}, false),
      llvm::GlobalValue::ExternalLinkage, "digits", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateBr(Loop);
  Builder.SetInsertPoint(Loop);
  auto *Index = Builder.CreatePHI(I64, 2);
  auto *Accumulator = Builder.CreatePHI(I32, 2);
  Index->addIncoming(Builder.getInt64(0), Entry);
  Accumulator->addIncoming(Builder.getInt32(0), Entry);
  auto *Digit = Builder.CreateZExt(
      Builder.CreateLoad(I8, Builder.CreateGEP(I8, Function->getArg(0), Index)),
      I64);
  auto *Product = Builder.CreateShl(
      Builder.CreateZExt(Builder.CreateMul(Accumulator, Builder.getInt32(5)),
                         I64),
      Builder.getInt64(1), "scaled", true, true);
  auto *Sum = Builder.CreateAdd(Digit, Product, "sum", true, true);
  auto *Next = Builder.CreateSub(
      Builder.CreateTrunc(Builder.CreateFreeze(Sum, "chosen"), I32),
      Builder.getInt32(48));
  auto *NextIndex = Builder.CreateAdd(Index, Builder.getInt64(1));
  Index->addIncoming(NextIndex, Loop);
  Accumulator->addIncoming(Next, Loop);
  Builder.CreateCondBr(Builder.CreateICmpULT(NextIndex, Function->getArg(1)),
                       Loop, Exit);
  Builder.SetInsertPoint(Exit);
  Builder.CreateRet(Next);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  const unsigned char text[] = "429496729612345678901234567890123456789";
  uint32_t expected = 0;
  for (uint64_t n = 1; n < sizeof(text); ++n) {
    expected = expected * UINT32_C(10) + (uint32_t)(text[n - 1] - '0');
    if (digits((void *)text, n) != expected) return 1;
  }
  return 0;
}
)");
}

TEST(LLVMCVoidAnalysis,
     FreezeOfDivisionRecurrenceRequiresDefinedDivisionInputs) {
  for (bool FreezeInputs : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("freeze-division-loop", Context);
    Module.setDataLayout("e-p:64:64");
    auto *I32 = llvm::Type::getInt32Ty(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(I32, {I32, I32}, false),
        llvm::GlobalValue::ExternalLinkage, "division_loop", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
    auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
    auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
    llvm::IRBuilder<> Builder(Entry);
    Builder.CreateBr(Loop);
    Builder.SetInsertPoint(Loop);
    auto *Index = Builder.CreatePHI(I32, 2);
    auto *Value = Builder.CreatePHI(I32, 2);
    Index->addIncoming(Builder.getInt32(0), Entry);
    Value->addIncoming(Function->getArg(0), Entry);
    llvm::Value *Input =
        Builder.CreateMul(Value, Builder.getInt32(3), "input", false, true);
    if (FreezeInputs)
      Input = Builder.CreateFreeze(Input);
    auto *Quotient = Builder.CreateSDiv(Input, Builder.getInt32(10));
    auto *Next = Builder.CreateAdd(Quotient, Builder.getInt32(5));
    auto *Chosen = Builder.CreateFreeze(Next);
    auto *NextIndex = Builder.CreateAdd(Index, Builder.getInt32(1));
    Index->addIncoming(NextIndex, Loop);
    Value->addIncoming(Chosen, Loop);
    Builder.CreateCondBr(Builder.CreateICmpULT(NextIndex, Function->getArg(1)),
                         Loop, Exit);
    Builder.SetInsertPoint(Exit);
    Builder.CreateRet(Chosen);
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (!FreezeInputs) {
      EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                   std::runtime_error);
      continue;
    }
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
    compileAndRun(Source + R"(
int main(void) {
  const uint32_t values[] = {0, 1, 0x80000000U, 0x7fffffffU, 0xffffffffU};
  for (unsigned i = 0; i < 5; ++i) {
    uint32_t value = values[i];
    for (unsigned n = 1; n < 20; ++n) {
      value = (uint32_t)((int32_t)(value * 3U) / 10) + 5U;
      if ((uint32_t)division_loop(values[i], n) != value) return 1;
    }
  }
  return 0;
}
)");
  }
}

TEST(LLVMCVoidAnalysis,
     FreezeThroughComparisonKeepsDefinedChoiceAndRejectsPoison) {
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

TEST(LLVMCVoidAnalysis, WholeModuleCollectsFPHelpersFromWindowsMetadataBodies) {
  using namespace neverd;
  llvm::LLVMContext Context;
  llvm::Module Module("windows-fp-helpers", Context);
  Module.setDataLayout("e-p:64:64");
  auto *Double = llvm::Type::getDoubleTy(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Type =
      llvm::FunctionType::get(Double, {Double, Double, Pointer}, false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "fp_body", Module);
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Assembly = llvm::InlineAsm::get(
      Type, x86FPStateBinaryAsm(Intrinsic::X86FPAddState, 8),
      X86FPStateBinaryConstraints, true);
  auto *Call =
      Builder.CreateCall(Assembly, {Function->getArg(0), Function->getArg(1),
                                    Function->getArg(2)});
  Call->setMetadata(X86FPStateAsmMetadata, llvm::MDNode::get(Context, {}));
  Builder.CreateRet(Call);
  Function->setMetadata(
      windows_eh_md::NativeAttachment,
      llvm::MDNode::get(Context,
                        {llvm::ConstantAsMetadata::get(Builder.getInt1(true)),
                         llvm::MDString::get(Context, "seh-x64-native")}));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(LLVMCEmitter().emit(Module, Out, Options));
  llvm::SmallVector<llvm::StringRef, 2> TargetFlags;
#if defined(__APPLE__) && defined(__aarch64__)
  // Apple Silicon runs this masked SSE control through Rosetta. Native Intel
  // and unmasked SSE exception behavior remain unverified.
  TargetFlags = {"-arch", "x86_64"};
#endif
  compileAndRun(Source + R"(
int main(void) {
  uint32_t initial, state = 0x1f80;
  __asm__ volatile("stmxcsr %0" : "=m"(initial));
  double value = fp_body(1.5, 2.25, &state);
  __asm__ volatile("ldmxcsr %0" :: "m"(initial));
  return value != 3.75 || state != 0x1f80;
}
)",
                TargetFlags);
}

} // namespace
