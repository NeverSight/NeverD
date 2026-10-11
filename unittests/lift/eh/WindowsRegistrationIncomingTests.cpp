//===- WindowsRegistrationIncomingTests.cpp - PE32 caller arguments ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "../../../lib/backend/llvm/X86/MedLLVMRegistrationIncoming.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86RegistrationEntry.h"
#include "neverd/ir/TargetRegInfo.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

using namespace neverd;

namespace {
TEST(WindowsRegistrationIncoming, EntryRequiresObservedPhysicalWords) {
  llvm::LLVMContext Context;
  llvm::Module Module("entry", Context);
  llvm::IRBuilder<> B(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), {B.getInt32Ty(), B.getInt32Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  MedFunc Source;
  Source.RegistrationCallerCleanupABIComplete = true;
  Source.RegistrationStates.emplace();
  Source.RegistrationStates->IncomingFrameAccessesComplete = true;
  for (int Index = 0; Index != 2; ++Index) {
    MedVar Param;
    Param.Kind = MedVar::Param;
    Param.RegOff = kNoParamReg;
    Param.Id = Index;
    Param.Size = 4;
    Source.Params.push_back(Param);
    Source.RegistrationStates->IncomingFrameAccesses.push_back(
        {va_t(0x1000 + Index), Index, 8 + Index * 4, 4, false});
  }
  EXPECT_EQ(getX86RegistrationCxxEntryABI(Source, *Function),
            llvm::CallingConv::C);
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Source;
    auto &State = *Changed.RegistrationStates;
    switch (Mutation) {
    case 0:
      Changed.RegistrationCallerCleanupABIComplete = false;
      break;
    case 1:
      Changed.CalleePopBytes = 8;
      break;
    case 2:
      State.IncomingFrameAccessesComplete = false;
      break;
    case 3:
      State.IncomingFrameAccesses.pop_back();
      break;
    case 4:
      State.IncomingFrameAccesses[1].Write = true;
      break;
    case 5:
      State.IncomingFrameAccesses[1].Offset = 16;
      break;
    case 6:
      State.IncomingFrameAccesses[1].Width = 1;
      break;
    case 7:
      Changed.Params[0].RegOff = getTargetRegInfo(Arch::X86).IntParamRegs[0];
      break;
    case 8:
      Changed.Params[1].Id = 2;
      break;
    case 9:
      Changed.IsVariadic = true;
      break;
    }
    EXPECT_FALSE(getX86RegistrationCxxEntryABI(Changed, *Function));
  }
  Function->addParamAttr(0, llvm::Attribute::InReg);
  EXPECT_FALSE(getX86RegistrationCxxEntryABI(Source, *Function));
}

TEST(WindowsRegistrationIncoming,
     FailedProjectionRestoresMemoryAndDeclaration) {
  llvm::LLVMContext Context;
  llvm::Module Module("incoming-rollback", Context);
  Module.setDataLayout("e-p:32:32");
  llvm::IRBuilder<> B(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(B.getInt32Ty(), false),
      llvm::GlobalValue::ExternalLinkage, "parent", Module);
  B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Slot = B.CreateAlloca(B.getInt32Ty());
  B.CreateStore(B.getInt32(1), Slot);
  B.CreateBr(llvm::BasicBlock::Create(Context, "body", Function));
  B.SetInsertPoint(&Function->back());
  auto *Load = B.CreateLoad(B.getInt32Ty(), Slot);
  auto *Store = B.CreateStore(Load, Slot);
  Store->setVolatile(true);
  auto *Original =
      llvm::MDNode::get(Context, llvm::MDString::get(Context, "original"));
  Load->setMetadata(windows_eh_md::RegistrationIncomingFrameAttachment,
                    Original);
  B.CreateRet(Load);
  MedFunc Source;
  Source.RegistrationStates.emplace();
  Source.RegistrationStates->IncomingFrameAccessesComplete = true;
  Source.Blocks.resize(1);
  for (bool Write : {false, true}) {
    MedOp Op;
    Op.Opcode = Write ? NdOp::STORE : NdOp::LOAD;
    Op.Addr = 0x1000 + unsigned(Write);
    Op.OriginSeq = unsigned(Write);
    Source.Blocks[0].Ops.push_back(Op);
    Source.RegistrationStates->IncomingFrameAccesses.push_back(
        {Op.Addr, Op.OriginSeq, 8, 4, Write});
  }
  std::map<std::pair<va_t, int>, llvm::Instruction *> Instructions{
      {{0x1000, 0}, Load}, {{0x1001, 1}, Store}};
  const auto Plan =
      x86_registration::prepareIncomingFrame(Source, *Function, Instructions);
  ASSERT_TRUE(Plan);
  std::string Before;
  llvm::raw_string_ostream BeforeStream(Before);
  Module.print(BeforeStream, nullptr);
  {
    x86_registration::IncomingFrameProjection Projection(*Function, 0x1000,
                                                         *Plan);
    EXPECT_TRUE(Projection.slot());
    EXPECT_TRUE(Load->isVolatile());
    EXPECT_NE(Load->getPointerOperand(), Slot);
    EXPECT_TRUE(Module.getFunction("llvm.frameaddress.p0"));
    ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  }
  std::string After;
  llvm::raw_string_ostream AfterStream(After);
  Module.print(AfterStream, nullptr);
  EXPECT_EQ(Before, After);
  EXPECT_FALSE(Load->isVolatile());
  EXPECT_TRUE(Store->isVolatile());
  EXPECT_FALSE(Module.getFunction("llvm.frameaddress.p0"));
  auto Duplicate = Instructions;
  Duplicate[{0x1001, 1}] = Load;
  EXPECT_FALSE(
      x86_registration::prepareIncomingFrame(Source, *Function, Duplicate));
  Source.RegistrationStates->IncomingFrameAccesses[0].Width = 8;
  EXPECT_FALSE(
      x86_registration::prepareIncomingFrame(Source, *Function, Instructions));
}
} // namespace
