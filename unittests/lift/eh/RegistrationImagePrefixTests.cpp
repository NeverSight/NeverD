//===- RegistrationImagePrefixTests.cpp - PE32 image observation paths ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check exact observation prefixes without hiding previous loop iterations.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationImageProof.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ModRef.h"

#include <array>
#include <memory>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

constexpr va_t CookieVA = 0x403010;
constexpr ExceptionAddressRange CookieRange{CookieVA, CookieVA + 4};

struct PrefixFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"image-prefix", Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(Context, "entry", Function)};

  PrefixFixture() { Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32"); }
  llvm::GlobalVariable *global(llvm::StringRef Name = "__nd_data_403010") {
    if (auto *Global = Module.getNamedGlobal(Name))
      return Global;
    return new llvm::GlobalVariable(Module, B.getInt32Ty(), false,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
  }
  llvm::LoadInst *observe() { return B.CreateLoad(B.getInt32Ty(), global()); }
  llvm::CallInst *call() {
    return B.CreateCall(Module.getOrInsertFunction(
        "opaque", llvm::FunctionType::get(B.getVoidTy(), false)));
  }
  llvm::BasicBlock *block(llvm::StringRef Name) {
    return llvm::BasicBlock::Create(Context, Name, Function);
  }
  bool preservesBefore(const llvm::Instruction &Before,
                       ImageProof::WriteProjection Project = {}) {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 1> Functions{Function};
    if (!Stores.build(Functions, Work))
      return false;
    AddressProof Addresses(*Function, nullptr, CookieVA, Stores, Work,
                           Exhausted);
    ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted, Project);
    return Image.preservesBefore(CookieRange, Before);
  }
};

TEST(RegistrationImagePrefix, ExcludesObservationAndFutureWrites) {
  PrefixFixture F;
  auto *First = F.observe();
  auto *Store = F.B.CreateStore(F.B.getInt32(0), F.global());
  auto *Later = F.block("later");
  F.B.CreateBr(Later);
  F.B.SetInsertPoint(Later);
  F.B.CreateStore(F.B.getInt8(0), F.global("__security_cookie"));
  auto *Last = F.B.CreateRetVoid();
  EXPECT_TRUE(F.preservesBefore(*First));
  EXPECT_TRUE(F.preservesBefore(*Store));
  EXPECT_FALSE(F.preservesBefore(*Last));
}

TEST(RegistrationImagePrefix, EarlierAliasedPartialWritesReject) {
  for (const char *Name :
       {"__nd_data_403010", "__nd_codeptr_403010", "__security_cookie"})
    for (int Offset : {-1, 0, 3, 4}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(Offset);
      PrefixFixture F;
      F.B.CreateStore(
          F.B.getInt16(0),
          F.B.CreateGEP(F.B.getInt8Ty(), F.global(Name), F.B.getInt32(Offset)));
      auto *Before = F.observe();
      F.B.CreateRetVoid();
      EXPECT_EQ(F.preservesBefore(*Before), Offset == 4);
    }
}

TEST(RegistrationImagePrefix, EveryBranchNeedsIndependentCallEffects) {
  for (bool OnLeft : {false, true}) {
    PrefixFixture F;
    auto *Left = F.block("left");
    auto *Right = F.block("right");
    auto *Join = F.block("join");
    F.B.CreateCondBr(F.Function->getArg(0), Left, Right);
    F.B.SetInsertPoint(OnLeft ? Left : Right);
    auto *Call = F.call();
    Call->setMemoryEffects(llvm::MemoryEffects::none());
    Call->getCalledFunction()->setMemoryEffects(llvm::MemoryEffects::none());
    F.B.CreateBr(Join);
    F.B.SetInsertPoint(OnLeft ? Right : Left);
    F.B.CreateBr(Join);
    F.B.SetInsertPoint(Join);
    auto *Before = F.observe();
    F.B.CreateRetVoid();
    EXPECT_FALSE(F.preservesBefore(*Before));
    EXPECT_TRUE(F.preservesBefore(
        *Before, [&](const auto &I, const auto &) { return &I == Call; }));
  }
}

TEST(RegistrationImagePrefix, SelfLoopIncludesThePreviousSuffix) {
  for (bool Clobber : {false, true}) {
    PrefixFixture F;
    auto *Loop = F.block("loop");
    auto *Exit = F.block("exit");
    F.B.CreateBr(Loop);
    F.B.SetInsertPoint(Loop);
    auto *Before = F.observe();
    if (Clobber)
      F.B.CreateStore(F.B.getInt8(0), F.global("__nd_codeptr_403010"));
    F.B.CreateCondBr(F.Function->getArg(0), Loop, Exit);
    F.B.SetInsertPoint(Exit);
    F.B.CreateRetVoid();
    EXPECT_EQ(F.preservesBefore(*Before), !Clobber);
  }
}

TEST(RegistrationImagePrefix, CallExcludesItselfButNotPreviousInvocations) {
  for (bool Repeated : {false, true}) {
    PrefixFixture F;
    auto *Body = F.block("body");
    auto *Exit = F.block("exit");
    F.B.CreateBr(Body);
    F.B.SetInsertPoint(Body);
    auto *Call = F.call();
    if (Repeated)
      F.B.CreateCondBr(F.Function->getArg(0), Body, Exit);
    else
      F.B.CreateBr(Exit);
    F.B.SetInsertPoint(Exit);
    F.B.CreateRetVoid();
    unsigned Dependencies = 0;
    auto Unknown = [&](const auto &I, const auto &) {
      EXPECT_EQ(&I, Call);
      ++Dependencies;
      return false;
    };
    EXPECT_EQ(F.preservesBefore(*Call, Unknown), !Repeated);
    EXPECT_EQ(Dependencies, Repeated ? 1u : 0u);
  }
}

TEST(RegistrationImagePrefix, CallbackClosureRemainsSeparate) {
  PrefixFixture F;
  auto *Before = F.observe();
  F.B.CreateRetVoid();
  auto *Callback = llvm::Function::Create(
      llvm::FunctionType::get(F.B.getVoidTy(), false),
      llvm::GlobalValue::ExternalLinkage, "callback", F.Module);
  F.B.SetInsertPoint(llvm::BasicBlock::Create(F.Context, "entry", Callback));
  F.B.CreateStore(F.B.getInt32(0), F.global());
  F.B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  const std::array<const llvm::Function *, 2> Functions{F.Function, Callback};
  ASSERT_TRUE(Stores.build(Functions, Work));
  AddressProof Addresses(*F.Function, nullptr, CookieVA, Stores, Work,
                         Exhausted);
  ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted);
  EXPECT_TRUE(Image.preservesBefore(CookieRange, *Before));
  EXPECT_FALSE(Image.preserves(CookieRange));
}

TEST(RegistrationImagePrefix, RejectsForeignUnindexedAndUnlistedObservations) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    PrefixFixture F;
    F.B.CreateRetVoid();
    llvm::Module Foreign{"foreign", F.Context};
    Foreign.setDataLayout(F.Module.getDataLayout());
    auto *Other =
        llvm::Function::Create(llvm::FunctionType::get(F.B.getVoidTy(), false),
                               llvm::GlobalValue::ExternalLinkage, "other",
                               Mutation == 0 ? Foreign : F.Module);
    F.B.SetInsertPoint(llvm::BasicBlock::Create(F.Context, "entry", Other));
    auto *Before = F.B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    ASSERT_FALSE(llvm::verifyModule(Foreign, &llvm::errs()));
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 2> Both{F.Function, Other};
    const std::array<const llvm::Function *, 1> Parent{F.Function};
    ASSERT_TRUE(Stores.build(
        Mutation == 1 ? llvm::ArrayRef<const llvm::Function *>(Parent)
                      : llvm::ArrayRef<const llvm::Function *>(Both),
        Work));
    AddressProof Addresses(*F.Function, nullptr, CookieVA, Stores, Work,
                           Exhausted);
    ImageProof Image(Addresses,
                     Mutation == 2
                         ? llvm::ArrayRef<const llvm::Function *>(Parent)
                         : llvm::ArrayRef<const llvm::Function *>(Both),
                     CookieVA, Work, Exhausted);
    EXPECT_FALSE(Image.preservesBefore(CookieRange, *Before)) << Mutation;
  }
}

TEST(RegistrationImagePrefix, EveryVisitedInstructionMustBeIndexed) {
  PrefixFixture F;
  auto *Before = F.observe();
  F.B.CreateRetVoid();
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  const std::array<const llvm::Function *, 1> Functions{F.Function};
  ASSERT_TRUE(Stores.build(Functions, Work));
  F.B.SetInsertPoint(Before);
  F.B.CreateLoad(F.B.getInt32Ty(), F.global());
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  AddressProof Addresses(*F.Function, nullptr, CookieVA, Stores, Work,
                         Exhausted);
  ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted);
  EXPECT_FALSE(Image.preservesBefore(CookieRange, *Before));
}

TEST(RegistrationImagePrefix, RejectsDetachedObservations) {
  PrefixFixture F;
  F.B.CreateRetVoid();
  auto *Detached = llvm::ReturnInst::Create(F.Context);
  EXPECT_FALSE(F.preservesBefore(*Detached));
  Detached->deleteValue();
  auto Block = std::unique_ptr<llvm::BasicBlock>(
      llvm::BasicBlock::Create(F.Context, "detached"));
  auto *InBlock = llvm::ReturnInst::Create(F.Context, Block.get());
  EXPECT_FALSE(F.preservesBefore(*InBlock));
}

TEST(RegistrationImagePrefix, RejectsClosedCyclesWithoutAnEntryPath) {
  PrefixFixture F;
  F.B.CreateRetVoid();
  auto *Loop = F.block("unreachable");
  F.B.SetInsertPoint(Loop);
  auto *Before = F.observe();
  F.B.CreateBr(Loop);
  EXPECT_FALSE(F.preservesBefore(*Before));
}

TEST(RegistrationImagePrefix, SharedExhaustionRemainsSticky) {
  PrefixFixture F;
  auto *Before = F.observe();
  F.B.CreateRetVoid();
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  const std::array<const llvm::Function *, 1> Functions{F.Function};
  ASSERT_TRUE(Stores.build(Functions, Work));
  AddressProof Addresses(*F.Function, nullptr, CookieVA, Stores, Work,
                         Exhausted);
  ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted);
  ASSERT_TRUE(Image.preserves(CookieRange));
  ASSERT_TRUE(Image.preservesBefore(CookieRange, *Before));
  Work = limits::kMaxRegistrationEHStateWork - 2;
  EXPECT_FALSE(Image.preservesBefore(CookieRange, *Before));
  EXPECT_TRUE(Exhausted);
  Work = 0;
  EXPECT_FALSE(Image.preservesBefore(CookieRange, *Before));
  EXPECT_FALSE(Image.preserves(CookieRange));
}
} // namespace
