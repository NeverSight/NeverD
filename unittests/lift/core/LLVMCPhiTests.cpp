//===- LLVMCPhiTests.cpp - Executable LLVM C PHI semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/pass/LLVMC/LLVMCLoopPhases.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/SourceMgr.h"

#include <utility>

namespace {

void compileAndRun(const std::string &Source,
                   llvm::StringRef Optimization = "-O2",
                   bool CheckUndefinedBehavior = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-llvmc-phi", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-llvmc-phi", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-llvmc-phi", "err", ErrorPath));
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
                                                   Optimization,
                                                   "-Werror=uninitialized",
                                                   "-Werror=return-type",
                                                   SourcePath,
                                                   "-o",
                                                   BinaryPath};
  if (CheckUndefinedBehavior) {
    Arguments.push_back("-fsanitize=undefined");
    Arguments.push_back("-fsanitize-trap=all");
  }
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

TEST(LLVMCValues, FoldedStoreArmsPublishTheirOutgoingPhiValues) {
  llvm::LLVMContext Context;
  llvm::Module Module("store-arm-phi", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy(), B.getPtrTy(), I64},
                              false),
      llvm::GlobalValue::ExternalLinkage, "choose_and_store", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Left = llvm::BasicBlock::Create(Context, "left", Function);
  auto *Right = llvm::BasicBlock::Create(Context, "right", Function);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  B.SetInsertPoint(Entry);
  B.CreateCondBr(B.CreateICmpNE(Function->getArg(2), B.getInt64(0)), Left,
                 Right);
  B.SetInsertPoint(Left);
  auto *A = B.CreateAdd(B.CreateLoad(I64, Function->getArg(0)), B.getInt64(7));
  B.CreateStore(A, Function->getArg(0));
  auto *LeftValue = B.CreateXor(A, B.getInt64(0x55));
  B.CreateBr(Join);
  B.SetInsertPoint(Right);
  auto *C = B.CreateMul(B.CreateLoad(I64, Function->getArg(0)), B.getInt64(3));
  B.CreateStore(C, Function->getArg(0));
  auto *RightValue = B.CreateAdd(C, B.getInt64(0x33));
  B.CreateBr(Join);
  B.SetInsertPoint(Join);
  auto *Merged = B.CreatePHI(I64, 2, "merged");
  Merged->addIncoming(LeftValue, Left);
  Merged->addIncoming(RightValue, Right);
  B.CreateStore(Merged, Function->getArg(1));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Output, {}));
  Source += R"(
int main(void) {
  for (uint64_t seed = 0; seed < 256; ++seed)
    for (uint64_t flag = 0; flag < 2; ++flag) {
      uint64_t input = seed, output = 0;
      choose_and_store(&input, &output, flag);
      if (input != (flag ? seed + 7 : seed * 3)) return 1;
      if (output != (flag ? ((seed + 7) ^ 0x55) : seed * 3 + 0x33)) return 2;
    }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, LoopPhiCopiesUseTheTakenEdgeAndPreserveParallelAssignments) {
  llvm::LLVMContext Context;
  llvm::Module Module("loop-phi-copies", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Signature = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                            {I64, Pointer, Pointer}, false);
  auto *Function = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "swap_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateBr(Loop);
  Builder.SetInsertPoint(Loop);
  auto *First = Builder.CreatePHI(I64, 2, "first");
  auto *Second = Builder.CreatePHI(I64, 2, "second");
  auto *Index = Builder.CreatePHI(I64, 2, "index");
  First->addIncoming(Builder.getInt64(11), Entry);
  Second->addIncoming(Builder.getInt64(29), Entry);
  Index->addIncoming(Builder.getInt64(0), Entry);
  Builder.CreateCondBr(Builder.CreateICmpULT(Index, Function->getArg(0)), Body,
                       Exit);
  Builder.SetInsertPoint(Body);
  auto *Next = Builder.CreateAdd(Index, Builder.getInt64(1));
  Builder.CreateBr(Loop);
  First->addIncoming(Second, Body);
  Second->addIncoming(First, Body);
  Index->addIncoming(Next, Body);
  Builder.SetInsertPoint(Exit);
  Builder.CreateStore(First, Function->getArg(1));
  Builder.CreateStore(Second, Function->getArg(2));
  Builder.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t count = 0; count < 32; ++count) {
    uint64_t first = 0, second = 0;
    swap_loop(count, &first, &second);
    if (first != (count & 1 ? 29 : 11)) return 1;
    if (second != (count & 1 ? 11 : 29)) return 2;
  }
  return 0;
}
)");
}

TEST(LLVMCValues, IndependentLoopPhiUpdatesNeedNoSnapshotLocals) {
  llvm::LLVMContext Context;
  llvm::Module Module("independent-loop-updates", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {I64, I64, B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "independent_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
  auto *Body = llvm::BasicBlock::Create(Context, "body", F);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
  B.SetInsertPoint(Entry);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *A = B.CreatePHI(I64, 2, "value");
  auto *C = B.CreatePHI(I64, 2, "value_suffix");
  auto *Index = B.CreatePHI(I64, 2, "index");
  A->addIncoming(F->getArg(1), Entry);
  C->addIncoming(B.getInt64(11), Entry);
  Index->addIncoming(B.getInt64(0), Entry);
  B.CreateCondBr(B.CreateICmpULT(Index, F->getArg(0)), Body, Exit);
  B.SetInsertPoint(Body);
  A->addIncoming(B.CreateAdd(A, B.getInt64(3)), Body);
  C->addIncoming(B.CreateAdd(B.CreateMul(C, B.getInt64(5)), B.getInt64(9)),
                 Body);
  Index->addIncoming(B.CreateAdd(Index, B.getInt64(1)), Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Exit);
  B.CreateStore(A, F->getArg(2));
  B.CreateStore(C, B.CreateGEP(I64, F->getArg(2), B.getInt64(1)));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  EXPECT_EQ(Source.find("phi_edge"), std::string::npos) << Source;
  Source += R"(
int main(void) {
  uint64_t seed = UINT64_MAX;
  for (unsigned trial = 0; trial < 128; ++trial) {
    seed = seed * UINT64_C(6364136223846793005) + 1;
    for (uint64_t count = 0; count < 33; ++count) {
      uint64_t output[2] = {0, 0}, a = seed, b = 11;
      for (uint64_t i = 0; i < count; ++i) { a += 3; b = b * 5 + 9; }
      independent_loop(count, seed, output);
      if (output[0] != a || output[1] != b) return 1;
    }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, CompoundLoopPhiReadsKeepOnlyNeededSnapshots) {
  llvm::LLVMContext Context;
  llvm::Module Module("compound-loop-updates", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(B.getVoidTy(), {I64, I64, B.getPtrTy()}, false),
      llvm::GlobalValue::ExternalLinkage, "compound_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", F);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", F);
  auto *Body = llvm::BasicBlock::Create(Context, "body", F);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", F);
  B.SetInsertPoint(Entry);
  B.CreateBr(Loop);
  B.SetInsertPoint(Loop);
  auto *A = B.CreatePHI(I64, 2, "first");
  auto *C = B.CreatePHI(I64, 2, "second");
  auto *D = B.CreatePHI(I64, 2, "third");
  auto *Index = B.CreatePHI(I64, 2, "index");
  A->addIncoming(F->getArg(1), Entry);
  C->addIncoming(B.getInt64(13), Entry);
  D->addIncoming(B.getInt64(29), Entry);
  Index->addIncoming(B.getInt64(0), Entry);
  B.CreateCondBr(B.CreateICmpULT(Index, F->getArg(0)), Body, Exit);
  B.SetInsertPoint(Body);
  A->addIncoming(B.CreateAdd(C, B.getInt64(7)), Body);
  C->addIncoming(B.CreateXor(D, A), Body);
  D->addIncoming(B.CreateMul(B.CreateAdd(A, D), B.getInt64(3)), Body);
  Index->addIncoming(B.CreateAdd(Index, B.getInt64(1)), Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Exit);
  B.CreateStore(A, F->getArg(2));
  B.CreateStore(C, B.CreateGEP(I64, F->getArg(2), B.getInt64(1)));
  B.CreateStore(D, B.CreateGEP(I64, F->getArg(2), B.getInt64(2)));
  B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  // One snapshot is read once; acyclic updates and loop seeds use assignments.
  EXPECT_EQ(llvm::StringRef(Source).count("phi_edge"), 2U) << Source;
  Source += R"(
int main(void) {
  uint64_t seed = 0;
  for (unsigned trial = 0; trial < 128; ++trial) {
    seed = seed * UINT64_C(2862933555777941757) + UINT64_C(3037000493);
    for (uint64_t count = 0; count < 33; ++count) {
      uint64_t output[3] = {0, 0, 0}, a = seed, b = 13, c = 29;
      for (uint64_t i = 0; i < count; ++i) {
        uint64_t next_a = b + 7, next_b = c ^ a, next_c = (a + c) * 3;
        a = next_a; b = next_b; c = next_c;
      }
      compound_loop(count, seed, output);
      if (output[0] != a || output[1] != b || output[2] != c) return 1;
    }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, InlinedFalseArmKeepsLoopEntryPhiCopy) {
  llvm::LLVMContext Context;
  llvm::Module Module("inlined-loop-entry", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {Pointer}, false),
      llvm::GlobalValue::ExternalLinkage, "seeded_loop", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *First = llvm::BasicBlock::Create(Context, "first", Function);
  auto *Loop = llvm::BasicBlock::Create(Context, "loop", Function);
  auto *Body = llvm::BasicBlock::Create(Context, "body", Function);
  // This preheader is physically after the loop but is printed on the false
  // edge of First. Its outgoing edge still initializes the shared PHI.
  auto *Preheader = llvm::BasicBlock::Create(Context, "preheader", Function);
  auto *Exit = llvm::BasicBlock::Create(Context, "exit", Function);
  llvm::IRBuilder<> B(Entry);
  auto *State = Function->getArg(0);
  auto *Count = B.CreateLoad(I64, State);
  B.CreateCondBr(B.CreateICmpEQ(Count, B.getInt64(0)), Exit, First);
  B.SetInsertPoint(First);
  B.CreateStore(B.CreateSub(Count, B.getInt64(1)), State);
  auto *Value = B.CreateGEP(I64, State, B.getInt64(1));
  auto Update = [&](uint64_t Addend) {
    return B.CreateAdd(B.CreateMul(B.CreateLoad(I64, Value), B.getInt64(13)),
                       B.getInt64(Addend));
  };
  B.CreateStore(Update(222), Value);
  auto *Remaining = B.CreateLoad(I64, State);
  B.CreateCondBr(B.CreateICmpEQ(Remaining, B.getInt64(0)), Exit, Preheader);
  B.SetInsertPoint(Loop);
  auto *Merged = B.CreatePHI(I64, 2, "merged");
  B.CreateStore(Merged, Value);
  auto *Budget = B.CreateLoad(I64, State);
  B.CreateCondBr(B.CreateICmpEQ(Budget, B.getInt64(0)), Exit, Body);
  B.SetInsertPoint(Body);
  B.CreateStore(B.CreateSub(Budget, B.getInt64(1)), State);
  Merged->addIncoming(Update(62), Body);
  B.CreateBr(Loop);
  B.SetInsertPoint(Preheader);
  B.CreateStore(B.CreateSub(Remaining, B.getInt64(1)), State);
  Merged->addIncoming(Update(215), Preheader);
  B.CreateBr(Loop);
  B.SetInsertPoint(Exit);
  B.CreateRet(B.getInt64(0));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, Options));
  compileAndRun(Source + R"(
int main(void) {
  for (uint64_t count = 0; count < 32; ++count) {
    for (uint64_t seed = 0; seed < 64; ++seed) {
      uint64_t state[2] = {count, seed}, expected = seed;
      for (uint64_t step = 0; step < count; ++step)
        expected = expected * 13 + (step == 0 ? 222 : step == 1 ? 215 : 62);
      if (seeded_loop(state) || state[0] || state[1] != expected) return 1;
    }
  }
  return 0;
}

)");
}

TEST(LLVMCValues, MovedFalseArmCannotRunAfterAnInlinedTrueArm) {
  llvm::LLVMContext Context;
  llvm::Module Module("moved-false-arm", Context);
  llvm::IRBuilder<> B(Context);
  auto *I64 = B.getInt64Ty();
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(I64, {B.getPtrTy(), I64}, false),
      llvm::GlobalValue::ExternalLinkage, "choose_effect", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  // Both arm bodies appear after the join in LLVM layout. Printing the false
  // body before the join must not make it a continuation of the true arm.
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  auto *Then = llvm::BasicBlock::Create(Context, "then", Function);
  auto *Else = llvm::BasicBlock::Create(Context, "else", Function);
  B.SetInsertPoint(Entry);
  auto *Seed = B.CreateLoad(I64, Function->getArg(0));
  B.CreateCondBr(B.CreateICmpNE(Function->getArg(1), B.getInt64(0)), Then,
                 Else);
  B.SetInsertPoint(Then);
  auto *Count = B.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, Seed);
  B.CreateStore(Count, Function->getArg(0));
  auto *TrueValue = B.CreateAdd(Count, B.getInt64(7));
  B.CreateBr(Join);
  B.SetInsertPoint(Else);
  auto *Product = B.CreateMul(Seed, B.getInt64(3));
  B.CreateStore(Product, Function->getArg(0));
  auto *FalseValue = B.CreateAdd(Product, B.getInt64(13));
  B.CreateBr(Join);
  B.SetInsertPoint(Join);
  auto *Result = B.CreatePHI(I64, 2, "result");
  Result->addIncoming(TrueValue, Then);
  Result->addIncoming(FalseValue, Else);
  B.CreateRet(Result);
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Output, Options));
  Source += R"(
int main(void) {
  for (uint64_t seed = 0; seed < 256; ++seed)
    for (uint64_t flag = 0; flag < 2; ++flag) {
      uint64_t value = seed;
      uint64_t expected = flag ? __builtin_popcountll(seed) : seed * 3;
      if (choose_effect(&value, flag) != expected + (flag ? 7 : 13)) return 1;
      if (value != expected) return 2;
    }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, BranchAndSwitchPhiCopiesFollowSelectedEdges) {
  llvm::LLVMContext Context;
  llvm::Module Module("branch-switch-phi-copies", Context);
  auto *I64 = llvm::Type::getInt64Ty(Context);
  auto *Signature = llvm::FunctionType::get(I64, {I64}, false);

  auto *Diamond = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "diamond", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Diamond);
  auto *Yes = llvm::BasicBlock::Create(Context, "yes", Diamond);
  auto *No = llvm::BasicBlock::Create(Context, "no", Diamond);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Diamond);
  llvm::IRBuilder<> Builder(Entry);
  Builder.CreateCondBr(
      Builder.CreateICmpEQ(Diamond->getArg(0), Builder.getInt64(0)), Yes, No);
  Builder.SetInsertPoint(Yes);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(No);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Join);
  auto *Answer = Builder.CreatePHI(I64, 2, "answer");
  Answer->addIncoming(Builder.getInt64(11), Yes);
  Answer->addIncoming(Builder.getInt64(22), No);
  auto *Unused = Builder.CreatePHI(I64, 2, "unused");
  Unused->addIncoming(Builder.getInt64(33), Yes);
  Unused->addIncoming(Builder.getInt64(44), No);
  Builder.CreateRet(Answer);

  auto *Switch = llvm::Function::Create(
      Signature, llvm::GlobalValue::ExternalLinkage, "switch_phi", Module);
  Entry = llvm::BasicBlock::Create(Context, "entry", Switch);
  auto *Zero = llvm::BasicBlock::Create(Context, "zero", Switch);
  auto *One = llvm::BasicBlock::Create(Context, "one", Switch);
  auto *Other = llvm::BasicBlock::Create(Context, "other", Switch);
  Builder.SetInsertPoint(Entry);
  auto *Dispatch = Builder.CreateSwitch(Switch->getArg(0), Other, 2);
  Dispatch->addCase(Builder.getInt64(0), Zero);
  Dispatch->addCase(Builder.getInt64(1), One);
  for (auto [Block, Value] :
       {std::pair{Zero, uint64_t(101)}, std::pair{One, uint64_t(202)},
        std::pair{Other, uint64_t(303)}}) {
    Builder.SetInsertPoint(Block);
    auto *Selected = Builder.CreatePHI(I64, 1, "selected");
    Selected->addIncoming(Builder.getInt64(Value), Entry);
    Builder.CreateRet(Selected);
  }

  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(Module, Out, {}));
  compileAndRun(Source + R"(
int main(void) {
  if (diamond(0) != 11 || diamond(7) != 22) return 1;
  if (switch_phi(0) != 101 || switch_phi(1) != 202) return 2;
  if (switch_phi(2) != 303 || switch_phi(UINT64_MAX) != 303) return 3;
  return 0;
}
)");
}

const char *CommonBranchIR = R"(
declare void @record_value(i32, ptr)
define i64 @branch_loop(i32 %seed, i32 %count, i32 %flag, ptr %output) {
entry:
  %limit = and i32 %count, 31
  %second = getelementptr i32, ptr %output, i32 1
  br label %head
head:
  %index = phi i32 [0, %entry], [%inc, %latch]
  %value = phi i32 [%seed, %entry], [%carried, %latch]
  %toggle = xor i32 %index, %flag
  %bit = and i32 %toggle, 1
  %choose = icmp ne i32 %bit, 0
  br i1 %choose, label %left, label %right
left:
  %a = add i32 %value, 7
  %aa = xor i32 %value, -1
  call void @record_value(i32 %a, ptr %output)
  call void @record_value(i32 %aa, ptr %second)
  %done.left = icmp uge i32 %index, %limit
  br i1 %done.left, label %exit, label %latch
right:
  %b = xor i32 %value, 53
  %bb = add i32 %value, 9
  call void @record_value(i32 %b, ptr %output)
  call void @record_value(i32 %bb, ptr %second)
  %done.right = icmp uge i32 %index, %limit
  br i1 %done.right, label %exit, label %latch
latch:
  %carried = phi i32 [%aa, %left], [%bb, %right]
  %inc = add i32 %index, 1
  br label %head
exit:
  %first = phi i32 [%a, %left], [%b, %right]
  %last = phi i32 [%aa, %left], [%bb, %right]
  %wide.first = zext i32 %first to i64
  %upper = shl i64 %wide.first, 32
  %wide.last = zext i32 %last to i64
  %result = or i64 %upper, %wide.last
  ret i64 %result
}
)";

void checkCommonBranch(bool DifferentCondition, bool AdditionalPredecessor,
                       bool OnlyFunction, bool SnapshotCondition = false) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  std::string IR = CommonBranchIR;
  if (SnapshotCondition) {
    const std::string Old = "%limit = and i32 %count, 31";
    IR.replace(IR.find(Old), Old.size(),
               "%entry.count = load i32, ptr %output\n"
               "  %limit = and i32 %entry.count, 31");
  }
  if (DifferentCondition) {
    const std::string Old = "%done.right = icmp uge i32 %index, %limit";
    IR.replace(IR.find(Old), Old.size(),
               "%other.limit = add i32 %limit, 1\n"
               "  %done.right = icmp uge i32 %index, %other.limit");
  }
  if (AdditionalPredecessor) {
    const std::string Old = "br i1 %choose, label %left, label %right\nleft:";
    IR.replace(IR.find(Old), Old.size(),
               "br i1 %choose, label %left, label %bridge\n"
               "bridge:\n"
               "  br i1 %choose, label %left, label %right\nleft:");
  }
  auto Module = llvm::parseAssemblyString(IR, Error, Context);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string Before;
  llvm::raw_string_ostream BeforeStream(Before);
  Module->print(BeforeStream, nullptr);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(
      *Module, Output, Options, nullptr, nullptr,
      OnlyFunction ? Module->getFunction("branch_loop") : nullptr));
  std::string After;
  llvm::raw_string_ostream AfterStream(After);
  Module->print(AfterStream, nullptr);
  EXPECT_EQ(Before, After) << "Source normalization must use its own clone";
  if (!DifferentCondition && !AdditionalPredecessor) {
    size_t Comparisons = 0;
    for (llvm::StringRef Token : {" >= ", " < "})
      for (size_t At = 0;
           (At = Source.find(Token.str(), At)) != std::string::npos;
           At += Token.size())
        ++Comparisons;
    EXPECT_EQ(Comparisons, 1U) << Source;
  }
  // The external observer is supplied by the harness, including when only a
  // selected function's source fragment is requested.
  Source =
      "#include <stdint.h>\nvoid record_value(uint32_t, void *);\n" + Source;
  Source += "\n#define DIFFERENT_CONDITION " +
            std::to_string(DifferentCondition) + "\n";
  Source += R"(
static volatile uint32_t observed_trace;
void record_value(uint32_t value, void *address) {
  *(uint32_t *)address = value;
  observed_trace = observed_trace * UINT32_C(65599) + value;
}
int main(void) {
  uint32_t seed = UINT32_MAX;
  for (unsigned trial = 0; trial < 128; ++trial) {
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    for (uint32_t count = 0; count < 32; ++count)
      for (uint32_t flag = 0; flag < 4; ++flag) {
        uint32_t value = seed, first = 0, last = 0, trace = 0;
        for (uint32_t i = 0;; ++i) {
          uint32_t choose = (i ^ flag) & 1;
          first = choose ? value + 7 : value ^ 53;
          last = choose ? ~value : value + 9;
          trace = trace * UINT32_C(65599) + first;
          trace = trace * UINT32_C(65599) + last;
          if (i >= count + (DIFFERENT_CONDITION && !choose)) break;
          value = last;
        }
        uint32_t output[] = {count, 0, UINT32_C(0xa154e93b)};
        observed_trace = 0;
        uint64_t actual = branch_loop(seed, count, flag, (void *)output);
        if (actual != (((uint64_t)first << 32) | last)) return 1;
        if (output[0] != first || output[1] != last) return 2;
        if (output[2] != UINT32_C(0xa154e93b)) return 3;
        if (observed_trace != trace) return 4;
      }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, CommonLoopExitKeepsArmEffectsAndBothPhiPairs) {
  checkCommonBranch(false, false, false);
  checkCommonBranch(false, false, true);
}

TEST(LLVMCValues, DifferentExitComparisonsKeepTheirOwnControl) {
  checkCommonBranch(true, false, false);
}

TEST(LLVMCValues, CommonExitArmsWithAnotherPredecessorRemainValid) {
  checkCommonBranch(false, true, false);
}

TEST(LLVMCValues, CommonExitRetainsValuesReadBeforeArmCalls) {
  checkCommonBranch(false, false, false, true);
}

const char *LoopPhaseIR = R"(
declare i32 @llvm.fshr.i32(i32, i32, i32)
declare void @observe_phase(i32, ptr)
define i32 @phased(i32 %seed, i32 %count, i32 %mask, ptr %output) {
entry:
  %limit = and i32 %count, 15
  %r0 = call i32 @llvm.fshr.i32(i32 %seed, i32 %seed, i32 7)
  %p0 = xor i32 %r0, %mask
  br label %head
head:
  %index = phi i32 [0, %entry], [%inc, %left], [%inc, %right]
  %value = phi i32 [%p0, %entry], [%pl, %left], [%pr, %right]
  call void @observe_phase(i32 %value, ptr %output)
  %done = icmp eq i32 %index, %limit
  br i1 %done, label %exit, label %choose
choose:
  %inc = add i32 %index, 1
  %bit = and i32 %index, 1
  %odd = icmp ne i32 %bit, 0
  br i1 %odd, label %left, label %right
left:
  %a = xor i32 %index, %seed
  %next.left = add i32 %value, %a
  %rl = call i32 @llvm.fshr.i32(i32 %next.left, i32 %next.left, i32 7)
  %pl = xor i32 %rl, %mask
  br label %head
right:
  %b = add i32 %index, 23
  %next.right = sub i32 %value, %b
  %rr = call i32 @llvm.fshr.i32(i32 %next.right, i32 %next.right, i32 7)
  %pr = xor i32 %rr, %mask
  br label %head
exit:
  ret i32 %value
}
)";

void checkLoopPhases(bool OnlyFunction, bool DifferentOperation,
                     bool SharedRoot, bool MemorySnapshot = false) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  std::string IR = LoopPhaseIR;
  if (DifferentOperation) {
    const std::string Old = "%pr = xor i32 %rr, %mask";
    IR.replace(IR.find(Old), Old.size(), "%pr = or i32 %rr, %mask");
  }
  if (SharedRoot) {
    const std::string Old = "%p0 = xor i32 %r0, %mask";
    IR.replace(IR.find(Old), Old.size(),
               Old + "\n  call void @observe_phase(i32 %p0, ptr %output)");
  }
  if (MemorySnapshot) {
    const std::string Old =
        "%r0 = call i32 @llvm.fshr.i32(i32 %seed, i32 %seed, i32 7)";
    IR.replace(
        IR.find(Old), Old.size(),
        "%saved = load i32, ptr %output\n"
        "  %r0 = call i32 @llvm.fshr.i32(i32 %saved, i32 %saved, i32 7)");
    const std::string Root = "%p0 = xor i32 %r0, %mask";
    IR.replace(IR.find(Root), Root.size(),
               Root + "\n  call void @observe_phase(i32 %mask, ptr %output)");
    const std::string Next = "%rr = call i32 @llvm.fshr.i32(i32 %next.right, "
                             "i32 %next.right, i32 7)";
    IR.replace(IR.find(Next), Next.size(),
               "%saved.right = load i32, ptr %output\n"
               "  %rr = call i32 @llvm.fshr.i32(i32 %saved.right, i32 "
               "%saved.right, i32 7)");
    const std::string Last = "%pr = xor i32 %rr, %mask";
    IR.replace(IR.find(Last), Last.size(),
               Last + "\n  call void @observe_phase(i32 %mask, ptr %output)");
  }
  auto Module = llvm::parseAssemblyString(IR, Error, Context);
  ASSERT_TRUE(Module);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string Before;
  llvm::raw_string_ostream BeforeStream(Before);
  Module->print(BeforeStream, nullptr);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(
      *Module, Output, Options, nullptr, nullptr,
      OnlyFunction ? Module->getFunction("phased") : nullptr));
  std::string After;
  llvm::raw_string_ostream AfterStream(After);
  Module->print(AfterStream, nullptr);
  EXPECT_EQ(Before, After);
  llvm::StringRef Call = "neverd_llvm_fshr_i32(";
  size_t Calls = 0;
  for (size_t At = 0; (At = Source.find(Call.str(), At)) != std::string::npos;
       At += Call.size())
    ++Calls;
  // Include the helper definition itself. All three incoming edges must agree.
  EXPECT_EQ(Calls, DifferentOperation || SharedRoot ? 4U : 2U) << Source;
  Source = "#include <stdint.h>\nvoid observe_phase(uint32_t, void *);\n" +
           Source + "\n#define DIFFERENT_OPERATION " +
           std::to_string(DifferentOperation) + "\n#define SHARED_ROOT " +
           std::to_string(SharedRoot) + "\n#define MEMORY_SNAPSHOT " +
           std::to_string(MemorySnapshot) + "\n";
  Source += R"(
static volatile uint32_t phase_trace;
void observe_phase(uint32_t value, void *output) {
  *(uint32_t *)output = value;
  phase_trace = phase_trace * UINT32_C(65599) + value;
}
static uint32_t rotate_right(uint32_t value) {
  return (value >> 7) | (value << 25);
}
int main(void) {
  uint32_t seed = UINT32_MAX, mask = 0;
  for (unsigned trial = 0; trial < 512; ++trial) {
    seed = seed * UINT32_C(1664525) + UINT32_C(1013904223);
    mask = mask * UINT32_C(22695477) + 1;
    for (uint32_t count = 0; count < 16; ++count) {
      uint32_t value = rotate_right(seed) ^ mask;
      uint32_t trace = MEMORY_SNAPSHOT ? mask : SHARED_ROOT ? value : 0;
      for (uint32_t i = 0;; ++i) {
        trace = trace * UINT32_C(65599) + value;
        if (i == count) break;
        if (i & 1) value = rotate_right(value + (i ^ seed)) ^ mask;
        else {
          value = rotate_right(MEMORY_SNAPSHOT ? value : value - (i + 23));
          value = DIFFERENT_OPERATION ? value | mask : value ^ mask;
          if (MEMORY_SNAPSHOT) trace = trace * UINT32_C(65599) + mask;
        }
      }
      uint32_t output[] = {seed, UINT32_C(0x957d62ab)};
      phase_trace = 0;
      if (phased(seed, count, mask, (void *)output) != value) return 1;
      if (output[0] != value || output[1] != UINT32_C(0x957d62ab)) return 2;
      if (phase_trace != trace) return 3;
    }
  }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, LoopPhasesRecombineEveryBackedgeAndKeepObserverOrder) {
  checkLoopPhases(false, false, false);
  checkLoopPhases(true, false, false);
}

TEST(LLVMCValues, LoopPhasesRetainConflictingBackedgesAndSharedRoots) {
  checkLoopPhases(false, true, false);
  checkLoopPhases(false, false, true);
}

TEST(LLVMCValues, LoopPhasesKeepMemorySnapshotsBeforeModifyingCalls) {
  checkLoopPhases(false, false, false, true);
}

TEST(LLVMCValues, LoopPhasesRetainNarrowWrapAndSignedExtension) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(R"(
define i64 @width_phase(i64 %seed, i32 %count) {
entry:
  %limit = and i32 %count, 31
  %n0 = trunc i64 %seed to i8
  %x0 = sext i8 %n0 to i64
  br label %head
head:
  %index = phi i32 [0, %entry], [%inc, %latch]
  %value = phi i64 [%x0, %entry], [%x1, %latch]
  %done = icmp eq i32 %index, %limit
  br i1 %done, label %exit, label %latch
latch:
  %wide = add i64 %value, %seed
  %n1 = trunc i64 %wide to i8
  %x1 = sext i8 %n1 to i64
  %inc = add i32 %index, 1
  br label %head
exit:
  ret i64 %value
}
)",
                                          Error, Context);
  ASSERT_TRUE(Module);
  ASSERT_TRUE(neverd::llvmc::factorLoopPhiExpressions(
      *Module->getFunction("width_phase")));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  unsigned Extensions = 0, Truncations = 0;
  for (auto &Block : *Module->getFunction("width_phase"))
    for (auto &I : Block) {
      Extensions += llvm::isa<llvm::SExtInst>(I);
      Truncations += llvm::isa<llvm::TruncInst>(I);
    }
  EXPECT_EQ(Extensions, 1U);
  EXPECT_EQ(Truncations, 1U);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, Output, Options));
  Source += R"(
int main(void) {
  for (uint64_t seed = 0; seed < 1024; ++seed)
    for (uint32_t count = 0; count < 32; ++count) {
      uint64_t input = (seed << 48) | seed;
      uint64_t expected = 0;
      for (uint32_t i = 0; i <= count; ++i) {
        uint64_t byte = (expected + input) & 255;
        expected = byte < 128 ? byte : byte | ~UINT64_C(255);
      }
      if (width_phase(input, count) != expected) return 1;
    }
  return 0;
}
)";
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}

TEST(LLVMCValues, LoopPhasesRejectPartialOperationsAndPreserveFrozenInputs) {
  for (llvm::StringRef Operation :
       {"add nuw i32 %x, 3", "lshr exact i32 %x, 3", "shl i32 %x, %amount",
        "sdiv i32 %x, 3", "xor i32 %x, undef", "xor i32 %x, poison",
        "freeze i32 %x"}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Error;
    std::string Op = Operation.str();
    std::string Back = Op;
    Back.replace(Back.find("%x"), 2, "%p");
    std::string IR =
        "define i32 @refuse(i32 %x, i32 %amount, i1 %again) {\n"
        "entry:\n %a = " +
        Op +
        "\n br label %head\nhead:\n"
        " %p = phi i32 [%a, %entry], [%b, %latch]\n"
        " br i1 %again, label %latch, label %exit\nlatch:\n %b = " +
        Back + "\n br label %head\nexit:\n ret i32 %p\n}";
    auto Module = llvm::parseAssemblyString(IR, Error, Context);
    ASSERT_TRUE(Module) << IR;
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    EXPECT_FALSE(
        neverd::llvmc::factorLoopPhiExpressions(*Module->getFunction("refuse")))
        << Operation.str();
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  }
}

TEST(LLVMCValues, LoopPhaseBudgetRefusalLeavesOriginalIR) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(LoopPhaseIR, Error, Context);
  ASSERT_TRUE(Module);
  auto *Function = Module->getFunction("phased");
  auto &Entry = Function->getEntryBlock();
  for (unsigned I = 0; I < 65536; ++I)
    llvm::BinaryOperator::CreateAdd(Function->getArg(0), Function->getArg(2),
                                    "budget",
                                    Entry.getTerminator()->getIterator());
  std::string Before;
  llvm::raw_string_ostream BeforeStream(Before);
  Module->print(BeforeStream, nullptr);
  EXPECT_FALSE(neverd::llvmc::factorLoopPhiExpressions(*Function));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  std::string After;
  llvm::raw_string_ostream AfterStream(After);
  Module->print(AfterStream, nullptr);
  EXPECT_EQ(Before, After);
}

TEST(LLVMCValues, LoopPhasesPreserveConstrainedCallsAndExceptionFunctions) {
  for (auto Attribute : {llvm::Attribute::Convergent, llvm::Attribute::NoMerge,
                         llvm::Attribute::NoDuplicate}) {
    llvm::LLVMContext Context;
    llvm::SMDiagnostic Error;
    auto Module = llvm::parseAssemblyString(LoopPhaseIR, Error, Context);
    ASSERT_TRUE(Module);
    auto *Function = Module->getFunction("phased");
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I))
          Intrinsic->addFnAttr(Attribute);
    neverd::llvmc::factorLoopPhiExpressions(*Function);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    unsigned Calls = 0;
    for (auto &Block : *Function)
      for (auto &I : Block)
        Calls += llvm::isa<llvm::IntrinsicInst>(I);
    EXPECT_EQ(Calls, 3U);
  }
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(LoopPhaseIR, Error, Context);
  ASSERT_TRUE(Module);
  auto *Function = Module->getFunction("phased");
  auto Personality = Module->getOrInsertFunction(
      "personality",
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context), true));
  Function->setPersonalityFn(
      llvm::cast<llvm::Constant>(Personality.getCallee()));
  EXPECT_FALSE(neverd::llvmc::factorLoopPhiExpressions(*Function));
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
}

/// Where \p Source defines the function whose header starts with \p Header:
/// the header's line that opens its body.  A whole module declares each
/// definition before the bodies (DefinitionDeclarations), which a unit of
/// one function leaves out.
size_t definitionAt(llvm::StringRef Source, llvm::StringRef Header) {
  for (size_t At = Source.find(Header); At != llvm::StringRef::npos;
       At = Source.find(Header, At + 1)) {
    const size_t End = Source.find('\n', At);
    if (Source.slice(At, End).rtrim().ends_with("{"))
      return At;
  }
  return llvm::StringRef::npos;
}

std::string emitScalarRegions(llvm::StringRef IR) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Error;
  auto Module = llvm::parseAssemblyString(IR, Error, Context);
  if (!Module) {
    ADD_FAILURE() << Error.getMessage().str();
    return {};
  }
  std::string Before;
  llvm::raw_string_ostream BeforeOut(Before);
  Module->print(BeforeOut, nullptr);
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  neverd::CEmitterOptions Options;
  Options.PreserveLLVMFunctionTypes = true;
  EXPECT_TRUE(neverd::LLVMCEmitter().emit(*Module, Out, Options));
  for (const auto &Function : *Module) {
    if (Function.isDeclaration())
      continue;
    std::string Selected;
    llvm::raw_string_ostream SelectedOut(Selected);
    EXPECT_TRUE(neverd::LLVMCEmitter().emit(*Module, SelectedOut, Options,
                                            nullptr, nullptr, &Function));
    // Selected-function emission deliberately omits unrelated prototypes and
    // module spacing. Its actual function projection must be identical.
    const std::string Definition = " " + Function.getName().str() + "(";
    const auto WholeAt = definitionAt(Source, Definition);
    const auto SelectedAt = definitionAt(Selected, Definition);
    EXPECT_NE(WholeAt, std::string::npos);
    EXPECT_NE(SelectedAt, std::string::npos);
    if (WholeAt != std::string::npos && SelectedAt != std::string::npos)
      EXPECT_EQ(Source.substr(WholeAt), Selected.substr(SelectedAt));
  }
  std::string After;
  llvm::raw_string_ostream AfterOut(After);
  Module->print(AfterOut, nullptr);
  EXPECT_EQ(Before, After);
  return Source;
}

TEST(LLVMCScalarRegions, NestedLoopsAndDiamondIgnorePhysicalBlockOrder) {
  const auto Source = emitScalarRegions(R"(
define i16 @grid(i16 %seed, i16 %rows, i16 %cols) {
entry:
  br label %outer
exit:
  ret i16 %sum
odd:
  %minus = sub i16 %inner_sum, %j
  br label %join
inner:
  %j = phi i16 [0, %outer], [%jn, %join]
  %inner_sum = phi i16 [%sum, %outer], [%chosen, %join]
  %done = icmp uge i16 %j, %cols
  br i1 %done, label %outer_step, label %body
outer_step:
  %total = add i16 %inner_sum, %i
  %in = add i16 %i, 1
  br label %outer
outer:
  %i = phi i16 [0, %entry], [%in, %outer_step]
  %sum = phi i16 [%seed, %entry], [%total, %outer_step]
  %more = icmp ult i16 %i, %rows
  br i1 %more, label %inner, label %exit
body:
  %bit = and i16 %j, 1
  %test = icmp ne i16 %bit, 0
  br i1 %test, label %odd, label %even
even:
  %plus = xor i16 %inner_sum, %i
  br label %join
join:
  %chosen = phi i16 [%minus, %odd], [%plus, %even]
  %jn = add i16 %j, 1
  br label %inner
}
)");
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  const auto First = Source.find("for (");
  ASSERT_NE(First, std::string::npos) << Source;
  EXPECT_NE(Source.find("for (", First + 1), std::string::npos) << Source;
  size_t Declarations = 0;
  for (size_t At = 0;
       (At = Source.find("    uint16_t ", At)) != std::string::npos; ++At)
    ++Declarations;
  EXPECT_EQ(Declarations, 1u) << Source;
  EXPECT_NE(Source.find("for (uint16_t "), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (unsigned seed = 65520; seed < 65536; ++seed)
    for (unsigned rows = 0; rows < 9; ++rows)
      for (unsigned cols = 0; cols < 11; ++cols) {
        uint16_t expected = seed;
        for (unsigned i = 0; i < rows; ++i) {
          for (unsigned j = 0; j < cols; ++j)
            expected = j & 1 ? expected - j : expected ^ i;
          expected += i;
        }
        if (grid(seed, rows, cols) != expected) return 1;
      }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, HeaderEffectsRunAtEveryTestIncludingExit) {
  const auto Source = emitScalarRegions(R"(
declare i32 @observe(i32)
define i32 @header_calls(i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %body]
  %v = call i32 @observe(i32 %i)
  %more = icmp ult i32 %v, %n
  br i1 %more, label %body, label %exit
body:
  %next = add i32 %i, 1
  br label %header
exit:
  %answer = phi i32 [%v, %header]
  ret i32 %answer
}
)");
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_NE(Source.find("while (1)"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
static unsigned calls, wrong;
uint32_t observe(uint32_t value) {
  if (value != calls++) wrong = 1;
  return value;
}
int main(void) {
  for (unsigned n = 0; n < 80; ++n) {
    calls = wrong = 0;
    if (header_calls(n) != n || calls != n + 1 || wrong) return 1;
  }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, SimultaneousSwapsAndLiveExitKeepOldValues) {
  const auto Source = emitScalarRegions(R"(
define i32 @swap_values(i32 %n, i32 %x, i32 %y) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %body]
  %a = phi i32 [%x, %entry], [%b, %body]
  %b = phi i32 [%y, %entry], [%a, %body]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %next = add i32 %i, 1
  br label %header
exit:
  %shifted = shl i32 %a, 3
  %result = xor i32 %shifted, %b
  ret i32 %result
}
)");
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_NE(Source.find("phi_edge"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t n = 0; n < 33; ++n)
    for (uint32_t x = 0; x < 256; ++x) {
      uint32_t y = ~x;
      uint32_t a = n & 1 ? y : x, b = n & 1 ? x : y;
      if (swap_values(n, x, y) != ((a << 3) ^ b)) return 1;
    }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, MultiExitFallbackRetainsEarlyExitAndPhiEdges) {
  const auto Source = emitScalarRegions(R"(
define i32 @early(i32 %n, i32 %stop) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %step]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %quit = icmp eq i32 %i, %stop
  br i1 %quit, label %exit, label %step
step:
  %next = add i32 %i, 1
  br label %header
exit:
  %result = phi i32 [%n, %header], [%i, %body]
  ret i32 %result
}
)");
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t n = 0; n < 24; ++n)
    for (uint32_t stop = 0; stop < 27; ++stop)
      if (early(n, stop) != (n < stop ? n : stop)) return 1;
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, LiveOuterValueCannotAliasInnerAccumulator) {
  const auto Source = emitScalarRegions(R"(
define i32 @separate_lifetimes(i32 %seed, i32 %n) {
entry:
  br label %outer
outer:
  %i = phi i32 [0, %entry], [%next, %step]
  %a = phi i32 [%seed, %entry], [%combined, %step]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %inner, label %exit
inner:
  %j = phi i32 [0, %outer], [%jn, %body]
  %b = phi i32 [%a, %outer], [%changed, %body]
  %again = icmp ult i32 %j, 4
  br i1 %again, label %body, label %step
body:
  %changed = add i32 %b, %j
  %jn = add i32 %j, 1
  br label %inner
step:
  %combined = xor i32 %a, %b
  %next = add i32 %i, 1
  br label %outer
exit:
  ret i32 %a
}
)");
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  // Both carriers must remain, because the old outer value survives the inner
  // loop. Removing that copy silently turns the final xor into zero.
  EXPECT_NE(Source.find("uint32_t result"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint32_t b"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t seed = 0; seed < 256; ++seed)
    for (uint32_t n = 0; n < 20; ++n) {
      uint32_t a = ~seed;
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t b = a;
        for (uint32_t j = 0; j < 4; ++j) b += j;
        a ^= b;
      }
      if (separate_lifetimes(~seed, n) != a) return 1;
    }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, InlinedFunnelShiftKeepsWidthEndpointsAndOldCarrier) {
  const auto Source = emitScalarRegions(R"(
declare i8 @llvm.fshr.i8(i8, i8, i8)
define i8 @byte_rounds(i8 %seed, i8 %n, i8 %right, i8 %shift) {
entry:
  br label %header
header:
  %i = phi i8 [0, %entry], [%next, %body]
  %a = phi i8 [%seed, %entry], [%changed, %body]
  %more = icmp ult i8 %i, %n
  br i1 %more, label %body, label %exit
body:
  %rotated = call i8 @llvm.fshr.i8(i8 %a, i8 %right, i8 %shift)
  %changed = xor i8 %rotated, %i
  %next = add i8 %i, 1
  br label %header
exit:
  ret i8 %a
}
)");
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
  EXPECT_EQ(Source.find("rotated"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (unsigned seed = 200; seed < 256; ++seed)
    for (unsigned n = 0; n < 33; ++n)
      for (unsigned shift = 0; shift < 64; ++shift) {
        unsigned k = shift % 8;
        uint8_t a = seed, right = seed ^ 93;
        for (unsigned i = 0; i < n; ++i) {
          uint8_t r = k ? ((unsigned)a << (8 - k)) | (right >> k) : right;
          a = r ^ i;
        }
        if (byte_rounds(seed, n, right, shift) != a) return 1;
      }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, SharedStepStaysMaterializedBeforePhiPublication) {
  const auto Source = emitScalarRegions(R"(
define i8 @shared_step(i8 %seed, i8 %n) {
entry:
  br label %header
header:
  %i = phi i8 [0, %entry], [%next, %body]
  %a = phi i8 [%seed, %entry], [%sum, %body]
  %more = icmp ult i8 %i, %n
  br i1 %more, label %body, label %exit
body:
  %next = add i8 %i, 1
  %sum = add i8 %a, %next
  br label %header
exit:
  ret i8 %a
}
)");
  EXPECT_NE(Source.find("while ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("for ("), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (unsigned seed = 241; seed < 256; ++seed)
    for (unsigned n = 0; n < 256; ++n) {
      uint8_t expected = seed;
      for (unsigned i = 0; i < n; ++i) expected += i + 1;
      if (shared_step(seed, n) != expected) return 1;
    }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, MultipleEntriesKeepIrreducibleFallback) {
  const auto Source = emitScalarRegions(R"(
define i32 @multiple_entries(i32 %n, i1 %choose) {
entry:
  br i1 %choose, label %left, label %right
left:
  %a = phi i32 [%n, %entry], [%bn, %right]
  %an = sub i32 %a, 1
  %stopa = icmp eq i32 %a, 0
  br i1 %stopa, label %exit, label %right
right:
  %b = phi i32 [%n, %entry], [%an, %left]
  %bn = sub i32 %b, 1
  %stopb = icmp eq i32 %b, 0
  br i1 %stopb, label %exit, label %left
exit:
  %result = phi i32 [11, %left], [23, %right]
  ret i32 %result
}
)");
  EXPECT_NE(Source.find("goto "), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (unsigned n = 0; n < 97; ++n)
    for (unsigned choose = 0; choose < 2; ++choose) {
      unsigned in_left = choose ^ (n & 1);
      if (multiple_entries(n, choose) != (in_left ? 11 : 23)) return 1;
    }
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, OversizedGraphFallsBackWithoutLosingTheLoop) {
  std::string IR = R"(
define i32 @large_graph(i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %step]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %bridge0, label %exit
)";
  for (unsigned I = 0; I < 1024; ++I) {
    IR += "bridge" + std::to_string(I) + ":\n  br label %";
    IR += I == 1023 ? "step\n" : "bridge" + std::to_string(I + 1) + "\n";
  }
  IR += R"(
step:
  %next = add i32 %i, 1
  br label %header
exit:
  ret i32 %i
}
)";
  const auto Source = emitScalarRegions(IR);
  EXPECT_EQ(Source.find("for ("), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t n = 0; n < 17; ++n)
    if (large_graph(n) != n) return 1;
  return 0;
}
)",
                  Optimization);
}

TEST(LLVMCScalarRegions, ScopedByteCountersKeepIncrementAndDecrementWrap) {
  for (bool Decrement : {false, true}) {
    SCOPED_TRACE(Decrement);
    const std::string IR = R"(
define i32 @byte_walk(i32 %seed, i8 %stop) {
entry:
  br label %header
header:
  %index = phi i8 [250, %entry], [%next, %body]
  %sum = phi i32 [%seed, %entry], [%updated, %body]
  %more = icmp ne i8 %index, %stop
  br i1 %more, label %body, label %exit
body:
  %wide = zext i8 %index to i32
  %updated = add i32 %sum, %wide
  %next = add i8 %index, )" +
                           std::string(Decrement ? "-1" : "1") + R"(
  br label %header
exit:
  ret i32 %sum
}
)";
    const auto Source = emitScalarRegions(IR);
    EXPECT_NE(Source.find("for (uint8_t "), std::string::npos) << Source;
    EXPECT_NE(Source.find(Decrement ? "--i" : "++i"), std::string::npos)
        << Source;
    EXPECT_NE(Source.find(" += "), std::string::npos) << Source;
    const std::string Main = R"(
int main(void) {
  for (unsigned stop = 0; stop < 256; ++stop)
    for (uint32_t seed = 0; seed < 17; ++seed) {
      uint32_t expected = UINT32_MAX - seed;
      for (uint8_t i = 250; i != stop; ) {
        expected += i;
        i = (uint8_t)(i )" + std::string(Decrement ? "-" : "+") +
                             R"( 1);
      }
      if (byte_walk(UINT32_MAX - seed, stop) != expected) return 1;
    }
  return 0;
}
)";
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Main, Optimization, true);
  }
}

TEST(LLVMCScalarRegions, CounterCoalescedWithExitValueKeepsFunctionScope) {
  const auto Source = emitScalarRegions(R"(
define i32 @exit_counter(i32 %n) {
entry:
  br label %header
header:
  %index = phi i32 [0, %entry], [%next, %body]
  %more = icmp ult i32 %index, %n
  br i1 %more, label %body, label %exit
body:
  %next = add i32 %index, 1
  br label %header
exit:
  %out = phi i32 [%index, %header]
  ret i32 %out
}
)");
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("for (uint32_t "), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t n = 0; n < 64; ++n)
    if (exit_counter(n) != n) return 1;
  return 0;
}
)",
                  Optimization, true);
}

TEST(LLVMCScalarRegions, InlineUseAfterLoopKeepsCounterInFunctionScope) {
  const auto Source = emitScalarRegions(R"(
define i32 @late_expression(i32 %n) {
entry:
  br label %header
header:
  %index = phi i32 [0, %entry], [%next, %body]
  %late = xor i32 %index, 13
  %more = icmp ult i32 %index, %n
  br i1 %more, label %body, label %exit
body:
  %next = add i32 %index, 1
  br label %header
exit:
  ret i32 %late
}
)");
  EXPECT_NE(Source.find("for ("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("for (uint32_t "), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t n = 0; n < 64; ++n)
    if (late_expression(n) != (n ^ 13u)) return 1;
  return 0;
}
)",
                  Optimization, true);
}

TEST(LLVMCScalarRegions,
     NarrowMultiplicationDoesNotBecomeUnsafeCompoundUpdate) {
  const auto Source = emitScalarRegions(R"(
define i16 @narrow_product(i16 %seed, i16 %factor, i16 %n) {
entry:
  br label %header
header:
  %i = phi i16 [0, %entry], [%next, %body]
  %state = phi i16 [%seed, %entry], [%product, %body]
  %more = icmp ult i16 %i, %n
  br i1 %more, label %body, label %exit
body:
  %product = mul i16 %state, %factor
  %next = add i16 %i, 1
  br label %header
exit:
  ret i16 %state
}
)");
  EXPECT_EQ(Source.find(" *= "), std::string::npos) << Source;
  EXPECT_NE(Source.find("(uint32_t)"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  const uint16_t values[] = {0, 1, 255, 256, 32767, 32768, 65535};
  for (unsigned a = 0; a < 7; ++a)
    for (unsigned b = 0; b < 7; ++b)
      for (unsigned n = 0; n < 20; ++n) {
        uint16_t expected = values[a];
        for (unsigned i = 0; i < n; ++i)
          expected = (uint16_t)((uint32_t)expected * values[b]);
        if (narrow_product(values[a], values[b], n) != expected) return 1;
      }
  return 0;
}
)",
                  Optimization, true);
}

TEST(LLVMCScalarRegions, BooleanUpdatesRetainTheLowBitMask) {
  const auto Source = emitScalarRegions(R"(
define i8 @toggle(i8 %seed, i32 %n) {
entry:
  %bit = trunc i8 %seed to i1
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %body]
  %state = phi i1 [%bit, %entry], [%changed, %body]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %changed = add i1 %state, 1
  %next = add i32 %i, 1
  br label %header
exit:
  %result = zext i1 %state to i8
  ret i8 %result
}
)");
  EXPECT_NE(Source.find("& 1u"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("++state"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (unsigned seed = 0; seed < 256; ++seed)
    for (unsigned n = 0; n < 32; ++n)
      if (toggle(seed, n) != ((seed ^ n) & 1u)) return 1;
  return 0;
}
)",
                  Optimization, true);
}

TEST(LLVMCScalarRegions, RoleNamesCannotHideCalledFunctions) {
  const std::string IR = R"(
declare void @observe(i32)
define i32 @with_observer(i32 %n) {
entry:
  br label %header
header:
  %index = phi i32 [0, %entry], [%next, %body]
  %sum = phi i32 [0, %entry], [%added, %body]
  %more = icmp ult i32 %index, %n
  br i1 %more, label %body, label %exit
body:
  call void @observe(i32 %index)
  %added = add i32 %sum, %index
  %next = add i32 %index, 1
  br label %header
exit:
  ret i32 %sum
}
)";
  const auto Initial = emitScalarRegions(IR);
  // Use actual emitted identifiers so this adversarial input stays valid
  // when the allocator's suffixes change. Both result and counter roles must
  // avoid hiding a directly called function with exactly that identifier.
  for (llvm::StringRef Prefix : {"for (uint32_t ", "    uint32_t "}) {
    const auto At = Initial.find(Prefix.str());
    ASSERT_NE(At, std::string::npos) << Initial;
    const auto Begin = At + Prefix.size();
    const auto End = Initial.find_first_of(" ;", Begin);
    ASSERT_NE(End, std::string::npos) << Initial;
    const std::string Callee = Initial.substr(Begin, End - Begin);
    std::string Changed = IR;
    for (size_t At = 0;
         (At = Changed.find("@observe", At)) != std::string::npos;
         At += Callee.size() + 1)
      Changed.replace(At, 8, "@" + Callee);
    const auto Source = emitScalarRegions(Changed);
    const std::string Main = "static uint32_t calls, observed;\nvoid " +
                             Callee +
                             R"((uint32_t x) { ++calls; observed += x; }
int main(void) {
  for (uint32_t n = 0; n < 32; ++n) {
    calls = observed = 0;
    uint32_t result = with_observer(n);
    if (result != n * (n - 1u) / 2u || calls != n || observed != result)
      return 1;
  }
  return 0;
}
)";
    for (llvm::StringRef Optimization : {"-O0", "-O2"})
      compileAndRun(Source + Main, Optimization, true);
  }
}

TEST(LLVMCScalarRegions, ConditionalPhiUpdateRetainsParallelOldValueSnapshot) {
  const auto Source = emitScalarRegions(R"(
define i32 @conditional_pair(i32 %seed, i32 %other, i32 %n) {
entry:
  br label %header
header:
  %i = phi i32 [0, %entry], [%next, %body]
  %a = phi i32 [%seed, %entry], [%updated, %body]
  %b = phi i32 [%other, %entry], [%a, %body]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %bit = and i32 %b, 1
  %condition = icmp eq i32 %bit, 0
  %selected = select i1 %condition, i32 0, i32 7
  %base = add i32 %a, 5
  %updated = add i32 %base, %selected
  %next = add i32 %i, 1
  br label %header
exit:
  %answer = xor i32 %a, %b
  ret i32 %answer
}
)");
  EXPECT_NE(Source.find("phi_edge"), std::string::npos) << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t seed = 0; seed < 32; ++seed)
    for (uint32_t other = 0; other < 32; ++other)
      for (unsigned n = 0; n < 16; ++n) {
        uint32_t a = UINT32_MAX - seed, b = other;
        for (unsigned i = 0; i < n; ++i) {
          uint32_t old = a;
          a += 5u + (b & 1 ? 7u : 0u);
          b = old;
        }
        if (conditional_pair(UINT32_MAX - seed, other, n) != (a ^ b))
          return 1;
      }
  return 0;
}
)",
                  Optimization, true);
}

TEST(LLVMCScalarRegions, BranchDependentSeedsKeepTheResultInSharedScope) {
  const auto Source = emitScalarRegions(R"(
define i32 @branch_seed(i32 %seed, i32 %n, i1 %choose) {
entry:
  br i1 %choose, label %left, label %right
left:
  %a = add i32 %seed, 9
  br label %header
right:
  %b = xor i32 %seed, 11
  br label %header
header:
  %i = phi i32 [0, %left], [0, %right], [%next, %body]
  %state = phi i32 [%a, %left], [%b, %right], [%updated, %body]
  %more = icmp ult i32 %i, %n
  br i1 %more, label %body, label %exit
body:
  %updated = add i32 %state, %i
  %next = add i32 %i, 1
  br label %header
exit:
  ret i32 %state
}
)");
  const auto At = Source.find("    uint32_t result");
  ASSERT_NE(At, std::string::npos) << Source;
  EXPECT_EQ(Source.substr(At, Source.find('\n', At) - At).find('='),
            std::string::npos)
      << Source;
  for (llvm::StringRef Optimization : {"-O0", "-O2"})
    compileAndRun(Source + R"(
int main(void) {
  for (uint32_t seed = 0; seed < 32; ++seed)
    for (unsigned n = 0; n < 16; ++n)
      for (unsigned choose = 0; choose < 2; ++choose) {
        uint32_t input = UINT32_MAX - seed;
        uint32_t expected = choose ? input + 9u : input ^ 11u;
        for (unsigned i = 0; i < n; ++i) expected += i;
        if (branch_seed(input, n, choose) != expected) return 1;
      }
  return 0;
}
)",
                  Optimization, true);
}

} // namespace

TEST(LLVMCBranches, UnconditionalCyclesRetainTheirControlFlow) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
define i32 @cycle(i32 %choice) {
entry:
  switch i32 %choice, label %done [i32 0, label %self
                                  i32 1, label %left
                                  i32 2, label %right]
self:
  %prior = phi i64 [0, %entry], [%next, %self]
  %next = add i64 %prior, 1
  br label %self
left:
  br label %right
right:
  br label %left
done:
  ret i32 7
}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(neverd::LLVMCEmitter().emit(*Module, Output, {}));
  Source += R"(
#if defined(__unix__)
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
static void still_running(int signal_number) { _Exit(0); }
#endif
int main(void) {
  if (cycle(3) != 7) return 1;
#if defined(__unix__)
  /* Every entry to either cycle must keep running until the observer fires. */
  for (int choice = 0; choice != 3; ++choice) {
    pid_t child = fork();
    if (child < 0) return 2;
    if (!child) {
      signal(SIGALRM, still_running);
      alarm(1);
      (void)cycle(choice);
      _Exit(3);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) return 4;
  }
#endif
  return 0;
}
)";
  for (const char *Optimization : {"-O0", "-O2"})
    compileAndRun(Source, Optimization);
}
