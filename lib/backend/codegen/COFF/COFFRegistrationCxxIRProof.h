//===- COFFRegistrationCxxIRProof.h - C++ control proof --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_COFFREGISTRATIONCXXIRPROOF_H
#define NEVERD_COFFREGISTRATIONCXXIRPROOF_H
#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/X86RegistrationCatch.h"
#include "neverd/ir/low/LowIR.h"

namespace neverd {
struct BinaryImage;
struct X86RegistrationFrameLayout;
} // namespace neverd
namespace neverd::coff_registration {
struct CxxIRCall {
  RegistrationCalleeFrameContract Contract;
  std::optional<int32_t> ObjectFrameOffset;
  bool Cleanup = false;
};
struct CxxIRCatch {
  const llvm::CatchPadInst *Pad = nullptr;
  std::optional<X86RegistrationCatchHome> Home;
  const llvm::AllocaInst *Stack = nullptr;
  std::set<const llvm::BasicBlock *> Blocks;
};
/// This is a control/source occurrence receipt. Its calls still need the
/// post-edit frame, initialization and runtime-object proof before
/// installation.
struct CxxIRControlProof {
  LowFunc Source;
  RegistrationFrame Frame;
  std::vector<CxxIRCatch> Catches;
  std::map<int, uint32_t> SourceCatchOwners;
  std::map<int, SourceSegment> Segments;
  std::map<uint32_t, const llvm::CleanupPadInst *> Cleanups;
  std::map<const llvm::CallBase *, CxxIRCall> Calls;
  std::set<const llvm::Instruction *> ChainReads;
  std::set<const llvm::Instruction *> IncomingAccesses;
  std::vector<ExceptionAddressRange> CallerPCWrites;
};
llvm::Expected<const llvm::CatchReturnInst *> validateCxxContinuationRestore(
    const llvm::Instruction &Anchor, const RegistrationFrame &Frame,
    int32_t SavedStackSlot, const RegistrationCxxContinuation &Resume);

llvm::Error bindCxxCatches(CxxIRControlProof &Proof, const MedFunc &Source,
                           const llvm::Function &Function,
                           const X86RegistrationFrameLayout &Layout);

llvm::Error bindCxxCatchStack(CxxIRControlProof &Proof, const MedFunc &Source,
                              const llvm::Function &Function);

llvm::Expected<CxxIRControlProof>
getCheckedCxxControlIRProof(const llvm::Function &Function,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image);
} // namespace neverd::coff_registration
#endif
