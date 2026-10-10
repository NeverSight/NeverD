//===- RegistrationCleanupTestUtils.cpp - PE32 cleanup proof edits ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Reject verifier-clean changes to cleanup order, borrowing and effects.
//===----------------------------------------------------------------------===//

#include "RegistrationCleanupTestUtils.h"

#include "gtest/gtest.h"

#include "neverd/backend/codegen/COFF/COFFRegistrationPatch.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/X86RegistrationCallback.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::registration_test {
void checkCatchCleanupReachability(const LowFunc &Low,
                                   const BinaryImage &Image) {
  const auto &States = Low.RegistrationStates->Blocks;
  const auto Dead = llvm::find_if(States, [&](const auto &S) {
    if (S.Reached)
      return false;
    const auto Block = llvm::find_if(Low.Blocks, [&](const auto &B) {
      return B.StartAddr == S.Range.Begin && B.EndAddr == S.Range.End;
    });
    return Block != Low.Blocks.end() &&
           llvm::any_of(Block->Ops,
                        [](const auto &Op) { return Op.Opcode == NdOp::CALL; });
  });
  // O1 removes the entire normal destructor tail. O0 retains actual calls;
  // a trivial jump-only block can also disappear through ordinary CFG folding.
  if (Dead == States.end())
    return;
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Low;
    auto &Proof = *Changed.RegistrationStates;
    if (Mutation == 1) {
      Changed.OrdinaryModuleAnalysisRoots.insert(Dead->Range.Begin);
      Changed.ModuleAnalysisRoots.insert(Dead->Range.Begin);
    }
    if (Mutation == 2)
      Proof.Complete = false;
    if (Mutation == 3)
      Proof.CallbackStatesComplete = false;
    if (Mutation == 4)
      llvm::find_if(Proof.Blocks, [&](const auto &S) {
        return S.Range.Begin == Dead->Range.Begin;
      })->Range.End += 1;
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto Med = Converter.convert(Changed, Arch::X86, BinaryFormat::COFF);
    const bool Retained = llvm::any_of(Med.Blocks, [&](const auto &B) {
      return B.StartAddr == Dead->Range.Begin;
    });
    EXPECT_EQ(Retained, Mutation != 0);
  }
}

void checkCatchCleanupEdits(const MedFunc &Med, const llvm::Function &Function,
                            const BinaryImage &Image) {
  const auto &EH = *Med.ExceptionMetadata;
  const auto &States = *Med.RegistrationStates;
  ASSERT_FALSE(States.CleanupContracts.empty());
  ASSERT_LE(States.CleanupContracts.size(), 2u);
  ASSERT_EQ(States.CleanupFrameEffects.size(), States.CleanupContracts.size());
  size_t CallsInSource = 0;
  for (const auto &Contract : States.CleanupContracts)
    CallsInSource += Contract.Calls.size();
  ASSERT_EQ(CallsInSource, 2u);
  ASSERT_FALSE(EH.FragmentRanges.empty());
  for (const auto &Contract : States.CleanupContracts)
    EXPECT_FALSE(EH.ownsCode(Contract.RelayTarget));
  for (const auto &Try : EH.Cxx->TryBlocks)
    for (const auto &Catch : Try.Handlers)
      EXPECT_TRUE(EH.ownsCode(Catch.HandlerVA));
  const auto Parents = registrationCleanupParents(Med);
  ASSERT_TRUE(Parents);
  ASSERT_EQ(Parents->size(), States.CleanupContracts.size());
  for (const auto &[State, Owner] : *Parents)
    EXPECT_EQ(Owner, (RegistrationCatchIdentity{1, 0}));

  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Module = llvm::CloneModule(*Function.getParent());
    auto *Parent = Module->getFunction(Function.getName());
    auto Source = EH;
    std::vector<llvm::CallInst *> Calls;
    llvm::CleanupReturnInst *FirstReturn = nullptr;
    for (auto &Block : *Parent)
      for (auto &I : Block) {
        if (auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
            Call && Call->getCalledFunction() &&
            Call->getCalledFunction()->getName().starts_with(
                "__nd_registration_cleanup.ecx_"))
          Calls.push_back(Call);
        if (auto *Return = llvm::dyn_cast<llvm::CleanupReturnInst>(&I);
            Return && Return->getUnwindDest() &&
            llvm::isa<llvm::CleanupPadInst>(
                &*Return->getUnwindDest()->getFirstNonPHIIt()))
          FirstReturn = Return;
      }
    ASSERT_EQ(Calls.size(), 2u);
    ASSERT_EQ(bool(FirstReturn), States.CleanupContracts.size() == 2);
    auto *Call = Calls.front();
    llvm::IRBuilder<> Builder(Call);
    if (Mutation == 0)
      Call->eraseFromParent();
    if (Mutation == 1)
      Call->setArgOperand(0, Builder.CreateGEP(Builder.getInt8Ty(),
                                               Call->getArgOperand(0),
                                               Builder.getInt32(1)));
    if (Mutation == 2)
      Call->setArgOperand(0, Builder.Insert(llvm::cast<llvm::Instruction>(
                                                Calls.back()->getArgOperand(0))
                                                ->clone()));
    if (Mutation == 3)
      Call->setMemoryEffects(llvm::MemoryEffects::none());
    if (Mutation == 4)
      Call->getCalledFunction()->setMemoryEffects(llvm::MemoryEffects::none());
    if (Mutation == 5)
      Call->addParamAttr(0, llvm::Attribute::InReg);
    if (Mutation == 6)
      Call->getCalledFunction()->addParamAttr(0, llvm::Attribute::NonNull);
    if (Mutation == 7)
      Call->removeFnAttr(llvm::Attribute::NoUnwind);
    if (Mutation == 8) {
      if (FirstReturn)
        FirstReturn->setUnwindDest(
            llvm::cast<llvm::CleanupReturnInst>(
                FirstReturn->getUnwindDest()->getTerminator())
                ->getUnwindDest());
      else {
        // O1 combines both destructors in one action. Keep valid SSA while
        // reversing its object order; O0 instead skips an entire unwind step.
        auto *Second = Builder.Insert(
            llvm::cast<llvm::Instruction>(Calls.back()->getArgOperand(0))
                ->clone());
        Calls.back()->setArgOperand(0, Call->getArgOperand(0));
        Call->setArgOperand(0, Second);
      }
    }
    if (Mutation == 9)
      Source.FragmentRanges.back().End += 1;
    if (Mutation == 10)
      Source.FragmentRanges.clear();
    if (Mutation == 11)
      Call->addFnAttr(llvm::Attribute::NoSync);
    if (Mutation == 12)
      Call->getCalledFunction()->addFnAttr(llvm::Attribute::WillReturn);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    auto Error = validateCOFFRegistrationCxxIR(*Parent, Source, Image);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
  }
}
} // namespace neverd::registration_test
