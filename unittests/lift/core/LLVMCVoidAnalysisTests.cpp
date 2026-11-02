//===- LLVMCVoidAnalysisTests.cpp - LLVM C return inference tests -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/LLVMValueProvenance.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/pass/LLVMC/LLVMCPasses.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86/LLVMX86FPStateAsm.h"

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

TEST(LLVMCVoidAnalysis, PreservedSignaturesKeepNoValueContractsAndEffects) {
  for (bool PreserveTypes : {false, true}) {
    SCOPED_TRACE(PreserveTypes);
    llvm::LLVMContext Context;
    llvm::Module Module("unused-return-carrier", Context);
    auto *I64 = llvm::Type::getInt64Ty(Context);
    auto *Pointer = llvm::PointerType::getUnqual(Context);
    auto *Type = llvm::FunctionType::get(I64, {Pointer}, false);
    auto Effect = Module.getOrInsertFunction("produce_effect", Type);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "no_value", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Call = Builder.CreateCall(Effect, {Function->getArg(0)});
    auto *Unknown = neverd::llvm_source_unknown::create(Builder, I64);
    Builder.CreateRet(Builder.CreateAdd(Call, Unknown));
    neverd::llvm_value_provenance::markReturnsNoValue(*Function);

    neverd::CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = PreserveTypes;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
    EXPECT_EQ(Source.find("unknown machine state"), std::string::npos)
        << Source;
    EXPECT_NE(
        Source.find(PreserveTypes ? "uint64_t no_value(" : "void no_value("),
        std::string::npos)
        << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source + R"(
uint64_t produce_effect(void *address) {
  uint64_t *count = (uint64_t *)address;
  return ++*count;
}
int main(void) {
  uint64_t count = 0;
  no_value(&count);
  no_value(&count);
  return count != 2;
}
)",
                    {Level});

    // Removing the settled contract makes the same return an observation.
    // A preserved signature must not turn arbitrary unknown values into zero.
    Function->setMetadata(
        neverd::llvm_value_provenance::ReturnsNoValueAttachment, nullptr);
    std::string Demanded;
    llvm::raw_string_ostream DemandedOut(Demanded);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, DemandedOut, Options));
    EXPECT_NE(Demanded.find("unknown machine state"), std::string::npos)
        << Demanded;
  }
}

TEST(LLVMCVoidAnalysis, NoValueContractsRetainMemoryEffects) {
  for (bool PreserveTypes : {false, true}) {
    SCOPED_TRACE(PreserveTypes);
    llvm::LLVMContext Context;
    llvm::Module Module("unused-return-memory-effects", Context);
    auto *I64 = llvm::Type::getInt64Ty(Context);
    auto *Pointer = llvm::PointerType::getUnqual(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(I64, {Pointer}, false),
        llvm::GlobalValue::ExternalLinkage, "update", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Read = Builder.CreateLoad(I64, Function->getArg(0));
    Read->setVolatile(true);
    auto *Update =
        Builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, Function->getArg(0),
                                llvm::ConstantInt::get(I64, 1), llvm::Align(8),
                                llvm::AtomicOrdering::SequentiallyConsistent);
    Builder.CreateRet(Builder.CreateAdd(Read, Update));
    neverd::llvm_value_provenance::markReturnsNoValue(*Function);
    neverd::LLVMCAnalysisState State;
    neverd::analyzeVoidDeadChain(State, *Function);
    EXPECT_FALSE(State.DeadFrameStores.count(Read));
    EXPECT_FALSE(State.DeadFrameStores.count(Update));

    neverd::CEmitterOptions Options;
    Options.PreserveLLVMFunctionTypes = PreserveTypes;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
    EXPECT_NE(Source.find("volatile"), std::string::npos) << Source;
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source + R"(
int main(void) {
  uint64_t count = 17;
  update(&count);
  update(&count);
  return count != 19;
}
)",
                    {Level});
  }
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

TEST(LLVMCVoidAnalysis, FreezeOfEveryShiftCountHasAStableDefinedProjection) {
  for (bool SingleUse : {false, true}) {
    for (unsigned Opcode : {llvm::Instruction::Shl, llvm::Instruction::LShr,
                            llvm::Instruction::AShr}) {
      llvm::LLVMContext Context;
      llvm::Module Module("shift-freeze", Context);
      auto *I64 = llvm::Type::getInt64Ty(Context);
      auto *Type = llvm::FunctionType::get(I64, {I64, I64}, false);
      auto *Function = llvm::Function::Create(
          Type, llvm::GlobalValue::ExternalLinkage, "frozen_shift", Module);
      llvm::IRBuilder<> Builder(
          llvm::BasicBlock::Create(Context, "entry", Function));
      auto *Shift =
          Builder.CreateBinOp(static_cast<llvm::Instruction::BinaryOps>(Opcode),
                              Function->getArg(0), Function->getArg(1));
      auto *Frozen = Builder.CreateFreeze(Shift);
      Builder.CreateRet(SingleUse ? Frozen : Builder.CreateAdd(Frozen, Frozen));
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
      Source += R"(
int main(void) {
  for (unsigned i = 0; i != 256; ++i) {
    uint64_t value = UINT64_C(0xe5ac367812346789) * (i + 1);
    uint64_t expected = )";
      Source += Opcode == llvm::Instruction::AShr
                    ? "(uint64_t)((int64_t)value >> (i % 64));\n"
                : Opcode == llvm::Instruction::LShr ? "value >> (i % 64);\n"
                                                    : "value << (i % 64);\n";
      if (!SingleUse)
        Source += "expected += expected;\n";
      Source +=
          "if (frozen_shift(value, i) != expected) return 1;\n} return 0; }\n";
      for (const char *Level : {"-O0", "-O2"})
        compileAndRun(Source,
                      {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
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

TEST(LLVMCVoidAnalysis, FreezeOfMinMaxRetainsLoadedValuesAndPoisonGuard) {
  for (bool PoisonOperand : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("freeze-minmax", Context);
    Module.setDataLayout("e-p:64:64");
    llvm::IRBuilder<> Builder(Context);
    auto *I32 = Builder.getInt32Ty();
    auto *Pointer = Builder.getPtrTy();
    const llvm::Intrinsic::ID Kinds[] = {
        llvm::Intrinsic::umin, llvm::Intrinsic::umax, llvm::Intrinsic::smin,
        llvm::Intrinsic::smax};
    for (unsigned Kind = 0; Kind != 4; ++Kind) {
      auto *Function = llvm::Function::Create(
          llvm::FunctionType::get(I32, {Pointer, I32, I32}, false),
          llvm::GlobalValue::ExternalLinkage,
          "frozen_minmax_" + std::to_string(Kind), Module);
      Builder.SetInsertPoint(
          llvm::BasicBlock::Create(Context, "entry", Function));
      llvm::Value *Value = Builder.CreateLoad(I32, Function->getArg(0));
      if (PoisonOperand)
        Value = Builder.CreateUDiv(
            Value, Builder.CreateAdd(Function->getArg(2), Builder.getInt32(1),
                                     "denominator", false, true));
      auto *Reduced = Builder.CreateBinaryIntrinsic(Kinds[Kind], Value,
                                                    Function->getArg(1));
      auto *Chosen = Builder.CreateFreeze(Reduced, "chosen");
      Builder.CreateStore(Chosen, Function->getArg(0));
      Builder.CreateRet(Chosen);
    }
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    if (PoisonOperand) {
      EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                   std::runtime_error);
      continue;
    }
    ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
    Source += R"(
int main(void) {
  for (unsigned i = 0; i != 512; ++i) {
    uint32_t a = i * UINT32_C(0x91e10da5);
    uint32_t b = (i + 17) * UINT32_C(0x9e3779b9);
    uint32_t cell = a, expected = a < b ? a : b;
    if (frozen_minmax_0(&cell, b, 0) != expected || cell != expected) return 1;
    cell = a; expected = a > b ? a : b;
    if (frozen_minmax_1(&cell, b, 0) != expected || cell != expected) return 2;
    cell = a; expected = (int32_t)a < (int32_t)b ? a : b;
    if (frozen_minmax_2(&cell, b, 0) != expected || cell != expected) return 3;
    cell = a; expected = (int32_t)a > (int32_t)b ? a : b;
    if (frozen_minmax_3(&cell, b, 0) != expected || cell != expected) return 4;
  }
  return 0;
}
)";
    for (const char *Level : {"-O0", "-O2"})
      compileAndRun(Source,
                    {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
  }
}

TEST(LLVMCVoidAnalysis, FreezeOfIntegerCallAndLoadKeepsOneReturnedValue) {
  llvm::LLVMContext Context;
  llvm::Module Module("freeze-call-join", Context);
  Module.setDataLayout("e-p:64:64");
  auto *I32 = llvm::Type::getInt32Ty(Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I32, {I64, Pointer, I32}, false),
      llvm::GlobalValue::ExternalLinkage, "call_or_load", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Native = llvm::BasicBlock::Create(Context, "native", Function);
  auto *Memory = llvm::BasicBlock::Create(Context, "memory", Function);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateCondBr(
      Builder.CreateICmpNE(Function->getArg(2), Builder.getInt32(0)), Native,
      Memory);
  Builder.SetInsertPoint(Native);
  auto Callee = Module.getOrInsertFunction(
      "integer_service", llvm::FunctionType::get(I32, {I64}, false));
  auto *Returned = Builder.CreateCall(Callee, {Function->getArg(0)});
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Memory);
  auto *Loaded = Builder.CreateLoad(I32, Function->getArg(1));
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Join);
  auto *Merged = Builder.CreatePHI(I32, 2);
  Merged->addIncoming(Returned, Native);
  Merged->addIncoming(Loaded, Memory);
  auto *Chosen = Builder.CreateFreeze(Merged, "chosen");
  Builder.CreateStore(Chosen, Function->getArg(1));
  Builder.CreateRet(Builder.CreateXor(Chosen, Builder.getInt32(0x9713)));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
static unsigned service_calls;
uint32_t integer_service(uint64_t value) {
  ++service_calls;return (uint32_t)(value ^ (value >> 37));
}
int main(void) {
  for (unsigned use = 0; use != 2; ++use)
    for (uint64_t i = 0; i != 512; ++i) {
      uint64_t input = UINT64_C(0xf72385317ac45281) * (i + 1);
      uint32_t cell = (uint32_t)i * UINT32_C(0x953170e9);
      uint32_t expected = use ? (uint32_t)(input ^ (input >> 37)) : cell;
      service_calls = 0;
      if (call_or_load(input, &cell, use) != (expected ^ 0x9713) ||
          cell != expected || service_calls != use) return 1;
    }
  return 0;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(LLVMCVoidAnalysis, FreezeKeepsCapturedMemoryAndBoundedShiftValues) {
  llvm::LLVMContext Context;
  llvm::Module Module("freeze-captured-values", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    const unsigned Bits = Variant == 1 ? 128 : 64;
    auto *Integer = B.getIntNTy(Bits);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(Integer, {B.getPtrTy(), B.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage,
        "frozen_capture_" + std::to_string(Variant), Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
    auto *Load = B.CreateLoad(Integer, F->getArg(0));
    Load->setAlignment(llvm::Align(Bits / 8));
    if (Variant <= 1)
      Load->setVolatile(true);
    if (Variant == 2)
      Load->setAtomic(llvm::AtomicOrdering::SequentiallyConsistent);
    llvm::Value *Value = Load;
    if (Variant == 3) {
      Value = B.CreateShl(Value, B.CreateAnd(F->getArg(1), B.getInt64(63)));
    } else if (Variant == 4) {
      auto *Local = B.CreateAlloca(B.getInt64Ty());
      B.CreateStore(Value, Local);
      auto Service = Module.getOrInsertFunction(
          "pointer_service",
          llvm::FunctionType::get(Integer, {B.getPtrTy()}, false));
      Value = B.CreateCall(Service, {Local});
    }
    auto *Frozen = B.CreateFreeze(Value);
    B.CreateStore(Frozen, F->getArg(0));
    B.CreateRet(Frozen);
  }
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
static unsigned calls;
uint64_t pointer_service(void *pointer) {
  ++calls;
  return *(uint64_t *)pointer ^ UINT64_C(0x91e10da5971317ab);
}
int main(void) {
  for (unsigned i = 0; i != 256; ++i) {
    _Alignas(16) uint64_t value = UINT64_C(0xfedcba9876543210) * (i + 1);
    uint64_t cell = value;
    if (frozen_capture_0(&cell, i) != value || cell != value) return 1;
    _Alignas(16) unsigned __int128 wide =
        ((unsigned __int128)value << 64) | ~value;
    unsigned __int128 copy = wide;
    if (frozen_capture_1(&copy, i) != wide || copy != wide) return 2;
    if (frozen_capture_2(&cell, i) != value || cell != value) return 3;
    const uint64_t shifted = value << (i & 63);
    if (frozen_capture_3(&cell, i) != shifted || cell != shifted) return 4;
    calls = 0;
    const uint64_t expected = shifted ^ UINT64_C(0x91e10da5971317ab);
    if (frozen_capture_4(&cell, i) != expected || cell != expected || calls != 1)
      return 5;
  }
  return 0;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(LLVMCVoidAnalysis, FreezeOfIntegerCallRejectsUnprovedOperands) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    llvm::LLVMContext Context;
    llvm::Module Module("freeze-call-operand", Context);
    auto *I64 = llvm::Type::getInt64Ty(Context);
    auto *Function = llvm::Function::Create(
        llvm::FunctionType::get(I64, {I64, I64}, false),
        llvm::GlobalValue::ExternalLinkage, "unproved_call", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    llvm::Value *Operand = Builder.CreateUDiv(
        Function->getArg(0),
        Builder.CreateAdd(Function->getArg(1), Builder.getInt64(1),
                          "denominator", false, true));
    if (Case == 1)
      Operand = llvm::UndefValue::get(I64);
    if (Case == 2)
      Operand = llvm::PoisonValue::get(I64);
    auto Callee = Module.getOrInsertFunction(
        "integer_service", llvm::FunctionType::get(I64, {I64}, false));
    Builder.CreateRet(
        Builder.CreateFreeze(Builder.CreateCall(Callee, {Operand})));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                 std::runtime_error);
  }
}

TEST(LLVMCVoidAnalysis, FreezeCapturesIndirectCallsAndAtomicUpdatesOnce) {
  llvm::LLVMContext Context;
  llvm::Module Module("freeze-observable-effects", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  auto *CallType =
      llvm::FunctionType::get(B.getInt64Ty(), {B.getInt64Ty()}, false);
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(
          B.getInt64Ty(), {B.getPtrTy(), B.getPtrTy(), B.getInt64Ty()}, false),
      llvm::GlobalValue::ExternalLinkage, "capture_effects", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
  auto *Returned =
      B.CreateFreeze(B.CreateCall(CallType, F->getArg(0), {F->getArg(2)}));
  auto *Previous = B.CreateFreeze(B.CreateAtomicRMW(
      llvm::AtomicRMWInst::Add, F->getArg(1), Returned, llvm::MaybeAlign(8),
      llvm::AtomicOrdering::SequentiallyConsistent));
  B.CreateRet(B.CreateXor(Previous, Returned));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
static unsigned called;
static uint64_t service(uint64_t value) { ++called; return value * 19 + 7; }
int main(void) {
  for (uint64_t i = 0; i != 256; ++i) {
    uint64_t cell = i * UINT64_C(0xfedcba9876543211);
    const uint64_t old = cell, value = service(i);
    called = 0;
    const uint64_t result = capture_effects((void *)service, &cell, i);
    if (called != 1 || cell != old + value || result != (old ^ value)) return 1;
  }
  return 0;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(LLVMCVoidAnalysis, FrozenOddWidthShiftsKeepAllDefinedResults) {
  llvm::LLVMContext Context;
  llvm::Module Module("frozen-odd-shifts", Context);
  Module.setDataLayout("e-p:64:64");
  llvm::IRBuilder<> B(Context);
  for (unsigned Op = 0; Op != 3; ++Op) {
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt64Ty(),
                                {B.getInt64Ty(), B.getInt64Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "odd_shift_" + std::to_string(Op),
        Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
    auto *Value = B.CreateTrunc(F->getArg(0), B.getIntNTy(7));
    auto *Count = B.CreateTrunc(F->getArg(1), B.getIntNTy(7));
    auto *Shifted = Op == 0   ? B.CreateShl(Value, Count)
                    : Op == 1 ? B.CreateLShr(Value, Count)
                              : B.CreateAShr(Value, Count);
    B.CreateRet(B.CreateZExt(B.CreateFreeze(Shifted), B.getInt64Ty()));
  }
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  Source += R"(
int main(void) {
  for (unsigned x = 0; x != 128; ++x)
    for (unsigned n = 0; n != 128; ++n) {
      const unsigned shift = n % 7;
      const int signed_x = x >= 64 ? (int)x - 128 : (int)x;
      const unsigned arithmetic = (unsigned)(signed_x >> shift) & 127;
      if (odd_shift_0(x,n) != ((x << shift) & 127)) return 1;
      if (odd_shift_1(x,n) != (x >> shift)) return 2;
      if (odd_shift_2(x,n) != arithmetic) return 3;
    }
  return 0;
}
)";
  for (const char *Level : {"-O0", "-O2"})
    compileAndRun(Source,
                  {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
}

TEST(LLVMCVoidAnalysis, FreezeRejectsPossibleSignedDivisionOverflow) {
  for (bool Remainder : {false, true}) {
    llvm::LLVMContext Context;
    llvm::Module Module("freeze-signed-division", Context);
    llvm::IRBuilder<> B(Context);
    auto *F = llvm::Function::Create(
        llvm::FunctionType::get(B.getInt32Ty(), {B.getInt32Ty()}, false),
        llvm::GlobalValue::ExternalLinkage, "signed_division", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
    auto *Numerator = B.CreateAdd(F->getArg(0), B.getInt32(1), "", false, true);
    auto *Denominator = B.getInt32(UINT32_MAX);
    auto *Result = Remainder ? B.CreateSRem(Numerator, Denominator)
                             : B.CreateSDiv(Numerator, Denominator);
    B.CreateRet(B.CreateFreeze(Result));
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    EXPECT_THROW(neverd::LLVMCEmitter().emit(Module, Out, {}),
                 std::runtime_error);
  }
}

TEST(LLVMCVoidAnalysis, FreezeOfDivisionRecurrenceHasASafeConstantDenominator) {
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
      Input = Builder.CreateFPToUI(
          Builder.CreateBitCast(Input, Builder.getDoubleTy()), I64);
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
