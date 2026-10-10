//===- RegistrationABI.h - Checked PE32 registration call ABI -----*- C++
//-*-===//
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
/// all return-pop immediates agree. Nested calls invalidate ESP; an explicit
/// restoration from an ABI-preserved register can recover it. No memory
/// value survives this proof, and no memory-effect authority is granted.
std::optional<uint32_t>
getCheckedX86CalleeStackPop(const BinaryImage &Image, va_t Target,
                            size_t *CumulativeWork = nullptr);

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

/// A checked MSVC cleanup relay borrows its object from the establisher EBP
/// and tail-jumps to a separately checked leaf. The parent still proves this
/// object's bounds, initialization and separation at every unwind dispatch.
struct RegistrationCleanupRelayABI {
  va_t Target = InvalidVA;
  va_t EndAddress = InvalidVA;
  int32_t ObjectFrameOffset = 0;
  RegistrationLeafCalleeABI Leaf;
};

std::optional<RegistrationCleanupRelayABI>
getCheckedX86RegistrationCleanupRelayABI(const BinaryImage &Image, va_t Target,
                                         size_t *CumulativeWork = nullptr);

struct RegistrationThrowCalleeABI {
  va_t Target = InvalidVA;
  va_t ImportVA = InvalidVA;
  va_t ImportIATVA = InvalidVA;
  va_t ThrowCallVA = InvalidVA;
  va_t ThrowCallEndVA = InvalidVA;
  int ThrowOpSeq = -1;
  int32_t ObjectOffset = 0;
  coff_loader::X86SimpleCxxThrowInfo ThrowInfo;
  std::vector<ExceptionAddressRange> ImageReads;
  std::vector<ExceptionAddressRange> ImageWrites;
  std::vector<ExceptionAddressRange> CallerPCWrites;
  std::vector<ExceptionAddressRange> CodeRanges;
};

/// Prove a closed PE32 helper that initializes a private scalar object and
/// terminates through the exact VCRUNTIME140 _CxxThrowException import. The
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
  explicit RegistrationCallCalleeIndex(const BinaryImage &Image)
      : Image(Image) {}
  std::optional<std::vector<RegistrationCalleeFrameContract>>
  contracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationCleanupFrameContract>>
  cleanupContracts(const LowFunc &Function);
  std::optional<std::vector<RegistrationCalleeStackContract>>
  stackContracts(const LowFunc &Function);

private:
  const BinaryImage &Image;
  size_t Work = 0;
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
} // namespace neverd

#endif
