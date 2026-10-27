//===- RegistrationFrameProof.h - Shared PE32 frame privacy ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Validate the current LLVM frame use closure independently of its producer.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOF_H
#define NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOF_H

#include "neverd/Common.h"
#include "neverd/backend/llvm/RegistrationCookieProof.h"
#include "neverd/ir/RegistrationCall.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <map>
#include <set>
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
class Value;
} // namespace llvm

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::coff_registration {

struct RegistrationSEHExceptionContract;
struct RegistrationSEHJumpContract;
struct RegistrationSEHLocalUnwindContract;
struct RegistrationSEHParentExitContract;
struct RegistrationSEHScopeState;

struct RegistrationFrameBorrow {
  int64_t Offset = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  /// Null selects the parent's logical frame. An alloca selects a checked
  /// callback stack; a catchpad selects its live reference exception object.
  const llvm::Value *Root = nullptr;
  /// Nonzero selects the two-pointer stdcall CRT ABI and exact table identity.
  va_t ThrowInfoVA = 0;
  /// Ordinary construction reads only values it initialized during the call.
  /// These must-writes seed the normal edge, never an unwind edge.
  bool Initializer = false;
  uint32_t ScalarStackWords = 0;
  std::vector<RegistrationObjectExtent> Initialized;
};
struct RegistrationRuntimeAccess {
  int32_t Offset = 0;
  uint16_t Width = 0;
  bool Write = false;
  const llvm::CatchPadInst *Catch = nullptr;
};
/// Source facts supplied by the independent C++ control replay. These define
/// required effects; the actual LLVM addresses and initialization are checked
/// below through the same address owner used for SEH frame privacy.
struct RegistrationCxxCatchFrameContract {
  const llvm::CatchPadInst *Catch = nullptr;
  int64_t HomeOffset = 0;
  /// Zero means no bound object, so dispatch initializes no frame bytes.
  /// HomeOffset, Reference and RuntimeAccesses must then also be empty.
  uint32_t ObjectSize = 0;
  bool Reference = false;
  const llvm::AllocaInst *CallbackStack = nullptr;
  std::set<const llvm::BasicBlock *> CallbackBlocks;
};
struct RegistrationCxxFrameContract {
  const BinaryImage *Image = nullptr;
  std::vector<RegistrationCxxCatchFrameContract> Catches;
  std::set<const llvm::StoreInst *> SavedStackRestores;
  int64_t SavedStackOffset = 0;
  std::map<const llvm::CallBase *, RegistrationFrameBorrow> Borrows;
  /// Complete source image effects, including the empty set, for every
  /// preserved current call. Private-frame preservation is not this proof.
  std::map<const llvm::CallBase *, std::vector<ExceptionAddressRange>>
      ImageWrites;
  /// Exact returns authenticated by the independent control replay. Exception
  /// destruction/copy image effects remain part of the complete call summaries
  /// above; a bound continuation alone does not prove image preservation.
  std::set<const llvm::CatchReturnInst *> CatchReturns;
  std::map<const llvm::Instruction *, RegistrationRuntimeAccess>
      RuntimeAccesses;
  /// Exact source-bound checker occurrences remain conditional until the
  /// complete current cookie protocol succeeds. They must not also have an
  /// ordinary borrow or image-write summary. No other use of their arguments
  /// loses frame taint.
  std::vector<registration_frame::CookieCall> CookieCalls;
  /// Own the source-bound guard set at every synchronous dispatch event.
  /// Consumers must independently establish source/dispatch completeness.
  std::map<const llvm::Instruction *,
           std::vector<registration_frame::CookieFrameGuard>>
      CookieDispatches;
};

/// Membership follows the already checked funclet parent chain.
bool isActiveCxxCatch(const llvm::CatchPadInst *Owner,
                      const llvm::CatchPadInst *Current);
const RegistrationCxxCatchFrameContract *
activeCxxReferenceHome(const RegistrationCxxFrameContract *Contract,
                       const RegistrationCxxCatchFrameContract *Current,
                       int64_t Offset, uint64_t Size = 0);

/// Recheck the actual post-edit LLVM use closure. Source LowIR privacy alone
/// cannot authenticate a later STORE, call argument or callback-frame edit.
llvm::Error validateFramePrivacy(
    const llvm::Function &Parent,
    llvm::ArrayRef<const llvm::Function *> Callbacks,
    const llvm::AllocaInst *LogicalFrame,
    const std::map<const llvm::Function *, const llvm::StoreInst *>
        &ExceptionBridges,
    const std::set<const llvm::Instruction *> &IncomingAccesses,
    llvm::ArrayRef<ExceptionAddressRange> CallerPCWrites = {},
    const std::set<const llvm::Instruction *> &ChainProtocolReads = {},
    va_t SecurityCookieVA = 0,
    llvm::ArrayRef<ExceptionAddressRange> ImmutableImageRanges = {},
    const RegistrationCxxFrameContract *Cxx = nullptr,
    const RegistrationSEHExceptionContract *SEH = nullptr,
    const RegistrationSEHJumpContract *Jumps = nullptr,
    const RegistrationSEHScopeState *Scopes = nullptr,
    const RegistrationSEHParentExitContract *ParentExits = nullptr,
    const RegistrationSEHLocalUnwindContract *LocalUnwinds = nullptr,
    const std::set<const llvm::LoadInst *> &ThreadReads = {});

} // namespace neverd::coff_registration

#endif // NEVERD_BACKEND_LLVM_REGISTRATIONFRAMEPROOF_H
