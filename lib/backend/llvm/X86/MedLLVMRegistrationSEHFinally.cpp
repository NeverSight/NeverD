//===- MedLLVMRegistrationSEHFinally.cpp - Nested finally invocation -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps a nested ordinary finally call on the same recovered parent frame.

#include "MedLLVMRegistrationSEHFinally.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/X86/X86RegistrationSEHJump.h"

#include "llvm/IR/IRBuilder.h"

#include <set>

namespace neverd::x86_registration {

void emitSEHNestedFinallyCalls(
    llvm::ArrayRef<SEHNestedFinallyCall> Calls,
    llvm::ArrayRef<llvm::Function *> Callbacks,
    llvm::ArrayRef<X86RegistrationCallbackInstructionMap> InstructionMaps,
    llvm::Function &Parent, va_t ParentVA) {
  assert(InstructionMaps.size() == Callbacks.size() &&
         "each outlined callback must have an exact instruction map");
  // Ordinary SSA result slots become localrecover pointers during outlining.
  // Delete only clones of the original, preflighted discard closure; repeating
  // the alloca-use proof after outlining would reject this recovered storage.
  // A result chain may be shared between calls or span callback bodies.
  std::set<const llvm::Instruction *> Discarded;
  for (const auto &Plan : Calls)
    for (const auto *Original : Plan.DiscardedResults)
      Discarded.insert(Original);
  std::set<llvm::Instruction *> Dead;
  for (const auto &Clones : InstructionMaps)
    for (const auto &[Original, Clone] : Clones)
      if (Discarded.count(Original))
        Dead.insert(Clone);
  for (auto *Instruction : Dead)
    Instruction->dropAllReferences();
  for (auto *Instruction : Dead)
    Instruction->eraseFromParent();

  for (const auto &Plan : Calls) {
    auto *Caller = Callbacks[Plan.CallerIndex];
    auto *Callee = Callbacks[Plan.CalleeIndex];
    const auto *Frame = getX86RegistrationSEHJumpParentFrame(*Caller, Parent);
    assert(Frame && "the outlined finally must retain its parent frame");
    auto *Call = llvm::cast<llvm::CallInst>(
        InstructionMaps[Plan.CallerIndex].at(Plan.Original));
    assert(Call->getFunction() == Caller &&
           (Plan.NoReturn || Call->use_empty()) &&
           "outlining must preserve the preflighted invocation and result");
    llvm::IRBuilder<> Builder(Call);
    auto *Replacement = Builder.CreateCall(
        Callee, {Builder.getInt8(0), const_cast<llvm::Value *>(Frame)});
    Replacement->copyMetadata(*Call);
    if (Plan.NoReturn) {
      // The checked invocation never reaches its source result copies. Keep
      // their block boundaries for independent replay, as for parent calls.
      Replacement->setDoesNotReturn();
      if (!Call->getType()->isVoidTy())
        Call->replaceAllUsesWith(llvm::PoisonValue::get(Call->getType()));
    }
    const auto Word = [&](uint64_t Value) {
      return llvm::ConstantAsMetadata::get(Builder.getInt64(Value));
    };
    Replacement->setMetadata(
        windows_eh_md::RegistrationFinallyCallAttachment,
        llvm::MDNode::get(
            Parent.getContext(),
            {Word(ParentVA), Word(Plan.Address), Word(Plan.Target)}));
    Call->eraseFromParent();
  }
}

} // namespace neverd::x86_registration
