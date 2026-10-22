//===- RegistrationFrameXorTests.cpp - PE32 cookie value recipes
//-----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish exact decoded cookies from remaining frame/address bits.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameXor.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ModRef.h"

#include <array>
#include <limits>
#include <optional>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

struct XorFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"cookie-values", Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(Context, "entry", Function)};
  llvm::AllocaInst *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));
  llvm::Value *Base = B.CreatePtrToInt(Frame, B.getInt32Ty());
  llvm::GlobalVariable *Global = new llvm::GlobalVariable(
      Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
      nullptr, "__nd_data_403010");
  llvm::Value *Cookie = B.CreateLoad(B.getInt32Ty(), Global);
  llvm::Value *Encoded = B.CreateXor(Cookie, Base);
  llvm::Value *Slot = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(32));

  XorFixture() {
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    Frame->setAlignment(llvm::Align(16));
  }

  std::optional<XorValue> query(llvm::Value *Value, size_t InitialWork = 0,
                                AddressProof::MemoryProjection Preserves = {}) {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    size_t Work = InitialWork;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 1> Functions{Function};
    if (!Stores.build(Functions, Work))
      return std::nullopt;
    AddressProof Addresses(*Function, nullptr, 0x403010, Stores, Work,
                           Exhausted, {}, {}, {}, Preserves);
    XorProof Values(Addresses, 0x403010, Work, Exhausted);
    auto Result = Values.evaluate(Value);
    return Exhausted ? std::nullopt : Result;
  }

  std::optional<XorValue>
  queryAt(const llvm::Instruction &Before,
          AddressProof::MemoryProjection Preserves = {}) {
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 1> Functions{Function};
    if (!Stores.build(Functions, Work))
      return std::nullopt;
    AddressProof Addresses(*Function, nullptr, 0x403010, Stores, Work,
                           Exhausted, {}, {}, {}, Preserves);
    const auto Where = Addresses.locate(Slot);
    if (!Where)
      return std::nullopt;
    XorProof Values(Addresses, 0x403010, Work, Exhausted);
    auto Result = Values.readWordAt(*Where, Before);
    return Exhausted ? std::nullopt : Result;
  }
};

TEST(RegistrationFrameXor, CancelsCurrentFrameAddressThroughFullSpills) {
  XorFixture F;
  F.B.CreateStore(F.Encoded, F.Slot);
  auto *Carrier = F.B.CreateAlloca(F.B.getInt64Ty());
  F.B.CreateStore(F.B.CreateZExt(F.Base, F.B.getInt64Ty()), Carrier);
  auto *Base = F.B.CreateTrunc(F.B.CreateLoad(F.B.getInt64Ty(), Carrier),
                               F.B.getInt32Ty());
  auto *Decoded = F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), Base);
  F.B.CreateRet(Decoded);
  EXPECT_EQ(F.query(Decoded), (std::optional<XorValue>{{0, true, {}}}));
}

TEST(RegistrationFrameXor, DistinctRootsAndCoordinatesDoNotCancel) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    XorFixture F;
    F.B.CreateStore(F.Encoded, F.Slot);
    llvm::Value *Base = nullptr;
    if (Mutation == 0)
      Base = F.B.CreatePtrToInt(F.B.CreateAlloca(F.B.getInt32Ty()),
                                F.B.getInt32Ty());
    else if (Mutation == 1)
      Base = F.B.CreateAdd(F.Base, F.B.getInt32(4));
    else
      Base = F.B.CreateAnd(F.Base, F.B.getInt32(-32));
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), Base);
    F.B.CreateRet(Decoded);
    EXPECT_NE(F.query(Decoded), (std::optional<XorValue>{{0, true, {}}}));
  }
}

TEST(RegistrationFrameXor, EveryReachingPathMustEstablishTheSameValue) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    XorFixture F;
    auto *Left = llvm::BasicBlock::Create(F.Context, "left", F.Function);
    auto *Right = llvm::BasicBlock::Create(F.Context, "right", F.Function);
    auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Function);
    F.B.CreateCondBr(F.Function->getArg(0), Left, Right);
    F.B.SetInsertPoint(Left);
    F.B.CreateStore(F.Encoded, F.Slot);
    F.B.CreateBr(Join);
    F.B.SetInsertPoint(Right);
    if (Mutation != 1) {
      llvm::Value *Value = F.Encoded;
      if (Mutation == 2)
        Value = F.B.getInt32(99);
      if (Mutation == 3)
        Value = F.B.CreateTrunc(F.Encoded, F.B.getInt16Ty());
      auto *Store = F.B.CreateStore(Value, F.Slot);
      if (Mutation == 4)
        Store->setAtomic(llvm::AtomicOrdering::Release);
    }
    F.B.CreateBr(Join);
    F.B.SetInsertPoint(Join);
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
    F.B.CreateRet(Decoded);
    EXPECT_EQ((F.query(Decoded) == std::optional<XorValue>{{0, true, {}}}),
              Mutation == 0);
  }
}

TEST(RegistrationFrameXor, LoopSuffixWritesCannotReuseAnOldRecipe) {
  for (bool Change : {false, true}) {
    SCOPED_TRACE(Change);
    XorFixture F;
    F.B.CreateStore(F.Encoded, F.Slot);
    auto *Loop = llvm::BasicBlock::Create(F.Context, "loop", F.Function);
    auto *Done = llvm::BasicBlock::Create(F.Context, "done", F.Function);
    F.B.CreateBr(Loop);
    F.B.SetInsertPoint(Loop);
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
    F.B.CreateStore(Change ? F.B.getInt32(99) : F.Encoded, F.Slot);
    F.B.CreateCondBr(F.Function->getArg(0), Loop, Done);
    F.B.SetInsertPoint(Done);
    F.B.CreateRet(Decoded);
    EXPECT_EQ((F.query(Decoded) == std::optional<XorValue>{{0, true, {}}}),
              !Change);
  }
}

TEST(RegistrationFrameXor, RejectsNarrowPoisonAtomicAndForeignCookieValues) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    XorFixture F;
    llvm::Value *Value = F.Cookie;
    switch (Mutation) {
    case 0:
      F.Global->setName("__nd_data_403014");
      break;
    case 1:
      F.Global->setInitializer(F.B.getInt32(123));
      break;
    case 2:
      llvm::cast<llvm::LoadInst>(F.Cookie)->setAtomic(
          llvm::AtomicOrdering::Acquire);
      break;
    case 3:
      Value = F.B.CreateZExt(F.B.CreateTrunc(Value, F.B.getInt16Ty()),
                             F.B.getInt32Ty());
      break;
    case 4:
      Value = llvm::PoisonValue::get(F.B.getInt32Ty());
      break;
    case 5:
      Value = F.B.CreateNSWAdd(F.Base, F.B.getInt32(4));
      break;
    case 6:
      F.Module.setDataLayout("e-m:e-p:64:64-i64:64-n8:16:32:64-S128");
      break;
    }
    F.B.CreateRet(Value);
    EXPECT_NE(F.query(Value), (std::optional<XorValue>{{0, true, {}}}));
  }
}

TEST(RegistrationFrameXor, ExhaustionCannotPublishAValue) {
  XorFixture F;
  F.B.CreateRet(F.Cookie);
  EXPECT_FALSE(F.query(F.Cookie, limits::kMaxRegistrationEHStateWork));
}

TEST(RegistrationFrameXor, UndefinedAddressRecipesCannotCancelTheirPoison) {
  for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
    SCOPED_TRACE(Mutation);
    XorFixture F;
    llvm::Value *Base = nullptr;
    if (Mutation == 0) {
      auto *PoisonBase = F.B.CreateNSWAdd(F.Base, F.B.getInt32(4));
      Base = F.B.CreateAdd(PoisonBase, F.B.getInt32(4));
    } else {
      auto *Outside =
          F.B.CreateInBoundsGEP(F.B.getInt8Ty(), F.Frame, F.B.getInt32(-4));
      auto *Back =
          F.B.CreateInBoundsGEP(F.B.getInt8Ty(), Outside, F.B.getInt32(4));
      Base = F.B.CreatePtrToInt(Back, F.B.getInt32Ty());
    }
    auto *Encoded = F.B.CreateXor(F.Cookie, Base);
    F.B.CreateStore(Encoded, F.Slot);
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), Base);
    F.B.CreateRet(Decoded);
    EXPECT_FALSE(F.query(Decoded));
  }
}

TEST(RegistrationFrameXor, DispatchPointReadsTheCurrentWordNotAnEarlierLoad) {
  XorFixture F;
  F.B.CreateStore(F.Encoded, F.Slot);
  auto *Decoded =
      F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
  auto *Callee = llvm::Function::Create(
      llvm::FunctionType::get(F.B.getVoidTy(), {}, false),
      llvm::GlobalValue::ExternalLinkage, "callee", F.Module);
  auto *Before = F.B.CreateCall(Callee);
  F.B.CreateStore(F.B.getInt32(0), F.Slot);
  auto *After = F.B.CreateCall(Callee);
  F.B.CreateRet(Decoded);
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  const auto Old = F.queryAt(*Before);
  ASSERT_TRUE(Old);
  EXPECT_TRUE(Old->Cookie);
  EXPECT_EQ(Old->Constant, 0u);
  EXPECT_EQ(Old->Addresses, (std::set<Cell>{{F.Frame, 0}}));
  EXPECT_EQ(F.queryAt(*After), (std::optional<XorValue>{{0, false, {}}}));
  EXPECT_EQ(F.query(Decoded), (std::optional<XorValue>{{0, true, {}}}));
}

TEST(RegistrationFrameXor, AnExhaustedCounterCannotWrapOrReviveARecipe) {
  XorFixture F;
  F.B.CreateRet(F.Cookie);
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  const std::array<const llvm::Function *, 1> Functions{F.Function};
  ASSERT_TRUE(Stores.build(Functions, Work));
  AddressProof Addresses(*F.Function, nullptr, 0x403010, Stores, Work,
                         Exhausted);
  XorProof Values(Addresses, 0x403010, Work, Exhausted);
  Work = std::numeric_limits<size_t>::max();
  EXPECT_FALSE(Values.evaluate(F.B.getInt32(0)));
  EXPECT_TRUE(Exhausted);
  EXPECT_EQ(Work, std::numeric_limits<size_t>::max());
  Work = 0;
  EXPECT_FALSE(Values.evaluate(F.B.getInt32(0)));
  EXPECT_FALSE(Addresses.locate(F.Frame));
}

TEST(RegistrationFrameXor, InterveningCallsNeedCurrentWriteEffects) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    XorFixture F;
    auto Callee = F.Module.getOrInsertFunction(
        "callee",
        llvm::FunctionType::get(F.B.getVoidTy(), {F.B.getPtrTy()}, false));
    if (Mutation != 2)
      F.B.CreateStore(F.Encoded, F.Slot);
    auto *Call = F.B.CreateCall(Callee, {F.Slot});
    if (Mutation == 1) {
      Call->setMemoryEffects(llvm::MemoryEffects::none());
      llvm::cast<llvm::Function>(Callee.getCallee())
          ->setMemoryEffects(llvm::MemoryEffects::none());
    }
    if (Mutation == 2 || Mutation == 3)
      F.B.CreateStore(F.Encoded, F.Slot);
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
    auto *Point = F.B.CreateCall(Callee, {F.Slot});
    F.B.CreateRet(Decoded);
    auto Preserves = [&](const llvm::Instruction &I, const Cell &Slot,
                         uint64_t Bytes) {
      // Model a separately authenticated call writing frame bytes [0,4).
      // The alternate write [31,33) overlaps one queried cookie byte.
      const int64_t Begin = Mutation == 5 ? 31 : 0;
      const int64_t End = Mutation == 5 ? 33 : 4;
      return &I == Call && (Mutation == 4 || Mutation == 5) &&
             (Slot.Root != F.Frame || Slot.Offset >= End ||
              Slot.Offset + int64_t(Bytes) <= Begin);
    };
    const bool Valid = Mutation >= 2 && Mutation <= 4;
    EXPECT_EQ(F.query(Decoded, 0, Preserves).has_value(), Valid);
    EXPECT_EQ(F.queryAt(*Point, Preserves).has_value(), Valid);
  }
}

TEST(RegistrationFrameXor, CallsOnJoinsAndLoopSuffixesInvalidateTheWord) {
  for (bool Loop : {false, true}) {
    SCOPED_TRACE(Loop);
    XorFixture F;
    F.B.CreateStore(F.Encoded, F.Slot);
    auto Callee = F.Module.getOrInsertFunction(
        "clobber",
        llvm::FunctionType::get(F.B.getVoidTy(), {F.B.getPtrTy()}, false));
    auto *Read = llvm::BasicBlock::Create(F.Context, "read", F.Function);
    auto *Write = llvm::BasicBlock::Create(F.Context, "write", F.Function);
    auto *Exit = llvm::BasicBlock::Create(F.Context, "exit", F.Function);
    if (Loop)
      F.B.CreateBr(Read);
    else
      F.B.CreateCondBr(F.Function->getArg(0), Read, Write);
    F.B.SetInsertPoint(Write);
    F.B.CreateCall(Callee, {F.Slot});
    F.B.CreateBr(Read);
    F.B.SetInsertPoint(Read);
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
    if (Loop)
      F.B.CreateCondBr(F.Function->getArg(0), Write, Exit);
    else
      F.B.CreateBr(Exit);
    F.B.SetInsertPoint(Exit);
    F.B.CreateRet(Decoded);
    EXPECT_FALSE(F.query(Decoded));
  }
}

TEST(RegistrationFrameXor, LifetimeChangesCannotReuseEarlierStores) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    XorFixture F;
    F.B.CreateLifetimeStart(F.Frame);
    F.B.CreateStore(F.Encoded, F.Slot);
    auto *Target = Mutation == 3 ? F.B.CreateAlloca(F.B.getInt32Ty()) : F.Frame;
    F.B.CreateLifetimeEnd(Target);
    if (Mutation == 1 || Mutation == 2)
      F.B.CreateLifetimeStart(F.Frame);
    if (Mutation == 2)
      F.B.CreateStore(F.Encoded, F.Slot);
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
    F.B.CreateRet(Decoded);
    EXPECT_EQ(F.query(Decoded).has_value(), Mutation >= 2);
  }
}

TEST(RegistrationFrameXor, PrivateSpillsDoNotAcquireUnrelatedCallAliases) {
  XorFixture F;
  auto *Carrier = F.B.CreateAlloca(F.B.getInt32Ty());
  F.B.CreateStore(F.Encoded, Carrier);
  auto Callee = F.Module.getOrInsertFunction(
      "opaque", llvm::FunctionType::get(F.B.getVoidTy(), false));
  F.B.CreateCall(Callee);
  auto *Decoded =
      F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), Carrier), F.Base);
  F.B.CreateRet(Decoded);
  EXPECT_EQ(F.query(Decoded), (std::optional<XorValue>{{0, true, {}}}));
}

TEST(RegistrationFrameXor, AtomicAndIntrinsicWritesAreNotPlainStores) {
  for (bool Atomic : {false, true}) {
    SCOPED_TRACE(Atomic);
    XorFixture F;
    F.B.CreateStore(F.Encoded, F.Slot);
    if (Atomic)
      F.B.CreateAtomicRMW(llvm::AtomicRMWInst::Xchg, F.Slot, F.B.getInt32(0),
                          llvm::Align(4), llvm::AtomicOrdering::Monotonic);
    else {
      auto *Call = F.B.CreateMemSet(F.Slot, F.B.getInt8(0), 4, llvm::Align(4));
      Call->setMemoryEffects(llvm::MemoryEffects::none());
      Call->getCalledFunction()->setMemoryEffects(llvm::MemoryEffects::none());
    }
    auto *Decoded =
        F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
    F.B.CreateRet(Decoded);
    EXPECT_FALSE(F.query(Decoded));
  }
}

TEST(RegistrationFrameXor, SideEffectMarkersPreservePrivateWords) {
  XorFixture F;
  F.B.CreateStore(F.Encoded, F.Slot);
  F.B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
      &F.Module, llvm::Intrinsic::sideeffect));
  auto *Decoded =
      F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
  F.B.CreateRet(Decoded);
  EXPECT_EQ(F.query(Decoded), (std::optional<XorValue>{{0, true, {}}}));
}

TEST(RegistrationFrameXor, VolatileObservationsDoNotWritePrivateWords) {
  XorFixture F;
  F.B.CreateStore(F.Encoded, F.Slot);
  F.B.CreateLoad(F.B.getInt32Ty(), F.Global)->setVolatile(true);
  auto *Decoded =
      F.B.CreateXor(F.B.CreateLoad(F.B.getInt32Ty(), F.Slot), F.Base);
  F.B.CreateRet(Decoded);
  EXPECT_EQ(F.query(Decoded), (std::optional<XorValue>{{0, true, {}}}));
}
} // namespace
