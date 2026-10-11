//===- WindowsRegistrationFramePrivate.h - PE32 callback planning ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_WINDOWSREGISTRATIONFRAMEPRIVATE_H
#define NEVERD_BACKEND_LLVM_X86_WINDOWSREGISTRATIONFRAMEPRIVATE_H

#include "neverd/backend/llvm/WindowsRegistrationFrame.h"
#include "neverd/backend/llvm/X86RegistrationCatchStack.h"

#include <cstddef>
#include <map>
#include <set>

namespace llvm {
class Argument;
class DataLayout;
class Instruction;
class ReturnInst;
class Value;
class User;
} // namespace llvm

namespace neverd::x86_registration {

/// Internal preflight state. Only the batch outliner commits checked plans,
/// after every request has passed closure, frame and stack validation.
struct CallbackPlan {
  const X86RegistrationCallbackRequest *Request = nullptr;
  X86RegistrationCallbackFrame Frame;
  std::set<llvm::AllocaInst *> Needed;
  std::set<llvm::Argument *> Arguments;
  std::vector<llvm::Instruction *> Recipes;
  std::map<llvm::StoreInst *, X86RegistrationRootKind> Seeds;
  std::set<llvm::AllocaInst *> PrivateSlots;
};

llvm::Error reject(llvm::StringRef Detail);
llvm::Value *filterResult(const llvm::ReturnInst &Return);
bool isStaticEntryAlloca(const llvm::AllocaInst &Slot,
                         const llvm::Function &Parent);
bool isPrivateSlotInitializer(const llvm::User *User,
                              const llvm::AllocaInst &Slot);
llvm::Error checkPrivateStack(
    const llvm::DataLayout &Layout, llvm::BasicBlock &Entry,
    const std::set<llvm::BasicBlock *> &Body,
    const std::map<llvm::StoreInst *, X86RegistrationRootKind> &Seeds,
    X86RegistrationCallbackFrame &Frame,
    std::set<llvm::AllocaInst *> &PrivateSlots, size_t &WorkUsed,
    const std::set<llvm::StoreInst *> *SourceStores = nullptr,
    llvm::ArrayRef<X86RegistrationCatchStackResume> Resumes = {});
llvm::Expected<CallbackPlan>
prepareCallback(llvm::Function &Parent,
                const X86RegistrationCallbackRequest &Request,
                size_t &WorkUsed);

} // namespace neverd::x86_registration

#endif // NEVERD_BACKEND_LLVM_X86_WINDOWSREGISTRATIONFRAMEPRIVATE_H
