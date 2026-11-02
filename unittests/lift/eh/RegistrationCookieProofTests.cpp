//===- RegistrationCookieProofTests.cpp - Ordered PE32 GS effects --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Refuse circular success assumptions and altered current guard values.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/RegistrationCookieProof.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ModRef.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;

constexpr va_t CookieVA = 0x4000;
constexpr va_t CheckVA = 0x2000;

struct CookieProofFixture {
  BinaryImage Image;
  llvm::LLVMContext LLVMContext;
  llvm::Module Module{"cookie-protocol", LLVMContext};
  llvm::Function *Parent = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(LLVMContext),
                              {llvm::Type::getInt1Ty(LLVMContext)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(LLVMContext, "entry", Parent)};
  llvm::AllocaInst *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));
  llvm::Value *Slot = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(4));
  llvm::Value *Base = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(8));
  llvm::GlobalVariable *Cookie = new llvm::GlobalVariable(
      Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
      nullptr, "__nd_data_4000");
  llvm::Function *Checker =
      createX86RegistrationSecurityCheck(Module, "checker");
  std::vector<CookieCall> Calls;
  std::array<CookieFrameGuard, 1> Guards{{{{Frame, 4}, {Frame, 8}, CheckVA}}};
  std::vector<CookieDispatch> Dispatches;

  CookieProofFixture() {
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    rewrite_source::setOriginalVA(*Checker, CheckVA);
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.DynInfo.SecurityCookieRVA = CookieVA;
    Segment Text;
    Text.VA = CheckVA;
    Text.Size = Text.FileSz = 0x100;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data.resize(Text.Size, 0xcc);
    const uint8_t Bytes[] = {0x3b, 0x0d, 0, 0x40, 0, 0, 0x75, 0x10, 0xc3};
    std::copy(std::begin(Bytes), std::end(Bytes), Text.Data.begin());
    Image.Segments.push_back(std::move(Text));
    Section Code;
    Code.VA = CheckVA;
    Code.Size = Code.FileSz = 0x100;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Image.Sections.push_back(Code);
    Segment Data;
    Data.VA = CookieVA;
    Data.Size = Data.FileSz = 4;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(4);
    writeLE<uint32_t>(Data.Data.data(), 0x12345678);
    Image.Segments.push_back(std::move(Data));
    Image.BaseRelocations.push_back(
        {CheckVA + 2, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
  }
  void initialize() {
    auto *Encoded = B.CreateXor(B.CreateLoad(B.getInt32Ty(), Cookie),
                                B.CreatePtrToInt(Base, B.getInt32Ty()));
    B.CreateStore(Encoded, Slot);
  }
  llvm::CallInst *check(uint32_t Delta = 0) {
    auto *Decoded = B.CreateXor(B.CreateLoad(B.getInt32Ty(), Slot),
                                B.CreatePtrToInt(Base, B.getInt32Ty()));
    if (Delta)
      Decoded = B.CreateXor(Decoded, B.getInt32(Delta));
    auto *Call =
        B.CreateCall(Checker, {B.CreateIntToPtr(Decoded, B.getPtrTy())});
    Call->setCallingConv(llvm::CallingConv::X86_FastCall);
    Call->addParamAttr(0, llvm::Attribute::InReg);
    Calls.push_back({Call, CheckVA});
    return Call;
  }
  llvm::CallInst *effect() {
    return B.CreateCall(Module.getOrInsertFunction(
        "effect", llvm::FunctionType::get(B.getVoidTy(), false)));
  }
  bool prove(AddressProof::MemoryProjection Memory = {},
             ImageProof::WriteProjection Writes = {}, size_t InitialWork = 0) {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    size_t Work = InitialWork;
    bool Exhausted = false;
    const std::array<const llvm::Function *, 1> Functions{Parent};
    coff_registration::RegistrationFrameStores Stores;
    if (!Stores.build(Functions, Work))
      return false;
    CookieProofContext Context{*Parent, nullptr, Stores, Functions, CookieVA};
    Context.PreservesMemory = Memory;
    Context.PreservesImage = Writes;
    return proveCookieProtocol(Image, Context, Calls, Dispatches, Work,
                               Exhausted);
  }
};

TEST(RegistrationCookieProof, CertifiesOrderedCallsFromCurrentValues) {
  CookieProofFixture F;
  F.initialize();
  F.check();
  F.check();
  F.B.CreateRetVoid();
  std::reverse(F.Calls.begin(), F.Calls.end());
  EXPECT_TRUE(F.prove());
}

TEST(RegistrationCookieProof, RejectsCurrentScalarAndFrameCorruption) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieProofFixture F;
    F.initialize();
    if (Mutation == 0)
      F.B.CreateStore(F.B.getInt32(0), F.Slot);
    if (Mutation == 1)
      F.B.CreateStore(F.B.getInt8(0), F.Slot);
    if (Mutation == 2)
      F.B.CreateLifetimeEnd(F.Frame);
    F.check(Mutation == 3 ? 1 : 0);
    F.B.CreateRetVoid();
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationCookieProof, RejectsChangedCookieBeforeOrAfterCheck) {
  for (bool Before : {false, true}) {
    CookieProofFixture F;
    F.initialize();
    if (Before)
      F.B.CreateStore(F.B.getInt32(1), F.Cookie);
    F.check();
    if (!Before)
      F.B.CreateStore(F.B.getInt32(1), F.Cookie);
    F.B.CreateRetVoid();
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationCookieProof, RejectsChangedCheckerCode) {
  CookieProofFixture F;
  F.initialize();
  F.check();
  auto *Code = new llvm::GlobalVariable(F.Module, F.B.getInt8Ty(), false,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "__nd_data_2000");
  F.B.CreateStore(F.B.getInt8(0x90), Code);
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationCookieProof, CheckerCodeNeedsItsOwnPrefixEffectProof) {
  CookieProofFixture F;
  F.initialize();
  auto *Event = F.effect();
  F.check();
  F.B.CreateRetVoid();
  unsigned MemoryQueries = 0;
  auto Memory = [&](const llvm::Instruction &I, const Cell &, uint64_t) {
    ++MemoryQueries;
    return &I == Event;
  };
  auto Writes = [&](const llvm::Instruction &I,
                    const ExceptionAddressRange &Range) {
    return &I == Event && Range.Begin == CookieVA;
  };
  EXPECT_FALSE(F.prove(Memory, Writes));
  EXPECT_EQ(MemoryQueries, 0u);
}

TEST(RegistrationCookieProof, RejectsDetachedCallOwnership) {
  CookieProofFixture F;
  F.initialize();
  auto *Call = F.check();
  auto *Return = F.B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  size_t Work = 0;
  bool Exhausted = false;
  const std::array<const llvm::Function *, 1> Functions{F.Parent};
  coff_registration::RegistrationFrameStores Stores;
  ASSERT_TRUE(Stores.build(Functions, Work));
  CookieProofContext Context{*F.Parent, nullptr, Stores, Functions, CookieVA};
  Call->removeFromParent();
  EXPECT_FALSE(hasExactCookieCallABI(*Call, CheckVA));
  EXPECT_FALSE(proveCookieProtocol(F.Image, Context, F.Calls, F.Dispatches,
                                   Work, Exhausted));
  Call->insertBefore(Return->getIterator());
  EXPECT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
}

TEST(RegistrationCookieProof, SourceIdentityCannotOverrideIntrinsicSemantics) {
  CookieProofFixture F;
  F.initialize();
  auto *Call = F.check();
  auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
      &F.Module, llvm::Intrinsic::stackrestore, {F.B.getPtrTy()});
  ASSERT_EQ(Intrinsic->getFunctionType(), F.Checker->getFunctionType());
  Intrinsic->setAttributes(F.Checker->getAttributes());
  Intrinsic->setCallingConv(F.Checker->getCallingConv());
  rewrite_source::setOriginalVA(*Intrinsic, CheckVA);
  Call->setCalledFunction(Intrinsic);
  F.B.CreateRetVoid();
  ASSERT_FALSE(llvm::verifyModule(F.Module, &llvm::errs()));
  EXPECT_FALSE(hasExactCookieCallABI(*Call, CheckVA));
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationCookieProof, RejectsChangedCurrentABIAndSourceIdentity) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieProofFixture F;
    F.initialize();
    auto *Call = F.check();
    switch (Mutation) {
    case 0:
      Call->removeParamAttr(0, llvm::Attribute::InReg);
      break;
    case 1:
      Call->setCallingConv(llvm::CallingConv::C);
      break;
    case 2:
      Call->addFnAttr(llvm::Attribute::NoUnwind);
      break;
    case 3:
      F.Checker->setMemoryEffects(llvm::MemoryEffects::none());
      break;
    case 4:
      rewrite_source::setOriginalVA(*F.Checker, CheckVA + 1);
      break;
    case 5:
      F.Image.Segments.front().Data[0] = 0x90;
      break;
    }
    F.B.CreateRetVoid();
    EXPECT_FALSE(F.prove());
  }
}

TEST(RegistrationCookieProof, DoesNotBootstrapCheckAcrossItsOwnBackedge) {
  CookieProofFixture F;
  F.initialize();
  auto *Loop = llvm::BasicBlock::Create(F.LLVMContext, "loop", F.Parent);
  auto *Exit = llvm::BasicBlock::Create(F.LLVMContext, "exit", F.Parent);
  F.B.CreateBr(Loop);
  F.B.SetInsertPoint(Loop);
  F.check();
  F.B.CreateCondBr(F.Parent->getArg(0), Loop, Exit);
  F.B.SetInsertPoint(Exit);
  F.B.CreateRetVoid();
  EXPECT_FALSE(F.prove());
}

TEST(RegistrationCookieProof, UnrelatedFutureCheckDoesNotTaintFirstPrefix) {
  CookieProofFixture F;
  F.initialize();
  auto *Normal = llvm::BasicBlock::Create(F.LLVMContext, "normal", F.Parent);
  auto *Throw = llvm::BasicBlock::Create(F.LLVMContext, "throw", F.Parent);
  F.B.CreateCondBr(F.Parent->getArg(0), Normal, Throw);
  F.B.SetInsertPoint(Normal);
  F.check();
  F.B.CreateRetVoid();
  F.B.SetInsertPoint(Throw);
  auto *Event = F.effect();
  F.Dispatches.push_back({Event, F.Guards});
  F.B.CreateUnreachable();
  auto Memory = [&](const llvm::Instruction &I, const Cell &, uint64_t) {
    return &I == Event;
  };
  auto Writes = [&](const llvm::Instruction &I, const ExceptionAddressRange &) {
    return &I == Event;
  };
  EXPECT_TRUE(F.prove(Memory, Writes));
}

TEST(RegistrationCookieProof, ChecksEffectsAtExceptionalDispatch) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieProofFixture F;
    F.initialize();
    if (Mutation == 3)
      F.B.CreateStore(F.B.getInt32(0), F.Slot);
    auto *Event = F.effect();
    F.Dispatches.push_back({Event, F.Guards});
    F.B.CreateRetVoid();
    auto Memory = [&](const llvm::Instruction &I, const Cell &, uint64_t) {
      return &I == Event && Mutation != 1;
    };
    auto Writes = [&](const llvm::Instruction &I,
                      const ExceptionAddressRange &) {
      return &I == Event && Mutation != 2;
    };
    EXPECT_EQ(F.prove(Memory, Writes), Mutation == 0);
  }
}

TEST(RegistrationCookieProof, UnknownCallsNeedIndependentImageAndFrameEffects) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    CookieProofFixture F;
    F.initialize();
    auto *Event = F.effect();
    Event->setMemoryEffects(llvm::MemoryEffects::none());
    F.check();
    F.B.CreateRetVoid();
    auto Memory = [&](const llvm::Instruction &I, const Cell &, uint64_t) {
      return &I == Event && (Mutation & 1);
    };
    auto Writes = [&](const llvm::Instruction &I,
                      const ExceptionAddressRange &) {
      return &I == Event && (Mutation & 2);
    };
    EXPECT_EQ(F.prove(Memory, Writes), Mutation == 3);
  }
}

TEST(RegistrationCookieProof, RejectsDuplicateOccurrencesAndExhaustion) {
  CookieProofFixture F;
  F.initialize();
  F.check();
  F.B.CreateRetVoid();
  F.Calls.push_back(F.Calls.front());
  EXPECT_FALSE(F.prove());
  F.Calls.pop_back();
  EXPECT_FALSE(F.prove({}, {}, limits::kMaxRegistrationEHStateWork));
}

} // namespace
