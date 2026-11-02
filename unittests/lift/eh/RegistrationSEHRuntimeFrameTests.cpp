//===- RegistrationSEHRuntimeFrameTests.cpp - SEH runtime memory proof ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep saved frame addresses across bound runtimes only with callback closure.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/RegistrationFrameProof.h"
#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <utility>

namespace {
using namespace neverd::coff_registration;

TEST(RegistrationSEHRuntimeFrame, NoBorrowNeedsCompleteCallbackPreservation) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("runtime-frame", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    llvm::IRBuilder<> B(llvm::BasicBlock::Create(Context, "entry", Parent));
    auto *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));
    B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                     &Module, llvm::Intrinsic::localescape),
                 {Frame});
    auto *Saved = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(32));
    auto *Address = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(16));
    B.CreateStore(B.CreatePtrToInt(Address, B.getInt32Ty()), Saved);
    auto Runtime = Module.getOrInsertFunction(
        "bound_runtime", llvm::FunctionType::get(B.getVoidTy(), false));
    auto *Call = B.CreateCall(Runtime);
    RegistrationSEHJumpContract Jumps;
    Jumps.Calls.insert(Call);
    if (Mutation != 1)
      Jumps.NoFrameBorrowCalls.insert(Call);
    if (Mutation == 6)
      Jumps.Calls.insert(B.CreateCall(Runtime));
    auto *Reload = B.CreateLoad(B.getInt32Ty(), Saved);
    B.CreateStore(B.getInt32(42), B.CreateIntToPtr(Reload, B.getPtrTy()));
    B.CreateRetVoid();

    auto *Callback = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getInt8Ty(), B.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage, "callback", Module);
    B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Callback));
    auto *Recovered =
        B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                         &Module, llvm::Intrinsic::localrecover),
                     {Parent, Callback->getArg(1), B.getInt32(0)});
    if (Mutation >= 2 && Mutation <= 4) {
      const unsigned Offset = Mutation == 2 ? 32 : Mutation == 3 ? 31 : 36;
      B.CreateStore(B.getInt32(0),
                    B.CreateGEP(B.getInt8Ty(), Recovered, B.getInt32(Offset)));
    }
    if (Mutation == 5)
      B.CreateCall(Runtime);
    B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    RegistrationSEHExceptionContract SEH;
    auto Error =
        validateFramePrivacy(*Parent, {Callback}, Frame, {}, {}, {}, {}, 0, {},
                             nullptr, Mutation == 7 ? nullptr : &SEH, &Jumps);
    const bool Failed = bool(Error);
    EXPECT_EQ(Failed, Mutation != 0 && Mutation != 4)
        << llvm::toString(std::move(Error));
    llvm::consumeError(std::move(Error));
  }
}

TEST(RegistrationSEHRuntimeFrame, ParentResumeUsesTheBoundCallbackSpill) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("parent-resume", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    auto *Dispatch = llvm::BasicBlock::Create(Context, "dispatch", Parent);
    auto *Resume = llvm::BasicBlock::Create(Context, "resume", Parent);
    auto *Invalid = llvm::BasicBlock::Create(Context, "invalid", Parent);
    llvm::IRBuilder<> B(Entry);
    auto *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));
    auto *Saved = B.CreateAlloca(B.getInt32Ty());
    auto *JumpContext = B.CreateAlloca(B.getPtrTy(), B.getInt32(5));
    B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                     &Module, llvm::Intrinsic::localescape),
                 {Frame, Saved, JumpContext});
    B.CreateBr(Dispatch);
    B.SetInsertPoint(Dispatch);
    auto *Select = B.CreateSwitch(B.getInt32(1), Invalid, 1);
    Select->addCase(B.getInt32(1), Resume);
    B.SetInsertPoint(Invalid);
    B.CreateUnreachable();
    B.SetInsertPoint(Resume);
    auto *Pointer = B.CreateLoad(B.getInt32Ty(), Saved);
    B.CreateStore(B.getInt32(42), B.CreateIntToPtr(Pointer, B.getPtrTy()));
    B.CreateRetVoid();
    auto *Callback = llvm::Function::Create(
        llvm::FunctionType::get(B.getVoidTy(), {B.getInt8Ty(), B.getPtrTy()},
                                false),
        llvm::GlobalValue::InternalLinkage, "callback", Module);
    auto *Transfer = llvm::BasicBlock::Create(Context, "transfer", Callback);
    B.SetInsertPoint(Transfer);
    auto Recover = [&](unsigned Index) {
      return B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                              &Module, llvm::Intrinsic::localrecover),
                          {Parent, Callback->getArg(1), B.getInt32(Index)});
    };
    llvm::Value *Address = Recover(0);
    auto *Slot = Recover(1);
    auto *JumpBuffer = Recover(2);
    if (Mutation == 5)
      Address = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(B.CreatePtrToInt(Address, B.getInt32Ty()), Slot);
    if (Mutation == 3)
      B.CreateStore(B.getInt32(0), Slot);
    if (Mutation == 4)
      B.CreateStore(B.getInt8(0), Slot);
    auto *Jump = B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(
            &Module, Mutation == 6 ? llvm::Intrinsic::eh_sjlj_setjmp
                                   : llvm::Intrinsic::eh_sjlj_longjmp),
        {JumpBuffer});
    B.CreateUnreachable();
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    RegistrationSEHParentExitContract Exits;
    Exits.Dispatch = Select;
    if (Mutation != 1)
      Exits.Calls.insert(Jump);
    if (Mutation != 2)
      Exits.ResumptionPredecessors[Resume].push_back(Transfer);
    RegistrationSEHExceptionContract SEH;
    auto Error =
        validateFramePrivacy(*Parent, {Callback}, Frame, {}, {}, {}, {}, 0, {},
                             nullptr, &SEH, nullptr, nullptr, &Exits);
    const bool Failed = bool(Error);
    EXPECT_EQ(Failed, Mutation != 0) << llvm::toString(std::move(Error));
  }
}
} // namespace
