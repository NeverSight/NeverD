//===- COFFRegistrationFrameProof.h - PE32 private frame proof ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_COFFREGISTRATIONFRAMEPROOF_H
#define NEVERD_COFFREGISTRATIONFRAMEPROOF_H

#include "neverd/Common.h"
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
class Function;
class Instruction;
class StoreInst;
} // namespace llvm

namespace neverd {
struct BinaryImage;
} // namespace neverd

namespace neverd::coff_registration {

struct RegistrationFrameBorrow {
  int64_t Offset = 0;
  std::vector<RegistrationObjectExtent> Reads;
  std::vector<RegistrationObjectExtent> Writes;
  /// Null selects the parent's logical frame. A callback borrow names its
  /// independently authenticated private stack allocation.
  const llvm::AllocaInst *Root = nullptr;
  /// Nonzero selects the two-pointer stdcall CRT ABI and exact table identity.
  va_t ThrowInfoVA = 0;
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
  int64_t SavedStackOffset = 0;
  std::map<const llvm::CallBase *, RegistrationFrameBorrow> Borrows;
  std::map<const llvm::Instruction *, RegistrationRuntimeAccess>
      RuntimeAccesses;
};

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
    const RegistrationCxxFrameContract *Cxx = nullptr);

} // namespace neverd::coff_registration

#endif // NEVERD_COFFREGISTRATIONFRAMEPROOF_H
