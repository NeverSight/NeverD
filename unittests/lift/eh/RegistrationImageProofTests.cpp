//===- RegistrationImageProofTests.cpp - Current PE32 image effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish image aliases and call effects from private-frame preservation.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameProof.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/RegistrationFrameXor.h"
#include "neverd/backend/llvm/RegistrationImageProof.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ModRef.h"

#include <array>
#include <limits>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

constexpr va_t CookieVA = 0x403010;
constexpr ExceptionAddressRange CookieRange{CookieVA, CookieVA + 4};

struct ImageFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"image-effects", Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(Context, "entry", Function)};
  llvm::AllocaInst *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));

  ImageFixture() { Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32"); }
  llvm::GlobalVariable *global(llvm::StringRef Name) {
    return new llvm::GlobalVariable(Module, B.getInt32Ty(), false,
                                    llvm::GlobalValue::ExternalLinkage, nullptr,
                                    Name);
  }
  llvm::CallInst *call() {
    return B.CreateCall(Module.getOrInsertFunction(
        "opaque", llvm::FunctionType::get(B.getVoidTy(), false)));
  }
  bool preserves(ImageProof::WriteProjection Project = {},
                 llvm::ArrayRef<const llvm::Function *> Callbacks = {},
                 bool IndexCallbacks = true) {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    std::vector<const llvm::Function *> Functions{Function};
    if (IndexCallbacks)
      Functions.insert(Functions.end(), Callbacks.begin(), Callbacks.end());
    if (!Stores.build(Functions, Work))
      return false;
    if (!IndexCallbacks)
      Functions.insert(Functions.end(), Callbacks.begin(), Callbacks.end());
    AddressProof Addresses(*Function, nullptr, CookieVA, Stores, Work,
                           Exhausted);
    ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted, Project);
    return Image.preserves(CookieRange);
  }
};

struct CatchImageFixture : ImageFixture {
  llvm::InvokeInst *Throw;
  llvm::CatchPadInst *Pad;
  llvm::CatchReturnInst *Return;

  CatchImageFixture() {
    Function->setPersonalityFn(llvm::cast<llvm::Constant>(
        Module
            .getOrInsertFunction("__CxxFrameHandler3",
                                 llvm::FunctionType::get(B.getInt32Ty(), true))
            .getCallee()));
    B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                     &Module, llvm::Intrinsic::localescape),
                 {Frame});
    auto Block = [&](const char *Name) {
      return llvm::BasicBlock::Create(Context, Name, Function);
    };
    auto *Normal = Block("normal"), *Dispatch = Block("dispatch"),
         *Catch = Block("catch"), *Resume = Block("resume");
    Throw = B.CreateInvoke(
        Module.getOrInsertFunction(
            "throwing", llvm::FunctionType::get(B.getVoidTy(), false)),
        Normal, Dispatch);
    B.SetInsertPoint(Normal);
    B.CreateRetVoid();
    B.SetInsertPoint(Dispatch);
    auto *Switch =
        B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context), nullptr, 1);
    Switch->addHandler(Catch);
    B.SetInsertPoint(Catch);
    auto *Null = llvm::ConstantPointerNull::get(B.getPtrTy());
    Pad = B.CreateCatchPad(Switch, {Null, B.getInt32(64), Null});
    Return = B.CreateCatchRet(Pad, Resume);
    B.SetInsertPoint(Resume);
    B.CreateRetVoid();
  }
};

TEST(RegistrationImageProof, ActualImageArgumentsRequireCanonicalValues) {
  for (uint32_t Address : {0x403010u, 0x18003010u})
    for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Address << " " << Mutation);
      ImageFixture F;
      llvm::Module Other("other-image", F.Context);
      auto *Global = F.global(makeNdDataSymbol(Address));
      llvm::Value *Pointer = Global;
      if (Mutation == 1)
        Global->setInitializer(F.B.getInt32(0));
      if (Mutation == 2)
        Global->setLinkage(llvm::GlobalValue::ExternalWeakLinkage);
      if (Mutation == 3)
        Global->setThreadLocalMode(
            llvm::GlobalVariable::GeneralDynamicTLSModel);
      if (Mutation == 4)
        Global->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
      if (Mutation == 5)
        Global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
      if (Mutation == 6)
        Global->setName((kNdCodePtrPrefix + llvm::utohexstr(Address)).str());
      if (Mutation == 7)
        Global->setName("__nd_data_0403010");
      if (Mutation == 8)
        Global->setName(makeNdDataSymbol(uint64_t(UINT32_MAX) + 1));
      if (Mutation == 9)
        Pointer = new llvm::GlobalVariable(Other, F.B.getInt32Ty(), false,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, makeNdDataSymbol(Address));
      if (Mutation == 10)
        Pointer = F.B.CreateGEP(F.B.getInt8Ty(), Global, F.B.getInt32(1));
      if (Mutation == 11) {
        Global->setInitializer(F.B.getInt32(0));
        Pointer = llvm::GlobalAlias::create(F.B.getInt32Ty(), 0,
                                            llvm::GlobalValue::ExternalLinkage,
                                            "alias", Global, &F.Module);
      }
      if (Mutation == 12)
        F.Module.setDataLayout("e-m:x-p:64:64-i64:64-n8:16:32-S32");
      if (Mutation == 13)
        Pointer = new llvm::GlobalVariable(
            F.Module, F.B.getInt32Ty(), false,
            llvm::GlobalValue::ExternalLinkage, nullptr, "address_space",
            nullptr, llvm::GlobalVariable::NotThreadLocal, 1);
      llvm::Value *Value = F.B.CreatePtrToInt(Pointer, F.B.getInt32Ty());
      if (Mutation == 14)
        Value = F.B.getInt32(Address);
      if (Mutation == 15) {
        auto *Load = F.B.CreateLoad(F.B.getInt32Ty(), F.Frame);
        Load->setVolatile(true);
        Value = Load;
      }
      if (Mutation == 16)
        Value = F.B.CreateLoad(F.B.getInt32Ty(), Global);
      if (Mutation == 17)
        Value = llvm::PoisonValue::get(F.B.getInt32Ty());
      if (Mutation == 18)
        Value = F.B.Insert(new llvm::PtrToIntInst(Global, F.B.getInt32Ty()));
      if (Mutation == 19)
        Value = F.B.CreateAdd(Value, F.B.getInt32(1));
      auto *Callee =
          F.Module
              .getOrInsertFunction(
                  "retained", llvm::FunctionType::get(
                                  F.B.getInt32Ty(), {F.B.getInt32Ty()}, false))
              .getCallee();
      auto *Call = F.B.CreateCall(
          llvm::FunctionType::get(F.B.getInt32Ty(), {F.B.getInt32Ty()}, false),
          Callee, {Value});
      F.B.CreateRetVoid();
      size_t Work = 0;
      const auto Actual = originalImageArgumentAddress(*Call, 0, Work);
      ASSERT_EQ(bool(Actual), Mutation == 0 || Mutation == 18);
      if (Actual)
        EXPECT_EQ(*Actual, Address);
      // A foreign global is deliberately malformed input to the boundary;
      // remove it before independently checking the remaining authored IR.
      if (Mutation == 9)
        Call->setArgOperand(0, F.B.getInt32(0));
      EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    }
}

TEST(RegistrationImageProof, ImageArgumentIdentityUsesOneWorkBudget) {
  ImageFixture F;
  auto *Value = F.B.CreatePtrToInt(F.global(makeNdDataSymbol(CookieVA)),
                                   F.B.getInt32Ty());
  auto Callee = F.Module.getOrInsertFunction(
      "retained",
      llvm::FunctionType::get(F.B.getInt32Ty(), {F.B.getInt32Ty()}, false));
  auto *Call = F.B.CreateCall(Callee, {Value});
  F.B.CreateRetVoid();
  size_t Required = 0;
  ASSERT_EQ(originalImageArgumentAddress(*Call, 0, Required), CookieVA);
  for (size_t Available : {size_t{0}, Required - 1, Required}) {
    size_t Work = limits::kMaxRegistrationEHStateWork - Available;
    EXPECT_EQ(bool(originalImageArgumentAddress(*Call, 0, Work)),
              Available == Required);
    EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  }
  size_t Work = 0;
  EXPECT_FALSE(originalImageArgumentAddress(*Call, 1, Work));
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}

TEST(RegistrationImageProof, ResolvesOriginalAliasesAndPartialOverlaps) {
  for (const char *Name :
       {"__nd_data_403010", "__nd_codeptr_403010", "__security_cookie"})
    for (int Offset : {-4, -3, 0, 3, 4}) {
      SCOPED_TRACE(Name);
      SCOPED_TRACE(Offset);
      ImageFixture F;
      F.B.CreateStore(
          F.B.getInt32(0),
          F.B.CreateGEP(F.B.getInt8Ty(), F.global(Name), F.B.getInt32(Offset)));
      F.B.CreateRetVoid();
      EXPECT_EQ(F.preserves(), Offset == -4 || Offset == 4);
    }
  ImageFixture F;
  F.B.CreateStore(F.B.getInt32(0),
                  F.B.CreateGEP(F.B.getInt8Ty(),
                                F.global("__nd_codeptr_403000"),
                                F.B.getInt32(16)));
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.preserves());
}

TEST(RegistrationImageProof, ChecksPE32IntervalsWithoutSignedOverflow) {
  ImageFixture F;
  EXPECT_FALSE(imageRange({nullptr, 0}, 4));
  auto *Global = F.global("__nd_data_ffffffff");
  auto Last = imageRange({Global, 0}, 1);
  ASSERT_TRUE(Last);
  EXPECT_EQ(Last->End, uint64_t(UINT32_MAX) + 1);
  EXPECT_FALSE(imageRange({Global, 0}, 2));
  EXPECT_FALSE(imageRange({Global, 1}, 1));
  EXPECT_TRUE(imageRange({Global, -int64_t(UINT32_MAX)}, 1));
  EXPECT_FALSE(imageRange({Global, -int64_t(UINT32_MAX) - 1}, 1));
  EXPECT_FALSE(imageRange({Global, std::numeric_limits<int64_t>::max()}, 4));
  EXPECT_FALSE(imageRange({Global, std::numeric_limits<int64_t>::min()}, 4));
  EXPECT_FALSE(imageRange({Global, 0}, 0));
  EXPECT_FALSE(imageRange({Global, 0}, std::numeric_limits<uint64_t>::max()));
  EXPECT_FALSE(imageRange({F.global("__nd_data_100000000"), 0}, 4));
  EXPECT_FALSE(imageRange({F.global("__security_cookie"), 0}, 4));
}

TEST(RegistrationImageProof, RejectsUnauthenticatedStorageRoles) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    ImageFixture F;
    auto *Global = F.global("__nd_data_403010");
    switch (Mutation) {
    case 0:
      Global->setInitializer(F.B.getInt32(0));
      break;
    case 1:
      Global->setThreadLocal(true);
      break;
    case 2:
      Global->setDLLStorageClass(llvm::GlobalValue::DLLImportStorageClass);
      break;
    case 3:
      Global->setLinkage(llvm::GlobalValue::ExternalWeakLinkage);
      break;
    case 4:
      Global->setName("foreign_global");
      break;
    case 5:
      Global = new llvm::GlobalVariable(F.Module, F.B.getInt32Ty(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "__nd_data_403014", nullptr,
                                        llvm::GlobalValue::NotThreadLocal, 1);
      break;
    }
    EXPECT_FALSE(imageRange({Global, 0}, 4)) << Mutation;
  }
}

TEST(RegistrationImageProof, PrivateWritesRequireLiveBoundedStorage) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    ImageFixture F;
    if (Mutation == 1 || Mutation == 2)
      F.B.CreateLifetimeStart(F.Frame);
    if (Mutation == 2 || Mutation == 3)
      F.B.CreateLifetimeEnd(F.Frame);
    F.B.CreateStore(F.B.getInt32(1),
                    F.B.CreateGEP(F.B.getInt8Ty(), F.Frame,
                                  F.B.getInt32(Mutation == 4 ? 63 : 60)));
    F.B.CreateRetVoid();
    EXPECT_EQ(F.preserves(), Mutation <= 1) << Mutation;
  }
}

TEST(RegistrationImageProof, CallsNeedIndependentImageEffects) {
  ImageFixture F;
  auto *Call = F.call();
  Call->setMemoryEffects(llvm::MemoryEffects::none());
  Call->getCalledFunction()->setMemoryEffects(llvm::MemoryEffects::none());
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.preserves());
  for (const ExceptionAddressRange Written :
       {ExceptionAddressRange{CookieVA - 4, CookieVA},
        ExceptionAddressRange{CookieVA - 1, CookieVA + 1}}) {
    auto Project = [&](const llvm::Instruction &I,
                       const ExceptionAddressRange &Range) {
      return &I == Call && !Written.overlaps(Range);
    };
    EXPECT_EQ(F.preserves(Project), Written.End == CookieVA);
  }
}

TEST(RegistrationImageProof, ProjectionCannotOverrideKnownImageWrites) {
  ImageFixture F;
  F.B.CreateStore(F.B.getInt32(0), F.global("__nd_data_403010"));
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.preserves([](const auto &, const auto &) { return true; }));
}

TEST(RegistrationImageProof, IntrinsicAttributesCannotEraseWrites) {
  ImageFixture F;
  auto *Call = F.B.CreateMemSet(F.global("__nd_data_403010"), F.B.getInt8(0), 4,
                                llvm::Align(4));
  Call->setMemoryEffects(llvm::MemoryEffects::none());
  Call->getCalledFunction()->setMemoryEffects(llvm::MemoryEffects::none());
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.preserves());
}

TEST(RegistrationImageProof, ReadAndOrderingEffectsPreserveImage) {
  ImageFixture F;
  F.B.CreateLoad(F.B.getInt32Ty(), F.global("__nd_data_403010"))
      ->setVolatile(true);
  F.B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
      &F.Module, llvm::Intrinsic::sideeffect));
  F.B.CreateLifetimeEnd(llvm::PoisonValue::get(F.B.getPtrTy()));
  F.B.CreateRetVoid();
  EXPECT_TRUE(F.preserves());
}

TEST(RegistrationImageProof, IncludesUnreachableBlocksAndCallbacks) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    ImageFixture F;
    F.B.CreateRetVoid();
    auto *Callback = llvm::Function::Create(
        llvm::FunctionType::get(F.B.getVoidTy(), false),
        llvm::GlobalValue::ExternalLinkage, "callback", F.Module);
    F.B.SetInsertPoint(llvm::BasicBlock::Create(F.Context, "entry", Callback));
    F.B.CreateRetVoid();
    F.B.SetInsertPoint(llvm::BasicBlock::Create(
        F.Context, "dead", Mutation == 1 ? F.Function : Callback));
    if (Mutation == 1 || Mutation == 2)
      F.B.CreateStore(F.B.getInt32(0), F.global("__nd_data_403010"));
    F.B.CreateRetVoid();
    EXPECT_EQ(F.preserves({}, {Callback}, Mutation != 3), Mutation == 0);
  }
}

TEST(RegistrationImageProof, AmbientStackEffectsNeedAProjection) {
  ImageFixture F;
  auto *Restore = F.B.CreateStackRestore(F.Frame);
  Restore->setMemoryEffects(llvm::MemoryEffects::none());
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.preserves());
}

TEST(RegistrationImageProof, RejectsUnsupportedLayoutAndRanges) {
  for (const char *Layout : {"E-p:32:32", "e-p:64:64"}) {
    ImageFixture F;
    F.Module.setDataLayout(Layout);
    F.B.CreateRetVoid();
    EXPECT_FALSE(F.preserves());
  }
  ImageFixture F;
  F.B.CreateRetVoid();
  size_t Work = 0;
  bool Exhausted = false;
  coff_registration::RegistrationFrameStores Stores;
  const std::array<const llvm::Function *, 1> Functions{F.Function};
  ASSERT_TRUE(Stores.build(Functions, Work));
  AddressProof Addresses(*F.Function, nullptr, CookieVA, Stores, Work,
                         Exhausted);
  ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted);
  EXPECT_FALSE(Image.preserves({CookieVA, CookieVA}));
  EXPECT_FALSE(Image.preserves({CookieVA + 4, CookieVA}));
  EXPECT_FALSE(Image.preserves({UINT32_MAX, uint64_t(UINT32_MAX) + 2}));
}

TEST(RegistrationImageProof, ExhaustionRemainsStickyAcrossCachedAnswers) {
  ImageFixture F;
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
  Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(Image.preserves(CookieRange));
  EXPECT_TRUE(Exhausted);
  Work = 0;
  EXPECT_FALSE(Image.preserves(CookieRange));
}

TEST(RegistrationImageProof, CookieRecipesStillRequireImageStability) {
  for (bool Clobber : {false, true}) {
    ImageFixture F;
    auto *Global = F.global("__nd_data_403010");
    auto *Old = F.B.CreateLoad(F.B.getInt32Ty(), Global);
    if (Clobber)
      F.B.CreateStore(F.B.getInt32(0), F.global("__security_cookie"));
    auto *Current = F.B.CreateLoad(F.B.getInt32Ty(), Global);
    auto *Difference = F.B.CreateXor(Old, Current);
    F.B.CreateRetVoid();
    size_t Work = 0;
    bool Exhausted = false;
    coff_registration::RegistrationFrameStores Stores;
    const std::array<const llvm::Function *, 1> Functions{F.Function};
    ASSERT_TRUE(Stores.build(Functions, Work));
    AddressProof Addresses(*F.Function, nullptr, CookieVA, Stores, Work,
                           Exhausted);
    XorProof Values(Addresses, CookieVA, Work, Exhausted);
    EXPECT_EQ(Values.evaluate(Difference),
              (std::optional<XorValue>{{0, false, {}}}));
    ImageProof Image(Addresses, Functions, CookieVA, Work, Exhausted);
    EXPECT_EQ(Image.preserves(CookieRange), !Clobber);
  }
}

TEST(RegistrationImageProof, COFFChecksEveryCallAgainstImmutableImage) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    ImageFixture F;
    F.B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                       &F.Module, llvm::Intrinsic::localescape),
                   {F.Frame});
    auto Member = F.Module.getOrInsertFunction(
        "member",
        llvm::FunctionType::get(F.B.getVoidTy(), {F.B.getPtrTy()}, false));
    auto *Call = F.B.CreateCall(Member, {F.Frame});
    Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
    Call->setMemoryEffects(llvm::MemoryEffects::none());
    F.B.CreateRetVoid();
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    BinaryImage Image;
    coff_registration::RegistrationCxxFrameContract Contract;
    Contract.Image = &Image;
    Contract.Borrows.emplace(Call,
                             coff_registration::RegistrationFrameBorrow{});
    if (Mutation != 1)
      Contract.ImageWrites[Call] = {};
    if (Mutation == 2)
      Contract.ImageWrites[Call] = {{CookieVA - 4, CookieVA}};
    if (Mutation == 3)
      Contract.ImageWrites[Call] = {{CookieVA - 1, CookieVA + 1}};
    if (Mutation == 4)
      Contract.ImageWrites[Call] = {{CookieVA + 1, CookieVA}};
    auto Error = coff_registration::validateFramePrivacy(
        *F.Function, {}, F.Frame, {}, {}, {}, {}, CookieVA, {CookieRange},
        &Contract);
    const bool Failed = bool(Error);
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_EQ(Failed, Mutation == 1 || Mutation >= 3) << Message;
    if (Failed)
      EXPECT_NE(Message.find("current writes do not preserve"),
                std::string::npos)
          << Message;
  }
}

TEST(RegistrationImageProof, CatchReturnNeedsAnIndependentProjection) {
  CatchImageFixture F;
  EXPECT_FALSE(
      F.preserves([&](const auto &I, const auto &) { return &I != F.Return; }));
  EXPECT_TRUE(F.preserves([](const auto &, const auto &) { return true; }));
}

TEST(RegistrationImageProof, COFFBindsCatchReturnWithoutErasingRuntimeWrites) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CatchImageFixture F;
    ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
    BinaryImage Image;
    coff_registration::RegistrationCxxFrameContract Contract;
    Contract.Image = &Image;
    auto &Catch = Contract.Catches.emplace_back();
    Catch.Catch = F.Pad;
    Catch.CallbackBlocks.insert(F.Pad->getParent());
    if (Mutation != 1)
      Contract.CatchReturns.insert(F.Return);
    if (Mutation != 2)
      Contract.ImageWrites[F.Throw] = {};
    // The source throw summary includes destruction after catch return.
    if (Mutation == 3)
      Contract.ImageWrites[F.Throw] = {{CookieVA - 1, CookieVA + 1}};
    if (Mutation == 4)
      Contract.ImageWrites[F.Throw] = {{CookieVA - 4, CookieVA}};
    auto Error = coff_registration::validateFramePrivacy(
        *F.Function, {}, F.Frame, {}, {}, {}, {}, CookieVA, {CookieRange},
        &Contract);
    const bool Failed = bool(Error);
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_EQ(Failed, Mutation >= 1 && Mutation <= 3) << Message;
    if (Failed)
      EXPECT_NE(Message.find("current writes do not preserve"),
                std::string::npos)
          << Message;
  }
}
} // namespace
