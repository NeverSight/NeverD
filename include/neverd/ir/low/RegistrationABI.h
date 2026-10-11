//===- RegistrationABI.h - PE32 registration call ABI ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_REGISTRATIONABI_H
#define NEVERD_IR_LOW_REGISTRATIONABI_H

#include "neverd/ir/RegistrationCall.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionCommon.h"

#include <map>
#include <optional>
#include <vector>

namespace neverd {
struct BinaryImage;
struct LowFunc;

/// Prove the entry ESP is restored at every ordinary near return and that
/// all return-pop immediates agree. Unchecked calls invalidate ESP; an explicit
/// restoration from an ABI-preserved register can recover it. Ordinary bodies
/// retain no memory values. SEH bodies also require complete current
/// registration state/lifetime evidence to separate dispatcher returns and
/// seed established EBP at except entries. Neither route infers saved-stack
/// memory values or grants memory-effect authority.
std::optional<uint32_t>
getCheckedX86CalleeStackPop(const BinaryImage &Image, va_t Target,
                            size_t *CumulativeWork = nullptr);

/// Authenticate the exact PE32 _local_unwind2 import or immutable IAT veneer.
/// This identifies its runtime protocol, not its caller's frame or callbacks.
std::optional<RegistrationLocalUnwindContract>
getCheckedX86LocalUnwindContract(const BinaryImage &Image, va_t Target,
                                 bool Indirect);

/// A leaf's two separate address domains: its private invocation frame and
/// the bounded object borrowed through entry ECX. Object offsets are relative
/// to that object, never invented image addresses or private stack offsets.
struct RegistrationLeafCalleeABI {
  va_t Target = InvalidVA;
  uint32_t StackPopBytes = 0;
  /// Every returning path computes its 32-bit scalar result independently of
  /// incoming registers, borrowed pointers and the physical caller PC. A frame
  /// privacy proof alone may still permit an unobserved entry-EAX return.
  bool HasIndependentScalarReturn = false;
  std::vector<RegistrationObjectExtent> ECXReads;
  std::vector<RegistrationObjectExtent> ECXWrites;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  std::vector<ExceptionAddressRange> CodeRanges;
};

/// Prove a complete returning PE32 leaf with a private stack, preserved
/// nonvolatile registers and nonescaping ECX object access. This describes
/// the callee only: each caller must still prove object bounds, initialization
/// and separation from its registration fields before using the projection.
std::optional<RegistrationLeafCalleeABI>
getCheckedX86RegistrationLeafCalleeABI(const BinaryImage &Image, va_t Target,
                                       size_t *CumulativeWork = nullptr);

struct RegistrationCleanupCallABI {
  int32_t ObjectFrameOffset = 0;
  RegistrationLeafCalleeABI Leaf;
};

/// Coordinates read by a realigned cleanup before calling its leaves. The
/// parent state proof must establish the saved entry EBP and keep it intact.
struct RegistrationCleanupParentFrame {
  int32_t BaseOffset = 0;
  int32_t SavedParentFrameOffset = 0;
};

/// A checked cleanup relay derives each ECX object from the establisher EBP.
/// It tail-jumps to one leaf or saves/restores runtime EBP around an ordered
/// sequence of leaf calls. The parent must prove every borrow at dispatch.
struct RegistrationCleanupRelayABI {
  va_t Target = InvalidVA;
  va_t EndAddress = InvalidVA;
  std::vector<RegistrationCleanupCallABI> Calls;
  std::optional<RegistrationCleanupParentFrame> RealignedParent;

  /// Bind a cached byte proof to this invocation's frame. This checks only
  /// coordinates; the parent's LowIR proof owns contents and lifetime.
  bool matchesParentFrame(const RegistrationChainInfo &Chain) const;
};

std::optional<RegistrationCleanupRelayABI>
getCheckedX86RegistrationCleanupRelayABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork = nullptr);

/// The exact two-argument stdcall runtime import and its transparent jump stub.
/// This proves an entry ABI only; a rethrow occurrence must also prove two null
/// arguments and an active catch in the source state solver.
struct RegistrationThrowImportABI {
  va_t Target = InvalidVA;
  va_t IATVA = InvalidVA;
};

std::optional<RegistrationThrowImportABI>
getCheckedX86RegistrationThrowImportABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork = nullptr);

struct RegistrationThrowCalleeABI {
  va_t Target = InvalidVA;
  va_t ImportVA = InvalidVA;
  va_t ImportIATVA = InvalidVA;
  va_t ThrowCallVA = InvalidVA;
  va_t ThrowCallEndVA = InvalidVA;
  int ThrowOpSeq = -1;
  int32_t ObjectOffset = 0;
  /// Exactly two null arguments reuse the active CRT exception. No new object
  /// or ThrowInfo is supplied, and the caller must prove an active catch.
  bool IsRethrow = false;
  coff_loader::X86SimpleCxxThrowInfo ThrowInfo;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  std::vector<ExceptionAddressRange> CodeRanges;
};

/// Prove a closed PE32 helper that initializes a private scalar object or
/// rethrows, then terminates through the exact VCRUNTIME140 import. The
/// original helper is preserved; no source frame pointer may escape except
/// its checked object argument and an observable real caller PC.
std::optional<RegistrationThrowCalleeABI>
getCheckedX86RegistrationThrowCalleeABI(const BinaryImage &Image, va_t Target,
                                        size_t *CumulativeWork = nullptr);

/// Memoized checked callees for one CFG construction and its continuation
/// closure. A shared work budget includes failed proofs; a new CFG build gets
/// a fresh index so edits to an image cannot reuse stale callee evidence.
class RegistrationCallCalleeIndex {
public:
  explicit RegistrationCallCalleeIndex(const BinaryImage &Image,
                                       size_t *CumulativeWork = nullptr)
      : Image(Image), Work(CumulativeWork ? *CumulativeWork : LocalWork) {}
  RegistrationCallCalleeIndex(const RegistrationCallCalleeIndex &) = delete;
  RegistrationCallCalleeIndex &
  operator=(const RegistrationCallCalleeIndex &) = delete;
  bool ownsImage(const BinaryImage &Other) const { return &Image == &Other; }
  std::optional<uint32_t> stackPop(va_t Target);
  std::optional<std::vector<RegistrationCalleeFrameContract>>
  contracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationCleanupFrameContract>>
  cleanupContracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationCalleeStackContract>>
  stackContracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationLocalUnwindContract>>
  localUnwindContracts(const LowFunc &Function);

private:
  const BinaryImage &Image;
  size_t LocalWork = 0;
  size_t &Work;
  unsigned StackDepth = 0;
  std::map<va_t, std::optional<RegistrationCalleeFrameContract>> Cache;
  std::map<va_t, std::optional<RegistrationCleanupRelayABI>> CleanupCache;
  std::map<va_t, std::optional<uint32_t>> StackCache;
};

/// Native registration lowering currently emits caller-cleanup calls. Require
/// the source and every preserved direct callee to have that same stack
/// contract. Indirect targets, incomplete bodies and tail-only bodies provide
/// no such proof. The final writer replays this check from immutable input.
bool hasCallerCleanupRegistrationABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);

/// Prove the C++ parent's exact RET cleanup separately from its runtime
/// callbacks, and replay the preserved call/frame closure. The returned byte
/// count is not the decoder's maximum across unrelated return instructions.
std::optional<uint16_t> getCheckedX86RegistrationCxxParentABI(
    const LowFunc &Function, const BinaryImage &Image,
    std::vector<ExceptionAddressRange> *CallerPCWrites = nullptr);
} // namespace neverd

#endif
