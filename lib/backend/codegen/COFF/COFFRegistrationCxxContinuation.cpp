//===- COFFRegistrationCxxContinuation.cpp - Catch resumes --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationCxxIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameAddress.h"

namespace neverd::coff_registration {

llvm::Expected<const llvm::CatchReturnInst *> validateCxxContinuationRestore(
    const llvm::Instruction &Anchor, const RegistrationFrame &Frame,
    int32_t SavedStackSlot, const RegistrationCxxContinuation &Resume) {
  // Require the writeback immediately before catchret. A receipt on another
  // store, an earlier write or a restored physical ESP cannot substitute for
  // this effect in the recovered source frame.
  const auto *Slot =
      llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(next(Anchor));
  const auto *Saved =
      Slot ? llvm::dyn_cast_or_null<llvm::GetElementPtrInst>(next(*Slot))
           : nullptr;
  const auto *Value =
      Saved ? llvm::dyn_cast_or_null<llvm::PtrToIntInst>(next(*Saved))
            : nullptr;
  const auto *Store =
      Value ? llvm::dyn_cast_or_null<llvm::StoreInst>(next(*Value)) : nullptr;
  const auto *Return =
      Store ? llvm::dyn_cast_or_null<llvm::CatchReturnInst>(next(*Store))
            : nullptr;
  const int64_t SavedOffset =
      int64_t(Frame.Establisher) + Resume.SavedStackOffset;
  if (!Slot || !Saved || !Value || !Store || !Return ||
      Frame.Establisher > INT32_MAX || SavedStackSlot > -16 ||
      Resume.SavedStackOffset > SavedStackSlot || SavedOffset < 0 ||
      Slot->getPointerOperand() != Frame.Slot ||
      Saved->getPointerOperand() != Frame.Slot ||
      registration_frame::checkedByteGEPOffset(Slot) !=
          int64_t(Frame.Establisher) + SavedStackSlot ||
      registration_frame::checkedByteGEPOffset(Saved) != SavedOffset ||
      Value->getPointerOperand() != Saved ||
      !Value->getType()->isIntegerTy(32) ||
      Store->getPointerOperand() != Slot || Store->getValueOperand() != Value ||
      Store->isAtomic() || !Store->isVolatile() ||
      Store->getAlign() != llvm::Align(1))
    return rejectIR("C++ catch changed its runtime SavedESP writeback");
  return Return;
}

} // namespace neverd::coff_registration
