//===- RegistrationCookieFrameTests.cpp - Closed PE32 cookie frames ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep conditional checker sinks inside the complete current frame closure.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/RegistrationFrameProof.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/ModRef.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_frame;
using namespace neverd::coff_registration;

constexpr va_t CookieVA = 0x4000;
constexpr va_t CheckVA = 0x2000;

struct CookieFrameFixture {
  BinaryImage Image;
  llvm::LLVMContext Context;
  llvm::Module Module{"cookie-frame", Context};
  llvm::Function *Parent = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> B{llvm::BasicBlock::Create(Context, "entry", Parent)};
  llvm::AllocaInst *Frame = B.CreateAlloca(B.getInt8Ty(), B.getInt32(64));
  llvm::Value *Slot = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(4));
  llvm::Value *Base = B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(8));
  llvm::GlobalVariable *Cookie = new llvm::GlobalVariable(
      Module, B.getInt32Ty(), false, llvm::GlobalValue::ExternalLinkage,
      nullptr, "__nd_data_4000");
  llvm::Function *Checker =
      createX86RegistrationSecurityCheck(Module, "checker");
  RegistrationCxxFrameContract Contract;
  std::vector<const llvm::Function *> Callbacks;

  CookieFrameFixture() {
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    rewrite_source::setOriginalVA(*Checker, CheckVA);
    Contract.Image = &Image;
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
    Data.Size = Data.FileSz = 8;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(Data.Size);
    writeLE<uint32_t>(Data.Data.data(), 0x12345678);
    Image.Segments.push_back(std::move(Data));
    Image.BaseRelocations.push_back(
        {CheckVA + 2, llvm::COFF::IMAGE_REL_BASED_HIGHLOW});
  }
  llvm::Value *initialize() {
    auto *Load = B.CreateLoad(B.getInt32Ty(), Cookie);
    auto MetadataInteger = [&](uint64_t Value, unsigned Width) {
      return llvm::ConstantAsMetadata::get(
          llvm::ConstantInt::get(B.getIntNTy(Width), Value));
    };
    Load->setMetadata(
        windows_eh_md::RegistrationOperationAttachment,
        llvm::MDNode::get(Context,
                          {MetadataInteger(0x1000, 64),
                           MetadataInteger(0x1005, 64), MetadataInteger(0, 32),
                           MetadataInteger(0, 32), MetadataInteger(0, 8)}));
    auto *Encoded = B.CreateXor(Load, B.CreatePtrToInt(Base, B.getInt32Ty()));
    B.CreateStore(Encoded, Slot);
    return Encoded;
  }
  llvm::CallInst *check(uint32_t Delta = 0) {
    auto *Value = B.CreateXor(B.CreateLoad(B.getInt32Ty(), Slot),
                              B.CreatePtrToInt(Base, B.getInt32Ty()));
    if (Delta)
      Value = B.CreateXor(Value, B.getInt32(Delta));
    auto *Call = B.CreateCall(Checker, {B.CreateIntToPtr(Value, B.getPtrTy())});
    Call->setCallingConv(llvm::CallingConv::X86_FastCall);
    Call->addParamAttr(0, llvm::Attribute::InReg);
    Contract.CookieCalls.push_back({Call, CheckVA});
    return Call;
  }
  llvm::CallInst *borrow(int32_t Offset = 16) {
    auto *Callee = llvm::cast<llvm::Function>(
        Module
            .getOrInsertFunction(
                "borrow",
                llvm::FunctionType::get(B.getVoidTy(), {B.getPtrTy()}, false))
            .getCallee());
    Callee->setCallingConv(llvm::CallingConv::X86_ThisCall);
    auto *Call = B.CreateCall(
        Callee, {B.CreateGEP(B.getInt8Ty(), Frame, B.getInt32(Offset))});
    Call->setCallingConv(Callee->getCallingConv());
    Contract.Borrows.emplace(Call, RegistrationFrameBorrow{Offset});
    Contract.ImageWrites.emplace(Call, std::vector<ExceptionAddressRange>{});
    return Call;
  }
  llvm::CallInst *opaque(llvm::Value *Argument = nullptr) {
    auto *Type = Argument ? llvm::FunctionType::get(
                                B.getVoidTy(), {Argument->getType()}, false)
                          : llvm::FunctionType::get(B.getVoidTy(), false);
    llvm::SmallVector<llvm::Value *, 1> Arguments;
    if (Argument)
      Arguments.push_back(Argument);
    return B.CreateCall(Module.getOrInsertFunction("opaque", Type), Arguments);
  }
  void dispatch(const llvm::Instruction *Call) {
    Contract.CookieDispatches[Call] = {{{Frame, 4}, {Frame, 8}, CheckVA}};
  }
  std::string error() {
    EXPECT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    const std::array<ExceptionAddressRange, 1> Immutable{
        {{CheckVA, CheckVA + 0x100}}};
    return llvm::toString(validateFramePrivacy(*Parent, Callbacks, Frame, {},
                                               {}, {}, {}, CookieVA, Immutable,
                                               &Contract));
  }
};

TEST(RegistrationCookieFrame, CompleteFrameAcceptsOnlyTheCheckedScalarSink) {
  CookieFrameFixture F;
  F.initialize();
  auto *Event = F.borrow();
  F.dispatch(Event);
  F.check();
  F.check();
  F.B.CreateRetVoid();
  EXPECT_EQ(F.error(), "");
}

TEST(RegistrationCookieFrame, EncodedAndDecodedValuesRemainTaintedElsewhere) {
  for (bool Encoded : {false, true})
    for (bool GlobalStore : {false, true}) {
      SCOPED_TRACE(Encoded);
      SCOPED_TRACE(GlobalStore);
      CookieFrameFixture F;
      auto *Encoding = F.initialize();
      F.borrow();
      auto *Check = F.check();
      llvm::Value *Value = Encoded ? Encoding : Check->getArgOperand(0);
      if (GlobalStore) {
        if (Value->getType()->isPointerTy())
          Value = F.B.CreatePtrToInt(Value, F.B.getInt32Ty());
        F.B.CreateStore(
            Value, F.B.CreateGEP(F.B.getInt8Ty(), F.Cookie, F.B.getInt32(4)));
      } else {
        auto *Call = F.opaque(Value);
        Call->setMemoryEffects(llvm::MemoryEffects::none());
        F.Contract.ImageWrites.emplace(Call,
                                       std::vector<ExceptionAddressRange>{});
      }
      F.B.CreateRetVoid();
      EXPECT_NE(F.error(), "");
    }
}

TEST(RegistrationCookieFrame, EarlierFrameEscapeCannotBorrowReadonlyPromises) {
  CookieFrameFixture F;
  F.initialize();
  F.borrow();
  auto *Escape = F.opaque(F.Frame);
  Escape->setMemoryEffects(llvm::MemoryEffects::none());
  F.Contract.ImageWrites.emplace(Escape, std::vector<ExceptionAddressRange>{});
  F.check();
  F.B.CreateRetVoid();
  EXPECT_NE(F.error(), "");
}

TEST(RegistrationCookieFrame, CurrentCookieSlotAndCheckerMutationsReject) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieFrameFixture F;
    F.initialize();
    F.borrow();
    if (Mutation == 0)
      F.B.CreateStore(F.B.getInt32(0), F.Cookie);
    if (Mutation == 1)
      F.B.CreateStore(F.B.getInt32(0), F.Slot);
    if (Mutation == 2)
      F.Image.Segments.front().Data[0] = 0x90;
    auto *Call = F.check(Mutation == 3 ? 1 : 0);
    if (Mutation == 4)
      Call->addFnAttr(llvm::Attribute::NoUnwind);
    F.B.CreateRetVoid();
    const auto Error = F.error();
    EXPECT_NE(Error.find(Mutation == 4 ? "unique current conditional ABI"
                                       : "current cookie protocol"),
              std::string::npos)
        << Error;
  }
}

TEST(RegistrationCookieFrame,
     UnknownOrdinaryCallNeedsItsIndependentImageProof) {
  for (bool Before : {false, true}) {
    CookieFrameFixture F;
    F.initialize();
    F.borrow();
    if (Before)
      F.opaque()->setMemoryEffects(llvm::MemoryEffects::none());
    F.check();
    if (!Before)
      F.opaque()->setMemoryEffects(llvm::MemoryEffects::none());
    F.B.CreateRetVoid();
    const auto Error = F.error();
    EXPECT_NE(Error.find("current cookie protocol"), std::string::npos)
        << Error;
  }
}

TEST(RegistrationCookieFrame, DispatchIncludesCalleeFrameAndImageEffects) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    CookieFrameFixture F;
    F.initialize();
    auto *Call = F.borrow(0);
    F.dispatch(Call);
    if (Mutation == 0)
      F.Contract.Borrows.at(Call).Writes = {{4, 8}};
    if (Mutation == 1) {
      F.Contract.Borrows.at(Call).Initializer = true;
      F.Contract.Borrows.at(Call).Initialized = {{4, 8}};
    }
    if (Mutation == 2)
      F.Contract.ImageWrites.at(Call) = {{CookieVA, CookieVA + 4}};
    if (Mutation == 3)
      F.Contract.ImageWrites.at(Call) = {{CheckVA, CheckVA + 1}};
    F.B.CreateRetVoid();
    const auto Error = F.error();
    EXPECT_NE(Error.find("current cookie protocol"), std::string::npos)
        << Error;
  }
}

TEST(RegistrationCookieFrame, CallbackWritesParticipateInWholeImageClosure) {
  for (bool Overlap : {false, true}) {
    CookieFrameFixture F;
    F.initialize();
    F.borrow();
    F.check();
    F.B.CreateRetVoid();
    auto *Callback = llvm::Function::Create(
        llvm::FunctionType::get(F.B.getVoidTy(), false),
        llvm::GlobalValue::InternalLinkage, "callback", F.Module);
    llvm::IRBuilder<> B{llvm::BasicBlock::Create(F.Context, "entry", Callback)};
    B.CreateStore(B.getInt32(0), B.CreateGEP(B.getInt8Ty(), F.Cookie,
                                             B.getInt32(Overlap ? 0 : 4)));
    B.CreateRetVoid();
    F.Callbacks.push_back(Callback);
    EXPECT_EQ(F.error().empty(), !Overlap);
  }
}

TEST(RegistrationCookieFrame, AllocationLifetimeMustCoverEveryCookieRead) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    CookieFrameFixture F;
    if (Mutation != 1)
      F.B.CreateLifetimeStart(F.Frame);
    F.initialize();
    F.borrow();
    if (Mutation != 1)
      F.B.CreateLifetimeEnd(F.Frame);
    if (Mutation != 0)
      F.B.CreateLifetimeStart(F.Frame);
    F.check();
    F.B.CreateRetVoid();
    EXPECT_NE(F.error(), "");
  }
}

TEST(RegistrationCookieFrame, ConditionalCallHasExactlyOneAuthority) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    CookieFrameFixture F;
    F.initialize();
    F.borrow();
    auto *Call = F.check();
    if (Mutation == 0)
      F.Contract.CookieCalls.push_back(F.Contract.CookieCalls.front());
    if (Mutation == 1)
      F.Contract.Borrows.emplace(Call, RegistrationFrameBorrow{});
    if (Mutation == 2)
      F.Contract.ImageWrites.emplace(Call,
                                     std::vector<ExceptionAddressRange>{});
    if (Mutation == 3) {
      auto *Intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
          &F.Module, llvm::Intrinsic::stackrestore, {F.B.getPtrTy()});
      Intrinsic->setAttributes(F.Checker->getAttributes());
      Intrinsic->setCallingConv(F.Checker->getCallingConv());
      rewrite_source::setOriginalVA(*Intrinsic, CheckVA);
      Call->setCalledFunction(Intrinsic);
    }
    F.B.CreateRetVoid();
    const auto Error = F.error();
    EXPECT_NE(Error.find("unique current conditional ABI"), std::string::npos)
        << Error;
  }
}

TEST(RegistrationCookieFrame, CurrentAddressCacheCannotBootstrapLoopChecks) {
  CookieFrameFixture F;
  F.initialize();
  F.borrow();
  auto *Loop = llvm::BasicBlock::Create(F.Context, "loop", F.Parent);
  auto *Exit = llvm::BasicBlock::Create(F.Context, "exit", F.Parent);
  F.B.CreateBr(Loop);
  F.B.SetInsertPoint(Loop);
  F.check();
  F.B.CreateCondBr(F.Parent->getArg(0), Loop, Exit);
  F.B.SetInsertPoint(Exit);
  F.B.CreateRetVoid();
  const auto Error = F.error();
  EXPECT_NE(Error.find("current cookie protocol"), std::string::npos) << Error;
}

} // namespace
