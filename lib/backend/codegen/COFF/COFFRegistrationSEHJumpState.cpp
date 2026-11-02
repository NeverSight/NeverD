//===- COFFRegistrationSEHJumpState.cpp - Resumed state replay -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independently checks logical state projection after a native capture.

#include "COFFRegistrationSEHJump.h"

#include "neverd/ir/med/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/Operator.h"

namespace neverd::coff_registration {
namespace {

bool isStatePointer(const llvm::GetElementPtrInst *GEP,
                    const llvm::CallBase &Call,
                    const RegistrationFrame &Frame) {
  llvm::APInt Offset(32, 0);
  return GEP && GEP->getPointerOperand() == Frame.Slot && Frame.EntrySP >= 8 &&
         GEP->accumulateConstantOffset(Call.getModule()->getDataLayout(),
                                       Offset) &&
         Offset.getZExtValue() == Frame.EntrySP - 8;
}

llvm::Expected<const llvm::LoadInst *>
validateCapturedState(const llvm::AllocaInst &Slot, const llvm::CallBase &Call,
                      const RegistrationFrame &Frame,
                      std::set<const llvm::Instruction *> &Saves,
                      std::set<const llvm::Instruction *> &Restores) {
  const llvm::StoreInst *Save = nullptr;
  const llvm::LoadInst *Reload = nullptr;
  for (const auto *User : Slot.users()) {
    if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(User);
        Store && Store->getPointerOperand() == &Slot && !Save)
      Save = Store;
    else if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(User);
             Load && Load->getPointerOperand() == &Slot && !Reload)
      Reload = Load;
    else
      return rejectIR("native captured state escapes or has extra uses");
  }
  const auto *Read =
      Save ? llvm::dyn_cast<llvm::LoadInst>(Save->getValueOperand()) : nullptr;
  const auto *GEP =
      Read ? llvm::dyn_cast<llvm::GetElementPtrInst>(Read->getPointerOperand())
           : nullptr;
  if (!Save || !Reload || !Read || !Read->hasOneUse() ||
      !isStatePointer(GEP, Call, Frame) || !GEP->hasOneUse() ||
      !Read->getType()->isIntegerTy(32) ||
      !Reload->getType()->isIntegerTy(32) || !Read->isVolatile() ||
      Read->isAtomic() || Read->getAlign() != llvm::Align(1) ||
      !Save->isVolatile() || Save->isAtomic() ||
      Save->getAlign() != llvm::Align(4) || !Reload->isVolatile() ||
      Reload->isAtomic() || Reload->getAlign() != llvm::Align(4))
    return rejectIR("native capture changed its source state snapshot");
  Saves.insert({GEP, Read, Save});
  Restores.insert(Reload);
  return Reload;
}

} // namespace

llvm::Error
validateSEHJumpState(const X86RegistrationSEHJumpCall &Expected,
                     const llvm::CallBase &Call, const llvm::StoreInst &State,
                     const RegistrationFrame &Frame,
                     const llvm::AllocaInst *CapturedState,
                     std::set<const llvm::Instruction *> &Saves,
                     std::set<const llvm::Instruction *> &Restores) {
  const auto *GEP =
      llvm::dyn_cast<llvm::GetElementPtrInst>(State.getPointerOperand());
  const auto ResumeLevel = Expected.ResumeLevel;
  const llvm::Value *Initial = State.getValueOperand();
  const llvm::LoadInst *Captured = nullptr;
  const bool Multiple = Expected.Capture->Levels.size() > 1;
  if (Multiple != bool(CapturedState))
    return rejectIR("native capture lost its saved source state");
  if (CapturedState) {
    auto Read =
        validateCapturedState(*CapturedState, Call, Frame, Saves, Restores);
    if (!Read)
      return Read.takeError();
    Captured = *Read;
  }
  size_t CapturedTests = 0;
  if (Multiple || !ResumeLevel ||
      *ResumeLevel != Expected.Capture->Levels.front()) {
    const auto *Select = llvm::dyn_cast<llvm::SelectInst>(Initial);
    const auto *Compare =
        Select ? llvm::dyn_cast<llvm::ICmpInst>(Select->getCondition())
               : nullptr;
    if (!Select || !Compare || Compare->hasPoisonGeneratingFlags() ||
        !Select->hasOneUse() || !Compare->hasOneUse() ||
        Compare->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
        Compare->getOperand(0) != &Call ||
        integer(Compare->getOperand(1), 32) != 0)
      return rejectIR("native jump changed its resumed source try level");
    if (ResumeLevel) {
      if (integer(Select->getFalseValue(), 32) != uint32_t(*ResumeLevel))
        return rejectIR("native jump changed its resumed source try level");
    } else {
      const llvm::Value *Projected = Select->getFalseValue();
      std::vector<const llvm::ICmpInst *> Tests;
      for (auto It = Expected.ResumeStateMap.rbegin();
           It != Expected.ResumeStateMap.rend(); ++It) {
        const auto *Step = llvm::dyn_cast<llvm::SelectInst>(Projected);
        const llvm::Value *Condition = Step ? Step->getCondition() : nullptr;
        if (Captured) {
          const auto *Both =
              llvm::dyn_cast_or_null<llvm::BinaryOperator>(Condition);
          const auto *SavedTest =
              Both ? llvm::dyn_cast<llvm::ICmpInst>(Both->getOperand(1))
                   : nullptr;
          if (!Both || Both->getOpcode() != llvm::Instruction::And ||
              !Both->hasOneUse() || !SavedTest || !SavedTest->hasOneUse() ||
              SavedTest->hasPoisonGeneratingFlags() ||
              SavedTest->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
              SavedTest->getOperand(0) != Captured ||
              integer(SavedTest->getOperand(1), 32) !=
                  uint32_t(It->first.first))
            return rejectIR("native jump changed its captured unwind bound");
          Condition = Both->getOperand(0);
          Restores.insert({Both, SavedTest});
          ++CapturedTests;
        }
        const auto *Test = llvm::dyn_cast_or_null<llvm::ICmpInst>(Condition);
        if (!Step || !Test || !Step->hasOneUse() || !Test->hasOneUse() ||
            Test->hasPoisonGeneratingFlags() ||
            Test->getPredicate() != llvm::ICmpInst::ICMP_EQ ||
            integer(Test->getOperand(1), 32) != uint32_t(It->first.second) ||
            integer(Step->getTrueValue(), 32) != uint32_t(It->second))
          return rejectIR("native jump changed its unwind state projection");
        Tests.push_back(Test);
        Restores.insert({Step, Test});
        Projected = Step->getFalseValue();
      }
      const auto *Current = llvm::dyn_cast<llvm::LoadInst>(Projected);
      if (!Current || Current->getNumUses() != Tests.size() + 1 ||
          !Current->isVolatile() || Current->isAtomic() ||
          Current->getAlign() != llvm::Align(1) ||
          Current->getPointerOperand() != State.getPointerOperand())
        return rejectIR("native jump changed its current source state read");
      for (const auto *Test : Tests)
        if (Test->getOperand(0) != Current)
          return rejectIR("native jump projection changed its observed state");
      Restores.insert(Current);
    }
    Initial = Select->getTrueValue();
    Restores.insert({Compare, Select});
  }
  if (!isStatePointer(GEP, Call, Frame) ||
      (Captured
           ? Initial != Captured || Captured->getNumUses() != CapturedTests + 1
           : integer(Initial, 32) !=
                 uint32_t(Expected.Capture->Levels.front())) ||
      State.isAtomic() || !State.isVolatile() ||
      State.getAlign() != llvm::Align(1))
    return rejectIR("native jump changed its restored source try level");
  Restores.insert({GEP, &State});
  return llvm::Error::success();
}

} // namespace neverd::coff_registration
