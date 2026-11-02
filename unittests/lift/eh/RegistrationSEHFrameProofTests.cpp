//===- RegistrationSEHFrameProofTests.cpp - PE32 frame proofs ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationSEHProof.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

namespace {

MedFunc cookieCall() {
  MedFunc Function;
  Function.ExceptionMetadata.emplace();
  Function.ExceptionMetadata->Registration.emplace();
  Function.RegistrationStates.emplace();
  auto &States = *Function.RegistrationStates;
  States.SecurityCookiesComplete = true;
  States.CookieCheckVA = 0x2000;
  States.CookieChecks.push_back({0x1010, 0x1015, 3});
  MedBlock Block;
  Block.Id = 1;
  MedOp Root;
  Root.Opcode = NdOp::COPY;
  Root.Addr = 0x1000;
  Root.OriginSeq = 2;
  Root.Output.Kind = MedVar::Temp;
  Root.Output.Id = 12;
  Root.Output.SSAVer = 1;
  Root.Output.Size = 4;
  Block.Ops.push_back(Root);
  States.FrameValues.push_back({Root.Addr, Root.OriginSeq, std::nullopt});
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1010;
  Call.OriginSeq = 3;
  Call.addInput(MedVar::makeConst(0x2000, 4));
  Block.Ops.push_back(Call);
  Function.Blocks.push_back(Block);
  MedCallInfo Info;
  Info.BlockId = 1;
  Info.OpIdx = 1;
  Info.Args.push_back(Root.Output);
  Function.CallInfos.push_back(Info);
  return Function;
}

bool provesFramePrivacy(const MedFunc &Function) {
  return x86_registration::getPrivateSEHFrameValues(
             Function, *Function.RegistrationStates,
             [](const MedOp &) { return false; })
      .has_value();
}

TEST(RegistrationSEHFrameProof, CheckedDecodedCookieIsNotAFrameEscape) {
  auto Function = cookieCall();
  EXPECT_TRUE(provesFramePrivacy(Function));
}

TEST(RegistrationSEHFrameProof, CookieExemptionRequiresItsExactSourceCall) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = cookieCall();
    auto &States = *Function.RegistrationStates;
    auto &Call = Function.Blocks.front().Ops.back();
    switch (Mutation) {
    case 0:
      States.SecurityCookiesComplete = false;
      break;
    case 1:
      States.CookieChecks.clear();
      break;
    case 2:
      Call.Addr += 1;
      break;
    case 3:
      Call.OriginSeq += 1;
      break;
    case 4:
      Call.Inputs[0].ConstVal += 1;
      break;
    case 5:
      Function.CallInfos.front().Args.front().Size = 8;
      break;
    case 6:
      Function.CallInfos.front().Args.push_back(MedVar::makeConst(0, 4));
      break;
    case 7:
      Function.CallInfos.front().OpIdx = 2;
      break;
    }
    EXPECT_FALSE(provesFramePrivacy(Function));
  }
}

TEST(RegistrationSEHFrameProof, CallbackStoresRequireBoundedPrivateScratch) {
  for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("callback-scratch", Context);
    Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
    auto *Parent = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false),
        llvm::GlobalValue::ExternalLinkage, "parent", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Parent);
    llvm::IRBuilder<> B(Entry);
    auto *Slot = B.CreateAlloca(B.getInt32Ty());
    B.CreateRetVoid();
    auto *Callback = llvm::BasicBlock::Create(Context, "callback", Parent);
    B.SetInsertPoint(Callback);
    auto *Seed = B.CreateStore(B.getInt32(0), Slot);
    auto *SP = B.CreateLoad(B.getInt32Ty(), Slot);
    auto *Address = B.CreateIntToPtr(
        B.CreateAdd(SP, B.getInt32(Mutation == 1 ? 0 : -4)), B.getPtrTy());
    auto *Scratch = B.CreateStore(B.getInt32(23), Address);
    llvm::Value *Value =
        Mutation == 2 ? static_cast<llvm::Value *>(SP) : B.getInt32(42);
    auto *Outside = B.CreateStore(
        Value,
        llvm::ConstantExpr::getIntToPtr(B.getInt32(0x1234000), B.getPtrTy()));
    B.CreateRetVoid();
    const X86RegistrationRootSeed Root{Seed,
                                       X86RegistrationRootKind::StackPointer};
    X86RegistrationCallbackRequest Request;
    Request.Entry = Callback;
    Request.Blocks = {Callback};
    Request.Frame.RootSeeds = {&Root, 1};
    Request.Frame.InferStackBounds = true;
    const auto Stores =
        x86_registration::getCheckedSEHCallbackStores({Request});
    if (Mutation) {
      EXPECT_FALSE(Stores);
    } else {
      ASSERT_TRUE(Stores);
      EXPECT_EQ(Stores->size(), 1u);
      EXPECT_TRUE(Stores->count(Scratch));
      EXPECT_FALSE(Stores->count(Outside));
    }
  }
}

TEST(RegistrationSEHFrameProof, CallbackJumpFramesRequireTheExactParent) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    llvm::LLVMContext Context;
    llvm::Module Module("callback-jump-parent", Context);
    auto *I32 = llvm::Type::getInt32Ty(Context);
    auto *Parent = llvm::Function::Create(llvm::FunctionType::get(I32, false),
                                          llvm::GlobalValue::ExternalLinkage,
                                          "parent", Module);
    auto *Other = llvm::Function::Create(Parent->getFunctionType(),
                                         llvm::GlobalValue::ExternalLinkage,
                                         "other", Module);
    auto *Callback = llvm::Function::Create(Parent->getFunctionType(),
                                            llvm::GlobalValue::InternalLinkage,
                                            "filter", Module);
    auto *Entry = llvm::BasicBlock::Create(Context, "entry", Callback);
    llvm::IRBuilder<> B(Entry);
    auto *Runtime = B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(
            &Module, llvm::Intrinsic::frameaddress, {B.getPtrTy()}),
        {B.getInt32(Mutation == 1 ? 0 : 1)});
    if (Mutation == 5) {
      auto *Body = llvm::BasicBlock::Create(Context, "body", Callback);
      B.CreateBr(Body);
      B.SetInsertPoint(Body);
    }
    auto *Recover = B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                                     &Module, llvm::Intrinsic::eh_recoverfp),
                                 {Mutation == 2 ? Other : Parent, Runtime});
    if (Mutation == 3)
      B.CreateCall(Recover->getCalledFunction(), {Parent, Runtime});
    if (Mutation == 4)
      Recover->setArgOperand(1, llvm::ConstantPointerNull::get(B.getPtrTy()));
    B.CreateRet(B.getInt32(1));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
    EXPECT_EQ(getX86RegistrationSEHJumpParentFrame(*Callback, *Parent),
              Mutation ? nullptr : Recover);
  }
}

} // namespace
