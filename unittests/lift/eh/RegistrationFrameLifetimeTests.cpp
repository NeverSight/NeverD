//===- RegistrationFrameLifetimeTests.cpp - Current allocation life ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish live storage, initialized contents and retained SSA values.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameProof.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/RegistrationFrameXor.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IR/WinEHFrame.h"

#include <array>
#include <limits>
#include <optional>
#include <string>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

struct LifetimeFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"frame-lifetime", Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(Context, "entry", Function)};
  llvm::AllocaInst *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));
  llvm::Value *Slot = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(32));
  llvm::CallInst *Borrow = nullptr;

  LifetimeFixture() {
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  }
  void finish(llvm::Value *Result) {
    auto Callee = Module.getOrInsertFunction(
        "no_memory_member",
        llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false));
    Borrow = B.CreateCall(Callee, {Frame});
    Borrow->setCallingConv(llvm::CallingConv::X86_ThisCall);
    B.CreateRet(Result);
  }
  std::optional<XorValue> word(const llvm::Instruction &Before) {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 1> Functions{Function};
    if (!Stores.build(Functions, Work))
      return std::nullopt;
    AddressProof Addresses(*Function, nullptr, 0, Stores, Work, Exhausted);
    XorProof Values(Addresses, 0, Work, Exhausted);
    return Values.readWordAt({Frame, 32}, Before);
  }
  bool privacy(bool MemberWrites = false) {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    BinaryImage Image;
    coff_registration::RegistrationCxxFrameContract Contract;
    Contract.Image = &Image;
    auto &Effect = Contract.Borrows[Borrow];
    if (MemberWrites)
      Effect.Writes = {{32, 36}};
    auto Error = coff_registration::validateFramePrivacy(
        *Function, {}, Frame, {}, {}, {}, {}, 0, {}, &Contract);
    const bool Valid = !bool(Error);
    llvm::consumeError(std::move(Error));
    return Valid;
  }
};

TEST(RegistrationFrameLifetime, WritesCannotReviveDeadStorageOrOldContents) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LifetimeFixture F;
    if (Mutation >= 3 && Mutation <= 6)
      F.B.CreateLifetimeStart(F.Frame);
    if (Mutation != 7)
      F.B.CreateStore(F.B.getInt32(42), F.Slot);
    if (Mutation == 1 || Mutation == 3 || Mutation >= 5)
      F.B.CreateLifetimeEnd(F.Frame);
    if (Mutation == 2 || Mutation == 3 || Mutation == 4 || Mutation == 5 ||
        Mutation == 7)
      F.B.CreateLifetimeStart(F.Frame);
    if (Mutation == 1 || Mutation == 2 || Mutation == 3 || Mutation == 7)
      F.B.CreateStore(F.B.getInt32(43), F.Slot);
    auto *Read = F.B.CreateLoad(F.B.getInt32Ty(), F.Slot);
    F.finish(Read);
    const bool CurrentWord =
        Mutation == 0 || Mutation == 2 || Mutation == 3 || Mutation == 7;
    EXPECT_EQ(F.word(*Read).has_value(), CurrentWord);
    // Mode 2 has a valid new word, but the earlier write before the first
    // lifetime.start is still invalid. A value query is not whole-IR proof.
    EXPECT_EQ(F.privacy(), CurrentWord && Mutation != 2);
  }
}

TEST(RegistrationFrameLifetime, JoinsRequireEveryIncomingAllocationToBeLive) {
  for (bool Restore : {false, true}) {
    SCOPED_TRACE(Restore);
    LifetimeFixture F;
    F.B.CreateLifetimeStart(F.Frame);
    F.B.CreateStore(F.B.getInt32(42), F.Slot);
    auto *Left = llvm::BasicBlock::Create(F.Context, "left", F.Function);
    auto *Right = llvm::BasicBlock::Create(F.Context, "right", F.Function);
    auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Function);
    F.B.CreateCondBr(F.Function->getArg(0), Left, Right);
    F.B.SetInsertPoint(Left);
    F.B.CreateLifetimeEnd(F.Frame);
    if (Restore) {
      F.B.CreateLifetimeStart(F.Frame);
      F.B.CreateStore(F.B.getInt32(42), F.Slot);
    }
    F.B.CreateBr(Join);
    F.B.SetInsertPoint(Right);
    F.B.CreateBr(Join);
    F.B.SetInsertPoint(Join);
    auto *Read = F.B.CreateLoad(F.B.getInt32Ty(), F.Slot);
    F.finish(Read);
    EXPECT_EQ(F.word(*Read).has_value(), Restore);
    EXPECT_EQ(F.privacy(), Restore);
  }
}

TEST(RegistrationFrameLifetime, LoopSuffixesCannotReuseAnEndedLifetime) {
  for (bool Restore : {false, true}) {
    SCOPED_TRACE(Restore);
    LifetimeFixture F;
    F.B.CreateLifetimeStart(F.Frame);
    F.B.CreateStore(F.B.getInt32(42), F.Slot);
    auto *Loop = llvm::BasicBlock::Create(F.Context, "loop", F.Function);
    auto *Exit = llvm::BasicBlock::Create(F.Context, "exit", F.Function);
    F.B.CreateBr(Loop);
    F.B.SetInsertPoint(Loop);
    auto *Read = F.B.CreateLoad(F.B.getInt32Ty(), F.Slot);
    F.B.CreateLifetimeEnd(F.Frame);
    if (Restore) {
      F.B.CreateLifetimeStart(F.Frame);
      F.B.CreateStore(F.B.getInt32(42), F.Slot);
    }
    F.B.CreateCondBr(F.Function->getArg(0), Loop, Exit);
    F.B.SetInsertPoint(Exit);
    F.finish(Read);
    EXPECT_EQ(F.word(*Read).has_value(), Restore);
    EXPECT_EQ(F.privacy(), Restore);
  }
}

TEST(RegistrationFrameLifetime, AnyStartUseMakesTheAllocationInitiallyDead) {
  for (bool Start : {false, true}) {
    SCOPED_TRACE(Start);
    LifetimeFixture F;
    F.B.CreateStore(F.B.getInt32(42), F.Slot);
    auto *Read = F.B.CreateLoad(F.B.getInt32Ty(), F.Slot);
    F.finish(Read);
    F.B.SetInsertPoint(
        llvm::BasicBlock::Create(F.Context, "unreachable", F.Function));
    if (Start)
      F.B.CreateLifetimeStart(F.Frame);
    else
      F.B.CreateLifetimeEnd(F.Frame);
    F.B.CreateUnreachable();
    EXPECT_EQ(F.word(*Read).has_value(), !Start);
    EXPECT_EQ(F.privacy(), !Start);
  }
}

TEST(RegistrationFrameLifetime, PoisonMarkersHaveNoEffect) {
  LifetimeFixture F;
  F.B.CreateStore(F.B.getInt32(42), F.Slot);
  auto *Poison = llvm::PoisonValue::get(F.B.getPtrTy());
  F.B.CreateLifetimeEnd(Poison);
  F.B.CreateLifetimeStart(Poison);
  auto *Read = F.B.CreateLoad(F.B.getInt32Ty(), F.Slot);
  F.finish(Read);
  EXPECT_EQ(F.word(*Read), (std::optional<XorValue>{{42, false, {}}}));
  EXPECT_TRUE(F.privacy());
}

TEST(RegistrationFrameLifetime, DynamicStorageIsExplicitlyUnsupported) {
  LifetimeFixture F;
  auto *Dynamic = F.B.CreateAlloca(
      F.B.getInt8Ty(), F.B.CreateZExt(F.Function->getArg(0), F.B.getInt32Ty()));
  ASSERT_FALSE(Dynamic->isStaticAlloca());
  F.B.CreateLifetimeStart(Dynamic);
  F.finish(F.B.getInt32(0));
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  auto Error =
      coff_registration::validateFramePrivacy(*F.Function, {}, F.Frame, {}, {});
  ASSERT_TRUE(bool(Error));
  EXPECT_NE(llvm::toString(std::move(Error)).find("unsupported storage"),
            std::string::npos);
}

TEST(RegistrationFrameLifetime, MemberWritesRequireLiveStorage) {
  for (bool Restore : {false, true}) {
    SCOPED_TRACE(Restore);
    LifetimeFixture F;
    F.B.CreateLifetimeStart(F.Frame);
    F.B.CreateStore(F.B.getInt32(42), F.Slot);
    F.B.CreateLifetimeEnd(F.Frame);
    if (Restore)
      F.B.CreateLifetimeStart(F.Frame);
    F.finish(F.B.getInt32(0));
    EXPECT_EQ(F.privacy(true), Restore);
  }
}

TEST(RegistrationFrameLifetime, RuntimeCatchCopiesRequireLiveStorage) {
  for (bool Object : {false, true})
    for (unsigned Lifetime = 0; Lifetime != 3; ++Lifetime) {
      SCOPED_TRACE(Object);
      SCOPED_TRACE(Lifetime);
      LifetimeFixture F;
      F.Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
      F.Function->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
      F.Function->addFnAttr("frame-pointer", "all");
      auto Personality = F.Module.getOrInsertFunction(
          "__CxxFrameHandler3",
          llvm::FunctionType::get(F.B.getInt32Ty(), true));
      F.Function->setPersonalityFn(
          llvm::cast<llvm::Constant>(Personality.getCallee()));
      auto Block = [&](const char *Name) {
        return llvm::BasicBlock::Create(F.Context, Name, F.Function);
      };
      auto *Normal = Block("normal"), *Dispatch = Block("dispatch");
      auto *Handler = Block("handler"), *Exit = Block("exit");
      F.B.CreateLifetimeStart(F.Frame);
      if (Lifetime)
        F.B.CreateLifetimeEnd(F.Frame);
      if (Lifetime == 2)
        F.B.CreateLifetimeStart(F.Frame);
      auto Trigger = F.Module.getOrInsertFunction(
          "trigger", llvm::FunctionType::get(F.B.getVoidTy(), false));
      F.B.CreateInvoke(Trigger, Normal, Dispatch);
      F.B.SetInsertPoint(Normal);
      F.B.CreateRet(F.B.getInt32(0));
      F.B.SetInsertPoint(Dispatch);
      auto *Switch = F.B.CreateCatchSwitch(
          llvm::ConstantTokenNone::get(F.Context), nullptr, 1);
      Switch->addHandler(Handler);
      F.B.SetInsertPoint(Handler);
      auto *Null = llvm::ConstantPointerNull::get(F.B.getPtrTy());
      llvm::Value *Home = Object ? static_cast<llvm::Value *>(F.Frame) : Null;
      auto *Pad = F.B.CreateCatchPad(Switch, {Null, F.B.getInt32(0), Home});
      if (Object)
        Pad->setMetadata(
            llvm::RewriteWinX86CxxCatchObjectAttachment,
            llvm::MDNode::get(
                F.Context, {llvm::ConstantAsMetadata::get(F.B.getInt32(1)),
                            llvm::ConstantAsMetadata::get(F.B.getInt32(32)),
                            llvm::ConstantAsMetadata::get(F.B.getInt32(4))}));
      F.B.CreateCatchRet(Pad, Exit);
      F.B.SetInsertPoint(Exit);
      F.B.CreateRet(F.B.getInt32(0));
      ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
      BinaryImage Image;
      coff_registration::RegistrationCxxFrameContract Contract;
      Contract.Image = &Image;
      auto &Catch = Contract.Catches.emplace_back();
      Catch.Catch = Pad;
      Catch.HomeOffset = Object ? 32 : 0;
      Catch.ObjectSize = Object ? 4 : 0;
      Catch.CallbackBlocks = {Handler};
      auto Error = coff_registration::validateFramePrivacy(
          *F.Function, {}, F.Frame, {}, {}, {}, {}, 0, {}, &Contract);
      EXPECT_EQ(bool(Error), Object && Lifetime == 1)
          << llvm::toString(std::move(Error));
      llvm::consumeError(std::move(Error));
    }
}

TEST(RegistrationFrameLifetime, MarkedParentNeedsCallbackLifetimeProjection) {
  for (bool Marked : {false, true}) {
    SCOPED_TRACE(Marked);
    LifetimeFixture F;
    if (Marked)
      F.B.CreateLifetimeStart(F.Frame);
    F.finish(F.B.getInt32(0));
    auto *Callback = llvm::Function::Create(
        llvm::FunctionType::get(F.B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "callback", F.Module);
    F.B.SetInsertPoint(llvm::BasicBlock::Create(F.Context, "entry", Callback));
    auto *Return = F.B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 2> Functions{F.Function, Callback};
    ASSERT_TRUE(Stores.build(Functions, Work));
    AddressProof Addresses(*F.Function, nullptr, 0, Stores, Work, Exhausted);
    EXPECT_EQ(Addresses.isAliveAt(*F.Frame, *Return), !Marked);
  }
}

TEST(RegistrationFrameLifetime,
     EndingStorageDoesNotInvalidateOldSSAOrAddresses) {
  LifetimeFixture F;
  F.B.CreateLifetimeStart(F.Frame);
  F.B.CreateStore(F.B.getInt32(42), F.Slot);
  auto *Read = F.B.CreateLoad(F.B.getInt32Ty(), F.Slot);
  F.B.CreateLifetimeEnd(F.Frame);
  auto *Pointer = F.B.CreateGEP(F.B.getInt8Ty(), F.Frame, F.B.getInt32(16));
  F.finish(Read);
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  const std::array<const llvm::Function *, 1> Functions{F.Function};
  ASSERT_TRUE(Stores.build(Functions, Work));
  AddressProof Addresses(*F.Function, nullptr, 0, Stores, Work, Exhausted);
  XorProof Values(Addresses, 0, Work, Exhausted);
  EXPECT_EQ(Values.evaluate(Read), (std::optional<XorValue>{{42, false, {}}}));
  EXPECT_EQ(Addresses.locate(Pointer), (std::optional<Cell>{{F.Frame, 16}}));
  EXPECT_FALSE(Addresses.isAliveAt(*F.Frame, *F.Borrow));
  EXPECT_TRUE(F.privacy());
  Work = std::numeric_limits<size_t>::max();
  EXPECT_FALSE(Addresses.isAliveAt(*F.Frame, *Read));
  EXPECT_TRUE(Exhausted);
  EXPECT_EQ(Work, std::numeric_limits<size_t>::max());
  Work = 0;
  EXPECT_FALSE(Addresses.isAliveAt(*F.Frame, *Read));
}

} // namespace
