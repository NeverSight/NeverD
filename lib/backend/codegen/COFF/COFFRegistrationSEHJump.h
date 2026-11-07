//===- COFFRegistrationSEHJump.h - Native jump buffer replay ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONSEHJUMP_H
#define NEVERD_COFFREGISTRATIONSEHJUMP_H

#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationFrameRuntime.h"

namespace neverd {
struct X86RegistrationSEHJumpCall;
}

namespace neverd::coff_registration {

/// Match the exact state projection and return every required restore
/// instruction to the caller's contiguous post-capture ordering proof.
llvm::Error validateSEHJumpState(const X86RegistrationSEHJumpCall &Expected,
                                 const llvm::CallBase &Call,
                                 const llvm::StoreInst &State,
                                 const RegistrationFrame &Frame,
                                 const llvm::AllocaInst *CapturedState,
                                 std::set<const llvm::Instruction *> &Saves,
                                 std::set<const llvm::Instruction *> &Restores);

/// Rebind source calls, private buffers, register snapshots and state restores
/// from actual post-edit IR. The returned calls alone may borrow these buffers.
/// IncomingSetup must come from successful validateIncomingCallerFrame replay;
/// its immutable runtime frame binding is not part of the logical registers.
llvm::Expected<RegistrationSEHJumpContract>
bindSEHJumps(const MedFunc &Source, const llvm::Function &Parent,
             const RegistrationFrame &Frame,
             const std::set<const llvm::Instruction *> &IncomingSetup,
             const std::map<CallbackKey, const llvm::Function *> &Callbacks);

llvm::Error validateSEHJumpArguments(
    const llvm::CallBase &Call, const llvm::LoadInst *Value,
    const RegistrationSEHJumpResume *Resume, const llvm::Function &Parent,
    const RegistrationFrame &Frame, const llvm::AllocaInst &Buffer,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks);

llvm::Error validateSEHJumpBufferUses(
    const llvm::Function &Parent, const llvm::AllocaInst &Buffer,
    const std::set<const llvm::CallBase *> &Calls,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks);

llvm::Error
validateSEHJumpUnwindBridge(const llvm::Function &Parent,
                            const RegistrationSEHJumpCapture &Source);

llvm::Expected<RegistrationSEHJumpContract> bindSEHBoundCalls(
    const MedFunc &Source, const llvm::Function &Parent,
    const RegistrationFrame &Frame,
    const std::map<va_t, const llvm::AllocaInst *> &Buffers,
    const std::map<CallbackKey, const llvm::Function *> &Callbacks);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONSEHJUMP_H
