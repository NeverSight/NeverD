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
} // namespace
