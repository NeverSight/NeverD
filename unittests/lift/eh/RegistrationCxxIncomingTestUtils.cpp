//===- RegistrationCxxIncomingTestUtils.cpp - Caller argument proofs ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "RegistrationCxxIncomingTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {
void checkCxxIncomingEdits(const llvm::Function &Parent,
                           const ExceptionFunction &Source,
                           const BinaryImage &Image) {
  bool HasIncoming = false;
  for (const auto &Block : Parent)
    for (const auto &I : Block)
      HasIncoming |=
          I.getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment) !=
          nullptr;
  if (!HasIncoming)
    return;
  const char *Names[] = {
      "missing incoming occurrence", "shifted physical word",
      "inbounds caller address",     "missing volatile read",
      "strengthened alignment",      "changed caller slot owner",
      "wrong frame depth",           "hidden caller slot overwrite",
      "unescaped caller slot",       "unindexed caller read",
      "unindexed caller write",      "private frame escapes to caller",
      "changed source occurrence",   "register instead of stack argument",
      "callee pops caller words"};
  for (unsigned Mutation = 0; Mutation != std::size(Names); ++Mutation) {
    SCOPED_TRACE(Names[Mutation]);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::LoadInst *Read = nullptr;
    llvm::StoreInst *Write = nullptr;
    llvm::AllocaInst *Slot = nullptr, *Frame = nullptr;
    llvm::IntrinsicInst *Escape = nullptr, *FrameAddress = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block) {
        if (I.getMetadata(windows_eh_md::RegistrationCallerFrameAttachment))
          Slot = llvm::cast<llvm::AllocaInst>(&I);
        if (I.getMetadata(windows_eh_md::RegistrationFrameAttachment))
          Frame = llvm::cast<llvm::AllocaInst>(&I);
        if (I.getMetadata(windows_eh_md::RegistrationIncomingFrameAttachment)) {
          if (auto *Load = llvm::dyn_cast<llvm::LoadInst>(&I))
            Read = Load;
          if (auto *Store = llvm::dyn_cast<llvm::StoreInst>(&I))
            Write = Store;
        }
        if (auto *Intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&I)) {
          if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::localescape)
            Escape = Intrinsic;
          if (Intrinsic->getIntrinsicID() == llvm::Intrinsic::frameaddress)
            FrameAddress = Intrinsic;
        }
      }
    ASSERT_TRUE(Read && Slot && Frame && Escape && FrameAddress);
    if (!Write && (Mutation == 11 || Mutation == 12))
      continue;
    llvm::IRBuilder<> B(Read);
    auto *Pointer =
        llvm::cast<llvm::GetElementPtrInst>(Read->getPointerOperand());
    switch (Mutation) {
    case 0:
      Read->setMetadata(windows_eh_md::RegistrationIncomingFrameAttachment,
                        nullptr);
      break;
    case 1:
      Pointer->setOperand(1, B.getInt32(16));
      break;
    case 2:
      Pointer->setIsInBounds(true);
      break;
    case 3:
      Read->setVolatile(false);
      break;
    case 4:
      Read->setAlignment(llvm::Align(4));
      break;
    case 5:
      Slot->setMetadata(
          windows_eh_md::RegistrationCallerFrameAttachment,
          llvm::MDNode::get(B.getContext(),
                            {llvm::ConstantAsMetadata::get(B.getInt64(1))}));
      break;
    case 6:
      FrameAddress->setArgOperand(0, B.getInt32(1));
      break;
    case 7:
      B.CreateStore(Frame, B.CreateGEP(B.getInt8Ty(), Slot, B.getInt32(1)));
      break;
    case 8:
      Escape->setArgOperand(1, Frame);
      break;
    case 9:
      B.CreateLoad(B.getInt32Ty(), Read->getPointerOperand())
          ->setVolatile(true);
      break;
    case 10:
      B.CreateStore(B.getInt32(1), Read->getPointerOperand())
          ->setVolatile(true);
      break;
    case 11:
      B.SetInsertPoint(Write);
      Write->setOperand(0, B.CreatePtrToInt(Frame, B.getInt32Ty()));
      break;
    case 12:
      Read->setMetadata(
          windows_eh_md::RegistrationIncomingFrameAttachment,
          Write->getMetadata(
              windows_eh_md::RegistrationIncomingFrameAttachment));
      break;
    case 13:
      ASSERT_FALSE(Function->arg_empty());
      Function->addParamAttr(0, llvm::Attribute::InReg);
      break;
    case 14:
      Function->setCallingConv(llvm::CallingConv::X86_StdCall);
      break;
    }
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Function, Source, Image);
    EXPECT_TRUE(bool(Error));
    if (Error)
      llvm::consumeError(std::move(Error));
  }
}
} // namespace neverd::registration_test
