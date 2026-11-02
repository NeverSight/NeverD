//===- RegistrationFrameRuntime.h - Native PE32 runtime domains ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Carry independently bound runtime domains into the shared frame-use proof.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMERUNTIME_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMERUNTIME_H

#include "neverd/Common.h"
#include "neverd/ir/RegistrationState.h"

#include <cstdint>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace llvm {
class AllocaInst;
class BasicBlock;
class CallBase;
class CatchPadInst;
class CatchReturnInst;
class Function;
class Instruction;
class LoadInst;
class StoreInst;
class SwitchInst;
class Value;
} // namespace llvm

namespace neverd::coff_registration {

/// Independently replayed source reads and the already authenticated runtime
/// bridge for each filter. Pointer roots are distinct for EXCEPTION_POINTERS
/// and EXCEPTION_RECORD and cannot be shared by different filter invocations.
struct RegistrationSEHExceptionContract {
  struct Access {
    RegistrationSEHExceptionAccess::Kind Field;
    const llvm::StoreInst *Bridge;
  };
  const llvm::AllocaInst *Frame = nullptr;
  int64_t PointersOffset = 0;
  std::map<const llvm::LoadInst *, Access> Reads;
  std::map<const llvm::Function *, const llvm::StoreInst *> Bridges;
  std::set<const llvm::Value *> Roots;
  /// Literal null reads retain their source memory effects, including faults.
  /// Their exact occurrence, pointer, width and ordering are checked at bind.
  std::set<const llvm::LoadInst *> NullReads;

  const llvm::Value *pointerResult(const llvm::LoadInst &Load) const;
  bool validRead(const llvm::LoadInst &Load, const llvm::Value *Root,
                 int64_t Offset) const;
  bool clobbersPointers(const llvm::StoreInst &Store, const llvm::Value *Root,
                        int64_t Offset, uint64_t Size) const;
};

struct RegistrationSEHJumpContract {
  struct Output {
    const llvm::AllocaInst *Frame = nullptr;
    int64_t Offset = 0;
    uint64_t Bytes = 0;
  };
  std::set<const llvm::CallBase *> Calls;
  /// Current source-bound runtime output borrows, not definite initialization.
  std::map<const llvm::CallBase *, Output> RuntimeOutputs;
  /// Current source-bound runtimes that borrow no caller storage. Image,
  /// exception and callback effects still need their independent proofs.
  std::set<const llvm::CallBase *> NoFrameBorrowCalls;
  std::set<const llvm::StoreInst *> StateRestores;
  std::vector<std::pair<int64_t, int64_t>> SourceBuffers;
  bool readsSourceBuffer(int64_t Offset, uint64_t Bytes) const;
};

struct RegistrationSEHScopeState {
  bool Enabled = false;
  int64_t Offset = 0;
  std::map<const llvm::StoreInst *, uint16_t> Writes;
  std::set<const llvm::StoreInst *> CallbackWrites;
  std::set<const llvm::CallBase *> PhysicalWrites;
  /// Exact SEH dispatch returns bound by the independent control replay.
  /// Their target transfer has no implicit exception-object destruction.
  std::map<const llvm::CatchPadInst *, const llvm::CatchReturnInst *>
      CatchReturns;
};

struct RegistrationSEHParentExitContract {
  struct Transfer {
    const llvm::BasicBlock *Block = nullptr;
    va_t Callback = 0;
    va_t Address = 0;
  };
  std::map<int, Transfer> Transfers;
  std::map<int, int> Targets;
  std::map<const llvm::BasicBlock *, std::vector<const llvm::BasicBlock *>>
      ResumptionPredecessors;
  const llvm::BasicBlock *NormalEntry = nullptr;
  const llvm::SwitchInst *Dispatch = nullptr;
  std::set<const llvm::Instruction *> Setup;
  std::set<const llvm::Instruction *> ChainReads;
  std::set<const llvm::Instruction *> ChainWrites;
  std::set<std::pair<va_t, int>> SourceChainRemovals;
  std::set<const llvm::CallBase *> Calls;
};

struct RegistrationSEHLocalUnwindContract {
  std::set<const llvm::CallBase *> Calls;
  std::set<const llvm::LoadInst *> ChainLoads;
  std::set<const llvm::StoreInst *> StateRestores;
};

} // namespace neverd::coff_registration

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMERUNTIME_H
