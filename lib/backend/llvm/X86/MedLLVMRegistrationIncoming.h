//===- MedLLVMRegistrationIncoming.h - PE32 caller frame projection ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_MEDLLVMREGISTRATIONINCOMING_H
#define NEVERD_MEDLLVMREGISTRATIONINCOMING_H

#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>
#include <optional>
#include <vector>

namespace llvm {
class AllocaInst;
class Function;
class Instruction;
class MDNode;
class Value;
} // namespace llvm

namespace neverd::x86_registration {
struct IncomingFrameAccess {
  llvm::Instruction *IR;
  const MedOp *Source;
  const RegistrationIncomingFrameAccess *Access;
};

/// Bind each physical caller access to its exact emitted memory occurrence.
std::optional<std::vector<IncomingFrameAccess>> prepareIncomingFrame(
    const MedFunc &Source, llvm::Function &Parent,
    const std::map<std::pair<va_t, int>, llvm::Instruction *> &Instructions);

/// Transactional projection shared by SEH callbacks and C++ funclets. The
/// caller must escape slot() with the parent frame before committing.
class IncomingFrameProjection {
  struct Original {
    llvm::Instruction *IR;
    llvm::Value *Pointer;
    llvm::MDNode *Marker;
    bool Volatile;
  };
  llvm::Function &Parent;
  llvm::AllocaInst *Slot = nullptr;
  llvm::Function *PreviousFrameAddress;
  std::vector<llvm::Instruction *> Setup;
  std::vector<Original> Originals;
  bool Committed = false;

public:
  IncomingFrameProjection(llvm::Function &Parent, va_t Owner,
                          llvm::ArrayRef<IncomingFrameAccess> Accesses);
  IncomingFrameProjection(const IncomingFrameProjection &) = delete;
  IncomingFrameProjection &operator=(const IncomingFrameProjection &) = delete;
  ~IncomingFrameProjection();
  llvm::AllocaInst *slot() const { return Slot; }
  void rollback();
  void commit() { Committed = true; }
};
} // namespace neverd::x86_registration
#endif
