//===- WindowsRegistrationFrame.h - x86 runtime callback frames -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_WINDOWSREGISTRATIONFRAME_H
#define NEVERD_BACKEND_LLVM_WINDOWSREGISTRATIONFRAME_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class AllocaInst;
class BasicBlock;
class CallInst;
class Function;
class Instruction;
class Module;
class StoreInst;
} // namespace llvm

namespace neverd {

/// The compiler's x86 MSVC stack protector passes the decoded cookie in ECX.
/// A canonical declaration is separate from authentication of its image VA.
bool hasX86RegistrationSecurityCheckABI(const llvm::Function &Function);
llvm::Function *createX86RegistrationSecurityCheck(llvm::Module &Module);
llvm::Function *createX86RegistrationSecurityCheck(llvm::Module &Module,
                                                   llvm::StringRef Name);

enum class X86RegistrationCallbackKind { Filter, Finally };

enum class X86RegistrationRootKind { FramePointer, StackPointer };

struct X86RegistrationRootSeed {
  /// Exact emitted SSA seed definition, not a register name or numeric VA.
  /// Its scalar destination must be private to this callback, and the seed
  /// must dominate every read. The clone uses private storage and replaces
  /// only this definition's stored value.
  llvm::StoreInst *Definition = nullptr;
  X86RegistrationRootKind Kind = X86RegistrationRootKind::FramePointer;
};

struct X86RegistrationCallbackFrame {
  /// When a lifted filter reads the original registration's exception-pointer
  /// cell, copy the runtime-provided pointer into this exact synthetic-frame
  /// cell. The producer must prove the source-frame offset; this helper only
  /// checks the alloca bounds and implements the runtime ABI.
  llvm::AllocaInst *SyntheticFrame = nullptr;
  std::optional<uint64_t> ExceptionPointersOffset;

  /// Original established EBP relative to SyntheticFrame's allocation start.
  /// LLVM's physical parent FP is used only by localrecover, never as this
  /// source register value.
  std::optional<uint64_t> EstablishedFramePointerOffset;
  llvm::ArrayRef<X86RegistrationRootSeed> RootSeeds;

  /// Undefined ESP live-ins of the outlined callback use a private stack,
  /// rather than the parent's entry ESP or the runtime dispatcher's stack.
  /// The producer supplies a proven bound on callback stack use. The helper
  /// additionally checks bounded scratch accesses below entry ESP and rejects
  /// observable addresses or reads of the original entry/return-address cell.
  uint32_t StackBytes = 0;
  std::optional<uint32_t> StackPointerOffset;
  /// Derive the allocation from the complete checked LLVM use closure of the
  /// ESP seed. No source frame-size estimate is used. Explicit sizes/anchors
  /// must be absent; positive or observable ESP uses still fail.
  bool InferStackBounds = false;
};

struct X86RegistrationCallbackRequest {
  llvm::BasicBlock *Entry = nullptr;
  std::vector<llvm::BasicBlock *> Blocks;
  X86RegistrationCallbackKind Kind = X86RegistrationCallbackKind::Filter;
  std::string Name;
  X86RegistrationCallbackFrame Frame;
  /// Exact source-bound nonlocal exits. Each call must be noreturn and end its
  /// block with unreachable; only source boundary anchors may follow it.
  std::vector<llvm::CallInst *> NonReturningCalls;
};

using X86RegistrationCallbackInstructionMap =
    std::map<const llvm::Instruction *, llvm::Instruction *>;

/// Prepare every callback, allocate one stable escape-index map, then commit
/// the whole batch. Any rejection leaves the complete module unchanged.
/// Preparation and commit are internal to this call; callers cannot mutate a
/// prepared plan. Parent must already have the EH3/EH4 personality.
/// Optional InstructionMaps records each original body instruction's final
/// clone, in request order. Rejection leaves this output unchanged too.
llvm::Expected<std::vector<llvm::Function *>> outlineX86RegistrationCallbacks(
    llvm::Function &Parent,
    llvm::ArrayRef<X86RegistrationCallbackRequest> Requests,
    std::vector<X86RegistrationCallbackInstructionMap> *InstructionMaps =
        nullptr);

/// Outline a closed ordinary-flow callback from an emitted PE32 function.
/// Filters use i32(), frameaddress(1), eh.recoverfp and localrecover. Finally
/// callbacks use void(i8, ptr), recovering locals from their parent-FP
/// argument. Every referenced parent alloca/argument is escaped by identity,
/// with any existing localescape indices preserved. Only checked, entry-local
/// address computations can cross the function boundary; other values fail
/// explicitly.
///
/// All rejection checks precede mutation. The original blocks remain in the
/// parent until its native EH dispatcher has been committed. This operation
/// alone is not a native exception reconstruction receipt.
llvm::Expected<llvm::Function *> outlineX86RegistrationCallback(
    llvm::Function &Parent, llvm::BasicBlock &Entry,
    llvm::ArrayRef<llvm::BasicBlock *> Blocks, X86RegistrationCallbackKind Kind,
    llvm::StringRef Name, const X86RegistrationCallbackFrame &Frame = {});

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSREGISTRATIONFRAME_H
