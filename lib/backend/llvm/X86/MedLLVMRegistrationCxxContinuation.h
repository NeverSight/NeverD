//===- MedLLVMRegistrationCxxContinuation.h - Catch resumes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_MEDLLVMREGISTRATIONCXXCONTINUATION_H
#define NEVERD_MEDLLVMREGISTRATIONCXXCONTINUATION_H

#include "neverd/Common.h"

namespace llvm {
class AllocaInst;
class BasicBlock;
class CatchPadInst;
class ReturnInst;
} // namespace llvm

namespace neverd {
struct RegistrationCxxContinuation;

/// Materialize the runtime's SavedESP writeback in the recovered source
/// frame. LLVM separately owns the generated physical registration frame.
void emitRegistrationCxxContinuation(
    llvm::ReturnInst &Return, llvm::AllocaInst &Frame, uint64_t Establisher,
    int32_t SavedStackSlot, llvm::CatchPadInst &Pad, llvm::BasicBlock &Target,
    va_t FunctionVA, const RegistrationCxxContinuation &Resume,
    llvm::AllocaInst &SavedFrame, uint64_t SavedBase);
} // namespace neverd

#endif // NEVERD_MEDLLVMREGISTRATIONCXXCONTINUATION_H
