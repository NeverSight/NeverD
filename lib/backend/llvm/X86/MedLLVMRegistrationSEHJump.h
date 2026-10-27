//===- MedLLVMRegistrationSEHJump.h - Native jump buffers -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHJUMP_H
#define NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHJUMP_H

#include "neverd/backend/llvm/X86RegistrationSEHBoundCall.h"
#include "neverd/backend/llvm/X86RegistrationSEHJump.h"

#include "llvm/IR/IRBuilder.h"

#include <vector>

namespace llvm {
class AllocaInst;
class CallInst;
class Function;
class Instruction;
class Value;
} // namespace llvm

namespace neverd::x86_registration {
struct SEHJumpPlan {
  std::map<llvm::CallInst *, X86RegistrationSEHJumpCall> Calls;
  std::map<llvm::CallInst *, X86RegistrationSEHBoundCall> BoundCalls;
  std::vector<llvm::AllocaInst *> Registers;
  std::vector<llvm::Instruction *> DiscardedResults;
};

bool prepareSEHBoundCall(const llvm::CallInst &Call,
                         const RegistrationSEHBoundCall &Source,
                         const llvm::AllocaInst &Frame, uint64_t EntrySP);
bool hasSEHBoundFrameArguments(const RegistrationSEHBoundCall &Source,
                               const llvm::AllocaInst &Frame, uint64_t EntrySP);
llvm::SmallVector<llvm::Value *, 8> projectSEHBoundArguments(
    const RegistrationSEHBoundCall &Source, llvm::IRBuilder<> &Builder,
    llvm::AllocaInst &Frame, uint64_t EntrySP,
    const std::map<va_t, llvm::AllocaInst *> &Buffers,
    const std::map<va_t, llvm::Value *> &ImagePointers = {});

std::map<llvm::CallInst *, llvm::CallInst *>
emitSEHBoundCalls(const SEHJumpPlan &Plan, llvm::AllocaInst &Frame,
                  uint64_t EntrySP,
                  const std::map<va_t, llvm::AllocaInst *> &Buffers);

struct SEHJumpCallbackCall {
  llvm::CallInst *Call = nullptr;
  llvm::Value *Buffer = nullptr;
  X86RegistrationSEHJumpStack Stack;
};

/// Retain each invocation-local load while rebinding its outlined stack slot.
void bindSEHRuntimeCallbackArguments(
    const SEHJumpPlan &Plan, const std::map<va_t, llvm::Function *> &Callbacks);

/// Bind cloned calls and recover their capture buffers after callback commit.
std::map<llvm::CallInst *, SEHJumpCallbackCall>
emitSEHJumpCallbackBindings(const SEHJumpPlan &Plan, llvm::Function &Parent,
                            const std::map<va_t, llvm::AllocaInst *> &Buffers,
                            const std::map<va_t, llvm::Function *> &Callbacks);

std::optional<SEHJumpPlan>
prepareSEHJumps(const MedFunc &Source, llvm::Function &Parent,
                llvm::AllocaInst &Frame, uint64_t EntrySP,
                const std::map<const llvm::CallInst *, va_t> &Calls);

/// Restore the capture state on initial return, or project the last logical
/// callback state through the source's proved remaining unwind steps.
llvm::Value *emitSEHJumpState(const X86RegistrationSEHJumpCall &Source,
                              llvm::CallInst &Call, llvm::Value &State,
                              llvm::Value *Captured,
                              llvm::IRBuilder<> &Builder);

/// Commit a preflighted buffer projection. The returned map replaces the old
/// source calls before protected invokes are constructed.
std::map<llvm::CallInst *, llvm::CallInst *>
emitSEHJumps(const SEHJumpPlan &Plan, llvm::Function &Parent,
             llvm::AllocaInst &Frame, uint64_t EntrySP, va_t Owner,
             const std::map<va_t, llvm::Function *> &Callbacks);

} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_MEDLLVMREGISTRATIONSEHJUMP_H
