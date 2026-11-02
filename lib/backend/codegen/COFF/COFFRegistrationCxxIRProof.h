//===- COFFRegistrationCxxIRProof.h - C++ control proof --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_COFFREGISTRATIONCXXIRPROOF_H
#define NEVERD_COFFREGISTRATIONCXXIRPROOF_H
#include "COFFRegistrationIRProof.h"

#include "neverd/backend/llvm/RegistrationCxxCookieBindings.h"
#include "neverd/backend/llvm/X86/X86RegistrationCatch.h"
#include "neverd/backend/llvm/X86/X86RegistrationLayout.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <map>
#include <optional>
#include <set>
#include <vector>

namespace llvm {
class CatchReturnInst;
} // namespace llvm

namespace neverd {
struct BinaryImage;
} // namespace neverd
namespace neverd::coff_registration {
struct CxxIRCall {
  RegistrationCalleeFrameContract Contract;
  std::optional<int32_t> ObjectFrameOffset;
  bool Cleanup = false;
  std::optional<RegistrationRuntimeThrow> RuntimeThrow;
  std::optional<RegistrationRuntimeObjectBorrow> RuntimeObject;
};
bool hasExactCxxCallAttributes(const llvm::CallBase &Call,
                               const CxxIRCall &Receipt);
struct CxxIRCatch {
  const llvm::CatchPadInst *Pad = nullptr;
  std::optional<X86RegistrationCatchHome> Home;
  const llvm::AllocaInst *Stack = nullptr;
  std::set<const llvm::BasicBlock *> Blocks;
};
/// Ephemeral bindings from independent source/control replay. The complete
/// current guarded frame proof must still succeed before dispatch is allowed.
struct CxxIRGuardedFrame {
  MedFunc Source;
  X86RegistrationFrameLayout Layout;
  std::vector<registration_frame::CxxSourceCallBinding> Calls;
  std::vector<registration_frame::CxxCleanupCallBinding> Cleanups;
};
/// This is a control/source occurrence receipt. Its calls still need the
/// post-edit frame, initialization and runtime-object proof before
/// installation.
struct CxxIRControlProof {
  LowFunc Source;
  RegistrationFrame Frame;
  std::map<X86RegistrationCatchIdentity, CxxIRCatch> Catches;
  std::map<int, X86RegistrationCatchIdentity> SourceCatchOwners;
  std::map<int, SourceSegment> Segments;
  std::map<const llvm::BasicBlock *, const llvm::BasicBlock *> CopyEdges;
  std::map<uint32_t, const llvm::CleanupPadInst *> Cleanups;
  std::map<const llvm::CallBase *, CxxIRCall> Calls;
  std::set<const llvm::CatchReturnInst *> CatchReturns;
  std::set<const llvm::Instruction *> ChainReads;
  std::set<const llvm::StoreInst *> SavedStackRestores;
  std::set<const llvm::Instruction *> IncomingAccesses;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  std::optional<CxxIRGuardedFrame> Guarded;
};
struct RegistrationCxxFrameContract;
llvm::Error bindCxxRuntimeThrow(const CxxIRControlProof &Proof,
                                const llvm::CallBase &Call,
                                const CxxIRCall &Checked,
                                const BinaryImage &Image,
                                RegistrationCxxFrameContract &Contract,
                                std::vector<ExceptionAddressRange> &Immutable,
                                size_t &Work);
llvm::Error bindCxxRuntimeObjectBorrow(const CxxIRControlProof &Proof,
                                       const llvm::CallBase &Call,
                                       const CxxIRCall &Checked,
                                       RegistrationCxxFrameContract &Contract);

llvm::Expected<const llvm::CatchReturnInst *> validateCxxContinuationRestore(
    const llvm::Instruction &Anchor, const RegistrationFrame &Frame,
    int32_t SavedStackSlot, const RegistrationCxxContinuation &Resume,
    const llvm::AllocaInst *SavedCallbackStack = nullptr);

llvm::Error bindCxxCatches(CxxIRControlProof &Proof, const MedFunc &Source,
                           const llvm::Function &Function,
                           const X86RegistrationFrameLayout &Layout);

/// Recognize only private scalar spill copies on one exact source CFG edge.
/// No emitter metadata or block name grants ownership of a generated block.
llvm::Error bindCxxCopyEdges(CxxIRControlProof &Proof);

llvm::Error bindCxxCatchResumes(CxxIRControlProof &Proof, const MedFunc &Source,
                                const llvm::Function &Function);

llvm::Error bindCxxCatchStack(CxxIRControlProof &Proof, const MedFunc &Source,
                              const llvm::Function &Function);

llvm::Expected<CxxIRControlProof>
getCheckedCxxControlIRProof(const llvm::Function &Function,
                            const ExceptionFunction &Source,
                            const BinaryImage &Image);

/// The image-only query remains unguarded. A guarded dispatcher additionally
/// requires independent source/control replay and the complete current frame
/// proof; neither producer metadata nor a previous receipt is sufficient.
llvm::Expected<coff_loader::X86CxxPersonalityABI>
getCheckedCxxRuntimeIR(const llvm::Function &Function,
                       const ExceptionFunction &Source,
                       const BinaryImage &Image);
} // namespace neverd::coff_registration
#endif
