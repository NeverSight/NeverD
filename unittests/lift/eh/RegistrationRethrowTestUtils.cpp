//===- RegistrationRethrowTestUtils.cpp - Rethrow argument edits ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject verifier-clean call edits and preserve current source arguments.
//===----------------------------------------------------------------------===//
#include "RegistrationRethrowTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {
TEST(RegistrationRethrowHighC, BareThrowRequiresTwoNullRuntimeArguments) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    HighFunc Function;
    Function.Name = "checked_rethrow";
    Function.Entry = 0x401000;
    Function.ReturnType = NdType::makeVoid();
    HighStmt Call;
    Call.Kind = StmtKind::Call;
    Call.CallExpr = HighExpr::makeCall(
        "_CxxThrowException", 0x402000,
        {HighExpr::makeConst(0, 4), HighExpr::makeConst(0, 4)});
    auto &Args = Call.CallExpr->Operands;
    if (Mutation == 1)
      Args[1] = HighExpr::makeConst(1, 4);
    if (Mutation == 2)
      Args.pop_back();
    if (Mutation == 3)
      Args.clear();
    if (Mutation == 4)
      Args[0] =
          HighExpr::makeUnary(NdOp::INT_NEGATE, HighExpr::makeConst(0, 4));
    Function.Body.push_back(std::move(Call));
    CEmitterOptions Options;
    Options.TheArch = Arch::X86;
    Options.Format = BinaryFormat::COFF;
    std::string Text;
    llvm::raw_string_ostream OS(Text);
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
    EXPECT_EQ(Text.find("throw;") != std::string::npos, Mutation == 0) << Text;
    if (Mutation > 0 && Mutation < 4)
      EXPECT_NE(Text.find("CxxThrowException("), std::string::npos) << Text;
  }
}

void checkRuntimeRethrowEdits(const MedFunc &Med, const llvm::Function &Parent,
                              const BinaryImage &Image) {
  const auto &States = *Med.RegistrationStates;
  const auto Callee = llvm::find_if(States.CalleeContracts, [](const auto &C) {
    return C.isRuntimeRethrow();
  });
  ASSERT_NE(Callee, States.CalleeContracts.end());
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Parent.getParent());
    auto *Function = Module->getFunction(Parent.getName());
    llvm::CallBase *Call = nullptr;
    for (auto &Block : *Function)
      for (auto &I : Block)
        if (auto *Candidate = llvm::dyn_cast<llvm::CallBase>(&I))
          if (const auto *Target = Candidate->getCalledFunction();
              Target &&
              Target->getName().starts_with("__nd_registration_rethrow_")) {
            ASSERT_FALSE(Call);
            Call = Candidate;
          }
    ASSERT_TRUE(Call);
    ASSERT_EQ(Call->arg_size(), 2u);
    EXPECT_EQ(Call->getCallingConv(), llvm::CallingConv::X86_StdCall);
    for (const auto &Argument : Call->args())
      EXPECT_TRUE(llvm::isa<llvm::ConstantPointerNull>(Argument.get()));
    if (Mutation < 2)
      Call->setArgOperand(
          Mutation, llvm::ConstantExpr::getIntToPtr(
                        llvm::ConstantInt::get(
                            llvm::Type::getInt32Ty(Module->getContext()), 1),
                        Call->getArgOperand(Mutation)->getType()));
    if (Mutation == 2) {
      Call->setCallingConv(llvm::CallingConv::C);
      Call->getCalledFunction()->setCallingConv(llvm::CallingConv::C);
    }
    if (Mutation == 3) {
      Call->removeFnAttr(llvm::Attribute::NoReturn);
      Call->getCalledFunction()->removeFnAttr(llvm::Attribute::NoReturn);
    }
    if (Mutation == 4)
      rewrite_source::setOriginalVA(*Call->getCalledFunction(),
                                    Callee->Target + 6);
    if (Mutation == 5)
      Call->addParamAttr(0, llvm::Attribute::InReg);
    if (Mutation == 6)
      Call->getCalledFunction()->addParamAttr(1, llvm::Attribute::InReg);
    if (Mutation == 7)
      Call->addParamAttr(0, llvm::Attribute::NonNull);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error =
        validateCOFFRegistrationCxxIR(*Function, *Med.ExceptionMetadata, Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
  for (unsigned Argument = 0; Argument != 2; ++Argument) {
    SCOPED_TRACE(Argument);
    auto Changed = Med;
    auto Call = llvm::find_if(Changed.CallInfos, [&](const auto &Candidate) {
      return Candidate.TargetAddr == Callee->Target;
    });
    ASSERT_NE(Call, Changed.CallInfos.end());
    ASSERT_EQ(Call->Args.size(), 2u);
    Call->Args[Argument] = MedVar::makeConst(1, 4);
    llvm::LLVMContext Context;
    auto Module =
        MedLLVMEmitter().emit({Changed}, Context, "edited-rethrow", Arch::X86,
                              {}, &Image, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    const auto *Function = Module->getFunction(Changed.Name);
    ASSERT_TRUE(Function);
    EXPECT_FALSE(Function->getMetadata(windows_eh_md::NativeAttachment));
    const auto High = MedToHighConverter().convert(Changed, Arch::X86);
    bool Found = false;
    walkStmts(High.Body, [&](const auto &Stmt) {
      // The HighIR call must retain the edited argument instead of regenerating
      // nulls from the LowIR admission receipt.
      auto Check = [&](const ExprPtr &Expr) {
        if (!Expr || Expr->Kind != ExprKind::Call ||
            Expr->CallAddr != Callee->Target)
          return;
        Found = true;
        ASSERT_EQ(Expr->Operands.size(), 2u);
        EXPECT_EQ(Expr->Operands[Argument]->Kind, ExprKind::Const);
        EXPECT_EQ(Expr->Operands[Argument]->ConstVal, 1u);
      };
      Check(Stmt.CallExpr);
      Check(Stmt.Val);
    });
    EXPECT_TRUE(Found);
  }
}
} // namespace neverd::registration_test
