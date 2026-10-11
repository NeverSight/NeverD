//===- RegistrationFrameBitsTests.cpp - PE32 scalar register slices ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/codegen/COFF/COFFRegistrationFrameProof.h"
#include "gtest/gtest.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

namespace {
using namespace neverd::coff_registration;

struct SliceFixture {
  llvm::LLVMContext Context;
  llvm::Module Module{"frame-bits", Context};
  llvm::Function *Function = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(Context),
                              {llvm::Type::getInt1Ty(Context)}, false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  llvm::IRBuilder<> Builder{
      llvm::BasicBlock::Create(Context, "entry", Function)};
  llvm::AllocaInst *Frame = Builder.CreateAlloca(Builder.getInt32Ty());
  llvm::Value *Address = Builder.CreatePtrToInt(Frame, Builder.getInt32Ty());

  SliceFixture() { Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32"); }

  llvm::Value *spill(llvm::Value *Value) {
    auto *Slot = Builder.CreateAlloca(Value->getType());
    Builder.CreateStore(llvm::Constant::getNullValue(Value->getType()), Slot);
    Builder.CreateStore(Value, Slot);
    return Builder.CreateLoad(Value->getType(), Slot);
  }

  void check(llvm::Value *Result, bool Valid) {
    Builder.CreateRet(Builder.CreateZExtOrTrunc(Result, Builder.getInt32Ty()));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    auto Error = validateFramePrivacy(*Function, {}, Frame, {}, {});
    EXPECT_EQ(!bool(Error), Valid) << llvm::toString(std::move(Error));
    llvm::consumeError(std::move(Error));
  }
};

TEST(RegistrationFrameBits, ScalarLowByteDiscardsPreservedPointerHighBits) {
  for (unsigned ScalarBits : {8, 16})
    for (bool RetainPointerBit : {false, true}) {
      SCOPED_TRACE(ScalarBits);
      SCOPED_TRACE(RetainPointerBit);
      SliceFixture F;
      auto &B = F.Builder;
      auto *Narrow = B.getIntNTy(ScalarBits);
      const uint32_t Mask = UINT32_MAX << (ScalarBits - RetainPointerBit);
      auto *High = F.spill(B.CreateAnd(F.Address, B.getInt32(Mask)));
      auto *Low = F.spill(B.CreateZExt(F.Function->getArg(0), B.getInt32Ty()));
      auto *Combined = F.spill(B.CreateOr(High, Low));
      F.check(B.CreateTrunc(Combined, Narrow), !RetainPointerBit);
    }
}

TEST(RegistrationFrameBits, ScalarProofIncludesEverySpillWriteAndAlias) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    SliceFixture F;
    auto &B = F.Builder;
    auto *Masked = B.CreateAnd(F.Address, B.getInt32(0xffffff00));
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    B.CreateStore(Masked, Slot);
    switch (Mutation) {
    case 0:
      break;
    case 1:
      B.CreateStore(F.Address, Slot);
      break;
    case 2:
      B.CreateStore(B.CreateTrunc(F.Address, B.getInt8Ty()), Slot);
      break;
    case 3: {
      auto *Store = B.CreateStore(Masked, Slot);
      Store->setAtomic(llvm::AtomicOrdering::Release);
      break;
    }
    case 4: {
      auto *Alias = B.CreateGEP(B.getInt8Ty(), Slot, B.getInt32(1));
      B.CreateLoad(B.getInt8Ty(), Alias);
      break;
    }
    case 5: {
      auto *Left = llvm::BasicBlock::Create(F.Context, "left", F.Function);
      auto *Right = llvm::BasicBlock::Create(F.Context, "right", F.Function);
      auto *Join = llvm::BasicBlock::Create(F.Context, "join", F.Function);
      B.CreateCondBr(F.Function->getArg(0), Left, Right);
      B.SetInsertPoint(Left);
      B.CreateStore(F.Address, Slot);
      B.CreateBr(Join);
      B.SetInsertPoint(Right);
      B.CreateBr(Join);
      B.SetInsertPoint(Join);
      break;
    }
    }
    auto *Bits = B.CreateLoad(B.getInt32Ty(), Slot);
    F.check(B.CreateTrunc(Bits, B.getInt8Ty()), Mutation == 0);
  }
}

TEST(RegistrationFrameBits, TracksCastsShiftsAndRetainedSignBits) {
  for (unsigned Form = 0; Form != 5; ++Form) {
    SliceFixture F;
    auto &B = F.Builder;
    llvm::Value *Value = nullptr;
    switch (Form) {
    case 0:
      Value = B.CreateShl(F.Address, B.getInt32(8));
      break;
    case 1:
      Value = B.CreateLShr(B.CreateAnd(F.Address, B.getInt32(0xffffff00)),
                           B.getInt32(8));
      break;
    case 2:
      Value = B.CreateZExt(B.CreateAnd(F.Address, B.getInt32(0xffffff00)),
                           B.getInt64Ty());
      break;
    case 3:
      Value = B.CreateSExt(B.CreateTrunc(F.Address, B.getInt16Ty()),
                           B.getInt64Ty());
      Value = B.CreateLShr(Value, B.getInt64(32));
      break;
    case 4:
      Value = B.CreateXor(B.CreateAnd(F.Address, B.getInt32(0xffffff00)),
                          B.getInt32(7));
      break;
    }
    F.check(B.CreateTrunc(F.spill(Value), B.getInt8Ty()),
            Form == 0 || Form == 2 || Form == 4);
  }
}

TEST(RegistrationFrameBits, DoesNotErasePoisonGeneratingFlags) {
  SliceFixture F;
  auto &B = F.Builder;
  auto *Shift = B.CreateShl(F.Address, B.getInt32(8));
  llvm::cast<llvm::BinaryOperator>(Shift)->setHasNoUnsignedWrap();
  F.check(B.CreateTrunc(Shift, B.getInt8Ty()), false);
}
} // namespace
