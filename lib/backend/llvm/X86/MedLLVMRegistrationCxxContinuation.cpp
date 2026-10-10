//===- MedLLVMRegistrationCxxContinuation.cpp - Catch resumes -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedLLVMRegistrationCxxContinuation.h"

#include "../eh/MedLLVMEHHelpers.h"

#include "neverd/ir/RegistrationState.h"

namespace neverd {

void emitRegistrationCxxContinuation(
    llvm::ReturnInst &Return, llvm::AllocaInst &Frame, uint64_t Establisher,
    int32_t SavedStackSlot, llvm::CatchPadInst &Pad, llvm::BasicBlock &Target,
    va_t FunctionVA, const RegistrationCxxContinuation &Resume) {
  llvm::IRBuilder<> B(&Return);
  med_llvm_eh::emitWindowsEHProvenanceAnchor(
      B, windows_eh_md::NativeProvenanceModel::X86RegistrationCxx,
      windows_eh_md::NativeProvenanceRole::HandlerTarget, FunctionVA,
      Resume.Address, Resume.TryIndex, Resume.CatchIndex, &Pad, Resume.TargetVA,
      uint32_t(Resume.SavedStackOffset));
  auto *Slot = B.CreateInBoundsGEP(B.getInt8Ty(), &Frame,
                                   B.getInt32(Establisher + SavedStackSlot));
  auto *Saved = B.CreateInBoundsGEP(
      B.getInt8Ty(), &Frame, B.getInt32(Establisher + Resume.SavedStackOffset));
  auto *Restore = B.CreateStore(B.CreatePtrToInt(Saved, B.getInt32Ty()), Slot);
  Restore->setAlignment(llvm::Align(1));
  Restore->setVolatile(true);
  B.CreateCatchRet(&Pad, &Target);
  Return.eraseFromParent();
}

} // namespace neverd
