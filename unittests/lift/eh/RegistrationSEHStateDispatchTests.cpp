//===- RegistrationSEHStateDispatchTests.cpp - State selection edits ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently rejects edits to a reconstructed callback's state selection.

#include "RegistrationSEHStateDispatchTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/ir/med/X86/X86RegistrationSEHStateStore.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {

void checkSEHStateDispatchEdits(const llvm::Function &Parent,
                                const MedFunc &Med,
                                const ExceptionFunction &Source,
                                const BinaryImage &Image) {
#ifdef LLVM_NEVERD_X86_REGISTRATION_SEH_STATE_WRITES
  const auto Stores = getX86RegistrationSEHStateStores(Med);
  ASSERT_TRUE(Stores);
  const size_t Expected = llvm::count_if(
      *Stores, [](const auto &Store) { return Store.TargetLevels.size() > 1; });
  for (size_t Index = 0; Index != Expected; ++Index)
    for (unsigned Mutation = 0; Mutation != 21; ++Mutation) {
      SCOPED_TRACE(Index);
      SCOPED_TRACE(Mutation);
      auto Module = llvm::CloneModule(*Parent.getParent());
      auto *Function = Module->getFunction(Parent.getName());
      std::vector<llvm::SwitchInst *> Dispatches;
      for (auto &Callback : *Module)
        for (auto &Block : Callback) {
          auto *Switch =
              llvm::dyn_cast<llvm::SwitchInst>(Block.getTerminator());
          if (!Switch || !Switch->getNumCases())
            continue;
          auto *Case = Switch->case_begin()->getCaseSuccessor();
          const auto *Call =
              Case->empty() ? nullptr
                            : llvm::dyn_cast<llvm::CallBase>(&Case->front());
          if (Call &&
              Call->getIntrinsicID() == llvm::Intrinsic::x86_seh_setstate)
            Dispatches.push_back(Switch);
        }
      ASSERT_EQ(Dispatches.size(), Expected);
      auto *Dispatch = Dispatches[Index];
      auto *Read = llvm::cast<llvm::LoadInst>(Dispatch->getCondition());
      auto *Store = llvm::cast<llvm::StoreInst>(Read->getPrevNode());
      auto Case = *Dispatch->case_begin();
      auto *CaseBlock = Case.getCaseSuccessor();
      auto *OtherCase = Dispatch->getDefaultDest();
      auto *Write = llvm::cast<llvm::CallInst>(&CaseBlock->front());
      auto *Branch = llvm::cast<llvm::UncondBrInst>(CaseBlock->getTerminator());
      auto *Resume = Branch->getSuccessor(0);
      llvm::IRBuilder<> B(Read);
      switch (Mutation) {
      case 0:
        Read->setVolatile(false);
        break;
      case 1:
        Read->setAlignment(llvm::Align(4));
        break;
      case 2:
        Read->setAtomic(llvm::AtomicOrdering::Monotonic);
        Read->setAlignment(llvm::Align(4));
        break;
      case 3:
        Read->setOperand(0,
                         B.CreateGEP(B.getInt8Ty(), Read->getPointerOperand(),
                                     B.getInt32(4)));
        break;
      case 4:
        Dispatch->setCondition(B.getInt32(0));
        break;
      case 5:
        B.SetInsertPoint(Dispatch);
        B.CreateFreeze(Read);
        break;
      case 6:
        Case.setValue(B.getInt32(Case.getCaseValue()->getZExtValue() + 1));
        break;
      case 7:
        Case.setSuccessor(OtherCase);
        Dispatch->setDefaultDest(CaseBlock);
        break;
      case 8:
        Write->setArgOperand(2, B.getInt32(42));
        break;
      case 9:
        Write->setArgOperand(7, B.getInt64(0));
        break;
      case 10:
        Write->setArgOperand(3, B.getInt64(42));
        break;
      case 11:
        Branch->setSuccessor(0, OtherCase);
        break;
      case 12:
        Write->eraseFromParent();
        break;
      case 13:
        Write->clone()->insertBefore(Branch->getIterator());
        break;
      case 14:
        B.SetInsertPoint(Write);
        B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
            Module.get(), llvm::Intrinsic::sideeffect));
        break;
      case 15:
        Dispatch->setDefaultDest(Resume);
        break;
      case 16: {
        auto *Byte = B.CreateLoad(B.getInt8Ty(), Read->getPointerOperand());
        Byte->setVolatile(true);
        Byte->setAlignment(llvm::Align(1));
        Dispatch->setCondition(B.CreateZExt(Byte, B.getInt32Ty()));
        break;
      }
      case 17:
        Read->moveBefore(Store->getIterator());
        break;
      case 18:
        // Physical block order is not the source execution order.
        CaseBlock->moveBefore(Dispatch->getParent());
        OtherCase->moveBefore(Dispatch->getParent());
        break;
      case 19:
      case 20:
        new llvm::GlobalVariable(
            *Module, B.getPtrTy(), false, llvm::GlobalValue::ExternalLinkage,
            llvm::BlockAddress::get(Mutation == 19 ? CaseBlock : Resume),
            "escaped.state.edge");
        break;
      }
      ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
      auto Error = validateCOFFRegistrationIR(*Function, Source, Image);
      if (Mutation == 18)
        ASSERT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
      else {
        ASSERT_TRUE(bool(Error));
        llvm::consumeError(std::move(Error));
      }
    }
#endif
}

} // namespace neverd::registration_test
